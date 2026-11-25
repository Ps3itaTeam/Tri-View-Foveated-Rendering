// EyeGaze.hpp -- Blickposition fuer das Dynamic Foveated Rendering (OpenVR).
//
// Quelle: IVRSystem::GetEyeTrackedFoveationCenter() aus OpenVR SDK 2.15.x (siehe
// openvr_ext/IVRSystem_026.hpp). Das liefert pro Auge den Blickpunkt in NDC. Fehlt ein Eye Tracker
// (oder ein neuer SteamVR-Runtime), bleibt der Provider auf "nicht verfuegbar" und die Foveation
// arbeitet mit festem Zentrum (Fixed Foveated Rendering).
//
// Aufruf: update() EINMAL pro Frame auf dem Render-/Hauptthread; get() ist von jedem Thread aus lesbar.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "GazeFilter.hpp"

namespace vr_ext {
class IVRSystem_026;
}

class EyeGaze {
public:
    struct Options {
        bool  invert_x{false};
        bool  invert_y{false};
        float offset_x{0.0f};        // NDC-Offset (Kalibrierung)
        float offset_y{0.0f};
        float smoothing_hz{2.0f};    // One-Euro min_cutoff: niedriger = ruhiger
        float saccade_boost{0.05f};  // One-Euro beta
        float hold_seconds{0.15f};
        float fallback_seconds{0.40f};
        bool  declare_sdk_version{true}; // SetSDKVersion(2,15,6) einmalig an den Runtime melden
    };

    struct EyeState {
        bool  tracking{false};   // gerade gueltiger Blick vom Tracker
        float x{0.0f};           // NDC
        float y{0.0f};           // NDC
        float radius_scale{1.0f};
    };

    enum class Status : uint8_t {
        NOT_OPENVR,          // anderes Runtime / HMD nicht aktiv
        INTERFACE_MISSING,   // SteamVR kennt IVRSystem_026 nicht (zu alt)
        NO_EYE_TRACKER,      // Interface da, aber der Aufruf liefert false
        TRACKING             // Blick kommt an
    };

    static EyeGaze& get();

    void set_options(const Options& o);
    const Options& options() const { return m_options; }

    // Einmal pro Frame. dt wird intern gemessen.
    // poll_tracker=false (fixed foveation): only the lens centres are refreshed; the eye tracker is not
    // queried and the SDK version is not reported to SteamVR.
    void update(bool poll_tracker = true);

    EyeState get_eye(int eye) const;
    Status status() const { return m_status; }
    const char* status_text() const;

    // Lens centre of an eye in NDC (where a straight-ahead gaze lands) -- the position used by fixed foveation
    std::array<float, 2> lens_center(int eye) const;

    // Fuer die UI: letzter roher Wert vom Tracker (vor Filter/Offset)
    std::array<float, 4> last_raw() const { return m_last_raw; }

private:
    bool ensure_interface();
    void compute_fallback_centers(float out[2][2]);

    Options m_options{};
    foveation::GazeFilter m_filter[2]{};
    std::array<EyeState, 2> m_state{};
    std::array<float, 4> m_last_raw{};
    float m_lens_center[2][2]{};

    vr_ext::IVRSystem_026* m_sys026{nullptr};
    bool m_tried_interface{false};
    bool m_declared_sdk{false};
    uint32_t m_interface_retry_frames{0};
    Status m_status{Status::NOT_OPENVR};

    std::chrono::steady_clock::time_point m_last_update{};
    // [CPU 04.11.2026] the lens centres from the headset's raw projections, asked again once a second (two OpenVR calls
    // per frame at BeginRendering before; they only change when the runtime's projection does)
    float m_fallback_cache[2][2]{};
    std::chrono::steady_clock::time_point m_fallback_t{};
    mutable std::mutex m_mutex{};
};
