"""[fix82 17.11.2026] The game's menus left in the fovea view's picture for the supersampling TAA (fix82: the fovea views go
to the mod's anti-aliasing as the engine drew them, interface included; the eye pass adds no interface inside the fovea).

   python3 ui_inside_test.py <capture folder> [--sheet=png] [--frames=8]

The capture is one of the supersampling TAA from the game (quadview_dump/supertaa_captureNN) with a menu or the interface in
its part:
  made by fix81     the fovea view's own colour is in the first 8 frames (ui_fovea_color; the pass's input had the
                    interface's light taken out). Those frames go through the reference model from an empty history.
  made by fix82     the pass's input is the view with its interface; the game's own output is compared.
Printed, where the picture is bright (the interface's lit parts over a dark panel; luma over 0.25): how far the anti-aliased
picture is from the frame's own pixel read where the jitter put it (the interface as the engine drew it), and how much of
its detail it keeps (mean difference between neighbouring pixels).
RE7 captures of 17.11.2026 16:0x (fix81), 8 frames from an empty history, bright pixels: distance / detail kept
  01 dialog 0.9 % / 89 %, 02 inventory 1.3 % / 78 %, 06 pause menu 0.9 % / 92 %, 05 objective text 3.5 % / 78 %,
  04 inventory with the head moving fast (the menu 1.2 pixels a frame against the scene's motion vectors) 3.4 % / 59 %.
The white bar of 06 and the item cards of 02 - 04 come out whole over the brightest scene behind them (fix81's light layer
had holes there in the hidden scene's shape: what the user saw as see-through bright parts)."""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # (also when Python runs isolated, -I)
import analyze_capture as AC  # noqa: E402
import capture_io as CIO  # noqa: E402
import resolvers as R  # noqa: E402
from capture_io import Capture  # noqa: E402


def place(model, cap):
    """the model on the capture's part of the view"""
    model.half = True
    model.view = tuple(cap.view)
    model.in_origin = tuple(cap.part_in[:2])
    model.hist_full = tuple(cap.history)
    model.hist_origin = tuple(cap.part_history[:2])
    model.hist_part = tuple(cap.part_history[2:])


def through_model(cap, cols):
    model, outs = None, []
    for i in range(cols.shape[0]):
        fr = cap.frames[i]
        s, fl = AC.model_settings(cap, fr, {})
        if model is None:
            model = R.NewTAA(**dict({k: v for k, v in s.items() if k in R.NewTAA.DEFAULTS}, af_clip=R.NewTAA.DEFAULTS["af_clip"]))
            place(model, cap)
        first = i == 0
        lin, guide, boxc, boxe = model.analyze(cols[i], cap.motion_px(i), None, fr["jitter"], z=cap.depth(i), depth_k=cap.depth_k, reverse_z=fl["reverse_z"],
                                               reset=first or fl["reset_a"], have_depth=True, dist_test=fl["dist_test"])
        model.accumulate(lin, guide, boxc, boxe, fr["jitter"], reset=first or fl["reset_c"])
        outs.append(np.clip(model.resolve(), 0, 1))
    return np.stack(outs)


def unjitter(img, jx, jy):
    """the frame's own picture read where its jitter put the point: U(p) = R(p + jitter), cubic"""
    return np.stack([ndimage.shift(img[..., c], (-jy, -jx), order=3, mode="nearest") for c in range(img.shape[-1])], axis=-1)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opt = {a[2:].split("=")[0]: (a.split("=")[1] if "=" in a else "1") for a in argv if a.startswith("--")}
    cap = Capture(args[0])
    own = [CIO.read_image(cap.frame_dir(i), "ui_fovea_color")[0] for i in range(min(cap.count, 8))]
    if all(o is not None for o in own) and len(own) >= 3:
        n = min(len(own), int(opt.get("frames", 8)))
        cols = np.stack([o[..., :3] for o in own[:n]])
        taa = through_model(cap, cols)
        how = "fix81 capture: the view's own colour of the first %d frames through the reference model, from an empty history" % n
    else:
        n = min(cap.count, int(opt.get("frames", 64)))
        cols = np.stack([cap.image_of(i, "color")[..., :3] for i in range(n)])
        taa = np.stack([np.clip(cap.image_of(i, "out")[..., :3], 0, 1) for i in range(n)])
        how = "the pass's input (the view with its interface) against the game's own output, %d frames" % n
    t = n - 1
    jx, jy = cap.frames[t]["jitter"]
    frame = unjitter(cols[t], jx, jy)
    la, lf = AC.luma(taa[t]), AC.luma(frame)
    bright = lf > 0.25
    print("%s: %s" % (args[0], how))
    if bright.mean() < 0.002:
        print("  hardly anything bright in the part (%.2f %%): no menu in it?" % (100.0 * bright.mean()))
        return
    grad = lambda x: np.abs(np.diff(x, axis=1))[bright[:, 1:]].mean()
    print("  bright pixels %.1f %% of the part; the anti-aliased picture there: %.2f %% from the frame's own pixel (mean %+.2f %%), detail kept %.0f %%" %
          (100.0 * bright.mean(), 100.0 * np.abs(la - lf)[bright].mean(), 100.0 * (la - lf)[bright].mean(), 100.0 * grad(la) / max(grad(lf), 1e-6)))
    if "sheet" in opt:
        AC.write_png(opt["sheet"], AC.side_by_side([np.clip(cols[t], 0, 1), np.clip(frame, 0, 1), taa[t], np.clip(np.abs(taa[t] - frame) * 4.0, 0, 1)]))
        print("  %s: the frame, the frame without its jitter, the anti-aliased picture, their difference x 4" % opt["sheet"])


if __name__ == "__main__":
    main(sys.argv[1:])
