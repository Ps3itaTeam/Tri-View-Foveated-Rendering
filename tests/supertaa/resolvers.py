"""numpy models of the mod's fovea TAA (OldTAA: FoveaComposite.cpp, FOVEA_TAA_SHADER) and of the mod's supersampling TAA
(NewTAA: src/mods/vr/d3d12/SuperTAA.cpp).

NewTAA is written pass by pass like the HLSL (analyze -> accumulate -> resolve) and is its reference: the GPU test
(gpu/supertaa_test.cpp, run_cases.sh) compares what the shaders write with it, and analyze_capture.py replays a
capture from the game with it. A change of a shader goes here too, and the other way round.

[fix77] One test sets the memory of every pixel: the low-passed frame against the low-passed history ("follow", see
NewTAA.analyze). [fix78] fix76's protection of what one frame's samples cannot show (clip widened by the deviation of the
pixel's sample against its 3x3 mean, the sample held back, more weight) is back: without it thin bright edges and the
engine's stochastic shadows flickered. What it had kept wrongly in the game (text that scrolled, the wall behind a
chair, a highlight on an edge) it lets go now: it is released where that test sees a change, and where a point leaves or
enters the rim of a nearer thing that moves against what is behind it. [fix79] Things that arrive: a fly moves several
pixels a frame and its motion vectors do not say so; its samples were taken for scatter and what it left was protected
(a dotted trail). Where something arrives that was not there a frame ago (nearer than anything the last frame held within
3 pixels, and a thing, not a line) nothing is protected for a few frames and the statistics stand still.
"""
import numpy as np


# ---- texture access as a GPU does it (clamp addressing, texel centres at +0.5) ---------------------------------
def load(img, x, y):
    h, w = img.shape[:2]
    return img[np.clip(y, 0, h - 1), np.clip(x, 0, w - 1)]


# GPUs filter with fixed-point weights (D3D: 8 fractional bits); None = exact
BILINEAR_BITS = None


def bilinear(img, px, py):
    """sample at continuous texel coordinates (px, py); pixel q's centre is q + 0.5"""
    h, w = img.shape[:2]
    x = px - 0.5
    y = py - 0.5
    if BILINEAR_BITS is not None:
        q = float(1 << BILINEAR_BITS)
        x = np.floor(np.asarray(x, dtype=np.float32) * q + 0.5) / q
        y = np.floor(np.asarray(y, dtype=np.float32) * q + 0.5) / q
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]
    a = load(img, x0, y0)
    b = load(img, x0 + 1, y0)
    c = load(img, x0, y0 + 1)
    d = load(img, x0 + 1, y0 + 1)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def gather4(img, px, py):
    """the four texels a bilinear tap at (px, py) reads"""
    x0 = np.floor(px - 0.5).astype(np.int64)
    y0 = np.floor(py - 0.5).astype(np.int64)
    return np.stack([load(img, x0, y0), load(img, x0 + 1, y0), load(img, x0, y0 + 1), load(img, x0 + 1, y0 + 1)], axis=-1)


def catmull_rom5(img, px, py, extra=0.0, dering=False):
    """the 5 bilinear taps of history_catmull_rom (FoveaComposite.cpp); returns rgb.
    extra: more of the difference to the plain bilinear tap (0 = Catmull-Rom); dering: the result kept inside the
    range of the four texels around the position"""
    c_x = np.floor(px - 0.5) + 0.5
    c_y = np.floor(py - 0.5) + 0.5
    fx = (px - c_x).astype(np.float32)
    fy = (py - c_y).astype(np.float32)

    def wts(f):
        w0 = f * (-0.5 + f * (1.0 - 0.5 * f))
        w1 = 1.0 + f * f * (-2.5 + 1.5 * f)
        w2 = f * (0.5 + f * (2.0 - 1.5 * f))
        w3 = f * f * (-0.5 + 0.5 * f)
        return w0, w1, w2, w3

    wx0, wx1, wx2, wx3 = wts(fx)
    wy0, wy1, wy2, wy3 = wts(fy)
    wx12 = wx1 + wx2
    wy12 = wy1 + wy2
    ox12 = wx2 / wx12
    oy12 = wy2 / wy12
    t0x, t0y = c_x - 1.0, c_y - 1.0
    t3x, t3y = c_x + 2.0, c_y + 2.0
    t12x, t12y = c_x + ox12, c_y + oy12
    rgb = img[..., :3]
    ta = bilinear(rgb, t12x, t0y)
    tb = bilinear(rgb, t0x, t12y)
    centre = bilinear(rgb, t12x, t12y)
    td = bilinear(rgb, t3x, t12y)
    te = bilinear(rgb, t12x, t3y)
    r = ta * (wx12 * wy0)[..., None]
    r = r + tb * (wx0 * wy12)[..., None]
    r = r + centre * (wx12 * wy12)[..., None]
    r = r + td * (wx3 * wy12)[..., None]
    r = r + te * (wx12 * wy3)[..., None]
    wsum = wx12 * wy0 + wx0 * wy12 + wx12 * wy12 + wx3 * wy12 + wx12 * wy3
    r = r / wsum[..., None]
    plain = None
    if extra != 0.0 or dering == 2:
        plain = bilinear(rgb, px, py)
    if extra != 0.0:
        r = r + extra * (r - plain)
    if dering == 2:
        # the range of the taps already read (no further reads)
        lo = np.minimum(np.minimum(np.minimum(ta, tb), np.minimum(td, te)), np.minimum(centre, plain))
        hi = np.maximum(np.maximum(np.maximum(ta, tb), np.maximum(td, te)), np.maximum(centre, plain))
        r = np.clip(r, lo, hi)
    elif dering == 3:
        # the green of the 4 texels around bounds the result's green; the other channels scale with it
        g = gather4(rgb[..., 1], px, py)
        glo = g.min(axis=-1)
        ghi = g.max(axis=-1)
        gg = r[..., 1]
        k = np.where(gg > ghi, ghi / np.maximum(gg, 1e-6), np.where(gg < glo, glo / np.maximum(gg, 1e-6), 1.0))
        k = np.clip(k, 0.0, 4.0)
        r = r * k[..., None]
    elif dering:
        g = gather4(rgb, px, py)  # [..., 3, 4]
        r = np.clip(r, g.min(axis=-1), g.max(axis=-1))
    return np.maximum(r, 0.0)


