#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h> // [DLSS VENDOR 12.11.2026]
#include <wrl.h>
#include <format>   // [QUADVIEW]
#include <filesystem> // [DUMP NUMBERS 25.10.2026]
#include <fstream>    // [JITTER CHECK 10.11.2026]
#include <cstdlib>
#include <chrono>   // [ONE SUBMIT 04.11.2026]
#include <functional> // [FSR DIRECT 06.11.2026]

#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/Renderer.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/SystemArray.hpp>
#include <sdk/Memory.hpp>

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
#ifdef RE4
#include "sdk/regenny/re4/via/Window.hpp"
#include "sdk/regenny/re4/via/SceneView.hpp"
#elif defined(SF6)
#include "sdk/regenny/sf6/via/Window.hpp"
#include "sdk/regenny/sf6/via/SceneView.hpp"
#else
#include "sdk/regenny/re8/via/Window.hpp"
#include "sdk/regenny/re8/via/SceneView.hpp"
#endif
#endif



#include "TemporalUpscaler.hpp"

#include "VR.hpp"
#include "FoveatedRendering.hpp"   // [FOVEATED RENDERING] draw_settings
#include "../../build64_all/_deps/directxtk12-src/Src/d3dx12.h"
#include <cstdarg>

QUADVIEW_BUILD_STAMP(TemporalUpscaler); // [BUILD STAMP fix107]


#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [DUMP NUMBERS 25.10.2026] highest dumpNN already in the dump folder (read once per game session)
static uint64_t qv_dump_number_base(const std::filesystem::path& dir) {
    static std::once_flag once{};
    static uint64_t base = 0;

    std::call_once(once, [&] {
        std::error_code ec{};

        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const auto name = e.path().filename().string();

            if (name.size() > 4 && name.rfind("dump", 0) == 0 && e.is_directory(ec)) {
                base = (std::max)(base, (uint64_t)std::strtoull(name.c_str() + 4, nullptr, 10));
            }
        }
    });

    return base;
}
#endif

std::shared_ptr<TemporalUpscaler>& TemporalUpscaler::get() {
    static std::shared_ptr instance = std::make_shared<TemporalUpscaler>();
    return instance;
}

// [NO PLUGIN fix91 19.11.2026] The external upscaler plugin was loaded here and asked for its upscale methods;
// with it went the check that kept DLSS from being offered on a card that is not NVIDIA's. The mod needs no plugin any
// more: the quad view is anti-aliased by the mod's own passes (its supersampling TAA in the fovea and in the periphery),
// and so are the two eye views when the quad view is not engaged (evaluate_plain). "Backend loaded" now only says that
// the game runs on Direct3D 12 (on_first_frame).
std::optional<std::string> TemporalUpscaler::on_initialize() {
    m_backend_loaded = true;
    // [SHADER FILES fix110 20.11.2026] the quad view's compiled shaders in files beside the config (71 s of every start)
    d3d12::set_shader_cache_dir(REFramework::get_persistent_dir("quadview_shader_cache"));
    return Mod::on_initialize();
}
std::optional<std::string> TemporalUpscaler::on_initialize_d3d_thread() try {
    // [AFW fix91 19.11.2026] The AFW plugin's renderer was made here (InitDevice) for the flat screen's "UI fix"; both are gone

    // all OK
    return Mod::on_initialize();
} catch (...) {
    REVR_LOG(spdlog::error("Exception occurred in VR::on_initialize()"));
    return Mod::on_initialize();
}

// [NO PLUGIN fix91 19.11.2026] apply_upscale_type_from_config was here: the choice among the plugin's upscale methods

void TemporalUpscaler::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    // [UPSCALING MENU fix91 19.11.2026] always enabled: the quad view needs it and the menu no longer has the switch
    m_enabled->value() = true;

#if REVR_RELEASE
    // [RELEASE SWITCH fix139 23.11.2026] the options the release has no entry for (edge softening, long edges, SMAA):
    // fixed at their defaults (off), whatever a configuration of an earlier build holds
    {
        const auto pin = [](auto& opt) { opt->value() = opt->default_value(); };
        pin(m_post_edge_soften);
        pin(m_post_edge_threshold);
        pin(m_post_long_edges);
        pin(m_post_smaa);
    }
#endif

    if (!ready()) {
        return;
    }
}

void TemporalUpscaler::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }

    if (!ready()) {
        return;
    }
}

void TemporalUpscaler::on_draw_ui() {
    if (!ImGui::CollapsingHeader("Upscaler")) {
        return;
    }

    draw_settings();
}

// [MENUE-KATEGORIEN 11.09.2026] Der Inhalt ohne den Header -- das Hauptfenster
// zeichnet ihn in der Kategorie "Upscaler", dort ist die Kategorie selbst
// schon die Ueberschrift.
void TemporalUpscaler::draw_settings() {
    // [UPSCALING MENU fix91 19.11.2026] The "Upscaling" block that was drawn here is gone (Enable Upscaling, Upscale Type,
    // Quality Level, DLSS Preset, and the hints shown without the plugin): the quad view is always on and anti-aliases
    // fovea and periphery with the mod's supersampling TAA. The module itself is always enabled (on_config_load).
    // [NO PLUGIN fix91 19.11.2026] The upscaler plugin is no longer loaded; its type, quality and preset options are gone.

    // [TECHNIQUE fix91 19.11.2026] the "Rendering Technique" choice was drawn here (VR.cpp): Single Frame Multipass is the
    // only technique now

    // [QUADVIEW] quad-view foveated rendering (multipass). Own heading + settings.
    FoveatedRendering::get()->draw_settings();

    // [POST_PROCESSING 28.09.2026 -- Ansage des Users] Unter "Two Frame
    // Sequential": Trennstrich + Ueberschrift (Resi-Schrift, erster Buchstabe
    // rot) wie alle Ueberschriften, darunter die beiden Toggles.
    g_framework->draw_menu_heading("POST PROCESSING", true);

    g_framework->push_menu_toggle_style();
    const bool fog_changed = m_fog_enabled->draw("Enable Fog");
    const bool volfog_changed = m_volfog_enabled->draw("Enable Volumetric Fog");
    const bool colorcorrect_changed = m_colorcorrect_enabled->draw("Enable Color Correction");
    g_framework->pop_menu_toggle_style();

    if (fog_changed) {
        m_fog_restore = m_fog_enabled->value();
        g_framework->request_save_config();
    }

    if (volfog_changed) {
        m_volfog_restore = m_volfog_enabled->value();
        g_framework->request_save_config();
    }

    if (colorcorrect_changed) {
        m_colorcorrect_restore = m_colorcorrect_enabled->value();
        g_framework->request_save_config();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix139] SMAA: not in the release (fixed off)
    // [POST_PASS 28.09.2026] SMAA auf den Augenbildern -- nur ohne Upscaler.
    // Sharpness/Saturation stehen ganz oben (draw_post_sliders).
    {
        // [SMAA NUR OHNE UPSCALER 28.09.2026 -- Ansage des Users] Laeuft der
        // Upscaler, wird SMAA ausgeschaltet und ausgegraut -- der glaettet selbst.
        const bool upscaling = ready();

        if (upscaling && m_post_smaa->value()) {
            m_post_smaa->value() = false;
            g_framework->request_save_config();
        }

        if (upscaling) {
            ImGui::BeginDisabled();
        }

        g_framework->push_menu_toggle_style();
        const bool smaa_changed = m_post_smaa->draw("Enable SMAA");
        g_framework->pop_menu_toggle_style();

        if (upscaling) {
            ImGui::EndDisabled();

            // [ROT 28.09.2026 -- Ansage des Users] Nur der Klammer-Hinweis rot,
            // NACH EndDisabled, damit er nicht mit ausgegraut wird.
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{1.0f, 0.0f, 0.0f, 1.0f}, "(Upscaler running)");
        }

        if (smaa_changed && !upscaling) {
            g_framework->request_save_config();
        }
    }
#endif
}

// [ALLGEMEINE REGLER 28.09.2026 -- Ansage des Users] Sharpness + Saturation
// gelten in JEDEM Zustand (mit/ohne Upscaler) und stehen deshalb ganz oben in
// der Kategorie, direkt unter "OpenXR Resolution Scale" (REFramework.cpp).
void TemporalUpscaler::draw_post_sliders() {
    // [ABSTAND 28.09.2026 -- Ansage des Users] Etwas mehr Luft zur Resolution-Zeile.
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.75f));

    // [BRIGHTNESS/CONTRAST 28.09.2026 -- Ansage des Users] eigene Shader (PostPass),
    // NICHT Engine-Gamma. Reihenfolge: Brightness, Contrast, Sharpness, Saturation.
    int bright = m_post_brightness->value();

    if (menu_text::SliderInt("Brightness##post", &bright, -10, 10)) {
        m_post_brightness->value() = std::clamp(bright, -10, 10);
        g_framework->request_save_config();
    }

    int contrast = m_post_contrast->value();

    if (menu_text::SliderInt("Contrast##post", &contrast, -10, 10)) {
        m_post_contrast->value() = std::clamp(contrast, -10, 10);
        g_framework->request_save_config();
    }

    int sharp = m_post_sharpness->value();

    if (menu_text::SliderInt("Sharpness##post", &sharp, 0, 10)) {
        m_post_sharpness->value() = std::clamp(sharp, 0, 10);
        g_framework->request_save_config();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix139] edge softening and long edges: not in the release (fixed off)
    // [EDGE SOFTEN 11.11.2026]
    int edge = m_post_edge_soften->value();

    if (menu_text::SliderInt("Soften edges##post", &edge, 0, 20)) {
        m_post_edge_soften->value() = std::clamp(edge, 0, 20);
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        // [RELEASE TEXTS fix136 23.11.2026]
        menu_text::hint_tooltip("%s", "Softens hard edges (bars, metal) that look jagged or flicker in the headset. Textures stay as "
                                      "they are. 0 = off.");
    }

    if (m_post_edge_soften->value() > 0) {
        int thr = m_post_edge_threshold->value();

        if (menu_text::SliderInt("Edge threshold##post", &thr, 1, 20)) {
            m_post_edge_threshold->value() = std::clamp(thr, 1, 20);
            g_framework->request_save_config();
        }

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("%s", "Lower: more edges are softened. Higher: only the strongest ones.");
        }
    }

    // [LONG EDGES 11.11.2026]
    int long_edges = m_post_long_edges->value();

    if (menu_text::SliderInt("Smooth long edges##post", &long_edges, 0, 20)) {
        m_post_long_edges->value() = std::clamp(long_edges, 0, 20);
        g_framework->request_save_config();
    }

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("%s", "Smooths the steps along long straight edges (bars, frames) in the sharp area. 0 = off.");
    }
#endif

    int sat = m_post_saturation->value();

    if (menu_text::SliderInt("Saturation##post", &sat, -10, 10)) {
        m_post_saturation->value() = std::clamp(sat, -10, 10);
        g_framework->request_save_config();
    }

    // [RESET 28.09.2026 -- Ansage des Users] Ein Knopf unter beiden Reglern:
    // alle vier auf 0 (= aus/neutral). Stil wie der Set-Knopf.
    g_framework->push_menu_toggle_style();
    const bool reset_clicked = ImGui::SmallButton("Reset##post");
    g_framework->pop_menu_toggle_style();

    if (reset_clicked) {
        m_post_brightness->value() = 0;
        m_post_contrast->value() = 0;
        m_post_sharpness->value() = 0;
        m_post_edge_soften->value() = 0;
        m_post_edge_threshold->value() = 5;
        m_post_long_edges->value() = 0;
        m_post_saturation->value() = 0;
        g_framework->request_save_config();
    }
}

// [POST_PROCESSING 28.09.2026] Wie zzz_re4_farbe_test.lua: findComponents ueber
// die ganze Szene (MainCamera + MainCamera (Clone)), set_Enabled auf jede Instanz.
void TemporalUpscaler::apply_post_processing() {
    using SteadyClock = std::chrono::steady_clock;

    // [CPU 22.10.2026] full scene search: on changes, otherwise only to find components that appear without any sign
    constexpr auto RESCAN_EVERY = std::chrono::seconds(60);
    constexpr auto MIN_GAP = std::chrono::milliseconds(250);

    struct Entry {
        const char* type_name;
        const char* sub_getter;   // nullptr or e.g. "get_ColorCorrect" (the property is set on the sub object)
        const char* property;     // bool property: get_<property> / set_<property>
        bool enabled;
        bool* restore;
    };

    // [LIGHT FLICKER TEST 15.11.2026 evening] a phase of the test switches both fogs off; afterwards the options' values are
    // written back once (as after switching an option on)
    const bool test_fog_off = m_test_fog_off.load(std::memory_order_relaxed);

    if (m_test_fog_was_off && !test_fog_off) {
        m_fog_restore = m_fog_restore || m_fog_enabled->value();
        m_volfog_restore = m_volfog_restore || m_volfog_enabled->value();
    }

    m_test_fog_was_off = test_fog_off;

    static_assert(std::tuple_size_v<decltype(m_pp_scan)> == 3);
    const std::array<Entry, 3> entries{{
        {"via.render.Fog", nullptr, "Enabled", m_fog_enabled->value() && !test_fog_off, &m_fog_restore},
        {"via.render.VolumetricFogControl", nullptr, "Enabled", m_volfog_enabled->value() && !test_fog_off, &m_volfog_restore},
        {"via.render.LDRPostProcess", "get_ColorCorrect", "Enabled", m_colorcorrect_enabled->value(), &m_colorcorrect_restore},
    }};

    bool any = false;

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];

        if (e.enabled && !*e.restore) {
            post_fx_release(m_pp_scan[i]);   // on and nothing to write back -> the game stays untouched
            continue;
        }

        any = true;
    }

    if (!any) {
        return;
    }

    // a new scene or camera brings new components: search again soon
    bool changed = false;
    const auto scene = (void*)sdk::get_current_scene();

    if (scene != m_pp_scene) {
        m_pp_scene = scene;
        changed = true;
    }

    for (auto* cam : VR::get()->get_cameras()) {
        if (cam == nullptr || std::find(m_pp_cameras.begin(), m_pp_cameras.end(), (void*)cam) != m_pp_cameras.end()) {
            continue;
        }

        m_pp_cameras[m_pp_camera_slot++ % m_pp_cameras.size()] = (void*)cam;
        changed = true;
    }

    if (changed) {
        for (auto& st : m_pp_scan) {
            st.dirty = true;
        }
    }

    const auto now = SteadyClock::now();
    bool searched = false;

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        auto& st = m_pp_scan[i];

        if (e.enabled && !*e.restore) {
            continue;
        }

        if (!searched) {
            const auto since = now - st.last;
            const bool due = *e.restore || !st.active || (st.dirty && since >= MIN_GAP) || since >= RESCAN_EVERY;

            if (due) {
                searched = true;
                st.dirty = false;
                st.last = now;
                post_fx_search(i, e.type_name, e.sub_getter, e.property, e.enabled);

                if (*e.restore) {
                    // switched back on: written once, nothing more to keep
                    *e.restore = false;
                    post_fx_release(st);
                } else {
                    st.active = true;
                }

                continue;
            }
        }

        if (!st.active) {
            continue;   // its first search comes in one of the next frames
        }

        // the game switches them back on: keep them off, on the components we know
        static const auto game_object_t = sdk::find_type_definition("via.GameObject");
        static const auto get_valid = game_object_t != nullptr ? game_object_t->get_method("get_Valid") : nullptr;
        const auto context = sdk::get_thread_context();

        for (auto it = st.objects.begin(); it != st.objects.end();) {
            auto* c = *it;
            const auto owner = ((::REComponent*)c)->ownerGameObject;
            const bool alive = c->referenceCount > 1 && owner != nullptr &&
                               (get_valid == nullptr || get_valid->call<bool>(context, (::REManagedObject*)owner));

            if (!alive) {
                utility::re_managed_object::release(c);
                it = st.objects.erase(it);
                ++m_pp_dropped_objects;
                st.dirty = true;   // its replacement (if any) comes with the next search
                continue;
            }

            if (post_fx_apply(st, c, e.sub_getter, e.property, e.enabled)) {
                ++m_pp_kept_off;
            }

            ++it;
        }
    }
}

// set_<property>(enabled) on the component (or its sub object) when it differs. true = it was changed.
bool TemporalUpscaler::post_fx_apply(PostFxScan& st, ::REManagedObject* component, const char* sub_getter, const char* property, bool enabled) {
    auto* target = component;

    if (sub_getter != nullptr) {
        target = sdk::call_object_func_easy<::REManagedObject*>(component, sub_getter);
    }

    if (target == nullptr) {
        return false;
    }

    const auto td = utility::re_managed_object::get_type_definition(target);

    if (td == nullptr) {
        return false;
    }

    if (td != st.td) {
        st.td = td;
        st.get_enabled = td->get_method(std::string{"get_"} + property);
        st.set_enabled = td->get_method(std::string{"set_"} + property);
    }

    if (st.set_enabled == nullptr) {
        return false;
    }

    const auto context = sdk::get_thread_context();

    if (st.get_enabled != nullptr && st.get_enabled->call<bool>(context, target) == enabled) {
        return false;
    }

    st.set_enabled->call<void>(context, target, enabled);
    return true;
}

void TemporalUpscaler::post_fx_release(PostFxScan& st) {
    if (st.objects.empty() && !st.active) {
        return;
    }

    for (auto* o : st.objects) {
        utility::re_managed_object::release(o);
    }

    st.objects.clear();
    st.active = false;

    std::scoped_lock _{m_pp_stats_mtx};
    st.owners.clear();
}

// Full scene search for one type: keeps a reference on every component found, writes the wanted state once.
void TemporalUpscaler::post_fx_search(size_t index, const char* type_name, const char* sub_getter, const char* property, bool enabled) {
    auto& st = m_pp_scan[index];
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    const auto t0 = std::chrono::steady_clock::now();
#endif
    const auto scene = sdk::get_current_scene();
    const auto t = sdk::find_type_definition(type_name);
    const auto rt = t != nullptr ? t->get_runtime_type() : nullptr;
    const auto arr = (scene != nullptr && rt != nullptr) ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", rt)
                                                          : nullptr;

    std::vector<::REManagedObject*> found{};

    if (arr != nullptr) {
        for (auto* c : arr->get_elements()) {
            if (c != nullptr && std::find(found.begin(), found.end(), c) == found.end()) {
                found.push_back(c);
            }
        }
    }

    bool set_changed = found.size() != st.objects.size();

    for (auto* c : found) {
        if (std::find(st.objects.begin(), st.objects.end(), c) == st.objects.end()) {
            utility::re_managed_object::add_ref(c);
            ++m_pp_new_objects;
            set_changed = true;
        }
    }

    for (auto* o : st.objects) {
        if (std::find(found.begin(), found.end(), o) == found.end()) {
            utility::re_managed_object::release(o);
        }
    }

    st.objects = std::move(found);

    for (auto* c : st.objects) {
        post_fx_apply(st, c, sub_getter, property, enabled);
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    if (set_changed) {
        std::string owners{};

        for (auto* c : st.objects) {
            const auto go = ((::REComponent*)c)->ownerGameObject;
            owners += owners.empty() ? "" : ", ";
            owners += (go != nullptr && go->name != nullptr) ? utility::narrow(utility::re_string::get_view(go->name)) : std::string{"?"};
        }

        auto text = std::format("{}.{} x{} on: {}", type_name, property, st.objects.size(), owners.empty() ? std::string{"-"} : owners);
        REVR_LOG(spdlog::info("[TemporalUpscaler] post processing: {}", text));

        std::scoped_lock _{m_pp_stats_mtx};
        st.owners = std::move(text);
    }

    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    m_pp_last_search_us = us;
    m_pp_max_search_us = (std::max)(m_pp_max_search_us, us);
    ++m_pp_searches;
#endif
}

void TemporalUpscaler::on_early_present() {
    m_rendering = true;

    if (!m_backend_loaded) {
        return;
    }

    if (!m_first_frame_finished) {
        if (!on_first_frame()) {
            m_backend_loaded = false;
            m_initialized = false;
            return;
        }
    }

    // [QUADVIEW] quad view switched on/off or its size/upscalers changed -> new upscaler instances + render size
    {
        auto& qv = quadview::State::get();
        const auto req = qv.requested();

        // The rendering technique can change (menu, 2D cutscenes) before FoveatedRendering withdraws the request in the same
        // frame: quad-view instances (render size = the quad view's) must never meet another technique's path.
        const bool qv_wrong_technique = m_qv_layout.valid && !(VR::get()->is_hmd_active() && VR::get()->is_using_multipass());

        if (activated() && !m_wants_reinitialize &&
            (req != m_qv_init_requested || (req && qv.generation() != m_qv_init_generation) || qv_wrong_technique)) {
            REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view {} (generation {}{}) -> re-initialising the upscaler", req ? "on" : "off", qv.generation(),
                qv_wrong_technique ? ", technique changed" : ""));
            release_upscale_features();
            m_wants_reinitialize = true;
        }
    }

    if (m_wants_reinitialize) {
        init_upscale_features();
        m_wants_reinitialize = false;
        return;
    }

    if (!ready() || !m_upscale) {
        return;
    }

    if (m_eye_states[0].scene_layer == nullptr) {
        return;
    }

    if (m_is_d3d12) {
        auto& hook = g_framework->get_d3d12_hook();
        auto swapchain = hook->get_swap_chain();
        auto device = hook->get_device();
        ComPtr<ID3D12Resource> backbuffer{};

        const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

        if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get backbuffer (D3D12)"));
            return;
        }

        auto bb_desc = backbuffer->GetDesc();
        eye_image_size(bb_desc.Width, bb_desc.Height); // [SMALL WINDOW fix122]

        m_backbuffer_size[0] = (uint32_t)bb_desc.Width;
        m_backbuffer_size[1] = bb_desc.Height;

        auto vr = VR::get();
        const auto vr_enabled = vr->is_hmd_active();
        const auto is_vr_multipass = vr_enabled && vr->is_using_multipass();
        const auto frame = vr->get_render_frame_count();

        auto& copier = m_copiers[swapchain->GetCurrentBackBufferIndex() % m_copiers.size()];
        copier.wait(INFINITE);

        // [QUADVIEW] multipass quad view: three (or four) layers, the multipass path with more states
        const auto is_qv = is_vr_multipass && qv_engaged();
        // [NO PLUGIN fix91 19.11.2026] the mod's supersampling TAA anti-aliased the two eye views in this frame (no quad view)
        bool plain_ran = false;

        if (vr_enabled && is_vr_multipass) {
            release_retired_copies(false); // [COPY LIFETIME 15.11.2026]

            for (auto& state : m_eye_states) {
                if (state.scene_layer == nullptr) {
                    state.depth = nullptr;
                    state.color = nullptr;
                    state.motion_vectors = nullptr;
                    retire_copy(state.depth_copy); // [COPY LIFETIME 15.11.2026] (was = nullptr)
                    retire_copy(state.motion_vectors_copy);
                    retire_copy(state.color_copy);
                    continue;
                }

                const auto new_depth = state.scene_layer->get_depth_stencil_d3d12();
                const auto new_motion_vectors = state.scene_layer->get_motion_vectors_d3d12();

                if (new_depth != nullptr && (state.depth_copy == nullptr || new_depth != state.depth.Get())) {
                    retire_copy(state.depth_copy); // [COPY LIFETIME 15.11.2026]
                    state.depth_copy = state.scene_layer->get_depth_stencil()->clone();
                    state.depth = new_depth;

                    REVR_LOG(spdlog::info("[TemporalUpscaler] Made clone of depth stencil @ {:x}", (uintptr_t)state.depth_copy.get()));
                }

                if (new_motion_vectors != nullptr && (state.motion_vectors_copy == nullptr || new_motion_vectors != state.motion_vectors.Get())) {
                    const auto motion_vectors_state = state.scene_layer->get_motion_vectors_state();

                    if (motion_vectors_state != nullptr) {
                        const auto rtv = motion_vectors_state->get_rtv(0);

                        if (rtv != nullptr) {
                            auto tex = rtv->get_texture_d3d12();

                            if (tex != nullptr) {
                                retire_copy(state.motion_vectors_copy); // [COPY LIFETIME 15.11.2026]
                                state.motion_vectors_copy = tex->clone();
                                state.motion_vectors = new_motion_vectors;

                                REVR_LOG(spdlog::info("[TemporalUpscaler] Made clone of motion vectors @ {:x}", (uintptr_t)state.motion_vectors_copy.get()));
                            }
                        }
                    }
                }
            }

            if (is_qv) {
                evaluate_quadview(copier, backbuffer.Get(), frame); // [QUADVIEW] wide + fovea + composite + copy
            } else {
                // [MENU HANG 14.11.2026] Both eyes are checked before anything is recorded. With a single scene layer (main menu)
                // the left eye used to be evaluated (plugin call without submit, copy into the copier) and the function returned
                // before the copier was submitted: every frame added unsubmitted work to the copier and the plugin, and the pile
                // ran on the GPU at once (device hung 887a0006, SteamVR and the driver reset with it). Now such a frame records
                // nothing and keeps the game's own image; the first complete frame afterwards starts the upscaler history again.
                struct EyeInputs {
                    ID3D12Resource* color{};
                    ID3D12Resource* depth{};
                    ID3D12Resource* motion_vectors{};
                };

                EyeInputs inputs[2]{};
                int missing = 0; // 0 = both eyes complete, otherwise 1 + eye * 3 + kind

                for (auto i = 0; i < 2 && missing == 0; ++i) {
                    const auto& state = m_eye_states[i];

                    if (state.color == nullptr || state.depth_copy == nullptr || state.motion_vectors_copy == nullptr || state.color_copy == nullptr) {
                        missing = 1 + i * 3;
                        break;
                    }

                    if (state.depth_copy->get_d3d12_resource_container() == nullptr ||
                        state.motion_vectors_copy->get_d3d12_resource_container() == nullptr ||
                        state.color_copy->get_d3d12_resource_container() == nullptr)
                    {
                        missing = 2 + i * 3;
                        break;
                    }

                    inputs[i].motion_vectors = state.motion_vectors_copy->get_d3d12_resource_container()->get_native_resource();
                    inputs[i].depth = state.depth_copy->get_d3d12_resource_container()->get_native_resource();
                    inputs[i].color = state.color_copy->get_d3d12_resource_container()->get_native_resource();

                    if (inputs[i].motion_vectors == nullptr || inputs[i].depth == nullptr || inputs[i].color == nullptr) {
                        missing = 3 + i * 3;
                    }
                }

                // one line per change instead of one per frame
                if (missing != m_mp_missing) {
                    if (missing != 0) {
                        static constexpr const char* kinds[] = {"eye state", "resource", "native resource"};
                        REVR_LOG(spdlog::warn("[TemporalUpscaler] Missing {} for eye {} (D3D12): upscaling paused, nothing recorded until both eyes are complete",
                            kinds[(missing - 1) % 3], (missing - 1) / 3));
                    } else {
                        REVR_LOG(spdlog::info("[TemporalUpscaler] both eyes complete again after {} skipped frame(s): upscaling resumes with a history reset",
                            m_mp_skipped));
                    }

                    m_mp_missing = missing;
                }

                if (missing != 0) {
                    ++m_mp_skipped;
                    m_mp_reset_next = true;
                } else {
                    const bool reset_history = m_mp_reset_next;
                    m_mp_reset_next = false;
                    m_mp_skipped = 0;

                    // [NO PLUGIN fix91 19.11.2026] the mod's supersampling TAA per eye, where the upscaler plugin ran
                    ID3D12Resource* const colors[2]{inputs[0].color, inputs[1].color};
                    ID3D12Resource* const depths[2]{inputs[0].depth, inputs[1].depth};
                    ID3D12Resource* const motions[2]{inputs[0].motion_vectors, inputs[1].motion_vectors};
                    plain_ran = evaluate_plain(copier, backbuffer.Get(), frame, colors, depths, motions, reset_history);

                    if (!plain_ran) {
                        m_mp_reset_next = true; // (a frame without its pass: the histories start again with the next one)
                    }
                }
            }
        } else {
            // [NO PLUGIN fix91 19.11.2026] One view (no headset, or a technique with one eye per frame): the upscaler plugin
            // anti-aliased it here and its result was copied over the window. Nothing is recorded now: the game's own image
            // stays, and the view gets no jitter (on_scene_layer_update: there is nothing that takes it out again).
        }

        m_plain_aa_now.store(plain_ran, std::memory_order_relaxed); // [NO PLUGIN fix91] for the next frame's jitter
        copier.execute();

#if !REVR_RELEASE // [RELEASE SWITCH fix141] dumps, the jitter check, captures, the engine probe: debug build only
        // [QUADVIEW] debug frame dump: wait for this frame's GPU work (only in the frame the dump was asked for),
        // then write the images next to the logs
        if (m_qv_dump_pending) {
            m_qv_dump_pending = false;
            copier.wait(INFINITE);

            if (m_qv_dump_pre_ctx != nullptr) {
                m_qv_dump_pre_ctx->wait(INFINITE);
                m_qv_dump_pre_ctx = nullptr;
            }

            auto& qv = quadview::State::get();
            const auto report = m_qv_dump_header + m_qv_dump.write_all(m_qv_dump_path, 1024);
            qv.push_dump_report(std::format("  folder: {}\n", m_qv_dump_path.string()) + report);
            qv.set_dump_dir(m_qv_dump_path); // the VR submit of this frame adds the two eye images
            REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view frame dump written to {}", m_qv_dump_path.string()));
        }

        // [JITTER CHECK 10.11.2026] all frames recorded: wait for this frame's work, then write them next to the dump
        if (m_jc.write) {
            m_jc.write = false;
            copier.wait(INFINITE);

            if (m_jc.pre_ctx != nullptr) {
                m_jc.pre_ctx->wait(INFINITE);
                m_jc.pre_ctx = nullptr;
            }

            const auto base = m_qv_dump_path.empty() ? REFramework::get_persistent_dir("quadview_dump") / "jitter_check"
                                                     : m_qv_dump_path.parent_path() / "jitter_check";
            std::string report{};

            for (int i = 0; i < JitterCheck::FRAMES; ++i) {
                report += std::format("frame {}:\n", i) + m_jc.dumps[i].write_all(base / std::format("frame{}", i), 512);
                m_jc.dumps[i].clear();
            }

            std::string log{};
            {
                std::scoped_lock _{m_jc.mtx};
                log = std::move(m_jc.log);
                m_jc.log.clear();
                m_jc.layers.clear();
            }

            std::error_code ec{};
            std::filesystem::create_directories(base, ec);
            std::ofstream f(base / "jitter_check.txt");
            f << log << "\n" << report;
            REVR_LOG(spdlog::info("[TemporalUpscaler] jitter check written to {}", base.string()));
        }

        // [SUPER TAA 16.11.2026] all frames of the supersampling TAA's capture recorded: wait for this frame's work, then
        // write them (the game stops for a moment, like for a dump)
        if (m_sc.write_pending()) {
            copier.wait(INFINITE);
            super_capture_write();
        }

        // [ENGINE PROBE 16.11.2026] the engine's buffers read back in this frame: written
        if (m_probe.written_pending) {
            copier.wait(INFINITE);
            engine_probe_write();
        }

        static bool once = true;

        if (once) {
            REVR_LOG(spdlog::info("Successfully rendered with TemporalUpscaler"));
            once = false;
        }
#endif

    } else {
        auto& hook = g_framework->get_d3d11_hook();
        auto swapchain = hook->get_swap_chain();
        auto device = hook->get_device();

        // Get the context.
        ComPtr<ID3D11DeviceContext> context{};
        device->GetImmediateContext(&context);

        // Get the back buffer.
        ComPtr<ID3D11Texture2D> backbuffer{};
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

        if (backbuffer == nullptr) {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get backbuffer (D3D11)"));
            return;
        }
    }
}

bool TemporalUpscaler::on_first_frame() {
    REVR_LOG(spdlog::info("[TemporalUpscaler] Initializing first frame..."));

    m_first_frame_finished = true;
    m_is_d3d12 = g_framework->is_dx12();

    // [NO PLUGIN fix91 19.11.2026] The upscaler plugin was set up here (its log, the game's command queue or device). The mod's own
    // passes are Direct3D 12 only: on Direct3D 11 this module stays off and the game renders as it does without it.
    if (!m_is_d3d12) {
        REVR_LOG(spdlog::info("[TemporalUpscaler] Direct3D 11: the tri-view and the mod's anti-aliasing need Direct3D 12, not used"));
        return false;
    }

    if (!init_upscale_features()) {
        return false;
    }

    m_initialized = true;

    return true;
}

bool TemporalUpscaler::init_upscale_features() {
    REVR_LOG(spdlog::info("[TemporalUpscaler] Initializing upscale features..."));

    uint32_t out_w = 0;
    uint32_t out_h = 0;
    uint32_t out_format = 0;

    if (m_is_d3d12) {
        auto& hook = g_framework->get_d3d12_hook();

        auto swapchain = hook->get_swap_chain();
        ComPtr<ID3D12Resource> backbuffer{};

        if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get backbuffer (D3D12)"));
            return false;
        }

        // Get bb desc
        auto bb_desc = backbuffer->GetDesc();
        eye_image_size(bb_desc.Width, bb_desc.Height); // [SMALL WINDOW fix122]

        m_backbuffer_size[0] = (uint32_t)bb_desc.Width;
        m_backbuffer_size[1] = bb_desc.Height;

        out_w = bb_desc.Width;
        out_h = bb_desc.Height;
        out_format = bb_desc.Format;

        for (auto& copier : m_copiers) {
            copier.setup(L"Upscaler copier"); // [DRED 14.11.2026] named: a device removed report lists the lists by name
        }
    } else {
        auto& hook = g_framework->get_d3d11_hook();

        auto swapchain = hook->get_swap_chain();
        auto device = hook->get_device();

        // Get the context.
        ComPtr<ID3D11DeviceContext> context{};
        device->GetImmediateContext(&context);

        // Get the back buffer.
        ComPtr<ID3D11Texture2D> backbuffer{};
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

        if (backbuffer == nullptr) {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get backbuffer (D3D11)"));
            return false;
        }

        // Get bb desc
        D3D11_TEXTURE2D_DESC bb_desc{};
        backbuffer->GetDesc(&bb_desc);

        out_w = bb_desc.Width;
        out_h = bb_desc.Height;
        out_format = bb_desc.Format;
    }

    // [QUADVIEW] remember which quad-view state these instances belong to; create the quad-view set if wanted
    {
        auto& qv = quadview::State::get();
        m_qv_init_requested = qv.requested();
        m_qv_init_generation = qv.generation();

        // [STEREO FOVEA 27.10.2026] the quad view runs in multipass (both eyes every frame)
        if (m_qv_init_requested && m_is_d3d12 && VR::get()->is_hmd_active() && VR::get()->is_using_multipass()) {
            if (init_quadview_features(out_w, out_h, out_format)) {
                m_features = true; // [NO PLUGIN fix91]
                update_motion_scale();
                return true;
            }

            const auto why = get_quadview_status();
            REVR_LOG(spdlog::error("[TemporalUpscaler] tri-view could not be set up ({}), the two eye views without it", why));

            // stop the clone camera and go on without the quad view (no second re-init)
            qv.mark_failed(m_qv_init_generation, why);
            m_qv_init_requested = qv.requested();
        }
    }

    // [NO PLUGIN fix91 19.11.2026] Without the quad view the plugin's instance per eye was created here. The supersampling
    // TAA of evaluate_plain is made with its first frame.
    m_features = true;
    m_plain_failed = false;

    // [CANVAS fix92 19.11.2026] The two eye images are made here, as the plugin's result textures were: they must exist from
    // the set-up on. The headset submit takes the window's picture for the flat canvas only while multipass has both eye
    // textures (D3D12Component::on_frame, multipass_eyes), and the first version of this made them with the first frame
    // that had both eye views drawn. A canvas shown before that stayed black: RE7's movie before the main menu was heard
    // and not seen (user, 19.11.2026; no log of that test, the main menu after it is drawn with one view). evaluate_plain
    // writes them (nothing is in them until then; with the canvas shown the eyes are blanked anyway).
    if (m_is_d3d12) {
        auto* device = g_framework->get_d3d12_hook()->get_device();

        for (int i = 0; i < 2; ++i) {
            if (device == nullptr || !ensure_uav_image(device, m_plain_eye[i], out_w, out_h, (DXGI_FORMAT)out_format,
                                         i == 0 ? L"RE4VR eye image 0 (no tri-view)" : L"RE4VR eye image 1 (no tri-view)")) {
                REVR_LOG(spdlog::error("[TemporalUpscaler] No tri-view: eye image {} could not be made (the flat canvas stays black)", i));
            }
        }
    }

    update_motion_scale();

    REVR_LOG(spdlog::info("[TemporalUpscaler] No tri-view: eye image {}x{}, each view rendered at {}x{}", out_w, out_h, get_render_width(), get_render_height()));

    return true;
}

void TemporalUpscaler::release_upscale_features() {
    if (!m_features) { // ([NO PLUGIN fix91] was: no instance of the plugin)
        return;
    }

    for (auto& copier : m_copiers) {
        copier.wait(2000);
    }

    release_quadview_features(); // [QUADVIEW]

    // [NO PLUGIN fix91 19.11.2026] what evaluate_plain made (the copiers' lists are done: the waits above)
    m_plain_taa.reset();

    for (auto& t : m_plain_eye) {
        t.Reset();
    }

    m_plain_frame = {-10, -10};
    m_plain_failed = false;
    m_plain_logged = false;
    m_plain_aa_now.store(false, std::memory_order_relaxed);
    m_features = false;

    m_wants_reinitialize = true;
    for (auto& copier : m_copiers) {
        copier.reset();
    }
    
    for (auto& state : m_eye_states) {
        state.color.Reset();
        state.depth.Reset();
        state.motion_vectors.Reset();
        state.scene_layer = nullptr;

        state.color_copy.reset();
        state.depth_copy.reset();
        state.motion_vectors_copy.reset();
    }

    release_retired_copies(true); // [COPY LIFETIME 15.11.2026] (as the copies above)

    m_big_motion_vectors.Reset();
    m_big_depth.Reset();
    m_big_color.Reset();
}

