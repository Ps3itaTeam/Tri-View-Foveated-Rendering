#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>

#include <spdlog/spdlog.h>

#include "GpuTimer.hpp"
#include "SubmitTimeline.hpp"

namespace d3d12 {

bool GpuTimer::enable(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::scoped_lock _{m_mutex};

    if (device == nullptr || queue == nullptr) {
        return false;
    }

    if (m_queue != queue) {
        // new queue (first use or after a device reset): rebuild everything
        m_heap.Reset();
        m_queue = queue;
    }

    if (!create(device)) {
        m_failed = true;
        m_enabled = false;
        REVR_LOG(spdlog::error("[GpuTimer] could not create the timestamp resources"));
        return false;
    }

    m_failed = false;
    m_sum = 0.0;
    m_min = 1e9;
    m_max = 0.0;
    m_count = 0;
    m_enabled = true;
    return true;
}

void GpuTimer::disable() {
    std::scoped_lock _{m_mutex};
    m_enabled = false;
    m_begun = false;
}

bool GpuTimer::create(ID3D12Device* device) {
    if (m_heap != nullptr) {
        return true;
    }

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = SLOTS * 2;

    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_heap)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)SLOTS * 16;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)))) {
        m_heap.Reset();
        return false;
    }

    for (auto& a : m_alloc) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) {
            m_heap.Reset();
            return false;
        }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(), nullptr, IID_PPV_ARGS(&m_list)))) {
        m_heap.Reset();
        return false;
    }

    m_list->SetName(L"RE4VR GpuTimer");
    m_list->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        m_heap.Reset();
        return false;
    }

    if (FAILED(m_queue->GetTimestampFrequency(&m_freq)) || m_freq == 0) {
        m_heap.Reset();
        return false;
    }

    m_fence_value = 0;
    m_slot_fence.fill(0);
    m_pending.fill(false);
    m_next = 0;
    m_begun = false;
    return true;
}

void GpuTimer::poll() {
    // m_mutex held by the caller
    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
        if (!m_pending[slot] || m_fence->GetCompletedValue() < m_slot_fence[slot]) {
            continue;
        }

        const D3D12_RANGE read{(SIZE_T)slot * 16, (SIZE_T)slot * 16 + 16};
        void* mapped = nullptr;

        if (SUCCEEDED(m_readback->Map(0, &read, &mapped)) && mapped != nullptr) {
            const auto* t = (const uint64_t*)((const uint8_t*)mapped + (size_t)slot * 16);
            const uint64_t t0 = t[0];
            const uint64_t t1 = t[1];
            const D3D12_RANGE none{0, 0};
            m_readback->Unmap(0, &none);

            if (t1 > t0) {
                const double ms = (double)(t1 - t0) * 1000.0 / (double)m_freq;

                if (ms < 1000.0) {
                    m_sum += ms;
                    m_min = (std::min)(m_min, ms);
                    m_max = (std::max)(m_max, ms);
                    ++m_count;
                }
            }
        }

        m_pending[slot] = false;
    }
}

void GpuTimer::begin() {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_heap == nullptr || m_begun) {
        return;
    }

    poll();

    const uint32_t slot = m_next % SLOTS;

    if (m_pending[slot]) {
        return; // the GPU has not finished this slot yet
    }

    auto& alloc = m_alloc[slot * 2];

    if (FAILED(alloc->Reset()) || FAILED(m_list->Reset(alloc.Get(), nullptr))) {
        m_failed = true;
        return;
    }

    m_list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    m_list->Close();

    ID3D12CommandList* lists[] = {m_list.Get()};
    SubmitTimeline::Tag tag{"the mod's GPU timer"}; // [SUBMIT TIMELINE 06.11.2026]
    m_queue->ExecuteCommandLists(1, lists);

    m_current = slot;
    m_begun = true;
}

