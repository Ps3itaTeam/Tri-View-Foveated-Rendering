// Standalone test for src/mods/vr/QuadView.hpp (no Windows / D3D12 / glm needed).
//   g++ -std=c++20 -Wall -Wextra -I src/mods/vr tests/quadview_math_test.cpp -o quadview_math_test && ./quadview_math_test
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <array>
#include <atomic>
#include <vector>

#include "QuadView.hpp"
#include "CpuStats.hpp"
#include "GpuGuard.hpp"
#include "SubmitMath.hpp"
#include "../../../shared/sdk/NameCache.hpp"

#include <string>

using namespace quadview;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
#define NEAR(a, b, eps) (std::fabs((a) - (b)) <= (eps))

// column-major 4x4, m[col][row] like glm
struct Mat4 {
    float c[4][4]{};
    float* operator[](int i) { return c[i]; }
    const float* operator[](int i) const { return c[i]; }
};

// Off-axis perspective (right handed, view looks down -z, D3D depth 0..1), from tangents like OpenVR's GetProjectionRaw
static Mat4 off_axis(float l, float r, float b, float t, float n, float f) {
    Mat4 m{};
    m[0][0] = 2.0f / (r - l);
    m[1][1] = 2.0f / (t - b);
    m[2][0] = (r + l) / (r - l);
    m[2][1] = (t + b) / (t - b);
    m[2][2] = f / (n - f);
    m[2][3] = -1.0f;
    m[3][2] = n * f / (n - f);
    return m;
}

static void project(const Mat4& m, float x, float y, float z, float& nx, float& ny) {
    const float cx = m[0][0] * x + m[1][0] * y + m[2][0] * z + m[3][0];
    const float cy = m[0][1] * x + m[1][1] * y + m[2][1] * z + m[3][1];
    const float cw = m[0][3] * x + m[1][3] * y + m[2][3] * z + m[3][3];
    nx = cx / cw;
    ny = cy / cw;
}

