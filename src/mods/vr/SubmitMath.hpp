#pragma once

// [SUBMIT TIMELINE 06.11.2026] The analysis of one frame's submits (d3d12::SubmitTimeline), without D3D12: how long the game's
// queue worked and stood idle and why, the time of each submit by a name that stays the same from frame to frame, and the
// compute queue's work next to it. Tested by tests/quadview_math_test.cpp.

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace submitmath {
struct Call {
    int kind{0};                 // 0 a direct queue, 1 compute, 2 copy or other, 3 a queue's Wait
    bool game{false};            // on the game's queue (the one that presents)
    const char* tag{nullptr};    // the mod's own submit by name, or none (the engine's)
    bool present_thread{false};  // made on the thread that presents
    uint32_t lists{0};
    double cpu{0};               // when it was made, ms after the frame's start
    bool timed{false};
    double gpu_start{0};         // ms after the frame's start
    double gpu_end{0};
};

struct KeyTime {
    std::string key;
    double busy{0};
    double at{0};
    uint32_t lists{0};
    double overlap{0}; // compute_keys: the part while the game's queue worked
};

struct GapTime {
    std::string key;  // "after <key> before <key>"
    double gap{0};
    double cpu_late{0}; // the part before the CPU had made the next submit
    bool waited{false}; // a Wait on the game's queue between the two
};

struct FrameResult {
    bool has_game{false};
    double span{0};      // the game's queue: first work to last
    double busy{0};
    double idle_cpu{0};
    double idle_wait{0};
    double idle_other{0};
    double end_to_mark{0}; // the last work on the game's queue ended this long before the frame's end mark
    double calls_game{0};
    double lists_game{0};
    double calls_compute{0};
    double busy_compute{0};
    double overlap_compute{0};
    double calls_copy{0};
    double waits{0};
    // [SUBMIT TIMELINE 06.11.2026 evening] the engine's first submit from the thread that presents (its frame from the render
    // thread): how long after it was made the GPU started it (-1: none timed), and when it was made; the game's queue's last
    // work (ms after the frame's start)
    double first_rt_delay{-1};
    double first_rt_cpu{0};
    double last_end{0};
    std::vector<KeyTime> keys{};
    std::vector<KeyTime> compute_keys{}; // [06.11.2026 evening] the compute queues' submits (every queue but the game's direct one)
    std::vector<GapTime> gaps{};
    std::vector<std::string> call_keys{}; // one per call, in the order given ("" for waits and copies: their tag)
};

// "<tag or engine / compute> <R = the thread that presents, W = another>#<n-th such submit of the frame>"
inline std::string key_of(const Call& c, uint32_t ordinal) {
    const char* name = c.tag != nullptr ? c.tag : (c.kind == 1 ? "compute" : "engine");
    return std::format("{} {}#{}", name, c.present_thread ? "R" : "W", ordinal);
}

// calls: in the order they were made (CPU); end_ms: the frame's end mark, ms after its start
inline FrameResult analyze(const std::vector<Call>& calls, double end_ms) {
    FrameResult r{};
    r.call_keys.resize(calls.size());
    std::unordered_map<std::string, uint32_t> ordinal{};
    std::vector<size_t> game{};
    std::vector<size_t> compute{};
    std::vector<size_t> game_waits{};

    for (size_t i = 0; i < calls.size(); ++i) {
        const auto& c = calls[i];

        if (c.kind == 3) {
            r.waits += 1.0;
            r.call_keys[i] = c.tag != nullptr ? c.tag : "";

            if (c.game) {
                game_waits.push_back(i);
            }

            continue;
        }

        if (c.kind == 2) {
            r.calls_copy += 1.0;
            r.call_keys[i] = c.tag != nullptr ? c.tag : "";
            continue;
        }

        const std::string base = std::format("{}/{}/{}", c.kind, c.tag != nullptr ? c.tag : "", c.present_thread ? "R" : "W");
        r.call_keys[i] = key_of(c, ++ordinal[base]);

        if (c.kind == 0 && c.game) {
            r.calls_game += 1.0;
            r.lists_game += (double)c.lists;

            if (c.timed) {
                game.push_back(i);

                if (r.first_rt_delay < 0.0 && c.present_thread && c.tag == nullptr) {
                    r.first_rt_delay = (std::max)(c.gpu_start - c.cpu, 0.0);
                    r.first_rt_cpu = c.cpu;
                }
            }
        } else if (c.kind == 1) {
            r.calls_compute += 1.0;

            if (c.timed) {
                compute.push_back(i);
            }
        }
    }

    // the game's queue in the order the GPU ran it
    std::stable_sort(game.begin(), game.end(), [&](size_t a, size_t b) { return calls[a].gpu_start < calls[b].gpu_start; });

    for (size_t n = 0; n < game.size(); ++n) {
        const auto& c = calls[game[n]];
        const double d = (std::max)(c.gpu_end - c.gpu_start, 0.0);
        r.busy += d;
        r.keys.push_back({r.call_keys[game[n]], d, c.gpu_start, c.lists});

        if (n == 0) {
            continue;
        }

        const auto& a = calls[game[n - 1]];
        const double gap = (std::max)(c.gpu_start - a.gpu_end, 0.0);

        if (gap <= 0.0) {
            continue;
        }

        // idle: the next submit made only after the GPU had finished the one before (the CPU's part), or a Wait in between
        const double cpu_late = (std::clamp)(c.cpu - a.gpu_end, 0.0, gap);
        bool waited = false;

        for (const size_t w : game_waits) {
            if (calls[w].cpu >= a.cpu && calls[w].cpu <= c.cpu) {
                waited = true;
                break;
            }
        }

        r.idle_cpu += cpu_late;
        (waited ? r.idle_wait : r.idle_other) += gap - cpu_late;
        r.gaps.push_back({std::format("after {} before {}", r.call_keys[game[n - 1]], r.call_keys[game[n]]), gap, cpu_late, waited});
    }

    if (!game.empty()) {
        double first = calls[game.front()].gpu_start;
        double last = calls[game.front()].gpu_end;

        for (const size_t i : game) {
            first = (std::min)(first, calls[i].gpu_start);
            last = (std::max)(last, calls[i].gpu_end);
        }

        r.has_game = true;
        r.span = last - first;
        r.end_to_mark = end_ms - last;
        r.last_end = last;
    }

    // the compute queue's work and the part of it while the game's queue worked
    for (const size_t i : compute) {
        const double s = calls[i].gpu_start;
        const double e = (std::max)(calls[i].gpu_end, s);
        double overlap = 0.0;

        for (const size_t g : game) {
            overlap += (std::max)(0.0, (std::min)(e, calls[g].gpu_end) - (std::max)(s, calls[g].gpu_start));
        }

        r.busy_compute += e - s;
        r.overlap_compute += overlap;
        r.compute_keys.push_back({r.call_keys[i], e - s, s, calls[i].lists, overlap});
    }

    return r;
}
} // namespace submitmath
