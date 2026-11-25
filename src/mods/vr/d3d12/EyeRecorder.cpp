#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the video of the eyes: debug build only (the header has the release's empty stand-in)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>

#include <windows.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <spdlog/spdlog.h>

#include "EyeRecorder.hpp"

// GUIDs of Media Foundation (static library); the DLLs themselves are loaded at run time (see MfApi)
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace d3d12 {

namespace {
constexpr char NV12_SHADER[] = R"(
Texture2D<float4>   g_left  : register(t0);
Texture2D<float4>   g_right : register(t1);
RWByteAddressBuffer g_out   : register(u0);
SamplerState        g_lin   : register(s0);

cbuffer Params : register(b0) {
    uint g_w;        // output: both eyes side by side
    uint g_h;
    uint g_y_bytes;  // start of the interleaved U/V plane
    uint g_pad;
};

// average of about the 4x4 source texels under one output pixel (4 bilinear taps)
float3 fetch(uint x, uint y) {
    const uint half_w = g_w / 2;
    const bool right = x >= half_w;
    const float2 inv = 1.0 / float2(half_w, g_h);
    const float2 uv = (float2(right ? x - half_w : x, y) + 0.5) * inv;
    const float2 d = 0.25 * inv;
    float3 c;

    if (right) {
        c = g_right.SampleLevel(g_lin, uv + float2(-d.x, -d.y), 0).rgb + g_right.SampleLevel(g_lin, uv + float2(d.x, -d.y), 0).rgb +
            g_right.SampleLevel(g_lin, uv + float2(-d.x, d.y), 0).rgb + g_right.SampleLevel(g_lin, uv + float2(d.x, d.y), 0).rgb;
    } else {
        c = g_left.SampleLevel(g_lin, uv + float2(-d.x, -d.y), 0).rgb + g_left.SampleLevel(g_lin, uv + float2(d.x, -d.y), 0).rgb +
            g_left.SampleLevel(g_lin, uv + float2(-d.x, d.y), 0).rgb + g_left.SampleLevel(g_lin, uv + float2(d.x, d.y), 0).rgb;
    }

    return saturate(c * 0.25);
}

// BT.709, video range
float luma(float3 c) { return 16.0 + 219.0 * dot(c, float3(0.2126, 0.7152, 0.0722)); }
float cb(float3 c) { return 128.0 + 224.0 * dot(c, float3(-0.1146, -0.3854, 0.5)); }
float cr(float3 c) { return 128.0 + 224.0 * dot(c, float3(0.5, -0.4542, -0.0458)); }

uint pack4(float4 v) {
    const uint4 b = (uint4)clamp(round(v), 0.0, 255.0);
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}

// one thread: 4 x 2 output pixels -> two words of Y, one word of U/V
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint x0 = id.x * 4;
    const uint y0 = id.y * 2;

    if (x0 >= g_w || y0 >= g_h) {
        return;
    }

    float3 c[2][4];

    [unroll] for (uint r = 0; r < 2; ++r) {
        [unroll] for (uint k = 0; k < 4; ++k) {
            c[r][k] = fetch(x0 + k, y0 + r);
        }
    }

    [unroll] for (uint r2 = 0; r2 < 2; ++r2) {
        g_out.Store((y0 + r2) * g_w + x0, pack4(float4(luma(c[r2][0]), luma(c[r2][1]), luma(c[r2][2]), luma(c[r2][3]))));
    }

    const float3 a = (c[0][0] + c[0][1] + c[1][0] + c[1][1]) * 0.25;
    const float3 b = (c[0][2] + c[0][3] + c[1][2] + c[1][3]) * 0.25;
    g_out.Store(g_y_bytes + (y0 / 2) * g_w + x0, pack4(float4(cb(a), cr(a), cb(b), cr(b))));
}
)";

DXGI_FORMAT srv_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
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

