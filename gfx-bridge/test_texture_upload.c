/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "texture_upload.h"
#include "vk_adapter.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CAPACITY = 256 };
static const uint64_t timeout_ns = UINT64_C(5000000000);
static const NvnTextureImageState transfer_source = {
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
};

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    uint8_t *mapped;
} Readback;

static PokopiaVkAdapter adapter;
static NvnTextureUpload *upload;
static Readback readback;

static void check(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS)
        fprintf(stderr, "%s returned %d\n", operation, (int)result);
    assert(result == VK_SUCCESS);
}

#define VK(call) check((call), #call)
#define PROC(name) \
    PFN_vk##name name = (PFN_vk##name)adapter.get_device_proc(adapter.device, "vk" #name); \
    assert(name)

static VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags properties)
{
    PROC(AllocateMemory);
    for (uint32_t i = 0; i < adapter.memory.memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) ||
            (adapter.memory.memoryTypes[i].propertyFlags & properties) != properties)
            continue;
        VkDeviceMemory memory;
        const VkMemoryAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = i,
        };
        VK(AllocateMemory(adapter.device, &info, NULL, &memory));
        return memory;
    }
    assert(!"No matching Vulkan memory type");
    return VK_NULL_HANDLE;
}

static void open_readback(void)
{
    PROC(CreateBuffer);
    PROC(GetBufferMemoryRequirements);
    PROC(BindBufferMemory);
    PROC(MapMemory);
    PROC(CreateCommandPool);
    PROC(AllocateCommandBuffers);
    PROC(CreateFence);

    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = CAPACITY,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK(CreateBuffer(adapter.device, &buffer_info, NULL, &readback.buffer));
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(adapter.device, readback.buffer, &requirements);
    readback.memory = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(BindBufferMemory(adapter.device, readback.buffer, readback.memory, 0));
    void *mapped;
    VK(MapMemory(adapter.device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    readback.mapped = mapped;

    const VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = adapter.family,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VK(CreateCommandPool(adapter.device, &pool, NULL, &readback.pool));
    const VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = readback.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VK(AllocateCommandBuffers(adapter.device, &commands, &readback.command));
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(CreateFence(adapter.device, &fence, NULL, &readback.fence));
}

static void close_readback(void)
{
    PROC(DestroyFence);
    PROC(DestroyCommandPool);
    PROC(UnmapMemory);
    PROC(DestroyBuffer);
    PROC(FreeMemory);
    DestroyFence(adapter.device, readback.fence, NULL);
    DestroyCommandPool(adapter.device, readback.pool, NULL);
    UnmapMemory(adapter.device, readback.memory);
    DestroyBuffer(adapter.device, readback.buffer, NULL);
    FreeMemory(adapter.device, readback.memory, NULL);
}

static VkImage create_image(VkFormat format, VkExtent2D extent, uint32_t mips, uint32_t layers, VkDeviceMemory *memory)
{
    PROC(CreateImage);
    PROC(GetImageMemoryRequirements);
    PROC(BindImageMemory);

    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {extent.width, extent.height, 1}, .mipLevels = mips, .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkImage image;
    VK(CreateImage(adapter.device, &info, NULL, &image));
    VkMemoryRequirements requirements;
    GetImageMemoryRequirements(adapter.device, image, &requirements);
    *memory = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(BindImageMemory(adapter.device, image, *memory, 0));
    return image;
}

static void destroy_image(VkImage image, VkDeviceMemory memory)
{
    PROC(DestroyImage);
    PROC(FreeMemory);
    DestroyImage(adapter.device, image, NULL);
    FreeMemory(adapter.device, memory, NULL);
}

static void check_image(const NvnTextureUploadRegion *region, const uint8_t *expected, size_t size)
{
    PROC(ResetCommandBuffer);
    PROC(BeginCommandBuffer);
    PROC(CmdCopyImageToBuffer);
    PROC(CmdPipelineBarrier);
    PROC(EndCommandBuffer);
    PROC(ResetFences);
    PROC(QueueSubmit);
    PROC(WaitForFences);

    assert(size < CAPACITY);
    memset(readback.mapped, 0xa9, CAPACITY);
    VK(ResetCommandBuffer(readback.command, 0));
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(BeginCommandBuffer(readback.command, &begin));
    const VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region->mip_level, region->array_layer, 1},
        .imageExtent = {region->mip_extent.width, region->mip_extent.height, 1},
    };
    CmdCopyImageToBuffer(readback.command, region->image, transfer_source.layout, readback.buffer, 1, &copy);

    const VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = readback.buffer, .size = VK_WHOLE_SIZE,
    };
    CmdPipelineBarrier(readback.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 0, NULL, 1, &host_read, 0, NULL);
    VK(EndCommandBuffer(readback.command));
    VK(ResetFences(adapter.device, 1, &readback.fence));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &readback.command,
    };
    VK(QueueSubmit(adapter.queue, 1, &submit, readback.fence));
    VK(WaitForFences(adapter.device, 1, &readback.fence, VK_TRUE, timeout_ns));

    assert(memcmp(readback.mapped, expected, size) == 0);
    for (size_t i = size; i < CAPACITY; ++i)
        assert(readback.mapped[i] == 0xa9);
}

