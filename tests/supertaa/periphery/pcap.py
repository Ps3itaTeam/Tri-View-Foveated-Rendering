"""[fix80 17.11.2026] The periphery's chain on a capture from the game (quadview_dump/supertaa_captureNN): the wide view
is made from the fovea view's own frames (every 4th sample per axis, placed as the engine's jitter would place it;
textures averaged over the wide pixel's footprint on the same surface, as mip-mapping does), run through the chains of
pchain.py and compared with what the fovea shows at the same place.

   python3 pcap.py <capture folder> <fovea images: "out" or a stack .npy> <variants> [mip | point] [sheet.png]
     "out": the fovea image the capture holds (what the game showed); a .npy: frames x height x width x 3 of the part
     (e.g. the model's replay of the capture)
prints per variant: diff (mean difference from the fovea's image), detA / detB / detC (detail kept in three bands, see
pchain.py), fl1 / fl3 (what changes from frame to frame in the difference from the fovea, low-passed 1 / 3 E pixels),
own1 (what changes from frame to frame in the periphery's own image along the motion).
"""
import os
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (also when Python runs isolated, -I)
import pchain  # noqa: E402
import psim  # noqa: E402

sys.path.insert(0, pchain.TOOLS)
import capture_io as CIO  # noqa: E402
import resolvers as R  # noqa: E402

K = 4


