#define NOMINMAX

#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <fstream>
#include <algorithm> // [GAME OPTIONS 12.11.2026]
#include <cctype>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtx/transform.hpp>

#include <utility/Profiler.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/TDBVer.hpp>
#include <reframework/API.hpp>

#if TDB_VER <= 49
#include "sdk/regenny/re7/via/Window.hpp"
#include "sdk/regenny/re7/via/SceneView.hpp"
#elif TDB_VER < 69
#include "sdk/regenny/re3/via/Window.hpp"
#include "sdk/regenny/re3/via/SceneView.hpp"
#elif TDB_VER == 69
#include "sdk/regenny/re8/via/Window.hpp"
#include "sdk/regenny/re8/via/SceneView.hpp"
#elif TDB_VER == 70
#include "sdk/regenny/re2_tdb70/via/Window.hpp"
#include "sdk/regenny/re2_tdb70/via/SceneView.hpp"
#elif TDB_VER >= 71
#ifdef SF6
#include "sdk/regenny/sf6/via/Window.hpp"
#include "sdk/regenny/sf6/via/SceneView.hpp"
#elif defined(RE4)
#include "sdk/regenny/re4/via/Window.hpp"
#include "sdk/regenny/re4/via/SceneView.hpp"
#elif defined(DD2) || TDB_VER >= 74
#include "sdk/regenny/dd2/via/Window.hpp"
#include "sdk/regenny/dd2/via/SceneView.hpp"
#else
#include "sdk/regenny/mhrise_tdb71/via/Window.hpp"
#include "sdk/regenny/mhrise_tdb71/via/SceneView.hpp"
#endif
#endif

#include "sdk/Math.hpp"
#include "sdk/SceneManager.hpp"
#include "sdk/RETypeDB.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/Application.hpp"
#include "sdk/Renderer.hpp"
#include "sdk/REMath.hpp"
#include "sdk/REGameObject.hpp"

#include "utility/Scan.hpp"
#include "utility/FunctionHook.hpp"
#include "utility/Module.hpp"
#include "utility/Memory.hpp"
#include "utility/Registry.hpp"
#include "utility/ScopeGuard.hpp"

#include "FirstPerson.hpp"
#include "ManualFlashlight.hpp"
#include "vr/QuadViewGame.hpp" // what the quad view knows about the game (item markers)
#include "TemporalUpscaler.hpp"
#include "VR.hpp"
#include "Hooks.hpp" // [PROJECTION CALLERS 15.11.2026] the engine's call site of a projection request
#include "vr/CpuStats.hpp" // [SUBMIT 05.11.2026]
#include "vr/d3d12/SubmitTimeline.hpp" // [SUBMIT TIMELINE 06.11.2026 evening] points on the game's queue in the present path
#if defined(RE7) || defined(RE8)
#include <thread>
#include "vr/games/RE8VR.hpp" // [HEIGHT PROBE 12.11.2026]
#include <sdk/MurmurHash.hpp>
#endif

// ============================================================================
// [UI_SONDE 28.09.2026 -- Ansage des Users] Messen, wie die Overlay-/UI-Ebene im
// Multipass pro Frame laeuft (Grundlage fuer "UI vom Upscaler/PostPass
// ausnehmen"). GOLDENE REGEL: der Fork schreibt keine Logs -- er zaehlt nur
// (atomar, Render-Thread) und veroeffentlicht die Werte als Lua-Globals
// __re4_uiprobe_* auf dem HAUPTthread (pre BeginRendering), niemals aus einem
// Layer-Hook (dort Lua-Sperre = Deadlock-Gefahr). Mitschreiben: Lua-Sonde.
// ============================================================================
#if defined(RE4)
#include "vr/games/RE4VR.hpp"
#include <atomic>
#include <mutex>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

QUADVIEW_BUILD_STAMP(VR); // [BUILD STAMP fix107]

namespace {
struct UiProbe {
    std::atomic<uint32_t> overlay_pre_draw{0};
    std::atomic<uint32_t> overlay_draw{0};
    std::atomic<uint32_t> overlay_pre_update{0};
    std::atomic<uint32_t> overlay_suppressed{0};
    std::atomic<uint32_t> ui_buffer_nonnull{0};
    std::atomic<uint32_t> scene_pre_update{0};
    std::atomic<uint32_t> ui_target_nonnull{0};
    uint32_t frames{0};
    std::mutex mtx{};
    std::unordered_set<uintptr_t> overlay_parents{};
    std::unordered_set<uintptr_t> overlay_layers{};
    std::unordered_set<uintptr_t> scene_layers{};
    std::unordered_set<uintptr_t> ui_buffers{};
};

UiProbe g_ui_probe{};

// [UI_TEST 28.09.2026] Schwarz-Sichttest: wo entsteht das VR-UI im Multipass?
// Lua-Global __re4_uitest_clear (auf dem HAUPTthread gelesen, pre BeginRendering):
//   0 = aus
//   1 = VOR dem PostEffect-Schritt das HDR-Bild (HDRTarget) schwarz machen
//   2 = NACH dem PostEffect-Schritt das Ergebnis (PostMainTarget) schwarz machen
//   3 = VOR der Overlay-Ebene PostMainTarget schwarz
//   4 = VOR PrepareOutput PostMainTarget schwarz
//   5 = NACH PrepareOutput dessen Ausgabe (OutputState) schwarz
// Befund 28.09.: bei 1 UND 2 bleibt das UI stehen -> es entsteht NACH PostEffect.
// Sieht man danach noch UI, wird es SPAETER gezeichnet als die Stelle.
std::atomic<int32_t> g_ui_test_mode{0};

void ui_test_clear_rtv(void* render_ctx, sdk::renderer::TargetState* ts) {
    if (render_ctx == nullptr || ts == nullptr) {
        return;
    }

    auto rtv = ts->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    ((sdk::renderer::RenderContext*)render_ctx)->clear_rtv(rtv.get(), black, false);
}

void ui_test_clear(void* render_ctx, sdk::renderer::RenderLayer* scene_layer, const char* target) {
    if (render_ctx == nullptr || scene_layer == nullptr) {
        return;
    }

    auto ts = scene_layer->get_target_state(target);

    if (ts == nullptr) {
        return;
    }

    auto rtv = ts->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
    ((sdk::renderer::RenderContext*)render_ctx)->clear_rtv(rtv.get(), black, false);
}

void ui_probe_note(std::unordered_set<uintptr_t>& set, const void* p) {
    if (p == nullptr) {
        return;
    }

    std::scoped_lock _{g_ui_probe.mtx};

    if (set.size() < 64) {
        set.insert((uintptr_t)p);
    }
}

// Hauptthread: alle 90 Frames Mittelwerte pro Frame + Anzahl verschiedener Objekte.
void ui_probe_publish() {
    auto& p = g_ui_probe;

    // [UI_REIHENFOLGE] Reihenfolge des letzten Frames merken, neu beginnen
    {
        auto& v = VR::get();
        std::scoped_lock _{v->m_ui_order_mtx};

        if (!v->m_ui_order.empty()) {
            v->m_ui_order_last = v->m_ui_order;
        }

        v->m_ui_order.clear();
    }

    // [UI_TEST] Modus jeden Frame vom Lua-Global uebernehmen (Hauptthread).
    g_ui_test_mode = (int32_t)re4vr::lua_get_number_opt("__re4_uitest_clear").value_or(0.0);
    VR::get()->m_ui_debug_mask = (g_ui_test_mode.load() == 6);   // [UI_MASKE_ROT]

    if (++p.frames < 90) {
        return;
    }

    const double f = (double)p.frames;
    size_t parents = 0, layers = 0, scenes = 0, buffers = 0;

    {
        std::scoped_lock _{p.mtx};
        parents = p.overlay_parents.size();
        layers = p.overlay_layers.size();
        scenes = p.scene_layers.size();
        buffers = p.ui_buffers.size();
        p.overlay_parents.clear();
        p.overlay_layers.clear();
        p.scene_layers.clear();
        p.ui_buffers.clear();
    }

    re4vr::lua_set_number("__re4_uiprobe_frames", f);
    re4vr::lua_set_number("__re4_uiprobe_overlay_pre_draw", p.overlay_pre_draw.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_overlay_draw", p.overlay_draw.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_overlay_pre_update", p.overlay_pre_update.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_overlay_suppressed", p.overlay_suppressed.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_ui_buffer_nonnull", p.ui_buffer_nonnull.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_scene_pre_update", p.scene_pre_update.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_ui_target_nonnull", p.ui_target_nonnull.exchange(0) / f);
    re4vr::lua_set_number("__re4_uiprobe_overlay_parents", (double)parents);
    re4vr::lua_set_number("__re4_uiprobe_overlay_layers", (double)layers);
    re4vr::lua_set_number("__re4_uiprobe_scene_layers", (double)scenes);
    re4vr::lua_set_number("__re4_uiprobe_ui_buffers", (double)buffers);
    re4vr::lua_set_number("__re4_uiprobe_seq", (double)(uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    re4vr::lua_set_number("__re4_uitest_active", (double)g_ui_test_mode.load());
    {
        auto& v = VR::get();
        std::scoped_lock _{v->m_ui_order_mtx};
        re4vr::lua_set_string("__re4_uiprobe_order", v->m_ui_order_last);
    }
    // [PAARE 28.09.2026] UI-Trennung: wie oft passte das Kopien-Paar (pro 90 Frames)
    {
        auto& v = VR::get();
        re4vr::lua_set_number("__re4_uiprobe_pair_ok_l", (double)v->m_ui_pair_ok[0].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_pair_ok_r", (double)v->m_ui_pair_ok[1].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_pair_bad_l", (double)v->m_ui_pair_bad[0].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_pair_bad_r", (double)v->m_ui_pair_bad[1].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_pair_old_l", (double)v->m_ui_pair_old[0].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_pair_old_r", (double)v->m_ui_pair_old[1].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_cap_hud_l", (double)v->m_ui_cap_hud[0].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_cap_hud_r", (double)v->m_ui_cap_hud[1].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_cap_fin_l", (double)v->m_ui_cap_fin[0].exchange(0));
        re4vr::lua_set_number("__re4_uiprobe_cap_fin_r", (double)v->m_ui_cap_fin[1].exchange(0));
    }

    p.frames = 0;
}
}
#endif

bool inside_on_end = false;
uint32_t actual_frame_count = 0;

thread_local bool inside_gui_draw = false;

std::shared_ptr<VR>& VR::get() {
    static auto inst = std::make_shared<VR>();
    return inst;
}

std::unique_ptr<FunctionHook> g_input_hook{};
std::unique_ptr<FunctionHook> g_projection_matrix_hook2{};
std::unique_ptr<FunctionHook> g_overlay_draw_hook{};
std::unique_ptr<FunctionHook> g_post_effect_draw_hook{};
std::unique_ptr<FunctionHook> g_wwise_listener_update_hook{};
//std::unique_ptr<FunctionHook> g_get_sharpness_hook{};

#if TDB_VER <= 49
std::optional<regenny::via::Size> g_previous_size{};
#endif

// Purpose: spoof the render target size to the size of the HMD displays
void VR::on_view_get_size(REManagedObject* scene_view, float* result) {
    // There are some very dumb optimizations that cause set_DisplayType
    // to go through this hook. This function is actually something like "updateSceneView"
    static thread_local bool already_inside = false;

    if (already_inside) {
        return;
    }

    already_inside = true;

    utility::ScopeGuard _{ [&]() { already_inside = false; } };

    if (!g_framework->is_ready()) {
        return;
    }

    if (!get_runtime()->loaded) {
        return;
    }

    // is_vr_suspended(): flat mode keeps the game's own window/backbuffer size,
    // otherwise the "flat" frame would still be rendered at HMD resolution and aspect.
    if (m_disable_backbuffer_size_override || is_vr_suspended()) {
        return;
    }

    auto regenny_view = (regenny::via::SceneView*)scene_view;
    auto window = regenny_view->window;

    static auto via_scene_view = sdk::find_type_definition("via.SceneView");
    static auto set_display_type_method = via_scene_view != nullptr ? via_scene_view->get_method("set_DisplayType") : nullptr;

    // Force the display to stretch to the window size
    if (set_display_type_method != nullptr) {
        set_display_type_method->call(sdk::get_thread_context(), regenny_view, via::DisplayType::Fit);
    } else {
#if not defined(RE7) || TDB_VER <= 49
        static auto is_sunbreak = utility::get_module_path(utility::get_executable())->find("MHRiseSunbreakDemo") != std::string::npos;

        if (is_sunbreak) {
            *(regenny::via::DisplayType*)((uintptr_t)&regenny_view->display_type + 4) = regenny::via::DisplayType::Fit;
        } else {
            regenny_view->display_type = regenny::via::DisplayType::Fit;
        }
#else
        *(regenny::via::DisplayType*)((uintptr_t)&regenny_view->display_type + 4) = regenny::via::DisplayType::Fit;
#endif
    }

    auto wanted_width = 0.0f;
    auto wanted_height = 0.0f;

    // Set the window size, which will increase the size of the backbuffer
    if (window != nullptr) {
        static const auto is_gng = utility::get_module_path(utility::get_executable())->find("makaimura_GG_RE.exe") != std::string::npos;

        auto& window_width = is_gng ? *(uint32_t*)((uintptr_t)window + 0x48) : window->width;
        auto& window_height = is_gng ? *(uint32_t*)((uintptr_t)window + 0x4C) : window->height;

        if (is_hmd_active()) {
#if TDB_VER <= 49
            if (!g_previous_size) {
                g_previous_size = regenny::via::Size{ (float)window->width, (float)window->height };
            }
#endif

            if (!TemporalUpscaler::get()->activated()) {
                if (!is_using_multipass()) {
                    window_width = get_hmd_width();
                    window_height = get_hmd_height();
                } else {
                    window_width = get_hmd_width() + 1;
                    window_height = get_hmd_height() + 1;
                }
            } else {
                window_width = get_hmd_width();
                window_height = get_hmd_height();

                // [SMALL WINDOW fix122 22.11.2026] The quad view's eye images are the mod's own and have the headset's size
                // whatever the window's; the engine's views are small (1240 x 960 at the user's settings). Still the window,
                // and with it the backbuffer, was an eye image's size (11.8 megapixels), and the engine fits its picture
                // into it every frame for a desktop window nobody looks at. With the option on the window is an engine
                // view's size plus one (the engine keeps a colour buffer of its own for the scene only while the two
                // differ). OpenVR, multipass with the upscaler mod (the quad view) only.
                uint32_t small_w = 0;
                uint32_t small_h = 0;

                if (is_using_multipass() && get_runtime()->is_openvr() && quadview::State::get().small_window.load(std::memory_order_relaxed) &&
                    quadview::State::get().requested() && TemporalUpscaler::get()->small_window_size(small_w, small_h)) {
                    window_width = small_w;
                    window_height = small_h;
                }
            }

            if (m_is_d3d12 && m_d3d12.is_initialized()) {
                const auto& backbuffer_size = m_d3d12.get_backbuffer_size();

                if (backbuffer_size[0] > 0 && backbuffer_size[1] > 0) {
                    if (std::abs((int)backbuffer_size[0] - (int)window_width) > 50 || std::abs((int)backbuffer_size[1] - (int)window_height) > 50) {
                        const auto now = get_game_frame_count();

                        if (!m_backbuffer_inconsistency) {
                            m_backbuffer_inconsistency_start = now;
                            m_backbuffer_inconsistency = true;
                        }

                        const auto is_true_inconsistency = (now - m_backbuffer_inconsistency_start) >= 5;

                        if (is_true_inconsistency) {
                            // Force a reset of the backbuffer size
                            window_width = window_width + 1;
                            window_height = window_height + 1;

                            REVR_LOG(spdlog::info("[VR] Previous backbuffer size: {}x{}", backbuffer_size[0], backbuffer_size[1]));
                            REVR_LOG(spdlog::info("[VR] Backbuffer size inconsistency detected, resetting backbuffer size to {}x{}", window_width, window_height));

                            // m_backbuffer_inconsistency gets set to false on device reset.
                        }
                    }
                } else {
                    m_backbuffer_inconsistency = false;
                }
            }
        } else {
            m_backbuffer_inconsistency = false;

#if TDB_VER > 49
            window_width = is_gng ? (uint32_t)*(float*)((uintptr_t)window + 0x88) : (uint32_t)window->borderless_size.w;
            window_height = is_gng ? (uint32_t)*(float*)((uintptr_t)window + 0x8C) : (uint32_t)window->borderless_size.h;
#else
            if (g_previous_size) {
                window->width = (uint32_t)g_previous_size->w;
                window->height = (uint32_t)g_previous_size->h;

                g_previous_size = std::nullopt;
            }
#endif
        }

        wanted_width = (float)window_width;
        wanted_height = (float)window_height;

        // Might be usable in other games too
#if defined(SF6) || TDB_VER >= 73
        if (!is_gng) {
            window->borderless_size.w = (float)window_width;
            window->borderless_size.h = (float)window_height;
        }
#endif
    }

    //auto out = original_func(scene_view, result);

    if (!m_in_render) {
        //return original_func(scene_view, result);
    }

    // spoof the size to the HMD's size
    if (!TemporalUpscaler::get()->activated()) {
        if (!is_using_multipass()) {
    #if TDB_VER < 73
            result[0] = wanted_width;
            result[1] = wanted_height;
    #else
            // Stupid optimizations cause the game to not use the result variant of this function
            // but rather update the current scene view's size directly.
            regenny_view->size.w = wanted_width;
            regenny_view->size.h = wanted_height;
    #endif
        } else {
    #if TDB_VER < 73
            result[0] = wanted_width - 1.0f;
            result[1] = wanted_height - 1.0f;
    #else
            // Stupid optimizations cause the game to not use the result variant of this function
            // but rather update the current scene view's size directly.
            regenny_view->size.w = wanted_width - 1.0f;
            regenny_view->size.h = wanted_height - 1.0f;
    #endif
        }
    }
}

// [QUADVIEW] true while THIS thread updates the inset (cloned camera) scene layer in quad view.
// Thread local: game-logic threads that ask for the projection meanwhile keep getting the normal one.
static thread_local bool t_quadview_inset_scope = false;
// [STEREO FOVEA 27.10.2026] eye of the scene layer THIS thread is updating (multipass: -1 outside a layer update)
static thread_local int32_t t_layer_eye = -1;
// [VIEW RACE 28.10.2026] this thread runs the application entries (the game's main loop: behaviours, GUI layout) /
// updates scene layers (the render side). Only the first kind without the second is treated as game logic; other
// threads (jobs working for a layer update) keep the pass as before.
// [VIEW RACE 03.11.2026] The layer flag was set by every scene layer update and never cleared: the game's main thread runs
// the entries AND updates the scene layers, so it never counted as game logic and the head was never given (the log's
// "views for game logic (head)/frame" was 0.0 in every log since 28.10). The item names kept jumping sideways in both
// eyes at once (dump of 03.11, ~1.7 px in the fovea). Now the flag holds only during a layer update (pre to post).
// [VIEW RACE 03.11.2026, second] Test of 12:00: the main thread got the head (~100 requests per frame, mostly in
// UpdatePrimitive), but ~350-400 requests per frame come from other threads (the engine's jobs) and the item names are laid
// out there: they kept the eye of whichever layer the render side was on (first stable and shifted to the right, then
// jumping again). Jobs of a layer update run while that update runs; while none runs, other threads work for game logic.
static thread_local bool t_entry_thread = false;
static thread_local bool t_in_layer_update = false;
// [LIGHTS fix82 17.11.2026] inside a fovea view's scene layer update: the engine got the eye's whole frustum for it (see
// quadview::State::whole_infos_now); on_scene_layer_update puts the fovea's part into the image's matrices
static thread_local bool t_whole_update = false;
static thread_local const char* t_entry_name = nullptr; // the application entry this thread is in (log)
static std::atomic<int32_t> s_layer_updates_running{0}; // scene layer updates between their pre and post hooks

bool VR::view_of_camera(const RECamera* camera, int32_t& eye, bool& fovea) const {
    if (camera == nullptr) {
        return false;
    }

    int32_t index = -1;

    for (size_t i = 0; i < m_multipass_cameras.size(); ++i) {
        if (m_multipass_cameras[i] != nullptr && m_multipass_cameras[i] == camera) {
            index = (int32_t)i;
            break;
        }
    }

    if (index < 0) {
        return false;
    }

    if (is_using_multipass()) {
        // a clone beyond what the technique wants (left over from another mode) renders nothing of ours
        if (index > 0 && (uint32_t)(index - 1) >= wanted_camera_clones()) {
            return false;
        }

        // the quad view roles only once the upscaler runs them (engaged): while it sets up, the views are the plain
        // multipass ones (primary = left, clone 0 = right), which is what the upscaler evaluates meanwhile
        if (is_stereo_fovea() && quadview::State::get().engaged()) {
            if (quadview::State::get().mono_periphery()) {
                // [MONO PERIPHERY] primary = wide view of both eyes, clone 0 = fovea left, clone 1 = fovea right
                eye = index == 0 ? CENTER_EYE : index - 1;
                fovea = index > 0;
            } else {
                // primary = wide left, clone 0 = wide right, clone 1 = fovea left, clone 2 = fovea right
                eye = index % 2;
                fovea = index >= 2;
            }
        } else {
            eye = index == 0 ? 0 : 1;
            fovea = false;
        }

        return true;
    }

    return false;
}

float VR::fovea_cull_fov(int32_t eye, float max_fov, float aspect, bool vertical) {
    if (!is_using_multipass() || !is_quadview_requested() || !is_stereo_fovea() || !quadview::State::get().engaged() || !(max_fov > 1.0f) ||
        eye < 0 || eye > 1) {
        return max_fov;
    }

    quadview::NdcRect rect{};

    if (!quadview::State::get().inset_for_eye(eye, rect, (int64_t)(uint32_t)m_frame_count)) {
        return max_fov;
    }

    // the fovea's edges as tangents around the eye's axis, enlarged by 15 % (the gaze moves between the engine's choice of
    // objects and the frame's projection)
    const float fov = quadview::cull_fov(get_projection_matrix((uint32_t)eye), rect, aspect, vertical, max_fov, 1.15f);

    auto& qv = quadview::State::get();
    const auto mdeg = (uint32_t)(fov * 1000.0f);
    qv.narrow_fov_count.fetch_add(1, std::memory_order_relaxed);
    qv.narrow_fov_sum.fetch_add(mdeg, std::memory_order_relaxed);
    qv.narrow_fov_engine.store((uint32_t)(max_fov * 1000.0f), std::memory_order_relaxed);

    for (auto cur = qv.narrow_fov_min.load(std::memory_order_relaxed); mdeg < cur && !qv.narrow_fov_min.compare_exchange_weak(cur, mdeg);) {
    }

    for (auto cur = qv.narrow_fov_max.load(std::memory_order_relaxed); mdeg > cur && !qv.narrow_fov_max.compare_exchange_weak(cur, mdeg);) {
    }

    return fov;
}

void VR::apply_quadview_inset(REManagedObject* camera, Matrix4x4f& projection, bool gui_camera) {
    if (!is_quadview_requested()) {
        return;
    }

    // [MONO PERIPHERY] the GUI of a fovea layer is drawn with the eye's full projection: it is not composited with the
    // fovea image but taken from that layer as this eye's interface (see ui_capture)
    if (gui_camera && is_mono_periphery()) {
        return;
    }

    auto& qv = quadview::State::get();
    int32_t eye = -1;
    bool fovea = false;
    const bool by_camera = !gui_camera && view_of_camera((const RECamera*)camera, eye, fovea) && fovea;
    const bool by_scope = t_quadview_inset_scope;

    if (!by_camera && !by_scope) {
        return;
    }

    if (!by_camera) {
        // inside the layer update of a fovea view: its eye
        eye = t_layer_eye;

        if (eye < 0) {
            return;
        }
    }

    quadview::NdcRect rect{};

    const auto frame = m_frame_count;

    if (!qv.inset_for_eye(eye, rect, (int64_t)(uint32_t)frame)) {
        return; // layout not active yet: the clone renders the normal frustum
    }

    // [LIGHT FLICKER TEST 15.11.2026 evening] one phase of the test: the fovea views with their eye's whole frustum (does what
    // the fovea views leave out change the wide view's lights?)
    if (qv.test_full_fovea_frustum.load(std::memory_order_relaxed)) {
        return;
    }

    // [PROJECTION CALLERS 15.11.2026] where the engine asks (call site and context, see quadview::State::proj_sites), and the
    // light flicker test's phases that give the eye's whole frustum to one call site or to the requests of some contexts
    if (!gui_camera) {
        const int ctx = t_in_layer_update ? (t_layer_eye == eye ? 0 : 1) : (s_layer_updates_running.load(std::memory_order_relaxed) > 0 ? 2 : 3);
        const uintptr_t site = Hooks::projection_caller();

        if (by_camera) {
            qv.note_proj_site(site, ctx);
        }

        if (site != 0 && qv.test_full_frustum_site.load(std::memory_order_relaxed) == site) {
            return;
        }

        if (((qv.test_full_frustum_ctx.load(std::memory_order_relaxed) >> (uint32_t)ctx) & 1u) != 0) {
            return;
        }
    }

    // [LIGHTS fix82 17.11.2026] the engine's own update of this fovea view works with the eye's whole frustum; its image gets
    // the fovea's part afterwards (on_scene_layer_update -> narrow_fovea_layer)
    if (!gui_camera && t_in_layer_update && t_layer_eye == eye && qv.update_whole_now()) {
        t_whole_update = true;
        qv.whole_update_requests.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // [CPU PROBE 03.11.2026] the fovea views almost empty (2 % of their frustum around its centre), to measure their CPU cost
    if (qv.cpu_probe_tiny_fovea.load(std::memory_order_relaxed)) {
        const float cx = 0.5f * (rect.x0 + rect.x1);
        const float cy = 0.5f * (rect.y0 + rect.y1);
        const float hx = 0.01f * (rect.x1 - rect.x0);
        const float hy = 0.01f * (rect.y1 - rect.y0);
        rect = quadview::NdcRect{cx - hx, cy - hy, cx + hx, cy + hy};
    }

    quadview::apply_sub_frustum(projection, quadview::sub_frustum(rect));

    if (gui_camera) {
        ++qv.gui_inset;
    } else if (by_camera) {
        ++qv.inset_by_camera;
    } else {
        ++qv.inset_by_scope;
    }
}

// [LIGHTS fix82 17.11.2026] After a fovea view's scene layer update that got the eye's whole frustum (apply_quadview_inset):
// the fovea's part of it into every set of matrices the engine made for the layer's image, as the request would have given
// it (the same rectangle of the frame). A set that does not hold the eye's whole frustum (x scale over 1.2 times the eye's)
// is left as it is. The upscaler's callback follows this one (Mods.cpp) and adds its jitter to these matrices.
// [LIGHTS fix83 17.11.2026] keep_whole: the sets that keep the eye's whole frustum (bit 0 the scene's, 1 depth distortion,
// 2 filter, 3 jitter disable, 4 jitter disable post, 5 z prepass; quadview::State::whole_infos_now)
void VR::narrow_fovea_layer(sdk::renderer::layer::Scene* layer, int32_t eye, uint32_t keep_whole) {
    auto& qv = quadview::State::get();
    quadview::NdcRect rect{};

    if (layer == nullptr || eye < 0 || eye > 1 || !qv.inset_for_eye(eye, rect, (int64_t)(uint32_t)m_frame_count)) {
        return;
    }

    if (qv.cpu_probe_tiny_fovea.load(std::memory_order_relaxed)) { // [CPU PROBE 03.11.2026] as apply_quadview_inset
        const float cx = 0.5f * (rect.x0 + rect.x1);
        const float cy = 0.5f * (rect.y0 + rect.y1);
        const float hx = 0.01f * (rect.x1 - rect.x0);
        const float hy = 0.01f * (rect.y1 - rect.y0);
        rect = quadview::NdcRect{cx - hx, cy - hy, cx + hx, cy + hy};
    }

    const auto sub = quadview::sub_frustum(rect);
    const float whole = std::abs(get_projection_matrix((uint32_t)eye)[0][0]);
    bool any = false;

    uint32_t bit = 1u;

    for (sdk::renderer::SceneInfo* si : {layer->get_scene_info(), layer->get_depth_distortion_scene_info(), layer->get_filter_scene_info(),
             layer->get_jitter_disable_scene_info(), layer->get_jitter_disable_post_scene_info(), layer->get_z_prepass_scene_info()}) {
        const bool keep = (keep_whole & bit) != 0;
        bit <<= 1;

        if (keep || si == nullptr || !(whole > 0.0f) || std::abs(si->projection_matrix[0][0]) > 1.2f * whole) {
            continue;
        }

        quadview::apply_sub_frustum(si->projection_matrix, sub);
        si->inverse_projection_matrix = glm::inverse(si->projection_matrix);
        si->view_projection_matrix = si->projection_matrix * si->view_matrix;
        si->inverse_view_projection_matrix = glm::inverse(si->view_projection_matrix);
        any = true;
    }

    if (any) {
        qv.whole_update_narrowed.fetch_add(1, std::memory_order_relaxed);
    }
}

void VR::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    quadview::State::get().calls_projection.fetch_add(1, std::memory_order_relaxed); // [CPU 22.10.2026]

    // should_suspend_camera_overrides(): canvas mode needs the engine's own projection.
    if (result == nullptr || !g_framework->is_ready() || !is_hmd_active() || m_disable_projection_matrix_override ||
        should_suspend_camera_overrides()) {
        return;
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!m_in_render) {
       // return original_func(camera, result);
    }

    if (m_in_lightshaft) {
        //return original_func(camera, result);
    }

    if (is_using_multipass()) {
        // [STEREO FOVEA] the clones of the quad view ask for their projection too
        if (!is_multipass_camera(camera)) {
            return;
        }
    }

#ifdef RE4
    if (const auto game_object = ((REComponent*)camera)->ownerGameObject; game_object != nullptr) {
        if (game_object->name != nullptr) {
            // Allows the sniper scope to work
            if (utility::re_string::get_view(game_object->name).starts_with(L"ScopeCamera")) {
                return;
            }
        }
    }
#endif

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?
    int32_t view_eye = -1;
    bool view_fovea = false;

    if (is_using_multipass() && view_of_camera((const RECamera*)camera, view_eye, view_fovea) && view_eye >= 0) {
        // [STEREO FOVEA] the camera says which eye it renders (the pass counter alone cannot tell four layers apart
        // when the engine asks outside the layer update)
        *result = get_projection_matrix((uint32_t)view_eye);
    } else {
        *result = get_current_projection_matrix(false);
    }

    // [QUADVIEW] the cloned camera renders the gaze inset
    apply_quadview_inset(camera, *result, false);
}

// [MAP-ASPECT 29.09.2026] x-Spalte so skalieren, dass die Projektion 16:9 statt des
// Augen-Seitenverhaeltnisses hat (NDC-x um die Bildmitte; gilt fuer Ortho wie
// Perspektive). Matrizen, die schon ~16:9 sind, bleiben unberuehrt.
void VR::map_aspect_apply(Matrix4x4f& m) {
    if (m[0][0] == 0.0f || m[1][1] == 0.0f) {
        return;
    }

    constexpr float target = 16.0f / 9.0f;
    const float aspect = std::abs(m[1][1] / m[0][0]);

    if (std::abs(aspect - target) < 0.01f) {
        return;
    }

    const float k = aspect / target;

    for (int i = 0; i < 4; ++i) {
        m[i][0] *= k;
    }
}

Matrix4x4f* VR::gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result) {
    auto original_func = g_projection_matrix_hook2->get_original<decltype(VR::gui_camera_get_projection_matrix_hook)>();

    auto& vr = VR::get();

    if (result == nullptr || !g_framework->is_ready()) {
        return original_func(camera, result);
    }

    if (!vr->is_hmd_active() || vr->m_disable_gui_camera_projection_matrix_override ||
        vr->should_suspend_camera_overrides()) {
        auto out = original_func(camera, result);

        // [MAP-ASPECT 29.09.2026] Nur auf dem Durchreich-Pfad (GUI-Matrix-Override aus),
        // nur bei offener Karte, nur mit aktivem HMD und ohne Suspend.
        if (vr->m_disable_gui_camera_projection_matrix_override && vr->is_hmd_active() &&
            !vr->should_suspend_camera_overrides() && vr->m_map_aspect_active && vr->m_map_aspect_fix) {
            map_aspect_apply(*result);
        }

        return out;
    }

    /*if (camera != sdk::get_primary_camera()) {
        return original_func(camera, result);
    }*/

    if (!vr->m_in_render) {
       // return original_func(camera, result);
    }

    if (vr->m_in_lightshaft) {
        //return original_func(camera, result);
    }

    // Get the projection matrix for the correct eye
    // For some reason we need to flip the projection matrix here?
#if TDB_VER > 49
    *result = vr->get_current_projection_matrix(false);
#else
    *result = vr->get_current_projection_matrix(true);
#endif

    // [LAYER TRACE] which pass the GUI asked in (multipass)
    if (vr->is_using_multipass()) {
        ++vr->m_gui_proj_calls[(std::min)(vr->m_multipass.pass, 2u)];
    }

    // [QUADVIEW] GUI drawn into the inset view must use the inset frustum too, so it lines up with the wide view
    vr->apply_quadview_inset(camera, *result, true);

    return result;
}

void VR::on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) {
    quadview::State::get().calls_view.fetch_add(1, std::memory_order_relaxed); // [CPU 22.10.2026]

    if (result == nullptr || !g_framework->is_ready()) {
        return;
    }

    if (!is_hmd_active() || m_disable_view_matrix_override || should_suspend_camera_overrides()) {
        return;
    }

    const auto cameras = get_cameras_for_view_hook();
    bool ours = false;

    for (auto c : cameras) {
        if (c != nullptr && camera == c) {
            ours = true;
            break;
        }
    }

    if (!ours && !(is_using_multipass() && is_multipass_camera(camera))) {
        return;
    }

    // A little note on this. We can't actually use the camera (when using multipass) to detect how to adjust the view matrix
    // For some reason, the game *always* uses the main camera when calling this function, even though it's rendering the other camera
    // So we detect which pass is getting called and adjust the view matrix accordingly
    //if (!is_using_multipass()) {
        auto& mtx = *result;

        //get the flipped eye to get the correct transform. something something right->left handedness i think
        const auto current_eye_transform = get_current_eye_transform(true);
        //auto current_head_pos = -(glm::inverse(vr->get_rotation(0)) * ((vr->get_position(0)) - vr->m_standing_origin));
        //current_head_pos.w = 0.0f;

        // Apply the complete eye transform. This fixes the need for parallel projections on all canted headsets like Pimax
        mtx = current_eye_transform * mtx;
    //}
}

HookManager::PreHookResult VR::pre_set_hdr_mode(std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
    if (!VR::get()->is_hmd_active()) {
        return HookManager::PreHookResult::CALL_ORIGINAL;
    }

    if (args.size() >= 2) {
        args[1] = 0;
    }

    return HookManager::PreHookResult::CALL_ORIGINAL;
}

void VR::inputsystem_update_hook(void* ctx, REManagedObject* input_system) {
    auto original_func = g_input_hook->get_original<decltype(VR::inputsystem_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(ctx, input_system);
        return;
    }

    auto& mod = VR::get();
    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - mod->get_last_controller_update()) <= std::chrono::seconds(10);

    if (mod->get_controllers().empty()) {
        // no controllers connected, don't do anything
        original_func(ctx, input_system);
        return;
    }

    auto lstick = sdk::call_object_func<REManagedObject*>(input_system, "get_LStick", sdk::get_thread_context());
    auto rstick = sdk::call_object_func<REManagedObject*>(input_system, "get_RStick", sdk::get_thread_context());

    if (lstick == nullptr || rstick == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto button_bits_obj = sdk::call_object_func<REManagedObject*>(input_system, "get_ButtonBits", sdk::get_thread_context(), input_system);

    if (button_bits_obj == nullptr) {
        original_func(ctx, input_system);
        return;
    }

    auto left_axis = mod->get_left_stick_axis();
    auto right_axis = mod->get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    //button_bits_down |= mod->m_button_states_down.to_ullong();
    //button_bits_on |= mod-> m_button_states_on.to_ullong();
    //button_bits_up |= mod->m_button_states_up.to_ullong();

    auto keep_button_down = [&](app::ropeway::InputDefine::Kind button) {
        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0 && (mod->m_button_states_down.to_ullong() & (uint64_t)button) == 0) {
            return;
        }

        if ((mod->m_button_states_on.to_ullong() & (uint64_t)button) == 0) {
            if (mod->m_button_states_down.to_ullong() & (uint64_t)button) {
                button_bits_on |= (uint64_t)button;
                button_bits_down &= ~(uint64_t)button;
            } else {
                button_bits_down |= (uint64_t)button;
            }
        } else {
            button_bits_down &= ~(uint64_t)button;
            button_bits_on |= (uint64_t)button;
        }
    };

    const auto deadzone = mod->m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };
        sdk::call_object_func<void*>(lstick, "update", sdk::get_thread_context(), lstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_L_STICK);
    }

    if (right_axis_len > deadzone) {
        mod->m_last_controller_update = now;
        is_using_controller = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };
        sdk::call_object_func<void*>(rstick, "update", sdk::get_thread_context(), rstick, &axis, &axis);

        keep_button_down(app::ropeway::InputDefine::Kind::UI_R_STICK);
    }

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        sdk::call_object_func<void*>(input_system, "set_InputMode", sdk::get_thread_context(), input_system, app::ropeway::InputDefine::InputMode::Pad);
    }

    original_func(ctx, input_system);

    mod->openvr_input_to_re2_re3(input_system);
}

// [UI_TRENNUNG 28.09.2026] PostMainTarget des Auges in unsere Kopie (Engine-Copy,
// laeuft in der Reihenfolge des Engine-Frames). [28.09.2026 -- Ansage des Users]
// NUR Multipass + Upscaler: ohne Upscaler darf die Nachbearbeitung das UI mitnehmen.
void VR::ui_capture(void* render_context, sdk::renderer::RenderLayer* scene_layer, bool final_pass) {
    if (render_context == nullptr || scene_layer == nullptr || !is_hmd_active() || is_vr_suspended() || !is_using_multipass()) {
        return;
    }

    const auto& upscaler = TemporalUpscaler::get();

    if (upscaler == nullptr || !upscaler->ready()) {
        return;
    }

    const auto layers = m_camera_duplicator.get_relevant_scene_layers();
    int eye = -1;

    if (is_mono_periphery()) {
        // [MONO UI] the shared wide layer -> [2], the fovea layers -> [0] / [1]. Each of them has the interface drawn
        // with its own frustum (the GUI is built once per frame); TemporalUpscaler puts each eye's interface together
        // from the wide view's (outside the fovea) and the eye's fovea view's (inside), see d3d12::MonoUi.
        for (size_t i = 0; i < layers.size() && i < 3; ++i) {
            if ((sdk::renderer::RenderLayer*)layers[i] == scene_layer) {
                eye = i == 0 ? UI_CAPTURE_CENTRE : (int)i - 1;
            }
        }
    } else {
        for (size_t i = 0; i < layers.size() && i < 2; ++i) {
            if ((sdk::renderer::RenderLayer*)layers[i] == scene_layer) {
                eye = (int)i;
            }
        }
    }

    if (eye < 0) {
        return;
    }

    // [COPIES fix116 21.11.2026] GPU test (measurement only): the fovea views' two copies left out. The eye pass reads them
    // only in a frame in which the wide view drew interface (the backdrop a fovea view draws alone, [BACKDROP fix83]);
    // they are made in every frame, two per view on the engine's list.
    if (is_mono_periphery() && eye != UI_CAPTURE_CENTRE && quadview::State::get().test_ui_copies.load(std::memory_order_relaxed) >= 1) {
        return;
    }

    auto ts = scene_layer->get_target_state("PostMainTarget");

    if (ts == nullptr) {
        return;
    }

    auto rtv = ts->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    auto& tex = rtv->get_texture_d3d12();

    if (tex == nullptr) {
        return;
    }

    auto desc = tex->get_desc();

    if (desc == nullptr) {
        return;
    }

    auto& cap = m_ui_capture[eye];
    void* src_native = nullptr;

    if (auto container = tex->get_d3d12_resource_container(); container != nullptr) {
        src_native = (void*)container->get_native_resource();
    }

    {
        std::scoped_lock _{cap.mtx};
        auto& dst = final_pass ? cap.final_tex : cap.hudless;
        const uint64_t now = (uint64_t)m_frame_count;
        const uint32_t generation = m_ui_capture_generation.load();

        // [UI COPIES 30.10.2026] copies cloned before a device reset, from another engine texture or before a refresh are
        // made again: after start-up resets the copy without GUI of the wide view held the right fovea view's picture
        // (the whole periphery became that view) until the copies were made again
        // (another source at most once a second: an engine alternating its targets must not make copies every frame)
        const bool new_source = cap.made_from != nullptr && src_native != nullptr && cap.made_from != src_native && now >= cap.source_change_frame + 60;
        const bool stale = cap.generation != generation || new_source;

        if (new_source) {
            cap.source_change_frame = now;
            REVR_LOG(spdlog::info("[VR] interface copies of view {}: engine texture {} -> {}, made again", eye, cap.made_from, src_native));
        }

        if (stale || cap.width != desc->width || cap.height != desc->height) {
            {
                std::scoped_lock __{m_ui_capture_old_mtx};

                // work recorded in the last frames may still read the old copies
                for (auto* old : {&cap.hudless, &cap.final_tex}) {
                    if (*old != nullptr) {
                        m_ui_capture_old.emplace_back(*old, now);
                    }
                }
            }

            cap.hudless = nullptr;
            cap.final_tex = nullptr;
            cap.hudless_ok = false;
            cap.final_ok = false;
            cap.width = desc->width;
            cap.height = desc->height;
            cap.generation = generation;
            cap.made_from = src_native;
        }

        if (dst == nullptr) {
            dst = tex->clone();

            if (dst == nullptr) {
                return;
            }
        }

        ((sdk::renderer::RenderContext*)render_context)->copy_texture(dst.get(), tex.get());
    }

    {
        const uint64_t now = (uint64_t)m_frame_count;
        std::scoped_lock _{m_ui_capture_old_mtx};
        std::erase_if(m_ui_capture_old, [now](const auto& e) { return e.second + 8 < now; });
    }

    cap.scene_layer = (void*)scene_layer;

    // [UI CURVE 27.10.2026] which engine texture each copy was taken from (dump report: the fovea layers share one)
    if (src_native != nullptr) {
        (final_pass ? cap.final_src : cap.hudless_src) = src_native;
    }

    {
        // [UI_REIHENFOLGE] H = Kopie ohne UI, F = Kopie mit UI, dahinter das Auge
        std::scoped_lock _{m_ui_order_mtx};

        if (m_ui_order.size() < 32) {
            m_ui_order += final_pass ? 'F' : 'H';
            m_ui_order += (char)('0' + eye);
        }
    }

    // [PAARE 28.09.2026] Frame-Nummer mitschreiben (Engine-Frame dieser Aufzeichnung)
    const uint64_t f = (uint64_t)m_frame_count;

    if (final_pass) {
        cap.final_frame = f;
        cap.final_ok = true;

        if (eye < 2) {
            ++m_ui_cap_fin[eye];
        }
    } else {
        cap.hudless_frame = f;
        cap.hudless_ok = true;

        if (eye < 2) {
            ++m_ui_cap_hud[eye];
        }
    }
}

float VR::get_ui_distance() const {
    return m_ui_distance_option->value();
}

// [UI CURVE 27.10.2026] see VR.hpp
std::string VR::ui_capture_report() {
    std::string order{};

    {
        std::scoped_lock _{m_ui_order_mtx};
        order = !m_ui_order_last.empty() ? m_ui_order_last : m_ui_order;
    }

    static constexpr const char* NAMES[]{"L", "R", "wide"};
    std::string out = std::format("order of the copies in a frame \"{}\" (H = without GUI, F = with GUI; 0 = left fovea, 1 = right fovea, 2 = wide view)",
        order.empty() ? "-" : order);

    for (size_t i = 0; i < m_ui_capture.size(); ++i) {
        auto& cap = m_ui_capture[i];
        std::scoped_lock _{cap.mtx};
        out += std::format(" | {}: from {} / {} (copies cloned from {}, generation {})", NAMES[i], cap.hudless_src.load(), cap.final_src.load(),
            cap.made_from, cap.generation);
    }

    out += std::format(" | generation now {}, copies made again {} times", m_ui_capture_generation.load(), m_ui_capture_refreshes.load());
    return out;
}

// [UI COPIES 30.10.2026] see VR.hpp
void VR::refresh_ui_capture(const char* reason) {
    m_ui_capture_generation.fetch_add(1);
    m_ui_capture_refreshes.fetch_add(1);

    for (auto& cap : m_ui_capture) {
        cap.hudless_ok = false;
        cap.final_ok = false;
    }

    REVR_LOG(spdlog::info("[VR] interface copies made again at their next capture ({})", reason));
}

// [MONO UI 27.10.2026] see VR.hpp
bool VR::ui_capture_pair(int index, ID3D12Resource*& hudless, ID3D12Resource*& final_tex, void*& scene_layer) {
    hudless = nullptr;
    final_tex = nullptr;
    scene_layer = nullptr;

    if (index < 0 || index >= (int)m_ui_capture.size()) {
        return false;
    }

    auto& cap = m_ui_capture[index];
    std::scoped_lock _{cap.mtx};

    if (!cap.hudless_ok || !cap.final_ok || cap.hudless_frame.load() != cap.final_frame.load() || cap.hudless == nullptr || cap.final_tex == nullptr) {
        return false;
    }

    auto h = cap.hudless->get_d3d12_resource_container();
    auto f = cap.final_tex->get_d3d12_resource_container();

    if (h == nullptr || f == nullptr) {
        return false;
    }

    hudless = h->get_native_resource();
    final_tex = f->get_native_resource();
    scene_layer = cap.scene_layer.load();
    return hudless != nullptr && final_tex != nullptr;
}

ID3D12Resource* VR::ui_capture_resource(int eye, bool final_pass) {
    if (eye < 0 || eye > 1) {
        return nullptr;
    }

    auto& cap = m_ui_capture[eye];
    std::scoped_lock _{cap.mtx};
    auto& t = final_pass ? cap.final_tex : cap.hudless;

    if (t == nullptr) {
        return nullptr;
    }

    auto container = t->get_d3d12_resource_container();

    return container != nullptr ? container->get_native_resource() : nullptr;
}

// [QUADVIEW] true for the scene layer of a fovea camera while quad view runs
bool VR::is_quadview_inset_layer(void* scene_layer) const {
    if (scene_layer == nullptr || !is_quadview_requested()) {
        return false;
    }

    int32_t eye = -1;
    bool fovea = false;

    return view_of_camera(((sdk::renderer::layer::Scene*)scene_layer)->get_main_camera_if_possible(), eye, fovea) && fovea;
}

bool VR::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
    ui_capture(render_ctx, layer->get_parent(), false);   // [UI_TRENNUNG] Kopie OHNE UI

#if defined(RE4)
    // [AFW fix91 19.11.2026] the UI buffer was kept per eye for AFW's frame warp; only the probe below still looks at it
    const ComPtr<ID3D12Resource> ui_buffer_tex = layer->get_ui_buffer_tex_d3d12();

    if (g_ui_test_mode.load() == 3) {
        ui_test_clear(render_ctx, layer->get_parent(), "PostMainTarget");   // [UI_TEST] Stelle 3
    }

    // [UI_SONDE 28.09.2026] nur zaehlen
    ++g_ui_probe.overlay_pre_draw;
    if (ui_buffer_tex != nullptr) {
        ++g_ui_probe.ui_buffer_nonnull;
        ui_probe_note(g_ui_probe.ui_buffers, ui_buffer_tex.Get());
    }
    ui_probe_note(g_ui_probe.overlay_layers, layer);
    ui_probe_note(g_ui_probe.overlay_parents, layer->get_parent());
    if (auto ui_target = layer->get_ui_target(); ui_target != nullptr && ui_target->get_rtv(0) != nullptr) {
        ++g_ui_probe.ui_target_nonnull;   // UITarget = Ziel, in das die Overlay-Ebene das UI zeichnet
    }
#endif

    // just don't render anything at all.
    // overlays just seem to break stuff in VR.
    // is_vr_suspended(): flat mode wants the engine's own overlays back.
    if (!is_hmd_active() || is_vr_suspended()) {
        return true;
    }

#if defined(RE4)
    ++g_ui_probe.overlay_suppressed;   // [UI_SONDE] bis hier = VR aktiv, Ebene wird (vermutlich) unterdrueckt
#endif

    // NOT RE3
    // for some reason RE3 has weird issues with the overlay rendering
    // causing double vision
#if (TDB_VER < 70 and not defined(RE3)) or (TDB_VER >= 70 and (not defined(RE3) and not defined(RE2) and not defined(RE7) and not defined(RE4) and not defined(SF6)))
    if (m_allow_engine_overlays->value()) {
        return true;
    }
#endif

    return false;
}

void VR::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
#if defined(RE4)
    ++g_ui_probe.overlay_draw;   // [UI_SONDE 28.09.2026] nur zaehlen
#endif

    // [AFW fix91 19.11.2026] the depth and the motion vectors were copied here for AFW's frame warp (the copies existed
    // only outside Single Frame Multipass)
}

bool VR::on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_ctx) {
#if defined(RE4)
    ++g_ui_probe.overlay_pre_update;   // [UI_SONDE 28.09.2026] nur zaehlen
#endif
    return true;
}

bool VR::on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
#if defined(RE4)
    if (g_ui_test_mode.load() == 1) {
        ui_test_clear(render_ctx, layer->get_parent(), "HDRTarget");   // [UI_TEST] Stelle 1
    }
#endif
    // is_vr_suspended(): the distortion fix below is a VR-only correction.
    if (!is_hmd_active() || is_vr_suspended()) {
        return true;
    }

    auto scene_layer = layer->get_parent();
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return true;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = utility::re_component::find(camera, render_output_type);

    if (render_output_component == nullptr) {
        return true;
    }

    if (!m_disable_post_effect_fix) {
        // Set the distortion type back to flatscreen mode
        // this will fix various graphical bugs
        //sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None

        if (scene_layer != nullptr) {
            m_previous_distortion_type = sdk::call_object_func_easy<uint32_t>(scene_layer, "get_DistortionType");
            m_set_next_post_effect_distortion_type = true;
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", 0); // None
        }
    }

    return true;
}

// Kein is_vr_suspended()-Check: wird mitten im Frame suspendiert, muss der im
// Pre-Hook gemerkte Distortion-Type trotzdem zurueckgestellt werden. Der Restore
// haengt ohnehin an m_set_next_post_effect_distortion_type.
void VR::on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
#if defined(RE4)
    if (g_ui_test_mode.load() == 2) {
        ui_test_clear(render_ctx, layer->get_parent(), "PostMainTarget");   // [UI_TEST] Stelle 2
    }
#endif
    if (!is_hmd_active()) {
        return;
    }

    if (!m_disable_post_effect_fix && m_set_next_post_effect_distortion_type) {
        auto scene_layer = layer->get_parent();

        // Restore the distortion type back to VR mode
        // to fix TAA
        if (scene_layer != nullptr) {
            sdk::call_object_func_easy<void*>(scene_layer, "set_DistortionType", m_previous_distortion_type); // Left
        }

        //mod->fix_temporal_effects();
        m_set_next_post_effect_distortion_type = false;
    }
}

bool VR::on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_ctx) {
    return true;
}

bool VR::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    return true;
}

// [MONO PERIPHERY 27.10.2026] Switches the scene layers of unused clones off and the others back on. Returning false
// from the layer update/draw hooks turned out not to stop the engine from rendering a layer (test 27.10.2026: the
// skipped fovea view kept changing, its matrices did not); the layer's own Enable flag is what the renderer looks at.
// Runs on the game thread before the frame's rendering. A layer the engine destroyed is not touched: only the layers
// found under the output layer right now are.
void VR::apply_layer_skips() {
    if (!is_hmd_active() || !is_using_multipass()) {
        return;
    }

    const auto output_layer = sdk::renderer::get_output_layer();

    if (output_layer == nullptr) {
        return;
    }

    static auto scene_t = sdk::find_type_definition("via.render.layer.Scene");
    static auto scene_type = scene_t != nullptr ? scene_t->get_type() : nullptr;
    static auto set_enable = scene_t != nullptr ? scene_t->get_method("set_Enable") : nullptr;

    if (scene_type == nullptr || set_enable == nullptr) {
        return;
    }

    // [PROBE FIX 27.10.2026] the layers are matched by pointer with the ones the clones rendered into (recorded in
    // on_pre_scene_layer_update); only layers that still exist are touched. Switched-off layers are not updated any more,
    // so their camera cannot be read from them: the clone index comes from the record, the state from what we set.
    const auto layers = output_layer->find_layers(scene_type);
    uint32_t off = 0;

    for (size_t k = 0; k < m_clone_layers.size(); ++k) {
        auto* known = m_clone_layers[k];

        if (known == nullptr || std::find(layers.begin(), layers.end(), (sdk::renderer::RenderLayer*)known) == layers.end()) {
            m_clone_layers[k] = nullptr;
            m_clone_layer_off[k] = false;
            continue;
        }

        const auto camera = k + 1 < m_multipass_cameras.size() ? m_multipass_cameras[k + 1] : nullptr;

        if (camera == nullptr) {
            continue;
        }

        const bool skip = is_skipped_layer_camera(camera);

        if (skip) {
            ++off;
        }

        if (skip != m_clone_layer_off[k]) {
            set_enable->call<void*>(sdk::get_thread_context(), known, !skip);
            m_clone_layer_off[k] = skip;
            REVR_LOG(spdlog::info("[VR] scene layer of clone {} switched {}", k, skip ? "off" : "on"));
        }
    }

    m_layers_switched_off = off;
    quadview::State::get().skipped_layers += off;
}

// [MONO PERIPHERY 27.10.2026] clone k's scene layer is left out when the technique wants fewer clones than exist (one
// left over from another setting). The camera table is the one on_pre_scene_layer_update keeps.
bool VR::is_skipped_layer_camera(const RECamera* camera) const {
    if (camera == nullptr || !is_using_multipass()) {
        return false;
    }

    for (size_t i = 1; i < m_multipass_cameras.size(); ++i) {
        if (m_multipass_cameras[i] != nullptr && m_multipass_cameras[i] == camera) {
            return (uint32_t)(i - 1) >= wanted_camera_clones();
        }
    }

    return false;
}

// [UI_TEST 28.09.2026] Stelle 4: vor PrepareOutput. Sonst nichts.
bool VR::on_pre_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    ui_capture(render_context, layer->get_parent(), true);   // [UI_TRENNUNG] Kopie MIT UI
#if defined(RE4)
    if (g_ui_test_mode.load() == 4) {
        ui_test_clear(render_context, layer->get_parent(), "PostMainTarget");
    }
#endif
    return true;
}

void VR::on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
#if defined(RE4)
    if (g_ui_test_mode.load() == 5) {
        ui_test_clear_rtv(render_context, layer->get_output_state());   // [UI_TEST] Stelle 5
    }
#endif

    // is_vr_suspended(): this only re-points the output at the VR eye textures.
    if (!is_hmd_active() || is_vr_suspended()) {
        return;
    }

    if (!is_using_multipass()) {
        return;
    }

    // We dont need to do anything here if upscaling is being used
    if (TemporalUpscaler::get()->ready()) {
        return;
    }
    
    auto context = (sdk::renderer::RenderContext*)render_context;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    if (scene_layer == nullptr) {
        return;
    }

    const auto output_state = layer->get_output_state();

    if (output_state == nullptr) {
        return;
    }

    const auto rtv = output_state->get_rtv(0);

    if (rtv == nullptr) {
        return;
    }

    const auto tex = rtv->get_texture_d3d12();

    if (tex == nullptr) {
        return;
    }

    auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

    if (scene_layers.size() < 2) {
        return;
    }

    const auto parent_layer = layer->get_parent();

    if (parent_layer == nullptr) {
        return;
    }

    if (parent_layer == scene_layers[0]) {
        if (m_multipass.native_res_copies[0] != nullptr) {
            context->copy_texture(m_multipass.native_res_copies[0], tex);
        }
    } else {
        if (m_multipass.native_res_copies[1] != nullptr) {
            context->copy_texture(m_multipass.native_res_copies[1], tex);
        }
    }
}

// ----------------------------------------------------------------------------
// [LAYER TRACE 27.10.2026] A quad-view frame dump also lists every fully rendered scene layer of the dumped frames:
// the camera it renders (primary / clone k / other, with the name of its game object), the eye and view the mod
// assigned to it, the pose of its camera joint and of its transform, the camera pose the engine actually rendered
// with (taken from the layer's view matrix) and the frustum of its projection matrix, plus the headset pose of the
// frame. Only active for a few frames after a dump request; nothing is read otherwise.
// ----------------------------------------------------------------------------
namespace layer_trace {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the trace of a frame dump: debug build only
std::mutex g_mtx{};
int g_until{-1};
int g_block_frame{-1}; // frame of the current block of lines
std::string g_text{};
size_t g_lines{0};
constexpr size_t MAX_LINES = 600;

// render thread: true = trace this layer now
bool active(int render_frame) {
    auto& qv = quadview::State::get();
    std::scoped_lock _{g_mtx};

    if (qv.layer_trace_request.exchange(false)) {
        g_until = render_frame + qv.layer_trace_frames.load();
        g_block_frame = -1;
        g_text.clear();
        g_lines = 0;
    }

    if (g_until < 0) {
        return false;
    }

    if (render_frame > g_until) {
        if (!g_text.empty()) {
            qv.push_dump_report("  scene layers of the dumped frames (what each camera rendered):\n" + g_text);
        }

        g_text.clear();
        g_until = -1;
        return false;
    }

    return g_lines < MAX_LINES;
}

// true once per frame: the header of the frame goes before its first layer
bool new_frame(int render_frame) {
    std::scoped_lock _{g_mtx};

    if (g_block_frame == render_frame) {
        return false;
    }

    g_block_frame = render_frame;
    return true;
}

void add(const std::string& line) {
    std::scoped_lock _{g_mtx};

    if (g_lines < MAX_LINES) {
        g_text += line;
        ++g_lines;
    }
}
#endif

std::string vec3(const Vector4f& v) {
    return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z);
}

// yaw / pitch / roll (degrees) of a rotation, from its forward (+Z) and up axes
std::string ypr(const Matrix4x4f& m) {
    const float yaw = glm::degrees(std::atan2(m[2].x, m[2].z));
    const float pitch = glm::degrees(std::asin(std::clamp(-m[2].y, -1.0f, 1.0f)));
    const float roll = glm::degrees(std::atan2(m[0].y, m[1].y));
    return std::format("yaw {:.2f} pitch {:.2f} roll {:.2f}", yaw, pitch, roll);
}

std::string ypr(const glm::quat& q) {
    return ypr(Matrix4x4f{q});
}

// angular extents of a projection matrix (degrees), the smaller angle first, sign conventions aside
std::string frustum(const Matrix4x4f& p) {
    if (std::abs(p[0][0]) < 1e-6f || std::abs(p[1][1]) < 1e-6f) {
        return std::format("P00={:.4f} P11={:.4f} (not a perspective projection)", p[0][0], p[1][1]);
    }

    const float xa = glm::degrees(std::atan((p[2][0] - 1.0f) / p[0][0]));
    const float xb = glm::degrees(std::atan((p[2][0] + 1.0f) / p[0][0]));
    const float ya = glm::degrees(std::atan((p[2][1] - 1.0f) / p[1][1]));
    const float yb = glm::degrees(std::atan((p[2][1] + 1.0f) / p[1][1]));

    return std::format("x {:.1f}..{:.1f} deg, y {:.1f}..{:.1f} deg (P00={:.4f} P11={:.4f} P20={:.4f} P21={:.4f})",
        (std::min)(xa, xb), (std::max)(xa, xb), (std::min)(ya, yb), (std::max)(ya, yb), p[0][0], p[1][1], p[2][0], p[2][1]);
}
} // namespace layer_trace

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// the header of a traced frame: headset pose, the pose the mod gives the cameras, the camera table
std::string VR::layer_trace_frame_header(int render_frame) {
    std::string o = std::format("  frame {} (game frame {}):", render_frame, m_frame_count);

    if (is_hmd_active()) {
        const auto hmd = get_transform(0);
        o += std::format(" HMD pos {} {} | standing origin {} | rotation offset {}", layer_trace::vec3(hmd[3]), layer_trace::ypr(hmd),
            layer_trace::vec3(m_standing_origin), layer_trace::ypr(Matrix4x4f{get_rotation_offset()}));

        const auto eye_l = get_eye_transform(0);
        const auto eye_r = get_eye_transform(1);
        o += std::format(" | eye offsets L {} R {}", layer_trace::vec3(eye_l[3]), layer_trace::vec3(eye_r[3]));
    } else {
        o += " no headset";
    }

    o += std::format("\n    game camera pose kept by the mod: pos {} {} | positional tracking {} | technique {} | stereo fovea {} | mono {}\n",
        layer_trace::vec3(m_original_camera_position), layer_trace::ypr(m_original_camera_rotation), m_positional_tracking,
        is_using_multipass() ? "multipass" : is_using_afr() ? "AFR" : "other", is_stereo_fovea(), m_mono_rendering);

    o += "    camera table:";

    for (size_t i = 0; i < m_multipass_cameras.size(); ++i) {
        const auto camera = m_multipass_cameras[i];
        std::string name = "-";

        if (camera != nullptr) {
            const auto go = utility::re_component::get_game_object((REComponent*)camera);
            name = (go != nullptr && go->name != nullptr) ? utility::narrow(utility::re_string::get_view(go->name)) : std::string{"?"};
        }

        o += std::format(" [{}] {}=0x{:x} \"{}\"", i, i == 0 ? std::string{"primary"} : std::format("clone {}", i - 1), (uintptr_t)camera, name);
    }

    const auto cameras = get_cameras();
    size_t posed = 0;

    for (auto camera : cameras) {
        if (camera != nullptr) {
            ++posed;
        }
    }

    o += std::format(" | cameras given the headset pose: {} | clones wanted {} made {}\n", posed, wanted_camera_clones(),
        m_multipass_cameras[0] != nullptr ? m_camera_duplicator.get_clone_count(m_multipass_cameras[0]) : 0);

    // [MONO PERIPHERY] which pass the GUI's projection requests used since the last traced frame (0 left, 1 right,
    // 2 centre): the GUI of a layer must use that layer's eye
    o += std::format("    GUI projection requests since the last traced frame: pass 0 (left) {}, pass 1 (right) {}, pass 2 (centre) {}",
        m_gui_proj_calls[0].exchange(0), m_gui_proj_calls[1].exchange(0), m_gui_proj_calls[2].exchange(0));

    if (is_mono_periphery()) {
        const auto f = quadview::State::get().frusta();
        o += std::format(" | frusta (tan): eye L x {:.3f}..{:.3f} y {:.3f}..{:.3f}, eye R x {:.3f}..{:.3f}, wide x {:.3f}..{:.3f} y {:.3f}..{:.3f} | eye offsets x L {:+.4f} R {:+.4f} m",
            f.eye_l[0], f.eye_r[0], f.eye_b[0], f.eye_t[0], f.eye_l[1], f.eye_r[1], f.wide_l, f.wide_r, f.wide_b, f.wide_t, f.eye_x[0], f.eye_x[1]);
    }

    o += "\n";
    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// one line per fully rendered scene layer
std::string VR::layer_trace_layer_line(sdk::renderer::layer::Scene* layer) {
    const auto camera = layer->get_camera();
    const auto main_camera = layer->get_main_camera_if_possible();
    std::string role = "other";
    std::string view = "-";
    std::string name = "?";
    std::string joint_pose = "no joint";
    std::string transform_pose = "no transform";

    if (camera != nullptr) {
        for (size_t i = 0; i < m_multipass_cameras.size(); ++i) {
            if (m_multipass_cameras[i] != nullptr && m_multipass_cameras[i] == camera) {
                role = i == 0 ? std::string{"primary"} : std::format("clone {}", i - 1);
                break;
            }
        }

        int32_t eye = -1;
        bool fovea = false;

        if (view_of_camera(camera, eye, fovea)) {
            view = std::format("{} {}", eye == CENTER_EYE ? "BOTH EYES (centre)" : eye == 0 ? "LEFT" : "RIGHT", fovea ? "fovea" : "wide");
        }

        if (is_skipped_layer_camera(camera)) {
            view += " (layer being switched off)";
        }

        const auto go = utility::re_component::get_game_object((REComponent*)camera);

        if (go != nullptr) {
            if (go->name != nullptr) {
                name = utility::narrow(utility::re_string::get_view(go->name));
            }

            if (go->transform != nullptr) {
                transform_pose = std::format("pos {}", layer_trace::vec3(sdk::get_transform_position(go->transform)));

                if (const auto joint = utility::re_transform::get_joint(*go->transform, 0); joint != nullptr) {
                    joint_pose = std::format("pos {} {}", layer_trace::vec3(sdk::get_joint_position(joint)), layer_trace::ypr(sdk::get_joint_rotation(joint)));
                }
            }
        }
    }

    std::string matrices = "no scene info";

    if (const auto scene_info = layer->get_scene_info(); scene_info != nullptr) {
        const auto camera_matrix = glm::inverse(scene_info->view_matrix);
        matrices = std::format("rendered from pos {} {} | frustum {}", layer_trace::vec3(camera_matrix[3]), layer_trace::ypr(camera_matrix),
            layer_trace::frustum(scene_info->projection_matrix));
    }

    return std::format("    layer 0x{:x} view_id={} camera=0x{:x} {} \"{}\" main camera {} | assigned {} (pass {}) | joint {} | transform {} | {}\n",
        (uintptr_t)layer, layer->get_view_id(), (uintptr_t)camera, role, name, main_camera == camera ? "yes" : "no", view, m_multipass.pass,
        joint_pose, transform_pose, matrices);
}
#endif

bool VR::on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    REF_PROFILE_FUNCTION();

#if defined(RE4)
    // [UI_SONDE 28.09.2026] nur zaehlen: Scene-Layer pro Frame
    ++g_ui_probe.scene_pre_update;
    ui_probe_note(g_ui_probe.scene_layers, layer);
#endif

    m_scene_update_mtx.lock();

    t_quadview_inset_scope = false; // [QUADVIEW]
    t_layer_eye = -1;               // [STEREO FOVEA]
    t_in_layer_update = true;       // [VIEW RACE] until on_scene_layer_update
    t_whole_update = false;         // [LIGHTS fix82]
    s_layer_updates_running.fetch_add(1, std::memory_order_relaxed);

    if (!is_hmd_active()) {
        return true;
    }

    if (!layer->is_fully_rendered()) {
        return true;
    }

    // [STEREO FOVEA] multipass: primary camera and its clones (see view_of_camera). The table of cameras is refreshed from
    // the primary camera's layer (the first one of the frame).
    const auto real_main_camera = sdk::get_primary_camera();
    const auto layer_camera = layer->get_main_camera_if_possible();

    if (is_using_multipass()) {
        if (layer_camera != nullptr && layer_camera == real_main_camera) {
            m_multipass_cameras[0] = real_main_camera;

            for (size_t i = 1; i < m_multipass_cameras.size(); ++i) {
                m_multipass_cameras[i] = m_camera_duplicator.get_clone(real_main_camera, i - 1);
            }
        }
    }

    // [PROBE FIX 27.10.2026] the scene layer each clone renders into, for apply_layer_skips (which runs before the layers
    // and found no camera on them: the probe of 27.10.2026 left nothing out)
    if (is_using_multipass() && layer_camera != nullptr) {
        for (size_t i = 1; i < m_multipass_cameras.size() && i - 1 < m_clone_layers.size(); ++i) {
            if (m_multipass_cameras[i] == layer_camera) {
                m_clone_layers[i - 1] = layer;
            }
        }
    }

    // [MONO PERIPHERY] clones the technique does not use (left over from another setting) are switched off by
    // apply_layer_skips (their layer's Enable flag); until that takes effect their layer is updated like any other, but
    // it is nothing of ours
    if (is_skipped_layer_camera(layer_camera)) {
        return true;
    }

    sdk::renderer::layer::Scene* wide_layer = nullptr; // [LIGHTS fix85] the shared wide view's layer, if this is it

    if (is_quadview_requested()) {
        int32_t eye = -1;
        bool fovea = false;

        if (layer_camera != nullptr && view_of_camera(layer_camera, eye, fovea)) {
            t_layer_eye = eye;

            if (fovea) {
                t_quadview_inset_scope = true;
                ++quadview::State::get().inset_layers;
            } else {
                ++quadview::State::get().wide_layers;
            }

            // [LIGHTS fix84 18.11.2026] which view's layer the frame's first update was
            {
                static int s_first_frame = -1;

                if (s_first_frame != m_render_frame_count) {
                    s_first_frame = m_render_frame_count;
                    auto& qs = quadview::State::get();
                    (fovea ? qs.first_fovea : qs.first_wide).fetch_add(1, std::memory_order_relaxed);
                }
            }

            // (the main camera's: with the shared periphery the one wide view)
            if (!fovea && layer_camera == real_main_camera) {
                wide_layer = layer;
            }
        }
    }

    // [LIGHTS fix85 18.11.2026] The shared wide view's layer before the fovea views' (quadview::Settings::wide_first; the
    // light flicker test's phases switch it): its priority is put below the lowest of the clones' layers, which are a
    // step of 128 apart, and the engine draws by priority (fix84's tests: the frame's first view update was the wide
    // view's from the next frame on). Its own priority again when the setting goes off or the quad view is not asked for.
    // The layer is only touched while it is the one being updated; one made anew has the engine's priority and is moved
    // again. (m_clone_layers holds layers that exist: apply_layer_skips clears the others before the layers' updates.)
    {
        static sdk::renderer::layer::Scene* s_moved_layer = nullptr;
        static uint32_t s_moved_from = 0;
        static uint32_t s_moved_to = 0;

        if (wide_layer != nullptr && quadview::State::get().wide_first_now()) {
            uint32_t lowest = 0xffffffffu;

            for (auto* cl : m_clone_layers) {
                if (cl != nullptr && cl != wide_layer) {
                    lowest = (std::min)(lowest, cl->m_priority);
                }
            }

            if (lowest != 0xffffffffu && lowest >= 256u && wide_layer->m_priority >= lowest) {
                s_moved_layer = wide_layer;
                s_moved_from = wide_layer->m_priority;
                s_moved_to = lowest - 128u;
                wide_layer->m_priority = s_moved_to;
                REVR_LOG(spdlog::info("[VR] the wide view's layer first: its priority {} -> {} (the fovea views' lowest {})", s_moved_from, s_moved_to, lowest));
            }
        } else if (s_moved_layer != nullptr && s_moved_layer == layer && layer_camera != nullptr && layer_camera == real_main_camera) {
            // (the layer of the main camera: the wide view's while the quad view runs, the game's own view without it)
            if (layer->m_priority == s_moved_to) {
                layer->m_priority = s_moved_from;
                REVR_LOG(spdlog::info("[VR] the wide view's layer first: off, its priority back to {}", s_moved_from));
            }

            s_moved_layer = nullptr;
        }
    }

    if (is_using_multipass()) {
        int32_t eye = -1;
        bool fovea = false;

        if (layer_camera != nullptr && view_of_camera(layer_camera, eye, fovea)) {
            m_multipass.pass = (uint32_t)eye; // the view matrix hook has no camera to go by: the pass says the eye
            t_layer_eye = eye;
        } else {
            return true; // dont care
        }
    }
    
    auto scene_info = layer->get_scene_info();
    auto depth_distortion_scene_info = layer->get_depth_distortion_scene_info();
    auto filter_scene_info = layer->get_filter_scene_info();
    auto jitter_disable_scene_info = layer->get_jitter_disable_scene_info();
    auto z_prepass_scene_info = layer->get_z_prepass_scene_info();

    auto& layer_data = m_scene_layer_data[layer];

    layer_data[0].setup(scene_info);
    layer_data[1].setup(depth_distortion_scene_info);
    layer_data[2].setup(filter_scene_info);
    layer_data[3].setup(jitter_disable_scene_info);
    layer_data[4].setup(z_prepass_scene_info);

    return true;
}

void VR::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_ctx) {
    REF_PROFILE_FUNCTION();

    utility::ScopeGuard ___([&]() {
        m_scene_update_mtx.unlock();
    });

    t_quadview_inset_scope = false; // [QUADVIEW] the inset frustum only applies inside its own layer update
    t_layer_eye = -1;               // [STEREO FOVEA]
    t_in_layer_update = false;      // [VIEW RACE 03.11.2026]
    const bool whole_update = t_whole_update; // [LIGHTS fix82] this update's request got the eye's whole frustum
    t_whole_update = false;
    s_layer_updates_running.fetch_sub(1, std::memory_order_relaxed);

    if (!is_hmd_active()) {
        return;
    }

    if (!layer->is_fully_rendered()) {
        return;
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    // [LAYER TRACE 27.10.2026] every fully rendered layer of the dumped frames, with the matrices the engine now holds
    {
        const int trace_frame = (int)m_frame_count;

        if (layer_trace::active(trace_frame)) {
            if (layer_trace::new_frame(trace_frame)) {
                layer_trace::add(layer_trace_frame_header(trace_frame));
            }

            layer_trace::add(layer_trace_layer_line(layer));
        }
    }
#endif

    if (is_using_multipass()) {
        const auto layer_camera = layer->get_camera();

        if (!is_multipass_camera(layer_camera) || is_skipped_layer_camera(layer_camera)) {
            return;
        }

        // [MONO UI] world -> clip of the shared wide view: the upscaler projects the GUI anchors with it
        if (is_mono_periphery()) {
            int32_t eye = -1;
            bool fovea = false;

            if (view_of_camera(layer_camera, eye, fovea) && eye == CENTER_EYE) {
                if (const auto si = layer->get_scene_info(); si != nullptr) {
                    // [FRAME LATCH 04.11.2026] kept per frame: the upscaler composites with the frame's own matrix
                    quadview::State::get().set_centre_view_proj(&si->view_projection_matrix[0][0], (int64_t)(uint32_t)m_frame_count);
                }
            }
        }

        // [PROJECTION CALLERS 15.11.2026] a fovea view's image matrices after its update: the fovea's frustum (narrow: the x
        // scale well above the eye's) or the eye's whole one; the light flicker test tells by it whether the fovea image
        // stayed right in a phase
        if (is_quadview_requested()) {
            int32_t eye = -1;
            bool fovea = false;

            if (view_of_camera(layer_camera, eye, fovea) && fovea && eye >= 0 && eye <= 1) {
                // [LIGHTS fix82 17.11.2026] the engine worked this update with the eye's whole frustum: the image's matrices
                // get the fovea's part (before they are looked at below, and before the upscaler's jitter)
                if (whole_update) {
                    narrow_fovea_layer(layer, eye, quadview::State::get().whole_infos_now());
                }

                if (const auto si = layer->get_scene_info(); si != nullptr) {
                    const float whole = std::abs(get_projection_matrix((uint32_t)eye)[0][0]);
                    const float got = std::abs(si->projection_matrix[0][0]);
                    auto& qs = quadview::State::get();
                    (whole > 0.0f && got > 1.2f * whole ? qs.fovea_matrix_narrow : qs.fovea_matrix_whole).fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    auto& layer_data = m_scene_layer_data[layer];

    const auto is_temporal_upscaler_active = TemporalUpscaler::get()->ready();
    const auto is_multipass = is_using_multipass();

    /*uint32_t pass = 0;

    if (is_multipass) {
        auto output_layer = sdk::renderer::get_output_layer();

        if (output_layer != nullptr) {
            static auto t = sdk::find_type_definition("via.render.layer.Scene")->get_type();
            const auto scenes = output_layer->find_layers(t);

            if (!scenes.empty()) {
                if (layer == scenes[0]) {
                    pass = 0;
                } else {
                    pass = 1;
                }
            }
        }
    }

    auto proj_mult = glm::identity<Matrix4x4f>();
    proj_mult[2].z = -1.0f;
    proj_mult[3].z = 1.0f;

    const auto projection_matrix = proj_mult * get_projection_matrix(pass);
    const auto current_eye_transform = get_eye_transform(pass + 1);*/

    for (auto& d : layer_data) {
        if (d.scene_info != nullptr) {
            /*if (is_multipass) {
                d.scene_info->view_matrix = current_eye_transform * d.scene_info->view_matrix;
                d.scene_info->projection_matrix = projection_matrix;
                d.scene_info->view_projection_matrix = d.scene_info->projection_matrix * d.scene_info->view_matrix;
                d.scene_info->inverse_view_matrix = glm::inverse(d.scene_info->view_matrix);
                d.scene_info->inverse_projection_matrix = glm::inverse(d.scene_info->projection_matrix);
                d.scene_info->inverse_view_projection_matrix = glm::inverse(d.scene_info->view_projection_matrix);
            }*/

            if (is_multipass && !is_temporal_upscaler_active) {
                const auto frame = this->get_game_frame_count();
                d.post_setup(frame);
            } else if (!is_temporal_upscaler_active) {
                // TAA fix
                d.scene_info->old_view_projection_matrix = d.view_projection_matrix;
            }
        }
    }
}

void VR::wwise_listener_update_hook(void* listener) {
    auto original_func = g_wwise_listener_update_hook->get_original<decltype(VR::wwise_listener_update_hook)>();

    if (!g_framework->is_ready()) {
        original_func(listener);
        return;
    }

    auto& mod = VR::get();

    if (!mod->is_hmd_active() || !mod->get_runtime()->loaded) {
        original_func(listener);
        return;
    }

    if (!mod->m_hmd_oriented_audio->value()) {
        original_func(listener);
        return;
    }

    std::scoped_lock _{mod->m_wwise_mtx};

#if defined(RE2) || defined(RE3)
    const auto skip_camera_set = FirstPerson::get()->will_be_used();
#else
    const auto skip_camera_set = false;
#endif

    if (!skip_camera_set) {
        mod->update_audio_camera();
    }

#if TDB_VER > 49
    constexpr auto CAMERA_OFFSET = 0x50;
#else
    constexpr auto CAMERA_OFFSET = 0x58;
#endif

    auto& listener_camera = *(::REManagedObject**)((uintptr_t)listener + CAMERA_OFFSET);
    bool changed = false;

    if (listener_camera == nullptr) {
        auto primary_camera = sdk::get_primary_camera();

        if (primary_camera != nullptr) {
            listener_camera = primary_camera;
            changed = true;
        }
    }

    original_func(listener);

    if (changed) {
        listener_camera = nullptr;
    }

    if (!skip_camera_set) {
        mod->restore_audio_camera();
    }
}

// put it on the backburner
/*
float VR::get_sharpness_hook(void* tonemapping) {
    auto original_func = g_get_sharpness_hook->get_original<decltype(get_sharpness_hook)>();
    
    if (!g_framework->is_ready()) {
        return original_func(tonemapping);
    }

    auto& mod = VR::get();

    if (mod->m_disable_sharpening) {
        return 0.0f;
    }

    return original_func(tonemapping);
}
*/

// Called when the mod is initialized
std::optional<std::string> VR::on_initialize_d3d_thread() try {

    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            REVR_LOG(spdlog::info("OpenVR failed to load: {}", *m_openvr->error));
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = 
R"(OpenVR loaded first.
If you want to use OpenXR, remove the openvr_api.dll from your game folder, 
and place the openxr_loader.dll in the same folder.)";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    auto hijack_error = hijack_resolution();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_input();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_camera();

    if (hijack_error) {
        return hijack_error;
    }

    hijack_error = hijack_wwise_listeners();

    if (hijack_error) {
        return hijack_error;
    }

    const auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    if (renderer_t != nullptr) {
        const auto set_hdr_method = renderer_t->get_method("set_HDRMode");

        if (set_hdr_method != nullptr) {
            REVR_LOG(spdlog::info("Hooking setHDRMode"));
            g_hookman.add(set_hdr_method, &pre_set_hdr_mode, &post_set_hdr_mode);
        }
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    REVR_LOG(spdlog::error("Exception occurred in VR::on_initialize()"));

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

void VR::on_lua_state_created(sol::state& lua) {
    lua.new_usertype<VR>("VR",
        "get_controllers", &VR::get_controllers,
        "get_position", &VR::get_position,
        "get_velocity", &VR::get_velocity,
        "get_angular_velocity", &VR::get_angular_velocity,
        "get_rotation", &VR::get_rotation,
        "get_transform", &VR::get_transform,
        "get_left_stick_axis", &VR::get_left_stick_axis,
        "get_right_stick_axis", &VR::get_right_stick_axis,
        "get_current_eye_transform", &VR::get_current_eye_transform,
        "get_current_projection_matrix", &VR::get_current_projection_matrix,
        "get_standing_origin", &VR::get_standing_origin,
        "set_standing_origin", &VR::set_standing_origin,
        "get_rotation_offset", &VR::get_rotation_offset,
        "set_rotation_offset", &VR::set_rotation_offset,
        "recenter_view", &VR::recenter_view,
        "get_gui_rotation_offset", &VR::get_gui_rotation_offset,
        "set_gui_rotation_offset", &VR::set_gui_rotation_offset,
        "recenter_gui", &VR::recenter_gui,
        "get_action_set", &VR::get_action_set,
        "get_active_action_set", &VR::get_active_action_set,
        "get_action_trigger", &VR::get_action_trigger,
        "get_action_grip", &VR::get_action_grip,
        "get_action_joystick", &VR::get_action_joystick,
        "get_action_joystick_click", &VR::get_action_joystick_click,
        "get_action_a_button", &VR::get_action_a_button,
        "get_action_b_button", &VR::get_action_b_button,
        "get_action_weapon_dial", &VR::get_action_weapon_dial,
        "get_action_minimap", &VR::get_action_minimap,
        "get_action_block", &VR::get_action_block,
        "get_action_dpad_up", &VR::get_action_dpad_up,
        "get_action_dpad_down", &VR::get_action_dpad_down,
        "get_action_dpad_left", &VR::get_action_dpad_left,
        "get_action_dpad_right", &VR::get_action_dpad_right,
        "get_action_heal", &VR::get_action_heal,
        "get_left_joystick", &VR::get_left_joystick,
        "get_right_joystick", &VR::get_right_joystick,
        "is_using_controllers", &VR::is_using_controllers,
        "is_openvr_loaded", &VR::is_openvr_loaded,
        "is_openxr_loaded", &VR::is_openxr_loaded,
        "is_hmd_active", &VR::is_hmd_active,
        "is_action_active", &VR::is_action_active,
        // [GRIP_FORCE] Grip threshold is configured from Lua, not from a framework UI.
        "get_grip_analog", &VR::get_grip_analog,
        "get_grip_source", &VR::get_grip_source,
        "set_grip_settings", &VR::set_grip_settings,
        "is_using_hmd_oriented_audio", &VR::is_using_hmd_oriented_audio,
        "toggle_hmd_oriented_audio", &VR::toggle_hmd_oriented_audio,
        // [RELOADED MENU 15.11.2026] RE7VR Reloaded's settings panel ("Camera and Audio") sets these two directly
        "set_hmd_oriented_audio", [](VR* vr, bool v) { vr->m_hmd_oriented_audio->value() = v; },
        "set_decoupled_pitch", [](VR* vr, bool v) { vr->m_decoupled_pitch->value() = v; },
        "is_decoupled_pitch", [](VR* vr) { return vr->m_decoupled_pitch->value(); },
        "apply_hmd_transform", [](VR* vr, glm::quat& rotation, Vector4f& position) {
            vr->apply_hmd_transform(rotation, position);
        },
        "trigger_haptic_vibration", &VR::trigger_haptic_vibration,
        "get_last_render_matrix", &VR::get_last_render_matrix,
        "should_handle_pause", [](VR* vr) { 
            return vr->get_runtime()->handle_pause;
        },
        "set_handle_pause", [](VR* vr, bool state) { 
            return vr->get_runtime()->handle_pause = state;
        },
        "unhide_crosshair", &VR::unhide_crosshair,
        "is_gui_projection_matrix_override_disabled", &VR::is_gui_projection_matrix_override_disabled,
        "set_gui_projection_matrix_override_disabled", &VR::set_gui_projection_matrix_override_disabled,
        "is_gui_element_override_disabled", &VR::is_gui_element_override_disabled,
        "set_gui_element_override_disabled", &VR::set_gui_element_override_disabled,
        // [LUA 2026-08-15] Neigung des Overlay-Zeigestrahls (Grad, negativ = nach unten).
        // Damit laesst sich der Wert aus einem Script setzen -- der ImGui-Slider bleibt
        // bestehen, beide schreiben dieselbe Variable.
        "get_overlay_pointer_pitch", &VR::get_overlay_pointer_pitch,
        "set_overlay_pointer_pitch", &VR::set_overlay_pointer_pitch,
        "is_mono_rendering", &VR::is_mono_rendering,
        "set_mono_rendering", &VR::set_mono_rendering,
        "get_mono_rendering_eye", &VR::get_mono_rendering_eye,
        "set_mono_rendering_eye", &VR::set_mono_rendering_eye,
        "is_mono_projection", &VR::is_mono_projection,
        "set_mono_projection", &VR::set_mono_projection,
        "is_vr_suspended", &VR::is_vr_suspended,
        "set_vr_suspended", &VR::set_vr_suspended,
        "is_map_face_glue", &VR::is_map_face_glue,
        "set_map_face_glue", &VR::set_map_face_glue,
        "get_map_glue_distance", &VR::get_map_glue_distance,
        "set_map_glue_distance", &VR::set_map_glue_distance,
        "get_map_glue_layer_gap", &VR::get_map_glue_layer_gap,
        "set_map_glue_layer_gap", &VR::set_map_glue_layer_gap,
        // Die Pin-Liste aus Lua: Name wie im Spiel ("Gui_ui3104"), order = Reihenfolge
        // (0 = hinten). Gehasht wird hier, damit die Scripte nichts davon wissen muessen.
        "add_glue_gui", [](VR* vr, const char* name, int order) {
            if (name == nullptr) { return; }
            vr->set_glue_gui_hash(utility::hash(std::string{name}), order);
        },
        "remove_glue_gui", [](VR* vr, const char* name) {
            if (name == nullptr) { return; }
            vr->remove_glue_gui_hash(utility::hash(std::string{name}));
        },
        "clear_glue_guis", &VR::clear_glue_guis,
        "reset_glue_guis", &VR::reset_glue_guis,
        "get_glue_gui_count", &VR::get_glue_gui_count,
        "is_flatscreen_overlay", &VR::is_flatscreen_overlay,
        "set_flatscreen_overlay", &VR::set_flatscreen_overlay,
        "get_flatscreen_overlay_width", &VR::get_flatscreen_overlay_width,
        "set_flatscreen_overlay_width", &VR::set_flatscreen_overlay_width,
        "get_flatscreen_overlay_distance", &VR::get_flatscreen_overlay_distance,
        "set_flatscreen_overlay_distance", &VR::set_flatscreen_overlay_distance,
        "is_projection_matrix_override_disabled", &VR::is_projection_matrix_override_disabled,
        "set_projection_matrix_override_disabled", &VR::set_projection_matrix_override_disabled,
        "is_view_matrix_override_disabled", &VR::is_view_matrix_override_disabled,
        "set_view_matrix_override_disabled", &VR::set_view_matrix_override_disabled,
        "get_image_shift_x", &VR::get_image_shift_x,
        "set_image_shift_x", &VR::set_image_shift_x,
        "get_image_shift_y", &VR::get_image_shift_y,
        "set_image_shift_y", &VR::set_image_shift_y,
        "is_using_multipass", &VR::is_using_multipass,
        "is_pose_freeze", &VR::is_pose_freeze,
        "set_pose_freeze", &VR::set_pose_freeze,
        "get_pose_freeze_submit", &VR::get_pose_freeze_submit,
        "set_pose_freeze_submit", &VR::set_pose_freeze_submit,
        "get_blank_eye", &VR::get_blank_eye,
        "set_blank_eye", &VR::set_blank_eye,
        "get_projection_zoom", &VR::get_projection_zoom,
        "set_projection_zoom", &VR::set_projection_zoom,
        "get_projection_fov", &VR::get_projection_fov,
        "set_projection_fov", &VR::set_projection_fov,
        "get_projection_shift_x", &VR::get_projection_shift_x,
        "set_projection_shift_x", &VR::set_projection_shift_x,
        "get_projection_shift_y", &VR::get_projection_shift_y,
        "set_projection_shift_y", &VR::set_projection_shift_y,
        "is_zoom_scaling_eye_offset", &VR::is_zoom_scaling_eye_offset,
        "set_zoom_scales_eye_offset", &VR::set_zoom_scales_eye_offset
    );

    lua["vrmod"] = this;
}

std::optional<std::string> VR::initialize_openvr() {
    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;
    apply_sync_mode(); // [SYNC_MODE_CFG]

    if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
        REVR_LOG(spdlog::info("[VR] Could not load openvr_api.dll"));

        m_openvr->dll_missing = true;
        m_openvr->error = "Could not load openvr_api.dll";
        return Mod::on_initialize();
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    // [AUTO_RECENTER 2026-08-19] Rausgenommen: kein automatisches Recenter mehr nach der
    // Initialisierung. Die Mechanik (arm_auto_recenter + der Block in update_hmd_state)
    // bleibt stehen, sie wird nur nirgends mehr scharfgeschaltet.

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    const auto module_directory = REFramework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        REVR_LOG(spdlog::info("Writing default binding file {}", it.first));

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        // [GRIP_THRESHOLD] GripValue is an OpenXR-only convenience action and has no binding in the
        // shipped OpenVR profiles. A missing handle must never abort VR init here: under OpenVR the
        // boolean Grip already carries the 0.3/0.25 hysteresis from the profile itself.
        const bool optional_action = (it.first == "/actions/default/in/GripValue")
                                  || (it.first == "/actions/default/in/GripForce")
                                  // [TRACKPAD] nur Index/Vive haben eines
                                  || (it.first == "/actions/default/in/TouchpadClick")
                                  || (it.first == "/actions/default/in/Touchpad")
                                  || (it.first == "/actions/default/in/TouchpadForce");

        if (error != vr::VRInputError_None) {
            if (optional_action) {
                REVR_LOG(spdlog::info("[VR] Optional action not available: {} ({})", it.first, (uint32_t)error));
                continue;
            }

            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            if (optional_action) {
                REVR_LOG(spdlog::info("[VR] Optional action has no handle: {}", it.first));
                continue;
            }

            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    m_openxr = std::make_shared<runtimes::OpenXR>();
    apply_sync_mode(); // [SYNC_MODE_CFG]

    REVR_LOG(spdlog::info("[VR] Initializing OpenXR"));

    if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
        REVR_LOG(spdlog::info("[VR] Could not load openxr_loader.dll"));

        m_openxr->loaded = false;
        m_openxr->error = "Could not load openxr_loader.dll";

        return std::nullopt;
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    REVR_LOG(spdlog::info("[VR] Creating OpenXR instance"));

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        // [STEAM_FRAME 2026-09-28] Das Frame-Controller-Profil
        // (/interaction_profiles/valve/frame_controller_valve) gibt es nur mit
        // dieser Extension. Nur anfordern, wenn die Runtime sie anbietet --
        // eine unbekannte Extension liesse xrCreateInstance scheitern.
        {
            uint32_t ext_count = 0;

            if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr) == XR_SUCCESS && ext_count > 0) {
                std::vector<XrExtensionProperties> props(ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});

                if (xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props.data()) == XR_SUCCESS) {
                    for (const auto& p : props) {
                        if (std::string_view{p.extensionName} == "XR_VALVE_frame_controller_interaction") {
                            extensions.push_back("XR_VALVE_frame_controller_interaction");
                            break;
                        }
                    }
                }
            }
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        strcpy(instance_create_info.applicationInfo.applicationName, g_framework->get_game_name());
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

            return std::nullopt;
        }
    } else {
        REVR_LOG(spdlog::info("[VR] Found existing openxr instance"));
    }
    
    // Step 2: Create a system
    REVR_LOG(spdlog::info("[VR] Creating OpenXR system"));

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

            return std::nullopt;
        }
    } else {
        REVR_LOG(spdlog::info("[VR] Found existing openxr system"));
    }

    // Step 3: Create a session
    REVR_LOG(spdlog::info("[VR] Initializing graphics info"));

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    REVR_LOG(spdlog::info("[VR] Creating OpenXR session"));
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

        return std::nullopt;
    }

    // Step 4: Create a space
    REVR_LOG(spdlog::info("[VR] Creating OpenXR space"));

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    REVR_LOG(spdlog::info("[VR] Getting OpenXR system properties"));

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

        return std::nullopt;
    }

    REVR_LOG(spdlog::info("[VR] OpenXR system Name: {}", system_properties.systemName));
    REVR_LOG(spdlog::info("[VR] OpenXR system Vendor: {}", system_properties.vendorId));
    REVR_LOG(spdlog::info("[VR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth));
    REVR_LOG(spdlog::info("[VR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight));
    REVR_LOG(spdlog::info("[VR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount));
    REVR_LOG(spdlog::info("[VR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking));
    REVR_LOG(spdlog::info("[VR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking));

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

        return std::nullopt;
    }

    detect_controllers();

    // [AUTO_RECENTER 2026-08-19] Siehe initialize_openvr(): nicht mehr scharfschalten.

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            REVR_LOG(spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name));
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    REVR_LOG(spdlog::info("[VR] Creating OpenXR swapchain"));

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            REVR_LOG(spdlog::error("[VR] {}", m_openxr->error.value()));
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_resolution() {
    // moved to global hook class
    return std::nullopt;
}

std::optional<std::string> VR::hijack_input() {
#if defined(RE2) || defined(RE3)
    REVR_LOG(spdlog::info("[VR] Hijacking InputSystem"));

    // We're going to hook InputSystem.update so we can
    // override the analog stick values with the VR controller's
    auto func = sdk::find_native_method(game_namespace("InputSystem"), "update");

    if (func == nullptr) {
        return "VR init failed: InputSystem.update function not found.";
    }

    REVR_LOG(spdlog::info("InputSystem.update: {:x}", (uintptr_t)func));

    // Hook the native function
    g_input_hook = std::make_unique<FunctionHook>(func, inputsystem_update_hook);

    if (!g_input_hook->create()) {
        return "VR init failed: InputSystem.update native function hook failed.";
    }
#endif

    return std::nullopt;
}

std::optional<std::string> VR::hijack_camera() {
    REVR_LOG(spdlog::info("[VR] Hijacking Camera"));

    const auto get_projection_matrix = (uintptr_t)sdk::find_native_method("via.Camera", "get_ProjectionMatrix");

    ///////////////////////////////
    // Hook GUI camera projection matrix start
    ///////////////////////////////
    auto func = sdk::find_native_method("via.gui.GUICamera", "get_ProjectionMatrix");

    if (func != nullptr) {
        REVR_LOG(spdlog::info("via.gui.GUICamera.get_ProjectionMatrix: {:x}", (uintptr_t)func));
        
        // Pattern scan for the native function call
        auto ref = utility::scan((uintptr_t)func, 0x100, "49 8B C8 E8");

        if (ref) {
            auto native_func = utility::calculate_absolute(*ref + 4);

            if (native_func != get_projection_matrix) {
                // Hook the native function
                g_projection_matrix_hook2 = std::make_unique<FunctionHook>(native_func, gui_camera_get_projection_matrix_hook);

                if (g_projection_matrix_hook2->create()) {
                    REVR_LOG(spdlog::info("Hooked via.gui.GUICamera.get_ProjectionMatrix"));
                }
            } else {
                REVR_LOG(spdlog::info("Did not hook via.gui.GUICamera.get_ProjectionMatrix, same as via.Camera.get_ProjectionMatrix"));
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_wwise_listeners() {
#ifndef RE4
#ifndef SF6
#if TDB_VER < 73
    REVR_LOG(spdlog::info("[VR] Hijacking WwiseListener"));

    const auto t = sdk::find_type_definition("via.wwise.WwiseListener");

    if (t == nullptr) {
        return "VR init failed: via.wwise.WwiseListener type not found.";
    }

    const auto update_method = t->get_method("update");

    if (update_method == nullptr) {
        return "VR init failed: via.wwise.WwiseListener.update method not found.";
    }

    const auto func_wrapper = update_method->get_function();

    if (func_wrapper == nullptr) {
        return "VR init failed: via.wwise.WwiseListener.update native function not found.";
    }
    
    REVR_LOG(spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)func_wrapper));
    
    // Use hde to disassemble the method and find the first jmp, which jmps to the real function
    // in the vtable
    const auto jmp = utility::scan_disasm((uintptr_t)func_wrapper, 10, "48 FF");

    if (!jmp) {
        return "VR init failed: could not find jmp opcode in via.wwise.WwiseListener.update native function.";
    }

    const auto vtable_index = *(uint8_t*)(*jmp + 3) / sizeof(void*);
    REVR_LOG(spdlog::info("via.wwise.WwiseListener.update vtable index: {}", vtable_index));
    REVR_LOG(spdlog::info("Attempting to create fake via.wwise.WwiseListener instance"));

    const void* fake_obj = t->create_instance_full();

    if (fake_obj == nullptr) {
        return "VR init failed: Failed to create fake via.wwise.WwiseListener instance.";
    }
    
    REVR_LOG(spdlog::info("Attempting to read vtable from fake via.wwise.WwiseListener instance"));
    auto obj_vtable = *(void***)fake_obj;

    if (obj_vtable == nullptr) {
        return "VR init failed: via.wwise.WwiseListener vtable not found.";
    }

    REVR_LOG(spdlog::info("via.wwise.WwiseListener vtable: {:x}", (uintptr_t)obj_vtable - g_framework->get_module()));

    auto update_native = obj_vtable[vtable_index];

    if (update_native == 0) {
        return "VR init failed: via.wwise.WwiseListener update native not found.";
    }

    REVR_LOG(spdlog::info("via.wwise.WwiseListener.update: {:x}", (uintptr_t)update_native));

    g_wwise_listener_update_hook = std::make_unique<FunctionHook>(update_native, wwise_listener_update_hook);

    if (!g_wwise_listener_update_hook->create()) {
        return "VR init failed: via.wwise.WwiseListener update native function hook failed.";
    }
#endif
#endif
#endif

    return std::nullopt;
}

bool VR::detect_controllers() {
    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        REVR_LOG(spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex));
        REVR_LOG(spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex));
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        REVR_LOG(spdlog::info("Left Hand: {}", 1));
        REVR_LOG(spdlog::info("Right Hand: {}", 2));
    }


    return true;
}

bool VR::is_any_action_down() {
    if (!m_runtime->ready() || !is_using_controllers()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            return true;
        }
    }

    return false;
}

void VR::update_hmd_state() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();
    
    if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::EARLY) {
        if (runtime->is_openxr()) {
            if (g_framework->get_renderer_type() == REFramework::RendererType::D3D11) {
                if (!runtime->got_first_sync || runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                    return;
                }  
            } else if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }

            m_openxr->begin_frame();
        } else {
            if (runtime->synchronize_frame() != VRRuntime::Error::SUCCESS) {
                return;
            }
        }
    }
    
    runtime->update_poses();

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);
        request_flatscreen_reanchor();   // [CANVAS_RECENTER] Runtime-Recenter

        runtime->wants_reset_origin = false;
    }

    // [AUTO_RECENTER 2026-08-18] Einmaliges Recenter nach der Initialisierung. Muss hier stehen
    // und nicht in den initialize_*-Funktionen: erst nach update_poses() liefert get_rotation(0)
    // eine echte HMD-Pose. Der pose_mtx des Blocks darueber ist mit dessen Scope wieder frei,
    // recenter_view() darf also selbst locken.
    if (m_wants_auto_recenter && runtime->ready() && runtime->got_first_valid_poses) {
        static constexpr auto AUTO_RECENTER_DELAY = std::chrono::milliseconds{2000};

        if (std::chrono::steady_clock::now() - m_auto_recenter_armed_at >= AUTO_RECENTER_DELAY) {
            m_wants_auto_recenter = false;
            recenter_view();

            REVR_LOG(spdlog::info("[VR] Auto recenter fired"));
        }
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;

    // Forcefully update the camera transform after submitting the frame
    // because the game logic thread does not run in sync with the rendering thread
    // This will massively improve HMD rotation smoothness for the user
    // if this is not done, the left eye will jitter a lot
#if defined(RE2) || defined(RE3)
    const auto cameras = get_cameras();

    for (auto camera : cameras) {
        if (camera == nullptr) {
            continue;
        }

        if (camera->ownerGameObject != nullptr && camera->ownerGameObject->transform != nullptr) {
            if (camera == cameras[0]) {
                FirstPerson::get()->on_update_transform(camera->ownerGameObject->transform);
            } else if (cameras[0] != nullptr && cameras[0]->ownerGameObject != nullptr && cameras[0]->ownerGameObject->transform != nullptr) {
                auto transform0 = cameras[0]->ownerGameObject->transform;
                auto transform1 = camera->ownerGameObject->transform;

                const auto camera_joint = utility::re_transform::get_joint(*transform1, 0);

                if (camera_joint == nullptr) {
                    continue;
                }

                auto& fp = FirstPerson::get();

                const auto mat = fp->get_last_camera_matrix();
                const auto pos = mat[3];

                //transform1->angles = transform0->angles;
                //transform1->position = transform0->position;
                transform1->worldTransform = transform0->worldTransform;

                sdk::set_joint_position(camera_joint, pos);
                sdk::set_joint_rotation(camera_joint, mat);
            }
        }
    }
#endif
}

void VR::update_action_states() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (runtime->wants_reinitialize) {
        return;
    }

    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            REVR_LOG(spdlog::error("VRInput failed to update action state: {}", (uint32_t)error));
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            REVR_LOG(spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count()));

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }

#if !defined(RE4)
    // [MENU SIZE 11.11.2026] the panel and text size of the VR menu (RE4: its menu editor)
    if (g_framework != nullptr) {
        g_framework->set_vr_menu_panel_width(m_menu_panel_width->value());
        g_framework->set_vr_menu_detail_text_scale(m_menu_text_scale->value());
    }

    // [MENU COMBO 11.11.2026] left trigger (or the left hand's weapon dial action) + left B (triangle on the PS VR2
    // Sense controller, right Y on the Steam Frame) toggles the menu, as in RE4. Read raw: with the menu open the game's actions are blocked, the
    // combination must close it too. Edge only, so holding it does not toggle again.
    if (runtime->is_openvr() && g_framework != nullptr && m_left_joystick != vr::k_ulInvalidInputValueHandle) {
        const bool left_trigger = is_action_active_raw(m_action_trigger, m_left_joystick) || is_action_active_raw(m_action_weapon_dial, m_left_joystick);
        // [STEAM_FRAME RE2/RE7 fix149] the Steam Frame profile has no left B: its Y on the right (Frame_LeftB) counts too, as
        // in RE4 (RE4VRBinding)
        const bool frame_b = m_right_joystick != vr::k_ulInvalidInputValueHandle && is_action_active_raw(m_action_frame_left_b, m_right_joystick);
        const bool combo = left_trigger && (is_action_active_raw(m_action_b_button, m_left_joystick) || frame_b);

        if (combo && !m_menu_combo_was) {
            g_framework->set_draw_ui(!g_framework->is_drawing_ui());
        }

        m_menu_combo_was = combo;
    }
#endif
}

void VR::update_camera() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        m_needs_camera_restore = false;
        return;
    }

    // [CANVAS_KOPF + CANVAS_FOV 26.09.2026] Leinwand/Suspend: die Kamera bleibt komplett die des
    // Spiels -- keine Kopfpose in Joint 0 (sonst steuert das Headset die Cutscene-Kamera) und
    // auch KEIN FOV/Seitenverhaeltnis vom Headset. Belegt (zzz_re4_cut2d_kamera_sonde): FOV stand
    // waehrend der Leinwand auf 109,3 (Index) statt 26/36 (Cutscene) -> falscher Bildausschnitt.
    // Ohne Write auch kein Restore.
    if (should_suspend_camera_overrides()) {
        m_needs_camera_restore = false;
        return;
    }

    if (inside_on_end) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        REVR_LOG(spdlog::error("VR: Failed to get primary camera!"));
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            break;
        }

        static auto via_camera = sdk::find_type_definition("via.Camera");
        static auto get_near_clip_plane_method = via_camera->get_method("get_NearClipPlane");
        static auto get_far_clip_plane_method = via_camera->get_method("get_FarClipPlane");
        static auto set_far_clip_plane_method = via_camera->get_method("set_FarClipPlane");
        static auto set_near_clip_plane_method = via_camera->get_method("set_NearClipPlane");
        static auto set_fov_method = via_camera->get_method("set_FOV");
        static auto set_vertical_enable_method = via_camera->get_method("set_VerticalEnable");
        static auto set_aspect_ratio_method = via_camera->get_method("set_AspectRatio");

        if (m_use_custom_view_distance->value()) {
            set_far_clip_plane_method->call<void*>(sdk::get_thread_context(), camera, m_view_distance->value());
        }

        m_nearz = get_near_clip_plane_method->call<float>(sdk::get_thread_context(), camera);
        m_farz = get_far_clip_plane_method->call<float>(sdk::get_thread_context(), camera);

        // [NEAR CLIP 09.11.2026] a larger near plane: the camera property (the engine's own uses of it, the clones copy it,
        // the upscaler reads it) and the projection the runtime builds from m_nearz
        if (const auto near_min = get_near_clip_min(); near_min > 0.0f && m_nearz < near_min && set_near_clip_plane_method != nullptr) {
            set_near_clip_plane_method->call<void*>(sdk::get_thread_context(), camera, near_min);
            m_nearz = near_min;
        }

        // Disable certain effects like the 3D overlay during the sewer gators
        // section in RE7
        disable_bad_effects();
        // Disable lens distortion
        set_lens_distortion(false);

    #if defined(RE2) || defined(RE3)
        if (FirstPerson::get()->will_be_used()) {
            m_needs_camera_restore = false;

            auto camera_object = utility::re_component::get_game_object(camera);

            if (camera_object == nullptr || camera_object->transform == nullptr) {
                return;
            }

            auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

            if (camera_joint == nullptr) {
                return;
            }

            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;

            return;
        }
    #endif

        // [MONO PERIPHERY] the shared wide view's camera gets the FOV / aspect of its wider (union) frustum
        int32_t view_eye = -1;
        bool view_fovea = false;
        const bool center_view = is_using_multipass() && view_of_camera(camera, view_eye, view_fovea) && view_eye == CENTER_EYE;
        auto projection_matrix = is_using_multipass() ? get_projection_matrix(center_view ? (uint32_t)CENTER_EYE : 0u) : get_current_projection_matrix(true);

        // Steps towards getting lens flares and volumetric lighting working
        // Get the FOV from the projection matrix
        const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
        const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
        const auto hfov = vfov * aspect;
        
        //spdlog::info("vFOV: {}", vfov);
        //spdlog::info("Aspect: {}", aspect);

        // [FOVEA FOV TEST 06.11.2026 evening] (GPU test) a fovea camera's FOV narrowed to what holds its fovea: the image comes
        // from the mod's projection (apply_quadview_inset); what the engine does with the property is what the test measures.
        // Here and not at LockScene: this is the last write before the frame is rendered (the copy at LockScene comes first).
        float fov = vfov;

        if (view_fovea && quadview::State::get().test_narrow_fovea_fov.load(std::memory_order_relaxed)) {
            fov = fovea_cull_fov(view_eye, vfov, aspect, true);
        }

        set_fov_method->call<void*>(sdk::get_thread_context(), camera, fov);
        set_vertical_enable_method->call<void*>(sdk::get_thread_context(), camera, true);
        set_aspect_ratio_method->call<void*>(sdk::get_thread_context(), camera, aspect);
    }

    update_camera_origin();
    m_needs_camera_restore = true;
}

void VR::update_camera_origin() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        return;
    }

    // this means that the user just put the headset on or something
    // and we caught them in a bad frame
    if (inside_on_end && !m_needs_camera_restore) {
        return;
    }

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            break; // [HEIGHT PROBE 12.11.2026] (was return: nothing after the loop but the probe)
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            REVR_LOG(spdlog::error("VR: Failed to get camera game object or transform!"));
            m_needs_camera_restore = false;
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (camera_joint == nullptr) {
            REVR_LOG(spdlog::error("VR: Failed to get camera joint!"));
            m_needs_camera_restore = false;
            return;
        }

        if (!inside_on_end && camera == cameras[0]) {
            m_original_camera_position = sdk::get_joint_position(camera_joint);
            m_original_camera_rotation = sdk::get_joint_rotation(camera_joint);
            m_original_camera_matrix = Matrix4x4f{m_original_camera_rotation};
            m_original_camera_matrix[3] = m_original_camera_position;
        }

        apply_hmd_transform(camera_joint);
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] heights for the debug log
    sample_height_probe(cameras); // [HEIGHT PROBE 12.11.2026]
#endif
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [HEIGHT PROBE 12.11.2026] Heights (world Y, metres) of the frame's camera chain, written to the periodic log: the headset
// and the standing origin, the game camera before the headset is applied, what each view renders from (first joint) and
// the camera's transform; in RE7/RE8 also the player's feet and the player's head joint. RE7 test of 12.11.2026: the view
// sat too low even right after "Reset View and Position".
void VR::sample_height_probe(const std::array<RECamera*, MAX_CAMERAS>& cameras) {
    HeightProbe p{};
    p.valid = true;
    p.hmd = get_position(0).y;
    p.standing = m_standing_origin.y;
    p.game_camera = m_original_camera_position.y;

    for (size_t i = 0; i < cameras.size() && i < p.views.size(); ++i) {
        auto* camera = cameras[i];

        if (camera == nullptr) {
            break;
        }

        auto* go = utility::re_component::get_game_object(camera);

        if (go == nullptr || go->transform == nullptr) {
            continue;
        }

        if (auto* joint = utility::re_transform::get_joint(*go->transform, 0); joint != nullptr) {
            p.views[i] = sdk::get_joint_position(joint).y;
        }

        if (i == 0) {
            p.camera_transform = sdk::get_transform_position(go->transform).y;
        }

        p.view_count = (int)i + 1;
    }

#if defined(RE7) || defined(RE8)
    if (auto* player = (::REGameObject*)RE8VR::get()->get_localplayer(); player != nullptr && player->transform != nullptr) {
        p.player = sdk::get_transform_position(player->transform).y;
        p.has_player = true;

        static const auto head_hash = sdk::murmur_hash::calc32("Head");

        if (auto* head = sdk::get_transform_joint_by_hash(player->transform, head_hash); head != nullptr) {
            p.head = sdk::get_joint_position(head).y;
            p.has_head = true;
        }
    }
#endif

    std::scoped_lock _{m_height_probe_mutex};
    m_height_probe = p;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string VR::height_probe_text() {
    HeightProbe p{};

    {
        std::scoped_lock _{m_height_probe_mutex};
        p = m_height_probe;
    }

    if (!p.valid) {
        return "height: no camera update yet";
    }

    std::string views{};

    for (int i = 0; i < p.view_count; ++i) {
        views += std::format("{}{:.3f}", i == 0 ? "" : " ", p.views[i]);
    }

    std::string o = std::format("height (world Y, m): headset {:.3f}, standing origin {:.3f} (headset above it {:+.3f}) | game camera {:.3f}, "
                                "camera transform {:.3f}, views render from [{}]",
        p.hmd, p.standing, p.hmd - p.standing, p.game_camera, p.camera_transform, views);

    if (p.has_player) {
        o += std::format(" | player feet {:.3f}: game camera {:+.3f} above them, view 0 {:+.3f}", p.player, p.game_camera - p.player,
            p.views[0] - p.player);

        if (p.has_head) {
            o += std::format(", head joint {:+.3f}", p.head - p.player);
        }
    }

#if defined(RE7) || defined(RE8)
    {
        auto& c = RE8VR::camera_probe();
        const auto calls = c.calls.exchange(0);
        o += std::format(" | RE8VR fix_player_camera: {} calls since the last report, last: camera transform {:.3f} before the headset "
                         "(posture offset applied {}, controllable {}, cutscene {}), {:.3f} after",
            calls, c.pre_y.load(), c.offset_applied.load(), c.controllable.load(), c.cutscene.load(), c.post_y.load());
    }
#endif

    return o;
}
#endif

void VR::apply_hmd_transform(glm::quat& rotation, Vector4f& position) {
    REF_PROFILE_FUNCTION();

#if defined(RE7)
    // [MENU SHEET 14.11.2026] the options in the main menu: the view stays at the deck's framing while the game camera goes
    // down to the cassette label (only the sheet is brought up, apply_menu_sheet)
    if (m_menu_hold && m_menu_camera_active && !m_flatscreen_overlay) {
        rotation = m_menu_deck_rot;
        position = m_menu_deck_pos;
    }
#endif

    const auto rotation_offset = get_rotation_offset();
    const auto current_hmd_rotation = glm::normalize(rotation_offset * glm::quat{get_rotation(0)});
    
    glm::quat new_rotation{};
    glm::quat camera_rotation{};
    
    if (!m_decoupled_pitch->value()) {
        camera_rotation = rotation;
        new_rotation = glm::normalize(rotation * current_hmd_rotation);
    } else if (m_decoupled_pitch->value()) {
        // facing forward matrix
        const auto camera_rotation_matrix = utility::math::remove_y_component(Matrix4x4f{rotation});
        camera_rotation = glm::quat{camera_rotation_matrix};
        new_rotation = glm::normalize(camera_rotation * current_hmd_rotation);
    }

    // [ROOMSCALE-KAMERA 16.09.2026] Beim Roomscale-Ducken senkt das Bein-IK Leons
    // Kopf ab, und die Spielkamera (am Charakter) sinkt mit. Gemessen: Kopf
    // 0,73 m tiefer bei 0,75 m Headset-Absenkung -- und die Kamera landete knapp
    // ueber dem Boden, weil der Headset-Versatz hier dieselbe Hoehe noch einmal
    // abzog. Der Ausgleich nimmt genau den Anteil heraus, den das IK schon
    // traegt. Ohne Roomscale ist er 0 -- dann rechnet diese Zeile wie immer.
    auto hmd_rel = get_position(0) - m_standing_origin;

#if defined(RE7)
    // [MENU VIEW 14.11.2026] the main menu in stereo: its own view height on top of the headset's (the cassette deck sat
    // above eye level, user 14.11.2026). Only while the game camera is the menu's and no canvas shows it.
    if (m_menu_camera_active && !m_flatscreen_overlay) {
        hmd_rel.y += m_menu_view_height->value();
    }
#endif

    // [KAMERA-IST-HOEHE 16.09.2026 -- Befund des Users] Mit dem Soll allein kam die
    // Kamera beim AUFSTEHEN zu spaet hoch: das Bein-IK hebt Leons Mesh sofort,
    // die Spielkamera folgt ihm aber GEGLAETTET. Beim Ducken fiel das nicht auf
    // (Kamera kurz ueber dem Kopf), beim Aufstehen steckte man im Mesh.
    //
    // Darum wird hier nicht das Soll abgezogen, sondern wie weit die Spielkamera
    // TATSAECHLICH unter ihrer Stehhoehe ueber Leons Transform liegt. Dann gilt
    // immer: Kamera = Stehhoehe + echte Kopfhoehe -- egal, wie traege die
    // Spielkamera folgt. Die Stehhoehe wird gelernt, solange das IK-Soll ~0 ist
    // (aufrecht). Ist noch keine gelernt, gilt das Soll wie bisher.
    //
    // Ohne Roomscale ist m_rs_body_y_valid false: die Zeile rechnet wie immer.
    if (m_rs_body_y_valid) {
        const float base_rel = position.y - m_rs_body_y;

        if (std::abs(m_roomscale_camera_y_comp) < 0.015f) {
            m_rs_base_stand = m_rs_base_stand_valid
                ? (m_rs_base_stand + (base_rel - m_rs_base_stand) * 0.1f)
                : base_rel;
            m_rs_base_stand_valid = true;
        }

        const float target = m_rs_base_stand_valid
            ? (std::min)(base_rel - m_rs_base_stand, 0.0f)
            : m_roomscale_camera_y_comp;

        // [KAMERA-LERP 16.09.2026] Nah an 1:1, aber weich: Zeitkonstante
        // RS_CAM_TAU_S -- nach ihr sind 63 % des Weges geschafft, nach dem
        // Dreifachen 95 %. Kleiner = schneller/direkter, groesser = weicher.
        constexpr float RS_CAM_TAU_S = 0.04f;

        const auto now = std::chrono::steady_clock::now();
        float dt_s = m_rs_drop_t_valid
            ? std::chrono::duration<float>(now - m_rs_drop_t).count()
            : 1.0f;   // erster Aufruf: sofort auf den Zielwert
        m_rs_drop_t = now;
        m_rs_drop_t_valid = true;

        dt_s = (std::min)((std::max)(dt_s, 0.0f), 1.0f);

        const float a = 1.0f - std::exp(-dt_s / RS_CAM_TAU_S);
        m_rs_drop_smooth += (target - m_rs_drop_smooth) * a;

        hmd_rel.y -= m_rs_drop_smooth;
    }

    auto current_relative_pos = rotation_offset * hmd_rel /*+ current_relative_eye_pos*/;
    current_relative_pos.w = 0.0f;

    auto current_head_pos = camera_rotation * current_relative_pos;

    rotation = new_rotation;
    position = position + current_head_pos;

#if defined(RE7)
    // [MENU VIEW 14.11.2026] the main menu in stereo: the view moved back, horizontally, away from where the game camera looks
    if (m_menu_camera_active && !m_flatscreen_overlay && m_menu_view_back->value() != 0.0f) {
        const auto fwd = camera_rotation * glm::vec3{0.0f, 0.0f, -1.0f};
        auto flat = glm::vec3{fwd.x, 0.0f, fwd.z};

        if (glm::length(flat) > 1e-4f) {
            flat = glm::normalize(flat) * m_menu_view_back->value();
            position -= Vector4f{flat.x, 0.0f, flat.z, 0.0f};
        }
    }
#endif
}

void VR::apply_hmd_transform(::REJoint* camera_joint) {
    REF_PROFILE_FUNCTION();

    auto rotation = m_original_camera_rotation;
    auto position = m_original_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

bool VR::is_hand_behind_head(VRRuntime::Hand hand, float sensitivity) const {
    if (hand > VRRuntime::Hand::RIGHT || !is_using_controllers()) {
        return false;
    }

    const auto hand_index = get_controllers()[(uint32_t)hand];

    const auto hmd = get_transform(0);
    const auto hand_pos = get_position(hand_index);
    const auto hmd_delta = Vector3f{hand_pos - hmd[3]};
    const auto distance = glm::length(hmd_delta);

    if (distance >= 0.3f) {
        return false;
    }

    const auto hmd_dir = glm::normalize(hmd_delta);

    const auto& hmd_forward = hmd[2];
    const auto flattened_forward = glm::normalize(Vector3f{hmd_forward.x, 0.0f, hmd_forward.z});

    const auto hand_dot_flat_raw = glm::dot(flattened_forward, hmd_dir);
    return hand_dot_flat_raw >= sensitivity;
}

void VR::update_audio_camera() {
    REF_PROFILE_FUNCTION();

    if (!is_hmd_active()) {
        return;
    }

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_object = utility::re_component::get_game_object(camera);

    if (camera_object == nullptr || camera_object->transform == nullptr) {
        return;
    }

    auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

    if (camera_joint == nullptr) {
        return;
    }

    m_original_audio_camera_position = sdk::get_joint_position(camera_joint);
    m_original_audio_camera_rotation = sdk::get_joint_rotation(camera_joint);
    m_needs_audio_restore = true;

    auto rotation = m_original_audio_camera_rotation;
    auto position = m_original_audio_camera_position;

    apply_hmd_transform(rotation, position);

    sdk::set_joint_rotation(camera_joint, rotation);

    if (m_positional_tracking) {
        sdk::set_joint_position(camera_joint, position);   
    }
}

void VR::update_render_matrix() {
    REF_PROFILE_FUNCTION();

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr || camera != cameras[0]) {
            return;
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            return;
        }

        auto camera_joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (camera_joint == nullptr) {
            return;
        }

        m_render_camera_matrix = Matrix4x4f{sdk::get_joint_rotation(camera_joint)};
        m_render_camera_matrix[3] = sdk::get_joint_position(camera_joint);
    }
}

void VR::restore_audio_camera() {
    if (!m_needs_audio_restore) {
        return;
    }

#if defined(RE2) || defined(RE3)
    if (FirstPerson::get()->will_be_used()) {
        m_needs_audio_restore = false;
        return;
    }
#endif

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    auto camera_object = utility::re_component::get_game_object(camera);

    if (camera_object == nullptr || camera_object->transform == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    //camera_object->transform->worldTransform = m_original_camera_matrix;

    auto joint = utility::re_transform::get_joint(*camera_object->transform, 0);

    if (joint == nullptr) {
        m_needs_audio_restore = false;
        return;
    }

    sdk::set_joint_rotation(joint, m_original_audio_camera_rotation);
    sdk::set_joint_position(joint, m_original_audio_camera_position);

    m_needs_audio_restore = false;
}

void VR::restore_camera() {
    REF_PROFILE_FUNCTION();

    if (!m_needs_camera_restore) {
        return;
    }

#if defined(RE2) || defined(RE3)
    if (FirstPerson::get()->will_be_used()) {
        m_needs_camera_restore = false;
        return;
    }
#endif

    auto cameras = get_cameras();

    if (cameras[0] == nullptr) {
        m_needs_camera_restore = false;
        return;
    }

    for (auto camera : cameras) {
        if (camera == nullptr) {
            return;
        }

        auto camera_object = utility::re_component::get_game_object(camera);

        if (camera_object == nullptr || camera_object->transform == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        //camera_object->transform->worldTransform = m_original_camera_matrix;

        auto joint = utility::re_transform::get_joint(*camera_object->transform, 0);

        if (joint == nullptr) {
            m_needs_camera_restore = false;
            return;
        }

        sdk::set_joint_rotation(joint, m_original_camera_rotation);
        sdk::set_joint_position(joint, m_original_camera_position);
    }


    m_needs_camera_restore = false;
}

void VR::set_lens_distortion(bool value) {
    REF_PROFILE_FUNCTION();

    if (!m_force_lensdistortion_settings->value()) {
        return;
    }

#ifdef RE7
    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    static auto lens_distortion_tdef = sdk::find_type_definition(game_namespace("LensDistortionController"));
    static auto lens_distortion_t = lens_distortion_tdef->get_type();

    auto lens_distortion_component = utility::re_component::find(camera, lens_distortion_t);

    if (lens_distortion_component != nullptr) {
        // Get "LensDistortion" field
        auto lens_distortion = *sdk::get_object_field<REManagedObject*>(lens_distortion_component, "LensDistortion");

        if (lens_distortion != nullptr) {
            // Call "set_Enabled" method
            sdk::call_object_func<void*>(lens_distortion, "set_Enabled", sdk::get_thread_context(), lens_distortion, value);
        }
    }
#endif
}

void VR::disable_bad_effects() {
    REF_PROFILE_FUNCTION();

    auto context = sdk::get_thread_context();

    auto application = sdk::Application::get();

    // minor optimizations to prevent hashing and map lookups every frame
    static auto renderer_t = sdk::find_type_definition("via.render.Renderer");

    static auto get_render_config_method = renderer_t->get_method("get_RenderConfig");

    static auto render_config_t = sdk::find_type_definition("via.render.RenderConfig");

    static auto get_framerate_setting_method = render_config_t->get_method("get_FramerateSetting");
    static auto set_framerate_setting_method = render_config_t->get_method("set_FramerateSetting");

    static auto get_antialiasing_method = render_config_t->get_method("get_AntiAliasing");
    static auto set_antialiasing_method = render_config_t->get_method("set_AntiAliasing");

    static auto get_lens_distortion_setting_method = render_config_t->get_method("getLensDistortionSetting");
    static auto set_lens_distortion_setting_method = render_config_t->get_method("setLensDistortionSetting");

    static auto get_motion_blur_enable_method = render_config_t->get_method("get_MotionBlurEnable");
    static auto set_motion_blur_enable_method = render_config_t->get_method("set_MotionBlurEnable");

    static auto get_vsync_method = render_config_t->get_method("get_VSync");
    static auto set_vsync_method = render_config_t->get_method("set_VSync");

    static auto get_transparent_buffer_quality_method = render_config_t->get_method("get_TransparentBufferQuality");
    static auto set_transparent_buffer_quality_method = render_config_t->get_method("set_TransparentBufferQuality");

    static auto get_lensflare_enable_method = render_config_t->get_method("get_LensFlareEnable");
    static auto set_lensflare_enable_method = render_config_t->get_method("set_LensFlareEnable");

    static auto get_colorspace_method = render_config_t->get_method("get_ColorSpace");
    static auto set_colorspace_method = render_config_t->get_method("set_ColorSpace");

    static auto get_dynamic_shadow_enable_method = render_config_t->get_method("get_DynamicShadowEnable");
    static auto set_dynamic_shadow_enable_method = render_config_t->get_method("set_DynamicShadowEnable");

    static auto get_hdrmode_method = renderer_t->get_method("get_HDRMode");
    static auto set_hdrmode_method = renderer_t->get_method("set_HDRMode");

    static auto get_hdr_display_mode_enable_method = renderer_t->get_method("get_HDRDisplayModeEnable");
    static auto set_hdr_display_mode_enable_method = renderer_t->get_method("set_HDRDisplayModeEnable");

    static auto get_delay_render_enable_method = renderer_t->get_method("get_DelayRenderEnable");
    static auto set_delay_render_enable_method = renderer_t->get_method("set_DelayRenderEnable");

    auto renderer = renderer_t->get_instance();

    auto render_config = get_render_config_method->call<::REManagedObject*>(context, renderer);

    if (render_config == nullptr) {
        REVR_LOG(spdlog::info("No render config!"));
        return;
    }

    static const auto is_sf6 = utility::get_module_path(utility::get_executable())->find("StreetFighter") != std::string::npos;

    if (!is_sf6 && m_force_fps_settings->value() && get_framerate_setting_method != nullptr && set_framerate_setting_method != nullptr) {
        const auto framerate_setting = get_framerate_setting_method->call<via::render::RenderConfig::FramerateType>(context, render_config);

        // Allow FPS to go above 60
        if (framerate_setting != via::render::RenderConfig::FramerateType::VARIABLE) {
            set_framerate_setting_method->call<void*>(context, render_config, via::render::RenderConfig::FramerateType::VARIABLE);
            REVR_LOG(spdlog::info("[VR] Set framerate to variable"));
        }
    }
    
    // get_MaxFps on application
    if (!is_sf6 && m_force_fps_settings->value() && application->get_max_fps() <  600.0f) {
        application->set_max_fps(600.0f);

        static bool once = []() {
            REVR_LOG(spdlog::info("[VR] Max FPS set to {}", 600.0f));
            return true;
        }();
    }

    if (m_force_aa_settings->value() && get_antialiasing_method != nullptr && set_antialiasing_method != nullptr) {
        const auto antialiasing = get_antialiasing_method->call<via::render::RenderConfig::AntiAliasingType>(context, render_config);

        // Disable TAA
        switch (antialiasing) {
            case via::render::RenderConfig::AntiAliasingType::TAA: [[fallthrough]];
            case via::render::RenderConfig::AntiAliasingType::FXAA_TAA:
                set_antialiasing_method->call<void*>(context, render_config, via::render::RenderConfig::AntiAliasingType::NONE);
                REVR_LOG(spdlog::info("[VR] TAA disabled"));
                break;
            default:
                break;
        }
    }

    if (m_force_lensdistortion_settings->value() && get_lens_distortion_setting_method != nullptr && set_lens_distortion_setting_method != nullptr) {
        const auto lens_distortion_setting = get_lens_distortion_setting_method->call<via::render::RenderConfig::LensDistortionSetting>(context, render_config);

        // Disable lens distortion
        if (lens_distortion_setting != via::render::RenderConfig::LensDistortionSetting::OFF) {
            set_lens_distortion_setting_method->call<void*>(context, render_config, via::render::RenderConfig::LensDistortionSetting::OFF);
            REVR_LOG(spdlog::info("[VR] Lens distortion disabled"));
        }
    }

    if (m_force_motionblur_settings->value() && get_motion_blur_enable_method != nullptr && set_motion_blur_enable_method != nullptr) {
        const auto is_motion_blur_enabled = get_motion_blur_enable_method->call<bool>(context, render_config);

        // Disable motion blur
        if (is_motion_blur_enabled) {
            set_motion_blur_enable_method->call<void*>(context, render_config, false);
            REVR_LOG(spdlog::info("[VR] Motion blur disabled"));
        }
    }

    if (m_force_vsync_settings->value() && get_vsync_method != nullptr && set_vsync_method != nullptr) {
#ifndef MHRISE
        const auto vsync = get_vsync_method->call<bool>(context, render_config);

        // Disable vsync
        if (vsync) {
            set_vsync_method->call<void*>(context, render_config, false);
            REVR_LOG(spdlog::info("[VR] VSync disabled"));
        }
#else
        // We are only calling set_vsync instead of checking with get_vsync in MHRise
        // because get_VSync has some insane code protection on it for some reason
        // which would increase frametimes to 15+
        set_vsync_method->call<void*>(context, render_config, false);
#endif
    }

    if (m_force_volumetrics_settings->value() && get_transparent_buffer_quality_method != nullptr && set_transparent_buffer_quality_method != nullptr) {
        const auto transparent_buffer_quality = get_transparent_buffer_quality_method->call<via::render::RenderConfig::Quality>(context, render_config);

        // Disable volumetrics
        if (transparent_buffer_quality != via::render::RenderConfig::Quality::NONE) {
            set_transparent_buffer_quality_method->call<void*>(context, render_config, via::render::RenderConfig::Quality::NONE);
            REVR_LOG(spdlog::info("[VR] Volumetrics disabled"));
        }
    }

    if (m_force_lensflares_settings->value() && get_lensflare_enable_method != nullptr && set_lensflare_enable_method != nullptr) {
        const auto is_lensflare_enabled = get_lensflare_enable_method->call<bool>(context, render_config);

        // Disable lensflares
        if (is_lensflare_enabled) {
            set_lensflare_enable_method->call<void*>(context, render_config, false);
            REVR_LOG(spdlog::info("[VR] Lensflares disabled"));
        }
    }

    if (get_hdrmode_method != nullptr && set_hdrmode_method != nullptr) {
        // static
        const auto is_hdr_enabled = get_hdrmode_method->call<bool>(context);

        // Disable HDR
        if (is_hdr_enabled) {
            set_hdrmode_method->call<void*>(context, false);
            
            if (set_hdr_display_mode_enable_method != nullptr) {
                set_hdr_display_mode_enable_method->call<void*>(context, false);
            }

            REVR_LOG(spdlog::info("[VR] HDR disabled"));
        }
    }

    if (get_colorspace_method != nullptr && set_colorspace_method != nullptr) {
        const auto is_hdr_enabled = get_colorspace_method->call<via::render::ColorSpace>(context, render_config) == via::render::ColorSpace::HDR10;

        if (is_hdr_enabled) {
            set_colorspace_method->call<void*>(context, render_config, via::render::ColorSpace::HDTV);
            REVR_LOG(spdlog::info("[VR] HDR disabled (ColorSpace)"));
        }
    }

    // [ENGINE SHADOWS 30.10.2026] the engine shadow test's value for its phase wins over the forcing below
    if (const int shadow_override = m_dynamic_shadow_override.load();
        shadow_override >= 0 && get_dynamic_shadow_enable_method != nullptr && set_dynamic_shadow_enable_method != nullptr) {
        const bool want = shadow_override == 1;

        if (get_dynamic_shadow_enable_method->call<bool>(context, render_config) != want) {
            set_dynamic_shadow_enable_method->call<void*>(context, render_config, want);
        }
    } else if (m_force_dynamic_shadows_settings->value() && get_dynamic_shadow_enable_method != nullptr && set_dynamic_shadow_enable_method != nullptr) {
        const auto is_dynamic_shadow_enabled = get_dynamic_shadow_enable_method->call<bool>(context, render_config);

        // Enable dynamic shadows
        if (!is_dynamic_shadow_enabled) {
            set_dynamic_shadow_enable_method->call<void*>(context, render_config, true);
            REVR_LOG(spdlog::info("[VR] Dynamic shadows enabled"));
        }
    }

    // [GAME OPTIONS 12.11.2026] Options of the game's own menu that do not work in VR, off while the headset is in use
    // (the game's ini keeps the player's choice; only the running values change):
    //  - the game's upscaling (RenderConfig.UpscaleType, the FSR 1 modes of the ray-tracing versions) and a render scale
    //    below 1: the scene is drawn smaller than the view's target, depth and motion come out smaller than the colour
    //    (RE7 log of 12.11.2026: 1347x1040 against 1752x1352), the fovea upscaler cannot run and the image no longer
    //    matches the eye's projection
    //  - depth of field, chromatic aberration and film grain: screen effects that differ per view (the fovea and the
    //    wide view get different grain and blur) and that in a headset read as a dirty or blurred lens
    //  - full screen: the windowed mode instead
    // (frame rate variable, anti-aliasing, lens distortion, motion blur, vsync, lens flares and HDR are the options
    // above, each with its "force" setting in the VR menu)
    {
        const auto find_setter = [](std::initializer_list<const char*> names) -> sdk::REMethodDefinition* {
            for (const auto* name : names) {
                if (auto* m = render_config_t->get_method(name); m != nullptr) {
                    return m;
                }
            }

            return nullptr;
        };

        static auto get_upscale_type_method = find_setter({"get_UpscaleType", "getUpscaleType"});
        static auto set_upscale_type_method = find_setter({"set_UpscaleType", "setUpscaleType"});
        static auto get_image_quality_rate_method = find_setter({"get_ImageQualityRate"});
        static auto set_image_quality_rate_method = find_setter({"set_ImageQualityRate"});

        // the "no upscaling" value of the enum, by name (the numbers differ between games)
        static const std::optional<int32_t> upscale_off = [&]() -> std::optional<int32_t> {
            if (get_upscale_type_method == nullptr) {
                return std::nullopt;
            }

            const auto* t = get_upscale_type_method->get_return_type();

            if (t == nullptr || !t->is_enum()) {
                return std::nullopt;
            }

            std::string names{};
            std::optional<int32_t> found{};

            for (auto* f : t->get_fields()) {
                if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal() ||
                    std::string_view{f->get_name()} == "value__") {
                    continue;
                }

                const auto* d = (const int32_t*)f->get_init_data();

                if (d == nullptr) {
                    continue;
                }

                std::string lower{f->get_name()};
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                REVR_LOG(names += std::format("{}{}={}", names.empty() ? "" : ", ", f->get_name(), *d));

                if (!found && (lower == "none" || lower == "off" || lower == "disable" || lower == "disabled" || lower == "native" ||
                               lower == "nouse" || lower == "invalid")) {
                    found = *d;
                }
            }

            REVR_LOG(spdlog::info("[VR] {}: {} -> off value {}", t->get_full_name(), names, found ? std::to_string(*found) : std::string{"not found"}));
            return found;
        }();

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        // the window mode, only written to the log once (exclusive full screen loses the swapchain to the headset's
        // compositor and resizes it in a loop: RE7 log of 12.11.2026, about 50 resizes a second)
        static const bool window_mode_logged = [&]() {
            for (const auto* getter : {"get_WindowMode", "get_FullScreenMode", "get_FullScreenDisplayMode"}) {
                auto* m = render_config_t->get_method(getter);

                if (m == nullptr) {
                    continue;
                }

                const auto* t = m->get_return_type();

                if (t != nullptr && t->is_enum()) {
                    std::string names{};

                    for (auto* f : t->get_fields()) {
                        if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal() ||
                            std::string_view{f->get_name()} == "value__") {
                            continue;
                        }

                        if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                            REVR_LOG(names += std::format("{}{}={}", names.empty() ? "" : ", ", f->get_name(), *d));
                        }
                    }

                    REVR_LOG(spdlog::info("[VR] RenderConfig.{} = {} ({}: {})", getter + 4, m->call<int32_t>(context, render_config), t->get_full_name(), names));
                } else if (t != nullptr && std::string_view{t->get_full_name()} == "System.Boolean") {
                    REVR_LOG(spdlog::info("[VR] RenderConfig.{} = {}", getter + 4, m->call<bool>(context, render_config)));
                }
            }

            return true;
        }();
        (void)window_mode_logged;
#endif

        if (upscale_off && set_upscale_type_method != nullptr) {
            const auto current = get_upscale_type_method->call<int32_t>(context, render_config);

            if (current != *upscale_off) {
                set_upscale_type_method->call<void*>(context, render_config, *upscale_off);
                REVR_LOG(spdlog::info("[VR] the game's upscaling turned off (was {})", current));
            }
        }

        if (get_image_quality_rate_method != nullptr && set_image_quality_rate_method != nullptr) {
            const auto rate = get_image_quality_rate_method->call<float>(context, render_config);

            if (rate != 1.0f) {
                set_image_quality_rate_method->call<void*>(context, render_config, 1.0f);
                REVR_LOG(spdlog::info("[VR] the game's render scale set to 1.0 (was {})", rate));
            }
        }

        // [GAME OPTIONS 12.11.2026] full screen off, the window in windowed mode: exclusive full screen loses the swapchain to
        // the headset's compositor and the game then leaves full screen and resizes in a loop (the D3D12 hook already keeps
        // the swapchain windowed, but the game keeps asking while its own option still says full screen)
        {
            static auto get_full_screen_method = find_setter({"get_FullScreenMode", "getFullScreenMode"});
            static auto set_full_screen_method = find_setter({"set_FullScreenMode", "setFullScreenMode"});
            static auto get_window_mode_method = find_setter({"get_WindowMode", "getWindowMode"});
            static auto set_window_mode_method = find_setter({"set_WindowMode", "setWindowMode"});

            static const bool full_screen_is_bool = [&]() {
                const auto* t = get_full_screen_method != nullptr ? get_full_screen_method->get_return_type() : nullptr;
                return t != nullptr && std::string_view{t->get_full_name()} == "System.Boolean";
            }();

            // the windowed value of the window mode enum, by name
            static const std::optional<int32_t> windowed = [&]() -> std::optional<int32_t> {
                const auto* t = get_window_mode_method != nullptr ? get_window_mode_method->get_return_type() : nullptr;

                if (t == nullptr || !t->is_enum()) {
                    return std::nullopt;
                }

                for (const auto* wanted : {"normal", "windowed", "window"}) {
                    for (auto* f : t->get_fields()) {
                        if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal()) {
                            continue;
                        }

                        std::string lower{f->get_name()};
                        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });

                        if (lower == wanted) {
                            if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                                REVR_LOG(spdlog::info("[VR] {} windowed value: {}={}", t->get_full_name(), f->get_name(), *d));
                                return *d;
                            }
                        }
                    }
                }

                REVR_LOG(spdlog::info("[VR] {}: no windowed value found", t->get_full_name()));
                return std::nullopt;
            }();

            // [WINDOW MODE fix103 20.11.2026] only while its option is on (off in RE4 by default: the game's own choice stays)
            const bool force_windowed = m_force_windowed->value();

            if (force_windowed && full_screen_is_bool && set_full_screen_method != nullptr && get_full_screen_method->call<bool>(context, render_config)) {
                set_full_screen_method->call<void*>(context, render_config, false);
                REVR_LOG(spdlog::info("[VR] the game's full screen turned off"));
            }

            if (force_windowed && windowed && set_window_mode_method != nullptr) {
                const auto current = get_window_mode_method->call<int32_t>(context, render_config);

                if (current != *windowed) {
                    set_window_mode_method->call<void*>(context, render_config, *windowed);
                    REVR_LOG(spdlog::info("[VR] the game's window mode set to windowed (was {})", current));
                }
            }
        }

        // depth of field, chromatic aberration, film grain: always off with the headset
        {
            struct Effect {
                sdk::REMethodDefinition* get;
                sdk::REMethodDefinition* set;
                const char* name;
            };

            static Effect effects[] = {
                {find_setter({"get_DepthOfFieldEnable", "get_DepthOfFiledEnable"}), find_setter({"set_DepthOfFieldEnable", "set_DepthOfFiledEnable"}), "depth of field"},
                {find_setter({"get_ChromaticAberration", "get_ChromaticAberrationEnable"}), find_setter({"set_ChromaticAberration", "set_ChromaticAberrationEnable"}), "chromatic aberration"},
                {find_setter({"get_FilmGrainEnable", "get_FilmGrain"}), find_setter({"set_FilmGrainEnable", "set_FilmGrain"}), "film grain"},
            };

            for (auto& e : effects) {
                if (e.get == nullptr || e.set == nullptr) {
                    continue;
                }

                if (e.get->call<bool>(context, render_config)) {
                    e.set->call<void*>(context, render_config, false);
                    REVR_LOG(spdlog::info("[VR] {} disabled", e.name));
                }
            }
        }

        // [FXAA 12.11.2026] the game's own anti-aliasing off (request of 12.11.2026: FXAA blurs the whole picture, and the
        // mod's temporal upscaler, FSR 4, already does the anti-aliasing of every view): the setting FXAA or FXAA + TAA
        // always becomes none, any other (SMAA, TAA) while the mod's upscaler runs (without it the picture keeps the game's
        // SMAA / TAA). The values by name: they differ between games. The via.render.FXAA components of the game camera and
        // of its clones are switched off by CameraDuplicator::force_fxaa_off.
        if (m_force_fxaa_off->value() && get_antialiasing_method != nullptr && set_antialiasing_method != nullptr) {
            struct AaValues {
                std::vector<int32_t> fxaa{};
                std::optional<int32_t> none{};
            };

            static const AaValues aa = [&]() {
                AaValues out{};
                const auto* t = get_antialiasing_method->get_return_type();

                if (t == nullptr || !t->is_enum()) {
                    return out;
                }

                std::string names{};

                for (auto* f : t->get_fields()) {
                    if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal() ||
                        std::string_view{f->get_name()} == "value__") {
                        continue;
                    }

                    const auto* d = (const int32_t*)f->get_init_data();

                    if (d == nullptr) {
                        continue;
                    }

                    std::string upper{f->get_name()};
                    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });
                    REVR_LOG(names += std::format("{}{}={}", names.empty() ? "" : ", ", f->get_name(), *d));

                    if (upper.find("FXAA") != std::string::npos) {
                        out.fxaa.push_back(*d);
                    } else if (!out.none && (upper == "NONE" || upper == "OFF" || upper == "DISABLE")) {
                        out.none = *d;
                    }
                }

                REVR_LOG(spdlog::info("[VR] {}: {} -> FXAA values {}, none {}", t->get_full_name(), names, out.fxaa.size(),
                    out.none ? std::to_string(*out.none) : std::string{"not found"}));
                return out;
            }();

            if (aa.none) {
                const auto current = get_antialiasing_method->call<int32_t>(context, render_config);
                const bool fxaa = std::find(aa.fxaa.begin(), aa.fxaa.end(), current) != aa.fxaa.end();
                const bool upscaler = TemporalUpscaler::get()->activated();

                if (current != *aa.none && (fxaa || upscaler)) {
                    set_antialiasing_method->call<void*>(context, render_config, *aa.none);
                    REVR_LOG(spdlog::info("[VR] the game's anti-aliasing turned off (was {}{})", current,
                        upscaler ? ", the mod's anti-aliasing does it" : ", FXAA"));
                }
            }
        }
    }

    // Causes crashes on D3D11. May be a performance detriment in VR with new rendering method.
    const auto is_new_rendering_method = get_rendering_technique() == VR::RenderingTechnique::MULTIPASS;

    if (!is_new_rendering_method && !is_sf6 && g_framework->get_renderer_type() == REFramework::RendererType::D3D12 && m_enable_asynchronous_rendering->value()) {
        if (get_delay_render_enable_method != nullptr && set_delay_render_enable_method != nullptr) {
            const auto is_delay_render_enabled = get_delay_render_enable_method->call<bool>(context);

            if (is_delay_render_enabled == true) {
                set_delay_render_enable_method->call<void*>(context, !m_enable_asynchronous_rendering->value());
                REVR_LOG(spdlog::info("[VR] Delay render modified"));
            }
        }
    } else if (is_sf6) {
        // Must be on in SF6 or left eye gets stuck
        if (set_delay_render_enable_method != nullptr) {
            set_delay_render_enable_method->call<void*>(context, true);
        }
    }

#ifdef RE7
    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_game_object = utility::re_component::get_game_object(camera);

    if (camera_game_object == nullptr) {
        return;
    }
    
    auto camera_transform = camera_game_object->transform;

    if (camera_transform == nullptr) {
        return;
    }

    auto get_type = [](std::string name) {
        auto tdef = sdk::find_type_definition(name);
        return tdef->get_type();
    };

    static auto effect_player_t = get_type("via.effect.EffectPlayer");
    static auto hide_effect_for_vr_t = get_type(game_namespace("EPVStandard.HideEffectForVR"));

    // Do not draw the game object if it is hidden for VR (this was there for the original PSVR release i guess)
    for (auto child = camera_transform->child; child != nullptr; child = child->child) {
        if (utility::re_component::find(child, effect_player_t) != nullptr) {
            if (utility::re_component::find(child, hide_effect_for_vr_t) != nullptr) {
                auto game_object = child->ownerGameObject;

                game_object->shouldDraw = false;
                //sdk::call_object_func<void*>(game_object, "set_Draw", sdk::get_thread_context(), game_object, false);
            }
        }
    }
#endif
}

void VR::fix_temporal_effects() {
    // this is SO DUMB!!!!!
    const auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }
    
    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput")->get_type();
    auto render_output_component = utility::re_component::find(camera, render_output_type);

    if (render_output_component == nullptr) {
        return;
    }

    if (!get_runtime()->ready() || m_disable_temporal_fix) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 0); // None
        return;
    }

    if (m_frame_count % 2 == m_left_eye_interval) {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 1); // left
    } else {
        sdk::call_object_func_easy<void*>(render_output_component, "set_DistortionType", 2); // right
    }
}

int32_t VR::get_frame_count() const {
    return get_game_frame_count();
}

int32_t VR::get_game_frame_count() const {
    static auto renderer_type = sdk::find_type_definition("via.render.Renderer");

    if (renderer_type == nullptr) {
        renderer_type = sdk::find_type_definition("via.render.Renderer");
        REVR_LOG(spdlog::warn("VR: Failed to find renderer type, trying again next time"));
        return 0;
    }

    auto renderer = renderer_type->get_instance();

    if (renderer == nullptr) {
        return 0;
    }

    static auto get_render_frame_method = renderer_type->get_method("get_RenderFrame");

    return get_render_frame_method->call<int32_t>(sdk::get_thread_context(), renderer);
}

float VR::get_standing_height() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

// [AUTO_RECENTER 2026-08-18] Scharfschalten. Das eigentliche Feuern passiert in
// update_hmd_state(), sobald die Runtime bereit ist UND die erste gueltige Pose da ist,
// fruehestens AUTO_RECENTER_DELAY nach diesem Aufruf. Direkt hier zu recentern waere
// wertlos: zu diesem Zeitpunkt liefert get_rotation(0) noch keine getrackte Pose.
void VR::arm_auto_recenter() {
    m_wants_auto_recenter = true;
    m_auto_recenter_armed_at = std::chrono::steady_clock::now();

    REVR_LOG(spdlog::info("[VR] Auto recenter armed"));
}

void VR::recenter_view() {
    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
    request_flatscreen_reanchor();   // [CANVAS_RECENTER]
}

glm::quat VR::get_gui_rotation_offset() {
    std::shared_lock _{ m_gui_mtx };

    return m_gui_rotation_offset;
}

void VR::set_gui_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_gui_mtx };

    m_gui_rotation_offset = offset;
}

void VR::recenter_gui(const glm::quat& from) {
    const auto new_gui_offset = glm::normalize(glm::inverse(utility::math::flatten(from)));
    set_gui_rotation_offset(new_gui_offset);
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    // [MONO PERIPHERY] the shared wide view sits between the eyes
    if (is_using_multipass() && m_multipass.pass == (uint32_t)CENTER_EYE) {
        return get_center_eye_transform()[3];
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (m_mono_rendering) {
        return apply_zoom_eye_offset(get_runtime()->eyes[m_mono_rendering_eye == 0 ? vr::Eye_Left : vr::Eye_Right])[3];
    }

    const auto count = is_using_multipass() ? m_multipass.pass : m_frame_count;

    if (count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

// [ZOOM_DEPTH 2026-08-14] Der Zoom vergroessert nicht nur das Bild, sondern auch die
// Parallaxe zwischen den Augen -- und genau daraus liest das Gehirn die Entfernung. Bei
// 2x Zoom wirkt der Augenabstand wie ein doppelter, das Ziel "rueckt naeher", und jede
// Zoomstufe verlangt neues Einstellen der Augen (im Spiel so beschrieben und gemessen).
// Ein echtes Fernglas loest das ueber eine kleinere Basis: Basis = IPD / Vergroesserung.
// Genau das machen wir hier, damit die wahrgenommene Tiefe ueber alle Stufen gleich bleibt.
Matrix4x4f VR::apply_zoom_eye_offset(Matrix4x4f eye) const {
    if (!m_zoom_shrinks_ipd->value()) {
        return eye;
    }

    const auto zoom = m_last_zoom_factor;

    if (zoom <= 1.0f) {
        return eye;
    }

    eye[3].x /= zoom;
    eye[3].y /= zoom;
    eye[3].z /= zoom;
    eye[3].w = 1.0f;

    return eye;
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // [VIEW RACE 28.10.2026] The pass is set by the render side as it updates one scene layer after the other. Game logic
    // asks for the camera's view at any moment in between (item names and markers laid out on the screen, aiming) and
    // got the left eye, the right eye or the head at random: the item names jumped sideways for single frames by the
    // shift between those viewpoints at the item's distance (~9.5 px at ~4 m, in both eyes at once; video of 27.10.2026
    // 22:01:35, frames 0, 2, 9, 12, 15). With the shared periphery game logic now always gets the head, between the eyes,
    // which is where the shared wide view is rendered from.
    if (is_mono_periphery() && quadview::State::get().engaged()) {
        auto& qs = quadview::State::get();
#if REVR_RELEASE
        constexpr int mode = 2; // [RELEASE SWITCH fix139] "Head's view for game logic": its third variant, fixed
#else
        const int mode = qs.head_view_mode.load(std::memory_order_relaxed);
#endif

        if (t_in_layer_update) {
            qs.views_in_layer.fetch_add(1, std::memory_order_relaxed);
        } else if (t_entry_thread) {
            qs.note_logic_entry(t_entry_name);

            if (mode >= 1) {
                qs.views_game_logic.fetch_add(1, std::memory_order_relaxed);
                return get_center_eye_transform();
            }

            qs.views_logic_kept.fetch_add(1, std::memory_order_relaxed);
        } else if (s_layer_updates_running.load(std::memory_order_relaxed) > 0) {
            qs.views_other_threads.fetch_add(1, std::memory_order_relaxed);

            if (qs.test_head_all_threads.load(std::memory_order_relaxed)) {
                return get_center_eye_transform(); // [FLICKER TEST 15.11.2026]
            }
        } else {
            qs.views_jobs_outside.fetch_add(1, std::memory_order_relaxed);

            if (mode >= 2) {
                qs.views_game_logic.fetch_add(1, std::memory_order_relaxed);
                return get_center_eye_transform();
            }
        }
    }

    // [MONO PERIPHERY] the shared wide view sits between the eyes (nothing to flip there)
    if (is_using_multipass() && m_multipass.pass == (uint32_t)CENTER_EYE) {
        return get_center_eye_transform();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (m_mono_rendering) {
        return apply_zoom_eye_offset(get_runtime()->eyes[m_mono_rendering_eye == 0 ? vr::Eye_Left : vr::Eye_Right]);
    }

    const auto count = is_using_multipass() ? m_multipass.pass : m_frame_count;
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Left]);
    }

    return apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Right]);
}

// [MONO PERIPHERY 27.10.2026] eye-to-head transform of the shared wide view: halfway between the two eyes (their
// zoomed offsets), with the left eye's orientation
Matrix4x4f VR::get_center_eye_transform() {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // [CPU 03.11.2026] once per thread and frame: game logic and the engine's jobs ask for it ~500 times per frame in fights
    // (log of 03.11.2026 15:40), each time through the eyes' reader lock; the eye-to-head transforms and the zoom offset hold
    // for the frame (the zoom factor is part of the key: it can change while the frame is laid out)
    thread_local int t_frame = 0;
    thread_local float t_zoom = 0.0f;
    thread_local bool t_valid = false;
    thread_local Matrix4x4f t_center{};
    const int frame = m_frame_count;
    const float zoom = m_last_zoom_factor;

    if (t_valid && t_frame == frame && t_zoom == zoom) {
        return t_center;
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    const auto left = apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Left]);
    const auto right = apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Right]);
    auto center = left;
    center[3] = (left[3] + right[3]) * 0.5f;
    center[3].w = 1.0f;
    t_center = center;
    t_frame = frame;
    t_zoom = zoom;
    t_valid = true;
    return center;
}

// [MONO PERIPHERY 27.10.2026] the smallest frustum holding both eyes' frusta, built on the left eye's projection (same
// handedness, near/far and tweaks). ndc = P00 * tan - P20 for these matrices, so tan(ndc) = (ndc + P20) / P00.
Matrix4x4f VR::get_union_projection_matrix() {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    const auto pl = get_projection_matrix(0);
    const auto pr = get_projection_matrix(1);

    if (std::abs(pl[0][0]) < 1e-6f || std::abs(pl[1][1]) < 1e-6f || std::abs(pr[0][0]) < 1e-6f || std::abs(pr[1][1]) < 1e-6f) {
        return pl;
    }

    const float l = (std::min)((-1.0f + pl[2][0]) / pl[0][0], (-1.0f + pr[2][0]) / pr[0][0]);
    const float r = (std::max)((1.0f + pl[2][0]) / pl[0][0], (1.0f + pr[2][0]) / pr[0][0]);
    const float b = (std::min)((-1.0f + pl[2][1]) / pl[1][1], (-1.0f + pr[2][1]) / pr[1][1]);
    const float t = (std::max)((1.0f + pl[2][1]) / pl[1][1], (1.0f + pr[2][1]) / pr[1][1]);

    // the union, as a rectangle in the left eye's NDC (it reaches past +1 on the right): the same sub-frustum math as the
    // fovea inset, only with a rectangle larger than the image
    quadview::NdcRect rect{};
    rect.x0 = pl[0][0] * l - pl[2][0];
    rect.x1 = pl[0][0] * r - pl[2][0];
    rect.y0 = pl[1][1] * b - pl[2][1];
    rect.y1 = pl[1][1] * t - pl[2][1];

    auto out = pl;
    quadview::apply_sub_frustum(out, quadview::sub_frustum(rect));
    return out;
}

// [MONO PERIPHERY 27.10.2026] geometry for the periphery reprojection (quadview::Frusta), once per frame
void VR::publish_frusta() {
    quadview::Frusta f{};

    if (!is_hmd_active()) {
        quadview::State::get().set_frusta(f);
        return;
    }

    const Matrix4x4f p[2]{get_projection_matrix(0), get_projection_matrix(1)};

    for (int e = 0; e < 2; ++e) {
        if (std::abs(p[e][0][0]) < 1e-6f || std::abs(p[e][1][1]) < 1e-6f) {
            quadview::State::get().set_frusta(f);
            return;
        }

        f.eye_l[e] = (-1.0f + p[e][2][0]) / p[e][0][0];
        f.eye_r[e] = (1.0f + p[e][2][0]) / p[e][0][0];
        f.eye_b[e] = (-1.0f + p[e][2][1]) / p[e][1][1];
        f.eye_t[e] = (1.0f + p[e][2][1]) / p[e][1][1];
    }

    f.wide_l = (std::min)(f.eye_l[0], f.eye_l[1]);
    f.wide_r = (std::max)(f.eye_r[0], f.eye_r[1]);
    f.wide_b = (std::min)(f.eye_b[0], f.eye_b[1]);
    f.wide_t = (std::max)(f.eye_t[0], f.eye_t[1]);

    {
        std::shared_lock _{get_runtime()->eyes_mtx};
        const auto left = apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Left]);
        const auto right = apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Right]);
        const float center_x = (left[3].x + right[3].x) * 0.5f;
        f.eye_x[0] = left[3].x - center_x;
        f.eye_x[1] = right[3].x - center_x;
    }

    f.valid = true;
    quadview::State::get().set_frusta(f);
}

// Scales the projection we already have instead of building a new one: handedness,
// near/far and the per-eye off-center asymmetry all stay exactly as the runtime made them.
Matrix4x4f VR::apply_projection_tweaks(Matrix4x4f proj) const {
    auto zoom = m_projection_zoom;

    // An explicit FOV wins - derive the factor from the projection itself, where
    // proj[1][1] is 1 / tan(vertical half fov).
    if (m_projection_fov > 0.0f) {
        const auto current = proj[1][1];

        if (current > 0.0f) {
            const auto target = glm::tan(glm::radians(m_projection_fov) * 0.5f);

            if (target > 0.0001f) {
                zoom = (1.0f / target) / current;
            }
        }
    }

    // [ZOOM_DEPTH] Faktor merken -- apply_zoom_eye_offset braucht ihn, und im FOV-Modus
    // kennt man ihn ohne die Projektion nicht.
    m_last_zoom_factor = zoom;

    if (zoom != 1.0f) {
        proj[0][0] *= zoom;
        proj[1][1] *= zoom;

        // [ZOOM_STEREO 2026-08-14] Die Off-Center-Terme NICHT mitskalieren.
        //
        // Frueher stand hier "sonst rutscht das Bild seitlich, statt auf denselben Punkt
        // zuzulaufen" -- das gilt fuers Einzelbild. Im Stereobild ist es genau falsch
        // herum: proj[2][0]/[2][1] sind pro Auge verschieden (das HMD-Frustum ist
        // asymmetrisch, links und rechts spiegelbildlich). Multipliziert man sie mit dem
        // Zoom, waechst der Versatz ZWISCHEN den Augen mit dem Zoomfaktor mit, der
        // binokulare Overlap wandert und man schielt -- im Spiel gemessen: je mehr Zoom,
        // desto schlimmer.
        //
        // So bleibt der Punkt geradeaus (x=0) fuer jedes Auge an seiner Bildstelle: beide
        // zoomen um IHRE eigene Blickachse und die Bilder bleiben fusionierbar.
        if (m_zoom_scales_eye_offset->value()) {
            proj[2][0] *= zoom;
            proj[2][1] *= zoom;
        }
    }

    proj[2][0] += m_projection_shift_x;
    proj[2][1] += m_projection_shift_y;

    // [2026-08-11] Hier stand der Bild-Versatz fuer OpenXR (proj[2][0] += shift * 2).
    // Wieder RAUS: die Projektion verschiebt die gerenderte WELT, waehrend das
    // Fadenkreuz als GUI-Element stehen bleibt -- der Treffpunkt wanderte damit gegen
    // das Fadenkreuz, und weil der Zoom oben dieselben Off-Center-Terme skaliert, war
    // der Versatz zusaetzlich zoomabhaengig. Unter OpenVR verschiebt der Regler den
    // AUSSCHNITT des fertigen Bildes (get_shifted_bounds), nicht die Welt. Genau das
    // macht XR jetzt auch, als versetzte Kopie in die Swapchain
    // (D3D12Component::OpenXR::copy, Abschnitt IMAGE_SHIFT).

    return proj;
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // [MONO PERIPHERY] the shared wide view
    if (is_using_multipass() && m_multipass.pass == (uint32_t)CENTER_EYE) {
        return get_union_projection_matrix();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    // Mono only overrides the projection when explicitly asked for. Each display must
    // normally keep its own off-center frustum, otherwise the two identical images sit
    // laterally offset and the eyes cannot fuse them.
    if (m_mono_rendering && m_mono_projection) {
        return apply_projection_tweaks(get_runtime()->projections[m_mono_rendering_eye == 0 ? (uint32_t)VRRuntime::Eye::LEFT : (uint32_t)VRRuntime::Eye::RIGHT]);
    }

    const auto count = is_using_multipass() ? m_multipass.pass : m_frame_count;
    const auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (count % 2 == mod_count) {
        return apply_projection_tweaks(get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT]);
    }

    return apply_projection_tweaks(get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT]);
}

Matrix4x4f VR::get_projection_matrix(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // [MONO PERIPHERY] the shared wide view
    if (pass == (uint32_t)CENTER_EYE && is_using_multipass()) {
        return get_union_projection_matrix();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    // See get_current_projection_matrix: per-eye frustum stays unless mono projection is on.
    if (m_mono_rendering && m_mono_projection) {
        return apply_projection_tweaks(get_runtime()->projections[m_mono_rendering_eye == 0 ? (uint32_t)VRRuntime::Eye::LEFT : (uint32_t)VRRuntime::Eye::RIGHT]);
    }

    if (pass % 2 == 0) {
        return apply_projection_tweaks(get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT]);
    }

    return apply_projection_tweaks(get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT]);
}

// for the foveated rendering log: what the runtime says about the two eyes (a rotation in the eye-to-head transform would
// turn each eye's view by a constant angle)
std::string VR::describe_eyes() const {
    if (!is_hmd_active() || get_runtime() == nullptr) {
        return "  eyes: no headset\n";
    }

    std::shared_lock _{get_runtime()->eyes_mtx};
    std::string o{};

    for (int eye = 0; eye < 2; ++eye) {
        const auto& e = get_runtime()->eyes[eye];
        const auto& p = get_runtime()->projections[eye];
        const auto& raw = get_runtime()->raw_projections[eye];
        // yaw / pitch / roll of the eye's axes in head space (degrees)
        const float yaw = glm::degrees(std::atan2(e[2].x, e[2].z));
        const float pitch = glm::degrees(std::asin(std::clamp(-e[2].y, -1.0f, 1.0f)));
        const float roll = glm::degrees(std::atan2(e[0].y, e[1].y));

        o += std::format("  eye{} to head: offset=({:.4f},{:.4f},{:.4f}) m yaw={:.3f} pitch={:.3f} roll={:.3f} deg | projection P00={:.4f} P11={:.4f} "
                         "P20={:.4f} P21={:.4f} | raw l={:.4f} r={:.4f} t={:.4f} b={:.4f}\n",
            eye, e[3].x, e[3].y, e[3].z, yaw, pitch, roll, p[0][0], p[1][1], p[2][0], p[2][1], raw.x, raw.y, raw.z, raw.w);
    }

    return o;
}

Matrix4x4f VR::get_eye_transform(uint32_t pass) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    // [MONO PERIPHERY] the shared wide view
    if (pass == (uint32_t)CENTER_EYE && is_using_multipass()) {
        return get_center_eye_transform();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if (m_mono_rendering) {
        return apply_zoom_eye_offset(get_runtime()->eyes[m_mono_rendering_eye == 0 ? vr::Eye_Left : vr::Eye_Right]);
    }

    if (pass % 2 == 0) {
        return apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Left]);
    }

    return apply_zoom_eye_offset(get_runtime()->eyes[vr::Eye_Right]);
}

// [XR_UI_POINTER 2026-08-14] Laeuft direkt nach ImGui::NewFrame (REFramework::call_on_frame).
// Nur hier darf in eine DrawList gezeichnet werden -- OverlayComponent setzt dort den roten
// Zeigerpunkt aufs Menue.
void VR::on_frame() {
    if (!get_runtime()->ready()) {
        return;
    }

    m_overlay_component.on_frame();
}

void VR::on_pre_imgui_frame() {
    REF_PROFILE_FUNCTION();

    if (!get_runtime()->ready()) {
        return;
    }

    m_overlay_component.on_pre_imgui_frame();
}

glm::mat4 to_reverseZ(const glm::mat4& proj) {

	glm::mat4 transformMat = glm::mat4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, -1,0,
		0, 0, 1, 1
	);

    return transformMat * proj;
}

void VR::on_present() {
    REF_PROFILE_FUNCTION();

    if (is_using_multipass() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;
            openvr->is_hmd_active = true;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    // [TECHNIQUE fix91 19.11.2026] The numpad switches that were here are gone: 7 (Multipass / AFW) and 8 (all four
    // techniques) changed the rendering technique outside the menu, 6 and 9 toggled AFW's FramewarpDebug and EnableUIFix.

    // [COMPOSITOR LOCK 05.11.2026] Submit, the overlays and PostPresentHandoff not while the main thread is in WaitGetPoses
    // (runtimes::OpenVR::compositor_mtx); held to the end of the present callback
    std::unique_lock<std::recursive_timed_mutex> compositor_lock{};

    if (runtime->is_openvr() && openvr != nullptr) {
        // (at most 50 ms: WaitGetPoses waits for the headset's frame timing, a few ms; longer means something else holds it)
        compositor_lock = runtimes::lock_compositor(*openvr, true, std::chrono::milliseconds(50));
    }

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (renderer == REFramework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            runtime->synchronize_frame();
            runtime->update_poses();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == REFramework::RendererType::D3D12) {
        m_is_d3d12 = true;
#if REVR_RELEASE
        e = m_d3d12.on_frame(this); // [RELEASE SWITCH fix141] not timed in the release
#else
        // [SUBMIT 05.11.2026] the D3D12 component's frame (copies, conversion, the compositor's Submit calls), timed
        const auto t0 = cpustats::now_ns();
        e = m_d3d12.on_frame(this);
        auto& vs = cpustats::vr_submit();
        vs.component_ns.fetch_add((uint64_t)(std::max)(cpustats::now_ns() - t0, (int64_t)0), std::memory_order_relaxed);
        vs.component_frames.fetch_add(1, std::memory_order_relaxed);
        d3d12::SubmitTimeline::get().marker("mark: after the headset frame"); // [SUBMIT TIMELINE 06.11.2026 evening]
#endif
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    m_last_frame_count = m_render_frame_count;

    if (m_submitted || runtime->needs_pose_update) {
        if (m_submitted) {
            m_overlay_component.on_post_compositor_submit();

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
#if REVR_RELEASE
                vr::VRCompositor()->PostPresentHandoff(); // [RELEASE SWITCH fix141] not timed in the release
#else
                const auto t0 = cpustats::now_ns(); // [SUBMIT 05.11.2026]
                vr::VRCompositor()->PostPresentHandoff();
                auto& vs = cpustats::vr_submit();
                vs.handoff_ns.fetch_add((uint64_t)(std::max)(cpustats::now_ns() - t0, (int64_t)0), std::memory_order_relaxed);
                vs.handoffs.fetch_add(1, std::memory_order_relaxed);
                d3d12::SubmitTimeline::get().marker("mark: after PostPresentHandoff"); // [SUBMIT TIMELINE 06.11.2026 evening]
#endif
            }
        }

        runtime->needs_pose_update = true;
        m_submitted = false;
    }

    // [FRAME TIMING 05.11.2026] SteamVR's timing of the last finished frame (cpustats::VrTiming)
    if (runtime->is_openvr() && vr::VRCompositor() != nullptr) {
        vr::Compositor_FrameTiming ft{};
        ft.m_nSize = sizeof(ft);

        if (vr::VRCompositor()->GetFrameTiming(&ft, 1)) {
            auto& vt = cpustats::vr_timing();

            if (ft.m_nFrameIndex != vt.last_index.exchange(ft.m_nFrameIndex, std::memory_order_relaxed)) {
                const auto total_us = (uint64_t)(std::max)(ft.m_flTotalRenderGpuMs * 1000.0f, 0.0f);
                vt.frames.fetch_add(1, std::memory_order_relaxed);
                vt.gpu_total_us.fetch_add(total_us, std::memory_order_relaxed);
                cpustats::store_max(vt.gpu_total_max_us, total_us);
                vt.gpu_compositor_us.fetch_add((uint64_t)(std::max)(ft.m_flCompositorRenderGpuMs * 1000.0f, 0.0f), std::memory_order_relaxed);

                if (total_us > vt.frame_us.load(std::memory_order_relaxed)) {
                    vt.gpu_over.fetch_add(1, std::memory_order_relaxed);
                }

                if ((ft.m_nReprojectionFlags & vr::VRCompositor_ReprojectionReason_Gpu) != 0) {
                    vt.late_gpu.fetch_add(1, std::memory_order_relaxed);
                }

                if ((ft.m_nReprojectionFlags & vr::VRCompositor_ReprojectionReason_Cpu) != 0) {
                    vt.late_cpu.fetch_add(1, std::memory_order_relaxed);
                }

                if (VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES(ft) > 0) {
                    vt.throttled.fetch_add(1, std::memory_order_relaxed);
                }

                vt.mispresented.fetch_add(ft.m_nNumMisPresented, std::memory_order_relaxed);
                vt.dropped.fetch_add(ft.m_nNumDroppedFrames, std::memory_order_relaxed);
            }
        }
    }

    if (is_using_multipass() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        SetEvent(m_present_finished_event);
    }
}

void VR::on_post_present() {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    const auto renderer = g_framework->get_renderer_type();

    if (renderer == REFramework::RendererType::D3D12) {
        m_d3d12.on_post_present(this);
    }
    
    if (is_using_multipass() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
        runtime->consume_events(nullptr);
    }

    const auto is_left_eye_frame = is_using_multipass() ? true : ((m_render_frame_count) % 2 == m_left_eye_interval);

    if ((is_left_eye_frame)) {
        if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !runtime->got_first_sync) {
            const auto had_sync = runtime->got_first_sync;
            runtime->synchronize_frame();

            if (!runtime->got_first_poses || !had_sync) {
                update_hmd_state();
            }
        }

        if (runtime->is_openxr() && runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            if (!m_openxr->frame_began) {
                m_openxr->begin_frame();
            }
        }
    }

    if (!inside_on_end && runtime->wants_reinitialize) {
        std::scoped_lock _{m_openvr_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

void VR::on_update_transform(RETransform* transform) {
    
}

void VR::on_update_camera_controller(RopewayPlayerCameraController* controller) {
    // get headset rotation
    /*const auto& headset_pose = m_game_poses[0];

    if (!headset_pose.bPoseIsValid) {
        return;
    }

    auto headset_matrix = Matrix4x4f{ *(Matrix3x4f*)&headset_pose.mDeviceToAbsoluteTracking };
    auto headset_rotation = glm::extractMatrixRotation(glm::rowMajor4(headset_matrix));

    headset_rotation *= get_current_rotation_offset();

    *(glm::quat*)&controller->worldRotation = glm::quat{ headset_rotation  };

    controller->worldPosition += get_current_offset();*/
}

struct GUIRestoreData {
    REComponent* element{nullptr};
    REComponent* view{nullptr};
    Vector4f original_position{ 0.0f, 0.0f, 0.0f, 1.0f };
    via::gui::ViewType view_type{ via::gui::ViewType::Screen };
    bool overlay{false};
    bool detonemap{true};
    std::vector<REManagedObject*> hidden_fx{}; // [GOLD-FX] nur fuer diesen Frame ausgeblendet
    // [ANCHOR FINAL 04.11.2026] the element's GUI anchor as the VR mod placed it, added in on_gui_draw_element with the
    // position it was drawn at: a mod after the VR mod may move it (RE4VRCrosshair: the weapon HUD to the hand, the
    // reticle to the aim point), and outside the fovea it has to reach each eye at that depth, not at the panels'
    bool has_anchor{false};
    quadview::GuiAnchor anchor{};
    uint64_t anchor_frame{0};
    Vector4f anchor_camera{ 0.0f, 0.0f, 0.0f, 1.0f };
};

thread_local std::vector<std::unique_ptr<GUIRestoreData>> g_elements_to_reset{};

// ----------------------------------------------------------------------------
// [GOLD-FX 29.09.2026] Gold-Partikel im Inlay-Menue ziehen in VR bleibende Spuren
// ----------------------------------------------------------------------------
// Im Edelstein-Inlay (Gui_ui3090) steigen Gold-Partikel auf; in VR hinterlassen sie
// Schlieren, die nie vergehen und im Kapitelende (Gui_ui5000) im selben Zustand
// wieder auftauchen. Flach sauber; auch mit der Installer-DLL, AFW wie Multipass,
// FSR2/FSR4/aus, Proton 10/11 -- also ein Upstream-Fehler, kein Proton-Thema.
//
// Ursache (Diagnose 28./29.09., Patch perf-logs/dlls/gui-diag-inlay-trails.patch):
// via.gui.Effect2D zeichnet die Partikel in Effekt-Ziel 4 = Overlay.UITargetOnly,
// und das ist DIESELBE Textur wie UITarget, also das ganze Bild. Sichtbar werden
// sie nur ueber via.gui.Effect2DTexture (Target 4), das diese Textur auf das Panel
// legt. Flach baut der Overlay-Layer das Bild jeden Frame neu; VR ueberspringt
// den Layer (on_pre_overlay_layer_draw), sonst sitzt jede GUI je Auge versetzt.
// So wird nie geleert und die Partikel stapeln sich.
// Verworfen: Layer laufen lassen (GUI-Versatz, auch ohne Overlay-Flag und ohne
// Engine-HMD-Verzerrung), Ziel leeren (ganzes 3D schwarz), Ziel umbiegen ueber
// Slot-Array/Feld/set_Target (wirkungslos, das Ziel steht ab Effekt-Aufbau fest).
//
// Daher: in VR Effect2D/Effect2DTexture mit Target 4 fuer die Dauer des Zeichnens
// ausblenden (Option "Gold-Partikel in Menues ausblenden", Standard an). Andere
// Effekte bleiben. Pro GUI wird der Baum nur beim ersten Mal / bei neuer View /
// alle 300 Zeichnungen durchsucht (Inlay hat ~1000 Controls); die gefundenen
// Controls werden per add_ref festgehalten, damit der Cache nie auf Freigegebenes zeigt.
namespace gold_fx {
struct Entry {
    REComponent* view{};
    std::vector<REManagedObject*> controls{}; // add_ref gehalten
    uint32_t draws{};
};

std::mutex g_mtx{};
std::unordered_map<size_t, Entry> g_cache{};

template <typename F>
void walk(REManagedObject* obj, F&& fn, int depth = 0) {
    auto context = sdk::get_thread_context();
    int count = 0;

    for (auto node = obj; node != nullptr && count < 512; ++count) {
        fn(node);

        if (depth < 16) {
            if (auto child = sdk::call_object_func<REManagedObject*>(node, "get_Child", context, node); child != nullptr) {
                walk(child, fn, depth + 1);
            }
        }

        node = sdk::call_object_func<REManagedObject*>(node, "get_Next", context, node);
    }
}

void rescan(Entry& e, REComponent* view) {
    for (auto c : e.controls) {
        utility::re_managed_object::release(c);
    }

    e.controls.clear();
    e.view = view;
    e.draws = 0;

    auto context = sdk::get_thread_context();

    walk((REManagedObject*)view, [&](REManagedObject* node) {
        if (!utility::re_managed_object::is_a(node, "via.gui.Effect2D") &&
            !utility::re_managed_object::is_a(node, "via.gui.Effect2DTexture")) {
            return;
        }

        if (sdk::call_object_func<int32_t>(node, "get_Target", context, node) == 4) {
            utility::re_managed_object::add_ref(node);
            e.controls.push_back(node);
        }
    });
}

void hide(size_t name_hash, std::string_view name, REComponent* view, std::vector<REManagedObject*>& out) {
    std::scoped_lock _{g_mtx};
    auto& e = g_cache[name_hash];

    if (e.view != view || e.draws >= 300) {
        // [GOLD-FX] Logzeile des Patches bewusst weggelassen (Fork schreibt keine Logs).
        rescan(e, view);
    }

    ++e.draws;

    auto context = sdk::get_thread_context();

    for (auto c : e.controls) {
        if (sdk::call_object_func<bool>(c, "get_Visible", context, c)) {
            sdk::call_object_func<void*>(c, "set_Visible", context, c, false);
            out.push_back(c);
        }
    }
}
}

// [RE4 MAP GLUE 2026-08-10] Die GUIs der Karte, am 2026-08-10 bei offener Karte gemessen:
//   3100 Navigationskreuz, 3101 Funde, 3103, 3104 Karte, 3120/3121 Karten-Ebenen.
// 3120 ist die einzige, die als ViewType World hereinkommt -- alle anderen sind Screen.
// Explizite Liste, kein Praefix-Raten.
// [GLUE-LISTE 2026-08-10] Welche GUIs am Kopf gepinnt werden, steht NICHT mehr im Code,
// sondern in m_glue_guis (Name-Hash -> Reihenfolge). Ein Script kann die Liste jederzeit
// umbauen, ein neuer Problem-Fall braucht also kein Compile mehr.
//
// Die Reihenfolge staffelt die Ebenen um `layer_gap` nach vorn: 0 = am weitesten hinten.
// Notwendig, weil bei exakt gleicher Distanz nur noch die Zeichenreihenfolge entscheidet
// und eine deckende Ebene die darunter schluckt -- gemessen am 2026-08-10, da war der
// Kartenumriss weg. 2 mm reichen; bei 1,5 m sind das 0,13 % Groesse, also unsichtbar.
//
// Vorbelegung = die sechs Karten-GUIs: Kartenkoerper hinten, Umriss darueber, dann die
// Funde, das Navigationskreuz ganz vorn.
void VR::reset_glue_guis() {
    std::scoped_lock _{m_glue_guis_mutex};

    m_glue_guis_initialized = true;
    // Cast, weil "..."_fnv hier size_t liefert und der Schluessel uint32_t ist (C4267).
    m_glue_guis = {
        { (uint32_t)"Gui_ui3120"_fnv, 0 },   // Karten-Ebene
        { (uint32_t)"Gui_ui3121"_fnv, 1 },   // Karte + Dekoration
        { (uint32_t)"Gui_ui3104"_fnv, 2 },   // Kartenumriss
        { (uint32_t)"Gui_ui3103"_fnv, 3 },
        { (uint32_t)"Gui_ui3101"_fnv, 4 },   // Funde
        { (uint32_t)"Gui_ui3100"_fnv, 5 },   // Navigationskreuz
    };
}

bool VR::is_glue_gui(uint32_t name_hash, int* order_out) {
    std::scoped_lock _{m_glue_guis_mutex};

    // Erstzugriff: Karten-GUIs vorbelegen. Ein bewusstes clear_glue_guis() setzt das Flag
    // ebenfalls, eine absichtlich geleerte Liste bleibt also leer.
    if (!m_glue_guis_initialized) {
        m_glue_guis_initialized = true;
        m_glue_guis = {
            { (uint32_t)"Gui_ui3120"_fnv, 0 },
            { (uint32_t)"Gui_ui3121"_fnv, 1 },
            { (uint32_t)"Gui_ui3104"_fnv, 2 },
            { (uint32_t)"Gui_ui3103"_fnv, 3 },
            { (uint32_t)"Gui_ui3101"_fnv, 4 },
            { (uint32_t)"Gui_ui3100"_fnv, 5 },
        };
    }

    const auto it = m_glue_guis.find(name_hash);

    if (it == m_glue_guis.end()) {
        return false;
    }

    if (order_out != nullptr) {
        *order_out = it->second;
    }

    return true;
}

void VR::set_glue_gui_hash(uint32_t name_hash, int order) {
    std::scoped_lock _{m_glue_guis_mutex};
    m_glue_guis_initialized = true;
    m_glue_guis[name_hash] = order;
}

void VR::remove_glue_gui_hash(uint32_t name_hash) {
    std::scoped_lock _{m_glue_guis_mutex};
    m_glue_guis_initialized = true;
    m_glue_guis.erase(name_hash);
}

void VR::clear_glue_guis() {
    std::scoped_lock _{m_glue_guis_mutex};
    m_glue_guis_initialized = true;
    m_glue_guis.clear();
}

size_t VR::get_glue_gui_count() {
    std::scoped_lock _{m_glue_guis_mutex};
    return m_glue_guis.size();
}

#if defined(RE7)
// [MENU SHEET 14.11.2026] transform helpers for the options sheet (VR::apply_menu_sheet)
namespace {
bool menu_sheet_same_matrix(const Matrix4x4f& a, const Matrix4x4f& b) {
    for (int c = 0; c < 4; ++c) {
        if (glm::length(a[c] - b[c]) > 1e-5f * (1.0f + glm::length(b[c]))) {
            return false;
        }
    }

    return true;
}

::RETransform* menu_sheet_parent(::RETransform* t) {
    static auto* get_parent = sdk::find_type_definition("via.Transform")->get_method("get_Parent");
    return t != nullptr && get_parent != nullptr ? get_parent->call<::RETransform*>(sdk::get_thread_context(), t) : nullptr;
}

std::string menu_sheet_chain(::RETransform* t) {
    std::string out{};

    for (auto* p = menu_sheet_parent(t); p != nullptr && out.size() < 300; p = menu_sheet_parent(p)) {
        out += " < ";
        out += p->ownerGameObject != nullptr ? utility::narrow(utility::re_string::get_view(p->ownerGameObject->name)) : std::string{"?"};
    }

    return out.empty() ? std::string{" (no parent)"} : out;
}

Vector4f menu_sheet_local_scale(::RETransform* t) {
    static auto* m = sdk::find_type_definition("via.Transform")->get_method("get_LocalScale");
    __declspec(align(16)) Vector4f out{1.0f, 1.0f, 1.0f, 0.0f};

    if (m != nullptr) {
        m->call<Vector4f*>(&out, sdk::get_thread_context(), t);
    }

    return out;
}

void menu_sheet_set_local_scale(::RETransform* t, const Vector4f& s) {
    static auto* m = sdk::find_type_definition("via.Transform")->get_method("set_LocalScale");
    __declspec(align(16)) Vector4f in{s.x, s.y, s.z, 0.0f};

    if (m != nullptr) {
        m->call<void*>(sdk::get_thread_context(), t, &in);
    }
}
} // namespace
#endif

#if defined(RE4)
// [CRAFT fix128 22.11.2026] The item the attache case's "Crea" page shows stands a little farther from the eyes (asked
// for by the user: about 10 cm, for every item of that page, also those the game gives later).
// What it is: the probe of fix127 (REFramework's log of 22.11.2026 20:35) listed the objects in front of the camera while
// the case's menus were open: the case itself (AC_RootObject, 0.68 m), the key items' root (KeyTreasure_RootObject, 1 m),
// the case's effects (AttacheCaseEffectRoot, 1.33 m) and CraftWindow_RootObject, 0.150 m straight in front of the
// camera, with the component chainsaw.CraftWindowRootObjectController. That object is the root the page hangs its item
// model on, whatever the item: moving the root moves all of them, and nothing else carries that controller.
// How: before the scene is locked for the frame (the game's own update has placed the root by then), while one of the
// case's GUI elements was drawn in the last 0.3 s, the object of that name with that component is moved along the
// camera's view by the user's distance. The place written is remembered: a root that still stands there (the game did
// not place it again) is not moved a second time.
namespace {
std::atomic<int64_t> s_case_gui_ns{0}; // the last time one of the case's GUI elements was drawn

// [MERCHANT fix156 24.11.2026] the merchant's shop as a game menu for the large-scale light's pause, from the game's state and
// not from a GUI name (the shop's own GUIs were never in a log: the user cannot reach the merchant now). re4.exe's type
// database has chainsaw.InGameShopManager with the property IsShopFlowBusy (with FlowController, CurrSpinelCount): while
// it is true the shop is open. Read on the main thread every 6th frame (one managed call); a missing type, method or
// singleton reads as closed. The case's own pages inside the shop are menus by their names already (Gui_ui3001 - 3006).
void shop_menu_probe(int32_t frame) {
    static uint32_t s_tick = 0;

    if (++s_tick % 6u != 0u) {
        return;
    }

    static auto* const t = sdk::find_type_definition("chainsaw.InGameShopManager");
    static auto* const busy_method = t != nullptr ? t->get_method("get_IsShopFlowBusy") : nullptr;

    if (busy_method == nullptr) {
        return;
    }

    auto* const manager = sdk::get_managed_singleton<::REManagedObject>("chainsaw.InGameShopManager");
    const bool busy = manager != nullptr && busy_method->call<bool>(sdk::get_thread_context(), manager);

#if !REVR_RELEASE // debug build: when the shop opens and closes, with the log's time
    static bool s_was = false;

    if (busy != s_was) {
        s_was = busy;
        spdlog::info("[VR] merchant's shop {} (chainsaw.InGameShopManager.IsShopFlowBusy)", busy ? "open: the large-scale light pauses" : "closed");
    }
#endif

    if (busy) {
        quadview::State::get().menu_gui_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        quadview::State::get().menu_gui_frame.store(frame);
    }
}

void craft_window_push() {
    static RETransform* s_moved = nullptr;
    static Vector3f s_written{};
    const float push = std::clamp(VR::get()->craft_push_m(), 0.0f, 1.0f); // [CRAFT fix159] the option of MOD OPTIONS
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();

    if (!(push > 0.001f) || now - s_case_gui_ns.load(std::memory_order_relaxed) > 300'000'000ll) {
        s_moved = nullptr;
        return;
    }

    auto scene = sdk::get_current_scene();
    auto camera = sdk::get_primary_camera();
    auto camera_object = camera != nullptr ? utility::re_component::get_game_object(camera) : nullptr;

    if (scene == nullptr || camera_object == nullptr || camera_object->transform == nullptr) {
        return;
    }

    auto* name = sdk::VM::create_managed_string(L"CraftWindow_RootObject");

    if (name == nullptr) {
        return;
    }

    auto* found = re4vr::call_safe<::REManagedObject*>((::REManagedObject*)scene, "findGameObject(System.String)", name);
    auto* object = (REGameObject*)found;

    if (object == nullptr || object->transform == nullptr) {
        s_moved = nullptr;
        return;
    }

    // only the craft page's root: the object has to carry the page's controller
    bool is_craft_root = false;

    for (auto c = object->transform->childComponent; c != nullptr && c != object->transform; c = c->childComponent) {
        if (const auto* td = utility::re_managed_object::get_type_definition(c); td != nullptr && td->get_full_name() == "chainsaw.CraftWindowRootObjectController") {
            is_craft_root = true;
            break;
        }
    }

    if (!is_craft_root) {
        s_moved = nullptr;
        return;
    }

    const Vector3f at{sdk::get_transform_position(object->transform)};

    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
        return;
    }

    // still where this put it: the game has not placed it again
    if (s_moved == object->transform && glm::length(at - s_written) < 0.0005f) {
        return;
    }

    // (the camera looks along its -z)
    const Vector3f forward = -glm::normalize(Vector3f{camera_object->transform->worldTransform[2]});

    if (!std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z)) {
        return;
    }

    const Vector3f target = at + forward * push;
    sdk::set_transform_position(object->transform, Vector4f{target, 1.0f});
    s_moved = object->transform;
    s_written = target;

    static bool once = true;

    if (once) {
        once = false;
        REVR_LOG(spdlog::info("[VR] the craft page's item root (CraftWindow_RootObject) is moved {:.2f} m farther along the view", push));
    }
}
} // namespace
#endif

#if defined(RE2)
namespace {
// [MENU LIGHT RE2 fix152] RE2's game menus (GUI object names, see VR::on_pre_gui_draw_element)
// [fix153 24.11.2026] from the debug log of the user's RE2 session of 24.11.2026 21:18 - 21:30 (title and its pages, new
// game, inventory, files, map, item box, pause). Not menus: the fades, TitleFlow, GUI_TitleL, GUI_GeneralGuide (the button
// guide), the loading and save icons, the movie, subtitles, GUI_FloatIcon, GUI_Purpose, GUI_Shortcut, GUI_Tutorial,
// GUI_PillarBox, GUI_Reticle, GUI_RemainingBullet, the floor maps' own names (gui_floormap_*: GUI_Map is drawn with them).
// [fix154] the second session (21:55 - 22:07): the typewriter (GUI_Save), game over, the extras' records (GUI_RogueRecord),
// an item examined (GuiSearch). Not menus: GUI_VitalHud, GUI_DangerScreen, the gimmicks' screens (sm*).
constexpr size_t RE2_MENU_GUIS[] = {
    // title and its pages
    "GUI_TitleMenu"_fnv, "GUI_MenuMain"_fnv, "GUI_MenuStory"_fnv, "GUI_MenuScenario"_fnv, "GUI_SelectBackground"_fnv,
    "GUI_Costume"_fnv, "GUI_Load"_fnv, "GUI_Bonus"_fnv, "GUI_Record"_fnv, "GUI_ConceptArt"_fnv, "GUI_ConceptArtDetail"_fnv,
    "GUI_Figure"_fnv, "GUI_FigureDetail"_fnv, "GUI_Option"_fnv, "GUI_OptionResult"_fnv,
    // questions
    "GUI_GeneralDialog"_fnv,
    // inventory (drawn together)
    "GuiCaption"_fnv, "GuiBack"_fnv, "GuiFront"_fnv, "GuiVital"_fnv, "GuiTab"_fnv, "GuiSlot"_fnv,
    // files, map, item box, pause
    "GUI_FileList"_fnv, "GUI_File"_fnv, "GUI_Map"_fnv, "GUI_MapBG"_fnv, "GUI_MapGrid"_fnv, "GUI_ItemBox"_fnv, "GUI_Pause"_fnv,
    // [fix154] typewriter, game over, extras' records, an item examined
    "GUI_Save"_fnv, "GUI_GameOver"_fnv, "GUI_RogueRecord"_fnv, "GuiSearch"_fnv,
    // [fix157] The Ghost Survivors' inventory: these object names are in re2.exe itself (never in a log yet)
    "GUI_RogueBack"_fnv, "GUI_RogueCaption"_fnv, "GUI_RogueItemSlot"_fnv, "GUI_RogueSearch"_fnv,
};
} // namespace
#endif

#if defined(RE2) || defined(RE7)
namespace {
// [MENU TYPES fix157 24.11.2026] the menus that were never opened in a debug session (so their object names are in no log)
// are found by type: each menu's GUI object carries the menu's own component (RE2: app.ropeway.gui.<name>Behavior, RE7:
// app.<name>, e.g. the object InventoryMenu with app.InventoryMenu, PauseMenuGUI with app.PauseMenu). The type names below
// are in the games' exe files (type database strings of re2.exe / re7.exe); only menus that cover the scene are listed, not
// the HUD, the icons, the subtitles, the fades, the in-world screens (gimmicks, floor maps) or the loading. The name lists
// stay first; the components are walked once per object name (cached), not at each draw.
#if defined(RE2)
constexpr size_t MENU_GUI_TYPES[] = {
    // title, its pages and the first start's settings
    "TitleMenuBehavior"_fnv, "MenuMainBehavior"_fnv, "MenuStoryBehavior"_fnv, "MenuScenarioBehavior"_fnv,
    "SelectBackgroundBehavior"_fnv, "SelectDifficultyBehavior"_fnv, "CostumeBehavior"_fnv, "LoadBehavior"_fnv,
    "SaveLoadBaseBehavior"_fnv, "BonusBehavior"_fnv, "RecordBehavior"_fnv, "OpenedRecordsBehavior"_fnv,
    "ConceptArtBehavior"_fnv, "ConceptArtDetailBehavior"_fnv, "FigureBehavior"_fnv, "FigureDetailBehavior"_fnv,
    "OptionBehavior"_fnv, "OptionPresetBehavior"_fnv, "OptionResultBehavior"_fnv, "KeyBindBehavior"_fnv,
    "ScreenSettingBehavior"_fnv, "SelectLanguageBehavior"_fnv, "SelectBrightnessBehavior"_fnv,
    "SelectHDRBrightnessBehavior"_fnv, "SelectCameraBehavior"_fnv, "SelectControlTypeBehavior"_fnv,
    "FirstGraphicSettingGuiBehavior"_fnv, "PrivacyPolicyBehavior"_fnv, "ReNetSettingBehavior"_fnv, "ReNetDetailBehavior"_fnv,
    "DialogBehavior"_fnv,
    // in the game: pause, typewriter, game over, the results after a scenario
    "PauseBehavior"_fnv, "SaveBehavior"_fnv, "GameOverBehavior"_fnv, "ResultBehavior"_fnv, "ResultExtraBehavior"_fnv,
    "ClearStateBehavior"_fnv,
    // inventory, files, map, item box and the item puzzles examined in the inventory (portable safe, ...)
    "NewInventoryBehavior"_fnv, "NewInventoryBackBehavior"_fnv, "NewInventoryCaptionBehavior"_fnv,
    "NewInventoryDetailBehavior"_fnv, "NewInventoryFrontBehavior"_fnv, "NewInventorySlotBehavior"_fnv,
    "NewInventoryTabBehavior"_fnv, "NewInventoryVitalBehavior"_fnv, "FileBehavior"_fnv, "FileListBehavior"_fnv,
    "MapBehavior"_fnv, "MapBgBehavior"_fnv, "MapGridBehavior"_fnv, "ItemBoxBehavior"_fnv, "ItemLockerBehavior"_fnv,
    "ItemPuzzleBehavior"_fnv, "ItemCurvePuzzleBehavior"_fnv, "ItemPortableSafeBehavior"_fnv, "ItemCarKeyBehavior"_fnv,
    // The Ghost Survivors: inventory, shop, item box, character and accessory choice, records, results, game over
    "RogueInventoryBehavior"_fnv, "RogueInventoryBackBehavior"_fnv, "RogueInventoryCaptionBehavior"_fnv,
    "RogueInventoryDetailBehavior"_fnv, "RogueInventorySlotBehavior"_fnv, "RogueInGameShopBehavior"_fnv,
    "RogueItemBoxBehavior"_fnv, "RogueCharaSelectBehavior"_fnv, "RogueAccessoryBehavior"_fnv, "RogueRecordBehavior"_fnv,
    "OpenedRogueRecordsBehavior"_fnv, "RogueResultBehavior"_fnv, "RogueGameOverBehavior"_fnv,
};
#else
constexpr size_t MENU_GUI_TYPES[] = {
    // the ones of the name list (VR::on_pre_gui_draw_element), by type too
    "InventoryMenu"_fnv, "PauseMenu"_fnv, "CutinMenu"_fnv, "OptionMenu"_fnv, "FileMenu"_fnv, "ChoiceMenu"_fnv,
    "MultiSubMenu"_fnv, "QuickEquipMenu"_fnv, "DetailSearchGUI"_fnv, "PharmacyDictionary"_fnv, "TutorialGUI"_fnv,
    "DifficultySelectGUI"_fnv, "AdjustBrightnessGUI"_fnv, "GameOverScreen"_fnv,
    // more of the main game: save (tape recorder), map, the inventory's item choice and context menu, the files' pictures,
    // records, dialogs, the audio options, the first start's settings
    "SaveMenu"_fnv, "MapWindow"_fnv, "InventoryItemSelect"_fnv, "InventoryContextMenu"_fnv, "FileArtGUI"_fnv,
    "OpenRecordMenu"_fnv, "PopupDialog"_fnv, "SystemDialog"_fnv, "WwiseOptionMenu"_fnv, "AdjustHDRGUI"_fnv,
    "AdjustHDR2GUI"_fnv, "ScreenAreaAdjustGUI"_fnv, "CalibrationGui"_fnv, "FirstSettingMenu"_fnv, "FirstLanguageGUI"_fnv,
    "FirstReNetMenu"_fnv, "GenomeCodexGUI"_fnv,
    // the DLC (Not a Hero, End of Zoe, Banned Footage): their own copies of the menus and the minigames' menus and results
    "CH8PauseMenu"_fnv, "CH8MainMenu"_fnv, "CH8OptionMenu"_fnv, "CH8SaveMenu"_fnv, "CH8QuickEquipMenu"_fnv,
    "CH8CutinMenu"_fnv, "CH8GameOverScreen"_fnv, "CH8MapWindow"_fnv, "CH8ResultMenu"_fnv, "CH9MainMenu"_fnv,
    "CH9PauseMenu"_fnv, "CH9SaveMenu"_fnv, "CH9MultiSubMenu"_fnv, "CH9MapWindow"_fnv, "CH9DifficultySelectGUI"_fnv,
    "CH9FileArtGUI"_fnv, "CardGameInventoryMenu"_fnv, "BirthdayMainMenu"_fnv, "BirthdayResult"_fnv, "Cp7MainMenu"_fnv,
    "Cp7AchievementMenu"_fnv, "Chapter3_IMD_MainMenu"_fnv, "Chapter3_IMD_Result"_fnv, "FF030_Ex_MainMenu"_fnv,
    // (not the title's: MainMenu, TitleScreen, TitleSubMenu, TitleExtraContentGUI, on the cassette, as before)
};
#endif

// the first of the object's own components whose type is a menu's (nullptr: none)
sdk::RETypeDefinition* menu_component_type(::REGameObject* go) {
    auto* const first = go != nullptr ? (::REComponent*)go->transform : nullptr;

    if (first == nullptr) {
        return nullptr;
    }

    int n = 0;

    for (auto* c = first->childComponent; c != nullptr && c != first && n < 64; c = c->childComponent, ++n) {
        auto* const td = utility::re_managed_object::get_type_definition((::REManagedObject*)c);
        const char* const name = td != nullptr ? td->get_name() : nullptr;

        if (name == nullptr) {
            continue;
        }

        const auto h = utility::hash(std::string_view{name});

        for (const auto t : MENU_GUI_TYPES) {
            if (h == t) {
                return td;
            }
        }
    }

    return nullptr;
}

// a menu by its component's type; the answer is kept per object name
bool is_menu_by_type(::REGameObject* go, size_t name_hash) {
    static std::mutex s_mtx{};
    static std::unordered_map<size_t, bool> s_known{};

    {
        std::scoped_lock _{s_mtx};

        if (const auto it = s_known.find(name_hash); it != s_known.end()) {
            return it->second;
        }

        if (s_known.size() >= 1024) {
            return false;
        }
    }

    auto* const td = menu_component_type(go);

#if !REVR_RELEASE // debug build: the menus found by type, with the type
    if (td != nullptr) {
        spdlog::info("[VR] \"{}\" counts as a game menu by its component {} (the large-scale light pauses while it is drawn)",
            utility::re_game_object::get_name(go), td->get_full_name());
    }
#endif

    std::scoped_lock _{s_mtx};
    s_known.emplace(name_hash, td != nullptr);
    return td != nullptr;
}

#if !REVR_RELEASE
// debug build: the game's own component types of a GUI object, for the "GUI first drawn" lines
std::string game_component_types(::REGameObject* go) {
    std::string out{};
    auto* const first = go != nullptr ? (::REComponent*)go->transform : nullptr;
    int n = 0;

    for (auto* c = first != nullptr ? first->childComponent : nullptr; c != nullptr && c != first && n < 64; c = c->childComponent, ++n) {
        auto* const td = utility::re_managed_object::get_type_definition((::REManagedObject*)c);
        const char* const ns = td != nullptr ? td->get_namespace() : nullptr;

        if (ns != nullptr && std::string_view{ns}.starts_with("app")) {
            out += out.empty() ? "" : ", ";
            out += td->get_full_name();
        }
    }

    return out.empty() ? std::string{"none"} : out;
}
#endif
} // namespace
#endif

bool VR::on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    REF_PROFILE_FUNCTION();

    inside_gui_draw = true;

#if defined(RE4)
    // [fix97 19.11.2026] RE4: the list of saves (main menu "Carica", dump of 19.11.2026: Gui_ui1030 with Gui_ui0610) dims the
    // scene behind it; the dimming is in the fovea views' picture and not in the cleaned wide view, so the large-scale
    // light took the difference for light: a lit band left of the list, gone with "Large scale light from periphery" off
    // (the user's test). The same as RE7's menus below: while it is drawn the quad view pauses that light.
    if (auto* go = utility::re_component::get_game_object(gui_element); go != nullptr) {
        const auto gui_name = utility::re_game_object::get_name(go);

        // [fix98 19.11.2026] every menu of the game (the user's request), by the number in the GUI's name. Names drawn in
        // the RE4 sessions of 19.11.2026: while playing 0300, 2010 - 2310 (HUD) and 3030; title 0900, main menu 1001 /
        // 1010 / 1030, saves 0600 / 0610, pause and options 0500 - 0510, a question ("Uscire dal gioco?") 0210, attache
        // case 3001 - 3006 / 3061, item inspection 3040, their backdrop AcBackGround; from the notes further down: map
        // 3100 - 3121, gem inlay 3090, end of chapter 5000. Not a menu: the 2xxx (HUD), 303x, 0300, and whatever number
        // has not been seen yet: each name is logged the first time it is drawn, so a log tells what is missing.
        bool is_menu = gui_name == "AcBackGround";

        if (!is_menu && gui_name.size() >= 10 && gui_name.compare(0, 6, "Gui_ui") == 0) {
            int number = 0;
            bool digits = true;

            for (size_t i = 6; i < 10; ++i) {
                const char c = gui_name[i];
                digits = digits && c >= '0' && c <= '9';
                number = number * 10 + (c - '0');
            }

            // [fix158 24.11.2026] from re4.exe (the GUI names in its strings, with the element names that follow each): 3xxx
            // are the in-game menus, also 32xx (3200 / 3210: drawn only with the case's pages in every log), 3700 the storage
            // (sl_warehouse), 3800 the case and its charms (sg_case, sg_charm, c_perks), 3500 and the numbers not seen yet;
            // not 36xx (3600 is drawn while playing, in seven logs) nor 303x. 08xx a list menu (c_header, c_list,
            // sl_list_sub), 2320 / 2330 the tutorials' list and pages (sl_tutorial; c_contents_move, c_cursor_left / right),
            // 2650 / 2660 the shooting range's token exchange (c_token_all, c_trade) and result (c_result, c_rank); the rest of
            // 26xx is the shooting range's HUD.
            is_menu = digits && (number == 210 || (number >= 500 && number <= 699) || (number >= 800 && number <= 899) ||
                                    (number >= 900 && number <= 1999) || number == 2320 || number == 2330 || number == 2650 ||
                                    number == 2660 ||
                                    (number >= 3000 && number <= 3999 && !(number >= 3030 && number <= 3039) &&
                                     !(number >= 3600 && number <= 3699)) || number == 5000 ||
                                    (number >= 4000 && number <= 4099)); // [fix156] 4xxx: the title's extras (re4.exe: 4010 the
                                                                         // extra contents shop, 4040 the figures, 4030 / 4031 / 4050)

            // [CRAFT fix128] the attache case's own elements are drawn: the craft page's item may be moved (craft_window_push)
            if (digits && number >= 3001 && number <= 3006) {
                s_case_gui_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
            }
        }

        if (is_menu) {
#if !REVR_RELEASE // [fix158] debug build: which menu GUI starts a pause of the light (as RE2, fix154), so a name drawn while
                  // playing shows up
            if (const int32_t last = quadview::State::get().menu_gui_frame.load(); m_render_frame_count - last > 24) {
                spdlog::info("[VR] menu GUI drawn after a pause (the large-scale light pauses): \"{}\"", gui_name);
            }
#endif
            quadview::State::get().menu_gui_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
            quadview::State::get().menu_gui_frame.store(m_render_frame_count);
        }

        {
            static std::mutex s_mtx{};
            static std::unordered_set<size_t> s_seen{};
            std::scoped_lock _{s_mtx};

            if (s_seen.size() < 512 && s_seen.insert(utility::hash(gui_name)).second) {
                REVR_LOG(spdlog::info("[VR] GUI first drawn: \"{}\" ({})", gui_name, is_menu ? "counts as a game menu: the large-scale light pauses" : "not a menu"));
            }
        }

        // ([HUD BLUR fix114 21.11.2026] the weapon panel's two via.gui.BlurFilter were hidden here before the object drew.
        // RE4 test of fix114: shown or hidden, the engine's render time with the right grip held is the same (4.285 /
        // 4.286 ms against 4.280 - 4.292) and the HUD looks the same; they hold no elements. Taken out in fix115.)

    }
#endif

#if defined(RE2)
    // [MENU LIGHT RE2 fix152 24.11.2026] as RE4 (fix97 / fix98) and RE7 (15.11.2026): a game menu dims the scene behind it in
    // the fovea views' picture and not in the cleaned wide view, and the large-scale light took that for light (a lit band
    // left of the menu). While one of RE2's menus is drawn the tri-view pauses that light. The names come from the debug
    // build's log ("GUI first drawn"); a menu missing from the list keeps the band, and its name is in that log.
    if (auto* go = utility::re_component::get_game_object(gui_element); go != nullptr) {
        const auto gui_name = utility::re_game_object::get_name(go);
        const auto h = utility::hash(gui_name);
        bool is_menu = false;

        for (const auto menu : RE2_MENU_GUIS) {
            is_menu = is_menu || h == menu;
        }

        is_menu = is_menu || is_menu_by_type(go, h); // [fix157] a menu whose name is in no log, by its component's type

        if (is_menu) {
#if !REVR_RELEASE // [fix154] debug build: which menu GUI starts a pause of the light (with the log's time), so a name that is
                  // drawn while playing shows up
            if (const int32_t last = quadview::State::get().menu_gui_frame.load(); m_render_frame_count - last > 24) {
                spdlog::info("[VR] menu GUI drawn after a pause (the large-scale light pauses): \"{}\"", gui_name);
            }
#endif
            quadview::State::get().menu_gui_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
            quadview::State::get().menu_gui_frame.store(m_render_frame_count);
        }

#if !REVR_RELEASE // the names for the list: debug build only
        static std::mutex s_mtx{};
        static std::unordered_set<size_t> s_seen{};
        std::scoped_lock _{s_mtx};

        if (s_seen.size() < 512 && s_seen.insert(h).second) {
            spdlog::info("[VR] GUI first drawn: \"{}\" ({}; components: {})", gui_name,
                is_menu ? "counts as a game menu: the large-scale light pauses" : "not a menu", game_component_types(go));
        }
#endif
    }
#endif

#if defined(RE7)
    // [MOVIE CANVAS 14.11.2026] each GUI object's name the first time it is drawn, with the time: tells which one carries the
    // movie before the main menu (for a detection by name, in case the game makes no 2D VR request on PC)
    if (auto* go = utility::re_component::get_game_object(gui_element); go != nullptr) {
        static std::mutex s_mtx{};
        static std::unordered_set<size_t> s_seen{};
        const auto gui_name = utility::re_game_object::get_name(go);
        std::scoped_lock _{s_mtx};

        if (s_seen.size() < 512 && s_seen.insert(utility::hash(gui_name)).second) {
            REVR_LOG(spdlog::info("[VR] GUI first drawn: \"{}\" (canvas {}, 2D VR requests 0x{:x}; components: {})", gui_name, m_menu_canvas_on ? "on" : "off",
                s_2dvr_requests.load(), game_component_types(go)));
        }

        // [MENU LIGHT 15.11.2026] a game menu is open: its dark backdrop is in the fovea views' picture but not in the cleaned
        // wide view, so the large-scale light took the difference for light and lit the fovea up (dumps 07 / 08 of 14.11.2026:
        // inventory, "Uscire dal gioco?": the fovea 2-3x brighter than the periphery). The quad view pauses that light.
        bool is_menu = false;
        const auto name_hash = utility::hash(gui_name);

        switch (name_hash) {
            case "SaveMenuGUI"_fnv: // [fix157] the tape recorder's save menu (logs of 16 - 18.11.2026; app.SaveMenu)
            case "InventoryMenu"_fnv:
            case "PauseMenuGUI"_fnv:
            case "CutinMenu"_fnv:
            case "OptionMenu"_fnv:
            case "FileMenu"_fnv:
            case "ChoiceMenu"_fnv:
            case "MultiSubMenu"_fnv:
            case "QuickEquipMenu"_fnv:
            case "DetailSearchGUI"_fnv:
            case "PharmacyDictionary"_fnv:
            case "TutorialGUI"_fnv:
            case "DifficultySelectGUI"_fnv:
            case "AdjustBrightnessGUI"_fnv:
            case "GameOverScreen"_fnv:
                is_menu = true;
                break;
            default:
                is_menu = is_menu_by_type(go, name_hash); // [fix157] a menu whose name is in no log, by its component's type
                break;
        }

        if (is_menu) {
            quadview::State::get().menu_gui_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
            quadview::State::get().menu_gui_frame.store(m_render_frame_count); // [MENU LIGHT 15.11.2026 evening]
        }
    }

    // [MENU SHEET 14.11.2026] the options GUI printed on the cassette label (drawn here, on the render thread). If it hangs
    // under the label in the hierarchy the label's move carries it; otherwise its world matrix gets the same move for this
    // draw and is set back after it (on_gui_draw_element)
    // (TitleExtraContentGUI lies at the same place on the label, without parent, log of 14.11.2026 19:18: it gets the same move)
    const auto sheet_gui_name = [&]() -> int {
        auto* g = utility::re_component::get_game_object(gui_element);
        if (g == nullptr || g->transform == nullptr) {
            return 0;
        }
        const auto n = utility::re_game_object::get_name(g);
        return n == "GUI_option" ? 1 : (n == "TitleExtraContentGUI" ? 2 : 0);
    }();

    if (auto* go = utility::re_component::get_game_object(gui_element); sheet_gui_name != 0) {
        if (sheet_gui_name == 1) {
            m_menu_option_gui = go;
            m_menu_option_gui_seen_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        }

        bool valid = false;
        Matrix4x4f x{};
        Vector4f label_up{};
        Vector4f label_down{};

        {
            std::scoped_lock _{m_menu_sheet_gui_mtx};
            valid = m_menu_sheet_gui_valid;
            x = m_menu_sheet_gui_x;
            label_up = m_menu_sheet_label_up_pos;
            label_down = m_menu_sheet_label_down_pos;
        }

        auto* label = m_menu_label;

        // [fix64] a draw whose restore did not come (GUI_option and TitleExtraContentGUI: the restore of 14.11.2026 kept
        // one element only; the world matrix then got the move on top of the move each frame, 4.8e11 m by the second
        // opening, the text gone): back to the original first. One entry per element.
        auto& entry = m_menu_sheet_gui_entries[gui_element];

        if (entry.pending) {
            entry.pending = false;

            if (menu_sheet_same_matrix(go->transform->worldTransform, entry.written)) {
                go->transform->worldTransform = entry.original;
            }
        }

        if (valid && label != nullptr && label->transform != nullptr) {
            bool under_label = false;

            for (auto* p = menu_sheet_parent(go->transform); p != nullptr; p = menu_sheet_parent(p)) {
                if (p == label->transform) {
                    under_label = true;
                    break;
                }
            }

            auto& world = go->transform->worldTransform;

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            // logged once the move is large enough to tell whether the label is moved while the GUI draws
            if (sheet_gui_name == 1 && glm::length(Vector3f{label_up} - Vector3f{label_down}) > 0.05f && !m_menu_sheet_logged_gui.exchange(true)) {
                auto* ctx = sdk::get_thread_context();
                auto* view = sdk::call_object_func<REComponent*>(gui_element, "get_View", ctx, gui_element);
                const auto view_type = view != nullptr ? sdk::call_object_func<uint32_t>(view, "get_ViewType", ctx, view) : 0xffffffffu;
                auto* target = sdk::call_object_func<::REManagedObject*>(gui_element, "get_RenderTarget", ctx, gui_element);
                REVR_LOG(spdlog::info("[VR] options sheet: GUI_option drawn at {} scale {:.3f}, {} the label (parents{}); view type {}, render "
                             "target {}; the sheet's copy goes from {} to {} this frame",
                    layer_trace::vec3(world[3]), glm::length(Vector3f{world[0]}), under_label ? "under" : "not under",
                    menu_sheet_chain(go->transform), view_type, target != nullptr ? "set" : "none", layer_trace::vec3(label_down),
                    layer_trace::vec3(label_up)));
            }
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            // [fix63] the options GUI's elements once (for a background of its own, should the paper not come up)
            if (sheet_gui_name == 1 && !m_menu_sheet_gui_tree_logged.exchange(true)) {
                auto* ctx = sdk::get_thread_context();
                auto* view = sdk::call_object_func<::REManagedObject*>(gui_element, "get_View", ctx, gui_element);
                std::string tree{};
                int count = 0;

                std::function<void(::REManagedObject*, int)> walk = [&](::REManagedObject* node, int depth) {
                    for (; node != nullptr && count < 250; ++count) {
                        auto* td = utility::re_managed_object::get_type_definition(node);
                        auto* nm = sdk::call_object_func<::REManagedObject*>(node, "get_Name", ctx, node);
                        const bool visible = sdk::call_object_func<bool>(node, "get_Visible", ctx, node);
                        tree += std::format("\n    {}{} \"{}\" {}", std::string(depth * 2, ' '), td != nullptr ? td->get_full_name() : std::string{"?"},
                            nm != nullptr ? utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm)) : std::string{},
                            visible ? "visible" : "hidden");

                        if (depth < 12) {
                            walk(sdk::call_object_func<::REManagedObject*>(node, "get_Child", ctx, node), depth + 1);
                        }

                        node = sdk::call_object_func<::REManagedObject*>(node, "get_Next", ctx, node);
                    }
                };

                walk(view, 0);
                REVR_LOG(spdlog::info("[VR] options sheet: GUI_option elements ({}):{}", count, tree));
            }
#endif

            if (!under_label) {
                entry.original = world;
                world = x * world;
                entry.written = world;
                entry.pending = true;
            }
        }
    }
#endif

    // This hook is what places every GUI element in VR space (2D UI distance, world-space
    // scale). It runs independently of the camera matrices, which is why a map's markers
    // keep their own depth even when the camera is left native - so hand the element back
    // untouched, same as the early-out below.
    //   should_suspend_camera_overrides(): canvas mode, the engine draws its own UI.
    //   m_disable_gui_element_override:    explicit switch, belongs with the projection
    //                                      override (see VR.hpp for why).
    if (should_suspend_camera_overrides() || m_disable_gui_element_override) {
        return true;
    }

    if (!get_runtime()->ready()) {
        return true;
    }

    auto game_object = utility::re_component::get_game_object(gui_element);

    if (game_object != nullptr && game_object->transform != nullptr) {
        auto context = sdk::get_thread_context();

        const auto name = utility::re_game_object::get_name(game_object);
        const auto name_hash = utility::hash(name);

        switch (name_hash) {
        // Don't mess with this, causes weird black boxes on the sides of the screen
        case "GUI_PillarBox"_fnv:
        case "GUIEventPillar"_fnv:
        // These allow the cutscene transitions to display (fade to black)
        case "BlackFade"_fnv:
        case "WhiteFade"_fnv:
        case "Fade_In_Out_Black"_fnv:
        case "Fade_In_Out_White"_fnv:
        case "FadeInOutBlack"_fnv:
        case "FadeInOutWhite"_fnv:
        case "GUIBlackMask"_fnv:
        case "GenomeCodexGUI"_fnv:
        case "sm42_020_keystrokeDevice01A_gimmick"_fnv: // this one is the keypad in the locker room...
            return true;

#if defined(RE2) || defined(RE3)
        // the weird buggy overlay in the inventory
        case "GuiBack"_fnv:
            return false;
#endif

#if defined(RE4)
        case "Gui_ui2510"_fnv: // Black bars in cutscenes
            return false;
#endif

#if TDB_VER >= 73
        case "ui0199"_fnv: // weird black bars in Kunitsu-Gami
            return false;
#endif

        default:
            break;
        };

        // Certain UI elements we want to remove when in VR (FirstPerson enabled)
#if defined(RE2) || defined(RE3)
        if (std::chrono::steady_clock::now() - m_last_crosshair_hide > std::chrono::seconds(1)) {
            auto& fp = FirstPerson::get();

            if (fp->is_enabled() && fp->will_be_used()) {
                const auto has_motion_controls = this->is_using_controllers();

                switch(name_hash) {
                case "GUI_Reticle"_fnv: // Crosshair
                    if (has_motion_controls) {
                        return false;
                    }
                    
                    break;
                default:
                    break;
                }
            }
        }
#endif

#ifdef RE7
        if (name_hash == "HUD"_fnv) { // not a hero
            // Stops HUD element from being stuck to the screen
            sdk::call_object_func<REComponent*>(gui_element, "set_RenderTarget", context, gui_element, nullptr);
        }
#endif

        //spdlog::info("VR: on_pre_gui_draw_element: {}", name);
        //spdlog::info("VR: on_pre_gui_draw_element: {} {:x}", name, (uintptr_t)game_object);

        auto view = sdk::call_object_func<REComponent*>(gui_element, "get_View", context, gui_element);

        if (view != nullptr) {
            const auto current_view_type = sdk::call_object_func<uint32_t>(view, "get_ViewType", context, view);

            // [RE4 MAP GLUE] Gui_ui3120 kommt als World herein und wuerde hier sonst
            // durchrutschen -- genau deshalb hing diese eine Ebene weiter in der 3D-Szene,
            // waehrend alle anderen Karten-Layer vorne angeheftet wurden. Der Original-
            // ViewType landet unten in restore_data und wird nach dem Zeichnen
            // zurueckgeschrieben, der Eingriff gilt also nur fuer diesen Frame.
            int map_layer = 0;
            const bool map_glue = m_map_face_glue && is_glue_gui(name_hash, &map_layer);

            if (current_view_type == (uint32_t)via::gui::ViewType::Screen || map_glue) {
                static sdk::RETypeDefinition* via_render_mesh_typedef = nullptr;

                if (via_render_mesh_typedef == nullptr) {
                    via_render_mesh_typedef = sdk::find_type_definition("via.render.Mesh");

                    // wait
                    if (via_render_mesh_typedef == nullptr) {
                        return true;
                    }
                }

                // we don't want to mess with any game object that has a mesh
                // because it might be something physical in the game world
                // that the player can interact with
                if (utility::re_component::find(game_object->transform, via_render_mesh_typedef->get_type()) != nullptr) {
                    return true;
                }

#ifdef RE7
                static auto ui_world_pos_attach_typedef = sdk::find_type_definition("app.UIWorldPosAttach");
#endif

                auto ui_scale = m_ui_scale_option->value();
                const auto world_ui_scale = m_world_ui_scale_option->value();

                auto& restore_data = g_elements_to_reset.emplace_back(std::make_unique<GUIRestoreData>());
                auto original_game_object_pos = sdk::get_transform_position(game_object->transform);

                restore_data->element = gui_element;
                restore_data->view = view;
                restore_data->original_position = original_game_object_pos;
                restore_data->overlay = sdk::call_object_func<bool>(view, "get_Overlay", context, view);
                restore_data->detonemap = sdk::call_object_func<bool>(view, "get_Detonemap", context, view);
                restore_data->view_type = (via::gui::ViewType)current_view_type;

                original_game_object_pos.w = 0.0f;

                // Set view type to world
                sdk::call_object_func<void*>(view, "set_ViewType", context, view, (uint32_t)via::gui::ViewType::World);

                // Set overlay = true (fixes double vision in VR)
                sdk::call_object_func<void*>(view, "set_Overlay", context, view, true);

                // Set detonemap = true (fixes weird tint)
                sdk::call_object_func<void*>(view, "set_Detonemap", context, view, true);

                // [GOLD-FX 29.09.2026] s. gold_fx oben.
                if (m_hide_feedback_fx) {
                    gold_fx::hide(name_hash, name, view, restore_data->hidden_fx);
                }

                // Go through the children until we hit a blur filter
                // And then remove it
                /*for (auto child = sdk::call_object_func<REComponent*>(view, "get_Child", sdk::get_thread_context(), view); child != nullptr; child = sdk::call_object_func<REComponent*>(child, "get_Child", sdk::get_thread_context(), child)) {
                    if (utility::re_managed_object::is_a(child, "via.gui.BlurFilter")) {
                        // Call remove()
                        sdk::call_object_func<void*>(child, "remove", sdk::get_thread_context(), child);
                        break;
                    }
                }*/

                auto camera = sdk::get_primary_camera();

                // Set the gui element's position to be in front of the camera
                if (camera != nullptr) {
                    auto camera_object = utility::re_component::get_game_object(camera);

                    if (camera_object != nullptr && camera_object->transform != nullptr) {
                        auto& gui_matrix = game_object->transform->worldTransform;
                        auto child = sdk::call_object_func<REManagedObject*>(view, "get_Child", context, view);

                        bool placed_at_target = false; // [MONO UI] fix_2d_position anchored it at something in the world

                        auto fix_2d_position = [&](const Vector4f& target_position, 
                                                    bool screen_correction = true,
                                                    std::optional<float> custom_ui_scale = std::nullopt)
                        {
                            placed_at_target = true;

                            if (!custom_ui_scale) {
                                custom_ui_scale = world_ui_scale;
                            }

                            auto delta = target_position - m_render_camera_matrix[3];
                            delta.w = 0.0f;

                            auto dir = glm::normalize(delta);
                            dir.w = 0.0f;
                            
                            // make matrix from dir
                            const auto look_mat = glm::rowMajor4(glm::lookAtLH(Vector3f{}, Vector3f{ dir }, Vector3f(0.0f, 1.0f, 0.0f)));
                            const auto look_rot = glm::quat{look_mat};

                            auto new_pos = target_position;
                            new_pos.w = 1.0f;

                            gui_matrix = look_mat;
                            gui_matrix[3] = new_pos;

                            // LESSON: DO NOT CALL THESE METHODS ON THE TRANSFORM!
                            // THEY CAUSE SOME STRANGE BUGS WHEN THE GUI ELEMENT HAS A PARENT TRANSFORM!
                            // THE GUI RENDERING FUNCTIONS PERFORM ON THE WORLD MATRIX, SO THIS IS NOT NECESSARY.
                            //sdk::set_transform_position(game_object->transform, new_pos);
                            //sdk::set_transform_rotation(game_object->transform, look_rot);
                            
                            const auto scaled_ui_scale = *custom_ui_scale * 0.01f;
                            const auto distance = glm::length(delta);
                            const auto scale = std::clamp<float>(distance * scaled_ui_scale, 0.1f, 100.0f);

                            regenny::via::Size gui_size{};
                            sdk::call_object_func<void*>(view, "get_ScreenSize", &gui_size, context, view);
                            
                            auto fix_transform_object = [&](::REManagedObject* object) {
                                static auto transform_object_type = sdk::find_type_definition("via.gui.TransformObject");

                                if (object == nullptr) {
                                    return;
                                }

                                const auto t = utility::re_managed_object::get_type_definition(object);

                                if (t == nullptr || !t->is_a(transform_object_type)) {
                                    return;
                                }

                                //sdk::call_object_func<void*>(object, "set_ResolutionAdjust", context, object, true);

                                if (screen_correction) {
                                    Vector3f half_size{ gui_size.w / 2.0f, gui_size.h / 2.0f, 0.0f };
                                    sdk::call_object_func<void*>(object, "set_Position", context, object, &half_size);
                                }

                                //Vector4f new_scale{ scale, scale, scale, 1.0f };
                                const auto old_scale = sdk::call_object_func_easy<Vector4f>(object, "get_Scale");
                                Vector4f new_scale{ old_scale.y, old_scale.y, old_scale.z, 1.0f };
                                sdk::call_object_func<void*>(object, "set_Scale", context, object, &new_scale);
                            };

                            for (auto c = child; c != nullptr; c = sdk::call_object_func<REManagedObject*>(c, "get_Next", context, c)) {
                                fix_transform_object(c);
                            }

                            // Fix for other kinds of world pos attach elements.
#ifdef RE7
                            if (name_hash == "InteractOperationCursor"_fnv) {
                                auto world_pos_attach_comp = utility::re_component::find(game_object->transform, ui_world_pos_attach_typedef->get_type());

                                // Fix the world position of the gui element
                                if (world_pos_attach_comp != nullptr) {
                                    auto target_cache = sdk::get_object_field<::REManagedObject*>(world_pos_attach_comp, "_TargetGUIElem");

                                    if (target_cache != nullptr && *target_cache != nullptr) {
                                        auto element = sdk::get_object_field<::REManagedObject*>(*target_cache, "_Element");

                                        if (element != nullptr && *element != nullptr) {
                                            Vector3f zero_size{ 0.0f, 0.0f, 0.0f };
                                            sdk::call_object_func<void*>(*element, "set_Position", context, *element, &zero_size);
                                        }
                                    }
                                }
                            }
#endif

                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        };

                        auto camera_transform = camera_object->transform;

#if defined(RE7)
                        // [MENU SHEET 14.11.2026] while the view is held at the deck, the 2D UI goes in front of that view
                        Matrix4x4f held_matrix{m_original_camera_matrix};

                        if (m_menu_hold && m_menu_camera_active && !m_flatscreen_overlay) {
                            held_matrix = Matrix4x4f{m_menu_deck_rot};
                            held_matrix[3] = m_menu_deck_pos;
                            held_matrix[3].w = 1.0f;
                        }

                        const auto& camera_matrix = held_matrix;
#else
                        const auto& camera_matrix = m_original_camera_matrix;
#endif
                        const auto& camera_position = camera_matrix[3];

                        glm::quat wanted_rotation{};
                        
                        bool wants_face_glue = false;
                        auto ui_distance = m_ui_distance_option->value();

                        // [RE4 MAP GLUE] Die Karte soll als EIN Stueck am Kopf haengen:
                        // dieselbe Rotation (Face-Glue) UND dieselbe Ankerposition fuer
                        // jede ihrer Ebenen. Sonst ankert jede Ebene an der Spielkamera,
                        // waehrend der Kopf sich frei bewegt -- das ist die Parallaxe,
                        // die Umriss, Navkreuz und Funde gegeneinander schieben laesst.
                        if (map_glue) {
                            wants_face_glue = true;
                        }

                        switch (name_hash) {
                        case "damage_ui2102"_fnv:
                        case "NightVision_Filter"_fnv:
                            wants_face_glue = true;
                            break;
                        default:
                            break;
                        };

                        float right_world_adjust{};

                        switch (name_hash) {
                        case "GuiFront"_fnv:
                        case "GUI_Pause"_fnv:
                        case "GUIPause"_fnv:
                        case "GUIMapBg"_fnv:
                        case "BG"_fnv:
                            ui_distance += 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_MapBG"_fnv:
                        case "GUI_Map"_fnv:
                        case "GUI_MapGrid"_fnv:
                        case "GUIMap"_fnv:
                        case "GUIMapIcon"_fnv:
                            ui_distance -= 0.025f; // give everything a little pop in the inventory.
                            break;
                        case "GuiCaption"_fnv:
                        case "GUIInventory"_fnv:
                        case "GUIInventoryCraft"_fnv:
                        case "GUIInventoryKeyItem"_fnv:
                        case "GUIInventoryTreasure"_fnv:
                        case "GUIBinder"_fnv:
                        case "GUIEquip"_fnv:
                            ui_distance -= 0.1f; // give everything a little pop in the inventory.
                            break;
                        case "GUI_RemainingBullet"_fnv:
                            right_world_adjust = 0.15f;
                            break;
                        case "GUI_Reticle"_fnv:
                        case "ReticleGUI"_fnv:
                        case "GUIReticle"_fnv:
                            ui_distance = 5.0f;
                            ui_scale = 2.0f;
                            break;
                        case "hud_hunterwirewindow"_fnv:
                            ui_distance = 10.0f;
                            ui_scale = 1.0f;
                            break;
                        default:
                            break;
                        }

                        if (ui_distance < 0.0f) {
                            ui_distance = 0.0f;
                        }

                        // glues the GUI to the camera rotation and position
                        if (wants_face_glue) {
                            // [RE4 MAP GLUE] Die 5 m sind fuer Vollbild-Filter gedacht;
                            // die Karte will man dichter und regelbar haben. Der Index
                            // staffelt die Ebenen um Millimeter, damit die Reihenfolge
                            // feststeht und keine Ebene eine andere schluckt.
                            ui_distance = map_glue
                                ? (m_map_glue_distance - map_layer * m_map_glue_layer_gap)
                                : 5.0f;

                            wanted_rotation = glm::extractMatrixRotation(m_render_camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };
                        } else {
                            const auto gui_rotation_offset = get_gui_rotation_offset();

                            wanted_rotation = glm::extractMatrixRotation(camera_matrix) * Matrix4x4f{
                                -1, 0, 0, 0,
                                0, 1, 0, 0,
                                0, 0, -1, 0,
                                0, 0, 0, 1
                            };

                            if (m_decoupled_pitch->value()) {
                                bool is_exception = false;

                                switch (name_hash) {
                                    case "GUIReticle"_fnv:[[fallthrough]];
                                    case "ReticleGUI"_fnv:[[fallthrough]];
                                    case "GUI_Reticle"_fnv:
                                        is_exception = true;
                                        break;
                                    default:
                                        break;
                                }

                                if (!is_exception) {
                                    wanted_rotation = utility::math::flatten(wanted_rotation);
                                }
                            }

                            wanted_rotation = gui_rotation_offset * wanted_rotation;
                        }

#if defined(RE7)
                        // [MENU SHEET fix64] while the options sheet is up, the guide ("Regola le impostazioni dei comandi",
                        // Conferma / Indietro) goes this many degrees lower, below the sheet instead of over it
                        auto wanted_rotation_mat = Matrix4x4f{wanted_rotation};

                        if (name_hash == "GuideIcon"_fnv && m_menu_sheet_up_for_gui.load()) {
                            // [fix65] it follows the sheet: turned to the sheet's heading (the sheet comes where the head looked
                            // when the options opened), and lowered with the sheet's own "below eye level" (15 deg = as measured)
                            Vector3f heading{};

                            {
                                std::scoped_lock _{m_menu_sheet_gui_mtx};
                                heading = m_menu_sheet_heading;
                            }

                            const Vector3f f0{wanted_rotation_mat[2].x, 0.0f, wanted_rotation_mat[2].z};

                            if (glm::length(f0) > 1e-4f && glm::length(heading) > 1e-4f) {
                                const auto a0 = glm::normalize(f0);
                                const auto a1 = glm::normalize(heading);
                                const float yaw = std::atan2(glm::cross(a0, a1).y, glm::dot(a0, a1));
                                wanted_rotation_mat = Matrix4x4f{glm::angleAxis(yaw, Vector3f{0.0f, 1.0f, 0.0f})} * wanted_rotation_mat;
                            }

                            const Vector3f fwd{wanted_rotation_mat[2]};
                            const Vector3f axis = glm::cross(Vector3f{0.0f, 1.0f, 0.0f}, fwd);

                            if (glm::length(axis) > 1e-4f) {
                                const float lower = m_menu_sheet_guide_down->value() + (m_menu_sheet_below->value() - 15.0f);
                                const auto down = glm::angleAxis(glm::radians(lower), glm::normalize(axis));
                                wanted_rotation_mat = Matrix4x4f{down} * wanted_rotation_mat;
                            }
                        }
#else
                        const auto wanted_rotation_mat = Matrix4x4f{wanted_rotation};
#endif

                        gui_matrix = wanted_rotation_mat;
                        // [RE4 MAP GLUE] Anker ist der KOPF (m_render_camera_matrix), nicht
                        // die Spielkamera. Nur so bekommen alle Karten-Ebenen exakt denselben
                        // Ursprung und koennen sich nicht mehr gegeneinander verschieben.
                        const auto anchor_position = map_glue ? m_render_camera_matrix[3] : camera_position;
                        gui_matrix[3] = anchor_position + (wanted_rotation_mat[2] * ui_distance) + (wanted_rotation_mat[0] * right_world_adjust);
                        gui_matrix[3].w = 1.0f;

                        // Scales the GUI so it's not massive.
                        // [RE4 MAP GLUE] Die Karte braucht die Skalierung weiterhin -- ohne sie
                        // steht sie riesig im Raum; nur die echten Vollbild-Filter lassen sie aus.
                        if (!wants_face_glue || map_glue) {
                            const auto scale = 1.0f / ui_scale;
                            gui_matrix = glm::scale(gui_matrix, Vector3f{ scale, scale, scale });
                        }

                        static auto gui_driver_typedef = sdk::find_type_definition(game_namespace("GUIDriver"));
                        static auto mhrise_npc_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcHeadMessage"));
                        static auto mhrise_speech_balloon_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonNpcSpeechBalloon"));
                        static auto mhrise_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonHeadMessage"));
                        static auto mhrise_otomo_head_message_typedef = sdk::find_type_definition(game_namespace("gui.GuiCommonOtomoHeadMessage"));
                        static auto kg_float_icon_behavior = sdk::find_type_definition("app.FloatIconBehavior");
                        static auto kg_general_vital_gauge_behavior = sdk::find_type_definition("app.GeneralVitalGaugeBehavior");

                        static auto gameobject_elements_list = {
                            mhrise_npc_head_message_typedef,
                            mhrise_speech_balloon_typedef,
                            mhrise_head_message_typedef,
                            mhrise_otomo_head_message_typedef,
                            kg_float_icon_behavior,
                            kg_general_vital_gauge_behavior
                        };
                        
                        // Fix position of interaction icons
                        if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "RogueFloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) { // RE2, RE3, RE4
                            if (name_hash == "GUI_FloatIcon"_fnv || name_hash == "Gui_FloatIcon"_fnv) {
                                m_last_interaction_display = std::chrono::steady_clock::now();
                            }
                        
                            fix_2d_position(original_game_object_pos);
                        } else if(gui_driver_typedef != nullptr) { // RE8
                            auto interact_icon_comp = utility::re_component::find(game_object->transform, gui_driver_typedef->get_type());

                            if (interact_icon_comp != nullptr) {
                                auto interact_icon_object = sdk::call_object_func<REGameObject*>(interact_icon_comp, "get_attachTarget", context, interact_icon_comp);

                                if (interact_icon_object != nullptr && interact_icon_object->transform != nullptr) {
                                    // call get_Position on the object
                                    Vector4f interact_icon_position{};
                                    sdk::call_object_func<Vector4f*>(interact_icon_object->transform, "get_Position", &interact_icon_position, context, interact_icon_object->transform);

                                    fix_2d_position(interact_icon_position);
                                }
                            }
                        } else {
                            // MHRise
                            for (auto element_type : gameobject_elements_list) {
                                if (element_type == nullptr) {
                                    continue;
                                }

                                auto element_comp = utility::re_component::find(game_object->transform, element_type->get_type());

                                if (element_comp == nullptr) {
                                    continue;
                                }

                                static auto get_parent_method = sdk::get_object_method(game_object->transform, "get_Parent");
                                auto parent = get_parent_method->call<::RETransform*>(context, game_object->transform);

                                if (parent != nullptr) {
                                    Vector4f offset{};

                                    std::optional<float> custom_ui_scale = std::nullopt;

                                    if (element_type == mhrise_speech_balloon_typedef) {
                                        static auto pos_data_field = mhrise_speech_balloon_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");

                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_npc_head_message_typedef) {
                                        static auto pos_data_field = mhrise_npc_head_message_typedef->get_field("posData");
                                        static auto npc_message_pos = pos_data_field->get_type()->get_field("NpcMessagePos");
                                        
                                        auto pos_data = pos_data_field->get_data<::REManagedObject*>(element_comp);

                                        if (pos_data != nullptr) {
                                            const auto y_offset = npc_message_pos->get_data<float>(pos_data);
                                            offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                        } else {
                                            offset = Vector4f{0.0f, 1.0f, 0.0f, 0.0f};
                                        }
                                    } else if (element_type == mhrise_head_message_typedef) {
                                        static auto message_pos_y_field = mhrise_head_message_typedef->get_field("MessagePosY");

                                        const auto y_offset = message_pos_y_field->get_data<float>(element_comp);
                                        offset = Vector4f{0.0f, y_offset, 0.0f, 0.0f};
                                    } else if (element_type == mhrise_otomo_head_message_typedef) { //airou and dog
                                        offset = Vector4f{0.0, 1.0f, 0.0f, 0.0f};
                                    } else if (element_type == kg_float_icon_behavior) {
                                        static auto param_field = kg_float_icon_behavior->get_field("_Param");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    } else if (element_type == kg_general_vital_gauge_behavior) {
                                        static auto param_field = kg_general_vital_gauge_behavior->get_field("_OpenParam");
                                        static auto world_pos_field = param_field != nullptr ? param_field->get_type()->get_field("WorldPos") : nullptr;

                                        if (world_pos_field != nullptr) {
                                            auto param = param_field->get_data<::REManagedObject*>(element_comp);

                                            if (param != nullptr) {
                                                offset = world_pos_field->get_data<Vector4f>(param);
                                                custom_ui_scale = 5.0f;
                                            }
                                        }
                                    }

                                    fix_2d_position(sdk::get_transform_position(parent) + offset, true, custom_ui_scale);
                                } else {
                                    fix_2d_position(original_game_object_pos);
                                }
                            }
                        }

                        // ... RE7
#ifdef RE7
                        auto world_pos_attach_comp = utility::re_component::find(game_object->transform, ui_world_pos_attach_typedef->get_type());

                        // Fix the world position of the gui element
                        if (world_pos_attach_comp != nullptr) {
                            const auto& target_pos = *sdk::get_object_field<Vector4f>(world_pos_attach_comp, "_NowTargetPos");
                            
                            fix_2d_position(target_pos);
                        }
#endif

                        // [MONO UI] where this element ended up: anchored at something in the world (fix_2d_position) or
                        // a panel in front of the head. A panel at another distance than the 2D UI distance (reticle,
                        // full-screen filters at 5 m) is kept as anchored too: its content sits at its centre.
                        if (is_mono_periphery()) {
                            quadview::GuiAnchor a{};
                            a.pos[0] = gui_matrix[3].x;
                            a.pos[1] = gui_matrix[3].y;
                            a.pos[2] = gui_matrix[3].z;
                            const float placed_distance = glm::length(Vector3f{gui_matrix[3]} - Vector3f{camera_position});
                            const bool other_distance = std::abs(placed_distance - m_ui_distance_option->value()) > 0.3f;
                            // [ANCHOR OWNERSHIP 27.10.2026] an icon at something in the world: a small area around it, and its
                            // pixels belong to it alone (dump 02 of 27.10.2026: the icon 0.04 tan above its anchor, about
                            // 0.05 across); a panel at another distance keeps the wide area and only claims pixels
                            a.at_target = placed_at_target;
                            a.radius_tan = placed_at_target ? 0.08f : (other_distance ? 0.20f : 0.0f);
                            a.kind = quadview::GuiAnchor::PANEL;
                            a.id = (uint64_t)game_object;
                            name.copy(a.name, sizeof(a.name) - 1);
                            // [ANCHOR FINAL 04.11.2026] listed after the draw, with the position it was drawn at
                            restore_data->has_anchor = true;
                            restore_data->anchor = a;
                            restore_data->anchor_frame = (uint64_t)m_render_frame_count;
                            restore_data->anchor_camera = camera_position;
                        }
                    }
                }
            } else if (is_mono_periphery()) {
                // [ITEM BEAMS 03.11.2026] The game's own world elements (ViewType World, not moved above) are drawn by every
                // scene layer at their place in the world. The green beam over an item (Gui_ui2130, one element per item;
                // notes of 25.10.2026) is one: outside the fovea the wide view's copy of it has to reach each eye at the
                // item's depth, not at the panels' (dump 01 of 03.11.2026). Its world matrix puts it at the item. The
                // other world elements are only listed in the log, to see which there are.
                const auto& world = game_object->transform->worldTransform;
                quadview::GuiAnchor a{};
                a.pos[0] = world[3].x;
                a.pos[1] = world[3].y;
                a.pos[2] = world[3].z;
                a.kind = quadview::game::is_item_beam(name) ? quadview::GuiAnchor::BEAM : quadview::GuiAnchor::WORLD;
                a.id = (uint64_t)game_object;
                name.copy(a.name, sizeof(a.name) - 1);
                // [UI MOTION fix125 22.11.2026] the matrix it is drawn with, kept under the frame's number
                std::memcpy(a.mat, &world[0][0], sizeof(a.mat));
                a.has_mat = true;
                quadview::State::get().add_gui_element((int64_t)(uint32_t)m_frame_count, a);
                quadview::State::get().add_gui_anchor((uint64_t)m_render_frame_count, a);
            }
        }
    } else {
        REVR_LOG(spdlog::info("VR: on_pre_gui_draw_element: nullptr gameobject"));
    }

    return true;
}

void VR::on_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    REF_PROFILE_FUNCTION();

    //spdlog::info("VR: on_gui_draw_element");

#if defined(RE7)
    // [MENU SHEET 14.11.2026] the options GUI's world matrix back as it was before its draw ([fix64] per element, and only
    // if it still holds what was written)
    if (auto it = m_menu_sheet_gui_entries.find(gui_element); it != m_menu_sheet_gui_entries.end() && it->second.pending) {
        if (auto* go = utility::re_component::get_game_object(gui_element); go != nullptr && go->transform != nullptr
            && menu_sheet_same_matrix(go->transform->worldTransform, it->second.written)) {
            go->transform->worldTransform = it->second.original;
        }

        it->second.pending = false;
    }
#endif

    auto context = sdk::get_thread_context();

    // Restore elements back to original states
    for (auto& data : g_elements_to_reset) {
        sdk::call_object_func<void*>(data->view, "set_ViewType", context, data->view, (uint32_t)via::gui::ViewType::Screen);
        sdk::call_object_func<void*>(data->view, "set_Overlay", context, data->view, data->overlay);
        sdk::call_object_func<void*>(data->view, "set_Detonemap", context, data->view, data->detonemap);

        for (auto c : data->hidden_fx) {
            sdk::call_object_func<void*>(c, "set_Visible", context, c, true);
        }
        
        auto game_object = utility::re_component::get_game_object(data->element);

        if (game_object != nullptr && game_object->transform != nullptr) {
            //sdk::set_transform_position(game_object->transform, data->original_position);

            auto& gui_matrix = game_object->transform->worldTransform;

            // [ANCHOR FINAL 04.11.2026] where the element was drawn (every mod's pre callback done): moved by another mod
            // since the VR mod placed it -> that position, and a panel at another distance than the 2D UI distance keeps
            // its pixels at its own depth (as the reticle at 5 m already did)
            if (data->has_anchor) {
                auto a = data->anchor;
                const Vector3f drawn{gui_matrix[3]};

                if (glm::length(drawn - Vector3f{a.pos[0], a.pos[1], a.pos[2]}) > 0.01f && std::isfinite(drawn.x) && std::isfinite(drawn.y)
                    && std::isfinite(drawn.z)) {
                    a.pos[0] = drawn.x;
                    a.pos[1] = drawn.y;
                    a.pos[2] = drawn.z;

                    if (!a.at_target) {
                        const float placed_distance = glm::length(drawn - Vector3f{data->anchor_camera});
                        a.radius_tan = std::abs(placed_distance - m_ui_distance_option->value()) > 0.3f ? 0.20f : 0.0f;
                    }

                    quadview::State::get().gui_anchors_moved.fetch_add(1, std::memory_order_relaxed);
                }

                // [UI MOTION fix125 22.11.2026] the matrix the element was drawn with (every mod's pre callback done: the
                // HUD another mod moved to the weapon has its place there), kept under the frame's number
                std::memcpy(a.mat, &gui_matrix[0][0], sizeof(a.mat));
                a.has_mat = std::isfinite(gui_matrix[0].x) && std::isfinite(gui_matrix[1].y) && std::isfinite(gui_matrix[2].z) &&
                            std::isfinite(gui_matrix[3].x) && std::isfinite(gui_matrix[3].y) && std::isfinite(gui_matrix[3].z);
                quadview::State::get().add_gui_element((int64_t)(uint32_t)m_frame_count, a);
                quadview::State::get().add_gui_anchor(data->anchor_frame, a);
            }

            gui_matrix[3] = data->original_position;
        }
    }

    g_elements_to_reset.clear();
    inside_gui_draw = false;
}

void VR::on_pre_update_before_lock_scene(void* ctx) {
    /*auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto projection_matrix = get_current_projection_matrix(true);

    // Steps towards getting lens flares and volumetric lighting working
    // Get the FOV from the projection matrix
    const auto vfov = glm::degrees(2.0f * std::atan(1.0f / projection_matrix[1][1]));
    const auto aspect = projection_matrix[1][1] / projection_matrix[0][0];
    const auto hfov = vfov * aspect;
    
    spdlog::info("vFOV: {}", vfov);
    spdlog::info("Aspect: {}", aspect);

    sdk::call_object_func<void*>(camera, "set_FOV", sdk::get_thread_context(), camera, vfov);
    sdk::call_object_func<void*>(camera, "set_VerticalEnable", sdk::get_thread_context(), camera, true);
    sdk::call_object_func<void*>(camera, "set_AspectRatio", sdk::get_thread_context(), camera, aspect);*/
}

void VR::on_pre_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = true;

    /*static auto transparent_layer_t = sdk::find_type_definition("via.render.layer.Transparent");
    auto transparent_layer = sdk::renderer::find_layer(transparent_layer_t->type);

    spdlog::info("transparent layer: {:x}", (uintptr_t)transparent_layer);

    if (transparent_layer == nullptr) {
        return;
    }

    static auto scene_layer_t = sdk::find_type_definition("via.render.layer.Scene");
    auto scene_layer = transparent_layer->find_parent(scene_layer_t->type);

    spdlog::info("scene layer: {:x}", (uintptr_t)scene_layer);

    if (scene_layer == nullptr) {
        return;
    }

    scene_layer->update();

    spdlog::info("scene layer update: {:x}", (uintptr_t)(*(void***)scene_layer)[sdk::renderer::RenderLayer::UPDATE_VTABLE_INDEX]);*/
}

void VR::on_lightshaft_draw(void* shaft, void* render_context) {
    m_in_lightshaft = false;
}

thread_local bool timed_out = false;

void VR::on_pre_begin_rendering(void* entry) {
    REF_PROFILE_FUNCTION();


    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    m_in_render = true;

    // Use the gamepad/motion controller sticks to lerp the standing origin back to the center
    if (m_via_hid_gamepad.update()) {
        auto pad = sdk::call_native_func_easy<REManagedObject*>(m_via_hid_gamepad.object, m_via_hid_gamepad.t, "get_LastInputDevice");

        if (pad != nullptr) {
            // Move direction
            // It's not a Vector2f because via.vec2 is not actually 8 bytes, we don't want stack corruption to occur.
            const auto axis_l = (Vector2f)*utility::re_managed_object::get_field<Vector3f*>(pad, "AxisL");
            const auto axis_r = (Vector2f)*utility::re_managed_object::get_field<Vector3f*>(pad, "AxisR");

            // Lerp the standing origin back to HMD position
            // if the user is moving
            if (glm::length(axis_l) > 0.0f || glm::length(axis_r) > 0.0f) {
                const auto highest_length = std::max<float>(glm::length(axis_l), glm::length(axis_r));
                auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

                const auto delta = sdk::Application::get()->get_delta_time();

                new_pos.y = m_standing_origin.y;
                // Don't set the Y because it would look really strange
                m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
            }
        }

        // TODO: do the same thing for the keyboard
        // will probably require some game-specific code
    }

    detect_controllers();

    //actual_frame_count = get_game_frame_count();
    m_frame_count++;
    actual_frame_count = m_frame_count;

    /*if (!inside_on_end) {
        spdlog::info("VR: frame count: {}", m_frame_count);
    } else {
        spdlog::info("VR: frame count: {} (inside on_end)", m_frame_count);
    }*/

    // if we timed out, just return. we're assuming that the rendering will go on as normal
    if (timed_out) {
        if (inside_on_end) {
            REVR_LOG(spdlog::warn("VR: on_pre_wait_rendering: timed out inside_on_end"));
        } else {
            REVR_LOG(spdlog::warn("VR: on_pre_wait_rendering: timed out"));
        }

        return;
    }

    if (runtime->needs_pose_update && inside_on_end) {
        REVR_LOG(spdlog::info("VR: on_pre_wait_rendering: inside on end!"));
    }
    
    // Call WaitGetPoses
    if (is_using_multipass() || (!inside_on_end && m_frame_count % 2 == m_left_eye_interval)) {
        update_hmd_state();
    }

    // [MONO PERIPHERY] geometry of this frame for the periphery reprojection (zoom can change the projections)
    if (is_mono_periphery()) {
        publish_frusta();
    }

    // [MONO PERIPHERY] clone layers the technique does not use, or the view cost probe leaves out, are switched off
    apply_layer_skips();

#if defined(RE7)
    if (!inside_on_end) {
        update_menu_canvas(); // [MENU CANVAS 12.11.2026]
        update_game_vr_menus(); // [GAME VR MENUS 12.11.2026]
    }
#endif

    const auto should_update_camera = (m_frame_count % 2 == m_left_eye_interval)|| is_using_afr() || is_using_multipass();

    if (!inside_on_end && should_update_camera) {
        update_camera();
    } else if (inside_on_end) {
        update_camera_origin();
    }

    // update our internally stored render matrix
    update_render_matrix();
    //fix_temporal_effects(); // BAD way to do it!

#if defined(RE7)
    apply_menu_sheet(); // [MENU SHEET 14.11.2026] after the camera, before the frame is drawn
#endif
}

void VR::on_begin_rendering(void* entry) {
    //spdlog::info("BeginRendering");
}

void VR::on_pre_end_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    if (runtime->ready() && (m_frame_count % 2 == m_left_eye_interval || is_using_multipass())) {
        const auto stage = runtime->get_synchronize_stage();

        if (stage == VRRuntime::SynchronizeStage::LATE && runtime->synchronize_frame() == VRRuntime::Error::SUCCESS) {
            if (runtime->is_openxr()) {
                m_openxr->begin_frame();
            }
        }
    }
}

void VR::on_end_rendering(void* entry) {
    REF_PROFILE_FUNCTION();


    // we set this because we've enabled asynchronous rendering
    // by the time the next frame (right eye) starts,
    // the frame count might get modified, screwing up our logic
    // so we need a render frame count to compare against
    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }

    // TODO: Check later if this is even necessary
    if ((!runtime->ready()) && !inside_on_end) {
        restore_camera();

        inside_on_end = false;
        m_in_render = false;
        return;
    }

    if (!inside_on_end) {
        auto app = sdk::Application::get();

        static auto app_type = sdk::find_type_definition("via.Application");
        static auto set_max_delta_time_fn = app_type->get_method("set_MaxDeltaTime");

        // RE8 and onwards...
        // defaults to 2, and will slow the game down if frame rate is too low
        if (set_max_delta_time_fn != nullptr) {
            // static func, no need for app
            set_max_delta_time_fn->call<void*>(sdk::get_thread_context(), 10.0f);
        }
    }

    if (is_using_afr() || inside_on_end) {
        if (is_using_afr()) {
            restore_camera();
            m_in_render = false;
        }

        return;
    }

    if (is_using_multipass()) {
        restore_camera();
        m_in_render = false;
        inside_on_end = false;

        const auto temporal_upscaler = TemporalUpscaler::get();

        if (temporal_upscaler->activated()) {
            // [STEREO FOVEA] with the quad view engaged these are the composited eye images of the upscaler
            m_multipass.eye_textures[0] = (ID3D12Resource*)temporal_upscaler->get_eye_image(0);
            m_multipass.eye_textures[1] = (ID3D12Resource*)temporal_upscaler->get_eye_image(1);
        } else {
            const auto scene_layers = m_camera_duplicator.get_relevant_scene_layers();

            if (scene_layers.size() < 2) {
                REVR_LOG(spdlog::warn("VR: on_end_rendering: scene layers are less than 2: {}", scene_layers.size()));
                return;
            }

            static auto potype = sdk::find_type_definition("via.render.layer.PrepareOutput")->get_type();

            std::array<sdk::renderer::layer::PrepareOutput**, 2> prepare_output_layers{
                (sdk::renderer::layer::PrepareOutput**)scene_layers[0]->find_layer(potype),
                (sdk::renderer::layer::PrepareOutput**)scene_layers[1]->find_layer(potype)
            };

            if (prepare_output_layers[0] == nullptr || prepare_output_layers[1] == nullptr) {
                REVR_LOG(spdlog::warn("VR: on_end_rendering: prepare output layers are null: {:x} {:x}", (uintptr_t)prepare_output_layers[0], (uintptr_t)prepare_output_layers[1]));
                return;
            }

            std::array<sdk::renderer::TargetState*, 2> output_states{
                (*prepare_output_layers[0])->get_output_state(),
                (*prepare_output_layers[1])->get_output_state()
            };

            if (output_states[0] == nullptr || output_states[1] == nullptr) {
                REVR_LOG(spdlog::warn("VR: on_end_rendering: output states are null: {:x} {:x}", (uintptr_t)output_states[0], (uintptr_t)output_states[1]));
                return;
            }

            if (g_framework->is_dx12()) {
                bool force_reset = false;

                if (m_multipass.allocated_size[0] != get_hmd_width() || m_multipass.allocated_size[1] != get_hmd_height()) {
                    const auto rtv0 = output_states[0]->get_rtv(0);
                    const auto rtv1 = output_states[1]->get_rtv(0);

                    if (rtv0 != nullptr && rtv1 != nullptr) {
                        const auto tex0 = rtv0->get_texture_d3d12();
                        const auto tex1 = rtv1->get_texture_d3d12();

                        if (tex0 != nullptr && tex1 != nullptr) {
                            m_multipass.native_res_copies[0] = tex0->clone();
                            m_multipass.native_res_copies[1] = tex1->clone();

                            m_multipass.allocated_size[0] = get_hmd_width();
                            m_multipass.allocated_size[1] = get_hmd_height();

                            force_reset = true;

                            REVR_LOG(spdlog::info("Allocated native res copies"));
                        } else {
                            REVR_LOG(spdlog::warn("VR: on_end_rendering: texs are null: {:x} {:x}", (uintptr_t)tex0.get(), (uintptr_t)tex1.get()));
                        }
                    } else {
                        REVR_LOG(spdlog::warn("VR: on_end_rendering: rtvs are null: {:x} {:x}", (uintptr_t)rtv0.get(), (uintptr_t)rtv1.get()));
                    }
                }

                if (m_multipass.native_res_copies[0] != nullptr) {
                    const auto container = m_multipass.native_res_copies[0]->get_d3d12_resource_container();

                    if (container != nullptr) {
                        m_multipass.eye_textures[0] = container->get_native_resource();

                        if (m_multipass.eye_textures[0] == nullptr) {
                            REVR_LOG(spdlog::warn("VR: on_end_rendering: eye texture 0 is null"));
                        }
                    }
                }
                
                if (m_multipass.native_res_copies[1] != nullptr) {
                    const auto container = m_multipass.native_res_copies[1]->get_d3d12_resource_container();

                    if (container != nullptr) {
                        m_multipass.eye_textures[1] = container->get_native_resource();

                        if (m_multipass.eye_textures[1] == nullptr) {
                            REVR_LOG(spdlog::warn("VR: on_end_rendering: eye texture 1 is null"));
                        }
                    }
                }

                if (force_reset) {
                    m_d3d12.force_reset();
                }
            } else {
                // TODO!
            }
        }

        return;
    }

    // Only render again on even (left eye) frames
    // We're checking == 1 because at this point, the frame has finished.
    // Meaning the previous frame was a left eye frame.
    if (!inside_on_end && m_render_frame_count % 2 == m_left_eye_interval) {
        inside_on_end = true;
        
        // Try to render again for the right eye
        auto app = sdk::Application::get();

        static auto chain = app->generate_chain("WaitRendering", "EndRendering");
        static bool do_once = true;

        if (do_once) {
            do_once = false;

            // Remove these from the chain (std::vector)
            auto entries_to_remove = std::vector<std::string> {
                "WaitRendering",
                "UpdatePhysicsCharacterController",
                "UpdateTelemetry",
                "UpdateMovie", // Causes movies to play twice as fast if ran again
                "UpdateSpeedTree",
                "UpdateHansoft",
                "UpdatePuppet",
                // The dynamics stuff causes a cloth physics step in the right eye
                "BeginRenderingDynamics",
                "BeginDynamics",
                "EndRenderingDynamics",
                "EndDynamics",
                "EndPhysics",
                "RenderDynamics",
#if TDB_VER < 73
                "RenderLandscape",
#endif
                "DevelopRenderer",
                "DrawWidget"
            };

            for (auto& entry : entries_to_remove) {
                chain.erase(std::remove_if(chain.begin(), chain.end(), [&](auto& func) {
                    return entry == func->description;
                }), chain.end());
            }
        }

        sdk::renderer::wait_rendering();
        sdk::renderer::begin_update_primitive();

        //static auto update_geometry = app->get_function("UpdateGeometry");
        static auto begin_update_effect = app->get_function("BeginUpdateEffect");
        static auto update_effect = app->get_function("UpdateEffect");
        static auto end_update_effect = app->get_function("EndUpdateEffect");
        static auto prerender_gui = app->get_function("PrerenderGUI");
        static auto begin_update_primitive = app->get_function("BeginUpdatePrimitive");

        // SO. Let me explain what's happening here.
        // If we try and just render a frame in this naive way in this order:
        // BeginUpdatePrimitive,
        // WaitRendering,
        // BeginRendering,
        // UpdatePrimitive,
        // EndPrimitive,
        // EndRendering,
        // This will end up having a chance to crash when rendering fluid effects for some reason when calling UpdatePrimitive.
        // The crash happens because some pipeline state inside the fluid simulator gets set to null.
        // So, we manually call BeginEffect, and then EndUpdateEffect
        // Which somehow solves the crash.
        // We don't call UpdateEffect because it will make effects appear to run at a higher framerate
        if (begin_update_effect != nullptr) {
            begin_update_effect->func(begin_update_effect->entry);
        }

        /*if (update_effect != nullptr) {
            update_effect->func(update_effect->entry);
        }*/

        if (end_update_effect != nullptr) {
            end_update_effect->func(end_update_effect->entry);
        }

        if (prerender_gui != nullptr) {
            prerender_gui->func(prerender_gui->entry);
        }

        for (auto func : chain) {
            //spdlog::info("Calling {}", func->description);

            func->func(func->entry);
        }

        restore_camera();

        m_in_render = false;
        inside_on_end = false;
    }
}

void VR::on_pre_wait_rendering(void* entry) {
}

void VR::on_wait_rendering(void* entry) {
    REF_PROFILE_FUNCTION();

    if (!get_runtime()->loaded) {
        return;
    }

    timed_out = false;

    if (!is_hmd_active()) {
        return;
    }

    if (is_using_multipass()) {
        return;
    }

    // wait for m_present_finished (std::condition_variable)
    // to be signaled
    // only on the left eye interval because we need the right eye
    // to start render work as soon as possible
    if (((m_frame_count + 1) % 2) == m_left_eye_interval || is_using_multipass()) {
        if (WaitForSingleObject(m_present_finished_event, 333) == WAIT_TIMEOUT) {
            timed_out = true;
        }

        ResetEvent(m_present_finished_event);
    }
}

std::array<RECamera*, VR::MAX_CAMERAS> VR::get_cameras_for_view_hook() {
    const auto serial = m_entry_serial.load(std::memory_order_relaxed);

    // [CPU 03.11.2026] every thread keeps the list of the current entry: the view hook runs ~540 times per frame in fights
    // (log of 03.11.2026 15:40), from the engine's job threads, and took the lock below every time
    thread_local uint32_t t_serial = 0;
    thread_local std::array<RECamera*, MAX_CAMERAS> t_cameras{};

    if (t_serial == serial && serial != 0) {
        return t_cameras;
    }

    {
        std::scoped_lock _{m_view_cameras_mtx};

        if (m_view_cameras_serial == serial) {
            t_cameras = m_view_cameras;
            t_serial = serial;
            return t_cameras;
        }
    }

    // looked up without the lock held: the lookup goes through other hooks with their own locks
    const auto cameras = get_cameras();

    std::scoped_lock _{m_view_cameras_mtx};
    m_view_cameras = cameras;
    m_view_cameras_serial = serial;
    t_cameras = cameras;
    t_serial = serial;
    return cameras;
}

void VR::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    m_entry_serial.fetch_add(1, std::memory_order_relaxed); // [CPU 22.10.2026] new cameras for the view matrix hook
    t_entry_thread = true;                                   // [VIEW RACE 28.10.2026]
    t_entry_name = name;                                     // [VIEW RACE 03.11.2026] (log)

    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_pre_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_pre_update_hid(entry);
            break;
        case "WaitRendering"_fnv:
            on_pre_wait_rendering(entry);
            break;
#if defined(RE4)
        case "LockScene"_fnv:
            craft_window_push(); // [CRAFT fix128] before the scene is locked for the frame (nothing outside the case's menus)
            shop_menu_probe(m_render_frame_count); // [MERCHANT fix156]
            break;
#endif
        case "BeginRendering"_fnv:
#if defined(RE4)
            ui_probe_publish();   // [UI_SONDE 28.09.2026] Hauptthread -> Lua-Globals
#endif
            on_pre_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_pre_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_application_entry(void* entry, const char* name, size_t hash) {
    t_entry_name = nullptr; // [VIEW RACE 03.11.2026]

    if (!get_runtime()->loaded) {
        return;
    }

    m_camera_duplicator.on_application_entry(entry, name, hash);

    switch (hash) {
        case "UpdateHID"_fnv:
            on_update_hid(entry);
            break;
#if defined(RE7)
        case "LateUpdateBehavior"_fnv:
            update_menu_sheet_objects(); // [MENU SHEET fix62] the options sheet's copy, outside the drawing
            break;
#endif
        case "WaitRendering"_fnv:
            on_wait_rendering(entry);
            break;
        case "BeginRendering"_fnv:
            on_begin_rendering(entry);
            break;
        case "EndRendering"_fnv:
            on_end_rendering(entry);
            break;
        default:
            break;
    }
}

void VR::on_pre_update_hid(void* entry) {
    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    update_action_states();
}

void VR::on_update_hid(void* entry) {
    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

#if not defined(RE2) and not defined(RE3)
    this->openvr_input_to_re_engine();
#endif

#ifdef RE8
    if (get_runtime()->handle_pause) {
        auto padman = sdk::get_managed_singleton<::REManagedObject>(game_namespace("HIDPadManager"));

        if (padman != nullptr) {
            auto merged_pad = sdk::call_object_func_easy<::REManagedObject*>(padman, "get_mergedPad");

            if (merged_pad != nullptr) {
                auto device = sdk::get_object_field<::REManagedObject*>(merged_pad, "Device");

                if (device != nullptr && *device != nullptr) {
                    sdk::call_object_func_easy<void*>(*device, "set_Button", via::hid::GamePadButton::CRight);
                }
            }
        }

        get_runtime()->handle_pause = false;
    }
#endif
}

void VR::openvr_input_to_re2_re3(REManagedObject* input_system) {
    if (!get_runtime()->loaded) {
        return;
    }

    static auto gui_master_type = sdk::find_type_definition(game_namespace("gui.GUIMaster"));
    static auto gui_master_get_instance = gui_master_type->get_method("get_Instance");
    static auto gui_master_get_input = gui_master_type->get_method("get_Input");
    static auto gui_master_input_type = gui_master_get_input->get_return_type();

    // ??????
#ifdef RE2
    static auto input_set_is_trigger_move_up_d = gui_master_input_type->get_method("set_IsTriggerMoveUpD");
    static auto input_set_is_trigger_move_down_d = gui_master_input_type->get_method("set_IsTriggerMoveDownD");
#endif

    auto gui_master = gui_master_get_instance->call<::REManagedObject*>(sdk::get_thread_context());
    auto gui_input = gui_master != nullptr ? 
                     gui_master_get_input->call<::REManagedObject*>(sdk::get_thread_context(), gui_master) : 
                     (::REManagedObject*)nullptr;

    auto ctx = sdk::get_thread_context();

    static auto get_lstick_method = sdk::get_object_method(input_system, "get_LStick");
    static auto get_rstick_method = sdk::get_object_method(input_system, "get_RStick");
    auto lstick = get_lstick_method->call<::REManagedObject*>(ctx);
    auto rstick = get_rstick_method->call<::REManagedObject*>(ctx);

    if (lstick == nullptr || rstick == nullptr) {
        return;
    }

    static auto get_button_bits_method = sdk::get_object_method(input_system, "get_ButtonBits");
    auto button_bits_obj = get_button_bits_method->call<::REManagedObject*>(ctx, input_system);

    if (button_bits_obj == nullptr) {
        return;
    }

    static auto get_active_user_input_unit_method = sdk::get_object_method(input_system, "getActiveUserInputUnit");
    auto input_unit_obj = get_active_user_input_unit_method->call<::REManagedObject*>(ctx, input_system);

    if (input_unit_obj == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto is_using_controller = (now - get_last_controller_update()) <= std::chrono::seconds(10);

    const auto is_grip_down = is_action_active(m_action_grip, m_right_joystick);
    const auto is_trigger_down = is_action_active(m_action_trigger, m_right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, m_left_joystick);
    const auto is_left_trigger_down = is_action_active(m_action_trigger, m_left_joystick);
    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, m_left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, m_right_joystick);

    const auto is_minimap_down = is_action_active(m_action_minimap, m_left_joystick) || is_action_active(m_action_minimap, m_right_joystick);
    const auto is_left_a_button_down = is_action_active(m_action_a_button, m_left_joystick);
    const auto is_left_b_button_down = !is_minimap_down && is_action_active(m_action_b_button, m_left_joystick);
    const auto is_right_a_button_down = is_action_active(m_action_a_button, m_right_joystick);
    const auto is_right_b_button_down = is_action_active(m_action_b_button, m_right_joystick);

    const auto is_dpad_up_down = is_action_active(m_action_dpad_up, m_left_joystick) || is_action_active(m_action_dpad_up, m_right_joystick);
    const auto is_dpad_right_down = is_action_active(m_action_dpad_right, m_left_joystick) || is_action_active(m_action_dpad_right, m_right_joystick);
    const auto is_dpad_down_down = is_action_active(m_action_dpad_down, m_left_joystick) || is_action_active(m_action_dpad_down, m_right_joystick);
    const auto is_dpad_left_down = is_action_active(m_action_dpad_left, m_left_joystick) || is_action_active(m_action_dpad_left, m_right_joystick);

    const auto is_weapon_dial_down = is_action_active(m_action_weapon_dial, m_left_joystick) || is_action_active(m_action_weapon_dial, m_right_joystick);
    const auto is_re3_dodge_down = is_action_active(m_action_re3_dodge, m_left_joystick) || is_action_active(m_action_re3_dodge, m_right_joystick);
    const auto is_quickturn_down = is_action_active(m_action_re2_quickturn, m_left_joystick) || is_action_active(m_action_re2_quickturn, m_right_joystick);
    const auto is_reset_view_down = is_action_active(m_action_re2_reset_view, m_left_joystick) || is_action_active(m_action_re2_reset_view, m_right_joystick);
    const auto is_change_ammo_down = is_action_active(m_action_re2_change_ammo, m_left_joystick) || is_action_active(m_action_re2_change_ammo, m_right_joystick);
	const auto is_toggle_flashlight_down = is_action_active(m_action_re2_toggle_flashlight, m_left_joystick);

    const auto is_left_system_button_down = is_action_active(m_action_system_button, m_left_joystick);
    const auto is_right_system_button_down = is_action_active(m_action_system_button, m_right_joystick);

    
    
#if defined(RE2) || defined(RE3) || defined(RE8)
    if (is_toggle_flashlight_down && !m_was_flashlight_toggle_down) {            
        ManualFlashlight::g_manual_flashlight-> toggle_flashlight();
    }    

    m_was_flashlight_toggle_down = is_toggle_flashlight_down;
#endif


#if defined(RE2) || defined(RE3)
    const auto is_firstperson_toggle_down = is_action_active(m_action_re2_firstperson_toggle, m_left_joystick) || is_action_active(m_action_re2_firstperson_toggle, m_right_joystick);

    if (is_firstperson_toggle_down && !m_was_firstperson_toggle_down) {
        FirstPerson::get()->toggle();
    }

    m_was_firstperson_toggle_down = is_firstperson_toggle_down;
#endif

#if defined(RE2) || defined(RE3)
    const auto is_gripping_weapon = FirstPerson::get()->was_gripping_weapon();
#else
    const auto is_gripping_weapon = false;
#endif

    // Current actual button bits used by the game
    auto& button_bits_down = *sdk::get_object_field<uint64_t>(button_bits_obj, "Down");
    auto& button_bits_on = *sdk::get_object_field<uint64_t>(button_bits_obj, "On");
    auto& button_bits_up = *sdk::get_object_field<uint64_t>(button_bits_obj, "Up");

    // Set button state based on our own history we keep that doesn't get overwritten by the game
    auto set_button_state = [&](app::ropeway::InputDefine::Kind kind, bool state) {
        const auto kind_uint64 = (uint64_t)kind;

        if (state) {
            m_last_controller_update = now;
            is_using_controller = true;

            button_bits_up &= ~kind_uint64;
            m_button_states_up &= ~kind_uint64;

            // if "On" state is not set
            if ((m_button_states_on.to_ullong() & kind_uint64) == 0) {
                if (m_button_states_down.to_ullong() & kind_uint64) {
                    m_button_states_on |= kind_uint64;
                    m_button_states_down &= ~kind_uint64;

                    button_bits_on |= kind_uint64;
                    button_bits_down &= ~kind_uint64;
                } else {
                    m_button_states_on &= ~kind_uint64;
                    m_button_states_down |= kind_uint64;

                    button_bits_on &= ~kind_uint64;
                    button_bits_down |= kind_uint64;
                }
            } else {
                m_button_states_down &= ~kind_uint64;
                button_bits_down &= ~kind_uint64;

                m_button_states_on |= kind_uint64;
                button_bits_on |= kind_uint64;
            }
        } else {
            if (m_button_states_down.to_ullong() & kind_uint64 || m_button_states_on.to_ullong() & kind_uint64) {
                m_button_states_up |= kind_uint64;
                button_bits_up |= kind_uint64;

                m_last_controller_update = now;
                is_using_controller = true;
            } else if (is_using_controller) {
                m_button_states_up &= ~kind_uint64;
                button_bits_up &= ~kind_uint64;
            }

            // Don't want to screw with the user's input if they aren't actively
            // Using their VR controllers
            if (is_using_controller) {
                button_bits_down &= ~kind_uint64;
                m_button_states_down &= ~kind_uint64;
                
                button_bits_on &= ~kind_uint64;
                m_button_states_on &= ~kind_uint64;
            }
        }
    };

    // Right Grip: Aim, UI Right (RB)
    set_button_state(app::ropeway::InputDefine::Kind::HOLD, is_grip_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT, is_grip_down);

    // Left Grip: Alternate aim (grenades, knives, etc), UI left (LB)
    set_button_state(app::ropeway::InputDefine::Kind::SUPPORT_HOLD, is_left_grip_down && !is_gripping_weapon);   
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT, is_left_grip_down);

    // Right Trigger (RB): Attack, Alternate UI right (RT), GE_RTrigBottom (quick time event), GE_RTrigTop (another quick time event)
    set_button_state(app::ropeway::InputDefine::Kind::ATTACK, is_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_RIGHT_2, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)18014398509481984, is_trigger_down);
    set_button_state((app::ropeway::InputDefine::Kind)9007199254740992, is_trigger_down);
    //set_button_state((app::ropeway::InputDefine::Kind)4503599627370496, is_trigger_down);

    // Left Trigger (LB): Alternate UI left (LT), DEFENSE (LB)
    set_button_state(app::ropeway::InputDefine::Kind::UI_SHIFT_LEFT_2, is_left_trigger_down);
    set_button_state(app::ropeway::InputDefine::Kind::DEFENSE, is_left_trigger_down);

    // L3: Sprint
    set_button_state(app::ropeway::InputDefine::Kind::JOG1, is_left_joystick_click_down);

    // R3: Reset camera
    set_button_state(app::ropeway::InputDefine::Kind::RESET_CAMERA, is_reset_view_down);

    // Left B: Inventory, PRESS_START
    set_button_state(app::ropeway::InputDefine::Kind::INVENTORY, is_left_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_b_button_down);

    // Left A: QUICK_TURN, PRESS_START, CANCEL, DIALOG_CANCEL
    set_button_state(app::ropeway::InputDefine::Kind::QUICK_TURN, is_quickturn_down); // unique, unbound by default as it causes issues
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::CANCEL, is_left_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_CANCEL, is_left_a_button_down);
    
    // Right A: Action, ITEM, PRESS_START, DECIDE, DIALOG_DECIDE, (1 << 51)
    set_button_state(app::ropeway::InputDefine::Kind::ACTION, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::ITEM, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::PRESS_START, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DECIDE, is_right_a_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::DIALOG_DECIDE, is_right_a_button_down);

    // only allow dodging if there's nothing to interact with nearby
    if (!is_right_a_button_down || (now - m_last_interaction_display) >= std::chrono::milliseconds(200)) {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), is_re3_dodge_down); // RE3 dodge
    } else {
        set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 51), false); // RE3 dodge
    }
    
    // Right B: Reload, Skip Event, UI_EXCHANGE, UI_RESET, (1 << 52) (that one is RE3 only? don't see it in the enum)
    set_button_state(app::ropeway::InputDefine::Kind::RELOAD, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::SKIP_EVENT, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_EXCHANGE, is_right_b_button_down);
    set_button_state(app::ropeway::InputDefine::Kind::UI_RESET, is_right_b_button_down);
    set_button_state((app::ropeway::InputDefine::Kind)((uint64_t)1 << 52), is_right_b_button_down);

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);

    if (!is_weapon_dial_down) {
        // DPad Up: Shortcut Up
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_UP, is_dpad_up_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_UP, is_dpad_up_down); // IsTriggerMoveUpD in RE3

        // DPad Right: Shortcut Right
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, is_dpad_right_down);

        // DPad Down: Shortcut Down
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_MAP_DOWN, is_dpad_down_down);
        set_button_state(app::ropeway::InputDefine::Kind::UI_DOWN, is_dpad_down_down); // IsTriggerMoveDownD in RE3

        // DPad Left: Shortcut Left
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, is_dpad_left_down);

        // well this was really annoying to figure out
#ifdef RE2
        if (is_dpad_up_down && gui_input != nullptr) {
            input_set_is_trigger_move_up_d->call<void*>(ctx, gui_input, true);
        }

        if (is_dpad_down_down && gui_input != nullptr) {
            input_set_is_trigger_move_down_d->call<void*>(ctx, gui_input, true);
        }
#endif
    } else {
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_UP, left_axis.y > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_RIGHT, left_axis.x > 0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_DOWN, left_axis.y < -0.9f);
        set_button_state(app::ropeway::InputDefine::Kind::SHORTCUT_LEFT, left_axis.x < -0.9f);
    }

    // Change Ammo
    set_button_state(app::ropeway::InputDefine::Kind::CHANGE_BULLET, is_change_ammo_down);

    // MiniMap
    set_button_state(app::ropeway::InputDefine::Kind::MINIMAP, is_minimap_down);

    // Left or Right System Button: Pause
    set_button_state(app::ropeway::InputDefine::Kind::PAUSE, is_left_system_button_down || is_right_system_button_down || get_runtime()->handle_pause);
    get_runtime()->handle_pause = false;

    // Fixes QTE bound to triggers
    if (is_using_controller) {
        const auto left_trigger_state = is_left_trigger_down ? 1.0f : 0.0f;
        const auto right_trigger_state = is_trigger_down ? 1.0f : 0.0f;

        static auto set__analog_l_method = sdk::get_object_method(input_unit_obj, "set__AnalogL");
        static auto set__analog_r_method = sdk::get_object_method(input_unit_obj, "set__AnalogR");
        set__analog_l_method->call<void*>(ctx, input_unit_obj, left_trigger_state);
        set__analog_r_method->call<void*>(ctx, input_unit_obj, right_trigger_state);
    }

    bool moved_sticks = false;
    const auto deadzone = m_joystick_deadzone->value();

    if (left_axis_len > deadzone) {
        moved_sticks = true;

        // Override the left stick's axis values to the VR controller's values
        Vector3f axis{ left_axis.x, left_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(lstick, "update");
        update_method->call<void*>(ctx, lstick, &axis, &axis);
    }

    if (right_axis_len > deadzone) {
        moved_sticks = true;

        // Override the right stick's axis values to the VR controller's values
        Vector3f axis{ right_axis.x, right_axis.y, 0.0f };

        static auto update_method = sdk::get_object_method(rstick, "update");
        update_method->call<void*>(ctx, rstick, &axis, &axis);
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
    }

    set_button_state(app::ropeway::InputDefine::Kind::MOVE, left_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_L_STICK, left_axis_len > deadzone);

    set_button_state(app::ropeway::InputDefine::Kind::WATCH, right_axis_len > deadzone);
    set_button_state(app::ropeway::InputDefine::Kind::UI_R_STICK, right_axis_len > deadzone);
    //set_button_state(app::ropeway::InputDefine::Kind::RUN, right_axis_len > 0.01f);

    // Causes the right stick to take effect properly
    if (is_using_controller) {
        static auto set_input_mode_method = sdk::get_object_method(input_system, "set_InputMode");
        set_input_mode_method->call<void*>(ctx, input_system, app::ropeway::InputDefine::InputMode::Pad);
    }
}

void VR::openvr_input_to_re_engine() {
    // TODO: Get the "merged pad" and actually modify some inputs!

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();
    const auto left_axis_len = glm::length(left_axis);
    const auto right_axis_len = glm::length(right_axis);
    const auto now = std::chrono::steady_clock::now();

    bool moved_sticks = false;

    if (left_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (right_axis_len > m_joystick_deadzone->value()) {
        moved_sticks = true;
    }

    if (moved_sticks) {
        auto new_pos = get_position(vr::k_unTrackedDeviceIndex_Hmd);

        const auto delta = sdk::Application::get()->get_delta_time();
        const auto highest_length = std::max<float>(glm::length(right_axis), glm::length(left_axis));

        new_pos.y = m_standing_origin.y;
        // Don't set the Y because it would look really strange
        m_standing_origin = glm::lerp(m_standing_origin, new_pos, ((float)highest_length * delta) * 0.01f);
        m_last_controller_update = now;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            m_last_controller_update = now;
        }
    }
}

// [TECHNIQUE fix91 19.11.2026] The "Rendering Technique" choice (Single Frame Multipass / Two Frame Sequential /
// AFW (beta)) that was drawn here, at the end of the upscaler's settings, is gone: Single Frame Multipass is the only
// technique the menu offers (see on_config_load).

// [RECENTER_TOP 2026-08-18] Bare button, no tree, drawn as the first thing inside the
// REFramework window. The VR tree it used to live in (on_draw_ui below) never gets drawn
// because Mods.cpp only lets ScriptRunner and TemporalUpscaler through the filter.
// The hotkey path (m_recenter_view_key, VR.cpp on_config_load/on_frame) is untouched.
// [RUECKGABE 10.09.2026] Meldet, ob der Knopf ueberhaupt gezeichnet wurde --
// ohne geladene Runtime zeichnet er nichts. Der Aufrufer haengt die Font-Size
// per SameLine daneben und darf das nur tun, wenn wirklich etwas in der Zeile
// steht, sonst rutscht sie ins Leere.
#if defined(RE4)
// [CRAFT fix159 25.11.2026] the option of the attache case's "Crea" page at the end of MOD OPTIONS (it was in RE4VRUi's
// Miscellaneous section, fix129); in centimetres, 0 = as the game places the item
void VR::draw_craft_push_option() {
    if (g_framework == nullptr) {
        return;
    }

    g_framework->draw_menu_heading("Craft Menu", true);
    float push_cm = m_craft_push_cm->value();

    if (menu_text::SliderFloat("Craft Menu: Item Farther From The Eyes (cm)", &push_cm, 0.0f, 50.0f, "%.0f")) {
        m_craft_push_cm->value() = std::clamp(push_cm, 0.0f, 50.0f);
        g_framework->request_save_config();
    }
}
#endif

bool VR::draw_recenter_button() {
    if (get_runtime() == nullptr || !get_runtime()->loaded) {
        return false;
    }

    if (ImGui::Button("Recenter View")) {
        recenter_view();
    }

    return true;
}

// [RESET VIEW 11.11.2026] see VR.hpp
#if defined(RE7)
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// ============================================================================================================
// [VR RESEARCH 12.11.2026] RE7 still carries the game's own VR mode (PlayStation VR): VrManager / VrModeStatus, a
// "MainMenu_VR", and per-cutscene parameters (Is2DVRMode, IsBlackOutForMotion, CutThreshold, ...). Written to a file to
// find which type holds what and what the game's managers hold while it runs: types whose name has VR / Vr / Hmd, the
// types that have members with those words (with only those members), the main menu types; managed singletons with
// their plain fields (bool / int / float / enum) read, nothing called.
// ============================================================================================================
void VR::write_vr_research_file() {
    const auto tdb = sdk::RETypeDB::get();

    if (tdb == nullptr) {
        return;
    }

    const auto has_vr_word = [](std::string_view n) {
        for (size_t i = 0; i + 1 < n.size(); ++i) {
            const char a = n[i];
            const char b = n[i + 1];
            const bool start = i == 0 || !std::isalpha((unsigned char)n[i - 1]) || std::islower((unsigned char)n[i - 1]) || std::isdigit((unsigned char)n[i - 1]);

            if (a == 'V' && (b == 'R' || b == 'r') && (start || b == 'R')) {
                const char c = i + 2 < n.size() ? n[i + 2] : '\0';

                // not video memory (VRAM) nor variable rate shading (VRS + upper case: VRSSetting, VRSType)
                const char d = i + 3 < n.size() ? n[i + 3] : '\0';

                if (b == 'R' && ((c == 'A' && d == 'M') || (c == 'S' && (d == '\0' || std::isupper((unsigned char)d))))) {
                    continue;
                }

                // VR / Vr followed by an upper case letter, a separator or the end (not "Version", "Vrect" ...)
                if (b == 'R' ? !std::islower((unsigned char)c) || c == '\0' : (std::isupper((unsigned char)c) || c == '\0' || c == '_' || c == '.')) {
                    return true;
                }
            }

            if ((a == 'H' || a == 'h') && (n.substr(i, 3) == "Hmd" || n.substr(i, 3) == "HMD" || n.substr(i, 3) == "hmd") && start) {
                return true;
            }
        }

        return false;
    };

    const auto value_of = [](const sdk::RETypeDefinition* ft, void* p) -> std::string {
        if (ft == nullptr || p == nullptr) {
            return {};
        }

        const auto n = ft->get_full_name();

        if (n == "System.Boolean") {
            return *(bool*)p ? "true" : "false";
        }

        if (n == "System.Int32" || n == "System.UInt32" || ft->is_enum()) {
            return std::to_string(*(int32_t*)p);
        }

        if (n == "System.Single") {
            return std::format("{:.4f}", *(float*)p);
        }

        return {};
    };

    std::string o = std::format("RE7 VR research, {} types in the type database\n\n", tdb->get_num_types());
    std::string by_member{};
    std::string menus{};
    std::string singletons{};
    size_t n_types = 0;

    const auto describe = [&](sdk::RETypeDefinition* t, bool only_vr_members) {
        std::string m{};
        std::string parents{};

        for (auto* pt = t->get_parent_type(); pt != nullptr && parents.size() < 300; pt = pt->get_parent_type()) {
            parents += " < " + pt->get_full_name();
        }

        for (auto* f : t->get_fields()) {
            if (f == nullptr || f->get_name() == nullptr || (only_vr_members && !has_vr_word(f->get_name()))) {
                continue;
            }

            const auto* ft = f->get_type();
            std::string val{};

            if (f->is_static() && f->is_literal()) {
                if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                    val = std::format(" = {}", *d);
                }
            }

            m += std::format("    field {}{}: {}{}\n", f->is_static() ? "static " : "", f->get_name(), ft != nullptr ? ft->get_full_name() : "?", val);
        }

        for (auto& md : t->get_methods()) {
            if (md.get_name() == nullptr || (only_vr_members && !has_vr_word(md.get_name()))) {
                continue;
            }

            std::string params{};

            for (auto* pt : md.get_param_types()) {
                params += (params.empty() ? "" : ", ") + (pt != nullptr ? pt->get_full_name() : std::string{"?"});
            }

            m += std::format("    method {}{}({}) -> {}\n", md.is_static() ? "static " : "", md.get_name(), params,
                md.get_return_type() != nullptr ? md.get_return_type()->get_full_name() : std::string{"?"});
        }

        return std::format("  {}{}\n{}", t->get_full_name(), parents, m);
    };

    for (uint32_t i = 0; i < tdb->get_num_types(); ++i) {
        auto* t = tdb->get_type(i);

        if (t == nullptr) {
            continue;
        }

        const auto name = t->get_full_name();

        if (name.ends_with("[]") || name.find('<') != std::string::npos || name.starts_with("System.")) {
            continue;
        }

        const bool app_or_via = name.starts_with("app.") || name.starts_with("via.");

        if (!app_or_via) {
            continue;
        }

        if (has_vr_word(name)) {
            o += describe(t, false);
            ++n_types;

            if (auto* obj = sdk::get_managed_singleton<::REManagedObject>(name); obj != nullptr) {
                std::string vals{};

                for (auto* f : t->get_fields()) {
                    if (f == nullptr || f->is_static() || f->get_name() == nullptr) {
                        continue;
                    }

                    if (const auto v = value_of(f->get_type(), f->get_data_raw(obj, false)); !v.empty()) {
                        vals += std::format("    {} = {}\n", f->get_name(), v);
                    }
                }

                singletons += std::format("  {} (singleton {:x})\n{}", name, (uintptr_t)obj, vals);
            }

            continue;
        }

        bool member_hit = false;

        for (auto* f : t->get_fields()) {
            member_hit = member_hit || (f != nullptr && f->get_name() != nullptr && has_vr_word(f->get_name()));
        }

        for (auto& md : t->get_methods()) {
            member_hit = member_hit || (md.get_name() != nullptr && has_vr_word(md.get_name()));
        }

        if (member_hit && name.starts_with("app.")) {
            by_member += describe(t, true);
        }

        if (name.starts_with("app.") && (name.find("MainMenu") != std::string::npos || name.find("TitleMenu") != std::string::npos ||
                                         name.find("Title") != std::string::npos && name.find("Camera") != std::string::npos)) {
            menus += describe(t, false);
        }
    }

    o = std::format("{}\n=== {} types named VR / Vr / Hmd (above)\n\n=== app types with VR members (only those members)\n{}\n=== main menu / title types\n{}\n=== "
                    "managed singletons of the VR types, plain fields\n{}",
        o, n_types, by_member, menus, singletons);

    // the app.VrGui components of the scene (the game's PlayStation VR placement of each menu) with their tracking
    // parameters; read only
    std::string vrguis{};

    if (auto* scene = sdk::get_current_scene(); scene != nullptr) {
        auto* vrgui_t = sdk::find_type_definition("app.VrGui");
        auto* param_t = sdk::find_type_definition("app.VrGui.TrackingParam");

        if (vrgui_t != nullptr) {
            auto* arr = sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", vrgui_t->get_runtime_type());
            const auto field = [](sdk::RETypeDefinition* t, void* obj, const char* name) -> void* {
                auto* f = t != nullptr ? t->get_field(name) : nullptr;
                return f != nullptr && obj != nullptr ? f->get_data_raw(obj, false) : nullptr;
            };

            for (int32_t i = 0; arr != nullptr && i < (int32_t)arr->size(); ++i) {
                auto* comp = (::REComponent*)arr->get_element(i);
                auto* go = comp != nullptr ? utility::re_component::get_game_object(comp) : nullptr;

                if (comp == nullptr) {
                    continue;
                }

                const auto go_name = go != nullptr ? utility::narrow(utility::re_string::get_view(go->name)) : std::string{"?"};
                const auto b = [&](const char* n) { auto* p = field(vrgui_t, comp, n); return p != nullptr && *(bool*)p; };
                const auto i32 = [&](const char* n) { auto* p = field(vrgui_t, comp, n); return p != nullptr ? *(int32_t*)p : -1; };
                const auto f32 = [&](const char* n) { auto* p = field(vrgui_t, comp, n); return p != nullptr ? *(float*)p : 0.0f; };

                vrguis += std::format("  {}: IsVrMode {}, DefaultTrackingParamIndex {}, ActiveTrackParamIndex {}, OffsetZ {:.3f}, DistanceMode {}, "
                                      "VrMinDistance {:.3f}, DistanceRate {:.3f}, camMaxDistance {:.3f}\n",
                    go_name, b("_IsVrMode"), i32("_DefaultTrackingParamIndex"), i32("_ActiveTrackParamIndex"), f32("_OffsetZ"),
                    i32("DistanceMode"), f32("VrMinDistance"), f32("DistanceRate"), f32("camMaxDistance"));

                auto* list = field(vrgui_t, comp, "_TrackingParams");
                auto* list_obj = list != nullptr ? *(::REManagedObject**)list : nullptr;

                if (list_obj == nullptr || param_t == nullptr) {
                    continue;
                }

                auto* list_t = utility::re_managed_object::get_type_definition(list_obj);
                auto* items_p = field(list_t, list_obj, "_items");
                auto* size_p = field(list_t, list_obj, "_size");
                auto* items = items_p != nullptr ? *(sdk::SystemArray**)items_p : nullptr;
                const int32_t count = size_p != nullptr ? *(int32_t*)size_p : 0;

                for (int32_t k = 0; items != nullptr && k < count && k < (int32_t)items->size(); ++k) {
                    auto* tp = items->get_element(k);

                    if (tp == nullptr) {
                        continue;
                    }

                    const auto pb = [&](const char* n) { auto* p = field(param_t, tp, n); return p != nullptr && *(bool*)p; };
                    const auto pi = [&](const char* n) { auto* p = field(param_t, tp, n); return p != nullptr ? *(int32_t*)p : -1; };
                    const auto pv = [&](const char* n) {
                        auto* p = (float*)field(param_t, tp, n);
                        return p != nullptr ? std::format("({:.3f}, {:.3f}, {:.3f})", p[0], p[1], p[2]) : std::string{"?"};
                    };

                    vrguis += std::format("    param {}: Mode {}, RotMode {}, TargetType {}, OffsetPos {}, Scale {}, DisableDepthTest {}, EnableBlackBg {}, "
                                          "RefOtherMenu {}\n",
                        k, pi("Mode"), pi("RotMode"), pi("GameObjTargetType"), pv("OffsetPos"), pv("Scale"), pb("DisableDepthTest"),
                        pb("EnableBlackBg"), pb("IsRefStandardPosOtherMenu"));
                }
            }

            o += std::format("\n=== app.VrGui components in the current scene ({})\n{}", arr != nullptr ? (int)arr->size() : 0, vrguis);
        }
    }

    // [MENU VIEW 14.11.2026] the main menu's scene around the game camera: the objects with a mesh within 4 m, the closest to
    // the camera's line of sight first (the options booklet in its close-up, the cassette deck); read only
    if (auto* scene = sdk::get_current_scene(); scene != nullptr && m_menu_camera_active) {
        auto* mesh_t = sdk::find_type_definition("via.render.Mesh");
        auto* camera = sdk::get_primary_camera();
        const auto cam_pos = Vector3f{m_original_camera_position};
        const auto fwd = glm::normalize(m_original_camera_rotation * glm::vec3{0.0f, 0.0f, -1.0f});
        const float fov = camera != nullptr ? sdk::call_object_func_easy<float>((::REManagedObject*)camera, "get_FOV") : 0.0f;
        auto* arr = mesh_t != nullptr ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", mesh_t->get_runtime_type())
                                      : nullptr;

        struct Row {
            float angle{};
            std::string text{};
        };

        std::vector<Row> rows{};
        const int32_t total = arr != nullptr ? (int32_t)arr->size() : 0;

        for (int32_t i = 0; i < total; ++i) {
            auto* comp = (::REComponent*)arr->get_element(i);
            auto* go = comp != nullptr ? utility::re_component::get_game_object(comp) : nullptr;

            if (go == nullptr || go->transform == nullptr) {
                continue;
            }

            const auto pos = sdk::get_transform_position(go->transform);
            const auto d = Vector3f{pos} - cam_pos;
            const float dist = glm::length(d);

            if (dist > 4.0f || dist < 1e-4f) {
                continue;
            }

            const float angle = glm::degrees(std::acos(std::clamp(glm::dot(d / dist, fwd), -1.0f, 1.0f)));
            rows.push_back({angle, std::format("  {:5.1f} deg off the line of sight, {:.2f} m: {} pos {} draw {}\n", angle, dist,
                utility::narrow(utility::re_string::get_view(go->name)), layer_trace::vec3(pos), go->shouldDraw)});
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.angle < b.angle; });
        std::string list{};

        for (size_t i = 0; i < rows.size() && i < 100; ++i) {
            list += rows[i].text;
        }

        o += std::format("\n=== main menu: game camera pos {} {} forward ({:.3f}, {:.3f}, {:.3f}) field of view {:.1f}; objects with a mesh "
                         "within 4 m: {} of {} meshes (the first 100, closest to the line of sight first)\n{}",
            layer_trace::vec3(m_original_camera_position), layer_trace::ypr(m_original_camera_rotation), fwd.x, fwd.y, fwd.z, fov,
            rows.size(), total, list);
    }

    const auto path = REFramework::get_persistent_dir("re7_vr_research.txt");

    if (std::ofstream f{path, std::ios::binary}; f) {
        f << o;
        REVR_LOG(spdlog::info("[VR] research file written: {} ({} bytes)", path.string(), o.size()));
    }
}
#endif
#endif


#if defined(RE7)
// ============================================================================================================
// [GAME VR MENUS 12.11.2026] Experimental. RE7's menus carry an app.VrGui component each: the game's PlayStation VR
// placement (distance, scale, follow mode, offsets; listed in re7_vr_research.txt). It switches them on when
// app.VrGui.is3DVrMode() is true. With the option on (and not while the menu canvas is on) that static method answers
// true, nothing else of the game's VR mode is switched on. Off by default; the hook stays installed once made and
// then lets the original answer through.
// ============================================================================================================
void VR::update_game_vr_menus() {
    const bool want = m_game_vr_menus->value() && is_hmd_active() && !m_flatscreen_overlay;

    if (want && !m_game_vr_menus_hooked) {
        m_game_vr_menus_hooked = true;

        auto* t = sdk::find_type_definition("app.VrGui");
        auto* m = t != nullptr ? t->get_method("is3DVrMode") : nullptr;

        if (m == nullptr) {
            REVR_LOG(spdlog::error("[VR] game VR menus: app.VrGui.is3DVrMode not found"));
            return;
        }

        g_hookman.add(m,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {
                if (s_game_vr_menus_active.load(std::memory_order_relaxed)) {
                    ret_val = 1;
                }
            });

        REVR_LOG(spdlog::info("[VR] game VR menus: app.VrGui.is3DVrMode hooked"));
    }

    if (want != s_game_vr_menus_active.load()) {
        REVR_LOG(spdlog::info("[VR] game VR menus {}", want ? "on: app.VrGui.is3DVrMode answers true" : "off"));
    }

    s_game_vr_menus_active = want;
}
#endif
#if defined(RE7)
// ============================================================================================================
// [MENU CANVAS 12.11.2026] RE7's main menu is a desk seen from above: the cassette deck holds the entries, the options
// are printed in a booklet that the flat game brings into view with its own camera framing. In the headset the view is
// the head's, the booklet stays a few centimetres wide and nothing in it can be read (RE7 test of 12.11.2026). While
// the game camera is the main menu camera ("Camera_mainmenu") the frame the monitor would show is put on the flat
// canvas in front of the head (the existing flatscreen canvas, camera overrides suspended), so the menu reads like
// on a screen; leaving the menu turns the canvas off again (only if the menu turned it on).
// ============================================================================================================
void VR::update_menu_canvas() {
    bool menu = false;
    auto* camera = sdk::get_primary_camera();

    if (camera != nullptr) {
        if (auto* go = utility::re_component::get_game_object((REComponent*)camera); go != nullptr) {
            const auto name = utility::re_string::get_view(go->name);
            menu = name == L"Camera_mainmenu";
        }
    }

    // [MENU VIEW 14.11.2026] the menu camera state for the view height (apply_hmd_transform), and the game camera's framings
    // in the menu (position, direction, field of view) logged as they change: the options booklet is a close-up the flat
    // game makes with its own camera, the data for bringing it closer in stereo
    if (menu != m_menu_camera_active) {
        REVR_LOG(spdlog::info("[VR] main menu camera {}", menu ? "active (main menu view height applies in stereo)" : "left"));
        m_menu_framing_fov = -1.0f;
    }

    m_menu_camera_active = menu;

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    if (menu && camera != nullptr && m_menu_framing_lines < 400) {
        const auto pos = m_original_camera_position;
        const auto rot = m_original_camera_rotation;
        const float fov = sdk::call_object_func_easy<float>((::REManagedObject*)camera, "get_FOV");
        const float moved = glm::length(Vector3f{pos} - Vector3f{m_menu_framing_pos});
        const float turned = glm::degrees(2.0f * std::acos((std::min)(1.0f, std::abs(glm::dot(rot, m_menu_framing_rot)))));

        if (m_menu_framing_fov < 0.0f || moved > 0.02f || turned > 2.0f || std::abs(fov - m_menu_framing_fov) > 0.5f) {
            const auto fwd = rot * glm::vec3{0.0f, 0.0f, -1.0f};
            REVR_LOG(spdlog::info("[VR] main menu framing: game camera pos {} {} forward ({:.3f}, {:.3f}, {:.3f}) field of view {:.1f}",
                layer_trace::vec3(pos), layer_trace::ypr(rot), fwd.x, fwd.y, fwd.z, fov));
            m_menu_framing_pos = pos;
            m_menu_framing_rot = rot;
            m_menu_framing_fov = fov;
            ++m_menu_framing_lines;
        }
    }
#endif

    // [MENU SHEET 14.11.2026] The options: the game animates its camera from the deck (pitch -51, 0.46 m from the cassette
    // label) to 12 cm above the label (pitch -79) in ~1 s. m_menu_closeup is that way's progress, 0 at the deck .. 1 at the
    // label: from the camera's distance to the label when the label is known, else from the pitch. The deck's pose is
    // recorded while the camera is there (less than 52 deg down, away from the label); while the camera is on its way to
    // the label or at it, and the options GUI was drawn in the last 2 s, the view is held at that pose (apply_hmd_transform)
    // and the sheet is brought up instead (apply_menu_sheet).
    {
        float closeup = 0.0f;
        bool hold = false;
        const auto cam_pos = m_original_camera_position;
        const auto fwd = m_original_camera_rotation * glm::vec3{0.0f, 0.0f, -1.0f};
        const float down = glm::degrees(std::asin(std::clamp(-fwd.y, -1.0f, 1.0f)));
        const auto now = std::chrono::steady_clock::now();

        if (!menu) {
            m_menu_deck_valid = false;
            m_menu_label = nullptr; // searched again in the next main menu (another scene may hold it)
            m_menu_label_searches = 0;
        } else {
            if (m_menu_label == nullptr && m_menu_label_searches < 40 && (uint32_t)m_frame_count - m_menu_label_search_frame >= 30u) {
                m_menu_label_search_frame = (uint32_t)m_frame_count;
                ++m_menu_label_searches;
                find_menu_sheet_label();
            }

            float label_dist = -1.0f;

            if (m_menu_label != nullptr && m_menu_label->transform != nullptr) {
                // [fix63] where the label lies on the desk (the saved pose while it is moved up: else this distance follows
                // the move and the progress flips every frame, log of 14.11.2026 19:56)
                const auto lies = m_menu_sheet_applied && m_menu_sheet_saved.transform == m_menu_label->transform
                    ? m_menu_sheet_saved.pos
                    : sdk::get_transform_position(m_menu_label->transform);
                label_dist = glm::length(Vector3f{cam_pos} - Vector3f{lies});
            }

            // the deck: not steep, away from the label (and not the camera's first frame at the origin, 43 m off)
            if (down < 52.0f && (label_dist < 0.0f || (label_dist > 0.40f && label_dist < 2.0f))) {
                if (!m_menu_deck_valid) {
                    REVR_LOG(spdlog::info("[VR] main menu: deck pose recorded: game camera pos {} {}, {:.3f} m from the cassette label",
                        layer_trace::vec3(cam_pos), layer_trace::ypr(m_original_camera_rotation), label_dist));
                }

                m_menu_deck_valid = true;
                m_menu_deck_pos = cam_pos;
                m_menu_deck_rot = m_original_camera_rotation;
                m_menu_deck_label_dist = label_dist;
            }

            if (m_menu_deck_valid && label_dist >= 0.0f && m_menu_deck_label_dist > 0.2f) {
                const float x = std::clamp((m_menu_deck_label_dist - label_dist) / (m_menu_deck_label_dist - 0.13f), 0.0f, 1.0f);
                closeup = x * x * (3.0f - 2.0f * x);
                hold = m_menu_deck_label_dist - label_dist > 0.005f;
            } else {
                const float x = std::clamp((down - 52.0f) / (77.0f - 52.0f), 0.0f, 1.0f);
                closeup = x * x * (3.0f - 2.0f * x);
                hold = m_menu_deck_valid && down >= 52.0f;
            }

            const bool options_recent = m_menu_option_gui.load() != nullptr
                && now.time_since_epoch().count() - m_menu_option_gui_seen_ns.load()
                    < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(2)).count();

            if (!options_recent) {
                closeup = 0.0f;
                hold = false;
            }

            hold = hold && m_menu_deck_valid && m_menu_sheet->value() && !m_flatscreen_overlay;
        }

        if ((closeup >= 0.5f) != (m_menu_closeup >= 0.5f)) {
            REVR_LOG(spdlog::info("[VR] main menu: options close-up {}", closeup >= 0.5f ? "entered" : "left"));
        }

        if (hold != m_menu_hold) {
            REVR_LOG(spdlog::info("[VR] main menu: view {} (game camera pos {} {})", hold ? "held at the deck, the options sheet comes up" : "follows the game camera again",
                layer_trace::vec3(cam_pos), layer_trace::ypr(m_original_camera_rotation)));
        }

        m_menu_closeup = closeup;
        m_menu_hold = hold;
    }

    if (!is_hmd_active() || (!m_menu_canvas->value() && !m_movie_canvas->value())) {
        if (m_menu_canvas_on) {
            set_flatscreen_overlay(false);
            set_flatscreen_overlay_aspect(0.0f);
            m_menu_canvas_logged_aspect = -1.0f;
            m_menu_canvas_on = false;
            REVR_LOG(spdlog::info("[VR] main menu / movie canvas off (options off or headset inactive)"));
        }

        return;
    }

    // [MOVIE CANVAS 14.11.2026] The movie before the main menu is drawn over the whole 3400x3468 headset frame: a 16:9
    // picture stretched upwards (user, 14.11.2026). RE7's PlayStation VR code puts such movies on a flat screen and asks
    // for it through app.vr.VrSystemService.requestStart2DVrMode(Movie) / requestStop2DVrMode(Movie). Those two are
    // hooked (logged, with their request type) and while a Movie request stands the frame goes on the canvas in 16:9,
    // like the main menu. Whether the PC game still makes the requests is what the log of the next test shows.
    if (!m_2dvr_hooked) {
        m_2dvr_hooked = true;

        auto* t = sdk::find_type_definition("app.vr.VrSystemService");

        for (const char* name : {"requestStart2DVrMode", "requestStop2DVrMode"}) {
            auto* m = t != nullptr ? t->get_method(name) : nullptr;

            if (m == nullptr) {
                REVR_LOG(spdlog::error("[VR] movie canvas: app.vr.VrSystemService.{} not found", name));
                continue;
            }

            const bool start = std::string_view{name} == "requestStart2DVrMode";

            g_hookman.add(m,
                [start](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>& arg_tys, uintptr_t ret_addr) {
                    // instance method: [0] thread context, [1] this, [2] the request type (flag)
                    if (args.size() >= 3) {
                        const auto type = (uint32_t)args[2];
                        const auto before = s_2dvr_requests.load();
                        const auto after = start ? (before | type) : (before & ~type);
                        s_2dvr_requests = after;

                        // the game repeats a standing request every frame: a line per change, and once per type and kind
                        static std::atomic<uint32_t> s_logged[2]{};
                        const bool first = (s_logged[start ? 1 : 0].fetch_or(type) & type) != type;

                        if (after != before || first) {
                            REVR_LOG(spdlog::info("[VR] game 2D VR request: {} type 0x{:x}, requests 0x{:x} -> 0x{:x}", start ? "start" : "stop", type,
                                before, after));
                        }
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret_val, sdk::RETypeDefinition* ret_ty, uintptr_t ret_addr) {});

            REVR_LOG(spdlog::info("[VR] movie canvas: app.vr.VrSystemService.{} hooked", name));
        }
    }

    constexpr uint32_t REQUEST_2DVR_MOVIE = 8; // app.vr.VrSystemService.Request2DVRTypeEnum.Movie
    // the title movie plays with the menu camera: a standing movie request goes first, in 16:9
    const bool movie = m_movie_canvas->value() && (s_2dvr_requests.load(std::memory_order_relaxed) & REQUEST_2DVR_MOVIE) != 0;
    const bool menu_screen = m_menu_canvas->value() && menu && !movie;

    if (movie) {
        constexpr float MOVIE_ASPECT = 16.0f / 9.0f;
        set_flatscreen_overlay_aspect(MOVIE_ASPECT);

        if (std::abs(MOVIE_ASPECT - m_menu_canvas_logged_aspect) > 0.01f) {
            m_menu_canvas_logged_aspect = MOVIE_ASPECT;
            REVR_LOG(spdlog::info("[VR] movie canvas: picture shape {:.3f} (16:9)", MOVIE_ASPECT));
        }
    }

    // [CANVAS ASPECT 12.11.2026] the shape of the menu picture: by hand (slider > 0) or the shape the game camera renders
    // with (its own projection: the camera overrides are suspended while the canvas runs). On the near-square headset
    // texture a wider picture looked stretched upwards (test of 12.11.2026).
    if (menu_screen) {
        float aspect = m_menu_canvas_aspect->value();

        if (aspect < 0.2f) {
            aspect = 0.0f;
            static const auto camera_t = sdk::find_type_definition("via.Camera");
            static const auto get_projection = camera_t != nullptr ? camera_t->get_method("get_ProjectionMatrix") : nullptr;

            if (get_projection != nullptr && m_menu_canvas_on) {
                const auto r = get_projection->invoke((::REManagedObject*)camera, std::span<void*>{});

                if (!r.exception_thrown) {
                    Matrix4x4f p{};
                    std::memcpy(&p, r.bytes.data(), sizeof(p));

                    if (std::abs(p[0][0]) > 1e-4f && std::abs(p[1][1]) > 1e-4f) {
                        aspect = std::abs(p[1][1] / p[0][0]);
                    }
                }
            }
        }

        set_flatscreen_overlay_aspect(aspect);

        if (std::abs(aspect - m_menu_canvas_logged_aspect) > 0.01f) {
            m_menu_canvas_logged_aspect = aspect;
            REVR_LOG(spdlog::info("[VR] main menu canvas: picture shape {:.3f} ({})", aspect,
                m_menu_canvas_aspect->value() >= 0.2f ? "set by hand" : "from the game camera's projection"));
        }
    }

    const bool show = menu_screen || movie;

    if (show && !m_menu_canvas_on) {
        if (!is_flatscreen_overlay()) {
            set_flatscreen_overlay_width(m_menu_canvas_width->value());
            set_flatscreen_overlay_distance(m_menu_canvas_distance->value());
            request_flatscreen_reanchor();
            set_flatscreen_overlay(true);
            m_menu_canvas_on = true;
            REVR_LOG(spdlog::info("[VR] {}: shown on the canvas ({:.2f} m wide, {:.2f} m away)", movie ? "movie" : "main menu",
                m_menu_canvas_width->value(), m_menu_canvas_distance->value()));
        }
    } else if (!show && m_menu_canvas_on) {
        set_flatscreen_overlay(false);
        set_flatscreen_overlay_aspect(0.0f);
        m_menu_canvas_logged_aspect = -1.0f;
        m_menu_canvas_on = false;
        REVR_LOG(spdlog::info("[VR] main menu / movie left: canvas off"));
    } else if (show && m_menu_canvas_on) {
        // the sliders apply live
        set_flatscreen_overlay_width(m_menu_canvas_width->value());
        set_flatscreen_overlay_distance(m_menu_canvas_distance->value());
    }
}

// ============================================================================================================
// [MENU SHEET 14.11.2026] The options sheet of RE7's main menu. In the options the game takes its camera 12 cm above
// the cassette label lying on the desk; the monitor then shows the label large. In the headset that close-up made the
// whole room look huge (fix58: the view at the game camera, a smaller base between the eyes); the user wants only the
// sheet enlarged (14.11.2026). Here the view stays at the deck (m_menu_hold, apply_hmd_transform) and the sheet - the
// mesh "sm0376_CassettLabel01A_Gimmick" and the options GUI "GUI_option" drawn on it - is moved by one similarity about
// the label's origin: what the game camera sees of it is put in front of the head as the game camera sees it (same turn,
// so the text stands as on the monitor), m_menu_sheet_size times larger, its middle m_menu_sheet_distance from the head,
// straight ahead of the head, m_menu_sheet_below degrees below eye level. The move grows with the game camera's way to
// the label (m_menu_closeup).
// [fix63] The engine did not draw the label where its transform was set (fix59-61), nor hide it by its game object's
// draw flag, nor draw a copy of it (fix62): its via.render.Mesh is a static mesh (StaticMesh, list of the methods in the
// log of 14.11.2026 21:30). While the sheet is up the mesh is switched to dynamic (update_menu_sheet_objects) and the
// label itself is moved; back on the desk its pose and then its flags are set back.
// ============================================================================================================

void VR::find_menu_sheet_label() {
    auto* scene = sdk::get_current_scene();
    auto* mesh_t = sdk::find_type_definition("via.render.Mesh");

    if (scene == nullptr || mesh_t == nullptr) {
        return;
    }

    auto* arr = sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", mesh_t->get_runtime_type());
    const int32_t total = arr != nullptr ? (int32_t)arr->size() : 0;
    ::REGameObject* exact = nullptr;
    ::REGameObject* similar = nullptr;

    for (int32_t i = 0; i < total && exact == nullptr; ++i) {
        auto* comp = (::REComponent*)arr->get_element(i);
        auto* go = comp != nullptr ? utility::re_component::get_game_object(comp) : nullptr;

        if (go == nullptr || go->transform == nullptr) {
            continue;
        }

        const auto name = utility::re_string::get_view(go->name);

        if (name == L"sm0376_CassettLabel01A_Gimmick") {
            exact = go;
        } else if (similar == nullptr && name.find(L"CassettLabel") != std::wstring_view::npos) {
            similar = go;
        }
    }

    m_menu_label = exact != nullptr ? exact : similar;

    if (m_menu_label == nullptr) {
        if (m_menu_label_searches == 1 || m_menu_label_searches % 10 == 0) {
            REVR_LOG(spdlog::warn("[VR] options sheet: no cassette label mesh in the scene ({} meshes, search {})", total, m_menu_label_searches));
        }

        return;
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    auto* tf = m_menu_label->transform;
    auto* joints = sdk::get_transform_joints(tf);
    REVR_LOG(spdlog::info("[VR] options sheet: label \"{}\" pos {} {} local scale ({:.3f}, {:.3f}, {:.3f}), {} joints, parents{}",
        utility::narrow(utility::re_string::get_view(m_menu_label->name)), layer_trace::vec3(sdk::get_transform_position(tf)),
        layer_trace::ypr(sdk::get_transform_rotation(tf)), menu_sheet_local_scale(tf).x, menu_sheet_local_scale(tf).y,
        menu_sheet_local_scale(tf).z, joints != nullptr ? (int32_t)joints->size() : -1, menu_sheet_chain(tf)));
#endif
}

namespace {
sdk::REMethodDefinition* menu_sheet_method(const char* type_name, std::initializer_list<const char*> names) {
    auto* t = sdk::find_type_definition(type_name);

    if (t == nullptr) {
        return nullptr;
    }

    for (const char* n : names) {
        if (auto* m = t->get_method(n); m != nullptr) {
            return m;
        }
    }

    return nullptr;
}

void menu_sheet_clear_vm_exception() {
    auto context = sdk::get_thread_context();

    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
    }
}
} // namespace

// [MENU SHEET fix63] Runs after LateUpdateBehavior (game thread, outside the drawing). The cassette label is a static
// mesh for the renderer: its transform is ignored, and so is its game object's draw flag (fix59-62: moved, hidden, the
// paper stayed on the desk; a copy of it was not drawn at all). via.render.Mesh has StaticMesh / ForceDynamicMesh: while
// the sheet is up the label's mesh is switched to dynamic (off and on again so the renderer takes it anew), so the move
// of the label (apply_menu_sheet) reaches the picture - the paper itself comes up and nothing stays on the desk. Back on
// the desk, after its pose is set back, the mesh gets its own flags again.
void VR::update_menu_sheet_objects() {
    auto* ctx = sdk::get_thread_context();
    static auto* mesh_t = sdk::find_type_definition("via.render.Mesh");
    static auto* mesh_get_static = menu_sheet_method("via.render.Mesh", {"get_StaticMesh"});
    static auto* mesh_set_static = menu_sheet_method("via.render.Mesh", {"set_StaticMesh"});
    static auto* mesh_get_force_dyn = menu_sheet_method("via.render.Mesh", {"get_ForceDynamicMesh"});
    static auto* mesh_set_force_dyn = menu_sheet_method("via.render.Mesh", {"set_ForceDynamicMesh"});
    static auto* mesh_get_enabled = menu_sheet_method("via.render.Mesh", {"get_Enabled"});
    static auto* mesh_set_enabled = menu_sheet_method("via.render.Mesh", {"set_Enabled"});
    static auto* mesh_get_dynamic = menu_sheet_method("via.render.Mesh", {"get_DynamicMesh"});
    static auto* mesh_get_ready = menu_sheet_method("via.render.Mesh", {"get_MeshReady"});
    static auto* mesh_get_instancing = menu_sheet_method("via.render.Mesh", {"get_EnabledInstancing"});
    static auto* mesh_get_parts = menu_sheet_method("via.render.Mesh", {"getPartsEnableCount"});

    const auto state = [&](::REManagedObject* mesh) -> std::string {
        const auto b = [&](sdk::REMethodDefinition* m) -> std::string {
            if (m == nullptr) {
                return "?";
            }

            const bool v = m->call_safe<bool>(ctx, mesh);
            menu_sheet_clear_vm_exception();
            return v ? "on" : "off";
        };

        std::string parts{"?"};

        if (mesh_get_parts != nullptr) {
            parts = std::to_string(mesh_get_parts->call_safe<uint32_t>(ctx, mesh));
            menu_sheet_clear_vm_exception();
        }

        return std::format("static {}, force dynamic {}, dynamic {}, enabled {}, ready {}, instancing {}, enabled parts {}", b(mesh_get_static),
            b(mesh_get_force_dyn), b(mesh_get_dynamic), b(mesh_get_enabled), b(mesh_get_ready), b(mesh_get_instancing), parts);
    };

    const bool want = m_menu_sheet_copy_wanted && m_menu_camera_active && m_menu_label != nullptr && m_menu_label->transform != nullptr;

    if (!want) {
        // back on the desk: the pose first (apply_menu_sheet set it back already), then the label's own flags
        if (m_menu_sheet_label_hidden && m_menu_sheet_hidden_mesh != nullptr && !m_menu_sheet_applied) {
            auto* mesh = m_menu_sheet_hidden_mesh;

            if (mesh_set_enabled != nullptr) {
                mesh_set_enabled->call_safe<void*>(ctx, mesh, false);
            }

            if (mesh_set_force_dyn != nullptr) {
                mesh_set_force_dyn->call_safe<void*>(ctx, mesh, m_menu_sheet_label_force_dyn);
            }

            if (mesh_set_static != nullptr) {
                mesh_set_static->call_safe<void*>(ctx, mesh, m_menu_sheet_label_static);
            }

            if (mesh_set_enabled != nullptr) {
                mesh_set_enabled->call_safe<void*>(ctx, mesh, m_menu_sheet_label_enabled);
            }

            menu_sheet_clear_vm_exception();
            REVR_LOG(spdlog::info("[VR] options sheet: label mesh back to its own flags: {}", state(mesh)));
            m_menu_sheet_label_hidden = false;
            m_menu_sheet_hidden_mesh = nullptr;
            m_menu_sheet_hidden_go = nullptr;
        }

        return;
    }

    if (m_menu_sheet_label_hidden) {
        return; // already dynamic
    }

    auto* mesh = mesh_t != nullptr ? utility::re_component::find(m_menu_label->transform, mesh_t->get_type()) : nullptr;

    if (mesh == nullptr) {
        if (!m_menu_sheet_copy_logged) {
            m_menu_sheet_copy_logged = true;
            REVR_LOG(spdlog::error("[VR] options sheet: the label has no via.render.Mesh component"));
        }

        return;
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    const auto before = state(mesh);
#endif
    m_menu_sheet_label_static = mesh_get_static != nullptr && mesh_get_static->call_safe<bool>(ctx, mesh);
    m_menu_sheet_label_force_dyn = mesh_get_force_dyn != nullptr && mesh_get_force_dyn->call_safe<bool>(ctx, mesh);
    m_menu_sheet_label_enabled = mesh_get_enabled == nullptr || mesh_get_enabled->call_safe<bool>(ctx, mesh);
    menu_sheet_clear_vm_exception();

    if (mesh_set_enabled != nullptr) {
        mesh_set_enabled->call_safe<void*>(ctx, mesh, false);
    }

    if (mesh_set_static != nullptr) {
        mesh_set_static->call_safe<void*>(ctx, mesh, false);
    }

    if (mesh_set_force_dyn != nullptr) {
        mesh_set_force_dyn->call_safe<void*>(ctx, mesh, true);
    }

    if (mesh_set_enabled != nullptr) {
        mesh_set_enabled->call_safe<void*>(ctx, mesh, true);
    }

    menu_sheet_clear_vm_exception();
    m_menu_sheet_label_hidden = true; // (here: "switched to dynamic")
    m_menu_sheet_hidden_mesh = mesh;
    m_menu_sheet_hidden_go = m_menu_label;
    REVR_LOG(spdlog::info("[VR] options sheet: label mesh switched to dynamic; before: {}; now: {}", before, state(mesh)));
}

void VR::apply_menu_sheet() {
    const bool want = m_menu_sheet->value() && m_menu_hold && m_menu_camera_active && !m_flatscreen_overlay && m_menu_closeup > 0.001f
        && m_menu_label != nullptr && m_menu_label->transform != nullptr && is_hmd_active();

    if (!want) {
        m_menu_sheet_copy_wanted = false; // [fix63] the label's mesh gets its own flags back (update_menu_sheet_objects)

        // the label's pose on the desk again (setters and world matrix)
        if (m_menu_sheet_applied) {
            m_menu_sheet_applied = false;
            const auto& sv = m_menu_sheet_saved;

            if (sv.transform != nullptr) {
                menu_sheet_set_local_scale(sv.transform, sv.local_scale);
                sdk::set_transform_rotation(sv.transform, sv.rot);
                sdk::set_transform_position(sv.transform, sv.pos);
                sv.transform->worldTransform = sv.world;
            }
        }

        if (m_menu_sheet_engaged) {
            m_menu_sheet_engaged = false;
            REVR_LOG(spdlog::info("[VR] options sheet: back on the desk"));

            std::scoped_lock _{m_menu_sheet_gui_mtx};
            m_menu_sheet_gui_valid = false;
            m_menu_sheet_up_for_gui = false;
        }

        return;
    }

    auto* tf = m_menu_label->transform;
    const float e = std::clamp(m_menu_closeup, 0.0f, 1.0f);
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    const bool gui_seen = m_menu_option_gui.load() != nullptr
        && now_ns - m_menu_option_gui_seen_ns.load() < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(500)).count();
#endif

    if (!m_menu_sheet_engaged) {
        m_menu_sheet_engaged = true;
        m_menu_sheet_logged_full = false;
        m_menu_sheet_logged_gui = false;
        // the head where the view was drawn from (already held at the deck), kept while the sheet is up
        m_menu_sheet_eye = m_render_camera_matrix[3];
        m_menu_sheet_head_fwd = -m_render_camera_matrix[2];

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        // what lies around the label, with the hierarchy (one listing per opening of the options, the first 4)
        if (m_menu_sheet_log_lines < 4) {
            ++m_menu_sheet_log_lines;
            auto* scene = sdk::get_current_scene();
            const Vector3f label_pos{sdk::get_transform_position(tf)};
            std::string list{};

            for (const char* type_name : {"via.render.Mesh", "via.gui.GUI"}) {
                auto* t = sdk::find_type_definition(type_name);
                auto* arr = scene != nullptr && t != nullptr
                    ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", t->get_runtime_type())
                    : nullptr;
                const float radius = std::string_view{type_name} == "via.gui.GUI" ? 1.5f : 0.35f;
                const int32_t total = arr != nullptr ? (int32_t)arr->size() : 0;

                for (int32_t i = 0; i < total; ++i) {
                    auto* comp = (::REComponent*)arr->get_element(i);
                    auto* go = comp != nullptr ? utility::re_component::get_game_object(comp) : nullptr;

                    if (go == nullptr || go->transform == nullptr) {
                        continue;
                    }

                    const auto pos = sdk::get_transform_position(go->transform);
                    const float dist = glm::length(Vector3f{pos} - label_pos);

                    if (dist <= radius) {
                        list += std::format("\n    {} {} {:.3f} m from the label, pos {}, draw {}, parents{}", type_name,
                            utility::narrow(utility::re_string::get_view(go->name)), dist, layer_trace::vec3(pos), go->shouldDraw,
                            menu_sheet_chain(go->transform));
                    }
                }
            }

            REVR_LOG(spdlog::info("[VR] options sheet: coming up; head at {}, label at {}; around the label:{}", layer_trace::vec3(m_menu_sheet_eye),
                layer_trace::vec3(Vector4f{label_pos, 1.0f}), list));
        }
#endif
    }

    // the label's pose on the desk, saved when the move starts ([fix63] the label itself is moved again, its mesh made
    // dynamic in update_menu_sheet_objects; the saved pose stays the source of each frame's move)
    auto& s = m_menu_sheet_saved;

    if (!m_menu_sheet_applied || s.transform != tf) {
        s.transform = tf;
        s.pos = sdk::get_transform_position(tf);
        s.rot = sdk::get_transform_rotation(tf);
        s.local_scale = menu_sheet_local_scale(tf);
        s.world = tf->worldTransform;
    }

    m_menu_sheet_copy_wanted = true;

    const glm::vec3 eye{m_menu_sheet_eye};
    const glm::vec3 pivot{s.pos};
    const glm::vec3 cam{m_original_camera_position};
    const glm::quat q_game = glm::normalize(m_original_camera_rotation);
    const glm::vec3 fwd_game = q_game * glm::vec3{0.0f, 0.0f, -1.0f};

    // [fix61] straight ahead of the head (its direction when the sheet started to come up), m_menu_sheet_below degrees
    // below eye level (fix59/60 put it in the label's direction: to the right of the view, user 14.11.2026 19:39)
    glm::vec3 dir{};
    {
        glm::vec3 ahead{m_menu_sheet_head_fwd.x, 0.0f, m_menu_sheet_head_fwd.z};

        if (glm::length(ahead) < 1e-3f) {
            ahead = glm::vec3{pivot.x - eye.x, 0.0f, pivot.z - eye.z};
        }

        if (glm::length(ahead) < 1e-3f) {
            return;
        }

        ahead = glm::normalize(ahead);
        const float below = glm::radians(m_menu_sheet_below->value());
        dir = ahead * std::cos(below) + glm::vec3{0.0f, -std::sin(below), 0.0f};
    }

    // the view's frame looking along dir: -Z forward, +Y up (the camera convention of the game)
    const glm::vec3 z = -dir;
    const glm::vec3 x = glm::normalize(glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, z));
    const glm::vec3 y = glm::cross(z, x);
    const glm::quat q_view = glm::normalize(glm::quat_cast(glm::mat3{x, y, z}));

    // the game camera's frame onto the view's: what the game camera sees ahead at distance d is then seen ahead of the
    // head at distance d * size, shifted so the point the game camera looks at on the label comes to `distance`
    const float size = m_menu_sheet_size->value();
    const float distance = m_menu_sheet_distance->value();
    const glm::quat turn = glm::normalize(q_view * glm::inverse(q_game));
    const float look_dist = (std::max)(glm::dot(pivot - cam, fwd_game), 0.0f);
    const glm::vec3 anchor = eye + dir * (distance - size * look_dist);
    const glm::vec3 pivot_up = anchor + turn * ((pivot - cam) * size);

    const glm::vec3 pos_e = glm::mix(pivot, pivot_up, e);
    const glm::quat turn_e = glm::normalize(glm::slerp(glm::identity<glm::quat>(), turn, e));
    const float size_e = glm::mix(1.0f, size, e);

    const Matrix4x4f xform = glm::translate(Matrix4x4f{1.0f}, pos_e) * Matrix4x4f{turn_e} * glm::scale(Matrix4x4f{1.0f}, glm::vec3{size_e})
        * glm::translate(Matrix4x4f{1.0f}, -pivot);

    const auto new_rot = glm::normalize(turn_e * s.rot);
    const Vector4f new_scale{s.local_scale.x * size_e, s.local_scale.y * size_e, s.local_scale.z * size_e, 0.0f};

    // the label moved: the setters reach the engine's next transform update, the world matrix written with them this
    // frame's picture (both only once its mesh is dynamic, update_menu_sheet_objects)
    auto* copy_tf = tf;
    menu_sheet_set_local_scale(tf, new_scale);
    sdk::set_transform_rotation(tf, new_rot);
    sdk::set_transform_position(tf, Vector4f{pos_e, 1.0f});
    tf->worldTransform = xform * s.world;
    m_menu_sheet_applied = true;

    {
        // the same move for the options GUI's draws (on_pre_gui_draw_element, render thread)
        std::scoped_lock _{m_menu_sheet_gui_mtx};
        m_menu_sheet_gui_valid = true;
        m_menu_sheet_up_for_gui = m_menu_closeup > 0.5f;
        m_menu_sheet_heading = Vector3f{dir.x, 0.0f, dir.z};
        m_menu_sheet_gui_x = xform;
        m_menu_sheet_label_up_pos = Vector4f{pos_e, 1.0f};
        m_menu_sheet_label_down_pos = s.pos;
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    if (e > 0.99f && !m_menu_sheet_logged_full && copy_tf != nullptr) {
        m_menu_sheet_logged_full = true;
        const auto& wt = copy_tf->worldTransform;
        REVR_LOG(spdlog::info("[VR] options sheet: up, x{:.2f} at {:.2f} m; head {}, direction ({:.3f}, {:.3f}, {:.3f}); game camera {} {} looks {:.3f} m "
                     "to the label; label from {} to {}; read back: transform {}, world matrix {} scale {:.3f}; label mesh {} "
                     "(draw self {}); options GUI {}",
            size, distance, layer_trace::vec3(m_menu_sheet_eye), dir.x, dir.y, dir.z, layer_trace::vec3(m_original_camera_position),
            layer_trace::ypr(q_game), look_dist, layer_trace::vec3(s.pos), layer_trace::vec3(Vector4f{pos_e, 1.0f}),
            layer_trace::vec3(sdk::get_transform_position(copy_tf)), layer_trace::vec3(wt[3]), glm::length(Vector3f{wt[0]}),
            m_menu_sheet_label_hidden ? "switched to dynamic" : "not switched yet", m_menu_label->shouldDraw,
            gui_seen ? std::string{"drawn"} : std::string{"not drawn in the last 0.5 s"}));
    }
#endif
}
#endif

void VR::draw_reset_view_button() {
    if (get_runtime() == nullptr || !get_runtime()->loaded) {
        ImGui::TextWrapped("The headset is not active.");
        return;
    }

    if (ImGui::Button("Reset View and Position")) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        const auto before = m_standing_origin;
#endif
        recenter_view();
        m_standing_origin = get_position(0);
        // [HEIGHT PROBE 12.11.2026]
        REVR_LOG(spdlog::info("[VR] Reset View and Position: standing origin ({:.3f}, {:.3f}, {:.3f}) -> ({:.3f}, {:.3f}, {:.3f})", before.x, before.y,
            before.z, m_standing_origin.x, m_standing_origin.y, m_standing_origin.z));
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Turns the view to where you look now and puts the standing position where the headset is now.");
    }

#if defined(RE7)
#if !REVR_RELEASE // [RELEASE SWITCH fix143] not in RE7's release menu (the user's choice): fixed at load
    // [MENU CANVAS 12.11.2026]
    if (m_menu_canvas->draw("Main menu on a screen")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Shows the game's main menu as on a monitor, on a flat screen in front of you, so that the options "
                                "booklet can be read. Off by default: with it on the game's GPU hung in the main menu after a few "
                                "minutes (14.11.2026). In game nothing changes.");
    }
#endif

    if (m_movie_canvas->draw("Full-screen movies on a screen (16:9)")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Movies the game shows over the whole screen (the one before the main menu, the title movie) go on "
                                "the flat screen in front of you in 16:9 instead of being stretched over the headset image.");
    }

    if (m_menu_view_height->draw("Main menu view height in stereo (m)")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Raises (or lowers) the view in the game's main menu only.");
    }

    if (m_menu_view_back->draw("Main menu view back in stereo (m)")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Moves the view back (negative: forward) in the game's main menu only, "
                                "horizontally, away from where the game's menu camera looks.");
    }

    if (m_menu_sheet->draw("Main menu options: bring the sheet up")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("In the options the game takes its camera down to the cassette label on the desk. With this on the view "
                                "stays where it is and only the label (the options sheet) comes up in front of you, enlarged, turned as "
                                "the game shows it. Off: the view follows the game camera down to the label.");
    }

    if (m_menu_sheet->value()) {
        if (m_menu_sheet_size->draw("Main menu options: sheet size (x)")) {
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("How many times larger the options sheet is while it is up. Only the sheet changes size.");
        }

        if (m_menu_sheet_distance->draw("Main menu options: sheet distance (m)")) {
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("How far from your head the middle of the sheet comes.");
        }

        if (m_menu_sheet_guide_down->draw("Main menu options: guide text lower (deg)")) {
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("While the sheet is up, the line \"Regola le impostazioni dei comandi / Conferma / Indietro\" follows the "
                                    "sheet and goes this many degrees lower, below it. Negative: higher, above it.");
        }

        if (m_menu_sheet_below->draw("Main menu options: sheet below eye level (deg)")) {
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("The sheet comes straight ahead of where your head looks when the options open, this many degrees "
                                    "below eye level.");
        }
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix143] not in RE7's release menu (the user's choice): fixed at load
    if (m_menu_canvas->value() || m_movie_canvas->value()) {
        if (m_menu_canvas_width->draw("Main menu screen width (m)")) {
            g_framework->request_save_config();
        }

        if (m_menu_canvas_distance->draw("Main menu screen distance (m)")) {
            g_framework->request_save_config();
        }

        if (m_menu_canvas_aspect->draw("Main menu screen shape (width / height, 0 = automatic)")) {
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("0: the shape the game renders the menu with. If the picture still looks stretched, set it by hand: "
                                    "1.78 is 16:9, larger makes it wider and lower.");
        }
    }
#endif
#endif

#if defined(RE7)
#if !REVR_RELEASE // [RELEASE SWITCH fix143] not in RE7's release menu (the user's choice): fixed at load
    // [GAME VR MENUS 12.11.2026]
    if (m_game_vr_menus->draw("Game's own VR menu placement (experimental)")) {
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("The menus place themselves as in the game's PlayStation VR mode (app.VrGui). Experimental: if the "
                                "game misbehaves or crashes, switch it off.");
    }
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] a research file: debug build only
    // [VR RESEARCH 12.11.2026]
    if (ImGui::Button("Write RE7 VR research file")) {
        write_vr_research_file();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Writes re7_vr_research.txt next to the game: the game's own VR (PlayStation VR) types, their members "
                                "and the current values of its managers. Press it again in a cutscene or in the menu for their values there.");
    }
#endif
#endif
}

// [RESOLUTION_TOP 2026-08-20] Same reason as draw_recenter_button: the "Resolution Scale" slider
// lives in the hidden VR tree (on_draw_ui, below), so it is drawn bare in the main window instead.
// OpenVR is deliberately not offered here -- there the resolution belongs to SteamVR.
// Live slider + "Set" button: writing the value alone does nothing, the OpenXR swapchains have to
// be rebuilt with it, which is what on_config_load does at startup. "Set" does exactly that now,
// and the value is saved either way, so it also survives into the next start.
// [SYNC_MODE_CFG 30.09.2026] Kopie des "Sync Mode" aus dem versteckten VR-Baum, gleicher Wert.
void VR::draw_sync_mode_combo() {
    if (get_runtime() == nullptr || !get_runtime()->loaded) {
        return;
    }

    if (menu_text::Combo("Sync Mode##top", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0")) {
        m_sync_mode->value() = (int32_t)get_runtime()->custom_stage + 1;
        apply_sync_mode();
        g_framework->request_save_config();
    }

    // [RELEASE TEXTS fix136 23.11.2026]
    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("%s", "When in each frame the game reads where your head is. Change it only if the image stutters or "
                                      "feels a little behind your head movements.");
    }
}

// [USER_MONO 01.10.2026] Toggle im Stil der Menue-Toggles, gespeichert in der Config.
void VR::draw_user_mono_toggle() {
    g_framework->push_menu_toggle_style();
    const bool changed = m_user_mono->draw("Enable Mono Rendering");
    g_framework->pop_menu_toggle_style();

    if (changed) {
        g_framework->request_save_config();
    }
}

bool VR::draw_resolution_scale_slider() {
    if (get_runtime() == nullptr || !get_runtime()->loaded || !get_runtime()->is_openxr()) {
        return false;
    }

    const auto changed = m_resolution_scale->draw("OpenXR Resolution Scale");

    ImGui::Text("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());
    ImGui::SameLine();

    const auto pending = m_resolution_scale->value() != m_openxr->resolution_scale;

    // [SET-KNOPF 12.09.2026] Weisser eckiger Rahmen wie die Toggles im Menue, und
    // immer nur "Set" -- der Zusatz "(no change)" ist raus.
    g_framework->push_menu_toggle_style();
    const bool set_clicked = ImGui::SmallButton("Set##resscale");
    g_framework->pop_menu_toggle_style();

    if (set_clicked && pending) {
        m_openxr->resolution_scale = m_resolution_scale->value();
        initialize_openxr_swapchains();
        return true;
    }

    return changed;
}

void VR::on_draw_ui() {
    // create VR tree entry in menu (imgui)
    if (get_runtime()->loaded) {
        ImGui::SetNextItemOpen(m_has_hw_scheduling, ImGuiCond_::ImGuiCond_FirstUseEver);
    } else {
        if (m_openvr->error && !m_openvr->dll_missing) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_FirstUseEver);
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);
        }
    }

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please drop the %s file into the game's directory if you want to use %s", dll_name.data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    display_error(m_openxr, "openxr_loader.dll");
    display_error(m_openvr, "openvr_api.dll");

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");
        return;
    }

    ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");

    if (m_has_hw_scheduling) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This will cause the game to run slower.");
        ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    ImGui::TextWrapped("VR Runtime: %s", get_runtime()->name().data());
    ImGui::TextWrapped("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());

    if (get_runtime()->is_openvr()) {
        ImGui::TextWrapped("Resolution can be changed in SteamVR");
    } else if (get_runtime()->is_openxr()) {
        if (ImGui::TreeNode("Bindings")) {
            m_openxr->display_bindings_editor();
            ImGui::TreePop();
        }

        if (m_resolution_scale->draw("Resolution Scale")) {
            m_openxr->resolution_scale = m_resolution_scale->value();
        }
    }
    
    if (menu_text::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0")) {
        // [SYNC_MODE_CFG] Auswahl als 1..3 speichern
        m_sync_mode->value() = (int32_t)get_runtime()->custom_stage + 1;
        apply_sync_mode();
        g_framework->request_save_config();
    }
    ImGui::Separator();

    if (ImGui::Button("Set Standing Height")) {
        m_standing_origin.y = get_position(0).y;
    }

    if (ImGui::Button("Set Standing Origin") || m_set_standing_key->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (ImGui::Button("Recenter View") || m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }

    //ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
    //ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);

    ImGui::Separator();

    m_set_standing_key->draw("Set Standing Origin Key");
    m_recenter_view_key->draw("Recenter View Key");

    ImGui::Separator();

    m_decoupled_pitch->draw("Decoupled Camera Pitch");

    if (menu_text::Checkbox("Positional Tracking", &m_positional_tracking)) {
    }

    m_hmd_oriented_audio->draw("Head Oriented Audio");
    m_use_custom_view_distance->draw("Use Custom View Distance");
    m_view_distance->draw("View Distance/FarZ");
    m_near_clip_min->draw("Near Clip Minimum (m)");

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Smallest near clip plane of the VR cameras (0 = the game's own).\n"
                          "RE2/RE3 first person: clips the hair strands left in front of the camera by the hidden head.");
    }

#if !defined(RE4)
    // [MENU SIZE 11.11.2026]
    m_menu_panel_width->draw("Menu Panel Width (m)");

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Width of the VR menu panel in metres.\nThe menu opens and closes with the left trigger + triangle (left B).");
    }

    m_menu_text_scale->draw("Menu Text Size");

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Size of the options and explanations on the right side of the menu.");
    }
#endif
    m_motion_controls_inactivity_timer->draw("Inactivity Timer");
    m_joystick_deadzone->draw("Joystick Deadzone");

    // [GRIP_THRESHOLD] OpenXR only -- under OpenVR the Index profile already does this (Bindings.cpp).
    if (get_runtime() != nullptr && get_runtime()->is_openxr()) {
        m_grip_use_analog->draw("Analog Grip Threshold (OpenXR)");

        if (m_grip_use_analog->value()) {
            // [GRIP_FORCE] On Index the force sensor is the input the OpenVR profile thresholds --
            // squeeze/value there is the capacitive grip and reads high from just holding the thing.
            m_grip_prefer_force->draw("  Prefer Grip Force Sensor (Index)");
            m_grip_activate_threshold->draw("  Grip Press Threshold");
            m_grip_deactivate_threshold->draw("  Grip Release Threshold");
            ImGui::TextWrapped("Only affects controllers with an analog squeeze (Index, Touch). "
                               "Release should stay below press; higher press values require a firmer squeeze.");
        }
    }

    m_ui_scale_option->draw("2D UI Scale");
    m_ui_distance_option->draw("2D UI Distance");
    m_world_ui_scale_option->draw("World-Space UI Scale");

    ImGui::DragFloat3("Overlay Rotation", (float*)&m_overlay_rotation, 0.01f, -360.0f, 360.0f);
    ImGui::DragFloat3("Overlay Position", (float*)&m_overlay_position, 0.01f, -100.0f, 100.0f);

    ImGui::Separator();
    ImGui::Text("Graphical Options");

    m_force_fps_settings->draw("Force Uncap FPS");
    m_force_aa_settings->draw("Force Disable TAA");
    m_force_fxaa_off->draw("Force Disable game anti-aliasing (FXAA, SMAA, TAA)");

    if (ImGui::IsItemHovered()) {
        // ([NO PLUGIN fix91 19.11.2026] it said "the mod's upscaler (FSR 4)": the mod's own anti-aliasing does it now)
        menu_text::hint_tooltip("The mod's anti-aliasing already smooths the edges of every view. FXAA is always switched off; "
                                "SMAA and TAA while the mod's anti-aliasing is on.");
    }

    m_force_windowed->draw("Force the game's window out of full screen"); // [WINDOW MODE fix103 20.11.2026]

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("With the game's own option on full screen: on = the mod puts the game in a window while the headset is in use "
                                "(needed in RE7, where the window resized in a loop); off = the game's option is left as it is. "
                                "The game's own setting is not changed either way. Switched off, the game is back in full screen at "
                                "its next start.");
    }

    m_force_motionblur_settings->draw("Force Disable Motion Blur");
    m_force_vsync_settings->draw("Force Disable V-Sync");
    m_force_lensdistortion_settings->draw("Force Disable Lens Distortion");
    m_force_volumetrics_settings->draw("Force Disable Volumetrics");
    m_force_lensflares_settings->draw("Force Disable Lens Flares");
    m_force_dynamic_shadows_settings->draw("Force Enable Dynamic Shadows");
    m_allow_engine_overlays->draw("Allow Engine Overlays");
    m_enable_asynchronous_rendering->draw("Enable Asynchronous Rendering");

    if (ImGui::TreeNode("Desktop Recording Fix")) {
        ImGui::PushID("Desktop");
        m_desktop_fix->draw("Enabled");
        m_desktop_fix_skip_present->draw("Skip Present");
        ImGui::PopID();
        ImGui::TreePop();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix140] "Debug info": the developer tools of the VR page, debug build only (the values stay
                  // what they are; the 2D cutscenes and the Lua scripts still switch canvas, mono and suspend themselves)
    ImGui::Separator();
    ImGui::Text("Debug info");
    m_camera_duplicator.on_draw_ui();
    

    menu_text::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
    menu_text::Checkbox("Disable GUI Projection Matrix Override", &m_disable_gui_camera_projection_matrix_override);
    menu_text::Checkbox("Disable GUI Element Override", &m_disable_gui_element_override);
    menu_text::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
    menu_text::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
    menu_text::Checkbox("Disable Temporal Fix", &m_disable_temporal_fix);
    menu_text::Checkbox("Disable Post Effect Fix", &m_disable_post_effect_fix);

    menu_text::Checkbox("Force Mono Rendering", &m_mono_rendering);

    if (m_mono_rendering) {
        int mono_eye = (int)m_mono_rendering_eye;

        if (menu_text::Combo("Mono Rendering Eye", &mono_eye, "Left\0Right\0")) {
            m_mono_rendering_eye = (uint32_t)mono_eye;
        }

        menu_text::Checkbox("Mono Forces Projection Too", &m_mono_projection);
        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("Off (default): each eye keeps its own frustum, the identical images fuse into one flat picture.\n"
                              "On: both eyes also get the chosen eye's frustum - the images no longer line up.");
        }
    }

    menu_text::Checkbox("Suspend VR (true flat)", &m_vr_suspended);
    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Parks every engine override at once: camera, projection, GUI projection, GUI elements,\n"
                          "backbuffer size, engine overlays and the post effect fix. The game renders exactly as it\n"
                          "would flat (including the orthographic GUI camera the map needs) and both eyes get the\n"
                          "same image. Frames are still submitted, so the headset does not freeze.\n"
                          "Scriptable as vrmod:set_vr_suspended(true).");
    }

    menu_text::Checkbox("Glue Map To Head (RE4)", &m_map_face_glue);
    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Pins every layer of the map (Gui_ui3100/3101/3103/3104/3120/3121) to the SAME\n"
                          "anchor: the head, at the distance below, with one shared rotation. Without this each\n"
                          "layer is anchored to the game camera and re-oriented from its own distance, which is\n"
                          "what makes outline, navigation cross and pickups slide against each other in the HMD.\n"
                          "Scriptable as vrmod:set_map_face_glue(true).");
    }

    if (m_map_face_glue) {
        menu_text::SliderFloat("Map Distance (m)", &m_map_glue_distance, 0.2f, 10.0f);
        menu_text::SliderFloat("Map Layer Gap (m)", &m_map_glue_layer_gap, 0.0f, 0.05f);
        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("Stagger between the map layers. At exactly 0 every layer sits in the same plane\n"
                              "and draw order decides - an opaque layer then swallows the outline.");
        }
    }

    menu_text::Checkbox("Flatscreen Canvas", &m_flatscreen_overlay);
    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("Renders the game exactly as it looks on a monitor and shows it as a flat quad in front of you.\n"
                          "All camera overrides are suspended while this is on. OpenVR + D3D12 only.");
    }

    if (m_flatscreen_overlay) {
        menu_text::SliderFloat("Canvas Width (m)", &m_flatscreen_overlay_width, 0.1f, 20.0f);
        menu_text::SliderFloat("Canvas Distance (m)", &m_flatscreen_overlay_distance, 0.1f, 20.0f);
    }

    menu_text::SliderFloat("Image Shift X", &m_image_shift_x, -0.5f, 0.5f);
    menu_text::SliderFloat("Image Shift Y", &m_image_shift_y, -0.5f, 0.5f);

    int blank_eye = m_blank_eye + 1;   // -1/0/1 -> 0/1/2 for the combo
    if (menu_text::Combo("Blank Eye", &blank_eye, "Off\0Left\0Right\0")) {
        m_blank_eye = blank_eye - 1;
    }

    menu_text::SliderFloat("Projection Zoom", &m_projection_zoom, 0.1f, 10.0f);
    menu_text::SliderFloat("Projection FOV (0 = off)", &m_projection_fov, 0.0f, 120.0f);
    menu_text::SliderFloat("Projection Shift X", &m_projection_shift_x, -1.0f, 1.0f);
    menu_text::SliderFloat("Projection Shift Y", &m_projection_shift_y, -1.0f, 1.0f);

    const double min_ = 0.0;
    const double max_ = 25.0;
    ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

    ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
    ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

    // convert m_avg_input_delay (std::chrono::nanoseconds) to milliseconds (float)
    auto duration_float = std::chrono::duration<float, std::milli>(m_avg_input_delay).count();

    menu_text::DragFloat("Avg Input Processing Delay (MS)", &duration_float, 0.00001f);
#endif
}

void VR::on_device_reset() {
    std::scoped_lock _{m_openxr->sync_mtx};

    m_multipass.eye_textures[0] = nullptr;
    m_multipass.eye_textures[1] = nullptr;

    REVR_LOG(spdlog::info("VR: on_device_reset"));
    m_backbuffer_inconsistency = false;
    if (g_framework->is_dx11()) {
        m_d3d11.on_reset(this);
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    }

    m_overlay_component.on_reset();

    // [UI COPIES 30.10.2026] the engine makes its targets again after a reset: interface copies cloned before it are made
    // again (TemporalUpscaler drops its own copies here as well)
    refresh_ui_capture("device reset");

    // so i guess device resets can happen between begin and end rendering...
    if (m_in_render) {
        REVR_LOG(spdlog::info("VR: on_device_reset: in_render"));

        // what the fuck
        if (m_needs_camera_restore) {
            REVR_LOG(spdlog::info("VR: on_device_reset: needs_camera_restore"));
            restore_camera();
        }
    }

    if (inside_on_end) {
        REVR_LOG(spdlog::info("VR: on_device_reset: inside_on_end"));
    }
}

void VR::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    // Run the rest of OpenXR initialization code here that depends on config values
    if (get_runtime()->is_openxr() && get_runtime()->loaded) {
        REVR_LOG(spdlog::info("[VR] Finishing up OpenXR initialization"));

        m_openxr->resolution_scale = m_resolution_scale->value();
        initialize_openxr_swapchains();
    }

#if TDB_VER >= 69
    // [TECHNIQUE fix91 19.11.2026] Single Frame Multipass is the only technique left to choose: a configuration saved
    // with another one (Two Frame Sequential, AFW, AFR) starts with it. Engines before TDB 69 keep what they have
    // (their start value is Two Frame Sequential).
    if (int32_t& technique = m_rendering_technique->value(); technique != MULTIPASS) {
        REVR_LOG(spdlog::info("[VR] Rendering technique {} in the configuration: Single Frame Multipass is used", technique));
        technique = MULTIPASS;
    }
#endif

#if defined(RE7) && REVR_RELEASE
    // [RELEASE SWITCH fix143 23.11.2026] the options taken out of RE7's release menu keep their fixed values: no main menu
    // screen, no game VR menu placement; the movies' screen keeps the default width, distance and shape
    {
        const auto pin = [](auto& opt) { opt->value() = opt->default_value(); };
        pin(m_menu_canvas);
        pin(m_menu_canvas_width);
        pin(m_menu_canvas_distance);
        pin(m_menu_canvas_aspect);
        pin(m_game_vr_menus);
    }
#endif

    apply_sync_mode(); // [SYNC_MODE_CFG]


    if (m_motion_controls_inactivity_timer->value() <= 10.0f) {
        m_motion_controls_inactivity_timer->value() = 30.0f;
    }
}

void VR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

std::array<RECamera*, VR::MAX_CAMERAS> VR::get_cameras() const {
    if (is_using_multipass()) {
        // [STEREO FOVEA] primary + every clone in use (plain multipass: [1] only, the rest nullptr)
        std::array<RECamera*, MAX_CAMERAS> out{ m_multipass_cameras[0], m_multipass_cameras[1], nullptr, nullptr };

        if (is_stereo_fovea()) {
            out[2] = m_multipass_cameras[2];
            out[3] = m_multipass_cameras[3];
        }

        // no gaps: the loops over these stop at the first nullptr, and [0] must stay the primary camera
        std::array<RECamera*, MAX_CAMERAS> packed{};

        if (out[0] == nullptr) {
            return packed;
        }

        size_t n = 0;

        for (auto camera : out) {
            if (camera != nullptr) {
                packed[n++] = camera;
            }
        }

        return packed;
    }

    return std::array<RECamera*, MAX_CAMERAS>{ sdk::get_primary_camera(), nullptr, nullptr, nullptr };
}

Vector4f VR::get_position(uint32_t index) const {
    // [POSE_FREEZE 2026-08-11] Auch die HMD-POSITION einfrieren, nicht nur die Rotation.
    // Gemessen im Scope: die Kameraposition streute bis 9 mm pro Sekunde bei Kopfbewegung
    // und 0,3-0,6 mm in Ruhe, weil das Script sie nur zweimal je Frame ueber
    // standing_origin nachzieht, gerendert aber mit der frischen Position wird.
    // Eingefroren bleibt der Augen-Versatz trotzdem einstellbar: er steckt in
    // standing_origin, und die Differenz dazu wird weiter live gerechnet.
    if (index == 0 && m_pose_freeze) {
        return m_frozen_hmd_position;
    }

    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };
    std::shared_lock __{ get_runtime()->eyes_mtx };

    return get_position_unsafe(index);
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Vector4f{ *(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f };
        } else if (index > 0) {
            return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].location.pose.position, 1.0f };
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

// [POSE_FREEZE 2026-08-11] Ab hier liefert die HMD-Rotation ueberall denselben Wert.
// Beim Einschalten wird die aktuelle Rotation gemerkt -- get_rotation() ist zu diesem
// Zeitpunkt noch ungefiltert, weil m_pose_freeze erst danach gesetzt wird.
void VR::set_pose_freeze(bool on) {
    if (on == m_pose_freeze) {
        return;
    }

    if (on) {
        m_frozen_hmd_rotation = glm::normalize(glm::quat{get_rotation(0)});
        m_frozen_hmd_position = get_position(0);
    }

    m_pose_freeze = on;

    // Dasselbe an die OpenXR-Runtime durchreichen: sie haengt die Pose an den
    // Compositor-Layer, und ohne das reprojiziert die Runtime das eingefrorene Bild
    // weiter gegen die echte Kopfbewegung. OpenVR braucht das nicht -- dort steckt die
    // Blickrichtung bereits in der gerenderten Textur.
    if (m_openxr != nullptr) {
        m_openxr->pose_freeze = on;
        m_openxr->frozen_orientation = XrQuaternionf{m_frozen_hmd_rotation.x, m_frozen_hmd_rotation.y,
                                                     m_frozen_hmd_rotation.z, m_frozen_hmd_rotation.w};
    }
}

// [POSE_FREEZE/SUBMIT] Die Pose, die unter OpenVR als "dafuer wurde gerendert" mitgeht.
// Modus 1 gibt die eingefrorene Blickrichtung an (Compositor dreht die Differenz nach,
// Bild bleibt weltfest), Modus 2 die frische (Compositor dreht nichts nach, Bild klebt am
// Display). Die POSITION ist immer die echte.
vr::HmdMatrix34_t VR::get_submit_pose() const {
    auto live = get_raw_transform(0);

    if (m_pose_freeze_submit != 1) {
        return live;
    }

    const auto rot = glm::mat3_cast(m_frozen_hmd_rotation);

    for (auto row = 0; row < 3; ++row) {
        for (auto col = 0; col < 3; ++col) {
            live.m[row][col] = rot[col][row];
        }
    }

    return live;
}

Matrix4x4f VR::get_rotation(uint32_t index) const {
    // [POSE_FREEZE] Gilt NUR fuer das HMD (index 0), die Haende bleiben live.
    if (index == 0 && m_pose_freeze) {
        return Matrix4x4f{m_frozen_hmd_rotation};
    }

    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::extractMatrixRotation(glm::rowMajor4(matrix));
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };
        std::shared_lock __{ get_runtime()->eyes_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            return Matrix4x4f{*(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            //return Matrix4x4f{*(glm::quat*)&m_openxr->stage_views[0].pose.orientation};
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                return Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
            }
        }

        return glm::identity<Matrix4x4f>();
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto out = glm::rowMajor4(matrix);

        // [POSE_FREEZE] Rotation eingefroren, Position bleibt echt.
        if (index == 0 && m_pose_freeze) {
            auto frozen = Matrix4x4f{m_frozen_hmd_rotation};
            frozen[3] = out[3];
            return frozen;
        }

        return out;
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            auto mat = Matrix4x4f{m_pose_freeze ? m_frozen_hmd_rotation
                                                : *(glm::quat*)&m_openxr->view_space_location.pose.orientation};
            mat[3] = Vector4f{*(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.position, 1.0f};
                return mat;
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.position, 1.0f};
                return mat;
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        REVR_LOG(spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name()));
        return vr::HmdMatrix34_t{};
    }
}

// [MENUE-STEUERUNG 11.09.2026] Die Sperre: Menue offen ODER Loslass-Waechter nach
// dem Schliessen (REFramework::run_vr_menu_frame setzt/loest ihn).
bool VR::is_menu_input_blocked() const {
    return m_menu_release_guard || (g_framework != nullptr && g_framework->is_drawing_ui());
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    if (is_menu_input_blocked()) {
        return false;
    }

    return is_action_active_raw(action, source);
}

bool VR::is_action_active_raw(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    if (!get_runtime()->loaded) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        // [GRIP_THRESHOLD] The grip action is declared boolean, so under OpenXR the runtime decides
        // when a squeeze counts as pressed -- and SteamVR's default fires far too early: a single
        // grab registers twice, menu entries advance two steps. OpenVR never showed this because the
        // shipped Index profile reads the FORCE sensor and thresholds it at 0.3 / 0.25 (Bindings.cpp).
        // So: read the analog value ourselves and apply that same hysteresis, per hand -- from the
        // force sensor where it exists, otherwise from squeeze/value. Controllers with neither
        // (Vive wands, WMR) bind no float action at all, get_action_float returns false, and the
        // boolean path below runs exactly as before.
        // [GRIP NUR KNUCKLES 16.09.2026] Die eigene Schwelle war fuer die Valve-Index-Controller
        // gebaut. Andere Controller (Pimax SLAM: Menue/Laufen gingen, Greifen nie) bekommen wieder
        // die Entscheidung der Runtime wie vor dem 20.08. -- der Bool-Weg unten.
        if (action == m_action_grip && m_grip_use_analog->value()
            && m_openxr->get_current_interaction_profile() == "/interaction_profiles/valve/index_controller") {
            const auto hand = (VRRuntime::Hand)source;

            if (hand <= VRRuntime::Hand::RIGHT) {
                float value = 0.0f;
                int source_kind = 0; // 0 = none yet -> boolean fallback below

                // [GRIP_FORCE] Order matters. On Index, squeeze/value is the CAPACITIVE grip: it sits
                // well above 0.3 just from holding the controller, so thresholding it still felt like
                // a hair trigger. squeeze/force is the force sensor -- the very input the shipped
                // OpenVR knuckles profile thresholds at 0.3/0.25. Controllers without a force sensor
                // never bind it, so they keep squeeze/value exactly as before.
                if (m_grip_prefer_force->value() && m_action_grip_force != 0
                    && m_openxr->get_action_float((XrAction)m_action_grip_force, hand, value)) {
                    source_kind = 2;
                } else if (m_action_grip_value != 0
                    && m_openxr->get_action_float((XrAction)m_action_grip_value, hand, value)) {
                    source_kind = 1;
                }

                m_grip_last_source[(size_t)hand] = source_kind;

                if (source_kind != 0) {
                    m_grip_last_value[(size_t)hand] = value;

                    auto& state = m_grip_value_state[(size_t)hand];
                    const auto on_threshold = m_grip_activate_threshold->value();
                    // Never let the release threshold sit above the press threshold -- that would
                    // latch the grip on forever.
                    auto off_threshold = m_grip_deactivate_threshold->value();

                    if (off_threshold > on_threshold) {
                        off_threshold = on_threshold;
                    }

                    if (state) {
                        state = value > off_threshold;
                    } else {
                        state = value >= on_threshold;
                    }

                    return state;
                }
            }
        }

        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    if (!active && action == m_action_minimap) {
        active = is_action_active_raw(m_action_b_button, m_left_joystick) && is_hand_behind_head(VRRuntime::Hand::LEFT);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    // [MENUE-STEUERUNG 11.09.2026] Bei offenem Menue stehen die Sticks fuers Spiel still.
    if (is_menu_input_blocked()) {
        return Vector2f{};
    }

    return get_joystick_axis_raw(handle);
}

Vector2f VR::get_joystick_axis_raw(vr::VRInputValueHandle_t handle) const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        const auto out = Vector2f{ data.x, data.y };

        return glm::length(out) > deadzone ? out : Vector2f{};
    } else if (get_runtime()->is_openxr()) {
        if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::LEFT) {
            auto out = m_openxr->get_left_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        } else if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::RIGHT) {
            auto out = m_openxr->get_right_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        }
    }

    return Vector2f{};
}

// [TRACKPAD 15.09.2026] Nur fuer das eigene Menue gedacht: die Achse des
// rechten Trackpads. Bewusst OHNE die Menue-Sperre aus get_joystick_axis --
// sie soll ja genau dann arbeiten, wenn das Menue offen ist.
Vector2f VR::get_right_touchpad_axis() const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        if (m_action_touchpad == vr::k_ulInvalidActionHandle) {
            return Vector2f{};
        }

        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_touchpad, &data, sizeof(data), m_right_joystick);

        return Vector2f{ data.x, data.y };
    }

    if (get_runtime()->is_openxr()) {
        return m_openxr->get_touchpad_axis(VRRuntime::Hand::RIGHT);
    }

    return Vector2f{};
}

// [TRACKPAD-PRESS 15.09.2026] Unter OpenVR bildet SteamVR den Klick selbst aus
// der Kraft -- dort reicht die boolesche Aktion. Unter OpenXR gibt es beim
// Index keinen trackpad/click, sondern nur die KRAFT: deshalb dort zusaetzlich
// die Kraft lesen und mit einer Schwelle vergleichen.
bool VR::is_touchpad_pressed(VRRuntime::Hand hand) const {
    if (!get_runtime()->loaded) {
        return false;
    }

    const auto handle = (hand == VRRuntime::Hand::LEFT) ? m_left_joystick : m_right_joystick;

    if (m_action_touchpad_click != vr::k_ulInvalidActionHandle
        && is_action_active(m_action_touchpad_click, handle)) {
        return true;
    }

    if (!get_runtime()->is_openxr()) {
        return false;
    }

    if (m_action_touchpad_force == vr::k_ulInvalidActionHandle) {
        return false;
    }

    // Schwelle wie im OpenVR-Knuckles-Profil (0.3 an/0.25 aus) -- hier nur die
    // obere, ein Klick ist kurz und braucht keine Hysterese.
    //
    // [KORREKTUR 15.09.2026] Erst stand hier get_action_axis -- das fragt
    // xrGetActionStateVector2f ab. TouchpadForce ist aber ein FLOAT
    // (vector1), deshalb kamen konstant 0.00 an, obwohl das Handle gueltig
    // war ("ForceHandle=ok, alle Werte 0"). Float-Aktionen gehen ueber
    // get_action_float -- denselben Weg nimmt auch GripForce.
    float force = 0.0f;

    if (!m_openxr->get_action_float((XrAction)m_action_touchpad_force, hand, force)) {
        return false;
    }

    return force > 0.3f;
}

float VR::get_touchpad_force(VRRuntime::Hand hand) const {
    if (!get_runtime()->loaded || !get_runtime()->is_openxr()) {
        return -1.0f;
    }

    if (m_action_touchpad_force == vr::k_ulInvalidActionHandle) {
        return -2.0f;   // Aktion gar nicht gebunden
    }

    float force = 0.0f;

    if (!m_openxr->get_action_float((XrAction)m_action_touchpad_force, hand, force)) {
        return -3.0f;   // Abfrage schlug fehl
    }

    return force;
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(m_left_joystick);
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(m_right_joystick);
}

Vector2f VR::get_left_stick_axis_raw() const {
    return get_joystick_axis_raw(m_left_joystick);
}

Vector2f VR::get_right_stick_axis_raw() const {
    return get_joystick_axis_raw(m_right_joystick);
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}
