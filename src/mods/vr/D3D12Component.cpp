#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>
#include <openvr.h>
#include <utility/ScopeGuard.hpp>
#include <utility/Profiler.hpp>

#include "../VR.hpp"
#include "../TemporalUpscaler.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>
#include <../../directxtk12-src/Src/d3dx12.h>

#include "d3d12/DirectXTK.hpp"
#include "d3d12/TextureDump.hpp" // [QUADVIEW] debug frame dump
#include "d3d12/GpuTimer.hpp"    // [GPU BREAKDOWN] GpuSpans
#include "d3d12/SubmitTimeline.hpp" // [SUBMIT TIMELINE 06.11.2026]
#include <cmath>
#include <cstring>
#include <format>

#include "D3D12Component.hpp"
#include "CpuStats.hpp" // [SUBMIT 05.11.2026]

namespace {
// [SUBMIT 05.11.2026] the compositor's Submit, timed (cpustats::VrSubmit)
vr::EVRCompositorError timed_submit(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags flags = vr::Submit_Default) {
#if REVR_RELEASE
    // [RELEASE SWITCH fix141 23.11.2026] the release does not time the Submit
    return vr::VRCompositor()->Submit(eye, texture, bounds, flags);
#else
    const auto t0 = cpustats::now_ns();
    d3d12::SubmitTimeline::Tag tag{eye == vr::Eye_Left ? "SteamVR submit L" : "SteamVR submit R"}; // [SUBMIT TIMELINE 06.11.2026]
    const auto e = vr::VRCompositor()->Submit(eye, texture, bounds, flags);
    const auto dt = (uint64_t)(std::max)(cpustats::now_ns() - t0, (int64_t)0);
    auto& s = cpustats::vr_submit();
    s.submits.fetch_add(1, std::memory_order_relaxed);
    s.submit_ns.fetch_add(dt, std::memory_order_relaxed);
    cpustats::store_max(s.submit_max_ns, dt);

    if (dt > 20'000'000) {
        s.submit_slow.fetch_add(1, std::memory_order_relaxed);
    }

    return e;
#endif
}
} // namespace

QUADVIEW_BUILD_STAMP(D3D12Component); // [BUILD STAMP fix107]

