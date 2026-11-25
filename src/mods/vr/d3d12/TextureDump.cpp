#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#if !REVR_RELEASE // [RELEASE SWITCH fix141] frame dumps: debug build only (the header has the release's empty stand-in)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

#include "TextureDump.hpp"

namespace d3d12 {

namespace {
float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1;
    const uint32_t exp = (h >> 10) & 0x1F;
    const uint32_t mant = h & 0x3FF;
    float v = 0.0f;

    if (exp == 0) {
        v = std::ldexp((float)mant, -24);
    } else if (exp == 31) {
        v = mant != 0 ? NAN : INFINITY;
    } else {
        v = std::ldexp((float)(mant | 0x400), (int)exp - 25);
    }

    return sign ? -v : v;
}

// unsigned small floats of R11G11B10_FLOAT (5-bit exponent, 6 or 5 bit mantissa)
float small_float(uint32_t bits, uint32_t mant_bits) {
    const uint32_t exp = (bits >> mant_bits) & 0x1F;
    const uint32_t mant = bits & ((1u << mant_bits) - 1);

    if (exp == 0) {
        return std::ldexp((float)mant, -14 - (int)mant_bits);
    }

    if (exp == 31) {
        return mant != 0 ? NAN : INFINITY;
    }

    return std::ldexp((float)(mant | (1u << mant_bits)), (int)exp - 15 - (int)mant_bits);
}

const char* format_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
    // [SUPER TAA 16.11.2026] the distances of the mod's TAAs, and what the engine buffer probe may meet
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
    case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R32_UINT: return "R32_UINT";
    case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    default: return "other";
    }
}

// Decodes one texel to linear-ish RGB (display values for 8/10 bit, raw for float). false = format not supported.
bool decode(DXGI_FORMAT f, const uint8_t* p, float& r, float& g, float& b) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        r = p[0] / 255.0f;
        g = p[1] / 255.0f;
        b = p[2] / 255.0f;
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        r = p[2] / 255.0f;
        g = p[1] / 255.0f;
        b = p[0] / 255.0f;
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t v{};
        std::memcpy(&v, p, 4);
        r = (v & 0x3FF) / 1023.0f;
        g = ((v >> 10) & 0x3FF) / 1023.0f;
        b = ((v >> 20) & 0x3FF) / 1023.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        uint16_t h[3]{};
        std::memcpy(h, p, 6);
        r = half_to_float(h[0]);
        g = half_to_float(h[1]);
        b = half_to_float(h[2]);
        return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        uint32_t v{};
        std::memcpy(&v, p, 4);
        r = small_float(v & 0x7FF, 6);
        g = small_float((v >> 11) & 0x7FF, 6);
        b = small_float((v >> 22) & 0x3FF, 5);
        return true;
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        std::memcpy(&r, p, 4);
        std::memcpy(&g, p + 4, 4);
        std::memcpy(&b, p + 8, 4);
        return true;
    case DXGI_FORMAT_R16G16_FLOAT: {
        uint16_t h[2]{};
        std::memcpy(h, p, 4);
        r = std::fabs(half_to_float(h[0])) * 50.0f; // motion vectors: magnified
        g = std::fabs(half_to_float(h[1])) * 50.0f;
        b = 0.0f;
        return true;
    }
    case DXGI_FORMAT_R32G32_FLOAT: {
        // [FSR DIRECT 06.11.2026] FSR 4's motion vectors side by side: magnified like R16G16_FLOAT
        float v[2]{};
        std::memcpy(v, p, 8);
        r = std::fabs(v[0]) * 50.0f;
        g = std::fabs(v[1]) * 50.0f;
        b = 0.0f;
        return true;
    }
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: {
        float d{};
        std::memcpy(&d, p, 4);
        r = g = b = std::pow(std::clamp(d, 0.0f, 1.0f), 0.25f); // reverse-Z depth: near = bright
        return true;
    }
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: {
        uint32_t v{};
        std::memcpy(&v, p, 4);
        r = g = b = std::pow((float)(v & 0xFFFFFF) / 16777215.0f, 0.25f);
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
        uint16_t u[3]{};
        std::memcpy(u, p, 6);
        r = u[0] / 65535.0f;
        g = u[1] / 65535.0f;
        b = u[2] / 65535.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_SNORM: {
        // [MOTION 24.10.2026] the engine's motion vectors: |x| red, |y| green, magnified like R16G16_FLOAT
        int16_t v[2]{};
        std::memcpy(v, p, 4);
        r = std::fabs((std::max)(v[0] / 32767.0f, -1.0f)) * 50.0f;
        g = std::fabs((std::max)(v[1] / 32767.0f, -1.0f)) * 50.0f;
        b = 0.0f;
        return true;
    }
    // [SUPER TAA 16.11.2026] single and double channel formats (shown as they are stored; distances in metres / 10)
    case DXGI_FORMAT_R16_FLOAT: {
        uint16_t h{};
        std::memcpy(&h, p, 2);
        r = g = b = half_to_float(h) * 0.1f;
        return true;
    }
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM: {
        uint16_t u{};
        std::memcpy(&u, p, 2);
        r = g = b = u / 65535.0f;
        return true;
    }
    case DXGI_FORMAT_R8_UNORM:
        r = g = b = p[0] / 255.0f;
        return true;
    case DXGI_FORMAT_R8G8_UNORM:
        r = p[0] / 255.0f;
        g = p[1] / 255.0f;
        b = 0.0f;
        return true;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_UNORM: {
        uint16_t u[2]{};
        std::memcpy(u, p, 4);
        r = u[0] / 65535.0f;
        g = u[1] / 65535.0f;
        b = 0.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16_SNORM: {
        int16_t v[2]{};
        std::memcpy(v, p, 4);
        r = std::fabs((std::max)(v[0] / 32767.0f, -1.0f));
        g = std::fabs((std::max)(v[1] / 32767.0f, -1.0f));
        b = 0.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
        uint16_t h[3]{};
        std::memcpy(h, p, 6);
        r = half_to_float(h[0]);
        g = half_to_float(h[1]);
        b = half_to_float(h[2]);
        return true;
    }
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        r = std::fabs((std::max)((int8_t)p[0] / 127.0f, -1.0f));
        g = std::fabs((std::max)((int8_t)p[1] / 127.0f, -1.0f));
        b = std::fabs((std::max)((int8_t)p[2] / 127.0f, -1.0f));
        return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: {
        float d{};
        std::memcpy(&d, p, 4);
        r = g = b = std::pow(std::clamp(d, 0.0f, 1.0f), 0.25f);
        return true;
    }
    default:
        return false;
    }
}