// [NO PLUGIN fix91 19.11.2026] Multipass without the quad view (the other half of the A/B benchmark, a quad view that
// could not be set up, the flat canvas outside RE4): each eye's view, rendered at the eye image's size less a pixel, is
// anti-aliased by the mod's supersampling TAA into an eye image of the mod's. Until now the upscaler plugin did this
// (FSR or DLSS at the chosen quality) and its result texture was the eye image. An instance of
// d3d12::SuperTAA of its own with a history of the image's size (the "lighter" choice: at this size the 2x history
// would be 1.5 GB), the fovea's settings from the menu. The left eye goes to the window as before.
// color / depth / motion: the engine copies of the two eye views (resting in PIXEL_SHADER_RESOURCE, as the quad view
// reads them). false: nothing was recorded (the game's own image stays in the window for this frame, and the eye images
// keep what they held).
bool TemporalUpscaler::evaluate_plain(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, ID3D12Resource* const color[2],
    ID3D12Resource* const depth[2], ID3D12Resource* const motion[2], bool reset_history) {
    constexpr auto ENGINE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (m_plain_failed || backbuffer == nullptr || !m_is_d3d12) {
        return false;
    }

    auto* device = g_framework->get_d3d12_hook()->get_device();
    auto* cmd = copier.cmd_list.Get();

    if (device == nullptr || cmd == nullptr) {
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        if (color[i] == nullptr || depth[i] == nullptr || motion[i] == nullptr) {
            return false;
        }

        // depth and motion at the colour's size only (the pass reads them at the colour's place; RE7's video tape draws the
        // game camera smaller for a frame or two): such a frame keeps the game's own image
        const auto cd = color[i]->GetDesc();
        const auto dd = depth[i]->GetDesc();
        const auto md = motion[i]->GetDesc();

        if (dd.Width != cd.Width || dd.Height != cd.Height || md.Width != cd.Width || md.Height != cd.Height) {
            return false;
        }
    }

    // for good (until the next set-up): the eye images keep their last picture, the window and the flat canvas show the
    // game's own image
    const auto fail = [&](const std::string& why) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] without the tri-view: {} -- the eye images are no longer written until the next set-up "
                      "(the window and the flat canvas show the game's own image)", why));
        m_plain_failed = true;
        return false;
    };

    auto bb = backbuffer->GetDesc();
    eye_image_size(bb.Width, bb.Height); // [SMALL WINDOW fix122] (the eye images keep the headset's size)

    for (int i = 0; i < 2; ++i) {
        if (!ensure_uav_image(device, m_plain_eye[i], (uint32_t)bb.Width, bb.Height, bb.Format,
                i == 0 ? L"RE4VR eye image 0 (no tri-view)" : L"RE4VR eye image 1 (no tri-view)")) {
            return fail("an eye image could not be made");
        }
    }

    const uint32_t render_w = (std::max)(get_render_width(), 1u);
    const uint32_t render_h = (std::max)(get_render_height(), 1u);
    const auto st = quadview::State::get().settings().super_taa;

    for (int i = 0; i < 2; ++i) {
        const auto jitter = jitter_of_frame(get_evaluate_id((uint32_t)i) - 1, (int64_t)(uint32_t)frame);

        d3d12::SuperTAA::Params sp{};
        sp.s.scale = 1u;
        sp.s.filter_scale = st.filter_scale;
        sp.s.stability = st.stability;
        sp.s.clip_gamma = st.clip_gamma;
        sp.s.motion_reduce = st.motion_reduce;
        sp.s.depth_reject = st.depth_reject;
        sp.s.history_sharpness = st.history_sharpness;
        sp.s.sharpen = st.sharpen;
        sp.s.luma_weight = st.luma_weight;
        sp.s.dering = st.lighter ? 2u : 1u;
        sp.s.nearest_sample = st.lighter;
        sp.s.box_resolve = !st.lanczos;
        sp.s.follow = st.follow;
        sp.s.follow_threshold = st.follow_threshold;
        sp.s.follow_memory = st.follow_memory;
        sp.s.protect = st.protect;
        sp.s.protect_range = st.protect_range;
        sp.s.protect_spark = st.protect_spark;
        sp.s.protect_memory = st.protect_memory;
        sp.s.arrive = st.arrive;
        sp.eye = (uint32_t)i;
        // same convention the fovea's pass gets: pixels = mv * m_motion_scale -> UV = mv * m_motion_scale / render size
        sp.mv_scale_x = m_motion_scale[0] / (float)render_w;
        sp.mv_scale_y = m_motion_scale[1] / (float)render_h;
        sp.jitter_x = jitter[0];
        sp.jitter_y = jitter[1];
        sp.reverse_z = true;
        // a frame without this eye's pass before this one: start again
        sp.reset = reset_history || m_plain_frame[i] + 1 != (int64_t)frame;

        // distance from the depth value: rows 2 and 3 of the eye's inverse projection (as the fovea's pass takes them). The
        // engine stores the depth reversed relative to that projection.
        {
            const glm::mat4 reverse_z{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
            const glm::mat4 proj = VR::get()->get_projection_matrix((uint32_t)i);
            const auto inv = glm::inverse(reverse_z * proj);
            sp.depth_k[0] = inv[2][2];
            sp.depth_k[1] = inv[3][2];
            sp.depth_k[2] = inv[2][3];
            sp.depth_k[3] = inv[3][3];
            sp.depth_k_valid = std::isfinite(sp.depth_k[0]) && std::isfinite(sp.depth_k[1]) && std::isfinite(sp.depth_k[2]) &&
                               std::isfinite(sp.depth_k[3]) && (std::abs(sp.depth_k[2]) + std::abs(sp.depth_k[3])) > 1e-9f;
        }

        if (!m_plain_taa.dispatch(device, cmd, color[i], motion[i], depth[i], ENGINE_STATE, m_plain_eye[i].Get(), sp)) {
            return fail(std::string{"the supersampling TAA failed ("} + m_plain_taa.last_error() + ")");
        }

        m_plain_frame[i] = (int64_t)frame;
        copier.has_commands = true;
    }

    // the window (and the flat canvas, which reads it) shows the left eye ([SMALL WINDOW fix122] while it has its size)
    if (mirror_fits(backbuffer)) {
        copier.copy(m_plain_eye[0].Get(), backbuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PRESENT);
    }

    copier.has_commands = true;

    if (!m_plain_logged) {
        m_plain_logged = true;
        REVR_LOG(spdlog::info("[TemporalUpscaler] without the tri-view: the two eye views {}x{} anti-aliased by the mod's supersampling TAA (history 1x) into {}x{}",
            render_w, render_h, (uint64_t)bb.Width, bb.Height));
    }

    return true;
}

void TemporalUpscaler::on_post_present() {
    m_rendering = false;

    if (!ready()) {
        return;
    }
}

void TemporalUpscaler::on_device_reset() {
    release_upscale_features();
    m_wants_reinitialize = true;
}

void TemporalUpscaler::on_view_get_size(REManagedObject* scene_view, float* result) {
    if (!ready() && (!m_rendering || !m_set_view)) {
        m_set_view = false;
        return;
    }

    /*auto regenny_view = (regenny::via::SceneView*)scene_view;
    auto window = regenny_view->window;

    static auto via_scene_view = sdk::find_type_definition("via.SceneView");
    static auto set_display_type_method = via_scene_view->get_method("set_DisplayType");

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
        if (m_enabled) {
#if TDB_VER <= 49
            if (!g_previous_size) {
                g_previous_size = regenny::via::Size{ (float)window->width, (float)window->height };
            }
#endif
            window->width = GetRenderWidth();
            window->height = GetRenderHeight();

            if (m_is_d3d12) {
                if (m_backbuffer_size[0] > 0 && m_backbuffer_size[1] > 0) {
                    if (std::abs((int)m_backbuffer_size[0] - (int)window->width) > 50 || std::abs((int)m_backbuffer_size[1] - (int)window->height) > 50) {
                        const auto now = VR::get()->get_game_frame_count();

                        if (!m_backbuffer_inconsistency) {
                            m_backbuffer_inconsistency_start = now;
                            m_backbuffer_inconsistency = true;
                        }

                        const auto is_true_inconsistency = (now - m_backbuffer_inconsistency_start) >= 5;

                        if (is_true_inconsistency) {
                            // Force a reset of the backbuffer size
                            window->width = GetRenderWidth() + 1;
                            window->height = GetRenderHeight() + 1;

                            spdlog::info("[TemporalUpscaler] Previous backbuffer size: {}x{}", m_backbuffer_size[0], m_backbuffer_size[1]);
                            spdlog::info("[TemporalUpscaler] Backbuffer size inconsistency detected, resetting backbuffer size to {}x{}", window->width, window->height);

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
            window->width = (uint32_t)window->borderless_size.w;
            window->height = (uint32_t)window->borderless_size.h;
#else
            if (g_previous_size) {
                window->width = (uint32_t)g_previous_size->w;
                window->height = (uint32_t)g_previous_size->h;

                g_previous_size = std::nullopt;
            }
#endif
        }

        wanted_width = (float)window->width;
        wanted_height = (float)window->height;
    }*/

    // spoof the size to the HMD's size
    auto vr = VR::get();

    const auto evaluate_id = get_evaluate_id(0);

    /*if (vr->is_hmd_active() && vr->is_using_multipass()) {
        result[0] = (float)GetRenderWidth(evaluate_id) / 2.0f;
        result[1] = (float)GetRenderHeight(evaluate_id);
    } else {*/
        result[0] = (float)get_render_width();
        result[1] = (float)get_render_height();
    //}

    m_set_view = true;
}

void TemporalUpscaler::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (!ready()) {
        return;
    }
}

void TemporalUpscaler::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    if (!ready()) {
        return;
    }

    auto& vr = VR::get();

    // Other layers appear when using scopes or mirrors are displayed
    if (!layer->is_fully_rendered() || !layer->has_main_camera()) {
        return;
    }

    const auto vr_enabled = vr->is_hmd_active();
    const auto is_vr_multipass = vr_enabled && vr->is_using_multipass();
    const bool qv_on = qv_engaged() && is_vr_multipass;

    // Which of our states this layer is (EndRendering assigned the scene layers): other layers (scopes, mirrors)
    // are not ours. [STEREO FOVEA 27.10.2026] quad view with the shared periphery [0] wide view [1] fovea left [2] fovea
    // right; with a wide view per eye [0] wide left [1] wide right [2] fovea left [3] fovea right; multipass [0] left
    // [1] right; else [0].
    int32_t state_index = -1;

    for (uint32_t i = 0; i < m_layers_in_use && i < m_eye_states.size(); ++i) {
        if (m_eye_states[i].scene_layer != nullptr && layer == m_eye_states[i].scene_layer) {
            state_index = (int32_t)i;
            break;
        }
    }

    if (state_index < 0) {
        return;
    }

    // [QUADVIEW] the fovea (inset) layers get their own jitter sequence and history (plugin ids 3/4)
    // [MONO PERIPHERY] multipass with the shared wide view: [0] wide (both eyes) [1] fovea left [2] fovea right
    const bool qv_inset_layer = qv_on && (m_qv_mono ? state_index >= 1 : state_index >= 2);

    auto scene_info = layer->get_scene_info();
    auto depth_distortion_scene_info = layer->get_depth_distortion_scene_info();
    auto filter_scene_info = layer->get_filter_scene_info();
    auto jitter_disable_scene_info = layer->get_jitter_disable_scene_info();
    auto jitter_disable_post_scene_info = layer->get_jitter_disable_post_scene_info();
    auto z_prepass_scene_info = layer->get_z_prepass_scene_info();

    uint32_t vr_index = 0;

    if (is_vr_multipass) {
        if (qv_on && m_qv_mono) {
            vr_index = state_index == 0 ? 0u : (uint32_t)(state_index - 1); // the eye of a fovea view; the wide view uses slot 0
        } else {
            vr_index = (uint32_t)(state_index % 2); // the eye
        }
    } else if (vr_enabled) {
        vr_index = vr->get_game_frame_count();
    }

    const auto evaluate_id = qv_inset_layer ? get_qv_fovea_id(vr_index) : get_evaluate_id(vr_enabled ? vr_index : 0); // [QUADVIEW]
    const auto evaluate_index = evaluate_id - 1;

    // [FSR DIRECT 06.11.2026] FSR 4 by the mod has no plugin instance for the fovea ids: the sequence length from the layout
    // [FOVEA OWN fix91 19.11.2026] no upscaler for the fovea at all: the same rule (m_qv_fovea_own, see the set-up)
    const bool fsr_layer = qv_inset_layer && (m_qv_fsr || m_qv_fovea_own);
    const uint32_t fsr_jitter_w = m_qv_fovea_own ? m_qv_fovea_jitter_w : m_qv_fsr_jitter_w;
    const bool fsr_frame_index = m_qv_fovea_own ? m_qv_fovea_frame_jitter : m_qv_fsr_atlas.valid;
    // [SUPER TAA 16.11.2026] the mod's supersampling TAA anti-aliases the fovea views: its own jitter sequence
    // (quadview::halton_jitter), the same in both views (the index is the engine's render frame) and as long as its history
    // asks for (2 x 2 history texels per pixel: 32 frames)
    // (m_super_scale_now: whether it is the fovea's anti-aliasing and its history's scale, as the last present saw them)
    const uint32_t super_scale = qv_inset_layer && m_is_d3d12 ? m_super_scale_now.load(std::memory_order_relaxed) : 0u;
    const bool super_layer = super_scale != 0;
    // [PERIPHERY SUPER fix80 17.11.2026] the shared wide view under the supersampling TAA: the same sequence, as long as its
    // history asks for (2 x 2 history texels per pixel of the wide view: 32 frames), counted per view as before
    // (m_super_wide_now: its history's texels per pixel as the last present saw the settings, 0 = the light TAA)
    const uint32_t super_wide = qv_on && m_qv_mono && state_index == 0 && m_is_d3d12 ? m_super_wide_now.load(std::memory_order_relaxed) : 0u;
    const bool super_wide_layer = super_wide != 0;
    // [FSR MODEL 09.11.2026] the fovea's width in FSR 4's output (larger with another network)
    const auto phase = super_layer ? quadview::jitter_phase_count(m_qv_layout.render_w, m_qv_layout.inset_w * super_scale)
                       : super_wide_layer ? quadview::jitter_phase_count(m_qv_layout.render_w, m_qv_layout.render_w * super_wide)
                       : fsr_layer ? quadview::jitter_phase_count(m_qv_layout.render_w, fsr_jitter_w != 0 ? fsr_jitter_w : m_qv_layout.inset_w)
                       // [NO PLUGIN fix91 19.11.2026] any other view: the length for its scale, display / render (AMD's rule, the
                       // one the plugin's instance of the view gave: created at the render size in the quad view, 8 frames, and at
                       // the eye image's size without it)
                                   : quadview::jitter_phase_count(get_render_width(), qv_on ? get_render_width() : m_backbuffer_size[0]);
    // [NO PLUGIN fix91 19.11.2026] Without the quad view the two eye views are jittered while the mod's supersampling TAA
    // anti-aliases them (evaluate_plain, as the last present saw it). Any other view without the quad view gets none:
    // nothing takes it out of the picture again.
    const bool plain_aa = !qv_on && is_vr_multipass && m_is_d3d12 && m_plain_aa_now.load(std::memory_order_relaxed);
    const auto w = (float)get_render_width();
    const auto h = (float)get_render_height();

    float x = 0.0f;
    float y = 0.0f;

    // [POSE_FREEZE 2026-08-11] Waehrend des Scope-Freeze KEIN Jitter.
    // Der Jitter wird in die Off-Center-Terme der Projektion gerechnet
    // (projection_matrix[2][0] += x, weiter unten), und genau die skaliert der Scope-Zoom
    // in VR::apply_projection_tweaks anschliessend noch einmal mit dem Zoomfaktor.
    // Dem Upscaler wird der Jitter aber in Pixeln der UNSKALIERTEN Projektion gemeldet,
    // er rechnet also einen anderen Betrag wieder heraus, als tatsaechlich drinsteckt --
    // der Rest bleibt als hochfrequentes Wackeln stehen. Es faellt nur im Scope auf, weil
    // dort das Bild sonst voellig still steht, und der master kennt es gar nicht, weil er
    // keinen Upscaler hat. Ausserhalb des Zielens bleibt alles wie bisher.
    const auto scope_freeze = VR::get()->is_pose_freeze();

    if (m_jitter && !scope_freeze && (qv_on || plain_aa)) {
        if ((fsr_layer && fsr_frame_index) || super_layer) {
            // [FSR DIRECT 06.11.2026] both foveas in one pass need the same jitter in both views: the index from the engine's
            // render frame (the same for every view of a frame), not a count per view
            m_jitter_indices[evaluate_index] = (uint32_t)vr->get_game_frame_count();
        } else {
            m_jitter_indices[evaluate_index]++;
        }

        // [SUPER TAA 16.11.2026] the mod's sequence (Halton 2, 3: the one AMD gives for FSR)
        // [NO PLUGIN fix91 19.11.2026] ... for every view: the plugin's GetJitterOffset gave the views not under the
        // supersampling TAA the same sequence
        quadview::halton_jitter(m_jitter_indices[evaluate_index], (uint32_t)(std::max)((int)phase, 1), x, y);

        m_jitter_offsets[evaluate_index][0] = -x;
        m_jitter_offsets[evaluate_index][1] = -y;

        // from FSR2 code
        x = m_jitter_scale[0] * (x / w);
        y = m_jitter_scale[1] * (y / h);
    } else {
        m_jitter_offsets[evaluate_index][0] = 0.0f;
        m_jitter_offsets[evaluate_index][1] = 0.0f;
    }

    // [JITTER LATCH 10.11.2026] kept for this frame: the upscaling of this frame may run after the next frame's update
    {
        const auto key = (int64_t)(uint32_t)vr->get_update_frame_count();
        std::scoped_lock _{m_jitter_frame_mtx};
        m_jitter_by_frame[evaluate_index][(size_t)key % JITTER_KEEP] = FrameJitter{key, m_jitter_offsets[evaluate_index][0], m_jitter_offsets[evaluate_index][1]};
    }

    auto add_jitter = [&](int32_t i, sdk::renderer::SceneInfo* scene_info) {
        if (scene_info == nullptr) {
            return;
        }

        // [UI MOTION fix125 22.11.2026] the view's matrices without jitter, of this frame and of the frame before (what the
        // engine's motion vectors are made of: the old ones below get this frame's jitter like the new ones), kept under
        // the frame's number for the interface's own motion (ui_motion_blocks)
        if (i == 0 && qv_on && m_qv_mono && state_index >= 0 && state_index <= 2) {
            const auto key = (int64_t)(uint32_t)vr->get_update_frame_count();
            std::scoped_lock _{m_jitter_frame_mtx};
            auto& k = m_ui_views_by_frame[state_index][(size_t)key % std::size(m_ui_views_by_frame[0])];
            k.frame = key;
            std::memcpy(k.proj, &scene_info->projection_matrix[0][0], sizeof(k.proj));
            std::memcpy(k.view, &scene_info->view_matrix[0][0], sizeof(k.view));
            std::memcpy(k.old_proj, &this->m_old_projection_matrix[evaluate_index][i][0][0], sizeof(k.old_proj));
            std::memcpy(k.old_view, &this->m_old_view_matrix[evaluate_index][i][0][0], sizeof(k.old_view));
        }

        auto old_projection_matrix = this->m_old_projection_matrix[evaluate_index][i];
        old_projection_matrix[2][0] += x;
        old_projection_matrix[2][1] += y;

        this->m_old_view_projection_matrix[evaluate_index][i] = scene_info->old_view_projection_matrix;
        scene_info->old_view_projection_matrix = old_projection_matrix * this->m_old_view_matrix[evaluate_index][i];

        this->m_old_projection_matrix[evaluate_index][i] = scene_info->projection_matrix;
        this->m_old_view_matrix[evaluate_index][i] = scene_info->view_matrix;

        scene_info->projection_matrix[2][0] += x;
        scene_info->projection_matrix[2][1] += y;

        scene_info->inverse_projection_matrix = glm::inverse(scene_info->projection_matrix);

        scene_info->view_projection_matrix = scene_info->projection_matrix * scene_info->view_matrix;
        scene_info->inverse_view_projection_matrix = glm::inverse(scene_info->view_projection_matrix);

    };

    add_jitter(0, scene_info);
    add_jitter(1, depth_distortion_scene_info);
    add_jitter(2, filter_scene_info);
    add_jitter(3, jitter_disable_scene_info);
    add_jitter(4, jitter_disable_post_scene_info);
    add_jitter(5, z_prepass_scene_info);

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the jitter check: debug build only
    // [JITTER CHECK 10.11.2026] what this view got: which of the engine's six matrix sets exist (bit i = set i above), the
    // sequence index and length, the offset in pixels and in clip space, and the projection's offset terms afterwards
    if (m_jc.next >= 0) {
        const uint32_t mask = (scene_info != nullptr ? 1u : 0u) | (depth_distortion_scene_info != nullptr ? 2u : 0u) | (filter_scene_info != nullptr ? 4u : 0u) |
                              (jitter_disable_scene_info != nullptr ? 8u : 0u) | (jitter_disable_post_scene_info != nullptr ? 16u : 0u) |
                              (z_prepass_scene_info != nullptr ? 32u : 0u);
        {
            std::scoped_lock _{m_jc.mtx};
            m_jc.layers[(void*)layer] = state_index;
        }
        jitter_check_line(std::format("update game frame {} (frame key {}) view {}{} | matrix sets 0x{:02x} | jitter on {} scope freeze {} | index {} of {} | pixels ({:+.4f}, {:+.4f}) "
                                      "| clip ({:+.6f}, {:+.6f}) | render {}x{} | projection [2][0] {:+.6f} [2][1] {:+.6f}",
            (uint64_t)vr->get_game_frame_count(), vr->get_update_frame_count(), state_index, qv_inset_layer ? " (fovea)" : " (wide)", mask, m_jitter, scope_freeze,
            m_jitter_indices[evaluate_index], phase, m_jitter_offsets[evaluate_index][0], m_jitter_offsets[evaluate_index][1], x, y, w, h,
            scene_info != nullptr ? scene_info->projection_matrix[2][0] : 0.0f, scene_info != nullptr ? scene_info->projection_matrix[2][1] : 0.0f));
    }
#endif
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [JITTER CHECK 10.11.2026] the projection's offset terms when the engine draws the view (the jitter written at update still
// there, or written over in between?)
bool TemporalUpscaler::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    if (m_jc.next < 0 || layer == nullptr) {
        return true;
    }

    int32_t state_index = -1;
    {
        std::scoped_lock _{m_jc.mtx};
        const auto it = m_jc.layers.find((void*)layer);

        if (it == m_jc.layers.end()) {
            return true;
        }

        state_index = it->second;
    }

    const auto si = layer->get_scene_info();
    jitter_check_line(std::format("draw view {} | projection [2][0] {:+.6f} [2][1] {:+.6f}", state_index, si != nullptr ? si->projection_matrix[2][0] : 0.0f,
        si != nullptr ? si->projection_matrix[2][1] : 0.0f));
    return true;
}
#endif

std::array<float, 2> TemporalUpscaler::jitter_of_frame(size_t index, int64_t frame, bool* newer) const {
    index = (std::min)(index, (size_t)3);
    std::scoped_lock _{m_jitter_frame_mtx};
    const std::array<float, 2> latest{m_jitter_offsets[index][0], m_jitter_offsets[index][1]};

    if (newer != nullptr) {
        *newer = false;
    }

    if (frame >= 0) {
        const auto& k = m_jitter_by_frame[index][(size_t)frame % JITTER_KEEP];

        if (k.frame == frame) {
            if (newer != nullptr) {
                *newer = k.x != latest[0] || k.y != latest[1];
            }

            return {k.x, k.y};
        }
    }

    return latest;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void TemporalUpscaler::jitter_check_line(std::string line) {
    std::scoped_lock _{m_jc.mtx};

    if (m_jc.lines++ > 4000) {
        return;
    }

    m_jc.log += std::format("[{:.3f} ms] ", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
    m_jc.log += line;
    m_jc.log += "\n";
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool TemporalUpscaler::on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) {
    // [ENGINE PROBE 16.11.2026] the probed view's targets before its post effects (one load of an atomic otherwise)
    if (m_probe.state.load(std::memory_order_relaxed) == 1 && layer != nullptr) {
        engine_probe_copy(layer->get_parent(), render_context, 0);
    }

    return true;
}
#endif

bool TemporalUpscaler::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_ctx) {

    if (!ready()) {
        return true;
    }

    auto context = (sdk::renderer::RenderContext*)render_ctx;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    auto& state = m_eye_states[0];
    if (state.scene_layer != scene_layer)
        return true;

    auto& vr = VR::get();


    return true;
}

// [COPY SIZE 15.11.2026] A copy (CopyResource) only between textures of the same size. When the game makes its targets
// again at another size (RE7: into and out of the video tape, where the game camera is drawn at 2/3 of the size; the main
// menu), the copy is made again only at the next present, and in that frame the game's list copied the new target into the
// old copy (RE7 15.11.2026 16:03: device removed by a page fault in that CopyResource). That frame's copy is left out.
bool TemporalUpscaler::copy_fits(sdk::renderer::Texture* dst, sdk::renderer::Texture* src) {
    auto* dc = dst != nullptr ? dst->get_d3d12_resource_container() : nullptr;
    auto* sc = src != nullptr ? src->get_d3d12_resource_container() : nullptr;
    auto* dr = dc != nullptr ? dc->get_native_resource() : nullptr;
    auto* sr = sc != nullptr ? sc->get_native_resource() : nullptr;

    if (dr == nullptr || sr == nullptr) {
        return true; // not known: copied as before
    }

    const auto dd = dr->GetDesc();
    const auto sd = sr->GetDesc();

    if (dd.Width == sd.Width && dd.Height == sd.Height && dd.DepthOrArraySize == sd.DepthOrArraySize && dd.MipLevels == sd.MipLevels) {
        return true;
    }

    if (m_copy_size_skips_logged < 20) {
        ++m_copy_size_skips_logged;
        REVR_LOG(spdlog::info("[TemporalUpscaler] copy left out this frame: the game's texture {}x{}, the copy {}x{} (made again at the next present)",
            (uint64_t)sd.Width, sd.Height, (uint64_t)dd.Width, dd.Height));
    }

    return false;
}

void TemporalUpscaler::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    if (!ready()) {
        return;
    }

    auto context = (sdk::renderer::RenderContext*)render_context;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    if (scene_layer == nullptr) {
        return;
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the engine probe: debug build only
    // [ENGINE PROBE 16.11.2026] the probed view's targets where the mod takes its depth and motion
    if (m_probe.state.load(std::memory_order_relaxed) == 1) {
        engine_probe_copy(scene_layer, render_context, 1);
    }
#endif

    for (auto& state : m_eye_states) {
        if (state.scene_layer != scene_layer) {
            continue;
        }

        auto depth = scene_layer->get_depth_stencil();

        if (depth != nullptr && state.depth_copy != nullptr && copy_fits(state.depth_copy, depth)) {
            context->copy_texture(state.depth_copy, depth);
        }

        auto motion_vectors_state = scene_layer->get_motion_vectors_state();

        if (motion_vectors_state != nullptr && state.motion_vectors_copy != nullptr) {
            auto rtv = motion_vectors_state->get_rtv(0);

            if (rtv != nullptr) {
                if (auto motion_vectors = rtv->get_texture_d3d12(); motion_vectors != nullptr && copy_fits(state.motion_vectors_copy, motion_vectors)) {
                    context->copy_texture(state.motion_vectors_copy, motion_vectors);
                }
            }
        }
    }
}

bool TemporalUpscaler::on_pre_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    auto context = (sdk::renderer::RenderContext*)render_context;
    auto scene_layer = (sdk::renderer::layer::Scene*)layer->get_parent();

    if (scene_layer == nullptr)
        return true;

    auto& state = m_eye_states[0];
    if (state.scene_layer != scene_layer)
        return true;

    return true;
}

void TemporalUpscaler::on_prepare_output_layer_draw(sdk::renderer::layer::PrepareOutput* layer, void* render_context) {
    if (!ready()) {
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

    for (auto& state : m_eye_states) {
        if (state.scene_layer != scene_layer) {
            continue;
        }

        if (state.color_copy != nullptr && copy_fits(state.color_copy, tex)) { // [COPY SIZE 15.11.2026]
            context->copy_texture(state.color_copy, tex);
        }
    }
}

bool TemporalUpscaler::on_pre_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_context) {
    return true;
}

void TemporalUpscaler::on_output_layer_draw(sdk::renderer::layer::Output* layer, void* render_context) {
}

bool TemporalUpscaler::on_pre_output_layer_update(sdk::renderer::layer::Output* layer, void* render_context) {
    return true;
}

void TemporalUpscaler::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (hash == "BeginRendering"_fnv) {
        finish_release_resources();
        apply_post_processing();   // [POST_PROCESSING 28.09.2026] vor dem ready()-Ausstieg, gilt auch ohne Upscaler
    }

    if (!ready()) {
        return;
    }

    if (hash == "BeginRendering"_fnv) {
        //update_extra_scene_layer();
    }

    if (hash == "EndRendering"_fnv) {
        fix_output_layer();

        auto& vr = VR::get();
        const auto is_vr_multipass = vr->is_hmd_active() && vr->is_using_multipass();
        // [STEREO FOVEA 27.10.2026] multipass quad view (CameraDuplicator's order: primary, clone 0, clone 1, clone 2):
        // shared periphery [0] wide view [1] fovea left [2] fovea right; a wide view per eye [0] wide left [1] wide right
        // [2] fovea left [3] fovea right
        const auto stereo_layers = is_vr_multipass && vr->is_quadview_requested() && qv_engaged();
        const auto two_layers = is_vr_multipass;
        auto root_layer = sdk::renderer::get_root_layer();

        if (root_layer != nullptr) {
            auto [output_parent, output_layer] = root_layer->find_layer_recursive("via.render.layer.Output");
            auto valid_scene_layers =  is_vr_multipass ? vr->get_camera_duplicator().get_relevant_scene_layers() : (*output_layer)->find_fully_rendered_scene_layers();

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            // [STEREO FOVEA] what the engine renders this frame, for the log (only re-formatted when it changes)
            {
                uint64_t signature = valid_scene_layers.size();

                for (auto* l : valid_scene_layers) {
                    signature = signature * 131u + (uint64_t)l->get_view_id() + 1u;
                }

                if (signature != m_layer_signature) {
                    m_layer_signature = signature;
                    std::string report = std::format("{} fully rendered scene layer(s)", valid_scene_layers.size());
                    const auto primary = sdk::get_primary_camera();

                    for (size_t i = 0; i < valid_scene_layers.size(); ++i) {
                        auto* l = valid_scene_layers[i];
                        const auto cam = l->get_main_camera_if_possible();
                        const int32_t clone = cam != nullptr ? vr->get_camera_duplicator().get_clone_index(cam) : -1;
                        report += std::format("{} [{}] view_id={} camera={}", i == 0 ? ":" : ",", i, l->get_view_id(),
                            cam == nullptr ? "none" : (cam == primary ? "primary" : (clone >= 0 ? std::format("clone {}", clone) : "other")));
                    }

                    report += std::format(" | clones wanted={} made={}", vr->wanted_camera_clones(), vr->get_camera_duplicator().get_active_clone_count());

                    {
                        std::scoped_lock _{m_qv_status_mutex};
                        m_layer_report = report;
                    }

                    REVR_LOG(spdlog::info("[TemporalUpscaler] {}", report));
                }
            }
#endif

            if (valid_scene_layers.empty()) {
                for (auto& state : m_eye_states) {
                    state.scene_layer = nullptr;
                }

                m_layers_in_use = 1;
                return;
            }

            // [MONO PERIPHERY] three layers: wide view of both eyes, fovea left, fovea right
            const bool mono_layers = stereo_layers && vr->is_mono_periphery();

            if (mono_layers && valid_scene_layers.size() >= 3) {
                for (size_t i = 0; i < 3; ++i) {
                    m_eye_states[i].scene_layer = valid_scene_layers[i];
                }

                m_eye_states[3].scene_layer = nullptr;
                m_layers_in_use = 3;
            } else if (stereo_layers && !mono_layers && valid_scene_layers.size() >= 4) {
                for (size_t i = 0; i < 4; ++i) {
                    m_eye_states[i].scene_layer = valid_scene_layers[i];
                }

                m_layers_in_use = 4;
            } else if (valid_scene_layers.size() > 1) {
                if (!two_layers) {
                    if (m_displayed_scene == 0) {
                        m_eye_states[0].scene_layer = valid_scene_layers[0];
                    } else {
                        m_eye_states[0].scene_layer = valid_scene_layers[1];
                    }

                    m_eye_states[1].scene_layer = nullptr;
                    m_layers_in_use = 1;
                } else {
                    m_eye_states[0].scene_layer = valid_scene_layers[0];
                    m_eye_states[1].scene_layer = valid_scene_layers[1];
                    m_layers_in_use = 2;
                }

                m_eye_states[2].scene_layer = nullptr;
                m_eye_states[3].scene_layer = nullptr;
            } else {
                m_eye_states[0].scene_layer = valid_scene_layers[0];
                m_eye_states[1].scene_layer = nullptr;
                m_eye_states[2].scene_layer = nullptr;
                m_eye_states[3].scene_layer = nullptr;
                m_layers_in_use = 1;
            }

            if (output_layer != nullptr && *output_layer != nullptr) {
                m_output_layer = (decltype(m_output_layer))*output_layer;

                if (m_output_layer == nullptr) {
                    REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to find output layer"));
                }
            } else {
                REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to find output layer"));
                m_output_layer = nullptr;
            }
        } else {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get root layer"));

            for (auto& state : m_eye_states) {
                state.scene_layer = nullptr;
            }

            m_layers_in_use = 1;
            m_output_layer = nullptr;
        }

        for (auto& state : m_eye_states) {
            if (state.scene_layer == nullptr) {
                state.depth.Reset();
                state.motion_vectors.Reset();
                state.color.Reset();
                continue;
            }

            if (m_is_d3d12) {
                static auto potype = sdk::find_type_definition("via.render.layer.PrepareOutput")->get_type();
                auto prepareoutput_layer = (sdk::renderer::layer::PrepareOutput**)state.scene_layer->find_layer(potype);

                if (prepareoutput_layer == nullptr) {
                    state.depth.Reset();
                    state.motion_vectors.Reset();
                    state.color.Reset();
                    continue;
                }

                if (!two_layers) {
                    auto new_depth = state.scene_layer->get_depth_stencil_d3d12();
                    auto new_motion_vectors = state.scene_layer->get_motion_vectors_d3d12();

                    state.depth = new_depth;
                    state.motion_vectors = new_motion_vectors;
                }

                if (prepareoutput_layer != nullptr && *prepareoutput_layer != nullptr) {
                    const auto current_target_state = (*prepareoutput_layer)->get_output_state();

                    if (current_target_state != nullptr) {
                        const auto new_color = current_target_state->get_native_resource_d3d12();

                        if (two_layers && new_color != nullptr && (state.color == nullptr || new_color != state.color.Get() || state.color_copy == nullptr)) {
                            const auto color_tex = current_target_state->get_rtv(0)->get_texture_d3d12();
                            retire_copy(state.color_copy); // [COPY LIFETIME 15.11.2026]
                            state.color_copy = color_tex->clone();
                            state.color = new_color;

                            REVR_LOG(spdlog::info("[TemporalUpscaler] Created color copy"));
                        } else if (!two_layers) {
                            state.color = new_color;
                        }
                    } else {
                        state.color.Reset();
                    }
                } else {
                    state.color.Reset();
                }
            } else {
                state.depth.Reset();
                state.motion_vectors.Reset();
                state.color.Reset();
            }
        }

        auto camera = sdk::get_primary_camera();

        if (camera == nullptr) {
            REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get primary camera"));
            return;
        }

        static auto via_camera = sdk::find_type_definition("via.Camera");
        static auto get_near_clip_plane_method = via_camera->get_method("get_NearClipPlane");
        static auto get_far_clip_plane_method = via_camera->get_method("get_FarClipPlane");
        static auto get_fov_method = via_camera->get_method("get_FOV");
        static auto get_projection_matrix_method = via_camera->get_method("get_ProjectionMatrix");

        auto context = sdk::get_thread_context();
        m_nearz = get_near_clip_plane_method->call<float>(context, camera);
        m_farz = get_far_clip_plane_method->call<float>(context, camera);

        // [NEAR CLIP 09.11.2026] the near plane the VR mod gives the cameras, even when the game set its own again since
        if (const auto& vr = VR::get(); vr->is_hmd_active()) {
            m_nearz = (std::max)(m_nearz, vr->get_near_clip_min());
        }
        //m_fov = glm::radians(get_fov_method->call<float>(sdk::get_thread_context(), camera));

        Matrix4x4f projection_matrix{};
        get_projection_matrix_method->call<void>(&projection_matrix, context, camera);
        m_fov = 2.0f * std::atan(1.0f / projection_matrix[1][1]);

        static auto renderer_t = sdk::find_type_definition("via.render.Renderer");
        static auto render_config_t = sdk::find_type_definition("via.render.RenderConfig");
        static auto get_render_config_method = renderer_t->get_method("get_RenderConfig");

        static auto get_antialiasing_method = render_config_t->get_method("get_AntiAliasing");
        static auto set_antialiasing_method = render_config_t->get_method("set_AntiAliasing");

        static auto get_image_quality_rate_method = render_config_t->get_method("get_ImageQualityRate");
        static auto set_image_quality_rate_method = render_config_t->get_method("set_ImageQualityRate");

        auto renderer = renderer_t->get_instance();
        auto render_config = get_render_config_method->call<::REManagedObject*>(context, renderer);
        const auto antialiasing = get_antialiasing_method->call<via::render::RenderConfig::AntiAliasingType>(context, render_config);

        // Disable TAA
        switch (antialiasing) {
            case via::render::RenderConfig::AntiAliasingType::TAA: [[fallthrough]];
            case via::render::RenderConfig::AntiAliasingType::FXAA_TAA:
                if (!m_allow_taa) {
                    set_antialiasing_method->call<void*>(context, render_config, via::render::RenderConfig::AntiAliasingType::NONE);
                    REVR_LOG(spdlog::info("[TemporalUpscaler] TAA disabled"));
                }

                break;
            case via::render::RenderConfig::AntiAliasingType::NONE:
                if (m_allow_taa) {
                    set_antialiasing_method->call<void*>(context, render_config, via::render::RenderConfig::AntiAliasingType::TAA);
                }

                break;
            default:
                break;
        }

        // It's necessary to force the image quality to 1.0 otherwise
        // the motion & depth buffers become misaligned with the color buffer
        if (get_image_quality_rate_method != nullptr) {
            const auto image_quality_rate = get_image_quality_rate_method->call<float>(context, render_config);

            if (image_quality_rate != 1.0f) {
                set_image_quality_rate_method->call<void*>(context, render_config, 1.0f);
                REVR_LOG(spdlog::info("[TemporalUpscaler] Image quality rate set to 1.0"));
            }
        }
    }
}

