// GPU test of the periphery's reprojection with 2 x 2 pixels per ray ([PERIPHERY 2X fix80 17.11.2026],
// d3d12::MonoPeriphery::Params::ss = 2): the shared wide view's image at twice its size brought to each eye.
//
//   periphery_test.exe <out dir> [eye pass shader text]
//     runs d3d12::MonoPeriphery::reproject twice on a made-up wide view (a wall, a slanted floor, a near box, a thin
//     pole, a near thing at the image's border: hits, gaps beside near things, rays past the border):
//       ss = 1  colour W x H        -> each eye's periphery ew x eh         (out1_L.bin, out1_R.bin)
//       ss = 2  colour 2W x 2H      -> each eye's periphery 2ew x 2eh       (out2_L.bin, out2_R.bin)
//     The colour's red and green hold the place in the image (u, v), the same picture at both sizes, so every read
//     returns where it read: check_periphery.py compares the four pixels of every ray with the ray's own (the same
//     surface found, each read a quarter of a column and row to its side).
//     With the eye pass's shader text (extract_shader.py takes it out of FoveaComposite.cpp): each left eye's periphery
//     then goes through that shader (the periphery alone: no fovea, interface or post pass) into an eye image four
//     times the rays' size, with groups of 32 x 32 and of 16 x 16 pixels (eye<ss>_t<32|16>_L.bin): check_periphery.py
//     compares the enlargement with the formula (Catmull-Rom kept within the 2 x 2 around), at both densities.
//   periphery_test.exe compile <hlsl file> <entry> <out file> [NAME | NAME=VALUE ...]
//     compiles an entry point of a shader text with the real D3DCompile (cs_5_0) and the given macros (1 without a
//     value), and writes the code: to compare what two versions of a source compile to.
#include <chrono>
#include <windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "FoveaComposite.hpp"

using d3d12::ComPtr;
using d3d12::MonoPeriphery;
using d3d12::StableDepth;

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
    WaitForSingleObject(g_event, 300000);
    g_alloc->Reset();
    g_cmd->Reset(g_alloc.Get(), nullptr);
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

static int compile_to_file(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: periphery_test.exe compile <hlsl file> <entry> <out file> [NAME | NAME=VALUE ...]\n");
        return 2;
    }

    std::ifstream f(argv[2], std::ios::binary);
    const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<D3D_SHADER_MACRO> macros;
    std::vector<std::string> names(argc), values(argc);

    for (int i = 5; i < argc; ++i) { // NAME or NAME=VALUE
        const std::string a = argv[i];
        const auto eq = a.find('=');
        names[i] = a.substr(0, eq);
        values[i] = eq == std::string::npos ? "1" : a.substr(eq + 1);
        macros.push_back({names[i].c_str(), values[i].c_str()});
    }

    macros.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(src.data(), src.size(), "shader", macros.data(), nullptr, argv[3], "cs_5_0", 0, 0, &code, &errors);
    std::printf("compile %s hr=0x%08lx size=%u%s%s\n", argv[3], (unsigned long)hr, code ? (unsigned)code->GetBufferSize() : 0u, errors ? "\n  messages: " : "",
        errors ? (const char*)errors->GetBufferPointer() : "");

    if (FAILED(hr)) {
        return 1;
    }

    std::ofstream o(argv[4], std::ios::binary);
    o.write((const char*)code->GetBufferPointer(), (std::streamsize)code->GetBufferSize());
    return 0;
}

// ---- the eye pass's shader (EYE_COMPOSE_SHADER of FoveaComposite.cpp) run on a periphery alone -----------------------
// d3d12::EyeCompose itself is not used here: its root signature (a constant buffer as a root descriptor, a table and a
// static sampler) is refused by vkd3d 1.2 on a Vulkan driver with 8 descriptor sets ("Required descriptor set count
// exceeds maximum allowed count of 8"); Windows takes it. The same shader gets its constant buffer through a table here.
// The block as the shader declares it (struct EyeComposeConstants of FoveaComposite.cpp); its size and the places of its
// members are checked against the shader's reflection below.
struct EyeK {
    uint32_t out_w, out_h, per_w, per_h;
    uint32_t flags, anchor_count, slot;
    float feather;
    float eye_tan[4];
    float wide_tan[4];
    float rect[4];
    int32_t rect_x, rect_y;
    uint32_t rect_w, rect_h;
    uint32_t fov_w, fov_h, gui_w, gui_h, map_w, map_h;
    float eye_x, panel_depth, ui_feather, rate;
    uint32_t parity, map_stamp;
    float post[4];
    float anchors[16][4];
    uint32_t loops[4];
    float light[4];
    float anchor_ext[16][4];
    float post2[4];
    float ui_jit[4]; // [MENUS fix81]
};

static ComPtr<ID3D12Resource> make_buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// periphery: R32G32B32A32_FLOAT resting in UNORDERED_ACCESS; writes the eye image (out_w x out_h, R32G32B32A32_FLOAT)
// [fix95] ui: with the wide view's interface on every pixel (its colour and the copies without / with GUI made here: a
// scene with a black and a nearly black part, light writing, a translucent backdrop, an opaque panel, a green beam; no
// GUI map: every pixel takes the whole path), to compare two texts of the shader on the interface's arithmetic
static int g_eye_map_only = 0; // 1: only the eye pass's case with the GUI map; 2: and only with the periphery image 1x
// [TILE PASS fix105, fix106] 1: the case with the GUI map takes its tiles' interface bits from the shader's tile pass (ui_tiles)
// and runs the eye pass compiled with TILE_BITS, as d3d12::EyeCompose does (fourth argument "tilepass"); the image must be
// the same bytes as without
static int g_eye_tile_pass = 0;
// [MARKED TEXT fix115] a sixth argument "ui_test1" .. "ui_test3": the marked tiles' text with that UI_TEST; "same_depth":
// the first two anchors at one depth with areas that overlap (what UI_TEST 2 changes the reads of)
static int g_marked_ui_test = 0;
static bool g_same_depth = false;
// [SMALL GROUPS fix117] "marked8": the marked tiles' pass in groups of 8 x 8 pixels (its text with TILE_BITS_SHIFT: the bits
// of the eye pass's tile the group lies in)
static bool g_marked8 = false;
// the control "marked8_wrong": the small groups read their own tile's place in the bits instead of the larger tile's
// (TILE_BITS_SHIFT 0); the image must then NOT be the same (it proves the option is live and the shift is needed)
static bool g_marked8_wrong = false;
// [DIRECT fix120] "plain": the marked tiles' pass by the text without the interface's calls (the mod's kind 10: the image
// is then the first pass's). "direct": the same by every thread on its own (MARKED_DIRECT, kind 11): where it gives the
// bytes of "plain", periphery_direct and the tile's taps make the same pixel.
static bool g_marked_plain = false;
static bool g_marked_direct = false;
// [OVER fix121] "over": the marked tiles' text with MARKED_OVER (the mod's kind 12): the interface over the pixel the first
// pass wrote. The harness's eye image is float, so the stored pixel is the composite itself and the image should be the
// full chain's but for the last float digit (in the game the stored pixel has 10 bits).
static bool g_marked_over = false;

