"""Synthetic test scene for temporal resolvers (linear light, analytic, point-sampled at any position).

Screen coordinates are in input pixels (pixel q covers [q, q+1), centre q+0.5). The camera only translates:
a background point at screen position p in frame t is the world point w = p + cam[t]. Regions are tiles of the
world plane (TILE px), each one a typical trouble maker for temporal anti-aliasing.
"""
import numpy as np

TILE = 48
GRID_X, GRID_Y = 4, 3

REGION_NAMES = [
    "texture",        # 0 fractal detail up to above Nyquist
    "lines_h",        # 1 thin bright near-horizontal lines on dark
    "lines_v",        # 2 thin bright near-vertical lines on dark
    "lines_diag",     # 3 thin bright diagonal lines
    "fence_dark",     # 4 thin dark wires on a bright background
    "glints",         # 5 view dependent sparkles on dark metal
    "edge",           # 6 high contrast slanted edge
    "zoneplate",      # 7 chirp, all frequencies
    "fire",           # 8 area flicker (must be followed, not smoothed)
    "shadow",         # 9 shadow edge moving through the world (no motion vectors)
    "switch",         # 10 a light that goes on at one frame
    "brushed",        # 11 brushed metal: sub-pixel highlight streaks that move with the view
]


def _hash_u32(x):
    x = np.asarray(x, dtype=np.uint64) & np.uint64(0xFFFFFFFF)
    x = (x ^ (x >> np.uint64(16))) * np.uint64(0x7FEB352D) & np.uint64(0xFFFFFFFF)
    x = (x ^ (x >> np.uint64(15))) * np.uint64(0x846CA68B) & np.uint64(0xFFFFFFFF)
    x = x ^ (x >> np.uint64(16))
    return x & np.uint64(0xFFFFFFFF)


def hash2(ix, iy, seed=0):
    """uniform [0,1) from integer lattice coordinates"""
    ix = np.asarray(ix).astype(np.int64)
    iy = np.asarray(iy).astype(np.int64)
    h = _hash_u32((ix * 73856093) ^ (iy * 19349663) ^ (seed * 83492791))
    return h.astype(np.float64) / 4294967296.0


def value_noise(x, y, seed=0):
    ix = np.floor(x)
    iy = np.floor(y)
    fx = x - ix
    fy = y - iy
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    a = hash2(ix, iy, seed)
    b = hash2(ix + 1, iy, seed)
    c = hash2(ix, iy + 1, seed)
    d = hash2(ix + 1, iy + 1, seed)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fractal(x, y, wavelengths, seed=0):
    s = 0.0
    wsum = 0.0
    for i, wl in enumerate(wavelengths):
        a = 1.0 / (1.0 + 0.6 * i)
        s = s + a * value_noise(x / wl, y / wl, seed + i * 17)
        wsum += a
    return s / wsum


def _line_cover(d, width):
    """hard-edged line of `width` centred at d == 0 (point sampled: 0 or 1)"""
    return (np.abs(d) < 0.5 * width).astype(np.float64)


