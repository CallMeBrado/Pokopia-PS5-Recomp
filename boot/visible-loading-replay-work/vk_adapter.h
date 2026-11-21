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
} PokopiaVkImage;

typedef struct {
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
/* Owns the Vulkan image independently of the NVN guest pool. Start with a
 * zeroed texture and retire submissions that reference it before closing. */
VkResult pokopia_vk_texture_open(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                 PokopiaVkTexture *texture);
void pokopia_vk_texture_close(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture);
/* Guest command buffers can outlive individual NVN queue objects. Serialize
 * allocation and release, and retire any submission before release. */
VkResult pokopia_vk_command_buffer_open(PokopiaVkAdapter *adapter, VkCommandBuffer *command);
void pokopia_vk_command_buffer_close(PokopiaVkAdapter *adapter, VkCommandBuffer command);
/* Commits a guest buffer transfer only after its Vulkan copy completes. */
VkResult pokopia_vk_copy_guest(PokopiaVkAdapter *adapter, const void *source, void *destination, size_t bytes);
VkResult pokopia_vk_submit_wait(PokopiaVkAdapter *adapter, VkCommandBuffer command, bool *retired);
VkResult pokopia_vk_clear_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, const float rgba[4]);
VkResult pokopia_vk_clear_color_region(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, const float rgba[4]);
VkResult pokopia_vk_clear_depth_stencil(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, VkImageAspectFlags aspects, VkClearDepthStencilValue value);
void pokopia_vk_close(PokopiaVkAdapter *adapter);

#endif
