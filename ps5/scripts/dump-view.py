#!/usr/bin/env python3
"""Convert texture dumps from the PS5 debug mailbox ("PDMP" files) to PNGs and
print per-channel statistics.

    python ps5/scripts/dump-view.py ps5/logs/dumps/*.bin

The game writes them after a draw whose shader hash is in debug/dump.req
(see ps5-ctl.py dump)."""
import struct
import sys

import numpy as np
from PIL import Image


def b10g11r11(raw):
    v = raw.view(np.uint32)

    def unpack(bits, mantissa_bits):
        exponent = (bits >> mantissa_bits) & 0x1f
        mantissa = bits & ((1 << mantissa_bits) - 1)
        value = np.where(exponent == 0, mantissa / (1 << mantissa_bits) * 2.0 ** -14,
                         (1 + mantissa / (1 << mantissa_bits)) * 2.0 ** (exponent.astype(np.float64) - 15))
        return np.where(exponent == 31, np.inf, value)

    r = unpack(v & 0x7ff, 6)
    g = unpack((v >> 11) & 0x7ff, 6)
    b = unpack((v >> 22) & 0x3ff, 5)
    return np.stack([r, g, b], -1)


def decode(fmt, raw, w, h):
    """Return float array (h, w, c)."""
    if fmt in (37, 43, 44, 50):  # RGBA8 / BGRA8 unorm/srgb
        a = raw.view(np.uint8).reshape(h, w, 4).astype(np.float64) / 255
        return a[..., [2, 1, 0, 3]] if fmt in (44, 50) else a
    if fmt == 9:
        return raw.view(np.uint8).reshape(h, w, 1) / 255.0
    if fmt == 64:  # A2B10G10R10 unorm
        v = raw.view(np.uint32).reshape(h, w)
        return np.stack([(v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023, (v >> 30) / 3], -1)
    if fmt == 91:
        return raw.view(np.uint16).reshape(h, w, 4) / 65535.0
    if fmt == 97:
        return raw.view(np.float16).reshape(h, w, 4).astype(np.float64)
    if fmt == 76:
        return raw.view(np.float16).reshape(h, w, 1).astype(np.float64)
    if fmt == 83:
        return raw.view(np.float16).reshape(h, w, 2).astype(np.float64)
    if fmt in (100, 126, 129, 130):
        return raw.view(np.float32).reshape(h, w, 1).astype(np.float64)
    if fmt == 103:
        return raw.view(np.float32).reshape(h, w, 2).astype(np.float64)
    if fmt == 109:
        return raw.view(np.float32).reshape(h, w, 4).astype(np.float64)
    if fmt == 124:
        return raw.view(np.uint16).reshape(h, w, 1) / 65535.0
    if fmt == 122:
        return b10g11r11(raw).reshape(h, w, 3)
    raise ValueError(f"format {fmt} not handled")


def main():
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        if data[:4] != b"PDMP":
            print(path, "not a dump")
            continue
        w, h, fmt, texel = struct.unpack_from("<4I", data, 4)
        raw = np.frombuffer(data[20:20 + w * h * texel], dtype=np.uint8).copy()
        img = decode(fmt, raw, w, h)
        finite = np.where(np.isfinite(img), img, np.nan)
        stats = " ".join(f"c{c}[min={np.nanmin(finite[..., c]):.4g} max={np.nanmax(finite[..., c]):.4g} "
                         f"mean={np.nanmean(finite[..., c]):.4g}]" for c in range(img.shape[-1]))
        nonfinite = int((~np.isfinite(img)).sum())
        print(f"{path}: {w}x{h} fmt={fmt} {stats}" + (f" nonfinite={nonfinite}" if nonfinite else ""))
        rgb = np.nan_to_num(finite[..., :3], nan=0, posinf=1)
        if rgb.shape[-1] == 1:
            rgb = np.repeat(rgb, 3, -1)
        elif rgb.shape[-1] == 2:
            rgb = np.concatenate([rgb, np.zeros_like(rgb[..., :1])], -1)
        peak = np.percentile(rgb, 99.5)
        if peak > 1.0 or (fmt not in (37, 43, 44, 50, 9, 64, 91) and peak > 0):
            rgb = rgb / peak  # normalise HDR and float data for viewing
        Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(path[:-4] + ".png")


if __name__ == "__main__":
    main()
