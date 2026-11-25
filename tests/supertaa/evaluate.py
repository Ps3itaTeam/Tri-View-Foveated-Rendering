"""Runs resolvers over the synthetic sequences and measures them against the per-pixel area average.

Per region of the scene:
  rmse     root mean square error (encoded values, all channels)
  flicker  mean |error now - error of the frame before brought to this frame| (luma): what changes from frame to
           frame and should not
  detail   projection of the result's fine detail (luma minus its Gaussian blur) on the reference's: 1 = all of it,
           below 1 = blurred
  energy   fine detail energy over the reference's: above 1 = aliasing or over-sharpening
"""
import sys
import numpy as np
from scipy import ndimage

import scene as S
import resolvers as R

LUMA = np.array([0.25, 0.5, 0.25], dtype=np.float32)


def luma(x):
    return x @ LUMA


def run(seq, make, out_ratio=1.0):
    res = make()
    frames = seq["color"].shape[0]
    outs = []
    for t in range(frames):
        outs.append(res.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t]))
    return np.stack(outs)


def metrics(seq, outs, warm=24, per_frame=False):
    ref = R.srgb_encode(seq["ref"])
    rid = seq["rid"]
    frames, H, W = rid.shape
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    names = ["object", "obj_ring"] + S.REGION_NAMES
    acc = {n: dict(se=0.0, n=0, fl=0.0, fl2=0.0, fn=0, pr=0.0, rr=0.0, oo=0.0) for n in names}
    prev_err = None
    prev_err_lp = None
    series = {n: [] for n in names}
    sw = []
    for t in range(frames):
        err = outs[t] - ref[t]
        el = luma(err)
        el_lp = ndimage.gaussian_filter(el, 1.0)
        hp_o = luma(outs[t]) - ndimage.gaussian_filter(luma(outs[t]), 1.0)
        hp_r = luma(ref[t]) - ndimage.gaussian_filter(luma(ref[t]), 1.0)
        if prev_err is not None:
            mv = seq["mv"][t]
            px = xs + mv[..., 0]
            py = ys + mv[..., 1]
            warped = ndimage.map_coordinates(prev_err, [py, px], order=3, mode="nearest")
            warped_lp = ndimage.map_coordinates(prev_err_lp, [py, px], order=3, mode="nearest")
            prid = ndimage.map_coordinates(rid[t - 1].astype(np.float32), [py, px], order=0, mode="nearest")
            inb = (px >= 2) & (px <= W - 3) & (py >= 2) & (py <= H - 3) & (prid == rid[t])
            dfl = np.abs(el - warped)
            dfl2 = np.abs(el_lp - warped_lp)
        obj = rid[t] == -1
        ring = ndimage.binary_dilation(obj, iterations=6) & ~obj
        for i, n in enumerate(names):
            if n == "obj_ring":
                m = ring
            else:
                m = rid[t] == (i - 2)
                # away from the region's border (other content two pixels away)
                m = m & ~ndimage.binary_dilation(~m, iterations=3)
                if n != "object":
                    m = m & ~ndimage.binary_dilation(obj, iterations=8)
            if not m.any():
                series[n].append(np.nan)
                continue
            series[n].append(float(np.sqrt((err[m] ** 2).mean())))
            if t < warm:
                continue
            a = acc[n]
            a["se"] += float((err[m] ** 2).sum())
            a["n"] += int(m.sum()) * 3
            a["pr"] += float((hp_o[m] * hp_r[m]).sum())
            a["rr"] += float((hp_r[m] ** 2).sum())
            a["oo"] += float((hp_o[m] ** 2).sum())
            if prev_err is not None:
                mm = m & inb
                a["fl"] += float(dfl[mm].sum())
                a["fl2"] += float(dfl2[mm].sum())
                a["fn"] += int(mm.sum())
        prev_err = el
        prev_err_lp = el_lp
    out = {}
    for n in names:
        a = acc[n]
        if a["n"] == 0:
            continue
        out[n] = dict(rmse=np.sqrt(a["se"] / a["n"]), flicker=a["fl"] / max(a["fn"], 1), flick_lp=a["fl2"] / max(a["fn"], 1),
                      detail=a["pr"] / max(a["rr"], 1e-12), energy=a["oo"] / max(a["rr"], 1e-12))
    # the light that goes on at frame 40: error over the 12 frames after it
    s10 = np.array(series["switch"], dtype=np.float64)
    out["switch_resp"] = dict(rmse=float(np.nanmean(s10[40:52])), flicker=np.nan, flick_lp=np.nan, detail=np.nan, energy=np.nan)
    if per_frame:
        return out, series
    return out


