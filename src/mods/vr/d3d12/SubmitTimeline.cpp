#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the GPU timeline: debug build only (the header has the release's empty stand-in)
#include <algorithm>
#include <stdexcept>
#include <format>
#include <thread>

#include <windows.h>

#include <spdlog/spdlog.h>
#include <utility/PointerHook.hpp>

#include "SubmitTimeline.hpp"

namespace d3d12 {
namespace {
thread_local const char* t_tag = nullptr;
thread_local bool t_in_hook = false;

// ID3D12CommandQueue: IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild 7, UpdateTileMappings 8, CopyTileMappings 9,
// ExecuteCommandLists 10, SetMarker 11, BeginEvent 12, EndEvent 13, Signal 14, Wait 15
constexpr size_t EXECUTE_INDEX = 10;
constexpr size_t WAIT_INDEX = 15;
constexpr uint32_t CALIBRATE_EVERY = 16; // frames
// [GAP TEST 06.11.2026 night] the test load's submits by name (d3d12::GapLoad)
constexpr const char* LOAD_TAG_A = "gap test load A";
constexpr const char* LOAD_TAG_B = "gap test load B";
constexpr const char* LOAD_TAG_REF = "test load after the wide view's first pass";

int64_t qpc_now() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
} // namespace

SubmitTimeline::Tag::Tag(const char* name) : prev{t_tag} {
    t_tag = name;
}

SubmitTimeline::Tag::~Tag() {
    t_tag = prev;
}

SubmitTimeline& SubmitTimeline::get() {
    // (kept to the end of the process: the hook may still be entered while the game shuts down)
    static auto* s = new SubmitTimeline{};
    return *s;
}

void STDMETHODCALLTYPE SubmitTimeline::execute_hook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    auto& t = get();
    const auto original = t.m_execute_original.load(std::memory_order_acquire);

    if (t_in_hook) {
        original(queue, count, lists);
        return;
    }

    // counted in before the check: reset() waits for the calls inside to leave before it drops the command lists
    t.m_in_flight.fetch_add(1, std::memory_order_acq_rel);

    if (!t.m_running.load(std::memory_order_acquire) || count == 0 || lists == nullptr) {
        t.m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        original(queue, count, lists);
        return;
    }

    t_in_hook = true;
    ID3D12CommandList* stamps[2]{};

    if (t.on_call(queue, count, stamps)) {
        // the two stamps in the same call: before the game's lists and after them
        ID3D12CommandList* all[MAX_LISTS + 2]{};
        all[0] = stamps[0];

        for (UINT i = 0; i < count; ++i) {
            all[i + 1] = lists[i];
        }

        all[count + 1] = stamps[1];
        original(queue, count + 2, all);
    } else {
        original(queue, count, lists);
    }

