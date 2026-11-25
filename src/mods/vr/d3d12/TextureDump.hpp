// TextureDump.hpp -- debug: copies GPU textures into readback buffers and writes them as BMP files.
//
// Usage (one frame):
//   dump.add(device, cmd, texture, current_state, "name");   // records copy + barriers, texture stays in current_state
//   ... execute cmd, wait for the GPU ...
//   std::string report = dump.write_all(directory, 1024);  // BMP files + one text line per image (mean colour, % black)
//
// Formats decoded: R8G8B8A8 / B8G8R8A8 (UNORM, SRGB, TYPELESS), R10G10B10A2, R16G16B16A16_FLOAT, R11G11B10_FLOAT,
// R32G32B32A32_FLOAT, R16G16_FLOAT (motion vectors, shown as |x|,|y|), R32_FLOAT / D32 / R32_TYPELESS (depth).
#pragma once
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {

#if REVR_RELEASE
// [RELEASE SWITCH fix141 23.11.2026] the release has no frame dumps: an empty stand-in, every call compiles to nothing
class TextureDump {
public:
    bool add(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, const std::string&) { return false; }
    bool add_region(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, const std::string&, uint32_t, uint32_t,
        uint32_t, uint32_t) {
        return false;
    }
    void add_crop(const std::string&, uint32_t, uint32_t, uint32_t, uint32_t) {}
    void add_raw(const std::string&, uint32_t, uint32_t, uint32_t, uint32_t) {}
    std::string write_all(const std::filesystem::path&, uint32_t, bool = true) { return {}; }
    bool read_rgb(const std::string&, std::vector<float>&, uint32_t&, uint32_t&) { return false; }
    bool read_xy(const std::string&, std::vector<float>&, uint32_t&, uint32_t&) { return false; }
    void clear() {}
    bool empty() const { return true; }
};
#else
class TextureDump {
public:
    bool add(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state, const std::string& name);
    // [SUPER TAA 16.11.2026] only the part x, y, w x h of the image is copied and kept (the item is that part); a
    // depth-stencil texture is copied whole (D3D12 has no other way), its item is then the whole image
    bool add_region(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state, const std::string& name,
        uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // [FOVEA EDGE 01.11.2026] also write a part of the image `name` (added before or after) at full resolution, as a 16-bit
    // PPM (P6, maxval 65535, the stored values 0..1 without any curve): <name>_full_x<x>_y<y>.ppm (the part's corner in the
    // image; clipped to it)
    void add_crop(const std::string& name, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // [JITTER CHECK 10.11.2026] also write a part of the image `name` exactly as stored (any format, rows packed without
    // padding): <name>_raw_x<x>_y<y>_<w>x<h>_<format>.bin (clipped to the image)
    void add_raw(const std::string& name, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // bmps false: no BMP of the images (only the parts asked for with add_crop / add_raw); the report line of an image
    // then has no file name
    std::string write_all(const std::filesystem::path& dir, uint32_t max_dim, bool bmps = true);
    // [SUPER TAA 16.11.2026] the image `name` (added before, its copy executed and done) as three values per pixel, as
    // stored (0..1 for the 8 and 10 bit formats, no curve); false: no such image, or a format that is not decoded
    bool read_rgb(const std::string& name, std::vector<float>& rgb, uint32_t& w, uint32_t& h);
    // ... its first two channels with their sign, as stored (motion vectors: R16G16B16A16_SNORM, R16G16_SNORM,
    // R16G16_FLOAT, R16G16B16A16_FLOAT, R32G32_FLOAT); false: no such image, or another format
    bool read_xy(const std::string& name, std::vector<float>& xy, uint32_t& w, uint32_t& h);
    void clear() {
        m_items.clear();
        m_crops.clear();
        m_raws.clear();
    }
    bool empty() const { return m_items.empty(); }

private:
    struct Item {
        std::string name{};
        ComPtr<ID3D12Resource> readback{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint32_t width{0};
        uint32_t height{0};
        uint64_t total{0}; // readback buffer size (the last row is not padded to RowPitch)
        uint32_t bpt{0};   // [SUPER TAA 16.11.2026] bytes per texel from the footprint (0: by the format)
    };

    struct Crop {
        std::string name{};
        uint32_t x{0}, y{0}, w{0}, h{0};
    };

    std::vector<Item> m_items{};
    std::vector<Crop> m_crops{};
    std::vector<Crop> m_raws{};
};
#endif

} // namespace d3d12
