/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "vk_adapter.h"
#include "nvn_guest_layout.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const VkExtent2D graphics_extent = {1920, 1080};

bool pokopia_vk_output(const PokopiaVkAdapter *adapter, VkExtent2D *extent)
{
#ifdef _WIN32
    if (!adapter->output.image || !adapter->output.memory)
        return false;
    *extent = graphics_extent;
#else
    if (!adapter->present.swapchain)
        return false;
    *extent = adapter->present.extent;
#endif
    return true;
}

static void close_image(PokopiaVkAdapter *adapter, PokopiaVkImage *image)
{
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    if (image->image) {
        PFN_vkDestroyImage destroy = (PFN_vkDestroyImage)proc(device, "vkDestroyImage");
        destroy(device, image->image, NULL);
    }
    if (image->memory) {
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)proc(device, "vkFreeMemory");
        release(device, image->memory, NULL);
    }
    *image = (PokopiaVkImage){0};
}

static VkResult open_image(PokopiaVkAdapter *adapter, const VkImageCreateInfo *info, PokopiaVkImage *output)
{
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkCreateImage create = (PFN_vkCreateImage)proc(device, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements requirements =
        (PFN_vkGetImageMemoryRequirements)proc(device, "vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)proc(device, "vkAllocateMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)proc(device, "vkBindImageMemory");
    if (!create || !requirements || !allocate || !bind ||
        !proc(device, "vkDestroyImage") || !proc(device, "vkFreeMemory"))
        return VK_ERROR_INITIALIZATION_FAILED;
    PokopiaVkImage image = {0};
    VkResult result = create(device, info, NULL, &image.image);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements memory;
    requirements(device, image.image, &memory);
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image.image,
    };
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        if (!(memory.memoryTypeBits & (UINT32_C(1) << i)) ||
            !(adapter->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            continue;
        const VkMemoryAllocateInfo request = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
            .allocationSize = memory.size, .memoryTypeIndex = i,
        };
        VkDeviceMemory allocated = VK_NULL_HANDLE;
        result = allocate(device, &request, NULL, &allocated);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            continue;
        if (result == VK_SUCCESS) {
            image.memory = allocated;
            result = bind(device, image.image, image.memory, 0);
        }
        if (result == VK_SUCCESS) {
            image.bytes = memory.size;
            *output = image;
            return VK_SUCCESS;
        }
        break;
    }
    close_image(adapter, &image);
    return result;
}

#ifdef _WIN32
static VkResult create_output_image(PokopiaVkAdapter *adapter)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {graphics_extent.width, graphics_extent.height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    return open_image(adapter, &info, &adapter->output);
}
#endif

static VkFormat storage_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

static VkImageAspectFlags depth_aspects(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return 0;
    }
}

static VkResult check_image_support(PokopiaVkAdapter *adapter, const VkPhysicalDeviceImageFormatInfo2 *query,
                                    const VkImageCreateInfo *image)
{
    VkImageFormatProperties2 supported = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    VkResult result = adapter->image_format_properties(adapter->physical, query, &supported);
    if (result != VK_SUCCESS)
        return result;
    const VkImageFormatProperties *limits = &supported.imageFormatProperties;
    if (image->extent.width > limits->maxExtent.width || image->extent.height > limits->maxExtent.height ||
        image->extent.depth > limits->maxExtent.depth || image->mipLevels > limits->maxMipLevels ||
        image->arrayLayers > limits->maxArrayLayers || !(limits->sampleCounts & image->samples))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return VK_SUCCESS;
}

void pokopia_vk_texture_close(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture)
{
    if (texture->texel.buffer)
        pokopia_vk_texel_close(&texture->texel);
    if (texture->view || texture->sample_view || texture->sample_alias_view || texture->storage_view) {
        PFN_vkDestroyImageView destroy = (PFN_vkDestroyImageView)
            adapter->get_device_proc(adapter->device, "vkDestroyImageView");
        if (texture->storage_view)
            destroy(adapter->device, texture->storage_view, NULL);
        if (texture->sample_view)
            destroy(adapter->device, texture->sample_view, NULL);
        if (texture->sample_alias_view)
            destroy(adapter->device, texture->sample_alias_view, NULL);
        if (texture->view)
            destroy(adapter->device, texture->view, NULL);
    }
    close_image(adapter, &texture->backing);
    *texture = (PokopiaVkTexture){0};
}

