#!/bin/bash
# A copy of the source tree for the syntax check with the MinGW cross compiler (the tree itself stays as it is):
#   sync.sh <deps dir> <work dir>
# <deps dir> holds shallow clones of: gabime/spdlog, g-truc/glm, ocornut/imgui, cursey/kananlib, bitdefender/bddisasm,
# cursey/safetyhook, asmjit/asmjit, TsudaKageyu/minhook, microsoft/DirectXTK12, microsoft/DirectXMath,
# microsoft/DirectX-Headers, KhronosGroup/OpenXR-SDK (git clone --depth 1 https://github.com/<name>)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../.." && pwd)
D=$(cd "$1" && pwd)
W=$2
DST=$W/tree
mkdir -p "$DST/build64_all/_deps" "$W/ci"
for d in src shared include; do rm -rf "$DST/$d"; cp -r "$SRC/$d" "$DST/$d"; done
[ -d "$DST/dependencies" ] || cp -r "$SRC/dependencies" "$DST/dependencies"
ln -sfn "$D/DirectXTK12" "$DST/build64_all/_deps/directxtk12-src"
ln -sfn "$D/DirectXTK12" "$D/directxtk12-src"
for n in glm imgui spdlog minhook; do [ -e "$DST/dependencies/$n" ] || ln -sfn "$D/$n" "$DST/dependencies/$n"; done
# Windows path separators in includes
grep -rlE '#\s*include\s*["<][^">]*\\' "$DST/src" "$DST/shared" "$DST/include" 2>/dev/null | while read f; do sed -i -E '/#\s*include/s#\\#/#g' "$f"; done
# MSVC accepts pasting "::" and a name; the standard preprocessor does not
sed -i -E 's/::\s*##\s*([A-Za-z_])/::\1/g' "$DST/src/Mod.hpp"
# the MSVC form of the methods that return a struct (the headers choose the other one for GCC on Windows)
sed -i 's/#if defined(_MSC_VER) || !defined(_WIN32)/#if 1/' "$D/DirectX-Headers/include/directx/d3d12.h" "$D/DirectX-Headers/include/directx/d3d12video.h" 2>/dev/null || true
# Windows headers by the capitalisation the sources use (the cross compiler's are lower case)
python3 - "$DST" "$D" "$W/ci" <<'PY'
import os, re, sys
dst, deps, ci = sys.argv[1:4]
mingw = '/usr/share/mingw-w64/include'
low = {}
for root, _, files in os.walk(mingw):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), mingw)
        low.setdefault(rel.lower(), rel)
names = set()
for r in (dst + '/src', dst + '/shared', dst + '/include', dst + '/dependencies', deps + '/kananlib', deps + '/DirectXTK12/Inc', deps + '/DirectXTK12/Src', deps + '/safetyhook/include'):
    for root, _, files in os.walk(r):
        for f in files:
            if f.endswith(('.h', '.hpp', '.cpp', '.c', '.inl')):
                try:
                    s = open(os.path.join(root, f), errors='ignore').read()
                except OSError:
                    continue
                names.update(m.group(1).replace('\\', '/') for m in re.finditer(r'#\s*include\s*[<"]([^>"]+)[>"]', s))
for n in sorted(names):
    t = low.get(n.lower())
    if t and not os.path.exists(os.path.join(mingw, n)) and n.lower() != 'directxmath.h':
        p = os.path.join(ci, n)
        os.makedirs(os.path.dirname(p) or ci, exist_ok=True)
        if not os.path.lexists(p):
            os.symlink(os.path.join(mingw, t), p)
PY
echo "tree for the check: $DST"
