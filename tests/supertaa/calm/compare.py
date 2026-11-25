"""[fix100 20.11.2026] gpu/periphery_test.exe calm <dir> against the reference model, frame by frame.
   python3 calm/compare.py <dir of psim.py's files> <kind> <dir> [other dir: the same run of another build, bytes compared]
What the model stores is cut to 16 bit floats towards zero, as the software GPU of the test writes its textures (a graphics
card rounds to the nearest; CalmTAA(half=True) does that)."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "periphery"))
import calm as C  # noqa: E402
import pchain as P  # noqa: E402

d = sys.argv[3]
k = {}
for line in open(os.path.join(d, "calm.txt")):
    p = line.split()
    if p and p[0] != "jitter":
        k[p[0]] = float(p[1])
W, H, F = int(k["w"]), int(k["h"]), int(k["frames"])
if len(sys.argv) > 4:
    same = sum(open(os.path.join(d, f"o{t:03d}.bin"), "rb").read() == open(os.path.join(sys.argv[4], f"o{t:03d}.bin"), "rb").read() for t in range(F))
    print(f"{d} against {sys.argv[4]}: {same} of {F} frames have the same bytes")
    sys.exit(0 if same == F else 1)
seq = dict(np.load(os.path.join(sys.argv[1], f"p_{sys.argv[2]}.npz")))
taa = C.CalmTAA(feedback=k["feedback"]) if k["calm"] else P.light_taa(k["feedback"])


def cut16(x):
    h = x.astype(np.float16)
    over = np.abs(h.astype(np.float32)) > np.abs(x)
    return np.where(over, np.nextafter(h, np.float16(0)), h).astype(np.float32)


worst = 0.0
means = []
p999 = []
aux_worst = 0.0
for t in range(F):
    if k["calm"]:
        taa.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t], fresh=(t == int(k.get("fresh_at", -1))))
    else:
        taa.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t])
    taa.hist = cut16(taa.hist)
    if k["calm"]:
        taa.aux = cut16(taa.aux)
    m = taa.hist[..., :3]
    g = np.fromfile(os.path.join(d, f"o{t:03d}.bin"), dtype=np.float16).reshape(H, W, 4).astype(np.float32)
    df = np.abs(g[..., :3] - m)
    worst = max(worst, float(df.max()))
    means.append(float(df.mean()))
    p999.append(float(np.percentile(df, 99.9)))
    if k["calm"]:
        a = np.fromfile(os.path.join(d, f"a{t:03d}.bin"), dtype=np.float16).reshape(H, W, 4).astype(np.float32)
        aux_worst = max(aux_worst, float(np.percentile(np.abs(a[..., :2] - taa.aux[..., :2]), 99.9)))
        assert float(np.abs(g[..., 3] - 1.0).max()) == 0.0, "alpha"
print(f"{d}: {F} frames {W}x{H}, GPU against the model: mean {np.mean(means):.2e}, 99.9 % of the pixels within {max(p999):.2e} (median of the frames {np.median(p999):.2e}), "
      f"largest {worst:.2e}" + (f"; statistics 99.9 % within {aux_worst:.2e}" if k["calm"] else ""))