void GpuTimer::end() {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_heap == nullptr || !m_begun) {
        return;
    }

    const uint32_t slot = m_current;
    auto& alloc = m_alloc[slot * 2 + 1];

    if (FAILED(alloc->Reset()) || FAILED(m_list->Reset(alloc.Get(), nullptr))) {
        m_failed = true;
        return;
    }

    m_list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    m_list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, m_readback.Get(), (UINT64)slot * 16);
    m_list->Close();

    ID3D12CommandList* lists[] = {m_list.Get()};
    SubmitTimeline::Tag tag{"the mod's GPU timer"}; // [SUBMIT TIMELINE 06.11.2026]
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence.Get(), ++m_fence_value);

    m_slot_fence[slot] = m_fence_value;
    m_pending[slot] = true;
    m_begun = false;
    ++m_next;
}

GpuTimer::Result GpuTimer::take() {
    std::scoped_lock _{m_mutex};

    Result r{};

    if (m_heap != nullptr && !m_failed) {
        poll();
    }

    if (m_count > 0) {
        r.avg_ms = m_sum / (double)m_count;
        r.min_ms = m_min;
        r.max_ms = m_max;
        r.samples = m_count;
    }

    m_sum = 0.0;
    m_min = 1e9;
    m_max = 0.0;
    m_count = 0;
    return r;
}

void GpuTimer::reset() {
    std::scoped_lock _{m_mutex};

    m_enabled = false;
    m_begun = false;
    m_heap.Reset();
    m_readback.Reset();
    m_list.Reset();
    m_fence.Reset();

    for (auto& a : m_alloc) {
        a.Reset();
    }

    m_pending.fill(false);
    m_queue = nullptr;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the GPU span timer: debug build only (the header has the release's stand-in)
// ============================================================================
// [GPU BREAKDOWN 27.10.2026] GpuSpans
// ============================================================================
GpuSpans& GpuSpans::get() {
    static GpuSpans s{};
    return s;
}

bool GpuSpans::enable(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::scoped_lock _{m_mutex};

    if (device == nullptr || queue == nullptr) {
        return false;
    }

    if (m_queue != queue) {
        m_heap.Reset();
        m_readback.Reset();
        m_mapped = nullptr;
        m_queue = queue;
    }

    if (!create(device)) {
        m_failed = true;
        m_enabled = false;
        REVR_LOG(spdlog::error("[GpuSpans] could not create the timestamp resources"));
        return false;
    }

    m_failed = false;
    m_sum.fill(0.0);
    m_max.fill(0.0);
    m_count.fill(0);
    m_enabled = true;
    return true;
}

void GpuSpans::disable() {
    std::scoped_lock _{m_mutex};
    m_enabled = false;
}

bool GpuSpans::create(ID3D12Device* device) {
    if (m_heap != nullptr && m_readback != nullptr && m_mapped != nullptr) {
        return true;
    }

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = STAMPS * FRAMES;

    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_heap)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)STAMPS * FRAMES * 8;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)))) {
        m_heap.Reset();
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};

    // a readback buffer may stay mapped; the CPU reads slots the GPU finished frames ago
    if (FAILED(m_readback->Map(0, &none, &mapped)) || mapped == nullptr) {
        m_readback.Reset();
        m_heap.Reset();
        return false;
    }

    m_mapped = (const uint64_t*)mapped;

    if (FAILED(m_queue->GetTimestampFrequency(&m_freq)) || m_freq == 0) {
        m_readback->Unmap(0, nullptr);
        m_mapped = nullptr;
        m_readback.Reset();
        m_heap.Reset();
        return false;
    }

    m_slot_frame.fill(~0ull);
    m_written.fill(0);
    m_resolved.fill(0);
    m_mode.fill(MODE_SYNC);
    m_collected.fill(true);
    return true;
}

void GpuSpans::stamp(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t index) {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_heap == nullptr || cmd == nullptr || index >= STAMPS) {
        return;
    }

    // [ENDS ONLY fix119] (see the header; the stamps after the copier's end are other lists' and stay)
    if (m_ends_only && index != PRE_BEGIN && index != COPIER_END && index < POST_BEGIN) {
        return;
    }

    const uint32_t slot = slot_for(frame);
    cmd->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * STAMPS + index);
    m_written[slot] |= 1u << index;
}

uint32_t GpuSpans::slot_for(uint64_t frame) {
    const uint32_t slot = (uint32_t)(frame % FRAMES);

    if (m_slot_frame[slot] != frame) {
        // a new frame takes the slot (its previous frame was read four frames ago)
        m_slot_frame[slot] = frame;
        m_written[slot] = 0;
        m_resolved[slot] = 0;
        m_mode[slot] = MODE_SYNC;
        m_collected[slot] = false;
    }

    return slot;
}