D3D12_RESOURCE_STATES compute_read_state(D3D12_RESOURCE_STATES resting) {
    if (resting == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || resting == D3D12_RESOURCE_STATE_COMMON) {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    return (D3D12_RESOURCE_STATES)(resting | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// Media Foundation, loaded when a video is written (not every Windows edition has it)
struct MfApi {
    using Startup = HRESULT(WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT(WINAPI*)();
    using CreateAttributes = HRESULT(WINAPI*)(IMFAttributes**, UINT32);
    using CreateMediaType = HRESULT(WINAPI*)(IMFMediaType**);
    using CreateMemoryBuffer = HRESULT(WINAPI*)(DWORD, IMFMediaBuffer**);
    using CreateSample = HRESULT(WINAPI*)(IMFSample**);
    using CreateSinkWriterFromURL = HRESULT(WINAPI*)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);

    HMODULE plat{nullptr};
    HMODULE rw{nullptr};
    Startup startup{nullptr};
    Shutdown shutdown{nullptr};
    CreateAttributes create_attributes{nullptr};
    CreateMediaType create_media_type{nullptr};
    CreateMemoryBuffer create_memory_buffer{nullptr};
    CreateSample create_sample{nullptr};
    CreateSinkWriterFromURL create_sink_writer{nullptr};

    bool load() {
        plat = LoadLibraryW(L"mfplat.dll");
        rw = LoadLibraryW(L"mfreadwrite.dll");

        if (plat == nullptr || rw == nullptr) {
            return false;
        }

        startup = (Startup)GetProcAddress(plat, "MFStartup");
        shutdown = (Shutdown)GetProcAddress(plat, "MFShutdown");
        create_attributes = (CreateAttributes)GetProcAddress(plat, "MFCreateAttributes");
        create_media_type = (CreateMediaType)GetProcAddress(plat, "MFCreateMediaType");
        create_memory_buffer = (CreateMemoryBuffer)GetProcAddress(plat, "MFCreateMemoryBuffer");
        create_sample = (CreateSample)GetProcAddress(plat, "MFCreateSample");
        create_sink_writer = (CreateSinkWriterFromURL)GetProcAddress(rw, "MFCreateSinkWriterFromURL");
        return startup != nullptr && shutdown != nullptr && create_attributes != nullptr && create_media_type != nullptr &&
               create_memory_buffer != nullptr && create_sample != nullptr && create_sink_writer != nullptr;
    }

    ~MfApi() {
        if (rw != nullptr) {
            FreeLibrary(rw);
        }

        if (plat != nullptr) {
            FreeLibrary(plat);
        }
    }
};

// NV12 frame at half the size (2x2 averages), for an encoder that refuses the full size
std::vector<uint8_t> half_nv12(const std::vector<uint8_t>& src, uint32_t w, uint32_t h) {
    const uint32_t hw = w / 2;
    const uint32_t hh = h / 2;
    std::vector<uint8_t> out((size_t)hw * hh * 3 / 2);

    for (uint32_t y = 0; y < hh; ++y) {
        const uint8_t* r0 = &src[(size_t)(2 * y) * w];
        const uint8_t* r1 = r0 + w;

        for (uint32_t x = 0; x < hw; ++x) {
            out[(size_t)y * hw + x] = (uint8_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) / 4);
        }
    }

    const uint8_t* uv = &src[(size_t)w * h];
    uint8_t* ouv = &out[(size_t)hw * hh];

    for (uint32_t y = 0; y < hh / 2; ++y) {
        const uint8_t* r0 = uv + (size_t)(2 * y) * w;
        const uint8_t* r1 = r0 + w;

        for (uint32_t x = 0; x < hw / 2; ++x) {
            for (uint32_t c = 0; c < 2; ++c) {
                const uint32_t i = 4 * x + c; // pairs 2x and 2x+1 of U/V
                ouv[(size_t)y * hw + 2 * x + c] = (uint8_t)((r0[i] + r0[i + 2] + r1[i] + r1[i + 2] + 2) / 4);
            }
        }
    }

    return out;
}

template <typename T> void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}
} // namespace