void TemporalUpscaler::on_application_entry(void* entry, const char* name, size_t hash) {
    if (!ready()) {
        return;
    }

    if (hash == "EndRendering"_fnv) {
        fix_output_layer();
    }
}

void TemporalUpscaler::fix_output_layer() {
    if (m_last_output_layer != nullptr && m_cloned_output_layer != nullptr && m_original_output_layer != nullptr) {
        const auto current_state = *(sdk::renderer::TargetState**)((uintptr_t)m_last_output_layer + 0x88);

        if (current_state != m_last_output_state) {
            REVR_LOG(spdlog::info("[TemporalUpscaler] Output layer state changed!"));

            if (m_last_output_layer != m_original_output_layer) {
                *(sdk::renderer::TargetState**)((uintptr_t)m_original_output_layer + 0x88) = m_last_output_state;
            } else {
                *(sdk::renderer::TargetState**)((uintptr_t)m_cloned_output_layer + 0x88) = m_last_output_state;
            }

            m_last_output_state = current_state;
        }
    }
}

void TemporalUpscaler::update_extra_scene_layer() {
    if (!VR::get()->is_hmd_active() || !VR::get()->is_using_multipass()) {
        return;
    }

    static auto last_time = std::chrono::high_resolution_clock::now();
    const auto now = std::chrono::high_resolution_clock::now();

    if (now - last_time < std::chrono::milliseconds(1000)) {
        return;
    }

    last_time = now;

    auto output_layer = sdk::renderer::get_output_layer();
    if (output_layer == nullptr) {
        return;
    }

    auto scene_layers = output_layer->find_fully_rendered_scene_layers();
    if (scene_layers.empty()) {
        return;
    }

    if (m_made_extra_scene_layer && scene_layers.size() > 1) {
        /*if (!scene_layers.empty()) {
            if (scene_layers.size() > 1) {
                static auto potype = sdk::find_type_definition("via.render.layer.PrepareOutput")->get_type();
                auto prepare_output_layer = (sdk::renderer::layer::PrepareOutput**)scene_layers[0]->find_layer(potype);

                if (prepare_output_layer == nullptr) {
                    spdlog::error("[TemporalUpscaler] Failed to find PrepareOutput layer");
                    return;
                }

                const auto current_target_state = (*prepare_output_layer)->get_output_state();

                if (m_new_target_state == nullptr) {
                    if (current_target_state != nullptr) {
                        m_new_target_state = current_target_state->clone();
                        (*prepare_output_layer)->set_output_state(m_new_target_state);

                        spdlog::info("[TemporalUpscaler] Created new target state");
                    } else {
                        spdlog::error("[TemporalUpscaler] Current target state is null");
                    }
                } else if (current_target_state != m_new_target_state) {
                    spdlog::error("[TemporalUpscaler] Target state mismatch");
                    m_new_target_state = nullptr;
                }
            } else {
                //spdlog::error("[TemporalUpscaler] Failed to find second scene layer");
            }
        }*/

        return;
    }

    auto camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto camera_gameobject = sdk::call_object_func_easy<::REGameObject*>(camera, "get_GameObject");

    if (camera_gameobject == nullptr) {
        return;
    }

    /*const auto tdef = sdk::find_type_definition("via.render.Mirror");
    if (tdef == nullptr) {
        spdlog::error("[TemporalUpscaler] Failed to find via.render.Mirror type definition");
        return;
    }

    const auto runtime_type = tdef->get_runtime_type();

    if (runtime_type == nullptr) {
        spdlog::error("[TemporalUpscaler] Failed to get via.render.Mirror runtime type");
        return;
    }

    const auto new_comp = sdk::call_object_func_easy<::REComponent*>(camera_gameobject, "createComponent(System.Type)", runtime_type);

    if (new_comp == nullptr) {
        spdlog::error("[TemporalUpscaler] Failed to create via.render.Mirror component");
        return;
    }*/

    const auto tdef = sdk::find_type_definition("via.render.RenderOutput");
    if (tdef == nullptr) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to find via.render.RenderOutput type definition"));
        return;
    }

    const auto runtime_type = tdef->get_runtime_type();

    if (runtime_type == nullptr) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get via.render.RenderOutput runtime type"));
        return;
    }

    auto comp = sdk::call_object_func_easy<sdk::renderer::RenderOutput*>(camera_gameobject, "getComponent(System.Type)", runtime_type);

    if (comp == nullptr) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] Failed to get via.render.RenderOutput component"));
        return;
    }

    if (m_eye_states[0].scene_layer == nullptr) {
        return; // wait.
    }

    auto& scene_layers_output = comp->get_scene_layers();

    if (scene_layers_output.empty()) {
        return; // wait.
    }

    if (scene_layers.size() == 1 && scene_layers[0] == scene_layers_output[0]) {
        scene_layers_output.emplace();
        scene_layers_output.num = 0;
    } else if (scene_layers.size() == 2 && scene_layers_output.size() == 1) {
        scene_layers_output[0] = (sdk::renderer::layer::Scene*)scene_layers[0];
        if (scene_layers_output.num_allocated == 1) {
            scene_layers_output.emplace();
        }
        scene_layers_output[1] = (sdk::renderer::layer::Scene*)scene_layers[1];
        m_made_extra_scene_layer = true;

        REVR_LOG(spdlog::info("[TemporalUpscaler] Made extra scene layer"));

        if (!m_hooked_resource_release) {
            REVR_LOG(spdlog::info("[TemporalUpscaler] Hooking resource release"));
            m_render_resource_release_hook = std::make_unique<FunctionHook>(sdk::renderer::RenderResource::get_release_fn(), &render_resource_release_hook);
            m_render_resource_release_hook->create();
            m_hooked_resource_release = true;

            REVR_LOG(spdlog::info("[TemporalUpscaler] Hooked resource release"));
        }
    }
}

// [SMALL WINDOW fix122 22.11.2026] The eye image's size, given the backbuffer's. The VR mod makes the game's window as large as
// an eye image, so the two were the same; with the option "the game's window small" the window is an engine view's size
// and the eye image keeps the headset's (what the window had).
void TemporalUpscaler::eye_image_size(UINT64& w, UINT& h) const {
    auto vr = VR::get();

    if (quadview::State::get().small_window.load(std::memory_order_relaxed) && vr->is_hmd_active() && vr->is_using_multipass() &&
        vr->get_hmd_width() > 0 && vr->get_hmd_height() > 0) {
        w = vr->get_hmd_width();
        h = vr->get_hmd_height();
    }
}

// the desktop window can take the left eye's image (a plain copy) only while it has the eye image's size
bool TemporalUpscaler::mirror_fits(ID3D12Resource* backbuffer) const {
    if (backbuffer == nullptr) {
        return false;
    }

    const auto d = backbuffer->GetDesc();
    return d.Width == m_backbuffer_size[0] && d.Height == m_backbuffer_size[1];
}

uint32_t TemporalUpscaler::get_render_width() const {
    // [QUADVIEW] all views render at the quad-view size (multipass only, see the re-init check in on_early_present)
    if (m_qv_layout.valid && VR::get()->is_using_multipass()) {
        return m_qv_layout.render_w;
    }

    // [SMALL WINDOW fix122] between a device reset and the layout made again: the last layout's size (the window has it)
    if (quadview::State::get().small_window.load(std::memory_order_relaxed) && quadview::State::get().requested() && VR::get()->is_using_multipass() &&
        m_qv_window_w.load(std::memory_order_relaxed) != 0) {
        return m_qv_window_w.load(std::memory_order_relaxed);
    }

    // [NO PLUGIN fix91 19.11.2026] without the quad view: the eye image's size (the plugin's quality levels are gone)
    // we subtract 1 from the native res because
    // the game will create a separate color buffer we can use
    // otherwise it will be null.
    return m_backbuffer_size[0] > 1 ? m_backbuffer_size[0] - 1 : 1;
}

uint32_t TemporalUpscaler::get_render_height() const {
    if (m_qv_layout.valid && VR::get()->is_using_multipass()) { // [QUADVIEW]
        return m_qv_layout.render_h;
    }

    // [SMALL WINDOW fix122] (see get_render_width)
    if (quadview::State::get().small_window.load(std::memory_order_relaxed) && quadview::State::get().requested() && VR::get()->is_using_multipass() &&
        m_qv_window_h.load(std::memory_order_relaxed) != 0) {
        return m_qv_window_h.load(std::memory_order_relaxed);
    }

    return m_backbuffer_size[1] > 1 ? m_backbuffer_size[1] - 1 : 1; // [NO PLUGIN fix91]
}

void TemporalUpscaler::on_render_resource_release(sdk::renderer::RenderResource* resource) {
    std::scoped_lock _{m_queued_release_resources_mutex};

    if (resource == nullptr) {
        return;
    }

    //if (resource->m_ref_count == 1) {
        m_queued_release_resources.push_back(resource);
    /*} else {
        const auto original = m_render_resource_release_hook->get_original<decltype(render_resource_release_hook)>();
        original(resource);
    }*/
}

// [COPY LIFETIME 15.11.2026] see m_retired_copies
void TemporalUpscaler::retire_copy(sdk::intrusive_ptr<sdk::renderer::Texture>& copy) {
    if (copy.get() == nullptr) {
        return;
    }

    const uint64_t now = (uint64_t)(std::max)(VR::get()->get_render_frame_count(), 0);
    std::scoped_lock _{m_retired_copies_mtx};
    m_retired_copies.emplace_back(std::move(copy), now); // copy is empty afterwards
}

void TemporalUpscaler::release_retired_copies(bool all) {
    const uint64_t now = (uint64_t)(std::max)(VR::get()->get_render_frame_count(), 0);
    std::scoped_lock _{m_retired_copies_mtx};

    if (all) {
        m_retired_copies.clear();
        return;
    }

    std::erase_if(m_retired_copies, [now](const auto& e) { return now < e.second || e.second + 8 < now; });
}

void TemporalUpscaler::finish_release_resources() {
    if (!m_hooked_resource_release) {
        return;
    }

    std::scoped_lock _{m_queued_release_resources_mutex};

    if (!m_queued_release_resources.empty()) {
        const auto original = m_render_resource_release_hook->get_original<decltype(render_resource_release_hook)>();

        for (auto resource : m_queued_release_resources) {
            original(resource);
        }

        m_queued_release_resources.clear();
    }
}

void TemporalUpscaler::update_motion_scale() {
#if TDB_VER > 67
    m_motion_scale[0] = (float)get_render_width() / 2.0f;
    m_motion_scale[1] = -1.0f * ((float)get_render_height() / 2.0f);
#else
    // I have no idea. Would need to take a look at the texture in RenderDoc.
    // Might need a shader to fix this?
    m_motion_scale[0] = 0.01f;
    m_motion_scale[1] = -1.0f * ((float)get_render_height() / 2.0f);
#endif

    // ([NO PLUGIN fix91 19.11.2026] the scale was handed to every instance of the plugin here; the mod's passes take
    // m_motion_scale)
}

void TemporalUpscaler::render_resource_release_hook(sdk::renderer::RenderResource* resource) {
    TemporalUpscaler::get()->on_render_resource_release(resource);
}
// =================================================================================================
// [QUADVIEW] Quad-view foveated rendering (multipass): shared wide view + a fovea per eye, composite, interface
// =================================================================================================
bool TemporalUpscaler::init_quadview_features(uint32_t out_w, uint32_t out_h, uint32_t out_format) {
    auto& qv = quadview::State::get();
    const auto settings = qv.settings();

    // [MONO PERIPHERY 27.10.2026] the shared wide view covers both eyes' frusta, so the render target (one size for all
    // views) is wider by that factor at the same pixel density
    const bool mono = VR::get()->is_hmd_active() && VR::get()->is_mono_periphery();
    float wide_x_factor = 1.0f;

    if (mono) {
        VR::get()->publish_frusta();
        const auto fr = qv.frusta();

        if (fr.valid && fr.eye_r[0] - fr.eye_l[0] > 1e-4f) {
            wide_x_factor = (fr.wide_r - fr.wide_l) / (fr.eye_r[0] - fr.eye_l[0]);
        }
    }

    auto layout = quadview::compute_layout(out_w, out_h, settings.fraction, settings.fovea_ratio, wide_x_factor);

    if (!layout.valid) {
        set_qv_status("invalid layout");
        return false;
    }

    // [NO PLUGIN fix91 19.11.2026] No upscaler plugin. Until now the plugin's instances were created here: two for the
    // eyes, which never ran (the periphery is the mod's), and before fix91 two for the fovea; one upscale method for all,
    // and a fix-up header set the output size per instance. Nothing of it is left: the quad view is the mod's own passes,
    // made below and with their first frame.
    //
    // [FOVEA OWN fix91 19.11.2026] The fovea is always anti-aliased by the mod (its supersampling TAA, its TAA if that
    // fails). With the shared periphery the layout stays what the set-up of FSR 4 by the mod made of it until fix88, with
    // the values every tested configuration had (both foveas in one pass, the layout's own network, no supersampling):
    // the layout as computed, moved to the nearest one with a gap of whole pixels between the foveas ([ONE PASS FIT
    // 06.11.2026], quadview::fit_atlas), and the fovea views' jitter as below (m_qv_fovea_own).
    if (mono) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        const auto before = layout;
#endif

        if (quadview::fit_atlas(layout)) {
            REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: fovea {}x{} rendered at {}x{} -> {}x{} at {}x{} (the layout of both foveas in one pass, kept)",
                before.inset_w, before.inset_h, before.render_w, before.render_h, layout.inset_w, layout.inset_h, layout.render_w, layout.render_h));
        }
    }

    fsr_direct_release();
    m_qv_fovea_own = true;

    // With a wide view per eye (the comparison) the fovea had an instance of the plugin: its rectangle sized for the
    // plugin's quality step where the ratio is within 4 % of one, so that its view renders at the render size.
    if (quadview::QualityRatio fovea_q{}; !mono && quadview::snap_quality(layout.fovea_ratio, 0.04f, fovea_q)) {
        quadview::fit_fovea(layout, fovea_q);
    } else {
        layout.fovea_quality = quadview::quality_for_ratio(layout.fovea_ratio).quality;
    }

    layout.wide_quality = quadview::PluginQuality::Native;

    // (the fovea views' jitter when the mod's TAA runs without its supersampling. Shared periphery: what FSR 4 by the mod
    // left, the sequence length from the layout and the index the engine's frame when both foveas had fitted in one pass.
    // A wide view per eye: the length for the fovea's own scale, counted per view, as the plugin's instance gave it.)
    m_qv_fovea_frame_jitter = mono && quadview::fsr_atlas(layout).valid;
    m_qv_fovea_jitter_w = mono ? (std::max)(layout.inset_w, layout.render_w) : 0u;
    REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: no upscaler set up for the fovea (the mod's anti-aliasing), jitter index {}",
        m_qv_fovea_frame_jitter ? "the engine's frame" : "counted per view"));

    m_qv_fovea_float_out = true; // (as without an instance of the plugin for the fovea; only the branches of FSR 4 by the mod read it)
    m_fovea_linear_off = false;

    // ([PERIPHERY SUPER fix80] what anti-aliases the periphery as the settings ask now; it can change without a set-up)
    // ([CALM fix100 20.11.2026] 3 = the calm light TAA: the light TAA's pass, nothing to set up here)
    const bool periphery_calm = mono && m_is_d3d12 && settings.periphery_aa == 3;
    const int periphery_aa = mono && m_is_d3d12 && !periphery_calm ? std::clamp(settings.periphery_aa, 0, 2) : 0;
    REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: eye {}x{}, fovea {}x{}, render {}x{} (x{} views{}), periphery {} + spatial x{:.2f}, "
                 "fovea x{:.2f} (the mod's anti-aliasing, no upscaler plugin)",
        out_w, out_h, layout.inset_w, layout.inset_h, layout.render_w, layout.render_h, mono ? 3 : 4,
        mono ? std::format(", shared wide view x{:.3f} wider, eye share {}x{}", layout.wide_x_factor, layout.eye_wide_w, layout.eye_wide_h) : std::string{},
        periphery_aa == 2 ? "supersampling TAA (image of twice the size)" : periphery_aa == 1 ? "supersampling TAA"
        : periphery_calm ? "calm light TAA" : "light TAA", layout.wide_ratio,
        layout.fovea_ratio));

    for (auto& pre : m_qv_pre_copiers) {
        pre.setup(L"Tri-view pre-fovea copier");
    }

    // [TAA READY 06.11.2026] the mod's fovea TAA made now, while the set-up holds the frame anyway: the guards switch the
    // fovea to it in a fight (made on that first switch, the frame took 0.56 s in the log of 06.11 15:02)
    if (m_is_d3d12 && settings.fovea_mode == quadview::FoveaMode::Upscaler &&
        !m_fovea_taa.prepare(g_framework->get_d3d12_hook()->get_device(), layout.render_w, layout.render_h)) {
        REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: the fovea TAA could not be made ahead ({}), it is made on its first use",
            m_fovea_taa.last_error()));
    }

    // [SUPER TAA 16.11.2026] the supersampling TAA made now too when it is the fovea's anti-aliasing (its four shaders are
    // compiled once per game session, its images made for this size); chosen later in the menu it is made on its first
    // frame. A failure here is not the end: the pass tries again on its first frame and the mod's TAA takes over if it fails.
    m_super_taa_failed = false;
    // [UI MOTION fix125] its analyze pass with the interface's own motion made with the others when the option is on
    m_super_taa.set_ui_motion(settings.ui_motion);

    if (m_is_d3d12 && settings.fovea_mode == quadview::FoveaMode::TAA && settings.fovea_super &&
        !m_super_taa.prepare(g_framework->get_d3d12_hook()->get_device(), layout.render_w, layout.render_h, layout.inset_w, layout.inset_h,
            settings.super_taa.scale >= 2 ? 2u : 1u)) {
        REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: the supersampling TAA could not be made ahead ({}), it is made on its first use",
            m_super_taa.last_error()));
    }

    // [PERIPHERY SUPER fix80 17.11.2026] the same for the periphery's instance (one view) and, for its image of twice the
    // wide view's size, the reprojection's passes that write 2 x 2 pixels per ray. Failures here are not the end either:
    // the pass tries again on its first frame, and the light TAA runs where it cannot.
    m_super_wide_failed = false;
    m_qv_calm_failed = false; // [CALM fix100]
    m_super_taa_wide.set_slots(1);

    if (periphery_aa > 0) {
        auto* device = g_framework->get_d3d12_hook()->get_device();
        // (the image of twice the size only where the eye pass can take a periphery that dense, see quadview::periphery_2x_fits)
        const bool fits_2x = quadview::periphery_2x_fits(layout);
        const uint32_t ss = periphery_aa == 2 && fits_2x && m_mono_periphery.ss_ready(device) ? 2u : 1u;

        if (periphery_aa == 2 && ss != 2) {
            REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: {}, the supersampling TAA writes the periphery at the wide view's size",
                fits_2x ? "the reprojection's passes for the periphery image of twice the size could not be made"
                        : "the eye image is less than 3 times its share of the wide view per side (no periphery image of twice the size)"));
        }

        // (the eye pass for a periphery that dense: compiled now, once per game session, not in its first frame)
        if (ss == 2 && !m_eye_compose.prepare_dense(device)) {
            REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: the eye pass for the periphery image of twice the size could not be made ahead ({})",
                m_eye_compose.last_error()));
        }

        if (!m_super_taa_wide.prepare(device, layout.render_w, layout.render_h, layout.render_w * ss, layout.render_h * ss, ss == 2 ? 1u : 2u)) {
            REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: the periphery's supersampling TAA could not be made ahead ({}), it is made on its first use",
                m_super_taa_wide.last_error()));
        }
    }

    // [GUARD fix101 20.11.2026] everything the GPU guard's steps switch to, made now: the light TAA's passes and histories,
    // the calm variant's second history, the supersampling TAA's image of the wide view's size and each eye's periphery
    // of that size. (The eye pass and the reprojection for it are their first passes, made on their first frame.)
    if (mono && m_is_d3d12 && settings.guard_warm) {
        auto* device = g_framework->get_d3d12_hook()->get_device();
        constexpr auto F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bool ok = m_light_taa.prepare(device) && ensure_qv_taa(device, layout.render_w, layout.render_h);
        ok = ensure_uav_image(device, m_qv_calm_aux[0], layout.render_w, layout.render_h, F16, L"RE4VR shared wide view, calm TAA statistics 0") && ok;
        ok = ensure_uav_image(device, m_qv_calm_aux[1], layout.render_w, layout.render_h, F16, L"RE4VR shared wide view, calm TAA statistics 1") && ok;
        ok = ensure_uav_image(device, m_qv_wide_super[0], layout.render_w, layout.render_h, F16, L"RE4VR shared wide view, supersampling TAA") && ok;
        ok = ensure_uav_image(device, m_qv_mono_eye_wide[0], layout.eye_wide_w, layout.eye_wide_h, F16, L"RE4VR mono periphery left") && ok;
        ok = ensure_uav_image(device, m_qv_mono_eye_wide[1], layout.eye_wide_w, layout.eye_wide_h, F16, L"RE4VR mono periphery right") && ok;

        if (!ok) {
            REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: not everything for the GPU guard's steps could be made ahead (made on its first use)"));
        }
    }

    m_qv_layout = layout;
    m_qv_window_w.store(layout.render_w, std::memory_order_relaxed); // [SMALL WINDOW fix122] (kept over resets)
    m_qv_window_h.store(layout.render_h, std::memory_order_relaxed);
    m_qv_stereo = true;  // both eyes every frame
    m_qv_mono = mono;    // [MONO PERIPHERY] 3 scene layers, one shared wide view
    m_qv_fovea_clean_psr = {false, false};
    qv.set_layout(layout, m_qv_init_generation);

    set_qv_status(std::format("active: render {}x{} x{}, fovea {}x{}", layout.render_w, layout.render_h, m_qv_mono ? 3 : 4, layout.inset_w, layout.inset_h));

    REVR_LOG(spdlog::info("[TemporalUpscaler] Eye image size: {}x{}", out_w, out_h));
    REVR_LOG(spdlog::info("[TemporalUpscaler] Tri-view set up (no upscaler plugin)"));
    return true;
}

void TemporalUpscaler::release_quadview_features() {
    // ([NO PLUGIN fix91 19.11.2026] the plugin's fovea instances were released here)
    if (m_qv_layout.valid) {
        m_qv_layout = quadview::Layout{};
        quadview::State::get().clear_layout();
    }

    for (auto& pre : m_qv_pre_copiers) {
        if (pre.cmd_list != nullptr) {
            pre.reset();
        }
    }

    // [FSR DIRECT 06.11.2026] after the pre-fovea lists (FSR 4's passes are on them) are done
    fsr_direct_release();

    for (auto& t : m_qv_wide_safe) {
        t.Reset();
    }

    for (auto& t : m_qv_eye_image) {
        t.Reset();
    }

    m_qv_post_tex = {}; // [POST IN UI] the images it pointed at are gone
    m_qv_eye_fresh = {false, false};
    m_qv_ftaa_frame = {-10, -10};
    m_qv_taa_frame = {-10, -10};
    m_qv_stereo = false;
    m_qv_mono = false;
    m_qv_dump_pre_ctx = nullptr;
    m_fovea_composite.reset();
    m_wide_upscale.reset();
    m_light_taa.reset();
    m_fovea_taa.reset();
    // [SUPER TAA 16.11.2026] the supersampling TAA with it; a capture or a probe under way ends without files
    m_super_taa.reset();
    m_super_taa_failed = false;
    m_qv_staa_frame = {-10, -10};

    for (auto& t : m_qv_fovea_cmp) {
        t.Reset();
    }

    // [PERIPHERY SUPER fix80 17.11.2026] the periphery's instance and its images
    m_super_taa_wide.reset();
    m_super_wide_failed = false;
    m_qv_wide_staa_frame = -10;
    m_qv_wide_staa_ss = 0;
    m_periphery_ss = 1;
    m_periphery_linear = false;
    m_periphery_running.store(0, std::memory_order_relaxed);
    m_periphery_why.store(PW_NONE, std::memory_order_relaxed);
    m_periphery_logged = ~0u;

    for (auto& t : m_qv_wide_super) {
        t.Reset();
    }

    for (auto& t : m_qv_mono_eye_wide2) {
        t.Reset();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] captures and the engine probe: debug build only
    if (m_sc.busy()) {
        REVR_LOG(spdlog::info("[TemporalUpscaler] supersampling TAA capture ended by the tri-view's set-up ({} frames recorded, not written)",
            m_sc.frames_recorded()));
    }

    m_sc.cancel();
    quadview::State::get().super_capture_running = false;

    if (m_probe.state.load() != 0) {
        std::scoped_lock _{m_probe.mtx};

        for (auto& c : m_probe.copies) {
            retire_copy(c.copy);
        }

        m_probe.copies.clear();
        m_probe.dump.clear();
        m_probe.report.clear();
        m_probe.layer = nullptr;
        m_probe.written_pending = false;
        m_probe.state = 0;
    }
#endif

    m_fovea_linear.reset(); // [FOVEA LINEAR 05.11.2026]
    m_qv_fovea_lin = {};
    m_qv_fovea_lin_psr = {false, false};
    m_qv_fovea_float_out = false;
    m_fovea_linear_off = false;
    m_qv_fovea_own = false; // [FOVEA OWN fix91 19.11.2026]
    m_qv_fovea_frame_jitter = false;
    m_qv_fovea_jitter_w = 0;
    m_mono_periphery.reset();
    m_stable_depth.reset(); // [STABLE DEPTH fix81]
    m_stable_depth_frame = -10;
    m_stable_depth_failed = false;
    m_ui_steady.reset(); // [MENUS fix86]
    m_ui_motion.reset(); // [UI MOTION fix125]
    m_ui_steady_frame = -10;
    m_ui_steady_failed = false;
    m_stable_depth_used = false;
    m_ui_clean.reset();
    m_ui_curve.reset();
    m_mono_ui.reset();
    m_eye_compose.reset();
    m_gui_map.reset();
    m_eye_compose_logged = false;
    m_qv_mirror_src = nullptr;
    m_qv_wide_clean.Reset();
    m_qv_wide_clean_psr = false;

    for (auto& t : m_qv_ui_out) {
        t.Reset();
    }

    for (auto& t : m_qv_mono_eye_wide) {
        t.Reset();
    }

    for (auto& t : m_qv_fovea_clean) {
        t.Reset();
    }

    m_qv_fovea_clean_psr = {false, false};

    for (auto& t : m_qv_fovea_safe) {
        t.Reset();
    }

    for (auto& t : m_qv_fovea_aa) {
        t.Reset();
    }

    for (auto& eye : m_qv_taa_hist) {
        for (auto& t : eye) {
            t.Reset();
        }
    }

    m_qv_taa_valid = {false, false};
    set_qv_status("off");
}

bool TemporalUpscaler::ensure_qv_taa(ID3D12Device* device, uint32_t w, uint32_t h) {
    return ensure_taa_history(device, m_qv_taa_hist, m_qv_taa_valid, w, h, L"RE4VR tri-view periphery TAA history");
}

// per eye slot two render-size histories (R16G16B16A16_FLOAT, rest in UNORDERED_ACCESS); recreated on a size change
bool TemporalUpscaler::ensure_taa_history(ID3D12Device* device, std::array<std::array<ComPtr<ID3D12Resource>, 2>, 2>& hist,
    std::array<bool, 2>& valid, uint32_t w, uint32_t h, const wchar_t* name) {
    if (device == nullptr || w == 0 || h == 0) {
        return false;
    }

    if (hist[0][0] != nullptr) {
        const auto have = hist[0][0]->GetDesc();

        if (have.Width == w && have.Height == h) {
            return true;
        }
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = w;
    tex.Height = h;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (auto& eye : hist) {
        for (auto& t : eye) {
            t.Reset();

            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&t)))) {
                REVR_LOG(spdlog::error("[TemporalUpscaler] tri-view: could not create a TAA history ({}x{})", w, h));

                for (auto& e2 : hist) {
                    for (auto& t2 : e2) {
                        t2.Reset();
                    }
                }

                return false;
            }

            t->SetName(name);
        }
    }

    valid = {false, false};
    REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: {} {}x{}", utility::narrow(std::wstring_view{name}), w, h));
    return true;
}

bool TemporalUpscaler::ensure_qv_wide_safe(ID3D12Device* device, uint32_t slot, uint32_t w, uint32_t h, DXGI_FORMAT format) {
    slot = slot < 2 ? slot : 1;
    return ensure_uav_image(device, m_qv_wide_safe[slot], w, h, format, slot == 0 ? L"RE4VR tri-view eye image 0" : L"RE4VR tri-view eye image 1");
}

// image of our own in the UAV-able variant of `format` (same format family -> CopyResource works both ways); rests in
// UNORDERED_ACCESS like the plugin outputs; recreated when size/format change
bool TemporalUpscaler::ensure_uav_image(ID3D12Device* device, ComPtr<ID3D12Resource>& target, uint32_t w, uint32_t h, DXGI_FORMAT format,
    const wchar_t* name, bool* created) {
    if (created != nullptr) {
        *created = false;
    }

    if (device == nullptr || w == 0 || h == 0) {
        return false;
    }

    const auto uav_format = d3d12::eye_uav_format(format);

    if (uav_format == DXGI_FORMAT_UNKNOWN) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] tri-view: eye image format {} cannot be written by a compute shader", (int)format));
        return false;
    }

    if (target != nullptr) {
        const auto have = target->GetDesc();

        if (have.Width == w && have.Height == h && have.Format == uav_format) {
            return true;
        }

        target.Reset();
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = w;
    tex.Height = h;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = uav_format;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // [STEREO FOVEA 27.10.2026] also a render target: in multipass the finished eye image goes through the same path as
    // the plugin's upscaled texture (D3D12Component makes an RTV + SRV of it for the 10-bit -> 8-bit conversion).
    // Crash log of 27.10.2026: RTV made on a UAV-only image, then the next descriptor heap could not be created.
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&target)))) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] tri-view: could not create {} ({}x{} format {})", utility::narrow(std::wstring_view{name}), w, h, (int)uav_format));
        target.Reset();
        return false;
    }

    target->SetName(name);
    REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: {} {}x{} format {}", utility::narrow(std::wstring_view{name}), w, h, (int)uav_format));

    if (created != nullptr) {
        *created = true;
    }

    return true;
}

// ---- [FSR DIRECT 06.11.2026] FSR 4 in the fovea called by the mod (see TemporalUpscaler.hpp, d3d12::FsrDirect) ------------
bool TemporalUpscaler::fsr_direct_create(ID3D12Device* device, const quadview::Layout& layout, const quadview::Settings& settings) {
    fsr_direct_release();

    if (device == nullptr || !layout.valid) {
        return false;
    }

    const auto atlas = settings.fsr_one_pass ? quadview::fsr_atlas(layout) : quadview::FsrAtlas{};

    if (settings.fsr_one_pass && !atlas.valid) {
        REVR_LOG(spdlog::warn("[TemporalUpscaler] tri-view: FSR 4 by the mod: no gap of whole pixels for the fovea {}x{} from {}x{}, a pass per eye",
            layout.inset_w, layout.inset_h, layout.render_w, layout.render_h));
    }

    d3d12::FsrDirect::Create c{};
    c.auto_exposure = settings.fsr_auto_exposure;
    bool ok = true;

    // [FSR MODEL 09.11.2026] another network: FSR 4's output this much larger than the fovea (scaled down after it).
    // [SUPERSAMPLE 10.11.2026] per axis (the render width and height are rounded on their own), never below the input
    const float fx = quadview::fsr_output_factor(settings.fsr_model, layout.render_w, layout.inset_w);
    const float fy = quadview::fsr_output_factor(settings.fsr_model, layout.render_h, layout.inset_h);
    const float factor = (std::max)(fx, fy);
    const auto grow_x = [fx](uint32_t v) { return fx > 1.0f ? (uint32_t)std::lround((double)v * fx) : v; };
    const auto grow_y = [fy](uint32_t v) { return fy > 1.0f ? (uint32_t)std::lround((double)v * fy) : v; };

    if (atlas.valid) {
        c.render_w = atlas.render_w;
        c.render_h = atlas.render_h;
        c.out_w = (std::max)(grow_x(atlas.out_w), atlas.render_w);
        c.out_h = (std::max)(grow_y(atlas.out_h), atlas.render_h);
        ok = m_fsr[0].create(device, c) &&
             ensure_uav_image(device, m_qv_fsr_out[0], atlas.out_w, atlas.out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR FSR 4 result, both foveas") &&
             (factor <= 1.0f ||
                 ensure_uav_image(device, m_qv_fsr_big[0], c.out_w, c.out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR FSR 4 larger result, both foveas")) &&
             ensure_uav_image(device, m_qv_fsr_color, atlas.render_w, atlas.render_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RE4VR FSR 4 colour, both foveas") &&
             ensure_uav_image(device, m_qv_fsr_depth, atlas.render_w, atlas.render_h, DXGI_FORMAT_R32_FLOAT, L"RE4VR FSR 4 depth, both foveas") &&
             ensure_uav_image(device, m_qv_fsr_motion, atlas.render_w, atlas.render_h, DXGI_FORMAT_R32G32_FLOAT, L"RE4VR FSR 4 motion, both foveas");
    } else {
        c.render_w = layout.render_w;
        c.render_h = layout.render_h;
        c.out_w = (std::max)(grow_x(layout.inset_w), layout.render_w);
        c.out_h = (std::max)(grow_y(layout.inset_h), layout.render_h);

        for (uint32_t i = 0; ok && i < 2; ++i) {
            ok = m_fsr[i].create(device, c) &&
                 ensure_uav_image(device, m_qv_fsr_out[i], layout.inset_w, layout.inset_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                     i == 0 ? L"RE4VR FSR 4 result left" : L"RE4VR FSR 4 result right") &&
                 (factor <= 1.0f || ensure_uav_image(device, m_qv_fsr_big[i], c.out_w, c.out_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                        i == 0 ? L"RE4VR FSR 4 larger result left" : L"RE4VR FSR 4 larger result right")) &&
                 ensure_uav_image(device, m_qv_fsr_depth_eye[i], layout.render_w, layout.render_h, DXGI_FORMAT_R32_FLOAT,
                     i == 0 ? L"RE4VR FSR 4 depth left" : L"RE4VR FSR 4 depth right") &&
                 ensure_uav_image(device, m_qv_fsr_motion_eye[i], layout.render_w, layout.render_h, DXGI_FORMAT_R32G32_FLOAT,
                     i == 0 ? L"RE4VR FSR 4 motion left" : L"RE4VR FSR 4 motion right");
        }
    }

    if (!ok) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        std::string why = !m_fsr[0].last_error().empty() ? m_fsr[0].last_error() : m_fsr[1].last_error();
        REVR_LOG(spdlog::error("[TemporalUpscaler] tri-view: FSR 4 by the mod could not be set up ({}), the upscaler plugin is used",
            why.empty() ? std::string{"an image could not be created"} : why));
#endif
        fsr_direct_release();
        return false;
    }

    m_qv_fsr = true;
    m_qv_fsr_atlas = atlas;
    m_qv_fsr_in_read = false;
    m_fsr_last = {};
    m_qv_fsr_factor = factor;
    m_qv_fsr_big_w = c.out_w;
    m_qv_fsr_big_h = c.out_h;
    m_qv_fsr_jitter_w = (std::max)(grow_x(layout.inset_w), layout.render_w);
    quadview::State::get().fovea_upscaler_kind.store(atlas.valid ? 2u : 1u, std::memory_order_relaxed);
    REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view: FSR 4 by the mod ({}, functions from {}): {}, automatic exposure {}, network option {} (scale {:.3f}{})",
        m_fsr[0].provider(), d3d12::FsrDirect::api_source(),
        atlas.valid ? std::format("both foveas in one pass, {}x{} -> {}x{} (gap {} / {} px)", atlas.render_w, atlas.render_h, atlas.out_w, atlas.out_h,
                          atlas.gap_r, atlas.gap_o)
                    : std::format("a pass per eye, {}x{} -> {}x{}", layout.render_w, layout.render_h, layout.inset_w, layout.inset_h),
        c.auto_exposure, settings.fsr_model, (double)c.out_w / (double)(std::max)(c.render_w, 1u),
        factor > 1.0f ? std::format(", FSR 4 writes {}x{}, scaled down by {:.3f}", c.out_w, c.out_h, factor) : std::string{}));
    return true;
}

void TemporalUpscaler::fsr_direct_release() {
    for (auto& f : m_fsr) {
        f.destroy();
    }

    m_qv_fsr_out = {};
    m_qv_fsr_big = {};
    m_qv_fsr_factor = 1.0f;
    m_qv_fsr_big_w = 0;
    m_qv_fsr_big_h = 0;
    m_qv_fsr_jitter_w = 0;
    m_qv_fsr_color.Reset();
    m_qv_fsr_depth.Reset();
    m_qv_fsr_motion.Reset();
    m_qv_fsr_depth_eye = {};
    m_qv_fsr_motion_eye = {};
    m_qv_fsr = false;
    m_qv_fsr_atlas = quadview::FsrAtlas{};
    m_qv_fsr_in_read = false;
    quadview::State::get().fovea_upscaler_kind.store(0, std::memory_order_relaxed);
}

bool TemporalUpscaler::fsr_direct_record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, int32_t frame, const std::array<ID3D12Resource*, 2>& color,
    const std::array<ID3D12Resource*, 2>& depth, const std::array<ID3D12Resource*, 2>& motion, float fov_y, bool dump_arg) {
#if REVR_RELEASE
    constexpr bool dump = false; // [RELEASE SWITCH fix141] no dumps in the release: the compiler leaves their branches out
    (void)dump_arg;
#else
    const bool dump = dump_arg;
#endif
    constexpr auto ENGINE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // what FSR 4 reads its inputs in (AMD: NON_PIXEL_SHADER_RESOURCE; the engine copies keep their pixel shader bit)
    constexpr auto READ = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto& qv = quadview::State::get();
    auto& spans = d3d12::GpuSpans::get();
    const auto& lay = m_qv_layout;
    const auto& atlas = m_qv_fsr_atlas;

    if (!m_qv_fsr || cmd == nullptr || color[0] == nullptr || color[1] == nullptr || depth[0] == nullptr || depth[1] == nullptr || motion[0] == nullptr ||
        motion[1] == nullptr) {
        return false;
    }

    std::vector<D3D12_RESOURCE_BARRIER> barriers{};
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        barriers.push_back(b);
    };

    // ---- the inputs: depth and motion of both views side by side, or each eye's own copied (the engine's motion vectors
    //      are R16G16B16A16_SNORM, a format AMD's API has no name for; the colour went in with the decode, step 3b) -----
    if (atlas.valid) {
        for (int eye = 0; eye < 2; ++eye) {
            const auto f = quadview::atlas_fill(atlas, lay.render_w, eye);
            d3d12::FoveaLinear::Region r{};
            r.out_x = f.out_x;
            r.w = f.w;
            r.h = lay.render_h;
            r.shift_x = f.shift;
            r.hi_x = (int32_t)lay.render_w - 1;
            r.hi_y = (int32_t)lay.render_h - 1;

            if (!m_fovea_linear.copy_part(device, cmd, depth[eye], ENGINE_STATE, m_qv_fsr_depth.Get(), r, true) ||
                !m_fovea_linear.copy_part(device, cmd, motion[eye], ENGINE_STATE, m_qv_fsr_motion.Get(), r, false)) {
                REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod: depth / motion side by side failed ({})", m_fovea_linear.last_error()));
                return false;
            }
        }

        transition(m_qv_fsr_color.Get(), UAV, READ);
        transition(m_qv_fsr_depth.Get(), UAV, READ);
        transition(m_qv_fsr_motion.Get(), UAV, READ);
    } else {
        for (int eye = 0; eye < 2; ++eye) {
            d3d12::FoveaLinear::Region r{};
            r.w = lay.render_w;
            r.h = lay.render_h;
            r.hi_x = (int32_t)lay.render_w - 1;
            r.hi_y = (int32_t)lay.render_h - 1;

            if (m_qv_fsr_depth_eye[eye] == nullptr || m_qv_fsr_motion_eye[eye] == nullptr ||
                !m_fovea_linear.copy_part(device, cmd, depth[eye], ENGINE_STATE, m_qv_fsr_depth_eye[eye].Get(), r, true) ||
                !m_fovea_linear.copy_part(device, cmd, motion[eye], ENGINE_STATE, m_qv_fsr_motion_eye[eye].Get(), r, false)) {
                REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod: depth / motion copies failed ({})", m_fovea_linear.last_error()));
                return false;
            }

            transition(color[eye], UAV, READ);
            transition(m_qv_fsr_depth_eye[eye].Get(), UAV, READ);
            transition(m_qv_fsr_motion_eye[eye].Get(), UAV, READ);
        }
    }

    cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());
    barriers.clear();
    m_qv_fsr_in_read = true;

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the jitter check: debug build only
    // [JITTER CHECK 10.11.2026] the centre of the left fovea in FSR 4's inputs, as stored
    const int jc = m_jc.next;

    if (jc >= 0 && jc < JitterCheck::FRAMES) {
        auto& d = m_jc.dumps[jc];
        const uint32_t P = JitterCheck::PART;
        const uint32_t px = lay.render_w > P ? (lay.render_w - P) / 2 : 0;
        const uint32_t py = lay.render_h > P ? (lay.render_h - P) / 2 : 0;
        ID3D12Resource* in_c = atlas.valid ? m_qv_fsr_color.Get() : color[0];
        ID3D12Resource* in_d = atlas.valid ? m_qv_fsr_depth.Get() : m_qv_fsr_depth_eye[0].Get();
        ID3D12Resource* in_m = atlas.valid ? m_qv_fsr_motion.Get() : m_qv_fsr_motion_eye[0].Get();

        d.add(device, cmd, in_c, READ, "fsr4_input_color");
        d.add(device, cmd, in_d, READ, "fsr4_input_depth");
        d.add(device, cmd, in_m, READ, "fsr4_input_motion");
        d.add_raw("fsr4_input_color", px, py, P, P);
        d.add_raw("fsr4_input_depth", px, py, P, P);
        d.add_raw("fsr4_input_motion", px, py, P, P);
    }
