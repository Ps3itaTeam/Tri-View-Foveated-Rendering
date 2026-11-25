#pragma once



#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <utility/FunctionHook.hpp>
#include <sdk/intrusive_ptr.hpp>

#include "vr/d3d12/SharpenPass.hpp"
#include "vr/d3d12/CommandContext.hpp"
#include "vr/d3d12/TextureContext.hpp"
#include "vr/d3d12/FoveaComposite.hpp"   // [QUADVIEW]
#include "vr/d3d12/TextureDump.hpp"      // [QUADVIEW] debug frame dump
#include "vr/d3d12/GpuTimer.hpp"         // [GPU BREAKDOWN] GpuSpans
#include "vr/d3d12/FsrDirect.hpp"        // [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod
#include "vr/d3d12/SuperTAA.hpp"         // [SUPER TAA 16.11.2026] the mod's supersampling TAA in the fovea
#include "vr/d3d12/SuperTAACapture.hpp"  // [SUPER TAA 16.11.2026] ... and its capture (debug)
#include "vr/QuadView.hpp"               // [QUADVIEW]
#include "Mod.hpp"

namespace sdk {
namespace renderer {
class RenderLayer;

namespace layer {
class Scene;
}
}
}

namespace sdk {
struct RETypeDefinition;
struct REMethodDefinition;
}

class TemporalUpscaler : public Mod {
public:
    // [AFW fix91 19.11.2026] the members of the flat screen's "UI fix" were here (copies of the image without and with the
    // interface, the interface extracted with the AFW plugin's renderer); it never ran in VR and went with the plugin

public:
    static std::shared_ptr<TemporalUpscaler>& get();

    std::string_view get_name() const { return "TemporalUpscaler"; }

    // [LIGHT FLICKER TEST 15.11.2026 evening] fog and volumetric fog off for one phase of the light flicker test (not saved);
    // when it ends, the components get the options' values back once
    void set_test_fog_off(bool off) { m_test_fog_off.store(off, std::memory_order_relaxed); }

    std::optional<std::string> on_initialize() override;
    std::optional<std::string> on_initialize_d3d_thread() override;

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;

    // ([NO PLUGIN fix91 19.11.2026] apply_upscale_type_from_config was here: the choice among the plugin's upscale methods)

    void on_draw_ui() override;
    void draw_settings(); // Inhalt ohne Header, fuer die Menue-Kategorie "Upscaler"
    void draw_post_sliders(); // [28.09.2026] Sharpness/Saturation/Reset, ganz oben in der Kategorie

    // [POST_PROCESSING 28.09.2026 -- Ansage des Users, aus der Farbe-Lua] Nebel
    // und Volumetrischer Nebel abschaltbar. Laeuft in pre-BeginRendering auf ALLEN
    // Instanzen (MainCamera + MainCamera (Clone)) -- wie zzz_re4_farbe_test.lua.
    void apply_post_processing();
    void on_early_present() override; // early because it needs to run before VR.

    void on_post_present() override;
    void on_device_reset() override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_view_get_size(REManagedObject* scene_view, float* result) override;
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;

    void on_scene_layer_update(sdk::renderer::layer::Scene* scene_layer, void* render_context) override;
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* scene_layer, void* render_context) override; // [JITTER CHECK 10.11.2026]
#endif
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* scene_layer, void* render_context) override;
#endif
    // [SUPER TAA 16.11.2026] is the supersampling TAA anti-aliasing the fovea (menu, log)
    bool super_taa_failed() const { return m_super_taa_failed; }
    // [PERIPHERY SUPER fix80 17.11.2026] what the last frame's periphery got (0 = the light TAA, 1 = the supersampling TAA
    // with an image of the wide view's size, 2 = of twice that size) and, when it is less than asked for, why
    enum PeripheryWhy : uint32_t { PW_NONE = 0, PW_NOT_SHARED, PW_FAILED, PW_SIZES, PW_NO_2X, PW_NO_IMAGE, PW_DENSE };
    uint32_t periphery_running() const { return m_periphery_running.load(std::memory_order_relaxed); }
    uint32_t periphery_why() const { return m_periphery_why.load(std::memory_order_relaxed); }
    static const char* periphery_why_text(uint32_t why) {
        switch (why) {
        case PW_NOT_SHARED: return "only with the shared periphery";
        case PW_FAILED: return "a pass failed: see the framework log";
        case PW_SIZES: return "the wide view's depth or motion has another size than its colour";
        case PW_NO_2X: return "the reprojection's passes for the image of twice the size could not be made: image of the wide view's size";
        case PW_NO_IMAGE: return "its image could not be made";
        case PW_DENSE: return "the eye image is less than 3 times its share of the wide view per side: image of the wide view's size";
        default: return "";
        }
    }

    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    void on_overlay_layer_draw(sdk::renderer::layer::Overlay* overlay_layer, void* render_context) override;

    bool on_pre_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) override;
    void on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) override;

    bool on_pre_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_context) override;
    bool on_pre_output_layer_update(sdk::renderer::layer::Output* layer, void* render_context) override;
    void on_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_context) override;

    bool ready() const {
        return m_initialized && m_backend_loaded && m_enabled->value() && !m_wants_reinitialize;
    }

    bool activated() const {
        return m_initialized && m_backend_loaded && m_enabled->value();
    }

    // [QUADVIEW] read by FoveatedRendering (menu, debug log)
    bool is_d3d12() const { return m_is_d3d12; }
    std::string get_quadview_status() const {
        std::scoped_lock _{m_qv_status_mutex};
        return m_qv_status;
    }
    quadview::Layout get_quadview_layout() const { return m_qv_layout; }
    std::array<uint32_t, 2> get_backbuffer_size() const { return m_backbuffer_size; }

    uint32_t get_evaluate_id(uint32_t counter) const {
        return (counter % 2) + 1;
    }

    // [STEREO FOVEA 27.10.2026] the finished image of `eye` for the multipass submit: with the quad view engaged in
    // multipass our own composited eye image (periphery + fovea)
    // [NO PLUGIN fix91 19.11.2026] ... otherwise the eye image the mod's supersampling TAA writes (evaluate_plain; it was the
    // upscaler plugin's result texture). Made with the set-up, like the plugin's: the flat canvas gets the window's
    // picture only while both exist (D3D12Component::on_frame).
    ID3D12Resource* get_eye_image(int32_t eye) {
        if (eye < 0 || eye > 1) {
            return nullptr;
        }

        if (m_qv_layout.valid && m_qv_stereo) {
            // nullptr until the first composite
            return m_qv_eye_image[eye].Get();
        }

        return m_plain_eye[eye].Get();
    }

    // [DIRECT 8 BIT 30.10.2026] the quad view's eye image of the last evaluation (present thread, read by the submit right
    // after it), or null without the quad view
    ID3D12Resource* qv_eye_image_now(int32_t eye) const {
        return eye >= 0 && eye < 2 && m_qv_layout.valid && m_qv_stereo ? m_qv_eye_image[eye].Get() : nullptr;
    }

    // [STEREO FOVEA] number of scene layers the upscaler found at the last EndRendering and their view ids (log)
    std::string get_layer_report() const {
        std::scoped_lock _{m_qv_status_mutex};
        return m_layer_report;
    }

    // [MONO UI] interface of the last frame (log)
    std::string get_ui_report() const {
        std::scoped_lock _{m_qv_status_mutex};
        return m_ui_report;
    }

    auto get_motion_scale() { return m_motion_scale;}

    // ([NO PLUGIN fix91 19.11.2026] the plugin's enumerations were here: graphics API, upscale type, quality level, and
    // the order of the quality levels in the menu)


