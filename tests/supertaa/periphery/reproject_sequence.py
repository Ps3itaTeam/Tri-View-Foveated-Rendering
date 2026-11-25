"""[fix81 17.11.2026] The periphery's chain on a capture from the game WITH the reprojection the game runs every frame
(d3d12::MonoPeriphery on the GPU test's device: gpu/periphery_test.exe seq), to see what the reprojection adds to the
flicker and what the stable depth (d3d12::StableDepth) takes away again.

   python3 reproject_sequence.py <capture folder> <work dir> <variants: none,raw,centre,model,game> [--frames=N] [--disp=10.4]
                                 [--warm=24] [--sheet=png] [--diff=png]

  the wide view made from the fovea view's frames (pcap.wide_sequence: every 4th sample per axis, where the wide view's
  jitter puts it; colour, motion, distance)
  -> the supersampling TAA, image of twice the wide view's size (periphery_aa 2)
  -> reprojected to the left eye with a depth image (the variants) and 2 x 2 pixels per ray
  -> enlarged to the eye image (Catmull-Rom kept within the inner 2 x 2)
Variants (what the reprojection is given as depth):
  none    no reprojection at all (the TAA's image enlarged: what fix80's measures were made on)
  raw     the frame's own depth, jittered with the frame (fix80)
  centre  the depth at the wide pixel's centre (from the fovea's frame without the wide view's jitter): a depth without jitter
  model   the reference of the stable depth (stable_depth.py)
  game    the game's pass (d3d12::StableDepth before the reprojection, as fix81 runs it), and its images against the model's
  soft    [fix84] as game, with each of a ray's four pixels on its own surface and the outlines from the stable depth's mean
          (MonoPeriphery::Params::soft_outlines)
Measures (eye pixels): flicker = mean |image - the image of the frame before brought along the motion|, low-passed with
sigma 1 and 3 pixels; within 24 eye pixels of a step of the wide view's distance by more than 15 %, and elsewhere.
RE7 captures of 17.11.2026 (kitchen, 48 / 64 frames), flicker x1000 near depth steps (sigma 1 / 3) and elsewhere:
  capture 01: none 0.99 / 0.50, 0.66 / 0.30; raw 2.83 / 1.90, 0.76 / 0.43; centre 1.63 / 0.96, 0.65 / 0.33; game 1.28 / 0.75, 0.59 / 0.29
  capture 02: none 0.90 / 0.45, 0.67 / 0.34; raw 3.01 / 1.97, 0.70 / 0.42; centre 2.10 / 1.19, 0.66 / 0.37; game 2.07 / 1.20, 0.60 / 0.33
needs gpu/periphery_test.exe (gpu/build.sh) and a D3D12 to run it (see run_cases.sh: RUN, default "xvfb-run -a wine")."""
import os
import subprocess
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (also when Python runs isolated, -I)
import pcap  # noqa: E402
import pchain  # noqa: E402
import stable_depth as SD  # noqa: E402

sys.path.insert(0, pchain.TOOLS)
import analyze_capture as AC  # noqa: E402
import capture_io as CIO  # noqa: E402
import resolvers as R  # noqa: E402

K = 4
GPU = os.environ.get("GPU_DIR", os.path.join(pchain.TOOLS, "gpu"))
RUN = os.environ.get("RUN", "xvfb-run -a wine").split()
ENV = dict(os.environ, WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), WINEDLLOVERRIDES=os.environ.get("WINEDLLOVERRIDES", "d3dcompiler_47=n;mscoree,mshtml="))


def centre_dist(cap):
    """the distance at the wide pixels' centres, from the fovea's frames (their own jitter left: +-1/8 of a wide pixel)"""
    out = []
    for t in range(cap.count):
        d = R.NewTAA.distance_of(cap.depth(t), cap.depth_k)
        out.append(d[2::K, 2::K].astype(np.float32))
    return np.stack(out)


