"""Reads a capture of the mod's supersampling TAA (d3d12::SuperTAACapture: quadview_dump/supertaa_captureNN).

Every image is a file <name>_raw_x<x>_y<y>_<w>x<h>_<FORMAT>.bin in a folder frameNN, rows packed, as the GPU stored it.
capture.txt holds the sizes, the parts' corners, the settings and per frame the jitter and the constants of the passes.
"""
import glob
import os
import re

import numpy as np


def _decode(raw, w, h, fmt):
    """-> float32 array [h, w, channels] with the values the format stores"""
    if fmt in ("R10G10B10A2_UNORM", "R10G10B10A2_TYPELESS"):
        v = np.frombuffer(raw, dtype="<u4").reshape(h, w)
        return np.stack([(v & 1023), ((v >> 10) & 1023), ((v >> 20) & 1023)], axis=-1).astype(np.float32) / 1023.0
    if fmt in ("R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB", "R8G8B8A8_TYPELESS"):
        return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32) / 255.0
    if fmt in ("B8G8R8A8_UNORM", "B8G8R8A8_UNORM_SRGB", "B8G8R8A8_TYPELESS"):
        return np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)[..., 2::-1].astype(np.float32) / 255.0
    if fmt in ("R16G16B16A16_FLOAT", "R16G16B16A16_TYPELESS"):
        return np.frombuffer(raw, dtype="<f2").reshape(h, w, 4).astype(np.float32)
    if fmt == "R16G16B16A16_SNORM":
        return np.maximum(np.frombuffer(raw, dtype="<i2").reshape(h, w, 4).astype(np.float32) / 32767.0, -1.0)
    if fmt == "R16G16B16A16_UNORM":
        return np.frombuffer(raw, dtype="<u2").reshape(h, w, 4).astype(np.float32) / 65535.0
    if fmt in ("R16G16_FLOAT", "R16G16_TYPELESS"):
        return np.frombuffer(raw, dtype="<f2").reshape(h, w, 2).astype(np.float32)
    if fmt == "R16G16_SNORM":
        return np.maximum(np.frombuffer(raw, dtype="<i2").reshape(h, w, 2).astype(np.float32) / 32767.0, -1.0)
    if fmt in ("R32G32_FLOAT", "R32G32_TYPELESS"):
        return np.frombuffer(raw, dtype="<f4").reshape(h, w, 2).copy()
    if fmt == "R32G32B32A32_FLOAT":
        return np.frombuffer(raw, dtype="<f4").reshape(h, w, 4).copy()
    if fmt in ("R32_FLOAT", "R32_TYPELESS", "D32_FLOAT"):
        return np.frombuffer(raw, dtype="<f4").reshape(h, w, 1).copy()
    if fmt in ("R32G8X24_TYPELESS", "R32_FLOAT_X8X24_TYPELESS", "D32_FLOAT_S8X24_UINT"):
        # (the depth plane as the copy holds it: 4 bytes per texel)
        return np.frombuffer(raw, dtype="<f4").reshape(h, w, 1).copy()
    if fmt in ("R24G8_TYPELESS", "D24_UNORM_S8_UINT", "R24_UNORM_X8_TYPELESS"):
        return ((np.frombuffer(raw, dtype="<u4").reshape(h, w, 1) & 0xFFFFFF).astype(np.float64) / 16777215.0).astype(np.float32)
    if fmt == "R16_FLOAT":
        return np.frombuffer(raw, dtype="<f2").reshape(h, w, 1).astype(np.float32)
    if fmt in ("R16_UNORM", "R16_TYPELESS", "D16_UNORM"):
        return np.frombuffer(raw, dtype="<u2").reshape(h, w, 1).astype(np.float32) / 65535.0
    if fmt == "R11G11B10_FLOAT":
        v = np.frombuffer(raw, dtype="<u4").reshape(h, w)

        def small(bits, mant_bits):
            e = (bits >> mant_bits) & 31
            m = (bits & ((1 << mant_bits) - 1)).astype(np.float64)
            return np.where(e == 0, m / (1 << mant_bits) * 2.0 ** -14, (1.0 + m / (1 << mant_bits)) * 2.0 ** (e.astype(np.float64) - 15)).astype(np.float32)

        return np.stack([small(v & 0x7FF, 6), small((v >> 11) & 0x7FF, 6), small((v >> 22) & 0x3FF, 5)], axis=-1)
    raise ValueError("format not decoded: " + fmt)


_NAME = re.compile(r"^(?P<name>.+)_raw_x(?P<x>\d+)_y(?P<y>\d+)_(?P<w>\d+)x(?P<h>\d+)_(?P<fmt>[A-Za-z0-9_]+)\.bin$")