namespace vrmod {
// [POST_PASS 28.09.2026 -- Ansage des Users: AA/Schaerfe/Saettigung auch OHNE
// Upscaler, wie ReShade] Laeuft auf den fertigen Multipass-Augenbildern, bevor
// sie in die OpenXR-/OpenVR-Swapchain kopiert werden. Mit Upscaler stehen die
// Texturen in UNORDERED_ACCESS, ohne in COPY_DEST (so uebergeben sie die Kopien
// unten). SMAA nur ohne Upscaler.
void D3D12Component::apply_post_pass(VR* vr, ID3D12Device* device, uint64_t frame_count) {
    const auto& upscaler = TemporalUpscaler::get();

    if (upscaler == nullptr) {
        return;
    }

    // [MIT UPSCALER 28.09.2026 -- Ansage des Users] Schaerfe/Saettigung gelten
    // auch mit Upscaler. Die Augenbilder stehen dann in UNORDERED_ACCESS (so
    // uebergeben sie die Kopien unten), ohne Upscaler in COPY_DEST. SMAA laeuft
    // mit Upscaler NICHT -- der glaettet selbst (Toggle ist dann aus/ausgegraut).
    const bool upscaling = upscaler->ready();
    const auto eye_state = upscaling ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;

    auto* tex0 = submit_eye_image(vr, 0);
    auto* tex1 = submit_eye_image(vr, 1);

    if (tex0 == nullptr || tex1 == nullptr) {
        return;
    }

    const float sharp = upscaler->post_sharpness();
    const float edge = upscaler->post_edge_soften(); // [EDGE SOFTEN 11.11.2026]
    const float edge_threshold = upscaler->post_edge_threshold();
    const float sat = upscaler->post_saturation();
    const bool smaa = upscaler->post_smaa() && !upscaling;
    const float bright = upscaler->post_brightness();
    const float contrast = upscaler->post_contrast();
    const bool post_active = smaa || sharp > 0.0f || edge > 0.0f || sat != 1.0f || bright != 1.0f || contrast != 1.0f;

    // [UI_TRENNUNG 28.09.2026 -- Ansage des Users: nur Multipass + Upscaler]
    // Beide Engine-Kopien (ohne/mit UI) muessen in diesem Frame da sein.
    ID3D12Resource* ui_hud[2]{};
    ID3D12Resource* ui_fin[2]{};
    bool ui_sep[2]{false, false};

    // [MONO UI 27.10.2026] with the shared periphery the interface is already in the eye images (TemporalUpscaler, MonoUi):
    // the copies of [0] / [1] are the fovea views', whose interface covers only the fovea (it was restored here as if it
    // covered the whole eye: the giant title logo of the test of 27.10.2026)
    if (upscaling && !vr->is_mono_periphery()) {
        for (int e = 0; e < 2; ++e) {
            auto& cap = vr->m_ui_capture[e];

            // [FLACKERN 28.09.2026] NICHT mehr pro Frame zuruecksetzen: die Engine
            // rendert zeitversetzt zur Abgabe, dann fehlte die Markierung jeden
            // zweiten Frame und das UI sprang zwischen "unseres" und "DLSS".
            // Sobald beide Kopien einmal geschrieben wurden, immer die letzten nehmen.
            // [PAARE 28.09.2026] Nur ein Paar aus DEMSELBEN Engine-Frame benutzen.
            // Gemischte Frames erzeugten Ghosting in bewegten Bereichen (Vegetation).
            if (cap.hudless_ok && cap.final_ok) {
                const uint64_t hf = cap.hudless_frame.load();
                const uint64_t ff = cap.final_frame.load();

                if (hf != ff) {
                    ++vr->m_ui_pair_bad[e];
                } else {
                    if (cap.used_frame.load() == ff) {
                        ++vr->m_ui_pair_old[e];   // Paar ist alt, aber passend -> trotzdem nehmen (kein Flackern)
                    } else {
                        ++vr->m_ui_pair_ok[e];
                        cap.used_frame = ff;
                    }

                    ui_hud[e] = vr->ui_capture_resource(e, false);
                    ui_fin[e] = vr->ui_capture_resource(e, true);
                    ui_sep[e] = ui_hud[e] != nullptr && ui_fin[e] != nullptr;
                }
            }
        }
    }

    // [POST IN UI 28.10.2026] the quad view's interface pass did the post pass on these eye images already, or the image
    // is the quad view's from an earlier frame (submitted again unchanged: its post pass is in it already)
    // [DIRECT 8 BIT 30.10.2026] an eye image that is the headset texture (written by the quad view's eye pass with its post
    // pass in it, resting in PIXEL_SHADER_RESOURCE) is left alone here
    const bool direct_eye[2]{is_direct_texture(0, tex0), is_direct_texture(1, tex1)};

    for (int e = 0; e < 2; ++e) {
        ui_sep[e] = ui_sep[e] && !direct_eye[e];
    }

    const bool post_eye[2]{!direct_eye[0] && post_active && !upscaler->qv_post_applied(0, tex0) && upscaler->qv_take_fresh(0, tex0),
                           !direct_eye[1] && post_active && !upscaler->qv_post_applied(1, tex1) && upscaler->qv_take_fresh(1, tex1)};

    if (!post_eye[0] && !post_eye[1] && !ui_sep[0] && !ui_sep[1] && !(smaa && !direct_eye[0] && !direct_eye[1])) {
        return;
    }

    auto& commands = m_post_commands[frame_count % m_post_commands.size()];

    if (commands.cmd_list == nullptr) {
        return;
    }

    commands.wait(INFINITE);

    // [GPU BREAKDOWN] two stamps bracket the post pass (interface restore, sharpness) of both eyes
    auto& spans = d3d12::GpuSpans::get();
    spans.stamp(commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::POST_BEGIN);

    // Reihenfolge: SMAA (nur ohne Upscaler), [UI_TRENNUNG] UI zurueck, dann
    // Brightness/Contrast/Schaerfe/Saettigung auf alles (inkl. UI).
    bool any = spans.enabled();
    ID3D12Resource* texs[2]{tex0, tex1};

    for (int e = 0; e < 2; ++e) {
        if (smaa && !direct_eye[e]) {
            any |= m_smaa_pass.dispatch_inplace(device, commands.cmd_list.Get(), texs[e], eye_state);
        }

        // [REIHENFOLGE 28.09.2026 -- Ansage des Users: unsere Shader sollen das UI
        // mit treffen] Erst das UI unverschmiert zurueck, DANN Brightness/Contrast/
        // Sharpness/Saturation aufs ganze Bild inkl. UI.
        if (ui_sep[e]) {
            // [UI_FARBE 28.09.2026] UI-Farbe aus der Upscaler-Kopie NACH PrepareOutput
            // (Kopie "final" liegt VOR PrepareOutput -> ohne Ausgabe-Gamma = zu dunkel).
            auto* ui_col = upscaler->get_color_copy_for(vr->m_ui_capture[e].scene_layer.load());

            any |= m_ui_restore.dispatch_inplace(device, commands.cmd_list.Get(), texs[e], eye_state,
                                                 ui_col != nullptr ? ui_col : ui_fin[e], true, ui_hud[e], ui_fin[e], e,
                                                 vr->m_ui_debug_mask.load());
        }

        if (post_eye[e]) {
            any |= m_post_pass.dispatch_inplace(device, commands.cmd_list.Get(), texs[e], eye_state, sharp, sat, bright, contrast, edge, edge_threshold);
        }
    }

    spans.stamp(commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::POST_END);
    spans.resolve(commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::POST_BEGIN, 2);

    if (any) {
        commands.has_commands = true;
        commands.execute();
    }
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [QUADVIEW] Writes the left and right OpenVR eye textures of this frame into the folder of the frame the upscaler
// just dumped. Called after both eye textures got their content, right before the submit.
void D3D12Component::dump_quadview_eyes(ID3D12Device* device) {
    quadview::DumpFrame frame{};

    if (!quadview::State::get().take_eye_dump(frame)) {
        return;
    }

    auto& cmds = m_qv_dump_commands;

    if (cmds.cmd_list == nullptr && !cmds.setup(L"Tri-view eye dump")) {
        quadview::State::get().push_dump_report("  submitted eyes: could not create a command list\n");
        return;
    }

    cmds.wait(INFINITE);

    d3d12::TextureDump dump{};
    // the OpenVR eye textures rest in PIXEL_SHADER_RESOURCE (see OpenVR::copy_left/right)
    dump.add(device, cmds.cmd_list.Get(), m_openvr.get_left().texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "6_submitted_LEFT_eye");
    dump.add(device, cmds.cmd_list.Get(), m_openvr.get_right().texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "6_submitted_RIGHT_eye");
    cmds.has_commands = true;
    cmds.execute();
    cmds.wait(INFINITE);

    const auto& eye_tex = m_openvr.get_left().texture;
    std::string size{};

    if (eye_tex != nullptr) {
        const auto desc = eye_tex->GetDesc();
        size = std::format(" ({}x{}, format {})", desc.Width, desc.Height, (int)desc.Format);
    }

    const auto& vr = VR::get();
    const char* note = vr->is_mono_periphery() ? " (left = shared wide view reprojected + clone 0, right = shared wide view reprojected + clone 1)"
                       : vr->is_stereo_fovea()   ? " (left = primary camera + clone 1, right = clone 0 + clone 2)"
                                                 : " (left = primary camera, right = clone 0)";
    const auto bl = vr->get_shifted_bounds(false);
    const auto br = vr->get_shifted_bounds(true);
    quadview::State::get().push_dump_report(std::format("  images that went to the headset in this frame{}{}:\n", note, size) +
        std::format("    submit bounds L u {:.3f}..{:.3f} v {:.3f}..{:.3f} | R u {:.3f}..{:.3f} v {:.3f}..{:.3f} | blank eye {} | all eyes blank {}\n",
            bl.uMin, bl.uMax, bl.vMin, bl.vMax, br.uMin, br.uMax, br.vMin, br.vMax, vr->get_blank_eye(), vr->should_blank_all_eyes()) +
        dump.write_all(frame.dir, 1024));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [MULTIPASS DUMP 13.11.2026] The plain multipass (quad view off) gives the two eyes different pictures in every RE VR
// game (user, 12.11.2026: one eye brighter, the other darker; shadows, effects and even grass missing in one eye). The
// quad view dump runs in the quad view's own pass only; this one writes the two eye images that went to the headset, at
// full resolution, for a few frames in a row: left = the game camera, right = clone 0. With it the camera check (framework
// log: the game camera's components against clone 0's) and the scene layers each camera rendered (foveation_debug.log).
void D3D12Component::dump_multipass_eyes(ID3D12Device* device) {
    auto& qv = quadview::State::get();

    if (qv.multipass_dump_request.load(std::memory_order_relaxed) <= 0) {
        return;
    }

    if (qv.engaged()) {
        qv.multipass_dump_request = 0;
        qv.push_dump_report("  plain multipass dump: not taken, the tri-view is on (its own dump covers it)\n");
        return;
    }

    const int left = qv.multipass_dump_request.fetch_sub(1);

    if (left <= 0) {
        return;
    }

    std::error_code ec{};

    // a new folder for each request: multipass_dumpNN next to the quad view dumps
    if (m_mp_dump_dir.empty() || m_mp_dump_frame == 0) {
        const auto base = REFramework::get_persistent_dir("quadview_dump");
        std::filesystem::create_directories(base, ec);

        for (int n = 1; n < 1000; ++n) {
            auto dir = base / std::format("multipass_dump{:02}", n);

            if (!std::filesystem::exists(dir, ec)) {
                m_mp_dump_dir = dir;
                break;
            }
        }
    }

    const auto dir = m_mp_dump_dir / std::format("frame{}", m_mp_dump_frame);
    std::filesystem::create_directories(dir, ec);

    auto& cmds = m_qv_dump_commands;

    if (cmds.cmd_list == nullptr && !cmds.setup(L"Tri-view eye dump")) {
        qv.push_dump_report("  plain multipass dump: could not create a command list\n");
        qv.multipass_dump_request = 0;
        m_mp_dump_frame = 0;
        return;
    }

    cmds.wait(INFINITE);

    d3d12::TextureDump dump{};
    // the OpenVR eye textures rest in PIXEL_SHADER_RESOURCE (see OpenVR::copy_left/right)
    dump.add(device, cmds.cmd_list.Get(), m_openvr.get_left().texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "multipass_LEFT_eye_game_camera");
    dump.add(device, cmds.cmd_list.Get(), m_openvr.get_right().texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "multipass_RIGHT_eye_clone0");
    cmds.has_commands = true;
    cmds.execute();
    cmds.wait(INFINITE);

    std::string size{};

    if (const auto& eye_tex = m_openvr.get_left().texture; eye_tex != nullptr) {
        const auto desc = eye_tex->GetDesc();
        size = std::format("{}x{}, format {}", desc.Width, desc.Height, (int)desc.Format);
    }

    const auto& vr = VR::get();
    const auto bl = vr->get_shifted_bounds(false);
    const auto br = vr->get_shifted_bounds(true);
    std::string report = std::format("  PLAIN MULTIPASS DUMP frame {} ({}), folder {}\n", m_mp_dump_frame, size, dir.string()) +
        std::format("    multipass {}, left = game camera, right = clone 0 | submit bounds L u {:.3f}..{:.3f} v {:.3f}..{:.3f} | R u {:.3f}..{:.3f} "
                    "v {:.3f}..{:.3f} | blank eye {}\n",
            vr->is_using_multipass(), bl.uMin, bl.uMax, bl.vMin, bl.vMax, br.uMin, br.uMax, br.vMin, br.vMax, vr->get_blank_eye()) +
        dump.write_all(dir, 8192); // full resolution

    qv.push_dump_report(report);
    REVR_LOG(spdlog::info("[VR] plain multipass eye dump written to {}", dir.string()));

    ++m_mp_dump_frame;

    if (left <= 1) {
        m_mp_dump_frame = 0; // the next request gets a new folder
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [VIDEO 25.10.2026] see d3d12::EyeRecorder. Same place as the dump: both eye textures have their content of this frame
// and rest in PIXEL_SHADER_RESOURCE; the recording goes on its own command lists on the same queue.
void D3D12Component::record_eyes(ID3D12Device* device) {
    auto& qv = quadview::State::get();
    double seconds = 0.0;

    if (qv.take_video_request(seconds) && !m_recorder.start(seconds, REFramework::get_persistent_dir("quadview_dump"))) {
        qv.set_video_status("a video is still being written, try again in a moment");
        m_recorder_status.clear();
    }

    // [LIGHT FLICKER TEST 15.11.2026] a flicker measurement in the recorder instead of a video (one at a time: when it is
    // busy, an empty result ends the test)
    if (double measure_seconds = 0.0; qv.take_flicker_measure_request(measure_seconds) && !m_recorder.start_measure(measure_seconds)) {
        qv.set_flicker_rows({});
    }

    m_recorder.set_measure_tag(qv.flicker_tag.load(std::memory_order_relaxed));

    // [LIGHT FLICKER TEST 15.11.2026 evening] the fovea rectangles in the measured image (each eye 424 x 432 of 848 x 432),
    // 8 of its pixels (about 64 of the eye image's) wider on each side: the rectangle moves with the gaze between frames
    if (m_recorder.busy()) {
        d3d12::EyeRecorder::MeasureMask mask{};
        const auto layout = qv.layout();

        if (layout.valid && layout.display_w > 0 && layout.display_h > 0) {
            for (int e = 0; e < 2; ++e) {
                quadview::Rect r{};

                if (!qv.rect_for_eye(e, r) || r.w == 0 || r.h == 0) {
                    continue;
                }

                const double sx = 424.0 / (double)layout.display_w;
                const double sy = 432.0 / (double)layout.display_h;
                mask.x0[e] = e * 424 + (int32_t)std::floor((double)r.x * sx) - 8;
                mask.x1[e] = e * 424 + (int32_t)std::ceil((double)(r.x + (int32_t)r.w) * sx) + 8;
                mask.y0[e] = (int32_t)std::floor((double)r.y * sy) - 8;
                mask.y1[e] = (int32_t)std::ceil((double)(r.y + (int32_t)r.h) * sy) + 8;
                mask.valid[e] = true;
            }
        }

        m_recorder.set_measure_mask(mask);
    }

    if (m_recorder.busy()) {
        m_recorder.on_frame(device, m_openvr.get_left().texture.Get(), m_openvr.get_right().texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (std::vector<d3d12::EyeRecorder::MeasureRow> rows{}; m_recorder.take_measure(rows)) {
        std::vector<quadview::State::FlickerRow> out{};

        for (const auto& r : rows) {
            quadview::State::FlickerRow f{};
            f.tag = r.tag;
            f.frames = r.frames;
            f.flips = r.flips;
            f.cell_x = r.cell_x;
            f.cell_y = r.cell_y;
            f.cell_flips = r.cell_flips;
            f.peri_flips = r.peri_flips; // [LIGHT FLICKER TEST 15.11.2026 evening]
            f.peri_cell_x = r.peri_cell_x;
            f.peri_cell_y = r.peri_cell_y;
            f.peri_cell_flips = r.peri_cell_flips;
            f.series_cell_x = r.series_cell_x; // [LIGHTS fix86 18.11.2026]
            f.series_cell_y = r.series_cell_y;
            f.series = r.series;
            f.moved = r.moved;
            out.push_back(std::move(f));
        }

        qv.set_flicker_rows(std::move(out));
    }

    auto status = m_recorder.status();

    if (status != m_recorder_status) {
        qv.set_video_status(status);
        m_recorder_status = std::move(status);
    }
}
#endif

vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    REF_PROFILE_FUNCTION();

    if (m_openvr.left_eye_tex[0].texture == nullptr || m_force_reset) {
        setup();
    }

    auto& hook = g_framework->get_d3d12_hook();
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};

    const auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)))) {
        REVR_LOG(spdlog::error("[VR] Failed to get back buffer"));
        return vr::VRCompositorError_None;
    }

    if (backbuffer == nullptr) {
        REVR_LOG(spdlog::error("[VR] Failed to get back buffer."));
        return vr::VRCompositorError_None;
    }

    // TODO: Correct this for the upscaler...?
    // [MIRROR COPY 27.10.2026] In multipass the eyes come from the eye textures; the converted backbuffer is only read by
    // the flatscreen canvas. Without the canvas the copy of the desktop window and its 8-bit conversion (every frame) went
    // nowhere.
    const bool multipass_eyes = vr->is_using_multipass() && vr->m_multipass.eye_textures[0] != nullptr && vr->m_multipass.eye_textures[1] != nullptr;

    // [SMALL WINDOW fix122 22.11.2026] With the game's window smaller than the eye image (see VR::on_view_get_size) the
    // desktop window's copy and its 8-bit image above have the eye image's size and cannot take the backbuffer: the canvas
    // has a pair of the window's size (made in setup; multipass only, where nothing else reads the converted backbuffer).
    // A copy between images of different sizes is never recorded: without a pair that fits, the canvas keeps its picture.
    auto* bb_copy = &m_backbuffer_copy;
    auto* bb_8bit = &m_converted_eye_tex;
    const bool canvas_pair = vr->is_using_multipass() && m_canvas_copy.texture != nullptr && m_canvas_8bit.texture != nullptr;

    if (canvas_pair) {
        bb_copy = &m_canvas_copy;
        bb_8bit = &m_canvas_8bit;
    }

    // [CANVAS fix124 22.11.2026] The quad view's left eye image as the canvas's picture. With the flatscreen canvas on, the
    // eye pass puts the left eye together without the fovea (TemporalUpscaler::evaluate_quadview_eye) and, with the direct
    // 8-bit write, straight into the left headset texture; that texture is cleared to black further down (the eyes are
    // black in canvas mode), after the canvas has copied it. So the canvas shows the picture it showed up to fix122 (the
    // eye image: the wide view after the mod's anti-aliasing, at the headset's size) without the small copy of it where
    // the fovea was, and without the way through the window: no 10-bit eye image, no two copies of the whole window and
    // no conversion per frame, and the window's size no longer matters. Test of fix123 with the small window (the canvas
    // showed the window, 1241 x 961, the engine's view without anti-aliasing): the user found the cutscenes less sharp.
    ID3D12Resource* canvas_eye = nullptr;

    if (!m_backbuffer_is_8bit && vr->is_using_multipass() && vr->is_flatscreen_overlay() && vr->get_runtime() != nullptr &&
        vr->get_runtime()->is_openvr()) {
        auto* const left = m_openvr.get_left().texture.Get();

        if (left != nullptr && submit_eye_image(vr, 0) == left) {
            canvas_eye = left;
        }
    }

    bool bb_copy_fits = canvas_eye == nullptr && bb_copy->texture != nullptr && bb_8bit->texture != nullptr;

    if (bb_copy_fits) {
        const auto bd = backbuffer->GetDesc();
        const auto cd = bb_copy->texture->GetDesc();
        bb_copy_fits = bd.Width == cd.Width && bd.Height == cd.Height;
    }

    if (!m_backbuffer_is_8bit && bb_copy_fits && (!vr->is_using_multipass() || (multipass_eyes && vr->is_flatscreen_overlay()))) {
        auto& commands = m_backbuffer_copy_commands[backbuffer_index % m_backbuffer_copy_commands.size()];
        auto command_list = commands.cmd_list.Get();
        commands.wait(INFINITE);

        // Copy current backbuffer into our copy so we can use it as an SRV.
        commands.copy(backbuffer.Get(), bb_copy->texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);

        float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
        commands.clear_rtv(*bb_8bit, clear_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Convert the backbuffer to 8-bit.
        // [MENU HANG 14.11.2026] The converted texture rests in PIXEL_SHADER_RESOURCE (made in it, cleared from and back to it
        // just above). The draw's first barrier started from PRESENT, and the copies that read the texture afterwards (canvas,
        // eye fallbacks) named PRESENT as well and left it there: each frame three barriers named a state the texture was not
        // in. In multipass this path runs only for the flatscreen canvas, i.e. RE7's main menu.
        render_srv_to_rtv(command_list, *bb_copy, *bb_8bit, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        commands.execute();
    }

    auto eye_texture = m_backbuffer_is_8bit ? backbuffer : m_converted_eye_tex.texture;
    // [SMALL WINDOW fix122] what the flatscreen canvas shows: the window's own 8-bit image where the pair above is in use
    auto canvas_texture = canvas_pair && !m_backbuffer_is_8bit ? m_canvas_8bit.texture : eye_texture;

    // [CANVAS fix124] (the headset textures rest in PIXEL_SHADER_RESOURCE like the converted image: eye_texture_state)
    if (canvas_eye != nullptr) {
        canvas_texture = canvas_eye;
    }
    // [MENU HANG 14.11.2026] the state eye_texture rests in between uses: the backbuffer PRESENT, its 8-bit copy PIXEL_SHADER_RESOURCE
    const auto eye_texture_state = m_backbuffer_is_8bit ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // --- Ein Auge schwarz unter OpenXR -----------------------------------
    // Das gab es bisher nur im OpenVR-Zweig (clear_left/clear_right). Unter OpenXR
    // wurde `get_blank_eye()` gar nicht ausgewertet, das Auge blieb hell -- damit war
    // das Scope-Bild dort unbrauchbar, obwohl Mono und Zoom laengst runtime-unabhaengig
    // arbeiten. `copy()` nimmt ohnehin eine CopyFn (der Multipass-Zweig nutzt sie
    // bereits), also wird hier statt des Bildes einfach der Rendertarget geleert.
    // Rueckgabe nullptr = ganz normal kopieren, es aendert sich also nichts, solange
    // weder ein Auge geschwaerzt noch die Leinwand aktiv ist.
    auto xr_black = [](d3d12::CommandContext& cmds, d3d12::TextureContext& dst,
                       D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
        const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
        cmds.clear_rtv(dst, black, dst_state);
    };

    auto xr_fn = [&](uint32_t eye) -> OpenXR::CopyFn {
        if (vr->should_blank_all_eyes() || vr->get_blank_eye() == (int32_t)eye) {
            return xr_black;
        }

        return nullptr;
    };

    auto runtime = vr->get_runtime();

    // Flatscreen canvas. Purely additive: it only reads the finished frame and drives its
    // own overlay, so with the feature off (the default) nothing below changes at all.
    // [CANVAS_RECENTER] Recenter -> die Leinwand nimmt die Kopfpose beim naechsten Frame neu ab.
    if (vr->consume_flatscreen_reanchor()) {
        m_flatscreen_overlay.anchored = false;

        if (vr->m_openxr != nullptr) {
            vr->m_openxr->flatscreen_anchored = false;
        }
    }

    if (runtime->is_openvr()) {
        update_flatscreen_overlay(vr, canvas_texture.Get(), eye_texture_state, command_queue);
    } else if (runtime->is_openxr() && vr->m_openxr->ready()) {
        // Zustand und Masse an die Runtime durchreichen -- OpenXR.cpp haengt das Quad
        // daran auf und darf VR.hpp nicht einbinden (Zyklus).
        vr->m_openxr->flatscreen_layer = vr->is_flatscreen_overlay();
        vr->m_openxr->flatscreen_width = vr->get_flatscreen_overlay_width();
        vr->m_openxr->flatscreen_distance = vr->get_flatscreen_overlay_distance();
    }

    if (runtime->is_openxr() && vr->is_flatscreen_overlay() && vr->m_openxr->ready()) {
        // OpenXR kennt keine Overlays -- dort ist die Leinwand ein zweiter Compositor-
        // Layer (Quad). Hier wird nur der fertige Frame in dessen eigene Swapchain
        // kopiert; aufgehaengt wird das Quad in OpenXR::end_frame.
        const auto canvas_idx = (uint32_t)vr->m_openxr->views.size();

        if (this->m_openxr.contexts.size() > canvas_idx) {
            m_openxr.copy(canvas_idx, eye_texture.Get(), nullptr, eye_texture_state);
        }
    }

    // [XR_UI_OVERLAY 2026-08-14] Das Menue-Rendertarget in die Slate-Swapchain kopieren.
    // ui_layer setzt OverlayComponent, sobald das Menue offen ist -- ist es zu, wird hier
    // gar nichts getan, der Layer wird dann auch nicht angehaengt.
    if (runtime->is_openxr() && vr->m_openxr->ready() && vr->m_openxr->ui_layer) {
        const auto ui_idx = (uint32_t)vr->m_openxr->views.size() + 1;
        auto& ui_rt = g_framework->get_rendertarget_d3d12();

        if (this->m_openxr.contexts.size() > ui_idx && ui_rt.Get() != nullptr) {
            // Das Rendertarget ruht als PIXEL_SHADER_RESOURCE -- REFramework schaltet es
            // nur zum Zeichnen kurz auf RENDER_TARGET und danach wieder zurueck.
            m_openxr.copy(ui_idx, ui_rt.Get(), nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto frame_count = vr->m_render_frame_count;

    
    // [AFW fix91 19.11.2026] the frame warp of AFW (its plugin, on the eye not rendered) ran here; the
    // technique is gone

    const auto is_multipass = vr->is_using_multipass();
    auto eye_format = DXGI_FORMAT_UNKNOWN;
    if (is_multipass && (vr->m_multipass.eye_textures[0] != nullptr && vr->m_multipass.eye_textures[1] != nullptr)) {
        // [DIRECT 8 BIT 30.10.2026] an eye the quad view's eye pass wrote into its headset texture holds that texture, not an
        // eye image: the format of the eye images is the other eye's, or the backbuffer's (the direct write needs it 10-bit)
        eye_format = backbuffer->GetDesc().Format;

        for (int i = 0; i < 2; ++i) {
            if (auto* t = submit_eye_image(vr, i); t != nullptr && !is_direct_texture(i, t)) {
                eye_format = t->GetDesc().Format;
                break;
            }
        }
    } else {
        eye_format = backbuffer->GetDesc().Format;
    }

    if (runtime->is_openxr()) {
        if (eye_format != m_openxr.last_format) {
            REVR_LOG(spdlog::info("[VR] OpenXR format changed from {} to {}", m_openxr.last_format, eye_format));
            m_openxr.create_swapchains();
        }
    } else {
        if (eye_format != m_openvr.last_format) {
            REVR_LOG(spdlog::info("[VR] OpenVR format changed from {} to {}", m_openvr.last_format, eye_format));
            on_reset(vr);
            setup();
        }
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (frame_count % 2 == vr->m_left_eye_interval && !is_multipass) {
        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            // Quelle und Resource-State bleiben die des AFW-Forks; ergaenzt ist nur die
            // CopyFn, mit der ein Auge unter OpenXR schwarz bleibt (VR::get_blank_eye).
            //m_openxr.copy(0, m_openvr.get_left().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, xr_fn(0));
            m_openxr.copy(0, eye_texture.Get(), nullptr, eye_texture_state, xr_fn(0));
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture (m_left_eye_tex0 holds the intermediate frame).
        if (runtime->is_openvr()) {
            // get_blank_eye(): that eye gets black instead of the frame (scope aiming).
            // should_blank_all_eyes(): canvas mode - nothing but the quad may be visible.
            if (vr->get_blank_eye() == 0 || vr->should_blank_all_eyes()) {
                m_openvr.clear_left();
            } else {
                m_openvr.copy_left(eye_texture.Get(), eye_texture_state);
            }

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            // [POSE_FREEZE 2026-08-11] Im Scope ist das Bild bewusst kopfunabhaengig.
            // Ohne Angabe nimmt SteamVR seine eigene Pose an und reprojiziert dagegen.
            // Modus 1 gibt die eingefrorene Pose an (Compositor dreht die Differenz nach),
            // Modus 2 die frische (er dreht nichts nach), Modus 0 laesst alles wie frueher.
            vr::VRTextureWithPose_t left_eye{};
            left_eye.handle = (void*)&left;
            left_eye.eType = vr::TextureType_DirectX12;
            left_eye.eColorSpace = vr::ColorSpace_Auto;
            left_eye.mDeviceToAbsoluteTracking = vr->get_submit_pose();

            const auto submit_flags = (vr->is_pose_freeze() && vr->get_pose_freeze_submit() != 0)
                ? (vr::EVRSubmitFlags)(vr::Submit_Default | vr::Submit_TextureWithPose)
                : vr::Submit_Default;

            const auto left_bounds = vr->get_shifted_bounds(false);
            auto e = timed_submit(vr::Eye_Left, (vr::Texture_t*)&left_eye, &left_bounds, submit_flags);

            if (e != vr::VRCompositorError_None) {
                REVR_LOG(spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e));
                return e;
            }
        }
    } else {
        // [POST_PASS 28.09.2026] Vor den Kopien an OpenXR/OpenVR: Schaerfe und
        // Saettigung direkt in die Augenbilder (nur Multipass; SMAA nur ohne Upscaler).
        if (is_multipass) {
            apply_post_pass(vr, device, frame_count);
        }

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_multipass) {
                /*D3D12_BOX src_box{};
                src_box.back = 1;
                src_box.right = vr->get_hmd_width();
                src_box.bottom = vr->get_hmd_height();
                m_openxr.copy(0, (ID3D12Resource*)vr->m_multipass.eye_textures[0], &src_box, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                src_box.left = src_box.right;
                src_box.right *= 2;
                m_openxr.copy(1, (ID3D12Resource*)vr->m_multipass.eye_textures[0], &src_box, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);*/

                if (vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
                    auto& ctx0 = vr->m_multipass.eye_contexts[0];
                    auto& ctx1 = vr->m_multipass.eye_contexts[1];

                    bool views_ok = true;

                    if (ctx0.texture.Get() != vr->m_multipass.eye_textures[0].Get()) {
                        ctx0.reset();
                        const auto desc = vr->m_multipass.eye_textures[0]->GetDesc();
                        views_ok &= ctx0.setup(device, vr->m_multipass.eye_textures[0].Get(), desc.Format, desc.Format);
                    }

                    if (ctx1.texture.Get() != vr->m_multipass.eye_textures[1].Get()) {
                        ctx1.reset();
                        const auto desc = vr->m_multipass.eye_textures[1]->GetDesc();
                        views_ok &= ctx1.setup(device, vr->m_multipass.eye_textures[1].Get(), desc.Format, desc.Format);
                    }

                    // [27.10.2026] no views -> nothing drawn with them (null descriptor heaps crashed); retried next frame
                    if (!views_ok) {
                        REVR_LOG(spdlog::error("[VR] multipass (OpenXR): views of the eye textures could not be created, frame not submitted"));
                        ctx0.reset();
                        ctx1.reset();
                    } else if (m_backbuffer_is_8bit) {
                        if (!TemporalUpscaler::get()->ready()) {
                            m_openxr.copy(0, vr->m_multipass.eye_textures[0].Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, xr_fn(0));
                            m_openxr.copy(1, vr->m_multipass.eye_textures[1].Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, xr_fn(1));
                        } else {
                            m_openxr.copy(0, vr->m_multipass.eye_textures[0].Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, xr_fn(0));
                            m_openxr.copy(1, vr->m_multipass.eye_textures[1].Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, xr_fn(1));
                        }
                    } else {
                        auto copy0fn = [&](d3d12::CommandContext& ctx, d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
                            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
                            ctx.clear_rtv(dst, clear_color, dst_state);
                            render_srv_to_rtv(ctx.cmd_list.Get(), vr->m_multipass.eye_contexts[0], dst, src_state, dst_state);
                        };

                        auto copy1fn = [&](d3d12::CommandContext& ctx, d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
                            const float clear_color[4]{0.0f, 0.0f, 0.0f, 0.0f};
                            ctx.clear_rtv(dst, clear_color, dst_state);
                            render_srv_to_rtv(ctx.cmd_list.Get(), vr->m_multipass.eye_contexts[1], dst, src_state, dst_state);
                        };

                        if (!TemporalUpscaler::get()->ready()) {
                            m_openxr.copy(0, ctx0.texture.Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, xr_fn(0) ? xr_fn(0) : OpenXR::CopyFn{copy0fn});
                            m_openxr.copy(1, ctx1.texture.Get(), nullptr, D3D12_RESOURCE_STATE_COPY_DEST, xr_fn(1) ? xr_fn(1) : OpenXR::CopyFn{copy1fn});
                        } else {
                            m_openxr.copy(0, ctx0.texture.Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, xr_fn(0) ? xr_fn(0) : OpenXR::CopyFn{copy0fn});
                            m_openxr.copy(1, ctx1.texture.Get(), nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, xr_fn(1) ? xr_fn(1) : OpenXR::CopyFn{copy1fn});
                        }
                    }
                } else {
                    // just copy the backbuffer to both eyes as a fallback
                    m_openxr.copy(0, eye_texture.Get(), nullptr, eye_texture_state, xr_fn(0));
                    m_openxr.copy(1, eye_texture.Get(), nullptr, eye_texture_state, xr_fn(1));
                }

                vr->m_multipass.eye_textures[0].Reset();
                vr->m_multipass.eye_textures[1].Reset();
            } else {
                // wie oben: AFW-Quelle/State, plus die CopyFn fuer das geschwaerzte Auge
                //m_openxr.copy(1, m_openvr.get_right().texture.Get(), nullptr, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, xr_fn(1));
                m_openxr.copy(1, eye_texture.Get(), nullptr, eye_texture_state, xr_fn(1));
            }
        }

        // OpenVR texture
        // Copy the back buffer to the right eye texture.
        if (runtime->is_openvr()) {
            if (is_multipass) {
                if (vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
                    // [HDR10_OPENVR 16.10.2026] AMD-Tester: Regenbogenfarben. RE4 legt dort die Swapchain als
                    // R10G10B10A2 (Format 24) an, auch mit HDR aus (ResizeBuffers fordert selbst 24). Die OpenVR-
                    // Augen sind 8 Bit (rt_desc) -> copy_left/right = CopyResource = Bitmuster 10:10:10:2 als
                    // 8:8:8:8 gelesen. OpenXR-Zweig oben und Nicht-Multipass rechnen bei !m_backbuffer_is_8bit
                    // schon per Shader um (render_srv_to_rtv), nur dieser Zweig nicht. Gleiches hier; der 8-Bit-Weg
                    // darunter bleibt unveraendert.
                    if (!m_backbuffer_is_8bit) {
                        const auto src_state = TemporalUpscaler::get()->ready() ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;

                        // [DIRECT 8 BIT 30.10.2026] an eye whose image the quad view's eye pass wrote into its headset texture
                        // itself needs nothing here; the stamps of the conversion go around the eyes that are converted
                        // (this frame's eye images: m_multipass.eye_textures is taken at EndRendering, one evaluation earlier)
                        bool convert[2]{};
                        ID3D12Resource* eye_img[2]{};

                        for (int i = 0; i < 2; ++i) {
                            eye_img[i] = submit_eye_image(vr, i);
                            convert[i] = eye_img[i] != nullptr && eye_img[i] != (i == 0 ? m_openvr.get_left() : m_openvr.get_right()).texture.Get();
                        }

                        const int first_converted = convert[0] ? 0 : (convert[1] ? 1 : -1);
                        const int last_converted = convert[1] ? 1 : (convert[0] ? 0 : -1);

                        for (int i = 0; i < 2; ++i) {
                            if (!convert[i]) {
                                continue;
                            }

                            auto& src = vr->m_multipass.eye_contexts[i];
                            auto* src_tex = eye_img[i];
                            const auto src_desc = src_tex->GetDesc();

                            // an 8-bit eye image the eye pass wrote into a headset texture that has been replaced since (the
                            // textures are made again after a reset): copied as it is, it rests in PIXEL_SHADER_RESOURCE
                            if (is_direct_texture(i, src_tex)) {
                                auto& dst = i == 0 ? m_openvr.acquire_left() : m_openvr.acquire_right();
                                auto& spans = d3d12::GpuSpans::get();

                                if (i == first_converted) {
                                    spans.stamp(dst.commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::CONVERT_BEGIN);
                                }

                                if (dst.texture != nullptr && dst.texture->GetDesc().Format == src_desc.Format && dst.texture->GetDesc().Width == src_desc.Width &&
                                    dst.texture->GetDesc().Height == src_desc.Height) {
                                    dst.commands.copy(src_tex, dst.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                }

                                if (i == last_converted) {
                                    spans.stamp(dst.commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::CONVERT_END);
                                    spans.resolve(dst.commands.cmd_list.Get(), frame_count, d3d12::GpuSpans::CONVERT_BEGIN, 2);
                                }

                                dst.commands.has_commands = true;
                                dst.commands.execute();
                                continue;
                            }

                            if (src.texture.Get() != src_tex) {
                                src.reset();
                                const auto desc = src_tex->GetDesc();

                                // [27.10.2026] no views -> no draw (it crashed on null descriptor heaps); retried next frame
                                if (!src.setup(device, src_tex, desc.Format, desc.Format)) {
                                    REVR_LOG(spdlog::error("[VR] multipass: views of eye texture {} could not be created, eye not submitted this frame", i));
                                    src.reset();
                                    continue;
                                }
                            }

                            auto& dst = i == 0 ? m_openvr.acquire_left() : m_openvr.acquire_right();
                            auto* cmd = dst.commands.cmd_list.Get();

                            // [GPU BREAKDOWN] two stamps bracket the 10-bit -> 8-bit conversion of both eyes
                            auto& spans = d3d12::GpuSpans::get();

                            if (i == first_converted) {
                                spans.stamp(cmd, frame_count, d3d12::GpuSpans::CONVERT_BEGIN);
                            }

                            D3D12_RESOURCE_BARRIER b{};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b.Transition.pResource = src_tex;
                            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            b.Transition.StateBefore = src_state;
                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                            cmd->ResourceBarrier(1, &b);

                            // [GPU 29.10.2026] the draw is opaque and covers the whole texture (setup_sprite_batch_pso): no clear before it.
                            // The clear was what marked the list as holding commands: without it the list has to be marked here.
                            render_srv_to_rtv(cmd, src, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                            dst.commands.has_commands = true;

                            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                            cmd->ResourceBarrier(1, &b);

                            if (i == last_converted) {
                                spans.stamp(cmd, frame_count, d3d12::GpuSpans::CONVERT_END);
                                spans.resolve(cmd, frame_count, d3d12::GpuSpans::CONVERT_BEGIN, 2);
                            }

                            dst.commands.execute();
                        }
                    } else if (!TemporalUpscaler::get()->ready()) {
                        m_openvr.copy_left(vr->m_multipass.eye_textures[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                        m_openvr.copy_right(vr->m_multipass.eye_textures[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                    } else {
                        m_openvr.copy_left(vr->m_multipass.eye_textures[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        m_openvr.copy_right(vr->m_multipass.eye_textures[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    }
                } else {
                    // just copy the backbuffer to both eyes as a fallback
                    m_openvr.copy_left(eye_texture.Get(), eye_texture_state);
                    m_openvr.copy_right(eye_texture.Get(), eye_texture_state);
                }

                // get_blank_eye(): overwrite that eye with black again (scope aiming).
                // should_blank_all_eyes(): canvas mode - both eyes black.
                if (vr->should_blank_all_eyes()) {
                    m_openvr.clear_left();
                    m_openvr.clear_right();
                } else if (vr->get_blank_eye() == 0) {
                    m_openvr.clear_left();
                } else if (vr->get_blank_eye() == 1) {
                    m_openvr.clear_right();
                }

                // [STEREO FOVEA 27.10.2026] both eye textures now hold what goes to the headset in this frame
                // (they rest in PIXEL_SHADER_RESOURCE): the frame dump and the video take them from here
#if !REVR_RELEASE // [RELEASE SWITCH fix141] frame dumps and the video: debug build only
                dump_quadview_eyes(device); // [QUADVIEW] debug dump only, no-op otherwise
                dump_multipass_eyes(device); // [MULTIPASS DUMP 13.11.2026] debug dump only, no-op otherwise
                record_eyes(device);        // [VIDEO 25.10.2026] no-op unless a video was asked for
#endif
            } else {
                if (vr->get_blank_eye() == 1 || vr->should_blank_all_eyes()) {
                    m_openvr.clear_right();
                } else {
                    m_openvr.copy_right(eye_texture.Get(), eye_texture_state);
                }
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            // [POSE_FREEZE] siehe oben -- gilt fuer beide Augen und beide Renderwege.
            const auto submit_flags = (vr->is_pose_freeze() && vr->get_pose_freeze_submit() != 0)
                ? (vr::EVRSubmitFlags)(vr::Submit_Default | vr::Submit_TextureWithPose)
                : vr::Submit_Default;
            const auto submit_pose = vr->get_submit_pose();

            vr::VRTextureWithPose_t right_eye{};
            right_eye.handle = (void*)&right;
            right_eye.eType = vr::TextureType_DirectX12;
            right_eye.eColorSpace = vr::ColorSpace_Auto;
            right_eye.mDeviceToAbsoluteTracking = submit_pose;

            if (is_multipass) {
                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{};
                left_eye.handle = (void*)&left;
                left_eye.eType = vr::TextureType_DirectX12;
                left_eye.eColorSpace = vr::ColorSpace_Auto;
                left_eye.mDeviceToAbsoluteTracking = submit_pose;

                const auto left_bounds = vr->get_shifted_bounds(false);
            auto e = timed_submit(vr::Eye_Left, (vr::Texture_t*)&left_eye, &left_bounds, submit_flags);
                runtime->frame_synced = false;

                if (e != vr::VRCompositorError_None) {
                    REVR_LOG(spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e));
                    return e;
                }
            }

            const auto right_bounds = vr->get_shifted_bounds(true);
            auto e = timed_submit(vr::Eye_Right, (vr::Texture_t*)&right_eye, &right_bounds, submit_flags);

            if (e != vr::VRCompositorError_None) {
                REVR_LOG(spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e));
                return e;
            } else {
                vr->m_submitted = true;
            }
            ++m_openvr.texture_counter;
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (frame_count % 2 == vr->m_right_eye_interval || is_multipass) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->ready() && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE) {
            runtime->synchronize_frame();

            if (!runtime->got_first_poses) {
                runtime->update_poses();
            }
        }

        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            auto result = vr->m_openxr->end_frame();

            if (result == XR_ERROR_LAYER_INVALID) {
                REVR_LOG(spdlog::info("[VR] Attempting to correct invalid layer"));

                m_openxr.wait_for_all_copies();

                REVR_LOG(spdlog::info("[VR] Calling xrEndFrame again"));
                result = vr->m_openxr->end_frame();
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                // [fix115 21.11.2026] counted for the periodic line instead of a log line per present
                quadview::State::get().pose_in_present.fetch_add(1, std::memory_order_relaxed);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        // [DESKTOP COPY fix116 21.11.2026] Not in multipass. It is for the techniques that draw one eye per frame (the
        // desktop window then shows the same eye in both); in multipass every frame holds the whole picture, and this
        // still copied the last frame's backbuffer over this one in every second frame: with the backbuffer at the eye's
        // size (3400 x 3468, R10G10B10A2) 47 MB read and 47 MB written on the GPU in every second frame, and the desktop
        // window at half the frame rate. The headset's image never came from it.
        if (vr->m_desktop_fix->value() && !is_multipass && (frame_count % 2 == vr->m_right_eye_interval)) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                auto& copier = m_generic_copiers[frame_count % m_generic_copiers.size()];
                copier.wait(INFINITE);
                copier.copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                copier.execute();
            }
        }
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::on_post_present(VR* vr) {
}

void D3D12Component::on_reset(VR* vr) {
    REF_PROFILE_FUNCTION();

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    m_direct_given = {}; // [DIRECT 8 BIT 30.10.2026]
    m_direct_prev = {};

    for (auto& copier : m_generic_copiers) {
        copier.reset();
    }

    // [POST_PASS 28.09.2026]
    for (auto& commands : m_post_commands) {
        commands.reset();
    }

    m_post_pass.reset();
    m_smaa_pass.reset();
    m_ui_restore.reset();   // [UI_TRENNUNG 28.09.2026]

    for (auto& commands : m_backbuffer_copy_commands) {
        commands.reset();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix141]
    m_recorder.reset();     // [VIDEO 25.10.2026] its own command lists, waited for inside
#endif
    
    m_prev_backbuffer.Reset();
    m_backbuffer_copy.reset();
    m_converted_eye_tex.reset();
    m_canvas_copy.reset(); // [SMALL WINDOW fix122]
    m_canvas_8bit.reset();

    // The canvas holds a device resource too, so it has to go with the rest.
    reset_flatscreen_overlay();

    if (runtime->is_openxr() && runtime->loaded) {
        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            m_openxr.create_swapchains();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_openvr.texture_counter = 0;
}

void D3D12Component::reset_flatscreen_overlay() {
    auto& ov = m_flatscreen_overlay;

    if (ov.handle != vr::k_ulOverlayHandleInvalid && vr::VROverlay() != nullptr) {
        vr::VROverlay()->DestroyOverlay(ov.handle);
    }

    ov.handle = vr::k_ulOverlayHandleInvalid;
    ov.tex.reset();
    ov.size[0] = 0;
    ov.size[1] = 0;
    ov.format = DXGI_FORMAT_UNKNOWN;
    ov.shown = false;
    ov.failed = false;
    ov.texel_aspect = 1.0f;
}

void D3D12Component::update_flatscreen_overlay(VR* vr, ID3D12Resource* frame, D3D12_RESOURCE_STATES frame_state, ID3D12CommandQueue* queue) {
    auto& ov = m_flatscreen_overlay;

    // Switched off: hide once, then stay out of the way entirely.
    if (!vr->is_flatscreen_overlay()) {
        if (ov.shown && ov.handle != vr::k_ulOverlayHandleInvalid && vr::VROverlay() != nullptr) {
            vr::VROverlay()->HideOverlay(ov.handle);
            ov.shown = false;
        }

        ov.anchored = false;   // [CANVAS_RAUM] naechstes Einschalten nimmt die Kopfpose neu ab
        return;
    }

    // ov.failed: something went wrong once - never retry every frame, that would flood the log.
    if (ov.failed || frame == nullptr || queue == nullptr || vr::VROverlay() == nullptr) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();

    if (device == nullptr) {
        return;
    }

    const auto desc = frame->GetDesc();

    // Our own copy of the frame, recreated whenever resolution or format changes.
    if (ov.tex.texture == nullptr || ov.size[0] != (uint32_t)desc.Width || ov.size[1] != (uint32_t)desc.Height ||
        ov.format != desc.Format) {
        ov.tex.reset();

        auto tex_desc = desc;
        tex_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        tex_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        ComPtr<ID3D12Resource> tex{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &tex_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(tex.GetAddressOf())))) {
            REVR_LOG(spdlog::error("[VR] Flatscreen canvas: failed to create texture."));
            ov.failed = true;
            return;
        }

        tex->SetName(L"Flatscreen Canvas Texture");

        if (!ov.tex.setup(device, tex.Get(), std::nullopt, std::nullopt, L"Flatscreen canvas copy")) {
            REVR_LOG(spdlog::error("[VR] Flatscreen canvas: failed to set up texture context."));
            ov.failed = true;
            return;
        }

        ov.size[0] = (uint32_t)desc.Width;
        ov.size[1] = (uint32_t)desc.Height;
        ov.format = desc.Format;

        REVR_LOG(spdlog::info("[VR] Flatscreen canvas: texture {}x{} format {}", ov.size[0], ov.size[1], (int)ov.format));
    }

    if (ov.handle == vr::k_ulOverlayHandleInvalid) {
        const auto err = vr::VROverlay()->CreateOverlay("REFrameworkFlatscreen", "REFramework Flatscreen", &ov.handle);

        if (err != vr::VROverlayError_None) {
            REVR_LOG(spdlog::error("[VR] Flatscreen canvas: failed to create overlay: {}", (int)err));
            ov.handle = vr::k_ulOverlayHandleInvalid;
            ov.failed = true;
            return;
        }

        // It is a picture, not a panel - no input, no interaction.
        vr::VROverlay()->SetOverlayInputMethod(ov.handle, vr::VROverlayInputMethod_None);
        vr::VROverlay()->SetOverlayFlag(ov.handle, vr::VROverlayFlags::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

        REVR_LOG(spdlog::info("[VR] Flatscreen canvas: created overlay with handle {}", ov.handle));
    }

    // Sits in front of the head. Recomputed every frame so the sliders act live.
    vr::HmdMatrix34_t transform{};
    transform.m[0][0] = 1.0f;
    transform.m[1][1] = 1.0f;
    transform.m[2][2] = 1.0f;
    transform.m[2][3] = -vr->get_flatscreen_overlay_distance();

    vr::VROverlay()->SetOverlayWidthInMeters(ov.handle, vr->get_flatscreen_overlay_width());

    // [CANVAS ASPECT 12.11.2026] the picture's own shape: texels (aspect * height / width) wide; the width stays, the height
    // follows (main menu of RE7 of 12.11.2026: the picture looked stretched upwards on the square texture)
    {
        const float aspect = vr->get_flatscreen_overlay_aspect();
        float texel = 1.0f;

        if (aspect > 0.0f && ov.size[0] > 0 && ov.size[1] > 0) {
            texel = std::clamp(aspect * (float)ov.size[1] / (float)ov.size[0], 0.2f, 5.0f);
        }

        if (std::abs(texel - ov.texel_aspect) > 0.001f) {
            const auto err = vr::VROverlay()->SetOverlayTexelAspect(ov.handle, texel);
            REVR_LOG(spdlog::info("[VR] Flatscreen canvas: picture {:.3f} wide per unit of height, texel aspect {:.3f} (result {})", aspect,
                texel, (int)err));
            ov.texel_aspect = texel;
        }
    }

    // [CANVAS_RAUM 26.09.2026 -- Ansage des Users] Die Leinwand steht im RAUM, nicht kopffest:
    // Kopfpose beim Einschalten EINMAL abnehmen (nur Yaw), dann im Tracking-Raum des Compositors
    // stehen lassen. Solange keine gueltige Pose da ist, bleibt es beim kopffesten Weg.
    bool placed = false;

    if (vr::VRSystem() != nullptr && vr::VRCompositor() != nullptr) {
        const auto universe = vr::VRCompositor()->GetTrackingSpace();

        if (!ov.anchored) {
            vr::TrackedDevicePose_t hmd_pose{};
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(universe, 0.0f, &hmd_pose, 1);

            if (hmd_pose.bPoseIsValid) {
                const auto& m = hmd_pose.mDeviceToAbsoluteTracking.m;
                ov.anchor_pos[0] = m[0][3];
                ov.anchor_pos[1] = m[1][3];
                ov.anchor_pos[2] = m[2][3];

                // Blickrichtung = -Z der HMD-Matrix, auf die Horizontale projiziert.
                float fx = -m[0][2];
                float fz = -m[2][2];
                const float len = std::sqrt(fx * fx + fz * fz);

                if (len > 1e-4f) {
                    fx /= len;
                    fz /= len;
                } else {
                    fx = 0.0f;
                    fz = -1.0f;
                }

                ov.anchor_fwd_xz[0] = fx;
                ov.anchor_fwd_xz[1] = fz;
                ov.anchored = true;
            }
        }

        if (ov.anchored) {
            const float fx = ov.anchor_fwd_xz[0];
            const float fz = ov.anchor_fwd_xz[1];
            const float d = vr->get_flatscreen_overlay_distance();

            // Spalten: X = rechts (-fz, 0, fx), Y = oben, Z = zum Betrachter (-fx, 0, -fz).
            vr::HmdMatrix34_t world{};
            world.m[0][0] = -fz;  world.m[0][1] = 0.0f; world.m[0][2] = -fx;
            world.m[1][0] = 0.0f; world.m[1][1] = 1.0f; world.m[1][2] = 0.0f;
            world.m[2][0] = fx;   world.m[2][1] = 0.0f; world.m[2][2] = -fz;
            world.m[0][3] = ov.anchor_pos[0] + fx * d;
            world.m[1][3] = ov.anchor_pos[1];
            world.m[2][3] = ov.anchor_pos[2] + fz * d;

            vr::VROverlay()->SetOverlayTransformAbsolute(ov.handle, universe, &world);
            placed = true;
        }
    }

    if (!placed) {
        vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(ov.handle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
    }

    // Same copy pattern the eye textures use.
    ov.tex.commands.wait(INFINITE);
    ov.tex.commands.copy(frame, ov.tex.texture.Get(), frame_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); // [MENU HANG 14.11.2026] the frame's own state
    ov.tex.commands.execute();

    vr::D3D12TextureData_t texture_data{ov.tex.texture.Get(), queue, 0};
    vr::Texture_t overlay_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};

    vr::VROverlay()->SetOverlayTexture(ov.handle, &overlay_tex);

    if (!ov.shown) {
        vr::VROverlay()->ShowOverlay(ov.handle);
        ov.shown = true;
    }
}

void D3D12Component::setup() {
    REF_PROFILE_FUNCTION();

    if (VR::get()->is_hmd_active()) {
        REVR_LOG(spdlog::info("[VR] Setting up d3d12 textures..."));
    }
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};

    const auto& vr = VR::get();
    const auto is_multipass = vr->is_using_multipass();
    
    bool eyes_direct = false; // [DIRECT 8 BIT 30.10.2026] both eye images are the headset textures (the backbuffer's format)

    if (is_multipass && vr->m_multipass.eye_textures[0].Get() != nullptr && vr->m_multipass.eye_textures[1].Get() != nullptr) {
        for (int i = 0; i < 2 && backbuffer == nullptr; ++i) {
            if (auto* t = submit_eye_image(vr.get(), i); t != nullptr && !is_direct_texture(i, t)) {
                backbuffer = t;
            }
        }

        eyes_direct = backbuffer == nullptr;
    } else if (is_multipass) {
        REVR_LOG(spdlog::warn("[VR] Multipass textures are not setup correctly."));
    }

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&real_backbuffer)))) {
        REVR_LOG(spdlog::error("[VR] Failed to get back buffer."));
        return;
    }

    if (backbuffer == nullptr) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        REVR_LOG(spdlog::error("[VR] Failed to get back buffer."));
        return;
    }

    auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    if (is_multipass) {
        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (eyes_direct) {
            REVR_LOG(spdlog::info("[VR] Multipass eye images are the headset textures (direct 8 bit): the backbuffer's format is taken."));
        } else if (backbuffer.Get() == real_backbuffer.Get()) {
            //backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            REVR_LOG(spdlog::warn("[VR] Multipass textures are not setup correctly: Re-using backbuffer."));
        } else {
            REVR_LOG(spdlog::info("[VR] Multipass textures are setup correctly."));
        }
    } else {
        backbuffer_desc.Width = real_backbuffer_desc.Width;
        backbuffer_desc.Height = real_backbuffer_desc.Height;
    }

    m_openvr.last_format = backbuffer_desc.Format;

    REVR_LOG(spdlog::info("[VR] D3D12 Backbuffer width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, backbuffer_desc.Format));
    REVR_LOG(spdlog::info("[VR] D3D12 Real Backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, real_backbuffer_desc.Format));

    m_backbuffer_is_8bit = backbuffer_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || backbuffer_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

    auto backbuffer_srv_desc = backbuffer_desc;
    backbuffer_srv_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_srv_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    // Create copy of backbuffer to use as SRV to convert from HDR to 8bit
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> backbuffer_copy{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_srv_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(backbuffer_copy.GetAddressOf())))) {
            REVR_LOG(spdlog::error("[VR] Failed to create backbuffer copy."));
            return;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy.Get(), std::nullopt, std::nullopt)) {
            REVR_LOG(spdlog::error("[VR] Error setting up backbuffer copy texture RTV/SRV."));
        }
    }

    auto rt_desc = backbuffer_desc;

    // [AFW-COLOR 26.09.2026] Fallback war B8G8R8A8_UNORM. Der greift bei jedem
    // Nicht-8-Bit-Backbuffer (RE4: R10G10B10A2), waehrend die OpenXR-Swapchain in
    // create_swapchains() dann auf R8G8B8A8_UNORM_SRGB faellt. Die Augen werden
    // per Byte-Kopie (CopyResource) hineingeschoben -> Rot/Blau vertauscht in AFW
    // (VDXR-User: blaues Bild). Jetzt dieselbe Familie wie die Swapchain.
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        
        default:
            REVR_LOG(spdlog::error("[OpenVR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format));
            break;
    };

    // [AFW fix91 19.11.2026] the AFW plugin made the eye frame buffers here and the headset textures below were
    // its textures whenever it gave any: they are always the mod's own now (same size, same format, with unordered access
    // for the direct 8-bit write)

    // Create converted eye texture
    if (!m_backbuffer_is_8bit) {
        ComPtr<ID3D12Resource> eye_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(eye_tex.GetAddressOf())))) {
            REVR_LOG(spdlog::error("[VR] Failed to create converted eye texture."));
            return;
        }

        if (!m_converted_eye_tex.setup(device, eye_tex.Get(), std::nullopt, std::nullopt)) {
            REVR_LOG(spdlog::error("[VR] Error setting up converted eye texture RTV/SRV."));
        }
    }

    // [SMALL WINDOW fix122 22.11.2026] the game's window (the real backbuffer) is not the eye image's size: the flatscreen
    // canvas gets a copy and an 8-bit image of the window's size (see on_frame). Multipass only.
    m_canvas_copy.reset();
    m_canvas_8bit.reset();

    if (!m_backbuffer_is_8bit && is_multipass &&
        (real_backbuffer_desc.Width != backbuffer_desc.Width || real_backbuffer_desc.Height != backbuffer_desc.Height)) {
        auto copy_desc = backbuffer_srv_desc;
        copy_desc.Width = real_backbuffer_desc.Width;
        copy_desc.Height = real_backbuffer_desc.Height;
        copy_desc.Format = real_backbuffer_desc.Format;
        auto small_desc = rt_desc;
        small_desc.Width = real_backbuffer_desc.Width;
        small_desc.Height = real_backbuffer_desc.Height;
        ComPtr<ID3D12Resource> copy_tex{};
        ComPtr<ID3D12Resource> small_tex{};

        if (SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                IID_PPV_ARGS(copy_tex.GetAddressOf()))) &&
            SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &small_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(small_tex.GetAddressOf()))) &&
            m_canvas_copy.setup(device, copy_tex.Get(), std::nullopt, std::nullopt) && m_canvas_8bit.setup(device, small_tex.Get(), std::nullopt, std::nullopt)) {
            REVR_LOG(spdlog::info("[VR] Flatscreen canvas: the game's window is {}x{}, the eye image {}x{}: the canvas shows the window", real_backbuffer_desc.Width,
                real_backbuffer_desc.Height, backbuffer_desc.Width, backbuffer_desc.Height));
        } else {
            REVR_LOG(spdlog::error("[VR] Flatscreen canvas: the textures of the window's size could not be made; the canvas keeps its last picture"));
            m_canvas_copy.reset();
            m_canvas_8bit.reset();
        }
    }

    // [DIRECT 8 BIT 30.10.2026] the eye textures also with unordered access: the quad view's eye pass can write them itself
    // (direct_eye_target). Without it (creation refused) as before.
    auto eye_desc = rt_desc;

    if (eye_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && eye_desc.SampleDesc.Count == 1) {
        eye_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    auto create_eye_texture = [&](ComPtr<ID3D12Resource>& out) {
        if (SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &eye_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(out.ReleaseAndGetAddressOf())))) {
            return true;
        }

        return SUCCEEDED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(out.ReleaseAndGetAddressOf())));
    };

    for (auto& ctx : m_openvr.left_eye_tex) {
        ComPtr<ID3D12Resource> left_eye_tex{};
        if (!create_eye_texture(left_eye_tex)) {
            REVR_LOG(spdlog::error("[VR] Failed to create left eye texture."));
            return;
        }

        left_eye_tex->SetName(L"OpenVR Left Eye Texture");
        if (!ctx.setup(device, left_eye_tex.Get(), std::nullopt, std::nullopt, L"OpenVR left eye")) {
            REVR_LOG(spdlog::error("[VR] Error setting up left eye texture RTV/SRV."));
        }
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ComPtr<ID3D12Resource> right_eye_tex{};
        if (!create_eye_texture(right_eye_tex)) {
            REVR_LOG(spdlog::error("[VR] Failed to create right eye texture."));
            return;
        }

        right_eye_tex->SetName(L"OpenVR Right Eye Texture");
        if (!ctx.setup(device, right_eye_tex.Get(), std::nullopt, std::nullopt, L"OpenVR right eye")) {
            REVR_LOG(spdlog::error("[VR] Error setting up right eye texture RTV/SRV."));
        }
    }

    for (auto& copier : m_generic_copiers) {
        copier.setup(L"Generic Copier");
    }

    // [POST_PASS 28.09.2026]
    for (auto& commands : m_post_commands) {
        commands.setup(L"Post Pass Commands");
    }
    
    for (auto& commands : m_backbuffer_copy_commands) {
        commands.setup(L"Backbuffer Copy Commands");
    }

    setup_sprite_batch_pso(rt_desc.Format);

    m_backbuffer_size[0] = real_backbuffer_desc.Width;
    m_backbuffer_size[1] = real_backbuffer_desc.Height;

    REVR_LOG(spdlog::info("[VR] d3d12 textures have been setup"));
    m_force_reset = false;
}

