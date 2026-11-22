/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_guest_layout.h"
#include "live_texture_upload.h"
#include "vk_ledger.h"
#include "vk_flush.h"
#include "ps_button_glyphs.h"
#include "ps_title_glyphs.h"
#include "port_settings.h"

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

static void unpack_region(const Storage *storage, Region blocks, const uint8_t *guest,
                          uint8_t *linear, size_t pitch)
{
    const size_t bytes_per_block = storage->footprint.bytes_per_block;
    for (uint32_t y = 0; y < blocks.height; ++y) {
        uint8_t *row = linear + y * pitch;
        if (storage->pitch) {
            memcpy(row, guest + (blocks.y + y) * storage->pitch + (size_t)blocks.x * bytes_per_block,
                   (size_t)blocks.width * bytes_per_block);
        } else {
            for (uint32_t x = 0; x < blocks.width; ++x)
                memcpy(row + (size_t)x * bytes_per_block,
                       guest + gob_offset(storage, blocks.x + x, blocks.y + y), bytes_per_block);
        }
    }
}

static void unpack_level(const Storage *storage, const uint8_t *guest, uint8_t *linear)
{
    const Region all = {.width = (uint32_t)(storage->footprint.row_bytes / storage->footprint.bytes_per_block),
                        .height = (uint32_t)storage->footprint.rows};
    unpack_region(storage, all, guest, linear, storage->footprint.row_bytes);
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

static const NvnTextureImageState sampled_texture_state = {
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
};

/* R90: small uploads made while the replay worker prepares a draw (the
 * per-frame 2x1 CPU-uncached parameters, glyph-cache rows, first-sample mips)
 * are copied into a host-visible ring and recorded into the open draw batch.
 * The staging path flushed and waited for every pending batch, submitted, and
 * waited again: two GPU drains a frame (~0.6 s of every 5 s outdoors). The
 * ring is in segments; a segment is reused once the batch that last used it
 * has completed, otherwise the upload takes the staging path. */
enum { RING_BYTES = 8u << 20, RING_SEGMENTS = 8, INLINE_MAX = 512u << 10 };
static struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    bool coherent, failed;
    VkDeviceSize head;
    uint64_t segment_batch[RING_SEGMENTS];
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges;
} ring;
static bool inline_allowed;
uint64_t pokopia_inline_uploads, pokopia_inline_bytes, pokopia_inline_fallbacks;

static bool ring_open(PokopiaVkAdapter *adapter)
{
    if (ring.failed)
        return false;
    ring.failed = true;
    VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkCreateBuffer CreateBuffer = (PFN_vkCreateBuffer)proc(device, "vkCreateBuffer");
    PFN_vkDestroyBuffer DestroyBuffer = (PFN_vkDestroyBuffer)proc(device, "vkDestroyBuffer");
    PFN_vkGetBufferMemoryRequirements GetRequirements =
        (PFN_vkGetBufferMemoryRequirements)proc(device, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory AllocateMemory = (PFN_vkAllocateMemory)proc(device, "vkAllocateMemory");
    PFN_vkFreeMemory FreeMemory = (PFN_vkFreeMemory)proc(device, "vkFreeMemory");
    PFN_vkBindBufferMemory BindBufferMemory = (PFN_vkBindBufferMemory)proc(device, "vkBindBufferMemory");
    PFN_vkMapMemory MapMemory = (PFN_vkMapMemory)proc(device, "vkMapMemory");
    ring.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    ring.CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)proc(device, "vkCmdCopyBufferToImage");
    ring.FlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)proc(device, "vkFlushMappedMemoryRanges");
    if (!CreateBuffer || !DestroyBuffer || !GetRequirements || !AllocateMemory || !FreeMemory || !BindBufferMemory ||
        !MapMemory || !ring.CmdPipelineBarrier || !ring.CmdCopyBufferToImage || !ring.FlushMappedMemoryRanges)
        return false;
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = RING_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    if (CreateBuffer(device, &info, NULL, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    GetRequirements(device, buffer, &requirements);
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
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (type == UINT32_MAX ||
        pokopia_vk_ledger_allocate(AllocateMemory, device, &allocation, &memory, VK_LEDGER_DOWNLOAD) != VK_SUCCESS) {
        DestroyBuffer(device, buffer, NULL);
        return false;
    }
    if (BindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
        MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        DestroyBuffer(device, buffer, NULL);
        pokopia_vk_ledger_free(FreeMemory, device, memory);
        return false;
    }
    ring.buffer = buffer;
    ring.memory = memory;
    ring.mapped = mapped;
    ring.coherent = coherent;
    ring.failed = false;
    return true;
}

static void ring_transition(VkCommandBuffer command, const NvnTextureUploadRegion *region, uint32_t layers,
                            NvnTextureImageState before, NvnTextureImageState after)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = before.access, .dstAccessMask = after.access,
        .oldLayout = before.layout, .newLayout = after.layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = region->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, region->mip_level, 1, region->array_layer, layers},
    };
    ring.CmdPipelineBarrier(command, before.stages, after.stages, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static bool stage_inline(PokopiaLiveTextureUpload *upload, const NvnTextureUploadRegion *region,
                         const uint8_t *bytes, size_t count)
{
    if (!inline_allowed || !count || count > INLINE_MAX || region->depth > 1 || !pokopia_vk_batch_command_hook ||
        !region->image || region->after.layout == VK_IMAGE_LAYOUT_UNDEFINED || !region->after.stages)
        return false;
    if (!ring.buffer && !ring_open(upload->adapter))
        return false;
    NvnTextureFootprint footprint;
    const uint32_t layers = region->layer_count ? region->layer_count : 1;
    if (nvn_texture_upload_footprint(region->format, region->extent, 0, &footprint) != VK_SUCCESS ||
        footprint.packed_bytes * layers != count)
        return false;
    const VkDeviceSize segment = RING_BYTES / RING_SEGMENTS;
    VkDeviceSize at = (ring.head + 255) & ~(VkDeviceSize)255;
    if (at + count > RING_BYTES)
        at = 0;
    const unsigned continuing = ring.head && at >= ring.head ? (unsigned)((ring.head - 1) / segment) : RING_SEGMENTS;
    const unsigned first = (unsigned)(at / segment), last = (unsigned)((at + count - 1) / segment);
    /* A segment entered anew (not the one being appended to) must hold nothing
     * a pending batch still reads, the open batch included: during loading one
     * batch can stage more than the whole ring, and reusing its own earlier
     * segments overwrote copies it had not run yet (wrong textures). */
    for (unsigned s = first; s <= last; ++s)
        if (s != continuing && ring.segment_batch[s] > pokopia_vk_batch_retired) {
            ++pokopia_inline_fallbacks;
            return false;
        }
    const VkCommandBuffer command = pokopia_vk_batch_command_hook();
    if (!command)
        return false;
    memcpy(ring.mapped + at, bytes, count);
    if (!ring.coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = ring.memory, .size = VK_WHOLE_SIZE,
        };
        if (ring.FlushMappedMemoryRanges(upload->adapter->device, 1, &range) != VK_SUCCESS)
            return false;
    }
    NvnTextureImageState before = region->before;
    if (before.layout == VK_IMAGE_LAYOUT_UNDEFINED)
        before.stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const NvnTextureImageState transfer = {
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    ring_transition(command, region, layers, before, transfer);
    const VkBufferImageCopy copy = {
        .bufferOffset = at,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region->mip_level, region->array_layer, layers},
        .imageOffset = {region->offset.x, region->offset.y, 0},
        .imageExtent = {region->extent.width, region->extent.height, 1},
    };
    ring.CmdCopyBufferToImage(command, ring.buffer, region->image, transfer.layout, 1, &copy);
    ring_transition(command, region, layers, transfer, region->after);
    for (unsigned s = first; s <= last; ++s)
        ring.segment_batch[s] = pokopia_vk_batch_id;
    ring.head = at + count;
    ++pokopia_inline_uploads;
    pokopia_inline_bytes += count;
    return true;
}