def read_image(folder, name):
    """the image `name` of a frame folder -> (array, format) or (None, None)"""
    for path in sorted(glob.glob(os.path.join(folder, name + "_raw_x*.bin"))):
        m = _NAME.match(os.path.basename(path))
        if m is None or m.group("name") != name:
            continue
        w, h, fmt = int(m.group("w")), int(m.group("h")), m.group("fmt")
        with open(path, "rb") as f:
            raw = f.read()
        return _decode(raw, w, h, fmt), fmt
    return None, None


def _numbers(text):
    out = []
    for tok in text.replace("x", " ").split():
        try:
            out.append(float(tok))
        except ValueError:
            pass
    return out


def _pairs(line):
    """'key value key value ...' -> dict (values as floats where they are numbers; several numbers after a key = a list)"""
    out = {}
    key = None
    for tok in line.split():
        try:
            v = float(tok)
        except ValueError:
            v = None
        if v is None and not (tok in ("true", "false")):
            key = tok.rstrip(":")
            out.setdefault(key, [])
        elif key is not None:
            out[key].append(v if v is not None else (1.0 if tok == "true" else 0.0))
    return {k: (v[0] if len(v) == 1 else v) for k, v in out.items() if v}


class Capture:
    def __init__(self, folder):
        self.folder = folder
        self.header = {}
        self.frames = []  # per frame: dict(jitter, constants of the three passes, ...)
        self.settings = {}
        self.old_settings = {}
        self.layout = {}
        cur = None
        with open(os.path.join(folder, "capture.txt"), errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("frames_recorded"):
                    self.header["frames_recorded"] = int(line.split()[1])
                    break
                if line.startswith("view "):
                    p = _pairs(line)
                    self.view = [int(v) for v in p["view"]]
                    self.image = [int(v) for v in p["fovea_image"]]
                    self.history = [int(v) for v in p["history"]]
                    self.scale = int(p["scale"])
                elif line.startswith("part_in "):
                    self.part_in = [int(v) for v in _numbers(line[8:])]
                elif line.startswith("part_out "):
                    self.part_out = [int(v) for v in _numbers(line[9:])]
                elif line.startswith("part_history "):
                    self.part_history = [int(v) for v in _numbers(line[13:])]
                elif line.startswith("mv_scale "):
                    self.mv_scale = _numbers(line[9:])[:2]
                elif line.startswith("depth_k "):
                    p = _pairs(line)
                    self.depth_k = p["depth_k"]
                    self.depth_k_valid = bool(p.get("valid", 1))
                    self.reverse_z = bool(p.get("reverse_z", 1))
                elif line.startswith("old_taa_settings:"):
                    self.old_settings = _pairs(line.split(":", 1)[1])
                elif line.startswith("old_taa "):
                    self.header["old_taa"] = int(line.split()[1])
                elif line.startswith("settings:"):
                    self.settings = _pairs(line.split(":", 1)[1])
                elif line.startswith("layout:"):
                    self.layout = _pairs(line.split(":", 1)[1])
                elif re.match(r"^frame \d+: jitter", line):
                    p = _pairs(line.split(":", 1)[1])
                    cur = dict(index=int(line.split()[1].rstrip(":")), jitter=p["jitter"], reset_asked=int(p.get("reset_asked", 0)),
                               old_taa=int(p.get("old_taa", 0)), engine_frame=p.get("engine_frame"), rect=p.get("fovea_rectangle"))
                    self.frames.append(cur)
                elif cur is not None and line.startswith("analyze:"):
                    cur["analyze"] = _pairs(line.split(":", 1)[1].replace("x", " ", 1))
                elif cur is not None and line.startswith("accumulate:"):
                    text = line.split(":", 1)[1].replace("(sigma", "sigma").replace("px)", "")
                    cur["accumulate"] = _pairs(re.sub(r"(\d+)x(\d+)", r"\1 \2", text))
                elif cur is not None and line.startswith("resolve:"):
                    cur["resolve"] = _pairs(re.sub(r"(\d+)x(\d+)", r"\1 \2", line.split(":", 1)[1]))
        self.count = self.header.get("frames_recorded", len(self.frames))

    def frame_dir(self, i):
        return os.path.join(self.folder, "frame%02d" % i)

    def image_of(self, i, name):
        return read_image(self.frame_dir(i), name)[0]

    def motion_px(self, i):
        """motion of the part in view pixels (previous position = pixel + motion)"""
        m = self.image_of(i, "motion")
        if m is None:
            return None
        return np.stack([m[..., 0] * self.mv_scale[0] * self.view[0], m[..., 1] * self.mv_scale[1] * self.view[1]], axis=-1).astype(np.float32)

    def depth(self, i):
        d = self.image_of(i, "depth")
        return None if d is None else d[..., 0]


if __name__ == "__main__":
    import sys

    c = Capture(sys.argv[1])
    print("view", c.view, "image", c.image, "history", c.history, "scale", c.scale)
    print("parts", c.part_in, c.part_out, c.part_history, "mv_scale", c.mv_scale, "depth_k", c.depth_k)
    print("settings", c.settings)
    print("frames", c.count)
    for fr in c.frames[:2]:
        print(fr)