EyeRecorder::~EyeRecorder() {
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool EyeRecorder::start(double seconds, const std::filesystem::path& dir) {
    if (busy()) {
        return false;
    }

    if (m_worker.joinable()) {
        m_worker.join(); // the last encode has finished (the phase went back to Idle)
    }

    m_seconds = std::clamp(seconds, 0.5, 10.0);
    m_dir = dir;
    m_started = false;
    m_dropped = 0;
    m_measure = false;
    m_frames.clear();
    m_frames.reserve(MAX_FRAMES);
    m_phase = Phase::Recording;
    set_status(std::format("recording starts ({:.1f} s)", m_seconds));
    return true;
}

// [LIGHT FLICKER TEST 15.11.2026] see the header
bool EyeRecorder::start_measure(double seconds) {
    if (busy()) {
        return false;
    }

    m_seconds = std::clamp(seconds, 1.0, 120.0);
    m_started = false;
    m_dropped = 0;
    m_measure = true;
    m_frames.clear();
    m_luma_count = 0;

    for (int i = 0; i < 3; ++i) {
        m_luma[i].assign((size_t)MEASURE_W * MEASURE_H, 0);
        m_luma_tag[i] = -1;
    }

    for (auto& t : m_tags) {
        t = TagSums{};
    }

    {
        std::scoped_lock _{m_status_mutex};
        m_measure_rows.clear();
        m_measure_ready = false;
    }

    m_phase = Phase::Recording;
    set_status(std::format("flicker measurement starts ({:.0f} s)", m_seconds));
    return true;
}

bool EyeRecorder::take_measure(std::vector<MeasureRow>& rows) {
    std::scoped_lock _{m_status_mutex};

    if (!m_measure_ready) {
        return false;
    }

    rows = std::move(m_measure_rows);
    m_measure_rows.clear();
    m_measure_ready = false;
    return true;
}

// one frame's Y plane (WIDTH x HEIGHT bytes at the start of the NV12 data); mask: the fovea rectangles of the frame
void EyeRecorder::measure_frame(const uint8_t* nv12, int tag, const MeasureMask& mask) {
    const uint32_t idx = m_luma_count % 3;
    auto& cur = m_luma[idx];
    cur.resize((size_t)MEASURE_W * MEASURE_H);

    for (uint32_t y = 0; y < MEASURE_H; ++y) {
        const uint8_t* r0 = nv12 + (size_t)(2 * y) * WIDTH;
        const uint8_t* r1 = r0 + WIDTH;
        uint8_t* o = cur.data() + (size_t)y * MEASURE_W;

        for (uint32_t x = 0; x < MEASURE_W; ++x) {
            o[x] = (uint8_t)(((uint32_t)r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) / 4);
        }
    }

    m_luma_tag[idx] = tag;
    ++m_luma_count;

    // [LIGHTS fix86 18.11.2026] did a fovea rectangle move since the frame before (every frame, measured or not)
    uint8_t moved = 0;

    if (m_have_prev_mask) {
        for (int e = 0; e < 2; ++e) {
            if (mask.valid[e] != m_prev_mask.valid[e] || mask.x0[e] != m_prev_mask.x0[e] || mask.y0[e] != m_prev_mask.y0[e]) {
                moved |= (uint8_t)(1u << e);
            }
        }
    }

    m_prev_mask = mask;
    m_have_prev_mask = true;

    if (m_luma_count < 3 || tag < 0 || tag >= MAX_TAGS) {
        return;
    }

    const uint32_t i1 = (m_luma_count - 2) % 3;
    const uint32_t i2 = (m_luma_count - 3) % 3;

    if (m_luma_tag[i1] != tag || m_luma_tag[i2] != tag) {
        return;
    }

    const uint8_t* p1 = m_luma[i1].data();
    const uint8_t* p2 = m_luma[i2].data();
    auto& sums = m_tags[tag];

    if (sums.cells.empty()) {
        sums.cells.assign((size_t)CELLS_X * CELLS_Y, 0);
    }

    if (sums.peri_cells.empty()) {
        sums.peri_cells.assign((size_t)CELLS_X * CELLS_Y, 0);
    }

    uint64_t flips = 0;
    uint64_t peri = 0;

    for (uint32_t y = 0; y < MEASURE_H; ++y) {
        const size_t row = (size_t)y * MEASURE_W;
        uint32_t* cells = sums.cells.data() + (size_t)(y / CELL) * CELLS_X;
        uint32_t* pcells = sums.peri_cells.data() + (size_t)(y / CELL) * CELLS_X;
        int32_t ex0[2]{-1, -1};
        int32_t ex1[2]{-1, -1};

        // [LIGHT FLICKER TEST 15.11.2026 evening] the part of this row inside each eye's fovea rectangle
        for (int e = 0; e < 2; ++e) {
            if (mask.valid[e] && (int32_t)y >= mask.y0[e] && (int32_t)y < mask.y1[e]) {
                ex0[e] = mask.x0[e];
                ex1[e] = mask.x1[e];
            }
        }

        for (uint32_t x = 0; x < MEASURE_W; ++x) {
            const int d0 = (int)p1[row + x] - (int)p2[row + x];
            const int d1 = (int)cur[row + x] - (int)p1[row + x];

            if ((d0 >= FLICKER_STEP && d1 <= -FLICKER_STEP) || (d0 <= -FLICKER_STEP && d1 >= FLICKER_STEP)) {
                ++flips;
                ++cells[x / CELL];
                const int32_t xi = (int32_t)x;

                if (!((xi >= ex0[0] && xi < ex1[0]) || (xi >= ex0[1] && xi < ex1[1]))) {
                    ++peri;
                    ++pcells[x / CELL];
                }
            }
        }
    }

    ++sums.frames;
    sums.flips += flips;
    sums.peri_flips += peri;

    // [LIGHTS fix86 18.11.2026] this frame's cells, for the light's course at the hot spot
    if (sums.moved.size() < SERIES_FRAMES) {
        const size_t at = sums.cell_means.size();
        sums.cell_means.resize(at + (size_t)CELLS_X * CELLS_Y);
        std::vector<uint32_t> sum((size_t)CELLS_X * CELLS_Y, 0);
        std::vector<uint32_t> count((size_t)CELLS_X * CELLS_Y, 0);

        for (uint32_t y = 0; y < MEASURE_H; ++y) {
            const uint8_t* row = cur.data() + (size_t)y * MEASURE_W;
            const size_t cell_row = (size_t)(y / CELL) * CELLS_X;

            for (uint32_t x = 0; x < MEASURE_W; ++x) {
                sum[cell_row + x / CELL] += row[x];
                ++count[cell_row + x / CELL];
            }
        }

        for (size_t i = 0; i < sum.size(); ++i) {
            sums.cell_means[at + i] = (uint8_t)(count[i] > 0 ? (sum[i] + count[i] / 2) / count[i] : 0);
        }

        sums.moved.push_back(moved);
    }
}

void EyeRecorder::finish_measure() {
    std::vector<MeasureRow> rows{};

    // [LIGHTS fix86 18.11.2026] one cell for every tag's course of the light: the periphery's cell with the most flips per
    // frame in any tag
    size_t series_cell = 0;
    double series_best = -1.0;

    for (int t = 0; t < MAX_TAGS; ++t) {
        const auto& s = m_tags[t];

        for (size_t i = 0; s.frames > 0 && i < s.peri_cells.size(); ++i) {
            const double v = (double)s.peri_cells[i] / (double)s.frames;

            if (v > series_best) {
                series_best = v;
                series_cell = i;
            }
        }
    }

    for (int t = 0; t < MAX_TAGS; ++t) {
        const auto& s = m_tags[t];

        if (s.frames == 0) {
            continue;
        }

        MeasureRow r{};
        r.series_cell_x = (uint32_t)(series_cell % CELLS_X);
        r.series_cell_y = (uint32_t)(series_cell / CELLS_X);
        r.moved = s.moved;
        r.series.reserve(s.moved.size());

        for (size_t f = 0; f < s.moved.size() && (f + 1) * (size_t)CELLS_X * CELLS_Y <= s.cell_means.size(); ++f) {
            r.series.push_back(s.cell_means[f * (size_t)CELLS_X * CELLS_Y + series_cell]);
        }
        r.tag = t;
        r.frames = s.frames;
        r.flips = s.flips;

        for (size_t i = 0; i < s.cells.size(); ++i) {
            if (s.cells[i] > r.cell_flips) {
                r.cell_flips = s.cells[i];
                r.cell_x = (uint32_t)(i % CELLS_X);
                r.cell_y = (uint32_t)(i / CELLS_X);
            }
        }

        // [LIGHT FLICKER TEST 15.11.2026 evening] the periphery's
        r.peri_flips = s.peri_flips;

        for (size_t i = 0; i < s.peri_cells.size(); ++i) {
            if (s.peri_cells[i] > r.peri_cell_flips) {
                r.peri_cell_flips = s.peri_cells[i];
                r.peri_cell_x = (uint32_t)(i % CELLS_X);
                r.peri_cell_y = (uint32_t)(i / CELLS_X);
            }
        }

        rows.push_back(r);
    }

    {
        std::scoped_lock _{m_status_mutex};
        m_measure_rows = std::move(rows);
        m_measure_ready = true;
    }

    for (auto& l : m_luma) {
        l.clear();
        l.shrink_to_fit();
    }

    for (auto& t : m_tags) {
        t = TagSums{};
    }

    m_have_prev_mask = false;
    m_measure = false;
}

std::string EyeRecorder::status() const {
    std::scoped_lock _{m_status_mutex};
    return m_status;
}

void EyeRecorder::set_status(const std::string& s) {
    std::scoped_lock _{m_status_mutex};
    m_status = s;
}

bool EyeRecorder::setup(ID3D12Device* device) {
    if (m_pso != nullptr) {
        return true;
    }

    if (m_setup_failed || device == nullptr) {
        return false;
    }

    m_setup_failed = true;

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3DCompile(NV12_SHADER, sizeof(NV12_SHADER) - 1, "EyeRecorder", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors))) {
        REVR_LOG(spdlog::error("[EyeRecorder] shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
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
        REVR_LOG(spdlog::error("[EyeRecorder] root signature failed"));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        REVR_LOG(spdlog::error("[EyeRecorder] CreateComputePipelineState failed"));
        m_root_sig.Reset();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING * 3;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[EyeRecorder] descriptor heap failed"));
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = NV12_BYTES;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES rb{};
    rb.Type = D3D12_HEAP_TYPE_READBACK;

    for (auto& s : m_slots) {
        auto uav_desc = buf;
        uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (!s.commands.setup(L"Eye recorder") ||
            FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &uav_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.nv12))) ||
            FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback)))) {
            REVR_LOG(spdlog::error("[EyeRecorder] buffers / command list failed"));
            reset();
            m_setup_failed = true;
            return false;
        }

        s.pending = false;
    }

    m_setup_failed = false;
    REVR_LOG(spdlog::info("[EyeRecorder] ready ({}x{}, {} buffers)", WIDTH, HEIGHT, RING));
    return true;
}