static VkResult open_view(PFN_vkCreateImageView create_view, VkDevice device, VkImage image,
                         VkImageViewType type, VkFormat format, VkImageSubresourceRange subresources,
                         VkImageUsageFlags usage, VkImageView *output)
{
    const VkImageViewUsageCreateInfo view_usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage,
    };
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &view_usage,
        .image = image, .viewType = type, .format = format,
        .subresourceRange = subresources,
    };
    VkImageView created = VK_NULL_HANDLE;
    VkResult result = create_view(device, &view, NULL, &created);
    if (result == VK_SUCCESS)
        *output = created;
    return result;
}

VkResult pokopia_vk_texture_open(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                 PokopiaVkTexture *texture)
{
    if (!adapter->device || texture->backing.image || !adapter->image_format_properties)
        return VK_ERROR_INITIALIZATION_FAILED;
    enum {
        TEXTURE_2D = 1, TEXTURE_3D = 2, TEXTURE_2D_ARRAY = 4, TEXTURE_CUBE = 8, TEXTURE_CUBE_ARRAY = 9,
        CUBE_FACES = 6, TEXTURE_DISPLAY = 1,
        TEXTURE_IMAGE = 4, TEXTURE_COMPRESSIBLE = 8, TEXTURE_LINEAR = 16,
    };
    VkFormat format = nvn_guest_texture_vk_format(config->format);
    const VkFormat linear_format = storage_format(format);
    const bool bc7 = format == VK_FORMAT_BC7_UNORM_BLOCK || format == VK_FORMAT_BC7_SRGB_BLOCK;
    const VkFormat sample_alias_format = format == VK_FORMAT_BC7_UNORM_BLOCK ? VK_FORMAT_BC7_SRGB_BLOCK :
        format == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    const VkImageAspectFlags depth = depth_aspects(format);
    const bool stencil = (depth & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    const bool volume = config->target == TEXTURE_3D;
    const bool array = config->target == TEXTURE_2D_ARRAY;
    const bool cube = config->target == TEXTURE_CUBE;
    const bool cube_array = config->target == TEXTURE_CUBE_ARRAY;
    const bool cube_compatible = cube || cube_array;
    const bool layered = array || cube_compatible;
    const bool pitch_linear = (config->flags & TEXTURE_LINEAR) != 0;
    const VkImageViewType view_type = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    const unsigned texel_bytes = linear_format == VK_FORMAT_R8_UNORM ? 1 :
        linear_format == VK_FORMAT_R16_SFLOAT ? 2 :
        linear_format == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 :
        (linear_format == VK_FORMAT_R16G16B16A16_UNORM || linear_format == VK_FORMAT_R16G16B16A16_SFLOAT ||
         linear_format == VK_FORMAT_R32G32_SFLOAT) ? 8 : 4;
    uint32_t max_levels = 0;
    uint32_t edge = config->width > config->height ? config->width : config->height;
    for (; edge; edge >>= 1)
        ++max_levels;
    /* SetSize2D leaves a cube's depth field at one. Cube arrays count faces. */
    const bool valid_cube = config->width == config->height &&
        (cube_array ? config->depth % CUBE_FACES == 0 : (config->depth == 1 || config->depth == CUBE_FACES));
    const bool valid_shape = layered ? (!depth && !pitch_linear && config->depth &&
        !(config->flags & TEXTURE_DISPLAY) && (!cube_compatible || valid_cube)) :
        volume ? (!depth && config->depth && !(config->flags & TEXTURE_DISPLAY)) :
        (config->target == TEXTURE_2D && config->depth == 1);
    /* Guest uploads and alternate subresource views have separate lifetimes.
     * Here we support attachment shapes with no packaged initial contents. */
    if ((!depth && !bc7 && linear_format != VK_FORMAT_R8_UNORM && linear_format != VK_FORMAT_R16_SFLOAT &&
         linear_format != VK_FORMAT_R8G8B8A8_UNORM && linear_format != VK_FORMAT_B8G8R8A8_UNORM &&
         linear_format != VK_FORMAT_R16G16_SFLOAT && linear_format != VK_FORMAT_R16G16B16A16_SFLOAT &&
         linear_format != VK_FORMAT_R16G16B16A16_UNORM &&
         linear_format != VK_FORMAT_R32_SFLOAT && linear_format != VK_FORMAT_R32G32_SFLOAT &&
         linear_format != VK_FORMAT_R32G32B32A32_SFLOAT &&
         linear_format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) ||
        (bc7 && (config->target != TEXTURE_2D || (config->flags & (TEXTURE_DISPLAY | TEXTURE_IMAGE | TEXTURE_LINEAR)))) ||
        !valid_shape || !config->width || !config->height ||
        !config->levels || config->levels > max_levels ||
        (config->levels > 1 && (volume || pitch_linear || depth || (config->flags & TEXTURE_DISPLAY))) ||
        config->samples > 1 || config->packaged_layout ||
        config->packaged_data || config->gl_texture_name ||
        (pitch_linear ? (volume || depth || (config->stride &&
            (config->stride % NVN_GUEST_LINEAR_STRIDE_ALIGNMENT ||
             config->stride < (uint64_t)config->width * texel_bytes))) :
            config->stride != 0) ||
        (depth && ((config->flags & TEXTURE_DISPLAY) || config->depth_stencil_mode > (unsigned)stencil)) ||
        (config->flags & ~(uint32_t)(TEXTURE_DISPLAY | TEXTURE_IMAGE | TEXTURE_COMPRESSIBLE | TEXTURE_LINEAR)) ||
        config->swizzle[0] != 2 || config->swizzle[1] != 3 || config->swizzle[2] != 4 || config->swizzle[3] != 5)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if ((cube_array && !adapter->features.imageCubeArray) || (bc7 && !adapter->features.textureCompressionBC))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    /* NVN's IMAGE hint is also present on depth attachments. Shader image
     * stores use color views, and depth/stencil uses attachment aspects.
     * Vulkan backing remains optimal for pitch-linear guest pool storage. */
    const bool storage = !depth && (config->flags & TEXTURE_IMAGE) != 0;
    const bool alias = storage && linear_format != format;
    const VkFormat formats[] = {format, bc7 ? sample_alias_format : linear_format};
    const VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = formats,
    };
    const VkImageUsageFlags attachment = volume || bc7 ? 0 :
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const VkImageUsageFlags view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | attachment;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = alias || bc7 ? &format_list : NULL,
        .flags = (alias || bc7 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) |
                 (alias ? VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0) |
                 (cube_compatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
        .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = format,
        .extent = {config->width, config->height, volume ? config->depth : 1},
        .mipLevels = config->levels, .arrayLayers = cube ? CUBE_FACES : layered ? config->depth : 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = view_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
    };
    VkPhysicalDeviceImageFormatInfo2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = image.pNext,
        .format = image.format, .type = image.imageType, .tiling = image.tiling,
        .usage = image.usage, .flags = image.flags,
    };
    VkResult result = check_image_support(adapter, &query, &image);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED &&
        (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32)) {
        /* Liverpool exposes float depth attachments. The NVN pool still
         * uses its original 24-bit footprint, independent of this image. */
        format = stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
        image.format = query.format = format;
        result = check_image_support(adapter, &query, &image);
    }
    if (result != VK_SUCCESS)
        return result;
    if (alias) {
        /* Check each view's usages directly. Orbis RADV skips usage checks
         * on the extended-usage query for the underlying image. */
        query.pNext = NULL;
        query.flags = image.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        query.usage = image.usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;
        result = check_image_support(adapter, &query, &image);
        if (result != VK_SUCCESS)
            return result;
        query.format = linear_format;
        query.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        result = check_image_support(adapter, &query, &image);
        if (result != VK_SUCCESS)
            return result;
    }

    VkDevice device = adapter->device;
    PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)adapter->get_device_proc(device, "vkCreateImageView");
    if (!create_view || !adapter->get_device_proc(device, "vkDestroyImageView"))
        return VK_ERROR_INITIALIZATION_FAILED;
    PokopiaVkTexture opened = {
        .format = format, .sample_alias_format = sample_alias_format,
        .aspects = depth ? depth : VK_IMAGE_ASPECT_COLOR_BIT,
        .extent = {image.extent.width, image.extent.height}, .array_layers = image.arrayLayers,
        .stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    };
    result = open_image(adapter, &image, &opened.backing);
    if (result != VK_SUCCESS)
        return result;
    /* The attachment view selects the first base-level layer. Sampled
     * views cover the whole image. Storage views cover one mip level. */
    const VkImageSubresourceRange base = {opened.aspects, 0, 1, 0, 1};
    const VkImageViewType sample_type = cube_array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
        cube ? VK_IMAGE_VIEW_TYPE_CUBE : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : view_type;
    const bool separate_sample = stencil || layered || (!bc7 && image.mipLevels > 1);
    VkImageSubresourceRange primary = base;
    if (bc7)
        primary.levelCount = image.mipLevels;
    result = open_view(create_view, device, opened.backing.image, view_type, format,
                       primary, separate_sample ? attachment : view_usage, &opened.view);
    if (result == VK_SUCCESS && separate_sample) {
        VkImageSubresourceRange sampled = base;
        if (stencil)
            sampled.aspectMask = config->depth_stencil_mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        sampled.levelCount = image.mipLevels;
        sampled.layerCount = image.arrayLayers;
        result = open_view(create_view, device, opened.backing.image, sample_type, format, sampled,
                           VK_IMAGE_USAGE_SAMPLED_BIT, &opened.sample_view);
    }
    if (result == VK_SUCCESS && bc7) {
        VkImageSubresourceRange sampled = base;
        sampled.levelCount = image.mipLevels;
        result = open_view(create_view, device, opened.backing.image, sample_type, sample_alias_format, sampled,
                           VK_IMAGE_USAGE_SAMPLED_BIT, &opened.sample_alias_view);
    }
    if (result == VK_SUCCESS && storage) {
        /* Storage accesses the same color bytes through the linear format. */
        VkImageSubresourceRange writable = base;
        writable.layerCount = image.arrayLayers;
        result = open_view(create_view, device, opened.backing.image, sample_type, linear_format, writable,
                           VK_IMAGE_USAGE_STORAGE_BIT, &opened.storage_view);
    }
    if (result != VK_SUCCESS) {
        pokopia_vk_texture_close(adapter, &opened);
        return result;
    }
    *texture = opened;
    return VK_SUCCESS;
}

