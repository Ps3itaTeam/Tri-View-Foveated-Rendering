// SuperTAA.hpp -- [SUPER TAA 16.11.2026] the mod's supersampling temporal anti-aliasing of a fovea view (quad view,
// multipass; chosen as "Supersampling TAA of the mod" instead of the upscaler plugin or d3d12::FoveaTAA).
//
// What it does differently from d3d12::FoveaTAA:
//   * linear light: the engine's image is display-encoded; it is decoded, accumulated in linear light and encoded
//     again at the end (an edge half covered by a bright thing comes out as bright as the eye would see it; in encoded
//     values thin bright things lose most of their light).
//   * a history with more texels than the fovea image (2 x 2 per pixel with scale 2): every frame's jittered samples go
//     into the history texels they lie on (weighted by their distance), so the history holds detail finer than a
//     pixel and resampling it for a head that never keeps still costs far less sharpness; the fovea image is its
//     2 : 1 reduction.
//   * history read with Catmull-Rom plus a part of its difference to the plain bilinear tap (sharper), kept inside the
//     range of the four texels around the position (no ringing, no feedback of the sharpening).
//   * [fix77 16.11.2026] the history has to be within what this frame shows around the place (the 3x3 box of the frame,
//     nothing wider), and it keeps as much weight as the picture allows: per pixel the low-passed frame is compared with
//     the low-passed history at the same places (3x3 samples each, without the largest and the smallest: a single spark
//     is no change; perceptual luma):
//       d = lowpass(frame) - lowpass(history)
//       b = the fast mean of d: a history that lags (text that changed, a light, a highlight that slides)
//       m = the mean of |d - b|: the noise around it (stochastic shadows, dither), believed up to a cap
//     A change at once (8 of the 9 samples differ from the history the same way: a light, a shadow, a surface) or
//     confirmed (d two frames in a row the same way and 5 of the 9 samples with it: not a line thinner than a pixel that
//     this frame's samples happened to hit) leaves the history little weight there: it shows the frame within a frame or
//     two. Everywhere else the history keeps the weight of the "stability".
//   * [fix78 16.11.2026] what one frame's samples cannot show is protected again (fix76 did it, and the picture was
//     steady; fix77 took it out with the ghosts, and thin bright edges and stochastic shadows flickered again), now
//     released where the history is stale. e = the pixel's sample minus the mean of its 3x3 (perceptual luma), its mean
//     and variance over time carried along the motion: the deviation is what things thinner than a pixel, edges under
//     the jitter and the engine's stochastic shadows make of the samples from frame to frame. Where it is high the
//     history may leave the frame's box by 1.5 deviations, the frame's sample is kept within 2.5 deviations of the
//     history, and the history may hold 1.5 times the weight. The release (0..1, carried along the motion, 0.8 of it
//     left a frame later) takes that away: where the test above sees a change (now, or a frame ago within a pixel and a
//     half); where a point leaves or enters the rim of a nearer thing that moves against what is behind it (the motion
//     that fetches the history minus the pixel's own motion changes by 0.08..0.25 px from one frame to the next: what
//     the history holds there was fetched along another motion; the thing itself and its rim are fetched the same way
//     every frame and keep their protection; fix76 looked at the motions within the 3x3 from 0.5 px on and left
//     trails); and where there is no history. The bands of the test above are never narrower than 0.7 deviations (a
//     stochastic shadow is not a change).
//   * [fix79 16.11.2026] things that arrive. A fly moves 3 to 11 pixels a frame and its motion vectors hold the
//     camera's motion only: every frame it is somewhere the history knows nothing of. Its samples (white specks on a
//     dark body) were taken for scatter, in its pixels and around them, and what it left was protected: a dotted trail
//     of some 20 frames. Now: where the nearest surface of the 3x3 is nearer than anything the last frame held within 3
//     pixels of the place the history comes from, and it is a thing and not a line (4 samples that near in some 3x3
//     window around the pixel: a wire thinner than a pixel that the samples hit here and there is not one), the
//     release is set above 1 (2; a frame later 1.6 in what was within a pixel and a half), fading like any release.
//     While it is above 1 the statistics stand still. What the fly leaves is clipped away in the next frame, as
//     without protection; nothing else changes (the test needs the depth).
//
// Passes (compute, cs_5_0, compiled at runtime like the passes of FoveaComposite.cpp):
//   analyze     render size   colour, motion, depth, last history, statistics, distances and release -> linear colour,
//                             guide (motion of the nearest surface in pixels, the weight the history may keep (0 = no
//                             history), protection), clip box (centre, extent), statistics, distances and release
//   accumulate  history size  linear colour, guide, box, statistics, last history -> history (rgb linear, a = weight)
//   encode      fovea size    history -> display-encoded image (1 : 1 or 2 : 1)
//   sharpen     fovea size    RCAS on the encoded image -> the fovea image (left out with sharpening 0)
//
// The reference of every pass is the numpy model the GPU test compares it with (tests/supertaa).
//
// [fix80 17.11.2026] A second instance anti-aliases the shared wide view (the periphery) instead of d3d12::LightTAA
// (TemporalUpscaler::periphery_taa): one view (set_slots(1)), the same passes and settings. Its image is either the
// wide view's size (history 2 x 2 texels per pixel, reduced 2 : 1 with Lanczos 2) or twice that size (`out` twice the
// colour's size with scale 1: the history itself, encoded 1 : 1; d3d12::MonoPeriphery then writes 2 x 2 pixels per
// pixel of the wide view, see its Params::ss).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>