#endif

    if (dump && atlas.valid) {
        m_qv_dump.add(device, cmd, m_qv_fsr_color.Get(), READ, "C_2a_fsr4_input_color_both_foveas");
        m_qv_dump.add(device, cmd, m_qv_fsr_depth.Get(), READ, "C_2a_fsr4_input_depth_both_foveas");
        m_qv_dump.add(device, cmd, m_qv_fsr_motion.Get(), READ, "C_2a_fsr4_input_motion_both_foveas");
    }

    // ---- the pass(es) ---------------------------------------------------------------------------------------------------
    d3d12::FsrDirect::Input in{};
    in.motion_scale_x = m_motion_scale[0];
    in.motion_scale_y = m_motion_scale[1];
    in.near_plane = m_nearz;
    in.fov_y = fov_y;
    // [GPU GUARD 05.11.2026] the first pass after the mod's TAA starts again
    in.reset = m_qv_plugin_reset[0] || m_qv_plugin_reset[1];
    m_qv_plugin_reset = {false, false};

    const auto now = std::chrono::steady_clock::now();
    in.frame_ms = m_fsr_last == std::chrono::steady_clock::time_point{} ? 11.1f
                                                                          : std::clamp(std::chrono::duration<float, std::milli>(now - m_fsr_last).count(), 1.0f, 100.0f);
    m_fsr_last = now;

    spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_FSR_BEGIN);
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    uint64_t passes = 0;
    bool jitter_newer = false; // [JITTER LATCH 10.11.2026] the views' latest jitter is already the next frame's
    // [FSR MODEL 09.11.2026] FSR 4 into the larger image(s), then scaled down to the fovea's size
    const bool big = m_qv_fsr_factor > 1.0f && m_qv_fsr_big[0] != nullptr && (atlas.valid || m_qv_fsr_big[1] != nullptr);

    if (atlas.valid) {
        // the same jitter in both views (one index for both, see the jitter in on_scene_layer_update)
        // [JITTER LATCH 10.11.2026] the jitter this frame's fovea views were rendered with
        bool newer = false;
        const auto j = jitter_of_frame(get_qv_fovea_id(0) - 1, (int64_t)(uint32_t)frame, &newer);
        in.jitter_x = j[0];
        in.jitter_y = j[1];
        jitter_newer = newer;
        in.color = m_qv_fsr_color.Get();
        in.depth = m_qv_fsr_depth.Get();
        in.motion = m_qv_fsr_motion.Get();
        in.output = big ? m_qv_fsr_big[0].Get() : m_qv_fsr_out[0].Get();
        in.render_w = atlas.render_w;
        in.render_h = atlas.render_h;
        in.out_w = big ? m_qv_fsr_big_w : atlas.out_w;
        in.out_h = big ? m_qv_fsr_big_h : atlas.out_h;
        ok = m_fsr[0].dispatch(cmd, in);
        passes = 1;
    } else {
        for (uint32_t eye = 0; ok && eye < 2; ++eye) {
            bool newer = false; // [JITTER LATCH 10.11.2026]
            const auto j = jitter_of_frame(get_qv_fovea_id(eye) - 1, (int64_t)(uint32_t)frame, &newer);
            in.jitter_x = j[0];
            in.jitter_y = j[1];
            jitter_newer = jitter_newer || newer;
            in.color = color[eye];
            in.depth = m_qv_fsr_depth_eye[eye].Get();
            in.motion = m_qv_fsr_motion_eye[eye].Get();
            in.output = big ? m_qv_fsr_big[eye].Get() : m_qv_fsr_out[eye].Get();
            in.render_w = lay.render_w;
            in.render_h = lay.render_h;
            in.out_w = big ? m_qv_fsr_big_w : lay.inset_w;
            in.out_h = big ? m_qv_fsr_big_h : lay.inset_h;
            ok = m_fsr[eye].dispatch(cmd, in);
            ++passes;
        }
    }

    // [FSR MODEL 09.11.2026] the larger result scaled down into the fovea-sized one(s), whole images (the gap between the
    // foveas side by side scales the same way): output pixel x covers [x, x + 1) * big / out of the larger image
    for (uint32_t i = 0; ok && big && i < (atlas.valid ? 1u : 2u); ++i) {
        const uint32_t ow = atlas.valid ? atlas.out_w : lay.inset_w;
        const uint32_t oh = atlas.valid ? atlas.out_h : lay.inset_h;
        d3d12::FoveaLinear::Region r{};
        r.w = ow;
        r.h = oh;
        r.scale_x = (float)m_qv_fsr_big_w / (float)ow;
        r.scale_y = (float)m_qv_fsr_big_h / (float)oh;

        if (!m_fovea_linear.downsample_part(device, cmd, m_qv_fsr_big[i].Get(), m_qv_fsr_out[i].Get(), r)) {
            m_fsr[0].set_error(std::string{"scaling down FSR 4's larger result failed: "} + m_fovea_linear.last_error());
            ok = false;
        }
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the jitter check: debug build only
    // [JITTER CHECK 10.11.2026] FSR 4's result at the same place (fovea size), and what FSR 4 was given this frame
    if (jc >= 0 && jc < JitterCheck::FRAMES) {
        auto& d = m_jc.dumps[jc];
        const uint32_t P = JitterCheck::PART;
        const uint32_t ox = lay.inset_w > P ? (lay.inset_w - P) / 2 : 0;
        const uint32_t oy = lay.inset_h > P ? (lay.inset_h - P) / 2 : 0;

        if (ok && m_qv_fsr_out[0] != nullptr) {
            d.add(device, cmd, m_qv_fsr_out[0].Get(), UAV, "fsr4_result");
            d.add_raw("fsr4_result", ox, oy, P, P);
        }

        jitter_check_line(std::format("FSR 4 frame {} (check frame {}) | recorded {} | jitter given ({:+.4f}, {:+.4f}) px (this frame's; latest ({:+.4f}, {:+.4f}), "
                                      "the next frame's already: {}) | index {} | motion scale ({:.1f}, {:.1f}) | "
                                      "near {:.4f} | fov y {:.4f} | frame {:.2f} ms | reset {} | input part at {},{} result part at {},{} ({} px)",
            frame, jc, ok, in.jitter_x, in.jitter_y, m_jitter_offsets[get_qv_fovea_id(0) - 1][0], m_jitter_offsets[get_qv_fovea_id(0) - 1][1], jitter_newer,
            m_jitter_indices[get_qv_fovea_id(0) - 1], in.motion_scale_x, in.motion_scale_y, in.near_plane, in.fov_y,
            in.frame_ms, in.reset, lay.render_w > P ? (lay.render_w - P) / 2 : 0, lay.render_h > P ? (lay.render_h - P) / 2 : 0, ox, oy, P));
        m_jc.captured = true;
        m_jc.next = jc + 1 < JitterCheck::FRAMES ? jc + 1 : -1;
        m_jc.write = jc + 1 >= JitterCheck::FRAMES;
    }
#endif

    if (jitter_newer) {
        qv.jitter_newer.fetch_add(1, std::memory_order_relaxed); // [JITTER LATCH 10.11.2026]
    }

    qv.fsr_dispatch_ns.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
    qv.fsr_dispatches.fetch_add(passes, std::memory_order_relaxed);

    spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_FSR_END);
    spans.resolve(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_FSR_BEGIN, 2);

    // (the mod's own inputs stay in the read state until the next frame's writes)
    if (!ok) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod: {}", !m_fsr[0].last_error().empty() ? m_fsr[0].last_error() : m_fsr[1].last_error()));
    }

    return ok;
}

