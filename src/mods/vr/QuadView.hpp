// QuadView.hpp -- quad-view foveated rendering for RE4VR in Single Frame Multipass: both eyes every frame.
//
// Every frame renders (all at the SAME render size: the engine shares one SceneView size between the cameras):
//   * one wide view from between the eyes, covering both eyes' frusta (the "shared periphery"), reprojected to each
//     eye with its depth, anti-aliased by the mod's light TAA and upscaled spatially to the eye image
//     (for comparison: one wide view per eye instead, four scene layers)
//   * per eye a fovea view: an off-axis sub-frustum around the gaze point, anti-aliased by the upscaler plugin (FSR 4 /
//     FSR 3) or by the mod's TAA, at exactly the size of the gaze rectangle in the eye image
// The fovea is blended into the eye image (FoveaComposite), then each eye's interface is put together (MonoUi).
//
// This header holds the geometry (portable, unit-tested in tests/quadview_math_test.cpp) and the small
// thread-safe state that FoveatedRendering (settings, gaze), VR (projection hook, camera clones),
// TemporalUpscaler (instances, evaluate) and FoveaComposite share. No engine / D3D12 dependency.
#pragma once

#include "ReleaseBuild.hpp" // [RELEASE SWITCH fix139] REVR_RELEASE, for every file of the mod
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace quadview {
// [BUILD STAMP fix107 20.11.2026] Which object files the DLL was linked from. The test of 20.11.2026 18:37 ran fix105 again
// with fix106's sources on disk: the packages' files carried the time of the machine that made them, one hour behind the
// user's clock and so older than the object files of his last build, and the build skipped every one of them. A build that
// skips only some would link sources compiled against different versions of these headers (structures of different
// layouts) without any message. So each main source records when it was compiled and the HEADER_REV it saw; the quad view
// logs them at start and reports a source that is missing or saw another HEADER_REV.
// HEADER_REV changes with every delivered change of QuadView.hpp, FoveaComposite.hpp, CameraDuplicator.hpp,
// TemporalUpscaler.hpp or FoveatedRendering.hpp.
inline constexpr int HEADER_REV = 133;

struct BuildStamps {
    static constexpr int MAX = 16;
    const char* source[MAX]{};
    const char* compiled[MAX]{};
    int rev[MAX]{};
    int count{0};

    static BuildStamps& get() {
        static BuildStamps s{};
        return s;
    }
};

struct BuildStampEntry {
    BuildStampEntry(const char* source, const char* compiled, int rev) {
        auto& s = BuildStamps::get();

        if (s.count < BuildStamps::MAX) {
            s.source[s.count] = source;
            s.compiled[s.count] = compiled;
            s.rev[s.count] = rev;
            ++s.count;
        }
    }
};

#if REVR_RELEASE
#define QUADVIEW_BUILD_STAMP(name) // [RELEASE SWITCH fix141] the stamps are for the debug log only
#else
#define QUADVIEW_BUILD_STAMP(name) \
    static const ::quadview::BuildStampEntry g_quadview_build_stamp_##name { #name, __DATE__ " " __TIME__, ::quadview::HEADER_REV }
#endif


// The quality steps the upscaler plugin had (their numbers were its own). [NO PLUGIN fix91 19.11.2026] The mod no longer
// uses the plugin; the steps still size the fovea with a wide view per eye (snap_quality, fit_fovea) and name the ratio
// in the menu.
enum class PluginQuality : int32_t { Performance = 0, Balanced = 1, Quality = 2, UltraPerformance = 3, UltraQuality = 4, Native = 5 };

// The upscale methods the plugin had (Settings::upscaler; nothing reads it since fix91: there is no plugin)
enum class Upscaler : int32_t { DLSS = 0, FSR2 = 1, XESS = 2, FSR3 = 3, FSR4 = 4 };

// What anti-aliases the fovea: the upscaler plugin or the mod's TAA (d3d12::FoveaTAA)
enum class FoveaMode : int32_t { Upscaler = 0, TAA = 1 };

struct QualityRatio {
    PluginQuality quality;
    float ratio;
};

// The plugin sizes each upscaler context for the ratio of its quality level ("max render size" =
// display / ratio). Render sizes BELOW that maximum are fine (dynamic resolution), above it are not.
// So we pick the largest standard ratio that does not exceed the real one.
inline QualityRatio quality_for_ratio(float wanted) {
    static constexpr QualityRatio table[] = {
        {PluginQuality::UltraPerformance, 3.0f},
        {PluginQuality::Performance, 2.0f},
        {PluginQuality::Balanced, 1.7f},
        {PluginQuality::Quality, 1.5f},
        {PluginQuality::Native, 1.0f},
    };

    for (const auto& e : table) {
        if (wanted + 1e-3f >= e.ratio) {
            return e;
        }
    }

    return {PluginQuality::Native, 1.0f};
}

// A standard ratio within `tolerance` (relative) of `wanted`. The upscaler context then runs at exactly its own
// render size, as everywhere else in the mod -- no dynamic resolution. (Test 21.10.2026, wide view only: a shrunken
// copy in the top-left corner and see-through hands with the wide context at 936 px inside a 1402 px maximum.)
inline bool snap_quality(float wanted, float tolerance, QualityRatio& out) {
    static constexpr QualityRatio table[] = {
        {PluginQuality::UltraPerformance, 3.0f},
        {PluginQuality::Performance, 2.0f},
        {PluginQuality::Balanced, 1.7f},
        {PluginQuality::Quality, 1.5f},
        {PluginQuality::Native, 1.0f},
    };

    for (const auto& e : table) {
        if (std::fabs(wanted - e.ratio) <= tolerance * e.ratio) {
            out = e;
            return true;
        }
    }

    return false;
}

inline uint32_t align_near(float v, uint32_t a) {
    const auto n = (uint32_t)std::lround(v / (float)a);
    return (n < 1 ? 1 : n) * a;
}

struct Layout {
    bool valid{false};
    uint32_t display_w{0};   // eye image (upscaled) size
    uint32_t display_h{0};
    uint32_t inset_w{0};     // fovea rectangle == fovea upscaler output size (display pixels, 1:1)
    uint32_t inset_h{0};
    uint32_t render_w{0};    // render size of ALL views (the engine has one render size)
    uint32_t render_h{0};
    // [MONO PERIPHERY 27.10.2026] the single wide view covers both eyes' frusta, so it is wider than one eye's view:
    // wide_x_factor = its width / an eye's width (tangents), 1 when every view is an eye's own. The render size keeps
    // the eye's pixel density, so render_w grows by this factor and so does the fovea rectangle (same density).
    // eye_wide_w/h: size of one eye's share of the wide view (what the reprojection writes, the spatial upscale reads).
    float wide_x_factor{1.0f};
    uint32_t eye_wide_w{0};
    uint32_t eye_wide_h{0};
    float wide_ratio{1.0f};  // display / render
    float fovea_ratio{1.0f}; // inset / render
    PluginQuality wide_quality{PluginQuality::Native};
    PluginQuality fovea_quality{PluginQuality::Native};
};

// fraction    : fovea width/height as a fraction of the eye image (same fraction on both axes)
// fovea_ratio : upscale ratio inside the fovea, 1.0 = rendered at full display density
// wide_x_factor: width of the wide view / width of an eye's view (tangents); 1 unless the wide view is shared
inline Layout compute_layout(uint32_t display_w, uint32_t display_h, float fraction, float fovea_ratio, float wide_x_factor = 1.0f) {
    Layout l{};

    if (display_w < 64 || display_h < 64) {
        return l;
    }

    fraction = std::clamp(fraction, 0.15f, 0.8f);
    fovea_ratio = std::clamp(fovea_ratio, 1.0f, 3.0f);
    wide_x_factor = std::isfinite(wide_x_factor) ? std::clamp(wide_x_factor, 1.0f, 2.0f) : 1.0f;

    l.display_w = display_w;
    l.display_h = display_h;
    l.wide_x_factor = wide_x_factor;
    // [MONO PERIPHERY] the fovea rectangle is wider by the same factor as the render target (same pixel density)
    l.inset_w = (std::min)(display_w, align_near((float)display_w * fraction * wide_x_factor, 8));
    l.inset_h = (std::min)(display_h, align_near((float)display_h * fraction, 8));
    l.render_w = (std::min)(l.inset_w, align_near((float)l.inset_w / fovea_ratio, 8));
    l.render_h = (std::min)(l.inset_h, align_near((float)l.inset_h / fovea_ratio, 8));
    l.fovea_ratio = (float)l.inset_w / (float)l.render_w;
    l.wide_ratio = (float)display_w / (float)l.render_w;
    l.fovea_quality = quality_for_ratio(l.fovea_ratio).quality;
    l.wide_quality = quality_for_ratio(l.wide_ratio).quality;
    l.eye_wide_w = (std::max)(8u, (uint32_t)std::lround((float)l.render_w / wide_x_factor));
    l.eye_wide_h = l.render_h;
    l.valid = true;
    return l;
}

// ---- fitting the layout to the plugin's own render sizes (no dynamic resolution) --------------------------
// Fovea rectangle sized so that a context of quality `q` renders exactly at the render size.
inline void fit_fovea(Layout& l, const QualityRatio& q) {
    if (!l.valid) {
        return;
    }

    l.fovea_quality = q.quality;
    l.inset_w = (std::min)(l.display_w, (uint32_t)std::lround((float)l.render_w * q.ratio));
    l.inset_h = (std::min)(l.display_h, (uint32_t)std::lround((float)l.render_h * q.ratio));
    l.fovea_ratio = (float)l.inset_w / (float)l.render_w;
}

// ---- [FSR DIRECT 06.11.2026] both foveas in one FSR 4 pass ------------------------------------------------------------
// The two fovea views side by side in one input image, with a gap between them that each view fills with its own edge
// column (so beside the inner edges FSR 4 sees what it sees at an image edge); the two results come out side by side the
// same way. FSR 4 maps output x to input x by the ratio of the two whole widths, so each view's part lines up only when
// the gap scales by the views' own ratio: render gap g_r and output gap g_o = g_r * inset_w / render_w, both whole pixels.
// The smallest even g_r of at least min_gap with a whole g_o; not valid (two passes) when none is found up to max_gap.
struct FsrAtlas {
    bool valid{false};
    uint32_t gap_r{0};          // gap in input (render) pixels, even: each view fills half of it
    uint32_t gap_o{0};          // gap in output (fovea) pixels
    uint32_t render_w{0};       // the whole input
    uint32_t render_h{0};
    uint32_t out_w{0};          // the whole output
    uint32_t out_h{0};
    uint32_t right_render_x{0}; // where the right view starts in the input
    uint32_t right_out_x{0};    // ... in the output
};

inline FsrAtlas fsr_atlas(const Layout& l, uint32_t min_gap = 64, uint32_t max_gap = 160) {
    FsrAtlas a{};

    // [SUPERSAMPLE 10.11.2026] also views larger than the fovea (FSR 4 then writes at their size, the mod scales down)
    if (!l.valid || l.render_w == 0 || l.render_h == 0) {
        return a;
    }

    for (uint32_t g = min_gap + (min_gap & 1u); g <= max_gap; g += 2) {
        const uint64_t scaled = (uint64_t)g * l.inset_w;

        if (scaled % l.render_w != 0 || scaled / l.render_w < 16) {
            continue;
        }

        a.gap_r = g;
        a.gap_o = (uint32_t)(scaled / l.render_w);
        a.render_w = 2 * l.render_w + a.gap_r;
        a.render_h = l.render_h;
        a.out_w = 2 * l.inset_w + a.gap_o;
        a.out_h = l.inset_h;
        a.right_render_x = l.render_w + a.gap_r;
        a.right_out_x = l.inset_w + a.gap_o;
        a.valid = true;
        return a;
    }

    return a;
}

// [ONE PASS FIT 06.11.2026] A layout without such a gap would get a pass per eye, and in the dumps of 06.11 the passes per
// eye are darker than the one pass in foliage against the sky (0.94 of their input instead of 0.98, the same inputs);
// with the sliders about half of the sizes have no gap (the presets Balanced and Fast too). This moves the layout to the
// nearest one that has a gap: the render width and, if that is not enough, the fovea width by steps of 8 pixels (a step of
// the fovea width counts twice; the render width never above the fovea width, the fovea never wider than the eye image).
// Heights unchanged. Returns true when the layout was changed.
inline bool fit_atlas(Layout& l, int max_cost = 8) {
    if (!l.valid || fsr_atlas(l).valid) {
        return false;
    }

    for (int cost = 1; cost <= max_cost; ++cost) {
        for (int di = 0; 2 * di <= cost; ++di) {
            const int dr = cost - 2 * di;

            for (int si : {1, -1}) {
                if (di == 0 && si < 0) {
                    continue;
                }

                for (int sr : {1, -1}) {
                    if (dr == 0 && sr < 0) {
                        continue;
                    }

                    const int64_t iw = (int64_t)l.inset_w + 8 * si * di;
                    const int64_t rw = (int64_t)l.render_w + 8 * sr * dr;

                    if (iw < 8 || rw < 8 || iw > (int64_t)l.display_w || rw > iw) {
                        continue;
                    }

                    Layout m = l;
                    m.inset_w = (uint32_t)iw;
                    m.render_w = (uint32_t)rw;

                    if (!fsr_atlas(m).valid) {
                        continue;
                    }

                    m.fovea_ratio = (float)m.inset_w / (float)m.render_w;
                    m.wide_ratio = (float)m.display_w / (float)m.render_w;
                    m.fovea_quality = quality_for_ratio(m.fovea_ratio).quality;
                    m.wide_quality = quality_for_ratio(m.wide_ratio).quality;
                    m.eye_wide_w = (std::max)(8u, (uint32_t)std::lround((float)m.render_w / m.wide_x_factor));
                    l = m;
                    return true;
                }
            }
        }
    }

    return false;
}

// The part of the atlas input one eye's view fills: columns [out_x, out_x + w) of the atlas, read from the view's column
// clamp(x - out_x + shift, 0, view_w - 1) (its own columns, then its edge column into its half of the gap).
struct AtlasFill {
    uint32_t out_x{0};
    uint32_t w{0};
    int32_t shift{0};
};

inline AtlasFill atlas_fill(const FsrAtlas& a, uint32_t view_w, int eye) {
    AtlasFill f{};
    f.w = view_w + a.gap_r / 2;

    if (eye == 0) {
        f.out_x = 0;
        f.shift = 0;
    } else {
        f.out_x = view_w + a.gap_r / 2;
        f.shift = -(int32_t)(a.gap_r / 2);
    }

    return f;
}

// [SUPERSAMPLE 10.11.2026] the views rendered `s` times the fovea's size (Settings::fovea_supersample): the render size
// near inset * s (steps of 8), the width moved by up to 8 steps of 8 so that both foveas fit in one FSR 4 pass (as
// fit_atlas). Returns false (layout unchanged) for s <= 1.
inline bool supersample_layout(Layout& l, float s) {
    if (!l.valid || !(s > 1.001f)) {
        return false;
    }

    s = (std::min)(s, 2.0f);
    Layout m = l;
    m.render_w = align_near((float)l.inset_w * s, 8);
    m.render_h = align_near((float)l.inset_h * s, 8);
    const uint32_t base_w = m.render_w;

    for (int k = 0; k <= 16; ++k) {
        const int64_t step = (k % 2 == 0) ? (int64_t)(k / 2) : -(int64_t)(k / 2 + 1);
        const int64_t rw = (int64_t)base_w + 8 * step;

        if (rw <= (int64_t)l.inset_w) {
            continue;
        }

        Layout t = m;
        t.render_w = (uint32_t)rw;

        if (fsr_atlas(t).valid) {
            m = t;
            break;
        }
    }

    m.fovea_ratio = (float)m.inset_w / (float)m.render_w;
    m.wide_ratio = (float)m.display_w / (float)m.render_w;
    m.fovea_quality = PluginQuality::Native;
    m.wide_quality = PluginQuality::Native;
    m.eye_wide_w = (std::max)(8u, (uint32_t)std::lround((float)m.render_w / m.wide_x_factor));
    l = m;
    return true;
}

// [FSR MODEL 09.11.2026] the scale FSR 4 is asked for with Settings::fsr_model (0 = the layout's own); see there
inline float fsr_model_scale(int model) {
    switch (model) {
    case 1:
        return 1.5f;
    case 2:
        return 1.7f;
    case 3:
        return 2.0f;
    default:
        return 1.0f;
    }
}

// the factor FSR 4's output gets beyond the fovea's size so that output / input reaches that scale (1 = none)
inline float fsr_output_factor(int model, uint32_t render_w, uint32_t inset_w) {
    if (render_w == 0 || inset_w == 0) {
        return 1.0f;
    }

    // (views larger than the fovea, SUPERSAMPLE: own < 1, so FSR 4 writes at least the views' size)
    const float own = (float)inset_w / (float)render_w;
    const float k = fsr_model_scale(model) / own;
    return k > 1.001f ? (std::min)(k, 3.0f) : 1.0f;
}

// The jitter sequence length for a scale n = display / render: AMD's FSR manual, ceil(8 n^2) (at least 8)
inline int32_t jitter_phase_count(uint32_t render_w, uint32_t display_w) {
    if (render_w == 0) {
        return 8;
    }

    const float r = (float)display_w / (float)render_w;
    return (std::max)(8, (int32_t)std::ceil(8.0f * r * r - 1e-4f));
}

// [PERIPHERY SUPER fix80 17.11.2026] Can the periphery's supersampling TAA hand its image of twice the wide view's size
// to the eyes (Settings::periphery_aa 2) with this layout: each eye's periphery then has twice its share of the wide
// view per side, and it must still fit the eye pass's groups of 16 x 16 pixels (d3d12::EyeCompose: at most 12 periphery
// pixels per 17 of the eye image, taken with a margin here), or every pixel of the eye pass reads its 16 taps on its own.
// It does while the eye image is 3 times its share of the wide view per side or more (a periphery denser than that
// gains little from the larger image anyway).
inline bool periphery_2x_fits(const Layout& l) {
    return l.valid && l.display_w > 0 && l.display_h > 0 && (uint64_t)l.eye_wide_w * 2 * 18 <= (uint64_t)l.display_w * 12 &&
           (uint64_t)l.eye_wide_h * 2 * 18 <= (uint64_t)l.display_h * 12;
}

// [SUPER TAA 16.11.2026] The jitter of frame `index` of a sequence of `phases` frames, in pixels within +-0.5: Halton
// (2, 3) minus 0.5, the sequence AMD gives for FSR (the upscaler plugin's GetJitterOffset for FSR). The mod's
// supersampling TAA takes its own: the same in both fovea views (the index is the engine's render frame), and as long
// as its history asks for (jitter_phase_count with the history's width).
inline float halton(uint32_t i, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;

    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }

    return r;
}