#include "ComPtr.hpp"
#include "UiMotion.hpp" // [UI MOTION fix125]

namespace d3d12 {

struct SuperTAA {
    struct Settings {
        uint32_t scale{2};             // history texels per fovea pixel and axis: 1 or 2
        float filter_scale{1.0f};      // rebuild of the current frame: its Gaussian sigma (see sigma_px) times this
        float stability{5.0f};         // accumulated weight a settled history holds (higher = steadier, slower)
        float clip_gamma{1.0f};        // neighbourhood clip: mean +- gamma * sigma
        float motion_reduce{0.25f};    // history weight lowered by up to this fraction at 24 px of motion per frame
        float depth_reject{0.10f};     // hidden in the last frame: something nearer by more than this fraction (0 = off)
        float history_sharpness{0.5f}; // history resampling: Catmull-Rom plus this much of its difference to bilinear
        float sharpen{0.30f};          // RCAS on the result (0 = off)
        bool luma_weight{true};        // blend weighted by 1 / (1 + luma)
        uint32_t dering{1};            // history resampling kept in the range of the 4 texels around: 0 off, 1 all
                                       // channels, 2 by the green channel (one read instead of three)
        bool nearest_sample{false};    // the current frame from the nearest sample only (else the 2 x 2 around it)
        bool box_resolve{true};        // scale 2: the fovea pixel = the mean of its 2 x 2 texels (else Lanczos 2 over 8 x 8)
        // ---- following changes (see the head of this file) ----
        bool follow{true};             // off: every pixel keeps the full weight (steadier, and what changes without
                                       // motion vectors takes several frames to show)
        float follow_threshold{1.0f};  // the bands of the test times this (lower = follows sooner, higher = steadier)
        float follow_memory{0.5f};     // the weight the history keeps where the picture changed
        float fl_k{2.0f};              // at once: fl_alln of the 9 samples differ from the history the same way by more
        float fl_b0{0.02f};            // than k * noise + b0 (perceptual luma)
        uint32_t fl_alln{8};
        float fl_kc{1.5f};             // confirmed: d two frames in a row the same way, over kc * noise + b0c, with
        float fl_b0c{0.012f};          // at least fl_count of the 9 samples
        uint32_t fl_count{5};
        float fl_cap{0.04f};           // the noise believed at most
        float fl_rate{0.125f};         // rate of the noise m
        float fl_rate_b{0.5f};         // rate of the fast mean b
        float fl_sde{0.7f};            // both bands at least this times the samples' scatter (the deviation of e)
        // ---- protection of what one frame's samples cannot show (see the head of this file) ----
        bool protect{true};            // off: fix77 (the frame's box only; thin bright things and stochastic shadows flicker)
        float protect_range{1.5f};     // the clip box wider by this times the samples' scatter
        float protect_spark{2.5f};     // the frame's sample no farther from the history than this times the scatter (0 = off)
        float protect_memory{1.5f};    // the history's weight limit times this where it is protected
        float af_spark_floor{0.02f};   // ... the sample's limit at least this (perceptual luma)
        float af_rate{0.125f};         // rate of the mean and the variance of e
        float af_t0{0.01f};            // deviation of e from which / at which the protection is full
        float af_t1{0.05f};
        float edge0{0.08f};            // change of (fetch motion - own motion), px, from one frame to the next from
        float edge1{0.25f};            // which / at which the protection is released
        float rel_decay{0.8f};         // what is left of a release a frame later
        // [fix81] the scatter's statistics take a sample no farther from their mean than af_clip deviations (as they were
        // before the frame) + af_clip_floor: one odd frame (a fly crossing the pixel) alone does not protect. Used with
        // `arrive` (0 = off: fix80).
        float af_clip{8.0f};
        float af_clip_floor{0.03f};
        // ---- things that arrive (see the head of this file; needs the depth) ----
        bool arrive{true};             // off: fix78 (what a fly leaves behind is protected: a dotted trail)
    };

