"""[fix81 17.11.2026] The frame's chain of a capture from the game (quadview_dump/supertaa_captureNN/chain.txt, written by
fix81 beside the supersampling TAA's own images): where along the way from the wide view to the eye image the picture
starts to flicker.

   python3 analyze_chain.py <capture folder> [--sheet=prefix] [--gain=1.5]

Images of a frame (a part around the place the left eye looked at; see TemporalUpscaler.cpp, [CAPTURE fix81]):
  wide_color      the shared wide view as its anti-aliasing got it (the frame's own, jittered pixels; interface taken out)
  wide_motion     its motion vectors, wide_depth its depth (the frame's own, jittered samples)
  wide_stable     the distances the reprojection got (d3d12::StableDepth: x = 1 / distance, y = its mean), when it ran
  wide_taa        the wide view's anti-aliased image (1 or 2 texels per pixel of the wide view and axis: "ss")
  eye_periphery   the left eye's periphery after the reprojection (the same density)
  eye_out         the finished eye image around the middle of the fovea rectangle (4 eye pixels per pixel of the wide view)
  eye_out_side    [fix82] the finished eye image at the chain's place: the periphery as the eye sees it
  ui_...          first frames only: what the interface was made from (the wide view's colour and its two copies; fix81
                  also the fovea view's colour and copy and UiClean's light layer and matte)
[fix82] The chain is taken beside the fovea rectangle, at one place for the whole capture (fix81: around the place looked
at, inside the fovea, and its corner followed the gaze). Every stage is compared over the place all its frames cover, each
frame at its own corner (chain.txt): fix81's version compared the parts as they came, so a corner that moved counted as
flicker (in the captures of 17.11.2026 16:0x two pixels of the wide view in a third of the frames: 3 to 4 times too much).
Printed: what the capture holds; per stage the flicker of its picture (luma as displayed, the frame against the mean of the
one before and the one after brought to its place by the part's median motion) low-passed over about two eye pixels and
over a pixel of the wide view, and its detail (mean difference between neighbouring pixels); how often the distance of a
pixel changes its surface from frame to frame in the frame's own depth and in the stable one; what the interface's images
hold.
The sheets (--sheet): per stage the first, a middle and the last frame, and the change between the last two."""
import os
import re
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # (also when Python runs isolated, -I)
import analyze_capture as AC  # noqa: E402
import capture_io as CIO  # noqa: E402
import resolvers as R  # noqa: E402

STAGES = ("wide_color", "wide_taa", "eye_periphery", "eye_out_side", "eye_out")


def read_chain(folder):
    """chain.txt -> (frames recorded, per frame {image name: (x, y, w, h, W, H)}, per frame {note name: {key: numbers}})"""
    images, notes = {}, {}
    n = 0
    with open(os.path.join(folder, "chain.txt")) as f:
        for line in f:
            m = re.match(r"frames_recorded (\d+)", line)
            if m:
                n = int(m.group(1))
                continue
            m = re.match(r"frame (\d+) image (\S+) format (\d+) part (\d+) (\d+) (\d+) (\d+) of (\d+) (\d+)", line)
            if m:
                images.setdefault(int(m.group(1)), {})[m.group(2)] = tuple(int(v) for v in m.groups()[3:])
                continue
            m = re.match(r"frame (\d+) (\w+): (.*)", line)
            if m:
                words = m.group(3).split()
                d, key = {}, None
                for w in words:
                    try:
                        v = float(w)
                    except ValueError:
                        key = w
                        d[key] = []
                        continue
                    if key is not None:
                        d[key].append(v)
                notes.setdefault(int(m.group(1)), {})[m.group(2)] = d
    return n, images, notes


def image(folder, i, name):
    img, fmt = CIO.read_image(os.path.join(folder, "frame%02d" % i), name)
    return img


def stack(folder, images, name, n):
    """the frames of an image over the place every frame's part covers -> (list of arrays or None, frames it was found in)"""
    have = [i for i in range(n) if name in images.get(i, {})]
    if len(have) < 3:
        return [None] * n, have
    org = {i: images[i][name][:4] for i in have}
    x0 = max(o[0] for o in org.values())
    y0 = max(o[1] for o in org.values())
    x1 = min(o[0] + o[2] for o in org.values())
    y1 = min(o[1] + o[3] for o in org.values())
    out = [None] * n
    if x1 - x0 < 32 or y1 - y0 < 32:
        return out, have
    for i in have:
        img = image(folder, i, name)
        if img is not None:
            out[i] = img[y0 - org[i][1]:y1 - org[i][1], x0 - org[i][0]:x1 - org[i][0]]
    return out, have


