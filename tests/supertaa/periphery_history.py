"""[fix83 17.11.2026] The history filter of the periphery's supersampling TAA (quadview::Settings::
periphery_history_sharpness): the wide view's inputs of a chain capture from the game (wide_color, wide_motion, wide_depth:
analyze_chain.py) through the reference model with several values, each frame at its own corner of the wide view.

   python3 periphery_history.py <capture folder> [--values=0.5,0,-0.25] [--stability=10] [--sheet=out.png]

Printed: the flicker of the anti-aliased wide image (the frame against the mean of its neighbours brought to its place by
the part's median motion; frames 24 on) at its own texels and low-passed over half, one and two pixels of the wide view,
its detail, what a frame holds above its own low-pass of one pixel of the wide view (the wavy outlines count here), and
how far the model's image is from the game's (with the game's values: the model's check). --sheet: the middle of frame 50
of the game's image and of every value side by side, three times enlarged (the outlines are plain to see).
RE7 captures of 17.11.2026 19:2x (fix82: the game ran with stability 10 and 0.5):
  02 (stairs)      the game 3.03 / 1.50 / 0.67 / 0.33, model 0.5: 2.84 / 1.41 / 0.65 / 0.32, 0: 1.77 / 1.05 / 0.55 / 0.27,
                   -0.5 (softer than Catmull-Rom; the shader stops at 0): 1.28 / 0.82 / 0.50 / 0.27;
                   detail 0.0265 / 0.0220 / 0.0185; above the low-pass 0.0305 / 0.0223 / 0.0165
  04 (inventory)   0.5: 2.20 / 0.89 / 0.37 / 0.16, 0: 1.35 / 0.67 / 0.31 / 0.13; detail 0.0200 / 0.0148
  01 (save menu)   0.5: 0.54 / 0.26 / 0.14 / 0.08, 0: 0.40 / 0.21 / 0.11 / 0.06; detail 0.0067 / 0.0061
With 0.5 a frame of the game's image is full of wavy outlines that are in another place a frame later; with 0 there are
none. Stability 16, a wider or narrower clip, the protection or the following of changes switched off change the flicker
by a few percent at most with 0 (capture 02)."""
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

    w_max = float(opt.get("stability", "10"))

    def run(extra):
        taa = R.NewTAA(scale=2, sharpen=0.0, w_max=w_max, hist_extra=extra)
        out = []
        for i in range(n):
            lin, guide, boxc, boxe = taa.analyze(col[i], mvs[i], None, jit[i], z=zs[i], depth_k=dk, reverse_z=True, reset=(i == 0), have_depth=True)
            taa.accumulate(lin, guide, boxc, boxe, jit[i], reset=(i == 0))
            out.append(np.clip(R.srgb_encode(taa.hist[..., :3]), 0, 1).astype(np.float32))
        return np.stack(out)

    tiles = []

    def row(name, c):
        x = np.stack([AC.luma(f) for f in c]) if c.ndim == 4 else c
        det = float(np.abs(np.diff(x[40], axis=1))[2 * edge:-2 * edge, 2 * edge:-2 * edge].mean())
        hp = float(np.mean([np.abs(x[i] - ndimage.gaussian_filter(x[i], 2.0))[2 * edge:-2 * edge, 2 * edge:-2 * edge].mean() for i in range(40, n)]))
        far = float(np.abs(x[40:] - game[40:])[:, 2 * edge:-2 * edge, 2 * edge:-2 * edge].mean())
        print("  %-22s | %7.3f %7.3f %7.3f %7.3f | %.4f | %.4f | %.4f" % (name, flicker(x, 0), flicker(x, 0.5), flicker(x, 1), flicker(x, 2), det, hp, far))
        if c.ndim == 4:
            tiles.append(c[min(50, n - 1)][H // 2:H // 2 * 3, W // 2:W // 2 * 3])

    print("%s: the wide view's part %dx%d, motion %.2f pixels per frame, stability %g" % (folder, W, H, float(np.mean([np.hypot(*m) for m in mid])), w_max))
    print("  %-22s | flicker x1000 at the texels and over 0.5 / 1 / 2 pixels of the wide view | detail | above the low-pass | from the game's image" % "history sharpness")
    row("the game", np.stack([np.clip(s[:2 * H, :2 * W, :3], 0, 1) for s in stacks["wide_taa"]]))
    for v in [float(t) for t in opt.get("values", "0.5,0,-0.25").split(",")]:
        row("model, %g" % v, run(v))
    if "sheet" in opt and tiles:
        from PIL import Image
        sheet = np.concatenate(tiles, axis=1)
        Image.fromarray((sheet * 255).astype(np.uint8)).resize((sheet.shape[1] * 3, sheet.shape[0] * 3), Image.NEAREST).save(opt["sheet"])
        print("  sheet: %s (the game's image, then every value)" % opt["sheet"])


if __name__ == "__main__":
    main(sys.argv[1:])
