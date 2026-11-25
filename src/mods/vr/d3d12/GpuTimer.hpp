// GpuTimer.hpp -- measures GPU queue time per frame with timestamp queries (debug / benchmark only).
//
// begin() submits a tiny command list with a timestamp at the start of the frame's GPU work (called when the
// engine starts rendering), end() submits the closing timestamp + resolve (called at present). The difference
// is the time the GPU queue needed for the frame -- the number that matters when the game is GPU bound.
#pragma once
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG

#include <array>
#include <cstdint>
#include <mutex>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {

class GpuTimer {
public:
    struct Result {
        double avg_ms{0.0};
        double min_ms{0.0};
        double max_ms{0.0};
        uint32_t samples{0};
    };

    bool enable(ID3D12Device* device, ID3D12CommandQueue* queue);
    void disable();
    bool enabled() const { return m_enabled; }

    void begin();
    void end();
    Result take(); // returns and clears the accumulated numbers

    void reset(); // device reset / shutdown: drops all GPU objects

private:
    static constexpr uint32_t SLOTS = 8;

    bool create(ID3D12Device* device);
    void poll();

    std::mutex m_mutex{};
    bool m_enabled{false};
    bool m_failed{false};

    ID3D12CommandQueue* m_queue{nullptr}; // not owned
    ComPtr<ID3D12QueryHeap> m_heap{};
    ComPtr<ID3D12Resource> m_readback{};
    std::array<ComPtr<ID3D12CommandAllocator>, SLOTS * 2> m_alloc{};
    ComPtr<ID3D12GraphicsCommandList> m_list{};
    ComPtr<ID3D12Fence> m_fence{};
    uint64_t m_fence_value{0};
    std::array<uint64_t, SLOTS> m_slot_fence{};
    std::array<bool, SLOTS> m_pending{};
    UINT64 m_freq{0};
    uint32_t m_next{0};
    uint32_t m_current{0};
    bool m_begun{false};

    double m_sum{0.0};
    double m_min{1e9};
    double m_max{0.0};
    uint32_t m_count{0};
};

// [GPU BREAKDOWN 27.10.2026] Timestamps written into the mod's own command lists, to see where the quad view's GPU time
// goes. Each frame up to STAMPS stamps (see `Stamp`, in the order the GPU runs them); a span is the time from one stamp to
// another (see `spans()`). Between two stamps of the same list it is that list's work; between the end of one list and
// the start of the next it is whatever the queue ran in between (the upscaler plugin's fovea passes, the engine). Read
// back four frames later.
// [GPU BREAKDOWN 29.10.2026] one stamp after each pass of the quad view (the pre-fovea work and every eye separately)
class GpuSpans {
public:
    static constexpr uint32_t STAMPS = 20;
    static constexpr uint32_t FRAMES = 8;

    enum Stamp : uint32_t {
        PRE_BEGIN = 0,     // pre-fovea context: start
        PRE_UI_CURVE,      //   output curve measured
        PRE_UI_CLEAN,      //   interface taken out of the wide view (and where it is)
        PRE_WIDE_TAA,      //   TAA of the wide view
        PRE_BLOCK_MIN,     //   reprojection: nearest distance per row and block
        PRE_REPROJECT_1,   //   reprojection: first pass (both eyes)
        PRE_REPROJECT,     //   reprojection: second pass (both eyes)
        PRE_END,           //   interface taken out of the fovea views
        COPIER_BEGIN,      // copier: start
        COPIER_FOVEA_L,    //   fovea of the left eye
        COPIER_EYE_L,      //   left eye image put together
        COPIER_FOVEA_R,
        COPIER_EYE_R,
        COPIER_END,        //   desktop mirror copied
        POST_BEGIN,        // post pass (D3D12Component; not used with the shared periphery)
        POST_END,
        CONVERT_BEGIN,     // eye images to the 8-bit headset textures
        CONVERT_END,
        // [FSR DIRECT 06.11.2026] FSR 4 called by the mod, on the pre-fovea list after PRE_END: its inputs side by side (depth,
        // motion), then its passes
        PRE_FSR_BEGIN,
        PRE_FSR_END,
    };

    // [ONE LIST 01.11.2026] a frame's layout of the stamps: which lists write which stamps (a span holds only in the layouts
    // it names)
    enum Mode : uint32_t {
        MODE_SYNC = 0,     // the pre list, then the copier (the plugin in the fovea, a dump frame, the GPU test's variant)
        MODE_ONE_LIST = 1, // the pre-fovea work on the copier's list (after COPIER_BEGIN), no list in between
    };
    static constexpr uint8_t SYNC = 1u << MODE_SYNC;
    static constexpr uint8_t ONE = 1u << MODE_ONE_LIST;
    static constexpr uint8_t BOTH = SYNC | ONE;

    struct SpanDef {
        uint32_t from;
        uint32_t to;
        const char* name;
        bool ours; // counts into the mod's total (the other spans are parts of these, or not the mod's work)
        uint8_t modes; // the stamp layouts the span holds in (Mode bits)
    };

    struct Span {
        double avg_ms{0.0};
        double max_ms{0.0};
        uint32_t samples{0};
    };