inline void halton_jitter(uint32_t index, uint32_t phases, float& x, float& y) {
    const uint32_t k = (index % (phases != 0 ? phases : 1u)) + 1u;
    x = halton(k, 2) - 0.5f;
    y = halton(k, 3) - 0.5f;
}

// Rectangle in eye-image pixels (x right, y DOWN)
struct Rect {
    int32_t x{0};
    int32_t y{0};
    uint32_t w{0};
    uint32_t h{0};
};

// Same rectangle in the NDC of the eye projection (x right, y UP)
struct NdcRect {
    float x0{-1.0f};
    float y0{-1.0f};
    float x1{1.0f};
    float y1{1.0f};
};

// ndc_inset = ndc_wide * scale + offset   (per axis)
struct Affine {
    float sx{1.0f};
    float sy{1.0f};
    float ox{0.0f};
    float oy{0.0f};
};

// Places the fovea rectangle centred on a gaze point (NDC of the eye projection), on even pixel coordinates, fully
// inside the image (near an edge it stops there and the gaze moves off its centre).
inline Rect place_rect(const Layout& l, float ndc_x, float ndc_y) {
    Rect r{};

    if (!l.valid) {
        return r;
    }

    if (!std::isfinite(ndc_x)) ndc_x = 0.0f;
    if (!std::isfinite(ndc_y)) ndc_y = 0.0f;

    const float cx = (std::clamp(ndc_x, -1.0f, 1.0f) * 0.5f + 0.5f) * (float)l.display_w;
    const float cy = (1.0f - (std::clamp(ndc_y, -1.0f, 1.0f) * 0.5f + 0.5f)) * (float)l.display_h;

    int32_t x = (int32_t)std::lround(cx - (float)l.inset_w * 0.5f);
    int32_t y = (int32_t)std::lround(cy - (float)l.inset_h * 0.5f);

    x = std::clamp(x, 0, (int32_t)(l.display_w - l.inset_w));
    y = std::clamp(y, 0, (int32_t)(l.display_h - l.inset_h));

    r.x = x & ~1;
    r.y = y & ~1;
    r.w = l.inset_w;
    r.h = l.inset_h;
    return r;
}

inline NdcRect rect_to_ndc(const Layout& l, const Rect& r) {
    NdcRect n{};

    if (!l.valid || l.display_w == 0 || l.display_h == 0) {
        return n;
    }

    n.x0 = 2.0f * (float)r.x / (float)l.display_w - 1.0f;
    n.x1 = 2.0f * (float)(r.x + (int32_t)r.w) / (float)l.display_w - 1.0f;
    n.y1 = 1.0f - 2.0f * (float)r.y / (float)l.display_h;
    n.y0 = 1.0f - 2.0f * (float)(r.y + (int32_t)r.h) / (float)l.display_h;
    return n;
}

// Sub-frustum: maps the NDC rectangle n of the wide projection onto the full [-1,1] range.
// Applied to a projection matrix P as  P_inset = M * P  with (column-major / glm)
//   M[0][0] = sx, M[1][1] = sy, M[3][0] = ox, M[3][1] = oy
// i.e. clip'.x = sx * clip.x + ox * clip.w  ->  ndc'.x = sx * ndc.x + ox.
inline Affine sub_frustum(const NdcRect& n) {
    Affine a{};
    const float hx = (n.x1 - n.x0) * 0.5f;
    const float hy = (n.y1 - n.y0) * 0.5f;

    if (hx <= 1e-4f || hy <= 1e-4f) {
        return a;
    }

    const float cx = (n.x0 + n.x1) * 0.5f;
    const float cy = (n.y0 + n.y1) * 0.5f;

    a.sx = 1.0f / hx;
    a.sy = 1.0f / hy;
    a.ox = -cx / hx;
    a.oy = -cy / hy;
    return a;
}

// Applies an Affine to a 4x4 column-major matrix stored as m[col][row] (glm layout): result = M * P.
// Generic over the matrix type so this header stays free of glm (tests use plain arrays).
template <typename Mat>
inline void apply_sub_frustum(Mat& p, const Affine& a) {
    for (int col = 0; col < 4; ++col) {
        const float x = p[col][0];
        const float y = p[col][1];
        const float w = p[col][3];
        p[col][0] = a.sx * x + a.ox * w;
        p[col][1] = a.sy * y + a.oy * w;
    }
}

// [FOVEA FOV TEST 06.11.2026 evening] The field of view (degrees) of the narrowest frustum around a camera's axis that holds the
// rectangle r of the projection p (NDC; p a 4x4 perspective matrix m[col][row], glm layout) with the tangents enlarged by margin:
// the vertical one for that aspect (width / height) when vertical is set, otherwise the horizontal one. Never under 20 degrees
// nor over max_fov; max_fov when p is not a perspective projection. A tangent t gives ndc = m00 * t / |m23| + m20 / m23.
template <typename Mat>
inline float cull_fov(const Mat& p, const NdcRect& r, float aspect, bool vertical, float max_fov, float margin = 1.15f) {
    if (p[0][0] == 0.0f || p[1][1] == 0.0f || p[2][3] == 0.0f || !(max_fov > 20.0f)) {
        return max_fov;
    }

    const float k = std::abs(p[2][3]);
    const auto tan_x = [&](float ndc) { return std::abs((ndc - p[2][0] / p[2][3]) * k / p[0][0]); };
    const auto tan_y = [&](float ndc) { return std::abs((ndc - p[2][1] / p[2][3]) * k / p[1][1]); };
    const float tx = (std::max)(tan_x(r.x0), tan_x(r.x1)) * margin;
    const float ty = (std::max)(tan_y(r.y0), tan_y(r.y1)) * margin;
    const float a = aspect > 0.1f && std::isfinite(aspect) ? aspect : 1.0f;
    const float half_tan = vertical ? (std::max)(ty, tx / a) : (std::max)(tx, ty * a);
    const float fov = 2.0f * std::atan(half_tan) * 57.29577951f;
    return std::isfinite(fov) ? std::clamp(fov, 20.0f, max_fov) : max_fov;
}

