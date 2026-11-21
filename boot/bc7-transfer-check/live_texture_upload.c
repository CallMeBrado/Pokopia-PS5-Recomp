/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_texture_upload.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEXTURE_2D = 1, TEXTURE_3D = 2, TEXTURE_2D_ARRAY = 4, TEXTURE_CUBE = 8, TEXTURE_CUBE_ARRAY = 9,
    CUBE_FACES = 6, TEXTURE_LINEAR = 0x10, TEXTURE_SPARSE = 0x20,
    VIEW_LEVELS = 1, VIEW_LAYERS = 2, VIEW_BYTES = 16, REGION_BYTES = 24,
    GOB_BYTES = 512, GOB_WIDTH = 64, GOB_ROWS = 8, GOB_PIECE_BYTES = 16, VOLUME_BLOCK_DEPTH = 16,
};
static const uint64_t upload_timeout = UINT64_C(5000000000);

typedef struct {
    uint32_t x, y, width, height;
} Region;

typedef struct {
    NvnTextureFootprint footprint;
    size_t layer_stride, pitch, gob_columns, gobs_high, block_bytes;
    uint32_t layers;
    bool layered;
} Storage;

static uint32_t guest_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool texture_storage(const NvnFrontTextureCreateInfo *info, VkFormat format, Storage *out)
{
    const NvnFrontTextureConfig *config = &info->config;
    const bool volume = config->target == TEXTURE_3D;
    const bool cube = config->target == TEXTURE_CUBE;
    const bool cube_array = config->target == TEXTURE_CUBE_ARRAY;
    const bool array = config->target == TEXTURE_2D_ARRAY || cube_array;
    const uint32_t layers = cube ? CUBE_FACES : array ? config->depth : 1;
    const uint32_t max_edge = volume ? 2048 : 16384;
    const bool valid_depth = array || config->depth == 1 || (cube && config->depth == CUBE_FACES);
    if ((!volume && !cube && !array && config->target != TEXTURE_2D) || !config->width || config->width > max_edge ||
        !config->height || config->height > max_edge || config->levels != 1 || !layers || !valid_depth ||
        ((cube || cube_array) && config->width != config->height) || (cube_array && layers % CUBE_FACES) ||
        config->samples > 1 || config->packaged_layout || config->packaged_data ||
        config->gl_texture_name || (config->flags & TEXTURE_SPARSE) || !info->storage_address ||
        !info->storage_size || info->storage_size > SIZE_MAX)
        return false;

    NvnTextureFootprint footprint;
    if (nvn_texture_upload_footprint(format, (VkExtent2D){config->width, config->height}, 0, &footprint) != VK_SUCCESS ||
        ((footprint.block_extent.width != 1 || footprint.block_extent.height != 1) &&
         format != VK_FORMAT_BC7_UNORM_BLOCK && format != VK_FORMAT_BC7_SRGB_BLOCK) ||
        GOB_PIECE_BYTES % footprint.bytes_per_block)
        return false;

    const size_t row_bytes = footprint.row_bytes;
    Storage storage = {.footprint = footprint, .layers = layers, .layered = cube || array};
    if (config->flags & TEXTURE_LINEAR) {
        storage.pitch = config->stride ? config->stride : ((row_bytes + 31) & ~(size_t)31);
        if (storage.pitch < row_bytes || storage.pitch > SIZE_MAX / footprint.rows)
            return false;
        storage.layer_stride = storage.pitch * footprint.rows;
    } else {
        const uint64_t alignment = info->storage_alignment;
        if (config->stride || alignment < GOB_BYTES || alignment > 32 * GOB_BYTES ||
            (alignment & (alignment - 1)))
            return false;
        /* Pinned alignment covers one vertical group of GOBs. A nonpackaged
         * volume pads its first slice with 15 more GOB groups in each block. */
        storage.gobs_high = (size_t)alignment / GOB_BYTES;
        storage.gob_columns = (row_bytes + GOB_WIDTH - 1) / GOB_WIDTH;
        storage.block_bytes = (size_t)alignment * (volume ? VOLUME_BLOCK_DEPTH : 1);
        const size_t block_height = GOB_ROWS * storage.gobs_high;
        const size_t block_rows = (footprint.rows + block_height - 1) / block_height;
        storage.layer_stride = storage.gob_columns * storage.block_bytes * block_rows;
    }
    /* At one mip, each array layer or cube face has the same pool stride. */
    if (info->storage_size % layers || storage.layer_stride != info->storage_size / layers)
        return false;
    *out = storage;
    return true;
}

