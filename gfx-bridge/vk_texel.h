/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_TEXEL_H
#define POKOPIA_VK_TEXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkBuffer buffer;
    VkBufferView view;
    VkFormat format;
    size_t bytes;

    VkDevice device;
    PFN_vkGetDeviceProcAddr get_proc;
    uint32_t visible_types, coherent_types;
    VkDeviceMemory memory;
    void *mapped;
    bool coherent;
    uint64_t refresh_epoch; /* R73: general draws refresh a texel buffer once per submit */
    uint64_t batch_stamp;   /* R75: pokopia_vk_batch_id of the last draw batch that read it */
} PokopiaVkTexel;

/* Validate a zeroed object and defer Vulkan allocation until its first refresh. */
VkResult pokopia_vk_texel_configure(PokopiaVkTexel *texel, VkDevice device,
                                   PFN_vkGetDeviceProcAddr get_proc,
                                   const VkPhysicalDeviceMemoryProperties *memory,
                                   VkFormatFeatureFlags buffer_features, VkFormat format,
                                   uint32_t texel_count, uint32_t texel_bytes, uint32_t max_texels);

/* Start with a zeroed object. The caller supplies the directly mapped Vulkan
 * format and its texel size. Allocate the backing and view immediately. */
VkResult pokopia_vk_texel_open(PokopiaVkTexel *texel, VkDevice device,
                              PFN_vkGetDeviceProcAddr get_proc,
                              const VkPhysicalDeviceMemoryProperties *memory,
                              VkFormatFeatureFlags buffer_features, VkFormat format,
                              uint32_t texel_count, uint32_t texel_bytes, uint32_t max_texels);

/* Refresh the full view from canonical guest bytes before submitting work
 * which reads it. Retire earlier GPU references before refreshing or closing.
 * The caller serializes refresh and close with recording and submission. */
VkResult pokopia_vk_texel_refresh(PokopiaVkTexel *texel, const void *guest_bytes, size_t byte_count);
void pokopia_vk_texel_close(PokopiaVkTexel *texel);

#ifdef __cplusplus
}
#endif
#endif