    t.after_call(queue); // [06.11.2026 evening] the engine's submits of the frame from the thread that presents, counted
    t_in_hook = false;
    t.m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

// (the thread that presents makes the engine's frame: m_rt_engine_calls and m_load_number are only used there and in
// frame_mark; t_in_hook is set: the load goes straight to the original)
void SubmitTimeline::after_call(ID3D12CommandQueue* queue) {
    if (queue != m_game_queue || t_tag != nullptr || GetCurrentThreadId() != m_present_thread) {
        return;
    }

    const uint32_t n = ++m_rt_engine_calls;
    const int mode = m_load_mode.load(std::memory_order_acquire);

    if (mode == 0 || !m_load.ready()) {
        return;
    }

    // [GAP TEST 06.11.2026 night] the reference: right after the submit (the small and large loads go in before_wait)
    if (mode == 3 && n == REF_AFTER) {
        queue_test_load(queue, true, LOAD_TAG_REF);
        queue_test_load(queue, true, LOAD_TAG_REF);
    }
}

// [GAP TEST 07.11.2026] the small and large loads: right before the engine's wait on the game's queue that follows its
// GAP_AFTER_A-th / GAP_AFTER_B-th submit. Not right after the submit: the engine signals its compute work between the two
// (the test of 07.11 15:00 with the load after the submit: the compute work started only after the load, and the queue
// still waited for all of it). Here the signal is already on the queue and the load runs while the compute work does.
void SubmitTimeline::before_wait(ID3D12CommandQueue* queue) {
    if (queue != m_game_queue || t_tag != nullptr || GetCurrentThreadId() != m_present_thread) {
        return;
    }

    const int mode = m_load_mode.load(std::memory_order_acquire);
    const uint32_t n = m_rt_engine_calls;

    if ((mode != 1 && mode != 2) || (n != GAP_AFTER_A && n != GAP_AFTER_B) || m_load_wait_done == n || !m_load.ready()) {
        return;
    }

    // one load per place (the first wait after that submit)
    m_load_wait_done = n;
    // counted in, as in the hook: reset() waits for it before it drops the lists
    m_in_flight.fetch_add(1, std::memory_order_acq_rel);

    if (m_running.load(std::memory_order_acquire)) {
        t_in_hook = true;
        queue_test_load(queue, mode == 2, n == GAP_AFTER_A ? LOAD_TAG_A : LOAD_TAG_B);
        t_in_hook = false;
    }

    m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

void SubmitTimeline::queue_test_load(ID3D12CommandQueue* queue, bool large, const char* tag) {
    const auto original = m_execute_original.load(std::memory_order_acquire);

    if (original == nullptr) {
        return;
    }

    const uint64_t number = ++m_load_number;
    const auto slot = (uint32_t)(number % GapLoad::SLOTS);

    if (!m_load.slot_free(slot)) {
        std::scoped_lock _{m_mutex};
        ++m_totals.loads_skipped;
        return;
    }

    m_load.use_slot(slot, number);
    ID3D12CommandList* l = m_load.list(large, slot);
    const char* prev = t_tag;
    t_tag = tag;
    ID3D12CommandList* stamps[2]{};

    if (on_call(queue, 1, stamps)) {
        ID3D12CommandList* all[3]{stamps[0], l, stamps[1]};
        original(queue, 3, all);
    } else {
        original(queue, 1, &l);
    }

    t_tag = prev;
    queue->Signal(m_load.done_fence(), number);
    std::scoped_lock _{m_mutex};
    (tag == LOAD_TAG_A ? m_totals.loads_a : tag == LOAD_TAG_B ? m_totals.loads_b : m_totals.loads_ref) += 1;
}

bool SubmitTimeline::prepare_test_load(ID3D12Device* device, uint32_t w, uint32_t h, std::string& why) {
    // (not while a load may be in a list in flight: only made when none runs)
    if (m_load_mode.load(std::memory_order_acquire) != 0) {
        return m_load.ready();
    }

    if (m_load.ready() && m_load.width() == w && m_load.height() == h) {
        return true;
    }

    // the frames in flight may still use the lists of the old one
    for (int i = 0; i < 2000 && m_in_flight.load(std::memory_order_acquire) > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::scoped_lock _{m_mutex};
    return m_load.setup(device, w, h, why);
}

HRESULT STDMETHODCALLTYPE SubmitTimeline::wait_hook(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    auto& t = get();
    const auto original = t.m_wait_original.load(std::memory_order_acquire);

    if (!t_in_hook && t.m_running.load(std::memory_order_acquire)) {
        t.before_wait(queue); // [GAP TEST 07.11.2026] the test load goes in before the wait (and before its record)
        t.on_wait(queue);
    }

    return original(queue, fence, value);
}

void SubmitTimeline::marker(const char* name) {
    if (t_in_hook || !m_running.load(std::memory_order_acquire)) {
        return;
    }

    // counted in before the check, as in the hook: reset() waits for it before it drops the stamp lists
    m_in_flight.fetch_add(1, std::memory_order_acq_rel);
    const auto original = m_execute_original.load(std::memory_order_acquire);
    ID3D12CommandQueue* queue = m_game_queue;

    if (m_running.load(std::memory_order_acquire) && original != nullptr && queue != nullptr) {
        t_in_hook = true;
        const char* prev = t_tag;
        t_tag = name;
        ID3D12CommandList* stamps[2]{};

        if (on_call(queue, 0, stamps)) {
            original(queue, 2, stamps);
        }

        t_tag = prev;
        t_in_hook = false;
    }

    m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

bool SubmitTimeline::start(ID3D12Device* device, ID3D12CommandQueue* game_queue, std::string& why) {
    std::scoped_lock _{m_mutex};

    if (m_running.load()) {
        return true;
    }

    if (device == nullptr || game_queue == nullptr) {
        why = "no D3D12 device or queue";
        return false;
    }

    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    m_qpc_freq = (std::max)(qpf.QuadPart, (LONGLONG)1);

    if (m_heap == nullptr) {
        const uint32_t count = 2 * FRAMES * STAMPS;
        D3D12_QUERY_HEAP_DESC hd{};
        hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        hd.Count = count;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf{};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = (UINT64)count * 8;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        void* mapped = nullptr;

        if (FAILED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&m_heap))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback))) ||
            FAILED(m_readback->Map(0, nullptr, &mapped))) {
            why = "the timestamp query heap or its readback buffer could not be made";
            release_gpu_objects();
            return false;
        }

