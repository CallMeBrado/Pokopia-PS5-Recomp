/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_EVENT_H
#define POKOPIA_HORIZON_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HZN_EVENT_HANDLE_BASE = 0x30000, HZN_EVENT_HANDLE_END = 0x40000 };

typedef struct HznEvents HznEvents;

typedef struct {
    void *opaque;
    /* Validate and snapshot the entire guest array. The event lock is not held. */
    bool (*copy_handles)(void *opaque, uint64_t guest_addr, uint32_t *out, size_t count);
} HznEventConfig;

HznEvents *hzn_events_create(const HznEventConfig *config);
/* WaitSynchronization may block. Enter it without the loader lock and pass
 * the current thread's nonzero, stable ID. Other SVCs ignore the ID. */
bool hzn_event_svc(HznEvents *events, uint32_t number, uint64_t x[32], uint64_t waiter_id);

/* Each successful copy gives the guest a new readable handle to the same
 * service-owned event. Keys are local to this table, and zero is a valid key. */
uint32_t hzn_events_copy_read(HznEvents *events, uint64_t key);
bool hzn_events_set_signaled(HznEvents *events, uint64_t key, bool signaled);

/* Thread objects are level-signaled on exit and share WaitSynchronization
 * with readable events. Closing hides a handle from new waits; registered
 * waits keep their references until exit, timeout or cancellation. */
bool hzn_events_add_thread(HznEvents *events, uint32_t handle, uint64_t id);
void hzn_events_close_thread(HznEvents *events, uint32_t handle);
void hzn_events_exit_thread(HznEvents *events, uint32_t handle);
void hzn_events_remove_thread(HznEvents *events, uint32_t handle);

/* CancelSynchronization targets one thread. When it has no armed wait, the
 * next wait that would block consumes the pending cancellation. */
uint32_t hzn_events_cancel_wait(HznEvents *events, uint64_t waiter_id);

/* Stop new waits and wake pending waiters. Join all callers before destroy. */
void hzn_events_cancel_all(HznEvents *events);
void hzn_events_destroy(HznEvents *events);

#ifdef __cplusplus
}
#endif
#endif