uint32_t bytes_per_texel(DXGI_FORMAT f) {
    switch (f) {
    // [SUPER TAA 16.11.2026]
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: return 2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return 8;
    case DXGI_FORMAT_R32G32_FLOAT: return 8; // [FSR DIRECT 06.11.2026]
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}

bool write_bmp(const std::filesystem::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t data_size = row * h;
    const uint32_t file_size = 54 + data_size;

    std::ofstream f(path, std::ios::binary);

    if (!f) {
        return false;
    }

    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54, dib = 40;
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &dib, 4);
    const int32_t iw = (int32_t)w, ih = (int32_t)h;
    std::memcpy(header + 18, &iw, 4);
    std::memcpy(header + 22, &ih, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &data_size, 4);
    f.write((const char*)header, 54);

    std::vector<uint8_t> line(row, 0);

    for (int32_t y = (int32_t)h - 1; y >= 0; --y) { // BMP rows are bottom-up
        for (uint32_t x = 0; x < w; ++x) {
            const auto* s = &rgb[((size_t)y * w + x) * 3];
            line[x * 3 + 0] = s[2];
            line[x * 3 + 1] = s[1];
            line[x * 3 + 2] = s[0];
        }

        f.write((const char*)line.data(), row);
    }

    return (bool)f;
}
} // namespace

bool TextureDump::add(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state, const std::string& name) {
    if (device == nullptr || cmd == nullptr || src == nullptr) {
        return false;
    }

    const auto desc = src->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
        return false;
    }

    Item item{};
    item.name = name;
    item.format = desc.Format;
    item.width = (uint32_t)desc.Width;
    item.height = desc.Height;

    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &rows, &row_size, &total);
    item.total = total;
    // decode with the format of the copied plane: depth-stencil textures copy their depth plane (R32 / R24G8)
    if (item.footprint.Footprint.Format != DXGI_FORMAT_UNKNOWN) {
        item.format = item.footprint.Footprint.Format;
    }

    // [SUPER TAA 16.11.2026] the bytes of one texel from the row's own size: right for any format, also one the list above
    // does not know (the engine buffer probe writes such images as they are stored)
    item.bpt = item.width != 0 ? (uint32_t)(row_size / item.width) : 0;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&item.readback)))) {
        return false;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = item.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = item.footprint;

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);

    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
    }

    m_items.push_back(std::move(item));
    return true;
}