bool TemporalUpscaler::evaluate_quadview(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame) {
    auto& qv = quadview::State::get();

    // [GPU BREAKDOWN] the copier's work of this frame (periphery, fovea, eye images, desktop copy) between two stamps,
    // with one after each eye's fovea and eye image in between
    auto& spans = d3d12::GpuSpans::get();
    spans.collect((uint64_t)frame);
    spans.stamp(copier.cmd_list.Get(), (uint64_t)frame, d3d12::GpuSpans::COPIER_BEGIN);
    copier.has_commands |= spans.enabled();

    utility::ScopeGuard copier_end([&]() {
        spans.stamp(copier.cmd_list.Get(), (uint64_t)frame, d3d12::GpuSpans::COPIER_END);
        spans.resolve(copier.cmd_list.Get(), (uint64_t)frame, d3d12::GpuSpans::COPIER_BEGIN, d3d12::GpuSpans::COPIER_END - d3d12::GpuSpans::COPIER_BEGIN + 1);
    });

    m_qv_frame = (int64_t)frame;
    m_qv_mirror_src = nullptr;

    // [SUPER TAA 16.11.2026] for the next frame's jitter (on_scene_layer_update): is the supersampling TAA the fovea's
    // anti-aliasing, and its history's scale
    {
        const auto qs = qv.settings();
        m_super_scale_now.store(qs.fovea_mode == quadview::FoveaMode::TAA && qs.fovea_super && !m_super_taa_failed ? (qs.super_taa.scale >= 2 ? 2u : 1u) : 0u,
            std::memory_order_relaxed);
        // [PERIPHERY SUPER fix80 17.11.2026] ... and is it the shared wide view's (its history: 2 x 2 texels per pixel)
        m_super_wide_now.store(m_qv_mono && qs.periphery_aa > 0 && !m_super_wide_failed ? 2u : 0u, std::memory_order_relaxed);
    }

    // ---- debug dump of this frame? ----------------------------------------------------------------
    quadview::DumpFrame df{};
#if REVR_RELEASE
    constexpr bool dump = false; // [RELEASE SWITCH fix141] no dumps, jitter checks, captures or engine probes in the release
#else
    const bool dump = !m_qv_dump_pending && qv.take_dump_frame(df);

    // [JITTER CHECK 10.11.2026] starts with a dump request (the frames after it are recorded one after the other)
    if (qv.jitter_check_request.exchange(false) && m_jc.next < 0 && !m_jc.write) {
        for (auto& d : m_jc.dumps) {
            d.clear();
        }

        {
            std::scoped_lock _{m_jc.mtx};
            m_jc.log = std::format("jitter check: FSR 4 by the mod {} (atlas {}), layout render {}x{} fovea {}x{}, FSR 4 output {}x{}, network option {}\n", m_qv_fsr,
                m_qv_fsr_atlas.valid, m_qv_layout.render_w, m_qv_layout.render_h, m_qv_layout.inset_w, m_qv_layout.inset_h, m_qv_fsr_big_w, m_qv_fsr_big_h,
                qv.settings().fsr_model);
            m_jc.lines = 0;
            m_jc.layers.clear();
        }

        m_jc.next = 0;
        m_jc.idle_frames = 0;
    }

    // (FSR 4 by the mod not running: after 90 frames without a recorded one, the lines alone)
    if (m_jc.next >= 0 && !m_jc.captured && ++m_jc.idle_frames > 90) {
        jitter_check_line("no FSR 4 by the mod for 90 frames: check ended (is FSR 4 by the mod on and running?)");
        m_jc.next = -1;
        m_jc.write = true;
    }

    if (m_jc.captured) {
        m_jc.idle_frames = 0;
    }

    m_jc.captured = false;

    // [SUPER TAA 16.11.2026] capture of the supersampling TAA (see m_sc): asked for in the menu. The mod's TAA runs beside
    // it from now on; the recording starts 32 frames later, when that one's history has settled (it would look worse
    // than it is in its first frames).
    if (qv.super_capture_request.exchange(false)) {
        const auto qs = qv.settings();

        if (m_sc.busy()) {
            qv.push_note("[SUPER TAA CAPTURE] a capture is running already\n");
        } else {
            // [CAPTURE fix81 17.11.2026] it starts whatever the fovea's anti-aliasing and the debug view are: the frame's
            // chain (the wide view, the periphery, the eye image) is recorded in any case, the supersampling TAA's own
            // images when its pass runs (the captures of 17.11.2026 made with the debug view "periphery only" were empty)
            if (qs.fovea_mode != quadview::FoveaMode::TAA || !qs.fovea_super || m_super_taa_failed) {
                qv.push_note(std::format("[SUPER TAA CAPTURE] the supersampling TAA is not the fovea's anti-aliasing{}: only the frame's chain (wide view, "
                                         "periphery, eye image) is recorded\n",
                    m_super_taa_failed ? " (it failed: see the framework log)" : ""));
            }

            m_sc.start(qv.super_capture_frames.load(), (uint32_t)(std::max)(qv.super_capture_part.load(), 0));
            qv.super_capture_running = true;
        }
    }

    m_sc.tick();

    // [ENGINE PROBE 16.11.2026] see m_probe
    if (qv.engine_probe_request.exchange(false)) {
        engine_probe_start();
    }

    if (m_probe.state.load(std::memory_order_relaxed) == 1) {
        bool any = false;
        bool all = true;
        {
            std::scoped_lock _{m_probe.mtx};

            for (const auto& c : m_probe.copies) {
                any |= c.copied;
                all &= c.copied;
            }
        }

        ++m_probe.wait_frames;

        // (all copied, or two more frames given to the ones that were not: a target another layer owns is never copied)
        if (any && (all || m_probe.wait_frames > 3)) {
            engine_probe_record(g_framework->get_d3d12_hook()->get_device(), copier);
        } else if (m_probe.wait_frames > 60) {
            m_probe.report += "  nothing was copied in 60 frames (the view's post effect and overlay layers did not draw)\n";
            m_probe.written_pending = true; // written (the text alone) after this frame's work
            m_probe.state = 2;
        }
    }

    if (dump) {
        m_qv_dump.clear();
        m_qv_dump_header.clear();
        m_qv_dump_pre_ctx = nullptr;
        // [DUMP NUMBERS 25.10.2026] a new game session goes on after the dumps already in the folder: the numbers used to
        // start again at 1 and the frame folders of two sessions got mixed
        const auto dump_dir = REFramework::get_persistent_dir("quadview_dump");
        df.serial += qv_dump_number_base(dump_dir);
        m_qv_dump_path = dump_dir / std::format("dump{:02}", df.serial) / std::format("frame{}_both_eyes", df.index);
    }
#endif

    bool ok = true;

    if (m_qv_mono) {
        // [MONO PERIPHERY 27.10.2026] one wide view for both eyes, reprojected
        ok = evaluate_quadview_mono(copier, backbuffer, frame, dump, df);
    } else {
        // [STEREO FOVEA 27.10.2026] a wide view per eye (comparison): [0]/[2] = wide/fovea of the left eye, [1]/[3] of the right
        // [ONE SUBMIT 04.11.2026] as in evaluate_quadview_mono: the left eye's plugin call only records when the right one follows
        const auto qs = qv.settings();
        const auto& right_inset = m_eye_states[3];
        const auto has = [](const sdk::intrusive_ptr<sdk::renderer::Texture>& t) {
            return t != nullptr && t->get_d3d12_resource_container() != nullptr && t->get_d3d12_resource_container()->get_native_resource() != nullptr;
        };
        const bool right_plugin = qs.fovea_mode == quadview::FoveaMode::Upscaler && qs.debug_view != 1 && right_inset.scene_layer != nullptr &&
                                  right_inset.color != nullptr && has(right_inset.color_copy) && has(right_inset.depth_copy) &&
                                  has(right_inset.motion_vectors_copy);

        for (int32_t eye = 0; eye < 2; ++eye) {
            QvEval ev{};
            ev.plugin_execute = eye == 1 || !right_plugin;
            ev.eye = eye;
            ev.slot = (uint32_t)eye;
            ev.wide_state = (uint32_t)eye;
            ev.inset_state = 2u + (uint32_t)eye;
            ev.wide_id = get_evaluate_id((uint32_t)eye);
            ev.fovea_id = get_qv_fovea_id((uint32_t)eye);
            ev.to_backbuffer = eye == 0; // the desktop mirror shows the left eye
            ev.dump_prefix = eye == 0 ? "L_" : "R_";
            ok &= evaluate_quadview_eye(copier, backbuffer, frame, ev, dump, df);
        }
    }

    // [GPU 29.10.2026] the desktop window shows the left eye; it can be left out (the flatscreen canvas reads it, then it stays)
    // [SMALL WINDOW fix122] not into a window of another size (the flatscreen canvas then shows the window itself)
    // [CANVAS fix123 22.11.2026] and not while the flatscreen canvas is shown (RE4's 2D cutscenes; the quad view stays on
    // there). The canvas shows the window, and the window got the left eye's image for it. But the camera overrides rest
    // while the canvas runs, so the two fovea views render the game camera's whole picture like the wide view (the user's
    // dump of 19.11.2026, 14 frames: the fovea inputs are the wide input, 0 - 3 of 255 apart), and the eye image put
    // together from them is that picture with a small copy of it where the fovea lies: the cutscene shown twice, one inside
    // the other. Without the copy the window keeps the picture the engine put there itself, the cutscene once.
    if (m_qv_mirror_src != nullptr && qv.settings().desktop_mirror && !VR::get()->is_flatscreen_overlay() && mirror_fits(backbuffer)) {
        copier.copy(m_qv_mirror_src, backbuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PRESENT);
        copier.has_commands = true;
    }

    if (dump) {
        copier.has_commands = true;
        m_qv_dump_pending = true;
    }

    return ok;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SUPER TAA 16.11.2026] see m_sc: the frames written (after the GPU finished the last one's list) into the next free
// quadview_dump/supertaa_captureNN, the first numbers into the log
void TemporalUpscaler::super_capture_write() {
    auto& qv = quadview::State::get();
    const int frames = m_sc.frames_recorded();
    const auto root = REFramework::get_persistent_dir("quadview_dump");
    uint64_t number = 1;
    {
        std::error_code ec{};

        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            const auto name = e.path().filename().string();
            constexpr std::string_view prefix{"supertaa_capture"};

            if (name.size() > prefix.size() && name.rfind(prefix, 0) == 0) {
                number = (std::max)(number, (uint64_t)std::strtoull(name.c_str() + prefix.size(), nullptr, 10) + 1);
            }
        }
    }

    const auto base = root / std::format("supertaa_capture{:02}", number);
    const std::string summary = m_sc.write(base);
    qv.super_capture_running = false;
    REVR_LOG(spdlog::info("[TemporalUpscaler] supersampling TAA capture written to {}", base.string()));
    qv.push_note(std::format("[SUPER TAA CAPTURE] {} frames written to {}\n{}", frames, base.string(), summary),
        std::format("Supersampling TAA capture written: quadview_dump/{} ({} frames).", base.filename().string(), frames));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [ENGINE PROBE 16.11.2026] see m_probe. At a present: the fields of the left fovea view's scene layer listed, a copy made
// for every render target of its target states; the engine copies into them in its next frame (engine_probe_copy).
void TemporalUpscaler::engine_probe_start() {
    auto& qv = quadview::State::get();

    if (m_probe.state.load() != 0) {
        qv.push_note("[ENGINE PROBE] the last probe is still running\n");
        return;
    }

    const uint32_t index = m_qv_layout.valid ? (m_qv_mono ? 1u : 2u) : 0u;
    auto* layer = m_eye_states[index].scene_layer.get();

    if (layer == nullptr) {
        qv.push_note("[ENGINE PROBE] no scene layer of the left fovea view\n", "Engine buffers: no scene layer to read.");
        return;
    }

    auto* obj = (::REManagedObject*)layer;
    std::string text = std::format("[ENGINE PROBE] the scene layer of {} (state {}, {:x}): its reflected fields\n",
        m_qv_layout.valid ? "the left fovea view" : "the left eye", index, (uintptr_t)layer);

    // a field known to hold a target state gives the type name such fields have: every field of that type is read as one
    // (a field of another type is never read: its getter may write more than a pointer)
    const VariableDescriptor* known = utility::re_managed_object::get_field_desc(obj, "HDRTarget");

    if (known == nullptr) {
        known = utility::re_managed_object::get_field_desc(obj, "VelocityTarget");
    }

    const std::string target_type = known != nullptr && known->typeName != nullptr ? known->typeName : "";
    std::vector<std::string> targets{};

    for (auto* t = utility::re_managed_object::safe_get_type(obj); t != nullptr; t = t->super) {
        text += std::format("  type {}\n", t->name != nullptr ? t->name : "?");
        auto* vars = utility::re_type::get_variables(t);

        if (vars == nullptr || vars->data == nullptr) {
            continue;
        }

        for (int i = 0; i < vars->num && i < 256; ++i) {
            const auto* var = vars->data->descriptors[i];

            if (var == nullptr || var->name == nullptr) {
                continue;
            }

            const std::string type_name = var->typeName != nullptr ? var->typeName : "?";
            const bool is_target = !target_type.empty() && type_name == target_type && var->function != nullptr;
            text += std::format("    {} : {}\n", var->name, type_name);

            if (is_target && std::find(targets.begin(), targets.end(), var->name) == targets.end()) {
                targets.emplace_back(var->name);
            }
        }
    }

    text += std::format("  target states (fields of the type '{}'): {}\n", target_type, targets.size());
    std::vector<EngineProbe::Copy> copies{};
    constexpr size_t MAX_COPIES = 48;

    for (const auto& name : targets) {
        auto* state = layer->get_target_state(name);

        if (state == nullptr) {
            text += std::format("    {}: none\n", name);
            continue;
        }

        if (IsBadReadPtr(state, 0x60)) {
            text += std::format("    {}: not readable ({:x})\n", name, (uintptr_t)state);
            continue;
        }

        const uint32_t count = state->get_rtv_count();
        text += std::format("    {}: {} render target(s)\n", name, count);

        if (count > 8) {
            continue; // not a target state after all
        }

        for (uint32_t k = 0; k < count; ++k) {
            const auto rtv = state->get_rtv((int32_t)k);

            if (rtv == nullptr) {
                text += std::format("      [{}] none\n", k);
                continue;
            }

            auto& tex = rtv->get_texture_d3d12();
            auto* container = tex != nullptr ? tex->get_d3d12_resource_container() : nullptr;
            auto* native = container != nullptr ? container->get_native_resource() : nullptr;

            if (native == nullptr) {
                text += std::format("      [{}] no texture\n", k);
                continue;
            }

            const auto rd = native->GetDesc();
            text += std::format("      [{}] {}x{} DXGI format {} mips {} samples {} flags {:x}\n", k, (uint64_t)rd.Width, rd.Height, (int)rd.Format, rd.MipLevels,
                rd.SampleDesc.Count, (uint32_t)rd.Flags);

            if (rd.SampleDesc.Count != 1 || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                continue;
            }

            for (int stage = 0; stage < 2 && copies.size() < MAX_COPIES; ++stage) {
                EngineProbe::Copy c{};
                c.target = name;
                c.rtv = (int)k;
                c.stage = stage;
                c.copy = tex->clone();

                if (c.copy != nullptr) {
                    copies.push_back(std::move(c));
                }
            }
        }
    }

    if (copies.empty()) {
        qv.push_note(text + "  no render target to copy\n", "Engine buffers: the list is in foveation_debug.log (nothing to copy).");
        return;
    }

    {
        std::scoped_lock _{m_probe.mtx};
        m_probe.copies = std::move(copies);
    }

    m_probe.report = std::move(text);
    m_probe.wait_frames = 0;
    m_probe.written_pending = false;
    m_probe.dump.clear();
    m_probe.layer = (void*)layer;
    m_probe.state = 1;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// The engine's draw of the probed view: `stage` 0 before its post effects, 1 after its overlay. The copies of that stage
// are recorded on the engine's list (as the mod's copies of depth and motion are).
void TemporalUpscaler::engine_probe_copy(sdk::renderer::RenderLayer* scene_layer, void* render_context, int stage) {
    if (m_probe.state.load(std::memory_order_relaxed) != 1 || scene_layer == nullptr || render_context == nullptr ||
        (void*)scene_layer != m_probe.layer.load(std::memory_order_relaxed)) {
        return;
    }

    auto* context = (sdk::renderer::RenderContext*)render_context;
    std::scoped_lock _{m_probe.mtx};

    for (auto& c : m_probe.copies) {
        if (c.stage != stage || c.copied || c.copy == nullptr) {
            continue;
        }

        auto* state = scene_layer->get_target_state(c.target);

        if (state == nullptr || (uint32_t)c.rtv >= state->get_rtv_count()) {
            continue;
        }

        const auto rtv = state->get_rtv(c.rtv);

        if (rtv == nullptr) {
            continue;
        }

        auto tex = rtv->get_texture_d3d12();

        if (tex == nullptr || !copy_fits(c.copy, tex)) {
            continue;
        }

        context->copy_texture(c.copy, tex);
        c.copied = true;
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// At the present after the engine copied: the copies read back on this frame's list
void TemporalUpscaler::engine_probe_record(ID3D12Device* device, d3d12::CommandContext& copier) {
    constexpr auto ENGINE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto* cmd = copier.cmd_list.Get();
    std::scoped_lock _{m_probe.mtx};

    for (auto& c : m_probe.copies) {
        const std::string name = std::format("{}_{}_{}", c.stage == 0 ? "before_post" : "after_overlay", c.target, c.rtv);

        if (!c.copied || c.copy == nullptr || device == nullptr || cmd == nullptr) {
            m_probe.report += std::format("  {}: not copied\n", name);
            continue;
        }

        auto* container = c.copy->get_d3d12_resource_container();
        auto* native = container != nullptr ? container->get_native_resource() : nullptr;

        if (native == nullptr || !m_probe.dump.add(device, cmd, native, ENGINE_STATE, name)) {
            m_probe.report += std::format("  {}: could not be read\n", name);
            continue;
        }

        // the middle of the image as stored too (the values themselves: which channel holds what)
        const auto rd = native->GetDesc();
        const uint32_t pw = (std::min)((uint32_t)rd.Width, 256u);
        const uint32_t ph = (std::min)(rd.Height, 256u);
        m_probe.dump.add_raw(name, ((uint32_t)rd.Width - pw) / 2, (rd.Height - ph) / 2, pw, ph);
        copier.has_commands = true;
    }

    m_probe.written_pending = true;
    m_probe.state = 2;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// After that frame's GPU work: the images and the list written, the copies given back (kept a few frames like every copy
// the mod drops)
void TemporalUpscaler::engine_probe_write() {
    auto& qv = quadview::State::get();
    const auto root = REFramework::get_persistent_dir("quadview_dump");
    uint64_t number = 1;
    {
        std::error_code ec{};

        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            const auto name = e.path().filename().string();
            constexpr std::string_view prefix{"engine_probe"};

            if (name.size() > prefix.size() && name.rfind(prefix, 0) == 0) {
                number = (std::max)(number, (uint64_t)std::strtoull(name.c_str() + prefix.size(), nullptr, 10) + 1);
            }
        }
    }

    const auto base = root / std::format("engine_probe{:02}", number);
    std::string text = std::move(m_probe.report);
    m_probe.report.clear();

    if (!m_probe.dump.empty()) {
        text += std::format("  images (BMP at up to 1024 pixels, and the middle 256x256 as stored) in {}:\n", base.string());
        text += m_probe.dump.write_all(base, 1024);
    }

    m_probe.dump.clear();
    {
        std::error_code ec{};
        std::filesystem::create_directories(base, ec);
        std::ofstream f(base / "engine_probe.txt");
        f << text;
    }
    {
        std::scoped_lock _{m_probe.mtx};

        for (auto& c : m_probe.copies) {
            retire_copy(c.copy);
        }

        m_probe.copies.clear();
    }

    m_probe.layer = nullptr;
    m_probe.written_pending = false;
    m_probe.state = 0;
    REVR_LOG(spdlog::info("[TemporalUpscaler] engine buffer probe written to {}", base.string()));
    qv.push_note(text, std::format("Engine buffers written: quadview_dump/{}.", base.filename().string()));
}
#endif

// [PERIPHERY] light TAA of a wide view (render size) into the history of `slot`; returns the new history image (UAV)
ID3D12Resource* TemporalUpscaler::periphery_taa(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t slot, ID3D12Resource* color,
    ID3D12Resource* motion, ID3D12Resource* depth, const quadview::Settings& settings, float jitter_x, float jitter_y) {
    auto& qv = quadview::State::get();

    if (color == nullptr || slot > 1) {
        set_qv_status("periphery TAA: no input");
        return nullptr;
    }

    const auto cd = color->GetDesc();

    // [PERIPHERY SUPER fix80 17.11.2026] the shared wide view through the mod's supersampling TAA when the settings ask for
    // it and it can run (periphery_super_taa); otherwise, and for a wide view per eye, the light TAA below as before.
    // m_periphery_ss / m_periphery_linear tell the caller what it got.
    m_periphery_ss = 1;
    m_periphery_linear = false;

    if (slot == 0) {
        if (ID3D12Resource* super = periphery_super_taa(device, cmd, color, motion, depth, settings, jitter_x, jitter_y)) {
            return super;
        }
    }

    if (!ensure_qv_taa(device, (uint32_t)cd.Width, cd.Height)) {
        set_qv_status("periphery: could not create the TAA history");
        qv.mark_failed(m_qv_init_generation, "periphery: could not create the TAA history");
        return nullptr;
    }

    auto& cur = m_qv_taa_cur[slot];
    ID3D12Resource* hist_in = m_qv_taa_hist[slot][cur].Get();
    ID3D12Resource* hist_out = m_qv_taa_hist[slot][cur ^ 1u].Get();

    d3d12::LightTAA::Params tp{};
    // same convention the upscaler gets: pixels = mv * m_motion_scale -> UV = mv * m_motion_scale / render size
    tp.mv_scale_x = m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u);
    tp.mv_scale_y = m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u);
    tp.feedback = std::clamp(settings.taa_feedback, 0.0f, 0.98f);
    // a frame without this history's pass (skipped, debug view, technique change): its content is too old to reproject
    tp.reset = !m_qv_taa_valid[slot] || m_qv_taa_frame[slot] + 1 != m_qv_frame;
    tp.dilate = true;
    tp.reverse_z = true; // the upscaler is set up with inverted depth as well
    tp.jitter_x = jitter_x;
    tp.jitter_y = jitter_y;
    tp.reconstruct = true;
    tp.anti_flicker = true;
    tp.tiled = !settings.test_round2_passes; // [GPU 29.10.2026 round 3] same result, the GPU pass test compares
    tp.tile = settings.test_light_taa_32 ? 32u : 16u; // [GPU 31.10.2026] same result
    tp.halo_serial = settings.test_halo_serial;       // [GPU 01.11.2026] same result

    // [CALM fix100 20.11.2026] the third choice of the periphery's anti-aliasing: the same pass with the protection of what
    // one frame's samples cannot show (LightTAA::Params::calm), for the shared wide view. Its second history: two images
    // of the wide view's size, swapped with the history.
    bool calm = false;

    if (slot == 0 && m_qv_mono && settings.periphery_aa == 3 && !m_qv_calm_failed) {
        bool made[2]{};

        if (ensure_uav_image(device, m_qv_calm_aux[0], (uint32_t)cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                L"RE4VR shared wide view, calm TAA statistics 0", &made[0]) &&
            ensure_uav_image(device, m_qv_calm_aux[1], (uint32_t)cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                L"RE4VR shared wide view, calm TAA statistics 1", &made[1])) {
            calm = true;
            tp.calm = true;
            tp.feedback = quadview::Settings::CALM_FEEDBACK;
            tp.aux_in = m_qv_calm_aux[cur].Get();
            tp.aux_out = m_qv_calm_aux[cur ^ 1u].Get();
            const bool goes_on = !tp.reset && !made[0] && !made[1] && m_qv_calm_frame + 1 == m_qv_frame;

            if (!goes_on) {
                // [GUARD fix101 20.11.2026] taking over from the supersampling TAA (the guard's step, or the menu): its image
                // of the frame before is the history (display encoded like this pass's; of the wide view's size or twice
                // that, read by its place), the statistics start from zero. Nothing starts again on the screen.
                ID3D12Resource* const super_image = m_qv_wide_staa_frame + 1 == m_qv_frame && (m_qv_wide_staa_ss == 1 || m_qv_wide_staa_ss == 2)
                                                        ? m_qv_wide_super[m_qv_wide_staa_ss - 1].Get() : nullptr;
                tp.reset = super_image == nullptr;

                if (super_image != nullptr) {
                    hist_in = super_image;
                    tp.calm_fresh = true;
                }
            }
        } else {
            REVR_LOG(spdlog::error("[TemporalUpscaler] the calm light TAA's second history could not be made: the light TAA in the periphery until the next set-up"));
            m_qv_calm_failed = true;
        }
    }

    if (slot == 0) {
        m_periphery_calm.store(calm, std::memory_order_relaxed);
    }

    if (!m_light_taa.dispatch(device, cmd, color, motion, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, hist_in, hist_out, tp)) {
        const std::string why = std::string{"periphery TAA failed: "} + m_light_taa.last_error();
        set_qv_status(why);
        qv.mark_failed(m_qv_init_generation, why);
        return nullptr;
    }

    m_qv_taa_valid[slot] = true;
    m_qv_taa_frame[slot] = m_qv_frame;

    if (calm) {
        m_qv_calm_frame = m_qv_frame;
    }

    const bool light_linear = m_light_taa.color_is_linear();

    // [GUARD fix101 20.11.2026] the frames before the guard's step back from the calm light TAA: the supersampling TAA runs
    // behind it (its image of the wide view's size is not shown), so that its history is settled when it takes over
    if (calm && settings.periphery_warm_super && !m_super_wide_failed) {
        quadview::Settings warm = settings;
        warm.periphery_aa = 1;
        const uint32_t logged = m_periphery_logged;
        m_periphery_logged = 1u | (1u << 16); // (what this call would log: nothing is shown of it)
        periphery_super_taa(device, cmd, color, motion, depth, warm, jitter_x, jitter_y);
        m_periphery_logged = logged;
        m_periphery_running.store(0u, std::memory_order_relaxed);
        m_periphery_ss = 1;
        m_periphery_linear = light_linear;
    }
    m_periphery_linear = m_light_taa.color_is_linear();
    cur ^= 1u;
    return hist_out;
}

// [PERIPHERY SUPER fix80 17.11.2026] The mod's supersampling TAA (d3d12::SuperTAA, an instance of its own with one view's
// histories) on the shared wide view, instead of the light TAA. The light TAA accumulates display-encoded values, holds
// its history inside the 3x3 of the frame and rebuilds every frame at the pixel centre with a Gaussian of 0.47 px: in a
// view with a quarter of the eye image's density most of what the fovea shows sharp is thinner than a pixel, and the
// light TAA loses it (thin bright things lose their light, textures the top of their detail), which is what the eye
// meets at the fovea's border. The supersampling TAA keeps what one frame's samples cannot show (its protection),
// accumulates in linear light and holds 2 x 2 history texels per pixel of the wide view.
//   periphery_aa 1: its image at the wide view's size (the history reduced 2 : 1 with Lanczos 2, sharpened like the fovea)
//   periphery_aa 2: its image at twice that size (the history itself, encoded): m_periphery_ss = 2 tells the caller, the
//     reprojection then writes 2 x 2 pixels per pixel of the wide view (MonoPeriphery::Params::ss). Detail between one
//     and two samples per pixel of the wide view gets to the eye image, and what moves under the wide view's grid
//     crawls less.
// The settings are the fovea's (quadview::Settings::super_taa), the stability its own ([fix82] Settings::periphery_stability). Returns the image (resting in UNORDERED_ACCESS, display
// encoded), or nullptr: the light TAA runs (m_periphery_why says why when it was asked for).
ID3D12Resource* TemporalUpscaler::periphery_super_taa(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* motion,
    ID3D12Resource* depth, const quadview::Settings& settings, float jitter_x, float jitter_y) {
    // ([CALM fix100 20.11.2026] 3 = the calm light TAA: not this pass, periphery_taa makes it)
    const bool calm_asked = settings.periphery_aa == 3;
    const int asked = calm_asked ? 0 : std::clamp(settings.periphery_aa, 0, 2);
    uint32_t why = PW_NONE;
    uint32_t ss = asked == 2 ? 2u : 1u;
    ID3D12Resource* image = nullptr;

    if (asked == 0) {
        // (the light TAA was asked for)
    } else if (!m_qv_mono) {
        why = PW_NOT_SHARED;
    } else if (m_super_wide_failed || color == nullptr || motion == nullptr) {
        why = PW_FAILED;
    } else {
        // depth and motion at the colour's size only (RE7's video tape draws the game camera smaller: the light TAA reads
        // them at the colour's place, see [IMAGE QUALITY 15.11.2026])
        const auto cd = color->GetDesc();
        const auto md = motion->GetDesc();
        const bool sizes = md.Width == cd.Width && md.Height == cd.Height &&
                           (depth == nullptr || (depth->GetDesc().Width == cd.Width && depth->GetDesc().Height == cd.Height));

        if (!sizes) {
            why = PW_SIZES;
        } else {
            if (ss == 2 && !quadview::periphery_2x_fits(m_qv_layout)) {
                ss = 1;
                why = PW_DENSE;
            } else if (ss == 2 && !m_mono_periphery.ss_ready(device)) {
                ss = 1;
                why = PW_NO_2X;
            }

            auto& target = m_qv_wide_super[ss - 1];

            if (!ensure_uav_image(device, target, (uint32_t)cd.Width * ss, cd.Height * ss, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    ss == 2 ? L"RE4VR shared wide view, supersampling TAA (twice the size)" : L"RE4VR shared wide view, supersampling TAA")) {
                why = PW_NO_IMAGE;
            } else {
                const auto& st = settings.super_taa;
                d3d12::SuperTAA::Params sp{};
                // the history holds 2 x 2 texels per pixel of the wide view either way: the image of twice the size is the
                // history itself (scale 1 of an image twice as large), the other its 2 : 1 reduction
                sp.s.scale = ss == 2 ? 1u : 2u;
                sp.s.filter_scale = st.filter_scale;
                sp.s.stability = settings.periphery_stability; // [PERIPHERY fix82 17.11.2026] its own (10; the fovea's is 5)
                sp.s.clip_gamma = st.clip_gamma;
                sp.s.motion_reduce = st.motion_reduce;
                sp.s.depth_reject = st.depth_reject;
                sp.s.history_sharpness = settings.periphery_history_sharpness; // [PERIPHERY fix83 17.11.2026] its own (0; the fovea's is 0.5)
                // (twice the size: no pass over its four times as many pixels for the sharpening; the enlargement to the
                // eye image is only x2 then)
                sp.s.sharpen = ss == 2 ? 0.0f : st.sharpen;
                sp.s.luma_weight = st.luma_weight;
                sp.s.dering = st.lighter ? 2u : 1u;
                sp.s.nearest_sample = st.lighter;
                // (the wide view's size: Lanczos 2 instead of the mean of 2 x 2; on the simulated periphery it keeps more of
                // the detail the wide view's grid can hold and crawls less in motion)
                sp.s.box_resolve = false;
                sp.s.follow = st.follow;
                sp.s.follow_threshold = st.follow_threshold;
                sp.s.follow_memory = st.follow_memory;
                sp.s.protect = st.protect;
                sp.s.protect_range = st.protect_range;
                sp.s.protect_spark = st.protect_spark;
                sp.s.protect_memory = st.protect_memory;
                sp.s.arrive = st.arrive;
                sp.eye = 0;
                // same convention the upscaler gets: pixels = mv * m_motion_scale -> UV = mv * m_motion_scale / render size
                sp.mv_scale_x = m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u);
                sp.mv_scale_y = m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u);
                sp.jitter_x = jitter_x;
                sp.jitter_y = jitter_y;
                sp.reverse_z = true;
                // a frame without its pass (skipped, the light TAA meanwhile) or the other image before: start again
                // ([GUARD fix101 20.11.2026] the image's size alone no longer starts the history again: it has the same size
                // with both, see SuperTAA::ensure_images)
                sp.reset = m_qv_wide_staa_frame + 1 != m_qv_frame;

                // distance from the depth value: rows 2 and 3 of the wide view's inverse projection (as the reprojection takes
                // them). The engine stores the depth reversed relative to that projection.
                {
                    const glm::mat4 reverse_z{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
                    const auto inv = glm::inverse(reverse_z * VR::get()->get_union_projection_matrix());
                    sp.depth_k[0] = inv[2][2];
                    sp.depth_k[1] = inv[3][2];
                    sp.depth_k[2] = inv[2][3];
                    sp.depth_k[3] = inv[3][3];
                    sp.depth_k_valid = std::isfinite(sp.depth_k[0]) && std::isfinite(sp.depth_k[1]) && std::isfinite(sp.depth_k[2]) &&
                                       std::isfinite(sp.depth_k[3]) && (std::abs(sp.depth_k[2]) + std::abs(sp.depth_k[3])) > 1e-9f;
                }

                if (m_super_taa_wide.dispatch(device, cmd, color, motion, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, target.Get(), sp)) {
                    m_qv_wide_staa_frame = m_qv_frame;
                    m_qv_wide_staa_ss = ss;
                    m_periphery_ss = ss;
                    m_periphery_linear = false; // (display encoded, whatever the colour view decodes)
                    image = target.Get();
                } else {
                    REVR_LOG(spdlog::error("[TemporalUpscaler] the periphery's supersampling TAA failed ({}): the light TAA in the periphery until the next set-up",
                        m_super_taa_wide.last_error()));
                    m_super_wide_failed = true;
                    why = PW_FAILED;
                }
            }
        }
    }

    const uint32_t running = image != nullptr ? ss : 0u;
    m_periphery_running.store(running, std::memory_order_relaxed);
    m_periphery_why.store(why, std::memory_order_relaxed);

    // (the log tells once what the periphery gets, and again when that changes)
    if (const uint32_t key = running | (why << 8) | ((uint32_t)asked << 16) | (calm_asked ? 1u << 24 : 0u); key != m_periphery_logged) {
        m_periphery_logged = key;
        REVR_LOG(spdlog::info("[TemporalUpscaler] tri-view periphery: {}{}{}",
            running == 2 ? "supersampling TAA, image of twice the wide view's size (2 x 2 pixels per pixel of the wide view to each eye)"
            : running == 1 ? "supersampling TAA, image of the wide view's size"
            : calm_asked ? "calm light TAA (the light TAA with the protection of thin detail), image of the wide view's size" : "light TAA",
            why != PW_NONE ? " -- " : "", periphery_why_text(why)));
    }

    return image;
}

// [MONO PERIPHERY 27.10.2026] see TemporalUpscaler.hpp. Everything the fovea upscaler must not wait for (TAA of the wide
// view, its reprojection to each eye, the interface taken out of the fovea views) is recorded on the pre-fovea context
// and submitted before the plugin runs; the periphery upscale, the fovea pass and the composite of each eye follow in
// evaluate_quadview_eye on the copier.
// [UI MOTION fix125 22.11.2026] the matrices view `view` (0 wide, 1 / 2 the fovea views) had in frame `frame`
bool TemporalUpscaler::ui_view_matrices(size_t view, int64_t frame, UiViewMatrices& out) const {
    if (view >= std::size(m_ui_views_by_frame) || frame < 0) {
        return false;
    }

    std::scoped_lock _{m_jitter_frame_mtx};
    const auto& k = m_ui_views_by_frame[view][(size_t)frame % std::size(m_ui_views_by_frame[0])];

    if (k.frame != frame) {
        return false;
    }

    out = k;
    return true;
}

// [UI MOTION fix125 22.11.2026] The blocks of the interface's own motion for the three views of frame `frame`
// ([0] the wide view, [1] / [2] the fovea views; none for a view without what it takes: the pass then uses the scene's
// motion vectors as before).
// Every element drawn in the frame has its world matrix (quadview::GuiAnchor::mat, kept per frame by the VR mod's GUI
// callbacks); with the same element's matrix of the frame before and the view's matrices of both frames its plane has
// an exact motion on the screen (quadview::ui_plane_motion). The pixels are given to the elements as the eye pass gives
// them their depth: an element that stands at its own place (the HUD at the weapon, an icon at an item, a beam, a world
// element of the game) owns the pixels around its anchor, the nearest first; every other interface pixel belongs to
// "the panels" (the 2D interface in front of the game camera: one plane, one motion; the panel at the middle depth is
// taken). An element without a frame before, or with numbers that make no sense, leaves its pixels to the scene's
// motion vectors.
std::array<d3d12::UiMotion::Block, 3> TemporalUpscaler::ui_motion_blocks(ID3D12Device* device, int32_t frame, bool want_report) {
    std::array<d3d12::UiMotion::Block, 3> blocks{};
    auto& qv = quadview::State::get();
    std::vector<quadview::GuiAnchor> now{};
    std::vector<quadview::GuiAnchor> before{};
    const int64_t f = (int64_t)(uint32_t)frame;
    const int64_t fb = (int64_t)(uint32_t)(frame - 1);

    if (!qv.gui_elements(f, now) || !qv.gui_elements(fb, before)) {
        if (want_report) {
            m_ui_motion_report = std::format("interface motion: no elements kept for frame {} or the one before ({} / {})", frame, now.size(), before.size());
        }

        return blocks;
    }

    const float w = (float)(std::max)(m_qv_layout.render_w, 1u);
    const float h = (float)(std::max)(m_qv_layout.render_h, 1u);
    std::string report{};

    for (size_t view = 0; view < 3; ++view) {
        UiViewMatrices vm{};

        if (!ui_view_matrices(view, f, vm)) {
            if (want_report) {
                report += std::format(" | view {}: no matrices of frame {}", view, frame);
            }

            continue;
        }

        double vp_cur[16];
        double vp_old[16];
        quadview::ui_view_proj(vm.proj, vm.view, vp_cur);
        quadview::ui_view_proj(vm.old_proj, vm.old_view, vp_old);
        const float sx = std::abs(vm.proj[0]) * 0.5f; // a tangent across -> UV
        const float sy = std::abs(vm.proj[5]) * 0.5f;

        struct Owned {
            float depth;
            float box[4];
            float d[9];
            bool known;
            bool other_panel; // a panel of the 2D interface at another distance than the others
            const char* name;
        };

        std::vector<Owned> owned{};
        std::vector<std::pair<float, std::array<float, 9>>> panels{}; // depth, motion
        uint32_t with_before = 0;
        uint32_t no_plane = 0;

        // the motion at (u, v) in pixels; false if it makes no sense (over a quarter of the view in a frame)
        const auto motion_px = [&](const float* d, float u, float v, float& mx, float& my) {
            float du = 0.0f;
            float dv = 0.0f;
            quadview::ui_motion_at(d, u, v, du, dv);
            mx = du * w;
            my = dv * h;
            return std::isfinite(du) && std::isfinite(dv) && std::abs(du) < 0.25f && std::abs(dv) < 0.25f;
        };

        for (const auto& a : now) {
            if (!a.has_mat) {
                continue;
            }

            // where its anchor is in this view
            const double px = a.mat[12];
            const double py = a.mat[13];
            const double pz = a.mat[14];
            const double cx = vp_cur[0] * px + vp_cur[4] * py + vp_cur[8] * pz + vp_cur[12];
            const double cy = vp_cur[1] * px + vp_cur[5] * py + vp_cur[9] * pz + vp_cur[13];
            const double cw = vp_cur[3] * px + vp_cur[7] * py + vp_cur[11] * pz + vp_cur[15];

            if (!(cw > 0.05) || !std::isfinite(cx) || !std::isfinite(cy)) {
                continue; // behind the head
            }

            const float cu = (float)(0.5 + 0.5 * cx / cw);
            const float cv = (float)(0.5 - 0.5 * cy / cw);
            const float z = (float)cw;
            const auto b = std::find_if(before.begin(), before.end(), [&](const quadview::GuiAnchor& o) { return o.id == a.id && o.has_mat; });
            float d[9]{};
            bool known = false;

            if (b != before.end()) {
                ++with_before;
                known = quadview::ui_plane_motion(vp_cur, vp_old, a.mat, b->mat, d);
                no_plane += known ? 0u : 1u;
            }

            const bool panel = a.kind == quadview::GuiAnchor::PANEL && !(a.radius_tan > 0.0f);

            if (panel) {
                float mx = 0.0f;
                float my = 0.0f;

                if (known && motion_px(d, 0.5f, 0.5f, mx, my)) {
                    std::array<float, 9> dd{};
                    std::copy(d, d + 9, dd.begin());
                    panels.emplace_back(z, dd);
                }

                continue;
            }

            // an element at its own place: the pixels around its anchor (as MonoUi gives them their depth)
            float across = a.radius_tan > 0.0f ? a.radius_tan : 0.15f;
            float above = across;
            float below = across;

            if (a.kind == quadview::GuiAnchor::BEAM) {
                across = (std::min)(0.12f, 0.035f + 0.06f / z);
                above = (std::min)(1.5f, 0.05f + 2.0f / z);
                below = (std::min)(0.6f, 0.05f + 0.6f / z);
            } else if (a.kind == quadview::GuiAnchor::PANEL && a.at_target) {
                across = 0.06f + 0.02f / z;
                above = 0.12f + 0.12f / z;
                below = 0.05f;
            }

            Owned o{};
            o.depth = z;
            o.box[0] = cu - across * sx;
            o.box[1] = cv - above * sy;
            o.box[2] = cu + across * sx;
            o.box[3] = cv + below * sy;
            o.name = a.name;
            o.other_panel = a.kind == quadview::GuiAnchor::PANEL && !a.at_target;

            if (o.box[2] < 0.0f || o.box[0] > 1.0f || o.box[3] < 0.0f || o.box[1] > 1.0f) {
                continue; // not in this view
            }

            float mx = 0.0f;
            float my = 0.0f;
            o.known = known && motion_px(d, std::clamp(cu, 0.0f, 1.0f), std::clamp(cv, 0.0f, 1.0f), mx, my);
            std::copy(d, d + 9, o.d);
            owned.push_back(o);
        }

        std::stable_sort(owned.begin(), owned.end(), [](const Owned& x, const Owned& y) { return x.depth < y.depth; });

        // [fix126 22.11.2026] A panel of the 2D interface that stands behind the others (RE4's menu backdrop AcBackGround at
        // 19 m, a full-screen filter at 5 m) takes no pixels from them: the writing of the panels in front of it lies all
        // over its place, and the dumps of 22.11.2026 had the backdrop's motion up to 0.5 pixels a frame off the panels'.
        // Its own picture has no fine detail; it goes with the panels.
        float panel_mid = -1.0f;

        if (!panels.empty()) {
            std::sort(panels.begin(), panels.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            panel_mid = panels[panels.size() / 2].first;
        }

        uint32_t behind = 0;

        // elements at the same place with the same motion (the HUD at the weapon is three of them) count once
        std::vector<Owned> kept{};

        for (const auto& o : owned) {
            if (o.other_panel && panel_mid > 0.0f && o.depth > panel_mid) {
                ++behind;
                continue;
            }

            const bool again = std::any_of(kept.begin(), kept.end(), [&](const Owned& e) {
                bool same = e.known == o.known;

                for (int i = 0; i < 4 && same; ++i) {
                    same = std::abs(e.box[i] - o.box[i]) < 0.002f;
                }

                for (int i = 0; i < 9 && same && o.known; ++i) {
                    same = std::abs(e.d[i] - o.d[i]) < 2e-5f;
                }

                return same;
            });

            if (!again) {
                kept.push_back(o);
            }
        }

        d3d12::UiMotion::Constants k{};
        const uint32_t count = (uint32_t)(std::min)(kept.size(), (size_t)d3d12::UiMotion::MAX);
        k.count = (float)count;

        for (uint32_t i = 0; i < count; ++i) {
            std::memcpy(k.box[i], kept[i].box, sizeof(k.box[i]));

            for (int r = 0; r < 3; ++r) {
                std::memcpy(k.d[i * 3 + r], kept[i].d + r * 3, 3 * sizeof(float));
            }

            k.d[i * 3][3] = kept[i].known ? 1.0f : 0.0f;
        }

        float panel_mx = 0.0f;
        float panel_my = 0.0f;
        float spread = 0.0f;

        if (!panels.empty()) {
            std::sort(panels.begin(), panels.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            const auto& mid = panels[panels.size() / 2].second;

            for (int r = 0; r < 3; ++r) {
                std::memcpy(k.panel[r], mid.data() + r * 3, 3 * sizeof(float));
            }

            k.panel[0][3] = 1.0f;
            motion_px(mid.data(), 0.5f, 0.5f, panel_mx, panel_my);

            // (how far the panels' motions are apart at the view's middle: they are placed by one rule and should agree)
            for (const auto& pm : panels) {
                float mx = 0.0f;
                float my = 0.0f;
                motion_px(pm.second.data(), 0.5f, 0.5f, mx, my);
                spread = (std::max)(spread, (std::max)(std::abs(mx - panel_mx), std::abs(my - panel_my)));
            }
        }

        blocks[view] = m_ui_motion.write(device, k);

        if (want_report) {
            report += std::format(" | {}: {} elements with a frame before ({} without a plane), panels {} moving ({:+.2f},{:+.2f}) px at the middle{}, {} places of their own{}",
                view == 0 ? "wide view" : (view == 1 ? "fovea L" : "fovea R"), with_before, no_plane, panels.size(), panel_mx, panel_my,
                spread > 0.05f ? std::format(" (apart by up to {:.2f} px)", spread) : std::string{}, count,
                (kept.size() > count ? std::format(" ({} more left to the panels)", kept.size() - count) : std::string{}) +
                    (behind > 0 ? std::format(" ({} behind the panels go with them)", behind) : std::string{}));

            for (uint32_t i = 0; i < count && i < 3; ++i) {
                float mx = 0.0f;
                float my = 0.0f;
                motion_px(kept[i].d, std::clamp((kept[i].box[0] + kept[i].box[2]) * 0.5f, 0.0f, 1.0f), std::clamp((kept[i].box[1] + kept[i].box[3]) * 0.5f, 0.0f, 1.0f), mx,
                    my);
                report += kept[i].known ? std::format(", {} {:.2f} m ({:+.2f},{:+.2f}) px", kept[i].name, kept[i].depth, mx, my)
                                        : std::format(", {} {:.2f} m (the scene's vectors)", kept[i].name, kept[i].depth);
            }
        }
    }

    if (want_report) {
        m_ui_motion_report = std::format("interface motion: frame {} has {} elements, the frame before {}", frame, now.size(), before.size()) + report;
    }

    return blocks;
}

bool TemporalUpscaler::evaluate_quadview_mono(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, bool dump_arg,
    const quadview::DumpFrame& df) {
#if REVR_RELEASE
    constexpr bool dump = false; // [RELEASE SWITCH fix141] no dumps in the release: the compiler leaves their branches out
    (void)dump_arg;
#else
    const bool dump = dump_arg;
#endif
    auto& qv = quadview::State::get();
    const auto settings = qv.settings();
    auto vr = VR::get();
    auto device = g_framework->get_d3d12_hook()->get_device();
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto ENGINE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    auto native = [](const sdk::intrusive_ptr<sdk::renderer::Texture>& tex) -> ID3D12Resource* {
        if (tex == nullptr) {
            return nullptr;
        }

        const auto container = tex->get_d3d12_resource_container();
        return container != nullptr ? container->get_native_resource() : nullptr;
    };

    const auto& wide = m_eye_states[0];
    ID3D12Resource* wide_color = native(wide.color_copy);
    ID3D12Resource* wide_depth = native(wide.depth_copy);
    ID3D12Resource* wide_mv = native(wide.motion_vectors_copy);

    if (m_layers_in_use < 3 || wide.scene_layer == nullptr || wide.color == nullptr || wide_color == nullptr || wide_depth == nullptr || wide_mv == nullptr) {
        ++qv.skipped;
        set_qv_status(std::format("waiting for the shared wide view (scene layers found: {})", m_layers_in_use));
        return false;
    }

    const auto fr = qv.frusta();

    if (!fr.valid || !m_qv_layout.valid) {
        ++qv.skipped;
        set_qv_status("waiting for the eye frusta");
        return false;
    }

    const size_t ring = (size_t)(&copier - m_copiers.data()) % m_qv_pre_copiers.size();
    // [ONE LIST 01.11.2026] with the mod's TAA in the fovea nothing runs between the pre-fovea work and the copier: the pre
    // work goes on the copier's list (test of 01.11: the queue's gap between two lists, -0.010 ms). A list of its own with
    // the plugin in the fovea (its passes run in between), in a dump frame (the dump waits for that list) and in the GPU
    // pass test's variant.
    const bool one_list = !settings.test_separate_pre && !dump && settings.fovea_mode == quadview::FoveaMode::TAA;
    auto& pre = one_list ? copier : m_qv_pre_copiers[ring];

    if (pre.cmd_list == nullptr) {
        ++qv.skipped;
        set_qv_status("mono periphery: no pre-fovea command context");
        return false;
    }

    pre.wait(INFINITE);
    auto* cmd = pre.cmd_list.Get();

    // [GPU BREAKDOWN] the pre-fovea work between two stamps, with one after each of its passes in between
    auto& spans = d3d12::GpuSpans::get();
    spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_BEGIN);
    pre.has_commands |= spans.enabled();

    if (one_list) {
        spans.set_mode((uint64_t)frame, d3d12::GpuSpans::MODE_ONE_LIST);
    }

    ++qv.path_frames[one_list ? 1 : 0];

    // the pre-fovea work's end; its own list goes to the queue (one list: the copier goes at the end of the frame)
    // [FSR DIRECT 06.11.2026] work recorded on the pre-fovea list after its end stamp (FSR 4 called by the mod)
    std::function<void()> after_pre{};
    auto pre_end = [&]() {
        spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_END);

        if (after_pre) {
            after_pre();
        }

        spans.resolve(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_BEGIN, d3d12::GpuSpans::PRE_END - d3d12::GpuSpans::PRE_BEGIN + 1);

        if (!one_list) {
            pre.execute();
        }
    };

    // ---- 0. [MONO UI] the wide view without its interface: the interface goes back per eye at the end (MonoUi) ----------
    // The GUI is drawn in this view as seen from between the eyes; reprojected with the depth of what lies behind it, it
    // landed at the wrong depth in each eye (test 27.10.2026: the title logo and the menus twice, the item icons at
    // different places in the two eyes).
    ID3D12Resource* centre_hud = nullptr;
    ID3D12Resource* centre_fin = nullptr;
    void* centre_ui_layer = nullptr;
    const bool centre_ui = vr->ui_capture_pair(VR::UI_CAPTURE_CENTRE, centre_hud, centre_fin, centre_ui_layer) &&
                           centre_ui_layer == (void*)wide.scene_layer.get();
    ID3D12Resource* wide_input = wide_color;
    bool gui_map_ok = false;
    // [MENUS fix81 17.11.2026] the interface steady, for the single eye pass (the separate passes and the GPU test's variant
    // without interface keep the path of fix80): read where the frame's jitter put it.
    // [MENUS fix82 17.11.2026] RE7 captures of 17.11.2026 16:0x (fix81: pause menu, inventory, a dialog, the objective): what
    // the interface lit was see-through. A white bar, the item cards: where the scene hidden behind the menu was as bright as
    // they are, UiClean's light layer had holes in the hidden scene's shape (the light is found against the view without
    // GUI), its fit of what the panels leave of the scene went wrong beside them, and the eye pass's menu backdrop put the
    // periphery where the fovea came out darker. No rule that takes the interface's light out of a fovea view's picture
    // holds there: the view has no copy with GUI to tell the interface by.
    // It does not have to come out. A fovea view's interface is drawn by the engine with that view's own projection (the
    // eye's own place, the fovea's resolution), through the frame's jitter like the scene (fix81's measurement), and the
    // menus stand in the world (captures 01, 02, 05, 06: their motion on the screen is the scene's motion vectors' within
    // 0.05 pixels a frame; 04, the head moving fast: 1.2). So the mod's temporal anti-aliasing takes it like the scene: the
    // captures' own frames with the interface left in, through the supersampling TAA's reference model from an empty
    // history, give the menu whole after 8 frames (0.9 % - 1.3 % from the frame's own pixel where the interface is bright,
    // 3.4 % with the head moving fast; a white bar and the cards solid over the brightest scene).
    // With the fovea on the mod's TAA the fovea views now go to it as the engine drew them (no UiClean on them: two passes
    // less) and the eye pass adds no interface inside the fovea (EyeCompose::Params::fovea_ui_inside). With FSR 4 in the
    // fovea the path of fix80 stays (FSR 4 has no way to be told what is interface).
    const bool steady_ui = settings.steady_interface && !settings.separate_eye_passes && !settings.test_no_interface && m_eye_compose.ready(device);
    const bool ui_inside = steady_ui && settings.fovea_mode == quadview::FoveaMode::TAA;

    if (centre_ui) {
        // [UI CURVE 27.10.2026] this view has both copies of the same image (PostMainTarget with GUI, colour after the output
        // pass): the output curve measured on it is what the fovea views are compared with
        // ([COPIES fix116] GPU test 58: not measured, the table of the last frames stays)
        if (!settings.test_ui_idle && !m_ui_curve.learn(device, cmd, centre_fin, wide_color, ENGINE_STATE, !settings.test_round2_passes)) {
            static bool once = true;

            if (once) {
                REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: output curve not measured: {}", m_ui_curve.last_error()));
                once = false;
            }
        }

        spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_UI_CURVE);
        pre.has_commands = true;

        // [GPU 29.10.2026] where the two GUI copies differ: the eye passes skip the interface work everywhere else (not made
        // in the GPU pass test's variant without it)
        gui_map_ok = !settings.test_no_gui_map && m_gui_map.dispatch(device, cmd, centre_hud, centre_fin);

        if (!gui_map_ok && !settings.test_no_gui_map) {
            static bool once = true;

            if (once) {
                REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: GUI map not made ({}), the interface is tested on every pixel", m_gui_map.last_error()));
                once = false;
            }
        }

        // [NO INTERFACE fix118 21.11.2026] ... made before the wide view is cleaned (it was made after: the two do not
        // depend on each other), so that the cleaning has this frame's map: when no block is marked the two copies hold the
        // same image in every texel and every pixel of the cleaning would leave with its colour after reading both copies;
        // with the map it leaves before reading them (see UiClean's shader). The same image, in every frame.
        ID3D12Resource* const clean_map = gui_map_ok && m_gui_map.frame_texel_ok() && !settings.test_clean_no_map ? m_gui_map.map() : nullptr;
        const auto cd = wide_color->GetDesc();
        const auto clean_format = d3d12::engine_copy_uav_format(cd.Format);

        bool made = false;

        // ([COPIES fix116] GPU test 58: the wide view goes to its anti-aliasing as the engine drew it; the same picture while
        // it holds no interface)
        if (!settings.test_ui_idle && clean_format != DXGI_FORMAT_UNKNOWN &&
            ensure_uav_image(device, m_qv_wide_clean, (uint32_t)cd.Width, cd.Height, clean_format, L"RE4VR shared wide view without interface", &made)) {
            auto* clean = m_qv_wide_clean.Get();

            if (made) {
                m_qv_wide_clean_psr = false; // [01.11.2026] a new image rests in UNORDERED_ACCESS
            }

            if (m_qv_wide_clean_psr) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = clean;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = ENGINE_STATE;
                b.Transition.StateAfter = UAV;
                cmd->ResourceBarrier(1, &b);
                m_qv_wide_clean_psr = false;
            }

            if (m_ui_clean.dispatch(device, cmd, wide_color, ENGINE_STATE, centre_hud, centre_fin, clean, m_ui_curve.table(), false, true, clean_map,
                    m_gui_map.stamp())) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = clean;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = UAV;
                b.Transition.StateAfter = ENGINE_STATE;
                cmd->ResourceBarrier(1, &b);
                m_qv_wide_clean_psr = true;
                wide_input = clean;
                pre.has_commands = true;
            }
        }
    } else {
        spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_UI_CURVE);
    }

    spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_UI_CLEAN);

    // ---- 1. the shared wide view: temporal AA once (its own slot 0 history) -----------------------------------------
    const auto wide_jitter_index = (size_t)(std::min)((std::max)(get_evaluate_id(0), 1u) - 1u, 3u);
    const auto wide_jitter = jitter_of_frame(wide_jitter_index, (int64_t)(uint32_t)frame); // [JITTER LATCH 10.11.2026]

    // [MENUS fix86 18.11.2026] RE7 test of fix85 (18.11.2026 20:4x): in the periphery the save menu's lines were dashes and
    // its letters pieces, others every frame (video with the head still: the picture's alternating part 5.3 per mille on
    // the menu, 1.5 on the scene behind it). The eye pass took the wide view's interface from this frame's images: one
    // sample per 4 x 4 eye pixels at the frame's jitter. It now gets them collected over the frames (d3d12::UiSteady: the
    // colour and both copies, twice the size, without jitter); if that pass cannot run, the frame's own as before.
    bool ui_steady = false;
    bool ui_around = false; // [fix94]

    // [UI MOTION fix125 22.11.2026] the interface's own motion of this frame for the wide view's pass below and the fovea
    // views' anti-aliasing (the report every 64th frame and for a dump: it is text for the log)
    // [fix130 22.11.2026] RE4 GPU test of 22.11.2026 22:3x (fix129, the weapon's HUD in the picture, 5 phases in one session):
    // with the interface's own motion the two fovea passes cost 0.031 ms more than along the scene's motion vectors (0.341 /
    // 0.322 against 0.326 / 0.306 ms; the runs of the build within 0.009), nothing anywhere else. The game draws three or
    // four interface elements in every frame, with or without anything of them on the screen (log of the same session: the
    // passes ran with it in 43 reports of 43, the map of the wide view's interface empty for 922 frames meanwhile), so
    // that was paid in every frame of the game. Not while the wide view has shown no interface for over a second: the
    // map's count, read back a few frames late, as for the wide view's pass below (a HUD that comes up goes along the
    // scene's motion vectors for those few frames, as every element does in its first frame). Without the map: as before.
    std::array<d3d12::UiMotion::Block, 3> um_blocks{};
    const bool um_idle = gui_map_ok && m_gui_map.idle_run() >= 120;
    const bool um_on = settings.ui_motion && !settings.test_no_ui_motion && steady_ui && !um_idle;

    if (um_on) {
#if REVR_RELEASE // [RELEASE SWITCH fix141] without the report (for the log only)
        um_blocks = ui_motion_blocks(device, frame, false);
#else
        um_blocks = ui_motion_blocks(device, frame, dump || (m_qv_frame & 63) == 0);
#endif
    }

    // (not while the wide view has shown no interface for over a second: the pass has nothing to collect then; the count
    // comes a few frames late, the eye passes take the frame's own images until it runs again)
    if (centre_ui && steady_ui && settings.ui_steady && !m_ui_steady_failed && wide_mv != nullptr && gui_map_ok && m_gui_map.idle_run() < 120) {
        d3d12::UiSteady::Params us{};
        // (the motion vectors' convention as for StableDepth below)
        us.mv_scale_x = m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u);
        us.mv_scale_y = m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u);
        us.jitter_x = wide_jitter[0];
        us.jitter_y = wide_jitter[1];
        us.reset = m_ui_steady_frame + 1 != m_qv_frame;
        us.weight_max = 4.0f;
        // [fix94 19.11.2026] only around the interface, if asked for. Not in a game menu with its backdrop fix: the eye pass
        // then reads the collected colour over the whole fovea rectangle (EyeCompose's wide_block_luma), everywhere else
        // only in the map's marked blocks.
        us.only_around = settings.ui_steady_around && !settings.menu_backdrop;
        us.ui_motion = um_blocks[0]; // [UI MOTION fix125] the wide view's interface along its own motion

        if (m_ui_steady.dispatch(device, cmd, wide_color, ENGINE_STATE, centre_hud, centre_fin, wide_mv, ENGINE_STATE, &m_gui_map, us) && m_ui_steady.color() != nullptr &&
            m_ui_steady.hud() != nullptr && m_ui_steady.fin() != nullptr) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
            static bool once = true;

            if (once) {
                const auto sd = m_ui_steady.color()->GetDesc();
                REVR_LOG(spdlog::info("[TemporalUpscaler] mono periphery: the wide view's interface is collected over the frames for the periphery ({}x{})",
                    (uint32_t)sd.Width, sd.Height));
                once = false;
            }
