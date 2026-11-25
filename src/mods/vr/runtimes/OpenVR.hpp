#pragma once

#include <chrono>
#include <mutex>

#include "VRRuntime.hpp"

namespace runtimes {
struct OpenVR final : public VRRuntime {
    OpenVR() {
        this->custom_stage = SynchronizeStage::EARLY;
    }

    virtual ~OpenVR() {
        this->destroy();
    }

    std::string_view name() const override {
        return "OpenVR";
    }

    VRRuntime::Type type() const override { 
        return VRRuntime::Type::OPENVR;
    }

    bool ready() const override {
        return VRRuntime::ready() && this->is_hmd_active && this->got_first_poses;
    }

    VRRuntime::Error synchronize_frame() override;
    VRRuntime::Error update_poses() override;
    VRRuntime::Error update_render_target_size() override;

    uint32_t get_width() const override;
    uint32_t get_height() const override;

    VRRuntime::Error consume_events(std::function<void(void*)> callback) override;
    VRRuntime::Error update_matrices(float nearz, float farz) override;

    void destroy() override;

    bool is_hmd_active{false};
    bool was_hmd_active{true};

    uint32_t w{0};
    uint32_t h{0};

    vr::IVRSystem* hmd{nullptr};

    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> real_render_poses;
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> real_game_poses;

    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> render_poses;
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> game_poses;

    std::chrono::system_clock::time_point last_hmd_active_time{};

    // [COMPOSITOR LOCK 05.11.2026] The compositor's client is used from two threads: WaitGetPoses on the game's main thread
    // (sync mode Late, before EndRendering) and Submit, the overlay textures and PostPresentHandoff on the render thread (the
    // VR mod's present callback). For D3D12 the client wraps the textures with D3D11On12 on the game's queue, which is not
    // safe from two threads at once. Crash of 05.11.2026 19:24: an access violation in the D3D12 driver under D3D11On12 under
    // vrclient on the main thread, while the render thread was inside vrclient and D3D11On12 too (right after a quad view
    // re-initialisation had held the render thread for 5 s). Both sides take this lock (with a time limit, so that a wait that
    // never ends cannot block the game); see lock_compositor.
    std::recursive_timed_mutex compositor_mtx{};
};

// [COMPOSITOR LOCK 05.11.2026] takes OpenVR::compositor_mtx for the scope, waiting at most `limit`; `present` = the render
// thread's side (for the periodic log's counters)
std::unique_lock<std::recursive_timed_mutex> lock_compositor(OpenVR& rt, bool present,
    std::chrono::milliseconds limit = std::chrono::milliseconds(250));
}