/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_GUEST_LAYOUT_H
#define POKOPIA_NVN_GUEST_LAYOUT_H

#include "nvn_front.h"
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NVN_GUEST_LINEAR_STRIDE_ALIGNMENT = 32 };

/* Computes guest NVN2 storage for non-sparse textures owned by NVN. Both
 * outputs are unchanged on failure. */
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config,
    uint64_t *storage_size, uint64_t *alignment);

/* Returns the native Vulkan encoding or VK_FORMAT_UNDEFINED when a direct
 * mapping is unavailable. Check device support, image usage and transfer
 * aspects separately. */
VkFormat nvn_guest_texture_vk_format(uint32_t format);
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format);

#ifdef __cplusplus
}
#endif
#endif