#endif

            ui_steady = true;
            ui_around = us.only_around;
            m_ui_steady_frame = m_qv_frame;
            pre.has_commands = true;
        } else {
            m_ui_steady_failed = true;
            REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: the interface could not be collected over the frames ({}): the eye passes take the "
                         "frame's own images",
                m_ui_steady.last_error()));
        }
    }

    ID3D12Resource* wide_src = periphery_taa(device, cmd, 0, wide_input, wide_mv, wide_depth, settings, wide_jitter[0], wide_jitter[1]);

    if (wide_src == nullptr) {
        ++qv.skipped;
        pre_end();
        return false;
    }

    spans.stamp(cmd, (uint64_t)frame, d3d12::GpuSpans::PRE_WIDE_TAA);
    const auto wide_src_state = UAV;
    // [PERIPHERY SUPER fix80 17.11.2026] what periphery_taa returned: the light TAA's image (linear when the colour view
    // decodes SRGB) or the supersampling TAA's (display encoded), the latter with wide_ss x wide_ss texels per pixel of
    // the wide view when it is the image of twice the size
    const bool wide_linear = m_periphery_linear;
    const uint32_t wide_ss = m_periphery_ss >= 2 ? 2u : 1u;
    pre.has_commands = true;

    // ---- 2. reprojection to each eye (its share of the wide frustum, same pixel density) ----------------------------
    const auto wd = wide_src->GetDesc();

    // [IMAGE QUALITY 15.11.2026] the depth (and motion) of the wide view at another size than its colour (RE7: the game
    // camera drawn at 2/3 of the size in the video tape, see CameraDuplicator::apply_image_quality): the reprojection and
    // the periphery's TAA read them at the colour's place (FoveaComposite.cpp, wide_depth_at). Logged when it starts and
    // when it ends.
    {
        const auto dd = wide_depth->GetDesc();
        const bool mismatch = dd.Width != wd.Width / wide_ss || dd.Height != wd.Height / wide_ss;

        if (mismatch != m_wide_depth_size_mismatch) {
            m_wide_depth_size_mismatch = mismatch;
            REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: wide view depth {}x{}, colour {}x{}{}", (uint64_t)dd.Width, dd.Height,
                (uint64_t)wd.Width / wide_ss, wd.Height / wide_ss,
                mismatch ? " (sizes differ: depth and motion read at the colour's place)" : " (sizes match again)"));
        }
    }
    // (the wide view's own size: the grid the reprojection searches on, whatever the size of the image it reads colour from)
    const float wide_w = (float)(wd.Width / wide_ss);
    const float wide_h = (float)(wd.Height / wide_ss);
    const float wide_tan_w = fr.wide_r - fr.wide_l;
    const float wide_tan_h = fr.wide_t - fr.wide_b;

    if (wide_tan_w <= 1e-4f || wide_tan_h <= 1e-4f) {
        ++qv.skipped;
        set_qv_status("mono periphery: degenerate frustum");
        pre_end();
        return false;
    }

    const float px_per_tan_x = wide_w / wide_tan_w; // the wide view's pixel density
    const float px_per_tan_y = wide_h / wide_tan_h;
    // distance from the depth value, rows 2 and 3 of the inverse projection (they do not depend on x / y). The engine
    // stores the depth reversed relative to the projection we hand it.
    const glm::mat4 reverse_z{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    const auto inv_proj = glm::inverse(reverse_z * vr->get_union_projection_matrix());

    // [STABLE DEPTH fix81 17.11.2026] RE7 test of fix80 (17.11.2026): the periphery flickered a lot whatever its
    // anti-aliasing. The reprojection got the frame's own depth, sampled with the frame's jitter: at the outline of
    // every nearer thing the pixels changed between the near thing's distance and the background's from frame to frame
    // and landed several pixels apart (see d3d12::StableDepth). It now gets the distances steadied over time; if that
    // pass cannot run, the frame's depth as before.
    ID3D12Resource* reproject_depth = wide_depth;
    auto reproject_depth_state = ENGINE_STATE;
    bool stable_depth = false;

    if (settings.stable_depth && !m_stable_depth_failed && wide_mv != nullptr) {
        d3d12::StableDepth::Params sd{};
        // same convention the upscaler gets: pixels = mv * m_motion_scale -> UV = mv * m_motion_scale / render size
        sd.mv_scale_x = m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u);
        sd.mv_scale_y = m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u);
        sd.depth_a = inv_proj[2][2];
        sd.depth_b = inv_proj[3][2];
        sd.depth_c = inv_proj[2][3];
        sd.depth_e = inv_proj[3][3];
        sd.dist_min = 0.12f;
        sd.reset = m_stable_depth_frame + 1 != m_qv_frame;

        if (m_stable_depth.dispatch(device, cmd, wide_depth, ENGINE_STATE, wide_mv, ENGINE_STATE, sd) && m_stable_depth.image() != nullptr) {
            reproject_depth = m_stable_depth.image();
            reproject_depth_state = UAV;
            stable_depth = true;
            m_stable_depth_frame = m_qv_frame;
        } else {
            m_stable_depth_failed = true;
            REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: the steady distances could not be made ({}): the reprojection takes the frame's depth",
                m_stable_depth.last_error()));
        }
    }

    m_stable_depth_used = stable_depth;

    // the eye's periphery image: its frustum at the wide view's density (eye_wide_w/h of the layout is the same thing
    // computed from the layout; the actual frusta decide here)
    std::array<ID3D12Resource*, 2> eye_wide{};
    bool reproject_ok = true;

    std::array<d3d12::MonoPeriphery::Params, 2> mp{};

    for (int eye = 0; eye < 2 && reproject_ok; ++eye) {
        const float eye_tan_w = fr.eye_r[eye] - fr.eye_l[eye];
        const float eye_tan_h = fr.eye_t[eye] - fr.eye_b[eye];
        const uint32_t ew = (std::max)(8u, (uint32_t)std::lround(eye_tan_w * px_per_tan_x));
        const uint32_t eh = (std::max)(8u, (uint32_t)std::lround(eye_tan_h * px_per_tan_y));

        // [PERIPHERY SUPER fix80 17.11.2026] with the image of twice the size the eye's periphery gets 2 x 2 pixels per ray
        // (an image of its own, kept beside the other: the two never change size while lists that use them are in flight)
        auto& eye_image = wide_ss == 2 ? m_qv_mono_eye_wide2[eye] : m_qv_mono_eye_wide[eye];

        if (!ensure_uav_image(device, eye_image, ew * wide_ss, eh * wide_ss, DXGI_FORMAT_R16G16B16A16_FLOAT,
                wide_ss == 2 ? (eye == 0 ? L"RE4VR mono periphery left (2 x 2 per ray)" : L"RE4VR mono periphery right (2 x 2 per ray)")
                             : (eye == 0 ? L"RE4VR mono periphery left" : L"RE4VR mono periphery right"))) {
            set_qv_status("mono periphery: could not create the eye periphery image");
            qv.mark_failed(m_qv_init_generation, "mono periphery: could not create the eye periphery image");
            reproject_ok = false;
            break;
        }

        auto& e = mp[eye];
        // output pixel centre -> source pixel of the same ray at infinity (both images are linear in the tangent)
        e.x_scale = (eye_tan_w / (float)ew) * px_per_tan_x;
        e.x_offset = (fr.eye_l[eye] - fr.wide_l) * px_per_tan_x;
        e.y_scale = (eye_tan_h / (float)eh) * px_per_tan_y;
        e.y_offset = (fr.wide_t - fr.eye_t[eye]) * px_per_tan_y;
        // a surface at distance d: the eye (offset x_e from the wide camera) sees it at tan + x_e / d -> its source
        // column lies at this ray's column + (pixels per tangent) * x_e / d
        e.disparity = px_per_tan_x * fr.eye_x[eye];
        e.dist_min = 0.12f;
        e.max_shift = 96;
        e.depth_a = inv_proj[2][2];
        e.depth_b = inv_proj[3][2];
        e.depth_c = inv_proj[2][3];
        e.depth_e = inv_proj[3][3];

        if (stable_depth) {
            d3d12::StableDepth::as_depth_of(e); // [STABLE DEPTH fix81] its image holds 1 / distance
        }

        e.mark_holes = settings.debug_view == 4;
        e.fill_anchored = settings.hole_fill;
        e.soft_outlines = stable_depth && wide_ss == 2 && settings.soft_outlines; // [OUTLINES fix84 18.11.2026]
        e.ss = wide_ss;
        eye_wide[eye] = eye_image.Get();
    }

    // [GPU 29.10.2026] both eyes in one call; the faster search (same result) unless the GPU test asks for the original pass
    // or round 2's passes
    if (reproject_ok) {
        ID3D12Resource* const outs[2]{eye_wide[0], eye_wide[1]};
        const auto stamp = [&](int k) {
            spans.stamp(cmd, (uint64_t)frame,
                k == 0 ? d3d12::GpuSpans::PRE_BLOCK_MIN : k == 1 ? d3d12::GpuSpans::PRE_REPROJECT_1 : d3d12::GpuSpans::PRE_REPROJECT);
        };

        const auto search = settings.test_original_reprojection ? d3d12::MonoPeriphery::Search::Original
                            : settings.test_round2_passes    ? d3d12::MonoPeriphery::Search::TwoPassRound2
                            : settings.test_bmin_texture     ? d3d12::MonoPeriphery::Search::TwoPassRound3BminTexture
                                                             : d3d12::MonoPeriphery::Search::TwoPassRound3;

        if (!m_mono_periphery.reproject(device, cmd, wide_src, wide_src_state, reproject_depth, reproject_depth_state, outs, mp.data(), search,
                (uint64_t)frame, stamp)) {
            const std::string why = std::string{"mono periphery reprojection failed: "} + m_mono_periphery.last_error();
            set_qv_status(why);
            qv.mark_failed(m_qv_init_generation, why);
            reproject_ok = false;
        } else {
            pre.has_commands = true;
            qv.reprojections += 2;
        }
    }

    if (!reproject_ok) {
        ++qv.skipped;
        pre_end();
        return false;
    }

    // [CAPTURE fix81 17.11.2026] the periphery's chain of this frame for a running capture: the wide view's inputs, its
    // anti-aliased image, the distances the reprojection got and the left eye's periphery; in the first frames the
    // interface's images of the wide view too. tests/supertaa/analyze_chain.py reads them.
    // [CAPTURE fix82 17.11.2026] The chain was taken around the place looked at: inside the fovea rectangle, where the eye
    // image shows the fovea view and never the periphery, and its corner followed the gaze from frame to frame (the
    // captures of 17.11.2026 16:0x: steps of two pixels of the wide view in a third of the frames). Now around a point of
    // the left eye's image beside the fovea rectangle (left of it when there is room, else right; at its height), the
    // same for all frames of a capture: what is recorded is periphery the eye image shows (evaluate_quadview_eye adds the
    // finished eye image there, eye_out_side).
    if (m_sc.recording()) {
        quadview::Rect crect{};
        const bool have_crect = qv.rect_for_frame(0, (int64_t)(uint32_t)frame, crect);
        const float eye_w = (float)(std::max)(m_qv_layout.display_w, 1u);
        const float eye_h = (float)(std::max)(m_qv_layout.display_h, 1u);

        if (m_sc.frame() == 0) {
            const float half = 0.5f * (float)m_sc.part() + 64.0f; // (half the part and a margin for the blend band's outside)
            const float left = (float)crect.x - half;
            const float right = (float)crect.x + (float)crect.w + half;
            m_sc_side[0] = !have_crect ? 0.25f * eye_w : left - half >= 0.0f ? left : (std::min)(right, eye_w - half);
            m_sc_side[1] = have_crect ? (float)crect.y + 0.5f * (float)crect.h : 0.5f * eye_h;
        }

        const float ecx = m_sc_side[0];
        const float ecy = m_sc_side[1];
        const float tx = fr.eye_l[0] + (fr.eye_r[0] - fr.eye_l[0]) * ecx / eye_w;
        const float ty = fr.eye_t[0] + (fr.eye_b[0] - fr.eye_t[0]) * ecy / eye_h;
        // (the wide view's pixel of that direction for a far point; nearer things lie disparity / distance beside it)
        const float wxc = (tx - fr.wide_l) * px_per_tan_x;
        const float wyc = (fr.wide_t - ty) * px_per_tan_y;
        const auto corner = [](float centre, uint32_t size, uint32_t limit) {
            const int32_t top = (int32_t)(limit > size ? limit - size : 0u);
            return (uint32_t)std::clamp((int32_t)std::lround(centre - 0.5f * (float)size), 0, top) & ~1u;
        };
        const uint32_t ww = (uint32_t)wide_w;
        const uint32_t wh = (uint32_t)wide_h;
        const uint32_t pw = (std::min)((m_sc.part() / 4u + 32u) & ~1u, ww & ~1u);
        const uint32_t ph = (std::min)((m_sc.part() / 4u + 32u) & ~1u, wh & ~1u);
        const uint32_t px = corner(wxc, pw, ww);
        const uint32_t py = corner(wyc, ph, wh);
        // the same part of the wide view's depth and motion (they may have another size, read at the colour's place)
        const auto scaled = [&](ID3D12Resource* res, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
            const auto d = res->GetDesc();
            x = (uint32_t)((uint64_t)px * d.Width / ww);
            y = (uint32_t)((uint64_t)py * d.Height / wh);
            w = (uint32_t)((uint64_t)pw * d.Width / ww);
            h = (uint32_t)((uint64_t)ph * d.Height / wh);
        };
        uint32_t qx = 0, qy = 0, qw = 0, qh = 0;
        m_sc.extra(device, cmd, wide_input, ENGINE_STATE, "wide_color", px, py, pw, ph);

        if (wide_mv != nullptr) {
            scaled(wide_mv, qx, qy, qw, qh);
            m_sc.extra(device, cmd, wide_mv, ENGINE_STATE, "wide_motion", qx, qy, qw, qh);
        }

        scaled(wide_depth, qx, qy, qw, qh);
        m_sc.extra(device, cmd, wide_depth, ENGINE_STATE, "wide_depth", qx, qy, qw, qh,
            [&](ID3D12Resource* depth, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                d3d12::FoveaLinear::Region r{};
                r.w = w;
                r.h = h;
                r.shift_x = (int32_t)x;
                r.shift_y = (int32_t)y;
                return m_fovea_linear.copy_part(device, cmd, depth, ENGINE_STATE, target, r, true);
            });

        if (stable_depth) {
            m_sc.extra(device, cmd, reproject_depth, UAV, "wide_stable", qx, qy, qw, qh);
        }

        m_sc.extra(device, cmd, wide_src, UAV, "wide_taa", px * wide_ss, py * wide_ss, pw * wide_ss, ph * wide_ss);

        // the left eye's periphery: the rays of the same directions (its pixel x shows the wide view's column
        // x * x_scale + x_offset for a far point)
        uint32_t ex = 0, ey = 0, ew = 0, eh = 0;

        if (eye_wide[0] != nullptr) {
            const auto ed = eye_wide[0]->GetDesc();
            const uint32_t rays_w = (uint32_t)ed.Width / wide_ss;
            const uint32_t rays_h = ed.Height / wide_ss;
            ew = (std::min)(pw, rays_w & ~1u);
            eh = (std::min)(ph, rays_h & ~1u);
            ex = corner((wxc - mp[0].x_offset) / (std::max)(mp[0].x_scale, 1e-6f), ew, rays_w);
            ey = corner((wyc - mp[0].y_offset) / (std::max)(mp[0].y_scale, 1e-6f), eh, rays_h);
            m_sc.extra(device, cmd, eye_wide[0], UAV, "eye_periphery", ex * wide_ss, ey * wide_ss, ew * wide_ss, eh * wide_ss);
        }

        // the interface's images of the wide view (first frames only: they are large)
        if (m_sc.frame() < 8 && centre_ui) {
            m_sc.extra(device, cmd, wide_color, ENGINE_STATE, "ui_wide_color", px, py, pw, ph);
            const auto part_of = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES state, const char* name) {
                if (res != nullptr) {
                    uint32_t x = 0, y = 0, w = 0, h = 0;
                    scaled(res, x, y, w, h);
                    m_sc.extra(device, cmd, res, state, name, x, y, w, h);
                }
            };
            part_of(centre_hud, D3D12_RESOURCE_STATE_COMMON, "ui_wide_hud");
            part_of(centre_fin, D3D12_RESOURCE_STATE_COMMON, "ui_wide_fin");
        }

        m_sc.note(std::format("wide: engine_frame {} view {} {} part {} {} {} {} ss {} jitter {:.9g} {:.9g} mv_scale {:.9g} {:.9g} depth_k {:.9g} {:.9g} {:.9g} "
                              "{:.9g} periphery_aa {} linear {} stable_depth {} wide_tan {:.9g} {:.9g} {:.9g} {:.9g} px_per_tan {:.9g} {:.9g}",
            frame, ww, wh, px, py, pw, ph, wide_ss, wide_jitter[0], wide_jitter[1], m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u),
            m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u), inv_proj[2][2], inv_proj[3][2], inv_proj[2][3], inv_proj[3][3],
            m_periphery_running.load(std::memory_order_relaxed), wide_linear ? 1 : 0, stable_depth ? 1 : 0, fr.wide_l, fr.wide_r, fr.wide_b, fr.wide_t,
            px_per_tan_x, px_per_tan_y));
        m_sc.note(std::format("eye_periphery: eye 0 rays {} {} part {} {} {} {} x_scale {:.9g} x_offset {:.9g} y_scale {:.9g} y_offset {:.9g} disparity {:.9g} "
                              "dist_min {:.9g} max_shift {} fill {} eye_tan {:.9g} {:.9g} {:.9g} {:.9g} eye_image {} {} side_centre {:.2f} {:.2f} fovea_rectangle {} "
                              "{} {} {} {}",
            eye_wide[0] != nullptr ? (uint32_t)eye_wide[0]->GetDesc().Width / wide_ss : 0u, eye_wide[0] != nullptr ? eye_wide[0]->GetDesc().Height / wide_ss : 0u,
            ex, ey, ew, eh, mp[0].x_scale, mp[0].x_offset, mp[0].y_scale, mp[0].y_offset, mp[0].disparity, mp[0].dist_min, mp[0].max_shift,
            mp[0].fill_anchored ? 1 : 0, fr.eye_l[0], fr.eye_r[0], fr.eye_b[0], fr.eye_t[0], m_qv_layout.display_w, m_qv_layout.display_h, ecx, ecy,
            have_crect ? 1 : 0, crect.x, crect.y, crect.w, crect.h));
        pre.has_commands = true;
    }


    // ---- 3. the fovea views without their interface (the fovea layers draw the GUI with their own narrow frustum; it
    //         goes back per eye at the end, MonoUi) ---------------------------------------------------------------------
    std::array<ID3D12Resource*, 2> fovea_clean{};
    std::array<ID3D12Resource*, 2> fovea_hud{};
    std::array<ID3D12Resource*, 2> fovea_fin{};

    // [MENUS fix83 17.11.2026] with the interface inside the fovea image: the fovea view's two interface copies for the eye
    // pass, which tells by them where the view drew interface (EyeCompose::Params::fovea_ui_inside)
    std::array<ID3D12Resource*, 2> inside_hud{};
    std::array<ID3D12Resource*, 2> inside_fin{};

    for (int eye = 0; eye < 2; ++eye) {
        const auto& inset = m_eye_states[1 + eye];
        ID3D12Resource* inset_color = native(inset.color_copy);
        ID3D12Resource* hudless = nullptr;
        ID3D12Resource* final_tex = nullptr;
        void* ui_layer = nullptr;

        if (inset.scene_layer == nullptr || inset_color == nullptr || !vr->ui_capture_pair(eye, hudless, final_tex, ui_layer) ||
            ui_layer != (void*)inset.scene_layer.get()) {
            continue; // no interface copies of this view yet: the fovea view is used as it is
        }

        // [MENUS fix82 17.11.2026] the fovea views go to the mod's anti-aliasing with their interface (see ui_inside): nothing
        // is taken out of them ([fix83] the eye pass gets their two copies only to tell where the interface is)
        if (ui_inside) {
            inside_hud[eye] = hudless;
            inside_fin[eye] = final_tex;
            continue;
        }

        fovea_hud[eye] = hudless;
        fovea_fin[eye] = final_tex;

        const auto cd = inset_color->GetDesc();
        const auto clean_format = d3d12::engine_copy_uav_format(cd.Format);

        // only a format a compute shader writes as the plugin reads it (an SRGB copy would need a typeless image)
        if (clean_format == DXGI_FORMAT_UNKNOWN) {
            static bool once = true;

            if (once) {
                REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: fovea colour format {} cannot be cleaned of the interface", (int)cd.Format));
                once = false;
            }

            continue;
        }

        bool made = false;

        if (!ensure_uav_image(device, m_qv_fovea_clean[eye], (uint32_t)cd.Width, cd.Height, clean_format, eye == 0 ? L"RE4VR fovea input left" : L"RE4VR fovea input right",
                &made)) {
            continue;
        }

        auto* clean = m_qv_fovea_clean[eye].Get();

        if (made) {
            m_qv_fovea_clean_psr[eye] = false; // [01.11.2026] a new image rests in UNORDERED_ACCESS
        }

        // the image rests like an engine copy between frames so the plugin reads it as one; written as a UAV here
        if (m_qv_fovea_clean_psr[eye]) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = clean;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = ENGINE_STATE;
            b.Transition.StateAfter = UAV;
            cmd->ResourceBarrier(1, &b);
            m_qv_fovea_clean_psr[eye] = false;
        }

        // its copy with GUI is not reliable (the engine reuses that target between the fovea views): the interface pixels
        // are found from its colour against the copy without GUI
        // [LIGHTS 11.11.2026] with this frame's GuiMap: nothing taken for interface when the wide view drew none
        ID3D12Resource* const frame_map = gui_map_ok && m_gui_map.frame_texel_ok() ? m_gui_map.map() : nullptr;

        if (!m_ui_clean.dispatch(device, cmd, inset_color, ENGINE_STATE, hudless, final_tex, clean, m_ui_curve.table(), true, !settings.test_round2_passes,
                frame_map, m_gui_map.stamp())) {
            static bool once = true;

            if (once) {
                REVR_LOG(spdlog::warn("[TemporalUpscaler] mono periphery: interface clean-up failed: {}", m_ui_clean.last_error()));
                once = false;
            }

            continue;
        }

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = clean;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = UAV;
        b.Transition.StateAfter = ENGINE_STATE;
        cmd->ResourceBarrier(1, &b);
        m_qv_fovea_clean_psr[eye] = true;
        fovea_clean[eye] = clean;
        pre.has_commands = true;
    }

    // ---- 3b. [FOVEA LINEAR 05.11.2026] the upscaler plugin's input in linear light (d3d12::FoveaLinear): decoded from the
    //          fovea view without interface (or the engine copy), resting like an engine copy for the plugin ---------------
    std::array<ID3D12Resource*, 2> fovea_lin{};

    // [FSR DIRECT 06.11.2026] FSR 4 called by the mod: its colour input written here (linear light; the encoded image with
    // the option off), into the two halves of the side-by-side image or into one image per eye; its depth and motion and
    // its passes follow on this list after its end stamp (fsr_direct_record). The inputs rest in the read state between
    // frames, here back to UNORDERED_ACCESS for the writes.
    const bool fsr_on = m_qv_fsr && settings.fovea_mode == quadview::FoveaMode::Upscaler && settings.debug_view != 1;
    std::array<ID3D12Resource*, 2> fsr_color{};
    bool fsr_inputs = false;
    bool fsr_broken = false; // a pass failed for good (not just inputs missing this frame): the plugin from the next set-up on

    if (fsr_on) {
        constexpr auto READ = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool atlas = m_qv_fsr_atlas.valid;
        const auto rw = m_qv_layout.render_w;
        const auto rh = m_qv_layout.render_h;
        std::array<ID3D12Resource*, 2> src{};

        for (int eye = 0; eye < 2; ++eye) {
            src[eye] = fovea_clean[eye] != nullptr ? fovea_clean[eye] : native(m_eye_states[1 + eye].color_copy);
        }

        fsr_inputs = src[0] != nullptr && src[1] != nullptr && native(m_eye_states[1].depth_copy) != nullptr && native(m_eye_states[2].depth_copy) != nullptr &&
                     native(m_eye_states[1].motion_vectors_copy) != nullptr && native(m_eye_states[2].motion_vectors_copy) != nullptr;

        for (int eye = 0; fsr_inputs && eye < 2; ++eye) {
            const auto sd = src[eye]->GetDesc();

            if (sd.Width < rw || sd.Height < rh) {
                fsr_inputs = false; // (a frame while the views change size)
            }
        }

        // [FSR DIRECT 09.11.2026] the depth and motion copies too: in the log of 09.11.2026 the engine's targets were cloned
        // again (after the headset's dashboard) and their side-by-side copy found them smaller than the layout; that counted
        // as FSR 4 failing for good and the plugin took over for the rest of the session. A frame without FSR 4 instead.
        for (int eye = 0; fsr_inputs && eye < 2; ++eye) {
            for (auto* r : {native(m_eye_states[1 + eye].depth_copy), native(m_eye_states[1 + eye].motion_vectors_copy)}) {
                const auto d = r->GetDesc();

                if (d.Width < rw || d.Height < rh) {
                    if (!m_fsr_inputs_small_logged) {
                        REVR_LOG(spdlog::warn("[TemporalUpscaler] FSR 4 by the mod: depth / motion {}x{} smaller than the views {}x{}: frames without FSR 4 "
                                     "until they match",
                            (uint64_t)d.Width, d.Height, rw, rh));
                        m_fsr_inputs_small_logged = true;
                    }

                    fsr_inputs = false;
                    break;
                }
            }
        }

        if (fsr_inputs) {
            m_fsr_inputs_small_logged = false;
        }

        if (fsr_inputs && m_qv_fsr_in_read) {
            std::array<ID3D12Resource*, 6> mine{};

            if (atlas) {
                mine = {m_qv_fsr_color.Get(), m_qv_fsr_depth.Get(), m_qv_fsr_motion.Get(), nullptr, nullptr, nullptr};
            } else {
                mine = {m_qv_fovea_lin[0].Get(), m_qv_fovea_lin[1].Get(), m_qv_fsr_depth_eye[0].Get(), m_qv_fsr_depth_eye[1].Get(),
                    m_qv_fsr_motion_eye[0].Get(), m_qv_fsr_motion_eye[1].Get()};
            }

            for (auto* r : mine) {
                if (r != nullptr) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = r;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = READ;
                    b.Transition.StateAfter = UAV;
                    cmd->ResourceBarrier(1, &b);
                }
            }

            m_qv_fsr_in_read = false;
        }

        for (int eye = 0; fsr_inputs && !fsr_broken && eye < 2; ++eye) {
            d3d12::FoveaLinear::Region r{};
            r.w = rw;
            r.h = rh;
            r.hi_x = (int32_t)rw - 1;
            r.hi_y = (int32_t)rh - 1;
            ID3D12Resource* target = nullptr;

            if (atlas) {
                const auto f = quadview::atlas_fill(m_qv_fsr_atlas, rw, eye);
                r.out_x = f.out_x;
                r.w = f.w;
                r.shift_x = f.shift;
                target = m_qv_fsr_color.Get();
            } else {
                // a new image rests in UNORDERED_ACCESS like the others now
                if (!ensure_uav_image(device, m_qv_fovea_lin[eye], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        eye == 0 ? L"RE4VR FSR 4 colour left" : L"RE4VR FSR 4 colour right")) {
                    fsr_broken = true;
                    break;
                }

                target = m_qv_fovea_lin[eye].Get();
            }

            if (!m_fovea_linear.decode_part(device, cmd, src[eye], ENGINE_STATE, target, r, settings.upscaler_linear)) {
                REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod: colour input failed ({})", m_fovea_linear.last_error()));
                fsr_broken = true;
                break;
            }

            fsr_color[eye] = target;
            pre.has_commands = true;
        }

        fsr_inputs = fsr_inputs && !fsr_broken;
    }

    const bool want_linear = !m_qv_fsr && settings.fovea_mode == quadview::FoveaMode::Upscaler && settings.upscaler_linear && settings.debug_view != 1 &&
                             m_qv_fovea_float_out && !m_fovea_linear_off;

    for (int eye = 0; want_linear && eye < 2; ++eye) {
        ID3D12Resource* src = fovea_clean[eye] != nullptr ? fovea_clean[eye] : native(m_eye_states[1 + eye].color_copy);

        if (src == nullptr) {
            continue;
        }

        const auto sd = src->GetDesc();
        bool made = false;

        if (!ensure_uav_image(device, m_qv_fovea_lin[eye], (uint32_t)sd.Width, sd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                eye == 0 ? L"RE4VR fovea input linear left" : L"RE4VR fovea input linear right", &made)) {
            m_fovea_linear_off = true;
            break;
        }

        auto* lin = m_qv_fovea_lin[eye].Get();

        if (made) {
            m_qv_fovea_lin_psr[eye] = false; // a new image rests in UNORDERED_ACCESS
        }

        if (m_qv_fovea_lin_psr[eye]) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = lin;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = ENGINE_STATE;
            b.Transition.StateAfter = UAV;
            cmd->ResourceBarrier(1, &b);
            m_qv_fovea_lin_psr[eye] = false;
        }

        if (!m_fovea_linear.decode(device, cmd, src, ENGINE_STATE, lin)) {
            REVR_LOG(spdlog::warn("[TemporalUpscaler] fovea linear light: decode failed ({}), the upscaler gets the encoded image", m_fovea_linear.last_error()));
            m_fovea_linear_off = true;
            break;
        }

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = lin;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = UAV;
        b.Transition.StateAfter = ENGINE_STATE;
        cmd->ResourceBarrier(1, &b);
        m_qv_fovea_lin_psr[eye] = true;
        fovea_lin[eye] = lin;
        pre.has_commands = true;
    }

    if (m_fovea_linear_off) {
        fovea_lin = {}; // (the images already decoded this frame are not used)
    }

    if (dump) {
        m_qv_dump.add(device, cmd, wide_color, ENGINE_STATE, "C_1_input_wide_color");
        m_qv_dump.add(device, cmd, wide_depth, ENGINE_STATE, "C_1_input_wide_depth");
        m_qv_dump.add(device, cmd, wide_mv, ENGINE_STATE, "C_1_input_wide_motion");

        if (wide_src != wide_color) {
            m_qv_dump.add(device, cmd, wide_src, wide_src_state,
                m_periphery_running.load(std::memory_order_relaxed) != 0 ? "C_3t_wide_supersampling_taa_output" : "C_3t_wide_light_taa_output");
        }

        if (wide_input != wide_color) {
            m_qv_dump.add(device, cmd, wide_input, ENGINE_STATE, "C_2c_wide_without_interface");
        }

        // [PERIPHERY DUMP fix84 18.11.2026] the periphery's chain whole and as stored, for analysis off the game: the wide
        // view's colour, depth and motion (raw), the depth the reprojection got (the stable depth's four values, raw) and
        // the anti-aliased image (16-bit PPM); per eye below the reprojected periphery and, for the left eye, the whole
        // finished image. With "periphery only" the finished image is the periphery alone. Both frames of a dump.
        if (settings.dump_periphery_full) {
            constexpr uint32_t ALL = 65535u; // (clipped to the image)
            m_qv_dump.add_raw("C_1_input_wide_color", 0, 0, ALL, ALL);
            m_qv_dump.add_raw("C_1_input_wide_depth", 0, 0, ALL, ALL);
            m_qv_dump.add_raw("C_1_input_wide_motion", 0, 0, ALL, ALL);

            if (wide_src != wide_color) {
                m_qv_dump.add_crop(m_periphery_running.load(std::memory_order_relaxed) != 0 ? "C_3t_wide_supersampling_taa_output" : "C_3t_wide_light_taa_output",
                    0, 0, ALL, ALL);
            }

            if (reproject_depth != wide_depth && reproject_depth != nullptr) {
                m_qv_dump.add(device, cmd, reproject_depth, reproject_depth_state, "C_2s_wide_stable_depth");
                m_qv_dump.add_raw("C_2s_wide_stable_depth", 0, 0, ALL, ALL);
            }
        }

        // [MONO UI] the engine's copies without / with GUI of each view (where the interface is taken from)
        if (centre_ui) {
            m_qv_dump.add(device, cmd, centre_hud, D3D12_RESOURCE_STATE_COMMON, "C_ui_copy_without_gui");
            m_qv_dump.add(device, cmd, centre_fin, D3D12_RESOURCE_STATE_COMMON, "C_ui_copy_with_gui");

            // [MENUS fix86 18.11.2026] what the eye passes get of them for the periphery; with the chain whole, all of them
            // as stored (the copies: 8 MB, the collected images 31 MB each)
            if (ui_steady) {
                m_qv_dump.add(device, cmd, m_ui_steady.color(), ENGINE_STATE, "C_ui_steady_colour");
                m_qv_dump.add(device, cmd, m_ui_steady.hud(), d3d12::UiSteady::COPIES_STATE, "C_ui_steady_without_gui");
                m_qv_dump.add(device, cmd, m_ui_steady.fin(), d3d12::UiSteady::COPIES_STATE, "C_ui_steady_with_gui");
            }

            if (settings.dump_periphery_full) {
                constexpr uint32_t ALL = 65535u; // (clipped to the image)
                m_qv_dump.add_raw("C_ui_copy_without_gui", 0, 0, ALL, ALL);
                m_qv_dump.add_raw("C_ui_copy_with_gui", 0, 0, ALL, ALL);

                if (ui_steady) {
                    m_qv_dump.add_raw("C_ui_steady_colour", 0, 0, ALL, ALL);
                    m_qv_dump.add_raw("C_ui_steady_without_gui", 0, 0, ALL, ALL);
                    m_qv_dump.add_raw("C_ui_steady_with_gui", 0, 0, ALL, ALL);
                }
            }
        }

        for (int eye = 0; eye < 2; ++eye) {
            const std::string p = eye == 0 ? "L_" : "R_";

            if (fovea_clean[eye] != nullptr) {
                m_qv_dump.add(device, cmd, fovea_clean[eye], ENGINE_STATE, p + "2c_fovea_input_without_interface");
            }

            if (fovea_lin[eye] != nullptr) {
                m_qv_dump.add(device, cmd, fovea_lin[eye], ENGINE_STATE, p + "2l_fovea_input_linear_light");
            }

            if (fovea_hud[eye] != nullptr) {
                m_qv_dump.add(device, cmd, native(m_eye_states[1 + eye].color_copy), ENGINE_STATE, p + "2_fovea_color_with_gui");
                m_qv_dump.add(device, cmd, fovea_hud[eye], D3D12_RESOURCE_STATE_COMMON, p + "ui_copy_without_gui");
                m_qv_dump.add(device, cmd, fovea_fin[eye], D3D12_RESOURCE_STATE_COMMON, p + "ui_copy_with_gui");
            }

            // [MENUS fix83] the copies the eye pass tells the fovea's interface by
            if (inside_hud[eye] != nullptr) {
                m_qv_dump.add(device, cmd, inside_hud[eye], D3D12_RESOURCE_STATE_COMMON, p + "ui_copy_without_gui");
                m_qv_dump.add(device, cmd, inside_fin[eye], D3D12_RESOURCE_STATE_COMMON, p + "ui_copy_with_gui");
            }
        }

        m_qv_dump_header += std::format("  mono periphery: wide view {}x{} covers x {:.3f}..{:.3f} y {:.3f}..{:.3f} (tan), {:.1f} px/tan; eye L x {:.3f}..{:.3f} offset {:+.4f} m "
                                        "-> periphery {}x{}; eye R x {:.3f}..{:.3f} offset {:+.4f} m -> periphery {}x{}; interface cleaned: L {} R {}\n",
            wd.Width / wide_ss, wd.Height / wide_ss, fr.wide_l, fr.wide_r, fr.wide_b, fr.wide_t, px_per_tan_x, fr.eye_l[0], fr.eye_r[0], fr.eye_x[0],
            eye_wide[0] != nullptr ? eye_wide[0]->GetDesc().Width : 0, eye_wide[0] != nullptr ? eye_wide[0]->GetDesc().Height : 0, fr.eye_l[1], fr.eye_r[1],
            fr.eye_x[1], eye_wide[1] != nullptr ? eye_wide[1]->GetDesc().Width : 0, eye_wide[1] != nullptr ? eye_wide[1]->GetDesc().Height : 0,
            fovea_clean[0] != nullptr, fovea_clean[1] != nullptr);
        // [PERIPHERY SUPER fix80 17.11.2026] what anti-aliased the wide view in this frame and the size of its image (the
        // "periphery" sizes above are the images the reprojection wrote: 2 x 2 pixels per ray with the image of twice the size)
        {
            const uint32_t aa_now = m_periphery_running.load(std::memory_order_relaxed);
            m_qv_dump_header += std::format("  periphery anti-aliasing: {} (asked {}{}{}), its image {}x{}, {} x {} pixels per ray to each eye\n",
                aa_now == 2 ? "supersampling TAA, image of twice the wide view's size" : aa_now == 1 ? "supersampling TAA, image of the wide view's size"
                : m_periphery_calm.load(std::memory_order_relaxed) ? "calm light TAA" : "light TAA",
                std::clamp(settings.periphery_aa, 0, 3), m_periphery_why.load(std::memory_order_relaxed) != PW_NONE ? ": " : "",
                periphery_why_text(m_periphery_why.load(std::memory_order_relaxed)), wd.Width, wd.Height, wide_ss, wide_ss);
        }
        // [STABLE DEPTH fix81 17.11.2026]
        m_qv_dump_header += std::format("  reprojection's depth: {}\n",
            stable_depth ? "the wide view's distances steadied over time (StableDepth)"
                         : settings.stable_depth ? "the frame's own depth (the steady distances could not be made)" : "the frame's own depth (option off)");
        // [MENUS fix86 18.11.2026] what a dump's images need to be put through the passes again (tests/supertaa)
        m_qv_dump_header += std::format("  wide view of this frame: jitter {:+.4f} {:+.4f} pixels (a point of the steady images at p is at p + jitter in the "
                                        "frame's), motion vectors to UV by {:.9g} {:.9g}; its interface for the periphery: {}\n",
            wide_jitter[0], wide_jitter[1], m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u),
            m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u),
            ui_steady ? (ui_around ? "collected over the frames, only around the interface (UiSteady; the blocks left out are empty)"
                                   : "collected over the frames (UiSteady)")
                      : (steady_ui ? "the frame's own images, read where the jitter put the point" : "the frame's own images"));
        pre.has_commands = true;
        m_qv_dump_pre_ctx = &pre;
    }

    if (gui_map_ok) {
        m_gui_map.copy_count(cmd, (uint64_t)frame);
    }

    m_gui_map.collect((uint64_t)frame);

    // [UI COPIES 30.10.2026] the wide view's two copies differing on (almost) every block for about a second: a GUI over
    // the whole view (menus, title) or a copy holding another picture (after start-up resets the copy without GUI held the
    // right fovea view's: the whole periphery was that view). The copies are cloned again once; again only after the
    // share went down for a while.
    if (m_gui_map.full_run() >= 90 && !m_ui_copies_refreshed) {
        m_ui_copies_refreshed = true;
        vr->refresh_ui_capture("the wide view's two copies differ on every block for 90 frames");
    } else if (m_gui_map.calm_run() >= 30) {
        m_ui_copies_refreshed = false;
    }

    m_mono_periphery.collect((uint64_t)frame);

    // [FSR DIRECT 06.11.2026] FSR 4 by the mod on this list after its end stamp: the inputs side by side, the pass(es)
    bool fsr_recorded = false;

    if (fsr_on && fsr_inputs) {
        const float fraction = (float)m_qv_layout.inset_h / (float)(std::max)(m_qv_layout.display_h, 1u);
        const float fov_y = 2.0f * std::atan(std::tan(m_fov * 0.5f) * fraction);
        const std::array<ID3D12Resource*, 2> depth{native(m_eye_states[1].depth_copy), native(m_eye_states[2].depth_copy)};
        const std::array<ID3D12Resource*, 2> motion{native(m_eye_states[1].motion_vectors_copy), native(m_eye_states[2].motion_vectors_copy)};

        after_pre = [&, depth, motion, fov_y]() {
            fsr_recorded = fsr_direct_record(device, cmd, frame, fsr_color, depth, motion, fov_y, dump);
            fsr_broken = fsr_broken || !fsr_recorded;

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the jitter check: debug build only
            // [JITTER CHECK 10.11.2026] the check's copies are in this list: written after it ran
            if (m_jc.captured) {
                pre.has_commands = true;
                m_jc.pre_ctx = &pre;
            }
#endif
        };
    }

    pre_end(); // (its own list goes before the plugin's fovea passes: they read the cleaned fovea views)
    after_pre = nullptr;

    if (fsr_on && fsr_broken && !m_fsr_direct_failed) {
        // for good: the upscaler set up again without it (the plugin); this frame without the fovea
        std::string why = m_fsr[0].last_error();

        if (m_fovea_linear.last_error()[0] != '\0') {
            why += (why.empty() ? "" : ", ") + std::string{m_fovea_linear.last_error()};
        }

        REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod failed ({}), the upscaler plugin from now on", why));
        m_fsr_direct_failed = true;
        qv.request_reinit();
    }

    // ---- [MONO UI] the GUI anchors in the wide view: tangents and depth, nearest first; the panels' depth -------------
    d3d12::MonoUi::Params ui{};
    std::memcpy(ui.wide_tan, std::array<float, 4>{fr.wide_l, fr.wide_r, fr.wide_b, fr.wide_t}.data(), sizeof(ui.wide_tan));
    ui.panel_depth = vr->get_ui_distance();
    ui.debug_tint = settings.debug_view == 5;
    ui.curve = m_ui_curve.table();
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    std::string ui_report{};
#endif

    {
        float vp16[16]{};
        // [FRAME LATCH 04.11.2026] the wide view's matrix of this frame (the main thread may already have the next one)
        bool vp_newer = false;
        const bool have_vp = qv.centre_view_proj(vp16, (int64_t)(uint32_t)frame, &vp_newer);

        if (vp_newer) {
            qv.compose_vp_newer.fetch_add(1, std::memory_order_relaxed);
        }

        glm::mat4 vp{};
        std::memcpy(&vp[0][0], vp16, sizeof(vp16));

        // [ANCHOR HOLD 03.11.2026] The elements of this frame: the list being filled and the last complete one (the
        // upscaler may read the first before every element of the frame is in it), one entry per game object. An element
        // that takes pixels to its own depth (item icon, item beam) and is missing from both keeps its place for a few
        // frames: its world position, projected with this frame's matrix. Dump 02 of 03.11.2026: the X icon was at the
        // panels' depth (0.94 m) in all nine frames, i.e. without its anchor; the user saw the X icons move in depth and
        // sideways outside the fovea.
        std::vector<quadview::GuiAnchor> listed_now{};
        std::vector<quadview::GuiAnchor> listed_done{};
        uint64_t gui_frame = 0;
        qv.gui_anchor_lists(listed_done, listed_now, gui_frame);
        std::vector<quadview::GuiAnchor> anchors = listed_now;

        for (const auto& a : listed_done) {
            const bool again = a.id != 0 && std::any_of(listed_now.begin(), listed_now.end(), [&](const quadview::GuiAnchor& b) { return b.id == a.id; });

            if (!again) {
                anchors.push_back(a);
            }
        }

        ++m_anchor_frame;
        bool icon_listed = false;
        bool beam_listed = false;

        for (const auto& a : anchors) {
            if (a.claims() && a.id != 0) {
                m_anchor_hold[a.id] = HeldAnchor{a, m_anchor_frame};
                icon_listed |= a.kind == quadview::GuiAnchor::PANEL && a.at_target;
                beam_listed |= a.kind == quadview::GuiAnchor::BEAM;
            }
        }

        bool icon_held = false;
        bool beam_held = false;
        constexpr uint64_t HOLD_FRAMES = 4;

        for (auto it = m_anchor_hold.begin(); it != m_anchor_hold.end();) {
            if (m_anchor_frame - it->second.seen > HOLD_FRAMES) {
                it = m_anchor_hold.erase(it);
                continue;
            }

            if (it->second.seen != m_anchor_frame) {
                anchors.push_back(it->second.a);
                icon_held |= it->second.a.kind == quadview::GuiAnchor::PANEL && it->second.a.at_target;
                beam_held |= it->second.a.kind == quadview::GuiAnchor::BEAM;
            }

            ++it;
        }

        struct Projected {
            float tx, ty, depth, radius;
            float ext[4];
            const char* name;
        };

        std::vector<Projected> anchored{};
        std::vector<float> panels{};
        uint32_t beams = 0;
        uint32_t beams_off = 0;
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        std::vector<std::pair<std::string, uint32_t>> world_names{}; // [ITEM BEAMS] the game's world elements by name (log)
#endif

        for (const auto& a : anchors) {
            if (a.kind != quadview::GuiAnchor::PANEL) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
                const std::string nm{a.name};
                auto w = std::find_if(world_names.begin(), world_names.end(), [&](const auto& e) { return e.first == nm; });

                if (w == world_names.end()) {
                    world_names.emplace_back(nm, 1u);
                } else {
                    ++w->second;
                }
#endif

                if (a.kind == quadview::GuiAnchor::WORLD) {
                    continue; // only listed
                }
            }

            if (!have_vp) {
                break;
            }

            const glm::vec4 c = vp * glm::vec4{a.pos[0], a.pos[1], a.pos[2], 1.0f};

            if (!(c.w > 0.05f) || !std::isfinite(c.x) || !std::isfinite(c.y)) {
                continue; // behind the head
            }

            const float nx = c.x / c.w;
            const float ny = c.y / c.w;

            // [ITEM BEAMS 03.11.2026] a beam at its item: a column over the anchor, sized for a beam of up to about 1.5 m
            // (dumps of 03.11.2026: about 0.6 - 1 m above the item, drawn up to 0.03 tan beside the item's place in the
            // wide view); only its green pixels go to its depth (MonoUi). The item may be below the view, its beam not.
            if (a.kind == quadview::GuiAnchor::BEAM) {
                if (!settings.item_beams) {
                    ++beams_off;
                    continue;
                }

                if (std::abs(nx) > 1.3f || ny > 1.3f || ny < -3.0f) {
                    continue;
                }

                const float z = c.w;
                Projected pr{};
                pr.tx = fr.wide_l + (nx * 0.5f + 0.5f) * (fr.wide_r - fr.wide_l);
                pr.ty = fr.wide_b + (ny * 0.5f + 0.5f) * (fr.wide_t - fr.wide_b);
                pr.depth = z;
                pr.radius = 0.0f;
                pr.ext[0] = (std::min)(0.12f, 0.035f + 0.06f / z); // across
                pr.ext[1] = (std::min)(1.5f, 0.05f + 2.0f / z);    // above
                pr.ext[2] = (std::min)(0.6f, 0.05f + 0.6f / z);    // below
                pr.ext[3] = 1.0f;
                pr.name = a.name;
                anchored.push_back(pr);
                ++beams;
                continue;
            }

            if (a.radius_tan <= 0.0f) {
                panels.push_back(c.w);
                continue;
            }

            if (std::abs(nx) > 1.5f || std::abs(ny) > 1.5f) {
                continue;
            }

            Projected pr{};
            pr.tx = fr.wide_l + (nx * 0.5f + 0.5f) * (fr.wide_r - fr.wide_l);
            pr.ty = fr.wide_b + (ny * 0.5f + 0.5f) * (fr.wide_t - fr.wide_b);
            pr.depth = c.w;
            // anchored at something in the world: its interface pixels belong to it alone (MonoUi, negative = only claims)
            pr.radius = a.at_target ? a.radius_tan : -a.radius_tan;
            pr.name = a.name;

            // [ICON STACK 03.11.2026 15:30] the item icon is a stack over its item (binoculars, button, arrow): up to 0.10 tan
            // above at 7 m and 0.17 at 1.3 m (dumps 01 / 02 of 15:00), about 0.05 to each side; in the circle of 0.08 only
            // the arrow was. A column over the item instead (MonoUi kind 2).
            if (a.at_target) {
                const float z = c.w;
                pr.ext[0] = 0.06f + 0.02f / z;  // across
                pr.ext[1] = 0.12f + 0.12f / z;  // above
                pr.ext[2] = 0.05f;              // below
                pr.ext[3] = 2.0f;
            }

            anchored.push_back(pr);
        }

        if (!panels.empty()) {
            std::sort(panels.begin(), panels.end());
            ui.panel_depth = panels[panels.size() / 2];
        }

        std::sort(anchored.begin(), anchored.end(), [](const Projected& x, const Projected& y) { return x.depth < y.depth; });
        ui.anchor_count = (uint32_t)(std::min)(anchored.size(), (size_t)d3d12::MonoUi::MAX_ANCHORS);

        for (uint32_t i = 0; i < ui.anchor_count; ++i) {
            ui.anchors[i][0] = anchored[i].tx;
            ui.anchors[i][1] = anchored[i].ty;
            ui.anchors[i][2] = anchored[i].depth;
            ui.anchors[i][3] = anchored[i].radius;
            std::memcpy(ui.anchor_ext[i], anchored[i].ext, sizeof(ui.anchor_ext[i]));
        }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        // [ANCHOR HOLD] the window of the log: frames, frames with an item icon / a beam listed or only held, how often
        // the item icon came and went in the lists
        {
            auto& st = m_anchor_stats;
            ++st.frames;
            st.icon_frames += icon_listed || icon_held ? 1u : 0u;
            st.icon_held += icon_held && !icon_listed ? 1u : 0u;
            st.icon_toggles += icon_listed != st.icon_last ? 1u : 0u;
            st.icon_last = icon_listed;
            st.beam_frames += beam_listed || beam_held ? 1u : 0u;
            st.beam_held += beam_held ? 1u : 0u;
            st.beams_max = (std::max)(st.beams_max, beams);
            st.dropped += anchored.size() > ui.anchor_count ? 1u : 0u;

            if (st.frames >= 450) {
                m_anchor_window = std::format("over {} frames: item icon in {} (only held in {}, came / went {} times), beams in {} (some held in {}, "
                                              "at most {} at once), more than {} anchors in {}",
                    st.frames, st.icon_frames, st.icon_held, st.icon_toggles, st.beam_frames, st.beam_held, st.beams_max, d3d12::MonoUi::MAX_ANCHORS, st.dropped);
                const bool last = st.icon_last;
                st = AnchorStats{};
                st.icon_last = last;
            }
        }
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        ui_report = std::format("interface: GUI elements {} (panels {}, anchored {}, item beams {}{}), panel depth {:.2f} m (2D UI distance {:.2f}), wide view matrix {} | "
                                "copies: wide {} L {} R {}, for the interface inside the fovea image L {} R {} | output curve measured {}",
            anchors.size(), panels.size(), anchored.size() - beams, beams, beams_off > 0 ? std::format(", {} beams off", beams_off) : std::string{},
            ui.panel_depth, vr->get_ui_distance(), have_vp, centre_ui, fovea_hud[0] != nullptr, fovea_hud[1] != nullptr, inside_hud[0] != nullptr,
            inside_hud[1] != nullptr, ui.curve != nullptr);

        for (uint32_t i = 0; i < ui.anchor_count && i < (dump ? d3d12::MonoUi::MAX_ANCHORS : 6u); ++i) {
            ui_report += std::format(" | anchor {} {}: tan ({:.3f},{:.3f}) depth {:.2f} m", i, anchored[i].name != nullptr ? anchored[i].name : "?", ui.anchors[i][0],
                ui.anchors[i][1], ui.anchors[i][2]);
        }

        // [ITEM BEAMS] which elements the VR mod placed this frame (names, at most 12; "@item" = at something in the world)
        {
            std::string placed{};
            uint32_t n = 0;

            for (const auto& a : anchors) {
                if (a.kind == quadview::GuiAnchor::PANEL && n++ < 12) {
                    placed += std::format("{}{}{}", placed.empty() ? "" : ", ", (const char*)a.name, a.at_target ? "@item" : "");
                }
            }

            if (!placed.empty()) {
                ui_report += " | placed by the VR mod: " + placed;
            }
        }

        if (!world_names.empty()) {
            ui_report += " | world elements of the game:";

            for (const auto& [nm, n] : world_names) {
                ui_report += std::format(" {} x{}", nm, n);
            }
        }

        if (!m_anchor_window.empty()) {
            ui_report += " | anchors " + m_anchor_window;
        }
