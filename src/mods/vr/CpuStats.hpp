// CpuStats.hpp -- [CPU 03.11.2026] counters for the CPU measurements of the periodic status line that several places add
// to: the render thread's present path, the mod's waits for its own earlier GPU work and the busiest engine hooks.
// Portable (no engine / D3D12 types); read and reset by FoveatedRendering once per status line.
#pragma once

#include "QuadView.hpp" // quadview::ShardedCounter

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace cpustats {
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A hook that runs thousands of times per frame from many threads: every call is counted; one call in EVERY, chosen at
// random per thread, is timed and counts EVERY times (the sum estimates the total without two clock reads per call).
struct HookMeter {
    static constexpr uint32_t EVERY = 64;
    quadview::ShardedCounter calls{};
    quadview::ShardedCounter ns{};
};

#if REVR_RELEASE
// [RELEASE SWITCH fix141 23.11.2026] the release measures nothing: the timers are empty and compile to nothing
class HookTimer {
public:
    explicit HookTimer(HookMeter&, bool = true) {}
    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;
};
#else
class HookTimer {
public:
    // count_call false: a second part of the same call (the callbacks after the game's function)
    explicit HookTimer(HookMeter& m, bool count_call = true) : m_meter{m} {
        thread_local uint32_t t_rng = 0x9E3779B9u;
        t_rng ^= t_rng << 13;
        t_rng ^= t_rng >> 17;
        t_rng ^= t_rng << 5;

        if (count_call) {
            m_meter.calls.fetch_add(1);
        }

        if ((t_rng & (HookMeter::EVERY - 1)) == 0) {
            m_t0 = now_ns();
        }
    }

    // a timed call that took over 50 us was taken off the processor in between (the callbacks take well under 1 us): left
    // out, it would count 64 times
    ~HookTimer() {
        if (m_t0 != 0) {
            const int64_t dt = now_ns() - m_t0;

            if (dt > 0 && dt < 50'000) {
                m_meter.ns.fetch_add((uint64_t)dt * HookMeter::EVERY);
            }
        }
    }

    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;

private:
    HookMeter& m_meter;
    int64_t m_t0{0};
};
#endif

// [GLUE 04.11.2026] Like HookTimer, for calls that happen thousands of times per frame where even counting each one costs
// more than it should: only the timed call (one in EVERY, per thread) adds anything, EVERY to the calls and EVERY times
// its time. Untimed calls cost one step of a per-thread random number.
#if REVR_RELEASE
class SampledTimer {
public:
    explicit SampledTimer(HookMeter&) {}
    SampledTimer(const SampledTimer&) = delete;
    SampledTimer& operator=(const SampledTimer&) = delete;
};
#else
class SampledTimer {
public:
    explicit SampledTimer(HookMeter& m) : m_meter{m} {
        thread_local uint32_t t_rng = 0x2545F491u;
        t_rng ^= t_rng << 13;
        t_rng ^= t_rng >> 17;
        t_rng ^= t_rng << 5;

        if ((t_rng & (HookMeter::EVERY - 1)) == 0) {
            m_t0 = now_ns();
        }
    }

    ~SampledTimer() {
        if (m_t0 != 0) {
            const int64_t dt = now_ns() - m_t0;
            m_meter.calls.fetch_add(HookMeter::EVERY);

            if (dt > 0 && dt < 50'000) {
                m_meter.ns.fetch_add((uint64_t)dt * HookMeter::EVERY);
            }
        }
    }

    SampledTimer(const SampledTimer&) = delete;
    SampledTimer& operator=(const SampledTimer&) = delete;

private:
    HookMeter& m_meter;
    int64_t m_t0{0};
};
#endif

// [GLUE 04.11.2026] The RE4VR mods' managed calls by method name (re4vr::call_safe / try_call) and their accesses to the
// Lua state (re4vr::LuaRef: the script runner's lock around one read, write or call), all threads, estimated (SampledTimer)
inline HookMeter& managed_calls() {
    static HookMeter m{};
    return m;
}

inline HookMeter& lua_accesses() {
    static HookMeter m{};
    return m;
}

// The engine's transform update hook (REFramework dispatches it to the mods that take part, Mod::wants_transform_hooks):
// how often it runs and the time of that dispatch (before and after the game's function, without the function itself)
inline HookMeter& transform_hook() {
    static HookMeter m{};
    return m;
}

// The thread that presents: what happens there per frame. The game's Present ends the render thread's frame; the time in
// it is the time the driver / the swap chain holds the thread (a sign of waiting for the GPU or the display).
struct Present {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> hook_ns{0};      // REFramework's present hook before the game's Present (includes the next two)
    std::atomic<uint64_t> lock_wait_ns{0}; // ... waiting for the ImGui lock (held by the main thread's ImGui frame)
    std::atomic<uint64_t> mods_ns{0};      // ... the mods' on_early_present and on_present
    std::atomic<uint64_t> present_ns{0};   // the game's own Present call
    std::atomic<uint64_t> post_ns{0};      // the mods' on_post_present after it
    std::atomic<uint32_t> tid{0};          // the thread that presents
};

inline Present& present() {
    static Present p{};
    return p;
}

// The mod's waits for its own earlier GPU work before reusing a command list (d3d12::CommandContext::wait), all threads
struct FenceWaits {
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> calls{0}; // waits that were needed (the GPU had not finished)
    std::atomic<uint64_t> long_waits{0}; // ... of them over 0.05 ms
};

inline FenceWaits& fence_waits() {
    static FenceWaits f{};
    return f;
}

// [SUBMIT 05.11.2026] The VR mod's hand-over of the eye images to SteamVR in the present hook (render thread): the D3D12
// component's frame (copies, conversion, the compositor's Submit calls), the Submit calls alone and PostPresentHandoff; and
// SteamVR's compositor events counted when the VR mod polls them. Log of 05.11.2026 (a fight, FSR 4 in the fovea): for the
// last half minute the VR mod's present callback took 98-100.5 ms per frame, the render and main threads were idle in that
// time, the mod's GPU work was normal and SteamVR reported the application as not responding; where in the callback the time
// went was not measured.
struct VrSubmit {
    std::atomic<uint64_t> component_frames{0};
    std::atomic<uint64_t> component_ns{0};
    std::atomic<uint64_t> submits{0};
    std::atomic<uint64_t> submit_ns{0};
    std::atomic<uint64_t> submit_max_ns{0}; // the longest single call since the periodic line took it
    std::atomic<uint64_t> submit_slow{0};   // calls over 20 ms
    std::atomic<uint64_t> handoffs{0};
    std::atomic<uint64_t> handoff_ns{0};
    std::atomic<uint64_t> not_responding{0};      // VREvent_Compositor_ApplicationNotResponding
    std::atomic<uint64_t> resumed{0};             // VREvent_Compositor_ApplicationResumed
    std::atomic<uint64_t> out_of_video_memory{0}; // VREvent_Compositor_OutOfVideoMemory
    // [COMPOSITOR LOCK 05.11.2026] runtimes::lock_compositor: waits over 0.05 ms for the other thread's compositor calls
    std::atomic<uint64_t> lock_waits_present{0};
    std::atomic<uint64_t> lock_wait_ns_present{0};
    std::atomic<uint64_t> lock_waits_poses{0};
    std::atomic<uint64_t> lock_wait_ns_poses{0};
    std::atomic<uint64_t> lock_timeouts{0}; // the time limit ran out: the call went ahead without the lock
};

inline VrSubmit& vr_submit() {
    static VrSubmit s{};
    return s;
}

// [FRAME TIMING 05.11.2026] SteamVR's timing of each finished frame (IVRCompositor::GetFrameTiming, taken in the VR mod's
// present callback): the GPU time of the frame (game + mod + compositor), the frames shown again and why, the compositor's
// throttling. For the periodic log and the GPU guard (quadview::GpuGuard).
struct VrTiming {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> gpu_total_us{0};      // m_flTotalRenderGpuMs
    std::atomic<uint64_t> gpu_total_max_us{0};  // the longest since the periodic line took it
    std::atomic<uint64_t> gpu_compositor_us{0}; // m_flCompositorRenderGpuMs
    std::atomic<uint64_t> gpu_over{0};          // frames whose GPU time was over the headset's frame time
    std::atomic<uint64_t> late_gpu{0};          // VRCompositor_ReprojectionReason_Gpu
    std::atomic<uint64_t> late_cpu{0};          // VRCompositor_ReprojectionReason_Cpu
    std::atomic<uint64_t> throttled{0};         // frames with the compositor throttling the application
    std::atomic<uint64_t> mispresented{0};      // m_nNumMisPresented
    std::atomic<uint64_t> dropped{0};           // m_nNumDroppedFrames
    std::atomic<uint32_t> last_index{0};
    std::atomic<uint32_t> frame_us{11111};      // the headset's frame time (FoveatedRendering sets it)
};

inline VrTiming& vr_timing() {
    static VrTiming t{};
    return t;
}

inline void store_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);

    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// [GUI 04.11.2026] The GUI draw hook (one call per GUI element and view, inside the overlay layers' update, which BeginRendering
