#!/bin/bash
# Builds the GPU test with the MinGW cross compiler (Linux: apt install g++-mingw-w64-x86-64). On Windows the same
# sources compile with MSVC (cl /std:c++20 /EHsc /I stub /I ..\..\..\src\mods\vr\d3d12 ... d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../../src/mods/vr/d3d12
x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-variable -I "$HERE/stub" -I "$SRC" -o "$HERE/supertaa_test.exe" \
  "$HERE/supertaa_test.cpp" "$SRC/SuperTAA.cpp" "$SRC/SuperTAACapture.cpp" "$SRC/TextureDump.cpp" "$SRC/FoveaComposite.cpp" \
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -static -static-libgcc -static-libstdc++
echo "built $HERE/supertaa_test.exe"
# [fix80] the periphery's reprojection with 2 x 2 pixels per ray (d3d12::MonoPeriphery)
x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-variable -I "$HERE/stub" -I "$SRC" -o "$HERE/periphery_test.exe" \
  "$HERE/periphery_test.cpp" "$SRC/FoveaComposite.cpp" \
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -static -static-libgcc -static-libstdc++
echo "built $HERE/periphery_test.exe"
