// shader_table_gen.cpp -- [BUILT-IN SHADERS fix145 23.11.2026] makes src/mods/vr/d3d12/ShaderTable.inl, the mod's shaders
// compiled ahead and built into the DLL (see ShaderCompile.hpp).
//
//   shader_table_gen.exe <ShaderTable.inl to write>
//
// Built with REVR_SHADER_GENERATOR (build.sh): every pass of the mod is set up on a D3D12 device as the game does it, and
// compile_shader hands each shader it compiles, with its key (source text, entry point, target, flags and macros), to
// shader_generator_record. The eye pass's texts made after its root signature come from eye_compose_generator_texts. The
// keys are the ones the game makes: the table is looked up by the same two hashes as the cache files. A shader that is
// not in the table (its text changed after the table was made, or a test variant) is compiled at its first use as before:
// make the table again after any change of a shader's text.
//
// Linux: wine + vkd3d + a Vulkan driver (lavapipe will do), and Microsoft's d3dcompiler_47.dll beside the exe.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <dxgi1_4.h>
#include <d3d11.h>

// (the passes' setup is private: this program calls it as the passes do on their first use; the standard headers above
// are read before)
#define private public
#include "FoveaComposite.hpp"
#include "SharpenPass.hpp"
#include "SuperTAA.hpp"
#undef private
#include "ShaderCompile.hpp"
#include "ShaderFileCache.hpp"

using Microsoft::WRL::ComPtr;

static std::map<std::string, std::vector<uint8_t>> g_code{};

namespace d3d12 {
void shader_generator_record(const std::string& key, ID3DBlob* code) {
    const auto* p = (const uint8_t*)code->GetBufferPointer();
    g_code[key] = std::vector<uint8_t>(p, p + code->GetBufferSize());
}
} // namespace d3d12

// the entry point and the macros of a key (source \0 entry \0 target \0 flags1 \0 flags2 [\0 NAME=VALUE ...])
static std::string describe(const std::string& key) {
    std::vector<std::string> parts{};
    size_t start = 0;

    for (size_t i = 0; i <= key.size(); ++i) {
        if (i == key.size() || key[i] == '\0') {
            parts.push_back(key.substr(start, i - start));
            start = i + 1;
        }
    }

    std::string o = parts.size() > 1 ? parts[1] : "?";
    o += parts.size() > 2 ? " " + parts[2] : "";

    for (size_t i = 5; i < parts.size(); ++i) {
        o += " " + parts[i];
    }

    // the source by its length and the start of its first line that has text
    const auto& src = parts[0];
    size_t a = src.find_first_not_of("\r\n ");
    std::string first = a == std::string::npos ? "" : src.substr(a, src.find('\n', a) - a);
    first = first.substr(0, 40);
    return o + "  [" + std::to_string(src.size()) + " bytes: " + first + "]";
}

