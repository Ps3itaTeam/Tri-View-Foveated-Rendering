#pragma once
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG

// [SUBMIT TIMELINE 06.11.2026] The GPU's work of a frame as the queues run it: every ExecuteCommandLists of the game's queues
// (the engine's, the mod's, SteamVR's copies) gets a timestamp before and after its lists, in the same call. Per frame
// (from one present to the next) the report gives how long the game's queue worked, how long it stood idle between two
// submits and why (the next work not yet sent by the CPU, or a wait for another queue), which submits take the GPU time,
// and the compute queue's work next to it. A measurement only: the image does not change. The hook (the queue class's
// ExecuteCommandLists entry) is installed on the first start and lets every call through unchanged while not running.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "ComPtr.hpp"
#include "GapLoad.hpp"
#include "../SubmitMath.hpp"

class PointerHook;

namespace d3d12 {
#if REVR_RELEASE
// [RELEASE SWITCH fix141 23.11.2026] the release has no GPU timeline (no queue hooks, no timestamps): an empty stand-in, its
// markers and tags compile to nothing
class SubmitTimeline {
public:
    static SubmitTimeline& get() {
        static SubmitTimeline s{};
        return s;
    }
    bool start(ID3D12Device*, ID3D12CommandQueue*, std::string&) { return false; }
    void stop() {}
    bool running() const { return false; }
    void frame_mark() {}
    std::string take_report(bool) { return {}; }
    void reset() {}
    void marker(const char*) {}
    bool prepare_test_load(ID3D12Device*, uint32_t, uint32_t, std::string&) { return false; }
    void set_test_load(int) {}
    static constexpr uint32_t GAP_AFTER_A = 7;
    static constexpr uint32_t GAP_AFTER_B = 10;
    static constexpr uint32_t REF_AFTER = 1;
    struct Tag {
        explicit Tag(const char*) {}
        Tag(const Tag&) = delete;
        Tag& operator=(const Tag&) = delete;
    };
};
#else
class SubmitTimeline {
public:
    static SubmitTimeline& get();

    // starts recording (creates its command lists, ~0.3 s, and hooks the queue class the first time); false with the reason
    bool start(ID3D12Device* device, ID3D12CommandQueue* game_queue, std::string& why);
    void stop();
    bool running() const { return m_running.load(std::memory_order_acquire); }

    // the end of a presented frame (the thread that presents, after the game's Present): the submits since the last mark
    // are that frame's; finished frames are read back here
    void frame_mark();

    // the frames read back since the last call: one summary, and with detail every submit of the last frame read; cleared
    std::string take_report(bool detail);

    // device reset / removal: the GPU objects are dropped (the hook stays and lets every call through)
    void reset();

    // [SUBMIT TIMELINE 06.11.2026 evening] a point of the frame on the game's queue: two timestamps (no work) submitted there
    // under this name while the timeline runs, so the report shows where the queue stands idle between our calls (after the
    // headset's frame, after PostPresentHandoff, before REFramework's pass, after the game's Present)
    void marker(const char* name);

    // [GAP TEST 06.11.2026 night] the GPU test's load (d3d12::GapLoad) on the game's queue: 0 none; 1 small, 2 large right
    // before the engine's wait that follows its GAP_AFTER_A-th and GAP_AFTER_B-th submits from the thread that presents (in
    // the fovea views, where the queue waits for its compute work; since 07.11 before the wait, after the engine's signal to
    // the compute queue); 3 two large ones right after its REF_AFTER-th (the wide view's first pass, where the queue does not
    // wait: what the load costs when nothing hides it). prepare_test_load makes it (game thread, before the test).
    bool prepare_test_load(ID3D12Device* device, uint32_t w, uint32_t h, std::string& why);
    void set_test_load(int mode) { m_load_mode.store(mode, std::memory_order_release); }
    static constexpr uint32_t GAP_AFTER_A = 7;
    static constexpr uint32_t GAP_AFTER_B = 10;
    static constexpr uint32_t REF_AFTER = 1;

    // the mod's own submits by name: the calls made on this thread while it lives carry it
    struct Tag {
        explicit Tag(const char* name);
        ~Tag();
        Tag(const Tag&) = delete;
        Tag& operator=(const Tag&) = delete;
        const char* prev;
    };

    static constexpr uint32_t FRAMES = 8;
    static constexpr uint32_t STAMPS = 192; // per frame and queue type: 96 submits
    static constexpr uint32_t MAX_LISTS = 62;

private:
    using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using WaitFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

    static void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    static HRESULT STDMETHODCALLTYPE wait_hook(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);

    struct Queue {
        ComPtr<ID3D12CommandQueue> queue{};
        D3D12_COMMAND_LIST_TYPE type{D3D12_COMMAND_LIST_TYPE_DIRECT};
        uint64_t freq{0};
        uint64_t cal_gpu{0};
        uint64_t cal_cpu{0};
        ComPtr<ID3D12Fence> fence{};
        uint64_t fence_value{0};
    };

    struct Call {
        uint8_t kind{0};  // 0 direct, 1 compute, 2 copy or other (no stamps), 3 a queue's Wait
        uint8_t queue{0}; // index into m_queues
        uint16_t lists{0};
        uint32_t thread{0};
        const char* tag{nullptr};
        int64_t cpu{0};      // QueryPerformanceCounter at the call
        int32_t stamp{-1};   // the first of its two stamps in its type's part of the frame's slot, -1 none
        double gpu_start{0}; // ms from the frame's start (read back)
        double gpu_end{0};
        bool timed{false};
    };

