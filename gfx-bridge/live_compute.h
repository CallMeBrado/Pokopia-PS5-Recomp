/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_COMPUTE_H
#define POKOPIA_LIVE_COMPUTE_H

#include "vk_adapter.h"
#include <string.h>

typedef struct {
    void *data;
    size_t bytes;
} PokopiaComputeStorage;

typedef struct {
    VkShaderModule module;
    const char *entry, *hash;
    const void *uniform;
    size_t uniform_bytes;
    uint32_t groups[3];
    PokopiaComputeStorage storage[2]; /* Vulkan set 1, in binding order. */
    const PokopiaVkTexel *texels[2];   /* Vulkan set 2, bindings 64 and 65. */
} PokopiaLiveCompute;

/* b81c48ff486acef4 tiles the screen in 64x64 cells and 16 depth slices.
 * Its output has two header words per cell followed by a million light indices. */
static inline size_t pokopia_cluster_output_bytes(const uint32_t groups[3])
{
    const uint32_t x = groups[0], y = groups[1], z = groups[2];
    if (z != 16 || !((x == 30 && y == 17) || (x == 25 && y == 15) || (x == 20 && y == 12)))
        return 0;
    return (1000000u + 2u * x * y * z) * sizeof(uint32_t);
}

static inline bool pokopia_cluster_extent_matches(const uint32_t groups[3], const void *uniform)
{
    uint32_t extent[2];
    memcpy(extent, (const uint8_t *)uniform + 0x38, sizeof extent);
    return extent[0] && extent[1] && (extent[0] - 1) / 64 + 1 == groups[0] &&
           (extent[1] - 1) / 64 + 1 == groups[1];
}

/* Queue and guest ranges are externally serialized. The texel buffers must
 * already contain canonical guest data. Publish storage writes after successful
 * fence retirement. An error with an unretired submission is terminal. */
VkResult pokopia_live_compute(PokopiaVkAdapter *adapter, const PokopiaLiveCompute *dispatch);

#endif
