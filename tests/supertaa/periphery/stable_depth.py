"""[fix81 17.11.2026] The reference of the stable depth for the reprojection (d3d12::StableDepth, FoveaComposite.cpp).

The wide view's depth is sampled with the frame's jitter: at a step of the distance (the outline of a nearer thing) the
sample of a pixel is the near surface in some frames and the far one in others. The reprojection moves a pixel by
disparity / distance: with the frame's own depth the outline pixels jump between the near thing's place and the
background's from frame to frame, while their colour (the temporal pass's image) stands still.

Per pixel of the wide view, carried along the motion of the nearest surface in its 3 x 3:
  m   the mean of 1 / distance over time (the last one held within what the 3 x 3 shows now): on a surface its
      1 / distance at the pixel's centre, at a step how much of the pixel the near surface covers
  c   the 1 / distance handed to the reprojection
No step in the 3 x 3: c = m. A step (the 3 x 3's 1 / distances apart by more than step0..step1 of the nearest): the
sample of the 3 x 3 nearest to m (the surface that covers most of the pixel), with a margin that keeps the surface the
pixel was on, and that surface's own 1 / distance steadied over time.
"""
import numpy as np


def bilinear(img, x, y):
    H, W = img.shape[:2]
    x = np.clip(x, 0.0, W - 1.0)
    y = np.clip(y, 0.0, H - 1.0)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, W - 1)
    y1 = np.minimum(y0 + 1, H - 1)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]
    return (img[y0, x0] * (1 - fx) + img[y0, x1] * fx) * (1 - fy) + (img[y1, x0] * (1 - fx) + img[y1, x1] * fx) * fy


class StableDepth:
    """the reference of d3d12::StableDepth (fix81): the same steps in the same order"""

    def __init__(self, rate=0.125, step0=0.05, step1=0.15, hyst=0.3, dist_min=0.12, half=False):
        self.half = half  # the state kept as the pass's image holds it (16-bit floats)
        self.rate = np.float32(rate)
        self.step0 = np.float32(step0)
        self.step1 = np.float32(step1)
        self.hyst = np.float32(hyst)
        self.dist_min = np.float32(dist_min)
        self.state = None  # [h, w, 2]: 1 / distance handed on, mean of 1 / distance

    def step(self, dist, mv):
        """dist [h, w] metres (this frame's samples), mv [h, w, 2] pixels to the place a frame before -> distance [h, w]"""
        H, W = dist.shape
        inv = (1.0 / np.clip(dist, max(float(self.dist_min), 1e-3), 10000.0)).astype(np.float32)
        ys, xs = np.mgrid[0:H, 0:W]
        v = []
        near = np.zeros((H, W), np.float32)
        far = np.full((H, W), 1e30, np.float32)
        nx = xs.copy()
        ny = ys.copy()
        for k in range(9):
            yy = np.clip(ys + k // 3 - 1, 0, H - 1)
            xx = np.clip(xs + k % 3 - 1, 0, W - 1)
            vk = inv[yy, xx]
            v.append(vk)
            upd = vk > near
            near = np.where(upd, vk, near)
            nx = np.where(upd, xx, nx)
            ny = np.where(upd, yy, ny)
            far = np.minimum(far, vk)
        own = v[4]
        # the motion of the nearest surface of the 3 x 3 (as the temporal passes take it)
        mvn = mv[ny, nx]
        px = xs + 0.5 + mvn[..., 0]
        py = ys + 0.5 + mvn[..., 1]
        ok = (px >= 0.0) & (px <= W) & (py >= 0.0) & (py <= H)
        if self.state is None:
            ok &= False
            prev = np.zeros((H, W, 2), np.float32)
        else:
            prev = bilinear(self.state, px - 0.5, py - 0.5)
        mp = prev[..., 1]
        ok &= mp > 0.0
        # [fix84] the 1 / distance handed on a frame ago: not read between the texels (beside an outline that mixes the two
        # surfaces' values, see the pass). pc: the four texels around the place, pw their weights, cp the nearest one's.
        tx = px - 0.5
        ty = py - 0.5
        x0 = np.floor(tx)
        y0 = np.floor(ty)
        fx = (tx - x0).astype(np.float32)
        fy = (ty - y0).astype(np.float32)
        if self.state is None:
            pc = [np.zeros((H, W), np.float32)] * 4
        else:
            xi = [np.clip(x0.astype(np.int64) + k, 0, W - 1) for k in (0, 1)]
            yi = [np.clip(y0.astype(np.int64) + k, 0, H - 1) for k in (0, 1)]
            pc = [self.state[yi[0], xi[0], 0], self.state[yi[0], xi[1], 0], self.state[yi[1], xi[0], 0], self.state[yi[1], xi[1], 0]]
        pw = [(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy]
        cp = np.where(fy >= 0.5, np.where(fx >= 0.5, pc[3], pc[2]), np.where(fx >= 0.5, pc[1], pc[0]))
        ok &= cp > 0.0
        # the mean of 1 / distance, the last one held within what the 3 x 3 shows now
        m = np.where(ok, np.clip(mp, far, near), own)
        m = (m + self.rate * (own - m)).astype(np.float32)
        # a step: the sample of the 3 x 3 nearest to the mean; the one nearest to what was handed on a frame ago stays
        # until the mean has gone hyst of the step farther from it
        best = v[0]
        prevc = v[0]
        for k in range(1, 9):
            best = np.where(np.abs(v[k] - m) < np.abs(best - m), v[k], best)
            prevc = np.where(np.abs(v[k] - cp) < np.abs(prevc - cp), v[k], prevc)
        span = near - far
        keep = ok & (np.abs(prevc - cp) <= 0.5 * self.step0 * cp) & (np.abs(prevc - m) <= np.abs(best - m) + self.hyst * span)
        chosen = np.where(keep, prevc, best)
        # ... and that surface's own 1 / distance steadied over time
        # ([fix84] from the last frame's texels around the place that hold this surface, by their weights)
        same = [np.where(np.abs(pc[k] - chosen) <= self.step0 * chosen, pw[k], 0.0).astype(np.float32) for k in range(4)]
        sw = same[0] + same[1] + same[2] + same[3]
        cps = (same[0] * pc[0] + same[1] * pc[1] + same[2] * pc[2] + same[3] * pc[3]) / np.maximum(sw, 1e-9)
        steady = ok & (sw > 0.02)
        chosen = np.where(steady, cps + self.rate * (chosen - cps), chosen)
        t = np.clip((span / np.maximum(near, 1e-9) - self.step0) / (self.step1 - self.step0), 0.0, 1.0)
        t = t * t * (3.0 - 2.0 * t)
        out_inv = (m + (chosen - m) * t).astype(np.float32)
        self.state = np.stack([out_inv, m], axis=-1)
        if self.half:
            self.state = self.state.astype(np.float16).astype(np.float32)
            out_inv = self.state[..., 0]
        return (1.0 / np.maximum(out_inv, 1e-6)).astype(np.float32)