static void check_footprint(void)
{
    NvnTextureFootprint shape;
    VK(nvn_texture_upload_footprint(VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){6, 5}, 29, &shape));
    assert(shape.block_extent.width == 1 && shape.block_extent.height == 1 && shape.bytes_per_block == 4);
    assert(shape.row_bytes == 24 && shape.rows == 5 && shape.required_source_bytes == 140 && shape.packed_bytes == 120);
    VK(nvn_texture_upload_footprint(VK_FORMAT_BC4_SNORM_BLOCK, (VkExtent2D){9, 7}, 29, &shape));
    assert(shape.block_extent.width == 4 && shape.block_extent.height == 4 && shape.bytes_per_block == 8);
    assert(shape.row_bytes == 24 && shape.rows == 2 && shape.required_source_bytes == 53 && shape.packed_bytes == 48);
    VK(nvn_texture_upload_footprint(VK_FORMAT_BC7_SRGB_BLOCK, (VkExtent2D){9, 7}, 0, &shape));
    assert(shape.bytes_per_block == 16 && shape.source_row_pitch == 48 && shape.required_source_bytes == 96);
    assert(nvn_texture_upload_footprint(VK_FORMAT_D32_SFLOAT, (VkExtent2D){1, 1}, 0, &shape) ==
           VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(nvn_texture_upload_footprint(VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){6, 5}, 23, &shape) ==
           VK_ERROR_INITIALIZATION_FAILED);
    assert(nvn_texture_upload_footprint(VK_FORMAT_BC7_UNORM_BLOCK, (VkExtent2D){9, 7}, SIZE_MAX, &shape) ==
           VK_ERROR_INITIALIZATION_FAILED);
    assert(nvn_texture_upload_footprint(VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){UINT32_MAX, UINT32_MAX}, 0, &shape) ==
           VK_ERROR_INITIALIZATION_FAILED);
}

static void fill_rows(uint8_t *source, const NvnTextureFootprint *shape, uint8_t seed)
{
    memset(source, 0xee, shape->required_source_bytes);
    for (size_t row = 0; row < shape->rows; ++row) {
        uint8_t *data = source + row * shape->source_row_pitch;
        for (size_t column = 0; column < shape->row_bytes; ++column)
            data[column] = (uint8_t)(seed + row * 39 + column * 11);
        /* Valid BC7 mode 6, also harmless for RGBA and the other BC formats. */
        if (shape->bytes_per_block == 16) {
            for (size_t column = 0; column < shape->row_bytes; column += 16)
                data[column] = 0x40;
        }
    }
}

static void merge_expected(uint8_t *expected, size_t destination_pitch, VkOffset2D offset,
                           const uint8_t *source, const NvnTextureFootprint *shape)
{
    const size_t x = (uint32_t)offset.x / shape->block_extent.width * shape->bytes_per_block;
    const size_t y = (uint32_t)offset.y / shape->block_extent.height;
    for (size_t row = 0; row < shape->rows; ++row)
        memcpy(expected + (y + row) * destination_pitch + x,
               source + row * shape->source_row_pitch, shape->row_bytes);
}