VkResult pokopia_vk_command_buffer_open(PokopiaVkAdapter *adapter, VkCommandBuffer *command)
{
    if (!adapter->device)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)proc(device, "vkAllocateCommandBuffers");
    if (!allocate || !proc(device, "vkFreeCommandBuffers"))
        return VK_ERROR_INITIALIZATION_FAILED;

    VkCommandPool pool = adapter->guest_commands;
    if (!pool) {
        const VkCommandPoolCreateInfo request = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = adapter->family,
        };
        VkResult result = adapter->create_command_pool(device, &request, NULL, &pool);
        if (result != VK_SUCCESS)
            return result;
    }
    const VkCommandBufferAllocateInfo request = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer allocated = VK_NULL_HANDLE;
    VkResult result = allocate(device, &request, &allocated);
    if (result == VK_SUCCESS) {
        adapter->guest_commands = pool;
        *command = allocated;
    } else if (!adapter->guest_commands) {
        adapter->destroy_command_pool(device, pool, NULL);
    }
    return result;
}

void pokopia_vk_command_buffer_close(PokopiaVkAdapter *adapter, VkCommandBuffer command)
{
    PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)
        adapter->get_device_proc(adapter->device, "vkFreeCommandBuffers");
    release(adapter->device, adapter->guest_commands, 1, &command);
}

