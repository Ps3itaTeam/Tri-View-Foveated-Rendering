"""[fix100 20.11.2026] Reference model of the periphery's calm light TAA (d3d12::LightTAA with Params::calm, calm_resolve in
LIGHT_TAA_SHADER; quadview::Settings::periphery_aa 3): the light TAA's pass with a protection of what one frame's samples
cannot show.

A second history holds, along the motion, the mean of max(e, 0)^2 and of max(-e, 0)^2, e = the pixel's own sample minus the
mean of its 3 x 3 (luma), and the depth. The history is kept within mean +- clip_gamma * sigma of this frame's 3 x 3 as in
the light TAA, the luma's limit wider by `relax` times the scatter the own samples have shown towards that side; where it is
protected the history weighs more (`mem`) and the frame's sample counts no farther from it than `wins` deviations; released
where the place held something nearer a frame ago and where the history is farther outside the limits than `over`; a
change of the whole 3 x 3 against the history beyond what its spread and the scatter explain is followed at once (`follow`).

The values are the shader's (CALM_*). half=True rounds what is stored to 16 bit floats as the textures do (to compare with
the GPU: tests/supertaa/calm/)."""
import numpy as np

import resolvers as R


def smooth01(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


class CalmTAA:
    DEFAULTS = dict(feedback=0.93, clip_gamma=1.25, filter_radius=0.46727, motion_reduce=0.25, luma_weight=True,
                    rate=1.0 / 32.0, t0=0.01, t1=0.05, relax=1.0, wins=2.5, wins_floor=0.02, mem=3.0, over=0.08, depth_reject=0.10,
                    follow=2.0, follow_b0=0.015, follow_short=0.5, half=False)

    def __init__(self, **kw):
        self.s = dict(self.DEFAULTS)
        for k, v in kw.items():
            assert k in self.s, k
            self.s[k] = v
        self.hist = None
        self.aux = None  # mean of max(e, 0)^2, mean of max(-e, 0)^2, distance

    def step(self, color, mv, dist, jitter, fresh=False):
        """fresh [fix101]: the statistics start from zero in this frame and the depth of the frame before is not there (the
        shader's g_flags 128: the history is another pass's image, see LightTAA::Params::calm_fresh)"""
        s = self.s
        f32 = np.float32
        H, W = color.shape[:2]
        ys, xs = np.mgrid[0:H, 0:W]
        k = 0.5 / (s["filter_radius"] ** 2)
        m1 = 0.0
        m2 = 0.0
        rec = 0.0
        rw = 0.0
        nearest_d = np.full((H, W), 1e30, dtype=f32)
        nx = xs.copy()
        ny = ys.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                c = R.to_ycocg(R.load(color, xs + dx, ys + dy))
                m1 = m1 + c
                m2 = m2 + c * c
                w = np.exp(-k * ((dx - jitter[0]) ** 2 + (dy - jitter[1]) ** 2))
                rec = rec + c * w
                rw += w
                d = R.load(dist, xs + dx, ys + dy)
                closer = d < nearest_d
                nearest_d = np.where(closer, d, nearest_d)
                nx = np.where(closer, np.clip(xs + dx, 0, W - 1), nx)
                ny = np.where(closer, np.clip(ys + dy, 0, H - 1), ny)
        own = R.to_ycocg(color)
        cur = rec / rw
        mean = m1 / 9.0
        sig = np.sqrt(np.maximum(m2 / 9.0 - mean * mean, 0.0))
        e = own[..., 0] - mean[..., 0]
        mvn = mv[ny, nx]
        prev_x = xs + 0.5 + mvn[..., 0]
        prev_y = ys + 0.5 + mvn[..., 1]
        valid = (prev_x >= 0) & (prev_x <= W) & (prev_y >= 0) & (prev_y <= H)
        if self.hist is None:
            valid &= False
            self.hist = np.zeros((H, W, 3), dtype=f32)
            self.aux = np.zeros((H, W, 3), dtype=f32)
        st = R.bilinear(self.aux, prev_x, prev_y) * (0.0 if fresh else 1.0)
        up_prev = st[..., 0]
        dn_prev = np.maximum(st[..., 1], 0.0)
        released = np.zeros((H, W), dtype=bool)
        if s["depth_reject"] > 0 and not fresh:
            pd = R.gather4(self.aux[..., 2], prev_x, prev_y)
            released = pd.max(axis=-1) < nearest_d * (1.0 - s["depth_reject"])
        sd_up = np.sqrt(np.maximum(up_prev, 0.0))
        sd_dn = np.sqrt(dn_prev)
        sde = np.maximum(sd_up, sd_dn)
        protect = np.where(released, 0.0, smooth01((sde - s["t0"]) / (s["t1"] - s["t0"])))
        h = R.to_ycocg(np.maximum(R.catmull_rom5(self.hist, prev_x, prev_y), 0.0))
        conf = smooth01(np.abs(mean[..., 0] - h[..., 0]) / (s["follow"] * (sig[..., 0] / 3.0 + sde) + s["follow_b0"]) - 1.0) if s["follow"] > 0 else 0.0
        protect = protect * (1.0 - conf)
        base = s["clip_gamma"] * sig
        far = (np.abs(h[..., 0] - mean[..., 0]) - (base[..., 0] + s["relax"] * sde * protect)) > s["over"]
        released = released | far
        protect = np.where(far, 0.0, protect)
        rl = s["relax"] * sde * protect
        lo = mean - base - np.stack([s["relax"] * sd_dn * protect, 0.5 * rl, 0.5 * rl], axis=-1)
        hi = mean + base + np.stack([s["relax"] * sd_up * protect, 0.5 * rl, 0.5 * rl], axis=-1)
        hc = R.clip_to_box(lo, hi, h)
        dd = cur - hc
        cl = hc + dd * np.minimum(1.0, np.maximum(s["wins"] * sde, s["wins_floor"]) / np.maximum(np.abs(dd[..., 0]), 1e-6))[..., None]
        cb = cur + (cl - cur) * protect[..., None]
        pixels = np.sqrt(mvn[..., 0] ** 2 + mvn[..., 1] ** 2)
        fb = s["feedback"] * (1.0 - s["motion_reduce"] * np.clip(pixels / 24.0, 0, 1))
        fb = 1.0 - (1.0 - fb) / (1.0 + (s["mem"] - 1.0) * protect)
        fb = fb + (np.minimum(fb, s["follow_short"]) - fb) * conf
        if s["luma_weight"]:
            wc = (1.0 - fb) / (1.0 + np.maximum(cb[..., 0], 0.0))
            wh = fb / (1.0 + np.maximum(hc[..., 0], 0.0))
            blended = (cb * wc[..., None] + hc * wh[..., None]) / np.maximum(wc + wh, 1e-6)[..., None]
        else:
            blended = cb + (hc - cb) * fb[..., None]
        result = np.where(valid[..., None], blended, cur)
        r = s["rate"]
        fresh = ~valid | released
        ep = np.maximum(e, 0.0)
        en = np.maximum(-e, 0.0)
        up_new = np.where(fresh, 0.0, up_prev + r * (ep * ep - up_prev))
        dn_new = np.where(fresh, 0.0, dn_prev + r * (en * en - dn_prev))
        self.hist = np.maximum(R.from_ycocg(result), 0.0).astype(f32)
        self.aux = np.stack([up_new, dn_new, dist], axis=-1).astype(f32)
        if s["half"]:
            self.hist = self.hist.astype(np.float16).astype(f32)
            self.aux = self.aux.astype(np.float16).astype(f32)
        self.debug = dict(protect=np.where(valid, protect, 0.0), sde=sde, released=released & valid)
        return self.hist
