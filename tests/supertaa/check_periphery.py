"""Checks what gpu/periphery_test.exe wrote ([PERIPHERY 2X fix80], d3d12::MonoPeriphery::Params::ss = 2).
   check_periphery.py <dir of the test's files>

The test's colour holds the place in the image in red and green (u, v), so every pixel of an eye's periphery tells where
the pass read it from. Checked per eye:
  1. ss 1, the faster search against the original pass: the same image.
  2. ss 2 against ss 1: the four pixels of every ray come from where the ray's own pixel comes from, a quarter of a
     column and row (times the pass's scale) to each side, with the same alpha (1 = a surface found, 0 = a gap filled).
     Left out: rays read within 3 columns or rows of the image's border (reads past the border are held at it, at the
     two sizes by different amounts).
  3. the eye pass's shader on the left eye's periphery alone (when the test got its text): its enlargement to the eye
     image against the formula, for the periphery of ss 1 (x4) and of ss 2 (x2), each with groups of 32 x 32 and of
     16 x 16 pixels (d3d12::EyeCompose takes 32 x 32 for the first and 16 x 16 for the second).
"""
import sys

import numpy as np


def cr_weights(t):
    return np.stack([t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t), t * (0.5 + t * (2.0 - 1.5 * t)), t * t * (-0.5 + 0.5 * t)], axis=-1)


def enlarge(img, ow, oh):
    """the eye pass's enlargement of the periphery (EYE_COMPOSE_SHADER, periphery_direct; WIDE_UPSCALE_SHADER): Catmull-Rom
    over 4 x 4 pixels, the result kept within the range of the inner 2 x 2"""
    h, w = img.shape[:2]
    spx = (np.arange(ow, dtype=np.float32) + np.float32(0.5)) * np.float32(w) / np.float32(ow) - np.float32(0.5)
    spy = (np.arange(oh, dtype=np.float32) + np.float32(0.5)) * np.float32(h) / np.float32(oh) - np.float32(0.5)
    bx = np.floor(spx).astype(np.int64)
    by = np.floor(spy).astype(np.int64)
    wx = cr_weights((spx - bx).astype(np.float64))
    wy = cr_weights((spy - by).astype(np.float64))
    c = 0.0
    for j in range(4):
        rows = img[np.clip(by + j - 1, 0, h - 1)]
        for i in range(4):
            c = c + rows[:, np.clip(bx + i - 1, 0, w - 1)] * (wx[None, :, i, None] * wy[:, j, None, None])
    x0, x1 = np.clip(bx, 0, w - 1), np.clip(bx + 1, 0, w - 1)
    y0, y1 = np.clip(by, 0, h - 1), np.clip(by + 1, 0, h - 1)
    four = [img[y0][:, x0], img[y0][:, x1], img[y1][:, x0], img[y1][:, x1]]
    return np.clip(c, np.minimum.reduce(four), np.maximum.reduce(four))