static size_t gob_offset(const Storage *storage, uint32_t x, uint32_t y)
{
    const size_t byte_x = (size_t)x * storage->footprint.bytes_per_block;
    const size_t gob_y = y / GOB_ROWS;
    const size_t block = (gob_y / storage->gobs_high) * storage->gob_columns + byte_x / GOB_WIDTH;
    const size_t base = block * storage->block_bytes + (gob_y % storage->gobs_high) * GOB_BYTES;
    const size_t piece = byte_x % GOB_WIDTH / GOB_PIECE_BYTES;
    const size_t row = y % GOB_ROWS;
    /* NVN2 uses the Switch 2 GOB piece order from pinned tegra_tiling.cpp. */
    return base + (piece >> 1) * 256 + (row >> 2) * 128 + (piece & 1) * 64 +
           ((row >> 1) & 1) * 32 + (row & 1) * GOB_PIECE_BYTES + byte_x % GOB_PIECE_BYTES;
}

static void unpack_level(const Storage *storage, const uint8_t *guest, uint8_t *linear)
{
    const size_t row_bytes = storage->footprint.row_bytes;
    const size_t bytes_per_block = storage->footprint.bytes_per_block;
    const size_t columns = row_bytes / bytes_per_block;
    for (uint32_t y = 0; y < storage->footprint.rows; ++y) {
        uint8_t *row = linear + y * row_bytes;
        if (storage->pitch) {
            memcpy(row, guest + y * storage->pitch, row_bytes);
        } else {
            for (uint32_t x = 0; x < columns; ++x)
                memcpy(row + (size_t)x * bytes_per_block, guest + gob_offset(storage, x, y), bytes_per_block);
        }
    }
}

/* The CPU storage rectangle counts blocks, even when Vulkan's copy uses pixels. */
static void pack_region(const Storage *storage, Region blocks, const uint8_t *linear,
                        size_t source_pitch, uint8_t *guest)
{
    const size_t bytes_per_block = storage->footprint.bytes_per_block;
    for (uint32_t y = 0; y < blocks.height; ++y) {
        const uint8_t *row = linear + y * source_pitch;
        if (storage->pitch) {
            memcpy(guest + (blocks.y + y) * storage->pitch + (size_t)blocks.x * bytes_per_block,
                   row, (size_t)blocks.width * bytes_per_block);
        } else {
            for (uint32_t x = 0; x < blocks.width; ++x)
                memcpy(guest + gob_offset(storage, blocks.x + x, blocks.y + y),
                       row + (size_t)x * bytes_per_block, bytes_per_block);
        }
    }
}

VkResult pokopia_live_texture_upload_close(PokopiaLiveTextureUpload *upload)
{
    if (!upload)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = nvn_texture_upload_close(&upload->staging);
    if (result == VK_NOT_READY) {
        result = nvn_texture_upload_wait(upload->staging, UINT64_MAX);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
            return result;
        result = nvn_texture_upload_close(&upload->staging);
    }
    if (result == VK_SUCCESS)
        upload->capacity = 0;
    return result;
}

