"""Compares what supertaa_test.exe wrote with the numpy reference.  compare_case.py <case dir>"""
import sys, os, json, numpy as np


def main():
    d = sys.argv[1]
    meta = {}
    for line in open(os.path.join(d, "meta.txt")):
        k, *v = line.split()
        if k != "jitter":
            meta[k] = v[0]
    w, h, ow, oh, frames = (int(meta[k]) for k in ("width", "height", "out_width", "out_height", "frames"))
    scale = int(meta["scale"])
    out10 = int(meta.get("out_10bit", "0")) != 0
    ref = np.load(os.path.join(d, "reference.npz"))
    worst = 0.0
    names = [("lin", (h, w, 4), 3), ("guide", (h, w, 4), 4), ("boxc", (h, w, 4), 3), ("boxe", (h, w, 4), 3), ("stats", (h, w, 4), 4),
             ("dist", (h, w, 4), 4), ("hist", (oh * scale, ow * scale, 4), 4), ("out", (oh, ow, 4), 3)]
    print(f"{'frame':>5s} " + " ".join(f"{n:>21s}" for n, _, _ in names) + "   (max |difference| / 99.9th percentile)")
    for t in range(frames):
        row = f"{t:5d} "
        for n, shape, ch in names:
            path = os.path.join(d, "gpu", f"frame{t:02d}_{n}.bin")
            if n == "out" and out10:
                raw = np.fromfile(path, dtype=np.uint32).reshape(oh, ow)
                g = np.stack([(raw >> s) & 1023 for s in (0, 10, 20)], axis=-1).astype(np.float32) / 1023.0
                r = np.round(np.clip(ref[f"{t}_{n}"], 0, 1) * 1023.0) / 1023.0
            else:
                g = np.fromfile(path, dtype=np.float16).reshape(shape).astype(np.float32)
                r = ref[f"{t}_{n}"]
                if ch:
                    g = g[..., :ch]
                    r = r[..., :ch]
            diff = np.abs(g - r)
            row += f"{diff.max():10.5f}/{np.percentile(diff, 99.9):10.5f} "
            if n in ("out", "hist"):
                worst = max(worst, float(np.percentile(diff[..., :3] if diff.ndim == 3 else diff, 99.9)))
        print(row)
    print(f"worst 99.9th percentile of history / output colour: {worst:.5f}")
    if "-v" in sys.argv:
        for t in (1, 5, frames - 1):
            for n, shape, labels in (("guide", (h, w, 4), ("mv.x", "mv.y", "weight", "code")), ("stats", (h, w, 4), ("noise", "mean", "d", "change")),
                                     ("hist", (oh * scale, ow * scale, 4), ("r", "g", "b", "weight"))):
                g = np.fromfile(os.path.join(d, "gpu", f"frame{t:02d}_{n}.bin"), dtype=np.float16).reshape(shape).astype(np.float32)
                r = ref[f"{t}_{n}"]
                diff = np.abs(g - r)
                print(f"  frame {t} {n}: " + "  ".join(f"{l} max {diff[..., i].max():.5f} p99 {np.percentile(diff[..., i], 99):.5f} p99.9 {np.percentile(diff[..., i], 99.9):.5f}"
                                                        for i, l in enumerate(labels)))
    return worst


if __name__ == "__main__":
    main()