// =================================================================================================
// Shared state
// =================================================================================================
// [FOVEA TAA 28.10.2026] the settings of the mod's TAA in the fovea (see d3d12::FoveaTAA, same meaning and defaults)
struct FoveaTaaSettings {
    float feedback{0.90f};      // weight of the settled history (higher = steadier, softer in motion)
    float clip_gamma{1.0f};     // neighbourhood clip: mean +- gamma * sigma (lower = less ghosting, more shimmer)
    float filter_radius{0.40f}; // rebuild of the current frame: Gaussian sigma in pixels (0 = the own sample only)
    float new_softness{0.5f};   // pixels without history: their rebuild widens towards sigma 1 px
    float motion_reduce{0.25f}; // history weight lowered by up to this fraction on fast motion
    float depth_reject{0.10f};  // hidden in the last frame: something nearer by more than this fraction (0 = off)
    float sharpen{0.30f};       // RCAS on the result (0 = off)
    bool dilate{true};          // motion vector of the nearest surface in 3x3
    bool catmull_rom{true};     // history read with Catmull-Rom instead of bilinear
    bool luma_weight{true};     // blend weighted by 1 / (1 + luma)
    bool fast_converge{true};   // a fresh history averages its first frames (1/n)
};

// [SUPER TAA 16.11.2026] the settings of the mod's supersampling TAA in the fovea (see d3d12::SuperTAA, same meaning and
// defaults)
struct SuperTaaSettings {
    int scale{2};                  // history texels per fovea pixel and axis: 1 or 2
    float stability{5.0f};         // weight a settled history holds (higher = steadier, slower to follow)
    float history_sharpness{0.5f}; // history resampling sharper than Catmull-Rom by this much
    float sharpen{0.30f};          // RCAS on the result (0 = off)
    // [fix77 16.11.2026] the history follows what changes:
    bool follow{true};             // the history keeps little weight where the picture changed (see d3d12::SuperTAA)
    float follow_threshold{1.0f};  // ... what counts as a change: the test's bands times this (lower = sooner)
    float follow_memory{0.5f};     // ... the weight it keeps there
    // [fix78 16.11.2026] fix76's protection of what one frame's samples cannot show (thin bright edges, stochastic
    // shadows) is back, released where the history is stale (a change seen, an edge that moves against what is behind it)
    bool protect{true};
    float protect_range{1.5f};     // ... the clip box wider by this times the samples' scatter
    float protect_spark{2.5f};     // ... the frame's sample within this times the scatter of the history (0 = off)
    float protect_memory{1.5f};    // ... the history's weight limit times this
    // [fix79 16.11.2026] what a thing leaves that moves without its motion vectors saying so (a fly) is not protected
    bool arrive{true};
    float filter_scale{1.0f};      // rebuild of the current frame: Gaussian radius times this
    float clip_gamma{1.0f};        // neighbourhood clip: mean +- gamma * sigma
    float motion_reduce{0.25f};    // history weight lowered by up to this fraction on fast motion
    float depth_reject{0.10f};     // hidden in the last frame: something nearer by more than this fraction (0 = off)
    bool luma_weight{true};        // blend weighted by 1 / (1 + luma)
    bool lighter{false};           // the history pass with fewer reads (nearest sample, range by the green channel)
    bool lanczos{false};           // scale 2: Lanczos 2 over 8 x 8 texels instead of the mean of 2 x 2
    // ---- debug (not saved) ----
    int debug_view{0};             // d3d12::SuperTAA::Params::debug_view
    bool compare{false};           // the mod's TAA (d3d12::FoveaTAA) runs beside it: its image for the split and the capture
    bool split{false};             // the left half of the fovea from the mod's TAA, a yellow line between
};

// ---- [UI MOTION fix125 22.11.2026] the interface's own motion on the screen ------------------------------------------------
// The engine's motion vectors hold the motion of the scene behind the interface: a panel in front of the game camera
// stands still on the screen while the player walks or turns and the scene moves under it, a panel in front of an empty
// background moves against it when the head moves sideways (RE4 capture of 19.11.2026: the text of the item screen
// 0.2 - 1 pixel a frame off its vectors), the HUD at the weapon moves with the hand. A temporal pass that fetches the
// interface's history along the scene's vectors smears it (trails, faint text). An interface element is a flat thing
// with a known place: the world matrix it was drawn with (GuiAnchor::mat), in this frame and in the frame before. With
// a view's world -> clip matrices of the two frames (without jitter) every point of its plane has an exact place in the
// frame before: a plane-to-plane map (homography) of the view's UV.
//
// ui_plane_motion: that map as D = H / H[2][2] - I (rows; UV 0..1, y down). The UV of the frame before of the point shown
// at (u, v):  e = D[6] u + D[7] v + D[8];  prev = (u, v) + ((D[0] u + D[1] v + D[2], D[3] u + D[4] v + D[5]) - (u, v) e) / (1 + e)
// (the small differences are worked with directly: no loss of digits in float). Matrices column-major like glm.
// False: the plane goes through the camera, or the numbers are not finite.
// (the views' matrices as doubles: projection times view worked in double keeps the digits of a camera far from the
// world's origin)
inline bool ui_plane_motion(const double* vp_cur16, const double* vp_old16, const float* m_cur16, const float* m_old16, float out_d[9]) {
    // clip (x, y, w) of the plane's local (x, y, 1): rows 0, 1, 3 and columns 0, 1, 3 of vp * m
    const auto plane = [](const double* vp, const float* m, double b[9]) {
        static constexpr int RC[3]{0, 1, 3};

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double v = 0.0;

                for (int k = 0; k < 4; ++k) {
                    v += vp[k * 4 + RC[i]] * (double)m[RC[j] * 4 + k];
                }

                b[i * 3 + j] = v;
            }
        }
    };
    const auto mul = [](const double* a, const double* b, double* c) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                c[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
            }
        }
    };

    double bc[9];
    double bo[9];
    plane(vp_cur16, m_cur16, bc);
    plane(vp_old16, m_old16, bo);

    const double det = bc[0] * (bc[4] * bc[8] - bc[5] * bc[7]) - bc[1] * (bc[3] * bc[8] - bc[5] * bc[6]) + bc[2] * (bc[3] * bc[7] - bc[4] * bc[6]);
    double scale = 0.0;

    for (int i = 0; i < 9; ++i) {
        scale = (std::max)(scale, std::abs(bc[i]));
    }

    if (!(scale > 0.0) || !std::isfinite(det) || !(std::abs(det) > 1e-9 * scale * scale * scale)) {
        return false;
    }

    const double inv[9]{
        (bc[4] * bc[8] - bc[5] * bc[7]) / det, (bc[2] * bc[7] - bc[1] * bc[8]) / det, (bc[1] * bc[5] - bc[2] * bc[4]) / det,
        (bc[5] * bc[6] - bc[3] * bc[8]) / det, (bc[0] * bc[8] - bc[2] * bc[6]) / det, (bc[2] * bc[3] - bc[0] * bc[5]) / det,
        (bc[3] * bc[7] - bc[4] * bc[6]) / det, (bc[1] * bc[6] - bc[0] * bc[7]) / det, (bc[0] * bc[4] - bc[1] * bc[3]) / det,
    };
    // clip -> UV (u = (x + w) / 2, v = (w - y) / 2) and back
    static constexpr double A[9]{0.5, 0.0, 0.5, 0.0, -0.5, 0.5, 0.0, 0.0, 1.0};
    static constexpr double AI[9]{2.0, 0.0, -1.0, 0.0, -2.0, 1.0, 0.0, 0.0, 1.0};
    double t0[9];
    double t1[9];
    double h[9];
    mul(bo, inv, t0);
    mul(A, t0, t1);
    mul(t1, AI, h);

    if (!std::isfinite(h[8]) || !(std::abs(h[8]) > 1e-12)) {
        return false;
    }

    for (int i = 0; i < 9; ++i) {
        const double d = h[i] / h[8] - (i == 0 || i == 4 || i == 8 ? 1.0 : 0.0);

        if (!std::isfinite(d)) {
            return false;
        }

        out_d[i] = (float)d;
    }

    return true;
}

inline bool ui_plane_motion(const float* vp_cur16, const float* vp_old16, const float* m_cur16, const float* m_old16, float out_d[9]) {
    double c[16];
    double o[16];

    for (int i = 0; i < 16; ++i) {
        c[i] = (double)vp_cur16[i];
        o[i] = (double)vp_old16[i];
    }

    return ui_plane_motion(c, o, m_cur16, m_old16, out_d);
}

// projection * view in double (column-major 4x4 floats in, doubles out)
inline void ui_view_proj(const float* proj16, const float* view16, double out16[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double v = 0.0;

            for (int k = 0; k < 4; ++k) {
                v += (double)proj16[k * 4 + row] * (double)view16[col * 4 + k];
            }

            out16[col * 4 + row] = v;
        }
    }
}

// the motion (UV of the frame before minus UV now) at (u, v), as the shaders work it (hlsl: um_motion)
inline void ui_motion_at(const float d[9], float u, float v, float& du, float& dv) {
    const float e = d[6] * u + d[7] * v + d[8];
    const float den = 1.0f + e;
    du = (d[0] * u + d[1] * v + d[2] - u * e) / den;
    dv = (d[3] * u + d[4] * v + d[5] - v * e) / den;
}