class Scene:
    def __init__(self, cam, frames, switch_frame=40, obj=True):
        self.cam = np.asarray(cam, dtype=np.float64)  # [frames + 1, 2], cam[0] = position before frame 0
        self.frames = frames
        self.switch_frame = switch_frame
        self.obj = obj

    # ---- the foreground object: a disc moving over the screen ------------------------------------------------
    def obj_pos(self, t):
        # screen position of its centre in frame t (t may be -1)
        x = 30.0 + 1.7 * (t + 1)
        period = 2 * 130.0
        x = np.mod(x, period)
        x = np.where(x > 130.0, period - x, x) + 20.0
        return np.array([x, 60.0 + 12.0 * np.sin(0.11 * (t + 1))])

    def sample(self, px, py, t):
        """px, py: screen positions (any shape). Returns linear rgb [...,3], distance [...], mv [...,2] (current
        position + mv = position of the same surface point in frame t-1), region id [...] (-1 = the object)."""
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
        rid = (np.mod(tx, GRID_X) + GRID_X * np.mod(ty, GRID_Y)).astype(np.int64)
        lx = wx - tx * TILE  # position inside the tile
        ly = wy - ty * TILE

        def put(mask, v, tint=(1.0, 1.0, 1.0)):
            rgb[mask] = v[:, None] * np.asarray(tint)[None, :]

        m = rid == 0
        if m.any():
            v = fractal(wx[m], wy[m], [24, 12, 6, 3, 1.5, 0.8, 0.45], seed=3)
            v = np.clip((v - 0.25) * 1.8, 0.0, 1.0) ** 1.5
            put(m, 0.03 + 0.85 * v, (1.0, 0.92, 0.8))

        m = rid == 1
        if m.any():
            d = np.mod(ly[m] + 0.03 * lx[m], 6.0) - 3.0
            put(m, 0.04 + 0.92 * _line_cover(d, 0.3), (0.9, 0.95, 1.0))

        m = rid == 2
        if m.any():
            d = np.mod(lx[m] + 0.045 * ly[m], 5.0) - 2.5
            put(m, 0.04 + 0.92 * _line_cover(d, 0.35), (1.0, 0.95, 0.85))

        m = rid == 3
        if m.any():
            d = np.mod((lx[m] + ly[m]) * 0.70710678, 5.0) - 2.5
            put(m, 0.05 + 0.9 * _line_cover(d, 0.35))

        m = rid == 4
        if m.any():
            d1 = np.mod(lx[m] + 0.02 * ly[m], 5.0) - 2.5
            d2 = np.mod(ly[m] - 0.035 * lx[m], 5.0) - 2.5
            wire = np.maximum(_line_cover(d1, 0.3), _line_cover(d2, 0.3))
            base = 0.55 + 0.25 * value_noise(wx[m] / 20.0, wy[m] / 20.0, 9)
            put(m, base * (1.0 - 0.93 * wire), (0.85, 0.95, 1.0))

        m = rid == 5
        if m.any():
            # micro facets of 0.2 px: lit while their slope is within r of the half vector, which moves with the view
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

        m = rid == 6
        if m.any():
            d = (lx[m] - 24.0) + 0.07 * (ly[m] - 24.0)
            d2 = (ly[m] - 36.0) - 0.05 * (lx[m] - 24.0)
            v = np.where(d > 0, 0.9, 0.02)
            v = np.where(d2 > 0, 0.9 - v + 0.02, v)
            put(m, v)

        m = rid == 7
        if m.any():
            r2 = (lx[m] - 24.0) ** 2 + (ly[m] - 24.0) ** 2
            put(m, 0.5 + 0.45 * np.cos(np.pi * r2 / 26.0))

        m = rid == 8
        if m.any():
            tex = fractal(wx[m], wy[m], [16, 8, 4, 2], seed=31)
            fl = 1.0 + 0.6 * (value_noise(wx[m] / 14.0 + 0.9 * t, wy[m] / 14.0 - 1.3 * t, 77) - 0.5) * 2.0
            put(m, np.clip((0.12 + 0.5 * tex) * fl, 0.0, 1.0), (1.0, 0.7, 0.35))

        m = rid == 9
        if m.any():
            tex = 0.35 + 0.3 * fractal(wx[m], wy[m], [12, 6, 3, 1.5], seed=41)
            edge = np.mod(1.9 * t, TILE * 1.0)
            inshadow = (np.mod(lx[m] - edge, TILE) < 16.0).astype(np.float64)
            put(m, tex * (1.0 - 0.7 * inshadow))

        m = rid == 10
        if m.any():
            tex = 0.14 + 0.2 * fractal(wx[m], wy[m], [10, 5, 2.5, 1.2], seed=51)
            gain = 2.6 if t >= self.switch_frame else 1.0
            put(m, np.clip(tex * gain, 0.0, 1.0), (1.0, 0.95, 0.8))

        m = rid == 11
        if m.any():
            # brushed metal: horizontal scratches 0.15 px high whose highlight comes and goes with the view height
            row = np.floor(wy[m] * 3.0)
            ph = hash2(row, 0 * row, 61)
            vis = np.mod(ph + 0.02 * cam[1] + 0.004 * wx[m], 1.0) < 0.10
            inside = np.mod(wy[m] * 3.0, 1.0) < 0.45
            base = 0.10 + 0.06 * value_noise(wx[m] / 30.0, wy[m] / 2.0, 63)
            put(m, np.clip(base + 0.9 * (vis & inside), 0.0, 1.0), (0.95, 0.97, 1.0))

        dist = np.full(n, 10.0)
        mv = np.empty((n, 2))
        mv[:, 0] = cam[0] - cam_prev[0]
        mv[:, 1] = cam[1] - cam_prev[1]

        if self.obj:
            o = self.obj_pos(t)
            op = self.obj_pos(t - 1)
            ox = px - o[0]
            oy = py - o[1]
            inside = (ox * ox + oy * oy) < 14.0 ** 2
            if inside.any():
                chk = (np.floor(ox[inside] / 3.0) + np.floor(oy[inside] / 3.0)) % 2
                stripe = _line_cover(np.mod(oy[inside], 4.0) - 2.0, 0.3)
                v = 0.25 + 0.35 * chk + 0.4 * stripe
                rgb[inside] = np.clip(v, 0, 1)[:, None] * np.array([0.8, 1.0, 0.75])[None, :]
                dist[inside] = 1.0
                mv[inside, 0] = op[0] - o[0]
                mv[inside, 1] = op[1] - o[1]
                rid = rid.copy()
                rid[inside] = -1

        return rgb.reshape(shape + (3,)), dist.reshape(shape), mv.reshape(shape + (2,)), rid.reshape(shape)