// [SUPER TAA 16.11.2026] A part of `src` only: the readback holds w x h texels (a capture of many frames of large images
// would otherwise keep every whole image until it is written). The item is that part: its BMP, crops and raw parts
// count from the part's corner. Not for depth-stencil textures (D3D12 copies those whole): they go through add().
bool TextureDump::add_region(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state, const std::string& name,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (device == nullptr || cmd == nullptr || src == nullptr) {
        return false;
    }

    auto desc = src->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || x >= desc.Width || y >= desc.Height || w == 0 || h == 0) {
        return false;
    }

    w = (uint32_t)(std::min)((uint64_t)w, desc.Width - x);
    h = (std::min)(h, desc.Height - y);

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 || (x == 0 && y == 0 && w == desc.Width && h == desc.Height)) {
        return add(device, cmd, src, state, name);
    }

    Item item{};
    item.name = name;
    item.format = desc.Format;
    item.width = w;
    item.height = h;

    // the footprint of an image of the part's size
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &rows, &row_size, &total);
    item.total = total;

    if (item.footprint.Footprint.Format != DXGI_FORMAT_UNKNOWN) {
        item.format = item.footprint.Footprint.Format;
    }

    item.bpt = (uint32_t)(row_size / w);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (total == 0 || FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&item.readback)))) {
        return false;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = item.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = item.footprint;

    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;

    D3D12_BOX box{};
    box.left = x;
    box.top = y;
    box.front = 0;
    box.right = x + w;
    box.bottom = y + h;
    box.back = 1;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, &box);

    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
    }

    m_items.push_back(std::move(item));
    return true;
}

void TextureDump::add_crop(const std::string& name, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        m_crops.push_back(Crop{name, x, y, w, h});
    }
}

void TextureDump::add_raw(const std::string& name, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        m_raws.push_back(Crop{name, x, y, w, h});
    }
}

bool TextureDump::read_rgb(const std::string& name, std::vector<float>& rgb, uint32_t& w, uint32_t& h) {
    for (auto& item : m_items) {
        if (item.name != name || item.readback == nullptr) {
            continue;
        }

        void* mapped = nullptr;
        const D3D12_RANGE read{0, (SIZE_T)item.total};

        if (FAILED(item.readback->Map(0, &read, &mapped)) || mapped == nullptr) {
            return false;
        }

        const auto* base = (const uint8_t*)mapped + item.footprint.Offset;
        const uint32_t pitch = item.footprint.Footprint.RowPitch;
        const uint32_t bpt = item.bpt != 0 ? item.bpt : bytes_per_texel(item.format);
        bool ok = true;
        w = item.width;
        h = item.height;
        rgb.assign((size_t)w * h * 3, 0.0f);

        for (uint32_t y = 0; y < h && ok; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                float* d = &rgb[((size_t)y * w + x) * 3];

                if (!decode(item.format, base + (size_t)y * pitch + (size_t)x * bpt, d[0], d[1], d[2])) {
                    ok = false;
                    break;
                }
            }
        }

        const D3D12_RANGE none{0, 0};
        item.readback->Unmap(0, &none);
        return ok;
    }

    return false;
}