struct Settings {
    float fraction{0.40f};        // fovea size
    float fovea_ratio{1.20f};     // upscale ratio inside the fovea
    Upscaler upscaler{Upscaler::FSR4}; // the plugin's method (one for all instances, see the top of this file)
    float feather{0.10f};         // blend width, fraction of the fovea size
    float deadzone{0.015f};       // the fovea only moves when the gaze moved more than this (fraction of image width)
    bool debug_border{false};
    // debug: 0 = normal, 1 = periphery only (no fovea), 4 = mark the reprojection holes, 5 = tint the interface sources
    int debug_view{0};
    // light TAA of the periphery: weight of the history (higher = calmer but more ghosting)
    float taa_feedback{0.90f};
    // [PERIPHERY SUPER fix80 17.11.2026] what anti-aliases the shared wide view (the periphery): 0 = the light TAA
    // (d3d12::LightTAA, as before), 1 = the mod's supersampling TAA (an instance of d3d12::SuperTAA of its own, with the
    // fovea's settings) writing an image of the wide view's size, 2 = the same writing an image of twice that size, which
    // the reprojection brings to each eye with 2 x 2 pixels per pixel of the wide view (d3d12::MonoPeriphery::Params::ss).
    // Only with the shared periphery (mono_periphery); changes need no re-init.
    // [CALM fix100 20.11.2026] 3 = the calm light TAA: the light TAA's pass with the protection of what one frame's samples
    // cannot show (d3d12::LightTAA::Params::calm, history weight CALM_FEEDBACK), an image of the wide view's size. One
    // pass where the supersampling TAA has three (its cost: the GPU pass test, 43 against 36); in the reference model (tests/supertaa/periphery/calm.py) as steady as the
    // supersampling TAA with the image of the wide view's size, with less detail. The wide view keeps the supersampling
    // TAA's jitter sequence.
    int periphery_aa{2};
    static constexpr float CALM_FEEDBACK = 0.93f;
    // [GUARD fix101 20.11.2026] set by the GPU guard (FoveatedRendering::gpu_guard_watch). periphery_warm_super: the
    // supersampling TAA (image of the wide view's size) runs behind the calm light TAA, its image not shown: the frames
    // before the step back to it, so that its history is settled when it takes over. guard_warm: everything the guard's
    // steps switch to is made at the set-up (no pass compiled, no image made in the frame of a step).
    bool periphery_warm_super{false};
    bool guard_warm{false};
    // [PERIPHERY fix82 17.11.2026] the stability of the periphery's supersampling TAA (the accumulated weight a settled
    // history holds; the fovea's is super_taa.stability, 5). RE7 captures of 17.11.2026 16:0x through the reference model
    // (tests/supertaa/periphery_stability.py; capture 03, the head still): with 10 the anti-aliased wide view's flicker
    // is 12 % lower at its own texels and 26 % / 29 % lower over one / two pixels of the wide view (what the periphery of
    // the eye sees), its detail the same (0.0213 against 0.0215); 16 adds little (30 % / 33 %). With the head moving
    // (capture 04) nothing changes: the motion limits the weight. A quarter of the eye's density leaves more for time to
    // even out than the fovea's, and the periphery of the eye notices flicker before softness.
    float periphery_stability{10.0f};
    // [PERIPHERY fix83 17.11.2026] the periphery's own history filter: how much sharper than Catmull-Rom the supersampling
    // TAA reads its history (the fovea's is super_taa.history_sharpness, 0.5). RE7 captures of 17.11.2026 19:2x (fix82,
    // periphery beside the fovea; tests/supertaa/periphery_history.py): with 0.5 the periphery's image of twice the wide
    // view's size was full of wavy outlines that crawl from frame to frame (the picture is read from its history at
    // almost every pixel in every frame, the head never stands still, and a filter sharper than Catmull-Rom sharpens what
    // it read the frame before again); the eye pass's enlargement and sharpening made them plainer. With 0 (plain
    // Catmull-Rom) they are gone and the flicker of the anti-aliased wide view is 38 % / 25 % lower at its own texels /
    // over half a pixel of the wide view on capture 02 (stairs), 39 % / 24 % on capture 04, 26 % / 19 % on capture 01;
    // detail 0.0220 against 0.0265 (capture 02).
    float periphery_history_sharpness{0.0f};
    // [OUTLINES fix84 18.11.2026] the periphery's reprojection with each pixel on its own surface and the outlines to a
    // fraction of a texel (MonoPeriphery::Params::soft_outlines; with the stable depth and the image of twice the size).
    // tests/supertaa/periphery/outline_test.py (a near thing before a wall, moving 0.02 texels of the wide view per frame,
    // the place of its outline against the truth, image pixels of 2 eye pixels): fix83 0.74 rms from frame to frame (a saw
    // tooth of 5 frames: the stable depth's fault, see its pass), with the stable depth mended 0.094, with this 0.053;
    // beside the gap 0.24 / 0.085 / 0.080 and the outline's mean place there +1.2 -> -0.1 pixels.
    bool soft_outlines{true};
    // [MENUS fix86 18.11.2026] the wide view's interface for the periphery collected over the frames (d3d12::UiSteady;
    // with steady_interface and the single eye pass): the menu's lines and letters outside the fovea whole and still
    // instead of other pieces every frame. Off: the frame's own images read where the jitter put the point, as fix81 - 85
    // (the debug tools' checkbox, for comparison; not saved).
    bool ui_steady{true};
    // [fix94 19.11.2026] that pass on the blocks around the interface only (d3d12::UiSteady::Params::only_around): RE4 test
    // of 19.11.2026, 0.32 ms of GPU time per frame for the whole image with a small HUD in the picture. On unless
    // switched off (QuadViewInterfaceOnlyAround; off in fix94): a new piece of interface shows in the periphery at once instead of
    // fading in over about a quarter of a second; the image is otherwise the same (to the bit with the head still).
    bool ui_steady_around{true}; // ([fix95] the option's default is on)
    // [PERIPHERY DUMP fix84 18.11.2026] dumps hold the periphery's chain whole and as stored (TemporalUpscaler: the wide
    // view's colour, depth, motion, the stable depth, the anti-aliased image, each eye's reprojected periphery, the left
    // eye's finished image): large files, for analysis off the game
    bool dump_periphery_full{false};
    // what anti-aliases the fovea; changes need no re-init
    FoveaMode fovea_mode{FoveaMode::Upscaler};
    FoveaTaaSettings fovea_taa{};
    // [SUPER TAA 16.11.2026] with fovea_mode TAA: the mod's supersampling TAA (d3d12::SuperTAA) instead of d3d12::FoveaTAA.
    // Changes need no re-init (its history's size follows the scale by itself).
    bool fovea_super{false};
    SuperTaaSettings super_taa{};
    // [FOVEA LINEAR 05.11.2026] the upscaler plugin in the fovea gets linear light (decoded before it, encoded after it,
    // d3d12::FoveaLinear) and its result is sharpened with RCAS by this amount (0 = off); no re-init
    bool upscaler_linear{true};
    float upscaler_sharpen{0.30f};
    // [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod itself through AMD's API (d3d12::FsrDirect), recorded
    // into the mod's own command list right after the pre-fovea work, instead of through the upscaler plugin (a list and a
    // submit of its own; log of 06.11: 0.12 ms of CPU per frame in its calls, up to 1.5 ms while the GPU is behind).
    // fsr_one_pass: both foveas in one FSR 4 pass (FsrAtlas: one fixed cost instead of two, GPU work in bigger blocks);
    // off = one pass per eye. fsr_auto_exposure: FSR's own exposure (AMD's recommendation); off = exposure 1 (the mod's
    // input is linear light in 0..1). Each needs a re-init. Only with FSR 4 and the shared periphery; otherwise the plugin.
    bool fsr_direct{true};
    bool fsr_one_pass{true};
    bool fsr_auto_exposure{true};
    // [FSR MODEL 09.11.2026] FSR 4 picks its network by the scale it is asked for (output / input; AMD: native AA 1.0,
    // quality 1.5, balanced 1.7, performance 2.0); AMD's API has no other way to choose it. 0 = the scale of the layout
    // (fovea upscale ratio 1.0: the native AA network). 1..3 = at least the quality / balanced / performance scale: FSR 4
    // writes a larger image (the fovea's size times the missing factor) and the mod scales it down to the fovea's size
    // (area filter, linear light), so its result is also supersampled. GPU time of FSR 4 grows with its output's pixels.
    // Needs a re-init.
    int fsr_model{0};
    // [SUPERSAMPLE 10.11.2026] FSR 4 by the mod only: the views rendered this much larger than the fovea's size in the eye
    // image (1 = the eye image's density); FSR 4 works at that size, the mod scales its result down to the fovea. More
    // samples for detail thinner than a pixel of the eye image (bars, rails, highlights), which FSR 4 alone cannot
    // rebuild (RE2 dumps of 10.11.2026). One render size for all views: the wide view grows the same. Needs a re-init.
    float fovea_supersample{1.0f};
    // [BRIGHTNESS 23.10.2026] the fovea camera has its own automatic exposure, adapted to the small part of the scene it
    // sees (dumps: up to ~2x brighter than the same area of the periphery). The composite scales the fovea by one overall
    // gain per eye to the mean brightness of the same area of the periphery; nothing of the periphery's image goes in.
    bool brightness_match{true};
    // [FOVEA TONE 01.11.2026] with the exposure match: instead of one gain for every brightness, a gain per brightness
    // (64 steps) that brings the fovea's distribution of brightness (blurred) to the periphery's in the same area. With
    // one gain the darker and brighter parts kept a visible difference at the edge (the two cameras' exposures differ, the
    // tone mapping after them is not linear); off = one gain as before.
    bool tone_match{true};
    // [LIGHT 01.11.2026] the periphery's large-scale light added to the fovea: the difference of the two images over blocks of
    // 16 pixels, blurred over 48 pixels (the cameras' fog, light and tone curve differ by place: the table cannot follow
    // that). Only a smooth light: the fovea's detail and sharpness stay as they are. Off = only the gain / table.
    bool periphery_light{true};
    // [LIGHT 02.11.2026] the blur of the large-scale light in pixels (32 = follows smaller areas, 48 = as on 01.11)
    float light_px{48.0f};
    // [GLOW 13.11.2026] (debug test) the large-scale light's limit (part of the level, 0 = built-in 0.35) and only adding light
    float light_limit{0.0f};
    bool light_add_only{false};
    bool light_auto{false}; // [AUTO LIGHT 14.11.2026] (debug test) see FoveaComposite::Params::light_auto
    // [MONO PERIPHERY 27.10.2026] the periphery of both eyes from ONE wide view rendered between the eyes and reprojected
    // to each eye with its depth (three scene layers); off = one wide view per eye (four), for comparison
    bool mono_periphery{true};
    // [HOLES 02.11.2026] the gaps of the shared periphery beside near things (arms, hands, weapon: background no view saw)
    // filled with the background beside them moved over by the gap's width, which stays with the background when the arm
    // or the head moves, and without the near thing's colour at the gap's edge; off = mirrored at the edge (before)
    bool hole_fill{true};
    // [STABLE DEPTH fix81 17.11.2026] the reprojection of the shared periphery gets the wide view's distances steadied over
    // time (d3d12::StableDepth) instead of the frame's own, jittered depth: the outlines of near things no longer jump from
    // frame to frame; off = the frame's depth (fix80), for comparison
    bool stable_depth{true};
    // [LIGHTS fix85 18.11.2026] the shared wide view's scene layer is drawn before the fovea views' (its priority is put
    // below theirs, VR::on_pre_scene_layer_update): lights the engine computes for the frame with the first view drawn
    // (RE7: the room at the top of the stairs) no longer change with the place of the fovea, which the wide view showed
    // as flicker whenever the eyes moved. Light flicker tests of 18.11.2026 (fix84, three runs): at the hot spot
    // 93.9 -> 15.5, 131.3 -> 16.2, 306.5 -> 34.6; off = the fovea views first (the engine's order), for comparison
    bool wide_first{true};
    // [MENUS fix81 17.11.2026] the game's menus and interface in the eye image: read where the frame's jitter put them (they
    // shook by up to half a pixel of their view every frame).
    // [MENUS fix82 17.11.2026] With the mod's TAA in the fovea the fovea views keep their interface and the anti-aliasing
    // takes it like the scene (TemporalUpscaler's ui_inside; fix81's light layer and matte let the hidden scene through
    // what the menus lit and are gone). Off = fix80, for comparison.
    bool steady_interface{true};
    // [HOLE BAND 13.11.2026] in the fovea's blend band the gaps of the shared periphery are covered by the fovea (which saw
    // them) instead of being blended with it; off only for comparison (debug build)
    bool hole_band{true};
    // [MENU BACKDROP 15.11.2026] while a game menu is open: inside the fovea, where its view still shows the menu's dark
    // backdrop past the edge the wide view clips it at (the fovea far darker than the periphery over a large area), the
    // periphery's scene instead (RE7 dumps of 15.11.2026: the inventory's backdrop over the whole height of the fovea, cut
    // off at its top and bottom edge: lighter edges that move with the gaze). Only where the fovea views are cleaned of
    // their interface (FSR 4 in the fovea, or the steady interface off).
    bool menu_backdrop{false};
    // [BACKDROP fix83 17.11.2026] with the interface inside the fovea image (TemporalUpscaler's ui_inside): a backdrop the
    // fovea view draws where the wide view draws none (or a lighter one) is taken back out of the fovea's own pixel, by
    // the fovea view's two interface copies (EyeCompose::Params::fovea_backdrop). Not tied to the game menus the mod knows
    // by name: the save menu's panel is wider in the fovea view than in the wide view too.
    // [fix84 18.11.2026] off: in the game (17.11.2026 22:xx) the menus looked better without it. The debug menu's switch
    // stays for comparisons.
    bool fovea_backdrop{false};
    // [ITEM BEAMS 03.11.2026] the green beams over items (a world element of the game) taken from the wide view to each eye
    // at their item's depth; off = at the interface panels' depth like everything else (before 03.11)
    bool item_beams{true};
    // [GPU 29.10.2026] the left eye copied into the desktop window every frame (off: the window no longer shows the
    // headset image; the flatscreen canvas keeps it on)
    bool desktop_mirror{true};
    // [DIRECT 8 BIT 30.10.2026] the eye pass writes the 8-bit headset textures itself (the eye image's 10-bit rounding
    // reproduced in it) instead of a 10-bit eye image the submit then converts: one pass less. Only for the eyes whose
    // image nothing else reads in 10 bits (the desktop window copies the left eye's).
    bool direct_8bit{true};
    // [GPU 29.10.2026] debug (not saved): the shared periphery's eye images with the three separate passes (WideUpscale,
    // FoveaComposite, MonoUi) instead of the single eye pass, to compare image and GPU time
    bool separate_eye_passes{false};
    // [GPU TEST 29.10.2026] debug (not saved): variants for the GPU pass test, each one part switched back or off to see
    // what it costs. The first three give the same image; the last two change it (measurement only).
    bool test_original_reprojection{false}; // the periphery reprojection as one pass (no faster search, no second pass)
    bool test_no_tile_skip{false};          // the eye pass tests the wide view's interface on every pixel of every tile
    bool test_tile_in_pass{false};          // [TILE PASS fix105] the tiles' test inside the eye pass, as before fix105
    bool test_tile_none{false};             // [fix106] the tile pass marks no tile: what the interface costs in the marked tiles
    uint32_t test_ui_calls{0};              // [fix108] d3d12::EyeCompose::Params::ui_calls
    bool test_prepare{false};               // [fix108] the GPU test counts down or runs (EyeCompose::Params::prepare_tests)
    bool test_one_pass{false};              // [fix111] the eye image in one pass with the interface's calls, as fix110
    bool test_first_skips{false};           // [fix112, fix115] EyeCompose::Params::first_skips_marked (the chain of fix113)
    uint32_t test_marked_text{2};           // [fix115] EyeCompose::Params::marked_test (2: the text in use)
    bool test_marked_over{true};            // [fix122] EyeCompose::Params::marked_over (the HUD over the stored pixels; false: as fix120)
    // [UI MOTION fix125 22.11.2026] The temporal passes fetch the interface's history along the interface's own motion
    // (ui_plane_motion) instead of the scene's motion vectors: the fovea's supersampling TAA where the interface has
    // detail of its own, the periphery's UiSteady everywhere. Off: as fix124.
    bool ui_motion{false};
    bool test_no_ui_motion{false};          // [fix125] GPU test: the passes without the interface's own motion (fix124)
    bool test_spans_ends_only{false};       // [fix119] d3d12::GpuSpans::set_ends_only (no timestamps between the mod's passes)
    bool test_clean_no_map{false};          // [fix118] the wide view's cleaning without the map's whole-frame texel (as fix117)
    bool test_marked_small{false};          // [fix117] EyeCompose::Params::marked_small (the marked tiles' pass in groups of 8 x 8)
    int test_ui_copies{0};                  // [fix116] 1 = the fovea views' interface copies are not made (State::test_ui_copies)
    bool test_ui_idle{false};               // [fix116] the output curve is not measured and the wide view not cleaned
    bool test_no_gui_map{false};            // the eye pass without the GUI map (the interface mask computed everywhere)
    bool test_no_interface{false};          // the eye pass without the interface
    bool test_no_post{false};               // the eye pass without the post pass (sharpness, saturation, brightness, contrast)
    bool test_no_post_sharpen{false};       // [GPU 07.11.2026] the eye pass without the post pass's sharpening only (image changed)
    bool test_round2_passes{false};         // round 2's passes where round 3 replaced them (same image, GPU time only)
    // [GPU 30.10.2026] the eye pass's test variants (same image): 0 = current, 1 = the exposure gain made in every group,
    // 2 = groups of 16 x 8 pixels, 3 = 8 x 8, 4 = 32 x 16, 5 = 32 x 32 (the current), 6 = 16 x 16 (before 31.10),
    // 7 = 30 x 30 pixels on 32 x 32 threads, 8 = before the ALU reductions of 31.10 evening, 9 = those without the branches,
    // 10 = the build of 31.10 22:51, 11 = the build of the 01.11 11:33 test (before the masks on the float3 places)
    // (see d3d12::EyeCompose::Params::Variant)
    uint32_t test_eye_variant{0};
    // [GPU 31.10.2026] the tiled TAA passes with groups of 32 x 32 pixels (same result): the fovea TAA's, the wide view's
    bool test_fovea_taa_32{false};
    bool test_light_taa_32{false};
    // [GPU 31.10.2026 night] the reprojection's first pass reading every block minimum from the texture (before: the
    // group's block minima in shared memory)
    bool test_bmin_texture{false};
    // [GPU 01.11.2026] the TAA passes (wide view, fovea accumulate and resolve) reading their halo in two rounds, each
    // awaited (before: both rounds' reads issued at once)
    bool test_halo_serial{false};
    // [ONE LIST 01.11.2026] test (same image): the pre-fovea work in a list of its own also with the mod's TAA in the
    // fovea (before: one list since 01.11 afternoon)
    bool test_separate_pre{false};
    // [VIEW DIFFERENCE 01.11.2026] measure how different the fovea and the periphery are (blocks of 16 px, see State::view_diff)
    bool view_metric{false};
};