def to_ycocg(c):
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    return np.stack([0.25 * r + 0.5 * g + 0.25 * b, 0.5 * r - 0.5 * b, -0.25 * r + 0.5 * g - 0.25 * b], axis=-1)


def from_ycocg(c):
    t = c[..., 0] - c[..., 2]
    return np.stack([t + c[..., 1], c[..., 0] + c[..., 2], t - c[..., 1]], axis=-1)


def clip_to_box(lo, hi, h):
    centre = 0.5 * (hi + lo)
    extent = 0.5 * (hi - lo) + 1e-4
    v = h - centre
    a = np.abs(v / extent)
    m = a.max(axis=-1, keepdims=True)
    return np.where(m > 1.0, centre + v / np.maximum(m, 1e-9), h)


def srgb_encode(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 1e-12), 1.0 / 2.4) - 0.055).astype(np.float32)


def srgb_decode(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4)).astype(np.float32)


def rcas(img, sharp):
    """RCAS as in FOVEA_TAA_RESOLVE_SHADER (on the image as stored, clamped addressing)"""
    if sharp <= 0.0:
        return np.maximum(img, 0.0)
    h, w = img.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w]
    e = img
    b = np.clip(load(img, xs, ys - 1), 0, 1)
    d = np.clip(load(img, xs - 1, ys), 0, 1)
    f = np.clip(load(img, xs + 1, ys), 0, 1)
    hh = np.clip(load(img, xs, ys + 1), 0, 1)
    ec = np.clip(e, 0, 1)
    mn4 = np.minimum(np.minimum(b, d), np.minimum(f, hh))
    mx4 = np.maximum(np.maximum(b, d), np.maximum(f, hh))
    hit_min = np.minimum(mn4, ec) / (4.0 * mx4 + 1e-5)
    hit_max = (1.0 - np.maximum(mx4, ec)) / np.minimum(4.0 * mn4 - 4.0, -1e-5)
    lobe3 = np.maximum(-hit_min, hit_max)
    lobe = np.maximum(-0.1875, np.minimum(lobe3.max(axis=-1), 0.0)) * sharp
    lobe = lobe[..., None]
    out = (lobe * (b + d + f + hh) + ec) / (4.0 * lobe + 1.0)
    return np.maximum(out, 0.0)


