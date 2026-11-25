// FoveatedRendering.hpp -- quad-view foveated rendering with eye tracking for RE4VR in Single Frame Multipass: both eyes
// rendered every frame, the periphery from one wide view between the eyes (reprojected to each eye), each eye's fovea
// from its own narrow view around the gaze.
//
// This mod owns the settings, the gaze (EyeGaze, OpenVR SDK 2.15 eye tracking) and the debug tooling.
// The rendering itself is spread over the components that own the relevant engine hooks:
//   * vr/QuadView.hpp         geometry + shared state (this mod writes settings and gaze into it)
//   * VR / CameraDuplicator   clone the main camera, give the clones their eye / fovea frusta and poses
//   * TemporalUpscaler        periphery (light TAA, reprojection, spatial upscale), fovea (plugin or the mod's TAA),
//                             composite and interface
//   * vr/d3d12/FoveaComposite the GPU passes
//
// Debug output goes to <persistent dir>/foveation_debug.log (status line every 5 s, snapshot, A/B benchmark).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Mod.hpp"
#include "vr/QuadView.hpp"
#include "vr/QuadViewGame.hpp" // [fix125] the options' defaults per game
#include "vr/GpuGuard.hpp" // [GPU GUARD 05.11.2026]
#include "vr/d3d12/GpuTimer.hpp"

class FoveatedRendering : public Mod {
public:
    static std::shared_ptr<FoveatedRendering>& get();
    ~FoveatedRendering() override;

    // [ALWAYS ON fix91 19.11.2026] the quad view is always on: Off (0) is gone, the values of the other two stay
    enum class Mode : int32_t { FIXED = 1, DYNAMIC = 2 };

    std::string_view get_name() const override { return "FoveatedRendering"; }

    std::optional<std::string> on_initialize_d3d_thread() override;
    void on_device_reset() override;
    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_application_entry(void* entry, const char* name, size_t hash) override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_early_present() override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_post_present() override; // [SUBMIT TIMELINE 06.11.2026] the end of a frame for the GPU timeline
#endif

    // [CPU 22.10.2026] render-thread time of the scene layers (wide / inset / other) -> log and benchmark
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
#endif
    // [CPU TEST 04.11.2026] the CPU test's phases without the fovea views' interface update (kind 5)
    bool on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    // [WHITE POINT 13.11.2026] the engine's adapted exposure / white point resources seen at each view's post effect draw
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
#endif

    // Drawn inside the RENDERING menu category (see TemporalUpscaler::draw_settings)
    void draw_settings();

    Mode mode() const { return m_mode->value() == (int32_t)Mode::DYNAMIC ? Mode::DYNAMIC : Mode::FIXED; }

private:
    struct Preset {
        const char* name;
        float fraction;
        float ratio;
    };

#if !REVR_RELEASE // [RELEASE SWITCH fix139] no preset list in the release
    // fovea size (fraction of the eye image) and its upscale ratio (1 = full pixel density)
    static constexpr Preset PRESETS[4] = {
        {"Sharp", 0.40f, 1.00f},
        {"Balanced", 0.40f, 1.20f},
        {"Fast", 0.35f, 1.35f},
        {"Exact 1/3", 0.3334f, 1.00f},
    };
#endif
    static constexpr int PRESET_COUNT = 4;
    static constexpr int PRESET_CUSTOM = 4;

    // [CPU] monotonic totals: scene layer kind (0 wide/primary, 1 inset, 2 other) x (0 update, 1 draw)
    struct CpuTotals {
        uint64_t ns[3][2]{};
        uint64_t n[3][2]{};
        uint64_t rendering_ns{0}; // BeginRendering -> EndRendering
        uint64_t rendering_n{0};
        uint64_t calls_projection{0}, calls_view{0}, calls_primary_camera{0}, views_game_logic{0};
        uint64_t views_logic_kept{0}, views_in_layer{0}, views_other_threads{0}, views_jobs_outside{0}; // [VIEW RACE 03.11.2026]
    };

    struct BenchRow {
        bool quadview_on{false};
        double cpu_ms{0.0};
        uint32_t cpu_frames{0};
        d3d12::GpuTimer::Result gpu{};
        std::string cpu_text{};
    };

    // [GPU TEST 29.10.2026] one phase of the GPU pass test: a variant's GPU time per pass
    struct PassTestRow {
        int variant{0};
        double cpu_ms{0.0};
        uint32_t cpu_frames{0};
        bool engaged{false};
        d3d12::GpuTimer::Result gpu{};
        std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT> spans{};
        std::array<uint64_t, 2> paths{}; // [ONE LIST 01.11.2026] measured frames by list layout (quadview::State::path_frames)
    };

#if !REVR_RELEASE // [RELEASE SWITCH fix139]
    void apply_preset(int index);
#endif
    void push_gaze_options();
    quadview::Settings current_settings() const;
    void update_frame();

    // ---- debug file / tools ---------------------------------------------------------------------
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the debug log, the status line, the tests: debug build only
    void debug_log(const std::string& text);
    std::string build_snapshot(const char* title);
    void write_periodic_line();
    bool gpu_timer_enable(bool enable);
    void bench_start();
    void bench_cancel(const char* why);
    void bench_tick(double dt_ms, bool& force_off);
    void bench_finish();
    void ptest_start();
    void ptest_cancel(const char* why);
    void ptest_tick(double dt_ms);
    void ptest_finish();
    // [ENGINE VIEWS 30.10.2026] what the engine renders in each view, and how its GPU time depends on the views
    std::string describe_render_layers();
    void vtest_start();
    void vtest_cancel(const char* why);
    void vtest_tick(double dt_ms);
    void vtest_finish();
    // [ENGINE SHADOWS 30.10.2026] the engine's dynamic shadows off and on (what they cost in all views), the shadow layers
    std::string describe_shadow_types();
    std::string describe_ao_settings();
    std::string describe_vrs_settings(); // [VRS 01.11.2026]
    std::string describe_engine_types(int kind); // 0 shadows, 1 AO, 2 variable rate shading, 3 exposure / tone mapping, 4 colour correction
    static std::optional<bool> get_dynamic_shadows();
    static bool set_dynamic_shadows(bool on);
    void stest_start();
    void stest_cancel(const char* why);
    void stest_tick(double dt_ms);
    void stest_finish();
    bool any_test_running() const {
        return m_bench_running || m_ptest_running || m_vtest_running || m_stest_running || m_ftest_running || m_rtest_running || m_ctest_running ||
               m_lft_running;
    }
    // [CPU TEST 03.11.2026] in a fight: 15 s phases of normal rendering and of one part switched off (the fovea views' objects,
    // the fovea views' dynamic shadows, all dynamic shadows). The periodic status line is closed at each change, so every line
    // belongs to one phase, and labelled with it.
    void ctest_start(bool gpu); // [GPU TEST 06.11.2026] gpu: the GPU test's phases (occlusion culling), with the GPU timeline
    void ctest_cancel(const char* why);
    void ctest_tick();
    void ctest_next_phase(int phase);
    void ctest_apply(int kind);
    bool set_fovea_dynamic_shadows(bool on); // the fovea views' shadow cast layers (the values found are put back)
    bool set_occlusion_culling(int views, bool restore); // [GPU TEST 06.11.2026] views 1: the fovea views, 0: all
    // [OCCLUSION 15.11.2026] views 1: the wide view, 2: every view; restore = each switch back to the value found
    int set_view_occlusion(int views, bool restore, std::vector<std::pair<const void*, bool>>& original);
    void view_occlusion_tick(bool engaged);
    bool submit_timeline_enable(bool enable);             // [SUBMIT TIMELINE 06.11.2026]
    std::string cpu_test_label() const;
    // [RENDER SETTINGS 30.10.2026] the ambient occlusion's settings tried at other values (game setting, cameras' AO)
    void rtest_start();
    void rtest_cancel(const char* why);
    void rtest_tick(double dt_ms);
    void rtest_finish();
    void rtest_apply_variant();
    void rtest_restore();
    // [ENGINE FEATURES 30.10.2026] the engine's effects switched off one at a time (what each costs in all views)
    void ftest_start();
    void ftest_cancel(const char* why);
    void ftest_tick(double dt_ms);
    void ftest_finish();
    void ftest_restore();
    int ftest_switch_off(int from, std::string& report);
    // [LIGHT SHAFT OFF 13.11.2026] (test) the scene's light shaft entities (via.render.LightShaft) off in all views, as the
    // game's own VR controller (app.vr.VrLightShaftController.setLightShaftEnable) does for its stereo mode
    void light_shafts_apply(bool off, bool announce);
    void set_message(std::string text);
    std::string get_message() const;
    void set_ptest_summary(std::string text);
    std::string get_ptest_summary() const;
#else
    bool any_test_running() const { return false; } // (no tests in the release)
#endif

