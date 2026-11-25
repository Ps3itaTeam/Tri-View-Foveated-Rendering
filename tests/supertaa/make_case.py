"""Writes a test case for gpu/supertaa_test.exe from a synthetic sequence (scene.py) and runs the numpy reference on it.
   make_case.py <sequence.npz> <case dir> <x0> <y0> <w> <h> <frames> [key=value ...]
   keys: the settings of d3d12::SuperTAA::Settings (CPP_DEFAULTS below), out_ratio (fovea image / view), out_10bit=1,
   first=N (the sequence's frame the case starts with), flies=N (N small near things put into the frames that move
   several pixels a frame while the motion vectors say nothing of it: the game's flies; the last one moves slowly and
   the one before is a single pixel), and for the test program: game=1 (the game's formats), slots=1 (one view's
   histories only: the periphery's instance; with out_ratio=2 scale=1 sharpen=0 its image of twice the view's size),
   capture=N capture_part=P (the last N frames through d3d12::SuperTAACapture), paths=1 (the pass's other paths and
   d3d12::TextureDump)"""
import sys, os, json, numpy as np
import resolvers as R

CPP_DEFAULTS = dict(scale=2, filter_scale=1.0, stability=5.0, clip_gamma=1.0, motion_reduce=0.25, depth_reject=0.10, history_sharpness=0.5,
                    sharpen=0.30, luma_weight=1, dering=1, nearest_sample=0, box_resolve=1, follow=1, follow_threshold=1.0, follow_memory=0.5,
                    fl_count=5, fl_alln=8, fl_cap=0.04, protect=1, protect_range=1.5, protect_spark=2.5, protect_memory=1.5, arrive=1, out_ratio=1.0,
                    debug_view=0, out_10bit=0)
INT_KEYS = ("scale", "luma_weight", "dering", "nearest_sample", "box_resolve", "follow", "fl_count", "fl_alln", "protect", "arrive", "debug_view", "out_10bit", "game",
            "capture", "capture_part", "paths", "split_x", "first", "flies", "slots")

# a fly as the game draws it at 2.5 m: a few bright specks (1) and dark parts (2) on some 15 pixels
FLY = np.array([[0, 1, 1, 0, 0],
                [1, 1, 2, 1, 1],
                [0, 2, 2, 1, 0],
                [0, 1, 0, 0, 0]])


def add_flies(col, dist, t, count):
    """puts `count` flies into frame t of a case (colour and distances; the motion vectors stay as they are)"""
    h, w = dist.shape
    col, dist = col.copy(), dist.copy()
    near = np.float32(0.3) * dist.min()
    for i in range(count):
        slow = count >= 3 and i == count - 1
        speck = count >= 4 and i == count - 2
        vx, vy = (1.0, 0.5) if slow else (6.0 + 1.5 * i, 3.0 - 2.0 * i)
        px = 7.0 + 23.0 * i + vx * t
        py = 9.0 + 17.0 * i + vy * t
        # (back and forth inside the part)
        rx, ry = w - 12, h - 10
        px = 3 + (rx - abs(px % (2 * rx) - rx))
        py = 3 + (ry - abs(py % (2 * ry) - ry))
        x0, y0 = int(px), int(py)
        shape = np.array([[1]]) if speck else FLY
        for (yy, xx), v in np.ndenumerate(shape):
            if v:
                col[y0 + yy, x0 + xx] = 0.95 if v == 1 else 0.03
                dist[y0 + yy, x0 + xx] = near
    return col, dist


