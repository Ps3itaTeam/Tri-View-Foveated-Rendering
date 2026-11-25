#!/bin/bash
# Syntax and type check of one source file of the mod with the MinGW cross compiler (no MSVC needed):
#   check.sh <deps dir> <work dir> <file relative to the source tree, e.g. src/mods/TemporalUpscaler.cpp> [extra flags]
# after sync.sh <deps dir> <work dir>. What it reports and is NOT an error of the mod (the environment differs from
# MSVC's): kananlib's utility/Scan.hpp (structured exceptions, a newer bddisasm), REContext.hpp:97 (and with it
# RETypeDB.hpp:1375/1381 "InvokeRet is incomplete"), RETypeDB.hpp:1570,
# D3D12Hook.hpp:152, D3D11Component.hpp:99, HookManager.hpp:166, VR.hpp:1357, rpcndr.h, RENativeArray.hpp:61/239,
# d3dx12.h (GetDesc), "call to non-'constexpr' function 'D3D12_RESOURCE_STATES operator|'" (TemporalUpscaler.cpp, two
# lines of 06.11) and bits/unique_ptr.h "conversion from 'void (*)(sdk::renderer::RenderResource*)' to 'Address'" (three
# lines, TemporalUpscaler.cpp). Everything else in the file it is given is real.
HERE=$(cd "$(dirname "$0")" && pwd)
D=$(cd "$1" && pwd); W=$(cd "$2" && pwd); T=$W/tree; f=$3; shift 3
x86_64-w64-mingw32-g++ -std=c++23 -fsyntax-only -fms-extensions ${WARN:--w} -fmax-errors=${MAXERR:-300} \
  -DRE4 -DREENGINE_PACKED -DREENGINE_AT -DREFRAMEWORK_EXPORTS '-DREFRAMEWORK_GAME_NAME="RE4"' -DDEVELOPER -DSPDLOG_COMPILED_LIB \
  -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 "-DIMGUI_USER_CONFIG=\"$T/src/re2-imgui/re2_imconfig.hpp\"" \
  -include "$HERE/shim.h" -I "$W/ci" -I "$D/DirectX-Headers/include/directx" -I "$D/DirectX-Headers/include" -I "$D/DirectXMath/Inc" \
  -I "$T/shared" -I "$T/src" -I "$T/include" -I "$D/OpenXR-SDK/src" -I "$D/OpenXR-SDK/include" -I "$D/spdlog/include" -I "$D/glm" -I "$D/imgui" \
  -I "$T/src/re2-imgui" -I "$T/dependencies/imguizmo" -I "$T/dependencies/imnodes" -I "$T/dependencies/nlohmann" \
  -I "$T/dependencies/sol2/single/include" -I "$T/dependencies/lua/src" -I "$T/dependencies/openvr/headers" \
  -I "$D/kananlib/include" -I "$D/bddisasm/inc" \
  -I "$D/safetyhook/include" -I "$D/asmjit" -I "$D/minhook/include" -I "$D/DirectXTK12/Inc" -I "$D/DirectXTK12/Src" \
  "$@" "$T/$f"