def wide_sequence(cap, mip=True):
    F = cap.count
    col = []
    mvs = []
    dist = []
    jit = []
    for t in range(F):
        c = cap.image_of(t, "color")[..., :3]
        m = cap.motion_px(t)
        z = cap.depth(t)
        d = R.NewTAA.distance_of(z, cap.depth_k)
        j = np.array(cap.frames[t]["jitter"], dtype=np.float64)
        H, W = c.shape[:2]
        n0 = np.clip(np.round(K * (0.5 - j) - 0.5 + j), 0, K - 1).astype(np.int64)
        jeff = 0.5 - (n0 + 0.5 - j) / K
        ys = np.arange(H // K) * K + n0[1]
        xs = np.arange(W // K) * K + n0[0]
        c0 = c[ys][:, xs]
        d0 = d[ys][:, xs]
        if mip:
            acc = np.zeros_like(c0, dtype=np.float64)
            wsum = np.zeros(d0.shape, dtype=np.float64)
            lin = R.srgb_decode(c)
            for dy in range(-1, 3):
                for dx in range(-1, 3):
                    yy = np.clip(ys + dy, 0, H - 1)
                    xx = np.clip(xs + dx, 0, W - 1)
                    same = (np.abs(d[yy][:, xx] - d0) <= 0.1 * d0).astype(np.float64)
                    acc += lin[yy][:, xx] * same[..., None]
                    wsum += same
            c0 = R.srgb_encode((acc / np.maximum(wsum, 1.0)[..., None]).astype(np.float32))
            c0 = np.round(c0 * 255.0) / 255.0
        col.append(c0.astype(np.float32))
        mvs.append((m[ys][:, xs] / K).astype(np.float32))
        dist.append(d0.astype(np.float32))
        jit.append(jeff)
    return dict(color=np.stack(col), mv=np.stack(mvs), dist=np.stack(dist), jitter=np.array(jit, dtype=np.float32))


def run(folder, fovea_path, names, mip=True, warm=24, margin=32, sheet=None):
    cap = CIO.Capture(folder)
    seq = wide_sequence(cap, mip)
    if fovea_path == "out":
        fov = np.stack([cap.image_of(t, "out")[..., :3] for t in range(cap.count)]).astype(np.float32)
    else:
        fov = np.load(fovea_path).astype(np.float32)
    F, EH, EW = fov.shape[:3]
    V = pchain.variants()
    chains = {n: V[n]() for n in names}
    acc = {n: dict(ad=0.0, n=0, fl1=0.0, fl3=0.0, fn=0, own1=0.0, **{f"pr{i}": 0.0 for i in range(3)}, **{f"rr{i}": 0.0 for i in range(3)},
                   **{f"oo{i}": 0.0 for i in range(3)}) for n in names}
    prev = {n: None for n in names}
    sl = (slice(margin, EH - margin), slice(margin, EW - margin))
    keep = {}
    for t in range(F):
        ref = fov[t]
        lr = pchain.luma(ref)
        br = [ndimage.gaussian_filter(lr, s0) - ndimage.gaussian_filter(lr, s1) for s0, s1 in pchain.BANDS]
        mv = cap.motion_px(t)
        ys, xs = np.mgrid[0:EH, 0:EW].astype(np.float32)
        px = xs + mv[..., 0]
        py = ys + mv[..., 1]
        for n in names:
            out = chains[n].step(seq, t, EW, EH)
            lo = pchain.luma(out)
            d = lo - lr
            d1 = ndimage.gaussian_filter(d, 1.0)
            d3 = ndimage.gaussian_filter(d, 3.0)
            o1 = ndimage.gaussian_filter(lo, 1.0)
            a = acc[n]
            if t >= warm:
                a["ad"] += float(np.abs(d[sl]).sum())
                a["n"] += d[sl].size
                for i, (s0, s1) in enumerate(pchain.BANDS):
                    bo = ndimage.gaussian_filter(lo, s0) - ndimage.gaussian_filter(lo, s1)
                    a[f"pr{i}"] += float((bo[sl] * br[i][sl]).sum())
                    a[f"rr{i}"] += float((br[i][sl] ** 2).sum())
                    a[f"oo{i}"] += float((bo[sl] ** 2).sum())
                if prev[n] is not None:
                    w1 = ndimage.map_coordinates(prev[n][0], [py, px], order=1, mode="nearest")
                    w3 = ndimage.map_coordinates(prev[n][1], [py, px], order=1, mode="nearest")
                    wo = ndimage.map_coordinates(prev[n][2], [py, px], order=1, mode="nearest")
                    a["fl1"] += float(np.abs(d1 - w1)[sl].sum())
                    a["fl3"] += float(np.abs(d3 - w3)[sl].sum())
                    a["own1"] += float(np.abs(o1 - wo)[sl].sum())
                    a["fn"] += d[sl].size
            prev[n] = (d1, d3, o1)
            if t == F - 1:
                keep[n] = out
    res = {}
    for n in names:
        a = acc[n]
        r = dict(diff=a["ad"] / max(a["n"], 1), fl1=a["fl1"] / max(a["fn"], 1), fl3=a["fl3"] / max(a["fn"], 1), own1=a["own1"] / max(a["fn"], 1))
        for i, b in enumerate("ABC"):
            r["det" + b] = a[f"pr{i}"] / max(a[f"rr{i}"], 1e-12)
        res[n] = r
    if sheet:
        import imageio.v2 as imageio
        raw = np.repeat(np.repeat(seq["color"][-1], K, axis=0), K, axis=1)
        tiles = [fov[-1], raw] + [keep[n] for n in names]
        img = np.concatenate([np.clip(x, 0, 1) for x in tiles], axis=1)
        imageio.imwrite(sheet, (img * 255 + 0.5).astype(np.uint8))
    return res


if __name__ == "__main__":
    folder, fovea_path = sys.argv[1], sys.argv[2]
    names = sys.argv[3].split(",")
    mip = not (len(sys.argv) > 4 and sys.argv[4] == "point")
    sheet = sys.argv[5] if len(sys.argv) > 5 else None
    res = run(folder, fovea_path, names, mip=mip, sheet=sheet)
    keys = ["diff", "detA", "detB", "detC", "fl1", "fl3", "own1"]
    print(f"{'variant':10s} " + " ".join(f"{k:>8s}" for k in keys))
    for n in names:
        print(f"{n:10s} " + " ".join(f"{res[n][k]:8.4f}" for k in keys), flush=True)
