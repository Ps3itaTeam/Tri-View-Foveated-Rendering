"""The ghost scene (scene2.py): what a temporal anti-aliasing leaves behind.

    python3 evaluate2.py data [tremor,pan] [variant ...]        (the sequences: python3 scene2.py data; needs scipy)

Error against the exact picture (the area average of every pixel), display-encoded, per variant:
  scroll   text that scrolls on its surface without motion vectors: before (steady) | while it scrolls | frames 1-6
           after | frames 7-14 after
  ringA/B  the wall within 6 px of two things that move against it (slow / faster): what trails behind them
  wire     a band around two wires thinner than a pixel that move against the wall: error, and flicker of the error
           (along the wire's motion)
  dither   a soft shadow rendered as the engine does (each pixel and frame lit or dark): what is left of the noise,
           and its flicker
  slide    a highlight sliding over a surface (view dependent: no motion vectors)
  blink    a lamp that changes every third frame
  sparkle, lines   micro facets, thin bright lines: error and flicker
  walls    plain boards: error and flicker
fix76 (17 settings of statistics) against the mod's TAA with feedback 0.78, pan: scroll 1-6 after +71 %, ringA +117 %.
"""
import sys

import numpy as np
from scipy import ndimage

import resolvers as R
import scene2 as S2

LUMA = np.array([0.25, 0.5, 0.25], dtype=np.float32)


def run(seq, make):
    res = make()
    return np.stack([res.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t]) for t in range(seq["color"].shape[0])])


class Masks:
    """the regions per frame"""

    def __init__(self, seq):
        rid = seq["rid"]
        self.F, self.H, self.W = rid.shape
        sc = S2.Scene2(seq["cam"], self.F)
        ys, xs = np.mgrid[0:self.H, 0:self.W].astype(np.float64)
        self.xs, self.ys = xs, ys
        self.m = []
        self.wire_mv = []
        cam = seq["cam"]
        for t in range(self.F):
            r = rid[t]
            obj = r < 0
            near = ndimage.binary_dilation(obj, iterations=8)
            d = {}
            for i, n in enumerate(S2.REGION_NAMES):
                mk = r == i
                if mk.any():
                    d[n] = mk & ~ndimage.binary_dilation(~mk, iterations=3) & ~near
            for oid, n in ((-1, "A"), (-2, "B")):
                o = r == oid
                if o.any():
                    # (the gaps between the bars count as the thing: the ring is the wall around the whole of it)
                    solid = ndimage.binary_erosion(ndimage.binary_dilation(o, iterations=5), iterations=5)
                    d["ring" + n] = ndimage.binary_dilation(solid | o, iterations=6) & ~obj
            for oid, n, x0, y0, ht in ((-3, "wire_bright", 115.0, 2.0, 44.0), (-4, "wire_dark", 120.0, 52.0, 40.0)):
                wx = sc.obj_x(t, x0, 1.5)
                oy = ys + 0.5 - (y0 - 1.5 * (cam[t + 1][1] - cam[0][1]))
                band = (np.abs(xs + 0.5 - wx - 0.04 * oy) < 3.0) & (oy >= 2) & (oy < ht - 2)
                d[n] = band & ~ndimage.binary_dilation((r == -1) | (r == -2), iterations=4)
            self.wire_mv.append(sc.obj_shift(t, 1.5))
            self.m.append(d)