// one pass set up on its own and let go again (its images and buffers with it: lavapipe keeps them in shared memory)
template <class T> static void run(const char* what, ID3D12Device* dev, void (*after)(T&, ID3D12Device*) = nullptr) {
    const size_t before = g_code.size();
    T pass{};
    const bool ok = pass.setup(dev);
    std::printf("%-14s setup %s, %zu shaders\n", what, ok ? "ok" : "FAILED", g_code.size() - before);

    if (after != nullptr) {
        const size_t before2 = g_code.size();
        after(pass, dev);
        std::printf("%-14s and its texts made later: %zu shaders\n", what, g_code.size() - before2);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: shader_table_gen.exe <ShaderTable.inl>\n");
        return 2;
    }

    ComPtr<ID3D12Device> dev{};

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("no D3D12 device\n");
        return 1;
    }

    run<d3d12::FoveaComposite>("FoveaComposite", dev.Get());
    run<d3d12::WideUpscale>("WideUpscale", dev.Get());
    run<d3d12::LightTAA>("LightTAA", dev.Get());
    run<d3d12::FoveaTAA>("FoveaTAA", dev.Get());
    run<d3d12::MonoPeriphery>("MonoPeriphery", dev.Get(), [](d3d12::MonoPeriphery& p, ID3D12Device* d) { p.ss_ready(d); });
    run<d3d12::StableDepth>("StableDepth", dev.Get());
    run<d3d12::UiSteady>("UiSteady", dev.Get());
    run<d3d12::UiClean>("UiClean", dev.Get());
    run<d3d12::MonoUi>("MonoUi", dev.Get());
    run<d3d12::UiCurve>("UiCurve", dev.Get());
    run<d3d12::GuiMap>("GuiMap", dev.Get());
    run<d3d12::EyeCompose>("EyeCompose", dev.Get(), [](d3d12::EyeCompose&, ID3D12Device*) { d3d12::eye_compose_generator_texts(); });
    run<d3d12::FoveaLinear>("FoveaLinear", dev.Get());
    run<d3d12::SuperTAA>("SuperTAA", dev.Get(), [](d3d12::SuperTAA& p, ID3D12Device* d) { p.ensure_ui_motion(d); });
    run<d3d12::SharpenPass>("SharpenPass", dev.Get());
    run<d3d12::PostPass>("PostPass", dev.Get());
    run<d3d12::SmaaPass>("SmaaPass", dev.Get());
    run<d3d12::UiRestorePass>("UiRestorePass", dev.Get());
    run<d3d12::LongEdgePass>("LongEdgePass", dev.Get());

    // the table, sorted by the keys' hashes
    struct Row {
        uint64_t a, b;
        const std::string* key;
        const std::vector<uint8_t>* code;
    };

    std::vector<Row> rows{};
    size_t total = 0;

    for (const auto& [key, code] : g_code) {
        const uint64_t a = d3d12::shader_files::fnv64(key.data(), key.size());
        const uint64_t b = d3d12::shader_files::fnv64(key.data(), key.size(), 0x9E3779B97F4A7C15ull ^ (uint64_t)key.size());
        rows.push_back({a, b, &key, &code});
        total += code.size();
    }

    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return std::tie(x.a, x.b) < std::tie(y.a, y.b); });

    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].a == rows[i - 1].a && rows[i].b == rows[i - 1].b) {
            std::printf("two keys with the same hashes: no table written\n");
            return 1;
        }
    }

    std::ofstream out{argv[1], std::ios::binary};
    out << "// ShaderTable.inl -- the mod's shaders compiled ahead, built into the DLL (see ShaderCompile.hpp).\n"
           "// MADE BY tests/shaders/build.sh: do not edit. "
        << rows.size() << " shaders, " << total << " bytes of code.\n"
           "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace d3d12::shader_table {\n"
           "struct Entry {\n"
           "    uint64_t a;        // the key's two hashes (as the name of its file, ShaderFileCache.hpp)\n"
           "    uint64_t b;\n"
           "    uint32_t key_size; // the key's length\n"
           "    uint32_t size;     // the code's size in bytes (a DXBC container)\n"
           "    const uint32_t* code;\n"
           "};\n\n";

    char buf[64];

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& c = *rows[i].code;
        out << "// " << describe(*rows[i].key) << "\n";
        out << "static const uint32_t C" << i << "[] = {";

        for (size_t w = 0; w * 4 < c.size(); ++w) {
            uint32_t v = 0;
            std::memcpy(&v, c.data() + w * 4, (std::min)((size_t)4, c.size() - w * 4));
            std::snprintf(buf, sizeof(buf), "%s0x%x", w == 0 ? "" : ",", v);
            out << ((w % 16) == 0 ? "\n" : "") << buf;
        }

        out << "};\n";
    }

    out << "\nstatic const Entry ENTRIES[] = {\n";

    for (size_t i = 0; i < rows.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "0x%016llxull, 0x%016llxull", (unsigned long long)rows[i].a, (unsigned long long)rows[i].b);
        out << "    {" << buf << ", " << rows[i].key->size() << "u, " << rows[i].code->size() << "u, C" << i << "},\n";
    }

    out << "    {~0ull, ~0ull, 0, 0, nullptr}}; // (the last entry closes the list)\n"
        << "static constexpr size_t COUNT = " << rows.size() << ";\n} // namespace d3d12::shader_table\n";
    std::printf("%zu shaders, %zu bytes of code -> %s\n", rows.size(), total, argv[1]);

    for (const auto& r : rows) {
        std::printf("  %6zu  %s\n", r.code->size(), describe(*r.key).c_str());
    }

    return 0;
}