    // The same name in both layouts = the same work (span_ms and the test's lines take them together by name). "wall" is the
    // direct queue's time from the first pre stamp to the copier's end: with a pre list of its own it also holds the queue's
    // gap before the copier, which the mod's total leaves out; with one list there is no gap (the copier's part after the
    // pre work counts).
    static constexpr SpanDef SPANS[] = {
        {PRE_BEGIN, PRE_END, "pre", true, BOTH},
        {PRE_BEGIN, PRE_UI_CURVE, "ui_curve", false, BOTH},
        {PRE_UI_CURVE, PRE_UI_CLEAN, "ui_clean_wide", false, BOTH},
        {PRE_UI_CLEAN, PRE_WIDE_TAA, "taa_wide", false, BOTH},
        {PRE_WIDE_TAA, PRE_BLOCK_MIN, "block_min", false, BOTH},
        {PRE_BLOCK_MIN, PRE_REPROJECT_1, "reproject_1", false, BOTH},
        {PRE_REPROJECT_1, PRE_REPROJECT, "reproject_2", false, BOTH},
        {PRE_REPROJECT, PRE_END, "ui_clean_fovea", false, BOTH},
        {PRE_END, COPIER_BEGIN, "between(plugin fovea)", false, SYNC},
        {PRE_END, PRE_FSR_BEGIN, "fsr4 inputs side by side", false, SYNC},
        {PRE_FSR_BEGIN, PRE_FSR_END, "fsr4 by the mod", false, SYNC},
        {COPIER_BEGIN, COPIER_END, "copier", true, SYNC},
        {PRE_END, COPIER_END, "copier", true, ONE},
        {COPIER_BEGIN, COPIER_FOVEA_L, "fovea_L", false, SYNC},
        {PRE_END, COPIER_FOVEA_L, "fovea_L", false, ONE},
        {COPIER_FOVEA_L, COPIER_EYE_L, "eye_L", false, BOTH},
        {COPIER_EYE_L, COPIER_FOVEA_R, "fovea_R", false, BOTH},
        {COPIER_FOVEA_R, COPIER_EYE_R, "eye_R", false, BOTH},
        {COPIER_EYE_R, COPIER_END, "mirror", false, BOTH},
        {PRE_BEGIN, COPIER_END, "wall(pre begin..copier end)", false, BOTH},
        {COPIER_END, POST_BEGIN, "between(engine)", false, BOTH},
        {POST_BEGIN, POST_END, "post", true, BOTH},
        {POST_END, CONVERT_BEGIN, "between", false, BOTH},
        {CONVERT_BEGIN, CONVERT_END, "convert(10->8 bit)", true, BOTH},
    };
    static constexpr uint32_t SPAN_COUNT = (uint32_t)(sizeof(SPANS) / sizeof(SPANS[0]));

#if REVR_RELEASE
    // [RELEASE SWITCH fix141 23.11.2026] the release measures no GPU time: every stamp of the render code compiles to nothing
    static GpuSpans& get() {
        static GpuSpans s{};
        return s;
    }
    bool enable(ID3D12Device*, ID3D12CommandQueue*) { return false; }
    void disable() {}
    void reset() {}
    bool enabled() const { return false; }
    void stamp(ID3D12GraphicsCommandList*, uint64_t, uint32_t) {}
    void set_ends_only(bool) {}
    void set_mode(uint64_t, uint32_t) {}
    void resolve(ID3D12GraphicsCommandList*, uint64_t, uint32_t, uint32_t) {}
    void collect(uint64_t) {}
    std::array<Span, SPAN_COUNT> take() { return {}; }
#else
    static GpuSpans& get();

    bool enable(ID3D12Device* device, ID3D12CommandQueue* queue);
    void disable();
    void reset();
    bool enabled() const { return m_enabled; }

    // a timestamp on `cmd` for stamp `index` of `frame` (the lists of one frame must run in stamp order)
    void stamp(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t index);
    // [ENDS ONLY fix119 22.11.2026] the GPU test's variant: of the pre work's and the copier's stamps only the first and the
    // last are written (PRE_BEGIN, COPIER_END), so only "wall" is measured. A timestamp is taken when the work before it is
    // done; whether the ones between the passes keep a pass from starting while the one before still ends (and so cost
    // time themselves, and make a short pass like the eye pass's marked tiles look dearer than it is without them) is
    // what the variant measures.
    void set_ends_only(bool on) {
        std::scoped_lock _{m_mutex};
        m_ends_only = on;
    }
    // [ONE LIST 01.11.2026] the stamp layout of `frame` (MODE_SYNC unless set)
    void set_mode(uint64_t frame, uint32_t mode);
    // copies the stamps [first, first + count) of `frame` into the readback buffer (after their timestamps, same list or later)
    void resolve(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t first, uint32_t count);
    // reads the frames finished a while ago (older than `frame` - 4) into the accumulators; call once per frame
    void collect(uint64_t frame);
    // accumulated time of each span of SPANS, cleared
    std::array<Span, SPAN_COUNT> take();

private:
    bool create(ID3D12Device* device);
    // the slot of `frame`, taken over (cleared) when it still holds an older frame; m_mutex held by the caller
    uint32_t slot_for(uint64_t frame);

    std::mutex m_mutex{};
    bool m_enabled{false};
    bool m_failed{false};
    bool m_ends_only{false}; // [ENDS ONLY fix119]
    ID3D12CommandQueue* m_queue{nullptr}; // not owned
    ComPtr<ID3D12QueryHeap> m_heap{};
    ComPtr<ID3D12Resource> m_readback{};
    const uint64_t* m_mapped{nullptr};
    UINT64 m_freq{0};
    std::array<uint64_t, FRAMES> m_slot_frame{};
    std::array<uint32_t, FRAMES> m_written{};   // bit i = stamp i written this frame
    std::array<uint32_t, FRAMES> m_resolved{};  // bit i = stamp i resolved
    std::array<uint32_t, FRAMES> m_mode{};      // Mode of the frame in the slot
    std::array<bool, FRAMES> m_collected{};
    std::array<double, SPAN_COUNT> m_sum{};
    std::array<double, SPAN_COUNT> m_max{};
    std::array<uint32_t, SPAN_COUNT> m_count{};
#endif
};

} // namespace d3d12
