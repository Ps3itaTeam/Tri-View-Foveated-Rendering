"""[fix85 18.11.2026] The reprojection of the periphery on whole-chain dumps from the game (debug tools: "Dumps: the
periphery's chain whole, as stored" and "Dump 2 frames": two frames in a row, each with the wide view's supersampled image
C_3t, the stable depth's image C_2s as the passes hold it, and the game's own left periphery L_3r).

   python3 dump_pair.py <work dir> <dump folder> [<dump folder> ...] [--soft=1] [--disparity=-8.96962929]
                        [--x_scale=0.999997914] [--check]

Each frame's C_3t and C_2s go through d3d12::MonoPeriphery of the test build (gpu/periphery_test.exe seq with "given 1":
the stable depth is read, not computed), which gives the left eye's periphery of that build for the very frames the game
drew. --check prints how far that is from the game's own L_3r (with the game's build: 0.00005 mean, the run's check).
Measured from frame 0 to frame 1, on tone-mapped luma, each block of 32 x 32 brought to its place first (phase
correlation; what is left is what changed, not what moved):
  - the mean change x1000 in blocks near a step of the stable depth (15 %, grown by the disparity) and elsewhere, and the
    same for the wide view's own image (what the reprojection adds);
  - the pixels that change by more than 3 / 6 / 12 % of the range: with the head moving a tenth of a pixel these are what
    sparkles along the outlines of near things;
  - isolated pixels (far from the median of their 3 x 3 and 5 x 5) of a frame.
RE7, seven dumps of 18.11.2026 16:5x (hands in view, the head moving 0.07 - 0.93 wide pixels per frame), means of the
seven: change near steps, elsewhere | pixels over 3 / 6 / 12 % | isolated pixels
  second pass only (--soft=0, fix83's way)  1.390, 0.626 | 2261 / 707 / 192 | 801
  fix84                                     1.257, 0.606 | 1962 / 693 / 204 | 407   (0.00003 from the game's own image)
  fix85                                     1.138, 0.607 | 1366 / 362 /  83 | 320
  the wide view's own image                 0.881, 0.626 |  267 /  40 /   1
What the reprojection adds near depth steps fell from 0.38 to 0.26, the pixels that change by more than 6 % by half, by
more than 12 % to two fifths; what is left is five to nine times what the wide view's image itself shows there.
needs gpu/periphery_test.exe (gpu/build.sh) and a D3D12 to run it (see run_cases.sh: RUN, default "xvfb-run -a wine";
GPU_DIR: where the test program is)."""
import glob
import os
import shutil
import subprocess
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
GPU = os.environ.get("GPU_DIR", os.path.join(os.path.dirname(HERE), "gpu"))
RUN = os.environ.get("RUN", "xvfb-run -a wine").split()
ENV = dict(os.environ, WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), WINEDLLOVERRIDES=os.environ.get("WINEDLLOVERRIDES", "d3dcompiler_47=n;mscoree,mshtml="))
LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)
WW, WH, EW, EH = 1120, 864, 848, 864  # the wide view, the rays of an eye's periphery (RE7, PSVR2, 18.11.2026)


def ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = [int(v) for v in f.readline().split()]
        mx = int(f.readline())
        a = np.frombuffer(f.read(), dtype=">u2" if mx > 255 else np.uint8).reshape(h, w, 3)
    return a.astype(np.float32) / mx


def one(folder, pat):
    g = glob.glob(os.path.join(folder, pat))
    if not g:
        raise SystemExit("not in %s: %s (the dump needs \"Dumps: the periphery's chain whole\" on)" % (folder, pat))
    return g[0]


def taa(folder):
    return ppm(one(folder, "C_3t_wide_supersampling_taa_output_full_*"))


def stable(folder):
    return np.fromfile(one(folder, "C_2s_wide_stable_depth_raw_*"), "<f2").reshape(WH, WW, 4).astype(np.float32)