        m_mapped = (const uint64_t*)mapped;

        // the stamp lists, recorded once: EndQuery and its resolve into the readback buffer (executed again 8 frames later)
        for (int kind = 0; kind < 2; ++kind) {
            const auto type = kind == 0 ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
            m_lists[kind].resize((size_t)FRAMES * STAMPS);

            for (uint32_t s = 0; s < FRAMES; ++s) {
                if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_alloc[kind][s])))) {
                    why = "a command allocator could not be made";
                    release_gpu_objects();
                    return false;
                }

                for (uint32_t i = 0; i < STAMPS; ++i) {
                    auto& list = m_lists[kind][(size_t)s * STAMPS + i];
                    const uint32_t q = ((uint32_t)kind * FRAMES + s) * STAMPS + i;

                    if (FAILED(device->CreateCommandList(0, type, m_alloc[kind][s].Get(), nullptr, IID_PPV_ARGS(&list)))) {
                        why = "a command list could not be made";
                        release_gpu_objects();
                        return false;
                    }

                    list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
                    list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 1, m_readback.Get(), (UINT64)q * 8);

                    if (FAILED(list->Close())) {
                        why = "a command list could not be closed";
                        release_gpu_objects();
                        return false;
                    }
                }
            }
        }
    }

    if (m_execute_hook == nullptr) {
        m_vtable = *(void***)game_queue;
        // the original first: a call can come through the new entry right after it is written
        m_execute_original.store((ExecuteFn)m_vtable[EXECUTE_INDEX], std::memory_order_release);
        m_wait_original.store((WaitFn)m_vtable[WAIT_INDEX], std::memory_order_release);
        try {
            m_execute_hook = std::make_unique<PointerHook>(&m_vtable[EXECUTE_INDEX], (void*)&SubmitTimeline::execute_hook);
            m_wait_hook = std::make_unique<PointerHook>(&m_vtable[WAIT_INDEX], (void*)&SubmitTimeline::wait_hook);
        } catch (const std::exception& e) {
            m_execute_hook.reset();
            m_wait_hook.reset();
            why = std::string{"the queue could not be hooked: "} + e.what();
            return false;
        }

        REVR_LOG(spdlog::info("[SubmitTimeline] the queue class's ExecuteCommandLists and Wait hooked"));
    } else if (*(void***)game_queue != m_vtable) {
        REVR_LOG(spdlog::warn("[SubmitTimeline] the game's queue has another class than the one hooked: its submits are not seen"));
    }

    m_game_queue = game_queue;
    m_device = device;

    // frames of an earlier run still waiting for the GPU are dropped when they are done (their stamp lists may be in flight)
    for (auto& fr : m_frames) {
        if (fr.closed) {
            fr.instrumented = false;
        }

        fr.open = false;
    }

    m_totals = Totals{};
    m_last_calls.clear();
    m_prev_calls.clear();
    auto& f = m_frames[m_frame_number % FRAMES];
    const bool pending = f.closed;
    f = Frame{};
    f.number = m_frame_number;
    f.open = true;
    f.instrumented = !pending;
    f.start_cpu = qpc_now();
    m_calibrated = 0;
    m_running.store(true, std::memory_order_release);
    REVR_LOG(spdlog::info("[SubmitTimeline] running"));
    return true;
}

