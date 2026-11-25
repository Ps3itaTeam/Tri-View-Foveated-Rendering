#!/bin/bash
# [BUILT-IN SHADERS fix145] makes src/mods/vr/d3d12/ShaderTable.inl: builds shader_table_gen.exe with the MinGW cross
# compiler (apt install g++-mingw-w64-x86-64) and runs it. Linux: wine + vkd3d + a Vulkan driver (lavapipe will do), with
# Microsoft's d3dcompiler_47.dll (wine's own is not FXC) given as the first argument; RUN="xvfb-run -a wine" by default.
#   build.sh <d3dcompiler_47.dll>
# Run it again after any change of a shader's text (otherwise that shader is compiled at its first use in the game).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$HERE/../..
SRC=$TREE/src/mods/vr/d3d12
OUT=$HERE/out
mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++20 -O1 -w -DREVR_SHADER_GENERATOR -I "$TREE/tests/supertaa/gpu/stub" -I "$SRC" -I "$TREE/shared" \
  -o "$OUT/shader_table_gen.exe" "$HERE/shader_table_gen.cpp" "$SRC/FoveaComposite.cpp" "$SRC/SuperTAA.cpp" "$SRC/SharpenPass.cpp" \
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -static -static-libgcc -static-libstdc++
cp "$1" "$OUT/d3dcompiler_47.dll"
cd "$OUT"
${RUN:-xvfb-run -a wine} ./shader_table_gen.exe ShaderTable.inl
cp ShaderTable.inl "$SRC/ShaderTable.inl"
rm -f "$OUT/d3dcompiler_47.dll"
echo "written $SRC/ShaderTable.inl"
