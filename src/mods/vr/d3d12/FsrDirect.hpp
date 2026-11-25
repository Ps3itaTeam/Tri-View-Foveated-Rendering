#pragma once

// [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod itself through AMD's API (headers in ffx/, functions taken
// at run time from the FSR DLLs in the game folder: amd_fidelityfx_loader_dx12.dll, else amd_fidelityfx_upscaler_dx12.dll).
// Its passes are recorded into a command list the mod gives (the pre-fovea list, right after the mod's own work), so they
// need no list and no submit of their own, and the CPU never waits for the GPU here. One context per upscaled image: both
// foveas side by side in one (quadview::FsrAtlas), or one per eye.

#include <cstdint>
#include <string>
#include <utility>

#include <d3d12.h>

namespace d3d12 {

class FsrDirect {
public:
    struct Create {
        uint32_t render_w{0}; // the input's size (largest render size)
        uint32_t render_h{0};
        uint32_t out_w{0};    // the output's size
        uint32_t out_h{0};
        bool auto_exposure{true};
    };

    struct Input {
        // resting in a state that includes NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE (the caller's barriers)
        ID3D12Resource* color{nullptr};  // linear light
        ID3D12Resource* depth{nullptr};  // reversed, infinite far plane
        ID3D12Resource* motion{nullptr};
        ID3D12Resource* output{nullptr}; // UAV-able, resting in UNORDERED_ACCESS
        uint32_t render_w{0};
        uint32_t render_h{0};
        uint32_t out_w{0};
        uint32_t out_h{0};
        float jitter_x{0.0f};            // pixels, as given to the upscaler plugin
        float jitter_y{0.0f};
        float motion_scale_x{1.0f};      // motion vector -> pixels
        float motion_scale_y{1.0f};
        float frame_ms{11.1f};
        float near_plane{0.1f};
        float fov_y{1.0f};               // radians
        bool reset{false};
    };

    // false: last_error() says why (the context is gone)
    bool create(ID3D12Device* device, const Create& c);
    // records FSR 4 into `cmd`; false: last_error()
    bool dispatch(ID3D12GraphicsCommandList* cmd, const Input& in);
    // the GPU must be done with the context's work (the caller waits for its lists first)
    void destroy();

    bool ready() const { return m_context != nullptr; }
    const Create& created() const { return m_created; }
    // the FSR version the API chose ("FSR 4.1.2" ...) and which DLL the functions came from
    const std::string& provider() const { return m_provider; }
    static std::string api_source();
    const std::string& last_error() const { return m_last_error; }
    void set_error(std::string e) { m_last_error = std::move(e); }
    // messages FSR sent (errors and warnings, also written to the framework log)
    static uint64_t messages();

private:
    void* m_context{nullptr};
    Create m_created{};
    std::string m_provider{};
    std::string m_last_error{};
};

} // namespace d3d12
