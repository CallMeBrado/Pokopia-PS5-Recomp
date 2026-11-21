/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_guest_layout.h"

#include "nvn_format.h"
#include "nvn_texture_layout.h"
#include "nxrt/game.h"
#include "nxrt/log.h"

#include <climits>
#include <cstdio>

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

bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config,
    uint64_t *storage_size, uint64_t *alignment)
{
    if (!config || !storage_size || !alignment || storage_size == alignment ||
        config->gl_texture_name || (config->flags & NVN_TEXTURE_FLAGS_SPARSE))
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

    nxrt::nvn::TextureBuilderInfo builder;
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

    const auto layout = nxrt::nvn::be::compute_texture_layout(builder);
    if (!layout.valid || !layout.storage_size || !layout.alignment)
        return false;

    *storage_size = layout.storage_size;
    *alignment = layout.alignment;
    return true;
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
