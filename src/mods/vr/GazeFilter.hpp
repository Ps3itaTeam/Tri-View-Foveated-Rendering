// GazeFilter.hpp -- filtering of the gaze point (portable, unit-tested in tests/gaze_filter_test.cpp).
//
//  * foveation::OneEuro    : One Euro filter for one scalar.
//  * foveation::GazeFilter : One Euro filter per axis plus holding the last gaze on a short loss (blink) and a smooth
//                            fall back to a fallback position (lens centre) afterwards.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace foveation {

// Klassischer One-Euro-Filter (Casiez et al. 2012) fuer EINEN Skalar.
class OneEuro {
public:
    void configure(float min_cutoff_hz, float beta, float d_cutoff_hz = 1.0f) {
        m_min_cutoff = min_cutoff_hz;
        m_beta = beta;
        m_d_cutoff = d_cutoff_hz;
    }

    void reset() { m_initialized = false; }

    float filter(float x, float dt) {
        if (!m_initialized || dt <= 0.0f) {
            m_initialized = true;
            m_x = x;
            m_dx = 0.0f;
            return x;
        }

        const float dx = (x - m_x) / dt;
        const float a_d = alpha(m_d_cutoff, dt);
        m_dx = a_d * dx + (1.0f - a_d) * m_dx;

        const float cutoff = m_min_cutoff + m_beta * std::fabs(m_dx);
        const float a = alpha(cutoff, dt);
        m_x = a * x + (1.0f - a) * m_x;

        return m_x;
    }

private:
    static float alpha(float cutoff, float dt) {
        const float tau = 1.0f / (2.0f * 3.14159265358979f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float m_min_cutoff{1.5f};
    float m_beta{0.02f};
    float m_d_cutoff{1.0f};
    float m_x{0.0f};
    float m_dx{0.0f};
    bool m_initialized{false};
};

// Blickpunkt eines Auges: filtert, haelt bei kurzem Verlust (Blinzeln) und faellt danach weich
// auf eine Rueckfall-Position (Linsenmitte) und einen groesseren Fovea-Radius zurueck.
class GazeFilter {
public:
    struct Output {
        float x{0.0f};           // NDC
        float y{0.0f};           // NDC
        float radius_scale{1.0f}; // >= 1: Fovea-Radien damit multiplizieren
        bool tracking{false};     // true = es liegt gerade ein gueltiger Blick an
    };

    struct Settings {
        float min_cutoff_hz{2.0f};   // niedrig = ruhiger, mehr Latenz
        float beta{0.05f};           // hoeher = schneller bei Sakkaden
        float hold_seconds{0.15f};   // so lange bleibt der letzte Blick nach Verlust stehen
        float fallback_seconds{0.40f}; // danach so lange dauert der Uebergang zur Rueckfall-Position
        float fallback_radius_scale{1.35f}; // Fovea im Rueckfall etwas groesser
    };

    void configure(const Settings& s) {
        m_s = s;
        m_fx.configure(s.min_cutoff_hz, s.beta);
        m_fy.configure(s.min_cutoff_hz, s.beta);
    }

    void reset() {
        m_fx.reset();
        m_fy.reset();
        m_time_since_valid = 1e9f;
        m_have_last = false;
    }

    // valid=false: Tracker liefert nichts. (fx,fy) = Rueckfall-Position in NDC.
    Output update(bool valid, float x, float y, float fx, float fy, float dt) {
        Output o{};

        if (valid && std::isfinite(x) && std::isfinite(y)) {
            x = std::clamp(x, -1.5f, 1.5f);
            y = std::clamp(y, -1.5f, 1.5f);

            m_last_x = m_fx.filter(x, dt);
            m_last_y = m_fy.filter(y, dt);
            m_have_last = true;
            m_time_since_valid = 0.0f;

            o.x = m_last_x;
            o.y = m_last_y;
            o.tracking = true;
            return o;
        }

        m_time_since_valid += dt;

        if (!m_have_last) {
            o.x = fx;
            o.y = fy;
            o.radius_scale = m_s.fallback_radius_scale;
            return o;
        }

        if (m_time_since_valid <= m_s.hold_seconds) {
            o.x = m_last_x;
            o.y = m_last_y;
            return o;
        }

        const float t = std::clamp((m_time_since_valid - m_s.hold_seconds) / (std::max)(m_s.fallback_seconds, 1e-3f), 0.0f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t); // smoothstep

        o.x = m_last_x + (fx - m_last_x) * s;
        o.y = m_last_y + (fy - m_last_y) * s;
        o.radius_scale = 1.0f + (m_s.fallback_radius_scale - 1.0f) * s;

        return o;
    }

private:
    Settings m_s{};
    OneEuro m_fx{};
    OneEuro m_fy{};
    float m_last_x{0.0f};
    float m_last_y{0.0f};
    float m_time_since_valid{1e9f};
    bool m_have_last{false};
};

} // namespace foveation