bool TextureDump::read_xy(const std::string& name, std::vector<float>& xy, uint32_t& w, uint32_t& h) {
    for (auto& item : m_items) {
        if (item.name != name || item.readback == nullptr) {
            continue;
        }

        // 0 = 16 bit signed, 1 = 16 bit float, 2 = 32 bit float
        int kind = -1;

        switch (item.format) {
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16_SNORM: kind = 0; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT: kind = 1; break;
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: kind = 2; break;
        default: return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE read{0, (SIZE_T)item.total};

        if (FAILED(item.readback->Map(0, &read, &mapped)) || mapped == nullptr) {
            return false;
        }

        const auto* base = (const uint8_t*)mapped + item.footprint.Offset;
        const uint32_t pitch = item.footprint.Footprint.RowPitch;
        const uint32_t bpt = item.bpt != 0 ? item.bpt : bytes_per_texel(item.format);
        w = item.width;
        h = item.height;
        xy.assign((size_t)w * h * 2, 0.0f);

        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = base + (size_t)y * pitch + (size_t)x * bpt;
                float* d = &xy[((size_t)y * w + x) * 2];

                if (kind == 0) {
                    int16_t v[2]{};
                    std::memcpy(v, p, 4);
                    d[0] = (std::max)(v[0] / 32767.0f, -1.0f);
                    d[1] = (std::max)(v[1] / 32767.0f, -1.0f);
                } else if (kind == 1) {
                    uint16_t v[2]{};
                    std::memcpy(v, p, 4);
                    d[0] = half_to_float(v[0]);
                    d[1] = half_to_float(v[1]);
                } else {
                    std::memcpy(d, p, 8);
                }
            }
        }

        const D3D12_RANGE none{0, 0};
        item.readback->Unmap(0, &none);
        return true;
    }

    return false;
}