private:
    bool m_wide_depth_size_mismatch{false}; // [IMAGE QUALITY 15.11.2026]
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool on_first_frame();
    bool init_upscale_features();

    // [QUADVIEW] multipass only: both eyes are rendered every frame, each as its fovea view plus the periphery (one
    // shared wide view, or one wide view per eye for comparison)
    bool qv_engaged() const { return m_qv_layout.valid; }
    // fovea instances use plugin ids 3 (left) and 4 (right)
    uint32_t get_qv_fovea_id(uint32_t counter) const { return (counter % 2) + 3; }
    bool evaluate_quadview(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame);
    // [STEREO FOVEA] one eye of the quad view: periphery + fovea -> composited eye image in m_qv_eye_image[eye] (stereo
    // periphery: eye 0 from states [0]/[2], eye 1 from [1]/[3]); `dump` = this frame is being dumped.
    struct QvEval {
        int32_t eye{0};
        uint32_t slot{0};         // history / gain slot (= eye)
        uint32_t wide_state{0};   // index into m_eye_states
        uint32_t inset_state{1};
        uint32_t wide_id{1};      // plugin instance ids
        uint32_t fovea_id{3};
        bool to_backbuffer{true}; // copy the finished eye image into the backbuffer (the desktop mirror: eye 0 only)
        const char* dump_prefix{""};
        // [MONO PERIPHERY 27.10.2026] this eye's periphery is already made (reprojected from the shared wide view, at the
        // render density, resting in UNORDERED_ACCESS): the spatial upscale starts from it, no TAA pass of its own
        ID3D12Resource* wide_override{nullptr};
        bool wide_override_linear{false};
        // [MONO PERIPHERY] the fovea upscaler reads this instead of the engine copy (interface taken out), same format
        ID3D12Resource* inset_color_override{nullptr};
        // [MONO UI 27.10.2026] the interface of this eye is put together after the composite (d3d12::MonoUi): the shared
        // wide view's and the fovea view's colour (with GUI) and GUI copies; the rectangle is filled in per eye
        bool mono_ui{false};
        ID3D12Resource* ui_centre_color{nullptr};
        ID3D12Resource* ui_centre_hud{nullptr};
        ID3D12Resource* ui_centre_fin{nullptr};
        ID3D12Resource* ui_fovea_color{nullptr};
        ID3D12Resource* ui_fovea_hud{nullptr};
        // [MENUS fix81 17.11.2026] for the eye pass (EyeCompose::Params): the wide view's jitter of this frame (its pixels)
        // [MENUS fix82 17.11.2026] the fovea views went to the anti-aliasing with their interface (EyeCompose::Params::fovea_ui_inside)
        bool ui_fovea_inside{false};
        // [MENUS fix83 17.11.2026] ... and their two interface copies (without / with GUI) for the eye pass to tell where it is
        ID3D12Resource* ui_inside_hud{nullptr};
        ID3D12Resource* ui_inside_fin{nullptr};
        d3d12::UiMotion::Block ui_motion{}; // [UI MOTION fix125] the fovea view's block of the interface's own motion
        float ui_wide_jitter[2]{0.0f, 0.0f};
        d3d12::MonoUi::Params ui_params{};
        // [GPU 29.10.2026] where the wide view's GUI copies differ (this frame), or null
        const d3d12::GuiMap* gui_map{nullptr};
        // [DIRECT 8 BIT 30.10.2026] the eye pass writes this headset texture (8 bit, resting in PIXEL_SHADER_RESOURCE)
        // instead of the 10-bit eye image, or null
        ID3D12Resource* direct_out{nullptr};
        // [ONE SUBMIT 04.11.2026] the upscaler plugin in the fovea submits its recorded work at this eye's call; false: the
        // other eye's call that follows submits both (the plugin's header: with more than one evaluation per frame only the
        // last one executes)
        bool plugin_execute{true};
        // [FOVEA LINEAR 05.11.2026] inset_color_override holds linear light: the plugin's result is encoded after it
        bool fovea_linear{false};
        // [FSR DIRECT 06.11.2026] FSR 4 by the mod ran for this frame on the pre-fovea list (its result is in m_qv_fsr_out)
        bool fsr_direct{false};
    };
    bool evaluate_quadview_eye(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, const QvEval& ev, bool dump,
        const quadview::DumpFrame& df);
    // [MONO PERIPHERY 27.10.2026] multipass quad view with the shared wide view: states [0] wide (both eyes) [1] fovea
    // left [2] fovea right. The wide view gets its TAA once, is reprojected to each eye, then each eye goes through
    // evaluate_quadview_eye with the reprojected periphery.
    bool evaluate_quadview_mono(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, bool dump, const quadview::DumpFrame& df);
    // [PERIPHERY] light TAA of a wide view at the render size (history slot `slot`): the new history image, or nullptr
    // (status set, quad view marked failed)
    // jitter_x / y: this view's jitter in pixels as the upscaler gets it (TAA QUALITY: the frame is rebuilt at the centre)
    ID3D12Resource* periphery_taa(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t slot, ID3D12Resource* color, ID3D12Resource* motion,
        ID3D12Resource* depth, const quadview::Settings& settings, float jitter_x = 0.0f, float jitter_y = 0.0f);
    bool init_quadview_features(uint32_t out_w, uint32_t out_h, uint32_t out_format);
    void release_quadview_features();
    void release_upscale_features();
    void fix_output_layer();
    void update_extra_scene_layer();
    uint32_t get_render_width() const;
public:
    // [SMALL WINDOW fix122 22.11.2026] the size the VR mod gives the game's window with the option on: an engine view's
    // (the last quad view layout's, kept over device resets so the window does not go back and forth) plus one, so that
    // the engine keeps a colour buffer of its own for the scene. False until a layout has been made.
    bool small_window_size(uint32_t& w, uint32_t& h) const {
        const uint32_t rw = m_qv_window_w.load(std::memory_order_relaxed);
        const uint32_t rh = m_qv_window_h.load(std::memory_order_relaxed);

        if (rw == 0 || rh == 0) {
            return false;
        }

        w = rw + 1;
        h = rh + 1;
        return true;
    }

