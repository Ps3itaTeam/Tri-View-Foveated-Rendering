// ThreadCpuSampler.hpp -- [CPU THREADS 03.11.2026] How busy each thread of the game is over an interval.
//
// Windows counts the CPU time of every thread (QueryThreadCycleTime: time stamp counter ticks while the thread runs, in
// user and kernel mode); the ticks are turned into milliseconds with the counter's rate measured over the same interval.
// A thread that waits by spinning counts as busy, a thread that sleeps does not. Taking the system's thread list costs a
// few milliseconds, so this runs on the debug log's writer thread (FoveatedRendering::log_writer), asked for by the
// periodic status line. Only included by FoveatedRendering.cpp.
#pragma once

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace threadcpu {
// What the status line knows when it asks: the frames counted so far and the threads it can name
struct Request {
    uint64_t frames{0};      // frames since the start (the difference to the last request = frames of the interval)
    uint32_t main_tid{0};    // the game's main thread (application entries)
    uint32_t present_tid{0}; // the thread that presents (the render thread's end of a frame)
    uint32_t draw_tid{0};    // the thread that ran the last scene layer draw
};

class Sampler {
public:
    Sampler() = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    ~Sampler() {
        for (auto& [tid, t] : m_threads) {
            if (t.handle != nullptr) {
                CloseHandle(t.handle);
            }
        }
    }

    // One line for the debug log; empty the first time (it only starts the interval) or when nothing could be read
    std::string sample(const Request& r) {
        LARGE_INTEGER qpc{};
        LARGE_INTEGER qpf{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&qpf);
        const uint64_t tsc = __rdtsc();
        ULONG64 process_cycles = 0;
        QueryProcessCycleTime(GetCurrentProcess(), &process_cycles);

        if (!refresh()) {
            return {};
        }

        const bool first = m_last_qpc == 0;
        const double seconds = first ? 0.0 : (double)(qpc.QuadPart - m_last_qpc) / (double)qpf.QuadPart;
        const double tsc_hz = seconds > 0.0 ? (double)(tsc - m_last_tsc) / seconds : 0.0;
        const uint64_t frames = r.frames - m_last_frames;
        const uint64_t process_delta = process_cycles - m_last_process_cycles;

        m_last_qpc = qpc.QuadPart;
        m_last_tsc = tsc;
        m_last_frames = r.frames;
        m_last_process_cycles = process_cycles;

        if (first || seconds < 0.5 || tsc_hz <= 0.0 || frames == 0) {
            return {};
        }

        const double frame_ms = seconds * 1000.0 / (double)frames;
        const auto ms_per_frame = [&](uint64_t cycles) { return (double)cycles / tsc_hz * 1000.0 / (double)frames; };
        const auto share = [&](uint64_t cycles) { return (double)cycles / tsc_hz / seconds * 100.0; };

        struct Row {
            uint32_t tid;
            uint64_t cycles;
        };

        std::vector<Row> rows{};
        rows.reserve(m_threads.size());
        int over90 = 0, over50 = 0, over25 = 0;

        for (const auto& [tid, t] : m_threads) {
            if (t.fresh) {
                continue;
            }

            rows.push_back(Row{tid, t.delta});
            const double s = share(t.delta);
            over90 += s >= 90.0 ? 1 : 0;
            over50 += s >= 50.0 ? 1 : 0;
            over25 += s >= 25.0 ? 1 : 0;
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cycles > b.cycles; });

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const double process_ms = ms_per_frame(process_delta);

        std::string o = std::format("threads, CPU time per frame over {:.1f} s ({} frames, {:.2f} ms each): the game {:.1f} ms = {:.1f} of {} "
                                    "logical processors busy on average",
            seconds, frames, frame_ms, process_ms, process_ms / frame_ms, (unsigned)si.dwNumberOfProcessors);

        const auto role_row = [&](const char* label, uint32_t tid) {
            if (tid == 0) {
                return;
            }

            const auto it = m_threads.find(tid);

            if (it == m_threads.end() || it->second.fresh) {
                o += std::format(" | {} (thread {}): not read", label, tid);
                return;
            }

            o += std::format(" | {} (thread {}{}): {:.2f} ms ({:.0f}%)", label, tid, name_suffix(it->second), ms_per_frame(it->second.delta),
                share(it->second.delta));
        };

        role_row("main thread", r.main_tid);

        if (r.present_tid != 0 && r.present_tid == r.main_tid) {
            o += " | presents on the main thread";
        } else {
            role_row("render thread (presents)", r.present_tid);
        }

        if (r.draw_tid != 0 && r.draw_tid != r.present_tid && r.draw_tid != r.main_tid) {
            role_row("scene layer draws", r.draw_tid);
        } else if (r.draw_tid != 0) {
            o += r.draw_tid == r.present_tid ? " | scene layer draws on the render thread" : " | scene layer draws on the main thread";
        }

        o += " | busiest threads:";

        for (size_t i = 0; i < rows.size() && i < 16; ++i) {
            const auto& t = m_threads[rows[i].tid];
            const char* role = rows[i].tid == r.main_tid ? " main" : rows[i].tid == r.present_tid ? " render" : rows[i].tid == r.draw_tid ? " draws" : "";
            o += std::format(" {}{}{} {:.2f} ({:.0f}%);", rows[i].tid, role, name_suffix(t), ms_per_frame(rows[i].cycles), share(rows[i].cycles));
        }

        o += std::format(" | {} threads, {} busy over 90%, {} over 50%, {} over 25%", rows.size(), over90, over50, over25);
        return o;
    }

private:
    struct Thread {
        HANDLE handle{nullptr};
        uint64_t cycles{0};
        uint64_t delta{0};
        std::string name{};
        bool seen{false};
        bool fresh{true}; // found in this sample: no interval yet
    };

    // the thread's name as the game set it (Windows 10 1607+), or ""
    static std::string thread_name(HANDLE h) {
        using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto fn = []() -> GetThreadDescriptionFn {
            if (const auto k = GetModuleHandleW(L"kernel32.dll"); k != nullptr) {
                return (GetThreadDescriptionFn)(void*)GetProcAddress(k, "GetThreadDescription");
            }

            return nullptr;
        }();

        if (fn == nullptr || h == nullptr) {
            return {};
        }

        PWSTR w = nullptr;

        if (FAILED(fn(h, &w)) || w == nullptr) {
            return {};
        }

        std::string out{};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);

        if (n > 1) {
            out.resize((size_t)n - 1);
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
        }

        LocalFree(w);

        // one word in the log line
        for (auto& c : out) {
            if (c == ' ' || c == ';' || c == '|') {
                c = '_';
            }
        }

        return out.size() > 40 ? out.substr(0, 40) : out;
    }

    static std::string name_suffix(const Thread& t) { return t.name.empty() ? std::string{} : " " + t.name; }

    // the process's threads now: new ones opened, ended ones closed, cycle counts and their change since the last sample
    bool refresh() {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

        if (snap == INVALID_HANDLE_VALUE) {
            return false;
        }

        const DWORD pid = GetCurrentProcessId();

        for (auto& [tid, t] : m_threads) {
            t.seen = false;
        }

        THREADENTRY32 te{};
        te.dwSize = sizeof(te);

        if (Thread32First(snap, &te)) {
            do {
                if (te.dwSize >= offsetof(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) && te.th32OwnerProcessID == pid) {
                    const uint32_t tid = te.th32ThreadID;
                    auto it = m_threads.find(tid);

                    if (it == m_threads.end()) {
                        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);

                        if (h != nullptr) {
                            Thread t{};
                            t.handle = h;
                            ULONG64 c = 0;
                            QueryThreadCycleTime(h, &c);
                            t.cycles = c;
                            t.name = thread_name(h);
                            t.seen = true;
                            t.fresh = true;
                            m_threads.emplace(tid, std::move(t));
                        }
                    } else {
                        auto& t = it->second;
                        ULONG64 c = 0;

                        if (QueryThreadCycleTime(t.handle, &c)) {
                            t.delta = c >= t.cycles ? c - t.cycles : 0;
                            t.cycles = c;
                        } else {
                            t.delta = 0;
                        }

                        if (t.name.empty() || (++m_name_refresh % 64) == 0) {
                            t.name = thread_name(t.handle);
                        }

                        t.seen = true;
                        t.fresh = false;
                    }
                }

                te.dwSize = sizeof(te);
            } while (Thread32Next(snap, &te));
        }

        CloseHandle(snap);

        for (auto it = m_threads.begin(); it != m_threads.end();) {
            if (!it->second.seen) {
                CloseHandle(it->second.handle);
                it = m_threads.erase(it);
            } else {
                ++it;
            }
        }

        return true;
    }

    std::unordered_map<uint32_t, Thread> m_threads{};
    int64_t m_last_qpc{0};
    uint64_t m_last_tsc{0};
    uint64_t m_last_frames{0};
    uint64_t m_last_process_cycles{0};
    uint32_t m_name_refresh{0};
};
} // namespace threadcpu
