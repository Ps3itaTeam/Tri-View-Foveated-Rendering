#pragma once

// [GAP TEST 06.11.2026 night] A test load for the GPU test: how much work fits into the places where the game's queue waits
// for the engine's compute queue inside the fovea views (~0.33 ms each, twice a frame in the timelines of 06.11), against the
// same work where the queue never waits. The work is like the mod's eye passes (every pixel of an eye-sized image, 12
// filtered samples of a render-sized image each) and writes only images of its own: what the headset shows does not change.
// d3d12::SubmitTimeline puts it into the frame while it runs. Small: one eye-sized image (~0.15 ms), large: two (~0.30 ms).

#include <cstdint>
#include <string>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
class GapLoad {
public:
    static constexpr uint32_t SLOTS = 8; // two loads a frame, four frames in flight at most

    // the shader, the images (w x h, two of them) and the lists (for the game's queue: a direct queue)
    bool setup(ID3D12Device* device, uint32_t w, uint32_t h, std::string& why);
    void release();
    bool ready() const { return m_ready; }

    ID3D12CommandList* list(bool large, uint32_t slot) const;
    ID3D12Fence* done_fence() const { return m_done_fence.Get(); } // after each load (value: load number)

    // a slot can take load n when its last load is done
    bool slot_free(uint32_t slot) const;
    void use_slot(uint32_t slot, uint64_t number) { m_slot_number[slot % SLOTS] = number; }

    uint32_t width() const { return m_w; }
    uint32_t height() const { return m_h; }

private:
    bool m_ready{false};
    uint32_t m_w{0};
    uint32_t m_h{0};
    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_src{};
    ComPtr<ID3D12Resource> m_dst{};
    ComPtr<ID3D12Fence> m_done_fence{};
    ComPtr<ID3D12CommandAllocator> m_alloc{};
    ComPtr<ID3D12GraphicsCommandList> m_lists[2][SLOTS]{};
    uint64_t m_slot_number[SLOTS]{};
};
} // namespace d3d12
