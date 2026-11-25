// UiMotion.hpp -- [UI MOTION fix125 22.11.2026] the interface's own motion for the temporal passes (quad view).
//
// The engine's motion vectors hold the motion of the scene behind the interface. An interface element is a flat thing
// the mod knows the place of in this frame and in the frame before (quadview::GuiAnchor::mat): every point of its plane
// has an exact place in the frame before (quadview::ui_plane_motion, QuadView.hpp). This file is what the GPU passes get
// of it: one block of constants per view and frame (the panels' motion, and for up to MAX elements that stand elsewhere
// their place in the view and their own motion), the shader text that reads it, and the buffer the blocks are kept in.
// A pass reads its block as a buffer of float4 rows through an ordinary descriptor of its table (no root descriptor:
// the passes' root signatures stay a table and root constants).
// Used by d3d12::SuperTAA (the fovea views) and d3d12::UiSteady (the wide view). Header only (no new source file).
#pragma once

#include <cstdint>
#include <cstring>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {

struct UiMotion {
    static constexpr uint32_t MAX = 16;

    // the block: ROWS rows of four floats, as the shaders read them (g_um[row])
    struct Constants {
        float panel[3][4]{};    // rows 0-2: the panels' motion (xyz, quadview::ui_plane_motion); [0][3] = 1: known
        float box[MAX][4]{};    // rows 3-18: an element's place in the view (UV: u0, v0, u1, v1), the nearest first
        float d[MAX * 3][4]{};  // rows 19-66: its motion's rows (xyz); [3 i][3] = 1: known (0: the scene's motion vectors there)
        float count{0.0f};      // row 67: x = elements
        float pad[3]{};
    };

    static constexpr uint32_t ROWS = 3 + MAX + MAX * 3 + 1;

    // a block written for a pass: the buffer and the block's first row in it (buffer null: none)
    struct Block {
        ID3D12Resource* buffer{nullptr};
        uint32_t first{0};

        explicit operator bool() const { return buffer != nullptr; }
    };

    // the shader text: the block and um_motion(uv, out motion) -- UV of the frame before minus UV now; false = the scene's
    // motion vectors are to be used at uv. The text before it has to name the block's register: #define UM_REG t<n>
    static const char* hlsl() {
        return R"(
Buffer<float4> g_um : register(UM_REG); // rows 0-2 the panels' motion (xyz; [0].w = 1: known), 3-18 the elements' places
                                        // (UV: u0, v0, u1, v1; the nearest first), 19-66 their motions' rows (the first
                                        // row's w = 1: known, 0: the scene's motion vectors), 67 x = elements

bool um_motion(float2 uv, out float2 m) {
    float4 r0 = g_um[0];
    float3 r1 = g_um[1].xyz;
    float3 r2 = g_um[2].xyz;
    const uint n = min(uint(g_um[67].x + 0.5), 16u);

    [loop] for (uint i = 0; i < n; ++i) {
        const float4 b = g_um[3 + i];

        [branch] if (uv.x >= b.x && uv.y >= b.y && uv.x <= b.z && uv.y <= b.w) {
            r0 = g_um[19 + i * 3];
            r1 = g_um[20 + i * 3].xyz;
            r2 = g_um[21 + i * 3].xyz;
            break;
        }
    }

    const bool known = r0.w > 0.5;
    const float3 h = float3(uv, 1.0);
    const float e = dot(r2, h);
    m = known ? (float2(dot(r0.xyz, h), dot(r1, h)) - uv * e) / (1.0 + e) : float2(0.0, 0.0);
    return known;
}
)";
    }

    // the descriptor of a block for a pass's table
    static void srv(ID3D12Device* device, const Block& block, D3D12_CPU_DESCRIPTOR_HANDLE at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Buffer.FirstElement = block.first;
        d.Buffer.NumElements = ROWS;
        device->CreateShaderResourceView(block.buffer, &d, at);
    }

    // One block for a pass of this frame (none if the buffer could not be made). The blocks are kept in a ring long
    // enough for the command lists of the last frames (three views a frame). The buffer is an upload buffer: it rests in
    // GENERIC_READ, which a compute pass reads as it is.
    Block write(ID3D12Device* device, const Constants& k) {
        if (m_failed || device == nullptr) {
            return {};
        }

        if (m_buffer == nullptr) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = (UINT64)sizeof(Constants) * SLOTS;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const D3D12_RANGE no_read{0, 0};
            void* mapped = nullptr;

            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_buffer))) ||
                FAILED(m_buffer->Map(0, &no_read, &mapped)) || mapped == nullptr) {
                m_buffer.Reset();
                m_failed = true;
                return {};
            }

            m_buffer->SetName(L"RE4VR interface motion constants");
            m_mapped = (uint8_t*)mapped;
        }

        const uint32_t slot = m_next++ % SLOTS;
        std::memcpy(m_mapped + (size_t)slot * sizeof(Constants), &k, sizeof(k));
        return Block{m_buffer.Get(), slot * ROWS};
    }

    // released (after the command lists using it are done)
    void reset() {
        if (m_buffer != nullptr && m_mapped != nullptr) {
            m_buffer->Unmap(0, nullptr);
        }

        m_buffer.Reset();
        m_mapped = nullptr;
        m_failed = false;
    }

private:
    static constexpr uint32_t SLOTS = 96; // three views a frame, lists a few frames deep

    ComPtr<ID3D12Resource> m_buffer{};
    uint8_t* m_mapped{nullptr};
    uint32_t m_next{0};
    bool m_failed{false};
};

static_assert(sizeof(UiMotion::Constants) == UiMotion::ROWS * 16);

} // namespace d3d12