# ================================================================================================================
# the mod's fovea TAA as it is (accumulate + resolve), on the image as the engine stores it (encoded)
# ================================================================================================================
class OldTAA:
    def __init__(self, feedback=0.90, clip_gamma=1.0, filter_radius=0.40, new_softness=0.5, motion_reduce=0.25, depth_reject=0.10,
                 sharpen=0.30, dilate=True, catmull=True, luma_weight=True, fast_converge=True):
        self.s = dict(feedback=feedback, clip_gamma=clip_gamma, filter_radius=filter_radius, new_softness=new_softness,
                      motion_reduce=motion_reduce, depth_reject=depth_reject, sharpen=sharpen, dilate=dilate, catmull=catmull,
                      luma_weight=luma_weight, fast_converge=fast_converge)
        self.hist = None
        self.pdist = None

    def step(self, color, mv, dist, jitter):
        s = self.s
        H, W = color.shape[:2]
        ys, xs = np.mgrid[0:H, 0:W]
        sigma = float(np.clip(s["filter_radius"], 0, 1))
        sigma_new = sigma + (max(1.0, sigma) - sigma) * s["new_softness"]
        k = 0.5 / (sigma * sigma) if sigma > 0 else 0.0
        k_new = 0.5 / (sigma_new * sigma_new) if sigma_new > 0 else 0.0
        m1 = 0.0
        m2 = 0.0
        rec = 0.0
        rec_new = 0.0
        rw = 0.0
        rnw = 0.0
        nearest_d = np.full((H, W), 1e30, dtype=np.float32)
        nx = xs.copy()
        ny = ys.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                c = to_ycocg(load(color, xs + dx, ys + dy))
                m1 = m1 + c
                m2 = m2 + c * c
                ddx = dx - jitter[0]
                ddy = dy - jitter[1]
                w = np.exp(-k * (ddx * ddx + ddy * ddy)) if sigma > 0 else (1.0 if (dx == 0 and dy == 0) else 0.0)
                wn = np.exp(-k_new * (ddx * ddx + ddy * ddy)) if sigma_new > 0 else (1.0 if (dx == 0 and dy == 0) else 0.0)
                rec = rec + c * w
                rw += w
                rec_new = rec_new + c * wn
                rnw += wn
                if s["dilate"]:
                    d = load(dist, xs + dx, ys + dy)
                    closer = d < nearest_d  # nearest surface (the shader compares depth values)
                    nearest_d = np.where(closer, d, nearest_d)
                    nx = np.where(closer, np.clip(xs + dx, 0, W - 1), nx)
                    ny = np.where(closer, np.clip(ys + dy, 0, H - 1), ny)
        own = to_ycocg(color)
        cur = rec / rw if rw > 1e-6 else own
        cur_new = rec_new / rnw if rnw > 1e-6 else own
        mean = m1 / 9.0
        sig = np.sqrt(np.maximum(m2 / 9.0 - mean * mean, 0.0))
        lo = mean - s["clip_gamma"] * sig
        hi = mean + s["clip_gamma"] * sig
        mvn = mv[ny, nx]
        prev_x = xs + 0.5 + mvn[..., 0]
        prev_y = ys + 0.5 + mvn[..., 1]
        dist_moved = nearest_d if s["dilate"] else dist
        valid = (prev_x >= 0) & (prev_x <= W) & (prev_y >= 0) & (prev_y <= H)
        if self.hist is None:
            valid &= False
            self.hist = np.zeros((H, W, 4), dtype=np.float32)
            self.pdist = np.zeros((H, W), dtype=np.float32)
        if s["depth_reject"] > 0:
            pd = gather4(self.pdist, prev_x, prev_y)
            valid &= pd.max(axis=-1) >= dist_moved * (1.0 - s["depth_reject"])
        result = cur_new.copy()
        age = np.ones((H, W), dtype=np.float32)
        hb = bilinear(self.hist, prev_x, prev_y)
        hist_rgb = catmull_rom5(self.hist, prev_x, prev_y) if s["catmull"] else hb[..., :3]
        h = clip_to_box(lo, hi, to_ycocg(hist_rgb))
        pixels = np.sqrt(mvn[..., 0] ** 2 + mvn[..., 1] ** 2)
        fb = s["feedback"] * (1.0 - s["motion_reduce"] * np.clip(pixels / 24.0, 0, 1))
        if s["fast_converge"]:
            fb = np.minimum(fb, hb[..., 3] / (hb[..., 3] + 1.0))
        if s["luma_weight"]:
            wc = (1.0 - fb) / (1.0 + np.maximum(cur[..., 0], 0.0))
            wh = fb / (1.0 + np.maximum(h[..., 0], 0.0))
            blended = (cur * wc[..., None] + h * wh[..., None]) / np.maximum(wc + wh, 1e-6)[..., None]
        else:
            blended = cur + (h - cur) * fb[..., None]
        result = np.where(valid[..., None], blended, result)
        age = np.where(valid, np.minimum(hb[..., 3] + 1.0, 64.0), age)
        self.hist = np.concatenate([np.maximum(from_ycocg(result), 0.0), age[..., None]], axis=-1).astype(np.float32)
        self.pdist = dist.astype(np.float32)
        return rcas(self.hist[..., :3], s["sharpen"])


# ================================================================================================================
# the new resolver
# ================================================================================================================
def lanczos2(x):
    x = np.abs(x)
    out = np.sinc(x) * np.sinc(x / 2.0)
    return np.where(x < 2.0, out, 0.0)