static VkResult stage_region(PokopiaLiveTextureUpload *upload, const NvnTextureUploadRegion *region,
                             const uint8_t *bytes, size_t count)
{
    if (stage_inline(upload, region, bytes, count))
        return VK_SUCCESS;
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

    VkResult result = nvn_texture_upload_submit(upload->staging, region, bytes, count, 0);
    if (result == VK_SUCCESS)
        result = nvn_texture_upload_wait(upload->staging, upload_timeout);
    return result;
}

static VkResult stage_image(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                            Region full, Region region, uint32_t layer, uint32_t layers, bool all_layers,
                            const uint8_t *bytes, size_t count)
{
    const NvnTextureImageState before = {texture->layout, texture->stages, texture->access};
    const NvnTextureUploadRegion copy = {
        .image = texture->backing.image, .format = texture->format,
        .mip_extent = {full.width, full.height}, .array_layer = layer, .layer_count = layers,
        .offset = {(int32_t)region.x, (int32_t)region.y},
        .extent = {region.width, region.height},
        /* A single-layer update restores the layout shared by all layers. */
        .before = before, .after = all_layers ? sampled_texture_state : before,
    };
    const VkResult result = stage_region(upload, &copy, bytes, count);
    if (result == VK_SUCCESS) {
        texture->layout = copy.after.layout;
        texture->stages = copy.after.stages;
        texture->access = copy.after.access;
        texture->render_serial = 0;
    }
    return result;
}

typedef struct {
    PokopiaLiveTextureUpload *upload;
    const PokopiaVkTexture *texture;
} DeferredTextureUpload;

/* R83: the UI icon atlas (1024x1024 BC7) holds the Switch button glyphs; its
 * A/B/X/Y, L/R/ZL/ZR and +/- are replaced with PlayStation glyphs
 * (ps_button_glyphs.h, ps5/scripts/gen-ps-glyphs.py) as it is uploaded. The
 * atlas is recognised by a hash of its level-0 blocks. */
static uint64_t glyph_atlas_hash(const uint8_t *data, size_t bytes)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, sizeof word);
        hash = (hash ^ word) * UINT64_C(0x100000001b3);
    }
    return hash;
}

/* R91: and the title logo texture (1024x1024 BC7) has "PRESS (A) BUTTON"
 * baked in; its (A) becomes the Circle glyph (ps_title_glyphs.h,
 * ps5/scripts/gen-ps-title.py). */
typedef struct {
    uint16_t x, y, width, height;
    const uint8_t *blocks;
} GlyphPatch;

/* R92: the textures that carry button glyphs, kept so a change of the
 * prompts or confirm setting can upload them again from the game's bytes
 * (pokopia_live_texture_prepare_sample). */
static struct {
    const PokopiaVkTexture *texture;
    bool stale;
} glyph_textures[4];
static unsigned glyph_seen_generation;
static void texture_forget(void *texture);