private:
    uint32_t get_render_height() const;
    void eye_image_size(UINT64& w, UINT& h) const;        // [SMALL WINDOW fix122] (see the .cpp)
    bool mirror_fits(ID3D12Resource* backbuffer) const;

    // ([NO PLUGIN fix91 19.11.2026] is_using_native_resolution and the quality-level conversion were here: without the quad view
    // the views are rendered at the eye image's size, see get_render_width)
    void update_motion_scale();

    void on_render_resource_release(sdk::renderer::RenderResource* resource);
    void finish_release_resources();
    static void render_resource_release_hook(sdk::renderer::RenderResource* resource);
    std::unique_ptr<FunctionHook> m_render_resource_release_hook{};
    std::vector<sdk::renderer::RenderResource*> m_queued_release_resources{};
    std::recursive_mutex m_queued_release_resources_mutex{};

    // ([NO PLUGIN fix91 19.11.2026] get_dlss_preset was here)

    bool m_first_frame_finished{false};
    bool m_initialized{false};
    bool m_is_d3d12{false};
    bool m_backend_loaded{false};
    bool m_backbuffer_inconsistency{false};
    bool m_upscale{true};
    bool m_rendering{false};
    bool m_set_view{false};
    bool m_jitter{true};
    bool m_allow_taa{false}; // the engine has its own TAA implementation, it can't be used with the upscaler
    bool m_wants_reinitialize{false};
    bool m_made_extra_scene_layer{false};
    bool m_hooked_resource_release{false};

    // [QUADVIEW] 4 slots: 0/1 = eye instances (wide view in quad view), 2/3 = fovea (inset) instances
    std::array<uint32_t, 4> m_jitter_indices{0, 0, 0, 0};


    // [CAS 15.09.2026] Eigener Schaerfe-Pass hinter dem Upscaler. Er schreibt
    // IN die upscaled Texturen zurueck -- nur die sieht das HMD.
    d3d12::SharpenPass m_sharpen{};

    uint32_t m_backbuffer_inconsistency_start{};
    std::array<uint32_t, 2> m_backbuffer_size{};

    // [NO PLUGIN fix91 19.11.2026] The upscaler plugin's result textures were here (two for the eyes, two for the fovea).
    // m_features: the set-up is done (quad view or not) and release_upscale_features has something to release.
    // Without the quad view (multipass): the mod's supersampling TAA anti-aliases each eye's view into m_plain_eye
    // (evaluate_plain): an instance of its own, the frame each eye last got its pass in (a gap starts its history again).
    // The eye images are made with the set-up (init_upscale_features). m_plain_failed: a pass failed, they are no longer
    // written until the next set-up. m_plain_aa_now: the pass ran in the last present (the two eye views are jittered
    // while it does, on_scene_layer_update).
    bool m_features{false};
    d3d12::SuperTAA m_plain_taa{};
    std::array<ComPtr<ID3D12Resource>, 2> m_plain_eye{};
    std::array<int64_t, 2> m_plain_frame{-10, -10};
    bool m_plain_failed{false};
    bool m_plain_logged{false};
    std::atomic<bool> m_plain_aa_now{false};
    bool evaluate_plain(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, ID3D12Resource* const color[2],
        ID3D12Resource* const depth[2], ID3D12Resource* const motion[2], bool reset_history);

    quadview::Layout m_qv_layout{};          // valid while the quad-view instances exist
    std::atomic<uint32_t> m_qv_window_w{0};  // [SMALL WINDOW fix122] the last layout's engine view size (kept over resets)
    std::atomic<uint32_t> m_qv_window_h{0};
    bool m_qv_init_requested{false};         // quad view state the current instances were created for
    uint32_t m_qv_init_generation{0};
    d3d12::FoveaComposite m_fovea_composite{};
    d3d12::TextureDump m_qv_dump{};          // debug: images of one quad-view frame
    bool m_qv_dump_pending{false};
    std::string m_qv_dump_header{};
    std::filesystem::path m_qv_dump_path{};  // folder of the frame being dumped
    d3d12::CommandContext* m_qv_dump_pre_ctx{nullptr}; // pre-fovea context that holds part of the dump this frame

    // [JITTER CHECK 10.11.2026] FSR 4 by the mod antialiases from the sub-pixel jitter of the fovea views. Started with every
    // dump: FRAMES consecutive frames of FSR 4's inputs and result (the centre of the left fovea, as stored), the jitter given
    // to FSR 4, and per view the jitter written into the engine's matrices at layer update and what they hold at layer draw.
    struct JitterCheck {
        static constexpr int FRAMES = 8;
        static constexpr uint32_t PART = 640; // side of the part written per image (px)
        std::array<d3d12::TextureDump, FRAMES> dumps{};
        int next{-1};       // frame being recorded (0..FRAMES-1); -1 = none
        bool captured{false}; // fsr_direct_record recorded into the pre-fovea list this frame
        bool write{false};  // all frames recorded: written after this frame's GPU work
        int idle_frames{0}; // frames in a row without FSR 4 by the mod while recording
        d3d12::CommandContext* pre_ctx{nullptr};
        std::mutex mtx{};
        std::string log{};
        uint64_t lines{0};
        std::unordered_map<void*, int32_t> layers{}; // scene layer -> our state index, for the draw-time line
    } m_jc{};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void jitter_check_line(std::string line);
