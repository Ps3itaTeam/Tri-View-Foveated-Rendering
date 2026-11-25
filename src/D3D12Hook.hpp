#pragma once

#include <iostream>
#include <functional>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi")

#include <d3d12.h>
#include <filesystem>
#include <dxgi1_4.h>

#include "utility/PointerHook.hpp"
#include "utility/FunctionHook.hpp"
#include "utility/VtableHook.hpp"
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE

// [DRED 14.11.2026] Device Removed Extended Data (debug build): enable() before the game makes its device (breadcrumbs after each
// GPU operation and the allocations around a page fault, a little GPU time), log() after a device removed: the command lists the
// GPU had not finished, the operation each one stopped at, and the faulting address with the allocations at it
namespace d3d12_dred {
// [DRED fix103 20.11.2026] Off unless the configuration asks for it. It was switched on at every start since 14.11.2026 (for the
// GPU hang in RE7's main menu) and stayed on in every game: the D3D12 runtime then writes a counter into the GPU's command
// stream after every operation of the game (draw, dispatch, copy, barrier) and keeps a record of every allocation, which
// Microsoft measures at 2-5 % of a game's frame; the release build before the RE2 port did not have it. wanted(): the key
// FoveatedRendering_DebugDeviceRemovedData=true in REFramework's configuration file (read as text: the game makes its device
// before the mods load their options). active(): enable() set DRED's settings in this session.
#if REVR_RELEASE // [RELEASE SWITCH fix141] the crash report is a debug tool: nothing of it in the release
inline bool wanted(const std::filesystem::path&) { return false; }
inline bool active() { return false; }
inline void enable() {}
inline void watch_queue(ID3D12CommandQueue*) {}
inline void log(ID3D12Device*) {}
#else
bool wanted(const std::filesystem::path& config_file);
bool active();
void enable();
void watch_queue(ID3D12CommandQueue* queue); // the game's queue known: its class's ExecuteCommandLists records who submits
void log(ID3D12Device* device);
#endif
}

class D3D12Hook
{
public:
	typedef std::function<void(D3D12Hook&)> OnPresentFn;
	typedef std::function<void(D3D12Hook&)> OnResizeBuffersFn;
    typedef std::function<void(D3D12Hook&)> OnResizeTargetFn;
    typedef std::function<void(D3D12Hook&)> OnCreateSwapChainFn;

	D3D12Hook() = default;
	virtual ~D3D12Hook();

	bool hook();
	bool unhook();

    bool is_hooked() {
        return m_hooked;
    }

    void on_present(OnPresentFn fn) {
        m_on_present = fn;
    }

    void on_post_present(OnPresentFn fn) {
        m_on_post_present = fn;
    }

    void on_resize_buffers(OnResizeBuffersFn fn) {
        m_on_resize_buffers = fn;
    }

    void on_resize_target(OnResizeTargetFn fn) {
        m_on_resize_target = fn;
    }

    /*void on_create_swap_chain(OnCreateSwapChainFn fn) {
        m_on_create_swap_chain = fn;
    }*/

    ID3D12Device4* get_device() const {
        return m_device;
    }

    IDXGISwapChain3* get_swap_chain() const {
        return m_swap_chain;
    }

    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }

    ID3D12CommandQueue* get_command_queue() const {
        return m_command_queue;
    }

    UINT get_display_width() const {
        return m_display_width;
    }

    UINT get_display_height() const {
        return m_display_height;
    }

    // [WINDOW PASS 06.11.2026 evening] the game's last Present to its window (sync interval and flags), for the log
    uint32_t get_last_sync_interval() const { return m_last_sync_interval; }
    uint32_t get_last_present_flags() const { return m_last_present_flags; }

    UINT get_render_width() const {
        return m_render_width;
    }

    UINT get_render_height() const {
        return m_render_height;
    }

    bool is_inside_present() const {
        return m_inside_present;
    }

    bool is_proton_swapchain() const {
        return m_using_proton_swapchain;
    }
    
    bool is_framegen_swapchain() const {
        return m_using_frame_generation_swapchain;
    }

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    static void hook_streamline(HMODULE dlssg_module = nullptr);

protected:
    void hook_impl();
    
    ID3D12Device4* m_device{ nullptr };
    IDXGISwapChain3* m_swap_chain{ nullptr };
    IDXGISwapChain3* m_swapchain_0{};
    IDXGISwapChain3* m_swapchain_1{};
    ID3D12CommandQueue* m_command_queue{ nullptr };
    UINT m_display_width{ NULL };
    UINT m_display_height{ NULL };
    UINT m_render_width{ NULL };
    UINT m_render_height{ NULL };

    static inline uint32_t s_command_queue_offset{};
    static inline uint32_t s_proton_swapchain_offset{};

    bool m_using_proton_swapchain{ false };
    bool m_using_frame_generation_swapchain{ false };
    bool m_hooked{ false };
    bool m_is_phase_1{ true };
    bool m_inside_present{false};
    bool m_ignore_next_present{false};
    volatile uint32_t m_last_sync_interval{0}; // [WINDOW PASS 06.11.2026 evening] written by the presenting thread, read for the log
    volatile uint32_t m_last_present_flags{0};

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<VtableHook> m_swapchain_hook{};

    struct Streamline {
        static void* link_swapchain_to_cmd_queue(void* rcx, void* rdx, void* r8, void* r9);

        std::unique_ptr<FunctionHook> link_swapchain_to_cmd_queue_hook{};
        std::mutex hook_mutex{};
        bool setup{ false };
    };

    static inline Streamline s_streamline{};

    // This is static because unhooking it seems to cause a crash sometimes
    static inline std::unique_ptr<PointerHook> s_create_swapchain_hook{};
    static inline void** s_factory_vtable{ nullptr };
    static inline void** s_swapchain_vtable{ nullptr };
    
    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    OnResizeTargetFn m_on_resize_target{ nullptr };
    //OnCreateSwapChainFn m_on_create_swap_chain{ nullptr };
    
    static HRESULT WINAPI present(IDXGISwapChain3* swap_chain, uint64_t sync_interval, uint64_t flags, void* r9);
    static HRESULT WINAPI set_fullscreen_state(IDXGISwapChain3* swap_chain, BOOL Fullscreen, IDXGIOutput* pTarget);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters);
    static HRESULT WINAPI create_swapchain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain);
};