// waits for): calls, the mods' callbacks around the element and the game's own draw of it, all threads
struct GuiHook {
    quadview::ShardedCounter calls{};
    quadview::ShardedCounter mods_ns{};
    quadview::ShardedCounter game_ns{};
};

inline GuiHook& gui_hook() {
    static GuiHook g{};
    return g;
}

// [EARLY IMGUI 04.11.2026] REFramework's ImGui frame with every mod's on_frame callback ran at the start of BeginRendering.
// The frame timeline of 04.11.2026 shows what lies on the path from one frame to the next: the main thread from the end of
// WaitRendering to EndRendering (where the render thread is started), then the render thread up to its Present (where
// WaitRendering returns). The ImGui frame was on that path twice: ~0.5 ms waiting for the ImGui lock, which the present hook
// holds while the mods' present callbacks run, and ~0.55 ms of its own work. With this on it runs at the start of
// WaitRendering instead (after LockScene, as before), while the render thread still works on the frame before; BeginRendering
// runs it only if WaitRendering did not come (Hooks.cpp).
inline std::atomic<bool>& early_imgui_frame() {
    static std::atomic<bool> on{true};
    return on;
}

// [HEAVY SLOT 04.11.2026] rounds of the game's main thread, advanced at the start of every BeginRendering (Hooks.cpp):
// one round = BeginRendering .. EndRendering, the update, WaitRendering (with the ImGui frame)
inline std::atomic<uint32_t>& main_round() {
    static std::atomic<uint32_t> r{0};
    return r;
}