    struct Frame {
        uint64_t number{0};
        bool open{false};      // the submits go here
        bool closed{false};    // marked, waiting for the GPU
        bool instrumented{true};
        int64_t start_cpu{0};  // the mark that opened it
        int64_t end_cpu{0};    // the mark that closed it
        uint32_t used[2]{};
        uint32_t overflow{0};
        std::vector<Call> calls{};
        std::vector<std::pair<uint32_t, uint64_t>> fences{}; // queue index, value signalled at the close
    };

    // the results of one frame, accumulated
    struct Totals {
        uint32_t frames{0};
        uint32_t frames_game{0}; // with timed work on the game's queue
        uint32_t skipped{0};
        uint32_t overflow{0};
        double span{0};      // the game's queue: first work to last
        double busy{0};
        double idle_cpu{0};  // idle while the next submit had not been made yet
        double idle_wait{0}; // idle after a Wait on that queue
        double idle_other{0};
        double calls_game{0};
        double lists_game{0};
        double calls_compute{0};
        double busy_compute{0};
        double overlap_compute{0};
        double calls_copy{0};
        double waits{0};
        double end_to_mark{0}; // the game's queue's last work ended this long before the frame's mark (negative: after)
        // [06.11.2026 evening] the engine's first submit from the thread that presents: how long the GPU started it after it was made
        double rt_delay{0};
        double rt_delay_max{0};
        uint32_t rt_frames{0};
        uint32_t rt_over_half{0}; // over 0.5 ms
        uint32_t rt_over_two{0};  // over 2 ms
        // [06.11.2026 evening] the engine's submits from the thread that presents per frame (CPU frames)
        uint64_t rt_engine_calls{0};
        uint32_t cpu_frames{0};
        // [GAP TEST 06.11.2026 night] the test loads made in each place (A / B: before the waits), left out (their slot still busy), frames whose engine
        // submits from the thread that presents were not the usual 15 (the places then fall elsewhere)
        uint32_t loads_a{0};
        uint32_t loads_b{0};
        uint32_t loads_ref{0};
        uint32_t loads_skipped{0};
        uint32_t frames_other_count{0};
        struct Key {
            double busy{0};
            double at{0};
            double lists{0};
            double overlap{0}; // compute keys: the part while the game's queue worked
            uint32_t n{0};
        };
        struct Gap {
            double gap{0};
            double cpu_late{0};
            uint32_t waits{0};
            uint32_t n{0};
        };
        std::unordered_map<std::string, Key> keys{};
        std::unordered_map<std::string, Gap> gaps{};
        std::unordered_map<std::string, Key> compute_keys{}; // [06.11.2026 evening] every compute submit by name
    };

    bool on_call(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* stamps[2]);
    void after_call(ID3D12CommandQueue* queue); // inside the hook: the engine's submits from the thread that presents, counted, the test load
    void queue_test_load(ID3D12CommandQueue* queue, bool large, const char* tag);
    void before_wait(ID3D12CommandQueue* queue); // [GAP TEST 07.11.2026] the present thread, before a wait: the test load
    void on_wait(ID3D12CommandQueue* queue);
    int queue_index(ID3D12CommandQueue* queue); // m_mutex held
    void close_open_frame(int64_t now);         // m_mutex held
    void read_back();                           // m_mutex held
    void process(Frame& f);                     // m_mutex held
    std::vector<submitmath::Call> as_math(const std::vector<Call>& calls, int64_t start_cpu, int game) const;
    double to_ms(const Queue& q, uint64_t gpu, int64_t ref_cpu) const;
    void release_gpu_objects();

    std::mutex m_mutex{};
    std::atomic<bool> m_running{false};
    std::atomic<int> m_in_flight{0};
    std::atomic<ExecuteFn> m_execute_original{nullptr};
    std::atomic<WaitFn> m_wait_original{nullptr};
    std::unique_ptr<PointerHook> m_execute_hook{};
    std::unique_ptr<PointerHook> m_wait_hook{};
    void** m_vtable{nullptr};

    ID3D12CommandQueue* m_game_queue{nullptr}; // not owned
    ID3D12Device* m_device{nullptr};           // not owned
    ComPtr<ID3D12QueryHeap> m_heap{};
    ComPtr<ID3D12Resource> m_readback{};
    const uint64_t* m_mapped{nullptr};
    ComPtr<ID3D12CommandAllocator> m_alloc[2][FRAMES]{};
    std::vector<ComPtr<ID3D12GraphicsCommandList>> m_lists[2]{}; // [kind][slot * STAMPS + i]
    bool m_have_compute{false};

    std::vector<Queue> m_queues{};
    std::unordered_map<uint32_t, uint32_t> m_threads{}; // thread id -> its number in the report
    uint32_t m_present_thread{0};
    Frame m_frames[FRAMES]{};
    uint64_t m_frame_number{0};
    uint32_t m_calibrated{0};
    int64_t m_qpc_freq{1};

    Totals m_totals{};
    uint32_t m_rt_engine_calls{0}; // this frame's (present thread only)
    GapLoad m_load{};             // [GAP TEST 06.11.2026 night]
    std::atomic<int> m_load_mode{0};
    uint64_t m_load_number{0};    // present thread only
    uint32_t m_load_wait_done{0}; // the engine's submit count whose wait got this frame's load (present thread only)
    std::vector<Call> m_last_calls{}; // the last frame read, for the detail
    std::vector<Call> m_prev_calls{}; // the frame before it (when it was the one just before), for the end of it in the detail
    uint64_t m_prev_number{0};
    int64_t m_prev_start_cpu{0};
    int64_t m_prev_end_cpu{0};
    uint64_t m_last_number{0};
    int64_t m_last_start_cpu{0};
    int64_t m_last_end_cpu{0};
    double m_last_span{0};
};
#endif
} // namespace d3d12
