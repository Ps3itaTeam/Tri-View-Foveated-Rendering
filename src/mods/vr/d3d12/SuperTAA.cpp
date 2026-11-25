// SuperTAA.cpp -- [SUPER TAA 16.11.2026] see SuperTAA.hpp
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "SuperTAA.hpp"
#include "ShaderCompile.hpp" // [BUILT-IN SHADERS fix145] compile_shader
#include "UiMotion.hpp" // [UI MOTION fix125]

#pragma comment(lib, "d3dcompiler")

namespace d3d12 {

namespace {
// ---- shared by the three sources ---------------------------------------------------------------------------------
#define SUPERTAA_COMMON                                                                                                  \
    "float3 to_ycocg(float3 c) {\n"                                                                                      \
    "    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);\n" \
    "}\n"                                                                                                                \
    "float3 from_ycocg(float3 c) {\n"                                                                                    \
    "    const float t = c.x - c.z;\n"                                                                                   \
    "    return float3(t + c.y, c.x + c.z, t - c.y);\n"                                                                  \
    "}\n"                                                                                                                \
    "float3 to_linear(float3 c) {\n"                                                                                     \
    "    c = saturate(c);\n"                                                                                             \
    "    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);\n"                                             \
    "}\n"                                                                                                                \
    "float3 to_srgb(float3 c) {\n"                                                                                       \
    "    c = saturate(c);\n"                                                                                             \
    "    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"                                       \
    "}\n"                                                                                                                \
    "// the perceptual encoding clip and statistics work in\n"                                                           \
    "float3 enc(float3 c) {\n"                                                                                           \
    "    return sqrt(max(c, 0.0));\n"                                                                                    \
    "}\n"                                                                                                                \
    "float3 clip_to_box(float3 lo, float3 hi, float3 h) {\n"                                                             \
    "    const float3 centre = 0.5 * (hi + lo);\n"                                                                       \
    "    const float3 extent = 0.5 * (hi - lo) + 1e-4;\n"                                                                \
    "    const float3 v = h - centre;\n"                                                                                 \
    "    const float3 a = abs(v / extent);\n"                                                                            \
    "    const float m = max(a.x, max(a.y, a.z));\n"                                                                     \
    "    return m > 1.0 ? centre + v / m : h;\n"                                                                         \
    "}\n"

// ---- analyze: one thread per pixel of the view -----------------------------------------------------------------------
constexpr char ANALYZE_SHADER[] =
    R"(
Texture2D<float4>   g_color  : register(t0);
Texture2D<float4>   g_motion : register(t1);
Texture2D<float4>   g_depth  : register(t2);
Texture2D<float4>   g_pstats : register(t3); // last frame: noise m, fast mean b, mean and variance of e
Texture2D<float4>   g_pdist  : register(t4); // last frame: x distance (m), y the protection's release (above 1: a thing arrived), zw fetch motion minus own motion
Texture2D<float4>   g_phist  : register(t5); // last frame: the history (rgb linear, a = accumulated weight)
RWTexture2D<float4> g_lin    : register(u0); // rgb linear colour, a = perceptual luma of the sample
RWTexture2D<float4> g_guide  : register(u1); // xy motion of the nearest surface (pixels), z the weight the history may keep (0 = no history), w protection
RWTexture2D<float4> g_boxc   : register(u2); // clip box centre (YCoCg of the perceptual encoding), a = mean luma of the 3x3
RWTexture2D<float4> g_boxe   : register(u3); // clip box extent
RWTexture2D<float4> g_stats  : register(u4); // noise m, fast mean b, mean and variance of e (the sample against its 3x3 mean)
RWTexture2D<float4> g_dist   : register(u5); // distance (m), release, the motion that fetches the history minus the pixel's own
SamplerState        g_samp   : register(s0);

cbuffer Params : register(b0) {
    uint2  g_size;
    float2 g_mv_scale;   // current UV + mv * scale = previous UV
    float4 g_depth_k;    // distance = |(k0 z + k1) / (k2 z + k3)|
    uint   g_flags;      // 1 = reset, 2 = reverse Z, 4 = depth (dilation), 8 = decode SRGB, 16 = follow changes,
                         // 32 = disocclusion test with the distances, 64 = protection, 128 = things that arrive
    float  g_reject;     // hidden in the last frame: nearer by more than this fraction
    float  g_gamma;      // clip box: mean +- gamma * sigma
    float  g_w_max;      // the weight a settled history holds
    float2 g_jitter;     // pixels: the sample of pixel q shows the point at q + 0.5 - jitter
    float  g_k;          // a change at once: samples farther from the history than k * noise + b0 ...
    float  g_b0;
    float  g_kc;         // a change confirmed: d over kc * noise + b0c two frames the same way ...
    float  g_b0c;
    float  g_cap;        // the noise believed at most
    float  g_rate;       // rate of the noise
    float  g_rate_b;     // rate of the fast mean
    float  g_w_short;    // the weight the history keeps where the picture changed
    uint   g_count;      // ... confirmed: with at least this many of the 9 samples
    uint   g_alln;       // ... at once: at least this many of the 9 samples, the same way
    float  g_fl_sde;     // both bands at least this times the scatter of the samples (deviation of e)
    float  g_af_rate;    // rate of the mean and the variance of e
    float  g_af_t0;      // deviation of e from which ...
    float  g_af_t1;      // ... and at which the protection is full
    float  g_af_mem;     // the history's weight limit times this at full protection
    float  g_edge0;      // change of (the motion that fetches the history - the pixel's own motion), pixels, from which ...
    float  g_edge1;      // ... and at which the protection is released
    float  g_rel_decay;  // what is left of a release a frame later
    float  g_af_clip;    // [fix81] a sample counts for the scatter no farther from the mean of e than this times the
    float  g_af_floor;   //         deviation of e so far + this (0 = as far as it is: fix80)
};

#ifdef UI_MOTION
#ifndef UM_OWN_WEIGHT
#define UM_OWN_WEIGHT 3.0
#endif
// ---- [UI MOTION fix125 22.11.2026] the interface's own motion (compiled with UI_MOTION, after UiMotion.hpp's text) ------
// The view's image holds the interface the engine drew into it, and the motion vectors hold the motion of the scene
// behind it. Where the interface has detail of its own the history is fetched along the interface's motion instead.
Texture2D<float4> g_ui_hud : register(t6); // the view's copy without GUI ...
Texture2D<float4> g_ui_fin : register(t7); // ... and with GUI: the same image except where the GUI drew

// Pixel p: where the GUI drew in its 3x3 (the copies differ; told by four bilinear reads of each at the pixel's corners,
// which are the same numbers where all nine texels are: the pixels next to a stroke belong to it like its own, as the
// rim of a nearer thing takes the thing's motion), the brightness with GUI of the 3x3 is fitted by the brightness without
// (fin = a hud + b: what a veil over the scene, a fade, a vignette or the flat inside of a panel make of the picture; the
// detail there is the scene's, and it keeps the scene's motion). What the fit leaves is detail of the interface itself
// (writing, icons, lines, a panel's edge): from 5 % of the brightness there on the pixel takes the interface's motion,
// as its own and as the one its history is fetched with.
bool um_pixel(int2 p, int2 last, float2 sz, inout float2 mv_d, inout float2 mv_own, out bool own) {
    own = false;
    const float2 c0 = float2(p) / sz;
    const float2 c1 = float2(p + 1) / sz;
    const bool drawn = any(g_ui_fin.SampleLevel(g_samp, c0, 0).rgb != g_ui_hud.SampleLevel(g_samp, c0, 0).rgb) ||
                       any(g_ui_fin.SampleLevel(g_samp, float2(c1.x, c0.y), 0).rgb != g_ui_hud.SampleLevel(g_samp, float2(c1.x, c0.y), 0).rgb) ||
                       any(g_ui_fin.SampleLevel(g_samp, float2(c0.x, c1.y), 0).rgb != g_ui_hud.SampleLevel(g_samp, float2(c0.x, c1.y), 0).rgb) ||
                       any(g_ui_fin.SampleLevel(g_samp, c1, 0).rgb != g_ui_hud.SampleLevel(g_samp, c1, 0).rgb);

    [branch] if (drawn) {
        float sf = 0.0;
        float sh = 0.0;
        float sff = 0.0;
        float shh = 0.0;
        float sfh = 0.0;

        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                const int3 q = int3(clamp(p + int2(x, y), int2(0, 0), last), 0);
                const float a = dot(min(max(g_ui_fin.Load(q).rgb, 0.0), 64.0), float3(0.299, 0.587, 0.114));
                const float b = dot(min(max(g_ui_hud.Load(q).rgb, 0.0), 64.0), float3(0.299, 0.587, 0.114));
                sf += a;
                sh += b;
                sff += a * a;
                shh += b * b;
                sfh += a * b;
            }
        }

        const float mf = sf / 9.0;
        const float mh = sh / 9.0;
        const float vf = max(sff / 9.0 - mf * mf, 0.0);
        const float vh = max(shh / 9.0 - mh * mh, 0.0);
        const float cv = sfh / 9.0 - mf * mh;
        const float left = vf - (vh > 1e-12 ? min(cv * cv / vh, vf) : 0.0);
        const float lim = 0.025 * (mf + mh) + 0.002;

        [branch] if (left > lim * lim) {
            float2 m;

            [branch] if (um_motion((float2(p) + 0.5) / sz, m)) {
                mv_d = m * sz;
                mv_own = mv_d;
                own = true; // the pixel goes with the interface
            }
        }
    }