static void glyph_texture_seen(const PokopiaVkTexture *texture)
{
    for (unsigned i = 0; i < 4; ++i)
        if (glyph_textures[i].texture == texture)
            return;
    for (unsigned i = 0; i < 4; ++i)
        if (!glyph_textures[i].texture) {
            glyph_textures[i].texture = texture;
            glyph_textures[i].stale = false;
            pokopia_vk_texture_forget_hook = texture_forget;
            return;
        }
}

static uint8_t *ps_glyph_patch(const PokopiaVkTexture *texture, VkFormat format, VkExtent2D extent, uint32_t mip,
                               uint32_t layers, const uint8_t *linear, size_t bytes)
{
    enum { BLOCK = 16, ROW_BLOCKS = PS_GLYPH_ATLAS_SIZE / 4 };
    _Static_assert(PS_GLYPH_ATLAS_SIZE == PS_TITLE_TEXTURE_SIZE, "both patched textures are 1024x1024");
    if (mip || layers != 1 || extent.width != PS_GLYPH_ATLAS_SIZE || extent.height != PS_GLYPH_ATLAS_SIZE ||
        (format != VK_FORMAT_BC7_UNORM_BLOCK && format != VK_FORMAT_BC7_SRGB_BLOCK) ||
        bytes != (size_t)ROW_BLOCKS * ROW_BLOCKS * BLOCK)
        return NULL;
    const uint64_t hash = glyph_atlas_hash(linear, bytes);
    if (hash != PS_GLYPH_ATLAS_HASH && hash != PS_TITLE_TEXTURE_HASH)
        return NULL;
    glyph_texture_seen(texture);
    /* R92: Nintendo prompts keep the game's own glyphs. */
    if (pokopia_settings.prompts == POKOPIA_PROMPTS_NINTENDO)
        return NULL;
    const bool cross = pokopia_settings.confirm == POKOPIA_CONFIRM_CROSS;
    _Static_assert(sizeof ps_glyph_patches[0] == sizeof(GlyphPatch) && sizeof ps_title_patches[0] == sizeof(GlyphPatch) &&
                       sizeof ps_title_patches_cross[0] == sizeof(GlyphPatch),
                   "patch tables share one layout");
    GlyphPatch atlas[sizeof ps_glyph_patches / sizeof ps_glyph_patches[0]];
    const GlyphPatch *patches;
    size_t count;
    if (hash == PS_GLYPH_ATLAS_HASH) {
        memcpy(atlas, ps_glyph_patches, sizeof atlas);
        count = sizeof atlas / sizeof atlas[0];
        /* The first two are A (right) and B (bottom), both 68x68: with Cross
         * confirming, A shows Cross and B shows Circle. */
        if (cross) {
            const uint8_t *a = atlas[0].blocks;
            atlas[0].blocks = atlas[1].blocks;
            atlas[1].blocks = a;
        }
        patches = atlas;
    } else {
        patches = (const GlyphPatch *)(cross ? (const void *)ps_title_patches_cross : (const void *)ps_title_patches);
        count = 1;
    }
    uint8_t *patched = malloc(bytes);
    if (!patched)
        return NULL;
    memcpy(patched, linear, bytes);
    for (size_t i = 0; i < count; ++i) {
        const size_t x = patches[i].x / 4, y = patches[i].y / 4;
        const size_t width = patches[i].width / 4, height = patches[i].height / 4;
        for (size_t row = 0; row < height; ++row)
            memcpy(patched + ((y + row) * ROW_BLOCKS + x) * BLOCK, patches[i].blocks + row * width * BLOCK,
                   width * BLOCK);
    }
    return patched;
}

static VkResult upload_compressed_mip(void *opaque, VkExtent2D extent, uint32_t mip,
                                      uint32_t first_layer, uint32_t layers, const uint8_t *linear, size_t bytes)
{
    const DeferredTextureUpload *deferred = opaque;
    uint8_t *patched = ps_glyph_patch(deferred->texture, deferred->texture->format, extent, mip, layers, linear, bytes);
    if (patched)
        linear = patched;
    const bool volume = deferred->texture->sample_view_type == VK_IMAGE_VIEW_TYPE_3D;
    const NvnTextureUploadRegion copy = {
        .image = deferred->texture->backing.image, .format = deferred->texture->format,
        .mip_extent = extent, .mip_level = mip, .array_layer = volume ? 0 : first_layer,
        .layer_count = volume ? 1 : layers, .depth = volume ? layers : 0,
        .extent = extent, .before = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0},
        .after = sampled_texture_state,
    };
    const VkResult result = stage_region(deferred->upload, &copy, linear, bytes);
    free(patched);
    return result;
}

static VkResult prepare_deferred(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                                 const NvnFrontTextureCreateInfo *info)
{
    if (!info->storage_size || info->storage_size > SIZE_MAX)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const size_t count = (size_t)info->storage_size;
    const uint8_t *guest = upload->read_guest(upload->guest_opaque, info->storage_address, count);
    if (!guest)
        return VK_ERROR_MEMORY_MAP_FAILED;

    /* A prior failed wait may still own the shared staging buffer and image. */
    VkResult result = upload->staging ? nvn_texture_upload_wait(upload->staging, upload_timeout) : VK_SUCCESS;
    if (result == VK_SUCCESS && !texture->backing.image) {
        result = pokopia_vk_texture_open(upload->adapter, &info->config, texture);
        texture->deferred = true;
    }
    if (result == VK_SUCCESS) {
        DeferredTextureUpload deferred = {upload, texture};
        result = nvn_guest_untile_compressed(&info->config, guest, count, upload_compressed_mip, &deferred);
    }
    if (result == VK_SUCCESS) {
        /* All mip levels and layers now share the same Vulkan state. */
        texture->layout = sampled_texture_state.layout;
        texture->stages = sampled_texture_state.stages;
        texture->access = sampled_texture_state.access;
        texture->deferred = false;
    }
    if (upload->log) {
        fprintf(upload->log, "NVN first sampled texture %ux%u format=%u guest=%#" PRIx64
                " bytes=%zu result=%d\n", info->config.width, info->config.height, (unsigned)texture->format,
                info->storage_address, count, (int)result);
        fflush(upload->log);
    }
    return result;
}

