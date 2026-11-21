/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_present.h"

#include <assert.h>
#include <stdio.h>

#define HANDLE(type, n) ((type)(uintptr_t)(n))
#define GAME_IMAGE HANDLE(VkImage, 10)
#define OUTPUT_IMAGE HANDLE(VkImage, 11)
#define INTERMEDIATE HANDLE(VkImage, 12)
#define MEMORY HANDLE(VkDeviceMemory, 20)

static struct {
    VkImageMemoryBarrier barriers[8];
    unsigned barrier_count, copies, blits, acquires, submits, allocations, destroys;
    VkImage copy_source, blit_destination;
    VkImageCopy copy;
    VkImageBlit blit;
    VkFilter filter;
    VkResult acquire, submit, capture, finish, begin, end;
    bool captured_after_present;
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
    *out = (VkPhysicalDeviceMemoryProperties){.memoryTypeCount = 2};
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    (void)device;
    (void)allocator;
    assert(info->allocationSize == 4096 && info->memoryTypeIndex == 1);
    *memory = MEMORY;
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
    assert(memory == MEMORY);
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
    (void)source;
    (void)destination;
    (void)flags;
    (void)memory;
    (void)buffers;
    assert(memory_count == 0 && buffer_count == 0 && image_count == 1);
    assert(trace.barrier_count < sizeof trace.barriers / sizeof trace.barriers[0]);
    trace.barriers[trace.barrier_count++] = *images;
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

    const VkExtent2D sizes[] = {{1280, 720}, {1600, 900}};
    frame.format = VK_FORMAT_R8G8B8A8_SRGB;
    frame.flip_y = false;
    for (uint32_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        frame.extent = sizes[i];
        frame.crop = (VkRect2D){0};
        frame.render_serial = frame.completed_render_serial = 5 + i;
        trace.barrier_count = 0;
        assert(pokopia_live_present_frame(&live, &frame, 1000, &shot) == VK_SUCCESS);
        assert(trace.blit_destination == INTERMEDIATE && trace.copy_source == INTERMEDIATE);
        assert(trace.filter == VK_FILTER_LINEAR && trace.allocations == 1);
        assert(trace.blit.srcOffsets[0].x == 0 && trace.blit.srcOffsets[0].y == 0);
        assert(trace.blit.srcOffsets[1].x == (int32_t)sizes[i].width &&
               trace.blit.srcOffsets[1].y == (int32_t)sizes[i].height);
        assert(trace.blit.dstOffsets[1].x == 1920 && trace.blit.dstOffsets[1].y == 1080);
    }
    puts("Both reduced game image extents blit to 1920x1080 VideoOut using one conversion image");
    pokopia_live_present_close(&live);
    assert(trace.destroys == 1 && !live.present);
    puts("Live present command sequencing passed");
}

