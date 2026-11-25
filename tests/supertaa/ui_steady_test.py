"""[fix86 18.11.2026] d3d12::UiSteady (the wide view's interface for the periphery collected over the frames) on a
synthetic menu with the answer known, through the pass itself (gpu/periphery_test.exe uisteady).

   python3 ui_steady_test.py <work dir> [--speed=0,0.1,0.6,3] [--frames=48] [--weight_max=4] [--sigma=0.3] [--srgb]
                             [--sheet=out.png] [--mv_speed=v] [--ui_motion]

The picture: a scene (soft texture, some edges) and over it a menu a degree and a half aslant as the save menu of RE7
stands in the periphery: a translucent dark backdrop, a white bar with dark writing, rows parted by lines a quarter of a
pixel wide, light writing (strokes 0.3 pixels wide). The wide view samples it once per pixel, each frame at another
place (the jitter: Halton 2 / 3 of 32, as the game's); everything moves `speed` pixels per frame (a turn of the head: the
panel moves like the scene, and the motion vectors say so). Given to the pass as the game gives it: the colour after
the output pass (here the value ^ (1 / 2.2); --srgb: as an 8 bit sRGB image) and the copies without / with GUI.
Compared, inside the menu, for the colour and for the copy with GUI:
  the frame   what the eye pass read up to fix85: the frame's image where the jitter put the point (bilinear)
  steady      the pass's result
against the picture itself seen through a pixel-sized window (Gaussian, sigma 0.3 pixels), on a grid of twice the size:
  error       mean |image - picture|
  change      mean |image - the image a frame before, moved by the motion| (what the eye sees as shimmer)
and: how often the blocks of a thin line with no backdrop are marked in GuiMap's map (the eye pass looks for interface
only in marked blocks); the texels far from the menu where the results without / with GUI differ by more than 0.001 (must be 0: the eye
pass's masks rest on it; they begin at 0.03); how many frames after the writing changes the old one is down to a tenth.
The game's values (weight_max 4, sigma 0.3), 48 frames, the last 16 measured, colour (error, change x1000), the frame |
steady: speed 0: 31.0 44.4 | 6.0 4.2; 0.1: 31.0 44.8 | 10.7 4.5; 0.6: 31.0 41.1 | 15.3 4.9; 3: 31.2 32.7 | 16.7 15.4;
0 texels differ; the lone line marked in 25 % / 100 % of the frames; a changed writing under a tenth after 18 frames
(weight_max 6: change 2.9, more than 20 frames; 3: change 5.4, 14 frames).
needs gpu/periphery_test.exe (gpu/build.sh) and a D3D12 to run it (RUN, default "xvfb-run -a wine"; GPU_DIR)."""
import os
import subprocess
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
GPU = os.environ.get("GPU_DIR", os.path.join(HERE, "gpu"))
RUN = os.environ.get("RUN", "xvfb-run -a wine").split()
ENV = dict(os.environ, WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), WINEDLLOVERRIDES=os.environ.get("WINEDLLOVERRIDES", "d3dcompiler_47=n;mscoree,mshtml="))
W, H = 160, 120
SS = 2          # the results' size over the images' (d3d12::UiSteady::SCALE)
FINE = 8        # points per result texel for the picture itself


def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def jitter(k):
    return halton(k % 32 + 1, 2) - 0.5, halton(k % 32 + 1, 3) - 0.5


def hash2(a, b):
    v = np.sin(a * 12.9898 + b * 78.233) * 43758.5453
    return v - np.floor(v)


