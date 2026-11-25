#pragma once

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Mod.hpp"
#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE: the class must look the same in every file

namespace sdk {
namespace renderer {
namespace layer {
class Scene;
}
}
}

namespace vrmod {
// Clones of the main camera (multipass only). Multipass renders the second eye with one clone; the quad view
// ([STEREO FOVEA 27.10.2026]) needs two with the shared periphery (the fovea of each eye) or three with a wide view per
// eye (the wide view of the other eye and the fovea of each eye). VR says every LockScene how many clones it wants
// (VR::wanted_camera_clones); the clones are created on demand and switched off (not destroyed) when fewer are wanted.
class CameraDuplicator : public Mod::Component {
public:
    static constexpr size_t MAX_CLONES = 3;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;
    void on_draw_ui() override;

    // first clone of `camera` (the multipass one), nullptr if none
    RECamera* get_new_camera_counterpart(RECamera* camera) {
        return get_clone(camera, 0);
    }

    // clone `index` (0-based) of `camera`, nullptr if it does not exist (yet)
    RECamera* get_clone(RECamera* camera, size_t index) {
        std::scoped_lock _{ m_camera_mutex };

        if (auto it = m_old_to_new_cameras.find(camera); it != m_old_to_new_cameras.end()) {
            if (index < it->second.size()) {
                return it->second[index];
            }
        }

        return nullptr;
    }

    size_t get_clone_count(RECamera* camera) {
        std::scoped_lock _{ m_camera_mutex };

        if (auto it = m_old_to_new_cameras.find(camera); it != m_old_to_new_cameras.end()) {
            return it->second.size();
        }

        return 0;
    }

    // index of `camera` among the clones of its original (0 = first clone), -1 if it is not a clone
    int32_t get_clone_index(RECamera* camera) {
        std::scoped_lock _{ m_camera_mutex };

        auto it = m_new_to_old_camera.find(camera);

        if (it == m_new_to_old_camera.end()) {
            return -1;
        }

        auto old = m_old_to_new_cameras.find(it->second);

        if (old == m_old_to_new_cameras.end()) {
            return -1;
        }

        for (size_t i = 0; i < old->second.size(); ++i) {
            if (old->second[i] == camera) {
                return (int32_t)i;
            }
        }

        return -1;
    }

    RECamera* get_old_camera_counterpart(RECamera* camera) {
        std::scoped_lock _{ m_camera_mutex };

        if (auto it = m_new_to_old_camera.find(camera); it != m_new_to_old_camera.end()) {
            return it->second;
        }

        return nullptr;
    }

    // Fully rendered scene layers, [0] = primary camera, [1] = clone 0, [2] = clone 1, ... (others, e.g. the sniper
    // scope camera, after them)
    std::vector<sdk::renderer::layer::Scene*> get_relevant_scene_layers();

    // [STEREO FOVEA] via.render.RenderOutput ids given to clone 0, 1, 2 (clone 0 has always had 3). Applied to the
    // existing clones as well when it changes (whether the engine honours a change at runtime is part of the test).
    void set_clone_output_ids(const std::array<int32_t, MAX_CLONES>& ids) {
        std::scoped_lock _{ m_camera_mutex };
        m_wanted_output_ids = ids;
    }

    std::array<int32_t, MAX_CLONES> get_clone_output_ids() {
        std::scoped_lock _{ m_camera_mutex };
        return m_wanted_output_ids;
    }

    // number of clones created for the current primary camera so far (diagnostics)
    size_t get_active_clone_count();