def shift(img, dx, dy):
    """img moved so that what was at p + (dx, dy) is at p (bilinear)"""
    H, W = img.shape
    yy, xx = np.meshgrid(np.arange(H, dtype=np.float32) + dy, np.arange(W, dtype=np.float32) + dx, indexing="ij")
    return ndimage.map_coordinates(img, [yy, xx], order=1, mode="nearest")


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    folder = args[0]
    n, images, notes = read_chain(folder)
    if n == 0:
        print("no frames in %s" % os.path.join(folder, "chain.txt"))
        return
    names = sorted({k for d in images.values() for k in d})
    print("%s: %d frames; images: %s" % (folder, n, ", ".join("%s (%d frames)" % (k, sum(1 for d in images.values() if k in d)) for k in names)))
    w0 = notes.get(0, {}).get("wide", {})
    e0 = notes.get(0, {}).get("eye_periphery", {})
    o0 = notes.get(0, {}).get("eye_out", {})
    if w0:
        print("  wide view %dx%d, part %s, %d texel(s) per pixel in its anti-aliased image, periphery anti-aliasing %d, stable depth %d" %
              (w0["view"][0], w0["view"][1], " ".join("%d" % v for v in w0["part"]), w0["ss"][0], w0["periphery_aa"][0], w0["stable_depth"][0]))
    if o0:
        how = ("interface inside the fovea's picture %d" % o0["interface_in_fovea"][0]) if "interface_in_fovea" in o0 else \
              ("fix81: light layer %d, matte %d" % (o0.get("light_layer", [0])[0], o0.get("matte", [0])[0]))
        print("  eye image %dx%d, part %s; fovea drawn %d; steady interface %d (%s); debug view %d" %
              (o0["image"][0], o0["image"][1], " ".join("%d" % v for v in o0["part"]), o0["fovea_drawn"][0], o0["steady_interface"][0], how, o0["debug_view"][0]))
    ss = int(w0["ss"][0]) if w0 else 1
    linear = bool(w0["linear"][0]) if w0 else False

    # ---- the part's motion per frame (its median), in pixels of the wide view (previous place = pixel + motion)
    mean_mv = []
    for i in range(n):
        mv = image(folder, i, "wide_motion")
        wn = notes.get(i, {}).get("wide", w0)
        if mv is None or not wn:
            mean_mv.append((0.0, 0.0))
            continue
        sx = wn["mv_scale"][0] * wn["view"][0]
        sy = wn["mv_scale"][1] * wn["view"][1]
        mean_mv.append((float(np.median(mv[..., 0])) * sx, float(np.median(mv[..., 1])) * sy))
    speed = float(np.mean([np.hypot(*m) for m in mean_mv]))
    print("  the part moved %.2f pixels of the wide view per frame on average (%.1f eye pixels)" % (speed, 4.0 * speed))
    moved = {st: len({images[i][st][:2] for i in range(n) if st in images.get(i, {})}) for st in STAGES}
    if any(v > 1 for v in moved.values()):
        print("  corners of the recorded parts: %s (each frame taken at its own)" % ", ".join("%s %d" % (k, v) for k, v in moved.items() if v > 0))

    # ---- per stage: luma as displayed, its scale against the wide view, flicker and detail
    eye_per_wide = 4.0
    if e0 and o0 and e0.get("rays") and e0["rays"][0] > 0:
        eye_per_wide = o0["image"][0] / e0["rays"][0]
    print("\n  stage          | pixels per wide pixel | detail  | flicker x1000, low-passed about two eye pixels | a pixel of the wide view | frames")
    lumas = {}
    for st in STAGES:
        frames, have = stack(folder, images, st, n)
        seq = []
        for img in frames:
            if img is None:
                seq.append(None)
                continue
            rgb = img[..., :3]
            if st in ("wide_taa", "eye_periphery") and linear:
                rgb = R.srgb_encode(np.clip(rgb, 0.0, None))
            seq.append(AC.luma(np.clip(rgb, 0.0, 1.0)))
        lumas[st] = seq
        have = [s for s in seq if s is not None]
        if len(have) < 3:
            continue
        scale = {"wide_color": 1.0, "wide_taa": float(ss), "eye_periphery": float(ss), "eye_out": eye_per_wide, "eye_out_side": eye_per_wide}[st]
        det = float(np.mean([0.5 * (np.abs(np.diff(s, axis=0)).mean() + np.abs(np.diff(s, axis=1)).mean()) for s in have]))
        cols = []
        for sigma in (max(2.0 * scale / eye_per_wide, 0.5), scale):
            fl, cnt = 0.0, 0
            for i in range(1, n - 1):
                if seq[i - 1] is None or seq[i] is None or seq[i + 1] is None:
                    continue
                a = ndimage.gaussian_filter(seq[i - 1], sigma)
                b = ndimage.gaussian_filter(seq[i], sigma)
                c = ndimage.gaussian_filter(seq[i + 1], sigma)
                before = shift(a, mean_mv[i][0] * scale, mean_mv[i][1] * scale)
                after = shift(c, -mean_mv[i + 1][0] * scale, -mean_mv[i + 1][1] * scale)
                m = int(max(8, np.ceil(4.0 * speed * scale) + 4))
                if b.shape[0] <= 2 * m + 8 or b.shape[1] <= 2 * m + 8:
                    continue
                fl += float(np.abs(b - 0.5 * (before + after))[m:-m, m:-m].mean())
                cnt += 1
            cols.append(1000.0 * fl / max(cnt, 1))
        print("  %-14s | %21.1f | %.5f | %46.3f | %24.3f | %d" % (st, scale, det, cols[0], cols[1], len(have)))
    print("  (read down the columns: the stage where the flicker rises is the one that adds it. wide_color is the frame's own pixels, without anti-aliasing;")
    print("   wide_taa -> eye_periphery is the reprojection; eye_periphery -> eye_out_side the enlargement and the post pass; eye_out is the fovea)")

    # ---- the distances: how often a pixel changes its surface from frame to frame
    if w0 and any("wide_depth" in d for d in images.values()):
        k = w0["depth_k"]
        raw_flip, st_flip, cnt = 0.0, 0.0, 0
        prev_raw, prev_st = None, None
        zs, _ = stack(folder, images, "wide_depth", n)
        sts, _ = stack(folder, images, "wide_stable", n)
        for i in range(n):
            z = zs[i]
            s = sts[i]
            raw = None if z is None else 1.0 / np.clip(np.abs((k[0] * z[..., 0] + k[1]) / np.where(np.abs(k[2] * z[..., 0] + k[3]) < 1e-9, 1e-9, k[2] * z[..., 0] + k[3])), 0.12, 1e4)
            stv = None if s is None else s[..., 0]
            still = np.hypot(*mean_mv[i]) < 0.05
            if still and prev_raw is not None and raw is not None and raw.shape == prev_raw.shape:
                raw_flip += float((np.abs(raw - prev_raw) > 0.1 * np.maximum(raw, prev_raw)).mean())
                if stv is not None and prev_st is not None and stv.shape == prev_st.shape:
                    st_flip += float((np.abs(stv - prev_st) > 0.1 * np.maximum(stv, prev_st)).mean())
                cnt += 1
            prev_raw, prev_st = raw, stv
        if cnt:
            print("\n  distances, frames with the head still (%d): pixels whose 1 / distance changes by more than 10 %% from one frame to the next:" % cnt)
            print("    the frame's own depth %.3f %%, the distances the reprojection got %.3f %%" % (100.0 * raw_flip / cnt, 100.0 * st_flip / cnt))
        else:
            print("\n  distances: no two frames in a row with the head still (below 0.05 pixels of the wide view): not compared")

    # ---- the interface's images
    ui = [k for k in names if k.startswith("ui_")]
    if ui:
        print("\n  interface (frames 0..%d):" % (max(i for i, d in images.items() if any(k in d for k in ui))))
        for i in sorted(images):
            row = []
            li = image(folder, i, "ui_fovea_light") if "ui_fovea_light" in images[i] else None
            if li is not None:
                row.append("fovea light layer: %.2f %% of the part lit, mean light %.4f" % (100.0 * float((li[..., 3] > 0.0).mean()), float(li[..., :3].mean())))
            ma = image(folder, i, "ui_wide_matte") if "ui_wide_matte" in images[i] else None
            if ma is not None:
                on = ma[..., 3] > 0.0
                row.append("wide matte: interface on %.2f %% of the part, shows through %.2f on average there" %
                           (100.0 * float(on.mean()), float(ma[..., 0][on].mean()) if on.any() else 0.0))
            if row:
                print("    frame %d: %s" % (i, "; ".join(row)))

    if "sheet" in opt:
        g = float(opt.get("gain", 1.0))
        for st in STAGES:
            have = [i for i in range(n) if lumas[st][i] is not None]
            if len(have) < 2:
                continue
            pick = [have[0], have[len(have) // 2], have[-1]]
            tiles = [np.clip(lumas[st][i] * g, 0, 1)[..., None].repeat(3, axis=-1) for i in pick]
            a, b = lumas[st][have[-2]], lumas[st][have[-1]]
            if a.shape == b.shape:
                tiles.append(np.clip(np.abs(b - a) * 8.0, 0, 1)[..., None].repeat(3, axis=-1))
            path = "%s_%s.png" % (opt["sheet"], st)
            AC.write_png(path, AC.side_by_side(tiles))
            print("  " + path + ": frames %s, and the change between the last two x 8" % ", ".join("%d" % i for i in pick))


if __name__ == "__main__":
    main(sys.argv[1:])