def metrics(seq, masks, outs, warm=24):
    ref = R.srgb_encode(seq["ref"])
    xs, ys = masks.xs, masks.ys
    acc = {}
    series = {}
    prev = None
    for t in range(masks.F):
        err = outs[t] - ref[t]
        el = ndimage.gaussian_filter(err @ LUMA, 1.0)
        if prev is not None:
            mv = seq["mv"][t]
            wb = ndimage.map_coordinates(prev, [ys + mv[..., 1], xs + mv[..., 0]], order=3, mode="nearest")
            wm = masks.wire_mv[t]
            ww = ndimage.map_coordinates(prev, [ys + wm[1], xs + wm[0]], order=3, mode="nearest")
        for n, mk in masks.m[t].items():
            if not mk.any():
                continue
            series.setdefault(n, {})[t] = float((err[mk] ** 2).mean())
            if t < warm:
                continue
            a = acc.setdefault(n, [0.0, 0, 0.0, 0])
            a[0] += float((err[mk] ** 2).sum())
            a[1] += int(mk.sum()) * 3
            if prev is not None:
                inb = mk & (xs > 3) & (xs < masks.W - 4) & (ys > 3) & (ys < masks.H - 4)
                a[2] += float(np.abs(el - (ww if n.startswith("wire") else wb))[inb].sum())
                a[3] += int(inb.sum())
        prev = el
    out = {n: (np.sqrt(a[0] / max(a[1], 1)), a[2] / max(a[3], 1)) for n, a in acc.items()}

    def win(t0, t1):
        v = [series["scroll"][t] for t in range(t0, t1 + 1) if "scroll" in series and t in series["scroll"]]
        return float(np.sqrt(np.mean(v))) if v else float("nan")

    s0, sn = S2.SCROLL_START, S2.SCROLL_FRAMES
    out["scroll_t"] = (win(30, s0 - 2), win(s0 - 1, s0 + sn - 2), win(s0 + sn - 1, s0 + sn + 4), win(s0 + sn + 5, s0 + sn + 12))
    return out


HEAD = "variant        scroll: before while  1-6    7-14  | ringA  ringB  | wire+ rm  fl   wire- rm  fl  | dither rm fl  | slide  blink  | sparkle rm fl | lines rm  fl  | walls rm  fl"


def line(name, m):
    def g(n, i):
        return m[n][i] if n in m else float("nan")
    st = m["scroll_t"]
    return ("%-14s %.4f %.4f %.4f %.4f | %.4f %.4f | %.4f %.4f  %.4f %.4f | %.4f %.4f | %.4f %.4f | %.4f %.4f | %.4f %.4f | %.4f %.4f" %
            (name, st[0], st[1], st[2], st[3], g("ringA", 0), g("ringB", 0), g("wire_bright", 0), g("wire_bright", 1), g("wire_dark", 0), g("wire_dark", 1),
             g("dither", 0), g("dither", 1), g("slide", 0), g("blink", 0), g("sparkle", 0), g("sparkle", 1), g("lines", 0), g("lines", 1),
             0.5 * (g("wall", 0) + g("wall2", 0)), 0.5 * (g("wall", 1) + g("wall2", 1))))


VARIANTS = {
    "old78": (8, lambda: R.OldTAA(feedback=0.78)),
    "s2": (32, lambda: R.NewTAA(scale=2)),
    "s2_noprotect": (32, lambda: R.NewTAA(scale=2, protect=False)),   # without the protection (what fix77 did)
    "s2_nofollow": (32, lambda: R.NewTAA(scale=2, follow=False)),
    "s2_w8": (32, lambda: R.NewTAA(scale=2, w_max=8.0)),
    "s2_w3": (32, lambda: R.NewTAA(scale=2, w_max=3.0)),
    "s2_clip125": (32, lambda: R.NewTAA(scale=2, clip_gamma=1.25)),
    "s1": (8, lambda: R.NewTAA(scale=1, sigma=0.35)),
}


def main(argv, variants=None):
    data = argv[0]
    kinds = argv[1].split(",") if len(argv) > 1 else ["tremor", "pan"]
    variants = variants or VARIANTS
    names = argv[2:] or ["old78", "s2", "s2_noprotect", "s1"]
    for kind in kinds:
        print("===== %s =====" % kind)
        print(HEAD, flush=True)
        cache = {}
        for n in names:
            phases, make = variants[n]
            if phases not in cache:
                seq = dict(np.load("%s/g_%s_p%d.npz" % (data, kind, phases)))
                cache[phases] = (seq, Masks(seq))
            seq, masks = cache[phases]
            print(line(n, metrics(seq, masks, run(seq, make))), flush=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    main(sys.argv[1:])