    // [EXPOSURE TEST 01.11.2026] (debug, not saved) the tone mapping of the views, to see where the fovea's brightness
    // differs from the wide view's: local_exposure 0 = as the game, 1 = off in the fovea views (the clones), 2 = off in all
    // views; copy_auto: the clones take the game camera's automatic exposure (getAutoExposure / setAutoExposure is not a
    // get_ / set_ property, so it is not copied otherwise); auto_value >= 0: that value of it in all views
    struct ToneTest {
        int local_exposure{0};
        bool copy_auto{false};
        int auto_value{-1};
        // [EV LOCK 13.11.2026] (RE7: UseAutoExposure has no setter) the automatic exposure's range (AutoExposureMinEV /
        // MaxEV) closed on the game camera's SnappedRealEV: 0 off, 1 in the fovea views, 2 in all views
        int ev_lock{0};
        // [EV FOLLOW 13.11.2026] the fovea views' EV: the game camera's + a fixed offset, and / or + an offset that follows the
        // measured periphery / fovea brightness (quadview::State::view_gain) until the two match
        float ev_offset{0.0f};
        bool ev_follow{false};
        // [ADAPT 13.11.2026] the fovea views' own adaptation (Bright / DarkAdaptationRate): 0 as the game, 1 both rates 0,
        // 2 both rates 1000 (whether 0 freezes it or makes it instant is not known)
        int clone_adapt{0};
        // [REAL EV 13.11.2026] the fovea views get the game camera's adapted exposure: calcRealEV() every frame on the game
        // camera, its SnappedRealEV (the EV after its automatic exposure) as the fovea views' EV. 0 off, 1 their own adaptation
        // stopped (rates 0), 2 their automatic exposure Disable, 3 FixedEnable. Their own remaining offset (their
        // SnappedRealEV - EV after calcRealEV on them) is taken off.
        int real_ev{0};
        // [REAL EV fix105 20.11.2026] the game camera's calcRealEV (0.7 ms of the main thread) every 8th frame while its value
        // stays the same, every 4th while it moves; false: every 4th always, as before fix105
        bool real_ev_steady{true};
    };

    void set_tone_test(const ToneTest& t) {
        std::scoped_lock _{ m_camera_mutex };
        m_tone_test = t;
    }

    // [COLOUR CORRECTION 02.11.2026] (test, not saved) the clones' colour correction (LDRPostProcess.ColorCorrect, an object
    // of its own in every camera: only its Enabled was copied) gets every property of the game camera's, every frame
    void set_copy_color_correct(bool on) {
        std::scoped_lock _{ m_camera_mutex };
        m_copy_color_correct = on;
        m_color_copy_logged = false;
    }

private:
    void apply_tone_test(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted);
    // [VIEW SCALE 15.11.2026] the cameras' via.render.ViewScaling at 1 while the fovea views are drawn (see quadview::State)
    void apply_view_scale(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted);
    bool m_view_scale_saved{false};
    float m_view_scale_original{1.0f};
    int m_view_scale_changes_logged{0};
    // [SCREEN EFFECTS 15.11.2026] the components switched off by apply_screen_effects (put back when no longer wanted)
    void apply_screen_effects(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted);
    std::vector<REComponent*> m_screen_effects_off{};
    int m_screen_effects_logged{0};
    // [IMAGE QUALITY 15.11.2026] the game camera's RenderOutput ImageQuality set to the clones' value while the fovea views
    // are drawn (see apply_image_quality)
    void apply_image_quality(void* ctx, REGameObject* main_gameobject, const std::vector<RECamera*>& clones, size_t wanted);
    int m_image_quality_logged{0};
    double m_image_quality_last_logged{0.0};
    int m_image_quality_window_calls{0}; // calls in the current window of 600
    int m_image_quality_window_sets{0};  // values set in it
    bool m_image_quality_given_up{false};
    // every get_X / set_X and getX / setX property of src written into dst (an object of the same type); the names into names
    size_t copy_object_properties(::REManagedObject* src, ::REManagedObject* dst, std::string* names);
    bool m_copy_color_correct{false};
    bool m_color_copy_logged{false};
    ToneTest m_tone_test{};
    bool m_main_local_saved{false}; // the game camera's own value while a test changes it
    bool m_main_local_value{true};
    bool m_main_auto_saved{false};
    uint32_t m_main_auto_value{0};
    std::unordered_map<RECamera*, uint32_t> m_clone_auto_saved{}; // the clones' own automatic exposure while a test changes it
    // [EV LOCK 13.11.2026] the game camera's own range while the lock of all views changes it; the exposure log's frame count
    bool m_main_range_saved{false};
    float m_main_range_saved_min{0.0f};
    float m_main_range_saved_max{0.0f};
    uint32_t m_exposure_log_frames{0};
    // [EV FOLLOW 13.11.2026] per eye slot (clone 0 / 1): the followed offset, the last measurement used, the smoothed error
    // (log2 of periphery / fovea brightness; a lower EV brightens)
    float m_ev_follow_offset[2]{};
    uint64_t m_ev_follow_seen[2]{};
    float m_ev_follow_error[2]{};
    // [REAL EV 13.11.2026] per clone: its own measured offset (SnappedRealEV - EV), smoothed; the cost of calcRealEV
    float m_real_ev_clone_offset[MAX_CLONES]{};
    float m_real_ev_last_set[MAX_CLONES]{-1000.0f, -1000.0f, -1000.0f};
    double m_real_ev_calc_us_sum{0.0};
    double m_real_ev_calc_us_max{0.0};
    uint32_t m_real_ev_calc_frames{0};
    uint32_t m_real_ev_frame{0};
    // [REAL EV fix105 20.11.2026] the game camera's calcRealEV: frames between two calls (4 or 8), frames since the last, the
    // value of the call before; its exposure mode as last seen (component, UseAutoExposure, EV) and a change of it not yet
    // measured; per fovea view: measurements in a row with its own offset 0 (up to 8: settled); the log's counts
    uint32_t m_real_ev_interval{4};
    uint32_t m_real_ev_since{1000};
    float m_real_ev_prev{-1000.0f};
    void* m_real_ev_comp{nullptr};
    bool m_real_ev_auto{false};
    float m_real_ev_ev{-1000.0f};
    bool m_real_ev_mode_pending{false};
    uint32_t m_real_ev_mode_calls{0};
    uint8_t m_real_ev_own_zero[MAX_CLONES]{};
    uint32_t m_real_ev_clone_wait[MAX_CLONES]{};  // settled: frames to its next check
    void* m_real_ev_clone_comp[MAX_CLONES]{};     // the component and the mode its settling belongs to
    int m_real_ev_mode_seen[MAX_CLONES]{};
    uint32_t m_real_ev_clone_calls{0};
    double m_real_ev_clone_us_sum{0.0};