def picture(x, y, writing=1.0):
    """the scene, and the menu's share let through and light added, at points (pixels of the wide view, any place)"""
    scene = 0.22 + 0.10 * np.sin(0.31 * x + 0.2 * y) * np.cos(0.17 * y) + 0.08 * (np.floor(x / 9.0) % 2) * (np.floor(y / 7.0) % 2)
    # the menu's own axes (u along the rows, v across them), a degree and a half aslant
    c, s = np.cos(np.radians(1.5)), np.sin(np.radians(1.5))
    u = (x - 80.0) * c + (y - 60.0) * s
    v = -(x - 80.0) * s + (y - 60.0) * c
    inside = (np.abs(u) < 52.0) & (np.abs(v) < 40.0)
    tr = np.where(inside, 0.25, 1.0)
    add = np.zeros_like(x)
    field = (np.abs(u) < 46.0) & (v > -30.0) & (v < 34.0)
    # the rows' lines: a quarter of a pixel wide, 6.5 pixels apart
    rows = np.abs((v + 30.0) % 6.5 - 3.25) > 3.25 - 0.125
    add = np.where(field & rows, 0.5, add)
    # the frame of the field: 0.3 pixels
    edge = (np.abs(np.abs(u) - 46.0) < 0.15) & (v > -30.0) & (v < 34.0)
    add = np.where(edge, 0.7, add)
    # writing: strokes 0.3 pixels wide in cells of 1.6 x 3.25 pixels, light in the rows, dark in the bar
    cu, cv = np.floor(u / 1.6), np.floor((v + 30.0) / 3.25)
    fu, fv = u / 1.6 - cu, (v + 30.0) / 3.25 - cv
    on = (hash2(cu, cv) > 0.45) & (np.abs(u) < 40.0) & (cv % 2 == 0)
    stroke = on & (((np.abs(fu - 0.5) < 0.3 / 1.6 / 2) & (fv > 0.15) & (fv < 0.85)) | ((np.abs(fv - 0.5) < 0.3 / 3.25 / 2) & (hash2(cv, cu) > 0.5)))
    bar = field & (v > -30.0) & (v < -23.5)
    add = np.where(bar, 0.9, add)
    add = np.where(stroke & field & ~bar & (cv >= 2) & (cv < 6), 0.8 * writing, add)
    add = np.where(stroke & bar, 0.9 * (1.0 - writing) + 0.05, add)
    # a line of its own, a quarter of a pixel wide and level, with no backdrop: under the menu on the left
    alone = (np.abs(y - 108.37) < 0.125) & (x > 20.0) & (x < 60.0)
    add = np.where(alone, 0.6, add)
    hud = scene
    fin = scene * tr + add
    return hud, fin, inside, alone | inside


def frames_of(speed, n, change_at=None):
    """per frame: the three images as the wide view samples them, the motion, the jitter; the picture's place"""
    out = []
    for k in range(n):
        jx, jy = jitter(k)
        off = speed * (k - (n - 1))   # the picture's place (pixels along x, half as much along y): in the middle at the end
        ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
        px = xs + 0.5 - jx - off
        py = ys + 0.5 - jy - 0.5 * off
        hud, fin, _, _ = picture(px, py, 0.0 if change_at is not None and k >= change_at else 1.0)
        out.append(dict(hud=hud, fin=fin, col=np.clip(fin, 0, 1) ** (1 / 2.2), jit=(jx, jy), off=(off, 0.5 * off)))
    return out


def truth(off, sigma=0.3, writing=1.0):
    """the picture through a pixel-sized window, at the results' texel middles: colour and the copy with GUI"""
    n = SS * FINE
    ys, xs = (np.mgrid[0:H * n, 0:W * n].astype(np.float64) + 0.5) / n
    hud, fin, inside, drawn = picture(xs - off[0], ys - off[1], writing)
    col = np.clip(fin, 0, 1) ** (1 / 2.2)
    res = []
    for img in (col, fin, inside.astype(np.float64), drawn.astype(np.float64)):
        b = ndimage.gaussian_filter(img, sigma * n, mode="nearest")
        res.append(b.reshape(H * SS, FINE, W * SS, FINE).mean((1, 3)))
    return res


def frame_read(img, jit):
    """the frame's image where the jitter put the point, at the results' texel middles (bilinear, as the eye pass reads)"""
    ys, xs = (np.mgrid[0:H * SS, 0:W * SS].astype(np.float64) + 0.5) / SS
    return ndimage.map_coordinates(img, [ys + jit[1] - 0.5, xs + jit[0] - 0.5], order=1, mode="nearest")