int main() {
    // 1) Layout for the user's headset: 3060x3120 eye image, fovea 40 %, fovea ratio 1.2
    {
        const auto l = compute_layout(3060, 3120, 0.40f, 1.20f);
        CHECK(l.valid);
        CHECK(l.inset_w == 1224 && l.inset_h == 1248);
        CHECK(l.render_w % 8 == 0 && l.render_h % 8 == 0);
        CHECK(l.render_w <= l.inset_w && l.render_h <= l.inset_h);
        CHECK(l.fovea_quality == PluginQuality::Native);    // ratio ~1.2 -> context sized for 1.0
        // The invariant that matters: the context's max render size (display / quality ratio) holds the real render size
        CHECK((float)l.inset_w / quality_for_ratio(l.fovea_ratio).ratio >= (float)l.render_w - 0.5f);

        const double now = 2040.0 * 2080.0;                  // today: FSR4 Quality, one view
        const double qv = 2.0 * l.render_w * l.render_h;      // quad view: two views
        std::printf("layout 40%%/1.2: inset %ux%u, render %ux%u x2 = %.2f MP (%.0f%% of today's %.2f MP), wide x%.2f, fovea x%.2f\n",
            l.inset_w, l.inset_h, l.render_w, l.render_h, qv / 1e6, qv / now * 100.0, now / 1e6, l.wide_ratio, l.fovea_ratio);
        CHECK(qv < now);
    }

    // 2) Native fovea never asks the upscaler to DOWNscale
    {
        const auto l = compute_layout(3060, 3120, 0.40f, 1.0f);
        CHECK(l.render_w == l.inset_w && l.render_h == l.inset_h);
        CHECK(l.fovea_quality == PluginQuality::Native);
    }

    // 3) quality_for_ratio never returns a ratio above the wanted one
    for (float r = 1.0f; r <= 4.0f; r += 0.05f) {
        CHECK(quality_for_ratio(r).ratio <= r + 1e-3f);
    }

    // 4) Rect placement: centre, clamping at the borders, even coordinates
    {
        const auto l = compute_layout(3060, 3120, 0.40f, 1.2f);
        const auto c = place_rect(l, 0.0f, 0.0f);
        CHECK(std::abs((c.x + (int)c.w / 2) - 1530) <= 2 && std::abs((c.y + (int)c.h / 2) - 1560) <= 2);
        CHECK((c.x & 1) == 0 && (c.y & 1) == 0);

        const auto tl = place_rect(l, -1.0f, 1.0f); // top left
        CHECK(tl.x == 0 && tl.y == 0);
        const auto br = place_rect(l, 1.0f, -1.0f); // bottom right
        CHECK(br.x + (int)br.w <= 3060 && br.y + (int)br.h <= 3120);
        CHECK(br.x >= 3060 - (int)br.w - 1 && br.y >= 3120 - (int)br.h - 1);

        const auto up = place_rect(l, 0.0f, 0.5f);  // gaze up -> rectangle higher in the image (smaller y)
        CHECK(up.y < c.y);

        const auto nan = place_rect(l, NAN, INFINITY);
        CHECK(nan.x >= 0 && nan.y >= 0);
    }

    // 4b) the rectangle always stays inside the image: near an edge it stops there
    {
        const auto l = compute_layout(3060, 3120, 0.40f, 1.2f);
        const auto in = place_rect(l, 0.9f, 0.0f);
        CHECK(in.x + (int)in.w <= 3060 && in.x >= 0);
        const auto corner = place_rect(l, -1.0f, 1.0f);
        CHECK(corner.x == 0 && corner.y == 0);
        const auto far = place_rect(l, 5.0f, -5.0f);
        CHECK(far.x == place_rect(l, 1.0f, -1.0f).x && far.y == place_rect(l, 1.0f, -1.0f).y);
        CHECK(far.x + (int)far.w <= 3060 && far.y + (int)far.h <= 3120);
    }

    // 5) Sub-frustum on a real asymmetric HMD projection: the rectangle's NDC corners map to +-1 and a 3D point
    //    projected through the inset matrix lands where the composite will put it.
    {
        const auto l = compute_layout(3060, 3120, 0.40f, 1.2f);
        const auto r = place_rect(l, 0.35f, -0.2f);
        const auto n = rect_to_ndc(l, r);
        const auto a = sub_frustum(n);

        CHECK(NEAR(n.x0 * a.sx + a.ox, -1.0f, 1e-5f) && NEAR(n.x1 * a.sx + a.ox, 1.0f, 1e-5f));
        CHECK(NEAR(n.y0 * a.sy + a.oy, -1.0f, 1e-5f) && NEAR(n.y1 * a.sy + a.oy, 1.0f, 1e-5f));

        const Mat4 wide = off_axis(-1.39f, 1.24f, -1.47f, 1.41f, 0.1f, 1000.0f); // ~Index-like canted tangents
        Mat4 inset = wide;
        apply_sub_frustum(inset, a);

        int checked = 0;
        for (float x = -3.0f; x <= 3.0f; x += 0.37f) {
            for (float y = -3.0f; y <= 3.0f; y += 0.41f) {
                float wx, wy, ix, iy;
                project(wide, x, y, -2.5f, wx, wy);
                project(inset, x, y, -2.5f, ix, iy);

                // inset NDC must be the affine image of the wide NDC
                CHECK(NEAR(ix, wx * a.sx + a.ox, 1e-4f) && NEAR(iy, wy * a.sy + a.oy, 1e-4f));

                // a point inside the rectangle: pixel position in the eye image == pixel position in the fovea output
                if (wx > n.x0 && wx < n.x1 && wy > n.y0 && wy < n.y1) {
                    const float px_wide = (wx * 0.5f + 0.5f) * l.display_w;
                    const float py_wide = (1.0f - (wy * 0.5f + 0.5f)) * l.display_h;
                    const float px_fov = r.x + (ix * 0.5f + 0.5f) * r.w;
                    const float py_fov = r.y + (1.0f - (iy * 0.5f + 0.5f)) * r.h;
                    CHECK(NEAR(px_wide, px_fov, 0.05f) && NEAR(py_wide, py_fov, 0.05f));
                    ++checked;
                }
            }
        }

        CHECK(checked > 10);
        // depth (z, w) must be untouched -> same depth buffer values in both views
        for (int col = 0; col < 4; ++col) {
            CHECK(inset[col][2] == wide[col][2] && inset[col][3] == wide[col][3]);
        }
    }

    // 6) State: requests bump the generation only on settings that need a re-init; dead zone
    {
        auto& s = State::get();
        Settings st{};
        const auto g0 = s.generation();
        s.set_request(true, st);
        const auto g1 = s.generation();
        CHECK(g1 != g0);
        st.feather = 0.3f;                      // blend width: no re-init
        s.set_request(true, st);
        CHECK(s.generation() == g1);
        CHECK(!s.engaged());

        s.set_layout(compute_layout(3060, 3120, st.fraction, st.fovea_ratio), g1);
        CHECK(s.engaged());

        NdcRect a{}, b{};
        s.set_gaze(0, 0.0f, 0.0f);
        CHECK(s.inset_for_eye(0, a));
        s.set_gaze(0, 0.005f, 0.0f);            // tiny move: stays
        CHECK(s.inset_for_eye(0, b));
        CHECK(a.x0 == b.x0 && a.y0 == b.y0);
        s.set_gaze(0, 0.3f, 0.0f);              // real move: follows
        CHECK(s.inset_for_eye(0, b));
        CHECK(b.x0 > a.x0);

        NdcRect c{}, d{};                       // one rectangle per frame: the first call decides
        s.set_gaze(0, -0.3f, 0.0f);
        CHECK(s.inset_for_eye(0, c, 7));
        s.set_gaze(0, 0.3f, 0.0f);
        CHECK(s.inset_for_eye(0, d, 7));
        CHECK(c.x0 == d.x0 && c.y0 == d.y0);
        CHECK(s.inset_for_eye(0, d, 8));        // next frame follows the gaze
        CHECK(d.x0 > c.x0);

        Rect used{};
        CHECK(s.rect_for_eye(0, used));
        CHECK(!s.rect_for_eye(1, used));        // right eye not rendered yet

        // [FRAME RECT 04.11.2026] the compositing of frame 8 runs while frame 9 already asks for its projection (the gaze
        // moved): frame 8 is composited with the rectangle it was rendered with, not the newer one
        Rect r8{}, r9{}, now{};
        bool newer = true;
        CHECK(s.rect_for_frame(0, 8, r8, &newer));
        CHECK(!newer);
        s.set_gaze(0, -0.3f, 0.0f);
        NdcRect e{};
        CHECK(s.inset_for_eye(0, e, 9));
        CHECK(e.x0 < d.x0);
        CHECK(s.rect_for_eye(0, now));          // the current rectangle is frame 9's
        CHECK(s.rect_for_frame(0, 8, r8, &newer));
        CHECK(newer);
        CHECK(r8.x != now.x);
        CHECK(quadview::rect_to_ndc(s.layout(), r8).x0 == d.x0); // what frame 8 was rendered with
        CHECK(s.rect_for_frame(0, 9, r9, &newer));
        CHECK(!newer && r9.x == now.x);
        CHECK(s.rect_for_frame(0, 1000, r9, &newer)); // a frame not kept: the current one, nothing to tell
        CHECK(!newer && r9.x == now.x);

        // the wide view's world -> clip matrix of each frame (GUI anchors are projected with the composited frame's)
        float m8[16]{}, m9[16]{}, got[16]{};
        m8[0] = 8.0f;
        m9[0] = 9.0f;
        s.set_centre_view_proj(m8, 8);
        s.set_centre_view_proj(m9, 9);
        CHECK(s.centre_view_proj(got, 8, &newer) && got[0] == 8.0f && newer);
        CHECK(s.centre_view_proj(got, 9, &newer) && got[0] == 9.0f && !newer);
        CHECK(s.centre_view_proj(got) && got[0] == 9.0f);
        CHECK(s.centre_view_proj(got, 3, &newer) && got[0] == 9.0f && !newer); // not kept: the latest

        st.fraction = 0.5f;                     // size change: re-init, layout dropped until the new instances exist
        s.set_request(true, st);
        CHECK(!s.engaged());
        CHECK(!s.inset_for_eye(0, a));

        // setup failure: request drops (no clone camera) until the settings change
        CHECK(s.requested());
        s.mark_failed(s.generation(), "upscaler init failed");
        CHECK(!s.requested());
        CHECK(!s.failure().empty());
        s.set_request(true, st);                // same settings every frame: stays failed
        CHECK(!s.requested());
        st.fovea_ratio = 1.0f;                  // user changes something: retry
        s.set_request(true, st);
        CHECK(s.requested());
        CHECK(s.failure().empty());
        s.mark_failed(s.generation() - 1, "stale");  // a failure for an older generation is ignored
        CHECK(s.requested());

        // [FOVEA LINEAR 05.11.2026] linear light, sharpening and the fovea's anti-aliasing switch without a re-init; the
        // upscaler method re-creates the upscalers
        const auto g2 = s.generation();
        st.upscaler_linear = !st.upscaler_linear;
        st.upscaler_sharpen = 0.7f;
        st.fovea_mode = FoveaMode::TAA;
        s.set_request(true, st);
        CHECK(s.generation() == g2);
        st.upscaler = st.upscaler == Upscaler::FSR4 ? Upscaler::FSR3 : Upscaler::FSR4;
        s.set_request(true, st);
        CHECK(s.generation() != g2);
        const auto g3 = s.generation();
        s.set_request(true, st);
        CHECK(s.generation() == g3);

        // [FSR DIRECT 06.11.2026] FSR 4 by the mod, one pass, its exposure: each re-creates the upscalers; request_reinit
        // too (the same settings, a new generation, no failure)
        st.fsr_direct = !st.fsr_direct;
        s.set_request(true, st);
        CHECK(s.generation() != g3);
        auto g4 = s.generation();
        st.fsr_one_pass = !st.fsr_one_pass;
        s.set_request(true, st);
        CHECK(s.generation() != g4);
        g4 = s.generation();
        st.fsr_auto_exposure = !st.fsr_auto_exposure;
        s.set_request(true, st);
        CHECK(s.generation() != g4);
        g4 = s.generation();
        s.set_request(true, st);
        CHECK(s.generation() == g4);
        s.request_reinit();
        CHECK(s.generation() == g4 + 1);
        CHECK(s.requested() && s.failure().empty());
        s.set_request(true, st);
        CHECK(s.generation() == g4 + 1);
    }

    // 10) [CPU 03.11.2026] the camera hooks from many threads: every thread gets the rectangle the first call of the frame
    // latched (the threads keep it without the lock), a new layout is seen at once, the sharded counters add up
    {
        auto& s = State::get();
        Settings st{};
        st.fraction = 0.37f;
        s.set_request(true, st);
        const auto g = s.generation();
        s.set_layout(compute_layout(3400, 3468, st.fraction, st.fovea_ratio), g);
        CHECK(s.engaged());
        ShardedCounter calls{};
        std::atomic<int> bad{0};

        for (int64_t frame = 100; frame < 140; ++frame) {
            s.set_gaze(1, (float)(frame % 7) * 0.1f - 0.3f, 0.05f);
            NdcRect first{};
            CHECK(s.inset_for_eye(1, first, frame));
            std::vector<std::thread> threads{};

            for (int t = 0; t < 8; ++t) {
                threads.emplace_back([&, frame] {
                    for (int k = 0; k < 200; ++k) {
                        NdcRect r{};

                        if (!s.inset_for_eye(1, r, frame) || r.x0 != first.x0 || r.y0 != first.y0 || r.x1 != first.x1) {
                            ++bad;
                        }

                        ++calls;
                        calls.fetch_add(1);
                    }
                });
            }

            s.set_gaze(1, 0.6f, -0.2f); // later in the frame: the latch holds
            NdcRect again{};
            CHECK(s.inset_for_eye(1, again, frame) && again.x0 == first.x0);

            for (auto& th : threads) {
                th.join();
            }
        }

        CHECK(bad.load() == 0);
        CHECK(calls.load() == 40ull * 8 * 200 * 2);

        // a new layout in the middle of a frame: no thread keeps the old one
        NdcRect before{}, after{};
        CHECK(s.inset_for_eye(1, before, 500));
        s.set_layout(compute_layout(3400, 3468, 0.25f, st.fovea_ratio), g);
        CHECK(s.inset_for_eye(1, after, 500));
        CHECK(after.x1 - after.x0 < before.x1 - before.x0);
        s.clear_layout();
        CHECK(!s.engaged());
        CHECK(!s.inset_for_eye(1, after, 501));
    }

    // 9) the fovea context sized to render exactly at the render size: "Exact 1/3" on a 2804x2860 headset
    {
        auto l = compute_layout(2804, 2860, 0.3334f, 1.0f);
        QualityRatio fq{};
        CHECK(snap_quality(l.fovea_ratio, 0.04f, fq));
        CHECK(fq.quality == PluginQuality::Native);
        const auto render_w = l.render_w;
        fit_fovea(l, fq);
        CHECK(l.inset_w == render_w); // Native context: display == render
        CHECK(NEAR(l.fovea_ratio, 1.0f, 1e-6f));

        const Rect r = place_rect(l, 0.321f, 0.0f);
        CHECK(r.x >= 0 && r.y >= 0 && r.x + (int32_t)r.w <= 2804 && r.y + (int32_t)r.h <= 2860);

        // Balanced: fovea ratio 1.2 is not a standard one (no snap), its quality never renders above the render size
        auto b = compute_layout(2804, 2860, 0.40f, 1.2f);
        CHECK(!snap_quality(b.fovea_ratio, 0.04f, fq));
        CHECK(quality_for_ratio(b.fovea_ratio).ratio * (float)b.render_w <= (float)b.inset_w + 0.5f);
        QualityRatio wq{};
        CHECK(!snap_quality(2.5f, 0.04f, wq));
        CHECK(snap_quality(1.52f, 0.04f, wq) && wq.quality == PluginQuality::Quality);
    }

    // ---- [MONO PERIPHERY 27.10.2026] one wide view for both eyes ----------------------------------------------------
    {
        // PSVR2 (log 27.10.2026): left eye l=-1.8418 r=0.9472, right eye mirrored, t/b +-1.329
        const float lL = -1.8418f, rL = 0.9472f, lR = -0.9472f, rR = 1.8418f, bb = -1.329f, tt = 1.329f;
        const Mat4 pl = off_axis(lL, rL, bb, tt, 0.1f, 1000.0f);
        const Mat4 pr = off_axis(lR, rR, bb, tt, 0.1f, 1000.0f);

        // tangents back from the matrix, the way VR::get_union_projection_matrix reads them (tan = (ndc + P20) / P00)
        CHECK(NEAR((-1.0f + pl[2][0]) / pl[0][0], lL, 1e-4f));
        CHECK(NEAR((1.0f + pl[2][0]) / pl[0][0], rL, 1e-4f));
        CHECK(NEAR((-1.0f + pr[2][0]) / pr[0][0], lR, 1e-4f));
        CHECK(NEAR((1.0f + pr[2][0]) / pr[0][0], rR, 1e-4f));

        // the union, as a rectangle in the left eye's NDC, pushed through the fovea's sub-frustum math
        const float lU = std::fmin(lL, lR), rU = std::fmax(rL, rR);
        NdcRect u{};
        u.x0 = pl[0][0] * lU - pl[2][0];
        u.x1 = pl[0][0] * rU - pl[2][0];
        u.y0 = pl[1][1] * bb - pl[2][1];
        u.y1 = pl[1][1] * tt - pl[2][1];
        CHECK(NEAR(u.x0, -1.0f, 1e-4f));
        CHECK(u.x1 > 1.0f); // reaches past the left eye's right edge
        Mat4 pu = pl;
        apply_sub_frustum(pu, sub_frustum(u));

        float nx = 0.0f, ny = 0.0f;
        project(pu, lU, 0.0f, -1.0f, nx, ny); // a direction at the union's left edge
        CHECK(NEAR(nx, -1.0f, 1e-3f));
        project(pu, rU, 0.0f, -1.0f, nx, ny);
        CHECK(NEAR(nx, 1.0f, 1e-3f));
        project(pu, 0.0f, tt, -1.0f, nx, ny);
        CHECK(NEAR(ny, 1.0f, 1e-3f));
        CHECK(NEAR(pu[0][0], 2.0f / (rU - lU), 1e-4f)); // symmetric: P00 = 1 / tan, P20 = 0
        CHECK(NEAR(pu[2][0], 0.0f, 1e-4f));

        // layout with the wider render target: same pixel density, fovea rectangle wider by the same factor
        const float xf = (rU - lU) / (rL - lL);
        CHECK(NEAR(xf, 1.3208f, 1e-3f));
        const auto m = compute_layout(3400, 3468, 0.30f, 1.2f, xf);
        const auto e = compute_layout(3400, 3468, 0.30f, 1.2f);
        CHECK(m.valid && NEAR(m.wide_x_factor, xf, 1e-6f));
        CHECK(m.inset_h == e.inset_h && m.render_h == e.render_h);
        CHECK(m.inset_w > e.inset_w && m.render_w > e.render_w);
        CHECK(NEAR((float)m.render_w / (float)e.render_w, xf, 0.02f));
        CHECK(NEAR((float)m.eye_wide_w, (float)m.render_w / xf, 1.0f));
        std::printf("mono layout 30%%/1.2: render %ux%u (eye share %ux%u), fovea %ux%u = %.2f MP for 3 views\n", m.render_w, m.render_h, m.eye_wide_w,
            m.eye_wide_h, m.inset_w, m.inset_h, 3.0 * m.render_w * m.render_h / 1e6);

        // reprojection: a point at distance d on the ray of output column x_e of the left eye lies in the wide image at
        // x_e + offset + (pixels per tangent) * eye_x / d, with eye_x = -0.03 m (shader: shift = disparity / d)
        const float rho = (float)m.render_w / (rU - lU);
        const float offset_l = (lL - lU) * rho;
        const float offset_r = (lR - lU) * rho;
        CHECK(NEAR(offset_l, 0.0f, 1e-3f));
        CHECK(offset_r > 200.0f);
        const float eye_x = -0.03f;
        const float d = 0.5f;
        const float tan_c = 0.2f;                // direction of a point as the wide (centre) camera sees it
        const float tan_e = tan_c + 0.03f / d;   // the left eye, 3 cm to the left, sees it further right
        const float x_c = (tan_c - lU) * rho;    // its column in the wide image
        const float x_e = (tan_e - lL) * rho;    // its column in the left eye's periphery image (same density)
        CHECK(NEAR(x_c, x_e + offset_l + rho * eye_x / d, 1e-2f));
        std::printf("reprojection: %.1f px/tan, left eye shift at 0.5 m = %.1f px, at 2 m = %.1f px\n", rho, rho * eye_x / 0.5f, rho * eye_x / 2.0f);
    }

    // 11. [CPU 03.11.2026] hook meter: every call counted once (the part after the game's function does not count), from
    //     two threads at once; one call in 64 timed, counted 64 times: the estimate is near the real total (two threads:
    //     with more threads than processors the time around the calls holds the time the threads were taken off)
    {
        cpustats::HookMeter meter{};
        std::atomic<uint64_t> real_ns{0};
        std::vector<std::thread> threads{};

        for (int k = 0; k < 2; ++k) {
            threads.emplace_back([&meter, &real_ns] {
                uint64_t mine = 0;

                for (int i = 0; i < 20000; ++i) {
                    const auto t0 = cpustats::now_ns();
                    {
                        cpustats::HookTimer _a{meter};
                        volatile double x = 1.0;
                        for (int j = 0; j < 200; ++j) x = x * 1.0000001 + 0.5;
                    }
                    {
                        cpustats::HookTimer _b{meter, false};
                    }
                    mine += (uint64_t)(cpustats::now_ns() - t0);
                }

                real_ns.fetch_add(mine);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(meter.calls.load() == 2u * 20000u);
        const double est = (double)meter.ns.load();
        const double real = (double)real_ns.load();
        CHECK(est > 0.5 * real && est < 1.3 * real);
        std::printf("hook meter: %llu calls, estimated %.2f ms of %.2f ms measured around the calls\n", (unsigned long long)meter.calls.load(), est / 1e6, real / 1e6);
    }

    // 12. [TIMELINE 03.11.2026] marks from several threads while recording, none while off; each thread keeps its own id
    {
        auto& tl = cpustats::timeline();
        tl.mark(cpustats::Ev::EntryPre, 1); // off: not recorded
        tl.start();
        std::vector<std::thread> threads{};

        for (int k = 0; k < 4; ++k) {
            threads.emplace_back([&tl, k] {
                for (int i = 0; i < 1000; ++i) {
                    tl.mark(cpustats::Ev::LayerPre, (uint64_t)k, (uint16_t)k);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        auto ev = tl.stop();
        tl.mark(cpustats::Ev::EntryPre, 2); // off again
        CHECK(ev.size() == 4000u);
        bool same_thread_same_id = true;
        uint32_t ids[4]{};

        for (const auto& e : ev) {
            if (ids[e.sub] == 0) {
                ids[e.sub] = e.tid;
            }

            same_thread_same_id = same_thread_same_id && ids[e.sub] == e.tid && e.arg == e.sub && e.ev == (uint16_t)cpustats::Ev::LayerPre && e.t > 0;
        }

        CHECK(same_thread_same_id);
        CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[2] != ids[3]);
        CHECK(tl.stop().size() == 4000u); // nothing added while off
    }

    // 13. [CPU 04.11.2026] the per-thread front of the lookups by name: the same answers as the full lookup, the full
    // lookup only once per (owner, name), a different text at the same address or a long name not mistaken for a kept one
    {
        sdk::namecache::Table<int, 64, 16> t{};
        int full = 0;
        const auto lookup = [&full](const void* owner, std::string_view n) {
            ++full;
            return (int)((uintptr_t)owner & 0xFF) * 1000 + (int)n.size() * 10 + (n.empty() ? 0 : (int)n[0] % 10);
        };
        int owners[3]{};
        const char* names[] = {"get_Position", "get_Rotation", "set_Enabled", "", "findComponents(System.Type)"};
        bool same = true;

        for (int round = 0; round < 3; ++round) {
            for (auto& o : owners) {
                for (const char* n : names) {
                    const std::string_view nv{n};
                    const int v = t.get(&o, nv, [&] { return lookup(&o, nv); });
                    same = same && v == (int)((uintptr_t)&o & 0xFF) * 1000 + (int)nv.size() * 10 + (nv.empty() ? 0 : (int)nv[0] % 10);
                }
            }
        }

        CHECK(same);
        // 3 owners x 4 names that fit: looked up once each (no collisions in 64 slots for 12 keys is not guaranteed: at
        // most once per round); the long name (27 > 16) every time
        CHECK(full >= 3 * 4 + 3 * 3 && full <= 3 * (4 * 3 + 3));
        CHECK(t.hits() + t.misses() == 3u * 3u * 4u);

        // the same buffer with a new text: looked up again, and the right answer
        char buf[32] = "get_Visible";
        int before = full;
        const int a = t.get(&owners[0], std::string_view{buf}, [&] { return lookup(&owners[0], std::string_view{buf}); });
        std::memcpy(buf, "set_Visible", 11);
        const int b = t.get(&owners[0], std::string_view{buf}, [&] { return lookup(&owners[0], std::string_view{buf}); });
        CHECK(full == before + 2);
        CHECK(a != b && b == lookup(&owners[0], std::string_view{"set_Visible"}));

        // keep_empty false: a 0 answer is asked again next time
        int asked = 0;
        for (int i = 0; i < 3; ++i) {
            CHECK(t.get(&owners[1], "missing", [&] { ++asked; return 0; }, false) == 0);
        }
        CHECK(asked == 3);
        // keep_empty true (default): asked once
        asked = 0;
        for (int i = 0; i < 3; ++i) {
            CHECK(t.get(&owners[2], "missing", [&] { ++asked; return 0; }) == 0);
        }
        CHECK(asked == 1);

        // one table per thread: two threads with their own tables give the same answers
        std::atomic<int> bad{0};
        std::vector<std::thread> th{};
        for (int k = 0; k < 2; ++k) {
            th.emplace_back([&] {
                thread_local auto mine = std::make_unique<sdk::namecache::Table<int, 64, 16>>();
                for (int i = 0; i < 2000; ++i) {
                    auto& o = owners[i % 3];
                    const std::string_view nv{names[i % 4]};
                    const auto pure = [&o, nv] { return (int)((uintptr_t)&o & 0xFF) * 1000 + (int)nv.size() * 10 + (nv.empty() ? 0 : (int)nv[0] % 10); };

                    if (mine->get(&o, nv, pure) != pure()) {
                        ++bad;
                    }
                }
            });
        }
        for (auto& x : th) {
            x.join();
        }
        CHECK(bad.load() == 0);
    }

    // 14. [GLUE 04.11.2026] the sampled timer: about one call in EVERY is timed and counted EVERY times
    {
        cpustats::HookMeter m{};
        for (int i = 0; i < 64 * 2000; ++i) {
            cpustats::SampledTimer _t{m};
        }
        const double calls = (double)m.calls.load();
        CHECK(calls > 64.0 * 2000 * 0.85 && calls < 64.0 * 2000 * 1.15);
        CHECK(m.calls.load() % cpustats::HookMeter::EVERY == 0);
    }

    // 12) [GPU GUARD 05.11.2026] when the anti-aliasing gives way to its lighter form and when it comes back ([fix91]: the
    // mod's supersampling TAA to its lighter choices; then the upscaler plugin in the fovea to the mod's TAA)
    {
        using C = GpuGuard::Change;
        const GpuGuard::Window calm{45, 0, 9.0};
        const GpuGuard::Window heavy{45, 3, 10.4};   // 3 of 45 frames late (6.7 %)
        const GpuGuard::Window edge{45, 0, 11.0};    // no late frame yet, the GPU time at the edge of the frame
        const GpuGuard::Window burst{45, 8, 9.5};    // 18 % late at once

        GpuGuard g{};
        g.frame_ms = 11.12;
        CHECK(g.update(calm) == C::None);
        CHECK(g.update(heavy) == C::None);   // one late window is not enough
        CHECK(g.update(calm) == C::None);    // ... and a calm one in between starts again
        CHECK(g.update(heavy) == C::None);
        CHECK(g.update(heavy) == C::ToLighter);  // two in a row
        CHECK(g.relieved());

        GpuGuard g2{};
        g2.frame_ms = 11.12;
        CHECK(g2.update(burst) == C::ToLighter);

        GpuGuard g3{};
        g3.frame_ms = 11.12;
        CHECK(g3.update(edge) == C::None);
        CHECK(g3.update(edge) == C::ToLighter);
        CHECK(g3.update(GpuGuard::Window{}) == C::None); // no frames timed: nothing changes

        // on the lighter one with time to spare (8.5 + 0.5 <= 11.12 - 1.2): back after the 10 windows it holds at least
        int n = 0;
        C c = C::None;
        for (n = 1; n <= 30 && c == C::None; ++n) {
            c = g.update(GpuGuard::Window{45, 0, 8.5});
        }
        CHECK(c == C::ToFull && n - 1 == 10);
        CHECK(!g.relieved());

        // on the lighter one without enough to spare (9.5 + 0.5 > 9.92): stays
        CHECK(g.update(heavy) == C::None);
        CHECK(g.update(heavy) == C::ToLighter);
        bool back = false;
        for (int i = 0; i < 40; ++i) {
            back |= g.update(GpuGuard::Window{45, 0, 9.5}) == C::ToFull;
        }
        CHECK(!back && g.relieved());

        // a late frame among the calm windows starts the count again: 6 calm windows in a row after it
        for (int i = 0; i < 5; ++i) {
            CHECK(g.update(GpuGuard::Window{45, 0, 8.0}) == C::None);
        }
        CHECK(g.update(GpuGuard::Window{45, 1, 8.0}) == C::None);
        for (int i = 0; i < 5; ++i) {
            CHECK(g.update(GpuGuard::Window{45, 0, 8.0}) == C::None);
        }
        CHECK(g.update(GpuGuard::Window{45, 0, 8.0}) == C::ToFull);

        g.reset();
        CHECK(!g.relieved());

        // [fix96 19.11.2026] two steps: the periphery first, the fovea only if frames still come late; back one at a time
        {
            GpuGuard s2{};
            s2.frame_ms = 11.12;
            s2.levels = 2;
            CHECK(s2.update(heavy) == C::None);
            CHECK(s2.update(heavy) == C::ToLighter);
            CHECK(s2.level() == 1);
            // still late: the second step, but not before the first has had its windows (2), then two late in a row
            CHECK(s2.update(heavy) == C::None);
            CHECK(s2.update(heavy) == C::None);
            CHECK(s2.update(heavy) == C::ToLighter);
            CHECK(s2.level() == 2);
            CHECK(s2.update(heavy) == C::None); // no third step
            CHECK(s2.update(burst) == C::None);
            CHECK(s2.level() == 2);
            // calm: back to level 1 after the 10 windows held (the fovea's step costs 0.2 ms: 8.5 + 0.2 <= 9.92)
            s2.full_extra_ms = 0.2;
            C c2 = C::None;
            int n2 = 0;
            for (n2 = 1; n2 <= 40 && c2 == C::None; ++n2) {
                c2 = s2.update(GpuGuard::Window{45, 0, 8.5});
            }
            CHECK(c2 == C::ToFull && s2.level() == 1);
            // the periphery's step costs 0.45 ms: 9.3 + 0.45 <= 9.92 holds, but only after this level's own 10 windows
            s2.full_extra_ms = 0.45;
            c2 = C::None;
            for (n2 = 1; n2 <= 40 && c2 == C::None; ++n2) {
                c2 = s2.update(GpuGuard::Window{45, 0, 9.3});
            }
            CHECK(c2 == C::ToFull && n2 - 1 == 10 && s2.level() == 0 && !s2.relieved());
            // on level 1 with the GPU just holding (10.0 ms, no late frame): it stays there, no step either way
            CHECK(s2.update(burst) == C::ToLighter);
            bool moved = false;
            for (int i = 0; i < 40; ++i) {
                moved |= s2.update(GpuGuard::Window{45, 0, 10.0}) != C::None;
            }
            CHECK(!moved && s2.level() == 1);
            // one step only where there is one thing to lighten
            GpuGuard s1{};
            s1.frame_ms = 11.12;
            CHECK(s1.update(burst) == C::ToLighter);
            for (int i = 0; i < 6; ++i) {
                CHECK(s1.update(burst) == C::None);
            }
            CHECK(s1.level() == 1);
        }

        // [GPU GUARD fix91 19.11.2026] what the full anti-aliasing costs more decides when it comes back: with only one of
        // the two to lighten (0.25 ms) 9.6 + 0.25 <= 11.12 - 1.2 is enough (9.85 <= 9.92), with both (0.5 ms) it is not (10.1)
        GpuGuard g4{};
        g4.frame_ms = 11.12;
        g4.full_extra_ms = 0.25;
        CHECK(g4.update(heavy) == C::None);
        CHECK(g4.update(heavy) == C::ToLighter);
        C c4 = C::None;
        for (int i = 0; i < 30 && c4 == C::None; ++i) {
            c4 = g4.update(GpuGuard::Window{45, 0, 9.6});
        }
        CHECK(c4 == C::ToFull);
        GpuGuard g5{};
        g5.frame_ms = 11.12;
        g5.full_extra_ms = 0.5;
        CHECK(g5.update(heavy) == C::None);
        CHECK(g5.update(heavy) == C::ToLighter);
        bool back5 = false;
        for (int i = 0; i < 30; ++i) {
            back5 |= g5.update(GpuGuard::Window{45, 0, 9.6}) == C::ToFull;
        }
        CHECK(!back5); // both: 9.6 + 0.5 > 9.92

        // [GUARD fix101 20.11.2026] four steps, the last only for a frame rate that collapses; what the levels save given
        auto ladder = []() {
            GpuGuard g{};
            g.levels = 4;
            g.extreme_level = 4;
            g.save_ms[1] = 0.26;
            g.save_ms[2] = 0.47;
            g.save_ms[3] = 0.67;
            g.save_ms[4] = 0.85;
            return g;
        };
        // a little over the limit (10.81): one step; then, still late after the wait, the next one
        GpuGuard g6 = ladder();
        CHECK(g6.update(GpuGuard::Window{45, 0, 11.0}) == C::None);
        CHECK(g6.update(GpuGuard::Window{45, 0, 11.0}) == C::ToLighter && g6.level() == 1); // 0.19 over, the first step saves 0.26
        CHECK(g6.update(GpuGuard::Window{45, 0, 10.9}) == C::None && g6.update(GpuGuard::Window{45, 0, 10.9}) == C::None);
        CHECK(g6.update(GpuGuard::Window{45, 0, 10.9}) == C::ToLighter && g6.level() == 2);
        // far over the limit: as many steps as it asks for at once, but never the last one
        GpuGuard g7 = ladder();
        g7.update(GpuGuard::Window{45, 2, 11.4});
        CHECK(g7.update(GpuGuard::Window{45, 2, 11.4}) == C::ToLighter && g7.level() == 3); // 0.59 over: 0.26 and 0.47 are not enough
        GpuGuard g8 = ladder();
        CHECK(g8.update(GpuGuard::Window{45, 9, 13.0}) == C::ToLighter && g8.level() == 3); // 20 % late at once: all but the last
        // the last level: not with 10 % of the frames late, however long ...
        bool extreme = false;

        for (int i = 0; i < 20; ++i) {
            extreme |= g8.update(GpuGuard::Window{45, 20, 11.6}) == C::ToLighter; // (late, but not throttled: fix102)
        }

        CHECK(!extreme && g8.level() == 3);
        // ... with a quarter of them late two windows in a row, or two fifths of one
        CHECK(g8.update(GpuGuard::Window{45, 12, 12.5, 12}) == C::None);
        CHECK(g8.update(GpuGuard::Window{45, 12, 12.5, 12}) == C::ToLighter && g8.level() == 4);
        GpuGuard g9 = ladder();
        g9.update(GpuGuard::Window{45, 9, 13.0});
        g9.update(GpuGuard::Window{45, 0, 10.0});
        g9.update(GpuGuard::Window{45, 0, 10.0});
        CHECK(g9.level() == 3 && g9.update(GpuGuard::Window{45, 20, 14.0, 20}) == C::ToLighter && g9.level() == 4);
        // back: one level at a time, the last level after its own (longer) hold
        g9.full_extra_ms = 0.18;
        int back_at = 0;

        for (int i = 1; i <= 30 && back_at == 0; ++i) {
            if (g9.update(GpuGuard::Window{45, 0, 8.0}) == C::ToFull) {
                back_at = i;
            }
        }

        CHECK(back_at == 12 && g9.level() == 3);
        back_at = 0;

        for (int i = 1; i <= 30 && back_at == 0; ++i) {
            if (g9.update(GpuGuard::Window{45, 0, 8.0}) == C::ToFull) {
                back_at = i;
            }
        }

        CHECK(back_at == 10 && g9.level() == 2);
    }

    // 13) [FSR DIRECT 06.11.2026] both foveas in one FSR 4 pass: the gap scales with the views, so FSR 4's mapping of the
    // whole output to the whole input puts each view's pixels exactly where its own pass would
    {
        // the user's layout of 06.11.2026: eye 3400x3468, fovea 0.276, ratio 1.0, shared wide view 1.321 wider
        const auto l = compute_layout(3400, 3468, 0.276f, 1.0f, 1.321f);
        CHECK(l.valid && l.inset_w == 1240 && l.inset_h == 960 && l.render_w == 1240 && l.render_h == 960);
        const auto a = fsr_atlas(l);
        CHECK(a.valid && a.gap_r == 64 && a.gap_o == 64);
        CHECK(a.render_w == 2544 && a.out_w == 2544 && a.render_h == 960 && a.out_h == 960);
        CHECK(a.right_render_x == 1304 && a.right_out_x == 1304);

        // ratios above 1: whole gaps on both sides, and the right view's first output pixel centre maps to its first input
        // pixel centre (FSR: in = (out + 0.5) * in_w / out_w - 0.5)
        int checked = 0;

        for (float ratio : {1.0f, 1.1f, 1.2f, 1.25f, 1.3f, 1.5f, 1.7f, 2.0f}) {
            for (float frac : {0.25f, 0.276f, 0.3f, 0.4f}) {
                const auto lr = compute_layout(3400, 3468, frac, ratio, 1.321f);
                const auto ar = fsr_atlas(lr);

                if (!ar.valid) {
                    continue; // no gap of whole pixels up to 160: one pass per eye
                }

                ++checked;
                CHECK(ar.gap_r >= 64 && ar.gap_r % 2 == 0 && ar.gap_r <= 160);
                CHECK((uint64_t)ar.gap_o * lr.render_w == (uint64_t)ar.gap_r * lr.inset_w);
                CHECK((uint64_t)ar.out_w * lr.render_w == (uint64_t)ar.render_w * lr.inset_w); // one ratio for the whole and each view
                const double s = (double)ar.render_w / (double)ar.out_w;
                const double right_first = (ar.right_out_x + 0.5) * s - 0.5;
                const double right_last = (ar.right_out_x + lr.inset_w - 1 + 0.5) * s - 0.5;
                const double left_last = (lr.inset_w - 1 + 0.5) * s - 0.5;
                const double own = (0.5) * ((double)lr.render_w / lr.inset_w) - 0.5; // the same pixel in a pass of its own
                CHECK(std::fabs((right_first - ar.right_render_x) - own) < 1e-6);
                CHECK(right_last <= ar.right_render_x + lr.render_w - 0.5 + 1e-6);
                CHECK(left_last <= lr.render_w - 0.5 + 1e-6);
            }
        }

        CHECK(checked >= 8);

        // the fill: left view's columns then its edge column into the first half of the gap; the right view's edge column
        // into the second half, then its columns
        const auto f0 = atlas_fill(a, l.render_w, 0);
        const auto f1 = atlas_fill(a, l.render_w, 1);
        CHECK(f0.out_x == 0 && f0.w == 1240 + 32 && f0.shift == 0);
        CHECK(f1.out_x == 1240 + 32 && f1.w == 1240 + 32 && f1.shift == -32);
        CHECK(f0.out_x + f0.w == f1.out_x && f1.out_x + f1.w == a.render_w);
        // the right view's column 0 lands at right_render_x
        CHECK((int)(a.right_render_x - f1.out_x) + f1.shift == 0);

        // nothing to fit: invalid layouts, a fovea smaller than its render size
        CHECK(!fsr_atlas(Layout{}).valid);
        Layout bad = l;
        bad.inset_w = l.render_w - 8;
        CHECK(!fsr_atlas(bad).valid);

        // [ONE PASS FIT] a layout with a gap stays as it is; the others get the nearest one with a gap (all slider sizes,
        // the presets, the shared wide view and stereo), render width first, a few steps of 8 at most
        {
            Layout same = l;
            CHECK(!fit_atlas(same));
            CHECK(same.inset_w == l.inset_w && same.render_w == l.render_w && same.eye_wide_w == l.eye_wide_w);
            Layout none{};
            CHECK(!fit_atlas(none) && !none.valid);

            // Balanced (0.40, 1.20) and Fast (0.35, 1.35) on this headset have no gap: one step of the render width
            auto bal = compute_layout(3400, 3468, 0.40f, 1.20f, 1.321f);
            CHECK(!fsr_atlas(bal).valid);
            const auto bal0 = bal;
            CHECK(fit_atlas(bal) && fsr_atlas(bal).valid);
            CHECK(bal.inset_w == bal0.inset_w && bal.render_w == bal0.render_w + 8);
            CHECK(bal.render_h == bal0.render_h && bal.inset_h == bal0.inset_h);
            CHECK(NEAR(bal.fovea_ratio, (float)bal.inset_w / (float)bal.render_w, 1e-6f));
            CHECK(NEAR(bal.wide_ratio, (float)bal.display_w / (float)bal.render_w, 1e-6f));
            CHECK(bal.eye_wide_w == (uint32_t)std::lround((float)bal.render_w / 1.321f));

            // the user's fovea with the slider at 1.20: render 1032 has no gap, 1024 and 1040 have one; the larger one first
            auto u120 = compute_layout(3400, 3468, 0.276f, 1.20f, 1.321f);
            CHECK(u120.render_w == 1032 && !fsr_atlas(u120).valid);
            CHECK(fit_atlas(u120) && u120.render_w == 1040 && u120.inset_w == 1240 && fsr_atlas(u120).valid);

            int fitted = 0;
            int moved_fovea = 0;
            int failed = 0;
            uint32_t worst_render = 0;
            uint32_t worst_fovea = 0;

            for (float wx : {1.0f, 1.321f, 1.5f}) {
                for (int fi = 15; fi <= 80; ++fi) {
                    for (int ri = 100; ri <= 300; ri += 1) {
                        const auto l0 = compute_layout(3400, 3468, (float)fi / 100.0f, (float)ri / 100.0f, wx);
                        auto l1 = l0;
                        const bool had = fsr_atlas(l0).valid;
                        const bool changed = fit_atlas(l1);

                        if (had) {
                            CHECK(!changed);
                            continue;
                        }

                        if (!changed || !fsr_atlas(l1).valid) {
                            ++failed;
                            continue;
                        }

                        ++fitted;
                        moved_fovea += l1.inset_w != l0.inset_w ? 1 : 0;
                        worst_render = (std::max)(worst_render, (uint32_t)std::abs((int)l1.render_w - (int)l0.render_w));
                        worst_fovea = (std::max)(worst_fovea, (uint32_t)std::abs((int)l1.inset_w - (int)l0.inset_w));
                        CHECK(l1.render_w % 8 == 0 && l1.inset_w % 8 == 0);
                        CHECK(l1.render_w <= l1.inset_w && l1.inset_w <= l1.display_w);
                        CHECK(l1.render_h == l0.render_h && l1.inset_h == l0.inset_h);
                    }
                }
            }

            CHECK(failed == 0);
            CHECK(fitted > 1000);
            CHECK(moved_fovea * 5 < fitted); // mostly the render width alone (06.11: 12 %)
            CHECK(worst_render <= 32 && worst_fovea <= 16);
            std::printf("one pass fit: %d layouts fitted (%d with the fovea width), at most %u / %u px\n", fitted, moved_fovea, worst_render,
                worst_fovea);
        }

        // the jitter sequence length (AMD: ceil(8 n^2), at least 8)
        CHECK(jitter_phase_count(1240, 1240) == 8);
        CHECK(jitter_phase_count(1000, 1200) == 12);
        CHECK(jitter_phase_count(1000, 1500) == 18);
        CHECK(jitter_phase_count(1000, 2000) == 32);
        CHECK(jitter_phase_count(0, 1240) == 8);
    }

    // ---- 14. [SUBMIT TIMELINE] one frame's submits: busy, idle by cause, names that stay the same, compute next to it ------
    {
        using submitmath::Call;
        const char* pre = "Tri-view pre-fovea copier";
        std::vector<Call> calls{};
        // engine on a worker: 0.5 .. 3.0 on the GPU (sent at 0.2)
        calls.push_back({0, true, nullptr, false, 5, 0.2, true, 0.5, 3.0});
        // a Wait on the game's queue, then engine again: sent at 1.0, the GPU starts at 3.4 (0.4 idle, not the CPU's fault)
        calls.push_back({3, true, nullptr, false, 0, 0.9, false, 0, 0});
        calls.push_back({0, true, nullptr, false, 7, 1.0, true, 3.4, 6.0});
        // the mod's list from the thread that presents, sent at 6.5 after the GPU had finished at 6.0: 0.5 idle by the CPU
        calls.push_back({0, true, pre, true, 1, 6.5, true, 6.5, 7.2});
        // engine on the render thread, sent at 7.0 while the GPU still works: no idle
        calls.push_back({0, true, nullptr, true, 2, 7.0, true, 7.2, 8.0});
        // compute queue: 2.0 .. 4.0 (overlaps the game's queue 2.0..3.0 and 3.4..4.0 = 1.6)
        calls.push_back({1, false, nullptr, false, 3, 1.5, true, 2.0, 4.0});
        // a copy queue submit and an untimed engine submit (over the limit)
        calls.push_back({2, false, nullptr, false, 1, 2.0, false, 0, 0});
        calls.push_back({0, true, nullptr, false, 1, 7.5, false, 0, 0});

        const auto r = submitmath::analyze(calls, 9.0);
        CHECK(r.has_game);
        CHECK(NEAR((float)r.span, 7.5f, 1e-6f));                   // 0.5 .. 8.0
        CHECK(NEAR((float)r.busy, 2.5f + 2.6f + 0.7f + 0.8f, 1e-6f));
        CHECK(NEAR((float)(r.span - r.busy), 0.9f, 1e-6f));        // 0.4 + 0.5
        CHECK(NEAR((float)r.idle_cpu, 0.5f, 1e-6f));
        CHECK(NEAR((float)r.idle_wait, 0.4f, 1e-6f));
        CHECK(NEAR((float)r.idle_other, 0.0f, 1e-6f));
        CHECK(NEAR((float)r.end_to_mark, 1.0f, 1e-6f));
        CHECK(r.calls_game == 5.0 && r.lists_game == 16.0 && r.calls_compute == 1.0 && r.calls_copy == 1.0 && r.waits == 1.0);
        CHECK(NEAR((float)r.busy_compute, 2.0f, 1e-6f));
        CHECK(NEAR((float)r.overlap_compute, 1.6f, 1e-6f));
        // the compute submit by its name, with its part while the game's queue worked
        CHECK(r.compute_keys.size() == 1 && r.compute_keys[0].key == "compute W#1" && NEAR((float)r.compute_keys[0].busy, 2.0f, 1e-6f) &&
              NEAR((float)r.compute_keys[0].overlap, 1.6f, 1e-6f) && NEAR((float)r.compute_keys[0].at, 2.0f, 1e-6f));
        // names: n-th submit of its kind, tag and thread in the frame
        CHECK(r.call_keys[0] == "engine W#1" && r.call_keys[2] == "engine W#2" && r.call_keys[3] == std::string{pre} + " R#1" &&
              r.call_keys[4] == "engine R#1" && r.call_keys[5] == "compute W#1" && r.call_keys[7] == "engine W#3");
        CHECK(r.keys.size() == 4 && r.keys[0].key == "engine W#1" && NEAR((float)r.keys[0].busy, 2.5f, 1e-6f));
        CHECK(r.gaps.size() == 2);
        CHECK(r.gaps[0].key == "after engine W#1 before engine W#2" && r.gaps[0].waited && NEAR((float)r.gaps[0].cpu_late, 0.0f, 1e-6f));
        CHECK(r.gaps[1].key == std::string{"after engine W#2 before "} + pre + " R#1" && !r.gaps[1].waited &&
              NEAR((float)r.gaps[1].cpu_late, 0.5f, 1e-6f));
        // the engine's first submit from the thread that presents (the mod's tagged one before it does not count): sent 7.0, GPU 7.2
        CHECK(NEAR((float)r.first_rt_delay, 0.2f, 1e-6f) && NEAR((float)r.first_rt_cpu, 7.0f, 1e-6f));
        CHECK(NEAR((float)r.last_end, 8.0f, 1e-6f));

        // the GPU order wins over the CPU order (two threads); idle with neither cause counts as other
        std::vector<Call> two{};
        two.push_back({0, true, nullptr, true, 1, 0.0, true, 4.0, 5.0});
        two.push_back({0, true, nullptr, false, 1, 0.1, true, 1.0, 2.0});
        const auto r2 = submitmath::analyze(two, 6.0);
        CHECK(NEAR((float)r2.span, 4.0f, 1e-6f) && NEAR((float)r2.busy, 2.0f, 1e-6f));
        CHECK(NEAR((float)r2.idle_other, 2.0f, 1e-6f) && r2.idle_cpu == 0.0 && r2.idle_wait == 0.0);
        CHECK(r2.gaps.size() == 1 && r2.gaps[0].key == "after engine W#1 before engine R#1");
        CHECK(NEAR((float)r2.first_rt_delay, 4.0f, 1e-6f)); // sent at 0.0, started at 4.0

        // nothing on the game's queue
        const auto r3 = submitmath::analyze({}, 5.0);
        CHECK(!r3.has_game && r3.span == 0.0 && r3.busy == 0.0 && r3.gaps.empty() && r3.first_rt_delay < 0.0);
    }

    // ---- 15. [FOVEA FOV TEST] the narrowest symmetric field of view that holds a rectangle of an (asymmetric) projection ----
    {
        // glm::frustum style (m23 = -1): tangents left -1.2, right 0.8, bottom -1.0, top 1.0 -> ndc = m00 * t - m20
        float p[4][4]{};
        p[0][0] = 2.0f / (0.8f + 1.2f);
        p[2][0] = (0.8f - 1.2f) / (0.8f + 1.2f);
        p[1][1] = 1.0f;
        p[2][1] = 0.0f;
        p[2][3] = -1.0f;
        // ndc 0.6 .. 1.0 in x is the tangent 0.4 .. 0.8; ndc -0.5 .. 0.5 in y is -0.5 .. 0.5
        const quadview::NdcRect r{0.6f, -0.5f, 1.0f, 0.5f};
        const auto deg = [](float half_tan) { return 2.0f * std::atan(half_tan) * 57.29577951f; };
        // vertical FOV, aspect 1: the horizontal tangent 0.8 decides
        CHECK(NEAR(quadview::cull_fov(p, r, 1.0f, true, 106.0f, 1.0f), deg(0.8f), 1e-3f));
        // aspect 2: 0.8 / 2 = 0.4 is less than the vertical 0.5
        CHECK(NEAR(quadview::cull_fov(p, r, 2.0f, true, 106.0f, 1.0f), deg(0.5f), 1e-3f));
        // horizontal FOV, aspect 2: max(0.8, 0.5 * 2)
        CHECK(NEAR(quadview::cull_fov(p, r, 2.0f, false, 106.0f, 1.0f), deg(1.0f), 1e-3f));
        // the margin enlarges the tangent, the eye's FOV is the top, 20 degrees the bottom
        CHECK(NEAR(quadview::cull_fov(p, r, 1.0f, true, 106.0f, 1.15f), deg(0.92f), 1e-3f));
        CHECK(quadview::cull_fov(p, quadview::NdcRect{-1.0f, -1.0f, 1.0f, 1.0f}, 1.0f, true, 90.0f, 1.15f) == 90.0f);
        // the axis is at ndc 0.2 (t = 0): a tiny rectangle around it gives the bottom
        CHECK(quadview::cull_fov(p, quadview::NdcRect{0.19f, -0.01f, 0.21f, 0.01f}, 1.0f, true, 90.0f, 1.0f) == 20.0f);
        // the other handedness (m23 = +1, ndc = m00 * t + m20): the same tangents give the same answer
        float q[4][4]{};
        q[0][0] = p[0][0];
        q[2][0] = -p[2][0];
        q[1][1] = 1.0f;
        q[2][3] = 1.0f;
        CHECK(NEAR(quadview::cull_fov(q, r, 1.0f, true, 106.0f, 1.0f), deg(0.8f), 1e-3f));
        // not a perspective projection
        float o[4][4]{};
        o[0][0] = 1.0f;
        o[1][1] = 1.0f;
        CHECK(quadview::cull_fov(o, r, 1.0f, true, 106.0f) == 106.0f);
    }

    // [SUPER TAA 16.11.2026] the supersampling TAA's own jitter sequence (Halton 2, 3 minus 0.5, as FSR's)
    {
        float x = 0.0f, y = 0.0f;
        // the first samples of the sequence: 1/2, 1/3; 1/4, 2/3; 3/4, 1/9
        quadview::halton_jitter(0, 32, x, y);
        CHECK(NEAR(x, 0.0f, 1e-6f) && NEAR(y, 1.0f / 3.0f - 0.5f, 1e-6f));
        quadview::halton_jitter(1, 32, x, y);
        CHECK(NEAR(x, -0.25f, 1e-6f) && NEAR(y, 2.0f / 3.0f - 0.5f, 1e-6f));
        quadview::halton_jitter(2, 32, x, y);
        CHECK(NEAR(x, 0.25f, 1e-6f) && NEAR(y, 1.0f / 9.0f - 0.5f, 1e-6f));

        // it repeats after `phases` frames and stays inside the pixel
        for (uint32_t phases : {8u, 32u}) {
            float sx = 0.0f, sy = 0.0f;

            for (uint32_t i = 0; i < phases; ++i) {
                float x2 = 0.0f, y2 = 0.0f;
                quadview::halton_jitter(i, phases, x, y);
                quadview::halton_jitter(i + phases, phases, x2, y2);
                CHECK(x == x2 && y == y2);
                CHECK(x > -0.5f && x < 0.5f && y > -0.5f && y < 0.5f);
                sx += x;
                sy += y;
            }

            // (its mean is near the pixel's centre: no shift of the image)
            CHECK(std::fabs(sx / (float)phases) < 0.07f && std::fabs(sy / (float)phases) < 0.07f);
        }

        // with 32 frames every one of the 2 x 2 history texels of a pixel (scale 2) gets 8 samples in x and in y halves
        int quad[2][2]{};

        for (uint32_t i = 0; i < 32; ++i) {
            quadview::halton_jitter(i, 32, x, y);
            ++quad[y < 0.0f ? 0 : 1][x < 0.0f ? 0 : 1];
        }

        CHECK(quad[0][0] >= 6 && quad[0][1] >= 6 && quad[1][0] >= 6 && quad[1][1] >= 6);
        // a sequence of 0 frames is one of 1 (no division by zero)
        quadview::halton_jitter(5, 0, x, y);
        CHECK(NEAR(x, 0.0f, 1e-6f));
        // the history's width decides the length: 8 frames at 1 x, 32 at 2 x (the fovea image as wide as the view)
        CHECK(jitter_phase_count(1360, 1360) == 8);
        CHECK(jitter_phase_count(1360, 2720) == 32);
        // the settings' defaults the menu's "Recommended values" give
        const quadview::SuperTaaSettings d{};
        CHECK(d.scale == 2 && d.follow && NEAR(d.follow_threshold, 1.0f, 1e-6f) && NEAR(d.follow_memory, 0.5f, 1e-6f) && d.protect && d.arrive && NEAR(d.protect_range, 1.5f, 1e-6f) && NEAR(d.protect_spark, 2.5f, 1e-6f) && NEAR(d.protect_memory, 1.5f, 1e-6f) && !d.lighter && !d.lanczos &&
              d.debug_view == 0 && !d.split && !d.compare);
        // [fix80] the periphery: the supersampling TAA with the image of twice the wide view's size unless chosen otherwise;
        // its history has 2 x 2 texels per pixel of the wide view either way (32 frames of jitter)
        const quadview::Settings qs{};
        CHECK(qs.periphery_aa == 2 && qs.mono_periphery && NEAR(qs.taa_feedback, 0.90f, 1e-6f));
        CHECK(jitter_phase_count(1120, 1120 * 2) == 32);
        // ... the image of twice the size only while the eye image is 3 times an eye's share of the wide view per side or
        // more: the eye pass's groups of 16 x 16 pixels hold at most 12 periphery pixels per 17 of the eye image
        // (quadview::periphery_2x_fits; otherwise the image of the wide view's size)
        {
            // the layout of the tests of 16.11.2026: fovea 1120 x 864, each eye's share of the wide view 848 x 864 (x4.01)
            const auto l = quadview::compute_layout(3400, 3468, 0.249f, 1.0f, 1.321f);
            CHECK(l.valid && l.render_w == 1120 && l.render_h == 864 && l.eye_wide_w == 848 && l.eye_wide_h == 864);
            CHECK(quadview::periphery_2x_fits(l));
            // the layout of 06.11.2026 (fovea 1240 x 960, x3.62)
            CHECK(quadview::periphery_2x_fits(quadview::compute_layout(3400, 3468, 0.276f, 1.0f, 1.321f)));
            // a fovea of 0.40 of the eye image: twice an eye's share is 0.80 of the eye image
            const auto big = quadview::compute_layout(3400, 3468, 0.40f, 1.0f, 1.321f);
            CHECK(big.valid && !quadview::periphery_2x_fits(big));
            // the limit itself (2 * share * 18 <= eye image * 12), per axis
            quadview::Layout t = l;
            t.eye_wide_w = 1133;
            CHECK(quadview::periphery_2x_fits(t));
            t.eye_wide_w = 1134;
            CHECK(!quadview::periphery_2x_fits(t));
            t.eye_wide_w = 848;
            t.eye_wide_h = 1156;
            CHECK(quadview::periphery_2x_fits(t));
            t.eye_wide_h = 1157;
            CHECK(!quadview::periphery_2x_fits(t));
            CHECK(!quadview::periphery_2x_fits(quadview::Layout{}));
        }
    }

    // [UI MOTION fix125 22.11.2026] the interface's own motion (quadview::ui_plane_motion): the map of a view's UV to the
    // UV of the frame before for the points of an element's plane, against the same points projected directly
    {
        using M4 = std::array<double, 16>; // column-major like glm
        const auto mul = [](const M4& a, const M4& b) {
            M4 c{};

            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    for (int k = 0; k < 4; ++k) {
                        c[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
                    }
                }
            }

            return c;
        };
        const auto tof = [](const M4& a) {
            std::array<float, 16> f{};

            for (int i = 0; i < 16; ++i) {
                f[i] = (float)a[i];
            }

            return f;
        };
        // rotation about y (yaw), then x (pitch), with a translation and a uniform scale: a rigid placement
        const auto place = [&](double yaw, double pitch, double x, double y, double z, double scale) {
            const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            const M4 ry{cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
            const M4 rx{1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1};
            M4 m = mul(ry, rx);

            for (int i = 0; i < 12; ++i) {
                m[i] *= scale;
            }

            m[12] = x;
            m[13] = y;
            m[14] = z;
            return m;
        };
        // a view matrix: the inverse of a rigid camera placement (scale 1)
        const auto view_of = [&](double yaw, double pitch, double x, double y, double z) {
            const M4 c = place(yaw, pitch, x, y, z, 1.0);
            M4 v{};

            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    v[j * 4 + i] = c[i * 4 + j];
                }
            }

            for (int i = 0; i < 3; ++i) {
                v[12 + i] = -(v[0 * 4 + i] * x + v[1 * 4 + i] * y + v[2 * 4 + i] * z);
            }

            v[15] = 1.0;
            return v;
        };
        // an off-centre projection looking down -z (reverse z, the far plane at infinity), as the views have them
        const auto proj = [](double l, double r, double b, double t, double n) {
            M4 p{};
            p[0] = 2.0 / (r - l);
            p[5] = 2.0 / (t - b);
            p[8] = (r + l) / (r - l);
            p[9] = (t + b) / (t - b);
            p[11] = -1.0;
            p[14] = n;
            return p;
        };
        const auto uv_of = [&](const M4& vp, const M4& m, double a, double b, double& u, double& v) {
            const M4 vm = mul(vp, m);
            const double x = vm[0] * a + vm[4] * b + vm[12];
            const double y = vm[1] * a + vm[5] * b + vm[13];
            const double w = vm[3] * a + vm[7] * b + vm[15];
            u = 0.5 + 0.5 * x / w;
            v = 0.5 - 0.5 * y / w;
            return w > 0.0;
        };

        struct Case {
            M4 vp_cur, vp_old, m_cur, m_old;
        };

        const M4 p_fovea = proj(-0.21, 0.33, -0.29, 0.25, 0.05); // a fovea's part of an eye's frustum
        const M4 p_fovea_old = proj(-0.24, 0.30, -0.27, 0.27, 0.05); // ... the frame before (the gaze moved)
        const M4 p_wide = proj(-1.35, 1.35, -1.2, 1.1, 0.05);
        std::vector<Case> cases{};
        // a panel 1 m in front of a still game camera, the head moving sideways and turning (the item screen's text)
        cases.push_back({mul(p_fovea, view_of(0.020, -0.010, 0.012, 1.6, 0.0)), mul(p_fovea_old, view_of(0.017, -0.012, 0.010, 1.6, 0.001)),
            place(3.14159, 0.0, 0.0, 1.6, -1.0, 0.001), place(3.14159, 0.0, 0.0, 1.6, -1.0, 0.001)});
        // a panel that goes with the game camera while the player walks and turns (the head with it)
        cases.push_back({mul(p_wide, view_of(0.30, 0.0, 2.0, 1.6, 5.0)), mul(p_wide, view_of(0.28, 0.0, 2.02, 1.6, 5.03)),
            place(3.14159 + 0.30, 0.0, 2.0 - std::sin(0.30), 1.6, 5.0 - std::cos(0.30), 0.001),
            place(3.14159 + 0.28, 0.0, 2.02 - std::sin(0.28), 1.6, 5.03 - std::cos(0.28), 0.001)});
        // the HUD at the weapon: moved and turned by the hand, the head nearly still
        cases.push_back({mul(p_fovea, view_of(0.0, 0.0, 0.0, 1.6, 0.0)), mul(p_fovea, view_of(0.001, 0.0, 0.0, 1.6, 0.0)),
            place(2.9, 0.3, 0.15, 1.35, -0.50, 0.0005), place(2.86, 0.27, 0.14, 1.345, -0.51, 0.0005)});

        for (size_t ci = 0; ci < cases.size(); ++ci) {
            const auto& c = cases[ci];
            float d[9]{};
            CHECK(quadview::ui_plane_motion(tof(c.vp_cur).data(), tof(c.vp_old).data(), tof(c.m_cur).data(), tof(c.m_old).data(), d));
            double worst = 0.0;
            double largest = 0.0;
            int seen = 0;

            for (int iy = -4; iy <= 4; ++iy) {
                for (int ix = -4; ix <= 4; ++ix) {
                    // points of the element's plane in its own units (the scale makes them a few decimetres apart)
                    const double a = ix * 90.0;
                    const double b = iy * 60.0;
                    double u0, v0, u1, v1;

                    if (!uv_of(c.vp_cur, c.m_cur, a, b, u0, v0) || !uv_of(c.vp_old, c.m_old, a, b, u1, v1) || u0 < 0.0 || u0 > 1.0 || v0 < 0.0 || v0 > 1.0) {
                        continue;
                    }

                    float du = 0.0f;
                    float dv = 0.0f;
                    quadview::ui_motion_at(d, (float)u0, (float)v0, du, dv);
                    worst = (std::max)(worst, (std::max)(std::abs(du - (u1 - u0)), std::abs(dv - (v1 - v0))));
                    largest = (std::max)(largest, (std::max)(std::abs(u1 - u0), std::abs(v1 - v0)));
                    ++seen;
                }
            }

            // within 0.01 pixel of a view 1240 pixels wide, with float matrices and float arithmetic
            CHECK(seen >= 4);
            CHECK(worst < 0.01 / 1240.0);
            // the first and the third case move on the screen; the panel that goes with the game camera stands still on
            // it while the scene's points move (a point 4 m behind it: more than 10 pixels of 1240 in that frame)
            if (ci == 1) {
                CHECK(largest < 0.05 / 1240.0);
            } else {
                CHECK(largest > 1e-4);
            }
        }

        // nothing moves: no motion at all
        {
            const auto vp = tof(mul(p_fovea, view_of(0.02, -0.01, 0.01, 1.6, 0.0)));
            const auto m = tof(place(3.14159, 0.0, 0.0, 1.6, -1.0, 0.001));
            float d[9]{1, 1, 1, 1, 1, 1, 1, 1, 1};
            CHECK(quadview::ui_plane_motion(vp.data(), vp.data(), m.data(), m.data(), d));

            for (float x : d) {
                CHECK(std::abs(x) < 1e-6f);
            }
        }

        // a static element against a static point of the scene at the element's depth: the same motion (what the engine's
        // motion vectors hold for geometry there: RE7's menus, within 0.05 pixels)
        {
            const M4 vc = mul(p_fovea, view_of(0.020, -0.010, 0.012, 1.6, 0.0));
            const M4 vo = mul(p_fovea, view_of(0.017, -0.012, 0.010, 1.6, 0.001));
            const M4 m = place(3.14159, 0.0, 0.0, 1.6, -1.0, 0.001);
            float d[9]{};
            CHECK(quadview::ui_plane_motion(tof(vc).data(), tof(vo).data(), tof(m).data(), tof(m).data(), d));
            // the world point of the plane's local (50, -30)
            const double wx = m[0] * 50.0 + m[4] * -30.0 + m[12];
            const double wy = m[1] * 50.0 + m[5] * -30.0 + m[13];
            const double wz = m[2] * 50.0 + m[6] * -30.0 + m[14];
            const auto project = [&](const M4& vp, double& u, double& v) {
                const double x = vp[0] * wx + vp[4] * wy + vp[8] * wz + vp[12];
                const double y = vp[1] * wx + vp[5] * wy + vp[9] * wz + vp[13];
                const double w = vp[3] * wx + vp[7] * wy + vp[11] * wz + vp[15];
                u = 0.5 + 0.5 * x / w;
                v = 0.5 - 0.5 * y / w;
            };
            double u0, v0, u1, v1;
            project(vc, u0, v0);
            project(vo, u1, v1);
            float du = 0.0f;
            float dv = 0.0f;
            quadview::ui_motion_at(d, (float)u0, (float)v0, du, dv);
            CHECK(std::abs(du - (u1 - u0)) < 1e-5 && std::abs(dv - (v1 - v0)) < 1e-5);
        }

        // a plane through the camera (seen edge-on), or matrices of zeros (no frame before): no motion is given
        {
            const auto vp = tof(mul(p_fovea, view_of(0.0, 0.0, 0.0, 0.0, 0.0)));
            const auto edge = tof(place(1.5708, 0.0, 0.0, 0.0, 0.0, 0.001));
            const std::array<float, 16> zero{};
            float d[9]{};
            CHECK(!quadview::ui_plane_motion(vp.data(), vp.data(), edge.data(), edge.data(), d));
            CHECK(!quadview::ui_plane_motion(zero.data(), vp.data(), edge.data(), edge.data(), d));
            const auto ok = tof(place(3.14159, 0.0, 0.0, 0.0, -1.0, 0.001));
            CHECK(!quadview::ui_plane_motion(vp.data(), zero.data(), ok.data(), ok.data(), d));
        }

        // the lists of a frame's elements: kept by frame, replaced when the slot is taken again
        {
            auto& st = quadview::State::get();
            quadview::GuiAnchor a{};
            a.id = 7;
            a.has_mat = true;
            st.add_gui_element(1000, a);
            a.id = 8;
            st.add_gui_element(1000, a);
            a.id = 9;
            st.add_gui_element(1001, a);
            std::vector<quadview::GuiAnchor> got{};
            CHECK(st.gui_elements(1000, got) && got.size() == 2 && got[0].id == 7 && got[1].id == 8);
            CHECK(st.gui_elements(1001, got) && got.size() == 1 && got[0].id == 9);
            CHECK(!st.gui_elements(999, got) && got.empty());
            st.add_gui_element(1008, a); // the slot of frame 1000
            CHECK(!st.gui_elements(1000, got));
            CHECK(st.gui_elements(1008, got) && got.size() == 1);
        }
    }

    std::printf(g_fail == 0 ? "ALL TESTS OK\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