    void clone_camera(size_t wanted);
    void hook_get_primary_camera();
    void copy_camera_properties(size_t wanted);
    // [CLONE POSE 09.11.2026] the game camera's world pose (transform and first joint) written into its clones, see the .cpp
    void sync_clone_poses();
#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    void log_camera_check(); // [CAMERA CHECK 10.11.2026] see quadview::State::camera_check_request
#endif
    // [FOVEA RAY TRACING 12.11.2026] the game camera's via.render.ExperimentalRayTrace given to the clones, see the .cpp
    void sync_clone_ray_tracing(size_t wanted);
    // [FXAA 12.11.2026] the via.render.FXAA components of the game camera and of every clone switched off (VR option)
    void force_fxaa_off();
    uint64_t m_fxaa_switched_off{0};
    // every value-type property of src that differs in dst written into dst (references are left alone); the names of the
    // copied and of the skipped properties into names / skipped
    size_t sync_object_values(::REManagedObject* src, ::REManagedObject* dst, std::string* names, std::string* skipped);
    std::string m_rt_state{};
    bool m_rt_names_logged{false};
    // [RT LIFETIME 12.11.2026] the game camera's ray tracing component and the frames it has been on; per clone its state
    // (see sync_clone_ray_tracing)
    REComponent* m_rt_game_component{nullptr};
    uint32_t m_rt_game_frames{0};
    struct RtClone {
        uint32_t in_use_frames{0};        // frames in a row the clone has been drawing with the game camera's component on
        uint32_t off_frames{0};           // frames its component has been off (destroyed after RT_DESTROY_FRAMES)
        REComponent* destroyed{nullptr};  // destroyed, still listed until the engine removes it
    };
    std::unordered_map<RECamera*, RtClone> m_rt_clones{};
    uint32_t m_rt_on_frames{0}; // [RT CHECK 12.11.2026] (debug build)
    uint32_t m_rt_checks{0};
    void apply_output_ids();

    sdk::SystemArray* get_all_cameras();

