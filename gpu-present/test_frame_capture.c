/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "frame_capture.h"

#include <assert.h>
#include <stdio.h>

enum { WIDTH = 193, HEIGHT = 117 };
static unsigned char pixels[WIDTH * HEIGHT * 4];
static bool fence_ready, coherent, invalidated;
static unsigned commands;
static VkBuffer buffer = (VkBuffer)(uintptr_t)4;
static VkDeviceMemory memory = (VkDeviceMemory)(uintptr_t)5;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                              const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    assert(device && !allocator && info->size == sizeof pixels);
    assert(info->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    *out = buffer;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer value,
                                                         VkMemoryRequirements *out)
{
    assert(device && value == buffer);
    *out = (VkMemoryRequirements){.size = sizeof pixels + 128, .alignment = 128, .memoryTypeBits = 7};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical,
                                                               VkPhysicalDeviceMemoryProperties *out)
{
    assert(physical);
    *out = (VkPhysicalDeviceMemoryProperties){.memoryTypeCount = 3};
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (coherent)
        out->memoryTypes[2].propertyFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
    assert(device && !allocator && info->allocationSize == sizeof pixels + 128 && info->memoryTypeIndex == 2);
    *out = memory;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer value,
                                                  VkDeviceMemory allocation, VkDeviceSize offset)
{
    assert(device && value == buffer && allocation == memory && !offset);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory allocation,
                                           VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                           void **out)
{
    assert(device && allocation == memory && !offset && size == VK_WHOLE_SIZE && !flags);
    *out = pixels;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory allocation)
{
    assert(device && allocation == memory);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer value,
                                           const VkAllocationCallbacks *allocator)
{
    assert(device && value == buffer && !allocator);
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory allocation,
                                        const VkAllocationCallbacks *allocator)
{
    assert(device && allocation == memory && !allocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
                                                VkPipelineStageFlags dest, VkDependencyFlags dependency,
                                                uint32_t memory_count, const VkMemoryBarrier *memory_barriers,
                                                uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
                                                uint32_t image_count, const VkImageMemoryBarrier *images)
{
    assert(command && !dependency && !memory_count && !memory_barriers && image_count == 1);
    assert(images->image && images->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    assert(images->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(images->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    if (commands == 0) {
        assert(source == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT && dest == VK_PIPELINE_STAGE_TRANSFER_BIT);
        assert(!buffer_count && !buffers);
        assert(images->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        assert(images->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        assert(images->srcAccessMask == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        assert(images->dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
    } else {
        assert(commands == 2 && source == VK_PIPELINE_STAGE_TRANSFER_BIT && (dest & VK_PIPELINE_STAGE_HOST_BIT));
        assert(buffer_count == 1 && buffers->buffer == buffer && buffers->size == VK_WHOLE_SIZE);
        assert(buffers->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
        assert(buffers->dstAccessMask == VK_ACCESS_HOST_READ_BIT);
        assert(images->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        assert(images->newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        assert(images->srcAccessMask == VK_ACCESS_TRANSFER_READ_BIT && !images->dstAccessMask);
    }
    ++commands;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer command, VkImage image,
                                                  VkImageLayout layout, VkBuffer dest, uint32_t count,
                                                  const VkBufferImageCopy *region)
{
    assert(commands == 1 && command && image && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(dest == buffer && count == 1 && !region->bufferOffset && !region->bufferRowLength && !region->bufferImageHeight);
    assert(region->imageExtent.width == WIDTH && region->imageExtent.height == HEIGHT && region->imageExtent.depth == 1);
    ++commands;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{
    assert(device && fence && commands == 3);
    return fence_ready ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t count,
                                                               const VkMappedMemoryRange *ranges)
{
    assert(device && count == 1 && ranges->memory == memory && !ranges->offset && ranges->size == VK_WHOLE_SIZE);
    assert(fence_ready && !coherent && !invalidated);
    invalidated = true;
    return VK_SUCCESS;
}

static void capture_test(const char *path, bool bgra)
{
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned char *pixel = &pixels[(y * WIDTH + x) * 4];
            pixel[bgra ? 2 : 0] = (unsigned char)(x + y);
            pixel[1] = (unsigned char)(2 * x + y);
            pixel[bgra ? 0 : 2] = (unsigned char)(x + 3 * y);
            pixel[3] = 0;
        }
    }
    coherent = !bgra;
    fence_ready = invalidated = false;
    commands = 0;
    PokopiaFrameCapture capture = {0};
    const VkDevice device = (VkDevice)(uintptr_t)1;
    const VkPhysicalDevice physical = (VkPhysicalDevice)(uintptr_t)2;
    const VkCommandBuffer command = (VkCommandBuffer)(uintptr_t)3;
    const VkImage image = (VkImage)(uintptr_t)6;
    const VkFence fence = (VkFence)(uintptr_t)7;
    const VkFormat format = bgra ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    assert(pokopia_frame_capture_open(&capture, physical, device, format, (VkExtent2D){0, HEIGHT}) ==
           VK_ERROR_INITIALIZATION_FAILED);
    assert(pokopia_frame_capture_open(&capture, physical, device, VK_FORMAT_R16_UNORM, (VkExtent2D){WIDTH, HEIGHT}) ==
           VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(pokopia_frame_capture_open(&capture, physical, device, format, (VkExtent2D){WIDTH, HEIGHT}) == VK_SUCCESS);
    assert(pokopia_frame_capture_record(&capture, command, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) == VK_SUCCESS);
    assert(pokopia_frame_capture_record(&capture, command, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) == VK_NOT_READY);

    FILE *file = fopen(path, "wb");
    assert(file && fputc('X', file) == 'X' && fclose(file) == 0);
    assert(pokopia_frame_capture_save(&capture, fence, path) == VK_NOT_READY && !invalidated);
    file = fopen(path, "rb");
    assert(file && fgetc(file) == 'X' && fclose(file) == 0);
    fence_ready = true;
    assert(pokopia_frame_capture_save(&capture, fence, path) == VK_SUCCESS && invalidated == bgra);
    assert(pokopia_frame_capture_save(&capture, fence, path) == VK_ERROR_INITIALIZATION_FAILED);
    pokopia_frame_capture_close(&capture);
    assert(!capture.device);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    capture_test(argv[1], false);
    capture_test(argv[2], true);
    puts("Framebuffer capture command and fence vectors passed.");
    return 0;
}