static int run_eye_shader(const std::string& hlsl, ID3D12Resource* periphery, UINT out_w, UINT out_h, UINT tile, const std::string& path, int ui = 0) {
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const std::string t = std::to_string(tile);
    // [TILE PASS fix106] the eye pass that takes its tiles' bits from ui_tiles is a text of its own (the macro TILE_BITS)
    const bool tile_bits = ui == 2 && g_eye_tile_pass != 0;
    // [fix108] "tilepass_loop": and the interface's calls behind a loop of 0 or 1 rounds (UI_LOOP), which must give the same bytes
    const bool ui_loop = tile_bits && g_eye_tile_pass == 3;
    // [TWO PASSES fix111] "tilepass_split": the eye image by the text without the interface's calls (UI_NEVER), then the
    // marked tiles again by the text that leaves the others at once (UI_MARKED), as d3d12::EyeCompose::dispatch does
    // [fix112] "tilepass_split2": the same with the first pass leaving the marked tiles to the second (UI_UNMARKED)
    // "tilepass_split2_first": that first pass alone (the marked tiles must be missing: the proof that it leaves them)
    // [fix113] "tilepass_split3": the marked tiles' pass recorded before the eye image's (the mod's order from fix113)
    const bool ui_split2 = tile_bits && (g_eye_tile_pass == 5 || g_eye_tile_pass == 6 || g_eye_tile_pass == 7);
    const bool ui_split = tile_bits && (g_eye_tile_pass == 4 || ui_split2);
    const char* const third = ui_loop ? "UI_LOOP" : ui_split ? "UI_NEVER" : nullptr;
    const D3D_SHADER_MACRO macros[]{{"TSX", t.c_str()}, {"TSY", t.c_str()}, {tile_bits ? "TILE_BITS" : nullptr, tile_bits ? "1" : nullptr},
        {third, third != nullptr ? "1" : nullptr}, {ui_split2 ? "UI_UNMARKED" : nullptr, ui_split2 ? "1" : nullptr}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;

    if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "EyeCompose", macros, nullptr, "main", "cs_5_0", 0, 0, &code, &errors))) {
        std::printf("eye pass shader (groups of %u): not compiled: %s\n", tile, errors ? (const char*)errors->GetBufferPointer() : "?");
        return 1;
    }

    // the constant block's layout
    {
        ComPtr<ID3D12ShaderReflection> refl;
        D3D12_SHADER_BUFFER_DESC bd{};
        ID3D12ShaderReflectionConstantBuffer* cb = nullptr;

        if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&refl))) ||
            (cb = refl->GetConstantBufferByName("Params")) == nullptr || FAILED(cb->GetDesc(&bd)) || bd.Size != sizeof(EyeK)) {
            std::printf("eye pass shader: the constant block is %u bytes, the test's %u\n", bd.Size, (unsigned)sizeof(EyeK));
            return 1;
        }

        const std::pair<const char*, size_t> places[]{{"g_out_size", offsetof(EyeK, out_w)}, {"g_per_size", offsetof(EyeK, per_w)},
            {"g_flags", offsetof(EyeK, flags)}, {"g_feather", offsetof(EyeK, feather)}, {"g_rect_pos", offsetof(EyeK, rect_x)},
            {"g_rate", offsetof(EyeK, rate)}, {"g_post", offsetof(EyeK, post)}, {"g_loops", offsetof(EyeK, loops)},
            {"g_light_p", offsetof(EyeK, light)}, {"g_post2", offsetof(EyeK, post2)}, {"g_ui_jit", offsetof(EyeK, ui_jit)}};

        for (const auto& [name, at] : places) {
            D3D12_SHADER_VARIABLE_DESC vd{};
            auto* v = cb->GetVariableByName(name);

            if (v == nullptr || FAILED(v->GetDesc(&vd)) || vd.StartOffset != at) {
                std::printf("eye pass shader: %s at %u in the shader, %u in the test's block\n", name, vd.StartOffset, (unsigned)at);
                return 1;
            }
        }
    }

    // root signature: a table with the constant buffer, a table with 8 SRVs and 7 UAVs, a linear sampler held at the edge
    D3D12_DESCRIPTOR_RANGE cb_range{};
    cb_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cb_range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 8; // [TILE PASS fix105] u7: the tiles' bits
    ranges[1].OffsetInDescriptorsFromTableStart = 8;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &cb_range;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};

    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors)) ||
        FAILED(g_dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        std::printf("eye pass shader: root signature failed\n");
        return 1;
    }

    pd.pRootSignature = root.Get();
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength = code->GetBufferSize();

    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        std::printf("eye pass shader: pipeline state failed\n");
        return 1;
    }

    ComPtr<ID3D12PipelineState> marked_pso; // [TWO PASSES fix111]

    if (ui_split) {
        const char* const ui_test = g_marked_ui_test == 1 ? "1" : g_marked_ui_test == 2 ? "2" : "3";
        const char* const mt = g_marked8 ? "8" : t.c_str();
        const char* const shift = g_marked8_wrong ? "0" : tile == 32 ? "2" : "1";
        std::vector<D3D_SHADER_MACRO> marked{{"TSX", mt}, {"TSY", mt}, {"TILE_BITS", "1"}, {"UI_MARKED", "1"}};

        if (g_marked_ui_test != 0) {
            marked.push_back({"UI_TEST", ui_test});
        }

        if (g_marked8) {
            marked.push_back({"TILE_BITS_SHIFT", shift});
        }

        if (g_marked_plain || g_marked_direct) {
            marked.push_back({"UI_NEVER", "1"});
        }

        if (g_marked_direct) {
            marked.push_back({"MARKED_DIRECT", "1"});
        }

        if (g_marked_over) {
            marked.push_back({"MARKED_OVER", "1"});
        }

        marked.push_back({nullptr, nullptr});
        ComPtr<ID3DBlob> mcode;

        if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "EyeCompose", marked.data(), nullptr, "main", "cs_5_0", 0, 0, &mcode, &errors))) {
            std::printf("eye pass shader, marked tiles' text: not compiled: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
            return 1;
        }

        pd.CS.pShaderBytecode = mcode->GetBufferPointer();
        pd.CS.BytecodeLength = mcode->GetBufferSize();

        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&marked_pso)))) {
            std::printf("eye pass shader, marked tiles' text: pipeline state failed\n");
            return 1;
        }
    }

    const auto pdesc = periphery->GetDesc();
    auto out = make_tex(out_w, out_h, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true);
    auto gain = make_tex(1024, 2, DXGI_FORMAT_R32_FLOAT, UAV, true);
    auto part = make_tex(256, 1, DXGI_FORMAT_R32_FLOAT, UAV, true);
    auto lsum = make_buffer(16 * 4096, D3D12_HEAP_TYPE_DEFAULT, UAV, true);
    auto light = make_buffer(16 * 4096, D3D12_HEAP_TYPE_DEFAULT, UAV, true);
    auto hist = make_buffer(4 * 4096, D3D12_HEAP_TYPE_DEFAULT, UAV, true);
    auto cbuf = make_buffer(1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);

    EyeK k{};
    k.out_w = out_w;
    k.out_h = out_h;
    k.per_w = (uint32_t)pdesc.Width;
    k.per_h = pdesc.Height;
    k.flags = 8192u; // the periphery's taps from shared memory (where they fit), nothing else: no fovea, interface, post pass
    k.feather = 1.0f;
    k.rate = 0.2f;
    k.post[1] = 1.0f;
    k.post[2] = 1.0f;
    k.post[3] = 1.0f;
    ComPtr<ID3D12Resource> c_tex[3]; // colour after the output pass, copy without GUI, copy with GUI
    // [fix99] ui == 2: with the GUI map as well (the blocks where the copies differ within 2 texels), the tile test, two
    // round anchors (one over the writing, one where nothing is) and an item beam over the green beam: the tiles take
    // the interface at some depths only, to compare two texts of the shader on what they leave out
    ComPtr<ID3D12Resource> c_map;
    std::vector<uint32_t> map_data;

    if (ui != 0) {
        const UINT cw = 160;
        const UINT ch = 120;
        std::vector<float> img[3];

        for (auto& v : img) {
            v.assign((size_t)cw * ch * 4, 1.0f);
        }

        for (UINT y = 0; y < ch; ++y) {
            for (UINT x = 0; x < cw; ++x) {
                float sc = 0.25f + 0.2f * std::sin((float)x * 0.31f) * std::cos((float)y * 0.23f) + 0.1f * (float)(((x / 7) + (y / 5)) & 1u);

                if (x < 24) {
                    sc = 0.0f;
                } else if (x < 40) {
                    sc *= 0.004f;
                }

                float h3[3]{sc, 0.9f * sc, 0.8f * sc};
                float f3[3]{h3[0], h3[1], h3[2]};

                if (y >= 20 && y < 34 && x >= 50 && x < 110 && (x + y) % 3 == 0) { // light writing
                    f3[0] += 0.6f, f3[1] += 0.6f, f3[2] += 0.6f;
                }

                if (y >= 44 && y < 70 && x >= 30 && x < 120) { // a translucent dark backdrop
                    f3[0] *= 0.4f, f3[1] *= 0.4f, f3[2] *= 0.4f;
                }

                if (y >= 80 && y < 100 && x >= 10 && x < 90) { // an opaque black panel (over the black part as well)
                    f3[0] = f3[1] = f3[2] = 0.0f;
                }

                if (x >= 130 && x < 134 && y >= 10 && y < 110) { // a green beam
                    f3[1] += 0.3f;
                }

                for (int c = 0; c < 3; ++c) {
                    const size_t at = ((size_t)y * cw + x) * 4 + c;
                    img[0][at] = std::pow(f3[c], 1.0f / 2.2f);
                    img[1][at] = h3[c];
                    img[2][at] = f3[c];
                }

                if (ui == 2 && (f3[0] != h3[0] || f3[1] != h3[1] || f3[2] != h3[2])) {
                    if (map_data.empty()) {
                        map_data.assign((size_t)(cw / 8) * (ch / 8 + 1), 0u);
                    }

                    for (int dy = -2; dy <= 2; dy += 2) {
                        for (int dx = -2; dx <= 2; dx += 2) {
                            const int bx = std::clamp(((int)x + dx) / 8, 0, (int)cw / 8 - 1);
                            const int by = std::clamp(((int)y + dy) / 8, 0, (int)ch / 8 - 1);
                            map_data[(size_t)by * (cw / 8) + bx] = 7u;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < 3; ++i) {
            c_tex[i] = make_tex(cw, ch, DXGI_FORMAT_R32G32B32A32_FLOAT, NPSR, false);
            upload(c_tex[i].Get(), NPSR, img[i].data(), 16);
        }

        k.flags |= 1u;
        k.eye_tan[0] = -1.0f, k.eye_tan[1] = 1.0f, k.eye_tan[2] = -1.0f, k.eye_tan[3] = 1.0f;
        k.wide_tan[0] = -1.3f, k.wide_tan[1] = 1.3f, k.wide_tan[2] = -1.0f, k.wide_tan[3] = 1.0f;
        k.eye_x = -0.032f;
        k.panel_depth = 1.0f;
        k.gui_w = cw;
        k.gui_h = ch;

        if (ui == 2) {
            // the beam's fitted place in the map's extra row (four texels per anchor: place, -, fitted points, stamp):
            // the green beam's column (0.87), 0.10 (tangent) right of its anchor (0.77): 6 texels of the wide view, outside
            // the anchor's own area (0.06). So the beam is found only at the fitted place and with the fitted area.
            if (map_data.empty()) {
                map_data.assign((size_t)(cw / 8) * (ch / 8 + 1), 0u);
            }

            const float fit_x = 0.87f;
            const float fit_n = 5.0f;
            uint32_t* const fit = map_data.data() + (size_t)(ch / 8) * (cw / 8) + 2 * 4;
            std::memcpy(fit + 0, &fit_x, 4);
            std::memcpy(fit + 2, &fit_n, 4);
            fit[3] = 7u;
            c_map = make_tex(cw / 8, ch / 8 + 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            upload(c_map.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, map_data.data(), 4);
            k.flags |= 128u | 32768u;
            k.map_w = cw / 8;
            k.map_h = ch / 8;
            k.map_stamp = 7;
            k.anchor_count = 3;
            float an[3][4] = {{0.0f, 0.55f, 0.6f, 0.3f}, {-0.9f, -0.8f, 0.5f, 0.2f}, {0.77f, -0.8f, 3.0f, 0.1f}};

            if (g_same_depth) { // [MARKED TEXT fix115]
                an[1][0] = 0.1f;
                an[1][1] = 0.45f;
                an[1][2] = 0.6f;
                an[1][3] = 0.35f;
            }

            const float ex[3][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.06f, 1.7f, 0.1f, 1.0f}};
            std::memcpy(k.anchors, an, sizeof(an));
            std::memcpy(k.anchor_ext, ex, sizeof(ex));
        }
    }

    // [TILE PASS fix105] the tile pass of the same text (its entry ui_tiles), the eye pass told to read its bits
    ComPtr<ID3D12PipelineState> tiles_pso;
    auto tiles = make_tex(2048, 1024, DXGI_FORMAT_R32_UINT, UAV, true);
    const UINT tiles_x = (out_w + tile - 1) / tile;
    const UINT tiles_y = (out_h + tile - 1) / tile;

    if (ui == 2 && g_eye_tile_pass != 0) {
        ComPtr<ID3DBlob> tcode;
        errors.Reset();

        const D3D_SHADER_MACRO tile_entry[]{{"TILE_PASS_ENTRY", "1"}, {nullptr, nullptr}};

        if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "EyeCompose", tile_entry, nullptr, "ui_tiles", "cs_5_0", 0, 0, &tcode, &errors))) {
            std::printf("eye pass shader: tile pass not compiled: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
            return 1;
        }

        pd.CS.pShaderBytecode = tcode->GetBufferPointer();
        pd.CS.BytecodeLength = tcode->GetBufferSize();

        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&tiles_pso)))) {
            std::printf("eye pass shader: tile pass pipeline state failed\n");
            return 1;
        }

        k.loops[2] = tile;
        k.loops[3] = tile;
    }

    void* mapped = nullptr;
    cbuf->Map(0, nullptr, &mapped);
    std::memset(mapped, 0, 1024);
    std::memcpy(mapped, &k, sizeof(k));
    cbuf->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 17;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    const UINT step = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = [&](UINT i) {
        auto h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * step;
        return h;
    };
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv{};
    cv.BufferLocation = cbuf->GetGPUVirtualAddress();
    cv.SizeInBytes = 1024;
    g_dev->CreateConstantBufferView(&cv, at(0));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    g_dev->CreateShaderResourceView(periphery, &sv, at(1));
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    for (UINT i = 1; i < 8; ++i) { // (fovea, the views' colour and GUI copies, the output curve: none)
        g_dev->CreateShaderResourceView(nullptr, &sv, at(1 + i));
    }

    if (ui) {
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

        for (UINT i = 0; i < 3; ++i) { // t2, t3, t4
            g_dev->CreateShaderResourceView(c_tex[i].Get(), &sv, at(3 + i));
        }
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    g_dev->CreateUnorderedAccessView(out.Get(), nullptr, &uv, at(9));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(gain.Get(), nullptr, &uv, at(10));
    uv.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(nullptr, nullptr, &uv, at(11)); // (no GUI map)

    if (c_map != nullptr) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC mv{};
        mv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        mv.Format = DXGI_FORMAT_R32_UINT;
        g_dev->CreateUnorderedAccessView(c_map.Get(), nullptr, &mv, at(11));
    }
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(part.Get(), nullptr, &uv, at(12));
    D3D12_UNORDERED_ACCESS_VIEW_DESC bv{};
    bv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bv.Format = DXGI_FORMAT_UNKNOWN;
    bv.Buffer.StructureByteStride = 16;
    bv.Buffer.NumElements = 4096;
    g_dev->CreateUnorderedAccessView(lsum.Get(), nullptr, &bv, at(13));
    g_dev->CreateUnorderedAccessView(light.Get(), nullptr, &bv, at(14));
    D3D12_UNORDERED_ACCESS_VIEW_DESC rv{};
    rv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rv.Format = DXGI_FORMAT_R32_TYPELESS;
    rv.Buffer.NumElements = 4096;
    rv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(hist.Get(), nullptr, &rv, at(15));
    uv.Format = DXGI_FORMAT_R32_UINT; // [TILE PASS fix105] u7
    g_dev->CreateUnorderedAccessView(tiles.Get(), nullptr, &uv, at(16));

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = periphery;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = UAV;
    b.Transition.StateAfter = NPSR;
    g_cmd->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetComputeRootSignature(root.Get());
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    g_cmd->SetComputeRootDescriptorTable(0, table);
    table.ptr += step;
    g_cmd->SetComputeRootDescriptorTable(1, table);

    // [TILE PASS fix105] as d3d12::EyeCompose::dispatch: the tiles' bits first. The control ("tilepass_blank") leaves this pass out
    // while the eye pass still reads its bits: the image must then NOT be the same (it proves the eye pass does read them)
    if (tiles_pso != nullptr && (g_eye_tile_pass == 1 || g_eye_tile_pass >= 3)) {
        g_cmd->SetPipelineState(tiles_pso.Get());
        g_cmd->Dispatch((tiles_x + 7) / 8, (tiles_y + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        tb.UAV.pResource = tiles.Get();
        g_cmd->ResourceBarrier(1, &tb);
    }

    const UINT mtile = g_marked8 ? 8u : tile; // [SMALL GROUPS fix117]

    if (marked_pso != nullptr && g_eye_tile_pass == 7) { // [MARKED FIRST fix113]
        g_cmd->SetPipelineState(marked_pso.Get());
        g_cmd->Dispatch((out_w + mtile - 1) / mtile, (out_h + mtile - 1) / mtile, 1);
    }

    g_cmd->SetPipelineState(pso.Get());
    g_cmd->Dispatch((out_w + tile - 1) / tile, (out_h + tile - 1) / tile, 1);

    if (marked_pso != nullptr && g_eye_tile_pass != 6 && g_eye_tile_pass != 7) { // [TWO PASSES fix111] the marked tiles again, over the first pass's pixels
        D3D12_RESOURCE_BARRIER ob{};
        ob.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ob.UAV.pResource = out.Get();

        if (!ui_split2) { // ([fix112] different tiles: no order needed, as in the mod)
            g_cmd->ResourceBarrier(1, &ob);
        }

        g_cmd->SetPipelineState(marked_pso.Get());
        g_cmd->Dispatch((out_w + mtile - 1) / mtile, (out_h + mtile - 1) / mtile, 1);
    }

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_cmd->ResourceBarrier(1, &b);
    run_list();
    download(out.Get(), UAV, path, 16);
    return 0;
}

// the wide view of the test: distance (m) of what pixel (x, y) of the grid shows
static float scene_distance(int x, int y, int W, int H) {
    float d = 6.0f; // a wall

    if (y >= 2 * H / 3) { // a floor coming nearer towards the lower edge
        d = 6.0f - 4.5f * (float)(y - 2 * H / 3) / (float)(H / 3);
    }

    if (x >= 3 * W / 8 && x < 17 * W / 32 && y >= H / 4 && y < 5 * H / 8) { // a near box (a hand)
        d = 0.45f;
    }

    if (x >= 23 * W / 32 && x < 23 * W / 32 + 2 && y >= H / 8 && y < 7 * H / 8) { // a thin pole
        d = 0.9f;
    }

    if (x < W / 26 && y >= 5 * H / 12 && y < 5 * H / 6) { // a near thing at the left border
        d = 0.3f;
    }

    if (x >= W - W / 32 && y >= H / 6 && y < H / 2) { // ... and one at the right border
        d = 0.35f;
    }

    return d;
}

// [fix81 17.11.2026] A sequence of frames through the reprojection, as the game runs it every frame:
//   periphery_test.exe seq <dir>
// <dir>/seq.txt: "key value" pairs: w h (the wide view's grid), ss (1 or 2: the colour image's size per axis), frames,
//   ew eh (rays of the eye's periphery), x_scale x_offset y_scale y_offset disparity dist_min max_shift fill (0 / 1)
// per frame k: c%03d.bin the wide view's image (w ss x h ss, 4 floats per pixel), d%03d.bin the distance in metres at
//   every pixel of the grid (w x h floats: what the pass is given as depth, with the constants that make it the distance)
// writes e%03d.bin: the left eye's periphery (ew ss x eh ss, 4 floats per pixel; alpha 1 = a surface found, 0 = a gap)
// "stable 1": the distances go through d3d12::StableDepth first, as in the game (m%03d.bin: the motion in pixels of the
//   grid to the place a frame before, 2 floats per pixel; rate step0 step1 hyst when given); its image is written too
//   (s%03d.bin, 4 half floats per pixel: 1 / distance handed on, the mean; [fix84] the slower mean, and it held still)
// "given 1" (with "stable 1") [fix85]: the stable depth's image is not computed but read (g%03d.bin, 4 half floats per
//   pixel, as the game's whole-chain dumps hold it: C_2s_wide_stable_depth_raw_*); d and m are not read then
static int run_seq(const std::string& dir) {
    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::map<std::string, double> k{};

    {
        std::ifstream f(dir + "/seq.txt");
        std::string name;
        double value = 0.0;

        while (f >> name >> value) {
            k[name] = value;
        }
    }

    const UINT w = (UINT)k["w"];
    const UINT h = (UINT)k["h"];
    const UINT ss = k["ss"] >= 2.0 ? 2u : 1u;
    const UINT frames = (UINT)k["frames"];
    const UINT ew = (UINT)k["ew"];
    const UINT eh = (UINT)k["eh"];

    if (w == 0 || h == 0 || frames == 0 || ew == 0 || eh == 0) {
        std::printf("seq: %s/seq.txt not read or incomplete\n", dir.c_str());
        return 2;
    }

    MonoPeriphery::Params p[2]{};

    for (auto& e : p) {
        e.x_scale = (float)k["x_scale"];
        e.x_offset = (float)k["x_offset"];
        e.y_scale = (float)k["y_scale"];
        e.y_offset = (float)k["y_offset"];
        e.disparity = (float)k["disparity"];
        e.dist_min = (float)k["dist_min"];
        e.max_shift = (uint32_t)k["max_shift"];
        e.depth_a = 1.0f; // distance = |(a z + b) / (c z + e)| = z
        e.depth_b = 0.0f;
        e.depth_c = 0.0f;
        e.depth_e = 1.0f;
        e.fill_anchored = k["fill"] != 0.0;
        e.ss = ss;
        e.soft_outlines = k.count("soft") != 0 && k["soft"] != 0.0; // [OUTLINES fix84] (needs "stable 1" and ss 2)
    }

    p[1].disparity = -p[1].disparity;
    MonoPeriphery mono{};
    const bool stable = k["stable"] != 0.0;
    StableDepth stable_depth{};
    StableDepth::Params sp{};
    sp.mv_scale_x = 1.0f / (float)w; // (the motion is given in pixels)
    sp.mv_scale_y = 1.0f / (float)h;
    sp.depth_a = 1.0f;
    sp.depth_b = 0.0f;
    sp.depth_c = 0.0f;
    sp.depth_e = 1.0f;
    sp.dist_min = p[0].dist_min;

    if (k.count("rate") != 0) {
        sp.rate = (float)k["rate"];
    }

    if (k.count("step0") != 0) {
        sp.step0 = (float)k["step0"];
    }

    if (k.count("step1") != 0) {
        sp.step1 = (float)k["step1"];
    }

    if (k.count("hyst") != 0) {
        sp.hysteresis = (float)k["hyst"];
    }

    if (stable) {
        for (auto& e : p) {
            StableDepth::as_depth_of(e);
        }
    }

    if (ss == 2 && !mono.ss_ready(g_dev.Get())) {
        std::printf("seq: the passes for ss 2 are not usable: %s\n", mono.last_error());
        return 1;
    }

    auto color = make_tex(w * ss, h * ss, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true);
    auto depth = make_tex(w, h, DXGI_FORMAT_R32_FLOAT, PSR, false);
    ComPtr<ID3D12Resource> out[2]{make_tex(ew * ss, eh * ss, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true),
        make_tex(ew * ss, eh * ss, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true)};
    ID3D12Resource* const outs[2]{out[0].Get(), out[1].Get()};
    std::vector<float> cbuf((size_t)w * ss * h * ss * 4);
    std::vector<float> dbuf((size_t)w * h);
    auto motion = make_tex(w, h, DXGI_FORMAT_R32G32_FLOAT, PSR, false);
    std::vector<float> mbuf((size_t)w * h * 2);
    char name[64];
    const bool given = stable && k.count("given") != 0 && k["given"] != 0.0; // [fix85]
    auto given_tex = make_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, PSR, false);
    std::vector<uint16_t> gbuf((size_t)w * h * 4);

    for (UINT f = 0; f < frames; ++f) {
        std::snprintf(name, sizeof(name), "/c%03u.bin", f);
        std::ifstream fc(dir + name, std::ios::binary);
        fc.read((char*)cbuf.data(), (std::streamsize)(cbuf.size() * sizeof(float)));
        std::snprintf(name, sizeof(name), given ? "/g%03u.bin" : "/d%03u.bin", f);
        std::ifstream fd(dir + name, std::ios::binary);

        if (given) {
            fd.read((char*)gbuf.data(), (std::streamsize)(gbuf.size() * sizeof(uint16_t)));
        } else {
            fd.read((char*)dbuf.data(), (std::streamsize)(dbuf.size() * sizeof(float)));
        }

        if (!fc || !fd) {
            std::printf("seq: frame %u not read\n", f);
            return 2;
        }

        upload(color.Get(), UAV, cbuf.data(), 16);
        upload(depth.Get(), PSR, dbuf.data(), 4);
        ID3D12Resource* depth_in = depth.Get();
        auto depth_state = PSR;

        if (given) {
            upload(given_tex.Get(), PSR, gbuf.data(), 8);
            depth_in = given_tex.Get();
        } else if (stable) {
            std::snprintf(name, sizeof(name), "/m%03u.bin", f);
            std::ifstream fm(dir + name, std::ios::binary);
            fm.read((char*)mbuf.data(), (std::streamsize)(mbuf.size() * sizeof(float)));

            if (!fm) {
                std::printf("seq: the motion of frame %u not read\n", f);
                return 2;
            }

            upload(motion.Get(), PSR, mbuf.data(), 8);

            if (!stable_depth.dispatch(g_dev.Get(), g_cmd.Get(), depth.Get(), PSR, motion.Get(), PSR, sp) || stable_depth.image() == nullptr) {
                std::printf("seq: StableDepth FAILED at frame %u (%s)\n", f, stable_depth.last_error());
                return 1;
            }

            depth_in = stable_depth.image();
            depth_state = UAV;
        }

        if (!mono.reproject(g_dev.Get(), g_cmd.Get(), color.Get(), UAV, depth_in, depth_state, outs, p, MonoPeriphery::Search::TwoPassRound3, (uint64_t)f + 1,
                [](int) {})) {
            std::printf("seq: reproject FAILED at frame %u (%s)\n", f, mono.last_error());
            return 1;
        }

        run_list();
        std::snprintf(name, sizeof(name), "/e%03u.bin", f);
        download(out[0].Get(), UAV, dir + name, 16);

        if (stable && !given) {
            std::snprintf(name, sizeof(name), "/s%03u.bin", f);
            download(stable_depth.image(), UAV, dir + name, 8); // ([fix84] four half floats per pixel)
        }
    }

    std::printf("done: %u frames, wide view %ux%u (image x%u), eye %ux%u rays\n", frames, w, h, ss, ew, eh);
    return 0;
}