// [CPU 03.11.2026] A counter that many threads add to hundreds of times per frame (the camera hooks: ~1000 calls per frame
// in fights, from the engine's job threads): one cache line per shard, a thread always adds to its own; reading sums them.
// One atomic shared by all threads made every call wait for that cache line (and the lines next to it).
struct ShardedCounter {
    static constexpr size_t SHARDS = 16;

    struct alignas(64) Shard {
        std::atomic<uint64_t> v{0};
    };

    std::array<Shard, SHARDS> shards{};

    static size_t index() {
        static std::atomic<size_t> next{0};
        thread_local const size_t i = next.fetch_add(1, std::memory_order_relaxed) % SHARDS;
        return i;
    }

    uint64_t fetch_add(uint64_t n, std::memory_order o = std::memory_order_relaxed) { return shards[index()].v.fetch_add(n, o); }

    uint64_t load(std::memory_order o = std::memory_order_relaxed) const {
        uint64_t sum = 0;

        for (const auto& sh : shards) {
            sum += sh.v.load(o);
        }

        return sum;
    }

    ShardedCounter& operator++() {
        fetch_add(1);
        return *this;
    }
};

// [RELEASE SWITCH fix141 23.11.2026] The counters below that only the debug log reads (calls of the camera hooks, scene
// layers seen, passes run ...): in the release a stand-in that counts nothing, so every increment compiles to nothing
// (checked: the release code of RE2, RE4 and RE7 never reads them). In the debug build they are what they were.
struct NoCount {
    constexpr NoCount(uint64_t = 0) noexcept {}
    template <class... A> constexpr uint64_t fetch_add(A&&...) noexcept { return 0; }
    template <class... A> constexpr uint64_t load(A&&...) const noexcept { return 0; }
    template <class... A> constexpr void store(A&&...) noexcept {}
    template <class... A> constexpr uint64_t exchange(A&&...) noexcept { return 0; }
    template <class C, class... A> constexpr bool compare_exchange_weak(C&, A&&...) noexcept { return true; }
    constexpr NoCount& operator++() noexcept { return *this; }
    constexpr uint64_t operator++(int) noexcept { return 0; }
    constexpr NoCount& operator+=(uint64_t) noexcept { return *this; }
};
#if REVR_RELEASE
using DiagCounter = NoCount;
template <class T> using DiagAtomic = NoCount;
#else
using DiagCounter = ShardedCounter;
template <class T> using DiagAtomic = std::atomic<T>;
#endif

// [MONO UI 27.10.2026] A GUI element as the VR mod placed it this frame (world position of its view). The GUI is built
// once per frame and drawn in every scene layer with that layer's matrices, so the shared wide view has it from the
// centre between the eyes and the fovea views with their narrow frusta. The interface of each eye is put together from
// those two (see d3d12::MonoUi); to move the wide view's interface to an eye it needs its depth: an element anchored at
// something in the world (item icons) is at that thing's depth, the interface panels at the 2D UI distance.
struct GuiAnchor {
    // [ITEM BEAMS 03.11.2026] what the element is
    enum Kind : uint8_t {
        PANEL = 0,  // placed by the VR mod (panel, or at something in the world: radius_tan / at_target say which)
        BEAM = 1,   // the green beam over an item (Gui_ui2130): a world element of the game, at its item
        WORLD = 2,  // another world element of the game (only listed in the log)
    };

    float pos[3]{};
    float radius_tan{0.0f}; // > 0: anchored element, its pixels lie within this angle (tangent) of its anchor
    // [ANCHOR OWNERSHIP 27.10.2026] placed at something in the world (item icon): no other depth may show its pixels
    bool at_target{false};
    uint8_t kind{PANEL};
    uint64_t id{0};      // [ANCHOR HOLD 03.11.2026] the element's game object: the same element in later frames
    char name[24]{};     // the game object's name (log)
    // [UI MOTION fix125 22.11.2026] the world matrix the element was drawn with (column-major like glm; the element's
    // content lies in its local x / y plane): with the same element's matrix of the frame before, the motion of its
    // pixels on the screen (ui_plane_motion)
    float mat[16]{};
    bool has_mat{false};

    // an element that takes interface pixels to its own depth
    bool claims() const { return kind == BEAM || (kind == PANEL && radius_tan > 0.0f); }
};

// [MONO PERIPHERY 27.10.2026] the frusta the reprojection works with: tangents of the half angles (left, right, bottom,
// top) of each eye's projection and of the wide view that covers both, plus each eye's position along the view x axis
// relative to the wide view's camera (metres). Written by VR (it owns projections and eye transforms, which the scope
// zoom changes), read by the upscaler for the reprojection shader.
struct Frusta {
    bool valid{false};
    float eye_l[2]{};
    float eye_r[2]{};
    float eye_b[2]{};
    float eye_t[2]{};
    float wide_l{0.0f};
    float wide_r{0.0f};
    float wide_b{0.0f};
    float wide_t{0.0f};
    float eye_x[2]{};
};

// One frame of a debug dump: where its images go.
struct DumpFrame {
    uint64_t serial{0};    // dump number (one per button press)
    int index{0};          // frame inside the dump (0, 1, ...)
    std::filesystem::path dir{}; // folder of this frame
};

class State {
public:
    static State& get() {
        static State s{};
        return s;
    }

    // ---- FoveatedRendering (main thread, every frame) -------------------------------------------
    // requested = the user wants quad view AND multipass + HMD + D3D12 + upscaler are available.
    void set_request(bool requested, const Settings& s) {
        std::scoped_lock _{m_mutex};

        const bool reinit = requested != m_requested.load() || s.fraction != m_settings.fraction || s.fovea_ratio != m_settings.fovea_ratio ||
                            s.upscaler != m_settings.upscaler || s.mono_periphery != m_settings.mono_periphery ||
                            s.fsr_direct != m_settings.fsr_direct || s.fsr_one_pass != m_settings.fsr_one_pass ||
                            s.fsr_auto_exposure != m_settings.fsr_auto_exposure || s.fsr_model != m_settings.fsr_model ||
                            s.fovea_supersample != m_settings.fovea_supersample;

        m_settings = s;
        m_requested = requested;
        m_mono_periphery = s.mono_periphery;

        if (reinit) {
            ++m_generation;
            m_layout = Layout{};
            m_have_rect[0] = m_have_rect[1] = false;
            clear_frame_rects();
            m_inset_serial.fetch_add(1, std::memory_order_relaxed);
        }

        update_engaged();
    }

    // false while the current settings failed to set up (see mark_failed) -> no clone camera, normal upscaler
    bool requested() const { return m_requested.load() && m_failed_generation.load() != m_generation.load(); }
    // [MONO PERIPHERY] the wanted periphery of the multipass quad view (no lock: read in the camera hooks)
    bool mono_periphery() const { return m_mono_periphery.load(); }
    uint32_t generation() const { return m_generation.load(); }

    // TemporalUpscaler: the upscaler instances for `generation` could not be created. Quad view stays off until
    // the settings change (or the mode is switched off and on).
    void mark_failed(uint32_t generation, const std::string& why) {
        std::scoped_lock _{m_mutex};

        if (generation == m_generation.load()) {
            m_failed_reason = why;
            m_failed_generation = generation;
        }

        update_engaged();
    }

    // [FSR DIRECT 06.11.2026] the upscaler set up again with the same settings (a part gave up and the next set-up leaves
    // it out): as a settings change
    void request_reinit() {
        std::scoped_lock _{m_mutex};
        ++m_generation;
        m_layout = Layout{};
        m_have_rect[0] = m_have_rect[1] = false;
        clear_frame_rects();
        m_inset_serial.fetch_add(1, std::memory_order_relaxed);
        update_engaged();
    }

    // empty when the current settings did not fail
    std::string failure() const {
        std::scoped_lock _{m_mutex};
        return m_failed_generation.load() == m_generation.load() ? m_failed_reason : std::string{};
    }

    Settings settings() const {
        std::scoped_lock _{m_mutex};
        return m_settings;
    }

    // [MONO PERIPHERY] VR -> upscaler, see Frusta
    void set_frusta(const Frusta& f) {
        std::scoped_lock _{m_mutex};
        m_frusta = f;
    }

    // [MONO UI] GUI elements of a frame (VR, GUI draw) and the world -> clip matrix of the shared wide view (VR, its layer
    // update; column-major like glm). The upscaler reads the last complete set.
    void add_gui_anchor(uint64_t frame, const GuiAnchor& a) {
        std::scoped_lock _{m_mutex};

        if (frame != m_gui_frame) {
            m_gui_anchors_done = std::move(m_gui_anchors);
            m_gui_anchors.clear();
            m_gui_frame = frame;
            m_gui_kind_count[0] = m_gui_kind_count[1] = m_gui_kind_count[2] = 0;
        }

        // [ITEM BEAMS 03.11.2026] the game's world elements must not crowd out the panels and the item icon: at most 48
        // beams and 16 other world elements (only listed) per frame
        const uint32_t kind = (std::min)((uint32_t)a.kind, 2u);
        const uint32_t cap = kind == GuiAnchor::BEAM ? 48u : kind == GuiAnchor::WORLD ? 16u : 96u;

        if (m_gui_kind_count[kind] >= cap) {
            return;
        }

        if (m_gui_anchors.size() < 128) {
            m_gui_anchors.push_back(a);
            ++m_gui_kind_count[kind];
        }
    }

    std::vector<GuiAnchor> gui_anchors() const {
        std::scoped_lock _{m_mutex};
        return m_gui_anchors.empty() ? m_gui_anchors_done : m_gui_anchors;
    }

    // [ANCHOR HOLD 03.11.2026] both lists: the last complete frame's and the one being filled (it may still miss elements
    // when the upscaler reads it), with the frame number of the latter
    void gui_anchor_lists(std::vector<GuiAnchor>& done, std::vector<GuiAnchor>& current, uint64_t& frame) const {
        std::scoped_lock _{m_mutex};
        done = m_gui_anchors_done;
        current = m_gui_anchors;
        frame = m_gui_frame;
    }