// finished GPU copies -> frames; wait = also the ones still running
void EyeRecorder::collect(bool wait) {
    for (uint32_t n = 0; n < RING; ++n) {
        // oldest first, so the frames stay in order
        auto& s = m_slots[(m_next_slot + n) % RING];

        if (!s.pending || s.commands.fence == nullptr) {
            continue;
        }

        if (s.commands.fence->GetCompletedValue() < s.commands.fence_value) {
            if (!wait) {
                if (m_measure) {
                    break; // [LIGHT FLICKER TEST] in order: the later ones wait for this one
                }

                continue;
            }

            s.commands.wait(INFINITE);
        }

        s.pending = false;
        void* p = nullptr;
        // (the measurement reads the luma plane only)
        const D3D12_RANGE range{0, (SIZE_T)(m_measure ? (uint64_t)WIDTH * HEIGHT : NV12_BYTES)};

        if (SUCCEEDED(s.readback->Map(0, &range, &p)) && p != nullptr) {
            if (m_measure) {
                measure_frame((const uint8_t*)p, s.tag, s.mask); // [LIGHT FLICKER TEST 15.11.2026]
            } else if (m_frames.size() < MAX_FRAMES) {
                Frame f{};
                f.nv12.resize((size_t)NV12_BYTES);
                std::memcpy(f.nv12.data(), p, (size_t)NV12_BYTES);
                f.time_100ns = s.time_100ns;
                m_frames.push_back(std::move(f));
            }

            const D3D12_RANGE nothing{0, 0};
            s.readback->Unmap(0, &nothing);
        } else {
            ++m_dropped;
        }
    }

}

