/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "port_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PokopiaSettings pokopia_settings = {
    .fps_counter = true, .debug_info = false, .prompts = POKOPIA_PROMPTS_PLAYSTATION,
    .confirm = POKOPIA_CONFIRM_CIRCLE, .frame_cap = 60, .upscaler = POKOPIA_UPSCALER_FSR, .sharpness = 3,
};
atomic_uint pokopia_glyph_generation;

void pokopia_settings_defaults(PokopiaSettings *settings)
{
    *settings = (PokopiaSettings){
        .fps_counter = true, .debug_info = false, .prompts = POKOPIA_PROMPTS_PLAYSTATION,
        .confirm = POKOPIA_CONFIRM_CIRCLE, .frame_cap = 60, .upscaler = POKOPIA_UPSCALER_FSR, .sharpness = 3,
    };
}

void pokopia_settings_load(PokopiaSettings *settings, const char *path)
{
    pokopia_settings_defaults(settings);
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    char line[128];
    while (fgets(line, sizeof line, file)) {
        char key[48];
        unsigned value;
        if (sscanf(line, " %47[a-z_] = %u", key, &value) != 2)
            continue;
        if (!strcmp(key, "fps_counter"))
            settings->fps_counter = value != 0;
        else if (!strcmp(key, "debug_info"))
            settings->debug_info = value != 0;
        else if (!strcmp(key, "prompts") && value <= POKOPIA_PROMPTS_NINTENDO)
            settings->prompts = value;
        else if (!strcmp(key, "confirm") && value <= POKOPIA_CONFIRM_CROSS)
            settings->confirm = value;
        else if (!strcmp(key, "frame_cap") && (value == 0 || value == 30 || value == 60))
            settings->frame_cap = value;
        else if (!strcmp(key, "upscaler") && value <= POKOPIA_UPSCALER_BILINEAR)
            settings->upscaler = value;
        else if (!strcmp(key, "sharpness") && value < POKOPIA_SHARPNESS_LEVELS)
            settings->sharpness = value;
    }
    fclose(file);
}

bool pokopia_settings_save(const PokopiaSettings *settings, const char *path)
{
    char temp[256];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *file = fopen(temp, "w");
    if (!file)
        return false;
    fprintf(file,
            "# Pokopia PS5 port settings (hold the touch pad in game to change them)\n"
            "fps_counter = %u\ndebug_info = %u\n"
            "# prompts: 0 PlayStation, 1 Nintendo; confirm: 0 Circle, 1 Cross\n"
            "prompts = %u\nconfirm = %u\n"
            "# frame_cap: 60, 30 or 0 (uncapped); upscaler: 0 FSR 1, 1 bilinear; sharpness: 0-4\n"
            "frame_cap = %u\nupscaler = %u\nsharpness = %u\n",
            settings->fps_counter, settings->debug_info, settings->prompts, settings->confirm, settings->frame_cap,
            settings->upscaler, settings->sharpness);
    const bool ok = fclose(file) == 0;
    return ok && rename(temp, path) == 0;
}

float pokopia_settings_rcas_stops(unsigned level)
{
    static const float stops[POKOPIA_SHARPNESS_LEVELS] = {2.0f, 1.0f, 0.5f, 0.25f, 0.0f};
    return stops[level < POKOPIA_SHARPNESS_LEVELS ? level : 3];
}
