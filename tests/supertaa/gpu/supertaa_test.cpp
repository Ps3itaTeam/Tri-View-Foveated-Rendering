// GPU test of d3d12::SuperTAA: compiles its shaders with the real D3DCompile (cs_5_0), checks the constant blocks'
// layout against the shaders' reflection, then runs the pass class over a sequence written by make_case.py and writes
// every image of every frame as raw files for compare_case.py.
//
//   supertaa_test.exe <case dir>
#include <windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <format>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "SuperTAA.hpp"
#include "UiMotion.hpp"
#include "SuperTAACapture.hpp"
#include "TextureDump.hpp"
#include "FoveaComposite.hpp"

using d3d12::ComPtr;
using d3d12::SuperTAA;

static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static HANDLE g_event;
static UINT64 g_fence_value = 0;

static void run_list() {
    g_cmd->Close();
    ID3D12CommandList* lists[]{g_cmd.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence.Get(), ++g_fence_value);
    g_fence->SetEventOnCompletion(g_fence_value, g_event);
    WaitForSingleObject(g_event, 120000);
    g_alloc->Reset();
    g_cmd->Reset(g_alloc.Get(), nullptr);
}

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static ComPtr<ID3D12Resource> make_tex(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// uploads tightly packed rows (bytes_per_pixel each) into `tex` resting in `state`
static void upload(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, const void* data, UINT bytes_per_pixel) {
    const auto td = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    void* p = nullptr;
    buf->Map(0, nullptr, &p);

    for (UINT y = 0; y < td.Height; ++y) {
        std::memcpy((char*)p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const char*)data + (size_t)y * td.Width * bytes_per_pixel,
            (size_t)td.Width * bytes_per_pixel);
    }

    buf->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_cmd->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = buf.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_cmd->ResourceBarrier(1, &b);
    run_list();
}

// writes `tex` (resting in `state`) as tightly packed rows
static void download(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, const std::string& path, UINT bytes_per_pixel) {
    if (tex == nullptr) {
        return;
    }

    const auto td = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rp{};
    rp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    g_dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_cmd->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = buf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_cmd->ResourceBarrier(1, &b);
    run_list();
    void* p = nullptr;
    buf->Map(0, nullptr, &p);
    std::ofstream f(path, std::ios::binary);

    for (UINT y = 0; y < td.Height; ++y) {
        f.write((const char*)p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (std::streamsize)td.Width * bytes_per_pixel);
    }

    buf->Unmap(0, nullptr);
}

// ---- the constant blocks: the shaders' reflection against the structs -------------------------------------------------
struct Expect {
    const char* name;
    size_t offset;
};

static int check_layout(const char* src, const char* entry, const std::vector<Expect>& expect, size_t struct_size) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(src, std::strlen(src), entry, nullptr, nullptr, entry, "cs_5_0", 0, 0, &code, &errors);
    std::printf("compile %-10s hr=0x%08lx size=%u%s%s\n", entry, (unsigned long)hr, code ? (unsigned)code->GetBufferSize() : 0u, errors ? "\n  messages: " : "",
        errors ? (const char*)errors->GetBufferPointer() : "");

    if (FAILED(hr)) {
        return 1;
    }

    ComPtr<ID3D12ShaderReflection> refl;

    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&refl)))) {
        std::printf("  reflection failed\n");
        return 1;
    }

    D3D12_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    std::printf("  instructions %u, bound resources %u\n", sd.InstructionCount, sd.BoundResources);
    auto* cb = refl->GetConstantBufferByName("Params");
    D3D12_SHADER_BUFFER_DESC bd{};

    if (cb == nullptr || FAILED(cb->GetDesc(&bd))) {
        std::printf("  no Params block\n");
        return 1;
    }

    int bad = 0;

    // (a shader's block is a multiple of 16 bytes; the root constants are the struct's)
    if (bd.Size != ((struct_size + 15) & ~(size_t)15)) {
        std::printf("  Params size %u, struct %u  <-- MISMATCH\n", bd.Size, (unsigned)struct_size);
        ++bad;
    }

    for (const auto& e : expect) {
        auto* v = cb->GetVariableByName(e.name);
        D3D12_SHADER_VARIABLE_DESC vd{};

        if (v == nullptr || FAILED(v->GetDesc(&vd))) {
            std::printf("  %s: not in the shader  <-- MISMATCH\n", e.name);
            ++bad;
            continue;
        }

        if (vd.StartOffset != e.offset) {
            std::printf("  %s: shader offset %u, struct %u  <-- MISMATCH\n", e.name, vd.StartOffset, (unsigned)e.offset);
            ++bad;
        }
    }

    std::printf("  layout %s (%u variables, %u bytes)\n", bad == 0 ? "ok" : "WRONG", bd.Variables, bd.Size);
    return bad;
}

