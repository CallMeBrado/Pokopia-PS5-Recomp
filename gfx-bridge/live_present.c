/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_present.h"
#include "vk_ledger.h"
#include "fsr_shaders.h"
#include "ps_banner.h"
#include "port_settings.h"

#include <math.h>
#include <string.h>

static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkImageSubresourceLayers color_layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
enum { OVERLAY_INSET = 24 };

static bool same_extent(VkExtent2D a, VkExtent2D b)
{
    return a.width == b.width && a.height == b.height;
}

static bool valid_extent(VkExtent2D extent)
{
    return extent.width && extent.height && extent.width <= INT32_MAX && extent.height <= INT32_MAX;
}

static bool valid_source_layout(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_GENERAL:
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        return true;
    default:
        return false;
    }
}

/* The sRGB view of a UNORM scanout format, or UNDEFINED. */
static VkFormat srgb_of_scanout(VkFormat output)
{
    switch (output) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* An sRGB frame into a UNORM scanout, in either channel order (the PS5
 * VideoOut is BGRA, the game renders RGBA). The frame is blitted into a
 * conversion image in the scanout's sRGB format, which filters in linear light
 * and swaps channels, then copied byte-for-byte into the UNORM scanout. */
static bool srgb_scanout_pair(VkFormat source, VkFormat output)
{
    return (source == VK_FORMAT_R8G8B8A8_SRGB || source == VK_FORMAT_B8G8R8A8_SRGB) &&
           srgb_of_scanout(output) != VK_FORMAT_UNDEFINED;
}

/* RGBA frame into a BGRA scanout (PS5 VideoOut) or the reverse: vkCmdBlitImage
 * converts channel order, a plain image copy does not, so these always blit. */
static bool channel_swap_pair(VkFormat source, VkFormat output)
{
    return (source == VK_FORMAT_R8G8B8A8_UNORM && output == VK_FORMAT_B8G8R8A8_UNORM) ||
           (source == VK_FORMAT_B8G8R8A8_UNORM && output == VK_FORMAT_R8G8B8A8_UNORM) ||
           (source == VK_FORMAT_R8G8B8A8_SRGB && output == VK_FORMAT_B8G8R8A8_SRGB) ||
           (source == VK_FORMAT_B8G8R8A8_SRGB && output == VK_FORMAT_R8G8B8A8_SRGB);
}

static bool supported_scanout(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

VkResult pokopia_live_present_init(PokopiaLivePresent *live, VkPhysicalDevice physical,
    PokopiaPresent *present, VkCommandBuffer command, PokopiaGameCapture *capture)
{
    if (!live || live->present || !physical || !present || !present->device || !present->queue ||
        !present->swapchain || !present->targets || !present->image_count || !present->fence ||
        !valid_extent(present->extent) || !command || (capture && !capture->mailbox.directory))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!supported_scanout(present->format))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *live = (PokopiaLivePresent){.physical = physical, .present = present, .capture = capture,
        .command = command};
    return VK_SUCCESS;
}

static void release_conversion(PokopiaLivePresent *live)
{
    if (live->conversion)
        vkDestroyImage(live->present->device, live->conversion, NULL);
    if (live->conversion_memory)
        pokopia_vk_ledger_free(vkFreeMemory, live->present->device, live->conversion_memory);
    live->conversion = VK_NULL_HANDLE;
    live->conversion_memory = VK_NULL_HANDLE;
}

static void release_fps(PokopiaLivePresent *live)
{
    VkDevice device = live->present->device;
    if (live->fps_pixels)
        vkUnmapMemory(device, live->fps_memory);
    if (live->fps_buffer)
        vkDestroyBuffer(device, live->fps_buffer, NULL);
    if (live->fps_memory)
        pokopia_vk_ledger_free(vkFreeMemory, device, live->fps_memory);
    live->fps_pixels = NULL;
    live->fps_buffer = VK_NULL_HANDLE;
    live->fps_memory = VK_NULL_HANDLE;
    live->fps_coherent = false;
    live->fps_ready = false;
}

static void release_overlay(PokopiaLivePresent *live)
{
    VkDevice device = live->present->device;
    if (live->overlay_pixels)
        vkUnmapMemory(device, live->overlay_memory);
    if (live->overlay_buffer)
        vkDestroyBuffer(device, live->overlay_buffer, NULL);
    if (live->overlay_memory)
        pokopia_vk_ledger_free(vkFreeMemory, device, live->overlay_memory);
    live->overlay_pixels = NULL;
    live->overlay_buffer = VK_NULL_HANDLE;
    live->overlay_memory = VK_NULL_HANDLE;
    live->overlay_coherent = false;
    live->overlay_ready = false;
}

void pokopia_live_present_close(PokopiaLivePresent *live)
{
    if (!live || !live->present)
        return;
    release_conversion(live);
    release_overlay(live);
    release_fps(live);
    *live = (PokopiaLivePresent){0};
}

static VkResult ensure_conversion(PokopiaLivePresent *live, VkFormat format)
{
    PokopiaPresent *present = live->present;
    if (live->conversion && live->conversion_format == format &&
        same_extent(live->conversion_extent, present->extent))
        return VK_SUCCESS;

    release_conversion(live);
    const VkImageCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {present->extent.width, present->extent.height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    VkResult result = vkCreateImage(present->device, &create, NULL, &live->conversion);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetImageMemoryRequirements(present->device, live->conversion, &requirements);
    vkGetPhysicalDeviceMemoryProperties(live->physical, &memory);
    uint32_t index = 0;
    for (; index < memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (UINT32_C(1) << index)) &&
            (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    }
    if (index == memory.memoryTypeCount) {
        release_conversion(live);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = index,
    };
    result = pokopia_vk_ledger_allocate(vkAllocateMemory, present->device, &allocate, &live->conversion_memory,
                                       VK_LEDGER_CONVERSION);
    if (result == VK_SUCCESS)
        result = vkBindImageMemory(present->device, live->conversion, live->conversion_memory, 0);
    if (result == VK_SUCCESS) {
        live->conversion_extent = present->extent;
        live->conversion_format = format;
    } else {
        release_conversion(live);
    }
    return result;
}

static void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags from_stage, VkAccessFlags from_access,
    VkPipelineStageFlags to_stage, VkAccessFlags to_access)
{
    const VkImageMemoryBarrier transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image, .oldLayout = from,
        .newLayout = to, .srcAccessMask = from_access, .dstAccessMask = to_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(command, from_stage, to_stage, 0, 0, NULL, 0, NULL, 1, &transition);
}

static VkResult open_panel(PokopiaLivePresent *live, VkDeviceSize size, VkBuffer *buffer,
    VkDeviceMemory *memory_out, void **pixels, bool *coherent)
{
    VkDevice device = live->present->device;
    const VkBufferCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, /* R96: the banner reads one */
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(device, &create, NULL, buffer);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(live->physical, &memory);
    uint32_t type = memory.memoryTypeCount;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) ||
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            type = i;
            *coherent = true;
            break;
        }
        if (type == memory.memoryTypeCount)
            type = i;
    }
    if (type == memory.memoryTypeCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = pokopia_vk_ledger_allocate(vkAllocateMemory, device, &allocation,
        memory_out, VK_LEDGER_UPLOAD);
    if (result == VK_SUCCESS)
        result = vkBindBufferMemory(device, *buffer, *memory_out, 0);
    if (result == VK_SUCCESS)
        result = vkMapMemory(device, *memory_out, 0, VK_WHOLE_SIZE, 0, pixels);
    return result;
}

