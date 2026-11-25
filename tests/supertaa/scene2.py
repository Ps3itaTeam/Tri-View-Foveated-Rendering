"""Second synthetic scene: what makes a temporal anti-aliasing ghost (found by the first test in the game, 16.11.2026:
text scrolling on a display, the wall behind a chair, a thin highlight; fix76 kept all three behind).

    python3 scene2.py data          ->  data/g_<camera>_p<phases>.npz   (about two minutes)

World plane as in scene.py (the camera translates; tiles of TILE px), plus things in front of it that move with their
own motion vectors (parallax), plus content that changes on its surface WITHOUT motion vectors.

Background tiles (id):
  0 scroll      text-like dark strokes (2 px) on bright yellow; scrolls up 3 px per frame during frames 40..46
  1 wall        boards, low contrast (behind the chairs and the wires)
  2 dither      a soft shadow rendered as the engine does: per pixel and frame either lit or dark (stochastic)
  3 slide       glossy surface with a soft highlight that slides 1 px per frame over it (view dependent, no motion vectors)
  4 sparkle     micro facets (scene.py "glints")
  5 lines       thin bright lines on dark (scene.py "lines_v")
  6 wall2       boards, brighter (behind the dark wire)
  7 texture     fractal detail (scene.py "texture")
  8 blink       a lamp flickering: gain changes every few frames
  9 edge        high contrast slanted edge
 10 wall3       boards again (behind the fast chair)
 11 fine        fine regular pattern near Nyquist (shimmer)
In front (negative ids), each with its own motion (parallax factor f: it moves f times as fast as the background):
 -1 chair A     vertical bars with gaps, f = 1.25   (slow parallax)
 -2 chair B     vertical bars with gaps, f = 1.75   (about 1 px per frame against the wall in the pan)
 -3 wire bright 0.3 px wide, f = 1.5, over the dark wall
 -4 wire dark   0.3 px wide, f = 1.5, over the bright wall
"""
import numpy as np

from scene import hash2, value_noise, fractal, _line_cover, jitter_sequence, camera_path, srgb_encode

TILE = 48
GRID_X, GRID_Y = 4, 3
REGION_NAMES = ["scroll", "wall", "dither", "slide", "sparkle", "lines", "wall2", "texture", "blink", "edge", "wall3", "fine"]
# which region a tile holds (rows of the world, repeated): the walls two tiles wide, for the things in front to move over
LAYOUT = np.array([[0, 3, 1, 1], [4, 5, 6, 6], [8, 2, 10, 10]], dtype=np.int64)
OBJECT_NAMES = {-1: "chairA", -2: "chairB", -3: "wire_bright", -4: "wire_dark"}
SCROLL_START, SCROLL_FRAMES, SCROLL_STEP = 40, 6, 3.0


def glyphs(u, v):
    """dark strokes 2 px wide on a grid of cells 8 x 10 (1 = stroke)"""
    cu = np.floor(u / 8.0)
    cv = np.floor(v / 10.0)
    fu = u - cu * 8.0
    fv = v - cv * 10.0
    a = (fu < 2.0) & (fv < 8.0) & (hash2(cu, cv, 101) > 0.35)
    b = (fv < 2.0) & (fu < 6.0) & (hash2(cu, cv, 102) > 0.45)
    c = (fv >= 4.0) & (fv < 6.0) & (fu < 6.0) & (hash2(cu, cv, 103) > 0.6)
    d = (fu >= 4.0) & (fu < 6.0) & (fv < 8.0) & (hash2(cu, cv, 104) > 0.55)
    return (a | b | c | d).astype(np.float64)


def boards(wx, wy, seed, base, amp):
    b = np.floor(wx / 9.0)
    tone = base + amp * (hash2(b, 0 * b, seed) - 0.5)
    gap = (np.mod(wx, 9.0) < 0.8).astype(np.float64)
    grain = 0.06 * (fractal(wx * 2.0, wy / 3.0, [8, 3, 1.3], seed=seed + 5) - 0.5)
    return np.clip(tone * (1.0 - 0.45 * gap) + grain, 0.0, 1.0)