/* R66: uncompressed colour textures got a Vulkan image at creation but their
 * guest bytes were never copied in, so every RGBA8/R8 asset (UI art, font
 * sheets) sampled as zeros. Copy them in on first sample, as BC textures are. */
static VkResult prepare_uncompressed(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                                     const NvnFrontTextureCreateInfo *info)
{
    if (!info->storage_size || info->storage_size > SIZE_MAX)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const size_t count = (size_t)info->storage_size;
    const uint8_t *guest = upload->read_guest(upload->guest_opaque, info->storage_address, count);
    if (!guest)
        return VK_ERROR_MEMORY_MAP_FAILED;
    VkResult result = upload->staging ? nvn_texture_upload_wait(upload->staging, upload_timeout) : VK_SUCCESS;
    if (result == VK_SUCCESS) {
        DeferredTextureUpload deferred = {upload, texture};
        result = nvn_guest_untile(&info->config, guest, count, upload_compressed_mip, &deferred);
    }
    if (result == VK_SUCCESS) {
        texture->layout = sampled_texture_state.layout;
        texture->stages = sampled_texture_state.stages;
        texture->access = sampled_texture_state.access;
    }
    if (upload->log) {
        fprintf(upload->log, "NVN first sampled uncompressed texture %ux%u layers=%u levels=%u format=%u guest=%#" PRIx64
                " bytes=%zu result=%d\n", info->config.width, info->config.height, texture->array_layers,
                info->config.levels, (unsigned)texture->format, info->storage_address, count, (int)result);
        fflush(upload->log);
    }
    return result;
}

/* R66: pitch-linear textures the CPU writes directly (the font glyph cache)
 * never pass through an NVN call. Compare the guest rows with the bytes last
 * copied in and upload the changed row range on each sample. */
static VkResult sync_linear(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                            const NvnFrontTextureCreateInfo *info)
{
    Storage storage;
    if (!texture_storage(info, texture->format, &storage) || storage.layered || storage.layers != 1)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* R78: once per guest submit; the guest writes it between submits (every
     * sample compared all 4 MiB of the glyph cache with its shadow). */
    if (texture->guest_shadow && texture->linear_epoch == pokopia_replay_job_epoch)
        return VK_SUCCESS;
    texture->linear_epoch = pokopia_replay_job_epoch;
    const size_t bytes = (size_t)info->storage_size;
    const uint8_t *guest = upload->read_guest(upload->guest_opaque, info->storage_address, bytes);
    if (!guest)
        return VK_ERROR_MEMORY_MAP_FAILED;
    const size_t rows = storage.footprint.rows, row_bytes = storage.footprint.row_bytes, pitch = storage.pitch;
    const bool first = !texture->guest_shadow;
    size_t top = 0, bottom = rows;
    if (first) {
        texture->guest_shadow = malloc(bytes);
        if (!texture->guest_shadow)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    } else {
        const uint8_t *shadow = texture->guest_shadow;
        while (top < rows && !memcmp(guest + top * pitch, shadow + top * pitch, row_bytes))
            ++top;
        if (top == rows)
            return VK_SUCCESS;
        while (bottom > top && !memcmp(guest + (bottom - 1) * pitch, shadow + (bottom - 1) * pitch, row_bytes))
            --bottom;
    }
    const size_t count = (bottom - top) * row_bytes;
    uint8_t *packed = malloc(count);
    if (!packed)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (size_t row = top; row < bottom; ++row)
        memcpy(packed + (row - top) * row_bytes, guest + row * pitch, row_bytes);
    const uint32_t block_height = storage.footprint.rows ? info->config.height / (uint32_t)storage.footprint.rows : 1;
    const Region full = {.width = info->config.width, .height = info->config.height};
    const Region region = {.x = 0, .y = (uint32_t)top * block_height, .width = info->config.width,
                           .height = (uint32_t)(bottom - top) * block_height};
    const VkResult result = stage_image(upload, texture, full, region, 0, 1, true, packed, count);
    free(packed);
    if (result == VK_SUCCESS)
        memcpy(texture->guest_shadow, guest, bytes);
    static unsigned logged;
    if (upload->log && (first || result != VK_SUCCESS || logged < 16)) {
        ++logged;
        fprintf(upload->log, "NVN linear texture sync %ux%u format=%u rows %zu..%zu of %zu first=%d result=%d\n",
                info->config.width, info->config.height, (unsigned)texture->format, top, bottom, rows, first,
                (int)result);
        fflush(upload->log);
    }
    return result;
}

/* R89: time in sync_linear, and the compares it made (bytes compared). */
uint64_t pokopia_linear_sync_ns, pokopia_linear_sync_calls, pokopia_linear_sync_bytes;