static VkResult open_overlay(PokopiaLivePresent *live)
{
    return open_panel(live, POKOPIA_OVERLAY_WIDTH * POKOPIA_OVERLAY_HEIGHT * 4, &live->overlay_buffer,
        &live->overlay_memory, &live->overlay_pixels, &live->overlay_coherent);
}

VkResult pokopia_live_present_keyboard_pixels(PokopiaLivePresent *live, uint32_t top, uint32_t bottom,
    uint8_t **pixels, uint32_t *width, bool *bgra)
{
    if (!live || !live->present || !pixels || !width || !bgra || !top || !bottom ||
        top + bottom > live->present->extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t w = live->present->extent.width;
    if (!live->keyboard_pixels || live->keyboard_width != w || live->keyboard_top != top ||
        live->keyboard_bottom != bottom) {
        if (live->keyboard_pixels)
            return VK_ERROR_INITIALIZATION_FAILED; /* sized once */
        const VkResult result = open_panel(live, (VkDeviceSize)w * (top + bottom) * 4, &live->keyboard_buffer,
            &live->keyboard_memory, &live->keyboard_pixels, &live->keyboard_coherent);
        if (result != VK_SUCCESS)
            return result;
        live->keyboard_width = w;
        live->keyboard_top = top;
        live->keyboard_bottom = bottom;
    }
    *pixels = live->keyboard_pixels;
    *width = w;
    *bgra = live->present->format == VK_FORMAT_B8G8R8A8_UNORM || live->present->format == VK_FORMAT_B8G8R8A8_SRGB;
    return VK_SUCCESS;
}

VkResult pokopia_live_present_keyboard_flush(PokopiaLivePresent *live)
{
    if (!live || !live->keyboard_pixels || live->keyboard_coherent)
        return VK_SUCCESS;
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = live->keyboard_memory, .size = VK_WHOLE_SIZE,
    };
    return vkFlushMappedMemoryRanges(live->present->device, 1, &range);
}