void SubmitTimeline::stop() {
    m_running.store(false, std::memory_order_release);

    {
        // the frame in progress gets its fences like any other (its stamp lists are not used again before they pass)
        std::scoped_lock _{m_mutex};

        if (m_heap != nullptr) {
            close_open_frame(qpc_now());
        }
    }

    REVR_LOG(spdlog::info("[SubmitTimeline] stopped"));
}

void SubmitTimeline::reset() {
    m_running.store(false, std::memory_order_release);

    // the calls inside the hook have taken command lists of ours: they leave first
    for (int i = 0; i < 2000 && m_in_flight.load(std::memory_order_acquire) > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::scoped_lock _{m_mutex};
    release_gpu_objects();
    m_load_mode.store(0, std::memory_order_release);
    m_load.release(); // [GAP TEST 06.11.2026 night]
    m_queues.clear();
    m_game_queue = nullptr;
    m_device = nullptr;

    for (auto& f : m_frames) {
        f = Frame{};
    }
}

void SubmitTimeline::release_gpu_objects() {
    for (auto& kind : m_lists) {
        kind.clear();
    }

    for (auto& kind : m_alloc) {
        for (auto& a : kind) {
            a.Reset();
        }
    }

    if (m_readback != nullptr && m_mapped != nullptr) {
        m_readback->Unmap(0, nullptr);
    }

    m_mapped = nullptr;
    m_readback.Reset();
    m_heap.Reset();
}

int SubmitTimeline::queue_index(ID3D12CommandQueue* queue) {
    for (size_t i = 0; i < m_queues.size(); ++i) {
        if (m_queues[i].queue.Get() == queue) {
            return (int)i;
        }
    }

    if (queue == nullptr || m_queues.size() >= 16) {
        return -1;
    }

    Queue q{};
    q.queue = queue;
    q.type = queue->GetDesc().Type;

    if (q.type == D3D12_COMMAND_LIST_TYPE_DIRECT || q.type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        UINT64 freq = 0;

        if (SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq > 0) {
            q.freq = freq;
            queue->GetClockCalibration(&q.cal_gpu, &q.cal_cpu);
        }

        if (m_device != nullptr) {
            m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&q.fence));
        }
    }

    m_queues.push_back(std::move(q));
    return (int)m_queues.size() - 1;
}

bool SubmitTimeline::on_call(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* stamps[2]) {
    const auto now = qpc_now();
    std::scoped_lock _{m_mutex};

    if (m_heap == nullptr) {
        return false;
    }

    const int qi = queue_index(queue);
    auto& f = m_frames[m_frame_number % FRAMES];

    if (qi < 0 || !f.open) {
        return false;
    }

    const auto& q = m_queues[(size_t)qi];
    Call c{};
    c.queue = (uint8_t)qi;
    c.lists = (uint16_t)(std::min)(count, 65535u);
    c.thread = GetCurrentThreadId();
    c.tag = t_tag;
    c.cpu = now;
    c.kind = q.type == D3D12_COMMAND_LIST_TYPE_DIRECT ? 0 : q.type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 2;
    bool stamped = false;

    if (c.kind < 2 && f.instrumented && q.freq > 0 && count <= MAX_LISTS) {
        if (f.used[c.kind] + 2 <= STAMPS) {
            const size_t base = (size_t)(f.number % FRAMES) * STAMPS + f.used[c.kind];
            c.stamp = (int32_t)f.used[c.kind];
            stamps[0] = m_lists[c.kind][base].Get();
            stamps[1] = m_lists[c.kind][base + 1].Get();
            f.used[c.kind] += 2;
            stamped = true;
        } else {
            ++f.overflow;
        }
    }

    if (f.calls.size() < 1024) {
        f.calls.push_back(c);
    }

    if (!m_threads.contains(c.thread)) {
        m_threads[c.thread] = (uint32_t)m_threads.size() + 1;
    }

    return stamped;
}

