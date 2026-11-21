/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_REPLAY_H
#define POKOPIA_NVN_REPLAY_H

#include "live_compute.h"
#include "live_draw.h"

typedef struct NvnReplay NvnReplay;

typedef enum {
    NVN_REPLAY_MEMORY_COMMAND = 0,
    NVN_REPLAY_MEMORY_CONTROL = 1,
} NvnReplayMemoryEvent;

typedef struct {
    uint64_t encoder, function, data, minimum_bytes, revision;
    void *native;
    NvnReplayMemoryEvent event;
} NvnReplayMemoryRequest;

typedef struct {
    NvnFront *front;
    void *opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t bytes);
    void *(*write_guest)(void *opaque, uint64_t address, size_t bytes);
    /* Copy commits to canonical guest memory after GPU completion. */
    bool (*copy)(void *opaque, const void *source, void *destination, size_t bytes);
    bool (*clear)(void *opaque, void *texture, VkRect2D area, const float rgba[4], uint32_t mask);
    bool (*clear_depth_stencil)(void *opaque, void *texture, VkRect2D area,
                                VkImageAspectFlags aspects, VkClearDepthStencilValue value);
    bool (*draw)(void *opaque, const PokopiaLiveDraw *draw);
    bool (*compute)(void *opaque, const PokopiaLiveCompute *dispatch);
    bool (*sync)(void *opaque, void *sync, bool wait);
    bool (*finish)(void *opaque);
    uint64_t (*timestamp_ns)(void);
} NvnReplayConfig;

/* The caller serializes dispatch with guest object lifetimes and Vulkan queue use. */
NvnReplay *nvn_replay_create(const NvnReplayConfig *config);
NvnFrontReplyKind nvn_replay_dispatch(NvnReplay *replay, const char *name, uint64_t x[32],
                                     const uint32_t float_args[3]);
/* Take a memory request under the dispatch lock. Invoke the registered callback
 * unlocked and retry after the same encoder receives the requested memory. */
bool nvn_replay_take_memory_request(NvnReplay *replay, NvnReplayMemoryRequest *request);
void nvn_replay_destroy(NvnReplay *replay);

#endif