VkResult pokopia_vk_submit_wait(PokopiaVkAdapter *adapter, VkCommandBuffer command, bool *retired)
{
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)proc(device, "vkQueueSubmit");
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)proc(device, "vkQueueWaitIdle");
    *retired = true;
    if (!submit || !wait)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkSubmitInfo info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    VkResult result = submit(adapter->queue, 1, &info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return result;
    result = wait(adapter->queue);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        VkResult idle = adapter->wait_idle(device);
        *retired = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST;
    }
    return result;
}

VkResult pokopia_vk_clear_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, const float rgba[4])
{
    if (!texture || !texture->backing.image || texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
        texture->sample_alias_view || adapter->render_serial == UINT64_MAX)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdClearColorImage clear = (PFN_vkCmdClearColorImage)proc(device, "vkCmdClearColorImage");
    if (!begin || !end || !barrier || !clear)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandBuffer command;
    VkResult result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = begin(command, &info);
    if (result == VK_SUCCESS) {
        const VkImageMemoryBarrier target = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = texture->access, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = texture->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->backing.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        barrier(command, texture->stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &target);
        VkClearColorValue value;
        memcpy(value.float32, rgba, sizeof value.float32);
        clear(command, target.image, target.newLayout, &value, 1, &target.subresourceRange);
        result = end(command);
    }
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (retired)
        pokopia_vk_command_buffer_close(adapter, command);
    if (result == VK_SUCCESS) {
        texture->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        texture->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        texture->access = VK_ACCESS_TRANSFER_WRITE_BIT;
        texture->render_serial = ++adapter->render_serial;
    }
    return result;
}

