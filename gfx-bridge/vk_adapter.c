/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "vk_adapter.h"
#include "vk_flush.h"
#include "nvn_guest_layout.h"
#include "vk_ledger.h"

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

/* Keep optimal images in their own blocks so buffers and linear resources
 * never share a Vulkan bufferImageGranularity page with these images. */
enum {
    IMAGE_BLOCK_BYTES = 1024 * 1024,
    IMAGE_BLOCK_MAX_SPAN = 256 * 1024,
};

typedef struct PokopiaVkImageBlock PokopiaVkImageBlock;
typedef struct PokopiaVkImageSpan {
    struct PokopiaVkImageSpan *next;
    PokopiaVkImageBlock *block;
    VkDeviceSize offset, bytes;
} PokopiaVkImageSpan;

struct PokopiaVkImageBlock {
    PokopiaVkImageBlock *next;
    PokopiaVkImageSpan *spans;
    VkDeviceMemory memory;
    uint32_t memory_type;
};

static bool place_image_span(PokopiaVkImageBlock *block, VkDeviceSize alignment, PokopiaVkImageSpan *span)
{
    VkDeviceSize cursor = 0;
    PokopiaVkImageSpan **link = &block->spans;
    while (true) {
        const VkDeviceSize end = *link ? (*link)->offset : IMAGE_BLOCK_BYTES;
        const VkDeviceSize offset = (cursor + alignment - 1) & ~(alignment - 1);
        if (offset <= end && span->bytes <= end - offset) {
            span->block = block;
            span->offset = offset;
            span->next = *link;
            *link = span;
            return true;
        }
        if (!*link)
            return false;
        cursor = (*link)->offset + (*link)->bytes;
        link = &(*link)->next;
    }
}

static VkResult allocate_image_span(PokopiaVkAdapter *adapter, PFN_vkAllocateMemory allocate,
                                    const VkMemoryRequirements *requirements, uint32_t memory_type,
                                    PokopiaVkImageSpan **output)
{
    PokopiaVkImageSpan *span = calloc(1, sizeof *span);
    if (!span)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    span->bytes = requirements->size;
    for (PokopiaVkImageBlock *block = adapter->image_blocks; block; block = block->next) {
        if (block->memory_type == memory_type && place_image_span(block, requirements->alignment, span)) {
            *output = span;
            return VK_SUCCESS;
        }
    }

    PokopiaVkImageBlock *block = calloc(1, sizeof *block);
    if (!block) {
        free(span);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const VkMemoryAllocateInfo request = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = IMAGE_BLOCK_BYTES, .memoryTypeIndex = memory_type,
    };
    VkResult result = pokopia_vk_ledger_allocate(allocate, adapter->device, &request, &block->memory,
                                                  VK_LEDGER_NV_TEXTURE);
    if (result != VK_SUCCESS) {
        free(block);
        free(span);
        return result;
    }
    block->memory_type = memory_type;
    block->next = adapter->image_blocks;
    adapter->image_blocks = block;
    span->block = block;
    block->spans = span;
    *output = span;
    return VK_SUCCESS;
}

static void release_image_span(PokopiaVkAdapter *adapter, PFN_vkFreeMemory release, PokopiaVkImageSpan *span)
{
    PokopiaVkImageBlock *block = span->block;
    PokopiaVkImageSpan **link = &block->spans;
    while (*link != span)
        link = &(*link)->next;
    *link = span->next;
    free(span);
    if (block->spans)
        return;

    PokopiaVkImageBlock **block_link = &adapter->image_blocks;
    while (*block_link != block)
        block_link = &(*block_link)->next;
    *block_link = block->next;
    pokopia_vk_ledger_free(release, adapter->device, block->memory);
    free(block);
}

static void close_image(PokopiaVkAdapter *adapter, PokopiaVkImage *image)
{
    pokopia_vk_flush_pending(POKOPIA_FLUSH_CLOSE_IMAGE);
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    if (image->image) {
        PFN_vkDestroyImage destroy = (PFN_vkDestroyImage)proc(device, "vkDestroyImage");
        destroy(device, image->image, NULL);
    }
    if (image->memory) {
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)proc(device, "vkFreeMemory");
        if (image->span)
            release_image_span(adapter, release, image->span);
        else
            pokopia_vk_ledger_free(release, device, image->memory);
    }
    *image = (PokopiaVkImage){0};
}

