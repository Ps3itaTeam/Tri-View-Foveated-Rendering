"""[fix94] A sequence for gpu/periphery_test.exe uisteady: a scene moving `speed` pixels a frame and HUD pieces that come and
go (one from the start, one for frames 20 - 25, a dark one in its place from 56, a thin line from 34).
   make.py <dir> <speed> <frames> key=value ...   (the keys go into steady.txt as they are: around, around_hold, ...)"""
import os, sys
import numpy as np
W, H = 256, 192
def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b; r += f * (i % b); i //= b
    return r
def scene(x, y):
    return 0.35 + 0.2 * np.sin(x * 0.21) * np.cos(y * 0.17) + 0.15 * np.sin(x * 0.045 + y * 0.06) + 0.1 * (np.sin(x * 1.3 + y * 0.9) > 0.6)
# (x0, y0, x1, y1, first frame, frame after the last, brightness, stripe period)
PIECES = [(40, 30, 76, 46, None, None, 0.95, 3.0), (180, 120, 206, 140, 20, 26, 0.9, 2.0), (182, 122, 204, 138, 56, 999, 0.05, 5.0), (120, 80, 123, 150, 34, 999, 0.8, 1.0)]
def main():
    d, speed, n = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
    keys = dict(a.split("=") for a in sys.argv[4:])
    first0 = int(keys.pop("hud_from", "0"))
    os.makedirs(d, exist_ok=True)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    with open(os.path.join(d, "steady.txt"), "w") as f:
        f.write("w %d\nh %d\nframes %d\nmv_scale_x %.9g\nmv_scale_y %.9g\nsrgb 0\nmap 1\n" % (W, H, n, 1.0 / W, 1.0 / H))
        for k_, v in keys.items():
            f.write("%s %s\n" % (k_, v))
        for k in range(n):
            jx, jy = halton(k % 32 + 1, 2) - 0.5, halton(k % 32 + 1, 3) - 0.5
            f.write("jitter %d %.9g %.9g\n" % (k, jx, jy))
            # pixel q's sample shows the point q + 0.5 + jitter... of the steady picture: p + jitter in the frame = q + 0.5
            px, py = xx + 0.5 - jx, yy + 0.5 - jy
            hud = scene(px - speed * k, py - 0.5 * speed * k)
            fin = hud.copy()
            for (x0, y0, x1, y1, a, b, br, per) in PIECES:
                a = first0 if a is None else a
                b = 999 if b is None else b
                if a <= k < b:
                    m = (px >= x0) & (px < x1) & (py >= y0) & (py < y1)
                    pat = br * (0.6 + 0.4 * (np.floor((px + py) / per) % 2))
                    fin = np.where(m, 0.3 * fin + 0.7 * pat, fin)
            col = np.clip(fin, 0, 1) ** (1 / 2.2)
            four = lambda a_: np.stack([a_, a_ * 0.9, a_ * 0.8, np.ones_like(a_)], -1).astype("<f4")
            four(col).tofile(os.path.join(d, "c%03d.bin" % k))
            four(hud).tofile(os.path.join(d, "h%03d.bin" % k))
            four(fin).tofile(os.path.join(d, "f%03d.bin" % k))
            np.stack([np.full((H, W), -speed), np.full((H, W), -0.5 * speed)], -1).astype("<f4").tofile(os.path.join(d, "m%03d.bin" % k))
main()