    return drawn; // the GUI drew in the pixel's 3x3
}
#endif
)" SUPERTAA_COMMON R"(
// metres, limited to what the 16-bit float distance image holds
float distance_of(float z) {
    const float den = g_depth_k.z * z + g_depth_k.w;
    return min(abs(den) > 1e-12 ? abs((g_depth_k.x * z + g_depth_k.y) / den) : 60000.0, 60000.0);
}

[numthreads(8, 8, 1)]
void analyze(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_size)) {
        return;
    }

    const int2 p = int2(id.xy);
    const int2 last = int2(g_size) - 1;
    const bool reverse_z = (g_flags & 2) != 0;
    const bool have_depth = (g_flags & 4) != 0;
    const bool decode = (g_flags & 8) != 0;
    const bool dist_test = (g_flags & 32) != 0;

    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 own_lin = 0.0;
    float3 own_y = 0.0;
    float lum[9]; // perceptual luma of the 3x3 samples
    int2 nearest = p;
    float near_z = reverse_z ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            const float3 c = g_color.Load(int3(q, 0)).rgb;
            const float3 lin = decode ? to_linear(c) : max(c, 0.0);
            const float3 yc = to_ycocg(enc(lin));
            m1 += yc;
            m2 += yc * yc;
            lum[(y + 1) * 3 + (x + 1)] = yc.x;

            if (x == 0 && y == 0) {
                own_lin = lin;
                own_y = yc;
            }

            if (have_depth) {
                const float z = g_depth.Load(int3(q, 0)).r;

                if (reverse_z ? z > near_z : z < near_z) {
                    near_z = z;
                    nearest = q;
                }
            }
        }
    }

    const float3 mean = m1 / 9.0;
    const float3 sig = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float2 sz = float2(g_size);

    // motion in pixels of the nearest surface of the 3x3 (an edge takes its history where the thing in front was)
    const float2 to_px = g_mv_scale * sz;
#ifdef UI_MOTION
    float2 mv_d = g_motion.Load(int3(nearest, 0)).xy * to_px;
    float2 mv_own = g_motion.Load(int3(p, 0)).xy * to_px;
    bool um_own;
    const bool um_drawn = um_pixel(p, last, sz, mv_d, mv_own, um_own); // [UI MOTION fix125]
#else
    const float2 mv_d = g_motion.Load(int3(nearest, 0)).xy * to_px;
#endif

    const float2 prev_px = float2(p) + 0.5 + mv_d;
    const float2 prev_uv = prev_px / sz;

    // The motion that fetches the history against the pixel's own: 0 on a surface, the difference of the two motions in
    // the rim of a nearer thing that moves against what is behind it. Where that changes from one frame to the next the
    // point has left such a rim (or entered one): what the history holds there was fetched along another motion, and it
    // is not to be kept (the thing itself and its rim keep theirs: they are fetched the same way every frame).
    const float4 pstate = g_pdist.SampleLevel(g_samp, prev_uv, 0);
#ifdef UI_MOTION
    const float2 offs = mv_d - mv_own;
#else
    const float2 offs = mv_d - g_motion.Load(int3(p, 0)).xy * to_px;
#endif
    const float2 doff = abs(offs - pstate.zw);
    const float medge = saturate((max(doff.x, doff.y) - g_edge0) / max(g_edge1 - g_edge0, 1e-4));
    bool valid = (g_flags & 1) == 0 && all(prev_px >= 0.0) && all(prev_px <= sz);

    // the own surface's distance is kept for the next frame; the test uses the surface whose motion fetches the history
    const float dist = dist_test ? distance_of(g_depth.Load(int3(p, 0)).r) : 0.0;
    const float dist_moved = dist_test ? distance_of(near_z) : 0.0;

    // hidden in the last frame: all four distances around the point it came from are nearer than this surface
    bool arrive = false;

    [branch] if (valid && dist_test) {
        const float4 pd = g_pdist.GatherRed(g_samp, prev_uv);
        const float prev_far = max(max(pd.x, pd.y), max(pd.z, pd.w));
        valid = prev_far >= dist_moved * (1.0 - g_reject);

        // ---- a thing that was not there a frame ago (a fly: it moves, its motion vectors do not; the history under it
        //      belongs to what is behind it, and what it does to the samples is not scatter). The nearest surface of the
        //      3x3 is nearer than anything the last frame held within 3 pixels of the place the history comes from (the
        //      6x6 block of its distances: an edge or a wire that the jitter moves by a pixel is still inside it), and it
        //      is a thing, not a line: some 3x3 window whose centre is at most a pixel away holds 4 samples that near (a
        //      line a pixel wide, or thinner and hit here and there, never has more than 3). Rare: the first test costs
        //      nothing, the rest runs for a handful of pixels. ------------------------------------------------------------
        float near_prev = min(min(pd.x, pd.y), min(pd.z, pd.w));

        [branch] if (valid && (g_flags & 128) != 0 && dist_moved < near_prev * (1.0 - g_reject)) {
            [unroll] for (int by = -2; by <= 2; by += 2) {
                [unroll] for (int bx = -2; bx <= 2; bx += 2) {
                    if (bx != 0 || by != 0) {
                        const float4 bq = g_pdist.GatherRed(g_samp, (prev_px + float2(bx, by)) / sz);
                        near_prev = min(near_prev, min(min(bq.x, bq.y), min(bq.z, bq.w)));
                    }
                }
            }

            const float thr = near_prev * (1.0 - g_reject);

            [branch] if (dist_moved < thr) {
                float that_near[25];

                [unroll] for (int wy = -2; wy <= 2; ++wy) {
                    [unroll] for (int wx = -2; wx <= 2; ++wx) {
                        const int2 q = clamp(p + int2(wx, wy), int2(0, 0), last);
                        that_near[(wy + 2) * 5 + (wx + 2)] = distance_of(g_depth.Load(int3(q, 0)).r) < thr ? 1.0 : 0.0;
                    }
                }

                float most = 0.0;

                [unroll] for (int cy = 0; cy <= 2; ++cy) {
                    [unroll] for (int cx = 0; cx <= 2; ++cx) {
                        float c = 0.0;

                        [unroll] for (int dy = 0; dy <= 2; ++dy) {
                            [unroll] for (int dx = 0; dx <= 2; ++dx) {
                                c += that_near[(cy + dy) * 5 + (cx + dx)];
                            }
                        }

                        most = max(most, c);
                    }
                }

                arrive = most >= 4.0;
            }
        }
    }

)"
// (the shader goes on in the next literal: MSVC takes at most 16 KB in one string literal)
R"(    // ---- what one frame's samples cannot show: e = the pixel's sample against the mean of its 3x3, over time (mean and
    //      variance, carried along the motion). Its deviation is what things thinner than a pixel, edges under the
    //      jitter and stochastic shadows make of the samples from frame to frame. See SuperTAA.hpp. ---------------------
    const float4 st = g_pstats.SampleLevel(g_samp, prev_uv, 0);
    const float e = own_y.x - mean.x;
    const float mu_e = valid ? st.z : e;
    const float var_e = (valid ? st.w : 0.0) * (1.0 - medge);
    const float sde = sqrt(max(var_e, 0.0));

    // ---- following changes: does the history still show, on the whole, what this frame shows around the pixel? -------
    // The 3x3 samples of the frame against the history at the places they show (a frame before, with this pixel's
    // motion), both without their largest and smallest value: d. Its fast mean b: a history that lags. The mean of
    // |d - b|: noise (m). The bands are never narrower than the samples' own scatter.
    float change = 0.0;
    float m_new = 0.0;
    float b_new = 0.0;
#ifdef UI_MOTION
    bool um_drop = false; // [fix127] the interface changed here: no history