VkResult pokopia_live_present_menu_pixels(PokopiaLivePresent *live, uint32_t width, uint32_t height,
    uint8_t **pixels, bool *bgra)
{
    if (!live || !live->present || !pixels || !bgra || !width || !height || width > live->present->extent.width ||
        height > live->present->extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!live->menu_pixels) {
        const VkResult result = open_panel(live, (VkDeviceSize)width * height * 4, &live->menu_buffer,
            &live->menu_memory, &live->menu_pixels, &live->menu_coherent);
        if (result != VK_SUCCESS)
            return result;
        live->menu_width = width;
        live->menu_height = height;
    } else if (live->menu_width != width || live->menu_height != height) {
        return VK_ERROR_INITIALIZATION_FAILED; /* sized once */
    }
    *pixels = live->menu_pixels;
    *bgra = live->present->format == VK_FORMAT_B8G8R8A8_UNORM || live->present->format == VK_FORMAT_B8G8R8A8_SRGB;
    return VK_SUCCESS;
}

VkResult pokopia_live_present_menu_flush(PokopiaLivePresent *live)
{
    if (!live || !live->menu_pixels || live->menu_coherent)
        return VK_SUCCESS;
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = live->menu_memory, .size = VK_WHOLE_SIZE,
    };
    return vkFlushMappedMemoryRanges(live->present->device, 1, &range);
}

VkResult pokopia_live_present_fps_update(PokopiaLivePresent *live, const char *text)
{
    if (!live || !live->present || !text || live->present->frame_open ||
        live->present->fence_pending || live->present->faulted)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (live->present->extent.width < 2 * OVERLAY_INSET + POKOPIA_FPS_WIDTH ||
        live->present->extent.height < OVERLAY_INSET + POKOPIA_FPS_HEIGHT)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = live->fps_pixels ? VK_SUCCESS :
        open_panel(live, POKOPIA_FPS_WIDTH * POKOPIA_FPS_HEIGHT * 4, &live->fps_buffer, &live->fps_memory,
            &live->fps_pixels, &live->fps_coherent);
    if (result == VK_SUCCESS) {
        const char *const lines[1] = {text};
        pokopia_overlay_panel(live->fps_pixels, POKOPIA_FPS_WIDTH, POKOPIA_FPS_HEIGHT, lines, 1);
        if (!live->fps_coherent) {
            const VkMappedMemoryRange range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = live->fps_memory,
                .size = VK_WHOLE_SIZE,
            };
            result = vkFlushMappedMemoryRanges(live->present->device, 1, &range);
        }
    }
    if (result != VK_SUCCESS)
        release_fps(live);
    live->fps_ready = result == VK_SUCCESS;
    return result;
}

VkResult pokopia_live_present_overlay_update(PokopiaLivePresent *live,
    const char *const lines[POKOPIA_OVERLAY_LINES])
{
    if (!live || !live->present || !lines || live->present->frame_open ||
        live->present->fence_pending || live->present->faulted)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (live->present->extent.width < OVERLAY_INSET + POKOPIA_OVERLAY_WIDTH ||
        live->present->extent.height < OVERLAY_INSET + POKOPIA_OVERLAY_HEIGHT)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    VkResult result = live->overlay_pixels ? VK_SUCCESS : open_overlay(live);
    if (result == VK_SUCCESS) {
        pokopia_overlay_pixels(live->overlay_pixels, lines);
        if (!live->overlay_coherent) {
            const VkMappedMemoryRange range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = live->overlay_memory,
                .size = VK_WHOLE_SIZE,
            };
            result = vkFlushMappedMemoryRanges(live->present->device, 1, &range);
        }
    }
    if (result != VK_SUCCESS)
        release_overlay(live);
    live->overlay_ready = result == VK_SUCCESS;
    return result;
}