// [DIRECT 8 BIT 30.10.2026] the headset texture of an eye when the quad view's eye pass may write it itself: OpenVR, the
// 10-bit images converted to 8 bits here (the eye textures are ours, R8G8B8A8_UNORM with unordered access), no reset
// pending. It rests in PIXEL_SHADER_RESOURCE; the conversion leaves an eye out whose eye image is this texture.
ID3D12Resource* D3D12Component::direct_eye_target(VR* vr, int eye) {
    if (vr == nullptr || eye < 0 || eye > 1 || m_force_reset || m_backbuffer_is_8bit || vr->get_runtime() == nullptr || !vr->get_runtime()->is_openvr()) {
        return nullptr;
    }

    auto* tex = (eye == 0 ? m_openvr.get_left() : m_openvr.get_right()).texture.Get();

    if (tex == nullptr) {
        return nullptr;
    }

    const auto desc = tex->GetDesc();

    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return nullptr;
    }

    if (m_direct_given[eye].Get() != tex) {
        m_direct_prev[eye] = m_direct_given[eye];
        m_direct_given[eye] = tex;
    }

    return tex;
}

// [DIRECT 8 BIT 30.10.2026] the eye image of this frame for the submit: with the quad view the upscaler's, written on this
// thread just before (on_early_present). m_multipass.eye_textures is taken on the game thread at EndRendering, before that
// evaluation: behind on a frame where an eye changes between its 10-bit eye image and its headset texture.
ID3D12Resource* D3D12Component::submit_eye_image(VR* vr, int eye) const {
    if (vr == nullptr || eye < 0 || eye > 1) {
        return nullptr;
    }

    const auto& up = TemporalUpscaler::get();

    if (up != nullptr && up->activated()) {
        if (auto* now = up->qv_eye_image_now(eye); now != nullptr) {
            return now;
        }
    }

    return vr->m_multipass.eye_textures[eye].Get();
}