class Scene2:
    def __init__(self, cam, frames, dither_mean=False):
        self.cam = np.asarray(cam, dtype=np.float64)
        self.frames = frames
        self.dither_mean = dither_mean  # the stochastic shadow as its mean (the reference)

    # screen position of an object's corner in frame t: it moves f times the background's motion
    def obj_x(self, t, x0, f, period=260.0):
        cam = self.cam[t + 1]
        return np.mod(x0 - f * (cam[0] - self.cam[0][0]) + 40.0, period) - 40.0

    def obj_shift(self, t, f):
        """motion vector of the object (to its place a frame before)"""
        return f * (self.cam[t + 1] - self.cam[t])

    def sample(self, px, py, t):
        px = np.asarray(px, dtype=np.float64)
        py = np.asarray(py, dtype=np.float64)
        shape = px.shape
        px = px.ravel()
        py = py.ravel()
        cam = self.cam[t + 1]
        cam_prev = self.cam[t]
        wx = px + cam[0]
        wy = py + cam[1]
        n = px.size
        rgb = np.zeros((n, 3))
        tx = np.floor(wx / TILE).astype(np.int64)
        ty = np.floor(wy / TILE).astype(np.int64)
        rid = LAYOUT[np.mod(ty, GRID_Y), np.mod(tx, GRID_X)]
        lx = wx - tx * TILE
        ly = wy - ty * TILE

        def put(mask, v, tint=(1.0, 1.0, 1.0)):
            rgb[mask] = v[:, None] * np.asarray(tint)[None, :]

        m = rid == 0
        if m.any():
            s = SCROLL_STEP * np.clip(t - SCROLL_START + 1, 0, SCROLL_FRAMES)
            g = glyphs(wx[m], wy[m] + s)
            put(m, 0.75 * (1.0 - 0.93 * g), (1.0, 0.82, 0.25))

        m = rid == 1
        if m.any():
            put(m, boards(wx[m], wy[m], 7, 0.16, 0.08), (1.0, 0.92, 0.8))

        m = rid == 2
        if m.any():
            base = 0.45 + 0.2 * value_noise(wx[m] / 25.0, wy[m] / 25.0, 12)
            p = np.clip(0.5 + 0.5 * np.sin(wx[m] / 7.0 + 0.3 * np.sin(wy[m] / 9.0)), 0.0, 1.0)   # share of shadow
            if self.dither_mean:
                put(m, base * (1.0 - 0.75 * p), (1.0, 0.9, 0.85))
            else:
                r = hash2(np.floor(px[m]), np.floor(py[m]), 200 + t)
                put(m, base * (1.0 - 0.75 * (r < p)), (1.0, 0.9, 0.85))

        m = rid == 3
        if m.any():
            tex = 0.10 + 0.10 * fractal(wx[m], wy[m], [10, 4, 2], seed=71)
            hx = 8.0 + np.mod(1.0 * t, 32.0)
            hl = np.exp(-0.5 * (((lx[m] - hx) / 4.0) ** 2 + ((ly[m] - 24.0) / 9.0) ** 2))
            put(m, np.clip(tex + 0.85 * hl, 0.0, 1.0), (0.95, 0.97, 1.0))

        m = rid == 4
        if m.any():
            cs = 0.2
            cx = np.floor(wx[m] / cs)
            cy = np.floor(wy[m] / cs)
            nx = hash2(cx, cy, 21)
            ny = hash2(cx, cy, 22)
            hx = np.mod(0.31 + 0.015 * cam[0], 1.0)
            hy = np.mod(0.57 + 0.015 * cam[1], 1.0)
            ddx = np.abs(nx - hx)
            ddx = np.minimum(ddx, 1.0 - ddx)
            ddy = np.abs(ny - hy)
            ddy = np.minimum(ddy, 1.0 - ddy)
            lit = ((ddx * ddx + ddy * ddy) < 0.11 ** 2).astype(np.float64)
            base = 0.06 + 0.05 * value_noise(wx[m] / 9.0, wy[m] / 9.0, 5)
            put(m, np.clip(base + 0.95 * lit, 0.0, 1.0), (1.0, 0.97, 0.9))

        m = rid == 5
        if m.any():
            d = np.mod(lx[m] + 0.045 * ly[m], 5.0) - 2.5
            put(m, 0.04 + 0.92 * _line_cover(d, 0.35), (1.0, 0.95, 0.85))

        m = rid == 6
        if m.any():
            put(m, boards(wx[m], wy[m], 17, 0.62, 0.14), (1.0, 0.95, 0.85))

        m = rid == 7
        if m.any():
            v = fractal(wx[m], wy[m], [24, 12, 6, 3, 1.5, 0.8, 0.45], seed=3)
            v = np.clip((v - 0.25) * 1.8, 0.0, 1.0) ** 1.5
            put(m, 0.03 + 0.85 * v, (1.0, 0.92, 0.8))

        m = rid == 8
        if m.any():
            tex = 0.14 + 0.25 * fractal(wx[m], wy[m], [10, 5, 2.5, 1.2], seed=51)
            gain = 1.0 + 1.2 * float(hash2(np.array(t // 3), np.array(0), 300) > 0.5)
            put(m, np.clip(tex * gain, 0.0, 1.0), (1.0, 0.95, 0.8))

        m = rid == 9
        if m.any():
            d = (lx[m] - 24.0) + 0.07 * (ly[m] - 24.0)
            d2 = (ly[m] - 36.0) - 0.05 * (lx[m] - 24.0)
            v = np.where(d > 0, 0.9, 0.02)
            v = np.where(d2 > 0, 0.9 - v + 0.02, v)
            put(m, v)

        m = rid == 10
        if m.any():
            put(m, boards(wx[m], wy[m], 27, 0.30, 0.12), (0.95, 0.95, 1.0))

        m = rid == 11
        if m.any():
            v = 0.5 + 0.4 * np.sin(2 * np.pi * (lx[m] + 0.3 * ly[m]) / 2.3) * np.sin(2 * np.pi * ly[m] / 2.7)
            put(m, v, (0.9, 1.0, 0.9))

        dist = np.full(n, 10.0)
        mv = np.empty((n, 2))
        mv[:, 0] = cam[0] - cam_prev[0]
        mv[:, 1] = cam[1] - cam_prev[1]
        rid = rid.copy()

        def chair(oid, x0, y0, f, tint, wd=34.0, ht=58.0):
            ox = px - self.obj_x(t, x0, f)
            oy = py - (y0 - f * (cam[1] - self.cam[0][1]))
            inside = (ox >= 0) & (ox < wd) & (oy >= 0) & (oy < ht)
            bars = (np.mod(ox, 9.0) < 4.5) | (oy < 12.0)
            hit = inside & bars
            if hit.any():
                shade = 0.55 + 0.35 * value_noise(ox[hit] / 6.0, oy[hit] / 6.0, 40 - oid)
                rgb[hit] = shade[:, None] * np.asarray(tint)[None, :]
                dist[hit] = 3.0
                sh = self.obj_shift(t, f)
                mv[hit, 0] = sh[0]
                mv[hit, 1] = sh[1]
                rid[hit] = oid

        def wire(oid, x0, y0, f, value, width=0.3, ht=44.0):
            ox = px - self.obj_x(t, x0, f)
            oy = py - (y0 - f * (cam[1] - self.cam[0][1]))
            hit = (np.abs(ox - 0.04 * oy) < 0.5 * width) & (oy >= 0) & (oy < ht)
            if hit.any():
                rgb[hit] = value
                dist[hit] = 2.0
                sh = self.obj_shift(t, f)
                mv[hit, 0] = sh[0]
                mv[hit, 1] = sh[1]
                rid[hit] = oid

        chair(-1, 150.0, 2.0, 1.25, (0.95, 0.62, 0.6))
        chair(-2, 140.0, 98.0, 1.6, (0.9, 0.85, 0.6), ht=44.0)
        wire(-3, 115.0, 2.0, 1.5, 0.85)
        wire(-4, 120.0, 52.0, 1.5, 0.02, ht=40.0)
        return rgb.reshape(shape + (3,)), dist.reshape(shape), mv.reshape(shape + (2,)), rid.reshape(shape)


def render_sequence(kind, frames, W, H, phases, ss=6, seed=1):
    cam = camera_path(kind, frames, seed)
    sc = Scene2(cam, frames)
    sc_ref = Scene2(cam, frames, dither_mean=True)
    jit = jitter_sequence(frames, phases)
    qx, qy = np.meshgrid(np.arange(W, dtype=np.float64), np.arange(H, dtype=np.float64))
    sub = (np.arange(ss) + 0.5) / ss
    sx, sy = np.meshgrid(sub, sub)
    col = np.zeros((frames, H, W, 3), dtype=np.float32)
    dist = np.zeros((frames, H, W), dtype=np.float32)
    mv = np.zeros((frames, H, W, 2), dtype=np.float32)
    ref = np.zeros((frames, H, W, 3), dtype=np.float32)
    rid = np.zeros((frames, H, W), dtype=np.int8)
    for t in range(frames):
        c, d, m, r = sc.sample(qx + 0.5 - jit[t, 0], qy + 0.5 - jit[t, 1], t)
        col[t] = np.round(srgb_encode(c) * 255.0) / 255.0     # 8 bit, as the game's image
        dist[t] = d
        mv[t] = m
        rx = qx[:, :, None, None] + sx[None, None]
        ry = qy[:, :, None, None] + sy[None, None]
        c2, _, _, _ = sc_ref.sample(rx, ry, t)
        ref[t] = c2.mean(axis=(2, 3))
        _, _, _, r0 = sc.sample(qx + 0.5, qy + 0.5, t)
        rid[t] = r0
    return dict(color=col, dist=dist, mv=mv, ref=ref, rid=rid, jitter=jit.astype(np.float32), cam=cam, kind=kind)


if __name__ == "__main__":
    import sys, os, time
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    W, H, F = 192, 144, 96
    kinds = sys.argv[2].split(",") if len(sys.argv) > 2 else ["tremor", "pan"]
    for kind in kinds:
        for phases in [8, 32]:
            t0 = time.time()
            seq = render_sequence(kind, F, W, H, phases)
            np.savez_compressed(os.path.join(out, f"g_{kind}_p{phases}.npz"), **seq)
            print(kind, phases, f"{time.time() - t0:.1f}s", flush=True)