void GpuSpans::set_mode(uint64_t frame, uint32_t mode) {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_heap == nullptr || mode > MODE_ONE_LIST) {
        return;
    }

    m_mode[slot_for(frame)] = mode;
}

void GpuSpans::resolve(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t first, uint32_t count) {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_heap == nullptr || cmd == nullptr || first >= STAMPS || count == 0) {
        return;
    }

    const uint32_t slot = (uint32_t)(frame % FRAMES);

    if (m_slot_frame[slot] != frame) {
        return;
    }

    count = (std::min)(count, STAMPS - first);

    // [GPU BREAKDOWN 29.10.2026] only the stamps written in this frame, in runs of consecutive ones: a query that was never
    // ended must not be resolved (a frame that stops early leaves some out)
    uint32_t i = first;

    while (i < first + count) {
        if ((m_written[slot] & (1u << i)) == 0) {
            ++i;
            continue;
        }

        uint32_t end = i;

        while (end + 1 < first + count && (m_written[slot] & (1u << (end + 1))) != 0) {
            ++end;
        }

        cmd->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * STAMPS + i, end - i + 1, m_readback.Get(), ((UINT64)slot * STAMPS + i) * 8);

        for (uint32_t k = i; k <= end; ++k) {
            m_resolved[slot] |= 1u << k;
        }

        i = end + 1;
    }
}

void GpuSpans::collect(uint64_t frame) {
    std::scoped_lock _{m_mutex};

    if (!m_enabled || m_failed || m_mapped == nullptr) {
        return;
    }

    // frames at least four behind: their lists were waited for by the three-deep command rings before reuse
    for (uint32_t slot = 0; slot < FRAMES; ++slot) {
        const auto f = m_slot_frame[slot];

        if (m_collected[slot] || f == ~0ull || f + 4 > frame) {
            continue;
        }

        const uint32_t ok = m_written[slot] & m_resolved[slot];
        const uint64_t* t = m_mapped + (size_t)slot * STAMPS;

        for (uint32_t i = 0; i < SPAN_COUNT; ++i) {
            const auto& d = SPANS[i];

            if ((ok & (1u << d.from)) == 0 || (ok & (1u << d.to)) == 0 || (d.modes & (1u << m_mode[slot])) == 0) {
                continue;
            }

            if (t[d.to] > t[d.from]) {
                const double ms = (double)(t[d.to] - t[d.from]) * 1000.0 / (double)m_freq;

                // [GPU BREAKDOWN 06.11.2026] at most 100 ms: a span over the gap between two lists also holds a stall of the CPU
                // between their submits (log of 06.11 15:02: one frame of 539 ms in "between" and "wall" at the first switch
                // to the TAA, the interval's averages 12.2 and 2.6 ms, the GPU guard's estimate of the mod with it)
                if (ms < 100.0) {
                    m_sum[i] += ms;
                    m_max[i] = (std::max)(m_max[i], ms);
                    ++m_count[i];
                }
            }
        }

        m_collected[slot] = true;
    }
}

std::array<GpuSpans::Span, GpuSpans::SPAN_COUNT> GpuSpans::take() {
    std::scoped_lock _{m_mutex};
    std::array<Span, SPAN_COUNT> out{};

    for (uint32_t i = 0; i < SPAN_COUNT; ++i) {
        if (m_count[i] > 0) {
            out[i].avg_ms = m_sum[i] / (double)m_count[i];
            out[i].max_ms = m_max[i];
            out[i].samples = m_count[i];
        }

        m_sum[i] = 0.0;
        m_max[i] = 0.0;
        m_count[i] = 0;
    }

    return out;
}

void GpuSpans::reset() {
    std::scoped_lock _{m_mutex};
    m_enabled = false;

    if (m_readback != nullptr && m_mapped != nullptr) {
        m_readback->Unmap(0, nullptr);
    }

    m_mapped = nullptr;
    m_readback.Reset();
    m_heap.Reset();
    m_queue = nullptr;
    m_failed = false;
}

#endif
} // namespace d3d12