static VkResult prepare_sample_slow(PokopiaLiveTextureUpload *upload, const NvnFrontTextureInfo *info)
{
    enum { TEXTURE_IMAGE = 4, POOL_CPU_MASK = 7, POOL_CPU_UNCACHED = 2, RGBA32F_PAIR_BYTES = 32 };
    if (!upload || !upload->adapter || !upload->read_guest || !info || !info->native)
        return VK_ERROR_INITIALIZATION_FAILED;

    PokopiaVkTexture *texture = info->native;
    if (texture->deferred)
        return prepare_deferred(upload, texture, &info->create_info);
    const NvnFrontTextureConfig *config = &info->create_info.config;
    enum { TEXTURE_DISPLAY = 1, TEXTURE_COMPRESSIBLE = 8, TEXTURE_LINEAR_FLAG = 16, TEXTURE_3D = 2 };
    /* R73: 3D textures too, even writable ones: the intro's colour LUT
     * volumes are loaded by the CPU into textures created with the image flag,
     * and a compute pass blends them into the LUT the tonemap samples.
     * R88: and writable 2D ones: outdoor assets (the sky gradient, the cloud
     * bands, 512x512 and 1024x1024 mipmapped RGBA8 art) are loaded by the CPU
     * into image-flagged textures and sampled as zeros, a black sky. Only a
     * texture the GPU has not written yet (undefined, never rendered) is
     * filled from its guest bytes. */
    (void)TEXTURE_IMAGE;
    if (texture->layout == VK_IMAGE_LAYOUT_UNDEFINED && !texture->render_serial && texture->backing.image &&
        texture->aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
        !(config->flags & (TEXTURE_DISPLAY | TEXTURE_COMPRESSIBLE | TEXTURE_LINEAR_FLAG))) {
        /* A failed copy leaves the old behaviour: the draw clears the texture. */
        (void)prepare_uncompressed(upload, texture, &info->create_info);
        return VK_SUCCESS;
    }
    /* CPU-written linear 2D textures; the GPU has not rendered to them. A
     * failed sync keeps the old behaviour. */
    if ((config->flags & TEXTURE_LINEAR_FLAG) && config->target == TEXTURE_2D && config->levels == 1 &&
        !(config->flags & (TEXTURE_DISPLAY | TEXTURE_COMPRESSIBLE)) && !texture->render_serial &&
        texture->backing.image && texture->aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
        (texture->guest_shadow || texture->layout == VK_IMAGE_LAYOUT_UNDEFINED) &&
        !(config->width == 2 && config->height == 1 && texture->format == VK_FORMAT_R32G32B32A32_SFLOAT)) {
        const uint64_t start = pokopia_now_ns ? pokopia_now_ns() : 0;
        const bool compare = !(texture->guest_shadow && texture->linear_epoch == pokopia_replay_job_epoch);
        (void)sync_linear(upload, texture, &info->create_info);
        if (pokopia_now_ns) {
            pokopia_linear_sync_ns += pokopia_now_ns() - start;
            pokopia_linear_sync_calls += compare;
            if (compare)
                pokopia_linear_sync_bytes += info->create_info.storage_size;
        }
        return VK_SUCCESS;
    }
    static unsigned skipped_logged;
    if (texture->layout == VK_IMAGE_LAYOUT_UNDEFINED && !texture->render_serial && upload->log &&
        skipped_logged < 24) {
        ++skipped_logged;
        fprintf(upload->log, "NVN first sample left undefined %ux%u target=%u format=%u flags=%#x aspects=%#x\n",
                config->width, config->height, config->target, config->format, config->flags,
                (unsigned)texture->aspects);
        fflush(upload->log);
    }
    if (config->target != TEXTURE_2D || config->flags != (TEXTURE_LINEAR | TEXTURE_IMAGE) ||
        config->width != 2 || config->height != 1 || config->levels != 1 ||
        texture->format != VK_FORMAT_R32G32B32A32_SFLOAT || texture->render_serial)
        return VK_SUCCESS;

    NvnFrontPoolConfig pool;
    if (!nvn_front_get_pool(upload->front, config->pool, &pool))
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((pool.flags & POOL_CPU_MASK) != POOL_CPU_UNCACHED)
        return VK_SUCCESS;
    if (info->create_info.storage_size != RGBA32F_PAIR_BYTES)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    /* CPU_UNCACHED mappings allow writes without FlushMappedRange. The guest's
     * two tightly packed texels remain canonical while this image is sampled. */
    const uint64_t storage = info->create_info.storage_address;
    const uint8_t *bytes = upload->read_guest(upload->guest_opaque, storage, RGBA32F_PAIR_BYTES);
    if (!bytes)
        return VK_ERROR_MEMORY_MAP_FAILED;

    const Region extent = {.width = 2, .height = 1};
    const bool first = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult result = stage_image(upload, texture, extent, extent, 0, 1, true, bytes, RGBA32F_PAIR_BYTES);
    if (upload->log && (first || result != VK_SUCCESS)) {
        fprintf(upload->log, "NVN coherent linear sample guest=%#" PRIx64 " bytes=%u result=%d\n",
                storage, RGBA32F_PAIR_BYTES, (int)result);
        fflush(upload->log);
    }
    return result;
}

/* R89: shadow readback. The game reads a few small GPU-written textures back
 * on the CPU (outdoors: 1x1 and 8x8, about once a second), after waiting on
 * its sync. A copy submitted then waits behind every frame queued since
 * (~110 ms with the graphics lock held). Instead, each texture read back once
 * gets a host-visible buffer, and a copy into it is recorded at the end of
 * every draw batch that changed the texture; a later read only waits for that
 * batch (normally done already). */