def halton(i, b):
    f = 1.0
    r = 0.0
    while i > 0:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def jitter_sequence(n, phases):
    """as the mod hands it to its TAA: the sample of pixel q shows the point at q + 0.5 - jitter (FSR's Halton
    (2,3) minus 0.5, sign flipped as TemporalUpscaler stores it)"""
    out = np.zeros((n, 2))
    for k in range(n):
        idx = (k % phases) + 1
        out[k, 0] = -(halton(idx, 2) - 0.5)
        out[k, 1] = -(halton(idx, 3) - 0.5)
    return out


def camera_path(kind, frames, seed=1):
    rng = np.random.default_rng(seed)
    cam = np.zeros((frames + 1, 2))
    if kind == "still":
        pass
    elif kind == "tremor":
        # a head "held still": slow drift plus tremor, ~0.4 px per frame
        v = np.zeros(2)
        for t in range(1, frames + 1):
            v = 0.85 * v + rng.normal(0, 0.22, 2)
            cam[t] = cam[t - 1] + v + np.array([0.12, -0.05])
    elif kind == "pan":
        for t in range(1, frames + 1):
            cam[t] = cam[t - 1] + np.array([1.37, 0.41]) + rng.normal(0, 0.05, 2)
    elif kind == "turn":
        for t in range(1, frames + 1):
            sp = 9.3 if 30 <= t < 55 else 0.35
            cam[t] = cam[t - 1] + np.array([sp, 0.23 * sp / 9.3 + 0.1]) + rng.normal(0, 0.08, 2)
    else:
        raise ValueError(kind)
    return cam + np.array([7.3, 3.1])


def srgb_encode(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 1e-12), 1.0 / 2.4) - 0.055)


def srgb_decode(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))


def render_sequence(kind, frames, W, H, phases, ss=6, seed=1, obj=True):
    """inputs as the mod's TAA gets them (encoded 10 bit colour, distance, motion in pixels) and the reference
    (area average of each pixel in linear light)"""
    cam = camera_path(kind, frames, seed)
    sc = Scene(cam, frames, obj=obj)
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
        enc = np.round(srgb_encode(c) * 1023.0) / 1023.0
        col[t] = enc
        dist[t] = d
        mv[t] = m
        # reference and region ids at the pixel (no jitter)
        rx = qx[:, :, None, None] + sx[None, None]
        ry = qy[:, :, None, None] + sy[None, None]
        c2, _, _, _ = sc.sample(rx, ry, t)
        ref[t] = c2.mean(axis=(2, 3))
        _, _, _, r0 = sc.sample(qx + 0.5, qy + 0.5, t)
        rid[t] = r0
    return dict(color=col, dist=dist, mv=mv, ref=ref, rid=rid, jitter=jit.astype(np.float32), cam=cam, kind=kind)


if __name__ == "__main__":
    import sys, os, time
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    W, H, F = 192, 144, 96
    for kind in ["still", "tremor", "pan", "turn"]:
        for phases in [8, 32]:
            t0 = time.time()
            seq = render_sequence(kind, F, W, H, phases)
            np.savez_compressed(os.path.join(out, f"{kind}_p{phases}.npz"), **seq)
            print(kind, phases, f"{time.time() - t0:.1f}s", flush=True)
