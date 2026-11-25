#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>

#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <span>

#include <d3d12.h>

#include <imgui.h>
#include <reframework/API.hpp>
#include <utility/Memory.hpp>
#include <spdlog/spdlog.h>
#include <sdk/Renderer.hpp>
#include <sdk/REManagedObject.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/RETypeDefinition.hpp>
#include <sdk/REComponent.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/SystemArray.hpp>

#include "REFramework.hpp"
#include "Mods.hpp" // [TIMELINE 04.11.2026] the mods' names for the GUI callbacks
#include "VR.hpp"
#include "TemporalUpscaler.hpp"

#include "vr/EyeGaze.hpp"
#include "vr/QuadView.hpp"
#include "vr/QuadViewGame.hpp"
#include "vr/CpuStats.hpp"         // [CPU 03.11.2026] render thread, GPU waits, transform hook
#include "vr/ThreadCpuSampler.hpp" // [CPU THREADS 03.11.2026] busy-ness of the game's threads (log writer thread)
#include "vr/d3d12/SubmitTimeline.hpp" // [SUBMIT TIMELINE 06.11.2026] the GPU timeline of the frame

#include "FoveatedRendering.hpp"
#include <cstdarg>


#include <dxgi1_4.h>   // [MEMORY 05.11.2026] video memory (IDXGIAdapter3::QueryVideoMemoryInfo)
#include <psapi.h>     // ... and the process's own memory
#include <wrl/client.h>

#if defined(RE4)
#include "vr/games/RE4VR.hpp" // re4vr::perf: per-mod CPU cost of the callbacks
#endif

namespace {
using Clock = std::chrono::steady_clock;

constexpr double BENCH_PHASE_SECONDS = 15.0;
constexpr double BENCH_WARMUP_SECONDS = 6.0; // includes the upscaler / render-target re-init after every switch
constexpr int BENCH_PHASES = 6;              // OFF, ON, OFF, ON, OFF, ON
constexpr double SIZE_EDIT_DELAY_SECONDS = 1.0;

// [GPU TEST 29.10.2026] The GPU pass test: after a countdown (close the menu, stand still) every variant runs for
// PTEST_PHASE_SECONDS, the first PTEST_WARMUP_SECONDS of each not counted (new pipelines, the histories settling). The
// current build runs first and last: the difference between the two shows how much the scene drifted.
constexpr double PTEST_COUNTDOWN_SECONDS = 8.0;
constexpr double PTEST_PHASE_SECONDS = 15.0;
constexpr double PTEST_WARMUP_SECONDS = 4.0;

struct PassVariant {
    const char* name;
    bool same_image;
};

constexpr PassVariant PASS_VARIANTS[] = {
    {"current build", true},
    {"reprojection as one pass (original)", true},
    {"eye pass: interface tested on every tile", true},
    {"eye pass: without the GUI map", true},
    {"eye image in three separate passes", true},
    {"eye pass without the interface", false},
    {"eye pass without the post pass", false},
    {"round 2 passes (TAA, output curve, fovea interface clean-up, reprojection)", true},
    {"eye images in 10 bit, then the conversion pass", true},
    {"eye pass: exposure gain made in every group (before 30.10)", true},
    {"eye pass: groups of 16 x 8 pixels", true},
    {"eye pass: groups of 8 x 8 pixels", true},
    {"eye pass: groups of 32 x 16 pixels", true},
    {"eye pass: groups of 32 x 32 pixels (= current build)", true},
    {"eye pass: groups of 16 x 16 pixels (before 31.10)", true},
    {"fovea TAA: groups of 32 x 32 pixels", true},
    {"wide view TAA: groups of 32 x 32 pixels", true},
    {"eye pass: 30 x 30 pixels on 32 x 32 threads", true},
    {"eye pass before the ALU reductions (31.10 evening)", true},
    {"eye pass: ALU reductions without the branches", true},
    {"eye pass of the 22:51 build (31.10)", true},
    {"reprojection: block minima from the texture (before 31.10 night)", true},
    {"eye pass of the 11:33 test build (01.11)", true},
    {"TAA passes: halo read in two rounds (before 01.11)", true},
    {"pre-fovea work in a list of its own (before 01.11 afternoon)", true},
    {"fovea exposure: one gain for every brightness (before 01.11 evening)", false},
    {"fovea without the periphery's large-scale light (before 01.11 night)", false},
    {"fovea exposure: one gain, no large-scale light (before 01.11 evening)", false},
    {"periphery gaps beside near things mirrored (before 02.11)", false},
    {"green item beams at the panels' depth outside the fovea (before 03.11)", false},
    {"fovea without its RCAS sharpening after FSR 4", false},
    {"eye pass without the global sharpening (post pass)", false},
    // [fix94 19.11.2026] today's chain (the supersampling TAA in the fovea and in the periphery, as the menu has them) and
    // what its parts cost (32 - 37: the periphery's anti-aliasing is left as the menu has it)
    {"today's chain", true},
    {"periphery interface not collected over the frames", false},
    {"periphery interface collected only around it", false},
    {"periphery outlines per ray (no outlines per pixel)", false},
    {"periphery image of the wide view's size", false},
    {"fovea history 1x", false},
    // [fix95 19.11.2026] what the eye pass's parts cost in today's chain (38 - 40; 5, 6 and 31 are the same switches with
    // the light TAA in the periphery)
    {"today's chain: eye pass without the interface", false},
    {"today's chain: eye pass without the post pass", false},
    {"today's chain: eye pass without the global sharpening", false},
    // [fix99 19.11.2026] what matching the fovea's light to the periphery costs today (41, 42; 26 and 27 with the light TAA)
    {"today's chain: without the large-scale light from the periphery", false},
    {"today's chain: without tone match and large-scale light", false},
    // [CALM fix100 20.11.2026] what the third choice of the periphery's anti-aliasing costs (against 36: the supersampling
    // TAA with the image of the wide view's size)
    {"today's chain: periphery with the calm light TAA", false},
    // [fix102 20.11.2026] where the eye pass's interface time goes (0.39 ms still, fix99 changed nothing): the fovea's
    // backdrop test (44), and the interface worked on every tile instead of the tile test (45)
    {"today's chain: eye pass without the fovea's backdrop test", false},
    {"today's chain: eye pass with the interface on every tile (no tile test)", false},
    // [TILE PASS fix105 20.11.2026] the tiles' test inside the eye pass as before fix105 (46): what the tile pass gains
    // ([fix106] this pass's bytecode is fix104's, byte for byte)
    {"today's chain: the tiles' interface test inside the eye pass (fix104's eye pass)", true},
    // [fix106 20.11.2026] what the interface costs in the tiles that have it (47; against 38: what it costs everywhere else)
    {"today's chain: the tile pass marks no tile", false},
    // [fix108 20.11.2026] where the interface's 0.25 ms outside its tiles go: the eye pass's text without the two calls of
    // the interface (48, against 47: the same work, only the text differs) and with them behind a loop (49, same image)
    {"today's chain: eye pass compiled without the interface's calls", false},
    {"today's chain: the interface's calls behind a loop", true},
    // [TWO PASSES fix111 20.11.2026] the eye image in one pass with the interface's calls, as fix106 - fix110 (50)
    {"today's chain: the eye image in one pass (as fix110)", true},
    // [TWO PASSES fix112 20.11.2026] the first pass leaves the tiles with interface to the second (51)
    // ([MARKED FIRST fix113] today's chain does that and records the marked tiles' pass first: 51 records it after)
    // ([fix115] 51: the chain of fix113 - fix114, the first pass leaving the marked tiles to their pass, recorded first)
    {"today's chain: the first eye pass leaves the marked tiles to their pass (fix113)", true},
    // [MARKED FIRST fix113 21.11.2026] the two passes as fix112 (52): every tile made, then the marked ones again
    // ([fix115] today's chain again: 52 changes nothing)
    {"today's chain: the two eye passes as fix112", true},
    // [HUD BLUR fix114 21.11.2026] the HUD objects' blur filters left as the game draws them (53)
    // ([fix115] hiding them changed nothing and is gone: 53 changes nothing)
    {"today's chain (was: the weapon HUD's background blur as the game draws it)", true},
    // [MARKED TEXT fix115 21.11.2026] the marked tiles' pass in parts (54, 56: today's text without one part; 55: the text
    // of fix114, every anchor reading its mask)
    {"today's chain: marked tiles without the line search behind panels", false},
    {"today's chain: marked tiles' text of fix114 (every anchor reads its mask)", true},
    {"today's chain: marked tiles without the panels' depth where no anchor is found", false},
    // [COPIES fix116 21.11.2026] the engine's copies for the interface (57: the fovea views' two each not made; 58: and the
    // wide view's curve not measured, its picture not cleaned). The same image while no interface is in the picture.
    {"today's chain: the fovea views' interface copies not made", false},
    {"today's chain: no fovea interface copies, no output curve, wide view not cleaned", false},
    // [SMALL GROUPS fix117 21.11.2026] the marked tiles' pass in groups of 8 x 8 pixels (59: the same image), and made by
    // the text without the interface's calls (60: measurement only, no interface outside the fovea)
    {"today's chain: marked tiles' pass in groups of 8 x 8 pixels", true},
    {"today's chain: marked tiles made again without the interface's calls (no HUD outside the fovea)", false},
    // [NO INTERFACE fix118 21.11.2026] the wide view's cleaning as up to fix117 (both copies read in every frame); the same
    // image. Not in the test's order (it tells only without interface in the picture, the order of fix117 / fix118 needs
    // the weapon HUD): the pass's own time is the span ui_clean_wide of the periodic line.
    {"today's chain: wide view cleaned without the map's whole-frame texel (as fix117)", true},
    // [ENDS ONLY fix119 22.11.2026] without the timestamps between the mod's passes (only "wall" is measured: the line's
    // ours_total and spans are empty), and the same with the eye pass without the interface's calls (63, measurement only)
    {"today's chain: no timestamps between the passes (read wall)", true},
    {"today's chain: no timestamps between the passes, eye pass without the interface's calls (read wall)", false},
    // [DIRECT fix120 22.11.2026] the marked tiles made again by every thread on its own (kind 11; measurement only)
    {"today's chain: marked tiles made again without the stages, every thread its pixel (no HUD outside the fovea)", false},
    // [OVER fix121 22.11.2026] the user's test: the HUD put over the pixels the first pass wrote instead of over the scene
    // made again (kind 12). Only the HUD's pixels can differ. Also to look at by hand ("GPU comparison").
    {"today's chain: HUD over the scene composed again (as fix120)", false},
    // [UI MOTION fix125 22.11.2026] the fovea's anti-aliasing and the periphery's interface pass along the scene's motion
    // vectors again (66): what the interface's own motion costs on the GPU, and by hand ("GPU comparison") how the
    // interface looked before. Only interface pixels and their rims can differ.
    {"today's chain: interface along the scene's motion vectors (as fix124)", false},
};
constexpr int PASS_VARIANT_COUNT = (int)(sizeof(PASS_VARIANTS) / sizeof(PASS_VARIANTS[0]));
// [GPU TEST 01.11.2026, end of the GPU work] the current build against the eye pass before the ALU reductions of 31.10
// (18, the biggest single step of the last days: a check that the gain is still there); every variant stays in the menu.
// The 16-bit trials of 01.11 (min16float: +0.020 ms; shader model 6.2 DXIL: +0.344 ms) are removed.
// [ONE LIST 01.11.2026] the GPU work opened again for the last points of 30.10. Test of 15:50: the wide view's passes on a
// compute queue in parallel with the fovea passes +0.046 ms "wall" (normal and high priority alike: both queues' passes
// slowed by each other, 0.05 + 0.07 ms between the queues; removed), the pre-fovea work on the copier's list -0.010 ms (now
// the build's way with the mod's TAA in the fovea). The test compares it with a pre list of its own (24) on "wall" (the
// direct queue from the first pre stamp to the copier's end: the mod's total leaves out the gap between two lists).
// [FOVEA TONE 01.11.2026 evening] what the tone table costs (the measure's histograms and the table's pass, one group):
// test of 18:00 +0.034 ms (0.974 -> 1.008). [LIGHT 01.11.2026 night] what the periphery's large-scale light costs (the
// blocks' means, their blur, one read of four blocks per fovea pixel): the current build against it off (26).
// [BLOCKS 02.11.2026] table and light from one blocks pass (blocks, match, separable blur; before 02.11 measure with
// histograms, tone, light_reduce, light_blur 2-D: +0.086 ms over the gain alone, 0.974 -> 1.056 on 01.11 night): the
// current build against both off (27, the gain alone: measure and gain as before) and against the light off (26).
// [GPU 07.11.2026] What the two sharpenings cost (image changed, measurement only): the fovea's RCAS after FSR 4 (30) and
// the post pass's sharpening of the whole eye image (31). Test of 07.11 18:00: 0.021 and 0.066 ms per frame. (The eye
// pass changes of 07.11 tested in the same test and removed: the anchor list made the pass 0.43 instead of 0.30 ms per
// eye in either loop form, no periphery under the fovea +0.003; the reprojection read-ahead was slower.)
// [fix94 19.11.2026] RE4 test of 19.11.2026: the mod's passes 2.7 - 3.5 ms of the frame with the supersampling TAA in
// the fovea (history 2x) and in the periphery (image 2x), 1.8 - 1.9 on 06.11 with FSR 4 and the light TAA. The test now
// measures that chain (before: the light TAA in the periphery whatever the menu said) and its parts in one place: the
// interface collected over the frames (the whole image; only around the interface: the new option), the outlines per
// pixel, the periphery's image 2x, the fovea's history 2x. To be run with the HUD in the picture.
constexpr int PTEST_REF = 32;
// [fix95 19.11.2026] The test of fix94 was run (RE4, 19.11.2026 17:3x): the interface collected
// over the whole image 0.32 ms (around it 0.02: now the option's default), the outlines per pixel 0.24, the periphery's
// image 2x 0.42, the fovea's history 2x 0.20. The eye passes are the largest part left (1.05 ms of 2.95, 1.24 with the
// HUD): the test now tells their parts (interface, post pass, sharpening); the chain is the menu's, with the interface
// collected as the option says. (Order of fix94: 32, 33, 32, 34, 32, 35, 32, 36, 32, 37, 32.)
// [fix99 19.11.2026] test of fix95 - 98 (RE4, 19.11.2026): the eye passes' interface 0.36 ms with a small HUD, 3 anchored
// elements and 3 - 5 item beams (fix99 leaves out the depths a tile has no marked block for), post pass and sharpening
// nothing. Now: the interface again (what is left of it), and the light matched to the periphery (the user's question).
// (Order of fix95: 32, 38, 32, 39, 32, 40, 32.)
// [CALM fix100 20.11.2026] ... and the periphery with the supersampling TAA's image of the wide view's size (36) and with
// the calm light TAA (43); 42 (tone match and light together) left out to keep the test short. (Order of fix99: 32, 38,
// 32, 41, 32, 42, 32.)
// [fix102 20.11.2026] the test of fix101: interface 0.39 ms (fix99 changed nothing), large-scale light 0.05, the image of
// the wide view's size 0.50, the calm light TAA 0.27 in taa_wide. Now the eye pass's interface in parts. (Order of fix100:
// 32, 38, 32, 41, 32, 36, 32, 43, 32.)
// [TILE PASS fix105 20.11.2026] the test of fix104 (DRED off): interface 0.36 ms; without the fovea's backdrop test +0.001
// (it costs nothing); with the interface on every tile +0.64 (1.00 in all: the tile test saves two thirds, most of the
// rest is the test itself). Now the tile pass against the test inside the eye pass, and what is left of the interface.
// (Order of fix102: 32, 38, 32, 44, 32, 45, 32.)
// [MARKED FIRST fix113 21.11.2026] the test of fix112: the first pass leaving the marked tiles out +0.010 / -0.021 ms (the
// noise), the single pass of fix110 +0.20 / +0.24. Now: the marked tiles' pass recorded first (today's chain) against the
// two passes of fix112 (52) and against the same texts in fix112's order (51); the periphery's interface not collected
// over the frames (33: the user's question whether smoothing the HUD is what costs; the HUD's image changes); the eye
// image without interface (48: the floor). (Order of fix112: 32, 51, 32, 50, 32, 51, 32.)
// [HUD BLUR fix114 21.11.2026] the test of fix113: the two passes of fix112 +0.076 (eyes +0.080), the marked tiles' pass
// after the eye image's -0.016 (the order changes nothing), the periphery's interface not collected -0.048, the eye image
// without interface -0.167. Now the engine's side: the weapon HUD (Gui_ui2030) holds two blur filters, hidden by today's
// chain; 53 leaves them, twice, and each phase's line has the engine's render time. 52 once more. (Order of fix113: 32,
// 52, 32, 51, 32, 33, 32, 48, 32.)
// [MARKED TEXT fix115 21.11.2026] the test of fix114 (under the frame time, runs within 0.003 ms): the HUD's blur filters
// shown or hidden, the engine's render time is the same (4.285 / 4.286 against 4.280 - 4.292); fix112's two eye passes
// -0.038 against the chain of fix113. Now the marked tiles' pass in parts (54, 55, 56) and without it (48: the floor).
// (Order of fix114: 32, 53, 32, 52, 32, 53, 32.)
// [COPIES fix116 21.11.2026] the test of fix115: without the line search -0.001, the text of fix114 -0.001, without the
// panels' depth -0.006, without the marked tiles' pass -0.093 (none of the parts is where its time goes). Now what the
// engine's lists carry for the mod in every frame: the fovea views' interface copies (57, twice) and what else runs for
// an interface that is not in the picture (58). To be run WITHOUT interface in the picture (no grip, no beams).
// (Order of fix115: 32, 54, 32, 55, 32, 56, 32, 48, 32.)
// [SMALL GROUPS fix117 21.11.2026] the test of fix116 (light spot, no interface, runs within 0.002 ms): the fovea views'
// four interface copies cost the engine 0.028 ms and the mod 0.015; with the curve not measured and the wide view not
// cleaned the mod 0.054 less. Now the marked tiles' pass again, which costs 0.093 ms with the weapon HUD and does not get
// lighter by its interface code: in groups of 8 x 8 pixels (59, the same image), made by the text without the
// interface's calls (60: what a second pass over those tiles costs by itself) and not made (48: the floor). To be run
// WITH the weapon HUD in the picture (grip held, standing still).
// (Order of fix116: 32, 57, 32, 58, 32, 57, 32.)
// [ENDS ONLY fix119 22.11.2026] the test of fix118 (weapon HUD, about 160 tiles marked per eye, runs within 0.004 ms): the
// two eyes 1.008 today; in groups of 8 x 8 pixels 1.031 / 1.033 (slower: not taken); the marked tiles made again without
// the interface's calls 0.978; no second pass 0.914. So of the second pass's 0.094 ms, 0.064 is making 0.35 % of the
// tiles again at all (the first pass makes all of them in 0.914) and 0.030 the interface: the time is the pass's own
// run from start to end, not its amount of work. Every pass ends at a timestamp of the spans; if those keep the next
// pass from starting while this one ends, they cost time themselves and the short pass looks dearer than it is in a
// build without them. Now measured on "wall" (the two stamps at the ends stay): 62 without the stamps between the
// passes, 63 the same with the eye pass without the interface's calls (the floor of 48). WITH the weapon HUD.
// (Order of fix117 / fix118: 32, 59, 32, 60, 32, 48, 32, 59, 32.)
// [DIRECT fix120 22.11.2026] the test of fix119 (weapon HUD, "wall", runs within 0.004 ms of their neighbours): without
// the timestamps between the passes 2.570 / 2.566 against 2.574 - 2.565 with them (-0.002: they cost nothing and hide
// nothing); the eye pass without the interface's calls 2.479 / 2.475 without them and 2.475 with them. The second pass
// costs its 0.090 ms either way. What is left to tell apart is the stages of a group (each ends at a barrier of the
// whole group) from the work itself: 64 makes the marked tiles again with every thread making its pixel on its own
// after the group's values, against 60 (the same through the stages, 0.064 over the floor in fix118) and 48 (the
// floor). WITH the weapon HUD.
// (Order of fix119: 32, 62, 32, 63, 32, 62, 32, 63, 32, 48, 32.)
// [OVER fix121 22.11.2026] the test of fix120 (weapon HUD, runs within 0.001 ms): the two eyes 1.006 today; the marked
// tiles made again without the stages 0.975 / 0.974, through the stages 0.979, not at all 0.914. The stages are worth
// 0.004: making those tiles' pixels again costs 0.06 ms however it is done. Asked for by the user as a quick test: the
// HUD over the pixels the first pass wrote (65), which makes no pixel again; against the floor (48).
// (Order of fix120: 32, 64, 32, 60, 32, 48, 32, 64, 32.)
// [OVER fix122 22.11.2026] the test of fix121 (weapon HUD): the two eyes 1.008 today, with the HUD over the stored pixels
// 0.977 / 0.981 (-0.030), no second pass 0.915; the user found the HUD's picture the same. It is the build's way now
// (EyeCompose::Params::marked_over); 65 is the way before. The small game window of fix122 cannot be a phase (the window
// is resized): it is compared by switching the option in the same place.
// (Order of fix121: 32, 65, 32, 48, 32, 65, 32.)
// [UI MOTION fix125 22.11.2026] The interface's own motion in the two temporal passes: against the same passes along the
// scene's motion vectors (66). To run with the HUD in the picture: the fovea's pass reads two more images per pixel then.
// (Order of fix122 - fix124: 32, 65, 32.)
constexpr int PTEST_ORDER[] = {32, 66, 32, 66, 32};

// [BUILD LABEL fix104 20.11.2026] Which build runs: in foveation_debug.log (module loaded, every snapshot), in REFramework's
// log and under the menu's heading. The user's test of 20.11.2026 13:53 was fix101 once more: the compile of fix103 had
// failed without being noticed (the package "over fix102" on a tree still at fix101: GpuGuard.hpp of fix102 missing) and
// the DLL of the build before stayed in the game's folder. The name is changed with every delivered build; the time is this
// file's compile time.
constexpr const char* QV_BUILD = "fix158";

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string qv_build_text() {
    return std::format("{} (compiled {} {})", QV_BUILD, __DATE__, __TIME__);
}
#endif

// [BUILD STAMP fix107 20.11.2026] the main sources the DLL was linked from (quadview::BuildStamps), and what is wrong with
// them ("": nothing): a source without a stamp or one that saw another HEADER_REV comes from an object file the build did
// not make again
QUADVIEW_BUILD_STAMP(FoveatedRendering);

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string qv_sources_text() {
    const auto& s = quadview::BuildStamps::get();
    std::string o{};

    for (int i = 0; i < s.count; ++i) {
        o += std::format("{}{} {} (headers {})", i > 0 ? "; " : "", s.source[i], s.compiled[i], s.rev[i]);
    }

    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string qv_sources_problem() {
    static constexpr const char* EXPECTED[] = {"FoveatedRendering", "TemporalUpscaler", "VR", "CameraDuplicator", "FoveaComposite", "D3D12Component"};
    const auto& s = quadview::BuildStamps::get();
    std::string o{};

    for (const auto* name : EXPECTED) {
        int found = -1;

        for (int i = 0; i < s.count; ++i) {
            if (std::string_view{s.source[i]} == std::string_view{name}) {
                found = i;
                break;
            }
        }

        if (found < 0) {
            o += std::format("{}{} has no stamp (compiled before fix107)", o.empty() ? "" : "; ", name);
        } else if (s.rev[found] != quadview::HEADER_REV) {
            o += std::format("{}{} was compiled with headers {} instead of {}", o.empty() ? "" : "; ", name, s.rev[found], quadview::HEADER_REV);
        }
    }

    return o;
}
#endif

// [GUARD fix101 20.11.2026] the GPU guard's steps, from the least visible: the periphery's outlines per ray instead of per
// pixel (only with its image of twice the size), its image of the wide view's size, the calm light TAA in the periphery,
// and, only when the frame rate collapses, the fovea's history 1x. What each saves (GPU time per frame): the user's RE4
// tests of 19.11.2026 (outlines 0.24 - 0.28 ms, the image of the wide view's size 0.42 - 0.53 with them, the fovea's
// history 0.16 - 0.20); the calm light TAA against the supersampling one is not measured yet (the passes' sizes: about
// 0.2).
enum : uint8_t { GS_OUTLINES = 0, GS_PERIPHERY_1X = 1, GS_CALM = 2, GS_FOVEA_1X = 3 };
constexpr const char* GS_NAMES[] = {"the periphery's outlines per ray", "the periphery's image of the wide view's size",
    "the calm light TAA in the periphery", "the fovea's history 1x"};
// ([fix102 20.11.2026] the user's GPU test of fix101: the image of the wide view's size 0.50 ms, the calm light TAA 0.27 in
// taa_wide against the supersampling one; the fovea's history about 0.12 in the frames of the guard's steps)
constexpr double GS_SAVE_OUTLINES = 0.26;
constexpr double GS_SAVE_1X_WITH_OUTLINES = 0.50;
constexpr double GS_SAVE_CALM = 0.25;
constexpr double GS_SAVE_FOVEA = 0.12;
constexpr auto GS_PREROLL = std::chrono::milliseconds(400);
constexpr int PTEST_PHASES = (int)(sizeof(PTEST_ORDER) / sizeof(PTEST_ORDER[0]));

// [ENGINE VIEWS 30.10.2026] The engine view cost test: the views at three render sizes (the fovea ratio times 1, 1.25,
// 1.5) and once as four views (a wide view per eye, the slider's fovea size). How the engine's GPU time follows the
// pixels tells the part of every view that does not depend on its resolution (shadow maps, culling, ...). Each phase
// re-creates render targets: a longer warm-up.
constexpr double VTEST_COUNTDOWN_SECONDS = 8.0;
constexpr double VTEST_PHASE_SECONDS = 18.0;
constexpr double VTEST_WARMUP_SECONDS = 8.0;

struct ViewPhase {
    float ratio_scale;
    bool four_views;
};

constexpr ViewPhase VTEST_PHASES[] = {{1.0f, false}, {1.5f, false}, {1.0f, false}, {1.25f, false}, {1.0f, true}, {1.0f, false}};
constexpr int VTEST_PHASE_COUNT = (int)(sizeof(VTEST_PHASES) / sizeof(VTEST_PHASES[0]));

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// average time of the named span in a row (0 when it has no samples); [ONE LIST 01.11.2026] a name can stand for a span
// of each stamp layout: the frames of all of them averaged together
double span_ms(const std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT>& sp, const char* name) {
    double sum = 0.0;
    uint64_t n = 0;

    for (uint32_t i = 0; i < d3d12::GpuSpans::SPAN_COUNT; ++i) {
        if (std::string_view{d3d12::GpuSpans::SPANS[i].name} == name && sp[i].samples > 0) {
            sum += sp[i].avg_ms * (double)sp[i].samples;
            n += sp[i].samples;
        }
    }

    return n > 0 ? sum / (double)n : 0.0;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [ONE LIST 01.11.2026] the span names in their first order, each once
std::vector<const char*> span_names() {
    std::vector<const char*> names{};

    for (uint32_t i = 0; i < d3d12::GpuSpans::SPAN_COUNT; ++i) {
        const char* n = d3d12::GpuSpans::SPANS[i].name;

        if (std::none_of(names.begin(), names.end(), [&](const char* m) { return std::string_view{m} == n; })) {
            names.push_back(n);
        }
    }

    return names;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// the mod's time per frame: its spans added up. [ONE LIST 01.11.2026] a span only some frames have (another stamp layout)
// counts for its share of the frames (the frames = the most samples of one of the mod's spans; with one layout every span
// has them all and this is the plain sum). in_frame: without the post pass and the conversion (sent after the frame).
double ours_total_ms(const std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT>& sp, bool in_frame = false) {
    double sum = 0.0;
    uint32_t frames = 0;

    for (uint32_t i = 0; i < d3d12::GpuSpans::SPAN_COUNT; ++i) {
        const auto& d = d3d12::GpuSpans::SPANS[i];

        if (!d.ours || sp[i].samples == 0 ||
            (in_frame && (std::string_view{d.name} == "post" || std::string_view{d.name} == "convert(10->8 bit)"))) {
            continue;
        }

        sum += sp[i].avg_ms * (double)sp[i].samples;
        frames = (std::max)(frames, sp[i].samples);
    }

    return frames > 0 ? sum / (double)frames : 0.0;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [GPU 30.10.2026] The lens' hidden area (OpenVR hidden area mesh, never shown by the headset) per eye: its triangles, the
// share of the eye image they cover and the share of the eye pass's 16 x 16 tiles lying wholly inside them (tiles an eye
// pass could leave out), with the range of the vertices (to see the mesh's coordinates). Made once per image size.
std::string hidden_area_report(uint32_t w, uint32_t h) {
    static std::string cached{};
    static uint32_t cached_w = 0;
    static uint32_t cached_h = 0;

    if (w == 0 || h == 0) {
        return "no eye image yet";
    }

    if (!cached.empty() && cached_w == w && cached_h == h) {
        return cached;
    }

    if (vr::VRSystem() == nullptr) {
        return "no OpenVR system";
    }

    std::string out{};
    const uint32_t tw = (w + 15) / 16;
    const uint32_t th = (h + 15) / 16;

    for (int eye = 0; eye < 2; ++eye) {
        const auto mesh = vr::VRSystem()->GetHiddenAreaMesh(eye == 0 ? vr::Eye_Left : vr::Eye_Right, vr::k_eHiddenAreaMesh_Standard);
        out += eye == 0 ? "L: " : " | R: ";

        if (mesh.pVertexData == nullptr || mesh.unTriangleCount == 0) {
            out += "no mesh";
            continue;
        }

        double area = 0.0;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        // per tile: how many of its 5 points (corners and centre) some triangle covers
        std::vector<uint8_t> covered((size_t)tw * th * 5, 0);

        for (uint32_t t = 0; t < mesh.unTriangleCount; ++t) {
            const auto* v = &mesh.pVertexData[t * 3];
            const double ax = v[0].v[0], ay = v[0].v[1], bx = v[1].v[0], by = v[1].v[1], cx = v[2].v[0], cy = v[2].v[1];
            const double cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            area += 0.5 * std::abs(cross);

            for (int k = 0; k < 3; ++k) {
                x0 = (std::min)(x0, v[k].v[0]);
                x1 = (std::max)(x1, v[k].v[0]);
                y0 = (std::min)(y0, v[k].v[1]);
                y1 = (std::max)(y1, v[k].v[1]);
            }

            if (cross == 0.0) {
                continue;
            }

            // the tiles under the triangle's bounds; a point is inside when its three edge signs agree with the winding
            const auto inside = [&](double px, double py) {
                const double e0 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * cross;
                const double e1 = ((cx - bx) * (py - by) - (cy - by) * (px - bx)) * cross;
                const double e2 = ((ax - cx) * (py - cy) - (ay - cy) * (px - cx)) * cross;
                return e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0;
            };

            const double mnx = (std::min)({ax, bx, cx}) * w, mxx = (std::max)({ax, bx, cx}) * w;
            const double mny = (std::min)({ay, by, cy}) * h, mxy = (std::max)({ay, by, cy}) * h;
            const uint32_t ta = (uint32_t)std::clamp(std::floor(mnx / 16.0), 0.0, (double)tw - 1.0);
            const uint32_t tb = (uint32_t)std::clamp(std::floor(mxx / 16.0), 0.0, (double)tw - 1.0);
            const uint32_t tc = (uint32_t)std::clamp(std::floor(mny / 16.0), 0.0, (double)th - 1.0);
            const uint32_t td = (uint32_t)std::clamp(std::floor(mxy / 16.0), 0.0, (double)th - 1.0);

            for (uint32_t ty = tc; ty <= td; ++ty) {
                for (uint32_t tx = ta; tx <= tb; ++tx) {
                    const double px0 = (double)(tx * 16) / w, px1 = (double)(std::min)(tx * 16 + 16, w) / w;
                    const double py0 = (double)(ty * 16) / h, py1 = (double)(std::min)(ty * 16 + 16, h) / h;
                    const double pts[5][2]{{px0, py0}, {px1, py0}, {px0, py1}, {px1, py1}, {(px0 + px1) * 0.5, (py0 + py1) * 0.5}};

                    for (int p = 0; p < 5; ++p) {
                        auto& c = covered[((size_t)ty * tw + tx) * 5 + p];
                        c = c != 0 || inside(pts[p][0], pts[p][1]) ? 1 : 0;
                    }
                }
            }
        }

        size_t hidden = 0;

        for (size_t i = 0; i < (size_t)tw * th; ++i) {
            hidden += covered[i * 5] && covered[i * 5 + 1] && covered[i * 5 + 2] && covered[i * 5 + 3] && covered[i * 5 + 4] ? 1 : 0;
        }

        out += std::format("{} triangles over {:.1f}% of the image, {:.1f}% of the 16x16 tiles wholly inside (corners and centre), vertices x {:.3f}..{:.3f} "
                           "y {:.3f}..{:.3f}",
            mesh.unTriangleCount, area * 100.0, 100.0 * (double)hidden / (double)((size_t)tw * th), x0, x1, y0, y1);
    }

    cached = out;
    cached_w = w;
    cached_h = h;
    return out;
}
#endif

// [ENGINE SHADOWS 30.10.2026] the engine's own render time of the last frame (via.render.Renderer get_RenderTime, ms; what
// it measures is not documented: logged next to the GPU time to compare), or a negative value
float engine_render_time_ms() {
    static const auto t = sdk::find_type_definition("via.render.Renderer");
    static const auto get = t != nullptr ? t->get_method("get_RenderTime") : nullptr;

    if (get == nullptr || get->get_function() == nullptr) {
        return -1.0f;
    }

    try {
        return get->call_safe<float>(sdk::get_thread_context());
    } catch (const std::exception&) {
        return -1.0f;
    }
}

// ([AIM COST fix112 - fix115] the engine's side of the cost of aiming, 0.15 - 0.19 ms: read here were the renderer's static
// values (none changes with the grip), via.render.Statistics (its counters stay 0 in the game also with StatisticsEnable
// on) and what the HUD objects are made of (the weapon panel's two blur filters: hidden or shown, the same time). All
// taken out in fix116; right_grip_frames stays.)

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [ENGINE VIEWS 30.10.2026] the mod's passes the frame's GPU time holds: the frame ends at early present, the post pass
// and the 10->8 bit conversion are sent in the present step after it (outside every frame's window)
double ours_in_frame_ms(const std::array<d3d12::GpuSpans::Span, d3d12::GpuSpans::SPAN_COUNT>& sp) {
    return ours_total_ms(sp, true); // [ONE LIST 01.11.2026] (= pre + copier, in both stamp layouts)
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
const char* quality_name(quadview::PluginQuality q) {
    switch (q) {
    case quadview::PluginQuality::Performance: return "Performance";
    case quadview::PluginQuality::Balanced: return "Balanced";
    case quadview::PluginQuality::Quality: return "Quality";
    case quadview::PluginQuality::UltraPerformance: return "Ultra Performance";
    case quadview::PluginQuality::UltraQuality: return "Ultra Quality";
    case quadview::PluginQuality::Native: return "Native";
    }

    return "?";
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [MEMORY 05.11.2026] Video memory as Windows accounts it for this process (IDXGIAdapter3::QueryVideoMemoryInfo: local =
// the card's own memory, non-local = system memory the GPU uses, each with the budget the OS gives the process) and the
// process's own memory. Sampled once a second on the main thread (a few microseconds).
struct MemorySample {
    bool video_ok{false};
    uint64_t local_usage{0};
    uint64_t local_budget{0};
    uint64_t nonlocal_usage{0};
    uint64_t nonlocal_budget{0};
    bool process_ok{false};
    uint64_t private_bytes{0};
    uint64_t working_set{0};
};

class AdapterMemory {
public:
    MemorySample sample(ID3D12Device* device) {
        MemorySample out{};

        if (device != nullptr) {
            const LUID luid = device->GetAdapterLuid();

            if (m_adapter != nullptr && (m_luid.LowPart != luid.LowPart || m_luid.HighPart != luid.HighPart)) {
                m_adapter.Reset();
                m_failed = false;
            }

            if (m_adapter == nullptr && !m_failed) {
                m_failed = true;
                using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);

                if (auto dxgi = GetModuleHandleW(L"dxgi.dll"); dxgi != nullptr) {
                    if (auto create = (CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1"); create != nullptr) {
                        Microsoft::WRL::ComPtr<IDXGIFactory4> factory{};
                        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter{};

                        if (SUCCEEDED(create(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
                            m_adapter = adapter;
                            m_luid = luid;
                            m_failed = false;
                        }
                    }
                }
            }

            if (m_adapter != nullptr) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local{};
                DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};

                if (SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                    SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
                    out.video_ok = true;
                    out.local_usage = local.CurrentUsage;
                    out.local_budget = local.Budget;
                    out.nonlocal_usage = nonlocal.CurrentUsage;
                    out.nonlocal_budget = nonlocal.Budget;
                }
            }
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);

        if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            out.process_ok = true;
            out.private_bytes = pmc.PrivateUsage;
            out.working_set = pmc.WorkingSetSize;
        }

        return out;
    }

    void reset() {
        m_adapter.Reset();
        m_failed = false;
    }

private:
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter{};
    LUID m_luid{};
    bool m_failed{false};
};

AdapterMemory& adapter_memory() {
    static AdapterMemory m{};
    return m;
}

std::string memory_line(const MemorySample& s, uint64_t peak_local) {
    constexpr double MB = 1024.0 * 1024.0;
    std::string o = "memory: ";

    if (s.video_ok) {
        o += std::format("video memory in use {:.0f} MB of the budget {:.0f} MB ({:.0f}%), highest in this interval {:.0f} MB | "
                         "system memory used by the GPU {:.0f} MB of {:.0f} MB",
            (double)s.local_usage / MB, (double)s.local_budget / MB, s.local_budget > 0 ? 100.0 * (double)s.local_usage / (double)s.local_budget : 0.0,
            (double)peak_local / MB, (double)s.nonlocal_usage / MB, (double)s.nonlocal_budget / MB);
    } else {
        o += "video memory not available";
    }

    if (s.process_ok) {
        o += std::format(" | the game's process: {:.0f} MB private, {:.0f} MB in RAM", (double)s.private_bytes / MB, (double)s.working_set / MB);
    }

    return o;
}
#endif
} // namespace

std::shared_ptr<FoveatedRendering>& FoveatedRendering::get() {
    static std::shared_ptr<FoveatedRendering> instance = std::make_shared<FoveatedRendering>();
    return instance;
}

std::optional<std::string> FoveatedRendering::on_initialize_d3d_thread() {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the build label and the stale object check: the debug log only
    // [BUILD LABEL fix104 20.11.2026]
    REVR_LOG(debug_log(std::format("[t={:.1f}s] tri-view module loaded, build {}", std::chrono::duration<double>(Clock::now() - m_start_time).count(),
        qv_build_text())));
    REVR_LOG(spdlog::info("[FoveatedRendering] tri-view build {}", qv_build_text()));
    REVR_LOG(spdlog::info("[FoveatedRendering] sources compiled: {}", qv_sources_text())); // [BUILD STAMP fix107]

    if (const auto problem = qv_sources_problem(); !problem.empty()) {
        REVR_LOG(spdlog::error("[FoveatedRendering] STALE OBJECT FILES: {}. The build skipped sources that changed: rebuild the whole project", problem));
    }
#endif
    return Mod::on_initialize_d3d_thread();
}

void FoveatedRendering::on_device_reset() {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the GPU timer, the GPU timeline and the tests: debug build only
    m_gpu_timer.reset();
    d3d12::GpuSpans::get().reset();

    // [SUBMIT TIMELINE 06.11.2026] its GPU objects go with the device (switched on again from the menu)
    if (d3d12::SubmitTimeline::get().running()) {
        REVR_LOG(debug_log("[SUBMIT TIMELINE] stopped by a device reset"));
    }

    d3d12::SubmitTimeline::get().reset();
    m_ctest_started_timeline = false;
    m_gpu_timing_user = false;

    if (m_bench_running) {
        bench_cancel("device reset");
    }

    if (m_ptest_running) {
        ptest_cancel("device reset");
    }

    if (m_vtest_running) {
        vtest_cancel("device reset");
    }

    if (m_stest_running) {
        m_stest_request.store(3); // cancelled in the next update_frame (the game thread: the engine's switch is put back there)
    }

    if (m_ftest_running) {
        m_ftest_request.store(3); // the same: the effects are switched back on in update_frame
    }

    if (m_rtest_running) {
        m_rtest_request.store(3); // the same: the settings are put back in update_frame
    }

    if (m_ctest_running) {
        m_ctest_request.store(3); // the same: the shadows and the fovea views are put back in update_frame
    }

    REVR_LOG(debug_log("device reset"));
#endif
}

void FoveatedRendering::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    // [ALWAYS ON fix91 19.11.2026] the mode Off is gone: a configuration saved with it starts with Fixed
    if (m_mode->value() != (int32_t)Mode::DYNAMIC) {
        m_mode->value() = (int32_t)Mode::FIXED;
    }

    // [UPSCALING MENU fix91 19.11.2026] the supersampling TAA is the only anti-aliasing offered: the fovea's always, the
    // periphery's with the image of the wide view's size (1) or of twice that (2)
    m_fovea_aa->value() = 2;

    // ([CALM fix100 20.11.2026] ... or the calm light TAA (3))
    if (m_periphery_aa->value() != 1 && m_periphery_aa->value() != 3) {
        m_periphery_aa->value() = 2;
    }

#if REVR_RELEASE
    // [RELEASE SWITCH fix139 23.11.2026] the options the release has no entry for: fixed at their defaults (each game's),
    // whatever a configuration of an earlier build holds. Diagnostics: off (the measurements are for the debug build).
    {
        const auto pin = [](auto& opt) { opt->value() = opt->default_value(); };
        m_preset->value() = PRESET_CUSTOM; // (no preset list: the sizes are the ones set by hand)
        pin(m_brightness_match);
        pin(m_tone_match);
        pin(m_head_view_logic);
        pin(m_mono_periphery);
        pin(m_debug_dred);
        m_log_periodic->value() = false;
        m_diag_off->value() = true;
    }
#endif

    m_applied_fraction = m_fraction->value();
    m_applied_ratio = m_ratio->value();
    m_options_dirty = true;
}

void FoveatedRendering::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

#if !REVR_RELEASE // [RELEASE SWITCH fix139] no preset list in the release
void FoveatedRendering::apply_preset(int index) {
    if (index < 0 || index >= PRESET_COUNT) {
        return;
    }

    m_fraction->value() = PRESETS[index].fraction;
    m_ratio->value() = PRESETS[index].ratio;
    m_last_size_edit = Clock::now();
    m_size_edit_pending = true;
}
#endif

void FoveatedRendering::push_gaze_options() {
    EyeGaze::Options o{};
    o.invert_x = m_invert_x->value();
    o.invert_y = m_invert_y->value();
    o.offset_x = m_offset_x->value();
    o.offset_y = m_offset_y->value();
    o.smoothing_hz = m_smoothing->value();
    o.declare_sdk_version = m_declare_sdk->value();

    EyeGaze::get().set_options(o);
    m_options_dirty = false;
}

quadview::Settings FoveatedRendering::current_settings() const {
    quadview::Settings s{};
    s.fraction = m_applied_fraction;
    s.fovea_ratio = m_applied_ratio;
    // ([NO PLUGIN fix91 19.11.2026] s.upscaler, the plugin's method, keeps its starting value: nothing reads it)
    s.taa_feedback = m_taa_feedback->value();
    s.periphery_aa = std::clamp((int)m_periphery_aa->value(), 0, 3); // [PERIPHERY SUPER fix80 17.11.2026] [CALM fix100] 3
    s.periphery_stability = std::clamp(m_periphery_stability->value(), 2.0f, 16.0f); // [PERIPHERY fix82 17.11.2026]
    s.periphery_history_sharpness = std::clamp(m_periphery_hist_sharp->value(), 0.0f, 1.0f); // [PERIPHERY fix83 17.11.2026]
    s.soft_outlines = m_soft_outlines;             // [OUTLINES fix84 18.11.2026]
    s.ui_steady = m_ui_steady;                     // [MENUS fix86 18.11.2026]
    s.ui_steady_around = m_ui_around->value();     // [fix94 19.11.2026]
    s.dump_periphery_full = m_dump_periphery_full; // [PERIPHERY DUMP fix84 18.11.2026]
    // [GUARDS fix91 19.11.2026] always the mod's anti-aliasing (the upscaler plugin in the fovea, and the two guards that made
    // it give way to the mod's TAA, are gone)
    s.fovea_mode = quadview::FoveaMode::TAA;
    // [SUPER TAA 16.11.2026] the third choice: the mod's supersampling TAA (the plugin's guards above keep giving way to the
    // mod's plain TAA, the light one)
    s.fovea_super = m_fovea_aa->value() == 2;
    s.super_taa.scale = m_staa_scale->value() == 0 ? 1 : 2;

    // [GPU GUARD fix91 19.11.2026] while the GPU is short of time (gpu_guard_watch): the lighter supersampling TAA, in the
    // fovea and in the periphery with the same settings, so in the same frame. Both are choices the menu offers and both
    // switch without a set-up (the histories start again).
    // [fix96 19.11.2026] in steps: the periphery's image first (0.45 ms of GPU time, RE4 tests of 19.11.2026), the fovea's
    // history (0.2 ms) only on the second step, if frames still come late
    // [GUARD fix101 20.11.2026] the steps taken, in gpu_guard_watch's order (m_gpu_steps). Each is a choice that switches
    // without a set-up and without starting a history again (the supersampling TAA keeps its history between its two
    // image sizes, the calm light TAA starts from the supersampling TAA's image, and before the step back from it the
    // supersampling TAA runs behind it: periphery_warm_super). Only the fovea's history, the last step, starts again.
    for (int i = 0; i < m_gpu_level_shown && i < m_gpu_step_count; ++i) {
        switch (m_gpu_steps[i]) {
        case GS_OUTLINES: s.soft_outlines = false; break;
        case GS_PERIPHERY_1X:
            if (s.periphery_aa == 2) {
                s.periphery_aa = 1;
            }
            break;
        case GS_CALM:
            if (s.periphery_aa == 1 || s.periphery_aa == 2) {
                s.periphery_aa = 3;
            }
            break;
        case GS_FOVEA_1X: s.super_taa.scale = 1; break;
        default: break;
        }
    }

    s.periphery_warm_super = m_gpu_level_shown > m_gpu_level;
    s.guard_warm = m_gpu_guard_opt->value();
    s.super_taa.stability = m_staa_stability->value();
    s.super_taa.history_sharpness = m_staa_hist_sharp->value();
    s.super_taa.sharpen = m_staa_sharpen->value();
    s.super_taa.follow = m_staa_follow->value();
    s.super_taa.follow_threshold = m_staa_follow_thr->value();
    s.super_taa.follow_memory = m_staa_follow_mem->value();
    s.super_taa.protect = m_staa_protect->value();
    s.super_taa.protect_range = m_staa_protect_range->value();
    s.super_taa.protect_spark = m_staa_protect_spark->value();
    s.super_taa.protect_memory = m_staa_protect_mem->value();
    s.super_taa.arrive = m_staa_arrive->value();
    s.super_taa.filter_scale = m_staa_filter->value();
    s.super_taa.clip_gamma = m_staa_clip->value();
    s.super_taa.motion_reduce = m_staa_motion->value();
    s.super_taa.depth_reject = m_staa_depth->value();
    s.super_taa.luma_weight = m_staa_luma->value();
    s.super_taa.lighter = m_staa_lighter->value();
    s.super_taa.lanczos = m_staa_lanczos->value();
    s.super_taa.debug_view = m_staa_debug_view;
    s.super_taa.split = m_staa_split;
    s.super_taa.compare = m_staa_split;
    s.fovea_taa.feedback = m_ftaa_feedback->value();
    s.fovea_taa.clip_gamma = m_ftaa_clip->value();
    s.fovea_taa.filter_radius = m_ftaa_filter->value();
    s.fovea_taa.new_softness = m_ftaa_new_soft->value();
    s.fovea_taa.motion_reduce = m_ftaa_motion->value();
    s.fovea_taa.depth_reject = m_ftaa_depth->value();
    s.fovea_taa.sharpen = m_ftaa_sharpen->value();
    s.fovea_taa.dilate = m_ftaa_dilate->value();
    s.fovea_taa.catmull_rom = m_ftaa_catmull->value();
    s.fovea_taa.luma_weight = m_ftaa_luma->value();
    s.fovea_taa.fast_converge = m_ftaa_fast->value();
    // [FOVEA OWN fix91 19.11.2026] fsr_direct, fsr_one_pass, fsr_auto_exposure, fsr_model, fovea_supersample: their options are
    // gone (no upscaler is set up for the fovea); the fields keep their start values
    s.brightness_match = m_brightness_match->value();
    s.tone_match = m_tone_match->value();
    s.periphery_light = m_periphery_light->value();

    // [MENU LIGHT 15.11.2026] paused while a game menu is drawn (and 0.3 s after), see quadview::State::menu_gui_ns
    // [MENU LIGHT 15.11.2026 evening] ... or drawn in one of the last 24 frames: RE7 dumps 08 / 09 of 15.11.2026 19:4x (pause
    // menu): the dump held the first frame for about a second, the 0.3 s ran out, the log said "game menu closed" and the
    // light came back for the second frame (the periphery's scene blurred over the menu's black panel), then "open" again.
    // Frames held on purpose or by a hitch do not end a menu now; a menu that really closes ends after 24 frames and 0.3 s.
    {
        const auto& qs = quadview::State::get();
        const int64_t seen = qs.menu_gui_ns.load();
        const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        const int32_t frames_since = VR::get()->get_render_frame_count() - qs.menu_gui_frame.load();
        const bool menu_open =
            seen != 0 && (now - seen < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(300)).count() ||
                             (frames_since >= 0 && frames_since < 24));

        if (menu_open != m_menu_light_paused) {
            m_menu_light_paused = menu_open;
            REVR_LOG(const_cast<FoveatedRendering*>(this)->debug_log(
                std::format("[MENU LIGHT] game menu {}: the large-scale light {}", menu_open ? "open" : "closed", menu_open ? "paused" : "back")));
        }

        if (menu_open) {
            s.periphery_light = false;
        }

        // [MENU BACKDROP 15.11.2026] RE7 test of 15.11.2026: with the light paused the fovea had the right brightness in the
        // inventory, but the menu's backdrop the fovea view draws past the menu's frame ended at the fovea's edges (lighter
        // edges, flickering as the gaze moved)
        s.menu_backdrop = menu_open && m_menu_backdrop;
        s.fovea_backdrop = m_fovea_backdrop; // [BACKDROP fix83 17.11.2026] (any interface, also menus the mod does not know by name)
    }

    // [DEFAULTS 15.11.2026] the user's choice of 14.11.2026 (tests of fix49): the fovea views with the game camera's real
    // exposure (rates 0) from the start, without opening the menu
    if (!m_tone_defaults_sent && m_tt_real_ev != 0) {
        m_tone_defaults_sent = true;
        vrmod::CameraDuplicator::ToneTest tt{};
        tt.local_exposure = m_tt_local;
        tt.copy_auto = m_tt_copy_auto;
        tt.ev_lock = m_tt_ev_lock;
        tt.ev_offset = m_tt_ev_offset;
        tt.ev_follow = m_tt_ev_follow;
        tt.clone_adapt = m_tt_clone_adapt;
        tt.real_ev = m_tt_real_ev;
        tt.real_ev_steady = m_tt_real_ev_steady;
        tt.auto_value = -1;
        VR::get()->get_camera_duplicator().set_tone_test(tt);
        REVR_LOG(const_cast<FoveatedRendering*>(this)->debug_log(std::format("[EXPOSURE TEST] defaults: {}", tone_test_text())));
    }
    s.light_px = m_light_px->value();
    s.light_limit = m_tt_light_limit;       // [GLOW 13.11.2026]
    s.light_add_only = m_tt_light_add_only;
    s.light_auto = m_tt_light_auto;         // [AUTO LIGHT 14.11.2026]
    // [VIEW DIFFERENCE 01.11.2026] measured during the effect test and when asked in the menu
    s.view_metric = m_ftest_running || m_log_view_diff || m_tt_ev_follow || m_tt_real_ev != 0; // [EV FOLLOW / REAL EV 13.11.2026] the tests read it
    s.feather = m_feather->value();
    s.deadzone = m_deadzone->value();
    s.debug_border = m_debug_border;
    s.debug_view = m_debug_view;
    s.mono_periphery = m_mono_periphery->value();
    quadview::State::get().head_view_mode = std::clamp(m_head_view_logic->value(), 0, 2); // [VIEW RACE 03.11.2026]
    quadview::State::get().fovea_ray_tracing = m_fovea_ray_tracing->value();               // [FOVEA RAY TRACING 12.11.2026]
    s.hole_fill = m_hole_fill->value();
    s.stable_depth = m_stable_depth->value(); // [STABLE DEPTH fix81 17.11.2026]
    s.wide_first = m_wide_first->value();     // [LIGHTS fix85 18.11.2026]
    quadview::State::get().wide_first.store(s.wide_first, std::memory_order_relaxed);
    s.steady_interface = m_steady_interface->value(); // [MENUS fix81 17.11.2026]
    quadview::State::get().fovea_whole_infos.store((uint32_t)std::clamp(m_fovea_whole_infos, 0, 63), std::memory_order_relaxed); // [LIGHTS fix83 17.11.2026]
    s.item_beams = m_item_beams->value();
    s.desktop_mirror = m_desktop_mirror->value();
    s.ui_motion = m_ui_motion->value(); // [UI MOTION fix125 22.11.2026]
    s.direct_8bit = m_direct_8bit->value();
    s.separate_eye_passes = m_separate_eye_passes;
    s.hole_band = m_hole_band; // [HOLE BAND 13.11.2026]

    // [GPU TEST 29.10.2026] the variant of the running GPU pass test, else the one chosen by hand. The test also fixes
    // what decides the measured path: the single eye pass (except in its own variant) and the normal view.
    const bool in_test = m_ptest_running && m_ptest_phase >= 0 && m_ptest_phase < PTEST_PHASES;
    // ([fix108] s.test_prepare = m_ptest_running compiled the test's own texts ahead; [fix111] no text of the test is left
    // that the start does not prepare: the one with the loop cost 0.63 ms more and is not measured again)

    if (in_test) {
        s.separate_eye_passes = false;
        s.debug_view = 0;
    }

    // [PERIPHERY SUPER fix80 17.11.2026] the GPU pass test and a variant chosen by hand compare the periphery's passes as
    // they were made for (the light TAA, the reprojection's searches, the eye pass's group sizes at the wide view's
    // density): the light TAA in the periphery meanwhile
    // [fix94 19.11.2026] not for the variants of today's chain (32 on)
    const int test_variant = in_test ? PTEST_ORDER[m_ptest_phase] : m_test_variant;

    if (test_variant >= 1 && test_variant < PTEST_REF) {
        s.periphery_aa = 0;
    }


    // [ENGINE VIEWS 30.10.2026] the view cost test: the phase's render size and number of views from the sizes taken at
    // its start (the menu cannot change them during the test); the fovea with the mod's TAA (in the mod's own passes,
    // so it is not counted as the engine's)
    if (m_vtest_running && m_vtest_phase >= 0 && m_vtest_phase < VTEST_PHASE_COUNT) {
        const auto& ph = VTEST_PHASES[m_vtest_phase];
        s.separate_eye_passes = false;
        s.debug_view = 0;
        s.fraction = m_vtest_fraction;
        s.fovea_ratio = std::clamp(m_vtest_ratio * ph.ratio_scale, 1.0f, 3.0f);
        s.mono_periphery = !ph.four_views;
        s.fovea_mode = quadview::FoveaMode::TAA;
        s.fovea_super = false; // [SUPER TAA 16.11.2026] the test's phases with the mod's plain TAA, as before
        s.periphery_aa = 0;    // [PERIPHERY SUPER fix80 17.11.2026] ... and the light TAA in the periphery
        return s;
    }

    switch (in_test ? PTEST_ORDER[m_ptest_phase] : m_test_variant) {
    case 1: s.test_original_reprojection = true; break;
    case 2: s.test_no_tile_skip = true; break;
    case 3: s.test_no_gui_map = true; break;
    case 4: s.separate_eye_passes = true; break;
    case 5: s.test_no_interface = true; break;
    case 6: s.test_no_post = true; break;
    case 7: s.test_round2_passes = true; break;
    case 8: s.direct_8bit = false; break;
    case 9: s.test_eye_variant = 1; break;
    case 10: s.test_eye_variant = 2; break;
    case 11: s.test_eye_variant = 3; break;
    case 12: s.test_eye_variant = 4; break;
    case 13: s.test_eye_variant = 5; break;
    case 14: s.test_eye_variant = 6; break;
    case 15: s.test_fovea_taa_32 = true; break;
    case 16: s.test_light_taa_32 = true; break;
    case 17: s.test_eye_variant = 7; break;
    case 18: s.test_eye_variant = 8; break;
    case 19: s.test_eye_variant = 9; break;
    case 20: s.test_eye_variant = 10; break;
    case 21: s.test_bmin_texture = true; break;
    case 22: s.test_eye_variant = 11; break;
    case 23: s.test_halo_serial = true; break;
    case 24: s.test_separate_pre = true; break;
    case 25: s.tone_match = false; break;
    case 26: s.periphery_light = false; break;
    case 27: s.tone_match = false; s.periphery_light = false; break;
    case 28: s.hole_fill = false; break;
    case 29: s.item_beams = false; break;
    case 30: s.upscaler_sharpen = 0.0f; break;
    case 31: s.test_no_post_sharpen = true; break;
    case 33: s.ui_steady = false; break;
    case 34: s.ui_steady_around = true; break;
    case 35: s.soft_outlines = false; break;
    case 36:
        if (s.periphery_aa >= 2) { // ([CALM fix100] also from the calm light TAA)
            s.periphery_aa = 1;
        }
        break;
    case 37: s.super_taa.scale = 1; break;
    case 38: s.test_no_interface = true; break;
    case 39: s.test_no_post = true; break;
    case 40: s.test_no_post_sharpen = true; break;
    case 41: s.periphery_light = false; break;
    case 42: s.tone_match = false; s.periphery_light = false; break;
    case 43: s.periphery_aa = 3; break;
    case 44: s.fovea_backdrop = false; break;
    case 45: s.test_no_tile_skip = true; break;
    case 46: s.test_tile_in_pass = true; break; // [TILE PASS fix105]
    case 47: s.test_tile_none = true; break;    // [fix106]
    case 48: s.test_ui_calls = 1; break;        // [fix108]
    case 49: s.test_ui_calls = 2; break;
    case 50: s.test_one_pass = true; break;     // [TWO PASSES fix111]
    case 51: s.test_first_skips = true; break; // [fix112, fix113, fix115]
    case 54: s.test_marked_text = 1; break;    // [MARKED TEXT fix115]
    case 55: s.test_marked_text = 0; break;
    case 56: s.test_marked_text = 3; break;
    case 57: s.test_ui_copies = 1; break; // [COPIES fix116]
    case 58: s.test_ui_copies = 1; s.test_ui_idle = true; break;
    case 59: s.test_marked_small = true; s.test_marked_over = false; break; // [SMALL GROUPS fix117]
    case 60: s.test_marked_text = 4; break;
    case 61: s.test_clean_no_map = true; break; // [NO INTERFACE fix118]
    case 62: s.test_spans_ends_only = true; break; // [ENDS ONLY fix119]
    case 63: s.test_spans_ends_only = true; s.test_ui_calls = 1; break;
    case 64: s.test_marked_text = 5; break; // [DIRECT fix120]
    case 65: s.test_marked_over = false; break; // [OVER fix121; fix122: the build's way, this is the way before]
    case 66: s.test_no_ui_motion = true; break; // [UI MOTION fix125]
    default: break;
    }

    // [COPIES fix116 21.11.2026] (VR::ui_capture reads it)
    quadview::State::get().test_ui_copies.store(s.test_ui_copies, std::memory_order_relaxed);
    d3d12::GpuSpans::get().set_ends_only(s.test_spans_ends_only); // [ENDS ONLY fix119]
    quadview::State::get().small_window.store(m_small_window->value(), std::memory_order_relaxed); // [SMALL WINDOW fix122]

    return s;
}

// [SUPER TAA 16.11.2026] the menu of the mod's supersampling TAA (the third choice of "Fovea anti-aliasing"): its settings
// and its debug tools
// [RELEASE TEXTS fix136 23.11.2026] The release menus (RE4, then RE7 and RE2): short, plain names and tooltips that
// anyone understands, the same in every game. QV_TIP: a tooltip on the item just drawn.
#define QV_TIP(text) (ImGui::IsItemHovered() ? menu_text::hint_tooltip("%s", text) : (void)0)

bool FoveatedRendering::draw_super_taa_settings() {
    bool changed = false;
    auto& qv = quadview::State::get();
    auto up = TemporalUpscaler::get();

    g_framework->draw_menu_heading("Sharp area anti-aliasing: fine settings", true);

    if (up->super_taa_failed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.75f, 0.25f, 1.0f});
        ImGui::TextWrapped("%s", "The anti-aliasing of the sharp area had a problem and uses a simpler method for now. Changing "
                                      "the sharp area size starts it again.");
        ImGui::PopStyleColor();
    }

    changed |= m_staa_stability->draw("Steadiness (higher = calmer, slower to react)");
    QV_TIP("How calm the image is. Higher: less shimmer, but changes take a moment longer to show.");
    changed |= m_staa_hist_sharp->draw("Sharpness (0 = softer)");
    QV_TIP("Higher: a sharper picture, but a little more shimmer.");
    changed |= m_staa_sharpen->draw("Extra sharpening (0 = off)");
    QV_TIP("Sharpens the finished image of the sharp area. 0 = off.");
    g_framework->push_menu_toggle_style();
    changed |= m_staa_protect->draw("Keep fine details steady");

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("%s", "Thin edges, small highlights and soft shadows stay still instead of flickering.");
    }

    changed |= m_staa_follow->draw("React quickly to changes (no trails)");
    g_framework->pop_menu_toggle_style();

    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("%s", "Lights, screens and shadows that change show up at once, without leaving trails.");
    }

    if (ImGui::TreeNode("More anti-aliasing settings")) {
        if (m_staa_follow->value()) {
            changed |= m_staa_follow_thr->draw("When something counts as a change (lower = reacts sooner)");
            changed |= m_staa_follow_mem->draw("Softness of changes (0 = instant)");
        }

        if (m_staa_protect->value()) {
            changed |= m_staa_protect_range->draw("Fine details: how much is kept (higher = steadier)");
            changed |= m_staa_protect_spark->draw("Fine details: limit for sparkles (lower = calmer; 0 = off)");
            changed |= m_staa_protect_mem->draw("Fine details: memory (1 = none)");
            g_framework->push_menu_toggle_style();
            changed |= m_staa_arrive->draw("Fine details: no trails behind fast small things (flies)");
            g_framework->pop_menu_toggle_style();

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("%s", "Small things that move fast, like flies, leave no dotted trail behind.");
            }
        }

        changed |= m_staa_filter->draw("Sharpness of each new frame (lower = sharper, more shimmer)");
        changed |= m_staa_clip->draw("Trail limit (lower = fewer trails, more shimmer)");
        changed |= m_staa_motion->draw("Less smoothing on fast movement");
        changed |= m_staa_depth->draw("Forget what was behind moving things (0 = off)");
        g_framework->push_menu_toggle_style();
        changed |= m_staa_luma->draw("Fewer flickering sparkles");
        changed |= m_staa_lighter->draw("Lighter version (a little softer, faster)");

        if (m_staa_scale->value() != 0) {
            changed |= m_staa_lanczos->draw("2x: sharper downsizing (more shimmer, slower)");
        }

        g_framework->pop_menu_toggle_style();

        if (ImGui::Button("Recommended values##staa")) {
            reset_super_taa();
            changed = true;
        }

        QV_TIP("Puts every setting of this list back to the recommended value.");
        ImGui::TreePop();
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix139]
    if (ImGui::TreeNode("Supersampling TAA: debug tools")) {
        static const char* views[]{"Off", "Steady detail kept (green), let go (red), no history (blue), jumps (white)", "Noise the history averages (yellow)",
            "History weight (white = settled)", "Motion (red = up to 8 px per frame)", "Scatter of the samples (what steady detail goes by)"};
        ImGui::SetNextItemWidth(420.0f);

        if (menu_text::Combo("Debug view (not saved)##staa", &m_staa_debug_view, views, (int)std::size(views))) {
            changed = true;
        }

        g_framework->push_menu_toggle_style();

        if (ImGui::Checkbox("Compare: left half of the fovea = TAA of the mod (not saved)", &m_staa_split)) {
            changed = true;
        }

        g_framework->pop_menu_toggle_style();
        menu_text::hint_text("The comparison runs both anti-aliasings on the same frames (the mod's TAA costs its GPU time on top); a yellow "
                            "line divides them.");

        static const char* frame_counts[]{"16 frames", "32 frames (one whole jitter cycle of 2x)", "64 frames (a change and what follows it)"};
        static const char* parts[]{"192 px", "256 px", "384 px"};
        ImGui::SetNextItemWidth(300.0f);
        menu_text::Combo("Capture length##staa", &m_staa_capture_frames, frame_counts, (int)std::size(frame_counts));
        ImGui::SetNextItemWidth(300.0f);
        menu_text::Combo("Captured part (square in the middle of the fovea)##staa", &m_staa_capture_part, parts, (int)std::size(parts));

        if (qv.super_capture_running.load()) {
            ImGui::TextUnformatted("Capturing...");
        } else if (m_staa_capture_at.time_since_epoch().count() == 0) {
            if (ImGui::Button("Capture in 4 s (look at the spot that flickers)")) {
                if (qv.engaged()) {
                    m_staa_capture_at = Clock::now() + std::chrono::seconds(STAA_CAPTURE_DELAY_SECONDS);
                    REVR_LOG(set_message("Capture in 4 seconds: look at the spot to capture and keep looking for 2 seconds."));
                } else {
                    REVR_LOG(set_message("The tri-view is not active: nothing to capture."));
                }
            }
        } else {
            const double left = std::chrono::duration<double>(m_staa_capture_at - Clock::now()).count();
            ImGui::Text("Capture in %.1f s", left > 0.0 ? left : 0.0);
        }

        menu_text::hint_text("Capture: consecutive frames of the middle of the left fovea, exactly as stored: what the pass got (colour, depth, "
                            "motion, jitter), what it made of it, its history and statistics, and the mod's TAA's result of the same "
                            "frames, and the finished eye image there. Also the periphery's chain, taken just beside the fovea rectangle "
                            "(left of it when there is room; the same place for the whole capture): the wide view, the distances, the "
                            "eye's periphery and the finished eye image there. For something that flickers in the periphery: look so "
                            "that it lies just left of the fovea. It works in every debug view and with every anti-aliasing (for a "
                            "menu: open it before pressing). "
                            "Written to quadview_dump/supertaa_captureNN (25 to 400 MB) with first numbers for detail and flicker "
                            "in capture.txt and in foveation_debug.log. The frames are replayed and measured outside the game. For "
                            "something that trails or stays behind: 64 frames, started while it happens.");

        if (ImGui::Button("Write the engine's buffers (which images the game has: G-buffer, image before tone mapping)")) {
            if (qv.engaged()) {
                qv.engine_probe_request = true;
                REVR_LOG(set_message("Engine buffers: reading the next frame..."));
            } else {
                REVR_LOG(set_message("The tri-view is not active: nothing to read."));
            }
        }

        menu_text::hint_text("Lists the render targets of the left fovea view and writes each one once (quadview_dump/engine_probeNN, the "
                            "list also in foveation_debug.log): what the engine could give the anti-aliasing besides colour, depth and "
                            "motion (normals, roughness, the image in linear light).");

        ImGui::Text("Passes so far: %llu  |  frames on the mod's TAA instead: %llu", (unsigned long long)qv.super_dispatches.load(),
            (unsigned long long)qv.super_fallbacks.load());
        ImGui::TreePop();
    }
#endif

    return changed;
}

// [FOVEA TAA 28.10.2026] the recommended values (quadview::FoveaTaaSettings defaults)
void FoveatedRendering::reset_fovea_taa() {
    const quadview::FoveaTaaSettings d{};
    m_ftaa_feedback->value() = d.feedback;
    m_ftaa_clip->value() = d.clip_gamma;
    m_ftaa_filter->value() = d.filter_radius;
    m_ftaa_new_soft->value() = d.new_softness;
    m_ftaa_motion->value() = d.motion_reduce;
    m_ftaa_depth->value() = d.depth_reject;
    m_ftaa_sharpen->value() = d.sharpen;
    m_ftaa_dilate->value() = d.dilate;
    m_ftaa_catmull->value() = d.catmull_rom;
    m_ftaa_luma->value() = d.luma_weight;
    m_ftaa_fast->value() = d.fast_converge;
}

// [SUPER TAA 16.11.2026] the recommended values (quadview::SuperTaaSettings defaults)
void FoveatedRendering::reset_super_taa() {
    const quadview::SuperTaaSettings d{};
    m_staa_scale->value() = d.scale >= 2 ? 1 : 0;
    m_staa_stability->value() = d.stability;
    m_staa_hist_sharp->value() = d.history_sharpness;
    m_staa_sharpen->value() = d.sharpen;
    m_staa_follow->value() = d.follow;
    m_staa_follow_thr->value() = d.follow_threshold;
    m_staa_follow_mem->value() = d.follow_memory;
    m_staa_protect->value() = d.protect;
    m_staa_protect_range->value() = d.protect_range;
    m_staa_protect_spark->value() = d.protect_spark;
    m_staa_protect_mem->value() = d.protect_memory;
    m_staa_arrive->value() = d.arrive;
    m_staa_filter->value() = d.filter_scale;
    m_staa_clip->value() = d.clip_gamma;
    m_staa_motion->value() = d.motion_reduce;
    m_staa_depth->value() = d.depth_reject;
    m_staa_luma->value() = d.luma_weight;
    m_staa_lighter->value() = d.lighter;
    m_staa_lanczos->value() = d.lanczos;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
FoveatedRendering::Counters FoveatedRendering::read_counters() const {
    auto& qv = quadview::State::get();
    Counters c{};
    c.wide = qv.wide_layers.load();
    c.inset = qv.inset_layers.load();
    c.by_scope = qv.inset_by_scope.load();
    c.by_camera = qv.inset_by_camera.load();
    c.gui = qv.gui_inset.load();
    c.evaluations = qv.evaluations.load();
    c.skipped = qv.skipped.load();
    c.moves = qv.rect_moves();
    return c;
}
#endif

// =================================================================================================
// Per frame
// =================================================================================================
void FoveatedRendering::update_frame() {
    auto vr = VR::get();
    auto up = TemporalUpscaler::get();
    auto& gaze = EyeGaze::get();
    auto& qv = quadview::State::get();

    // CPU frame time = interval between two BeginRendering calls
    const auto now = Clock::now();
#if !REVR_RELEASE // [RELEASE SWITCH fix141] frame times for the tests and the status line: debug build only
    double dt_ms = 0.0;

    if (m_last_begin.time_since_epoch().count() != 0) {
        dt_ms = std::chrono::duration<double, std::milli>(now - m_last_begin).count();
        m_cpu_frame_ms = dt_ms;
        // [CPU 28.10.2026] frame time histogram of the periodic interval (quarter milliseconds)
        ++m_frame_hist[(size_t)std::clamp((int)(dt_ms * 4.0), 0, (int)m_frame_hist.size() - 1)];
        m_frame_max_ms = (std::max)(m_frame_max_ms, dt_ms);
    }

    m_last_begin = now;
    m_begin_rendering = now;
#endif

    if (m_options_dirty) {
        push_gaze_options();
    }

    // apply the size sliders once they have been still for a moment
    if (m_size_edit_pending && std::chrono::duration<double>(now - m_last_size_edit).count() >= SIZE_EDIT_DELAY_SECONDS) {
        m_size_edit_pending = false;
        m_applied_fraction = m_fraction->value();
        m_applied_ratio = m_ratio->value();
    }

    const Mode mode_now = mode();
    const bool hmd = vr->is_hmd_active();

    m_zoomed = std::fabs(vr->get_projection_zoom() - 1.0f) > 1e-3f || vr->get_projection_fov() > 0.0f;

    // [STEREO FOVEA 27.10.2026] Single Frame Multipass only: both eyes every frame, no frame warp
    const bool technique_ok = vr->is_using_multipass();
    bool want = hmd && technique_ok && up->activated() && up->is_d3d12();
#if !defined(RE4)
    // [MENU CANVAS 12.11.2026] no quad view while the flat canvas shows the monitor frame
    want = want && !VR::get()->is_flatscreen_overlay();
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the GPU timer, the tests, the engine reports and the debug options' switches
    // GPU time in the periodic status line without having to tick the checkbox. The D3D12 hook learns the command
    // queue only once the game has executed command lists, so an early attempt fails: retried every 3 seconds.
    // [fix94 19.11.2026] "Diagnostics off": no timestamps either (the GPU guard reads the mod's GPU time from them: kept
    // while it is on; a test switches them on for itself and back to m_gpu_timing_user at its end)
    // ([fix96] the GPU guard no longer needs them: it reads SteamVR's measure)
    const bool no_gpu_timing = m_diag_off->value();

    if (no_gpu_timing && !any_test_running() && (m_gpu_timer.enabled() || m_gpu_timing_user)) {
        gpu_timer_enable(false);
        m_gpu_timing_user = false;
        REVR_LOG(spdlog::info("[FoveatedRendering] diagnostics off: GPU timer disabled"));
    }

    if (hmd && !m_gpu_timer.enabled() && !m_gpu_timing_auto_off && !m_bench_running && !no_gpu_timing) {
        const auto now = Clock::now();

        if (now >= m_gpu_timing_auto_next) {
            m_gpu_timing_auto_next = now + std::chrono::seconds(3);

            if (gpu_timer_enable(true)) {
                m_gpu_timing_user = true;
                REVR_LOG(spdlog::info("[FoveatedRendering] GPU timer enabled"));
            }
        }
    }

    if (m_bench_running) {
        if (!hmd || !technique_ok) {
            bench_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            bool force_off = false;
            bench_tick(dt_ms, force_off);
            want = want && !force_off;
        }
    }

    // [GPU TEST 29.10.2026] started / cancelled from the menu, run here (the game thread)
    switch (m_ptest_request.exchange(0)) {
    case 1: ptest_start(); break;
    case 2: ptest_cancel("cancelled by user"); break;
    default: break;
    }

    if (m_ptest_running) {
        if (!hmd || !technique_ok || !vr->is_mono_periphery()) {
            ptest_cancel("the headset is not active, the technique is not Single Frame Multipass or the shared periphery is off");
        } else {
            ptest_tick(dt_ms);
        }
    }

    // [ENGINE VIEWS 30.10.2026] the view cost test and the render layer tree, asked for in the menu
    switch (m_vtest_request.exchange(0)) {
    case 1: vtest_start(); break;
    case 2: vtest_cancel("cancelled by user"); break;
    default: break;
    }

    if (m_vtest_running) {
        if (!hmd || !technique_ok) {
            vtest_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            vtest_tick(dt_ms);
        }
    }

    if (m_layer_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_render_layers()));
        REVR_LOG(set_message("Render layer tree written to foveation_debug.log"));
    }

    // [ENGINE SHADOWS 30.10.2026] the shadow test and the shadow layer details, asked for in the menu
    switch (m_stest_request.exchange(0)) {
    case 1: stest_start(); break;
    case 2: stest_cancel("cancelled by user"); break;
    case 3: stest_cancel("device reset"); break;
    default: break;
    }

    if (m_stest_running) {
        if (!hmd || !technique_ok) {
            stest_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            stest_tick(dt_ms);
        }
    }

    if (m_shadow_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_shadow_types()));
        REVR_LOG(set_message("Shadow layer details written to foveation_debug.log"));
    }

    if (m_ao_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_ao_settings()));
        REVR_LOG(set_message("Ambient occlusion details written to foveation_debug.log"));
    }

    if (m_vrs_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_vrs_settings()));
        REVR_LOG(set_message("Variable rate shading details written to foveation_debug.log"));
    }

    if (m_exposure_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_engine_types(3)));
        REVR_LOG(set_message("Exposure and tone mapping details written to foveation_debug.log"));
    }

    if (m_effects_dump_request.exchange(false)) { // [VIEW EFFECTS 13.11.2026]
        REVR_LOG(debug_log(describe_engine_types(8)));
        REVR_LOG(set_message("The views' effect values written to foveation_debug.log"));
    }

    if (m_colour_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_engine_types(4)));
        REVR_LOG(set_message("Colour correction details written to foveation_debug.log"));
    }

    // [LOD 06.11.2026] the engine's level of detail and culling settings of the views
    if (m_shaft_dump_request.exchange(false)) { // [LIGHT SHAFT 13.11.2026]
        REVR_LOG(debug_log(describe_engine_types(6)));
        REVR_LOG(set_message("Light shaft details written to foveation_debug.log."));
    }

    if (m_lod_dump_request.exchange(false)) {
        REVR_LOG(debug_log(describe_engine_types(5)));
        REVR_LOG(set_message("Level of detail and culling details written to foveation_debug.log"));
    }

    // [SUBMIT TIMELINE 06.11.2026] the GPU timeline of the frame, switched in the menu
    switch (m_st_request.exchange(0)) {
    case 1: submit_timeline_enable(true); break;
    case 2: submit_timeline_enable(false); break;
    default: break;
    }

    // [ENGINE FEATURES 30.10.2026] the effect cost test, asked for in the menu
    switch (m_ftest_request.exchange(0)) {
    case 1: ftest_start(); break;
    case 2: ftest_cancel("cancelled by user"); break;
    case 3: ftest_cancel("device reset"); break;
    default: break;
    }

    if (m_ftest_running) {
        if (!hmd || !technique_ok) {
            ftest_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            ftest_tick(dt_ms);
        }
    }

    // [LIGHT SHAFT OFF 13.11.2026] applied when changed and again every 90 frames while on (new areas, the game switching them on)
    if (const bool asked = m_ls_changed.exchange(false); asked || (m_ls_off && (++m_ls_frames % 90) == 0)) {
        light_shafts_apply(m_ls_off, asked);
    }

    // [RENDER SETTINGS 30.10.2026] the render settings cost test, asked for in the menu
    switch (m_rtest_request.exchange(0)) {
    case 1: rtest_start(); break;
    case 2: rtest_cancel("cancelled by user"); break;
    case 3: rtest_cancel("device reset"); break;
    default: break;
    }

    if (m_rtest_running) {
        if (!hmd || !technique_ok) {
            rtest_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            rtest_tick(dt_ms);
        }
    }

    // [CPU TEST 03.11.2026] the CPU test in a fight, asked for in the menu
    switch (m_ctest_request.exchange(0)) {
    case 1: ctest_start(false); break;
    case 4: ctest_start(true); break; // [GPU TEST 06.11.2026]
    case 2: ctest_cancel("cancelled by user"); break;
    case 3: ctest_cancel("device reset"); break;
    default: break;
    }

    if (m_ctest_running) {
        if (!hmd || !technique_ok) {
            ctest_cancel("the headset is not active or the technique is not Single Frame Multipass");
        } else {
            ctest_tick();
        }
    }

    // [OCCLUSION 15.11.2026] the engine's occlusion culling in the wide view (the GPU test switches it itself)
    if (!m_ctest_running) {
        view_occlusion_tick(hmd && technique_ok && qv.engaged());
    }

    flicker_test_tick(!m_ctest_running && hmd && technique_ok && qv.engaged()); // [FLICKER TEST 15.11.2026]

    // [LIGHT FLICKER TEST 15.11.2026] started from the menu, run here (engine switches on the game thread); then the shadow
    // cache mode it chose or the menu's
    {
        const bool qv_on = !m_ctest_running && hmd && technique_ok && qv.engaged();

        if (m_lft_request.exchange(false)) {
            if (qv_on) {
                lft_start();
            } else {
                REVR_LOG(set_message("Light flicker test: the tri-view has to be engaged (headset on, Mode Fixed or Dynamic)."));
            }
        }

        lft_tick(qv_on);
        shadow_cache_tick(qv_on); // [SHADOW CACHE 15.11.2026]
    }

#endif

    // [AIM COST fix112 20.11.2026] frames of the periodic interval with the right grip held (the gun aimed): the log told
    // the states apart by the weapon HUD's elements, which a knife does not have. OpenVR: a read of the action's state
    // (under OpenXR is_action_active_raw keeps a state of its own for the Index grip: not asked there). The field of view
    // logged by fix111 said nothing: VR::update_camera writes the headset's on every camera in every frame (106.08 in every
    // interval, aiming or not), which also means that the engine does not render with a narrower one while aiming.
#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the status line: debug build only
    if (hmd && vr->get_runtime()->loaded && vr->get_runtime()->is_openvr()) {
        ++m_grip_n;

        if (vr->is_action_active_raw(vr->get_action_grip(), vr->get_right_joystick())) {
            ++m_grip_frames;
        }

    }
#endif

    // [ENGINE SHADOWS 30.10.2026] the engine's own render time, summed for the periodic line and the shadow test's phase
    if (hmd) {
        if (const float rt = engine_render_time_ms(); rt >= 0.0f && rt < 1000.0f) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the status line's and the tests' sums: debug build only
            m_rt_sum += rt;
            ++m_rt_n;
#endif
            m_gg_rt_sum += rt; // [GPU GUARD fix91 19.11.2026]
            ++m_gg_rt_n;
#if !REVR_RELEASE
            if (m_ptest_measuring) { // [HUD BLUR fix114] ... and the GPU test's
                m_ptest_rt_sum += rt;
                ++m_ptest_rt_n;
            }

            if (m_stest_measuring) {
                m_stest_rt_sum += rt;
                ++m_stest_rt_n;
            }

            if (m_ftest_measuring) {
                m_ftest_rt_sum += rt;
                ++m_ftest_rt_n;
            }

            if (m_rtest_measuring) {
                m_rtest_rt_sum += rt;
                ++m_rtest_rt_n;
            }
#endif
        }
    }

    // gaze per eye: dynamic = eye tracker, fixed (or zoomed) = lens centre
    if (hmd) {
        gaze.update(mode_now == Mode::DYNAMIC);

        for (int eye = 0; eye < 2; ++eye) {
            float x = 0.0f;
            float y = 0.0f;

            if (mode_now == Mode::DYNAMIC && !(m_zoomed && m_center_when_zoomed->value())) {
                const auto s = gaze.get_eye(eye);
                x = s.x;
                y = s.y;
            } else {
                const auto c = gaze.lens_center(eye);
                x = c[0];
                y = c[1];
            }

            qv.set_gaze(eye, x, y);
        }
    }

    m_requested = want;
    qv.set_request(want, current_settings());

    // [COLLAPSE 05.11.2026] / [GPU GUARD 05.11.2026] / [MEMORY 05.11.2026]
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the collapse note and the memory sample: the debug log only
    collapse_watch(now, want && qv.engaged());
#endif
    gpu_guard_watch(now, want && qv.engaged());

#if !REVR_RELEASE
    if (hmd && !m_diag_off->value() && (m_mem_at.time_since_epoch().count() == 0 || now - m_mem_at >= std::chrono::seconds(1))) {
        m_mem_at = now;

        if (auto& hook = g_framework->get_d3d12_hook(); g_framework->is_dx12() && hook != nullptr) {
            const auto ms = adapter_memory().sample(hook->get_device());
            m_mem_peak_local = (std::max)(m_mem_peak_local, ms.local_usage);
        }
    }
#endif
    m_overlay_skip.store(m_skip_fovea_ui_update->value() ? 1 : 0, std::memory_order_relaxed); // [FOVEA UI UPDATE 04.11.2026]
    quadview::State::get().real_view_scale.store(m_real_view_scale, std::memory_order_relaxed); // [VIEW SCALE 15.11.2026]
    quadview::State::get().screen_effects_off.store(m_screen_effects_off, std::memory_order_relaxed); // [SCREEN EFFECTS 15.11.2026]
    quadview::State::get().image_quality_hold.store(m_image_quality_hold, std::memory_order_relaxed); // [IMAGE QUALITY 15.11.2026]

#if !REVR_RELEASE // [RELEASE SWITCH fix141] dumps, video, captures, their notes, the GPU timer, the status line: debug build only
    // [DUMP DELAY 24.10.2026] the delayed sequence starts now
    if (m_dump_at.time_since_epoch().count() != 0 && now >= m_dump_at) {
        m_dump_at = {};
        qv.request_dump(DELAYED_DUMP_FRAMES);
        m_cw_quiet_until = now + std::chrono::seconds(15); // [COLLAPSE 05.11.2026] a dump holds frames on purpose
        m_dump_snapshot_written = false;
        REVR_LOG(set_message(std::format("Dumping {} frames...", DELAYED_DUMP_FRAMES)));
    }

    // [VIDEO 25.10.2026] the delayed recording starts now (D3D12Component takes the request at the next submit)
    if (m_video_at.time_since_epoch().count() != 0 && now >= m_video_at) {
        m_video_at = {};
        qv.request_video(VIDEO_SECONDS);
        m_cw_quiet_until = now + std::chrono::seconds((int)VIDEO_SECONDS + 10); // [COLLAPSE 05.11.2026]
    }

    // [SUPER TAA 16.11.2026] the delayed capture of the supersampling TAA starts now (TemporalUpscaler takes the request at
    // the next present, lets the mod's TAA run beside it for 32 frames and then records)
    if (m_staa_capture_at.time_since_epoch().count() != 0 && now >= m_staa_capture_at) {
        m_staa_capture_at = {};
        qv.super_capture_frames = m_staa_capture_frames == 2 ? 64 : (m_staa_capture_frames == 1 ? 32 : 16);
        qv.super_capture_part = m_staa_capture_part == 0 ? 192 : (m_staa_capture_part == 2 ? 384 : 256);
        qv.super_capture_request = true;
        m_cw_quiet_until = now + std::chrono::seconds(15); // (the capture's write holds a frame on purpose)
        REVR_LOG(debug_log(std::format("[SUPER TAA CAPTURE] asked for: {} frames, part {} px", qv.super_capture_frames.load(), qv.super_capture_part.load())));
        REVR_LOG(set_message("Capturing the supersampling TAA..."));
    }

    // [SUPER TAA 16.11.2026] text from the render side (the capture's numbers, the engine buffer probe) -> into the log
    if (auto notes = qv.pop_notes(); !notes.empty()) {
        for (const auto& [text, message] : notes) {
            REVR_LOG(debug_log(text));

            if (!message.empty()) {
                REVR_LOG(set_message(message));
            }
        }
    }

    // [SUPER TAA 16.11.2026] one log line when the fovea's anti-aliasing really changes to or from the supersampling TAA
    // (chosen and its passes run / chosen but the mod's TAA runs because a pass failed), so a log shows which one made
    // the image
    {
        const uint64_t passes = qv.super_dispatches.load(std::memory_order_relaxed);
        const uint64_t fallbacks = qv.super_fallbacks.load(std::memory_order_relaxed);
        const int state = m_fovea_aa->value() != 2 ? 0 : (fallbacks != m_staa_logged_fallbacks ? 2 : (passes != m_staa_logged_dispatches ? 1 : m_staa_logged_state));
        m_staa_logged_dispatches = passes;
        m_staa_logged_fallbacks = fallbacks;

        if (state != m_staa_logged_state && state >= 0) {
            m_staa_logged_state = state;
            REVR_LOG(debug_log(std::format("[SUPER TAA] {}", state == 1   ? "the supersampling TAA anti-aliases the fovea"
                                                    : state == 2 ? "chosen, but the mod's TAA runs instead: a pass failed (see the framework log)"
                                                                 : "not the fovea's anti-aliasing")));
        }
    }

    // pieces of a frame dump finished on the render thread / in the VR submit -> into the log
    if (auto reports = qv.pop_dump_reports(); !reports.empty()) {
        std::string text{};

        if (!m_dump_snapshot_written) {
            text = build_snapshot("frame dump") + "  FRAME DUMP (images in the quadview_dump folder next to this log):\n";
            m_dump_snapshot_written = true;
        }

        for (const auto& r : reports) {
            text += r;
        }

        REVR_LOG(debug_log(text));
        REVR_LOG(set_message("Frame images written to the quadview_dump folder (see foveation_debug.log)."));
    }

    // [CPU 04.11.2026] the timer's start is a command list sent to the GPU queue from here (reset, record, ExecuteCommandLists,
    // and reading back a finished pair), ~10-30 us of the main thread at BeginRendering, which lies on the path from one
    // frame to the next. Outside the tests (they compare phases by it) one frame in 8 is enough for the status line's
    // average: the frames are alike.
    if (m_gpu_timer.enabled() && (any_test_running() || (m_frame & 7) == 0)) {
        m_gpu_timer.begin();
    }

    // periodic status line (every 5 s while the feature is on)
    m_periodic_cpu_sum += m_cpu_frame_ms;
    ++m_periodic_frames;

    if (m_last_periodic.time_since_epoch().count() == 0 || !m_log_periodic->value() || m_diag_off->value()) {
        m_last_periodic = now;
        m_periodic_prev = read_counters();
        m_cpu_periodic_prev = read_cpu();
        m_periodic_cpu_sum = 0.0;
        m_periodic_frames = 0;
        m_frame_hist.fill(0);
        m_frame_max_ms = 0.0;
        m_periodic_due = false;
    } else if (m_periodic_due || now - m_last_periodic >= std::chrono::seconds(5)) {
        // [CPU 04.11.2026] composing the line takes ~1 ms of the main thread (the fight log of 04.11.2026: FoveatedRendering
        // at BeginRendering 0.85-1.5 ms in one frame per interval). BeginRendering lies on the path from one frame to the
        // next, WaitRendering does not (the main thread waits for the render thread there): the line is written at the
        // next WaitRendering, here only if none came since the last frame.
        // Once a WaitRendering came, only WaitRendering writes it.
        if (!m_periodic_due) {
            m_periodic_due = true;
            m_periodic_wr_seen = false;
        } else if (!m_periodic_wr_seen) {
            write_periodic_line();
        }
    }
#endif

    ++m_frame;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
// [CPU PROFILE 03.11.2026] start of the application entry this thread is in (the game's main thread runs them in turn)
thread_local int64_t t_entry_t0 = 0;

int64_t entry_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace
#endif

void FoveatedRendering::on_pre_application_entry(void* entry, const char* name, size_t hash) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the entries' timing and the status line: debug build only
    t_entry_t0 = entry_now_ns();

    // [CPU 04.11.2026] the status line that came due at the last BeginRendering (see update_frame)
    if (hash == "WaitRendering"_fnv && m_periodic_due) {
        m_periodic_wr_seen = true;
    }

    const bool periodic_now = hash == "WaitRendering"_fnv && m_periodic_due;

    if (periodic_now) {
        try {
            write_periodic_line();
        } catch (const std::exception& e) {
            m_periodic_due = false;
            REVR_LOG(spdlog::error("[TriView] status line failed: {}", e.what()));
        } catch (...) {
            m_periodic_due = false;
            REVR_LOG(spdlog::error("[TriView] status line failed (unknown exception)"));
        }
    }
#endif

    if (hash == "BeginRendering"_fnv) {
#if REVR_RELEASE
        // [EARLY IMGUI 04.11.2026] the option ([RELEASE SWITCH fix141] no CPU test in the release)
        cpustats::early_imgui_frame().store(m_early_imgui->value(), std::memory_order_relaxed);
#else
        m_entry_frames.fetch_add(1, std::memory_order_relaxed);
        m_frames_total.fetch_add(1, std::memory_order_relaxed);
        m_main_tid.store((uint32_t)GetCurrentThreadId(), std::memory_order_relaxed);
        timeline_tick(); // [TIMELINE 03.11.2026]
        // [EARLY IMGUI 04.11.2026] the option; the CPU test sets it per phase (on, except in its "as before" phases)
        cpustats::early_imgui_frame().store(m_early_imgui->value() && !(m_ctest_running && m_ctest_phase >= 0 && m_ctest_kind == 4),
            std::memory_order_relaxed);
#endif

        try {
            update_frame();
        } catch (const std::exception& e) {
            REVR_LOG(spdlog::error("[TriView] update_frame failed: {}", e.what()));
        } catch (...) {
            REVR_LOG(spdlog::error("[TriView] update_frame failed (unknown exception)"));
        }
    }
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::on_application_entry(void* entry, const char* name, size_t hash) {
    // [CPU PROFILE 03.11.2026] this entry's time into its slot (open addressing on the engine's hash of the name)
    if (t_entry_t0 != 0) {
        const auto dt = (uint64_t)(std::max)(entry_now_ns() - t_entry_t0, (int64_t)0);
        t_entry_t0 = 0;
        const size_t h = hash != 0 ? hash : 1;
        bool counted = false;

        for (size_t k = 0; k < 32; ++k) {
            auto& e = m_entry_stats[(h + k * 7) % m_entry_stats.size()];
            size_t cur = e.hash.load(std::memory_order_relaxed);

            if (cur == 0 && e.hash.compare_exchange_strong(cur, h, std::memory_order_relaxed)) {
                e.name.store(name, std::memory_order_relaxed);
                cur = h;
            }

            if (cur == h) {
                e.ns.fetch_add(dt, std::memory_order_relaxed);
                e.n.fetch_add(1, std::memory_order_relaxed);

                if (dt > e.max_ns.load(std::memory_order_relaxed)) {
                    e.max_ns.store(dt, std::memory_order_relaxed);
                }

                counted = true;
                break;
            }
        }

        if (!counted) {
            m_entry_lost.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (hash == "EndRendering"_fnv && m_begin_rendering.time_since_epoch().count() != 0) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_begin_rendering).count();

        if (ns > 0 && ns < 1'000'000'000) {
            m_rendering_ns.fetch_add((uint64_t)ns, std::memory_order_relaxed);
            m_rendering_n.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
#endif

// =================================================================================================
// [CPU 22.10.2026] Scene layer timing (render thread). The engine's update/draw of a scene layer run between our
// pre and post callbacks; the inset layer is the cloned camera's, the wide one the primary camera's.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
thread_local int64_t t_layer_start[2]{};
thread_local int t_layer_kind[2]{-1, -1};

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::layer_begin(sdk::renderer::layer::Scene* layer, int op) {
    // [fix94 19.11.2026] diagnostics off: no layer timing
    if (m_diag_off->value()) {
        t_layer_kind[op] = -1;
        t_layer_start[op] = 0;
        return;
    }

    int kind = 2;

    if (layer != nullptr && layer->is_fully_rendered()) {
        kind = VR::get()->is_quadview_inset_layer(layer) ? 1 : 0;
    }

    t_layer_kind[op] = kind;
    t_layer_start[op] = now_ns();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::layer_end(int op) {
    const int kind = t_layer_kind[op];

    if (kind < 0 || t_layer_start[op] == 0) {
        return;
    }

    const auto dt = now_ns() - t_layer_start[op];
    t_layer_kind[op] = -1;
    t_layer_start[op] = 0;

    if (dt > 0 && dt < 1'000'000'000) {
        m_cpu_ns[kind][op].fetch_add((uint64_t)dt, std::memory_order_relaxed);
        m_cpu_n[kind][op].fetch_add(1, std::memory_order_relaxed);
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool FoveatedRendering::on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    layer_begin(layer, 0);
    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    layer_end(0);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool FoveatedRendering::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    m_draw_tid.store((uint32_t)GetCurrentThreadId(), std::memory_order_relaxed); // [CPU THREADS 03.11.2026]
    layer_begin(layer, 1);
    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    layer_end(1);
}
#endif

// [CPU TEST 04.11.2026] kind 5: the fovea views' overlay layer update is not run (the engine's update function is skipped by
// the layer hook when a mod returns false); the VR mod does not let the overlay layers draw in VR anyway
// [FOVEA UI UPDATE 04.11.2026] the same with the option "Leave out the fovea views' interface update" (m_overlay_skip)
bool FoveatedRendering::on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) {
#if REVR_RELEASE
    const int mode = m_overlay_skip.load(std::memory_order_relaxed); // [RELEASE SWITCH fix141] (no CPU test)
#else
    const bool test = m_ctest_skip_fovea_overlay.load(std::memory_order_relaxed);
    const int mode = test ? 1 : m_overlay_skip.load(std::memory_order_relaxed);
#endif

    if (mode == 0 || layer == nullptr) {
        return true;
    }

    const auto& vr = VR::get();

    // the VR mod lets the overlay layers draw again without the headset or with VR suspended: then nothing is left out
    if (!vr->is_hmd_active() || vr->is_vr_suspended()) {
        return true;
    }

    if (vr->is_quadview_inset_layer(layer->get_parent())) {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] counters for the CPU test and the status line
        if (test) {
            m_ctest_overlay_skipped.fetch_add(1, std::memory_order_relaxed);
        }

        m_overlay_skipped.fetch_add(1, std::memory_order_relaxed);
#endif
        return false;
    }

    return true;
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
FoveatedRendering::CpuTotals FoveatedRendering::read_cpu() const {
    CpuTotals t{};

    for (int k = 0; k < 3; ++k) {
        for (int op = 0; op < 2; ++op) {
            t.ns[k][op] = m_cpu_ns[k][op].load(std::memory_order_relaxed);
            t.n[k][op] = m_cpu_n[k][op].load(std::memory_order_relaxed);
        }
    }

    t.rendering_ns = m_rendering_ns.load(std::memory_order_relaxed);
    t.rendering_n = m_rendering_n.load(std::memory_order_relaxed);

    const auto& qv = quadview::State::get();
    t.calls_projection = qv.calls_projection.load(std::memory_order_relaxed);
    t.calls_view = qv.calls_view.load(std::memory_order_relaxed);
    t.calls_primary_camera = qv.calls_primary_camera.load(std::memory_order_relaxed);
    t.views_game_logic = qv.views_game_logic.load(std::memory_order_relaxed);
    t.views_logic_kept = qv.views_logic_kept.load(std::memory_order_relaxed);
    t.views_in_layer = qv.views_in_layer.load(std::memory_order_relaxed);
    t.views_other_threads = qv.views_other_threads.load(std::memory_order_relaxed);
    t.views_jobs_outside = qv.views_jobs_outside.load(std::memory_order_relaxed);
    return t;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// per-frame averages since `prev` (ms): scene layer update/draw per kind, BeginRendering->EndRendering, mods
std::string FoveatedRendering::format_cpu(const CpuTotals& now, const CpuTotals& prev, double frames) const {
    frames = (std::max)(frames, 1.0);

    auto ms = [&](int k, int op) { return (double)(now.ns[k][op] - prev.ns[k][op]) / 1e6 / frames; };

    const auto rn = now.rendering_n - prev.rendering_n;
    const double rendering = rn > 0 ? (double)(now.rendering_ns - prev.rendering_ns) / 1e6 / (double)rn : 0.0;

    std::string o = std::format("cpu_ms/frame: wide upd={:.2f} draw={:.2f} | inset upd={:.2f} draw={:.2f} | other upd={:.2f} draw={:.2f} | begin->end rendering={:.2f}",
        ms(0, 0), ms(0, 1), ms(1, 0), ms(1, 1), ms(2, 0), ms(2, 1), rendering);

#if defined(RE4)
    o += std::format(" | mod callbacks={:.2f}", re4vr::perf::frame_avg_us() / 1000.0);
#endif

    o += std::format(" | hook calls/frame: projection={:.0f} view={:.0f} primary camera={:.0f} | views for game logic (head)/frame={:.1f}",
        (double)(now.calls_projection - prev.calls_projection) / frames, (double)(now.calls_view - prev.calls_view) / frames,
        (double)(now.calls_primary_camera - prev.calls_primary_camera) / frames, (double)(now.views_game_logic - prev.views_game_logic) / frames);
    // [VIEW RACE 03.11.2026] the other requests of the eye's view with the shared periphery and the entries game logic asked in
    o += std::format(" (head view mode {}; main thread kept the pass's eye {:.1f}, inside layer updates {:.1f}, other threads during layer updates "
                     "{:.1f}, other threads outside them {:.1f}; main thread entries: {})",
        quadview::State::get().head_view_mode.load(), (double)(now.views_logic_kept - prev.views_logic_kept) / frames,
        (double)(now.views_in_layer - prev.views_in_layer) / frames, (double)(now.views_other_threads - prev.views_other_threads) / frames,
        (double)(now.views_jobs_outside - prev.views_jobs_outside) / frames, quadview::State::get().take_logic_entries());

    // [HEIGHT PROBE 12.11.2026]
    o += "\n    " + VR::get()->height_probe_text();

    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [CPU PROFILE 03.11.2026] the main thread's application entries since the last report: ms per frame, most first, and their
// sum (the rest of the frame is between entries); max = the longest single call
std::string FoveatedRendering::format_entry_profile() {
    const double frames = (double)(std::max)(m_entry_frames.exchange(0, std::memory_order_relaxed), (uint64_t)1);

    struct Row {
        const char* name;
        double ms;
        double calls;
        double max_ms;
    };

    std::vector<Row> rows{};
    double total = 0.0;

    for (auto& e : m_entry_stats) {
        if (e.hash.load(std::memory_order_relaxed) == 0) {
            continue;
        }

        const uint64_t ns = e.ns.exchange(0, std::memory_order_relaxed);
        const uint64_t n = e.n.exchange(0, std::memory_order_relaxed);
        const uint64_t mx = e.max_ns.exchange(0, std::memory_order_relaxed);

        if (n == 0) {
            continue;
        }

        const double ms = (double)ns / 1e6 / frames;
        total += ms;
        rows.push_back(Row{e.name.load(std::memory_order_relaxed), ms, (double)n / frames, (double)mx / 1e6});
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });

    // the frame: time since the last report over its frames; the runtime's frame sync is waiting, not work
    const int64_t t = entry_now_ns();
    const double frame_ms = m_entry_report_ns != 0 ? (double)(t - m_entry_report_ns) / 1e6 / frames : 0.0;
    m_entry_report_ns = t;
    double wait_ms = 0.0;

    if (auto& vr = VR::get(); vr != nullptr && vr->get_runtime() != nullptr) {
        const uint64_t w = vr->get_runtime()->sync_wait_ns.load(std::memory_order_relaxed);
        wait_ms = (double)(w - m_entry_wait_ns_prev) / 1e6 / frames;
        m_entry_wait_ns_prev = w;
    }

    std::string o = std::format("main thread per frame (ms){}: frame {:.2f}, waiting for the headset's frame timing (WaitGetPoses) {:.2f}, in application "
                                "entries {:.2f} ({} entries; engine + the mods' callbacks in them)",
        cpu_test_label(), frame_ms, wait_ms, total, rows.size());

    for (size_t i = 0; i < rows.size() && i < 18; ++i) {
        o += std::format(" | {} {:.3f} (x{:.1f}, max {:.2f})", rows[i].name != nullptr ? rows[i].name : "?", rows[i].ms, rows[i].calls, rows[i].max_ms);
    }

    const auto lost = m_entry_lost.exchange(0, std::memory_order_relaxed);

    if (lost > 0) {
        o += std::format(" | {} entries not counted", lost);
    }

    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [COLLAPSE 05.11.2026] Log of 05.11.2026, a fight with FSR 4 in the fovea: from one frame to the next every frame took
// ~110 ms (the VR mod's present callback 98-100.5 ms) until the end of the session half a minute later, with the game's
// threads and the mod's GPU work idle and SteamVR reporting the application as not responding. This watch looks at the
// mods' present callbacks in half-second windows: six windows in a row (three seconds) over 40 ms per frame is a collapse.
// With the upscaler plugin in the fovea the fovea gives way to the mod's TAA (until the menu asks for the plugin again) and
// six seconds later the log says whether the frames came back; otherwise the collapse is only noted, once a minute. Not
// during the tests, a frame dump or a video and for a while after them (they hold frames on purpose).
void FoveatedRendering::collapse_watch(Clock::time_point now, bool active) {
    const auto& p = cpustats::present();
    const auto& vs = cpustats::vr_submit();
    auto& qv = quadview::State::get();

    if (any_test_running() || qv.dump_running()) {
        m_cw_quiet_until = (std::max)(m_cw_quiet_until, now + std::chrono::seconds(10));
    }

    const uint64_t presents = p.frames.load(std::memory_order_relaxed);
    const uint64_t mods_ns = p.mods_ns.load(std::memory_order_relaxed);
    const uint64_t submits = vs.submits.load(std::memory_order_relaxed);
    const uint64_t submit_ns = vs.submit_ns.load(std::memory_order_relaxed);

    const auto take_baseline = [&]() {
        m_cw_window = now;
        m_cw_presents = presents;
        m_cw_mods_ns = mods_ns;
        m_cw_submits = submits;
        m_cw_submit_ns = submit_ns;
    };

    if (!active || m_cw_window.time_since_epoch().count() == 0) {
        take_baseline();
        m_cw_slow_windows = 0;
        m_cw_slow_mods_ms = 0.0;
        m_cw_slow_submit_ms = 0.0;
        return;
    }

    if (now - m_cw_window < std::chrono::milliseconds(500)) {
        return;
    }

    const uint64_t dp = presents - m_cw_presents;
    const uint64_t ds = submits - m_cw_submits;
    const double window_mods_ms = dp > 0 ? (double)(mods_ns - m_cw_mods_ns) / 1e6 / (double)dp : 0.0;
    const double window_submit_ms = ds > 0 ? (double)(submit_ns - m_cw_submit_ns) / 1e6 / (double)ds : 0.0;
    take_baseline();

    const double t = std::chrono::duration<double>(now - m_start_time).count();

    if (now < m_cw_quiet_until || dp == 0 || window_mods_ms <= 40.0) {
        m_cw_slow_windows = 0;
        m_cw_slow_mods_ms = 0.0;
        m_cw_slow_submit_ms = 0.0;
        return;
    }

    ++m_cw_slow_windows;
    m_cw_slow_mods_ms += window_mods_ms;
    m_cw_slow_submit_ms += window_submit_ms;

    if (m_cw_slow_windows < 6) {
        return;
    }

    const double mods_avg = m_cw_slow_mods_ms / (double)m_cw_slow_windows;
    const double submit_avg = m_cw_slow_submit_ms / (double)m_cw_slow_windows;
    m_cw_slow_windows = 0;
    m_cw_slow_mods_ms = 0.0;
    m_cw_slow_submit_ms = 0.0;

    MemorySample mem{};

    if (auto& hook = g_framework->get_d3d12_hook(); g_framework->is_dx12() && hook != nullptr) {
        mem = adapter_memory().sample(hook->get_device());
    }

    const std::string what = std::format("the mods' present callbacks took {:.1f} ms per frame for 3 s (Submit to SteamVR {:.1f} ms per call; "
                                         "SteamVR: application not responding {} times so far) | {}",
        mods_avg, submit_avg, vs.not_responding.load(std::memory_order_relaxed), memory_line(mem, (std::max)(m_mem_peak_local, mem.local_usage)));

    // [GUARDS fix91 19.11.2026] the collapse is noted; the switch of the fovea from the upscaler plugin to the mod's TAA that
    // followed it here is gone with the plugin in the fovea
    if (now >= m_cw_noted_at) {
        m_cw_noted_at = now + std::chrono::seconds(60);
        const char* fovea = m_fovea_aa->value() == 2 ? "the mod's supersampling TAA" : "the mod's TAA";
        REVR_LOG(debug_log(std::format("[t={:.1f}s] COLLAPSE: {} | fovea: {}", t, what, fovea)));
        REVR_LOG(spdlog::warn("[FoveatedRendering] frames collapsed (fovea: {}): {}", fovea, what));
    }
}
#endif

// [GPU GUARD fix91 19.11.2026] The GPU guard of 05.11.2026 for the mod's own anti-aliasing. (Then it made the upscaler plugin
// in the fovea give way to the mod's TAA; the plugin is no longer the fovea's anti-aliasing.) Half-second windows of
// SteamVR's frame timing (frames shown again because the GPU was late) and of the estimated GPU time of a frame (the
// engine's render time + the mod's GPU time + the compositor's) decide, with quadview::GpuGuard, when the supersampling TAA
// runs with its lighter choices and when it goes back to the menu's: the fovea's history 1x instead of 2x and the
// periphery's image of the wide view's size instead of twice that, both at once (current_settings). Every switch goes
// into the log. Only when switched on in the menu, with something to lighten and OpenVR; not during the tests (the menu's
// choices then), and it stays as it is during a dump or a video. It also reads the headset's frame time, once, for the
// frame timing of the periodic line.
void FoveatedRendering::gpu_guard_watch(Clock::time_point now, bool active) {
    auto vr = VR::get();
    auto& vt = cpustats::vr_timing();

    // the headset's frame time, once
    if (m_hmd_frame_ms <= 0.0f && vr->is_hmd_active() && vr->get_runtime()->is_openvr() && vr->get_hmd() != nullptr) {
        const float hz = vr->get_hmd()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);

        if (hz > 30.0f && hz < 400.0f) {
            m_hmd_frame_ms = 1000.0f / hz;
            vt.frame_us.store((uint32_t)(m_hmd_frame_ms * 1000.0f), std::memory_order_relaxed);
            m_gpu_guard.frame_ms = m_hmd_frame_ms;
        }
    }

    const uint64_t frames = vt.frames.load(std::memory_order_relaxed);
    const uint64_t late = vt.late_gpu.load(std::memory_order_relaxed);
    const uint64_t comp_us = vt.gpu_compositor_us.load(std::memory_order_relaxed);
    // [fix96 19.11.2026] SteamVR's own measure of the frame's GPU time and its count of the frames over the frame time.
    // RE4 logs of 19.11.2026: frames shown again "because of the GPU" (late_gpu) stayed 0 while a third of the frames took
    // over the frame time and were shown on another vsync: the guard would never have seen them. And the mod's own GPU
    // time came from the periodic status line (every 5 s, none with the diagnostics off).
    const uint64_t total_us = vt.gpu_total_us.load(std::memory_order_relaxed);
    const uint64_t over = vt.gpu_over.load(std::memory_order_relaxed);
    // [fix102 20.11.2026] what SteamVR itself counts as missed: frames shown on another vsync than theirs, and frames it held
    // to half the rate (the test of fix101: "over the frame time" counted a quarter of the frames with the game at 90 fps)
    const uint64_t mis = vt.mispresented.load(std::memory_order_relaxed);
    const uint64_t throttled = vt.throttled.load(std::memory_order_relaxed);
    const double t = std::chrono::duration<double>(now - m_start_time).count();

    const auto take_baseline = [&]() {
        m_gg_window = now;
        m_gg_frames = frames;
        m_gg_late_gpu = late;
        m_gg_comp_us = comp_us;
        m_gg_total_us = total_us;
        m_gg_over = over;
        m_gg_mis = mis;
        m_gg_throttled = throttled;
        m_gg_rt_sum = 0.0;
        m_gg_rt_n = 0;
    };

    // what the menu's choices leave to lighten
    const bool fovea_2x = m_staa_scale->value() != 0;
    // (the tests compare their phases with each other: the menu's choices while one runs)
    const bool testing = any_test_running();
    // [GUARD fix101 20.11.2026] the steps there are from the menu's choices, and what each level saves
    uint8_t steps[4]{};
    double saves[5]{};
    int step_count = 0;
    {
        const int pa = (int)m_periphery_aa->value();
        const bool mono = vr->is_mono_periphery();
        const auto add = [&](uint8_t step, double save_ms) {
            steps[step_count] = step;
            saves[step_count + 1] = saves[step_count] + save_ms;
            ++step_count;
        };

        if (mono && pa == 2 && m_soft_outlines) {
            add(GS_OUTLINES, GS_SAVE_OUTLINES);
        }

        if (mono && pa == 2) {
            add(GS_PERIPHERY_1X, GS_SAVE_1X_WITH_OUTLINES - saves[step_count]);
        }

        if (mono && (pa == 1 || pa == 2)) {
            add(GS_CALM, GS_SAVE_CALM);
        }
    }
    const int extreme_level = fovea_2x ? step_count + 1 : 0;

    if (fovea_2x) {
        steps[step_count] = GS_FOVEA_1X;
        saves[step_count + 1] = saves[step_count] + GS_SAVE_FOVEA;
        ++step_count;
    }

    // (the menu's choices changed under the guard: from the start)
    if (step_count != m_gpu_step_count || std::memcmp(steps, m_gpu_steps, sizeof(steps)) != 0) {
        std::memcpy(m_gpu_steps, steps, sizeof(steps));
        m_gpu_step_count = step_count;
        m_gpu_guard.reset();
        m_gpu_level = 0;
        m_gpu_level_shown = 0;
        m_gpu_relief = false;
    }

    // (the supersampling TAA has run behind the calm light TAA long enough: it takes over)
    if (m_gpu_level_shown > m_gpu_level && now >= m_gpu_preroll_until) {
        m_gpu_level_shown = m_gpu_level;
    }

    const bool can_run = active && m_gpu_guard_opt->value() && step_count > 0 && vr->get_runtime()->is_openvr() && !testing;

    if (!can_run) {
        if (m_gpu_relief) {
            REVR_LOG(debug_log(std::format("[t={:.1f}s] GPU GUARD: back on the menu's supersampling TAA ({})", t,
                !m_gpu_guard_opt->value()       ? "the guard switched off in the menu"
                : step_count == 0               ? "the lightest choices set in the menu"
                : testing                       ? "a test"
                                                : "tri-view not active")));
        }

        m_gpu_relief = false;
        m_gpu_level = 0;
        m_gpu_level_shown = 0;
        m_gpu_guard.reset();
        take_baseline();
        return;
    }

    // a dump, a video or a capture holds frames on purpose (and for a while after it, and after a test): nothing is decided
    // from those frames and nothing switches, so a dump shows what was on the screen when it was asked for
    if (quadview::State::get().dump_running() || now < m_cw_quiet_until) {
        take_baseline();
        return;
    }

    if (m_gg_window.time_since_epoch().count() == 0) {
        take_baseline();
        return;
    }

    if (now - m_gg_window < std::chrono::milliseconds(500)) {
        return;
    }

    quadview::GpuGuard::Window w{};
    w.frames = (uint32_t)(frames - m_gg_frames);
    w.gpu_late = (uint32_t)(std::max)(late - m_gg_late_gpu, mis - m_gg_mis);
    w.throttled = (uint32_t)(throttled - m_gg_throttled);
    const double comp_ms = w.frames > 0 ? (double)(comp_us - m_gg_comp_us) / 1000.0 / (double)w.frames : 0.35;
    const double engine_ms = m_gg_rt_n > 0 ? m_gg_rt_sum / (double)m_gg_rt_n : 0.0;
    // (SteamVR's measure; the sum of the parts where it has none)
    w.gpu_ms = w.frames > 0 && total_us > m_gg_total_us ? (double)(total_us - m_gg_total_us) / 1000.0 / (double)w.frames : engine_ms + m_ours_gpu_ms + comp_ms;
    take_baseline();

    // what going back costs: the periphery's image of twice the size +0.25 ms per frame (measured, RE7, 17.11.2026); the
    // fovea's history 2x against 1x has not been measured in the game, taken as the same (the log's gpu_ours tells)
    // [fix96 19.11.2026] in steps, the periphery first: its image of twice the size costs 0.42 - 0.53 ms (the two GPU
    // tests of 19.11.2026, RE4), the fovea's history 2x 0.16 - 0.19. The step back from the level the guard is on:
    const int level_before = m_gpu_guard.level();
    m_gpu_stage_periphery = step_count > 0 && steps[0] != GS_FOVEA_1X;
    m_gpu_guard.levels = step_count;
    m_gpu_guard.extreme_level = extreme_level;
    // [fix102 20.11.2026] see GpuGuard::up_margin_ms: frames miss their vsync from about 10.1 ms (90 Hz): 1.2 and 1.9 ms
    // [GUARD START fix138 23.11.2026] the start from the menu's slider (GPU time per frame, 9.9 ms = fix137's 1.2 ms at
    // 90 Hz); the way back stays 0.7 ms under it, as before (1.9 - 1.2)
    {
        // [GUARD START % fix150 24.11.2026] a share of the headset's frame time (11.11 ms until it is read); the way back
        // 0.7 ms under it at 90 Hz, in the same proportion at other refresh rates
        const double share = std::clamp((double)m_gpu_guard_start_pct->value(), 0.70, 1.00);
        m_gpu_guard.up_margin_ms = (std::max)(m_gpu_guard.frame_ms * (1.0 - share), 0.0);
        m_gpu_guard.margin_ms = m_gpu_guard.up_margin_ms + 0.7 * m_gpu_guard.frame_ms / 11.11;
    }

    for (int i = 0; i <= quadview::GpuGuard::MAX_LEVELS; ++i) {
        m_gpu_guard.save_ms[i] = i <= step_count ? saves[i] : 0.0;
    }

    m_gpu_guard.full_extra_ms = level_before > 0 ? saves[level_before] - saves[level_before - 1] : saves[1];
    const auto change = m_gpu_guard.update(w);
    m_gpu_level = m_gpu_guard.level();

    if (change == quadview::GpuGuard::Change::ToLighter) {
        m_gpu_relief = true;
        m_gpu_level_shown = m_gpu_level; // (at once; also when the frames before a step back were running)

        if (level_before == 0) {
            m_gpu_relief_t = t;
        }

#if !REVR_RELEASE // [RELEASE SWITCH fix141] for the log only
        ++m_gpu_reliefs;
        std::string names{};

        for (int i = level_before; i < m_gpu_level; ++i) {
            names += std::format("{}{}", i > level_before ? " + " : "", GS_NAMES[steps[i]]);
        }

        REVR_LOG(debug_log(std::format("[t={:.1f}s] GPU GUARD: {} of {} frames shown again because the GPU was late, the frame's GPU time ~{:.2f} ms (game {:.2f} + mod "
                              "{:.2f} + compositor {:.2f}) of {:.2f} -> step {} of {}: {}{}",
            t, w.gpu_late, w.frames, w.gpu_ms, engine_ms, m_ours_gpu_ms, comp_ms, m_gpu_guard.frame_ms, m_gpu_level, step_count, names,
            extreme_level > 0 && m_gpu_level == extreme_level ? " (the frame rate collapsed: the last step)" : "")));
#endif
    } else if (change == quadview::GpuGuard::Change::ToFull) {
        m_gpu_relief = m_gpu_level > 0;
        // (from the calm light TAA back to the supersampling one: that runs behind it for GS_PREROLL first)
        const bool preroll = steps[m_gpu_level] == GS_CALM;

        if (preroll) {
            m_gpu_preroll_until = now + GS_PREROLL;
        } else {
            m_gpu_level_shown = m_gpu_level;
        }

        REVR_LOG(debug_log(std::format("[t={:.1f}s] GPU GUARD: {:.0f} s on lighter choices, the frame's GPU time now ~{:.2f} ms (game {:.2f} + mod {:.2f} + "
                              "compositor {:.2f}) -> one step back: no longer {}{} (now on step {} of {})",
            t, t - m_gpu_relief_t, w.gpu_ms, engine_ms, m_ours_gpu_ms, comp_ms, GS_NAMES[steps[m_gpu_level]],
            preroll ? ", after 0.4 s with the supersampling TAA running behind it" : "", m_gpu_level, step_count)));
    }
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [CPU 03.11.2026] the render thread's end of the frame (the thread that presents), per presented frame: REFramework's present
// hook (the wait for the ImGui lock, which the main thread's ImGui frame holds, and the mods' present callbacks are part of
// it), the game's own Present and the mods' callbacks after it. Then the mod's waits for its own earlier GPU work (all
// threads) and the engine's transform update hook (calls and REFramework's callbacks around them, all threads), per frame.
std::string FoveatedRendering::format_render_thread(double frames) {
    const auto& p = cpustats::present();
    const auto& f = cpustats::fence_waits();
    auto& th = cpustats::transform_hook();

    RenderThreadTotals now{};
    now.presents = p.frames.load(std::memory_order_relaxed);
    now.hook_ns = p.hook_ns.load(std::memory_order_relaxed);
    now.lock_wait_ns = p.lock_wait_ns.load(std::memory_order_relaxed);
    now.mods_ns = p.mods_ns.load(std::memory_order_relaxed);
    now.present_ns = p.present_ns.load(std::memory_order_relaxed);
    now.post_ns = p.post_ns.load(std::memory_order_relaxed);
    now.fence_ns = f.ns.load(std::memory_order_relaxed);
    now.fence_calls = f.calls.load(std::memory_order_relaxed);
    now.fence_long = f.long_waits.load(std::memory_order_relaxed);
    now.transform_calls = th.calls.load();
    now.transform_ns = th.ns.load();
    // [SUBMIT 05.11.2026]
    auto& vs = cpustats::vr_submit();
    now.vr_frames = vs.component_frames.load(std::memory_order_relaxed);
    now.vr_ns = vs.component_ns.load(std::memory_order_relaxed);
    now.submits = vs.submits.load(std::memory_order_relaxed);
    now.submit_ns = vs.submit_ns.load(std::memory_order_relaxed);
    now.submit_slow = vs.submit_slow.load(std::memory_order_relaxed);
    now.handoffs = vs.handoffs.load(std::memory_order_relaxed);
    now.handoff_ns = vs.handoff_ns.load(std::memory_order_relaxed);
    now.not_responding = vs.not_responding.load(std::memory_order_relaxed);
    now.resumed = vs.resumed.load(std::memory_order_relaxed);
    now.out_of_video_memory = vs.out_of_video_memory.load(std::memory_order_relaxed);
    now.lock_waits_present = vs.lock_waits_present.load(std::memory_order_relaxed);
    now.lock_wait_ns_present = vs.lock_wait_ns_present.load(std::memory_order_relaxed);
    now.lock_waits_poses = vs.lock_waits_poses.load(std::memory_order_relaxed);
    now.lock_wait_ns_poses = vs.lock_wait_ns_poses.load(std::memory_order_relaxed);
    now.lock_timeouts = vs.lock_timeouts.load(std::memory_order_relaxed);
    // [FRAME TIMING 05.11.2026]
    auto& vt = cpustats::vr_timing();
    now.ft_frames = vt.frames.load(std::memory_order_relaxed);
    now.ft_gpu_us = vt.gpu_total_us.load(std::memory_order_relaxed);
    now.ft_comp_us = vt.gpu_compositor_us.load(std::memory_order_relaxed);
    now.ft_over = vt.gpu_over.load(std::memory_order_relaxed);
    now.ft_late_gpu = vt.late_gpu.load(std::memory_order_relaxed);
    now.ft_late_cpu = vt.late_cpu.load(std::memory_order_relaxed);
    now.ft_throttled = vt.throttled.load(std::memory_order_relaxed);
    now.ft_mis = vt.mispresented.load(std::memory_order_relaxed);
    now.ft_dropped = vt.dropped.load(std::memory_order_relaxed);

    const auto& o = m_rt_prev;
    const double presents = (double)(std::max)(now.presents - o.presents, (uint64_t)1);
    const auto per_present = [&](uint64_t a, uint64_t b) { return (double)(a - b) / 1e6 / presents; };
    const auto per_frame = [&](uint64_t a, uint64_t b) { return (double)(a - b) / (std::max)(frames, 1.0); };

    std::string out = std::format("render thread per presented frame (ms, {} presents): present hook {:.3f} (waiting for the ImGui lock {:.3f}, the mods' "
                                  "present callbacks {:.3f}), the game's Present {:.3f}, the mods after it {:.3f} | the mod's waits for its own GPU work "
                                  "(all threads) {:.3f} ms per frame in {:.1f} waits ({:.2f} over 0.05 ms) | transform updates {:.0f} per frame, "
                                  "REFramework's callbacks around them {:.3f} ms per frame (all threads)",
        now.presents - o.presents, per_present(now.hook_ns, o.hook_ns), per_present(now.lock_wait_ns, o.lock_wait_ns),
        per_present(now.mods_ns, o.mods_ns), per_present(now.present_ns, o.present_ns), per_present(now.post_ns, o.post_ns),
        per_frame(now.fence_ns, o.fence_ns) / 1e6, per_frame(now.fence_calls, o.fence_calls), per_frame(now.fence_long, o.fence_long),
        per_frame(now.transform_calls, o.transform_calls), per_frame(now.transform_ns, o.transform_ns) / 1e6);

    // [SUBMIT 05.11.2026] the VR mod's hand-over to SteamVR inside its present callback
    {
        const auto per = [](uint64_t ns, uint64_t n) { return n > 0 ? (double)ns / 1e6 / (double)n : 0.0; };
        out += std::format(" | hand-over to SteamVR: the VR mod's D3D12 frame {:.3f} ms per present, of it Submit {:.3f} ms per call ({} calls, the longest "
                           "{:.1f} ms, {} over 20 ms), PostPresentHandoff {:.3f} ms | SteamVR events: application not responding {}, resumed {}, out of "
                           "video memory {}",
            per(now.vr_ns - o.vr_ns, now.vr_frames - o.vr_frames), per(now.submit_ns - o.submit_ns, now.submits - o.submits), now.submits - o.submits,
            (double)vs.submit_max_ns.exchange(0, std::memory_order_relaxed) / 1e6, now.submit_slow - o.submit_slow,
            per(now.handoff_ns - o.handoff_ns, now.handoffs - o.handoffs), now.not_responding - o.not_responding, now.resumed - o.resumed,
            now.out_of_video_memory - o.out_of_video_memory);
        // [COMPOSITOR LOCK 05.11.2026] how often one thread found the other inside the compositor
        out += std::format(" | compositor calls kept apart: the present waited {} times ({:.2f} ms), WaitGetPoses {} times ({:.2f} ms), time limit "
                           "reached {} times",
            now.lock_waits_present - o.lock_waits_present, (double)(now.lock_wait_ns_present - o.lock_wait_ns_present) / 1e6,
            now.lock_waits_poses - o.lock_waits_poses, (double)(now.lock_wait_ns_poses - o.lock_wait_ns_poses) / 1e6,
            now.lock_timeouts - o.lock_timeouts);

        // [FRAME TIMING 05.11.2026] SteamVR's timing of the frames
        const uint64_t ftn = now.ft_frames - o.ft_frames;
        out += std::format(" | SteamVR frame timing ({} frames, frame time {:.2f} ms): GPU time of the frame (game + mod + compositor) {:.2f} ms, the longest "
                           "{:.2f}, {} over the frame time; compositor {:.2f} ms | shown again because of the GPU {}, of the CPU {}; throttled by the "
                           "compositor {}; shown on another vsync {}, previous frame shown again {} | GPU guard: {}, the lighter supersampling TAA "
                           "because of it: {} ({} times in this session)",
            ftn, (double)vt.frame_us.load(std::memory_order_relaxed) / 1000.0,
            ftn > 0 ? (double)(now.ft_gpu_us - o.ft_gpu_us) / 1000.0 / (double)ftn : 0.0,
            (double)vt.gpu_total_max_us.exchange(0, std::memory_order_relaxed) / 1000.0, now.ft_over - o.ft_over,
            ftn > 0 ? (double)(now.ft_comp_us - o.ft_comp_us) / 1000.0 / (double)ftn : 0.0, now.ft_late_gpu - o.ft_late_gpu, now.ft_late_cpu - o.ft_late_cpu,
            now.ft_throttled - o.ft_throttled, now.ft_mis - o.ft_mis, now.ft_dropped - o.ft_dropped, m_gpu_guard_opt->value() ? "on" : "off",
            m_gpu_level <= 0 ? std::string{"no"} : std::format("yes, step {} of {}", m_gpu_level, m_gpu_step_count), m_gpu_reliefs);
    }

    m_rt_prev = now;

    // [EARLY IMGUI 04.11.2026] the main thread's ImGui frame (every mod's on_frame callback inside)
    {
        const auto& im = cpustats::imgui_frame();
        const uint64_t f = im.frames.load(std::memory_order_relaxed);
        const uint64_t ns = im.ns.load(std::memory_order_relaxed);
        const uint64_t lw = im.lock_wait_ns.load(std::memory_order_relaxed);
        const double n = (double)(std::max)(f - m_imgui_prev_frames, (uint64_t)1);
        out += std::format(" | main thread's ImGui frame (mods' on_frame inside) at {}: {:.3f} ms, of it waiting for the present hook's lock {:.3f}",
            cpustats::early_imgui_frame().load() ? "WaitRendering" : "BeginRendering", (double)(ns - m_imgui_prev_ns) / 1e6 / n,
            (double)(lw - m_imgui_prev_lock_ns) / 1e6 / n);
        m_imgui_prev_frames = f;
        m_imgui_prev_ns = ns;
        m_imgui_prev_lock_ns = lw;
    }

    // [FOVEA FOV TEST 06.11.2026 evening] the fovea cameras' field of view given to the engine in the GPU test's phases (set in
    // VR::update_camera at WaitRendering since 22:30; the build before set it at LockScene, where update_camera overwrote it)
    {
        auto& qv = quadview::State::get();
        const uint64_t n = qv.narrow_fov_count.exchange(0, std::memory_order_relaxed);
        const uint64_t sum = qv.narrow_fov_sum.exchange(0, std::memory_order_relaxed);
        const uint32_t lo = qv.narrow_fov_min.exchange(UINT32_MAX, std::memory_order_relaxed);
        const uint32_t hi = qv.narrow_fov_max.exchange(0, std::memory_order_relaxed);

        if (n > 0) {
            out += std::format(" | fovea cameras' field of view for the engine: {:.1f} degrees on average ({:.1f} .. {:.1f}, {} settings), "
                               "without the test (the VR mod's, from the headset) {:.1f}",
                (double)sum / 1000.0 / (double)n, lo / 1000.0, hi / 1000.0, n, qv.narrow_fov_engine.load(std::memory_order_relaxed) / 1000.0);
        }
    }

    // [LIGHTS fix82 17.11.2026] the fovea views' engine update with the eye's whole frustum: requests answered so since the
    // last line, and updates whose image matrices got the fovea's part afterwards (two per frame when it works)
    {
        auto& qv = quadview::State::get();
        const uint64_t asked = qv.whole_update_requests.exchange(0, std::memory_order_relaxed);
        const uint64_t narrowed = qv.whole_update_narrowed.exchange(0, std::memory_order_relaxed);
        out += std::format(" | fovea views' matrix sets with the eye's whole frustum: mask {} ({} requests, {} updates with the fovea's part put "
                           "into the others)",
            qv.whole_infos_now(), asked, narrowed);
    }

    // [GUI 04.11.2026] the GUI draw hook (inside the overlay layers' update, which BeginRendering waits for), all threads
    {
        auto& gh = cpustats::gui_hook();
        const uint64_t c = gh.calls.load();
        const uint64_t mn = gh.mods_ns.load();
        const uint64_t gn = gh.game_ns.load();
        const double fr = (std::max)(frames, 1.0);
        // [OVERLAY TEST 04.11.2026]
        const uint64_t ov = m_overlay_skipped.load(std::memory_order_relaxed);
        out += std::format(" | overlay layer updates left out {:.2f} per frame (fovea views' interface update: {})", (double)(ov - m_overlay_skipped_prev) / (std::max)(frames, 1.0),
            m_overlay_skip.load() == 0 ? "as the game" : "left out");
        m_overlay_skipped_prev = ov;
        out += std::format(" | GUI elements drawn {:.0f} per frame (all views): the mods' callbacks around them {:.3f} ms, the game's draw {:.3f} ms (all threads)",
            (double)(c - m_gui_prev_calls) / fr, (double)(mn - m_gui_prev_mods_ns) / 1e6 / fr, (double)(gn - m_gui_prev_game_ns) / 1e6 / fr);
        m_gui_prev_calls = c;
        m_gui_prev_mods_ns = mn;
        m_gui_prev_game_ns = gn;
    }

    out += " | " + ray_tracing_text();
    return out;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [RT 04.11.2026] the game's ray tracing settings as the render config has them now (a few getters, every 5 s)
std::string FoveatedRendering::ray_tracing_text() {
    static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");
    static const auto get_render_config = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;
    static const auto config_t = sdk::find_type_definition("via.render.RenderConfig");

    if (renderer_t == nullptr || get_render_config == nullptr || get_render_config->get_function() == nullptr || config_t == nullptr) {
        return "ray tracing: render config not found";
    }

    auto* ctx = sdk::get_thread_context();
    ::REManagedObject* config = nullptr;

    try {
        config = get_render_config->call_safe<::REManagedObject*>(ctx, renderer_t->get_instance());
    } catch (const std::exception&) {
        config = nullptr;
    }

    if (config == nullptr) {
        return "ray tracing: render config not found";
    }

    const auto value = [&](const char* getter, bool is_bool) -> std::string {
        const auto m = config_t->get_method(getter);

        if (m == nullptr || m->get_function() == nullptr || utility::is_stub_code((uint8_t*)m->get_function())) {
            return "?";
        }

        try {
            return is_bool ? (m->call_safe<bool>(ctx, config) ? "on" : "off") : std::to_string(m->call_safe<int32_t>(ctx, config));
        } catch (const std::exception&) {
            return "?";
        }
    };

    // (these are the choices the game keeps for when ray tracing is switched on in its options; with it off they do nothing:
    // no ray tracing layer in the views' render layer tree, 30.10.2026)
    return std::format("ray tracing choices kept by the game (used only with ray tracing on in its options): reflections {}, quality {}, GI {}, shadows {}, "
                       "transparency {}", value("get_RayTracingReflectionEnable", true),
        value("get_RayTracingQuality", false), value("get_RayTracingGIEnable", true), value("get_RayTracingShadowEnable", true),
        value("get_RayTracingTransparentEnable", true));
}
#endif

// =================================================================================================
// [TIMELINE 03.11.2026] A recording of a few dozen frames: every application entry of the main thread (REFramework's
// dispatch, the game's function, the mods' callbacks after it), REFramework's ImGui frame, every render layer hook (scene
// update and draw of each view, post effect, overlay, prepare output, output), the present path and WaitGetPoses, each with
// its thread and time. Appended to foveation_timeline.csv (by a thread of its own: sorting and writing take a while) to work
// out which work lies on the path from one frame to the next.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
uint32_t os_thread_id() {
    return (uint32_t)GetCurrentThreadId();
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::timeline_start(const std::string& label, int frames) {
    auto& tl = cpustats::timeline();

    if (tl.recording() || m_tl_frames_left > 0) {
        return;
    }

    cpustats::Timeline::s_thread_id = &os_thread_id;
    m_tl_label = label;
    m_tl_frames_left = (std::max)(frames, 2);
    tl.start();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::timeline_tick() {
    if (m_tl_request.exchange(false)) {
        timeline_start("by hand", 40);
        return;
    }

    if (m_tl_frames_left > 0 && --m_tl_frames_left == 0) {
        timeline_finish();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::timeline_finish() {
    auto events = cpustats::timeline().stop();
    const uint32_t index = ++m_tl_count;

    // the names the table needs, taken here (the game thread): the entries' names (the engine's static strings), the scene
    // layers of the views, the threads with a known role
    std::map<uint64_t, std::string> entry_names{};
    std::map<uint64_t, std::string> layer_names{};

    for (const auto& e : events) {
        if (e.ev >= (uint16_t)cpustats::Ev::EntryPre && e.ev <= (uint16_t)cpustats::Ev::EntryPost && e.arg != 0 && !entry_names.contains(e.arg)) {
            entry_names[e.arg] = (const char*)(uintptr_t)e.arg;
        }
    }

    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    const char* view_names[] = {"scene: wide view", "scene: fovea (clone 0)", "scene: fovea (clone 1)"};

    for (size_t v = 0; v < scenes.size(); ++v) {
        layer_names[(uint64_t)(uintptr_t)scenes[v]] = v < 3 ? view_names[v] : std::format("scene: view {}", v);
    }

    const uint32_t main_tid = m_main_tid.load();
    const uint32_t present_tid = cpustats::present().tid.load();
    const std::string label = m_tl_label;
    std::vector<std::string> mod_names{};

    for (const auto& mod : g_framework->get_mods()->get_mods()) {
        mod_names.emplace_back(mod != nullptr ? std::string{mod->get_name()} : std::string{"?"});
    }
    const double t_start = std::chrono::duration<double>(Clock::now() - m_start_time).count();

    REVR_LOG(debug_log(std::format("[TIMELINE] recording {} ({}): {} events, appended to foveation_timeline.csv", index, label, events.size())));

    std::thread([events = std::move(events), entry_names = std::move(entry_names), layer_names = std::move(layer_names), mod_names = std::move(mod_names), main_tid,
                    present_tid, label, index, t_start]() mutable {
        try {
            std::sort(events.begin(), events.end(), [](const cpustats::Event& a, const cpustats::Event& b) { return a.t < b.t; });

            std::map<uint64_t, uint32_t> entry_id{};
            std::map<uint64_t, uint32_t> layer_id{};
            std::map<uint64_t, uint32_t> gui_id{};
            std::string head = std::format("# timeline {}: {} (t={:.0f}s in foveation_debug.log), {} events; t in microseconds from the first event\n", index,
                label, t_start, events.size());
            head += "# ev: 1 entry dispatch begins, 2 the game's entry function begins, 3 it returned, 4 dispatch done, 5 ImGui frame begins, 6 ImGui frame done, "
                    "7 layer hook begins, 8 the game's layer function begins, 9 it returned, 10 layer hook done, 11 present hook begins, 12 present hook "
                    "done (the game's Present follows), 13 the game's Present returned, 14 mods' present callbacks begin, 15 done, 16 WaitGetPoses begins, "
                    "17 it returned\n";
            head += "# sub for layers: 0 scene update, 1 scene draw, 2 post effect update, 3 post effect draw, 4 overlay update, 5 overlay draw, "
                    "6 prepare output update, 7 prepare output draw, 8 output update, 9 output draw\n";
            head += "# ev 18-21: GUI draw hook of one element (id = element): mods' pre callbacks begin, the game's draw begins, it returned, mods' post "
                    "callbacks done; ev 22: one mod's GUI callback over 1 us (id = mod index, sub 0 pre / 1 post, val = its time in microseconds)\n";
            for (size_t m = 0; m < mod_names.size(); ++m) {
                head += std::format("# mod {} {}\n", m, mod_names[m]);
            }
            head += std::format("# thread {} main\n# thread {} present\n", main_tid, present_tid);

            for (const auto& [ptr, name] : entry_names) {
                const auto id = (uint32_t)entry_id.size() + 1;
                entry_id[ptr] = id;
                head += std::format("# entry {} {}\n", id, name);
            }

            std::string body{};
            body.reserve(events.size() * 24);
            const int64_t t0 = events.empty() ? 0 : events.front().t;

            for (const auto& e : events) {
                if (e.ev == 0) {
                    continue;
                }

                uint32_t id = 0;
                double val = 0.0;

                if (e.ev >= (uint16_t)cpustats::Ev::GuiPre && e.ev <= (uint16_t)cpustats::Ev::GuiPost) {
                    auto it = gui_id.find(e.arg);

                    if (it == gui_id.end()) {
                        it = gui_id.emplace(e.arg, (uint32_t)gui_id.size() + 1).first;
                    }

                    id = it->second;
                } else if (e.ev == (uint16_t)cpustats::Ev::GuiMod) {
                    id = (uint32_t)(e.arg >> 32);
                    val = (double)(uint32_t)(e.arg & 0xFFFFFFFFu) / 1000.0;
                } else if (e.ev >= (uint16_t)cpustats::Ev::EntryPre && e.ev <= (uint16_t)cpustats::Ev::EntryPost) {
                    const auto it = entry_id.find(e.arg);
                    id = it != entry_id.end() ? it->second : 0;
                } else if (e.ev >= (uint16_t)cpustats::Ev::LayerPre && e.ev <= (uint16_t)cpustats::Ev::LayerPost) {
                    auto it = layer_id.find(e.arg);

                    if (it == layer_id.end()) {
                        const auto nid = (uint32_t)layer_id.size() + 1;
                        it = layer_id.emplace(e.arg, nid).first;
                        const auto known = layer_names.find(e.arg);
                        head += std::format("# layer {} {}\n", nid, known != layer_names.end() ? known->second : std::string{"other"});
                    }

                    id = it->second;
                }

                body += std::format("{:.1f},{},{},{},{},{:.1f}\n", (double)(e.t - t0) / 1000.0, e.tid, e.ev, e.sub, id, val);
            }

            const auto path = REFramework::get_persistent_dir("foveation_timeline.csv");
            std::error_code ec{};

            if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 24u * 1024u * 1024u) {
                std::filesystem::remove(path, ec);
            }

            std::ofstream f(path, std::ios::app);

            if (f) {
                f << head << "t_us,tid,ev,sub,id,val\n" << body;
            }
        } catch (...) {
        }
    }).detach();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [CPU THREADS 03.11.2026] the writer thread measures the threads where this marker is in the queue
void FoveatedRendering::request_thread_sample() {
    auto log = m_log;

    LogQueue::ThreadSampleRequest r{};
    r.frames = m_frames_total.load(std::memory_order_relaxed);
    r.main_tid = m_main_tid.load(std::memory_order_relaxed);
    r.present_tid = cpustats::present().tid.load(std::memory_order_relaxed);
    r.draw_tid = m_draw_tid.load(std::memory_order_relaxed);

    {
        std::scoped_lock _{log->mutex};
        log->queue.push_back(std::string(1, '\x01'));
        log->thread_requests.push_back(r);

        if (!log->started) {
            log->started = true;
            std::thread([log] { log_writer(log); }).detach();
        }
    }

    log->cv.notify_one();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [CPU 28.10.2026] the usual frame time of the interval (10th percentile: the headset's refresh when the frame rate holds),
// how many frames took clearly longer (missed a refresh: the runtime showed the previous frame again) and the longest
std::string FoveatedRendering::format_frame_times() const {
    uint64_t total = 0;

    for (const auto n : m_frame_hist) {
        total += n;
    }

    if (total == 0) {
        return "frames: none";
    }

    uint64_t acc = 0;
    size_t p10 = 0;

    for (size_t i = 0; i < m_frame_hist.size(); ++i) {
        acc += m_frame_hist[i];

        if (acc * 10 >= total) {
            p10 = i;
            break;
        }
    }

    const double typical = ((double)p10 + 0.5) * 0.25;
    uint64_t missed = 0;
    uint64_t long_frames = 0;

    for (size_t i = 0; i < m_frame_hist.size(); ++i) {
        const double ms = ((double)i + 0.5) * 0.25;

        if (ms > typical * 1.4) {
            missed += m_frame_hist[i];
        }

        if (ms > 33.0) {
            long_frames += m_frame_hist[i];
        }
    }

    return std::format("frames: {} at typically {:.2f} ms, {} took over 1.4x ({:.2f}% = a repeated image while the head moves), "
                       "{} over 33 ms, longest {:.1f} ms",
        total, typical, missed, 100.0 * (double)missed / (double)total, long_frames, m_frame_max_ms);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::format_mod_costs(size_t top) const {
#if defined(RE4)
    const auto rows = re4vr::perf::rows();
    std::string o = std::format("  mod callbacks: {:.2f} ms/frame in total, most expensive (avg / max us):\n", re4vr::perf::frame_avg_us() / 1000.0);

    for (size_t i = 0; i < rows.size() && i < top; ++i) {
        o += std::format("    {:>8.1f} / {:>8.1f}  {}\n", rows[i].avg_us, rows[i].max_us, rows[i].name);
    }

    return o;
#else
    return {};
#endif
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::on_early_present() {
    if (m_gpu_timer.enabled()) {
        m_gpu_timer.end();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SUBMIT TIMELINE 06.11.2026] after the game's Present: the submits since the last one are this frame's
void FoveatedRendering::on_post_present() {
    d3d12::SubmitTimeline::get().marker("mark: after the game's Present"); // [06.11.2026 evening] still this frame's
    d3d12::SubmitTimeline::get().frame_mark();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// =================================================================================================
// Debug file
// =================================================================================================
// [CPU 28.10.2026] The file is written by a thread of its own: opening, measuring and appending to it on the game's
// render thread (every status line, every snapshot) took up to several milliseconds when the disk or a virus scanner
// was slow -- a missed frame each time. The callers only queue the text. The queue lives in a shared block that the
// writer thread keeps alive itself, so the mod can go away (DLL statics) without waiting for the thread.
void FoveatedRendering::debug_log(const std::string& text) {
    auto log = m_log;

    {
        std::scoped_lock _{log->mutex};
        log->queue.push_back(text);

        if (!log->started) {
            log->started = true;
            std::thread([log] { log_writer(log); }).detach();
        }
    }

    log->cv.notify_one();
}
#endif

// The instance is destroyed with the DLL's statics (DllMain, loader lock held): no lock and no join here. At process
// exit the writer is already gone (possibly while holding the mutex); waiting for it could hang the game's exit.
FoveatedRendering::~FoveatedRendering() {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] the debug log's thread
    if (m_log != nullptr) {
        m_log->stop.store(true);
        m_log->cv.notify_all();
    }
#endif
}

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::log_writer(std::shared_ptr<LogQueue> log) {
    std::vector<std::string> batch{};
    std::deque<LogQueue::ThreadSampleRequest> requests{};
    threadcpu::Sampler sampler{}; // [CPU THREADS 03.11.2026] lives on this thread (its thread handles with it)
    bool session_started = false;

    for (;;) {
        {
            std::unique_lock lock{log->mutex};
            log->cv.wait(lock, [&log] { return !log->queue.empty() || log->stop.load(); });

            if (log->queue.empty() && log->stop.load()) {
                return;
            }

            batch.assign(std::make_move_iterator(log->queue.begin()), std::make_move_iterator(log->queue.end()));
            log->queue.clear();
            requests.insert(requests.end(), log->thread_requests.begin(), log->thread_requests.end());
            log->thread_requests.clear();
        }

        // [CPU THREADS 03.11.2026] the markers become the threads' line (measured now, a few ms after the request), or nothing
        for (auto& t : batch) {
            if (t.size() != 1 || t[0] != '\x01') {
                continue;
            }

            t.clear();

            if (requests.empty()) {
                continue;
            }

            const auto r = requests.front();
            requests.pop_front();

            try {
                const auto line = sampler.sample(threadcpu::Request{r.frames, r.main_tid, r.present_tid, r.draw_tid});

                if (!line.empty()) {
                    t = "    " + line;
                }
            } catch (...) {
            }
        }

        try {
            const auto path = REFramework::get_persistent_dir("foveation_debug.log");
            std::error_code ec{};

            if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 8u * 1024u * 1024u) {
                std::filesystem::remove(path, ec);
                session_started = false;
            }

            std::ofstream f(path, std::ios::app);

            if (f) {
                if (!session_started) {
                    session_started = true;
                    f << "\n==================== new session (tri-view build) ====================\n";
                }

                for (const auto& t : batch) {
                    if (!t.empty()) {
                        f << t << "\n";
                    }
                }
            }
        } catch (...) {
        }

        batch.clear();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::build_snapshot(const char* title) {
    auto vr = VR::get();
    auto up = TemporalUpscaler::get();
    auto& g = EyeGaze::get();
    auto& qv = quadview::State::get();
    const double t = std::chrono::duration<double>(Clock::now() - m_start_time).count();
    const auto st = current_settings();

    std::string o{};
    o += std::format("[t={:.1f}s] SNAPSHOT: {}\n", t, title);
    o += std::format("  build: {}\n", qv_build_text()); // [BUILD LABEL fix104 20.11.2026]
    o += std::format("  sources compiled: {}\n", qv_sources_text()); // [BUILD STAMP fix107]

    if (const auto problem = qv_sources_problem(); !problem.empty()) {
        o += std::format("  STALE OBJECT FILES: {}\n", problem);
    }
    o += std::format("  settings: mode={} preset={} fovea_size={:.2f} fovea_ratio={:.2f} (slider {:.2f}/{:.2f}) upscaler={} feather={:.2f} deadzone={:.3f} "
                     "centre_when_zoomed={} border={} periphery_taa={:.2f} fovea_aa={} exposure_match={} tone_match={} periphery_light={} light_px={:.0f} shared_periphery={} hole_fill={} item_beams={} head_view_logic={} sharpness={:.1f} desktop_mirror={}\n",
        (int)m_mode->value(), (int)m_preset->value(), st.fraction, st.fovea_ratio, m_fraction->value(), m_ratio->value(), (int)st.upscaler, st.feather,
        st.deadzone, (bool)m_center_when_zoomed->value(), m_debug_border, st.taa_feedback,
        st.fovea_mode == quadview::FoveaMode::TAA ? (st.fovea_super ? "mod_super_taa" : "mod_taa") : "upscaler",
        st.brightness_match, st.tone_match, st.periphery_light, st.light_px, st.mono_periphery, st.hole_fill, st.item_beams, m_head_view_logic->value(), up->post_sharpness(), st.desktop_mirror);
    // [PERIPHERY SUPER fix80 17.11.2026]
    o += std::format("  periphery anti-aliasing: chosen={} (0 light TAA, 1 supersampling TAA, 2 supersampling TAA with the image of twice the size) "
                     "running={}{}{}\n",
        st.periphery_aa, up->periphery_running(), up->periphery_why() != TemporalUpscaler::PW_NONE ? " -- " : "",
        TemporalUpscaler::periphery_why_text(up->periphery_why()));
    o += std::format("  exposure test: {}; colour correction copied into the fovea views: {}\n", tone_test_text(), m_copy_colour_correct);
    o += std::format("  fovea TAA: feedback={:.2f} clip={:.2f} filter={:.2f} new_softness={:.2f} motion_reduce={:.2f} depth_reject={:.2f} sharpen={:.2f} "
                     "dilate={} catmull_rom={} luma_weight={} fast_converge={}\n",
        st.fovea_taa.feedback, st.fovea_taa.clip_gamma, st.fovea_taa.filter_radius, st.fovea_taa.new_softness, st.fovea_taa.motion_reduce,
        st.fovea_taa.depth_reject, st.fovea_taa.sharpen, st.fovea_taa.dilate, st.fovea_taa.catmull_rom, st.fovea_taa.luma_weight, st.fovea_taa.fast_converge);
    // [SUPER TAA 16.11.2026]
    {
        const auto& sx = st.super_taa;
        o += std::format("  supersampling TAA: chosen={} failed={} passes so far={} frames on the mod's TAA instead={} | history x{} stability={:.2f} "
                         "history_sharpness={:.2f} sharpen={:.2f} follow_changes={} threshold={:.2f} memory_there={:.2f} steady_detail={} range={:.2f} "
                         "spark={:.2f} memory={:.2f} jumps={} filter_scale={:.2f} clip={:.2f} motion_reduce={:.2f} depth_reject={:.2f} luma_weight={} lighter={} "
                         "lanczos={} | debug_view={} split={}\n",
            st.fovea_super, up->super_taa_failed(), qv.super_dispatches.load(), qv.super_fallbacks.load(), sx.scale, sx.stability, sx.history_sharpness,
            sx.sharpen, sx.follow, sx.follow_threshold, sx.follow_memory, sx.protect, sx.protect_range, sx.protect_spark, sx.protect_memory, sx.arrive,
            sx.filter_scale, sx.clip_gamma, sx.motion_reduce, sx.depth_reject, sx.luma_weight, sx.lighter, sx.lanczos, sx.debug_view, sx.split);
    }
    // [GPU GUARD fix91 19.11.2026] (the scale and the periphery's anti-aliasing above are what runs now, with the guard's part)
    o += std::format("  GPU guard: option={} the lighter supersampling TAA because of it={} (step {} of {}, the first is the periphery's: {}; {} steps taken) | menu: fovea history {}x, periphery {}\n",
        (bool)m_gpu_guard_opt->value(), m_gpu_relief, m_gpu_level, m_gpu_guard.levels, m_gpu_stage_periphery, m_gpu_reliefs, m_staa_scale->value() == 0 ? 1 : 2, (int)m_periphery_aa->value());
    // [fix94 19.11.2026]
    o += std::format("  periphery interface collected only around it: option={} | diagnostics off: option={} (GPU timestamps {})\n", (bool)m_ui_around->value(),
        (bool)m_diag_off->value(), m_gpu_timer.enabled() ? "on" : "off");
    // [DRED fix103 20.11.2026]
    o += std::format("  device removed data (DRED): {} in this session, option for the next start={} | game window forced out of full screen: option={}\n",
        d3d12_dred::active() ? "on" : "off", (bool)m_debug_dred->value(), vr->is_force_windowed());

    const bool hmd = vr->is_hmd_active();
    o += std::format("  vr: hmd_active={} openvr={} multipass={} technique={} render_frame={} zoomed={}\n", hmd, hmd && vr->get_runtime()->is_openvr(),
        vr->is_using_multipass(), (int)vr->get_rendering_technique(), vr->get_render_frame_count(), m_zoomed);

    const auto bb = up->get_backbuffer_size();
    o += std::format("  upscaler: activated={} ready={} d3d12={} eye_image={}x{} quad_view_status=\"{}\"\n", up->activated(), up->ready(), up->is_d3d12(), bb[0],
        bb[1], up->get_quadview_status());

    if (hmd && vr->get_runtime()->is_openvr()) {
        o += std::format("  lens hidden area: {}\n", hidden_area_report((uint32_t)bb[0], (uint32_t)bb[1]));
    }

    const auto l = up->get_quadview_layout();
    o += std::format("  layout: valid={} display={}x{} fovea={}x{} render={}x{} periphery x{:.2f} fovea x{:.2f} ({}) shared wide view x{:.3f} wider, eye share {}x{}\n",
        l.valid, l.display_w, l.display_h, l.inset_w, l.inset_h, l.render_w, l.render_h, l.wide_ratio, l.fovea_ratio, quality_name(l.fovea_quality),
        l.wide_x_factor, l.eye_wide_w, l.eye_wide_h);

    o += std::format("  state: requested={} engaged={} generation={} failure=\"{}\" debug_view={} separate_eye_passes={}\n", qv.requested(), qv.engaged(),
        qv.generation(), qv.failure(), m_debug_view, m_separate_eye_passes);

    for (int eye = 0; eye < 2; ++eye) {
        quadview::Rect r{};
        const bool have = qv.rect_for_eye(eye, r);
        o += std::format("    eye{} fovea rect: {} x={} y={} w={} h={}\n", eye, have ? "yes" : "no", r.x, r.y, r.w, r.h);
    }

    const auto raw = g.last_raw();
    o += std::format("  gaze: status=\"{}\" raw L=({:.3f},{:.3f}) R=({:.3f},{:.3f})\n", g.status_text(), raw[0], raw[1], raw[2], raw[3]);

    for (int eye = 0; eye < 2; ++eye) {
        const auto s = g.get_eye(eye);
        const auto c = g.lens_center(eye);
        o += std::format("    eye{}: tracking={} filtered=({:.3f},{:.3f}) lens_centre=({:.3f},{:.3f})\n", eye, s.tracking, s.x, s.y, c[0], c[1]);
    }

    const auto c = read_counters();
    o += std::format("  totals: wide_layers={} inset_layers={} inset_proj(scope={} camera={} gui={}) evaluations={} skipped={} rect_moves={}\n", c.wide, c.inset,
        c.by_scope, c.by_camera, c.gui, c.evaluations, c.skipped, c.moves);
    o += std::format("  cpu_frame_ms={:.2f} gpu_timer={}\n", m_cpu_frame_ms, m_gpu_timer.enabled());
    o += std::format("  tri-view in multipass={} shared periphery={} clones wanted={} made={} | {}\n", vr->is_stereo_fovea(), vr->is_mono_periphery(),
        vr->wanted_camera_clones(), vr->get_camera_duplicator().get_active_clone_count(), up->get_layer_report());
    o += std::format("  surplus clone layers switched off total={} reprojections total={}\n", qv.skipped_layers.load(), qv.reprojections.load());
    o += std::format("  interface: composed total={} without copies total={} | {}\n", qv.ui_composed.load(), qv.ui_missing.load(), up->get_ui_report());

    if (vr->is_mono_periphery()) {
        const auto f = qv.frusta();
        o += std::format("  frusta (tan): eye L x {:.3f}..{:.3f} y {:.3f}..{:.3f} | eye R x {:.3f}..{:.3f} | wide x {:.3f}..{:.3f} y {:.3f}..{:.3f} | eye offsets x L {:+.4f} R {:+.4f} m\n",
            f.eye_l[0], f.eye_r[0], f.eye_b[0], f.eye_t[0], f.eye_l[1], f.eye_r[1], f.wide_l, f.wide_r, f.wide_b, f.wide_t, f.eye_x[0], f.eye_x[1]);
    }

    o += "  post processing options (fog / volumetric fog / colour correction off): " + up->get_post_fx_stats() + "\n";
    o += VR::get()->describe_eyes();

    o += "  " + format_cpu(read_cpu(), m_cpu_periodic_prev, (double)(std::max)(m_periodic_frames, 1u)) + " (since the last status line)\n";
    o += format_mod_costs(15);
    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::write_periodic_line() {
    m_periodic_due = false;
    const auto c = read_counters();
    const double frames = (double)(std::max)(m_periodic_frames, 1u);
    auto up = TemporalUpscaler::get();
    const auto l = up->get_quadview_layout();

    std::string line = std::format("[t={:.0f}s] mode={} requested={} engaged={} technique={} render={}x{} cpu_ms={:.2f} per-frame: wide={:.2f} inset={:.2f} "
                                   "inset_proj(scope={:.2f} camera={:.2f} gui={:.2f}) eval={:.2f} skipped={:.2f} moves={} status=\"{}\"",
        std::chrono::duration<double>(Clock::now() - m_start_time).count(), (int)m_mode->value(), m_requested, quadview::State::get().engaged(),
        VR::get()->is_using_multipass() ? "multipass" : "other", l.render_w,
        l.render_h, m_periodic_cpu_sum / frames, (double)(c.wide - m_periodic_prev.wide) / frames, (double)(c.inset - m_periodic_prev.inset) / frames,
        (double)(c.by_scope - m_periodic_prev.by_scope) / frames, (double)(c.by_camera - m_periodic_prev.by_camera) / frames,
        (double)(c.gui - m_periodic_prev.gui) / frames, (double)(c.evaluations - m_periodic_prev.evaluations) / frames,
        (double)(c.skipped - m_periodic_prev.skipped) / frames, c.moves - m_periodic_prev.moves, up->get_quadview_status());

    // [STEREO FOVEA] what the engine renders (count and view ids of the scene layers, which camera each belongs to)
    line += " | " + up->get_layer_report();

    // [VIEW DIFFERENCE 01.11.2026] the fovea / periphery difference over blocks of 16 px (the last measurement)
    if (m_log_view_diff) {
        const auto& q = quadview::State::get();
        line += std::format(" | fovea/periphery difference L {:.1f}% R {:.1f}% (one gain), L {:.1f}% R {:.1f}% (as matched)", q.view_diff[0].load() * 100.0,
            q.view_diff[1].load() * 100.0, q.view_diff_matched[0].load() * 100.0, q.view_diff_matched[1].load() * 100.0);
    }

    // [MONO PERIPHERY] shared periphery
    {
        auto& qv = quadview::State::get();
        const auto skipped = qv.skipped_layers.load();
        const auto reproj = qv.reprojections.load();
        const auto ui_ok = qv.ui_composed.load();
        const auto ui_none = qv.ui_missing.load();
        line += std::format(" | periphery={} reprojections/frame={:.2f} surplus layers off/frame={:.2f} | interface composed/frame={:.2f} without copies/frame={:.2f} | {}",
            VR::get()->is_mono_periphery() ? "shared" : "per eye", (double)(reproj - m_periodic_reproj) / frames, (double)(skipped - m_periodic_skipped) / frames,
            (double)(ui_ok - m_periodic_ui) / frames, (double)(ui_none - m_periodic_ui_none) / frames, up->get_ui_report());
        m_periodic_reproj = reproj;
        m_periodic_skipped = skipped;
        m_periodic_ui = ui_ok;
        m_periodic_ui_none = ui_none;

        // [FRAME LATCH 04.11.2026] how often the compositing found the next frame's fovea rectangle / wide view matrix
        // already set (before: used; now each frame is composited with its own)
        line += std::format(" | compositing: fovea rectangle already the next frame's in {} eye images, wide view matrix the next "
                            "frame's in {} frames, FSR 4 jitter the next frame's in {} frames (each frame now uses its own) | interface anchors "
                            "moved by another mod after the VR mod (at the place drawn) {:.2f} per frame",
            qv.compose_rect_newer.exchange(0, std::memory_order_relaxed), qv.compose_vp_newer.exchange(0, std::memory_order_relaxed),
            qv.jitter_newer.exchange(0, std::memory_order_relaxed),
            (double)qv.gui_anchors_moved.exchange(0, std::memory_order_relaxed) / frames);

        // [ONE SUBMIT 04.11.2026] the upscaler plugin in the fovea (FSR 4 / FSR 3): calls and the time in them
        if (const auto evals = qv.plugin_evals.exchange(0, std::memory_order_relaxed); evals > 0) {
            line += std::format(" | fovea upscaler plugin: {:.2f} calls per frame, {:.3f} ms per frame in them",
                (double)evals / frames, (double)qv.plugin_eval_ns.exchange(0, std::memory_order_relaxed) / 1e6 / frames);
        } else {
            qv.plugin_eval_ns.store(0, std::memory_order_relaxed);
        }

        // [FSR DIRECT 06.11.2026] FSR 4 called by the mod: its passes and the CPU time in recording them
        if (const auto passes = qv.fsr_dispatches.exchange(0, std::memory_order_relaxed); passes > 0) {
            line += std::format(" | FSR 4 by the mod: {:.2f} passes per frame, {:.3f} ms per frame in them (CPU)", (double)passes / frames,
                (double)qv.fsr_dispatch_ns.exchange(0, std::memory_order_relaxed) / 1e6 / frames);
        } else {
            qv.fsr_dispatch_ns.store(0, std::memory_order_relaxed);
        }
    }

    // (the CPU test does not use the GPU timer: its lines keep the GPU numbers)
    if (m_gpu_timer.enabled() && (!any_test_running() || m_ctest_running)) {
        const auto gt = m_gpu_timer.take();
        line += std::format(" gpu_ms avg={:.2f} min={:.2f} max={:.2f} (n={})", gt.avg_ms, gt.min_ms, gt.max_ms, gt.samples);

        if (m_rt_n > 0) {
            line += std::format(" engine_render_time={:.2f}", m_rt_sum / (double)m_rt_n);
        }
    }

    // [AIM COST fix112]
    if (m_grip_n > 0) {
        line += std::format(" right_grip_frames={}/{}", m_grip_frames, m_grip_n);
    }

    // [fix115 21.11.2026] presents that submitted nothing because the runtime still waited for its pose update
    line += std::format(" pose_update_in_present={}", quadview::State::get().pose_in_present.exchange(0));

    m_grip_frames = 0;
    m_grip_n = 0;

    m_rt_sum = 0.0;
    m_rt_n = 0;

    // [GPU BREAKDOWN] our own passes (ms per frame, average): what the queue did between our lists is marked "between"
    // (during the GPU pass test the test takes them)
    if (d3d12::GpuSpans::get().enabled() && !any_test_running()) {
        const auto sp = d3d12::GpuSpans::get().take();
        std::string ours{" gpu_ours:"};
        double total = 0.0;
        // [GPU BREAKDOWN 06.11.2026] a name in both stamp layouts (the copier with and without a pre list of its own) is the
        // same work: an interval with frames of both (the GPU guard's switches) counts it once, by its frames
        std::vector<std::pair<std::string_view, std::pair<double, uint32_t>>> ours_by_name{};

        for (uint32_t i = 0; i < d3d12::GpuSpans::SPAN_COUNT; ++i) {
            if (sp[i].samples == 0) {
                continue;
            }

            ours += std::format(" {}={:.3f}(max {:.2f})", d3d12::GpuSpans::SPANS[i].name, sp[i].avg_ms, sp[i].max_ms);

            if (d3d12::GpuSpans::SPANS[i].ours) {
                const std::string_view name{d3d12::GpuSpans::SPANS[i].name};
                auto it = std::find_if(ours_by_name.begin(), ours_by_name.end(), [&](const auto& e) { return e.first == name; });

                if (it == ours_by_name.end()) {
                    ours_by_name.push_back({name, {0.0, 0u}});
                    it = std::prev(ours_by_name.end());
                }

                it->second.first += sp[i].avg_ms * (double)sp[i].samples;
                it->second.second += sp[i].samples;
            }
        }

        for (const auto& e : ours_by_name) {
            total += e.second.first / (double)(std::max)(e.second.second, 1u);
        }

        ours += std::format(" ours_total={:.3f}", total);
        line += ours;

        // [GPU GUARD 05.11.2026] the mod's GPU time per frame for the guard's estimate (pre begin .. copier end)
        for (uint32_t i = 0; i < d3d12::GpuSpans::SPAN_COUNT; ++i) {
            if (sp[i].samples > 0 && std::string_view{d3d12::GpuSpans::SPANS[i].name} == "wall(pre begin..copier end)") {
                m_ours_gpu_ms = sp[i].avg_ms;
            }
        }
    }

    const auto eye0 = EyeGaze::get().get_eye(0);
    line += std::format(" gaze0=({:.2f},{:.2f}){}", eye0.x, eye0.y, eye0.tracking ? "" : " [no tracking]");

    const auto cpu = read_cpu();
    line += "\n    " + format_cpu(cpu, m_cpu_periodic_prev, frames);
    m_cpu_periodic_prev = cpu;

    // [CPU 28.10.2026] frames that missed the headset's refresh in this interval (a missed frame is what shows as a short
    // double image when the head moves: the runtime repeats the previous frame), and the slowest single mod callbacks
    line += "\n    " + format_frame_times();
    m_frame_hist.fill(0);
    m_frame_max_ms = 0.0;

#if defined(RE4)
    {
        auto rows = re4vr::perf::rows();
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.max_us > b.max_us; });
        line += " | slowest callbacks (largest frame share, us):";

        for (size_t i = 0; i < rows.size() && i < 4; ++i) {
            line += std::format(" {} {:.0f};", rows[i].name, rows[i].max_us);
        }

        // [CPU PROFILE 03.11.2026] the mods' callbacks by average per frame (all threads)
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.avg_us > b.avg_us; });
        line += "\n    mod callbacks, average us per frame:";

        for (size_t i = 0; i < rows.size() && i < 32; ++i) {
            line += std::format(" {} {:.0f};", rows[i].name, rows[i].avg_us);
        }
    }
#endif

    // [CPU PROFILE 03.11.2026] where the game's main thread spends the frame
    line += "\n    " + format_entry_profile();

    // [CPU 03.11.2026] the render thread's present path, the mod's waits for its GPU work, the transform hook
    line += "\n    " + format_render_thread(frames);

    // [MEMORY 05.11.2026] video memory and the process's memory
    {
        MemorySample mem{};

        if (auto& hook = g_framework->get_d3d12_hook(); g_framework->is_dx12() && hook != nullptr) {
            mem = adapter_memory().sample(hook->get_device());
        }

        line += "\n    " + memory_line(mem, (std::max)(m_mem_peak_local, mem.local_usage));
        m_mem_peak_local = 0;
    }

    // [SUBMIT TIMELINE 06.11.2026] the GPU timeline of the frames of this interval (every sixth line with one frame in detail)
    if (d3d12::SubmitTimeline::get().running()) {
        // (in the GPU test every line: one frame of each part of a phase, with the end of the frame before it)
        line += "\n    " + d3d12::SubmitTimeline::get().take_report((m_st_lines++ % 6) == 0 || (m_ctest_running && m_ctest_gpu));
    }

    REVR_LOG(debug_log(line));

    // [CPU THREADS 03.11.2026] how busy each thread was over the same interval (measured on the log's writer thread, written
    // right after this line)
    request_thread_sample();

    m_periodic_prev = c;
    m_periodic_cpu_sum = 0.0;
    m_periodic_frames = 0;
    m_last_periodic = Clock::now();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool FoveatedRendering::gpu_timer_enable(bool enable) {
    if (!enable) {
        m_gpu_timer.disable();
        d3d12::GpuSpans::get().disable();
        return true;
    }

    auto& hook = g_framework->get_d3d12_hook();

    if (!g_framework->is_dx12() || hook == nullptr) {
        return false;
    }

    const bool ok = m_gpu_timer.enable(hook->get_device(), hook->get_command_queue());

    if (ok) {
        // [GPU BREAKDOWN] the mod's own passes, timed inside their command lists
        d3d12::GpuSpans::get().enable(hook->get_device(), hook->get_command_queue());
    }

    return ok;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// =================================================================================================
// Benchmark: alternating quad view OFF / ON phases, CPU frame time + GPU queue time
// =================================================================================================
void FoveatedRendering::bench_start() {
    if (any_test_running()) {
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("Benchmark: GPU timing could not be started."));
        return;
    }

    m_bench_rows.clear();
    m_bench_summary.clear();
    m_bench_running = true;
    m_bench_phase = 0;
    m_bench_measuring = false;
    m_bench_cpu_sum = 0.0;
    m_bench_cpu_n = 0;
    m_bench_phase_start = Clock::now();
    REVR_LOG(set_message("Benchmark running (about 90 s). Stay in the same spot and look around normally."));

    REVR_LOG(debug_log(build_snapshot("benchmark start") + std::format("  BENCHMARK: {} phases of {:.0f} s (first {:.0f} s of each discarded), tri-view OFF/ON alternating\n",
                                                     BENCH_PHASES, BENCH_PHASE_SECONDS, BENCH_WARMUP_SECONDS)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::bench_cancel(const char* why) {
    if (!m_bench_running) {
        return;
    }

    m_bench_running = false;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"Benchmark cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"BENCHMARK cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::bench_tick(double dt_ms, bool& force_off) {
    const bool on = (m_bench_phase % 2) == 1;
    force_off = !on;

    const double elapsed = std::chrono::duration<double>(Clock::now() - m_bench_phase_start).count();

    if (!m_bench_measuring && elapsed >= BENCH_WARMUP_SECONDS) {
        m_gpu_timer.take(); // drop the warm-up samples
        m_bench_cpu_sum = 0.0;
        m_bench_cpu_n = 0;
        m_cpu_bench_prev = read_cpu();
        m_bench_measuring = true;
    }

    if (m_bench_measuring && dt_ms > 0.0) {
        m_bench_cpu_sum += dt_ms;
        ++m_bench_cpu_n;
    }

    REVR_LOG(set_message(std::format("Benchmark phase {}/{} ({}), {:.0f}/{:.0f} s", m_bench_phase + 1, BENCH_PHASES, on ? "tri-view ON" : "tri-view OFF", elapsed,
        BENCH_PHASE_SECONDS)));

    if (elapsed < BENCH_PHASE_SECONDS) {
        return;
    }

    BenchRow row{};
    row.quadview_on = on;
    row.cpu_frames = m_bench_cpu_n;
    row.cpu_ms = m_bench_cpu_n > 0 ? m_bench_cpu_sum / (double)m_bench_cpu_n : 0.0;
    row.gpu = m_gpu_timer.take();
    row.cpu_text = format_cpu(read_cpu(), m_cpu_bench_prev, (double)m_bench_cpu_n);
    m_bench_rows.push_back(row);

    const auto l = TemporalUpscaler::get()->get_quadview_layout();
    REVR_LOG(debug_log(std::format("  BENCH phase {}: quad_view={} engaged={} render={}x{} cpu_frame_ms={:.3f} ({} frames, {:.1f} fps) gpu_ms avg={:.3f} min={:.3f} max={:.3f} (n={})\n      {}",
        m_bench_phase + 1, on ? "ON " : "OFF", quadview::State::get().engaged(), l.render_w, l.render_h, row.cpu_ms, row.cpu_frames,
        row.cpu_ms > 0.0 ? 1000.0 / row.cpu_ms : 0.0, row.gpu.avg_ms, row.gpu.min_ms, row.gpu.max_ms, row.gpu.samples, row.cpu_text)));

    ++m_bench_phase;
    m_bench_measuring = false;
    m_bench_phase_start = Clock::now();

    if (m_bench_phase >= BENCH_PHASES) {
        bench_finish();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::bench_finish() {
    double cpu[2]{}, gpu[2]{};
    int n[2]{};

    for (const auto& row : m_bench_rows) {
        const int i = row.quadview_on ? 1 : 0;
        cpu[i] += row.cpu_ms;
        gpu[i] += row.gpu.avg_ms;
        ++n[i];
    }

    for (int i = 0; i < 2; ++i) {
        if (n[i] > 0) {
            cpu[i] /= n[i];
            gpu[i] /= n[i];
        }
    }

    const double cpu_delta = cpu[0] > 0.0 ? (cpu[1] - cpu[0]) / cpu[0] * 100.0 : 0.0;
    const double gpu_delta = gpu[0] > 0.0 ? (gpu[1] - gpu[0]) / gpu[0] * 100.0 : 0.0;

    m_bench_summary = std::format("OFF: frame {:.2f} ms ({:.1f} fps), GPU {:.2f} ms | ON: frame {:.2f} ms ({:.1f} fps), GPU {:.2f} ms | frame {:+.1f}%, GPU {:+.1f}%",
        cpu[0], cpu[0] > 0.0 ? 1000.0 / cpu[0] : 0.0, gpu[0], cpu[1], cpu[1] > 0.0 ? 1000.0 / cpu[1] : 0.0, gpu[1], cpu_delta, gpu_delta);

    REVR_LOG(debug_log("  BENCHMARK RESULT: " + m_bench_summary + "\n" + build_snapshot("benchmark end")));

    m_bench_running = false;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("Benchmark finished. Result written to foveation_debug.log"));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// =================================================================================================
// [GPU TEST 29.10.2026] GPU pass test: the variants of the shared periphery's passes one after the other, GPU time per pass
// =================================================================================================
// the menu shows these texts while the game thread writes them
void FoveatedRendering::set_message(std::string text) {
    std::scoped_lock _{m_text_mutex};
    m_debug_message = std::move(text);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::get_message() const {
    std::scoped_lock _{m_text_mutex};
    return m_debug_message;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::set_ptest_summary(std::string text) {
    std::scoped_lock _{m_text_mutex};
    m_ptest_summary = std::move(text);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::get_ptest_summary() const {
    std::scoped_lock _{m_text_mutex};
    return m_ptest_summary;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ptest_start() {
    if (any_test_running()) {
        return;
    }

    if (!VR::get()->is_mono_periphery()) {
        REVR_LOG(set_message("GPU pass test: switch the shared periphery on first."));
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("GPU pass test: GPU timing could not be started."));
        return;
    }

    m_ptest_rows.clear();
    set_ptest_summary({});
    m_ptest_running = true;
    m_ptest_phase = -1;
    m_ptest_measuring = false;
    m_ptest_cpu_sum = 0.0;
    m_ptest_cpu_n = 0;
    m_ptest_phase_start = Clock::now();

    std::string order{};

    for (int i = 0; i < PTEST_PHASES; ++i) {
        order += std::format("{}{}", i > 0 ? ", " : "", PASS_VARIANTS[PTEST_ORDER[i]].name);
    }

    REVR_LOG(debug_log(build_snapshot("GPU pass test start") +
              std::format("  [GPU TEST] {} phases of {:.0f} s (first {:.0f} s of each not counted) after a {:.0f} s countdown: {}\n", PTEST_PHASES,
                  PTEST_PHASE_SECONDS, PTEST_WARMUP_SECONDS, PTEST_COUNTDOWN_SECONDS, order)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ptest_cancel(const char* why) {
    if (!m_ptest_running) {
        return;
    }

    m_ptest_running = false;
    m_ptest_phase = -1;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"GPU pass test cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"[GPU TEST] cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ptest_tick(double dt_ms) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_ptest_phase_start).count();

    if (m_ptest_phase < 0) {
        REVR_LOG(set_message(std::format("GPU pass test starts in {:.0f} s: close the menu, stand still and keep looking at the same spot.",
            (std::max)(PTEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        // [fix109 20.11.2026] the test's own texts of the eye pass are compiled on another thread from the countdown on (about
        // a minute the first time in a session): the phases begin when they are there (or after 4 minutes without them:
        // their phases then measure the pass in use)
        const int texts = quadview::State::get().test_texts.load(std::memory_order_relaxed);

        if (elapsed >= PTEST_COUNTDOWN_SECONDS && texts == 1 && elapsed < 240.0) {
            REVR_LOG(set_message(std::format("GPU pass test: compiling its passes ({:.0f} s, about a minute the first time). Stand still; the picture keeps running.", elapsed)));
            return;
        }

        if (elapsed >= PTEST_COUNTDOWN_SECONDS) {
            REVR_LOG(debug_log(std::format("[GPU TEST] the test's texts of the eye pass: {} after {:.0f} s", texts == 2 ? "ready" : texts == 1 ? "still compiling" :
                                                                                                    texts == 3 ? "failed" : "not asked for", elapsed)));
            m_ptest_phase = 0;
            m_ptest_measuring = false;
            m_ptest_phase_start = now;
            REVR_LOG(debug_log(std::format("[GPU TEST] phase 1/{}: {}", PTEST_PHASES, PASS_VARIANTS[PTEST_ORDER[0]].name)));
        }

        return;
    }

    const int v = PTEST_ORDER[m_ptest_phase];

    if (!m_ptest_measuring && elapsed >= PTEST_WARMUP_SECONDS) {
        d3d12::GpuSpans::get().take(); // the warm-up is not counted
        m_gpu_timer.take();
        m_ptest_cpu_sum = 0.0;
        m_ptest_cpu_n = 0;
        m_ptest_rt_sum = 0.0; // [HUD BLUR fix114]
        m_ptest_rt_n = 0;

        for (size_t i = 0; i < m_ptest_paths_start.size(); ++i) {
            m_ptest_paths_start[i] = quadview::State::get().path_frames[i].load();
        }
        m_ptest_engaged = true;
        m_ptest_measuring = true;
    }

    if (m_ptest_measuring && dt_ms > 0.0) {
        m_ptest_cpu_sum += dt_ms;
        ++m_ptest_cpu_n;
        m_ptest_engaged = m_ptest_engaged && quadview::State::get().engaged();
    }

    REVR_LOG(set_message(std::format("GPU pass test {}/{}: {} ({:.0f}/{:.0f} s). Stand still.", m_ptest_phase + 1, PTEST_PHASES, PASS_VARIANTS[v].name, elapsed,
        PTEST_PHASE_SECONDS)));

    if (elapsed < PTEST_PHASE_SECONDS) {
        return;
    }

    PassTestRow row{};
    row.variant = v;
    row.cpu_frames = m_ptest_cpu_n;
    row.cpu_ms = m_ptest_cpu_n > 0 ? m_ptest_cpu_sum / (double)m_ptest_cpu_n : 0.0;
    row.engaged = m_ptest_engaged && quadview::State::get().engaged();
    row.gpu = m_gpu_timer.take();
    row.spans = d3d12::GpuSpans::get().take();

    for (size_t i = 0; i < row.paths.size(); ++i) {
        row.paths[i] = quadview::State::get().path_frames[i].load() - m_ptest_paths_start[i];
    }

    m_ptest_rows.push_back(row);

    std::string line = std::format("[GPU TEST] phase {}/{} done: {}{} | engaged={} frames={} cpu_frame_ms={:.2f} gpu_ms={:.3f} ours_total={:.3f} | lists: "
                                   "pre list + copier {} one list {} |",
        m_ptest_phase + 1, PTEST_PHASES, PASS_VARIANTS[v].name, PASS_VARIANTS[v].same_image ? "" : " (image changed, measurement only)", row.engaged,
        row.cpu_frames, row.cpu_ms, row.gpu.avg_ms, ours_total_ms(row.spans), row.paths[0], row.paths[1]);

    for (const char* n : span_names()) {
        if (const double ms = span_ms(row.spans, n); ms > 0.0) {
            line += std::format(" {}={:.3f}", n, ms);
        }
    }

    // [HUD BLUR fix114 21.11.2026] the engine's own render time over the phase (the frame's GPU time is this, the mod's
    // passes and 0.62 ms: RE4 logs of fix112)
    if (m_ptest_rt_n > 0) {
        line += std::format(" | engine_render_time={:.3f} ({} frames)", m_ptest_rt_sum / (double)m_ptest_rt_n, m_ptest_rt_n);
    }

    REVR_LOG(debug_log(line));

    ++m_ptest_phase;
    m_ptest_measuring = false;
    m_ptest_phase_start = now;

    if (m_ptest_phase >= PTEST_PHASES) {
        ptest_finish();
        return;
    }

    REVR_LOG(debug_log(std::format("[GPU TEST] phase {}/{}: {}", m_ptest_phase + 1, PTEST_PHASES, PASS_VARIANTS[PTEST_ORDER[m_ptest_phase]].name)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ptest_finish() {
    // per variant: the passes the variants change (the current build's two runs averaged)
    struct Parts {
        double total{0.0}, pre{0.0}, reproject{0.0}, ui_wide{0.0}, eyes{0.0}, copier{0.0}, convert{0.0}, frame{0.0}, wall{0.0};
        int n{0};
    };

    const auto names = span_names();
    std::array<Parts, PASS_VARIANT_COUNT> parts{};
    // [ONE LIST 01.11.2026] the phases whose frames did not all take the list layout of their variant (the plugin in the
    // fovea, dump frames, ...); only the variant of a pre list of its own (24) has one
    std::string wrong_path{};

    // [07.11.2026] with FSR 4 in the fovea every frame takes the pre list + copier (the one list is only for the mod's TAA
    // in the fovea): a phase is noted when its frames mixed both layouts, or variant 24 had one-list frames
    for (const auto& row : m_ptest_rows) {
        const size_t want = row.variant == 24 || row.paths[1] == 0 ? 0u : 1u;
        const uint64_t all = row.paths[0] + row.paths[1];

        if (all > 0 && row.paths[want] != all) {
            wrong_path += std::format("{}{} ({} of {} frames as meant)", wrong_path.empty() ? "" : "; ", PASS_VARIANTS[row.variant].name, row.paths[want], all);
        }
    }
    std::array<std::vector<double>, PASS_VARIANT_COUNT> span_avg{}; // by name (names)

    for (auto& a : span_avg) {
        a.assign(names.size(), 0.0);
    }
    std::vector<double> current_runs{};
    bool all_engaged = true;

    for (const auto& row : m_ptest_rows) {
        auto& p = parts[(size_t)row.variant];

        for (size_t i = 0; i < names.size(); ++i) {
            span_avg[(size_t)row.variant][i] += span_ms(row.spans, names[i]);
        }

        p.total += ours_total_ms(row.spans);
        p.wall += span_ms(row.spans, "wall(pre begin..copier end)");
        p.pre += span_ms(row.spans, "pre");
        p.reproject += span_ms(row.spans, "block_min") + span_ms(row.spans, "reproject_1") + span_ms(row.spans, "reproject_2");
        p.ui_wide += span_ms(row.spans, "ui_clean_wide");
        p.eyes += span_ms(row.spans, "eye_L") + span_ms(row.spans, "eye_R");
        p.copier += span_ms(row.spans, "copier");
        p.convert += span_ms(row.spans, "convert(10->8 bit)");
        p.frame += row.gpu.avg_ms;
        ++p.n;
        all_engaged = all_engaged && row.engaged;

        if (row.variant == PTEST_REF) {
            current_runs.push_back(ours_total_ms(row.spans));
        }
    }

    for (size_t v = 0; v < parts.size(); ++v) {
        auto& p = parts[v];

        if (p.n > 0) {
            const double k = 1.0 / (double)p.n;

            for (auto& a : span_avg[v]) {
                a *= k;
            }

            p.total *= k;
            p.pre *= k;
            p.reproject *= k;
            p.ui_wide *= k;
            p.eyes *= k;
            p.copier *= k;
            p.convert *= k;
            p.frame *= k;
            p.wall *= k;
        }
    }

    const auto& c = parts[PTEST_REF];
    // how far the current build's runs spread: differences below that are noise
    const double drift = current_runs.size() >= 2 ? *std::max_element(current_runs.begin(), current_runs.end()) -
                                                        *std::min_element(current_runs.begin(), current_runs.end())
                                                  : 0.0;
    std::string o = "[GPU TEST] RESULT (GPU ms per frame; in brackets the variant minus the current build: + = the variant costs more)\n";
    o += std::format("  {}: ours_total {:.3f} (first run {:.3f}, last run {:.3f}, spread of its runs {:.3f}) | wall {:.3f} | pre {:.3f} reprojection {:.3f} "
                     "ui_clean_wide {:.3f} eye_L+eye_R {:.3f} copier {:.3f} convert {:.3f} | whole frame {:.2f}\n",
        PASS_VARIANTS[PTEST_REF].name, c.total, current_runs.empty() ? 0.0 : current_runs.front(), current_runs.empty() ? 0.0 : current_runs.back(), drift,
        c.wall, c.pre, c.reproject, c.ui_wide, c.eyes, c.copier, c.convert, c.frame);
    std::string summary = std::format("{} {:.2f} ms", PASS_VARIANTS[PTEST_REF].name, c.total);

    for (int v = 0; v < PASS_VARIANT_COUNT; ++v) {
        const auto& p = parts[(size_t)v];

        if (p.n == 0 || v == PTEST_REF) {
            continue;
        }

        o += std::format("  {}{}: ours_total {:.3f} ({:+.3f}) | wall {:.3f} ({:+.3f}) | pre {:.3f} ({:+.3f}) reprojection {:.3f} ({:+.3f}) ui_clean_wide {:.3f} "
                         "({:+.3f}) eye_L+eye_R {:.3f} ({:+.3f}) copier {:.3f} ({:+.3f}) | whole frame {:.2f} ({:+.2f})\n",
            PASS_VARIANTS[v].name, PASS_VARIANTS[v].same_image ? "" : " [image changed]", p.total, p.total - c.total, p.wall, p.wall - c.wall, p.pre,
            p.pre - c.pre, p.reproject, p.reproject - c.reproject, p.ui_wide, p.ui_wide - c.ui_wide, p.eyes, p.eyes - c.eyes, p.copier, p.copier - c.copier,
            p.frame, p.frame - c.frame);
        summary += std::format(" | {} {:+.2f} (wall {:+.2f})", PASS_VARIANTS[v].name, p.total - c.total, p.wall - c.wall);

        // every pass: the variant's time and the difference
        o += "      per pass:";

        for (size_t i = 0; i < names.size(); ++i) {
            if (span_avg[(size_t)v][i] > 0.0 || span_avg[PTEST_REF][i] > 0.0) {
                o += std::format(" {} {:.3f} ({:+.3f})", names[i], span_avg[(size_t)v][i], span_avg[(size_t)v][i] - span_avg[PTEST_REF][i]);
            }
        }

        o += "\n";
    }

    set_ptest_summary(summary);

    if (!all_engaged) {
        o += "  NOTE: tri-view was not engaged for the whole of a phase: the numbers of that phase are not comparable\n";
    }

    if (!wrong_path.empty()) {
        o += "  NOTE: phases with frames on other lists than their variant's (their numbers mix both ways): " + wrong_path + "\n";
    }

    if (drift > 0.05) {
        o += std::format("  NOTE: the current build's runs differ by up to {:.3f} ms: the scene changed during the test, differences below that are noise\n", drift);
    }

    m_ptest_running = false;
    m_ptest_phase = -1;
    REVR_LOG(debug_log(o + build_snapshot("GPU pass test end")));
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("GPU pass test finished. Result written to foveation_debug.log"));
}
#endif

// =================================================================================================
// [ENGINE VIEWS 30.10.2026] The engine's render layers and the view cost test
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
struct LayerNode {
    int depth{0};
    std::string type{};
    uint32_t priority{0};
    size_t children{0};
    const void* ptr{nullptr};
};

// the layer and everything below it, depth first (type, priority, number of children); read only
void collect_layers(sdk::renderer::RenderLayer* layer, int depth, std::vector<LayerNode>& out, size_t budget) {
    if (layer == nullptr || out.size() >= budget || depth > 12 || !utility::re_managed_object::is_managed_object(layer)) {
        return;
    }

    const auto tdef = utility::re_managed_object::get_type_definition((::REManagedObject*)layer);
    LayerNode n{};
    n.depth = depth;
    n.type = tdef != nullptr ? tdef->get_full_name() : std::string{"?"};
    n.priority = layer->m_priority;
    n.ptr = layer;
    auto& children = layer->get_layers();
    n.children = (size_t)children.size();
    out.push_back(n);

    for (auto* child : children) {
        collect_layers(child, depth + 1, out, budget);
    }
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::describe_render_layers() {
    std::string o = std::format("[t={:.1f}s] [LAYERS] the engine's render layers (type, priority, number of child layers)\n",
        std::chrono::duration<double>(Clock::now() - m_start_time).count());
    const auto output = sdk::renderer::get_output_layer();

    if (output == nullptr) {
        return o + "  no output layer\n";
    }

    // the output layer's own children: what runs once per frame next to the views
    {
        auto& top = output->get_layers();
        o += std::format("  output layer: {} child layers\n", (size_t)top.size());

        for (auto* child : top) {
            if (child == nullptr || !utility::re_managed_object::is_managed_object(child)) {
                continue;
            }

            const auto tdef = utility::re_managed_object::get_type_definition((::REManagedObject*)child);
            o += std::format("    {} prio={} children={}\n", tdef != nullptr ? tdef->get_full_name() : std::string{"?"}, child->m_priority,
                (size_t)child->get_layers().size());
        }
    }

    // the views: the wide view's tree in full, the others compared with it
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    std::vector<std::vector<LayerNode>> trees{};
    const char* names_mono[] = {"view 0 (primary camera, shared wide view)", "view 1 (clone 0, left fovea)", "view 2 (clone 1, right fovea)", "view 3 (other layer)"};
    const char* names_four[] = {"view 0 (primary camera, left wide view)", "view 1 (clone 0, right wide view)", "view 2 (clone 1, left fovea)", "view 3 (clone 2, right fovea)"};
    const char* const* names = VR::get()->is_mono_periphery() ? names_mono : names_four;

    for (size_t i = 0; i < scenes.size() && i < 4; ++i) {
        std::vector<LayerNode> t{};
        collect_layers(scenes[i], 0, t, 600);
        trees.push_back(std::move(t));
    }

    for (size_t i = 0; i < trees.size(); ++i) {
        const auto& t = trees[i];
        std::map<std::string, int> counts{};

        for (const auto& n : t) {
            ++counts[n.type];
        }

        o += std::format("  {}: {} layers in its tree, enabled={}\n", names[i], t.size(), scenes[i]->is_enabled());

        if (i == 0) {
            for (const auto& n : t) {
                o += std::format("    {}{} prio={} children={}\n", std::string((size_t)n.depth * 2, ' '), n.type, n.priority, n.children);
            }

            o += "    count per type:";

            for (const auto& [type, c] : counts) {
                o += std::format(" {} x{};", type, c);
            }

            o += "\n";
            continue;
        }

        // the same types in the same order as the wide view? layers that are the same objects as the wide view's?
        const auto& w = trees[0];
        bool same = t.size() == w.size();

        for (size_t k = 0; same && k < t.size(); ++k) {
            same = t[k].type == w[k].type && t[k].depth == w[k].depth;
        }

        size_t shared = 0;

        for (const auto& n : t) {
            for (const auto& m : w) {
                if (n.ptr == m.ptr) {
                    ++shared;
                    break;
                }
            }
        }

        o += std::format("    same layer types in the same order as view 0: {}; layer objects shared with view 0: {}\n", same, shared);

        if (!same) {
            std::map<std::string, int> wc{};

            for (const auto& m : w) {
                ++wc[m.type];
            }

            for (const auto& [type, c] : counts) {
                const int wn = wc.contains(type) ? wc[type] : 0;

                if (wn != c) {
                    o += std::format("      {}: {} here, {} in view 0\n", type, c, wn);
                }
            }

            for (const auto& [type, c] : wc) {
                if (!counts.contains(type)) {
                    o += std::format("      {}: 0 here, {} in view 0\n", type, c);
                }
            }
        }
    }

    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::vtest_start() {
    if (any_test_running()) {
        return;
    }

    if (!VR::get()->is_mono_periphery()) {
        REVR_LOG(set_message("Engine view cost test: switch the shared periphery on first."));
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("Engine view cost test: GPU timing could not be started."));
        return;
    }

    m_vtest_rows.clear();
    m_vtest_fraction = m_applied_fraction;
    m_vtest_ratio = m_applied_ratio;
    m_vtest_running = true;
    m_vtest_phase = -1;
    m_vtest_measuring = false;
    m_vtest_phase_start = Clock::now();

    std::string order{};

    for (int i = 0; i < VTEST_PHASE_COUNT; ++i) {
        order += std::format("{}{} views ratio x{:.2f}", i > 0 ? ", " : "", VTEST_PHASES[i].four_views ? 4 : 3, VTEST_PHASES[i].ratio_scale);
    }

    REVR_LOG(debug_log(build_snapshot("engine view cost test start") + describe_render_layers() +
              std::format("  [VIEW TEST] {} phases of {:.0f} s (first {:.0f} s of each not counted) after a {:.0f} s countdown: {}\n", VTEST_PHASE_COUNT,
                  VTEST_PHASE_SECONDS, VTEST_WARMUP_SECONDS, VTEST_COUNTDOWN_SECONDS, order)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::vtest_cancel(const char* why) {
    if (!m_vtest_running) {
        return;
    }

    m_vtest_running = false;
    m_vtest_phase = -1;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"Engine view cost test cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"[VIEW TEST] cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::vtest_tick(double dt_ms) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_vtest_phase_start).count();

    if (m_vtest_phase < 0) {
        REVR_LOG(set_message(std::format("Engine view cost test starts in {:.0f} s: close the menu, stand still and keep looking at the same spot.",
            (std::max)(VTEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        if (elapsed >= VTEST_COUNTDOWN_SECONDS) {
            m_vtest_phase = 0;
            m_vtest_measuring = false;
            m_vtest_phase_start = now;
        }

        return;
    }

    const auto& ph = VTEST_PHASES[m_vtest_phase];

    if (!m_vtest_measuring && elapsed >= VTEST_WARMUP_SECONDS) {
        d3d12::GpuSpans::get().take(); // the warm-up (new render targets) is not counted
        m_gpu_timer.take();
        m_vtest_cpu_sum = 0.0;
        m_vtest_cpu_n = 0;
        m_vtest_engaged = true;
        m_vtest_measuring = true;
    }

    if (m_vtest_measuring && dt_ms > 0.0) {
        m_vtest_cpu_sum += dt_ms;
        ++m_vtest_cpu_n;
        m_vtest_engaged = m_vtest_engaged && quadview::State::get().engaged() && VR::get()->is_mono_periphery() == !ph.four_views;
    }

    REVR_LOG(set_message(std::format("Engine view cost test {}/{}: {} views, ratio x{:.2f} ({:.0f}/{:.0f} s). Stand still.", m_vtest_phase + 1, VTEST_PHASE_COUNT,
        ph.four_views ? 4 : 3, ph.ratio_scale, elapsed, VTEST_PHASE_SECONDS)));

    if (elapsed < VTEST_PHASE_SECONDS) {
        return;
    }

    const auto l = TemporalUpscaler::get()->get_quadview_layout();
    ViewTestRow row{};
    row.phase = m_vtest_phase;
    row.ratio = ph.ratio_scale;
    row.four_views = ph.four_views;
    row.render_w = l.render_w;
    row.render_h = l.render_h;
    row.views = VR::get()->is_mono_periphery() ? 3u : 4u;
    row.engaged = m_vtest_engaged && quadview::State::get().engaged() && row.views == (ph.four_views ? 4u : 3u);
    row.frames = m_vtest_cpu_n;
    row.cpu_ms = m_vtest_cpu_n > 0 ? m_vtest_cpu_sum / (double)m_vtest_cpu_n : 0.0;
    row.gpu = m_gpu_timer.take();
    row.spans = d3d12::GpuSpans::get().take();
    m_vtest_rows.push_back(row);

    const double ours = ours_in_frame_ms(row.spans);
    REVR_LOG(debug_log(std::format("[VIEW TEST] phase {}/{} done: {} views, ratio x{:.2f}, render {}x{} | engaged={} frames={} cpu_frame_ms={:.2f} | frame GPU {:.3f} ms, "
                          "mod in the frame {:.3f} ms, engine and the rest {:.3f} ms | after the frame: post {:.3f} ms, conversion {:.3f} ms",
        m_vtest_phase + 1, VTEST_PHASE_COUNT, row.views, row.ratio, row.render_w, row.render_h, row.engaged, row.frames, row.cpu_ms, row.gpu.avg_ms, ours,
        row.gpu.avg_ms - ours, span_ms(row.spans, "post"), span_ms(row.spans, "convert(10->8 bit)"))));

    ++m_vtest_phase;
    m_vtest_measuring = false;
    m_vtest_phase_start = now;

    if (m_vtest_phase >= VTEST_PHASE_COUNT) {
        vtest_finish();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::vtest_finish() {
    // least squares over the three-view phases: engine time = fixed + per_mpx * (pixels of all views, millions)
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;
    std::string o = "[VIEW TEST] RESULT (GPU ms per frame; \"engine\" = the frame minus the mod's passes inside it)\n";

    for (const auto& r : m_vtest_rows) {
        const double mpx = (double)r.views * r.render_w * r.render_h / 1e6;
        const double ours = ours_in_frame_ms(r.spans);
        const double engine = r.gpu.avg_ms - ours;
        o += std::format("  {} views {}x{} ({:.2f} Mpx in all views): frame {:.3f}, mod {:.3f}, engine {:.3f}{}\n", r.views, r.render_w, r.render_h, mpx,
            r.gpu.avg_ms, ours, engine, r.engaged ? "" : " (tri-view not engaged for the whole phase)");

        if (!r.four_views && r.engaged && r.gpu.samples > 0) {
            sx += mpx;
            sy += engine;
            sxx += mpx * mpx;
            sxy += mpx * engine;
            ++n;
        }
    }

    const double den = n * sxx - sx * sx;

    if (n >= 3 && std::fabs(den) > 1e-9) {
        const double per_mpx = (n * sxy - sx * sy) / den;
        const double fixed = (sy - per_mpx * sx) / n;
        o += std::format("  three views: engine = {:.3f} ms that do not depend on the views' pixels + {:.3f} ms per million pixels rendered\n", fixed,
            per_mpx);

        // the same setting measured more than once (start, middle, end): how far the scene and the clocks drifted
        double lo = 1e9, hi = -1e9;
        int same_n = 0;

        for (const auto& r : m_vtest_rows) {
            if (!r.four_views && r.ratio == 1.0f && r.engaged && r.gpu.samples > 0) {
                const double engine = r.gpu.avg_ms - ours_in_frame_ms(r.spans);
                lo = (std::min)(lo, engine);
                hi = (std::max)(hi, engine);
                ++same_n;
            }
        }

        if (same_n >= 2) {
            o += std::format("  the user's size measured {} times: engine {:.3f} .. {:.3f} ms (spread {:.3f} ms: smaller differences are noise)\n", same_n, lo,
                hi, hi - lo);
        }

        // the four-view phase: one more view at its size -> the part of one view that does not depend on its pixels
        for (const auto& r : m_vtest_rows) {
            if (!r.four_views || !r.engaged || r.gpu.samples == 0) {
                continue;
            }

            const double mpx = (double)r.views * r.render_w * r.render_h / 1e6;
            const double engine = r.gpu.avg_ms - ours_in_frame_ms(r.spans);
            const double per_view = engine - fixed - per_mpx * mpx;
            o += std::format("  four views: engine {:.3f} ms -> a view costs about {:.3f} ms that do not depend on its pixels (shadow maps, culling, ...: "
                             "done once instead of three times this would save at most {:.2f} ms)\n",
                engine, per_view, 2.0 * per_view);

            // four views with the fovea as it is now: every view at the current render size, one view more
            for (const auto& b : m_vtest_rows) {
                if (b.four_views || !b.engaged || b.ratio != 1.0f || b.gpu.samples == 0) {
                    continue;
                }

                const double view_mpx = (double)b.render_w * b.render_h / 1e6;
                o += std::format("  four views with the fovea as it is now ({}x{} per view): the engine about {:+.3f} ms per frame\n", b.render_w, b.render_h,
                    per_view + per_mpx * view_mpx);
                break;
            }
        }
    } else {
        o += "  not enough complete phases for the fit\n";
    }

    m_vtest_running = false;
    m_vtest_phase = -1;
    REVR_LOG(debug_log(o + build_snapshot("engine view cost test end")));
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("Engine view cost test finished. Result written to foveation_debug.log"));
}
#endif

// =================================================================================================
// [ENGINE SHADOWS 30.10.2026] The engine's dynamic shadows off and on (via.render.Renderer DynamicShadowEnable, the switch
// the path tracing option of the Graphics mod uses): what they cost in all views together. Each view draws its own four
// shadow cascades (see the render layer tree), so this is the most a sharing of them could save. The image changes during
// the "off" phases (measurement only). The engine's switch is set and put back on the game thread.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
constexpr double STEST_COUNTDOWN_SECONDS = 8.0;
constexpr double STEST_PHASE_SECONDS = 15.0;
constexpr double STEST_WARMUP_SECONDS = 5.0;
constexpr bool STEST_PHASES[] = {true, false, true, false, true}; // shadows on / off
constexpr int STEST_PHASE_COUNT = (int)(sizeof(STEST_PHASES) / sizeof(STEST_PHASES[0]));

const sdk::REMethodDefinition* renderer_method(const char* name) {
    const auto t = sdk::find_type_definition("via.render.Renderer");
    return t != nullptr ? t->get_method(name) : nullptr;
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::optional<bool> FoveatedRendering::get_dynamic_shadows() {
    static const auto get = renderer_method("get_DynamicShadowEnable");

    if (get == nullptr || get->get_function() == nullptr) {
        return std::nullopt;
    }

    try {
        return get->call_safe<bool>(sdk::get_thread_context());
    } catch (const std::exception& e) {
        REVR_LOG(spdlog::error("[FoveatedRendering] get_DynamicShadowEnable: {}", e.what()));
        return std::nullopt;
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool FoveatedRendering::set_dynamic_shadows(bool on) {
    static const auto set = renderer_method("set_DynamicShadowEnable");

    if (set == nullptr || set->get_function() == nullptr) {
        return false;
    }

    try {
        set->call_safe<void>(sdk::get_thread_context(), on);
    } catch (const std::exception& e) {
        REVR_LOG(spdlog::error("[FoveatedRendering] set_DynamicShadowEnable({}): {}", on, e.what()));
        return false;
    }

    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::stest_start() {
    if (any_test_running()) {
        return;
    }

    const auto original = get_dynamic_shadows();

    if (!original.has_value()) {
        REVR_LOG(set_message("Engine shadow test: the engine's shadow switch was not found."));
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("Engine shadow test: GPU timing could not be started."));
        return;
    }

    m_stest_original = *original;
    m_stest_rows.clear();
    m_stest_running = true;
    m_stest_phase = -1;
    m_stest_measuring = false;
    m_stest_phase_start = Clock::now();
    REVR_LOG(debug_log(build_snapshot("engine shadow test start") +
              std::format("  [SHADOW TEST] dynamic shadows were {} | {} phases of {:.0f} s (first {:.0f} s of each not counted) after a {:.0f} s countdown: "
                          "on, off, on, off, on\n",
                  m_stest_original ? "on" : "off", STEST_PHASE_COUNT, STEST_PHASE_SECONDS, STEST_WARMUP_SECONDS, STEST_COUNTDOWN_SECONDS)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::stest_cancel(const char* why) {
    if (!m_stest_running) {
        return;
    }

    m_stest_running = false;
    m_stest_phase = -1;
    VR::get()->set_dynamic_shadow_override(-1);
    set_dynamic_shadows(m_stest_original); // (always on the game thread: update_frame)
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"Engine shadow test cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"[SHADOW TEST] cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::stest_tick(double dt_ms) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_stest_phase_start).count();

    if (m_stest_phase < 0) {
        REVR_LOG(set_message(std::format("Engine shadow test starts in {:.0f} s: close the menu, stand still and keep looking at the same spot.",
            (std::max)(STEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        if (elapsed >= STEST_COUNTDOWN_SECONDS) {
            m_stest_phase = 0;
            m_stest_measuring = false;
            m_stest_phase_start = now;
            VR::get()->set_dynamic_shadow_override(STEST_PHASES[0] ? 1 : 0);
            set_dynamic_shadows(STEST_PHASES[0]);
        }

        return;
    }

    const bool shadows = STEST_PHASES[m_stest_phase];

    if (!m_stest_measuring && elapsed >= STEST_WARMUP_SECONDS) {
        d3d12::GpuSpans::get().take();
        m_gpu_timer.take();
        m_stest_cpu_sum = 0.0;
        m_stest_cpu_n = 0;
        m_stest_rt_sum = 0.0;
        m_stest_rt_n = 0;
        m_stest_engaged = true;
        m_stest_measuring = true;
    }

    if (m_stest_measuring && dt_ms > 0.0) {
        m_stest_cpu_sum += dt_ms;
        ++m_stest_cpu_n;
        m_stest_engaged = m_stest_engaged && quadview::State::get().engaged();
    }

    REVR_LOG(set_message(std::format("Engine shadow test {}/{}: shadows {} ({:.0f}/{:.0f} s). Stand still.", m_stest_phase + 1, STEST_PHASE_COUNT, shadows ? "on" : "OFF",
        elapsed, STEST_PHASE_SECONDS)));

    if (elapsed < STEST_PHASE_SECONDS) {
        return;
    }

    ShadowTestRow row{};
    row.phase = m_stest_phase;
    row.shadows = shadows;
    row.engaged = m_stest_engaged && quadview::State::get().engaged() && get_dynamic_shadows().value_or(!shadows) == shadows;
    row.frames = m_stest_cpu_n;
    row.cpu_ms = m_stest_cpu_n > 0 ? m_stest_cpu_sum / (double)m_stest_cpu_n : 0.0;
    row.render_time_ms = m_stest_rt_n > 0 ? m_stest_rt_sum / (double)m_stest_rt_n : -1.0;
    row.gpu = m_gpu_timer.take();
    row.spans = d3d12::GpuSpans::get().take();
    m_stest_rows.push_back(row);

    const double ours = ours_in_frame_ms(row.spans);
    REVR_LOG(debug_log(std::format("[SHADOW TEST] phase {}/{} done: dynamic shadows {} | engaged={} frames={} cpu_frame_ms={:.2f} | frame GPU {:.3f} ms, mod in the frame "
                          "{:.3f} ms, engine and the rest {:.3f} ms | the engine's render time {:.3f} ms",
        m_stest_phase + 1, STEST_PHASE_COUNT, shadows ? "on" : "off", row.engaged, row.frames, row.cpu_ms, row.gpu.avg_ms, ours, row.gpu.avg_ms - ours,
        row.render_time_ms)));

    ++m_stest_phase;
    m_stest_measuring = false;
    m_stest_phase_start = now;

    if (m_stest_phase >= STEST_PHASE_COUNT) {
        stest_finish();
    } else {
        VR::get()->set_dynamic_shadow_override(STEST_PHASES[m_stest_phase] ? 1 : 0);
        set_dynamic_shadows(STEST_PHASES[m_stest_phase]);
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::stest_finish() {
    double on_sum = 0.0, off_sum = 0.0, on_lo = 1e9, on_hi = -1e9, cpu_on = 0.0, cpu_off = 0.0;
    int on_n = 0, off_n = 0;
    std::string o = "[SHADOW TEST] RESULT (GPU ms per frame; \"engine\" = the frame minus the mod's passes inside it)\n";

    for (const auto& r : m_stest_rows) {
        const double engine = r.gpu.avg_ms - ours_in_frame_ms(r.spans);
        o += std::format("  shadows {}: frame {:.3f}, engine {:.3f}, the engine's render time {:.3f}, cpu frame {:.2f} ms{}\n", r.shadows ? "on " : "off",
            r.gpu.avg_ms, engine, r.render_time_ms, r.cpu_ms, r.engaged ? "" : " (not counted: tri-view not engaged or the switch did not hold)");

        if (!r.engaged || r.gpu.samples == 0) {
            continue;
        }

        if (r.shadows) {
            on_sum += engine;
            cpu_on += r.cpu_ms;
            on_lo = (std::min)(on_lo, engine);
            on_hi = (std::max)(on_hi, engine);
            ++on_n;
        } else {
            off_sum += engine;
            cpu_off += r.cpu_ms;
            ++off_n;
        }
    }

    if (on_n > 0 && off_n > 0) {
        const double d = on_sum / on_n - off_sum / off_n;
        o += std::format("  the engine's dynamic shadows cost {:.3f} ms of GPU time per frame in all views together (shadows on measured {} times: "
                         "{:.3f} .. {:.3f} ms, smaller differences are noise); CPU frame time {:.2f} ms with them, {:.2f} without\n",
            d, on_n, on_lo, on_hi, cpu_on / on_n, cpu_off / off_n);
    } else {
        o += "  not enough complete phases\n";
    }

    m_stest_running = false;
    m_stest_phase = -1;
    VR::get()->set_dynamic_shadow_override(-1);
    set_dynamic_shadows(m_stest_original);
    REVR_LOG(debug_log(o + build_snapshot("engine shadow test end")));
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("Engine shadow test finished. Result written to foveation_debug.log"));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// The engine types around the shadows: what each layer type has (fields and methods, with the values of the plain ones in
// the layers of the wide view and of the left fovea view), the shadow settings of via.render.Renderer, and every
// via.render type with "Shadow" in its name. Read only: only getters without parameters are called.
std::string FoveatedRendering::describe_shadow_types() {
    return describe_engine_types(0);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [AMBIENT OCCLUSION 30.10.2026] the same for the ambient occlusion: the game's render settings (via.render.RenderConfig,
// every getter), the AO components of each view's camera, and every via.render type about ambient occlusion (enums with
// their values). Read only.
std::string FoveatedRendering::describe_ao_settings() {
    return describe_engine_types(1);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [REAL EV 13.11.2026] the choices of the real exposure test
static const char* const* real_ev_names() {
    static const char* names[] = {"off", "on, their adaptation stopped (rates 0)", "on, their automatic exposure Disable",
        "on, their automatic exposure FixedEnable"};
    return names;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [EXPOSURE TEST 01.11.2026] the tone mapping test as text (log, snapshot)
std::string FoveatedRendering::tone_test_text() const {
    static const char* local_names[] = {"as the game", "off in the fovea views", "off in all views"};
    const auto autos = m_tt_auto > 0 && m_tt_auto <= (int)m_auto_values.size()
                           ? std::format("{} ({}) in all views", m_auto_values[m_tt_auto - 1].first, m_auto_values[m_tt_auto - 1].second)
                           : std::string{"as the game"};
    static const char* lock_names[] = {"off", "in the fovea views", "in all views"};
    return std::format("local exposure {}, fovea views with the wide view's automatic exposure {}, automatic exposure {}, exposure range "
                       "closed on the game camera's SnappedRealEV {}, fovea views' EV offset {:+.2f}, EV follows the periphery {}, fovea views' "
                       "adaptation {}, the game camera's real exposure {} (measured every 8th frame while steady: {})",
        local_names[std::clamp(m_tt_local, 0, 2)], m_tt_copy_auto, autos, lock_names[std::clamp(m_tt_ev_lock, 0, 2)], m_tt_ev_offset, m_tt_ev_follow,
        m_tt_clone_adapt == 0 ? "as the game" : m_tt_clone_adapt == 1 ? "rates 0" : "rates 1000", real_ev_names()[std::clamp(m_tt_real_ev, 0, 3)],
        m_tt_real_ev_steady);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [VRS 01.11.2026] the same for variable rate shading: what the GPU offers (D3D12), the members about it of the render
// settings, the renderer, the cameras, the scene layers and every layer type in the views (with each view's value), then
// every type of the engine and every member of any type named after it (also foveated rendering, which the engine might
// do with it). Read only. Decides whether a coarser shading in the wide view alone is possible (only a global setting ->
// dropped, as agreed).
std::string FoveatedRendering::describe_vrs_settings() {
    return describe_engine_types(2);
}
#endif

// =================================================================================================
// [WHITE POINT 13.11.2026] the engine's adapted exposure
// =================================================================================================
// RE7 test of 13.11.2026: every via.render.ToneMapping value is the same on the three views (EV and SnappedRealEV a constant
// 4.5), the adaptation itself is on the GPU. The fovea views' own adaptation (Bright / DarkAdaptationRate) at 0 keeps it
// where it is, at 1000 it runs away (the fovea white). re7.exe has a lighting class with getWhitePointSRV / getWhitePointUAV,
// getHistogramBuffer / getHistogramUAV and gettTonemapCB (no parameters): the resources the tone mapping reads its adapted
// white point from. The report lists the types with such members, calls the getters without parameters of the one found
// as a singleton and looks behind the returned views for the D3D12 resources; the post effect draws of the next frames
// record which resources each view sees (one for all views, or one per view) and the order of the views.
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
std::string wp_module_of(const void* p) {
    HMODULE mod{};

    if (p == nullptr || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &mod)) {
        return {};
    }

    char name[MAX_PATH]{};
    GetModuleFileNameA(mod, name, MAX_PATH);
    std::string n{name};
    const auto slash = n.find_last_of("\\/");
    return slash != std::string::npos ? n.substr(slash + 1) : n;
}

std::string wp_resource_text(ID3D12Resource* r) {
    const auto d = r->GetDesc();

    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return std::format("buffer {} bytes, flags 0x{:x}", d.Width, (uint32_t)d.Flags);
    }

    return std::format("texture dim {} {}x{}x{} mips {} format {} flags 0x{:x}", (int)d.Dimension, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels,
        (int)d.Format, (uint32_t)d.Flags);
}

// the D3D12 resources reachable from p through pointers in the first 0x80 bytes of each object (depth levels)
void wp_scan(const void* p, int depth, const std::string& path, std::set<const void*>& seen, std::string& out, int& budget) {
    if (p == nullptr || depth < 0 || budget <= 0 || seen.contains(p) || IsBadReadPtr(p, 0x80)) {
        return;
    }

    seen.insert(p);
    --budget;

    for (uint32_t off = 0; off < 0x80; off += 8) {
        const auto q = *(void* const*)((uintptr_t)p + off);

        if (q == nullptr || ((uintptr_t)q & 7) != 0 || (uintptr_t)q < 0x10000 || IsBadReadPtr(q, 8)) {
            continue;
        }

        const auto vt = *(void* const*)q;

        if (vt != nullptr && !IsBadReadPtr(vt, 8)) {
            auto m = wp_module_of(vt);
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            if (m.find("d3d12") != std::string::npos) {
                ID3D12Resource* r{};

                if (SUCCEEDED(((IUnknown*)q)->QueryInterface(__uuidof(ID3D12Resource), (void**)&r)) && r != nullptr) {
                    out += std::format("      {}+0x{:x} -> ID3D12Resource {} ({}), {}\n", path, off, (void*)r, m, wp_resource_text(r));
                    r->Release();
                }

                continue;
            }
        }

        wp_scan(q, depth - 1, std::format("{}+0x{:x}->", path, off), seen, out, budget);
    }
}

std::string wp_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return v;
}

bool wp_word(const std::string& name) {
    const auto l = wp_lower(name);
    return l.find("whitepoint") != std::string::npos || l.find("histogram") != std::string::npos || l.find("tonemapcb") != std::string::npos ||
           l.find("realev") != std::string::npos || l.find("adapt") != std::string::npos || l.find("luminance") != std::string::npos;
}

// the type with getWhitePointSRV / get_WhitePointSRV and its instance (searched once)
struct WpFound {
    sdk::RETypeDefinition* t{};
    void* instance{};
    std::vector<sdk::REMethodDefinition*> getters{};
};

WpFound& wp_found() {
    static WpFound f = [] {
        WpFound r{};
        const auto tdb = sdk::RETypeDB::get();

        for (uint32_t i = 0; tdb != nullptr && i < tdb->get_num_types(); ++i) {
            auto* t = tdb->get_type(i);

            if (t == nullptr) {
                continue;
            }

            bool has = false;

            for (auto& m : t->get_methods()) {
                if (m.get_name() != nullptr && (std::string_view{m.get_name()} == "getWhitePointSRV" || std::string_view{m.get_name()} == "get_WhitePointSRV")) {
                    has = true;
                }
            }

            if (!has) {
                continue;
            }

            void* inst = t->get_instance();

            if (inst == nullptr) {
                inst = sdk::get_native_singleton<void>(t->get_full_name());
            }

            if (r.t == nullptr || (r.instance == nullptr && inst != nullptr)) {
                r.t = t;
                r.instance = inst;
            }
        }

        if (r.t != nullptr) {
            for (auto& m : r.t->get_methods()) {
                if (m.get_name() != nullptr && m.get_num_params() == 0 && wp_word(m.get_name()) && std::string_view{m.get_name()}.starts_with("get")) {
                    r.getters.push_back(&m);
                }
            }
        }

        return r;
    }();

    return f;
}

// the first 8 bytes a getter returns (a pointer for the ...Ptr structures)
void* wp_call(sdk::REMethodDefinition* m, void* obj) {
    if (m == nullptr) {
        return nullptr;
    }

    auto ret = m->invoke(m->is_static() ? nullptr : obj, std::span<void*>{});
    return ret.exception_thrown ? nullptr : ret.ptr;
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::white_point_report() {
    std::string o = "  [WHITE POINT] the types with members about the white point, the histogram, the adaptation or the tone map constants:\n";
    const auto tdb = sdk::RETypeDB::get();
    int types = 0;

    for (uint32_t i = 0; tdb != nullptr && i < tdb->get_num_types() && types < 60; ++i) {
        auto* t = tdb->get_type(i);

        if (t == nullptr) {
            continue;
        }

        const auto tn = t->get_full_name();

        if (!tn.starts_with("via.") || tn.ends_with("[]")) {
            continue;
        }

        std::string members{};

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || !wp_word(m.get_name())) {
                continue;
            }

            std::string params{};

            for (auto* pt : m.get_param_types()) {
                params += (params.empty() ? "" : ", ") + (pt != nullptr ? pt->get_full_name() : std::string{"?"});
            }

            members += std::format("      {}{}({}) -> {}\n", m.is_static() ? "static " : "", m.get_name(), params,
                m.get_return_type() != nullptr ? m.get_return_type()->get_full_name() : std::string{"?"});
        }

        for (auto* f : t->get_fields()) {
            if (f != nullptr && f->get_name() != nullptr && wp_word(f->get_name())) {
                members += std::format("      field {}{} ({}) offset 0x{:x}\n", f->is_static() ? "static " : "", f->get_name(),
                    f->get_type() != nullptr ? f->get_type()->get_full_name() : std::string{"?"}, f->get_offset_from_base());
            }
        }

        if (!members.empty()) {
            ++types;
            o += std::format("    {}{}\n", tn, t->get_instance() != nullptr ? " (has an instance)" : "") + members;
        }
    }

    auto& f = wp_found();

    if (f.t == nullptr) {
        return o + "    no type with getWhitePointSRV / get_WhitePointSRV was found\n";
    }

    o += std::format("    the white point type: {}, instance {}\n", f.t->get_full_name(), f.instance);

    if (f.instance != nullptr) {
        for (auto* m : f.getters) {
            const auto v = wp_call(m, f.instance);
            o += std::format("    {}() -> {} ({})\n", m->get_name(), v, m->get_return_type() != nullptr ? m->get_return_type()->get_full_name() : "?");

            std::set<const void*> seen{};
            std::string found{};
            int budget = 200;
            wp_scan(v, 3, "", seen, found, budget);
            o += found.empty() ? std::string{"      no D3D12 resource found behind it\n"} : found;

            // where the instance keeps it (the same pointer in its first 0x2000 bytes)
            if (v != nullptr && !IsBadReadPtr(f.instance, 0x2000)) {
                std::string offs{};

                for (uint32_t off = 0; off < 0x2000; off += 8) {
                    if (*(void**)((uintptr_t)f.instance + off) == v) {
                        offs += std::format(" 0x{:x}", off);
                    }
                }

                o += std::format("      kept in the instance at:{}\n", offs.empty() ? " (not in its first 0x2000 bytes)" : offs);
            }
        }
    }

    m_wp_probe_left.store(48);
    o += "    the post effect draws of the next frames follow as [WHITE POINT DRAWS]\n";
    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::white_point_record(sdk::renderer::layer::PostEffect* layer, bool post) {
    if (m_wp_probe_left.load(std::memory_order_relaxed) <= 0) {
        return;
    }

    auto& f = wp_found();
    auto* scene = layer != nullptr ? (sdk::renderer::layer::Scene*)layer->get_parent() : nullptr;
    auto* cam = scene != nullptr ? scene->get_camera() : nullptr;
    auto& dup = VR::get()->get_camera_duplicator();
    const auto clone = cam != nullptr ? dup.get_clone_index(cam) : -1;
    const char* label = cam == nullptr ? "no camera" : clone == 0 ? "clone 0" : clone == 1 ? "clone 1" : clone >= 2 ? "clone 2+"
                      : dup.get_clone_count(cam) > 0 ? "game camera" : "other camera";

    std::string line = std::format("  frame {} {} {} (layer {}, scene {}):", m_frame, post ? "after " : "before", label, (void*)layer, (void*)scene);

    if (f.instance != nullptr) {
        for (auto* m : f.getters) {
            line += std::format(" {} {}", m->get_name(), wp_call(m, f.instance));
        }
    }

    std::scoped_lock _{m_wp_mutex};
    m_wp_text += line + "\n";

    if (m_wp_probe_left.fetch_sub(1) == 1) {
        REVR_LOG(debug_log("[WHITE POINT DRAWS] the white point type's getters at each view's post effect draw:\n" + m_wp_text));
        m_wp_text.clear();
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
bool FoveatedRendering::on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) {
    white_point_record(layer, false);
    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) {
    white_point_record(layer, true);
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::describe_engine_types(int kind) {
    const bool ambient_occlusion = kind == 1;
    std::string o = std::format("[t={:.1f}s] {}\n", std::chrono::duration<double>(Clock::now() - m_start_time).count(),
        kind == 8   ? "[VIEW EFFECTS] the effect components of each view's camera (fog and its sun scattering, light shafts, god rays, volumetric "
                      "fog, bloom, AO, SSR): every getter without parameters in each view, '<- differs' where the views do not agree"
        : kind == 6 ? "[LIGHT SHAFT TYPES] light shafts, god rays, light scattering: the engine's settings, each view's camera and layers, the types "
                      "(components also on the scene's other objects, the lights)"
        : kind == 5 ? "[LOD TYPES] level of detail and culling: the engine's settings, each view's camera and layers, the types (with the cameras' FOV)"
        : kind == 4 ? "[COLOUR CORRECTION] via.render.LDRPostProcess of each view's camera and the objects it holds (colour correction)"
        : kind == 3 ? "[EXPOSURE TYPES] automatic exposure and tone mapping: the engine's settings, the cameras' components, the types"
        : kind == 2 ? "[VRS TYPES] variable rate shading: the GPU, the engine's settings and types"
        : ambient_occlusion ? "[AO TYPES] the engine's ambient occlusion and render settings"
                            : "[SHADOW TYPES] the engine's shadow layers and settings");
    auto* ctx = sdk::get_thread_context();

    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return v;
    };

    // the value of a plain field or getter result, as text ("" for a type it does not print)
    const auto value_text = [](const sdk::RETypeDefinition* t, const void* p) -> std::string {
        if (t == nullptr || p == nullptr) {
            return {};
        }

        const auto n = t->get_full_name();

        if (n == "System.Boolean") return *(const bool*)p ? "true" : "false";
        if (n == "System.Int32") return std::to_string(*(const int32_t*)p);
        if (n == "System.UInt32") return std::to_string(*(const uint32_t*)p);
        if (n == "System.Int16") return std::to_string(*(const int16_t*)p);
        if (n == "System.UInt16") return std::to_string(*(const uint16_t*)p);
        if (n == "System.Byte") return std::to_string(*(const uint8_t*)p);
        if (n == "System.Single") return std::format("{:.4f}", *(const float*)p);
        if (t->is_enum()) return std::to_string(*(const int32_t*)p) + " (enum)";
        return {};
    };

    // (a method without native code, or only a stub, is not called: REFramework's own bindings skip those too)
    const auto getter_value = [&](const sdk::REMethodDefinition& m, void* obj) -> std::string {
        const auto* rt = m.get_return_type();
        auto* fn = (uint8_t*)m.get_function();

        if (rt == nullptr || fn == nullptr || utility::is_stub_code(fn)) {
            return {};
        }

        const auto n = rt->get_full_name();

        // (call_safe passes an exception of the game on as std::runtime_error: one getter must not end the whole list)
        try {
            if (n == "System.Boolean") {
                return obj != nullptr ? (m.call_safe<bool>(ctx, obj) ? "true" : "false") : (m.call_safe<bool>(ctx) ? "true" : "false");
            }

            if (n == "System.Int32" || n == "System.UInt32" || rt->is_enum()) {
                const int32_t v = obj != nullptr ? m.call_safe<int32_t>(ctx, obj) : m.call_safe<int32_t>(ctx);
                return std::to_string(v) + (rt->is_enum() ? " (enum)" : "");
            }

            if (n == "System.Single") {
                const float v = obj != nullptr ? m.call_safe<float>(ctx, obj) : m.call_safe<float>(ctx);
                return std::format("{:.4f}", v);
            }
        } catch (const std::exception&) {
            return "(threw)";
        }

        return {};
    };

    // [LOD 06.11.2026 evening] a getter returning a structure (via.Float2, ...) or an object: the structure's fields from the
    // returned bytes; for an object the plain values of its own getters and fields (one level, at most 24)
    const auto invoke_value = [&](const sdk::REMethodDefinition& m, void* obj) -> std::string {
        const auto* rt = m.get_return_type();
        auto* fn = (uint8_t*)m.get_function();

        if (rt == nullptr || fn == nullptr || utility::is_stub_code(fn) || rt->is_enum() || rt->is_primitive()) {
            return {};
        }

        const auto n = rt->get_full_name();

        if (n == "System.String" || n == "System.Void") {
            return {};
        }

        try {
            const auto r = m.invoke(obj, std::span<void*>{});

            if (r.exception_thrown) {
                return "(threw)";
            }

            std::string text{};

            if (rt->is_value_type()) {
                for (auto* f : rt->get_fields()) {
                    if (f == nullptr || f->is_static() || f->get_name() == nullptr) {
                        continue;
                    }

                    const auto v = value_text(f->get_type(), f->get_data_raw((void*)r.bytes.data(), true));
                    text += std::format("{}{}={}", text.empty() ? "{" : ", ", f->get_name(), v.empty() ? "?" : v);
                }

                return text.empty() ? std::string{} : text + "}";
            }

            if (r.ptr == nullptr) {
                return "null";
            }

            int count = 0;

            for (auto* f : rt->get_fields()) {
                if (f == nullptr || f->is_static() || f->get_name() == nullptr || count >= 24) {
                    continue;
                }

                if (const auto v = value_text(f->get_type(), f->get_data_raw(r.ptr)); !v.empty()) {
                    text += std::format("{}{}={}", text.empty() ? "{" : ", ", f->get_name(), v);
                    ++count;
                }
            }

            for (auto& gm : rt->get_methods()) {
                if (gm.get_name() == nullptr || gm.is_static() || gm.get_num_params() != 0 || count >= 24) {
                    continue;
                }

                const std::string gname = gm.get_name();

                if (!gname.starts_with("get_")) {
                    continue;
                }

                if (const auto v = getter_value(gm, r.ptr); !v.empty()) {
                    text += std::format("{}{}={}", text.empty() ? "{" : ", ", gname.substr(4), v);
                    ++count;
                }
            }

            return "object " + (text.empty() ? std::string{"{}"} : text + "}");
        } catch (const std::exception&) {
            return "(threw)";
        }
    };

    // the layers of the wide view and of the fovea views, by type name (first of each type, depth first)
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    std::vector<std::map<std::string, sdk::renderer::RenderLayer*>> layer_of(3);

    for (size_t v = 0; v < 3 && v < scenes.size(); ++v) {
        std::vector<LayerNode> t{};
        collect_layers(scenes[v], 0, t, 600);

        for (const auto& n : t) {
            if (!layer_of[v].contains(n.type)) {
                layer_of[v][n.type] = (sdk::renderer::RenderLayer*)n.ptr;
            }
        }
    }

    // one type: its parents, fields and methods; the values of plain fields and of getters without parameters in each object.
    // [VRS 01.11.2026] only: just the members whose name holds one of the words (skip_empty: nothing at all without one)
    // call_static false: static getters are not called (types of which nothing is known)
    const auto describe_type = [&](const char* name, const std::vector<std::pair<const char*, void*>>& objs, const std::vector<std::string>* only = nullptr,
                                   bool skip_empty = false, bool call_static = true) {
        const auto t = sdk::find_type_definition(name);

        if (t == nullptr) {
            if (!skip_empty) {
                o += std::format("  {}: type not found\n", name);
            }

            return;
        }

        const auto wanted = [&](const char* member) {
            if (only == nullptr) {
                return true;
            }

            const auto l = lower(member);
            return std::any_of(only->begin(), only->end(), [&](const std::string& w) { return l.find(w) != std::string::npos; });
        };

        std::string members{};

        std::string parents{};

        for (auto* p = t->get_parent_type(); p != nullptr && parents.size() < 400; p = p->get_parent_type()) {
            parents += " < " + p->get_full_name();
        }

        std::string found{};

        for (const auto& [label, obj] : objs) {
            found += std::format("{}{}: {}", found.empty() ? "" : ", ", label, obj != nullptr ? "found" : "none");
        }

        for (auto* f : t->get_fields()) {
            if (f == nullptr || f->get_name() == nullptr || !wanted(f->get_name())) {
                continue;
            }

            const auto* ft = f->get_type();
            std::string vals{};

            if (!f->is_static()) {
                for (const auto& [label, obj] : objs) {
                    if (obj != nullptr) {
                        if (const auto text = value_text(ft, f->get_data_raw(obj)); !text.empty()) {
                            vals += std::format(" | {} = {}", label, text);
                        }
                    }
                }
            }

            members += std::format("    field {}{}: {}{}\n", f->is_static() ? "static " : "", f->get_name(), ft != nullptr ? ft->get_full_name() : std::string{"?"}, vals);
        }

        for (auto& m : t->get_methods()) {
            if (m.get_name() == nullptr || !wanted(m.get_name())) {
                continue;
            }

            const std::string mname = m.get_name();
            std::string params{};

            for (auto* pt : m.get_param_types()) {
                params += (params.empty() ? "" : ", ") + (pt != nullptr ? pt->get_full_name() : std::string{"?"});
            }

            std::string vals{};

            if (mname.starts_with("get_") && m.get_num_params() == 0) {
                // (a structure or an object only with the level of detail dump: [LOD 06.11.2026 evening])
                const auto value_of = [&](void* obj) {
                    auto text = getter_value(m, obj);
                    return text.empty() && kind == 5 ? invoke_value(m, obj) : text;
                };

                if (m.is_static()) {
                    if (const auto text = call_static ? value_of(nullptr) : std::string{}; !text.empty()) {
                        vals = " = " + text;
                    }
                } else {
                    for (const auto& [label, obj] : objs) {
                        if (obj != nullptr) {
                            if (const auto text = value_of(obj); !text.empty()) {
                                vals += std::format(" | {} = {}", label, text);
                            }
                        }
                    }
                }
            }

            members += std::format("    method {}{}({}) -> {}{}\n", m.is_static() ? "static " : "", mname, params,
                m.get_return_type() != nullptr ? m.get_return_type()->get_full_name() : std::string{"?"}, vals);
        }

        if (skip_empty && members.empty()) {
            return;
        }

        o += std::format("  {}{} ({}){}\n", name, parents, found, only != nullptr ? " - only the members about it" : "");
        o += members.empty() ? std::string{"    (no member about it)\n"} : members;
    };

    // a component of each view's camera (the camera clones get copies of the game camera's)
    const auto camera_components = [&](const char* type_name) {
        std::vector<std::pair<const char*, void*>> cams{};
        const char* cam_labels[] = {"wide view camera", "left fovea camera", "right fovea camera", "camera 4"};

        for (size_t v = 0; v < scenes.size() && v < 4; ++v) {
            auto* cam = scenes[v] != nullptr ? scenes[v]->get_camera() : nullptr;
            void* comp = nullptr;

            if (cam != nullptr && cam->ownerGameObject != nullptr && cam->ownerGameObject->transform != nullptr) {
                comp = utility::re_component::find<REComponent>(cam->ownerGameObject->transform, type_name);
            }

            cams.emplace_back(cam_labels[v], comp);
        }

        return cams;
    };

    // every via.render type whose name holds one of the words; enums with their values
    const auto list_types = [&](std::initializer_list<const char*> words) {
        const auto tdb = sdk::RETypeDB::get();

        if (tdb == nullptr) {
            return;
        }

        std::string names{};
        int count = 0;

        for (uint32_t i = 0; i < tdb->get_num_types() && count < 200; ++i) {
            const auto t = tdb->get_type(i);

            if (t == nullptr) {
                continue;
            }

            const auto n = t->get_full_name();
            const auto l = lower(n);

            if (!n.starts_with("via.render") || n.ends_with("[]")) {
                continue;
            }

            bool hit = false;

            for (const char* w : words) {
                hit = hit || l.find(w) != std::string::npos;
            }

            if (!hit) {
                continue;
            }

            ++count;
            names += "    " + n;

            if (t->is_enum()) {
                std::string values{};

                for (auto* f : t->get_fields()) {
                    if (f == nullptr || f->get_name() == nullptr || !f->is_static() || std::string_view{f->get_name()} == "value__") {
                        continue;
                    }

                    values += (values.empty() ? "" : ", ") + std::string{f->get_name()};

                    if (f->is_literal()) {
                        if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                            values += std::format("={}", *d);
                        }
                    }
                }

                names += " (enum: " + values + ")";
            }

            names += "\n";
        }

        o += std::format("  via.render types about it ({}):\n", count) + names;
    };

    // [EXPOSURE 01.11.2026] kind 3: the tone mapping component of each view's camera (each view has its own: the clones get
    // copies of the game camera's components, the get_/set_ properties copied every few frames by CameraDuplicator).
    // [COLOUR CORRECTION 02.11.2026] kind 4: the same for via.render.LDRPostProcess and the objects its getters return
    // (the colour correction): the engine effect test of 02.11 found the fovea / periphery difference 10.6 % -> 5.8 % with
    // the colour correction off. Every getter without parameters (get_X and getX) with each view's value, structures field
    // by field, enums with their names; "<- differs" where the views do not agree.
    if (kind == 3 || kind == 4 || kind == 8) {
        static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");

        if (kind == 3 && renderer_t != nullptr) {
            if (auto* m = renderer_t->get_method("get_AutoExposureHistogramMode"); m != nullptr) {
                o += std::format("  via.render.Renderer.AutoExposureHistogramMode = {}\n", getter_value(*m, nullptr));
            }
        }

        const auto enum_names = [](const sdk::RETypeDefinition* t) {
            std::string values{};

            for (auto* f : t->get_fields()) {
                if (f == nullptr || f->get_name() == nullptr || !f->is_static() || std::string_view{f->get_name()} == "value__") {
                    continue;
                }

                values += (values.empty() ? "" : ", ") + std::string{f->get_name()};

                if (f->is_literal()) {
                    if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                        values += std::format("={}", *d);
                    }
                }
            }

            return values;
        };

        std::set<std::string> enums{};

        // the getters of one type in each view's object; objects returned by a getter are described below it (depth 1)
        std::function<void(const sdk::RETypeDefinition*, const std::vector<std::pair<const char*, void*>>&, const std::string&, int)> dump{};
        dump = [&](const sdk::RETypeDefinition* t, const std::vector<std::pair<const char*, void*>>& objs, const std::string& indent, int depth) {
            for (auto& m : t->get_methods()) {
                if (m.get_name() == nullptr || m.get_num_params() != 0 || m.is_static()) {
                    continue;
                }

                const std::string mname = m.get_name();

                if (!mname.starts_with("get") && !mname.starts_with("is")) {
                    continue;
                }

                const auto* rt = m.get_return_type();
                auto* fn = (uint8_t*)m.get_function();

                if (rt == nullptr || fn == nullptr || utility::is_stub_code(fn)) {
                    continue;
                }

                std::string vals{};
                std::vector<std::string> texts{};
                std::vector<std::pair<const char*, void*>> children{};

                for (const auto& [label, obj] : objs) {
                    if (obj == nullptr) {
                        continue;
                    }

                    std::string text = getter_value(m, obj);

                    if (text.empty()) {
                        try {
                            const auto r = m.invoke(obj, std::span<void*>{});

                            if (r.exception_thrown) {
                                text = "(threw)";
                            } else if (rt->is_value_type() && !rt->is_primitive() && !rt->is_enum()) {
                                // a structure: its fields from the returned bytes
                                for (auto* f : rt->get_fields()) {
                                    if (f == nullptr || f->is_static() || f->get_name() == nullptr) {
                                        continue;
                                    }

                                    const auto v = value_text(f->get_type(), f->get_data_raw((void*)r.bytes.data(), true));
                                    text += std::format("{}{}={}", text.empty() ? "{" : ", ", f->get_name(), v.empty() ? "?" : v);

                                    if (f->get_type() != nullptr && f->get_type()->is_enum()) {
                                        enums.insert(f->get_type()->get_full_name());
                                    }
                                }

                                text += text.empty() ? "{}" : "}";
                            } else if (!rt->is_value_type()) {
                                // an object: described below when it is of a via.render type
                                text = r.ptr != nullptr ? "object" : "null";
                                children.emplace_back(label, r.ptr);
                            }
                        } catch (const std::exception&) {
                            text = "(threw)";
                        }
                    }

                    if (!text.empty()) {
                        vals += std::format(" | {} = {}", label, text);
                        texts.push_back(text);
                    }
                }

                if (rt->is_enum()) {
                    enums.insert(rt->get_full_name());
                }

                const bool differs = texts.size() > 1 && std::any_of(texts.begin(), texts.end(), [&](const std::string& x) { return x != texts[0]; }) &&
                                     !std::all_of(texts.begin(), texts.end(), [](const std::string& x) { return x == "object"; });
                o += std::format("{}{}() -> {}{}{}\n", indent, mname, rt->get_full_name(), vals, differs ? "  <- differs" : "");

                // the objects of this getter (the colour correction, its curves, ...), one level down
                const auto rname = rt->get_full_name();

                if (depth < 1 && !children.empty() && rname.starts_with("via.render") &&
                    std::any_of(children.begin(), children.end(), [](const auto& c) { return c.second != nullptr; })) {
                    const sdk::RETypeDefinition* ct = rt;

                    for (const auto& c : children) {
                        if (c.second != nullptr) {
                            ct = utility::re_managed_object::get_type_definition((::REManagedObject*)c.second);
                            break;
                        }
                    }

                    if (ct != nullptr) {
                        o += std::format("{}  {} ({}):\n", indent, mname, ct->get_full_name());
                        dump(ct, children, indent + "    ", depth + 1);
                    }
                }
            }
        };

        // [VIEW EFFECTS 13.11.2026] RE7 test of fix45 (the fovea views with the game camera's real exposure): the brightness
        // the same where the sun is not near (the house: periphery = 1.006 x fovea), but facing the gate with the sun above it the
        // periphery up to 3.5x brighter at the top of the fovea, falling off smoothly downwards: a glow toward the sun that the
        // fovea views do not have (the game's bloom, lens flares and volumetrics are off in that config, its god rays on).
        if (kind == 8) {
            static const char* effect_types[] = {"via.render.Fog", "via.render.LightShaftFilterControl", "via.render.GodRay", "via.render.VolumetricFog",
                "via.render.VolumetricFogControl", "via.render.SoftBloom", "via.render.FFTBloom", "via.render.SSAOControl", "via.render.GeometryAOControl",
                "via.render.SSRControl", "via.render.LDRPostProcess"};

            for (const char* en : effect_types) {
                const auto et = sdk::find_type_definition(en);

                if (et == nullptr) {
                    o += std::format("  {}: type not found\n", en);
                    continue;
                }

                const auto ecomps = camera_components(en);
                std::string efound{};

                for (const auto& [label, obj] : ecomps) {
                    efound += std::format("{}{}: {}", efound.empty() ? "" : ", ", label, obj != nullptr ? "found" : "none");
                }

                o += std::format("  {} on the view cameras ({})\n", en, efound);
                dump(et, ecomps, "    ", 0);
            }

            for (const auto& n : enums) {
                if (const auto et = sdk::find_type_definition(n); et != nullptr) {
                    o += std::format("  {} (enum: {})\n", n, enum_names(et));
                }
            }

            return o;
        }

        const char* type_name = kind == 3 ? "via.render.ToneMapping" : "via.render.LDRPostProcess";
        const auto t = sdk::find_type_definition(type_name);

        if (t == nullptr) {
            return o + std::format("  {}: type not found\n", type_name);
        }

        const auto comps = camera_components(type_name);
        std::string found{};

        for (const auto& [label, obj] : comps) {
            found += std::format("{}{}: {}", found.empty() ? "" : ", ", label, obj != nullptr ? "found" : "none");
        }

        o += std::format("  {} on the view cameras ({})\n", type_name, found);
        dump(t, comps, "    ", 0);

        // [EXPOSURE MEMBERS 13.11.2026] every method of the type with its parameters (RE7: UseAutoExposure has no setter;
        // another way to switch the automatic exposure off or to read the adapted exposure may exist)
        if (kind == 3) {
            o += "  all methods of the type:\n";

            for (const auto* tt = t; tt != nullptr && tt->get_full_name() != "System.Object"; tt = tt->get_parent_type()) {
                for (auto& m : tt->get_methods()) {
                    if (m.get_name() == nullptr) {
                        continue;
                    }

                    std::string params{};

                    for (auto* pt : m.get_param_types()) {
                        params += (params.empty() ? "" : ", ") + (pt != nullptr ? pt->get_full_name() : std::string{"?"});
                    }

                    o += std::format("    {}{}::{}({}) -> {}\n", m.is_static() ? "static " : "", tt->get_full_name(), m.get_name(), params,
                        m.get_return_type() != nullptr ? m.get_return_type()->get_full_name() : std::string{"?"});
                }

                for (auto* f : tt->get_fields()) {
                    if (f != nullptr && f->get_name() != nullptr) {
                        o += std::format("    field {}::{} ({})\n", tt->get_full_name(), f->get_name(),
                            f->get_type() != nullptr ? f->get_type()->get_full_name() : std::string{"?"});
                    }
                }
            }
        }

        if (kind == 3) {
            o += white_point_report(); // [WHITE POINT 13.11.2026]
        }

        for (const auto& n : enums) {
            if (const auto et = sdk::find_type_definition(n); et != nullptr) {
                o += std::format("  {} (enum: {})\n", n, enum_names(et));
            }
        }

        return o;
    }

    // [LOD 06.11.2026] kind 5: the same search for level of detail and culling (and the cameras' field of view: the engine may
    // choose an object's detail by how large it looks in a view, and the fovea views see a small part of the eye's view)
    // [LIGHT SHAFT 13.11.2026] kind 6: the same search for light shafts / god rays / scattering. RE7 multipass dump of
    // 13.11.2026 (the right eye's exposure range locked: no change): the game camera's eye has the haze and the glow of
    // the sun over the sky and the trees, the clone's eye not (the clouds visible, the shadows darker).
    if (kind == 2 || kind == 5 || kind == 6) {
        // the GPU (the device the game renders with)
        auto& hook = g_framework->get_d3d12_hook();

        if (kind == 5 || kind == 6) {
            // (no device part)
        } else if (auto* device = hook != nullptr ? hook->get_device() : nullptr; device != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};

            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
                o += std::format("  device: variable rate shading tier {} (0 none, 1 per draw, 2 also a shading rate image and per primitive), shading rate "
                                 "image tile {} px, rates 2x4 / 4x2 / 4x4 {}, per primitive with a viewport index {}\n",
                    (int)o6.VariableShadingRateTier, o6.ShadingRateImageTileSize, o6.AdditionalShadingRatesSupported ? "yes" : "no",
                    o6.PerPrimitiveShadingRateSupportedWithViewportIndexing ? "yes" : "no");
            } else {
                o += "  device: no answer to D3D12_OPTIONS6 (variable rate shading unknown to this runtime)\n";
            }
        } else {
            o += "  device: not available\n";
        }

        const std::vector<std::string> words = kind == 6   ? std::vector<std::string>{"lightshaft", "godray", "shaft", "scatter", "haze", "sunlight", "glare",
                                                                 "volumetricfog"}
                                             : kind == 5 ? std::vector<std::string>{"lod", "levelofdetail", "cull", "meshquality", "drawdistance", "viewdistance", "fov"}
                                                         : std::vector<std::string>{"vrs", "shadingrate", "shading_rate", "variablerate", "variable_rate",
                                                               "variableshading", "foveat"};
        const auto has_word = [&](const std::string& name) {
            const auto l = lower(name);
            return std::any_of(words.begin(), words.end(), [&](const std::string& w) { return l.find(w) != std::string::npos; });
        };

        // the game's render settings, the renderer, the cameras and the scene layers of the three views: the members about it
        static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");
        static const auto get_render_config = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;
        ::REManagedObject* config = nullptr;
        void* renderer = renderer_t != nullptr ? renderer_t->get_instance() : nullptr;

        if (renderer != nullptr && get_render_config != nullptr && get_render_config->get_function() != nullptr) {
            try {
                config = get_render_config->call_safe<::REManagedObject*>(ctx, renderer);
            } catch (const std::exception&) {
                config = nullptr;
            }
        }

        const char* view_labels[] = {"wide view", "left fovea", "right fovea"};
        std::vector<std::pair<const char*, void*>> cams{};
        std::vector<std::pair<const char*, void*>> scene_objs{};

        for (size_t v = 0; v < 3; ++v) {
            auto* scene = v < scenes.size() ? scenes[v] : nullptr;
            cams.emplace_back(view_labels[v], scene != nullptr ? (void*)scene->get_camera() : nullptr);
            scene_objs.emplace_back(view_labels[v], (void*)scene);
        }

        describe_type("via.render.RenderConfig", {{"render config", config}}, &words);
        describe_type("via.render.Renderer", {{"renderer", renderer}}, &words);
        describe_type("via.Camera", cams, &words);
        describe_type("via.render.layer.Scene", scene_objs, &words);

        if (kind == 5) {
            // [LOD 06.11.2026 evening] the type of the renderer's level of detail settings (get_LodSettings) in full
            describe_type("via.render.GlobalLodSettings", {}, nullptr, false, false);
        }

        // every layer type in the three views' trees that has a member about it (with each view's layer of that type)
        std::set<std::string> layer_types{};

        for (const auto& m : layer_of) {
            for (const auto& [type_name, layer] : m) {
                layer_types.insert(type_name);
            }
        }

        o += std::format("  the {} layer types of the three views, those with a member about it:\n", layer_types.size());

        for (const auto& type_name : layer_types) {
            std::vector<std::pair<const char*, void*>> objs{};

            for (size_t v = 0; v < 3; ++v) {
                objs.emplace_back(view_labels[v], layer_of[v].contains(type_name) ? (void*)layer_of[v][type_name] : nullptr);
            }

            describe_type(type_name.c_str(), objs, &words, true);
        }

        // every type of the engine named after it: in full, with the views' objects when it is a layer or a camera component
        const auto tdb = sdk::RETypeDB::get();
        std::vector<std::string> named{};
        std::string member_list{};
        int member_count = 0;

        if (tdb != nullptr) {
            for (uint32_t i = 0; i < tdb->get_num_types(); ++i) {
                const auto t = tdb->get_type(i);

                if (t == nullptr) {
                    continue;
                }

                const auto n = t->get_full_name();

                if (n.ends_with("[]")) {
                    continue;
                }

                // (not the instances of generic types: [LOD 06.11.2026 evening] they took most of the 60 places)
                if (has_word(n) && named.size() < 60 && n.find('<') == std::string::npos && n.find('`') == std::string::npos) {
                    named.push_back(n);
                }

                // members named after it in any type (not in the instances of generic types: the same members again)
                if (member_count >= 300 || n.find('<') != std::string::npos) {
                    continue;
                }

                for (auto* f : t->get_fields()) {
                    if (f != nullptr && f->get_name() != nullptr && has_word(f->get_name()) && member_count < 300) {
                        member_list += std::format("    {}::{} (field: {})\n", n, f->get_name(), f->get_type() != nullptr ? f->get_type()->get_full_name() : std::string{"?"});
                        ++member_count;
                    }
                }

                for (auto& m : t->get_methods()) {
                    if (m.get_name() != nullptr && has_word(m.get_name()) && member_count < 300) {
                        member_list += std::format("    {}::{}({} parameters) -> {}\n", n, m.get_name(), m.get_num_params(),
                            m.get_return_type() != nullptr ? m.get_return_type()->get_full_name() : std::string{"?"});
                        ++member_count;
                    }
                }
            }
        }

        o += std::format("  types named after it ({}{}):\n", named.size(), named.size() >= 60 ? ", the first 60" : "");

        for (const auto& n : named) {
            const auto t = sdk::find_type_definition(n);

            if (t != nullptr && t->is_enum()) {
                std::string values{};

                for (auto* f : t->get_fields()) {
                    if (f == nullptr || f->get_name() == nullptr || !f->is_static() || std::string_view{f->get_name()} == "value__") {
                        continue;
                    }

                    values += (values.empty() ? "" : ", ") + std::string{f->get_name()};

                    if (f->is_literal()) {
                        if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                            values += std::format("={}", *d);
                        }
                    }
                }

                o += std::format("  {} (enum: {})\n", n, values);
                continue;
            }

            std::vector<std::pair<const char*, void*>> objs{};

            // [LIGHT SHAFT 13.11.2026] (kind 6) also up to four of the scene's components of the type (lights, volumes)
            std::vector<std::string> scene_labels{};

            if (t != nullptr && t->is_a("via.Component")) {
                objs = camera_components(n.c_str());

                if (kind == 6 && t->get_runtime_type() != nullptr) {
                    if (auto* scene = sdk::get_current_scene(); scene != nullptr) {
                        auto* found = sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", t->get_runtime_type());
                        const int count = found != nullptr ? found->size() : 0;
                        scene_labels.reserve(4);

                        for (int k = 0; k < count && k < 4; ++k) {
                            auto* c = (REComponent*)found->get_element(k);
                            auto* go = c != nullptr ? utility::re_component::get_game_object(c) : nullptr;
                            scene_labels.push_back(std::format("scene {} of {} ({})", k, count, go != nullptr ? utility::re_string::get_string(go->name) : std::string{"?"}));
                            objs.emplace_back(scene_labels.back().c_str(), (void*)c);
                        }
                    }
                }
            } else {
                for (size_t v = 0; v < 3; ++v) {
                    if (layer_of[v].contains(n)) {
                        objs.emplace_back(view_labels[v], (void*)layer_of[v][n]);
                    }
                }
            }

            describe_type(n.c_str(), objs, nullptr, false, false);
        }

        o += std::format("  members of any type named after it ({}{}):\n", member_count, member_count >= 300 ? ", the first 300" : "") + member_list;
        return o;
    }

    if (ambient_occlusion) {
        // the game's render settings: all of them (the ambient occlusion choice is one of them)
        static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");
        static const auto get_render_config = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;
        ::REManagedObject* config = nullptr;

        if (renderer_t != nullptr && get_render_config != nullptr && get_render_config->get_function() != nullptr) {
            try {
                config = get_render_config->call_safe<::REManagedObject*>(ctx, renderer_t->get_instance());
            } catch (const std::exception&) {
                config = nullptr;
            }
        }

        describe_type("via.render.RenderConfig", {{"render config", config}});
        describe_type("via.render.SSAOControl", camera_components("via.render.SSAOControl"));
        describe_type("via.render.GeometryAOControl", camera_components("via.render.GeometryAOControl"));
        list_types({"ambientocclusion", "ssao", "cacao", "hbao", "aocontrol", "occlusion", "geometryao"});
        return o;
    }

    const char* layer_types[] = {"via.render.layer.SceneDependentShadowCast", "via.render.layer.ShadowCast", "via.render.layer.PostSceneDependentShadowCast",
        "via.render.layer.ShadowCastJunction", "via.render.layer.DeferredLighting", "via.render.layer.PreDeferredLighting", "via.render.layer.Visibility",
        "via.render.layer.PreZforCull"};

    for (const char* name : layer_types) {
        describe_type(name, {{"wide view", layer_of[0].contains(name) ? layer_of[0][name] : nullptr},
                                {"left fovea", layer_of.size() > 1 && layer_of[1].contains(name) ? layer_of[1][name] : nullptr}});
    }

    // the shadow quality of each view's camera
    describe_type("via.render.ShadowQualityControl", camera_components("via.render.ShadowQualityControl"));

    // via.render.Renderer: shadows, cascades and anything that looks like a GPU time
    if (const auto r = sdk::find_type_definition("via.render.Renderer"); r != nullptr) {
        o += "  via.render.Renderer (methods about shadows, cascades, GPU time)\n";

        for (auto& m : r->get_methods()) {
            if (m.get_name() == nullptr) {
                continue;
            }

            const std::string mname = m.get_name();
            const auto l = lower(mname);

            if (l.find("shadow") == std::string::npos && l.find("cascade") == std::string::npos && l.find("gpu") == std::string::npos &&
                l.find("time") == std::string::npos) {
                continue;
            }

            std::string params{};

            for (auto* pt : m.get_param_types()) {
                params += (params.empty() ? "" : ", ") + (pt != nullptr ? pt->get_full_name() : std::string{"?"});
            }

            std::string val{};

            if (mname.starts_with("get_") && m.get_num_params() == 0 && m.is_static()) {
                if (const auto text = getter_value(m, nullptr); !text.empty()) {
                    val = " = " + text;
                }
            }

            o += std::format("    method {}{}({}) -> {}{}\n", m.is_static() ? "static " : "", mname, params,
                m.get_return_type() != nullptr ? m.get_return_type()->get_full_name() : std::string{"?"}, val);
        }
    }

    // every via.render type about shadows
    if (const auto tdb = sdk::RETypeDB::get(); tdb != nullptr) {
        std::string names{};
        int count = 0;

        for (uint32_t i = 0; i < tdb->get_num_types() && count < 200; ++i) {
            const auto t = tdb->get_type(i);

            if (t == nullptr) {
                continue;
            }

            const auto n = t->get_full_name();

            if (n.starts_with("via.render") && lower(n).find("shadow") != std::string::npos) {
                names += "    " + n + "\n";
                ++count;
            }
        }

        o += std::format("  via.render types with \"shadow\" in the name ({}):\n", count) + names;
    }

    return o;
}
#endif

// =================================================================================================
// [ENGINE FEATURES 30.10.2026] What each of the engine's effects costs in all views: phases with everything as the game has
// it and phases with one effect switched off (its components' Enabled false on every instance in the scene: the camera of
// each view has its own copy), alternating. The cost of an effect = the mean of the phases around it minus its phase.
// The image changes during an effect's phase (measurement only); every component gets its own value back after it. An
// effect whose components are all off already (or not there) is left out. All on the game thread.
// =================================================================================================
// =================================================================================================
// [CPU TEST 03.11.2026] What the parts of the render cost the CPU and the GPU in a fight: 15 s phases of normal rendering
// and of one part switched off, normal phases in between. Each phase closes the periodic status line first, so every line
// belongs to one phase (the label is in its main thread part); the threads' line follows each status line. The image
// changes in the test phases (measurement only); everything is put back after each phase. All on the game thread.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
// 0 normal, 1 the fovea views almost empty (their frustum 2 % of its size: their objects are gone), 2 the fovea views
// without dynamic shadows (their shadow cast layers draw only the cached static shadows), 3 no dynamic shadows in any view.
// Since the test of 03.11.2026 20:50 (no difference with either shadow phase): shorter phases in turn, so that the pairs
// compare the same moments of a fight; a frame timeline in the middle of the first two. 4: REFramework's ImGui frame at
// BeginRendering as before 04.11.2026 (the other phases: at WaitRendering). Since 04.11.2026: that comparison. 5: the fovea
// views' overlay layer update (the interface of the view, 1.2-1.5 ms per view on the workers inside BeginRendering in the
// fight timeline of 04.11.2026, ~1.75 ms of the main thread's path) not run: what it costs on that path and what it is
// needed for (whatever goes missing in the fovea in those phases).
constexpr int CTEST_PHASES[] = {0, 1, 0, 1, 0, 1, 0}; // [04.11.2026 19:30] kind 5 is the default setting since then
constexpr int CTEST_PHASE_COUNT = (int)(sizeof(CTEST_PHASES) / sizeof(CTEST_PHASES[0]));
// [GPU TEST 06.11.2026] the GPU test in a fight (with the GPU timeline of the frame): 6 the fovea views without the engine's
// occlusion culling, 7 no view with it (the culling only leaves out objects hidden behind others: the image stays the same,
// the GPU time can go either way), 1 for comparison: what the objects in the fovea views cost
// [GPU TEST 06.11.2026 evening] the test of 18:30 settled those (the culling saves 0.5-1.1 ms; the objects in the fovea views
// cost ~1.8 ms) and showed the game's queue idle 1-3 ms per frame right before REFramework's pass at the present, with the next
// frame's work waiting behind it. Now: 8 that pass without the game window part (it draws the desktop interface into the game
// window's back buffer), 9 without the whole pass (the VR menu's texture too), 10 the engine's level of detail off (every
// object at its full detail): how much the level of detail saves in the wide view and in the fovea views
// [GPU TEST 06.11.2026 19:35] the test of 19:25 settled those (the queue waits for SteamVR's frame after PostPresentHandoff;
// REFramework's pass and the level of detail do not matter; 8 and 9 are gone). Now the parallel work: 11 a test load (like
// the mod's eye passes, written to images of its own) on the game's queue after the wide view, 12 the same load on a second
// queue in parallel with the fovea views (the game's queue waits for it before the mod's pre-fovea pass)
// [GPU TEST 06.11.2026 21:30] the parallel test of 20:22 settled that (the second queue slows the fovea views by more than its
// own work: 11 and 12 and d3d12::ParallelLoad are gone). Now 13: the fovea cameras' field of view for the engine narrowed to
// what holds the fovea (the image comes from the mod's projection), to see whether the engine then sends fewer draws to the
// fovea views (RGP of 21:01: as many with a frustum of 2 % as normally) and what else it changes
// [GPU TEST 06.11.2026 22:30] 13 settled that too (the engine renders the same with 106 or ~65 degrees). Now the places where the
// game's queue waits for the engine's compute work inside the fovea views (~0.33 ms, twice a frame): a test load (like the
// mod's eye passes, written to images of its own) right before each of the two waits, small (~0.15 ms each, 14) and large
// (~0.30 ms each, 15), against both large loads where the queue does not wait (after the wide view's first pass, 16): what
// fits into the waits can be moved there (the mod's passes that only need the wide view). 07.11: the load right after the
// engine's submit came before its signal to the compute queue (the compute work then waited for it): now right before the
// engine's wait (d3d12::SubmitTimeline::before_wait)
constexpr int GTEST_PHASES[] = {0, 14, 0, 15, 0, 16, 0, 14, 0, 15, 0, 16, 0};
constexpr int GTEST_PHASE_COUNT = (int)(sizeof(GTEST_PHASES) / sizeof(GTEST_PHASES[0]));
constexpr double CTEST_PHASE_SECONDS = 10.0;
constexpr double CTEST_TIMELINE_AT_SECONDS = 5.0;
constexpr double CTEST_COUNTDOWN_SECONDS = 5.0;
constexpr const char* CTEST_NAMES[] = {"normal", "fovea views almost empty", "fovea views without dynamic shadows", "no dynamic shadows in any view",
    "ImGui frame at BeginRendering (as before)", "fovea views without their interface update", "fovea views without occlusion culling",
    "no view with occlusion culling", "REFramework's pass without the game window (until 06.11 19:35)",
    "no REFramework pass at the present (until 06.11 19:35)", "objects at full detail (level of detail off)",
    "test load on the game's queue (until 06.11 21:30)", "test load on a second queue (until 06.11 21:30)",
    "fovea cameras with a narrow field of view (until 06.11 22:30)", "small test loads in the fovea views' waits",
    "large test loads in the fovea views' waits", "large test loads where the queue does not wait"};
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
std::string FoveatedRendering::cpu_test_label() const {
    if (m_ctest_running && m_ctest_phase >= 0) {
        return std::format(" [{} TEST {}/{}: {}]", m_ctest_gpu ? "GPU" : "CPU", m_ctest_phase + 1, m_ctest_count, CTEST_NAMES[m_ctest_kind]);
    }

    if (quadview::State::get().cpu_probe_tiny_fovea.load()) {
        return " [CPU TEST: fovea views almost empty]";
    }

    return {};
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// The fovea views' shadow cast layers (four per view): off = DynamicShadowingEnable false on each (the value found is kept),
// on = the kept values back on the layers that still exist (a layer is only touched while it is in the views' layer tree)
bool FoveatedRendering::set_fovea_dynamic_shadows(bool on) {
    static const auto t = sdk::find_type_definition("via.render.layer.ShadowCast");
    static const auto get_m = t != nullptr ? t->get_method("get_DynamicShadowingEnable") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_DynamicShadowingEnable") : nullptr;

    // (a method without native code, or only a stub, is not called)
    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    if (!callable(set_m)) {
        return false;
    }

    auto* ctx = sdk::get_thread_context();
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    std::vector<const void*> present{};

    for (size_t v = 1; v < scenes.size() && v < 3; ++v) {
        std::vector<LayerNode> nodes{};
        collect_layers(scenes[v], 0, nodes, 600);

        for (const auto& n : nodes) {
            if (n.type == "via.render.layer.ShadowCast" && n.ptr != nullptr) {
                present.push_back(n.ptr);
            }
        }
    }

    int changed = 0;

    if (!on) {
        for (const void* layer : present) {
            const auto known = std::find_if(m_fovea_shadow_original.begin(), m_fovea_shadow_original.end(), [layer](const auto& e) { return e.first == layer; });

            if (known == m_fovea_shadow_original.end()) {
                bool original = true;

                if (callable(get_m)) {
                    try {
                        original = get_m->call_safe<bool>(ctx, (void*)layer);
                    } catch (const std::exception&) {
                        original = true;
                    }
                }

                m_fovea_shadow_original.emplace_back(layer, original);
            }

            try {
                set_m->call_safe<void*>(ctx, (void*)layer, false);
                ++changed;
            } catch (const std::exception&) {
            }
        }

        return changed > 0;
    }

    for (const auto& [layer, original] : m_fovea_shadow_original) {
        if (std::find(present.begin(), present.end(), layer) == present.end()) {
            continue;
        }

        try {
            set_m->call_safe<void*>(ctx, (void*)layer, original);
            ++changed;
        } catch (const std::exception&) {
        }
    }

    m_fovea_shadow_original.clear();
    return changed > 0;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [OCCLUSION 15.11.2026] RE7 test of 15.11.2026 (video and dump 10 at the top of the stairs): what lies behind an opening
// came and went from frame to frame in the wide view only (the window at the top: 21.8 in frames 0-2, 12, 14 of the 20,
// 6.2 in the others), while the fovea views (drawn before it in the same frame) looked elsewhere. The engine's occlusion
// culling of the wide view (via.render.layer.PreZforCull) switched off; its value found is put back when it is no longer
// wanted. views 1: the wide view, 2: every view. Returns the switches set.
int FoveatedRendering::set_view_occlusion(int views, bool restore, std::vector<std::pair<const void*, bool>>& original) {
    static const auto t = sdk::find_type_definition("via.render.layer.PreZforCull");
    static const auto get_m = t != nullptr ? t->get_method("get_DisableOcclusionCulling") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_DisableOcclusionCulling") : nullptr;

    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    if (!callable(set_m)) {
        return 0;
    }

    auto* ctx = sdk::get_thread_context();
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    const size_t last = restore || views == 2 ? (std::min<size_t>)(scenes.size(), 3) : (std::min<size_t>)(scenes.size(), 1);
    std::vector<const void*> present{};

    for (size_t v = 0; v < last; ++v) {
        std::vector<LayerNode> nodes{};
        collect_layers(scenes[v], 0, nodes, 600);

        for (const auto& n : nodes) {
            if (n.type == "via.render.layer.PreZforCull" && n.ptr != nullptr) {
                present.push_back(n.ptr);
            }
        }
    }

    int changed = 0;

    if (restore) {
        for (const auto& [layer, value] : original) {
            if (std::find(present.begin(), present.end(), layer) == present.end()) {
                continue;
            }

            try {
                set_m->call_safe<void*>(ctx, (void*)layer, value);
                ++changed;
            } catch (const std::exception&) {
            }
        }

        original.clear();
        return changed;
    }

    for (const void* layer : present) {
        const auto known = std::find_if(original.begin(), original.end(), [layer](const auto& e) { return e.first == layer; });

        if (known == original.end()) {
            bool value = false;

            if (callable(get_m)) {
                try {
                    value = get_m->call_safe<bool>(ctx, (void*)layer);
                } catch (const std::exception&) {
                    value = false;
                }
            }

            original.emplace_back(layer, value);
        }

        try {
            set_m->call_safe<void*>(ctx, (void*)layer, true);
            ++changed;
        } catch (const std::exception&) {
        }
    }

    return changed;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [OCCLUSION 15.11.2026] once a second while the quad view is engaged (layers the engine makes again are found too); back to
// the game's values when the option or the quad view goes off
void FoveatedRendering::view_occlusion_tick(bool engaged) {
    const int want = engaged ? std::clamp(m_wide_occlusion, 0, 2) : 0;

    if (want != m_wide_occlusion_applied) {
        if (!m_wide_occlusion_original.empty()) {
            const int n = set_view_occlusion(0, true, m_wide_occlusion_original);
            REVR_LOG(debug_log(std::format("[OCCLUSION] the engine's occlusion culling back to the game's values ({} switches)", n)));
        }

        m_wide_occlusion_applied = want;
        m_wide_occlusion_frames = 0;

        if (want != 0) {
            const int n = set_view_occlusion(want, false, m_wide_occlusion_original);
            REVR_LOG(debug_log(std::format("[OCCLUSION] the engine's occlusion culling off in {}: {} switches{}", want == 1 ? "the wide view" : "every view", n,
                n == 0 ? " (none found: no change)" : "")));
        }

        return;
    }

    if (want != 0 && (++m_wide_occlusion_frames % 90) == 0) {
        set_view_occlusion(want, false, m_wide_occlusion_original);
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [FLICKER TEST 15.11.2026] RE7 test of 15.11.2026: a light at the top of the stairs went on and off from frame to frame in
// the wide view only (video 14:57: on in about half of the frames, in no pattern), with the engine's occlusion culling off
// in any view too, and not in the normal multipass. One switch of the engine at a time (chosen in the menu) to find which
// part of the three views' rendering it comes from; each is put back when another is chosen or the quad view stops.
void FoveatedRendering::flicker_test_tick(bool engaged) {
    static const char* names[] = {"none", "fovea views almost empty", "fovea views without dynamic shadows", "no dynamic shadows in any view",
        "the head's view for every thread during view updates", "the engine's level of detail off"};
    auto& qs = quadview::State::get();
    const int want = engaged ? std::clamp(m_flicker_test, 0, 5) : 0;

    if (want != m_flicker_applied) {
        switch (m_flicker_applied) {
        case 1: qs.cpu_probe_tiny_fovea.store(false); break;
        case 2: set_fovea_dynamic_shadows(true); break;
        case 3:
            VR::get()->set_dynamic_shadow_override(-1);
            set_dynamic_shadows(m_flicker_shadows_original);
            break;
        case 4: qs.test_head_all_threads.store(false); break;
        case 5: set_lod_disabled(false); break;
        default: break;
        }

        bool found = true;

        switch (want) {
        case 1: qs.cpu_probe_tiny_fovea.store(true); break;
        case 2: found = set_fovea_dynamic_shadows(false); break;
        case 3: {
            const auto original = get_dynamic_shadows();
            m_flicker_shadows_original = original.value_or(true);
            VR::get()->set_dynamic_shadow_override(0);
            found = original.has_value() && set_dynamic_shadows(false);
            break;
        }
        case 4: qs.test_head_all_threads.store(true); break;
        case 5: found = set_lod_disabled(true); break;
        default: break;
        }

        REVR_LOG(debug_log(std::format("[FLICKER TEST] {} -> {}{}", names[m_flicker_applied], names[want], found ? "" : " (switch not found: no change)")));
        m_flicker_applied = want;
        m_flicker_frames = 0;
        return;
    }

    // layers the engine makes again are found too
    if (want == 2 && (++m_flicker_frames % 90) == 0) {
        set_fovea_dynamic_shadows(false);
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SHADOW CACHE 15.11.2026] RE7 test of 15.11.2026 17:21-17:34 (three videos, dumps 13-17: the piano, the armchair, the
// ceiling): light came and went from frame to frame where a shadow belongs (the patch under the piano 1 / 2 / 2 in the
// wide view and both fovea views of one frame, 5 / 7 / 7 in another: on in about half of the frames, in no pattern, in
// all three views at once), never in the normal multipass. All three views agree in every frame, so it is a state the
// views share, not a view of its own. Each view has its own via.render.layer.ShadowCast layers, but the shadow cache is
// shared (SceneNo 0 in every view, CacheEnable on) while only the clones' layers cull with GPUDrivenFrustumCulling (RE2
// shadow types of 30.10.2026): taken as the cause until the light flicker test says otherwise. A fovea view's narrow
// frustum would leave casters out of an entry; in the normal multipass the second view sees what the first sees.
// off = CacheEnable false on each of the fovea views' layers (the value found is kept), on = the kept values back on the
// layers that still exist.
bool FoveatedRendering::set_fovea_shadow_cache(bool on) {
    static const auto t = sdk::find_type_definition("via.render.layer.ShadowCast");
    static const auto get_m = t != nullptr ? t->get_method("get_CacheEnable") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_CacheEnable") : nullptr;

    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    if (!callable(set_m)) {
        return false;
    }

    auto* ctx = sdk::get_thread_context();
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    std::vector<const void*> present{};

    for (size_t v = 1; v < scenes.size() && v < 4; ++v) {
        std::vector<LayerNode> nodes{};
        collect_layers(scenes[v], 0, nodes, 600);

        for (const auto& n : nodes) {
            if (n.type == "via.render.layer.ShadowCast" && n.ptr != nullptr) {
                present.push_back(n.ptr);
            }
        }
    }

    int changed = 0;

    if (!on) {
        for (const void* layer : present) {
            const auto known = std::find_if(m_shadow_cache_original.begin(), m_shadow_cache_original.end(), [layer](const auto& e) { return e.first == layer; });

            if (known == m_shadow_cache_original.end()) {
                bool original = true;

                if (callable(get_m)) {
                    try {
                        original = get_m->call_safe<bool>(ctx, (void*)layer);
                    } catch (const std::exception&) {
                        original = true;
                    }
                }

                m_shadow_cache_original.emplace_back(layer, original);
            }

            try {
                set_m->call_safe<void*>(ctx, (void*)layer, false);
                ++changed;
            } catch (const std::exception&) {
            }
        }

        return changed > 0;
    }

    for (const auto& [layer, original] : m_shadow_cache_original) {
        if (std::find(present.begin(), present.end(), layer) == present.end()) {
            continue;
        }

        try {
            set_m->call_safe<void*>(ctx, (void*)layer, original);
            ++changed;
        } catch (const std::exception&) {
        }
    }

    m_shadow_cache_original.clear();
    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SHADOW CACHE 15.11.2026] the engine's shadow cache as a whole (via.render.Renderer ShadowCacheEnable, the game's option):
// off = the value found is kept and the cache switched off, on = the kept value back
bool FoveatedRendering::set_global_shadow_cache(bool on) {
    static const auto get = renderer_method("get_ShadowCacheEnable");
    static const auto set = renderer_method("set_ShadowCacheEnable");

    if (get == nullptr || get->get_function() == nullptr || set == nullptr || set->get_function() == nullptr) {
        return false;
    }

    try {
        auto* ctx = sdk::get_thread_context();

        if (!on) {
            const bool now = get->call_safe<bool>(ctx);

            if (!m_shadow_cache_global_original.has_value()) {
                m_shadow_cache_global_original = now;
            }

            if (now) {
                set->call_safe<void>(ctx, false);
            }

            return true;
        }

        if (m_shadow_cache_global_original.has_value()) {
            set->call_safe<void>(ctx, *m_shadow_cache_global_original);
            m_shadow_cache_global_original.reset();
        }
    } catch (const std::exception& e) {
        REVR_LOG(spdlog::error("[FoveatedRendering] ShadowCacheEnable({}): {}", on, e.what()));
        return false;
    }

    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SHADOW CACHE 15.11.2026] while the quad view is engaged (the light flicker test chooses the mode while it runs); once a
// second again (layers the engine makes again, the game's option set again); back to the game's values otherwise
void FoveatedRendering::shadow_cache_tick(bool engaged) {
    static const char* names[] = {"as the game", "off in the fovea views", "off everywhere"};
    const int want = engaged ? std::clamp(m_shadow_cache_mode, 0, 2) : 0;

    if (want != m_shadow_cache_applied) {
        if (m_shadow_cache_applied == 1) {
            set_fovea_shadow_cache(true);
        } else if (m_shadow_cache_applied == 2) {
            set_global_shadow_cache(true);
        }

        bool found = true;

        if (want == 1) {
            found = set_fovea_shadow_cache(false);
        } else if (want == 2) {
            found = set_global_shadow_cache(false);
        }

        REVR_LOG(debug_log(std::format("[SHADOW CACHE] {} -> {}{}", names[m_shadow_cache_applied], names[want], found ? "" : " (switch not found: no change)")));
        m_shadow_cache_applied = want;
        m_shadow_cache_frames = 0;
        return;
    }

    if (want != 0 && (++m_shadow_cache_frames % 90) == 0) {
        if (want == 1) {
            set_fovea_shadow_cache(false);
        } else {
            set_global_shadow_cache(false);
        }
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SHADOW CACHE 15.11.2026] what the views' shadow cast layers say (the cause above is taken from RE2; this is RE7's), and
// the layers that run once per frame next to the views (the output layer's other children)
std::string FoveatedRendering::describe_shadow_layers() {
    static const auto t = sdk::find_type_definition("via.render.layer.ShadowCast");
    std::string o = std::format("[t={:.1f}s] [SHADOW LAYERS] the views' shadow cast layers", std::chrono::duration<double>(Clock::now() - m_start_time).count());

    if (t == nullptr) {
        return o + ": the type was not found\n";
    }

    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    auto* ctx = sdk::get_thread_context();

    const auto flag = [&](const char* name, const void* layer) -> std::string {
        const auto m = t->get_method(name);

        if (!callable(m)) {
            return "-";
        }

        try {
            return m->call_safe<bool>(ctx, (void*)layer) ? "1" : "0";
        } catch (const std::exception&) {
            return "?";
        }
    };

    const auto number = [&](const char* name, const void* layer) -> std::string {
        const auto m = t->get_method(name);

        if (!callable(m)) {
            return "-";
        }

        try {
            return std::to_string(m->call_safe<int32_t>(ctx, (void*)layer));
        } catch (const std::exception&) {
            return "?";
        }
    };

    {
        static const auto get = renderer_method("get_ShadowCacheEnable");
        std::string cache = "-";

        if (get != nullptr && get->get_function() != nullptr) {
            try {
                cache = get->call_safe<bool>(ctx) ? "on" : "off";
            } catch (const std::exception&) {
                cache = "?";
            }
        }

        o += std::format(" (the engine's shadow cache: {})\n", cache);
    }

    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();

    for (size_t v = 0; v < scenes.size() && v < 4; ++v) {
        std::vector<LayerNode> nodes{};
        collect_layers(scenes[v], 0, nodes, 600);
        int count = 0;

        for (size_t k = 0; k < nodes.size(); ++k) {
            if (nodes[k].type != "via.render.layer.ShadowCast" || nodes[k].ptr == nullptr) {
                continue;
            }

            std::string parent{"?"};

            for (size_t a = k; a-- > 0;) {
                if (nodes[a].depth < nodes[k].depth) {
                    parent = nodes[a].type;
                    break;
                }
            }

            const void* l = nodes[k].ptr;
            o += std::format("  view {} layer {} (under {}): SceneNo {} CacheEnable {} Independent {} GPUDrivenFrustumCulling {} DynamicShadowingEnable {} "
                             "StaticShadowingEnable {} DrawDynamicShadow {} CastFlag {} Subresource {} ShadowCastStrategy {} Stable {} EnableSelf {}\n",
                v, count++, parent, number("get_SceneNo", l), flag("get_CacheEnable", l), flag("get_Independent", l), flag("get_GPUDrivenFrustumCulling", l),
                flag("get_DynamicShadowingEnable", l), flag("get_StaticShadowingEnable", l), flag("get_DrawDynamicShadow", l), number("get_CastFlag", l),
                number("get_Subresource", l), number("get_ShadowCastStrategy", l), flag("get_Stable", l), flag("get_EnableSelf", l));
        }

        if (count == 0) {
            o += std::format("  view {}: no shadow cast layer\n", v);
        }
    }

    if (const auto output = sdk::renderer::get_output_layer(); output != nullptr) {
        for (auto* child : output->get_layers()) {
            if (child == nullptr || !utility::re_managed_object::is_managed_object(child)) {
                continue;
            }

            std::vector<LayerNode> nodes{};
            collect_layers(child, 0, nodes, 60);

            if (nodes.empty() || nodes[0].type == "via.render.layer.Scene") {
                continue;
            }

            o += std::format("  next to the views: {} ({} layers in its tree)\n", nodes[0].type, nodes.size());

            for (size_t k = 1; k < nodes.size() && k < 40; ++k) {
                o += std::format("    {}{}\n", std::string((size_t)nodes[k].depth * 2, ' '), nodes[k].type);
            }
        }
    }

    return o;
}
#endif

// =================================================================================================
// [LIGHT FLICKER TEST 15.11.2026] Instead of choosing switches by hand and judging by eye: after a countdown, phases of
// 4.5 s with one engine switch each (1.5 s to settle, 3 s measured). The eye images that go to the headset are measured
// by d3d12::EyeRecorder (pixels that go up and right back down, or down and back up, between frames: a flickering light;
// camera motion moves values one way). The phases "as the game" open and close the test (the flicker has to be there
// in both). The result goes into foveation_debug.log.
// [15.11.2026 evening] Second round. The first (shadow cache, the fovea views' dynamic shadows, the fovea views almost
// empty) changed nothing at the hot spot: 144-178 flips per frame in its cell in every phase. RE7 dump 04 of 15.11.2026
// 19:4x (the stairs, two frames in a row): the room behind the opening at the top is dark in the wide view's first frame
// (mean 2.4 of 255, its lamp black too) and lit in the second (10.5, the lamp bright), with the same depth in both (the
// same geometry): the room's light goes on and off in the wide view, which the fovea views do not show there. Each phase
// switched one candidate: both fogs (the room's darkness could be fog), the engine's dynamic shadows everywhere (a light
// without its shadow map), the fovea views with their eye's whole frustum (does what the fovea views leave out switch
// the wide view's lights?), the engine's level of detail, the head's view for other threads during view updates.
// The flicker is counted outside the fovea rectangles on its own (the wide view's part), the fovea's only in the totals.
// Nothing is kept by itself: the verdict names what takes the flicker away.
// [PROJECTION CALLERS 15.11.2026] Third round. The second (log of 15.11.2026 21:3x, the stairs) gave the periphery's hot
// spot 16 % of the game's with the fovea views on their eye's whole frustum, 71 % with the fogs off, 104 - 117 % with
// the others; the first round's fovea views almost empty changed nothing either: the room's light stays on in the wide
// view only while a fovea view's frustum holds it too (the user: it goes away with a much larger fovea). Something of
// the engine that takes a fovea camera's projection decides about the light for the wide view as well. The phases now
// give the eye's whole frustum to a part of the fovea cameras' projection requests only: those of one context (another
// view's update on the same thread, other threads while a view updates, no view updating; the contexts the engine asked
// in before the test), and those of one call site inside the fovea views' own updates (the three asked most). Per phase
// the fovea views' image matrices after their update say whether the fovea image stayed right (the fovea's frustum) or
// not (the eye's whole one): a phase that takes the flicker away with the fovea image right is the fix.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
constexpr double LFT_COUNTDOWN_SECONDS = 5.0;
constexpr double LFT_SETTLE_SECONDS = 1.5;
constexpr double LFT_MEASURE_SECONDS = 3.0;

enum LftSwitch { LFT_NONE, LFT_FOG_OFF, LFT_NO_SHADOWS, LFT_FOVEA_WHOLE, LFT_LOD_OFF, LFT_HEAD_VIEW, LFT_FRUSTUM_CTX, LFT_FRUSTUM_SITE,
    LFT_UPDATE_OWN, LFT_WHOLE_INFOS, // [LIGHTS fix82 / fix83 17.11.2026]
    LFT_WIDE_FIRST,                  // [LIGHTS fix84 18.11.2026]
    LFT_FOVEA_FIRST,                 // [LIGHTS fix85 18.11.2026]
    LFT_HOLD_FOVEA };                // [LIGHTS fix86 18.11.2026]

// [PROJECTION CALLERS 15.11.2026] the contexts of quadview::State::proj_sites, as the log names them
constexpr const char* PROJ_CTX_NAMES[quadview::State::PROJ_CTX] = {"inside the fovea view's own update", "during another view's update",
    "from other threads while a view updates", "while no view updates"};

// a code address as module+offset
std::string code_place(uintptr_t a) {
    HMODULE mod{};

    if (a == 0 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &mod) ||
        mod == nullptr) {
        return std::format("0x{:x}", a);
    }

    char name[MAX_PATH]{};
    GetModuleFileNameA(mod, name, MAX_PATH);
    const std::string n{name};
    const auto slash = n.find_last_of("\\/");
    return std::format("{}+0x{:x}", slash != std::string::npos ? n.substr(slash + 1) : n, a - (uintptr_t)mod);
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::lft_start() {
    if (m_lft_running || any_test_running()) {
        return;
    }

    auto& qs = quadview::State::get();
    constexpr int NC = quadview::State::PROJ_CTX;

    // [PROJECTION CALLERS 15.11.2026] the fovea cameras' projection requests so far, per call site and context
    struct Site {
        uintptr_t addr{0};
        std::array<uint64_t, NC> n{};
        uint64_t total{0};
    };

    std::vector<Site> sites{};
    std::array<uint64_t, NC> per_ctx{};

    for (const auto& s : qs.proj_sites) {
        const auto a = s.addr.load(std::memory_order_relaxed);

        if (a == 0) {
            continue;
        }

        Site e{};
        e.addr = a;

        for (int c = 0; c < NC; ++c) {
            e.n[(size_t)c] = s.n[(size_t)c].load(std::memory_order_relaxed);
            e.total += e.n[(size_t)c];
            per_ctx[(size_t)c] += e.n[(size_t)c];
        }

        sites.push_back(e);
    }

    // [LIGHTS fix82 17.11.2026] Fourth round. The third (log of 17.11.2026 16:0x) took the flicker away only with the eye's
    // whole frustum for the request inside the fovea views' own updates (13 % at the hot spot), with the fovea image wrong.
    // fix82 gave that request the whole frustum and put the fovea's part into the image's matrices afterwards.
    // [LIGHTS fix83 17.11.2026] Fifth round. The fourth (log of 17.11.2026 19:2x): with fix82's way the flicker stayed (120 %),
    // with the whole frustum for everything it fell to 11 %. So it hangs on the matrices the fovea layer is drawn with after
    // its update, of which the engine keeps six sets. The phases leave the eye's whole frustum in one set at a time (the
    // others get the fovea's part, quadview::State::test_whole_infos), then in all but the scene's, then in all (the fourth
    // round's 11 % again, through this way). Where a phase takes the flicker away with the scene's set narrow, the fovea's
    // picture has to be looked at during that phase (the message names the phase): it is right only if that set does not
    // draw it.
    // [LIGHTS fix84 18.11.2026] Sixth round. The fifth (log of 17.11.2026 22:5x): of the six sets only the scene's takes the
    // flicker away (12 %; the others 92 - 115 %, all but the scene's 106 %), and that set draws the fovea's picture: the
    // matrices cannot be parted. What is left to try from here is the order: the fovea views' layers are drawn before the
    // wide view's; this round draws the wide view's first (quadview::State::test_wide_first) and counts whether the
    // engine followed. The scene's set with the whole frustum stays in as the known cure, for the comparison.
    // [LIGHTS fix85 18.11.2026] Seventh round, the check of the cure. The sixth (logs of 18.11.2026, three runs, the third
    // from the top of the stairs): with the wide view's layer first the flicker at the hot spot fell from 93.9 / 131.3 /
    // 306.5 to 15.5 / 16.2 / 34.6 (mask 1: 15.4 / 13.5 / 10.5) with the fovea's picture right, and the engine followed
    // the priority. It is the rule now (quadview::Settings::wide_first); the phases switch between the engine's order
    // and it, whatever the setting says, and the setting counts again when the test ends.
    // [LIGHTS fix86 18.11.2026] Eighth round, about what is left. The seventh (log of 18.11.2026 20:4x: from the top of the
    // stairs, and twice at a ceiling with the same light): with the wide view's layer first the hot spot fell from
    // 280.6 / 250.5 to 91.1 / 67.6, from 86.4 / 95.7 to 18.3 / 33.8, from 88.9 / 32.7 to 20.7 / 7.9: a third to a fifth is
    // left, the user sees it, and in the third run the fovea's own picture flickered where the light stood at its border
    // (inside the fovea rectangles 500 -> 172 against 30 elsewhere). With the user's gaze dead zone of 0 the fovea
    // rectangles move in nearly every frame. Every phase now keeps the wide view's layer first and adds one thing: the
    // fovea rectangles held still (does the light change only when a fovea view's frustum moves across it?), both fogs
    // off (the second round: 29 % of the flicker went with them), the engine's dynamic shadows off, and the eye's whole
    // frustum in the fovea views' scene matrices (the known cure, with the sharp area wrong: the floor to compare with).
    // The result also shows the light's course at the hot spot frame by frame, with the frames a fovea rectangle moved.
    m_lft_phases.clear();
    m_lft_phases.push_back({"as fix85 (the wide view's layer first)", LFT_WIDE_FIRST});
    m_lft_phases.push_back({"the fovea rectangles held still", LFT_HOLD_FOVEA});
    m_lft_phases.push_back({"both fogs off", LFT_FOG_OFF});
    m_lft_phases.push_back({"the engine's dynamic shadows off", LFT_NO_SHADOWS});
    m_lft_phases.push_back({"the eye's whole frustum in the fovea views' 'scene' matrices (mask 1: the sharp area is wrong)", LFT_WHOLE_INFOS, 1u});
    m_lft_phases.push_back({"as fix85 again", LFT_WIDE_FIRST});
    m_lft_matrix.assign(m_lft_phases.size(), std::array<uint64_t, 2>{0, 0});

    // the call sites for the log, most asked first (per fovea view update: two per frame)
    std::sort(sites.begin(), sites.end(), [](const Site& x, const Site& y) { return x.total > y.total; });
    const double updates = (double)(std::max<uint64_t>)(qs.inset_layers.load(std::memory_order_relaxed), 1);
    std::string text = std::format("  the fovea cameras' projection requests since the start, per fovea view update ({} updates), by the engine's call site and "
                                   "context ({} / {} / {} / {}):",
        (uint64_t)updates, PROJ_CTX_NAMES[0], PROJ_CTX_NAMES[1], PROJ_CTX_NAMES[2], PROJ_CTX_NAMES[3]);

    for (const auto& s : sites) {
        text += std::format(" {}: {:.2f} / {:.2f} / {:.2f} / {:.2f};", code_place(s.addr), (double)s.n[0] / updates, (double)s.n[1] / updates,
            (double)s.n[2] / updates, (double)s.n[3] / updates);
    }

    if (sites.empty()) {
        text += " none";
    }

    if (const auto lost = qs.proj_sites_full.load(std::memory_order_relaxed); lost > 0) {
        text += std::format(" | from call sites beyond the table: {}", lost);
    }

    m_lft_sites_text = text + "\n";

    m_lft_running = true;
    m_lft_phase = -1;
    m_lft_t0 = Clock::now();
    m_lft_shadows_known = false;
    m_flicker_test = 0; // the switches chosen by hand go back first
    qs.flicker_tag.store(-1);
    qs.test_full_frustum_ctx.store(0);
    qs.test_full_frustum_site.store(0);
    qs.test_whole_infos.store(-1); // [LIGHTS fix82 / fix83]
    qs.test_wide_first.store(1); // [LIGHTS fix86] the wide view's layer first in every phase, whatever the setting says
    qs.set_test_hold_rect(false);

    std::string phases{};

    for (size_t p = 0; p < m_lft_phases.size(); ++p) {
        phases += std::format("{}{} {}", p == 0 ? "" : ", ", p + 1, m_lft_phases[p].name);
    }

    REVR_LOG(debug_log(build_snapshot("light flicker test start") + m_lft_sites_text + "  phases: " + phases + "\n"));
    REVR_LOG(set_message(std::format("Light flicker test in 5 s: look so that the flickering light is beside your gaze (outside the sharp area) and keep "
                            "still for about {:.0f} s.",
        (double)m_lft_phases.size() * (LFT_SETTLE_SECONDS + LFT_MEASURE_SECONDS) + 2.0)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::lft_set_phase_switches(int phase, bool on) {
    if (phase < 0 || phase >= (int)m_lft_phases.size()) {
        return;
    }

    auto& qs = quadview::State::get();
    const auto& ph = m_lft_phases[(size_t)phase];
    bool found = true;

    switch (ph.sw) {
    case LFT_FOG_OFF:
        if (const auto& tu = TemporalUpscaler::get(); tu != nullptr) {
            tu->set_test_fog_off(on);
        } else {
            found = false;
        }

        break;
    case LFT_NO_SHADOWS:
        if (on) {
            const auto original = get_dynamic_shadows();
            m_lft_shadows_known = original.has_value();
            m_lft_shadows_original = original.value_or(true);
            VR::get()->set_dynamic_shadow_override(0);
            found = m_lft_shadows_known && set_dynamic_shadows(false);
        } else {
            VR::get()->set_dynamic_shadow_override(-1);

            if (m_lft_shadows_known) {
                set_dynamic_shadows(m_lft_shadows_original);
            }
        }

        break;
    case LFT_FOVEA_WHOLE: qs.test_full_fovea_frustum.store(on); break;
    case LFT_LOD_OFF: found = set_lod_disabled(on); break;
    case LFT_HEAD_VIEW: qs.test_head_all_threads.store(on); break;
    case LFT_FRUSTUM_CTX: qs.test_full_frustum_ctx.store(on ? ph.ctx : 0u); break;   // [PROJECTION CALLERS 15.11.2026]
    case LFT_FRUSTUM_SITE: qs.test_full_frustum_site.store(on ? ph.site : 0); break; // [PROJECTION CALLERS 15.11.2026]
    case LFT_UPDATE_OWN: qs.test_whole_infos.store(on ? 0 : -1); break;              // [LIGHTS fix82 17.11.2026]
    case LFT_WHOLE_INFOS: qs.test_whole_infos.store(on ? (int32_t)ph.ctx : -1); break; // [LIGHTS fix83 17.11.2026] (ctx: the mask)
    case LFT_WIDE_FIRST:  // [LIGHTS fix84 18.11.2026]; [fix86] the test keeps the wide view's layer first throughout (lft_start)
    case LFT_FOVEA_FIRST: // [LIGHTS fix85 18.11.2026] the engine's order for the phase
        if (ph.sw == LFT_FOVEA_FIRST) {
            qs.test_wide_first.store(on ? 0 : 1);
        }

        REVR_LOG(debug_log(std::format("[LIGHT FLICKER TEST] {}: {}; frames so far whose first view update was the wide view's {}, a fovea view's {}",
            ph.sw == LFT_WIDE_FIRST ? "the wide view's layer first" : "the fovea views' layers first", on ? "on" : "off",
            qs.first_wide.load(std::memory_order_relaxed), qs.first_fovea.load(std::memory_order_relaxed))));
        break;
    case LFT_HOLD_FOVEA: // [LIGHTS fix86 18.11.2026]
        qs.set_test_hold_rect(on);
        REVR_LOG(debug_log(std::format("[LIGHT FLICKER TEST] the fovea rectangles held still: {}; moves so far {}", on ? "on" : "off", qs.rect_moves())));
        break;
    default: break;
    }

    if (on && !found) {
        REVR_LOG(debug_log(std::format("[LIGHT FLICKER TEST] phase {}: {}: switch not found (the same as as the game)", phase + 1, ph.name)));
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::lft_cancel(const std::string& why) {
    if (!m_lft_running) {
        return;
    }

    lft_set_phase_switches(m_lft_phase, false);
    m_lft_running = false;
    m_lft_phase = -1;
    auto& qs = quadview::State::get();
    qs.flicker_tag.store(-1);
    qs.test_full_frustum_ctx.store(0);
    qs.test_full_frustum_site.store(0);
    qs.test_whole_infos.store(-1); // [LIGHTS fix82 / fix83]
    qs.test_wide_first.store(-1);  // [LIGHTS fix86] (the setting again)
    qs.set_test_hold_rect(false);
    REVR_LOG(debug_log(std::format("[LIGHT FLICKER TEST] stopped: {}", why)));
    REVR_LOG(set_message("Light flicker test stopped: " + why + "."));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::lft_tick(bool engaged) {
    if (!m_lft_running) {
        return;
    }

    if (!engaged) {
        lft_cancel("the tri-view is no longer engaged");
        return;
    }

    auto& qs = quadview::State::get();
    const auto now = Clock::now();
    const double t = std::chrono::duration<double>(now - m_lft_t0).count();
    const int count = (int)m_lft_phases.size();

    if (m_lft_phase < 0) {
        if (t < LFT_COUNTDOWN_SECONDS) {
            REVR_LOG(set_message(std::format("Light flicker test in {:.0f} s: the flickering light beside your gaze (outside the sharp area), keep still.",
                std::ceil(LFT_COUNTDOWN_SECONDS - t))));
            return;
        }

        m_lft_phase = 0;
        m_lft_t0 = now;
        lft_set_phase_switches(0, true);
        qs.flicker_tag.store(-1);
        qs.request_flicker_measure(count * (LFT_SETTLE_SECONDS + LFT_MEASURE_SECONDS) + 1.0);
        REVR_LOG(set_message(std::format("Light flicker test 1/{}: {}", count, m_lft_phases[0].name)));
        return;
    }

    // all phases done: the measurement's result (the recorder ends a moment later)
    if (m_lft_phase >= count) {
        if (std::vector<quadview::State::FlickerRow> rows{}; qs.take_flicker_rows(rows)) {
            lft_finish(rows);
        } else if (t > 8.0) {
            lft_cancel("no result from the measurement (the recorder takes one recording at a time: was a video being written?)");
        }

        return;
    }

    if (t < LFT_SETTLE_SECONDS) {
        qs.flicker_tag.store(-1);
        return;
    }

    if (t < LFT_SETTLE_SECONDS + LFT_MEASURE_SECONDS) {
        // [PROJECTION CALLERS 15.11.2026] the fovea views' image matrices are counted over the measured part
        if (qs.flicker_tag.exchange(m_lft_phase) != m_lft_phase) {
            qs.fovea_matrix_narrow.store(0);
            qs.fovea_matrix_whole.store(0);
        }

        return;
    }

    // the next phase
    qs.flicker_tag.store(-1);
    m_lft_matrix[(size_t)m_lft_phase] = {qs.fovea_matrix_narrow.load(), qs.fovea_matrix_whole.load()};
    lft_set_phase_switches(m_lft_phase, false);
    ++m_lft_phase;
    m_lft_t0 = now;

    if (m_lft_phase < count) {
        lft_set_phase_switches(m_lft_phase, true);
        REVR_LOG(set_message(std::format("Light flicker test {}/{}: {}", m_lft_phase + 1, count, m_lft_phases[(size_t)m_lft_phase].name)));
    } else {
        REVR_LOG(set_message("Light flicker test: waiting for the result..."));
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::lft_finish(const std::vector<quadview::State::FlickerRow>& rows) {
    m_lft_running = false;
    m_lft_phase = -1;
    quadview::State::get().flicker_tag.store(-1);
    quadview::State::get().test_whole_infos.store(-1); // [LIGHTS fix82 / fix83] (the last phase's switch goes back with it as well)
    quadview::State::get().test_wide_first.store(-1); // [LIGHTS fix84 / fix85] (the setting again)
    quadview::State::get().set_test_hold_rect(false); // [LIGHTS fix86]
    const int count = (int)m_lft_phases.size();

    // [15.11.2026 evening] the hot spot of the periphery (its 32 x 32 cell with the most flips, outside the fovea
    // rectangles) is what a flickering light in the wide view shows; the totals include the fovea and the whole image's noise
    std::vector<double> total((size_t)count, -1.0);
    std::vector<double> hot((size_t)count, -1.0);
    std::string o = std::format("[t={:.1f}s] [LIGHT FLICKER TEST] result: pixels per frame that went up and right back down (or down and back up) by "
                                "{} or more of 219 luma steps, in both eye images at half size (848 x 432, left eye left); the periphery: outside "
                                "the fovea rectangles (8 of these pixels wider)\n",
        std::chrono::duration<double>(Clock::now() - m_start_time).count(), d3d12::EyeRecorder::FLICKER_STEP);
    o += m_lft_sites_text;

    const auto where = [](uint32_t cell_x, uint32_t cell_y) {
        const uint32_t cx = cell_x * 32 + 16;
        const uint32_t cy = cell_y * 32 + 16;
        const bool right = cx >= 424;
        return std::format("{} eye at x {:.2f} y {:.2f}", right ? "right" : "left", (double)(right ? cx - 424 : cx) / 424.0, (double)cy / 432.0);
    };

    // [PROJECTION CALLERS 15.11.2026] the fovea image during the phase: its views' image matrices with the fovea's frustum
    const auto fovea_image = [this](int p) -> std::string {
        const auto& m = m_lft_matrix[(size_t)p];
        const uint64_t n = m[0] + m[1];

        if (n == 0) {
            return "fovea image not seen";
        }

        const double narrow = (double)m[0] / (double)n;
        return std::format("fovea image by its scene matrices {} (the fovea's frustum in them in {:.0f} % of {} view updates)",
            narrow >= 0.95 ? "right" : (narrow <= 0.05 ? "wrong: the eye's whole frustum" : "partly wrong"), narrow * 100.0, n);
    };

    for (int p = 0; p < count; ++p) {
        const auto it = std::find_if(rows.begin(), rows.end(), [p](const auto& r) { return r.tag == p; });

        if (it == rows.end() || it->frames == 0) {
            o += std::format("  phase {}: {}: not measured\n", p + 1, m_lft_phases[(size_t)p].name);
            continue;
        }

        const double n = (double)it->frames;
        total[(size_t)p] = (double)it->flips / n;
        hot[(size_t)p] = (double)it->peri_cell_flips / n;
        // ([LIGHTS fix83] inside the fovea rectangles: a fovea picture that breaks in a phase shows here as well)
        o += std::format("  phase {}: {}: {:.1f} per frame, periphery {:.1f}, inside the fovea rectangles {:.1f} ({} frames); periphery hot spot "
                         "{:.1f} per frame in the {} of its image; whole image hot spot {:.1f} in the {}; {}\n",
            p + 1, m_lft_phases[(size_t)p].name, total[(size_t)p], (double)it->peri_flips / n,
            ((double)it->flips - (double)it->peri_flips) / n, it->frames, hot[(size_t)p], where(it->peri_cell_x, it->peri_cell_y),
            (double)it->cell_flips / n, where(it->cell_x, it->cell_y), fovea_image(p));
    }

    double base = 0.0;
    int base_n = 0;

    for (const int p : {0, count - 1}) {
        if (p >= 0 && hot[(size_t)p] >= 0.0) {
            base += hot[(size_t)p];
            ++base_n;
        }
    }

    base = base_n > 0 ? base / base_n : -1.0;
    std::string verdict{};

    if (rows.empty() || base < 0.0) {
        verdict = "the phases with the fovea views' own frustum were not measured";
    } else if (base < 30.0) {
        verdict = std::format("hardly any flicker in the periphery with the fovea views' own frustum ({:.1f} per frame at its hot "
                              "spot): look so that the flickering light is beside your gaze while the test runs",
            base);
    } else {
        std::string away{};
        std::string shares{};

        for (int p = 1; p + 1 < count; ++p) {
            if (hot[(size_t)p] < 0.0) {
                continue;
            }

            const double s = hot[(size_t)p] / base;
            shares += std::format("{}{} {:.0f} %", shares.empty() ? "" : ", ", m_lft_phases[(size_t)p].name, s * 100.0);

            if (s <= 0.35) {
                const auto& m = m_lft_matrix[(size_t)p];
                const uint64_t n = m[0] + m[1];
                const bool right = n > 0 && (double)m[0] >= 0.95 * (double)n;
                away += std::format("{}{} ({})", away.empty() ? "" : "; ", m_lft_phases[(size_t)p].name,
                    right ? "the fovea's scene matrices right: was its picture right in this phase?" : "fovea image wrong");
            }
        }

        verdict = away.empty() ? "none of the switches takes the periphery's flicker away" : "the periphery's flicker goes away with: " + away;
        verdict += std::format(" (hot spot in the first and last phase {:.1f} per frame; {})", base, shares);
    }

    // [LIGHTS fix86 18.11.2026] the light's course at one cell, frame by frame, for every phase
    {
        int lo = 255;
        int hi = 0;
        const quadview::State::FlickerRow* any = nullptr;

        for (const auto& r : rows) {
            for (const auto v : r.series) {
                lo = (std::min)(lo, (int)v);
                hi = (std::max)(hi, (int)v);
            }

            if (any == nullptr && !r.series.empty()) {
                any = &r;
            }
        }

        if (any != nullptr && hi > lo) {
            const int mid2 = lo + hi; // twice the middle
            o += std::format("  the light's course in the {} (a cell of 32 x 32 of the half-size image; the periphery's with the most flips in any "
                             "phase): its mean luma per frame as 0 .. 9 between the darkest and the brightest of all phases ({} .. {} of 255){}; "
                             "under it the frames in which a fovea rectangle stood elsewhere than the frame before (l / r / b = left, right, both)\n",
                where(any->series_cell_x, any->series_cell_y), lo, hi, hi - lo < 6 ? ": hardly any change there" : "");

            for (int p = 0; p < count; ++p) {
                const auto it = std::find_if(rows.begin(), rows.end(), [p](const auto& r) { return r.tag == p; });

                if (it == rows.end() || it->series.empty()) {
                    continue;
                }

                std::string levels{};
                std::string moves{};
                int switches = 0;
                int with_move = 0;
                int moved_frames = 0;
                int phase_lo = 255;
                int phase_hi = 0;

                for (size_t f = 0; f < it->series.size(); ++f) {
                    const int v = it->series[f];
                    phase_lo = (std::min)(phase_lo, v);
                    phase_hi = (std::max)(phase_hi, v);
                    levels += (char)('0' + std::clamp((v - lo) * 10 / (hi - lo + 1), 0, 9));
                    const uint8_t m = f < it->moved.size() ? it->moved[f] : 0;
                    moves += m == 3 ? 'b' : (m == 2 ? 'r' : (m == 1 ? 'l' : '.'));
                    moved_frames += m != 0 ? 1 : 0;

                    // a switch: the light crosses the middle between the darkest and the brightest
                    if (f > 0 && hi - lo >= 6 && ((2 * v > mid2) != (2 * (int)it->series[f - 1] > mid2))) {
                        ++switches;
                        const bool before = f - 1 < it->moved.size() && it->moved[f - 1] != 0;
                        with_move += m != 0 || before ? 1 : 0;
                    }
                }

                o += std::format("  phase {}: {}\n           {}\n           luma {} .. {}; crossings of the middle {}, of them {} in a frame with a move or the "
                                 "one after; frames with a move {} of {}\n",
                    p + 1, levels, moves, phase_lo, phase_hi, switches, with_move, moved_frames, it->series.size());
            }
        }
    }

    o += "  verdict: " + verdict + "\n";
    REVR_LOG(debug_log(o));
    REVR_LOG(set_message("Light flicker test finished: " + verdict + ". The result is in foveation_debug.log."));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [GPU TEST 06.11.2026] The engine's occlusion culling of the views (via.render.layer.PreZforCull, DisableOcclusionCulling):
// switched off in the fovea views (views 1) or in every view (views 0), or each switch back to the value found (restore;
// only layers still in a view's tree: the engine may have made them again). True when a switch was found.
bool FoveatedRendering::set_occlusion_culling(int views, bool restore) {
    static const auto t = sdk::find_type_definition("via.render.layer.PreZforCull");
    static const auto get_m = t != nullptr ? t->get_method("get_DisableOcclusionCulling") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_DisableOcclusionCulling") : nullptr;

    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    if (!callable(set_m)) {
        return false;
    }

    auto* ctx = sdk::get_thread_context();
    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();
    std::vector<const void*> present{};

    for (size_t v = restore ? 0 : (size_t)(views == 1 ? 1 : 0); v < scenes.size() && v < 3; ++v) {
        std::vector<LayerNode> nodes{};
        collect_layers(scenes[v], 0, nodes, 600);

        for (const auto& n : nodes) {
            if (n.type == "via.render.layer.PreZforCull" && n.ptr != nullptr) {
                present.push_back(n.ptr);
            }
        }
    }

    int changed = 0;

    if (restore) {
        for (const auto& [layer, original] : m_occlusion_original) {
            if (std::find(present.begin(), present.end(), layer) == present.end()) {
                continue;
            }

            try {
                set_m->call_safe<void*>(ctx, (void*)layer, original);
                ++changed;
            } catch (const std::exception&) {
            }
        }

        m_occlusion_original.clear();
        return changed > 0;
    }

    for (const void* layer : present) {
        const auto known = std::find_if(m_occlusion_original.begin(), m_occlusion_original.end(), [layer](const auto& e) { return e.first == layer; });

        if (known == m_occlusion_original.end()) {
            bool original = false;

            if (callable(get_m)) {
                try {
                    original = get_m->call_safe<bool>(ctx, (void*)layer);
                } catch (const std::exception&) {
                    original = false;
                }
            }

            m_occlusion_original.emplace_back(layer, original);
        }

        try {
            set_m->call_safe<void*>(ctx, (void*)layer, true);
            ++changed;
        } catch (const std::exception&) {
        }
    }

    return changed > 0;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [SUBMIT TIMELINE 06.11.2026] the GPU timeline of the frame (d3d12::SubmitTimeline) on / off, from the game thread
bool FoveatedRendering::submit_timeline_enable(bool enable) {
    auto& st = d3d12::SubmitTimeline::get();

    if (!enable) {
        if (st.running()) {
            st.stop();
            REVR_LOG(debug_log("[SUBMIT TIMELINE] stopped"));
        }

        return true;
    }

    auto& hook = g_framework->get_d3d12_hook();

    if (!g_framework->is_dx12() || hook == nullptr || hook->get_device() == nullptr || hook->get_command_queue() == nullptr) {
        REVR_LOG(set_message("GPU timeline of the frame: needs D3D12."));
        return false;
    }

    std::string why{};
    const auto t0 = Clock::now();

    if (!st.start(hook->get_device(), hook->get_command_queue(), why)) {
        REVR_LOG(set_message("GPU timeline of the frame could not start: " + why));
        REVR_LOG(debug_log("[SUBMIT TIMELINE] could not start: " + why));
        return false;
    }

    m_st_lines = 0;
    REVR_LOG(debug_log(std::format("[SUBMIT TIMELINE] started ({:.0f} ms to set up): each status line ends with the GPU timeline of the frame, every "
                          "sixth with every submit of one frame",
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count())));
    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ctest_apply(int kind) {
    quadview::State::get().cpu_probe_tiny_fovea.store(kind == 1);
    m_ctest_skip_fovea_overlay.store(kind == 5, std::memory_order_relaxed);

    if (kind == 2) {
        if (!set_fovea_dynamic_shadows(false)) {
            REVR_LOG(debug_log("[CPU TEST] the fovea views' shadow cast layers were not found: this phase is the same as a normal one"));
        }
    } else if (!m_fovea_shadow_original.empty()) {
        set_fovea_dynamic_shadows(true);
    }

    if (kind == 3) {
        VR::get()->set_dynamic_shadow_override(0);
        set_dynamic_shadows(false);
    } else {
        VR::get()->set_dynamic_shadow_override(-1);
        set_dynamic_shadows(m_ctest_shadows_original);
    }

    // [GPU TEST 06.11.2026] the engine's occlusion culling off in the fovea views (6) or in every view (7)
    if (kind == 6 || kind == 7) {
        if (!set_occlusion_culling(kind == 6 ? 1 : 0, false)) {
            REVR_LOG(debug_log("[GPU TEST] no occlusion culling switch of the views was found: this phase is the same as a normal one"));
        }
    } else if (!m_occlusion_original.empty()) {
        set_occlusion_culling(0, true);
    }

    // [FOVEA FOV TEST 06.11.2026 evening] the fovea cameras' field of view for the engine narrowed (13)
    quadview::State::get().test_narrow_fovea_fov.store(kind == 13, std::memory_order_relaxed);

    // [GAP TEST 06.11.2026 night] the test load: small (14) or large (15) in the waits, large where the queue does not wait (16)
    d3d12::SubmitTimeline::get().set_test_load(kind == 14 ? 1 : kind == 15 ? 2 : kind == 16 ? 3 : 0);

    // [GPU TEST 06.11.2026 evening] the engine's level of detail off (10)
    if (kind == 10) {
        if (!set_lod_disabled(true)) {
            REVR_LOG(debug_log("[GPU TEST] the engine's level of detail switch was not found: this phase is the same as a normal one"));
        }
    } else if (m_lod_changed) {
        set_lod_disabled(false);
    }

    m_ctest_kind = kind;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [GPU TEST 06.11.2026 evening] via.render.Renderer's static DisableLod (the dump of 18:30: false): on = every object at its full
// detail; off = the value found back
bool FoveatedRendering::set_lod_disabled(bool disable) {
    static const auto t = sdk::find_type_definition("via.render.Renderer");
    static const auto get_m = t != nullptr ? t->get_method("get_DisableLod") : nullptr;
    static const auto set_m = t != nullptr ? t->get_method("set_DisableLod") : nullptr;

    const auto callable = [](const sdk::REMethodDefinition* m) {
        return m != nullptr && m->get_function() != nullptr && !utility::is_stub_code((uint8_t*)m->get_function());
    };

    if (!callable(set_m)) {
        return false;
    }

    auto* ctx = sdk::get_thread_context();

    try {
        if (disable) {
            if (!m_lod_changed) {
                m_lod_original = callable(get_m) ? get_m->call_safe<bool>(ctx) : false;
            }

            set_m->call_safe<void*>(ctx, true);
            m_lod_changed = true;
        } else if (m_lod_changed) {
            set_m->call_safe<void*>(ctx, m_lod_original);
            m_lod_changed = false;
        }
    } catch (const std::exception&) {
        return false;
    }

    return true;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// [WINDOW PASS 06.11.2026 evening] the game window's swap chain, its last Present, the window and its monitor, DXGI's frame
// statistics (at the end: the presents shown and the monitor's refreshes since the start)
std::string FoveatedRendering::window_present_info(bool at_end) {
    auto& hook = g_framework->get_d3d12_hook();

    if (!g_framework->is_dx12() || hook == nullptr || hook->get_swap_chain() == nullptr) {
        return "[WINDOW] no D3D12 swap chain";
    }

    auto* sc = hook->get_swap_chain();
    std::string o = "[WINDOW] the game window's swap chain:";
    DXGI_SWAP_CHAIN_DESC1 d{};

    if (SUCCEEDED(sc->GetDesc1(&d))) {
        const char* effect = d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip discard"
                             : d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? "flip sequential"
                             : d.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? "discard"
                                                                         : "sequential";
        o += std::format(" {}x{}, {} buffers, {}, flags 0x{:x} (tearing allowed {}, waitable {})", d.Width, d.Height, d.BufferCount, effect, d.Flags,
            (d.Flags & 2048u) != 0 ? "yes" : "no", (d.Flags & 64u) != 0 ? "yes" : "no"); // ALLOW_TEARING, FRAME_LATENCY_WAITABLE_OBJECT
    }

    BOOL fullscreen = FALSE;
    sc->GetFullscreenState(&fullscreen, nullptr);
    o += std::format(", fullscreen {}, the game's last Present: sync interval {}, flags 0x{:x}", fullscreen ? "yes" : "no", hook->get_last_sync_interval(),
        hook->get_last_present_flags());

    HWND hwnd{};

    if (SUCCEEDED(sc->GetHwnd(&hwnd)) && hwnd != nullptr) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        o += std::format(" | window: minimised {}, visible {}, in front {}, {}x{}", IsIconic(hwnd) ? "yes" : "no", IsWindowVisible(hwnd) ? "yes" : "no",
            GetForegroundWindow() == hwnd ? "yes" : "no", rc.right - rc.left, rc.bottom - rc.top);

        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);

        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);

            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                o += std::format(", its monitor {}x{} at {} Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
            }
        }
    }

    DXGI_FRAME_STATISTICS st{};
    const HRESULT hr = sc->GetFrameStatistics(&st);
    UINT last_count = 0;
    sc->GetLastPresentCount(&last_count);

    if (FAILED(hr)) {
        o += std::format(" | DXGI frame statistics: not available (0x{:x}), presents {}", (uint32_t)hr, last_count);
        return o;
    }

    o += std::format(" | DXGI frame statistics: presents {} (the last one shown: {}), refreshes {}", last_count, st.PresentCount, st.SyncRefreshCount);

    if (!at_end) {
        m_win_present_count = st.PresentCount;
        m_win_sync_refresh = st.SyncRefreshCount;
        m_win_sync_qpc = st.SyncQPCTime.QuadPart;
    } else if (m_win_sync_qpc != 0 && st.SyncQPCTime.QuadPart > m_win_sync_qpc) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        const double s = (double)(st.SyncQPCTime.QuadPart - m_win_sync_qpc) / (double)(std::max)(f.QuadPart, (LONGLONG)1);
        const uint64_t shown = st.PresentCount - m_win_present_count;
        const uint64_t refreshes = st.SyncRefreshCount - m_win_sync_refresh;
        o += std::format(" | since the start ({:.1f} s): {} presents shown ({:.1f} per s), {} refreshes ({:.1f} per s)", s, shown, (double)shown / s,
            refreshes, (double)refreshes / s);
    }

    return o;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ctest_start(bool gpu) {
    if (any_test_running()) {
        return;
    }

    m_ctest_gpu = gpu;
    m_ctest_seq = gpu ? GTEST_PHASES : CTEST_PHASES;
    m_ctest_count = gpu ? GTEST_PHASE_COUNT : CTEST_PHASE_COUNT;

    if (!m_log_periodic->value()) {
        REVR_LOG(set_message("CPU test: switch on \"Periodic status line\" first (the test is read from those lines)."));
        return;
    }

    const auto original = get_dynamic_shadows();

    if (!original.has_value()) {
        REVR_LOG(set_message("CPU test: the engine's shadow switch was not found."));
        return;
    }

    m_ctest_shadows_original = *original;
    quadview::State::get().cpu_probe_tiny_fovea.store(false);
    m_ctest_running = true;
    m_ctest_phase = -1;
    m_ctest_kind = 0;
    m_ctest_frames = 0;
    m_ctest_phase_start = Clock::now();

    std::string order{};

    for (int i = 0; i < m_ctest_count; ++i) {
        order += (i > 0 ? ", " : "") + std::string{CTEST_NAMES[m_ctest_seq[i]]};
    }

    // [GPU TEST 06.11.2026] the GPU timeline of the frame runs through the GPU test (started here if it was not on)
    m_ctest_started_timeline = false;

    if (gpu && !d3d12::SubmitTimeline::get().running()) {
        m_ctest_started_timeline = submit_timeline_enable(true);
    }

    // [GAP TEST 06.11.2026 night] the test load, eye-sized (the game window's back buffer has the eye images' size)
    if (gpu && d3d12::SubmitTimeline::get().running()) {
        auto& hook = g_framework->get_d3d12_hook();
        DXGI_SWAP_CHAIN_DESC1 d{};
        std::string why{};
        const auto t0 = Clock::now();

        if (hook != nullptr && hook->get_swap_chain() != nullptr && SUCCEEDED(hook->get_swap_chain()->GetDesc1(&d)) &&
            d3d12::SubmitTimeline::get().prepare_test_load(hook->get_device(), d.Width, d.Height, why)) {
            REVR_LOG(debug_log(std::format("[GPU TEST] test load ready ({:.0f} ms): one (small) or two (large) {}x{} images, 12 samples per pixel, "
                                  "before the engine's waits that follow its submits {} and {} from the thread that presents (or two large after its submit {})",
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), d.Width, d.Height, d3d12::SubmitTimeline::GAP_AFTER_A,
                d3d12::SubmitTimeline::GAP_AFTER_B, d3d12::SubmitTimeline::REF_AFTER)));
        } else {
            REVR_LOG(debug_log("[GPU TEST] the test load could not be made" + (why.empty() ? std::string{} : ": " + why) + " (its phases are the same as normal ones)"));
        }
    }

    if (gpu) {
        REVR_LOG(debug_log(window_present_info(false))); // [WINDOW PASS 06.11.2026 evening]
    }

    REVR_LOG(debug_log(std::format("[{} TEST] start: {} phases of {:.0f} s after a {:.0f} s countdown: {} (dynamic shadows were {}{})", gpu ? "GPU" : "CPU", m_ctest_count,
        CTEST_PHASE_SECONDS, CTEST_COUNTDOWN_SECONDS, order, m_ctest_shadows_original ? "on" : "off",
        gpu ? (d3d12::SubmitTimeline::get().running() ? ", GPU timeline of the frame on" : ", GPU timeline of the frame NOT available") : "")));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ctest_next_phase(int phase) {
    if (m_ctest_phase >= 0 && m_ctest_seq[m_ctest_phase] == 5) {
        REVR_LOG(debug_log(std::format("[CPU TEST] fovea views' interface updates left out in the phase: {}", m_ctest_overlay_skipped.exchange(0))));
    }

    write_periodic_line(); // closes the interval of the phase before (labelled with it)
    m_ctest_phase = phase;
    m_ctest_phase_start = Clock::now();
    m_ctest_frames = 0;
    m_ctest_timeline_done = false;
    ctest_apply(m_ctest_seq[phase]);
    REVR_LOG(debug_log(std::format("[{} TEST] phase {}/{}: {}{}", m_ctest_gpu ? "GPU" : "CPU", phase + 1, m_ctest_count, CTEST_NAMES[m_ctest_seq[phase]],
        m_ctest_seq[phase] == 6 || m_ctest_seq[phase] == 7 ? std::format(" ({} occlusion culling switches found)", m_occlusion_original.size())
        : m_ctest_seq[phase] == 10 ? std::format(" (the level of detail switch {}, it was {})", m_lod_changed ? "set" : "NOT found", m_lod_original ? "on" : "off")
                                   : std::string{})));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ctest_cancel(const char* why) {
    if (!m_ctest_running) {
        return;
    }

    ctest_apply(0);
    m_ctest_running = false;
    m_ctest_phase = -1;

    if (m_ctest_started_timeline) {
        submit_timeline_enable(false);
        m_ctest_started_timeline = false;
    }

    REVR_LOG(set_message(std::format("{} test cancelled: {}", m_ctest_gpu ? "GPU" : "CPU", why)));
    REVR_LOG(debug_log(std::format("[{} TEST] cancelled: {}", m_ctest_gpu ? "GPU" : "CPU", why)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ctest_tick() {
    const double elapsed = std::chrono::duration<double>(Clock::now() - m_ctest_phase_start).count();

    if (m_ctest_phase < 0) {
        REVR_LOG(set_message(std::format("{} test starts in {:.0f} s: keep many enemies in view (a fight).", m_ctest_gpu ? "GPU" : "CPU",
            (std::max)(CTEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        if (elapsed >= CTEST_COUNTDOWN_SECONDS) {
            ctest_next_phase(0);
        }

        return;
    }

    // the fovea views' layers once a second again (layers made again by the engine are found too)
    if (m_ctest_seq[m_ctest_phase] == 2 && (++m_ctest_frames % 90) == 0) {
        set_fovea_dynamic_shadows(false);
    }

    // [GPU TEST 06.11.2026] the same for the occlusion culling switches
    if ((m_ctest_seq[m_ctest_phase] == 6 || m_ctest_seq[m_ctest_phase] == 7) && (++m_ctest_frames % 90) == 0) {
        set_occlusion_culling(m_ctest_seq[m_ctest_phase] == 6 ? 1 : 0, false);
    }

    // [TIMELINE 03.11.2026] 40 frames in the middle of the first normal phase and of the first one with empty fovea views
    if (m_ctest_phase < 2 && !m_ctest_timeline_done && elapsed >= CTEST_TIMELINE_AT_SECONDS) {
        m_ctest_timeline_done = true;
        timeline_start(std::format("{} TEST {}/{}: {}", m_ctest_gpu ? "GPU" : "CPU", m_ctest_phase + 1, m_ctest_count, CTEST_NAMES[m_ctest_seq[m_ctest_phase]]), 40);
    }

    REVR_LOG(set_message(std::format("{} test {}/{}: {} ({:.0f} s left in this phase)", m_ctest_gpu ? "GPU" : "CPU", m_ctest_phase + 1, m_ctest_count,
        CTEST_NAMES[m_ctest_seq[m_ctest_phase]], (std::max)(CTEST_PHASE_SECONDS - elapsed, 0.0))));

    if (elapsed < CTEST_PHASE_SECONDS) {
        return;
    }

    if (m_ctest_phase + 1 < m_ctest_count) {
        ctest_next_phase(m_ctest_phase + 1);
        return;
    }

    write_periodic_line(); // the last phase's interval
    ctest_apply(0);
    m_ctest_running = false;
    m_ctest_phase = -1;

    if (m_ctest_started_timeline) {
        submit_timeline_enable(false);
        m_ctest_started_timeline = false;
    }

    if (m_ctest_gpu) {
        REVR_LOG(debug_log(window_present_info(true))); // [WINDOW PASS 06.11.2026 evening]
    }

    REVR_LOG(debug_log(std::format("[{} TEST] done", m_ctest_gpu ? "GPU" : "CPU")));
    REVR_LOG(set_message(std::format("{} test done: please send foveation_debug.log.", m_ctest_gpu ? "GPU" : "CPU")));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
struct EngineFeature {
    const char* label;
    const char* type_name;
    const char* sub_getter; // nullptr, or the getter of the object the property belongs to
    const char* property;   // bool property: get_ / set_
};

constexpr EngineFeature FTEST_FEATURES[] = {
    {"volumetric fog (control)", "via.render.VolumetricFogControl", nullptr, "Enabled"},
    {"volumetric fog", "via.render.VolumetricFog", nullptr, "Enabled"},
    {"fog", "via.render.Fog", nullptr, "Enabled"},
    {"SSAO", "via.render.SSAOControl", nullptr, "Enabled"},
    {"geometry AO", "via.render.GeometryAOControl", nullptr, "Enabled"},
    {"soft bloom", "via.render.SoftBloom", nullptr, "Enabled"},
    {"FFT bloom", "via.render.FFTBloom", nullptr, "Enabled"},
    {"god rays", "via.render.GodRay", nullptr, "Enabled"},
    {"light shafts", "via.render.LightShaftFilterControl", nullptr, "Enabled"},
    {"light shaft entities", "via.render.LightShaft", nullptr, "Enabled"}, // [LIGHT SHAFT OFF 13.11.2026] the scene's own entities
    {"depth of field", "via.render.DepthOfField", nullptr, "Enabled"},
    {"subsurface scattering", "via.render.SSSSSControl", nullptr, "Enabled"},
    {"tessellation", "via.render.TessellationFactor", nullptr, "Enabled"},
    {"lens flare", "via.render.FakeLensFlare", nullptr, "Enabled"},
    {"motion blur", "via.render.MotionBlur", nullptr, "Enabled"},
    {"colour correction", "via.render.LDRPostProcess", "get_ColorCorrect", "Enabled"},
    // [VIEW DIFFERENCE 02.11.2026] two more that depend on each view: screen space reflections (only what the view sees can
    // be reflected) and the dynamic shadows (cascades fitted to each view's frustum); "renderer" = via.render.Renderer's
    // DynamicShadowEnable, not a component
    {"screen space reflections", "via.render.SSRControl", nullptr, "Enabled"},
    {"dynamic shadows", "renderer", nullptr, "DynamicShadowEnable"},
};
constexpr int FTEST_FEATURE_COUNT = (int)(sizeof(FTEST_FEATURES) / sizeof(FTEST_FEATURES[0]));
constexpr double FTEST_COUNTDOWN_SECONDS = 8.0;
constexpr double FTEST_PHASE_SECONDS = 8.0;
constexpr double FTEST_WARMUP_SECONDS = 2.5;

// the object a feature's property is set on (the component itself or its sub object), or null
::REManagedObject* feature_target(::REManagedObject* component, const EngineFeature& f) {
    if (component == nullptr) {
        return nullptr;
    }

    if (f.sub_getter == nullptr) {
        return component;
    }

    try {
        return sdk::call_object_func_easy<::REManagedObject*>(component, f.sub_getter);
    } catch (const std::exception&) {
        return nullptr;
    }
}

// get_ / set_ of a bool property on an object (nullopt / false when there is none or it threw)
std::optional<bool> get_bool_property(::REManagedObject* obj, const char* property) {
    const auto td = obj != nullptr ? utility::re_managed_object::get_type_definition(obj) : nullptr;
    const auto m = td != nullptr ? td->get_method(std::string{"get_"} + property) : nullptr;

    if (m == nullptr || m->get_function() == nullptr) {
        return std::nullopt;
    }

    try {
        return m->call_safe<bool>(sdk::get_thread_context(), obj);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool set_bool_property(::REManagedObject* obj, const char* property, bool value) {
    const auto td = obj != nullptr ? utility::re_managed_object::get_type_definition(obj) : nullptr;
    const auto m = td != nullptr ? td->get_method(std::string{"set_"} + property) : nullptr;

    if (m == nullptr || m->get_function() == nullptr) {
        return false;
    }

    try {
        m->call_safe<void>(sdk::get_thread_context(), obj, value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// the component still belongs to a live game object (as TemporalUpscaler's post processing options check it)
bool component_alive(::REManagedObject* c) {
    static const auto game_object_t = sdk::find_type_definition("via.GameObject");
    static const auto get_valid = game_object_t != nullptr ? game_object_t->get_method("get_Valid") : nullptr;
    const auto owner = ((::REComponent*)c)->ownerGameObject;

    if (c->referenceCount <= 1 || owner == nullptr) {
        return false;
    }

    try {
        return get_valid == nullptr || get_valid->call_safe<bool>(sdk::get_thread_context(), (::REManagedObject*)owner);
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ftest_start() {
    if (any_test_running()) {
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("Engine effect cost test: GPU timing could not be started."));
        return;
    }

    m_ftest_rows.clear();
    m_ftest_objects.clear();
    m_ftest_running = true;
    m_ftest_phase = -1;
    m_ftest_feature = -1;
    m_ftest_measuring = false;
    m_ftest_phase_start = Clock::now();

    std::string list{};

    for (int i = 0; i < FTEST_FEATURE_COUNT; ++i) {
        list += std::format("{}{} ({})", i > 0 ? ", " : "", FTEST_FEATURES[i].label, FTEST_FEATURES[i].type_name);
    }

    REVR_LOG(debug_log(build_snapshot("engine effect cost test start") +
              std::format("  [EFFECT TEST] phases of {:.0f} s (first {:.1f} s not counted) after a {:.0f} s countdown: everything on, then each effect off "
                          "with everything on again after it: {}\n",
                  FTEST_PHASE_SECONDS, FTEST_WARMUP_SECONDS, FTEST_COUNTDOWN_SECONDS, list)));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// the switched-off components get their own value back, our references go
// [LIGHT SHAFT OFF 13.11.2026] RE7 tests of 13.11.2026 with the fovea views' exposure the game camera's (calcRealEV): the same
// brightness where the sun is inside the fovea (the house, the sun by the chimney: periphery = 1.006 x fovea), but with the sun
// just above the fovea (the gate) the periphery up to 3.5x brighter at the top of the fovea, falling off downwards. The game's
// bloom, lens flares, volumetrics and god rays are off in that config; the effect test's "light shafts" switch only the
// cameras' LightShaftFilterControl. The scene has one light shaft entity (LightShaft_: fog density 1.5, Rayleigh / Mie
// scattering, angular attenuation on) and the game has a VR controller that switches light shafts off for its stereo mode.
void FoveatedRendering::light_shafts_apply(bool off, bool announce) {
    const auto scene = sdk::get_current_scene();
    std::string report{};
    int switched = 0;

    if (!off) {
        int restored = 0;

        for (const auto& [c, prop] : m_ls_objects) {
            if (component_alive(c) && set_bool_property(c, prop.c_str(), true)) {
                ++restored;
            }

            utility::re_managed_object::release(c);
        }

        report += std::format("{} light shaft entities switched on again", restored);
        m_ls_objects.clear();
    }

    if (scene != nullptr) {
        // the scene's light shaft entities
        if (off) {
            static const auto t = sdk::find_type_definition("via.render.LightShaft");
            const auto rt = t != nullptr ? t->get_runtime_type() : nullptr;
            sdk::SystemArray* arr = nullptr;

            try {
                arr = rt != nullptr ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", rt) : nullptr;
            } catch (const std::exception&) {
                arr = nullptr;
            }

            int found = 0;
            std::string owners{};

            for (auto* c : arr != nullptr ? arr->get_elements() : std::vector<::REManagedObject*>{}) {
                if (c == nullptr) {
                    continue;
                }

                ++found;
                const auto go = ((::REComponent*)c)->ownerGameObject;
                std::string how{"no Enabled / Visible / DrawEnable setter"};

                for (const char* prop : {"Enabled", "Visible", "DrawEnable"}) {
                    const auto v = get_bool_property(c, prop);

                    if (!v.has_value()) {
                        continue;
                    }

                    if (!*v) {
                        how = std::format("{} already off", prop);
                        break;
                    }

                    if (set_bool_property(c, prop, false)) {
                        utility::re_managed_object::add_ref(c);
                        m_ls_objects.emplace_back(c, prop);
                        ++switched;
                        how = std::format("{} switched off", prop);
                        break;
                    }
                }

                owners += std::format("{}{}: {}", owners.empty() ? "" : ", ",
                    (go != nullptr && go->name != nullptr) ? utility::narrow(utility::re_string::get_view(go->name)) : std::string{"?"}, how);
            }

            report += std::format("{} light shaft entities, {} switched off now ({})", found, switched, owners);
        }

        // the game's VR controller of the light shafts
        static const auto vt = sdk::find_type_definition("app.vr.VrLightShaftController");
        const auto vrt = vt != nullptr ? vt->get_runtime_type() : nullptr;
        sdk::SystemArray* varr = nullptr;

        try {
            varr = vrt != nullptr ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", vrt) : nullptr;
        } catch (const std::exception&) {
            varr = nullptr;
        }

        int controllers = 0;

        for (auto* c : varr != nullptr ? varr->get_elements() : std::vector<::REManagedObject*>{}) {
            if (c == nullptr) {
                continue;
            }

            try {
                sdk::call_object_func_easy<void*>(c, "setLightShaftEnable(System.Boolean)", !off);
                ++controllers;
            } catch (const std::exception&) {
            }
        }

        report += std::format(" | the game's VR light shaft controllers: {} told {}", controllers, off ? "off" : "on");
    }

    // when asked from the menu or when the repeat found new ones, not every 90 frames
    if (announce || switched > 0) {
        REVR_LOG(debug_log(std::format("[LIGHT SHAFT OFF] {}", report)));
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ftest_restore() {
    if (m_ftest_renderer_off) {
        set_dynamic_shadows(true);
        m_ftest_renderer_off = false;
    }

    if (m_ftest_feature >= 0 && m_ftest_feature < FTEST_FEATURE_COUNT) {
        const auto& f = FTEST_FEATURES[m_ftest_feature];

        for (const auto& [c, original] : m_ftest_objects) {
            if (component_alive(c)) {
                set_bool_property(feature_target(c, f), f.property, original);
            }
        }
    }

    for (const auto& [c, original] : m_ftest_objects) {
        utility::re_managed_object::release(c);
    }

    m_ftest_objects.clear();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ftest_cancel(const char* why) {
    if (!m_ftest_running) {
        return;
    }

    ftest_restore();
    m_ftest_running = false;
    m_ftest_phase = -1;
    m_ftest_feature = -1;
    m_ftest_measuring = false;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"Engine effect cost test cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"[EFFECT TEST] cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// The next effect to switch off from `from` on: its components found in the scene, the ones that are on switched off.
// -1 when there is none left. `report` gets one line per effect left out.
int FoveatedRendering::ftest_switch_off(int from, std::string& report) {
    const auto scene = sdk::get_current_scene();

    for (int k = from; k < FTEST_FEATURE_COUNT; ++k) {
        const auto& f = FTEST_FEATURES[k];

        // [VIEW DIFFERENCE 02.11.2026] the renderer's dynamic shadows (one switch for all views)
        if (std::string_view{f.type_name} == "renderer") {
            if (!get_dynamic_shadows().value_or(false) || !set_dynamic_shadows(false)) {
                report += std::format("[EFFECT TEST] {}: left out (off or not readable)\n", f.label);
                continue;
            }

            m_ftest_renderer_off = true;
            report += std::format("[EFFECT TEST] {}: switched off (via.render.Renderer)\n", f.label);
            return k;
        }
        const auto t = sdk::find_type_definition(f.type_name);
        const auto rt = t != nullptr ? t->get_runtime_type() : nullptr;
        sdk::SystemArray* arr = nullptr;

        try {
            arr = (scene != nullptr && rt != nullptr) ? sdk::call_object_func_easy<sdk::SystemArray*>(scene, "findComponents(System.Type)", rt) : nullptr;
        } catch (const std::exception&) {
            arr = nullptr;
        }

        std::vector<::REManagedObject*> found{};

        if (arr != nullptr) {
            for (auto* c : arr->get_elements()) {
                if (c != nullptr && std::find(found.begin(), found.end(), c) == found.end()) {
                    found.push_back(c);
                }
            }
        }

        int on = 0;
        std::string owners{};

        for (auto* c : found) {
            const auto value = get_bool_property(feature_target(c, f), f.property);
            const auto go = ((::REComponent*)c)->ownerGameObject;
            owners += std::format("{}{}={}", owners.empty() ? "" : ", ",
                (go != nullptr && go->name != nullptr) ? utility::narrow(utility::re_string::get_view(go->name)) : std::string{"?"},
                value.has_value() ? (*value ? "on" : "off") : "?");

            if (value.value_or(false)) {
                ++on;
            }
        }

        if (on == 0) {
            report += std::format("[EFFECT TEST] {}: left out ({} components{}{})\n", f.label, found.size(), found.empty() ? "" : ": ", owners);
            continue;
        }

        for (auto* c : found) {
            if (const auto value = get_bool_property(feature_target(c, f), f.property); value.value_or(false)) {
                utility::re_managed_object::add_ref(c);
                m_ftest_objects.emplace_back(c, true);
                set_bool_property(feature_target(c, f), f.property, false);
            }
        }

        report += std::format("[EFFECT TEST] {}: {} of {} components switched off ({})\n", f.label, on, found.size(), owners);
        return k;
    }

    return -1;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ftest_tick(double dt_ms) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_ftest_phase_start).count();

    if (m_ftest_phase < 0) {
        REVR_LOG(set_message(std::format("Engine effect cost test starts in {:.0f} s: close the menu, stand still and keep looking at the same spot.",
            (std::max)(FTEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        if (elapsed >= FTEST_COUNTDOWN_SECONDS) {
            m_ftest_phase = 0; // everything on
            m_ftest_feature = -1;
            m_ftest_next = 0;
            m_ftest_measuring = false;
            m_ftest_phase_start = now;
        }

        return;
    }

    const bool off_phase = m_ftest_feature >= 0;

    // the game may switch an effect on again: kept off every frame during its phase
    if (off_phase) {
        const auto& f = FTEST_FEATURES[m_ftest_feature];

        if (m_ftest_renderer_off && get_dynamic_shadows().value_or(false)) {
            set_dynamic_shadows(false);
            ++m_ftest_reapplied;
        }

        for (const auto& [c, original] : m_ftest_objects) {
            if (component_alive(c)) {
                auto* target = feature_target(c, f);

                if (get_bool_property(target, f.property).value_or(false)) {
                    set_bool_property(target, f.property, false);
                    ++m_ftest_reapplied;
                }
            }
        }
    }

    if (!m_ftest_measuring && elapsed >= FTEST_WARMUP_SECONDS) {
        d3d12::GpuSpans::get().take();
        m_gpu_timer.take();
        m_ftest_vd_sum = 0.0;
        m_ftest_vdm_sum = 0.0;
        m_ftest_vd_n = 0;
        m_ftest_vd_seen = quadview::State::get().view_diff_frames.load();
        m_ftest_cpu_sum = 0.0;
        m_ftest_cpu_n = 0;
        m_ftest_rt_sum = 0.0;
        m_ftest_rt_n = 0;
        m_ftest_engaged = true;
        m_ftest_reapplied = 0;
        m_ftest_measuring = true;
    }

    if (m_ftest_measuring && dt_ms > 0.0) {
        m_ftest_cpu_sum += dt_ms;
        ++m_ftest_cpu_n;
        m_ftest_engaged = m_ftest_engaged && quadview::State::get().engaged();

        // [VIEW DIFFERENCE] a new measurement since the last frame: both eyes
        const auto& q = quadview::State::get();

        if (const auto seen = q.view_diff_frames.load(); seen != m_ftest_vd_seen) {
            m_ftest_vd_seen = seen;
            m_ftest_vd_sum += 0.5 * ((double)q.view_diff[0].load() + (double)q.view_diff[1].load());
            m_ftest_vdm_sum += 0.5 * ((double)q.view_diff_matched[0].load() + (double)q.view_diff_matched[1].load());
            ++m_ftest_vd_n;
        }
    }

    REVR_LOG(set_message(std::format("Engine effect cost test, phase {}: {} ({:.0f}/{:.0f} s). Stand still.", m_ftest_phase + 1,
        off_phase ? std::string{FTEST_FEATURES[m_ftest_feature].label} + " OFF" : std::string{"everything on"}, elapsed, FTEST_PHASE_SECONDS)));

    if (elapsed < FTEST_PHASE_SECONDS) {
        return;
    }

    FeatureTestRow row{};
    row.feature = m_ftest_feature;
    row.objects = (uint32_t)m_ftest_objects.size();
    row.reapplied = m_ftest_reapplied;
    row.engaged = m_ftest_engaged && quadview::State::get().engaged();
    row.frames = m_ftest_cpu_n;
    row.cpu_ms = m_ftest_cpu_n > 0 ? m_ftest_cpu_sum / (double)m_ftest_cpu_n : 0.0;
    row.render_time_ms = m_ftest_rt_n > 0 ? m_ftest_rt_sum / (double)m_ftest_rt_n : -1.0;
    row.gpu = m_gpu_timer.take();
    row.spans = d3d12::GpuSpans::get().take();
    row.view_diff_n = m_ftest_vd_n;
    row.view_diff = m_ftest_vd_n > 0 ? m_ftest_vd_sum / m_ftest_vd_n : 0.0;
    row.view_diff_matched = m_ftest_vd_n > 0 ? m_ftest_vdm_sum / m_ftest_vd_n : 0.0;
    m_ftest_rows.push_back(row);

    const double ours = ours_in_frame_ms(row.spans);
    REVR_LOG(debug_log(std::format("[EFFECT TEST] phase {} done: {} | engaged={} frames={} cpu_frame_ms={:.2f} | frame GPU {:.3f} ms, engine {:.3f} ms, the engine's "
                          "render time {:.3f} ms{} | fovea/periphery difference {:.2f}% (one gain), {:.2f}% (as matched), {} measurements",
        m_ftest_phase + 1, off_phase ? std::string{FTEST_FEATURES[m_ftest_feature].label} + " off" : std::string{"everything on"}, row.engaged, row.frames,
        row.cpu_ms, row.gpu.avg_ms, row.gpu.avg_ms - ours, row.render_time_ms,
        row.reapplied > 0 ? std::format(" (the game switched it on again {} times, kept off)", row.reapplied) : std::string{},
        row.view_diff * 100.0, row.view_diff_matched * 100.0, row.view_diff_n)));

    ++m_ftest_phase;
    m_ftest_measuring = false;
    m_ftest_phase_start = now;

    if (off_phase) {
        // back on, then a phase with everything on
        ftest_restore();
        m_ftest_feature = -1;
        return;
    }

    // after an "everything on" phase: the next effect, or the end
    std::string report{};
    const int k = m_ftest_next < FTEST_FEATURE_COUNT ? ftest_switch_off(m_ftest_next, report) : -1;

    if (!report.empty()) {
        REVR_LOG(debug_log(report.substr(0, report.size() - 1)));
    }

    if (k < 0) {
        ftest_finish();
        return;
    }

    m_ftest_feature = k;
    m_ftest_next = k + 1;
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::ftest_finish() {
    struct Cost {
        int feature;
        double engine;
        double render_time;
        bool ok;
    };

    std::vector<Cost> costs{};
    double on_lo = 1e9, on_hi = -1e9;

    const auto engine_of = [](const FeatureTestRow& r) { return r.gpu.avg_ms - ours_in_frame_ms(r.spans); };

    for (const auto& r : m_ftest_rows) {
        if (r.feature < 0 && r.engaged && r.gpu.samples > 0) {
            on_lo = (std::min)(on_lo, engine_of(r));
            on_hi = (std::max)(on_hi, engine_of(r));
        }
    }

    for (size_t j = 0; j < m_ftest_rows.size(); ++j) {
        const auto& r = m_ftest_rows[j];

        if (r.feature < 0) {
            continue;
        }

        // the "everything on" phases right before and after it
        const FeatureTestRow* before = j > 0 && m_ftest_rows[j - 1].feature < 0 ? &m_ftest_rows[j - 1] : nullptr;
        const FeatureTestRow* after = j + 1 < m_ftest_rows.size() && m_ftest_rows[j + 1].feature < 0 ? &m_ftest_rows[j + 1] : nullptr;
        const bool ok = before != nullptr && after != nullptr && r.engaged && before->engaged && after->engaged && r.gpu.samples > 0;
        Cost c{r.feature, 0.0, 0.0, ok};

        if (before != nullptr && after != nullptr) {
            c.engine = (engine_of(*before) + engine_of(*after)) * 0.5 - engine_of(r);
            c.render_time = (before->render_time_ms + after->render_time_ms) * 0.5 - r.render_time_ms;
        }

        costs.push_back(c);
    }

    std::sort(costs.begin(), costs.end(), [](const Cost& a, const Cost& b) { return a.engine > b.engine; });

    std::string o = std::format("[EFFECT TEST] RESULT (GPU ms per frame, all views together; the phases with everything on spread {:.3f} .. {:.3f} ms of engine time: "
                                "smaller differences are noise)\n",
        on_lo, on_hi);

    for (const auto& c : costs) {
        o += std::format("  {:<24} costs {:+.3f} ms (the engine's render time {:+.3f}){}\n", FTEST_FEATURES[c.feature].label, c.engine, c.render_time,
            c.ok ? "" : " (not reliable: a phase around it was not complete)");
    }

    if (costs.empty()) {
        o += "  no effect could be switched off\n";
    }

    // [VIEW DIFFERENCE 01.11.2026] how different fovea and periphery are with each effect off, against the phases with
    // everything on right before and after it (an effect that makes the two views differ lowers it when off)
    o += "[EFFECT TEST] fovea / periphery difference (blocks of 16 px, % of the brightness; one gain = the views' own difference, "
         "as matched = with the build's gain / table, before the large-scale light):\n";

    for (size_t j = 0; j < m_ftest_rows.size(); ++j) {
        const auto& r = m_ftest_rows[j];

        if (r.feature < 0) {
            continue;
        }

        const FeatureTestRow* before = j > 0 && m_ftest_rows[j - 1].feature < 0 ? &m_ftest_rows[j - 1] : nullptr;
        const FeatureTestRow* after = j + 1 < m_ftest_rows.size() && m_ftest_rows[j + 1].feature < 0 ? &m_ftest_rows[j + 1] : nullptr;

        if (before == nullptr || after == nullptr || r.view_diff_n == 0 || before->view_diff_n == 0 || after->view_diff_n == 0) {
            o += std::format("  {:<24} not measured\n", FTEST_FEATURES[r.feature].label);
            continue;
        }

        const double on = 0.5 * (before->view_diff + after->view_diff);
        const double on_m = 0.5 * (before->view_diff_matched + after->view_diff_matched);
        o += std::format("  {:<24} off {:.2f}% against on {:.2f}% ({:+.2f}; on before / after {:.2f} / {:.2f}) | as matched off {:.2f}% against {:.2f}%\n",
            FTEST_FEATURES[r.feature].label, r.view_diff * 100.0, on * 100.0, (r.view_diff - on) * 100.0, before->view_diff * 100.0, after->view_diff * 100.0,
            r.view_diff_matched * 100.0, on_m * 100.0);
    }

    m_ftest_running = false;
    m_ftest_phase = -1;
    m_ftest_feature = -1;
    REVR_LOG(debug_log(o + build_snapshot("engine effect cost test end")));
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("Engine effect cost test finished. Result written to foveation_debug.log"));
}
#endif

// =================================================================================================
// [RENDER SETTINGS 30.10.2026] What the ambient occlusion costs, and what a lighter one in the wide view only would save:
// phases with everything as it is and phases with one setting at another value, alternating. A setting is either the
// game's (via.render.RenderConfig) or one of the AO component of the cameras (via.render.SSAOControl): of the wide view's
// camera only, or of every view's. A switch is tried off, a quality at the values below the current one. During a phase
// every camera is kept at its value each frame (the camera clones get the game camera's properties copied to them now and
// then); afterwards each object gets its own value back. The image changes during those phases (measurement only). All on
// the game thread.
// =================================================================================================
#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
namespace {
enum class SettingKind { RenderConfig, CameraComponent };

struct RenderSetting {
    const char* label;
    SettingKind target;
    const char* component; // the camera component (CameraComponent)
    const char* getter;
    const char* setter;
    bool wide_only;        // CameraComponent: the wide view's camera only (the fovea cameras keep their value)
};

constexpr RenderSetting RTEST_SETTINGS[] = {
    {"ambient occlusion (game setting)", SettingKind::RenderConfig, nullptr, "get_AOSetting", "set_AOSetting", false},
    {"AO sample quality, wide view", SettingKind::CameraComponent, "via.render.SSAOControl", "getSampleQuality", "setSampleQuality", true},
    {"AO sample quality, all views", SettingKind::CameraComponent, "via.render.SSAOControl", "getSampleQuality", "setSampleQuality", false},
    {"AO filter quality, wide view", SettingKind::CameraComponent, "via.render.SSAOControl", "getFilterQuality", "setFilterQuality", true},
    {"AO distance field, wide view", SettingKind::CameraComponent, "via.render.SSAOControl", "get_DFAO", "set_DFAO", true},
    {"AO distance field, all views", SettingKind::CameraComponent, "via.render.SSAOControl", "get_DFAO", "set_DFAO", false},
    {"god rays (game setting)", SettingKind::RenderConfig, nullptr, "get_GodRayEnable", "set_GodRayEnable", false},
    {"chromatic aberration (game setting)", SettingKind::RenderConfig, nullptr, "get_ChromaticAberration", "set_ChromaticAberration", false},
    {"film grain (game setting)", SettingKind::RenderConfig, nullptr, "get_FilmGrainEnable", "set_FilmGrainEnable", false},
};
constexpr int RTEST_SETTING_COUNT = (int)(sizeof(RTEST_SETTINGS) / sizeof(RTEST_SETTINGS[0]));
constexpr double RTEST_COUNTDOWN_SECONDS = 8.0;
constexpr double RTEST_PHASE_SECONDS = 9.0;
constexpr double RTEST_WARMUP_SECONDS = 4.0; // a changed setting can make the engine build things first

::REManagedObject* render_config() {
    static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");
    static const auto get = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;

    if (renderer_t == nullptr || get == nullptr || get->get_function() == nullptr) {
        return nullptr;
    }

    try {
        return get->call_safe<::REManagedObject*>(sdk::get_thread_context(), renderer_t->get_instance());
    } catch (const std::exception&) {
        return nullptr;
    }
}

// a bool or enum (int) value read with a getter without parameters; nullopt when it cannot be read
std::optional<int32_t> get_setting(::REManagedObject* obj, const char* getter, bool* is_bool = nullptr, const sdk::RETypeDefinition** enum_type = nullptr) {
    const auto td = obj != nullptr ? utility::re_managed_object::get_type_definition(obj) : nullptr;
    const auto m = td != nullptr ? td->get_method(getter) : nullptr;

    if (m == nullptr || m->get_function() == nullptr || m->get_return_type() == nullptr || m->get_num_params() != 0) {
        return std::nullopt;
    }

    const auto* rt = m->get_return_type();
    const bool b = rt->get_full_name() == "System.Boolean";

    if (!b && !rt->is_enum()) {
        return std::nullopt;
    }

    if (is_bool != nullptr) {
        *is_bool = b;
    }

    if (enum_type != nullptr) {
        *enum_type = b ? nullptr : rt;
    }

    try {
        return b ? (m->call_safe<bool>(sdk::get_thread_context(), obj) ? 1 : 0) : m->call_safe<int32_t>(sdk::get_thread_context(), obj);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool set_setting(::REManagedObject* obj, const char* setter, bool is_bool, int32_t value) {
    const auto td = obj != nullptr ? utility::re_managed_object::get_type_definition(obj) : nullptr;
    const auto m = td != nullptr ? td->get_method(setter) : nullptr;

    if (m == nullptr || m->get_function() == nullptr) {
        return false;
    }

    try {
        if (is_bool) {
            m->call_safe<void>(sdk::get_thread_context(), obj, value != 0);
        } else {
            m->call_safe<void>(sdk::get_thread_context(), obj, value);
        }
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

// the name of an enum value ("" when not found)
std::string enum_name(const sdk::RETypeDefinition* t, int32_t value) {
    if (t == nullptr) {
        return {};
    }

    for (auto* f : t->get_fields()) {
        if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal()) {
            continue;
        }

        if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr && *d == value) {
            return f->get_name();
        }
    }

    return {};
}

// the objects a setting is written on, in view order (render config: one; camera component: wide view, left, right fovea)
std::vector<::REManagedObject*> setting_objects(const RenderSetting& r) {
    std::vector<::REManagedObject*> out{};

    if (r.target == SettingKind::RenderConfig) {
        if (auto* cfg = render_config(); cfg != nullptr) {
            out.push_back(cfg);
        }

        return out;
    }

    const auto scenes = VR::get()->get_camera_duplicator().get_relevant_scene_layers();

    for (size_t v = 0; v < scenes.size() && v < 3; ++v) {
        auto* cam = scenes[v] != nullptr ? scenes[v]->get_camera() : nullptr;
        ::REManagedObject* comp = nullptr;

        if (cam != nullptr && cam->ownerGameObject != nullptr && cam->ownerGameObject->transform != nullptr) {
            comp = (::REManagedObject*)utility::re_component::find<REComponent>(cam->ownerGameObject->transform, r.component);
        }

        out.push_back(comp);
    }

    return out;
}
} // namespace
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::rtest_start() {
    if (any_test_running()) {
        return;
    }

    // the variants: a switch off, a quality at each value below the current one (the current value from the first object)
    m_rtest_variants.clear();
    std::string plan{};

    for (int i = 0; i < RTEST_SETTING_COUNT; ++i) {
        const auto& r = RTEST_SETTINGS[i];
        const auto objs = setting_objects(r);
        bool is_bool = false;
        const sdk::RETypeDefinition* et = nullptr;
        const auto cur = !objs.empty() && objs[0] != nullptr ? get_setting(objs[0], r.getter, &is_bool, &et) : std::nullopt;

        if (!cur.has_value()) {
            plan += std::format("  [AO TEST] {} ({}): not readable ({} objects), left out\n", r.label, r.getter, objs.size());
            continue;
        }

        std::string values{};

        for (size_t k = 0; k < objs.size(); ++k) {
            const auto v = objs[k] != nullptr ? get_setting(objs[k], r.getter) : std::nullopt;
            values += std::format("{}{}", k > 0 ? " / " : "", v.has_value() ? std::to_string(*v) : std::string{"?"});
        }

        const std::string cur_name = is_bool ? (*cur != 0 ? "on" : "off") : std::format("{} {}", *cur, enum_name(et, *cur));
        std::string tried{};

        if (is_bool) {
            if (*cur != 0) {
                m_rtest_variants.push_back({i, 0, "off", is_bool});
                tried = "off";
            }
        } else {
            for (int32_t v = 0; v < *cur && v < 16; ++v) {
                const auto name = std::format("{} {}", v, enum_name(et, v));
                m_rtest_variants.push_back({i, v, name, is_bool});
                tried += (tried.empty() ? "" : ", ") + name;
            }
        }

        plan += std::format("  [AO TEST] {} ({}): now {} (per object: {}); tried: {}\n", r.label, r.getter, cur_name, values,
            tried.empty() ? std::string{"nothing"} : tried);
    }

    if (m_rtest_variants.empty()) {
        REVR_LOG(set_message("Ambient occlusion cost test: nothing to try."));
        REVR_LOG(debug_log(plan));
        return;
    }

    if (!gpu_timer_enable(true)) {
        REVR_LOG(set_message("Ambient occlusion cost test: GPU timing could not be started."));
        return;
    }

    m_rtest_rows.clear();
    m_rtest_targets.clear();
    m_rtest_running = true;
    m_rtest_phase = -1;
    m_rtest_variant = -1;
    m_rtest_next = 0;
    m_rtest_measuring = false;
    m_rtest_phase_start = Clock::now();
    REVR_LOG(debug_log(build_snapshot("ambient occlusion cost test start") +
              std::format("  [AO TEST] phases of {:.0f} s (first {:.0f} s not counted) after a {:.0f} s countdown; everything as it is between them\n",
                  RTEST_PHASE_SECONDS, RTEST_WARMUP_SECONDS, RTEST_COUNTDOWN_SECONDS) +
              plan));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// the variant's objects: each with the value of the phase and its own value (kept with a reference of ours)
void FoveatedRendering::rtest_apply_variant() {
    const auto& v = m_rtest_variants[(size_t)m_rtest_variant];
    const auto& r = RTEST_SETTINGS[v.setting];
    const auto objs = setting_objects(r);

    for (size_t k = 0; k < objs.size(); ++k) {
        auto* o = objs[k];
        const auto orig = o != nullptr ? get_setting(o, r.getter) : std::nullopt;

        if (!orig.has_value()) {
            continue;
        }

        // the wide view's camera only: the fovea cameras are kept at their own value (the copy from the game camera)
        const bool change = r.target == SettingKind::RenderConfig || !r.wide_only || k == 0;
        utility::re_managed_object::add_ref(o);
        m_rtest_targets.push_back({o, *orig, change ? v.value : *orig});
        set_setting(o, r.setter, v.is_bool, change ? v.value : *orig);
    }
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
// every object back to its own value, our references go
void FoveatedRendering::rtest_restore() {
    if (m_rtest_variant >= 0 && m_rtest_variant < (int)m_rtest_variants.size()) {
        const auto& v = m_rtest_variants[(size_t)m_rtest_variant];
        const auto& r = RTEST_SETTINGS[v.setting];

        for (const auto& t : m_rtest_targets) {
            if (r.target == SettingKind::RenderConfig || component_alive(t.object)) {
                set_setting(t.object, r.setter, v.is_bool, t.original);
            }
        }
    }

    for (const auto& t : m_rtest_targets) {
        utility::re_managed_object::release(t.object);
    }

    m_rtest_targets.clear();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::rtest_cancel(const char* why) {
    if (!m_rtest_running) {
        return;
    }

    rtest_restore();
    m_rtest_running = false;
    m_rtest_phase = -1;
    m_rtest_variant = -1;
    m_rtest_measuring = false;
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message(std::string{"Ambient occlusion cost test cancelled: "} + why));
    REVR_LOG(debug_log(std::string{"[AO TEST] cancelled: "} + why));
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::rtest_tick(double dt_ms) {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_rtest_phase_start).count();

    if (m_rtest_phase < 0) {
        REVR_LOG(set_message(std::format("Ambient occlusion cost test starts in {:.0f} s: close the menu, stand still and keep looking at the same spot.",
            (std::max)(RTEST_COUNTDOWN_SECONDS - elapsed, 0.0))));

        if (elapsed >= RTEST_COUNTDOWN_SECONDS) {
            m_rtest_phase = 0;
            m_rtest_variant = -1;
            m_rtest_measuring = false;
            m_rtest_phase_start = now;
        }

        return;
    }

    const bool variant_phase = m_rtest_variant >= 0;
    std::string label = "everything as it is";

    if (variant_phase) {
        const auto& v = m_rtest_variants[(size_t)m_rtest_variant];
        const auto& r = RTEST_SETTINGS[v.setting];
        label = std::format("{} at {}", r.label, v.name);

        // the game (or the copy to the camera clones) may write it again: every object kept at the phase's value
        for (const auto& t : m_rtest_targets) {
            if (r.target == SettingKind::CameraComponent && !component_alive(t.object)) {
                continue;
            }

            if (get_setting(t.object, r.getter).value_or(t.value) != t.value) {
                set_setting(t.object, r.setter, v.is_bool, t.value);
                ++m_rtest_reapplied;
            }
        }
    }

    if (!m_rtest_measuring && elapsed >= RTEST_WARMUP_SECONDS) {
        d3d12::GpuSpans::get().take();
        m_gpu_timer.take();
        m_rtest_cpu_sum = 0.0;
        m_rtest_cpu_n = 0;
        m_rtest_rt_sum = 0.0;
        m_rtest_rt_n = 0;
        m_rtest_engaged = true;
        m_rtest_reapplied = 0;
        m_rtest_measuring = true;
    }

    if (m_rtest_measuring && dt_ms > 0.0) {
        m_rtest_cpu_sum += dt_ms;
        ++m_rtest_cpu_n;
        m_rtest_engaged = m_rtest_engaged && quadview::State::get().engaged();
    }

    REVR_LOG(set_message(std::format("Ambient occlusion cost test, phase {}: {} ({:.0f}/{:.0f} s). Stand still.", m_rtest_phase + 1, label, elapsed, RTEST_PHASE_SECONDS)));

    if (elapsed < RTEST_PHASE_SECONDS) {
        return;
    }

    SettingTestRow row{};
    row.variant = m_rtest_variant;
    row.reapplied = m_rtest_reapplied;
    row.engaged = m_rtest_engaged && quadview::State::get().engaged();
    row.frames = m_rtest_cpu_n;
    row.cpu_ms = m_rtest_cpu_n > 0 ? m_rtest_cpu_sum / (double)m_rtest_cpu_n : 0.0;
    row.render_time_ms = m_rtest_rt_n > 0 ? m_rtest_rt_sum / (double)m_rtest_rt_n : -1.0;
    row.gpu = m_gpu_timer.take();
    row.spans = d3d12::GpuSpans::get().take();
    m_rtest_rows.push_back(row);

    const double ours = ours_in_frame_ms(row.spans);
    REVR_LOG(debug_log(std::format("[AO TEST] phase {} done: {} | engaged={} frames={} cpu_frame_ms={:.2f} | frame GPU {:.3f} ms, engine {:.3f} ms, the engine's "
                          "render time {:.3f} ms{}",
        m_rtest_phase + 1, label, row.engaged, row.frames, row.cpu_ms, row.gpu.avg_ms, row.gpu.avg_ms - ours, row.render_time_ms,
        row.reapplied > 0 ? std::format(" (written again {} times, kept)", row.reapplied) : std::string{})));

    ++m_rtest_phase;
    m_rtest_measuring = false;
    m_rtest_phase_start = now;

    if (variant_phase) {
        rtest_restore();
        m_rtest_variant = -1;
        return;
    }

    if (m_rtest_next >= (int)m_rtest_variants.size()) {
        rtest_finish();
        return;
    }

    m_rtest_variant = m_rtest_next++;
    rtest_apply_variant();
}
#endif

#if !REVR_RELEASE // [RELEASE SWITCH fix141] debug build only
void FoveatedRendering::rtest_finish() {
    const auto engine_of = [](const SettingTestRow& r) { return r.gpu.avg_ms - ours_in_frame_ms(r.spans); };
    double lo = 1e9, hi = -1e9, rlo = 1e9, rhi = -1e9;

    for (const auto& r : m_rtest_rows) {
        if (r.variant < 0 && r.engaged && r.gpu.samples > 0) {
            lo = (std::min)(lo, engine_of(r));
            hi = (std::max)(hi, engine_of(r));
            rlo = (std::min)(rlo, r.render_time_ms);
            rhi = (std::max)(rhi, r.render_time_ms);
        }
    }

    std::string o = std::format("[AO TEST] RESULT (GPU ms per frame, all views; + = the setting as it is costs this much more than the value tried). "
                                "Phases as it is: engine {:.3f} .. {:.3f}, the engine's render time {:.3f} .. {:.3f} (smaller differences are noise)\n",
        lo, hi, rlo, rhi);

    for (size_t j = 0; j < m_rtest_rows.size(); ++j) {
        const auto& r = m_rtest_rows[j];

        if (r.variant < 0) {
            continue;
        }

        const auto& v = m_rtest_variants[(size_t)r.variant];
        const SettingTestRow* before = j > 0 && m_rtest_rows[j - 1].variant < 0 ? &m_rtest_rows[j - 1] : nullptr;
        const SettingTestRow* after = j + 1 < m_rtest_rows.size() && m_rtest_rows[j + 1].variant < 0 ? &m_rtest_rows[j + 1] : nullptr;

        if (before == nullptr || after == nullptr) {
            o += std::format("  {} at {}: no complete phases around it\n", RTEST_SETTINGS[v.setting].label, v.name);
            continue;
        }

        const bool ok = r.engaged && before->engaged && after->engaged && r.gpu.samples > 0;
        o += std::format("  {:<36} at {:<14} saves {:+.3f} ms (the engine's render time {:+.3f}){}\n", RTEST_SETTINGS[v.setting].label, v.name,
            (engine_of(*before) + engine_of(*after)) * 0.5 - engine_of(r), (before->render_time_ms + after->render_time_ms) * 0.5 - r.render_time_ms,
            ok ? "" : " (not reliable: tri-view not engaged in a phase)");
    }

    m_rtest_running = false;
    m_rtest_phase = -1;
    m_rtest_variant = -1;
    REVR_LOG(debug_log(o + build_snapshot("ambient occlusion cost test end")));
    gpu_timer_enable(m_gpu_timing_user);
    REVR_LOG(set_message("Ambient occlusion cost test finished. Result written to foveation_debug.log"));
}
#endif

// =================================================================================================
// UI
// =================================================================================================
void FoveatedRendering::draw_settings() {
    auto vr = VR::get();
    auto up = TemporalUpscaler::get();
    auto& gaze = EyeGaze::get();

    // ([UPSCALING MENU fix91 19.11.2026] the first block under the category's dividing line now: no second line of its own)
    g_framework->draw_menu_heading("TRI-VIEW FOVEATED RENDERING", false);

#if !REVR_RELEASE // [RELEASE SWITCH fix141] the build label and the stale object check: debug build only
    // [BUILD LABEL fix104 20.11.2026]
    ImGui::TextDisabled("Build %s", qv_build_text().c_str());

    // [BUILD STAMP fix107]
    if (static const auto problem = qv_sources_problem(); !problem.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Stale object files, rebuild the whole project: %s", problem.c_str());
    }
#endif

    bool changed = false;

    // [ALWAYS ON fix91 19.11.2026] The quad view is always on: the choice is between its two modes only. The stored value
    // stays the old one (1 fixed, 2 dynamic; key QuadViewMode), so a saved configuration keeps its mode. What the menu
    // showed with the mode Off (the explanation and "Debug tools (plain multipass)") is gone with it.
    {
        static const char* mode_names[] = {"Fixed (middle of the lens)", "Follows your eyes (eye tracking)"};
        int mode_index = mode() == Mode::DYNAMIC ? 1 : 0;

        g_framework->push_menu_toggle_style();
        ImGui::PushID(m_mode.get());

        if (menu_text::Combo("Sharp area position", &mode_index, mode_names, 2)) {
            m_mode->value() = (int32_t)(mode_index == 1 ? Mode::DYNAMIC : Mode::FIXED);
            changed = true;
        }

        QV_TIP("Only the middle of the image is drawn at full quality, the rest at a lower quality to save GPU power.\n"
               "Fixed: the sharp area stays in the middle of the view.\n"
               "Follows your eyes: it moves to where you look (needs eye tracking, like on the PS VR2).");

        ImGui::PopID();
        g_framework->pop_menu_toggle_style();
    }

    if (!vr->is_using_multipass()) {
        ImGui::TextWrapped("%s", "Not active: it needs the Multipass rendering mode.");
    } else if (!up->activated()) {
        // ([NO PLUGIN fix91 19.11.2026] no plugin is needed any more: what can be missing is Direct3D 12, or the first frame)
        ImGui::TextWrapped("%s", "Not active: it needs the game to run on DirectX 12.");
    }

    const int preset_before = m_preset->value();

#if !defined(RE4) && !REVR_RELEASE // [RELEASE fix132 23.11.2026] RE4: no preset list ("Custom"); [fix139] no release either
    if (m_preset->draw("Preset")) {
        if (m_preset->value() < PRESET_COUNT) {
            apply_preset(m_preset->value());
        }

        changed = true;
    }
#endif

    bool size_edit = false;
    {
        // [MENU STEP 11.11.2026] Pad and keyboard move a float slider by 1% of its range (0.0035 here), which the
        // three displayed decimals show as 3 or 4. One press moves the fovea size by exactly 0.001 instead; the
        // mouse (and a laser pointer driving it) still drags freely.
        const float before = m_fraction->value();
        const bool edited = m_fraction->draw("Sharp area size");
        QV_TIP("How big the sharp middle part of the image is (the rest is drawn at a lower quality).\n"
               "Bigger: sharp over more of what you see, but the GPU has more work.\n"
               "Smaller: faster, the image gets softer sooner away from the middle.");

        if (edited && ImGui::IsItemActive() && ImGui::GetCurrentContext()->ActiveIdSource != ImGuiInputSource_Mouse) {
            const float after = m_fraction->value();

            if (after != before && std::abs(after - before) < 0.02f) {
                const float stepped = std::round((before + (after > before ? 0.001f : -0.001f)) * 1000.0f) / 1000.0f;
                m_fraction->value() = std::clamp(stepped, QV_DEFAULT(0.20f, 0.25f), 0.60f); // [fix132]
            }
        }

        size_edit |= edited;
    }
    size_edit |= m_ratio->draw("Sharp area upscaling (1.0 = none, best image)");
    QV_TIP("1.0: the sharp area is drawn at full resolution, the best image.\n"
           "Higher: it is drawn smaller and then enlarged. Faster, but softer.");

    if (size_edit) {
        m_last_size_edit = Clock::now();
        m_size_edit_pending = true;

        if (preset_before != PRESET_CUSTOM) {
            m_preset->value() = PRESET_CUSTOM; // edited by hand
        }

        changed = true;
    }

    if (m_size_edit_pending) {
        menu_text::hint_text("%s", "The new size is used when you let go of the slider.");
    }

    // [PERIPHERY SUPER fix80 17.11.2026] what anti-aliases the periphery
    // [UPSCALING MENU fix91 19.11.2026] the light TAA is no longer offered: the list is drawn here with the two
    // supersampling choices (stored values 1 and 2 as before, key QuadViewPeripheryAA)
    // [CALM fix100 20.11.2026] third choice: the calm light TAA (stored value 3)
    {
        // [fix136 23.11.2026] the fovea's anti-aliasing (the supersampling TAA's history resolution, key
        // QuadViewSuperTAA_HistoryScale), up from the fine settings, above the periphery's ([fix137] every game)
        changed |= m_staa_scale->draw("Sharp area anti-aliasing");
        QV_TIP("Smooths jagged edges and shimmer in the sharp area.\n"
               "2x: sharper and steadier, uses more GPU power.\n"
               "1x: lighter, a little softer when you move.");
        static const char* periphery_names[] = {"Light",
            "Sharp (2x)",
            "Lightest (softer)"};
        int periphery_index = m_periphery_aa->value() == 1 ? 0 : m_periphery_aa->value() == 3 ? 2 : 1;

        ImGui::PushID(m_periphery_aa.get());

        if (menu_text::Combo("Outside area anti-aliasing", &periphery_index, periphery_names, 3)) {
            m_periphery_aa->value() = periphery_index + 1;
            changed = true;
        }

        ImGui::PopID();
    }

    // [fix81 17.11.2026] only while the pointer is on the option: in fix80 the tooltip was drawn in every frame (the test
    // was missing here), so it stayed on the screen whatever the pointer was on
    if (ImGui::IsItemHovered()) {
        menu_text::hint_tooltip("%s", "Smooths jagged edges and shimmer in the rest of the image, outside the sharp area.\n"
                                           "Sharp (2x): the most detail and the steadiest, uses more GPU power.\n"
                                           "Light: a good balance.\n"
                                           "Lightest: the softest and the cheapest.");
    }

    if (m_periphery_aa->value() != 0 && m_periphery_aa->value() != 3 && vr->is_mono_periphery()) {
        // (what the last frame's periphery got, when it is less than chosen)
        const auto running = up->periphery_running();
        const auto why = up->periphery_why();

        if (why != TemporalUpscaler::PW_NONE && (int)running != (int)m_periphery_aa->value()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.75f, 0.25f, 1.0f});
            (void)running;
            ImGui::TextWrapped("Outside area anti-aliasing is on a lighter setting for now (%s).", TemporalUpscaler::periphery_why_text(why));
            ImGui::PopStyleColor();
        }
    }

    if (m_periphery_aa->value() == 3 && vr->is_mono_periphery()) {
        // [CALM fix100 20.11.2026] (nothing to set: its values are those of the reference model)
    } else if (m_periphery_aa->value() == 0 || !vr->is_mono_periphery()) {
        changed |= m_taa_feedback->draw("Outside area steadiness (higher = calmer, more trails)");
        QV_TIP("Higher: calmer image outside the sharp area, but moving things can leave trails.");
    } else {
        // [PERIPHERY fix82 17.11.2026]
        changed |= m_periphery_stability->draw("Outside area steadiness");
        QV_TIP("Higher: less shimmer outside the sharp area. Lower: changes show sooner.");
        // [PERIPHERY fix83 17.11.2026]
        changed |= m_periphery_hist_sharp->draw("Outside area sharpness (0 = calmest)");
        QV_TIP("Higher: sharper outside the sharp area, but restless edges. 0 is the calmest.");
    }

    // [UPSCALING MENU fix91 19.11.2026] The choice "Fovea anti-aliasing" is gone, and with it what it showed for the upscaler
    // plugin (method, FSR 4 by the mod and its options, the two guards, linear light, sharpening) and for the mod's TAA
    // ("Fovea TAA settings"): the fovea is anti-aliased by the mod's supersampling TAA, whose "History resolution" chooses
    // between 2x and the lighter 1x. The values of the options no longer shown stay what the configuration has (they still
    // decide how the quad view is set up: FSR 4 by the mod, one pass, network, supersampling).
    // [GPU GUARD fix91 19.11.2026]
    const auto draw_guard_state = [&] {
        if (m_gpu_guard_opt->value() && m_staa_scale->value() == 0 &&
            !((m_periphery_aa->value() == 1 || m_periphery_aa->value() == 2) && vr->is_mono_periphery())) {
            menu_text::hint_text("%s", "Nothing to lower: both anti-aliasings are already on their lightest setting.");
        } else if (m_gpu_relief) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.75f, 0.25f, 1.0f});
            ImGui::TextWrapped("Anti-aliasing lowered for now (step %d of %d): the GPU is too busy.", m_gpu_level, m_gpu_step_count);
            ImGui::PopStyleColor();
        }
    };

    {
        g_framework->push_menu_toggle_style();
        changed |= m_gpu_guard_opt->draw("Lower the anti-aliasing when the GPU can't keep up");
        g_framework->pop_menu_toggle_style();

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("%s", "If the graphics card can't keep up and the game starts to stutter, the anti-aliasing is\n"
                                               "lowered for a while. It goes back up by itself when there is room again.");
        }

        // [GUARD START fix138 23.11.2026] from what GPU time per frame it steps in; [fix150] as a share of the headset's frame
        // time (steps of 1 %), with the milliseconds at the headset's refresh rate next to it
        if (m_gpu_guard_opt->value()) {
            const float frame_ms = m_hmd_frame_ms > 0.0f ? m_hmd_frame_ms : 1000.0f / 90.0f;
            float pct = m_gpu_guard_start_pct->value() * 100.0f;
            char fmt[64]{};
            std::snprintf(fmt, sizeof(fmt), "%%.0f%%%% of the frame (%.1f ms at %.0f Hz)", pct * 0.01f * frame_ms, 1000.0f / frame_ms);

            if (menu_text::SliderFloat("  Lower it from this share of the frame time##guardstart", &pct, 70.0f, 100.0f, fmt)) {
                m_gpu_guard_start_pct->value() = std::clamp(std::round(pct) / 100.0f, 0.70f, 1.00f);
            }

            // (read by the guard every half second; saved when the slider is let go, nothing of the quad view to set up again)
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_framework->request_save_config();
            }

            QV_TIP("When the graphics card needs more than this share of each frame's time, the anti-aliasing is lowered.\n"
                   "It follows your headset's refresh rate by itself (at 90 Hz a frame has 11.1 ms, at 120 Hz 8.3 ms).\n"
                   "Lower: it steps in sooner (smoother game, softer image). Higher: later (sharper image, but it can stutter).");
        }

        draw_guard_state();
    }

    // [RELEASE fix132 23.11.2026] The user's menu for the release: everything below the GPU guard's switch in a submenu
    // of its own, closed at first ("Advanced Options"). [fix137 23.11.2026] In every game (the user: RE7 and RE2 too).
    if (ImGui::TreeNode("Advanced Options")) {
        // [fix94 19.11.2026]
        g_framework->push_menu_toggle_style();
        changed |= m_ui_around->draw("Faster menus and HUD outside the sharp area");
        g_framework->pop_menu_toggle_style();

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("%s", "Keeps menus and HUD steady outside the sharp area with less work. Leave it on.");
        }

#if !REVR_RELEASE // [RELEASE SWITCH fix139] measurements and the crash report: debug build only
        g_framework->push_menu_toggle_style();
        changed |= m_diag_off->draw("Turn off the measurements");
        g_framework->pop_menu_toggle_style();

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("%s", "Stops the measurements written to the log file. The image does not change.");
        }

        // [DRED fix103 20.11.2026]
        g_framework->push_menu_toggle_style();
        changed |= m_debug_dred->draw("Graphics card crash report (from the next start, slower)");
        g_framework->pop_menu_toggle_style();

        if (ImGui::IsItemHovered()) {
            menu_text::hint_tooltip("%s", "Only to find the cause of a graphics card crash. It makes the game slower and works from\n"
                                               "the next start.");
        }

        ImGui::TextWrapped("Crash report in this session: %s", d3d12_dred::active() ? "on" : "off");
#endif

    changed |= draw_super_taa_settings(); // [SUPER TAA 16.11.2026]

#if !defined(RE4)
    // [FOVEA RAY TRACING 12.11.2026]
    g_framework->push_menu_toggle_style();
    changed |= m_fovea_ray_tracing->draw("Ray tracing in the sharp area too (experimental, can crash)");
    QV_TIP("With the game's ray tracing on, the sharp area gets it too. It uses more GPU power and can crash the game,\n"
           "mostly when a level loads.");
    g_framework->pop_menu_toggle_style();
#endif
    g_framework->push_menu_toggle_style();
#if !REVR_RELEASE // [RELEASE SWITCH fix139] the older exposure match: fixed at each game's default in the release
    changed |= m_brightness_match->draw("Match the sharp area's brightness to the rest");
    QV_TIP("Makes the sharp area as bright as the image around it.");
    if (m_brightness_match->value()) {
        changed |= m_tone_match->draw("  also dark and bright parts separately");
    }
#endif
    changed |= m_periphery_light->draw("Even out light and fog with the rest of the image");
    QV_TIP("Evens out light and fog differences between the sharp area and the image around it.\n"
           "If something flickers in the sharp area, turn it off or raise the smoothness.");
    g_framework->pop_menu_toggle_style();
    if (m_periphery_light->value()) {
        changed |= m_light_px->draw("  smoothness");
    }
    g_framework->push_menu_toggle_style();
    changed |= m_hole_fill->draw("Fill the gaps beside arms and hands");
    QV_TIP("Next to your arms, hands and weapon small gaps can appear outside the sharp area: they are filled with the background.");
    g_framework->pop_menu_toggle_style();
    // [STABLE DEPTH fix81 17.11.2026]
    g_framework->push_menu_toggle_style();
    changed |= m_stable_depth->draw("Steady outlines outside the sharp area");
    QV_TIP("Stops the outlines of near objects from flickering outside the sharp area.");
    g_framework->pop_menu_toggle_style();
    // [LIGHTS fix85 18.11.2026]
    g_framework->push_menu_toggle_style();
    changed |= m_wide_first->draw("Steady lights when you move your eyes");
    QV_TIP("Stops some lights from flickering outside the sharp area when you move your eyes.");
    g_framework->pop_menu_toggle_style();
    // [MENUS fix81 17.11.2026]
    g_framework->push_menu_toggle_style();
    changed |= m_steady_interface->draw("Steady game menus");
    QV_TIP("Keeps the pause menu, the inventory and the HUD sharp and still.");
    g_framework->pop_menu_toggle_style();
    if (quadview::game::has_item_beams()) {
        g_framework->push_menu_toggle_style();
        changed |= m_item_beams->draw("Green item beams on their item");
        QV_TIP("The green beam over an item sits on the item instead of floating in front of it.");
        g_framework->pop_menu_toggle_style();
    }
    g_framework->push_menu_toggle_style();
    m_early_imgui->draw("Faster frame start for the mods");
    QV_TIP("The mods do their work earlier in each frame, for a smoother game. Leave it on.");
    g_framework->pop_menu_toggle_style();
    g_framework->push_menu_toggle_style();
    m_skip_fovea_ui_update->draw("Skip a double interface update (faster)");
    QV_TIP("Saves CPU time without changing the image. Leave it on.");
    g_framework->pop_menu_toggle_style();
    changed |= m_feather->draw("Sharp area edge softness (0 = hard edge)");
    QV_TIP("How gently the sharp area blends into the rest of the image.");
    changed |= m_deadzone->draw("Eye movement dead zone");
    QV_TIP("With eye tracking: small eye movements below this do not move the sharp area.");

    g_framework->push_menu_toggle_style();
    changed |= m_center_when_zoomed->draw("Sharp area in the middle when aiming with a scope");
    QV_TIP("While you look through a scope the sharp area stays in the middle.");
    changed |= m_desktop_mirror->draw("Show the left eye in the desktop window");
    QV_TIP("Shows the headset image on the PC screen. Off saves a little GPU power.");
    changed |= m_small_window->draw("Small game window (faster)");
    QV_TIP("The game's window on the PC screen gets smaller, so the game draws less. The headset image does not change.");
    changed |= m_ui_motion->draw("No trails on the HUD and text when moving");
    QV_TIP("Stops trails behind the HUD and the menu text when you move.");
    changed |= m_direct_8bit->draw("Faster image output to the headset");
    QV_TIP("Saves a step when the image is sent to the headset. The image does not change.");
#if !REVR_RELEASE // [RELEASE SWITCH fix139]
    menu_text::Checkbox("Show the edge of the sharp area", &m_debug_border);
    QV_TIP("Draws a line around the sharp area, to see where it is.");
#endif
    g_framework->pop_menu_toggle_style();

    if (mode() == Mode::DYNAMIC) {
        ImGui::Separator();
        changed |= m_smoothing->draw("Eye tracking smoothing (lower = calmer)");
        QV_TIP("Lower: the sharp area moves more calmly. Higher: it follows your eyes faster.");

        g_framework->push_menu_toggle_style();
        changed |= m_invert_x->draw("Invert eye tracking left/right");
        changed |= m_invert_y->draw("Invert eye tracking up/down");
        changed |= m_declare_sdk->draw("Eye tracking support for SteamVR");
        QV_TIP("Needed for eye tracking with SteamVR. Leave it on.");
        g_framework->pop_menu_toggle_style();

        changed |= m_offset_x->draw("Eye tracking offset left/right");
        QV_TIP("Moves the sharp area if it is not exactly where you look.");
        changed |= m_offset_y->draw("Eye tracking offset up/down");
        QV_TIP("Moves the sharp area if it is not exactly where you look.");

        ImGui::Text("Eye tracking: %s", gaze.status_text());
    }

    ImGui::TreePop();
    } // Advanced Options

    if (changed) {
        m_options_dirty = true;
        g_framework->request_save_config();
    }

    // ---- status ---------------------------------------------------------------------------------
    ImGui::Separator();

    const auto status = up->get_quadview_status();
    ImGui::TextWrapped("Status: %s", status.c_str());

    if (const auto failure = quadview::State::get().failure(); !failure.empty()) {
        ImGui::TextWrapped("Tri-view could not start with these settings: %s. Change a setting or the mode to retry.", failure.c_str());
    }

#if !REVR_RELEASE // [RELEASE SWITCH fix139] the layout's numbers, the counters, "Diagnostics" and "Debug tools": debug build only
    const auto l = up->get_quadview_layout();

    if (l.valid) {
        const int views = vr->is_mono_periphery() ? 3 : 4;
        ImGui::Text("Eye image %ux%u, fovea %ux%u", l.display_w, l.display_h, l.inset_w, l.inset_h);
        ImGui::Text("Render %ux%u x %d views = %.2f MP per frame", l.render_w, l.render_h, views, (double)views * l.render_w * l.render_h / 1e6);
        const auto report = up->get_layer_report();
        ImGui::TextWrapped("Scene layers: %s", report.c_str());
        ImGui::Text("Periphery x%.2f (light TAA + spatial), fovea x%.2f (%s)", l.wide_ratio, l.fovea_ratio, quality_name(l.fovea_quality));
    }

    // counters per second for the UI (updated twice a second)
    const auto now = Clock::now();

    if (std::chrono::duration<double>(now - m_ui_rate_time).count() >= 0.5) {
        const auto c = read_counters();
        const double secs = (std::max)(std::chrono::duration<double>(now - m_ui_rate_time).count(), 0.001);

        if (m_ui_rate_time.time_since_epoch().count() != 0) {
            m_ui_rate.wide = (uint64_t)((c.wide - m_ui_prev.wide) / secs);
            m_ui_rate.inset = (uint64_t)((c.inset - m_ui_prev.inset) / secs);
            m_ui_rate.by_scope = (uint64_t)((c.by_scope - m_ui_prev.by_scope) / secs);
            m_ui_rate.by_camera = (uint64_t)((c.by_camera - m_ui_prev.by_camera) / secs);
            m_ui_rate.gui = (uint64_t)((c.gui - m_ui_prev.gui) / secs);
            m_ui_rate.evaluations = (uint64_t)((c.evaluations - m_ui_prev.evaluations) / secs);
            m_ui_rate.skipped = (uint64_t)((c.skipped - m_ui_prev.skipped) / secs);
        }

        m_ui_prev = c;
        m_ui_rate_time = now;
    }

    if (ImGui::TreeNode("Diagnostics (per second)")) {
        ImGui::Text("Scene layers: wide %llu, fovea %llu", m_ui_rate.wide, m_ui_rate.inset);
        ImGui::Text("Fovea projections: in layer %llu, by camera %llu, GUI %llu", m_ui_rate.by_scope, m_ui_rate.by_camera, m_ui_rate.gui);
        ImGui::Text("Composited eye images %llu, skipped %llu", m_ui_rate.evaluations, m_ui_rate.skipped);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Debug tools (write to foveation_debug.log)")) {
        const auto path = REFramework::get_persistent_dir("foveation_debug.log").string();
        ImGui::TextWrapped("File: %s", path.c_str());
        ImGui::TextWrapped("While tri-view is on, a status line is written every 5 s. Send me that file.");

        g_framework->push_menu_toggle_style();

        if (m_log_periodic->draw("Periodic status line (every 5 s)")) {
            g_framework->request_save_config();
        }

        // [VIEW RACE 03.11.2026] for comparison: item names that jump sideways
        if (m_head_view_logic->draw("Head's view for game logic (item names, markers)")) {
            g_framework->request_save_config();
        }

        // [MONO PERIPHERY 27.10.2026] for comparison: does a fault come from the reprojection of the shared wide view?
        if (m_mono_periphery->draw("Shared periphery (off = one wide view per eye, 4 scene layers, for comparison)")) {
            m_options_dirty = true;
            g_framework->request_save_config();
        }

        g_framework->pop_menu_toggle_style();

        if (menu_text::Checkbox("Measure GPU time (timestamp queries)", &m_gpu_timing_user) && !any_test_running()) {
            m_gpu_timing_auto_off = !m_gpu_timing_user; // turned off by hand: not switched on again by itself

            if (!gpu_timer_enable(m_gpu_timing_user)) {
                m_gpu_timing_user = false;
                REVR_LOG(set_message("GPU timing could not be started."));
            }
        }

        ImGui::Separator();
        ImGui::TextWrapped("Debug view (not saved): Normal / Periphery only (no fovea) / Mark reprojection holes (pixels the wide view "
                           "could not see behind a near object are tinted magenta) / Tint interface sources (interface from the wide "
                           "view blue, from the fovea view green).");

        static constexpr int VIEW_IDS[] = {0, 1, 4, 5};
        const char* views[] = {"Normal", "Periphery only", "Mark reprojection holes", "Tint interface sources"};

        for (int i = 0; i < 4; ++i) {
            if (i == 1 || i == 3) {
                ImGui::SameLine();
            }

            if (ImGui::RadioButton(views[i], m_debug_view == VIEW_IDS[i])) {
                m_debug_view = VIEW_IDS[i];
            }
        }

        // [GPU 29.10.2026] A/B of the single eye pass against the three passes it replaced (same image; GPU time in the log)
        if (menu_text::Checkbox("Separate eye passes (comparison, not saved)", &m_separate_eye_passes)) {
            REVR_LOG(debug_log(std::format("[DEBUG] separate eye passes: {}", m_separate_eye_passes)));
        }

        // [HOLE BAND 13.11.2026]
        if (menu_text::Checkbox("Fovea covers the periphery's gaps in its blend band (comparison, not saved)", &m_hole_band)) {
            REVR_LOG(debug_log(std::format("[DEBUG] hole band: {}", m_hole_band)));
        }

        // [MENU BACKDROP 15.11.2026]
        if (menu_text::Checkbox("Game menus: scene from the periphery where the fovea shows the menu's backdrop past its frame (not saved)",
                &m_menu_backdrop)) {
            REVR_LOG(debug_log(std::format("[DEBUG] menu backdrop fix: {}", m_menu_backdrop)));
        }

        // [BACKDROP fix83 17.11.2026]
        if (menu_text::Checkbox("Interface in the fovea image: a backdrop only the fovea view draws is taken out (not saved)", &m_fovea_backdrop)) {
            REVR_LOG(debug_log(std::format("[DEBUG] fovea backdrop as the wide view's: {}", m_fovea_backdrop)));
        }

        // [OUTLINES fix84 18.11.2026]
        // [MENUS fix86 18.11.2026]
        if (menu_text::Checkbox("Periphery: menus and writing collected over the frames (not saved)", &m_ui_steady)) {
            REVR_LOG(debug_log(std::format("[DEBUG] periphery interface collected over the frames: {}", m_ui_steady)));
        }

        if (menu_text::Checkbox("Periphery: outlines per pixel, to a fraction of a texel (not saved)", &m_soft_outlines)) {
            REVR_LOG(debug_log(std::format("[DEBUG] periphery outlines per pixel: {}", m_soft_outlines)));
        }

        menu_text::hint_text("Each pixel of the periphery on the surface its own ray meets, the outline of a near thing where the "
                            "depth's mean over time puts it. Off: one surface per 4 x 4 eye pixels (up to fix83). Needs 'steady "
                            "outlines' (the stable depth) and the periphery's image of twice the size.");

        // [PERIPHERY DUMP fix84 18.11.2026]
        if (menu_text::Checkbox("Dumps: the periphery's chain whole, as stored (large files; not saved)", &m_dump_periphery_full)) {
            REVR_LOG(debug_log(std::format("[DEBUG] dumps with the periphery's chain whole: {}", m_dump_periphery_full)));
        }

        menu_text::hint_text("With it a dump also holds, at full size and exactly as stored: the wide view's colour, depth and "
                            "motion vectors, the depth the reprojection got, the anti-aliased wide image, each eye's reprojected "
                            "periphery and the left eye's finished image (with 'periphery only' that is the periphery alone). "
                            "About 150 MB per dump before zipping. For hunting what moves in the periphery: two or three dumps "
                            "in a row while it trembles.");

        // [LIGHTS fix83 17.11.2026]
        if (ImGui::SliderInt("Fovea views: matrix sets with the eye's whole frustum (mask, test, not saved)", &m_fovea_whole_infos, 0, 63)) {
            REVR_LOG(debug_log(std::format("[DEBUG] fovea views' matrix sets with the eye's whole frustum: mask {}", m_fovea_whole_infos)));
        }

        menu_text::hint_text("For the flashing light beside the gaze (RE7, the top of the stairs). 0 = off. Which of the six sets "
                            "of matrices a fovea view is drawn with keep the eye's whole frustum (1 scene, 2 depth distortion, "
                            "4 filter, 8 jitter disable, 16 jitter disable post, 32 z prepass; sums combine them). The test of "
                            "17.11.2026: only 1 takes the flicker away, and with 1 the fovea's picture is wrong (the whole eye "
                            "squeezed into it).");

        // [VIEW SCALE 15.11.2026]
        if (menu_text::Checkbox("Looked-at objects at their real distance: no doubled edge at the fovea (not saved)", &m_real_view_scale)) {
            REVR_LOG(debug_log(std::format("[DEBUG] real view scale: {}", m_real_view_scale)));
        }

        // [SCREEN EFFECTS 15.11.2026]
        if (menu_text::Checkbox("Lens and video tape effects off in the tri-view (not saved)", &m_screen_effects_off)) {
            REVR_LOG(debug_log(std::format("[DEBUG] screen effects off: {}", m_screen_effects_off)));
        }

        // [IMAGE QUALITY 15.11.2026]
        if (menu_text::Checkbox("Video tapes: the wide view at the fovea views' size (not saved; off: the game's value from its next change)",
                &m_image_quality_hold)) {
            REVR_LOG(debug_log(std::format("[DEBUG] image quality hold: {}", m_image_quality_hold)));
        }

        // [FLICKER TEST 15.11.2026]
        {
            static const char* flicker_names[] = {"none", "fovea views almost empty", "fovea views without dynamic shadows",
                "no dynamic shadows in any view", "head's view for every thread", "engine level of detail off"};

            ImGui::Combo("Flicker test (one engine switch, not saved)", &m_flicker_test, flicker_names, 6);
        }

        // [SHADOW CACHE 15.11.2026]
        {
            static const char* cache_names[] = {"as the game", "off in the fovea views", "off everywhere"};

            if (ImGui::Combo("Shadow cache in the tri-view (lights that flicker, not saved)", &m_shadow_cache_mode, cache_names, 3)) {
                REVR_LOG(debug_log(std::format("[DEBUG] shadow cache: {}", cache_names[std::clamp(m_shadow_cache_mode, 0, 2)])));
            }
        }

        // [LIGHT FLICKER TEST 15.11.2026]
        if (!m_lft_running) {
            if (!any_test_running() && ImGui::Button("Light flicker test (about 25 s)")) {
                m_lft_request.store(true);
            }

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("Stand still with a light that flickers (the top of the stairs) beside your gaze, outside the\n"
                                  "sharp area. After 5 s, 4 phases of 4.5 s: as the game; the eye's whole frustum in the fovea\n"
                                  "views' scene matrices (the sharp area looks squeezed: the known cure, for comparison); the wide\n"
                                  "view drawn before the fovea views; as the game again. The flicker outside the sharp area is\n"
                                  "measured; the result goes into foveation_debug.log. Note whether the picture looks wrong in\n"
                                  "the third phase. Nothing is kept.");
            }
        } else {
            ImGui::TextUnformatted("Light flicker test running: keep the light beside your gaze and keep still.");
        }

        // [OCCLUSION 15.11.2026]
        {
            static const char* occl_names[] = {"as the game", "off in the wide view", "off in every view"};

            if (ImGui::Combo("Engine occlusion culling (flicker through openings, not saved)", &m_wide_occlusion, occl_names, 3)) {
                REVR_LOG(debug_log(std::format("[DEBUG] occlusion culling: {}", occl_names[std::clamp(m_wide_occlusion, 0, 2)])));
            }
        }

        // [CPU TEST 03.11.2026] the CPU test in a fight (phases of 15 s, the periodic lines labelled with them)
        if (!m_ctest_running) {
            if (!any_test_running() && ImGui::Button("Run CPU test in a fight (75 s)")) {
                m_ctest_request.store(1);
            }

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("Start it when many enemies are in view and keep fighting or looking at them. After 5 s, 7 phases of 10 s:\n"
                                  "normal and fovea views almost empty in turn (the fovea shows a tiny part of the scene for 10 s).\n"
                                  "Then send foveation_debug.log and foveation_timeline.csv.");
            }

            // [TIMELINE 03.11.2026]
            if (ImGui::Button("Write frame timeline (40 frames, to foveation_timeline.csv)")) {
                m_tl_request.store(true);
            }

            // [GPU TEST 06.11.2026]
            if (!any_test_running() && ImGui::Button("Run GPU test in a fight (2.5 min)")) {
                m_ctest_request.store(4);
            }

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("Start it when many enemies are in view and keep fighting or looking at them. After 5 s, 13 phases of 10 s:\n"
                                  "normal in between; a test load (work like the eye passes, on images of its own) in the two places where\n"
                                  "the GPU waits inside the fovea views, small and large; the large one where the GPU does not wait.\n"
                                  "The headset image stays the same.\n"
                                  "The GPU timeline of the frame runs through it. Then send foveation_debug.log.");
            }
        } else if (ImGui::Button(m_ctest_gpu ? "Cancel GPU test" : "Cancel CPU test")) {
            m_ctest_request.store(2);
        }

        // [SUBMIT TIMELINE 06.11.2026] every submit of the game's queues timed on the GPU (a measurement: the image stays the same)
        {
            bool st_on = d3d12::SubmitTimeline::get().running();

            if (menu_text::Checkbox("GPU timeline of the frame (measurement, not saved)", &st_on)) {
                m_st_request.store(st_on ? 1 : 2);
            }

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("Times every ExecuteCommandLists of the game's queues on the GPU (the engine's, the mod's, SteamVR's).\n"
                                  "Each status line then ends with how long the game's queue worked and stood idle per frame and why,\n"
                                  "and which submits take the time. The game stops ~0.3 s when it is switched on.");
            }
        }

        // [CPU PROBE 03.11.2026] what the two fovea views cost the CPU for the objects in them (switch on and off for 15 s
        // each in the same scene; the periodic log lines show the difference)
        if (!m_ctest_running) {
            bool probe = quadview::State::get().cpu_probe_tiny_fovea.load();

            if (menu_text::Checkbox("CPU test: fovea views almost empty (not saved)", &probe)) {
                quadview::State::get().cpu_probe_tiny_fovea.store(probe);
                REVR_LOG(debug_log(std::format("[CPU PROBE] fovea views almost empty: {}", probe)));
            }

            if (ImGui::IsItemHovered()) {
                menu_text::hint_tooltip("The two fovea views render only 2%% of their area (the fovea shows a tiny, magnified part of the scene).\n"
                                  "Switch it on and off for about 15 s each in the same scene (best: a fight with many enemies);\n"
                                  "the log lines every 5 s show what the main thread spends with and without the objects in the fovea views.");
            }
        }

        // [GPU TEST 29.10.2026] one variant of the GPU pass test by hand (the last two change the image)
        if (!m_ptest_running) {
            const char* names[PASS_VARIANT_COUNT]{};

            for (int i = 0; i < PASS_VARIANT_COUNT; ++i) {
                names[i] = PASS_VARIANTS[i].name;
            }

            if (menu_text::Combo("GPU comparison (not saved)", &m_test_variant, names, PASS_VARIANT_COUNT)) {
                REVR_LOG(debug_log(std::format("[DEBUG] GPU comparison: {}", PASS_VARIANTS[m_test_variant].name)));
            }
        }

        // [CAMERA CHECK 10.11.2026] test: the RenderOutputID of the clones (3 since the first multipass; the game camera's
        // view uses another one); applied to the existing clones at the next scene lock, not saved
        {
            static int clone_output_id = 3;
            ImGui::SetNextItemWidth(160.0f);
            menu_text::InputInt("Clone RenderOutputID (test, not saved)", &clone_output_id);
            clone_output_id = std::clamp(clone_output_id, 0, 255);
            ImGui::SameLine();

            if (ImGui::Button("Apply to the clones")) {
                auto& dup = VR::get()->get_camera_duplicator();
                auto ids = dup.get_clone_output_ids();
                ids.fill(clone_output_id);
                dup.set_clone_output_ids(ids);
                REVR_LOG(debug_log(std::format("[DEBUG] clone RenderOutputID -> {}", clone_output_id)));
                REVR_LOG(set_message(std::format("Clone RenderOutputID {} (applied at the next frame)", clone_output_id)));
            }
        }

        if (ImGui::Button("Dump 2 frames = both eyes (BMP)")) {
            quadview::State::get().request_dump(2);
            m_dump_snapshot_written = false;
            REVR_LOG(set_message("Dump requested..."));
        }

        // [ALWAYS ON fix91 19.11.2026] the button "Dump plain multipass eyes (quad view off, ...)" was here: it asked for the
        // quad view to be switched off first, which the menu no longer does

        // [DUMP DELAY 24.10.2026] free hands for the movement to capture; every dumped frame waits for the GPU and
        // writes its images, so the game stutters for a few seconds while the sequence is taken
        ImGui::SameLine();

        if (m_dump_at.time_since_epoch().count() == 0) {
            if (ImGui::Button("Dump in 4 s: 20 frames in a row")) {
                m_dump_at = Clock::now() + std::chrono::seconds(DUMP_DELAY_SECONDS);
                REVR_LOG(set_message("Dump in 4 seconds: start moving now."));
            }
        } else {
            const double left = std::chrono::duration<double>(m_dump_at - Clock::now()).count();
            ImGui::Text("Dump in %.1f s", left > 0.0 ? left : 0.0);
        }

        // [VIDEO 25.10.2026] the real game at its frame rate: 4 s of both eye images as they go to the headset, as an
        // MP4 in the quadview_dump folder (the dumps above make the game stutter, so fast movements look different)
        if (m_video_at.time_since_epoch().count() == 0) {
            if (ImGui::Button("Video in 4 s: 4 seconds of both eyes (MP4)")) {
                m_video_at = Clock::now() + std::chrono::seconds(VIDEO_DELAY_SECONDS);
                quadview::State::get().set_video_status("video in 4 seconds: get ready to move");
            }
        } else {
            const double left = std::chrono::duration<double>(m_video_at - Clock::now()).count();
            ImGui::Text("Video starts in %.1f s", left > 0.0 ? left : 0.0);
        }

        if (const auto video = quadview::State::get().video_status(); !video.empty()) {
            ImGui::TextWrapped("%s", video.c_str());
        }

        ImGui::Separator();

        if (ImGui::Button("Write status snapshot")) {
            REVR_LOG(debug_log(build_snapshot("button")));
            REVR_LOG(set_message("Snapshot written."));
        }

        if (!m_bench_running) {
            if (!any_test_running() && ImGui::Button("Run A/B benchmark (about 90 s)")) {
                bench_start();
            }
        } else if (ImGui::Button("Cancel benchmark")) {
            bench_cancel("cancelled by user");
        }

        // [ENGINE VIEWS 30.10.2026] what the engine renders in each view, and how its GPU time follows the views
        if (ImGui::Button("Write render layer tree")) {
            m_layer_dump_request.store(true);
        }

        if (!m_vtest_running) {
            if (!any_test_running() && ImGui::Button("Run engine view cost test (about 2 min, stand still, the image changes)")) {
                m_vtest_request.store(1);
            }
        } else if (ImGui::Button("Cancel engine view cost test")) {
            m_vtest_request.store(2);
        }

        // [ENGINE SHADOWS 30.10.2026] the shadow layers' settings, and what the engine's dynamic shadows cost in all views
        if (ImGui::Button("Write shadow layer details")) {
            m_shadow_dump_request.store(true);
        }

        if (!m_stest_running) {
            if (!any_test_running() && ImGui::Button("Run engine shadow test (about 1.5 min, stand still, shadows go off and on)")) {
                m_stest_request.store(1);
            }
        } else if (ImGui::Button("Cancel engine shadow test")) {
            m_stest_request.store(2);
        }

        // [AMBIENT OCCLUSION 30.10.2026] the game's render settings and the ambient occlusion types
        if (ImGui::Button("Write ambient occlusion details")) {
            m_ao_dump_request.store(true);
        }

        // [VRS 01.11.2026] variable rate shading: the GPU, the engine's settings per camera / layer
        if (ImGui::Button("Write variable rate shading details")) {
            m_vrs_dump_request.store(true);
        }

        // [EXPOSURE 01.11.2026] the engine's automatic exposure and tone mapping: settings, each view camera's components
        if (ImGui::Button("Write exposure and tone mapping details")) {
            m_exposure_dump_request.store(true);
        }

        // [LOD 06.11.2026] level of detail and culling: the engine's settings, each view's camera and layers, the types
        if (ImGui::Button("Write level of detail and culling details")) {
            m_lod_dump_request.store(true);
        }

        // [LIGHT SHAFT 13.11.2026]
        if (ImGui::Button("Write light shaft / god ray / scattering details")) {
            m_shaft_dump_request.store(true);
        }

        // [EXPOSURE TEST 01.11.2026] the views' tone mapping changed for a test (not saved): does the fovea's brightness
        // then match the wide view's? Best with the exposure match and the large-scale light off, to see the views as they are.
        if (!m_auto_values_read) {
            m_auto_values_read = true;

            if (const auto t = sdk::find_type_definition("via.render.ToneMapping"); t != nullptr) {
                if (const auto m = t->get_method("getAutoExposure"); m != nullptr && m->get_return_type() != nullptr && m->get_return_type()->is_enum()) {
                    for (auto* f : m->get_return_type()->get_fields()) {
                        if (f == nullptr || f->get_name() == nullptr || !f->is_static() || !f->is_literal() || std::string_view{f->get_name()} == "value__") {
                            continue;
                        }

                        if (const auto* d = (const int32_t*)f->get_init_data(); d != nullptr) {
                            m_auto_values.emplace_back(f->get_name(), *d);
                        }
                    }
                }
            }
        }

        static const char* local_items[] = {"as the game", "off in the fovea views", "off in all views"};
        bool tone_changed = menu_text::Combo("Local exposure (test, not saved)", &m_tt_local, local_items, 3);
        tone_changed |= menu_text::Checkbox("Fovea views: the wide view's automatic exposure (test, not saved)", &m_tt_copy_auto);

        if (!m_auto_values.empty()) {
            std::vector<std::string> names{"as the game"};

            for (const auto& [n, v] : m_auto_values) {
                names.push_back(std::format("{} ({}) in all views", n, v));
            }

            std::vector<const char*> items{};

            for (const auto& n : names) {
                items.push_back(n.c_str());
            }

            tone_changed |= menu_text::Combo("Automatic exposure (test, not saved)", &m_tt_auto, items.data(), (int)items.size());
        }

        // [EV LOCK 13.11.2026] RE7 (no automatic exposure setter): its range closed on the game camera's exposure
        static const char* lock_items[] = {"off", "in the fovea views", "in all views (the game camera's too)"};
        tone_changed |= menu_text::Combo("Exposure range closed on the game camera's (test, not saved)", &m_tt_ev_lock, lock_items, 3);

        // [EV FOLLOW 13.11.2026] the fovea views' EV: the game camera's + an offset (fixed and / or following the measurement)
        tone_changed |= menu_text::SliderFloat("Fovea views: EV offset (test, not saved)", &m_tt_ev_offset, -4.0f, 4.0f, "%.2f");
        tone_changed |= menu_text::Checkbox("Fovea views: EV follows the periphery's brightness (test, not saved)", &m_tt_ev_follow);
        // [ADAPT 13.11.2026]
        static const char* adapt_items[] = {"as the game", "rates 0", "rates 1000"};
        tone_changed |= menu_text::Combo("Fovea views: their own exposure adaptation (test, not saved)", &m_tt_clone_adapt, adapt_items, 3);
        // [REAL EV 13.11.2026] the game camera's adapted exposure (calcRealEV + SnappedRealEV) as the fovea views' EV
        tone_changed |= menu_text::Combo("Fovea views: the game camera's real exposure (default rates 0, not saved)", &m_tt_real_ev, real_ev_names(), 4);
        // [REAL EV fix105]
        tone_changed |= menu_text::Checkbox("Exposure: measure the game camera every 8th frame while its exposure is steady (off: every 4th "
                                            "always, as before fix105; default on, not saved)", &m_tt_real_ev_steady);

        // [VIEW EFFECTS 13.11.2026] the effect components of the three views side by side (once by itself with the real exposure)
        if (ImGui::Button("Write the views' fog / light shaft / bloom values")) {
            m_effects_dump_request.store(true);
        }

        // [LIGHT SHAFT OFF 13.11.2026]
        if (menu_text::Checkbox("Light shaft entities of the scene off in all views (test, not saved)", &m_ls_off)) {
            m_ls_changed.store(true);
        }

        // [GLOW 13.11.2026] the glow around the sun only exists in a view that has the sun in it: taken from the periphery by the
        // large-scale light, whose limit (35 % of the level) is far below that glow
        menu_text::SliderFloat("Large-scale light: limit (part of the level, 0 = 0.35; test, not saved)", &m_tt_light_limit, 0.0f, 4.0f, "%.2f");
        menu_text::Checkbox("Large-scale light: only adds light (test, not saved)", &m_tt_light_add_only);
        // [AUTO LIGHT 14.11.2026]
        menu_text::Checkbox("Large-scale light: automatic (blocks that disagree with the light around them and the periphery's "
                            "gaps left out; limit 0 = 4; default on, not saved)", &m_tt_light_auto);

        if (m_tt_real_ev != 0 && !m_effects_dump_done && VR::get()->get_camera_duplicator().get_active_clone_count() >= 2) {
            m_effects_dump_done = true;
            m_effects_dump_request.store(true);
        }

        if (tone_changed) {
            vrmod::CameraDuplicator::ToneTest tt{};
            tt.local_exposure = m_tt_local;
            tt.copy_auto = m_tt_copy_auto;
            tt.ev_lock = m_tt_ev_lock;
            tt.ev_offset = m_tt_ev_offset;
            tt.ev_follow = m_tt_ev_follow;
            tt.clone_adapt = m_tt_clone_adapt;
            tt.real_ev = m_tt_real_ev;
            tt.real_ev_steady = m_tt_real_ev_steady;

            if (!m_exposure_dump_done) { // [EXPOSURE MEMBERS 13.11.2026]
                m_exposure_dump_done = true;
                m_exposure_dump_request.store(true);
            }
            tt.auto_value = m_tt_auto > 0 && m_tt_auto <= (int)m_auto_values.size() ? m_auto_values[m_tt_auto - 1].second : -1;
            VR::get()->get_camera_duplicator().set_tone_test(tt);
            REVR_LOG(debug_log(std::format("[EXPOSURE TEST] {}", tone_test_text())));
        }

        menu_text::hint_text("To compare the views as they are: exposure match and large-scale light off, blend width 0.");

        // [VIEW DIFFERENCE 01.11.2026] the fovea / periphery difference measured on the GPU, in the 5-second log line
        menu_text::Checkbox("Log the fovea / periphery difference (5 s line, not saved)", &m_log_view_diff);

        // [COLOUR CORRECTION 02.11.2026] the effect test of 02.11: with the colour correction off the difference halves
        if (ImGui::Button("Write colour correction details")) {
            m_colour_dump_request.store(true);
        }

        if (menu_text::Checkbox("Fovea views: the wide view's colour correction values (test, not saved)", &m_copy_colour_correct)) {
            VR::get()->get_camera_duplicator().set_copy_color_correct(m_copy_colour_correct);
            REVR_LOG(debug_log(std::format("[COLOUR TEST] fovea views with the wide view's colour correction values: {}", m_copy_colour_correct)));
        }

        // [ENGINE FEATURES 30.10.2026] what each of the engine's effects costs in all views (each switched off for a moment)
        if (!m_ftest_running) {
            if (!any_test_running() && ImGui::Button("Run engine effect cost test (about 4 min, stand still, effects go off one at a time)")) {
                m_ftest_request.store(1);
            }
        } else if (ImGui::Button("Cancel engine effect cost test")) {
            m_ftest_request.store(2);
        }

        // [RENDER SETTINGS 30.10.2026] what the ambient occlusion costs, and a lighter one in the wide view only
        if (!m_rtest_running) {
            if (!any_test_running() && ImGui::Button("Run ambient occlusion cost test (about 3 min, stand still, the image changes)")) {
                m_rtest_request.store(1);
            }
        } else if (ImGui::Button("Cancel ambient occlusion cost test")) {
            m_rtest_request.store(2);
        }

        // [GPU TEST 29.10.2026] every pass variant for 15 s while the player stands still; GPU time per pass into the log
        if (!m_ptest_running) {
            if (!any_test_running() && ImGui::Button("Run GPU pass test (about 2 min, stand still, HUD in the picture)")) {
                m_ptest_request.store(1);
            }
        } else if (ImGui::Button("Cancel GPU pass test")) {
            m_ptest_request.store(2);
        }

        if (const auto message = get_message(); !message.empty()) {
            ImGui::TextWrapped("%s", message.c_str());
        }

        if (!m_bench_summary.empty()) {
            ImGui::TextWrapped("Last benchmark: %s", m_bench_summary.c_str());
        }

        if (const auto summary = get_ptest_summary(); !summary.empty()) {
            ImGui::TextWrapped("Last GPU pass test (ms): %s", summary.c_str());
        }

        ImGui::TreePop();
    }
#endif
}