void EyeRecorder::on_frame(ID3D12Device* device, ID3D12Resource* left, ID3D12Resource* right, D3D12_RESOURCE_STATES state) {
    const auto phase = m_phase.load();

    if (phase == Phase::Idle || phase == Phase::Encoding) {
        return;
    }

    if (!setup(device)) {
        set_status("video: could not set up the recording (see the REFramework log)");
        m_frames.clear();

        if (m_measure) {
            finish_measure(); // an empty result: the caller stops waiting
        }

        m_phase = Phase::Idle;
        return;
    }

    collect(false);

    const auto now = std::chrono::steady_clock::now();

    if (phase == Phase::Recording) {
        if (!m_started) {
            m_t0 = now;
            m_started = true;
        }

        const double elapsed = std::chrono::duration<double>(now - m_t0).count();
        uint32_t pending = 0;

        for (const auto& s : m_slots) {
            pending += s.pending ? 1 : 0;
        }

        if (elapsed >= m_seconds || (!m_measure && m_frames.size() + pending >= MAX_FRAMES)) {
            m_phase = Phase::Draining;
        } else if (left != nullptr && right != nullptr) {
            auto& s = m_slots[m_next_slot];

            if (s.pending) {
                ++m_dropped; // the GPU has not finished this buffer yet: rather skip a frame than wait
            } else {
                s.commands.wait(INFINITE); // finished long ago: only resets the command list
                auto* cmd = s.commands.cmd_list.Get();

                auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
                auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
                cpu.ptr += (SIZE_T)(m_next_slot * 3) * m_descriptor_size;
                gpu.ptr += (UINT64)(m_next_slot * 3) * m_descriptor_size;

                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                srv.Format = srv_format(left->GetDesc().Format);
                device->CreateShaderResourceView(left, &srv, cpu);
                srv.Format = srv_format(right->GetDesc().Format);
                device->CreateShaderResourceView(right, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + m_descriptor_size});

                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_R32_TYPELESS;
                uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                uav.Buffer.NumElements = (UINT)(NV12_BYTES / 4);
                uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                device->CreateUnorderedAccessView(s.nv12.Get(), nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * (SIZE_T)m_descriptor_size});

                const auto read = compute_read_state(state);
                barrier(cmd, left, state, read);
                barrier(cmd, right, state, read);
                barrier(cmd, s.nv12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                const uint32_t constants[4]{WIDTH, HEIGHT, WIDTH * HEIGHT, 0};
                ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetComputeRootSignature(m_root_sig.Get());
                cmd->SetPipelineState(m_pso.Get());
                cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
                cmd->SetComputeRootDescriptorTable(1, gpu);
                cmd->Dispatch((WIDTH / 4 + 7) / 8, (HEIGHT / 2 + 7) / 8, 1);

                barrier(cmd, s.nv12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyBufferRegion(s.readback.Get(), 0, s.nv12.Get(), 0, NV12_BYTES);
                barrier(cmd, s.nv12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                barrier(cmd, right, read, state);
                barrier(cmd, left, read, state);

                s.commands.has_commands = true;
                s.commands.execute();
                s.pending = true;
                s.time_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_t0).count() / 100;
                s.tag = m_measure_tag.load(std::memory_order_relaxed); // [LIGHT FLICKER TEST 15.11.2026]
                s.mask = m_measure_mask;                               // [LIGHT FLICKER TEST 15.11.2026 evening]
                m_next_slot = (m_next_slot + 1) % RING;
            }

            if (m_measure) {
                set_status(std::format("measuring the flicker: {:.0f} / {:.0f} s", elapsed, m_seconds));
            } else {
                set_status(std::format("recording: {:.1f} / {:.1f} s ({} frames)", elapsed, m_seconds, m_frames.size()));
            }
        }
    }

    if (m_phase.load() == Phase::Draining) {
        collect(true);

        // [LIGHT FLICKER TEST 15.11.2026] the measurement ends here: no video
        if (m_measure) {
            finish_measure();
            set_status("flicker measurement finished");
            m_phase = Phase::Idle;
            return;
        }

        // a frame may have come back after a later one (collect without waiting skips the unfinished ones)
        std::sort(m_frames.begin(), m_frames.end(), [](const Frame& a, const Frame& b) { return a.time_100ns < b.time_100ns; });

        const double recorded = m_frames.size() > 1 ? (double)(m_frames.back().time_100ns - m_frames.front().time_100ns) / 1e7 : 0.0;

        if (m_frames.empty()) {
            set_status("video: no frame was recorded");
            m_phase = Phase::Idle;
            return;
        }

        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        const auto path = m_dir / std::format("video_{:04}{:02}{:02}_{:02}{:02}{:02}.mp4", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                                               tm.tm_min, tm.tm_sec);

        if (m_worker.joinable()) {
            m_worker.join();
        }

        m_phase = Phase::Encoding;
        set_status(std::format("writing the video ({} frames)...", m_frames.size()));
        m_worker = std::thread(&EyeRecorder::encode, this, std::move(m_frames), path, recorded, m_dropped);
        m_frames = {};
    }
}