static VkResult stage_image(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                            Region full, Region region, uint32_t layer, uint32_t layers, bool all_layers,
                            const uint8_t *bytes, size_t count)
{
    if (!upload->staging || upload->capacity < count) {
        VkResult result = pokopia_live_texture_upload_close(upload);
        if (result != VK_SUCCESS)
            return result;
        PokopiaVkAdapter *adapter = upload->adapter;
        result = nvn_texture_upload_open(&upload->staging, adapter->device, adapter->queue,
            adapter->family, &adapter->memory, adapter->get_device_proc, count);
        if (result != VK_SUCCESS)
            return result;
        upload->capacity = count;
    }

    const NvnTextureImageState before = {texture->layout, texture->stages, texture->access};
    const NvnTextureImageState sampled = {
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
    };
    const NvnTextureUploadRegion copy = {
        .image = texture->backing.image, .format = texture->format,
        .mip_extent = {full.width, full.height}, .array_layer = layer, .layer_count = layers,
        .offset = {(int32_t)region.x, (int32_t)region.y},
        .extent = {region.width, region.height},
        /* A single-layer update restores the layout shared by all layers. */
        .before = before, .after = all_layers ? sampled : before,
    };
    VkResult result = nvn_texture_upload_submit(upload->staging, &copy, bytes, count, 0);
    if (result == VK_SUCCESS)
        result = nvn_texture_upload_wait(upload->staging, upload_timeout);
    if (result == VK_SUCCESS) {
        texture->layout = copy.after.layout;
        texture->stages = copy.after.stages;
        texture->access = copy.after.access;
        texture->render_serial = 0;
    }
    return result;
}

static VkResult read_image(PokopiaVkAdapter *adapter, const PokopiaVkTexture *texture,
                           const Storage *storage, Region full, uint8_t *destination)
{
    /* An undefined host image has not superseded the canonical bytes. */
    if (texture->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return VK_SUCCESS;
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
#define LOAD(name) PFN_vk##name name = (PFN_vk##name)proc(device, "vk" #name)
    LOAD(CreateBuffer); LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(UnmapMemory);
    LOAD(InvalidateMappedMemoryRanges); LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer);
    LOAD(CmdCopyImageToBuffer); LOAD(CmdPipelineBarrier);
#undef LOAD
    if (!CreateBuffer || !DestroyBuffer || !GetBufferMemoryRequirements || !AllocateMemory ||
        !FreeMemory || !BindBufferMemory || !MapMemory || !UnmapMemory || !InvalidateMappedMemoryRanges ||
        !BeginCommandBuffer || !EndCommandBuffer || !CmdCopyImageToBuffer || !CmdPipelineBarrier)
        return VK_ERROR_INITIALIZATION_FAILED;

    const size_t pitch = storage->footprint.row_bytes;
    const size_t layer_bytes = storage->footprint.packed_bytes;
    const Region blocks = {.width = (uint32_t)(pitch / storage->footprint.bytes_per_block),
                           .height = (uint32_t)storage->footprint.rows};
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = layer_bytes * storage->layers,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = CreateBuffer(device, &buffer_info, NULL, &buffer);
    if (result != VK_SUCCESS)
        return result;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, buffer, &requirements);
    uint32_t type = UINT32_MAX;
    bool coherent = false;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = adapter->memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        type = i;
        coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (coherent)
            break;
    }
    if (type == UINT32_MAX) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory allocated;
    result = AllocateMemory(device, &allocation, NULL, &allocated);
    if (result != VK_SUCCESS)
        goto done;
    memory = allocated;
    result = BindBufferMemory(device, buffer, memory, 0);
    if (result != VK_SUCCESS)
        goto done;
    void *mapping;
    result = MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result != VK_SUCCESS)
        goto done;
    mapped = mapping;
    result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        goto done;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = BeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        goto done;
    VkImageMemoryBarrier image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = texture->access,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = texture->layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = texture->backing.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, storage->layers},
    };
    CmdPipelineBarrier(command, texture->stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
    const VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, storage->layers},
        .imageExtent = {full.width, full.height, 1},
    };
    CmdCopyImageToBuffer(command, image.image, image.newLayout, buffer, 1, &copy);
    image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image.dstAccessMask = texture->access;
    image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.newLayout = texture->layout;
    const VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer, .size = VK_WHOLE_SIZE,
    };
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, texture->stages | VK_PIPELINE_STAGE_HOST_BIT,
                       0, 0, NULL, 1, &host, 1, &image);
    result = EndCommandBuffer(command);
    if (result != VK_SUCCESS)
        goto done;
    bool retired;
    result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (!retired)
        return result;
    if (result == VK_SUCCESS && !coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory, .size = VK_WHOLE_SIZE,
        };
        result = InvalidateMappedMemoryRanges(device, 1, &range);
    }
    if (result == VK_SUCCESS)
        for (uint32_t layer = 0; layer < storage->layers; ++layer)
            pack_region(storage, blocks, (const uint8_t *)mapped + layer * layer_bytes, pitch,
                        destination + layer * storage->layer_stride);