static void record_panel(VkCommandBuffer command, VkImage image, VkBuffer buffer, int32_t x, int32_t y,
    uint32_t width, uint32_t height)
{
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkBufferImageCopy region = {
        .imageSubresource = color_layer, .imageOffset = {x, y, 0},
        .imageExtent = {width, height, 1},
    };
    barrier(command, image, destination, destination, transfer, VK_ACCESS_TRANSFER_WRITE_BIT,
        transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(command, buffer, image, destination, 1, &region);
}

/* R86: a panel taken from part of a larger buffer. */
static void record_panel_at(VkCommandBuffer command, VkImage image, VkBuffer buffer, VkDeviceSize offset,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkBufferImageCopy region = {
        .bufferOffset = offset, .imageSubresource = color_layer, .imageOffset = {x, y, 0},
        .imageExtent = {width, height, 1},
    };
    barrier(command, image, destination, destination, transfer, VK_ACCESS_TRANSFER_WRITE_BIT,
        transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(command, buffer, image, destination, 1, &region);
}

static void record_overlay(VkCommandBuffer command, VkImage image, VkBuffer buffer)
{
    record_panel(command, image, buffer, OVERLAY_INSET, OVERLAY_INSET, POKOPIA_OVERLAY_WIDTH, POKOPIA_OVERLAY_HEIGHT);
}

static VkResult begin_present(PokopiaLivePresent *live, uint64_t timeout_ns, PokopiaPresentTarget *target)
{
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (live->present->async) {
        /* R81: acquire first; the frame's command buffer is free once its fence is. */
        VkResult result = pokopia_present_acquire(live->present, timeout_ns, target);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return result;
        const VkCommandBuffer command = pokopia_present_command(live->present);
        live->active_command = command;
        VkResult begun = command ? vkResetCommandBuffer(command, 0) : VK_ERROR_INITIALIZATION_FAILED;
        if (begun == VK_SUCCESS)
            begun = vkBeginCommandBuffer(command, &begin);
        if (begun != VK_SUCCESS) {
            live->present->faulted = true;
            return begun;
        }
        return result;
    }
    const VkCommandBuffer command = live->command;
    live->active_command = command;
    VkResult result = vkResetCommandBuffer(command, 0);
    if (result == VK_SUCCESS)
        result = vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        return result;

    result = pokopia_present_acquire(live->present, timeout_ns, target);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        vkResetCommandBuffer(command, 0);
    return result;
}

VkResult pokopia_live_present_show_overlay(PokopiaLivePresent *live, uint64_t timeout_ns)
{
    if (!live || !live->overlay_ready || live->present->frame_open || live->present->fence_pending ||
        live->present->faulted || (live->capture && live->capture->pending_render_serial))
        return VK_NOT_READY;

    PokopiaPresentTarget target;
    VkResult result = begin_present(live, timeout_ns, &target);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return result;

    const VkCommandBuffer command = live->active_command;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    barrier(command, target.image, VK_IMAGE_LAYOUT_UNDEFINED, destination,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(command, target.image, destination, &black, 1, &color_range);
    record_overlay(command, target.image, live->overlay_buffer);
    barrier(command, target.image, destination, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, transfer,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    result = vkEndCommandBuffer(command);
    if (result == VK_SUCCESS)
        return pokopia_present_frame(live->present, command, timeout_ns);
    live->present->faulted = true;
    return result;
}

static void copy(VkCommandBuffer command, VkImage source, VkOffset2D offset, VkImage destination,
    VkExtent2D extent)
{
    const VkImageCopy region = {
        .srcSubresource = color_layer, .srcOffset = {offset.x, offset.y, 0},
        .dstSubresource = color_layer, .extent = {extent.width, extent.height, 1},
    };
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void blit(VkCommandBuffer command, const PokopiaLiveFrame *frame, VkRect2D crop,
    VkImage destination, VkExtent2D extent, VkFilter filter)
{
    const int32_t x0 = crop.offset.x, y0 = crop.offset.y;
    const int32_t x1 = x0 + (int32_t)crop.extent.width, y1 = y0 + (int32_t)crop.extent.height;
    const VkImageBlit region = {
        .srcSubresource = color_layer, .dstSubresource = color_layer,
        .srcOffsets = {{x0, frame->flip_y ? y1 : y0, 0}, {x1, frame->flip_y ? y0 : y1, 1}},
        .dstOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
    };
    vkCmdBlitImage(command, frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
}

static VkResult check_blit(PokopiaLivePresent *live, VkFormat source, VkFormat output, bool scale)
{
    VkFormatProperties src, dst;
    vkGetPhysicalDeviceFormatProperties(live->physical, source, &src);
    vkGetPhysicalDeviceFormatProperties(live->physical, output, &dst);
    const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        (scale ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0u);
    if ((src.optimalTilingFeatures & features) != features ||
        !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return VK_SUCCESS;
}

/* ---------------------------------------------------------------- R84: FSR 1 */

static bool fsr_srgb(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static uint32_t fsr_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static VkResult fsr_image(PokopiaLivePresent *live, VkFormat format, VkImageUsageFlags usage, bool mutable_srgb,
                          VkImage *image, VkDeviceMemory *memory)
{
    PokopiaPresent *present = live->present;
    const VkImageCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = mutable_srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {present->extent.width, present->extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateImage(present->device, &create, NULL, image);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties properties;
    vkGetImageMemoryRequirements(present->device, *image, &requirements);
    vkGetPhysicalDeviceMemoryProperties(live->physical, &properties);
    uint32_t index = 0;
    while (index < properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (UINT32_C(1) << index)) ||
            !(properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
        ++index;
    if (index == properties.memoryTypeCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = index,
    };
    result = pokopia_vk_ledger_allocate(vkAllocateMemory, present->device, &allocate, memory, VK_LEDGER_CONVERSION);
    return result == VK_SUCCESS ? vkBindImageMemory(present->device, *image, *memory, 0) : result;
}

static VkImageView fsr_view(VkDevice device, VkImage image, VkFormat format)
{
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = color_range,
    };
    VkImageView view = VK_NULL_HANDLE;
    return vkCreateImageView(device, &info, NULL, &view) == VK_SUCCESS ? view : VK_NULL_HANDLE;
}

static bool fsr_build(PokopiaLivePresent *live)
{
    PokopiaPresent *present = live->present;
    const VkDevice device = present->device;
    live->fsr_out_format = fsr_srgb(present->format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (check_blit(live, live->fsr_out_format, present->format, false) != VK_SUCCESS)
        return false;
    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT;
    if (fsr_image(live, VK_FORMAT_R8G8B8A8_UNORM, storage | VK_IMAGE_USAGE_SAMPLED_BIT, false, &live->fsr_images[0],
                  &live->fsr_memory[0]) != VK_SUCCESS ||
        fsr_image(live, live->fsr_out_format, storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  live->fsr_out_format != VK_FORMAT_R8G8B8A8_UNORM, &live->fsr_images[1],
                  &live->fsr_memory[1]) != VK_SUCCESS)
        return false;
    live->fsr_sample_view = fsr_view(device, live->fsr_images[0], VK_FORMAT_R8G8B8A8_UNORM);
    live->fsr_storage_views[0] = fsr_view(device, live->fsr_images[0], VK_FORMAT_R8G8B8A8_UNORM);
    live->fsr_storage_views[1] = fsr_view(device, live->fsr_images[1], VK_FORMAT_R8G8B8A8_UNORM);
    if (!live->fsr_sample_view || !live->fsr_storage_views[0] || !live->fsr_storage_views[1])
        return false;

    const VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f,
    };
    if (vkCreateSampler(device, &sampler, NULL, &live->fsr_sampler) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(device, &set_info, NULL, &live->fsr_set_layout) != VK_SUCCESS)
        return false;
    const uint32_t *code[2] = {fsr_easu_spv, fsr_rcas_spv};
    const size_t code_bytes[2] = {sizeof fsr_easu_spv, sizeof fsr_rcas_spv};
    const uint32_t constant_bytes[2] = {80, 16};
    for (unsigned i = 0; i < 2; ++i) {
        const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, constant_bytes[i]};
        const VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
            .pSetLayouts = &live->fsr_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range,
        };
        if (vkCreatePipelineLayout(device, &layout_info, NULL, &live->fsr_layouts[i]) != VK_SUCCESS)
            return false;
        const VkShaderModuleCreateInfo module_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_bytes[i], .pCode = code[i],
        };
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &module_info, NULL, &module) != VK_SUCCESS)
            return false;
        const VkComputePipelineCreateInfo pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
            .layout = live->fsr_layouts[i],
        };
        const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                                       &live->fsr_pipelines[i]);
        vkDestroyShaderModule(device, module, NULL);
        if (made != VK_SUCCESS)
            return false;
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6},
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 6, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    if (vkCreateDescriptorPool(device, &pool_info, NULL, &live->fsr_pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetLayout layouts[6];
    for (unsigned i = 0; i < 6; ++i)
        layouts[i] = live->fsr_set_layout;
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = live->fsr_pool,
        .descriptorSetCount = 6, .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(device, &allocate, &live->fsr_sets[0][0]) != VK_SUCCESS)
        return false;
    /* The sharpening pass's sets never change: EASU's output in, the final image out. */
    for (unsigned slot = 0; slot < 3; ++slot) {
        const VkDescriptorImageInfo in = {live->fsr_sampler, live->fsr_sample_view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo out = {VK_NULL_HANDLE, live->fsr_storage_views[1], VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo easu_out = {VK_NULL_HANDLE, live->fsr_storage_views[0], VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->fsr_sets[slot][1], 0, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->fsr_sets[slot][1], 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out, NULL, NULL},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->fsr_sets[slot][0], 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &easu_out, NULL, NULL},
        };
        vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    }
    return true;
}

/* Upscale with FSR 1 when the frame is scaled to the scanout and sits at the
 * image origin unflipped. */
static bool fsr_prepare(PokopiaLivePresent *live, const PokopiaLiveFrame *frame, VkRect2D crop, bool scale)
{
    if (!scale || frame->flip_y || crop.offset.x || crop.offset.y)
        return false;
    if (!live->fsr_tried) {
        live->fsr_tried = true;
        live->fsr_ready = fsr_build(live);
    }
    return live->fsr_ready;
}

static VkImageView fsr_input(PokopiaLivePresent *live, const PokopiaLiveFrame *frame)
{
    for (unsigned i = 0; i < live->fsr_input_count; ++i)
        if (live->fsr_inputs[i].image == frame->image && live->fsr_inputs[i].format == frame->format)
            return live->fsr_inputs[i].view;
    const VkImageView view = fsr_view(live->present->device, frame->image, frame->format);
    if (view) {
        const unsigned at = live->fsr_input_count < 8 ? live->fsr_input_count++ : 7;
        live->fsr_inputs[at].image = frame->image;
        live->fsr_inputs[at].format = frame->format;
        live->fsr_inputs[at].view = view;
    }
    return view;
}

/* R96: the banner's buffer (white, black and shadow coverage packed per pixel)
 * and its compute pipeline, writing the FSR output image in place. */
static bool banner_build(PokopiaLivePresent *live)
{
    live->banner_tried = true;
    VkDevice device = live->present->device;
    void *pixels = NULL;
    bool coherent = false;
    const VkDeviceSize bytes = (VkDeviceSize)PS_BANNER_WIDTH * PS_BANNER_HEIGHT * 4;
    if (open_panel(live, bytes, &live->banner_buffer, &live->banner_memory, &pixels, &coherent) != VK_SUCCESS)
        return false;
    uint32_t *packed = pixels;
    for (size_t i = 0; i < (size_t)PS_BANNER_WIDTH * PS_BANNER_HEIGHT; ++i)
        packed[i] = (uint32_t)ps_banner_white[i] | (uint32_t)ps_banner_black[i] << 8 | (uint32_t)ps_banner_shadow[i] << 16;
    if (!coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = live->banner_memory, .size = VK_WHOLE_SIZE,
        };
        vkFlushMappedMemoryRanges(device, 1, &range);
    }
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    const VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(device, &set_info, NULL, &live->banner_set_layout) != VK_SUCCESS)
        return false;
    const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &live->banner_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range,
    };
    if (vkCreatePipelineLayout(device, &layout_info, NULL, &live->banner_layout) != VK_SUCCESS)
        return false;
    const VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fsr_banner_spv, .pCode = fsr_banner_spv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &module_info, NULL, &module) != VK_SUCCESS)
        return false;
    const VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = live->banner_layout,
    };
    const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                                   &live->banner_pipeline);
    vkDestroyShaderModule(device, module, NULL);
    if (made != VK_SUCCESS)
        return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    if (vkCreateDescriptorPool(device, &pool_info, NULL, &live->banner_pool) != VK_SUCCESS)
        return false;
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = live->banner_pool,
        .descriptorSetCount = 1, .pSetLayouts = &live->banner_set_layout,
    };
    if (vkAllocateDescriptorSets(device, &allocate, &live->banner_set) != VK_SUCCESS)
        return false;
    const VkDescriptorImageInfo image = {VK_NULL_HANDLE, live->fsr_storage_views[1], VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffer = {live->banner_buffer, 0, bytes};
    const VkWriteDescriptorSet writes[] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->banner_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &image, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->banner_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         NULL, &buffer, NULL},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    live->banner_ready = true;
    return true;
}