void EyeRecorder::encode(std::vector<Frame> frames, std::filesystem::path path, double seconds_recorded, uint32_t dropped) {
    const auto finish = [&](const std::string& s) {
        REVR_LOG(spdlog::info("[EyeRecorder] {}", s));
        set_status(s);
        frames.clear();
        frames.shrink_to_fit();
        m_phase = Phase::Idle;
    };

    std::error_code ec{};
    std::filesystem::create_directories(path.parent_path(), ec);

    MfApi mf{};

    if (!mf.load()) {
        finish("video: Media Foundation is not available on this Windows (Media Feature Pack missing?)");
        return;
    }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(mf.startup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
        if (SUCCEEDED(co)) {
            CoUninitialize();
        }

        finish("video: MFStartup failed");
        return;
    }

    const double fps_measured = seconds_recorded > 0.0 ? (double)(frames.size() - 1) / seconds_recorded : 90.0;
    const uint32_t fps_milli = (uint32_t)std::clamp(std::lround(fps_measured * 1000.0), 10000l, 240000l);
    std::string result{};

    // full size with the High profile, full size with the encoder's default profile, half size
    for (int attempt = 0; attempt < 3 && result.empty(); ++attempt) {
        const bool half = attempt == 2;
        const uint32_t w = half ? WIDTH / 2 : WIDTH;
        const uint32_t h = half ? HEIGHT / 2 : HEIGHT;
        const DWORD frame_bytes = (DWORD)((uint64_t)w * h * 3 / 2);

        IMFAttributes* attributes = nullptr;
        IMFSinkWriter* writer = nullptr;
        IMFMediaType* out_type = nullptr;
        IMFMediaType* in_type = nullptr;
        DWORD stream = 0;
        HRESULT hr = mf.create_attributes(&attributes, 2);

        if (SUCCEEDED(hr)) {
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            hr = mf.create_sink_writer(path.wstring().c_str(), nullptr, attributes, &writer);
        }

        if (SUCCEEDED(hr)) {
            hr = mf.create_media_type(&out_type);
        }

        if (SUCCEEDED(hr)) {
            out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            out_type->SetUINT32(MF_MT_AVG_BITRATE, half ? 20000000u : 50000000u);
            out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(out_type, MF_MT_FRAME_RATE, fps_milli, 1000);
            MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            if (attempt == 0) {
                out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            }

            hr = writer->AddStream(out_type, &stream);
        }

        if (SUCCEEDED(hr)) {
            hr = mf.create_media_type(&in_type);
        }

        if (SUCCEEDED(hr)) {
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
            in_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, w, h);
            MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, fps_milli, 1000);
            MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            hr = writer->SetInputMediaType(stream, in_type, nullptr);
        }

        if (SUCCEEDED(hr)) {
            hr = writer->BeginWriting();
        }

        for (size_t i = 0; i < frames.size() && SUCCEEDED(hr); ++i) {
            std::vector<uint8_t> half_frame{};
            const std::vector<uint8_t>& data = half ? (half_frame = half_nv12(frames[i].nv12, WIDTH, HEIGHT)) : frames[i].nv12;
            const int64_t t = frames[i].time_100ns - frames.front().time_100ns;
            const int64_t next = i + 1 < frames.size() ? frames[i + 1].time_100ns - frames.front().time_100ns : t + (int64_t)(1e7 * 1000.0 / fps_milli);

            IMFMediaBuffer* buffer = nullptr;
            IMFSample* sample = nullptr;
            BYTE* p = nullptr;
            hr = mf.create_memory_buffer(frame_bytes, &buffer);

            if (SUCCEEDED(hr)) {
                hr = buffer->Lock(&p, nullptr, nullptr);
            }

            if (SUCCEEDED(hr)) {
                std::memcpy(p, data.data(), frame_bytes);
                buffer->Unlock();
                hr = buffer->SetCurrentLength(frame_bytes);
            }

            if (SUCCEEDED(hr)) {
                hr = mf.create_sample(&sample);
            }

            if (SUCCEEDED(hr)) {
                hr = sample->AddBuffer(buffer);
            }

            if (SUCCEEDED(hr)) {
                sample->SetSampleTime(t);
                sample->SetSampleDuration((std::max)(next - t, (int64_t)1));
                hr = writer->WriteSample(stream, sample);
            }

            release(sample);
            release(buffer);

            if ((i & 31) == 0) {
                set_status(std::format("writing the video: frame {} of {}...", i + 1, frames.size()));
            }
        }

        if (SUCCEEDED(hr)) {
            hr = writer->Finalize();
        }

        release(in_type);
        release(out_type);
        release(writer);
        release(attributes);

        if (SUCCEEDED(hr)) {
            result = std::format("video written: {} ({} frames, {:.1f} s, {:.0f} fps{}{})", path.string(), frames.size(), seconds_recorded,
                fps_measured, dropped > 0 ? std::format(", {} frames skipped", dropped) : std::string{}, half ? ", half size" : "");
        } else {
            REVR_LOG(spdlog::warn("[EyeRecorder] attempt {} failed: HRESULT {:#x}", attempt, (uint32_t)hr));
            std::filesystem::remove(path, ec);

            if (attempt == 2) {
                result = std::format("video: the encoder refused it (HRESULT {:#x})", (uint32_t)hr);
            }
        }
    }

    mf.shutdown();

    if (SUCCEEDED(co)) {
        CoUninitialize();
    }

    finish(result);
}

void EyeRecorder::reset() {
    if (m_phase.load() == Phase::Recording || m_phase.load() == Phase::Draining) {
        m_frames.clear();

        if (m_measure) {
            finish_measure(); // [LIGHT FLICKER TEST] what was measured so far
        }

        m_phase = Phase::Idle;
        set_status("video: recording stopped (the graphics device was reset)");
    }

    for (auto& s : m_slots) {
        s.commands.reset();
        s.nv12.Reset();
        s.readback.Reset();
        s.pending = false;
    }

    m_pso.Reset();
    m_root_sig.Reset();
    m_heap.Reset();
    m_next_slot = 0;
    m_setup_failed = false;
}

} // namespace d3d12
#endif // !REVR_RELEASE