typedef struct {
    PokopiaVkTexture *texture;
    VkImage image;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize bytes;
    void *mapped;
    bool coherent;
    uint32_t width, height, layers;
    uint64_t serial, batch;
} Shadow;
enum { SHADOWS = 32, SHADOW_MAX_BYTES = 256 * 1024 };
static Shadow shadows[SHADOWS];
static PokopiaVkAdapter *shadow_adapter;
uint64_t pokopia_readback_fast, pokopia_readback_slow;

static Shadow *shadow_find(const PokopiaVkTexture *texture)
{
    for (unsigned i = 0; i < SHADOWS; ++i)
        if (shadows[i].texture == texture && shadows[i].image == texture->backing.image)
            return &shadows[i];
    return NULL;
}

static void shadow_forget(void *texture);
/* R92: both kinds of remembered texture (readback shadows, glyph textures). */
static void texture_forget(void *texture)
{
    shadow_forget(texture);
    for (unsigned i = 0; i < 4; ++i)
        if (glyph_textures[i].texture == texture)
            glyph_textures[i].texture = NULL;
}

static void shadow_forget(void *texture)
{
    for (unsigned i = 0; i < SHADOWS; ++i)
        if (shadows[i].texture == texture) {
            shadows[i].texture = NULL;
            shadows[i].image = VK_NULL_HANDLE;
            shadows[i].serial = 0;
        }
}

static void shadow_record(VkCommandBuffer command)
{
    PokopiaVkAdapter *adapter = shadow_adapter;
    static PFN_vkCmdPipelineBarrier barrier;
    static PFN_vkCmdCopyImageToBuffer copy;
    if (!adapter || !command)
        return;
    if (!barrier || !copy) {
        barrier = (PFN_vkCmdPipelineBarrier)adapter->get_device_proc(adapter->device, "vkCmdPipelineBarrier");
        copy = (PFN_vkCmdCopyImageToBuffer)adapter->get_device_proc(adapter->device, "vkCmdCopyImageToBuffer");
        if (!barrier || !copy)
            return;
    }
    for (unsigned i = 0; i < SHADOWS; ++i) {
        Shadow *shadow = &shadows[i];
        PokopiaVkTexture *texture = shadow->texture;
        if (!texture || texture->backing.image != shadow->image || !texture->render_serial ||
            texture->render_serial == shadow->serial || texture->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        const VkPipelineStageFlags stages = texture->stages ? texture->stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkImageMemoryBarrier image = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = texture->access,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = texture->layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = shadow->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, shadow->layers},
        };
        barrier(command, stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
        const VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, shadow->layers},
            .imageExtent = {shadow->width, shadow->height, 1},
        };
        copy(command, shadow->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shadow->buffer, 1, &region);
        image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image.dstAccessMask = texture->access;
        image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.newLayout = texture->layout;
        const VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = shadow->buffer, .size = VK_WHOLE_SIZE,
        };
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, stages | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host,
                1, &image);
        shadow->serial = texture->render_serial;
        shadow->batch = pokopia_vk_batch_id;
    }
}

/* Starts shadowing a small texture (after a read that had to wait). */
static void shadow_add(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, const Storage *storage, Region full)
{
    const VkDeviceSize bytes = (VkDeviceSize)storage->footprint.packed_bytes * storage->layers;
    if (!bytes || bytes > SHADOW_MAX_BYTES || shadow_find(texture) || !texture->backing.image)
        return;
    Shadow *slot = NULL;
    for (unsigned i = 0; i < SHADOWS && !slot; ++i)
        if (!shadows[i].texture && shadows[i].buffer && shadows[i].bytes >= bytes)
            slot = &shadows[i];
    for (unsigned i = 0; i < SHADOWS && !slot; ++i)
        if (!shadows[i].texture && !shadows[i].buffer)
            slot = &shadows[i];
    if (!slot)
        return;
    VkDevice device = adapter->device;
    if (!slot->buffer) {
        PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
        PFN_vkCreateBuffer CreateBuffer = (PFN_vkCreateBuffer)proc(device, "vkCreateBuffer");
        PFN_vkDestroyBuffer DestroyBuffer = (PFN_vkDestroyBuffer)proc(device, "vkDestroyBuffer");
        PFN_vkGetBufferMemoryRequirements GetRequirements =
            (PFN_vkGetBufferMemoryRequirements)proc(device, "vkGetBufferMemoryRequirements");
        PFN_vkAllocateMemory AllocateMemory = (PFN_vkAllocateMemory)proc(device, "vkAllocateMemory");
        PFN_vkFreeMemory FreeMemory = (PFN_vkFreeMemory)proc(device, "vkFreeMemory");
        PFN_vkBindBufferMemory BindBufferMemory = (PFN_vkBindBufferMemory)proc(device, "vkBindBufferMemory");
        PFN_vkMapMemory MapMemory = (PFN_vkMapMemory)proc(device, "vkMapMemory");
        if (!CreateBuffer || !DestroyBuffer || !GetRequirements || !AllocateMemory || !FreeMemory ||
            !BindBufferMemory || !MapMemory)
            return;
        const VkDeviceSize size = bytes < 4096 ? 4096 : bytes;
        const VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer buffer;
        if (CreateBuffer(device, &info, NULL, &buffer) != VK_SUCCESS)
            return;
        VkMemoryRequirements requirements;
        GetRequirements(device, buffer, &requirements);
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
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = NULL;
        const VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
            .memoryTypeIndex = type,
        };
        if (type == UINT32_MAX ||
            pokopia_vk_ledger_allocate(AllocateMemory, device, &allocation, &memory, VK_LEDGER_DOWNLOAD) != VK_SUCCESS) {
            DestroyBuffer(device, buffer, NULL);
            return;
        }
        if (BindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
            MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            DestroyBuffer(device, buffer, NULL);
            pokopia_vk_ledger_free(FreeMemory, device, memory);
            return;
        }
        *slot = (Shadow){.buffer = buffer, .memory = memory, .bytes = size, .mapped = mapped, .coherent = coherent};
    }
    slot->texture = texture;
    slot->image = texture->backing.image;
    slot->width = full.width;
    slot->height = full.height;
    slot->layers = storage->layers;
    slot->serial = slot->batch = 0;
    shadow_adapter = adapter;
    pokopia_vk_before_batch_end_hook = shadow_record;
    pokopia_vk_texture_forget_hook = texture_forget;
}