    // ---- settings -------------------------------------------------------------------------------
    // New keys (QuadView*) on purpose: a config from the VRS builds must not switch quad view on by itself.
    const ModCombo::Ptr m_mode{ModCombo::create(generate_name("QuadViewMode"), {"Off", "Fixed (lens centre)", "Dynamic (eye tracking)"}, 1)};
    // ([ALWAYS ON fix91 19.11.2026] starts with Fixed; "Off" only keeps the stored values 1 and 2 what they were: the menu
    // draws its own list of the two modes, see draw_settings)
    // [RELEASE fix132 23.11.2026] The release's values for RE4, as the user set them in his table of the menu: RE4_OR(a, b)
    // is a in the RE4 build and b in every other (RE7 as it was). No preset list in RE4 (Custom), the fovea size down to
    // 0.20 (quadview::Settings takes 0.15 .. 0.8).
// [fix143 23.11.2026] RE7 takes RE4's defaults (the user's choice), [fix144] RE2 too: QV_DEFAULT(a, b) is a in RE4, RE7 and
// RE2, b in the other games (b: the values before fix143).
#if defined(RE4) || defined(RE7) || defined(RE2)
#define QV_DEFAULT(release, other) (release)
#else
#define QV_DEFAULT(release, other) (other)
#endif
    const ModCombo::Ptr m_preset{ModCombo::create(generate_name("QuadViewPreset"), {"Sharp", "Balanced", "Fast", "Exact 1/3 (no dynamic resolution)", "Custom"}, QV_DEFAULT(4, 1))};
    const ModSlider::Ptr m_fraction{ModSlider::create(generate_name("QuadViewFoveaSize"), QV_DEFAULT(0.20f, 0.25f), 0.60f, QV_DEFAULT(0.25f, 0.40f))};
    const ModSlider::Ptr m_ratio{ModSlider::create(generate_name("QuadViewFoveaRatio"), 1.0f, 1.6f, QV_DEFAULT(1.0f, 1.20f))};
    // ([NO PLUGIN fix91 19.11.2026] the upscaler plugin's method was chosen here, key QuadViewUpscaler: there is no plugin)
    // light TAA of the periphery: history weight
    const ModSlider::Ptr m_taa_feedback{ModSlider::create(generate_name("QuadViewTAAFeedback"), 0.75f, 0.97f, 0.90f)};
    // [PERIPHERY SUPER fix80 17.11.2026] what anti-aliases the periphery (the shared wide view): the light TAA as before,
    // or the mod's supersampling TAA with an image of the wide view's size or of twice that size (see
    // quadview::Settings::periphery_aa)
    const ModCombo::Ptr m_periphery_aa{ModCombo::create(generate_name("QuadViewPeripheryAA"),
        {"Light TAA (as before)", "Supersampling TAA of the mod", "Supersampling TAA of the mod, 2x image (sharper, steadier)",
         "Calm light TAA (lightest)"}, 2)}; // [CALM fix100 20.11.2026] 3, see quadview::Settings::periphery_aa
    // [FOVEA TAA 28.10.2026] what anti-aliases the fovea (new key: the fovea starts with the plugin again)
    // [SUPER TAA 16.11.2026] third choice: the mod's supersampling TAA (d3d12::SuperTAA)
    const ModCombo::Ptr m_fovea_aa{ModCombo::create(generate_name("QuadViewFoveaAA_V2"),
        {"Upscaler plugin (FSR 4 / FSR 3)", "TAA of the mod", "Supersampling TAA of the mod (new)"}, 2)};
    // ([UPSCALING MENU fix91 19.11.2026] always 2, see on_config_load: the choice is no longer in the menu)
    // [SUPER TAA 16.11.2026] its settings (see quadview::SuperTaaSettings for the meaning and defaults)
    const ModCombo::Ptr m_staa_scale{ModCombo::create(generate_name("QuadViewSuperTAA_HistoryScale"), {"1x (lighter)", "2x (sharper and steadier)"}, 1)};
    const ModSlider::Ptr m_staa_stability{ModSlider::create(generate_name("QuadViewSuperTAA_Stability"), 2.0f, 16.0f, 5.0f)};
    // [PERIPHERY fix82 17.11.2026] the same for the periphery's supersampling TAA (see quadview::Settings::periphery_stability)
    const ModSlider::Ptr m_periphery_stability{ModSlider::create(generate_name("QuadViewPeripheryStability"), 2.0f, 16.0f, 10.0f)};
    const ModSlider::Ptr m_staa_hist_sharp{ModSlider::create(generate_name("QuadViewSuperTAA_HistorySharpness"), 0.0f, 1.0f, 0.50f)};
    const ModSlider::Ptr m_staa_sharpen{ModSlider::create(generate_name("QuadViewSuperTAA_Sharpen"), 0.0f, 1.0f, 0.30f)};
    // [fix77 16.11.2026] following changes. (fix76's anti-flicker keys QuadViewSuperTAA_AntiFlicker, ..._AntiFlickerRange,
    // ..._SparkLimit, ..._AntiFlickerMemory, ..._ChangeThreshold of an older config are ignored: fix78's protection has
    // its own keys below, with its own defaults.)
    const ModToggle::Ptr m_staa_follow{ModToggle::create(generate_name("QuadViewSuperTAA_FollowChanges"), true)};
    const ModSlider::Ptr m_staa_follow_thr{ModSlider::create(generate_name("QuadViewSuperTAA_FollowThreshold"), 0.5f, 3.0f, 1.0f)};
    const ModSlider::Ptr m_staa_follow_mem{ModSlider::create(generate_name("QuadViewSuperTAA_FollowMemory"), 0.0f, 3.0f, 0.5f)};
    // [fix78 16.11.2026] the protection of what one frame cannot show is back (released where the history is stale)
    const ModToggle::Ptr m_staa_protect{ModToggle::create(generate_name("QuadViewSuperTAA_SteadyDetail"), true)};
    const ModSlider::Ptr m_staa_protect_range{ModSlider::create(generate_name("QuadViewSuperTAA_SteadyRange"), 0.0f, 3.0f, 1.5f)};
    const ModSlider::Ptr m_staa_protect_spark{ModSlider::create(generate_name("QuadViewSuperTAA_SteadySpark"), 0.0f, 6.0f, 2.5f)};
    const ModSlider::Ptr m_staa_protect_mem{ModSlider::create(generate_name("QuadViewSuperTAA_SteadyMemory"), 1.0f, 3.0f, 1.5f)};
    // [fix79 16.11.2026] no trails behind things that move without their motion vectors saying so (flies)
    const ModToggle::Ptr m_staa_arrive{ModToggle::create(generate_name("QuadViewSuperTAA_SteadyJumps"), true)};
    const ModSlider::Ptr m_staa_filter{ModSlider::create(generate_name("QuadViewSuperTAA_FilterScale"), 0.6f, 1.6f, 1.0f)};
    // (fix78: a new key. With the protection the range to try is 1.0; a 1.6 saved for fix77 would stay otherwise)
    const ModSlider::Ptr m_staa_clip{ModSlider::create(generate_name("QuadViewSuperTAA_ClipRange"), 0.50f, 2.50f, 1.00f)};
    const ModSlider::Ptr m_staa_motion{ModSlider::create(generate_name("QuadViewSuperTAA_MotionReduce"), 0.00f, 0.60f, 0.25f)};
    const ModSlider::Ptr m_staa_depth{ModSlider::create(generate_name("QuadViewSuperTAA_DepthReject"), 0.00f, 0.50f, 0.10f)};
    const ModToggle::Ptr m_staa_luma{ModToggle::create(generate_name("QuadViewSuperTAA_LumaWeight"), true)};
    const ModToggle::Ptr m_staa_lighter{ModToggle::create(generate_name("QuadViewSuperTAA_LighterPass"), false)};
    const ModToggle::Ptr m_staa_lanczos{ModToggle::create(generate_name("QuadViewSuperTAA_Lanczos"), false)};
    // [FOVEA TAA 28.10.2026] the mod's TAA in the fovea (see quadview::FoveaTaaSettings for the meaning and defaults)
    const ModSlider::Ptr m_ftaa_feedback{ModSlider::create(generate_name("QuadViewFoveaTAA_Feedback"), 0.50f, 0.98f, 0.90f)};
    const ModSlider::Ptr m_ftaa_clip{ModSlider::create(generate_name("QuadViewFoveaTAA_Clip"), 0.50f, 2.50f, 1.00f)};
    const ModSlider::Ptr m_ftaa_filter{ModSlider::create(generate_name("QuadViewFoveaTAA_Filter"), 0.00f, 0.80f, 0.40f)};
    const ModSlider::Ptr m_ftaa_new_soft{ModSlider::create(generate_name("QuadViewFoveaTAA_NewSoftness"), 0.00f, 1.00f, 0.50f)};
    const ModSlider::Ptr m_ftaa_motion{ModSlider::create(generate_name("QuadViewFoveaTAA_MotionReduce"), 0.00f, 0.60f, 0.25f)};
    const ModSlider::Ptr m_ftaa_depth{ModSlider::create(generate_name("QuadViewFoveaTAA_DepthReject"), 0.00f, 0.50f, 0.10f)};
    const ModSlider::Ptr m_ftaa_sharpen{ModSlider::create(generate_name("QuadViewFoveaTAA_Sharpen"), 0.00f, 1.00f, 0.30f)};
    const ModToggle::Ptr m_ftaa_dilate{ModToggle::create(generate_name("QuadViewFoveaTAA_Dilate"), true)};
    const ModToggle::Ptr m_ftaa_catmull{ModToggle::create(generate_name("QuadViewFoveaTAA_CatmullRom"), true)};
    const ModToggle::Ptr m_ftaa_luma{ModToggle::create(generate_name("QuadViewFoveaTAA_LumaWeight"), true)};
    const ModToggle::Ptr m_ftaa_fast{ModToggle::create(generate_name("QuadViewFoveaTAA_FastConverge"), true)};
    // [GUARDS fix91 19.11.2026] the options of the upscaler plugin in the fovea were here (QuadViewFoveaUpscalerLinear,
    // QuadViewFoveaUpscalerSharpen, QuadViewFoveaCollapseGuard, QuadViewFoveaGpuGuard): the plugin is no longer the fovea's
    // anti-aliasing
    // [GPU GUARD fix91 19.11.2026] while the GPU is short of time the supersampling TAA runs with its lighter choices, in the
    // fovea (history 1x) and in the periphery (image of the wide view's size) at once, and comes back by itself (see
    // gpu_guard_watch, quadview::GpuGuard). A key of its own, off unless switched on in the menu.
    const ModToggle::Ptr m_gpu_guard_opt{ModToggle::create(generate_name("QuadViewLighterWhenGpuLate"), QV_DEFAULT(true, false))};
    // [GUARD START fix138 23.11.2026] The user's slider: from what GPU time per frame (ms, 8.0 .. 11.0 in steps of 0.1)
    // the guard steps in. Up to fix137 it was the headset's frame time less 1.2 ms (9.9 ms at 90 Hz), the default.
    // [GUARD START % fix150 24.11.2026] now a share of the headset's frame time (0.70 .. 1.00, steps of 0.01), so it fits
    // every refresh rate: 0.891 = 9.9 ms at 90 Hz, fix138's default. New key: the old one held milliseconds.
    const ModSlider::Ptr m_gpu_guard_start_pct{ModSlider::create(generate_name("QuadViewLighterWhenGpuLateFromFrameShare"), 0.70f, 1.00f, 0.891f)};
    // [fix94 19.11.2026] the periphery's interface collected only around it (quadview::Settings::ui_steady_around), off
    // unless switched on; and the diagnostics off: no GPU timestamps (kept while the GPU guard is on: it reads them), no
    // periodic log line, no layer timing, no video memory sample. The tests switch the timestamps on while they run.
    // ([fix95] on unless switched off: tried by the user in RE4 on 19.11.2026 and kept)
    const ModToggle::Ptr m_ui_around{ModToggle::create(generate_name("QuadViewInterfaceOnlyAround"), true)};
    const ModToggle::Ptr m_diag_off{ModToggle::create(generate_name("DebugDiagnosticsOff"), false)};
    // [DRED fix103 20.11.2026] Device Removed Extended Data from the next start (d3d12_dred, D3D12Hook.hpp: REFramework reads
    // this key from the configuration file at its entry, before the game makes its device). Off unless switched on: it
    // was on at every start from 14.11.2026 to fix102.
    const ModToggle::Ptr m_debug_dred{ModToggle::create(generate_name("DebugDeviceRemovedData"), false)};
    // [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod itself, both foveas in one pass, FSR's own exposure (see
    // quadview::Settings::fsr_direct)
    // [FOVEA OWN fix91 19.11.2026] the options of FSR 4 by the mod were here (QuadViewFoveaFsrDirect, ...FsrOnePass,
    // ...FsrAutoExposure, ...FsrModel, QuadViewFoveaSupersample): no upscaler is set up for the fovea any more
    // [BRIGHTNESS 23.10.2026] the fovea camera's own exposure brought to the periphery's (new key: on for everybody)
    const ModToggle::Ptr m_brightness_match{ModToggle::create(generate_name("QuadViewFoveaExposureMatch"), QV_DEFAULT(false, true))};
    // [FOVEA TONE 01.11.2026] a gain per brightness instead of one (see quadview::Settings)
    const ModToggle::Ptr m_tone_match{ModToggle::create(generate_name("QuadViewFoveaToneMatch"), QV_DEFAULT(false, true))};
    // [FOVEA RAY TRACING 12.11.2026] the game's ray tracing in the fovea views too (see quadview::State::fovea_ray_tracing)
    // [RT DEFAULT 13.11.2026] off by default (new key: the old one was on): RE7 crashed four times in the engine's own mesh
    // preparation of the clones' ray tracing (13.11.2026 14:31: 10 ms after the components were made in game)
    const ModToggle::Ptr m_fovea_ray_tracing{ModToggle::create(generate_name("QuadViewFoveaRayTracing_V2"), false)};
    // [LIGHT 01.11.2026] the periphery's large-scale light in the fovea (see quadview::Settings)
    const ModToggle::Ptr m_periphery_light{ModToggle::create(generate_name("QuadViewFoveaPeripheryLight"), true)};
    // [LIGHT 02.11.2026] its blur in pixels (see quadview::Settings)
    const ModSlider::Ptr m_light_px{ModSlider::create(generate_name("QuadViewFoveaLightSize"), 32.0f, 64.0f, 48.0f)};
    const ModSlider::Ptr m_feather{ModSlider::create(generate_name("QuadViewFeather"), 0.0f, 0.50f, QV_DEFAULT(0.105f, 0.10f))};
    const ModSlider::Ptr m_deadzone{ModSlider::create(generate_name("QuadViewDeadzone"), 0.0f, 0.05f, QV_DEFAULT(0.005f, 0.015f))};
    const ModToggle::Ptr m_center_when_zoomed{ModToggle::create(generate_name("QuadViewCenterWhenZoomed"), true)};