#endif
    // the work the fovea upscaler must not wait for (TAA and reprojection of the shared wide view, interface taken out of
    // the fovea views) goes on a context of its own, submitted before the plugin runs. [ONE LIST 01.11.2026] with the mod's
    // TAA in the fovea (no plugin pass in between) it goes on the copier's list instead.
    std::array<d3d12::CommandContext, 3> m_qv_pre_copiers{};
    // one eye image per eye: the periphery upscaled to the eye size, the fovea composited into it
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_wide_safe{};
    // the finished eye images handed to the multipass submit (get_eye_image); = m_qv_wide_safe once composited
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_eye_image{};
    bool m_qv_stereo{false};                 // the current quad-view instances run (multipass, both eyes per frame)
    bool m_qv_mono{false};                   // [MONO PERIPHERY] ... with one shared wide view (three scene layers)
    // [MONO PERIPHERY] reprojection of the shared wide view to each eye, and the interface taken out of the fovea views
    d3d12::MonoPeriphery m_mono_periphery{};
    // [STABLE DEPTH fix81 17.11.2026] the wide view's distances for the reprojection, steady from frame to frame (see
    // d3d12::StableDepth); m_stable_depth_frame: the render frame of its last pass (a frame without one: no history),
    // m_stable_depth_failed: it could not run (logged once), the reprojection takes the frame's depth as before
    d3d12::StableDepth m_stable_depth{};
    int64_t m_stable_depth_frame{-10};
    bool m_stable_depth_failed{false};
    // [MENUS fix86 18.11.2026] the wide view's interface for the periphery collected over the frames (see d3d12::UiSteady);
    // m_ui_steady_frame: the render frame of its last pass (a frame without one: no history), m_ui_steady_failed: it could
    // not run (logged once), the eye passes take the frame's own images as before
    d3d12::UiSteady m_ui_steady{};
    int64_t m_ui_steady_frame{-10};

    // [UI MOTION fix125 22.11.2026] The interface's own motion for the temporal passes (vr/d3d12/UiMotion.hpp). Each
    // view's projection and view matrix without jitter, of the frame and of the frame before as the engine holds them for
    // its motion vectors, kept per frame at the view's layer update (like the jitter: the passes of a frame may run after
    // the next frame's update); the blocks of a frame (ui_motion_blocks: [0] the wide view, [1] / [2] the fovea views)
    // and the last report for the log.
    struct UiViewMatrices {
        int64_t frame{-1};
        float proj[16]{};
        float view[16]{};
        float old_proj[16]{};
        float old_view[16]{};
    };

    UiViewMatrices m_ui_views_by_frame[3][8]{};
    bool ui_view_matrices(size_t view, int64_t frame, UiViewMatrices& out) const;
    std::array<d3d12::UiMotion::Block, 3> ui_motion_blocks(ID3D12Device* device, int32_t frame, bool want_report);
    d3d12::UiMotion m_ui_motion{};
    std::string m_ui_motion_report{};
    bool m_ui_steady_failed{false};
    bool m_stable_depth_used{false}; // the last frame's reprojection got it (for the dump's header)
    d3d12::UiClean m_ui_clean{};
    d3d12::UiCurve m_ui_curve{}; // [UI CURVE 27.10.2026] the engine's output curve, measured on the shared wide view
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_mono_eye_wide{};   // per eye: its periphery at the render density (R16G16B16A16_FLOAT)
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fovea_clean{};     // per eye: the fovea view without interface (engine colour format)
    std::array<bool, 2> m_qv_fovea_clean_psr{false, false};       // ... currently resting in PIXEL_SHADER_RESOURCE (else UNORDERED_ACCESS)
    // [FOVEA LINEAR 05.11.2026] the upscaler plugin in the fovea gets linear light (see d3d12::FoveaLinear): per eye its input
    // (R16G16B16A16_FLOAT, resting in PIXEL_SHADER_RESOURCE between frames like an engine copy once written)
    d3d12::FoveaLinear m_fovea_linear{};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fovea_lin{};
    std::array<bool, 2> m_qv_fovea_lin_psr{false, false};
    bool m_qv_fovea_float_out{false}; // the plugin's fovea results are R16G16B16A16_FLOAT (linear light needs it: no banding)
    bool m_fovea_linear_off{false};   // a pass failed: the plugin gets the encoded image again (as before 05.11.2026)
    // [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod (d3d12::FsrDirect, quadview::Settings::fsr_direct): one
    // context for both foveas side by side (m_qv_fsr_atlas valid) or one per eye. Its inputs side by side (colour in linear
    // light, depth, motion; written by FoveaLinear, then resting in the read state) and its results (R16G16B16A16_FLOAT,
    // resting in UNORDERED_ACCESS: [0] both foveas or the left one, [1] the right one).
    std::array<d3d12::FsrDirect, 2> m_fsr{};
    bool m_qv_fsr{false};                 // this set-up uses it (else the plugin)
    // [FOVEA OWN fix91 19.11.2026] this set-up has no upscaler for the fovea (the mod's anti-aliasing only; always with the
    // shared periphery on D3D12): FSR 4 by the mod is no longer created. What its set-up gave the fovea views' jitter when
    // the mod's TAA runs without its supersampling is kept here: the width the sequence length follows, and whether the
    // index is the engine's frame (both foveas would have fitted in one pass).
    bool m_qv_fovea_own{false};
    bool m_qv_fovea_frame_jitter{false};
    uint32_t m_qv_fovea_jitter_w{0};
    quadview::FsrAtlas m_qv_fsr_atlas{};  // valid: both foveas in one pass
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fsr_out{};
    ComPtr<ID3D12Resource> m_qv_fsr_color{};
    ComPtr<ID3D12Resource> m_qv_fsr_depth{};
    ComPtr<ID3D12Resource> m_qv_fsr_motion{};
    // [FSR DIRECT 06.11.2026 evening] a pass per eye: each eye's depth and motion copied the same way (R32_FLOAT,
    // R32G32_FLOAT), so the two ways differ only in the side-by-side image
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fsr_depth_eye{};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fsr_motion_eye{};
    bool m_qv_fsr_in_read{false};         // the inputs side by side rest in the read state (else UNORDERED_ACCESS)
    // [FSR MODEL 09.11.2026] another FSR 4 network than the layout's scale (quadview::Settings::fsr_model): FSR 4 writes into
    // these larger images (as m_qv_fsr_out: [0] both foveas or the left one, [1] the right one; UNORDERED_ACCESS), scaled
    // down into m_qv_fsr_out after it. Factor 1 = none (FSR 4 writes m_qv_fsr_out itself).
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fsr_big{};
    float m_qv_fsr_factor{1.0f};
    uint32_t m_qv_fsr_big_w{0};           // size of each larger image
    uint32_t m_qv_fsr_big_h{0};
    uint32_t m_qv_fsr_jitter_w{0};        // one fovea's width in FSR 4's output (the jitter sequence's length follows it)
    bool m_fsr_inputs_small_logged{false}; // [FSR DIRECT 09.11.2026] inputs smaller than the layout (logged once per run)
    bool m_fsr_direct_failed{false};      // it failed: the plugin from the next set-up on, until the option is switched off and on
    std::chrono::steady_clock::time_point m_fsr_last{};
    // records FSR 4 for this frame on the pre-fovea list (after its PRE_END stamp): the inputs side by side, the pass(es)
    bool fsr_direct_record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, int32_t frame, const std::array<ID3D12Resource*, 2>& color,
        const std::array<ID3D12Resource*, 2>& depth, const std::array<ID3D12Resource*, 2>& motion, float fov_y, bool dump);
    // sets up the context(s) and results for `layout` (false: nothing set up, the plugin is used)
    bool fsr_direct_create(ID3D12Device* device, const quadview::Layout& layout, const quadview::Settings& settings);
    void fsr_direct_release();
    // [MONO UI 27.10.2026] the wide view without its interface (input of its TAA and the reprojection), the eye images with
    // their interface put together, and the pass that does it
    ComPtr<ID3D12Resource> m_qv_wide_clean{};
    // [CAPTURE fix82 17.11.2026] the place of a capture's chain (a point of the left eye's image beside the fovea rectangle),
    // kept from the capture's first frame for all its frames
    float m_sc_side[2]{0.0f, 0.0f};
    bool m_qv_wide_clean_psr{false};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_ui_out{};
    d3d12::MonoUi m_mono_ui{};
    // [GPU 29.10.2026] the shared periphery's eye images in one pass each (instead of WideUpscale + FoveaComposite + MonoUi),
    // and where the wide view has GUI (so the pass skips the interface work elsewhere)
    d3d12::EyeCompose m_eye_compose{};
    d3d12::GuiMap m_gui_map{};
    bool m_ui_copies_refreshed{false}; // [UI COPIES 30.10.2026] the wide view's copies were cloned again for the current run
    // [ANCHOR HOLD 03.11.2026] the interface elements that take pixels to their own depth (item icon, item beams) by game
    // object, with the upscaler frame they were last listed in: one missing from a frame's lists keeps its place for a
    // few frames (world position, projected with the frame's own matrix)
    struct HeldAnchor {
        quadview::GuiAnchor a{};
        uint64_t seen{0};
    };
    std::unordered_map<uint64_t, HeldAnchor> m_anchor_hold{};
    uint64_t m_anchor_frame{0};
    // per window of the log (450 frames): frames with an item icon listed or held, only held, how often it came and went
    // in the lists; the same for the beams; frames with more anchors than the pass takes
    struct AnchorStats {
        uint32_t frames{0};
        uint32_t icon_frames{0};
        uint32_t icon_held{0};
        uint32_t icon_toggles{0};
        uint32_t beam_frames{0};
        uint32_t beam_held{0};
        uint32_t beams_max{0};
        uint32_t dropped{0};
        bool icon_last{false};
    };
    AnchorStats m_anchor_stats{};
    std::string m_anchor_window{}; // the last complete window, for the log
    bool m_eye_compose_logged{false};
    // [GPU 29.10.2026] the image the desktop window gets after both eyes (the left eye), null = none this frame
    ID3D12Resource* m_qv_mirror_src{nullptr};
    std::string m_ui_report{}; // last frame: anchors, panel depth, which interface copies were there (log)
    std::string m_layer_report{};            // scene layers seen at EndRendering (log)
    uint64_t m_layer_signature{0};           // count + view ids of those layers, to re-format the report only on change
    // eye-sized image of our own (UAV-able variant of `format`); recreated when size/format change
    bool ensure_qv_wide_safe(ID3D12Device* device, uint32_t slot, uint32_t w, uint32_t h, DXGI_FORMAT format);
    d3d12::WideUpscale m_wide_upscale{};     // [PERIPHERY] spatial upscale of the periphery into m_qv_wide_safe
    // [PERIPHERY] light TAA: per eye slot two render-size histories (read one, write the other)
    d3d12::LightTAA m_light_taa{};
    std::array<std::array<ComPtr<ID3D12Resource>, 2>, 2> m_qv_taa_hist{};
    std::array<uint32_t, 2> m_qv_taa_cur{0, 0};
    std::array<bool, 2> m_qv_taa_valid{false, false};
    // [CALM fix100 20.11.2026] the calm light TAA of the shared wide view: its second history (swapped with the history,
    // m_qv_taa_cur[0]), the last frame it ran, and whether the last frame's periphery was made by it
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_calm_aux{};
    int64_t m_qv_calm_frame{-10};
    std::atomic<bool> m_periphery_calm{false};
    bool m_qv_calm_failed{false};
    bool ensure_qv_taa(ID3D12Device* device, uint32_t w, uint32_t h);
    // [FOVEA TAA 28.10.2026] the mod's TAA in the fovea instead of the plugin (own per-eye histories) and the fovea-sized
    // image it writes for the composite
    d3d12::FoveaTAA m_fovea_taa{};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fovea_safe{}; // per eye slot
    // [LONG EDGES 11.11.2026] the fovea image after LongEdgePass (per eye slot) and the pass
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fovea_aa{};
    d3d12::LongEdgePass m_long_edge{};
    bool m_qv_fovea_taa_last{false};   // the last frame used the TAA (a switch starts the histories again)
    // [SUPER TAA 16.11.2026] the mod's supersampling TAA in the fovea (quadview::Settings::fovea_super): its own histories;
    // the frame each eye last got its pass in (a gap starts its history again); the mod's TAA's image beside it when the
    // menu asks for the comparison (split view, capture). m_super_taa_failed: a pass failed, the mod's TAA until the next
    // set-up.
    d3d12::SuperTAA m_super_taa{};
    bool m_super_taa_failed{false};
    std::atomic<uint32_t> m_super_scale_now{0}; // 0 = not the fovea's anti-aliasing, else its history's scale (1, 2)
    std::array<int64_t, 2> m_qv_staa_frame{-10, -10};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_fovea_cmp{};
    // [PERIPHERY SUPER fix80 17.11.2026] the mod's supersampling TAA on the shared wide view instead of the light TAA
    // (quadview::Settings::periphery_aa): an instance of its own with one view's histories. Its image at the wide view's
    // size [0] or at twice that size [1], and for the latter each eye's periphery with 2 x 2 pixels per pixel of the wide
    // view (m_qv_mono_eye_wide2): each made on its first use and kept until the quad view is released, so a change in the
    // menu never frees an image a list in flight still uses. m_qv_wide_staa_frame / _ss: the frame it last ran in and with
    // which image (a gap or a change starts its history again). m_super_wide_failed: a pass failed, the light TAA until
    // the next set-up. m_periphery_ss / m_periphery_linear: what the last periphery_taa returned (texels per pixel of the
    // wide view and axis; linear values, to be encoded after the enlargement). m_super_wide_now: for the next frame's
    // jitter (on_scene_layer_update), like m_super_scale_now.
    d3d12::SuperTAA m_super_taa_wide{};
    bool m_super_wide_failed{false};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_wide_super{};
    std::array<ComPtr<ID3D12Resource>, 2> m_qv_mono_eye_wide2{};
    int64_t m_qv_wide_staa_frame{-10};
    uint32_t m_qv_wide_staa_ss{0};
    uint32_t m_periphery_ss{1};
    bool m_periphery_linear{false};
    std::atomic<uint32_t> m_super_wide_now{0};
    std::atomic<uint32_t> m_periphery_running{0}; // what the last frame's periphery got: 0 light TAA, 1 / 2 as periphery_aa
    std::atomic<uint32_t> m_periphery_why{0};     // ... and why the light TAA when the other was asked for (PeripheryWhy)
    uint32_t m_periphery_logged{~0u};
    // the supersampling TAA's pass on the shared wide view: its image, or nullptr (the light TAA runs then)
    ID3D12Resource* periphery_super_taa(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion,
        ID3D12Resource* depth, const quadview::Settings& settings, float jitter_x, float jitter_y);
    // [SUPER TAA 16.11.2026] Capture for the replay outside the game (FoveatedRendering: "Capture in 4 s"): consecutive
    // frames of the left fovea's pass exactly as stored, see d3d12::SuperTAACapture. Written to
    // quadview_dump/supertaa_captureNN.
    d3d12::SuperTAACapture m_sc{};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void super_capture_write();
