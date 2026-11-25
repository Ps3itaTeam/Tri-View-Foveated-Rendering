#!/bin/bash
# The GPU test of the supersampling TAA over a set of cases; one line per case: the shaders' result against the numpy
# reference (each frame from the GPU's own state of the frame before).
#   needs: gpu/supertaa_test.exe (gpu/build.sh), the sequences (python3 scene.py data; python3 scene2.py data), and a
#   D3D12 to run the exe:
#   Windows itself, or wine with vkd3d and a Vulkan driver (the software driver lavapipe is enough) and Microsoft's
#   d3dcompiler_47.dll beside the exe (wine's own compiler is not FXC).
#   RUN="xvfb-run -a wine" ./run_cases.sh        (Linux)        RUN="" ./run_cases.sh   (Windows, a POSIX shell)
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}
RUN=${RUN-xvfb-run -a wine}
export WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3dcompiler_47=n;mscoree,mshtml=}"
mkdir -p "$WORK"
cd "$HERE"
run() {
  name=$1; shift
  python3 make_case.py data/tremor_p32.npz "$WORK/case_$name" 16 24 96 72 12 "$@" > /dev/null || { echo "$name: make_case failed"; return; }
  (cd "$HERE/gpu" && timeout 900 $RUN ./supertaa_test.exe "$WORK/case_$name" > "$WORK/case_$name/run.log" 2>&1)
  if ! grep -q '^done:' "$WORK/case_$name/run.log"; then echo "$name: GPU RUN FAILED"; tail -5 "$WORK/case_$name/run.log"; return; fi
  echo "$name: $(python3 onestep.py "$WORK/case_$name" none | tail -1)"
}
# the same on the ghost scene (scene2.py), around the frames where its text scrolls: there the test of what changed has
# work to do ("pixels following a change" in the line), and the protection is released around it
run_g() {
  name=$1; shift
  python3 make_case.py data/g_pan_p32.npz "$WORK/case_$name" 96 0 96 72 14 first=34 "$@" > /dev/null || { echo "$name: make_case failed"; return; }
  (cd "$HERE/gpu" && timeout 900 $RUN ./supertaa_test.exe "$WORK/case_$name" > "$WORK/case_$name/run.log" 2>&1)
  if ! grep -q '^done:' "$WORK/case_$name/run.log"; then echo "$name: GPU RUN FAILED"; tail -5 "$WORK/case_$name/run.log"; return; fi
  echo "$name: $(python3 onestep.py "$WORK/case_$name" none | tail -1)"
}
run default
run scale1 scale=1
run lanczos box_resolve=0
run cheap nearest_sample=1 dering=2
run nodering dering=0
run nofollow follow=0
run nosharp sharpen=0
run out10 out_10bit=1
run ratio125 out_ratio=1.25
run ratio125s1 out_ratio=1.25 scale=1
run noluma luma_weight=0
run follow2 follow_threshold=0.5 follow_memory=1.5 fl_count=4 fl_alln=7
run noprotect protect=0
run protect2 protect_range=2.5 protect_spark=1.5 protect_memory=2.5
run_g ghost
run_g ghost_s1 scale=1
run_g ghost_follow2 follow_threshold=0.5 follow_memory=1.5 fl_count=4 fl_alln=7 fl_cap=0.02
run_g ghost_noprotect protect=0
# flies (make_case.py: small near things that move several pixels a frame, the motion vectors say nothing of it): the test
# for things that arrive has work to do ("where a thing arrived" in the line)
run flies flies=4
run flies_s1 flies=4 scale=1
run flies_off flies=4 arrive=0
run_g ghost_flies flies=4
# [fix80] the periphery's instance (one view's histories): its image at twice the view's size (the history itself: scale
# 1 of an image twice as large, no sharpening), alone, with flies and on the ghost scene; and at the view's size (the
# history reduced with Lanczos 2)
run wide2x slots=1 out_ratio=2 scale=1 sharpen=0
run wide2x_flies slots=1 out_ratio=2 scale=1 sharpen=0 flies=4
run_g ghost_wide2x slots=1 out_ratio=2 scale=1 sharpen=0
run wide1x slots=1 box_resolve=0
# ... and the reprojection that brings that image to each eye with 2 x 2 pixels per ray (d3d12::MonoPeriphery, ss = 2)
# and the eye pass's shader enlarging that periphery (its text taken out of FoveaComposite.cpp; the test reads it from
# its own folder, where the exe finds it by name under wine and under Windows alike)
mkdir -p "$WORK/periphery"
python3 extract_shader.py "$HERE/../../src/mods/vr/d3d12/FoveaComposite.cpp" EYE_COMPOSE_SHADER "$HERE/gpu/eye_compose.hlsl" > /dev/null
python3 extract_shader.py "$HERE/../../src/mods/vr/d3d12/FoveaComposite.cpp" MONO_PERIPHERY_SHADER | sed 's/^/periphery: /'
(cd "$HERE/gpu" && timeout 1800 $RUN ./periphery_test.exe "$WORK/periphery" eye_compose.hlsl > "$WORK/periphery/run.log" 2>&1)
rm -f "$HERE/gpu/eye_compose.hlsl"
if grep -q '^done:' "$WORK/periphery/run.log"; then python3 check_periphery.py "$WORK/periphery" | sed 's/^/periphery: /'; else echo "periphery: GPU RUN FAILED"; tail -5 "$WORK/periphery/run.log"; fi
# the pass's other paths (debug views, split, the history's scale changed between lists, no depth, reset) and
# d3d12::TextureDump's parts
python3 make_case.py data/tremor_p32.npz "$WORK/case_paths" 16 24 96 72 12 paths=1 > /dev/null
(cd "$HERE/gpu" && timeout 900 $RUN ./supertaa_test.exe "$WORK/case_paths" > "$WORK/case_paths/run.log" 2>&1)
grep -E '^paths: ' "$WORK/case_paths/run.log" | tail -1
# a capture as the game writes it (the game's formats: 10 bit colour, 16 bit signed motion in the engine's units, a
# depth-stencil texture; the mod's TAA beside), replayed and measured by analyze_capture.py
# (64 frames: both anti-aliasings have settled when the last 16 are captured, as in the game)
python3 make_case.py data/tremor_p32.npz "$WORK/case_game" 8 8 160 120 64 out_10bit=1 game=1 capture=16 capture_part=96 > /dev/null
rm -rf "$WORK/case_game/capture"
(cd "$HERE/gpu" && timeout 900 $RUN ./supertaa_test.exe "$WORK/case_game" > "$WORK/case_game/run.log" 2>&1)
python3 analyze_capture.py "$WORK/case_game/capture" | grep -E '^  -> |picture moved by|supersampling TAA against'
# the same with flies in the frames: the capture holds things that arrive, and the replay has to follow them
python3 make_case.py data/tremor_p32.npz "$WORK/case_game_flies" 8 8 160 120 64 out_10bit=1 game=1 capture=16 capture_part=96 flies=4 > /dev/null
rm -rf "$WORK/case_game_flies/capture"
(cd "$HERE/gpu" && timeout 900 $RUN ./supertaa_test.exe "$WORK/case_game_flies" > "$WORK/case_game_flies/run.log" 2>&1)
python3 analyze_capture.py "$WORK/case_game_flies/capture" | grep -E '^  -> fovea|things that arrived'