    const ModSlider::Ptr m_smoothing{ModSlider::create(generate_name("GazeSmoothing"), 0.5f, 10.0f, 2.0f)};
    const ModToggle::Ptr m_invert_x{ModToggle::create(generate_name("GazeInvertX"), false)};
    const ModToggle::Ptr m_invert_y{ModToggle::create(generate_name("GazeInvertY"), false)};
    const ModSlider::Ptr m_offset_x{ModSlider::create(generate_name("GazeOffsetX"), -0.5f, 0.5f, 0.0f)};
    const ModSlider::Ptr m_offset_y{ModSlider::create(generate_name("GazeOffsetY"), -0.5f, 0.5f, 0.0f)};
    const ModToggle::Ptr m_declare_sdk{ModToggle::create(generate_name("DeclareSDK2_15"), true)};
    const ModToggle::Ptr m_log_periodic{ModToggle::create(generate_name("DebugPeriodicLog"), QV_DEFAULT(false, true))};
    // [MONO PERIPHERY 27.10.2026] the periphery of both eyes from ONE wide view (three scene layers); off = one wide view
    // per eye (four), for comparison (see quadview::Settings)
    const ModToggle::Ptr m_mono_periphery{ModToggle::create(generate_name("QuadViewMonoPeriphery"), true)};
    // [VIEW RACE 03.11.2026] who gets the head's view, between the eyes, for game logic (item names, markers, aiming): see
    // quadview::State::head_view_mode (new key: the toggle of 12:00 is not taken over)
    const ModCombo::Ptr m_head_view_logic{ModCombo::create(generate_name("QuadViewHeadViewMode"),
        {"Off (names jump sideways, before 03.11)", "Main thread only (test of 03.11 12:00)", "Main thread and engine jobs outside view updates"}, 2)};
    // [HOLES 02.11.2026] the shared periphery's gaps beside near things (see quadview::Settings)
    const ModToggle::Ptr m_hole_fill{ModToggle::create(generate_name("QuadViewHoleFill"), true)};
    // [STABLE DEPTH fix81 17.11.2026] the reprojection's distances steadied over time (see quadview::Settings)
    const ModToggle::Ptr m_stable_depth{ModToggle::create(generate_name("QuadViewStableDepth"), true)};
    // [LIGHTS fix85 18.11.2026] the shared wide view drawn before the fovea views (see quadview::Settings::wide_first)
    const ModToggle::Ptr m_wide_first{ModToggle::create(generate_name("QuadViewWideViewFirst"), true)};
    // [MENUS fix81 17.11.2026] the menus steady (see quadview::Settings; fix82: the fovea views keep their interface)
    const ModToggle::Ptr m_steady_interface{ModToggle::create(generate_name("QuadViewSteadyInterface"), true)};
    // [PERIPHERY fix83 17.11.2026] the periphery's own history filter (see quadview::Settings::periphery_history_sharpness)
    const ModSlider::Ptr m_periphery_hist_sharp{ModSlider::create(generate_name("QuadViewPeripheryHistorySharpness"), 0.0f, 1.0f, 0.0f)};
    // [ITEM BEAMS 03.11.2026] the green beams over items at their item's depth outside the fovea (see quadview::Settings)
    const ModToggle::Ptr m_item_beams{ModToggle::create(generate_name("QuadViewItemBeams"), true)};
    // [EARLY IMGUI 04.11.2026] REFramework's ImGui frame (every mod's on_frame) before the wait for the rendering, see cpustats
    const ModToggle::Ptr m_early_imgui{ModToggle::create(generate_name("EarlyImguiFrame"), true)};
    // [GPU 29.10.2026] the left eye in the desktop window (see quadview::Settings)
    const ModToggle::Ptr m_desktop_mirror{ModToggle::create(generate_name("QuadViewDesktopMirror"), QV_DEFAULT(false, true))};
    // [SMALL WINDOW fix122 22.11.2026] the game's window an engine view's size instead of an eye image's (quadview::State)
    // ([fix125] on from the start in RE4: quadview::game::small_window_default)
    const ModToggle::Ptr m_small_window{ModToggle::create(generate_name("QuadViewSmallWindow"), quadview::game::small_window_default())};
    // [UI MOTION fix125 22.11.2026] the temporal passes fetch the interface along its own motion (quadview::Settings::ui_motion)
    const ModToggle::Ptr m_ui_motion{ModToggle::create(generate_name("QuadViewInterfaceMotion"), quadview::game::ui_motion_default())};
    // [DIRECT 8 BIT 30.10.2026] the eye pass writes the headset textures (see quadview::Settings)
    const ModToggle::Ptr m_direct_8bit{ModToggle::create(generate_name("QuadViewDirect8Bit"), true)};
    // [FOVEA UI UPDATE 04.11.2026] the fovea views' overlay layer update left out (see on_pre_overlay_layer_update)
    const ModToggle::Ptr m_skip_fovea_ui_update{ModToggle::create(generate_name("QuadViewSkipFoveaInterfaceUpdate"), true)};