static VkResult clear_attachment(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                 VkRect2D area, VkImageAspectFlags aspects, VkClearValue value)
{
    const VkImageAspectFlags depth_stencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const bool color = aspects == VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags target_aspects = color ? VK_IMAGE_ASPECT_COLOR_BIT : depth_stencil;
    if (!texture || !texture->backing.image || !texture->view || !texture->stages ||
        texture->aspects != target_aspects || texture->array_layers != 1 || !aspects || (aspects & ~target_aspects) ||
        area.offset.x < 0 || area.offset.y < 0 || !area.extent.width || !area.extent.height ||
        (uint64_t)area.offset.x + area.extent.width > texture->extent.width ||
        (uint64_t)area.offset.y + area.extent.height > texture->extent.height ||
        (color && (texture->sample_alias_view || adapter->render_serial == UINT64_MAX)) ||
        ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && !(value.depthStencil.depth >= 0 && value.depthStencil.depth <= 1)) ||
        (!color && value.depthStencil.stencil > 0xff))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    const VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdBeginRendering begin_rendering = (PFN_vkCmdBeginRendering)proc(device, "vkCmdBeginRendering");
    PFN_vkCmdEndRendering end_rendering = (PFN_vkCmdEndRendering)proc(device, "vkCmdEndRendering");
    if (!begin || !end || !barrier || !begin_rendering || !end_rendering)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkCommandBuffer command;
    VkResult result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const VkPipelineStageFlags stages = color ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT :
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkImageMemoryBarrier transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = texture->access,
        .dstAccessMask = color ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT :
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = texture->layout, .newLayout = color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = {target_aspects, 0, 1, 0, 1},
    };
    result = begin(command, &info);
    if (result == VK_SUCCESS) {
        barrier(command, texture->stages, stages, 0, 0, NULL, 0, NULL, 1, &transition);
        const VkAttachmentLoadOp preserve = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED ?
            VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        const VkRenderingAttachmentInfo attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = texture->view,
            .imageLayout = transition.newLayout, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .loadOp = (aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT)) ?
                VK_ATTACHMENT_LOAD_OP_CLEAR : preserve,
            .clearValue = value,
        };
        VkRenderingAttachmentInfo stencil = attachment;
        stencil.loadOp = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : preserve;
        const VkRenderingInfo rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
            .colorAttachmentCount = color, .pColorAttachments = color ? &attachment : NULL,
            .pDepthAttachment = color ? NULL : &attachment, .pStencilAttachment = color ? NULL : &stencil,
        };
        begin_rendering(command, &rendering);
        end_rendering(command);
        result = end(command);
    }
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (retired)
        pokopia_vk_command_buffer_close(adapter, command);
    if (result == VK_SUCCESS) {
        texture->layout = transition.newLayout;
        texture->stages = stages;
        texture->access = transition.dstAccessMask;
        if (color)
            texture->render_serial = ++adapter->render_serial;
    }
    return result;
}