def run_pass(work, fr, speed, opt):
    os.makedirs(work, exist_ok=True)
    srgb = "srgb" in opt
    with open(os.path.join(work, "steady.txt"), "w") as f:
        f.write("w %d\nh %d\nframes %d\nmv_scale_x %.9g\nmv_scale_y %.9g\nsrgb %d\nmap 1\n" % (W, H, len(fr), 1.0 / W, 1.0 / H, 1 if srgb else 0))
        for key in ("weight_max", "sigma", "motion_half"):
            if key in opt:
                f.write("%s %s\n" % (key, opt[key]))
        if "ui_motion" in opt:
            f.write("ui_motion 1\n")
        for k, d in enumerate(fr):
            f.write("jitter %d %.9g %.9g\n" % (k, d["jit"][0], d["jit"][1]))
    for k, d in enumerate(fr):
        def four(a):
            return np.stack([a, a, a, np.ones_like(a)], -1)
        if srgb:
            # (the sRGB image holds the encoded values; the game's colour is the output pass's result, taken as such)
            (np.clip(four(d["col"]), 0, 1) * 255 + 0.5).astype(np.uint8).tofile(os.path.join(work, "c%03d.bin" % k))
        else:
            four(d["col"]).astype("<f4").tofile(os.path.join(work, "c%03d.bin" % k))
        four(d["hud"]).astype("<f4").tofile(os.path.join(work, "h%03d.bin" % k))
        four(d["fin"]).astype("<f4").tofile(os.path.join(work, "f%03d.bin" % k))
        # the point now at x was at x - speed a frame before
        # [UI MOTION fix125] --mv_speed=v: the motion vectors hold another motion than the picture's (the scene behind an
        # interface that moves its own way: v pixels a frame); --ui_motion: the pass also gets the interface's own motion
        # (the picture's: the panels' rows of d3d12::UiMotion::Constants, 68 rows of four floats)
        mvs = float(opt["mv_speed"]) if "mv_speed" in opt else speed
        np.stack([np.full((H, W), -mvs), np.full((H, W), -0.5 * mvs)], -1).astype("<f4").tofile(os.path.join(work, "m%03d.bin" % k))
        if "ui_motion" in opt:
            um = np.zeros((68, 4), "<f4")
            um[0] = (0.0, 0.0, -speed / W, 1.0)
            um[1] = (0.0, 0.0, -0.5 * speed / H, 0.0)
            um.tofile(os.path.join(work, "um%03d.bin" % k))
    wd = "Z:" + os.path.abspath(work).replace("/", "\\") if "wine" in " ".join(RUN) else work
    r = subprocess.run(RUN + [os.path.join(GPU, "periphery_test.exe"), "uisteady", wd], cwd=GPU, env=ENV, capture_output=True, text=True, timeout=3600)
    if "done:" not in r.stdout:
        raise SystemExit("the GPU run failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    res = []
    for k in range(len(fr)):
        d = {n: np.fromfile(os.path.join(work, "s%s%03d.bin" % (n, k)), "<f2").reshape(H * SS, W * SS, 4).astype(np.float64) for n in "chf"}
        # the map's blocks marked in this frame (stamp k + 1): by GuiMap alone, and after the pass
        for n in ("gm", "gs"):
            m = np.fromfile(os.path.join(work, "%s%03d.bin" % (n, k)), "<u4").reshape(-1, (W + 7) // 8)[:(H + 7) // 8]
            d[n] = m == k + 1
        res.append(d)
    for n in os.listdir(work):
        if n.endswith(".bin"):
            os.remove(os.path.join(work, n))
    return res


def moved(img, d):
    return ndimage.shift(img, (d[1] * SS, d[0] * SS), order=3, mode="nearest")


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    if len(args) < 1:
        raise SystemExit(__doc__)
    work = args[0]
    n = int(opt.get("frames", "48"))
    speeds = [float(v) for v in opt.get("speed", "0,0.1,0.6,3").split(",")]
    last = 16
    sheet = []
    print("in the menu, the last %d of %d frames: error and change from frame to frame (x1000), colour | copy with GUI" % (last, n))
    print("%-6s | %-28s | %-28s | %s" % ("speed", "the frame (to fix85)", "steady", "without / with GUI differ far from the menu"))
    for speed in speeds:
        fr = frames_of(speed, n)
        res = run_pass(os.path.join(work, "run"), fr, speed, opt)
        err = {"frame": [[], []], "steady": [[], []]}
        chg = {"frame": [[], []], "steady": [[], []]}
        differ = 0
        prev = None
        for k in range(n - last, n):
            tc, tf, tin, tdrawn = truth(fr[k]["off"], float(opt.get("sigma", "0.3")))
            menu = tin > 0.5
            cur = {"frame": (frame_read(fr[k]["col"], fr[k]["jit"]), frame_read(fr[k]["fin"], fr[k]["jit"])),
                   "steady": (res[k]["c"][..., 0], res[k]["f"][..., 0])}
            for name in cur:
                for j, t in enumerate((tc, tf)):
                    err[name][j].append(np.abs(cur[name][j] - t)[menu].mean())
                    if prev is not None:
                        d = (fr[k]["off"][0] - fr[k - 1]["off"][0], fr[k]["off"][1] - fr[k - 1]["off"][1])
                        chg[name][j].append(np.abs(cur[name][j] - moved(prev[name][j], d))[menu].mean())
            prev = cur
            far = ndimage.binary_dilation(tdrawn > 0.001, iterations=6 * SS)
            differ += int(((np.abs(res[k]["h"][..., :3] - res[k]["f"][..., :3]) > 1e-3).any(-1) & ~far).sum())
            if k == n - 1:
                g = np.concatenate([np.clip(cur["frame"][0], 0, 1), np.clip(cur["steady"][0], 0, 1), np.clip(tc, 0, 1)], 1)
                sheet.append(g)

        def cell(name):
            return "%5.1f %5.1f | %5.1f %5.1f" % (1000 * np.mean(err[name][0]), 1000 * np.mean(chg[name][0]), 1000 * np.mean(err[name][1]), 1000 * np.mean(chg[name][1]))
        print("%-6g | %-28s | %-28s | %d texels" % (speed, cell("frame"), cell("steady"), differ), flush=True)
    # the writing changes at frame 24 (head still): how long the old one shows
    fr = frames_of(0.0, 44, change_at=24)
    res = run_pass(os.path.join(work, "run"), fr, 0.0, opt)
    # (the head still: the lone line's blocks in the map, frame by frame)
    line_blocks = [(108 // 8, bx) for bx in range(24 // 8, 56 // 8 + 1)]
    alone_gm = np.mean([all(res[k]["gm"][b] for b in line_blocks) for k in range(8, 24)])
    alone_gs = np.mean([all(res[k]["gs"][b] for b in line_blocks) for k in range(8, 24)])
    print("a level line of a quarter pixel with no backdrop: all its blocks are marked in %.0f %% of the frames by GuiMap alone, in %.0f %% with the pass" % (
        100 * alone_gm, 100 * alone_gs))
    t0 = truth((0.0, 0.0), float(opt.get("sigma", "0.3")), 1.0)[0]
    t1 = truth((0.0, 0.0), float(opt.get("sigma", "0.3")), 0.0)[0]
    where = np.abs(t0 - t1) > 0.2
    left = [float((np.abs(res[k]["c"][..., 0] - t1)[where]).mean() / np.abs(t0 - t1)[where].mean()) for k in range(23, 44)]
    tenth = next((i for i, v in enumerate(left) if v < 0.1), None)
    print("the writing changes: of the old one is left after 1 / 2 / 4 / 8 / 12 frames %.2f / %.2f / %.2f / %.2f / %.2f; under a tenth after %s frames" % (
        left[1], left[2], left[4], left[8], left[12], "more than 20" if tenth is None else str(tenth)))
    if "sheet" in opt and sheet:
        from PIL import Image
        img = np.concatenate(sheet, 0)
        Image.fromarray((img * 255).astype(np.uint8)).resize((img.shape[1] * 2, img.shape[0] * 2), Image.NEAREST).save(opt["sheet"])
        print("sheet (per speed: the frame as read to fix85 | steady | the picture itself):", opt["sheet"])


if __name__ == "__main__":
    main(sys.argv[1:])