    ValueList m_options{
        *m_mode, *m_preset, *m_fraction, *m_ratio, *m_taa_feedback, *m_fovea_aa, *m_ftaa_feedback, *m_ftaa_clip, *m_ftaa_filter,
        *m_ftaa_new_soft, *m_ftaa_motion, *m_ftaa_depth, *m_ftaa_sharpen, *m_ftaa_dilate, *m_ftaa_catmull, *m_ftaa_luma, *m_ftaa_fast,
        *m_brightness_match, *m_tone_match, *m_periphery_light, *m_light_px, *m_feather, *m_deadzone, *m_center_when_zoomed, *m_smoothing, *m_invert_x, *m_invert_y, *m_offset_x, *m_offset_y,
        *m_declare_sdk, *m_log_periodic, *m_mono_periphery, *m_head_view_logic, *m_hole_fill, *m_item_beams, *m_early_imgui, *m_desktop_mirror, *m_small_window, *m_ui_motion, *m_direct_8bit,
        *m_skip_fovea_ui_update, *m_gpu_guard_opt, *m_gpu_guard_start_pct, *m_ui_around, *m_diag_off, *m_debug_dred,
        *m_fovea_ray_tracing,
        // [SUPER TAA 16.11.2026]
        *m_staa_scale, *m_staa_stability, *m_staa_hist_sharp, *m_staa_sharpen, *m_staa_follow, *m_staa_follow_thr, *m_staa_follow_mem,
        *m_staa_protect, *m_staa_protect_range, *m_staa_protect_spark, *m_staa_protect_mem, *m_staa_arrive,
        *m_staa_filter, *m_staa_clip, *m_staa_motion, *m_staa_depth, *m_staa_luma, *m_staa_lighter, *m_staa_lanczos,
        // [PERIPHERY SUPER fix80 17.11.2026]
        *m_periphery_aa,
        // [STABLE DEPTH, MENUS fix81 17.11.2026]
        *m_stable_depth, *m_wide_first, *m_steady_interface,
        // [PERIPHERY fix82 / fix83 17.11.2026]
        *m_periphery_stability, *m_periphery_hist_sharp
    };

