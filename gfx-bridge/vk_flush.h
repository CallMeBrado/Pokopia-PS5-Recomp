/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_FLUSH_H
#define POKOPIA_VK_FLUSH_H

/* R75: general draws are recorded into one command buffer and submitted
 * together. Anything that submits other GPU work, waits for the queue, reads
 * GPU results, rewrites host memory the GPU reads, or destroys a Vulkan object
 * a pending draw may use calls pokopia_vk_flush_pending() first. The hook is
 * set by the batching code (live_draw_generic.inc) and is reentrancy-safe. */
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

enum {
    POKOPIA_FLUSH_UPLOAD, POKOPIA_FLUSH_CLOSE_IMAGE, POKOPIA_FLUSH_TEXTURE_CLOSE, POKOPIA_FLUSH_SUBMIT_WAIT,
    POKOPIA_FLUSH_TEXEL_CLOSE, POKOPIA_FLUSH_TEXEL_REFRESH, POKOPIA_FLUSH_FINISH, POKOPIA_FLUSH_SYNC,
    POKOPIA_FLUSH_PRESENT, POKOPIA_FLUSH_QUEUE_SUBMIT, POKOPIA_FLUSH_REASONS
};

/* Returns true when it submitted or waited for pending draws. With wait set,
 * every submitted batch has completed when it returns (R77). */
extern bool (*pokopia_vk_flush_hook)(bool wait);
/* Flushes that submitted something, by reason (reset by the stats reporter). */
extern uint64_t pokopia_vk_flush_counts[POKOPIA_FLUSH_REASONS];
/* Number of the open draw batch; advanced by every flush. Objects stamped
 * with it are read by a pending draw. Starts at 1 so a zero stamp never matches. */
extern uint64_t pokopia_vk_batch_id;
/* R77: highest batch id known complete on the GPU. */
extern uint64_t pokopia_vk_batch_retired;
/* Advanced whenever replay writes guest memory between draws (buffer copies);
 * general draws reuse an upload of the same guest range only within one value. */
extern uint64_t pokopia_vk_guest_write_epoch;
/* R78: advanced at the start of every replayed guest submit. */
extern uint64_t pokopia_replay_job_epoch;
/* R78: called before an image is destroyed, so views cached for it go too. */
extern void (*pokopia_vk_image_forget_hook)(VkImage image);
/* R78: called right after the draw batch is submitted (deferred fence signals). */
extern void (*pokopia_vk_after_submit_hook)(void);
/* R78: the open draw batch's command buffer (outside any rendering) for work
 * recorded in order with the draws, or VK_NULL_HANDLE. */
extern VkCommandBuffer (*pokopia_vk_batch_command_hook)(void);
/* R89: called with the draw batch's command buffer just before it is ended and
 * submitted (outside rendering), to record work that must follow its draws. */
extern void (*pokopia_vk_before_batch_end_hook)(VkCommandBuffer command);
/* R89: called with a PokopiaVkTexture before it is closed. */
extern void (*pokopia_vk_texture_forget_hook)(void *texture);
/* R89: waits for one submitted draw batch (by id) and the ones before it;
 * true once pokopia_vk_batch_retired >= id. */
bool pokopia_live_generic_wait_batch(uint64_t id);
/* R78: true while general draws are being recorded into an unsubmitted batch. */
bool pokopia_live_generic_batch_open(void);
/* R90: A/B switches read from debug/flags in the mailbox (bit 0: no
 * redundant-bind skipping; bit 1: no batch submit at fence syncs;
 * bit 4: use the graphics lock for image acquire; bit 5: retain all draws). */
extern volatile unsigned pokopia_debug_flags;
enum { POKOPIA_DEBUG_SAMPLE_NVNS = 1u << 18, POKOPIA_DEBUG_SAMPLE_DRAWS = 1u << 19 };
/* R94: bit 15 turns off per-call NVN timing. POKOPIA_DEBUG_DEFAULTS apply when
 * there is no debug/flags file: the switches measured faster on the PS5 (bit 9
 * non-blocking sync polls, 15, 18-19 sampled timing, 20 quiet acquire log, 21
 * direct draw replay, 22 last-pipeline cache, 24 texture descriptor reuse). */
enum { POKOPIA_DEBUG_NO_NVN_TIMING = 1u << 15 };
#define POKOPIA_DEBUG_DEFAULTS     ((1u << 9) | (1u << 15) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 24))

/* Sample once per group with a varying offset to cover repeated draw patterns. */
static inline bool pokopia_timing_sample(uint64_t iteration)
{
    return ((iteration ^ (iteration >> 4)) & 15u) == 0;
}
/* R90: number of draws in the open batch (0 when none is open). */
uint32_t pokopia_live_generic_batch_count(void);
/* R76: cheap monotonic clock (invariant TSC) set by the loader; replay and draw
 * timing called clock_gettime, which enters the kernel on the PS5 (~0.9 us). */
extern uint64_t (*pokopia_now_ns)(void);

/* R77: a guest queue submit, a present or a fence signal only needs the draws
 * queued ahead of it on the same VkQueue; everything else needs them finished. */
static inline void pokopia_vk_flush_pending(unsigned reason)
{
    const bool wait = reason != POKOPIA_FLUSH_QUEUE_SUBMIT && reason != POKOPIA_FLUSH_PRESENT &&
                      reason != POKOPIA_FLUSH_SYNC;
    if (pokopia_vk_flush_hook && pokopia_vk_flush_hook(wait))
        ++pokopia_vk_flush_counts[reason];
}

#endif
