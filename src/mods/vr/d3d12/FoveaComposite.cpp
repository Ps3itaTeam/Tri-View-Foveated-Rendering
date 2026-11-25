#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "FoveaComposite.hpp"
#include "ShaderFileCache.hpp" // [SHADER FILES fix110]
#include "ShaderCompile.hpp" // [BUILT-IN SHADERS fix145]
#if !defined(REVR_SHADER_GENERATOR)
#include "ShaderTable.inl" // [BUILT-IN SHADERS fix145] made by tests/shaders/build.sh
#endif
#include "UiMotion.hpp" // [UI MOTION fix125]
#include "../QuadView.hpp" // [BUILD STAMP fix107] only for QUADVIEW_BUILD_STAMP

#pragma comment(lib, "d3dcompiler")

QUADVIEW_BUILD_STAMP(FoveaComposite); // [BUILD STAMP fix107]

namespace d3d12 {

namespace {
// [COMPILE CACHE 05.11.2026] The quad view's passes are made again at every re-initialisation (a size, the upscaler method,
// a device reset), and their shaders were compiled again each time: ~4.8 s on the render thread per re-initialisation in the
// log of 05.11.2026 19:22-19:24 (the eye pass alone 3.4 s), the headset without frames meanwhile and SteamVR reporting the
// game as not responding. The compiled code does not depend on the device: it is kept for the session, keyed by the whole
// source text, entry point, target, flags and macros (the pipelines are still made again from it).
// [SHADER FILES fix110 20.11.2026] ... and from one session to the next in files (ShaderFileCache.hpp: 71 s of every start
// of the game went into the eye pass's three texts). The folder is given by set_shader_cache_dir; without one (the test
// program) nothing is read or written.
std::mutex s_files_mutex{};
std::filesystem::path s_files_dir{};
std::atomic<uint32_t> s_files_read{0};
std::atomic<uint32_t> s_files_compiled{0};

#if !defined(REVR_SHADER_GENERATOR)
// [BUILT-IN SHADERS fix145 23.11.2026] the compiled code built into the DLL for a key (ShaderTable.inl: entries sorted by the
// key's two hashes, the ones of its file's name), or null
const shader_table::Entry* builtin_shader(const std::string& key) {
    const uint64_t a = shader_files::fnv64(key.data(), key.size());
    const uint64_t b = shader_files::fnv64(key.data(), key.size(), 0x9E3779B97F4A7C15ull ^ (uint64_t)key.size());
    const auto* first = shader_table::ENTRIES;
    const auto* last = shader_table::ENTRIES + shader_table::COUNT;
    const auto* it = std::lower_bound(first, last, std::make_pair(a, b), [](const shader_table::Entry& e, const std::pair<uint64_t, uint64_t>& k) {
        return e.a != k.first ? e.a < k.first : e.b < k.second;
    });

    if (it == last || it->a != a || it->b != b || it->key_size != key.size() || it->code == nullptr || it->size < 32) {
        return nullptr;
    }

    return it;
}
#endif

std::filesystem::path shader_files_dir() {
    std::scoped_lock _{s_files_mutex};
    return s_files_dir;
}

HRESULT compile_cached(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry, LPCSTR target,
    UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    std::string key{(const char*)src, (size_t)size};
    key += '\0';
    key += entry != nullptr ? entry : "";
    key += '\0';
    key += target != nullptr ? target : "";
    key += '\0' + std::to_string(flags1) + '\0' + std::to_string(flags2);

    for (auto* d = defines; d != nullptr && d->Name != nullptr; ++d) {
        key += '\0';
        key += d->Name;
        key += '=';
        key += d->Definition != nullptr ? d->Definition : "";
    }

    static std::mutex s_mutex{};
    // kept to the end of the process on purpose (no release of the blobs while the game shuts down)
    static auto* s_cache = new std::unordered_map<std::string, ComPtr<ID3DBlob>>{};

    {
        std::scoped_lock _{s_mutex};

        if (const auto it = s_cache->find(key); it != s_cache->end() && code != nullptr) {
            *code = it->second.Get();
            (*code)->AddRef();

            if (errors != nullptr) {
                *errors = nullptr;
            }

            return S_OK;
        }
    }

#if !defined(REVR_SHADER_GENERATOR)
    // [BUILT-IN SHADERS fix145] the code built into the DLL
    if (include == nullptr && code != nullptr) {
        if (const auto* e = builtin_shader(key); e != nullptr) {
            ComPtr<ID3DBlob> blob{};

            if (SUCCEEDED(D3DCreateBlob(e->size, &blob)) && blob != nullptr) {
                std::memcpy(blob->GetBufferPointer(), e->code, e->size);

                {
                    std::scoped_lock _{s_mutex};
                    s_cache->emplace(std::move(key), blob);
                }

                *code = blob.Get();
                (*code)->AddRef();

                if (errors != nullptr) {
                    *errors = nullptr;
                }

                return S_OK;
            }
        }
    }
#endif

    // [SHADER FILES fix110] the code of an earlier session: a whole file of ours that holds a DXBC container of its size
    const auto dir = include == nullptr && code != nullptr ? shader_files_dir() : std::filesystem::path{};
    const auto file = dir.empty() ? std::filesystem::path{} : dir / shader_files::file_name(key);

    if (!file.empty()) {
        std::vector<uint8_t> bytes{};

        if (shader_files::load(file, bytes) && bytes.size() >= 32 && std::memcmp(bytes.data(), "DXBC", 4) == 0) {
            uint32_t total = 0;
            std::memcpy(&total, bytes.data() + 24, 4);
            ComPtr<ID3DBlob> blob{};

            if (total == bytes.size() && SUCCEEDED(D3DCreateBlob(bytes.size(), &blob)) && blob != nullptr) {
                std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
                shader_files::touch(file);
                ++s_files_read;

                {
                    std::scoped_lock _{s_mutex};
                    s_cache->emplace(std::move(key), blob);
                }

                *code = blob.Get();
                (*code)->AddRef();

                if (errors != nullptr) {
                    *errors = nullptr;
                }

                return S_OK;
            }
        }
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    const auto compile_start = std::chrono::steady_clock::now();
#endif
    const HRESULT hr = D3DCompile(src, size, name, defines, include, entry, target, flags1, flags2, code, errors);

    if (SUCCEEDED(hr) && code != nullptr && *code != nullptr && include == nullptr) {
        if (!file.empty()) {
            const bool saved = shader_files::save(file, (*code)->GetBufferPointer(), (*code)->GetBufferSize(), std::to_string(GetCurrentThreadId()));
            ++s_files_compiled;
            REVR_LOG(spdlog::info("[ShaderFiles] {} {} compiled in {:.1f} s{}", name != nullptr ? name : "?", entry != nullptr ? entry : "?",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - compile_start).count(),
                saved ? ", kept in a file" : " (its file could not be written)"));
        }

#if defined(REVR_SHADER_GENERATOR)
        shader_generator_record(key, *code);
#endif

        std::scoped_lock _{s_mutex};
        s_cache->emplace(std::move(key), ComPtr<ID3DBlob>{*code});
    }

    return hr;
}

constexpr char COMPOSITE_SHADER[] = R"(
Texture2D<float4>   g_wide  : register(t0);
Texture2D<float4>   g_fovea : register(t1);
RWTexture2D<float4> g_out   : register(u0);
RWTexture2D<float>  g_gain  : register(u1);   // per eye slot (x = slot * 4): smoothed gain, last measurement, wide, fovea
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    int2  g_rect_pos;    // rectangle origin in the eye image
    uint2 g_rect_size;   // rectangle size
    uint2 g_fovea_size;  // fovea texture size (normally == rectangle size)
    float g_feather;     // blend width in pixels
    uint  g_flags;       // 1 = debug border, 2 = encode SRGB on output, 4 = brightness gain
    uint2 g_wide_size;   // eye image size
    uint  g_slot;        // gain slot (eye)
    float g_rate;        // gain smoothing per measurement
};

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// ---- measure: mean brightness of the fovea image vs. the same area of the periphery (one group) -----------------
// The fovea camera has its own automatic exposure, adapted to the small part of the scene it sees: its image can be
// up to ~2x brighter or darker than the same place in the periphery. One overall gain per eye brings it back; nothing
// of the periphery's image goes into the fovea.
groupshared float s_w[256];
groupshared float s_f[256];

[numthreads(16, 16, 1)]
void measure(uint3 tid : SV_GroupThreadID) {
    const uint idx = tid.y * 16 + tid.x;
    float sw = 0.0;
    float sf = 0.0;

    // 64 x 64 points over the inner 80 % of the rectangle (its border blends into the periphery anyway)
    [unroll] for (uint k = 0; k < 16; ++k) {
        const float2 g = float2(tid.x * 4 + (k & 3), tid.y * 4 + (k >> 2));
        const float2 r = lerp(0.1, 0.9, (g + 0.5) / 64.0);
        const float2 wide_uv = (float2(g_rect_pos) + r * float2(g_rect_size)) / float2(g_wide_size);
        sw += dot(g_wide.SampleLevel(g_lin, wide_uv, 0).rgb, LUMA);
        sf += dot(g_fovea.SampleLevel(g_lin, r, 0).rgb, LUMA);
    }

    s_w[idx] = sw;
    s_f[idx] = sf;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = 128; step > 0; step >>= 1) {
        if (idx < step) {
            s_w[idx] += s_w[idx + step];
            s_f[idx] += s_f[idx + step];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (idx == 0) {
        const float lw = s_w[0] / 4096.0;
        const float lf = s_f[0] / 4096.0;
        const float measured = lf > 1e-4 ? clamp(lw / lf, 0.33, 3.0) : 1.0;
        const uint2 at = uint2(g_slot * 4, 0);
        const float old = g_gain[at];
        // log space: brightening and darkening settle equally fast; no valid history yet -> take the measurement
        const float next = (old > 0.0 && old < 10.0) ? exp(lerp(log(old), log(measured), g_rate)) : measured;
        g_gain[at] = next;
        g_gain[at + uint2(1, 0)] = measured;
        g_gain[at + uint2(2, 0)] = lw;
        g_gain[at + uint2(3, 0)] = lf;
    }
}

// ---- composite ----------------------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_rect_size.x || id.y >= g_rect_size.y) {
        return;
    }

    const int2 p = g_rect_pos + int2(id.xy);

    if (any(p < 0) || any(p >= int2(g_wide_size))) {
        return;
    }

    const float4 wcol = g_wide.Load(int3(p, 0));

    float4 fcol;

    if (g_fovea_size.x == g_rect_size.x && g_fovea_size.y == g_rect_size.y) {
        fcol = g_fovea.Load(int3(int2(id.xy), 0));
    } else {
        const float2 uv = (float2(id.xy) + 0.5) / float2(g_rect_size);
        fcol = g_fovea.SampleLevel(g_lin, uv, 0);
    }

    if ((g_flags & 4) != 0) {
        const float g = g_gain[uint2(g_slot * 4, 0)];
        fcol.rgb *= (g > 0.0 && g < 10.0) ? g : 1.0;
    }

    // distance (pixels) to the rectangle border, per axis
    const float2 d = min(float2(id.xy) + 0.5, float2(g_rect_size) - float2(id.xy) - 0.5);
    const float f = max(g_feather, 1.0);
    const float wx = smoothstep(0.0, 1.0, saturate(d.x / f));
    const float wy = smoothstep(0.0, 1.0, saturate(d.y / f));
    float4 c = lerp(wcol, fcol, wx * wy);

    if ((g_flags & 1) != 0 && min(d.x, d.y) < 4.0) {
        c = float4(1.0, 0.0, 0.0, 1.0);
    }

    if ((g_flags & 2) != 0) {
        c.rgb = to_srgb(c.rgb);
    }

    g_out[uint2(p)] = c;
}
)";

DXGI_FORMAT uav_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:    // [FSR DIRECT 06.11.2026] FSR 4's depth side by side
    case DXGI_FORMAT_R32G32_FLOAT: // ... and its motion vectors
        return f;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool is_srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}
} // namespace

DXGI_FORMAT eye_uav_format(DXGI_FORMAT f) {
    return uav_format_for(f);
}

namespace {
constexpr char WIDE_UPSCALE_SHADER[] = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

cbuffer Params : register(b0) {
    uint2 g_src_size;
    uint2 g_dst_size;
    uint  g_flags;      // 1 = encode SRGB (the source view decoded it)
    uint3 g_pad;
};

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 weights(float t) {
    // Catmull-Rom
    return float4(t * (-0.5 + t * (1.0 - 0.5 * t)),
                  1.0 + t * t * (-2.5 + 1.5 * t),
                  t * (0.5 + t * (2.0 - 1.5 * t)),
                  t * t * (-0.5 + 0.5 * t));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_dst_size.x || id.y >= g_dst_size.y) {
        return;
    }

    const float2 sp = (float2(id.xy) + 0.5) * float2(g_src_size) / float2(g_dst_size) - 0.5;
    const int2 base = int2(floor(sp));
    const float2 f = sp - float2(base);
    const int2 last = int2(g_src_size) - 1;

    const float4 wx = weights(f.x);
    const float4 wy = weights(f.y);

    float4 c = 0.0;
    float4 lo = 1e30;
    float4 hi = -1e30;

    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            const int2 p = clamp(base + int2(x - 1, y - 1), int2(0, 0), last);
            const float4 s = g_src.Load(int3(p, 0));
            c += s * (wx[x] * wy[y]);

            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                lo = min(lo, s);
                hi = max(hi, s);
            }
        }
    }

    c = clamp(c, lo, hi); // no ringing / halos

    if ((g_flags & 1) != 0) {
        c.rgb = to_srgb(c.rgb);
    }

    g_dst[id.xy] = float4(c.rgb, 1.0);
}
)";

// a compute read needs NON_PIXEL_SHADER_RESOURCE; a resource resting in a read state keeps its bits too
D3D12_RESOURCE_STATES compute_read_state(D3D12_RESOURCE_STATES resting) {
    if (resting == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || resting == D3D12_RESOURCE_STATE_COMMON) {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    return (D3D12_RESOURCE_STATES)(resting | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// typed SRV format for a depth(-stencil) texture: its depth plane as a float / unorm
DXGI_FORMAT depth_srv_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    // [STABLE DEPTH fix81] StableDepth's image as the reprojection's depth (its first channel)
    case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT; // ([OUTLINES fix84] four values now)
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// typed SRV format for a (possibly typeless) colour texture
DXGI_FORMAT srv_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}
} // namespace

DXGI_FORMAT engine_copy_uav_format(DXGI_FORMAT f) {
    const auto typed = srv_format_for(f);

    if (is_srgb(typed)) {
        return DXGI_FORMAT_UNKNOWN;
    }

    return uav_format_for(typed);
}

bool WideUpscale::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(WIDE_UPSCALE_SHADER, sizeof(WIDE_UPSCALE_SHADER) - 1, "WideUpscale", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[WideUpscale] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[WideUpscale] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[WideUpscale] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[WideUpscale] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[WideUpscale] ready"));
    return true;
}

bool WideUpscale::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, ID3D12Resource* dst,
    int encode_srgb) {
    if (device == nullptr || cmd == nullptr || src == nullptr || dst == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto sdesc = src->GetDesc();
    const auto ddesc = dst->GetDesc();
    const auto srv_format = srv_format_for(sdesc.Format);
    const auto uav_format = uav_format_for(ddesc.Format);

    if (sdesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || uav_format == DXGI_FORMAT_UNKNOWN || is_srgb(ddesc.Format)) {
        m_last_error = "unsupported format";
        return false;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = srv_format;
    device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = uav_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav{cpu.ptr + m_descriptor_size};
    device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu_uav);

    const auto read_state = compute_read_state(src_state);

    barrier(cmd, src, src_state, read_state);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    uint32_t constants[8]{};
    constants[0] = (uint32_t)sdesc.Width;
    constants[1] = sdesc.Height;
    constants[2] = (uint32_t)ddesc.Width;
    constants[3] = ddesc.Height;
    constants[4] = encode_srgb < 0 ? (is_srgb(srv_format) ? 1u : 0u) : (encode_srgb != 0 ? 1u : 0u);
    cmd->SetComputeRoot32BitConstants(1, 8, constants, 0);

    cmd->Dispatch(((uint32_t)ddesc.Width + 7) / 8, (ddesc.Height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = dst;
    cmd->ResourceBarrier(1, &uav_done);

    barrier(cmd, src, read_state, src_state);

    m_last_error = "";
    return true;
}

void WideUpscale::reset() {
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

namespace {
constexpr char LIGHT_TAA_SHADER[] = R"(
Texture2D<float4>   g_color  : register(t0);
Texture2D<float4>   g_motion : register(t1);
Texture2D<float4>   g_depth  : register(t2);
Texture2D<float4>   g_hist   : register(t3);
RWTexture2D<float4> g_out    : register(u0);
SamplerState        g_lin    : register(s0);
Texture2D<float4>   g_aux     : register(t4); // [CALM fix100 20.11.2026] the calm variant's second history, see calm_resolve
RWTexture2D<float4> g_aux_out : register(u1);

cbuffer Params : register(b0) {
    uint2  g_size;
    float2 g_mv_scale;   // current UV + mv * scale = previous UV
    float  g_feedback;   // history weight
    uint   g_flags;      // 1 = reset, 2 = dilate with depth, 4 = reverse Z, 8 = rebuild the frame at the pixel centre,
                         // 16 = blend weighted by 1 / (1 + luma) (bright sparks flicker less),
                         // 64 = the calm variant (calm_resolve; g_aux, g_aux_out),
                         // 128 = ... its statistics start now (the history is another pass's image of the frame before)
    float2 g_jitter;     // this frame's jitter in pixels (as the upscaler gets it): the sample of pixel q shows the point at
                         // q + 0.5 - jitter (the engine's projection has w = -z, the offset goes into its [2][0] / [2][1];
                         // same rule as FSR)
};

// [IMAGE QUALITY 15.11.2026] the depth and the motion of the wide view at another size than its colour (RE7, the video tape:
// the game camera drawn at 2/3 of the size, its depth and motion 746x576 under a 1120x864 colour): read at the same place
// of the image instead of the same pixel numbers
int2 wide_at_size(int2 q, uint w, uint h) {
    if (w == g_size.x && h == g_size.y) {
        return q;
    }

    return min(int2((float2(q) + 0.5) * float2(w, h) / float2(g_size)), int2(w, h) - 1);
}

float4 wide_depth_at(int2 q) {
    uint w, h;
    g_depth.GetDimensions(w, h);
    return g_depth.Load(int3(wide_at_size(q, w, h), 0));
}

float4 wide_motion_at(int2 q) {
    uint w, h;
    g_motion.GetDimensions(w, h);
    return g_motion.Load(int3(wide_at_size(q, w, h), 0));
}

float3 to_ycocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 from_ycocg(float3 c) {
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

// history with a 5-tap Catmull-Rom built from bilinear taps (keeps the accumulated image sharp)
float3 sample_history(float2 uv) {
    const float2 size = float2(g_size);
    const float2 pos = uv * size;
    const float2 c = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 o12 = w2 / w12;

    const float2 t0 = (c - 1.0) / size;
    const float2 t3 = (c + 2.0) / size;
    const float2 t12 = (c + o12) / size;

    float3 r = 0.0;
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    r += g_hist.SampleLevel(g_lin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);

    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

float3 clip_to_box(float3 lo, float3 hi, float3 h) {
    const float3 centre = 0.5 * (hi + lo);
    const float3 extent = 0.5 * (hi - lo) + 1e-4;
    const float3 v = h - centre;
    const float3 a = abs(v / extent);
    const float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : h;
}
)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(
// [CALM fix100 20.11.2026] The calm variant (g_flags 64): the light TAA with a protection of what one frame's samples
// cannot show, as the supersampling TAA has it, in one pass. A second history (g_aux) holds, along the motion, the mean
// of max(e, 0)^2 and of max(-e, 0)^2, e = the pixel's own sample minus the mean of its 3 x 3 (luma), and the depth.
//  - the history is kept within mean +- 1.25 sigma of this frame's 3 x 3 as before, and the luma's limit is wider by
//    the scatter the pixel's own samples have shown TOWARDS that side (a line thinner than a pixel is hit one frame in
//    four: the frames that miss it no longer take it out of the history; a pixel beside an edge, whose own samples never
//    cross it, keeps its limit, so the edge stays as steady as without the protection);
//  - where it is protected the history weighs more (1 - feedback divided by up to CALM_MEM) and the frame's sample
//    counts no farther from the history than CALM_WINS deviations (a spark);
//  - released (and the statistics started again) where the place held something nearer a frame ago (depth), and where
//    the history is farther outside the limits than CALM_OVER; a change of the whole 3 x 3 against the history beyond
//    what its spread and the scatter explain (a light, a shadow) is followed at once.
// Reference model and the numbers: tests/supertaa/periphery/calm.py (fix100).
#define CALM_RATE 0.03125
#define CALM_T0 0.01
#define CALM_T1 0.05
#define CALM_RELAX 1.0
#define CALM_WINS 2.5
#define CALM_WINS_FLOOR 0.02
#define CALM_MEM 3.0
#define CALM_OVER 0.08
#define CALM_DEPTH 0.10
#define CALM_FOLLOW 2.0
#define CALM_FOLLOW_B0 0.015
#define CALM_FOLLOW_SHORT 0.5

float calm_smooth(float x) {
    const float t = saturate(x);
    return t * t * (3.0 - 2.0 * t);
}

// mean, sigma: of the 3 x 3 (YCoCg); cur_y: the frame rebuilt at the pixel centre; own_y: the pixel's own sample (luma);
// own_d: its depth; nearest_d: the nearest depth of the 3 x 3; mv: the motion of that nearest pixel
void calm_resolve(int2 p, float3 mean, float3 sigma, float3 cur_y, float own_y, float own_d, float nearest_d, float2 mv) {
    const float2 uv = (float2(p) + 0.5) / float2(g_size);
    const float2 prev = uv + mv * g_mv_scale;
    const float e = own_y - mean.x;
    float3 result = cur_y;
    float up2 = 0.0;
    float dn2 = 0.0;

    if ((g_flags & 1) == 0 && all(prev >= 0.0) && all(prev <= 1.0)) {
        // [GUARD fix101 20.11.2026] (128: started from the supersampling TAA's image, see LightTAA::Params::calm_fresh: no
        // statistics and no depth of the frame before yet)
        const bool fresh = (g_flags & 128) != 0;
        const float4 st = fresh ? float4(0.0, 0.0, 0.0, 0.0) : g_aux.SampleLevel(g_lin, prev, 0);
        bool released = false;

        // (inverted depth only, as the mod has it: the farthest of the four texels the history is read from was nearer
        // than everything around this pixel is now, by more than CALM_DEPTH of the distance)
        if ((g_flags & 6) == 6 && !fresh) {
            const float4 pd = g_aux.GatherBlue(g_lin, prev);
            released = min(min(pd.x, pd.y), min(pd.z, pd.w)) * (1.0 - CALM_DEPTH) > nearest_d;
        }

        const float sd_up = sqrt(max(st.r, 0.0));
        const float sd_dn = sqrt(max(st.g, 0.0));
        const float sde = max(sd_up, sd_dn);
        float protect = released ? 0.0 : calm_smooth((sde - CALM_T0) / (CALM_T1 - CALM_T0));
        const float3 h = to_ycocg(sample_history(prev));
        const float conf = calm_smooth(abs(mean.x - h.x) / (CALM_FOLLOW * (sigma.x / 3.0 + sde) + CALM_FOLLOW_B0) - 1.0);
        protect *= 1.0 - conf;
        const float3 base = 1.25 * sigma;

        if (abs(h.x - mean.x) - (base.x + CALM_RELAX * sde * protect) > CALM_OVER) {
            released = true;
            protect = 0.0;
        }

        const float rl = CALM_RELAX * sde * protect;
        const float3 lo = mean - base - float3(CALM_RELAX * sd_dn * protect, 0.5 * rl, 0.5 * rl);
        const float3 hi = mean + base + float3(CALM_RELAX * sd_up * protect, 0.5 * rl, 0.5 * rl);
        const float3 hc = clip_to_box(lo, hi, h);
        const float3 dd = cur_y - hc;
        const float3 cl = hc + dd * min(1.0, max(CALM_WINS * sde, CALM_WINS_FLOOR) / max(abs(dd.x), 1e-6));
        const float3 cb = cur_y + (cl - cur_y) * protect;
        const float pixels = length(mv * g_mv_scale * float2(g_size));
        float fb = g_feedback * lerp(1.0, 0.75, saturate(pixels / 24.0));
        fb = 1.0 - (1.0 - fb) / (1.0 + (CALM_MEM - 1.0) * protect);
        fb = fb + (min(fb, CALM_FOLLOW_SHORT) - fb) * conf;

        if ((g_flags & 16) != 0) {
            const float wc = (1.0 - fb) / (1.0 + max(cb.x, 0.0));
            const float wh = fb / (1.0 + max(hc.x, 0.0));
            result = (cb * wc + hc * wh) / max(wc + wh, 1e-6);
        } else {
            result = cb + (hc - cb) * fb;
        }

        if (!released) {
            const float ep = max(e, 0.0);
            const float en = max(-e, 0.0);
            const float dn_prev = max(st.g, 0.0);
            up2 = st.r + CALM_RATE * (ep * ep - st.r);
            dn2 = dn_prev + CALM_RATE * (en * en - dn_prev);
        }
    }

    g_out[uint2(p)] = float4(max(from_ycocg(result), 0.0), 1.0);
    g_aux_out[uint2(p)] = float4(up2, dn2, own_d, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_size.x || id.y >= g_size.y) {
        return;
    }

    const int2 p = int2(id.xy);
    const int2 last = int2(g_size) - 1;
    const bool dilate = (g_flags & 2) != 0;
    const bool reverse_z = (g_flags & 4) != 0;

    const float3 cur = g_color.Load(int3(p, 0)).rgb;
    const bool rebuild = (g_flags & 8) != 0;
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 rec = 0.0;
    float rec_w = 0.0;
    int2 nearest = p;
    float nearest_d = reverse_z ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            const float3 c = to_ycocg(g_color.Load(int3(q, 0)).rgb);
            m1 += c;
            m2 += c * c;

            // the frame at this pixel's centre from the jittered samples around it (Gaussian close to Blackman-Harris): the
            // jitter does not wobble the result, and the history gets the same point every frame
            if (rebuild) {
                const float2 d = float2(x, y) - g_jitter;
                const float w = exp(-2.29 * dot(d, d));
                rec += c * w;
                rec_w += w;
            }

            if (dilate) {
                const float d = wide_depth_at(q).r;

                if (reverse_z ? d > nearest_d : d < nearest_d) {
                    nearest_d = d;
                    nearest = q;
                }
            }
        }
    }

    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 lo = mean - 1.25 * sigma;
    const float3 hi = mean + 1.25 * sigma;

    const float2 mv = wide_motion_at(nearest).xy;
    const float2 uv = (float2(p) + 0.5) / float2(g_size);
    const float2 prev = uv + mv * g_mv_scale;

    const float3 cur_y = rebuild && rec_w > 0.0 ? rec / rec_w : to_ycocg(cur);

    if ((g_flags & 64) != 0) { // [CALM fix100 20.11.2026]
        calm_resolve(p, mean, sigma, cur_y, to_ycocg(cur).x, dilate ? wide_depth_at(p).r : 0.0, nearest_d, mv);
        return;
    }
    float3 result = from_ycocg(cur_y);

    if ((g_flags & 1) == 0 && all(prev >= 0.0) && all(prev <= 1.0)) {
        const float3 h = clip_to_box(lo, hi, to_ycocg(sample_history(prev)));
        // fast motion: lean more on the current frame (less ghosting where the history is least reliable)
        const float pixels = length(mv * g_mv_scale * float2(g_size));
        const float feedback = g_feedback * lerp(1.0, 0.75, saturate(pixels / 24.0));

        if ((g_flags & 16) != 0) {
            const float wc = (1.0 - feedback) / (1.0 + max(cur_y.x, 0.0));
            const float wh = feedback / (1.0 + max(h.x, 0.0));
            result = from_ycocg((cur_y * wc + h * wh) / max(wc + wh, 1e-6));
        } else {
            result = from_ycocg(lerp(cur_y, h, feedback));
        }
    }

    g_out[uint2(p)] = float4(max(result, 0.0), 1.0);
}

// [GPU 29.10.2026 round 3] The same pass for 16 x 16 pixels per group: the 18 x 18 colours around them (in YCoCg) and
// depths are read once into shared memory, the nine reconstruction weights (the same for every pixel: they depend on
// the jitter only) once per group. Every value is computed as above, in the same order.
#ifndef TILE
#define TILE 16 // [GPU 31.10.2026] pixels per group and axis (one per thread); the C++ also makes 32
#endif
#define HALO (TILE + 2)
// [GPU 01.11.2026] the reads of both rounds of the halo issued before either is stored: one wait instead of two (the
// trace of 31.10 night: 18 % of the pass's time waited on the first round's reads). HALO_SERIAL = the loop before (test
// variant); also used where two rounds do not cover the halo.
#if !defined(HALO_SERIAL) && (HALO * HALO > 2 * TILE * TILE)
#define HALO_SERIAL 1
#endif
groupshared float3 s_lc[HALO * HALO];
groupshared float  s_ld[HALO * HALO];
groupshared float  s_lw[9];

[numthreads(TILE, TILE, 1)]
void main_tiled(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 last = int2(g_size) - 1;
    const int2 o = int2(gid.xy) * TILE - 1;
    const bool dilate = (g_flags & 2) != 0;
    const bool reverse_z = (g_flags & 4) != 0;
    const bool rebuild = (g_flags & 8) != 0;

#ifdef HALO_SERIAL
    for (uint e = gi; e < HALO * HALO; e += TILE * TILE) {
        const int2 q = clamp(o + int2(int(e % HALO), int(e / HALO)), int2(0, 0), last);
        s_lc[e] = to_ycocg(g_color.Load(int3(q, 0)).rgb);
        s_ld[e] = dilate ? wide_depth_at(q).r : 0.0;
    }
#else
    {
        const uint e0 = gi;
        const uint e1 = gi + TILE * TILE;
        const uint e1c = min(e1, HALO * HALO - 1u);
        const int2 q0 = clamp(o + int2(int(e0 % HALO), int(e0 / HALO)), int2(0, 0), last);
        const int2 q1 = clamp(o + int2(int(e1c % HALO), int(e1c / HALO)), int2(0, 0), last);
        const float3 c0 = g_color.Load(int3(q0, 0)).rgb;
        const float3 c1 = g_color.Load(int3(q1, 0)).rgb;
        const float z0 = dilate ? wide_depth_at(q0).r : 0.0;
        const float z1 = dilate ? wide_depth_at(q1).r : 0.0;
        s_lc[e0] = to_ycocg(c0);
        s_ld[e0] = z0;

        if (e1 < HALO * HALO) {
            s_lc[e1] = to_ycocg(c1);
            s_ld[e1] = z1;
        }
    }
#endif

    if (gi < 9) {
        const float2 d = float2(int(gi % 3) - 1, int(gi / 3) - 1) - g_jitter;
        s_lw[gi] = exp(-2.29 * dot(d, d));
    }

    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(gid.xy) * TILE + int2(tid.xy);

    if (any(p > last)) {
        return;
    }

    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 rec = 0.0;
    float rec_w = 0.0;
    int2 nearest = p;
    float nearest_d = reverse_z ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const uint t = uint((int(tid.y) + 1 + y) * HALO + (int(tid.x) + 1 + x));
            const float3 c = s_lc[t];
            m1 += c;
            m2 += c * c;

            if (rebuild) {
                const float w = s_lw[(y + 1) * 3 + (x + 1)];
                rec += c * w;
                rec_w += w;
            }

            if (dilate) {
                const float d = s_ld[t];

                if (reverse_z ? d > nearest_d : d < nearest_d) {
                    nearest_d = d;
                    nearest = clamp(p + int2(x, y), int2(0, 0), last);
                }
            }
        }
    }

    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 lo = mean - 1.25 * sigma;
    const float3 hi = mean + 1.25 * sigma;

    const float2 mv = wide_motion_at(nearest).xy;
    const float2 uv = (float2(p) + 0.5) / float2(g_size);
    const float2 prev = uv + mv * g_mv_scale;

    const float3 cur_y = rebuild && rec_w > 0.0 ? rec / rec_w : s_lc[uint((int(tid.y) + 1) * HALO + (int(tid.x) + 1))];

    if ((g_flags & 64) != 0) { // [CALM fix100 20.11.2026]
        const uint centre = uint((int(tid.y) + 1) * HALO + (int(tid.x) + 1));
        calm_resolve(p, mean, sigma, cur_y, s_lc[centre].x, s_ld[centre], nearest_d, mv);
        return;
    }
    float3 result = from_ycocg(cur_y);

    if ((g_flags & 1) == 0 && all(prev >= 0.0) && all(prev <= 1.0)) {
        const float3 h = clip_to_box(lo, hi, to_ycocg(sample_history(prev)));
        const float pixels = length(mv * g_mv_scale * float2(g_size));
        const float feedback = g_feedback * lerp(1.0, 0.75, saturate(pixels / 24.0));

        if ((g_flags & 16) != 0) {
            const float wc = (1.0 - feedback) / (1.0 + max(cur_y.x, 0.0));
            const float wh = feedback / (1.0 + max(h.x, 0.0));
            result = from_ycocg((cur_y * wc + h * wh) / max(wc + wh, 1e-6));
        } else {
            result = from_ycocg(lerp(cur_y, h, feedback));
        }
    }

    g_out[uint2(p)] = float4(max(result, 0.0), 1.0);
}
)";
} // namespace

bool LightTAA::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(LIGHT_TAA_SHADER, sizeof(LIGHT_TAA_SHADER) - 1, "LightTAA", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[LightTAA] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    // [GPU 29.10.2026 round 3] the tiled pass (optional: the one above otherwise)
    ComPtr<ID3DBlob> tiled{};

    if (FAILED(compile_cached(LIGHT_TAA_SHADER, sizeof(LIGHT_TAA_SHADER) - 1, "LightTAA", nullptr, nullptr, "main_tiled", "cs_5_0", 0, 0, &tiled, &errors))) {
        REVR_LOG(spdlog::error("[LightTAA] tiled pass not compiled: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        tiled.Reset();
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5; // colour, motion, depth, history, [CALM fix100] the calm variant's second history
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; // out, [CALM fix100] the second history's
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[LightTAA] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[LightTAA] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    if (tiled != nullptr) {
        pso_desc.CS.pShaderBytecode = tiled->GetBufferPointer();
        pso_desc.CS.BytecodeLength = tiled->GetBufferSize();

        if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_tiled)))) {
            m_pso_tiled.Reset();
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[LightTAA] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_pso_tiled.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[LightTAA] ready{}", m_pso_tiled != nullptr ? " (tiled pass)" : ""));
    return true;
}

// [GPU 31.10.2026] a test variant of the tiled pass (same result), compiled on its first use: k = 0 groups of 32 x 32 pixels,
// k = 1 (01.11.2026) the halo read in two rounds, each awaited
ID3D12PipelineState* LightTAA::alt_pso(ID3D12Device* device, int k) {
    k = k != 0 ? 1 : 0;

    if (m_pso_alt[k] != nullptr || m_alt_failed[k] || m_root_sig == nullptr || device == nullptr) {
        return m_pso_alt[k].Get();
    }

    m_alt_failed[k] = true;
    const D3D_SHADER_MACRO tile32[]{{"TILE", "32"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO serial[]{{"HALO_SERIAL", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> blob{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(LIGHT_TAA_SHADER, sizeof(LIGHT_TAA_SHADER) - 1, "LightTAA", k == 0 ? tile32 : serial, nullptr, "main_tiled", "cs_5_0", 0, 0, &blob,
            &errors))) {
        REVR_LOG(spdlog::error("[LightTAA] test variant {} not compiled: {}", k, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = blob->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_alt[k])))) {
        REVR_LOG(spdlog::error("[LightTAA] test variant {}: CreateComputePipelineState failed", k));
        m_pso_alt[k].Reset();
        return nullptr;
    }

    m_alt_failed[k] = false;
    REVR_LOG(spdlog::info("[LightTAA] test variant {} ready", k));
    return m_pso_alt[k].Get();
}

// [GUARD fix101 20.11.2026] the passes made ahead (the GPU guard's step to the calm light TAA must not compile anything)
bool LightTAA::prepare(ID3D12Device* device) {
    return setup(device);
}

bool LightTAA::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
    D3D12_RESOURCE_STATES in_state, ID3D12Resource* history_in, ID3D12Resource* out, const Params& p) {
    if (device == nullptr || cmd == nullptr || color == nullptr || motion == nullptr || history_in == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    // [CALM fix100 20.11.2026] the calm variant needs its second history (both of the output's size, like the histories)
    const bool calm = p.calm && p.aux_in != nullptr && p.aux_out != nullptr;

    if (p.calm && !calm) {
        m_last_error = "calm: no second history";
        return false;
    }

    const auto cdesc = color->GetDesc();
    const auto mdesc = motion->GetDesc();
    const auto odesc = out->GetDesc();
    const auto color_format = srv_format_for(cdesc.Format);
    const auto motion_format = srv_format_for(mdesc.Format);
    const auto depth_format = depth != nullptr ? depth_srv_format_for(depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
    const bool dilate = p.dilate && depth_format != DXGI_FORMAT_UNKNOWN;

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(0));
    srv.Format = motion_format;
    device->CreateShaderResourceView(motion, &srv, slot(1));

    if (dilate) {
        srv.Format = depth_format;
        device->CreateShaderResourceView(depth, &srv, slot(2));
    } else {
        // the table needs a valid descriptor in every slot: a null SRV reads 0 (dilation is off anyway)
        D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = srv;
        null_srv.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(nullptr, &null_srv, slot(2));
    }

    srv.Format = history_in->GetDesc().Format;
    device->CreateShaderResourceView(history_in, &srv, slot(3));
    srv.Format = calm ? p.aux_in->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(calm ? p.aux_in : nullptr, &srv, slot(4));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = odesc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(5));
    uav.Format = calm ? p.aux_out->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(calm ? p.aux_out : nullptr, nullptr, &uav, slot(6));

    const auto read_in = compute_read_state(in_state);
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    barrier(cmd, color, in_state, read_in);
    barrier(cmd, motion, in_state, read_in);

    if (dilate) {
        barrier(cmd, depth, in_state, read_in);
    }

    barrier(cmd, history_in, UAV, NPSR);

    if (calm) {
        barrier(cmd, p.aux_in, UAV, NPSR);
    }

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    // [GPU 31.10.2026] the 32 x 32 groups when asked for and made (the 16 x 16 ones otherwise); [GPU 01.11.2026] the halo
    // read in two rounds (test variant)
    const bool tiled = p.tiled && m_pso_tiled != nullptr;
    ID3D12PipelineState* pso32 = tiled && p.tile == 32 ? alt_pso(device, 0) : nullptr;
    ID3D12PipelineState* pso_serial = tiled && pso32 == nullptr && p.halo_serial ? alt_pso(device, 1) : nullptr;
    const uint32_t tile = pso32 != nullptr ? 32u : 16u;
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(pso32 != nullptr ? pso32 : pso_serial != nullptr ? pso_serial : tiled ? m_pso_tiled.Get() : m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    uint32_t constants[8]{};
    constants[0] = (uint32_t)odesc.Width;
    constants[1] = odesc.Height;
    std::memcpy(&constants[2], &p.mv_scale_x, 4);
    std::memcpy(&constants[3], &p.mv_scale_y, 4);
    std::memcpy(&constants[4], &p.feedback, 4);
    constants[5] = (p.reset ? 1u : 0u) | (dilate ? 2u : 0u) | (p.reverse_z ? 4u : 0u) | (p.reconstruct ? 8u : 0u) | (p.anti_flicker ? 16u : 0u) |
                   (calm ? 64u : 0u) | (calm && p.calm_fresh ? 128u : 0u);
    const float jx = std::clamp(p.jitter_x, -1.0f, 1.0f);
    const float jy = std::clamp(p.jitter_y, -1.0f, 1.0f);
    std::memcpy(&constants[6], &jx, 4);
    std::memcpy(&constants[7], &jy, 4);
    cmd->SetComputeRoot32BitConstants(1, 8, constants, 0);

    if (tiled || pso32 != nullptr) {
        cmd->Dispatch(((uint32_t)odesc.Width + tile - 1) / tile, (odesc.Height + tile - 1) / tile, 1);
    } else {
        cmd->Dispatch(((uint32_t)odesc.Width + 7) / 8, (odesc.Height + 7) / 8, 1);
    }

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &uav_done);

    if (calm) {
        uav_done.UAV.pResource = p.aux_out;
        cmd->ResourceBarrier(1, &uav_done);
        barrier(cmd, p.aux_in, NPSR, UAV);
    }

    barrier(cmd, history_in, NPSR, UAV);

    if (dilate) {
        barrier(cmd, depth, read_in, in_state);
    }

    barrier(cmd, motion, read_in, in_state);
    barrier(cmd, color, read_in, in_state);

    m_color_linear = is_srgb(color_format);
    m_last_error = "";
    return true;
}

void LightTAA::reset() {
    m_pso.Reset();
    m_pso_tiled.Reset();

    for (int k = 0; k < 2; ++k) {
        m_pso_alt[k].Reset();
        m_alt_failed[k] = false;
    }

    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [FOVEA TAA 28.10.2026] FoveaTAA, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char FOVEA_TAA_SHADER[] = R"(
Texture2D<float4>   g_color    : register(t0);
Texture2D<float4>   g_motion   : register(t1);
Texture2D<float4>   g_depth    : register(t2);
Texture2D<float4>   g_hist     : register(t3); // rgb = colour, a = frames accumulated
Texture2D<float>    g_pdist    : register(t4); // distances (m) of the last frame
RWTexture2D<float4> g_out_hist : register(u0);
RWTexture2D<float>  g_out_dist : register(u1);
SamplerState        g_lin      : register(s0);

cbuffer Params : register(b0) {
    uint2  g_size;
    float2 g_mv_scale;      // current UV + mv * scale = previous UV
    float2 g_jitter;        // pixels: the sample of pixel q shows the point at q + 0.5 - jitter
    float  g_feedback;      // weight of the settled history
    float  g_gamma;         // clip box: mean +- gamma * sigma
    float  g_sigma;         // rebuild of the current frame (pixels, 0 = own sample)
    float  g_sigma_new;     // ... for pixels without history
    float  g_motion_reduce; // history weight lowered by up to this fraction at 24 px of motion per frame
    float  g_reject;        // hidden in the last frame: nearer by more than this fraction
    float4 g_depth_k;       // distance = |(k0 z + k1) / (k2 z + k3)|
    uint   g_flags;         // 1 = reset, 2 = dilate, 4 = reverse Z, 8 = Catmull-Rom history, 16 = luma weighted blend,
                            // 32 = fast convergence, 64 = disocclusion test with the distances
    uint3  g_pad;
};

float3 to_ycocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 from_ycocg(float3 c) {
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

// metres, limited to what the R16_FLOAT distance image holds
float distance_of(float z) {
    const float den = g_depth_k.z * z + g_depth_k.w;
    return min(abs(den) > 1e-12 ? abs((g_depth_k.x * z + g_depth_k.y) / den) : 60000.0, 60000.0);
}

// history with a 5-tap Catmull-Rom built from bilinear taps (keeps the accumulated image sharp)
float3 history_catmull_rom(float2 uv) {
    const float2 size = float2(g_size);
    const float2 pos = uv * size;
    const float2 c = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 o12 = w2 / w12;

    const float2 t0 = (c - 1.0) / size;
    const float2 t3 = (c + 2.0) / size;
    const float2 t12 = (c + o12) / size;

    float3 r = 0.0;
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    r += g_hist.SampleLevel(g_lin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    r += g_hist.SampleLevel(g_lin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);

    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

float3 clip_to_box(float3 lo, float3 hi, float3 h) {
    const float3 centre = 0.5 * (hi + lo);
    const float3 extent = 0.5 * (hi - lo) + 1e-4;
    const float3 v = h - centre;
    const float3 a = abs(v / extent);
    const float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : h;
}

[numthreads(8, 8, 1)]
void accumulate(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_size)) {
        return;
    }

    const int2 p = int2(id.xy);
    const int2 last = int2(g_size) - 1;
    const bool dilate = (g_flags & 2) != 0;
    const bool reverse_z = (g_flags & 4) != 0;
    const bool depth_test = (g_flags & 64) != 0;
    const float k = g_sigma > 0.0 ? 0.5 / (g_sigma * g_sigma) : 0.0;
    const float k_new = g_sigma_new > 0.0 ? 0.5 / (g_sigma_new * g_sigma_new) : 0.0;

    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 rec = 0.0;
    float3 rec_new = 0.0;
    float rec_w = 0.0;
    float rec_new_w = 0.0;
    int2 nearest = p;
    float nearest_z = reverse_z ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            const float3 c = to_ycocg(g_color.Load(int3(q, 0)).rgb);
            m1 += c;
            m2 += c * c;

            // this sample shows the point at its own offset from the pixel centre minus the jitter
            const float2 d = float2(x, y) - g_jitter;
            const float w = g_sigma > 0.0 ? exp(-k * dot(d, d)) : ((x == 0 && y == 0) ? 1.0 : 0.0);
            const float w_new = g_sigma_new > 0.0 ? exp(-k_new * dot(d, d)) : ((x == 0 && y == 0) ? 1.0 : 0.0);
            rec += c * w;
            rec_w += w;
            rec_new += c * w_new;
            rec_new_w += w_new;

            if (dilate) {
                const float z = g_depth.Load(int3(q, 0)).r;

                if (reverse_z ? z > nearest_z : z < nearest_z) {
                    nearest_z = z;
                    nearest = q;
                }
            }
        }
    }

    const float3 own = to_ycocg(g_color.Load(int3(p, 0)).rgb);
    const float3 cur = rec_w > 1e-6 ? rec / rec_w : own;
    const float3 cur_new = rec_new_w > 1e-6 ? rec_new / rec_new_w : own;
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 lo = mean - g_gamma * sigma;
    const float3 hi = mean + g_gamma * sigma;

    const float2 mv = g_motion.Load(int3(nearest, 0)).xy;
    const float2 uv = (float2(p) + 0.5) / float2(g_size);
    const float2 prev = uv + mv * g_mv_scale;
    // the own surface's distance is kept for the next frame; the test uses the surface whose motion fetched the history
    // (the nearest in 3x3 with dilation: next to a moving edge the history comes from where that edge was)
    const float dist = depth_test ? distance_of(g_depth.Load(int3(p, 0)).r) : 0.0;
    const float dist_moved = depth_test && dilate ? distance_of(nearest_z) : dist;

    bool valid = (g_flags & 1) == 0 && all(prev >= 0.0) && all(prev <= 1.0);

    // hidden in the last frame: all four distances around the point it came from are nearer than this surface
    if (valid && depth_test) {
        const float4 pd = g_pdist.Gather(g_lin, prev);
        const float prev_far = max(max(pd.x, pd.y), max(pd.z, pd.w));
        valid = prev_far >= dist_moved * (1.0 - g_reject);
    }

    float3 result = cur_new;
    float age = 1.0;

    if (valid) {
        const float4 hb = g_hist.SampleLevel(g_lin, prev, 0);
        const float3 hist_rgb = (g_flags & 8) != 0 ? history_catmull_rom(prev) : hb.rgb;
        const float3 h = clip_to_box(lo, hi, to_ycocg(hist_rgb));
        const float pixels = length(mv * g_mv_scale * float2(g_size));
        float fb = g_feedback * (1.0 - g_motion_reduce * saturate(pixels / 24.0));

        if ((g_flags & 32) != 0) {
            fb = min(fb, hb.a / (hb.a + 1.0));
        }

        if ((g_flags & 16) != 0) {
            const float wc = (1.0 - fb) / (1.0 + max(cur.x, 0.0));
            const float wh = fb / (1.0 + max(h.x, 0.0));
            result = (cur * wc + h * wh) / max(wc + wh, 1e-6);
        } else {
            result = lerp(cur, h, fb);
        }

        age = min(hb.a + 1.0, 64.0);
    }

    g_out_hist[id.xy] = float4(max(from_ycocg(result), 0.0), age);
    g_out_dist[id.xy] = dist;
}

// [GPU 29.10.2026 round 3] The same pass for 16 x 16 pixels per group: the 18 x 18 colours around them (in YCoCg) and
// depths are read once into shared memory, the reconstruction weights (they depend on the jitter only) once per group.
// Every value is computed as in accumulate, in the same order.
#ifndef TILE
#define TILE 16 // [GPU 31.10.2026] pixels per group and axis (one per thread); the C++ also makes 32
#endif
#define HALO (TILE + 2)
// [GPU 01.11.2026] the reads of both rounds of the halo issued before either is stored: one wait instead of two (the
// trace of 31.10 night: 18 % of the pass's time waited on the first round's reads). HALO_SERIAL = the loop before (test
// variant); also used where two rounds do not cover the halo.
#if !defined(HALO_SERIAL) && (HALO * HALO > 2 * TILE * TILE)
#define HALO_SERIAL 1
#endif
groupshared float3 s_fc[HALO * HALO];
groupshared float  s_fd[HALO * HALO];
groupshared float  s_fw[9];
groupshared float  s_fwn[9];

[numthreads(TILE, TILE, 1)]
void accumulate_tiled(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 last = int2(g_size) - 1;
    const int2 o = int2(gid.xy) * TILE - 1;
    const bool dilate = (g_flags & 2) != 0;
    const bool reverse_z = (g_flags & 4) != 0;
    const bool depth_test = (g_flags & 64) != 0;

#ifdef HALO_SERIAL
    for (uint e = gi; e < HALO * HALO; e += TILE * TILE) {
        const int2 q = clamp(o + int2(int(e % HALO), int(e / HALO)), int2(0, 0), last);
        s_fc[e] = to_ycocg(g_color.Load(int3(q, 0)).rgb);
        s_fd[e] = dilate || depth_test ? g_depth.Load(int3(q, 0)).r : 0.0;
    }
#else
    {
        const uint e0 = gi;
        const uint e1 = gi + TILE * TILE;
        const uint e1c = min(e1, HALO * HALO - 1u);
        const int2 q0 = clamp(o + int2(int(e0 % HALO), int(e0 / HALO)), int2(0, 0), last);
        const int2 q1 = clamp(o + int2(int(e1c % HALO), int(e1c / HALO)), int2(0, 0), last);
        const float3 c0 = g_color.Load(int3(q0, 0)).rgb;
        const float3 c1 = g_color.Load(int3(q1, 0)).rgb;
        const float z0 = dilate || depth_test ? g_depth.Load(int3(q0, 0)).r : 0.0;
        const float z1 = dilate || depth_test ? g_depth.Load(int3(q1, 0)).r : 0.0;
        s_fc[e0] = to_ycocg(c0);
        s_fd[e0] = z0;

        if (e1 < HALO * HALO) {
            s_fc[e1] = to_ycocg(c1);
            s_fd[e1] = z1;
        }
    }
#endif

    if (gi < 9) {
        const int x = int(gi % 3) - 1;
        const int y = int(gi / 3) - 1;
        const float k = g_sigma > 0.0 ? 0.5 / (g_sigma * g_sigma) : 0.0;
        const float k_new = g_sigma_new > 0.0 ? 0.5 / (g_sigma_new * g_sigma_new) : 0.0;
        const float2 d = float2(x, y) - g_jitter;
        s_fw[gi] = g_sigma > 0.0 ? exp(-k * dot(d, d)) : ((x == 0 && y == 0) ? 1.0 : 0.0);
        s_fwn[gi] = g_sigma_new > 0.0 ? exp(-k_new * dot(d, d)) : ((x == 0 && y == 0) ? 1.0 : 0.0);
    }

    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(gid.xy) * TILE + int2(tid.xy);

    if (any(p > last)) {
        return;
    }

    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 rec = 0.0;
    float3 rec_new = 0.0;
    float rec_w = 0.0;
    float rec_new_w = 0.0;
    int2 nearest = p;
    float nearest_z = reverse_z ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const uint t = uint((int(tid.y) + 1 + y) * HALO + (int(tid.x) + 1 + x));
            const float3 c = s_fc[t];
            m1 += c;
            m2 += c * c;

            const float w = s_fw[(y + 1) * 3 + (x + 1)];
            const float w_new = s_fwn[(y + 1) * 3 + (x + 1)];
            rec += c * w;
            rec_w += w;
            rec_new += c * w_new;
            rec_new_w += w_new;

            if (dilate) {
                const float z = s_fd[t];

                if (reverse_z ? z > nearest_z : z < nearest_z) {
                    nearest_z = z;
                    nearest = clamp(p + int2(x, y), int2(0, 0), last);
                }
            }
        }
    }

    const uint centre = uint((int(tid.y) + 1) * HALO + (int(tid.x) + 1));
    const float3 own = s_fc[centre];
    const float3 cur = rec_w > 1e-6 ? rec / rec_w : own;
    const float3 cur_new = rec_new_w > 1e-6 ? rec_new / rec_new_w : own;
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 lo = mean - g_gamma * sigma;
    const float3 hi = mean + g_gamma * sigma;

    const float2 mv = g_motion.Load(int3(nearest, 0)).xy;
    const float2 uv = (float2(p) + 0.5) / float2(g_size);
    const float2 prev = uv + mv * g_mv_scale;
    const float dist = depth_test ? distance_of(s_fd[centre]) : 0.0;
    const float dist_moved = depth_test && dilate ? distance_of(nearest_z) : dist;

    bool valid = (g_flags & 1) == 0 && all(prev >= 0.0) && all(prev <= 1.0);

    if (valid && depth_test) {
        const float4 pd = g_pdist.Gather(g_lin, prev);
        const float prev_far = max(max(pd.x, pd.y), max(pd.z, pd.w));
        valid = prev_far >= dist_moved * (1.0 - g_reject);
    }

    float3 result = cur_new;
    float age = 1.0;

    if (valid) {
        const float4 hb = g_hist.SampleLevel(g_lin, prev, 0);
        const float3 hist_rgb = (g_flags & 8) != 0 ? history_catmull_rom(prev) : hb.rgb;
        const float3 h = clip_to_box(lo, hi, to_ycocg(hist_rgb));
        const float pixels = length(mv * g_mv_scale * float2(g_size));
        float fb = g_feedback * (1.0 - g_motion_reduce * saturate(pixels / 24.0));

        if ((g_flags & 32) != 0) {
            fb = min(fb, hb.a / (hb.a + 1.0));
        }

        if ((g_flags & 16) != 0) {
            const float wc = (1.0 - fb) / (1.0 + max(cur.x, 0.0));
            const float wh = fb / (1.0 + max(h.x, 0.0));
            result = (cur * wc + h * wh) / max(wc + wh, 1e-6);
        } else {
            result = lerp(cur, h, fb);
        }

        age = min(hb.a + 1.0, 64.0);
    }

    g_out_hist[uint2(p)] = float4(max(from_ycocg(result), 0.0), age);
    g_out_dist[uint2(p)] = dist;
}
)";

constexpr char FOVEA_TAA_RESOLVE_SHADER[] = R"(
Texture2D<float4>   g_src : register(t0); // the history just written (render size)
RWTexture2D<float4> g_dst : register(u0); // the fovea image

cbuffer Params : register(b0) {
    uint2 g_src_size;
    uint2 g_dst_size;
    uint  g_flags;   // 1 = encode SRGB (the colour view decoded it), 2 = sharpen
    float g_sharp;   // RCAS amount 0..1
    uint2 g_pad;
};

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 cr_weights(float t) {
    return float4(t * (-0.5 + t * (1.0 - 0.5 * t)),
                  1.0 + t * t * (-2.5 + 1.5 * t),
                  t * (0.5 + t * (2.0 - 1.5 * t)),
                  t * t * (-0.5 + 0.5 * t));
}

// the result at fovea pixel q: 1:1 when the sizes match, else Catmull-Rom clamped to its 2x2 neighbourhood (no ringing)
float3 tap(int2 q) {
    q = clamp(q, int2(0, 0), int2(g_dst_size) - 1);
    const int2 last = int2(g_src_size) - 1;
    float3 c;

    if (all(g_src_size == g_dst_size)) {
        c = g_src.Load(int3(q, 0)).rgb;
    } else {
        const float2 sp = (float2(q) + 0.5) * float2(g_src_size) / float2(g_dst_size) - 0.5;
        const int2 base = int2(floor(sp));
        const float2 f = sp - float2(base);
        const float4 wx = cr_weights(f.x);
        const float4 wy = cr_weights(f.y);
        float3 lo = 1e30;
        float3 hi = -1e30;
        c = 0.0;

        [unroll] for (int y = 0; y < 4; ++y) {
            [unroll] for (int x = 0; x < 4; ++x) {
                const float3 s = g_src.Load(int3(clamp(base + int2(x - 1, y - 1), int2(0, 0), last), 0)).rgb;
                c += s * (wx[x] * wy[y]);

                if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                    lo = min(lo, s);
                    hi = max(hi, s);
                }
            }
        }

        c = clamp(c, lo, hi);
    }

    return (g_flags & 1) != 0 ? to_srgb(c) : c;
}

[numthreads(8, 8, 1)]
void resolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_dst_size)) {
        return;
    }

    const int2 q = int2(id.xy);
    float3 e = tap(q);

    if ((g_flags & 2) != 0) {
        // RCAS: the strongest negative lobe that keeps the cross around the pixel within [0, 1] (no clipping, no halos)
        const float3 b = saturate(tap(q + int2(0, -1)));
        const float3 d = saturate(tap(q + int2(-1, 0)));
        const float3 f = saturate(tap(q + int2(1, 0)));
        const float3 h = saturate(tap(q + int2(0, 1)));
        const float3 ec = saturate(e);
        const float3 mn4 = min(min(b, d), min(f, h));
        const float3 mx4 = max(max(b, d), max(f, h));
        const float3 hit_min = min(mn4, ec) / (4.0 * mx4 + 1e-5);
        const float3 hit_max = (1.0 - max(mx4, ec)) / min(4.0 * mn4 - 4.0, -1e-5);
        const float3 lobe3 = max(-hit_min, hit_max);
        const float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * g_sharp;
        e = (lobe * (b + d + f + h) + ec) / (4.0 * lobe + 1.0);
    }

    g_dst[id.xy] = float4(max(e, 0.0), 1.0);
}

// [GPU 29.10.2026 round 3] The same pass for 16 x 16 pixels per group: tap() (scaling, SRGB) once per pixel of the
// 18 x 18 around them into shared memory instead of five times per pixel; the same values, the same RCAS.
#ifndef TILE
#define TILE 16 // [GPU 31.10.2026] pixels per group and axis (one per thread); the C++ also makes 32
#endif
#define HALO (TILE + 2)
// [GPU 01.11.2026] the reads of both rounds of the halo issued before either is stored: one wait instead of two (the
// trace of 31.10 night: 18 % of the pass's time waited on the first round's reads). HALO_SERIAL = the loop before (test
// variant); also used where two rounds do not cover the halo.
#if !defined(HALO_SERIAL) && (HALO * HALO > 2 * TILE * TILE)
#define HALO_SERIAL 1
#endif
groupshared float3 s_tap[HALO * HALO];

#ifndef HALO_SERIAL
// tap() without the encoding, and the encoding as a branch (the ternary made SRGB on every pixel)
float3 tap_linear(int2 q) {
    q = clamp(q, int2(0, 0), int2(g_dst_size) - 1);
    const int2 last = int2(g_src_size) - 1;
    float3 c;

    if (all(g_src_size == g_dst_size)) {
        c = g_src.Load(int3(q, 0)).rgb;
    } else {
        const float2 sp = (float2(q) + 0.5) * float2(g_src_size) / float2(g_dst_size) - 0.5;
        const int2 base = int2(floor(sp));
        const float2 f = sp - float2(base);
        const float4 wx = cr_weights(f.x);
        const float4 wy = cr_weights(f.y);
        float3 lo = 1e30;
        float3 hi = -1e30;
        c = 0.0;

        [unroll] for (int y = 0; y < 4; ++y) {
            [unroll] for (int x = 0; x < 4; ++x) {
                const float3 s = g_src.Load(int3(clamp(base + int2(x - 1, y - 1), int2(0, 0), last), 0)).rgb;
                c += s * (wx[x] * wy[y]);

                if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                    lo = min(lo, s);
                    hi = max(hi, s);
                }
            }
        }

        c = clamp(c, lo, hi);
    }

    return c;
}

float3 encode(float3 c) {
    [branch] if ((g_flags & 1) != 0) {
        return to_srgb(c);
    }

    return c;
}
#endif

[numthreads(TILE, TILE, 1)]
void resolve_tiled(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 o = int2(gid.xy) * TILE - 1;

#ifdef HALO_SERIAL
    for (uint t = gi; t < HALO * HALO; t += TILE * TILE) {
        s_tap[t] = tap(o + int2(int(t % HALO), int(t / HALO)));
    }
#else
    {
        const uint t0 = gi;
        const uint t1 = gi + TILE * TILE;
        const uint t1c = min(t1, HALO * HALO - 1u);
        const int2 qa = o + int2(int(t0 % HALO), int(t0 / HALO));
        const int2 qb = o + int2(int(t1c % HALO), int(t1c / HALO));
        float3 ca;
        float3 cb;

        [branch] if (all(g_src_size == g_dst_size)) {
            ca = g_src.Load(int3(clamp(qa, int2(0, 0), int2(g_dst_size) - 1), 0)).rgb;
            cb = g_src.Load(int3(clamp(qb, int2(0, 0), int2(g_dst_size) - 1), 0)).rgb;
        } else {
            ca = tap_linear(qa);
            cb = tap_linear(qb);
        }

        s_tap[t0] = encode(ca);

        if (t1 < HALO * HALO) {
            s_tap[t1] = encode(cb);
        }
    }
#endif

    GroupMemoryBarrierWithGroupSync();

    const uint2 id = gid.xy * TILE + tid.xy;

    if (any(id >= g_dst_size)) {
        return;
    }

    const uint c = (tid.y + 1) * HALO + (tid.x + 1);
    float3 e = s_tap[c];

    if ((g_flags & 2) != 0) {
        const float3 b = saturate(s_tap[c - HALO]);
        const float3 d = saturate(s_tap[c - 1]);
        const float3 f = saturate(s_tap[c + 1]);
        const float3 h = saturate(s_tap[c + HALO]);
        const float3 ec = saturate(e);
        const float3 mn4 = min(min(b, d), min(f, h));
        const float3 mx4 = max(max(b, d), max(f, h));
        const float3 hit_min = min(mn4, ec) / (4.0 * mx4 + 1e-5);
        const float3 hit_max = (1.0 - max(mx4, ec)) / min(4.0 * mn4 - 4.0, -1e-5);
        const float3 lobe3 = max(-hit_min, hit_max);
        const float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * g_sharp;
        e = (lobe * (b + d + f + h) + ec) / (4.0 * lobe + 1.0);
    }

    g_dst[id] = float4(max(e, 0.0), 1.0);
}
)";
} // namespace

bool FoveaTAA::setup(ID3D12Device* device) {
    if (m_pso_acc != nullptr && m_pso_res != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> acc{};
    ComPtr<ID3DBlob> res{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(FOVEA_TAA_SHADER, sizeof(FOVEA_TAA_SHADER) - 1, "FoveaTAA", nullptr, nullptr, "accumulate", "cs_5_0", 0, 0, &acc, &errors))) {
        REVR_LOG(spdlog::error("[FoveaTAA] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    if (FAILED(compile_cached(FOVEA_TAA_RESOLVE_SHADER, sizeof(FOVEA_TAA_RESOLVE_SHADER) - 1, "FoveaTAAResolve", nullptr, nullptr, "resolve", "cs_5_0", 0, 0,
            &res, &errors))) {
        REVR_LOG(spdlog::error("[FoveaTAA] resolve shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "resolve shader compile failed";
        return false;
    }

    // [GPU 29.10.2026 round 3] the tiled passes (optional: the ones above otherwise)
    ComPtr<ID3DBlob> acc_tiled{};
    ComPtr<ID3DBlob> res_tiled{};

    if (FAILED(compile_cached(FOVEA_TAA_SHADER, sizeof(FOVEA_TAA_SHADER) - 1, "FoveaTAA", nullptr, nullptr, "accumulate_tiled", "cs_5_0", 0, 0, &acc_tiled,
            &errors)) ||
        FAILED(compile_cached(FOVEA_TAA_RESOLVE_SHADER, sizeof(FOVEA_TAA_RESOLVE_SHADER) - 1, "FoveaTAAResolve", nullptr, nullptr, "resolve_tiled", "cs_5_0", 0,
            0, &res_tiled, &errors))) {
        REVR_LOG(spdlog::error("[FoveaTAA] tiled passes not compiled: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        acc_tiled.Reset();
        res_tiled.Reset();
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // accumulate: 5 SRVs, 2 UAVs, 20 constants
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 5;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 5;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 20;
        params[1].Constants.ShaderRegister = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2;
        rs_desc.pParameters = params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> rs_blob{};

        if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_acc)))) {
            REVR_LOG(spdlog::error("[FoveaTAA] root signature failed"));
            m_last_error = "root signature failed";
            return false;
        }
    }

    // resolve: 1 SRV, 1 UAV, 8 constants
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 8;
        params[1].Constants.ShaderRegister = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2;
        rs_desc.pParameters = params;

        ComPtr<ID3DBlob> rs_blob{};

        if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
            FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_res)))) {
            REVR_LOG(spdlog::error("[FoveaTAA] resolve root signature failed"));
            m_last_error = "root signature failed";
            m_root_acc.Reset();
            return false;
        }
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_acc.Get();
    pso_desc.CS.pShaderBytecode = acc->GetBufferPointer();
    pso_desc.CS.BytecodeLength = acc->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_acc)))) {
        REVR_LOG(spdlog::error("[FoveaTAA] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_acc.Reset();
        m_root_res.Reset();
        return false;
    }

    pso_desc.pRootSignature = m_root_res.Get();
    pso_desc.CS.pShaderBytecode = res->GetBufferPointer();
    pso_desc.CS.BytecodeLength = res->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_res)))) {
        REVR_LOG(spdlog::error("[FoveaTAA] CreateComputePipelineState (resolve) failed"));
        m_last_error = "pipeline state failed";
        m_pso_acc.Reset();
        m_root_acc.Reset();
        m_root_res.Reset();
        return false;
    }

    if (acc_tiled != nullptr && res_tiled != nullptr) {
        pso_desc.pRootSignature = m_root_acc.Get();
        pso_desc.CS.pShaderBytecode = acc_tiled->GetBufferPointer();
        pso_desc.CS.BytecodeLength = acc_tiled->GetBufferSize();
        const bool acc_ok = SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_acc_tiled)));
        pso_desc.pRootSignature = m_root_res.Get();
        pso_desc.CS.pShaderBytecode = res_tiled->GetBufferPointer();
        pso_desc.CS.BytecodeLength = res_tiled->GetBufferSize();

        if (!acc_ok || FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_res_tiled)))) {
            m_pso_acc_tiled.Reset();
            m_pso_res_tiled.Reset();
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[FoveaTAA] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso_acc.Reset();
        m_pso_res.Reset();
        m_pso_acc_tiled.Reset();
        m_pso_res_tiled.Reset();
        m_root_acc.Reset();
        m_root_res.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[FoveaTAA] ready{}", m_pso_acc_tiled != nullptr ? " (tiled passes)" : ""));
    return true;
}

bool FoveaTAA::prepare(ID3D12Device* device, uint32_t w, uint32_t h) {
    if (device == nullptr || w == 0 || h == 0) {
        return false;
    }

    return setup(device) && ensure_histories(device, w, h);
}

// per eye two histories and two distance images at the render size (rest in UNORDERED_ACCESS); recreated on a size change
bool FoveaTAA::ensure_histories(ID3D12Device* device, uint32_t w, uint32_t h) {
    if (m_hist[0][0] != nullptr && m_hist_w == w && m_hist_h == h) {
        return true;
    }

    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 2; ++i) {
            m_hist[e][i].Reset();
            m_dist[e][i].Reset();
        }

        m_last_hist[e] = nullptr;
        m_valid[e] = false;
        m_cur[e] = 0;
    }

    m_hist_w = 0;
    m_hist_h = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = w;
    tex.Height = h;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 2; ++i) {
            tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            const bool hist_ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_hist[e][i])));
            tex.Format = DXGI_FORMAT_R16_FLOAT;
            const bool dist_ok = hist_ok && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr, IID_PPV_ARGS(&m_dist[e][i])));

            if (!dist_ok) {
                REVR_LOG(spdlog::error("[FoveaTAA] could not create the histories ({}x{})", w, h));
                m_last_error = "could not create the histories";

                for (int e2 = 0; e2 < 2; ++e2) {
                    for (int i2 = 0; i2 < 2; ++i2) {
                        m_hist[e2][i2].Reset();
                        m_dist[e2][i2].Reset();
                    }
                }

                return false;
            }

            m_hist[e][i]->SetName(L"RE4VR fovea TAA history");
            m_dist[e][i]->SetName(L"RE4VR fovea TAA distances");
        }
    }

    m_hist_w = w;
    m_hist_h = h;
    REVR_LOG(spdlog::info("[FoveaTAA] histories {}x{}", w, h));
    return true;
}

// [GPU 31.10.2026] the tiled passes with groups of 32 x 32 pixels (same result), compiled on their first use
bool FoveaTAA::alt_psos(ID3D12Device* device, int k) {
    k = k != 0 ? 1 : 0;

    if (m_pso_acc_alt[k] != nullptr && m_pso_res_alt[k] != nullptr) {
        return true;
    }

    if (m_alt_failed[k] || m_root_acc == nullptr || m_root_res == nullptr || device == nullptr) {
        return false;
    }

    m_alt_failed[k] = true;
    const D3D_SHADER_MACRO tile32[]{{"TILE", "32"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO serial[]{{"HALO_SERIAL", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO* defines = k == 0 ? tile32 : serial;
    ComPtr<ID3DBlob> acc{};
    ComPtr<ID3DBlob> res{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(FOVEA_TAA_SHADER, sizeof(FOVEA_TAA_SHADER) - 1, "FoveaTAA", defines, nullptr, "accumulate_tiled", "cs_5_0", 0, 0, &acc, &errors)) ||
        FAILED(compile_cached(FOVEA_TAA_RESOLVE_SHADER, sizeof(FOVEA_TAA_RESOLVE_SHADER) - 1, "FoveaTAAResolve", defines, nullptr, "resolve_tiled", "cs_5_0", 0, 0,
            &res, &errors))) {
        REVR_LOG(spdlog::error("[FoveaTAA] test variant {} not compiled: {}", k, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_acc.Get();
    pso_desc.CS.pShaderBytecode = acc->GetBufferPointer();
    pso_desc.CS.BytecodeLength = acc->GetBufferSize();
    const bool acc_ok = SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_acc_alt[k])));
    pso_desc.pRootSignature = m_root_res.Get();
    pso_desc.CS.pShaderBytecode = res->GetBufferPointer();
    pso_desc.CS.BytecodeLength = res->GetBufferSize();

    if (!acc_ok || FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_res_alt[k])))) {
        REVR_LOG(spdlog::error("[FoveaTAA] test variant {}: CreateComputePipelineState failed", k));
        m_pso_acc_alt[k].Reset();
        m_pso_res_alt[k].Reset();
        return false;
    }

    m_alt_failed[k] = false;
    REVR_LOG(spdlog::info("[FoveaTAA] test variant {} ready", k));
    return true;
}

bool FoveaTAA::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
    D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, const Params& p) {
    if (device == nullptr || cmd == nullptr || color == nullptr || motion == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto cdesc = color->GetDesc();
    const auto odesc = out->GetDesc();
    const auto color_format = srv_format_for(cdesc.Format);
    const auto motion_format = srv_format_for(motion->GetDesc().Format);
    const auto depth_format = depth != nullptr ? depth_srv_format_for(depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
    const auto out_format = uav_format_for(odesc.Format);

    if (out_format == DXGI_FORMAT_UNKNOWN || is_srgb(odesc.Format)) {
        m_last_error = "unsupported fovea image format";
        return false;
    }

    if (!ensure_histories(device, (uint32_t)cdesc.Width, cdesc.Height)) {
        return false;
    }

    const uint32_t eye = (std::min)(p.eye, 1u);
    const uint32_t cur = m_cur[eye];
    ID3D12Resource* hist_in = m_hist[eye][cur].Get();
    ID3D12Resource* hist_out = m_hist[eye][cur ^ 1u].Get();
    ID3D12Resource* dist_in = m_dist[eye][cur].Get();
    ID3D12Resource* dist_out = m_dist[eye][cur ^ 1u].Get();
    const bool have_depth = depth_format != DXGI_FORMAT_UNKNOWN;
    const bool dilate = p.s.dilate && have_depth;
    const bool depth_test = have_depth && p.depth_k_valid && p.s.depth_reject > 0.0f;

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(0));
    srv.Format = motion_format;
    device->CreateShaderResourceView(motion, &srv, slot(1));

    if (have_depth) {
        srv.Format = depth_format;
        device->CreateShaderResourceView(depth, &srv, slot(2));
    } else {
        // the table needs a valid descriptor in every slot: a null view reads 0 (no dilation, no test then)
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(nullptr, &srv, slot(2));
    }

    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(hist_in, &srv, slot(3));
    srv.Format = DXGI_FORMAT_R16_FLOAT;
    device->CreateShaderResourceView(dist_in, &srv, slot(4));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(hist_out, nullptr, &uav, slot(5));
    uav.Format = DXGI_FORMAT_R16_FLOAT;
    device->CreateUnorderedAccessView(dist_out, nullptr, &uav, slot(6));

    // resolve table
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(hist_out, &srv, slot(DESCRIPTORS_ACCUMULATE));
    uav.Format = out_format;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(DESCRIPTORS_ACCUMULATE + 1));

    const auto read_in = compute_read_state(in_state);
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    barrier(cmd, color, in_state, read_in);
    barrier(cmd, motion, in_state, read_in);

    if (have_depth) {
        barrier(cmd, depth, in_state, read_in);
    }

    barrier(cmd, hist_in, UAV, NPSR);
    barrier(cmd, dist_in, UAV, NPSR);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    // ---- accumulate ----
    const auto& s = p.s;
    const float sigma = std::clamp(s.filter_radius, 0.0f, 1.0f);
    const float softness = std::clamp(s.new_softness, 0.0f, 1.0f);
    const float sigma_new = sigma + ((std::max)(1.0f, sigma) - sigma) * softness; // softness 0 = like the others
    const float feedback = std::clamp(s.feedback, 0.0f, 0.98f);
    const float gamma = std::clamp(s.clip_gamma, 0.25f, 4.0f);
    const float motion_reduce = std::clamp(s.motion_reduce, 0.0f, 1.0f);
    const float reject = std::clamp(s.depth_reject, 0.0f, 0.9f);
    const float jx = std::clamp(p.jitter_x, -1.0f, 1.0f);
    const float jy = std::clamp(p.jitter_y, -1.0f, 1.0f);
    const uint32_t flags = (p.reset || !m_valid[eye] ? 1u : 0u) | (dilate ? 2u : 0u) | (p.reverse_z ? 4u : 0u) | (s.catmull_rom ? 8u : 0u) |
                           (s.luma_weight ? 16u : 0u) | (s.fast_converge ? 32u : 0u) | (depth_test ? 64u : 0u);

    uint32_t k[20]{};
    k[0] = (uint32_t)cdesc.Width;
    k[1] = cdesc.Height;
    std::memcpy(&k[2], &p.mv_scale_x, 4);
    std::memcpy(&k[3], &p.mv_scale_y, 4);
    std::memcpy(&k[4], &jx, 4);
    std::memcpy(&k[5], &jy, 4);
    std::memcpy(&k[6], &feedback, 4);
    std::memcpy(&k[7], &gamma, 4);
    std::memcpy(&k[8], &sigma, 4);
    std::memcpy(&k[9], &sigma_new, 4);
    std::memcpy(&k[10], &motion_reduce, 4);
    std::memcpy(&k[11], &reject, 4);
    std::memcpy(&k[12], p.depth_k, sizeof(p.depth_k));
    k[16] = flags;

    // [GPU 31.10.2026] the 32 x 32 groups when asked for and made (the 16 x 16 ones otherwise)
    const bool tiled32 = p.tiled && p.tile == 32 && alt_psos(device, 0);
    const bool serial = !tiled32 && p.tiled && p.halo_serial && alt_psos(device, 1); // [GPU 01.11.2026] test variant
    const bool tiled = tiled32 || (p.tiled && m_pso_acc_tiled != nullptr && m_pso_res_tiled != nullptr);
    const uint32_t tile = tiled32 ? 32u : 16u;
    cmd->SetComputeRootSignature(m_root_acc.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);
    cmd->SetComputeRoot32BitConstants(1, 20, k, 0);

    if (tiled) {
        cmd->SetPipelineState(tiled32 ? m_pso_acc_alt[0].Get() : serial ? m_pso_acc_alt[1].Get() : m_pso_acc_tiled.Get());
        cmd->Dispatch(((uint32_t)cdesc.Width + tile - 1) / tile, (cdesc.Height + tile - 1) / tile, 1);
    } else {
        cmd->SetPipelineState(m_pso_acc.Get());
        cmd->Dispatch(((uint32_t)cdesc.Width + 7) / 8, (cdesc.Height + 7) / 8, 1);
    }

    D3D12_RESOURCE_BARRIER written[2]{};
    written[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[0].UAV.pResource = hist_out;
    written[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[1].UAV.pResource = dist_out;
    cmd->ResourceBarrier(2, written);

    // ---- resolve into the fovea image ----
    barrier(cmd, hist_out, UAV, NPSR);

    const float sharp = std::clamp(s.sharpen, 0.0f, 1.0f);
    uint32_t r[8]{};
    r[0] = (uint32_t)cdesc.Width;
    r[1] = cdesc.Height;
    r[2] = (uint32_t)odesc.Width;
    r[3] = odesc.Height;
    r[4] = (is_srgb(color_format) ? 1u : 0u) | (sharp > 0.0f ? 2u : 0u);
    std::memcpy(&r[5], &sharp, 4);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_res{gpu.ptr + (UINT64)DESCRIPTORS_ACCUMULATE * m_descriptor_size};
    cmd->SetComputeRootSignature(m_root_res.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu_res);
    cmd->SetComputeRoot32BitConstants(1, 8, r, 0);

    if (tiled && sharp > 0.0f) { // without sharpening the plain pass needs one tap per pixel only
        cmd->SetPipelineState(tiled32 ? m_pso_res_alt[0].Get() : serial ? m_pso_res_alt[1].Get() : m_pso_res_tiled.Get());
        cmd->Dispatch(((uint32_t)odesc.Width + tile - 1) / tile, (odesc.Height + tile - 1) / tile, 1);
    } else {
        cmd->SetPipelineState(m_pso_res.Get());
        cmd->Dispatch(((uint32_t)odesc.Width + 7) / 8, (odesc.Height + 7) / 8, 1);
    }

    D3D12_RESOURCE_BARRIER out_done{};
    out_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    out_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &out_done);

    barrier(cmd, hist_out, NPSR, UAV);
    barrier(cmd, dist_in, NPSR, UAV);
    barrier(cmd, hist_in, NPSR, UAV);

    if (have_depth) {
        barrier(cmd, depth, read_in, in_state);
    }

    barrier(cmd, motion, read_in, in_state);
    barrier(cmd, color, read_in, in_state);

    m_valid[eye] = true;
    m_cur[eye] = cur ^ 1u;
    m_last_hist[eye] = hist_out;
    m_last_error = "";
    return true;
}

void FoveaTAA::invalidate() {
    m_valid[0] = m_valid[1] = false;
}

void FoveaTAA::reset() {
    m_pso_acc.Reset();
    m_pso_res.Reset();
    m_pso_acc_tiled.Reset();
    m_pso_res_tiled.Reset();

    for (int k = 0; k < 2; ++k) {
        m_pso_acc_alt[k].Reset();
        m_pso_res_alt[k].Reset();
        m_alt_failed[k] = false;
    }

    m_root_acc.Reset();
    m_root_res.Reset();
    m_heap.Reset();

    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 2; ++i) {
            m_hist[e][i].Reset();
            m_dist[e][i].Reset();
        }

        m_last_hist[e] = nullptr;
        m_valid[e] = false;
        m_cur[e] = 0;
    }

    m_hist_w = 0;
    m_hist_h = 0;
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

bool FoveaComposite::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true; // cleared at the end on success: never retry a broken setup every frame

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(COMPOSITE_SHADER, sizeof(COMPOSITE_SHADER) - 1, "FoveaComposite", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[FoveaComposite] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    ComPtr<ID3DBlob> measure_shader{};

    if (FAILED(compile_cached(COMPOSITE_SHADER, sizeof(COMPOSITE_SHADER) - 1, "FoveaComposite", nullptr, nullptr, "measure", "cs_5_0", 0, 0, &measure_shader,
            &errors))) {
        REVR_LOG(spdlog::error("[FoveaComposite] measure shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "measure shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; // t0 wide, t1 fovea
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; // u0 target, u1 brightness gain
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 12;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[FoveaComposite] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[FoveaComposite] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    pso_desc.CS.pShaderBytecode = measure_shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = measure_shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_measure_pso)))) {
        REVR_LOG(spdlog::error("[FoveaComposite] CreateComputePipelineState (measure) failed"));
        m_last_error = "pipeline state failed";
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    // 4 floats per eye slot: gain, last measured ratio, wide and fovea brightness
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = 8;
        tex.Height = 1;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R32_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_gain)))) {
            REVR_LOG(spdlog::error("[FoveaComposite] could not create the brightness gain texture"));
            m_last_error = "gain texture failed";
            m_measure_pso.Reset();
            m_pso.Reset();
            m_root_sig.Reset();
            return false;
        }

        m_gain->SetName(L"RE4VR FoveaComposite brightness gain");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[FoveaComposite] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_gain.Reset();
        m_measure_pso.Reset();
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";

    REVR_LOG(spdlog::info("[FoveaComposite] ready"));
    return true;
}

bool FoveaComposite::ensure_target(ID3D12Device* device, ID3D12Resource* wide) {
    const auto desc = wide->GetDesc();

    if (m_target != nullptr && m_target_w == (uint32_t)desc.Width && m_target_h == desc.Height && m_source_format == desc.Format) {
        return true;
    }

    m_target.Reset();

    const auto uav_format = uav_format_for(desc.Format);

    if (uav_format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
        REVR_LOG(spdlog::error("[FoveaComposite] unsupported eye image format {}", (int)desc.Format));
        m_last_error = "unsupported eye image format";
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = uav_format;

    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) || (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        REVR_LOG(spdlog::error("[FoveaComposite] format {} has no typed UAV store", (int)uav_format));
        m_last_error = "format has no typed UAV store";
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = desc.Width;
    tex.Height = desc.Height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = uav_format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_target)))) {
        REVR_LOG(spdlog::error("[FoveaComposite] could not create the intermediate texture"));
        m_last_error = "intermediate texture failed";
        return false;
    }

    m_target->SetName(L"RE4VR FoveaComposite target");
    m_target_w = (uint32_t)desc.Width;
    m_target_h = desc.Height;
    m_source_format = desc.Format;
    m_uav_format = uav_format;
    return true;
}

bool FoveaComposite::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* wide, ID3D12Resource* fovea, const Params& p) {
    if (device == nullptr || cmd == nullptr || wide == nullptr || fovea == nullptr || p.rect_w == 0 || p.rect_h == 0) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device) || !ensure_target(device, wide)) {
        return false;
    }

    const auto fdesc = fovea->GetDesc();

    // the rectangle is placed inside the eye image (quadview::place_rect); only that part is drawn and copied back
    const int32_t vis_x0 = (std::max)(p.rect_x, 0);
    const int32_t vis_y0 = (std::max)(p.rect_y, 0);
    const int32_t vis_x1 = (std::min)(p.rect_x + (int32_t)p.rect_w, (int32_t)m_target_w);
    const int32_t vis_y1 = (std::min)(p.rect_y + (int32_t)p.rect_h, (int32_t)m_target_h);

    if (vis_x1 - vis_x0 < 8 || vis_y1 - vis_y0 < 8) {
        m_last_error = "rectangle outside the eye image";
        return false;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    srv.Format = m_source_format;
    device->CreateShaderResourceView(wide, &srv, slot(0));

    srv.Format = fdesc.Format;
    device->CreateShaderResourceView(fovea, &srv, slot(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = m_uav_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_target.Get(), nullptr, &uav, slot(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC gain_uav{};
    gain_uav.Format = DXGI_FORMAT_R32_FLOAT;
    gain_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_gain.Get(), nullptr, &gain_uav, slot(3));

    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    barrier(cmd, wide, UAV, SRV);
    barrier(cmd, fovea, UAV, SRV);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    const uint32_t flags = (p.debug_border ? 1u : 0u) | (is_srgb(m_source_format) ? 2u : 0u) | (p.match_brightness ? 4u : 0u);
    const float feather = std::clamp(p.feather_px, 1.0f, (float)(std::min)(p.rect_w, p.rect_h) * 0.5f);
    const float rate = std::clamp(p.gain_rate, 0.01f, 1.0f);

    uint32_t constants[12]{};
    std::memcpy(&constants[0], &p.rect_x, 4);
    std::memcpy(&constants[1], &p.rect_y, 4);
    constants[2] = p.rect_w;
    constants[3] = p.rect_h;
    constants[4] = (uint32_t)fdesc.Width;
    constants[5] = fdesc.Height;
    std::memcpy(&constants[6], &feather, 4);
    constants[7] = flags;
    constants[8] = m_target_w;
    constants[9] = m_target_h;
    constants[10] = (std::min)(p.gain_slot, 1u);
    std::memcpy(&constants[11], &rate, 4);
    cmd->SetComputeRoot32BitConstants(1, 12, constants, 0);

    // measure this frame's fovea against the periphery, then composite with the smoothed gain
    if (p.match_brightness) {
        cmd->SetPipelineState(m_measure_pso.Get());
        cmd->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER gain_done{};
        gain_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        gain_done.UAV.pResource = m_gain.Get();
        cmd->ResourceBarrier(1, &gain_done);
    }

    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((p.rect_w + 7) / 8, (p.rect_h + 7) / 8, 1);

    // copy the blended rectangle back into the eye image (UNORM <-> SRGB is the same format family)
    barrier(cmd, m_target.Get(), UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(cmd, wide, SRV, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = wide;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m_target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_BOX box{};
    box.left = (UINT)vis_x0;
    box.top = (UINT)vis_y0;
    box.front = 0;
    box.right = (UINT)vis_x1;
    box.bottom = (UINT)vis_y1;
    box.back = 1;

    cmd->CopyTextureRegion(&dst, (UINT)vis_x0, (UINT)vis_y0, 0, &src, &box);

    barrier(cmd, m_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UAV);
    barrier(cmd, wide, D3D12_RESOURCE_STATE_COPY_DEST, UAV);
    barrier(cmd, fovea, SRV, UAV);

    m_last_error = "";
    return true;
}

void FoveaComposite::reset() {
    // Called on every quad-view re-init and on device reset (after the copiers were waited for):
    // drop everything, setup() rebuilds it on the next dispatch.
    m_pso.Reset();
    m_measure_pso.Reset();
    m_gain.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
    m_target.Reset();
    m_target_w = 0;
    m_target_h = 0;
    m_source_format = DXGI_FORMAT_UNKNOWN;
    m_uav_format = DXGI_FORMAT_UNKNOWN;
}

// ================================================================================================================
// [MONO PERIPHERY 27.10.2026] MonoPeriphery, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char MONO_PERIPHERY_SHADER[] = R"(
Texture2D<float4>   g_src   : register(t0); // wide view colour (render size)
Texture2D<float4>   g_depth : register(t1); // wide view depth (render size); from the stable depth pass: x = 1 / distance
                                            // handed on, w = its mean over time, held still (flag 2048 reads it)
RWTexture2D<float4> g_out   : register(u0); // this eye's periphery
#ifndef NO_BLOCK_MIN
// [GPU 29.10.2026] the faster search: per row and block of 32 columns the nearest distance in it; the pixels the bounded
// search finds no column for, listed for the second pass, and their count per eye
RWTexture2D<float>        g_bmin  : register(u1);
RWStructuredBuffer<uint>  g_holes : register(u2);
RWByteAddressBuffer       g_count : register(u3);
#endif
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    uint  g_out_w;
    uint  g_out_h;
    uint  g_src_w;
    uint  g_src_h;
    float g_x_scale;     // output pixel centre -> source column of the same ray at infinite distance
    float g_x_offset;
    float g_y_scale;
    float g_y_offset;
    float g_disparity;   // source shift (pixels * metres) of a surface at distance d: shift = g_disparity / d
    float g_dist_min;    // nearest distance searched (m)
    uint  g_max_shift;   // largest shift searched (source pixels)
    uint  g_flags;       // 1 = tint the pixels without a fitting column (debug), 2 = first pass of the faster search,
                         // 4 = its second pass (the listed pixels), 8 = right eye (count and list of the second eye),
                         // 16 = clean edges (a column next to a much nearer one read one column farther from it),
                         // 32 = smooth hole fill (the background's local mean deeper in the gap),
                         // 64 = the gap filled with the background beside it moved over by the gap's width (anchored to
                         //   the background instead of the near thing's edge: it does not slide when the near thing moves),
                         // 128 = that copy read at texel centres without the blur (as sharp as the background around),
                         // 256 = clean edges over two columns (TAA spreads the near thing's colour wider),
                         // 1024 = the gap's width from the near thing's own edge column instead of the nearest surface the
                         //   search passed (smooth along the edge: the copy does not jump from row to row)
                         // 2048 = [OUTLINES fix84] with 2 x 2 pixels per ray and the stable depth's image: each of the four
                         //   pixels on the surface its own ray meets, the outline between two surfaces where the mean of
                         //   1 / distance crosses the middle between them (see soft_inv)
    float g_da;          // distance from the depth value: |(a z + b) / (c z + e)|
    float g_db;
    float g_dc;
    float g_de;
};

// [IMAGE QUALITY 15.11.2026] the wide view's depth at another size than its colour (RE7, the video tape: 746x576 under a
// 1120x864 colour): read at the same place of the image instead of the same pixel numbers
float wide_depth_at(int2 q) {
    uint w, h;
    g_depth.GetDimensions(w, h);

    if (w == g_src_w && h == g_src_h) {
        return g_depth.Load(int3(q, 0)).x;
    }

    return g_depth.Load(int3(min(int2((float2(q) + 0.5) * float2(w, h) / float2(g_src_w, g_src_h)), int2(w, h) - 1), 0)).x;
}

// [NEAR 09.11.2026] Surfaces nearer than the nearest distance searched count as at that distance: their column lies
// within the search and they land with a smaller shift than their own, instead of no column fitting there at all (RE2
// first person, dumps of 09.11.2026: the jacket and the collar at 1-12 cm below the head camera, a whole region of the eye's
// periphery without a fitting column, filled with background next to it).
float distance_of(float z) {
    float den = g_dc * z + g_de;
    den = abs(den) < 1e-9 ? (den < 0.0 ? -1e-9 : 1e-9) : den;
    return max(abs((g_da * z + g_db) / den), max(g_dist_min, 1e-3));
}

// [HOLES 02.11.2026] (flag 16) A background column right next to a near thing's edge holds part of that thing's colour
// (anti-aliasing, TAA) but the background's depth: in the eye it lands on the far side of the gap, apart from the near
// thing, and draws a faint outline of it there (the dotted line along the gap). Its colour is read one column farther from
// the near thing instead. Only the columns left and right count: the eyes are apart along x.
float clean_edge(float sx_f, int sx, int yi, float dist) {
    if ((g_flags & 16) == 0) {
        return sx_f;
    }

    const int wl = int(g_src_w) - 1;
    const bool nl = distance_of(wide_depth_at(int2(max(sx - 1, 0), yi))) < 0.77 * dist;
    const bool nr = distance_of(wide_depth_at(int2(min(sx + 1, wl), yi))) < 0.77 * dist;

    // (flag 256) two columns: a near column two away moves the read one column, one next to it two columns
    if ((g_flags & 256) != 0) {
        const bool nl2 = nl || distance_of(wide_depth_at(int2(max(sx - 2, 0), yi))) < 0.77 * dist;
        const bool nr2 = nr || distance_of(wide_depth_at(int2(min(sx + 2, wl), yi))) < 0.77 * dist;

        if (nl2 && !nr2) {
            return min(sx_f + (nl ? 2.0 : 1.0), float(wl) + 0.5);
        }

        if (nr2 && !nl2) {
            return max(sx_f - (nr ? 2.0 : 1.0), 0.5);
        }

        return sx_f;
    }

    if (nl && !nr) {
        return min(sx_f + 1.0, float(wl) + 0.5);
    }

    if (nr && !nl) {
        return max(sx_f - 1.0, 0.5);
    }

    return sx_f;
}

// the colour at source column sx of row yc; with soften > 0 a little blurred (the filled gap, more the deeper into it: less
// of a copied pattern next to the edge)
float4 read_soft(float sx, float yc, float soften) {
    const float2 uv = float2(sx / float(g_src_w), yc / float(g_src_h));
    float4 c = g_src.SampleLevel(g_lin, uv, 0);

    if (soften > 0.0) {
        const float2 px = 1.0 / float2(g_src_w, g_src_h);
        const float r = 0.75 + 1.25 * soften;
        float4 b = c;
        b += g_src.SampleLevel(g_lin, uv + float2(0.0, r) * px, 0);
        b += g_src.SampleLevel(g_lin, uv - float2(0.0, r) * px, 0);
        b += g_src.SampleLevel(g_lin, uv + float2(r, 0.0) * px, 0);
        b += g_src.SampleLevel(g_lin, uv - float2(r, 0.0) * px, 0);
        c = lerp(c, b * 0.2, soften);
    }

    return c;
}

#ifdef SS2
// [PERIPHERY 2X fix80 17.11.2026] The colour image holds 2 x 2 texels per column and row of the wide view (the
// supersampling TAA's image at twice the wide view's size) and the eye's periphery gets 2 x 2 pixels per ray searched.
// The search runs as before, once per ray, over the columns of the wide view's depth (g_src_w x g_src_h is the size of
// that grid; the colour is read by its place in the image, whatever its size). Each of the four pixels then reads the
// colour where its own ray lands on the surface found: the same shift for the four (one surface), their rays a quarter
// of a column and row to each side of the searched one.
//   SS_OFFSET(k): pixel k's ray from the searched one, in columns and rows (k = 0 left top, 1 right top, 2 left bottom,
//   3 right bottom: output pixel 2 * id + (k & 1, k >> 1))
#define SS_OFFSET(k) (float2(float((k) & 1u) - 0.5, float((k) >> 1) - 0.5) * 0.5 * float2(g_x_scale, g_y_scale))

// what the full search does with what it found, for the ray `o` from the searched one
float4 shade_at(float best_sx, float shift_sx, float yc, float soften, float shift_w, float3 smooth_col, float smooth_w, bool hit, float2 o) {
    float4 c = read_soft(best_sx + o.x, yc + o.y, soften);

    if (shift_w > 0.0) {
        c = lerp(c, read_soft(shift_sx + o.x, yc + o.y, (g_flags & 128) != 0 ? 0.0 : soften), shift_w);
    }

    c.rgb = lerp(c.rgb, smooth_col, smooth_w);

    if (!hit && (g_flags & 1) != 0) {
        c.rgb = lerp(c.rgb, float3(1.0, 0.0, 1.0), 0.6);
    }

    return float4(c.rgb, hit ? 1.0 : 0.0);
}

// [OUTLINES fix84 18.11.2026] The outline of a near thing in the eye's periphery stood on the grid of the rays: one search
// per ray (4 x 4 eye pixels), one surface for its four pixels, and that surface the one the ray's column of the depth
// holds: steps of 4 eye pixels along every outline, in other places whenever the head moves a little (RE7, captures and
// the video of 17.11.2026 22:2x: the outlines are what trembles in the periphery, with the stable depth less, not gone).
// The stable depth's image also holds the mean of 1 / distance over time: in a texel an outline crosses it tells how much
// of the texel the near surface covers (the frames' jitter samples the texel all over). Read bilinearly, it crosses the
// middle between the two surfaces where the outline is, to a fraction of a texel, and it moves smoothly with the head.
//   soft_inv(p): the 1 / distance of the surface at the place p (columns, rows) of the wide view. One surface in the four
//   texels around p (apart by less than SOFT_STEP of the nearer): its 1 / distance between the texels. Two: the near one
//   where the mean at p is above the middle, else the far one; `stepped` says that there are two.
#define SOFT_STEP 0.10
#define SOFT_SAME 0.06 // a surface found at p is the searched column's if their 1 / distance differ by less than this part
// [fix85 18.11.2026] `share`: with two surfaces, how much of the place the near one covers (0 .. 1, from the mean; the
// outline is where it crosses 0.5), `near_inv`: the near one's 1 / distance; one surface: 0 and its own.
float soft_inv(float2 p, out bool stepped, out float share, out float near_inv) {
    const float2 t = p - 0.5;
    const float2 fl = floor(t);
    const float2 f = t - fl;
    const int2 i0 = int2(fl);
    const int2 last = int2(int(g_src_w), int(g_src_h)) - 1;
    const float2 a = g_depth.Load(int3(clamp(i0, int2(0, 0), last), 0)).xw;
    const float2 b = g_depth.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0)).xw;
    const float2 c = g_depth.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0)).xw;
    const float2 d = g_depth.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0)).xw;
    const float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    const float top = 1.0 / max(g_dist_min, 1e-3);
    const float4 cs = min(float4(a.x, b.x, c.x, d.x), top);
    float hi = max(max(cs.x, cs.y), max(cs.z, cs.w));
    float lo = min(min(cs.x, cs.y), min(cs.z, cs.w));
    const float plain = dot(w, cs);
    const float m = dot(w, min(float4(a.y, b.y, c.y, d.y), top));
    stepped = hi - lo > SOFT_STEP * hi;

    // one surface in the four texels, and the mean says the same: nothing of another surface in these texels
    share = 0.0;
    near_inv = plain;

    [branch] if (!stepped && abs(m - plain) <= 0.02 * plain) {
        return plain;
    }

    // Which two surfaces meet here is taken from the twelve texels around the four as well: the texel the outline
    // crosses is handed on as the near surface while that covers more than half of it and as the far one after; with the
    // four texels alone the outline jumped by up to half a texel when that changed (outline_test.py, a slow motion).
    [unroll] for (int k = 0; k < 16; ++k) {
        const int2 o = int2(k % 4 - 1, k / 4 - 1);

        if (any(o < 0) || any(o > 1)) {
            const float v = min(g_depth.Load(int3(clamp(i0 + o, int2(0, 0), last), 0)).x, top);
            hi = max(hi, v);
            lo = min(lo, v);
        }
    }

    stepped = hi - lo > SOFT_STEP * hi;

    if (!stepped) {
        return plain;
    }

    const float mid = 0.5 * (hi + lo);
    const bool near = m >= mid;
    const float4 nearm = step(mid, cs); // 1 where the texel is handed on as the near surface
    share = saturate((m - lo) / (hi - lo));
    near_inv = hi;
    const float4 side = near ? nearm : 1.0 - nearm;
    const float sw = dot(w, side);
    return sw > 1e-4 ? dot(w * side, cs) / sw : (near ? hi : lo);
}
#endif

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// The search of the original pass for one output pixel, unchanged (full search from the largest shift, the gap filled
// with the background mirrored at the edge, rays past the border mirrored back).
#ifdef SS2
float4 reproject_full(uint2 id, out float4 ss1, out float4 ss2, out float4 ss3) { // (SS2: the pixels 1 .. 3 of the ray)
#else
float4 reproject_full(uint2 id) {
#endif
    const float xc = (float(id.x) + 0.5) * g_x_scale + g_x_offset; // source column of this ray at infinity
    const float yc = (float(id.y) + 0.5) * g_y_scale + g_y_offset;
    const int   yi = clamp(int(yc), 0, int(g_src_h) - 1);
    const int   x0 = int(floor(xc));
    const int   sgn = g_disparity >= 0.0 ? 1 : -1;
    const int   n = min(int(g_max_shift), int(ceil(abs(g_disparity) / max(g_dist_min, 1e-3))));

    float best_err = 1e9;
    float best_sx = xc;
    float d_min = 1e9;
    bool  hit = false;

    // from the largest shift (nearest possible surface) down to none: the first column whose own distance puts it on
    // this pixel is the nearest surface the eye sees here
#ifdef SERIAL_SEARCH
    [loop] for (int i = n; i >= 0; --i) {
        const int sx = x0 + sgn * i;

        if (sx < 0 || sx >= int(g_src_w)) {
            continue;
        }

        const float dist = distance_of(wide_depth_at(int2(sx, yi)));
        const float landed = (float(sx) + 0.5) - g_disparity / dist; // the ray (at infinity) this column lands on
        const float err = abs(landed - xc);

        if (err <= 0.501) {
            best_sx = clean_edge(xc + g_disparity / dist, sx, yi, dist); // exact position of this ray on that surface
            hit = true;
            break;
        }

        d_min = min(d_min, dist);

        if (err < best_err) {
            best_err = err;
            best_sx = float(sx) + 0.5;
        }
    }
#else
    // [GPU 29.10.2026 round 3] the depths of four columns are read at once, then the columns are tested one after the
    // other as before (same order, same stop at the first fitting column)
    const int wlast = int(g_src_w) - 1;

    [loop] for (int i4 = n; i4 >= 0 && !hit; i4 -= 4) {
        float zq[4];

        [unroll] for (int u = 0; u < 4; ++u) {
            zq[u] = wide_depth_at(int2(clamp(x0 + sgn * (i4 - u), 0, wlast), yi));
        }

        [unroll] for (int v = 0; v < 4; ++v) {
            const int i = i4 - v;
            const int sx = x0 + sgn * i;

            if (hit || i < 0 || sx < 0 || sx >= int(g_src_w)) {
                continue;
            }

            const float dist = distance_of(zq[v]);
            const float landed = (float(sx) + 0.5) - g_disparity / dist; // the ray (at infinity) this column lands on
            const float err = abs(landed - xc);

            if (err <= 0.501) {
                best_sx = clean_edge(xc + g_disparity / dist, sx, yi, dist); // exact position of this ray on that surface
                hit = true;
                continue;
            }

            d_min = min(d_min, dist);

            if (err < best_err) {
                best_err = err;
                best_sx = float(sx) + 0.5;
            }
        }
    }
#endif

    // no column fits: the eye looks past the edge of a near thing at background the wide view could not see (it lies
    // behind that thing there). The background right next to the gap, on the far side of the near thing's edge, is the
    // starting point: of the columns clearly farther than the nearest one around, the one that lands closest. The near
    // thing itself is kept only for the last pixel at its own edge (anti-aliased border); taking it for the whole gap
    // repeated hands and weapon as light streaks next to them (test 27.10.2026, gap up to ~35 px).
    // [GAP FILL 28.10.2026] Repeating that one background column across the gap drew horizontal streaks along every near
    // edge, a band that read as a second, coloured outline of hands, arm and weapon (test 27.10.2026 evening, both
    // eyes, the band on the outer side of each eye). The gap now continues the background mirrored at its edge (the
    // texture next to the gap, read away from the near thing), a little blurred the deeper into the gap.
    float soften = 0.0;
    bool filled = false;
    float3 smooth_col = 0.0; // [HOLES 02.11.2026] flag 32: the background's local mean and its share
    float smooth_w = 0.0;
    float shift_sx = 0.0;    // [HOLES 02.11.2026] flag 64: the column of the moved copy and its share
    float shift_w = 0.0;

    if (!hit && n > 0) {
        float bg_err = 1e9;
        float bg_sx = best_sx;
        float bg_land = xc;

#ifdef SERIAL_SEARCH
        [loop] for (int i = n; i >= 0; --i) {
            const int sx = x0 + sgn * i;

            if (sx < 0 || sx >= int(g_src_w)) {
                continue;
            }

            const float dist = distance_of(wide_depth_at(int2(sx, yi)));

            if (dist < 1.3 * d_min) {
                continue;
            }

            const float landed = (float(sx) + 0.5) - g_disparity / dist;
            const float err = abs(landed - xc);

            if (err < bg_err) {
                bg_err = err;
                bg_sx = float(sx) + 0.5;
                bg_land = landed;
            }
        }
#else
        [loop] for (int i4 = n; i4 >= 0; i4 -= 4) {
            float zq[4];

            [unroll] for (int u = 0; u < 4; ++u) {
                zq[u] = wide_depth_at(int2(clamp(x0 + sgn * (i4 - u), 0, wlast), yi));
            }

            [unroll] for (int v = 0; v < 4; ++v) {
                const int i = i4 - v;
                const int sx = x0 + sgn * i;

                if (i < 0 || sx < 0 || sx >= int(g_src_w)) {
                    continue;
                }

                const float dist = distance_of(zq[v]);

                if (dist < 1.3 * d_min) {
                    continue;
                }

                const float landed = (float(sx) + 0.5) - g_disparity / dist;
                const float err = abs(landed - xc);

                if (err < bg_err) {
                    bg_err = err;
                    bg_sx = float(sx) + 0.5;
                    bg_land = landed;
                }
            }
        }
#endif

        if (bg_err < 1e8 && best_err > 1.0) {
            // how far into the gap this pixel lies, read on the background's own side of the edge
            const float into = xc - bg_land;
            const float w = float(g_src_w);
            // [HOLES 02.11.2026] (flag 16) mirrored about a point a column and a half inside the background: the edge
            // column itself holds part of the near thing's colour (the dotted line along the gap)
            const float edge_m = (g_flags & 256) != 0 ? 2.5 : 1.5; // columns of the background's edge left out
            float sx_m = bg_sx - into + ((g_flags & 16) != 0 ? (into >= 0.0 ? -edge_m : edge_m) : 0.0);
            // past the image border: mirrored back in (the column actually read)
            float sx_r = sx_m < 0.0 ? -sx_m : (sx_m > w ? 2.0 * w - sx_m : sx_m);

            // stay on background (a near thing on that side as well: closer to the edge); tested where it is read
            [unroll] for (int k = 0; k < 3; ++k) {
                const int si = clamp(int(floor(sx_r)), 0, int(g_src_w) - 1);

                if (distance_of(wide_depth_at(int2(si, yi))) >= 1.3 * d_min) {
                    break;
                }

                sx_m = lerp(bg_sx, sx_m, 0.5);
                sx_r = sx_m < 0.0 ? -sx_m : (sx_m > w ? 2.0 * w - sx_m : sx_m);
            }

            best_sx = clamp(sx_r, 0.5, w - 0.5);
            soften = saturate(abs(into) / 6.0);
            filled = true;

            // [HOLES 02.11.2026] (flag 64) The mirror's axis is the near thing's edge: when the arm moves, the mirrored
            // copy moves with it while the real background stays still. Here the background strip beside the gap, as wide
            // as the gap, is moved over into it: pixel x reads x + (constant) as long as the gap keeps its width, so the
            // copy stays with the background. At the gap's own edge the mirror (it continues the background without a
            // seam), over three columns over to the moved copy.
            if ((g_flags & 64) != 0) {
                const float sgn_in = into >= 0.0 ? 1.0 : -1.0;
                const float d_bg = distance_of(wide_depth_at(int2(clamp(int(bg_sx), 0, int(g_src_w) - 1), yi)));
                float d_near = d_min;

                // [HOLES 02.11.2026 night] (flag 1024) the nearest surface the search passed can be another near thing in
                // the same row (the hand, the upper arm): the copy's shift then jumps from row to row (short horizontal
                // dashes in the dumps of 21:55). The gap's own edge: the first column from the background's edge towards
                // the gap clearly nearer than the background (at most 4 columns on).
                if ((g_flags & 1024) != 0) {
                    [unroll] for (int k3 = 1; k3 <= 4; ++k3) {
                        const int si = clamp(int(floor(bg_sx)) + int(sgn_in) * k3, 0, int(g_src_w) - 1);
                        const float dk = distance_of(wide_depth_at(int2(si, yi)));

                        if (dk < 0.77 * d_bg) {
                            d_near = dk;
                            break;
                        }
                    }
                }

                const float gap = abs(g_disparity) * max(1.0 / d_near - 1.0 / d_bg, 0.0);
                float st = bg_sx - sgn_in * (gap + ((g_flags & 16) != 0 ? edge_m : 0.0)) + into;
                st = st < 0.0 ? -st : (st > w ? 2.0 * w - st : st);

                [unroll] for (int k2 = 0; k2 < 3; ++k2) {
                    const int si = clamp(int(floor(st)), 0, int(g_src_w) - 1);

                    if (distance_of(wide_depth_at(int2(si, yi))) >= 1.3 * d_min) {
                        break;
                    }

                    st = lerp(bg_sx - sgn_in * 1.5, st, 0.5);
                }

                // (flag 128) at the texel's centre: the copy keeps the background's own sharpness (between two texels
                // the bilinear read averages them)
#ifdef SS2
                // [fix85 18.11.2026] not with the image of twice the size: it has the detail between the columns'
                // middles, and on whole columns the copy jumped a column (4 eye pixels) at a time, each row at another
                // moment (RE7 whole-chain dumps of 18.11.2026: of the pixels that change by more than 3 % from a frame
                // to the next, those in the gaps 445 -> 292)
                shift_sx = clamp(st, 0.5, w - 0.5);
#else
                shift_sx = clamp((g_flags & 128) != 0 ? floor(st) + 0.5 : st, 0.5, w - 0.5);
#endif
                shift_w = smoothstep(0.5, 3.0, abs(into));
            }

            // [HOLES 02.11.2026] (flag 32) the gap is background no view saw. A copy of the background next to it
            // (mirrored) has detail that does not continue the real one and slides with the near thing when it moves
            // (the "arm-shaped silhouette"). Deeper into the gap the fill goes over to the background's local mean:
            // a 4 x 4 grid of reads over a square of the background beside the gap (past the edge column and its
            // neighbour), only background depths (as far as the background the search took, not the near thing).
            if ((g_flags & 32) != 0) {
                const float side = into >= 0.0 ? -1.0 : 1.0; // from the edge column away from the gap
                const float d_bg = distance_of(wide_depth_at(int2(clamp(int(bg_sx), 0, int(g_src_w) - 1), yi)));
                const float gap = abs(g_disparity) * max(1.0 / d_min - 1.0 / d_bg, 0.0);
                const float r = clamp(0.5 * gap, 2.0, 8.0);
                const float cx = bg_sx + side * (2.0 + r);
                float3 sum = 0.0;
                float sw = 0.0;

                [unroll] for (int gy = 0; gy < 4; ++gy) {
                    [unroll] for (int gx = 0; gx < 4; ++gx) {
                        const float2 q = float2(cx + (float(gx) - 1.5) * (2.0 * r / 3.0), yc + (float(gy) - 1.5) * (2.0 * r / 3.0));
                        const int2 qi = clamp(int2(floor(q)), int2(0, 0), int2(g_src_w, g_src_h) - 1);

                        // (read at the texel's corner: the mean of four texels, a smoother mean from the same reads)
                        if (distance_of(wide_depth_at(qi)) >= 1.3 * d_min) {
                            sum += g_src.SampleLevel(g_lin, float2(max(qi, int2(1, 1))) / float2(g_src_w, g_src_h), 0).rgb;
                            sw += 1.0;
                        }
                    }
                }

                if (sw > 0.0) {
                    smooth_col = sum / sw;
                    smooth_w = smoothstep(0.5, 4.0, abs(into));
                }
            }
        }
    }

    // [HOLES 02.11.2026] (flag 16) a pixel at the gap's edge that takes the column landing closest (within a column, no
    // fill): the same clean read as a hit
    if (!hit && !filled && (g_flags & 16) != 0) {
        const int bi = clamp(int(floor(best_sx)), 0, int(g_src_w) - 1);
        best_sx = clean_edge(best_sx, bi, yi, distance_of(wide_depth_at(int2(bi, yi))));
    }

    // [GAP FILL 28.10.2026] the ray of a near surface at the image border needs a column beyond the wide image (the outer
    // edge of each eye): mirrored back into the image instead of the border column repeated (streaks along the edge)
    if (!hit && !filled && (best_sx < 1.0 || best_sx > float(g_src_w) - 1.0)) {
        const int edge = best_sx < 1.0 ? 0 : int(g_src_w) - 1;
        const float need = xc + g_disparity / distance_of(wide_depth_at(int2(edge, yi)));
        const float w = float(g_src_w);

        if (need < 0.0) {
            best_sx = min(-need, w - 1.0);
            soften = saturate(-need / 6.0);
        } else if (need > w) {
            best_sx = max(2.0 * w - need, 1.0);
            soften = saturate((need - w) / 6.0);
        }
    }

#ifdef SS2
    ss1 = shade_at(best_sx, shift_sx, yc, soften, shift_w, smooth_col, smooth_w, hit, SS_OFFSET(1u));
    ss2 = shade_at(best_sx, shift_sx, yc, soften, shift_w, smooth_col, smooth_w, hit, SS_OFFSET(2u));
    ss3 = shade_at(best_sx, shift_sx, yc, soften, shift_w, smooth_col, smooth_w, hit, SS_OFFSET(3u));
    return shade_at(best_sx, shift_sx, yc, soften, shift_w, smooth_col, smooth_w, hit, SS_OFFSET(0u));
#else
    float4 c = read_soft(best_sx, yc, soften);

    if (shift_w > 0.0) {
        c = lerp(c, read_soft(shift_sx, yc, (g_flags & 128) != 0 ? 0.0 : soften), shift_w);
    }

    c.rgb = lerp(c.rgb, smooth_col, smooth_w);

    if (!hit && (g_flags & 1) != 0) {
        c.rgb = lerp(c.rgb, float3(1.0, 0.0, 1.0), 0.6);
    }

    // [HOLE BAND 13.11.2026] alpha 0 = no column of the wide view fits this pixel (a gap beside a nearer surface, filled
    // above): the eye pass lets the fovea cover it in the fovea's blend band (EyeCompose flag 2097152)
    return float4(c.rgb, hit ? 1.0 : 0.0);
#endif
}

// the original pass: every pixel with the full search
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_out_w || id.y >= g_out_h) {
        return;
    }

#ifdef SS2
    float4 s1, s2, s3;
    const float4 s0 = reproject_full(id.xy, s1, s2, s3);
    g_out[id.xy * 2] = s0;
    g_out[id.xy * 2 + uint2(1, 0)] = s1;
    g_out[id.xy * 2 + uint2(0, 1)] = s2;
    g_out[id.xy * 2 + uint2(1, 1)] = s3;
#else
    g_out[id.xy] = reproject_full(id.xy);
#endif
}

)"
// (two literals, joined by the compiler: MSVC takes at most 16 KB in one string literal)
R"(#ifndef NO_BLOCK_MIN
// [GPU 29.10.2026] once per frame, before the eyes: the nearest distance of every block of 32 columns of every row (all
// 32 reads issued at once), and the counts of both eyes' lists set to zero
[numthreads(64, 1, 1)]
void block_min(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0 && id.y == 0) {
        g_count.Store2(0, uint2(0, 0));
    }

    const uint blocks = (g_src_w + 31) / 32;

    if (id.x >= blocks || id.y >= g_src_h) {
        return;
    }

    const int x0 = int(id.x) * 32;
    const int last = int(g_src_w) - 1;
    float m = 1e30;

    [unroll] for (int k = 0; k < 32; ++k) {
        const float d = distance_of(wide_depth_at(int2(min(x0 + k, last), int(id.y))));
        m = min(m, d);
    }

    g_bmin[id.xy] = m;
}

// [GPU 29.10.2026 round 3] The same minima with one thread per column: a group is one block of 32 columns over 8 rows,
// each thread reads one depth, the minimum of the block's row is taken in shared memory (the same values, the minimum
// does not depend on the order). The counts are set to zero by the first thread.
groupshared float s_bm[256];

[numthreads(32, 8, 1)]
void block_min_cols(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gid.x == 0 && gid.y == 0 && gi == 0) {
        g_count.Store2(0, uint2(0, 0));
    }

    const int row = int(gid.y) * 8 + int(tid.y);
    const int col = min(int(gid.x) * 32 + int(tid.x), int(g_src_w) - 1);
    s_bm[gi] = row < int(g_src_h) ? distance_of(wide_depth_at(int2(col, row))) : 1e30;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint s = 16; s > 0; s >>= 1) {
        if (tid.x < s) {
            s_bm[gi] = min(s_bm[gi], s_bm[gi + s]);
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (tid.x == 0 && row < int(g_src_h)) {
        g_bmin[uint2(gid.x, uint(row))] = s_bm[gi];
    }
}

// [GPU 29.10.2026] First pass of the faster search. A column at shift i lands on this pixel's ray only if
// i <= |disparity| / (its distance) + 1.001 (the ray and the column are within half a pixel), so a block of columns whose
// nearest surface is at distance d can only hit with shifts up to |disparity| / d + 1.5: the search from the largest shift
// down skips the shifts each block cannot have. The first column found is the one the full search finds first, and the
// pixel gets exactly what the full search gives it. A pixel without any (the eye sees past the edge of a near thing, or a
// ray past the image border) is listed for the second pass, which runs the full search on the listed pixels only: those
// are few, and in this pass they no longer hold up the pixels around them.
// [GPU 31.10.2026 night] The block minima the group's pixels can use, read once into shared memory as their reach
// (computed as before): the trace of 31.10 showed each wave waiting for a block minimum read from memory at nearly every
// step of the search (35 % of the pass's time). The group's rows and blocks come from its corner pixels (the positions
// grow with x and y), one row and one column more on each side; a group they do not fit reads the texture as before.
// BMIN_TEXTURE = the pass of 31.10 22:51 (every block minimum from the texture).
// [GPU 01.11.2026] test of 01.11: the first version (a few threads reading row after row, each read awaited before the
// next) was slower (0.080 against 0.050 ms). Now every thread of the group (one wave) reads at most two values, both
// read before either is stored: one wait for the whole group.
#ifndef BMIN_TEXTURE
#define RS_ROWS 16  // rows of block minima a group keeps (two per thread: 8 x 8 threads)
#define RS_BLOCKS 8 // blocks per row (8 x 32 = 256 columns: the search range is 96 columns)
groupshared float s_reach[RS_ROWS * RS_BLOCKS];
#endif

[numthreads(8, 8, 1)]
void bounded(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
#ifdef BMIN_TEXTURE
    if (id.x >= g_out_w || id.y >= g_out_h) {
        return;
    }
#else
    // the group's rows and block columns (the same for every thread of the group)
    const int g_n = min(int(g_max_shift), int(ceil(abs(g_disparity) / max(g_dist_min, 1e-3))));
    const int g_sgn = g_disparity >= 0.0 ? 1 : -1;
    const int g_w = int(g_src_w);
    const uint2 c0 = gid.xy * 8;
    const uint2 c1 = min(c0 + 7, uint2(g_out_w, g_out_h) - 1);
    const int xa = int(floor((float(c0.x) + 0.5) * g_x_scale + g_x_offset));
    const int xb = int(floor((float(c1.x) + 0.5) * g_x_scale + g_x_offset));
    const int ya = clamp(int((float(c0.y) + 0.5) * g_y_scale + g_y_offset), 0, int(g_src_h) - 1);
    const int yb = clamp(int((float(c1.y) + 0.5) * g_y_scale + g_y_offset), 0, int(g_src_h) - 1);
    const int sx_lo = clamp(min(min(xa, xb), min(xa, xb) + g_sgn * g_n) - 1, 0, g_w - 1);
    const int sx_hi = clamp(max(max(xa, xb), max(xa, xb) + g_sgn * g_n) + 1, 0, g_w - 1);
    const int y_lo = max(min(ya, yb) - 1, 0);
    const int y_hi = min(max(ya, yb) + 1, int(g_src_h) - 1);
    const int b_lo = sx_lo >> 5;
    const int span = (sx_hi >> 5) - b_lo + 1;
    const int rows = y_hi - y_lo + 1;
    const bool lds = span <= RS_BLOCKS && rows <= RS_ROWS;

    // thread (fb, fr): block b_lo + fb of rows y_lo + fr and y_lo + fr + 8 (places past the group's range read a valid
    // place and are not stored)
    [branch] if (lds) {
        const int fb = int(gi & 7u);
        const int fr = int(gi >> 3);
        const uint bx = uint(min(b_lo + fb, (g_w - 1) >> 5));
        const float m0 = g_bmin[uint2(bx, uint(min(y_lo + fr, y_hi)))];
        const float m1 = g_bmin[uint2(bx, uint(min(y_lo + fr + 8, y_hi)))];

        if (fb < span) {
            if (fr < rows) {
                s_reach[fr * RS_BLOCKS + fb] = abs(g_disparity) / max(m0, 1e-3) + 1.5;
            }

            if (fr + 8 < rows) {
                s_reach[(fr + 8) * RS_BLOCKS + fb] = abs(g_disparity) / max(m1, 1e-3) + 1.5;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= g_out_w || id.y >= g_out_h) {
        return;
    }
#endif

    const float xc = (float(id.x) + 0.5) * g_x_scale + g_x_offset;
    const float yc = (float(id.y) + 0.5) * g_y_scale + g_y_offset;
    const int   yi = clamp(int(yc), 0, int(g_src_h) - 1);
    const int   x0 = int(floor(xc));
    const int   sgn = g_disparity >= 0.0 ? 1 : -1;
    const int   n = min(int(g_max_shift), int(ceil(abs(g_disparity) / max(g_dist_min, 1e-3))));
    const int   w = int(g_src_w);
    const float reach_k = abs(g_disparity);

    // the shifts whose column lies inside the image (the full search skips the others)
    const int i_hi = min(n, sgn > 0 ? w - 1 - x0 : x0);
    const int i_lo = max(0, sgn > 0 ? -x0 : x0 - (w - 1));

)"
// (the pass goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(#ifdef SS2
    // [OUTLINES fix84 18.11.2026] (flag 2048) each of the ray's four pixels on the surface its own ray meets. The columns
    // are searched from the largest shift down as below, in the ray's row and the rows above and below it (an outline
    // along a row), and a column counts when it lands within a column of the ray; for each pixel not yet placed, the
    // surface at the place its own ray meets that column's surface (soft_inv): the column's own -> the pixel is placed
    // there (the first one is the nearest, as below). A pixel past a near surface's outline that nothing else covers
    // (the eye sees past the outline, into the gap the wide view cannot see) gets what the wide view shows beside the
    // outline at that place. A ray with a pixel that got neither goes to the second pass as before.
    uint gw = 0;
    uint gh = 0;
    g_depth.GetDimensions(gw, gh);

    [branch] if ((g_flags & 2048) != 0 && gw == g_src_w && gh == g_src_h) {
        const int yl = int(g_src_h) - 1;
        int rows3[3];
        rows3[0] = yi;
        rows3[1] = max(yi - 1, 0);
        rows3[2] = min(yi + 1, yl);
        uint placed = 0u;
        uint has_beside = 0u;
        float2 at[4];
        float2 fb[4];
        float at_inv[4];
        float fb_inv[4];
        float fb_w[4]; // [fix85] see below: the share of the read beside a nearer outline in a placed pixel

        [unroll] for (uint z0 = 0; z0 < 4; ++z0) {
            at[z0] = float2(xc, yc);
            fb[z0] = float2(xc, yc);
            at_inv[z0] = 1.0;
            fb_inv[z0] = 1.0;
            fb_w[z0] = 0.0;
        }

        int i2 = i_hi;
        int cur_b2 = -1;
        float reach2 = 0.0;

        [loop] while (i2 >= i_lo && placed != 15u) {
            const int sx = x0 + sgn * i2;
            const int b = sx >> 5;

            if (b != cur_b2) {
                cur_b2 = b;
                reach2 = 0.0;

                // (a column lands within a column of the ray: half a column more than below)
                [unroll] for (int r0 = 0; r0 < 3; ++r0) {
#ifdef BMIN_TEXTURE
                    reach2 = max(reach2, reach_k / max(g_bmin[uint2(uint(b), uint(rows3[r0]))], 1e-3) + 2.0);
#else
                    [branch] if (lds) {
                        reach2 = max(reach2, s_reach[(rows3[r0] - y_lo) * RS_BLOCKS + (b - b_lo)] + 0.5);
                    } else {
                        reach2 = max(reach2, reach_k / max(g_bmin[uint2(uint(b), uint(rows3[r0]))], 1e-3) + 2.0);
                    }
#endif
                }
            }

            if (float(i2) > reach2) {
                const int i_edge2 = sgn > 0 ? i2 - (sx - (b << 5)) : i2 - (((b << 5) + 31) - sx);
                const int i_ok2 = int(reach2);
                i2 = i_ok2 >= i_edge2 ? i_ok2 : i_edge2 - 1;
                continue;
            }

            [unroll] for (int r = 0; r < 3; ++r) {
                // [GPU fix95 19.11.2026] (once the four pixels are placed, by the ray's own row as a rule, the rows above
                // and below have nothing left to do: their depth is not read)
                float inv_c = 1.0;

                [branch] if (placed != 15u) {
                    inv_c = 1.0 / distance_of(wide_depth_at(int2(sx, rows3[r])));
                }

                const float s = g_disparity * inv_c;

                [branch] if (placed != 15u && abs((float(sx) + 0.5) - s - xc) <= 1.0) {
                    [unroll] for (uint ss = 0; ss < 4; ++ss) {
                        const uint bit = 1u << ss;

                        if ((placed & bit) == 0u) {
                            const float2 o = SS_OFFSET(ss);
                            bool stepped;
                            float share;
                            float near_inv;
                            const float inv_p = soft_inv(float2(xc + o.x + s, yc + o.y), stepped, share, near_inv);

                            if (abs(inv_p - inv_c) <= SOFT_SAME * max(inv_p, inv_c)) {
                                // its own ray on that surface, at the surface's 1 / distance there
                                at[ss] = float2(xc + o.x + g_disparity * inv_p, yc + o.y);
                                at_inv[ss] = inv_p;
                                placed |= bit;
                            } else if ((has_beside & bit) == 0u) {
                                // Not this surface at the place: the pixel looks past the outline of a nearer surface.
                                //  - The column is the nearer surface and the place shows a farther one (the ray passes
                                //    beside the outline): what the place shows, the wide view's image at the nearer
                                //    surface's shift carried on past its outline. If no column places the pixel it is
                                //    in the gap beside the nearer surface (what the wide view could not see) and gets
                                //    this. [fix85 18.11.2026] If one does (the farther surface, further on), the pixel
                                //    gets part of this as well, by the nearer surface's share of the place (all of it
                                //    at the outline, share 0.5, none from 0.25 down: about the width of the image's own
                                //    edge): the outline's anti-aliasing in the wide view's image goes on across the
                                //    outline instead of ending at it. Before, a pixel was the nearer surface or the
                                //    farther one and nothing between: with the head moving a tenth of a pixel, one
                                //    outline pixel in ten changed from the one colour to the other. RE7 whole-chain
                                //    dumps of 18.11.2026, the left periphery from one frame to the next, pixels that
                                //    change by more than 3 / 6 / 12 % of the range: fix84 1980 / 633 / 189, now
                                //    963 / 277 / 42 (dump 02; the wide view's own image 103 / 15 / 0). With 2 * share
                                //    the same counts and twice the error on the far side (outline_test.py).
                                // (The column the farther surface and the place behind a nearer one, the pixel just
                                // past the end of the farther surface as this eye sees it: nothing here, the ray goes to
                                // the second pass if no column places the pixel. Tried for fix85: that column itself;
                                // its texels hold the nearer thing's colour and drew it along the gap.)
                                if (inv_p < inv_c) {
                                    fb[ss] = float2(xc + o.x + s, yc + o.y);
                                    fb_inv[ss] = inv_p;
                                    fb_w[ss] = stepped && abs(near_inv - inv_c) <= SOFT_SAME * inv_c ? saturate(4.0 * share - 1.0) : 0.0;
                                    has_beside |= bit;
                                }
                            }
                        }
                    }
                }
            }

            --i2;
        }

        [branch] if ((placed | has_beside) == 15u) {
            [unroll] for (uint ss2 = 0; ss2 < 4; ++ss2) {
                const uint bit2 = 1u << ss2;
                const bool own = (placed & bit2) != 0u;
                const float2 pos = own ? at[ss2] : fb[ss2];
                // (clean_edge moves the read only where a column beside it is much nearer than this surface; [fix85] not
                // for the read beside a nearer outline: it is the outline's own edge that is wanted there. Tried for
                // fix85 and left out: the read of a placed pixel moved off a nearer outline any way round, by up to two
                // texels; as many pixels changed between two frames of the dumps, or more.)
                const float sxf = own ? clean_edge(pos.x, clamp(int(floor(pos.x)), 0, w - 1), clamp(int(pos.y), 0, yl), 1.0 / max(at_inv[ss2], 1e-6)) : pos.x;
                const float2 uv = float2(sxf / float(g_src_w), pos.y / float(g_src_h));
                float3 c = g_src.SampleLevel(g_lin, uv, 0).rgb;

                [branch] if (own && (has_beside & bit2) != 0u && fb_w[ss2] > 0.0) {
                    c = lerp(c, g_src.SampleLevel(g_lin, fb[ss2] / float2(float(g_src_w), float(g_src_h)), 0).rgb, fb_w[ss2]);
                }

                g_out[id.xy * 2 + uint2(ss2 & 1u, ss2 >> 1)] = float4(c, 1.0);
            }

            return;
        }

        uint slot2;
        g_count.InterlockedAdd((g_flags & 8) != 0 ? 4 : 0, 1u, slot2);
        g_holes[slot2] = (id.y << 16) | id.x;
        return;
    }
#endif

    bool hit = false;
    float best_sx = xc;
    int i = i_hi;
    int cur_b = -1;
    float b_reach = 0.0;

    [loop] while (i >= i_lo) {
        const int sx = x0 + sgn * i;
        const int b = sx >> 5;

        if (b != cur_b) {
            cur_b = b;
#ifdef BMIN_TEXTURE
            b_reach = reach_k / max(g_bmin[uint2(uint(b), uint(yi))], 1e-3) + 1.5;
#else
            [branch] if (lds) {
                b_reach = s_reach[(yi - y_lo) * RS_BLOCKS + (b - b_lo)];
            } else {
                b_reach = reach_k / max(g_bmin[uint2(uint(b), uint(yi))], 1e-3) + 1.5;
            }
#endif
        }

        if (float(i) > b_reach) {
            // no column of this block has this shift: on at the largest shift the block allows, or at the next block
            const int i_edge = sgn > 0 ? i - (sx - (b << 5)) : i - (((b << 5) + 31) - sx);
            const int i_ok = int(b_reach);
            i = i_ok >= i_edge ? i_ok : i_edge - 1;
            continue;
        }

        const float dist = distance_of(wide_depth_at(int2(sx, yi)));
        const float landed = (float(sx) + 0.5) - g_disparity / dist;

        if (abs(landed - xc) <= 0.501) {
            best_sx = clean_edge(xc + g_disparity / dist, sx, yi, dist);
            hit = true;
            break;
        }

        --i;
    }

    if (!hit) {
        uint slot;
        g_count.InterlockedAdd((g_flags & 8) != 0 ? 4 : 0, 1u, slot);
        g_holes[slot] = (id.y << 16) | id.x;
        return;
    }

    // what the full search does with a hit: the colour at that exact position
#ifdef SS2
    // ([PERIPHERY 2X fix80] ... for each of the ray's four pixels, see SS_OFFSET)
    [unroll] for (uint ss = 0; ss < 4; ++ss) {
        const float2 o = SS_OFFSET(ss);
        const float2 uv = float2((best_sx + o.x) / float(g_src_w), (yc + o.y) / float(g_src_h));
        g_out[id.xy * 2 + uint2(ss & 1u, ss >> 1)] = float4(g_src.SampleLevel(g_lin, uv, 0).rgb, 1.0);
    }
#else
    const float2 uv = float2(best_sx / float(g_src_w), yc / float(g_src_h));
    g_out[id.xy] = float4(g_src.SampleLevel(g_lin, uv, 0).rgb, 1.0);
#endif
}

// [GPU 29.10.2026] Second pass: the listed pixels with the full search (a fixed number of threads walking the list).
// [GPU 30.10.2026] compiled with SERIAL_SEARCH: a thread per possible pixel (round 3) launched far more groups than there
// are listed pixels, and the depths read four at a time were slower here too (test of 30.10: 0.057 against 0.050 ms).
[numthreads(64, 1, 1)]
void holes(uint3 id : SV_DispatchThreadID) {
    const uint count = g_count.Load((g_flags & 8) != 0 ? 4 : 0);

    for (uint k = id.x; k < count; k += 64 * 1024) {
        const uint p = g_holes[k];
        const uint2 px = uint2(p & 0xffff, p >> 16);
#ifdef SS2
        float4 s1, s2, s3;
        const float4 s0 = reproject_full(px, s1, s2, s3);
        g_out[px * 2] = s0;
        g_out[px * 2 + uint2(1, 0)] = s1;
        g_out[px * 2 + uint2(0, 1)] = s2;
        g_out[px * 2 + uint2(1, 1)] = s3;
#else
        g_out[px] = reproject_full(px);
#endif
    }
}
#endif
)";
} // namespace

bool MonoPeriphery::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> bmin_shader{};
    ComPtr<ID3DBlob> bounded_shader{};
    ComPtr<ID3DBlob> holes_shader{};
    ComPtr<ID3DBlob> bmin_cols_shader{};
    ComPtr<ID3DBlob> errors{};

    auto compile = [&](const char* entry, const D3D_SHADER_MACRO* defines, ComPtr<ID3DBlob>& out) {
        errors.Reset();

        if (FAILED(compile_cached(MONO_PERIPHERY_SHADER, sizeof(MONO_PERIPHERY_SHADER) - 1, "MonoPeriphery", defines, nullptr, entry, "cs_5_0", 0, 0, &out,
                &errors))) {
            REVR_LOG(spdlog::error("[MonoPeriphery] shader {} not compiled: {}", entry, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
            out.Reset();
            return false;
        }

        return true;
    };

    // [GPU 29.10.2026] the faster search needs its three entry points; without them the original pass alone (compiled
    // without the faster search's resources). The original pass and the second pass of round 2 keep their column by
    // column search (SERIAL_SEARCH): the GPU pass test compares them as they were.
    // [GPU 29.10.2026 round 3] block minima one thread per column (optional: round 2's pass otherwise); the second pass is
    // the same in both
    const D3D_SHADER_MACRO serial[]{{"SERIAL_SEARCH", "1"}, {nullptr, nullptr}};
    const bool fast = compile("main", serial, shader) && compile("block_min", nullptr, bmin_shader) && compile("bounded", nullptr, bounded_shader) &&
                      compile("holes", serial, holes_shader);
    const bool round3 = fast && compile("block_min_cols", nullptr, bmin_cols_shader);
    const D3D_SHADER_MACRO bmin_texture[]{{"BMIN_TEXTURE", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> bounded_tex_shader{};
    const bool bounded_tex = round3 && compile("bounded", bmin_texture, bounded_tex_shader);

    if (!fast) {
        bmin_shader.Reset();
        bounded_shader.Reset();
        holes_shader.Reset();
        const D3D_SHADER_MACRO defines[]{{"NO_BLOCK_MIN", "1"}, {"SERIAL_SEARCH", "1"}, {nullptr, nullptr}};

        if (!compile("main", defines, shader)) {
            m_last_error = "shader compile failed";
            return false;
        }
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; // colour, depth
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4; // out, block minima, list, counts
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 16;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();

    auto make_pso = [&](ID3DBlob* blob, ComPtr<ID3D12PipelineState>& out) {
        pso_desc.CS.pShaderBytecode = blob->GetBufferPointer();
        pso_desc.CS.BytecodeLength = blob->GetBufferSize();
        return SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&out)));
    };

    if (!make_pso(shader.Get(), m_pso)) {
        REVR_LOG(spdlog::error("[MonoPeriphery] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    if (fast && !(make_pso(bmin_shader.Get(), m_pso_bmin) && make_pso(bounded_shader.Get(), m_pso_bounded) && make_pso(holes_shader.Get(), m_pso_holes))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] pipeline states of the faster search failed, the original pass is used"));
        m_pso_bmin.Reset();
        m_pso_bounded.Reset();
        m_pso_holes.Reset();
    }

    if (round3 && m_pso_bounded != nullptr && !make_pso(bmin_cols_shader.Get(), m_pso_bmin_cols)) {
        REVR_LOG(spdlog::error("[MonoPeriphery] pipeline state of the round 3 block minima failed, round 2's is used"));
        m_pso_bmin_cols.Reset();
    }

    if (bounded_tex && m_pso_bounded != nullptr && !make_pso(bounded_tex_shader.Get(), m_pso_bounded_tex)) {
        m_pso_bounded_tex.Reset(); // only a test variant: the current first pass is used for it
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_pso_bmin.Reset();
        m_pso_bounded.Reset();
        m_pso_holes.Reset();
        m_pso_bmin_cols.Reset();
        m_pso_bounded_tex.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[MonoPeriphery] ready ({})", m_pso_bmin_cols != nullptr ? "with the faster search, round 3 passes"
                                              : m_pso_bounded != nullptr ? "with the faster search" : "original pass only"));
    return true;
}

// [PERIPHERY 2X fix80 17.11.2026] the first and the second pass of the faster search compiled with SS2 (2 x 2 pixels
// written per ray, see the shader); made on the first call
bool MonoPeriphery::ss_ready(ID3D12Device* device) {
    if (m_pso_bounded_ss != nullptr && m_pso_holes_ss != nullptr) {
        return true;
    }

    if (m_ss_failed || !setup(device) || m_pso_bounded == nullptr || m_pso_holes == nullptr || m_root_sig == nullptr) {
        return false;
    }

    m_ss_failed = true;
    const D3D_SHADER_MACRO ss[]{{"SS2", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO ss_serial[]{{"SS2", "1"}, {"SERIAL_SEARCH", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> bounded{};
    ComPtr<ID3DBlob> holes{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(MONO_PERIPHERY_SHADER, sizeof(MONO_PERIPHERY_SHADER) - 1, "MonoPeriphery", ss, nullptr, "bounded", "cs_5_0", 0, 0, &bounded,
            &errors)) ||
        FAILED(compile_cached(MONO_PERIPHERY_SHADER, sizeof(MONO_PERIPHERY_SHADER) - 1, "MonoPeriphery", ss_serial, nullptr, "holes", "cs_5_0", 0, 0, &holes,
            &errors))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] the passes writing 2 x 2 pixels per ray not compiled: {}",
            errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = bounded->GetBufferPointer();
    pso_desc.CS.BytecodeLength = bounded->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_bounded_ss)))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] the passes writing 2 x 2 pixels per ray: CreateComputePipelineState failed"));
        m_pso_bounded_ss.Reset();
        return false;
    }

    pso_desc.CS.pShaderBytecode = holes->GetBufferPointer();
    pso_desc.CS.BytecodeLength = holes->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_holes_ss)))) {
        REVR_LOG(spdlog::error("[MonoPeriphery] the passes writing 2 x 2 pixels per ray: CreateComputePipelineState failed"));
        m_pso_bounded_ss.Reset();
        m_pso_holes_ss.Reset();
        return false;
    }

    m_ss_failed = false;
    REVR_LOG(spdlog::info("[MonoPeriphery] the passes writing 2 x 2 pixels per ray ready"));
    return true;
}

// [GPU 29.10.2026] the faster search's resources (all rest in UNORDERED_ACCESS): block minima of the wide view, a list
// per eye (one uint per output pixel), both counts; recreated on a size change
bool MonoPeriphery::ensure_buffers(ID3D12Device* device, uint32_t src_w, uint32_t src_h, ID3D12Resource* const out[2]) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    const uint32_t bw = (src_w + 31) / 32;

    if (m_bmin == nullptr || m_bmin_w != bw || m_bmin_h != src_h) {
        if (m_bmin != nullptr) {
            m_retired.push_back(std::move(m_bmin));
            m_retired_age = 0;
        }

        m_bmin.Reset();
        m_bmin_w = 0;
        m_bmin_h = 0;

        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = bw;
        tex.Height = src_h;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R32_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (bw == 0 || src_h == 0 ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_bmin)))) {
            m_bmin.Reset();
            return false;
        }

        m_bmin->SetName(L"RE4VR mono periphery block minima");
        m_bmin_w = bw;
        m_bmin_h = src_h;
    }

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int eye = 0; eye < 2; ++eye) {
        const auto od = out[eye]->GetDesc();
        const uint32_t n = (uint32_t)od.Width * od.Height;

        if (m_holes[eye] != nullptr && m_holes_n[eye] == n) {
            continue;
        }

        if (m_holes[eye] != nullptr) {
            m_retired.push_back(std::move(m_holes[eye]));
            m_retired_age = 0;
        }

        m_holes[eye].Reset();
        m_holes_n[eye] = 0;
        buf.Width = (UINT64)(std::max)(n, 1u) * 4;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_holes[eye])))) {
            m_holes[eye].Reset();
            return false;
        }

        m_holes[eye]->SetName(eye == 0 ? L"RE4VR mono periphery leftover pixels left" : L"RE4VR mono periphery leftover pixels right");
        m_holes_n[eye] = n;
    }

    if (m_count == nullptr) {
        buf.Width = 256;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_count)))) {
            m_count.Reset();
            return false;
        }

        m_count->SetName(L"RE4VR mono periphery leftover counts");
    }

    // [GPU 29.10.2026 round 3] the counts of every frame copied here for the log (share of the pixels in the second pass)
    if (m_count_rb == nullptr && !m_count_rb_failed) {
        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb = buf;
        rb.Width = COUNT_FRAMES * 8;
        rb.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};

        if (FAILED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_count_rb))) ||
            FAILED(m_count_rb->Map(0, &no_read, &mapped)) || mapped == nullptr) {
            m_count_rb.Reset(); // only the log misses it
            m_count_rb_failed = true;
        } else {
            m_count_mapped = (const uint32_t*)mapped;
            m_count_frame.fill(~0ull);
        }
    }

    return true;
}

// [GPU 29.10.2026 round 3] CPU: the counts of frames at least four behind (their lists are done), as a share of the pixels
void MonoPeriphery::collect(uint64_t frame) {
    if (m_count_mapped == nullptr) {
        return;
    }

    for (uint32_t s = 0; s < COUNT_FRAMES; ++s) {
        if (m_count_frame[s] == ~0ull || m_count_frame[s] + 4 > frame) {
            continue;
        }

        for (int eye = 0; eye < 2; ++eye) {
            if (m_count_px[s][eye] > 0) {
                m_share_sum[eye] += (double)m_count_mapped[s * 2 + eye] / (double)m_count_px[s][eye];
            }
        }

        ++m_share_n;
        m_count_frame[s] = ~0ull;
    }

    if (m_share_n >= 90) {
        m_share[0] = m_share_sum[0] / m_share_n;
        m_share[1] = m_share_sum[1] / m_share_n;
        m_share_sum[0] = 0.0;
        m_share_sum[1] = 0.0;
        m_share_n = 0;
    }
}

void MonoPeriphery::write_constants(ID3D12GraphicsCommandList* cmd, uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h, const Params& p,
    uint32_t flags) {
    uint32_t constants[16]{};
    constants[0] = out_w;
    constants[1] = out_h;
    constants[2] = src_w;
    constants[3] = src_h;
    std::memcpy(&constants[4], &p.x_scale, 4);
    std::memcpy(&constants[5], &p.x_offset, 4);
    std::memcpy(&constants[6], &p.y_scale, 4);
    std::memcpy(&constants[7], &p.y_offset, 4);
    std::memcpy(&constants[8], &p.disparity, 4);
    std::memcpy(&constants[9], &p.dist_min, 4);
    constants[10] = (std::min)(p.max_shift, 256u);
    // [HOLES 02.11.2026 evening] the copy at texel centres without blur (128) and the edges cleaned over two columns (256):
    // the dumps of 02.11 20:48 showed the copy blurrier than the background around it and a dark outline along the gap
    // (the periphery's TAA spreads the arm's colour over two columns of the background)
    // [HOLES 02.11.2026 night] and the gap's width from the near thing's own edge column (1024): the dumps of 21:55 showed
    // short horizontal dashes in the copy (its shift jumping from row to row)
    // [OUTLINES fix84 18.11.2026] (2048) the four pixels of a ray each on their own surface, the outline from the stable
    // depth's mean (only the pass with 2 x 2 pixels per ray reads it)
    constants[11] = flags | (p.mark_holes ? 1u : 0u) | (p.fill_anchored ? 16u | 64u | 128u | 256u | 1024u : 0u) | (p.soft_outlines ? 2048u : 0u);
    std::memcpy(&constants[12], &p.depth_a, 4);
    std::memcpy(&constants[13], &p.depth_b, 4);
    std::memcpy(&constants[14], &p.depth_c, 4);
    std::memcpy(&constants[15], &p.depth_e, 4);
    cmd->SetComputeRoot32BitConstants(1, 16, constants, 0);
}

D3D12_GPU_DESCRIPTOR_HANDLE MonoPeriphery::write_table(ID3D12Device* device, ID3D12Resource* color, DXGI_FORMAT color_format, ID3D12Resource* depth,
    DXGI_FORMAT depth_format, ID3D12Resource* out, DXGI_FORMAT out_format, int eye) {
    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(0));
    srv.Format = depth_format;
    device->CreateShaderResourceView(depth, &srv, slot(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = out_format;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(2));
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(m_bmin.Get(), nullptr, &uav, slot(3));

    D3D12_UNORDERED_ACCESS_VIEW_DESC list{};
    list.Format = DXGI_FORMAT_UNKNOWN;
    list.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    list.Buffer.FirstElement = 0;
    list.Buffer.StructureByteStride = 4;
    const bool have_list = eye >= 0 && m_holes[eye < 2 ? eye : 1] != nullptr;
    list.Buffer.NumElements = have_list ? m_holes_n[eye] : 1;
    device->CreateUnorderedAccessView(have_list ? m_holes[eye].Get() : nullptr, nullptr, &list, slot(4));

    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.FirstElement = 0;
    raw.Buffer.NumElements = 64;
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(m_count.Get(), nullptr, &raw, slot(5));

    return gpu;
}

bool MonoPeriphery::reproject(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
    ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource* const out[2], const Params p[2], Search search, uint64_t frame,
    const std::function<void(int)>& stamp) {
    if (device == nullptr || cmd == nullptr || color == nullptr || depth == nullptr || out == nullptr || out[0] == nullptr || out[1] == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    // the replaced resources go once no list in flight can use them any more (lists run at most three frames deep)
    if (!m_retired.empty() && ++m_retired_age > 8) {
        m_retired.clear();
    }

    const auto cdesc = color->GetDesc();
    const auto color_format = srv_format_for(cdesc.Format);
    const auto depth_format = depth_srv_format_for(depth->GetDesc().Format);
    const DXGI_FORMAT out_format[2]{uav_format_for(out[0]->GetDesc().Format), uav_format_for(out[1]->GetDesc().Format)};

    if (depth_format == DXGI_FORMAT_UNKNOWN || out_format[0] == DXGI_FORMAT_UNKNOWN || out_format[1] == DXGI_FORMAT_UNKNOWN) {
        m_last_error = "unsupported format";
        return false;
    }

    // [PERIPHERY 2X fix80 17.11.2026] ss = 2: the colour and the eyes' images hold 2 x 2 texels per column and row of the
    // grid the search runs on (the wide view's own size); the sizes handed to the shader are the grid's and the number of
    // rays, as with 1. Only with the faster search's two passes (the test variants of the search are left out).
    const uint32_t ss = p[0].ss >= 2 ? 2u : 1u;

    if (ss == 2) {
        const auto o0 = out[0]->GetDesc();
        const auto o1 = out[1]->GetDesc();

        if (m_pso_bounded_ss == nullptr || m_pso_holes_ss == nullptr || (cdesc.Width & 1) != 0 || (cdesc.Height & 1) != 0 || (o0.Width & 1) != 0 ||
            (o0.Height & 1) != 0 || (o1.Width & 1) != 0 || (o1.Height & 1) != 0) {
            m_last_error = "2 x 2 pixels per ray: passes or image sizes not usable";
            return false;
        }

        if (search == Search::Original || search == Search::TwoPassRound2 || search == Search::TwoPassRound3BminTexture) {
            search = Search::TwoPassRound3;
        }
    }

    const uint32_t src_w = (uint32_t)cdesc.Width / ss;
    const uint32_t src_h = cdesc.Height / ss;
    const bool use_fast = search != Search::Original && m_pso_bounded != nullptr && ensure_buffers(device, src_w, src_h, out);
    const bool round3 = use_fast && (search == Search::TwoPassRound3 || search == Search::TwoPassRound3BminTexture) && m_pso_bmin_cols != nullptr;

    if (ss == 2 && !use_fast) {
        m_last_error = "2 x 2 pixels per ray: the faster search's buffers could not be made";
        return false;
    }

    if (!use_fast) {
        m_share[0] = -1.0; // the original pass has no second pass to count
        m_share[1] = -1.0;
    }

    // (the original pass gets null views for the faster search's resources that do not exist)
    const auto color_read = compute_read_state(color_state);
    const auto depth_read = compute_read_state(depth_state);

    // inputs to read states once for all the dispatches
    {
        D3D12_RESOURCE_BARRIER b[2]{};
        uint32_t nb = 0;

        for (auto [res, before, after] : {std::tuple{color, color_state, color_read}, std::tuple{depth, depth_state, depth_read}}) {
            if (before != after) {
                b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[nb].Transition.pResource = res;
                b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b[nb].Transition.StateBefore = before;
                b[nb].Transition.StateAfter = after;
                ++nb;
            }
        }

        if (nb > 0) {
            cmd->ResourceBarrier(nb, b);
        }
    }

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());

    auto uav_barrier = [&](std::initializer_list<ID3D12Resource*> list) {
        D3D12_RESOURCE_BARRIER b[4]{};
        uint32_t nb = 0;

        for (auto* r : list) {
            if (r != nullptr && nb < 4) {
                b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b[nb].UAV.pResource = r;
                ++nb;
            }
        }

        if (nb > 0) {
            cmd->ResourceBarrier(nb, b);
        }
    };

    if (use_fast) {
        // block minima of the wide view, counts to zero
        cmd->SetComputeRootDescriptorTable(0, write_table(device, color, color_format, depth, depth_format, out[0], out_format[0], 0));
        write_constants(cmd, 0, 0, src_w, src_h, p[0], 0);

        if (round3) {
            cmd->SetPipelineState(m_pso_bmin_cols.Get());
            cmd->Dispatch(m_bmin_w, (src_h + 7) / 8, 1); // a group per block of 32 columns and 8 rows
        } else {
            cmd->SetPipelineState(m_pso_bmin.Get());
            cmd->Dispatch((m_bmin_w + 63) / 64, src_h, 1);
        }

        uav_barrier({m_bmin.Get(), m_count.Get()});
        stamp(0);

        // first pass of both eyes, then the leftover pixels of both
        // ([PERIPHERY 2X fix80] rays_w x rays_h: the rays searched per eye, the eye's image / ss)
        cmd->SetPipelineState(ss == 2 ? m_pso_bounded_ss.Get()
                              : round3 && search == Search::TwoPassRound3BminTexture && m_pso_bounded_tex != nullptr ? m_pso_bounded_tex.Get()
                                                                                                                 : m_pso_bounded.Get());

        for (int eye = 0; eye < 2; ++eye) {
            const auto od = out[eye]->GetDesc();
            const uint32_t rays_w = (uint32_t)od.Width / ss;
            const uint32_t rays_h = od.Height / ss;
            cmd->SetComputeRootDescriptorTable(0, write_table(device, color, color_format, depth, depth_format, out[eye], out_format[eye], eye));
            write_constants(cmd, rays_w, rays_h, src_w, src_h, p[eye], 2u | (eye == 1 ? 8u : 0u));
            cmd->Dispatch((rays_w + 7) / 8, (rays_h + 7) / 8, 1);
        }

        uav_barrier({m_holes[0].Get(), m_holes[1].Get(), m_count.Get()});
        stamp(1);
        cmd->SetPipelineState(ss == 2 ? m_pso_holes_ss.Get() : m_pso_holes.Get());

        for (int eye = 0; eye < 2; ++eye) {
            const auto od = out[eye]->GetDesc();
            cmd->SetComputeRootDescriptorTable(0, write_table(device, color, color_format, depth, depth_format, out[eye], out_format[eye], eye));
            write_constants(cmd, (uint32_t)od.Width / ss, od.Height / ss, src_w, src_h, p[eye], 4u | (eye == 1 ? 8u : 0u));
            // 64 x 1024 threads walking the list
            cmd->Dispatch(1024, 1, 1);
        }

        uav_barrier({out[0], out[1]});

        // the counts for the log
        if (m_count_rb != nullptr) {
            const uint32_t s = (uint32_t)(frame % COUNT_FRAMES);
            barrier(cmd, m_count.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(m_count_rb.Get(), (UINT64)s * 8, m_count.Get(), 0, 8);
            barrier(cmd, m_count.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_count_frame[s] = frame;

            for (int eye = 0; eye < 2; ++eye) {
                const auto od = out[eye]->GetDesc();
                m_count_px[s][eye] = ((uint32_t)od.Width / ss) * (od.Height / ss); // (rays: the list holds rays)
            }
        }

        stamp(2); // (the copy of the counts is timed with the reprojection)
    } else {
        stamp(0);
        cmd->SetPipelineState(m_pso.Get());

        for (int eye = 0; eye < 2; ++eye) {
            const auto od = out[eye]->GetDesc();
            cmd->SetComputeRootDescriptorTable(0, write_table(device, color, color_format, depth, depth_format, out[eye], out_format[eye], -1));
            write_constants(cmd, (uint32_t)od.Width, od.Height, src_w, src_h, p[eye], 0);
            cmd->Dispatch(((uint32_t)od.Width + 7) / 8, (od.Height + 7) / 8, 1);
        }

        uav_barrier({out[0], out[1]});
        stamp(1);
        stamp(2);
    }

    {
        D3D12_RESOURCE_BARRIER b[2]{};
        uint32_t nb = 0;

        for (auto [res, before, after] : {std::tuple{depth, depth_read, depth_state}, std::tuple{color, color_read, color_state}}) {
            if (before != after) {
                b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[nb].Transition.pResource = res;
                b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b[nb].Transition.StateBefore = before;
                b[nb].Transition.StateAfter = after;
                ++nb;
            }
        }

        if (nb > 0) {
            cmd->ResourceBarrier(nb, b);
        }
    }

    m_last_error = "";
    return true;
}

void MonoPeriphery::reset() {
    m_pso.Reset();
    m_pso_bmin.Reset();
    m_pso_bounded.Reset();
    m_pso_holes.Reset();
    m_pso_bounded_ss.Reset();
    m_pso_holes_ss.Reset();
    m_ss_failed = false;
    m_bmin.Reset();
    m_holes[0].Reset();
    m_holes[1].Reset();
    m_count.Reset();
    m_bmin_w = 0;
    m_bmin_h = 0;
    m_holes_n[0] = 0;
    m_holes_n[1] = 0;
    m_retired.clear();
    m_retired_age = 0;
    m_pso_bmin_cols.Reset();
    m_pso_bounded_tex.Reset();

    if (m_count_rb != nullptr && m_count_mapped != nullptr) {
        m_count_rb->Unmap(0, nullptr);
    }

    m_count_rb.Reset();
    m_count_rb_failed = false;
    m_count_mapped = nullptr;
    m_count_frame.fill(~0ull);
    m_share_sum[0] = 0.0;
    m_share_sum[1] = 0.0;
    m_share_n = 0;
    m_share[0] = -1.0;
    m_share[1] = -1.0;
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [STABLE DEPTH fix81 17.11.2026] StableDepth, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char STABLE_DEPTH_SHADER[] = R"(
Texture2D<float>    g_depth : register(t0); // the wide view's depth
Texture2D<float4>   g_mv    : register(t1); // its motion vectors
Texture2D<float4>   g_prev  : register(t2); // the last frame's result: x = 1 / distance handed on, y = mean of 1 / distance,
                                            // z, w = [OUTLINES fix84] its slower mean and that mean held still (see below)
RWTexture2D<float4> g_out   : register(u0);
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    uint2  g_size;     // the depth image (and the result)
    uint2  g_mv_size;  // the motion vectors' image
    float2 g_mv_scale; // current UV + mv * scale = previous UV
    uint   g_flags;    // 1 = no history
    float  g_rate;     // of the means
    float4 g_depth_k;  // distance = |(x z + y) / (z z + w)|
    float  g_dist_min; // nearer counts as this
    float  g_step0;    // a step: the 3 x 3's values apart by this part of the nearest ...
    float  g_step1;    // ... to this
    float  g_hyst;     // of the step, kept on the surface the pixel was on
};

// 1 / distance (the distance as the reprojection takes it, at most 10 km)
float inv_dist(float z) {
    float den = g_depth_k.z * z + g_depth_k.w;
    den = abs(den) < 1e-9 ? (den < 0.0 ? -1e-9 : 1e-9) : den;
    return 1.0 / clamp(abs((g_depth_k.x * z + g_depth_k.y) / den), max(g_dist_min, 1e-3), 10000.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_size.x || id.y >= g_size.y) {
        return;
    }

    const int2 last = int2(g_size) - 1;
    float v[9];
    float vnear = 0.0;
    float vfar = 1e30;
    int2 nq = int2(id.xy);

    [unroll] for (int k = 0; k < 9; ++k) {
        const int2 q = clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), last);
        v[k] = inv_dist(g_depth.Load(int3(q, 0)));

        if (v[k] > vnear) {
            vnear = v[k];
            nq = q;
        }

        vfar = min(vfar, v[k]);
    }

    const float own = v[4];
    // the motion of the nearest surface of the 3 x 3 (as the temporal passes take it), read at its place in the motion image
    const float2 sz = float2(g_size);
    const int2 mq = min(int2((float2(nq) + 0.5) * float2(g_mv_size) / sz), int2(g_mv_size) - 1);
    const float2 puv = (float2(id.xy) + 0.5) / sz + g_mv.Load(int3(mq, 0)).xy * g_mv_scale;
    bool ok = (g_flags & 1) == 0 && all(puv >= 0.0) && all(puv <= 1.0);
    float4 prev = 0.0;
    // [OUTLINES fix84 18.11.2026] The 1 / distance handed on a frame ago is no longer read between the texels: beside an
    // outline that mixed the near surface's value with the far one's, and the mix, as long as it stayed within g_step0 of
    // the surface, was steadied into the surface's own value below: while the picture moved slowly (the head is never
    // still) the outline texels' 1 / distance sank by up to 5 % and jumped back every few frames, and the outline of a
    // near thing with it by half a column of the wide view (tests/supertaa/periphery/outline_test.py, 0.02 texels per
    // frame: a saw tooth of 5 frames; in the game the outlines that tremble). pc: the four texels around the place, pw
    // their weights; cp: the nearest one's (the surface the pixel was on).
    float4 pc = 0.0;
    float4 pw = 0.0;
    float cp = 0.0;

    [branch] if (ok) {
        prev = g_prev.SampleLevel(g_lin, puv, 0);
        ok = prev.y > 0.0;
        const float2 tt = puv * sz - 0.5;
        const float2 fl = floor(tt);
        const float2 f = tt - fl;
        const int2 i0 = int2(fl);
        pc = float4(g_prev.Load(int3(clamp(i0, int2(0, 0), last), 0)).x, g_prev.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0)).x,
                    g_prev.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0)).x, g_prev.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0)).x);
        pw = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
        cp = f.y >= 0.5 ? (f.x >= 0.5 ? pc.w : pc.z) : (f.x >= 0.5 ? pc.y : pc.x);
        ok = ok && cp > 0.0;
    }

    // the mean of 1 / distance, the last one held within what the 3 x 3 shows now
    float m = ok ? clamp(prev.y, vfar, vnear) : own;
    m += g_rate * (own - m);

    // a step: the sample of the 3 x 3 nearest to the mean; the one nearest to what was handed on a frame ago stays until
    // the mean has gone g_hyst of the step farther from it
    float best = v[0];
    float prevc = v[0];

    [unroll] for (int j = 1; j < 9; ++j) {
        best = abs(v[j] - m) < abs(best - m) ? v[j] : best;
        prevc = abs(v[j] - cp) < abs(prevc - cp) ? v[j] : prevc;
    }

    const float span = vnear - vfar;
    const bool keep = ok && abs(prevc - cp) <= 0.5 * g_step0 * cp && abs(prevc - m) <= abs(best - m) + g_hyst * span;
    float chosen = keep ? prevc : best;

    // ... and that surface's own 1 / distance steadied over time ([fix84] from the last frame's texels around the place
    // that hold this surface, by their weights; none of them: the sample as it is)
    const float4 same = float4(abs(pc.x - chosen) <= g_step0 * chosen ? 1.0 : 0.0, abs(pc.y - chosen) <= g_step0 * chosen ? 1.0 : 0.0,
                               abs(pc.z - chosen) <= g_step0 * chosen ? 1.0 : 0.0, abs(pc.w - chosen) <= g_step0 * chosen ? 1.0 : 0.0) * pw;
    const float sw = same.x + same.y + same.z + same.w;

    if (ok && sw > 0.02) {
        const float cps = dot(same, pc) / sw;
        chosen = cps + g_rate * (chosen - cps);
    }

    float t = saturate((span / max(vnear, 1e-9) - g_step0) / (g_step1 - g_step0));
    t = t * t * (3.0 - 2.0 * t);

    // [OUTLINES fix84 18.11.2026] For the outline to a fraction of a texel (MonoPeriphery's soft_inv reads w): how much of
    // the texel the near surface covers. The mean above still ripples with the frames' jitter (a texel half covered is the
    // near surface in some frames of the jitter's round and the far one in the others): an outline placed by it would
    // flicker in a scene that stands still (tests/supertaa/periphery/outline_test.py). z: the mean at half the rate.
    // w: z held still: it only follows where z leaves it by more than a twentieth of the step between the two surfaces
    // (a play of less than a twentieth of a texel in the outline's place), so in a still scene it does not move at all.
    float e2 = ok && prev.z > 0.0 ? clamp(prev.z, vfar, vnear) : m;
    e2 += 0.5 * g_rate * (own - e2);
    const float play = 0.05 * span;
    const float held = ok && prev.w > 0.0 ? clamp(clamp(prev.w, vfar, vnear), e2 - play, e2 + play) : e2;
    g_out[id.xy] = float4(m + (chosen - m) * t, m, e2, held);
}
)";
} // namespace

bool StableDepth::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(STABLE_DEPTH_SHADER, sizeof(STABLE_DEPTH_SHADER) - 1, "StableDepth", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[StableDepth] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 16;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[StableDepth] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[StableDepth] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[StableDepth] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[StableDepth] ready"));
    return true;
}

bool StableDepth::ensure_images(ID3D12Device* device, uint32_t w, uint32_t h) {
    if (m_image[0] != nullptr && m_image[1] != nullptr && m_w == w && m_h == h) {
        return true;
    }

    // (a new size, e.g. RE7's video tape with the game camera at 2/3 of the size: lists in flight may still read the old
    // images, they are kept for some frames)
    for (auto& image : m_image) {
        if (image != nullptr) {
            m_retired.emplace_back(std::move(image), 0u);
        }

        image.Reset();
    }

    m_valid = false;
    m_w = 0;
    m_h = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_image[i])))) {
            m_image[0].Reset();
            m_image[1].Reset();
            m_last_error = "could not create the images";
            return false;
        }

        m_image[i]->SetName(i == 0 ? L"RE4VR stable depth 0" : L"RE4VR stable depth 1");
    }

    m_w = w;
    m_h = h;
    m_cur = 0;
    return true;
}

bool StableDepth::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state,
    ID3D12Resource* motion, D3D12_RESOURCE_STATES motion_state, const Params& p) {
    if (device == nullptr || cmd == nullptr || depth == nullptr || motion == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto dd = depth->GetDesc();
    const auto md = motion->GetDesc();
    const auto depth_format = depth_srv_format_for(dd.Format);
    const auto motion_format = srv_format_for(md.Format);

    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || depth_format == DXGI_FORMAT_UNKNOWN ||
        dd.SampleDesc.Count != 1 || md.SampleDesc.Count != 1 || dd.Width == 0 || dd.Height == 0 || md.Width == 0 || md.Height == 0) {
        m_last_error = "unsupported input";
        return false;
    }

    const uint32_t w = (uint32_t)dd.Width;
    const uint32_t h = dd.Height;

    if (!ensure_images(device, w, h)) {
        return false;
    }

    for (auto it = m_retired.begin(); it != m_retired.end();) {
        it = ++it->second > RETIRE_FRAMES ? m_retired.erase(it) : it + 1;
    }

    const bool reset = p.reset || !m_valid;
    ID3D12Resource* prev = m_image[m_cur].Get();
    ID3D12Resource* out = m_image[m_cur ^ 1u].Get();

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = depth_format;
    device->CreateShaderResourceView(depth, &srv, cpu);
    srv.Format = motion_format;
    device->CreateShaderResourceView(motion, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + m_descriptor_size});
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(prev, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * (SIZE_T)m_descriptor_size});

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 3 * (SIZE_T)m_descriptor_size});

    const auto depth_read = compute_read_state(depth_state);
    const auto motion_read = compute_read_state(motion_state);
    const auto UAV_STATE = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto READ_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    barrier(cmd, depth, depth_state, depth_read);
    barrier(cmd, motion, motion_state, motion_read);
    barrier(cmd, prev, UAV_STATE, READ_STATE);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    struct Constants {
        uint32_t size[2];
        uint32_t mv_size[2];
        float mv_scale[2];
        uint32_t flags;
        float rate;
        float depth_k[4];
        float dist_min;
        float step0;
        float step1;
        float hyst;
    } k{};
    static_assert(sizeof(Constants) == 16 * 4);
    k.size[0] = w;
    k.size[1] = h;
    k.mv_size[0] = (uint32_t)md.Width;
    k.mv_size[1] = md.Height;
    k.mv_scale[0] = p.mv_scale_x;
    k.mv_scale[1] = p.mv_scale_y;
    k.flags = reset ? 1u : 0u;
    k.rate = std::clamp(p.rate, 0.01f, 1.0f);
    k.depth_k[0] = p.depth_a;
    k.depth_k[1] = p.depth_b;
    k.depth_k[2] = p.depth_c;
    k.depth_k[3] = p.depth_e;
    k.dist_min = (std::max)(p.dist_min, 1e-3f);
    k.step0 = std::clamp(p.step0, 0.001f, 0.9f);
    k.step1 = (std::max)(std::clamp(p.step1, 0.001f, 1.0f), k.step0 + 1e-3f);
    k.hyst = std::clamp(p.hysteresis, 0.0f, 1.0f);
    cmd->SetComputeRoot32BitConstants(1, 16, &k, 0);

    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &uav_done);

    barrier(cmd, prev, READ_STATE, UAV_STATE);
    barrier(cmd, motion, motion_read, motion_state);
    barrier(cmd, depth, depth_read, depth_state);

    m_cur ^= 1u;
    m_valid = true;
    m_last_error = "";
    return true;
}

void StableDepth::reset() {
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_image[0].Reset();
    m_image[1].Reset();
    m_retired.clear();
    m_w = 0;
    m_h = 0;
    m_cur = 0;
    m_valid = false;
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [MENUS fix86 18.11.2026] UiSteady, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char UI_STEADY_SHADER[] = R"(
Texture2D<float4>   g_col  : register(t0); // the wide view's colour after the output pass, this frame (with its jitter)
Texture2D<float4>   g_hud  : register(t1); // the engine's copy without GUI
Texture2D<float4>   g_fin  : register(t2); // the engine's copy with GUI
Texture2D<float4>   g_mv   : register(t3); // the wide view's motion vectors
Texture2D<float4>   g_pc   : register(t4); // the last results (rgb, alpha = the weight collected)
Texture2D<float4>   g_ph   : register(t5);
Texture2D<float4>   g_pf   : register(t6);
RWTexture2D<float4> g_oc   : register(u0);
RWTexture2D<float4> g_oh   : register(u1);
RWTexture2D<float4> g_of   : register(u2);
RWTexture2D<uint>   g_map  : register(u3); // GuiMap's blocks (8 x 8 pixels of the frame's images; this frame's stamp = marked)
RWTexture2D<uint>   g_act  : register(u4); // [fix94] per block of the map: the number of the last dispatch that worked on it
RWTexture2D<uint>   g_actp : register(u5); //   and as the dispatch before left it
SamplerState        g_lin  : register(s0);

cbuffer Params : register(b0) {
    uint2  g_size;        // the frame's images
    uint2  g_out_size;    // the results
    uint2  g_mv_size;     // the motion vectors' image
    float2 g_jit;         // a point of the results at p (pixels of the frame's images) is at p + jit in the frame
    float2 g_mv_scale;    // current UV + mv * scale = previous UV
    uint   g_flags;       // 1 = no history, 2 = the colour's view is sRGB (kept encoded), 4 = mark the map's blocks,
                          // [fix94] 8 = only the blocks g_act holds g_seq for, 32 = the history only from blocks g_actp
                          // holds g_seq_prev for, 64 (around) = every block
    float  g_w_max;       // the history's weight at most
    float  g_inv_2s2;     // 1 / (2 sigma^2) of a sample's weight
    float  g_inv_half2;   // 1 / motion_half^2
    uint2  g_map_size;    // the map's blocks
    uint   g_stamp;       // this frame's stamp of the map
    uint   g_seq;         // [fix94] this dispatch's number (flags 8, 64)
    uint   g_seq_prev;    //   the number the dispatch before wrote (flag 32)
    uint   g_hold;        //   a block is worked on this many stamps after the last mark in its reach
};

// what the images may hold that must not get into the history for good (R11G11B10_FLOAT has infinities and NaN)
float3 sane(float3 c) {
    return min(max(c, 0.0), 64000.0);
}

float3 encode(float3 c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_out_size.x || id.y >= g_out_size.y) {
        return;
    }

    // the texel's middle in pixels of the frame's images, and where this frame holds that point
    const float2 x = (float2(id.xy) + 0.5) * float2(g_size) / float2(g_out_size);

    // [fix94 19.11.2026] (flag 8) Only the blocks around the interface are worked on (`around` below says which). A block
    // left out for the first or the second time gets empty texels in this dispatch's images (the two sets of images take
    // turns): whatever reads them afterwards finds the results without and with GUI equal there, no interface.
    [branch] if ((g_flags & 8) != 0) {
        const uint age = g_seq - g_act[min(uint2(x) >> 3, g_map_size - 1)];

        if (age != 0) {
            if (age <= 2) {
                g_oc[id.xy] = 0.0;
                g_oh[id.xy] = 0.0;
                g_of[id.xy] = 0.0;
            }

            return;
        }
    }

    const float2 f = x + g_jit;
    const int2 last = int2(g_size) - 1;
    const int2 i0 = int2(floor(f - 0.5));
    float3 cc = 0.0;
    float3 ch = 0.0;
    float3 cf = 0.0;
    float ws = 0.0;

    // the four samples around it, each by its distance (the same weights for the three images)
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 i = i0 + int2(k & 1, k >> 1);
        const float2 d = float2(i) + 0.5 - f;
        const int3 q = int3(clamp(i, int2(0, 0), last), 0);
        const float w = exp(-dot(d, d) * g_inv_2s2);
        float3 c = sane(g_col.Load(q).rgb);

        if ((g_flags & 2) != 0) {
            c = encode(c);
        }

        cc += w * c;
        ch += w * sane(g_hud.Load(q).rgb);
        cf += w * sane(g_fin.Load(q).rgb);
        ws += w;
    }

    // the results of the frame before, where the motion vectors say the point was
    const int2 mq = clamp(int2(f / float2(g_size) * float2(g_mv_size)), int2(0, 0), int2(g_mv_size) - 1);
#ifdef UI_MOTION
    // [UI MOTION fix125 22.11.2026] (compiled with UI_MOTION, after UiMotion.hpp's text) These images hold the interface
    // for the eye pass, and the motion vectors hold the motion of the scene behind it: the HUD at the weapon, a panel in
    // front of the game camera while the player walks, were fetched where the scene had been (a trail where they were,
    // faint where they are; fix94's note on the HUD at the weapon). The history comes from where the interface itself
    // was: the element's own motion where one is known for the place, the scene's vectors otherwise.
    float2 mv = g_mv.Load(int3(mq, 0)).xy * g_mv_scale;
    {
        float2 um;

        [branch] if (um_motion(x / float2(g_size), um)) {
            mv = um;
        }
    }
#else
    const float2 mv = g_mv.Load(int3(mq, 0)).xy * g_mv_scale;
#endif
    const float2 puv = x / float2(g_size) + mv;
    float wh = 0.0;
    float3 hc = 0.0;
    float3 hh = 0.0;
    float3 hf = 0.0;

    bool have = (g_flags & 1) == 0 && all(puv > 0.0) && all(puv < 1.0);

    // [fix94] (flag 32) the dispatch before left blocks out: the history counts where the four texels read are all of
    // blocks it worked on
    [branch] if (have && (g_flags & 32) != 0) {
        const int2 olast = int2(g_out_size) - 1;
        const int2 ta = int2(floor(puv * float2(g_out_size) - 0.5));
        const float2 to_px = float2(g_size) / float2(g_out_size);
        const uint2 mlast2 = g_map_size - 1;
        const uint2 ba = min(uint2((float2(clamp(ta, int2(0, 0), olast)) + 0.5) * to_px) >> 3, mlast2);
        const uint2 bb = min(uint2((float2(clamp(ta + 1, int2(0, 0), olast)) + 0.5) * to_px) >> 3, mlast2);
        have = g_actp[ba] == g_seq_prev && g_actp[uint2(bb.x, ba.y)] == g_seq_prev && g_actp[uint2(ba.x, bb.y)] == g_seq_prev && g_actp[bb] == g_seq_prev;
    }

    if (have) {
        const float4 a = g_pc.SampleLevel(g_lin, puv, 0);
        const float2 px = mv * float2(g_size);
        wh = min(max(a.a, 0.0), g_w_max) / (1.0 + dot(px, px) * g_inv_half2);
        hc = sane(a.rgb);
        hh = sane(g_ph.SampleLevel(g_lin, puv, 0).rgb);
        hf = sane(g_pf.SampleLevel(g_lin, puv, 0).rgb);
    }

    const float wt = wh + ws;
    const float3 oh = (hh * wh + ch) / wt;
    const float3 of = (hf * wh + cf) / wt;
    g_oc[id.xy] = float4((hc * wh + cc) / wt, wt);
    g_oh[id.xy] = float4(oh, wt);
    g_of[id.xy] = float4(of, wt);

    // where the results differ the map's blocks are marked: a block reaches 2 pixels around itself (as GuiMap marks them),
    // so the pixel t is in the reach of the blocks (t - 2) / 8 .. (t + 2) / 8. The same value from every thread.
    // (the eye pass's masks begin at 0.03 of the output's range; what is left of an interface gone falls under this in
    // about a second)
    if ((g_flags & 4) != 0 && any(abs(of - oh) > 1e-4 + 0.005 * max(of, oh))) {
        const int2 t = int2(floor(x));
        const int2 mlast = int2(g_map_size) - 1;
        const int2 b0 = clamp((t - 2) >> 3, int2(0, 0), mlast);
        const int2 b1 = clamp((t + 2) >> 3, int2(0, 0), mlast);

        [branch] if ((g_flags & 8) == 0) {
            g_map[uint2(b0)] = g_stamp;
            g_map[uint2(b1.x, b0.y)] = g_stamp;
            g_map[uint2(b0.x, b1.y)] = g_stamp;
            g_map[uint2(b1)] = g_stamp;
        } else {
            // [fix94] (only blocks worked on in this dispatch: the eye pass reads the results of every marked block)
            [unroll] for (uint k4 = 0; k4 < 4; ++k4) {
                const uint2 b = uint2((k4 & 1u) != 0 ? b1.x : b0.x, (k4 & 2u) != 0 ? b1.y : b0.y);

                if (g_act[b] == g_seq) {
                    g_map[b] = g_stamp;
                }
            }
        }
    }
}

// [fix94 19.11.2026] Which blocks the pass works on when it is told to work around the interface only (one thread per
// block of the map, before `main`): the blocks with a mark of the last g_hold stamps within AROUND blocks (GuiMap's of
// this frame's copies, and those `main` made where its results differed). g_act gets this dispatch's number for them and
// keeps what it held for the others: `main` tells from it how long ago a block was last worked on.
// AROUND: what the eye pass reads of a tile with a marked block in reach lies within a tile and its taps of that block
// (under 2 blocks with tiles of 32 x 32 eye pixels); and the history of a marked block comes from blocks worked on as
// long as the picture moves less than the rest of it in a frame.
#define AROUND 3

[numthreads(8, 8, 1)]
void around(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_map_size.x || id.y >= g_map_size.y) {
        return;
    }

    bool on = (g_flags & 64) != 0;

    [branch] if (!on) {
        const int2 mlast = int2(g_map_size) - 1;

        [unroll] for (int dy = -AROUND; dy <= AROUND; ++dy) {
            [unroll] for (int dx = -AROUND; dx <= AROUND; ++dx) {
                const uint s = g_map[uint2(clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), mlast))];
                on = on || (s != 0 && g_stamp - s <= g_hold);
            }
        }
    }

    g_act[id.xy] = on ? g_seq : g_actp[id.xy];
}
)";
} // namespace

// [UI MOTION fix125] the pass's text with the interface's motion (UiMotion.hpp's text first)
std::string UiSteady::ui_motion_source() {
    return std::string{"#define UI_MOTION 1\n#define UM_REG t7\n"} + UiMotion::hlsl() + UI_STEADY_SHADER;
}

const char* UiSteady::source() {
    return UI_STEADY_SHADER;
}

bool UiSteady::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(UI_STEADY_SHADER, sizeof(UI_STEADY_SHADER) - 1, "UiSteady", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[UiSteady] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 7;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 6; // [fix94] u4, u5: the blocks worked on
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 7;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 20;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[UiSteady] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[UiSteady] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    // [fix94] the pass that says which blocks are worked on (Params::only_around); without it the whole image as before
    {
        ComPtr<ID3DBlob> around{};
        m_pso_around.Reset();

        if (FAILED(compile_cached(UI_STEADY_SHADER, sizeof(UI_STEADY_SHADER) - 1, "UiSteady", nullptr, nullptr, "around", "cs_5_0", 0, 0, &around, &errors))) {
            REVR_LOG(spdlog::warn("[UiSteady] the pass for the blocks around the interface did not compile (the whole image is worked on): {}",
                errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        } else {
            pso_desc.CS.pShaderBytecode = around->GetBufferPointer();
            pso_desc.CS.BytecodeLength = around->GetBufferSize();

            if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_around)))) {
                REVR_LOG(spdlog::warn("[UiSteady] no pipeline state for the blocks around the interface (the whole image is worked on)"));
                m_pso_around.Reset();
            }
        }
    }

    // [UI MOTION fix125 22.11.2026] `main` with the interface's own motion (Params::ui_motion): a root signature like the
    // plain one with one more SRV (t7, the motion's block; the UAVs follow it in the table), `main` compiled with the
    // motion, and `around` for that root signature. Without any of them the plain pass goes on.
    {
        m_root_um.Reset();
        m_pso_um.Reset();
        m_pso_around_um.Reset();
        ranges[0].NumDescriptors = 8;
        ranges[1].OffsetInDescriptorsFromTableStart = 8;
        ComPtr<ID3DBlob> um_rs{};
        const std::string src = ui_motion_source();
        ComPtr<ID3DBlob> um{};
        ComPtr<ID3DBlob> um_around{};

        if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &um_rs, &errors)) ||
            FAILED(device->CreateRootSignature(0, um_rs->GetBufferPointer(), um_rs->GetBufferSize(), IID_PPV_ARGS(&m_root_um)))) {
            REVR_LOG(spdlog::warn("[UiSteady] no root signature for the pass with the interface's own motion (the scene's motion vectors are used)"));
            m_root_um.Reset();
        } else if (FAILED(compile_cached(src.data(), src.size(), "UiSteadyUiMotion", nullptr, nullptr, "main", "cs_5_0", 0, 0, &um, &errors)) ||
                   FAILED(compile_cached(UI_STEADY_SHADER, sizeof(UI_STEADY_SHADER) - 1, "UiSteady", nullptr, nullptr, "around", "cs_5_0", 0, 0, &um_around, &errors))) {
            REVR_LOG(spdlog::warn("[UiSteady] the pass with the interface's own motion did not compile (the scene's motion vectors are used): {}",
                errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
            m_root_um.Reset();
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC um_desc{};
            um_desc.pRootSignature = m_root_um.Get();
            um_desc.CS.pShaderBytecode = um->GetBufferPointer();
            um_desc.CS.BytecodeLength = um->GetBufferSize();
            const bool main_ok = SUCCEEDED(device->CreateComputePipelineState(&um_desc, IID_PPV_ARGS(&m_pso_um)));
            um_desc.CS.pShaderBytecode = um_around->GetBufferPointer();
            um_desc.CS.BytecodeLength = um_around->GetBufferSize();

            if (!main_ok || FAILED(device->CreateComputePipelineState(&um_desc, IID_PPV_ARGS(&m_pso_around_um)))) {
                REVR_LOG(spdlog::warn("[UiSteady] no pipeline state for the pass with the interface's own motion (the scene's motion vectors are used)"));
                m_root_um.Reset();
                m_pso_um.Reset();
                m_pso_around_um.Reset();
            }
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[UiSteady] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_pso_around.Reset();
        m_pso_um.Reset();
        m_pso_around_um.Reset();
        m_root_um.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[UiSteady] ready"));
    return true;
}

// [fix94] the two small images of the blocks worked on (R32_UINT, a texel per block of the map; they rest in UNORDERED_ACCESS)
bool UiSteady::ensure_blocks(ID3D12Device* device, uint32_t bw, uint32_t bh) {
    if (m_act[0] != nullptr && m_act[1] != nullptr && m_act_w == bw && m_act_h == bh) {
        return true;
    }

    for (auto& image : m_act) {
        if (image != nullptr) {
            m_retired.emplace_back(std::move(image), 0u);
        }

        image.Reset();
    }

    m_act_w = 0;
    m_act_h = 0;

    if (bw == 0 || bh == 0) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = bw;
    desc.Height = bh;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_act[i])))) {
            m_act[0].Reset();
            m_act[1].Reset();
            return false;
        }

        m_act[i]->SetName(i == 0 ? L"RE4VR steady interface blocks 0" : L"RE4VR steady interface blocks 1");
    }

    m_act_w = bw;
    m_act_h = bh;
    m_act_cur = 0;
    // (new images hold nothing: the whole image for two dispatches, as after a reset)
    m_full_left = 2;
    m_around_last = false;
    return true;
}

bool UiSteady::ensure_images(ID3D12Device* device, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES color_state) {
    bool have = m_w == w && m_h == h && m_color_state == color_state;

    for (const auto& pair : m_image) {
        for (const auto& image : pair) {
            have = have && image != nullptr;
        }
    }

    if (have) {
        return true;
    }

    // (a new size: lists in flight may still read the old images, they are kept for some frames)
    for (auto& pair : m_image) {
        for (auto& image : pair) {
            if (image != nullptr) {
                m_retired.emplace_back(std::move(image), 0u);
            }

            image.Reset();
        }
    }

    m_valid = false;
    m_w = 0;
    m_h = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    static const wchar_t* const NAMES[2][3] = {{L"RE4VR steady interface colour 0", L"RE4VR steady interface hud 0", L"RE4VR steady interface fin 0"},
        {L"RE4VR steady interface colour 1", L"RE4VR steady interface hud 1", L"RE4VR steady interface fin 1"}};

    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            // (the colour rests where the frame's colour rests: the eye pass's transition of it; the copies' results in a
            // read state)
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, j == 0 ? color_state : COPIES_STATE, nullptr,
                    IID_PPV_ARGS(&m_image[i][j])))) {
                for (auto& pair : m_image) {
                    for (auto& image : pair) {
                        image.Reset();
                    }
                }

                m_last_error = "could not create the images";
                return false;
            }

            m_image[i][j]->SetName(NAMES[i][j]);
        }
    }

    m_w = w;
    m_h = h;
    m_color_state = color_state;
    m_cur = 0;
    return true;
}

bool UiSteady::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state, ID3D12Resource* hud,
    ID3D12Resource* fin, ID3D12Resource* motion, D3D12_RESOURCE_STATES motion_state, const GuiMap* gui_map, const Params& p) {
    if (device == nullptr || cmd == nullptr || color == nullptr || hud == nullptr || fin == nullptr || motion == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto cd = color->GetDesc();
    const auto hd = hud->GetDesc();
    const auto fd = fin->GetDesc();
    const auto md = motion->GetDesc();
    const auto color_format = srv_format_for(cd.Format);

    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || hd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        fd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cd.SampleDesc.Count != 1 ||
        hd.SampleDesc.Count != 1 || fd.SampleDesc.Count != 1 || md.SampleDesc.Count != 1 || cd.Width == 0 || cd.Height == 0 || md.Width == 0 ||
        md.Height == 0) {
        m_last_error = "unsupported input";
        return false;
    }

    // (one size: a sample of the three is the same point of the frame)
    if (hd.Width != cd.Width || hd.Height != cd.Height || fd.Width != cd.Width || fd.Height != cd.Height ||
        srv_format_for(hd.Format) != srv_format_for(fd.Format)) {
        m_last_error = "images of different size or copies of different format";
        return false;
    }

    const uint32_t w = (uint32_t)cd.Width;
    const uint32_t h = cd.Height;

    if (!ensure_images(device, w * SCALE, h * SCALE, color_state)) {
        return false;
    }

    for (auto it = m_retired.begin(); it != m_retired.end();) {
        it = ++it->second > RETIRE_FRAMES ? m_retired.erase(it) : it + 1;
    }

    const bool reset = p.reset || !m_valid;
    const auto& prev = m_image[m_cur];
    const auto& out = m_image[m_cur ^ 1u];

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    // [UI MOTION fix125 22.11.2026] `main` with the interface's own motion when its block is given: its block is one more
    // SRV (t7) in front of the UAVs
    const bool ui_motion = p.ui_motion && m_pso_um != nullptr && m_pso_around_um != nullptr && m_root_um != nullptr;
    ID3D12PipelineState* const main_pso = ui_motion ? m_pso_um.Get() : m_pso.Get();
    const uint32_t ub = ui_motion ? 8u : 7u; // the first UAV's place in the table
    m_last_ui_motion = ui_motion;

    if (ui_motion) {
        UiMotion::srv(device, p.ui_motion, slot(7));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(0));
    srv.Format = srv_format_for(hd.Format);
    device->CreateShaderResourceView(hud, &srv, slot(1));
    srv.Format = srv_format_for(fd.Format);
    device->CreateShaderResourceView(fin, &srv, slot(2));
    srv.Format = srv_format_for(md.Format);
    device->CreateShaderResourceView(motion, &srv, slot(3));
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    for (uint32_t j = 0; j < 3; ++j) {
        device->CreateShaderResourceView(prev[j].Get(), &srv, slot(4 + j));
        device->CreateUnorderedAccessView(out[j].Get(), nullptr, &uav, slot(ub + j));
    }

    // the map of this frame's copies (R32_UINT, resting in UNORDERED_ACCESS), if it is theirs: its blocks get marked too
    ID3D12Resource* map = gui_map != nullptr && gui_map->map() != nullptr && gui_map->gui_w() == w && gui_map->gui_h() == h && gui_map->map_w() != 0 &&
                                  gui_map->map_h() != 0 && gui_map->stamp() != 0
                              ? gui_map->map()
                              : nullptr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC map_uav{};
    map_uav.Format = DXGI_FORMAT_R32_UINT;
    map_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(map, nullptr, &map_uav, slot(ub + 3));

    // [fix94 19.11.2026] Only the blocks around the interface (Params::only_around). After a reset, when the option comes
    // on and with new images the whole image is worked on for two dispatches (both sets of images written everywhere);
    // then the blocks `around` names, and the history counts only from blocks the dispatch before worked on.
    const bool around = p.only_around && map != nullptr && m_pso_around != nullptr && ensure_blocks(device, gui_map->map_w(), gui_map->map_h());

    if (reset || (around && !m_around_last)) {
        m_full_left = 2;
    }

    const bool whole = !around || m_full_left > 0;
    const uint32_t seq_prev = m_seq;
    const bool check_history = m_partial_last && !reset && m_act[m_act_cur] != nullptr;
    ID3D12Resource* act_prev = (around || check_history) ? m_act[m_act_cur].Get() : nullptr;
    ID3D12Resource* act_out = around ? m_act[m_act_cur ^ 1u].Get() : nullptr;
    device->CreateUnorderedAccessView(act_out, nullptr, &map_uav, slot(ub + 4));
    device->CreateUnorderedAccessView(act_prev, nullptr, &map_uav, slot(ub + 5));

    if (around) {
        m_seq = m_seq + 1 != 0 ? m_seq + 1 : 1;
    }

    const auto UAV_STATE = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto color_read = compute_read_state(color_state);
    const auto motion_read = compute_read_state(motion_state);

    // (the frame's hud and fin rest in COMMON: the compute read promotes them implicitly, as in UiClean and GuiMap)
    barrier(cmd, color, color_state, color_read);
    barrier(cmd, motion, motion_state, motion_read);

    for (uint32_t j = 0; j < 3; ++j) {
        const auto rest = j == 0 ? color_state : COPIES_STATE;
        barrier(cmd, prev[j].Get(), rest, j == 0 ? color_read : COPIES_STATE);
        barrier(cmd, out[j].Get(), rest, UAV_STATE);
    }

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(ui_motion ? m_root_um.Get() : m_root_sig.Get());
    cmd->SetPipelineState(main_pso);
    cmd->SetComputeRootDescriptorTable(0, gpu);

    struct Constants {
        uint32_t size[2];
        uint32_t out_size[2];
        uint32_t mv_size[2];
        float jit[2];
        float mv_scale[2];
        uint32_t flags;
        float w_max;
        float inv_2s2;
        float inv_half2;
        uint32_t map_size[2];
        uint32_t stamp;
        uint32_t seq;
        uint32_t seq_prev;
        uint32_t hold;
    } k{};
    static_assert(sizeof(Constants) == 20 * 4);
    const float sigma = std::clamp(p.sigma, 0.1f, 1.0f);
    const float half = (std::max)(p.motion_half, 0.25f);
    k.size[0] = w;
    k.size[1] = h;
    k.out_size[0] = m_w;
    k.out_size[1] = m_h;
    k.mv_size[0] = (uint32_t)md.Width;
    k.mv_size[1] = md.Height;
    k.jit[0] = std::clamp(p.jitter_x, -2.0f, 2.0f);
    k.jit[1] = std::clamp(p.jitter_y, -2.0f, 2.0f);
    k.mv_scale[0] = p.mv_scale_x;
    k.mv_scale[1] = p.mv_scale_y;
    k.flags = (reset ? 1u : 0u) | (is_srgb(color_format) ? 2u : 0u) | (map != nullptr ? 4u : 0u) | (around && !whole ? 8u : 0u) |
              (check_history ? 32u : 0u) | (around && whole ? 64u : 0u);
    k.seq = m_seq;
    k.seq_prev = seq_prev;
    k.hold = p.around_hold != 0 ? p.around_hold : AROUND_HOLD;
    k.map_size[0] = map != nullptr ? gui_map->map_w() : 1u;
    k.map_size[1] = map != nullptr ? gui_map->map_h() : 1u;
    k.stamp = map != nullptr ? gui_map->stamp() : 0u;
    k.w_max = std::clamp(p.weight_max, 0.0f, 64.0f);
    k.inv_2s2 = 1.0f / (2.0f * sigma * sigma);
    k.inv_half2 = 1.0f / (half * half);
    cmd->SetComputeRoot32BitConstants(1, 20, &k, 0);

    if (around) {
        // which blocks: from the map as GuiMap left it for this frame, before `main` marks blocks of its own
        cmd->SetPipelineState(ui_motion ? m_pso_around_um.Get() : m_pso_around.Get());
        cmd->Dispatch((gui_map->map_w() + 7) / 8, (gui_map->map_h() + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER act_done{};
        act_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        act_done.UAV.pResource = nullptr; // (every UAV: `main` reads these blocks and marks the map this pass has read)
        cmd->ResourceBarrier(1, &act_done);
        cmd->SetPipelineState(main_pso);
        m_act_cur ^= 1u;
    }

    cmd->Dispatch((m_w + 7) / 8, (m_h + 7) / 8, 1);
    m_around_last = around;
    m_partial_last = around && !whole;

    if (m_full_left > 0) {
        --m_full_left;
    }

    D3D12_RESOURCE_BARRIER uav_done[4]{};

    for (uint32_t j = 0; j < 3; ++j) {
        uav_done[j].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_done[j].UAV.pResource = out[j].Get();
    }

    uav_done[3].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[3].UAV.pResource = map;
    cmd->ResourceBarrier(map != nullptr ? 4 : 3, uav_done);

    for (uint32_t j = 0; j < 3; ++j) {
        const auto rest = j == 0 ? color_state : COPIES_STATE;
        barrier(cmd, out[j].Get(), UAV_STATE, rest);
        barrier(cmd, prev[j].Get(), j == 0 ? color_read : COPIES_STATE, rest);
    }

    barrier(cmd, motion, motion_read, motion_state);
    barrier(cmd, color, color_read, color_state);

    m_cur ^= 1u;
    m_valid = true;
    m_last_error = "";
    return true;
}

void UiSteady::reset() {
    m_pso.Reset();
    m_pso_around.Reset();
    m_pso_um.Reset(); // [UI MOTION fix125]
    m_pso_around_um.Reset();
    m_root_um.Reset();
    m_last_ui_motion = false;
    m_act[0].Reset();
    m_act[1].Reset();
    m_act_w = 0;
    m_act_h = 0;
    m_around_last = false;
    m_partial_last = false;
    m_full_left = 0;
    m_root_sig.Reset();
    m_heap.Reset();

    for (auto& pair : m_image) {
        for (auto& image : pair) {
            image.Reset();
        }
    }

    m_retired.clear();
    m_w = 0;
    m_h = 0;
    m_color_state = D3D12_RESOURCE_STATE_COMMON;
    m_cur = 0;
    m_valid = false;
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [MONO PERIPHERY 27.10.2026] UiClean, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char UI_CLEAN_SHADER[] = R"(
Texture2D<float4>   g_color : register(t0); // the view's colour after the output pass (the upscaler's input)
Texture2D<float4>   g_hud   : register(t1); // engine copy without GUI (PostMainTarget, linear, before the output pass)
Texture2D<float4>   g_fin   : register(t2); // engine copy with GUI (same place in the frame)
Texture2D<float4>   g_curve : register(t3); // UiCurve: PostMainTarget value -> output colour, per channel
RWTexture2D<float4> g_out   : register(u0);
RWTexture2D<uint>   g_map   : register(u1); // [LIGHTS 11.11.2026] GuiMap (read only here)
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    uint g_w;
    uint g_h;
    uint g_flags; // 1 = the curve is measured (else the plain SRGB curve), 2 = interface found from the colour itself,
                  // 4 = [LIGHTS 11.11.2026] g_map / g_stamp given
    uint g_stamp; // GuiMap's stamp of this frame
};

// [GPU 29.10.2026 round 3] the curve table read from shared memory (main_lds: the fovea views, where every pixel needs it)
#ifdef CURVE_LDS
groupshared float4 s_curve[80];
#define CURVE_AT(i) s_curve[i]
#else
#define CURVE_AT(i) g_curve.Load(int3(i, 0, 0))
#endif

float to_srgb1(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// the engine's output pass as measured on the shared wide view (bins of a quarter stop from 2^-14)
float curve_of(float x, int c) {
    if (!(x > 0.0)) {
        return 0.0;
    }

    if ((g_flags & 1) == 0) {
        return to_srgb1(x);
    }

    const float f = (log2(x) + 14.0) * 4.0 - 0.5;

    if (f <= 0.0) {
        return CURVE_AT(0)[c] * saturate(x * 15024.0); // below the first bin: towards black
    }

    const float fc = min(f, 79.0);
    const int b0 = int(fc);
    const int b1 = min(b0 + 1, 79);
    return lerp(CURVE_AT(b0)[c], CURVE_AT(b1)[c], fc - float(b0));
}

float3 curve3(float3 x) {
    return float3(curve_of(x.r, 0), curve_of(x.g, 1), curve_of(x.b, 2));
}

// [HIGHLIGHTS 11.11.2026] The curve is measured on the shared wide view; above the highest bin measured there (the table's
// alpha) it is only held, while the engine's output keeps rising. The fovea views see small highlights (a torch or a
// lamp on a glossy floor) brighter than anything the wide view holds: their colour came out above the held value and
// counted as interface (the fovea view's own, jittered pixels over the upscaled fovea: sparkling dots). Above the measured
// range a channel only counts where the colour is darker than the curve (an interface pixel over a highlight).
float3 color_off_curve(float3 color, float3 hud, float3 pred) {
    float3 d = abs(color - pred);

    if ((g_flags & 1) != 0) {
        const float top = CURVE_AT(0).a;

        [unroll] for (int c = 0; c < 3; ++c) {
            if (hud[c] > 0.0 && (log2(hud[c]) + 14.0) * 4.0 - 0.5 > top) {
                d[c] = max(pred[c] - color[c], 0.0);
            }
        }
    }

    return d;
}

// [SPECKS 11.11.2026] Light added by the interface, found from a fovea view's colour: only where the colour is above the
// view without GUI by more than the threshold (above the measured curve range nothing counts, see color_off_curve), and
// only where at least two of the 8 pixels around are found too. The interface draws strokes, icons and panels; what the
// per-channel curve misses on the scene (a glint on a shoe, a lit hair strand, a floor speck: one or two pixels that
// change with the jitter) stands alone and is dropped.
float light_mask_of(float3 color, float3 hud, float3 pred) {
    float3 d = color - pred;

    if ((g_flags & 1) != 0) {
        const float top = CURVE_AT(0).a;

        [unroll] for (int c = 0; c < 3; ++c) {
            if (hud[c] > 0.0 && (log2(hud[c]) + 14.0) * 4.0 - 0.5 > top) {
                d[c] = 0.0;
            }
        }
    }

    return smoothstep(0.045, 0.11, max(d.r, max(d.g, d.b)));
}

void clean(uint2 id) {
    const float2 uv = (float2(id) + 0.5) / float2(g_w, g_h);
    const float4 c = g_color.Load(int3(id, 0));

    // [NO INTERFACE fix118 21.11.2026] the view whose two copies the GuiMap was made from, with this frame's map (flags: 4
    // without 2): when the map's whole-frame texel (see [LIGHTS] below; written only by maps wider than 64 blocks) does
    // not hold this frame's stamp, no texel of the two copies differs, so both samples below would come out the same and
    // the pixel would leave at "both copies the same" with its colour. It leaves here, before the copies are read: the
    // same image, two reads per pixel less in every frame without interface (RE4 test of fix116: this pass 0.018 ms).
    if ((g_flags & 6) == 4) {
        uint fw;
        uint fh;
        g_map.GetDimensions(fw, fh);

        if (fw > 64 && g_map[uint2(fw - 1, fh - 1)] != g_stamp) {
            g_out[id] = c;
            return;
        }

        // [BLOCKS fix120 22.11.2026] and in a frame with interface, per block (flags 8: the copies have the image's size, so
        // the pixel is the copies' texel and its block of 8 x 8 texels the map's): GuiMap stamps a block when a texel of it
        // or of a block next to it differs, so in a block without the stamp the two samples come out the same as well.
        // RE4 test of fix118: this pass 0.019 - 0.020 ms without interface, 0.027 - 0.028 with the weapon HUD.
        if ((g_flags & 8) != 0 && g_map[id >> 3] != g_stamp) {
            g_out[id] = c;
            return;
        }
    }

    const float3 hud = g_hud.SampleLevel(g_lin, uv, 0).rgb;
    float m = 0.0;

    // [LIGHTS 11.11.2026] the wide view drew no interface in this frame (the last texel of the GuiMap's extra row does
    // not hold this frame's stamp): neither did this view. Coloured lights (a red lamp) come out of the engine's output
    // pass other than the per-channel curve predicts and would count as interface otherwise.
    if ((g_flags & 6) == 6) {
        uint mw;
        uint mh;
        g_map.GetDimensions(mw, mh);

        if (g_map[uint2(mw - 1, mh - 1)] != g_stamp) {
            g_out[id] = c;
            return;
        }
    }

    if ((g_flags & 2) == 0) {
        const float3 fin = g_fin.SampleLevel(g_lin, uv, 0).rgb;

        // [GPU 29.10.2026] both copies the same here: no interface (the mask below would be exactly 0)
        if (all(fin == hud)) {
            g_out[id] = c;
            return;
        }

        // the two engine copies of the same image, both through the output curve (darks count as the eye sees them)
        const float3 d = abs(curve3(fin) - curve3(hud));
        m = smoothstep(0.03, 0.08, max(d.r, max(d.g, d.b)));
    }

    const float3 scene = curve3(hud); // the view without GUI, as the output pass shows it

    if ((g_flags & 2) != 0) {
        // the colour itself against the view without GUI: needs no copy with GUI (the fovea layers' one is not
        // reliable: the engine reuses that target for the next view before it is taken, test 27.10.2026)
        m = light_mask_of(c.rgb, hud, scene); // [SPECKS 11.11.2026]

        if (m > 0.0) {
            float n = 0.0;

            [unroll] for (int dy = -1; dy <= 1; ++dy) {
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 || dy != 0) {
                        const int2 q = clamp(int2(id) + int2(dx, dy), int2(0, 0), int2(g_w, g_h) - 1);
                        const float2 quv = (float2(q) + 0.5) / float2(g_w, g_h);
                        const float3 qh = g_hud.SampleLevel(g_lin, quv, 0).rgb;
                        n += light_mask_of(g_color.Load(int3(q, 0)).rgb, qh, curve3(qh));
                    }
                }
            }

            m *= saturate(n - 1.0);
        }
    }

    // [TRANSLUCENT 11.11.2026] from the colour: only what the interface added is taken out (min); a translucent panel's
    // darkening stays, the eye pass then only adds the interface's light (EyeCompose's ui_light)
    g_out[id] = float4(lerp(c.rgb, (g_flags & 2) != 0 ? min(c.rgb, scene) : scene, m), c.a);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_w || id.y >= g_h) {
        return;
    }

    clean(id.xy);
}

#ifdef CURVE_LDS
// 16 x 16 pixels per group, the curve table loaded once into shared memory
[numthreads(16, 16, 1)]
void main_lds(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 80) {
        s_curve[gi] = g_curve.Load(int3(int(gi), 0, 0));
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= g_w || id.y >= g_h) {
        return;
    }

    clean(id.xy);
}
#endif
)";
} // namespace

bool UiClean::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(UI_CLEAN_SHADER, sizeof(UI_CLEAN_SHADER) - 1, "UiClean", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[UiClean] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    // [GPU 29.10.2026 round 3] the fovea views' pass with the curve table in shared memory (optional)
    ComPtr<ID3DBlob> lds{};
    const D3D_SHADER_MACRO curve_lds[]{{"CURVE_LDS", "1"}, {nullptr, nullptr}};

    if (FAILED(compile_cached(UI_CLEAN_SHADER, sizeof(UI_CLEAN_SHADER) - 1, "UiClean", curve_lds, nullptr, "main_lds", "cs_5_0", 0, 0, &lds, &errors))) {
        REVR_LOG(spdlog::error("[UiClean] shared-memory pass not compiled: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        lds.Reset();
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4; // colour, hudless, final, curve
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; // out, [LIGHTS 11.11.2026] GuiMap
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[UiClean] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[UiClean] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    if (lds != nullptr) {
        pso_desc.CS.pShaderBytecode = lds->GetBufferPointer();
        pso_desc.CS.BytecodeLength = lds->GetBufferSize();

        if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_lds)))) {
            m_pso_lds.Reset();
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[UiClean] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_pso_lds.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[UiClean] ready"));
    return true;
}

bool UiClean::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
    ID3D12Resource* hudless, ID3D12Resource* final_tex, ID3D12Resource* out, ID3D12Resource* curve, bool mask_from_color, bool round3,
    ID3D12Resource* gui_map, uint32_t gui_stamp) {
    if (device == nullptr || cmd == nullptr || color == nullptr || hudless == nullptr || out == nullptr || (final_tex == nullptr && !mask_from_color)) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto cdesc = color->GetDesc();
    const auto odesc = out->GetDesc();
    // the colour is read and written as its display-encoded bits (an SRGB view would decode them)
    const auto color_format = engine_copy_uav_format(cdesc.Format);
    const auto uav_format = uav_format_for(odesc.Format);
    const auto hud_format = srv_format_for(hudless->GetDesc().Format);
    // without a copy with GUI the copy without GUI stands in (never read: the mask comes from the colour)
    ID3D12Resource* fin = final_tex != nullptr ? final_tex : hudless;
    const auto fin_format = srv_format_for(fin->GetDesc().Format);

    if (color_format == DXGI_FORMAT_UNKNOWN || uav_format == DXGI_FORMAT_UNKNOWN || odesc.Width != cdesc.Width || odesc.Height != cdesc.Height) {
        m_last_error = "unsupported format or size";
        return false;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(0));
    srv.Format = hud_format;
    device->CreateShaderResourceView(hudless, &srv, slot(1));
    srv.Format = fin_format;
    device->CreateShaderResourceView(fin, &srv, slot(2));

    // the curve table (UiCurve, resting in NON_PIXEL_SHADER_RESOURCE); without it the shader uses the SRGB curve
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(curve, &srv, slot(3));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = uav_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(4));

    // [LIGHTS 11.11.2026] the GuiMap (a null view without it: the shader does not read it then)
    D3D12_UNORDERED_ACCESS_VIEW_DESC map_uav{};
    map_uav.Format = DXGI_FORMAT_R32_UINT;
    map_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(gui_map, nullptr, &map_uav, slot(5));

    if (gui_map != nullptr) {
        // written by GuiMap::dispatch earlier in the frame
        D3D12_RESOURCE_BARRIER map_written{};
        map_written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        map_written.UAV.pResource = gui_map;
        cmd->ResourceBarrier(1, &map_written);
    }

    // the engine copies (hudless / final) rest in COMMON: a compute read promotes them implicitly (as the interface
    // restore does every frame), no barrier
    const auto color_read = compute_read_state(color_state);

    barrier(cmd, color, color_state, color_read);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    // round 3: the fovea views (every pixel through the curve) with the table in shared memory
    const bool lds = round3 && mask_from_color && m_pso_lds != nullptr;
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(lds ? m_pso_lds.Get() : m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    uint32_t constants[4]{};
    constants[0] = (uint32_t)odesc.Width;
    constants[1] = odesc.Height;
    constants[2] = (curve != nullptr ? 1u : 0u) | (mask_from_color ? 2u : 0u) | (gui_map != nullptr ? 4u : 0u);

    // [BLOCKS fix120] the view the map was made from (see the shader): per block only when the pixel is the copies' texel
    if (gui_map != nullptr && !mask_from_color) {
        const auto hdesc = hudless->GetDesc();
        const auto fdesc = fin->GetDesc();

        if (hdesc.Width == odesc.Width && hdesc.Height == odesc.Height && fdesc.Width == odesc.Width && fdesc.Height == odesc.Height) {
            constants[2] |= 8u;
        }
    }

    constants[3] = gui_stamp;
    cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);

    if (lds) {
        cmd->Dispatch(((uint32_t)odesc.Width + 15) / 16, (odesc.Height + 15) / 16, 1);
    } else {
        cmd->Dispatch(((uint32_t)odesc.Width + 7) / 8, (odesc.Height + 7) / 8, 1);
    }

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &uav_done);

    barrier(cmd, color, color_read, color_state);

    m_last_error = "";
    return true;
}

void UiClean::reset() {
    m_pso_lds.Reset();
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [MONO UI 27.10.2026] MonoUi, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char MONO_UI_SHADER[] = R"(
Texture2D<float4>   g_eye   : register(t0); // this eye's image (periphery + fovea)
Texture2D<float4>   g_c_col : register(t1); // shared wide view: colour after the output pass
Texture2D<float4>   g_c_hud : register(t2); //   copy without GUI
Texture2D<float4>   g_c_fin : register(t3); //   copy with GUI
Texture2D<float4>   g_f_col : register(t4); // this eye's fovea view: colour after the output pass
Texture2D<float4>   g_f_hud : register(t5);
Texture2D<float4>   g_f_fin : register(t6);
Texture2D<float4>   g_curve : register(t7); // UiCurve: PostMainTarget value -> output colour, per channel
RWTexture2D<float4> g_out   : register(u0);
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    uint2  g_out_size;
    uint   g_flags;        // 1 = wide view GUI valid, 2 = fovea GUI valid, 4 = debug tint, 8 / 16 = encode the wide / fovea colour,
                           // 32 = output curve measured, 64 = the fovea's interface from its colour (no copy with GUI),
                           // 256 = post pass (g_post)
    uint   g_anchor_count;
    float4 g_eye_tan;      // l, r, b, t
    float4 g_wide_tan;
    float4 g_rect;         // x, y, w, h (eye pixels)
    float  g_eye_x;
    float  g_panel_depth;
    float  g_feather;
    float  g_pad;
    float4 g_post;         // flag 256: sharpness, saturation, brightness exponent, contrast
    float4 g_anchors[16];  // tangent x, tangent y, depth, radius (tangent; > 0: an element at something in the world, whose
                           // pixels belong to it only; < 0: a panel at another distance)
    float4 g_anchor_ext[16]; // [ITEM BEAMS 03.11.2026] x = reach across, y = above, z = below (tangents), w = 1: item beam
    float4 g_post2;        // [EDGE SOFTEN 11.11.2026] flag 256: edge softening weight (0 = off), edge threshold
};

// [EDGE SOFTEN 11.11.2026] 1 on high-contrast edges (luma range of a pixel and its 4 neighbours from t up, full at t + 0.1),
// 0 on flat areas and fine texture
float edge_weight(float3 c, float3 n0, float3 n1, float3 n2, float3 n3, float t) {
    const float3 W = float3(0.2126, 0.7152, 0.0722);
    const float lc = dot(c, W), l0 = dot(n0, W), l1 = dot(n1, W), l2 = dot(n2, W), l3 = dot(n3, W);
    const float range = max(lc, max(max(l0, l1), max(l2, l3))) - min(lc, min(min(l0, l1), min(l2, l3)));
    return smoothstep(t, t + 0.1, range);
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float to_srgb1(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// [UI CURVE] the engine's output pass as measured on the shared wide view (bins of a quarter stop from 2^-14)
float curve_of(float x, int c) {
    if (!(x > 0.0)) {
        return 0.0;
    }

    if ((g_flags & 32) == 0) {
        return to_srgb1(x);
    }

    const float f = (log2(x) + 14.0) * 4.0 - 0.5;

    if (f <= 0.0) {
        return g_curve.Load(int3(0, 0, 0))[c] * saturate(x * 15024.0);
    }

    const float fc = min(f, 79.0);
    const int b0 = int(fc);
    const int b1 = min(b0 + 1, 79);
    return lerp(g_curve.Load(int3(b0, 0, 0))[c], g_curve.Load(int3(b1, 0, 0))[c], fc - float(b0));
}

float3 curve3(float3 x) {
    return float3(curve_of(x.r, 0), curve_of(x.g, 1), curve_of(x.b, 2));
}

// interface pixels of a view: its copies with and without GUI differ (both through the output curve, so the darks
// count as the eye sees them)
float mask_of(Texture2D<float4> hud, Texture2D<float4> fin, float2 uv) {
    const float3 d = abs(curve3(fin.SampleLevel(g_lin, uv, 0).rgb) - curve3(hud.SampleLevel(g_lin, uv, 0).rgb));
    return smoothstep(0.03, 0.08, max(d.r, max(d.g, d.b)));
}

// [HIGHLIGHTS 11.11.2026] The curve is measured on the shared wide view; above the highest bin measured there (the table's
// alpha) it is only held, while the engine's output keeps rising. The fovea views see small highlights (a torch or a
// lamp on a glossy floor) brighter than anything the wide view holds: their colour came out above the held value and
// counted as interface (the fovea view's own, jittered pixels over the upscaled fovea: sparkling dots). Above the measured
// range a channel only counts where the colour is darker than the curve (an interface pixel over a highlight).
float3 color_off_curve(float3 color, float3 hud, float3 pred) {
    float3 d = abs(color - pred);

    if ((g_flags & 32) != 0) {
        const float top = g_curve.Load(int3(0, 0, 0)).a;

        [unroll] for (int c = 0; c < 3; ++c) {
            if (hud[c] > 0.0 && (log2(hud[c]) + 14.0) * 4.0 - 0.5 > top) {
                d[c] = max(pred[c] - color[c], 0.0);
            }
        }
    }

    return d;
}

// [UI CURVE] interface pixels from the view's colour after the output pass against its copy without GUI
float mask_from_color(Texture2D<float4> hud, float3 color, float2 uv) {
    const float3 h = hud.SampleLevel(g_lin, uv, 0).rgb;
    const float3 d = color_off_curve(color, h, curve3(h)); // [HIGHLIGHTS 11.11.2026]
    return smoothstep(0.045, 0.11, max(d.r, max(d.g, d.b)));
}

// [TRANSLUCENT 11.11.2026] What the interface added to a fovea view's pixel: its colour above the view without GUI (as the
// output pass shows it), per channel. Only that is put over the upscaled fovea; where the interface darkened the view (a
// panel's translucent background: the opened map) the upscaled fovea already has it (UiClean keeps the darkening in the
// upscaler's input). Before, the fovea view's own pixel replaced the upscaled one wherever the mask counted: behind a
// translucent panel the view's highlights (a torch on the floor) passed the mask frame by frame, and its jittered pixels
// sparkled through the panel.
float3 ui_light(Texture2D<float4> hud, float3 color, float2 uv) {
    return max(color - curve3(hud.SampleLevel(g_lin, uv, 0).rgb), 0.0);
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [SPECKS 11.11.2026] Light added by the interface, found from a fovea view's colour: only where the colour is above the
// view without GUI by more than the threshold (above the measured curve range nothing counts, see color_off_curve), and
// only where at least two of the 8 pixels around are found too. The interface draws strokes, icons and panels; what the
// per-channel curve misses on the scene (a glint on a shoe, a lit hair strand, a floor speck: one or two pixels that
// change with the jitter) stands alone and is dropped.
float light_mask_of(float3 color, float3 h) {
    const float3 pred = curve3(h);
    const float top = (g_flags & 32) != 0 ? g_curve.Load(int3(0, 0, 0)).a : 1e9;
    float3 d = color - pred;

    [unroll] for (int c = 0; c < 3; ++c) {
        if (h[c] > 0.0 && (log2(h[c]) + 14.0) * 4.0 - 0.5 > top) {
            d[c] = 0.0;
        }
    }

    return smoothstep(0.045, 0.11, max(d.r, max(d.g, d.b)));
}

float fovea_light_mask(float3 cf, float2 fuv) {
    const float m = light_mask_of(cf, g_f_hud.SampleLevel(g_lin, fuv, 0).rgb);

    if (!(m > 0.0)) {
        return 0.0;
    }

    uint w;
    uint h;
    g_f_col.GetDimensions(w, h);
    const float2 t = 1.0 / float2(w, h);
    float n = 0.0;

    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) {
                const float2 uv = fuv + t * float2(dx, dy);
                float3 col = g_f_col.SampleLevel(g_lin, uv, 0).rgb;

                if ((g_flags & 16) != 0) {
                    col = to_srgb(col);
                }

                n += light_mask_of(col, g_f_hud.SampleLevel(g_lin, uv, 0).rgb);
            }
        }
    }

    return m * saturate(n - 1.0);
}

bool inside01(float2 uv) { return all(uv >= 0.0) && all(uv <= 1.0); }

// where the wide view sees the point this eye sees along (tx, ty) at depth z (the cameras differ by a shift along x)
float2 wide_uv(float tx, float ty, float z) {
    const float tcx = tx + g_eye_x / z;
    return float2((tcx - g_wide_tan.x) / (g_wide_tan.y - g_wide_tan.x), (g_wide_tan.w - ty) / (g_wide_tan.w - g_wide_tan.z));
}

// [ITEM BEAMS 03.11.2026] the green beams over items and the item icon as a column: as in EyeCompose (see there; this
// pass has no GuiMap, so a beam stays where the wide view drew it)
bool beam_colour(float2 uv) {
    const float3 d = curve3(g_c_fin.SampleLevel(g_lin, uv, 0).rgb) - curve3(g_c_hud.SampleLevel(g_lin, uv, 0).rgb);
    return d.g > 0.01 && d.g - max(d.r, d.b) > 0.08 * d.g + 0.003;
}

uint anchor_kind(uint i) {
    const float w = g_anchor_ext[i].w;
    return w > 1.5 ? 2u : (w > 0.5 ? 1u : 0u);
}

bool in_area(uint i, float wx, float wy) {
    const float4 a = g_anchors[i];

    if (anchor_kind(i) == 0u) {
        return length(float2(wx - a.x, wy - a.y)) <= abs(a.w);
    }

    const float4 e = g_anchor_ext[i];
    const float dy = wy - a.y;
    return abs(wx - a.x) <= e.x && dy <= e.y && dy >= -e.z;
}

int beam_owner(float wx, float wy) {
    int owner = -1;
    float best = 1e9;

    [loop] for (uint j = 0; j < g_anchor_count; ++j) {
        if (anchor_kind(j) == 1u) {
            const float dx = abs(wx - g_anchors[j].x);

            if (dx < best && in_area(j, wx, wy)) {
                best = dx;
                owner = int(j);
            }
        } else if (g_anchors[j].w > 0.0 && in_area(j, wx, wy)) {
            return -2;
        }
    }

    return owner;
}

// the wide view's interface at eye pixel p (centre of a pixel or of a block): coverage, colour in cc
float centre_ui(float2 p, out float3 cc) {
    cc = 0.0;

    if ((g_flags & 1) == 0) {
        return 0.0;
    }

    const float2 uv = p / float2(g_out_size);
    const float tx = lerp(g_eye_tan.x, g_eye_tan.y, uv.x);
    const float ty = lerp(g_eye_tan.w, g_eye_tan.z, uv.y);
    float z = g_panel_depth;
    bool anchored = false;

    // an anchored element (nearest first) claims the pixels in its area where it has interface
    [loop] for (uint i = 0; i < g_anchor_count; ++i) {
        const float4 a = g_anchors[i];
        const float tcx = tx + g_eye_x / a.z;

        if (anchor_kind(i) == 1u) {
            continue; // an item beam: below
        }

        if (in_area(i, tcx, ty)) {
            const float2 auv = wide_uv(tx, ty, a.z);

            if (inside01(auv) && mask_of(g_c_hud, g_c_fin, auv) > 0.02) {
                z = a.z;
                anchored = true;
                break;
            }
        }
    }

    // [ITEM BEAMS 03.11.2026] no anchored element here: a beam pixel at its item's depth
    if (!anchored) {
        [loop] for (uint k = 0; k < g_anchor_count; ++k) {
            if (anchor_kind(k) != 1u) {
                continue;
            }

            const float4 a = g_anchors[k];
            const float tcx = tx + g_eye_x / a.z;

            if (!in_area(k, tcx, ty)) {
                continue;
            }

            const float2 auv = wide_uv(tx, ty, a.z);

            if (inside01(auv) && mask_of(g_c_hud, g_c_fin, auv) > 0.02 && beam_colour(auv) && beam_owner(tcx, ty) == int(k)) {
                z = a.z;
                anchored = true;
                break;
            }
        }
    }

    const float2 cuv = wide_uv(tx, ty, z);

    if (!inside01(cuv)) {
        return 0.0;
    }

    float mc = mask_of(g_c_hud, g_c_fin, cuv);

    // [ANCHOR OWNERSHIP 27.10.2026] seen at the panels' depth, an interface pixel of an element anchored at something
    // in the world would show it a second time beside the place where it belongs (the item icon twice, about the
    // shift between 1 m and its distance, test 27.10.2026): those pixels are left to the anchor
    if (!anchored && mc > 0.0) {
        const float tcp = tx + g_eye_x / z;

        [loop] for (uint j = 0; j < g_anchor_count; ++j) {
            // [ITEM BEAMS 03.11.2026] a beam pixel is shown at its item's depth (its beam's column holds it)
            if (anchor_kind(j) == 1u) {
                if (in_area(j, tcp, ty) && beam_colour(cuv)) {
                    mc = 0.0;
                    break;
                }

                continue;
            }

            if (g_anchors[j].w > 0.0 && in_area(j, tcp, ty)) {
                mc = 0.0;
                break;
            }
        }
    }

    cc = g_c_col.SampleLevel(g_lin, cuv, 0).rgb;

    if ((g_flags & 8) != 0) {
        cc = to_srgb(cc);
    }

    return mc;
}

// the fovea view's interface at eye pixel p (exact for this eye, inside its rectangle): coverage, colour in cf
float fovea_ui(float2 p, out float3 cf) {
    cf = 0.0;

    if ((g_flags & 2) == 0 || g_rect.z <= 0.0 || g_rect.w <= 0.0) {
        return 0.0;
    }

    const float2 fuv = (p - g_rect.xy) / g_rect.zw;

    if (!inside01(fuv)) {
        return 0.0;
    }

    cf = g_f_col.SampleLevel(g_lin, fuv, 0).rgb;

    if ((g_flags & 16) != 0) {
        cf = to_srgb(cf);
    }

    const float m = (g_flags & 64) != 0 ? fovea_light_mask(cf, fuv) : mask_of(g_f_hud, g_f_fin, fuv);
    cf = ui_light(g_f_hud, cf, fuv); // [TRANSLUCENT 11.11.2026]
    return m;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_out_size)) {
        return;
    }

    const float4 eye = g_eye.Load(int3(id.xy, 0));
    const float2 p = float2(id.xy) + 0.5;

    // ---- the wide view's interface, moved to this eye ----
    float3 cc;
    const float mc = centre_ui(p, cc);

    // ---- the fovea view's interface (exact for this eye) inside its rectangle, blended like the fovea image ----
    float mf = 0.0;
    float3 cf = 0.0;
    float wf = 0.0;

    if ((g_flags & 2) != 0 && g_rect.z > 0.0 && g_rect.w > 0.0) {
        // the same weights as the fovea image in the composite: per axis, over the blend width
        const float2 d = min(p - g_rect.xy, g_rect.xy + g_rect.zw - p);

        if (d.x > 0.0 && d.y > 0.0) {
            const float f = clamp(g_feather, 1.0, 0.5 * min(g_rect.z, g_rect.w));
            wf = smoothstep(0.0, 1.0, saturate(d.x / f)) * smoothstep(0.0, 1.0, saturate(d.y / f));
            mf = fovea_ui(p, cf);
        }
    }

    if ((g_flags & 4) != 0) {
        cc = lerp(cc, float3(0.2, 0.4, 1.0), 0.5);
        cf = lerp(cf, float3(0.2, 1.0, 0.3), 0.5);
    }

    // [TRANSLUCENT 11.11.2026] the fovea view's interface adds its light (cf = ui_light) over the upscaled fovea
    const float m = mc * (1.0 - wf);
    const float3 premul = lerp(cc * mc, cf * mf, wf);
    float3 scene = eye.rgb;

    // [POST IN UI 28.10.2026] the post pass of the eye images (Sharpness / Saturation / Brightness / Contrast of the
    // upscaler menu, same maths as d3d12::PostPass) done here, while the image is read and written anyway: the result goes
    // into an image of its own, so an eye image that is submitted again unchanged never gets it twice. The sharpening
    // works on the image under the interface (the interface itself is sharp already), the rest on everything.
    if ((g_flags & 256) != 0 && (g_post.x > 0.0 || g_post2.x > 0.0)) {
        const int2 q = int2(id.xy);
        const int2 hi = int2(g_out_size) - 1;
        const float3 n0 = g_eye.Load(int3(clamp(q + int2(0, -1), 0, hi), 0)).rgb;
        const float3 n1 = g_eye.Load(int3(clamp(q + int2(0, 1), 0, hi), 0)).rgb;
        const float3 n2 = g_eye.Load(int3(clamp(q + int2(-1, 0), 0, hi), 0)).rgb;
        const float3 n3 = g_eye.Load(int3(clamp(q + int2(1, 0), 0, hi), 0)).rgb;
        const float3 blur = (n0 + n1 + n2 + n3) * 0.25;
        // [EDGE SOFTEN 11.11.2026] on edges the neighbours' mean is blended in instead of the sharpening
        const float e = g_post2.x > 0.0 ? edge_weight(scene, n0, n1, n2, n3, g_post2.y) : 0.0;
        scene += clamp(dot(scene - blur, float3(0.2126, 0.7152, 0.0722)) * g_post.x * (1.0 - e), -0.08, 0.08);
        scene = lerp(scene, blur, g_post2.x * e);
    }

    float3 o = scene * (1.0 - m) + premul;

    if ((g_flags & 256) != 0) {
        if (g_post.y != 1.0) {
            const float l = dot(o, float3(0.2126, 0.7152, 0.0722));
            o = max(lerp(float3(l, l, l), o, g_post.y), 0.0);
        }

        if (g_post.z != 1.0) {
            o = pow(max(o, 0.0), g_post.z);
        }

        if (g_post.w != 1.0) {
            o = max((o - 0.5) * g_post.w + 0.5, 0.0);
        }
    }

    g_out[id.xy] = float4(o, eye.a);
}
)";

struct MonoUiConstants {
    uint32_t out_w;
    uint32_t out_h;
    uint32_t flags;
    uint32_t anchor_count;
    float eye_tan[4];
    float wide_tan[4];
    float rect[4];
    float eye_x;
    float panel_depth;
    float feather;
    float pad;
    float post[4]; // sharpness, saturation, brightness exponent, contrast
    float anchors[MonoUi::MAX_ANCHORS][4];
    float anchor_ext[MonoUi::MAX_ANCHORS][4]; // [ITEM BEAMS 03.11.2026]
    float post2[4]; // [EDGE SOFTEN 11.11.2026] edge softening weight, edge threshold, -, -
};
static_assert(sizeof(MonoUiConstants) == 624, "matches the HLSL cbuffer");
static_assert(sizeof(MonoUiConstants) <= 768, "fits a constant buffer slot (MonoUi::CB_SLOT_SIZE)");
} // namespace

bool MonoUi::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(MONO_UI_SHADER, sizeof(MONO_UI_SHADER) - 1, "MonoUi", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[MonoUi] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; // u0 out
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[MonoUi] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[MonoUi] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cb_desc{};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Width = (UINT64)RING_COUNT * CB_SLOT_SIZE;
    cb_desc.Height = 1;
    cb_desc.DepthOrArraySize = 1;
    cb_desc.MipLevels = 1;
    cb_desc.SampleDesc.Count = 1;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap))) ||
        FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))) ||
        FAILED(m_cb->Map(0, &no_read, &mapped)) || mapped == nullptr) {
        REVR_LOG(spdlog::error("[MonoUi] descriptor heap / constant buffer failed"));
        m_last_error = "descriptor heap / constant buffer failed";
        m_pso.Reset();
        m_root_sig.Reset();
        m_heap.Reset();
        m_cb.Reset();
        return false;
    }

    m_cb_mapped = (uint8_t*)mapped;
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[MonoUi] ready"));
    return true;
}

bool MonoUi::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* eye_in, ID3D12Resource* out,
    ID3D12Resource* centre_color, ID3D12Resource* centre_hud, ID3D12Resource* centre_fin,
    ID3D12Resource* fovea_color, ID3D12Resource* fovea_hud, const Params& p) {
    if (device == nullptr || cmd == nullptr || eye_in == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto edesc = eye_in->GetDesc();
    const auto odesc = out->GetDesc();
    const auto eye_format = uav_format_for(edesc.Format);
    const auto out_format = uav_format_for(odesc.Format);

    if (eye_format == DXGI_FORMAT_UNKNOWN || out_format == DXGI_FORMAT_UNKNOWN || edesc.Width != odesc.Width || edesc.Height != odesc.Height) {
        m_last_error = "unsupported format or size";
        return false;
    }

    const bool centre_ok = centre_color != nullptr && centre_hud != nullptr && centre_fin != nullptr;
    const bool fovea_ok = fovea_color != nullptr && fovea_hud != nullptr && p.rect[2] > 0.0f && p.rect[3] > 0.0f;

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    auto make_srv = [&](uint32_t i, ID3D12Resource* r) {
        if (r != nullptr) {
            srv.Format = srv_format_for(r->GetDesc().Format);
            device->CreateShaderResourceView(r, &srv, slot(i));
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = srv;
            null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            device->CreateShaderResourceView(nullptr, &null_srv, slot(i));
        }
    };

    srv.Format = eye_format;
    device->CreateShaderResourceView(eye_in, &srv, slot(0));
    make_srv(1, centre_ok ? centre_color : nullptr);
    make_srv(2, centre_ok ? centre_hud : nullptr);
    make_srv(3, centre_ok ? centre_fin : nullptr);
    make_srv(4, fovea_ok ? fovea_color : nullptr);
    make_srv(5, fovea_ok ? fovea_hud : nullptr);
    // the fovea views' copy with GUI is not reliable (the engine reuses that target between them): their interface is
    // found from their colour, this slot only needs a valid view
    make_srv(6, fovea_ok ? fovea_hud : nullptr);

    // the output curve (UiCurve, resting in NON_PIXEL_SHADER_RESOURCE); a null view when it is not measured yet
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(p.curve, &srv, slot(7));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = out_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(8));

    MonoUiConstants k{};
    k.out_w = (uint32_t)odesc.Width;
    k.out_h = odesc.Height;
    k.flags = (centre_ok ? 1u : 0u) | (fovea_ok ? 2u : 0u) | (p.debug_tint ? 4u : 0u) |
              (centre_ok && is_srgb(srv_format_for(centre_color->GetDesc().Format)) ? 8u : 0u) |
              (fovea_ok && is_srgb(srv_format_for(fovea_color->GetDesc().Format)) ? 16u : 0u) |
              (p.curve != nullptr ? 32u : 0u) | (fovea_ok ? 64u : 0u) | (p.post ? 256u : 0u);
    k.post[0] = p.post_sharp;
    k.post[1] = p.post_sat;
    k.post[2] = p.post_gamma;
    k.post[3] = p.post_contrast;
    k.post2[0] = p.post_edge; // [EDGE SOFTEN 11.11.2026]
    k.post2[1] = p.post_edge_threshold;
    k.anchor_count = (std::min)(p.anchor_count, MAX_ANCHORS);
    std::memcpy(k.eye_tan, p.eye_tan, sizeof(k.eye_tan));
    std::memcpy(k.wide_tan, p.wide_tan, sizeof(k.wide_tan));
    std::memcpy(k.rect, p.rect, sizeof(k.rect));
    k.eye_x = p.eye_x;
    k.panel_depth = (std::max)(p.panel_depth, 0.05f);
    k.feather = p.feather_px;
    std::memcpy(k.anchors, p.anchors, sizeof(k.anchors));
    std::memcpy(k.anchor_ext, p.anchor_ext, sizeof(k.anchor_ext));
    std::memcpy(m_cb_mapped + (size_t)ring * CB_SLOT_SIZE, &k, sizeof(k));

    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto ENGINE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto engine_read = compute_read_state(ENGINE);

    barrier(cmd, eye_in, UAV, NPSR);

    if (centre_ok) {
        barrier(cmd, centre_color, ENGINE, engine_read);
    }

    if (fovea_ok) {
        barrier(cmd, fovea_color, ENGINE, engine_read);
    }

    // the GUI copies rest in COMMON: a compute read promotes them implicitly (as in the interface restore)

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress() + (UINT64)ring * CB_SLOT_SIZE);
    cmd->SetComputeRootDescriptorTable(1, gpu);
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch(((uint32_t)odesc.Width + 7) / 8, (odesc.Height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &uav_done);

    if (fovea_ok) {
        barrier(cmd, fovea_color, engine_read, ENGINE);
    }

    if (centre_ok) {
        barrier(cmd, centre_color, engine_read, ENGINE);
    }

    barrier(cmd, eye_in, NPSR, UAV);

    m_last_error = "";
    return true;
}

void MonoUi::reset() {
    if (m_cb != nullptr && m_cb_mapped != nullptr) {
        m_cb->Unmap(0, nullptr);
    }

    m_cb_mapped = nullptr;
    m_cb.Reset();
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [UI CURVE 27.10.2026] UiCurve, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char UI_CURVE_SHADER[] = R"(
Texture2D<float4>   g_pmt   : register(t0); // PostMainTarget copy with GUI (linear)
Texture2D<float4>   g_col   : register(t1); // the same layer's colour after the output pass
RWByteAddressBuffer g_sums  : register(u0); // per bin and channel: sum (x1023), count; then the table as floats
RWTexture2D<float4> g_table : register(u1); // rgb = output per channel; [HIGHLIGHTS 11.11.2026] a = the highest bin
                                            // measured this frame on all three channels (above it the table is held)

cbuffer Params : register(b0) {
    uint g_w;
    uint g_h;
    uint g_flags; // 1 = the colour view decodes SRGB (encode it again), 2 = only clear the sums, 4 = first measurement
    uint g_pad;
};

#define BINS 80
#define TABLE_OFFSET (BINS * 3 * 8)

float to_srgb1(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

groupshared uint s_sum[BINS * 3];
groupshared uint s_cnt[BINS * 3];

[numthreads(16, 16, 1)]
void accumulate(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < BINS * 3; i += 256) {
        s_sum[i] = 0;
        s_cnt[i] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x < g_w && id.y < g_h) {
        const float3 p = g_pmt.Load(int3(id.xy, 0)).rgb;
        float3 o = g_col.Load(int3(id.xy, 0)).rgb;

        if ((g_flags & 1) != 0) {
            o = float3(to_srgb1(o.r), to_srgb1(o.g), to_srgb1(o.b));
        }

        [unroll] for (uint c = 0; c < 3; ++c) {
            const float x = p[c];

            // 2^-14 .. 2^6, a quarter stop per bin
            if (x >= 6.103515625e-05 && x < 64.0) {
                const uint b = min((uint)((log2(x) + 14.0) * 4.0), (uint)(BINS - 1));
                InterlockedAdd(s_sum[b * 3 + c], (uint)(saturate(o[c]) * 1023.0 + 0.5));
                InterlockedAdd(s_cnt[b * 3 + c], 1u);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint k = gi; k < BINS * 3; k += 256) {
        if (s_cnt[k] != 0) {
            uint before = 0;
            g_sums.InterlockedAdd(k * 8, s_sum[k], before);
            g_sums.InterlockedAdd(k * 8 + 4, s_cnt[k], before);
        }
    }
}

// [GPU 29.10.2026 round 3] The same sums with 2 x 2 pixels per thread: a quarter of the groups, so a quarter of the
// additions into the shared sums (integers: the order does not change them).
[numthreads(16, 16, 1)]
void accumulate4(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < BINS * 3; i += 256) {
        s_sum[i] = 0;
        s_cnt[i] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint j = 0; j < 4; ++j) {
        const uint2 px = gid.xy * 32 + tid.xy * 2 + uint2(j & 1, j >> 1);

        if (px.x < g_w && px.y < g_h) {
            const float3 p = g_pmt.Load(int3(px, 0)).rgb;
            float3 o = g_col.Load(int3(px, 0)).rgb;

            if ((g_flags & 1) != 0) {
                o = float3(to_srgb1(o.r), to_srgb1(o.g), to_srgb1(o.b));
            }

            [unroll] for (uint c = 0; c < 3; ++c) {
                const float x = p[c];

                if (x >= 6.103515625e-05 && x < 64.0) {
                    const uint b = min((uint)((log2(x) + 14.0) * 4.0), (uint)(BINS - 1));
                    InterlockedAdd(s_sum[b * 3 + c], (uint)(saturate(o[c]) * 1023.0 + 0.5));
                    InterlockedAdd(s_cnt[b * 3 + c], 1u);
                }
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint k = gi; k < BINS * 3; k += 256) {
        if (s_cnt[k] != 0) {
            uint before = 0;
            g_sums.InterlockedAdd(k * 8, s_sum[k], before);
            g_sums.InterlockedAdd(k * 8 + 4, s_cnt[k], before);
        }
    }
}

groupshared float s_v[BINS * 3];
groupshared uint  s_ok[BINS * 3];
groupshared int   s_top[3]; // [HIGHLIGHTS 11.11.2026] the last measured bin per channel (-1: none)

// [HIGHLIGHTS 11.11.2026] the table's alpha: the lowest of the channels' last measured bins (no channel measured: the
// whole table counts as measured, as before)
float measured_top(int a, int b, int c) {
    const int lo = min(a < 0 ? BINS : a, min(b < 0 ? BINS : b, c < 0 ? BINS : c));
    return (float)(lo >= BINS ? BINS - 1 : lo);
}

[numthreads(BINS, 1, 1)]
void resolve(uint3 id : SV_GroupThreadID) {
    const uint b = id.x;

    [unroll] for (uint c = 0; c < 3; ++c) {
        const uint k = b * 3 + c;
        const uint sum = g_sums.Load(k * 8);
        const uint cnt = g_sums.Load(k * 8 + 4);
        g_sums.Store2(k * 8, uint2(0, 0)); // ready for the next frame
        s_ok[k] = cnt >= 24 ? 1u : 0u;
        s_v[k] = cnt > 0 ? (float)sum / (1023.0 * (float)cnt) : 0.0;
    }

    GroupMemoryBarrierWithGroupSync();

    // per channel (one thread each): gaps between measured bins interpolated, below the measured range the SRGB shape
    // scaled to the first measured bin, above it held; then kept rising. No measured bin: -1 (keep the old table).
    if (b < 3) {
        const uint c = b;
        int first = -1;
        int last = -1;

        [loop] for (int i = 0; i < BINS; ++i) {
            if (s_ok[i * 3 + c] != 0) {
                first = first < 0 ? i : first;
                last = i;
            }
        }

        s_top[c] = last;

        if (first >= 0) {
            int prev = first;

            [loop] for (int i = 0; i < BINS; ++i) {
                const uint k = (uint)i * 3 + c;

                if (s_ok[k] != 0) {
                    prev = i;
                    continue;
                }

                if (i < first) {
                    const float xi = exp2(((float)i + 0.5) / 4.0 - 14.0);
                    const float xf = exp2(((float)first + 0.5) / 4.0 - 14.0);
                    s_v[k] = s_v[(uint)first * 3 + c] * to_srgb1(xi) / max(to_srgb1(xf), 1e-6);
                } else if (i > last) {
                    s_v[k] = s_v[(uint)last * 3 + c];
                } else {
                    int next = i + 1;

                    [loop] while (next < last && s_ok[(uint)next * 3 + c] == 0) {
                        ++next;
                    }

                    s_v[k] = lerp(s_v[(uint)prev * 3 + c], s_v[(uint)next * 3 + c], (float)(i - prev) / (float)(next - prev));
                }
            }

            [loop] for (int j = 1; j < BINS; ++j) {
                s_v[(uint)j * 3 + c] = max(s_v[(uint)j * 3 + c], s_v[(uint)(j - 1) * 3 + c]);
            }
        } else {
            [loop] for (int i = 0; i < BINS; ++i) {
                s_v[(uint)i * 3 + c] = -1.0;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if ((g_flags & 2) == 0) {
        const float x = exp2(((float)b + 0.5) / 4.0 - 14.0);
        float3 v = 0.0;

        [unroll] for (uint c = 0; c < 3; ++c) {
            const float now = s_v[b * 3 + c];
            const float old = asfloat(g_sums.Load(TABLE_OFFSET + (b * 3 + c) * 4));

            if ((g_flags & 4) != 0) {
                v[c] = now >= 0.0 ? now : to_srgb1(x); // first measurement
            } else {
                v[c] = now >= 0.0 ? lerp(old, now, 0.25) : old; // a few frames' worth; no data: unchanged
            }

            g_sums.Store(TABLE_OFFSET + (b * 3 + c) * 4, asuint(v[c]));
        }

        g_table[uint2(b, 0)] = float4(v, measured_top(s_top[0], s_top[1], s_top[2]));
    }
}

// [GPU 29.10.2026 round 3] The same table with one thread per bin and channel instead of one per channel walking the
// bins: the nearest measured bins before and after every bin, the measured range and the running maximum come from
// scans over the bins in shared memory. Every value is computed with the same expression as above.
groupshared int   s_first[3];
groupshared int   s_last[3];
groupshared int   s_prev[BINS * 3];  // largest measured bin <= this one (-1: none)
groupshared int   s_next[BINS * 3];  // smallest measured bin >= this one (BINS: none)
groupshared float s_fill[BINS * 3];

[numthreads(BINS, 3, 1)]
void resolve_par(uint3 id : SV_GroupThreadID) {
    const uint b = id.x;
    const uint c = id.y;
    const uint k = b * 3 + c;

    if (b == 0) {
        s_first[c] = BINS;
        s_last[c] = -1;
    }

    const uint sum = g_sums.Load(k * 8);
    const uint cnt = g_sums.Load(k * 8 + 4);
    g_sums.Store2(k * 8, uint2(0, 0)); // ready for the next frame
    const bool ok = cnt >= 24;
    s_ok[k] = ok ? 1u : 0u;
    s_v[k] = cnt > 0 ? (float)sum / (1023.0 * (float)cnt) : 0.0;
    s_prev[k] = ok ? int(b) : -1;
    s_next[k] = ok ? int(b) : BINS;
    GroupMemoryBarrierWithGroupSync();

    if (ok) {
        InterlockedMin(s_first[c], int(b));
        InterlockedMax(s_last[c], int(b));
    }

    [unroll] for (uint d = 1; d < BINS; d <<= 1) {
        GroupMemoryBarrierWithGroupSync();
        const int pv = b >= d ? s_prev[(b >= d ? b - d : 0) * 3 + c] : -1;
        const int nx = b + d < BINS ? s_next[min(b + d, BINS - 1) * 3 + c] : BINS;
        GroupMemoryBarrierWithGroupSync();
        s_prev[k] = max(s_prev[k], pv);
        s_next[k] = min(s_next[k], nx);
    }

    GroupMemoryBarrierWithGroupSync();

    const int first = s_first[c];
    const int last = s_last[c];
    const int i = int(b);
    float v = s_v[k];

    if (last < 0) {
        v = -1.0; // no measured bin: keep the old table
    } else if (!ok) {
        if (i < first) {
            const float xi = exp2(((float)i + 0.5) / 4.0 - 14.0);
            const float xf = exp2(((float)first + 0.5) / 4.0 - 14.0);
            v = s_v[(uint)first * 3 + c] * to_srgb1(xi) / max(to_srgb1(xf), 1e-6);
        } else if (i > last) {
            v = s_v[(uint)last * 3 + c];
        } else {
            const int prev = s_prev[k];
            const int next = s_next[k];
            v = lerp(s_v[(uint)prev * 3 + c], s_v[(uint)next * 3 + c], (float)(i - prev) / (float)(next - prev));
        }
    }

    s_fill[k] = v;

    // kept rising: the largest value up to this bin
    [unroll] for (uint e = 1; e < BINS; e <<= 1) {
        GroupMemoryBarrierWithGroupSync();
        const float before = b >= e ? s_fill[(b >= e ? b - e : 0) * 3 + c] : -1e30;
        GroupMemoryBarrierWithGroupSync();

        if (last >= 0) {
            s_fill[k] = max(s_fill[k], before);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if ((g_flags & 2) == 0) {
        const float x = exp2(((float)b + 0.5) / 4.0 - 14.0);
        const float now = s_fill[k];
        const float old = asfloat(g_sums.Load(TABLE_OFFSET + k * 4));
        float nv;

        if ((g_flags & 4) != 0) {
            nv = now >= 0.0 ? now : to_srgb1(x); // first measurement
        } else {
            nv = now >= 0.0 ? lerp(old, now, 0.25) : old; // a few frames' worth; no data: unchanged
        }

        g_sums.Store(TABLE_OFFSET + k * 4, asuint(nv));
        s_v[k] = nv;
    }

    GroupMemoryBarrierWithGroupSync();

    if ((g_flags & 2) == 0 && c == 0) {
        g_table[uint2(b, 0)] = float4(s_v[b * 3], s_v[b * 3 + 1], s_v[b * 3 + 2], measured_top(s_last[0], s_last[1], s_last[2]));
    }
}

)";
} // namespace

bool UiCurve::setup(ID3D12Device* device) {
    if (m_pso_sum != nullptr && m_pso_resolve != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> sum_cs{};
    ComPtr<ID3DBlob> resolve_cs{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(UI_CURVE_SHADER, sizeof(UI_CURVE_SHADER) - 1, "UiCurve", nullptr, nullptr, "accumulate", "cs_5_0", 0, 0, &sum_cs, &errors))) {
        REVR_LOG(spdlog::error("[UiCurve] shader compile failed (accumulate): {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    errors.Reset();

    if (FAILED(compile_cached(UI_CURVE_SHADER, sizeof(UI_CURVE_SHADER) - 1, "UiCurve", nullptr, nullptr, "resolve", "cs_5_0", 0, 0, &resolve_cs, &errors))) {
        REVR_LOG(spdlog::error("[UiCurve] shader compile failed (resolve): {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    // [GPU 29.10.2026 round 3] the faster passes (optional: the ones above otherwise)
    ComPtr<ID3DBlob> sum4_cs{};
    ComPtr<ID3DBlob> resolve_par_cs{};

    if (FAILED(compile_cached(UI_CURVE_SHADER, sizeof(UI_CURVE_SHADER) - 1, "UiCurve", nullptr, nullptr, "accumulate4", "cs_5_0", 0, 0, &sum4_cs, &errors)) ||
        FAILED(compile_cached(UI_CURVE_SHADER, sizeof(UI_CURVE_SHADER) - 1, "UiCurve", nullptr, nullptr, "resolve_par", "cs_5_0", 0, 0, &resolve_par_cs, &errors))) {
        REVR_LOG(spdlog::error("[UiCurve] round 3 passes not compiled: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        sum4_cs.Reset();
        resolve_par_cs.Reset();
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[UiCurve] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = sum_cs->GetBufferPointer();
    pso_desc.CS.BytecodeLength = sum_cs->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_sum)))) {
        REVR_LOG(spdlog::error("[UiCurve] CreateComputePipelineState failed (accumulate)"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    pso_desc.CS.pShaderBytecode = resolve_cs->GetBufferPointer();
    pso_desc.CS.BytecodeLength = resolve_cs->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_resolve)))) {
        REVR_LOG(spdlog::error("[UiCurve] CreateComputePipelineState failed (resolve)"));
        m_last_error = "pipeline state failed";
        m_pso_sum.Reset();
        m_root_sig.Reset();
        return false;
    }

    if (sum4_cs != nullptr && resolve_par_cs != nullptr) {
        pso_desc.CS.pShaderBytecode = sum4_cs->GetBufferPointer();
        pso_desc.CS.BytecodeLength = sum4_cs->GetBufferSize();
        const bool sum_ok = SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_sum4)));
        pso_desc.CS.pShaderBytecode = resolve_par_cs->GetBufferPointer();
        pso_desc.CS.BytecodeLength = resolve_par_cs->GetBufferSize();

        if (!sum_ok || FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso_resolve_par)))) {
            m_pso_sum4.Reset();
            m_pso_resolve_par.Reset();
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = (UINT64)BINS * 3 * 12; // sums + counts (8 bytes), table (4 bytes)
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = BINS;
    tex.Height = 1;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_sums))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_table)))) {
        REVR_LOG(spdlog::error("[UiCurve] descriptor heap / buffers failed"));
        m_last_error = "descriptor heap / buffers failed";
        m_pso_sum.Reset();
        m_pso_resolve.Reset();
        m_pso_sum4.Reset();
        m_pso_resolve_par.Reset();
        m_root_sig.Reset();
        m_heap.Reset();
        m_sums.Reset();
        m_table.Reset();
        return false;
    }

    m_sums->SetName(L"RE4VR interface curve sums");
    m_table->SetName(L"RE4VR interface curve");
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_cleared = false;
    m_measured = false;
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[UiCurve] ready{}", m_pso_sum4 != nullptr ? " (round 3 passes)" : ""));
    return true;
}

bool UiCurve::learn(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* pmt, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
    bool round3) {
    if (device == nullptr || cmd == nullptr || pmt == nullptr || color == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto pdesc = pmt->GetDesc();
    const auto cdesc = color->GetDesc();

    if (pdesc.Width != cdesc.Width || pdesc.Height != cdesc.Height) {
        m_last_error = "sizes differ";
        return false;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    const auto color_format = srv_format_for(cdesc.Format);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = srv_format_for(pdesc.Format);
    device->CreateShaderResourceView(pmt, &srv, slot(0));
    srv.Format = color_format;
    device->CreateShaderResourceView(color, &srv, slot(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.FirstElement = 0;
    raw.Buffer.NumElements = BINS * 3 * 3; // 12 bytes per bin and channel
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(m_sums.Get(), nullptr, &raw, slot(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_table.Get(), nullptr, &uav, slot(3));

    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto color_read = compute_read_state(color_state);

    barrier(cmd, color, color_state, color_read);
    barrier(cmd, m_table.Get(), NPSR, UAV);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    D3D12_RESOURCE_BARRIER sums_done{};
    sums_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    sums_done.UAV.pResource = m_sums.Get();

    uint32_t constants[4]{};
    constants[0] = (uint32_t)cdesc.Width;
    constants[1] = cdesc.Height;
    const uint32_t encode = is_srgb(color_format) ? 1u : 0u;

    const bool fast = round3 && m_pso_sum4 != nullptr && m_pso_resolve_par != nullptr;
    ID3D12PipelineState* const resolve = fast ? m_pso_resolve_par.Get() : m_pso_resolve.Get();

    // the sums start at zero: once (the resolve pass zeroes them after every measurement)
    if (!m_cleared) {
        constants[2] = encode | 2u;
        cmd->SetPipelineState(resolve);
        cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);
        cmd->Dispatch(1, 1, 1);
        cmd->ResourceBarrier(1, &sums_done);
        m_cleared = true;
    }

    constants[2] = encode;
    cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);

    if (fast) {
        cmd->SetPipelineState(m_pso_sum4.Get());
        cmd->Dispatch(((uint32_t)cdesc.Width + 31) / 32, (cdesc.Height + 31) / 32, 1); // 32 x 32 pixels per group
    } else {
        cmd->SetPipelineState(m_pso_sum.Get());
        cmd->Dispatch(((uint32_t)cdesc.Width + 15) / 16, (cdesc.Height + 15) / 16, 1);
    }

    cmd->ResourceBarrier(1, &sums_done);

    constants[2] = encode | (m_measured ? 0u : 4u);
    cmd->SetPipelineState(resolve);
    cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);
    cmd->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER done[2]{};
    done[0] = sums_done;
    done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    done[1].UAV.pResource = m_table.Get();
    cmd->ResourceBarrier(2, done);

    barrier(cmd, m_table.Get(), UAV, NPSR);
    barrier(cmd, color, color_read, color_state);

    m_measured = true;
    m_last_error = "";
    return true;
}

void UiCurve::reset() {
    m_pso_sum.Reset();
    m_pso_resolve.Reset();
    m_pso_sum4.Reset();
    m_pso_resolve_par.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_sums.Reset();
    m_table.Reset();
    m_ring = 0;
    m_cleared = false;
    m_measured = false;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [GPU 29.10.2026] GuiMap, see FoveaComposite.hpp
// ================================================================================================================
namespace {
constexpr char GUI_MAP_SHADER[] = R"(
// [GPU 29.10.2026] where the shared wide view's two GUI copies differ: per block of 8 x 8 texels (and 2 texels around it,
// so a bilinear read starting in the block stays inside, whatever way the sampler rounds) the block holds this frame's
// stamp when some texel in that reach differs. Everywhere else both copies hold the same image and the interface mask is
// exactly 0 (EyeCompose skips it there). One thread per texel, one group per block: the copies are read once, in parallel,
// and nothing has to be cleared (a block with an older stamp has no difference this frame).
// [BEAM FIT 03.11.2026] The map has one row more than there are blocks (row g_bh): beam_fit's results, see there.
Texture2D<float4>   g_hud   : register(t0); // copy without GUI
Texture2D<float4>   g_fin   : register(t1); // copy with GUI (same size)
RWTexture2D<uint>   g_map   : register(u0);
RWByteAddressBuffer g_count : register(u1); // running count of the marked blocks (read back for the log)

cbuffer Params : register(b0) {
    uint g_w;
    uint g_h;
    uint g_bw;
    uint g_bh;
    uint g_stamp; // this frame's stamp (never 0)
    uint3 g_pad;
};

groupshared uint s_mark; // bit (dy + 1) * 3 + (dx + 1): the block at (dx, dy) from this one has a differing texel in reach

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi == 0) {
        s_mark = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x < g_w && id.y < g_h && any(g_hud.Load(int3(id.xy, 0)).rgb != g_fin.Load(int3(id.xy, 0)).rgb)) {
        // block b reaches the texels 8 b - 2 .. 8 b + 9: this one is in reach of its own block and, two texels from a
        // border or closer, of the block on that side
        const uint2 l = id.xy & 7;
        const int x0 = l.x <= 1 ? -1 : 0;
        const int x1 = l.x >= 6 ? 1 : 0;
        const int y0 = l.y <= 1 ? -1 : 0;
        const int y1 = l.y >= 6 ? 1 : 0;
        uint bits = 0;

        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                if (dx >= x0 && dx <= x1 && dy >= y0 && dy <= y1) {
                    bits |= 1u << uint((dy + 1) * 3 + dx + 1);
                }
            }
        }

        InterlockedOr(s_mark, bits);
    }

    GroupMemoryBarrierWithGroupSync();

    // one thread per block in reach marks it (and counts it once per frame)
    if (gi < 9 && (s_mark & (1u << gi)) != 0) {
        const int2 b = int2(gid.xy) + int2(int(gi % 3) - 1, int(gi / 3) - 1);

        if (all(b >= 0) && b.x < int(g_bw) && b.y < int(g_bh)) {
            uint before;
            InterlockedExchange(g_map[uint2(b)], g_stamp, before);

            if (before != g_stamp) {
                uint n;
                g_count.InterlockedAdd(0, 1u, n);

                // [LIGHTS 11.11.2026] interface somewhere in this frame: the stamp also into the extra row's last texel
                // (the beam results use its first 64)
                if (g_bw > 64) {
                    g_map[uint2(g_bw - 1, g_bh)] = g_stamp;
                }
            }
        }
    }
}

// [BEAM FIT 03.11.2026] Where the wide view drew each item beam (dump 03 of 03.11.2026 15:00: 0.024 tan left of its
// item, whose position the anchor has; in the fovea it stands on it). Per beam anchor one group searches a box of the
// copies around the anchor for texels where the GUI added green (as EyeCompose's beam_colour, on the linear copies),
// counts them per column and takes the column with the most, the ones nearer the anchor preferred: across, and with the
// column's lowest beam texel near the anchor's height (a beam rises from its item: two beams 0.016 tan apart, each drawn
// 0.02 to the side, are told apart by where they start); around it (3 columns each side) the weighted centre and the
// lowest texel. Into the map's extra row g_bh, 4 values per anchor index: x of the column (tangent), its lowest point
// (tangent y), the texels counted, this frame's stamp.
cbuffer Fit : register(b1) {
    uint   g_fit_count;      // beam anchors to fit (groups)
    uint   g_fit_stamp;
    float2 g_fit_tpt;        // tangents per texel, x and y
    float4 g_fit_wide;       // the wide view's left and top tangents
    int4   g_fit_box[16];    // texel box searched: x0, y0, x1, y1 (inclusive)
    float4 g_fit_anchor[16]; // x = the anchor's texel x, y = its index in the eye pass's anchor list, z = its texel y
};

groupshared uint s_cnt[128];
groupshared uint s_low[128];

[numthreads(256, 1, 1)]
void beam_fit(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const uint a = gid.x;

    if (a >= g_fit_count) {
        return;
    }

    const int4 box = g_fit_box[a];
    const int w = clamp(box.z - box.x + 1, 1, 128);
    const int h = max(box.w - box.y + 1, 1);

    if (gi < 128) {
        s_cnt[gi] = 0;
        s_low[gi] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    for (int t = int(gi); t < w * h; t += 256) {
        const int c = t % w;
        const int2 p = int2(box.x + c, box.y + t / w);

        if (any(p < 0) || p.x >= int(g_w) || p.y >= int(g_h)) {
            continue;
        }

        const float3 d = g_fin.Load(int3(p, 0)).rgb - g_hud.Load(int3(p, 0)).rgb;

        if (d.g > 0.002 && d.g - max(d.r, d.b) > 0.15 * d.g) {
            InterlockedAdd(s_cnt[c], 1u);
            InterlockedMax(s_low[c], uint(p.y) + 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0) {
        const float ax = g_fit_anchor[a].x - float(box.x);
        const float ay = g_fit_anchor[a].z;
        float best = 0.0;
        int bc = -1;

        for (int c = 0; c < w; ++c) {
            if (s_cnt[c] == 0) {
                continue;
            }

            const float q = (float(c) + 0.5 - ax) / 12.0;
            const float gap = ay - (float(s_low[c]) - 0.5); // > 0: the column's beam starts above the anchor (texels)
            const float g = gap > 0.0 ? gap / 24.0 : gap / 16.0;
            const float s = float(s_cnt[c]) / ((1.0 + q * q) * (1.0 + g * g));

            if (s > best) {
                best = s;
                bc = c;
            }
        }

        float sum = 0.0;
        float sx = 0.0;
        uint low = 0;

        if (bc >= 0) {
            for (int k = max(bc - 3, 0); k <= min(bc + 3, w - 1); ++k) {
                sum += float(s_cnt[k]);
                sx += float(s_cnt[k]) * (float(k) + 0.5);
                low = max(low, s_low[k]);
            }
        }

        const float xt = float(box.x) + (sum > 0.0 ? sx / sum : ax);
        const uint i = uint(g_fit_anchor[a].y) * 4u;
        g_map[uint2(i, g_bh)] = asuint(g_fit_wide.x + xt * g_fit_tpt.x);
        g_map[uint2(i + 1u, g_bh)] = asuint(g_fit_wide.y - float(low) * g_fit_tpt.y);
        g_map[uint2(i + 2u, g_bh)] = asuint(sum);
        g_map[uint2(i + 3u, g_bh)] = g_fit_stamp;
    }
}
)";

// in several literals, joined by the compiler (MSVC takes at most 16 KB in one string literal)
constexpr char EYE_COMPOSE_SHADER[] = R"(
// [EYE COMPOSE 29.10.2026] one pass per eye: the periphery enlarged to the eye image (WideUpscale), the fovea blended in
// (FoveaComposite), the post sharpening, the interface and the rest of the post pass (MonoUi) -- the same maths as those
// three passes, without writing and reading the eye-sized image in between. A group makes TSX x TSY eye pixels (16 x 16):
// the periphery taps of the tile are read once into shared memory and enlarged in two steps (rows, then columns); the
// composite is made for one pixel more around the tile (the sharpening's neighbours).
Texture2D<float4>   g_per    : register(t0); // this eye's periphery (render density)
Texture2D<float4>   g_fov    : register(t1); // the fovea image (fovea size)
Texture2D<float4>   g_c_col  : register(t2); // shared wide view: colour after the output pass
Texture2D<float4>   g_c_hud  : register(t3); //   copy without GUI
Texture2D<float4>   g_c_fin  : register(t4); //   copy with GUI
Texture2D<float4>   g_f_col  : register(t5); // this eye's fovea view: colour after the output pass
Texture2D<float4>   g_f_hud  : register(t6); //   copy without GUI
Texture2D<float4>   g_curve  : register(t7); // UiCurve: PostMainTarget value -> output colour, per channel
RWTexture2D<float4> g_out    : register(u0); // the finished eye image
RWTexture2D<float>  g_gain   : register(u1); // row 0, per eye slot (x = slot * 4): smoothed gain (two, by g_parity), last measurement,
                                             // this frame's; [TONE 01.11.2026] row 1, per eye slot (x = slot * TONE_SLOT): the tone
                                             // table twice (by g_parity), this frame's
RWTexture2D<uint>   g_ui_map : register(u2); // GuiMap: 8 x 8 texel blocks of the wide view where its GUI copies differ
RWTexture2D<float>  g_part   : register(u3); // measure: per eye slot (x = slot * PART_W) 16 partial sums of the periphery, 16 of the
                                             // fovea
SamplerState        g_lin    : register(s0);

cbuffer Params : register(b0) {
    uint2  g_out_size;     // eye image
    uint2  g_per_size;     // periphery
    uint   g_flags;        // 1 = wide view GUI valid, 2 = fovea GUI valid, 4 = debug tint, 8 / 16 = encode the wide / fovea colour,
                           // 32 = output curve measured, 64 = the fovea's interface from its colour, 128 = GUI map valid,
                           // 256 = post pass (g_post), 512 = encode the periphery to SRGB, 1024 = fovea blended in,
                           // 2048 = exposure gain, 4096 = debug border, 8192 = periphery taps from shared memory,
                           // 16384 = the eye image is UNORM (the separate passes' images clamped every step to [0, 1]),
                           // 32768 = tiles without a marked GuiMap block in reach skip the wide view's interface,
                           // 65536 = the output is the 8-bit headset texture: rounded as the 10-bit eye image first,
                           // 131072 = with 2048: the fovea scaled by the tone table (a factor per brightness) instead of the gain,
                           // 262144 = the periphery's large-scale light added to the fovea (g_light, made by blocks / light_blur),
                           // 524288 = match measures the fovea / periphery difference (view metric),
                           // 1048576 = the gain / table of the last frame from their ping-pong places (blocks path: match
                           //   writes the next ones at the same time),
                           // 2097152 = the periphery's alpha marks the gaps of the shared periphery: the fovea covers them in
                           //   its blend band (HOLE BAND),
                           // 4194304 = a game menu is open: the periphery's scene where the fovea is far darker over a
                           //   large area (light_blur's w, MENU BACKDROP),
                           // 8388608 = [MENUS fix82] the fovea image holds its view's interface (it went through the
                           //   anti-aliasing with it): no interface of the fovea view is added (g_f_col / g_f_hud not
                           //   read), the wide view's changes over to it with the fovea's weight, and the blocks leave out
                           //   the places where the wide view has interface (with 2: the rectangle is given)
                           // 16777216 = [MENUS fix83] with 8388608: the fovea view's two interface copies are bound (g_f_col
                           //   the one with GUI, g_f_hud the one without): where they differ the fovea view drew
                           //   interface (blocks leave those places out; the debug tint's green),
                           // 33554432 = [BACKDROP fix83] with 16777216: a backdrop the fovea view draws darker than the
                           //   wide view does there is taken out of the fovea's pixel (fovea_backdrop)
                           // 67108864 = [TILE PASS fix106] only for ui_tiles (test): no tile marked
    uint   g_anchor_count;
    uint   g_slot;         // gain slot (eye)
    float  g_feather;      // composite blend width (pixels, limited to half the rectangle)
    float4 g_eye_tan;      // l, r, b, t
    float4 g_wide_tan;
    float4 g_rect;         // interface: x, y, w, h (eye pixels)
    int2   g_rect_pos;     // composite: fovea rectangle origin in the eye image
    uint2  g_rect_size;    //            and size
    uint2  g_fov_size;     // fovea image size
    uint2  g_gui_size;     // the wide view's GUI copies
    uint2  g_map_size;     // GuiMap blocks
    float  g_eye_x;
    float  g_panel_depth;
    float  g_ui_feather;   // interface blend width (pixels)
    float  g_rate;         // gain smoothing per measurement
    uint   g_parity;       // which of the slot's two gains holds the last frame's (the other gets this frame's)
    uint   g_map_stamp;    // GuiMap: the stamp of this frame's marked blocks
    float4 g_post;         // flag 256: sharpness, saturation, brightness exponent, contrast
    float4 g_anchors[16];  // tangent x, tangent y, depth, radius (see MonoUi)
    uint4  g_loops;        // [GPU 31.10.2026 night] rounds of a step the driver would otherwise make on both sides and select
                           // (a loop is never flattened): x = contrast (g_post.w != 1), y = the composite clamped to [0, 1]
                           // (flag 16384); 0 or 1
    float4 g_light_p;      // [LIGHT 02.11.2026] x = the large-scale light's blur (gaussian sigma, blocks of 16 px); [GLOW 13.11.2026]
                           // y = its limit (part of the level, 0 = LIGHT_LIMIT), z = 1: only adding light, w = 1: automatic
                           // (blocks judged by how well they agree with the light around them, the periphery's gaps left out)
    float4 g_anchor_ext[16]; // [ITEM BEAMS 03.11.2026] per anchor: x = reach across, y = above, z = below its anchor
                             // (tangents), w = 1: an item beam (see MonoUi)
    float4 g_post2;        // [EDGE SOFTEN 11.11.2026] flag 256: edge softening weight (0 = off), edge threshold
    float4 g_ui_jit;       // [MENUS fix81] this frame's jitter as a UV offset: xy the fovea view's, zw the wide view's (the
                           // engine draws the interface with the frame's jitter: an image read at uv + offset stands still)
};

// [EDGE SOFTEN 11.11.2026] 1 on high-contrast edges (luma range of a pixel and its 4 neighbours from t up, full at t + 0.1),
// 0 on flat areas and fine texture
float edge_weight(float3 c, float3 n0, float3 n1, float3 n2, float3 n3, float t) {
    const float3 W = float3(0.2126, 0.7152, 0.0722);
    const float lc = dot(c, W), l0 = dot(n0, W), l1 = dot(n1, W), l2 = dot(n2, W), l3 = dot(n3, W);
    const float range = max(lc, max(max(l0, l1), max(l2, l3))) - min(lc, min(min(l0, l1), min(l2, l3)));
    return smoothstep(t, t + 0.1, range);
}

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

// [TONE 01.11.2026] (flag 131072) the fovea brought to the periphery by a tone table instead of one gain: the fovea camera
// exposes for the small part of the scene it sees, and through the engine's tone curve a different exposure changes the
// dark and the bright parts differently (dumps of 01.11: with one gain the sides of the rectangle 10-30 % off in contrasty
// scenes, with the table 3-6 %). The measure's points give two histograms of the brightness (sqrt of the luma, of a mean
// around each point so both images are about equally soft); the tone pass makes from their quantiles a table of factors
// over the fovea's brightness, smoothed like the gain.
#define TONE_BINS  64                     // histogram bins over sqrt(luma)
#define TONE_KNOTS 17                     // quantiles 0.5 / n .. 1 - 0.5 / n
#define TONE_N     64                     // table entries over sqrt(luma)
#define TONE_SLOT  (3 * TONE_N)           // per eye slot in row 1: the smoothed table twice, this frame's
#define PART_W     32                     // per eye slot in g_part (the measure's 16 + 16 sums)

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float to_srgb1(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// ---- the periphery enlarged (WideUpscale: Catmull-Rom, clamped to the 2x2 around the position) ----------------------
float4 cr_weights(float t) {
    return float4(t * (-0.5 + t * (1.0 - 0.5 * t)),
                  1.0 + t * t * (-2.5 + 1.5 * t),
                  t * (0.5 + t * (2.0 - 1.5 * t)),
                  t * t * (-0.5 + 0.5 * t));
}

// periphery position of an eye pixel's column / row (as WideUpscale computes it)
float src_x(int x) { return (float(x) + 0.5) * float(g_per_size.x) / float(g_out_size.x) - 0.5; }
float src_y(int y) { return (float(y) + 0.5) * float(g_per_size.y) / float(g_out_size.y) - 0.5; }

// the enlarged periphery at eye pixel q, read from the texture (when the tile does not fit in shared memory)
float3 periphery_direct(int2 q) {
    const float2 sp = float2(src_x(q.x), src_y(q.y));
    const int2 base = int2(floor(sp));
    const float2 f = sp - float2(base);
    const int2 last = int2(g_per_size) - 1;
    const float4 wx = cr_weights(f.x);
    const float4 wy = cr_weights(f.y);
    float3 c = 0.0;
    float3 lo = 1e30;
    float3 hi = -1e30;

    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            const float3 s = g_per.Load(int3(clamp(base + int2(x - 1, y - 1), int2(0, 0), last), 0)).rgb;
            c += s * (wx[x] * wy[y]);

            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                lo = min(lo, s);
                hi = max(hi, s);
            }
        }
    }

    return clamp(c, lo, hi);
}

// the separate passes' eye image at pixel q before the fovea: enlarged, encoded, stored (UNORM: clamped)
float3 eye_periphery(int2 q) {
    float3 c = periphery_direct(q);

    if ((g_flags & 512) != 0) {
        c = to_srgb(c);
    }

    return (g_flags & 16384) != 0 ? saturate(c) : c;
}

// ---- measure: mean brightness of the fovea image vs. the same area of the periphery (as FoveaComposite) -------------
// The fovea camera has its own automatic exposure: one overall gain per eye brings it to the periphery. 64 x 64 points
// over the inner 80 % of the rectangle, one per thread in 16 groups; the periphery is read at each point bilinearly from
// the four eye pixels around it, as the separate passes read their eye image. Each group leaves its two sums; the eye
// pass adds them up and smooths the gain (main, step 1).
groupshared float s_w[256];
groupshared float s_f[256];

[numthreads(16, 16, 1)]
void measure(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint idx = tid.y * 16 + tid.x;
    const float2 g = float2(gid.xy * 16 + tid.xy);
    const float2 r = lerp(0.1, 0.9, (g + 0.5) / 64.0);
    const float2 wide_uv = (float2(g_rect_pos) + r * float2(g_rect_size)) / float2(g_out_size);
    const float2 pe = wide_uv * float2(g_out_size) - 0.5;
    const int2 p0 = int2(floor(pe));
    const float2 f = pe - float2(p0);
    const int2 last = int2(g_out_size) - 1;
    const float3 c00 = eye_periphery(clamp(p0, int2(0, 0), last));
    const float3 c10 = eye_periphery(clamp(p0 + int2(1, 0), int2(0, 0), last));
    const float3 c01 = eye_periphery(clamp(p0 + int2(0, 1), int2(0, 0), last));
    const float3 c11 = eye_periphery(clamp(p0 + int2(1, 1), int2(0, 0), last));

    s_w[idx] = dot(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y), LUMA);
    s_f[idx] = dot(g_fov.SampleLevel(g_lin, r, 0).rgb, LUMA);
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = 128; step > 0; step >>= 1) {
        if (idx < step) {
            s_w[idx] += s_w[idx + step];
            s_f[idx] += s_f[idx + step];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (idx == 0) {
        const uint gi = gid.y * 4 + gid.x;
        g_part[uint2(g_slot * 32 + gi, 0)] = s_w[0];
        g_part[uint2(g_slot * 32 + 16 + gi, 0)] = s_f[0];
    }
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [GPU 30.10.2026] TSX x TSY = eye pixels per group, one pixel per thread (16 x 16 here; the pass is made with 32 x 32,
// see EyeCompose::setup). Tests on the PC: 32 x 32 with four pixels per thread 0.414 against 0.398 ms per eye (30.10);
// 16 x 8 +0.12, 8 x 8 +0.25 (the work per group that does not depend on its size), 32 x 16 +0.025, 32 x 32 with 1024
// threads -0.044 ms per eye (31.10). Any tile size gives the same image while the tile's periphery taps fit in SM x SM:
// each pixel's composite is made the same way, the tile only decides which group makes it.
#ifndef TSX
#define TSX 16
#endif
#ifndef TSY
#define TSY 16
#endif
// [GPU 31.10.2026] threads per group and axis: one per eye pixel by default; with NTX = TSX + 2 (and NTY = TSY + 2) the
// group has one thread per composite pixel, the tile and the pixel around it (the composite in one round)
#ifndef NTX
#define NTX TSX
#endif
#ifndef NTY
#define NTY TSY
#endif
#define NT (NTX * NTY) // threads per group
#define CSX (TSX + 2)  // composite pixels per group and axis (one around)
#define CSY (TSY + 2)
#define SM 16          // periphery taps per axis that fit in shared memory
// [GPU 31.10.2026] fewer ALU instructions, the same image (the pass was limited by them: VALU 83 % in the capture of 31.10):
// the uniform tests as branches instead of both sides and a select, the Catmull-Rom weights once per composite column and
// row, no integer division (taps by rows of SM, the composite's row from a multiply and a shift), shared memory places
// from small offsets. Test variants: BEFORE_1031 = the pass before, SELECT_TESTS = without the branches only.
#if defined(BEFORE_1031) || defined(SELECT_TESTS)
#define BR
#else
#define BR [branch]
#endif
// e / CSX for e < 4096 (checked for CSX = 10, 12, 18, 32, 34), the product below 2^24
#define DIV_CSX(e) (((e) * ((65536u + CSX - 1u) / CSX)) >> 16)
// [GPU 31.10.2026 night] the range of an index for the compiler: a mask (x & m, m = 2^k - 1 above every value x takes:
// the same value) lets it use 16-bit multiplies (v_mad_u32_u16) for the shared memory places instead of 32-bit ones
// (v_mul_lo_u32, a quarter of the rate; the trace of 31.10 night showed min() was not enough). PREV_2251 = the build of
// 31.10 22:51 (min(), no masks, the ternary of fovea_ui, the contrast and the clamp as branches).
#ifdef PREV_2251
#define HINT(x, bound, mask) min(x, bound)
#define MASK(x, mask) (x)
#else
#define HINT(x, bound, mask) ((x) & (mask))
#define MASK(x, mask) ((x) & (mask))
#endif
// [GPU 01.11.2026] the same for the places of the float3 arrays (x 12 bytes) and the composite row in step 5: the trace
// of 01.11 still showed v_mul_lo_u32 there. PREV_1101 = without these (the build of the 01.11 11:33 test).
#if defined(PREV_2251) || defined(PREV_1101)
#define MASK2(x, mask) (x)
#else
#define MASK2(x, mask) ((x) & (mask))
#endif

groupshared float3 s_src[SM * SM];  // periphery taps of the tile
groupshared float3 s_h[SM * CSX];   // taps of each tile row enlarged along x, per composite column
groupshared float4 s_c[CSX * CSY];  // composite (rgb, alpha) of the tile's pixels and one around
// ([MENU BAND 15.11.2026] the periphery of each pixel is not kept for step 5: a float4 per composite pixel took the group's
// shared memory to 48292 bytes, over the 32768 of cs_5_0, and the pass did not compile from 15.11.2026 15:00 on (the RE7
// logs: "[EyeCompose] shader compile failed", the separate eye passes ran instead). Step 5 reads it again where needed.)
#ifdef BEFORE_1031
groupshared int    s_bx[CSX];       // per composite column / row: first tap and fraction
groupshared int    s_by[CSY];
groupshared float  s_fx[CSX];
groupshared float  s_fy[CSY];
#else
groupshared uint   s_ox[CSX];       // per composite column / row: its first tap from the tile's first tap (0 .. SM - 4 when
groupshared uint   s_oy[CSY];       //   the tile's taps fit in shared memory)
groupshared float4 s_wx[CSX];       //   and the Catmull-Rom weights of its four taps
groupshared float4 s_wy[CSY];
groupshared int2   s_t0;            // the tile's first tap
#endif
groupshared float  s_gain;          // this frame's exposure gain
groupshared float  s_lut[TONE_N];   // [TONE] this frame's tone table (factor over sqrt(luma) of the fovea)
groupshared uint   s_ui_beam[17];   // [GPU fix99] the same for an item beam, read at its fitted place (only in a tile with s_ui_any)
groupshared uint   s_ui_depth[17];  // per interface depth (panel, then each anchor): a marked GuiMap block in reach of the tile
groupshared uint   s_ui_any;        // 0 = the wide view's interface is 0 on every pixel of the tile

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- the interface (MonoUi) ---------------------------------------------------------------------------------------
#define CURVE_AT(i) g_curve.Load(int3(i, 0, 0))

float curve_of(float x, int c) {
    if (!(x > 0.0)) {
        return 0.0;
    }

    if ((g_flags & 32) == 0) {
        return to_srgb1(x);
    }

    const float f = (log2(x) + 14.0) * 4.0 - 0.5;

    if (f <= 0.0) {
        return CURVE_AT(0)[c] * saturate(x * 15024.0);
    }

    const float fc = min(f, 79.0);
    const int b0 = int(fc);
    const int b1 = min(b0 + 1, 79);
    return lerp(CURVE_AT(b0)[c], CURVE_AT(b1)[c], fc - float(b0));
}

float3 curve3(float3 x) {
    return float3(curve_of(x.r, 0), curve_of(x.g, 1), curve_of(x.b, 2));
}

float mask_of(Texture2D<float4> hud, Texture2D<float4> fin, float2 uv) {
    const float3 fs = fin.SampleLevel(g_lin, uv, 0).rgb;
    const float3 hs = hud.SampleLevel(g_lin, uv, 0).rgb;

    // [GPU fix95 19.11.2026] the copies are the same image where no interface was drawn (most pixels of a marked block):
    // equal reads go through the curve to equal values, a difference of exactly 0 and the mask 0. RE4 test of 19.11.2026:
    // with the HUD in the picture (under 1 % of the blocks) the eye pass took 0.62 instead of 0.53 ms per eye.
    [branch] if (all(fs == hs)) {
        return 0.0;
    }

    const float3 d = abs(curve3(fs) - curve3(hs));
    return smoothstep(0.03, 0.08, max(d.r, max(d.g, d.b)));
}

// [HIGHLIGHTS 11.11.2026] The curve is measured on the shared wide view; above the highest bin measured there (the table's
// alpha) it is only held, while the engine's output keeps rising. The fovea views see small highlights (a torch or a
// lamp on a glossy floor) brighter than anything the wide view holds: their colour came out above the held value and
// counted as interface (the fovea view's own, jittered pixels over the upscaled fovea: sparkling dots). Above the measured
// range a channel only counts where the colour is darker than the curve (an interface pixel over a highlight).
float3 color_off_curve(float3 color, float3 hud, float3 pred) {
    float3 d = abs(color - pred);

    if ((g_flags & 32) != 0) {
        const float top = CURVE_AT(0).a;

        [unroll] for (int c = 0; c < 3; ++c) {
            if (hud[c] > 0.0 && (log2(hud[c]) + 14.0) * 4.0 - 0.5 > top) {
                d[c] = max(pred[c] - color[c], 0.0);
            }
        }
    }

    return d;
}

float mask_from_color(Texture2D<float4> hud, float3 color, float2 uv) {
    const float3 h = hud.SampleLevel(g_lin, uv, 0).rgb;
    const float3 d = color_off_curve(color, h, curve3(h)); // [HIGHLIGHTS 11.11.2026]
    return smoothstep(0.045, 0.11, max(d.r, max(d.g, d.b)));
}

// [TRANSLUCENT 11.11.2026] What the interface added to a fovea view's pixel: its colour above the view without GUI (as the
// output pass shows it), per channel. Only that is put over the upscaled fovea; where the interface darkened the view (a
// panel's translucent background: the opened map) the upscaled fovea already has it (UiClean keeps the darkening in the
// upscaler's input). Before, the fovea view's own pixel replaced the upscaled one wherever the mask counted: behind a
// translucent panel the view's highlights (a torch on the floor) passed the mask frame by frame, and its jittered pixels
// sparkled through the panel.
float3 ui_light(Texture2D<float4> hud, float3 color, float2 uv) {
    return max(color - curve3(hud.SampleLevel(g_lin, uv, 0).rgb), 0.0);
}

// [SPECKS 11.11.2026] Light added by the interface, found from a fovea view's colour: only where the colour is above the
// view without GUI by more than the threshold (above the measured curve range nothing counts, see color_off_curve), and
// only where at least two of the 8 pixels around are found too. The interface draws strokes, icons and panels; what the
// per-channel curve misses on the scene (a glint on a shoe, a lit hair strand, a floor speck: one or two pixels that
// change with the jitter) stands alone and is dropped.
float light_mask_of(float3 color, float3 h) {
    const float3 pred = curve3(h);
    const float top = (g_flags & 32) != 0 ? CURVE_AT(0).a : 1e9;
    float3 d = color - pred;

    [unroll] for (int c = 0; c < 3; ++c) {
        if (h[c] > 0.0 && (log2(h[c]) + 14.0) * 4.0 - 0.5 > top) {
            d[c] = 0.0;
        }
    }

    return smoothstep(0.045, 0.11, max(d.r, max(d.g, d.b)));
}

float fovea_light_mask(float3 cf, float2 fuv) {
    const float m = light_mask_of(cf, g_f_hud.SampleLevel(g_lin, fuv, 0).rgb);

    if (!(m > 0.0)) {
        return 0.0;
    }

    uint w;
    uint h;
    g_f_col.GetDimensions(w, h);
    const float2 t = 1.0 / float2(w, h);
    float n = 0.0;

    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) {
                const float2 uv = fuv + t * float2(dx, dy);
                float3 col = g_f_col.SampleLevel(g_lin, uv, 0).rgb;

                if ((g_flags & 16) != 0) {
                    col = to_srgb(col);
                }

                n += light_mask_of(col, g_f_hud.SampleLevel(g_lin, uv, 0).rgb);
            }
        }
    }

    return m * saturate(n - 1.0);
}

bool inside01(float2 uv) { return all(uv >= 0.0) && all(uv <= 1.0); }

// [GPU 29.10.2026] the wide view's copies are the same image except where the GUI was drawn: whether the GuiMap block of
// the texels a bilinear read at uv takes was marked this frame (without the map: everywhere)
bool gui_marked(float2 uv) {
    if ((g_flags & 128) == 0) {
        return true;
    }

    const int2 t = clamp(int2(floor(uv * float2(g_gui_size) - 0.5)), int2(0, 0), int2(g_gui_size) - 1);
    return g_ui_map[uint2(min(t / 8, int2(g_map_size) - 1))] == g_map_stamp;
}

// where the block was not marked both reads are equal and the mask is exactly 0
float wide_mask(float2 uv) {
    return gui_marked(uv) ? mask_of(g_c_hud, g_c_fin, uv) : 0.0;
}

// [PANEL HOLES 15.11.2026] How much of the scene behind the wide view's interface shows through it: the brightness of the
// copy with GUI over the copy without, as the output pass shows them (lf, lh). 1 where the interface drew nothing or only
// added light, 0 behind an opaque panel, the dimming of a translucent backdrop between. The copies are exact (the same
// image where nothing was drawn), so the share holds down to almost black; sure: how well it can be told (0 over a black
// scene: there the copies are the same whatever the interface does).
#define TR_E 0.00002
float through_of(float lh, float lf, out float sure) {
    sure = smoothstep(0.0002, 0.002, lh);
    return (min(lf, lh) + TR_E) / (lh + TR_E);
}

// the interface mask at uv (as wide_mask) and how much of the scene shows through there (tr, sure; where nothing was
// drawn both are 1: known)
// [GPU fix95] hc: the copy without GUI through the curve at uv (what centre_ui read a second time for the light the
// interface adds); set where the mask is not 0
float wide_mask_through(float2 uv, out float tr, out float sure, out float3 hc) {
    tr = 1.0;
    sure = 1.0;
    hc = 0.0;

    if (!gui_marked(uv)) {
        return 0.0;
    }

    const float3 hs = g_c_hud.SampleLevel(g_lin, uv, 0).rgb;
    const float3 fs = g_c_fin.SampleLevel(g_lin, uv, 0).rgb;
    const float3 h = curve3(hs);
    hc = h;

    // [GPU fix95] equal reads: the copy with GUI through the curve is h again (see mask_of): the share from h and h, the
    // differences 0, both masks 0
    [branch] if (all(fs == hs)) {
        const float lh0 = dot(h, LUMA);
        tr = through_of(lh0, lh0, sure);
        return 0.0;
    }

    const float3 f = curve3(fs);
    const float3 d = abs(f - h);
    tr = through_of(dot(h, LUMA), dot(f, LUMA), sure);
    // [MENUS fix84 18.11.2026] Where the interface only darkened the view (a translucent backdrop, a dark bar: no channel
    // brighter) and the share that shows through can be told, the mask is no longer set: the periphery's anti-aliased
    // scene dimmed by that share stands there instead of the wide view's own pixels of this frame. Those are one sample
    // per 4 x 4 eye pixels, with the frame's jitter: over a lit floor a dark bar was a field of 4-pixel blocks in other
    // places every frame (RE7 capture 03 of 17.11.2026 22:3x: the flicker of the finished image beside the fovea 12 times
    // that of the periphery under it). Light the interface adds (text, icons, frames) keeps the mask.
    const float3 up = max(f - h, 0.0);
    return lerp(smoothstep(0.03, 0.08, max(d.r, max(d.g, d.b))), smoothstep(0.03, 0.08, max(up.r, max(up.g, up.b))), sure);
}

// the same share alone, from the brightness only (the line search of centre_ui); where it cannot be told: 1
float wide_through(float2 uv) {
    if (!inside01(uv) || !gui_marked(uv)) {
        return 1.0;
    }

    float sure;
    const float3 hs = g_c_hud.SampleLevel(g_lin, uv, 0).rgb;
    const float3 fs = g_c_fin.SampleLevel(g_lin, uv, 0).rgb;
    const float lh = curve_of(dot(hs, LUMA), 1);
    float lf = lh; // [GPU fix95] equal reads: the same value through the curve

    [branch] if (any(fs != hs)) {
        lf = curve_of(dot(fs, LUMA), 1);
    }

    const float t = through_of(lh, lf, sure);
    return lerp(1.0, t, sure);
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- the interface (MonoUi), positions in the wide view ------------------------------------------------------------
// [MENUS fix81 17.11.2026] RE7 captures of 17.11.2026 (pause menu, inventory): a line of the menu follows the frame's jitter
// one to one in both axes (slope 1.00 - 1.06, correlation 0.99 - 1.00 over 64 frames, the scene's motion taken out): the
// engine draws the interface through the jittered projection like the scene. The eye pass put it over the anti-aliased
// picture as the frame had it: the whole interface shook by up to half a pixel of its view every frame, which is two eye
// pixels for the wide view's. The views are now read where the frame's jitter put the point (g_ui_jit).
float2 wide_uv(float tx, float ty, float z) {
    const float tcx = tx + g_eye_x / z;
    return float2((tcx - g_wide_tan.x) / (g_wide_tan.y - g_wide_tan.x), (g_wide_tan.w - ty) / (g_wide_tan.w - g_wide_tan.z)) + g_ui_jit.zw;
}

// the wide view position of eye pixel centre p at depth z (as centre_ui computes it)
float2 wide_uv_at(float2 p, float z) {
    const float2 uv = p / float2(g_out_size);
    return wide_uv(lerp(g_eye_tan.x, g_eye_tan.y, uv.x), lerp(g_eye_tan.w, g_eye_tan.z, uv.y), z);
}

// [GPU 29.10.2026] Whether a GuiMap block marked this frame lies in reach of the tile's pixels at depth z. The wide view
// position is linear in the pixel position, so the tile's first and last pixel bound it; one texel more on every side
// covers the rounding. Where no block in reach is marked, wide_mask is 0 for every pixel of the tile at this depth.
uint tile_marked(int2 a, int2 b, float z) {
    const float2 u0 = wide_uv_at(float2(a) + 0.5, z);
    const float2 u1 = wide_uv_at(float2(b) + 0.5, z);
    const int2 glast = int2(g_gui_size) - 1;
    const int2 t0 = clamp(int2(floor(min(u0, u1) * float2(g_gui_size) - 0.5)) - 1, int2(0, 0), glast);
    const int2 t1 = clamp(int2(floor(max(u0, u1) * float2(g_gui_size) - 0.5)) + 1, int2(0, 0), glast);
    const int2 mlast = int2(g_map_size) - 1;
    const int2 b0 = min(t0 / 8, mlast);
    const int2 b1 = min(t1 / 8, mlast);

    if (any(b1 - b0 >= 4) || any(b1 < b0)) {
        return 1; // not the expected few blocks: no skip
    }

    uint hit = 0;

    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            hit |= g_ui_map[uint2(min(b0 + int2(i, j), b1))] == g_map_stamp ? 1u : 0u;
        }
    }

    return hit;
}

// [ITEM BEAMS 03.11.2026] The green beam over an item (Gui_ui2130) is a world element of the game: every layer draws it
// at the item, so the wide view's copy of it belongs at the item's depth in each eye, not at the panels' (dump 01 of
// 03.11.2026: outside the fovea the beams had the disparity of 0.99 m, the egg was 6.7 m away). A beam pixel is one
// where the GUI added green (the X icon, the names and the panels add white, grey or brown). Around each beam anchor a
// column (reach across, above, below; from its depth); a beam pixel belongs to exactly one anchor, so each eye shows it
// once. [15:30] The test was too strict for the beam's bright core (dump 03 of 15:00: display difference about
// 120 / 160 / 125, green only 1.27 times red): those rows went to the panels' depth, the beam showed twice. Now: green
// above red and blue by 8 % of it and a little more.
// [GPU fix99 19.11.2026] tile_marked for a place read `s` (tangent) beside where the depth puts it: an item beam with a
// fitted place (centre_ui's `shift`)
uint tile_marked_shift(int2 a, int2 b, float z, float s) {
    const float2 pa = (float2(a) + 0.5) / float2(g_out_size);
    const float2 pb = (float2(b) + 0.5) / float2(g_out_size);
    const float2 u0 = wide_uv(lerp(g_eye_tan.x, g_eye_tan.y, pa.x) + s, lerp(g_eye_tan.w, g_eye_tan.z, pa.y), z);
    const float2 u1 = wide_uv(lerp(g_eye_tan.x, g_eye_tan.y, pb.x) + s, lerp(g_eye_tan.w, g_eye_tan.z, pb.y), z);
    const int2 glast = int2(g_gui_size) - 1;
    const int2 t0 = clamp(int2(floor(min(u0, u1) * float2(g_gui_size) - 0.5)) - 1, int2(0, 0), glast);
    const int2 t1 = clamp(int2(floor(max(u0, u1) * float2(g_gui_size) - 0.5)) + 1, int2(0, 0), glast);
    const int2 mlast = int2(g_map_size) - 1;
    const int2 b0 = min(t0 / 8, mlast);
    const int2 b1 = min(t1 / 8, mlast);

    if (any(b1 - b0 >= 4) || any(b1 < b0)) {
        return 1;
    }

    uint hit = 0;

    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            hit |= g_ui_map[uint2(min(b0 + int2(i, j), b1))] == g_map_stamp ? 1u : 0u;
        }
    }

    return hit;
}

bool beam_colour(float2 uv) {
    const float3 fs = g_c_fin.SampleLevel(g_lin, uv, 0).rgb;
    const float3 hs = g_c_hud.SampleLevel(g_lin, uv, 0).rgb;

    // [GPU fix95] equal reads: no green added
    [branch] if (all(fs == hs)) {
        return false;
    }

    const float3 d = curve3(fs) - curve3(hs);
    return d.g > 0.01 && d.g - max(d.r, d.b) > 0.08 * d.g + 0.003;
}

// what anchor i is (g_anchor_ext[i].w): 0 = an element in a circle of radius |a.w| (before 03.11), 1 = an item beam,
// 2 = [ICON STACK 03.11.2026 15:30] the item icon as a column (the icon stack: the binoculars over the button over the
// arrow reaches 0.10 - 0.17 tan above the item, dumps 01 / 02 of 15:00; in the circle of 0.08 only the arrow was, the
// rest went to the panels' depth and moved sideways)
uint anchor_kind(uint i) {
    const float w = g_anchor_ext[i].w;
    return w > 1.5 ? 2u : (w > 0.5 ? 1u : 0u);
}

// [BEAM FIT 03.11.2026] beam i as GuiMap found it in the wide view this frame: x of its column (tangent), 1 = found
float2 beam_fit(uint i) {
    if ((g_flags & 128) == 0) {
        return float2(0.0, 0.0);
    }

    const uint row = g_map_size.y;
    const uint c = i * 4u;

    if (g_ui_map[uint2(c + 3u, row)] != g_map_stamp || asfloat(g_ui_map[uint2(c + 2u, row)]) < 3.0) {
        return float2(0.0, 0.0);
    }

    return float2(asfloat(g_ui_map[uint2(c, row)]), 1.0);
}

// the centre x of anchor i's area in the wide view (a beam: its column where the wide view drew it) and its reach across
float2 area_x(uint i) {
    const float4 a = g_anchors[i];
    const float4 e = g_anchor_ext[i];

    if (anchor_kind(i) == 1u) {
        const float2 f = beam_fit(i);

        if (f.y > 0.5) {
            return float2(f.x, min(e.x, 0.025));
        }
    }

    return float2(a.x, e.x);
}

// whether wide view tangents (wx, wy) lie in anchor i's area
)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [GPU fix99 19.11.2026] in_area for an item beam whose fit (beam_fit) is already read: the same values without reading
// the map's row again
bool in_area_beam(uint i, float wx, float wy, float2 f) {
    const float4 a = g_anchors[i];
    const float4 e = g_anchor_ext[i];
    float2 cx = float2(a.x, e.x);

    if (f.y > 0.5) {
        cx = float2(f.x, min(e.x, 0.025));
    }

    const float dy = wy - a.y;
    return abs(wx - cx.x) <= cx.y && dy <= e.y && dy >= -e.z;
}

bool in_area(uint i, float wx, float wy) {
    const float4 a = g_anchors[i];

    if (anchor_kind(i) == 0u) {
        return length(float2(wx - a.x, wy - a.y)) <= abs(a.w);
    }

    const float4 e = g_anchor_ext[i];
    const float2 cx = area_x(i);
    const float dy = wy - a.y;
    return abs(wx - cx.x) <= cx.y && dy <= e.y && dy >= -e.z;
}

// whose the beam pixel at wide view tangents (wx, wy) is: inside an item icon's own area -2, else the beam whose column
// (where the wide view drew it) is closest across, -1 none
int beam_owner(float wx, float wy) {
    int owner = -1;
    float best = 1e9;

    [loop] for (uint j = 0; j < g_anchor_count; ++j) {
        const uint kind = anchor_kind(j);

        if (kind == 1u) {
            const float dx = abs(wx - area_x(j).x);

            if (dx < best && in_area(j, wx, wy)) {
                best = dx;
                owner = int(j);
            }
        } else if (g_anchors[j].w > 0.0 && in_area(j, wx, wy)) {
            return -2;
        }
    }

    return owner;
}

// cl: [MENU BAND 15.11.2026] the light the interface added at the pixel (as ui_light gives the fovea's); the rest of cc
// is the wide view's picture under the interface, a translucent panel's darkening included
// tr: [PANEL HOLES 15.11.2026] how much of the scene behind the interface shows through it at the pixel (1 without
// interface; also given where the mask is 0)
// [GPU fix99 19.11.2026] tile: 1 = p is a pixel of the group's tile and the tile's test is done (s_ui_depth, s_ui_beam)
float centre_ui(float2 p, out float3 cc, out float3 cl, out float tr, uint tile) {
    cc = 0.0;
    cl = 0.0;
    tr = 1.0;

    if ((g_flags & 1) == 0) {
        return 0.0;
    }

    const float2 uv = p / float2(g_out_size);
    const float tx = lerp(g_eye_tan.x, g_eye_tan.y, uv.x);
    const float ty = lerp(g_eye_tan.w, g_eye_tan.z, uv.y);
    // [GPU fix99 19.11.2026] RE4 test of 19.11.2026: with a small HUD (0.5 % of the map's blocks), 3 anchored elements
    // and 3 - 5 item beams the interface took 0.36 ms of the two eye passes. Every pixel of a tile with a marked block in
    // reach at ANY of those depths went through every anchor (its area) and every beam (three map reads for its fit, its
    // area) and the panel. The tile's test knows at which depths a marked block is in reach of the tile's pixels
    // (s_ui_depth; a beam at its fitted place: s_ui_beam): at the others gui_marked is false on each of them, the mask
    // 0, so they are left out. The same image: a depth left out here gave no interface before either.
    const bool tile_flags = tile != 0u && (g_flags & (128 | 32768)) == (128 | 32768);
    float z = g_panel_depth;
    float shift = 0.0; // [BEAM FIT] the wide view position read is this much right of where the depth puts it
    bool anchored = false;
    // [MARKED TEXT fix115 21.11.2026] RE4 test of fix114 (the tiles counted): the weapon HUD marks 170 - 250 of an eye's
    // 46221 tiles (0.4 - 0.5 %) and the marked tiles' pass costs 0.10 ms for the two eyes: 0.27 us a tile, about 30
    // times a tile of the eye image. The time is in this function, per pixel. UI_TEST (the marked tiles' text only), by
    // bits: 2 = an anchor at the depth of the one before it takes that one's mask (the same read: the HUD's three
    // anchors are at one depth; the same image; the text in use), and for the GPU test 1 = without the line search of
    // [PANEL HOLES] below, 4 = nothing at the panels' depth where no anchor is found. Without the macro the text is the
    // one before.
#if defined(UI_TEST) && (UI_TEST & 2)
    float last_z = -1.0;
    float last_m = 0.0;
#endif

    [loop] for (uint i = 0; i < g_anchor_count; ++i) {
        const float4 a = g_anchors[i];
        const float tcx = tx + g_eye_x / a.z;

        if (anchor_kind(i) == 1u) {
            continue; // an item beam: below
        }

        if (tile_flags && s_ui_depth[min(1u + i, 16u)] == 0u) {
            continue; // [GPU fix99] no marked block in reach of the tile at this depth
        }

        if (in_area(i, tcx, ty)) {
            const float2 auv = wide_uv(tx, ty, a.z);
#if defined(UI_TEST) && (UI_TEST & 2)
            // (auv is tx, ty and the depth: the same place, the same mask)
            if (a.z != last_z) {
                last_m = inside01(auv) ? wide_mask(auv) : 0.0;
                last_z = a.z;
            }

            if (last_m > 0.02) {
                z = a.z;
                anchored = true;
                break;
            }
#else

            if (inside01(auv) && wide_mask(auv) > 0.02) {
                z = a.z;
                anchored = true;
                break;
            }
#endif
        }
    }

    // [ITEM BEAMS 03.11.2026] no anchored element here: a beam pixel at its item's depth. [BEAM FIT] Where GuiMap found
    // the beam, it is read that much to the side so that it stands over its anchor (its item) in each eye.
    if (!anchored) {
        [loop] for (uint k = 0; k < g_anchor_count; ++k) {
            if (anchor_kind(k) != 1u) {
                continue;
            }

            if (tile_flags && s_ui_beam[min(1u + k, 16u)] == 0u) {
                continue; // [GPU fix99] none at its fitted place
            }

            const float4 a = g_anchors[k];
            const float2 f = beam_fit(k);
            const float s = f.y > 0.5 ? f.x - a.x : 0.0;
            const float tcx = tx + g_eye_x / a.z + s;

            if (!in_area_beam(k, tcx, ty, f)) {
                continue;
            }

            const float2 auv = wide_uv(tx + s, ty, a.z);

            // [GPU fix99] one test after the other: `&&` works out all four for every pixel of the beam's area (the
            // two reads of beam_colour, every anchor in beam_owner) also where the mask is 0
            BR if (inside01(auv)) {
                BR if (wide_mask(auv) > 0.02) {
                    BR if (beam_colour(auv)) {
                        BR if (beam_owner(tcx, ty) == int(k)) {
                            z = a.z;
                            shift = s;
                            anchored = true;
                            break;
                        }
                    }
                }
            }
        }
    }

#if defined(UI_TEST) && (UI_TEST & 4)
    if (!anchored) {
        return 0.0;
    }
#endif
    const float2 cuv = wide_uv(tx + shift, ty, z);

    if (!inside01(cuv)) {
        return 0.0;
    }

    // [GPU fix99] no anchor, and no marked block in reach of the tile at the panels' depth: gui_marked(cuv) is false,
    // wide_mask_through gives mask 0, share 1, sure 1 (nothing below changes them)
    if (tile_flags && !anchored && s_ui_depth[0] == 0u) {
        return 0.0;
    }

    float sure;
    float3 hc;
    float mc = wide_mask_through(cuv, tr, sure, hc);

    if (!anchored && (mc > 0.0 || tr < 1.0)) {
        const float tcp = tx + g_eye_x / z;

        bool beam_read = false; // [GPU fix99] beam_colour(cuv) once, and only inside a beam's area (it was read per beam)
        bool beam_is = false;

        [loop] for (uint j = 0; j < g_anchor_count; ++j) {
            // [ITEM BEAMS 03.11.2026] a beam pixel is shown at its item's depth (its beam's column holds it)
            if (anchor_kind(j) == 1u) {
                BR if (in_area(j, tcp, ty)) {
                    BR if (!beam_read) {
                        beam_is = beam_colour(cuv);
                        beam_read = true;
                    }

                    if (beam_is) {
                        mc = 0.0;
                        tr = 1.0;
                        sure = 1.0;
                        break;
                    }
                }

                continue;
            }

            if (g_anchors[j].w > 0.0 && in_area(j, tcp, ty)) {
                mc = 0.0;
                tr = 1.0;
                sure = 1.0;
                break;
            }
        }
    }

    // [PANEL HOLES 15.11.2026] RE7 dumps 16 and 17 of 15.11.2026 21:3x (pause menu across the fovea's top edge): holes in
    // the menu's black panel outside the fovea and in the blend band, the scene (beams, cobwebs) seen through it, while
    // the wide view's picture has the panel whole. Outside the fovea the pixel was the periphery by (1 - mask) and the
    // wide view's picture by the mask; the mask (the copies' difference) misses the panel where the scene under it at the
    // panels' depth is dark (a black panel over a dark beam changes the copies by less than its threshold), while the
    // periphery there is the scene this eye sees at the scene's depth: another, bright place of the wide view (by up to
    // 9 texels of it). On the dumps' images: in the panel's inside, outside the fovea, 41 % of the pixels whose scene under
    // the panel is below 0.01 in the dump's picture of the copy kept more than 30 % of the periphery (0 - 3 % where it is
    // brighter). Now the periphery's part is weighed by tr too (an opaque panel lets nothing of it through, a translucent
    // backdrop its dimming). Emulated on the dumps' images: the panel's inside outside the fovea at 0.007 / 0.008 / 0.007
    // instead of 0.034 / 0.073 / 0.037 (99th percentile; dump 16 L / R, dump 17 L). Over a black scene at cuv tr cannot be
    // told; the scene this eye sees there lies on the wide view's line from cuv (the panels' depth) towards where an
    // infinitely far point would be (and a third of it the other way: things nearer than the panels), so there the lowest
    // tr along that line that can be told counts (a panel lets nothing through anywhere on it).
#if !(defined(UI_TEST) && (UI_TEST & 1))
    BR if (sure < 1.0 && tr > 0.02) {
        const float2 step = float2(-g_eye_x / (z * (g_wide_tan.y - g_wide_tan.x)), 0.0);
        float t_line = 1.0;

        [unroll] for (int k = -1; k <= 3; ++k) {
            if (k != 0) {
                t_line = min(t_line, wide_through(cuv + step * (float(k) / 3.0)));
            }
        }

        tr = lerp(t_line, tr, sure);
    }
#endif

    // nothing of this view's interface here: its colour would only be multiplied by zero
    if (!(mc > 0.0)) {
        return 0.0;
    }

    cc = g_c_col.SampleLevel(g_lin, cuv, 0).rgb;

    if ((g_flags & 8) != 0) {
        cc = to_srgb(cc);
    }

    cl = max(cc - hc, 0.0); // [MENU BAND 15.11.2026] ([GPU fix95] hc: the same read at cuv through the curve, made by wide_mask_through)
    return mc;
}

float fovea_ui(float2 p, out float3 cf) {
    cf = 0.0;

    // ([MENUS fix82] flag 8388608: the fovea image holds its interface already)
    if ((g_flags & 2) == 0 || (g_flags & 8388608) != 0 || g_rect.z <= 0.0 || g_rect.w <= 0.0) {
        return 0.0;
    }

    float2 fuv = (p - g_rect.xy) / g_rect.zw;

    if (!inside01(fuv)) {
        return 0.0;
    }

    fuv += g_ui_jit.xy; // [MENUS fix81] where the frame's jitter put the point

    cf = g_f_col.SampleLevel(g_lin, fuv, 0).rgb;

    if ((g_flags & 16) != 0) {
        cf = to_srgb(cf);
    }

#if defined(PREV_2251) || defined(BEFORE_1031)
    const float m = (g_flags & 64) != 0 ? fovea_light_mask(cf, fuv) : mask_of(g_f_hud, g_f_hud, fuv);
#else
    // [GPU 31.10.2026 night] one side only (the ternary made both: two more samples and six curve lookups per pixel)
    float m;

    BR if ((g_flags & 64) != 0) {
        m = fovea_light_mask(cf, fuv);
    } else {
        m = mask_of(g_f_hud, g_f_hud, fuv);
    }
#endif

    cf = ui_light(g_f_hud, cf, fuv); // [TRANSLUCENT 11.11.2026]
    return m;
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- [MENUS fix83 17.11.2026] the interface inside the fovea image, told by the fovea view's two copies ------------------
// fix82 left the interface in the fovea views and their image, and nothing told any more where it is. RE7 tests of
// 17.11.2026 19:2x: a band of light in the save menu (the large-scale light took the menu's panel, which the fovea view
// draws wider than the wide view, for a darker fovea), and "tint interface sources" showed no green. The engine's copies
// of a fovea view before and after its GUI pass are the same image except where the GUI drew (as the wide view's, GuiMap):
// whether they differ tells it exactly, at the view's own pixels, for any menu.
// The fovea views draw the interface the wide view draws: in a frame in which the wide view drew none (the last texel of
// the GuiMap's extra row does not hold the frame's stamp, as UiClean reads it) nothing of this is done, so a frame of plain
// play cannot be touched by it. (Without the map: not known, taken as drawn.)
bool wide_ui_frame() {
    if ((g_flags & 128) == 0) {
        return true;
    }

    uint mw;
    uint mh;
    g_ui_map.GetDimensions(mw, mh);
    return g_ui_map[uint2(mw - 1, mh - 1)] == g_map_stamp;
}

// Whether the fovea view drew interface at rectangle uv (bilinear reads of both copies: the same where all four texels are)
bool fovea_drawn(float2 fuv) {
    bool drawn = false;

    BR if ((g_flags & 16777216) != 0 && inside01(fuv)) {
        BR if (wide_ui_frame()) {
            drawn = any(g_f_col.SampleLevel(g_lin, fuv, 0).rgb != g_f_hud.SampleLevel(g_lin, fuv, 0).rgb);
        }
    }

    return drawn;
}

// [BACKDROP fix83 17.11.2026] The factor that takes a backdrop only the fovea view draws out of the fovea's pixel at eye
// pixel centre p. RE7 dumps of 17.11.2026 19:2x (inventory): the fovea view draws the inventory's dark strip down to its
// lower edge, the wide view ends it about 60 pixels higher (and the save menu's panel starts more than 100 pixels further
// left in the fovea view): the engine clips these backdrops in the wide view only. Where the gaze is, the strip ended at
// the fovea's edge and moved with it (15.11.2026); the periphery's pixels put there instead (MENU BACKDROP) were pieces of
// the periphery inside the fovea, chosen by blocks, flickering (17.11.2026). The fovea view's own copies give the dimming
// exactly: the brightness with GUI over the one without, as the output pass shows them (tf, as the wide view's tr). Where
// the interface only darkened the pixel (no channel brighter: not text, not an icon's light) and the wide view lets more
// through at the place (tr; 1 where it drew nothing), the fovea's pixel is raised by tr / tf: the fovea's own sharp
// picture with the wide view's backdrop. Nothing where the wide view has interface of its own (mc), where the scene under
// it is black (nothing to tell, nothing to raise), or where the two agree within 10 % (from 40 % on in full). At most 8x.
// The wide view's share is the least within one and a half of its texels around the place, so the edge of a backdrop
// both views draw stays the fovea's own sharp one.
float fovea_backdrop(float2 p) {
    float gain = 1.0;
    float2 fuv = (p - g_rect.xy) / max(g_rect.zw, 1.0);

    // (only with the wide view's copies and its map: without them nothing says what the wide view lets through)
    BR if ((g_flags & (33554432 | 128 | 1)) == (33554432 | 128 | 1) && g_rect.z > 0.0 && g_rect.w > 0.0 && inside01(fuv)) {
        BR if (wide_ui_frame()) {
            fuv += g_ui_jit.xy; // where the frame's jitter put the point
            const float3 f = g_f_col.SampleLevel(g_lin, fuv, 0).rgb;
            const float3 h = g_f_hud.SampleLevel(g_lin, fuv, 0).rgb;

            // drawn on, and no channel brighter than before
            BR if (any(f != h) && !any(f > h * 1.002 + 0.000001)) {
                float sure;
                const float tf = through_of(curve_of(dot(h, LUMA), 1), curve_of(dot(f, LUMA), 1), sure);

                BR if (tf < 0.95 && sure > 0.0) {
                    float3 cc;
                    float3 cl;
                    float tr;
                    const float mc = centre_ui(p, cc, cl, tr, 0u); // (p can be the pixel beside the tile: the whole way)

                    // the least the wide view lets through within one and a half of its texels: its backdrop's own
                    // edge (soft, four eye pixels a texel, and placed by the panels' depth) leaves the fovea's alone
                    const float2 wuv = wide_uv_at(p, g_panel_depth);
                    const float2 wt = 1.5 / float2(g_gui_size);

                    [unroll] for (int k = 0; k < 9; ++k) {
                        if (k != 4) {
                            tr = min(tr, wide_through(wuv + wt * float2(float(k % 3 - 1), float(k / 3 - 1))));
                        }
                    }

                    const float r = min(tr / max(tf, 0.02), 8.0);
                    gain = lerp(1.0, r, sure * (1.0 - mc) * smoothstep(1.1, 1.4, r));
                }
            }
        }
    }

    return gain;
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- [LIGHT 01.11.2026, BLOCKS 02.11.2026] the rectangle in blocks: tone table, gain, large-scale light ----------------
// One pass over the whole rectangle in blocks of 16 x 16 pixels (blocks) gives everything the matching needs: each
// block's mean of the periphery (as the eye image has it) and of the fovea (its own and matched with the last frame's
// gain / tone table), and the two brightness histograms of the whole blocks. One group per eye (match) then makes the
// gain and the tone table of the next frame from them (the eye pass uses the last frame's: the blocks were matched with
// it), and light_blur the large-scale light: the difference periphery - fovea blurred over the blocks (gaussian of
// g_light_p.x blocks, blocks weighted by their part inside the rectangle), limited to 35 % of the level, added to the
// fovea, between the blocks bilinearly. Only a smooth light: the fovea's detail stays as it is (no blur, same contrast).
// (Removed at the reset of 28.10: then 8 x 8 / 16 x 16 blocks without blur, which carried the periphery's shimmering
// into the fovea; the blur over 32-64 pixels averages it out.) Before 02.11 the table came from the measure's 64 x 64
// points (3 x 3 reads each, +0.017 ms per eye); from the blocks it matches as well or better (dumps of 30.10-01.11).
#define LIGHT_BLOCK 16
#define LIGHT_MAX   128                          // blocks per side (rectangles up to 2048 pixels)
#define LIGHT_SLOT  (LIGHT_MAX * LIGHT_MAX)      // per eye slot
#define LIGHT_R     8                            // blocks each way
#define LIGHT_LIMIT 0.35                         // of the blurred level
RWStructuredBuffer<float4> g_lsum  : register(u4); // per eye slot (2 x LIGHT_SLOT): the blocks' periphery means (w = part inside),
                                                   // then the fovea's matched with the last frame's gain / table (w = its own
                                                   // brightness)
RWStructuredBuffer<float4> g_light : register(u5); // per eye slot (LIGHT_SLOT): the light added at each block (rgb); after
                                                   // both slots, per slot: the measured difference (match, flag 524288)
RWByteAddressBuffer        g_hist  : register(u6); // per eye slot 2 x TONE_BINS counts: the whole blocks' brightness (periphery,
                                                   // fovea's own); match reads and empties them

uint2 light_blocks() {
    return (g_rect_size + (LIGHT_BLOCK - 1)) / LIGHT_BLOCK;
}

uint tone_bin(float y) {
    return min(uint(sqrt(saturate(y)) * float(TONE_BINS)), TONE_BINS - 1u);
}

// the light at pixel id of the rectangle, between the four nearest blocks (w: [MENU BACKDROP 15.11.2026] the menu's
// replacement weight)
float4 light4_at(int2 id) {
    const int2 last = int2(light_blocks()) - 1;
    const float2 b = (float2(id) + 0.5) / float(LIGHT_BLOCK) - 0.5;
    const int2 b0 = int2(floor(b));
    const float2 t = b - float2(b0);
    const int2 i0 = clamp(b0, int2(0, 0), last);
    const int2 i1 = clamp(b0 + 1, int2(0, 0), last);
    const uint base = g_slot * LIGHT_SLOT;
    const float4 a00 = g_light[base + uint(i0.y) * LIGHT_MAX + uint(i0.x)];
    const float4 a10 = g_light[base + uint(i0.y) * LIGHT_MAX + uint(i1.x)];
    const float4 a01 = g_light[base + uint(i1.y) * LIGHT_MAX + uint(i0.x)];
    const float4 a11 = g_light[base + uint(i1.y) * LIGHT_MAX + uint(i1.x)];
    return lerp(lerp(a00, a10, t.x), lerp(a01, a11, t.x), t.y);
}

float3 light_at(int2 id) {
    return light4_at(id).rgb;
}

groupshared float4 s_lp[256];
groupshared float4 s_lf[256];
groupshared float  s_ls[256]; // [AUTO LIGHT 14.11.2026] the periphery's seen part (its alpha with the hole band)
groupshared float  s_lt[TONE_N];
groupshared float  s_lg;

// [02.11.2026 evening] one group of 16 x 16 threads per 4 x 4 blocks; each thread the 4 x 4 pixels of its place as four
// quads of 2 x 2 (one bilinear read of each image at each quad's middle); each block's 16 threads reduce in their own
// part of shared memory (before: 2 x 2 blocks per group, one quad per thread: four times the reduction work)
[numthreads(16, 16, 1)]
void blocks(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    // the last frame's table and gain (what the eye pass of this frame uses); nothing usable yet = 1
    if (gi < TONE_N) {
        const float v = g_gain[uint2(g_slot * TONE_SLOT + g_parity * TONE_N + gi, 1)];
        s_lt[gi] = (v > 0.0 && v < 10.0) ? v : 1.0;
    }

    if (gi == 0) {
        const float v = g_gain[uint2(g_slot * 4 + g_parity, 0)];
        s_lg = (v > 0.0 && v < 10.0) ? v : 1.0;
    }

    GroupMemoryBarrierWithGroupSync();

    const uint2 blk = gid.xy * 4 + tid.xy / 4;
    const uint seg = ((tid.y / 4) * 4 + tid.x / 4) * 16;
    const uint loc = (tid.y % 4) * 4 + tid.x % 4;
    const int2 size = int2(g_rect_size);
    float4 sp = 0.0;
    float4 sf = 0.0;
    float ss = 0.0;

    [unroll] for (uint q = 0; q < 4; ++q) {
        const int2 p0 = int2(blk * LIGHT_BLOCK + (tid.xy % 4) * 4 + uint2(q % 2, q / 2) * 2);
        const int2 lo = min(p0, size);
        const int2 hi = min(p0 + 2, size);
        const float n = float((hi.x - lo.x) * (hi.y - lo.y));

        if (n > 0.0) {
            const float2 c = float2(lo + hi) * 0.5; // the middle of the pixels inside (rectangle pixels, corners at integers)
            float3 f = g_fov.SampleLevel(g_lin, c / float2(size), 0).rgb;
            const float fr = dot(f, LUMA); // the fovea's own brightness (without gain or table)

            // the fovea as the composite takes it: with the gain or the table
            if ((g_flags & 2048) != 0) {
                if ((g_flags & 131072) != 0) {
                    const float t = sqrt(saturate(fr)) * float(TONE_N - 1);
                    const uint i = min(uint(t), TONE_N - 2u);
                    f *= lerp(s_lt[i], s_lt[i + 1], t - float(i));
                } else {
                    f *= s_lg;
                }
            }

            // the periphery: its position in the eye image as a fraction is its position in the periphery image
            const float4 pa = g_per.SampleLevel(g_lin, (float2(g_rect_pos) + c) / float2(g_out_size), 0);
            float3 p = pa.rgb;
            ss += ((g_flags & 2097152) != 0 ? saturate(pa.a) : 1.0) * n;
            // [HOLES 15.11.2026] the periphery's gaps (alpha 0: background the wide view saw beside a near object, or the
            // magenta of "mark reprojection holes") are not light: a quad counts only where the periphery saw all of it,
            // in both sums (periphery and fovea over the same pixels). Before, the gaps' colour went into the blocks and the
            // large-scale light carried it into the middle of the fovea (RE7 dumps 03-08 of 14.11.2026: leaves, bars and
            // grass tinted purple with the holes marked; +0.03 magenta over the fovea's middle)
            float seen_w = (g_flags & 2097152) != 0 ? saturate(pa.a * 4.0 - 3.0) : 1.0;

            // [MENUS fix82] the fovea image holds its view's interface, the periphery image none: where the wide view has
            // interface at the panels' depth (its GuiMap block marked) the quad is left out of both sums, or the large-
            // scale light would take the interface's light for a difference between the views and darken the fovea around
            // it. (In a game menu the light is paused and the blocks only tell where the menu's backdrop is: all quads.)
            if ((g_flags & (8388608 | 4194304 | 128 | 1)) == (8388608 | 128 | 1) &&
                gui_marked(wide_uv_at(float2(g_rect_pos) + c, g_panel_depth))) {
                seen_w = 0.0;
            }

            // [MENUS fix83] ... and where the fovea view itself drew interface (its copies differ): exact, at its own
            // pixels, also where it draws a panel wider than the wide view does (the save menu's band of light)
            BR if (fovea_drawn(c / float2(size))) {
                seen_w = 0.0;
            }

            if ((g_flags & 512) != 0) {
                p = to_srgb(p);
            }

            if ((g_flags & 16384) != 0) {
                p = saturate(p);
            }

            sp += float4(p * (n * seen_w), n * seen_w);
            sf += float4(f * (n * seen_w), fr * (n * seen_w));
        }
    }

    s_lp[seg + loc] = sp;
    s_lf[seg + loc] = sf;
    s_ls[seg + loc] = ss;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = 8; step > 0; step >>= 1) {
        if (loc < step) {
            s_lp[seg + loc] += s_lp[seg + loc + step];
            s_lf[seg + loc] += s_lf[seg + loc + step];
            s_ls[seg + loc] += s_ls[seg + loc + step];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    const uint2 nb = light_blocks();

    if (loc == 0 && blk.x < nb.x && blk.y < nb.y) {
        const float w = s_lp[seg].w;
        const uint b = g_slot * 2 * LIGHT_SLOT + blk.y * LIGHT_MAX + blk.x;
        // [AUTO LIGHT 14.11.2026] a block counts by the part the periphery saw ([HOLES 15.11.2026] w is already that part: the
        // sums above leave the gaps out, for the automatic and the fixed limit alike)
        g_lsum[b] = w > 0.0 ? float4(s_lp[seg].rgb / w, w / float(LIGHT_BLOCK * LIGHT_BLOCK)) : 0.0;
        g_lsum[b + LIGHT_SLOT] = w > 0.0 ? s_lf[seg] / w : 0.0;

        // the whole blocks into the brightness histograms
        if (w >= float(LIGHT_BLOCK * LIGHT_BLOCK)) {
            const uint h0 = g_slot * 2 * TONE_BINS;
            g_hist.InterlockedAdd((h0 + tone_bin(dot(s_lp[seg].rgb, LUMA) / w)) * 4, 1u);
            g_hist.InterlockedAdd((h0 + TONE_BINS + tone_bin(s_lf[seg].w / w)) * 4, 1u);
        }
    }
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- match: one group per eye, after the blocks -------------------------------------------------------------------
// 1. the gain: all blocks' periphery over the fovea's own brightness (weighted by their part inside), smoothed in log
// space from the last frame's; 2. the tone table: the two histograms' brightness at the same 17 quantiles (inside a bin
// linearly), per table entry the periphery's brightness at the fovea's (linear between the quantiles, beyond them the
// factor of the last one) over the fovea's, limited to 0.25 .. 4, smoothed like the gain; 3. (flag 524288) how different
// the two views are over the blocks. The next frame's values go to the slot's other ping-pong place; "this frame's"
// places get the last frame's. [02.11.2026 evening] 1024 threads (the sums in 5 instead of 73 rounds for 78 x 60 blocks),
// the quantiles from the histograms' running sums by one thread each (before: two threads through all bins); the eye
// pass runs at the same time (it reads the last frame's places, match writes the others).
#define MATCH_T 1024
groupshared float4 s_mx[MATCH_T];
groupshared float  s_th[2 * TONE_BINS];
groupshared float  s_tc[2 * TONE_BINS];  // running sums of the counts (each histogram on its own)
groupshared float  s_tq[2 * TONE_KNOTS]; // periphery's quantiles, then the fovea's

[numthreads(MATCH_T, 1, 1)]
void match(uint gi : SV_GroupIndex) {
    const uint2 nb = light_blocks();
    const uint count = nb.x * nb.y;
    const uint base = g_slot * 2 * LIGHT_SLOT;
    float4 a = 0.0;

    // [MENU GAIN 15.11.2026] in a game menu (the large-scale light paused) one gain brings the fovea to the periphery's
    // exposure; the blocks of the menu's backdrop (the fovea far darker, 0.2 - 0.35 in the dumps) and others that do not
    // agree are left out of it. RE7 dump of 15.11.2026 14:41 (inventory in front of a lamp): the rest of the fovea was
    // 0.85 - 0.95 of the periphery, its edges lighter.
    const bool menu = (g_flags & 4194304) != 0;

    for (uint i = gi; i < count; i += MATCH_T) {
        const uint b = base + (i / nb.x) * LIGHT_MAX + (i % nb.x);
        const float4 pm = g_lsum[b];
        const float4 fm = g_lsum[b + LIGHT_SLOT];
        const float lp = dot(pm.rgb, LUMA);
        float wb = pm.w;

        if (menu) {
            const float r = (fm.w + 0.002) / (lp + 0.002);
            wb *= r > 0.45 && r < 2.2 ? 1.0 : 0.0;
        }

        a += float4(wb * lp, wb * fm.w, wb, 0.0);
    }

    s_mx[gi] = a;

    // the histograms read and emptied (the next frame's blocks add to them)
    if (gi < 2 * TONE_BINS) {
        const uint addr = (g_slot * 2 * TONE_BINS + gi) * 4;
        const float c = float(g_hist.Load(addr));
        g_hist.Store(addr, 0u);
        s_th[gi] = c;
        s_tc[gi] = c;
    }

    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint step = MATCH_T / 2; step > 0; step >>= 1) {
        if (gi < step) {
            s_mx[gi] += s_mx[gi + step];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // the running sums (counts are whole numbers: exact in float)
    [unroll] for (uint off = 1; off < TONE_BINS; off <<= 1) {
        const float add = (gi < 2 * TONE_BINS && (gi % TONE_BINS) >= off) ? s_tc[gi - off] : 0.0;
        GroupMemoryBarrierWithGroupSync();

        if (gi < 2 * TONE_BINS) {
            s_tc[gi] += add;
        }

        GroupMemoryBarrierWithGroupSync();
    }

    const float4 t = s_mx[0];
    const float one_gain = t.y > 1e-6 ? t.x / t.y : 1.0;

    // 1. the gain
    if (gi == 0) {
        const float measured = t.y > 1e-6 ? clamp(t.x / t.y, 0.33, 3.0) : 1.0;
        const float old = g_gain[uint2(g_slot * 4 + g_parity, 0)];
        const bool ok = old > 0.0 && old < 10.0;
        g_gain[uint2(g_slot * 4 + (g_parity ^ 1), 0)] = ok ? exp(lerp(log(old), log(measured), g_rate)) : measured;
        g_gain[uint2(g_slot * 4 + 2, 0)] = measured;
        g_gain[uint2(g_slot * 4 + 3, 0)] = ok ? old : 1.0;
    }

    // 2. the tone table: quantile j of histogram h by one thread: the first bin whose running sum passes it (none: 1)
    if (gi < 2 * TONE_KNOTS) {
        const uint h0 = (gi / TONE_KNOTS) * TONE_BINS;
        const uint j = gi % TONE_KNOTS;
        const float total = s_tc[h0 + TONE_BINS - 1];
        const float q = 0.5 + float(j) * ((total - 1.0) / float(TONE_KNOTS - 1));
        float v = 1.0;

        if (total >= 1.0) {
            uint b = 0;

            [unroll] for (uint st = TONE_BINS / 2; st > 0; st >>= 1) {
                if (s_tc[h0 + b + st - 1] <= q) {
                    b += st;
                }
            }

            const float above = s_tc[h0 + b];

            if (above > q) {
                const float cnt = s_th[h0 + b];
                const float u = (q - (above - cnt)) / cnt;
                const float x = (float(b) + u) / float(TONE_BINS);
                v = x * x;
            }
        }

        s_tq[gi] = v;
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi < TONE_N) {
        const float x = float(gi) / float(TONE_N - 1);
        const float y = x * x;
        const float f0 = s_tq[TONE_KNOTS];
        const float fn = s_tq[2 * TONE_KNOTS - 1];
        float r;

        if (y <= f0) {
            r = s_tq[0] / max(f0, 1e-6);
        } else if (y >= fn) {
            r = s_tq[TONE_KNOTS - 1] / max(fn, 1e-6);
        } else {
            float tt = s_tq[TONE_KNOTS - 1];

            [loop] for (uint k = 0; k + 1 < TONE_KNOTS; ++k) {
                const float qa = s_tq[TONE_KNOTS + k];
                const float qc = s_tq[TONE_KNOTS + k + 1];

                if (y >= qa && y < qc) {
                    tt = lerp(s_tq[k], s_tq[k + 1], (y - qa) / (qc - qa));
                    break;
                }
            }

            r = tt / y;
        }

        r = clamp(r, 0.25, 4.0);
        const float old_t = g_gain[uint2(g_slot * TONE_SLOT + g_parity * TONE_N + gi, 1)];
        const bool ok_t = old_t > 0.0 && old_t < 10.0;
        g_gain[uint2(g_slot * TONE_SLOT + (g_parity ^ 1) * TONE_N + gi, 1)] = ok_t ? exp(lerp(log(old_t), log(r), g_rate)) : r;
        g_gain[uint2(g_slot * TONE_SLOT + 2 * TONE_N + gi, 1)] = ok_t ? old_t : 1.0;
    }

    // 3. [VIEW DIFFERENCE] x = mean |periphery - fovea x one gain| over the mean periphery (the views' own difference),
    // y = the same with the fovea matched as the eye pass does (before the light), z = mean periphery, w = the one gain
    if ((g_flags & 524288) != 0) {
        float4 d = 0.0;

        for (uint i2 = gi; i2 < count; i2 += MATCH_T) {
            const uint b = base + (i2 / nb.x) * LIGHT_MAX + (i2 % nb.x);
            const float4 pm = g_lsum[b];
            const float4 fm = g_lsum[b + LIGHT_SLOT];
            const float lp = dot(pm.rgb, LUMA);
            d += float4(pm.w * abs(lp - one_gain * fm.w), pm.w * abs(lp - dot(fm.rgb, LUMA)), 0.0, 0.0);
        }

        GroupMemoryBarrierWithGroupSync();
        s_mx[gi] = d;
        GroupMemoryBarrierWithGroupSync();

        [unroll] for (uint step2 = MATCH_T / 2; step2 > 0; step2 >>= 1) {
            if (gi < step2) {
                s_mx[gi] += s_mx[gi + step2];
            }

            GroupMemoryBarrierWithGroupSync();
        }

        if (gi == 0) {
            const float level = max(t.x, 1e-6);
            g_light[2 * LIGHT_SLOT + g_slot] = float4(s_mx[0].x / level, s_mx[0].y / level, t.z > 0.0 ? t.x / t.z : 0.0, one_gain);
        }
    }
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [MENU HOLES 15.11.2026] the brightness of the wide view's own picture (its interface included, as centre_ui reads it)
// over block b of the rectangle, at the panels' depth: the darkest of four reads at the block's quarters. The fovea image
// of the blocks has no interface light (its text and lines come in step 5), the wide view's picture has it: with the mean,
// the text and the white bar of the pause menu made the panel's blocks brighter there than the fovea's (dump 05,
// emulated: 290 blocks of 3780 still replaced, around the buttons); with the darkest read the panel under the text counts
// (109, the white bar's, which the pixel gate keeps). lp (the periphery's) where the wide view's copy is missing or the
// block lies outside the wide view.
float wide_block_luma(int2 b, float lp) {
    if ((g_flags & 1) == 0) {
        return lp;
    }

    float m = 1e9;

    [unroll] for (uint k = 0; k < 4; ++k) {
        const float2 p = float2(g_rect_pos + b * LIGHT_BLOCK) + float2(4.0 + 8.0 * float(k % 2u), 4.0 + 8.0 * float(k / 2u));
        const float2 uv = wide_uv_at(p, g_panel_depth);

        if (inside01(uv)) {
            float3 c = g_c_col.SampleLevel(g_lin, uv, 0).rgb;

            if ((g_flags & 8) != 0) {
                c = to_srgb(c);
            }

            m = min(m, dot(c, LUMA));
        }
    }

    return m < 1e8 ? m : lp;
}

// ---- light_blur: one thread per block, the group's 8 x 8 blocks and LIGHT_R around them read once (24 x 24, outside the
// rectangle weight 0); the gaussian along x per row of the tile, then along y (the same sums as the 2-D blur, 34 instead
// of 289 products per block)
#define LIGHT_T (8 + 2 * LIGHT_R)
groupshared float4 s_bp[LIGHT_T * LIGHT_T];
groupshared float3 s_bf[LIGHT_T * LIGHT_T];
groupshared float4 s_hd[LIGHT_T * 8]; // per tile row and output column: sum g w (periphery - fovea), sum g w
groupshared float2 s_hl[LIGHT_T * 8]; //   sum g w luma periphery, sum g w luma fovea
groupshared float  s_gw[2 * LIGHT_R + 1];

[numthreads(8, 8, 1)]
void light_blur(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 nb = int2(light_blocks());
    const int2 o = int2(gid.xy) * 8 - LIGHT_R;
    const uint base = g_slot * 2 * LIGHT_SLOT;

    if (gi < 2 * LIGHT_R + 1) {
        const float sigma = max(g_light_p.x, 0.5);
        const float dd = float(int(gi) - LIGHT_R);
        s_gw[gi] = exp(-0.5 * dd * dd / (sigma * sigma));
    }

    for (uint i = gi; i < LIGHT_T * LIGHT_T; i += 64) {
        const int2 b = o + int2(int(i % LIGHT_T), int(i / LIGHT_T));
        float4 pm = 0.0;
        float3 fm = 0.0;

        if (b.x >= 0 && b.y >= 0 && b.x < nb.x && b.y < nb.y) {
            pm = g_lsum[base + uint(b.y) * LIGHT_MAX + uint(b.x)];
            fm = g_lsum[base + LIGHT_SLOT + uint(b.y) * LIGHT_MAX + uint(b.x)].rgb;
        }

        s_bp[i] = pm;
        s_bf[i] = fm;
    }

    GroupMemoryBarrierWithGroupSync();

    // along x: every row of the tile, for the group's 8 columns
    for (uint j = gi; j < LIGHT_T * 8; j += 64) {
        const uint row = j / 8;
        const uint col = j % 8;
        float4 hd = 0.0;
        float2 hl = 0.0;

        [loop] for (uint k = 0; k < 2 * LIGHT_R + 1; ++k) {
            const uint at = row * LIGHT_T + col + k;
            const float4 pm = s_bp[at];
            const float3 fm = s_bf[at];
            const float w = pm.w * s_gw[k];
            hd += float4(w * (pm.rgb - fm), w);
            hl += float2(w * dot(pm.rgb, LUMA), w * dot(fm, LUMA));
        }

        s_hd[j] = hd;
        s_hl[j] = hl;
    }

    GroupMemoryBarrierWithGroupSync();

    const int2 id = int2(gid.xy * 8 + tid.xy);

    if (id.x >= nb.x || id.y >= nb.y) {
        return;
    }

    // along y
    float4 sd = 0.0;
    float2 sl = 0.0;

    [loop] for (uint k2 = 0; k2 < 2 * LIGHT_R + 1; ++k2) {
        const uint at = (tid.y + k2) * 8 + tid.x;
        sd += s_gw[k2] * s_hd[at];
        sl += s_gw[k2] * s_hl[at];
    }

    float3 res = 0.0;

    if (sd.w > 0.0) {
        // [GLOW 13.11.2026] RE7: the glow the game draws around the sun only in a view that has the sun in it (the fovea
        // views without it: the periphery up to 3.5x brighter at the top of the fovea) is far above the 35 % limit
        const bool automatic = g_light_p.w > 0.5;
        const float limit = g_light_p.y > 0.0 ? g_light_p.y : (automatic ? 4.0 : LIGHT_LIMIT);
        const float level = max(sl.x, sl.y) / sd.w;
        float3 est = sd.rgb / sd.w;

        // [AUTO LIGHT 14.11.2026] automatic: light is smooth, so a block whose difference does not agree with the light
        // around it shows something else (an edge seen from another place, a moving thing, the interface, the periphery's
        // shimmer). Two rounds of a robust average over the same window: each block's weight falls with how far its
        // difference is from the current estimate (1 / (1 + (r / (0.3 level))^2)). A smooth glow keeps all its blocks and
        // passes whole; single blocks that disagree hardly count. The limit stays only as a high safety bound (4x).
        if (automatic) {
            const float scale = 0.3 * max(level, 0.02);

            [loop] for (uint it = 0; it < 2; ++it) {
                const float e = dot(est, LUMA);
                float4 acc = 0.0;

                [loop] for (uint ky = 0; ky < 2 * LIGHT_R + 1; ++ky) {
                    [loop] for (uint kx = 0; kx < 2 * LIGHT_R + 1; ++kx) {
                        const uint at = (tid.y + ky) * LIGHT_T + tid.x + kx;
                        const float4 pm = s_bp[at];
                        const float3 dd = pm.rgb - s_bf[at];
                        const float r = (dot(dd, LUMA) - e) / scale;
                        const float wr = pm.w * s_gw[kx] * s_gw[ky] / (1.0 + r * r);
                        acc += float4(wr * dd, wr);
                    }
                }

                if (acc.w > 1e-5) {
                    est = acc.rgb / acc.w;
                }
            }
        }

        const float lim = limit * level + 0.003;
        res = clamp(est, -lim, lim);

        if (g_light_p.z > 0.5) {
            res = max(res, 0.0);
        }
    }

    // [MENU BACKDROP 15.11.2026] in a game menu: the share of this block where the fovea's scene is replaced by the
    // periphery's. RE7 dumps of 15.11.2026 (inventory): the fovea view draws the menu's dark backdrop over its whole height,
    // the wide view only inside the menu's frame; past the frame the fovea's blocks were 0.2 - 0.6 of the periphery's
    // brightness, elsewhere 0.9 - 1.1. The blocks around (3 x 3, unblurred) far darker in the fovea = the backdrop.
    // [MENU HOLES 15.11.2026] ... and far darker than the wide view's own picture too (its interface included, at the
    // panels' depth). RE7 dumps 05-09 of 15.11.2026 19:4x (pause menu): the menu's opaque black panel lies over the scene in
    // both views; the periphery is the wide view without its interface, so inside the panel the fovea was far darker than
    // it and the blocks asked for the replacement. Where the wide view's interface mask missed the panel (black over a dark
    // beam: no change between its copies with and without the GUI) the periphery's bright ceiling came through as white
    // lines (holes in the panel), changing from frame to frame. Inside a menu's panel or frame the wide view is as dark as
    // the fovea: nothing is replaced there; past the frame (the inventory) it shows the scene: replaced as before. Emulated
    // on the dumps' images (weight above 0.5): pause menu (dump 05) 3118 of 3780 blocks before (the menu's dimming of the
    // whole view is interface: the periphery has none of it), 109 now; inventory (dump 02) 1270 before (its dark backdrop
    // under the grid, which the wide view draws as well: the bright wall came through in patches), 100 now.
    float rep = 0.0;

    BR if ((g_flags & 4194304) != 0) {
        float3 lum = 0.0; // sums of the part inside x brightness: periphery, fovea, the wide view with its interface

        [unroll] for (int ry = -1; ry <= 1; ++ry) {
            [unroll] for (int rx = -1; rx <= 1; ++rx) {
                const uint at = uint(int(tid.y) + LIGHT_R + ry) * LIGHT_T + uint(int(tid.x) + LIGHT_R + rx);
                const float4 pm = s_bp[at];
                const float lp = dot(pm.rgb, LUMA);
                const float lw = pm.w > 0.0 ? wide_block_luma(id + int2(rx, ry), lp) : lp;
                lum += pm.w * float3(lp, dot(s_bf[at], LUMA), lw);
            }
        }

        // the periphery bright enough to tell (a dark scene behind the backdrop needs no replacement)
        rep = lum.x > 0.004 ? smoothstep(0.7, 0.45, (lum.y + 0.001) / (lum.x + 0.001)) * smoothstep(0.7, 0.45, (lum.y + 0.001) / (lum.z + 0.001))
                            : 0.0;
    }

    g_light[g_slot * LIGHT_SLOT + uint(id.y) * LIGHT_MAX + uint(id.x)] = float4(res, rep);
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// ---- the composite of one eye pixel q (FoveaComposite), from the enlarged periphery -------------------------------------
// [MENU BAND 15.11.2026] the fovea's weight at eye pixel q in the blend (0 outside its rectangle): the band, and the
// periphery's gaps covered by the fovea (see composite); step 5 asks for it again where the interface needs it
float fovea_weight(int2 q) {
    const int2 id = q - g_rect_pos;

    if ((g_flags & 1024) == 0 || any(id < 0) || any(id >= int2(g_rect_size))) {
        return 0.0;
    }

    const float2 d = min(float2(id) + 0.5, float2(g_rect_size) - float2(id) - 0.5);
    const float f = max(g_feather, 1.0);
    const float wx = smoothstep(0.0, 1.0, saturate(d.x / f));
    const float wy = smoothstep(0.0, 1.0, saturate(d.y / f));
    float w = wx * wy;

    // [HOLE BAND 13.11.2026] In the blend band the periphery's gaps (pixels the shared wide view did not see, filled with
    // the background beside them: alpha 0 in the reprojected periphery) are covered by the fovea, which saw them: they
    // made a ring of smeared, doubled background along the inside of the fovea's edge (RE7 dumps of 13.11.2026, "mark
    // reprojection holes": the frame of the fovea in magenta). The gap's share of the periphery texels around this pixel
    // (bilinear) raises the fovea's weight up to 1.
    BR if ((g_flags & 2097152) != 0 && w < 1.0) {
        const float2 sp = float2(src_x(q.x), src_y(q.y)) + 0.5;
        const float seen = g_per.SampleLevel(g_lin, sp / float2(g_per_size), 0).a;
        w = max(w, smoothstep(0.0, 0.75, 1.0 - seen));
    }

    return w;
}

float4 composite(int2 q, float3 per) {
    float4 c = float4(per, 1.0);

    if ((g_flags & 1024) == 0) {
        return c;
    }

    const int2 id = q - g_rect_pos;

    if (any(id < 0) || any(id >= int2(g_rect_size))) {
        return c;
    }

    float4 fcol;

    if (g_fov_size.x == g_rect_size.x && g_fov_size.y == g_rect_size.y) {
        fcol = g_fov.Load(int3(id, 0));
    } else {
        const float2 uv = (float2(id) + 0.5) / float2(g_rect_size);
        fcol = g_fov.SampleLevel(g_lin, uv, 0);
    }

    // [BACKDROP fix83 17.11.2026] a backdrop only the fovea view draws, out of its own pixel
    BR if ((g_flags & 33554432) != 0) {
        fcol.rgb *= fovea_backdrop(float2(q) + 0.5);
    }

    if ((g_flags & 2048) != 0) {
        BR if ((g_flags & 131072) != 0) {
            // [TONE] the factor of the fovea pixel's brightness, between two table entries
            const float t = sqrt(saturate(dot(fcol.rgb, LUMA))) * float(TONE_N - 1);
            const uint i = min(uint(t), TONE_N - 2u);
            fcol.rgb *= lerp(s_lut[i], s_lut[i + 1], t - float(i));
        } else {
            const float g = s_gain;
            fcol.rgb *= (g > 0.0 && g < 10.0) ? g : 1.0;
        }
    }

    // [LIGHT 01.11.2026] the periphery's large-scale light (smooth: the fovea's detail stays; at most down to half)
    BR if ((g_flags & 262144) != 0) {
        fcol.rgb = max(fcol.rgb + light_at(id), fcol.rgb * 0.5);
    }

    const float w = fovea_weight(q);
    c = lerp(c, fcol, w);

    if ((g_flags & 4096) != 0) {
        const float2 d = min(float2(id) + 0.5, float2(g_rect_size) - float2(id) - 0.5);

        if (min(d.x, d.y) < 4.0) {
            c = float4(1.0, 0.0, 0.0, 1.0);
        }
    }

    return (g_flags & 16384) != 0 ? saturate(c) : c;
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [MENU BAND 15.11.2026] the periphery at eye pixel q as step 4 gives it to the composite (encoded and clamped as there):
// from the tile's taps in shared memory (composite row i, column j) or read directly
float3 encode_periphery(float3 per) {
    BR if ((g_flags & 512) != 0) {
        per = to_srgb(per);
    }

#ifdef PREV_2251
    BR if ((g_flags & 16384) != 0) {
        per = saturate(per);
    }
#else
    [loop] for (uint n = 0; n < g_loops.y; ++n) {
        per = saturate(per);
    }
#endif

    return per;
}

#ifndef BEFORE_1031
float3 tile_periphery(uint i, uint j) {
    const float4 wy = s_wy[i];
    const uint r0 = s_oy[i] & (SM - 1u);
    const uint h0 = MASK2(r0 * CSX + j, 1023u);
    const float3 per = s_h[h0] * wy.x + s_h[h0 + CSX] * wy.y + s_h[h0 + 2 * CSX] * wy.z + s_h[h0 + 3 * CSX] * wy.w;
    const uint c1 = (s_ox[j] & (SM - 1u)) + 1;
    const uint q0 = MASK2((r0 + 1) * SM + c1, 255u);
    const float3 a = s_src[q0];
    const float3 b = s_src[q0 + 1];
    const float3 c = s_src[q0 + SM];
    const float3 d = s_src[q0 + SM + 1];
    return clamp(per, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}
#endif

// [MENU BACKDROP 15.11.2026] the menu's replacement weight of the blocks at eye pixel q (0 outside the fovea rectangle)
float menu_rep(int2 q) {
    const int2 id = q - g_rect_pos;

    if ((g_flags & 1024) == 0 || any(id < 0) || any(id >= int2(g_rect_size))) {
        return 0.0;
    }

    return light4_at(id).w;
}

// [MENU BACKDROP 15.11.2026] RE7 dumps of 15.11.2026 (inventory): the fovea view draws the menu's dark backdrop over its
// whole height, the wide view only inside the menu's frame, so the backdrop ended at the fovea's top and bottom edges
// (lighter edges, moving with the gaze). Where the blocks say the fovea is far darker than the periphery, the pixel the
// periphery path gives (its scene with the wide view's interface) replaces the fovea's where the fovea's is far darker
// too: past the frame the scene comes back; inside it both have the backdrop and the fovea (sharp text) stays.
// [MENU HOLES 15.11.2026] the blocks give no weight where the wide view's own picture is as dark as the fovea (a panel
// both views draw: light_blur), so a mask that misses part of a panel no longer opens it here.
// (per: the periphery at the pixel; rep: the blocks' replacement weight there, menu_rep; tr: [PANEL HOLES 15.11.2026] how
// much of the scene the wide view's interface lets through there, centre_ui)
float3 menu_backdrop(float3 per, float3 o3, float mc, float3 cc, float rep, float tr) {
    if (rep <= 0.0) {
        return o3;
    }

    const float3 alt = per * ((1.0 - mc) * tr) + cc * mc;
    const float gate = smoothstep(0.85, 0.6, (dot(o3, LUMA) + 0.004) / (dot(alt, LUMA) + 0.004));
    return lerp(o3, alt, rep * gate);
}

// [MENU BAND 15.11.2026] RE7 dumps of 15.11.2026 17:xx (inventory open): a light strip along the inside of the fovea's
// top and bottom edges over the menu (dump 7, eye column 2506: 11-17 just above the edge, 38-44 in the blend band, 27
// further in; the periphery without the interface 80-109 there). The fovea keeps the darkening of the menu's translucent
// backdrop in its own picture (UiClean), the periphery is the wide view without its interface. The band mixed the two
// pictures with the fovea's weight and put the wide view's interface back with the interface's weight, so the bright
// scene behind the backdrop came through in the band (by w (1 - w) of the difference even with equal weights). Now the
// periphery side of the band is the wide view's picture under its interface (cc without the light the interface added),
// mixed with the scene's weight; the interface's light (text, icons) still changes over with the interface's weight.
// Outside the fovea and fully inside it the result is the same as before.
// per: the periphery at the pixel; w: the fovea's weight there (fovea_weight); eye_rgb: the composite before the post
// sharpening
// [PANEL HOLES 15.11.2026] tr: how much of the scene the wide view's interface lets through (centre_ui); the periphery's
// part that the mask leaves (1 - mc) is weighed by it, outside the fovea and on the periphery's side of the band (a black
// panel the mask missed over a dark scene stays black; a translucent backdrop dims the periphery as the wide view shows
// it). Fully inside the fovea the result is the same as before.
float3 band_ui(float3 scene, float3 eye_rgb, float3 per, float w, float mc, float3 cc, float3 cl, float mf, float3 cf, float wf, float tr) {
    const float3 per_s = per + (scene - eye_rgb); // the periphery with the sharpening the pixel got
    // [MENUS fix82] the fovea image holds its interface (flag 8388608): the wide view's light changes over with the fovea's
    // own weight like the rest of its picture (w of the fovea's picture with its interface + (1 - w) of the periphery's)
    const float wl = (g_flags & 8388608) != 0 ? w : wf;
    return scene + (1.0 - w) * (mc * (cc - cl - per_s) - (1.0 - mc) * (1.0 - tr) * per_s) + (1.0 - wl) * mc * cl + wf * cf * mf;
}

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(// [GPU 30.10.2026] the exposure gain of this frame, once per eye (one thread, after the measure): from the measure's sums
// and the last frame's gain. [3] = this frame's gain for the eye pass, the slot's other ping-pong place = the same value
// for the next frame, [2] = the measurement. (It was made by one thread of every group of the eye pass: the instruction
// trace of 30.10 showed the groups waiting for it at their first barrier.)
float exposure_gain(out float measured) {
    float lw = 0.0;
    float lf = 0.0;

    [unroll] for (uint k = 0; k < 16; ++k) {
        lw += g_part[uint2(g_slot * PART_W + k, 0)];
        lf += g_part[uint2(g_slot * PART_W + 16 + k, 0)];
    }

    lw /= 4096.0;
    lf /= 4096.0;
    measured = lf > 1e-4 ? clamp(lw / lf, 0.33, 3.0) : 1.0;
    const float old = g_gain[uint2(g_slot * 4 + g_parity, 0)];
    return (old > 0.0 && old < 10.0) ? exp(lerp(log(old), log(measured), g_rate)) : measured;
}

[numthreads(1, 1, 1)]
void gain() {
    float measured;
    const float next = exposure_gain(measured);
    g_gain[uint2(g_slot * 4 + (g_parity ^ 1), 0)] = next;
    g_gain[uint2(g_slot * 4 + 2, 0)] = measured;
    g_gain[uint2(g_slot * 4 + 3, 0)] = next;
}
)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(
// ---- [TILE PASS fix105 20.11.2026] the tiles' interface test in a pass of its own, one thread per tile ---------------------
// RE4 test of 20.11.2026 (fix104): the interface costs 0.36 ms of the two eye passes with a small HUD (0.4 % of the map's
// blocks), and 1.00 ms with the tile test off: the test saves 0.64 but most of what is left is the test itself. In the eye
// pass the last 17 threads of every group made it (one per depth, 16 map reads each, and again for the item beams): one wave
// of every group ran those reads with 10 of its 64 lanes while the group waited at its barrier. Here every thread is a
// tile: the same blocks read (each once), the same bits, on full waves; the eye pass reads its tile's two texels.
// g_tiles: per tile (x, y) the texels (2 x, y) and (2 x + 1, y): bit k = s_ui_depth[k], bit k = s_ui_beam[k].
// [fix106] All of this behind macros: the declaration of g_tiles alone, unused, changes what the compiler makes of the eye
// pass with 16 x 16 groups (other registers through 35 000 of its 52 000 words). Without TILE_BITS and TILE_PASS_ENTRY the
// eye pass's bytecode is fix104's, byte for byte (checked for 16 x 16 and 32 x 32).
#if defined(TILE_BITS) || defined(TILE_PASS_ENTRY)
RWTexture2D<uint> g_tiles : register(u7);
// [SMALL GROUPS fix117 21.11.2026] the tile whose bits a group reads: its own, or with TILE_BITS_SHIFT n the tile of
// 2^n x 2^n groups it lies in (the marked tiles' pass in groups of 8 x 8 pixels over the eye pass's tiles of 16 x 16:
// the bits say what is in reach of the larger tile, so nothing the smaller group needs is left out)
#ifdef TILE_BITS_SHIFT
#define TILE_OF(v) ((v) >> TILE_BITS_SHIFT)
#else
#define TILE_OF(v) (v)
#endif
#endif
#ifdef TILE_PASS_ENTRY

// whether a marked GuiMap block lies in the blocks the wide view positions u0 .. u1 reach (tile_marked's blocks: it reads
// min(b0 + (i, j), b1) for i, j < 4, which are these, some of them several times)
uint tile_blocks_marked(float2 u0, float2 u1) {
    const int2 glast = int2(g_gui_size) - 1;
    const int2 t0 = clamp(int2(floor(min(u0, u1) * float2(g_gui_size) - 0.5)) - 1, int2(0, 0), glast);
    const int2 t1 = clamp(int2(floor(max(u0, u1) * float2(g_gui_size) - 0.5)) + 1, int2(0, 0), glast);
    const int2 mlast = int2(g_map_size) - 1;
    const int2 b0 = min(t0 / 8, mlast);
    const int2 b1 = min(t1 / 8, mlast);

    if (any(b1 - b0 >= 4) || any(b1 < b0)) {
        return 1; // not the expected few blocks: no skip
    }

    uint hit = 0;

    [loop] for (int y = b0.y; y <= b1.y; ++y) {
        [loop] for (int x = b0.x; x <= b1.x; ++x) {
            hit |= g_ui_map[uint2(x, y)] == g_map_stamp ? 1u : 0u;
        }
    }

    return hit;
}

// g_loops.zw: the eye pass's tile size (its TSX, TSY)
[numthreads(8, 8, 1)]
void ui_tiles(uint3 id : SV_DispatchThreadID) {
    const int2 last = int2(g_out_size) - 1;
    const int2 ts = int2(g_loops.zw);
    const int2 a = int2(id.xy) * ts;

    if (any(a > last)) {
        return;
    }

    // [fix106] the GPU test's "no tile marked" (measurement only: the image changes)
    if ((g_flags & 67108864) != 0) {
        g_tiles[uint2(id.x * 2, id.y)] = 0;
        g_tiles[uint2(id.x * 2 + 1, id.y)] = 0;
        return;
    }

    const int2 b = min(a + ts - 1, last);
    uint depth = 0;

    // the panels' depth, then each anchor's (as the eye pass's s_ui_depth)
    [loop] for (uint k = 0; k <= g_anchor_count; ++k) {
        const float z = k == 0 ? g_panel_depth : g_anchors[k > 0 ? k - 1 : 0].z;
        depth |= tile_blocks_marked(wide_uv_at(float2(a) + 0.5, z), wide_uv_at(float2(b) + 0.5, z)) << k;
    }

    // an item beam with a fitted place: read there (as the eye pass's s_ui_beam, tile_marked_shift)
    uint beam = depth;

    BR if (depth != 0) {
        [loop] for (uint kb = 1; kb <= g_anchor_count; ++kb) {
            const uint ai = kb - 1;

            BR if (anchor_kind(ai) == 1u) {
                const float2 bf = beam_fit(ai);

                BR if (bf.y > 0.5) {
                    const float s = bf.x - g_anchors[ai].x;
                    const float z = g_anchors[ai].z;
                    const float2 pa = (float2(a) + 0.5) / float2(g_out_size);
                    const float2 pb = (float2(b) + 0.5) / float2(g_out_size);
                    const float2 u0 = wide_uv(lerp(g_eye_tan.x, g_eye_tan.y, pa.x) + s, lerp(g_eye_tan.w, g_eye_tan.z, pa.y), z);
                    const float2 u1 = wide_uv(lerp(g_eye_tan.x, g_eye_tan.y, pb.x) + s, lerp(g_eye_tan.w, g_eye_tan.z, pb.y), z);
                    beam = (beam & ~(1u << kb)) | (tile_blocks_marked(u0, u1) << kb);
                }
            }
        }
    }

    g_tiles[uint2(id.x * 2, id.y)] = depth;
    g_tiles[uint2(id.x * 2 + 1, id.y)] = beam;
}
#endif // TILE_PASS_ENTRY
)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(

#ifdef BEFORE_1031
[numthreads(NTX, NTY, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 last = int2(g_out_size) - 1;
    const int2 plast = int2(g_per_size) - 1;
    const int2 o = int2(gid.xy) * int2(TSX, TSY) - 1; // first composite pixel of this group (one before the group's first eye pixel)

    // ---- 1. per composite column and row: the first tap (base - 1 of WideUpscale) and the fraction ----
    // pixels outside the image stand for the border pixel (the sharpening reads its neighbours clamped to the image)
    if (gi < CSX) {
        const float sx = src_x(clamp(o.x + int(gi), 0, last.x));
        const int bx = int(floor(sx));
        s_bx[gi] = bx;
        s_fx[gi] = sx - float(bx);
    } else if (gi < CSX + CSY) {
        const float sy = src_y(clamp(o.y + int(gi - CSX), 0, last.y));
        const int by = int(floor(sy));
        s_by[gi - CSX] = by;
        s_fy[gi - CSX] = sy - float(by);
    }
#ifdef GAIN_PER_GROUP
    // (before 30.10.2026: one thread of every group made the gain; the first group kept it for the next frame)
    else if (gi == CSX + CSY && (g_flags & 2048) != 0) {
        float measured;
        const float next = exposure_gain(measured);
        s_gain = next;

        if (gid.x == 0 && gid.y == 0) {
            g_gain[uint2(g_slot * 4 + (g_parity ^ 1), 0)] = next;
            g_gain[uint2(g_slot * 4 + 2, 0)] = measured;
        }
    }
#else
    if (gi == 0 && (g_flags & 2048) != 0) {
        s_gain = g_gain[uint2(g_slot * 4 + ((g_flags & 1048576) != 0 ? g_parity : 3u), 0)];
    }
#endif

    // [TONE] this frame's tone table; [BLOCKS 02.11.2026] the last frame's from its ping-pong place (nothing usable = 1)
    if (gi < TONE_N && (g_flags & (2048 | 131072)) == (2048 | 131072)) {
        BR if ((g_flags & 1048576) != 0) {
            const float v = g_gain[uint2(g_slot * TONE_SLOT + g_parity * TONE_N + gi, 1)];
            s_lut[gi] = (v > 0.0 && v < 10.0) ? v : 1.0;
        } else {
            s_lut[gi] = g_gain[uint2(g_slot * TONE_SLOT + 2 * TONE_N + gi, 1)];
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // the taps of the tile: from tap 0 of the first column / row to tap 3 of the last (the positions grow with x and y)
    const int2 t0 = int2(s_bx[0], s_by[0]) - 1;
    const int2 tn = int2(s_bx[CSX - 1], s_by[CSY - 1]) + 3 - t0;
    const bool tiled = (g_flags & 8192) != 0 && all(tn <= SM);

    // ---- 2. the taps into shared memory (clamped to the periphery like WideUpscale's taps) ----
    if (tiled) {
        for (uint i = gi; i < uint(tn.x * tn.y); i += NT) {
            const int2 s = int2(int(i % uint(tn.x)), int(i / uint(tn.x)));
            s_src[s.y * SM + s.x] = g_per.Load(int3(clamp(t0 + s, int2(0, 0), plast), 0)).rgb;
        }
    }

    // [GPU 29.10.2026] meanwhile the last threads: the interface depths this tile's pixels can read the wide view's
    // interface at (the panels' and each anchor's), whether a marked GuiMap block is in reach at each
    const bool tile_test = (g_flags & (1 | 128 | 32768)) == (1 | 128 | 32768);

    if (tile_test && gi >= NT - 17 && gi - (NT - 17) <= g_anchor_count) {
        const uint k = gi - (NT - 17);
        s_ui_depth[k] = tile_marked(int2(gid.xy) * int2(TSX, TSY), min(int2(gid.xy) * int2(TSX, TSY) + int2(TSX, TSY) - 1, last),
            k == 0 ? g_panel_depth : g_anchors[k > 0 ? k - 1 : 0].z);
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0) {
        uint any_ui = 1;

        if (tile_test) {
            any_ui = 0;

            for (uint k = 0; k <= g_anchor_count; ++k) {
                any_ui |= s_ui_depth[k];
            }
        }

        s_ui_any = any_ui;
    }

    // ---- 3. every tap row enlarged along x for each composite column ----
    if (tiled) {
        for (uint e = gi; e < uint(tn.y) * CSX; e += NT) {
            const uint r = e / CSX;
            const uint j = e % CSX;
            const float4 wx = cr_weights(s_fx[j]);
            const uint c0 = r * SM + uint(s_bx[j] - 1 - t0.x);
            s_h[r * CSX + j] = s_src[c0] * wx.x + s_src[c0 + 1] * wx.y + s_src[c0 + 2] * wx.z + s_src[c0 + 3] * wx.w;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // ---- 4. along y, clamped to the 2x2 around the position, encoded; then the fovea blended in ----
    for (uint e = gi; e < CSX * CSY; e += NT) {
        const uint i = e / CSX;
        const uint j = e % CSX;
        const int2 q = clamp(o + int2(int(j), int(i)), int2(0, 0), last);
        float3 per;

        if (tiled) {
            const float4 wy = cr_weights(s_fy[i]);
            const uint r0 = uint(s_by[i] - 1 - t0.y);
            per = s_h[r0 * CSX + j] * wy.x + s_h[(r0 + 1) * CSX + j] * wy.y + s_h[(r0 + 2) * CSX + j] * wy.z + s_h[(r0 + 3) * CSX + j] * wy.w;
            const uint c1 = uint(s_bx[j] - t0.x);
            const float3 a = s_src[(r0 + 1) * SM + c1];
            const float3 b = s_src[(r0 + 1) * SM + c1 + 1];
            const float3 c = s_src[(r0 + 2) * SM + c1];
            const float3 d = s_src[(r0 + 2) * SM + c1 + 1];
            per = clamp(per, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
        } else {
            per = periphery_direct(q);
        }

        if ((g_flags & 512) != 0) {
            per = to_srgb(per);
        }

        if ((g_flags & 16384) != 0) {
            per = saturate(per);
        }

        s_c[e] = composite(q, per);
    }

    GroupMemoryBarrierWithGroupSync();

    // ---- 5. the eye pixels: post sharpening, interface, rest of the post pass (MonoUi); one per thread and round ----
    for (uint k = gi; k < TSX * TSY; k += NT) {
        const int2 lp = int2(int(k % TSX), int(k / TSX));
        const int2 pix = int2(gid.xy) * int2(TSX, TSY) + lp;

        if (any(pix > last)) {
            continue;
        }

        const uint ci = uint(lp.y + 1) * CSX + uint(lp.x + 1);
        const float4 eye = s_c[ci];
        const float2 p = float2(pix) + 0.5;

        // [GPU 29.10.2026] no GuiMap block marked in reach of the tile: the wide view's interface is 0 on all its pixels
        float3 cc = 0.0;
        float3 cl = 0.0;
        float mc = 0.0;
        float tr = 1.0; // [PANEL HOLES 15.11.2026]

        if (s_ui_any != 0) {
            mc = centre_ui(p, cc, cl, tr, 0u);
        }

        float mf = 0.0;
        float3 cf = 0.0;
        float wf = 0.0;

        if ((g_flags & 2) != 0 && g_rect.z > 0.0 && g_rect.w > 0.0) {
            const float2 d = min(p - g_rect.xy, g_rect.xy + g_rect.zw - p);

            if (d.x > 0.0 && d.y > 0.0) {
                const float f = clamp(g_ui_feather, 1.0, 0.5 * min(g_rect.z, g_rect.w));
                wf = smoothstep(0.0, 1.0, saturate(d.x / f)) * smoothstep(0.0, 1.0, saturate(d.y / f));

                // [LIGHTS 11.11.2026] the fovea view draws the same interface as the wide view: none in reach of the
                // tile there (s_ui_any, at the panel's and every anchor's depth), none here either. Coloured lights
                // (a red lamp) come out of the engine's output pass other than the per-channel curve predicts and
                // counted as interface: the fovea view's own jittered pixels over the upscaled fovea (dots).
                if (s_ui_any != 0) {
                    mf = fovea_ui(p, cf);
                }
            }
        }

        if ((g_flags & 4) != 0) {
            cc = lerp(cc, float3(0.2, 0.4, 1.0), 0.5);
            cf = lerp(cf, float3(0.2, 1.0, 0.3), 0.5);
        }

        // [TRANSLUCENT 11.11.2026] the fovea view's interface adds its light (cf = ui_light) over the upscaled fovea
        const float m = mc * (1.0 - wf);
        const float3 premul = lerp(cc * mc, cf * mf, wf);
        float3 scene = eye.rgb;

        if ((g_flags & 256) != 0 && (g_post.x > 0.0 || g_post2.x > 0.0)) {
            const float3 n0 = s_c[ci - CSX].rgb;
            const float3 n1 = s_c[ci + CSX].rgb;
            const float3 n2 = s_c[ci - 1].rgb;
            const float3 n3 = s_c[ci + 1].rgb;
            const float3 blur = (n0 + n1 + n2 + n3) * 0.25;
            // [EDGE SOFTEN 11.11.2026] on edges the neighbours' mean is blended in instead of the sharpening
            const float e = g_post2.x > 0.0 ? edge_weight(scene, n0, n1, n2, n3, g_post2.y) : 0.0;
            scene += clamp(dot(scene - blur, LUMA) * g_post.x * (1.0 - e), -0.08, 0.08);
            scene = lerp(scene, blur, g_post2.x * e);
        }

        float3 o3 = scene * (1.0 - m) + premul;

        // [MENUS fix83] the debug tint's green: the interface the fovea image holds itself (told by its view's copies)
        BR if ((g_flags & (4 | 16777216)) == (4 | 16777216) && wf > 0.0) {
            BR if (fovea_drawn((p - g_rect.xy) / g_rect.zw + g_ui_jit.xy)) {
                o3 = lerp(o3, float3(0.2, 1.0, 0.3), 0.5 * wf);
            }
        }

        // [MENU BACKDROP, MENU BAND 15.11.2026] the periphery at the pixel where the interface or the menu needs it (this
        // test variant reads it directly)
        const float rep = (g_flags & 4194304) != 0 ? menu_rep(pix) : 0.0;
        const bool band = (g_flags & 4) == 0 && (mc > 0.0 || tr < 1.0); // [PANEL HOLES 15.11.2026] tr also without the mask
        float3 per_px = 0.0;

        BR if (band || rep > 0.0) {
            per_px = encode_periphery(periphery_direct(pix));
        }

        // [MENU BAND 15.11.2026] the interface's darkening with the scene's weight (not with the tinted debug colours)
        BR if (band) {
            o3 = band_ui(scene, eye.rgb, per_px, fovea_weight(pix), mc, cc, cl, mf, cf, wf, tr);
        }

        BR if (rep > 0.0) {
            o3 = menu_backdrop(per_px, o3, mc, cc, rep, tr); // [MENU BACKDROP 15.11.2026]
        }

        if ((g_flags & 256) != 0) {
            if (g_post.y != 1.0) {
                const float l = dot(o3, LUMA);
                o3 = max(lerp(float3(l, l, l), o3, g_post.y), 0.0);
            }

            if (g_post.z != 1.0) {
                o3 = pow(max(o3, 0.0), g_post.z);
            }

            if (g_post.w != 1.0) {
                o3 = max((o3 - 0.5) * g_post.w + 0.5, 0.0);
            }
        }

        // [DIRECT 8 BIT 30.10.2026] the 10-bit eye image's rounding (R10G10B10A2), which the conversion to 8 bits then read
        if ((g_flags & 65536) != 0) {
            g_out[uint2(pix)] = float4(round(saturate(o3) * 1023.0) / 1023.0, round(saturate(eye.a) * 3.0) / 3.0);
        } else {
            g_out[uint2(pix)] = float4(o3, eye.a);
        }
    }
}
#else
)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"([numthreads(NTX, NTY, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
#ifdef UI_MARKED
    // [TWO PASSES fix111 20.11.2026] RE4 test of fix110 (weapon HUD and 5 item beams in the picture): this pass's text
    // without the interface's two calls (UI_NEVER) costs 0.899 ms for the two eyes, with them 1.19 - 1.20 even when no tile
    // has interface and the calls never run (and 1.77 with the calls behind a loop): the calls cost by being in the text.
    // So the eye image is made by the text without them, and this text (UI_MARKED) then makes again only the tiles the
    // tile pass marked: all the others leave here. (The bits are read by one thread and the leaving comes after a sync:
    // the compiler refuses a sync after a branch on something read from an image, X4026.) A tile without interface gets
    // the same pixels from either text (m = 0, premul = 0), so the image is the one of the single pass.
    if (gi == 0) {
        s_ui_any = g_tiles[uint2(TILE_OF(gid.x) * 2, TILE_OF(gid.y))] != 0 ? 1u : 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    if (s_ui_any == 0) {
        return;
    }
#endif
#ifdef UI_UNMARKED
    // [TWO PASSES fix112 20.11.2026] ... and the first pass's text leaves the marked tiles here, in the same way: they were
    // made twice (RE4 test of fix111: the second pass costs 0.10 - 0.13 ms with the weapon HUD and item beams in the
    // picture, nothing without interface: about 8 % of the tiles). The two passes then write different tiles.
    if (gi == 0) {
        s_ui_any = g_tiles[uint2(TILE_OF(gid.x) * 2, TILE_OF(gid.y))] != 0 ? 1u : 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    if (s_ui_any != 0) {
        return;
    }
#endif
    const int2 last = int2(g_out_size) - 1;
    const int2 plast = int2(g_per_size) - 1;
    const int2 o = int2(gid.xy) * int2(TSX, TSY) - 1; // first composite pixel of this group (one before the group's first eye pixel)

    // ([GPU 01.11.2026] tested: the taps asked for first with steps 1 and 2 sharing one barrier, every thread computing the
    // tile's first tap and tap count: +0.005 ms (the trace: 53 instead of 15 VALU per wave before the barrier).)
    // ---- 1. per composite column and row: the first tap (base - 1 of WideUpscale) and the Catmull-Rom weights ----
    // pixels outside the image stand for the border pixel (the sharpening reads its neighbours clamped to the image)
    if (gi < CSX) {
        const float sx = src_x(clamp(o.x + int(gi), 0, last.x));
        const int bx = int(floor(sx));
        s_ox[gi] = uint(bx - int(floor(src_x(clamp(o.x, 0, last.x)))));
        s_wx[gi] = cr_weights(sx - float(bx));

        if (gi == 0) {
            s_t0.x = bx - 1;
        }
    } else if (gi < CSX + CSY) {
        const uint k = gi - CSX;
        const float sy = src_y(clamp(o.y + int(k), 0, last.y));
        const int by = int(floor(sy));
        s_oy[k] = uint(by - int(floor(src_y(clamp(o.y, 0, last.y)))));
        s_wy[k] = cr_weights(sy - float(by));

        if (k == 0) {
            s_t0.y = by - 1;
        }
    }
#ifdef GAIN_PER_GROUP
    // (before 30.10.2026: one thread of every group made the gain; the first group kept it for the next frame)
    else if (gi == CSX + CSY && (g_flags & 2048) != 0) {
        float measured;
        const float next = exposure_gain(measured);
        s_gain = next;

        if (gid.x == 0 && gid.y == 0) {
            g_gain[uint2(g_slot * 4 + (g_parity ^ 1), 0)] = next;
            g_gain[uint2(g_slot * 4 + 2, 0)] = measured;
        }
    }
#else
    if (gi == 0 && (g_flags & 2048) != 0) {
        s_gain = g_gain[uint2(g_slot * 4 + ((g_flags & 1048576) != 0 ? g_parity : 3u), 0)];
    }
#endif

    // [TONE] this frame's tone table; [BLOCKS 02.11.2026] the last frame's from its ping-pong place (nothing usable = 1)
    if (gi < TONE_N && (g_flags & (2048 | 131072)) == (2048 | 131072)) {
        BR if ((g_flags & 1048576) != 0) {
            const float v = g_gain[uint2(g_slot * TONE_SLOT + g_parity * TONE_N + gi, 1)];
            s_lut[gi] = (v > 0.0 && v < 10.0) ? v : 1.0;
        } else {
            s_lut[gi] = g_gain[uint2(g_slot * TONE_SLOT + 2 * TONE_N + gi, 1)];
        }
    }

    GroupMemoryBarrierWithGroupSync();

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(#ifdef MARKED_DIRECT
    // [DIRECT fix120 22.11.2026] GPU test, measurement only. RE4 tests of fix118 and fix119: making the marked tiles (0.35 % of
    // the eye image) again costs 0.064 ms for the two eyes without the interface's calls and 0.094 with them, the same
    // with and without the spans' timestamps between the passes: the pass's own run from first group to last, not its
    // work. A group of this text goes through four stages that each end at a barrier of the whole group (taps, the
    // horizontal filter, the composite, the pixel). Here every thread makes its pixel by itself after the values of the
    // group (gain, tone table): the periphery by periphery_direct, no stage in between. Without the interface, the
    // sharpening after the composite and the menu's part, so the image is the first pass's where those are not in use.
    for (uint dk = gi; dk < TSX * TSY; dk += NT) {
        const int2 dpix = int2(gid.xy) * int2(TSX, TSY) + int2(int(dk % TSX), int(dk / TSX));

        if (all(dpix <= last)) {
            float3 dper = periphery_direct(dpix);

            BR if ((g_flags & 512) != 0) {
                dper = to_srgb(dper);
            }

            [loop] for (uint dn = 0; dn < g_loops.y; ++dn) {
                dper = saturate(dper);
            }

            const float4 deye = composite(dpix, dper);
            const float3 d3 = deye.rgb + 0.0;

            if ((g_flags & 65536) != 0) {
                g_out[uint2(dpix)] = float4(round(saturate(d3) * 1023.0) / 1023.0, round(saturate(deye.a) * 3.0) / 3.0);
            } else {
                g_out[uint2(dpix)] = float4(d3, deye.a);
            }
        }
    }

    return;
#endif
    // the taps of the tile: from tap 0 of the first column / row to tap 3 of the last (the positions grow with x and y)
    const int2 t0 = s_t0;
    const int2 tn = int2(int(s_ox[CSX - 1]), int(s_oy[CSY - 1])) + 4;
#ifdef MARKED_OVER
    const bool tiled = false; // [OVER fix121] no taps and no composite: only the interface, over the first pass's pixel
#else
    const bool tiled = (g_flags & 8192) != 0 && all(tn <= SM);
#endif

    // ---- 2. the taps into shared memory (clamped to the periphery like WideUpscale's taps), by rows of SM ----
    if (tiled) {
        for (uint i = gi; i < SM * SM; i += NT) {
            const int2 s = int2(int(i % SM), int(i / SM));

            if (s.x < tn.x && s.y < tn.y) {
                s_src[MASK2(i, 255u)] = g_per.Load(int3(clamp(t0 + s, int2(0, 0), plast), 0)).rgb;
            }
        }
    }

    // [GPU 29.10.2026] meanwhile the last threads: the interface depths this tile's pixels can read the wide view's
    // interface at (the panels' and each anchor's), whether a marked GuiMap block is in reach at each
    const bool tile_test = (g_flags & (1 | 128 | 32768)) == (1 | 128 | 32768);

    if (tile_test && gi >= NT - 17 && gi - (NT - 17) <= g_anchor_count) {
        const uint k = gi - (NT - 17);
#ifdef TILE_BITS
        // [TILE PASS fix106 20.11.2026] both made by ui_tiles for this tile (the same values): two reads instead of the test.
        // A pass of its own text (this macro), not a flag: without the macro the text is fix104's, token for token (RE4 test of
        // fix105: with a flag the pass with its own test cost 0.28 ms more than fix104's, and the tile pass gained nothing)
        s_ui_depth[k] = (g_tiles[uint2(TILE_OF(gid.x) * 2, TILE_OF(gid.y))] >> k) & 1u;
        s_ui_beam[k] = (g_tiles[uint2(TILE_OF(gid.x) * 2 + 1, TILE_OF(gid.y))] >> k) & 1u;
#else
        s_ui_depth[k] = tile_marked(int2(gid.xy) * int2(TSX, TSY), min(int2(gid.xy) * int2(TSX, TSY) + int2(TSX, TSY) - 1, last),
            k == 0 ? g_panel_depth : g_anchors[k > 0 ? k - 1 : 0].z);
#endif
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi == 0) {
        uint any_ui = 1;

        if (tile_test) {
            any_ui = 0;

            for (uint k = 0; k <= g_anchor_count; ++k) {
                any_ui |= s_ui_depth[k];
            }
        }

        s_ui_any = any_ui;
    }

    // [GPU fix99 19.11.2026] meanwhile the last threads, in a tile with interface in reach: an item beam's depth again,
    // read at the beam's fitted place (where centre_ui reads it) ([TILE PASS fix106] not with TILE_BITS: made by ui_tiles)
#ifndef TILE_BITS
    if (tile_test && gi >= NT - 17 && gi - (NT - 17) <= g_anchor_count) {
        const uint kb = gi - (NT - 17);
        const uint ai = kb > 0 ? kb - 1 : 0;
        uint beam = s_ui_depth[kb];

        BR if (kb > 0 && anchor_kind(ai) == 1u) {
            uint any_tile = 0;

            for (uint n = 0; n <= g_anchor_count; ++n) {
                any_tile |= s_ui_depth[n];
            }

            BR if (any_tile != 0) {
                const float2 bf = beam_fit(ai);

                BR if (bf.y > 0.5) {
                    beam = tile_marked_shift(int2(gid.xy) * int2(TSX, TSY), min(int2(gid.xy) * int2(TSX, TSY) + int2(TSX, TSY) - 1, last),
                        g_anchors[ai].z, bf.x - g_anchors[ai].x);
                }
            }
        }

        s_ui_beam[kb] = beam;
    }
#endif


    // ---- 3. every tap row enlarged along x for each composite column ----
    if (tiled) {
        for (uint e = gi; e < uint(tn.y) * CSX; e += NT) {
            const uint ec = HINT(e, SM * CSX - 1u, 4095u); // (= e: the bound gives the compiler the range)
            const uint r = MASK(DIV_CSX(ec), SM - 1u);
            const uint j = MASK(ec - r * CSX, 63u);
            const float4 wx = s_wx[j];
            const uint c0 = MASK2(r * SM + (s_ox[j] & (SM - 1u)), 255u);
            s_h[ec] = s_src[c0] * wx.x + s_src[c0 + 1] * wx.y + s_src[c0 + 2] * wx.z + s_src[c0 + 3] * wx.w;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // ---- 4. along y, clamped to the 2x2 around the position, encoded; then the fovea blended in ----
#ifndef MARKED_OVER
    for (uint e = gi; e < CSX * CSY; e += NT) {
        const uint ec = HINT(e, CSX * CSY - 1u, 4095u);
        const uint i = MASK(DIV_CSX(ec), 63u);
        const uint j = MASK(ec - i * CSX, 63u);
        const int2 q = clamp(o + int2(int(j), int(i)), int2(0, 0), last);
        float3 per;

        BR if (tiled) {
            const float4 wy = s_wy[i];
            const uint r0 = s_oy[i] & (SM - 1u);
            const uint h0 = MASK2(r0 * CSX + j, 1023u);
            per = s_h[h0] * wy.x + s_h[h0 + CSX] * wy.y + s_h[h0 + 2 * CSX] * wy.z + s_h[h0 + 3 * CSX] * wy.w;
            const uint c1 = (s_ox[j] & (SM - 1u)) + 1;
            const uint q0 = MASK2((r0 + 1) * SM + c1, 255u);
            const float3 a = s_src[q0];
            const float3 b = s_src[q0 + 1];
            const float3 c = s_src[q0 + SM];
            const float3 d = s_src[q0 + SM + 1];
            per = clamp(per, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
        } else {
            per = periphery_direct(q);
        }

        BR if ((g_flags & 512) != 0) {
            per = to_srgb(per);
        }

#ifdef PREV_2251
        BR if ((g_flags & 16384) != 0) {
            per = saturate(per);
        }
#else
        [loop] for (uint n = 0; n < g_loops.y; ++n) {
            per = saturate(per);
        }
#endif

        s_c[ec] = composite(q, per);
    }
#endif // MARKED_OVER (no composite: the first pass made it)

    GroupMemoryBarrierWithGroupSync();

    // ---- 5. the eye pixels: post sharpening, interface, rest of the post pass (MonoUi); one per thread and round ----
    for (uint k = gi; k < TSX * TSY; k += NT) {
        const uint kc = HINT(k, TSX * TSY - 1u, 4095u);
        const int2 lp = int2(int(MASK(kc % TSX, 63u)), int(MASK(kc / TSX, 63u)));
        const int2 pix = int2(gid.xy) * int2(TSX, TSY) + lp;

        if (any(pix > last)) {
            continue;
        }

        const uint ci = MASK2(uint(lp.y + 1), 63u) * CSX + uint(lp.x + 1);
#ifndef MARKED_OVER
        const float4 eye = s_c[ci];
#endif
        const float2 p = float2(pix) + 0.5;

        // [GPU 29.10.2026] no GuiMap block marked in reach of the tile: the wide view's interface is 0 on all its pixels
        float3 cc = 0.0;
        float3 cl = 0.0; // [MENU BAND 15.11.2026]
        float mc = 0.0;
        float tr = 1.0; // [PANEL HOLES 15.11.2026]
        bool ui = (g_flags & 4) != 0; // an interface term can be other than 0 at this pixel

        // [fix108 20.11.2026] RE4 test of fix107 (weapon HUD and 3 item beams in the picture): the tiles with interface cost
        // 0.02 ms of the two eye passes, the interface in all the others 0.25 (1.154 with no tile marked against 0.905
        // without interface). Two texts of this call for the GPU test: UI_NEVER without it (measurement only), UI_LOOP
        // behind a loop of 0 or 1 rounds, which a driver cannot turn into both sides and a select (same image).
#if defined(UI_NEVER)
#elif defined(UI_LOOP)
        [loop] for (uint ur = 0; ur < s_ui_any; ++ur) {
            mc = centre_ui(p, cc, cl, tr, 1u);
            ui = true;
        }
#else
        if (s_ui_any != 0) {
            mc = centre_ui(p, cc, cl, tr, 1u);
            ui = true;
        }
#endif

        float mf = 0.0;
        float3 cf = 0.0;
        float wf = 0.0;

        if ((g_flags & 2) != 0 && g_rect.z > 0.0 && g_rect.w > 0.0) {
            const float2 d = min(p - g_rect.xy, g_rect.xy + g_rect.zw - p);

            if (d.x > 0.0 && d.y > 0.0) {
                const float f = clamp(g_ui_feather, 1.0, 0.5 * min(g_rect.z, g_rect.w));
                wf = smoothstep(0.0, 1.0, saturate(d.x / f)) * smoothstep(0.0, 1.0, saturate(d.y / f));

                // [LIGHTS 11.11.2026] the fovea view draws the same interface as the wide view: none in reach of the
                // tile there (s_ui_any, at the panel's and every anchor's depth), none here either. Coloured lights
                // (a red lamp) come out of the engine's output pass other than the per-channel curve predicts and
                // counted as interface: the fovea view's own jittered pixels over the upscaled fovea (dots).
#if defined(UI_NEVER)
#elif defined(UI_LOOP)
                [loop] for (uint uf = 0; uf < s_ui_any; ++uf) {
                    mf = fovea_ui(p, cf);
                }
#else
                if (s_ui_any != 0) {
                    mf = fovea_ui(p, cf);
                }
#endif
                ui = true;
            }
        }

#ifdef MARKED_OVER
        // [OVER fix121 22.11.2026] test: the interface over the pixel the first pass wrote. A pixel without an interface
        // term keeps the first pass's (the same bytes); one with interface takes the first pass's pixel as its scene (as
        // stored: 10 bits) in place of the composite made again, so only the pixels of the interface can differ, by the
        // rounding of what lies under a translucent part. The periphery for the bands comes from periphery_direct. Not
        // with the sharpening and the colours after the composite (the stored pixel has them, the interface would not),
        // nor with the menus' backdrop or the debug tint: the mod then takes the usual text.
        if (!(mc > 0.0 || tr < 1.0 || mf > 0.0)) {
            continue;
        }

        const float4 eye = g_out[uint2(pix)];
#endif
        float3 scene = eye.rgb;

#ifndef MARKED_OVER
        BR if ((g_flags & 256) != 0 && (g_post.x > 0.0 || g_post2.x > 0.0)) {
            const float3 n0 = s_c[ci - CSX].rgb;
            const float3 n1 = s_c[ci + CSX].rgb;
            const float3 n2 = s_c[ci - 1].rgb;
            const float3 n3 = s_c[ci + 1].rgb;
            const float3 blur = (n0 + n1 + n2 + n3) * 0.25;
            // [EDGE SOFTEN 11.11.2026] on edges the neighbours' mean is blended in instead of the sharpening
            const float e = g_post2.x > 0.0 ? edge_weight(scene, n0, n1, n2, n3, g_post2.y) : 0.0;
            scene += clamp(dot(scene - blur, LUMA) * g_post.x * (1.0 - e), -0.08, 0.08);
            scene = lerp(scene, blur, g_post2.x * e);
        }
#endif // MARKED_OVER (the sharpening)

        // [MENU BACKDROP, MENU BAND 15.11.2026] the periphery at the pixel where the interface or the menu needs it (from
        // the tile's taps, as step 4 had it)
        const float rep = (g_flags & 4194304) != 0 ? menu_rep(pix) : 0.0;
        const bool band = ui && (g_flags & 4) == 0 && (mc > 0.0 || tr < 1.0); // [PANEL HOLES 15.11.2026] tr also without the mask
        float3 per_px = 0.0;

        BR if (band || rep > 0.0) {
            per_px = encode_periphery(tiled ? tile_periphery(uint(lp.y + 1), uint(lp.x + 1)) : periphery_direct(pix));
        }

        // without an interface term m = 0 and premul = 0 (cc, mc, cf, mf and wf are all 0): the blend is scene + 0
        float3 o3;

        BR if (ui) {
            BR if ((g_flags & 4) != 0) {
                cc = lerp(cc, float3(0.2, 0.4, 1.0), 0.5);
                cf = lerp(cf, float3(0.2, 1.0, 0.3), 0.5);
            }

            // [TRANSLUCENT 11.11.2026] the fovea view's interface adds its light (cf = ui_light) over the upscaled fovea
            BR if (band) {
                o3 = band_ui(scene, eye.rgb, per_px, fovea_weight(pix), mc, cc, cl, mf, cf, wf, tr); // [MENU BAND, PANEL HOLES 15.11.2026]
            } else {
                const float m = mc * (1.0 - wf);
                const float3 premul = lerp(cc * mc, cf * mf, wf);
                o3 = scene * (1.0 - m) + premul;
            }
        } else {
            o3 = scene + 0.0;
        }

        // [MENUS fix83] the debug tint's green: the interface the fovea image holds itself (told by its view's copies)
        BR if ((g_flags & (4 | 16777216)) == (4 | 16777216) && wf > 0.0) {
            BR if (fovea_drawn((p - g_rect.xy) / g_rect.zw + g_ui_jit.xy)) {
                o3 = lerp(o3, float3(0.2, 1.0, 0.3), 0.5 * wf);
            }
        }

        BR if (rep > 0.0) {
            o3 = menu_backdrop(per_px, o3, mc, cc, rep, tr); // [MENU BACKDROP 15.11.2026]
        }

#ifndef MARKED_OVER
        BR if ((g_flags & 256) != 0) {
            BR if (g_post.y != 1.0) {
                const float l = dot(o3, LUMA);
                o3 = max(lerp(float3(l, l, l), o3, g_post.y), 0.0);
            }

            BR if (g_post.z != 1.0) {
                o3 = pow(max(o3, 0.0), g_post.z);
            }

#ifdef PREV_2251
            BR if (g_post.w != 1.0) {
                o3 = max((o3 - 0.5) * g_post.w + 0.5, 0.0);
            }
#else
            [loop] for (uint n = 0; n < g_loops.x; ++n) {
                o3 = max((o3 - 0.5) * g_post.w + 0.5, 0.0);
            }
#endif
        }

        // [DIRECT 8 BIT 30.10.2026] the 10-bit eye image's rounding (R10G10B10A2), which the conversion to 8 bits then read
#endif // MARKED_OVER (the colours after the interface)
        if ((g_flags & 65536) != 0) {
            g_out[uint2(pix)] = float4(round(saturate(o3) * 1023.0) / 1023.0, round(saturate(eye.a) * 3.0) / 3.0);
        } else {
            g_out[uint2(pix)] = float4(o3, eye.a);
        }
    }
}
#endif

)";

struct EyeComposeConstants {
    uint32_t out_w;
    uint32_t out_h;
    uint32_t per_w;
    uint32_t per_h;
    uint32_t flags;
    uint32_t anchor_count;
    uint32_t slot;
    float feather;
    float eye_tan[4];
    float wide_tan[4];
    float rect[4];
    int32_t rect_x;
    int32_t rect_y;
    uint32_t rect_w;
    uint32_t rect_h;
    uint32_t fov_w;
    uint32_t fov_h;
    uint32_t gui_w;
    uint32_t gui_h;
    uint32_t map_w;
    uint32_t map_h;
    float eye_x;
    float panel_depth;
    float ui_feather;
    float rate;
    uint32_t parity;
    uint32_t map_stamp;
    float post[4];
    float anchors[MonoUi::MAX_ANCHORS][4];
    uint32_t loops[4]; // [GPU 31.10.2026 night] contrast rounds, composite clamp rounds (0 or 1)
    float light[4];    // [LIGHT 02.11.2026] x = the large-scale light's blur (sigma in blocks)
    float anchor_ext[MonoUi::MAX_ANCHORS][4]; // [ITEM BEAMS 03.11.2026] reach across, above, below, 1 = item beam
    float post2[4]; // [EDGE SOFTEN 11.11.2026] edge softening weight, edge threshold, -, -
    float ui_jit[4]; // [MENUS fix81] the frame's jitter as a UV offset: the fovea view's (x, y), the wide view's (z, w)
};
static_assert(sizeof(EyeComposeConstants) == 736, "matches the HLSL cbuffer");
static_assert(sizeof(EyeComposeConstants) <= 768, "fits a constant buffer slot (EyeCompose::CB_SLOT_SIZE)");
} // namespace

bool GuiMap::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(GUI_MAP_SHADER, sizeof(GUI_MAP_SHADER) - 1, "GuiMap", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[GuiMap] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; // hud, fin
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2; // map, count
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // [BEAM FIT 03.11.2026] beam_fit's boxes (b1)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[GuiMap] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[GuiMap] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    // [BEAM FIT 03.11.2026] optional: without it the beams stay where the wide view drew them
    {
        ComPtr<ID3DBlob> fit_shader{};
        ComPtr<ID3DBlob> fit_errors{};
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cb{};
        cb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cb.Width = (UINT64)RING_COUNT * FIT_CB_SIZE;
        cb.Height = 1;
        cb.DepthOrArraySize = 1;
        cb.MipLevels = 1;
        cb.SampleDesc.Count = 1;
        cb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* cb_mapped = nullptr;
        const D3D12_RANGE none{0, 0};

        if (FAILED(compile_cached(GUI_MAP_SHADER, sizeof(GUI_MAP_SHADER) - 1, "GuiMapFit", nullptr, nullptr, "beam_fit", "cs_5_0", 0, 0, &fit_shader, &fit_errors))) {
            REVR_LOG(spdlog::error("[GuiMap] beam fit compile failed: {}", fit_errors != nullptr ? (const char*)fit_errors->GetBufferPointer() : "?"));
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC fit_desc{};
            fit_desc.pRootSignature = m_root_sig.Get();
            fit_desc.CS.pShaderBytecode = fit_shader->GetBufferPointer();
            fit_desc.CS.BytecodeLength = fit_shader->GetBufferSize();

            if (FAILED(device->CreateComputePipelineState(&fit_desc, IID_PPV_ARGS(&m_fit_pso))) ||
                FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cb, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_fit_cb))) ||
                FAILED(m_fit_cb->Map(0, &none, &cb_mapped)) || cb_mapped == nullptr) {
                REVR_LOG(spdlog::error("[GuiMap] beam fit pipeline / constants failed"));
                m_fit_pso.Reset();
                m_fit_cb.Reset();
            } else {
                m_fit_mapped = (uint8_t*)cb_mapped;
            }
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = 256;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC rb = buf;
    rb.Width = FRAMES * 4;
    rb.Flags = D3D12_RESOURCE_FLAG_NONE;

    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_count))) ||
        FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback))) ||
        FAILED(m_readback->Map(0, &no_read, &mapped)) || mapped == nullptr) {
        REVR_LOG(spdlog::error("[GuiMap] descriptor heap / buffers failed"));
        m_last_error = "descriptor heap / buffers failed";
        m_pso.Reset();
        m_root_sig.Reset();
        m_heap.Reset();
        m_count.Reset();
        m_readback.Reset();
        return false;
    }

    m_count->SetName(L"RE4VR GUI map count");
    m_mapped = (const uint32_t*)mapped;
    m_slot_frame.fill(~0ull);
    m_slot_ready.fill(false);
    m_last_frame = ~0ull;
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[GuiMap] ready"));
    return true;
}

bool GuiMap::ensure_map(ID3D12Device* device, uint32_t w, uint32_t h) {
    const uint32_t bw = (w + 7) / 8;
    const uint32_t bh = (h + 7) / 8;

    if (m_map != nullptr && m_map_w == bw && m_map_h == bh) {
        return true;
    }

    m_map.Reset();
    m_map_w = 0;
    m_map_h = 0;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = bw;
    tex.Height = bh + 1; // [BEAM FIT 03.11.2026] row bh: beam_fit's results
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R32_UINT;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (bw == 0 || bh == 0 ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_map)))) {
        m_map.Reset();
        m_last_error = "map texture failed";
        return false;
    }

    m_map->SetName(L"RE4VR GUI map");
    m_map_w = bw;
    m_map_h = bh;
    return true;
}

bool GuiMap::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* hud, ID3D12Resource* fin) {
    m_gui_w = 0;
    m_gui_h = 0;

    if (device == nullptr || cmd == nullptr || hud == nullptr || fin == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto hd = hud->GetDesc();
    const auto fd = fin->GetDesc();

    // same size and format: equal texels then give equal bilinear reads (the filtering precision follows the format)
    if (hd.Width != fd.Width || hd.Height != fd.Height || srv_format_for(hd.Format) != srv_format_for(fd.Format)) {
        m_last_error = "copies of different size or format";
        return false;
    }

    if (!ensure_map(device, (uint32_t)hd.Width, hd.Height)) {
        m_last_error = "map texture failed";
        return false;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = srv_format_for(hd.Format);
    device->CreateShaderResourceView(hud, &srv, slot(0));
    srv.Format = srv_format_for(fd.Format);
    device->CreateShaderResourceView(fin, &srv, slot(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R32_UINT;
    device->CreateUnorderedAccessView(m_map.Get(), nullptr, &uav, slot(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.FirstElement = 0;
    raw.Buffer.NumElements = 64;
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(m_count.Get(), nullptr, &raw, slot(3));

    // the GUI copies rest in COMMON: a compute read promotes them implicitly (as in UiClean)
    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);
    m_table = gpu;
    m_table_valid = true;

    // this frame's stamp: the blocks holding it are the marked ones (0 is what a new map holds)
    m_stamp = m_stamp + 1 != 0 ? m_stamp + 1 : 1;
    const uint32_t constants[8]{(uint32_t)hd.Width, hd.Height, m_map_w, m_map_h, m_stamp, 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(1, 8, constants, 0);
    cmd->Dispatch(m_map_w, m_map_h, 1); // one group per block

    D3D12_RESOURCE_BARRIER done[2]{};
    done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    done[0].UAV.pResource = m_map.Get();
    done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    done[1].UAV.pResource = m_count.Get();
    cmd->ResourceBarrier(2, done);

    m_gui_w = (uint32_t)hd.Width;
    m_gui_h = hd.Height;
    m_last_error = "";
    return true;
}

// [BEAM FIT 03.11.2026] see FoveaComposite.hpp
bool GuiMap::fit_beams(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const BeamFit& f) {
    if (device == nullptr || cmd == nullptr || f.count == 0 || m_fit_pso == nullptr || m_fit_mapped == nullptr || !m_table_valid || m_map == nullptr ||
        m_map_w < 64) {
        return false;
    }

    struct FitConstants {
        uint32_t count;
        uint32_t stamp;
        float tpt[2];
        float wide[4];
        int32_t box[16][4];
        float anchor[16][4];
    };
    static_assert(sizeof(FitConstants) <= FIT_CB_SIZE, "fits a slot");

    FitConstants k{};
    k.count = (std::min)(f.count, 16u);
    k.stamp = m_stamp;
    k.tpt[0] = f.tan_per_texel[0];
    k.tpt[1] = f.tan_per_texel[1];
    k.wide[0] = f.wide_left_top[0];
    k.wide[1] = f.wide_left_top[1];

    for (uint32_t i = 0; i < k.count; ++i) {
        std::memcpy(k.box[i], f.box[i], sizeof(k.box[i]));
        k.anchor[i][0] = f.anchor_x[i];
        k.anchor[i][1] = (float)f.index[i];
        k.anchor[i][2] = f.anchor_y[i];
    }

    const uint32_t ring = m_fit_ring;
    m_fit_ring = (m_fit_ring + 1) % RING_COUNT;
    std::memcpy(m_fit_mapped + (size_t)ring * FIT_CB_SIZE, &k, sizeof(k));

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_fit_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, m_table);
    const uint32_t constants[8]{m_gui_w, m_gui_h, m_map_w, m_map_h, m_stamp, 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(1, 8, constants, 0);
    cmd->SetComputeRootConstantBufferView(2, m_fit_cb->GetGPUVirtualAddress() + (UINT64)ring * FIT_CB_SIZE);
    cmd->Dispatch(k.count, 1, 1);

    D3D12_RESOURCE_BARRIER done{};
    done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    done.UAV.pResource = m_map.Get();
    cmd->ResourceBarrier(1, &done);
    return true;
}

void GuiMap::copy_count(ID3D12GraphicsCommandList* cmd, uint64_t frame) {
    if (cmd == nullptr || m_count == nullptr || m_readback == nullptr) {
        return;
    }

    const uint32_t s = (uint32_t)(frame % FRAMES);
    barrier(cmd, m_count.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(m_readback.Get(), (UINT64)s * 4, m_count.Get(), 0, 4);
    barrier(cmd, m_count.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_slot_frame[s] = frame;
    m_slot_ready[s] = true;
}

void GuiMap::collect(uint64_t frame) {
    if (m_mapped == nullptr) {
        return;
    }

    // frames at least four behind: their lists were waited for by the three-deep command rings before reuse
    for (;;) {
        int pick = -1;

        for (uint32_t s = 0; s < FRAMES; ++s) {
            if (m_slot_ready[s] && m_slot_frame[s] + 4 <= frame && (pick < 0 || m_slot_frame[s] < m_slot_frame[(uint32_t)pick])) {
                pick = (int)s;
            }
        }

        if (pick < 0) {
            break;
        }

        const uint64_t f = m_slot_frame[(uint32_t)pick];
        const uint32_t v = m_mapped[pick];
        m_slot_ready[(uint32_t)pick] = false;

        if (m_last_frame != ~0ull && f == m_last_frame + 1 && m_map_w > 0 && m_map_h > 0) {
            const uint32_t marked = (uint32_t)(v - m_last_count);
            const uint64_t blocks = (uint64_t)m_map_w * m_map_h;
            m_acc_marked += marked;
            m_acc_blocks += blocks;
            ++m_acc_frames;

            // [UI COPIES 30.10.2026] runs of nearly fully marked / mostly clear frames
            if ((uint64_t)marked * 100 >= blocks * 98) {
                ++m_full_run;
                m_calm_run = 0;
            } else {
                m_full_run = 0;
                m_calm_run = (uint64_t)marked * 2 < blocks ? m_calm_run + 1 : 0;
            }

            m_idle_run = marked == 0 ? m_idle_run + 1 : 0; // [MENUS fix86 18.11.2026]
        }

        m_last_frame = f;
        m_last_count = v;
    }

    if (m_acc_frames >= 90 && m_acc_blocks > 0) {
        m_share = (double)m_acc_marked / (double)m_acc_blocks;
        m_acc_marked = 0;
        m_acc_blocks = 0;
        m_acc_frames = 0;
    }
}

void GuiMap::reset() {
    if (m_readback != nullptr && m_mapped != nullptr) {
        m_readback->Unmap(0, nullptr);
    }

    m_mapped = nullptr;

    if (m_fit_cb != nullptr && m_fit_mapped != nullptr) {
        m_fit_cb->Unmap(0, nullptr);
    }

    m_fit_mapped = nullptr;
    m_fit_cb.Reset();
    m_fit_pso.Reset();
    m_table_valid = false;
    m_fit_ring = 0;
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_map.Reset();
    m_count.Reset();
    m_readback.Reset();
    m_slot_frame.fill(~0ull);
    m_slot_ready.fill(false);
    m_last_frame = ~0ull;
    m_last_count = 0;
    m_acc_marked = 0;
    m_acc_blocks = 0;
    m_acc_frames = 0;
    m_share = -1.0;
    m_full_run = 0;
    m_calm_run = 0;
    m_idle_run = 0;
    m_map_w = 0;
    m_map_h = 0;
    m_gui_w = 0;
    m_gui_h = 0;
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

// ================================================================================================================
// [EYE COMPOSE 29.10.2026] EyeCompose, see FoveaComposite.hpp
// ================================================================================================================
bool EyeCompose::setup(ID3D12Device* device) {
    // (the blocks' passes are optional: without them the gain alone as before 01.11)
    if (m_pso != nullptr && m_measure_pso != nullptr && m_gain_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> measure_shader{};
    ComPtr<ID3DBlob> gain_shader{};
    ComPtr<ID3DBlob> blocks_shader{};
    ComPtr<ID3DBlob> match_shader{};
    ComPtr<ID3DBlob> light_blur_shader{};
    ComPtr<ID3DBlob> errors{};

    // [GPU 30.10.2026] the curve table read from the texture: the copy in shared memory (round 3) was slower on the PC
    // [GPU 31.10.2026] groups of 32 x 32 pixels, one per thread (test of 31.10: 0.346 against 0.390 ms per eye with 16 x 16)
    const D3D_SHADER_MACRO tile_32x32[]{{"TSX", "32"}, {"TSY", "32"}, {nullptr, nullptr}};

    if (FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", tile_32x32, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[EyeCompose] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    errors.Reset();

    if (FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", nullptr, nullptr, "measure", "cs_5_0", 0, 0, &measure_shader,
            &errors))) {
        REVR_LOG(spdlog::error("[EyeCompose] measure shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    errors.Reset();

    if (FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", nullptr, nullptr, "gain", "cs_5_0", 0, 0, &gain_shader,
            &errors))) {
        REVR_LOG(spdlog::error("[EyeCompose] gain shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    // [BLOCKS 02.11.2026] the rectangle in blocks (means, histograms), the matching (gain, tone table, difference), the
    // large-scale light blurred over the blocks. If one of them fails the eye pass goes on with the gain alone (logged).
    bool blocks_compiled = true;

    for (auto [entry, blob] : {std::pair<const char*, ComPtr<ID3DBlob>*>{"blocks", &blocks_shader}, {"match", &match_shader},
             {"light_blur", &light_blur_shader}}) {
        errors.Reset();

        if (blocks_compiled && FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", nullptr, nullptr, entry, "cs_5_0", 0, 0,
                                   blob->GetAddressOf(), &errors))) {
            REVR_LOG(spdlog::error("[EyeCompose] {} shader compile failed (tone table and large-scale light off): {}", entry,
                errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
            blocks_compiled = false;
        }
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 8; // out, gain, GUI map, partial sums, [LIGHT] the blocks' means, the light, the histograms,
                                  // [TILE PASS fix105] the tiles' bits
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[EyeCompose] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[EyeCompose] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }


    pso_desc.CS.pShaderBytecode = measure_shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = measure_shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_measure_pso)))) {
        REVR_LOG(spdlog::error("[EyeCompose] CreateComputePipelineState (measure) failed"));
        m_last_error = "pipeline state failed";
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    pso_desc.CS.pShaderBytecode = gain_shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = gain_shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_gain_pso)))) {
        REVR_LOG(spdlog::error("[EyeCompose] CreateComputePipelineState (gain) failed"));
        m_last_error = "pipeline state failed";
        m_pso.Reset();
        m_measure_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    if (blocks_compiled) {
        pso_desc.CS.pShaderBytecode = blocks_shader->GetBufferPointer();
        pso_desc.CS.BytecodeLength = blocks_shader->GetBufferSize();
        bool ok = SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_blocks_pso)));
        pso_desc.CS.pShaderBytecode = match_shader->GetBufferPointer();
        pso_desc.CS.BytecodeLength = match_shader->GetBufferSize();
        ok = ok && SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_match_pso)));
        pso_desc.CS.pShaderBytecode = light_blur_shader->GetBufferPointer();
        pso_desc.CS.BytecodeLength = light_blur_shader->GetBufferSize();
        ok = ok && SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_light_blur_pso)));

        if (!ok) {
            REVR_LOG(spdlog::error("[EyeCompose] CreateComputePipelineState (blocks / match / light) failed: tone table and large-scale light off"));
            m_blocks_pso.Reset();
            m_match_pso.Reset();
            m_light_blur_pso.Reset();
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC cb_desc{};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Width = (UINT64)RING_COUNT * CB_SLOT_SIZE;
    cb_desc.Height = 1;
    cb_desc.DepthOrArraySize = 1;
    cb_desc.MipLevels = 1;
    cb_desc.SampleDesc.Count = 1;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // row 0, 4 floats per eye slot: the smoothed gain twice (ping-pong), the last measured ratio, this frame's gain; [TONE]
    // row 1, per eye slot: the tone table twice (ping-pong) and this frame's (64 entries each). Partials per eye slot: the
    // measure's 16 groups' two sums (as PART_W in the shader)
    D3D12_RESOURCE_DESC gain{};
    gain.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    gain.Width = 2 * 3 * 64;
    gain.Height = 2;
    gain.DepthOrArraySize = 1;
    gain.MipLevels = 1;
    gain.Format = DXGI_FORMAT_R32_FLOAT;
    gain.SampleDesc.Count = 1;
    gain.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    gain.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC partials = gain;
    partials.Width = 2 * 32;
    partials.Height = 1;

    // [LIGHT 01.11.2026] structured buffers of float4: per eye slot the blocks' means (periphery, fovea) and the light
    D3D12_RESOURCE_DESC light_sums{};
    light_sums.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    light_sums.Width = (UINT64)2 * 2 * LIGHT_SLOT * 16;
    light_sums.Height = 1;
    light_sums.DepthOrArraySize = 1;
    light_sums.MipLevels = 1;
    light_sums.SampleDesc.Count = 1;
    light_sums.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    light_sums.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC light = light_sums;
    light.Width = (UINT64)(2 * LIGHT_SLOT + 2) * 16; // [VIEW DIFFERENCE] + one float4 per eye slot: match's measurement
    // [BLOCKS 02.11.2026] the brightness histograms: 2 eye slots x 2 x 64 counts (raw buffer)
    D3D12_RESOURCE_DESC hist = light_sums;
    hist.Width = (UINT64)2 * 2 * 64 * 4;
    // [VIEW DIFFERENCE 01.11.2026] the CPU's copy of match's measurements: METRIC_RING frames x 2 eye slots
    D3D12_RESOURCE_DESC metric_rb = light_sums;
    metric_rb.Width = (UINT64)METRIC_RING * 2 * 16;
    metric_rb.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    void* metric_mapped = nullptr;

    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap))) ||
        FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gain, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_gain))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &partials, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_partials))) ||
        // (buffers start in COMMON and are promoted to UNORDERED_ACCESS by their first use in each list)
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &light_sums, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_light_sums))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &light, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_light))) ||
        // (a new default heap is zeroed: the histograms start empty)
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &hist, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_hist))) ||
        FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &metric_rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_metric_rb))) ||
        FAILED(m_metric_rb->Map(0, nullptr, &metric_mapped)) || metric_mapped == nullptr ||
        FAILED(m_cb->Map(0, &no_read, &mapped)) || mapped == nullptr) {
        REVR_LOG(spdlog::error("[EyeCompose] descriptor heap / buffers failed"));
        m_last_error = "descriptor heap / buffers failed";
        m_pso.Reset();
        m_measure_pso.Reset();
        m_gain_pso.Reset();
        m_blocks_pso.Reset();
        m_match_pso.Reset();
        m_light_blur_pso.Reset();
        m_root_sig.Reset();
        m_heap.Reset();
        m_cb.Reset();
        m_gain.Reset();
        m_partials.Reset();
        m_light_sums.Reset();
        m_light.Reset();
        m_hist.Reset();
        m_metric_rb.Reset();
        return false;
    }

    m_metric_mapped = (const float*)metric_mapped;
    m_metric_count[0] = 0;
    m_metric_count[1] = 0;

    m_gain->SetName(L"RE4VR EyeCompose brightness gain");
    m_partials->SetName(L"RE4VR EyeCompose brightness sums");
    m_light_sums->SetName(L"RE4VR EyeCompose light blocks");
    m_light->SetName(L"RE4VR EyeCompose light");
    m_hist->SetName(L"RE4VR EyeCompose brightness histograms");

    // [TILE PASS fix105 20.11.2026] the tiles' interface test as a pass of its own. Optional: if it cannot be made the eye
    // pass tests its tiles itself, as before.
    {
        ComPtr<ID3DBlob> tiles_shader{};
        errors.Reset();

        const D3D_SHADER_MACRO tile_entry[]{{"TILE_PASS_ENTRY", "1"}, {nullptr, nullptr}};

        if (FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", tile_entry, nullptr, "ui_tiles", "cs_5_0", 0, 0, &tiles_shader,
                &errors))) {
            REVR_LOG(spdlog::error("[EyeCompose] tile pass compile failed (the eye pass tests its tiles itself): {}",
                errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        } else {
            pso_desc.CS.pShaderBytecode = tiles_shader->GetBufferPointer();
            pso_desc.CS.BytecodeLength = tiles_shader->GetBufferSize();
            D3D12_RESOURCE_DESC tiles = gain;
            tiles.Width = TILES_W;
            tiles.Height = TILES_H;
            tiles.Format = DXGI_FORMAT_R32_UINT;

            if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_tiles_pso))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tiles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&m_tiles)))) {
                REVR_LOG(spdlog::error("[EyeCompose] tile pass: pipeline state or image failed (the eye pass tests its tiles itself)"));
                m_tiles_pso.Reset();
                m_tiles.Reset();
            } else {
                m_tiles->SetName(L"RE4VR EyeCompose interface tiles");
                REVR_LOG(spdlog::info("[EyeCompose] tile pass ready (the tiles' interface test in a pass of its own, {} x {} tiles at most)", TILES_W / 2, TILES_H));
            }
        }
    }
    m_parity[0] = 0;
    m_parity[1] = 0;
    m_cb_mapped = (uint8_t*)mapped;
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[EyeCompose] ready (shaders so far in this session: {} read from files, {} compiled)", s_files_read.load(), s_files_compiled.load()));

    return true;
}

// [BUILT-IN SHADERS fix145] see ShaderCompile.hpp
HRESULT compile_shader(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    return compile_cached(src, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

// [SHADER FILES fix110 20.11.2026] the folder of the compiled shaders' files (made if missing; files not used for 30 days
// are removed). Called once at start, before any pass is set up.
void set_shader_cache_dir(const std::filesystem::path& dir) {
    {
        std::scoped_lock _{s_files_mutex};

        if (s_files_dir == dir) {
            return;
        }

        s_files_dir = dir;
    }

    if (dir.empty()) {
        return;
    }

    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);
    const int removed = shader_files::prune(dir, std::chrono::hours{24 * 30});
    REVR_LOG(spdlog::info("[ShaderFiles] compiled shaders are kept in the folder \"{}\" beside the config ({}, {} old files removed)",
        dir.filename().string(), ec ? "the folder could not be made" : "folder ready", removed));
}

namespace {
// [fix109 20.11.2026] per variant, the state of the GPU test's two texts (as quadview::State::test_texts): variant_pso makes
// their pipelines only when it is 2, so the render thread never compiles them itself
std::array<std::atomic<int>, (size_t)EyeCompose::Params::Variant::Count> s_test_texts{};

// [fix108 20.11.2026] the macros of one text of the eye pass: the variant's own, then TILE_BITS (bits >= 1) and UI_NEVER
// (bits 2) or UI_LOOP (bits 3). The arrays are static: precompile_tests hands the list to another thread.
std::vector<D3D_SHADER_MACRO> eye_pass_defines(EyeCompose::Params::Variant v, int bits) {
    static const D3D_SHADER_MACRO gain_per_group[]{{"GAIN_PER_GROUP", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_16x8[]{{"TSX", "16"}, {"TSY", "8"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_8x8[]{{"TSX", "8"}, {"TSY", "8"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_32x16[]{{"TSX", "32"}, {"TSY", "16"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_16x16[]{{"TSX", "16"}, {"TSY", "16"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_30_on_32[]{{"TSX", "30"}, {"TSY", "30"}, {"NTX", "32"}, {"NTY", "32"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO before_1031[]{{"TSX", "32"}, {"TSY", "32"}, {"BEFORE_1031", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO select_tests[]{{"TSX", "32"}, {"TSY", "32"}, {"SELECT_TESTS", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO prev_2251[]{{"TSX", "32"}, {"TSY", "32"}, {"PREV_2251", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO prev_1101[]{{"TSX", "32"}, {"TSY", "32"}, {"PREV_1101", "1"}, {nullptr, nullptr}};
    static const D3D_SHADER_MACRO tile_32x32[]{{"TSX", "32"}, {"TSY", "32"}, {nullptr, nullptr}}; // (the current pass: only with tile_bits)
    const D3D_SHADER_MACRO* defines = v == EyeCompose::Params::Variant::Current       ? tile_32x32
                                      : v == EyeCompose::Params::Variant::GainPerGroup ? gain_per_group
                                      : v == EyeCompose::Params::Variant::Tile16x8    ? tile_16x8
                                      : v == EyeCompose::Params::Variant::Tile8x8     ? tile_8x8
                                      : v == EyeCompose::Params::Variant::Tile32x16   ? tile_32x16
                                      : v == EyeCompose::Params::Variant::Tile30on32  ? tile_30_on_32
                                      : v == EyeCompose::Params::Variant::Before1031  ? before_1031
                                      : v == EyeCompose::Params::Variant::SelectTests ? select_tests
                                      : v == EyeCompose::Params::Variant::Prev2251    ? prev_2251
                                      : v == EyeCompose::Params::Variant::Prev1101    ? prev_1101
                                                                          : tile_16x16;


    std::vector<D3D_SHADER_MACRO> list{};

    for (const auto* d = defines; d->Name != nullptr; ++d) {
        list.push_back(*d);
    }

    if (bits >= 1) {
        list.push_back({"TILE_BITS", "1"});
    }

    if (bits >= 2) {
        list.push_back({bits == 2 || bits == 5 ? "UI_NEVER" : bits == 3 ? "UI_LOOP" : "UI_MARKED", "1"});
    }

    if (bits == 5) {
        list.push_back({"UI_UNMARKED", "1"});
    }

    // [MARKED TEXT fix115] the marked tiles' text (see UI_TEST in the shader): 7 = the one in use (an anchor at the last
    // one's depth takes its mask), 6 = and without the line search, 8 = and without the panels' depth
    // [SMALL GROUPS fix117] 9 = the one in use, for groups half the eye pass's tile a side (asked for with the variant
    // of 8 x 8 pixels while the eye pass has 16 x 16)
    if ((bits >= 6 && bits <= 9) || bits == 12) {
        list.push_back({"UI_TEST", bits == 6 ? "3" : bits == 8 ? "6" : "2"});
    }

    // [OVER fix121] 12 = the marked tiles' text in use (7), with the interface put over the pixel the first pass wrote instead
    // of over the composite made again (test: only the interface's pixels can differ)
    if (bits == 12) {
        list.push_back({"MARKED_OVER", "1"});
    }

    if (bits == 9) {
        list.push_back({"TILE_BITS_SHIFT", "1"});
    }

    // [fix117] 10 = the marked tiles made again by the text WITHOUT the interface's calls (UI_MARKED's leaving of the other
    // tiles and UI_NEVER): the GPU test's measure of what a second pass over the marked tiles costs by itself (the image
    // changes: no interface outside the fovea)
    if (bits == 10 || bits == 11) {
        list.push_back({"UI_NEVER", "1"});
    }

    // [DIRECT fix120] 11 = the marked tiles made again by every thread on its own, without the stages (GPU test, measurement only)
    if (bits == 11) {
        list.push_back({"MARKED_DIRECT", "1"});
    }

    list.push_back({nullptr, nullptr});
    return list;
}
} // namespace

// [fix129 22.11.2026] The eye pass's texts for the other periphery size, compiled ahead on a thread of their own.
// RE4 test of 22.11.2026 21:16 (fix128, "Lighter when the GPU is late" on, a heavy fight): the guard's step to the
// periphery's image of the wide view's size made the pass change its groups from 16 x 16 to 32 x 32 pixels, four texts
// that had never been compiled on that machine: 9.2 + 22.6 + 22.9 + 3.9 s on the render thread, the picture stood still
// for a minute (SteamVR: application not responding). The texts are kept in files afterwards, but every build that
// changes the pass's text would do it again at the guard's first step.
// With the first dispatch the texts of the group size NOT in use (the kinds the build's path asks for) are compiled by
// a thread of low priority (found in the files at once when they are there). While it runs variant_pso makes none of
// them itself: the pass falls back to the one with its own tile test (the same picture, a little slower) until they
// are ready.
namespace {
std::atomic<int> s_other_texts{0};    // 0 not started, 1 compiling, 2 ready, 3 failed
std::atomic<int> s_other_variant{-1}; // the variant whose texts that thread compiles

void precompile_other_variant(EyeCompose::Params::Variant used) {
    using V = EyeCompose::Params::Variant;

    if (used != V::Current && used != V::Tile16x16) {
        return;
    }

    int expected = 0;

    if (!s_other_texts.compare_exchange_strong(expected, 1)) {
        return;
    }

    const V other = used == V::Current ? V::Tile16x16 : V::Current;
    s_other_variant.store((int)other);

    std::thread([other] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        const auto t0 = std::chrono::steady_clock::now();
#endif
        bool ok = true;

        // (what EyeCompose::dispatch asks for on the build's path: the first pass, the marked tiles' text and the one in
        // use, the HUD over the stored pixels, the pass with the tiles' bits; the test of 22.11.2026 compiled four)
        for (const int bits : {2, 4, 7, 12, 1}) {
            const auto list = eye_pass_defines(other, bits);
            ComPtr<ID3DBlob> shader{};
            ComPtr<ID3DBlob> errors{};
            ok = SUCCEEDED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", list.data(), nullptr, "main", "cs_5_0", 0, 0, &shader, &errors)) && ok;
        }

        REVR_LOG(spdlog::info("[EyeCompose] the pass's texts for the other periphery size (variant {}) are ready ahead of use after {:.1f} s: {}", (int)other,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), ok ? "ok" : "one failed (it is compiled when asked for)"));
        s_other_texts.store(ok ? 2 : 3);
    }).detach();
}
} // namespace

// [fix108 20.11.2026] the GPU test's two texts of the pass (UI_NEVER, UI_LOOP) compiled ahead on another thread, from the
// test's countdown on: the text with the loop takes twice the time of the pass in use (142 s against 68 under Wine), which
// on the render thread would freeze the picture at the start of its phase. Only the compilation (compile_cached keeps the
// code and is safe from any thread); the pipeline is made by variant_pso when the phase begins.
void EyeCompose::precompile_tests(Params::Variant v) {
    if (v == Params::Variant::Tile32x32) {
        v = Params::Variant::Current;
    }

    if ((size_t)v >= s_test_texts.size() || v == Params::Variant::Before1031) {
        return;
    }

    // [fix109] 0 -> 1 once per variant and session; the state also goes to quadview::State for the test's countdown
    int expected = 0;

    if (!s_test_texts[(size_t)v].compare_exchange_strong(expected, 1)) {
        quadview::State::get().test_texts.store(expected, std::memory_order_relaxed);
        return;
    }

    quadview::State::get().test_texts.store(1, std::memory_order_relaxed);

    std::thread([v] {
        bool ok = true;

        for (int bits = 2; bits <= 3; ++bits) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            const auto t0 = std::chrono::steady_clock::now();
#endif
            const auto list = eye_pass_defines(v, bits);
            ComPtr<ID3DBlob> shader{};
            ComPtr<ID3DBlob> errors{};
            const HRESULT hr = compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", list.data(), nullptr, "main", "cs_5_0", 0, 0, &shader, &errors);
            ok = ok && SUCCEEDED(hr);
            REVR_LOG(spdlog::info("[EyeCompose] GPU test text {} of variant {} compiled ahead in {:.1f} s: {}", bits == 2 ? "UI_NEVER" : "UI_LOOP", (int)v,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), SUCCEEDED(hr) ? "ok" : "failed"));
        }

        s_test_texts[(size_t)v].store(ok ? 2 : 3);
        quadview::State::get().test_texts.store(ok ? 2 : 3, std::memory_order_relaxed);
    }).detach();
}

#if defined(REVR_SHADER_GENERATOR)
// [BUILT-IN SHADERS fix145 23.11.2026] tests/shaders: the eye pass's texts that are compiled after its root signature (not
// given by vkd3d on lavapipe): the tile pass (setup) and the texts dispatch asks for (variant_pso) on the build's path, for
// the two group sizes: 0 the pass alone, 1 with the tiles' bits, 2 without the interface's calls, 4 the marked tiles, 7 the
// marked tiles' text in use (marked_test 2), 12 the interface over the stored pixels (marked_over). The same calls as there.
void eye_compose_generator_texts() {
    ComPtr<ID3DBlob> blob{};
    ComPtr<ID3DBlob> errors{};
    const D3D_SHADER_MACRO tile_entry[]{{"TILE_PASS_ENTRY", "1"}, {nullptr, nullptr}};
    compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", tile_entry, nullptr, "ui_tiles", "cs_5_0", 0, 0, &blob, &errors);

    for (const auto v : {EyeCompose::Params::Variant::Current, EyeCompose::Params::Variant::Tile16x16}) {
        for (const int bits : {0, 1, 2, 4, 7, 12}) {
            const auto list = eye_pass_defines(v, bits);
            blob.Reset();
            errors.Reset();
            compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", list.data(), nullptr, "main", "cs_5_0", 0, 0, &blob, &errors);
        }
    }
}
#endif

ID3D12PipelineState* EyeCompose::variant_pso(ID3D12Device* device, Params::Variant v, int bits) {
    // [fix108, fix111] bits: 0 = the pass with its own tile test, 1 = with TILE_BITS, 2 = and UI_NEVER (the eye image of the
    // two passes), 3 = and UI_LOOP (was a GPU test), 4 = and UI_MARKED (the marked tiles again), [fix112] 5 = UI_NEVER and
    // UI_UNMARKED (the eye image without the marked tiles), [fix115] 6 - 8 = UI_MARKED with UI_TEST 3, 2, 6 (7: in use),
    // [fix117] 9 = 7 for groups half the tile a side, 10 = UI_MARKED with UI_NEVER (GPU test)
    bits = std::clamp(bits, 0, 12);
    const bool tile_bits = bits != 0;

    if (v == Params::Variant::Tile32x32) {
        v = Params::Variant::Current;
    }

    if (v == Params::Variant::Current && !tile_bits) {
        return m_pso.Get();
    }

    // [TILE PASS fix106 20.11.2026] tile_bits: the same pass compiled with TILE_BITS (its tiles' interface bits from ui_tiles
    // instead of its own test); the pass of before 31.10 has none
    if (tile_bits && v == Params::Variant::Before1031) {
        return nullptr;
    }

    const auto i = (size_t)v;
    auto& psos = tile_bits ? m_bits_pso[(size_t)bits - 1] : m_variant_pso;
    auto& failed = tile_bits ? m_bits_failed[(size_t)bits - 1] : m_variant_failed;

    if (i >= psos.size() || failed[i] || m_root_sig == nullptr) {
        return nullptr;
    }

    // [fix109] the GPU test's texts only once their thread has compiled them (never here: 45 s on the render thread)
    if (bits == 3 && psos[i] == nullptr && s_test_texts[i].load() != 2) {
        return nullptr;
    }

    if (psos[i] != nullptr) {
        return psos[i].Get();
    }

    // [fix129] the other periphery size's texts are being compiled by their own thread: never here meanwhile
    if (tile_bits && (int)v == s_other_variant.load() && s_other_texts.load() == 1) {
        return nullptr;
    }

    const auto list = eye_pass_defines(v, bits);
    const D3D_SHADER_MACRO* defines = list.data();

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};
    failed[i] = true;

    if (FAILED(compile_cached(EYE_COMPOSE_SHADER, sizeof(EYE_COMPOSE_SHADER) - 1, "EyeCompose", defines, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[EyeCompose] {} {} compile failed: {}", tile_bits ? "pass with the tiles' bits, variant" : "test variant", i,
            errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&psos[i])))) {
        REVR_LOG(spdlog::error("[EyeCompose] {} {}: CreateComputePipelineState failed", tile_bits ? "pass with the tiles' bits, variant" : "test variant", i));
        return nullptr;
    }

    failed[i] = false;
    REVR_LOG(spdlog::info("[EyeCompose] {} {} ready", tile_bits ? "pass with the tiles' bits, variant" : "test variant", i));
    return psos[i].Get();
}

bool EyeCompose::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* periphery, ID3D12Resource* fovea, ID3D12Resource* out,
    ID3D12Resource* centre_color, ID3D12Resource* centre_hud, ID3D12Resource* centre_fin, ID3D12Resource* fovea_color, ID3D12Resource* fovea_hud,
    const GuiMap* gui_map, const Params& p) {
    m_fovea_drawn = false;

    if (device == nullptr || cmd == nullptr || periphery == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto pdesc = periphery->GetDesc();
    const auto odesc = out->GetDesc();
    const auto per_format = srv_format_for(pdesc.Format);
    const auto out_format = uav_format_for(odesc.Format);

    if (out_format == DXGI_FORMAT_UNKNOWN || is_srgb(odesc.Format) || pdesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        m_last_error = "unsupported format";
        return false;
    }

    const uint32_t out_w = (uint32_t)odesc.Width;
    const uint32_t out_h = odesc.Height;

    // the fovea rectangle: only the part inside the eye image is drawn (as FoveaComposite, which needed 8 x 8 of it)
    bool fovea_ok = p.fovea && fovea != nullptr && p.rect_w > 0 && p.rect_h > 0;

    if (fovea_ok) {
        const int32_t vis_x0 = (std::max)(p.rect_x, 0);
        const int32_t vis_y0 = (std::max)(p.rect_y, 0);
        const int32_t vis_x1 = (std::min)(p.rect_x + (int32_t)p.rect_w, (int32_t)out_w);
        const int32_t vis_y1 = (std::min)(p.rect_y + (int32_t)p.rect_h, (int32_t)out_h);
        fovea_ok = vis_x1 - vis_x0 >= 8 && vis_y1 - vis_y0 >= 8;
    }

    const bool centre_ok = centre_color != nullptr && centre_hud != nullptr && centre_fin != nullptr;
    // [MENUS fix82] the fovea image holds its view's interface: none of the fovea view's is added (its images are not read)
    const bool inside = p.fovea_ui_inside && fovea_ok && p.ui.rect[2] > 0.0f && p.ui.rect[3] > 0.0f;
    const bool fovea_ui_ok = !inside && fovea_color != nullptr && fovea_hud != nullptr && p.ui.rect[2] > 0.0f && p.ui.rect[3] > 0.0f;
    // [MENUS fix83 17.11.2026] with the interface inside the fovea image the two images are the fovea view's interface copies
    // (with GUI, without GUI; both resting in COMMON like the wide view's): where they differ, the view drew interface
    const bool pair_ok = inside && fovea_color != nullptr && fovea_hud != nullptr && fovea_color->GetDesc().Width == fovea_hud->GetDesc().Width &&
                         fovea_color->GetDesc().Height == fovea_hud->GetDesc().Height && fovea_color->GetDesc().Format == fovea_hud->GetDesc().Format;
    // ([MENUS fix86] or UiSteady's results of those copies: UiSteady::SCALE times the size, read by UV like the copies; the
    // map's blocks are found from the UV and the map's own size)
    const auto chd = centre_ok ? centre_hud->GetDesc() : D3D12_RESOURCE_DESC{};
    const bool map_ok = centre_ok && gui_map != nullptr && gui_map->map() != nullptr && gui_map->gui_w() != 0 && gui_map->gui_h() != 0 &&
                        ((gui_map->gui_w() == (uint32_t)chd.Width && gui_map->gui_h() == chd.Height) ||
                            (gui_map->gui_w() * UiSteady::SCALE == (uint32_t)chd.Width && gui_map->gui_h() * UiSteady::SCALE == chd.Height));

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    auto make_srv = [&](uint32_t i, ID3D12Resource* r) {
        if (r != nullptr) {
            srv.Format = srv_format_for(r->GetDesc().Format);
            device->CreateShaderResourceView(r, &srv, slot(i));
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = srv;
            null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            device->CreateShaderResourceView(nullptr, &null_srv, slot(i));
        }
    };

    make_srv(0, periphery);
    make_srv(1, fovea_ok ? fovea : nullptr);
    make_srv(2, centre_ok ? centre_color : nullptr);
    make_srv(3, centre_ok ? centre_hud : nullptr);
    make_srv(4, centre_ok ? centre_fin : nullptr);
    make_srv(5, fovea_ui_ok || pair_ok ? fovea_color : nullptr);
    make_srv(6, fovea_ui_ok || pair_ok ? fovea_hud : nullptr);

    // the output curve (UiCurve, resting in NON_PIXEL_SHADER_RESOURCE); a null view when it is not measured yet
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(p.ui.curve, &srv, slot(7));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = out_format;
    device->CreateUnorderedAccessView(out, nullptr, &uav, slot(8));
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(m_gain.Get(), nullptr, &uav, slot(9));
    uav.Format = DXGI_FORMAT_R32_UINT;
    device->CreateUnorderedAccessView(map_ok ? gui_map->map() : nullptr, nullptr, &uav, slot(10));
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateUnorderedAccessView(m_partials.Get(), nullptr, &uav, slot(11));

    // [LIGHT 01.11.2026] the blocks' means and the light (structured buffers of float4)
    D3D12_UNORDERED_ACCESS_VIEW_DESC buf{};
    buf.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buf.Format = DXGI_FORMAT_UNKNOWN;
    buf.Buffer.StructureByteStride = 16;
    buf.Buffer.NumElements = 2 * 2 * LIGHT_SLOT;
    device->CreateUnorderedAccessView(m_light_sums.Get(), nullptr, &buf, slot(12));
    buf.Buffer.NumElements = 2 * LIGHT_SLOT + 2;
    device->CreateUnorderedAccessView(m_light.Get(), nullptr, &buf, slot(13));
    // [BLOCKS 02.11.2026] the histograms (raw buffer of counts)
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.Buffer.NumElements = 2 * 2 * 64;
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(m_hist.Get(), nullptr, &raw, slot(14));
    // [TILE PASS fix105] the tiles' bits (a null view without the pass)
    uav.Format = DXGI_FORMAT_R32_UINT;
    device->CreateUnorderedAccessView(m_tiles.Get(), nullptr, &uav, slot(15));

    // the separate passes stored the periphery and the composite in images of the eye format: UNORM clamped each to [0, 1]
    const bool unorm = out_format == DXGI_FORMAT_R10G10B10A2_UNORM || out_format == DXGI_FORMAT_R8G8B8A8_UNORM || out_format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       out_format == DXGI_FORMAT_R16G16B16A16_UNORM;
    // [BLOCKS 02.11.2026] the blocks of the rectangle (at most LIGHT_MAX per side). With the tone table, the large-scale light
    // or the view difference the blocks pass and match make everything (gain and table for the next frame; the eye pass uses
    // the last frame's, which the blocks were matched with); with the gain alone the measure and the gain pass as before.
    const uint32_t light_bx = (p.rect_w + LIGHT_BLOCK - 1) / LIGHT_BLOCK;
    const uint32_t light_by = (p.rect_h + LIGHT_BLOCK - 1) / LIGHT_BLOCK;
    const bool blocks_fit = fovea_ok && light_bx <= LIGHT_MAX && light_by <= LIGHT_MAX && m_blocks_pso != nullptr && m_match_pso != nullptr &&
                            m_light_blur_pso != nullptr;
    const bool tone = blocks_fit && p.match_brightness && p.tone_curve; // [TONE 01.11.2026]
    const bool light = blocks_fit && p.periphery_light;                  // [LIGHT 01.11.2026]
    const bool backdrop = blocks_fit && p.menu_backdrop && !inside;      // [MENU BACKDROP 15.11.2026] ([fix83] not with the interface inside)
    const bool metric = blocks_fit && p.view_metric && m_metric_rb != nullptr; // [VIEW DIFFERENCE 01.11.2026]
    const bool in_blocks = tone || light || metric || backdrop;
    const bool measure = fovea_ok && p.match_brightness && !in_blocks;
    const uint32_t slot_index = (std::min)(p.gain_slot, 1u);

    EyeComposeConstants k{};
    k.out_w = out_w;
    k.out_h = out_h;
    k.per_w = (uint32_t)pdesc.Width;
    k.per_h = pdesc.Height;
    k.flags = (centre_ok ? 1u : 0u) | (fovea_ui_ok || inside ? 2u : 0u) | (p.ui.debug_tint ? 4u : 0u) |
              (centre_ok && is_srgb(srv_format_for(centre_color->GetDesc().Format)) ? 8u : 0u) |
              (fovea_ui_ok && is_srgb(srv_format_for(fovea_color->GetDesc().Format)) ? 16u : 0u) | (p.ui.curve != nullptr ? 32u : 0u) |
              (fovea_ui_ok ? 64u : 0u) | (map_ok ? 128u : 0u) | (p.ui.post ? 256u : 0u) | (p.encode_periphery ? 512u : 0u) | (fovea_ok ? 1024u : 0u) |
              (fovea_ok && (p.match_brightness || backdrop) ? 2048u : 0u) | (fovea_ok && p.debug_border ? 4096u : 0u) | 8192u | (unorm ? 16384u : 0u) |
              (map_ok && p.tile_skip ? 32768u : 0u) | (p.quantize_10bit ? 65536u : 0u) | (tone ? 131072u : 0u) |
              (light ? 262144u : 0u) | (metric ? 524288u : 0u) | (in_blocks ? 1048576u : 0u) |
              (fovea_ok && p.holes_to_fovea ? 2097152u : 0u) | // [HOLE BAND 13.11.2026]
              (backdrop ? 4194304u : 0u) |                     // [MENU BACKDROP 15.11.2026]
              (inside ? 8388608u : 0u) |                       // [MENUS fix82]
              (pair_ok ? 16777216u : 0u) |                     // [MENUS fix83]
              (pair_ok && p.fovea_backdrop ? 33554432u : 0u);  // [BACKDROP fix83]
    k.anchor_count = (std::min)(p.ui.anchor_count, MonoUi::MAX_ANCHORS);
    k.slot = slot_index;
    k.parity = m_parity[slot_index] & 1u;
    k.feather = std::clamp(p.feather_px, 1.0f, (float)(std::max)((std::min)(p.rect_w, p.rect_h), 2u) * 0.5f);
    std::memcpy(k.eye_tan, p.ui.eye_tan, sizeof(k.eye_tan));
    std::memcpy(k.wide_tan, p.ui.wide_tan, sizeof(k.wide_tan));
    std::memcpy(k.rect, p.ui.rect, sizeof(k.rect));
    k.rect_x = p.rect_x;
    k.rect_y = p.rect_y;
    k.rect_w = p.rect_w;
    k.rect_h = p.rect_h;
    k.fov_w = fovea_ok ? (uint32_t)fovea->GetDesc().Width : 0u;
    k.fov_h = fovea_ok ? fovea->GetDesc().Height : 0u;
    k.gui_w = map_ok ? gui_map->gui_w() : 0u;
    k.gui_h = map_ok ? gui_map->gui_h() : 0u;
    k.map_w = map_ok ? gui_map->map_w() : 0u;
    k.map_h = map_ok ? gui_map->map_h() : 0u;
    k.map_stamp = map_ok ? gui_map->stamp() : 0u;
    k.eye_x = p.ui.eye_x;
    k.panel_depth = (std::max)(p.ui.panel_depth, 0.05f);
    k.ui_feather = p.ui.feather_px;
    k.rate = std::clamp(p.gain_rate, 0.01f, 1.0f);
    k.post[0] = p.ui.post_sharp;
    k.post[1] = p.ui.post_sat;
    k.post[2] = p.ui.post_gamma;
    k.post[3] = p.ui.post_contrast;
    k.post2[0] = p.ui.post_edge; // [EDGE SOFTEN 11.11.2026]
    k.post2[1] = p.ui.post_edge_threshold;
    std::memcpy(k.anchors, p.ui.anchors, sizeof(k.anchors));
    std::memcpy(k.anchor_ext, p.ui.anchor_ext, sizeof(k.anchor_ext));
    k.loops[0] = p.ui.post_contrast != 1.0f ? 1u : 0u; // the shader's test g_post.w != 1 (inside the post pass)
    k.loops[1] = (k.flags & 16384u) != 0 ? 1u : 0u;
    k.light[0] = std::clamp(p.light_px, 16.0f, 64.0f) / (float)LIGHT_BLOCK; // [LIGHT 02.11.2026] the blur in blocks
    k.light[1] = std::clamp(p.light_limit, 0.0f, 8.0f);                      // [GLOW 13.11.2026]
    k.light[2] = p.light_add_only ? 1.0f : 0.0f;
    k.light[3] = p.light_auto ? 1.0f : 0.0f;                                  // [AUTO LIGHT 14.11.2026]

    // [MENUS fix81] the jitter as UV offsets (of the image the fovea's interface is read from, of the wide view's colour)
    if (fovea_ui_ok || pair_ok) {
        const auto fd = fovea_color->GetDesc();
        k.ui_jit[0] = p.fovea_jitter[0] / (float)(std::max)((uint32_t)fd.Width, 1u);
        k.ui_jit[1] = p.fovea_jitter[1] / (float)(std::max)(fd.Height, 1u);
    }

    if (centre_ok) {
        const auto wd = centre_color->GetDesc();
        k.ui_jit[2] = p.wide_jitter[0] / (float)(std::max)((uint32_t)wd.Width, 1u);
        k.ui_jit[3] = p.wide_jitter[1] / (float)(std::max)(wd.Height, 1u);
    }

    // [GPU 30.10.2026] the test variant's pass (the current one when it cannot be made) and its group size
    // [GPU 31.10.2026] the current pass's 32 x 32 groups keep the image only while their periphery taps fit in shared
    // memory (16 per axis: at most 12 periphery pixels per 33 eye pixels, with a margin); a denser periphery (a bigger
    // render size) gets the 16 x 16 groups (up to 12 per 17)
    auto used = p.variant == Params::Variant::Tile32x32 ? Params::Variant::Current : p.variant;

    // (the gain-per-group variant makes its gain from the measure's sums: with the blocks there are none)
    if (in_blocks && used == Params::Variant::GainPerGroup) {
        used = Params::Variant::Current;
    }
    const bool big_fits = (uint64_t)pdesc.Width * 34 <= (uint64_t)out_w * 12 && (uint64_t)pdesc.Height * 34 <= (uint64_t)out_h * 12;
    const bool is_32 = used == Params::Variant::Current || used == Params::Variant::Before1031 || used == Params::Variant::SelectTests ||
                       used == Params::Variant::Prev2251 || used == Params::Variant::Prev1101;

    if ((is_32 || used == Params::Variant::Tile30on32) && !big_fits) {
        used = Params::Variant::Tile16x16;
    }

    ID3D12PipelineState* pso = variant_pso(device, used);

    if (pso == nullptr) {
        used = Params::Variant::Current;
        pso = m_pso.Get();
    }

    precompile_other_variant(used); // [fix129] once: the other group size's texts ahead of the GPU guard's steps

    if (p.prepare_tests) { // [fix108] the GPU test is counting down or running
        precompile_tests(used);
    }

    // [TILE PASS fix106 20.11.2026] the same pass compiled to read its tiles' bits (null: its own test, as fix104)
    const bool tile_test_on = (k.flags & (1u | 128u | 32768u)) == (1u | 128u | 32768u);
    const bool bits_possible = p.tile_pass && tile_test_on && m_tiles_pso != nullptr && m_tiles != nullptr;
    // [TWO PASSES fix111 20.11.2026] the eye image by the text without the interface's calls, then the marked tiles again by
    // the text that has them (see UI_MARKED in the shader): 0.2 - 0.3 ms less for the two eyes whenever an interface
    // element is in the picture. Without one of the two pipelines: the single pass of fix106 - fix110.
    ID3D12PipelineState* marked_pso = nullptr;
    ID3D12PipelineState* bits_pso = nullptr;

    bool first_skips = false; // [fix112] the first pass leaves the marked tiles to the second

    if (bits_possible && p.two_pass && p.ui_calls == 0) {
        // [fix114 21.11.2026] RE4 test of fix113: with no interface in the picture the two eye passes cost 0.931 - 0.940 ms
        // (0.894 - 0.897 with fix112, 0.886 - 0.891 with fix111): the text that leaves the marked tiles out reads its
        // tile's bits and synchronises its group before anything else, in every tile, 0.04 ms for the two eyes. With
        // interface (HUD and 3 beams) the same text was 0.09 ms faster than fix112's two passes (1.058 against 1.150).
        // So it runs only while the wide view shows interface: the map's count of marked blocks, read back a few frames
        // late (both ways the image is the same, a late switch costs a few frames of the slower pass).
        // [fix115 21.11.2026] RE4 test of fix114, under the frame time, runs within 0.003 ms: with the weapon HUD that text
        // and its pass cost 0.036 ms MORE than fix112's two passes (1.033 against 0.997), so fix112's are the chain again
        // and first_skips_marked is off (the tests of fix113 that showed the opposite ran over the frame time).
        const bool map_busy = !p.first_skips_by_map || gui_map == nullptr || gui_map->idle_run() < 30;

        if (p.first_skips_marked && map_busy) {
            bits_pso = variant_pso(device, used, 5);
            first_skips = bits_pso != nullptr;
        }

        if (bits_pso == nullptr) {
            bits_pso = variant_pso(device, used, 2);
        }

        marked_pso = bits_pso != nullptr ? variant_pso(device, used, 4) : nullptr;

        // [MARKED TEXT fix115] the marked tiles' text: 2 = an anchor at the last one's depth takes its mask (the same
        // image; in use), 1 / 3 = the GPU test's two texts, 0 = the text of fix111 - fix114 (also where one is not there)
        // [fix117] 4 = the GPU test's marked pass without the interface's calls (measurement only)
        // [DIRECT fix120] 5 = the same by every thread on its own (kind 11)
        if (marked_pso != nullptr && p.marked_test >= 1 && p.marked_test <= 5) {
            if (auto* const test_pso = variant_pso(device, used, p.marked_test >= 4 ? 6 + (int)p.marked_test : 5 + (int)p.marked_test); test_pso != nullptr) {
                marked_pso = test_pso;
            }
        }

        // [OVER fix121 22.11.2026] 6 = the interface over the first pass's stored pixels (kind 12, see MARKED_OVER in the
        // shader). The text reads the eye image it writes, which needs the format's typed UAV load (asked once per format);
        // not with the sharpening / colours after the composite (256), the menus' backdrop (4194304) or the debug tint (4),
        // where the stored pixel is not what the interface goes over in the usual text.
        // ([fix122] nor where the first pass leaves the marked tiles to this one (the GPU test's first_skips_marked): there is
        // no stored pixel to go over)
        if (marked_pso != nullptr && (p.marked_test == 6 || (p.marked_test == 2 && p.marked_over)) && !p.first_skips_marked &&
            (k.flags & (4u | 256u | 4194304u)) == 0) {
            if (m_over_format != out_format) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
                fs.Format = out_format;
                m_over_ok = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
                            (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
                m_over_format = out_format;

                if (!m_over_ok) {
                    REVR_LOG(spdlog::warn("[FoveaComposite] eye image format {} has no typed UAV load: the interface over the stored pixels is not used", (int)out_format));
                }
            }

            if (m_over_ok) {
                if (auto* const over_pso = variant_pso(device, used, 12); over_pso != nullptr) {
                    marked_pso = over_pso;
                    ++m_over_frames;
                }
            }
        }

        if (marked_pso == nullptr) {
            bits_pso = nullptr;
            first_skips = false;
        }
    } else if (bits_possible && p.ui_calls != 0) {
        bits_pso = variant_pso(device, used, 1 + (int)(std::min)(p.ui_calls, 2u)); // (GPU test: one of the texts alone)
    }

    if (bits_possible && bits_pso == nullptr) {
        bits_pso = variant_pso(device, used, 1);
    }

    const bool is_32_now = used == Params::Variant::Current || used == Params::Variant::Before1031 || used == Params::Variant::SelectTests ||
                           used == Params::Variant::Prev2251 || used == Params::Variant::Prev1101;
    const uint32_t tile_w = used == Params::Variant::Tile8x8 ? 8u : is_32_now || used == Params::Variant::Tile32x16 ? 32u
                            : used == Params::Variant::Tile30on32 ? 30u : 16u;
    const uint32_t tile_h = used == Params::Variant::Tile16x8 || used == Params::Variant::Tile8x8 ? 8u : is_32_now ? 32u
                            : used == Params::Variant::Tile30on32 ? 30u : 16u;

    // [TILE PASS fix105, fix106 20.11.2026] the tiles' interface bits from a pass of their own (ui_tiles) when the eye pass
    // tests its tiles at all (wide view's interface, its map, the skip), both passes exist and the tiles fit the image.
    const uint32_t tiles_x = (out_w + tile_w - 1) / tile_w;
    const uint32_t tiles_y = (out_h + tile_h - 1) / tile_h;
    const bool tile_pass = bits_pso != nullptr && tiles_x * 2 <= TILES_W && tiles_y <= TILES_H;

    if (tile_pass) {
        pso = bits_pso;
        k.loops[2] = tile_w;
        k.loops[3] = tile_h;

        if (p.tile_none) { // the GPU test's "no tile marked" (ui_tiles writes 0)
            k.flags |= 67108864u;
        }
    }

    std::memcpy(m_cb_mapped + (size_t)ring * CB_SLOT_SIZE, &k, sizeof(k));

    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto ENGINE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto engine_read = compute_read_state(ENGINE);

    // all transitions in one call (the GUI copies rest in COMMON: a compute read promotes them implicitly)
    std::array<D3D12_RESOURCE_BARRIER, 4> to_read{};
    std::array<D3D12_RESOURCE_BARRIER, 4> back{};
    uint32_t n = 0;

    auto add = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        to_read[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_read[n].Transition.pResource = r;
        to_read[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_read[n].Transition.StateBefore = before;
        to_read[n].Transition.StateAfter = after;
        back[n] = to_read[n];
        back[n].Transition.StateBefore = after;
        back[n].Transition.StateAfter = before;
        ++n;
    };

    add(periphery, UAV, NPSR);

    if (fovea_ok) {
        add(fovea, UAV, NPSR);
    }

    if (centre_ok) {
        add(centre_color, ENGINE, engine_read);
    }

    if (fovea_ui_ok) {
        add(fovea_color, ENGINE, engine_read);
    }

    cmd->ResourceBarrier(n, to_read.data());

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress() + (UINT64)ring * CB_SLOT_SIZE);
    cmd->SetComputeRootDescriptorTable(1, gpu);

    // [TILE PASS fix105 20.11.2026] the tiles' bits for this eye. No barrier of its own with the blocks: theirs follows.
    if (tile_pass) {
        cmd->SetPipelineState(m_tiles_pso.Get());
        cmd->Dispatch((tiles_x + 7) / 8, (tiles_y + 7) / 8, 1);

        if (!in_blocks) {
            D3D12_RESOURCE_BARRIER tiles_done{};
            tiles_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            tiles_done.UAV.pResource = m_tiles.Get();
            cmd->ResourceBarrier(1, &tiles_done);
        }
    }

    // measure this frame's fovea against the periphery (16 groups), make this frame's gain from it (one thread), then
    // compose with it
    if (measure) {
        cmd->SetPipelineState(m_measure_pso.Get());
        cmd->Dispatch(4, 4, 1);

        D3D12_RESOURCE_BARRIER sums_done{};
        sums_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        sums_done.UAV.pResource = m_partials.Get();
        cmd->ResourceBarrier(1, &sums_done);

        if (used != Params::Variant::GainPerGroup) {
            cmd->SetPipelineState(m_gain_pso.Get());
            cmd->Dispatch(1, 1, 1);

            D3D12_RESOURCE_BARRIER gain_done{};
            gain_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            gain_done.UAV.pResource = m_gain.Get();
            cmd->ResourceBarrier(1, &gain_done);
        }
    }

    // [BLOCKS 02.11.2026] the blocks (one group per 4 x 4), the light blurred over them, then match (gain, table,
    // difference for the next frame; one group) together with the eye pass: no barrier between the two (the eye pass reads
    // the last frame's places of the gain texture, match writes the others), so match's one group no longer holds the GPU
    if (in_blocks) {
        cmd->SetPipelineState(m_blocks_pso.Get());
        cmd->Dispatch((light_bx + 3) / 4, (light_by + 3) / 4, 1);

        D3D12_RESOURCE_BARRIER blocks_done[3]{};
        blocks_done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blocks_done[0].UAV.pResource = m_light_sums.Get();
        blocks_done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blocks_done[1].UAV.pResource = m_hist.Get();
        blocks_done[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // [TILE PASS fix105] the tiles' bits, written just before
        blocks_done[2].UAV.pResource = m_tiles.Get();
        cmd->ResourceBarrier(tile_pass ? 3 : 2, blocks_done);

        if (light || backdrop) {
            cmd->SetPipelineState(m_light_blur_pso.Get());
            cmd->Dispatch((light_bx + 7) / 8, (light_by + 7) / 8, 1);

            D3D12_RESOURCE_BARRIER light_done{};
            light_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            light_done.UAV.pResource = m_light.Get();
            cmd->ResourceBarrier(1, &light_done);
        }

        cmd->SetPipelineState(m_match_pso.Get());
        cmd->Dispatch(1, 1, 1);
    }

    // [MARKED FIRST fix113 21.11.2026] RE4 test of fix112 (per interval, same place): the two eye passes cost 0.895 ms
    // without interface, 1.000 with the weapon HUD (3 anchored elements), 1.015 with 3 item beams, 1.02 - 1.03 with both,
    // 1.047 with 6 beams: about 0.05 ms per eye as soon as one tile is marked and little for every element more, and the
    // first pass leaving the marked tiles out (fix112's phase 51) changed nothing (+0.010 / -0.021, noise 0.05 - 0.08).
    // That is not work by the number of tiles: it is how long ONE marked group takes from its first tap to its last pixel,
    // which the few groups of the second pass cannot hide behind one another, after the first pass's thousands are done.
    // So the marked tiles' pass is recorded first and the eye image's pass right behind it, no barrier between (they
    // write different tiles: the first pass leaves the marked ones out): the few long groups run under the others. As
    // [BLOCKS 02.11.2026] did with match's one group. The same image.
    const bool marked_first = tile_pass && marked_pso != nullptr && first_skips && p.marked_first;

    // [SMALL GROUPS fix117 21.11.2026] RE4 tests of fix114 and fix115 (under the frame time, runs within 0.003 - 0.009 ms):
    // the marked tiles' pass costs 0.093 ms for the two eyes with about 190 tiles marked per eye, and nothing taken out of
    // its interface code changes that (line search -0.001, the panels' depth -0.006); a menu with 2200 tiles marked cost
    // less. What is left is the group itself: 256 threads in four waves that meet at four barriers. With groups of 8 x 8
    // pixels a group is one wave and waits for no other (the whole eye pass in such groups cost the same on 30.10.2026).
    // The marked tiles' text for them reads the bits of the 16 x 16 tile the group lies in (TILE_BITS_SHIFT); the same
    // image (the harness: the same bytes).
    uint32_t marked_w = tile_w;
    uint32_t marked_h = tile_h;

    if (tile_pass && marked_pso != nullptr && p.marked_small && p.marked_test == 2 && !p.marked_over && used == Params::Variant::Tile16x16) {
        if (auto* const small_pso = variant_pso(device, Params::Variant::Tile8x8, 9); small_pso != nullptr) {
            marked_pso = small_pso;
            marked_w = 8;
            marked_h = 8;
        }
    }

    if (marked_first) {
        cmd->SetPipelineState(marked_pso);
        cmd->Dispatch((out_w + marked_w - 1) / marked_w, (out_h + marked_h - 1) / marked_h, 1);
    }

    cmd->SetPipelineState(pso);
    cmd->Dispatch((out_w + tile_w - 1) / tile_w, (out_h + tile_h - 1) / tile_h, 1);

    // [TWO PASSES fix111] the tiles with interface again, over the first pass's pixels (the same groups: those without
    // leave after one read)
    if (tile_pass && marked_pso != nullptr && !marked_first) {
        // ([fix112] when the first pass left the marked tiles out the two passes write different pixels: no order needed)
        if (!first_skips) {
            D3D12_RESOURCE_BARRIER first_done{};
            first_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            first_done.UAV.pResource = out;
            cmd->ResourceBarrier(1, &first_done);
        }

        cmd->SetPipelineState(marked_pso);
        cmd->Dispatch((out_w + marked_w - 1) / marked_w, (out_h + marked_h - 1) / marked_h, 1);
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the tile probe: debug build only
    // [TILES PROBE fix114 21.11.2026] the tiles' bits of this eye copied for the count (see TilesCount), after the passes
    // that read them; the copy of 8 dispatches ago is counted first
    if (tile_pass && p.tiles_probe && !m_tiles_rb_failed) {
        if (m_tiles_rb == nullptr) {
            D3D12_HEAP_PROPERTIES rb_heap{};
            rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rb{};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = (UINT64)TILES_PROBE_SLOT * 2;
            rb.Height = 1;
            rb.DepthOrArraySize = 1;
            rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* rb_mapped = nullptr;

            if (FAILED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&m_tiles_rb))) ||
                FAILED(m_tiles_rb->Map(0, nullptr, &rb_mapped)) || rb_mapped == nullptr) {
                m_tiles_rb.Reset();
                m_tiles_rb_failed = true;
            } else {
                m_tiles_mapped = (const uint8_t*)rb_mapped;
            }
        }

        if (m_tiles_rb != nullptr) {
            auto& pr = m_tiles_probe[slot_index];
            const uint64_t n = m_tiles_dispatches[slot_index]++;

            if (pr.pending && n >= pr.at + 8) {
                TilesCount c{};
                const uint8_t* const base = m_tiles_mapped + (size_t)slot_index * TILES_PROBE_SLOT;
                uint32_t x0 = ~0u;
                uint32_t y0 = ~0u;
                uint32_t x1 = 0;
                uint32_t y1 = 0;

                for (uint32_t y = 0; y < pr.h; ++y) {
                    const uint32_t* const row = (const uint32_t*)(base + (size_t)y * pr.pitch);

                    for (uint32_t x = 0; x < pr.w / 2; ++x) {
                        const uint32_t depth = row[x * 2];
                        const uint32_t beam = row[x * 2 + 1];
                        ++c.tiles;

                        if (depth == 0) {
                            continue;
                        }

                        ++c.marked;
                        c.panel += depth & 1u;
                        c.anchors += (depth >> 1) != 0 ? 1u : 0u;
                        c.only_panel += depth == 1u ? 1u : 0u;
                        c.beam_other += beam != depth ? 1u : 0u;
                        x0 = (std::min)(x0, x);
                        y0 = (std::min)(y0, y);
                        x1 = (std::max)(x1, x);
                        y1 = (std::max)(y1, y);

                        for (uint32_t b = 0; b < 17; ++b) {
                            c.per_bit[b] += (depth >> b) & 1u;
                        }
                    }
                }

                c.box = c.marked != 0 ? (x1 - x0 + 1) * (y1 - y0 + 1) : 0u;
                c.valid = true;
                m_tiles_count[slot_index] = c;
                pr.pending = false;
            }

            const uint32_t probe_w = tiles_x * 2;
            const uint32_t probe_pitch = (probe_w * 4 + 255u) & ~255u;

            if (!pr.pending && (n & 63u) == 0 && (uint64_t)probe_pitch * tiles_y <= TILES_PROBE_SLOT) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = m_tiles.Get();
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                cmd->ResourceBarrier(1, &tb);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = m_tiles_rb.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = (UINT64)slot_index * TILES_PROBE_SLOT;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
                dst.PlacedFootprint.Footprint.Width = probe_w;
                dst.PlacedFootprint.Footprint.Height = tiles_y;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = probe_pitch;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = m_tiles.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                D3D12_BOX box{0, 0, 0, probe_w, tiles_y, 1};
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                std::swap(tb.Transition.StateBefore, tb.Transition.StateAfter);
                cmd->ResourceBarrier(1, &tb);
                pr.at = n;
                pr.w = probe_w;
                pr.h = tiles_y;
                pr.pitch = probe_pitch;
                pr.pending = true;
            }
        }
    }
#endif

    if (in_blocks) {
        if (metric) {
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = m_light.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd->ResourceBarrier(1, &to_copy);
            const uint32_t entry = (uint32_t)(m_metric_count[slot_index] % METRIC_RING);
            cmd->CopyBufferRegion(m_metric_rb.Get(), ((UINT64)entry * 2 + slot_index) * 16, m_light.Get(), ((UINT64)2 * LIGHT_SLOT + slot_index) * 16, 16);
            std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
            cmd->ResourceBarrier(1, &to_copy);
            ++m_metric_count[slot_index];
        }
    }

    // the out image is complete for its readers; the gain for the next frame is read by the next frame's pass; [LIGHT] the
    // blocks and the light are written again by the other eye's / the next frame's passes
    D3D12_RESOURCE_BARRIER uav_done[6]{};
    uav_done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[0].UAV.pResource = out;
    uav_done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[1].UAV.pResource = m_gain.Get();
    uav_done[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[2].UAV.pResource = m_light_sums.Get();
    uav_done[3].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[3].UAV.pResource = m_light.Get();
    uav_done[4].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[4].UAV.pResource = m_hist.Get();
    uav_done[5].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // [TILE PASS fix105] read by this eye's pass, written again by the next
    uav_done[5].UAV.pResource = m_tiles.Get();
    cmd->ResourceBarrier(tile_pass ? 6 : 5, uav_done);
    cmd->ResourceBarrier(n, back.data());

    if (measure || in_blocks) {
        m_parity[slot_index] ^= 1u;
    }

    m_fovea_drawn = fovea_ok;
    m_last_error = !p.fovea || fovea_ok ? "" : (fovea == nullptr ? "no fovea image" : "rectangle outside the eye image");
    return true;
}

// [VIEW DIFFERENCE 01.11.2026] light_metric's result of an eye slot from METRIC_LAG dispatches ago (the GPU has finished
// it by then); false while there is none yet
bool EyeCompose::view_metric(uint32_t slot, float out[4]) const {
    slot = (std::min)(slot, 1u);

    if (m_metric_mapped == nullptr || m_metric_count[slot] < METRIC_LAG) {
        return false;
    }

    const uint64_t entry = (m_metric_count[slot] - METRIC_LAG) % METRIC_RING;
    std::memcpy(out, m_metric_mapped + (entry * 2 + slot) * 4, 4 * sizeof(float));
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

void EyeCompose::reset() {
    m_pso.Reset();
    m_measure_pso.Reset();
    m_gain_pso.Reset();
    m_blocks_pso.Reset();
    m_match_pso.Reset();
    m_light_blur_pso.Reset();
    m_tiles_pso.Reset(); // [TILE PASS fix105]
    m_over_format = DXGI_FORMAT_UNKNOWN; // [OVER fix121] (asked again on the new device)
    m_over_ok = false;

    for (auto& kind : m_bits_pso) {
        for (auto& b : kind) {
            b.Reset();
        }
    }

    for (auto& kind : m_bits_failed) {
        kind.fill(false);
    }
    m_tiles.Reset();

    for (auto& v : m_variant_pso) {
        v.Reset();
    }

    m_variant_failed.fill(false);
    m_root_sig.Reset();
    m_heap.Reset();
    m_cb.Reset();
    m_gain.Reset();
    m_partials.Reset();
    m_light_sums.Reset();
    m_light.Reset();
    m_hist.Reset();

    if (m_metric_rb != nullptr && m_metric_mapped != nullptr) {
        m_metric_rb->Unmap(0, nullptr);
    }

    m_metric_rb.Reset();
    m_metric_mapped = nullptr;
    m_metric_count[0] = 0;
    m_metric_count[1] = 0;

    // [TILES PROBE fix114]
    if (m_tiles_rb != nullptr && m_tiles_mapped != nullptr) {
        m_tiles_rb->Unmap(0, nullptr);
    }

    m_tiles_rb.Reset();
    m_tiles_mapped = nullptr;
    m_tiles_rb_failed = false;

    for (uint32_t e = 0; e < 2; ++e) {
        m_tiles_probe[e] = {};
        m_tiles_count[e] = {};
        m_tiles_dispatches[e] = 0;
    }

    m_parity[0] = 0;
    m_parity[1] = 0;
    m_cb_mapped = nullptr;
    m_ring = 0;
    m_fovea_drawn = false;
    m_failed = false;
    m_last_error = "";
}

// ---- [FOVEA LINEAR 05.11.2026] see FoveaComposite.hpp ----------------------------------------------------------------
namespace {
constexpr char FOVEA_LINEAR_SHADER[] = R"(
Texture2D<float4>   g_in  : register(t0);
RWTexture2D<float4> g_out : register(u0);

cbuffer Params : register(b0) {
    uint2 g_size;    // threads: the part written
    uint  g_flags;   // 1 = decode SRGB -> linear, 2 = encode linear -> SRGB, 4 = sharpen (with 2), 8 = (with 2) the input
                     // is encoded already (RCAS only)
    float g_sharp;   // RCAS amount 0..1
    uint2 g_out_off; // [FSR DIRECT 06.11.2026] the part's corner in g_out
    int2  g_shift;   // input pixel = clamp(thread + g_shift, g_lo, g_hi)
    int2  g_lo;
    int2  g_hi;
    float2 g_scale;   // [FSR MODEL 09.11.2026] flag 16: input pixels per output pixel (1..3)
    float2 g_src;     // ... where the part's first output pixel starts in the input
};

float3 to_linear(float3 c) {
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// the encoded result at input pixel q (clamped to the input's part)
float3 enc(int2 q) {
    q = clamp(q, g_lo, g_hi);
    const float3 c = g_in.Load(int3(q, 0)).rgb;
    return (g_flags & 8) != 0 ? saturate(c) : to_srgb(c);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_size)) {
        return;
    }

    const int2 q = clamp(int2(id.xy) + g_shift, g_lo, g_hi);
    const uint2 o = id.xy + g_out_off;

    if ((g_flags & 16) != 0) {
        // [FSR MODEL 09.11.2026] area average of the input over this output pixel's footprint (at most 4 x 4 pixels)
        const float2 a = g_src + float2(id.xy) * g_scale;
        const float2 b = a + g_scale;
        const int2 i0 = int2(floor(a));
        float3 sum = 0.0;
        float wsum = 0.0;

        [loop] for (int y = 0; y < 4; ++y) {
            const float py = (float)(i0.y + y);
            const float wy = min(b.y, py + 1.0) - max(a.y, py);

            if (wy <= 0.0) {
                break;
            }

            [loop] for (int x = 0; x < 4; ++x) {
                const float px = (float)(i0.x + x);
                const float wx = min(b.x, px + 1.0) - max(a.x, px);

                if (wx <= 0.0) {
                    break;
                }

                const int2 p = clamp(i0 + int2(x, y), g_lo, g_hi);
                sum += (wx * wy) * g_in.Load(int3(p, 0)).rgb;
                wsum += wx * wy;
            }
        }

        g_out[o] = float4(sum / max(wsum, 1e-6), 1.0);
        return;
    }

    if ((g_flags & 2) == 0) {
        const float4 c = g_in.Load(int3(q, 0));
        g_out[o] = float4((g_flags & 1) != 0 ? to_linear(c.rgb) : c.rgb, 1.0);
        return;
    }

    float3 e = enc(q);

    if ((g_flags & 4) != 0) {
        // RCAS as in the fovea TAA's resolve: the strongest negative lobe that keeps the cross around the pixel within
        // [0, 1] (no clipping, no halos)
        const float3 b = enc(q + int2(0, -1));
        const float3 d = enc(q + int2(-1, 0));
        const float3 f = enc(q + int2(1, 0));
        const float3 h = enc(q + int2(0, 1));
        const float3 mn4 = min(min(b, d), min(f, h));
        const float3 mx4 = max(max(b, d), max(f, h));
        const float3 hit_min = min(mn4, e) / (4.0 * mx4 + 1e-5);
        const float3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
        const float3 lobe3 = max(-hit_min, hit_max);
        const float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * g_sharp;
        e = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    }

    g_out[o] = float4(saturate(e), 1.0);
}
)";
} // namespace

bool FoveaLinear::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_cached(FOVEA_LINEAR_SHADER, sizeof(FOVEA_LINEAR_SHADER) - 1, "FoveaLinear", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader,
            &errors))) {
        REVR_LOG(spdlog::error("[FoveaLinear] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        m_last_error = "shader compile failed";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 16; // [FSR DIRECT 06.11.2026] + the part (out offset, shift, clamp box); [FSR MODEL] + scale, start
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_root_sig)))) {
        REVR_LOG(spdlog::error("[FoveaLinear] root signature failed"));
        m_last_error = "root signature failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[FoveaLinear] CreateComputePipelineState failed"));
        m_last_error = "pipeline state failed";
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[FoveaLinear] CreateDescriptorHeap failed"));
        m_last_error = "descriptor heap failed";
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[FoveaLinear] ready"));
    return true;
}

bool FoveaLinear::run(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out,
    uint32_t flags, float sharpen, const Region* part) {
    if (device == nullptr || cmd == nullptr || in == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    const auto idesc = in->GetDesc();
    const auto odesc = out->GetDesc();
    const bool depth = (flags & FLAG_DEPTH) != 0;
    flags &= ~FLAG_DEPTH;
    const auto in_format = depth ? depth_srv_format_for(idesc.Format) : srv_format_for(idesc.Format);
    const auto out_format = uav_format_for(odesc.Format);

    if (in_format == DXGI_FORMAT_UNKNOWN || out_format == DXGI_FORMAT_UNKNOWN) {
        m_last_error = "unsupported format";
        return false;
    }

    // [FSR DIRECT 06.11.2026] the part written and where it reads (whole images: the same size, 1:1)
    Region r{};

    if (part != nullptr) {
        r = *part;
        r.hi_x = r.hi_x < 0 ? (int32_t)idesc.Width - 1 : r.hi_x;
        r.hi_y = r.hi_y < 0 ? (int32_t)idesc.Height - 1 : r.hi_y;

        if (r.w == 0 || r.h == 0 || (uint64_t)r.out_x + r.w > odesc.Width || (uint64_t)r.out_y + r.h > odesc.Height || r.lo_x < 0 || r.lo_y < 0 ||
            r.lo_x > r.hi_x || r.lo_y > r.hi_y || r.hi_x >= (int32_t)idesc.Width || r.hi_y >= (int32_t)idesc.Height) {
            m_last_error = "part outside the images";
            return false;
        }
    } else {
        if (odesc.Width != idesc.Width || odesc.Height != idesc.Height) {
            m_last_error = "unsupported format or size";
            return false;
        }

        r.w = (uint32_t)odesc.Width;
        r.h = odesc.Height;
        r.hi_x = (int32_t)idesc.Width - 1;
        r.hi_y = (int32_t)idesc.Height - 1;
    }

    // decode: an SRGB view decodes by itself (the shader then copies)
    if ((flags & 1) != 0 && is_srgb(in_format)) {
        flags &= ~1u;
    }

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = in_format;
    device->CreateShaderResourceView(in, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = out_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(out, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + m_descriptor_size});

    const auto in_read = compute_read_state(in_state);
    barrier(cmd, in, in_state, in_read);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    uint32_t constants[16]{};
    constants[0] = r.w;
    constants[1] = r.h;
    constants[2] = flags;
    std::memcpy(&constants[3], &sharpen, sizeof(float));
    constants[4] = r.out_x;
    constants[5] = r.out_y;
    constants[6] = (uint32_t)r.shift_x;
    constants[7] = (uint32_t)r.shift_y;
    constants[8] = (uint32_t)r.lo_x;
    constants[9] = (uint32_t)r.lo_y;
    constants[10] = (uint32_t)r.hi_x;
    constants[11] = (uint32_t)r.hi_y;
    std::memcpy(&constants[12], &r.scale_x, sizeof(float));
    std::memcpy(&constants[13], &r.scale_y, sizeof(float));
    std::memcpy(&constants[14], &r.src_x, sizeof(float));
    std::memcpy(&constants[15], &r.src_y, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 16, constants, 0);
    cmd->Dispatch((r.w + 7) / 8, (r.h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_done{};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = out;
    cmd->ResourceBarrier(1, &uav_done);

    barrier(cmd, in, in_read, in_state);
    m_last_error = "";
    return true;
}

bool FoveaLinear::decode(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out) {
    return run(device, cmd, in, in_state, out, 1u, 0.0f);
}

bool FoveaLinear::encode(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, float sharpen) {
    const float s = std::clamp(sharpen, 0.0f, 1.0f);
    return run(device, cmd, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out, 2u | (s > 0.0f ? 4u : 0u), s);
}

bool FoveaLinear::decode_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state,
    ID3D12Resource* out, const Region& r, bool decode_curve) {
    return run(device, cmd, in, in_state, out, decode_curve ? 1u : 0u, 0.0f, &r);
}

bool FoveaLinear::copy_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state,
    ID3D12Resource* out, const Region& r, bool depth) {
    return run(device, cmd, in, in_state, out, depth ? FLAG_DEPTH : 0u, 0.0f, &r);
}

bool FoveaLinear::encode_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, float sharpen,
    const Region& r, bool curve) {
    const float s = std::clamp(sharpen, 0.0f, 1.0f);
    return run(device, cmd, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out, 2u | (s > 0.0f ? 4u : 0u) | (curve ? 0u : 8u), s, &r);
}

bool FoveaLinear::downsample_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, const Region& r) {
    if (!(r.scale_x >= 1.0f && r.scale_x <= 3.0f && r.scale_y >= 1.0f && r.scale_y <= 3.0f)) {
        m_last_error = "scale outside 1..3";
        return false;
    }

    return run(device, cmd, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out, 16u, 0.0f, &r);
}

void FoveaLinear::reset() {
    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

} // namespace d3d12
