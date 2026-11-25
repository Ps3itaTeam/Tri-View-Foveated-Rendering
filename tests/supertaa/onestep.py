"""One-step comparison: for every frame the numpy model starts from the GPU's own state of the frame before, so the
differences of one frame are seen without those accumulated over the frames before.  onestep.py <case dir> [bits]"""
import sys, os, json, numpy as np
import resolvers as R
import make_case as M

d = sys.argv[1]
R.BILINEAR_BITS = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] != "none" else None
meta = {}
jit = []
for line in open(os.path.join(d, "meta.txt")):
    k, *v = line.split()
    if k == "jitter":
        jit.append((float(v[0]), float(v[1])))
    else:
        meta[k] = v[0]
w, h, ow, oh, frames = (int(meta[k]) for k in ("width", "height", "out_width", "out_height", "frames"))
c = json.load(open(os.path.join(d, "settings.json")))
scale = int(c["scale"])
out10 = int(c.get("out_10bit", 0)) != 0


def gpu(t, n, shape):
    return np.fromfile(os.path.join(d, "gpu", f"frame{t:02d}_{n}.bin"), dtype=np.float16).reshape(shape).astype(np.float32)


def gpu_out(t):
    if out10:
        raw = np.fromfile(os.path.join(d, "gpu", f"frame{t:02d}_out.bin"), dtype=np.uint32).reshape(oh, ow)
        return np.stack([(raw >> s) & 1023 for s in (0, 10, 20)], axis=-1).astype(np.float32) / 1023.0
    return gpu(t, "out", (oh, ow, 4))[..., :3]


print(f"{'frame':>5s} {'guide mv':>18s} {'guide weight':>18s} {'guide protect':>18s} {'stats':>18s} {'release':>18s} {'hist rgb':>18s} {'hist weight':>18s} {'out':>18s} | pixels of out off by more than 0.004 / 0.02")
changed = 0
protected = 0
arrived = 0
marked = 0
tot_bad = 0
tot = 0
worst_p999 = 0.0
for t in range(1, frames):
    res = R.NewTAA(**M.numpy_settings(c))
    res.half = True
    res.hist = gpu(t - 1, "hist", (oh * scale, ow * scale, 4))
    res.stats = gpu(t - 1, "stats", (h, w, 4))
    res.set_state_image(gpu(t - 1, "dist", (h, w, 4)))
    col = np.fromfile(os.path.join(d, f"frame{t:02d}_color.bin"), dtype=np.float32).reshape(h, w, 3)
    mv = np.fromfile(os.path.join(d, f"frame{t:02d}_mv.bin"), dtype=np.float32).reshape(h, w, 2)
    dist = np.float32(0.1) / np.fromfile(os.path.join(d, f"frame{t:02d}_depth.bin"), dtype=np.float32).reshape(h, w)
    lin, guide, boxc, boxe = res.analyze(col, mv, dist, jit[t])
    res.accumulate(lin, guide, boxc, boxe, jit[t])
    changed += int((res.debug["conf"] > 0.5).sum())
    protected += int((guide[..., 3] > 0.5).sum())
    arrived += int(res.debug["arrive"].sum())
    marked += int((res.rel > 1.0).sum())
    o = res.resolve()
    if out10:
        o = np.round(np.clip(o, 0, 1) * 1023.0) / 1023.0
    gg = gpu(t, "guide", (h, w, 4))
    gs = gpu(t, "stats", (h, w, 4))
    gd = gpu(t, "dist", (h, w, 4))
    gh = gpu(t, "hist", (oh * scale, ow * scale, 4))
    go = gpu_out(t)

    def f(a, b):
        df = np.abs(a - b)
        return f"{df.max():8.5f}/{np.percentile(df, 99.9):8.5f}"

    do = np.abs(go - o).max(axis=-1)
    tot_bad += int((do > 0.02).sum())
    tot += do.size
    worst_p999 = max(worst_p999, float(np.percentile(do, 99.9)))
    print(f"{t:5d} {f(gg[..., :2], guide[..., :2]):>18s} {f(gg[..., 2], guide[..., 2]):>18s} {f(gg[..., 3], guide[..., 3]):>18s} {f(gs, res.stats):>18s} {f(gd[..., 1], res.rel):>18s} "
          f"{f(gh[..., :3], res.hist[..., :3]):>18s} {f(gh[..., 3], res.hist[..., 3]):>18s} {f(go, o):>18s} | {int((do > 0.004).sum()):5d} {int((do > 0.02).sum()):4d} of {do.size}")
print(f"output: worst 99.9th percentile {worst_p999:.5f}; pixels off by more than 0.02: {tot_bad} of {tot} ({100.0 * tot_bad / tot:.4f} %); "
      f"pixels following a change: {changed}; protected: {protected}; where a thing arrived: {arrived} (marked: {marked})")
