#!/usr/bin/env python3
"""Generate the title screen's "PlayStation Edition" banner.

Draws, at the 3840x2160 scanout scale, a white PlayStation logo (an upright P
standing on an S laid flat in perspective) followed by "PlayStation Edition"
in Arial Rounded MT Bold, both white over a soft dark shadow like the title's
"PRESS BUTTON" line. Writes gfx-bridge/ps_banner.h: the white coverage and the
shadow coverage, one byte each per pixel, which the present path blends under
the game's logo while the title screen is up (live_present.c).

Nothing is copied from the game; the font is only rasterised here.

    python ps5/scripts/gen-ps-banner.py [--preview banner.png] [--over title.png]
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "gfx-bridge", "ps_banner.h")
FONT = os.environ.get("BANNER_FONT", r"C:\Windows\Fonts\ARLRDBD.TTF")
LOGO_FONT = os.environ.get("BANNER_LOGO_FONT", r"C:\Windows\Fonts\arialbd.ttf")
SS = 4                      # supersampling
HEIGHT = 170                # banner height in scanout pixels
TEXT_SIZE = 96
SHADOW_BLUR, SHADOW_ALPHA, SHADOW_OFFSET = 7, 0.55, (0, 4)


def glyph_mask(char, font, height):
    """A tight mask of one glyph scaled to `height` pixels (at SS scale)."""
    big = ImageFont.truetype(font, height * 2)
    box = big.getbbox(char)
    img = Image.new("L", (box[2] - box[0] + 8, box[3] - box[1] + 8), 0)
    ImageDraw.Draw(img).text((4 - box[0], 4 - box[1]), char, fill=255, font=big)
    img = img.crop(img.getbbox())
    scale = height / img.height
    return img.resize((max(1, round(img.width * scale)), height), Image.LANCZOS)


def perspective(mask, top_scale, height):
    """Lay a glyph down: squash it to `height` and narrow its far (top) edge."""
    w, h = mask.size
    out_w = w
    # quad corners in the source for the output's corners (PIL QUAD: ul, ll, lr, ur)
    inset = w * (1 - top_scale) / 2
    squashed = mask.resize((w, height), Image.LANCZOS)
    return squashed.transform((out_w, height), Image.QUAD,
                              (-inset, 0, 0, height, w, height, w + inset, 0), Image.BICUBIC)


def ps_logo(height):
    """The PlayStation logo mark, white on transparent, `height` px tall (SS scale)."""
    p = glyph_mask("P", LOGO_FONT, int(height * 0.9))
    p = p.resize((int(p.width * 0.82), p.height), Image.LANCZOS)          # the logo's P is tall and narrow
    s = glyph_mask("S", LOGO_FONT, int(height * 1.15))
    s = s.resize((int(s.width * 1.9), s.height), Image.LANCZOS)          # wide, as if seen lying down
    s = perspective(s, 0.62, int(height * 0.4))                          # far edge narrower
    width = int(p.width * 0.1 + s.width) + 8
    canvas = Image.new("L", (width + 8, height), 0)
    # S lies at the bottom, from under the P's stem out to the right.
    sx, sy = int(p.width * 0.08), height - s.height
    canvas.paste(s, (sx, sy), s)
    # A dark gap around the P where it stands in front of the S.
    gap = p.filter(ImageFilter.MaxFilter(9))
    cut = Image.new("L", canvas.size, 0)
    cut.paste(gap, (0, 0), gap)
    canvas = Image.fromarray(np.minimum(np.asarray(canvas), 255 - np.asarray(cut)).astype(np.uint8))
    canvas.paste(p, (0, 0), p)
    return canvas


def logo_from_image(path, height):
    """A supplied logo (dark mark on a light background): its darkness becomes coverage."""
    gray = np.asarray(Image.open(path).convert("L"), dtype=np.float32)
    cover = np.clip((235.0 - gray) / (235.0 - 20.0), 0, 1)
    mask = Image.fromarray((cover * 255).astype(np.uint8))
    mask = mask.crop(mask.getbbox())
    return mask.resize((max(1, round(mask.width * height / mask.height)), height), Image.LANCZOS)


def banner(logo_path=None):
    h = HEIGHT * SS
    logo = logo_from_image(logo_path, int(h * 0.92)) if logo_path else ps_logo(int(h * 0.92))
    font = ImageFont.truetype(FONT, TEXT_SIZE * SS)
    text = "PlayStation Edition"
    tb = font.getbbox(text)
    gap = int(16 * SS)
    width = logo.width + gap + (tb[2] - tb[0]) + 16 * SS
    white = Image.new("L", (width, h), 0)
    black = Image.new("L", (width, h), 0)
    (black if logo_path else white).paste(logo, (8 * SS, (h - logo.height) // 2), logo)
    ImageDraw.Draw(white).text((8 * SS + logo.width + gap - tb[0], (h - (tb[3] - tb[1])) // 2 - tb[1]),
                               text, fill=255, font=font)
    pad = SHADOW_BLUR * 3
    layers = []
    for layer in (white, black):
        layer = layer.resize((width // SS, HEIGHT), Image.LANCZOS)
        padded = Image.new("L", (layer.width + 2 * pad, layer.height + 2 * pad), 0)
        padded.paste(layer, (pad, pad))
        layers.append(padded)
    # The text's dark shadow; the black logo gets none.
    shadow = layers[0].filter(ImageFilter.GaussianBlur(SHADOW_BLUR))
    shadow = Image.fromarray((np.asarray(shadow, dtype=np.float32) * SHADOW_ALPHA).astype(np.uint8))
    shadow = shadow.transform(shadow.size, Image.AFFINE, (1, 0, -SHADOW_OFFSET[0], 0, 1, -SHADOW_OFFSET[1]))
    return layers[0], layers[1], shadow


def composite(white, black, shadow, background):
    w = np.asarray(white, dtype=np.float32)[..., None] / 255
    b = np.asarray(black, dtype=np.float32)[..., None] / 255
    s = np.asarray(shadow, dtype=np.float32)[..., None] / 255
    out = np.asarray(background, dtype=np.float32)
    out = out * (1 - s) + np.array([13, 11, 9], np.float32) * s
    out = out * (1 - b)
    out = out * (1 - w) + 255 * w
    return Image.fromarray(out.clip(0, 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logo", help="the PlayStation logo image to use (dark on light); default: drawn")
    ap.add_argument("--preview")
    ap.add_argument("--over", help="a 3840x2160 title screenshot to preview the banner on")
    ap.add_argument("--at", default="2510,1285", help="banner top-left on the scanout")
    args = ap.parse_args()
    white, black, shadow = banner(args.logo)
    w, h = white.size
    if args.preview:
        composite(white, black, shadow, Image.new("RGB", white.size, (96, 110, 100))).save(args.preview)
    if args.over:
        x, y = (int(v) for v in args.at.split(","))
        shot = Image.open(args.over).convert("RGB")
        region = shot.crop((x, y, x + w, y + h))
        shot.paste(composite(white, black, shadow, region), (x, y))
        shot.save(os.path.splitext(args.over)[0] + "-banner.png")
    layers = (("ps_banner_white", np.asarray(white)), ("ps_banner_black", np.asarray(black)),
              ("ps_banner_shadow", np.asarray(shadow)))
    with open(OUT, "w", newline="\n") as out:
        out.write("/* GENERATED by ps5/scripts/gen-ps-banner.py - do not edit.\n"
                  " * The title screen's \"PlayStation Edition\" banner at scanout scale: white\n"
                  " * coverage, then shadow coverage, one byte per pixel each. */\n"
                  "#ifndef POKOPIA_PS_BANNER_H\n#define POKOPIA_PS_BANNER_H\n\n#include <stdint.h>\n\n")
        out.write(f"#define PS_BANNER_WIDTH {w}\n#define PS_BANNER_HEIGHT {h}\n\n")
        for name, data in layers:
            flat = data.reshape(-1)
            out.write(f"static const uint8_t {name}[{flat.size}] = {{\n")
            for i in range(0, flat.size, 32):
                out.write("    " + ",".join(str(int(v)) for v in flat[i:i + 32]) + ",\n")
            out.write("};\n\n")
        out.write("#endif\n")
    print(f"banner {w}x{h} -> {OUT}")


if __name__ == "__main__":
    main()