std::string TextureDump::write_all(const std::filesystem::path& dir, uint32_t max_dim, bool bmps) {
    std::string report{};
    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);

    for (auto& item : m_items) {
        void* mapped = nullptr;
        // [FIX 21.10.2026] the range must stay inside the buffer: RowPitch * height is past its end whenever a row
        // is not a multiple of 256 bytes (the last row is not padded) -> Map failed for every image
        const D3D12_RANGE read{0, (SIZE_T)item.total};

        if (const auto hr = item.readback->Map(0, &read, &mapped); FAILED(hr) || mapped == nullptr) {
            report += std::format("  {}: {}x{} {} map failed (hr=0x{:08x})\n", item.name, item.width, item.height, format_name(item.format), (uint32_t)hr);
            continue;
        }

        const auto* base = (const uint8_t*)mapped + item.footprint.Offset;
        const uint32_t pitch = item.footprint.Footprint.RowPitch;
        const uint32_t bpt = item.bpt != 0 ? item.bpt : bytes_per_texel(item.format); // [SUPER TAA 16.11.2026]
        const uint32_t factor = (std::max)(1u, ((std::max)(item.width, item.height) + max_dim - 1) / max_dim);
        const uint32_t ow = (std::max)(1u, item.width / factor);
        const uint32_t oh = (std::max)(1u, item.height / factor);
        const bool is_float = item.format == DXGI_FORMAT_R16G16B16A16_FLOAT || item.format == DXGI_FORMAT_R11G11B10_FLOAT ||
                              item.format == DXGI_FORMAT_R32G32B32A32_FLOAT;

        std::vector<uint8_t> rgb((size_t)ow * oh * 3, 0);
        double sum[3]{};
        uint64_t black = 0, nan_count = 0, samples = 0;
        bool supported = true;

        for (uint32_t oy = 0; oy < oh && supported; ++oy) {
            for (uint32_t ox = 0; ox < ow; ++ox) {
                const uint32_t x = ox * factor + factor / 2;
                const uint32_t y = oy * factor + factor / 2;
                float r = 0, g = 0, b = 0;

                if (!decode(item.format, base + (size_t)(std::min)(y, item.height - 1) * pitch + (size_t)(std::min)(x, item.width - 1) * bpt, r, g, b)) {
                    supported = false;
                    break;
                }

                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                    ++nan_count;
                    r = g = b = 0.0f;
                }

                sum[0] += r;
                sum[1] += g;
                sum[2] += b;
                ++samples;

                if (r + g + b < 3.0f / 255.0f) {
                    ++black;
                }

                // values to show: float formats get a simple Reinhard so HDR scenes stay visible
                if (is_float) {
                    r = r / (1.0f + r);
                    g = g / (1.0f + g);
                    b = b / (1.0f + b);
                }

                // 8/10-bit values are written as they are stored (already display encoded)
                auto* d = &rgb[((size_t)oy * ow + ox) * 3];
                d[0] = (uint8_t)std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f);
                d[1] = (uint8_t)std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
                d[2] = (uint8_t)std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f);
            }
        }

        // [FOVEA EDGE 01.11.2026] the parts of this image asked for at full resolution
        for (const auto& c : m_crops) {
            if (!supported || c.name != item.name || c.x >= item.width || c.y >= item.height) {
                continue;
            }

            const uint32_t cw = (std::min)(c.w, item.width - c.x);
            const uint32_t ch = (std::min)(c.h, item.height - c.y);
            std::string ppm = std::format("P6\n{} {}\n65535\n", cw, ch);
            const size_t head = ppm.size();
            ppm.resize(head + (size_t)cw * ch * 6);
            auto* out = (uint8_t*)ppm.data() + head;
            bool decoded = true;

            for (uint32_t y = 0; y < ch && decoded; ++y) {
                for (uint32_t x = 0; x < cw; ++x) {
                    float v[3]{};

                    if (!decode(item.format, base + (size_t)(c.y + y) * pitch + (size_t)(c.x + x) * bpt, v[0], v[1], v[2])) {
                        decoded = false;
                        break;
                    }

                    for (int k = 0; k < 3; ++k) {
                        const float f = std::isfinite(v[k]) ? std::clamp(v[k], 0.0f, 1.0f) : 0.0f;
                        const auto q = (uint16_t)(f * 65535.0f + 0.5f);
                        *out++ = (uint8_t)(q >> 8); // big-endian, as PPM wants
                        *out++ = (uint8_t)(q & 0xFF);
                    }
                }
            }

            // the size in the name too: parts of the same image may start at the same corner (a side's top and left strips)
            const auto file = dir / std::format("{}_full_x{}_y{}_{}x{}.ppm", item.name, c.x, c.y, cw, ch);
            bool written = false;

            if (decoded) {
                std::ofstream f(file, std::ios::binary);
                written = (bool)f.write(ppm.data(), (std::streamsize)ppm.size());
            }

            report += std::format("  {}: full resolution part {}x{} at {},{} -> {}{}\n", item.name, cw, ch, c.x, c.y, file.filename().string(),
                written ? "" : " WRITE FAILED");
        }

        // [JITTER CHECK 10.11.2026] parts as stored (the values for an exact comparison of consecutive frames)
        for (const auto& c : m_raws) {
            if (c.name != item.name || c.x >= item.width || c.y >= item.height || bpt == 0) {
                continue;
            }

            const uint32_t cw = (std::min)(c.w, item.width - c.x);
            const uint32_t ch = (std::min)(c.h, item.height - c.y);
            std::string bin{};
            bin.resize((size_t)cw * ch * bpt);

            for (uint32_t y = 0; y < ch; ++y) {
                std::memcpy(bin.data() + (size_t)y * cw * bpt, base + (size_t)(c.y + y) * pitch + (size_t)c.x * bpt, (size_t)cw * bpt);
            }

            const auto file = dir / std::format("{}_raw_x{}_y{}_{}x{}_{}.bin", item.name, c.x, c.y, cw, ch, format_name(item.format));
            std::ofstream f(file, std::ios::binary);
            const bool written = (bool)f.write(bin.data(), (std::streamsize)bin.size());

            report += std::format("  {}: part as stored {}x{} at {},{} ({} bytes per pixel) -> {}{}\n", item.name, cw, ch, c.x, c.y, bpt,
                file.filename().string(), written ? "" : " WRITE FAILED");
        }

        const D3D12_RANGE none{0, 0};
        item.readback->Unmap(0, &none);

        if (!supported) {
            report += std::format("  {}: {}x{} format {} ({}) not decoded\n", item.name, item.width, item.height, (int)item.format, format_name(item.format));
            continue;
        }

        const auto file = dir / (item.name + ".bmp");
        const bool ok = !bmps || write_bmp(file, ow, oh, rgb);
        const double n = (double)(std::max)(samples, (uint64_t)1);

        report += std::format("  {}: {}x{} {} -> {} ({}x{}){} mean=({:.3f},{:.3f},{:.3f}) black={:.1f}% nan={}\n", item.name, item.width, item.height,
            format_name(item.format), bmps ? file.filename().string() : std::string{"no BMP"}, ow, oh, ok ? "" : " WRITE FAILED", sum[0] / n, sum[1] / n,
            sum[2] / n, 100.0 * (double)black / n, nan_count);
    }

    m_items.clear();
    m_crops.clear();
    m_raws.clear(); // [SUPER TAA 16.11.2026] (they stayed for the next write; no item of theirs is left)
    return report;
}

} // namespace d3d12
#endif // !REVR_RELEASE
