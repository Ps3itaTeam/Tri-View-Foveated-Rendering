// FoveaComposite.hpp -- the GPU passes of the quad-view foveated rendering (multipass, see vr/QuadView.hpp):
//   LightTAA       temporal anti-aliasing of the wide view(s) at the render size
//   MonoPeriphery  the shared wide view reprojected to one eye with its depth
//   StableDepth    [fix81] the wide view's distances for that reprojection, steady from frame to frame
//   WideUpscale    spatial upscale of an eye's periphery to the eye image
//   FoveaTAA       the mod's temporal anti-aliasing of a fovea view (instead of the upscaler plugin)
//   FoveaComposite the fovea blended into the eye image (with the exposure gain)
//   UiClean / UiCurve / MonoUi  the interface taken out of the views and put together again per eye
//   UiSteady       [fix86] the wide view's interface for the periphery collected over the frames
//   GuiMap / EyeCompose  [GPU 29.10.2026] where the wide view has GUI; one pass per eye doing what WideUpscale,
//                  FoveaComposite and MonoUi do with the shared periphery
//
// All run on the TemporalUpscaler's command lists as compute passes; every pass documents the states its inputs rest
// in and returns them in the same states. Shaders are compiled at runtime (D3DCompile, cs_5_0).
#pragma once

#include <array>
#include <functional>
#include <string>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include <d3d12.h>

#include "ComPtr.hpp"
#include "utility/ReleaseBuild.hpp" // [RELEASE fix151] REVR_RELEASE (prepare_dense)
#include "UiMotion.hpp" // [UI MOTION fix125]

namespace d3d12 {
// [SHADER FILES fix110 20.11.2026] where the compiled shaders are kept between sessions (FoveaComposite.cpp, compile_cached)
void set_shader_cache_dir(const std::filesystem::path& dir);


struct FoveaComposite {
    struct Params {
        int32_t rect_x{0};      // fovea rectangle in the eye image (pixels)
        int32_t rect_y{0};
        uint32_t rect_w{0};
        uint32_t rect_h{0};
        float feather_px{64.0f}; // width of the blend towards the rectangle border
        bool debug_border{false};
        // scale the fovea so its mean brightness matches the same area of the periphery: the fovea camera has its own
        // automatic exposure (smoothed per eye slot, measured on the GPU every frame)
        bool match_brightness{false};
        uint32_t gain_slot{0};
        float gain_rate{0.2f};
    };

    // wide / fovea: eye image and fovea image in D3D12_RESOURCE_STATE_UNORDERED_ACCESS; returned in the same state.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* wide, ID3D12Resource* fovea, const Params& p);
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 4; // t0 wide, t1 fovea, u0 target, u1 gain

    bool setup(ID3D12Device* device);
    bool ensure_target(ID3D12Device* device, ID3D12Resource* wide);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12PipelineState> m_measure_pso{};
    ComPtr<ID3D12Resource> m_gain{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};

    ComPtr<ID3D12Resource> m_target{};
    uint32_t m_target_w{0};
    uint32_t m_target_h{0};
    DXGI_FORMAT m_source_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT m_uav_format{DXGI_FORMAT_UNKNOWN};

    bool m_failed{false};
    const char* m_last_error{""};
};

// [PERIPHERY 22.10.2026] Cheap spatial upscale of the wide view into the eye-sized image (Catmull-Rom, clamped to
// the 2x2 neighbourhood so it does not ring). Used when the periphery does not get the temporal upscaler at the
// full eye size: its input is either a small upscaler output (UAV) or the engine's render-size colour.
struct WideUpscale {
    // src rests in src_state (UNORDERED_ACCESS or PIXEL_SHADER_RESOURCE), dst in UNORDERED_ACCESS (UAV-able,
    // non-SRGB format); both are returned in those states. Every pixel of dst is written.
    // encode_srgb: -1 = when the source view decodes SRGB, 0/1 = forced (a float source holding linear values)
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state, ID3D12Resource* dst,
        int encode_srgb = -1);
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 2; // t0 source, u0 destination

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [PERIPHERY 22.10.2026] Very cheap temporal anti-aliasing of the periphery at the render size, instead of a plugin
// pass: reprojects its own history with the engine's motion vectors (taken from the nearest depth in 3x3), rebuilds
// the current frame at the pixel centre from its jittered samples, clips the history to the 3x3 colour neighbourhood
// (YCoCg mean +- 1.25 sigma) and blends. Kills most of the flicker of the raw render for ~0.05 ms.
struct LightTAA {
    struct Params {
        float mv_scale_x{0.5f};  // motion vector -> UV offset (current + mv * scale = previous position)
        float mv_scale_y{-0.5f};
        float feedback{0.90f};   // weight of the history
        bool reset{false};       // no history this frame
        bool dilate{true};       // motion vector of the nearest surface in 3x3 (needs depth)
        bool reverse_z{true};    // nearest = largest depth
        // this frame's jitter in pixels as the upscaler gets it (the sample of pixel q shows the point at q + 0.5 - jitter);
        // reconstruct: rebuild the frame at the pixel centre from the samples around it; anti_flicker: blend weighted by
        // 1 / (1 + luma)
        float jitter_x{0.0f};
        float jitter_y{0.0f};
        bool reconstruct{true};
        bool anti_flicker{true};
        bool tiled{true}; // [GPU 29.10.2026 round 3] 16 x 16 pixels per group with the neighbours in shared memory (same result)
        uint32_t tile{16}; // [GPU 31.10.2026] the tiled pass's group: 16 or 32 pixels per axis (same result)
        bool halo_serial{false}; // [GPU 01.11.2026] test variant: the halo read in two rounds, each awaited (before 01.11)
        // [CALM fix100 20.11.2026] the calm variant (see calm_resolve in the shader): the light TAA with the protection of
        // what one frame's samples cannot show. aux_in / aux_out: its second history, two images of the output's size
        // (R16G16B16A16_FLOAT, at rest in UNORDERED_ACCESS like the histories), swapped by the caller with them.
        bool calm{false};
        ID3D12Resource* aux_in{nullptr};
        ID3D12Resource* aux_out{nullptr};
        // [GUARD fix101 20.11.2026] the calm variant taking over from another pass: history_in is that pass's image of the
        // frame before (display encoded; any size, read by its place), the statistics start from zero in this frame
        bool calm_fresh{false};
    };

    bool prepare(ID3D12Device* device); // [GUARD fix101] the passes made ahead

    // color / motion / depth rest in in_state (engine copies: PIXEL_SHADER_RESOURCE); history_in / out are ours and rest
    // in UNORDERED_ACCESS (R16G16B16A16_FLOAT, render size). depth may be nullptr (no dilation).
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
        D3D12_RESOURCE_STATES in_state, ID3D12Resource* history_in, ID3D12Resource* out, const Params& p);
    void reset();
    const char* last_error() const { return m_last_error; }
    bool color_is_linear() const { return m_color_linear; } // the colour view decoded SRGB -> the result is linear

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 7; // t0 colour, t1 motion, t2 depth, t3 history, t4 second history, u0 out, u1

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12PipelineState> m_pso_tiled{};
    // [GPU 31.10.2026] test variants made on their first use (null: not usable): [0] 32 x 32 groups, [1] (01.11.2026) the
    // halo read in two rounds, each awaited
    ComPtr<ID3D12PipelineState> m_pso_alt[2]{};
    bool m_alt_failed[2]{};
    ID3D12PipelineState* alt_pso(ID3D12Device* device, int k);
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    bool m_color_linear{false};
    const char* m_last_error{""};
};