    void reset_fovea_taa();            // the TAA sliders back to their recommended values
    // [SUPER TAA 16.11.2026]
    void reset_super_taa();            // the supersampling TAA's settings back to their recommended values
    bool draw_super_taa_settings();    // its part of the menu; true = a setting changed
    int m_staa_debug_view{0};          // d3d12::SuperTAA::Params::debug_view (never persisted)
    bool m_staa_split{false};          // left half of the fovea from the mod's TAA (never persisted)
    int m_staa_capture_frames{0};      // capture: 0 = 16 frames, 1 = 32, 2 = 64 (never persisted)
    int m_staa_capture_part{1};        // capture: 0 = 192, 1 = 256, 2 = 384 pixels (never persisted)
    // "Capture in 4 s": when it starts (time_point{} = none pending), so the eyes are on the thing to capture
    std::chrono::steady_clock::time_point m_staa_capture_at{};
    static constexpr int STAA_CAPTURE_DELAY_SECONDS = 4;
    uint64_t m_staa_logged_dispatches{0}; // the pass's counters at the last log line (see the periodic status)
    uint64_t m_staa_logged_fallbacks{0};
    int m_staa_logged_state{-1};       // -1 nothing logged yet, 0 not chosen, 1 running, 2 chosen but the mod's TAA runs
    bool m_debug_border{false}; // never persisted
    int m_debug_view{0};        // 0 normal, 1 periphery only, 4 mark reprojection holes, 5 tint interface sources (never persisted)
    bool m_separate_eye_passes{false}; // [GPU 29.10.2026] comparison: the old separate eye passes (never persisted)
    bool m_hole_band{true};            // [HOLE BAND 13.11.2026] comparison: off = the periphery's gaps blended in the band (never persisted)
    bool m_menu_backdrop{true};        // [MENU BACKDROP 15.11.2026] in game menus the periphery's scene past the menu's frame (never persisted)
    bool m_fovea_backdrop{false};       // [BACKDROP fix83 17.11.2026] see quadview::Settings::fovea_backdrop (never persisted; [fix84] off)
    int m_fovea_whole_infos{0};        // [LIGHTS fix83 17.11.2026] see quadview::State::fovea_whole_infos (never persisted)
    bool m_soft_outlines{true};        // [OUTLINES fix84 18.11.2026] see quadview::Settings::soft_outlines (never persisted)
    bool m_ui_steady{true};            // [MENUS fix86 18.11.2026] see quadview::Settings::ui_steady (never persisted)
    bool m_dump_periphery_full{false}; // [PERIPHERY DUMP fix84 18.11.2026] see quadview::Settings::dump_periphery_full (never persisted)
    bool m_real_view_scale{true};      // [VIEW SCALE 15.11.2026] looked-at objects at their real distance (never persisted)
    bool m_screen_effects_off{true};   // [SCREEN EFFECTS 15.11.2026] lens / video tape effects off in the quad view (never persisted)
    bool m_image_quality_hold{true};   // [IMAGE QUALITY 15.11.2026] the wide view at the fovea views' size (never persisted)
    int m_wide_occlusion{0};           // [OCCLUSION 15.11.2026] the engine's occlusion culling: 0 as the game, 1 off in the wide
                                       // view, 2 off in every view (never persisted; [fix88 18.11.2026] 0 at the start, was 1)
    int m_wide_occlusion_applied{0};   //   what is applied now
    int m_wide_occlusion_frames{0};
    std::vector<std::pair<const void*, bool>> m_wide_occlusion_original{}; // switches changed by it (the values found)
    // [FLICKER TEST 15.11.2026] one engine switch at a time while the quad view is engaged, to find what makes a light
    // flicker in the wide view (top of the stairs in RE7): 0 none, 1 fovea views almost empty, 2 fovea views without dynamic
    // shadows, 3 no dynamic shadows in any view, 4 the head's view for every other thread during view updates, 5 the
    // engine's level of detail off (never persisted)
    int m_flicker_test{0};
    int m_flicker_applied{0};
    int m_flicker_frames{0};
    bool m_flicker_shadows_original{true};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void flicker_test_tick(bool engaged);
#endif
    // [SHADOW CACHE 15.11.2026] the engine's shadow cache while the quad view is engaged: 0 as the game, 1 off in the fovea
    // views (their via.render.layer.ShadowCast CacheEnable), 2 off everywhere (via.render.Renderer ShadowCacheEnable, the
    // game's own option). The values found are put back (never persisted).
    // [15.11.2026 evening] as the game from the start: the light flicker test of 19:2x on the stairs found no effect (94 % /
    // 99 %), and the lights under the piano, the armchair and the ceiling stayed calm with "as the game" too (user's test)
    int m_shadow_cache_mode{0};
    int m_shadow_cache_applied{0};
    int m_shadow_cache_frames{0};
    std::vector<std::pair<const void*, bool>> m_shadow_cache_original{}; // the fovea views' layers switched off, the values found
    std::optional<bool> m_shadow_cache_global_original{};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool set_fovea_shadow_cache(bool on);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool set_global_shadow_cache(bool on);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void shadow_cache_tick(bool engaged);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string describe_shadow_layers();
#endif
    // [LIGHT FLICKER TEST 15.11.2026] automatic: after a countdown, phases of one engine switch each while the flicker of the
    // eye images is measured (d3d12::EyeRecorder); the result goes into the log
    // [15.11.2026 evening] second round: fog, the engine's dynamic shadows, the fovea views' frustum, level of detail and the
    // view of other threads; the periphery (outside the fovea rectangles) measured on its own. Nothing is kept by itself.
    // [PROJECTION CALLERS 15.11.2026] third round: the phases are chosen at the start (the fovea cameras' projection requests
    // by context and call site so far, quadview::State::proj_sites)
    std::atomic<bool> m_lft_request{false}; // the menu's button (the test starts on the game thread)
    bool m_lft_running{false};
    int m_lft_phase{-1};                 // -1 countdown, 0.. phases, m_lft_phases.size() waiting for the result
    struct LftPhase {
        std::string name{};
        int sw{0};          // what the phase switches (LftSwitch in FoveatedRendering.cpp)
        uint32_t ctx{0};    // the contexts (one bit each) whose fovea camera requests get the eye's whole frustum
        uintptr_t site{0};  // the call site whose fovea camera requests get it
    };
    std::vector<LftPhase> m_lft_phases{};
    std::vector<std::array<uint64_t, 2>> m_lft_matrix{}; // per phase, measured part: fovea view updates with narrow / whole matrices
    std::string m_lft_sites_text{};                      // the call sites at the start (for the result)
    std::chrono::steady_clock::time_point m_lft_t0{};
    bool m_lft_shadows_original{true};   // the engine's dynamic shadows before the phase that switches them off
    bool m_lft_shadows_known{false};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void lft_start();
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void lft_tick(bool engaged);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void lft_set_phase_switches(int phase, bool on);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void lft_finish(const std::vector<quadview::State::FlickerRow>& rows);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void lft_cancel(const std::string& why);
#endif
    int m_test_variant{0};             // [GPU TEST 29.10.2026] variant of the GPU pass test chosen by hand (0 = none, never persisted)
    uint64_t m_periodic_reproj{0};   // counters at the last periodic line
    uint64_t m_periodic_skipped{0};
    uint64_t m_periodic_ui{0};
    uint64_t m_periodic_ui_none{0};
    bool m_dump_snapshot_written{false}; // one status snapshot per dump, before its first report piece
    // [DUMP DELAY 24.10.2026] "Dump in 4 s": when the delayed sequence starts (time_point{} = none pending), so the
    // hands are free to move the weapon or look at something while it is taken
    std::chrono::steady_clock::time_point m_dump_at{};
    static constexpr int DELAYED_DUMP_FRAMES = 20;
    static constexpr int DUMP_DELAY_SECONDS = 4;
    // [VIDEO 25.10.2026] "Video in 4 s": when the recording starts (time_point{} = none pending), see d3d12::EyeRecorder
    std::chrono::steady_clock::time_point m_video_at{};
    static constexpr int VIDEO_DELAY_SECONDS = 4;
    static constexpr double VIDEO_SECONDS = 4.0;

    // Size/ratio sliders re-create the upscalers AND the engine render targets: apply them only once the
    // slider has been still for a moment.
    float m_applied_fraction{0.40f};
    float m_applied_ratio{1.20f};
    std::chrono::steady_clock::time_point m_last_size_edit{};
    bool m_size_edit_pending{false};

    // ---- runtime state --------------------------------------------------------------------------
    bool m_options_dirty{true};
    bool m_zoomed{false};
    bool m_requested{false};
    uint64_t m_frame{0};

