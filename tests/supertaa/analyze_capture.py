"""Replays and measures a capture of the mod's supersampling TAA (quadview_dump/supertaa_captureNN).

    python3 analyze_capture.py <capture folder> [--out <folder for the pictures>] [--set key=value ...] [--bits 8]

What it does:
  1. replay: the captured frames run through the reference model (resolvers.NewTAA, the model the shaders are tested
     against) from the captured state, with the constants each pass got; its images are compared with the game's.
     This tells whether the pass in the game does what the model does (formats, jitter, motion, depth conventions).
     A capture written by fix76 or fix77 (other statistics, other constants) is not compared: its frames run through
     this version's model, with this version's settings, from the captured history instead ("this version"), so an old
     capture shows what this version does with the very frames that went wrong.
     A capture written by fix78 (or with "no trails behind things that jump" off) is compared as it is, and its frames
     also run through the model with that test on ("this version"): what fix79 does with the flies of those frames.
  2. checks of the inputs: how far the image really moved between frames against what jitter and motion vectors say.
  3. measures: detail and flicker (along the motion) of the supersampling TAA, of the mod's TAA of the same frames and
     of the raw frames; and what is left of older frames where the picture has changed since (ghosting).
  4. pictures (PNG): the frames side by side, where each anti-aliasing flickers, where the pass followed a change.
  5. --set: the same frames through the model with other settings (w_max=8, w_short=1, fl_cap=0.02, ...): what a
     change of a setting would do to these frames (numbers and pictures get a column "what if").
"""
import math
import os
import struct
import sys
import zlib

import numpy as np

import resolvers as R
from capture_io import Capture


# ---- small tools -----------------------------------------------------------------------------------------------------
def luma(rgb):
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]).astype(np.float32)