done:
    if (command)
        pokopia_vk_command_buffer_close(adapter, command);
    if (mapped)
        UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    if (memory)
        FreeMemory(device, memory, NULL);
    return result;
}

NvnFrontReplyKind pokopia_live_texture_upload_dispatch(PokopiaLiveTextureUpload *upload,
                                                       const char *name, const uint64_t x[32])
{
    if (!name)
        return NVN_FRONT_NOT_OURS;
    const bool flush = !strcmp(name, "nvnTextureFlushTexels");
    const bool invalidate = !strcmp(name, "nvnTextureInvalidateTexels");
    if (!flush && !invalidate && strcmp(name, "nvnTextureWriteTexels"))
        return !strcmp(name, "nvnTextureWriteTexelsStrided") ? NVN_FRONT_BOUNDARY : NVN_FRONT_NOT_OURS;
    if (!upload || !upload->front || !upload->adapter || !upload->read_guest || !upload->write_guest || !x)
        return NVN_FRONT_BOUNDARY;

    NvnFrontTextureInfo info;
    if (!nvn_front_get_texture(upload->front, x[0], &info))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkTexture *texture = info.native;
    Storage storage;
    if (!texture || !texture->backing.image || texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
        !texture_storage(&info.create_info, texture->format, &storage))
        return NVN_FRONT_BOUNDARY;

    const Region full = {.width = info.create_info.config.width, .height = info.create_info.config.height};
    const uint64_t address = info.create_info.storage_address;
    const size_t storage_bytes = storage.layer_stride * storage.layers;
    if (invalidate) {
        /* NVN invalidation refreshes canonical storage for the whole image. */
        uint8_t *destination = upload->write_guest(upload->guest_opaque, address, storage_bytes);
        if (!destination)
            return NVN_FRONT_INVALID_GUEST;
        const VkResult result = read_image(upload->adapter, texture, &storage, full, destination);
        if (upload->log && (result != VK_SUCCESS || upload->logged < 12)) {
            fprintf(upload->log, "NVN invalidate Vulkan texture %ux%u guest=%#" PRIx64 " result=%d\n",
                    full.width, full.height, address, (int)result);
            fflush(upload->log);
            ++upload->logged;
        }
        return result == VK_SUCCESS ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }

    /* NVN texel writes select the view's first mip and layer. Format and
     * swizzle affect sampling, while CPU writes use the base storage format. */
    uint32_t layer = 0;
    if (!flush && x[1]) {
        const uint8_t *view = upload->read_guest(upload->guest_opaque, x[1], VIEW_BYTES);
        if (!view || ((view[7] & VIEW_LEVELS) && view[4]))
            return NVN_FRONT_INVALID_GUEST;
        if (view[7] & VIEW_LAYERS)
            layer = (uint32_t)view[0] | ((uint32_t)view[1] << 8);
        if (layer >= storage.layers)
            return NVN_FRONT_INVALID_GUEST;
    }
    Region region = full;
    if (!flush && x[2]) {
        const uint8_t *args = upload->read_guest(upload->guest_opaque, x[2], REGION_BYTES);
        if (!args)
            return NVN_FRONT_INVALID_GUEST;
        region = (Region){guest_u32(args), guest_u32(args + 4), guest_u32(args + 12), guest_u32(args + 16)};
        /* For arrays and cubes the view selects the slice, independent of region Z. */
        if ((!storage.layered && (guest_u32(args + 8) || guest_u32(args + 20) != 1)) ||
            !region.width || !region.height ||
            region.x >= full.width || region.y >= full.height ||
            region.width > full.width - region.x || region.height > full.height - region.y)
            return NVN_FRONT_INVALID_GUEST;
    } else if (!flush) {
        return NVN_FRONT_INVALID_GUEST;
    }
    const VkExtent2D block = storage.footprint.block_extent;
    if (region.x % block.width || region.y % block.height ||
        (region.width % block.width && region.width != full.width - region.x) ||
        (region.height % block.height && region.height != full.height - region.y))
        return NVN_FRONT_INVALID_GUEST;
    NvnTextureFootprint region_footprint;
    if (nvn_texture_upload_footprint(texture->format, (VkExtent2D){region.width, region.height}, 0,
                                     &region_footprint) != VK_SUCCESS)
        return NVN_FRONT_BOUNDARY;
    const Region blocks = {region.x / block.width, region.y / block.height,
        (uint32_t)(region_footprint.row_bytes / region_footprint.bytes_per_block), (uint32_t)region_footprint.rows};

    /* First use and flush initialize all layers together. A first partial
     * write preserves the untouched texels from canonical guest storage. */
    const bool full_upload = flush || texture->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    const bool unpack = flush || (full_upload &&
        (storage.layers > 1 || region.width != full.width || region.height != full.height));
    uint8_t *destination = flush ? NULL : upload->write_guest(upload->guest_opaque, address, storage_bytes);
    const uint8_t *guest = unpack ? upload->read_guest(upload->guest_opaque, address, storage_bytes) : NULL;
    const size_t region_pitch = region_footprint.row_bytes;
    const uint8_t *source = !flush && x[3] ? upload->read_guest(upload->guest_opaque, x[3], region_footprint.packed_bytes) : NULL;
    if ((!flush && (!destination || !source)) || (unpack && !guest))
        return NVN_FRONT_INVALID_GUEST;

    const Region transfer = full_upload ? full : region;
    const size_t pitch = full_upload ? storage.footprint.row_bytes : region_pitch;
    const size_t layer_bytes = full_upload ? storage.footprint.packed_bytes : region_footprint.packed_bytes;
    const uint32_t layers = full_upload ? storage.layers : 1;
    const size_t count = layer_bytes * layers;
    uint8_t *linear = malloc(count);
    if (!linear)
        return NVN_FRONT_BOUNDARY;
    uint8_t *patch = linear;
    if (unpack)
        for (uint32_t i = 0; i < layers; ++i)
            unpack_level(&storage, guest + i * storage.layer_stride, linear + i * layer_bytes);
    if (full_upload)
        patch += layer * layer_bytes + blocks.y * pitch + (size_t)blocks.x * storage.footprint.bytes_per_block;
    if (!flush)
        for (uint32_t y = 0; y < blocks.height; ++y)
            memcpy(patch + y * pitch, source + y * region_pitch, region_pitch);

    const VkResult result = stage_image(upload, texture, full, transfer, full_upload ? 0 : layer, layers,
                                       layers == storage.layers, linear, count);
    if (result == VK_SUCCESS && !flush)
        pack_region(&storage, blocks, patch, pitch, destination + layer * storage.layer_stride);
    free(linear);

    if (upload->log && (result != VK_SUCCESS || upload->logged < 12)) {
        fprintf(upload->log, "NVN %s Vulkan texture %ux%u region %u,%u %ux%u guest=%#" PRIx64
            " bytes=%zu result=%d\n", flush ? "flush" : "write", full.width, full.height,
            region.x, region.y, region.width, region.height, address, count, (int)result);
        fflush(upload->log);
        ++upload->logged;
    }
    return result == VK_SUCCESS ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}