    struct Params {
        Settings s{};
        uint32_t eye{0};          // history slot
        float mv_scale_x{0.5f};   // motion vector -> UV offset (current + mv * scale = previous position)
        float mv_scale_y{-0.5f};
        float jitter_x{0.0f};     // this frame's jitter in pixels as the upscaler gets it (the sample of pixel q shows the
        float jitter_y{0.0f};     // point at q + 0.5 - jitter)
        bool reset{false};        // no history this frame
        bool reverse_z{true};     // nearest = largest depth value
        // distance (m) from a depth value: |(k0 * z + k1) / (k2 * z + k3)|; depth_k_valid = false -> no disocclusion test
        float depth_k[4]{};
        bool depth_k_valid{false};
        // debug picture over the fovea image (not saved): 0 none, 1 protected (green), protection released (red), no
        // history (blue) and a thing that arrived (white), 2 the noise believed (0.05 = full yellow), 3 accumulated weight of the history (8 = white),
        // 4 motion of the nearest surface (red: 0..8 px per frame), 5 the scatter of the samples (0.10 = full)
        uint32_t debug_view{0};
        // debug: the left part (x < split_x fovea pixels) of the fovea image taken from `split_src` (same size and format
        // as `out`, resting in UNORDERED_ACCESS: the other anti-aliasing's result), with a line between; 0 = off
        uint32_t split_x{0};
        ID3D12Resource* split_src{nullptr};
        // the constants of this dispatch kept as text (last_constants; a capture's header)
        bool keep_constants{false};
        // [UI MOTION fix125 22.11.2026] the interface's own motion (UiMotion.hpp): the view's copies without and with GUI
        // (the colour's size, resting in COMMON) and the view's block of the motion (UiMotion::write).
        // With all three the history of a pixel where the interface has detail of its own is fetched along the
        // interface's motion instead of the scene's motion vectors; without them: the plain pass.
        ID3D12Resource* ui_hud{nullptr};
        ID3D12Resource* ui_fin{nullptr};
        UiMotion::Block ui_motion{};
    };