def smooth01(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


class NewTAA:
    DEFAULTS = dict(
        scale=2,              # history texels per output pixel and axis (1 or 2)
        out_ratio=1.0,        # output pixels per input pixel
        sigma=0.25,           # rebuild of the current frame: Gaussian sigma in input pixels
        w_max=5.0,            # accumulated weight a settled history holds (higher = steadier, slower)
        clip_gamma=1.0,       # neighbourhood clip: mean +- gamma * sigma of this frame's 3x3
        motion_reduce=0.25,
        depth_reject=0.10,
        sharpen=0.30,
        luma_weight=True,
        hist_extra=0.5,       # sharper than Catmull-Rom history resampling
        hist_dering=True,     # True = range of the 4 texels around, 2 = range of the taps, 3 = the 4 texels' green only
        cur_nearest=False,    # the current frame from the nearest sample only (else the 2 x 2 around, Gaussian)
        down='box',           # 2 : 1 resolve: 'lanczos2' or 'box'
        linear=True,          # accumulate in linear light
        perceptual_clip=True, # clip in sqrt space
        # following changes: d = lowpass(frame) - lowpass(history) at the same places (3x3 samples each, without the largest
        # and the smallest), b = its fast mean, m = the mean of |d - b| (noise)
        follow=True,
        fl_k=2.0, fl_b0=0.02,     # a change at once: fl_alln of the 9 samples differ from the history the same way by more
        fl_alln=8,                # than k * noise + b0 (a light, a shadow, a surface: not a spark, not a thin line)
        fl_kc=1.5, fl_b0c=0.012,  # a change confirmed: d two frames in a row the same way, over kc * noise + b0c, and
        fl_count=5,               # at least fl_count of the 9 samples moved that way (a line thinner than a pixel moves 3)
        fl_cap=0.04,              # the noise believed at most (stochastic shadows; more than this is a picture that changes)
        fl_rate=0.125, fl_rate_b=0.5,
        w_short=0.5,              # the weight the history keeps where the picture changed
        fl_sde=0.7,               # both bands at least this times the samples' own scatter (the deviation of e)
        # protection of what one frame's samples cannot show: e = the pixel's sample minus the mean of its 3x3, its mean
        # and variance over time (along the motion); released where the history is stale
        protect=True,
        af_rate=0.125,            # rate of the mean and the variance of e
        af_t0=0.01, af_t1=0.05,   # deviation of e from which / at which the protection is full
        af_relax=1.5,             # the clip box wider by this times the deviation
        af_wins=2.5,              # the frame's sample no farther from the history than this times the deviation (0 = off)
        af_wins_floor=0.02,
        af_mem=1.5,               # the history's weight limit times this where it is protected
        edge0=0.08, edge1=0.25,   # change of (fetch motion - own motion) from one frame to the next, px: released
        rel_decay=0.8,            # what is left of a release a frame later
        # a thing that was not there a frame ago (a fly: it moves, its motion vectors do not): its samples are not scatter,
        # and what it leaves behind is not protected
        arrive=True,
        arrive_count=4,           # ... a thing, not a line: this many samples that near in some 3x3 window around the pixel
        mark_level=2.0,           # the release where it arrived (above 1: the statistics stand still while it is above 1)
        mark_spread=1.6,          # ... and, a frame later, in what was within a pixel and a half of it
        # [fix81] one sample far off what the place has shown so far is not yet scatter (a fly of one or two pixels crossing
        # it): for the scatter's statistics it counts no farther from their mean than af_clip times the deviation so far plus
        # af_clip_floor. With `protect` and `arrive` (0 = off: fix80).
        af_clip=8.0,
        af_clip_floor=0.03,
    )

    def __init__(self, **kw):
        self.s = dict(self.DEFAULTS)
        for k_, v in kw.items():
            if k_ not in self.s:
                raise KeyError(k_)
            self.s[k_] = v
        self.hist = None
        self.pdist = None
        self.stats = None  # [h, w, 4]: noise m, fast mean b, mean and variance of e
        self.rel = None    # [h, w]: the protection's release (1 = none; above 1: a thing arrived, see analyze)
        self.offs = None   # [h, w, 2]: the motion that fetched the history minus the pixel's own
        self.debug = {}
        self.half = False  # images kept as the GPU keeps them (16-bit floats)
        # A part of the images only (the replay of a capture from the game, analyze_capture.py): the arrays hold the part
        # whose corner is in_origin (view pixels) / hist_origin (history texels) of images of the size view / hist_full;
        # hist_part is the history part's size. None: the arrays are the whole images.
        self.view = None
        self.in_origin = (0, 0)
        self.hist_full = None
        self.hist_origin = (0, 0)
        self.hist_part = None

    def store(self, a):
        return a.astype(np.float16).astype(np.float32) if self.half else a.astype(np.float32)

    # perceptual encoding of linear values for clip and statistics
    @staticmethod
    def enc(c):
        return np.sqrt(np.maximum(c, 0.0))

    @staticmethod
    def distance_of(z, depth_k):
        """metres from a depth value as the shader computes it (limited to what the 16-bit distance image holds)"""
        k = [np.float32(v) for v in depth_k]
        z = z.astype(np.float32)
        den = k[2] * z + k[3]
        with np.errstate(divide="ignore", invalid="ignore"):
            d = np.where(np.abs(den) > 1e-12, np.abs((k[0] * z + k[1]) / den), np.float32(60000.0))
        return np.minimum(d, np.float32(60000.0)).astype(np.float32)

    def geometry(self, H, W):
        """sizes of the view and the history, and where the arrays' corners lie in them"""
        s = self.s
        if self.hist_full is not None:
            Wv, Hv = self.view
            Whf, Hhf = self.hist_full
            Wh, Hh = self.hist_part
            hx0, hy0 = self.hist_origin
            ix0, iy0 = self.in_origin
        else:
            Wo = int(round(W * s["out_ratio"]))
            Ho = int(round(H * s["out_ratio"]))
            Wh, Hh = Wo * s["scale"], Ho * s["scale"]
            Wv, Hv, Whf, Hhf = W, H, Wh, Hh
            hx0 = hy0 = ix0 = iy0 = 0
        return Wv, Hv, Whf, Hhf, Wh, Hh, hx0, hy0, ix0, iy0

    def state_image(self):
        """the distances image as the pass keeps it: distance, release, fetch motion minus own motion"""
        return np.concatenate([self.pdist[..., None], self.rel[..., None], self.offs], axis=-1)

    def set_state_image(self, img):
        self.pdist = img[..., 0].astype(np.float32).copy()
        self.rel = img[..., 1].astype(np.float32).copy()
        self.offs = img[..., 2:4].astype(np.float32).copy()

    def analyze(self, color, mv, dist, jitter, z=None, depth_k=None, reverse_z=True, reset=False, have_depth=True, dist_test=None):
        """color: the engine's image (encoded); mv: motion in view pixels; dist: distances (m), or None with z: the depth
        values as the engine stores them (the shader picks the nearest of the 3x3 by those) and depth_k; jitter: this
        frame's (the sample of pixel q shows the point at q + 0.5 - jitter)"""
        s = self.s
        H, W = color.shape[:2]
        ys, xs = np.mgrid[0:H, 0:W]
        lin = srgb_decode(color) if s["linear"] else color.astype(np.float32)
        work = self.enc(lin) if s["perceptual_clip"] else lin
        yc = to_ycocg(work)
        m1 = 0.0
        m2 = 0.0
        by_z = z is not None
        if by_z:
            key = z.astype(np.float32) if reverse_z else -z.astype(np.float32)  # larger = nearer
            near_k = np.full((H, W), -np.inf, dtype=np.float32)
        else:
            key = -dist.astype(np.float32)
            near_k = np.full((H, W), -1e30, dtype=np.float32)
        nx = xs.copy()
        ny = ys.copy()
        lum = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                c = load(yc, xs + dx, ys + dy)
                m1 = m1 + c
                m2 = m2 + c * c
                lum.append(c[..., 0])
                if not have_depth:
                    continue
                kq = load(key, xs + dx, ys + dy)
                closer = kq > near_k
                near_k = np.where(closer, kq, near_k)
                nx = np.where(closer, np.clip(xs + dx, 0, W - 1), nx)
                ny = np.where(closer, np.clip(ys + dy, 0, H - 1), ny)
        if by_z:
            dist = self.distance_of(z, depth_k)
            nearest_d = self.distance_of(z[ny, nx], depth_k)
        else:
            nearest_d = dist[ny, nx].astype(np.float32)
        mean = m1 / 9.0
        sig = np.sqrt(np.maximum(m2 / 9.0 - mean * mean, 0.0))
        mvd = mv[ny, nx].astype(np.float32)
        prev_x = xs + 0.5 + mvd[..., 0]
        prev_y = ys + 0.5 + mvd[..., 1]
        Wv, Hv, Whf, Hhf, Wh, Hh, hx0, hy0, ix0, iy0 = self.geometry(H, W)
        valid = (prev_x + ix0 >= 0) & (prev_x + ix0 <= Wv) & (prev_y + iy0 >= 0) & (prev_y + iy0 <= Hv)
        if self.pdist is None:
            valid &= False
            self.pdist = np.zeros((H, W), dtype=np.float32)
        if self.stats is None:
            self.stats = np.zeros((H, W, 4), dtype=np.float32)
        if self.rel is None:
            self.rel = np.zeros((H, W), dtype=np.float32)
        if self.offs is None:
            self.offs = np.zeros((H, W, 2), dtype=np.float32)
        if self.hist is None:
            valid &= False
        if reset:
            valid &= False

        # the motion that fetches the history against the pixel's own: 0 on a surface, the difference of the two motions
        # in the rim of a nearer thing. Where it changes from one frame to the next the point left such a rim or entered one
        offs = mvd - mv.astype(np.float32)
        rel_prev = bilinear(self.rel[..., None], prev_x, prev_y)[..., 0]
        doff = np.abs(offs - bilinear(self.offs, prev_x, prev_y)).max(axis=-1)
        medge = np.clip((doff - s["edge0"]) / max(s["edge1"] - s["edge0"], 1e-4), 0.0, 1.0)

        if dist_test is None:
            dist_test = s["depth_reject"] > 0
        arrive = np.zeros((H, W), dtype=bool)
        fresh = np.zeros((H, W), dtype=bool)
        if dist_test:
            pd = gather4(self.pdist, prev_x, prev_y)
            valid &= pd.max(axis=-1) >= nearest_d * (1.0 - np.float32(s["depth_reject"]))
            if s["arrive"]:
                # ---- a thing that was not there: the nearest surface of the 3x3 is nearer than anything the last frame held
                #      within 3 pixels of the place the history comes from (the 6x6 block of its distances: an edge or a wire
                #      a pixel wide that the jitter moves by a pixel is still inside it), and it is a thing, not a line: some
                #      3x3 window whose centre is at most a pixel away holds arrive_count samples that near (a line a pixel
                #      wide, or thinner and hit here and there, never has more than 3). --------------------------------------
                keep = np.float32(1.0) - np.float32(s["depth_reject"])
                near_prev = pd.min(axis=-1)
                for oy in (-2, 0, 2):
                    for ox in (-2, 0, 2):
                        if ox or oy:
                            near_prev = np.minimum(near_prev, gather4(self.pdist, prev_x + ox, prev_y + oy).min(axis=-1))
                thr = near_prev * keep
                that_near = {}
                for dy in (-2, -1, 0, 1, 2):
                    for dx in (-2, -1, 0, 1, 2):
                        that_near[(dx, dy)] = (load(dist, xs + dx, ys + dy) < thr).astype(np.float32)
                most = np.zeros((H, W), dtype=np.float32)
                for cy in (-1, 0, 1):
                    for cx in (-1, 0, 1):
                        most = np.maximum(most, sum(that_near[(cx + dx, cy + dy)] for dy in (-1, 0, 1) for dx in (-1, 0, 1)))
                fresh = valid & (nearest_d < thr)
                arrive = fresh & (most >= s["arrive_count"])
        else:
            dist = np.zeros((H, W), dtype=np.float32)  # (the shader writes 0 without the test)

        # ---- what one frame's samples cannot show: e = the pixel's sample against the mean of its 3x3, over time ---------
        st = bilinear(self.stats, prev_x, prev_y)
        e = yc[..., 0] - mean[..., 0]
        mu_e = np.where(valid, st[..., 2], e)
        var_e = np.where(valid, st[..., 3], 0.0) * (1.0 - medge)
        sde = np.sqrt(np.maximum(var_e, 0.0))

        # ---- following changes: does the history still show, on the whole, what this frame shows around the pixel? -------
        d = np.zeros((H, W), dtype=np.float32)
        conf = np.zeros((H, W), dtype=np.float32)
        m_new = np.zeros((H, W), dtype=np.float32)
        b_new = np.zeros((H, W), dtype=np.float32)
        if s["follow"] and self.hist is not None:
            sx = Whf / Wv
            sy = Hhf / Hv
            hls = []
            hw = None
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    # the place the neighbour's sample shows, a frame before (with this pixel's motion)
                    hx = (xs + dx + 0.5 - jitter[0] + ix0 + mvd[..., 0]) * sx - hx0
                    hy = (ys + dy + 0.5 - jitter[1] + iy0 + mvd[..., 1]) * sy - hy0
                    hq = bilinear(self.hist, hx, hy)
                    hls.append(to_ycocg(self.enc(hq[..., :3]) if s["perceptual_clip"] else hq[..., :3])[..., 0])
                    if dx == 0 and dy == 0:
                        hw = hq[..., 3]
            lp_cur = (sum(lum) - np.maximum.reduce(lum) - np.minimum.reduce(lum)) / 7.0
            lp_his = (sum(hls) - np.maximum.reduce(hls) - np.minimum.reduce(hls)) / 7.0
            fvalid = valid & (hw > 1e-3)
            d = np.where(fvalid, lp_cur - lp_his, 0.0)
            m_prev = np.where(fvalid, st[..., 0], 0.0)
            b_prev = np.where(fvalid, st[..., 1], 0.0)
            noise = np.minimum(m_prev, s["fl_cap"])
            # (never narrower than the samples' own scatter: a stochastic shadow is not a change)
            band_i = np.maximum(s["fl_k"] * noise + s["fl_b0"], s["fl_sde"] * sde)
            band_c = np.maximum(s["fl_kc"] * noise + s["fl_b0c"], s["fl_sde"] * sde)
            dqs = [lum[i] - hls[i] for i in range(9)]
            n_up = sum((dq > band_i).astype(np.float32) for dq in dqs)
            n_dn = sum((dq < -band_i).astype(np.float32) for dq in dqs)
            at_once = np.clip(np.maximum(n_up, n_dn) - (s["fl_alln"] - 1), 0.0, 1.0)
            ad = np.abs(d)
            agree = (d * b_prev) > 0.0
            a = np.where(agree, np.minimum(ad, np.abs(b_prev) / s["fl_rate_b"]), 0.0)
            sgn = np.where(d >= 0.0, 1.0, -1.0)
            cnt = sum(((dq * sgn) > band_c).astype(np.float32) for dq in dqs)
            conf = smooth01(a / band_c - 1.0) * np.clip(cnt - (s["fl_count"] - 1), 0.0, 1.0)
            conf = np.where(fvalid, np.maximum(conf, at_once), 0.0)
            m_new = np.where(fvalid, m_prev + s["fl_rate"] * (np.minimum(np.abs(d - b_prev), 2.0 * s["fl_cap"]) - m_prev), 0.0)
            b_new = np.where(fvalid, b_prev + s["fl_rate_b"] * (d - b_prev), 0.0)

        # ---- the release of the protection (1 = none): a change seen now, a rim left or entered (0.9), what is left of
        #      earlier releases, and, at 0.8, a change seen a frame ago within about a pixel and a half (only that
        #      spreads: the other kinds stay at or below 0.9). A pixel without history starts released.
        #      Where a thing arrived the release is set above 1 (mark_level), and a frame later what was within a pixel and a
        #      half of it gets mark_spread (that level is handed on once); both fade like any release. Above 1 the
        #      statistics stand still: what such a thing does to the samples, in its pixels and around them, is not scatter. -
        around = np.zeros((H, W), dtype=np.float32)
        for oy in (-1, 1):
            for ox in (-1, 1):
                around = np.maximum(around, gather4(self.rel, prev_x + ox, prev_y + oy).max(axis=-1))
        spread = 0.8 * np.clip((around - 0.9) / 0.1, 0.0, 1.0)
        rel = np.maximum(np.maximum(conf, 0.9 * medge), np.maximum(rel_prev * s["rel_decay"], spread))
        if s["arrive"]:
            rel = np.maximum(rel, np.where(around > 1.9, s["mark_spread"], 0.0))
            rel = np.where(arrive, s["mark_level"], rel)
        rel = np.where(valid, np.clip(rel, 0.0, s["mark_level"]), 0.9)
        still = rel > 1.0
        pr = np.clip((sde - s["af_t0"]) / (s["af_t1"] - s["af_t0"]), 0.0, 1.0)
        pr = pr * pr * (3.0 - 2.0 * pr)
        protect = pr * (1.0 - np.minimum(rel, 1.0)) if s["protect"] else pr * 0.0
        # a change is not scatter, and the mean follows it faster
        r = s["af_rate"]
        de = e - mu_e
        ds = de
        if s["af_clip"] > 0.0 and s["protect"] and s["arrive"]:
            lim = np.float32(s["af_clip"]) * sde + np.float32(s["af_clip_floor"])
            ds = np.clip(de, -lim, lim)
        # (the part of the mean that follows a change takes the whole distance)
        mu_new = np.where(still, mu_e, mu_e + r * ds + (0.5 - r) * conf * de)
        var_new = np.where(still, var_e, (1.0 - r) * (var_e + r * (1.0 - conf) * ds * ds))
        # the weight the history may keep: more where it is protected, next to none where the picture changed
        wlim = s["w_max"] * (1.0 + ((s["af_mem"] if s["protect"] else 1.0) - 1.0) * protect)
        wlim = wlim + (np.minimum(wlim, s["w_short"]) - wlim) * conf
        wlim = np.where(valid, wlim, 0.0)

        self.stats = self.store(np.stack([m_new, b_new, mu_new, var_new], axis=-1))
        self.pdist = self.store(dist)
        self.rel = self.store(rel)
        self.offs = self.store(offs)
        guide = self.store(np.concatenate([mvd, wlim[..., None], protect[..., None]], axis=-1))
        self.debug = dict(valid=valid, d=d, conf=conf, noise=self.stats[..., 0], rel=rel, protect=protect, sde=sde, medge=medge, arrive=arrive, fresh=fresh)
        return self.store(lin), guide, self.store(mean), self.store(s["clip_gamma"] * sig)

    def accumulate(self, lin, guide, boxc, boxe, jitter, reset=False):
        s = self.s
        H, W = lin.shape[:2]
        Wv, Hv, Whf, Hhf, Wh, Hh, hx0, hy0, ix0, iy0 = self.geometry(H, W)
        sx = Whf / Wv
        sy = Hhf / Hv
        if self.hist is None or self.hist.shape[:2] != (Hh, Wh):
            self.hist = np.zeros((Hh, Wh, 4), dtype=np.float32)
        ty, tx = np.mgrid[0:Hh, 0:Wh]
        # the texel's centre in view pixels, counted from the part's corner
        ux = (tx + hx0 + 0.5) / sx - ix0
        uy = (ty + hy0 + 0.5) / sy - iy0
        # the 2 x 2 input samples around the texel: pixel q's sample sits at q + 0.5 - jitter
        qfx = ux - 0.5 + jitter[0]
        qfy = uy - 0.5 + jitter[1]
        q0x = np.floor(qfx).astype(np.int64)
        q0y = np.floor(qfy).astype(np.int64)
        k = 0.5 / (s["sigma"] ** 2)
        acc = 0.0
        wsum = 0.0
        for dy in (0, 1):
            for dx in (0, 1):
                ddx = (q0x + dx + 0.5 - jitter[0]) - ux
                ddy = (q0y + dy + 0.5 - jitter[1]) - uy
                w = np.exp(-k * (ddx * ddx + ddy * ddy)).astype(np.float32)
                acc = acc + load(lin, q0x + dx, q0y + dy) * w[..., None]
                wsum = wsum + w
        cur = acc / np.maximum(wsum, 1e-6)[..., None]
        kappa = np.clip(wsum, 1e-3, 1.0)
        if s["cur_nearest"]:
            qnx = np.floor(qfx + 0.5).astype(np.int64)
            qny = np.floor(qfy + 0.5).astype(np.int64)
            ddx = (qnx + 0.5 - jitter[0]) - ux
            ddy = (qny + 0.5 - jitter[1]) - uy
            cur = load(lin, qnx, qny)
            kappa = np.clip(np.exp(-k * (ddx * ddx + ddy * ddy)), 1e-3, 1.0).astype(np.float32)

        px = np.clip(np.floor(ux).astype(np.int64), 0, W - 1)
        py = np.clip(np.floor(uy).astype(np.int64), 0, H - 1)
        g = guide[py, px]
        prev_fx = (ux + ix0 + g[..., 0]) * sx  # in the whole history
        prev_fy = (uy + iy0 + g[..., 1]) * sy
        valid = (g[..., 2] > 0.0) & (prev_fx >= 0) & (prev_fx <= Whf) & (prev_fy >= 0) & (prev_fy <= Hhf)
        if reset:
            valid &= False
        prev_hx = prev_fx - hx0
        prev_hy = prev_fy - hy0
        # protection, and the scatter of the samples there (this frame's statistics)
        protect = np.where(valid, g[..., 3], 0.0)
        sde = np.sqrt(np.maximum(self.stats[py, px, 3], 0.0))
        hb = bilinear(self.hist, prev_hx, prev_hy)
        h_rgb = catmull_rom5(self.hist, prev_hx, prev_hy, extra=s["hist_extra"], dering=s["hist_dering"])
        # the history has to be within what this frame shows around the texel (perceptual YCoCg); where it is protected,
        # within that plus the scatter the samples have shown there
        bc = bilinear(boxc, ux, uy)
        relax = ((s["af_relax"] if s["protect"] else 0.0) * sde * protect)[..., None] * np.array([1.0, 0.5, 0.5], dtype=np.float32)
        be = bilinear(boxe, ux, uy) + relax
        hw = self.enc(h_rgb) if s["perceptual_clip"] else h_rgb
        hcp = clip_to_box(bc - be, bc + be, to_ycocg(hw))
        hc = np.maximum(from_ycocg(hcp), 0.0)
        h_lin = hc * hc if s["perceptual_clip"] else hc

        # protected: the frame's sample no farther from the history than a few deviations
        if s["protect"] and s["af_wins"] > 0:
            dd = to_ycocg(self.enc(cur) if s["perceptual_clip"] else cur) - hcp
            lim = np.maximum(s["af_wins"] * sde, s["af_wins_floor"])
            cl = np.maximum(from_ycocg(hcp + dd * np.minimum(1.0, lim / np.maximum(np.abs(dd[..., 0]), 1e-6))[..., None]), 0.0)
            cl = cl * cl if s["perceptual_clip"] else cl
            cur_b = cur + (cl - cur) * protect[..., None]
        else:
            cur_b = cur

        pixels = np.sqrt(g[..., 0] ** 2 + g[..., 1] ** 2)
        wmax = g[..., 2] * (1.0 - s["motion_reduce"] * np.clip(pixels / 24.0, 0, 1))
        wacc = np.minimum(hb[..., 3], wmax)
        wacc = np.where(valid, wacc, 0.0)
        alpha = kappa / (wacc + kappa)
        if s["luma_weight"]:
            lc = to_ycocg(cur_b)[..., 0]
            lh = to_ycocg(h_lin)[..., 0]
            wc = alpha / (1.0 + np.maximum(lc, 0.0))
            wh = (1.0 - alpha) / (1.0 + np.maximum(lh, 0.0))
            res = (cur_b * wc[..., None] + h_lin * wh[..., None]) / np.maximum(wc + wh, 1e-6)[..., None]
        else:
            res = h_lin + (cur_b - h_lin) * alpha[..., None]
        res = np.where(valid[..., None], res, cur)
        new_w = np.minimum(wacc + kappa, s["w_max"] * (s["af_mem"] if s["protect"] else 1.0))
        self.hist = self.store(np.concatenate([np.maximum(res, 0.0), new_w[..., None]], axis=-1))
        self.debug.update(kappa=kappa, alpha=alpha, wacc=new_w, prev_hx=prev_hx, prev_hy=prev_hy, hist_valid=valid)
        return self.hist

    def resolve(self):
        s = self.s
        hist = self.hist[..., :3]
        if s["scale"] == 1:
            lin = hist
        else:
            # 2 : 1 with Lanczos 2 (8 x 8 texels, as 4 x 4 bilinear taps in the shader), or the mean of the 2 x 2
            Hh, Wh = hist.shape[:2]
            Ho, Wo = Hh // 2, Wh // 2
            offs = np.array([-3.5, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 3.5])
            w1 = lanczos2(offs / 2.0)
            if s["down"] == "box":
                w1 = np.array([0, 0, 0, 1, 1, 0, 0, 0], dtype=np.float64)
            w1 = w1 / w1.sum()
            # separable: rows then columns (same weights as the 2-D kernel)
            xs_h = np.arange(Wo) * 2 + 1.0
            horiz = 0.0
            for i, o in enumerate(offs):
                horiz = horiz + w1[i] * hist[:, np.clip(np.floor(xs_h + o).astype(np.int64), 0, Wh - 1)]
            ys_h = np.arange(Ho) * 2 + 1.0
            lin = 0.0
            for i, o in enumerate(offs):
                lin = lin + w1[i] * horiz[np.clip(np.floor(ys_h + o).astype(np.int64), 0, Hh - 1)]
            lin = np.maximum(lin, 0.0)
        enc = srgb_encode(lin) if s["linear"] else np.clip(lin, 0, 1)
        if s["sharpen"] > 0.0:
            enc = self.store(enc)  # (the GPU writes it into a 16-bit float image between the two passes)
        return rcas(enc, s["sharpen"])

    def step(self, color, mv, dist, jitter):
        lin, guide, boxc, boxe = self.analyze(color, mv, dist, jitter)
        self.accumulate(lin, guide, boxc, boxe, jitter)
        return self.resolve()
