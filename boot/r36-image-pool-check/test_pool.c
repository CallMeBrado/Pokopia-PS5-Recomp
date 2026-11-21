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

static PFN_vkGetDeviceProcAddr original_proc;
static VkDeviceSize bound_offset;
static VkDeviceSize requested_alignment;
static VkDeviceSize test_alignment;
static bool test_requires_dedicated, reject_pool_allocation, reject_bind;

static VKAPI_ATTR VkResult VKAPI_CALL record_bind(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                  VkDeviceSize offset)
{
    PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)original_proc(device, "vkBindImageMemory");
    if (reject_bind)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    bound_offset = offset;
    assert(!requested_alignment || offset % requested_alignment == 0);
    return bind(device, image, memory, offset);
}

static VKAPI_ATTR void VKAPI_CALL record_requirements(VkDevice device, const VkImageMemoryRequirementsInfo2 *query,
                                                      VkMemoryRequirements2 *requirements)
{
    PFN_vkGetImageMemoryRequirements2 original =
        (PFN_vkGetImageMemoryRequirements2)original_proc(device, "vkGetImageMemoryRequirements2");
    original(device, query, requirements);
    if (requirements->memoryRequirements.alignment < test_alignment)
        requirements->memoryRequirements.alignment = test_alignment;
    if (test_requires_dedicated) {
        VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
        assert(dedicated);
        dedicated->requiresDedicatedAllocation = dedicated->prefersDedicatedAllocation = VK_TRUE;
    }
    requested_alignment = requirements->memoryRequirements.alignment;
}

static VKAPI_ATTR VkResult VKAPI_CALL record_allocate(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *callbacks, VkDeviceMemory *memory)
{
    if (reject_pool_allocation && !info->pNext && info->allocationSize == 1024 * 1024)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)original_proc(device, "vkAllocateMemory");
    return allocate(device, info, callbacks, memory);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL record_proc(VkDevice device, const char *name)
{
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)record_allocate;
    if (strcmp(name, "vkBindImageMemory") == 0)
        return (PFN_vkVoidFunction)record_bind;
    if (strcmp(name, "vkGetImageMemoryRequirements2") == 0)
        return (PFN_vkVoidFunction)record_requirements;
    return original_proc(device, name);
}

static NvnTextureUploadRegion write_pooled(PokopiaVkTexture *texture, uint8_t seed, uint8_t expected[CAPACITY],
                                           size_t *bytes)
{
    NvnTextureUploadRegion region = {
        .image = texture->backing.image, .format = texture->format,
        .mip_extent = texture->extent, .extent = texture->extent, .after = transfer_source,
    };
    NvnTextureFootprint shape;
    VK(nvn_texture_upload_footprint(texture->format, texture->extent, 0, &shape));
    uint8_t source[CAPACITY];
    assert(shape.packed_bytes <= sizeof source);
    fill_rows(source, &shape, seed);
    memcpy(expected, source, shape.packed_bytes);
    *bytes = shape.packed_bytes;
    VK(nvn_texture_upload_submit(upload, &region, source, shape.packed_bytes, shape.source_row_pitch));
    VK(nvn_texture_upload_wait(upload, timeout_ns));
    return region;
}

static void test_image_pool(void)
{
    const NvnFrontTextureConfig bc = {
        .target = 1, .format = 0x4d, .width = 9, .height = 7, .depth = 1, .levels = 1,
        .flags = 4, .swizzle = {2, 3, 4, 5},
    };
    NvnFrontTextureConfig rgba = bc;
    rgba.format = 0x25;
    rgba.width = 6;
    rgba.height = 5;
    PokopiaVkTexture a = {0}, b = {0}, c = {0}, recycled = {0}, dedicated = {0}, large = {0};
    original_proc = adapter.get_device_proc;
    adapter.get_device_proc = record_proc;
    VK(pokopia_vk_texture_open(&adapter, &bc, &a));
    assert(a.backing.span && bound_offset == 0);
    const VkDeviceSize bc_alignment = requested_alignment;
    test_alignment = bc_alignment * 4;
    VK(pokopia_vk_texture_open(&adapter, &rgba, &b));
    assert(b.backing.span && a.backing.memory == b.backing.memory && bound_offset >= a.backing.bytes);
    const VkDeviceSize rgba_offset = bound_offset, rgba_alignment = requested_alignment;
    test_alignment = 0;
    VK(pokopia_vk_texture_open(&adapter, &bc, &c));
    assert(c.backing.span && a.backing.memory == c.backing.memory && bound_offset != rgba_offset);
    assert(bound_offset >= a.backing.bytes);

    uint8_t expected[4][CAPACITY];
    size_t sizes[4];
    NvnTextureUploadRegion regions[4];
    regions[0] = write_pooled(&a, 0x11, expected[0], &sizes[0]);
    regions[1] = write_pooled(&b, 0x22, expected[1], &sizes[1]);
    regions[2] = write_pooled(&c, 0x33, expected[2], &sizes[2]);
    for (unsigned i = 0; i < 3; ++i)
        check_image(&regions[i], expected[i], sizes[i]);

    pokopia_vk_texture_close(&adapter, &b);
    test_alignment = bc_alignment * 4;
    VK(pokopia_vk_texture_open(&adapter, &rgba, &recycled));
    assert(recycled.backing.memory == a.backing.memory && bound_offset == rgba_offset);
    regions[3] = write_pooled(&recycled, 0x44, expected[3], &sizes[3]);
    check_image(&regions[3], expected[3], sizes[3]);
    check_image(&regions[0], expected[0], sizes[0]);
    check_image(&regions[2], expected[2], sizes[2]);

    test_requires_dedicated = true;
    VK(pokopia_vk_texture_open(&adapter, &rgba, &dedicated));
    assert(!dedicated.backing.span && dedicated.backing.memory != a.backing.memory && bound_offset == 0);
    test_requires_dedicated = false;
    test_alignment = 0;
    NvnFrontTextureConfig big = rgba;
    big.width = big.height = 512;
    VK(pokopia_vk_texture_open(&adapter, &big, &large));
    assert(!large.backing.span && large.backing.memory != a.backing.memory && bound_offset == 0);
    pokopia_vk_texture_close(&adapter, &a);
    pokopia_vk_texture_close(&adapter, &large);
    pokopia_vk_texture_close(&adapter, &c);
    pokopia_vk_texture_close(&adapter, &dedicated);
    assert(adapter.image_blocks);
    pokopia_vk_texture_close(&adapter, &recycled);
    assert(!adapter.image_blocks);
    reject_bind = true;
    assert(pokopia_vk_texture_open(&adapter, &rgba, &a) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(!adapter.image_blocks && !a.backing.image);
    reject_bind = false;
    reject_pool_allocation = true;
    VK(pokopia_vk_texture_open(&adapter, &rgba, &a));
    assert(!adapter.image_blocks && a.backing.memory && !a.backing.span);
    reject_pool_allocation = false;
    pokopia_vk_texture_close(&adapter, &a);
    adapter.get_device_proc = original_proc;
    printf("PASS pool BC7/RGBA8 exact readback, alignments %llu/%llu, offset %llu recycled, dedicated paths\n",
           (unsigned long long)bc_alignment, (unsigned long long)rgba_alignment, (unsigned long long)rgba_offset);
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
    test_image_pool();
    close_readback();
    VK(nvn_texture_upload_close(&upload));
    assert(!upload);
    VK(nvn_texture_upload_close(&upload));
    pokopia_vk_close(&adapter);
    puts("PASS Vulkan texture uploads, footprint validation, and fence lifetime");
    return 0;
}
