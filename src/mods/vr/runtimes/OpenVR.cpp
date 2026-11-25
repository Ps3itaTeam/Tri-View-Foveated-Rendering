#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <chrono>

#include "../../VR.hpp"

#include "OpenVR.hpp"
#include "../CpuStats.hpp" // [TIMELINE 03.11.2026]

namespace runtimes {
// [COMPOSITOR LOCK 05.11.2026] see OpenVR::compositor_mtx
std::unique_lock<std::recursive_timed_mutex> lock_compositor(OpenVR& rt, bool present, std::chrono::milliseconds limit) {
    std::unique_lock<std::recursive_timed_mutex> lock{rt.compositor_mtx, std::defer_lock};

    if (lock.try_lock()) {
        return lock;
    }

#if REVR_RELEASE
    (void)lock.try_lock_for(limit); // [RELEASE SWITCH fix141] (the debug build also times the wait and counts the time-outs)
#else
    const auto t0 = cpustats::now_ns();
    const bool got = lock.try_lock_for(limit);
    const auto dt = (uint64_t)(std::max)(cpustats::now_ns() - t0, (int64_t)0);
    auto& s = cpustats::vr_submit();

    if (dt > 50'000) {
        (present ? s.lock_waits_present : s.lock_waits_poses).fetch_add(1, std::memory_order_relaxed);
        (present ? s.lock_wait_ns_present : s.lock_wait_ns_poses).fetch_add(dt, std::memory_order_relaxed);
    }

    if (!got) {
        s.lock_timeouts.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};

        if (logged.fetch_add(1, std::memory_order_relaxed) < 5) {
            REVR_LOG(spdlog::warn("[VR] the compositor was busy on the other thread for {} ms: {} goes ahead without waiting", limit.count(),
                present ? "the submit" : "WaitGetPoses"));
        }
    }
#endif

    return lock;
}

VRRuntime::Error OpenVR::synchronize_frame() {
    if (this->got_first_poses && !this->is_hmd_active) {
        return VRRuntime::Error::SUCCESS;
    }

    if (this->frame_synced) {
        return VRRuntime::Error::SUCCESS;
    }

    // [COMPOSITOR LOCK 05.11.2026] not while the render thread is in the compositor (see compositor_mtx)
    const auto compositor_lock = lock_compositor(*this, false);
    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);
    const auto t0 = std::chrono::steady_clock::now();
    cpustats::timeline().mark(cpustats::Ev::WaitPosesBegin); // [TIMELINE 03.11.2026]
    auto ret = vr::VRCompositor()->WaitGetPoses(this->real_render_poses.data(), vr::k_unMaxTrackedDeviceCount, this->real_game_poses.data(), vr::k_unMaxTrackedDeviceCount);
    cpustats::timeline().mark(cpustats::Ev::WaitPosesEnd);
    // [CPU PROFILE 03.11.2026] the wait for the compositor's frame timing (idle, not work)
    this->sync_wait_ns.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
    this->sync_waits.fetch_add(1, std::memory_order_relaxed);

    if (ret == vr::VRCompositorError_None) {
        this->got_first_valid_poses = true;
        this->got_first_sync = true;
        this->frame_synced = true;
    }

    return (VRRuntime::Error)ret;
}