// [MENUS fix81 17.11.2026] Frames through the game's interface cleaning (d3d12::UiClean):
//   periphery_test.exe uiclean <dir>
// <dir>/ui.txt: "key value" pairs: w h frames, fovea (1: the interface found from the colour, a fovea view; 0: from the two
//   copies, the wide view), lds (1: the pass with the curve table in shared memory)
// per frame k: c%03d.bin the view's colour (display values, 4 floats per pixel), h%03d.bin its copy without GUI (linear;
//   the plain SRGB curve stands for the output pass), f%03d.bin the copy with GUI (fovea 0)
// writes o%03d.bin (the colour without the interface, 4 floats per pixel)
// [NO INTERFACE fix118] "map 1" (the wide view only): every frame a second time with d3d12::GuiMap made from the two copies
// first and given to the pass (which then leaves before reading the copies in a frame without interface): p%03d.bin, which
// must hold the same bytes as o%03d.bin; printed per frame. "map 2" is the control: the pass gets a stamp that is not the
// frame's, so it takes every frame for one without interface: the frames WITH interface must then come out different.
// [BLOCKS fix120] "map 3" is the control of the per-block leaving: the map is made from the frame BEFORE (with the stamp
// the pass gets), so a frame whose interface lies elsewhere than the one before must come out different there.
// ([fix82] fix81's image for the eye pass is gone: the light layer and the matte let the hidden scene through on the game's menus)
static int run_uiclean(const std::string& dir) {
    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto COMMON = D3D12_RESOURCE_STATE_COMMON;
    std::map<std::string, double> k{};

    {
        std::ifstream f(dir + "/ui.txt");
        std::string name;
        double value = 0.0;

        while (f >> name >> value) {
            k[name] = value;
        }
    }

    const UINT w = (UINT)k["w"];
    const UINT h = (UINT)k["h"];
    const UINT frames = (UINT)k["frames"];
    const bool fovea = k["fovea"] != 0.0;

    if (w == 0 || h == 0 || frames == 0) {
        std::printf("uiclean: %s/ui.txt not read or incomplete\n", dir.c_str());
        return 2;
    }

    d3d12::UiClean clean{};
    d3d12::GuiMap gui_map{};          // [NO INTERFACE fix118]
    const int with_map = (int)k["map"];
    auto color = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, PSR, false);
    auto hud = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, COMMON, false);
    auto fin = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, COMMON, false);
    auto out = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true);
    std::vector<float> buf((size_t)w * h * 4);
    char name[64];

    for (UINT f = 0; f < frames; ++f) {
        // [BLOCKS fix120] the control "map 3": the map from the copies of the frame before (still in the textures)
        if (with_map == 3 && !fovea && f > 0) {
            if (!gui_map.dispatch(g_dev.Get(), g_cmd.Get(), hud.Get(), fin.Get())) {
                std::printf("uiclean: GuiMap FAILED before frame %u (%s)\n", f, gui_map.last_error());
                return 1;
            }

            run_list();
        }

        for (const char* what : {"c", "h", "f"}) {
            if (what[0] == 'f' && fovea) {
                continue;
            }

            std::snprintf(name, sizeof(name), "/%s%03u.bin", what, f);
            std::ifstream in(dir + name, std::ios::binary);
            in.read((char*)buf.data(), (std::streamsize)(buf.size() * sizeof(float)));

            if (!in) {
                std::printf("uiclean: %s not read\n", name);
                return 2;
            }

            if (what[0] == 'c') {
                upload(color.Get(), PSR, buf.data(), 16);
            } else {
                upload(what[0] == 'h' ? hud.Get() : fin.Get(), COMMON, buf.data(), 16);
            }
        }

        if (!clean.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), PSR, hud.Get(), fovea ? nullptr : fin.Get(), out.Get(), nullptr, fovea, k["lds"] != 0.0,
                nullptr, 0)) {
            std::printf("uiclean: FAILED at frame %u (%s)\n", f, clean.last_error());
            return 1;
        }

        run_list();
        std::snprintf(name, sizeof(name), "/o%03u.bin", f);
        download(out.Get(), UAV, dir + name, 16);

        if (with_map != 0 && !fovea) {
            if ((with_map != 3 || f == 0) && !gui_map.dispatch(g_dev.Get(), g_cmd.Get(), hud.Get(), fin.Get())) {
                std::printf("uiclean: GuiMap FAILED at frame %u (%s)\n", f, gui_map.last_error());
                return 1;
            }

            if (!clean.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), PSR, hud.Get(), fin.Get(), out.Get(), nullptr, false, k["lds"] != 0.0,
                    gui_map.map(), gui_map.stamp() + (with_map == 2 ? 1000u : 0u))) {
                std::printf("uiclean: FAILED with the map at frame %u (%s)\n", f, clean.last_error());
                return 1;
            }

            run_list();
            std::snprintf(name, sizeof(name), "/p%03u.bin", f);
            download(out.Get(), UAV, dir + name, 16);

            std::vector<char> a{};
            std::vector<char> b{};

            for (int which = 0; which < 2; ++which) {
                std::snprintf(name, sizeof(name), "/%s%03u.bin", which == 0 ? "o" : "p", f);
                std::ifstream in(dir + name, std::ios::binary);
                (which == 0 ? a : b).assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }

            size_t differ = 0;

            for (size_t i = 0; i + 16 <= a.size() && i + 16 <= b.size(); i += 16) {
                differ += std::memcmp(a.data() + i, b.data() + i, 16) != 0 ? 1 : 0;
            }

            std::printf("uiclean: frame %u with the map: %zu of %zu pixels differ (sizes %zu / %zu)\n", f, differ, a.size() / 16, a.size(), b.size());
        }
    }

    std::printf("done: %u frames, %ux%u, %s\n", frames, w, h, fovea ? "a fovea view" : "the wide view");
    return 0;
}