void SubmitTimeline::on_wait(ID3D12CommandQueue* queue) {
    const auto now = qpc_now();
    std::scoped_lock _{m_mutex};
    const int qi = queue_index(queue);
    auto& f = m_frames[m_frame_number % FRAMES];

    if (qi < 0 || !f.open || f.calls.size() >= 1024) {
        return;
    }

    Call c{};
    c.kind = 3;
    c.queue = (uint8_t)qi;
    c.thread = GetCurrentThreadId();
    c.tag = t_tag;
    c.cpu = now;
    f.calls.push_back(c);
}

// the open frame closed: a fence after its work on every queue it used (and the game's); m_mutex held
void SubmitTimeline::close_open_frame(int64_t now) {
    auto& f = m_frames[m_frame_number % FRAMES];

    if (f.open) {
        f.open = false;
        f.closed = true;
        f.end_cpu = now;
        f.fences.clear();

        // a fence after the frame's work on every queue it used (and the game's): read back once all have passed it
        std::vector<bool> used(m_queues.size(), false);

        if (const int g = queue_index(m_game_queue); g >= 0) {
            used[(size_t)g] = true;
        }

        for (const auto& c : f.calls) {
            if (c.kind < 2 && c.queue < used.size()) {
                used[c.queue] = true;
            }
        }

        for (size_t i = 0; i < m_queues.size() && i < used.size(); ++i) {
            auto& q = m_queues[i];

            if (used[i] && q.fence != nullptr && SUCCEEDED(q.queue->Signal(q.fence.Get(), q.fence_value + 1))) {
                ++q.fence_value;
                f.fences.emplace_back((uint32_t)i, q.fence_value);
            }
        }

        // the GPU clocks against the CPU's, now and then
        if (m_calibrated++ % CALIBRATE_EVERY == 0) {
            for (auto& q : m_queues) {
                if (q.freq > 0) {
                    q.queue->GetClockCalibration(&q.cal_gpu, &q.cal_cpu);
                }
            }
        }
    }
}

void SubmitTimeline::frame_mark() {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = qpc_now();
    std::scoped_lock _{m_mutex};

    if (m_heap == nullptr) {
        return;
    }

    m_present_thread = GetCurrentThreadId();
    close_open_frame(now);
    read_back();

    // [PARALLEL TEST 06.11.2026 evening] the engine's submits of this frame from the thread that presents
    m_totals.rt_engine_calls += m_rt_engine_calls;
    ++m_totals.cpu_frames;

    if (m_rt_engine_calls != 15) {
        ++m_totals.frames_other_count;
    }

    m_rt_engine_calls = 0;
    m_load_wait_done = 0;

    ++m_frame_number;
    auto& n = m_frames[m_frame_number % FRAMES];
    const bool pending = n.closed; // still not done on the GPU after 8 frames: its stamp lists may still be in flight

    if (pending) {
        ++m_totals.skipped;
    }

    n = Frame{};
    n.number = m_frame_number;
    n.open = true;
    n.instrumented = !pending;
    n.start_cpu = now;
    n.calls.reserve(160);
}

void SubmitTimeline::read_back() {
    // the frames done on the GPU, oldest first (the detail shows the end of the frame before the last one read)
    Frame* done_frames[FRAMES]{};
    uint32_t n = 0;

    for (auto& f : m_frames) {
        if (!f.closed) {
            continue;
        }

        bool done = true;

        for (const auto& [qi, value] : f.fences) {
            if (qi >= m_queues.size() || m_queues[qi].fence == nullptr || m_queues[qi].fence->GetCompletedValue() < value) {
                done = false;
                break;
            }
        }

        if (done && n < FRAMES) {
            done_frames[n++] = &f;
        }
    }

    std::sort(done_frames, done_frames + n, [](const Frame* a, const Frame* b) { return a->number < b->number; });

    for (uint32_t i = 0; i < n; ++i) {
        auto& f = *done_frames[i];

        if (f.instrumented) {
            process(f);
        } else {
            ++m_totals.skipped;
        }

        f.closed = false;
        f.calls.clear();
    }
}