#endif

    [branch] if (valid && (g_flags & 16) != 0) {
        float hl[9];
        float hw = 0.0;

        [unroll] for (int ty = -1; ty <= 1; ++ty) {
            [unroll] for (int tx = -1; tx <= 1; ++tx) {
                const float4 hq = g_phist.SampleLevel(g_samp, (float2(p + int2(tx, ty)) + 0.5 - g_jitter + mv_d) / sz, 0);
                hl[(ty + 1) * 3 + (tx + 1)] = to_ycocg(enc(hq.rgb)).x;

                if (tx == 0 && ty == 0) {
                    hw = hq.a;
                }
            }
        }

        [branch] if (hw > 1e-3) {
            float cs = 0.0;
            float cmax = -1e9;
            float cmin = 1e9;
            float hs = 0.0;
            float hmax = -1e9;
            float hmin = 1e9;

            [unroll] for (int i = 0; i < 9; ++i) {
                cs += lum[i];
                cmax = max(cmax, lum[i]);
                cmin = min(cmin, lum[i]);
                hs += hl[i];
                hmax = max(hmax, hl[i]);
                hmin = min(hmin, hl[i]);
            }

            const float d = (cs - cmax - cmin) / 7.0 - (hs - hmax - hmin) / 7.0;
            const float m_prev = st.x;
            const float b_prev = st.y;
            const float noise = min(m_prev, g_cap);
            const float band_i = max(g_k * noise + g_b0, g_fl_sde * sde);
            const float band_c = max(g_kc * noise + g_b0c, g_fl_sde * sde);
            const float sgn = d >= 0.0 ? 1.0 : -1.0;
            float n_up = 0.0;
            float n_dn = 0.0;
            float cnt = 0.0;

            [unroll] for (int j = 0; j < 9; ++j) {
                const float dq = lum[j] - hl[j];
                n_up += dq > band_i ? 1.0 : 0.0;
                n_dn += dq < -band_i ? 1.0 : 0.0;
                cnt += dq * sgn > band_c ? 1.0 : 0.0;
            }

            // at once: (nearly) all of the samples moved the same way: a light, a shadow, a surface
            const float at_once = saturate(max(n_up, n_dn) - (float(g_alln) - 1.0));
            // confirmed: the frame before differed the same way, and most of the samples go with it
            const float a = d * b_prev > 0.0 ? min(abs(d), abs(b_prev) / g_rate_b) : 0.0;
            float t = saturate(a / band_c - 1.0);
            t = t * t * (3.0 - 2.0 * t);
            change = max(t * saturate(cnt - (float(g_count) - 1.0)), at_once);
#ifdef UI_MOTION
            // [UI MOTION fix126 22.11.2026] What moves inside the interface. RE4 dumps of 22.11.2026 (fix125; the lists
            // "Missione / Rozzo amuleto / Distintivo dell'agente" and "???"): when the selection goes to another row the
            // rows slide and the list scrolls inside its panel, 5 pixels a frame and more. No motion says so (the panel
            // itself stands still), the writing is too thin for the tests above (they want most of the 3x3 to change the
            // same way), and where the old writing lies among the new one the frame's box holds both: the writing stayed
            // double for many frames. Where the GUI drew, a sample of the frame is compared with the history at its own
            // place: under the jitter an edge that stands still is at most half its height away from its history (a
            // step against the same step smoothed); a stroke where the history holds none, or none where it holds one,
            // is its whole height away. Two of the nine samples farther than 0.7 of the range of both: the interface
            // changed here, and the history keeps the little weight of a change.
            [branch] if (um_drawn) {
                const float rng = max(cmax, hmax) - min(cmin, hmin);
                float n_far = 0.0;

                [unroll] for (int u = 0; u < 9; ++u) {
                    n_far += abs(lum[u] - hl[u]) > 0.7 * rng ? 1.0 : 0.0;
                }

                // [fix127 22.11.2026] RE4 dump of 22.11.2026 (fix126): a trace of the old writing was left for a frame or two
                // (the history kept the weight of a change, a third of the picture). From three samples on (a stroke that
                // crosses the 3x3 where the history holds none) the history is not used here at all; two keep the weight
                // of a change as before (dropping it there too took some calm from writing that stands still in the test
                // cases: 95th percentile of the error 0.10 -> 0.13).
                change = rng > 0.08 && n_far >= 2.0 ? 1.0 : change;
                um_drop = rng > 0.08 && n_far >= 3.0;
            }
#endif
            m_new = m_prev + g_rate * (min(abs(d - b_prev), 2.0 * g_cap) - m_prev);
            b_new = b_prev + g_rate_b * (d - b_prev);
        }
    }

    // ---- the release of the protection (1 = none): a change seen now, a rim left or entered (0.9), what is left of
    //      earlier releases, and, at 0.8, a change seen a frame ago within about a pixel and a half (only that spreads:
    //      the other kinds stay at or below 0.9). A pixel without history starts released.
    //      Where a thing arrived the release is set above 1 (2), and a frame later what was within a pixel and a half of
    //      it gets 1.6 (that level is handed on once); both fade like any release. Above 1 the statistics stand still:
    //      what such a thing does to the samples, in its pixels and around them, is not scatter. ------------------------
    float rel = 0.9;

    [branch] if (valid) {
        const float rel_prev = pstate.y;
        float around = 0.0;

        [unroll] for (int oy = -1; oy <= 1; oy += 2) {
            [unroll] for (int ox = -1; ox <= 1; ox += 2) {
                const float4 gq = g_pdist.GatherGreen(g_samp, (prev_px + float2(ox, oy)) / sz);
                around = max(around, max(max(gq.x, gq.y), max(gq.z, gq.w)));
            }
        }

        const float spread = 0.8 * saturate((around - 0.9) / 0.1);
        rel = max(max(change, 0.9 * medge), max(rel_prev * g_rel_decay, spread));

        if ((g_flags & 128) != 0) {
            rel = max(rel, around > 1.9 ? 1.6 : 0.0);
            rel = arrive ? 2.0 : rel;
        }

        rel = clamp(rel, 0.0, 2.0);
    }

    const bool still = rel > 1.0;
    float pr = saturate((sde - g_af_t0) / (g_af_t1 - g_af_t0));
    pr = pr * pr * (3.0 - 2.0 * pr);
    const float protect = (g_flags & 64) != 0 ? pr * (1.0 - min(rel, 1.0)) : 0.0;
    // a change is not scatter, and the mean follows it faster
    const float de = e - mu_e;
    // [fix81 17.11.2026] RE7 captures of 17.11.2026 (flies of one or two pixels crossing a wall): one frame's sample far from
    // everything the pixel showed before made a deviation of a third of its distance at once (full protection for 10 to
    // 40 frames): what the fly left in the history was kept under the widened clip box, a dotted trail. For the scatter a
    // sample now counts no farther from the mean than g_af_clip deviations (as they were before this frame) + g_af_floor:
    // one such frame leaves the deviation below the protection's start; a pattern that comes back (a thin thing the jitter
    // hits every few frames) still builds the same deviation within three or four of its frames, and holds it. The part of
    // the mean that follows a change takes the whole distance as before.
    const float lim = g_af_clip * sde + g_af_floor;
    const float ds = g_af_clip > 0.0 ? clamp(de, -lim, lim) : de;
    const float mu_new = still ? mu_e : mu_e + g_af_rate * ds + (0.5 - g_af_rate) * change * de;
    const float var_new = still ? var_e : (1.0 - g_af_rate) * (var_e + g_af_rate * (1.0 - change) * ds * ds);
    // the weight the history may keep: more where it is protected, next to none where the picture changed
    float wlim = g_w_max * (1.0 + (g_af_mem - 1.0) * protect);
    wlim = wlim + (min(wlim, g_w_short) - wlim) * change;

    g_stats[id.xy] = float4(m_new, b_new, mu_new, var_new);
    g_dist[id.xy] = float4(dist, rel, offs);
    g_lin[id.xy] = float4(own_lin, own_y.x);
#ifdef UI_MOTION
    // [fix128 22.11.2026] RE4 dump of 22.11.2026 (fix127): the row that has just lost the selection slides back a pixel or
    // two a frame, too little for the rule above (the old stroke lies under the new one's edge), and its writing kept a
    // faint second outline while it did. The interface's writing is drawn sharp by the engine and followed along its own
    // motion: it does not need the long memory that the scene's thin things need. Its history holds two frames' worth at
    // most (the scene's: g_w_max, five and more), so what is left of a small slide is gone in two or three frames.
    wlim = um_own ? min(wlim, UM_OWN_WEIGHT) : wlim;
    g_guide[id.xy] = float4(mv_d, valid && !um_drop ? wlim : 0.0, protect);
#else
    g_guide[id.xy] = float4(mv_d, valid ? wlim : 0.0, protect);
#endif
    g_boxc[id.xy] = float4(mean, mean.x);
    g_boxe[id.xy] = float4(g_gamma * sig, 0.0);
}
)";

// ---- accumulate: one thread per history texel ------------------------------------------------------------------------
constexpr char ACCUMULATE_SHADER[] =
    R"(
Texture2D<float4>   g_lin   : register(t0);
Texture2D<float4>   g_guide : register(t1);
Texture2D<float4>   g_boxc  : register(t2);
Texture2D<float4>   g_boxe  : register(t3);
Texture2D<float4>   g_hist  : register(t4); // rgb linear, a = accumulated weight
Texture2D<float4>   g_stats : register(t5); // this frame's statistics: w = variance of e (the samples' scatter)
RWTexture2D<float4> g_out   : register(u0);
SamplerState        g_samp  : register(s0);

