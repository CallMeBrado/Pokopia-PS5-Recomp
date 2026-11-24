/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debug_overlay_pixels.h"

#include <assert.h>
#include <stdio.h>

enum { PIXEL_BYTES = POKOPIA_OVERLAY_WIDTH * POKOPIA_OVERLAY_HEIGHT * 4 };

static unsigned tone(const uint8_t *rgba, unsigned x, unsigned y)
{
    const uint8_t *pixel = rgba + (y * POKOPIA_OVERLAY_WIDTH + x) * 4;
    assert(pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[3] == 255);
    return pixel[0];
}

int main(void)
{
    static uint8_t guarded[PIXEL_BYTES + 2];
    uint8_t *pixels = guarded + 1;
    guarded[0] = guarded[PIXEL_BYTES + 1] = 0x7a;
    const char *lines[POKOPIA_OVERLAY_LINES] = {
        "a@?",
        "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM",
        NULL,
        [POKOPIA_OVERLAY_LINES - 1] = "+",
    };
    pokopia_overlay_pixels(pixels, lines);
    assert(guarded[0] == 0x7a && guarded[PIXEL_BYTES + 1] == 0x7a);
    assert(tone(pixels, 0, 0) == 88 && tone(pixels, POKOPIA_OVERLAY_WIDTH - 1, POKOPIA_OVERLAY_HEIGHT - 1) == 88);
    assert(tone(pixels, 8, 8) == 16 && tone(pixels, 10, 8) == 224 && tone(pixels, 11, 9) == 224);
    for (unsigned y = 8; y < 22; ++y)
        for (unsigned x = 20; x < 30; ++x)
            assert(tone(pixels, x, y) == tone(pixels, x + 12, y));
    assert(tone(pixels, 413, 28) == 224 && tone(pixels, 414, 28) == 16 && tone(pixels, 419, 28) == 88);
    assert(tone(pixels, 8, 48) == 16 && tone(pixels, 8, POKOPIA_OVERLAY_HEIGHT - 14) == 224);
    for (unsigned y = 0; y < POKOPIA_OVERLAY_HEIGHT; ++y)
        for (unsigned x = 0; x < POKOPIA_OVERLAY_WIDTH; ++x)
            assert(tone(pixels, x, y) != 0);
    puts("PASS debug overlay opacity, grayscale, glyph scale, fallback and clipping");
    return 0;
}