double SubmitTimeline::to_ms(const Queue& q, uint64_t gpu, int64_t ref_cpu) const {
    const double gpu_s = (double)(int64_t)(gpu - q.cal_gpu) / (double)q.freq;
    const double cpu = (double)(int64_t)q.cal_cpu + gpu_s * (double)m_qpc_freq;
    return (cpu - (double)ref_cpu) * 1000.0 / (double)m_qpc_freq;
}

// the frame's submits as the analysis takes them (submitmath, tested without D3D12)
std::vector<submitmath::Call> SubmitTimeline::as_math(const std::vector<Call>& calls, int64_t start_cpu, int game) const {
    std::vector<submitmath::Call> out{};
    out.reserve(calls.size());

    for (const auto& c : calls) {
        submitmath::Call m{};
        m.kind = c.kind;
        m.game = (int)c.queue == game;
        m.tag = c.tag;
        m.present_thread = c.thread == m_present_thread;
        m.lists = c.lists;
        m.cpu = (double)(c.cpu - start_cpu) * 1000.0 / (double)m_qpc_freq;
        m.timed = c.timed;
        m.gpu_start = c.gpu_start;
        m.gpu_end = c.gpu_end;
        out.push_back(m);
    }

    return out;
}

void SubmitTimeline::process(Frame& f) {
    const size_t slot = (size_t)(f.number % FRAMES);

    for (auto& c : f.calls) {
        if (c.stamp < 0 || c.kind > 1 || c.queue >= m_queues.size()) {
            continue;
        }

        const auto& q = m_queues[c.queue];
        const size_t idx = ((size_t)c.kind * FRAMES + slot) * STAMPS + (size_t)c.stamp;
        const uint64_t s = m_mapped[idx];
        const uint64_t e = m_mapped[idx + 1];

        if (s == 0 || e < s || q.freq == 0) {
            continue;
        }

        c.gpu_start = to_ms(q, s, f.start_cpu);
        c.gpu_end = to_ms(q, e, f.start_cpu);
        c.timed = true;
    }

    const int g = queue_index(m_game_queue);
    const auto r = submitmath::analyze(as_math(f.calls, f.start_cpu, g), (double)(f.end_cpu - f.start_cpu) * 1000.0 / (double)m_qpc_freq);
    auto& t = m_totals;
    ++t.frames;
    t.overflow += f.overflow;
    t.calls_game += r.calls_game;
    t.lists_game += r.lists_game;
    t.calls_compute += r.calls_compute;
    t.busy_compute += r.busy_compute;
    t.overlap_compute += r.overlap_compute;
    t.calls_copy += r.calls_copy;
    t.waits += r.waits;

    if (r.has_game) {
        ++t.frames_game;
        t.span += r.span;
        t.busy += r.busy;
        t.idle_cpu += r.idle_cpu;
        t.idle_wait += r.idle_wait;
        t.idle_other += r.idle_other;
        t.end_to_mark += r.end_to_mark;
        m_last_span = r.span;
    }

    if (r.first_rt_delay >= 0.0) {
        t.rt_delay += r.first_rt_delay;
        t.rt_delay_max = (std::max)(t.rt_delay_max, r.first_rt_delay);
        ++t.rt_frames;
        t.rt_over_half += r.first_rt_delay > 0.5 ? 1u : 0u;
        t.rt_over_two += r.first_rt_delay > 2.0 ? 1u : 0u;
    }

    for (const auto& k : r.keys) {
        auto& v = t.keys[k.key];
        v.busy += k.busy;
        v.at += k.at;
        v.lists += (double)k.lists;
        ++v.n;
    }

    for (const auto& k : r.compute_keys) {
        auto& v = t.compute_keys[k.key];
        v.busy += k.busy;
        v.at += k.at;
        v.lists += (double)k.lists;
        v.overlap += k.overlap;
        ++v.n;
    }

    for (const auto& gp : r.gaps) {
        auto& v = t.gaps[gp.key];
        v.gap += gp.gap;
        v.cpu_late += gp.cpu_late;
        v.waits += gp.waited ? 1u : 0u;
        ++v.n;
    }

    // the frame before it is kept when it is the one just before (its end is shown with the detail)
    if (!m_last_calls.empty() && m_last_number + 1 == f.number) {
        m_prev_calls = std::move(m_last_calls);
        m_prev_number = m_last_number;
        m_prev_start_cpu = m_last_start_cpu;
        m_prev_end_cpu = m_last_end_cpu;
    } else {
        m_prev_calls.clear();
    }

    m_last_calls = f.calls;
    m_last_number = f.number;
    m_last_start_cpu = f.start_cpu;
    m_last_end_cpu = f.end_cpu;
}