static void test_texture(VkFormat format, bool compressed, const char *label)
{
    const VkExtent2D extent = compressed ? (VkExtent2D){9, 7} : (VkExtent2D){6, 5};
    const VkExtent2D image_extent = compressed ? extent : (VkExtent2D){12, 10};
    VkDeviceMemory memory;
    const VkImage image = create_image(format, image_extent, compressed ? 1 : 2, compressed ? 1 : 2, &memory);
    NvnTextureUploadRegion region = {
        .image = image, .format = format, .mip_extent = extent, .extent = extent,
        .mip_level = compressed ? 0 : 1, .array_layer = compressed ? 0 : 1,
        .after = transfer_source,
    };
    NvnTextureFootprint shape;
    VK(nvn_texture_upload_footprint(format, extent, 0, &shape));
    const size_t tight_size = shape.packed_bytes;
    const size_t tight_pitch = shape.row_bytes;
    VK(nvn_texture_upload_footprint(format, extent, tight_pitch + 5, &shape));

    uint8_t source[CAPACITY], expected[CAPACITY] = {0};
    assert(shape.required_source_bytes <= sizeof source);
    fill_rows(source, &shape, 0x13);
    merge_expected(expected, tight_pitch, region.offset, source, &shape);
    assert(nvn_texture_upload_submit(upload, &region, source, shape.required_source_bytes - 1,
                                     shape.source_row_pitch) == VK_ERROR_INITIALIZATION_FAILED);
    VK(nvn_texture_upload_submit(upload, &region, source, shape.required_source_bytes, shape.source_row_pitch));
    assert(nvn_texture_upload_submit(upload, &region, source, sizeof source, shape.source_row_pitch) == VK_NOT_READY);
    assert(nvn_texture_upload_close(&upload) == VK_NOT_READY);
    memset(source, 0x22, sizeof source);
    const VkResult poll = nvn_texture_upload_wait(upload, 0);
    assert(poll == VK_SUCCESS || poll == VK_TIMEOUT);
    VK(nvn_texture_upload_wait(upload, timeout_ns));
    check_image(&region, expected, tight_size);

    region.before = transfer_source;
    region.offset = compressed ? (VkOffset2D){4, 4} : (VkOffset2D){2, 1};
    region.extent = compressed ? (VkExtent2D){5, 3} : (VkExtent2D){3, 2};
    VK(nvn_texture_upload_footprint(format, region.extent, 0, &shape));
    VK(nvn_texture_upload_footprint(format, region.extent, shape.row_bytes + 3, &shape));
    fill_rows(source, &shape, 0x96);
    merge_expected(expected, tight_pitch, region.offset, source, &shape);

    NvnTextureUploadRegion invalid = region;
    invalid.offset.x = compressed ? 3 : -1;
    assert(nvn_texture_upload_submit(upload, &invalid, source, sizeof source, shape.source_row_pitch) ==
           VK_ERROR_INITIALIZATION_FAILED);
    invalid = region;
    invalid.extent.width = compressed ? 3 : 5;
    assert(nvn_texture_upload_submit(upload, &invalid, source, sizeof source, shape.source_row_pitch) ==
           VK_ERROR_INITIALIZATION_FAILED);
    VK(nvn_texture_upload_submit(upload, &region, source, shape.required_source_bytes, shape.source_row_pitch));
    VK(nvn_texture_upload_wait(upload, timeout_ns));
    check_image(&region, expected, tight_size);

    printf("%s %ux%u, %zu exact bytes after pitched full and partial uploads\n",
           label, extent.width, extent.height, tight_size);
    destroy_image(image, memory);
}

int main(void)
{
    check_footprint();
    assert(pokopia_vk_open(&adapter, stdout));
    assert(adapter.features.textureCompressionBC);
    VK(nvn_texture_upload_open(&upload, adapter.device, adapter.queue, adapter.family, &adapter.memory,
                               adapter.get_device_proc, CAPACITY));
    open_readback();
    test_texture(VK_FORMAT_R8G8B8A8_UNORM, false, "RGBA8 mip 1 layer 1");
    test_texture(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true, "BC1 edge blocks");
    test_texture(VK_FORMAT_BC7_UNORM_BLOCK, true, "BC7 edge blocks");
    close_readback();
    VK(nvn_texture_upload_close(&upload));
    assert(!upload);
    VK(nvn_texture_upload_close(&upload));
    pokopia_vk_close(&adapter);
    puts("PASS Vulkan texture uploads, footprint validation, and fence lifetime");
    return 0;
}