// [FOVEA TAA 28.10.2026] Temporal anti-aliasing of the fovea views (instead of an upscaler pass there). Two passes:
//   accumulate (render size): the current frame is rebuilt at each pixel's centre from its jittered 3x3 samples
//     (Gaussian), the own history is fetched where the engine's motion vectors say the point was (motion of the
//     nearest surface in 3x3, history read with a 5-tap Catmull-Rom), clipped to the 3x3 colour neighbourhood (YCoCg,
//     mean +- gamma * sigma) and blended. A pixel that was hidden in the last frame (something nearer stood there,
//     from the distances of the last frame) or lies outside the last frame drops its history and starts from a slightly
//     softer rebuild of the current frame; a fresh history averages its first frames (1/n) before the fixed weight.
//     The alpha channel of the history counts the frames accumulated.
//   resolve (fovea image size): the history is copied (or resampled, Catmull-Rom without ringing) into the fovea image
//     and sharpened with RCAS (AMD FidelityFX contrast adaptive sharpening, the FSR 1 variant), encoded to SRGB when
//     the colour view decoded it.
// The fovea views share nothing with the periphery: no image of it goes into the fovea.
struct FoveaTAA {
    struct Settings {
        float feedback{0.90f};      // weight of the settled history (higher = steadier, softer in motion)
        float clip_gamma{1.0f};     // neighbourhood clip: mean +- gamma * sigma (lower = less ghosting, more shimmer)
        float filter_radius{0.40f}; // rebuild of the current frame: Gaussian sigma in pixels (0 = the own sample only)
        float new_softness{0.5f};   // pixels without history: their rebuild widens towards sigma 1 px (0 = like the others)
        float motion_reduce{0.25f}; // history weight lowered by up to this fraction on fast motion (24 px per frame)
        float depth_reject{0.10f};  // hidden in the last frame: something nearer by more than this fraction (0 = off)
        float sharpen{0.30f};       // RCAS on the result (0 = off, 1 = the strongest)
        bool dilate{true};          // motion vector of the nearest surface in 3x3
        bool catmull_rom{true};     // history read with Catmull-Rom (sharper) instead of bilinear
        bool luma_weight{true};     // blend weighted by 1 / (1 + luma): bright sparks flicker less
        bool fast_converge{true};   // a fresh history averages its first frames (1/n) before the fixed weight
    };

    struct Params {
        Settings s{};
        uint32_t eye{0};         // history slot
        float mv_scale_x{0.5f};  // motion vector -> UV offset (current + mv * scale = previous position)
        float mv_scale_y{-0.5f};
        float jitter_x{0.0f};    // this frame's jitter in pixels as the upscaler gets it (the sample of pixel q shows the
        float jitter_y{0.0f};    // point at q + 0.5 - jitter)
        bool reset{false};       // no history this frame
        bool reverse_z{true};    // nearest = largest depth value
        // distance (m) from a depth value: |(k0 * z + k1) / (k2 * z + k3)|; depth_k_valid = false -> no disocclusion test
        float depth_k[4]{};
        bool depth_k_valid{false};
        bool tiled{true}; // [GPU 29.10.2026 round 3] 16 x 16 pixels per group with the neighbours in shared memory (same result)
        uint32_t tile{16}; // [GPU 31.10.2026] the tiled passes' group: 16 or 32 pixels per axis (same result)
        bool halo_serial{false}; // [GPU 01.11.2026] test variant: the halo read in two rounds, each awaited (before 01.11)
    };

    // color / motion / depth: engine copies at the render size resting in in_state (depth may be null: no dilation and no
    // disocclusion test). out: the fovea image (UAV-able, non-SRGB), resting in UNORDERED_ACCESS, every pixel written.
    // The histories (per eye, render size) are owned here.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
        D3D12_RESOURCE_STATES in_state, ID3D12Resource* out, const Params& p);
    // [TAA READY 06.11.2026] pipelines and histories (w x h) made ahead of the first dispatch: they used to be made on the
    // first switch to the TAA (GPU guard, log of 06.11 15:02: a frame of 0.56 s, SteamVR "not responding")
    bool prepare(ID3D12Device* device, uint32_t w, uint32_t h);
    void reset();      // pipelines and histories released (after the command lists using them are done)
    void invalidate(); // histories start again with the next frame
    const char* last_error() const { return m_last_error; }
    ID3D12Resource* history(uint32_t eye) const { return m_last_hist[eye < 2 ? eye : 1]; } // last written (dump)

