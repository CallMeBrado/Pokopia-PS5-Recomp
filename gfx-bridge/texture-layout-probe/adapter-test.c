/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_guest_layout.h"

#include <stdio.h>

int main(void)
{
    NvnFrontTextureConfig config = {
        .target = 1, .width = 1920, .height = 1080, .depth = 1, .levels = 1,
        .format = 0x38, .flags = 0xd,
    };
    uint64_t size = 0, alignment = 0;
    if (!nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0x870000 || alignment != 0x2000)
        return 1;

    config.flags = 0x10;
    config.stride = 8192;
    if (!nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 8192 * 1080 || alignment != 4096)
        return 2;

    config.flags = 0;
    config.stride = 0;
    config.width = 36;
    config.height = 36;
    config.format = 0x4d;
    config.packaged_layout = 1;
    if (!nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0xc00 || alignment != 0x200)
        return 3;

    config.packaged_layout = 0x1001;
    if (nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0xc00 || alignment != 0x200)
        return 4;

    config.width = UINT32_MAX;
    config.packaged_layout = 0;
    if (nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0xc00 || alignment != 0x200)
        return 5;

    config.width = 36;
    config.flags = 0x20;
    if (nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0xc00 || alignment != 0x200)
        return 6;

    config.flags = 0;
    config.gl_texture_name = 5;
    if (nvn_guest_texture_layout(&config, &size, &alignment) ||
        size != 0xc00 || alignment != 0x200)
        return 7;

    puts("NVN guest layout C ABI passed");
    return 0;
}
