/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "texture_upload.h"
#include "vk_adapter.h"
#include "live_texture_upload.h"
#include "nvn_guest_layout.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

/* Substitute the object lookup and guest address translation, exercising the
 * production allocator and texel dispatch against a real Vulkan device. */
enum {
    LIVE_TEXTURE = 0x1000, LIVE_VIEW = 0x2000, LIVE_REGION = 0x3000,
    LIVE_SOURCE = 0x4000, LIVE_STORAGE = 0x5000,
};

typedef struct {
    NvnFrontTextureInfo texture;
    uint8_t view[16];
    uint32_t region[6], source;
    uint8_t storage[512];
} LiveGuest;

bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info)
{
    if (address != LIVE_TEXTURE)
        return false;
    *info = ((LiveGuest *)front)->texture;
    return true;
}

static void *live_write(void *opaque, uint64_t address, size_t size)
{
    LiveGuest *guest = opaque;
    switch (address) {
    case LIVE_VIEW: return size == sizeof guest->view ? guest->view : NULL;
    case LIVE_REGION: return size == sizeof guest->region ? guest->region : NULL;
    case LIVE_SOURCE: return size == sizeof guest->source ? &guest->source : NULL;
    case LIVE_STORAGE: return size == sizeof guest->storage ? guest->storage : NULL;
    default: return NULL;
    }
}

static const void *live_read(void *opaque, uint64_t address, size_t size)
{
    return live_write(opaque, address, size);
}

