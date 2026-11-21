#!/usr/bin/env python3
"""Generate PlayStation button glyphs for the game's UI icon atlas.

The game draws its button prompts from one 1024x1024 BC7 atlas of Switch
glyphs (A/B/X/Y, L/R/ZL/ZR, +/-). This script draws PlayStation replacements
in the same style (white shape, 4 px dark edge at alpha 191, dark symbol),
encodes them as BC7 blocks and writes gfx-bridge/ps_button_glyphs.h. The
bridge patches those blocks into the atlas when it is first uploaded
(live_texture_upload.c), recognising the atlas by a hash of its blocks.

Everything in the header is drawn here; no game art is copied. The atlas dump
(a PDMP file from debug/dump.req, BC7 blocks) is read only to compute that
hash and to write a preview of the patched atlas.

    python ps5/scripts/gen-ps-glyphs.py ps5/logs/dumps/dump-2db3327d9fbc5bb3-t128.bin

Button mapping (horizon/hid_ps4.c), by position: A=Circle, B=Cross, X=Triangle, Y=Square,
L/R=L1/R1, ZL/ZR=L2/R2, Plus=OPTIONS, Minus=touch pad.
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "gfx-bridge", "ps_button_glyphs.h")
ATLAS = 1024
SS = 4  # supersampling
EDGE = (51, 51, 51, 191)
FILL = (255, 255, 255, 255)
INK = (51, 51, 51, 191)

# Icon bounding boxes in the atlas (x0, y0, x1, y1), measured from its alpha.
DISCS = {
    "circle": (249, 345, 315, 411),    # A (right)
    "cross": (937, 417, 1003, 483),    # B (bottom)
    "triangle": (713, 605, 779, 671),  # X
    "square": (9, 609, 75, 675),       # Y
    "options": (9, 521, 75, 587),      # +
    "touchpad": (777, 517, 843, 583),  # -
}
SHOULDERS = {  # label, rounded top corner on the left
    "L1": ((271, 520, 341, 580), True),   # L
    "R1": ((535, 524, 605, 584), False),  # R
    "L2": ((95, 612, 165, 672), True),    # ZL
    "R2": ((183, 612, 253, 672), False),  # ZR
}


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"):
        for directory in ("C:/Windows/Fonts", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/truetype/liberation"):
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    raise SystemExit("no bold sans font found")


def disc(w, h, symbol):
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, r = W / 2, H / 2, 32.5 * SS
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=EDGE)
    ri = r - 4 * SS
    d.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=FILL)
    s = SS
    if symbol == "cross":
        k = 11.5 * s
        d.line((cx - k, cy - k, cx + k, cy + k), fill=INK, width=int(5.5 * s))
        d.line((cx - k, cy + k, cx + k, cy - k), fill=INK, width=int(5.5 * s))
    elif symbol == "circle":
        k = 13 * s
        d.ellipse((cx - k, cy - k, cx + k, cy + k), outline=INK, width=int(5 * s))
    elif symbol == "triangle":
        top, half, base = cy - 13 * s, 14 * s, cy + 10 * s
        d.line((cx, top, cx + half, base, cx - half, base, cx, top), fill=INK, width=int(5 * s), joint="curve")
    elif symbol == "square":
        k = 11 * s
        d.rectangle((cx - k, cy - k, cx + k, cy + k), outline=INK, width=int(5 * s))
    elif symbol == "options":
        for dy in (-8, 0, 8):
            d.rounded_rectangle((cx - 13 * s, cy + (dy - 2) * s, cx + 13 * s, cy + (dy + 2) * s), radius=2 * s, fill=INK)
    elif symbol == "touchpad":
        d.rounded_rectangle((cx - 16 * s, cy - 10 * s, cx + 16 * s, cy + 10 * s), radius=4 * s, outline=INK,
                            width=int(4 * s))
    return img.resize((w, h), Image.LANCZOS)


def shoulder(w, h, label, left):
    W, H = w * SS, h * SS
    s = SS

    def shape(inset):
        # small corners everywhere, a large rounded top corner on the button's side
        yy, xx = np.mgrid[0:H, 0:W] + 0.5
        x0, y0, x1, y1 = (1 + inset) * s, (1 + inset) * s, W - (1 + inset) * s, H - (1 + inset) * s
        m = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1)
        for (cx, cy, r, sx, sy) in (
                (x0, y0, (16 if left else 4) * s, 1, 1), (x1, y0, (4 if left else 16) * s, -1, 1),
                (x0, y1, 4 * s, 1, -1), (x1, y1, 4 * s, -1, -1)):
            r = max(r - inset * s, 1)
            ccx, ccy = cx + sx * r, cy + sy * r
            corner = (sx * (xx - ccx) < 0) & (sy * (yy - ccy) < 0)
            m &= ~(corner & ((xx - ccx) ** 2 + (yy - ccy) ** 2 > r * r))
        return m

    outer, inner = shape(0), shape(4)
    arr = np.zeros((H, W, 4), np.uint8)
    arr[outer] = EDGE
    arr[inner] = FILL
    img = Image.fromarray(arr, "RGBA")
    d = ImageDraw.Draw(img)
    f = font(31 * s)
    box = d.textbbox((0, 0), label, font=f)
    tx = (W - (box[2] - box[0])) / 2 - box[0]
    ty = (H - (box[3] - box[1])) / 2 - box[1]
    d.text((tx, ty), label, font=f, fill=INK)
    return img.resize((w, h), Image.LANCZOS)


# ------------------------------------------------------------------ BC7 mode 6
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64])


def quantize(v):
    best = None
    for p in (0, 1):
        q = np.clip(np.round((v - p) / 2), 0, 127).astype(int)
        e = ((q << 1) | p).astype(float)
        err = ((e - v) ** 2).sum()
        if best is None or err < best[0]:
            best = (err, q, p)
    return best[1], best[2]


def encode_block(px):
    px = px.reshape(16, 4).astype(float)
    if np.ptp(px, axis=0).max() == 0:
        axis = np.array([1.0, 1, 1, 1]) / 2
    else:
        c = px - px.mean(0)
        w, v = np.linalg.eigh(c.T @ c)
        axis = v[:, -1]
    m = px.mean(0)
    t = (px - m) @ axis
    e0 = np.clip(m + t.min() * axis, 0, 255)
    e1 = np.clip(m + t.max() * axis, 0, 255)
    best = None
    for a, b in ((e0, e1), (px.min(0), px.max(0))):
        q0, p0 = quantize(a)
        q1, p1 = quantize(b)
        c0 = ((q0 << 1) | p0).astype(float)
        c1 = ((q1 << 1) | p1).astype(float)
        pal = ((64 - WEIGHTS)[:, None] * c0 + WEIGHTS[:, None] * c1 + 32) // 64
        dist = ((px[:, None, :] - pal[None]) ** 2).sum(-1)
        idx = dist.argmin(1)
        err = dist.min(1).sum()
        if best is None or err < best[0]:
            best = (err, q0, p0, q1, p1, idx)
    _, q0, p0, q1, p1, idx = best
    if idx[0] >= 8:
        q0, q1, p0, p1, idx = q1, q0, p1, p0, 15 - idx
    bits = 1 << 6
    pos = 7
    for ch in range(4):
        for q in (q0, q1):
            bits |= int(q[ch]) << pos
            pos += 7
    bits |= p0 << pos
    bits |= p1 << (pos + 1)
    pos += 2
    bits |= int(idx[0]) << pos
    pos += 3
    for i in range(1, 16):
        bits |= int(idx[i]) << pos
        pos += 4
    assert pos == 128
    return bits.to_bytes(16, "little")


def encode(img):
    a = np.array(img.convert("RGBA"))
    h, w = a.shape[:2]
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            out += encode_block(a[by:by + 4, bx:bx + 4])
    return bytes(out)


def decode_dds(blocks, w, h):
    hdr = struct.pack("<4sI", b"DDS ", 124)
    hdr += struct.pack("<IIIIII", 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(blocks), 0, 1) + b"\0" * 44
    hdr += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    hdr += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    hdr += struct.pack("<IIIII", 98, 3, 0, 1, 0)
    img = Image.open(io.BytesIO(hdr + blocks))
    img.load()
    return img.convert("RGBA")


def fnv64(data):
    # FNV-1a over 64-bit words, as live_texture_upload.c computes it.
    h = 0xcbf29ce484222325
    words = np.frombuffer(data, dtype="<u8")
    for word in words.tolist():
        h = ((h ^ word) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    assert raw[:4] == b"PDMP"
    w, h, fmt, texel = struct.unpack("<4I", raw[4:20])
    assert (w, h, fmt) == (ATLAS, ATLAS, 145), (w, h, fmt)
    atlas_blocks = bytearray(raw[20:20 + ATLAS * ATLAS])
    atlas_hash = fnv64(bytes(atlas_blocks))

    patches = []
    for name, box in DISCS.items():
        patches.append((name, box, lambda w, h, n=name: disc(w, h, n)))
    for label, (box, left) in SHOULDERS.items():
        patches.append((label, box, lambda w, h, l=label, lt=left: shoulder(w, h, l, lt)))

    entries = []
    preview_parts = []
    for name, (x0, y0, x1, y1), draw in patches:
        X0, Y0, X1, Y1 = x0 // 4 * 4, y0 // 4 * 4, -(-x1 // 4) * 4, -(-y1 // 4) * 4
        icon = draw(x1 - x0, y1 - y0)
        tile = Image.new("RGBA", (X1 - X0, Y1 - Y0), (0, 0, 0, 0))
        tile.paste(icon, (x0 - X0, y0 - Y0))
        blocks = encode(tile)
        entries.append((name, X0, Y0, X1 - X0, Y1 - Y0, blocks))
        bw = (X1 - X0) // 4
        for row in range((Y1 - Y0) // 4):
            start = ((Y0 // 4 + row) * (ATLAS // 4) + X0 // 4) * 16
            atlas_blocks[start:start + bw * 16] = blocks[row * bw * 16:(row + 1) * bw * 16]
        preview_parts.append(tile)

    patched = decode_dds(bytes(atlas_blocks), ATLAS, ATLAS)
    bg = Image.new("RGBA", patched.size, (60, 60, 60, 255))
    bg.alpha_composite(patched)
    preview = os.path.join(os.path.dirname(sys.argv[1]), "atlas-ps.png")
    bg.convert("RGB").save(preview)

    with open(OUT, "w", newline="\n") as f:
        f.write("/* GENERATED by ps5/scripts/gen-ps-glyphs.py - do not edit.\n"
                " * PlayStation button glyphs (drawn by that script, BC7 blocks) patched over\n"
                " * the Switch glyphs of the game's 1024x1024 BC7 UI icon atlas. */\n"
                "#ifndef POKOPIA_PS_BUTTON_GLYPHS_H\n#define POKOPIA_PS_BUTTON_GLYPHS_H\n\n#include <stdint.h>\n\n")
        f.write(f"/* FNV-1a over the atlas's level-0 BC7 blocks as 64-bit words. */\n"
                f"#define PS_GLYPH_ATLAS_HASH UINT64_C({atlas_hash:#018x})\n#define PS_GLYPH_ATLAS_SIZE {ATLAS}\n\n")
        for name, x, y, w, h, blocks in entries:
            f.write(f"static const uint8_t ps_glyph_{name}[{len(blocks)}] = {{\n")
            for i in range(0, len(blocks), 16):
                f.write("    " + ", ".join(f"0x{b:02x}" for b in blocks[i:i + 16]) + ",\n")
            f.write("};\n")
        f.write("\nstatic const struct {\n    uint16_t x, y, width, height; /* pixels, multiples of 4 */\n"
                "    const uint8_t *blocks;\n} ps_glyph_patches[] = {\n")
        for name, x, y, w, h, _ in entries:
            f.write(f"    {{{x}, {y}, {w}, {h}, ps_glyph_{name}}},\n")
        f.write("};\n\n#endif\n")
    print(f"hash {atlas_hash:#018x}; wrote {OUT} and {preview}")


if __name__ == "__main__":
    main()
