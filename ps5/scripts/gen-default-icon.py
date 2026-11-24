#!/usr/bin/env python3
"""Draw the builder's default sce_sys/icon0.png (512x512): a plain gradient tile with the port's name.

Nothing of the game is used; the user can pick their own icon in the builder instead.

    python ps5/scripts/gen-default-icon.py
"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "builder", "default-icon0.png")
FONT = os.environ.get("ICON_FONT", r"C:\Windows\Fonts\ARLRDBD.TTF")
SIZE, SS = 512, 4


def main():
    big = SIZE * SS
    top, bottom = (126, 200, 80), (40, 120, 200)
    tile = Image.new("RGB", (big, big))
    draw = ImageDraw.Draw(tile)
    for y in range(big):
        t = y / (big - 1)
        draw.line([(0, y), (big, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    text = Image.new("L", (big, big), 0)
    tdraw = ImageDraw.Draw(text)
    for line, size, y in (("Pokopia", 118, 0.40), ("PS5", 150, 0.64)):
        font = ImageFont.truetype(FONT, size * SS)
        box = tdraw.textbbox((0, 0), line, font=font, anchor="mm")
        tdraw.text((big // 2, int(big * y)), line, font=font, fill=255, anchor="mm")
        del box
    shadow = text.filter(ImageFilter.GaussianBlur(10 * SS)).point(lambda v: v * 0.5)
    tile.paste((20, 40, 60), (0, 3 * SS), shadow)
    tile.paste((255, 255, 255), (0, 0), text)
    tile.resize((SIZE, SIZE), Image.LANCZOS).save(OUT, optimize=True)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