VkResult pokopia_vk_clear_depth_stencil(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                       VkRect2D area, VkImageAspectFlags aspects, VkClearDepthStencilValue value)
{
    return clear_attachment(adapter, texture, area, aspects, (VkClearValue){.depthStencil = value});
}

VkResult pokopia_vk_clear_color_region(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, const float rgba[4])
{
    if (texture && area.offset.x == 0 && area.offset.y == 0 &&
        area.extent.width == texture->extent.width && area.extent.height == texture->extent.height)
        return pokopia_vk_clear_color(adapter, texture, rgba);
    VkClearValue value;
    memcpy(value.color.float32, rgba, sizeof value.color.float32);
    return clear_attachment(adapter, texture, area, VK_IMAGE_ASPECT_COLOR_BIT, value);
}

static PFN_vkGetInstanceProcAddr open_loader(PokopiaVkAdapter *adapter)
{
#ifdef _WIN32
    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    adapter->loader = loader;
    return loader ? (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr") : NULL;
#else
    (void)adapter;
    return vkGetInstanceProcAddr;
#endif
}

void pokopia_vk_close(PokopiaVkAdapter *adapter)
{
    if (adapter->device && adapter->wait_idle)
        adapter->wait_idle(adapter->device);
    if (adapter->guest_commands)
        adapter->destroy_command_pool(adapter->device, adapter->guest_commands, NULL);
#ifdef _WIN32
    close_image(adapter, &adapter->output);
#else
    if (adapter->present.device)
        pokopia_present_close(&adapter->present);
#endif
    if (adapter->device && adapter->destroy_device)
        adapter->destroy_device(adapter->device, NULL);
#ifndef _WIN32
    if (adapter->surface)
        vkDestroySurfaceKHR(adapter->instance, adapter->surface, NULL);
#endif
    if (adapter->instance && adapter->destroy_instance)
        adapter->destroy_instance(adapter->instance, NULL);
#ifdef _WIN32
    if (adapter->loader)
        FreeLibrary((HMODULE)adapter->loader);
#endif
    *adapter = (PokopiaVkAdapter){0};
}

static bool graphics_family(PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queues, VkPhysicalDevice physical,
                            uint32_t *family)
{
    uint32_t count = 0;
    get_queues(physical, &count, NULL);
    VkQueueFamilyProperties *queues = calloc(count, sizeof *queues);
    if (!queues)
        return false;
    get_queues(physical, &count, queues);
    const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & required) == required) {
            *family = i;
            found = true;
            break;
        }
    }
    free(queues);
    return found;
}