std::string SubmitTimeline::take_report(bool detail) {
    std::scoped_lock _{m_mutex};
    auto& t = m_totals;

    if (t.frames == 0 || t.frames_game == 0) {
        std::string o = std::format("GPU timeline of the frame: no frame read back with work on the game's queue ({} read, {} without stamps)", t.frames, t.skipped);
        t = Totals{};
        return o;
    }

    const double n = (double)t.frames;
    const double ng = (double)t.frames_game;
    std::string o = std::format(
        "GPU timeline of the frame ({} frames, {} without stamps, {} submits over the limit): the game's queue from its first work to its last "
        "{:.2f} ms per frame, busy {:.2f}, idle {:.2f} (the next work not yet sent by the CPU {:.2f}, after a wait for another queue {:.2f}, "
        "other {:.2f}); its last work ends {:.2f} ms before the frame's end (after the game's Present) | submits per frame: game's queue {:.1f} "
        "({:.1f} command lists), compute queue {:.1f} (busy {:.2f} ms, {:.2f} of it while the game's queue works), copy {:.1f}, queue waits {:.1f}",
        t.frames, t.skipped, t.overflow, t.span / ng, t.busy / ng, (t.span - t.busy) / ng, t.idle_cpu / ng, t.idle_wait / ng, t.idle_other / ng,
        t.end_to_mark / ng, t.calls_game / n, t.lists_game / n, t.calls_compute / n, t.busy_compute / n, t.overlap_compute / n, t.calls_copy / n,
        t.waits / n);

    // [06.11.2026 evening] the engine's frame from the render thread: how long its first submit waited on the queue
    if (t.rt_frames > 0) {
        o += std::format(" | the engine's first submit from the thread that presents started {:.2f} ms after it was sent (over 0.5 ms in {} of {} "
                         "frames, over 2 ms in {}, the longest {:.2f})",
            t.rt_delay / (double)t.rt_frames, t.rt_over_half, t.rt_frames, t.rt_over_two, t.rt_delay_max);
    }

    // where the game's queue stands idle (per frame), the largest first
    std::vector<std::pair<std::string, Totals::Gap>> gaps(t.gaps.begin(), t.gaps.end());
    std::sort(gaps.begin(), gaps.end(), [](const auto& a, const auto& b) { return a.second.gap > b.second.gap; });
    o += "\n    idle on the game's queue (ms per frame; the part before the CPU had sent the next work; frames with a queue wait in between):";

    for (size_t i = 0; i < gaps.size() && i < 8; ++i) {
        const auto& [k, g] = gaps[i];
        o += std::format(" {} {:.3f} (CPU {:.3f}, waits {}/{}) |", k, g.gap / ng, g.cpu_late / ng, g.waits, g.n);
    }

    // the submits that take the game's queue's time (per frame), the largest first
    std::vector<std::pair<std::string, Totals::Key>> keys(t.keys.begin(), t.keys.end());
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.second.busy > b.second.busy; });
    o += "\n    work on the game's queue (ms per frame; starting ms after the frame's start; command lists; frames with it):";

    for (size_t i = 0; i < keys.size() && i < 14; ++i) {
        const auto& [k, v] = keys[i];
        const double kn = (std::max)((double)v.n, 1.0);
        o += std::format(" {} {:.3f} (at {:.2f}, {:.1f} lists, {}/{}) |", k, v.busy / ng, v.at / kn, v.lists / kn, v.n, t.frames_game);
    }

    // [06.11.2026 evening] the compute queues' submits by name, with their part
    // while the game's queue worked
    if (!t.compute_keys.empty()) {
        std::vector<std::pair<std::string, Totals::Key>> ck(t.compute_keys.begin(), t.compute_keys.end());
        std::sort(ck.begin(), ck.end(), [](const auto& a, const auto& b) { return a.second.busy > b.second.busy; });
        o += "\n    work on the compute queues (ms per frame; of it while the game's queue worked; starting ms after the frame's start; frames with it):";

        for (size_t i = 0; i < ck.size() && i < 10; ++i) {
            const auto& [k, v] = ck[i];
            const double kn = (std::max)((double)v.n, 1.0);
            o += std::format(" {} {:.3f} ({:.3f}, at {:.2f}, {}/{}) |", k, v.busy / n, v.overlap / n, v.at / kn, v.n, t.frames);
        }
    }

    if (t.cpu_frames > 0) {
        o += std::format("\n    the engine's submits from the thread that presents {:.2f} per frame", (double)t.rt_engine_calls / (double)t.cpu_frames);
        o += std::format(" ({} frames with other than 15)", t.frames_other_count);

        if (t.loads_a + t.loads_b + t.loads_ref + t.loads_skipped > 0) {
            o += std::format(" | test load: {} before the wait after its {}th, {} before the wait after its {}th, {} after its {}st, {} left out (its slot still busy)", t.loads_a,
                GAP_AFTER_A, t.loads_b, GAP_AFTER_B, t.loads_ref, REF_AFTER, t.loads_skipped);
        }
    }

    if (detail && !m_last_calls.empty()) {
        const auto ms_of = [&](int64_t cpu) { return (double)(cpu - m_last_start_cpu) * 1000.0 / (double)m_qpc_freq; };
        const int g = queue_index(m_game_queue);
        const auto r = submitmath::analyze(as_math(m_last_calls, m_last_start_cpu, g), ms_of(m_last_end_cpu));

        const auto line = [&](const Call& c, const std::string& key, double shift) {
            const char* qname = c.kind == 3   ? ((int)c.queue == g ? "wait-g" : "wait-o")
                                : c.kind == 2 ? "copy"
                                : c.kind == 1 ? "compute"
                                              : ((int)c.queue == g ? "game" : "direct");
            std::string l = std::format("\n      {:7s} {:44s} thread {:3} sent {:6.2f}", qname, key, m_threads.contains(c.thread) ? m_threads.at(c.thread) : 0u,
                ms_of(c.cpu));

            if (c.kind < 2) {
                l += std::format(" lists {:3}", c.lists);
            }

            if (c.timed) {
                l += std::format(" GPU {:6.2f} .. {:6.2f} ({:.3f})", c.gpu_start + shift, c.gpu_end + shift, c.gpu_end - c.gpu_start);
            }

            return l;
        };

        // the end of the frame before it (its last 12 calls), on this frame's clock: what the queue still did when this frame was sent
        if (!m_prev_calls.empty() && m_prev_number + 1 == m_last_number) {
            const double shift = (double)(m_prev_start_cpu - m_last_start_cpu) * 1000.0 / (double)m_qpc_freq;
            const auto rp = submitmath::analyze(as_math(m_prev_calls, m_prev_start_cpu, g), (double)(m_prev_end_cpu - m_prev_start_cpu) * 1000.0 / (double)m_qpc_freq);
            const size_t from = m_prev_calls.size() > 12 ? m_prev_calls.size() - 12 : 0;
            o += std::format("\n    [SUBMIT TIMELINE] the end of frame {} (on the clock of the frame after it: ms after that frame's start):", m_prev_number);

            for (size_t i = from; i < m_prev_calls.size(); ++i) {
                o += line(m_prev_calls[i], i < rp.call_keys.size() ? rp.call_keys[i] : std::string{}, shift);
            }
        }

        o += std::format("\n    [SUBMIT TIMELINE] frame {} (ms after the end of the frame before it, i.e. after its Present; this frame's end at {:.2f}; "
                         "the game's queue from first to last work {:.2f}):",
            m_last_number, ms_of(m_last_end_cpu), m_last_span);

        for (size_t i = 0; i < m_last_calls.size(); ++i) {
            o += line(m_last_calls[i], i < r.call_keys.size() ? r.call_keys[i] : std::string{}, 0.0);
        }
    }

    t = Totals{};
    return o;
}
} // namespace d3d12
#endif // !REVR_RELEASE
