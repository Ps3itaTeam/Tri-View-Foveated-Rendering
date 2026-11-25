#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the GPU test's load: debug build only (the header has the release's empty stand-in)
#include <cstring>
#include <format>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "GapLoad.hpp"

#pragma comment(lib, "d3dcompiler")

namespace d3d12 {
namespace {
// every pixel of the images written: 12 filtered samples of the source around it, some arithmetic, one write (like an eye pass)
constexpr char LOAD_SHADER[] = R"(
Texture2D<float4> g_src : register(t0);
RWTexture2DArray<unorm float4> g_dst : register(u0);
SamplerState g_linear : register(s0);
cbuffer Constants : register(b0) {
    float2 g_inv_size;
    uint2 g_size;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_size.x || id.y >= g_size.y) {
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) * g_inv_size;
    float4 sum = 0;

    [unroll]
    for (int i = 0; i < 12; ++i) {
        const float a = (float)i * 0.5236 + (float)id.z * 0.25;
        const float2 o = float2(cos(a), sin(a)) * (0.0015 + 0.0005 * (float)(i & 3));
        const float4 c = g_src.SampleLevel(g_linear, uv + o, 0);
        sum += c * c * (1.0 + 0.1 * (float)i);
    }

    g_dst[id] = sqrt(sum / 18.6);
}
)";

constexpr uint32_t SRC_W = 1240;
constexpr uint32_t SRC_H = 960;
} // namespace

bool GapLoad::setup(ID3D12Device* device, uint32_t w, uint32_t h, std::string& why) {
    if (m_ready && m_w == w && m_h == h) {
        return true;
    }

    release();

    if (device == nullptr || w < 64 || h < 64) {
        why = "no device or image size";
        return false;
    }

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3DCompile(LOAD_SHADER, sizeof(LOAD_SHADER) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader,
            &errors))) {
        why = std::string{"the shader did not compile: "} + (errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        why = "the root signature could not be made";
        release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_root_sig.Get();
    pso.CS.pShaderBytecode = shader->GetBufferPointer();
    pso.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)))) {
        why = "the pipeline state could not be made";
        release();
        return false;
    }

    // the source (render-sized, read only: its contents do not matter) and the two eye-sized images written
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = SRC_W;
    desc.Height = SRC_H;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_src)))) {
        why = "the source image could not be made";
        release();
        return false;
    }

    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 2;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_dst)))) {
        why = "the images written could not be made";
        release();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) {
        why = "the descriptor heap could not be made";
        release();
        return false;
    }

    const auto inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_src.Get(), &srv, cpu);
    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.ArraySize = 2;
    device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uav, cpu);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_done_fence)))) {
        why = "the fence could not be made";
        release();
        return false;
    }

    // the lists, recorded once (the same work every frame): one per slot for each size (one image or both)
    const float consts_f[2]{1.0f / (float)w, 1.0f / (float)h};
    uint32_t consts[4]{};
    memcpy(&consts[0], &consts_f[0], 4);
    memcpy(&consts[1], &consts_f[1], 4);
    consts[2] = w;
    consts[3] = h;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc)))) {
        why = "the command allocator could not be made";
        release();
        return false;
    }

    for (int size = 0; size < 2; ++size) {
        for (uint32_t s = 0; s < SLOTS; ++s) {
            auto& l = m_lists[size][s];

            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), m_pso.Get(), IID_PPV_ARGS(&l)))) {
                why = "a command list could not be made";
                release();
                return false;
            }

            ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
            l->SetDescriptorHeaps(1, heaps);
            l->SetComputeRootSignature(m_root_sig.Get());
            l->SetComputeRootDescriptorTable(0, m_heap->GetGPUDescriptorHandleForHeapStart());
            l->SetComputeRoot32BitConstants(1, 4, consts, 0);
            l->Dispatch((w + 7) / 8, (h + 7) / 8, size == 0 ? 1 : 2);

            if (FAILED(l->Close())) {
                why = "a command list could not be closed";
                release();
                return false;
            }
        }
    }

    for (auto& n : m_slot_number) {
        n = 0;
    }

    m_w = w;
    m_h = h;
    m_ready = true;
    REVR_LOG(spdlog::info("[GapLoad] ready: one or two {}x{} images from a {}x{} one", w, h, SRC_W, SRC_H));
    return true;
}

void GapLoad::release() {
    m_ready = false;

    for (auto& size : m_lists) {
        for (auto& l : size) {
            l.Reset();
        }
    }

    m_alloc.Reset();
    m_done_fence.Reset();
    m_heap.Reset();
    m_dst.Reset();
    m_src.Reset();
    m_pso.Reset();
    m_root_sig.Reset();
    m_w = 0;
    m_h = 0;
}

ID3D12CommandList* GapLoad::list(bool large, uint32_t slot) const {
    return m_ready ? m_lists[large ? 1 : 0][slot % SLOTS].Get() : nullptr;
}

bool GapLoad::slot_free(uint32_t slot) const {
    return m_ready && m_done_fence != nullptr && m_done_fence->GetCompletedValue() >= m_slot_number[slot % SLOTS];
}
} // namespace d3d12
#endif // !REVR_RELEASE
