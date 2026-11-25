#pragma once

// [GPU GUARD 05.11.2026] Log of 05.11.2026 20:18 (two fights, FSR 4 in the fovea): in the heavy parts the GPU time of a
// frame was the game's 8.4-9.3 ms + the mod's 2.2-2.5 ms + the compositor's ~0.35 ms, over the headset's 11.1 ms, and the
// frames came out late (up to 3.7 % shown again, 42 % for a few seconds after a kick with SteamVR halving the frame rate).
// This decides, from half-second windows, when the mod's anti-aliasing gives way to its lighter form and when it goes back
// to the full one. Pure logic (unit test).
// [GPU GUARD fix91 19.11.2026] Then: the upscaler plugin in the fovea gave way to the mod's TAA. Now: the mod's
// supersampling TAA gives way to its lighter choices, in the fovea and in the periphery at once (see
// FoveatedRendering::gpu_guard_watch). The decisions are the same; the names follow what is switched now.

// [GUARD fix101 20.11.2026] Several steps (FoveatedRendering::gpu_guard_watch names them: the periphery's outlines per ray,
// its image of the wide view's size, the calm light TAA, and last the fovea's history): `levels` of them. With save_ms
// (what each level saves against the menu's choices, measured on the user's PC) a step up goes as far as the time over
// the limit asks for, at once, instead of one level per one and a half seconds. The last level can be marked as the
// one for a frame rate that collapses (extreme_level): it is taken only when a quarter of the frames of two windows in a
// row, or two fifths of one, were shown again, and held longer. The steps back are one level at a time, as before.

#include <cstdint>

namespace quadview {

class GpuGuard {
public:
    struct Window {
        uint32_t frames{0};   // frames SteamVR timed in the window
        uint32_t gpu_late{0}; // ... of them shown again because the GPU was late (VRCompositor_ReprojectionReason_Gpu)
        double gpu_ms{0.0};   // the frame's GPU time: the game's + the mod's + the compositor's (average of the window)
        uint32_t throttled{0}; // [fix102] ... of them with the compositor holding the game to half the frame rate
    };

    enum class Change { None, ToLighter, ToFull };

    static constexpr int MAX_LEVELS = 6;

    double frame_ms{11.11};     // the headset's frame time
    double full_extra_ms{0.5};  // what the step back from the level it is on needs more (GPU time per frame; the caller
                                // sets it before update)
    int levels{1};
    int step_wait{2};           // windows on a level before the next step up (the step's effect has to show first)
    // [fix102 20.11.2026] RE4 test of fix101 (20.11.2026, PSVR2 at 90 Hz): frames were shown on another vsync from a frame's
    // GPU time of about 10.1 ms on (6 - 15 % of them at 10.1 - 10.3 ms, next to none at 9.6): SteamVR's measure spans the
    // frame's whole GPU work with its gaps, and the compositor needs its part of the 11.1 ms. So the mod sets these two to
    // 1.2 and 1.9 ms (FoveatedRendering::gpu_guard_watch); the values here are those of fix96, which the tests use.
    double up_margin_ms{0.3};   // a window counts as late from this much under the frame time
    double margin_ms{1.2};      // back to the full one only with this much of the frame left over afterwards
    int hold_windows{10};       // at least this many windows on the lighter one (5 s)
    int calm_windows{6};        // ... and this many calm windows in a row before going back (3 s)
    // [GUARD fix101 20.11.2026]
    double save_ms[MAX_LEVELS + 1]{}; // GPU time per frame each level saves against level 0 ([0] = 0); all 0 = not given:
                                      // one level per step
    // [fix102 20.11.2026] ... measured by the compositor's own throttling (Window::throttled: it holds the game to 45 fps),
    // no longer by the frames that were late: in the test of fix101 a quarter of the frames over the frame time, with the
    // game still at 90 fps, took the last step three times in 80 s.
    int extreme_level{0};             // > 0: this level (the last one) only when the frame rate collapses
    double extreme_late{0.25};        // ... : this share of the frames throttled in two windows in a row,
    double extreme_late_once{0.40};   // or this share in one
    int extreme_hold_windows{12};     // at least this many windows on it (6 s)

    bool relieved() const { return m_level > 0; }
    int level() const { return m_level; }

    void reset() {
        m_level = 0;
        m_late_streak = 0;
        m_calm_streak = 0;
        m_collapse_streak = 0;
        m_windows_lighter = 0;
    }

    Change update(const Window& w) {
        if (w.frames == 0) {
            m_late_streak = 0;
            m_calm_streak = 0;
            m_collapse_streak = 0;
            return Change::None;
        }

        const double late = (double)w.gpu_late / (double)w.frames;
        const int top = levels < 1 ? 1 : (levels > MAX_LEVELS ? MAX_LEVELS : levels);
        const int top_normal = extreme_level > 0 && extreme_level <= top ? extreme_level - 1 : top;

        if (m_level > top) {
            m_level = top;
        }

        ++m_windows_lighter; // (windows on this level)

        if (m_level < top) {
            const bool late_window = late >= 0.04 || w.gpu_ms >= frame_ms - up_margin_ms;
            m_late_streak = late_window ? m_late_streak + 1 : 0;
            const double held = (double)w.throttled / (double)w.frames;
            m_collapse_streak = held >= extreme_late ? m_collapse_streak + 1 : 0;
            const bool waited = m_level == 0 || m_windows_lighter > step_wait;
            int to = m_level;

            if (m_level < top_normal) {
                if (waited && (late >= 0.15 || m_late_streak >= 2)) {
                    to = m_level + 1;

                    // (as far as the time over the limit asks for, where the caller gave what the levels save)
                    if (save_ms[top_normal] > 0.0) {
                        const double need = w.gpu_ms - (frame_ms - up_margin_ms);

                        while (to < top_normal && save_ms[to] - save_ms[m_level] < need) {
                            ++to;
                        }
                    }
                }
            } else if (waited && (held >= extreme_late_once || m_collapse_streak >= 2)) {
                to = top; // (the level kept for a frame rate that collapses)
            }

            if (to != m_level) {
                m_level = to;
                m_windows_lighter = 0;
                m_calm_streak = 0;
                m_late_streak = 0;
                m_collapse_streak = 0;
                return Change::ToLighter;
            }
        }

        if (m_level == 0) {
            return Change::None;
        }

        const bool calm = w.gpu_late == 0 && w.gpu_ms + full_extra_ms <= frame_ms - margin_ms;
        m_calm_streak = calm ? m_calm_streak + 1 : 0;
        const int hold = extreme_level > 0 && m_level == extreme_level ? extreme_hold_windows : hold_windows;

        if (m_windows_lighter >= hold && m_calm_streak >= calm_windows) {
            --m_level;
            m_windows_lighter = 0;
            m_calm_streak = 0;
            m_late_streak = 0;
            m_collapse_streak = 0;
            return Change::ToFull;
        }

        return Change::None;
    }

private:
    int m_level{0};
    int m_late_streak{0};
    int m_calm_streak{0};
    int m_collapse_streak{0};
    int m_windows_lighter{0};
};
} // namespace quadview