    // color / motion / depth: engine copies at the render size resting in in_state (depth may be null: no dilation, no
    // disocclusion test). out: the fovea image (UAV-able, non-SRGB), resting in UNORDERED_ACCESS, every
    // pixel written. Histories and statistics (per eye) are owned here.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
        D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, const Params& p);
    // pipelines and images made ahead of the first dispatch (render size, fovea image size, scale)
    bool prepare(ID3D12Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t scale);
    void reset();      // pipelines and images released (after the command lists using them are done)
    void invalidate(); // histories start again with the next frame
    const char* last_error() const { return m_last_error; }
    // [fix80 17.11.2026] how many views this instance keeps histories for: 2 (the fovea views, the default) or 1 (the
    // shared wide view). Set before the first prepare / dispatch; the images of the views left out are not made.
    void set_slots(uint32_t n) { m_slots = n >= 2 ? 2u : 1u; }
    // [UI MOTION fix125] the analyze pass with the interface's motion made with the others (prepare); otherwise with the
    // first dispatch that asks for it
    void set_ui_motion(bool wanted) { m_um_wanted = wanted; }
    // whether the last dispatch ran with the interface's motion
    bool last_ui_motion() const { return m_last_ui_motion; }
    uint32_t slots() const { return m_slots; }

    // rebuild sigma in input pixels for `history texels per input pixel` (before filter_scale): 0.5 texel, within
    // 0.20 .. 0.35 px
    static float sigma_px(float texels_per_pixel) {
        const float s = texels_per_pixel > 0.0f ? 0.5f / texels_per_pixel : 0.35f;
        return s < 0.20f ? 0.20f : (s > 0.35f ? 0.35f : s);
    }

    // ---- debug (dumps, captures): the images of the last dispatch of `eye`, resting in UNORDERED_ACCESS; null before it.
    // The per-frame ones (linear, guide, box, encoded) are shared by both eyes: valid until the other eye's dispatch.
    ID3D12Resource* history(uint32_t eye) const { return m_last_hist[eye < 2 ? eye : 1]; }
    ID3D12Resource* history_before(uint32_t eye) const { return m_prev_hist[eye < 2 ? eye : 1]; }
    ID3D12Resource* stats(uint32_t eye) const { return m_last_stats[eye < 2 ? eye : 1]; }
    ID3D12Resource* stats_before(uint32_t eye) const { return m_prev_stats[eye < 2 ? eye : 1]; }
    ID3D12Resource* distances(uint32_t eye) const { return m_last_dist[eye < 2 ? eye : 1]; }
    ID3D12Resource* distances_before(uint32_t eye) const { return m_prev_dist[eye < 2 ? eye : 1]; }
    ID3D12Resource* linear() const { return m_lin.Get(); }
    ID3D12Resource* guide() const { return m_guide.Get(); }
    ID3D12Resource* box_centre() const { return m_boxc.Get(); }
    ID3D12Resource* box_extent() const { return m_boxe.Get(); }
    ID3D12Resource* encoded() const { return m_enc.Get(); }
    uint32_t history_w() const { return m_hist_w; }
    uint32_t history_h() const { return m_hist_h; }
    // the constants of the last dispatch as text (one line per pass; for the capture's header)
    const std::string& last_constants() const { return m_last_constants; }

