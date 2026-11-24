/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_present.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HANDLE(type, n) ((type)(uintptr_t)(n))
#define GAME_IMAGE HANDLE(VkImage, 10)
#define OUTPUT_IMAGE HANDLE(VkImage, 11)
#define INTERMEDIATE HANDLE(VkImage, 12)
#define MEMORY HANDLE(VkDeviceMemory, 20)
#define OVERLAY_MEMORY HANDLE(VkDeviceMemory, 21)
#define OVERLAY_BUFFER HANDLE(VkBuffer, 30)

enum { OVERLAY_WIDTH = 420, OVERLAY_HEIGHT = 88, OVERLAY_INSET = 24,
       OVERLAY_BYTES = OVERLAY_WIDTH * OVERLAY_HEIGHT * 4 };

static unsigned char overlay_pixels[OVERLAY_BYTES];

static struct {
    VkImageMemoryBarrier barriers[8];
    unsigned barrier_count, copies, blits, acquires, submits, allocations, destroys;
    VkImage copy_source, blit_destination;
    VkImageCopy copy;
    VkImageBlit blit;
    VkFilter filter;
    VkResult acquire, submit, capture, finish, begin, end, overlay_flush;
    bool captured_after_present;
    bool coherent_overlay;
    unsigned overlay_allocations, overlay_destroys, overlay_unmaps, overlay_frees, overlay_flushes;
    unsigned capture_calls, overlay_copies, standalone_clears;
    unsigned command_order, game_write_order, clear_order, transfer_dependency_order,
             overlay_copy_order, capture_order, present_layout_order, submit_order;
} trace;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image)
{
    (void)device;
    (void)allocator;
    assert(info->format == VK_FORMAT_R8G8B8A8_SRGB);
    assert(info->usage == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    trace.allocations++;
    *image = INTERMEDIATE;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    assert(image == INTERMEDIATE);
    trace.destroys++;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
    VkMemoryRequirements *out)
{
    (void)device;
    assert(image == INTERMEDIATE);
    *out = (VkMemoryRequirements){.size = 4096, .alignment = 4096, .memoryTypeBits = 2};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical,
    VkPhysicalDeviceMemoryProperties *out)
{
    (void)physical;
    *out = (VkPhysicalDeviceMemoryProperties){.memoryTypeCount = 3};
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (trace.coherent_overlay ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    (void)device;
    (void)allocator;
    if (info->allocationSize == OVERLAY_BYTES) {
        assert(info->memoryTypeIndex == (trace.coherent_overlay ? 2u : 0u));
        *memory = OVERLAY_MEMORY;
    } else {
        assert(info->allocationSize == 4096 && info->memoryTypeIndex == 1);
        *memory = MEMORY;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image,
    VkDeviceMemory memory, VkDeviceSize offset)
{
    (void)device;
    assert(image == INTERMEDIATE && memory == MEMORY && offset == 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    if (memory == OVERLAY_MEMORY)
        trace.overlay_frees++;
    else
        assert(memory == MEMORY);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    (void)device;
    (void)allocator;
    assert(info->size == OVERLAY_BYTES && (info->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    trace.overlay_allocations++;
    *buffer = OVERLAY_BUFFER;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    assert(buffer == OVERLAY_BUFFER);
    trace.overlay_destroys++;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *out)
{
    (void)device;
    assert(buffer == OVERLAY_BUFFER);
    *out = (VkMemoryRequirements){.size = OVERLAY_BYTES, .alignment = 4, .memoryTypeBits = 5};
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize offset)
{
    (void)device;
    assert(buffer == OVERLAY_BUFFER && memory == OVERLAY_MEMORY && offset == 0);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
    (void)device;
    assert(memory == OVERLAY_MEMORY && offset == 0 && !flags);
    assert(size == VK_WHOLE_SIZE || size == OVERLAY_BYTES);
    memset(overlay_pixels, 0, sizeof overlay_pixels);
    *data = overlay_pixels;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    (void)device;
    assert(memory == OVERLAY_MEMORY);
    trace.overlay_unmaps++;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t count,
    const VkMappedMemoryRange *ranges)
{
    (void)device;
    assert(!trace.coherent_overlay && count == 1 && ranges->memory == OVERLAY_MEMORY && ranges->offset == 0);
    assert(ranges->size == VK_WHOLE_SIZE || ranges->size == OVERLAY_BYTES);
    trace.overlay_flushes++;
    return trace.overlay_flush;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical,
    VkFormat format, VkFormatProperties *out)
{
    (void)physical;
    (void)format;
    *out = (VkFormatProperties){.optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT};
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer command, VkCommandBufferResetFlags flags)
{
    (void)command;
    (void)flags;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer command,
    const VkCommandBufferBeginInfo *info)
{
    (void)command;
    assert(info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    trace.command_order = trace.game_write_order = trace.clear_order = trace.transfer_dependency_order = 0;
    trace.overlay_copy_order = trace.capture_order = trace.present_layout_order = trace.submit_order = 0;
    return trace.begin;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer command)
{
    (void)command;
    return trace.end;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memory_count,
    const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
    uint32_t image_count, const VkImageMemoryBarrier *images)
{
    (void)command;
    (void)flags;
    (void)memory;
    (void)buffers;
    assert(memory_count == 0 && buffer_count == 0 && image_count == 1);
    assert(trace.barrier_count < sizeof trace.barriers / sizeof trace.barriers[0]);
    trace.barriers[trace.barrier_count++] = *images;
    if (images->image != OUTPUT_IMAGE)
        return;
    if (images->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        images->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        assert(source == VK_PIPELINE_STAGE_TRANSFER_BIT && destination == VK_PIPELINE_STAGE_TRANSFER_BIT);
        assert(images->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
               images->dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
        trace.transfer_dependency_order = ++trace.command_order;
    } else if (images->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        trace.present_layout_order = ++trace.command_order;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer command, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    uint32_t count, const VkImageCopy *region)
{
    (void)command;
    assert(source_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
           destination_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && destination == OUTPUT_IMAGE && count == 1);
    trace.copy_source = source;
    trace.copy = *region;
    trace.copies++;
    trace.game_write_order = ++trace.command_order;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer command, VkImage source,
    VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout,
    uint32_t count, const VkImageBlit *region, VkFilter filter)
{
    (void)command;
    assert(source == GAME_IMAGE && source_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
           destination_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1);
    trace.blit_destination = destination;
    trace.blit = *region;
    trace.filter = filter;
    trace.blits++;
    if (destination == OUTPUT_IMAGE)
        trace.game_write_order = ++trace.command_order;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer command, VkBuffer source, VkImage destination,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *region)
{
    (void)command;
    assert(source == OVERLAY_BUFFER && destination == OUTPUT_IMAGE && layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(count == 1 && region->bufferOffset == 0);
    assert(region->imageOffset.x == OVERLAY_INSET && region->imageOffset.y == OVERLAY_INSET &&
           region->imageOffset.z == 0);
    assert(region->imageExtent.width == OVERLAY_WIDTH && region->imageExtent.height == OVERLAY_HEIGHT &&
           region->imageExtent.depth == 1);
    assert(region->imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && region->imageSubresource.layerCount == 1);
    trace.overlay_copies++;
    trace.overlay_copy_order = ++trace.command_order;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *range)
{
    (void)command;
    (void)color;
    assert(image == OUTPUT_IMAGE && layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1);
    assert(range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && range->levelCount == 1 && range->layerCount == 1);
    trace.standalone_clears++;
    trace.clear_order = ++trace.command_order;
}

VkResult pokopia_present_acquire(PokopiaPresent *present, uint64_t timeout, PokopiaPresentTarget *target)
{
    (void)timeout;
    trace.acquires++;
    if (trace.acquire == VK_SUCCESS || trace.acquire == VK_SUBOPTIMAL_KHR) {
        *target = *present->targets;
        present->frame_open = true;
    }
    return trace.acquire;
}

VkResult pokopia_present_frame(PokopiaPresent *present, VkCommandBuffer command, uint64_t timeout)
{
    (void)command;
    (void)timeout;
    assert(present->frame_open);
    trace.submits++;
    trace.submit_order = ++trace.command_order;
    present->frame_open = false;
    return trace.submit;
}

VkResult pokopia_game_capture_record(PokopiaGameCapture *capture, VkPhysicalDevice physical,
    const PokopiaPresent *present, VkCommandBuffer command, VkImageLayout layout,
    VkPipelineStageFlags stages, VkAccessFlags access, uint64_t serial)
{
    (void)physical;
    (void)command;
    assert(present->frame_open && layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
           stages == VK_PIPELINE_STAGE_TRANSFER_BIT && access == VK_ACCESS_TRANSFER_WRITE_BIT);
    trace.capture_calls++;
    trace.capture_order = ++trace.command_order;
    if (trace.capture == VK_SUCCESS)
        capture->pending_render_serial = serial;
    return trace.capture;
}

VkResult pokopia_game_capture_finish(PokopiaGameCapture *capture, const PokopiaPresent *present,
    uint64_t serial, VkResult result)
{
    assert(capture->pending_render_serial == serial && !present->frame_open);
    trace.captured_after_present = trace.submits && result == VK_SUCCESS;
    if (trace.finish != VK_NOT_READY)
        capture->pending_render_serial = 0;
    return trace.finish;
}

static void test_overlay(PokopiaPresent *present, PokopiaGameCapture *capture)
{
    memset(&trace, 0, sizeof trace);
    trace.coherent_overlay = true;
    trace.capture = VK_SUCCESS;
    PokopiaLivePresent live = {0};
    assert(pokopia_live_present_init(&live, HANDLE(VkPhysicalDevice, 5), present,
        HANDLE(VkCommandBuffer, 6), capture) == VK_SUCCESS);

    const char *const lines[POKOPIA_OVERLAY_LINES] = {"POKOPIA PS5", "VIDEO READY", "WAITING FOR GAME", "DEBUG STATUS"};
    assert(pokopia_live_present_overlay_update(&live, lines) == VK_SUCCESS);
    assert(trace.overlay_allocations == 1 && !trace.overlay_flushes && !trace.acquires);
    bool dark = false, bright = false;
    for (size_t i = 0; i < sizeof overlay_pixels; i += 4) {
        assert(overlay_pixels[i + 3] == 255);
        dark |= overlay_pixels[i] < 64 && overlay_pixels[i + 1] < 64 && overlay_pixels[i + 2] < 64;
        bright |= overlay_pixels[i] > 192 && overlay_pixels[i + 1] > 192 && overlay_pixels[i + 2] > 192;
    }
    assert(dark && bright);

    trace.acquire = VK_TIMEOUT;
    assert(pokopia_live_present_show_overlay(&live, 1000) == VK_TIMEOUT);
    assert(trace.acquires == 1 && !trace.submits && !trace.overlay_copies && !trace.standalone_clears);
    assert(!present->frame_open && !present->faulted && !live.presented_render_serial);

    trace.acquire = VK_SUCCESS;
    assert(pokopia_live_present_show_overlay(&live, 1000) == VK_SUCCESS);
    assert(trace.acquires == 2 && trace.submits == 1 && trace.overlay_copies == 1 && trace.standalone_clears == 1);
    assert(trace.clear_order && trace.clear_order < trace.transfer_dependency_order &&
           trace.transfer_dependency_order < trace.overlay_copy_order &&
           trace.overlay_copy_order < trace.present_layout_order && trace.present_layout_order < trace.submit_order);
    assert(!trace.game_write_order && !trace.capture_calls && !live.presented_render_serial);

    const PokopiaLiveFrame frame = {.image = GAME_IMAGE, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {1280, 720}, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .render_serial = 1, .completed_render_serial = 1};
    trace.barrier_count = 0;
    VkResult shot = VK_NOT_READY;
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS);
    assert(shot == VK_SUCCESS && trace.captured_after_present && trace.capture_calls == 1 && live.presented_render_serial == 1);
    assert(trace.acquires == 3 && trace.submits == 2 && trace.overlay_copies == 2 && trace.standalone_clears == 1);
    assert(trace.game_write_order && trace.game_write_order < trace.transfer_dependency_order &&
           trace.transfer_dependency_order < trace.overlay_copy_order &&
           trace.overlay_copy_order < trace.capture_order && trace.capture_order < trace.submit_order);
    assert(!trace.clear_order);

    pokopia_live_present_close(&live);
    pokopia_live_present_close(&live);
    assert(trace.overlay_destroys == 1 && trace.overlay_unmaps == 1 && trace.overlay_frees == 1);

    memset(&trace, 0, sizeof trace);
    assert(pokopia_live_present_init(&live, HANDLE(VkPhysicalDevice, 5), present,
        HANDLE(VkCommandBuffer, 6), NULL) == VK_SUCCESS);
    trace.overlay_flush = VK_ERROR_MEMORY_MAP_FAILED;
    assert(pokopia_live_present_overlay_update(&live, lines) == VK_ERROR_MEMORY_MAP_FAILED);
    assert(trace.overlay_allocations == 1 && trace.overlay_flushes == 1);
    assert(trace.overlay_destroys == 1 && trace.overlay_unmaps == 1 && trace.overlay_frees == 1);
    trace.overlay_flush = VK_SUCCESS;
    assert(pokopia_live_present_overlay_update(&live, lines) == VK_SUCCESS);
    assert(pokopia_live_present_overlay_update(&live, lines) == VK_SUCCESS);
    assert(trace.overlay_allocations == 2 && trace.overlay_flushes == 3);
    pokopia_live_present_close(&live);
    assert(trace.overlay_destroys == 2 && trace.overlay_unmaps == 2 && trace.overlay_frees == 2);
}

int main(void)
{
    PokopiaPresentTarget target = {.image = OUTPUT_IMAGE};
    PokopiaPresent present = {.device = HANDLE(VkDevice, 1), .queue = HANDLE(VkQueue, 2),
        .swapchain = HANDLE(VkSwapchainKHR, 3), .fence = HANDLE(VkFence, 4),
        .targets = &target, .image_count = 1, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1920, 1080}};
    PokopiaGameCapture capture = {.mailbox = {.directory = "debug"}};
    PokopiaLivePresent live = {0};
    assert(pokopia_live_present_init(&live, HANDLE(VkPhysicalDevice, 5), &present,
        HANDLE(VkCommandBuffer, 6), &capture) == VK_SUCCESS);

    PokopiaLiveFrame frame = {.image = GAME_IMAGE, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {1920, 1080}, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .render_serial = 2, .completed_render_serial = 1};
    assert(pokopia_live_present_frame(&live, &frame, 1000, NULL) == VK_NOT_READY && !trace.acquires);
    frame.completed_render_serial = 2;
    frame.crop = (VkRect2D){.offset = {1, 0}, .extent = {1920, 1080}};
    assert(pokopia_live_present_frame(&live, &frame, 1000, NULL) == VK_ERROR_INITIALIZATION_FAILED && !trace.acquires);

    frame.crop = (VkRect2D){0};
    trace.capture = VK_NOT_READY;
    VkResult shot = VK_SUCCESS;
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS);
    assert(shot == VK_NOT_READY && live.presented_render_serial == 2 && trace.acquires == 1 && trace.submits == 1);
    assert(trace.copies == 1 && trace.copy_source == GAME_IMAGE && !trace.blits && !trace.allocations);
    assert(trace.barrier_count == 4 && trace.barriers[2].image == GAME_IMAGE);
    assert(trace.barriers[2].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        trace.barriers[2].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(trace.barriers[3].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    trace.barrier_count = 0;
    trace.capture = VK_SUCCESS;
    trace.finish = VK_ERROR_UNKNOWN;
    frame.render_serial = frame.completed_render_serial = 3;
    frame.flip_y = true;
    frame.crop = (VkRect2D){.offset = {8, 12}, .extent = {1280, 720}};
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS);
    assert(shot == VK_ERROR_UNKNOWN && trace.captured_after_present && live.presented_render_serial == 3);
    assert(trace.allocations == 1 && trace.blits == 1 && trace.copies == 2 && trace.barrier_count == 5);
    assert(trace.blit_destination == INTERMEDIATE && trace.copy_source == INTERMEDIATE && trace.filter == VK_FILTER_LINEAR);
    assert(trace.blit.srcOffsets[0].x == 8 && trace.blit.srcOffsets[0].y == 732 && trace.blit.srcOffsets[1].y == 12);
    assert(trace.blit.dstOffsets[1].x == 1920 && trace.blit.dstOffsets[1].y == 1080);

    trace.barrier_count = 0;
    trace.capture = VK_NOT_READY;
    trace.acquire = VK_TIMEOUT;
    frame.render_serial = frame.completed_render_serial = 4;
    assert(pokopia_live_present_frame(&live, &frame, 1000, NULL) == VK_TIMEOUT);
    assert(!trace.barrier_count && trace.submits == 2 && live.presented_render_serial == 3);
    assert(trace.allocations == 1);

    trace.acquire = VK_SUCCESS;
    trace.capture = VK_SUCCESS;
    trace.finish = VK_NOT_READY;
    frame.format = present.format;
    frame.crop = (VkRect2D){0};
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS && shot == VK_NOT_READY);
    assert(trace.blit_destination == OUTPUT_IMAGE && trace.filter == VK_FILTER_NEAREST);
    assert(capture.pending_render_serial == 4 && trace.submits == 3);
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_NOT_READY && trace.submits == 3);
    trace.finish = VK_SUCCESS;
    trace.capture = VK_NOT_READY;
    trace.barrier_count = 0;
    assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS && shot == VK_SUCCESS);
    assert(!capture.pending_render_serial && trace.submits == 4);

    pokopia_live_present_close(&live);
    assert(trace.destroys == 1 && !live.present && !trace.overlay_allocations && !trace.overlay_copies);
    test_overlay(&present, &capture);
    puts("Live present command sequencing passed");
}
