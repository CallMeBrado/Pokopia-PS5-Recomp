/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_TEXTURE_UPLOAD_H
#define POKOPIA_TEXTURE_UPLOAD_H

#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvnTextureUpload NvnTextureUpload;

typedef struct {
    VkExtent2D block_extent;
    size_t bytes_per_block;
    size_t row_bytes;
    size_t rows;
    size_t source_row_pitch;
    size_t required_source_bytes;
    size_t packed_bytes;
} NvnTextureFootprint;

typedef struct {
    VkImageLayout layout;
    VkPipelineStageFlags stages;
    VkAccessFlags access;
} NvnTextureImageState;

typedef struct {
    VkImage image;
    VkFormat format;
    VkExtent2D mip_extent;
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t layer_count; /* Zero selects one layer. */
    VkOffset2D offset;
    VkExtent2D extent;
    NvnTextureImageState before;
    NvnTextureImageState after;
    uint32_t depth; /* R73: z slices of a 3D image (layer_count must then be 1); zero selects one */
} NvnTextureUploadRegion;

/* Describe linear data for supported color formats. A zero source pitch
 * means tightly packed. Nonzero pitch is in bytes and may include padding
 * between rows or block rows. Trailing padding is optional. */
VkResult nvn_texture_upload_footprint(VkFormat format, VkExtent2D extent, size_t source_row_pitch,
                                      NvnTextureFootprint *footprint);

/* Initialize *out to NULL. The upload object owns one mapped staging buffer
 * of capacity bytes, command buffer and fence. Device, graphics queue, and
 * uploaded image are borrowed. The caller serializes host access to the queue. */
VkResult nvn_texture_upload_open(NvnTextureUpload **out, VkDevice device, VkQueue queue,
                                 uint32_t queue_family, const VkPhysicalDeviceMemoryProperties *memory,
                                 PFN_vkGetDeviceProcAddr get_proc, size_t capacity);

/* Upload the same rectangle from one or more consecutive layers of one mip.
 * Source layers follow each other at the end of the previous layer's last
 * row pitch. The caller provides the actual mip extent/format and Vulkan
 * states. The 2D color image must be single-sample, support TRANSFER_DST,
 * and belong to the queue family. UNDEFINED discards each selected mip/layer.
 * Source bytes are copied before return. Keep the image alive through wait. */
VkResult nvn_texture_upload_submit(NvnTextureUpload *upload, const NvnTextureUploadRegion *region,
                                   const void *source, size_t source_size, size_t source_row_pitch);

/* A timeout leaves the copy in flight. Submit and close return VK_NOT_READY
 * while a submitted copy has yet to be retired by wait. */
VkResult nvn_texture_upload_wait(NvnTextureUpload *upload, uint64_t timeout_ns);
VkResult nvn_texture_upload_close(NvnTextureUpload **upload);

#ifdef __cplusplus
}
#endif
#endif
