"""[fix80 17.11.2026] Periphery simulation: the synthetic scene of tests/supertaa (scene.py) seen at the eye image's pixel
density (E pixels), and the shared wide view sampling it at 1/K of that density with the engine's jitter.
   python3 psim.py <out dir> <kinds: still,tremor,pan,turn> [frames, default 96] [capture.txt of a capture | haltonN]
writes <out dir>/p_<kind>.npz for pchain.py (the jitter: the engine's as a capture's header holds it, or Halton (2, 3)
of N frames; without the argument of 64).

  E pixel     one pixel of the eye image (the fovea is drawn 1 : 1 at this density)
  wide pixel  K E pixels per axis (the game: 4.01)
  scene unit  C E pixels (tests/supertaa/scene.py is written in units of the pixel density it was made for; with C = 3
              its thin lines are ~1 E pixel wide, 15-18 E pixels apart)

Textures are averaged over the wide pixel's footprint (what mip-mapping does); geometry (lines, wires, edges, the
object's outline) and highlights are point sampled, as a renderer without multisampling does.
"""
import os
import sys
import time

import numpy as np

TOOLS = os.environ.get("TOOLS", os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, TOOLS)
import scene as S  # noqa: E402

C = 3.0
K = 4
MIP = (-1, 0, 7, 8, 9, 10)  # object's texture, texture, zone plate, fire, shadow, switch
SWITCH = 56


def camera_path_e(kind, frames, seed=1):
    rng = np.random.default_rng(seed)
    cam = np.zeros((frames + 1, 2))
    if kind == "still":
        pass
    elif kind == "tremor":  # a head held still: ~0.5 E pixels per frame
        v = np.zeros(2)
        for t in range(1, frames + 1):
            v = 0.85 * v + rng.normal(0, 0.33, 2)
            cam[t] = cam[t - 1] + v + np.array([0.18, -0.08])
    elif kind == "pan":  # a slow turn: ~6 E pixels per frame (1.5 wide pixels)
        for t in range(1, frames + 1):
            cam[t] = cam[t - 1] + np.array([5.6, 1.7]) + rng.normal(0, 0.15, 2)
    elif kind == "turn":  # tremor, then a fast turn (38 E pixels per frame), then tremor
        v = np.zeros(2)
        for t in range(1, frames + 1):
            v = 0.85 * v + rng.normal(0, 0.33, 2)
            fast = np.array([38.0, 3.1]) if 40 <= t < 52 else np.zeros(2)
            cam[t] = cam[t - 1] + v + fast
    else:
        raise ValueError(kind)
    return cam + np.array([22.0, 9.4])


def engine_jitter(frames, capture_txt=None):
    """the engine's jitter (as the mod hands it on): from a capture's header when given, else Halton (2, 3) of 64"""
    if capture_txt and capture_txt.startswith("halton"):
        return S.jitter_sequence(frames, int(capture_txt[6:]))
    if capture_txt and os.path.exists(capture_txt):
        import re
        js = []
        for line in open(capture_txt, errors="replace"):
            m = re.match(r"frame \d+: jitter (\S+) (\S+)", line)
            if m:
                js.append((float(m.group(1)), float(m.group(2))))
        if js:
            js = np.array(js)
            return np.array([js[i % len(js)] for i in range(frames)])
    return S.jitter_sequence(frames, 64)


def render(kind, frames, EW, EH, jit, ss=4, nfoot=4, fovea=False):
    cam = camera_path_e(kind, frames)
    sc = S.Scene(cam / C, frames, switch_frame=SWITCH)
    WW, WH = EW // K, EH // K
    qx, qy = np.meshgrid(np.arange(WW, dtype=np.float64), np.arange(WH, dtype=np.float64))
    ex, ey = np.meshgrid(np.arange(EW, dtype=np.float64), np.arange(EH, dtype=np.float64))
    foot = ((np.arange(nfoot) + 0.5) / nfoot - 0.5) * K
    sub = (np.arange(ss) + 0.5) / ss
    col = np.zeros((frames, WH, WW, 3), dtype=np.float32)
    dist = np.zeros((frames, WH, WW), dtype=np.float32)
    mv = np.zeros((frames, WH, WW, 2), dtype=np.float32)
    ref = np.zeros((frames, EH, EW, 3), dtype=np.float16)
    rid = np.zeros((frames, EH, EW), dtype=np.int8)
    mve = np.zeros((frames, EH, EW, 2), dtype=np.float16)
    fcol = np.zeros((frames, EH, EW, 3), dtype=np.float16) if fovea else None
    for t in range(frames):
        cx = K * (qx + 0.5 - jit[t, 0])
        cy = K * (qy + 0.5 - jit[t, 1])
        c0, d0, m0, r0 = sc.sample(cx / C, cy / C, t)
        fx = cx[:, :, None, None] + foot[None, None, None, :]
        fy = cy[:, :, None, None] + foot[None, None, :, None]
        cf, _, _, rf = sc.sample(fx / C, fy / C, t)
        same = (rf == r0[:, :, None, None]).astype(np.float64)
        avg = (cf * same[..., None]).sum(axis=(2, 3)) / np.maximum(same.sum(axis=(2, 3)), 1.0)[..., None]
        mip = np.isin(r0, MIP)
        c = np.where(mip[..., None], avg, c0)
        col[t] = np.round(S.srgb_encode(c) * 255.0) / 255.0  # the engine's image after its output pass: 8 bit
        dist[t] = d0
        mv[t] = m0 * C / K
        rx = ex[:, :, None, None] + sub[None, None, None, :]
        ry = ey[:, :, None, None] + sub[None, None, :, None]
        c2, _, _, _ = sc.sample(rx / C, ry / C, t)
        ref[t] = c2.mean(axis=(2, 3))
        _, _, me, re_ = sc.sample((ex + 0.5) / C, (ey + 0.5) / C, t)
        rid[t] = re_
        mve[t] = me * C
        if fovea:  # the fovea view's own jittered samples (one per E pixel)
            cfv, _, _, _ = sc.sample((ex + 0.5 - jit[t, 0]) / C, (ey + 0.5 - jit[t, 1]) / C, t)
            fcol[t] = np.round(S.srgb_encode(cfv) * 255.0) / 255.0
    out = dict(color=col, dist=dist, mv=mv, ref=ref, rid=rid, mve=mve, jitter=jit.astype(np.float32), cam=cam)
    if fovea:
        out["fcolor"] = fcol
    return out


if __name__ == "__main__":
    out = sys.argv[1]
    kinds = sys.argv[2].split(",")
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 80
    cap = sys.argv[4] if len(sys.argv) > 4 else None
    os.makedirs(out, exist_ok=True)
    EW, EH = 576, 432
    jit = engine_jitter(frames, cap)
    for kind in kinds:
        t0 = time.time()
        seq = render(kind, frames, EW, EH, jit)
        np.savez(os.path.join(out, f"p_{kind}.npz"), **seq)
        print(kind, f"{time.time() - t0:.1f}s", flush=True)