def numpy_settings(c):
    """the model's settings for the pass's (d3d12::SuperTAA::dispatch fills its constants the same way)"""
    tpp = c["scale"] * c["out_ratio"]
    sigma = min(max(0.5 / tpp, 0.20), 0.35) * c["filter_scale"]
    d = R.NewTAA.DEFAULTS
    thr = c["follow_threshold"]
    return dict(scale=int(c["scale"]), out_ratio=c["out_ratio"], sigma=sigma, w_max=c["stability"], clip_gamma=c["clip_gamma"],
                motion_reduce=c["motion_reduce"], depth_reject=c["depth_reject"], sharpen=c["sharpen"], luma_weight=bool(c["luma_weight"]),
                hist_extra=c["history_sharpness"], hist_dering={0: False, 1: True, 2: 3}[int(c["dering"])], cur_nearest=bool(c["nearest_sample"]),
                down="box" if c["box_resolve"] else "lanczos2", follow=bool(c["follow"]), fl_k=d["fl_k"] * thr, fl_b0=d["fl_b0"] * thr,
                fl_kc=d["fl_kc"] * thr, fl_b0c=d["fl_b0c"] * thr, fl_cap=c["fl_cap"], fl_count=int(c["fl_count"]), fl_alln=int(c["fl_alln"]),
                w_short=min(c["follow_memory"], c["stability"]), fl_sde=d["fl_sde"] * thr, protect=bool(c["protect"]),
                af_relax=min(max(c["protect_range"], 0.0), 8.0), af_wins=min(max(c["protect_spark"], 0.0), 16.0),
                af_mem=min(max(c["protect_memory"], 1.0), 4.0), arrive=bool(c.get("arrive", 1)))


def main():
    seq = dict(np.load(sys.argv[1]))
    out = sys.argv[2]
    x0, y0, w, h, frames = [int(v) for v in sys.argv[3:8]]
    c = dict(CPP_DEFAULTS)
    for kv in sys.argv[8:]:
        k, v = kv.split("=")
        c[k] = float(v) if k not in INT_KEYS else int(v)
    first = int(c.pop("first", 0))
    flies = int(c.pop("flies", 0))
    os.makedirs(out, exist_ok=True)
    ow, oh = int(round(w * c["out_ratio"])), int(round(h * c["out_ratio"]))
    with open(os.path.join(out, "meta.txt"), "w") as f:
        f.write(f"width {w}\nheight {h}\nout_width {ow}\nout_height {oh}\nframes {frames}\n")
        for k, v in c.items():
            if k != "out_ratio":
                f.write(f"{k} {v}\n")
        for t in range(first, first + frames):
            f.write(f"jitter {seq['jitter'][t, 0]:.9g} {seq['jitter'][t, 1]:.9g}\n")
    res = R.NewTAA(**numpy_settings(c))
    res.half = True
    R.BILINEAR_BITS = int(os.environ['BILINEAR_BITS']) if os.environ.get('BILINEAR_BITS') else None
    ref = {}
    for t in range(frames):
        col = seq["color"][first + t, y0:y0 + h, x0:x0 + w].astype(np.float32)
        mv = seq["mv"][first + t, y0:y0 + h, x0:x0 + w].astype(np.float32)
        dist = seq["dist"][first + t, y0:y0 + h, x0:x0 + w].astype(np.float32)
        if flies:
            col, dist = add_flies(col, dist, t, flies)
        col.tofile(os.path.join(out, f"frame{t:02d}_color.bin"))
        mv.tofile(os.path.join(out, f"frame{t:02d}_mv.bin"))
        (np.float32(0.1) / dist).astype(np.float32).tofile(os.path.join(out, f"frame{t:02d}_depth.bin"))
        lin, guide, boxc, boxe = res.analyze(col, mv, dist, seq["jitter"][first + t])
        res.accumulate(lin, guide, boxc, boxe, seq["jitter"][first + t])
        o = res.resolve()
        ref[f"{t}_lin"] = lin
        ref[f"{t}_guide"] = guide
        ref[f"{t}_boxc"] = boxc
        ref[f"{t}_boxe"] = boxe
        ref[f"{t}_stats"] = res.stats.copy()
        ref[f"{t}_dist"] = res.state_image()
        ref[f"{t}_hist"] = res.hist.copy()
        ref[f"{t}_out"] = o.astype(np.float32)
    np.savez_compressed(os.path.join(out, "reference.npz"), **ref)
    json.dump(c, open(os.path.join(out, "settings.json"), "w"))
    print("case written:", out, f"{w}x{h} -> {ow}x{oh}, {frames} frames")


if __name__ == "__main__":
    main()