// [fix86] "uisteady <dir>": d3d12::UiSteady on a sequence of frames. <dir>/steady.txt: w h frames, mv_scale_x mv_scale_y
// (motion value -> UV offset to the place a frame before), weight_max sigma motion_half (0 or absent: the pass's own),
// srgb (1: the colour is given as an sRGB image, 8 bits). Per frame: c%03u.bin, h%03u.bin, f%03u.bin (w x h x 4 floats:
// the colour after the output pass, the copies without / with GUI; with srgb 1 the colour as w x h x 4 bytes),
// m%03u.bin (w x h x 2 floats: the motion vectors), and a line "jitter <frame> <x> <y>" in steady.txt (pixels; a point of
// the steady image at p is at p + jitter in the frame). Written per frame: sc / sh / sf%03u.bin (2w x 2h, 4 half floats).
// "map 1": d3d12::GuiMap runs on the copies first and the pass marks its blocks as well; written per frame: gm%03u.bin (the
// map after GuiMap alone) and gs%03u.bin (after the pass): one uint32 per block of 8 x 8 pixels, a row more than there
// are blocks; the blocks marked in frame f hold f + 1.
// [fix94] "around 1" (with "map 1"): Params::only_around, from frame "around_from" to before "around_to" (0: to the end);
// "around_hold": Params::around_hold; "reset_at": that frame starts without history as well.
static int run_uisteady(const std::string& dir) {
    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto COMMON = D3D12_RESOURCE_STATE_COMMON;
    std::map<std::string, double> k{};
    std::map<UINT, std::pair<float, float>> jitter{};

    {
        std::ifstream f(dir + "/steady.txt");
        std::string name;

        while (f >> name) {
            if (name == "jitter") {
                UINT frame = 0;
                float x = 0.0f;
                float y = 0.0f;
                f >> frame >> x >> y;
                jitter[frame] = {x, y};
            } else {
                double value = 0.0;
                f >> value;
                k[name] = value;
            }
        }
    }

    const UINT w = (UINT)k["w"];
    const UINT h = (UINT)k["h"];
    const UINT frames = (UINT)k["frames"];
    const bool srgb = k["srgb"] != 0.0;

    if (w == 0 || h == 0 || frames == 0) {
        std::printf("uisteady: %s/steady.txt not read or incomplete\n", dir.c_str());
        return 2;
    }

    d3d12::UiSteady steady{};
    d3d12::GuiMap gui_map{};
    const bool with_map = k["map"] != 0.0;
    auto color = make_tex(w, h, srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R32G32B32A32_FLOAT, PSR, false);
    auto hud = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, COMMON, false);
    auto fin = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, COMMON, false);
    auto motion = make_tex(w, h, DXGI_FORMAT_R32G32_FLOAT, PSR, false);
    std::vector<float> buf((size_t)w * h * 4);
    std::vector<uint8_t> buf8((size_t)w * h * 4);
    std::vector<float> mbuf((size_t)w * h * 2);
    char name[64];

    for (UINT f = 0; f < frames; ++f) {
        for (const char* what : {"c", "h", "f", "m"}) {
            std::snprintf(name, sizeof(name), "/%s%03u.bin", what, f);
            std::ifstream in(dir + name, std::ios::binary);

            if (what[0] == 'm') {
                in.read((char*)mbuf.data(), (std::streamsize)(mbuf.size() * sizeof(float)));
            } else if (what[0] == 'c' && srgb) {
                in.read((char*)buf8.data(), (std::streamsize)buf8.size());
            } else {
                in.read((char*)buf.data(), (std::streamsize)(buf.size() * sizeof(float)));
            }

            if (!in) {
                std::printf("uisteady: %s not read\n", name);
                return 2;
            }

            if (what[0] == 'm') {
                upload(motion.Get(), PSR, mbuf.data(), 8);
            } else if (what[0] == 'c') {
                upload(color.Get(), PSR, srgb ? (const void*)buf8.data() : (const void*)buf.data(), srgb ? 4 : 16);
            } else {
                upload(what[0] == 'h' ? hud.Get() : fin.Get(), COMMON, buf.data(), 16);
            }
        }

        d3d12::UiSteady::Params p{};
        p.mv_scale_x = (float)k["mv_scale_x"];
        p.mv_scale_y = (float)k["mv_scale_y"];
        p.jitter_x = jitter[f].first;
        p.jitter_y = jitter[f].second;
        p.reset = f == 0 || (k["reset_at"] > 0.0 && f == (UINT)k["reset_at"]);
        // [fix94] "around 1": only the blocks around the interface, from frame "around_from" to before "around_to" (0: to the end)
        p.only_around = k["around"] != 0.0 && f >= (UINT)k["around_from"] && (k["around_to"] == 0.0 || f < (UINT)k["around_to"]);
        p.around_hold = (uint32_t)k["around_hold"];

        if (k["weight_max"] > 0.0) {
            p.weight_max = (float)k["weight_max"];
        }

        if (k["sigma"] > 0.0) {
            p.sigma = (float)k["sigma"];
        }

        if (k["motion_half"] > 0.0) {
            p.motion_half = (float)k["motion_half"];
        }

        // [UI MOTION fix125] "ui_motion 1": every frame brings the block of the interface's own motion (umNNN.bin:
        // d3d12::UiMotion::Constants as it is); the pass must run with it
        static d3d12::UiMotion um_blocks;

        if (k["ui_motion"] != 0.0) {
            std::snprintf(name, sizeof(name), "/um%03u.bin", f);
            std::ifstream um_in(dir + name, std::ios::binary);
            d3d12::UiMotion::Constants uk{};
            um_in.read((char*)&uk, sizeof(uk));

            if (!um_in) {
                std::printf("uisteady: %s not read\n", name);
                return 2;
            }

            p.ui_motion = um_blocks.write(g_dev.Get(), uk);
        }

        if (with_map) {
            if (!gui_map.dispatch(g_dev.Get(), g_cmd.Get(), hud.Get(), fin.Get())) {
                std::printf("uisteady: GuiMap FAILED at frame %u (%s)\n", f, gui_map.last_error());
                return 1;
            }

            run_list();
            std::snprintf(name, sizeof(name), "/gm%03u.bin", f);
            download(gui_map.map(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dir + name, 4);
        }

        if (!steady.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), PSR, hud.Get(), fin.Get(), motion.Get(), PSR, with_map ? &gui_map : nullptr, p) ||
            steady.color() == nullptr) {
            std::printf("uisteady: FAILED at frame %u (%s)\n", f, steady.last_error());
            return 1;
        }

        run_list();
        std::snprintf(name, sizeof(name), "/sc%03u.bin", f);
        download(steady.color(), PSR, dir + name, 8);
        std::snprintf(name, sizeof(name), "/sh%03u.bin", f);
        download(steady.hud(), d3d12::UiSteady::COPIES_STATE, dir + name, 8);
        std::snprintf(name, sizeof(name), "/sf%03u.bin", f);
        download(steady.fin(), d3d12::UiSteady::COPIES_STATE, dir + name, 8);

        if (with_map) {
            std::snprintf(name, sizeof(name), "/gs%03u.bin", f);
            download(gui_map.map(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dir + name, 4);
        }
    }

    std::printf("done: %u frames, %ux%u -> %ux%u\n", frames, w, h, w * d3d12::UiSteady::SCALE, h * d3d12::UiSteady::SCALE);
    return 0;
}