def tone(e):
    v = e.astype(np.float32) @ LUM
    return v / (1.0 + v)


def reproject(folder, work, soft, disparity, x_scale):
    os.makedirs(work, exist_ok=True)
    c = taa(folder)
    np.concatenate([c, np.ones(c.shape[:2] + (1,), np.float32)], -1).astype("<f4").tofile(os.path.join(work, "c000.bin"))
    shutil.copy(one(folder, "C_2s_wide_stable_depth_raw_*"), os.path.join(work, "g000.bin"))
    with open(os.path.join(work, "seq.txt"), "w") as f:
        f.write("w %d\nh %d\new %d\neh %d\nss 2\nframes 1\nx_scale %.9g\nx_offset 0\ny_scale 1\ny_offset 0\ndisparity %.9g\ndist_min 0.12\nmax_shift 96\n"
                "fill 1\nstable 1\ngiven 1\nsoft %d\n" % (WW, WH, EW, EH, x_scale, disparity, soft))
    d = work if RUN[-1] != "wine" and "wine" not in " ".join(RUN) else "Z:" + os.path.abspath(work).replace("/", "\\")
    r = subprocess.run(RUN + [os.path.join(GPU, "periphery_test.exe"), "seq", d], cwd=GPU, env=ENV, capture_output=True, text=True, timeout=1800)
    if "done:" not in r.stdout:
        raise SystemExit("the GPU run failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    e = np.fromfile(os.path.join(work, "e000.bin"), "<f4").reshape(2 * EH, 2 * EW, 4)[..., :3].copy()
    for n in ("c000.bin", "g000.bin", "e000.bin"):
        os.remove(os.path.join(work, n))
    return e


def near_steps(folder, grow=10):
    """the left periphery's pixels near a step of the stable depth's 1 / distance by more than 15 %"""
    inv = stable(folder)[..., 0]
    m = np.zeros(inv.shape, bool)
    r = np.abs(inv[:, 1:] - inv[:, :-1]) / np.maximum(np.minimum(inv[:, 1:], inv[:, :-1]), 1e-6)
    m[:, 1:] |= r > 0.15
    m[:, :-1] |= r > 0.15
    r = np.abs(inv[1:] - inv[:-1]) / np.maximum(np.minimum(inv[1:], inv[:-1]), 1e-6)
    m[1:] |= r > 0.15
    m[:-1] |= r > 0.15
    # (the left eye sees a thing of the wide column x further right by its disparity: grown that way as well)
    sh = np.clip(9.0 * ndimage.maximum_filter(inv, 5), 0, 40)
    out = ndimage.binary_dilation(m, iterations=grow)
    for k in (8, 16, 24, 32):
        out[:, k:] |= ndimage.binary_dilation(m & (sh >= k - 4), iterations=grow)[:, :-k]
    return np.kron(out[:, :EW], np.ones((2, 2), bool))


def brought(a, b, n=32):
    """|b - a|, each block of a moved to its place in b first (phase correlation, to a part of a pixel)"""
    H, W = a.shape
    win = np.outer(np.hanning(n), np.hanning(n)).astype(np.float32)
    pad = 8
    ap = np.pad(a, pad, mode="edge")
    out = np.zeros(a.shape, np.float32)
    for j in range(H // n):
        for i in range(W // n):
            p = a[j * n:(j + 1) * n, i * n:(i + 1) * n]
            q = b[j * n:(j + 1) * n, i * n:(i + 1) * n]
            d0 = np.abs(q - p)
            best = d0
            if p.std() >= 0.004:
                R = np.fft.fft2((p - p.mean()) * win).conj() * np.fft.fft2((q - q.mean()) * win)
                R /= np.abs(R) + 1e-9
                r = np.fft.ifft2(R).real
                py, px = np.unravel_index(np.argmax(r), r.shape)

                def sub(v, k):
                    lo, c, hi = v[(k - 1) % n], v[k], v[(k + 1) % n]
                    den = lo - 2 * c + hi
                    return 0.0 if abs(den) < 1e-9 else 0.5 * (lo - hi) / den

                sx = px + sub(r[py], px)
                sy = py + sub(r[:, px], py)
                sx = sx - n if sx > n / 2 else sx
                sy = sy - n if sy > n / 2 else sy
                if abs(sx) <= 6 and abs(sy) <= 6:
                    big = ap[j * n:(j + 1) * n + 2 * pad, i * n:(i + 1) * n + 2 * pad]
                    d1 = np.abs(q - ndimage.shift(big, (sy, sx), order=3, mode="nearest")[pad:-pad, pad:-pad])
                    if d1.mean() < d0.mean():
                        best = d1
            out[j * n:(j + 1) * n, i * n:(i + 1) * n] = best
    out[:64] = 0
    out[-64:] = 0
    out[:, :64] = 0
    out[:, -64:] = 0
    return out


def isolated(e, thr=0.06):
    v = tone(e)
    return int(((np.abs(v - ndimage.median_filter(v, size=3)) > thr) & (np.abs(v - ndimage.median_filter(v, size=5)) > thr)).sum())


def measures(a, b, mask, n=32):
    r = brought(tone(a), tone(b), n)
    by, bx = r.shape[0] // n, r.shape[1] // n
    blk = r[:by * n, :bx * n].reshape(by, n, bx, n).mean((1, 3))
    frac = mask[:by * n, :bx * n].reshape(by, n, bx, n).mean((1, 3))
    inner = np.zeros(blk.shape, bool)
    inner[2:-2, 2:-2] = True
    return (1000 * float(blk[inner & (frac > 0.25)].mean()), 1000 * float(blk[inner & (frac == 0)].mean()),
            int((r > 0.03).sum()), int((r > 0.06).sum()), int((r > 0.12).sum()))


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    if len(args) < 2:
        raise SystemExit(__doc__)
    work = args[0]
    soft = int(opt.get("soft", "1"))
    disparity = float(opt.get("disparity", "-8.96962929"))
    x_scale = float(opt.get("x_scale", "0.999997914"))
    tot = []
    print("soft %d | change x1000 near depth steps, elsewhere (the wide view's own image) | pixels over 3 / 6 / 12 %% (the wide view's) | isolated pixels" % soft)
    for d in args[1:]:
        F = [os.path.join(d, "frame0_both_eyes"), os.path.join(d, "frame1_both_eyes")]
        e = [reproject(f, work, soft, disparity, x_scale) for f in F]
        mask = near_steps(F[1])
        m = measures(e[0], e[1], mask)
        t = [taa(f)[:, :2 * EW] for f in F]
        mt = measures(t[0], t[1], mask)
        iso = (isolated(e[0]) + isolated(e[1])) / 2
        line = "%s: %.3f, %.3f (%.3f, %.3f) | %d / %d / %d (%d / %d / %d) | %d" % (os.path.basename(d.rstrip("/")), m[0], m[1], mt[0], mt[1], m[2], m[3], m[4], mt[2], mt[3], mt[4], iso)
        if "check" in opt:
            g = ppm(one(F[1], "L_3r_periphery_reprojected_to_this_eye_full_*"))
            line += " | from the game's own periphery: mean %.5f" % float(np.abs(e[1] - g).mean())
        print(line, flush=True)
        tot.append(m + mt + (iso,))
    if len(tot) > 1:
        t = np.mean(np.array(tot, np.float64), axis=0)
        print("MEAN: %.3f, %.3f (%.3f, %.3f) | %.0f / %.0f / %.0f (%.0f / %.0f / %.0f) | %.0f" % (t[0], t[1], t[5], t[6], t[2], t[3], t[4], t[7], t[8], t[9], t[10]))


if __name__ == "__main__":
    main(sys.argv[1:])