#endif
    // [ENGINE PROBE 16.11.2026] Debug ("Write the engine's buffers"): the reflected fields of the left fovea view's scene
    // layer listed (name, type; for the target states their render targets with size and format), and every render
    // target of those target states copied by the engine at two points of the next frame (before the post effects: the
    // G-buffer, the image before tone mapping; after the overlay: what the mod takes its inputs from) and written once
    // (BMP, and the middle part as stored). What the engine has besides colour, depth and motion (normals, roughness,
    // the image in linear light) and in which format is read from the game instead of guessed.
    struct EngineProbe {
        struct Copy {
            std::string target{};  // the target state's field name
            int rtv{0};
            int stage{0};          // 0 = before the post effects, 1 = after the overlay
            sdk::intrusive_ptr<sdk::renderer::Texture> copy{};
            bool copied{false};    // the engine's list of a frame holds its copy
        };
        std::atomic<int> state{0}; // 0 idle, 1 copies made (the engine copies in its next frame), 2 read back in this present
        int wait_frames{0};
        std::atomic<void*> layer{nullptr}; // the scene layer probed
        std::vector<Copy> copies{};
        std::string report{};
        std::mutex mtx{};          // copies (the engine's draw thread marks them)
        d3d12::TextureDump dump{};
        bool written_pending{false};
    } m_probe{};
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void engine_probe_start();
    void engine_probe_copy(sdk::renderer::RenderLayer* scene_layer, void* render_context, int stage);
    void engine_probe_record(ID3D12Device* device, d3d12::CommandContext& copier);
    void engine_probe_write();
