/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_ADAPTER_H
#define POKOPIA_VK_ADAPTER_H

#include "nvn_front.h"
#include "vk_texel.h"
#include <stdbool.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#ifndef _WIN32
#include "../gpu-present/present.h"
#endif

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize bytes;
    struct PokopiaVkImageSpan *span;
} PokopiaVkImage;

typedef struct PokopiaVkTexture {
    PokopiaVkImage backing;
    PokopiaVkTexel texel;
    /* Combined depth/stencil and layered 2D images have separate sampled views. */
    VkImageView view, sample_view, sample_alias_view, storage_view;
    VkFormat format, sample_alias_format;
    VkExtent2D extent;
    uint32_t array_layers;
    VkImageAspectFlags aspects;
    VkImageLayout layout;
    VkPipelineStageFlags stages;
    VkAccessFlags access;
    uint64_t render_serial;
    bool deferred;
    /* R66: sampled view type, and views with a component swizzle made on demand. */
    VkImageViewType sample_view_type;
    struct { uint64_t key; VkImageView view; } swizzle_views[8];
    /* R66: copy sampled by draws that also render to this texture. */
    struct PokopiaVkTexture *feedback_copy;
    /* R66: last guest bytes copied in for CPU-written linear textures. */
    void *guest_shadow;
    uint64_t linear_epoch; /* R78: replay job of the last linear sync */
} PokopiaVkTexture;

typedef struct {
    void *loader;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceFeatures features;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format_properties;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetDeviceProcAddr get_device_proc;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDeviceWaitIdle wait_idle;
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    VkCommandPool guest_commands;
    struct PokopiaVkImageBlock *image_blocks;
    PokopiaVkTexture feedback_snapshot;
    FILE *log;
    uint64_t render_serial;
#ifdef _WIN32
    PokopiaVkImage output;
#else
    VkSurfaceKHR surface;
    PokopiaPresent present;
#endif
} PokopiaVkAdapter;

bool pokopia_vk_open(PokopiaVkAdapter *adapter, FILE *log);
bool pokopia_vk_output(const PokopiaVkAdapter *adapter, VkExtent2D *extent);
/* Owns the Vulkan image independently of the NVN guest pool. Serialize
 * texture opens and closes per adapter, and retire its submissions before close. */
VkResult pokopia_vk_texture_open(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                 PokopiaVkTexture *texture);
/* Validates sampled-only BC images and lets the first sample allocate their backing. */
VkResult pokopia_vk_texture_open_lazy(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                      PokopiaVkTexture *texture);
void pokopia_vk_texture_close(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture);
/* Guest command buffers can outlive individual NVN queue objects. Serialize
 * allocation and release, and retire any submission before release. */
VkResult pokopia_vk_command_buffer_open(PokopiaVkAdapter *adapter, VkCommandBuffer *command);
void pokopia_vk_command_buffer_close(PokopiaVkAdapter *adapter, VkCommandBuffer command);
/* Commits a guest buffer transfer only after its Vulkan copy completes. */
VkResult pokopia_vk_copy_guest(PokopiaVkAdapter *adapter, const void *source, void *destination, size_t bytes);
VkResult pokopia_vk_submit_wait_at(PokopiaVkAdapter *adapter, VkCommandBuffer command, bool *retired,
                                   const char *site);
/* R75: the call site is recorded when the submit flushes pending general draws. */
#define POKOPIA_VK_STR2(x) #x
#define POKOPIA_VK_STR(x) POKOPIA_VK_STR2(x)
#define pokopia_vk_submit_wait(adapter, command, retired) \
    pokopia_vk_submit_wait_at((adapter), (command), (retired), __FILE__ ":" POKOPIA_VK_STR(__LINE__))
typedef struct { const char *site; uint64_t count; } PokopiaVkFlushSite;
extern PokopiaVkFlushSite pokopia_vk_flush_sites[16];
VkResult pokopia_vk_clear_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, const float rgba[4]);
/* R87: a 1x1x1 volume texture of one RGBA8 colour, made from a 2D texture's
 * config (format, swizzle) and kept, for shaders that sample a 3D texture
 * where the game bound a small 2D placeholder. */
PokopiaVkTexture *pokopia_vk_volume_placeholder(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *like,
                                                uint32_t rgba8);
VkResult pokopia_vk_clear_color_region(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, const float rgba[4]);
VkResult pokopia_vk_clear_depth_stencil(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, VkImageAspectFlags aspects, VkClearDepthStencilValue value);
/* One side of an image copy: a mip level, a range of array layers (or one
 * 3D level with a z range), and a texel box already clipped to that level. */
typedef struct {
    uint32_t level, layer, layers;
    bool volume;
    VkOffset3D offset;
    VkExtent3D extent;
} PokopiaVkCopyRegion;
/* Copies (equal formats and sizes) or blits between two images and waits.
 * VK_ERROR_FORMAT_NOT_SUPPORTED means the pair cannot be copied this way. */
VkResult pokopia_vk_copy_texture(PokopiaVkAdapter *adapter, PokopiaVkTexture *source, const PokopiaVkCopyRegion *from,
                                 PokopiaVkTexture *destination, const PokopiaVkCopyRegion *to, bool linear);
/* Returns a cached sampled view of a colour texture with an NVN component
 * swizzle (ZERO 0, ONE 1, R 2, G 3, B 4, A 5) or VK_NULL_HANDLE. */
VkImageView pokopia_vk_texture_swizzle_view(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                            VkFormat view_format, const uint8_t swizzle[4]);
VkImageView pokopia_vk_texture_typed_view(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                          VkFormat view_format, const uint8_t swizzle[4], VkImageViewType view_type);
/* R73: debug readback of one level and layer to a raw "PDMP" file. */
VkResult pokopia_vk_texture_dump(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, uint32_t level,
                                 uint32_t layer, const char *path);
/* R96: bytes per texel of an uncompressed colour texture (0: not readable). */
uint32_t pokopia_vk_texture_texel_bytes(const PokopiaVkTexture *texture);
/* R96: reads a region of one level (layers [layer, layer+layers)) back into
 * `destination`, rows `row_stride` bytes apart and layers `image_stride`
 * apart, and waits. Leaves the texture in TRANSFER_SRC, as the dump does. */
VkResult pokopia_vk_texture_read_region(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, uint32_t level,
                                        uint32_t layer, uint32_t layers, VkOffset2D offset, VkExtent2D extent,
                                        uint8_t *destination, size_t row_stride, size_t image_stride);
/* Copies every level and layer of a texture into its feedback copy (made on
 * first use from the texture's config) and waits. NVN lets a draw sample the
 * texture it renders to; Vulkan draws sample the copy instead. */
VkResult pokopia_vk_texture_snapshot(PokopiaVkAdapter *adapter, PokopiaVkTexture *source,
                                     const NvnFrontTextureConfig *config, PokopiaVkTexture **snapshot);
/* R75: as above, recorded into an open command buffer (no submit) when into is set. */
VkResult pokopia_vk_texture_snapshot_into(PokopiaVkAdapter *adapter, PokopiaVkTexture *source,
                                          const NvnFrontTextureConfig *config, PokopiaVkTexture **snapshot,
                                          VkCommandBuffer *batch);
void pokopia_vk_close(PokopiaVkAdapter *adapter);

#endif
