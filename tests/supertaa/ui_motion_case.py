"""[UI MOTION fix125] Cases for the interface's own motion in the fovea's supersampling TAA (gpu/supertaa_test.exe).

A view with interface drawn into it (writing of lines 1.2 - 1.6 pixels wide, a flat panel with dark writing, a veil over
the scene without detail of its own) over a scene that moves another way, rendered through the frame's jitter like the
engine does it: the colour (with interface), the view's copies without and with GUI, the scene's motion vectors, the
depth of the scene, and the block of the interface's motion (d3d12::UiMotion::Constants) of every frame.

   ui_motion_case.py make <dir> <case> <mode>     case: locked | parallax | weapon | slide | slow   mode: 1 = with the interface's motion,
                                                  2 = the same inputs along the scene's motion vectors (fix124)
   ui_motion_case.py check <dir>                  after supertaa_test.exe <dir>: the last frame against the picture the
                                                  frame would have with many samples per pixel, on the writing and off it

cases:  locked    the interface stands still on the screen, the scene moves 5.3 / 2.1 pixels a frame under it (a panel in
                  front of the game camera while the player walks or turns)
        parallax  the scene stands still (nothing behind the panel: the vectors of the background), the interface moves
                  0.6 / 0.25 pixels a frame (the item screen's text with the head moving sideways)
        weapon    the scene moves 1 pixel a frame, the interface 9 / 4 pixels and turns half a degree a frame (the HUD at
                  the weapon)
        slide     [fix126] nothing moves on the screen, but the writing slides 5 pixels a frame inside the interface for
                  four frames (a list's rows when the selection changes): what is left where the writing was
"""
import os
import struct
import sys

import numpy as np

W = H = 192
FRAMES = 28
MAX = 16


def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def encode(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.maximum(c, 0.0) ** (1.0 / 2.4) - 0.055)


def scene(x, y):
    s = 0.30 + 0.12 * np.sin(0.55 * x + 0.2 * y) + 0.10 * np.sin(0.23 * x - 0.61 * y) + 0.08 * np.sign(np.sin(1.3 * x) * np.sin(1.1 * y))
    return np.stack([s, 0.95 * s, 0.9 * s], axis=-1)


def interface(u, v, slide=0.0):
    """colour and alpha of the interface at its own coordinates, and where its writing is (`slide`: the writing moved
    that many pixels to the right inside the interface: a list whose rows slide when the selection changes)"""
    panel_u = u
    u = u - slide
    col = np.zeros(u.shape + (3,))
    alpha = np.zeros(u.shape)
    writing = np.zeros(u.shape, bool)
    # writing over the scene: upright strokes, lines, a slanted stroke
    a = (np.abs((u % 7.0) - 3.5) < 0.7) & (u > 40) & (u < 150) & (v > 50) & (v < 70)
    b = (np.abs((v % 9.0) - 4.5) < 0.6) & (u > 60) & (u < 140) & (v > 80) & (v < 105)
    c = (np.abs(u - v - 20.0) < 0.8) & (u > 100) & (u < 150) & (v > 30) & (v < 115)
    w = a | b | c
    col[w] = 0.92
    alpha[w] = 1.0
    writing |= w
    # a flat panel with dark writing
    p = (panel_u > 20) & (panel_u < 62) & (v > 120) & (v < 162)
    col[p] = 0.6
    alpha[p] = 1.0
    d = p & (np.abs((u % 6.0) - 3.0) < 0.7) & (v > 128) & (v < 154)
    col[d] = 0.05
    writing |= d
    # a veil without detail of its own
    veil = (panel_u > 100) & (panel_u < 176) & (v > 122) & (v < 176) & ~w
    col[veil] = 0.0
    alpha[veil] = 0.5
    return col, alpha, writing, veil


CASES = {
    "locked": dict(scene_v=(5.3, 2.1), ui_v=(0.0, 0.0), turn=0.0),
    "parallax": dict(scene_v=(0.0, 0.0), ui_v=(0.6, 0.25), turn=0.0),
    "weapon": dict(scene_v=(1.0, 0.0), ui_v=(-1.5, 0.7), turn=np.radians(0.5), swing=(9.0, 4.0)),
    # [fix126] nothing moves on the screen; from frame SLIDE_AT on the writing slides 5 pixels a frame inside the
    # interface for SLIDE_FRAMES frames (the rows of a list when the selection changes), then stands
    "slide": dict(scene_v=(0.0, 0.0), ui_v=(0.0, 0.0), turn=0.0, slide=5.0),
    # [fix128] ... and 1.5 pixels a frame for six frames (the row that lost the selection sliding back)
    "slow": dict(scene_v=(0.0, 0.0), ui_v=(0.0, 0.0), turn=0.0, slide=1.5, slide_frames=6),
}
SLIDE_AT = 16
SLIDE_FRAMES = 4