// the main thread's ImGui frame (REFramework::run_imgui_frame): how often, its time and the wait for its lock in it
struct ImguiFrame {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> lock_wait_ns{0};
};

inline ImguiFrame& imgui_frame() {
    static ImguiFrame f{};
    return f;
}

// [TIMELINE 03.11.2026] A short recording of what happens when, on which thread: the game's application entries (with
// REFramework's dispatch around them), the render layers' update and draw, the present path and the wait for the headset.
// Written out as a table (FoveatedRendering) to work out which work lies on the path from one frame to the next. While
// it is off a mark costs one relaxed load.
enum class Ev : uint16_t {
    EntryPre = 1,     // REFramework's dispatch of an application entry starts (arg = the entry's name, const char*)
    EntryCall,        // the game's entry function starts (the mods' pre callbacks are done)
    EntryRet,         // it returned
    EntryPost,        // the mods' post callbacks are done
    ImguiBegin,       // REFramework's ImGui frame with the mods' on_frame callbacks (before BeginRendering's pre callbacks)
    ImguiEnd,
    LayerPre,         // a render layer hook (arg = the layer, sub = which: see Hooks.cpp LAYER_HOOK_BODY)
    LayerCall,        // the game's layer function starts
    LayerRet,
    LayerPost,
    PresentHookBegin, // REFramework's present hook (the thread that presents)
    PresentHookEnd,   // ... leaves for the game's Present
    PresentCallEnd,   // the game's Present returned
    ModsPresentBegin, // the mods' on_early_present / on_present
    ModsPresentEnd,
    WaitPosesBegin,   // OpenVR WaitGetPoses
    WaitPosesEnd,
    GuiPre,           // [GUI 04.11.2026] the GUI draw hook of one element (arg = the element): the mods' pre callbacks begin
    GuiCall,          // the game's draw of the element begins
    GuiRet,           // it returned
    GuiPost,          // the mods' post callbacks are done
    GuiMod,           // one mod's callback for an element that took over 1 us (arg = mod index << 32 | ns, sub = 0 pre / 1 post)
};

struct Event {
    int64_t t;
    uint64_t arg;
    uint32_t tid;
    uint16_t ev;
    uint16_t sub;
};

#if REVR_RELEASE
// [RELEASE SWITCH fix141] no frame timeline in the release: marks compile to nothing
class Timeline {
public:
    static constexpr uint32_t CAP = 1u << 17;
    static inline uint32_t (*s_thread_id)() = nullptr;
    bool recording() const { return false; }
    void start() {}
    std::vector<Event> stop() { return {}; }
    void mark(Ev, uint64_t = 0, uint16_t = 0) {}
};
#else
class Timeline {
public:
    static constexpr uint32_t CAP = 1u << 17;

    // the OS thread id (set once by the mod; without it a small number per thread)
    static inline uint32_t (*s_thread_id)() = nullptr;

    bool recording() const { return m_on.load(std::memory_order_relaxed); }

    void start() {
        if (m_events == nullptr) {
            m_events = std::make_unique<Event[]>(CAP);
        }

        m_n.store(0, std::memory_order_relaxed);
        m_on.store(true, std::memory_order_release);
    }

    // stops and hands out what was recorded (marks still running on other threads land after the copied range)
    std::vector<Event> stop() {
        m_on.store(false, std::memory_order_release);
        const uint32_t n = (std::min)(m_n.load(std::memory_order_acquire), CAP);
        std::vector<Event> out{};

        if (m_events != nullptr) {
            out.assign(m_events.get(), m_events.get() + n);
        }

        return out;
    }

    void mark(Ev e, uint64_t arg = 0, uint16_t sub = 0) {
        if (!m_on.load(std::memory_order_acquire)) {
            return;
        }

        const uint32_t i = m_n.fetch_add(1, std::memory_order_relaxed);

        if (i < CAP && m_events != nullptr) {
            m_events[i] = Event{now_ns(), arg, thread_id(), (uint16_t)e, sub};
        }
    }

private:
    static uint32_t thread_id() {
        thread_local uint32_t t_id = 0;

        if (t_id == 0) {
            static std::atomic<uint32_t> next{1};
            t_id = s_thread_id != nullptr ? s_thread_id() : next.fetch_add(1);
        }

        return t_id;
    }

    std::atomic<bool> m_on{false};
    std::atomic<uint32_t> m_n{0};
    std::unique_ptr<Event[]> m_events{};
};
#endif

inline Timeline& timeline() {
    static Timeline t{};
    return t;
}
} // namespace cpustats