static bool select_device(PokopiaVkAdapter *adapter, PFN_vkGetInstanceProcAddr proc)
{
    VkInstance instance = adapter->instance;
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)proc(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)proc(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)proc(instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceFeatures features =
        (PFN_vkGetPhysicalDeviceFeatures)proc(instance, "vkGetPhysicalDeviceFeatures");
    if (!enumerate || !properties || !queues || !memory || !features)
        return false;

    uint32_t count = 0;
    if (enumerate(instance, &count, NULL) != VK_SUCCESS || !count)
        return false;
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices)
        return false;
    VkResult result = enumerate(instance, &count, devices);
    int score = 0;
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties caps;
            uint32_t family;
            properties(devices[i], &caps);
            if (caps.apiVersion < VK_API_VERSION_1_3 || caps.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                !graphics_family(queues, devices[i], &family))
                continue;
            int candidate_score = caps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (candidate_score > score) {
                score = candidate_score;
                adapter->physical = devices[i];
                adapter->family = family;
                adapter->properties = caps;
            }
        }
    }
    free(devices);
    if (!adapter->physical)
        return false;
    memory(adapter->physical, &adapter->memory);
    VkPhysicalDeviceFeatures available;
    features(adapter->physical, &available);
    adapter->features.imageCubeArray = available.imageCubeArray;
    adapter->features.samplerAnisotropy = available.samplerAnisotropy;
    adapter->features.shaderFloat64 = available.shaderFloat64;
    adapter->features.shaderImageGatherExtended = available.shaderImageGatherExtended;
    adapter->features.textureCompressionBC = available.textureCompressionBC;
    adapter->features.textureCompressionETC2 = available.textureCompressionETC2;
    adapter->features.textureCompressionASTC_LDR = available.textureCompressionASTC_LDR;
    return true;
}

bool pokopia_vk_open(PokopiaVkAdapter *adapter, FILE *log)
{
    if (adapter->device)
        return true;
#ifndef _WIN32
    /* The bundled WSI otherwise holds the first flip until the next present.
     * Set this before swapchain probing caches the presentation mode. */
    if (setenv("ORBIS_WSI_NO_DEFER", "1", 1) != 0) {
        fputs("FAIL could not select immediate Orbis VideoOut\n", log);
        fflush(log);
        return false;
    }
#endif
    PFN_vkGetInstanceProcAddr proc = open_loader(adapter);
    const char *failed = "Vulkan loader";
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (!proc)
        goto fail;
#ifdef _WIN32
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)proc(NULL, "vkCreateInstance");
    if (!create_instance)
        goto fail;
#else
    /* The Orbis loader records this instance for its exported Vulkan thunks. */
    PFN_vkCreateInstance create_instance = vkCreateInstance;
#endif
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pokopia PS4",
        .apiVersion = VK_API_VERSION_1_3,
    };
#ifndef _WIN32
    const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
#endif
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
#ifndef _WIN32
        .enabledExtensionCount = sizeof instance_extensions / sizeof instance_extensions[0],
        .ppEnabledExtensionNames = instance_extensions,
#endif
    };
    failed = "Vulkan instance";
    result = create_instance(&instance_info, NULL, &adapter->instance);
    if (result != VK_SUCCESS)
        goto fail;
    adapter->destroy_instance = (PFN_vkDestroyInstance)proc(adapter->instance, "vkDestroyInstance");
#ifndef _WIN32
    failed = "Orbis VideoOut surface";
    result = pokopia_present_surface(adapter->instance, &adapter->surface);
    if (result != VK_SUCCESS)
        goto fail;