    // [UI MOTION fix125 22.11.2026] the elements of a frame with the matrices they were drawn with, kept per frame (the
    // number the frame's scene layer updates run under: the GUI is drawn after BeginRendering in the same frame, see the
    // engine's entry list in REFramework's log: LockScene, WaitRendering, BeginRendering, RenderGUI, EndRendering)
    void add_gui_element(int64_t frame, const GuiAnchor& a) {
        std::scoped_lock _{m_mutex};
        auto& k = m_gui_elements_by_frame[(size_t)frame % FRAME_KEEP];

        if (k.frame != frame) {
            k.frame = frame;
            k.v.clear();
        }

        if (k.v.size() < 192) {
            k.v.push_back(a);
        }
    }

    // false: no element of that frame is kept
    bool gui_elements(int64_t frame, std::vector<GuiAnchor>& out) const {
        std::scoped_lock _{m_mutex};
        const auto& k = m_gui_elements_by_frame[(size_t)frame % FRAME_KEEP];

        if (frame < 0 || k.frame != frame) {
            out.clear();
            return false;
        }

        out = k.v;
        return true;
    }

    // `frame` >= 0: also kept as that frame's matrix (FRAME_KEEP frames), see centre_view_proj
    void set_centre_view_proj(const float* m16, int64_t frame = -1) {
        std::scoped_lock _{m_mutex};
        std::copy(m16, m16 + 16, m_centre_vp);
        m_have_centre_vp = true;

        if (frame >= 0) {
            auto& k = m_centre_vp_by_frame[(size_t)frame % FRAME_KEEP];
            k.frame = frame;
            std::copy(m16, m16 + 16, k.m);
        }
    }

    // [FRAME LATCH 04.11.2026] The upscaler composites a frame on the render thread while the main thread may already
    // update the next frame's scene layers (since the mods' frame callbacks run at WaitRendering, 04.11.2026, the start of
    // BeginRendering no longer waits for the present hook): the latest matrix can be the next frame's. `frame` >= 0: the
    // matrix of that frame if it is kept (`newer`: the latest one is another frame's), else the latest.
    bool centre_view_proj(float* out16, int64_t frame = -1, bool* newer = nullptr) const {
        std::scoped_lock _{m_mutex};

        if (newer != nullptr) {
            *newer = false;
        }

        if (frame >= 0) {
            const auto& k = m_centre_vp_by_frame[(size_t)frame % FRAME_KEEP];

            if (k.frame == frame) {
                std::copy(k.m, k.m + 16, out16);

                if (newer != nullptr) {
                    *newer = !std::equal(k.m, k.m + 16, m_centre_vp);
                }

                return true;
            }
        }

        std::copy(m_centre_vp, m_centre_vp + 16, out16);
        return m_have_centre_vp;
    }

    Frusta frusta() const {
        std::scoped_lock _{m_mutex};
        return m_frusta;
    }

    void set_gaze(int eye, float ndc_x, float ndc_y) {
        std::scoped_lock _{m_mutex};
        eye = std::clamp(eye, 0, 1);
        m_gaze[eye][0] = ndc_x;
        m_gaze[eye][1] = ndc_y;
    }

    // ---- TemporalUpscaler (render thread) ---------------------------------------------------------
    // Called once the upscaler instances for `generation` exist. From then on the views render at layout.render_*.
    void set_layout(const Layout& l, uint32_t generation) {
        std::scoped_lock _{m_mutex};

        if (generation != m_generation.load()) {
            return; // settings changed meanwhile, a new re-init follows
        }

        m_layout = l;
        m_layout_generation = generation;
        m_have_rect[0] = m_have_rect[1] = false;
        clear_frame_rects();
        m_inset_serial.fetch_add(1, std::memory_order_relaxed);
        update_engaged();
    }

    void clear_layout() {
        std::scoped_lock _{m_mutex};
        m_layout = Layout{};
        m_inset_serial.fetch_add(1, std::memory_order_relaxed);
        update_engaged();
    }

    // Quad view is fully running: requested and the instances for the current settings exist.
    // [CPU 03.11.2026] Without the lock: the camera hooks ask it 500-1000 times per frame from many threads in fights with
    // many NPCs (log of 03.11.2026 15:40: 540 projection and 540 view requests per frame); the flag is set under the lock
    // wherever one of its parts changes (update_engaged).
    bool engaged() const {
        return m_engaged.load(std::memory_order_acquire);
    }

    Layout layout() const {
        std::scoped_lock _{m_mutex};
        return m_layout;
    }

    // ---- VR projection hook (render thread) ----------------------------------------------------
    // Computes (or reuses, inside the dead zone) the fovea rectangle of `eye` from the gaze and remembers it
    // for the compositing of this eye's image. Returns false when no layout is active.
    // `frame` >= 0: the engine asks for the projection 100-250 times per frame (log 22.10.2026); the rectangle is
    // decided by the first call of a frame and every later call of that frame gets the same one.
    bool inset_for_eye(int eye, NdcRect& out, int64_t frame = -1) {
        // [CPU 03.11.2026] the rectangle of a frame is latched by its first call; every thread keeps the latched one of
        // this frame and layout, so later calls of the frame (346 per frame in the fight log of 03.11.2026) take no lock
        struct Cached {
            int64_t frame{-1};
            uint32_t serial{0};
            NdcRect rect{};
        };
        thread_local Cached t_cached[2]{};
        const int ce = std::clamp(eye, 0, 1);
        const uint32_t serial = m_inset_serial.load(std::memory_order_relaxed);

        if (frame >= 0 && t_cached[ce].frame == frame && t_cached[ce].serial == serial && m_engaged.load(std::memory_order_relaxed)) {
            out = t_cached[ce].rect;
            return true;
        }

        const bool ok = inset_for_eye_locked(eye, out, frame);

        if (ok && frame >= 0) {
            t_cached[ce] = Cached{frame, serial, out};
        }

        return ok;
    }

    bool inset_for_eye_locked(int eye, NdcRect& out, int64_t frame) {
        std::scoped_lock _{m_mutex};

        if (!requested() || !m_layout.valid || m_layout_generation != m_generation.load()) {
            return false;
        }

        eye = std::clamp(eye, 0, 1);

        if (frame >= 0 && m_have_rect[eye] && m_latch_frame[eye] == frame) {
            out = m_latch_ndc[eye];
            return true;
        }

        const Rect wanted = place_rect(m_layout, m_gaze[eye][0], m_gaze[eye][1]);

        if (!m_have_rect[eye]) {
            m_rect[eye] = wanted;
            m_have_rect[eye] = true;
        } else {
            const float dz = m_settings.deadzone * (float)m_layout.display_w;
            const float dx = (float)(wanted.x - m_rect[eye].x);
            const float dy = (float)(wanted.y - m_rect[eye].y);

            // ([LIGHTS fix86 18.11.2026] the light flicker test's phase "the fovea rectangles held still": no move at all)
            if (std::sqrt(dx * dx + dy * dy) > dz && !m_test_hold_rect.load(std::memory_order_relaxed)) {
                m_rect[eye] = wanted;
                ++m_rect_moves;
            }
        }

        out = rect_to_ndc(m_layout, m_rect[eye]);
        m_latch_frame[eye] = frame;
        m_latch_ndc[eye] = out;

        if (frame >= 0) {
            m_frame_rect[eye][(size_t)frame % FRAME_KEEP] = FrameRect{frame, m_rect[eye]};
        }

        return true;
    }

    // The rectangle the inset of `eye` was last rendered with (for the compositing).
    bool rect_for_eye(int eye, Rect& out) const {
        std::scoped_lock _{m_mutex};
        eye = std::clamp(eye, 0, 1);

        if (!m_have_rect[eye] || !m_layout.valid) {
            return false;
        }

        out = m_rect[eye];
        return true;
    }

    // [FRAME LATCH 04.11.2026] The rectangle the fovea view of render frame `frame` was rendered with. The compositing of a
    // frame runs on the render thread while the main thread may already have started the next frame, whose first
    // projection request moves the rectangle when the gaze has moved: the fovea image was then put where the next frame's
    // fovea will be, for one frame (the user's short flash near the gaze, 04.11.2026). `newer`: the current rectangle is
    // another one (what the compositing used before). A frame that is not kept: the current rectangle.
    bool rect_for_frame(int eye, int64_t frame, Rect& out, bool* newer = nullptr) const {
        std::scoped_lock _{m_mutex};
        eye = std::clamp(eye, 0, 1);

        if (newer != nullptr) {
            *newer = false;
        }

        if (!m_have_rect[eye] || !m_layout.valid) {
            return false;
        }

        out = m_rect[eye];

        if (frame >= 0) {
            const auto& k = m_frame_rect[eye][(size_t)frame % FRAME_KEEP];

            if (k.frame == frame) {
                out = k.rect;

                if (newer != nullptr) {
                    *newer = k.rect.x != m_rect[eye].x || k.rect.y != m_rect[eye].y || k.rect.w != m_rect[eye].w || k.rect.h != m_rect[eye].h;
                }
            }
        }

        return true;
    }

    // ---- frame dump (debug) --------------------------------------------------------------------
    // The next `frames` quad-view frames are dumped. Each frame gets its own folder <base>/dumpNN/frameK_both_eyes; the
    // upscaler writes its images there, the VR submit adds the two eye images that actually went to the headset.
    void request_dump(int frames) {
        std::scoped_lock _{m_mutex};
        m_dump_frames_left = (std::max)(frames, 1);
        m_dump_frame_index = 0;
        ++m_dump_serial;
        layer_trace_frames = (std::max)(frames, 2) + 1;
        layer_trace_request = true;
        jitter_check_request = true; // [JITTER CHECK 10.11.2026]
        camera_check_request = true; // [CAMERA CHECK 10.11.2026]
    }

    // [JITTER CHECK 10.11.2026] with every dump: TemporalUpscaler records consecutive frames of FSR 4's inputs (centre of the
    // left fovea, values as stored) and the jitter of each view at update and at draw time (dumpNN/jitter_check)
    std::atomic<bool> jitter_check_request{false};
    // [SUPER TAA 16.11.2026] the supersampling TAA's capture (TemporalUpscaler: consecutive frames of its inputs, results and
    // state as stored, for the replay with the numpy model), the engine buffer probe (the scene layer's targets listed and
    // written once), and its dispatches (the log: is it the one running)
    std::atomic<bool> super_capture_request{false};
    std::atomic<bool> super_capture_running{false};
    std::atomic<int> super_capture_frames{16};   // 1..64 (d3d12::SuperTAACapture::MAX_FRAMES)
    std::atomic<int> super_capture_part{256};    // side of the captured part in render pixels (64..512)
    std::atomic<bool> engine_probe_request{false};
    DiagAtomic<uint64_t> super_dispatches{0};
    DiagAtomic<uint64_t> super_fallbacks{0}; // frames it was chosen and the mod's TAA ran instead (it failed)
    // [CAMERA CHECK 10.11.2026] with every dump: CameraDuplicator logs the components of the game camera and of each clone,
    // and every property of the copied components that differs between them (framework log)
    std::atomic<bool> camera_check_request{false};
    // [MULTIPASS DUMP 13.11.2026] frames of the plain multipass (quad view off) whose two submitted eye images are written
    // at full resolution (D3D12Component::dump_multipass_eyes)
    std::atomic<int> multipass_dump_request{0};
    // [FOVEA RAY TRACING 12.11.2026] the fovea views (the camera clones) get the game camera's ray tracing component
    // (CameraDuplicator::sync_clone_ray_tracing); FoveatedRendering's option, every frame
    std::atomic<bool> fovea_ray_tracing{false};

    // [LAYER TRACE 27.10.2026] VR lists every fully rendered scene layer of the dumped frames with its camera, pose and
    // matrices (into the dump report). One frame more than the dump: the request may land in the middle of a frame.
    std::atomic<bool> layer_trace_request{false};
    std::atomic<int> layer_trace_frames{3};

    bool dump_running() const {
        std::scoped_lock _{m_mutex};
        return m_dump_frames_left > 0 || m_eye_dump_pending;
    }

    // TemporalUpscaler (render thread): true = dump this frame; `out.dir` must be filled in by the caller via
    // set_dump_dir before the VR submit of the same frame reads it.
    bool take_dump_frame(DumpFrame& out) {
        std::scoped_lock _{m_mutex};

        if (m_dump_frames_left <= 0) {
            return false;
        }

        --m_dump_frames_left;
        out.serial = m_dump_serial;
        out.index = m_dump_frame_index++;
        out.dir.clear();
        m_eye_dump = out;
        m_eye_dump_pending = false;
        return true;
    }

    // TemporalUpscaler: folder of the frame it just dumped -> the VR submit of this frame dumps the eye images there
    void set_dump_dir(const std::filesystem::path& dir) {
        std::scoped_lock _{m_mutex};
        m_eye_dump.dir = dir;
        m_eye_dump_pending = true;
    }