cbuffer Params : register(b0) {
    uint2  g_in_size;       // the view
    uint2  g_hist_size;     // the history
    float2 g_jitter;        // pixels: the sample of pixel q shows the point at q + 0.5 - jitter
    float  g_k;             // rebuild of the current frame: 0.5 / sigma^2 (sigma in view pixels)
    float  g_w_cap;         // the most weight the history ever holds
    float  g_motion_reduce; // weight lowered by up to this fraction at 24 px of motion per frame
    float  g_extra;         // history: Catmull-Rom plus this much of its difference to the bilinear tap
    uint   g_flags;         // 1 = reset, 2 = luma weighted blend, 4 = dering (all channels), 8 = dering (green), 16 = nearest sample
    float  g_relax;         // protected: the clip box wider by this times the samples' scatter
    float  g_wins;          // protected: the frame's sample no farther from the history than this times the scatter (0 = off)
    float  g_wins_floor;    // ... and at least this
    uint2  g_pad;
};
)" SUPERTAA_COMMON R"(
// the history at `pos` (history texels): 5-tap Catmull-Rom built from bilinear taps, plus g_extra of its difference to
// the plain bilinear tap, kept inside the range of the four texels around the position
float3 history_at(float2 pos, float3 plain) {
    const float2 size = float2(g_hist_size);
    const float2 c = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 o12 = w2 / w12;

    const float2 t0 = (c - 1.0) / size;
    const float2 t3 = (c + 2.0) / size;
    const float2 t12 = (c + o12) / size;

    float3 r = 0.0;
    r += g_hist.SampleLevel(g_samp, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    r += g_hist.SampleLevel(g_samp, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    r += g_hist.SampleLevel(g_samp, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += g_hist.SampleLevel(g_samp, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    r += g_hist.SampleLevel(g_samp, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);

    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    r = r / wsum;
    r = r + g_extra * (r - plain);

    const float2 uv = pos / size;

    [branch] if ((g_flags & 4) != 0) {
        const float4 gr = g_hist.GatherRed(g_samp, uv);
        const float4 gg = g_hist.GatherGreen(g_samp, uv);
        const float4 gb = g_hist.GatherBlue(g_samp, uv);
        const float3 lo = float3(min(min(gr.x, gr.y), min(gr.z, gr.w)), min(min(gg.x, gg.y), min(gg.z, gg.w)), min(min(gb.x, gb.y), min(gb.z, gb.w)));
        const float3 hi = float3(max(max(gr.x, gr.y), max(gr.z, gr.w)), max(max(gg.x, gg.y), max(gg.z, gg.w)), max(max(gb.x, gb.y), max(gb.z, gb.w)));
        r = clamp(r, lo, hi);
    } else if ((g_flags & 8) != 0) {
        // the green of the four texels bounds the result's green; the other channels scale with it
        const float4 gg = g_hist.GatherGreen(g_samp, uv);
        const float glo = min(min(gg.x, gg.y), min(gg.z, gg.w));
        const float ghi = max(max(gg.x, gg.y), max(gg.z, gg.w));
        float k = 1.0;

        if (r.g > ghi) {
            k = ghi / max(r.g, 1e-6);
        } else if (r.g < glo) {
            k = glo / max(r.g, 1e-6);
        }

        r = r * clamp(k, 0.0, 4.0);
    }

    return max(r, 0.0);
}

[numthreads(8, 8, 1)]
void accumulate(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_hist_size)) {
        return;
    }

    const float2 in_sz = float2(g_in_size);
    const float2 h_sz = float2(g_hist_size);
    const int2 last = int2(g_in_size) - 1;
    // the texel's centre in view pixels
    const float2 u = (float2(id.xy) + 0.5) * in_sz / h_sz;

    // ---- the current frame at the texel: the 2 x 2 samples around it (pixel q's sample sits at q + 0.5 - jitter),
    //      weighted by their distance; kappa = how much this frame knows about the texel ---------------------------------
    const float2 qf = u - 0.5 + g_jitter;
    float3 cur;
    float kappa;

    [branch] if ((g_flags & 16) != 0) {
        const int2 qn = int2(floor(qf + 0.5));
        const float2 d = (float2(qn) + 0.5 - g_jitter) - u;
        cur = g_lin.Load(int3(clamp(qn, int2(0, 0), last), 0)).rgb;
        kappa = clamp(exp(-g_k * dot(d, d)), 1e-3, 1.0);
    } else {
        const int2 q0 = int2(floor(qf));
        float3 acc = 0.0;
        float wsum = 0.0;

        [unroll] for (int dy = 0; dy <= 1; ++dy) {
            [unroll] for (int dx = 0; dx <= 1; ++dx) {
                const int2 q = q0 + int2(dx, dy);
                const float2 d = (float2(q) + 0.5 - g_jitter) - u;
                const float w = exp(-g_k * dot(d, d));
                acc += g_lin.Load(int3(clamp(q, int2(0, 0), last), 0)).rgb * w;
                wsum += w;
            }
        }

        cur = acc / max(wsum, 1e-6);
        kappa = clamp(wsum, 1e-3, 1.0);
    }

    // ---- the history where the nearest surface of the pixel's 3x3 was ---------------------------------------------------
    const int2 pg = clamp(int2(floor(u)), int2(0, 0), last);
    const float4 g = g_guide.Load(int3(pg, 0));
    const float2 prev_h = (u + g.xy) * h_sz / in_sz;
    const bool valid = (g_flags & 1) == 0 && g.z > 0.0 && all(prev_h >= 0.0) && all(prev_h <= h_sz);
    // protection (what one frame's samples cannot show, see the analyze pass) and the scatter of the samples there
    const float protect = valid ? g.w : 0.0;
    const float sde = sqrt(max(g_stats.Load(int3(pg, 0)).w, 0.0));

    const float4 hb = g_hist.SampleLevel(g_samp, prev_h / h_sz, 0);
    const float3 h_rgb = history_at(prev_h, hb.rgb);

    // the history has to be within what this frame shows around the texel (the frame's 3x3 box, perceptual YCoCg);
    // where it is protected, within that plus the scatter the samples have shown there
    const float2 uv_in = u / in_sz;
    const float3 bc = g_boxc.SampleLevel(g_samp, uv_in, 0).xyz;
    const float3 be = g_boxe.SampleLevel(g_samp, uv_in, 0).xyz + g_relax * sde * protect * float3(1.0, 0.5, 0.5);
    const float3 hcp = clip_to_box(bc - be, bc + be, to_ycocg(enc(h_rgb)));
    const float3 hc = max(from_ycocg(hcp), 0.0);
    const float3 h_lin = hc * hc;

    // protected: the frame's sample no farther from the history than a few deviations (one spark adds little, a
    // stochastic shadow's sample does not pull the average about)
    [branch] if (g_wins > 0.0 && protect > 0.0) {
        const float3 dd = to_ycocg(enc(cur)) - hcp;
        const float lim = max(g_wins * sde, g_wins_floor);
        const float3 cl = max(from_ycocg(hcp + dd * min(1.0, lim / max(abs(dd.x), 1e-6))), 0.0);
        cur = cur + (cl * cl - cur) * protect;
    }

    // the weight the history keeps: what the pixel's test allows (g.z), less on fast motion
    const float pixels = length(g.xy);
    const float wmax = g.z * (1.0 - g_motion_reduce * saturate(pixels / 24.0));
    const float wacc = valid ? min(hb.a, wmax) : 0.0;
    const float alpha = kappa / (wacc + kappa);
    float3 res;

    if ((g_flags & 2) != 0) {
        const float lc = to_ycocg(cur).x;
        const float lh = to_ycocg(h_lin).x;
        const float wc = alpha / (1.0 + max(lc, 0.0));
        const float wh = (1.0 - alpha) / (1.0 + max(lh, 0.0));
        res = (cur * wc + h_lin * wh) / max(wc + wh, 1e-6);
    } else {
        res = h_lin + (cur - h_lin) * alpha;
    }

    if (!valid) {
        res = cur;
    }

    g_out[id.xy] = float4(max(res, 0.0), min(wacc + kappa, g_w_cap));
}
)";

// ---- encode / sharpen: one thread per fovea pixel ---------------------------------------------------------------------
constexpr char RESOLVE_SHADER[] =
    R"(
Texture2D<float4>   g_src   : register(t0); // encode: the history; sharpen: the encoded image
Texture2D<float4>   g_guide : register(t1); // sharpen, debug views
Texture2D<float4>   g_hist  : register(t2); // sharpen, debug views
Texture2D<float4>   g_split : register(t3); // sharpen, split: the other anti-aliasing's image
Texture2D<float4>   g_stats : register(t4); // sharpen, debug views: noise, fast mean, mean and variance of e (the view's size)
Texture2D<float4>   g_rel   : register(t5); // sharpen, debug views: distance, the protection's release, ... (the view's size)
RWTexture2D<float4> g_dst   : register(u0);
SamplerState        g_samp  : register(s0);

cbuffer Params : register(b0) {
    uint2  g_src_size;
    uint2  g_dst_size;
    uint   g_flags;   // 1 = encode SRGB, 4 = 2 : 1 with Lanczos 2, 8 = 2 : 1 as the mean of 2 x 2
    float  g_sharp;   // RCAS amount 0..1
    uint   g_debug;   // sharpen: debug view
    uint   g_split_x; // sharpen: pixels left of it from g_split (0 = off)
    float4 g_lz;      // Lanczos 2 as two bilinear taps per side: position a, weight a, position b, weight b
};
)" SUPERTAA_COMMON R"(
[numthreads(8, 8, 1)]
void encode(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_dst_size)) {
        return;
    }

    float3 c;

    [branch] if ((g_flags & 4) != 0) {
        // 8 x 8 history texels around the pixel's centre (which lies between four of them) as 4 x 4 bilinear taps
        const float2 centre = 2.0 * float2(id.xy) + 1.0;
        const float2 inv = 1.0 / float2(g_src_size);
        const float off[4] = {-g_lz.z, -g_lz.x, g_lz.x, g_lz.z};
        const float wt[4] = {g_lz.w, g_lz.y, g_lz.y, g_lz.w};
        c = 0.0;

        [unroll] for (int j = 0; j < 4; ++j) {
            [unroll] for (int i = 0; i < 4; ++i) {
                c += g_src.SampleLevel(g_samp, (centre + float2(off[i], off[j])) * inv, 0).rgb * (wt[i] * wt[j]);
            }
        }

        c = max(c, 0.0);
    } else if ((g_flags & 8) != 0) {
        c = g_src.SampleLevel(g_samp, (2.0 * float2(id.xy) + 1.0) / float2(g_src_size), 0).rgb;
    } else {
        c = g_src.Load(int3(id.xy, 0)).rgb;
    }

    g_dst[id.xy] = float4((g_flags & 1) != 0 ? to_srgb(c) : saturate(c), 1.0);
}

float3 tap(int2 q) {
    return saturate(g_src.Load(int3(clamp(q, int2(0, 0), int2(g_dst_size) - 1), 0)).rgb);
}

// debug pictures over the image (see SuperTAA::Params::debug_view)
float3 debug_view(int2 q, float3 img) {
    uint gw, gh;
    g_guide.GetDimensions(gw, gh);
    const int2 gp = clamp(int2((float2(q) + 0.5) * float2(gw, gh) / float2(g_dst_size)), int2(0, 0), int2(gw, gh) - 1);
    const float4 g = g_guide.Load(int3(gp, 0));

    const float4 st = g_stats.Load(int3(gp, 0));
    const float rel_all = g_rel.Load(int3(gp, 0)).y;
    const float rel = saturate(rel_all);

    if (g_debug == 1) {
        // no history: blue; protected (the history may hold what this frame's samples do not show): green; the
        // protection released (a change seen, the rim of a moving thing left or entered, and a while after): red;
        // a thing that arrived (a fly), and around it for a few frames: white
        const float3 kept = lerp(lerp(img, float3(0.0, 1.0, 0.0), 0.6 * saturate(g.w)), float3(1.0, 0.0, 0.0), 0.8 * rel);
        const float3 marked = lerp(kept, float3(1.0, 1.0, 1.0), 0.9 * saturate(rel_all - 1.0));
        return g.z <= 0.0 ? lerp(img, float3(0.1, 0.3, 1.0), 0.6) : marked;
    }

    if (g_debug == 2) {
        // the noise believed (0.05 = full yellow)
        const float v = saturate(st.x / 0.05);
        return float3(v, v, 0.25 * img.g);
    }

    if (g_debug == 5) {
        // the scatter of the samples (deviation of e; 0.10 = full)
        const float v = saturate(sqrt(max(st.w, 0.0)) / 0.10);
        return float3(v, 0.6 * v, 0.25 * img.g);
    }

    if (g_debug == 3) {
        // the weight the history holds (8 = white)
        uint hw, hh;
        g_hist.GetDimensions(hw, hh);
        const int2 hp = clamp(int2((float2(q) + 0.5) * float2(hw, hh) / float2(g_dst_size)), int2(0, 0), int2(hw, hh) - 1);
        const float v = saturate(g_hist.Load(int3(hp, 0)).a / 8.0);
        return float3(v, v, v);
    }

    if (g_debug == 4) {
        // motion of the nearest surface: red from 0 to 8 pixels per frame, green up to 1 pixel
        const float m = length(g.xy);
        return lerp(img, float3(saturate(m / 8.0), 0.5 * saturate(m), 0.0), 0.7);
    }

    return img;
}