def main():
    d = sys.argv[1]
    meta = {}
    eyes = {}
    for line in open(d + "/meta.txt"):
        t = line.split()
        if t[0] == "eye":
            eyes[int(t[1])] = {t[i]: float(t[i + 1]) for i in range(2, len(t), 2)}
        else:
            meta[t[0]] = float(t[1])
    W, H = int(meta["width"]), int(meta["height"])
    bad = 0
    for eye, tag in enumerate("LR"):
        e = eyes[eye]
        ew, eh = int(e["ew"]), int(e["eh"])
        a = np.fromfile(f"{d}/out1_{tag}.bin", dtype="<f4").reshape(eh, ew, 4).astype(np.float64)
        b = np.fromfile(f"{d}/out2_{tag}.bin", dtype="<f4").reshape(2 * eh, 2 * ew, 4).astype(np.float64)
        o = np.fromfile(f"{d}/out0_{tag}.bin", dtype="<f4").reshape(eh, ew, 4).astype(np.float64)
        same = float(np.abs(a - o).max())
        hit = a[..., 3] > 0.5
        col = a[..., 0] * W
        row = a[..., 1] * H
        inner = (col > 3.0) & (col < W - 3.0) & (row > 3.0) & (row < H - 3.0)
        worst_hit = 0.0
        worst_gap = 0.0
        alpha_off = 0
        off = 0
        for k in range(4):
            ax, ay = k & 1, k >> 1
            bk = b[ay::2, ax::2]
            ex = (bk[..., 0] - a[..., 0]) * W - (ax - 0.5) * 0.5 * e["x_scale"]
            ey = (bk[..., 1] - a[..., 1]) * H - (ay - 0.5) * 0.5 * e["y_scale"]
            err = np.maximum(np.abs(ex), np.abs(ey))
            alpha_off += int((bk[..., 3] != a[..., 3]).sum())
            if (hit & inner).any():
                worst_hit = max(worst_hit, float(err[hit & inner].max()))
            if (~hit & inner).any():
                worst_gap = max(worst_gap, float(err[~hit & inner].max()))
            off += int((err[inner] > 0.02).sum())
        ok = same == 0.0 and off == 0 and alpha_off == 0
        bad += 0 if ok else 1
        print(f"{'left' if eye == 0 else 'right'} eye: {ew}x{eh} rays, a surface found in {100.0 * hit.mean():.1f} %, gaps filled in "
              f"{100.0 * (~hit).mean():.1f} % ({int((~hit).sum())} rays; {int((~hit & inner).sum())} of them checked) | faster search against the "
              f"original pass at ss 1: largest difference {same:.3g} | ss 2: the four pixels of a ray off its own place by at most "
              f"{worst_hit:.2g} columns or rows where a surface was found, {worst_gap:.2g} in the gaps; pixels off by more than 0.02: {off}; "
              f"alpha differing: {alpha_off} -> {'ok' if ok else 'WRONG'}")
    # 3. the eye pass's enlargement of the left eye's periphery, at both densities and with both group sizes, against the
    #    formula (files written only when the test got the eye pass's shader text)
    import os
    e = eyes[0]
    ew, eh = int(e["ew"]), int(e["eh"])
    eye_runs = 0
    for ss in (1, 2):
        per = np.fromfile(f"{d}/out{ss}_L.bin", dtype="<f4").reshape(ss * eh, ss * ew, 4).astype(np.float64)[..., :3]
        want = None
        for tile in (32, 16):
            path = f"{d}/eye{ss}_t{tile}_L.bin"
            if not os.path.exists(path):
                continue
            if want is None:
                want = enlarge(per, 4 * ew, 4 * eh)
                # (how much of the enlargement is more than a copy: pixels that differ from the nearest pixel of the periphery)
                near = per[np.clip(((np.arange(4 * eh) + 0.5) * ss / 4.0).astype(np.int64), 0, ss * eh - 1)][:, np.clip(((np.arange(4 * ew) + 0.5) * ss / 4.0).astype(np.int64), 0, ss * ew - 1)]
                moved = float((np.abs(want - near).max(axis=-1) > 0.01).mean())
            got = np.fromfile(path, dtype="<f4").reshape(4 * eh, 4 * ew, 4).astype(np.float64)[..., :3]
            diff = np.abs(got - want)
            off = int((diff.max(axis=-1) > 0.002).sum())
            ok = off == 0
            bad += 0 if ok else 1
            eye_runs += 1
            # the pass keeps the periphery's taps of a group in shared memory while they fit (16 per axis), else it reads
            # them from the texture: 12 per 34 eye pixels (groups of 32) or per 18 (groups of 16)
            fits = (ss * ew) * (tile + 2) <= (4 * ew) * 12
            print(f"eye pass, periphery {ss * ew}x{ss * eh} -> eye image {4 * ew}x{4 * eh} (x{4 // ss}), groups of {tile}x{tile} (taps "
                  f"{'in shared memory' if fits else 'from the texture'}): largest difference from the formula {diff.max():.2g}, pixels off by "
                  f"more than 0.002: {off} of {diff.shape[0] * diff.shape[1]} ({100.0 * moved:.0f} % of the pixels are more than a copy of the "
                  f"nearest) -> {'ok' if ok else 'WRONG'}")
    if eye_runs == 0:
        print("eye pass: not run (no shader text given to the test)")
    print("periphery 2 x 2 per ray: " + ("all ok" if bad == 0 else "FAILED"))
    return bad


if __name__ == "__main__":
    sys.exit(main())
