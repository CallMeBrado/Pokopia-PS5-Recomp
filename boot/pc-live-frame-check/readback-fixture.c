/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc-frame-check.h"

int main(void)
{
    PokopiaVkAdapter adapter = {0};
    if (!pokopia_vk_open(&adapter, stdout))
        return 1;
    const NvnFrontTextureConfig config = {
        .target = 1, .width = 8, .height = 8, .depth = 1, .levels = 1,
        .format = 37, .swizzle = {2, 3, 4, 5},
    };
    PokopiaVkTexture texture = {0};
    VkResult result = pokopia_vk_texture_open(&adapter, &config, &texture);
    if (result == VK_SUCCESS)
        result = pokopia_vk_clear_color(&adapter, &texture, (float[]){0, 0, 0, 1});
    if (result == VK_SUCCESS) {
        pokopia_pc_frame_check(&adapter, &texture, stdout);
        result = pokopia_vk_clear_color(&adapter, &texture, (float[]){0, 0, 1, 1});
    }
    if (result == VK_SUCCESS) {
        pokopia_pc_frame_check(&adapter, &texture, stdout);
        result = pokopia_vk_clear_color_region(&adapter, &texture, (VkRect2D){{2, 3}, {3, 2}},
                                              (float[]){1, 0, 0, 1});
    }
    if (result == VK_SUCCESS)
        pokopia_pc_frame_check(&adapter, &texture, stdout);
    pokopia_vk_texture_close(&adapter, &texture);
    pokopia_vk_close(&adapter);
    return result != VK_SUCCESS;
}