private:
    static constexpr uint32_t RING_COUNT = 8;
    // accumulate: t0 colour, t1 motion, t2 depth, t3 history, t4 last distances, u0 history, u1 distances;
    // resolve: t0 history, u0 fovea image
    static constexpr uint32_t DESCRIPTORS_ACCUMULATE = 7;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = DESCRIPTORS_ACCUMULATE + 2;

    bool setup(ID3D12Device* device);
    bool ensure_histories(ID3D12Device* device, uint32_t w, uint32_t h);

    ComPtr<ID3D12RootSignature> m_root_acc{};
    ComPtr<ID3D12RootSignature> m_root_res{};
    ComPtr<ID3D12PipelineState> m_pso_acc{};
    ComPtr<ID3D12PipelineState> m_pso_res{};
    ComPtr<ID3D12PipelineState> m_pso_acc_tiled{}; // round 3 (null: not compiled, the passes above are used)
    ComPtr<ID3D12PipelineState> m_pso_res_tiled{};
    // [GPU 31.10.2026] test variants made on their first use (null: not usable): [0] 32 x 32 groups, [1] (01.11.2026) the
    // halo read in two rounds, each awaited
    ComPtr<ID3D12PipelineState> m_pso_acc_alt[2]{};
    ComPtr<ID3D12PipelineState> m_pso_res_alt[2]{};
    bool m_alt_failed[2]{};
    bool alt_psos(ID3D12Device* device, int k);
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_hist[2][2]{}; // [eye][ping-pong] R16G16B16A16_FLOAT, rest in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_dist[2][2]{}; // [eye][ping-pong] R16_FLOAT distances (m), rest in UNORDERED_ACCESS
    ID3D12Resource* m_last_hist[2]{};
    uint32_t m_hist_w{0};
    uint32_t m_hist_h{0};
    uint32_t m_cur[2]{};
    bool m_valid[2]{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [MONO PERIPHERY 27.10.2026] Builds one eye's periphery from the single wide view rendered between the eyes. The two
// cameras share their orientation and only differ by a shift along x, so every output ray stays on its row of the wide
// image: a surface at distance d appears shifted by (pixels per tangent * eye offset) / d. Per output pixel the source
// columns from the largest shift (nearest possible surface) down to zero are tested for the one whose own distance puts
// it on this pixel; the first hit is the nearest surface and is sampled at its exact (fractional) position. Where no
// column fits (the eye sees past an edge the wide view could not see behind) the background just beyond the near
// object's edge continues into the gap mirrored at that edge (slightly blurred) -- never the object itself, which would
// show it twice, and no longer one background column repeated across the gap, whose streaks drew a second outline next
// to hands and weapon. Rays that need a column past the wide image's border (near things at each eye's outer edge) are
// mirrored back into the image the same way.
struct MonoPeriphery {
    struct Params {
        // output pixel (x + 0.5, y + 0.5) -> source pixel of the same ray at infinite distance
        float x_scale{1.0f};
        float x_offset{0.0f};
        float y_scale{1.0f};
        float y_offset{0.0f};
        // source shift (pixels * metres) of a surface at distance d: shift = disparity / d (signed: this eye's offset)
        float disparity{0.0f};
        float dist_min{0.12f};      // nearest distance searched (m)
        uint32_t max_shift{64};     // search range (source pixels)
        // distance from the depth value: d = |(a * z + b) / (c * z + e)| (rows 2 and 3 of the inverse projection)
        float depth_a{0.0f};
        float depth_b{0.0f};
        float depth_c{0.0f};
        float depth_e{1.0f};
        bool mark_holes{false};     // debug: pixels without a fitting column tinted magenta
        // [HOLES 02.11.2026] the gaps beside near things filled with the background beside them moved over by the gap's
        // width (stays with the background) and the near thing's colour kept out of the background's edge column
        bool fill_anchored{false};
        // [OUTLINES fix84 18.11.2026] with ss 2 and the stable depth's image as depth (StableDepth::as_depth_of): each of a
        // ray's four pixels on the surface its own ray meets, the outline between two surfaces to a fraction of a texel
        // from the mean of 1 / distance (the shader's soft_inv). Without it: one surface per ray (fix80 - fix83).
        bool soft_outlines{false};
        // [PERIPHERY 2X fix80 17.11.2026] pixels written per ray searched and axis: 1, or 2 with a colour image of twice
        // the wide view's size (the supersampling TAA's image) and an `out` of twice the eye's share of the wide view.
        // The search runs once per ray on the wide view's own grid (colour size / ss) as with 1; each of the 2 x 2 pixels
        // reads the colour where its own ray lands on the surface found. All the values above stay in columns of that
        // grid. Taken from p[0] (the same for both eyes); needs ss_ready().
        uint32_t ss{1};
    };

    // the search used (all give the same image): the original single pass; the bounded search with the leftover pixels
    // in a second pass as in round 2; the same with round 3's block minima and second pass; [GPU 31.10.2026 night] round 3
    // with the first pass reading every block minimum from the texture (the build of 31.10 22:51, test variant)
    enum class Search { Original, TwoPassRound2, TwoPassRound3, TwoPassRound3BminTexture };

    // [GPU 29.10.2026] Both eyes in one call (the inputs change state once). color / depth: the wide view's image (resting
    // in color_state) and its depth (engine copy, depth_state), both at the render size; out[eye]: that eye's periphery
    // image (UAV-able colour format, rests in UNORDERED_ACCESS), every pixel written; p[eye] its parameters.
    // stamp(k) is called after the block minima (k = 0), the first pass (1) and the second pass (2), for GPU timing.
    // frame: the render frame (the leftover counts are read back for the log, see collect()).
    bool reproject(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
        ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource* const out[2], const Params p[2], Search search, uint64_t frame,
        const std::function<void(int)>& stamp);
    bool fast_available() const { return m_pso_bounded != nullptr; } // after the first call
    // [PERIPHERY 2X fix80 17.11.2026] the passes for Params::ss = 2 (made on the first call; they need the faster search):
    // false = not usable, the caller keeps the periphery at the wide view's size
    bool ss_ready(ID3D12Device* device);
    // CPU, once per frame: reads the leftover counts of finished frames; share of each eye's pixels that went through the
    // second pass, averaged over ~90 frames (-1 = none yet)
    void collect(uint64_t frame);
    double second_pass_share(int eye) const { return m_share[eye < 2 ? eye : 1]; }
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 32; // up to five dispatches per frame, lists three frames deep
    // t0 colour, t1 depth, u0 out, u1 block minima, u2 list of the leftover pixels, u3 their counts
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 6;

    bool setup(ID3D12Device* device);
    bool ensure_buffers(ID3D12Device* device, uint32_t src_w, uint32_t src_h, ID3D12Resource* const out[2]);
    void write_constants(ID3D12GraphicsCommandList* cmd, uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h, const Params& p, uint32_t flags);
    D3D12_GPU_DESCRIPTOR_HANDLE write_table(ID3D12Device* device, ID3D12Resource* color, DXGI_FORMAT color_format, ID3D12Resource* depth,
        DXGI_FORMAT depth_format, ID3D12Resource* out, DXGI_FORMAT out_format, int eye);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};          // the original pass
    ComPtr<ID3D12PipelineState> m_pso_bmin{};     // block minima (and the counts set to zero)
    ComPtr<ID3D12PipelineState> m_pso_bounded{};  // first pass of the faster search
    ComPtr<ID3D12PipelineState> m_pso_holes{};    // its second pass
    ComPtr<ID3D12PipelineState> m_pso_bmin_cols{};  // round 3: block minima one thread per column
    ComPtr<ID3D12PipelineState> m_pso_bounded_tex{}; // the first pass with the block minima from the texture (test variant)
    ComPtr<ID3D12PipelineState> m_pso_bounded_ss{};  // [PERIPHERY 2X fix80] the two passes writing 2 x 2 pixels per ray
    ComPtr<ID3D12PipelineState> m_pso_holes_ss{};
    bool m_ss_failed{false};
    ComPtr<ID3D12Resource> m_bmin{};              // R32_FLOAT, (src_w + 31) / 32 x src_h, rests in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_holes[2]{};          // per eye: one uint per output pixel, rests in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_count{};             // both eyes' counts (RAW), rests in UNORDERED_ACCESS
    uint32_t m_bmin_w{0};
    uint32_t m_bmin_h{0};
    uint32_t m_holes_n[2]{};
    // resources replaced on a size change, kept until the command lists that may still use them are done
    std::vector<ComPtr<ID3D12Resource>> m_retired{};
    uint32_t m_retired_age{0};
    // the leftover counts read back for the log
    static constexpr uint32_t COUNT_FRAMES = 8;
    ComPtr<ID3D12Resource> m_count_rb{};
    bool m_count_rb_failed{false};
    const uint32_t* m_count_mapped{nullptr};
    std::array<uint64_t, COUNT_FRAMES> m_count_frame{};
    uint32_t m_count_px[COUNT_FRAMES][2]{};
    double m_share_sum[2]{};
    uint32_t m_share_n{0};
    double m_share[2]{-1.0, -1.0};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [STABLE DEPTH fix81 17.11.2026] The shared wide view's distances for the reprojection (MonoPeriphery), steady from frame
// to frame. The wide view's depth is sampled with the frame's jitter like its colour; the colour goes through a temporal
// pass and stands still, the depth went to the reprojection as it was. At the outline of a nearer thing a pixel's sample is
// the near surface in some frames and the far one in others, and the reprojection moves the pixel by disparity / distance:
// the outline's pixels (and the gap filled beside them) jumped between the near thing's place and the background's from
// frame to frame, by up to several pixels of the wide view (four eye pixels each), whatever anti-aliasing the periphery
// had. On RE7 captures of 17.11.2026 put through the game's reprojection (tests/supertaa/periphery/reproject_sequence.py):
// the periphery's flicker within 24 eye pixels of a depth step 2.9 and 3.3 times that of the same image without
// reprojection; with this pass 1.3 and 2.3 times (the rest: outlines that cross a pixel while the head moves), away from
// the steps 0.9 times instead of 1.1 (steep near surfaces).
// Per pixel of the wide view, carried along the motion of the nearest surface in its 3 x 3:
//   the mean of 1 / distance over time (the last one held within what the 3 x 3 shows now): on a surface its 1 / distance
//   at the pixel's centre, at a step how much of the pixel the near surface covers;
//   what is handed on: without a step in the 3 x 3 that mean; at a step the sample of the 3 x 3 nearest to the mean (the
//   surface that covers most of the pixel), with a margin that keeps the surface the pixel was on, and that surface's
//   own 1 / distance steadied over time.
// The reference is tests/supertaa/periphery/stable_depth.py (the GPU test compares the pass with it).
struct StableDepth {
    struct Params {
        float mv_scale_x{0.5f};  // motion vector -> UV offset (current + mv * scale = previous position), as LightTAA's
        float mv_scale_y{-0.5f};
        // distance from the depth value: d = |(a * z + b) / (c * z + e)| (as MonoPeriphery::Params)
        float depth_a{0.0f};
        float depth_b{0.0f};
        float depth_c{0.0f};
        float depth_e{1.0f};
        float dist_min{0.12f};   // nearer counts as this (as the reprojection takes it)
        bool reset{false};       // no history this frame (the frame before had no pass)
        float rate{0.125f};      // of the means
        float step0{0.05f};      // a step: the 3 x 3's values of 1 / distance apart by this part of the nearest ...
        float step1{0.15f};      // ... to this (fully a step)
        float hysteresis{0.3f};  // of the step: the mean has to lie this much nearer to another surface than to the one
                                 // the pixel was on
    };

    // depth: the wide view's depth (resting in depth_state); motion: its motion vectors (motion_state; any size, read at
    // the depth's place). The result is kept inside (two images of the depth's size, R16G16_FLOAT, resting in
    // UNORDERED_ACCESS): image() after a dispatch that returned true.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state,
        ID3D12Resource* motion, D3D12_RESOURCE_STATES motion_state, const Params& p);
    // x = 1 / distance (metres), y = the mean. For MonoPeriphery as its depth (state UNORDERED_ACCESS) with the constants
    // of as_depth_of(): distance = |(0 z + 1) / (1 z + 0)|.
    ID3D12Resource* image() const { return m_valid ? m_image[m_cur].Get() : nullptr; }
    static void as_depth_of(MonoPeriphery::Params& p) {
        p.depth_a = 0.0f;
        p.depth_b = 1.0f;
        p.depth_c = 1.0f;
        p.depth_e = 0.0f;
    }
    void invalidate() { m_valid = false; } // the history starts again with the next dispatch
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 4; // t0 depth, t1 motion, t2 last result, u0 result
    static constexpr uint32_t RETIRE_FRAMES = 8;            // dispatches the images of another size are kept for

    bool setup(ID3D12Device* device);
    bool ensure_images(ID3D12Device* device, uint32_t w, uint32_t h);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_image[2]{};
    std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> m_retired{}; // images of the size before, dispatches since
    uint32_t m_w{0};
    uint32_t m_h{0};
    uint32_t m_cur{0};       // the image holding the last result
    bool m_valid{false};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [MENUS fix86 18.11.2026] The wide view's interface for the periphery, steady from frame to frame.
// RE7 test of fix85 (18.11.2026 20:4x, the save menu in the periphery, video with the head still): the menu's lines and
// letters were other pieces every frame, the same again every 4 frames (the jitter's four places); the alternating part
// of the picture was 5.3 per mille on the menu (99th percentile 114) against 1.5 on the air conditioner's grille behind
// it, which goes through the periphery's anti-aliasing. The interface does not: the eye pass takes it from the wide
// view's frame as it is (its colour and the two copies without / with GUI), one sample per 4 x 4 eye pixels. fix81 reads
// that frame where the jitter put the point, so the interface stands still, but a line a quarter of a pixel wide is still
// hit by this frame's samples in other places than by the next frame's.
// This pass collects the three images over the frames: each sample goes to the texels of an image of twice the size at
// the place it was taken (the frame's pixel middle less the jitter), with a weight by its distance from the texel's
// middle; the image of the frame before is fetched along the motion vectors (the panels stand in the world: a turn of
// the head moves them like the scene behind them) and keeps weight_max frames' worth. The eye pass gets the three
// results instead of the frame's images and no jitter to follow. The same arithmetic runs on all three, so where the
// copies were equal in every frame collected the results are equal as well (the eye pass's masks are 0 there).
// What changes in the interface (a cursor, another page) fades over in about weight_max frames; a fast turn of the head
// lowers the kept weight (motion_half pixels per frame halve it), so what does not move with the scene cannot trail far.
// GuiMap marks the blocks where this frame's copies differ, and the eye pass looks for interface only there: a thin line
// alone on the picture is in a frame's samples in one frame of four, its blocks were marked in those frames only. With
// the map given, the pass marks the blocks where its results differ as well (the map's own reach of 2 texels around a
// block), so what is collected shows in every frame.
struct GuiMap;

struct UiSteady {
    struct Params {
        float mv_scale_x{0.5f};  // motion vector -> UV offset (current + mv * scale = previous position), as StableDepth's
        float mv_scale_y{-0.5f};
        float jitter_x{0.0f};    // the frame's jitter in pixels of the images: a point of the steady image at p is at
        float jitter_y{0.0f};    // p + jitter in the frame (as EyeCompose::Params::wide_jitter)
        bool reset{false};       // no history this frame (the frame before had no pass)
        float weight_max{4.0f};  // the history's weight at most, in samples taken right at a texel's middle
        float sigma{0.3f};       // of a sample's weight by its distance from the texel's middle (pixels of the images)
        float motion_half{4.0f}; // pixels per frame at which the history keeps half its weight
        // [fix94 19.11.2026] Work only on the map's blocks around the interface (within 3 blocks of a block marked in the
        // last AROUND_HOLD frames) instead of the whole image: RE4 test of 19.11.2026, a small HUD in the picture (0.3 -
        // 1 % of the blocks): the pass took 0.32 ms of the frame for the whole image. Needs the map (gui_map). The results
        // are the same where the interface stands and the head is still; what is new in the picture (a piece of
        // interface where none was for over a second) is collected from that frame on, without the scene's history
        // behind it: it shows at once instead of fading in over some 8 frames. Blocks left out hold empty texels (the
        // results without and with GUI equal: no interface for whoever reads them).
        bool only_around{false};
        uint32_t around_hold{0}; // map stamps a block stays worked on after the last mark in its reach (0: AROUND_HOLD)
        // [UI MOTION fix125 22.11.2026] the wide view's block of the interface's own motion (UiMotion::write,
        // UiMotion.hpp): the history is fetched where the interface itself was instead of where the scene behind it
        // was. None: the scene's motion vectors (fix124).
        UiMotion::Block ui_motion{};
    };

    // color: the wide view's colour after the output pass (resting in color_state); hud / fin: the engine's copies without
    // and with GUI (the same size, resting in COMMON: read by implicit promotion like everywhere else); motion: the wide
    // view's motion vectors (motion_state; any size). The results are kept inside (three images of twice the size,
    // R16G16B16A16_FLOAT, alpha = the weight collected; two of each): color() rests in color_state as the frame's colour
    // does, hud() and fin() in NON_PIXEL_SHADER_RESOURCE (COPIES_STATE): the eye pass reads the frame's copies without a
    // transition (they rest in COMMON and are promoted), these it can read as they are. They hold what a view of the
    // input's format reads; of an sRGB colour the encoded values (the eye pass's view of the result is not sRGB, so it
    // does not encode them again).
    // gui_map: the map made of these copies in this frame (GuiMap::dispatch before this), or null: no blocks marked here.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state, ID3D12Resource* hud,
        ID3D12Resource* fin, ID3D12Resource* motion, D3D12_RESOURCE_STATES motion_state, const GuiMap* gui_map, const Params& p);
    ID3D12Resource* color() const { return m_valid ? m_image[m_cur][0].Get() : nullptr; }
    ID3D12Resource* hud() const { return m_valid ? m_image[m_cur][1].Get() : nullptr; }
    ID3D12Resource* fin() const { return m_valid ? m_image[m_cur][2].Get() : nullptr; }
    void invalidate() { m_valid = false; } // the history starts again with the next dispatch
    void reset();
    const char* last_error() const { return m_last_error; }
    bool last_ui_motion() const { return m_last_ui_motion; } // [UI MOTION fix125] the last dispatch ran with it
    static const char* source();              // the pass's text (tests)
    static std::string ui_motion_source();    // ... with the interface's motion

    static constexpr uint32_t SCALE = 2; // the results' size over the images'
    static constexpr D3D12_RESOURCE_STATES COPIES_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // hud() and fin() rest in it

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 14; // t0-2 colour, hud, fin; t3 motion; t4-6 last results; u0-2 results; u3 map;
                                                             // [fix94] u4, u5 the blocks worked on (this dispatch, the one before)
                                                             // [UI MOTION fix125] with the interface's motion: t7 its block
                                                             // between them (8 SRVs, then the UAVs)
    static constexpr uint32_t AROUND_HOLD = 120;             // [fix94] map stamps a block is worked on after the last mark in its reach
    static constexpr uint32_t RETIRE_FRAMES = 8;             // dispatches the images of another size are kept for

    bool setup(ID3D12Device* device);
    bool ensure_images(ID3D12Device* device, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES color_state);
    bool ensure_blocks(ID3D12Device* device, uint32_t bw, uint32_t bh); // [fix94]

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    // [fix94] only_around: the pass naming the blocks, the blocks' images (the last written: m_act_cur), the dispatch count
    // they hold, whether the last dispatch worked around the interface / left blocks out, whole images still to make
    ComPtr<ID3D12PipelineState> m_pso_around{};
    // [UI MOTION fix125] with the interface's own motion: the root signature with one more SRV, `main` reading the motion,
    // and `around` for that root signature
    ComPtr<ID3D12RootSignature> m_root_um{};
    ComPtr<ID3D12PipelineState> m_pso_um{};
    ComPtr<ID3D12PipelineState> m_pso_around_um{};
    bool m_last_ui_motion{false};
    ComPtr<ID3D12Resource> m_act[2]{};
    uint32_t m_act_w{0};
    uint32_t m_act_h{0};
    uint32_t m_act_cur{0};
    uint32_t m_seq{0};
    bool m_around_last{false};
    bool m_partial_last{false};
    uint32_t m_full_left{0};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_image[2][3]{};
    std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> m_retired{}; // images of the size before, dispatches since
    uint32_t m_w{0}; // of the results
    uint32_t m_h{0};
    D3D12_RESOURCE_STATES m_color_state{D3D12_RESOURCE_STATE_COMMON};
    uint32_t m_cur{0}; // the images holding the last results
    bool m_valid{false};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [MONO PERIPHERY 27.10.2026] Takes the interface out of a view's colour before the temporal pass sees it (the interface
// goes back per eye at the end, MonoUi). Interface pixels get the view without GUI instead, passed through the engine's
// output curve (UiCurve) so they match the colour around them.
// [UI CURVE 27.10.2026] Two ways to find the interface pixels:
//   the engine's copies with and without GUI differ (both through the output curve) -- the shared wide view
//   the colour itself differs from the copy without GUI through the curve -- the fovea views: their copy with GUI is not
//     reliable (test 27.10.2026: the left fovea's one held the right fovea's image, the engine reuses the target)
struct UiClean {
    // color: the view's colour after the output pass (engine copy, color_state); hudless / final: the engine's
    // PostMainTarget copies without / with GUI (same size, resting in COMMON; final may be null with mask_from_color);
    // curve: UiCurve::table() or null (plain SRGB curve); out: UAV-able copy of color's format, rests in UNORDERED_ACCESS
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
        ID3D12Resource* hudless, ID3D12Resource* final_tex, ID3D12Resource* out, ID3D12Resource* curve, bool mask_from_color,
        bool round3 = true, // round3: the fovea views' pass with the curve table in shared memory (same result)
        // [LIGHTS 11.11.2026] GuiMap::map() of this frame (resting in UNORDERED_ACCESS) and its stamp: with the interface
        // found from the colour, nothing is taken for interface in a frame where the wide view drew none
        ID3D12Resource* gui_map = nullptr, uint32_t gui_stamp = 0);
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 16; // up to three dispatches per frame (wide view, two fovea views)
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 6; // t0 colour, t1 hudless, t2 final, t3 curve, u0 out, u1 GuiMap

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12PipelineState> m_pso_lds{}; // round 3
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [MONO UI 27.10.2026] Puts the interface of one eye together with the shared periphery. The GUI is built once per frame
// and drawn in every scene layer with that layer's matrices: the shared wide view has it as seen from between the eyes
// (union frustum, at the periphery's low pixel density), each fovea view as its eye sees it inside its narrow frustum
// (sharp). Per pixel of the eye image:
//   inside the fovea rectangle: the fovea view's interface (exact for this eye; mapped from the fovea's own image)
//   outside: the wide view's interface, moved to this eye for the depth of what it belongs to -- an anchored element
//     (item icon) at its anchor's depth, everything else at the depth of the interface panels
//   across the blend band of the rectangle the two are mixed like the fovea image itself.
// Interface pixels: for the wide view where the engine's copies with and without GUI differ, for the fovea views where
// the colour differs from the copy without GUI through the output curve (UiCurve); the colour is the layer's colour after
// the output pass. An interface pixel of an element anchored in the world is only taken at its anchor's depth. The
// result is written into `out` (eye-sized, same format).
struct MonoUi {
    static constexpr uint32_t MAX_ANCHORS = 16;

    struct Params {
        float eye_tan[4]{};   // this eye's frustum: tangents left, right, bottom, top
        float wide_tan[4]{};  // the shared wide view's frustum
        float eye_x{0.0f};    // this eye's offset from the wide view's camera along x (m)
        float panel_depth{1.0f}; // depth of the interface panels (m)
        float rect[4]{};      // fovea rectangle in eye pixels: x, y, w, h (w = 0: no fovea interface)
        float feather_px{0.0f};
        uint32_t anchor_count{0};
        // anchored elements in the wide view: tangent x, tangent y, depth (m), radius (tangent; > 0: anchored at something in
        // the world, its pixels belong to it alone; < 0: a panel at another distance, it only claims pixels)
        float anchors[MAX_ANCHORS][4]{};
        // [ITEM BEAMS 03.11.2026] per anchor: reach across, above, below its anchor (tangents), 1 = the green beam over an
        // item (Gui_ui2130): only its green pixels go to that depth, each to one anchor (the closest across)
        float anchor_ext[MAX_ANCHORS][4]{};
        bool debug_tint{false}; // interface from the wide view tinted blue, from the fovea view green
        ID3D12Resource* curve{nullptr}; // the output curve (UiCurve::table(), null = plain SRGB)
        // [POST IN UI 28.10.2026] the eye images' post pass done here (d3d12::PostPass maths): sharpness 0..3 (0 = off),
        // saturation (1 = unchanged), brightness as exponent (1 = unchanged), contrast (1 = unchanged)
        bool post{false};
        float post_sharp{0.0f};
        float post_sat{1.0f};
        float post_gamma{1.0f};
        float post_contrast{1.0f};
        // [EDGE SOFTEN 11.11.2026] weight of the 4 neighbours' mean on high-contrast edges (0 = off) and the luma range
        // from which an edge counts
        float post_edge{0.0f};
        float post_edge_threshold{0.15f};
    };

    // eye_in rests in UNORDERED_ACCESS (read), out in UNORDERED_ACCESS (written); the colour copies in
    // PIXEL_SHADER_RESOURCE, the GUI copies (hudless / final) in COMMON. Either the centre triple or the fovea pair may be null.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* eye_in, ID3D12Resource* out,
        ID3D12Resource* centre_color, ID3D12Resource* centre_hud, ID3D12Resource* centre_fin,
        ID3D12Resource* fovea_color, ID3D12Resource* fovea_hud, const Params& p);
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    // t0 eye, t1-t3 centre colour/hud/final, t4-t6 fovea colour/hud/(hud again), t7 curve, u0 out
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 9;
    static constexpr uint32_t CB_SLOT_SIZE = 768; // [ITEM BEAMS 03.11.2026] was 512 (constants 704 / 608 bytes)

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_cb{};
    uint8_t* m_cb_mapped{nullptr};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [UI CURVE 27.10.2026] The engine's output pass, measured: it turns a layer's PostMainTarget (linear) into the colour that
// goes on (display-encoded, with its own curve). On the shared wide view both the PostMainTarget copy with GUI and the
// colour after the output pass are the same image, so every pixel is a sample of that curve. Per channel, 80 bins of a
// quarter stop from 2^-14: the mean output of each bin, gaps filled from the neighbours, kept rising, blended over a few
// frames. Used to compare a fovea view's colour with its copy without GUI (UiClean, MonoUi).
struct UiCurve {
    static constexpr uint32_t BINS = 80;

    // pmt: PostMainTarget copy with GUI (resting in COMMON); color: the same layer's colour after the output pass (resting
    // in color_state, read as stored). Records the measurement; the table rests in NON_PIXEL_SHADER_RESOURCE afterwards.
    // round3: 2 x 2 pixels per thread and the table built in parallel (same table)
    bool learn(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* pmt, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
        bool round3 = true);
    // BINS x 1 R32G32B32A32_FLOAT (rgb = output per channel), null until the first measurement
    ID3D12Resource* table() const { return m_measured ? m_table.Get() : nullptr; }
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 4; // t0 pmt, t1 colour, u0 sums, u1 table

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso_sum{};
    ComPtr<ID3D12PipelineState> m_pso_resolve{};
    ComPtr<ID3D12PipelineState> m_pso_sum4{};        // round 3
    ComPtr<ID3D12PipelineState> m_pso_resolve_par{}; // round 3
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_sums{};   // per bin and channel: sum of the output (x1023) and count, RAW
    ComPtr<ID3D12Resource> m_table{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_cleared{false};   // the sums were zeroed once (afterwards the resolve pass zeroes them)
    bool m_measured{false};  // the table holds a measurement
    bool m_failed{false};
    const char* m_last_error{""};
};

// [GPU 29.10.2026] Where the shared wide view's two GUI copies (without / with GUI) differ: one value per block of 8 x 8
// texels, the frame's stamp (see stamp()) when a texel of the block or of the 2 texels around it differs. Everywhere else
// both copies hold the same image, so the interface mask read there is exactly 0 and EyeCompose skips that work (most of
// the image, most of the time). The marked blocks are counted for the log. One thread per texel (the copies read once,
// in parallel); the stamps need no clearing.
struct GuiMap {
    // hud / fin: the copies (same size, resting in COMMON). The map rests in UNORDERED_ACCESS.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* hud, ID3D12Resource* fin);
    // the running count of marked blocks into the readback ring (later in the same list as dispatch)
    void copy_count(ID3D12GraphicsCommandList* cmd, uint64_t frame);
    // CPU: reads the counts of frames finished a while ago; share of marked blocks over the last ~90 frames (-1 = none yet)
    void collect(uint64_t frame);
    double marked_share() const { return m_share; }
    // [UI COPIES 30.10.2026] consecutive frames read back with at least 98 % of the blocks marked / with less than half marked
    uint32_t full_run() const { return m_full_run; }
    uint32_t calm_run() const { return m_calm_run; }
    uint32_t idle_run() const { return m_idle_run; } // [MENUS fix86] frames in a row (as counted) with no block marked by dispatch
    ID3D12Resource* map() const { return m_map.Get(); }
    uint32_t map_w() const { return m_map_w; }
    uint32_t map_h() const { return m_map_h; }
    // [LIGHTS 11.11.2026] the last texel of the extra row holds the frame's stamp when any block was marked (the wide
    // view drew interface somewhere); only with more than 64 columns (the beam results use the first 64)
    bool frame_texel_ok() const { return m_map_w > 64; }
    uint32_t gui_w() const { return m_gui_w; }
    uint32_t gui_h() const { return m_gui_h; }
    uint32_t stamp() const { return m_stamp; } // the marked blocks of the last dispatch hold this value
    void reset();
    const char* last_error() const { return m_last_error; }

    // [BEAM FIT 03.11.2026] where the wide view drew each item beam: per beam anchor a box of the copies (texels) around
    // it is searched for the beam's green; the column found goes into the map's extra row (row map_h(): x tangent, lowest
    // point, texels, stamp at 4 * index), which EyeCompose reads to put the beam over its item. After dispatch, in a list
    // that runs before the eye passes; the copies rest in COMMON, the map in UNORDERED_ACCESS.
    struct BeamFit {
        uint32_t count{0};          // beam anchors (at most 16)
        float tan_per_texel[2]{};   // x, y
        float wide_left_top[2]{};   // the wide view's left and top tangents
        int32_t box[16][4]{};       // x0, y0, x1, y1 (texels, inclusive; at most 128 columns)
        float anchor_x[16]{};       // the anchor's texel x
        float anchor_y[16]{};       // and y
        uint32_t index[16]{};       // its index in EyeCompose's anchor list
    };
    bool fit_beams(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const BeamFit& f);

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 4; // t0 hud, t1 fin, u0 map, u1 count
    static constexpr uint32_t FRAMES = 8;
    static constexpr uint32_t FIT_CB_SIZE = 768;            // [BEAM FIT] one constant buffer slot per ring entry

    bool setup(ID3D12Device* device);
    bool ensure_map(ID3D12Device* device, uint32_t w, uint32_t h);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12PipelineState> m_fit_pso{}; // [BEAM FIT] null when it did not compile: no fit, beams stay where drawn
    ComPtr<ID3D12Resource> m_fit_cb{};       // upload, RING_COUNT slots of FIT_CB_SIZE, mapped
    uint8_t* m_fit_mapped{nullptr};
    D3D12_GPU_DESCRIPTOR_HANDLE m_table{};   // the descriptors of the last dispatch (copies, map, count)
    bool m_table_valid{false};
    uint32_t m_fit_ring{0};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_map{};      // R32_UINT, rests in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_count{};    // running count (RAW), rests in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_readback{}; // one count per frame slot, mapped
    const uint32_t* m_mapped{nullptr};
    std::array<uint64_t, FRAMES> m_slot_frame{};
    std::array<bool, FRAMES> m_slot_ready{};
    uint64_t m_last_frame{~0ull};        // last count read (consecutive frames give the count of one frame)
    uint32_t m_last_count{0};
    uint64_t m_acc_marked{0};
    uint64_t m_acc_blocks{0};
    uint32_t m_acc_frames{0};
    double m_share{-1.0};
    uint32_t m_full_run{0};
    uint32_t m_calm_run{0};
    uint32_t m_idle_run{0};
    uint32_t m_map_w{0};
    uint32_t m_map_h{0};
    uint32_t m_gui_w{0};
    uint32_t m_gui_h{0};
    uint32_t m_stamp{0};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [EYE COMPOSE 29.10.2026] One eye image of the shared periphery in a single pass: the periphery enlarged to the eye size
// (WideUpscale), the fovea image blended in with its exposure gain (FoveaComposite, which measures the gain in a small
// pass first), then the post sharpening, the interface and the rest of the post pass (MonoUi) -- the same maths, without
// the eye-sized image written and read again between the three passes. Each group makes 16 x 16 eye pixels from the
// periphery taps it reads once into shared memory.
struct EyeCompose {
    struct Params {
        bool fovea{false};        // blend the fovea image in (needs the rectangle)
        int32_t rect_x{0};        // fovea rectangle in the eye image (pixels)
        int32_t rect_y{0};
        uint32_t rect_w{0};
        uint32_t rect_h{0};
        float feather_px{64.0f};  // blend width towards the rectangle border
        bool debug_border{false};
        bool match_brightness{false}; // the fovea's exposure brought to the periphery's (gain per eye slot, measured here)
        // [TONE 01.11.2026] with match_brightness: a tone table from the two images' brightness histograms instead of one gain
        bool tone_curve{false};
        // [LIGHT 01.11.2026] the periphery's large-scale light added to the fovea (blocks of 16 px, blurred over light_px)
        bool periphery_light{false};
        float light_px{48.0f}; // [LIGHT 02.11.2026] the light's blur (gaussian sigma, eye pixels)
        // [GLOW 13.11.2026] the light's limit (part of the level, 0 = the built-in 0.35) and only adding light (not darkening)
        float light_limit{0.0f};
        bool light_add_only{false};
        bool light_auto{false}; // [AUTO LIGHT 14.11.2026] blocks judged by agreement with the light around them, gaps left out
        // [VIEW DIFFERENCE 01.11.2026] measure how different fovea and periphery are (blocks of 16 px), read with view_metric
        bool view_metric{false};
        uint32_t gain_slot{0};
        float gain_rate{0.2f};
        bool encode_periphery{false}; // the periphery holds linear values: SRGB after the enlargement
        bool tile_skip{true};         // tiles without a marked GuiMap block in reach skip the wide view's interface (same image)
        // [TILE PASS fix105 20.11.2026] with tile_skip: the tiles' test made by a pass of its own, one thread per tile (ui_tiles),
        // instead of by 17 threads of every group of the eye pass (the same bits, the same image); false: as before fix105
        bool tile_pass{true};
        bool tile_none{false}; // [fix106] GPU test: the tile pass marks no tile (the image changes)
        uint32_t ui_calls{0};  // [fix108] GPU test, with tile_pass: 1 = the pass's text without the interface's calls, 2 = behind a loop
        // [TWO PASSES fix111 20.11.2026] with tile_pass: the eye image by the text without the interface's calls and the marked
        // tiles again by the text with them (the same image); false: one pass with the calls, as fix106 - fix110
        bool two_pass{true};
        // [TWO PASSES fix112 20.11.2026] the first pass leaves the tiles with interface to the second (each tile made once;
        // the same image). ([fix113] RE4 test of fix112: +0.010 / -0.021 ms, within the noise; on, because marked_first
        // needs it. [fix115] off: RE4 test of fix114, see dispatch.)
        bool first_skips_marked{false};
        // [MARKED FIRST fix113 21.11.2026] the marked tiles' pass recorded before the eye image's (see dispatch); needs
        // first_skips_marked. Off: after it, as fix112.
        bool marked_first{true};
        // [fix114 21.11.2026] first_skips_marked only while the wide view shows interface (the map's count): see dispatch.
        // Off: in every frame, as fix113.
        bool first_skips_by_map{true};
        // [TILES PROBE fix114 21.11.2026] every 64th time the tiles' bits are read back and counted (tiles_count; for the
        // log: a diagnosis, to go with the other debug tools)
        bool tiles_probe{false};
        // [MARKED TEXT fix115 21.11.2026] the marked tiles' text: 2 = an anchor at the depth of the one before it takes its
        // mask (the same image; in use), 0 = the text of fix111 - fix114, 1 = 2 without the line search behind panels,
        // 3 = 2 without the panels' depth where no anchor is found (GPU test: the image changes)
        uint32_t marked_test{2};
        // (marked_test: [fix117] 4 = the marked tiles made again without the interface's calls, [fix120] 5 = the same with
        // every thread making its pixel on its own, without the group's stages; both for the GPU test only.
        // [fix121] 6 = the interface over the first pass's stored pixels: a test, only the interface's pixels can differ)
        // [OVER fix122 22.11.2026] the build's way after the user's test of fix121 (the HUD looked the same; two eyes 0.977 /
        // 0.981 ms against 1.008): with marked_test 2 the marked tiles' pass puts the interface over the first pass's stored
        // pixels (kind 12) wherever that text may be used (see dispatch); false = the text of fix120 (GPU test).
        bool marked_over{true};
        // [SMALL GROUPS fix117 21.11.2026] the marked tiles' pass in groups of 8 x 8 pixels (see dispatch); only with the eye
        // pass in tiles of 16 x 16 and marked_test 2. Off until the GPU test has measured it.
        bool marked_small{false};
        bool prepare_tests{false}; // [fix108] the GPU test counts down or runs: its texts are compiled ahead on another thread
        bool quantize_10bit{false};   // [DIRECT 8 BIT 30.10.2026] `out` is the 8-bit headset texture: rounded as a 10-bit image first
        // [HOLE BAND 13.11.2026] the periphery is the shared one (MonoPeriphery: alpha 0 = a gap no view saw): the fovea
        // covers its gaps in the blend band
        bool holes_to_fovea{false};
        // [MENU BACKDROP 15.11.2026] in a game menu: the periphery's scene where the fovea is far darker over a large area
        bool menu_backdrop{false};
        // [MENUS fix81 17.11.2026, fix82 17.11.2026] the interface steady:
        //   fovea_jitter / wide_jitter: this frame's jitter of the two views in their pixels (as the temporal passes get
        //     it: the sample of pixel q shows the point at q + 0.5 - jitter); the interface is read where it put the point
        //   fovea_ui_inside [fix82]: the fovea image holds its view's interface (the view went to the mod's anti-aliasing
        //     as the engine drew it, see TemporalUpscaler's [MENUS fix82]): the pass adds no interface of the fovea view
        //     (dispatch's fovea_color / fovea_hud are not read), the wide view's changes over to it with the fovea's own
        //     weight, and the large-scale light's blocks leave out the places where the wide view has interface.
        //     (fix81's light layer and matte are gone: on the game's menus they let the hidden scene through what the
        //     interface lit, captures of 17.11.2026 16:0x.)
        bool fovea_ui_inside{false};
        //   [fix83] with it, fovea_color / fovea_hud given to dispatch are the fovea view's interface copies with and without
        //   GUI (the same size and format): where they differ the view drew interface (the large-scale light leaves those
        //   places out, the debug tint marks them green)
        //   fovea_backdrop [fix83]: ... and a backdrop the fovea view draws darker than the wide view does at the place is
        //   taken out of the fovea's own pixel (the shader's fovea_backdrop)
        bool fovea_backdrop{true};
        float fovea_jitter[2]{0.0f, 0.0f};
        float wide_jitter[2]{0.0f, 0.0f};
        // [GPU 30.10.2026] test variants of the same image: the exposure gain made in every group (as before 30.10, groups
        // of 16 x 16), groups of 16 x 8, 8 x 8, 32 x 16, 32 x 32 or 16 x 16 pixels. Current (31.10.2026): the gain made
        // once per eye before the pass, groups of 32 x 32 (16 x 16 when the periphery is too dense for their taps).
        // Tile30on32 (31.10.2026): 30 x 30 pixels on 32 x 32 threads, one composite pixel per thread. Before1031 (31.10.2026
        // evening): the pass before the ALU reductions (the uniform tests made on both sides and selected, the Catmull-Rom
        // weights per pixel, integer divisions); SelectTests: the reductions without the branches. Both 32 x 32. Prev2251
        // (31.10.2026 night): the build of 22:51 (before the index masks, the one-sided fovea mask and the loop steps).
        // Prev1101 (01.11.2026): the build of the 01.11 11:33 test (before the masks on the float3 places).
        enum class Variant : uint32_t { Current = 0, GainPerGroup, Tile16x8, Tile8x8, Tile32x16, Tile32x32, Tile16x16, Tile30on32, Before1031, SelectTests,
                                        Prev2251, Prev1101, Count };
        Variant variant{Variant::Current};
        MonoUi::Params ui{};          // interface and post pass (rect / feather as for MonoUi)
    };

    // compiles the passes on the first call; false = not usable (the separate passes are used instead)
    bool ready(ID3D12Device* device) { return setup(device); }
    // [PERIPHERY 2X fix80 17.11.2026] the pass with 16 x 16 groups made ahead: a periphery image denser than 12 pixels per
    // 34 of the eye image (the supersampling TAA's image of twice the wide view's size) does not fit the 32 x 32 groups'
    // taps and gets it (see dispatch); compiled on its first frame it would hold that frame for its compilation
    // ([TILE PASS fix106] and the same pass with the tiles' bits, which is the one that runs; optional)
    // [TILES PROBE fix114 21.11.2026] What the tile pass marked, counted on the CPU from a copy of the tiles' bits (every
    // 64th frame, read 8 frames later). RE4 tests of fix112 and fix113: with a small HUD in the picture (0.2 - 0.4 % of
    // the map's blocks) the second eye pass costs 0.10 - 0.15 ms, and the passes' times fit 5 - 15 % of the tiles marked
    // better than the HUD's own size. The count says which: all tiles, tiles with any bit, with the panels' bit, with an
    // anchor's bit, with the panels' bit alone, with a beam's fitted place other than its depth's, the box around the
    // marked tiles (in tiles), and per bit.
    struct TilesCount {
        uint32_t tiles{0};
        uint32_t marked{0};
        uint32_t panel{0};
        uint32_t anchors{0};
        uint32_t only_panel{0};
        uint32_t beam_other{0};
        uint32_t box{0};
        uint32_t per_bit[17]{};
        bool valid{false};
    };
    const TilesCount& tiles_count(uint32_t slot) const { return m_tiles_count[slot & 1u]; }
    // [OVER fix121] eye passes so far whose marked tiles' pass used the interface-over-the-stored-pixels text
    uint64_t over_frames() const { return m_over_frames; }

    bool prepare_dense(ID3D12Device* device) {
        if (!setup(device) || variant_pso(device, Params::Variant::Tile16x16) == nullptr) {
            return false;
        }

        if (m_tiles_pso != nullptr) {
            // [TWO PASSES fix111] the two passes' texts, and the single pass's for the GPU test (all optional)
            variant_pso(device, Params::Variant::Tile16x16, 2);
            variant_pso(device, Params::Variant::Tile16x16, 4);
            variant_pso(device, Params::Variant::Tile16x16, 1);
            // [MARKED TEXT fix115] the marked tiles' text in use (7) and the GPU test's two (the text of fix112 - fix114
            // that leaves the marked tiles out is no longer made ahead: only its test variant uses it)
            // ([fix116] the two test texts 6 and 8 are no longer made ahead: the test of fix115 is done)
            variant_pso(device, Params::Variant::Tile16x16, 7);
#if !REVR_RELEASE // [RELEASE fix151 24.11.2026] the GPU test's texts: debug build only (the release never uses them; in
                  // the user's RE4 test of fix150 they were the only shaders not in the DLL, about a minute at every start)
            variant_pso(device, Params::Variant::Tile8x8, 9); // [SMALL GROUPS fix117] (GPU test)
            variant_pso(device, Params::Variant::Tile16x16, 10);
            variant_pso(device, Params::Variant::Tile16x16, 11); // [DIRECT fix120] (GPU test)
#endif
            variant_pso(device, Params::Variant::Tile16x16, 12); // [OVER fix121] (the marked tiles' text in use)
        }

        return true;
    }
    // periphery: this eye's periphery (render density, resting in UNORDERED_ACCESS); fovea: the fovea image (UNORDERED_ACCESS,
    // may be null without p.fovea); out: the eye image (UNORDERED_ACCESS, UAV-able format), every pixel written. The colour
    // and GUI copies as for MonoUi::dispatch; gui_map: GuiMap of this frame for centre_hud / centre_fin, or null.
    bool dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* periphery, ID3D12Resource* fovea, ID3D12Resource* out,
        ID3D12Resource* centre_color, ID3D12Resource* centre_hud, ID3D12Resource* centre_fin, ID3D12Resource* fovea_color, ID3D12Resource* fovea_hud,
        const GuiMap* gui_map, const Params& p);
    bool fovea_drawn() const { return m_fovea_drawn; } // the last dispatch blended the fovea in
    // [VIEW DIFFERENCE] an eye slot's measurement of some frames ago: [0] mean |periphery - fovea x one gain| / mean periphery,
    // [1] the same with the fovea's gain or table, [2] mean periphery brightness, [3] the one gain; false = none yet
    bool view_metric(uint32_t slot, float out[4]) const;
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 8;
    // t0 periphery, t1 fovea, t2-t4 centre colour/hud/final, t5-t6 fovea colour/hud, t7 curve, u0 out, u1 gain, u2 GUI map,
    // u3 the measure's partial sums, u4 the blocks, u5 the light, u6 the histograms
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 16; // [TILE PASS fix105] 8 + 8 (was 15)
    // [TILE PASS fix105] the tiles' bits: two texels per tile, up to 1024 x 1024 tiles
    static constexpr uint32_t TILES_W = 2048;
    static constexpr uint32_t TILES_H = 1024;
    // [LIGHT 01.11.2026] as in the shader: blocks of 16 x 16 pixels, at most 128 per side and eye slot
    static constexpr uint32_t LIGHT_BLOCK = 16;
    static constexpr uint32_t LIGHT_MAX = 128;
    static constexpr uint32_t LIGHT_SLOT = LIGHT_MAX * LIGHT_MAX;
    static constexpr uint64_t METRIC_RING = 8; // [VIEW DIFFERENCE] readback entries per eye slot
    static constexpr uint64_t METRIC_LAG = 5;  // dispatches before an entry is read
    static constexpr uint32_t CB_SLOT_SIZE = 768; // [ITEM BEAMS 03.11.2026] was 512 (constants 704 / 608 bytes)

    bool setup(ID3D12Device* device);

    // the pass of a variant (the current one is made by setup, the others on their first use; null = failed)
    ID3D12PipelineState* variant_pso(ID3D12Device* device, Params::Variant v, int bits = 0);
    void precompile_tests(Params::Variant v); // [fix108]

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12PipelineState> m_measure_pso{};
    ComPtr<ID3D12PipelineState> m_gain_pso{}; // [GPU 30.10.2026] one thread: this frame's gain from the measure's sums
    // [BLOCKS 02.11.2026] the rectangle in blocks of 16 x 16 pixels (one group per 2 x 2 blocks: means of both images,
    // brightness histograms); one group per eye: the next frame's gain and tone table, the view difference; the large-scale
    // light blurred over the blocks (one thread per block)
    ComPtr<ID3D12PipelineState> m_blocks_pso{};
    ComPtr<ID3D12PipelineState> m_match_pso{};
    ComPtr<ID3D12PipelineState> m_light_blur_pso{};
    ComPtr<ID3D12PipelineState> m_tiles_pso{}; // [TILE PASS fix105] ui_tiles (optional: without it the eye pass tests its tiles itself)
    std::array<ComPtr<ID3D12PipelineState>, (size_t)Params::Variant::Count> m_variant_pso{};
    std::array<bool, (size_t)Params::Variant::Count> m_variant_failed{};
    // [TILE PASS fix106 20.11.2026] the same passes compiled with TILE_BITS (the tiles' interface bits read from m_tiles)
    // ([fix108, fix111] four kinds: TILE_BITS alone, with UI_NEVER, with UI_LOOP, with UI_MARKED; see variant_pso)
    std::array<std::array<ComPtr<ID3D12PipelineState>, (size_t)Params::Variant::Count>, 12> m_bits_pso{};
    std::array<std::array<bool, (size_t)Params::Variant::Count>, 12> m_bits_failed{};
    // [OVER fix121] the eye image format last asked for its typed UAV load, and the answer; frames in which the marked tiles'
    // pass put the interface over the stored pixels (for the log)
    DXGI_FORMAT m_over_format{DXGI_FORMAT_UNKNOWN};
    bool m_over_ok{false};
    uint64_t m_over_frames{0};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    ComPtr<ID3D12Resource> m_cb{};
    // per eye slot: two smoothed gains (ping-pong), the last measurement, this frame's gain; [TONE] row 1: the tone table
    // twice (ping-pong) and this frame's
    ComPtr<ID3D12Resource> m_gain{};
    ComPtr<ID3D12Resource> m_partials{}; // per eye slot: the measure's 16 + 16 group sums (gain only)
    ComPtr<ID3D12Resource> m_light_sums{}; // [LIGHT] per eye slot: the blocks' means (periphery, fovea), float4
    ComPtr<ID3D12Resource> m_light{};      // [LIGHT] per eye slot: the light added at each block, float4; + match's measurement
    ComPtr<ID3D12Resource> m_hist{};       // [BLOCKS] per eye slot: the two brightness histograms of the whole blocks (raw)
    ComPtr<ID3D12Resource> m_tiles{};      // [TILE PASS fix105] R32_UINT, TILES_W x TILES_H, rests in UNORDERED_ACCESS
    ComPtr<ID3D12Resource> m_metric_rb{};  // [VIEW DIFFERENCE] readback of match's measurements (mapped)
    // [TILES PROBE fix114]
    static constexpr uint32_t TILES_PROBE_SLOT = 1u << 20; // bytes of the readback per eye slot
    struct TilesProbe {
        uint64_t at{0}; // the slot's dispatch number the copy was recorded in
        uint32_t w{0};  // texels per row (two per tile)
        uint32_t h{0};
        uint32_t pitch{0};
        bool pending{false};
    };
    ComPtr<ID3D12Resource> m_tiles_rb{};
    const uint8_t* m_tiles_mapped{nullptr};
    bool m_tiles_rb_failed{false};
    TilesProbe m_tiles_probe[2]{};
    TilesCount m_tiles_count[2]{};
    uint64_t m_tiles_dispatches[2]{};
    const float* m_metric_mapped{nullptr};
    uint64_t m_metric_count[2]{};          // measurements per eye slot
    uint32_t m_parity[2]{};              // which of the slot's gains holds the last frame's
    uint8_t* m_cb_mapped{nullptr};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_fovea_drawn{false};
    bool m_failed{false};
    const char* m_last_error{""};
};

// [FOVEA LINEAR 05.11.2026] The upscaler plugin in the fovea (FSR 4) expects linear light (AMD's FSR manual: "The contents
// of the color buffer should always be in Linear Colorspace"); the fovea views come after the game's output pass, SRGB
// encoded. Fed as they were, FSR 4 averaged encoded values: in the dumps of 05.11.2026 fine detail (foliage against the
// sky) came out 7-13 % darker than its input, with yellow-green fringes on the foliage's edges; the mod's TAA keeps it
// within 0.5 %. Two small passes around the plugin:
//   decode: the fovea input (SRGB encoded; an SRGB view decodes by itself) -> linear, into an R16G16B16A16_FLOAT image
//   encode: the plugin's linear result -> the SRGB-encoded fovea image the eye pass reads, with RCAS (the same as the
//           fovea TAA's resolve: the strongest negative lobe that keeps the cross around the pixel within [0, 1])
struct FoveaLinear {
    // [FSR DIRECT 06.11.2026] a part of the output written from a part of the input: the thread at (x, y) of the part (w x h)
    // writes out[out_x + x, out_y + y] from in[clamp(x + shift_x, lo_x, hi_x), clamp(y + shift_y, lo_y, hi_y)]; the RCAS
    // neighbours are clamped the same way (they stay inside [lo, hi]). hi < 0: the input's last column / row.
    struct Region {
        uint32_t out_x{0};
        uint32_t out_y{0};
        uint32_t w{0};
        uint32_t h{0};
        int32_t shift_x{0};
        int32_t shift_y{0};
        int32_t lo_x{0};
        int32_t lo_y{0};
        int32_t hi_x{-1};
        int32_t hi_y{-1};
        // [FSR MODEL 09.11.2026] downsample_part only: the thread at (x, y) averages the input over
        // [src + (x, y) * scale, src + (x + 1, y + 1) * scale) (area weights, scale 1..3); the clamp box as above
        float scale_x{1.0f};
        float scale_y{1.0f};
        float src_x{0.0f};
        float src_y{0.0f};
    };

    // in: resting in in_state, read; out: R16G16B16A16_FLOAT (UAV-able), same size, resting in UNORDERED_ACCESS
    bool decode(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out);
    // in: the plugin's result, resting in UNORDERED_ACCESS (read, then back); out: UAV-able non-SRGB image of the same
    // size, resting in UNORDERED_ACCESS. sharpen: RCAS amount (0 = off, 1 = the strongest)
    bool encode(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, float sharpen);
    // [FSR DIRECT 06.11.2026] the same into / from a part (Region): decode (linear light, or a plain copy when `decode_curve`
    // is false or the input is an SRGB view); copy: a plain copy of the first channels (`depth`: the depth plane of a
    // depth(-stencil) texture) into an R32_FLOAT / R32G32_FLOAT / ... image; encode: `curve` false = the input holds encoded
    // values already (RCAS only). States as in decode / encode.
    bool decode_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out,
        const Region& r, bool decode_curve = true);
    bool copy_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out,
        const Region& r, bool depth);
    bool encode_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, float sharpen, const Region& r,
        bool curve = true);
    // [FSR MODEL 09.11.2026] the input (resting in UNORDERED_ACCESS, read, then back) scaled down into a part of the output
    // (UNORDERED_ACCESS): area average in the input's own values (FSR 4's result: linear light)
    bool downsample_part(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, ID3D12Resource* out, const Region& r);
    void reset();
    const char* last_error() const { return m_last_error; }

private:
    static constexpr uint32_t RING_COUNT = 24; // up to eight dispatches per frame (two per eye, four atlas copies, ...)
    static constexpr uint32_t DESCRIPTORS_PER_DISPATCH = 2; // t0 in, u0 out
    static constexpr uint32_t FLAG_DEPTH = 1u << 16;   // (CPU side) the input's SRV is its depth plane

    bool setup(ID3D12Device* device);
    bool run(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* in, D3D12_RESOURCE_STATES in_state, ID3D12Resource* out,
        uint32_t flags, float sharpen, const Region* part = nullptr);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};
    const char* m_last_error{""};
};

// UAV-able, non-SRGB variant of an eye image format (UNKNOWN = not supported)
DXGI_FORMAT eye_uav_format(DXGI_FORMAT f);
// [MONO PERIPHERY] format of a compute-written copy of an engine colour texture that reads back exactly like the
// original (typeless -> its typed UNORM/float; SRGB -> UNKNOWN, those bits would be decoded differently)
DXGI_FORMAT engine_copy_uav_format(DXGI_FORMAT f);

} // namespace d3d12
