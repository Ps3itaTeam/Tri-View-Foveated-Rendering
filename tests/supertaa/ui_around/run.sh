#!/bin/bash
# [fix94] d3d12::UiSteady on the blocks around the interface only (Params::only_around) against the whole image, on the
# pass itself (gpu/periphery_test.exe uisteady; build: gpu/build.sh; a D3D12 to run it: see ../README.md).
#   RUN="xvfb-run -a wine" ./run.sh [work dir]
# Results of 19.11.2026 (software GPU): with the option off the pass's images and map are the same bytes as before the
# change (speed 0 and 0.6). With it on: head still, the interface there from the start: the same bits in the marked
# blocks; a piece that appears is there at once (the whole image: fades in, the difference halves every 5 frames); the
# head moving 3 / 9 pixels a frame: at most 0.002 / 0.007 of the range apart with the blocks kept for 6 frames as here
# (0.002 / 0.002 with the pass's own 120); blocks left out are empty two dispatches later and a piece coming back into
# them starts from its own frame.
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$HERE/work}
RUN=${RUN-xvfb-run -a wine}
export WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3dcompiler_47=n;mscoree,mshtml=}"
one() { # name speed frames keys...
  name=$1; speed=$2; frames=$3; shift 3
  d=$WORK/$name; rm -rf "$d"; mkdir -p "$d"
  python3 "$HERE/make.py" "$d" "$speed" "$frames" "$@" || return
  wd=$d; case "$RUN" in *wine*) wd="Z:${d//\//\\}";; esac
  (cd "$HERE/../gpu" && timeout 1800 $RUN ./periphery_test.exe uisteady "$wd" 2>&1 | grep -a "done:\|FAILED\|not read" || echo "$name: NO RESULT")
  rm -f "$d"/c[0-9]*.bin "$d"/h[0-9]*.bin "$d"/f[0-9]*.bin "$d"/m[0-9]*.bin
}
for s in 0 0.6 3 9; do
  one whole_$s $s 64
  one around_$s $s 64 around=1 around_hold=6
  echo "== speed $s: the whole image (A) against only around the interface (B)"
  python3 "$HERE/compare.py" "$WORK/whole_$s" "$WORK/around_$s" marked
done
one switch 0.6 40 around=1 around_from=10 around_to=30
one reset_whole 0.6 24 reset_at=12
one reset_around 0.6 24 around=1 reset_at=12
echo "== a reset at frame 12"; python3 "$HERE/compare.py" "$WORK/reset_whole" "$WORK/reset_around" marked
