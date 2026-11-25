// Standalone test of src/mods/vr/GazeFilter.hpp (no Windows / D3D12 dependency).
//   g++ -std=c++20 -Wall -Wextra -I src/mods/vr tests/gaze_filter_test.cpp -o gaze_filter_test && ./gaze_filter_test
#include <cmath>
#include <cstdio>
#include "GazeFilter.hpp"

using namespace foveation;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // 1) valid gaze -> followed; loss -> held -> smoothly to the fallback position with a larger radius
    {
        GazeFilter g; GazeFilter::Settings s; g.configure(s);
        GazeFilter::Output o{};
        for (int i = 0; i < 20; ++i) o = g.update(true, 0.5f, -0.25f, 0.f, 0.f, 1.f / 90.f);
        CHECK(o.tracking && std::fabs(o.x - 0.5f) < 0.01f && std::fabs(o.y + 0.25f) < 0.01f);
        o = g.update(false, 0, 0, 0, 0, 0.05f);                // 50 ms lost: held
        CHECK(!o.tracking && std::fabs(o.x - 0.5f) < 0.01f && o.radius_scale == 1.0f);
        for (int i = 0; i < 60; ++i) o = g.update(false, 0, 0, 0, 0, 1.f / 90.f); // ~0.67 s -> fallback
        CHECK(std::fabs(o.x) < 0.02f && o.radius_scale > 1.3f);
        o = g.update(true, -0.9f, 0.9f, 0, 0, 1.f / 90.f);     // back again
        CHECK(o.tracking && o.radius_scale == 1.0f);
    }
    // 2) NaN / Inf from the tracker is treated as "invalid"
    {
        GazeFilter g; g.configure({});
        auto o = g.update(true, NAN, 0.f, 0.1f, 0.2f, 0.01f);
        CHECK(!o.tracking && std::isfinite(o.x) && std::isfinite(o.y));
    }

    std::printf(g_fail == 0 ? "ALL TESTS OK\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