def reproject(work, name, color2x, dist, disp, fill=1, mv=None, state=None, soft=False, extra=None):
    """runs the game's pass over the frames: color2x [F, 2h, 2w, 3] encoded, dist [F, h, w] metres -> [F, 2h, 2w, 4].
    mv [F, h, w, 2] given: the distances go through the game's StableDepth pass first (its images into `state`)"""
    d = os.path.join(work, "seq_" + name)
    os.makedirs(d, exist_ok=True)
    F, h, w = dist.shape
    with open(os.path.join(d, "seq.txt"), "w") as f:
        f.write("w %d\nh %d\nss 2\nframes %d\new %d\neh %d\nx_scale 1\nx_offset 0\ny_scale 1\ny_offset 0\ndisparity %g\ndist_min 0.12\nmax_shift 96\nfill %d\n" %
                (w, h, F, w, h, disp, fill))
        if mv is not None:
            f.write("stable 1\n")
        if soft:
            f.write("soft 1\n")  # [fix84] the four pixels of a ray each on their own surface (MonoPeriphery::Params::soft_outlines)
        for k, v in (extra or {}).items():
            f.write("%s %g\n" % (k, v))  # (other keys of gpu/periphery_test.cpp's seq mode: rate, step0, step1, ...)
    for t in range(F):
        c = np.concatenate([color2x[t].astype(np.float32), np.ones(color2x[t].shape[:2] + (1,), np.float32)], axis=-1)
        c.astype("<f4").tofile(os.path.join(d, "c%03d.bin" % t))
        dist[t].astype("<f4").tofile(os.path.join(d, "d%03d.bin" % t))
        if mv is not None:
            mv[t].astype("<f4").tofile(os.path.join(d, "m%03d.bin" % t))
    r = subprocess.run(RUN + ["./periphery_test.exe", "seq", d], cwd=GPU, env=ENV, capture_output=True, text=True, timeout=1800)
    if "done:" not in r.stdout:
        raise SystemExit("the GPU run failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    out = np.stack([np.fromfile(os.path.join(d, "e%03d.bin" % t), dtype="<f4").reshape(2 * h, 2 * w, 4) for t in range(F)])
    if mv is not None and state is not None:
        state.extend(np.fromfile(os.path.join(d, "s%03d.bin" % t), dtype="<f2").reshape(h, w, 4)[..., :2].astype(np.float32) for t in range(F))  # ([fix84] two more values follow)
    for t in range(F):
        for p in ("c", "d", "e", "m", "s"):
            f = os.path.join(d, "%s%03d.bin" % (p, t))
            if os.path.exists(f):
                os.remove(f)
    return out


def model_depth(seq):
    """the reference of the stable depth over the frames -> distances [F, h, w]"""
    sd = SD.StableDepth()
    return np.stack([sd.step(seq["dist"][t], seq["mv"][t]) for t in range(seq["dist"].shape[0])])


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    folder, work, names = args[0], args[1], args[2].split(",")
    os.makedirs(work, exist_ok=True)
    cap = CIO.Capture(folder)
    if "frames" in opt:
        cap.count = min(cap.count, int(opt["frames"]))
    F = cap.count
    disp = -float(opt.get("disp", 10.4))
    warm = int(opt.get("warm", 24))
    cn = os.path.basename(folder.rstrip("/"))[-2:]
    cache = os.path.join(work, "wide_%s_%d.npz" % (cn, F))
    if os.path.exists(cache):
        z = np.load(cache)
        seq = {k: z[k] for k in ("color", "mv", "dist", "jitter")}
        c2x = z["c2x"]
    else:
        seq = pcap.wide_sequence(cap, True)
        taa = R.NewTAA(scale=2, sharpen=0.0)
        c2x = []
        for t in range(F):
            taa.step(seq["color"][t], seq["mv"][t], seq["dist"][t], seq["jitter"][t])
            c2x.append(R.srgb_encode(taa.hist[..., :3]).astype(np.float32))
        c2x = np.stack(c2x)
        np.savez_compressed(cache, c2x=c2x, **seq)
    h, w = seq["dist"].shape[1:3]
    EH, EW = h * K, w * K
    # eye pixels near a step of the wide view's distance (any frame's raw depth), 24 eye pixels around
    dm = np.median(seq["dist"], axis=0)
    step = np.zeros((h, w), bool)
    rel = np.abs(dm[:, 1:] - dm[:, :-1]) / np.minimum(dm[:, 1:], dm[:, :-1])
    step[:, 1:] |= rel > 0.15
    step[:, :-1] |= rel > 0.15
    rel = np.abs(dm[1:] - dm[:-1]) / np.minimum(dm[1:], dm[:-1])
    step[1:] |= rel > 0.15
    step[:-1] |= rel > 0.15
    edge_e = ndimage.binary_dilation(np.kron(step, np.ones((K, K), bool)), iterations=24)
    margin = 32
    inner = np.zeros((EH, EW), bool)
    inner[margin:-margin, margin:-margin] = True
    res = {}
    keep = {}
    diffs = {}
    for name in names:
        if name == "none":
            per = c2x
        else:
            if name == "raw":
                dist = seq["dist"]
            elif name == "centre":
                dist = centre_dist(cap)
            elif name == "game":
                # the game's own pass (d3d12::StableDepth) before the reprojection, and its images against the model's
                st = []
                per = reproject(work, name + "_" + cn, c2x, seq["dist"], disp, mv=seq["mv"], state=st)[..., :3]
                sd = SD.StableDepth(half=True)
                worst = 0.0
                off = 0.0
                for t in range(F):
                    sd.step(seq["dist"][t], seq["mv"][t])
                    rel = np.abs(st[t] - sd.state) / np.maximum(np.abs(sd.state), 1e-6)
                    worst = max(worst, float(rel.max()))
                    off += float((rel[..., 0] > 0.01).mean())
                print("  the game's StableDepth against the model: largest relative difference %.4f, pixels over 1 %%: %.4f %% (mean over the frames)" %
                      (worst, 100.0 * off / F))
                dist = None
            elif name == "soft":
                # [fix84] the game's passes with the outlines from the stable depth's mean (soft_outlines)
                per = reproject(work, name + "_" + cn, c2x, seq["dist"], disp, mv=seq["mv"], soft=True)[..., :3]
                dist = None
            elif name == "model":
                dist = model_depth(seq)
            else:
                raise SystemExit("variant not known: " + name)
            if dist is not None:
                per = reproject(work, name + "_" + cn, c2x, dist, disp)[..., :3]
        acc = dict(f1e=0.0, f1o=0.0, f3e=0.0, f3o=0.0, n=0)
        prev = None
        outs = []
        for t in range(F):
            out = pchain.magnify(per[t], EW, EH, K / 2)
            outs.append(out)
            lo = pchain.luma(out)
            o1 = ndimage.gaussian_filter(lo, 1.0)
            o3 = ndimage.gaussian_filter(lo, 3.0)
            if prev is not None and t >= warm:
                mv = cap.motion_px(t)
                ys, xs = np.mgrid[0:EH, 0:EW].astype(np.float32)
                px = xs + mv[..., 0]
                py = ys + mv[..., 1]
                w1 = ndimage.map_coordinates(prev[0], [py, px], order=1, mode="nearest")
                w3 = ndimage.map_coordinates(prev[1], [py, px], order=1, mode="nearest")
                d1 = np.abs(o1 - w1)
                d3 = np.abs(o3 - w3)
                acc["f1e"] += float(d1[inner & edge_e].mean())
                acc["f1o"] += float(d1[inner & ~edge_e].mean())
                acc["f3e"] += float(d3[inner & edge_e].mean())
                acc["f3o"] += float(d3[inner & ~edge_e].mean())
                acc["n"] += 1
                if "diff" in opt and t >= F - 3:
                    diffs.setdefault(name, []).append(np.clip(d1 * float(opt.get("dgain", 12.0)), 0, 1)[..., None].repeat(3, axis=-1))
            prev = (o1, o3)
        n = max(acc["n"], 1)
        res[name] = {k: acc[k] / n for k in ("f1e", "f1o", "f3e", "f3o")}
        keep[name] = outs
    print("capture %s: %d frames, wide view %dx%d, eye pixels near a depth step %.0f %% (of the inner part)" % (cn, F, w, h, 100.0 * edge_e[inner].mean()))
    print("  %-8s | flicker x1000 near depth steps: sigma 1  sigma 3 | elsewhere: sigma 1  sigma 3" % "depth")
    for name in names:
        r = res[name]
        print("  %-8s |                                 %6.3f   %6.3f |            %6.3f   %6.3f" % (name, 1000 * r["f1e"], 1000 * r["f3e"], 1000 * r["f1o"], 1000 * r["f3o"]))
    if "sheet" in opt:
        t = F - 1
        rows = []
        for tt in (t - 2, t - 1, t):
            rows.append(AC.side_by_side([np.clip(keep[n][tt] * float(opt.get("gain", 1.5)), 0, 1) for n in names]))
        AC.write_png(opt["sheet"], np.concatenate(rows, axis=0))
        print("  " + opt["sheet"] + ": columns " + ", ".join(names) + "; rows: the last three frames")
    if "diff" in opt:
        rows = [AC.side_by_side([diffs[n][k] for n in names]) for k in range(3)]
        rows.append(AC.side_by_side([np.clip(keep[n][F - 1] * 1.5, 0, 1) for n in names]))
        AC.write_png(opt["diff"], np.concatenate(rows, axis=0))
        print("  " + opt["diff"] + ": columns " + ", ".join(names) + "; rows: |frame - the frame before along the motion| of the last three frames, the last frame")
    if "save" in opt:
        np.savez_compressed(opt["save"], **{n: np.stack(keep[n]).astype(np.float16) for n in names})


if __name__ == "__main__":
    main(sys.argv[1:])
