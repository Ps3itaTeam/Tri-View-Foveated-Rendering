"""[fix80 17.11.2026] The periphery's chain on the simulated sequences of psim.py, measured at the eye image's density.
   python3 pchain.py <dir of psim.py's files> <kind> <variants, comma separated> <out prefix>
prints a table per group of regions and writes <out prefix>_<kind>.pkl (every region) and _<kind>.png (the exact
picture and every variant's, frame 90, side by side).

  temporal pass on the wide view (Light TAA as the mod has it / the supersampling TAA at 1x or 2x)
  -> the eye's share of it (MonoPeriphery reads the wide image at a fractional column: a shift f along x)
  -> enlarged to the eye image (EyeCompose / WideUpscale: Catmull-Rom kept within the 2x2 around)

Measured against the exact picture (area average of every E pixel), per region of the scene:
  rmse      all of the error (encoded values)
  rmse_lp   the error low-passed with sigma 2 E pixels: what a periphery could show at all
  band A/B/C  detail kept in three bands (projection on the reference's: 1 = all, in place):
            A wavelengths ~ 4-8 E px (1-2 wide px: only more than one sample per wide pixel can hold it)
            B ~ 8-16 E px (2-4 wide px: the top of what the wide view's own grid holds)
            C ~ 16-32 E px
  flicker   mean |error now - error of the frame before brought to this frame|, low-passed sigma 1 and sigma 3 E px
"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.environ.get("TOOLS", os.path.join(HERE, ".."))
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import resolvers as R  # noqa: E402
import scene as S  # noqa: E402
import psim  # noqa: E402
import calm as CALM  # noqa: E402

LUMA = np.array([0.25, 0.5, 0.25], dtype=np.float32)
BANDS = [(1.3, 2.6), (2.6, 5.2), (5.2, 10.4)]


def luma(x):
    return x @ LUMA


def light_taa(fb=0.90):
    """LIGHT_TAA_SHADER: rebuild exp(-2.29 d^2), clip mean +- 1.25 sigma, no disocclusion test, no sharpening"""
    return R.OldTAA(feedback=fb, clip_gamma=1.25, filter_radius=0.46727, new_softness=0.0, motion_reduce=0.25, depth_reject=0.0,
                    sharpen=0.0, dilate=True, catmull=True, luma_weight=True, fast_converge=False)


def cr_weights(t):
    return np.stack([t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t), t * (0.5 + t * (2.0 - 1.5 * t)),
                     t * t * (-0.5 + 0.5 * t)], axis=-1)


def shift_x(img, f, mode="bilinear"):
    """the image read f pixels to the right of every pixel centre"""
    if f == 0.0:
        return img
    H, W = img.shape[:2]
    xs = np.arange(W)

    def col(i):
        return img[:, np.clip(xs + i, 0, W - 1)]

    if mode == "bilinear":
        return col(0) * (1.0 - f) + col(1) * f
    w = cr_weights(np.float64(f))
    out = sum(w[i] * col(i - 1) for i in range(4))
    if mode == "cr_clamp":
        out = np.clip(out, np.minimum(col(0), col(1)), np.maximum(col(0), col(1)))
    return out.astype(np.float32)


def magnify(img, EW, EH, k, fx=0.0):
    """WIDE_UPSCALE_SHADER: Catmull-Rom 4x4, the result kept within the range of the inner 2x2. The source pixel under the
    centre of destination pixel i: (i + 0.5) / k - 0.5 - fx (fx: the source was read fx to the right, see shift_x)"""
    H, W = img.shape[:2]
    spx = (np.arange(EW) + 0.5) / k - 0.5 - fx
    spy = (np.arange(EH) + 0.5) / k - 0.5
    bx = np.floor(spx).astype(np.int64)
    by = np.floor(spy).astype(np.int64)
    wx = cr_weights(spx - bx).astype(np.float32)
    wy = cr_weights(spy - by).astype(np.float32)
    horiz = 0.0
    for i in range(4):
        horiz = horiz + img[:, np.clip(bx + i - 1, 0, W - 1)] * wx[None, :, i, None]
    c = 0.0
    for j in range(4):
        c = c + horiz[np.clip(by + j - 1, 0, H - 1)] * wy[:, j, None, None]
    x0 = np.clip(bx, 0, W - 1)
    x1 = np.clip(bx + 1, 0, W - 1)
    y0 = np.clip(by, 0, H - 1)
    y1 = np.clip(by + 1, 0, H - 1)
    a = img[y0][:, x0]
    b = img[y0][:, x1]
    d = img[y1][:, x0]
    e = img[y1][:, x1]
    lo = np.minimum(np.minimum(a, b), np.minimum(d, e))
    hi = np.maximum(np.maximum(a, b), np.maximum(d, e))
    return np.clip(c, lo, hi).astype(np.float32)


def box_down(lin, k):
    H, W = lin.shape[:2]
    return lin.reshape(H // k, k, W // k, k, -1).mean(axis=(1, 3))


# ---- the chains ----------------------------------------------------------------------------------------------------
class Chain:
    """taa: object with step(color, mv, dist, jitter) -> encoded image at 1x; out2x: take the supersampling TAA's history
    (2x) instead; shift: (f, mode) of the eye's share (the reprojection reads the wide image at a fractional column: a
    quarter of a pixel of the image it reads, bilinear); rcas: sharpening of the image before the shift"""

    def __init__(self, make, out2x=False, shift=(0.25, "bilinear"), rcas=0.0):
        self.taa = make()
        self.out2x = out2x
        self.shift = shift
        self.rcas = rcas

    def step(self, seq, t, EW, EH):
        o = self.taa.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t])
        if self.out2x:
            o = R.srgb_encode(self.taa.hist[..., :3])
            dens = 2
        else:
            dens = 1
        if self.rcas > 0.0:
            o = R.rcas(o, self.rcas)
        f, mode = self.shift
        o = shift_x(o, f, mode)
        return magnify(o, EW, EH, psim.K / dens, fx=f)


class Raw:
    def step(self, color, mv, dist, jitter):
        return color


class Ideal:
    """the exact picture averaged over the pixels of an image dens times as dense as the wide view (no temporal pass at all)"""

    def __init__(self, dens):
        self.dens = dens

    def step_e(self, seq, t, EW, EH):
        lin = seq["ref"][t].astype(np.float32)
        k = psim.K // self.dens
        img = R.srgb_encode(box_down(lin, k))
        return magnify(img, EW, EH, k)


def variants():
    """what anti-aliases the wide view, and how its image gets to the eye:
      raw      nothing (the frame as rendered)
      light90, light97   the light TAA (history weight 0.90: the default; 0.97)                    periphery_aa 0
      super1x  the supersampling TAA, image of the wide view's size (history 2x, Lanczos 2 down)   periphery_aa 1
      super2x  the supersampling TAA, image of twice the size (the history itself)                 periphery_aa 2
      ideal1x, ideal2x   the exact picture averaged over the pixels of an image of that size (no temporal pass: what an
               image of that size can hold at best through the same enlargement)
      calm     [fix100] the calm light TAA (calm.py)                                                periphery_aa 3
      ship1x   [fix100] super1x with the periphery's own values since fix82 / fix83 (stability 10, plain Catmull-Rom)
    and for comparison: super1x_box (the mean of 2 x 2 instead of Lanczos 2), super1x_np / super2x_np (without the
    protection of what one frame cannot show), super1h (history of the wide view's own size)"""
    N = R.NewTAA
    return {
        "raw": lambda: Chain(Raw),
        "light90": lambda: Chain(lambda: light_taa(0.90)),
        "light97": lambda: Chain(lambda: light_taa(0.97)),
        "super1x": lambda: Chain(lambda: N(scale=2, down="lanczos2")),
        "super2x": lambda: Chain(lambda: N(scale=2, sharpen=0.0), out2x=True),
        "calm": lambda: Chain(lambda: CALM.CalmTAA()),
        "ship1x": lambda: Chain(lambda: N(scale=2, down="lanczos2", w_max=10.0, hist_extra=0.0)),
        "ideal1x": lambda: Ideal(1),
        "ideal2x": lambda: Ideal(2),
        "super1x_box": lambda: Chain(lambda: N(scale=2)),
        "super1x_np": lambda: Chain(lambda: N(scale=2, down="lanczos2", protect=False)),
        "super2x_np": lambda: Chain(lambda: N(scale=2, sharpen=0.0, protect=False), out2x=True),
        "super1h": lambda: Chain(lambda: N(scale=1, sigma=0.35)),
    }


# ---- measures ------------------------------------------------------------------------------------------------------
NAMES = ["object", "obj_ring"] + S.REGION_NAMES
GROUPS = {
    "thin": ["lines_h", "lines_v", "lines_diag", "fence_dark"],
    "spec": ["glints", "brushed"],
    "tex": ["texture", "zoneplate", "object"],
    "edge": ["edge"],
    "chg": ["fire", "shadow", "switch"],
}


def region_masks(rid):
    """E pixels well inside each region (12 E px from its border, 24 from the object), the object and the ring around it"""
    obj = rid == -1
    border = np.zeros(rid.shape, dtype=bool)
    border[:, 1:] |= rid[:, 1:] != rid[:, :-1]
    border[1:, :] |= rid[1:, :] != rid[:-1, :]
    dist_b = ndimage.distance_transform_edt(~border)
    dist_o = ndimage.distance_transform_edt(~obj) if obj.any() else np.full(rid.shape, 1e9)
    masks = {}
    masks["object"] = obj & (dist_b > 8)
    masks["obj_ring"] = (~obj) & (dist_o <= 24)
    for i, n in enumerate(S.REGION_NAMES):
        masks[n] = (rid == i) & (dist_b > 12) & (dist_o > 24)
    return masks


class Meter:
    def __init__(self):
        self.acc = {n: dict(se=0.0, n=0, selp=0.0, nl=0, fl1=0.0, fl3=0.0, fn=0, **{f"pr{i}": 0.0 for i in range(3)},
                            **{f"rr{i}": 0.0 for i in range(3)}, **{f"oo{i}": 0.0 for i in range(3)}) for n in NAMES}
        self.prev = None
        self.series = []

    def add(self, out, ref_enc, masks, mve, measure):
        err = out - ref_enc
        el = luma(err)
        e1 = ndimage.gaussian_filter(el, 1.0)
        e2 = ndimage.gaussian_filter(el, 2.0)
        e3 = ndimage.gaussian_filter(el, 3.0)
        lo = luma(out)
        lr = luma(ref_enc)
        bo = []
        br = []
        for s0, s1 in BANDS:
            bo.append(ndimage.gaussian_filter(lo, s0) - ndimage.gaussian_filter(lo, s1))
            br.append(ndimage.gaussian_filter(lr, s0) - ndimage.gaussian_filter(lr, s1))
        H, W = el.shape
        if self.prev is not None:
            ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
            px = xs + mve[..., 0]
            py = ys + mve[..., 1]
            w1 = ndimage.map_coordinates(self.prev[0], [py, px], order=1, mode="nearest")
            w3 = ndimage.map_coordinates(self.prev[1], [py, px], order=1, mode="nearest")
            inb = (px >= 4) & (px <= W - 5) & (py >= 4) & (py <= H - 5)
            d1 = np.abs(e1 - w1)
            d3 = np.abs(e3 - w3)
        if measure:
            for n in NAMES:
                m = masks[n]
                if not m.any():
                    continue
                a = self.acc[n]
                a["se"] += float((err[m] ** 2).sum())
                a["n"] += int(m.sum()) * 3
                a["selp"] += float((e2[m] ** 2).sum())
                a["nl"] += int(m.sum())
                for i in range(3):
                    a[f"pr{i}"] += float((bo[i][m] * br[i][m]).sum())
                    a[f"rr{i}"] += float((br[i][m] ** 2).sum())
                    a[f"oo{i}"] += float((bo[i][m] ** 2).sum())
                if self.prev is not None:
                    mm = m & inb
                    a["fl1"] += float(d1[mm].sum())
                    a["fl3"] += float(d3[mm].sum())
                    a["fn"] += int(mm.sum())
        self.prev = (e1, e3)

    def result(self):
        out = {}
        for n in NAMES:
            a = self.acc[n]
            if a["n"] == 0:
                continue
            r = dict(rmse=np.sqrt(a["se"] / a["n"]), rmse_lp=np.sqrt(a["selp"] / max(a["nl"], 1)), fl1=a["fl1"] / max(a["fn"], 1),
                     fl3=a["fl3"] / max(a["fn"], 1))
            for i, b in enumerate("ABC"):
                r["det" + b] = a[f"pr{i}"] / max(a[f"rr{i}"], 1e-12)
                r["en" + b] = a[f"oo{i}"] / max(a[f"rr{i}"], 1e-12)
            out[n] = r
        return out


def summary(res, groups=GROUPS):
    keys = ["rmse_lp", "detA", "detB", "detC", "fl1", "fl3"]
    lines = []
    for g, regs in groups.items():
        lines.append(f"-- {g} ({', '.join(regs)})")
        lines.append(f"{'variant':10s} " + " ".join(f"{k:>8s}" for k in keys))
        for name, v in res.items():
            vals = []
            for k in keys:
                xs = [v[r][k] for r in regs if r in v]
                vals.append(float(np.mean(xs)) if xs else float("nan"))
            lines.append(f"{name:10s} " + " ".join(f"{x:8.4f}" for x in vals))
    return "\n".join(lines)


def run(seq_path, names, warm=32, sheet=None, sheet_t=None, frames=None):
    seq = dict(np.load(seq_path))
    F = seq["color"].shape[0] if frames is None else frames
    EH, EW = seq["ref"].shape[1:3]
    V = variants()
    chains = {n: V[n]() for n in names}
    meters = {n: Meter() for n in names}
    keep = {}
    for t in range(F):
        ref_enc = R.srgb_encode(seq["ref"][t].astype(np.float32))
        masks = region_masks(seq["rid"][t])
        mve = seq["mve"][t].astype(np.float32)
        for n in names:
            ch = chains[n]
            out = ch.step_e(seq, t, EW, EH) if isinstance(ch, Ideal) else ch.step(seq, t, EW, EH)
            meters[n].add(out, ref_enc, masks, mve, t >= warm)
            if sheet_t is not None and t == sheet_t:
                keep[n] = out
        if sheet_t is not None and t == sheet_t:
            keep["_ref"] = ref_enc
    res = {n: meters[n].result() for n in names}
    if sheet and keep:
        import imageio.v2 as imageio
        tiles = [keep["_ref"]] + [keep[n] for n in names]
        img = np.concatenate([np.clip(x, 0, 1) for x in tiles], axis=1)
        imageio.imwrite(sheet, (img * 255 + 0.5).astype(np.uint8))
    return res


if __name__ == "__main__":
    import pickle
    seq_dir = sys.argv[1]
    kind = sys.argv[2]
    names = sys.argv[3].split(",")
    out = sys.argv[4]
    res = run(os.path.join(seq_dir, f"p_{kind}.npz"), names, sheet=out + f"_{kind}.png", sheet_t=90)
    with open(out + f"_{kind}.pkl", "wb") as f:
        pickle.dump(res, f)
    print(f"===== {kind} =====")
    print(summary(res), flush=True)
