/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_DEBUG_OVERLAY_PIXELS_H
#define POKOPIA_DEBUG_OVERLAY_PIXELS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__PROSPERO__)
enum { POKOPIA_OVERLAY_LINES = 6 };
#else
enum { POKOPIA_OVERLAY_LINES = 4 };
#endif
enum { POKOPIA_OVERLAY_WIDTH = 420, POKOPIA_OVERLAY_HEIGHT = 8 + POKOPIA_OVERLAY_LINES * 20 };
/* R75: the one-line frame-rate panel in the top-right corner. */
enum { POKOPIA_FPS_WIDTH = 128, POKOPIA_FPS_HEIGHT = 30 };

static inline void pokopia_overlay_panel(uint8_t *rgba, unsigned width, unsigned height,
                                         const char *const *lines, unsigned line_count)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -/:.%+?";
    /* Five bits per row, with the left pixel in bit four. */
    static const uint8_t font[][7] = {
        {14, 17, 17, 31, 17, 17, 17}, /* A */
        {30, 17, 17, 30, 17, 17, 30}, /* B */
        {14, 17, 16, 16, 16, 17, 14}, /* C */
        {30, 17, 17, 17, 17, 17, 30}, /* D */
        {31, 16, 16, 30, 16, 16, 31}, /* E */
        {31, 16, 16, 30, 16, 16, 16}, /* F */
        {14, 17, 16, 23, 17, 17, 15}, /* G */
        {17, 17, 17, 31, 17, 17, 17}, /* H */
        {14, 4, 4, 4, 4, 4, 14}, /* I */
        {7, 2, 2, 2, 2, 18, 12}, /* J */
        {17, 18, 20, 24, 20, 18, 17}, /* K */
        {16, 16, 16, 16, 16, 16, 31}, /* L */
        {17, 27, 21, 21, 17, 17, 17}, /* M */
        {17, 25, 21, 19, 17, 17, 17}, /* N */
        {14, 17, 17, 17, 17, 17, 14}, /* O */
        {30, 17, 17, 30, 16, 16, 16}, /* P */
        {14, 17, 17, 17, 21, 18, 13}, /* Q */
        {30, 17, 17, 30, 20, 18, 17}, /* R */
        {15, 16, 16, 14, 1, 1, 30}, /* S */
        {31, 4, 4, 4, 4, 4, 4}, /* T */
        {17, 17, 17, 17, 17, 17, 14}, /* U */
        {17, 17, 17, 17, 17, 10, 4}, /* V */
        {17, 17, 17, 21, 21, 21, 10}, /* W */
        {17, 17, 10, 4, 10, 17, 17}, /* X */
        {17, 17, 10, 4, 4, 4, 4}, /* Y */
        {31, 1, 2, 4, 8, 16, 31}, /* Z */
        {14, 17, 19, 21, 25, 17, 14}, /* 0 */
        {4, 12, 4, 4, 4, 4, 14}, /* 1 */
        {14, 17, 1, 2, 4, 8, 31}, /* 2 */
        {30, 1, 1, 14, 1, 1, 30}, /* 3 */
        {2, 6, 10, 18, 31, 2, 2}, /* 4 */
        {31, 16, 16, 30, 1, 1, 30}, /* 5 */
        {6, 8, 16, 30, 17, 17, 14}, /* 6 */
        {31, 1, 2, 4, 8, 8, 8}, /* 7 */
        {14, 17, 17, 14, 17, 17, 14}, /* 8 */
        {14, 17, 17, 15, 1, 2, 12}, /* 9 */
        {0, 0, 0, 0, 0, 0, 0}, /* space */
        {0, 0, 0, 31, 0, 0, 0}, /* - */
        {1, 1, 2, 4, 8, 16, 16}, /* / */
        {0, 4, 4, 0, 4, 4, 0}, /* : */
        {0, 0, 0, 0, 0, 4, 4}, /* . */
        {17, 2, 4, 4, 8, 16, 17}, /* % */
        {0, 4, 4, 31, 4, 4, 0}, /* + */
        {14, 17, 1, 2, 4, 0, 4}, /* ? */
    };
    enum {
        SCALE = 2, GLYPH_WIDTH = 5, GLYPH_HEIGHT = 7,
        LEFT = 8, TOP = 8, COLUMN_STEP = 12, LINE_STEP = 20,
    };
    const unsigned columns = (width - LEFT - 1 + SCALE) / COLUMN_STEP;

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint8_t *pixel = rgba + (y * width + x) * 4;
            const bool border = !x || !y || x == width - 1 || y == height - 1;
            pixel[0] = pixel[1] = pixel[2] = border ? 88 : 16;
            pixel[3] = 255;
        }
    }

    for (unsigned line = 0; line < line_count; ++line) {
        const char *text = lines[line];
        if (!text)
            continue;
        for (unsigned column = 0; column < columns && text[column]; ++column) {
            int character = (unsigned char)text[column];
            if (character >= 'a' && character <= 'z')
                character -= 'a' - 'A';
            const char *match = strchr(alphabet, character);
            const size_t glyph = match ? (size_t)(match - alphabet) : sizeof alphabet - 2;
            for (unsigned row = 0; row < GLYPH_HEIGHT; ++row) {
                for (unsigned bit = 0; bit < GLYPH_WIDTH; ++bit) {
                    if (!(font[glyph][row] & (1u << (GLYPH_WIDTH - 1 - bit))))
                        continue;
                    const unsigned x = LEFT + column * COLUMN_STEP + bit * SCALE;
                    const unsigned y = TOP + line * LINE_STEP + row * SCALE;
                    for (unsigned dy = 0; dy < SCALE; ++dy) {
                        for (unsigned dx = 0; dx < SCALE; ++dx) {
                            uint8_t *pixel = rgba + ((y + dy) * width + x + dx) * 4;
                            pixel[0] = pixel[1] = pixel[2] = 224;
                        }
                    }
                }
            }
        }
    }
}

static inline void pokopia_overlay_pixels(uint8_t *rgba, const char *const lines[POKOPIA_OVERLAY_LINES])
{
    pokopia_overlay_panel(rgba, POKOPIA_OVERLAY_WIDTH, POKOPIA_OVERLAY_HEIGHT, lines, POKOPIA_OVERLAY_LINES);
}

#endif