    // the constant blocks as the shaders declare them (root constants; checked against the shaders' reflection by the GPU
    // test)
    struct AnalyzeK {
        uint32_t size[2];
        float mv_scale[2];
        float depth_k[4];
        uint32_t flags; // 1 reset, 2 reverse z, 4 depth (dilation), 8 decode SRGB, 16 follow changes, 32 distance test, 64 protection,
                        // 128 things that arrive
        float reject;
        float gamma;
        float w_max;    // the weight a settled history holds
        float jitter[2];
        float k;        // at once: band k * noise + b0
        float b0;
        float kc;       // confirmed: band kc * noise + b0c
        float b0c;
        float cap;      // the noise believed at most
        float rate;     // rate of the noise
        float rate_b;   // rate of the fast mean
        float w_short;  // the weight the history keeps where the picture changed
        uint32_t count; // confirmed: at least this many of the 9 samples
        uint32_t alln;  // at once: at least this many of the 9 samples
        float fl_sde;   // both bands at least this times the deviation of e
        float af_rate;  // rate of the mean and the variance of e
        float af_t0;    // deviation of e from which / at which the protection is full
        float af_t1;
        float af_mem;   // the weight limit times this at full protection
        float edge0;    // change of (fetch motion - own motion) from which / at which the protection is released
        float edge1;
        float rel_decay;
        float af_clip;  // [fix81] the scatter's sample limit: this times the deviation of e + af_floor (0 = none)
        float af_floor;
    };

    struct AccumulateK {
        uint32_t in_size[2];
        uint32_t hist_size[2];
        float jitter[2];
        float k;            // 0.5 / sigma^2
        float w_cap;        // the most weight the history ever holds
        float motion_reduce;
        float extra;
        uint32_t flags;     // 1 reset, 2 luma weight, 4 dering (all channels), 8 dering (green), 16 nearest sample
        float relax;        // protected: the clip box wider by this times the scatter
        float wins;         // protected: the sample within this times the scatter of the history (0 = off)
        float wins_floor;
        uint32_t pad[2];    // (the shader's block is a multiple of 16 bytes)
    };

    struct ResolveK {
        uint32_t src_size[2];
        uint32_t dst_size[2];
        uint32_t flags;     // 1 encode SRGB, 4 = 2 : 1 Lanczos, 8 = 2 : 1 mean of 2 x 2
        float sharp;
        uint32_t debug;     // sharpen pass: debug view (Params::debug_view)
        uint32_t split_x;   // sharpen pass: split (Params::split_x)
        float lz[4];        // encode pass: Lanczos pairs (position a, weight a, position b, weight b)
    };

    static constexpr uint32_t ANALYZE_K = sizeof(AnalyzeK) / 4;
    static constexpr uint32_t ACCUMULATE_K = sizeof(AccumulateK) / 4;
    static constexpr uint32_t RESOLVE_K = sizeof(ResolveK) / 4;

    // the shaders' source (the GPU test compiles and reflects them)
    static const char* analyze_source();
    static std::string analyze_ui_motion_source(); // [UI MOTION fix125] the analyze pass with the interface's motion
    static const char* accumulate_source();
    static const char* resolve_source();

private:
    // two dispatches per frame (the two fovea views), lists three frames deep
    static constexpr uint32_t RING_COUNT = 16;
    // images replaced by others (the history's scale changed in the menu) are kept for this many dispatches: the
    // command lists of the last frames may still use them
    static constexpr uint32_t RETIRE_DISPATCHES = 24;
    // analyze: t0 colour, t1 motion, t2 depth, t3 last statistics, t4 last distances and release, t5 last history,
    //          u0 linear, u1 guide, u2 box centre, u3 box extent, u4 statistics, u5 distances and release
    // accumulate: t0 linear, t1 guide, t2 box centre, t3 box extent, t4 last history, t5 statistics, u0 history
    // encode: t0 history, u0 encoded (or the fovea image)
    // sharpen: t0 encoded, t1 guide, t2 history, t3 split source, t4 statistics, t5 distances and release, u0 fovea image
    //          [UI MOTION fix125] the pass with the interface's motion: t6 the view's copy without GUI, t7 with GUI,
    //          t8 the motion's block between them (9 SRVs, then the UAVs)
    static constexpr uint32_t D_ANALYZE = 15;
    static constexpr uint32_t D_ACCUMULATE = 7;
    static constexpr uint32_t D_ENCODE = 2;
    static constexpr uint32_t D_SHARPEN = 7;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = D_ANALYZE + D_ACCUMULATE + D_ENCODE + D_SHARPEN;