// [fix100 20.11.2026] calm <dir>: d3d12::LightTAA over a sequence, as the mod runs it on the shared wide view (two
// histories R16G16B16A16_FLOAT swapped; with `calm 1` the calm variant and its second history). <dir>/calm.txt: w, h,
// frames, calm, feedback, tiled, tile, fresh_at, and a line "jitter <frame> <x> <y>" per frame; c%03u.bin (RGBA32F), m%03u.bin
// (RG32F, pixels to the previous place), d%03u.bin (R32F, inverted depth). Writes o%03u.bin (the history, RGBA16F) and,
// with calm, a%03u.bin (the second history). Compared with the reference model by calm/compare.py.
// (NO_CALM: built against a tree before fix100, to compare the light TAA's own image before and after.)
// [SHADER FILES fix110 20.11.2026] the compiled shaders kept in files: the mod's eye pass is set up with the folder given
// (d3d12::EyeCompose compiles its texts through compile_cached). Run twice: the second run must read every shader from
// the files ("0 compiled") and make its pipelines from them.
static int run_cachetest(const std::string& dir) {
    d3d12::set_shader_cache_dir(dir);
    const auto t0 = std::chrono::steady_clock::now();
    d3d12::EyeCompose eye{};
    const bool ok = eye.prepare_dense(g_dev.Get());
    // (Wine's software device refuses the eye pass's root signature, which comes after its shaders: what counts here is
    // the "[ShaderFiles] ... compiled in" lines, six in the first run and none in the second, and the time)
    std::printf("cachetest: eye pass set-up %s after %.1f s\n", ok ? "done" : "stopped (after the shaders, on this device)",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

static int run_calm(const std::string& dir) {
    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::map<std::string, double> k{};
    std::map<UINT, std::pair<float, float>> jitter{};
    {
        std::ifstream f(dir + "/calm.txt");
        std::string name;

        while (f >> name) {
            if (name == "jitter") {
                UINT frame = 0;
                float x = 0.0f;
                float y = 0.0f;
                f >> frame >> x >> y;
                jitter[frame] = {x, y};
            } else {
                double value = 0.0;
                f >> value;
                k[name] = value;
            }
        }
    }

    const UINT w = (UINT)k["w"];
    const UINT h = (UINT)k["h"];
    const UINT frames = (UINT)k["frames"];

    if (w == 0 || h == 0 || frames == 0) {
        std::printf("calm: %s/calm.txt not read or incomplete\n", dir.c_str());
        return 2;
    }

    d3d12::LightTAA taa{};
    auto color = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, PSR, false);
    auto motion = make_tex(w, h, DXGI_FORMAT_R32G32_FLOAT, PSR, false);
    auto depth = make_tex(w, h, DXGI_FORMAT_R32_FLOAT, PSR, false);
    ComPtr<ID3D12Resource> hist[2]{make_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true), make_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true)};
    ComPtr<ID3D12Resource> aux[2]{make_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true), make_tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, UAV, true)};
    std::vector<float> buf((size_t)w * h * 4);
    char name[64];
    UINT cur = 0;

    for (UINT f = 0; f < frames; ++f) {
        for (const char* what : {"c", "m", "d"}) {
            std::snprintf(name, sizeof(name), "/%s%03u.bin", what, f);
            std::ifstream in(dir + name, std::ios::binary);
            const UINT channels = what[0] == 'c' ? 4u : what[0] == 'm' ? 2u : 1u;
            in.read((char*)buf.data(), (std::streamsize)((size_t)w * h * channels * sizeof(float)));

            if (!in) {
                std::printf("calm: %s not read\n", name);
                return 2;
            }

            upload(what[0] == 'c' ? color.Get() : what[0] == 'm' ? motion.Get() : depth.Get(), PSR, buf.data(), channels * 4);
        }

        d3d12::LightTAA::Params tp{};
        tp.mv_scale_x = 1.0f / (float)w;
        tp.mv_scale_y = 1.0f / (float)h;
        tp.feedback = (float)k["feedback"];
        tp.reset = f == 0;
        tp.dilate = true;
        tp.reverse_z = true;
        tp.jitter_x = jitter[f].first;
        tp.jitter_y = jitter[f].second;
        tp.reconstruct = true;
        tp.anti_flicker = true;
        tp.tiled = k["tiled"] != 0.0;
        tp.tile = (uint32_t)k["tile"];
#ifndef NO_CALM
        tp.calm = k["calm"] != 0.0;
        tp.calm_fresh = k.count("fresh_at") != 0 && (int)k["fresh_at"] == (int)f; // [fix101] the statistics start from zero
        tp.aux_in = aux[cur].Get();
        tp.aux_out = aux[cur ^ 1u].Get();
#endif

        if (!taa.dispatch(g_dev.Get(), g_cmd.Get(), color.Get(), motion.Get(), depth.Get(), PSR, hist[cur].Get(), hist[cur ^ 1u].Get(), tp)) {
            std::printf("calm: frame %u: %s\n", f, taa.last_error());
            return 1;
        }

        run_list();
        cur ^= 1u;
        std::snprintf(name, sizeof(name), "/o%03u.bin", f);
        download(hist[cur].Get(), UAV, dir + name, 8);

        if (k["calm"] != 0.0) {
            std::snprintf(name, sizeof(name), "/a%03u.bin", f);
            download(aux[cur].Get(), UAV, dir + name, 8);
        }
    }

    std::printf("calm: %u frames %ux%u, calm %d, tiled %d (%u)\n", frames, w, h, (int)(k["calm"] != 0.0), (int)(k["tiled"] != 0.0), (UINT)k["tile"]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 3) {
        g_eye_map_only = std::string(argv[3]) == "map" ? 1 : std::string(argv[3]) == "map1" ? 2 : 0;
    }

    if (argc > 4) {
        g_eye_tile_pass = std::string(argv[4]) == "tilepass" ? 1 : std::string(argv[4]) == "tilepass_blank" ? 2 : std::string(argv[4]) == "tilepass_loop" ? 3 : std::string(argv[4]) == "tilepass_split" ? 4 : std::string(argv[4]) == "tilepass_split2" ? 5 : std::string(argv[4]) == "tilepass_split2_first" ? 6 : std::string(argv[4]) == "tilepass_split3" ? 7 : 0;

        for (int a = 5; a < argc; ++a) {
            const std::string opt = argv[a];
            g_marked_ui_test = opt == "ui_test1" ? 1 : opt == "ui_test2" ? 2 : opt == "ui_test3" ? 3 : g_marked_ui_test;
            g_same_depth = g_same_depth || opt == "same_depth";
            g_marked8 = g_marked8 || opt == "marked8" || opt == "marked8_wrong";
            g_marked8_wrong = g_marked8_wrong || opt == "marked8_wrong";
            g_marked_plain = g_marked_plain || opt == "plain";
            g_marked_direct = g_marked_direct || opt == "direct";
            g_marked_over = g_marked_over || opt == "over";
        }
    }

    if (argc >= 2 && std::string{argv[1]} == "compile") {
        return compile_to_file(argc, argv);
    }

    if (argc < 2 || ((std::string{argv[1]} == "seq" || std::string{argv[1]} == "uiclean" || std::string{argv[1]} == "uisteady" || std::string{argv[1]} == "cachetest") && argc < 3)) {
        std::printf("usage: periphery_test.exe <out dir> [eye pass shader text] | seq <dir> | uiclean <dir> | uisteady <dir> | compile <hlsl file> <entry> "
                    "<out file> [NAME | NAME=VALUE ...]\n");
        return 2;
    }

    const std::string dir = argv[1];

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

    if (dir == "seq") {
        return run_seq(argv[2]);
    }

    if (dir == "uiclean") {
        return run_uiclean(argv[2]);
    }

    if (dir == "uisteady") {
        return run_uisteady(argv[2]);
    }

    if (dir == "calm") {
        return run_calm(argv[2]);
    }

    if (dir == "cachetest") {
        return run_cachetest(argv[2]);
    }

    constexpr auto PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr UINT W = 320;
    constexpr UINT H = 240;

    // depth: the value z with distance 1 / z (the pass's distance is |(a z + b) / (c z + e)|: a 0, b 1, c 1, e 0)
    std::vector<float> z((size_t)W * H);

    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            z[(size_t)y * W + x] = 1.0f / scene_distance((int)x, (int)y, (int)W, (int)H);
        }
    }

    auto depth = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, PSR, false);
    upload(depth.Get(), PSR, z.data(), 4);

    // colour at the grid's size and at twice it: red = u, green = v of the texel's centre (a ramp: a bilinear read returns
    // the place it read), blue = a pattern
    auto make_color = [&](UINT k) {
        const UINT w = W * k;
        const UINT h = H * k;
        std::vector<float> c((size_t)w * h * 4);

        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const float u = ((float)x + 0.5f) / (float)w;
                const float v = ((float)y + 0.5f) / (float)h;
                float* p = &c[((size_t)y * w + x) * 4];
                p[0] = u;
                p[1] = v;
                // (squares of 3 columns and rows of the grid: fine detail for the eye pass's enlargement, the same picture
                // at both sizes)
                p[2] = (((int)std::floor(u * (float)W / 3.0f) + (int)std::floor(v * (float)H / 3.0f)) & 1) != 0 ? 0.9f : 0.1f;
                p[3] = 1.0f;
            }
        }

        auto t = make_tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true);
        upload(t.Get(), UAV, c.data(), 16);
        return t;
    };

    auto color1 = make_color(1);
    auto color2 = make_color(2);

    // the frusta as the mod sets them up (tangents): the shared wide view and each eye's share of it (the wide view is the
    // union of the two: each eye's outer edge is the wide view's, where near things need columns past the image)
    const float wide_l = -1.715f, wide_r = 1.715f;
    const float eye_l[2]{-1.715f, -0.885f};
    const float eye_r[2]{0.885f, 1.715f};
    const float eye_x[2]{-0.032f, 0.032f};
    const float px_per_tan_x = (float)W / (wide_r - wide_l);
    MonoPeriphery::Params p[2]{};
    UINT ew[2]{};
    const UINT eh = H;

    for (int eye = 0; eye < 2; ++eye) {
        const float eye_tan_w = eye_r[eye] - eye_l[eye];
        ew[eye] = (UINT)std::lround(eye_tan_w * px_per_tan_x);
        auto& e = p[eye];
        e.x_scale = (eye_tan_w / (float)ew[eye]) * px_per_tan_x;
        e.x_offset = (eye_l[eye] - wide_l) * px_per_tan_x;
        e.y_scale = 1.0f;
        e.y_offset = 0.0f;
        e.disparity = px_per_tan_x * eye_x[eye];
        e.dist_min = 0.12f;
        e.max_shift = 96;
        e.depth_a = 0.0f;
        e.depth_b = 1.0f;
        e.depth_c = 1.0f;
        e.depth_e = 0.0f;
        e.fill_anchored = true;
    }

    MonoPeriphery mono{};
    // (the eye pass: its shader text, taken out of FoveaComposite.cpp by extract_shader.py; empty = left out)
    std::string eye_hlsl{};

    if (argc >= 3) {
        std::ifstream f(argv[2], std::ios::binary);
        eye_hlsl.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        if (eye_hlsl.empty()) {
            std::printf("eye pass shader text not read: %s\n", argv[2]);
            return 2;
        }
    }

    int rc = 0;

    {
        std::ofstream m(dir + "/meta.txt");
        m << "width " << W << "\nheight " << H << "\n";

        for (int eye = 0; eye < 2; ++eye) {
            m << "eye " << eye << " ew " << ew[eye] << " eh " << eh << " x_scale " << p[eye].x_scale << " x_offset " << p[eye].x_offset << " y_scale "
              << p[eye].y_scale << " disparity " << p[eye].disparity << "\n";
        }
    }

    for (UINT ss = 1; ss <= 2; ++ss) {
        if (g_eye_map_only == 2 && ss != 1) {
            continue;
        }

        ComPtr<ID3D12Resource> out[2]{make_tex(ew[0] * ss, eh * ss, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true),
            make_tex(ew[1] * ss, eh * ss, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true)};
        ID3D12Resource* const outs[2]{out[0].Get(), out[1].Get()};

        if (ss == 2 && !mono.ss_ready(g_dev.Get())) {
            std::printf("ss 2: the passes are not usable: %s\n", mono.last_error());
            rc = 1;
            break;
        }

        p[0].ss = ss;
        p[1].ss = ss;
        const bool ok = mono.reproject(g_dev.Get(), g_cmd.Get(), ss == 2 ? color2.Get() : color1.Get(), UAV, depth.Get(), PSR, outs, p,
            MonoPeriphery::Search::TwoPassRound3, (uint64_t)ss, [](int) {});
        run_list();
        std::printf("ss %u: reproject %s (%s), faster search %s\n", ss, ok ? "ok" : "FAILED", mono.last_error(), mono.fast_available() ? "yes" : "no");

        if (!ok) {
            rc = 1;
            break;
        }

        download(out[0].Get(), UAV, dir + "/out" + std::to_string(ss) + "_L.bin", 16);
        download(out[1].Get(), UAV, dir + "/out" + std::to_string(ss) + "_R.bin", 16);

        // the eye pass's shader on the left eye's periphery alone (no fovea, no interface, no post pass): its enlargement to
        // an eye image four times the rays' size, as in the game, with groups of 32 x 32 and of 16 x 16 pixels. With ss 2
        // the periphery is twice as dense: its taps do not fit the 32 x 32 groups' shared memory (the pass then reads
        // them from the texture) and d3d12::EyeCompose takes the 16 x 16 groups.
        if (!eye_hlsl.empty()) {
            // (third argument "map": only the case with the GUI map, to compare two texts of the shader quickly; "map1":
            // that case with the periphery image 1x only)
            const bool map_only = g_eye_map_only != 0;

            for (UINT tile : {32u, 16u}) {
                if (map_only) {
                    const int em = run_eye_shader(eye_hlsl, out[0].Get(), ew[0] * 4, eh * 4, tile,
                        dir + "/eyemap" + std::to_string(ss) + "_t" + std::to_string(tile) + "_L.bin", 2);
                    std::printf("ss %u: eye pass shader with the GUI map, the tile test, anchors and a beam, groups of %u x %u: %s\n", ss, tile, tile,
                        em == 0 ? "ok" : "FAILED");
                    rc |= em;
                    continue;
                }

                const int er = run_eye_shader(eye_hlsl, out[0].Get(), ew[0] * 4, eh * 4, tile,
                    dir + "/eye" + std::to_string(ss) + "_t" + std::to_string(tile) + "_L.bin");
                std::printf("ss %u: eye pass shader, groups of %u x %u: %s\n", ss, tile, tile, er == 0 ? "ok" : "FAILED");
                rc |= er;
                // [fix95] ... and with the wide view's interface on every pixel
                const int eu = run_eye_shader(eye_hlsl, out[0].Get(), ew[0] * 4, eh * 4, tile,
                    dir + "/eyeui" + std::to_string(ss) + "_t" + std::to_string(tile) + "_L.bin", 1);
                std::printf("ss %u: eye pass shader with the interface, groups of %u x %u: %s\n", ss, tile, tile, eu == 0 ? "ok" : "FAILED");
                rc |= eu;
                const int em = run_eye_shader(eye_hlsl, out[0].Get(), ew[0] * 4, eh * 4, tile,
                    dir + "/eyemap" + std::to_string(ss) + "_t" + std::to_string(tile) + "_L.bin", 2);
                std::printf("ss %u: ... with the GUI map, the tile test, anchors and a beam, groups of %u x %u: %s\n", ss, tile, tile, em == 0 ? "ok" : "FAILED");
                rc |= em;
            }

            if (rc != 0) {
                break;
            }
        }
    }

    // the same once more with the original pass at ss 1 (every pixel with the full search): the faster search has to give
    // the same image (as in the game's GPU pass test)
    if (rc == 0) {
        ComPtr<ID3D12Resource> out[2]{make_tex(ew[0], eh, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true), make_tex(ew[1], eh, DXGI_FORMAT_R32G32B32A32_FLOAT, UAV, true)};
        ID3D12Resource* const outs[2]{out[0].Get(), out[1].Get()};
        p[0].ss = 1;
        p[1].ss = 1;
        const bool ok = mono.reproject(g_dev.Get(), g_cmd.Get(), color1.Get(), UAV, depth.Get(), PSR, outs, p, MonoPeriphery::Search::Original, 3, [](int) {});
        run_list();
        std::printf("original pass: reproject %s (%s)\n", ok ? "ok" : "FAILED", mono.last_error());

        if (ok) {
            download(out[0].Get(), UAV, dir + "/out0_L.bin", 16);
            download(out[1].Get(), UAV, dir + "/out0_R.bin", 16);
        } else {
            rc = 1;
        }
    }

    std::printf("%s\n", rc == 0 ? "done: periphery test written" : "FAILED");
    return rc;
}
