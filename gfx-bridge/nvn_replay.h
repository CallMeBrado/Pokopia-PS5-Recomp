/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_REPLAY_H
#define POKOPIA_NVN_REPLAY_H

#include "live_compute.h"
#include "live_draw.h"

#include <stdio.h>

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

/* nvnCommandBufferCopyTextureToTexture, resolved to one level and a range of
 * layers (or a 3D slab) per side, regions clipped to each level. */
typedef struct {
    NvnFrontTextureInfo texture;
    uint32_t level, layer, layers;
    bool volume;
    int32_t x, y, z, width, height, depth;
} NvnReplayTextureCopySide;

typedef struct {
    NvnReplayTextureCopySide source, destination;
    bool linear;
} NvnReplayTextureCopy;

typedef struct NvnReplayJob NvnReplayJob;
#if defined(__PROSPERO__)
void nvn_replay_trace_submission(const NvnReplayJob *job, uint64_t seq);
void pokopia_trace_producer(const char *kind, uint64_t ns, uint64_t first, uint64_t second);
#endif

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
    /* R61: general draw path; NULL keeps such draws skipped. */
    bool (*draw_generic)(void *opaque, const PokopiaGenericDraw *draw);
    bool (*compute)(void *opaque, const PokopiaLiveCompute *dispatch);
    /* R73: general compute; NULL keeps unknown dispatches skipped. */
    bool (*compute_generic)(void *opaque, const PokopiaGenericCompute *dispatch);
    /* NULL, or false on execution, skips the copy (counted) instead of stopping. */
    bool (*copy_texture)(void *opaque, const NvnReplayTextureCopy *copy);
    bool (*sync)(void *opaque, void *sync, bool wait);
    bool (*finish)(void *opaque);
    uint64_t (*timestamp_ns)(void);
    /* R68: optional. Called around a submit's execute pass, which only reads
     * recorded lists, guest memory and graphics objects; the caller may let
     * other guest threads run (but not other graphics calls) in between. */
    void (*execute_begin)(void *opaque);
    void (*execute_end)(void *opaque);
    /* R76: optional. Called between executed operations. The caller may let
     * other threads make graphics calls here; lists being executed are pinned
     * and every operation still runs whole under the caller's lock. */
    void (*execute_yield)(void *opaque);
    /* R77: optional. When set, a submit validates its lists on the calling
     * thread and hands the execute pass to the caller as a job, run later (in
     * order, on any thread holding the caller's graphics lock) with
     * nvn_replay_run_job(). Returns false to execute in place instead. */
    bool (*enqueue)(void *opaque, NvnReplayJob *job);
    /* R77: optional. During validation, for every fence sync a queued job will
     * signal; the caller orders waits on it after that job. */
    void (*sync_queued)(void *opaque, void *sync);
} NvnReplayConfig;

/* R77: runs (and frees) a job handed to config->enqueue. */
NvnFrontReplyKind nvn_replay_run_job(NvnReplay *replay, NvnReplayJob *job);

/* The caller serializes dispatch with guest object lifetimes and Vulkan queue use. */
NvnReplay *nvn_replay_create(const NvnReplayConfig *config);
NvnFrontReplyKind nvn_replay_dispatch(NvnReplay *replay, const char *name, uint64_t x[32],
                                     const uint32_t float_args[3]);
/* static_key is NULL or the matching name from the front's static proc table.
 * The caller serializes the per-replay cache along with recording dispatch. */
NvnFrontReplyKind nvn_replay_dispatch_keyed(NvnReplay *replay, const char *name, const char *static_key,
                                           uint64_t x[32], const uint32_t float_args[3]);
/* Take a memory request under the dispatch lock. Invoke the registered callback
 * unlocked and retry after the same encoder receives the requested memory. */
bool nvn_replay_take_memory_request(NvnReplay *replay, NvnReplayMemoryRequest *request);
void nvn_replay_destroy(NvnReplay *replay);
/* R50: draws whose shaders or state the bridge cannot translate are skipped
 * (with command memory still charged) instead of stopping the game. Prints
 * and resets the counts, plus each unsupported shader hash the first time. */
void nvn_replay_report_skips(FILE *log);
/* R62: the pinned draw callback sets this when it skipped a draw as
 * unsupported, so replay retries the draw on the general path. */
extern bool nvn_replay_draw_unsupported;
/* R63: why the last BOUNDARY reply from EndRecording happened (NULL if unknown). */
extern const char *nvn_replay_boundary_reason;

#endif