def table(results, keys=("rmse", "flicker", "flick_lp", "detail", "energy"), regions=None):
    algos = list(results.keys())
    regions = regions or list(next(iter(results.values())).keys())
    lines = []
    for key in keys:
        lines.append(f"-- {key} " + "-" * 60)
        lines.append(f"{'region':12s} " + " ".join(f"{a[:12]:>12s}" for a in algos))
        for r in regions:
            vals = [results[a].get(r, {}).get(key, float('nan')) for a in algos]
            if all(v != v for v in vals):
                continue
            lines.append(f"{r:12s} " + " ".join(f"{v:12.4f}" for v in vals))
    return "\n".join(lines)


def contact_sheet(path, seq, outs_by_name, t, zoom=3):
    import imageio.v2 as imageio
    ref = R.srgb_encode(seq["ref"][t])
    tiles = [ref] + [np.clip(o[t], 0, 1) for o in outs_by_name.values()]
    row = np.concatenate(tiles, axis=1)
    row = np.repeat(np.repeat(row, zoom, axis=0), zoom, axis=1)
    imageio.imwrite(path, (np.clip(row, 0, 1) * 255 + 0.5).astype(np.uint8))


GROUPS = {
    "thin": ["lines_h", "lines_v", "lines_diag", "fence_dark"],
    "spec": ["glints", "brushed"],
    "tex": ["texture", "edge", "zoneplate", "object"],
}


def summary(res):
    """one line per variant: what matters, grouped"""
    def avg(v, regs, key):
        xs = [v[r][key] for r in regs if r in v]
        return float(np.mean(xs)) if xs else float("nan")
    lines = [f"{'variant':16s} {'thin fl':>8s} {'thin det':>8s} {'thin rm':>8s} | {'spec fl':>8s} {'spec rm':>8s} | {'tex det':>8s} {'tex rm':>8s} {'tex fl':>8s} |"
             f" {'ring':>7s} {'shadow':>7s} {'switch':>7s} {'fire':>7s}"]
    for name, v in res.items():
        lines.append(f"{name:16s} {avg(v, GROUPS['thin'], 'flick_lp'):8.4f} {avg(v, GROUPS['thin'], 'detail'):8.3f} {avg(v, GROUPS['thin'], 'rmse'):8.4f} | "
                     f"{avg(v, GROUPS['spec'], 'flick_lp'):8.4f} {avg(v, GROUPS['spec'], 'rmse'):8.4f} | "
                     f"{avg(v, GROUPS['tex'], 'detail'):8.3f} {avg(v, GROUPS['tex'], 'rmse'):8.4f} {avg(v, GROUPS['tex'], 'flick_lp'):8.4f} | "
                     f"{v['obj_ring']['rmse']:7.4f} {v['shadow']['rmse']:7.4f} {v['switch_resp']['rmse']:7.4f} {v['fire']['rmse']:7.4f}")
    return "\n".join(lines)


VARIANTS = {
    "old": (8, lambda: R.OldTAA()),                      # the mod's TAA as it comes (feedback 0.90)
    "old78": (8, lambda: R.OldTAA(feedback=0.78)),       # ... with the feedback of the first test in the game
    "s1": (8, lambda: R.NewTAA(scale=1, sigma=0.35)),    # the supersampling TAA, history 1x
    "s2": (32, lambda: R.NewTAA(scale=2)),               # ... 2x (the default)
    "s2_noprotect": (32, lambda: R.NewTAA(scale=2, protect=False)),   # ... without the protection (what fix77 did)
    "s2_nofollow": (32, lambda: R.NewTAA(scale=2, follow=False)),
}

_SEQ_CACHE = {}


def get_seq(data, kind, phases):
    key = (kind, phases)
    if key not in _SEQ_CACHE:
        _SEQ_CACHE[key] = dict(np.load(f"{data}/{kind}_p{phases}.npz"))
    return _SEQ_CACHE[key]


def run_all(data, kinds, variants, sheet_t=80, full=False, sheets=False, keep=None):
    for kind in kinds:
        res = {}
        outs = {}
        for name, (phases, make) in variants.items():
            seq = get_seq(data, kind, phases)
            outs[name] = run(seq, make)
            res[name] = metrics(seq, outs[name])
        print(f"===== {kind} =====")
        print(summary(res), flush=True)
        if full:
            print(table(res, keys=("rmse", "flick_lp", "detail")), flush=True)
        if sheets:
            contact_sheet(f"{data}/sheet_{kind}.png", get_seq(data, kind, 8), outs, sheet_t)
        if keep is not None:
            keep[kind] = (res, outs)


if __name__ == "__main__":
    data = sys.argv[1]
    kinds = sys.argv[2].split(",") if len(sys.argv) > 2 else ["tremor"]
    run_all(data, kinds, VARIANTS)
