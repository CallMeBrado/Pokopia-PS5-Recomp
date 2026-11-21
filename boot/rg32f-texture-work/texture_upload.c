/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "texture_upload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UPLOAD_DEVICE_PROCS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) \
    X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit)

struct NvnTextureUpload {
    VkDevice device;
    VkQueue queue;
    size_t capacity;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    bool coherent;
    bool pending;
    bool lost;

#define DECLARE_PROC(name) PFN_vk##name name;
    UPLOAD_DEVICE_PROCS(DECLARE_PROC)
#undef DECLARE_PROC
};

static size_t block_count(uint32_t texels, uint32_t block_extent)
{
    return texels / block_extent + (texels % block_extent != 0);
}

VkResult nvn_texture_upload_footprint(VkFormat format, VkExtent2D extent, size_t source_row_pitch,
                                      NvnTextureFootprint *footprint)
{
    if (!footprint || !extent.width || !extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;

    NvnTextureFootprint result = {0};
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        result.block_extent = (VkExtent2D){1, 1};
        result.bytes_per_block = 4;
        break;
    case VK_FORMAT_R32G32_SFLOAT:
        result.block_extent = (VkExtent2D){1, 1};
        result.bytes_per_block = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        result.block_extent = (VkExtent2D){1, 1};
        result.bytes_per_block = 16;
        break;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        result.block_extent = (VkExtent2D){4, 4};
        result.bytes_per_block = 8;
        break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        result.block_extent = (VkExtent2D){4, 4};
        result.bytes_per_block = 16;
        break;
    default:
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    const size_t columns = block_count(extent.width, result.block_extent.width);
    if (columns > SIZE_MAX / result.bytes_per_block)
        return VK_ERROR_INITIALIZATION_FAILED;
    result.rows = block_count(extent.height, result.block_extent.height);
    result.row_bytes = columns * result.bytes_per_block;
    result.source_row_pitch = source_row_pitch ? source_row_pitch : result.row_bytes;
    if (result.source_row_pitch < result.row_bytes || result.rows > SIZE_MAX / result.row_bytes ||
        result.rows - 1 > (SIZE_MAX - result.row_bytes) / result.source_row_pitch)
        return VK_ERROR_INITIALIZATION_FAILED;

    result.packed_bytes = result.rows * result.row_bytes;
    result.required_source_bytes = (result.rows - 1) * result.source_row_pitch + result.row_bytes;
    *footprint = result;
    return VK_SUCCESS;
}

static bool bind_procs(NvnTextureUpload *upload, PFN_vkGetDeviceProcAddr get_proc)
{
#define BIND_PROC(name) \
    upload->name = (PFN_vk##name)get_proc(upload->device, "vk" #name); \
    if (!upload->name) return false;
    UPLOAD_DEVICE_PROCS(BIND_PROC)
#undef BIND_PROC
    return true;
}
#undef UPLOAD_DEVICE_PROCS

static VkResult open_buffer(NvnTextureUpload *upload, const VkPhysicalDeviceMemoryProperties *memory)
{
    const VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = upload->capacity,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = upload->CreateBuffer(upload->device, &buffer, NULL, &upload->buffer);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements;
    upload->GetBufferMemoryRequirements(upload->device, upload->buffer, &requirements);
    uint32_t type = memory->memoryTypeCount;
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) ||
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            upload->coherent = true;
            type = i;
            break;
        }
        if (type == memory->memoryTypeCount)
            type = i;
    }
    if (type == memory->memoryTypeCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = upload->AllocateMemory(upload->device, &allocation, NULL, &upload->memory);
    if (result == VK_SUCCESS)
        result = upload->BindBufferMemory(upload->device, upload->buffer, upload->memory, 0);
    if (result == VK_SUCCESS) {
        void *mapped;
        result = upload->MapMemory(upload->device, upload->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result == VK_SUCCESS)
            upload->mapped = mapped;
    }
    return result;
}

static VkResult open_commands(NvnTextureUpload *upload, uint32_t family)
{
    const VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkResult result = upload->CreateCommandPool(upload->device, &pool, NULL, &upload->pool);
    if (result != VK_SUCCESS)
        return result;

    const VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = upload->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = upload->AllocateCommandBuffers(upload->device, &command, &upload->command);
    if (result == VK_SUCCESS) {
        const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = upload->CreateFence(upload->device, &fence, NULL, &upload->fence);
    }
    return result;
}