[numthreads(8, 8, 1)]
void sharpen(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_dst_size)) {
        return;
    }

    const int2 q = int2(id.xy);
    float3 e = tap(q);

    [branch] if (g_sharp > 0.0) {
        // RCAS: the strongest negative lobe that keeps the cross around the pixel within [0, 1] (no clipping, no halos)
        const float3 b = tap(q + int2(0, -1));
        const float3 d = tap(q + int2(-1, 0));
        const float3 f = tap(q + int2(1, 0));
        const float3 h = tap(q + int2(0, 1));
        const float3 mn4 = min(min(b, d), min(f, h));
        const float3 mx4 = max(max(b, d), max(f, h));
        const float3 hit_min = min(mn4, e) / (4.0 * mx4 + 1e-5);
        const float3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
        const float3 lobe3 = max(-hit_min, hit_max);
        const float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * g_sharp;
        e = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    }

    e = max(e, 0.0);

    [branch] if (g_debug != 0) {
        e = debug_view(q, e);
    }

    [branch] if (g_split_x != 0) {
        if (id.x < g_split_x) {
            e = g_split.Load(int3(q, 0)).rgb;
        }

        if (id.x == g_split_x || id.x + 1 == g_split_x) {
            e = float3(1.0, 1.0, 0.0);
        }
    }

    g_dst[id.xy] = float4(e, 1.0);
}
)";

#undef SUPERTAA_COMMON

// The compiled code is kept for the session (it does not depend on the device): a re-initialisation of the quad view
// makes the pipelines again without compiling (see compile_cached in FoveaComposite.cpp).
HRESULT compile_once(int slot, const char* src, size_t size, const char* name, const char* entry, ID3DBlob** code, ID3DBlob** errors) {
    static std::mutex s_mutex{};
    static ID3DBlob* s_code[5]{};

    {
        std::scoped_lock _{s_mutex};

        if (s_code[slot] != nullptr) {
            *code = s_code[slot];
            (*code)->AddRef();

            if (errors != nullptr) {
                *errors = nullptr;
            }

            return S_OK;
        }
    }

    const HRESULT hr = compile_shader(src, size, name, nullptr, nullptr, entry, "cs_5_0", 0, 0, code, errors); // [BUILT-IN SHADERS fix145]

    if (SUCCEEDED(hr) && *code != nullptr) {
        std::scoped_lock _{s_mutex};

        if (s_code[slot] == nullptr) {
            s_code[slot] = *code;
            s_code[slot]->AddRef(); // kept to the end of the process on purpose
        }
    }

    return hr;
}

DXGI_FORMAT uav_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return f;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool is_srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// typed SRV format for a depth(-stencil) texture: its depth plane as a float / unorm
DXGI_FORMAT depth_srv_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// typed SRV format for a (possibly typeless) colour texture
DXGI_FORMAT srv_format_for(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}

// a compute read needs NON_PIXEL_SHADER_RESOURCE; a resource resting in a read state keeps its bits too
D3D12_RESOURCE_STATES compute_read_state(D3D12_RESOURCE_STATES resting) {
    if (resting == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || resting == D3D12_RESOURCE_STATE_COMMON) {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    return (D3D12_RESOURCE_STATES)(resting | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after || res == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

void uav_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmd->ResourceBarrier(1, &b);
}

// one descriptor table (SRVs then UAVs), root constants at b0, a linear clamp sampler at s0
bool make_root(ID3D12Device* device, uint32_t srvs, uint32_t uavs, uint32_t constants, ComPtr<ID3D12RootSignature>& out) {
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = srvs;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = uavs;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = srvs;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = constants;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> blob{};
    ComPtr<ID3DBlob> errors{};

    return SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
           SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)));
}

bool make_image(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, const wchar_t* name, ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = w;
    tex.Height = h;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    out.Reset();

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)))) {
        return false;
    }

    out->SetName(name);
    return true;
}
} // namespace

const char* SuperTAA::analyze_source() {
    return ANALYZE_SHADER;
}

// [UI MOTION fix125] the analyze pass's text with the interface's motion (UiMotion.hpp's text first)
std::string SuperTAA::analyze_ui_motion_source() {
    return std::string{"#define UI_MOTION 1\n#define UM_REG t8\n"} + UiMotion::hlsl() + ANALYZE_SHADER;
}

const char* SuperTAA::accumulate_source() {
    return ACCUMULATE_SHADER;
}

const char* SuperTAA::resolve_source() {
    return RESOLVE_SHADER;
}

bool SuperTAA::setup(ID3D12Device* device) {
    if (m_pso_analyze != nullptr && m_pso_accumulate != nullptr && m_pso_encode != nullptr && m_pso_sharpen != nullptr && m_heap != nullptr) {
        return true;
    }

    if (m_failed || device == nullptr) {
        return false;
    }

    m_failed = true;

    struct Pass {
        int slot;
        const char* src;
        size_t size;
        const char* name;
        const char* entry;
        uint32_t srvs;
        uint32_t uavs;
        uint32_t constants;
        ComPtr<ID3D12RootSignature>* root;
        ComPtr<ID3D12PipelineState>* pso;
    };

    const Pass passes[]{
        {0, ANALYZE_SHADER, sizeof(ANALYZE_SHADER) - 1, "SuperTAAAnalyze", "analyze", 6, 6, ANALYZE_K, &m_root_analyze, &m_pso_analyze},
        {1, ACCUMULATE_SHADER, sizeof(ACCUMULATE_SHADER) - 1, "SuperTAAAccumulate", "accumulate", 6, 1, ACCUMULATE_K, &m_root_accumulate, &m_pso_accumulate},
        {2, RESOLVE_SHADER, sizeof(RESOLVE_SHADER) - 1, "SuperTAAEncode", "encode", 1, 1, RESOLVE_K, &m_root_encode, &m_pso_encode},
        {3, RESOLVE_SHADER, sizeof(RESOLVE_SHADER) - 1, "SuperTAASharpen", "sharpen", 6, 1, RESOLVE_K, &m_root_sharpen, &m_pso_sharpen},
    };

    auto fail = [&](const char* why) {
        m_root_analyze.Reset();
        m_root_accumulate.Reset();
        m_root_encode.Reset();
        m_root_sharpen.Reset();
        m_pso_analyze.Reset();
        m_pso_accumulate.Reset();
        m_pso_encode.Reset();
        m_pso_sharpen.Reset();
        m_heap.Reset();
        m_last_error = why;
        return false;
    };

    for (const auto& pass : passes) {
        ComPtr<ID3DBlob> code{};
        ComPtr<ID3DBlob> errors{};

        if (FAILED(compile_once(pass.slot, pass.src, pass.size, pass.name, pass.entry, &code, &errors)) || code == nullptr) {
            REVR_LOG(spdlog::error("[SuperTAA] shader {} did not compile: {}", pass.entry, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
            return fail("shader compile failed");
        }

        if (!make_root(device, pass.srvs, pass.uavs, pass.constants, *pass.root)) {
            REVR_LOG(spdlog::error("[SuperTAA] root signature of {} failed", pass.entry));
            return fail("root signature failed");
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = pass.root->Get();
        pso_desc.CS.pShaderBytecode = code->GetBufferPointer();
        pso_desc.CS.BytecodeLength = code->GetBufferSize();

        if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pass.pso->ReleaseAndGetAddressOf())))) {
            REVR_LOG(spdlog::error("[SuperTAA] CreateComputePipelineState of {} failed", pass.entry));
            return fail("pipeline state failed");
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * DESCRIPTORS_PER_DISPATCH;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        REVR_LOG(spdlog::error("[SuperTAA] CreateDescriptorHeap failed"));
        return fail("descriptor heap failed");
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_failed = false;
    m_last_error = "";
    REVR_LOG(spdlog::info("[SuperTAA] ready"));
    return true;
}

// [UI MOTION fix125 22.11.2026] the analyze pass with the interface's motion: made when asked for (set_ui_motion before
// prepare, or the first dispatch that brings the interface's copies). If it cannot be made the plain pass goes on.
bool SuperTAA::ensure_ui_motion(ID3D12Device* device) {
    if (m_pso_analyze_um != nullptr && m_root_analyze_um != nullptr) {
        return true;
    }

    if (m_um_failed || device == nullptr) {
        return false;
    }

    m_um_failed = true;
    const std::string src = analyze_ui_motion_source();
    ComPtr<ID3DBlob> code{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_once(4, src.data(), src.size(), "SuperTAAAnalyzeUiMotion", "analyze", &code, &errors)) || code == nullptr) {
        REVR_LOG(spdlog::error("[SuperTAA] shader analyze (interface motion) did not compile: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?"));
        return false;
    }

    // (the plain pass's kind of root signature with three more SRVs: t6, t7 the view's copies, t8 the motion's block)
    if (!make_root(device, 9, 6, ANALYZE_K, m_root_analyze_um)) {
        REVR_LOG(spdlog::error("[SuperTAA] root signature of analyze (interface motion) failed"));
        m_root_analyze_um.Reset();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_analyze_um.Get();
    pso_desc.CS.pShaderBytecode = code->GetBufferPointer();
    pso_desc.CS.BytecodeLength = code->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(m_pso_analyze_um.ReleaseAndGetAddressOf())))) {
        REVR_LOG(spdlog::error("[SuperTAA] CreateComputePipelineState of analyze (interface motion) failed"));
        m_root_analyze_um.Reset();
        m_pso_analyze_um.Reset();
        return false;
    }

    m_um_failed = false;
    REVR_LOG(spdlog::info("[SuperTAA] the analyze pass with the interface's own motion is ready"));
    return true;
}

