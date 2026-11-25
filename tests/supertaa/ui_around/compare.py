"""[fix94] compare.py <case dir A> <case dir B> [exact | marked]: the results of two runs of periphery_test.exe uisteady on
the same sequence (make.py), frame by frame.
   exact: every file the same bytes (the pass before a change against the pass after it; the whole image both times).
   marked: A the whole image, B only around the interface (around 1): in the blocks marked in B and one block around,
   the largest difference and the texels that are not equal to the bit; the texels far from every mark so far that
   are not empty in B (0 from the third frame on); B's texels outside those blocks where the results without / with GUI
   differ at all."""
import os, sys
import numpy as np
W, H, SS = 256, 192, 2
BW, BH = W // 8, H // 8
a, b = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "exact"
n = len([f for f in os.listdir(a) if f.startswith("sc")])
if mode == "exact":
    bad = 0
    for f in sorted(os.listdir(a)):
        if f.endswith(".bin") and open(os.path.join(a, f), "rb").read() != open(os.path.join(b, f), "rb").read():
            bad += 1
            if bad < 5: print("differs:", f)
    print("%s vs %s: %d frames, %d files differ" % (sys.argv[1], sys.argv[2], n, bad))
    sys.exit(1 if bad else 0)
def img(d, n_, k):
    return np.fromfile(os.path.join(d, "s%s%03d.bin" % (n_, k)), "<f2").reshape(H * SS, W * SS, 4).astype(np.float64)
def marks(d, k, n_="gs"):
    return np.fromfile(os.path.join(d, "%s%03d.bin" % (n_, k)), "<u4").reshape(-1, BW)[:BH] == k + 1
def grow(m, r):
    o = np.zeros_like(m)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            s = np.zeros_like(m)
            ys, xs = slice(max(dy, 0), BH + min(dy, 0)), slice(max(dx, 0), BW + min(dx, 0))
            yd, xd = slice(max(-dy, 0), BH + min(-dy, 0)), slice(max(-dx, 0), BW + min(-dx, 0))
            s[yd, xd] = m[ys, xs]
            o |= s
    return o
def texels(m):
    return np.repeat(np.repeat(m, 8 * SS, 0), 8 * SS, 1)
ever = np.zeros((BH, BW), bool)
print("frame: blocks marked A / B | in B's marked blocks +1: max |A - B| colour, hud, fin, weight; texels not bitwise equal | far from every mark so far: nonzero texels in B | B: hud != fin outside marked +1")
for k in range(n):
    ma, mb = marks(a, k), marks(b, k)
    ever |= mb | marks(b, k, "gm")
    near = texels(grow(mb, 1))
    far = texels(~grow(ever, 4))
    row = []
    neq = 0
    for n_ in "chf":
        A, B = img(a, n_, k), img(b, n_, k)
        d = np.abs(A - B)[near]
        row.append(d[..., :3].max() if d.size else 0.0)
        if n_ == "c":
            wdiff = d[..., 3].max() if d.size else 0.0
        neq += int((A != B)[near].any(-1).sum()) if d.size else 0
        if n_ == "c":
            nz = int((B[far] != 0).any(-1).sum())
    hb, fb = img(b, "h", k), img(b, "f", k)
    out = int(((hb[..., :3] != fb[..., :3]).any(-1) & ~near).sum())
    if k < 8 or k % 4 == 0 or k in (20, 21, 22, 26, 27, 34, 35, 56, 57) or k == n - 1:
        print("%3d: %3d / %3d | %.5f %.5f %.5f w %.3f; %6d | %6d | %6d" % (k, ma.sum(), mb.sum(), row[0], row[1], row[2], wdiff, neq, nz, out))
