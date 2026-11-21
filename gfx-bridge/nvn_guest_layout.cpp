/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_guest_layout.h"

#include "nvn_format.h"
#include "nvn_texture_layout.h"
#include "nxrt/game.h"
#include "nxrt/log.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>

/* The portable upstream layout only uses this for unique packaged shapes. */
void nxrt::logf(LogLevel, const char *tag, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    std::fprintf(stderr, "[NVN] %s %s\n", tag, message);
}

static bool texture_builder(const NvnFrontTextureConfig *config, nxrt::nvn::TextureBuilderInfo &builder)
{
    if (!config || config->gl_texture_name || (config->flags & NVN_TEXTURE_FLAGS_SPARSE))
        return false;

    /* NVN2 device limits also bound the portable layout's arithmetic. */
    const bool buffer = config->target == NVN_TEXTURE_TARGET_BUFFER;
    const bool volume = config->target == NVN_TEXTURE_TARGET_3D;
    const uint32_t max_edge = buffer ? 134217728u : volume ? 2048u : 16384u;
    if (!config->width || config->width > max_edge || config->height > max_edge ||
        config->depth > 2048 || config->levels > 16 || config->samples > 16 ||
        config->stride > INT_MAX || config->flags > INT_MAX ||
        config->format > INT_MAX || config->target > INT_MAX)
        return false;

    /* Builder+0x54 puts the presence bit below the three packed shape nibbles. */
    const bool packaged = (config->packaged_layout & 1u) != 0;
    const uint16_t shape = config->packaged_layout >> 4;
    if (packaged && !(config->flags & NVN_TEXTURE_FLAGS_LINEAR) &&
        ((shape & 0xf00u) || (shape & 0x00fu) > 5 || (volume && (shape & 0x0f0u) > 0x50u)))
        return false;

    static constexpr nxrt::game::Info platform{.platform = nxrt::game::Platform::Switch2};
    static const bool platform_registered = [] {
        nxrt::game::set_current(&platform);
        return true;
    }();
    (void)platform_registered;

    builder.flags = static_cast<int>(config->flags);
    builder.target = static_cast<int>(config->target);
    builder.width = static_cast<int>(config->width);
    builder.height = static_cast<int>(config->height);
    builder.depth = static_cast<int>(config->depth);
    builder.levels = static_cast<int>(config->levels);
    builder.format = static_cast<int>(config->format);
    builder.samples = static_cast<int>(config->samples);
    builder.stride = static_cast<ptrdiff_t>(config->stride);
    builder.has_packaged_layout = packaged;
    builder.packaged_layout[0] = static_cast<uint8_t>(shape);
    builder.packaged_layout[1] = static_cast<uint8_t>(shape >> 8);
    return true;
}

bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config,
    uint64_t *storage_size, uint64_t *alignment)
{
    nxrt::nvn::TextureBuilderInfo builder;
    if (!storage_size || !alignment || storage_size == alignment || !texture_builder(config, builder))
        return false;
    const auto layout = nxrt::nvn::be::compute_texture_layout(builder);
    if (!layout.valid || !layout.storage_size || !layout.alignment)
        return false;

    *storage_size = layout.storage_size;
    *alignment = layout.alignment;
    return true;
}

static VkResult untile_texture(const NvnFrontTextureConfig *config, const void *tiled,
    size_t tiled_bytes, NvnGuestCompressedMipUpload upload, void *opaque, bool compressed_only);

VkResult nvn_guest_untile_compressed(const NvnFrontTextureConfig *config, const void *tiled,
    size_t tiled_bytes, NvnGuestCompressedMipUpload upload, void *opaque)
{
    return untile_texture(config, tiled, tiled_bytes, upload, opaque, true);
}

VkResult nvn_guest_untile(const NvnFrontTextureConfig *config, const void *tiled,
    size_t tiled_bytes, NvnGuestCompressedMipUpload upload, void *opaque)
{
    return untile_texture(config, tiled, tiled_bytes, upload, opaque, false);
}

