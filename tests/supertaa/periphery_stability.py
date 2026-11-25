"""[fix82 17.11.2026] The stability of the periphery's supersampling TAA (quadview::Settings::periphery_stability): the wide
view's inputs of a chain capture from the game (wide_color, wide_motion, wide_depth: analyze_chain.py) through the reference
model with several values, each frame at its own corner of the wide view.

   python3 periphery_stability.py <capture folder> [--values=5,10,16]

Printed: the flicker of the anti-aliased wide image (the frame against the mean of its neighbours brought to its place by
the part's median motion; frames 24 on) at its own texels and low-passed over half, one and two pixels of the wide view,
its detail, and how far the model's image is from the game's (with the game's value: the model's check).
RE7 captures of 17.11.2026 16:0x (the game ran with 5):
  03 (blinds, head still)   the game 2.88 / 1.32 / 0.68 / 0.37, model 5: 2.84 / 1.30 / 0.67 / 0.37 (0.18 % from the game),
                            10: 2.51 / 1.08 / 0.49 / 0.26, 16: 2.47 / 1.06 / 0.47 / 0.24; detail 0.0215 / 0.0213 / 0.0213
  04 (head moving, 1.6 px)  5: 1.33 / 0.72 / 0.44 / 0.26, 10: 1.30 / 0.70 / 0.43 / 0.25 (the motion limits the weight)"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # (also when Python runs isolated, -I)
import analyze_capture as AC  # noqa: E402
import analyze_chain as ACH  # noqa: E402
import resolvers as R  # noqa: E402


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    folder = args[0]
    n, images, notes = ACH.read_chain(folder)
    need = ("wide_color", "wide_motion", "wide_depth", "wide_taa")
    stacks = {k: ACH.stack(folder, images, k, n)[0] for k in need}
    if n < 32 or any(s is None for k in need for s in stacks[k]):
        print("the capture does not hold the wide view's chain for all its frames")
        return
    H = min(stacks["wide_color"][0].shape[0], stacks["wide_taa"][0].shape[0] // 2) // 2 * 2
    W = min(stacks["wide_color"][0].shape[1], stacks["wide_taa"][0].shape[1] // 2) // 2 * 2
    col = [s[:H, :W, :3] for s in stacks["wide_color"]]
    zs = [s[:H, :W, 0] for s in stacks["wide_depth"]]
    game = np.stack([AC.luma(np.clip(s[:2 * H, :2 * W, :3], 0, 1)) for s in stacks["wide_taa"]])
    mvs, jit = [], []
    for i in range(n):
        w = notes[i]["wide"]
        m = stacks["wide_motion"][i][:H, :W]
        mvs.append(np.stack([m[..., 0] * w["mv_scale"][0] * w["view"][0], m[..., 1] * w["mv_scale"][1] * w["view"][1]], axis=-1).astype(np.float32))
        jit.append((w["jitter"][0], w["jitter"][1]))
    dk = notes[0]["wide"]["depth_k"]
    mid = [(float(np.median(m[..., 0])), float(np.median(m[..., 1]))) for m in mvs]
    edge = 16 if min(H, W) > 80 else 6

    def flicker(x, sig):
        xs = np.stack([ndimage.gaussian_filter(f, sig * 2) if sig > 0 else f for f in x])
        tot = []
        for i in range(24, n - 1):
            a = ndimage.shift(xs[i - 1], (-mid[i][1] * 2, -mid[i][0] * 2), order=1, mode="nearest")
            b = ndimage.shift(xs[i + 1], (mid[i + 1][1] * 2, mid[i + 1][0] * 2), order=1, mode="nearest")
            tot.append(np.abs(xs[i] - 0.5 * (a + b))[2 * edge:-2 * edge, 2 * edge:-2 * edge].mean())
        return 1000.0 * float(np.mean(tot))

    def run(w_max):
        taa = R.NewTAA(scale=2, sharpen=0.0, w_max=w_max)
        out = []
        for i in range(n):
            lin, guide, boxc, boxe = taa.analyze(col[i], mvs[i], None, jit[i], z=zs[i], depth_k=dk, reverse_z=True, reset=(i == 0), have_depth=True)
            taa.accumulate(lin, guide, boxc, boxe, jit[i], reset=(i == 0))
            out.append(AC.luma(np.clip(R.srgb_encode(taa.hist[..., :3]), 0, 1).astype(np.float32)))
        return np.stack(out)

    def row(name, x):
        det = float(np.abs(np.diff(x[40], axis=1))[2 * edge:-2 * edge, 2 * edge:-2 * edge].mean())
        far = float(np.abs(x[40:] - game[40:])[:, 2 * edge:-2 * edge, 2 * edge:-2 * edge].mean())
        print("  %-22s | %7.3f %7.3f %7.3f %7.3f | %.4f | %.4f" % (name, flicker(x, 0), flicker(x, 0.5), flicker(x, 1), flicker(x, 2), det, far))

    print("%s: the wide view's part %dx%d, motion %.2f pixels per frame" % (folder, W, H, float(np.mean([np.hypot(*m) for m in mid]))))
    print("  %-22s | flicker x1000 at the texels and over 0.5 / 1 / 2 pixels of the wide view | detail | from the game's image" % "stability")
    row("the game", game)
    for v in [float(t) for t in opt.get("values", "5,10,16").split(",")]:
        row("model, %g" % v, run(v))


if __name__ == "__main__":
    main(sys.argv[1:])