def slide_of(case, t):
    return CASES[case].get("slide", 0.0) * min(max(t - SLIDE_AT + 1, 0), CASES[case].get("slide_frames", SLIDE_FRAMES))


def placement(case, t):
    """the interface at frame t: screen = R(angle) (own - pivot) + pivot + offset"""
    c = CASES[case]
    if "swing" in c:  # back and forth, so it stays in the picture
        ph = np.sin(t * 0.45)
        off = np.array([c["swing"][0] * 4.0 * ph, c["swing"][1] * 4.0 * ph])
        ang = c["turn"] * 6.0 * ph
    else:
        off = np.array(c["ui_v"]) * t
        ang = c["turn"] * t
    return ang, off


PIVOT = np.array([96.0, 96.0])


def to_own(case, t, x, y):
    ang, off = placement(case, t)
    dx, dy = x - PIVOT[0] - off[0], y - PIVOT[1] - off[1]
    ca, sa = np.cos(-ang), np.sin(-ang)
    return ca * dx - sa * dy + PIVOT[0], sa * dx + ca * dy + PIVOT[1]


def compose(case, t, x, y):
    sv = CASES[case]["scene_v"]
    hud = scene(x - sv[0] * t, y - sv[1] * t)
    u, v = to_own(case, t, x, y)
    col, alpha, writing, veil = interface(u, v, slide_of(case, t))
    fin = hud * (1.0 - alpha[..., None]) + col * alpha[..., None]
    return hud, fin, writing, veil


def motion_block(case, t):
    """the interface's motion of frame t as d3d12::UiMotion::Constants (the panels' motion only)"""
    a1, o1 = placement(case, t)
    a0, o0 = placement(case, t - 1)
    da = a0 - a1
    A = np.array([[np.cos(da), -np.sin(da)], [np.sin(da), np.cos(da)]])
    c1 = PIVOT + o1
    c0 = PIVOT + o0
    b = c0 - A @ c1  # previous = A (now - c1) + c0
    d = np.zeros(9)
    d[0], d[1], d[2] = A[0, 0] - 1.0, A[0, 1] * H / W, b[0] / W
    d[3], d[4], d[5] = A[1, 0] * W / H, A[1, 1] - 1.0, b[1] / H
    panel = np.zeros((3, 4), np.float32)
    panel[:, :3] = d.reshape(3, 3)
    panel[0, 3] = 1.0
    return panel.tobytes() + bytes(MAX * 16) + bytes(MAX * 3 * 16) + struct.pack("<4f", 0.0, 0.0, 0.0, 0.0), d


def make(out, case, mode):
    os.makedirs(out, exist_ok=True)
    jit = [(halton(i + 1, 2) - 0.5, halton(i + 1, 3) - 0.5) for i in range(FRAMES)]

    with open(os.path.join(out, "meta.txt"), "w") as f:
        f.write(f"width {W}\nheight {H}\nout_width {W}\nout_height {H}\nframes {FRAMES}\nscale 2\nui_motion {mode}\ncase {case}\n")
        for j in jit:
            f.write(f"jitter {j[0]:.9g} {j[1]:.9g}\n")

    qy, qx = np.mgrid[0:H, 0:W].astype(np.float64)
    sv = CASES[case]["scene_v"]

    for t in range(FRAMES):
        x = qx + 0.5 - jit[t][0]
        y = qy + 0.5 - jit[t][1]
        hud, fin, _, _ = compose(case, t, x, y)
        base = os.path.join(out, f"frame{t:02d}")
        fin.astype(np.float32).tofile(base + "_color.bin")
        hud.astype(np.float32).tofile(base + "_hud.bin")
        fin.astype(np.float32).tofile(base + "_fin.bin")
        mv = np.zeros((H, W, 2), np.float32)
        mv[..., 0] = -sv[0]
        mv[..., 1] = -sv[1]
        mv.tofile(base + "_mv.bin")
        np.full((H, W), 0.1 / 50.0, np.float32).tofile(base + "_depth.bin")
        blob, _ = motion_block(case, t)
        open(base + "_um.bin", "wb").write(blob)

    print("case written:", out, case, "mode", mode)