    // D3D12Component (present): the eye images of the frame the upscaler just dumped
    bool take_eye_dump(DumpFrame& out) {
        std::scoped_lock _{m_mutex};

        if (!m_eye_dump_pending) {
            return false;
        }

        m_eye_dump_pending = false;
        out = m_eye_dump;
        return true;
    }

    // Report pieces (upscaler part, eye part) -> FoveatedRendering writes them into foveation_debug.log
    void push_dump_report(std::string report) {
        std::scoped_lock _{m_mutex};
        m_dump_reports.push_back(std::move(report));
    }

    std::vector<std::string> pop_dump_reports() {
        std::scoped_lock _{m_mutex};
        std::vector<std::string> out{};
        out.swap(m_dump_reports);
        return out;
    }

    // [SUPER TAA 16.11.2026] text for foveation_debug.log from the render side that is not a frame dump's (the
    // supersampling TAA's capture, the engine buffer probe): FoveatedRendering writes each as it is, and shows `message`
    // of the last one in the menu
    void push_note(std::string text, std::string message = {}) {
        std::scoped_lock _{m_mutex};
        m_notes.emplace_back(std::move(text), std::move(message));
    }

    std::vector<std::pair<std::string, std::string>> pop_notes() {
        std::scoped_lock _{m_mutex};
        std::vector<std::pair<std::string, std::string>> out{};
        out.swap(m_notes);
        return out;
    }

    // ---- [VIDEO 25.10.2026] video of the two eye images that go to the headset ---------------------------------
    // FoveatedRendering (menu) asks, D3D12Component records (it owns d3d12::EyeRecorder) and reports its status.
    void request_video(double seconds) {
        std::scoped_lock _{m_mutex};
        m_video_request = seconds;
    }

    bool take_video_request(double& seconds) {
        std::scoped_lock _{m_mutex};

        if (m_video_request <= 0.0) {
            return false;
        }

        seconds = m_video_request;
        m_video_request = 0.0;
        return true;
    }

    void set_video_status(const std::string& status) {
        std::scoped_lock _{m_mutex};
        m_video_status = status;
    }

    std::string video_status() const {
        std::scoped_lock _{m_mutex};
        return m_video_status;
    }

    // ---- [LIGHT FLICKER TEST 15.11.2026] the flicker measurement of FoveatedRendering's light flicker test -----------
    // FoveatedRendering asks and sets the tag of each frame (the test's phase, -1 = not measured); D3D12Component runs the
    // measurement in d3d12::EyeRecorder (both eye images as they go to the headset) and hands back one row per tag.
    struct FlickerRow {
        int tag{-1};
        uint64_t frames{0};     // frames measured with this tag
        uint64_t flips{0};      // pixels that went up and back down (or down and back up), summed over the frames
        uint32_t cell_x{0};     // the cell (32 x 32 of the half-size image, both eyes side by side) with the most of them
        uint32_t cell_y{0};
        uint64_t cell_flips{0};
        uint64_t peri_flips{0};      // [LIGHT FLICKER TEST 15.11.2026 evening] the same outside the fovea rectangles
        uint32_t peri_cell_x{0};
        uint32_t peri_cell_y{0};
        uint64_t peri_cell_flips{0};
        // [LIGHTS fix86 18.11.2026] the light's course at one cell (as d3d12::EyeRecorder::MeasureRow's)
        uint32_t series_cell_x{0};
        uint32_t series_cell_y{0};
        std::vector<uint8_t> series{};
        std::vector<uint8_t> moved{};
    };

    // [LIGHTS fix86 18.11.2026] the light flicker test: the fovea rectangles stay where they are while this is set
    void set_test_hold_rect(bool hold) { m_test_hold_rect.store(hold, std::memory_order_relaxed); }

    void request_flicker_measure(double seconds) {
        std::scoped_lock _{m_mutex};
        m_flicker_request = seconds;
        m_flicker_ready = false;
        m_flicker_rows.clear();
    }

    bool take_flicker_measure_request(double& seconds) {
        std::scoped_lock _{m_mutex};

        if (m_flicker_request <= 0.0) {
            return false;
        }

        seconds = m_flicker_request;
        m_flicker_request = 0.0;
        return true;
    }

    void set_flicker_rows(std::vector<FlickerRow> rows) {
        std::scoped_lock _{m_mutex};
        m_flicker_rows = std::move(rows);
        m_flicker_ready = true;
    }

    bool take_flicker_rows(std::vector<FlickerRow>& rows) {
        std::scoped_lock _{m_mutex};

        if (!m_flicker_ready) {
            return false;
        }

        rows = std::move(m_flicker_rows);
        m_flicker_rows.clear();
        m_flicker_ready = false;
        return true;
    }

    std::atomic<int> flicker_tag{-1};

    // ---- counters (debug log) ------------------------------------------------------------------
    DiagCounter inset_by_camera{};   // inset projection given because the clone camera asked
    DiagCounter inset_by_scope{};    // ... because we are inside the inset layer update
    DiagCounter gui_inset{};         // GUI camera projection inside the inset layer update
    DiagAtomic<uint64_t> inset_layers{0};      // inset scene layer updates seen
    DiagAtomic<uint64_t> wide_layers{0};       // wide scene layer updates seen
    DiagAtomic<uint64_t> evaluations{0};       // quad view evaluations (both upscalers + composite)
    DiagAtomic<uint64_t> skipped{0};           // engaged but evaluation skipped (missing resources)
    DiagAtomic<uint64_t> skipped_layers{0};    // [MONO PERIPHERY] scene layers of surplus clones switched off
    DiagAtomic<uint64_t> reprojections{0};     // [MONO PERIPHERY] periphery reprojections (one per eye and frame)
    // [ONE LIST 01.11.2026] shared periphery frames by the way their lists went: [0] a pre list of its own, then the copier,
    // [1] one list (the GPU pass test checks its variants really ran)
    std::array<std::atomic<uint64_t>, 2> path_frames{};
    // [VIEW DIFFERENCE 01.11.2026] per eye, measured some frames ago when Settings::view_metric: mean |periphery - fovea x one
    // gain| over the mean periphery, the same with the fovea's gain / table; view_diff_frames counts the measurements
    std::array<std::atomic<float>, 2> view_diff{};
    std::array<std::atomic<float>, 2> view_diff_matched{};
    DiagAtomic<uint64_t> view_diff_frames{0};
    // [EV FOLLOW 13.11.2026] the same measurement's one gain (periphery / fovea brightness, blocks of 16 px) per eye slot,
    // with a count per slot: the fovea views' exposure test follows it (CameraDuplicator::apply_tone_test)
    std::array<std::atomic<float>, 2> view_gain{};
    std::array<std::atomic<uint64_t>, 2> view_gain_frames{};
    // [MENU LIGHT 15.11.2026] when a game menu (inventory, pause, dialog, ...) was last drawn (steady clock, ns; 0 = never): the
    // large-scale light pauses while one is open (FoveatedRendering::make_settings)
    std::atomic<int64_t> menu_gui_ns{0};
    // [MENU LIGHT 15.11.2026 evening] the render frame of that draw (VR::get_render_frame_count): the menu counts as open while
    // either is recent, so a frame held for a while (a frame dump, a hitch) does not end it
    std::atomic<int32_t> menu_gui_frame{0};
    // [fix109 20.11.2026] the GPU test's two texts of the eye pass (d3d12::EyeCompose::precompile_tests): 0 = not asked for,
    // 1 = being compiled on their thread, 2 = ready, 3 = failed. The test's countdown waits while it is 1: the text with the
    // loop took 45 s on the user's PC, and on the render thread it froze the game for 42 s (fix108, 20:28).
    std::atomic<int> test_texts{0};
    // [fix115 21.11.2026] presents that found the runtime still waiting for its pose update and submitted nothing
    // (D3D12Component::on_frame; it wrote a log line each time: 5492 lines in 8 minutes, each one flushed to the file)
    DiagAtomic<uint32_t> pose_in_present{0};
    // [COPIES fix116 21.11.2026] GPU test: 1 = VR::ui_capture leaves the fovea views' two copies out (see Settings)
    std::atomic<int> test_ui_copies{0};
    // [SMALL WINDOW fix122 22.11.2026] the option "the game's window small": the VR mod makes the window (and with it the
    // backbuffer the engine fits its picture into every frame) the size of an engine view instead of an eye image; the eye
    // images keep the headset's size (TemporalUpscaler::eye_image_size). Read by VR::on_view_get_size and TemporalUpscaler.
    std::atomic<bool> small_window{false};
    // [VIEW SCALE 15.11.2026] objects the engine draws with view space scaling (the item looked at in RE7) at their real
    // distance while the fovea views are drawn: the camera's via.render.ViewScaling at 1 (CameraDuplicator)
    std::atomic<bool> real_view_scale{true};
    // [SCREEN EFFECTS 15.11.2026] the cameras' lens and video tape effects (via.render.PolynomialLens, via.render.RetroFilm)
    // off while the fovea views are drawn (CameraDuplicator)
    std::atomic<bool> screen_effects_off{true};
    // [IMAGE QUALITY 15.11.2026] the game camera's via.render.RenderOutput ImageQuality held at the fovea views' value while
    // they are drawn (RE7 video tape: the wide view at 2/3 of the size otherwise; CameraDuplicator)
    std::atomic<bool> image_quality_hold{true};
    DiagAtomic<uint64_t> ui_composed{0};       // [MONO UI] eye images that got their interface put together (one per eye and frame)
    DiagAtomic<uint64_t> ui_missing{0};        // [MONO UI] ... that had no interface copies of the frame to use
    // [FRAME LATCH 04.11.2026] compositings (one per eye and frame) where the current fovea rectangle / the latest wide view
    // matrix already was the next frame's (before: used, the fovea one frame in the wrong place / the anchors one frame
    // off); now the frame's own is used
    DiagAtomic<uint64_t> compose_rect_newer{0};
    DiagAtomic<uint64_t> compose_vp_newer{0};
    // [JITTER LATCH 10.11.2026] FSR 4 frames where the views' latest jitter was already the next frame's (each frame now
    // gets its own)
    DiagAtomic<uint64_t> jitter_newer{0};
    // [ANCHOR FINAL 04.11.2026] GUI anchors listed at the position the element was drawn at, another mod having moved it
    // after the VR mod placed it (weapon HUD at the hand, reticle at the aim point)
    DiagAtomic<uint64_t> gui_anchors_moved{0};
    // [ONE SUBMIT 04.11.2026] the upscaler plugin's evaluations in the fovea and the CPU time in them (wall clock)
    DiagAtomic<uint64_t> plugin_evals{0};
    DiagAtomic<uint64_t> plugin_eval_ns{0};
    // [FSR DIRECT 06.11.2026] FSR 4 called by the mod: its dispatches in the fovea and the CPU time in them (wall clock)
    DiagAtomic<uint64_t> fsr_dispatches{0};
    DiagAtomic<uint64_t> fsr_dispatch_ns{0};
    // [FSR DIRECT 06.11.2026 evening] what the upscaler in the fovea is when it is chosen: 0 = the plugin, 1 = FSR 4 by the
    // mod a pass per eye, 2 = FSR 4 by the mod in one pass (the GPU guard's estimate of what it costs above the mod's TAA)
    std::atomic<uint32_t> fovea_upscaler_kind{0};
    // [CPU 22.10.2026] how often hot hooks run (rates in the foveation log)
    DiagCounter calls_projection{};      // camera projection matrix hook (VR)
    DiagCounter calls_view{};            // camera view matrix hook (VR)
    DiagCounter views_game_logic{};      // [VIEW RACE 28.10.2026] eye transforms asked by game logic (head given)
    DiagCounter calls_primary_camera{};  // get_PrimaryCamera hook (camera duplicator)
    // [VIEW RACE 03.11.2026] the other eye transform requests with the shared periphery: game logic that kept the pass's
    // eye (the head view switched off or not for its kind), requests inside a scene layer update (render side), requests
    // of other threads while a scene layer update runs (its jobs: the pass's eye), requests of other threads while none
    // runs (the engine's jobs for game logic: the head in mode 2)
    DiagCounter views_logic_kept{};
    DiagCounter views_in_layer{};
    DiagCounter views_other_threads{};
    DiagCounter views_jobs_outside{};
    // [VIEW RACE 03.11.2026] who gets the head's view (Debug tools): 0 = nobody (the pass's eye, as before 03.11), 1 = the
    // main thread outside scene layer updates (test of 03.11 12:00), 2 = also other threads while no layer update runs
    alignas(64) std::atomic<int> head_view_mode{2}; // [CPU 03.11.2026] read by every camera hook call: a line of its own
    // [CPU PROBE 03.11.2026] (Debug tools, not saved) the fovea views render a frustum of 2 % of their size: what they cost the
    // CPU for the objects in them (culling, draw lists, per-object work) shows as the difference in the log; the fovea shows
    // a tiny part of the scene meanwhile
    std::atomic<bool> cpu_probe_tiny_fovea{false};
    // [FOVEA FOV TEST 06.11.2026 evening] (GPU test) the fovea cameras' field of view for the engine narrowed to what holds the
    // fovea (VR::fovea_cull_fov, set in VR::update_camera at WaitRendering, after the copy at LockScene). The image uses the mod's projection; the
    // RGP captures of 06.11 21:01 show the fovea views sending as many draws with a frustum of 2 % as normally: the engine
    // picks their objects with something else, maybe the camera's FOV (106 degrees, the eye's). The values set, for the log
    // (thousandths of a degree; narrow_fov_engine: the VR mod's value without the test, from the headset)
    std::atomic<bool> test_narrow_fovea_fov{false};
    // [FLICKER TEST 15.11.2026] other threads asking for the camera's view while a view is updated get the head's view too
    std::atomic<bool> test_head_all_threads{false};
    // [LIGHT FLICKER TEST 15.11.2026 evening] (one phase of the test) the fovea views render their eye's whole frustum into the
    // fovea's size: the fovea rectangle shows the whole eye squeezed for that phase, the periphery is what is measured
    std::atomic<bool> test_full_fovea_frustum{false};
    // [PROJECTION CALLERS 15.11.2026] The light flicker test of 15.11.2026 21:3x (RE7, top of the stairs): the periphery's
    // flicker fell to 16 % with the fovea views on their eye's whole frustum, and to 71 - 117 % with each other switch.
    // Something of the engine that takes a fovea camera's projection switches the wide view's light. Where the engine
    // asks for it: per call site (the return address of its get_ProjectionMatrix call) the requests in each context:
    // 0 = the fovea view's own scene layer update on this thread (its image's matrices are taken there), 1 = another
    // view's layer update on this thread, 2 = another thread while a layer update runs, 3 = no layer update running.
    static constexpr int PROJ_CTX = 4;
    static constexpr int PROJ_SITES = 16;
    struct ProjSite {
        std::atomic<uintptr_t> addr{0};
        std::array<std::atomic<uint64_t>, PROJ_CTX> n{};
    };
    std::array<ProjSite, PROJ_SITES> proj_sites{};
    DiagAtomic<uint64_t> proj_sites_full{0}; // requests of call sites beyond the table