#endif
    std::array<bool, 2> m_qv_plugin_reset{false, false}; // [GPU GUARD 05.11.2026] the plugin's next evaluation per eye starts again
    // [MENU HANG 14.11.2026] multipass without the quad view: which input was missing last (0 = none), the frames skipped since,
    // and a history reset for the first complete frame after them
    int m_mp_missing{0};
    uint64_t m_mp_skipped{0};
    bool m_mp_reset_next{false};
    // the frame being evaluated and the last frame each history got its pass in (a gap starts the history again)
    int64_t m_qv_frame{0};
    std::array<int64_t, 2> m_qv_ftaa_frame{-10, -10};
    std::array<int64_t, 2> m_qv_taa_frame{-10, -10};
    // [POST IN UI] the eye image was written in this frame (an image submitted again unchanged already has its post pass)
    std::array<bool, 2> m_qv_eye_fresh{false, false};
    bool ensure_taa_history(ID3D12Device* device, std::array<std::array<ComPtr<ID3D12Resource>, 2>, 2>& hist, std::array<bool, 2>& valid,
        uint32_t w, uint32_t h, const wchar_t* name);
    // created: set to true when a new image was made (it then rests in UNORDERED_ACCESS)
    bool ensure_uav_image(ID3D12Device* device, ComPtr<ID3D12Resource>& tex, uint32_t w, uint32_t h, DXGI_FORMAT format, const wchar_t* name,
        bool* created = nullptr);
    std::string m_qv_status{"off"};
    mutable std::mutex m_qv_status_mutex{};
    void set_qv_status(std::string text) {
        std::scoped_lock _{m_qv_status_mutex};
        m_qv_status = std::move(text);
    }

    sdk::renderer::layer::Scene* m_cloned_scene_layer{nullptr};
    sdk::renderer::layer::Output* m_output_layer{nullptr};
    sdk::renderer::layer::Output* m_original_output_layer{nullptr};
    sdk::renderer::layer::Output* m_cloned_output_layer{nullptr};
    sdk::renderer::layer::Output* m_last_output_layer{nullptr};
    sdk::renderer::TargetState* m_last_output_state{nullptr};
    sdk::renderer::ConstantBuffer* m_original_scene_info_buffer{};
    sdk::renderer::ConstantBuffer* m_cloned_scene_info_buffer{};

    sdk::renderer::TargetState* m_new_target_state{nullptr};


    struct EyeState {
        sdk::intrusive_ptr<sdk::renderer::layer::Scene> scene_layer{};
        ComPtr<ID3D12Resource> motion_vectors{};
        ComPtr<ID3D12Resource> depth{};
        ComPtr<ID3D12Resource> color{};

        sdk::intrusive_ptr<sdk::renderer::Texture> color_copy{};
        sdk::intrusive_ptr<sdk::renderer::Texture> motion_vectors_copy{};
        sdk::intrusive_ptr<sdk::renderer::Texture> depth_copy{};
    };

    // [STEREO FOVEA] 4 states: multipass [0] left [1] right; quad view with the shared periphery [0] wide view [1] fovea
    // left [2] fovea right; with a wide view per eye [0] wide left [1] wide right [2] fovea left [3] fovea right (the
    // order of CameraDuplicator's scene layers)
    std::array<EyeState, 4> m_eye_states{};
    // number of states in use this frame (1, 2 or 4), decided at EndRendering
    uint32_t m_layers_in_use{1};

    // [COPY LIFETIME 15.11.2026] copies replaced or dropped (the game's targets changed), kept for 8 frames: the game's
    // command lists of the last frames may still copy into them (RE7 15.11.2026 16:03: device removed, a page fault in a
    // CopyResource of the game's list on a just freed allocation, 4 ms after a copy was made again)
    std::mutex m_retired_copies_mtx{};
    std::vector<std::pair<sdk::intrusive_ptr<sdk::renderer::Texture>, uint64_t>> m_retired_copies{};
    void retire_copy(sdk::intrusive_ptr<sdk::renderer::Texture>& copy);
    void release_retired_copies(bool all);
    // [COPY SIZE 15.11.2026] a copy only between textures of the same size (see on_overlay_layer_draw)
    bool copy_fits(sdk::renderer::Texture* dst, sdk::renderer::Texture* src);
    int m_copy_size_skips_logged{0};

    // 3 giant textures to encapsulate the motion vectors, depth, and color buffers
    // because the upscaler needs them all in one texture
    // well... it doesn't necessarily need them
    // but it causes some insane lag if using multiple features to evaluate multiple textures
    // so this is the best solution for now
    ComPtr<ID3D12Resource> m_big_motion_vectors{};
    ComPtr<ID3D12Resource> m_big_depth{};
    ComPtr<ID3D12Resource> m_big_color{};

    ComPtr<ID3D12Resource> m_blank_big_motion_vectors{};
    ComPtr<ID3D12Resource> m_blank_big_depth{};
    ComPtr<ID3D12Resource> m_blank_big_color{};

    int32_t m_displayed_scene{0}; // 0 = original, 1 = cloned

    float m_nearz{0.0f};
    float m_farz{0.0f};
    float m_fov{90.0f};

    float m_jitter_offsets[4][2]{};   // [QUADVIEW] was [2][2]

    // [JITTER LATCH 10.11.2026] The jitter each view was rendered with, per frame. The views of the next frame can be
    // updated (m_jitter_offsets overwritten) before this frame's upscaling runs: jitter check of 10.11.2026, at 15 ms frames
    // FSR 4 got the next frame's jitter in every frame (0.3..0.8 px off; measured from the depth of consecutive frames).
    struct FrameJitter {
        int64_t frame{-1};
        float x{0.0f};
        float y{0.0f};
    };
    static constexpr size_t JITTER_KEEP = 8;
    FrameJitter m_jitter_by_frame[4][JITTER_KEEP]{};
    mutable std::mutex m_jitter_frame_mtx{};
    // the jitter view `index` had in frame `frame` (render frame number); not kept: the latest. `newer`: the latest is another one
    std::array<float, 2> jitter_of_frame(size_t index, int64_t frame, bool* newer = nullptr) const;
    float m_jitter_scale[2]{2.0f, -2.0f};
    float m_motion_scale[2]{-1.0f, 1.0f};
    float m_jitter_evaluate_scale{1.0f};

    std::array<d3d12::CommandContext, 3> m_copiers{};
    ComPtr<ID3D12Resource> m_old_backbuffer{};

    // [QUADVIEW] 4 slots, see m_jitter_indices
    std::array<std::array<Matrix4x4f, 6>, 4> m_old_projection_matrix{};
    std::array<std::array<Matrix4x4f, 6>, 4> m_old_view_matrix{};
    std::array<std::array<Matrix4x4f, 6>, 4> m_old_view_projection_matrix{};

    // [UPSCALER_DEFAULT_AUS 2026-08-19] War true. Der Upscaler soll bei einer frischen
    // Installation ausgeschaltet starten; eine bereits gespeicherte re2_fw_config.txt
    // sticht diesen Default weiterhin.
    const ModToggle::Ptr m_enabled{
        ModToggle::create(generate_name("Enabled"), false)
    };

    const ModToggle::Ptr m_sharpness{
        ModToggle::create(generate_name("SharpnessEnable"), true)
    };

    // [SKALA 1-10 / CAP 1.0 -- 15.09.2026, Ansage des Users] Der Regler geht in
    // GANZEN Schritten 0,1,2 ... 10; an den Upscaler geht der Zehntelwert, 10
    // im Menue ist also 1.0 und damit die Obergrenze. Der alte Bereich ging bis
    // 5.0 -- Artefakte lagen dort lange vor dem Anschlag.
    const ModInt32::Ptr m_sharpness_amount{
        ModInt32::create(generate_name("SharpnessAmount_V4"), 5)
    };

    // Der Wert, der wirklich hinausgeht: 0.0 .. 1.0
    float sharpness_value() const {
        const int32_t v = std::clamp(m_sharpness_amount->value(), 0, 10);

        return (float)v * 0.1f;
    }


    // ([NO PLUGIN fix91 19.11.2026] the plugin's options were here: UpscaleType_V2, UpscaleQuality_V2, DLSSPreset)

    // [POST_PROCESSING 28.09.2026] Default AN = Spielzustand. AN schreibt nur
    // EINMAL zurueck (nach dem Umschalten), AUS schreibt jeden Frame.
    const ModToggle::Ptr m_fog_enabled{ModToggle::create(generate_name("FogEnabled"), true)};
    const ModToggle::Ptr m_volfog_enabled{ModToggle::create(generate_name("VolumetricFogEnabled"), true)};
    bool m_fog_restore{false};
    bool m_volfog_restore{false};
    std::atomic<bool> m_test_fog_off{false}; // [LIGHT FLICKER TEST 15.11.2026 evening]
    bool m_test_fog_was_off{false};
    // [POST_PROCESSING 28.09.2026] LDRPostProcess -> get_ColorCorrect -> set_Enabled.
    const ModToggle::Ptr m_colorcorrect_enabled{ModToggle::create(generate_name("ColorCorrectEnabled"), true)};
    bool m_colorcorrect_restore{false};

    // [CPU 22.10.2026] The scene-wide search costs 2.5-3 ms; running it every frame cost ~1.5-1.9 ms per frame. The game
    // switches these components back on all the time (log 22.10.2026, second test), so the wanted state is still
    // written every frame -- on the components found by the last search. Each keeps one reference of ours and is
    // dropped as soon as its game object is gone. The search runs when the option changes, when the scene or a
    // camera changes and once a minute, at most one type per frame.
    struct PostFxScan {
        std::chrono::steady_clock::time_point last{};
        bool dirty{true};
        bool active{false};                         // option off: `objects` are kept off every frame
        std::vector<::REManagedObject*> objects{};  // one reference of ours on each
        std::string owners{};                       // their game objects (log)
        sdk::RETypeDefinition* td{nullptr};         // method cache for the type set_Enabled is called on
        sdk::REMethodDefinition* get_enabled{nullptr};
        sdk::REMethodDefinition* set_enabled{nullptr};
    };
    std::array<PostFxScan, 3> m_pp_scan{};
    void* m_pp_scene{nullptr};
    std::array<void*, 4> m_pp_cameras{};
    size_t m_pp_camera_slot{0};
    uint64_t m_pp_searches{0};
    uint64_t m_pp_kept_off{0};       // times a component had been switched back on and was switched off again
    uint64_t m_pp_new_objects{0};
    uint64_t m_pp_dropped_objects{0};
    double m_pp_last_search_us{0.0};
    double m_pp_max_search_us{0.0};
    mutable std::mutex m_pp_stats_mtx{};   // owners text (read by the log)

    void post_fx_search(size_t index, const char* type_name, const char* sub_getter, const char* property, bool enabled);
    bool post_fx_apply(PostFxScan& st, ::REManagedObject* component, const char* sub_getter, const char* property, bool enabled);
    void post_fx_release(PostFxScan& st);

