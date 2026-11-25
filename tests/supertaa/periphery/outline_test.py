"""[fix84 18.11.2026] The outline of a near thing through the periphery's reprojection, against the picture the eye would
really see: a made-up scene whose answer is known.

   python3 outline_test.py <work dir> [--frames=48] [--variants=game,soft] [--sheet=png] [--speed=0.13,0.07] [--dump=npz]

A wall 3 m away and a near thing (a disc and a tilted bar, 0.8 m) with patterns of their own; the near thing moves a
fraction of a texel per frame. Per frame: the wide view's distances sampled with the frame's jitter (as the game's depth),
its colour as the supersampling TAA would hold it once settled (image of twice the size, anti-aliased, no jitter), the
motion. Through the game's passes (gpu/periphery_test.exe seq: d3d12::StableDepth, then d3d12::MonoPeriphery with 2 x 2
pixels per ray) to the left eye, and against the eye's true picture (the same scene seen from the eye, 12 x 12 samples per
pixel).
  game  one surface per ray (fix80 - fix83)
  soft  each pixel on its own surface, the outline from the stable depth's mean (MonoPeriphery::Params::soft_outlines)
Measures, in the band of 3 eye-image pixels around the true outline (pixels of the image with 2 x 2 per ray = 2 eye
pixels each): the mean error against the truth, the error's change from frame to frame (what trembles), and the same
where the eye sees past the outline (the gap the wide view cannot see into) and on the other side.
needs gpu/periphery_test.exe (gpu/build.sh) and a D3D12 to run it (see run_cases.sh: RUN, GPU_DIR)."""
import os
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reproject_sequence as RS  # noqa: E402

W = H = 96
DISP = 10.4
Z_NEAR, Z_FAR = 0.8, 3.0


def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def inside(x, y, cx, cy):
    """the near thing at wide-view position (columns, rows), its centre at cx, cy"""
    dx, dy = x - cx, y - cy
    disc = dx * dx + dy * dy < 17.0 ** 2
    u = dx * 0.94 + dy * 0.34 - 6.0
    v = -dx * 0.34 + dy * 0.94 + 22.0
    bar = (np.abs(u) < 30.0) & (np.abs(v) < 5.5)
    return disc | bar


def far_col(x, y):
    g = 0.35 + 0.25 * np.sin(x * 0.9) * np.sin(y * 0.7) + 0.15 * np.sin((x + y) * 0.23)
    return np.stack([g, g * 0.85 + 0.05, g * 0.6], -1)  # (red over blue everywhere: the near thing is told by blue over red)


