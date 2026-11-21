/* SPDX-License-Identifier: GPL-3.0-or-later */
/* R86: on-screen keyboard in the style of the Switch's software keyboard,
 * drawn by the port and driven by the pad. It stands in for the swkbd library
 * applet (horizon/applet.c) when the game asks for text. */
#ifndef POKOPIA_OSK_H
#define POKOPIA_OSK_H

#include <stdbool.h>
#include <stdint.h>

enum { POKOPIA_OSK_TEXT = 128, POKOPIA_OSK_TITLE = 128 };

typedef struct {
    char text[POKOPIA_OSK_TEXT + 1]; /* ASCII */
    unsigned length, max_length, min_length;
    char title[POKOPIA_OSK_TITLE + 1];
    unsigned row, key;
    bool shift;
    /* auto-repeat of the held direction */
    int held_dx, held_dy;
    uint64_t next_repeat_ms;
} PokopiaOsk;

/* Pad buttons as Switch bits (horizon/hid_ps4.c): A=Circle, B=Cross,
 * X=Triangle, Y=Square, Plus=OPTIONS, Minus=touch pad, d-pad bits 12-15. */
enum {
    POKOPIA_OSK_A = 1u << 0, POKOPIA_OSK_B = 1u << 1, POKOPIA_OSK_X = 1u << 2, POKOPIA_OSK_Y = 1u << 3,
    POKOPIA_OSK_PLUS = 1u << 10, POKOPIA_OSK_MINUS = 1u << 11,
    POKOPIA_OSK_LEFT = 1u << 12, POKOPIA_OSK_UP = 1u << 13, POKOPIA_OSK_RIGHT = 1u << 14, POKOPIA_OSK_DOWN = 1u << 15,
};

typedef enum { POKOPIA_OSK_OPEN, POKOPIA_OSK_DONE, POKOPIA_OSK_CANCELLED } PokopiaOskState;

void pokopia_osk_init(PokopiaOsk *osk, const char *title, unsigned max_length, unsigned min_length);
/* One pad sample (buttons held now, left stick, -32767..32767 with up positive).
 * Sets *changed when the picture must be redrawn. */
PokopiaOskState pokopia_osk_input(PokopiaOsk *osk, uint64_t buttons, uint64_t previous, int lx, int ly,
                                  uint64_t now_ms, bool *changed);

/* Draws the title band (width x top rows) followed by the keyboard panel
 * (width x bottom rows) into one buffer of 4-byte pixels, B,G,R,A when bgra. */
void pokopia_osk_draw(const PokopiaOsk *osk, uint8_t *pixels, unsigned width, unsigned top, unsigned bottom,
                      bool bgra, uint64_t now_ms);

#endif