def write_png(path, img):
    """img: [h, w, 3] or [h, w] floats 0..1"""
    a = np.clip(np.asarray(img, dtype=np.float32), 0.0, 1.0)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=-1)
    b = (a * 255.0 + 0.5).astype(np.uint8)
    h, w = b.shape[:2]
    raw = b"".join(b"\x00" + b[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def zoom(img, k):
    return np.repeat(np.repeat(img, k, axis=0), k, axis=1)


def heat(v, top):
    """0..top -> black, red, yellow, white"""
    t = np.clip(v / max(top, 1e-9), 0.0, 1.0)
    return np.stack([np.clip(t * 3.0, 0, 1), np.clip(t * 3.0 - 1.0, 0, 1), np.clip(t * 3.0 - 2.0, 0, 1)], axis=-1)


def side_by_side(images, gap=4):
    h = max(i.shape[0] for i in images)
    out = []
    for k, i in enumerate(images):
        if i.ndim == 2:
            i = np.repeat(i[..., None], 3, axis=-1)
        pad = np.zeros((h - i.shape[0], i.shape[1], 3), dtype=np.float32)
        out.append(np.concatenate([i, pad], axis=0))
        if k + 1 < len(images):
            out.append(np.full((h, gap, 3), 0.25, dtype=np.float32))
    return np.concatenate(out, axis=1)


def erode(mask, r):
    out = mask.copy()
    for _ in range(r):
        m = out.copy()
        m[1:] &= out[:-1]
        m[:-1] &= out[1:]
        m[:, 1:] &= out[:, :-1]
        m[:, :-1] &= out[:, 1:]
        m[0] = m[-1] = False
        m[:, 0] = m[:, -1] = False
        out = m
    return out


def lanczos_at(img, x, y, a=3):
    """img [h, w] sampled at continuous index positions (pixel centres at whole numbers), Lanczos a, clamped"""
    h, w = img.shape
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    acc = np.zeros(x.shape, dtype=np.float64)
    wsum = np.zeros(x.shape, dtype=np.float64)
    ks = range(-a + 1, a + 1)
    wx = [np.sinc(x - (x0 + k)) * np.sinc((x - (x0 + k)) / a) for k in ks]
    wy = [np.sinc(y - (y0 + k)) * np.sinc((y - (y0 + k)) / a) for k in ks]
    for j, kj in enumerate(ks):
        yy = np.clip(y0 + kj, 0, h - 1)
        for i, ki in enumerate(ks):
            wgt = wx[i] * wy[j]
            acc += img[yy, np.clip(x0 + ki, 0, w - 1)] * wgt
            wsum += wgt
    return (acc / np.where(np.abs(wsum) > 1e-9, wsum, 1.0)).astype(np.float32)


def blur(img, sigma):
    r = int(math.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(img, ((0, 0), (r, r)), mode="edge")
    t = sum(k[i] * p[:, i:i + img.shape[1]] for i in range(2 * r + 1))
    p = np.pad(t, ((r, r), (0, 0)), mode="edge")
    return sum(k[i] * p[i:i + img.shape[0]] for i in range(2 * r + 1)).astype(np.float32)


def global_shift(a, b, margin=12, rounds=4):
    """(dx, dy) so that b(p) ~ a(p - d): how far the picture moved from a to b, by least squares on smoothed luma"""
    sa = blur(a, 1.5)
    sb = blur(b, 1.5)
    h, w = a.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.zeros(2, dtype=np.float64)
    inner = (slice(margin, h - margin), slice(margin, w - margin))
    for _ in range(rounds):
        wa = lanczos_at(sa, xs - d[0], ys - d[1])
        gy, gx = np.gradient(wa)
        e = (sb - wa)[inner].ravel()
        A = np.stack([-gx[inner].ravel(), -gy[inner].ravel()], axis=1)
        ata = A.T @ A
        if abs(np.linalg.det(ata)) < 1e-12:
            return None
        d += np.linalg.solve(ata, A.T @ e)
    return d


# ---- the model with the constants a frame's passes got -----------------------------------------------------------------
def version_of(cap):
    """the version that wrote a capture: 76 (statistics of the pixel's sample, no test against the history), 77 (the test
    against the history, no protection), 78 (both), 79 (and the test for things that arrive: bit 128 of the first
    pass's flags; a fix79 capture with that option off reads as 78, which is what the pass then does) or 81 (and the
    scatter's statistics with a limit for one odd sample: af_clip above 0). The statistics and constants of 76 and 77
    mean other things."""
    an = cap.frames[0]["analyze"]
    if "follow_k" not in an:
        return 76
    if "af_mem" not in an:
        return 77
    if not int(an["flags"]) & 128:
        return 78
    return 81 if an.get("af_clip", 0.0) > 0.0 else 79


def is_fix76(cap):
    return version_of(cap) == 76


def is_older(cap):
    """written by a version before this one: replayed with this version's model, not compared"""
    return version_of(cap) < 78


def model_settings(cap, fr, overrides):
    an, ac, rs = fr["analyze"], fr["accumulate"], fr["resolve"]
    aflags, cflags, rflags = int(an["flags"]), int(ac["flags"]), int(rs["flags"])
    s = dict(scale=cap.scale, sigma=math.sqrt(0.5 / ac["k"]), clip_gamma=an["gamma"], motion_reduce=ac["motion_reduce"],
             depth_reject=an["reject"], sharpen=rs["sharpen"], luma_weight=bool(cflags & 2), hist_extra=ac["extra"],
             hist_dering=True if cflags & 4 else (3 if cflags & 8 else False), cur_nearest=bool(cflags & 16), down="box" if rflags & 8 else "lanczos2",
             linear=True)
    if "af_mem" in an:
        s.update(w_max=an["w_max"], follow=bool(aflags & 16), fl_k=an["follow_k"], fl_b0=an["b0"], fl_kc=an["kc"], fl_b0c=an["b0c"], fl_cap=an["cap"],
                 fl_rate=an["rate"], fl_rate_b=an["rate_b"], w_short=an["w_short"], fl_count=int(an["count"]), fl_alln=int(an["alln"]),
                 fl_sde=an["fl_sde"], protect=bool(aflags & 64), af_rate=an["af_rate"], af_t0=an["af_t0"], af_t1=an["af_t1"], af_mem=an["af_mem"],
                 edge0=an["edge0"], edge1=an["edge1"], rel_decay=an["rel_decay"], af_relax=ac["relax"], af_wins=ac["wins"],
                 af_wins_floor=ac["wins_floor"], arrive=bool(aflags & 128), af_clip=an.get("af_clip", 0.0),
                 af_clip_floor=an.get("af_floor", R.NewTAA.DEFAULTS["af_clip_floor"]))
        if not s["protect"]:
            # (the pass sets these when the protection is off; the model's switch does the same)
            s.update(af_mem=R.NewTAA.DEFAULTS["af_mem"], af_relax=R.NewTAA.DEFAULTS["af_relax"], af_wins=R.NewTAA.DEFAULTS["af_wins"])
    else:
        # fix76, fix77: what the versions share, the rest as this version sets it (fix77's clip range was often set
        # wider to make up for the missing protection: this version's is used)
        s.update(w_max=an["w_max"] if "follow_k" in an else ac["w_max"], clip_gamma=R.NewTAA.DEFAULTS["clip_gamma"])
        s = dict(R.NewTAA.DEFAULTS, **s)
    for k, v in overrides.items():
        if k not in s:
            raise SystemExit("--set: no such setting '%s' (one of: %s)" % (k, ", ".join(sorted(s))))
        s[k] = type(s[k])(v) if not isinstance(s[k], (bool, str)) else (bool(int(v)) if isinstance(s[k], bool) else v)
    return s, dict(reset_a=bool(aflags & 1), reverse_z=bool(aflags & 2), have_depth=bool(aflags & 4), decode=bool(aflags & 8), dist_test=bool(aflags & 32),
                   reset_c=bool(cflags & 1), final_pass=bool(rs.get("final_pass", 1)), debug=int(rs.get("debug", 0)), split=int(rs.get("split_x", 0)))


def replay(cap, overrides, bits, log, debug=None):
    """-> list of the model's output parts (one per frame), and the comparison with the game's images.
    debug: a list that gets the model's statistics and guide of every frame"""
    n = cap.count
    old = is_older(cap)
    R.BILINEAR_BITS = bits
    model = None
    outs = []
    ok = None
    px, py, pw, ph = cap.part_in
    ox, oy, ow, oh = cap.part_out
    worst = []

    def compare(name, mine, theirs, mask, tol):
        if theirs is None:
            return
        if theirs.shape[-1] == 1 and mine.ndim == 2:
            theirs = theirs[..., 0]
        if mine.ndim == 3 and theirs.ndim == 3:
            c = min(mine.shape[-1], theirs.shape[-1])
            mine, theirs = mine[..., :c], theirs[..., :c]
        d = np.abs(mine.astype(np.float64) - theirs.astype(np.float64))
        if d.ndim == 3:
            d = d.max(axis=-1)
        v = d[mask] if mask is not None else d.ravel()
        if v.size == 0:
            log("    %-16s nothing to compare (no pixel of the part can be followed this far)" % name)
            return
        off = float((v > tol).mean()) * 100.0
        log("    %-16s median %.5f  99th %.5f  worst %.5f  off by more than %.3f: %.3f %% of %d" % (name, np.median(v), np.percentile(v, 99), v.max(), tol,
                                                                                                    off, v.size))
        return off

    for i in range(n):
        fr = cap.frames[i]
        s, fl = model_settings(cap, fr, overrides)
        if model is None:
            model = R.NewTAA(**s)
            model.half = True
            model.view = tuple(cap.view)
            model.in_origin = (px, py)
            model.hist_full = tuple(cap.history)
            model.hist_origin = tuple(cap.part_history[:2])
            model.hist_part = tuple(cap.part_history[2:])
            hb = cap.image_of(0, "history_before")
            sb = cap.image_of(0, "stats_before")
            db = cap.image_of(0, "dist_before")
            if hb is not None and sb is not None and db is not None:
                model.hist = hb.copy()
                if old:
                    # (the older versions kept other statistics: this version's start empty)
                    model.pdist, model.stats = db[..., 0].copy(), np.zeros(sb.shape[:2] + (4,), dtype=np.float32)
                else:
                    model.set_state_image(db)
                    model.stats = sb.copy()
            else:
                log("  (the state before the first frame is not in the capture: the replay starts without a history)")
            ok = np.ones((ph, pw), dtype=bool)
        else:
            model.s.update(s)
        col = cap.image_of(i, "color")
        mv = cap.motion_px(i)
        z = cap.depth(i)
        if col is None or mv is None:
            log("  frame %d: inputs missing, replay ended" % i)
            break
        if not fl["decode"] or fl["debug"] or fl["split"]:
            log("  frame %d: not replayed exactly (%s)" % (i, "no decode of the colour" if not fl["decode"] else "a debug view or the split was on"))
        have_depth = fl["have_depth"] and z is not None
        lin, guide, boxc, boxe = model.analyze(col, mv, None if have_depth else np.ones(col.shape[:2], np.float32), fr["jitter"],
                                               z=z if have_depth else None, depth_k=cap.depth_k, reverse_z=fl["reverse_z"], reset=fl["reset_a"],
                                               have_depth=have_depth, dist_test=fl["dist_test"] and have_depth)
        model.accumulate(lin, guide, boxc, boxe, fr["jitter"], reset=fl["reset_c"])
        out = model.resolve()
        outs.append(out)
        if debug is not None:
            debug.append(dict(stats=model.stats.copy(), guide=guide.copy(), rel=model.rel.copy(), conf=model.debug["conf"].copy(),
                              sde=model.debug["sde"].copy(), arrive=model.debug["arrive"].copy()))

        # the pixels whose whole past lies inside the part: the others read what the capture does not hold
        ys, xs = np.mgrid[0:ph, 0:pw]
        qx = np.floor(xs + 0.5 + guide[..., 0]).astype(np.int64)
        qy = np.floor(ys + 0.5 + guide[..., 1]).astype(np.int64)
        # (what a pixel reads of the last frame: the statistics' 2 x 2 texels, the history's taps 2 texels to each side of
        # its place there, and for the test of what changed the history under its 3x3 neighbours' samples: at most 2 view
        # pixels around its place there, one more to be safe. With the test for things that arrive: the last frame's
        # distances up to 3 pixels around that place (the 6x6 block), and the 2x2 beside them)
        reach = 5 if s["arrive"] else 3
        inside = (qx >= reach) & (qx < pw - reach) & (qy >= reach) & (qy < ph - reach)
        src = erode(ok, reach)[np.clip(qy, 0, ph - 1), np.clip(qx, 0, pw - 1)]
        border = np.zeros((ph, pw), dtype=bool)
        border[2:-2, 2:-2] = True
        ok = border & inside & src

        if overrides or old:
            continue  # (other settings than the game's, or another version's capture: nothing to compare with)
        theirs = cap.image_of(i, "out")
        # the mask at the fovea image's and the history's size
        my, mx = np.mgrid[0:oh, 0:ow]
        mo = ok[np.clip(((my + oy + 0.5) * cap.view[1] / cap.image[1] - py).astype(np.int64), 0, ph - 1),
                np.clip(((mx + ox + 0.5) * cap.view[0] / cap.image[0] - px).astype(np.int64), 0, pw - 1)]
        log("  frame %d (%d %% of the part can be followed):" % (i, round(100.0 * ok.mean())))
        w = compare("fovea image", out, theirs, mo, 0.02)
        if w is not None:
            worst.append(w)
        if i == 0 or i + 1 == n:
            hy, hx = np.mgrid[0:cap.part_history[3], 0:cap.part_history[2]]
            mh = mo[np.clip(hy // cap.scale, 0, oh - 1), np.clip(hx // cap.scale, 0, ow - 1)]
            compare("linear colour", lin, cap.image_of(i, "linear"), None, 0.004)
            compare("guide (motion)", guide[..., :2], cap.image_of(i, "guide"), ok, 0.01)
            compare("guide (weight)", guide[..., 2], None if cap.image_of(i, "guide") is None else cap.image_of(i, "guide")[..., 2], ok, 0.05)
            compare("guide (protection)", guide[..., 3], None if cap.image_of(i, "guide") is None else cap.image_of(i, "guide")[..., 3], ok, 0.05)
            compare("box centre", boxc, cap.image_of(i, "box_centre"), border, 0.004)
            compare("box extent", boxe, cap.image_of(i, "box_extent"), border, 0.004)
            compare("statistics", model.stats, cap.image_of(i, "stats_after"), ok, 0.004)
            da = cap.image_of(i, "dist_after")
            compare("distances", model.pdist, None if da is None else da[..., 0], None, 0.02)
            compare("release", model.rel, None if da is None else da[..., 1], ok, 0.05)
            ha = cap.image_of(i, "history_after")
            if ha is not None:
                compare("history colour", model.hist[..., :3], ha[..., :3], mh, 0.01)
                compare("history weight", model.hist[..., 3], ha[..., 3], mh, 0.05)
    return outs, worst


# ---- measures --------------------------------------------------------------------------------------------------------
def measure(name, frames, motions, cap, log, margin=16):
    """frames: display-encoded parts of the fovea image [h, w, 3] per frame; motions: motion of the part in view pixels.
    Flicker along the motion: every frame against the mean of the frame before and the frame after, both brought to its
    place with the motion vectors (Lanczos 3)."""
    n = len(frames)
    ys_l = [luma(f) for f in frames]
    h, w = ys_l[0].shape
    fy_ = cap.image[1] / cap.view[1]
    fx_ = cap.image[0] / cap.view[0]
    ox, oy = cap.part_out[0], cap.part_out[1]
    px, py = cap.part_in[0], cap.part_in[1]
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)

    def motion_at_out(m):
        # the motion (view pixels) of the view pixel under each fovea image pixel, in fovea image pixels
        iy = np.clip(((ys + oy + 0.5) / fy_ - py).astype(np.int64), 0, m.shape[0] - 1)
        ix = np.clip(((xs + ox + 0.5) / fx_ - px).astype(np.int64), 0, m.shape[1] - 1)
        return m[iy, ix, 0] * fx_, m[iy, ix, 1] * fy_

    detail = float(np.mean([0.5 * (np.abs(np.diff(y, axis=1))[:-1].mean() + np.abs(np.diff(y, axis=0))[:, :-1].mean()) for y in ys_l]))
    # the top octave of the spatial frequencies (finer than 4 pixels per cycle): mean amplitude of a 3-tap high pass
    fine = float(np.mean([np.abs(y[1:-1, 1:-1] - 0.25 * (y[1:-1, :-2] + y[1:-1, 2:] + y[:-2, 1:-1] + y[2:, 1:-1])).mean() for y in ys_l]))
    inner = (slice(margin, h - margin), slice(margin, w - margin))
    fl, fine_fl, tails = [], [], []
    maps = np.zeros((h, w), dtype=np.float64)
    count = 0
    # flicker: of the picture smoothed over about 4 pixels (sigma 1.5), each frame against the mean of the frame before and
    # the frame after brought to its place along the motion. Checked against the simulation's true flicker: it follows it
    # on thin lines, fences, glints and edges, still and moving; without the smoothing ("pixel flicker") a sharper image
    # scores up to 3 times higher in motion only because its detail moves.
    for t in range(1, n - 1):
        mx, my = motion_at_out(motions[t])
        mx2, my2 = motion_at_out(motions[t + 1])
        before = lanczos_at(ys_l[t - 1], xs + mx, ys + my)
        after = lanczos_at(ys_l[t + 1], xs - mx2, ys - my2)
        d_fine = np.abs(ys_l[t] - 0.5 * (before + after))
        d2 = np.abs(blur(ys_l[t], 1.5) - 0.5 * (blur(before, 1.5) + blur(after, 1.5)))
        fl.append(float(d2[inner].mean()))
        fine_fl.append(float(d_fine[inner].mean()))
        tails.append(float(np.percentile(d2[inner], 99)))
        maps += d2
        count += 1
    res = dict(name=name, detail=detail, fine=fine, flicker=float(np.mean(fl)) if fl else 0.0, pixel_flicker=float(np.mean(fine_fl)) if fine_fl else 0.0,
               tail=float(np.mean(tails)) if tails else 0.0, map=(maps / max(count, 1)).astype(np.float32))
    log("  %-28s detail %.5f  fine detail %.5f  |  flicker %.5f  flicker of the worst 1 %% %.5f  (pixel flicker %.5f)" %
        (name, detail, fine, res["flicker"], res["tail"], res["pixel_flicker"]))
    return res


def bilin(img, x, y):
    """img [h, w(, c)] at continuous index positions, clamped"""
    h, w = img.shape[:2]
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0).astype(np.float32)
    fy = (y - y0).astype(np.float32)
    if img.ndim == 3:
        fx = fx[..., None]
        fy = fy[..., None]

    def at(xx, yy):
        return img[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)]

    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy


class Lag:
    """Ghosting: how much of what a place showed k frames ago is still in the output where the picture has changed since.

    For frame t and lag k: the raw frame t-k brought to frame t along the motion vectors (P), the raw frame t (C), the
    output (O), all smoothed (sigma 1.5 px), display-encoded luma. Where the picture smoothed over a wider area
    (sigma 3: single-pixel noise does not count) differs by more than thr (something moved over the place without
    motion vectors, was uncovered, a light changed), the part of P - C still in O - C:
        lag_k = sum (O - C)(P - C) / sum (P - C)^2        0 = nothing left of it, 1 = the output still shows the old picture
    (stochastic shadows that survive the wider smoothing count as changes too: there a higher number is the averaging
    that is wanted. Read the number where something really changed: a display, a thing moving against a wall.)"""

    def __init__(self, raws, motions, jitters, ks=(1, 2, 4, 8), thr=0.04, sigma=1.5, mask_sigma=3.0, margin=6):
        self.ks = ks
        n = len(raws)
        h, w = raws[0].shape[:2]
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        lum = [luma(r) for r in raws]
        self.items = []   # (t, k, C, D = P - C, mask)
        for t in range(n):
            # the raw frame at the unjittered pixel places (the sample of pixel q shows q + 0.5 - jitter)
            c0 = bilin(lum[t], xs + jitters[t][0], ys + jitters[t][1])
            c = blur(c0, sigma)
            cm = blur(c0, mask_sigma)
            px, py = xs + 0.5, ys + 0.5
            ok = np.ones((h, w), dtype=bool)
            for k in range(1, max(ks) + 1):
                if t - k < 0:
                    break
                m = bilin(motions[t - k + 1], px - 0.5, py - 0.5)
                px = px + m[..., 0]
                py = py + m[..., 1]
                ok &= (px > margin) & (px < w - margin) & (py > margin) & (py < h - margin)
                if k in ks:
                    j = jitters[t - k]
                    p0 = bilin(lum[t - k], xs + j[0], ys + j[1])
                    pw = bilin(blur(p0, sigma), px - 0.5, py - 0.5)
                    pm = bilin(blur(p0, mask_sigma), px - 0.5, py - 0.5)
                    self.items.append((t, k, c, pw - c, ok & (np.abs(pm - cm) > thr)))

    def share(self, k):
        v = [mask.mean() for (t, kk, c, d, mask) in self.items if kk == k]
        return float(np.mean(v)) if v else 0.0

    def measure(self, outs, t0=0):
        """-> {k: lag} over the frames from t0 on"""
        acc = dict((k, [0.0, 0.0]) for k in self.ks)
        cache = {}
        for t, k, c, d, mask in self.items:
            if t < t0:
                continue
            if t not in cache:
                cache[t] = blur(luma(np.clip(outs[t], 0, 1)), 1.5)
            o = cache[t]
            acc[k][0] += float(((o - c)[mask] * d[mask]).sum())
            acc[k][1] += float((d[mask] ** 2).sum())
        return dict((k, a[0] / a[1] if a[1] > 0 else float("nan")) for k, a in acc.items())


def input_checks(cap, log):
    """how far the picture really moved from frame to frame against what jitter and motion vectors say"""
    n = cap.count
    rows = []
    prev = None
    for i in range(n):
        col = cap.image_of(i, "color")
        mv = cap.motion_px(i)
        if col is None or mv is None:
            break
        y = luma(col)
        if prev is not None:
            d = global_shift(prev[0], y)
            if d is not None:
                mvm = mv.reshape(-1, 2).mean(axis=0)
                mvs = mv.reshape(-1, 2).std(axis=0)
                j1 = np.array(cap.frames[i]["jitter"])
                j0 = np.array(cap.frames[i - 1]["jitter"])
                rows.append((i, d, mvm, mvs, j1 - j0))
        prev = (y,)
    if not rows:
        log("  (not enough frames)")
        return
    log("  frame: the picture moved (px)   | motion vectors say (mean -mv, spread) | jitter(t) - jitter(t-1) as the pass got it")
    res = {"-mv": [], "-mv - dj": [], "-mv + dj": []}
    for i, d, mvm, mvs, dj in rows:
        log("  %5d: %+7.3f %+7.3f          | %+7.3f %+7.3f  (+-%.2f %.2f)        | %+6.3f %+6.3f" % (i, d[0], d[1], -mvm[0], -mvm[1], mvs[0], mvs[1], dj[0], dj[1]))
        res["-mv"].append(d - (-mvm))
        res["-mv - dj"].append(d - (-mvm - dj))
        res["-mv + dj"].append(d - (-mvm + dj))
    log("  how well each reading explains the picture's movement (RMS of what is left, px; the smallest is the true one):")
    best = None
    for k, v in res.items():
        r = float(np.sqrt(np.mean(np.square(np.array(v)))))
        log("    picture moved by %-9s: %.3f" % (k, r))
        if best is None or r < best[1]:
            best = (k, r)
    log("  -> %s. The pass takes the sample of pixel q to show the point at q + 0.5 - jitter, which moves the picture by +(jitter(t) - jitter(t-1))"
        " from frame to frame on top of the motion: '-mv + dj' is what it expects%s." %
        (best[0], "" if best[0] == "-mv + dj" else "  <-- NOT what the capture shows: the jitter's sign or the motion vectors' meaning differ"))
    spread = float(np.mean([np.abs(m[3]).max() for m in rows]))
    if spread > 0.5:
        log("  (the motion differs across the part by +-%.2f px: the picture's movement is not one shift, the numbers above are rough)" % spread)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    folder = args[0]
    out_dir = os.path.join(folder, "analysis")
    overrides = {}
    bits = None
    i = 1
    while i < len(args):
        if args[i] == "--out":
            out_dir = args[i + 1]
            i += 2
        elif args[i] == "--bits":
            bits = int(args[i + 1])
            i += 2
        elif args[i] == "--set":
            i += 1
            while i < len(args) and "=" in args[i]:
                k, v = args[i].split("=", 1)
                overrides[k] = float(v)
                i += 1
        else:
            raise SystemExit("unknown argument " + args[i])
    os.makedirs(out_dir, exist_ok=True)
    lines = []

    def log(text=""):
        print(text)
        lines.append(text)

    cap = Capture(folder)
    n = cap.count
    log("capture %s: %d frames, view %dx%d, fovea image %dx%d, history x%d" % (folder, n, cap.view[0], cap.view[1], cap.image[0], cap.image[1], cap.scale))
    log("  part of the view %s, of the fovea image %s" % (cap.part_in, cap.part_out))
    log("  settings: " + ", ".join("%s %g" % (k, v) for k, v in cap.settings.items()))
    if n < 1:
        raise SystemExit("no frame in the capture")

    old = is_older(cap)
    wrote = "fix%d" % version_of(cap)
    log("\n1. replay with the reference model%s:" % (" (filter weights of %d bits)" % bits if bits else ""))
    if old:
        log("  this capture was written by %s (other statistics, other constants): its images are not compared. Its frames run through" % wrote)
        log("  this version's model from the captured history instead, with this version's settings: 'this version' below.")
    dbg = []
    outs, worst = replay(cap, {}, bits, log, debug=dbg)
    if worst:
        log("  -> fovea image: at most %.3f %% of the pixels that can be followed differ by more than 0.02 in a frame" % max(worst))

    log("\n2. the inputs:")
    input_checks(cap, log)

    log("\n3. measures (display-encoded luma 0..1; the middle of the part; flicker = of the picture smoothed over about 4 pixels, along the motion):")
    motions = [cap.motion_px(i) for i in range(n)]
    new = [cap.image_of(i, "out") for i in range(n)]
    olds = [cap.image_of(i, "old") for i in range(n)]
    raw = [cap.image_of(i, "color") for i in range(n)]
    speed = float(np.mean([np.hypot(m[..., 0], m[..., 1]).mean() for m in motions]))
    log("  mean motion %.2f view pixels per frame" % speed)
    shown = [("supersampling TAA (game%s)" % (", " + wrote if old else ""), new)]
    if all(o is not None for o in olds):
        shown.append(("the mod's TAA (game)", olds))
    same_size = raw[0].shape == new[0].shape
    if same_size:
        shown.append(("raw frames (no AA)", raw))
    this_version = None
    tv_dbg = []
    if old and len(outs) == n:
        this_version = [np.clip(o, 0, 1) for o in outs]
        shown.append(("this version (model)", this_version))
    elif version_of(cap) in (78, 79) and not overrides:
        # (written without the test for things that arrive or without the limit of the scatter's statistics: the same
        # frames through the model with both)
        log("  (this capture was written %s: 'this version' is the model" %
            ("without the test for things that arrive, by fix78 or with that option off" if version_of(cap) == 78 else
             "by fix79 or fix80, whose scatter statistics took one odd sample whole (what a fly left behind was protected)"))
        log("   with what it lacks, on the same frames and from the same state)")
        tv, _ = replay(cap, {"arrive": 1.0, "af_clip": R.NewTAA.DEFAULTS["af_clip"]}, bits, lambda *_: None, debug=tv_dbg)
        if len(tv) == n:
            this_version = [np.clip(o, 0, 1) for o in tv]
            shown.append(("this version (model)", this_version))
    what_if = None
    if overrides:
        what_if, _ = replay(cap, overrides, bits, lambda *_: None)
        what_if = [np.clip(o, 0, 1) for o in what_if]
        if not old and len(outs) == n:
            shown.append(("model, the game's settings", [np.clip(o, 0, 1) for o in outs]))
        if len(what_if) == n:
            shown.append(("model, what if: " + " ".join("%s=%g" % kv for kv in overrides.items()), what_if))
        else:
            what_if = None
    # (a capture of an older version: this version's model starts from that version's history and from empty statistics;
    # the first frames are it settling, so every row is measured from frame 6 on, from frame 16 with 48 frames or more)
    t0 = (16 if n >= 48 else 6) if old and n >= 12 else 0
    if t0:
        log("  (all rows from frame %d on: until then this version's model is still settling on the history %s left)" % (t0, wrote))
    results = [measure(name, frames[t0:], motions[t0:], cap, log) for name, frames in shown]
    by_name = dict((r["name"], r) for r in results)

    def against(a, b):
        return ("detail %+.0f %%, fine detail %+.0f %%, flicker %+.0f %%, flicker of the worst 1 %% %+.0f %%" %
                (100 * (a["detail"] / b["detail"] - 1), 100 * (a["fine"] / max(b["fine"], 1e-9) - 1), 100 * (a["flicker"] / b["flicker"] - 1),
                 100 * (a["tail"] / max(b["tail"], 1e-9) - 1)))

    mod = by_name.get("the mod's TAA (game)")
    if mod is not None and mod["flicker"] > 0 and mod["detail"] > 0:
        log("  supersampling TAA against the mod's TAA: " + against(results[0], mod))
        if this_version is not None:
            log("  this version against the mod's TAA:      " + against(by_name["this version (model)"], mod))
        if what_if is not None:
            log("  what if against the mod's TAA:           " + against(results[-1], mod))

    # ghosting: what is left of older frames where the picture has changed since
    if same_size and n >= 4:
        lag = Lag(raw, motions, [cap.frames[i]["jitter"] for i in range(n)])
        share = lag.share(4)
        log("  what is left of the picture of k frames before where it has changed since (0 = nothing, 1 = all of it; at k = 4 %.2f %% of the part changed%s):" %
            (100 * share, ": hardly anything, the numbers say little" if share < 0.002 else ""))
        for name, frames in shown:
            if name.startswith("raw"):
                continue
            m = lag.measure(frames, t0)
            log("  %-32s " % name[:32] + "  ".join("k=%d %5.2f" % (k, m[k]) for k in lag.ks if m[k] == m[k]))

    # what the test of what changed did (first and last frame)
    for i in sorted({0, len(dbg) - 1}) if dbg else []:
        g, st = dbg[i]["guide"], dbg[i]["stats"]
        hist = g[..., 2] > 0.0
        log("  frame %d%s: history %.1f %% of the pixels, following a change %.1f %%, protected %.1f %% (released %.1f %%), noise believed: mean %.4f, "
            "95th %.4f, scatter of the samples: mean %.4f" %
            (i, " (model)" if old else "", 100 * hist.mean(), 100 * (hist & (dbg[i]["conf"] > 0.5)).mean(), 100 * (g[..., 3] > 0.5).mean(),
             100 * (hist & (dbg[i]["rel"] > 0.5)).mean(), float(st[..., 0].mean()), float(np.percentile(st[..., 0], 95)), float(dbg[i]["sde"].mean())))
    adbg = tv_dbg or dbg
    if adbg:
        arr = np.array([d["arrive"].mean() for d in adbg])
        mk = np.array([(d["rel"] > 1.0).mean() for d in adbg])
        log("  things that arrived (flies: they move, their motion vectors do not)%s: in %d of %d frames, %.3f %% of the pixels on average; marked (no "
            "protection, statistics standing still): %.2f %%" % (", this version" if tv_dbg else "", int((arr > 0).sum()), len(adbg), 100 * arr.mean(),
                                                               100 * mk.mean()))

    # ---- pictures ----
    log("\n4. pictures in %s:" % out_dir)
    k = 3 if max(new[0].shape[:2]) <= 256 else 2
    cols = [(nm, fr) for nm, fr in shown if not nm.startswith("raw") and not nm.startswith("model, the game")]
    if same_size:
        cols = [("raw", raw)] + cols
    short = {"supersampling TAA (game)": "supersampling-TAA", "supersampling TAA (game, fix76)": "fix76", "supersampling TAA (game, fix77)": "fix77",
             "the mod's TAA (game)": "mod-TAA", "raw": "raw", "this version (model)": "this-version"}
    for i in sorted({0, n // 2, n - 1}):
        row = [zoom(np.clip(fr[i], 0, 1), k) for _, fr in cols]
        names = [short.get(nm, "what-if") for nm, _ in cols]
        write_png(os.path.join(out_dir, "frame%02d_%s.png" % (i, "_".join(names))), side_by_side(row))
        log("  frame%02d: %s (x%d)" % (i, " | ".join(names), k))
    top = max(float(np.percentile(r["map"], 99.5)) for r in results)
    write_png(os.path.join(out_dir, "flicker_maps.png"), side_by_side([zoom(heat(r["map"], top), k) for r in results]))
    log("  flicker_maps.png: where each one flickers along the motion (white = %.3f): %s" % (top, " | ".join(r["name"] for r in results)))
    if dbg:
        g, st = dbg[-1]["guide"], dbg[-1]["stats"]
        pic = np.stack([np.clip(dbg[-1]["rel"], 0, 1), np.clip(g[..., 3], 0, 1), np.where(g[..., 2] <= 0.0, 1.0, 0.0)], axis=-1)
        write_png(os.path.join(out_dir, "follow_last_frame.png"),
                  side_by_side([zoom(pic, k), zoom(heat(st[..., 0], 0.05), k), zoom(heat(dbg[-1]["sde"], 0.10), k)]))
        log("  follow_last_frame.png%s: green protected, red the protection released, blue no history | the noise believed (white = 0.05) | the scatter "
            "of the samples (white = 0.10)" % (" (model)" if old else ""))
    with open(os.path.join(out_dir, "analysis.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