public:
    std::string get_post_fx_stats() const {
        std::scoped_lock _{m_pp_stats_mtx};
        std::string o = std::format("searches={} last={:.0f} us max={:.0f} us | kept off after the game switched them on={} | objects found={} gone={}",
            m_pp_searches, m_pp_last_search_us, m_pp_max_search_us, m_pp_kept_off, m_pp_new_objects, m_pp_dropped_objects);

        for (const auto& st : m_pp_scan) {
            if (!st.owners.empty()) {
                o += " | " + st.owners;
            }
        }

        return o;
    }

private:

    // [POST_PASS 28.09.2026] Eigene Schaerfe + Saettigung auf den Augenbildern
    // (vr/D3D12Component::apply_post_pass, nur ohne Upscaler). Menue ganze Stufen:
    // Schaerfe 0..10 (0 = aus) -> 0.0..3.0, Saettigung -10..10 (0 = neutral) -> 0.0..2.0.
    const ModInt32::Ptr m_post_sharpness{ModInt32::create(generate_name("PostSharpness"), 0)};
    // [0 = MITTE 28.09.2026 -- Ansage des Users] Saettigung -10..10, 0 = neutral.
    // Neuer Config-Schluessel (_V2), sonst waere eine gespeicherte alte 10 jetzt "doppelt".
    const ModInt32::Ptr m_post_saturation{ModInt32::create(generate_name("PostSaturation_V2"), 0)};
    // [POST_PASS SMAA 28.09.2026] SMAA auf den Augenbildern (ohne Upscaler), Default aus.
    const ModToggle::Ptr m_post_smaa{ModToggle::create(generate_name("PostSMAA"), false)};
    // [BRIGHTNESS/CONTRAST 28.09.2026 -- Ansage des Users] -10..10, 0 = neutral.
    const ModInt32::Ptr m_post_brightness{ModInt32::create(generate_name("PostBrightness"), 0)};
    const ModInt32::Ptr m_post_contrast{ModInt32::create(generate_name("PostContrast"), 0)};
    // [EDGE SOFTEN 11.11.2026] softening of high-contrast edges only (straight edges, metal structures), against the stair
    // steps and flicker the headset shows on them; strength 0..10 (0 = off), threshold 1..10 (the edge contrast from which
    // it starts)
    const ModInt32::Ptr m_post_edge_soften{ModInt32::create(generate_name("PostEdgeSoften"), 0)};
    const ModInt32::Ptr m_post_edge_threshold{ModInt32::create(generate_name("PostEdgeThreshold"), 5)};
    // [LONG EDGES 11.11.2026] anti-aliasing along the edges of the upscaled fovea (d3d12::LongEdgePass), 0..20 (0 = off)
    const ModInt32::Ptr m_post_long_edges{ModInt32::create(generate_name("PostLongEdges"), 0)};