int main(int argc, char** argv) {
    int bad = 0;
#define O(S, m) offsetof(SuperTAA::S, m)
    bad += check_layout(SuperTAA::analyze_source(), "analyze",
        {{"g_size", O(AnalyzeK, size)}, {"g_mv_scale", O(AnalyzeK, mv_scale)}, {"g_depth_k", O(AnalyzeK, depth_k)}, {"g_flags", O(AnalyzeK, flags)},
            {"g_reject", O(AnalyzeK, reject)}, {"g_gamma", O(AnalyzeK, gamma)}, {"g_w_max", O(AnalyzeK, w_max)}, {"g_jitter", O(AnalyzeK, jitter)},
            {"g_k", O(AnalyzeK, k)}, {"g_b0", O(AnalyzeK, b0)}, {"g_kc", O(AnalyzeK, kc)}, {"g_b0c", O(AnalyzeK, b0c)}, {"g_cap", O(AnalyzeK, cap)},
            {"g_rate", O(AnalyzeK, rate)}, {"g_rate_b", O(AnalyzeK, rate_b)}, {"g_w_short", O(AnalyzeK, w_short)}, {"g_count", O(AnalyzeK, count)},
            {"g_alln", O(AnalyzeK, alln)}, {"g_fl_sde", O(AnalyzeK, fl_sde)}, {"g_af_rate", O(AnalyzeK, af_rate)}, {"g_af_t0", O(AnalyzeK, af_t0)},
            {"g_af_t1", O(AnalyzeK, af_t1)}, {"g_af_mem", O(AnalyzeK, af_mem)}, {"g_edge0", O(AnalyzeK, edge0)}, {"g_edge1", O(AnalyzeK, edge1)},
            {"g_rel_decay", O(AnalyzeK, rel_decay)}},
        sizeof(SuperTAA::AnalyzeK));
    bad += check_layout(SuperTAA::accumulate_source(), "accumulate",
        {{"g_in_size", O(AccumulateK, in_size)}, {"g_hist_size", O(AccumulateK, hist_size)}, {"g_jitter", O(AccumulateK, jitter)}, {"g_k", O(AccumulateK, k)},
            {"g_w_cap", O(AccumulateK, w_cap)}, {"g_motion_reduce", O(AccumulateK, motion_reduce)}, {"g_extra", O(AccumulateK, extra)},
            {"g_flags", O(AccumulateK, flags)}, {"g_relax", O(AccumulateK, relax)}, {"g_wins", O(AccumulateK, wins)},
            {"g_wins_floor", O(AccumulateK, wins_floor)}},
        sizeof(SuperTAA::AccumulateK));
    bad += check_layout(SuperTAA::resolve_source(), "encode",
        {{"g_src_size", O(ResolveK, src_size)}, {"g_dst_size", O(ResolveK, dst_size)}, {"g_flags", O(ResolveK, flags)}, {"g_lz", O(ResolveK, lz)}},
        sizeof(SuperTAA::ResolveK));
    bad += check_layout(SuperTAA::resolve_source(), "sharpen",
        {{"g_src_size", O(ResolveK, src_size)}, {"g_dst_size", O(ResolveK, dst_size)}, {"g_flags", O(ResolveK, flags)}, {"g_sharp", O(ResolveK, sharp)},
            {"g_debug", O(ResolveK, debug)}, {"g_split_x", O(ResolveK, split_x)}},
        sizeof(SuperTAA::ResolveK));
#undef O
    std::printf("layout check: %s\n", bad == 0 ? "all ok" : "FAILED");

    if (argc < 2) {
        return bad;
    }

    // "cso <out dir>": the compiled code of the analyze pass, plain and with the interface's motion (to compare with
    // another build's: the plain pass must not change when only the other is touched)
    if (std::string{argv[1]} == "cso" && argc >= 3) {
        const auto compile_to = [&](const std::string& src, const char* file) {
            ComPtr<ID3DBlob> code;
            ComPtr<ID3DBlob> errors;

            if (FAILED(D3DCompile(src.data(), src.size(), file, nullptr, nullptr, "analyze", "cs_5_0", 0, 0, &code, &errors)) || code == nullptr) {
                std::printf("%s: compile failed: %s\n", file, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
                return 1;
            }

            std::ofstream f(std::string{argv[2]} + "/" + file, std::ios::binary);
            f.write((const char*)code->GetBufferPointer(), (std::streamsize)code->GetBufferSize());
            std::printf("%s: %zu bytes\n", file, (size_t)code->GetBufferSize());
            return 0;
        };
        return compile_to(SuperTAA::analyze_source(), "analyze.cso") + compile_to(SuperTAA::analyze_ui_motion_source(), "analyze_ui_motion.cso");
    }

    const std::string dir = argv[1];
    std::map<std::string, std::string> meta;
    std::vector<std::pair<float, float>> jitter;
    {
        std::ifstream f(dir + "/meta.txt");
        std::string line;

        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string key;
            ss >> key;

            if (key == "jitter") {
                float x = 0, y = 0;
                ss >> x >> y;
                jitter.emplace_back(x, y);
            } else {
                std::string value;
                ss >> value;
                meta[key] = value;
            }
        }
    }

    auto geti = [&](const char* k, int d) { return meta.count(k) ? std::stoi(meta[k]) : d; };
    auto getf = [&](const char* k, float d) { return meta.count(k) ? std::stof(meta[k]) : d; };
    const UINT W = (UINT)geti("width", 0);
    const UINT H = (UINT)geti("height", 0);
    const UINT OW = (UINT)geti("out_width", (int)W);
    const UINT OH = (UINT)geti("out_height", (int)H);
    const int frames = geti("frames", 0);

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) {
        std::printf("no D3D12 device\n");
        return 100;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // the engine's formats: colour 10 bit encoded; motion and depth as floats (motion in pixels, mv scale = 1 / size)
    // "game" 1: the formats the game's images have (motion: 16 bit signed, in the engine's units; depth: a depth-stencil
    // texture)
    const bool game = geti("game", 0) != 0;
    auto color = make_tex(W, H, DXGI_FORMAT_R10G10B10A2_UNORM, PSR, false);
    auto motion = make_tex(W, H, game ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R32G32_FLOAT, PSR, false);
    ComPtr<ID3D12Resource> depth;

    if (game) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W;
        td.Height = H;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, PSR, &cv, IID_PPV_ARGS(&depth));

        if (depth == nullptr) {
            std::printf("no depth-stencil texture\n");
            return 102;
        }
    } else {
        depth = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, PSR, false);
    }
    const bool out10 = geti("out_10bit", 0) != 0;
    auto out = make_tex(OW, OH, out10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);
    auto split = make_tex(OW, OH, out10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);

    // [UI MOTION fix125] "ui_motion" 1: every frame brings the view's copies without and with GUI (frameNN_hud.bin /
    // _fin.bin, like the colour; resting in COMMON as the engine's) and the block of the interface's motion
    // (frameNN_um.bin: d3d12::UiMotion::Constants as it is); "ui_motion" 2: the copies and the block are there but the
    // pass is not given them (the same case along the scene's motion vectors)
    const int ui_motion_mode = geti("ui_motion", 0);
    constexpr auto COMMON = D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> ui_hud;
    ComPtr<ID3D12Resource> ui_fin;
    d3d12::UiMotion ui_motion_blocks;

    if (ui_motion_mode != 0) {
        ui_hud = make_tex(W, H, DXGI_FORMAT_R10G10B10A2_UNORM, COMMON, false);
        ui_fin = make_tex(W, H, DXGI_FORMAT_R10G10B10A2_UNORM, COMMON, false);
    }

    SuperTAA taa;
    // ([fix80] "slots 1": one view's histories only, as the periphery's instance has them)
    taa.set_slots((uint32_t)geti("slots", 2));
    taa.set_ui_motion(ui_motion_mode == 1);
    SuperTAA::Params p{};
    auto& s = p.s;
    s.scale = (uint32_t)geti("scale", 2);
    s.filter_scale = getf("filter_scale", s.filter_scale);
    s.stability = getf("stability", s.stability);
    s.clip_gamma = getf("clip_gamma", s.clip_gamma);
    s.motion_reduce = getf("motion_reduce", s.motion_reduce);
    s.depth_reject = getf("depth_reject", s.depth_reject);
    s.history_sharpness = getf("history_sharpness", s.history_sharpness);
    s.sharpen = getf("sharpen", s.sharpen);
    s.luma_weight = geti("luma_weight", 1) != 0;
    s.dering = (uint32_t)geti("dering", 1);
    s.nearest_sample = geti("nearest_sample", 0) != 0;
    s.box_resolve = geti("box_resolve", 1) != 0;
    s.follow = geti("follow", 1) != 0;
    s.follow_threshold = getf("follow_threshold", s.follow_threshold);
    s.follow_memory = getf("follow_memory", s.follow_memory);
    s.fl_count = (uint32_t)geti("fl_count", (int)s.fl_count);
    s.fl_alln = (uint32_t)geti("fl_alln", (int)s.fl_alln);
    s.fl_cap = getf("fl_cap", s.fl_cap);
    s.protect = geti("protect", 1) != 0;
    s.protect_range = getf("protect_range", s.protect_range);
    s.protect_spark = getf("protect_spark", s.protect_spark);
    s.protect_memory = getf("protect_memory", s.protect_memory);
    s.arrive = geti("arrive", 1) != 0;
    p.mv_scale_x = game ? 0.5f : 1.0f / (float)W; // (the engine: current UV + motion * (0.5, -0.5) = previous UV)
    p.mv_scale_y = game ? -0.5f : 1.0f / (float)H;
    p.reverse_z = true;
    p.depth_k[0] = 0.0f; // z = 0.1 / distance
    p.depth_k[1] = 0.1f;
    p.depth_k[2] = 1.0f;
    p.depth_k[3] = 0.0f;
    p.depth_k_valid = true;
    p.debug_view = (uint32_t)geti("debug_view", 0);
    p.keep_constants = true;
    const int split_x = geti("split_x", 0);
    std::printf("case %s: %ux%u -> %ux%u, %d frames, scale %u\n", dir.c_str(), W, H, OW, OH, frames, s.scale);
    CreateDirectoryA((dir + "/gpu").c_str(), nullptr);

    // "capture" N: the last N frames through d3d12::SuperTAACapture as the game does it, the mod's TAA (d3d12::FoveaTAA)
    // beside the pass, the depth-stencil's part through d3d12::FoveaLinear
    const int capture_frames = geti("capture", 0);
    const int capture_start = frames - capture_frames;
    d3d12::SuperTAACapture capture;
    d3d12::FoveaTAA old_taa;
    d3d12::FoveaLinear linear_helper;
    auto cmp = make_tex(OW, OH, out10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);

    if (capture_frames > 0) {
        capture.start(capture_frames, (uint32_t)geti("capture_part", 64));

        // (the warm-up frames before the case's own: the recording starts with frame capture_start)
        for (int k = 0; k < d3d12::SuperTAACapture::WARM_FRAMES - 1 - capture_start; ++k) {
            capture.tick();
        }
    }

    for (int t = 0; t < frames; ++t) {
        char name[64];
        std::snprintf(name, sizeof(name), "/frame%02d", t);
        const std::string base = dir + name;
        // colour: float rgba -> 10 bit
        {
            const auto raw = read_file(base + "_color.bin");
            const float* f = (const float*)raw.data();
            std::vector<uint32_t> packed((size_t)W * H);

            for (size_t i = 0; i < packed.size(); ++i) {
                const uint32_t r = (uint32_t)(f[i * 3 + 0] * 1023.0f + 0.5f);
                const uint32_t g = (uint32_t)(f[i * 3 + 1] * 1023.0f + 0.5f);
                const uint32_t b = (uint32_t)(f[i * 3 + 2] * 1023.0f + 0.5f);
                packed[i] = r | (g << 10) | (b << 20) | (3u << 30);
            }

            upload(color.Get(), PSR, packed.data(), 4);
        }
        {
            const auto raw = read_file(base + "_mv.bin");

            if (game) {
                // pixels -> the engine's units, 16 bit signed (4 channels, the last two unused)
                const float* f = (const float*)raw.data();
                std::vector<int16_t> packed((size_t)W * H * 4, 0);

                for (size_t i = 0; i < (size_t)W * H; ++i) {
                    packed[i * 4 + 0] = (int16_t)std::lround(std::clamp(f[i * 2 + 0] * 2.0f / (float)W, -1.0f, 1.0f) * 32767.0f);
                    packed[i * 4 + 1] = (int16_t)std::lround(std::clamp(-f[i * 2 + 1] * 2.0f / (float)H, -1.0f, 1.0f) * 32767.0f);
                }

                upload(motion.Get(), PSR, packed.data(), 8);
            } else {
                upload(motion.Get(), PSR, raw.data(), 8);
            }
        }
        {
            const auto raw = read_file(base + "_depth.bin");
            upload(depth.Get(), PSR, raw.data(), 4);
        }

        if (ui_motion_mode != 0) {
            for (int which = 0; which < 2; ++which) {
                const auto raw = read_file(base + (which == 0 ? "_hud.bin" : "_fin.bin"));
                const float* f = (const float*)raw.data();
                std::vector<uint32_t> packed((size_t)W * H);

                for (size_t i = 0; i < packed.size(); ++i) {
                    const uint32_t r = (uint32_t)(f[i * 3 + 0] * 1023.0f + 0.5f);
                    const uint32_t g = (uint32_t)(f[i * 3 + 1] * 1023.0f + 0.5f);
                    const uint32_t b = (uint32_t)(f[i * 3 + 2] * 1023.0f + 0.5f);
                    packed[i] = r | (g << 10) | (b << 20) | (3u << 30);
                }

                upload((which == 0 ? ui_hud : ui_fin).Get(), COMMON, packed.data(), 4);
            }

            const auto raw = read_file(base + "_um.bin");
            d3d12::UiMotion::Constants uk{};

            if (raw.size() != sizeof(uk)) {
                std::printf("frame %d: _um.bin has %zu bytes, the block %zu\n", t, raw.size(), sizeof(uk));
                return 103;
            }

            std::memcpy(&uk, raw.data(), sizeof(uk));
            p.ui_hud = ui_motion_mode == 1 ? ui_hud.Get() : nullptr;
            p.ui_fin = ui_motion_mode == 1 ? ui_fin.Get() : nullptr;
            p.ui_motion = ui_motion_mode == 1 ? ui_motion_blocks.write(g_dev.Get(), uk) : d3d12::UiMotion::Block{};
        }

        const bool capturing = capture_frames > 0 && capture.tick();
        ID3D12Resource* cmp_now = nullptr;

        if (capturing && capture.wants_compare()) {
            d3d12::FoveaTAA::Params tp{};
            tp.eye = 0;
            tp.mv_scale_x = p.mv_scale_x;
            tp.mv_scale_y = p.mv_scale_y;
            tp.jitter_x = jitter[(size_t)t].first;
            tp.jitter_y = jitter[(size_t)t].second;
            tp.reverse_z = true;
            std::memcpy(tp.depth_k, p.depth_k, sizeof(tp.depth_k));
            tp.depth_k_valid = true;

            if (old_taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, cmp.Get(), tp)) {
                cmp_now = cmp.Get();
            } else {
                std::printf("frame %d: the mod's TAA failed: %s\n", t, old_taa.last_error());
            }
        }

        p.eye = 0;
        p.jitter_x = jitter[(size_t)t].first;
        p.jitter_y = jitter[(size_t)t].second;
        p.reset = false;
        p.split_x = (uint32_t)split_x;
        p.split_src = split_x != 0 ? split.Get() : nullptr;

        if (!taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p)) {
            std::printf("frame %d: dispatch failed: %s\n", t, taa.last_error());
            return 101;
        }

        if (ui_motion_mode == 1 && !taa.last_ui_motion()) {
            std::printf("frame %d: the pass ran without the interface's motion\n", t);
            return 104;
        }

        if (capture_frames > 0 && capture.recording()) {
            const std::string settings_text = std::format(
                "settings: scale {} stability {:.9g} history_sharpness {:.9g} sharpen {:.9g} follow {} follow_threshold {:.9g} follow_memory {:.9g} "
                "filter_scale {:.9g} clip_gamma {:.9g} motion_reduce {:.9g} depth_reject {:.9g} luma_weight {} lighter {} lanczos {} protect {} "
                "protect_range {:.9g} protect_spark {:.9g} protect_memory {:.9g} arrive {}\n",
                s.scale, s.stability, s.history_sharpness, s.sharpen, s.follow ? 1 : 0, s.follow_threshold, s.follow_memory, s.filter_scale, s.clip_gamma,
                s.motion_reduce, s.depth_reject, s.luma_weight ? 1 : 0, s.nearest_sample ? 1 : 0, s.box_resolve ? 0 : 1, s.protect ? 1 : 0,
                s.protect_range, s.protect_spark, s.protect_memory, s.arrive ? 1 : 0);
            capture.record(g_dev.Get(), g_cmd.Get(), taa, 0, color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), cmp_now, p, settings_text,
                std::format("engine_frame {} fovea_rectangle 0 0 {} {}", t, OW, OH),
                [&](ID3D12Resource* d, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                    d3d12::FoveaLinear::Region r{};
                    r.w = w;
                    r.h = h;
                    r.shift_x = (int32_t)x;
                    r.shift_y = (int32_t)y;
                    return linear_helper.copy_part(g_dev.Get(), g_cmd.Get(), d, PSR, target, r, true);
                });

            // [CAPTURE fix81] other images and numbers of the frame, as the game adds the periphery's chain (here the same
            // view stands for every stage: this checks the files and chain.txt, read by analyze_chain.py)
            const uint32_t cw = (std::min)((uint32_t)W, 64u) & ~1u;
            const uint32_t ch = (std::min)((uint32_t)H, 48u) & ~1u;
            const uint32_t cs = (uint32_t)OW / (uint32_t)W;
            capture.extra(g_dev.Get(), g_cmd.Get(), color.Get(), PSR, "wide_color", 2, 2, cw, ch);
            capture.extra(g_dev.Get(), g_cmd.Get(), motion.Get(), PSR, "wide_motion", 2, 2, cw, ch);
            capture.extra(g_dev.Get(), g_cmd.Get(), depth.Get(), PSR, "wide_depth", 2, 2, cw, ch,
                [&](ID3D12Resource* d, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                    d3d12::FoveaLinear::Region r{};
                    r.w = w;
                    r.h = h;
                    r.shift_x = (int32_t)x;
                    r.shift_y = (int32_t)y;
                    return linear_helper.copy_part(g_dev.Get(), g_cmd.Get(), d, PSR, target, r, true);
                });
            capture.extra(g_dev.Get(), g_cmd.Get(), out.Get(), UAV, "wide_taa", 2 * cs, 2 * cs, cw * cs, ch * cs);
            capture.extra(g_dev.Get(), g_cmd.Get(), out.Get(), UAV, "eye_periphery", 2 * cs, 2 * cs, cw * cs, ch * cs);
            capture.extra(g_dev.Get(), g_cmd.Get(), out.Get(), UAV, "eye_out", 2 * cs, 2 * cs, cw * cs, ch * cs);
            capture.note(std::format("wide: engine_frame {} view {} {} part 2 2 {} {} ss {} jitter {:.9g} {:.9g} mv_scale {:.9g} {:.9g} depth_k {:.9g} {:.9g} "
                                     "{:.9g} {:.9g} periphery_aa 2 linear 0 stable_depth 0 wide_tan -1 1 -1 1 px_per_tan {:.9g} {:.9g}",
                t, W, H, cw, ch, cs, p.jitter_x, p.jitter_y, p.mv_scale_x, p.mv_scale_y, p.depth_k[0], p.depth_k[1], p.depth_k[2], p.depth_k[3], 0.5f * (float)W,
                0.5f * (float)H));
            capture.note(std::format("eye_periphery: eye 0 rays {} {} part 2 2 {} {} x_scale 1 x_offset 0 y_scale 1 y_offset 0 disparity 0 dist_min 0.12 "
                                     "max_shift 96 fill 1 eye_tan -1 1 -1 1 eye_image {} {} looked_at 0 0 fovea_rectangle 0 0 0 0 0",
                W, H, cw, ch, OW, OH));
            capture.note(std::format("eye_out: eye 0 image {} {} part {} {} {} {} fovea_drawn 1 fovea_rectangle 1 0 0 {} {} fovea_jitter {:.9g} {:.9g} wide_jitter 0 "
                                     "0 steady_interface 0 light_layer 0 matte 0 debug_view 0 post 0",
                OW, OH, 2 * cs, 2 * cs, cw * cs, ch * cs, OW, OH, p.jitter_x, p.jitter_y));
        }

        run_list();

        // ([CAPTURE fix81] a captured frame is closed by the tick after it; after the run's last frame that tick is made here)
        if (capture_frames > 0 && t + 1 == frames && capture.recording()) {
            capture.tick();
        }

        if (capture_frames > 0 && capture.write_pending()) {
            const std::string summary = capture.write(dir + "/capture");
            std::printf("capture written:\n%s", summary.c_str());
        }

        if (t == 0) {
            std::printf("%s", taa.last_constants().c_str());
        }

        const std::string o = dir + "/gpu" + name;
        download(out.Get(), UAV, o + "_out.bin", out10 ? 4 : 8);
        download(taa.linear(), UAV, o + "_lin.bin", 8);
        download(taa.guide(), UAV, o + "_guide.bin", 8);
        download(taa.box_centre(), UAV, o + "_boxc.bin", 8);
        download(taa.box_extent(), UAV, o + "_boxe.bin", 8);
        download(taa.stats(0), UAV, o + "_stats.bin", 8);
        download(taa.distances(0), UAV, o + "_dist.bin", 8);
        download(taa.history(0), UAV, o + "_hist.bin", 8);
    }

    std::printf("done: %d frames\n", frames);

    // ---- the other paths of the pass (not compared with the model: they must run, and write what they say) -------------
    if (geti("paths", 0) != 0) {
        int path_bad = 0;
        const UINT obpp = out10 ? 4 : 8;
        auto removed = [&](const char* what) {
            const HRESULT hr = g_dev->GetDeviceRemovedReason();

            if (FAILED(hr)) {
                std::printf("paths: DEVICE REMOVED after %s (0x%08lx)\n", what, (unsigned long)hr);
                ++path_bad;
                return true;
            }

            return false;
        };
        // one pixel of an image as floats (rgb)
        auto read_px = [&](ID3D12Resource* tex, const char* file, std::vector<float>& rgb) {
            download(tex, UAV, dir + "/gpu/" + file, obpp);
            const auto raw = read_file(dir + "/gpu/" + file);
            rgb.assign((size_t)OW * OH * 3, 0.0f);

            for (size_t i = 0; i < (size_t)OW * OH && (i + 1) * obpp <= raw.size(); ++i) {
                if (out10) {
                    uint32_t v;
                    std::memcpy(&v, raw.data() + i * 4, 4);
                    rgb[i * 3 + 0] = (float)(v & 1023) / 1023.0f;
                    rgb[i * 3 + 1] = (float)((v >> 10) & 1023) / 1023.0f;
                    rgb[i * 3 + 2] = (float)((v >> 20) & 1023) / 1023.0f;
                } else {
                    uint16_t h[4];
                    std::memcpy(h, raw.data() + i * 8, 8);

                    for (int c = 0; c < 3; ++c) {
                        // half -> float
                        const uint32_t e = (h[c] >> 10) & 31, m = h[c] & 1023;
                        float f = e == 0 ? std::ldexp((float)m, -24) : (e == 31 ? 1e30f : std::ldexp((float)(m + 1024), (int)e - 25));
                        rgb[i * 3 + c] = (h[c] & 0x8000) ? -f : f;
                    }
                }
            }
        };
        auto finite = [&](const std::vector<float>& rgb) {
            for (float v : rgb) {
                if (!(v >= -0.001f && v <= 4.0f)) {
                    return false;
                }
            }

            return true;
        };

        std::vector<float> plain;
        p.debug_view = 0;
        p.split_x = 0;
        p.split_src = nullptr;

        // (a) both eyes in one list, as a frame of the game does
        for (uint32_t eye = 0; eye < 2; ++eye) {
            p.eye = eye;

            if (!taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p)) {
                std::printf("paths: both eyes: dispatch of eye %u failed: %s\n", eye, taa.last_error());
                ++path_bad;
            }
        }

        run_list();
        removed("both eyes in one list");
        p.eye = 0;
        taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p);
        run_list();
        read_px(out.Get(), "paths_plain.bin", plain);
        std::printf("paths: plain output finite: %s\n", finite(plain) ? "yes" : "NO");
        path_bad += finite(plain) ? 0 : 1;

        // (b) the debug views
        for (uint32_t view = 1; view <= 5; ++view) {
            p.debug_view = view;

            if (!taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p)) {
                std::printf("paths: debug view %u: dispatch failed: %s\n", view, taa.last_error());
                ++path_bad;
                continue;
            }

            run_list();

            if (removed("a debug view")) {
                break;
            }

            std::vector<float> img;
            char file[64];
            std::snprintf(file, sizeof(file), "paths_debug%u.bin", view);
            read_px(out.Get(), file, img);
            double diff = 0.0;

            for (size_t i = 0; i < img.size(); ++i) {
                diff += std::fabs(img[i] - plain[i]);
            }

            std::printf("paths: debug view %u: finite %s, mean difference to the plain image %.4f\n", view, finite(img) ? "yes" : "NO", diff / (double)img.size());
            path_bad += finite(img) ? 0 : 1;
        }

        p.debug_view = 0;

        // (c) the split: the left half from another image, a yellow line between
        {
            std::vector<uint8_t> fill((size_t)OW * OH * obpp);

            for (size_t i = 0; i < (size_t)OW * OH; ++i) {
                if (out10) {
                    const uint32_t v = 512u | (256u << 10) | (128u << 20) | (3u << 30);
                    std::memcpy(fill.data() + i * 4, &v, 4);
                } else {
                    const uint16_t h[4]{0x3800, 0x3400, 0x3000, 0x3C00}; // 0.5, 0.25, 0.125, 1
                    std::memcpy(fill.data() + i * 8, h, 8);
                }
            }

            upload(split.Get(), UAV, fill.data(), obpp);
            p.split_x = OW / 2;
            p.split_src = split.Get();

            if (!taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p)) {
                std::printf("paths: split: dispatch failed: %s\n", taa.last_error());
                ++path_bad;
            } else {
                run_list();
                removed("the split");
                std::vector<float> img;
                read_px(out.Get(), "paths_split.bin", img);
                const size_t y = OH / 2;
                const float* l = &img[(y * OW + 3) * 3];
                const float* m = &img[(y * OW + OW / 2) * 3];
                const float* r = &img[(y * OW + OW - 4) * 3];
                const float* rp = &plain[(y * OW + OW - 4) * 3];
                const bool left_ok = std::fabs(l[0] - 0.5f) < 0.01f && std::fabs(l[1] - 0.25f) < 0.01f && std::fabs(l[2] - 0.125f) < 0.01f;
                const bool line_ok = m[0] > 0.95f && m[1] > 0.95f && m[2] < 0.05f;
                const bool right_ok = std::fabs(r[0] - rp[0]) < 0.02f && std::fabs(r[1] - rp[1]) < 0.02f && std::fabs(r[2] - rp[2]) < 0.02f;
                std::printf("paths: split: left half from the other image %s, line %s, right half the pass's own %s\n", left_ok ? "ok" : "WRONG",
                    line_ok ? "ok" : "WRONG", right_ok ? "ok" : "WRONG");
                path_bad += (left_ok && line_ok && right_ok) ? 0 : 1;
            }

            p.split_x = 0;
            p.split_src = nullptr;
        }

        // (d) the history's scale changed while a list with the old images is still on the queue: the old images must
        //     stay alive (the pass keeps them for a while) and the pass go on with the new ones
        {
            ComPtr<ID3D12CommandAllocator> alloc2;
            ComPtr<ID3D12GraphicsCommandList> cmd2;
            g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc2));
            g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2.Get(), nullptr, IID_PPV_ARGS(&cmd2));
            const uint32_t first = s.scale >= 2 ? 2u : 1u;
            const uint32_t other = first == 2 ? 1u : 2u;
            bool ok = true;

            for (int round = 0; round < 3 && ok; ++round) {
                // list 1 with the scale so far, sent without waiting for it
                s.scale = (round % 2) == 0 ? first : other;
                ok &= taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p);
                g_cmd->Close();
                ID3D12CommandList* l1[]{g_cmd.Get()};
                g_queue->ExecuteCommandLists(1, l1);
                // list 2 with the other scale: the pass makes its images again while list 1 may still run
                s.scale = (round % 2) == 0 ? other : first;
                ok &= taa.dispatch(g_dev.Get(), cmd2.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p);
                cmd2->Close();
                ID3D12CommandList* l2[]{cmd2.Get()};
                g_queue->ExecuteCommandLists(1, l2);
                g_queue->Signal(g_fence.Get(), ++g_fence_value);
                g_fence->SetEventOnCompletion(g_fence_value, g_event);
                WaitForSingleObject(g_event, 120000);
                g_alloc->Reset();
                g_cmd->Reset(g_alloc.Get(), nullptr);
                alloc2->Reset();
                cmd2->Reset(alloc2.Get(), nullptr);
            }

            cmd2->Close();
            s.scale = first;
            const bool gone = removed("the scale changes");
            std::printf("paths: scale changed between lists on the queue: %s (history now %ux%u)\n", ok && !gone ? "ok" : "FAILED", taa.history_w(),
                taa.history_h());
            path_bad += ok ? 0 : 1;

            // ... and it settles again: many frames later the image is the plain one
            for (int k = 0; k < 40; ++k) {
                taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p);
                run_list();
            }

            std::vector<float> img;
            read_px(out.Get(), "paths_after_scale.bin", img);
            std::printf("paths: after the scale changes: finite %s\n", finite(img) ? "yes" : "NO");
            path_bad += finite(img) ? 0 : 1;
        }

        // (d2) [fix101 20.11.2026] the image's size changes and the history's does not (the periphery's instance under the
        // GPU guard: an image of twice the view's size with scale 1 <-> of the view's size with scale 2). Six frames with
        // one, then a frame with the other: the same bytes as an instance that made that other image all along.
        {
            auto small_out = make_tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);
            auto big_out = make_tex(W * 2, H * 2, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);
            const SuperTAA::Params saved = p;
            int failed = 0;

            for (int dir = 0; dir < 2; ++dir) {
                std::vector<float> got[2];

                for (int ref = 0; ref < 2; ++ref) {
                    SuperTAA t2;
                    t2.set_slots(1);
                    SuperTAA::Params q = saved;
                    q.eye = 0;

                    for (int f = 0; f < 7; ++f) {
                        const bool small = (f == 6 || ref == 1) ? dir == 0 : dir != 0;
                        q.s.scale = small ? 2u : 1u;
                        q.reset = f == 0;
                        failed += t2.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, small ? small_out.Get() : big_out.Get(), q) ? 0 : 1;
                        run_list();
                    }

                    read_px(dir == 0 ? small_out.Get() : big_out.Get(), ref == 0 ? "paths_size_switch.bin" : "paths_size_switch_ref.bin", got[ref]);
                }

                const bool same = !got[0].empty() && got[0].size() == got[1].size() &&
                                  std::memcmp(got[0].data(), got[1].data(), got[0].size() * sizeof(float)) == 0;
                std::printf("paths: the image's size changed, the history kept (%s): %s\n", dir == 0 ? "2x -> 1x" : "1x -> 2x",
                    same ? "the same bytes as without the change" : "DIFFERENT");
                path_bad += same ? 0 : 1;
            }

            path_bad += failed;
        }

        // (e) no depth texture, and a reset
        {
            p.reset = true;
            const bool ok = taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), nullptr, PSR, out.Get(), p);
            run_list();
            p.reset = false;
            const bool ok2 = taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), nullptr, PSR, out.Get(), p);
            run_list();
            std::vector<float> img;
            read_px(out.Get(), "paths_no_depth.bin", img);
            const bool gone = removed("no depth");
            std::printf("paths: without a depth texture: %s, finite %s\n", ok && ok2 && !gone ? "ok" : "FAILED", finite(img) ? "yes" : "NO");
            path_bad += (ok && ok2 && finite(img)) ? 0 : 1;
        }

        // (f) reset() and a new start
        {
            taa.reset();
            const bool ok = taa.prepare(g_dev.Get(), W, H, OW, OH, s.scale) &&
                            taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, out.Get(), p);
            run_list();
            const bool gone = removed("reset and prepare");
            std::printf("paths: reset, prepare, dispatch: %s\n", ok && !gone ? "ok" : "FAILED");
            path_bad += ok ? 0 : 1;
        }

        // ---- d3d12::TextureDump: parts of images, as stored ---------------------------------------------------------------
        {
            const UINT TW = 70, TH = 37; // (a row of 280 bytes: not a multiple of the 256 byte row pitch)
            auto t10 = make_tex(TW, TH, DXGI_FORMAT_R10G10B10A2_UNORM, PSR, false);
            auto t16 = make_tex(TW, TH, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true);
            auto t1 = make_tex(TW, TH, DXGI_FORMAT_R16_FLOAT, UAV, true);
            auto t32 = make_tex(TW, TH, DXGI_FORMAT_R32_FLOAT, UAV, true);
            std::vector<uint32_t> d10((size_t)TW * TH);
            std::vector<uint16_t> d16((size_t)TW * TH * 4);
            std::vector<uint16_t> d1((size_t)TW * TH);
            std::vector<float> d32((size_t)TW * TH);

            for (UINT y = 0; y < TH; ++y) {
                for (UINT x = 0; x < TW; ++x) {
                    const size_t i = (size_t)y * TW + x;
                    d10[i] = (x * 13u) | ((y * 27u) << 10) | (((x + y) * 7u) << 20) | (3u << 30);
                    d16[i * 4 + 0] = (uint16_t)(0x3000 + x);
                    d16[i * 4 + 1] = (uint16_t)(0x3400 + y);
                    d16[i * 4 + 2] = (uint16_t)(0x3800 + x + y);
                    d16[i * 4 + 3] = 0x3C00;
                    d1[i] = (uint16_t)(0x3000 + x * 3 + y);
                    d32[i] = (float)x * 0.5f + (float)y * 100.0f;
                }
            }

            upload(t10.Get(), PSR, d10.data(), 4);
            upload(t16.Get(), UAV, d16.data(), 8);
            upload(t1.Get(), UAV, d1.data(), 2);
            upload(t32.Get(), UAV, d32.data(), 4);

            // a depth-stencil texture cleared to a value: it is read whole
            ComPtr<ID3D12Resource> ds;
            ComPtr<ID3D12DescriptorHeap> dsv_heap;
            {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = TW;
                td.Height = TH;
                td.DepthOrArraySize = 1;
                td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R32_TYPELESS;
                td.SampleDesc.Count = 1;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                D3D12_CLEAR_VALUE cv{};
                cv.Format = DXGI_FORMAT_D32_FLOAT;
                cv.DepthStencil.Depth = 0.25f;
                g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&ds));
                D3D12_DESCRIPTOR_HEAP_DESC hd{};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
                hd.NumDescriptors = 1;
                g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
                D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format = DXGI_FORMAT_D32_FLOAT;
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

                if (ds != nullptr && dsv_heap != nullptr) {
                    g_dev->CreateDepthStencilView(ds.Get(), &dd, dsv_heap->GetCPUDescriptorHandleForHeapStart());
                    g_cmd->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = ds.Get();
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    b.Transition.StateAfter = PSR;
                    g_cmd->ResourceBarrier(1, &b);
                    run_list();
                }
            }

            d3d12::TextureDump dump;
            const UINT PX = 9, PY = 5, PW = 40, PH = 20;
            bool added = dump.add_region(g_dev.Get(), g_cmd.Get(), t10.Get(), PSR, "ten", PX, PY, PW, PH);
            dump.add_raw("ten", 0, 0, PW, PH);
            added &= dump.add_region(g_dev.Get(), g_cmd.Get(), t16.Get(), UAV, "half4", PX, PY, PW, PH);
            dump.add_raw("half4", 0, 0, PW, PH);
            added &= dump.add_region(g_dev.Get(), g_cmd.Get(), t1.Get(), UAV, "half1", PX, PY, PW, PH);
            dump.add_raw("half1", 0, 0, PW, PH);
            added &= dump.add(g_dev.Get(), g_cmd.Get(), t32.Get(), UAV, "float1");
            dump.add_raw("float1", PX, PY, PW, PH);
            // (a part that reaches past the image is clipped; a part outside is refused)
            added &= dump.add_region(g_dev.Get(), g_cmd.Get(), t16.Get(), UAV, "clipped", TW - 8, TH - 6, 40, 40);
            dump.add_raw("clipped", 0, 0, 40, 40);
            const bool refused = !dump.add_region(g_dev.Get(), g_cmd.Get(), t16.Get(), UAV, "outside", TW, 0, 4, 4);
            bool depth_added = false;

            if (ds != nullptr) {
                depth_added = dump.add_region(g_dev.Get(), g_cmd.Get(), ds.Get(), PSR, "depth", PX, PY, PW, PH);
                dump.add_raw("depth", PX, PY, PW, PH);
            }

            run_list();
            removed("the texture dump's copies");
            std::vector<float> rgb;
            uint32_t rw = 0, rh = 0;
            const bool read_ok = dump.read_rgb("ten", rgb, rw, rh) && rw == PW && rh == PH;
            bool rgb_ok = read_ok;

            if (read_ok) {
                // pixel (3, 2) of the part = (PX + 3, PY + 2) of the image
                const uint32_t v = d10[(size_t)(PY + 2) * TW + PX + 3];
                const float* q = &rgb[(size_t)(2 * PW + 3) * 3];
                rgb_ok = std::fabs(q[0] - (float)(v & 1023) / 1023.0f) < 1e-4f && std::fabs(q[1] - (float)((v >> 10) & 1023) / 1023.0f) < 1e-4f &&
                         std::fabs(q[2] - (float)((v >> 20) & 1023) / 1023.0f) < 1e-4f;
            }

            const std::string ddir = dir + "/gpu/dump";
            const std::string report = dump.write_all(ddir, 1024);
            std::printf("%s", report.c_str());

            auto same = [&](const char* file, const void* data, UINT bpp, UINT x0, UINT y0, UINT w, UINT h) {
                const auto raw = read_file(ddir + "/" + file);

                if (raw.size() != (size_t)w * h * bpp) {
                    std::printf("paths: dump %s: %u bytes, expected %u  <-- WRONG\n", file, (unsigned)raw.size(), (unsigned)(w * h * bpp));
                    return false;
                }

                for (UINT y = 0; y < h; ++y) {
                    if (std::memcmp(raw.data() + (size_t)y * w * bpp, (const char*)data + ((size_t)(y0 + y) * TW + x0) * bpp, (size_t)w * bpp) != 0) {
                        std::printf("paths: dump %s: row %u differs  <-- WRONG\n", file, y);
                        return false;
                    }
                }

                return true;
            };

            char fn[160];
            bool files_ok = true;
            std::snprintf(fn, sizeof(fn), "ten_raw_x0_y0_%ux%u_R10G10B10A2_UNORM.bin", PW, PH);
            files_ok &= same(fn, d10.data(), 4, PX, PY, PW, PH);
            std::snprintf(fn, sizeof(fn), "half4_raw_x0_y0_%ux%u_R16G16B16A16_FLOAT.bin", PW, PH);
            files_ok &= same(fn, d16.data(), 8, PX, PY, PW, PH);
            std::snprintf(fn, sizeof(fn), "half1_raw_x0_y0_%ux%u_R16_FLOAT.bin", PW, PH);
            files_ok &= same(fn, d1.data(), 2, PX, PY, PW, PH);
            std::snprintf(fn, sizeof(fn), "float1_raw_x%u_y%u_%ux%u_R32_FLOAT.bin", PX, PY, PW, PH);
            files_ok &= same(fn, d32.data(), 4, PX, PY, PW, PH);
            std::snprintf(fn, sizeof(fn), "clipped_raw_x0_y0_%ux%u_R16G16B16A16_FLOAT.bin", 8u, 6u);
            files_ok &= same(fn, d16.data(), 8, TW - 8, TH - 6, 8, 6);
            bool depth_ok = depth_added;

            if (depth_added) {
                std::snprintf(fn, sizeof(fn), "depth_raw_x%u_y%u_%ux%u_R32_TYPELESS.bin", PX, PY, PW, PH);
                auto raw = read_file(ddir + "/" + fn);

                if (raw.empty()) {
                    std::snprintf(fn, sizeof(fn), "depth_raw_x%u_y%u_%ux%u_R32_FLOAT.bin", PX, PY, PW, PH);
                    raw = read_file(ddir + "/" + fn);
                }

                depth_ok = raw.size() == (size_t)PW * PH * 4;

                for (size_t i = 0; depth_ok && i < (size_t)PW * PH; ++i) {
                    float v;
                    std::memcpy(&v, raw.data() + i * 4, 4);
                    depth_ok = v == 0.25f;
                }
            }

            std::printf("paths: texture dump: added %s, outside refused %s, read_rgb %s, files %s, depth-stencil whole %s\n", added ? "ok" : "FAILED",
                refused ? "ok" : "NO", rgb_ok ? "ok" : "WRONG", files_ok ? "ok" : "WRONG", depth_ok ? "ok" : (ds != nullptr ? "WRONG" : "not made"));
            path_bad += (added && refused && rgb_ok && files_ok && depth_ok) ? 0 : 1;
        }

        std::printf("paths: %s\n", path_bad == 0 ? "all ok" : "FAILED");
        bad += path_bad;
    }

    return bad;
}