    // ---- debug / benchmark state ----------------------------------------------------------------
    // [CPU 28.10.2026] frame times of the periodic interval (quarter milliseconds up to 100 ms) and the longest
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string format_frame_times() const;
#endif
    // [CPU PROFILE 03.11.2026] the game's main thread per application entry (BeginRendering, UpdateBehavior, ...): its time
    // from our pre to our post callback (the engine's work in it and the mods' callbacks in between), summed per interval
    struct EntryStat {
        std::atomic<size_t> hash{0};
        std::atomic<const char*> name{nullptr};
        std::atomic<uint64_t> ns{0};
        std::atomic<uint64_t> n{0};
        std::atomic<uint64_t> max_ns{0};
    };
    std::array<EntryStat, 512> m_entry_stats{};
    std::atomic<uint64_t> m_entry_frames{0};    // BeginRendering calls since the last report
    std::atomic<uint64_t> m_entry_lost{0};      // entries not counted (table full)
    int64_t m_entry_report_ns{0};               // when the last report was made (steady clock)
    uint64_t m_entry_wait_ns_prev{0};           // the runtime's sync wait total at the last report
    uint64_t m_entry_waits_prev{0};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string format_entry_profile();
#endif
    // [CPU 03.11.2026] the render thread's present path, the mod's GPU waits and the transform hook since the last line
    struct RenderThreadTotals {
        uint64_t presents{0}, hook_ns{0}, lock_wait_ns{0}, mods_ns{0}, present_ns{0}, post_ns{0};
        uint64_t fence_ns{0}, fence_calls{0}, fence_long{0}, transform_calls{0}, transform_ns{0};
        // [SUBMIT 05.11.2026] cpustats::VrSubmit
        uint64_t vr_frames{0}, vr_ns{0}, submits{0}, submit_ns{0}, submit_slow{0}, handoffs{0}, handoff_ns{0};
        uint64_t not_responding{0}, resumed{0}, out_of_video_memory{0};
        uint64_t lock_waits_present{0}, lock_wait_ns_present{0}, lock_waits_poses{0}, lock_wait_ns_poses{0}, lock_timeouts{0}; // [COMPOSITOR LOCK]
        // [FRAME TIMING 05.11.2026] cpustats::VrTiming
        uint64_t ft_frames{0}, ft_gpu_us{0}, ft_comp_us{0}, ft_over{0}, ft_late_gpu{0}, ft_late_cpu{0}, ft_throttled{0}, ft_mis{0}, ft_dropped{0};
    };
    RenderThreadTotals m_rt_prev{};
    // [COLLAPSE 05.11.2026] see collapse_watch
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void collapse_watch(std::chrono::steady_clock::time_point now, bool active);
#endif
    std::chrono::steady_clock::time_point m_cw_window{};
    uint64_t m_cw_presents{0};
    uint64_t m_cw_mods_ns{0};
    uint64_t m_cw_submits{0};
    uint64_t m_cw_submit_ns{0};
    int m_cw_slow_windows{0};
    double m_cw_slow_mods_ms{0.0};   // the slow windows' present callbacks per frame, summed (for the note)
    double m_cw_slow_submit_ms{0.0}; // ... and their Submit time per call
    std::chrono::steady_clock::time_point m_cw_quiet_until{}; // tests, frame dumps and videos hold frames on purpose
    std::chrono::steady_clock::time_point m_cw_noted_at{};    // the next note of a collapse without a switch
    // [GPU GUARD fix91 19.11.2026] see gpu_guard_watch: the headset's frame time, read once, and the guard that makes the
    // supersampling TAA lighter (fovea and periphery at once) while the GPU is short of time
    void gpu_guard_watch(std::chrono::steady_clock::time_point now, bool active);
    quadview::GpuGuard m_gpu_guard{};
    // [fix96 19.11.2026] the guard's step: 0 the menu's choices, 1 the first thing lightened (the periphery's image if
    // the menu has it at twice the size, else the fovea's history), 2 the fovea's history as well
    int m_gpu_level{0};
    bool m_gpu_stage_periphery{false}; // the first step is the periphery's
    // [GUARD fix101 20.11.2026] the steps the guard can take from the menu's choices, in their order (GS_*, see
    // gpu_guard_watch), the level current_settings applies (the guard's own, or one more during the frames the
    // supersampling TAA runs behind the calm one before taking over again) and until when those frames last
    uint8_t m_gpu_steps[4]{};
    int m_gpu_step_count{0};
    int m_gpu_level_shown{0};
    std::chrono::steady_clock::time_point m_gpu_preroll_until{};
    bool m_gpu_relief{false};          // the lighter choices are in use because of the guard (see current_settings)
    double m_gpu_relief_t{0.0};        // ... since this time of the session (s)
    uint32_t m_gpu_reliefs{0};         // ... how many times in this session
    std::chrono::steady_clock::time_point m_gg_window{};
    uint64_t m_gg_frames{0}, m_gg_late_gpu{0}, m_gg_total_us{0}, m_gg_over{0}, m_gg_comp_us{0};
    uint64_t m_gg_mis{0}, m_gg_throttled{0}; // [fix102] frames shown on another vsync, frames throttled by the compositor
    double m_gg_rt_sum{0.0};           // the engine's render time, summed over the window
    int m_gg_rt_n{0};
    double m_ours_gpu_ms{2.2};         // the mod's GPU time per frame (periodic line, "wall" span)
    float m_hmd_frame_ms{0.0f};        // the headset's frame time (display frequency), 0 = not read yet
    // [MEMORY 05.11.2026] the highest video memory use since the periodic line, sampled once a second
    std::chrono::steady_clock::time_point m_mem_at{};
    uint64_t m_mem_peak_local{0};
    uint64_t m_imgui_prev_frames{0}, m_imgui_prev_ns{0}, m_imgui_prev_lock_ns{0};
    uint64_t m_gui_prev_calls{0}, m_gui_prev_mods_ns{0}, m_gui_prev_game_ns{0}; // [GUI 04.11.2026]
    uint64_t m_glue_prev_calls{0}, m_glue_prev_calls_ns{0}, m_glue_prev_lua{0}, m_glue_prev_lua_ns{0}; // [GLUE 04.11.2026]
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    static std::string ray_tracing_text(); // [RT 04.11.2026] the game's ray tracing settings (render config), for the log
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string format_render_thread(double frames);
#endif
    // [CPU THREADS 03.11.2026] the threads the busy-ness line names, and the frames it divides by
    std::atomic<uint32_t> m_main_tid{0};
    std::atomic<uint32_t> m_draw_tid{0};
    std::atomic<uint64_t> m_frames_total{0};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void request_thread_sample();
#endif
    // [TIMELINE 03.11.2026] a recording of what happens when, on which thread, for a number of frames (cpustats::Timeline),
    // appended to foveation_timeline.csv by a thread of its own
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void timeline_start(const std::string& label, int frames);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void timeline_tick();   // once per frame (BeginRendering)
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void timeline_finish();
#endif
    int m_tl_frames_left{0};
    std::string m_tl_label{};
    uint32_t m_tl_count{0};
    std::atomic<bool> m_tl_request{false};
    std::array<uint32_t, 401> m_frame_hist{};
    double m_frame_max_ms{0.0};
    // [CPU 28.10.2026] the debug file is written by its own thread (debug_log only queues); the queue is shared with
    // that thread, which keeps it alive
    struct LogQueue {
        std::mutex mutex{};
        std::condition_variable cv{};
        std::deque<std::string> queue{};
        std::atomic<bool> stop{false};
        bool started{false};
        // [CPU THREADS 03.11.2026] a queue entry of the single character '\x01' stands for the next of these: the writer
        // thread measures the threads' CPU time there and writes that line in its place
        struct ThreadSampleRequest {
            uint64_t frames{0};
            uint32_t main_tid{0};
            uint32_t present_tid{0};
            uint32_t draw_tid{0};
        };
        std::deque<ThreadSampleRequest> thread_requests{};
    };
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    static void log_writer(std::shared_ptr<LogQueue> log);
#endif
    std::shared_ptr<LogQueue> m_log{std::make_shared<LogQueue>()};
    std::chrono::steady_clock::time_point m_start_time{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point m_last_begin{};
    std::chrono::steady_clock::time_point m_last_periodic{};
    double m_cpu_frame_ms{0.0};
    double m_periodic_cpu_sum{0.0};
    uint32_t m_periodic_frames{0};
    // [CPU 04.11.2026] the status line is due: written at the next WaitRendering (the main thread waits for the render
    // thread there), or at the next BeginRendering if no WaitRendering came in between
    bool m_periodic_due{false};
    bool m_periodic_wr_seen{false}; // a WaitRendering came since it was due (it waits there for a free frame, see below)

    struct Counters {
        uint64_t wide{0}, inset{0}, by_scope{0}, by_camera{0}, gui{0}, evaluations{0}, skipped{0}, moves{0};
    };
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    Counters read_counters() const;
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    CpuTotals read_cpu() const;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string format_cpu(const CpuTotals& now, const CpuTotals& prev, double frames) const;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string format_mod_costs(size_t top) const;
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void layer_begin(sdk::renderer::layer::Scene* layer, int op);
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void layer_end(int op);
#endif

    std::array<std::array<std::atomic<uint64_t>, 2>, 3> m_cpu_ns{};
    std::array<std::array<std::atomic<uint64_t>, 2>, 3> m_cpu_n{};
    std::atomic<uint64_t> m_rendering_ns{0};
    std::atomic<uint64_t> m_rendering_n{0};
    std::chrono::steady_clock::time_point m_begin_rendering{};
    CpuTotals m_cpu_periodic_prev{};
    CpuTotals m_cpu_bench_prev{};
    uint32_t m_bench_frames{0};
    Counters m_periodic_prev{};
    Counters m_ui_prev{};
    Counters m_ui_rate{};
    std::chrono::steady_clock::time_point m_ui_rate_time{};

    d3d12::GpuTimer m_gpu_timer{};
    bool m_gpu_timing_user{false};
    // [STEREO FOVEA] GPU time is measured by itself while quad view is on; the command queue is not known in the first
    // frames, so the attempt is repeated every few seconds until it works (or the user turns it off)
    std::chrono::steady_clock::time_point m_gpu_timing_auto_next{};
    bool m_gpu_timing_auto_off{false};
    std::string m_debug_message{};      // through set_message / get_message (the menu reads it on another thread)
    mutable std::mutex m_text_mutex{};

    bool m_bench_running{false};
    int m_bench_phase{0};
    bool m_bench_measuring{false};
    std::chrono::steady_clock::time_point m_bench_phase_start{};
    double m_bench_cpu_sum{0.0};
    uint32_t m_bench_cpu_n{0};
    std::vector<BenchRow> m_bench_rows{};
    std::string m_bench_summary{};

    // [GPU TEST 29.10.2026] GPU pass test (phase -1 = the countdown before the first)
    bool m_ptest_running{false};
    int m_ptest_phase{-1};
    bool m_ptest_measuring{false};
    std::chrono::steady_clock::time_point m_ptest_phase_start{};
    double m_ptest_cpu_sum{0.0};
    double m_ptest_rt_sum{0.0}; // [HUD BLUR fix114] the engine's render time over the phase
    uint64_t m_ptest_rt_n{0};
    uint32_t m_ptest_cpu_n{0};
    std::array<uint64_t, 2> m_ptest_paths_start{}; // [ONE LIST 01.11.2026] quadview::State::path_frames when the measurement began
    std::vector<PassTestRow> m_ptest_rows{};
    std::string m_ptest_summary{};      // through set_ptest_summary / get_ptest_summary
    bool m_ptest_engaged{true};         // quad view engaged on every measured frame of the phase
    std::atomic<int> m_ptest_request{0}; // from the menu: 1 = start, 2 = cancel (handled in update_frame)

    // [ENGINE VIEWS 30.10.2026] engine view cost test (phase -1 = the countdown before the first)
    struct ViewTestRow {
        int phase{0};
        float ratio{1.0f};
        bool four_views{false};
        uint32_t render_w{0};
        uint32_t render_h{0};
        uint32_t views{3};
        bool engaged{false};
        double cpu_ms{0.0};
        uint32_t frames{0};
        d3d12::GpuTimer::Result gpu{};
        std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT> spans{};
    };
    bool m_vtest_running{false};
    int m_vtest_phase{-1};
    bool m_vtest_measuring{false};
    bool m_vtest_engaged{true};
    std::chrono::steady_clock::time_point m_vtest_phase_start{};
    double m_vtest_cpu_sum{0.0};
    uint32_t m_vtest_cpu_n{0};
    float m_vtest_fraction{0.28f}; // the sizes at the start of the test (the phases scale these)
    float m_vtest_ratio{1.0f};
    std::vector<ViewTestRow> m_vtest_rows{};
    std::atomic<int> m_vtest_request{0};          // from the menu: 1 = start, 2 = cancel
    std::atomic<bool> m_layer_dump_request{false}; // from the menu: write the render layer tree

    // [ENGINE SHADOWS 30.10.2026] engine shadow test (phase -1 = the countdown before the first)
    struct ShadowTestRow {
        int phase{0};
        bool shadows{true};
        bool engaged{false};
        uint32_t frames{0};
        double cpu_ms{0.0};
        double render_time_ms{-1.0}; // the engine's own (via.render.Renderer get_RenderTime), average
        d3d12::GpuTimer::Result gpu{};
        std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT> spans{};
    };
    bool m_stest_running{false};
    int m_stest_phase{-1};
    bool m_stest_measuring{false};
    bool m_stest_engaged{true};
    std::chrono::steady_clock::time_point m_stest_phase_start{};
    double m_stest_cpu_sum{0.0};
    uint32_t m_stest_cpu_n{0};
    double m_stest_rt_sum{0.0};
    uint32_t m_stest_rt_n{0};
    double m_rt_sum{0.0}; // the engine's render time over the periodic interval
    int m_grip_frames{0}; // [AIM COST fix112] frames of the periodic interval with the right grip held, of m_grip_n
    int m_grip_n{0};
    uint32_t m_rt_n{0};
    bool m_stest_original{true};          // the engine's switch before the test (put back after it, on the game thread)
    std::vector<ShadowTestRow> m_stest_rows{};
    std::atomic<int> m_stest_request{0};           // from the menu: 1 = start, 2 = cancel; 3 = cancel after a device reset
    std::atomic<bool> m_shadow_dump_request{false}; // from the menu: write the shadow layer details
    std::atomic<bool> m_ao_dump_request{false};     // from the menu: write the ambient occlusion details
    std::atomic<bool> m_vrs_dump_request{false};     // from the menu: write the variable rate shading details
    // [WHITE POINT 13.11.2026] the types holding the engine's white point / histogram resources, and a record of
    // what they return at the post effect draws of the next frames (which views, in which order, which resources)
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string white_point_report();
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void white_point_record(sdk::renderer::layer::PostEffect* layer, bool post);
#endif
    std::atomic<int> m_wp_probe_left{0};
    std::mutex m_wp_mutex{};
    std::string m_wp_text{};
    std::atomic<bool> m_exposure_dump_request{false}; // [EXPOSURE 01.11.2026] from the menu: the exposure / tone mapping details
    std::atomic<bool> m_lod_dump_request{false};      // [LOD 06.11.2026] from the menu: the level of detail and culling details
    std::atomic<bool> m_shaft_dump_request{false}; // [LIGHT SHAFT 13.11.2026]
    std::atomic<int> m_st_request{0};                 // [SUBMIT TIMELINE 06.11.2026] from the menu: 1 on, 2 off
    uint32_t m_st_lines{0};                           // status lines since it started (every sixth with one frame in detail)
    std::atomic<bool> m_colour_dump_request{false};   // [COLOUR CORRECTION 02.11.2026] from the menu: the colour correction details
    bool m_copy_colour_correct{false};                // (not saved) the fovea views get the wide view's colour correction values
    bool m_ftest_renderer_off{false};                 // the effect test switched the renderer's dynamic shadows off
    // [EXPOSURE TEST 01.11.2026] (not saved) the views' tone mapping for a test, see CameraDuplicator::ToneTest
    bool m_log_view_diff{false}; // [VIEW DIFFERENCE 01.11.2026] (not saved) the fovea / periphery difference in the 5 s line
    int m_tt_local{0};          // 0 as the game, 1 off in the fovea views, 2 off in all views
    bool m_tt_copy_auto{false}; // the fovea views take the wide view's automatic exposure
    int m_tt_auto{0};           // 0 as the game, i > 0: value i - 1 of the list in all views
    int m_tt_ev_lock{0};        // [EV LOCK 13.11.2026] 0 off, 1 fovea views, 2 all views: exposure range closed on the game camera's
    float m_tt_ev_offset{0.0f}; // [EV FOLLOW 13.11.2026] the fovea views' EV = the game camera's + this
    bool m_tt_ev_follow{false}; // [EV FOLLOW 13.11.2026] + an offset following the measured periphery / fovea brightness
    int m_tt_clone_adapt{0};    // [ADAPT 13.11.2026] the fovea views' own adaptation: 0 as the game, 1 rates 0, 2 rates 1000
    int m_tt_real_ev{1};        // [REAL EV 13.11.2026] the fovea views get the game camera's adapted exposure (CameraDuplicator::ToneTest::real_ev)
    bool m_tt_real_ev_steady{true}; // [REAL EV fix105] the game camera's exposure measured every 8th frame while it is steady (ToneTest::real_ev_steady)
    std::atomic<bool> m_effects_dump_request{false}; // [VIEW EFFECTS 13.11.2026] the effect components of the three views (describe_engine_types(8))
    bool m_effects_dump_done{true}; // [DEFAULTS 15.11.2026] the real exposure is on from the start: the views' effect values only on the button
    float m_tt_light_limit{0.0f};   // [GLOW 13.11.2026] (test, not saved) the large-scale light's limit, 0 = built-in
    bool m_tt_light_add_only{false};
    bool m_tt_light_auto{true};     // [AUTO LIGHT 14.11.2026] (not saved) [DEFAULTS 15.11.2026] on
    mutable bool m_tone_defaults_sent{false}; // [DEFAULTS 15.11.2026] the exposure defaults given to the camera duplicator once
    mutable bool m_menu_light_paused{false};  // [MENU LIGHT 15.11.2026]
    bool m_ls_off{false};                              // [LIGHT SHAFT OFF 13.11.2026] see light_shafts_apply
    std::atomic<bool> m_ls_changed{false};
    uint32_t m_ls_frames{0};
    std::vector<std::pair<::REManagedObject*, std::string>> m_ls_objects{}; // switched off by us (our reference) and the property
    bool m_exposure_dump_done{false}; // [EXPOSURE MEMBERS 13.11.2026] the exposure details once, at the first exposure test
    std::vector<std::pair<std::string, int32_t>> m_auto_values{}; // via.render.ToneMapping's automatic exposure values
    bool m_auto_values_read{false};
    std::string tone_test_text() const;

    // [ENGINE FEATURES 30.10.2026] engine effect cost test (phase -1 = the countdown before the first)
    struct FeatureTestRow {
        int feature{-1}; // -1 = everything on
        uint32_t objects{0};
        uint32_t reapplied{0};
        bool engaged{false};
        uint32_t frames{0};
        double cpu_ms{0.0};
        double render_time_ms{-1.0};
        d3d12::GpuTimer::Result gpu{};
        std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT> spans{};
        // [VIEW DIFFERENCE 01.11.2026] mean fovea / periphery difference (one gain; as matched), and how many measurements
        double view_diff{0.0};
        double view_diff_matched{0.0};
        uint32_t view_diff_n{0};
    };
    bool m_ftest_running{false};
    double m_ftest_vd_sum{0.0};
    double m_ftest_vdm_sum{0.0};
    uint32_t m_ftest_vd_n{0};
    uint64_t m_ftest_vd_seen{0};
    int m_ftest_phase{-1};
    int m_ftest_feature{-1}; // the effect switched off in this phase, -1 = everything on
    int m_ftest_next{0};     // the next effect to try
    bool m_ftest_measuring{false};
    bool m_ftest_engaged{true};
    uint32_t m_ftest_reapplied{0};
    std::chrono::steady_clock::time_point m_ftest_phase_start{};
    double m_ftest_cpu_sum{0.0};
    uint32_t m_ftest_cpu_n{0};
    double m_ftest_rt_sum{0.0};
    uint32_t m_ftest_rt_n{0};
    std::vector<std::pair<::REManagedObject*, bool>> m_ftest_objects{}; // switched-off components (our reference) and their value
    std::vector<FeatureTestRow> m_ftest_rows{};
    std::atomic<int> m_ftest_request{0}; // from the menu: 1 = start, 2 = cancel; 3 = cancel after a device reset

    // [RENDER SETTINGS 30.10.2026] render settings cost test (phase -1 = the countdown before the first)
    struct SettingVariant {
        int setting{0};      // index into the test's list
        int32_t value{0};    // the value tried
        std::string name{};  // the value tried, as text
        bool is_bool{false};
    };
    struct SettingTarget {
        ::REManagedObject* object{nullptr}; // a reference of ours during the phase
        int32_t original{0};                // its own value
        int32_t value{0};                   // the value it is kept at in the phase
    };
    std::vector<SettingTarget> m_rtest_targets{};
    struct SettingTestRow {
        int variant{-1}; // -1 = everything as it is
        uint32_t reapplied{0};
        bool engaged{false};
        uint32_t frames{0};
        double cpu_ms{0.0};
        double render_time_ms{-1.0};
        d3d12::GpuTimer::Result gpu{};
        std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT> spans{};
    };
    bool m_rtest_running{false};
    int m_rtest_phase{-1};
    int m_rtest_variant{-1}; // the variant of this phase, -1 = everything as it is
    int m_rtest_next{0};
    bool m_rtest_measuring{false};
    bool m_rtest_engaged{true};
    uint32_t m_rtest_reapplied{0};
    std::chrono::steady_clock::time_point m_rtest_phase_start{};
    double m_rtest_cpu_sum{0.0};
    uint32_t m_rtest_cpu_n{0};
    double m_rtest_rt_sum{0.0};
    uint32_t m_rtest_rt_n{0};
    std::vector<SettingVariant> m_rtest_variants{};
    std::vector<SettingTestRow> m_rtest_rows{};
    std::atomic<int> m_rtest_request{0}; // from the menu: 1 = start, 2 = cancel; 3 = cancel after a device reset
    // [CPU TEST 03.11.2026]
    bool m_ctest_running{false};
    int m_ctest_phase{-1};
    int m_ctest_kind{0};
    uint32_t m_ctest_frames{0};
    bool m_ctest_timeline_done{false};
    bool m_ctest_shadows_original{true};
    std::atomic<bool> m_ctest_skip_fovea_overlay{false}; // [CPU TEST 04.11.2026] kind 5 (read by the layer hooks' threads)
    std::atomic<uint64_t> m_ctest_overlay_skipped{0};
    // [OVERLAY TEST 04.11.2026] m_skip_fovea_ui_update as the layer hooks' threads read it (copied once per frame on the main
    // thread): 0 the views' overlay layer update as the game does it, 1 not in the fovea views (only while the VR mod keeps
    // the overlay layers from drawing: headset on, VR not suspended). Leaving it out in all views as well made the GPU device
    // go lost 5-22 s after it was chosen, three times in the tests of 04.11.2026 (the only device removals in the logs since
    // 26.10): not offered.
    std::atomic<int> m_overlay_skip{0};
    std::atomic<uint64_t> m_overlay_skipped{0}; // overlay updates left out (all reasons), for the status line
    uint64_t m_overlay_skipped_prev{0};
    std::chrono::steady_clock::time_point m_ctest_phase_start{};
    std::atomic<int> m_ctest_request{0}; // from the menu: 1 = start, 2 = cancel; 3 = cancel after a device reset
    std::vector<std::pair<const void*, bool>> m_fovea_shadow_original{}; // shadow cast layers of the fovea views switched off
    // [GPU TEST 06.11.2026]
    bool m_ctest_gpu{false};
    const int* m_ctest_seq{nullptr};
    int m_ctest_count{0};
    bool m_ctest_started_timeline{false};
    std::vector<std::pair<const void*, bool>> m_occlusion_original{}; // occlusion culling switches changed (the values found)
    // [GPU TEST 06.11.2026 evening] the engine's level of detail switch (via.render.Renderer DisableLod) and the game window
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool set_lod_disabled(bool disable); // true when the switch was found and set (or put back)
#endif
    bool m_lod_changed{false};
    bool m_lod_original{false};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    std::string window_present_info(bool at_end);
#endif
    uint64_t m_win_present_count{0};   // DXGI frame statistics at the start of the GPU test
    uint64_t m_win_sync_refresh{0};
    int64_t m_win_sync_qpc{0};
};