#endif

        // [ITEM BEAMS] in a dump: every element with its world position (to compare with the images)
        if (dump) {
            for (const auto& a : anchors) {
                m_qv_dump_header += std::format("  GUI element {} kind {} at ({:.3f}, {:.3f}, {:.3f}) radius {:.2f} at item {}\n", (const char*)a.name, (int)a.kind, a.pos[0], a.pos[1],
                    a.pos[2], a.radius_tan, a.at_target);
            }
        }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        // [GPU 29.10.2026 round 3] the share of each eye's periphery that needed the reprojection's second pass
        if (m_mono_periphery.second_pass_share(0) >= 0.0) {
            ui_report += std::format(" | reprojection second pass: L {:.1f}% R {:.1f}% of the pixels", m_mono_periphery.second_pass_share(0) * 100.0,
                m_mono_periphery.second_pass_share(1) * 100.0);
        }

        // [GPU 29.10.2026] the share of the wide view where the eye passes look for its interface at all
        if (m_gui_map.marked_share() >= 0.0) {
            ui_report += std::format(" | GUI map: {:.1f}% of the blocks with interface", m_gui_map.marked_share() * 100.0);
        }

        // [TILES PROBE fix114 21.11.2026] what the tile pass marked, at the last copy (see EyeCompose::TilesCount)
        for (uint32_t e = 0; e < 2; ++e) {
            const auto& tc = m_eye_compose.tiles_count(e);

            if (!tc.valid || tc.tiles == 0) {
                continue;
            }

            ui_report += std::format(" | eye tiles {}: marked {} of {} ({:.2f}%), panels' bit {}, an anchor's bit {}, panels' bit alone {}, beam place other {}, "
                                     "box {} tiles, map idle {} frames, per bit",
                e == 0 ? "L" : "R", tc.marked, tc.tiles, 100.0 * (double)tc.marked / (double)tc.tiles, tc.panel, tc.anchors, tc.only_panel, tc.beam_other,
                tc.box, m_gui_map.idle_run());

            for (uint32_t b = 0; b < 17; ++b) {
                if (tc.per_bit[b] != 0) {
                    ui_report += std::format(" {}:{}", b, tc.per_bit[b]);
                }
            }
        }

        // [OVER fix121] the eye passes whose marked tiles' pass put the interface over the stored pixels (running count)
        ui_report += std::format(" | interface over the stored pixels: {} eye passes so far", m_eye_compose.over_frames());

        // [UI COPIES 30.10.2026] device resets and the check above
        ui_report += std::format(" | copies made again {} times", vr->ui_capture_refreshes());
#endif
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
    // [UI MOTION fix125] what the interface's own motion was made of, and whether the passes ran with it
    if (um_on) {
        ui_report += std::format(" | {} | passes with it: wide view {}, fovea (the last one) {}", m_ui_motion_report, m_ui_steady.last_ui_motion() ? "yes" : "no",
            m_super_taa.last_ui_motion() ? "yes" : "no");
    } else if (um_idle && settings.ui_motion && !settings.test_no_ui_motion && steady_ui) {
        ui_report += std::format(" | interface motion: resting, the wide view has shown no interface for {} frames", m_gui_map.idle_run());
    } else {
        ui_report += " | interface motion: off (the scene's motion vectors)";
    }

    {
        std::scoped_lock _{m_qv_status_mutex};
        m_ui_report = ui_report;
    }

    if (dump) {
        m_qv_dump_header += "  " + ui_report + "\n";
        m_qv_dump_header += "  interface copies: " + vr->ui_capture_report() + "\n";
    }
#endif

    // ---- [BEAM FIT 03.11.2026] where the wide view drew each item beam (GuiMap, into its extra row; EyeCompose puts the
    // beam over its item from it). Boxes in texels of the copies around each beam anchor: its column's reach to each side,
    // above and below. Recorded in the eye passes' list, before them.
    uint32_t beams_fitted = 0;

    if (gui_map_ok && m_gui_map.gui_w() > 0 && m_gui_map.gui_h() > 0) {
        d3d12::GuiMap::BeamFit fit{};
        const float ppx = (float)m_gui_map.gui_w() / (fr.wide_r - fr.wide_l);
        const float ppy = (float)m_gui_map.gui_h() / (fr.wide_t - fr.wide_b);
        fit.tan_per_texel[0] = 1.0f / ppx;
        fit.tan_per_texel[1] = 1.0f / ppy;
        fit.wide_left_top[0] = fr.wide_l;
        fit.wide_left_top[1] = fr.wide_t;

        for (uint32_t i = 0; i < ui.anchor_count && fit.count < 16; ++i) {
            if (!(ui.anchor_ext[i][3] > 0.5f && ui.anchor_ext[i][3] < 1.5f)) {
                continue;
            }

            const float ax = (ui.anchors[i][0] - fr.wide_l) * ppx;
            const float ay = (fr.wide_t - ui.anchors[i][1]) * ppy;
            const int half = (std::clamp)((int)std::ceil(ui.anchor_ext[i][0] * ppx), 2, 63);
            const int up = (std::clamp)((int)std::ceil(ui.anchor_ext[i][1] * ppy), 4, 600);
            const int down = (std::clamp)((int)std::ceil(ui.anchor_ext[i][2] * ppy), 2, 200);
            const uint32_t k = fit.count++;
            fit.box[k][0] = (int32_t)std::floor(ax) - half;
            fit.box[k][1] = (int32_t)std::floor(ay) - up;
            fit.box[k][2] = (int32_t)std::floor(ax) + half;
            fit.box[k][3] = (int32_t)std::floor(ay) + down;
            fit.anchor_x[k] = ax;
            fit.anchor_y[k] = ay;
            fit.index[k] = i;
        }

        if (fit.count > 0 && m_gui_map.fit_beams(device, copier.cmd_list.Get(), fit)) {
            beams_fitted = fit.count;
        }
    }

    if (dump) {
        m_qv_dump_header += std::format("  item beams fitted in the wide view: {}\n", beams_fitted);
    }

    // ---- 4. per eye: periphery upscale, fovea pass, composite, interface ---------------------------------------------
    bool ok = true;

    // [DIRECT 8 BIT 30.10.2026] the headset textures the eye pass writes itself (the eye image would be 10-bit
    // R10G10B10A2, its rounding is reproduced in the pass): not while a dump takes the eye images, and the left eye not
    // while the desktop window copies its 10-bit image
    ID3D12Resource* direct[2]{};

    if (settings.direct_8bit && !dump && !settings.separate_eye_passes && backbuffer != nullptr &&
        backbuffer->GetDesc().Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        for (int e = 0; e < 2; ++e) {
            direct[e] = vr->direct_eye_target(e);
        }

        // ([SMALL WINDOW fix122] only while the window can take the left eye's image: with the small window the left eye
        // stays direct and no 10-bit image is made and converted for the window)
        // ([CANVAS fix123] nor while the flatscreen canvas is shown: the window keeps the engine's own picture, see evaluate_quadview)
        if (settings.desktop_mirror && !vr->is_flatscreen_overlay() && mirror_fits(backbuffer)) {
            direct[0] = nullptr;
        }
    }

    // [ONE SUBMIT 04.11.2026] the plugin in the fovea: the left eye's call only records when the right eye's call follows
    // (the same test as evaluate_quadview_eye's for the fovea view's inputs, for the right eye's)
    const auto& right_inset = m_eye_states[2];
    const bool right_plugin = settings.fovea_mode == quadview::FoveaMode::Upscaler && settings.debug_view != 1 && right_inset.scene_layer != nullptr &&
                              right_inset.color != nullptr && native(right_inset.color_copy) != nullptr && native(right_inset.depth_copy) != nullptr &&
                              native(right_inset.motion_vectors_copy) != nullptr;

    for (int32_t eye = 0; eye < 2; ++eye) {
        QvEval ev{};
        ev.plugin_execute = eye == 1 || !right_plugin;
        ev.mono_ui = true;
        // [MENUS fix86 18.11.2026] collected over the frames where that pass ran (they hold no jitter: none to follow)
        ev.ui_centre_color = centre_ui ? (ui_steady ? m_ui_steady.color() : wide_color) : nullptr;
        ev.ui_centre_hud = centre_ui ? (ui_steady ? m_ui_steady.hud() : centre_hud) : nullptr;
        ev.ui_centre_fin = centre_ui ? (ui_steady ? m_ui_steady.fin() : centre_fin) : nullptr;
        ev.ui_fovea_color = fovea_hud[eye] != nullptr ? native(m_eye_states[1 + eye].color_copy) : nullptr;
        ev.ui_fovea_hud = fovea_hud[eye];
        ev.ui_fovea_inside = ui_inside; // [MENUS fix82 17.11.2026]
        ev.ui_inside_hud = inside_hud[eye]; // [MENUS fix83 17.11.2026]
        ev.ui_inside_fin = inside_fin[eye];
        ev.ui_motion = ui_inside ? um_blocks[1 + eye] : d3d12::UiMotion::Block{}; // [UI MOTION fix125]
        ev.ui_wide_jitter[0] = steady_ui && !ui_steady ? wide_jitter[0] : 0.0f;
        ev.ui_wide_jitter[1] = steady_ui && !ui_steady ? wide_jitter[1] : 0.0f;
        ev.ui_params = ui;
        std::memcpy(ev.ui_params.eye_tan, std::array<float, 4>{fr.eye_l[eye], fr.eye_r[eye], fr.eye_b[eye], fr.eye_t[eye]}.data(), sizeof(ev.ui_params.eye_tan));
        ev.ui_params.eye_x = fr.eye_x[eye];
        ev.eye = eye;
        ev.slot = (uint32_t)eye;
        ev.wide_state = 0;
        ev.inset_state = 1u + (uint32_t)eye;
        ev.wide_id = get_evaluate_id(0);
        ev.fovea_id = get_qv_fovea_id((uint32_t)eye);
        ev.to_backbuffer = eye == 0;
        ev.dump_prefix = eye == 0 ? "L_" : "R_";
        ev.wide_override = eye_wide[eye];
        ev.wide_override_linear = wide_linear;
        ev.inset_color_override = fovea_lin[eye] != nullptr ? fovea_lin[eye] : fovea_clean[eye];
        ev.fovea_linear = fovea_lin[eye] != nullptr; // [FOVEA LINEAR 05.11.2026]
        ev.fsr_direct = fsr_recorded;                // [FSR DIRECT 06.11.2026]
        ev.gui_map = gui_map_ok ? &m_gui_map : nullptr;
        ev.direct_out = direct[eye];
        ok &= evaluate_quadview_eye(copier, backbuffer, frame, ev, dump, df);
    }

    return ok;
}

