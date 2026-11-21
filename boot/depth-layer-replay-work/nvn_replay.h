/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_REPLAY_H
#define POKOPIA_NVN_REPLAY_H

#include "live_compute.h"
#include "live_draw.h"

typedef struct NvnReplay NvnReplay;

typedef struct {
    NvnFront *front;
    void *opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t bytes);
    void *(*write_guest)(void *opaque, uint64_t address, size_t bytes);
    /* Copy commits to canonical guest memory after GPU completion. */
    bool (*copy)(void *opaque, const void *source, void *destination, size_t bytes);
    bool (*clear)(void *opaque, void *texture, const float rgba[4], uint32_t mask);
    bool (*clear_depth_stencil)(void *opaque, void *texture, VkRect2D area,
                                VkImageAspectFlags aspects, VkClearDepthStencilValue value);
    bool (*draw)(void *opaque, const PokopiaLiveDraw *draw);
    bool (*compute)(void *opaque, const PokopiaLiveCompute *dispatch);
    bool (*sync)(void *opaque, void *sync, bool wait);
    bool (*finish)(void *opaque);
} NvnReplayConfig;

/* The caller serializes dispatch with guest object lifetimes and Vulkan queue use. */
NvnReplay *nvn_replay_create(const NvnReplayConfig *config);
NvnFrontReplyKind nvn_replay_dispatch(NvnReplay *replay, const char *name, uint64_t x[32],
                                     const uint32_t float_args[3]);
void nvn_replay_destroy(NvnReplay *replay);

#endif