static void check_live_single_texel(bool array)
{
    PokopiaVkTexture texture = {0};
    LiveGuest guest = {
        .texture = {
            .native = &texture,
            .create_info = {
                .config = {
                    .target = array ? 4 : 1, .width = 1, .height = 1, .depth = 1, .levels = 1,
                    .format = array ? 0x25 : 0xa, .flags = 0x4, .swizzle = {2, 3, 4, 5},
                },
                .storage_address = LIVE_STORAGE,
            },
        },
        .view = {[2] = 1, [5] = 1, [7] = 3},
        .region = {0, 0, array ? 17 : 0, 1, 1, array ? 0 : 1},
        .source = 0x3fa00000,
    };
    NvnFrontTextureCreateInfo *info = &guest.texture.create_info;
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    assert(info->storage_size == sizeof guest.storage && info->storage_alignment == 512);
    VK(pokopia_vk_texture_open(&adapter, &info->config, &texture));
    assert(texture.format == (array ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_SFLOAT) && texture.storage_view);
    assert(!array || texture.sample_view);

    PokopiaLiveTextureUpload live = {
        .front = (NvnFront *)&guest, .adapter = &adapter, .guest_opaque = &guest,
        .read_guest = live_read, .write_guest = live_write, .log = stdout,
    };
    const uint64_t args[32] = {LIVE_TEXTURE, LIVE_VIEW, LIVE_REGION, LIVE_SOURCE};
    memset(guest.storage, 0x5a, sizeof guest.storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    assert(texture.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(memcmp(guest.storage, &guest.source, sizeof guest.source) == 0);
    memset(guest.storage, 0x5a, sizeof guest.source);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(memcmp(guest.storage, &guest.source, sizeof guest.source) == 0);

    guest.source = 0xc0200000;
    memcpy(guest.storage, &guest.source, sizeof guest.source);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureFlushTexels", args) == NVN_FRONT_HANDLED);
    memset(guest.storage, 0x5a, sizeof guest.source);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(memcmp(guest.storage, &guest.source, sizeof guest.source) == 0);
    for (size_t i = sizeof guest.source; i < sizeof guest.storage; ++i)
        assert(guest.storage[i] == 0x5a);

    VK(pokopia_live_texture_upload_close(&live));
    pokopia_vk_texture_close(&adapter, &texture);
    puts(array ? "PASS RGBA8 1x1x1 array NVN write, flush and invalidate exact byte round trips"
               : "PASS R32F 1x1 storage image NVN write, flush and invalidate exact byte round trips");
}

enum { TEST_MAX_LAYERS = 12, TEST_LAYER_STRIDE = 512 };

typedef struct {
    LiveGuest guest;
    uint8_t storage[TEST_MAX_LAYERS * TEST_LAYER_STRIDE];
} CubeGuest;

static void *cube_write(void *opaque, uint64_t address, size_t size)
{
    CubeGuest *cube = opaque;
    if (address == LIVE_STORAGE)
        return size == cube->guest.texture.create_info.storage_size && size <= sizeof cube->storage ? cube->storage : NULL;
    return live_write(&cube->guest, address, size);
}

static const void *cube_read(void *opaque, uint64_t address, size_t size)
{
    return cube_write(opaque, address, size);
}

static void check_layer_storage(const CubeGuest *cube, const uint32_t values[TEST_MAX_LAYERS][4], uint8_t padding)
{
    const NvnFrontTextureConfig *config = &cube->guest.texture.create_info.config;
    const size_t row_bytes = config->width * sizeof(uint32_t);
    for (size_t layer = 0; layer < config->depth; ++layer) {
        uint8_t expected[TEST_LAYER_STRIDE];
        memset(expected, padding, sizeof expected);
        for (size_t row = 0; row < config->height; ++row)
            memcpy(expected + row * 16, values[layer] + row * config->width, row_bytes);
        assert(memcmp(cube->storage + layer * TEST_LAYER_STRIDE, expected, sizeof expected) == 0);
    }
}

static void check_live_layers(uint32_t target, uint32_t size, uint32_t depth, const char *label)
{
    PokopiaVkTexture texture = {0};
    CubeGuest cube = {.guest = {
        .texture = {
            .native = &texture,
            .create_info = {
                .config = {
                    .target = target, .width = size, .height = size, .depth = depth, .levels = 1,
                    .format = 0x25, .flags = 4, .swizzle = {2, 3, 4, 5},
                },
                .storage_address = LIVE_STORAGE,
            },
        },
        .view = {[0] = (uint8_t)(depth - 2), [2] = 1, [5] = 1, [7] = 3},
        .region = {size - 1, size - 1, 17, 1, 1, 0},
        .source = 0x1234abcd,
    }};
    LiveGuest *guest = &cube.guest;
    NvnFrontTextureCreateInfo *info = &guest->texture.create_info;
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    assert(info->storage_size == depth * TEST_LAYER_STRIDE && info->storage_alignment == TEST_LAYER_STRIDE);
    VK(pokopia_vk_texture_open(&adapter, &info->config, &texture));
    assert(texture.format == VK_FORMAT_R8G8B8A8_UNORM && texture.view && texture.sample_view && texture.storage_view);
    PokopiaLiveTextureUpload live = {
        .front = (NvnFront *)guest, .adapter = &adapter, .guest_opaque = &cube,
        .read_guest = cube_read, .write_guest = cube_write, .log = stdout,
    };
    const uint64_t args[32] = {LIVE_TEXTURE, LIVE_VIEW, LIVE_REGION, LIVE_SOURCE};
    uint32_t values[TEST_MAX_LAYERS][4];
    memset(cube.storage, 0x5a, sizeof cube.storage);
    for (size_t layer = 0; layer < depth; ++layer) {
        for (size_t texel = 0; texel < size * size; ++texel)
            values[layer][texel] = (uint32_t)((layer + 1) * 100 + texel);
        for (size_t row = 0; row < size; ++row)
            memcpy(cube.storage + layer * TEST_LAYER_STRIDE + row * 16, values[layer] + row * size,
                   size * sizeof(uint32_t));
    }
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    values[depth - 2][size * size - 1] = guest->source;
    assert(texture.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    check_layer_storage(&cube, values, 0x5a);

    guest->view[0] = 1;
    guest->source = 0xdcba4321;
    guest->region[0] = 0;
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    values[1][(size - 1) * size] = guest->source;
    check_layer_storage(&cube, values, 0x5a);
    guest->view[0] = (uint8_t)depth;
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_INVALID_GUEST);
    check_layer_storage(&cube, values, 0x5a);

    memset(cube.storage, 0xa5, sizeof cube.storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    check_layer_storage(&cube, values, 0xa5);

    values[depth - 1][0] = 0x11223344;
    memcpy(cube.storage + (depth - 1) * TEST_LAYER_STRIDE, values[depth - 1], 4);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureFlushTexels", args) == NVN_FRONT_HANDLED);
    memset(cube.storage, 0x3c, sizeof cube.storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    check_layer_storage(&cube, values, 0x3c);

    VK(pokopia_live_texture_upload_close(&live));
    pokopia_vk_texture_close(&adapter, &texture);
    printf("PASS %s, layer-selected writes, flush, invalidate, and guest padding\n", label);
}


typedef struct {
    LiveGuest guest;
    uint32_t source[4];
} RgbaFloatGuest;

static void *rgba32f_map(void *opaque, uint64_t address, size_t size)
{
    RgbaFloatGuest *image = opaque;
    if (address == LIVE_SOURCE)
        return size == sizeof image->source ? image->source : NULL;
    if (address == LIVE_STORAGE)
        return size == image->guest.texture.create_info.storage_size && size <= sizeof image->guest.storage
            ? image->guest.storage : NULL;
    return live_write(&image->guest, address, size);
}

static const void *rgba32f_read(void *opaque, uint64_t address, size_t size)
{
    return rgba32f_map(opaque, address, size);
}

static void check_live_rgba32f(bool linear)
{
    PokopiaVkTexture texture = {0};
    RgbaFloatGuest image = {.guest = {
        .texture = {
            .native = &texture,
            .create_info = {
                .config = {
                    .target = 1, .width = 2, .height = 1, .depth = 1, .levels = 1,
                    .format = 0x2e, .flags = linear ? 0x14 : 4, .swizzle = {2, 3, 4, 5},
                },
                .storage_address = LIVE_STORAGE,
            },
        },
        .view = {[2] = 1, [5] = 1, [7] = 3},
        .region = {1, 0, 0, 1, 1, 1},
    }, .source = {0x3f800000, 0x7fc0a5a5, 0x80000000, 0x00000001}};
    LiveGuest *guest = &image.guest;
    NvnFrontTextureCreateInfo *info = &guest->texture.create_info;
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    assert(info->storage_size == (linear ? 32 : 512));
    VK(pokopia_vk_texture_open(&adapter, &info->config, &texture));
    assert(texture.format == VK_FORMAT_R32G32B32A32_SFLOAT && texture.storage_view);

    PokopiaLiveTextureUpload live = {
        .front = (NvnFront *)guest, .adapter = &adapter, .guest_opaque = &image,
        .read_guest = rgba32f_read, .write_guest = rgba32f_map, .log = stdout,
    };
    const uint64_t args[32] = {LIVE_TEXTURE, LIVE_VIEW, LIVE_REGION, LIVE_SOURCE};
    const size_t second_texel = linear ? 16 : 64;
    uint32_t first[4] = {0x40000000, 0xbf800000, 0x7f800000, 0xff800000};
    uint8_t expected[sizeof guest->storage];
    memset(guest->storage, 0x5a, sizeof guest->storage);
    memcpy(guest->storage, first, sizeof first);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    memcpy(expected, guest->storage, sizeof expected);
    assert(memcmp(guest->storage, first, sizeof first) == 0);
    assert(memcmp(guest->storage + second_texel, image.source, sizeof image.source) == 0);
    for (size_t i = sizeof first; i < sizeof expected; ++i)
        if (i < second_texel || i >= second_texel + sizeof image.source)
            assert(expected[i] == 0x5a);
    memset(guest->storage, 0x5a, sizeof guest->storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(memcmp(guest->storage, expected, sizeof expected) == 0);

    first[1] = 0xc0200000;
    memcpy(guest->storage, first, sizeof first);
    memcpy(expected, first, sizeof first);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureFlushTexels", args) == NVN_FRONT_HANDLED);
    memset(guest->storage, 0x5a, sizeof guest->storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(memcmp(guest->storage, expected, sizeof expected) == 0);

    VK(pokopia_live_texture_upload_close(&live));
    pokopia_vk_texture_close(&adapter, &texture);
    printf("PASS RGBA32F 2x1 %s partial write, flush, invalidate and padding\n", linear ? "pitch-linear" : "block-linear");
}

bool bc7_reference_tile(uint32_t width, uint32_t height, uint32_t gob_blocks_y,
    const uint8_t *linear, size_t linear_size, uint8_t *tiled, size_t tiled_size);

typedef struct {
    LiveGuest guest;
    const uint8_t *source;
    uint8_t *storage;
    size_t source_size, storage_size;
} Bc7Guest;

static void *bc7_write(void *opaque, uint64_t address, size_t size)
{
    Bc7Guest *guest = opaque;
    if (address == LIVE_STORAGE)
        return size == guest->storage_size ? guest->storage : NULL;
    return live_write(&guest->guest, address, size);
}

static const void *bc7_read(void *opaque, uint64_t address, size_t size)
{
    const Bc7Guest *guest = opaque;
    if (address == LIVE_SOURCE)
        return size == guest->source_size ? guest->source : NULL;
    return bc7_write(opaque, address, size);
}

static void bc7_reference(const Bc7Guest *guest, bool linear, const NvnTextureFootprint *shape,
                          const uint8_t *packed, uint8_t *expected)
{
    const NvnFrontTextureCreateInfo *info = &guest->guest.texture.create_info;
    memset(expected, 0x5a, guest->storage_size);
    if (linear) {
        for (size_t y = 0; y < shape->rows; ++y)
            memcpy(expected + y * info->config.stride, packed + y * shape->row_bytes, shape->row_bytes);
    } else {
        assert(bc7_reference_tile(info->config.width, info->config.height, (uint32_t)(info->storage_alignment / 512),
            packed, shape->packed_bytes, expected, guest->storage_size));
    }
}

static void check_live_bc7(uint32_t width, uint32_t height, bool linear, bool srgb, const uint8_t *fixture)
{
    PokopiaVkTexture texture = {0};
    Bc7Guest guest = {.guest.texture = {.native = &texture, .create_info = {
        .config = {.target = 1, .width = width, .height = height, .depth = 1, .levels = 1,
                   .format = srgb ? 0x4e : 0x4d, .flags = linear ? 0x10 : 0, .stride = linear ? 64 : 0,
                   .swizzle = {2, 3, 4, 5}},
        .storage_address = LIVE_STORAGE,
    }}};
    NvnFrontTextureCreateInfo *info = &guest.guest.texture.create_info;
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    guest.storage_size = (size_t)info->storage_size;
    guest.storage = malloc(guest.storage_size);
    uint8_t *expected = malloc(guest.storage_size);
    const VkFormat format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    VkDeviceMemory backing = VK_NULL_HANDLE;
    if (linear) {
        texture.backing.image = create_image(format, (VkExtent2D){width, height}, 1, 1, &backing);
        texture.format = format;
        texture.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    } else {
        VK(pokopia_vk_texture_open(&adapter, &info->config, &texture));
        assert(texture.format == format);
    }
    NvnTextureFootprint shape;
    VK(nvn_texture_upload_footprint(texture.format, (VkExtent2D){width, height}, 0, &shape));
    uint8_t *packed = malloc(shape.packed_bytes);
    assert(guest.storage && expected && packed);
    memcpy(packed, fixture, shape.packed_bytes);

    PokopiaLiveTextureUpload live = {
        .front = (NvnFront *)&guest, .adapter = &adapter, .guest_opaque = &guest,
        .read_guest = bc7_read, .write_guest = bc7_write, .log = stdout,
    };
    const uint64_t args[32] = {LIVE_TEXTURE, 0, LIVE_REGION, LIVE_SOURCE};
    const bool edge = width == 9;
    uint32_t *region = guest.guest.region;
    region[3] = width;
    region[4] = height;
    region[5] = 1;
    guest.source = fixture;
    guest.source_size = shape.packed_bytes;
    memset(guest.storage, 0x5a, guest.storage_size);
    if (edge) {
        /* First partial update must unpack untouched canonical blocks. */
        bc7_reference(&guest, linear, &shape, packed, guest.storage);
        region[0] = 8;
        region[1] = 4;
        region[3] = 1;
        region[4] = 3;
        guest.source_size = 16;
        memcpy(packed + shape.packed_bytes - 16, fixture, 16);
    }
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    bc7_reference(&guest, linear, &shape, packed, expected);
    assert(!memcmp(guest.storage, expected, guest.storage_size));
    memset(guest.storage, 0x5a, guest.storage_size);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(!memcmp(guest.storage, expected, guest.storage_size));

    if (edge) {
        region[0] = 5;
        assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_INVALID_GUEST);
        region[0] = 4;
        assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_INVALID_GUEST);
        region[0] = 8;
        region[1] = 1;
        assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_INVALID_GUEST);
        region[1] = 0;
        assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_INVALID_GUEST);
        assert(!memcmp(guest.storage, expected, guest.storage_size));
    }

    /* Flush a different canonical block, then overwrite one live Vulkan region. */
    memcpy(packed, fixture + 16, 16);
    bc7_reference(&guest, linear, &shape, packed, guest.storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureFlushTexels", args) == NVN_FRONT_HANDLED);
    region[0] = edge ? 8 : 12;
    region[1] = edge ? 4 : 8;
    region[3] = edge ? 1 : 4;
    region[4] = edge ? 3 : 4;
    guest.source = fixture + 32;
    guest.source_size = 16;
    memcpy(packed + (region[1] / 4) * shape.row_bytes + (region[0] / 4) * 16, guest.source, 16);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    bc7_reference(&guest, linear, &shape, packed, expected);
    assert(!memcmp(guest.storage, expected, guest.storage_size));
    memset(guest.storage, 0x5a, guest.storage_size);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(!memcmp(guest.storage, expected, guest.storage_size));

    VK(pokopia_live_texture_upload_close(&live));
    if (linear)
        destroy_image(texture.backing.image, backing);
    else
        pokopia_vk_texture_close(&adapter, &texture);
    printf("PASS BC7 %ux%u %s %s write/flush/readback versus reference, %zu packed bytes\n",
        width, height, linear ? "pitch-linear" : "Tegra", srgb ? "sRGB" : "UNORM", shape.packed_bytes);
    free(packed);
    free(expected);
    free(guest.storage);
}

static void check_bc7_fixture(void)
{
    enum { ATLAS_BYTES = 2097152 };
    FILE *file = NULL;
    assert(fopen_s(&file, "../../frame-reference-655360/loading-replay/draw4-bc7.bin", "rb") == 0);
    uint8_t *fixture = malloc(ATLAS_BYTES);
    assert(file && fixture && fread(fixture, 1, ATLAS_BYTES, file) == ATLAS_BYTES && fgetc(file) == EOF);
    fclose(file);
    check_live_bc7(1024, 2048, false, false, fixture);
    check_live_bc7(9, 7, false, true, fixture);
    check_live_bc7(9, 7, true, true, fixture);
    free(fixture);
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
    check_live_single_texel(false);
    check_live_single_texel(true);
    check_live_rgba32f(false);
    check_live_rgba32f(true);
    check_live_layers(8, 2, 6, "six-face cubemap");
    check_live_layers(4, 2, 6, "six-layer 2D array");
    check_live_layers(9, 1, 6, "1x1x6 RGBA8 cube array");
    check_live_layers(9, 1, 12, "two-cube 1x1x12 RGBA8 cube array");
    check_bc7_fixture();
    close_readback();
    VK(nvn_texture_upload_close(&upload));
    assert(!upload);
    VK(nvn_texture_upload_close(&upload));
    pokopia_vk_close(&adapter);
    puts("PASS Vulkan texture uploads, footprint validation, and fence lifetime");
    return 0;
}