def check_slide(out, case):
    """the writing that slid inside the interface: per frame, the error on the writing where it is now, and what is left
    where it was before the slide and is no more"""
    n = 4
    qy, qx = np.mgrid[0:H, 0:W].astype(np.float64)

    def picture(t):
        acc = np.zeros((H, W, 3))
        wr = np.zeros((H, W))
        for sy in range(n):
            for sx in range(n):
                _, fin, writing, _ = compose(case, t, qx + (sx + 0.5) / n, qy + (sy + 0.5) / n)
                acc += to_linear(fin)
                wr += writing
        return encode(acc / (n * n)), wr > 0.25 * n * n

    _, before = picture(SLIDE_AT - 1)
    print(f"{out}: case {case}, the writing slides in frames {SLIDE_AT} - {SLIDE_AT + SLIDE_FRAMES - 1}")
    frames = CASES[case].get("slide_frames", SLIDE_FRAMES)
    for t in (SLIDE_AT - 1, SLIDE_AT + 1, SLIDE_AT + frames - 1, SLIDE_AT + frames, SLIDE_AT + frames + 2, SLIDE_AT + frames + 5):
        truth, on = picture(t)
        got = np.fromfile(os.path.join(out, "gpu", f"frame{t:02d}_out.bin"), np.float16).reshape(H, W, 4)[..., :3].astype(np.float64)
        err = np.abs(got - truth).max(axis=-1)
        near = on.copy()
        for _ in range(2):
            near = near | np.roll(near, 1, 0) | np.roll(near, -1, 0) | np.roll(near, 1, 1) | np.roll(near, -1, 1)
        old = before & ~near
        left = f"{err[old].mean():.4f} ({int(old.sum())} px)" if old.any() else "-"
        print(f"  frame {t:2d}: on the writing {err[on].mean():.4f}; where it was and is no more {left}")


def check(out):
    meta = dict(line.split()[:2] for line in open(os.path.join(out, "meta.txt")) if not line.startswith("jitter"))
    case = meta["case"]
    if "slide" in CASES[case]:
        return check_slide(out, case)
    t = FRAMES - 1
    n = 6
    qy, qx = np.mgrid[0:H, 0:W].astype(np.float64)
    acc = np.zeros((H, W, 3))
    wr = np.zeros((H, W))
    ve = np.zeros((H, W))

    for sy in range(n):
        for sx in range(n):
            _, fin, writing, veil = compose(case, t, qx + (sx + 0.5) / n, qy + (sy + 0.5) / n)
            acc += to_linear(fin)
            wr += writing
            ve += veil

    truth = encode(acc / (n * n))
    got = np.fromfile(os.path.join(out, "gpu", f"frame{t:02d}_out.bin"), np.float16).reshape(H, W, 4)[..., :3].astype(np.float64)
    guide = np.fromfile(os.path.join(out, "gpu", f"frame{t:02d}_guide.bin"), np.float16).reshape(H, W, 4).astype(np.float64)
    inner = np.zeros((H, W), bool)
    inner[12:-12, 12:-12] = True
    on = (wr > 0.25 * n * n) & inner
    # the scene's detail under the veil, away from its edge and from the writing
    veil_in = (ve == n * n) & inner
    for _ in range(3):
        veil_in = veil_in & np.roll(veil_in, 1, 0) & np.roll(veil_in, -1, 0) & np.roll(veil_in, 1, 1) & np.roll(veil_in, -1, 1)
    err = np.abs(got - truth).max(axis=-1)
    sv = CASES[case]["scene_v"]
    _, d = motion_block(case, t)
    u = (qx + 0.5) / W
    v = (qy + 0.5) / H
    e = d[6] * u + d[7] * v + d[8]
    um_x = (d[0] * u + d[1] * v + d[2] - u * e) / (1.0 + e) * W
    um_y = (d[3] * u + d[4] * v + d[5] - v * e) / (1.0 + e) * H
    own = (np.abs(guide[..., 0] - um_x) < 0.02) & (np.abs(guide[..., 1] - um_y) < 0.02)
    scn = (np.abs(guide[..., 0] + sv[0]) < 0.02) & (np.abs(guide[..., 1] + sv[1]) < 0.02)
    differ = max(abs(um_x + sv[0]).max(), abs(um_y + sv[1]).max()) > 0.05
    print(f"{out}: case {case} mode {meta['ui_motion']}")
    print(f"  writing ({int(on.sum())} pixels): error against the many-sample picture mean {err[on].mean():.4f}  rms {np.sqrt((err[on] ** 2).mean()):.4f}  "
          f"95th {np.percentile(err[on], 95):.4f}")
    print(f"  under the veil ({int(veil_in.sum())} pixels): mean {err[veil_in].mean():.4f}  rms {np.sqrt((err[veil_in] ** 2).mean()):.4f}")
    if differ:
        print(f"  history fetched along the interface's motion: {100.0 * own[on].mean():.1f} % of the writing's pixels, "
              f"{100.0 * own[veil_in].mean():.1f} % under the veil (along the scene's: {100.0 * scn[veil_in].mean():.1f} %)")
    w = guide[..., 2]
    print(f"  weight the history may keep: on the writing {w[on].mean():.2f}, under the veil {w[veil_in].mean():.2f}")


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1] == "make":
        make(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        check(sys.argv[2])
    else:
        raise SystemExit(__doc__)