bool SuperTAA::prepare(ID3D12Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t scale) {
    if (device == nullptr || in_w == 0 || in_h == 0 || out_w == 0 || out_h == 0) {
        return false;
    }

    if (!setup(device) || !ensure_images(device, in_w, in_h, out_w, out_h, scale)) {
        return false;
    }

    if (m_um_wanted) {
        ensure_ui_motion(device); // (without it the plain pass)
    }

    return true;
}

// histories (history size), statistics and distances (render size) per eye, each twice (read one, write the other), and
// the images of one frame; all rest in UNORDERED_ACCESS; made again on a size change
bool SuperTAA::ensure_images(ID3D12Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t scale) {
    scale = scale >= 2 ? 2u : 1u;
    const uint32_t hist_w = out_w * scale;
    const uint32_t hist_h = out_h * scale;

    if (m_hist[0][0] != nullptr && m_in_w == in_w && m_in_h == in_h && m_out_w == out_w && m_out_h == out_h && m_hist_w == hist_w && m_hist_h == hist_h &&
        m_slots_made == m_slots) {
        return true;
    }

    // [GUARD fix101 20.11.2026] Only the output's size changed and the history's did not (the periphery's instance: an image
    // of the wide view's size with 2 x 2 history texels per pixel <-> an image of twice that size with one; the GPU
    // guard's step): the history, the statistics and the distances stay as they are, so the picture goes on without
    // starting again. Only the encoded image, a per-frame image of the output's size, is another: the one of the other
    // size is kept beside it.
    if (m_hist[0][0] != nullptr && m_in_w == in_w && m_in_h == in_h && m_hist_w == hist_w && m_hist_h == hist_h && m_slots_made == m_slots) {
        std::swap(m_enc, m_enc_other);
        std::swap(m_out_w, m_other_w);
        std::swap(m_out_h, m_other_h);

        if (m_enc == nullptr || m_out_w != out_w || m_out_h != out_h) {
            if (m_enc != nullptr) {
                m_retired.emplace_back(std::move(m_enc), RETIRE_DISPATCHES);
            }

            if (!make_image(device, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA encoded image", m_enc)) {
                m_last_error = "could not create the encoded image";
                m_in_w = m_in_h = m_out_w = m_out_h = m_hist_w = m_hist_h = 0;
                return false;
            }

            m_out_w = out_w;
            m_out_h = out_h;
            REVR_LOG(spdlog::info("[SuperTAA] view {}x{}: image now {}x{}, the history {}x{} kept", in_w, in_h, out_w, out_h, hist_w, hist_h));
        }

        return true;
    }

    // (the images in use so far are not released here: the command lists of the last frames may still read and write
    // them. They are kept for RETIRE_DISPATCHES more dispatches, see dispatch.)
    auto retire = [&](ComPtr<ID3D12Resource>& image) {
        if (image != nullptr) {
            m_retired.emplace_back(std::move(image), RETIRE_DISPATCHES);
        }

        image.Reset();
    };

    auto drop = [&]() {
        for (int e = 0; e < 2; ++e) {
            for (int i = 0; i < 2; ++i) {
                retire(m_hist[e][i]);
                retire(m_stats[e][i]);
                retire(m_dist[e][i]);
            }

            m_last_hist[e] = m_prev_hist[e] = nullptr;
            m_last_stats[e] = m_prev_stats[e] = nullptr;
            m_last_dist[e] = m_prev_dist[e] = nullptr;
            m_valid[e] = false;
            m_cur[e] = 0;
        }

        retire(m_lin);
        retire(m_guide);
        retire(m_boxc);
        retire(m_boxe);
        retire(m_enc);
        retire(m_enc_other);
        m_other_w = m_other_h = 0;
        m_in_w = m_in_h = m_out_w = m_out_h = m_hist_w = m_hist_h = 0;
        m_slots_made = 0;
    };

    drop();
    bool ok = true;

    // ([fix80] one view's images only for the instance of the shared wide view, see set_slots)
    for (int e = 0; e < (int)m_slots && ok; ++e) {
        for (int i = 0; i < 2 && ok; ++i) {
            ok = make_image(device, hist_w, hist_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA history", m_hist[e][i]) &&
                 make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA statistics", m_stats[e][i]) &&
                 make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA distances", m_dist[e][i]);
        }
    }

    ok = ok && make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA linear colour", m_lin) &&
         make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA guide", m_guide) &&
         make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA box centre", m_boxc) &&
         make_image(device, in_w, in_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA box extent", m_boxe) &&
         make_image(device, out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR super TAA encoded image", m_enc);

    if (!ok) {
        REVR_LOG(spdlog::error("[SuperTAA] could not create the images (view {}x{}, fovea {}x{}, history {}x{})", in_w, in_h, out_w, out_h, hist_w, hist_h));
        drop();
        m_last_error = "could not create the images";
        return false;
    }

    m_in_w = in_w;
    m_in_h = in_h;
    m_out_w = out_w;
    m_out_h = out_h;
    m_hist_w = hist_w;
    m_hist_h = hist_h;
    m_slots_made = m_slots;
    // (per view: two histories, two statistics and two distance images; per frame: four images of the view's size and one
    // of the image's)
    REVR_LOG(spdlog::info("[SuperTAA] view {}x{}, {} {}x{}, history {}x{} ({:.0f} MB of images)", in_w, in_h, m_slots == 1 ? "image" : "fovea", out_w, out_h, hist_w,
        hist_h,
        ((double)hist_w * hist_h * 8.0 * 2.0 * m_slots + (double)in_w * in_h * (8.0 * 2.0 * m_slots + 8.0 * 2.0 * m_slots + 8.0 * 4.0) +
            (double)out_w * out_h * 8.0) /
            (1024.0 * 1024.0)));
    return true;
}

bool SuperTAA::dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
    D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, const Params& p) {
    if (device == nullptr || cmd == nullptr || color == nullptr || motion == nullptr || out == nullptr) {
        m_last_error = "missing input";
        return false;
    }

    if (!setup(device)) {
        return false;
    }

    // images replaced a while ago: the lists that used them are done
    for (auto it = m_retired.begin(); it != m_retired.end();) {
        if (it->second <= 1) {
            it = m_retired.erase(it);
        } else {
            --it->second;
            ++it;
        }
    }

    const auto cdesc = color->GetDesc();
    const auto odesc = out->GetDesc();
    const auto color_format = srv_format_for(cdesc.Format);
    const auto motion_format = srv_format_for(motion->GetDesc().Format);
    const auto depth_format = depth != nullptr ? depth_srv_format_for(depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
    const auto out_format = uav_format_for(odesc.Format);

    if (out_format == DXGI_FORMAT_UNKNOWN || is_srgb(odesc.Format)) {
        m_last_error = "unsupported fovea image format";
        return false;
    }

    const uint32_t in_w = (uint32_t)cdesc.Width;
    const uint32_t in_h = cdesc.Height;
    const uint32_t out_w = (uint32_t)odesc.Width;
    const uint32_t out_h = odesc.Height;
    const auto& s = p.s;
    const uint32_t scale = s.scale >= 2 ? 2u : 1u;

    if (!ensure_images(device, in_w, in_h, out_w, out_h, scale)) {
        return false;
    }

    // the split's source has to be an image like the fovea image
    ID3D12Resource* split = p.split_x != 0 ? p.split_src : nullptr;

    if (split != nullptr) {
        const auto sd = split->GetDesc();

        if (sd.Width != odesc.Width || sd.Height != odesc.Height || uav_format_for(sd.Format) == DXGI_FORMAT_UNKNOWN || is_srgb(sd.Format)) {
            split = nullptr;
        }
    }

    const uint32_t eye = (std::min)(p.eye, m_slots - 1u);
    const uint32_t cur = m_cur[eye];
    ID3D12Resource* hist_in = m_hist[eye][cur].Get();
    ID3D12Resource* hist_out = m_hist[eye][cur ^ 1u].Get();
    ID3D12Resource* stats_in = m_stats[eye][cur].Get();
    ID3D12Resource* stats_out = m_stats[eye][cur ^ 1u].Get();
    ID3D12Resource* dist_in = m_dist[eye][cur].Get();
    ID3D12Resource* dist_out = m_dist[eye][cur ^ 1u].Get();
    const bool have_depth = depth_format != DXGI_FORMAT_UNKNOWN;
    const bool dist_test = have_depth && p.depth_k_valid && s.depth_reject > 0.0f;
    const bool reset = p.reset || !m_valid[eye];
    const float sharp = std::clamp(s.sharpen, 0.0f, 1.0f);
    const bool final_pass = sharp > 0.0f || p.debug_view != 0 || split != nullptr;

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * DESCRIPTORS_PER_DISPATCH) * m_descriptor_size;

    auto slot = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * m_descriptor_size}; };
    auto table = [&](uint32_t i) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + (UINT64)i * m_descriptor_size}; };

    constexpr auto F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    auto srv_at = [&](uint32_t i, ID3D12Resource* res, DXGI_FORMAT format) {
        srv.Format = format;
        device->CreateShaderResourceView(res, &srv, slot(i));
    };
    auto uav_at = [&](uint32_t i, ID3D12Resource* res, DXGI_FORMAT format) {
        uav.Format = format;
        device->CreateUnorderedAccessView(res, nullptr, &uav, slot(i));
    };

    // [UI MOTION fix125 22.11.2026] the view's interface copies (resting in COMMON, read by promotion like the eye pass
    // reads them) and the interface's motion: the analyze pass that uses them. Only with copies of the colour's size.
    bool ui_motion = p.ui_motion && p.ui_hud != nullptr && p.ui_fin != nullptr;

    if (ui_motion) {
        const auto hd = p.ui_hud->GetDesc();
        const auto fd = p.ui_fin->GetDesc();
        ui_motion = hd.Width == cdesc.Width && hd.Height == cdesc.Height && fd.Width == cdesc.Width && fd.Height == cdesc.Height && ensure_ui_motion(device);
    }

    m_last_ui_motion = ui_motion;

    // analyze (the plain pass: 6 SRVs, then the UAVs; with the interface's motion: 9 SRVs, then the UAVs)
    const uint32_t A = 0;
    const uint32_t AU = A + (ui_motion ? 9u : 6u);
    srv_at(A + 0, color, color_format);
    srv_at(A + 1, motion, motion_format);
    // (the table needs a valid descriptor in every slot: a null view reads 0)
    srv_at(A + 2, have_depth ? depth : nullptr, have_depth ? depth_format : DXGI_FORMAT_R32_FLOAT);
    srv_at(A + 3, stats_in, F16);
    srv_at(A + 4, dist_in, F16);
    srv_at(A + 5, hist_in, F16);

    if (ui_motion) {
        srv_at(A + 6, p.ui_hud, srv_format_for(p.ui_hud->GetDesc().Format));
        srv_at(A + 7, p.ui_fin, srv_format_for(p.ui_fin->GetDesc().Format));
        UiMotion::srv(device, p.ui_motion, slot(A + 8));
    }

    uav_at(AU + 0, m_lin.Get(), F16);
    uav_at(AU + 1, m_guide.Get(), F16);
    uav_at(AU + 2, m_boxc.Get(), F16);
    uav_at(AU + 3, m_boxe.Get(), F16);
    uav_at(AU + 4, stats_out, F16);
    uav_at(AU + 5, dist_out, F16);
    // accumulate
    const uint32_t B = D_ANALYZE;
    srv_at(B + 0, m_lin.Get(), F16);
    srv_at(B + 1, m_guide.Get(), F16);
    srv_at(B + 2, m_boxc.Get(), F16);
    srv_at(B + 3, m_boxe.Get(), F16);
    srv_at(B + 4, hist_in, F16);
    srv_at(B + 5, stats_out, F16);
    uav_at(B + 6, hist_out, F16);
    // encode (into the fovea image itself when nothing follows)
    const uint32_t C = B + D_ACCUMULATE;
    srv_at(C + 0, hist_out, F16);

    if (final_pass) {
        uav_at(C + 1, m_enc.Get(), F16);
    } else {
        uav_at(C + 1, out, out_format);
    }

    // sharpen
    const uint32_t D = C + D_ENCODE;
    srv_at(D + 0, m_enc.Get(), F16);
    srv_at(D + 1, m_guide.Get(), F16);
    srv_at(D + 2, hist_out, F16);
    srv_at(D + 3, split, split != nullptr ? uav_format_for(split->GetDesc().Format) : F16);
    srv_at(D + 4, stats_out, F16);
    srv_at(D + 5, dist_out, F16);
    uav_at(D + 6, out, out_format);

    const auto read_in = compute_read_state(in_state);
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    // ---- analyze -------------------------------------------------------------------------------------------------------
    barrier(cmd, color, in_state, read_in);
    barrier(cmd, motion, in_state, read_in);

    if (have_depth) {
        barrier(cmd, depth, in_state, read_in);
    }

    barrier(cmd, stats_in, UAV, NPSR);
    barrier(cmd, dist_in, UAV, NPSR);
    // (the last history: read here for the test of what changed, then by the accumulation)
    barrier(cmd, hist_in, UAV, NPSR);

    const float w_max = std::clamp(s.stability, 1.0f, 32.0f);
    const float jitter_x = std::clamp(p.jitter_x, -1.0f, 1.0f);
    const float jitter_y = std::clamp(p.jitter_y, -1.0f, 1.0f);
    const float threshold = std::clamp(s.follow_threshold, 0.25f, 4.0f);

    AnalyzeK ak{};
    ak.size[0] = in_w;
    ak.size[1] = in_h;
    ak.mv_scale[0] = p.mv_scale_x;
    ak.mv_scale[1] = p.mv_scale_y;
    std::memcpy(ak.depth_k, p.depth_k, sizeof(ak.depth_k));
    ak.flags = (reset ? 1u : 0u) | (p.reverse_z ? 2u : 0u) | (have_depth ? 4u : 0u) | (is_srgb(color_format) ? 0u : 8u) | (s.follow ? 16u : 0u) |
               (dist_test ? 32u : 0u) | (s.protect ? 64u : 0u) | (s.arrive ? 128u : 0u);
    ak.reject = std::clamp(s.depth_reject, 0.0f, 0.9f);
    ak.gamma = std::clamp(s.clip_gamma, 0.25f, 4.0f);
    ak.w_max = w_max;
    ak.jitter[0] = jitter_x;
    ak.jitter[1] = jitter_y;
    ak.k = std::clamp(s.fl_k, 0.0f, 16.0f) * threshold;
    ak.b0 = std::clamp(s.fl_b0, 0.001f, 0.5f) * threshold;
    ak.kc = std::clamp(s.fl_kc, 0.0f, 16.0f) * threshold;
    ak.b0c = std::clamp(s.fl_b0c, 0.001f, 0.5f) * threshold;
    ak.cap = std::clamp(s.fl_cap, 0.0f, 0.5f);
    ak.rate = std::clamp(s.fl_rate, 0.01f, 1.0f);
    ak.rate_b = std::clamp(s.fl_rate_b, 0.05f, 1.0f);
    ak.w_short = std::clamp(s.follow_memory, 0.0f, w_max);
    ak.count = std::clamp(s.fl_count, 1u, 9u);
    ak.alln = std::clamp(s.fl_alln, 1u, 9u);
    ak.fl_sde = std::clamp(s.fl_sde, 0.0f, 4.0f) * threshold;
    ak.af_rate = std::clamp(s.af_rate, 0.01f, 1.0f);
    ak.af_t0 = std::clamp(s.af_t0, 0.0f, 0.5f);
    ak.af_t1 = (std::max)(std::clamp(s.af_t1, 0.0f, 1.0f), ak.af_t0 + 1e-4f);
    ak.af_mem = s.protect ? std::clamp(s.protect_memory, 1.0f, 4.0f) : 1.0f;
    ak.edge0 = std::clamp(s.edge0, 0.0f, 8.0f);
    ak.edge1 = (std::max)(std::clamp(s.edge1, 0.0f, 16.0f), ak.edge0 + 1e-3f);
    ak.rel_decay = std::clamp(s.rel_decay, 0.0f, 0.99f);
    // [fix81] with the test for things that arrive (the same switch: both are about what a fly leaves behind)
    ak.af_clip = (s.protect && s.arrive) ? std::clamp(s.af_clip, 0.0f, 64.0f) : 0.0f;
    ak.af_floor = std::clamp(s.af_clip_floor, 0.0f, 1.0f);

    if (ui_motion) {
        cmd->SetComputeRootSignature(m_root_analyze_um.Get());
        cmd->SetComputeRootDescriptorTable(0, table(A));
        cmd->SetComputeRoot32BitConstants(1, ANALYZE_K, &ak, 0);
        cmd->SetPipelineState(m_pso_analyze_um.Get());
    } else {
        cmd->SetComputeRootSignature(m_root_analyze.Get());
        cmd->SetComputeRootDescriptorTable(0, table(A));
        cmd->SetComputeRoot32BitConstants(1, ANALYZE_K, &ak, 0);
        cmd->SetPipelineState(m_pso_analyze.Get());
    }

    cmd->Dispatch((in_w + 7) / 8, (in_h + 7) / 8, 1);

    {
        D3D12_RESOURCE_BARRIER written[6]{};
        ID3D12Resource* const res[6]{m_lin.Get(), m_guide.Get(), m_boxc.Get(), m_boxe.Get(), stats_out, dist_out};

        for (int i = 0; i < 6; ++i) {
            written[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            written[i].UAV.pResource = res[i];
        }

        cmd->ResourceBarrier(6, written);
    }

    barrier(cmd, dist_in, NPSR, UAV);
    barrier(cmd, stats_in, NPSR, UAV);
    // (this frame's statistics: read by the accumulation, and by the debug views of the last pass)
    barrier(cmd, stats_out, UAV, NPSR);

    if (have_depth) {
        barrier(cmd, depth, read_in, in_state);
    }

    barrier(cmd, motion, read_in, in_state);
    barrier(cmd, color, read_in, in_state);

    // ---- accumulate ----------------------------------------------------------------------------------------------------
    barrier(cmd, m_lin.Get(), UAV, NPSR);
    barrier(cmd, m_guide.Get(), UAV, NPSR);
    barrier(cmd, m_boxc.Get(), UAV, NPSR);
    barrier(cmd, m_boxe.Get(), UAV, NPSR);

    const float texels_per_pixel = (float)m_hist_w / (float)in_w;
    const float sigma = sigma_px(texels_per_pixel) * std::clamp(s.filter_scale, 0.5f, 2.0f);

    AccumulateK ck{};
    ck.in_size[0] = in_w;
    ck.in_size[1] = in_h;
    ck.hist_size[0] = m_hist_w;
    ck.hist_size[1] = m_hist_h;
    ck.jitter[0] = jitter_x;
    ck.jitter[1] = jitter_y;
    ck.k = 0.5f / (sigma * sigma);
    ck.w_cap = w_max * ak.af_mem;
    ck.motion_reduce = std::clamp(s.motion_reduce, 0.0f, 1.0f);
    ck.extra = std::clamp(s.history_sharpness, 0.0f, 1.5f);
    ck.flags = (reset ? 1u : 0u) | (s.luma_weight ? 2u : 0u) | (s.dering == 1 ? 4u : 0u) | (s.dering >= 2 ? 8u : 0u) | (s.nearest_sample ? 16u : 0u);
    ck.relax = s.protect ? std::clamp(s.protect_range, 0.0f, 8.0f) : 0.0f;
    ck.wins = s.protect ? std::clamp(s.protect_spark, 0.0f, 16.0f) : 0.0f;
    ck.wins_floor = std::clamp(s.af_spark_floor, 0.001f, 0.5f);

    cmd->SetComputeRootSignature(m_root_accumulate.Get());
    cmd->SetComputeRootDescriptorTable(0, table(B));
    cmd->SetComputeRoot32BitConstants(1, ACCUMULATE_K, &ck, 0);
    cmd->SetPipelineState(m_pso_accumulate.Get());
    cmd->Dispatch((m_hist_w + 7) / 8, (m_hist_h + 7) / 8, 1);
    uav_barrier(cmd, hist_out);

    barrier(cmd, hist_in, NPSR, UAV);
    barrier(cmd, m_boxe.Get(), NPSR, UAV);
    barrier(cmd, m_boxc.Get(), NPSR, UAV);
    barrier(cmd, m_lin.Get(), NPSR, UAV);
    // (the guide stays readable for the debug views of the last pass)

    // ---- encode (and reduce) -------------------------------------------------------------------------------------------
    barrier(cmd, hist_out, UAV, NPSR);

    ResolveK rk{};
    rk.src_size[0] = m_hist_w;
    rk.src_size[1] = m_hist_h;
    rk.dst_size[0] = out_w;
    rk.dst_size[1] = out_h;
    rk.flags = 1u | (scale == 2 ? (s.box_resolve ? 8u : 4u) : 0u);
    rk.sharp = 0.0f;
    rk.debug = 0;
    rk.split_x = 0;

    {
        // Lanczos 2 over 8 history texels per axis (at +-0.5, 1.5, 2.5, 3.5 texels = +-0.25 .. 1.75 pixels), each side's
        // two inner and two outer texels as one bilinear tap
        double w[4]{};
        double sum = 0.0;

        for (int i = 0; i < 4; ++i) {
            const double x = ((double)i + 0.5) * 0.5;
            const double pi = 3.14159265358979323846;
            w[i] = (std::sin(pi * x) / (pi * x)) * (std::sin(pi * x * 0.5) / (pi * x * 0.5));
            sum += 2.0 * w[i];
        }

        rk.lz[0] = (float)((0.5 * w[0] + 1.5 * w[1]) / (w[0] + w[1]));
        rk.lz[1] = (float)((w[0] + w[1]) / sum);
        rk.lz[2] = (float)((2.5 * w[2] + 3.5 * w[3]) / (w[2] + w[3]));
        rk.lz[3] = (float)((w[2] + w[3]) / sum);
    }

    cmd->SetComputeRootSignature(m_root_encode.Get());
    cmd->SetComputeRootDescriptorTable(0, table(C));
    cmd->SetComputeRoot32BitConstants(1, RESOLVE_K, &rk, 0);
    cmd->SetPipelineState(m_pso_encode.Get());
    cmd->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    uav_barrier(cmd, final_pass ? m_enc.Get() : out);

    // ---- sharpen (RCAS), debug views, split ----------------------------------------------------------------------------
    if (final_pass) {
        barrier(cmd, m_enc.Get(), UAV, NPSR);
        barrier(cmd, dist_out, UAV, NPSR);

        if (split != nullptr) {
            barrier(cmd, split, UAV, NPSR);
        }

        ResolveK sk = rk;
        sk.src_size[0] = out_w;
        sk.src_size[1] = out_h;
        sk.flags = 0;
        sk.sharp = sharp;
        sk.debug = p.debug_view;
        sk.split_x = split != nullptr ? (std::min)(p.split_x, out_w) : 0u;

        cmd->SetComputeRootSignature(m_root_sharpen.Get());
        cmd->SetComputeRootDescriptorTable(0, table(D));
        cmd->SetComputeRoot32BitConstants(1, RESOLVE_K, &sk, 0);
        cmd->SetPipelineState(m_pso_sharpen.Get());
        cmd->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
        uav_barrier(cmd, out);

        if (split != nullptr) {
            barrier(cmd, split, NPSR, UAV);
        }

        barrier(cmd, dist_out, NPSR, UAV);
        barrier(cmd, m_enc.Get(), NPSR, UAV);
    }

    barrier(cmd, hist_out, NPSR, UAV);
    barrier(cmd, stats_out, NPSR, UAV);
    barrier(cmd, m_guide.Get(), NPSR, UAV);

    m_valid[eye] = true;
    m_cur[eye] = cur ^ 1u;
    m_last_hist[eye] = hist_out;
    m_prev_hist[eye] = hist_in;
    m_last_stats[eye] = stats_out;
    m_prev_stats[eye] = stats_in;
    m_last_dist[eye] = dist_out;
    m_prev_dist[eye] = dist_in;
    m_last_error = "";

    if (p.keep_constants) {
        // (the keys are the shaders' names; tests/supertaa/capture_io.py reads them. "follow_k" tells a capture of fix77
        // or later from one of fix76, "af_mem" one of fix78 or later from one of fix77: their statistics mean other things;
        // bit 128 of the first pass's flags: fix79's test for things that arrive is on)
        m_last_constants = std::format(
            "analyze: size {}x{} mv_scale {:.9g} {:.9g} depth_k {:.9g} {:.9g} {:.9g} {:.9g} flags {} reject {:.9g} gamma {:.9g} w_max {:.9g} follow_k {:.9g} "
            "b0 {:.9g} kc {:.9g} b0c {:.9g} cap {:.9g} rate {:.9g} rate_b {:.9g} w_short {:.9g} count {} alln {} fl_sde {:.9g} af_rate {:.9g} af_t0 {:.9g} "
            "af_t1 {:.9g} af_mem {:.9g} edge0 {:.9g} edge1 {:.9g} rel_decay {:.9g} af_clip {:.9g} af_floor {:.9g}\n"
            "accumulate: in {}x{} hist {}x{} jitter {:.9g} {:.9g} k {:.9g} (sigma {:.9g} px) w_cap {:.9g} motion_reduce {:.9g} extra {:.9g} flags {} "
            "relax {:.9g} wins {:.9g} wins_floor {:.9g}\n"
            "resolve: out {}x{} scale {} flags {} sharpen {:.9g} lanczos {:.9g} {:.9g} {:.9g} {:.9g} final_pass {} debug {} split_x {}\n",
            ak.size[0], ak.size[1], ak.mv_scale[0], ak.mv_scale[1], ak.depth_k[0], ak.depth_k[1], ak.depth_k[2], ak.depth_k[3], ak.flags, ak.reject, ak.gamma,
            ak.w_max, ak.k, ak.b0, ak.kc, ak.b0c, ak.cap, ak.rate, ak.rate_b, ak.w_short, ak.count, ak.alln, ak.fl_sde, ak.af_rate, ak.af_t0, ak.af_t1,
            ak.af_mem, ak.edge0, ak.edge1, ak.rel_decay, ak.af_clip, ak.af_floor, ck.in_size[0], ck.in_size[1], ck.hist_size[0], ck.hist_size[1], ck.jitter[0], ck.jitter[1], ck.k,
            sigma, ck.w_cap, ck.motion_reduce, ck.extra, ck.flags, ck.relax, ck.wins, ck.wins_floor, out_w, out_h, scale, rk.flags, sharp,
            rk.lz[0], rk.lz[1], rk.lz[2], rk.lz[3], final_pass, p.debug_view, split != nullptr ? p.split_x : 0u);
    }

    return true;
}