    bool m_copy_camera{true};
    bool m_called_activate{false};
    bool m_hooked_get_primary_camera{false};
    bool m_identified_new_cameras{false};
    bool m_waiting_for_identify{false};
    std::recursive_mutex m_camera_mutex{};
    std::unordered_set<RECamera*> m_seen_cameras{};
    std::unordered_set<RECamera*> m_old_cameras{};
    std::unordered_set<RECamera*> m_new_cameras{};
    std::unordered_map<RECamera*, std::vector<RECamera*>> m_old_to_new_cameras{};
    std::unordered_map<RECamera*, RECamera*> m_new_to_old_camera{};
    // the via.render.RenderOutput component of each clone and the id it was given
    std::unordered_map<RECamera*, ::REManagedObject*> m_clone_render_output{};
    std::unordered_map<RECamera*, int32_t> m_clone_output_id{};
    std::array<int32_t, MAX_CLONES> m_wanted_output_ids{3, 3, 3};

    RECamera* m_last_primary_camera{nullptr};

    struct GetterSetter {
        sdk::REMethodDefinition* getter{nullptr};
        sdk::REMethodDefinition* setter{nullptr};
    };

    std::unordered_map<sdk::RETypeDefinition*, std::unordered_map<std::string, GetterSetter>> m_getter_setters{};

    struct WantedComponent {
        std::string name{};
        std::unordered_set<size_t> ignored_properties{};
        bool allowed{true}; // used for debug imgui checkbox
    };

    // Components that we want to copy from the old camera to the new camera
    std::vector<WantedComponent> m_wanted_components {
        WantedComponent{ "via.render.ToneMapping", { "set_InjectingGameObject"_fnv } },
        WantedComponent{ "via.render.LDRPostProcess", {} },
        WantedComponent{ "via.render.SoftBloom", {} },
        WantedComponent{ "via.render.SSRControl", {} },
        WantedComponent{ "via.render.SSAOControl", {} },
        WantedComponent{ "via.render.SSSSSControl", {} },
        WantedComponent{ "via.render.DepthOfField", {} },
        WantedComponent{ "via.render.LightShaftFilterControl", {} },
        WantedComponent{ "via.render.FakeLensFlare", {} },
        WantedComponent{ "via.render.MotionBlur", {} },
        WantedComponent{ "via.render.FFTBloom", {} },
        WantedComponent{ "via.render.Echo", {} },
        WantedComponent{ "via.render.OverlayFXAA", {} },
        WantedComponent{ "via.render.TessellationFactor", {} },
        WantedComponent{ "via.render.ShadowQualityControl", {} },
        WantedComponent{ "via.render.Fog", { "set_FSSunPosGameObject"_fnv } },
        WantedComponent{ "via.render.VolumetricFog", { } },
        WantedComponent{ "via.render.VolumetricFogControl", { } },
        WantedComponent{ "via.render.GodRay", { } },
        WantedComponent{ "via.render.GeometryAOControl", { } },
        WantedComponent{ "via.render.MotionBlur", { } },
        WantedComponent{ "via.render.FakeLensflare", { } },
        WantedComponent{ "via.render.RetroFilm", { } },
#if defined(RE7)
        // [RE7 11.11.2026] VR::set_lens_distortion turns the game camera's lens distortion off; the clones get the
        // same state instead of the component's defaults
        WantedComponent{ "via.render.LensDistortion", { } },
#endif
#if !defined(RE4)
        // [FOVEA RAY TRACING 12.11.2026] the camera check of 12.11 (RE7): CapturePlane and PolynomialLens are off on the
        // game camera but on in the clones (created with the component's defaults, their state never copied); FXAA, also
        // on in the clones, is switched off in every view by force_fxaa_off
        WantedComponent{ "via.render.CapturePlane", { } },
        WantedComponent{ "via.render.PolynomialLens", { } },
#endif
        //WantedComponent{ "via.render.ExperimentalRayTrace", {}}
        //WantedComponent{ "via.render.CustomFilter", { } },
    };

    // property copy jobs, one list per clone (the copy is spread over several frames)
    struct PropertyJobs {
        std::vector<std::function<void()>> jobs{};
        std::unordered_set<sdk::RETypeDefinition*> types_added{};
        size_t index{0};
    };

    std::unordered_map<RECamera*, PropertyJobs> m_property_jobs{};
};
}