static bool fsr_record(PokopiaLivePresent *live, VkCommandBuffer command, const PokopiaLiveFrame *frame,
                       VkRect2D crop, VkImage target)
{
    PokopiaPresent *present = live->present;
    const VkImageView input = fsr_input(live, frame);
    if (!input)
        return false;
    const unsigned slot = present->async ? present->ring_at % 3 : 0;
    const VkDescriptorImageInfo source = {live->fsr_sampler, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, live->fsr_sets[slot][0], 0, 0, 1,
                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source, NULL, NULL};
    vkUpdateDescriptorSets(present->device, 1, &write, 0, NULL);

    const float in_w = (float)crop.extent.width, in_h = (float)crop.extent.height;
    const float size_w = (float)frame->extent.width, size_h = (float)frame->extent.height;
    const float out_w = (float)present->extent.width, out_h = (float)present->extent.height;
    uint32_t easu[20] = {
        /* FsrEasuCon (ffx_fsr1.h) */
        fsr_bits(in_w / out_w), fsr_bits(in_h / out_h),
        fsr_bits(0.5f * in_w / out_w - 0.5f), fsr_bits(0.5f * in_h / out_h - 0.5f),
        fsr_bits(1.0f / size_w), fsr_bits(1.0f / size_h), fsr_bits(1.0f / size_w), fsr_bits(-1.0f / size_h),
        fsr_bits(-1.0f / size_w), fsr_bits(2.0f / size_h), fsr_bits(1.0f / size_w), fsr_bits(2.0f / size_h),
        fsr_bits(0.0f), fsr_bits(4.0f / size_h), 0, 0,
        /* flags: the input samples as linear light */
        fsr_srgb(frame->format) ? 1u : 0u, 0, 0, 0,
    };
    /* FsrRcasCon: sharpness in stops below maximum (AMD's samples use 0.2). */
    const uint32_t rcas[4] = {fsr_bits(exp2f(-pokopia_settings_rcas_stops(pokopia_settings.sharpness))), 0, 0, 0};

    const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL;
    barrier(command, frame->image, frame->layout, read, frame->stages, frame->access, compute,
            VK_ACCESS_SHADER_READ_BIT);
    /* The intermediates are shared by frames in flight; order after the last frame's reads. */
    barrier(command, live->fsr_images[0], VK_IMAGE_LAYOUT_UNDEFINED, general, compute, VK_ACCESS_SHADER_READ_BIT,
            compute, VK_ACCESS_SHADER_WRITE_BIT);
    barrier(command, live->fsr_images[1], VK_IMAGE_LAYOUT_UNDEFINED, general, transfer, VK_ACCESS_TRANSFER_READ_BIT,
            compute, VK_ACCESS_SHADER_WRITE_BIT);
    const uint32_t groups_x = (present->extent.width + 15) / 16, groups_y = (present->extent.height + 15) / 16;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->fsr_pipelines[0]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->fsr_layouts[0], 0, 1,
                            &live->fsr_sets[slot][0], 0, NULL);
    vkCmdPushConstants(command, live->fsr_layouts[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof easu, easu);
    vkCmdDispatch(command, groups_x, groups_y, 1);
    barrier(command, live->fsr_images[0], general, read, compute, VK_ACCESS_SHADER_WRITE_BIT, compute,
            VK_ACCESS_SHADER_READ_BIT);
    barrier(command, frame->image, read, frame->layout, compute, VK_ACCESS_SHADER_READ_BIT, frame->stages,
            frame->access);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->fsr_pipelines[1]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->fsr_layouts[1], 0, 1,
                            &live->fsr_sets[slot][1], 0, NULL);
    vkCmdPushConstants(command, live->fsr_layouts[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rcas, rcas);
    vkCmdDispatch(command, groups_x, groups_y, 1);
    /* R96: the title banner, over the sharpened image. */
    if (live->banner_alpha > 0.0f && !live->banner_tried)
        banner_build(live);
    if (live->banner_alpha > 0.0f && live->banner_ready && live->banner_x >= 0 && live->banner_y >= 0 &&
        (uint32_t)live->banner_x + PS_BANNER_WIDTH <= present->extent.width &&
        (uint32_t)live->banner_y + PS_BANNER_HEIGHT <= present->extent.height) {
        barrier(command, live->fsr_images[1], general, general, compute, VK_ACCESS_SHADER_WRITE_BIT, compute,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        struct { int32_t x, y; uint32_t width, height; float alpha; } params = {
            live->banner_x, live->banner_y, PS_BANNER_WIDTH, PS_BANNER_HEIGHT,
            live->banner_alpha > 1.0f ? 1.0f : live->banner_alpha,
        };
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->banner_pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, live->banner_layout, 0, 1, &live->banner_set,
                                0, NULL);
        vkCmdPushConstants(command, live->banner_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof params, &params);
        vkCmdDispatch(command, (PS_BANNER_WIDTH + 15) / 16, (PS_BANNER_HEIGHT + 15) / 16, 1);
    }
    barrier(command, live->fsr_images[1], general, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, compute,
            VK_ACCESS_SHADER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    /* Same size: the blit only swaps channels for the BGRA scanout. */
    const VkImageBlit region = {
        .srcSubresource = color_layer, .dstSubresource = color_layer,
        .srcOffsets = {{0, 0, 0}, {(int32_t)present->extent.width, (int32_t)present->extent.height, 1}},
        .dstOffsets = {{0, 0, 0}, {(int32_t)present->extent.width, (int32_t)present->extent.height, 1}},
    };
    vkCmdBlitImage(command, live->fsr_images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    return true;
}

VkResult pokopia_live_present_frame(PokopiaLivePresent *live, const PokopiaLiveFrame *frame,
    uint64_t timeout_ns, VkResult *capture_result)
{
    if (capture_result)
        *capture_result = VK_NOT_READY;
    if (!live || !live->present || !frame || !frame->image || !frame->stages ||
        !valid_extent(frame->extent) || !valid_source_layout(frame->layout))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!frame->render_serial || frame->render_serial > frame->completed_render_serial)
        return VK_NOT_READY;

    PokopiaPresent *present = live->present;
    if (!present->swapchain || present->faulted || present->frame_open || present->fence_pending)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < present->image_count; ++i) {
        if (frame->image == present->targets[i].image)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    const bool srgb_pair = srgb_scanout_pair(frame->format, present->format);
    const bool channel_swap = channel_swap_pair(frame->format, present->format);
    if (frame->format != present->format && !srgb_pair && !channel_swap)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    VkRect2D crop = frame->crop;
    if (!crop.extent.width && !crop.extent.height && !crop.offset.x && !crop.offset.y)
        crop.extent = frame->extent;
    if (!valid_extent(crop.extent) || crop.offset.x < 0 || crop.offset.y < 0 ||
        (uint64_t)crop.offset.x + crop.extent.width > frame->extent.width ||
        (uint64_t)crop.offset.y + crop.extent.height > frame->extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result;
    VkResult completed_shot = VK_NOT_READY;
    if (live->capture && live->capture->pending_render_serial) {
        completed_shot = pokopia_game_capture_finish(live->capture, present,
            live->capture->pending_render_serial, VK_SUCCESS);
        if (capture_result)
            *capture_result = completed_shot;
        if (live->capture->pending_render_serial)
            return completed_shot;
    }

    const bool scale = !same_extent(crop.extent, present->extent);
    /* R92: the port menu can pick plain bilinear scaling instead of FSR 1. */
    const bool use_fsr = pokopia_settings.upscaler == POKOPIA_UPSCALER_FSR && fsr_prepare(live, frame, crop, scale);
    const bool transform = !use_fsr && (scale || frame->flip_y || channel_swap ||
        (srgb_pair && srgb_of_scanout(present->format) != frame->format));
    if (transform) {
        const VkFormat blit_target = srgb_pair ? srgb_of_scanout(present->format) : present->format;
        result = check_blit(live, frame->format, blit_target, scale);
        if (result != VK_SUCCESS)
            return result;
        if (srgb_pair) {
            result = ensure_conversion(live, blit_target);
            if (result != VK_SUCCESS)
                return result;
        }
    }

    PokopiaPresentTarget target;
    result = begin_present(live, timeout_ns, &target);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return result;
    const VkCommandBuffer command = live->active_command;

    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout source = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const bool fsr_done = use_fsr && fsr_record(live, command, frame, crop, target.image);
    if (!fsr_done) {
    barrier(command, frame->image, frame->layout, source, frame->stages, frame->access,
        transfer, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(command, target.image, VK_IMAGE_LAYOUT_UNDEFINED, destination,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);

    if (!transform && !use_fsr) {
        copy(command, frame->image, crop.offset, target.image, present->extent);
    } else if (!srgb_pair) {
        blit(command, frame, crop, target.image, present->extent, scale ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        /* Filter in linear light and keep the encoded result. A direct blit
         * to UNORM would decode sRGB and send linear bytes to VideoOut. */
        barrier(command, live->conversion, VK_IMAGE_LAYOUT_UNDEFINED, destination,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
        blit(command, frame, crop, live->conversion, present->extent, scale ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        barrier(command, live->conversion, destination, source, transfer,
            VK_ACCESS_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        copy(command, live->conversion, (VkOffset2D){0}, target.image, present->extent);
    }
    barrier(command, frame->image, source, frame->layout, transfer, VK_ACCESS_TRANSFER_READ_BIT,
        frame->stages, frame->access);
    }
    if (live->keyboard_visible && live->keyboard_buffer && live->keyboard_width == present->extent.width) {
        record_panel_at(command, target.image, live->keyboard_buffer, 0, 0, 0, live->keyboard_width,
            live->keyboard_top);
        record_panel_at(command, target.image, live->keyboard_buffer,
            (VkDeviceSize)live->keyboard_width * live->keyboard_top * 4, 0,
            (int32_t)(present->extent.height - live->keyboard_bottom), live->keyboard_width, live->keyboard_bottom);
    }
    if (live->menu_visible && live->menu_buffer && live->menu_width <= present->extent.width &&
        live->menu_height <= present->extent.height)
        record_panel_at(command, target.image, live->menu_buffer, 0,
            (int32_t)(present->extent.width - live->menu_width) / 2,
            (int32_t)(present->extent.height - live->menu_height) / 2, live->menu_width, live->menu_height);
    if (live->overlay_ready && !live->overlay_hidden)
        record_overlay(command, target.image, live->overlay_buffer);
    if (live->fps_ready && !live->fps_hidden)
        record_panel(command, target.image, live->fps_buffer,
            (int32_t)(present->extent.width - OVERLAY_INSET - POKOPIA_FPS_WIDTH), OVERLAY_INSET,
            POKOPIA_FPS_WIDTH, POKOPIA_FPS_HEIGHT);

    VkResult shot = VK_NOT_READY;
    if (live->capture) {
        shot = pokopia_game_capture_record(live->capture, live->physical, present, command,
            destination, transfer, VK_ACCESS_TRANSFER_WRITE_BIT, frame->render_serial);
    }
    if (shot != VK_SUCCESS) {
        barrier(command, target.image, destination, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, transfer,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    result = vkEndCommandBuffer(command);
    if (result == VK_SUCCESS)
        result = pokopia_present_frame(present, command, timeout_ns);
    else
        present->faulted = true;
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        live->presented_render_serial = frame->render_serial;
    if (shot == VK_SUCCESS)
        shot = pokopia_game_capture_finish(live->capture, present, frame->render_serial, result);
    if (capture_result)
        *capture_result = shot == VK_NOT_READY ? completed_shot : shot;
    return result;
}
