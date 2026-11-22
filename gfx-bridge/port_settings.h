/* SPDX-License-Identifier: GPL-3.0-or-later */
/* R92: the port's own settings (not the game's), changed in the port menu
 * (hold the touch pad) and kept in /data/pokopia/port-settings.ini. */
#ifndef POKOPIA_PORT_SETTINGS_H
#define POKOPIA_PORT_SETTINGS_H

#include <stdatomic.h>
#include <stdbool.h>

enum { POKOPIA_PROMPTS_PLAYSTATION, POKOPIA_PROMPTS_NINTENDO };
enum { POKOPIA_CONFIRM_CIRCLE, POKOPIA_CONFIRM_CROSS };
enum { POKOPIA_UPSCALER_FSR, POKOPIA_UPSCALER_BILINEAR };
enum { POKOPIA_SHARPNESS_LEVELS = 5 };

typedef struct {
    bool fps_counter;   /* FPS counter, top right */
    bool debug_info;    /* the developer box, top left */
    unsigned prompts;   /* button glyphs: PlayStation or the game's Nintendo ones */
    unsigned confirm;   /* the PlayStation button that acts as A: Circle (right) or Cross (bottom) */
    unsigned frame_cap; /* 60 or 30 */
    unsigned upscaler;  /* FSR 1 or bilinear */
    unsigned sharpness; /* FSR sharpening, 0 (soft) to 4 (sharpest) */
} PokopiaSettings;

extern PokopiaSettings pokopia_settings;
/* Advanced whenever the prompts or confirm setting changes: the textures
 * carrying button glyphs are uploaded again with the matching patch. */
extern atomic_uint pokopia_glyph_generation;

void pokopia_settings_defaults(PokopiaSettings *settings);
/* Missing or unreadable file: defaults. Unknown keys and bad values are ignored. */
void pokopia_settings_load(PokopiaSettings *settings, const char *path);
bool pokopia_settings_save(const PokopiaSettings *settings, const char *path);
/* FSR RCAS sharpness in stops below the maximum for a level. */
float pokopia_settings_rcas_stops(unsigned level);

#endif