bool D3D12Component::is_direct_texture(int eye, const ID3D12Resource* tex) const {
    if (tex == nullptr || eye < 0 || eye > 1) {
        return false;
    }

    return tex == (eye == 0 ? m_openvr.left_eye_tex[0] : m_openvr.right_eye_tex[0]).texture.Get() || tex == m_direct_given[eye].Get() ||
           tex == m_direct_prev[eye].Get();
}

void D3D12Component::setup_sprite_batch_pso(DXGI_FORMAT output_format) {
    REVR_LOG(spdlog::info("[D3D12] Setting up sprite batch PSO"));

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    DirectX::RenderTargetState output_state{output_format, DXGI_FORMAT_UNKNOWN};
    // [GPU 29.10.2026] opaque: every draw with it copies a whole texture into a whole render target that was cleared to zero
    // before (premultiplied blending onto zero gave the source unchanged); without blending the target is not read
    D3D12_BLEND_DESC opaque{};
    opaque.RenderTarget[0].BlendEnable = FALSE;
    opaque.RenderTarget[0].LogicOpEnable = FALSE;
    opaque.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    opaque.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    opaque.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    opaque.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    opaque.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    opaque.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    opaque.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    DirectX::SpriteBatchPipelineStateDescription pd{output_state, &opaque};

    m_sprite_batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, pd);

    auto result = upload.End(command_queue);
    result.wait();

    REVR_LOG(spdlog::info("[D3D12] Sprite batch PSO setup complete"));
}