static VkResult open_image(PokopiaVkAdapter *adapter, const VkImageCreateInfo *info, PokopiaVkImage *output,
                           PokopiaVkLedgerClass category)
{
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkCreateImage create = (PFN_vkCreateImage)proc(device, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements requirements =
        (PFN_vkGetImageMemoryRequirements)proc(device, "vkGetImageMemoryRequirements");
    PFN_vkGetImageMemoryRequirements2 requirements2 =
        (PFN_vkGetImageMemoryRequirements2)proc(device, "vkGetImageMemoryRequirements2");
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)proc(device, "vkAllocateMemory");
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)proc(device, "vkBindImageMemory");
    if (!create || !requirements || !allocate || !bind ||
        !proc(device, "vkDestroyImage") || !proc(device, "vkFreeMemory"))
        return VK_ERROR_INITIALIZATION_FAILED;
    PokopiaVkImage image = {0};
    VkResult result = create(device, info, NULL, &image.image);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryDedicatedRequirements dedicated_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 image_requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated_requirements,
    };
    const bool allow_pool = requirements2 && category == VK_LEDGER_NV_TEXTURE &&
        info->tiling == VK_IMAGE_TILING_OPTIMAL;
    if (allow_pool) {
        const VkImageMemoryRequirementsInfo2 query = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image.image,
        };
        requirements2(device, &query, &image_requirements);
    } else {
        requirements(device, image.image, &image_requirements.memoryRequirements);
    }
    const VkMemoryRequirements memory = image_requirements.memoryRequirements;
    const bool pooled = allow_pool && !dedicated_requirements.requiresDedicatedAllocation &&
        !dedicated_requirements.prefersDedicatedAllocation && memory.size &&
        memory.size <= IMAGE_BLOCK_MAX_SPAN && memory.alignment && memory.alignment <= IMAGE_BLOCK_BYTES;
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image.image,
    };
    const char *failure = "memory type";
    uint32_t type = UINT32_MAX;
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        if (!(memory.memoryTypeBits & (UINT32_C(1) << i)) ||
            !(adapter->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            continue;
        failure = "allocate";
        type = i;
        VkDeviceSize offset = 0;
        if (pooled) {
            result = allocate_image_span(adapter, allocate, &memory, i, &image.span);
            if (result == VK_SUCCESS) {
                image.memory = image.span->block->memory;
                offset = image.span->offset;
            } else if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                break;
            }
        }
        if (!image.span) {
            const VkMemoryAllocateInfo request = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
                .allocationSize = memory.size, .memoryTypeIndex = i,
            };
            VkDeviceMemory allocated = VK_NULL_HANDLE;
            result = pokopia_vk_ledger_allocate(allocate, device, &request, &allocated, category);
            if (result == VK_SUCCESS)
                image.memory = allocated;
        }
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            continue;
        if (result == VK_SUCCESS) {
            failure = "bind";
            result = bind(device, image.image, image.memory, offset);
        }
        if (result == VK_SUCCESS) {
            image.bytes = memory.size;
            *output = image;
            return VK_SUCCESS;
        }
        break;
    }
    if (adapter->log) {
        fprintf(adapter->log, "Vulkan image backing %s result=%d %ux%ux%u format=%u bytes=%" PRIu64
                " alignment=%" PRIu64 " type-mask=%#" PRIx32 " last-type=%" PRIu32 "\n",
                failure, result, info->extent.width, info->extent.height, info->extent.depth,
                (unsigned)info->format, (uint64_t)memory.size, (uint64_t)memory.alignment,
                memory.memoryTypeBits, type);
        fflush(adapter->log);
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
    return open_image(adapter, &info, &adapter->output, VK_LEDGER_HOST_DISPLAY);
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
    pokopia_vk_flush_pending(POKOPIA_FLUSH_TEXTURE_CLOSE);
    if (texture->feedback_copy) {
        pokopia_vk_texture_close(adapter, texture->feedback_copy);
        free(texture->feedback_copy);
        texture->feedback_copy = NULL;
    }
    free(texture->guest_shadow);
    texture->guest_shadow = NULL;
    if (texture->texel.device)
        pokopia_vk_texel_close(&texture->texel);
    bool swizzled = false;
    for (size_t i = 0; i < sizeof texture->swizzle_views / sizeof texture->swizzle_views[0]; ++i)
        swizzled |= texture->swizzle_views[i].view != VK_NULL_HANDLE;
    if (texture->view || texture->sample_view || texture->sample_alias_view || texture->storage_view || swizzled) {
        PFN_vkDestroyImageView destroy = (PFN_vkDestroyImageView)
            adapter->get_device_proc(adapter->device, "vkDestroyImageView");
        for (size_t i = 0; i < sizeof texture->swizzle_views / sizeof texture->swizzle_views[0]; ++i)
            if (texture->swizzle_views[i].view)
                destroy(adapter->device, texture->swizzle_views[i].view, NULL);
        if (texture->storage_view)
            destroy(adapter->device, texture->storage_view, NULL);
        if (texture->sample_view)
            destroy(adapter->device, texture->sample_view, NULL);
        if (texture->sample_alias_view)
            destroy(adapter->device, texture->sample_alias_view, NULL);
        if (texture->view)
            destroy(adapter->device, texture->view, NULL);
    }
    if (pokopia_vk_image_forget_hook && texture->backing.image)
        pokopia_vk_image_forget_hook(texture->backing.image);
    if (pokopia_vk_texture_forget_hook)
        pokopia_vk_texture_forget_hook(texture);
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

static bool bc_format(VkFormat format)
{
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
}

static VkFormat bc_sample_alias(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    /* R66: the game samples RGBA8 textures through sRGB views (format 37 -> 56). */
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkResult open_texture(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                             PokopiaVkTexture *texture, bool allow_deferred)
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
    const bool block_compressed = bc_format(format);
    const VkFormat sample_alias_format = bc_sample_alias(format);
    const bool sampled_alias = sample_alias_format != VK_FORMAT_UNDEFINED;
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
    if ((!depth && !block_compressed && linear_format != VK_FORMAT_R8_UNORM && linear_format != VK_FORMAT_R16_SFLOAT &&
         linear_format != VK_FORMAT_R8G8B8A8_UNORM && linear_format != VK_FORMAT_B8G8R8A8_UNORM &&
         linear_format != VK_FORMAT_R16G16_SFLOAT && linear_format != VK_FORMAT_R16G16B16A16_SFLOAT &&
         linear_format != VK_FORMAT_R16G16B16A16_UNORM &&
         linear_format != VK_FORMAT_R32_SFLOAT && linear_format != VK_FORMAT_R32G32_SFLOAT &&
         linear_format != VK_FORMAT_R32G32B32A32_SFLOAT &&
         linear_format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) ||
        (block_compressed && (volume || (config->flags & (TEXTURE_DISPLAY | TEXTURE_LINEAR)))) ||
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

    if ((cube_array && !adapter->features.imageCubeArray) ||
        (block_compressed && !adapter->features.textureCompressionBC))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    /* NVN's IMAGE hint also appears on compressed and depth textures.
     * Block formats are sampled/transferred, with no storage or color
     * attachment usage. Vulkan backing remains optimal for guest linear data. */
    const bool storage = !depth && !block_compressed && (config->flags & TEXTURE_IMAGE) != 0;
    const bool alias = storage && linear_format != format;
    const VkFormat formats[] = {format, sampled_alias ? sample_alias_format : linear_format};
    const VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = formats,
    };
    const VkImageUsageFlags attachment = volume || block_compressed ? 0 :
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const VkImageUsageFlags view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | attachment;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = alias || sampled_alias ? &format_list : NULL,
        .flags = (alias || sampled_alias ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) |
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
    /* BC images only support sampling and transfers. Keep the guest's
     * tiled bytes canonical until the renderer actually samples the image. */
    if (allow_deferred && block_compressed) {
        opened.deferred = true;
        *texture = opened;
        return VK_SUCCESS;
    }
    result = open_image(adapter, &image, &opened.backing, VK_LEDGER_NV_TEXTURE);
    if (result != VK_SUCCESS)
        return result;
    /* The attachment view selects the first base-level layer. Sampled
     * views cover the whole image. Storage views cover one mip level. */
    const VkImageSubresourceRange base = {opened.aspects, 0, 1, 0, 1};
    const VkImageViewType sample_type = cube_array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY :
        cube ? VK_IMAGE_VIEW_TYPE_CUBE : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : view_type;
    opened.sample_view_type = sample_type;
    const bool separate_sample = !block_compressed && (stencil || layered || image.mipLevels > 1);
    VkImageSubresourceRange sampled = base;
    sampled.levelCount = image.mipLevels;
    sampled.layerCount = image.arrayLayers;
    result = open_view(create_view, device, opened.backing.image,
                       block_compressed ? sample_type : view_type, format,
                       block_compressed ? sampled : base,
                       separate_sample ? attachment : view_usage, &opened.view);
    if (result == VK_SUCCESS && separate_sample) {
        if (stencil)
            sampled.aspectMask = config->depth_stencil_mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        result = open_view(create_view, device, opened.backing.image, sample_type, format, sampled,
                           VK_IMAGE_USAGE_SAMPLED_BIT, &opened.sample_view);
    }
    if (result == VK_SUCCESS && sampled_alias) {
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

VkResult pokopia_vk_texture_open(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                 PokopiaVkTexture *texture)
{
    return open_texture(adapter, config, texture, false);
}

VkResult pokopia_vk_texture_open_lazy(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *config,
                                      PokopiaVkTexture *texture)
{
    return open_texture(adapter, config, texture, true);
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

bool (*pokopia_vk_flush_hook)(bool wait);
uint64_t pokopia_vk_flush_counts[POKOPIA_FLUSH_REASONS];
uint64_t pokopia_vk_batch_id = 1;
uint64_t pokopia_vk_batch_retired;
uint64_t pokopia_vk_guest_write_epoch = 1;
uint64_t pokopia_replay_job_epoch = 1;
void (*pokopia_vk_image_forget_hook)(VkImage image);
void (*pokopia_vk_before_batch_end_hook)(VkCommandBuffer command);
void (*pokopia_vk_texture_forget_hook)(void *texture);
volatile unsigned pokopia_debug_flags = POKOPIA_DEBUG_DEFAULTS; /* R94 */
void (*pokopia_vk_after_submit_hook)(void);
VkCommandBuffer (*pokopia_vk_batch_command_hook)(void);
uint64_t (*pokopia_now_ns)(void);

PokopiaVkFlushSite pokopia_vk_flush_sites[16];

VkResult pokopia_vk_submit_wait_at(PokopiaVkAdapter *adapter, VkCommandBuffer command, bool *retired,
                                   const char *site)
{
    /* R75: pending general draws go first */
    /* R78: submit the pending draws without waiting; the queue wait below
     * covers them (each call waited twice). */
    if (pokopia_vk_flush_hook && pokopia_vk_flush_hook(false)) {
        ++pokopia_vk_flush_counts[POKOPIA_FLUSH_SUBMIT_WAIT];
        for (size_t i = 0; i < 16; ++i)
            if (!pokopia_vk_flush_sites[i].site || pokopia_vk_flush_sites[i].site == site) {
                pokopia_vk_flush_sites[i].site = site;
                ++pokopia_vk_flush_sites[i].count;
                break;
            }
    }
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
        bc_format(texture->format) || adapter->render_serial == UINT64_MAX)
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
    const VkImageAspectFlags target_aspects = texture ? texture->aspects : 0;
    const bool supported_aspects = color ? target_aspects == VK_IMAGE_ASPECT_COLOR_BIT :
        target_aspects == VK_IMAGE_ASPECT_DEPTH_BIT || target_aspects == depth_stencil;
    if (!texture || !supported_aspects || !texture->backing.image || !texture->view || !texture->stages ||
        texture->array_layers != 1 || !aspects || (aspects & ~target_aspects) ||
        area.offset.x < 0 || area.offset.y < 0 || !area.extent.width || !area.extent.height ||
        (uint64_t)area.offset.x + area.extent.width > texture->extent.width ||
        (uint64_t)area.offset.y + area.extent.height > texture->extent.height ||
        (color && (bc_format(texture->format) || adapter->render_serial == UINT64_MAX)) ||
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
            .pDepthAttachment = color ? NULL : &attachment,
            .pStencilAttachment = (target_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &stencil : NULL,
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

PokopiaVkTexture *pokopia_vk_volume_placeholder(PokopiaVkAdapter *adapter, const NvnFrontTextureConfig *like,
                                                uint32_t rgba8)
{
    static struct {
        uint32_t rgba8, format;
        PokopiaVkTexture *texture;
    } cache[16];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i)
        if (cache[i].rgba8 == rgba8 && cache[i].format == like->format)
            return cache[i].texture;
    if (count == sizeof cache / sizeof cache[0])
        return NULL;
    NvnFrontTextureConfig config = *like;
    config.target = 2; /* 3D */
    config.width = config.height = config.depth = config.levels = 1;
    config.packaged_data = 0;
    PokopiaVkTexture *texture = calloc(1, sizeof *texture);
    if (!texture)
        return NULL;
    VkResult result = pokopia_vk_texture_open(adapter, &config, texture);
    if (result == VK_SUCCESS) {
        const float rgba[4] = {(rgba8 & 0xff) / 255.0f, ((rgba8 >> 8) & 0xff) / 255.0f,
                               ((rgba8 >> 16) & 0xff) / 255.0f, (rgba8 >> 24) / 255.0f};
        result = pokopia_vk_clear_color(adapter, texture, rgba);
    }
    if (adapter->log) {
        fprintf(adapter->log, "NVN volume placeholder rgba=%08x format=%u result=%d\n", rgba8, like->format, result);
        fflush(adapter->log);
    }
    if (result != VK_SUCCESS) {
        free(texture);
        return NULL;
    }
    cache[count++] = (__typeof__(cache[0])){rgba8, like->format, texture};
    return texture;
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

/* nvnCommandBufferCopyTextureToTexture. Layout tracking covers the whole
 * image: both images move to transfer layouts for the copy and return to the
 * layout they had (a destination that was undefined stays TRANSFER_DST). */
VkResult pokopia_vk_copy_texture(PokopiaVkAdapter *adapter, PokopiaVkTexture *source, const PokopiaVkCopyRegion *from,
                                 PokopiaVkTexture *destination, const PokopiaVkCopyRegion *to, bool linear)
{
    if (!adapter || !source || !destination || !from || !to || !source->backing.image ||
        !destination->backing.image || source->deferred || destination->deferred ||
        source->aspects != destination->aspects || !from->extent.width || !from->extent.height ||
        !from->extent.depth || !to->extent.width || !to->extent.height || !to->extent.depth ||
        from->layers != to->layers || !from->layers || from->volume != to->volume)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const bool same_shape = from->extent.width == to->extent.width && from->extent.height == to->extent.height &&
                            from->extent.depth == to->extent.depth;
    const bool copy = same_shape && source->format == destination->format;
    const bool depth_stencil = source->aspects != VK_IMAGE_ASPECT_COLOR_BIT;
    if (!copy) {
        /* Blits cannot change block-compressed or depth/stencil formats. */
        if (bc_format(source->format) || bc_format(destination->format) ||
            (depth_stencil && source->format != destination->format))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        VkFormatProperties src_props = {0}, dst_props = {0};
        adapter->format_properties(adapter->physical, source->format, &src_props);
        adapter->format_properties(adapter->physical, destination->format, &dst_props);
        if (!(src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(dst_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
            (linear && !depth_stencil &&
             !(src_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (depth_stencil)
            linear = false;
    }

    const VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImage copy_image = (PFN_vkCmdCopyImage)proc(device, "vkCmdCopyImage");
    PFN_vkCmdBlitImage blit_image = (PFN_vkCmdBlitImage)proc(device, "vkCmdBlitImage");
    if (!begin || !end || !barrier || !copy_image || !blit_image)
        return VK_ERROR_INITIALIZATION_FAILED;

    const bool same = source == destination;
    const VkImageLayout src_layout = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout dst_layout = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageSubresourceRange src_range = {source->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    const VkImageSubresourceRange dst_range = {destination->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    const VkImageLayout src_restore = source->layout;
    const VkImageLayout dst_restore = destination->layout == VK_IMAGE_LAYOUT_UNDEFINED ? dst_layout : destination->layout;
    VkImageMemoryBarrier before[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = source->access,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | (same ? VK_ACCESS_TRANSFER_WRITE_BIT : 0),
         .oldLayout = source->layout, .newLayout = src_layout,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = source->backing.image, .subresourceRange = src_range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = destination->access,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = destination->layout, .newLayout = dst_layout,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = destination->backing.image, .subresourceRange = dst_range},
    };
    VkImageMemoryBarrier after[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = src_layout, .newLayout = same ? dst_restore : src_restore,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = source->backing.image, .subresourceRange = src_range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = dst_layout, .newLayout = dst_restore,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = destination->backing.image, .subresourceRange = dst_range},
    };
    /* An undefined source has no content worth keeping across the copy. */
    const bool restore_source = !same && src_restore != VK_IMAGE_LAYOUT_UNDEFINED && src_restore != src_layout;
    const bool restore_destination = dst_restore != dst_layout;
    const VkPipelineStageFlags wait_stages = (source->stages ? source->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) |
                                             (destination->stages ? destination->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    const VkImageSubresourceLayers src_layers = {source->aspects, from->level, from->volume ? 0 : from->layer,
        from->volume ? 1 : from->layers};
    const VkImageSubresourceLayers dst_layers = {destination->aspects, to->level, to->volume ? 0 : to->layer,
        to->volume ? 1 : to->layers};

    /* R78: recorded into the draw batch when one is open (each copy was a submit
     * and wait, ~5 ms with batched work queued ahead of it). */
    const VkCommandBuffer batch = pokopia_vk_batch_command_hook ? pokopia_vk_batch_command_hook() : VK_NULL_HANDLE;
    VkCommandBuffer command = batch;
    VkResult result = VK_SUCCESS;
    if (!batch) {
        result = pokopia_vk_command_buffer_open(adapter, &command);
        if (result != VK_SUCCESS)
            return result;
        const VkCommandBufferBeginInfo info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        result = begin(command, &info);
    }
    if (result == VK_SUCCESS) {
        barrier(command, wait_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, same ? 1 : 2,
                same ? &before[1] : before);
        if (copy) {
            const VkImageCopy region = {
                .srcSubresource = src_layers, .srcOffset = from->offset,
                .dstSubresource = dst_layers, .dstOffset = to->offset, .extent = from->extent,
            };
            copy_image(command, source->backing.image, src_layout, destination->backing.image, dst_layout, 1, &region);
        } else {
            const VkImageBlit region = {
                .srcSubresource = src_layers,
                .srcOffsets = {from->offset, {from->offset.x + (int32_t)from->extent.width,
                    from->offset.y + (int32_t)from->extent.height, from->offset.z + (int32_t)from->extent.depth}},
                .dstSubresource = dst_layers,
                .dstOffsets = {to->offset, {to->offset.x + (int32_t)to->extent.width,
                    to->offset.y + (int32_t)to->extent.height, to->offset.z + (int32_t)to->extent.depth}},
            };
            blit_image(command, source->backing.image, src_layout, destination->backing.image, dst_layout, 1, &region,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        VkImageMemoryBarrier restore[2];
        uint32_t restores = 0;
        if (restore_source)
            restore[restores++] = after[0];
        if (restore_destination)
            restore[restores++] = after[1];
        if (restores)
            barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                    restores, restore);
        if (!batch)
            result = end(command);
    }
    bool retired = true;
    if (result == VK_SUCCESS && !batch)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (retired && !batch)
        pokopia_vk_command_buffer_close(adapter, command);
    if (result == VK_SUCCESS) {
        if (!same) {
            source->layout = restore_source ? src_restore : src_layout;
            source->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            source->access = 0;
        }
        destination->layout = dst_restore;
        destination->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination->access = VK_ACCESS_TRANSFER_WRITE_BIT;
        /* GPU content now; a later sample must not re-upload guest bytes. */
        destination->render_serial = ++adapter->render_serial;
    }
    return result;
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
    if (adapter->device && adapter->wait_idle && adapter->wait_idle(adapter->device) == VK_SUCCESS)
        pokopia_vk_texture_close(adapter, &adapter->feedback_snapshot);
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
    adapter->log = log;
#if !defined(_WIN32) && !defined(__PROSPERO__)
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
#if defined(__PROSPERO__)
    const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    /* R78: guest memory imported for vertex and index reads (live_draw_generic.inc). */
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
#elif !defined(_WIN32)
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
#ifdef POKOPIA_VK_MEMORY_TRACE
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        const VkMemoryType *type = &adapter->memory.memoryTypes[i];
        fprintf(log, "Vulkan memory type %u heap=%u flags=%#x\n", i, type->heapIndex, type->propertyFlags);
    }
#endif
    fflush(log);
    return true;

fail:
    fprintf(log, "FAIL %s (%d)\n", failed, result);
    fflush(log);
    pokopia_vk_close(adapter);
    return false;
}

VkImageView pokopia_vk_texture_swizzle_view(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                            VkFormat view_format, const uint8_t swizzle[4])
{
    return pokopia_vk_texture_typed_view(adapter, texture, view_format, swizzle, texture ? texture->sample_view_type :
                                                                                           VK_IMAGE_VIEW_TYPE_2D);
}

/* R73: also a view of another type, such as a one-layer 2D array view of a 2D
 * texture bound where the shader declares a 2D array. */
VkImageView pokopia_vk_texture_typed_view(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                          VkFormat view_format, const uint8_t swizzle[4], VkImageViewType view_type)
{
    enum { SLOTS = sizeof texture->swizzle_views / sizeof texture->swizzle_views[0] };
    if (!adapter || !texture || !texture->backing.image || texture->deferred ||
        texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT || swizzle[0] > 5 || swizzle[1] > 5 ||
        swizzle[2] > 5 || swizzle[3] > 5 || (unsigned)view_type > 6)
        return VK_NULL_HANDLE;
    const VkFormat format = view_format == VK_FORMAT_UNDEFINED ? texture->format : view_format;
    if (format != texture->format && format != texture->sample_alias_format)
        return VK_NULL_HANDLE;
    const uint64_t key = ((uint64_t)(uint32_t)format << 32) | ((uint64_t)swizzle[0] << 24) |
                         ((uint64_t)swizzle[1] << 16) | ((uint64_t)swizzle[2] << 8) | swizzle[3] | (1ull << 31) |
                         ((uint64_t)view_type << 28);
    size_t slot = 0;
    for (; slot < SLOTS && texture->swizzle_views[slot].view; ++slot)
        if (texture->swizzle_views[slot].key == key)
            return texture->swizzle_views[slot].view;
    if (slot == SLOTS)
        return VK_NULL_HANDLE;
    PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)
        adapter->get_device_proc(adapter->device, "vkCreateImageView");
    if (!create_view)
        return VK_NULL_HANDLE;
    /* NVN TextureSwizzle ZERO..A is VkComponentSwizzle ZERO..A minus one. */
    const VkImageViewUsageCreateInfo usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage,
        .image = texture->backing.image, .viewType = view_type, .format = format,
        .components = {(VkComponentSwizzle)(swizzle[0] + 1), (VkComponentSwizzle)(swizzle[1] + 1),
                       (VkComponentSwizzle)(swizzle[2] + 1), (VkComponentSwizzle)(swizzle[3] + 1)},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
    };
    VkImageView view = VK_NULL_HANDLE;
    if (create_view(adapter->device, &info, NULL, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    texture->swizzle_views[slot].key = key;
    texture->swizzle_views[slot].view = view;
    if (adapter->log) {
        fprintf(adapter->log, "NVN swizzled view %ux%u format=%d swizzle=%u%u%u%u\n", texture->extent.width,
                texture->extent.height, (int)format, swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
        fflush(adapter->log);
    }
    return view;
}

VkResult pokopia_vk_texture_snapshot(PokopiaVkAdapter *adapter, PokopiaVkTexture *source,
                                     const NvnFrontTextureConfig *config, PokopiaVkTexture **snapshot)
{
    return pokopia_vk_texture_snapshot_into(adapter, source, config, snapshot, NULL);
}

VkResult pokopia_vk_texture_snapshot_into(PokopiaVkAdapter *adapter, PokopiaVkTexture *source,
                                          const NvnFrontTextureConfig *config, PokopiaVkTexture **snapshot,
                                          VkCommandBuffer *batch)
{
    enum { MAX_LEVELS = 16, TARGET_3D = 2 };
    if (!adapter || !source || !config || !snapshot || !source->backing.image || source->deferred ||
        !config->levels || config->levels > MAX_LEVELS)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!source->feedback_copy) {
        PokopiaVkTexture *copy = calloc(1, sizeof *copy);
        if (!copy)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        const VkResult opened = pokopia_vk_texture_open(adapter, config, copy);
        if (opened != VK_SUCCESS || copy->format != source->format || copy->aspects != source->aspects ||
            copy->array_layers != source->array_layers || copy->extent.width != source->extent.width ||
            copy->extent.height != source->extent.height) {
            if (opened == VK_SUCCESS)
                pokopia_vk_texture_close(adapter, copy);
            free(copy);
            return opened != VK_SUCCESS ? opened : VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        source->feedback_copy = copy;
        if (adapter->log) {
            fprintf(adapter->log, "NVN feedback copy %ux%u format=%d levels=%u layers=%u\n", source->extent.width,
                    source->extent.height, (int)source->format, config->levels, source->array_layers);
            fflush(adapter->log);
        }
    }
    PokopiaVkTexture *destination = source->feedback_copy;
    *snapshot = destination;
    /* Nothing written yet: the draw path gives an undefined texture a neutral value. */
    if (source->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return VK_SUCCESS;

    const VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImage copy_image = (PFN_vkCmdCopyImage)proc(device, "vkCmdCopyImage");
    if (!begin || !end || !barrier || !copy_image)
        return VK_ERROR_INITIALIZATION_FAILED;

    const bool volume = config->target == TARGET_3D;
    VkImageCopy regions[MAX_LEVELS];
    for (uint32_t level = 0; level < config->levels; ++level) {
        const uint32_t width = source->extent.width >> level, height = source->extent.height >> level;
        const uint32_t depth = volume ? config->depth >> level : 1;
        const VkImageSubresourceLayers layers = {source->aspects, level, 0, volume ? 1 : source->array_layers};
        regions[level] = (VkImageCopy){
            .srcSubresource = layers, .dstSubresource = layers,
            .extent = {width ? width : 1, height ? height : 1, depth ? depth : 1},
        };
    }
    const VkImageSubresourceRange range = {source->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    const VkImageMemoryBarrier before[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = source->access,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = source->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = source->backing.image, .subresourceRange = range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = destination->access,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = destination->backing.image, .subresourceRange = range},
    };
    const VkImageMemoryBarrier restore = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = source->access,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = source->layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = source->backing.image, .subresourceRange = range,
    };
    const VkPipelineStageFlags wait_stages = (source->stages ? source->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) |
                                             (destination->stages ? destination->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    /* Read only now: opening the copy above may have flushed the batch. */
    const VkCommandBuffer into = batch ? *batch : VK_NULL_HANDLE;
    if (into) {
        /* R75: recorded into the open draw batch, in order with its draws. */
        barrier(into, wait_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, before);
        copy_image(into, source->backing.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->backing.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, config->levels, regions);
        if (source->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            barrier(into, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                    1, &restore);
        source->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destination->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination->access = VK_ACCESS_TRANSFER_WRITE_BIT;
        destination->render_serial = source->render_serial;
        return VK_SUCCESS;
    }
    VkCommandBuffer command;
    VkResult result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = begin(command, &info);
    if (result == VK_SUCCESS) {
        barrier(command, wait_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, before);
        copy_image(command, source->backing.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->backing.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, config->levels, regions);
        if (source->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                    1, &restore);
        result = end(command);
    }
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (retired)
        pokopia_vk_command_buffer_close(adapter, command);
    if (result == VK_SUCCESS) {
        source->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destination->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination->access = VK_ACCESS_TRANSFER_WRITE_BIT;
        destination->render_serial = source->render_serial;
    }
    return result;
}


/* R73: debug readback of one mip level and layer of a texture to a raw file:
 * "PDMP", u32 width, height, VkFormat, texel bytes, then the rows. Depth
 * formats dump their depth aspect. Leaves the texture in TRANSFER_SRC. */
static uint32_t dump_texel_bytes(VkFormat format, VkImageAspectFlags *aspect)
{
    *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch ((int)format) {
    case 9: case 13: return 1;
    case 70: case 76: return 2;
    case 16: case 37: case 43: case 44: case 50: case 64: case 83: case 100: case 122: case 98: return 4;
    case 91: case 97: case 103: return 8;
    case 109: return 16;
    case 124: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return 2;
    case 126: case 129: case 130: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return 4;
    /* R82: block-compressed formats dump raw 4x4 blocks (bytes per block). */
    case 131: case 132: case 133: case 134: case 139: case 140: return 8;
    case 135: case 136: case 137: case 138: case 141: case 142: case 143: case 144: case 145: case 146: return 16;
    default: return 0;
    }
}

VkResult pokopia_vk_texture_dump(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, uint32_t level,
                                 uint32_t layer, const char *path)
{
    VkImageAspectFlags aspect;
    const uint32_t texel = texture ? dump_texel_bytes(texture->format, &aspect) : 0;
    if (!adapter || !texture || !texture->backing.image || texture->deferred || !texel)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t width = texture->extent.width >> level, height = texture->extent.height >> level;
    width = width ? width : 1;
    height = height ? height : 1;
    const VkDevice device = adapter->device;
    const PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
#define LOAD(name) PFN_vk##name name = (PFN_vk##name)proc(device, "vk" #name); if (!name) return VK_ERROR_INITIALIZATION_FAILED;
    LOAD(CreateBuffer) LOAD(DestroyBuffer) LOAD(GetBufferMemoryRequirements) LOAD(AllocateMemory) LOAD(FreeMemory)
    LOAD(BindBufferMemory) LOAD(MapMemory) LOAD(UnmapMemory) LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer)
    LOAD(CmdPipelineBarrier) LOAD(CmdCopyImageToBuffer)
#undef LOAD
    const bool blocks = texture->format >= 131 && texture->format <= 146;
    const VkDeviceSize bytes = blocks ? (VkDeviceSize)((width + 3) / 4) * ((height + 3) / 4) * texel
                                      : (VkDeviceSize)width * height * texel;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = CreateBuffer(device, &buffer_info, NULL, &buffer);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements needs;
    GetBufferMemoryRequirements(device, buffer, &needs);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((needs.memoryTypeBits & (1u << i)) &&
            (adapter->memory.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = i;
    const VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size, .memoryTypeIndex = type,
    };
    result = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : AllocateMemory(device, &alloc, NULL, &memory);
    if (result == VK_SUCCESS)
        result = BindBufferMemory(device, buffer, memory, 0);
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (result == VK_SUCCESS)
        result = pokopia_vk_command_buffer_open(adapter, &command);
    bool retired = true;
    if (result == VK_SUCCESS) {
        const VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        result = BeginCommandBuffer(command, &begin);
        if (result == VK_SUCCESS) {
            const VkImageMemoryBarrier to_source = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = texture->access, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = texture->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = texture->backing.image,
                .subresourceRange = {texture->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
            };
            CmdPipelineBarrier(command, texture->stages ? texture->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_source);
            const VkBufferImageCopy region = {
                .imageSubresource = {aspect, level, layer, 1}, .imageExtent = {width, height, 1},
            };
            CmdCopyImageToBuffer(command, texture->backing.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                                 &region);
            result = EndCommandBuffer(command);
        }
        if (result == VK_SUCCESS)
            result = pokopia_vk_submit_wait(adapter, command, &retired);
        if (retired)
            pokopia_vk_command_buffer_close(adapter, command);
    }
    if (result == VK_SUCCESS) {
        texture->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        texture->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        texture->access = VK_ACCESS_TRANSFER_READ_BIT;
        void *mapped = NULL;
        result = MapMemory(device, memory, 0, bytes, 0, &mapped);
        if (result == VK_SUCCESS) {
            char temp[512];
            snprintf(temp, sizeof temp, "%s.tmp", path);
            FILE *file = fopen(temp, "wb");
            if (file) {
                const uint32_t header[4] = {width, height, (uint32_t)texture->format, texel};
                fwrite("PDMP", 1, 4, file);
                fwrite(header, sizeof header, 1, file);
                fwrite(mapped, 1, (size_t)bytes, file);
                fclose(file);
                rename(temp, path);
            } else {
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
            UnmapMemory(device, memory);
        }
    }
    if (retired) {
        if (memory)
            FreeMemory(device, memory, NULL);
        DestroyBuffer(device, buffer, NULL);
    }
    return result;
}
