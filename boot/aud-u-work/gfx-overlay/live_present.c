/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_present.h"

static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkImageSubresourceLayers color_layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

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

static bool srgb_scanout_pair(VkFormat source, VkFormat output)
{
    return (source == VK_FORMAT_R8G8B8A8_SRGB && output == VK_FORMAT_R8G8B8A8_UNORM) ||
           (source == VK_FORMAT_B8G8R8A8_SRGB && output == VK_FORMAT_B8G8R8A8_UNORM);
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
        vkFreeMemory(live->present->device, live->conversion_memory, NULL);
    live->conversion = VK_NULL_HANDLE;
    live->conversion_memory = VK_NULL_HANDLE;
}

void pokopia_live_present_close(PokopiaLivePresent *live)
{
    if (!live || !live->present)
        return;
    release_conversion(live);
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
    result = vkAllocateMemory(present->device, &allocate, NULL, &live->conversion_memory);
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
    if (frame->format != present->format && !srgb_pair)
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
    const bool transform = scale || frame->flip_y;
    if (transform) {
        result = check_blit(live, frame->format, srgb_pair ? frame->format : present->format, scale);
        if (result != VK_SUCCESS)
            return result;
        if (srgb_pair) {
            result = ensure_conversion(live, frame->format);
            if (result != VK_SUCCESS)
                return result;
        }
    }

    const VkCommandBuffer command = live->command;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkResetCommandBuffer(command, 0);
    if (result == VK_SUCCESS)
        result = vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        return result;

    PokopiaPresentTarget target;
    result = pokopia_present_acquire(present, timeout_ns, &target);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        vkResetCommandBuffer(command, 0);
        return result;
    }

    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout source = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier(command, frame->image, frame->layout, source, frame->stages, frame->access,
        transfer, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(command, target.image, VK_IMAGE_LAYOUT_UNDEFINED, destination,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);

    if (!transform) {
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