void D3D12Component::render_srv_to_rtv(ID3D12GraphicsCommandList* command_list, const d3d12::TextureContext& src, const d3d12::TextureContext& dst, D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state) {
    if (m_sprite_batch == nullptr) {
        return;
    }
    
    d3d12::render_srv_to_rtv(m_sprite_batch.get(), command_list, src, dst, src_state, dst_state);
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    REF_PROFILE_FUNCTION();

    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    REVR_LOG(spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR..."));
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    REVR_LOG(spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR"));
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    REVR_LOG(spdlog::info("[VR] Creating OpenXR swapchains for D3D12"));

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    const auto& vr = VR::get();
    const auto is_multipass = vr->is_using_multipass();

    // Get the existing backbuffer
    // so we can get the format and stuff.
    bool has_multipass_buffer = false;
    if (is_multipass && vr->m_multipass.eye_textures[0].Get() != nullptr) {
        backbuffer = vr->m_multipass.eye_textures[0];
        has_multipass_buffer = true;
    }

    if (backbuffer.Get() == nullptr && FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        REVR_LOG(spdlog::error("[VR] Failed to get back buffer."));
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();
    // +2: die vorletzte Swapchain ist die Flatscreen-Leinwand, die letzte das ImGui-Slate
    // (beides Quad-Layer). Sie werden nur befuellt, wenn das jeweilige Feature laeuft --
    // angelegt werden sie aber immer, damit beim Einschalten nicht mitten im Frame
    // Ressourcen entstehen muessen.
    this->contexts.resize(openxr->views.size() + 2);

    this->last_format = backbuffer_desc.Format;

    DXGI_FORMAT swapchain_format{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};

    switch (backbuffer_desc.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;

        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            swapchain_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            break;
        
        default:
            REVR_LOG(spdlog::error("[VR] Possibly unsupported backbuffer format: {}", backbuffer_desc.Format));
            break;
    };
    
    for (auto i = 0; i < openxr->views.size(); ++i) {
        REVR_LOG(spdlog::info("[VR] Creating swapchain for eye {}", i));
        REVR_LOG(spdlog::info("[VR] Width: {}", vr->get_hmd_width()));
        REVR_LOG(spdlog::info("[VR] Height: {}", vr->get_hmd_height()));

        backbuffer_desc.Width = vr->get_hmd_width();
        backbuffer_desc.Height = vr->get_hmd_height();

        if (swapchain_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else {
            backbuffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        REVR_LOG(spdlog::info("[VR] Format: {}", backbuffer_desc.Format));

        // Create the swapchain.
        XrSwapchainCreateInfo swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchain_create_info.arraySize = 1;
        swapchain_create_info.format = swapchain_format;
        swapchain_create_info.width = backbuffer_desc.Width;
        swapchain_create_info.height = backbuffer_desc.Height;
        swapchain_create_info.mipCount = 1;
        swapchain_create_info.faceCount = 1;
        swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
        swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            REVR_LOG(spdlog::error("[VR] D3D12: Failed to create swapchain."));
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains.push_back(swapchain);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            REVR_LOG(spdlog::error("[VR] Failed to enumerate swapchain images."));
            return "Failed to enumerate swapchain images.";
        }

        REVR_LOG(spdlog::info("[VR] Runtime wants {} images for swapchain {}", image_count, i));

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);

        if (result != XR_SUCCESS) {
            REVR_LOG(spdlog::error("[VR] Failed to enumerate swapchain images after texture creation."));
            return "Failed to enumerate swapchain images after texture creation.";
        }

        // [XR_EYE_RTV 2026-08-15] Hier stand vorher nur `commands.setup(...)` -- die
        // Augen-Kontexte hatten damit KEIN RTV/SRV. Ohne RTV steigt CommandContext::clear_rtv
        // STILL aus (kein Log, kein Fehler): das Auge wurde nicht schwarz geraeumt und zeigte
        // den vorigen Frame weiter -- das ist das zitternde/nachziehende linke Auge unter
        // OpenXR und das "eingefrorene statt schwarze" Auge vom 11.08. Der Upscaler-Fork hat
        // genau diesen Block seither korrekt; hier war er beim AFW-Port verlorengegangen.
        // Warum acquire/wait vor dem setup: eine Swapchain-Textur darf erst benutzt werden,
        // wenn die Runtime sie uns gegeben hat, und `real_index` ist der Index, den SIE
        // vergibt -- nicht zwingend `j`. Das anschliessende Release gibt sie sofort zurueck,
        // die Deskriptoren bleiben gueltig. TextureContext::setup ruft commands.setup selbst,
        // die alte Zeile ist damit ersetzt und nicht verloren.
        for (uint32_t j = 0; j < image_count; ++j) {
            uint32_t real_index{};
            XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &real_index);
            if (result != XR_SUCCESS) {
                REVR_LOG(spdlog::error("[VR] Failed to acquire swapchain image."));
                return "Failed to acquire swapchain image.";
            }

            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

            if (result != XR_SUCCESS) {
                REVR_LOG(spdlog::error("[VR] Failed to wait for swapchain image."));
                return "Failed to wait for swapchain image.";
            }

            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->setup(device, ctx.textures[j].texture, swapchain_format, swapchain_format,
                (std::wstring{L"OpenXR Swapchain "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            if (result != XR_SUCCESS) {
                REVR_LOG(spdlog::error("[VR] Failed to release swapchain image."));
                return "Failed to release swapchain image.";
            }
        }
    }

    // ---------------------------------------------------------------------
    // Flatscreen-Leinwand: eigene Swapchain in BACKBUFFER-Aufloesung
    // ---------------------------------------------------------------------
    // Unter OpenVR haengt die Leinwand an einem Overlay (vr::VROverlay). Ein Gegenstueck
    // dazu gibt es in OpenXR nicht -- dort ist es ein zweiter Compositor-Layer, und der
    // braucht zwingend eine eigene Swapchain. Aufloesung ist die des fertigen Bildes,
    // NICHT die des HMD: wir zeigen ja den Monitor-Frame.
    {
        ComPtr<ID3D12Resource> bb{};

        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            const auto bb_desc = bb->GetDesc();

            // [CANVAS_FARBEN 26.09.2026] Wie [XR_UI_FARBEN] beim Slate: die Leinwand bekommt
            // die Kanalreihenfolge ihrer QUELLE. Die Quelle ist der ECHTE Swapchain-Backbuffer
            // (Log: "Real Backbuffer ... format: 28" = R8G8B8A8), swapchain_format folgt aber
            // im Multipass den Augen-Texturen ("format: 87" = B8G8R8A8) -> R und B vertauscht,
            // die Cutscene im Headset blau. _SRGB bleibt, nur die Kanalreihenfolge folgt bb.
            DXGI_FORMAT canvas_format = swapchain_format;

            switch (bb_desc.Format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    canvas_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                    break;
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                    canvas_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    break;
                default:
                    break;
            }

            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.arraySize = 1;
            ci.format = canvas_format;
            ci.width = (uint32_t)bb_desc.Width;
            ci.height = (uint32_t)bb_desc.Height;
            ci.mipCount = 1;
            ci.faceCount = 1;
            ci.sampleCount = 1;
            ci.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

            runtimes::OpenXR::Swapchain sc{};
            sc.width = ci.width;
            sc.height = ci.height;

            auto canvas_res = xrCreateSwapchain(openxr->session, &ci, &sc.handle);

            // [CANVAS_FARBEN] Rueckfall: lehnt die Laufzeit das Format ab, exakt wie bisher.
            if (canvas_res != XR_SUCCESS && canvas_format != swapchain_format) {
                REVR_LOG(spdlog::warn("[VR] Flatscreen: format {} refused by runtime, falling back to {}.",
                             (uint32_t)canvas_format, (uint32_t)swapchain_format));
                canvas_format = swapchain_format;
                ci.format = swapchain_format;
                canvas_res = xrCreateSwapchain(openxr->session, &ci, &sc.handle);
            }

            if (canvas_res == XR_SUCCESS) {
                const auto canvas_idx = (uint32_t)openxr->views.size();
                openxr->swapchains.push_back(sc);

                uint32_t image_count{};

                if (xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr) == XR_SUCCESS && image_count > 0) {
                    auto& cctx = this->contexts[canvas_idx];
                    cctx.textures.clear();
                    cctx.textures.resize(image_count);
                    cctx.texture_contexts.clear();
                    cctx.texture_contexts.resize(image_count);

                    for (uint32_t j = 0; j < image_count; ++j) {
                        cctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
                    }

                    if (xrEnumerateSwapchainImages(sc.handle, image_count, &image_count,
                            (XrSwapchainImageBaseHeader*)&cctx.textures[0]) == XR_SUCCESS) {
                        for (uint32_t j = 0; j < image_count; ++j) {
                            uint32_t real_index{};
                            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

                            if (xrAcquireSwapchainImage(sc.handle, &ai, &real_index) != XR_SUCCESS) {
                                break;
                            }

                            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                            xrWaitSwapchainImage(sc.handle, &wi);

                            cctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
                            cctx.texture_contexts[j]->setup(device, cctx.textures[j].texture,
                                canvas_format, canvas_format,
                                (std::wstring{L"OpenXR Flatscreen "} + std::to_wstring(j)).c_str());

                            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            xrReleaseSwapchainImage(sc.handle, &ri);
                        }

                        REVR_LOG(spdlog::info("[VR] OpenXR flatscreen swapchain: {}x{} ({} images, format {}, source {})",
                                     sc.width, sc.height, image_count, (uint32_t)canvas_format, (uint32_t)bb_desc.Format));
                    }
                }
            } else {
                REVR_LOG(spdlog::error("[VR] Failed to create OpenXR flatscreen swapchain."));
            }
        }
    }

    // ---------------------------------------------------------------------
    // ImGui-Slate: eigene Swapchain in der Groesse des Menue-Rendertargets
    // ---------------------------------------------------------------------
    // [XR_UI_OVERLAY 2026-08-14] Aufbau bewusst wie der Leinwand-Block darueber und
    // NICHT zusammengefasst: der Leinwand-Weg laeuft und soll dafuer nicht angefasst
    // werden. Das Rendertarget ist so gross wie der Backbuffer, deshalb reicht dessen
    // Beschreibung; der sichtbare Ausschnitt wird spaeter ueber subImage.imageRect
    // auf das Menuefenster begrenzt.
    {
        ComPtr<ID3D12Resource> bb{};

        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            const auto bb_desc = bb->GetDesc();

            // [XR_UI_FARBEN 2026-09-09] Das Slate bekommt NICHT swapchain_format,
            // sondern die Kanalreihenfolge seiner QUELLE.
            //
            // Die Quelle ist REFrameworks ImGui-Rendertarget, und das ist hart
            // DXGI_FORMAT_R8G8B8A8_UNORM (REFramework.cpp, Kommentar dort: "For
            // VR"). swapchain_format folgt dagegen dem Backbuffer -- im Multipass
            // ist das B8G8R8A8 (Log: "Format: 87"). RGBA-Quelle in ein
            // BGRA-Ziel kopiert heisst: R und B vertauscht. Sichtbar wurde es an
            // den Farbknoepfen fuer Laser/Crosshair -- "Red" stand in blauer
            // Schrift, ebenso alle orangen Ueberschriften. Unter OpenVR faellt es
            // nicht auf, weil die Flaeche dort ueber vr::VROverlay laeuft.
            //
            // Die _SRGB-Variante bleibt wie gehabt -- geaendert wird NUR die
            // Kanalreihenfolge, an Helligkeit/Gamma damit nichts.
            //
            // Betroffen ist ausschliesslich dieses Slate: es wird nur befuellt,
            // solange `ui_layer` steht (also das Framework-Menue offen ist).
            // Augen-Swapchains und Flatscreen-Leinwand behalten swapchain_format.
            const DXGI_FORMAT ui_slate_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.arraySize = 1;
            ci.format = ui_slate_format;
            // [VR-MENUE-KONTEXT 11.09.2026] Groesse des VR-Menues statt des
            // Backbuffers -- REFrameworks VR-Rendertarget ist jetzt genau so gross,
            // die Kopie in D3D12Component::on_frame passt damit 1:1.
            ci.width = (uint32_t)REFramework::VR_MENU_WIDTH;
            ci.height = (uint32_t)REFramework::VR_MENU_HEIGHT;
            ci.mipCount = 1;
            ci.faceCount = 1;
            ci.sampleCount = 1;
            ci.usageFlags = XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

            runtimes::OpenXR::Swapchain sc{};
            sc.width = ci.width;
            sc.height = ci.height;

            // [RUECKFALL] Bietet die Laufzeit das Format nicht an, waere sonst gar
            // kein Menue mehr im Headset -- schlechter als falsche Farben. Also
            // zurueck auf das bisherige Format, dann ist das Verhalten exakt das
            // von vorher.
            auto slate_format = ui_slate_format;
            auto slate_res = xrCreateSwapchain(openxr->session, &ci, &sc.handle);

            if (slate_res != XR_SUCCESS) {
                REVR_LOG(spdlog::warn("[VR] UI slate: format {} refused by runtime, falling back to {}.",
                             (uint32_t)ui_slate_format, (uint32_t)swapchain_format));
                ci.format = swapchain_format;
                slate_format = swapchain_format;
                slate_res = xrCreateSwapchain(openxr->session, &ci, &sc.handle);
            }

            if (slate_res == XR_SUCCESS) {
                const auto ui_idx = (uint32_t)openxr->views.size() + 1;
                openxr->swapchains.push_back(sc);

                uint32_t image_count{};

                if (xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr) == XR_SUCCESS && image_count > 0) {
                    auto& uctx = this->contexts[ui_idx];
                    uctx.textures.clear();
                    uctx.textures.resize(image_count);
                    uctx.texture_contexts.clear();
                    uctx.texture_contexts.resize(image_count);

                    for (uint32_t j = 0; j < image_count; ++j) {
                        uctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
                    }

                    if (xrEnumerateSwapchainImages(sc.handle, image_count, &image_count,
                            (XrSwapchainImageBaseHeader*)&uctx.textures[0]) == XR_SUCCESS) {
                        for (uint32_t j = 0; j < image_count; ++j) {
                            uint32_t real_index{};
                            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

                            if (xrAcquireSwapchainImage(sc.handle, &ai, &real_index) != XR_SUCCESS) {
                                break;
                            }

                            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                            xrWaitSwapchainImage(sc.handle, &wi);

                            uctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
                            uctx.texture_contexts[j]->setup(device, uctx.textures[j].texture,
                                slate_format, slate_format,
                                (std::wstring{L"OpenXR UI Slate "} + std::to_wstring(j)).c_str());

                            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            xrReleaseSwapchainImage(sc.handle, &ri);
                        }

                        REVR_LOG(spdlog::info("[VR] OpenXR UI slate swapchain: {}x{} ({} images), format {}",
                                     sc.width, sc.height, image_count, (uint32_t)slate_format));
                    }
                }
            } else {
                REVR_LOG(spdlog::error("[VR] Failed to create OpenXR UI slate swapchain."));
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

	if (this->contexts.empty()) {
        return;
    }

    REVR_LOG(spdlog::info("[VR] Destroying swapchains."));

    this->wait_for_all_copies();

    for (auto i = 0; i < this->contexts.size(); ++i) {
        auto& ctx = this->contexts[i];
        ctx.texture_contexts.clear();

        // Es gibt zwei Zusatz-Swapchains (Leinwand, UI-Slate). Schlaegt eine davon beim
        // Anlegen fehl, sind es weniger Swapchains als Kontexte -- dann hier nicht ins
        // Leere greifen.
        if (i >= VR::get()->m_openxr->swapchains.size()) {
            ctx.textures.clear();
            continue;
        }

        auto result = xrDestroySwapchain(VR::get()->m_openxr->swapchains[i].handle);

        if (result != XR_SUCCESS) {
            REVR_LOG(spdlog::error("[VR] Failed to destroy swapchain {}.", i));
        } else {
            REVR_LOG(spdlog::info("[VR] Destroyed swapchain {}.", i));
        }

        ctx.textures.clear();
    }

    this->contexts.clear();
    VR::get()->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    D3D12_BOX* src_box, 
    D3D12_RESOURCE_STATES src_state,
    OpenXR::CopyFn copy_fn)
{
    REF_PROFILE_FUNCTION();

    std::scoped_lock _{this->mtx};

    auto& vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->m_openxr->get_synchronize_stage() != VRRuntime::SynchronizeStage::VERY_LATE) {
            REVR_LOG(spdlog::error("[VR] OpenXR: Frame not begun when trying to copy."));
            return;
        }
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        REVR_LOG(spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx));
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        REVR_LOG(spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
        REVR_LOG(spdlog::info("[VR] Attempting to correct..."));

        for (auto& texture_ctx : ctx.texture_contexts) {
            if (texture_ctx != nullptr) {
                texture_ctx->commands.reset();
            }
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        REVR_LOG(spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            REVR_LOG(spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
        } else if (texture_index >= ctx.texture_contexts.size() || ctx.texture_contexts[texture_index] == nullptr) {
            // [PIMAX 16.09.2026] Letzte Sicherung: nie in einen fehlenden Kontext kopieren,
            // das Bild nur zurueckgeben. Kostet einen Frame statt eines Absturzes.
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

            if (xrReleaseSwapchainImage(swapchain.handle, &release_info) == XR_SUCCESS) {
                ctx.num_textures_acquired--;
            }
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            // [IMAGE_SHIFT 2026-08-11] Bild-Versatz unter OpenXR am GLEICHEN Angriffspunkt
            // wie unter OpenVR: dort verschiebt VR::get_shifted_bounds den AUSSCHNITT des
            // fertigen Bildes beim Submit, das Fadenkreuz wandert also mit. Vorher wurde
            // der Versatz in XR ersatzweise in die Projektion gerechnet (VR.cpp) -- das
            // verschiebt aber die gerenderte WELT, waehrend das GUI-Fadenkreuz stehen
            // bleibt, und es skaliert zusaetzlich mit dem Zoom. Genau daher stimmten die
            // in OpenVR eingestellten Scope-Offsets in XR nicht mehr.
            // Greift nur beim einfachen Weg (kein eigener src_box, keine CopyFn): der
            // geschwaerzte Zweig braucht ihn nicht, und im Upscaler-Multipass-Zweig
            // schreibt render_srv_to_rtv das Bild selbst.
            const auto shift_x = vr->get_image_shift_x();
            const auto shift_y = vr->get_image_shift_y();

            const auto dst_desc = ctx.textures[texture_index].texture->GetDesc();
            const auto src_desc = resource->GetDesc();

            const bool same_size = src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height;
            const bool has_shift = same_size && src_box == nullptr && (shift_x != 0.0f || shift_y != 0.0f);

            if (copy_fn == nullptr) {
                if (has_shift) {
                    const auto w = (int32_t)dst_desc.Width;
                    const auto h = (int32_t)dst_desc.Height;

                    // Vorzeichen wie in get_shifted_bounds: dort wandert der gelesene
                    // Bereich um -shift, das Bild im Auge also um +shift.
                    int32_t dx = (int32_t)(shift_x * (float)w);
                    int32_t dy = (int32_t)(shift_y * (float)h);

                    if (dx > w - 1) dx = w - 1;
                    if (dx < -(w - 1)) dx = -(w - 1);
                    if (dy > h - 1) dy = h - 1;
                    if (dy < -(h - 1)) dy = -(h - 1);

                    D3D12_BOX box{};
                    box.left   = dx < 0 ? (UINT)(-dx) : 0u;
                    box.right  = dx < 0 ? (UINT)w : (UINT)(w - dx);
                    box.top    = dy < 0 ? (UINT)(-dy) : 0u;
                    box.bottom = dy < 0 ? (UINT)h : (UINT)(h - dy);
                    box.front  = 0;
                    box.back   = 1;

                    // Erst schwarz, sonst steht im freiwerdenden Rand der alte Frame.
                    const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
                    texture_ctx->commands.clear_rtv(*texture_ctx, black, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    texture_ctx->commands.copy_region(
                        resource,
                        ctx.textures[texture_index].texture,
                        &box, src_state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                        dx > 0 ? (UINT)dx : 0u,
                        dy > 0 ? (UINT)dy : 0u);
                } else if (src_box != nullptr) {
                    texture_ctx->commands.copy_region(
                        resource,
                        ctx.textures[texture_index].texture,
                        src_box, src_state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    texture_ctx->commands.copy(
                        resource,
                        ctx.textures[texture_index].texture,
                        src_state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                copy_fn(texture_ctx->commands, *texture_ctx, src_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                REVR_LOG(spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
                REVR_LOG(spdlog::info("[VR] Attempting to correct..."));

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    REVR_LOG(spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    if (texture_ctx != nullptr) {
                        texture_ctx->commands.wait(INFINITE);
                    }
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                REVR_LOG(spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result)));
                return;
            }

            ctx.num_textures_acquired--;
        }
    }
}
} // namespace vrmod