VRRuntime::Error OpenVR::update_poses() {
    if (!this->ready()) {
        return VRRuntime::Error::SUCCESS;
    }

    std::unique_lock _{ this->pose_mtx };

    memcpy(this->render_poses.data(), this->real_render_poses.data(), sizeof(this->render_poses));
    this->needs_pose_update = false;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenVR::update_render_target_size() {
    this->hmd->GetRecommendedRenderTargetSize(&this->w, &this->h);

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenVR::get_width() const {
    return this->w;
}

uint32_t OpenVR::get_height() const {
    return this->h;
}

VRRuntime::Error OpenVR::consume_events(std::function<void(void*)> callback) {
    // Process OpenVR events
    vr::VREvent_t event{};
    while (this->hmd->PollNextEvent(&event, sizeof(event))) {
        if (callback) {
            callback(&event);
        }

        switch ((vr::EVREventType)event.eventType) {
            // Detect whether video settings changed
            case vr::VREvent_SteamVRSectionSettingChanged: {
                REVR_LOG(spdlog::info("VR: VREvent_SteamVRSectionSettingChanged"));
                update_render_target_size();
            } break;

            // Detect whether SteamVR reset the standing/seated pose
            case vr::VREvent_SeatedZeroPoseReset: [[fallthrough]];
            case vr::VREvent_StandingZeroPoseReset: {
                REVR_LOG(spdlog::info("VR: VREvent_SeatedZeroPoseReset"));
                this->wants_reset_origin = true;
            } break;

            case vr::VREvent_DashboardActivated: {
                this->handle_pause = true;
            } break;

            // [SUBMIT 05.11.2026] the compositor's view of the application (counted for the periodic log, named here)
            case vr::VREvent_Compositor_ApplicationNotResponding: {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] counted for the debug log
                cpustats::vr_submit().not_responding.fetch_add(1, std::memory_order_relaxed);
#endif
                REVR_LOG(spdlog::info("VR: compositor event: application not responding (1415)"));
            } break;

            case vr::VREvent_Compositor_ApplicationResumed: {
#if !REVR_RELEASE
                cpustats::vr_submit().resumed.fetch_add(1, std::memory_order_relaxed);
#endif
                REVR_LOG(spdlog::info("VR: compositor event: application resumed (1416)"));
            } break;

            case vr::VREvent_Compositor_OutOfVideoMemory: {
#if !REVR_RELEASE
                cpustats::vr_submit().out_of_video_memory.fetch_add(1, std::memory_order_relaxed);
#endif
                REVR_LOG(spdlog::warn("VR: compositor event: out of video memory (1417)"));
            } break;

            case vr::VREvent_Compositor_ChaperoneBoundsShown: {
                REVR_LOG(spdlog::info("VR: compositor event: chaperone bounds shown (1410)"));
            } break;

            case vr::VREvent_Compositor_ChaperoneBoundsHidden: {
                REVR_LOG(spdlog::info("VR: compositor event: chaperone bounds hidden (1411)"));
            } break;

            default:
                REVR_LOG(spdlog::info("VR: Unknown event: {}", (uint32_t)event.eventType));
                break;
        }
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenVR::update_matrices(float nearz, float farz){
    std::unique_lock __{ this->eyes_mtx };
    const auto local_left = this->hmd->GetEyeToHeadTransform(vr::Eye_Left);
    const auto local_right = this->hmd->GetEyeToHeadTransform(vr::Eye_Right);

    this->eyes[vr::Eye_Left] = glm::rowMajor4(Matrix4x4f{ *(Matrix3x4f*)&local_left } );
    this->eyes[vr::Eye_Right] = glm::rowMajor4(Matrix4x4f{ *(Matrix3x4f*)&local_right } );

    auto pleft = this->hmd->GetProjectionMatrix(vr::Eye_Left, nearz, farz);
    auto pright = this->hmd->GetProjectionMatrix(vr::Eye_Right, nearz, farz);

    this->projections[vr::Eye_Left] = glm::rowMajor4(Matrix4x4f{ *(Matrix4x4f*)&pleft } );
    this->projections[vr::Eye_Right] = glm::rowMajor4(Matrix4x4f{ *(Matrix4x4f*)&pright } );

    this->hmd->GetProjectionRaw(vr::Eye_Left, &this->raw_projections[vr::Eye_Left][0], &this->raw_projections[vr::Eye_Left][1], &this->raw_projections[vr::Eye_Left][2], &this->raw_projections[vr::Eye_Left][3]);
    this->hmd->GetProjectionRaw(vr::Eye_Right, &this->raw_projections[vr::Eye_Right][0], &this->raw_projections[vr::Eye_Right][1], &this->raw_projections[vr::Eye_Right][2], &this->raw_projections[vr::Eye_Right][3]);

    return VRRuntime::Error::SUCCESS;
}

void OpenVR::destroy() {
    if (this->loaded) {
        vr::VR_Shutdown();
    }
}
}