    bool setup(ID3D12Device* device);
    bool ensure_ui_motion(ID3D12Device* device); // [UI MOTION fix125]
    bool ensure_images(ID3D12Device* device, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t scale);

    ComPtr<ID3D12RootSignature> m_root_analyze{};
    ComPtr<ID3D12RootSignature> m_root_accumulate{};
    ComPtr<ID3D12RootSignature> m_root_encode{};
    ComPtr<ID3D12RootSignature> m_root_sharpen{};
    ComPtr<ID3D12PipelineState> m_pso_analyze{};
    ComPtr<ID3D12PipelineState> m_pso_accumulate{};
    ComPtr<ID3D12PipelineState> m_pso_encode{};
    ComPtr<ID3D12PipelineState> m_pso_sharpen{};
    ComPtr<ID3D12RootSignature> m_root_analyze_um{}; // [UI MOTION fix125] the analyze pass with the interface's motion
    ComPtr<ID3D12PipelineState> m_pso_analyze_um{};
    bool m_um_wanted{false};
    bool m_um_failed{false};
    bool m_last_ui_motion{false};
    ComPtr<ID3D12DescriptorHeap> m_heap{};

    ComPtr<ID3D12Resource> m_hist[2][2]{};  // [eye][ping-pong] R16G16B16A16_FLOAT, history size, rest in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_stats[2][2]{}; // [eye][ping-pong] R16G16B16A16_FLOAT, render size: noise m, fast mean b, mean and variance of e
    ComPtr<ID3D12Resource> m_dist[2][2]{};  // [eye][ping-pong] R16G16B16A16_FLOAT, render size: distance (m), the protection's release (above 1: a thing arrived), fetch motion - own motion
    ComPtr<ID3D12Resource> m_lin{};         // per frame, render size: rgb linear colour, a = perceptual luma
    ComPtr<ID3D12Resource> m_guide{};       // ... xy motion of the nearest surface (px), z the weight the history may keep (0 = no history), w protection
    ComPtr<ID3D12Resource> m_boxc{};        // ... clip box centre (YCoCg of the perceptual encoding), a = the 3x3 mean luma
    ComPtr<ID3D12Resource> m_boxe{};        // ... clip box extent
    ComPtr<ID3D12Resource> m_enc{};         // per frame, fovea image size: the encoded image before the sharpening
    // [GUARD fix101 20.11.2026] the encoded image of the other output size (see ensure_images: the periphery's image of the
    // wide view's size <-> of twice that keeps the history), kept so that the switch makes nothing
    ComPtr<ID3D12Resource> m_enc_other{};
    uint32_t m_other_w{0};
    uint32_t m_other_h{0};
    ID3D12Resource* m_last_hist[2]{};
    ID3D12Resource* m_prev_hist[2]{};
    ID3D12Resource* m_last_stats[2]{};
    ID3D12Resource* m_prev_stats[2]{};
    ID3D12Resource* m_last_dist[2]{};
    ID3D12Resource* m_prev_dist[2]{};
    uint32_t m_in_w{0};
    uint32_t m_in_h{0};
    uint32_t m_out_w{0};
    uint32_t m_out_h{0};
    uint32_t m_hist_w{0};
    uint32_t m_hist_h{0};
    uint32_t m_cur[2]{};
    bool m_valid[2]{};
    uint32_t m_slots{2};      // views with histories (set_slots)
    uint32_t m_slots_made{0}; // ... the images were made for
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
    std::string m_last_constants{};
    std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> m_retired{}; // image, dispatches left (see RETIRE_DISPATCHES)
};

static_assert(sizeof(SuperTAA::AnalyzeK) == 34 * 4);
static_assert(sizeof(SuperTAA::AccumulateK) == 16 * 4);
static_assert(sizeof(SuperTAA::ResolveK) == 12 * 4);

} // namespace d3d12