static VkResult untile_texture(const NvnFrontTextureConfig *config, const void *tiled,
    size_t tiled_bytes, NvnGuestCompressedMipUpload upload, void *opaque, bool compressed_only)
{
    if (!config || !tiled || !upload)
        return VK_ERROR_INITIALIZATION_FAILED;

    const bool cube = config->target == NVN_TEXTURE_TARGET_CUBEMAP;
    const bool cube_array = config->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    const bool array = config->target == NVN_TEXTURE_TARGET_2D_ARRAY;
    /* R73: 3D textures (colour LUT volumes): one layer, z slices per level. */
    const bool volume = config->target == NVN_TEXTURE_TARGET_3D;
    const bool valid_depth = array || cube_array || volume || config->depth == 1 || (cube && config->depth == 6);
    if ((!array && !cube && !cube_array && !volume && config->target != NVN_TEXTURE_TARGET_2D) ||
        ((cube || cube_array) && config->width != config->height) ||
        !valid_depth || !config->levels || config->stride || config->samples > 1)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    nxrt::nvn::TextureBuilderInfo builder;
    if (!texture_builder(config, builder))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const auto texture = nxrt::nvn::be::compute_texture_layout(builder);
    if (!texture.valid || !texture.block_linear || (compressed_only && !texture.fmt->compressed) ||
        texture.levels != config->levels)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    const auto &layout = texture.layout;
    const uint64_t stride = layout.tiled_layer_stride;
    if (!stride || !texture.layers || layout.tiled_size != tiled_bytes ||
        texture.storage_size != tiled_bytes || tiled_bytes % texture.layers ||
        tiled_bytes / texture.layers != stride)
        return VK_ERROR_INITIALIZATION_FAILED;

    constexpr size_t batch_target = 2 * 1024 * 1024;
    uint32_t layers_per_batch[16] = {};
    size_t scratch_bytes = 0;
    for (uint32_t mip = 0; mip < texture.levels; ++mip) {
        const auto &level = layout.levels[mip];
        const uint64_t row = static_cast<uint64_t>(level.blocks_x) * texture.fmt->bytes;
        if ((volume ? texture.layers != 1 : level.depth != 1) || level.tiled_offset > stride ||
            level.tiled_size > stride - level.tiled_offset ||
            level.linear_row_stride != row || level.linear_slice_size != row * level.blocks_y * level.depth ||
            !level.linear_slice_size || level.linear_slice_size > SIZE_MAX)
            return VK_ERROR_INITIALIZATION_FAILED;

        const size_t layer_bytes = static_cast<size_t>(level.linear_slice_size);
        const size_t batch = std::min<size_t>(texture.layers, std::max<size_t>(1, batch_target / layer_bytes));
        layers_per_batch[mip] = static_cast<uint32_t>(batch);
        scratch_bytes = std::max(scratch_bytes, layer_bytes * batch);
    }

    auto *linear = static_cast<uint8_t *>(std::malloc(scratch_bytes));
    if (!linear)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto *source = static_cast<const uint8_t *>(tiled);
    VkResult result = VK_SUCCESS;
    for (uint32_t mip = 0; mip < texture.levels && result == VK_SUCCESS; ++mip) {
        const auto &level = layout.levels[mip];
        const size_t layer_bytes = static_cast<size_t>(level.linear_slice_size);
        for (uint32_t first = 0; first < texture.layers && result == VK_SUCCESS;) {
            const uint32_t layers = std::min(layers_per_batch[mip], texture.layers - first);
            for (uint32_t i = 0; i < layers; ++i) {
                const uint64_t offset = (first + i) * stride + level.tiled_offset;
                tegra::untile_level(texture.desc, level, source + offset, linear + i * layer_bytes);
            }
            /* A volume level passes its z slices as the layer count; the
             * callback knows the texture is 3D. */
            result = upload(opaque, {level.width, level.height}, mip, first, volume ? level.depth : layers, linear,
                            layers * layer_bytes);
            first += layers;
        }
    }
    std::free(linear);
    return result;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    if (format > INT_MAX)
        return VK_FORMAT_UNDEFINED;

    const auto *info = nxrt::nvn::format_info(static_cast<int>(format));
    if (!info || (format >= NVN_FORMAT_RGBX8 && format <= NVN_FORMAT_RGBX32I))
        return VK_FORMAT_UNDEFINED;

    switch (format) {
    /* X-channel aliases need a view swizzle. The pinned table also flags
     * conflicting definitions for the packed 555 formats below. */
    case NVN_FORMAT_RGBX8_SRGB:
    case NVN_FORMAT_BGRX8:
    case NVN_FORMAT_BGRX8_SRGB:
    case NVN_FORMAT_RGB5:
    case NVN_FORMAT_RGB5A1:
    case NVN_FORMAT_BGR5:
    case NVN_FORMAT_A1BGR5:
    /* NVN stores eight bytes per texel. Vulkan's D32S8 block has five. */
    case NVN_FORMAT_DEPTH32F_STENCIL8:
        return VK_FORMAT_UNDEFINED;
    default:
        return info->vk;
    }
}

uint32_t nvn_guest_buffer_texel_bytes(uint32_t format)
{
    if (format > INT_MAX)
        return 0;
    const auto *info = nxrt::nvn::format_info(static_cast<int>(format));
    return info && !info->compressed && !info->srgb && info->aspect == nxrt::nvn::kAspectColor
        ? info->bytes : 0;
}