/* A read from a current shadow; false when the slow path must run. */
static bool shadow_read(PokopiaVkAdapter *adapter, const PokopiaVkTexture *texture, const Storage *storage,
                        uint8_t *destination)
{
    Shadow *shadow = shadow_find(texture);
    if (!shadow || !shadow->serial || shadow->serial != texture->render_serial ||
        !pokopia_live_generic_wait_batch(shadow->batch))
        return false;
    if (!shadow->coherent) {
        PFN_vkInvalidateMappedMemoryRanges invalidate = (PFN_vkInvalidateMappedMemoryRanges)
            adapter->get_device_proc(adapter->device, "vkInvalidateMappedMemoryRanges");
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = shadow->memory, .size = VK_WHOLE_SIZE,
        };
        if (!invalidate || invalidate(adapter->device, 1, &range) != VK_SUCCESS)
            return false;
    }
    const size_t pitch = storage->footprint.row_bytes;
    const size_t layer_bytes = storage->footprint.packed_bytes;
    const Region blocks = {.width = (uint32_t)(pitch / storage->footprint.bytes_per_block),
                           .height = (uint32_t)storage->footprint.rows};
    for (uint32_t layer = 0; layer < storage->layers; ++layer)
        pack_region(storage, blocks, (const uint8_t *)shadow->mapped + layer * layer_bytes, pitch,
                    destination + layer * storage->layer_stride);
    ++pokopia_readback_fast;
    return true;
}

