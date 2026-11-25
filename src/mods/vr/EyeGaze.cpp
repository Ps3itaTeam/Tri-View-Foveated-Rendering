#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "../VR.hpp"
#include "openvr_ext/IVRSystem_026.hpp"

#include "EyeGaze.hpp"

EyeGaze& EyeGaze::get() {
    static EyeGaze instance{};
    return instance;
}

void EyeGaze::set_options(const Options& o) {
    std::scoped_lock _{m_mutex};
    m_options = o;

    foveation::GazeFilter::Settings s{};
    s.min_cutoff_hz = (std::max)(o.smoothing_hz, 0.1f);
    s.beta = (std::max)(o.saccade_boost, 0.0f);
    s.hold_seconds = (std::max)(o.hold_seconds, 0.0f);
    s.fallback_seconds = (std::max)(o.fallback_seconds, 0.01f);

    m_filter[0].configure(s);
    m_filter[1].configure(s);
}

const char* EyeGaze::status_text() const {
    switch (m_status) {
    case Status::NOT_OPENVR: return "HMD not active / not using OpenVR";
    case Status::INTERFACE_MISSING: return "SteamVR too old (no IVRSystem_026)";
    case Status::NO_EYE_TRACKER: return "No eye tracking data (fixed foveation)";
    case Status::TRACKING: return "Eye tracking active";
    }

    return "?";
}

EyeGaze::EyeState EyeGaze::get_eye(int eye) const {
    std::scoped_lock _{m_mutex};
    return m_state[std::clamp(eye, 0, 1)];
}

std::array<float, 2> EyeGaze::lens_center(int eye) const {
    std::scoped_lock _{m_mutex};
    eye = std::clamp(eye, 0, 1);
    return {m_lens_center[eye][0], m_lens_center[eye][1]};
}

bool EyeGaze::ensure_interface() {
    if (m_sys026 != nullptr) {
        return true;
    }

    // Nach einem Fehlschlag nur alle ~5 Sekunden (300 Frames) erneut versuchen.
    if (m_tried_interface && ++m_interface_retry_frames < 300) {
        return false;
    }

    m_interface_retry_frames = 0;
    m_tried_interface = true;

    vr::EVRInitError err{vr::VRInitError_None};
    m_sys026 = vr_ext::get_system_026(&err);

    if (m_sys026 == nullptr) {
        REVR_LOG(spdlog::info("[EyeGaze] IVRSystem_026 not available (error {}), eye tracking disabled", (int)err));
        return false;
    }

    REVR_LOG(spdlog::info("[EyeGaze] Acquired IVRSystem_026 (OpenVR SDK 2.15 eye tracking API)"));
    return true;
}

// Rueckfall = Linsenmitte: der Punkt, auf den der Blick geradeaus faellt. Aus den Raw-Tangenten des HMD:
//   x_ndc = -(l + r) / (r - l)        (y wird bewusst als Mitte genommen -- die Vertikale ist praktisch symmetrisch)
void EyeGaze::compute_fallback_centers(float out[2][2]) {
    out[0][0] = out[0][1] = out[1][0] = out[1][1] = 0.0f;

    auto vr = VR::get();
    if (!vr->is_openvr_loaded()) {
        return;
    }

    auto hmd = vr->get_hmd();
    if (hmd == nullptr) {
        return;
    }

    for (int eye = 0; eye < 2; ++eye) {
        float l = 0.0f, r = 0.0f, t = 0.0f, b = 0.0f;
        hmd->GetProjectionRaw((vr::EVREye)eye, &l, &r, &t, &b);

        if (std::isfinite(l) && std::isfinite(r) && std::fabs(r - l) > 1e-4f) {
            out[eye][0] = std::clamp(-(l + r) / (r - l), -1.0f, 1.0f);
        }
    }
}

void EyeGaze::update(bool poll_tracker) {
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 90.0f;

    if (m_last_update.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - m_last_update).count();
        dt = std::clamp(dt, 1.0f / 500.0f, 0.25f); // nach Pausen (Menue/Ladebildschirm) kein riesiger Sprung
    }

    m_last_update = now;

    auto vr = VR::get();

    if (!vr->is_openvr_loaded() || !vr->is_hmd_active() || !vr->get_runtime()->is_openvr()) {
        m_status = Status::NOT_OPENVR;
        std::scoped_lock _{m_mutex};
        m_state = {};
        m_filter[0].reset();
        m_filter[1].reset();
        return;
    }

    if (poll_tracker && !ensure_interface()) {
        m_status = Status::INTERFACE_MISSING;
    }

    if (m_fallback_t.time_since_epoch().count() == 0 || now - m_fallback_t >= std::chrono::seconds(1)) {
        compute_fallback_centers(m_fallback_cache);
        m_fallback_t = now;
    }

    float fallback[2][2]{};
    std::copy(&m_fallback_cache[0][0], &m_fallback_cache[0][0] + 4, &fallback[0][0]);

    {
        std::scoped_lock lock{m_mutex};
        std::copy(&fallback[0][0], &fallback[0][0] + 4, &m_lens_center[0][0]);
    }

    bool valid = false;
    vr::HmdVector2_t ndc[2]{};

    if (poll_tracker && m_sys026 != nullptr) {
        // Optional: dem Runtime melden, dass wir gegen SDK 2.15.6 bauen. Ein Runtime darf
        // Eye-Tracking-Daten davon abhaengig machen; Fehler sind harmlos (z.B. "schon gemeldet").
        if (m_options.declare_sdk_version && !m_declared_sdk) {
            m_declared_sdk = true;
            const auto e = m_sys026->SetSDKVersion(vr_ext::k_nSdkMajor, vr_ext::k_nSdkMinor, vr_ext::k_nSdkBuild);
            REVR_LOG(spdlog::info("[EyeGaze] SetSDKVersion({}.{}.{}) -> {}", vr_ext::k_nSdkMajor, vr_ext::k_nSdkMinor, vr_ext::k_nSdkBuild, (int)e));
        }

        valid = m_sys026->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1]);
        m_status = valid ? Status::TRACKING : Status::NO_EYE_TRACKER;
    }

    std::scoped_lock _{m_mutex};

    m_last_raw = {ndc[0].v[0], ndc[0].v[1], ndc[1].v[0], ndc[1].v[1]};

    for (int eye = 0; eye < 2; ++eye) {
        float x = ndc[eye].v[0];
        float y = ndc[eye].v[1];

        if (m_options.invert_x) x = -x;
        if (m_options.invert_y) y = -y;

        x += m_options.offset_x;
        y += m_options.offset_y;

        const auto out = m_filter[eye].update(valid, x, y, fallback[eye][0], fallback[eye][1], dt);

        m_state[eye].tracking = out.tracking;
        m_state[eye].x = out.x;
        m_state[eye].y = out.y;
        m_state[eye].radius_scale = out.radius_scale;
    }
}