void SuperTAA::invalidate() {
    m_valid[0] = m_valid[1] = false;
}

void SuperTAA::reset() {
    m_pso_analyze_um.Reset(); // [UI MOTION fix125]
    m_root_analyze_um.Reset();
    m_um_failed = false;
    m_last_ui_motion = false;
    m_pso_analyze.Reset();
    m_pso_accumulate.Reset();
    m_pso_encode.Reset();
    m_pso_sharpen.Reset();
    m_root_analyze.Reset();
    m_root_accumulate.Reset();
    m_root_encode.Reset();
    m_root_sharpen.Reset();
    m_heap.Reset();

    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 2; ++i) {
            m_hist[e][i].Reset();
            m_stats[e][i].Reset();
            m_dist[e][i].Reset();
        }

        m_last_hist[e] = m_prev_hist[e] = nullptr;
        m_last_stats[e] = m_prev_stats[e] = nullptr;
        m_last_dist[e] = m_prev_dist[e] = nullptr;
        m_valid[e] = false;
        m_cur[e] = 0;
    }

    m_lin.Reset();
    m_guide.Reset();
    m_boxc.Reset();
    m_boxe.Reset();
    m_enc.Reset();
    m_enc_other.Reset();
    m_other_w = m_other_h = 0;
    m_retired.clear();
    m_in_w = m_in_h = m_out_w = m_out_h = m_hist_w = m_hist_h = 0;
    m_slots_made = 0;
    m_ring = 0;
    m_failed = false;
    m_last_error = "";
}

} // namespace d3d12