public:
    float post_sharpness() const { return (float)std::clamp(m_post_sharpness->value(), 0, 10) * 0.3f; }
    // [EDGE SOFTEN 11.11.2026] weight of the 4 neighbours' mean on an edge (0.06 per step up to 10 = 0.6, then 0.04 per
    // step up to 20 = 1.0: the neighbours' mean alone) and the luma range of the pixel and its 4 neighbours from which an
    // edge counts (0.03 per step, 1..20; full weight 0.1 above it)
    float post_edge_soften() const {
        const int v = std::clamp(m_post_edge_soften->value(), 0, 20);
        return v <= 10 ? (float)v * 0.06f : 0.6f + (float)(v - 10) * 0.04f;
    }
    float post_edge_threshold() const { return (float)std::clamp(m_post_edge_threshold->value(), 1, 20) * 0.03f; }
    // [LONG EDGES 11.11.2026] share of the long-edge result, 0.05 per step (20 = all of it)
    float post_long_edges() const { return (float)std::clamp(m_post_long_edges->value(), 0, 20) * 0.05f; }
    float post_saturation() const { return (float)(std::clamp(m_post_saturation->value(), -10, 10) + 10) * 0.1f; }
    bool post_smaa() const { return m_post_smaa->value(); }
    // [UI_FARBE 28.09.2026] Farbkopie NACH PrepareOutput (Upscaler-Eingang, mit UI,
    // richtiger Farbraum) fuer diese Scene-Layer; nullptr, wenn keine.
    ID3D12Resource* get_color_copy_for(void* scene_layer) const {
        for (const auto& st : m_eye_states) {
            if ((void*)st.scene_layer.get() == scene_layer && st.color_copy != nullptr) {
                auto c = st.color_copy->get_d3d12_resource_container();
                return c != nullptr ? c->get_native_resource() : nullptr;
            }
        }
        return nullptr;
    }
    // [UI_TRENNUNG 28.09.2026] irgendeine Nachbearbeitung an? (dann UI-Kopien noetig)
    bool post_any_active() const {
        return m_post_smaa->value() || m_post_sharpness->value() != 0 || m_post_edge_soften->value() != 0 || m_post_saturation->value() != 0
            || m_post_brightness->value() != 0 || m_post_contrast->value() != 0;
    }
    // [BRIGHTNESS/CONTRAST 28.09.2026] Menue -10..10, 0 = neutral.
    // Helligkeit als Gamma-Exponent: +10 -> 0.5 (heller), -10 -> 2.0 (dunkler).
    float post_brightness() const { return std::exp2(-(float)std::clamp(m_post_brightness->value(), -10, 10) * 0.1f); }
    // Kontrast: +10 -> 1.5, -10 -> 0.5.
    float post_contrast() const { return 1.0f + (float)std::clamp(m_post_contrast->value(), -10, 10) * 0.05f; }
    // [POST IN UI 28.10.2026] this eye image already holds the post pass (written by the quad view interface pass). Tied to
    // the image, not to a frame number: on a frame where the quad view stops early the image stays as it was (with the
    // post pass in it) and is submitted again, and the frame counter is written by another thread.
    bool qv_post_applied(int eye, const ID3D12Resource* tex) const {
        return eye >= 0 && eye < 2 && tex != nullptr && m_qv_post_tex[eye] == tex;
    }

    // The submit may run the post pass in place on `tex` (once per written image): false for a quad-view eye image that
    // was not written again since (a frame the quad view skipped submits the last one again, which has its post pass
    // already). Other images (the plugin's outputs without the quad view) are written every frame: always true.
    // Called once per eye and frame by the submit, on the present thread like the evaluation.
    bool qv_take_fresh(int eye, const ID3D12Resource* tex) {
        if (eye < 0 || eye > 1 || !m_qv_layout.valid || tex == nullptr || tex != m_qv_eye_image[eye].Get()) {
            return true;
        }

        const bool fresh = m_qv_eye_fresh[eye];
        m_qv_eye_fresh[eye] = false;
        return fresh;
    }

private:
    std::array<const ID3D12Resource*, 2> m_qv_post_tex{};


     ValueList m_options{
        *m_enabled,
        *m_sharpness,
        *m_sharpness_amount,
        *m_fog_enabled,
        *m_volfog_enabled,
        *m_colorcorrect_enabled,
        *m_post_sharpness,
        *m_post_saturation,
        *m_post_smaa,
        *m_post_brightness,
        *m_post_contrast,
        *m_post_edge_soften,
        *m_post_edge_threshold,
        *m_post_long_edges
     };
};