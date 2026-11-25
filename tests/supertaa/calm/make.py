"""[fix100 20.11.2026] The inputs of gpu/periphery_test.exe calm <dir> from a sequence of periphery/psim.py.
   python3 calm/make.py <dir of psim.py's files> <kind> <out dir> [frames=40] [calm=1] [tiled=1] [tile=16] [feedback] [fresh_at]
fresh_at [fix101]: the frame in which the calm variant's statistics start from zero (LightTAA::Params::calm_fresh)."""
import os
import sys

import numpy as np

seq = dict(np.load(os.path.join(sys.argv[1], f"p_{sys.argv[2]}.npz")))
out = sys.argv[3]
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 40
calm = int(sys.argv[5]) if len(sys.argv) > 5 else 1
tiled = int(sys.argv[6]) if len(sys.argv) > 6 else 1
tile = int(sys.argv[7]) if len(sys.argv) > 7 else 16
feedback = float(sys.argv[8]) if len(sys.argv) > 8 and float(sys.argv[8]) > 0 else (0.93 if calm else 0.90)
fresh_at = int(sys.argv[9]) if len(sys.argv) > 9 else -1
os.makedirs(out, exist_ok=True)
H, W = seq["color"].shape[1:3]
with open(os.path.join(out, "calm.txt"), "w") as f:
    f.write(f"w {W}\nh {H}\nframes {frames}\ncalm {calm}\nfeedback {feedback}\ntiled {tiled}\ntile {tile}\nfresh_at {fresh_at}\n")
    for t in range(frames):
        f.write(f"jitter {t} {float(seq['jitter'][t][0]):.9g} {float(seq['jitter'][t][1]):.9g}\n")
for t in range(frames):
    c = np.concatenate([seq["color"][t].astype(np.float32), np.ones((H, W, 1), dtype=np.float32)], axis=-1)
    c.tofile(os.path.join(out, f"c{t:03d}.bin"))
    seq["mv"][t].astype(np.float32).tofile(os.path.join(out, f"m{t:03d}.bin"))
    (1.0 / seq["dist"][t].astype(np.float32)).astype(np.float32).tofile(os.path.join(out, f"d{t:03d}.bin"))
print(f"{out}: {frames} frames {W}x{H}, calm {calm}, tiled {tiled} ({tile}), feedback {feedback}")