#endif

    failed = "Vulkan 1.3 graphics and compute adapter";
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (!select_device(adapter, proc))
        goto fail;
    adapter->image_format_properties = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
        proc(adapter->instance, "vkGetPhysicalDeviceImageFormatProperties2");
    adapter->format_properties = (PFN_vkGetPhysicalDeviceFormatProperties)
        proc(adapter->instance, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)proc(adapter->instance, "vkCreateDevice");
    adapter->get_device_proc = (PFN_vkGetDeviceProcAddr)proc(adapter->instance, "vkGetDeviceProcAddr");
    adapter->destroy_device = (PFN_vkDestroyDevice)proc(adapter->instance, "vkDestroyDevice");
    if (!create_device || !adapter->get_device_proc || !adapter->destroy_device)
        goto fail;

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = adapter->family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan13Features enabled_13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features enabled_12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &enabled_13,
    };
    VkPhysicalDeviceVulkan11Features enabled_11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &enabled_12,
    };
    PFN_vkGetPhysicalDeviceFeatures2 features2 =
        (PFN_vkGetPhysicalDeviceFeatures2)proc(adapter->instance, "vkGetPhysicalDeviceFeatures2");
    if (features2) {
        VkPhysicalDeviceVulkan13Features available_13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceVulkan12Features available_12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &available_13,
        };
        VkPhysicalDeviceVulkan11Features available_11 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &available_12,
        };
        VkPhysicalDeviceFeatures2 available = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &available_11,
        };
        features2(adapter->physical, &available);
        enabled_13.dynamicRendering = available_13.dynamicRendering;
        enabled_13.synchronization2 = available_13.synchronization2;
        enabled_12.timelineSemaphore = available_12.timelineSemaphore;
        enabled_11.shaderDrawParameters = available_11.shaderDrawParameters;
    }
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled_11,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .pEnabledFeatures = &adapter->features,
#ifndef _WIN32
        .enabledExtensionCount = sizeof device_extensions / sizeof device_extensions[0],
        .ppEnabledExtensionNames = device_extensions,
#endif
    };
    failed = "Vulkan logical device";
    result = create_device(adapter->physical, &device_info, NULL, &adapter->device);
    if (result != VK_SUCCESS)
        goto fail;
    PFN_vkGetDeviceProcAddr dproc = adapter->get_device_proc;
    VkDevice device = adapter->device;
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)dproc(device, "vkGetDeviceQueue");
    adapter->wait_idle = (PFN_vkDeviceWaitIdle)dproc(device, "vkDeviceWaitIdle");
    adapter->create_command_pool = (PFN_vkCreateCommandPool)dproc(device, "vkCreateCommandPool");
    adapter->destroy_command_pool = (PFN_vkDestroyCommandPool)dproc(device, "vkDestroyCommandPool");
    failed = "Vulkan queue functions";
    result = VK_ERROR_INITIALIZATION_FAILED;
    if (!get_queue || !adapter->wait_idle || !adapter->create_command_pool || !adapter->destroy_command_pool)
        goto fail;
    get_queue(device, adapter->family, 0, &adapter->queue);
    if (!adapter->queue)
        goto fail;
#ifdef _WIN32
    failed = "desktop offscreen output";
    result = create_output_image(adapter);
    if (result != VK_SUCCESS)
        goto fail;
    fprintf(log, "Desktop offscreen Vulkan output %ux%u\n", graphics_extent.width, graphics_extent.height);
#else
    failed = "Orbis VideoOut swapchain";
    result = pokopia_present_open(&adapter->present, adapter->physical, device, adapter->family,
                                  adapter->queue, adapter->surface, VK_FORMAT_R8G8B8A8_UNORM,
                                  graphics_extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (result != VK_SUCCESS)
        goto fail;
    fprintf(log, "Pokopia backend VideoOut %ux%u, %u images\n", adapter->present.extent.width,
            adapter->present.extent.height, adapter->present.image_count);
#endif

    const uint32_t version = adapter->properties.apiVersion;
    fprintf(log, "Vulkan adapter %s API %u.%u.%u queue-family=%u ASTC=%u BC=%u\n",
            adapter->properties.deviceName, VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
            VK_API_VERSION_PATCH(version), adapter->family, adapter->features.textureCompressionASTC_LDR,
            adapter->features.textureCompressionBC);
    for (uint32_t i = 0; i < adapter->memory.memoryHeapCount; ++i) {
        const VkMemoryHeap *heap = &adapter->memory.memoryHeaps[i];
        fprintf(log, "Vulkan heap %u size=%" PRIu64 " bytes flags=%#x\n", i, heap->size, heap->flags);
    }
    fflush(log);
    return true;

fail:
    fprintf(log, "FAIL %s (%d)\n", failed, result);
    fflush(log);
    pokopia_vk_close(adapter);
    return false;
}