VkResult nvn_texture_upload_open(NvnTextureUpload **out, VkDevice device, VkQueue queue,
                                 uint32_t queue_family, const VkPhysicalDeviceMemoryProperties *memory,
                                 PFN_vkGetDeviceProcAddr get_proc, size_t capacity)
{
    if (!out || *out || !device || !queue || !memory || !get_proc || !capacity ||
        !memory->memoryTypeCount || memory->memoryTypeCount > VK_MAX_MEMORY_TYPES)
        return VK_ERROR_INITIALIZATION_FAILED;

    NvnTextureUpload *upload = calloc(1, sizeof *upload);
    if (!upload)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    upload->device = device;
    upload->queue = queue;
    upload->capacity = capacity;
    if (!bind_procs(upload, get_proc)) {
        free(upload);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = open_buffer(upload, memory);
    if (result == VK_SUCCESS)
        result = open_commands(upload, queue_family);
    if (result == VK_SUCCESS)
        *out = upload;
    else
        nvn_texture_upload_close(&upload);
    return result;
}

VkResult nvn_texture_upload_close(NvnTextureUpload **upload_ptr)
{
    if (!upload_ptr)
        return VK_ERROR_INITIALIZATION_FAILED;
    NvnTextureUpload *upload = *upload_ptr;
    if (!upload)
        return VK_SUCCESS;
    if (upload->pending)
        return VK_NOT_READY;

    if (upload->fence)
        upload->DestroyFence(upload->device, upload->fence, NULL);
    if (upload->pool)
        upload->DestroyCommandPool(upload->device, upload->pool, NULL);
    if (upload->mapped)
        upload->UnmapMemory(upload->device, upload->memory);
    if (upload->buffer)
        upload->DestroyBuffer(upload->device, upload->buffer, NULL);
    if (upload->memory)
        upload->FreeMemory(upload->device, upload->memory, NULL);
    free(upload);
    *upload_ptr = NULL;
    return VK_SUCCESS;
}

static bool axis_valid(int32_t offset, uint32_t span, uint32_t full, uint32_t block)
{
    if (offset < 0 || (uint32_t)offset > full || span > full - (uint32_t)offset)
        return false;
    return (uint32_t)offset % block == 0 && (span % block == 0 || span == full - (uint32_t)offset);
}

static void transition(NvnTextureUpload *upload, const NvnTextureUploadRegion *region, uint32_t layers,
                       NvnTextureImageState before, NvnTextureImageState after)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = before.access,
        .dstAccessMask = after.access,
        .oldLayout = before.layout,
        .newLayout = after.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = region->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, region->mip_level, 1, region->array_layer, layers},
    };
    upload->CmdPipelineBarrier(upload->command, before.stages, after.stages, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static VkResult record(NvnTextureUpload *upload, const NvnTextureUploadRegion *region, uint32_t layers)
{
    VkResult result = upload->ResetCommandBuffer(upload->command, 0);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = upload->BeginCommandBuffer(upload->command, &begin);
    if (result != VK_SUCCESS)
        return result;

    NvnTextureImageState before = region->before;
    if (before.layout == VK_IMAGE_LAYOUT_UNDEFINED)
        before.stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const NvnTextureImageState transfer = {
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    transition(upload, region, layers, before, transfer);
    const VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region->mip_level, region->array_layer, layers},
        .imageOffset = {region->offset.x, region->offset.y, 0},
        .imageExtent = {region->extent.width, region->extent.height, 1},
    };
    upload->CmdCopyBufferToImage(upload->command, upload->buffer, region->image, transfer.layout, 1, &copy);
    transition(upload, region, layers, transfer, region->after);
    return upload->EndCommandBuffer(upload->command);
}

static VkResult stage_rows(NvnTextureUpload *upload, const uint8_t *source,
                           const NvnTextureFootprint *footprint, size_t rows)
{
    for (size_t row = 0; row < rows; ++row)
        memcpy(upload->mapped + row * footprint->row_bytes,
               source + row * footprint->source_row_pitch, footprint->row_bytes);

    if (upload->coherent)
        return VK_SUCCESS;
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = upload->memory,
        .size = VK_WHOLE_SIZE,
    };
    return upload->FlushMappedMemoryRanges(upload->device, 1, &range);
}

VkResult nvn_texture_upload_submit(NvnTextureUpload *upload, const NvnTextureUploadRegion *region,
                                   const void *source, size_t source_size, size_t source_row_pitch)
{
    if (!upload)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (upload->pending)
        return VK_NOT_READY;
    if (upload->lost)
        return VK_ERROR_DEVICE_LOST;
    if (!region || !region->image || !source ||
        (region->before.layout == VK_IMAGE_LAYOUT_UNDEFINED ? region->before.access != 0 : !region->before.stages) ||
        region->after.layout == VK_IMAGE_LAYOUT_UNDEFINED || region->after.layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
        !region->after.stages)
        return VK_ERROR_INITIALIZATION_FAILED;

    NvnTextureFootprint footprint;
    VkResult result = nvn_texture_upload_footprint(region->format, region->extent, source_row_pitch, &footprint);
    if (result != VK_SUCCESS)
        return result;
    const uint32_t layers = region->layer_count ? region->layer_count : 1;
    if (layers - 1 > UINT32_MAX - region->array_layer || footprint.packed_bytes > upload->capacity / layers ||
        !axis_valid(region->offset.x, region->extent.width, region->mip_extent.width, footprint.block_extent.width) ||
        !axis_valid(region->offset.y, region->extent.height, region->mip_extent.height, footprint.block_extent.height))
        return VK_ERROR_INITIALIZATION_FAILED;

    const size_t rows = footprint.rows * layers;
    if (source_size < footprint.row_bytes || rows - 1 > (source_size - footprint.row_bytes) / footprint.source_row_pitch)
        return VK_ERROR_INITIALIZATION_FAILED;

    result = stage_rows(upload, source, &footprint, rows);
    if (result == VK_SUCCESS)
        result = record(upload, region, layers);
    if (result == VK_SUCCESS)
        result = upload->ResetFences(upload->device, 1, &upload->fence);
    if (result == VK_SUCCESS) {
        const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &upload->command,
        };
        result = upload->QueueSubmit(upload->queue, 1, &submit, upload->fence);
    }
    upload->pending = result == VK_SUCCESS;
    upload->lost = result == VK_ERROR_DEVICE_LOST;
    return result;
}

VkResult nvn_texture_upload_wait(NvnTextureUpload *upload, uint64_t timeout_ns)
{
    if (!upload)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (upload->lost)
        return VK_ERROR_DEVICE_LOST;
    if (!upload->pending)
        return VK_SUCCESS;

    VkResult result = upload->WaitForFences(upload->device, 1, &upload->fence, VK_TRUE, timeout_ns);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        upload->pending = false;
    upload->lost = result == VK_ERROR_DEVICE_LOST;
    return result;
}