def near_col(x, y, cx, cy):
    g = 0.55 + 0.3 * np.sin((x - cx) * 1.3) * np.cos((y - cy) * 1.1)
    return np.stack([g * 0.5, g * 0.8, g], -1)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = dict(a[2:].split("=", 1) for a in argv if a.startswith("--") and "=" in a)
    work = args[0]
    F = int(opt.get("frames", "48"))
    variants = opt.get("variants", "game,soft").split(",")
    vx, vy = [float(v) for v in opt.get("speed", "0.13,0.07").split(",")]
    warm = 20
    s_n, s_f = DISP / Z_NEAR, DISP / Z_FAR
    S = 12  # (samples per pixel and axis of the colour image and of the truth: fewer make their outlines move in steps)
    c2x = np.zeros((F, 2 * H, 2 * W, 3), np.float32)
    dist = np.zeros((F, H, W), np.float32)
    mv = np.zeros((F, H, W, 2), np.float32)
    truth = np.zeros((F, 2 * H, 2 * W, 3), np.float32)
    cover = np.zeros((F, 2 * H, 2 * W), np.float32)   # how much of the eye pixel the near thing covers
    gap = np.zeros((F, 2 * H, 2 * W), bool)           # far, and hidden from the wide view
    ys, xs = np.mgrid[0:H, 0:W]
    sub = (np.arange(S) + 0.5) / S
    for t in range(F):
        cx, cy = 52.0 + vx * t, 46.0 + vy * t
        jx, jy = halton(t % 8 + 1, 2) - 0.5, halton(t % 8 + 1, 3) - 0.5
        near = inside(xs + 0.5 + jx, ys + 0.5 + jy, cx, cy)
        dist[t] = np.where(near, Z_NEAR, Z_FAR)
        mv[t][near] = (-vx, -vy)
        # the colour image of twice the size, anti-aliased
        acc = np.zeros((2 * H, 2 * W, 3)); y2, x2 = np.mgrid[0:2 * H, 0:2 * W]
        tr = np.zeros((2 * H, 2 * W, 3)); cov = np.zeros((2 * H, 2 * W)); gp = np.zeros((2 * H, 2 * W))
        for sy in sub:
            for sx in sub:
                x = (x2 + sx) * 0.5; y = (y2 + sy) * 0.5
                n = inside(x, y, cx, cy)
                acc += np.where(n[..., None], near_col(x, y, cx, cy), far_col(x, y))
                # the eye: its ray at x meets the near thing at x + s_n of the wide view, the wall at x + s_f
                ne = inside(x + s_n, y, cx, cy)
                tr += np.where(ne[..., None], near_col(x + s_n, y, cx, cy), far_col(x + s_f, y))
                cov += ne
                gp += (~ne) & inside(x + s_f, y, cx, cy)
        c2x[t] = acc / (S * S); truth[t] = tr / (S * S); cover[t] = cov / (S * S); gap[t] = gp > 0
    res = {}
    for name in variants:
        # (a variant "soft:rate=0.06" hands further keys to the GPU test's seq mode)
        keys = dict((kv.split("=")[0], float(kv.split("=")[1])) for kv in name.split(":")[1:])
        res[name] = RS.reproject(work, "outline_" + name.replace(":", "_").replace("=", ""), c2x, dist, DISP, mv=mv, soft=name.startswith("soft"), extra=keys)[..., :3]
    res["truth"] = truth  # (the measures' floor: the true picture through the same measures)
    variants = variants + ["truth"]
    print("the outline of a near thing (%.1f m before a wall at %.1f m), moving %.2f / %.2f texels of the wide view per frame; %d frames, measured from frame %d" %
          (Z_NEAR, Z_FAR, vx, vy, F, warm))
    print("  %-6s | band around the outline: error, its change per frame | where the eye sees past it: error, change | other side: error, change | away from it: error" % "")
    m = 8
    for name in variants:
        e = np.abs(res[name] - truth).mean(-1)
        acc = np.zeros(7); n = 0
        for t in range(warm, F):
            edge = (cover[t] > 0.02) & (cover[t] < 0.98)
            band = ndimage.binary_dilation(edge, iterations=3)
            gband = band & ndimage.binary_dilation(gap[t], iterations=2)
            oband = band & ~gband
            band[:m] = band[-m:] = False; band[:, :m] = band[:, -m:] = False
            # the error's change: this frame's against the last frame's brought along with the near thing (whole pixels of
            # motion add up slowly: compare at the same place, the motion is a fraction of a pixel per frame)
            de = np.abs((res[name][t] - truth[t]) - (res[name][t - 1] - truth[t - 1])).mean(-1)
            acc += [e[t][band].mean(), de[band].mean(), e[t][gband & band].mean(), de[gband & band].mean(), e[t][oband & band].mean(), de[oband & band].mean(),
                    e[t][~ndimage.binary_dilation(edge, iterations=8)][..., None].mean()]
            n += 1
        acc /= n
        print("  %-6s | %.4f  %.4f | %.4f  %.4f | %.4f  %.4f | %.4f" % (name, *acc))
        # the outline's place: which pixels show the near thing (its colour is blue, the wall's is not), against the truth;
        # per pixel of outline length: pixels on the wrong side, and how many change sides from one frame to the next
        # without the truth changing there
        wrong = 0.0; flips = 0.0; length = 0.0; wrong_g = 0.0; flips_g = 0.0; length_g = 0.0
        for t in range(warm, F):
            mk = [(res[name][k][..., 2] - res[name][k][..., 0]) > 0.0 for k in (t - 1, t)]
            tk = [cover[k] >= 0.5 for k in (t - 1, t)]
            edge = tk[1] ^ ndimage.binary_erosion(tk[1])
            edge[:m] = edge[-m:] = False; edge[:, :m] = edge[:, -m:] = False
            near_gap = ndimage.binary_dilation(gap[t], iterations=3)
            zone = ndimage.binary_dilation(edge, iterations=4)
            w = (mk[1] ^ tk[1]) & zone
            f = ((mk[1] ^ mk[0]) & ~(tk[1] ^ tk[0])) & zone
            wrong += (w & ~near_gap).sum(); flips += (f & ~near_gap).sum(); length += (edge & ~near_gap).sum()
            wrong_g += (w & near_gap).sum(); flips_g += (f & near_gap).sum(); length_g += (edge & near_gap).sum()
        print("  %-6s   the outline's place, per pixel of its length: pixels on the wrong side %.3f, changing sides per frame without cause %.3f | "
              "beside the gap: %.3f, %.3f" % (name, wrong / length, flips / length, wrong_g / max(length_g, 1), flips_g / max(length_g, 1)))
    if "dump" in opt:
        np.savez(opt["dump"], truth=truth, cover=cover, gap=gap, **{k.replace(":", "_").replace("=", "").replace(".", ""): v for k, v in res.items() if k != "truth"})
    if "sheet" in opt:
        from PIL import Image
        t = F - 1
        variants = variants[:-1]
        tiles = [truth[t]] + [res[v][t] for v in variants] + [np.abs(res[v][t] - truth[t]) * 3 for v in variants]
        sheet = np.clip(np.concatenate([x[40:150, 20:170] for x in tiles], 1), 0, 1)
        Image.fromarray((sheet * 255).astype(np.uint8)).resize((sheet.shape[1] * 3, sheet.shape[0] * 3), Image.NEAREST).save(opt["sheet"])
        print("  %s: the truth, %s, then their errors x3 (the last frame)" % (opt["sheet"], ", ".join(variants)))


if __name__ == "__main__":
    main(sys.argv[1:])
