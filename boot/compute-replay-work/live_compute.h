/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_COMPUTE_H
#define POKOPIA_LIVE_COMPUTE_H

#include "vk_adapter.h"

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

/* Queue and guest ranges are externally serialized. The texel buffers must
 * already contain canonical guest data. Publish storage writes after successful
 * fence retirement. An error with an unretired submission is terminal. */
VkResult pokopia_live_compute(PokopiaVkAdapter *adapter, const PokopiaLiveCompute *dispatch);

#endif
