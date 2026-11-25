#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <atomic>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "ffx/upscalers/include/ffx_upscale.h"
#include "ffx/api/include/dx12/ffx_api_dx12.h"

#include "FsrDirect.hpp"

namespace d3d12 {

namespace {
struct Api {
    PfnFfxCreateContext create{nullptr};
    PfnFfxDestroyContext destroy{nullptr};
    PfnFfxDispatch dispatch{nullptr};
    PfnFfxQuery query{nullptr};
    std::string source{};
    bool tried{false};
};

std::mutex g_api_mutex{};
Api g_api{};
std::atomic<uint64_t> g_messages{0};

std::string narrow(const wchar_t* w) {
    if (w == nullptr) {
        return {};
    }

    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);

    if (n <= 1) {
        return {};
    }

    std::string out((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// FSR's messages (errors and warnings) in the framework log; the first 64 only
void ffx_message(uint32_t type, const wchar_t* message) {
    const auto n = g_messages.fetch_add(1, std::memory_order_relaxed);

    if (n >= 64) {
        return;
    }

    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        REVR_LOG(spdlog::error("[FsrDirect] FSR: {}", narrow(message)));
    } else {
        REVR_LOG(spdlog::warn("[FsrDirect] FSR: {}", narrow(message)));
    }
}

// the API's functions: from an FSR DLL already loaded (the upscaler plugin loads them), else from the game folder
bool load_api(std::string& error) {
    std::scoped_lock _{g_api_mutex};

    if (g_api.create != nullptr) {
        return true;
    }

    if (g_api.tried) {
        error = "AMD's FSR API not found (amd_fidelityfx_loader_dx12.dll / amd_fidelityfx_upscaler_dx12.dll)";
        return false;
    }

    g_api.tried = true;

    static constexpr const wchar_t* NAMES[] = {L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll"};

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path{exe}.parent_path();

    for (int pass = 0; pass < 2; ++pass) {
        for (const auto* name : NAMES) {
            HMODULE module = pass == 0 ? GetModuleHandleW(name) : LoadLibraryW((dir / name).c_str());

            if (module == nullptr) {
                continue;
            }

            Api a{};
            a.create = (PfnFfxCreateContext)GetProcAddress(module, "ffxCreateContext");
            a.destroy = (PfnFfxDestroyContext)GetProcAddress(module, "ffxDestroyContext");
            a.dispatch = (PfnFfxDispatch)GetProcAddress(module, "ffxDispatch");
            a.query = (PfnFfxQuery)GetProcAddress(module, "ffxQuery");

            if (a.create == nullptr || a.destroy == nullptr || a.dispatch == nullptr || a.query == nullptr) {
                continue;
            }

            a.source = narrow(name) + (pass == 0 ? "" : " (loaded by the mod)");
            a.tried = true;
            g_api = a;
            REVR_LOG(spdlog::info("[FsrDirect] AMD's FSR API from {}", g_api.source));
            return true;
        }
    }

    error = "AMD's FSR API not found (amd_fidelityfx_loader_dx12.dll / amd_fidelityfx_upscaler_dx12.dll)";
    REVR_LOG(spdlog::error("[FsrDirect] {}", error));
    return false;
}
} // namespace

std::string FsrDirect::api_source() {
    std::scoped_lock _{g_api_mutex};
    return g_api.source;
}

uint64_t FsrDirect::messages() {
    return g_messages.load(std::memory_order_relaxed);
}

bool FsrDirect::create(ID3D12Device* device, const Create& c) {
    destroy();

    if (device == nullptr || c.render_w == 0 || c.render_h == 0 || c.out_w < c.render_w || c.out_h < c.render_h) {
        m_last_error = "bad sizes";
        return false;
    }

    if (!load_api(m_last_error)) {
        return false;
    }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &version.header;
    // the engine's depth: reversed (near = 1), infinite far plane
    desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | (c.auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u);
    desc.maxRenderSize = FfxApiDimensions2D{c.render_w, c.render_h};
    desc.maxUpscaleSize = FfxApiDimensions2D{c.out_w, c.out_h};
    desc.fpMessage = &ffx_message;

    ffxContext context = nullptr;
    auto rc = g_api.create(&context, &desc.header, nullptr);

    if (rc != FFX_API_RETURN_OK || context == nullptr) {
        // an older FSR API (before SDK 2.1) does not know the version description: once more without it
        REVR_LOG(spdlog::warn("[FsrDirect] ffxCreateContext with the API version {:x} failed (code {}), once more without the version",
            (uint32_t)FFX_UPSCALER_VERSION, (uint32_t)rc));
        desc.header.pNext = &backend.header;
        context = nullptr;
        rc = g_api.create(&context, &desc.header, nullptr);
    }

    if (rc != FFX_API_RETURN_OK || context == nullptr) {
        m_last_error = std::format("ffxCreateContext failed (code {})", (uint32_t)rc);
        REVR_LOG(spdlog::error("[FsrDirect] {} for {}x{} -> {}x{}", m_last_error, c.render_w, c.render_h, c.out_w, c.out_h));
        return false;
    }

    m_context = context;
    m_created = c;

    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;

    if (g_api.query(&context, &pv.header) == FFX_API_RETURN_OK && pv.versionName != nullptr) {
        m_provider = std::format("{} (id {:x})", pv.versionName, pv.versionId);
    } else {
        m_provider = "unknown version";
    }

    m_last_error.clear();
    REVR_LOG(spdlog::info("[FsrDirect] context {}x{} -> {}x{}, automatic exposure {}: {} from {}", c.render_w, c.render_h, c.out_w, c.out_h,
        c.auto_exposure, m_provider, g_api.source));
    return true;
}

bool FsrDirect::dispatch(ID3D12GraphicsCommandList* cmd, const Input& in) {
    if (m_context == nullptr || cmd == nullptr || in.color == nullptr || in.depth == nullptr || in.motion == nullptr || in.output == nullptr) {
        m_last_error = "not ready or missing input";
        return false;
    }

    constexpr uint32_t READ = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = ffxApiGetResourceDX12(in.color, READ);
    d.depth = ffxApiGetResourceDX12(in.depth, READ);
    d.motionVectors = ffxApiGetResourceDX12(in.motion, READ);
    d.exposure = ffxApiGetResourceDX12(nullptr, READ);
    d.reactive = ffxApiGetResourceDX12(nullptr, READ);
    d.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, READ);
    d.output = ffxApiGetResourceDX12(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = FfxApiFloatCoords2D{in.jitter_x, in.jitter_y};
    d.motionVectorScale = FfxApiFloatCoords2D{in.motion_scale_x, in.motion_scale_y};
    d.renderSize = FfxApiDimensions2D{in.render_w, in.render_h};
    d.upscaleSize = FfxApiDimensions2D{in.out_w, in.out_h};
    d.enableSharpening = false; // the mod's RCAS after the encode
    d.sharpness = 0.0f;
    d.frameTimeDelta = in.frame_ms;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    // reversed depth with an infinite far plane: near = FLT_MAX, far = the near plane
    d.cameraNear = FLT_MAX;
    d.cameraFar = in.near_plane;
    d.cameraFovAngleVertical = in.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    const auto rc = g_api.dispatch(&m_context, &d.header);

    if (rc != FFX_API_RETURN_OK) {
        m_last_error = std::format("ffxDispatch failed (code {})", (uint32_t)rc);
        return false;
    }

    return true;
}

void FsrDirect::destroy() {
    if (m_context != nullptr) {
        ffxContext context = m_context;

        if (g_api.destroy != nullptr) {
            g_api.destroy(&context, nullptr);
        }

        m_context = nullptr;
        REVR_LOG(spdlog::info("[FsrDirect] context released"));
    }

    m_created = Create{};
    m_provider.clear();
}

} // namespace d3d12