static VkResult read_image(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                           const Storage *storage, Region full, uint8_t *destination)
{
    /* An undefined host image has not superseded the canonical bytes. */
    if (texture->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return VK_SUCCESS;
    if (shadow_read(adapter, texture, storage, destination))
        return VK_SUCCESS;
    ++pokopia_readback_slow;
    shadow_add(adapter, texture, storage, full);
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
    result = pokopia_vk_ledger_allocate(AllocateMemory, device, &allocation, &allocated, VK_LEDGER_DOWNLOAD);
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
        pokopia_vk_ledger_free(FreeMemory, device, memory);
    return result;
}

static NvnFrontReplyKind read_texels(PokopiaLiveTextureUpload *upload, PokopiaVkTexture *texture,
                                    const Storage *storage, Region full, Region blocks, uint32_t layer,
                                    uint64_t address, uint64_t output, const NvnTextureFootprint *footprint)
{
    const size_t storage_bytes = storage->layer_stride * storage->layers;
    uint8_t *destination = output ? upload->write_guest(upload->guest_opaque, output, footprint->packed_bytes) : NULL;
    if (!destination)
        return NVN_FRONT_INVALID_GUEST;
    const bool download = texture->layout != VK_IMAGE_LAYOUT_UNDEFINED;
    uint8_t *canonical = download ? upload->write_guest(upload->guest_opaque, address, storage_bytes) : NULL;
    const uint8_t *source = download ? canonical : upload->read_guest(upload->guest_opaque, address, storage_bytes);
    if (!source)
        return NVN_FRONT_INVALID_GUEST;
    source += layer * storage->layer_stride;
    const uintptr_t src = (uintptr_t)source, dst = (uintptr_t)destination;
    const bool overlap = dst >= src ? dst - src < storage->layer_stride : src - dst < footprint->packed_bytes;
    uint8_t *linear = overlap ? malloc(footprint->packed_bytes) : destination;
    if (!linear)
        return NVN_FRONT_BOUNDARY;
    const VkResult result = download ? read_image(upload->adapter, texture, storage, full, canonical) : VK_SUCCESS;
    if (result == VK_SUCCESS) {
        unpack_region(storage, blocks, source, linear, footprint->row_bytes);
        if (overlap)
            memcpy(destination, linear, footprint->packed_bytes);
    }
    if (overlap)
        free(linear);
    if (upload->log && (result != VK_SUCCESS || upload->logged < 12)) {
        fprintf(upload->log, "NVN read texture %ux%u guest=%#" PRIx64 " bytes=%zu Vulkan=%u result=%d\n",
                full.width, full.height, address, footprint->packed_bytes, (unsigned)download, (int)result);
        fflush(upload->log);
        ++upload->logged;
    }
    return result == VK_SUCCESS ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

NvnFrontReplyKind pokopia_live_texture_upload_dispatch(PokopiaLiveTextureUpload *upload,
                                                       const char *name, const uint64_t x[32])
{
    if (!name)
        return NVN_FRONT_NOT_OURS;
    const bool flush = !strcmp(name, "nvnTextureFlushTexels");
    const bool invalidate = !strcmp(name, "nvnTextureInvalidateTexels");
    const bool read = !strcmp(name, "nvnTextureReadTexels");
    if (!flush && !invalidate && !read && strcmp(name, "nvnTextureWriteTexels"))
        return !strcmp(name, "nvnTextureWriteTexelsStrided") || !strcmp(name, "nvnTextureReadTexelsStrided")
             ? NVN_FRONT_BOUNDARY : NVN_FRONT_NOT_OURS;
    if (!upload || !upload->front || !upload->adapter || !upload->read_guest || !upload->write_guest || !x)
        return NVN_FRONT_BOUNDARY;

    NvnFrontTextureInfo info;
    if (!nvn_front_get_texture(upload->front, x[0], &info))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkTexture *texture = info.native;
    Storage storage;
    if (!texture || (!texture->backing.image && !texture->deferred) ||
        texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
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

    /* R89: a CPU write or flush changes the image without a render serial. */
    if (!read) {
        Shadow *shadow = shadow_find(texture);
        if (shadow)
            shadow->serial = 0;
    }
    /* With no sampled backing, the pool already contains the flushed bytes. */
    if (flush && texture->deferred)
        return upload->read_guest(upload->guest_opaque, address, storage_bytes) ?
            NVN_FRONT_HANDLED : NVN_FRONT_INVALID_GUEST;

    /* CPU texel calls select the view's first mip and layer, using the base
     * storage format independently of sampling format and swizzle. */
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

    if (read)
        return read_texels(upload, texture, &storage, full, blocks, layer, address, x[3], &region_footprint);

    /* First use and flush initialize all layers together. A first partial
     * write preserves the untouched texels from canonical guest storage. */
    const bool full_upload = !texture->deferred && (flush || texture->layout == VK_IMAGE_LAYOUT_UNDEFINED);
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

    const VkResult result = texture->deferred ? VK_SUCCESS :
        stage_image(upload, texture, full, transfer, full_upload ? 0 : layer, layers,
                    layers == storage.layers, linear, count);
    if (result == VK_SUCCESS && !flush)
        pack_region(&storage, blocks, patch, pitch, destination + layer * storage.layer_stride);
    free(linear);

    if (upload->log && (result != VK_SUCCESS || upload->logged < 12)) {
        fprintf(upload->log, "NVN %s %s texture %ux%u region %u,%u %ux%u guest=%#" PRIx64
            " bytes=%zu result=%d\n", flush ? "flush" : "write", texture->deferred ? "deferred" : "Vulkan",
            full.width, full.height, region.x, region.y, region.width, region.height, address, count, (int)result);
        fflush(upload->log);
        ++upload->logged;
    }
    return result == VK_SUCCESS ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

/* R90: textures already prepared during this guest submit. Every draw used
 * to run the checks above for each of its textures (~550,000 draws per 5 s
 * outdoors, ~0.8 s of the replay worker), touching each texture's state.
 * A texture is remembered once it has Vulkan contents (or was rendered); the
 * table is forgotten at each guest submit, when CPU-written textures may need
 * a new sync. */
enum { PREPARED_SLOTS = 1024 };
static struct {
    const void *texture;
    uint64_t epoch;
} prepared[PREPARED_SLOTS];
uint64_t pokopia_prepare_skips;

VkResult pokopia_live_texture_prepare_sample(PokopiaLiveTextureUpload *upload, const NvnFrontTextureInfo *info)
{
    if (!info || !info->native)
        return prepare_sample_slow(upload, info);
    const PokopiaVkTexture *texture = info->native;
    /* R92: after a prompts/confirm change, a glyph texture is uploaded again
     * from the game's bytes with the matching patch when next sampled. */
    const unsigned generation = atomic_load_explicit(&pokopia_glyph_generation, memory_order_relaxed);
    if (generation != glyph_seen_generation) {
        glyph_seen_generation = generation;
        for (unsigned i = 0; i < 4; ++i)
            glyph_textures[i].stale = glyph_textures[i].texture != NULL;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (glyph_textures[i].texture == texture && glyph_textures[i].stale) {
            glyph_textures[i].stale = false;
            inline_allowed = true;
            (void)prepare_deferred(upload, (PokopiaVkTexture *)texture, &info->create_info);
            inline_allowed = false;
        }
    const size_t at = (size_t)(((uintptr_t)texture >> 4) * UINT64_C(0x9e3779b97f4a7c15) >> 54) & (PREPARED_SLOTS - 1);
    if (prepared[at].texture == texture && prepared[at].epoch == pokopia_replay_job_epoch) {
        ++pokopia_prepare_skips;
        return VK_SUCCESS;
    }
    inline_allowed = true;
    const VkResult result = prepare_sample_slow(upload, info);
    inline_allowed = false;
    const NvnFrontTextureConfig *config = &info->create_info.config;
    /* The 2x1 RGBA32F CPU-uncached pair is copied on every sample. */
    if (result == VK_SUCCESS && !texture->deferred &&
        (texture->layout != VK_IMAGE_LAYOUT_UNDEFINED || texture->render_serial) &&
        !(config->width == 2 && config->height == 1 && texture->format == VK_FORMAT_R32G32B32A32_SFLOAT)) {
        prepared[at].texture = texture;
        prepared[at].epoch = pokopia_replay_job_epoch;
    }
    return result;
}