    void note_proj_site(uintptr_t addr, int ctx) {
        if (addr == 0 || ctx < 0 || ctx >= PROJ_CTX) {
            return;
        }

        for (auto& s : proj_sites) {
            uintptr_t a = s.addr.load(std::memory_order_relaxed);

            // a free place: taken for this call site (another thread may take it first, for this or another one)
            if (a == 0 && s.addr.compare_exchange_strong(a, addr, std::memory_order_relaxed)) {
                a = addr;
            }

            if (a == addr) {
                s.n[(size_t)ctx].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        proj_sites_full.fetch_add(1, std::memory_order_relaxed);
    }

    // the test's switches: the fovea cameras' requests from this call site / in these contexts (one bit each) get the eye's
    // whole frustum
    std::atomic<uintptr_t> test_full_frustum_site{0};
    std::atomic<uint32_t> test_full_frustum_ctx{0};
    // [LIGHTS fix82 17.11.2026] The light flicker test of 17.11.2026 16:0x (RE7, the top of the stairs; log of fix81): the
    // periphery's flicker at its hot spot fell to 13 - 14 % with the eye's whole frustum for the one projection request the
    // engine makes inside a fovea view's own scene layer update, and stayed (124 %) with it for the requests outside the
    // updates. What the engine decides in a fovea view's update with that view's frustum (which lights count, among other
    // things) holds for the wide view of the same frame as well: a light outside the fovea's frustum went out in the
    // periphery, and on and off at the fovea's border as the gaze moved. In that test the fovea's image was wrong (the
    // whole eye squeezed into it), because the image's matrices came from the same request.
    // fix82 parted the two: the request inside a fovea view's own update gets the eye's whole frustum (VR::
    // apply_quadview_inset), and right after the update the fovea's part is put into the matrices the engine made for the
    // image (VR::on_scene_layer_update, before the upscaler adds its jitter to the same matrices).
    // [LIGHTS fix83 17.11.2026] The test of fix82 (log of 17.11.2026 19:2x, the same place): with that the flicker stayed
    // (120 % at the hot spot), with the whole frustum for everything it fell to 11 %. What the engine decides during the
    // update is not it: it is one of the six sets of matrices the fovea layer is drawn with afterwards (scene, depth
    // distortion, filter, jitter disable, jitter disable post, z prepass). fix82's setting did nothing and is gone; the
    // mechanism stays for the test and for trying its result by hand: the sets named by a mask (bit 0 the scene's ... bit 5
    // the z prepass's) keep the eye's whole frustum, the others get the fovea's part. Mask 0: the fovea views' update with
    // their own frustum, as before fix82.
    //   fovea_whole_infos: the setting (FoveatedRendering, "Fovea views: matrix sets with the eye's whole frustum"), 0 = off
    //   test_whole_infos: the light flicker test's phases: -1 the setting, else the mask
    //   whole_update_requests / whole_update_narrowed: requests answered with the whole frustum, and scene layer updates
    //   whose image matrices got the fovea's part afterwards (for the log)
    static constexpr uint32_t WHOLE_INFOS_ALL = 63u;
    std::atomic<uint32_t> fovea_whole_infos{0};
    std::atomic<int32_t> test_whole_infos{-1};
    DiagAtomic<uint64_t> whole_update_requests{0};
    DiagAtomic<uint64_t> whole_update_narrowed{0};
    // [LIGHTS fix84 18.11.2026] The fifth round (log of 17.11.2026 22:5x): only the eye's whole frustum in the fovea views'
    // *scene* matrices takes the flicker away (12 % at the hot spot; the other five sets 92 - 115 %), and those draw the
    // fovea's picture. So the light in the wide view hangs on the frustum the fovea views are drawn with. The fovea views'
    // layers come before the wide view's (their priorities are lower: 83887488 and 83887616 against 83887744 in RE7): if
    // the engine decides something for the frame with the first view drawn, the wide view first would decide it for all.
    //   test_wide_first: the light flicker test's phase: the wide view's layer gets a priority below the fovea views'
    //   (VR::on_pre_scene_layer_update; its own again when the phase ends)
    //   first_wide / first_fovea: frames whose first scene layer update was the wide view's / a fovea view's (whether the
    //   order followed the priority)
    // [LIGHTS fix85 18.11.2026] The three tests of 18.11.2026: it does (see Settings::wide_first, now the rule).
    //   wide_first: the setting (FoveatedRendering keeps it)
    //   test_wide_first: -1 the setting, 0 / 1 the light flicker test's phases (the fovea views first / the wide view)
    std::atomic<bool> wide_first{true};
    std::atomic<int32_t> test_wide_first{-1};
    bool wide_first_now() const {
        const int32_t t = test_wide_first.load(std::memory_order_relaxed);
        return t >= 0 ? t != 0 : wide_first.load(std::memory_order_relaxed);
    }
    DiagAtomic<uint64_t> first_wide{0};
    DiagAtomic<uint64_t> first_fovea{0};
    uint32_t whole_infos_now() const {
        const int32_t t = test_whole_infos.load(std::memory_order_relaxed);
        return (t >= 0 ? (uint32_t)t : fovea_whole_infos.load(std::memory_order_relaxed)) & WHOLE_INFOS_ALL;
    }
    bool update_whole_now() const { return whole_infos_now() != 0; }
    // the fovea views' scene layer updates whose image matrices came out with the fovea's frustum (narrow) or the eye's whole
    // one: whether the fovea image stayed right during a phase of the test
    DiagAtomic<uint64_t> fovea_matrix_narrow{0};
    DiagAtomic<uint64_t> fovea_matrix_whole{0};
    DiagAtomic<uint64_t> narrow_fov_count{0};
    DiagAtomic<uint64_t> narrow_fov_sum{0};
    std::atomic<uint32_t> narrow_fov_min{UINT32_MAX};
    DiagAtomic<uint32_t> narrow_fov_max{0};
    DiagAtomic<uint32_t> narrow_fov_engine{0};

    // [VIEW RACE 03.11.2026] which application entries asked for the view as game logic (for the log): counted by name
    void note_logic_entry(const char* name) {
        std::scoped_lock _{m_entry_mtx};

        for (auto& e : m_logic_entries) {
            if (e.first == name) {
                ++e.second;
                return;
            }
        }

        if (m_logic_entries.size() < 32) {
            m_logic_entries.emplace_back(name, 1);
        }
    }

    // the entries since the last call, most first ("name count, ..."), and start counting again
    std::string take_logic_entries() {
        std::vector<std::pair<const char*, uint64_t>> v{};
        {
            std::scoped_lock _{m_entry_mtx};
            v.swap(m_logic_entries);
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string o{};

        for (size_t i = 0; i < v.size() && i < 8; ++i) {
            o += std::string(i > 0 ? ", " : "") + (v[i].first != nullptr ? v[i].first : "(none)") + " " + std::to_string(v[i].second);
        }

        return o.empty() ? std::string{"none"} : o;
    }

    uint64_t rect_moves() const {
        std::scoped_lock _{m_mutex};
        return m_rect_moves;
    }

private:
    std::mutex m_entry_mtx{};
    std::vector<std::pair<const char*, uint64_t>> m_logic_entries{}; // (entry names are the engine's own strings)

public:

private:
    State() = default;

    mutable std::mutex m_mutex{};
    std::atomic<bool> m_requested{false};
    std::atomic<uint32_t> m_generation{1};
    std::atomic<uint32_t> m_failed_generation{0};
    alignas(64) std::atomic<bool> m_engaged{false}; // [CPU 03.11.2026] engaged(), set by update_engaged under m_mutex
    std::atomic<uint32_t> m_inset_serial{0};  // [CPU 03.11.2026] changes with the layout: the threads' latched rects are old

    // under m_mutex, after a change of the request, the failure, the generation or the layout
    void update_engaged() {
        m_engaged.store(requested() && m_layout.valid && m_layout_generation == m_generation.load(), std::memory_order_release);
    }
    std::string m_failed_reason{};
    Settings m_settings{};
    Layout m_layout{};
    uint32_t m_layout_generation{0};
    float m_gaze[2][2]{};
    Rect m_rect[2]{};
    bool m_have_rect[2]{};
    Frusta m_frusta{};
    std::atomic<bool> m_mono_periphery{true};
    std::vector<GuiAnchor> m_gui_anchors{};
    std::vector<GuiAnchor> m_gui_anchors_done{};
    uint64_t m_gui_frame{~0ull};
    uint32_t m_gui_kind_count[3]{}; // [ITEM BEAMS] entries of this frame per GuiAnchor::Kind
    float m_centre_vp[16]{};
    bool m_have_centre_vp{false};
    // [FRAME LATCH 04.11.2026] per frame: the fovea rectangle each eye was rendered with, the wide view's matrix
    static constexpr size_t FRAME_KEEP = 8;
    struct FrameRect {
        int64_t frame{-1};
        Rect rect{};
    };
    struct FrameMatrix {
        int64_t frame{-1};
        float m[16]{};
    };
    FrameRect m_frame_rect[2][FRAME_KEEP]{};
    FrameMatrix m_centre_vp_by_frame[FRAME_KEEP]{};

    struct FrameElements { // [UI MOTION fix125]
        int64_t frame{-1};
        std::vector<GuiAnchor> v{};
    };

    FrameElements m_gui_elements_by_frame[FRAME_KEEP]{};

    // under m_mutex
    void clear_frame_rects() {
        for (auto& e : m_frame_rect) {
            for (auto& k : e) {
                k = FrameRect{};
            }
        }
    }

    int64_t m_latch_frame[2]{-1, -1};
    NdcRect m_latch_ndc[2]{};
    uint64_t m_rect_moves{0};

    int m_dump_frames_left{0};
    int m_dump_frame_index{0};
    uint64_t m_dump_serial{0};
    DumpFrame m_eye_dump{};
    bool m_eye_dump_pending{false};
    std::vector<std::string> m_dump_reports{};
    std::vector<std::pair<std::string, std::string>> m_notes{}; // [SUPER TAA 16.11.2026] push_note

    double m_video_request{0.0};
    std::string m_video_status{};

    double m_flicker_request{0.0};          // [LIGHT FLICKER TEST 15.11.2026]
    std::vector<FlickerRow> m_flicker_rows{};
    std::atomic<bool> m_test_hold_rect{false}; // [LIGHTS fix86 18.11.2026]
    bool m_flicker_ready{false};
};

} // namespace quadview
