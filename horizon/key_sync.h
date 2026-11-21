/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_KEY_SYNC_H
#define POKOPIA_HORIZON_KEY_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HZN_KEY_SYNC_WAIT_MASK UINT32_C(0x40000000)

typedef struct HznKeySync HznKeySync;

typedef struct {
    void *opaque;
    /* Resolve one writable, naturally aligned guest word. It must stay
     * mapped until the calling SVC, including a blocked wait, has returned. */
    uint32_t *(*word)(void *opaque, uint64_t address);
    /* Resolve real guest thread handles to stable IDs, or zero. Called under
     * the key lock. It must not reenter this backend or take the loader lock. */
    uint64_t (*thread_id)(void *opaque, uint32_t handle);
} HznKeySyncConfig;

HznKeySync *hzn_key_sync_create(const HznKeySyncConfig *config);

/* Handles SVCs 0x1a through 0x1d, 0x34 and 0x35. Call outside the loader lock
 * with the current thread's stable ID and priority. Lower priority numbers
 * wake first. The two words of a 64-bit address wait must be contiguous. */
bool hzn_key_sync_svc(HznKeySync *sync, uint32_t number, uint64_t x[32],
                     uint64_t current_thread_id, int32_t current_priority);

/* Reject new waits and wake blocked callers with TerminationRequested.
 * Stop and join all SVC users before destroying this object. */
void hzn_key_sync_cancel_all(HznKeySync *sync);
void hzn_key_sync_destroy(HznKeySync *sync);

#ifdef __cplusplus
}
#endif
#endif