bool TemporalUpscaler::evaluate_quadview_eye(d3d12::CommandContext& copier, ID3D12Resource* backbuffer, int32_t frame, const QvEval& ev, bool dump_arg,
    const quadview::DumpFrame& df) {
#if REVR_RELEASE
    constexpr bool dump = false; // [RELEASE SWITCH fix141] no dumps in the release: the compiler leaves their branches out
    (void)dump_arg;
#else
    const bool dump = dump_arg;
#endif
    auto& qv = quadview::State::get();
    const auto settings = qv.settings();
    auto device = g_framework->get_d3d12_hook()->get_device();
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // engine copies are read as shader resources (same assumption as the frame dump)
    constexpr auto ENGINE_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    auto native = [](const sdk::intrusive_ptr<sdk::renderer::Texture>& tex) -> ID3D12Resource* {
        if (tex == nullptr) {
            return nullptr;
        }

        const auto container = tex->get_d3d12_resource_container();
        return container != nullptr ? container->get_native_resource() : nullptr;
    };

    struct Inputs {
        ID3D12Resource* color{nullptr};
        ID3D12Resource* depth{nullptr};
        ID3D12Resource* mv{nullptr};
        float jitter_x{0.0f};
        float jitter_y{0.0f};
        bool ok{false};
    };

    const uint32_t slot = ev.slot;
    const auto wide_index = ev.wide_id - 1;
    const auto fovea_id = ev.fovea_id;
    const auto fovea_index = fovea_id - 1;
    const auto eye = ev.eye;
    const uint32_t image_slot = (uint32_t)eye;

    auto inputs_of = [&](const EyeState& s, uint32_t jitter_index) {
        Inputs in{};
        in.color = native(s.color_copy);
        in.depth = native(s.depth_copy);
        in.mv = native(s.motion_vectors_copy);
        const auto j = jitter_of_frame(jitter_index, (int64_t)(uint32_t)frame); // [JITTER LATCH 10.11.2026]
        in.jitter_x = j[0];
        in.jitter_y = j[1];
        in.ok = s.scene_layer != nullptr && s.color != nullptr && in.color != nullptr && in.depth != nullptr && in.mv != nullptr;
        return in;
    };

    if (eye < 0 || eye > 1 || ev.wide_state >= m_eye_states.size() || ev.inset_state >= m_eye_states.size()) {
        ++qv.skipped;
        set_qv_status("internal: eye state index out of range");
        return false;
    }

    const Inputs wide_in = inputs_of(m_eye_states[ev.wide_state], wide_index);
    Inputs inset_in = inputs_of(m_eye_states[ev.inset_state], fovea_index);

    // [MONO PERIPHERY] the fovea view without its interface (same format and size as the engine copy)
    if (inset_in.ok && ev.inset_color_override != nullptr) {
        inset_in.color = ev.inset_color_override;
    }

    if (ev.wide_override == nullptr && !wide_in.ok) {
        ++qv.skipped;
        set_qv_status(std::format("waiting for the wide view of eye {} (scene layers found: {})", eye, m_layers_in_use));
        return false;
    }

    // debug view 1: periphery only, no fovea
    const bool fovea_taa = settings.fovea_mode == quadview::FoveaMode::TAA || m_qv_fovea_own; // ([FOVEA OWN fix91] no instance to run)
    // [FSR DIRECT 06.11.2026] FSR 4 by the mod, its pass not recorded this frame (inputs missing, or it failed): the
    // periphery only (there is no plugin instance for the fovea)
    // [CANVAS fix124 22.11.2026] nor while the flatscreen canvas is shown (RE4's 2D cutscenes): the camera overrides rest
    // then and the fovea views hold the game camera's whole picture, which the eye image showed as a small copy of the
    // cutscene inside the cutscene (the user's dump of 19.11.2026). The eye image is the periphery alone, and the canvas
    // takes the left one (D3D12Component::on_frame); the fovea's anti-aliasing is not run either.
    const bool use_fovea = inset_in.ok && settings.debug_view != 1 && (!m_qv_fsr || fovea_taa || ev.fsr_direct) && !VR::get()->is_flatscreen_overlay();
    auto* cmd = copier.cmd_list.Get();

    auto& spans = d3d12::GpuSpans::get();

    // [EYE COMPOSE 29.10.2026] the shared periphery: enlargement, fovea composite, interface and post pass in one pass per
    // eye (d3d12::EyeCompose); the separate passes below stay for a wide view per eye, and if that pass cannot be built
    const bool fused = ev.mono_ui && ev.wide_override != nullptr && !settings.separate_eye_passes && m_eye_compose.ready(device);

    if (ev.mono_ui && ev.wide_override != nullptr && !settings.separate_eye_passes && !fused && !m_eye_compose_logged) {
        REVR_LOG(spdlog::error("[TemporalUpscaler] eye pass not available ({}), the separate passes are used", m_eye_compose.last_error()));
        m_eye_compose_logged = true;
    }

    // ---- periphery -> eye-sized image ---------------------------------------------------------------
    // shared periphery: this eye's part of the shared wide view, reprojected (and anti-aliased) already; a wide view per
    // eye: its light TAA here. Then the cheap spatial upscale to the eye size (in the eye pass when fused; a dump still
    // gets the separate image). Both run after the fovea pass, below.
    const auto bb = backbuffer->GetDesc();
    const uint32_t img_w = m_qv_layout.display_w > 0 ? m_qv_layout.display_w : (uint32_t)bb.Width;
    const uint32_t img_h = m_qv_layout.display_h > 0 ? m_qv_layout.display_h : bb.Height;

    ID3D12Resource* periphery = ev.wide_override;
    int encode = ev.wide_override_linear ? 1 : 0;
    ID3D12Resource* eye_src = nullptr;

    copier.has_commands = true;

    // ---- fovea view -> fovea image ------------------------------------------------------------------
    ID3D12Resource* plugin_linear_out = nullptr; // [FOVEA LINEAR 05.11.2026] the plugin's own result (linear light), for the dump
    quadview::Rect rect{};
    // [FRAME LATCH 04.11.2026] the rectangle this frame's fovea view was rendered with (the main thread may already have
    // moved it for the next frame)
    bool rect_newer = false;
    const bool have_rect = qv.rect_for_frame(eye, (int64_t)(uint32_t)frame, rect, &rect_newer);

    if (rect_newer) {
        qv.compose_rect_newer.fetch_add(1, std::memory_order_relaxed);
    }
    ID3D12Resource* fovea_out = nullptr; // ([NO PLUGIN fix91] was the plugin's result image of this eye)
    bool fovea_super_ran = false; // [SUPER TAA 16.11.2026] this eye's fovea image is the supersampling TAA's

    if (fovea_taa != m_qv_fovea_taa_last) {
        m_fovea_taa.invalidate(); // a switch starts the histories again
        m_qv_fovea_taa_last = fovea_taa;

        // [GPU GUARD 05.11.2026] ... the plugin's too: its history is from before the switch (seconds old with the GPU guard)
        if (!fovea_taa) {
            m_qv_plugin_reset = {true, true};
        }
    }

    if (use_fovea && fovea_taa) {
        // [FOVEA TAA 28.10.2026] the mod's TAA at the render size, then 1:1 (or resampled) into a fovea-sized image
        if (!ensure_uav_image(device, m_qv_fovea_safe[image_slot], m_qv_layout.inset_w, m_qv_layout.inset_h, bb.Format, L"RE4VR tri-view fovea image")) {
            set_qv_status("fovea: could not create the fovea image");
            qv.mark_failed(m_qv_init_generation, "fovea: could not create the fovea image");
            return false;
        }

        // distance from the depth value: rows 2 and 3 of the inverse projection (the fovea's sub-frustum changes rows 0 and
        // 1 only, so the eye's projection gives them). The engine stores the depth reversed relative to that projection.
        float depth_k[4]{};
        bool depth_k_valid = false;
        {
            const glm::mat4 reverse_z{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
            const glm::mat4 proj = VR::get()->get_projection_matrix((uint32_t)eye);
            const auto inv = glm::inverse(reverse_z * proj);
            depth_k[0] = inv[2][2];
            depth_k[1] = inv[3][2];
            depth_k[2] = inv[2][3];
            depth_k[3] = inv[3][3];
            depth_k_valid = std::isfinite(depth_k[0]) && std::isfinite(depth_k[1]) && std::isfinite(depth_k[2]) && std::isfinite(depth_k[3]) &&
                            (std::abs(depth_k[2]) + std::abs(depth_k[3])) > 1e-9f;
        }

        // all views render at the same size -> same motion vector scale as the periphery
        const float mv_scale_x = m_motion_scale[0] / (float)(std::max)(m_qv_layout.render_w, 1u);
        const float mv_scale_y = m_motion_scale[1] / (float)(std::max)(m_qv_layout.render_h, 1u);

        // the mod's TAA (d3d12::FoveaTAA) of this eye into `target` (a fovea-sized image resting in UNORDERED_ACCESS)
        auto run_fovea_taa = [&](ID3D12Resource* target) {
            d3d12::FoveaTAA::Params tp{};
            const auto& ft = settings.fovea_taa;
            tp.s.feedback = ft.feedback;
            tp.s.clip_gamma = ft.clip_gamma;
            tp.s.filter_radius = ft.filter_radius;
            tp.s.new_softness = ft.new_softness;
            tp.s.motion_reduce = ft.motion_reduce;
            tp.s.depth_reject = ft.depth_reject;
            tp.s.sharpen = ft.sharpen;
            tp.s.dilate = ft.dilate;
            tp.s.catmull_rom = ft.catmull_rom;
            tp.s.luma_weight = ft.luma_weight;
            tp.s.fast_converge = ft.fast_converge;
            tp.eye = slot;
            tp.mv_scale_x = mv_scale_x;
            tp.mv_scale_y = mv_scale_y;
            tp.jitter_x = inset_in.jitter_x;
            tp.jitter_y = inset_in.jitter_y;
            tp.reverse_z = true;
            // a frame without a fovea TAA pass of this eye (skipped, debug view, the plugin meanwhile): start again
            tp.reset = m_qv_ftaa_frame[slot] + 1 != m_qv_frame;
            tp.tiled = !settings.test_round2_passes; // [GPU 29.10.2026 round 3] same result, the GPU pass test compares
            tp.tile = settings.test_fovea_taa_32 ? 32u : 16u; // [GPU 31.10.2026] same result
            tp.halo_serial = settings.test_halo_serial;       // [GPU 01.11.2026] same result
            std::memcpy(tp.depth_k, depth_k, sizeof(tp.depth_k));
            tp.depth_k_valid = depth_k_valid;

            if (!m_fovea_taa.dispatch(device, cmd, inset_in.color, inset_in.mv, inset_in.depth, ENGINE_STATE, target, tp)) {
                return false;
            }

            m_qv_ftaa_frame[slot] = m_qv_frame;
            return true;
        };

        // [SUPER TAA 16.11.2026] the mod's supersampling TAA (d3d12::SuperTAA) when chosen; the mod's TAA beside it into an
        // image of its own when the menu asks for the comparison (split view) or a capture runs. If a pass of it fails
        // (shader, image): the mod's TAA from now on (until the next set-up), with the reason in the log.
        bool super_done = false;

        if (settings.fovea_super && !m_super_taa_failed) {
            const auto& st = settings.super_taa;
            const bool capture = m_sc.recording() && eye == 0;
            ID3D12Resource* cmp = nullptr;

            if ((st.compare || st.split || m_sc.wants_compare()) &&
                ensure_uav_image(device, m_qv_fovea_cmp[image_slot], m_qv_layout.inset_w, m_qv_layout.inset_h, bb.Format, L"RE4VR tri-view fovea image (the mod's TAA beside)") &&
                run_fovea_taa(m_qv_fovea_cmp[image_slot].Get())) {
                cmp = m_qv_fovea_cmp[image_slot].Get();
            }

            d3d12::SuperTAA::Params sp{};
            sp.s.scale = st.scale >= 2 ? 2u : 1u;
            sp.s.filter_scale = st.filter_scale;
            sp.s.stability = st.stability;
            sp.s.clip_gamma = st.clip_gamma;
            sp.s.motion_reduce = st.motion_reduce;
            sp.s.depth_reject = st.depth_reject;
            sp.s.history_sharpness = st.history_sharpness;
            sp.s.sharpen = st.sharpen;
            sp.s.luma_weight = st.luma_weight;
            sp.s.dering = st.lighter ? 2u : 1u;
            sp.s.nearest_sample = st.lighter;
            sp.s.box_resolve = !st.lanczos;
            sp.s.follow = st.follow;
            sp.s.follow_threshold = st.follow_threshold;
            sp.s.follow_memory = st.follow_memory;
            sp.s.protect = st.protect;
            sp.s.protect_range = st.protect_range;
            sp.s.protect_spark = st.protect_spark;
            sp.s.protect_memory = st.protect_memory;
            sp.s.arrive = st.arrive;
            sp.eye = slot;
            sp.mv_scale_x = mv_scale_x;
            sp.mv_scale_y = mv_scale_y;
            sp.jitter_x = inset_in.jitter_x;
            sp.jitter_y = inset_in.jitter_y;
            sp.reverse_z = true;
            sp.reset = m_qv_staa_frame[slot] + 1 != m_qv_frame;
            std::memcpy(sp.depth_k, depth_k, sizeof(sp.depth_k));
            sp.depth_k_valid = depth_k_valid;
            sp.debug_view = (uint32_t)(std::max)(st.debug_view, 0);
            sp.split_x = st.split && cmp != nullptr ? m_qv_layout.inset_w / 2 : 0u;
            sp.split_src = sp.split_x != 0 ? cmp : nullptr;
            sp.keep_constants = dump || capture;
            // [UI MOTION fix125 22.11.2026] the interface the engine drew into the view is fetched along its own motion
            sp.ui_hud = ev.ui_inside_hud;
            sp.ui_fin = ev.ui_inside_fin;
            sp.ui_motion = ev.ui_motion;

            if (m_super_taa.dispatch(device, cmd, inset_in.color, inset_in.mv, inset_in.depth, ENGINE_STATE, m_qv_fovea_safe[image_slot].Get(), sp)) {
                super_done = true;
                m_qv_staa_frame[slot] = m_qv_frame;
                qv.super_dispatches.fetch_add(1, std::memory_order_relaxed);

                if (capture) {
                    // (the settings as text for the capture's header; the depth-stencil's part through the mod's own copy pass)
                    const auto& ft = settings.fovea_taa;
                    const std::string settings_text = std::format(
                        "settings: scale {} stability {:.9g} history_sharpness {:.9g} sharpen {:.9g} follow {} follow_threshold {:.9g} "
                        "follow_memory {:.9g} filter_scale {:.9g} clip_gamma {:.9g} motion_reduce {:.9g} depth_reject {:.9g} luma_weight {} "
                        "lighter {} lanczos {} protect {} protect_range {:.9g} protect_spark {:.9g} protect_memory {:.9g} arrive {}\n"
                        "old_taa_settings: feedback {:.9g} clip_gamma {:.9g} filter_radius {:.9g} new_softness {:.9g} motion_reduce {:.9g} depth_reject {:.9g} "
                        "sharpen {:.9g} dilate {} catmull_rom {} luma_weight {} fast_converge {}\n"
                        "layout: render {} {} fovea {} {} eye_image {} {}\n",
                        st.scale, st.stability, st.history_sharpness, st.sharpen, st.follow ? 1 : 0, st.follow_threshold, st.follow_memory,
                        st.filter_scale, st.clip_gamma, st.motion_reduce, st.depth_reject, st.luma_weight ? 1 : 0, st.lighter ? 1 : 0, st.lanczos ? 1 : 0,
                        st.protect ? 1 : 0, st.protect_range, st.protect_spark, st.protect_memory, st.arrive ? 1 : 0, ft.feedback, ft.clip_gamma, ft.filter_radius, ft.new_softness,
                        ft.motion_reduce, ft.depth_reject, ft.sharpen, ft.dilate ? 1 : 0,
                        ft.catmull_rom ? 1 : 0, ft.luma_weight ? 1 : 0, ft.fast_converge ? 1 : 0, m_qv_layout.render_w, m_qv_layout.render_h,
                        m_qv_layout.inset_w, m_qv_layout.inset_h, img_w, img_h);
                    m_sc.record(device, cmd, m_super_taa, slot, inset_in.color, inset_in.mv, inset_in.depth, ENGINE_STATE, m_qv_fovea_safe[image_slot].Get(), cmp,
                        sp, settings_text, std::format("engine_frame {} fovea_rectangle {} {} {} {}", frame, rect.x, rect.y, rect.w, rect.h),
                        [&](ID3D12Resource* depth, ID3D12Resource* target, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                            d3d12::FoveaLinear::Region r{};
                            r.w = w;
                            r.h = h;
                            r.shift_x = (int32_t)x;
                            r.shift_y = (int32_t)y;
                            return m_fovea_linear.copy_part(device, cmd, depth, ENGINE_STATE, target, r, true);
                        });
                }
            } else {
                REVR_LOG(spdlog::error("[TemporalUpscaler] supersampling TAA failed ({}): the mod's TAA in the fovea until the next set-up", m_super_taa.last_error()));
                m_super_taa_failed = true;
            }
        }

        if (settings.fovea_super && !super_done) {
            qv.super_fallbacks.fetch_add(1, std::memory_order_relaxed);
        }

        if (!super_done && !run_fovea_taa(m_qv_fovea_safe[image_slot].Get())) {
            const std::string why = std::string{"fovea TAA failed: "} + m_fovea_taa.last_error();
            set_qv_status(why);
            qv.mark_failed(m_qv_init_generation, why);
            return false;
        }

        fovea_super_ran = super_done;
        fovea_out = m_qv_fovea_safe[image_slot].Get();
    } else if (use_fovea && m_qv_fsr) {
        // [FSR DIRECT 06.11.2026] FSR 4 by the mod ran on the pre-fovea list: this eye's part of its result (both foveas side
        // by side, or this eye's own) encoded (and sharpened with RCAS) into the fovea image, as after the plugin. Without
        // linear light it holds the encoded image already: copied without sharpening, as the plugin's result was used.
        const bool atlas = m_qv_fsr_atlas.valid;
        ID3D12Resource* result = m_qv_fsr_out[atlas ? 0 : (slot < 2 ? slot : 0)].Get();
        const int32_t x0 = atlas && eye == 1 ? (int32_t)m_qv_fsr_atlas.right_out_x : 0;
        d3d12::FoveaLinear::Region r{};
        r.w = m_qv_layout.inset_w;
        r.h = m_qv_layout.inset_h;
        r.shift_x = x0;
        r.lo_x = x0;
        r.hi_x = x0 + (int32_t)m_qv_layout.inset_w - 1;
        r.hi_y = (int32_t)m_qv_layout.inset_h - 1;

        if (result != nullptr &&
            ensure_uav_image(device, m_qv_fovea_safe[image_slot], m_qv_layout.inset_w, m_qv_layout.inset_h, bb.Format, L"RE4VR tri-view fovea image") &&
            m_fovea_linear.encode_part(device, cmd, result, m_qv_fovea_safe[image_slot].Get(), settings.upscaler_linear ? settings.upscaler_sharpen : 0.0f, r,
                settings.upscaler_linear)) {
            plugin_linear_out = atlas && eye == 1 ? nullptr : result; // (the dump: the side-by-side result once)
            fovea_out = m_qv_fovea_safe[image_slot].Get();
        } else {
            // for good: the upscaler set up again with the plugin; this eye keeps its last image
            REVR_LOG(spdlog::error("[TemporalUpscaler] FSR 4 by the mod: the fovea image could not be written ({}), the upscaler plugin from now on",
                m_fovea_linear.last_error()));
            m_fsr_direct_failed = true;
            qv.request_reinit();
            ++qv.skipped;
            return false;
        }
    }
    // ([NO PLUGIN fix91 19.11.2026] the third branch, the upscaler plugin in the fovea, was here: the plugin with the
    // output size set per instance, and its result encoded when it had worked in linear light)

    // [LONG EDGES 11.11.2026] anti-aliasing along the edges of the upscaled fovea (its own image, resting in UNORDERED_ACCESS
    // like the fovea image); the interface is added later by the eye pass, so it is not touched
    if (use_fovea && post_long_edges() > 0.0f && fovea_out != nullptr && fovea_out == m_qv_fovea_safe[image_slot].Get()) {
        const auto fd = fovea_out->GetDesc();

        if (ensure_uav_image(device, m_qv_fovea_aa[image_slot], (uint32_t)fd.Width, fd.Height, fd.Format, L"RE4VR tri-view fovea image (long edges)") &&
            m_long_edge.dispatch(device, cmd, fovea_out, UAV, m_qv_fovea_aa[image_slot].Get(), post_long_edges())) {
            fovea_out = m_qv_fovea_aa[image_slot].Get();
        } else {
            static bool once = true;

            if (once) {
                REVR_LOG(spdlog::warn("[TemporalUpscaler] long edges: pass not run ({})", m_long_edge.last_error()));
                once = false;
            }
        }
    }

    spans.stamp(cmd, (uint64_t)frame, eye == 0 ? d3d12::GpuSpans::COPIER_FOVEA_L : d3d12::GpuSpans::COPIER_FOVEA_R);

    // the periphery's separate enlargement (after the fovea pass, so the time spans of both ways cover the same work)
    if (!fused || dump) {
        if (!ensure_qv_wide_safe(device, image_slot, img_w, img_h, bb.Format)) {
            set_qv_status("periphery: could not create the eye image");
            qv.mark_failed(m_qv_init_generation, "periphery: could not create the eye image");
            return false;
        }

        if (periphery == nullptr) {
            periphery = periphery_taa(device, cmd, slot, wide_in.color, wide_in.mv, wide_in.depth, settings, wide_in.jitter_x, wide_in.jitter_y);

            if (periphery == nullptr) {
                ++qv.skipped; // status already says why; the eye keeps its last image this frame
                return false;
            }

            encode = m_periphery_linear ? 1 : 0;
        }

        if (!m_wide_upscale.dispatch(device, cmd, periphery, UAV, m_qv_wide_safe[image_slot].Get(), encode)) {
            // permanent (shader / format): switch quad view off with the reason shown in the menu
            const std::string why = std::string{"periphery upscale failed: "} + m_wide_upscale.last_error();
            set_qv_status(why);
            qv.mark_failed(m_qv_init_generation, why);
            return false;
        }

        eye_src = m_qv_wide_safe[image_slot].Get();
    }

    // ---- debug dump: inputs and outputs before the composite -------------------------------------
    if (dump) {
        const std::string p = ev.dump_prefix;

        m_qv_dump_header += std::format(
            "  dump {} frame {} ({} eye): render_frame={} slot={} debug_view={} use_fovea={} fovea={}\n"
            "  layout: display={}x{} render={}x{} fovea={}x{} rect={} ({},{} {}x{})\n"
            "  inputs ok: wide={} fovea={} | scene layers in use: {}\n",
            df.serial, df.index, eye == 0 ? "left" : "right", frame, slot, settings.debug_view, use_fovea,
            fovea_super_ran ? "mod supersampling TAA" : (fovea_taa ? "mod TAA" : "upscaler plugin"),
            m_qv_layout.display_w, m_qv_layout.display_h, m_qv_layout.render_w, m_qv_layout.render_h, m_qv_layout.inset_w, m_qv_layout.inset_h,
            have_rect, rect.x, rect.y, rect.w, rect.h, wide_in.ok, inset_in.ok, m_layers_in_use);

        // [FSR DIRECT 06.11.2026]
        if (m_qv_fsr) {
            m_qv_dump_header += std::format("  FSR 4 by the mod ({}, functions from {}): {} | ran this frame {} | automatic exposure {} | output {}x{}, "
                                            "scaled down by {:.3f} (network option {})\n",
                m_fsr[0].provider(), d3d12::FsrDirect::api_source(),
                m_qv_fsr_atlas.valid ? std::format("both foveas in one pass, {}x{} -> {}x{}, gap {} / {} px, right view at x {} / {}", m_qv_fsr_atlas.render_w,
                                           m_qv_fsr_atlas.render_h, m_qv_fsr_atlas.out_w, m_qv_fsr_atlas.out_h, m_qv_fsr_atlas.gap_r, m_qv_fsr_atlas.gap_o,
                                           m_qv_fsr_atlas.right_render_x, m_qv_fsr_atlas.right_out_x)
                                     : std::string{"a pass per eye"},
                ev.fsr_direct, m_fsr[0].created().auto_exposure, m_fsr[0].created().out_w, m_fsr[0].created().out_h, m_qv_fsr_factor,
                qv.settings().fsr_model);
        }

        if (ev.wide_override == nullptr) { // [MONO PERIPHERY] the shared wide view was dumped once already (C_ prefix)
            m_qv_dump.add(device, cmd, wide_in.color, ENGINE_STATE, p + "1_input_wide_color");
            m_qv_dump.add(device, cmd, wide_in.depth, ENGINE_STATE, p + "1_input_wide_depth");
            m_qv_dump.add(device, cmd, wide_in.mv, ENGINE_STATE, p + "1_input_wide_motion");
        }

        if (inset_in.ok) {
            m_qv_dump.add(device, cmd, inset_in.color, ENGINE_STATE, p + "2_input_fovea_color");
            m_qv_dump.add(device, cmd, inset_in.depth, ENGINE_STATE, p + "2_input_fovea_depth");
            m_qv_dump.add(device, cmd, inset_in.mv, ENGINE_STATE, p + "2_input_fovea_motion");
        }

        m_qv_dump.add(device, cmd, periphery, UAV, p + (ev.wide_override != nullptr ? "3r_periphery_reprojected_to_this_eye" : "3t_light_taa_output"));

        if (settings.dump_periphery_full) { // [PERIPHERY DUMP fix84]
            m_qv_dump.add_crop(p + (ev.wide_override != nullptr ? "3r_periphery_reprojected_to_this_eye" : "3t_light_taa_output"), 0, 0, 65535u, 65535u);
        }

        if (eye_src != nullptr) {
            m_qv_dump.add(device, cmd, eye_src, UAV, p + "3c_wide_eye_image_spatial_upscale");
        }

        if (use_fovea && fovea_taa && fovea_super_ran) {
            // [SUPER TAA 16.11.2026] the supersampling TAA's images of this eye: its history (linear light; the weight in the
            // alpha channel is not shown), what the passes hand each other, and the result
            m_qv_dump_header += std::format("  supersampling TAA ({} eye):\n{}", eye == 0 ? "left" : "right", m_super_taa.last_constants());

            const std::pair<ID3D12Resource*, const char*> images[]{
                {m_super_taa.history(slot), "4s_supertaa_history_linear"},
                {m_super_taa.linear(), "4s_supertaa_input_linear"},
                {m_super_taa.guide(), "4s_supertaa_guide_motion_weight_protection"},
                {m_super_taa.stats(slot), "4s_supertaa_statistics"},
                {m_super_taa.box_centre(), "4s_supertaa_box_centre"},
                {m_super_taa.box_extent(), "4s_supertaa_box_extent"},
                {m_super_taa.distances(slot), "4s_supertaa_distances_release_offset"},
                // (the mod's TAA beside it, when it ran in this frame)
                {m_qv_ftaa_frame[slot] == m_qv_frame ? m_qv_fovea_cmp[image_slot].Get() : nullptr, "4s_fovea_taa_image_beside"},
            };

            for (const auto& [res, name] : images) {
                if (res != nullptr) {
                    m_qv_dump.add(device, cmd, res, UAV, p + name);
                }
            }

            m_qv_dump.add(device, cmd, fovea_out, UAV, p + "4_output_fovea_supertaa_image");
        } else if (use_fovea && fovea_taa) {
            if (auto* hist = m_fovea_taa.history(slot); hist != nullptr) {
                m_qv_dump.add(device, cmd, hist, UAV, p + "4t_fovea_taa_history");
            }

            m_qv_dump.add(device, cmd, fovea_out, UAV, p + "4_output_fovea_taa_image");
        } else if (use_fovea) {
            m_qv_dump.add(device, cmd, fovea_out, UAV, p + "4_output_fovea_upscaler");

            if (plugin_linear_out != nullptr && m_qv_fsr && m_qv_fsr_atlas.valid) {
                // [FSR DIRECT 06.11.2026] both foveas' result once, with each half at full resolution
                m_qv_dump.add(device, cmd, plugin_linear_out, UAV, "C_4a_fsr4_result_linear_both_foveas");
                m_qv_dump.add_crop("C_4a_fsr4_result_linear_both_foveas", 0, 0, m_qv_layout.inset_w, m_qv_layout.inset_h);
                m_qv_dump.add_crop("C_4a_fsr4_result_linear_both_foveas", m_qv_fsr_atlas.right_out_x, 0, m_qv_layout.inset_w, m_qv_layout.inset_h);
            } else if (plugin_linear_out != nullptr) {
                m_qv_dump.add(device, cmd, plugin_linear_out, UAV, p + "4l_upscaler_result_linear_light");
            }
        }
    }

    // ---- [EYE COMPOSE] the eye image in one pass ----------------------------------------------------
    // [DIRECT 8 BIT 30.10.2026] into the headset texture itself when it has the eye image's size
    auto* direct_out = fused && ev.direct_out != nullptr && ev.direct_out->GetDesc().Width == img_w && ev.direct_out->GetDesc().Height == img_h
                           ? ev.direct_out
                           : nullptr;

    if (fused) {
        if (direct_out == nullptr &&
            !ensure_uav_image(device, m_qv_ui_out[image_slot], img_w, img_h, bb.Format, eye == 0 ? L"RE4VR eye image with interface left" : L"RE4VR eye image with interface right")) {
            set_qv_status("eye pass: could not create the eye image");
            qv.mark_failed(m_qv_init_generation, "eye pass: could not create the eye image");
            return false;
        }

        d3d12::EyeCompose::Params cp{};
        cp.fovea = use_fovea && have_rect;
        cp.rect_x = rect.x;
        cp.rect_y = rect.y;
        cp.rect_w = rect.w;
        cp.rect_h = rect.h;
        cp.feather_px = settings.feather * (float)(std::min)(rect.w, rect.h);
        cp.debug_border = settings.debug_border;
        cp.match_brightness = settings.brightness_match;
        cp.tone_curve = settings.brightness_match && settings.tone_match;
        cp.periphery_light = settings.periphery_light;
        cp.light_px = settings.light_px;
        cp.light_limit = settings.light_limit;       // [GLOW 13.11.2026]
        cp.light_add_only = settings.light_add_only;
        cp.light_auto = settings.light_auto;          // [AUTO LIGHT 14.11.2026]
        cp.view_metric = settings.view_metric;
        cp.gain_slot = slot;
        cp.gain_rate = 0.2f;
        cp.encode_periphery = encode != 0;
        cp.holes_to_fovea = ev.wide_override != nullptr && settings.mono_periphery && settings.hole_band; // [HOLE BAND 13.11.2026]
        cp.menu_backdrop = settings.menu_backdrop; // [MENU BACKDROP 15.11.2026]
        cp.ui = ev.ui_params;

        if (have_rect) {
            cp.ui.rect[0] = (float)rect.x;
            cp.ui.rect[1] = (float)rect.y;
            cp.ui.rect[2] = (float)rect.w;
            cp.ui.rect[3] = (float)rect.h;
            cp.ui.feather_px = settings.feather * (float)(std::min)(rect.w, rect.h);
        }

        cp.ui.post_sharp = settings.test_no_post_sharpen ? 0.0f : post_sharpness(); // [GPU 07.11.2026] (GPU pass test)
        cp.ui.post_edge = settings.test_no_post_sharpen ? 0.0f : post_edge_soften(); // [EDGE SOFTEN 11.11.2026]
        cp.ui.post_edge_threshold = post_edge_threshold();
        cp.ui.post_sat = post_saturation();
        cp.ui.post_gamma = post_brightness();
        cp.ui.post_contrast = post_contrast();
        const bool post_wanted = cp.ui.post_sharp > 0.0f || cp.ui.post_edge > 0.0f || cp.ui.post_sat != 1.0f || cp.ui.post_gamma != 1.0f || cp.ui.post_contrast != 1.0f;
        // [GPU TEST 29.10.2026] the GPU pass test's variants (the post pass left out still counts as done: the submit must
        // not run it instead, that would only move its time)
        cp.ui.post = post_wanted && !settings.test_no_post;
        cp.tile_skip = !settings.test_no_tile_skip;
        cp.tile_pass = !settings.test_tile_in_pass; // [TILE PASS fix105]
        cp.tile_none = settings.test_tile_none;     // [fix106]
        cp.ui_calls = settings.test_ui_calls;       // [fix108]
        cp.prepare_tests = settings.test_prepare;
        cp.two_pass = !settings.test_one_pass;      // [TWO PASSES fix111]
        cp.first_skips_marked = settings.test_first_skips; // [fix112, fix113, fix115] (the chain of fix113: a test variant)
        cp.marked_test = settings.test_marked_text;        // [MARKED TEXT fix115]
        cp.marked_small = settings.test_marked_small;      // [SMALL GROUPS fix117]
        cp.marked_over = settings.test_marked_over;        // [OVER fix122]
        cp.tiles_probe = !REVR_RELEASE; // [TILES PROBE fix114] (a diagnosis, for the periodic line; [RELEASE SWITCH fix141] not in the release)
        cp.variant = (d3d12::EyeCompose::Params::Variant)(std::min)(settings.test_eye_variant, (uint32_t)d3d12::EyeCompose::Params::Variant::Count - 1);
        const bool test_ui = !settings.test_no_interface;
        const d3d12::GuiMap* gui_map = settings.test_no_gui_map ? nullptr : ev.gui_map;

        const bool any_ui = test_ui && (ev.ui_centre_hud != nullptr || ev.ui_fovea_hud != nullptr);
        auto* out = direct_out != nullptr ? direct_out : m_qv_ui_out[image_slot].Get();
        cp.quantize_10bit = direct_out != nullptr;

        // the headset texture rests in PIXEL_SHADER_RESOURCE (the submit's conversion and copies leave it there)
        D3D12_RESOURCE_BARRIER direct_barrier{};
        direct_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        direct_barrier.Transition.pResource = direct_out;
        direct_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        direct_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        direct_barrier.Transition.StateAfter = UAV;

        if (direct_out != nullptr) {
            cmd->ResourceBarrier(1, &direct_barrier);
        }

        // [MENUS fix81 17.11.2026] the interface read where the frame's jitter put it
        // [MENUS fix82 17.11.2026] the fovea image holds its view's interface (the view went to the anti-aliasing as it was)
        if (test_ui && settings.steady_interface) {
            cp.fovea_ui_inside = ev.ui_fovea_inside && have_rect && use_fovea;
            cp.fovea_jitter[0] = inset_in.jitter_x;
            cp.fovea_jitter[1] = inset_in.jitter_y;
            cp.wide_jitter[0] = ev.ui_wide_jitter[0];
            cp.wide_jitter[1] = ev.ui_wide_jitter[1];
        }

        // [MENUS fix83 17.11.2026] with the interface inside the fovea image the pass gets the fovea view's copies with and
        // without GUI in the places of its colour and its copy without GUI
        const bool inside_pair = cp.fovea_ui_inside && ev.ui_inside_hud != nullptr && ev.ui_inside_fin != nullptr;
        cp.fovea_backdrop = settings.fovea_backdrop; // [BACKDROP fix83 17.11.2026]
        ID3D12Resource* eye_fovea_color = inside_pair ? ev.ui_inside_fin : (cp.fovea_ui_inside ? nullptr : ev.ui_fovea_color);
        ID3D12Resource* eye_fovea_hud = inside_pair ? ev.ui_inside_hud : (cp.fovea_ui_inside ? nullptr : ev.ui_fovea_hud);

        const bool composed = m_eye_compose.dispatch(device, cmd, periphery, use_fovea ? fovea_out : nullptr, out, test_ui ? ev.ui_centre_color : nullptr,
            test_ui ? ev.ui_centre_hud : nullptr, test_ui ? ev.ui_centre_fin : nullptr, test_ui && have_rect && use_fovea ? eye_fovea_color : nullptr,
            test_ui && have_rect && use_fovea ? eye_fovea_hud : nullptr, gui_map, cp);

        if (direct_out != nullptr) {
            std::swap(direct_barrier.Transition.StateBefore, direct_barrier.Transition.StateAfter);
            cmd->ResourceBarrier(1, &direct_barrier);
        }

        if (!composed) {
            const std::string why = std::string{"eye pass failed: "} + m_eye_compose.last_error();
            set_qv_status(why);
            qv.mark_failed(m_qv_init_generation, why);
            return false;
        }

        eye_src = out;
        m_qv_post_tex[eye] = post_wanted ? out : nullptr;

        // [CAPTURE fix81 17.11.2026] the finished eye image around the place looked at (the middle of the fovea rectangle),
        // and in the first frames what the fovea's interface was made from; the left eye
        if (eye == 0 && m_sc.recording()) {
            const auto od = out->GetDesc();
            const uint32_t ow = (uint32_t)od.Width;
            const uint32_t oh = od.Height;
            const uint32_t side = m_sc.part();
            const uint32_t pw = (std::min)(side, ow) & ~1u;
            const uint32_t ph = (std::min)(side, oh) & ~1u;
            const float ecx = have_rect ? (float)rect.x + 0.5f * (float)rect.w : 0.5f * (float)ow;
            const float ecy = have_rect ? (float)rect.y + 0.5f * (float)rect.h : 0.5f * (float)oh;
            const uint32_t px = (uint32_t)std::clamp((int32_t)std::lround(ecx - 0.5f * (float)pw), 0, (int32_t)(ow - pw)) & ~1u;
            const uint32_t py = (uint32_t)std::clamp((int32_t)std::lround(ecy - 0.5f * (float)ph), 0, (int32_t)(oh - ph)) & ~1u;
            m_sc.extra(device, cmd, out, direct_out != nullptr ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : UAV, "eye_out", px, py, pw, ph);

            // [CAPTURE fix82 17.11.2026] and beside the fovea rectangle, where the periphery's chain of this capture was taken
            // (m_sc_side: the periphery as the eye image shows it, after the enlargement and the post pass)
            {
                const uint32_t sx = (uint32_t)std::clamp((int32_t)std::lround(m_sc_side[0] - 0.5f * (float)pw), 0, (int32_t)(ow - pw)) & ~1u;
                const uint32_t sy = (uint32_t)std::clamp((int32_t)std::lround(m_sc_side[1] - 0.5f * (float)ph), 0, (int32_t)(oh - ph)) & ~1u;
                m_sc.extra(device, cmd, out, direct_out != nullptr ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : UAV, "eye_out_side", sx, sy, pw, ph);
            }

            if (m_sc.frame() < 8 && test_ui && have_rect && use_fovea) {
                // (the fovea view's images: the middle of the view, as the supersampling TAA's part)
                const auto middle = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES state, const char* name) {
                    if (res != nullptr) {
                        const auto d = res->GetDesc();
                        const uint32_t w = (std::min)(side, (uint32_t)d.Width) & ~1u;
                        const uint32_t h = (std::min)(side, d.Height) & ~1u;
                        m_sc.extra(device, cmd, res, state, name, (((uint32_t)d.Width - w) / 2u) & ~1u, ((d.Height - h) / 2u) & ~1u, w, h);
                    }
                };
                middle(ev.ui_fovea_color, ENGINE_STATE, "ui_fovea_color");
                middle(ev.ui_fovea_hud, D3D12_RESOURCE_STATE_COMMON, "ui_fovea_hud");
                // [MENUS fix83] the copies the eye pass tells the fovea's interface by (the interface inside the fovea image)
                middle(ev.ui_inside_hud, D3D12_RESOURCE_STATE_COMMON, "ui_fovea_hud");
                middle(ev.ui_inside_fin, D3D12_RESOURCE_STATE_COMMON, "ui_fovea_fin");
            }

            m_sc.note(std::format("eye_out: eye 0 image {} {} part {} {} {} {} fovea_drawn {} fovea_rectangle {} {} {} {} {} fovea_jitter {:.9g} {:.9g} "
                                  "wide_jitter {:.9g} {:.9g} steady_interface {} interface_in_fovea {} debug_view {} post {}",
                ow, oh, px, py, pw, ph, use_fovea && have_rect ? 1 : 0, have_rect ? 1 : 0, rect.x, rect.y, rect.w, rect.h, cp.fovea_jitter[0], cp.fovea_jitter[1],
                cp.wide_jitter[0], cp.wide_jitter[1], settings.steady_interface ? 1 : 0, cp.fovea_ui_inside ? 1 : 0,
                settings.debug_view, cp.ui.post ? 1 : 0));
        }

        // [VIEW DIFFERENCE 01.11.2026] the measurement of some frames ago, for the tests
        if (float vd[4]{}; settings.view_metric && m_eye_compose.view_metric(slot, vd)) {
            qv.view_diff[slot].store(vd[0]);
            qv.view_diff_matched[slot].store(vd[1]);
            ++qv.view_diff_frames;
            qv.view_gain[slot].store(vd[3]); // [EV FOLLOW 13.11.2026]
            ++qv.view_gain_frames[slot];
        }

        if (any_ui) {
            ++qv.ui_composed;
        } else {
            ++qv.ui_missing;
        }

        if (use_fovea && have_rect && m_eye_compose.fovea_drawn()) {
            ++qv.evaluations;
            // ([PERIPHERY SUPER fix80] ... and what anti-aliases the shared periphery when it is not the light TAA)
            const uint32_t periphery_aa_now = m_periphery_running.load(std::memory_order_relaxed);
            set_qv_status(std::format("active: render {}x{} x{}, fovea {}x{}{}{}", m_qv_layout.render_w, m_qv_layout.render_h, m_qv_mono ? 3 : 4,
                m_qv_layout.inset_w, m_qv_layout.inset_h, fovea_taa ? ", fovea: mod TAA" : (m_qv_fsr ? (m_qv_fsr_atlas.valid ? ", fovea: FSR 4 by the mod, one pass" : ", fovea: FSR 4 by the mod, a pass per eye") : ", fovea: upscaler plugin"),
                !m_qv_mono ? ", a wide view per eye"
                : periphery_aa_now == 2 ? ", shared periphery (supersampling TAA, 2x image)"
                : periphery_aa_now == 1 ? ", shared periphery (supersampling TAA)"
                : m_periphery_calm.load(std::memory_order_relaxed) ? ", shared periphery (calm light TAA)" : ", shared periphery"));
        } else if (use_fovea && have_rect) {
            ++qv.skipped;
            set_qv_status(std::string{"composite failed: "} + m_eye_compose.last_error());
        } else if (use_fovea) {
            ++qv.skipped;
            set_qv_status("no fovea rectangle for this eye yet");
        } else if (!inset_in.ok) {
            ++qv.skipped;
            set_qv_status(std::format("waiting for the fovea view of eye {} (scene layers found: {})", eye, m_layers_in_use));
        } else {
            ++qv.evaluations;
            set_qv_status("debug: periphery only");
        }
    }

    // ---- composite ----------------------------------------------------------------------------------
    if (fused) {
        // done in the eye pass
    } else if (use_fovea) {
        if (have_rect) {
            d3d12::FoveaComposite::Params cp{};
            cp.rect_x = rect.x;
            cp.rect_y = rect.y;
            cp.rect_w = rect.w;
            cp.rect_h = rect.h;
            cp.feather_px = settings.feather * (float)(std::min)(rect.w, rect.h);
            cp.debug_border = settings.debug_border;
            cp.match_brightness = settings.brightness_match;
            cp.gain_slot = slot;
            cp.gain_rate = 0.2f;

            if (m_fovea_composite.dispatch(device, cmd, eye_src, fovea_out, cp)) {
                ++qv.evaluations;
                set_qv_status(std::format("active: render {}x{} x{}, fovea {}x{}{}{}", m_qv_layout.render_w, m_qv_layout.render_h, m_qv_mono ? 3 : 4,
                    m_qv_layout.inset_w, m_qv_layout.inset_h, fovea_taa ? ", fovea: mod TAA" : (m_qv_fsr ? (m_qv_fsr_atlas.valid ? ", fovea: FSR 4 by the mod, one pass" : ", fovea: FSR 4 by the mod, a pass per eye") : ", fovea: upscaler plugin"),
                    m_qv_mono ? ", shared periphery" : ", a wide view per eye"));
            } else {
                ++qv.skipped;
                set_qv_status(std::string{"composite failed: "} + m_fovea_composite.last_error());
            }
        } else {
            ++qv.skipped;
            set_qv_status("no fovea rectangle for this eye yet");
        }
    } else if (!inset_in.ok) {
        ++qv.skipped;
        set_qv_status(std::format("waiting for the fovea view of eye {} (scene layers found: {})", eye, m_layers_in_use));
    } else {
        ++qv.evaluations;
        set_qv_status("debug: periphery only");
    }

    // ---- [MONO UI] the interface of this eye (the wide view's outside the fovea, the fovea view's inside) -----------
    if (ev.mono_ui && !fused) {
        const auto ed = eye_src->GetDesc();
        auto up = ev.ui_params;

        if (have_rect) {
            up.rect[0] = (float)rect.x;
            up.rect[1] = (float)rect.y;
            up.rect[2] = (float)rect.w;
            up.rect[3] = (float)rect.h;
            up.feather_px = settings.feather * (float)(std::min)(rect.w, rect.h);
        }

        const bool any_ui = ev.ui_centre_hud != nullptr || ev.ui_fovea_hud != nullptr;

        // [POST IN UI 28.10.2026] the post pass of the eye images (sharpness, saturation, brightness, contrast) runs in this
        // pass, which reads and writes the whole eye image anyway; the submit then leaves it out for this image
        up.post_sharp = settings.test_no_post_sharpen ? 0.0f : post_sharpness(); // [GPU 07.11.2026] (GPU pass test)
        up.post_edge = settings.test_no_post_sharpen ? 0.0f : post_edge_soften(); // [EDGE SOFTEN 11.11.2026]
        up.post_edge_threshold = post_edge_threshold();
        up.post_sat = post_saturation();
        up.post_gamma = post_brightness();
        up.post_contrast = post_contrast();
        up.post = up.post_sharp > 0.0f || up.post_edge > 0.0f || up.post_sat != 1.0f || up.post_gamma != 1.0f || up.post_contrast != 1.0f;

        if (!any_ui) {
            ++qv.ui_missing;
        }

        if ((any_ui || up.post) &&
            ensure_uav_image(device, m_qv_ui_out[image_slot], (uint32_t)ed.Width, ed.Height, ed.Format, eye == 0 ? L"RE4VR eye image with interface left" : L"RE4VR eye image with interface right")) {
            if (m_mono_ui.dispatch(device, cmd, eye_src, m_qv_ui_out[image_slot].Get(), ev.ui_centre_color, ev.ui_centre_hud, ev.ui_centre_fin,
                    have_rect && use_fovea ? ev.ui_fovea_color : nullptr, have_rect && use_fovea ? ev.ui_fovea_hud : nullptr, up)) {
                eye_src = m_qv_ui_out[image_slot].Get();

                if (any_ui) {
                    ++qv.ui_composed;
                }

                m_qv_post_tex[eye] = up.post ? eye_src : nullptr;
            } else {
                static bool once = true;

                if (once) {
                    REVR_LOG(spdlog::error("[TemporalUpscaler] interface composition failed: {}", m_mono_ui.last_error()));
                    once = false;
                }
            }
        }
    }

    // the finished eye image goes to the multipass submit (D3D12Component); the desktop mirror gets the left eye (copied
    // after both eyes, evaluate_quadview)
    m_qv_eye_image[eye] = eye_src;
    m_qv_eye_fresh[eye] = true;
    spans.stamp(cmd, (uint64_t)frame, eye == 0 ? d3d12::GpuSpans::COPIER_EYE_L : d3d12::GpuSpans::COPIER_EYE_R);

    // (not the headset texture: the desktop window takes the 10-bit image)
    if (ev.to_backbuffer && eye_src != direct_out) {
        m_qv_mirror_src = eye_src;
    }

    if (dump) {
        const std::string p = ev.dump_prefix;
        m_qv_dump.add(device, cmd, eye_src, UAV, p + "5_eye_image_composited");

        // [FOVEA EDGE 01.11.2026] the first frame of a dump also at full resolution where the fovea meets the periphery:
        // the rectangle and 128 pixels around it from the periphery's eye image and the eye image, the fovea image and its
        // depth whole (the band analysis compares them pixel by pixel)
        if (settings.dump_periphery_full && eye == 0) { // [PERIPHERY DUMP fix84] the left eye's finished image, whole
            m_qv_dump.add_crop(p + "5_eye_image_composited", 0, 0, 65535u, 65535u);
        }

        if (df.index == 0 && have_rect && use_fovea) {
            constexpr int M = 128;
            const uint32_t cx = (uint32_t)(std::max)(rect.x - M, 0);
            const uint32_t cy = (uint32_t)(std::max)(rect.y - M, 0);
            const uint32_t cw = (uint32_t)(rect.w + 2 * M);
            const uint32_t ch = (uint32_t)(rect.h + 2 * M);
            m_qv_dump.add_crop(p + "3c_wide_eye_image_spatial_upscale", cx, cy, cw, ch);
            m_qv_dump.add_crop(p + "5_eye_image_composited", cx, cy, cw, ch);
            m_qv_dump.add_crop(p + (fovea_super_ran ? "4_output_fovea_supertaa_image" : (fovea_taa ? "4_output_fovea_taa_image" : "4_output_fovea_upscaler")), 0, 0, m_qv_layout.inset_w, m_qv_layout.inset_h);
            m_qv_dump.add_crop(p + "2_input_fovea_depth", 0, 0, m_qv_layout.render_w, m_qv_layout.render_h);
            m_qv_dump_header += std::format("  {}eye: fovea rectangle {},{} {}x{}, blend width {:.0f} px (feather {:.2f}), brightness match {}, per brightness {}, light from the periphery {} ({:.0f} px), "
                                            "post: sharpness {:.2f} edge softening {:.2f} (threshold {:.2f}) "
                                            "saturation {:.2f} brightness {:.2f} contrast {:.2f} | gaps covered by the fovea in the band {}\n",
                p, rect.x, rect.y, rect.w, rect.h, settings.feather * (float)(std::min)(rect.w, rect.h), settings.feather, settings.brightness_match,
                settings.brightness_match && settings.tone_match, settings.periphery_light, settings.light_px, post_sharpness(), post_edge_soften(), post_edge_threshold(), post_saturation(),
                post_brightness(), post_contrast(), settings.mono_periphery && settings.hole_band);
        } else if (have_rect && use_fovea) {
            // [FOVEA EDGE 01.11.2026] the other frames of a dump (moving the eyes): only the edges at full resolution, 64 pixels
            // of each side of the fovea view as the engine made it and as the fovea pass left it, and of the eye image 32
            // pixels on both sides of each side of the rectangle (where the band starts)
            constexpr uint32_t S = 64;
            const auto strips = [&](const std::string& name, uint32_t w, uint32_t h) {
                m_qv_dump.add_crop(name, 0, 0, w, S);
                m_qv_dump.add_crop(name, 0, h > S ? h - S : 0, w, S);
                m_qv_dump.add_crop(name, 0, 0, S, h);
                m_qv_dump.add_crop(name, w > S ? w - S : 0, 0, S, h);
            };
            strips(p + "2_input_fovea_color", m_qv_layout.render_w, m_qv_layout.render_h);
            strips(p + (fovea_super_ran ? "4_output_fovea_supertaa_image" : (fovea_taa ? "4_output_fovea_taa_image" : "4_output_fovea_upscaler")), m_qv_layout.inset_w, m_qv_layout.inset_h);
            const uint32_t ex = (uint32_t)(std::max)(rect.x - 32, 0);
            const uint32_t ey = (uint32_t)(std::max)(rect.y - 32, 0);
            const uint32_t ew = rect.w + 64;
            const uint32_t eh = rect.h + 64;
            m_qv_dump.add_crop(p + "5_eye_image_composited", ex, ey, ew, S);
            m_qv_dump.add_crop(p + "5_eye_image_composited", ex, ey + eh - S, ew, S);
            m_qv_dump.add_crop(p + "5_eye_image_composited", ex, ey, S, eh);
            m_qv_dump.add_crop(p + "5_eye_image_composited", ex + ew - S, ey, S, eh);
            m_qv_dump_header += std::format("  {}eye: fovea rectangle {},{} {}x{}, blend width {:.0f} px (feather {:.2f}) (edges at full resolution)\n", p, rect.x,
                rect.y, rect.w, rect.h, settings.feather * (float)(std::min)(rect.w, rect.h), settings.feather);
        }
    }

    return true;
}
