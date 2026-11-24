/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_THREAD_H
#define POKOPIA_HORIZON_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ctx Ctx;
typedef struct HznThreads HznThreads;
typedef struct HznEvents HznEvents;

typedef struct {
    void *opaque;
    Ctx *main_context;
    uint32_t main_handle, worker_handle_base;
    /* Exclusive worker handle limit. Zero uses the pseudo-handle boundary. */
    uint32_t worker_handle_limit;
    uint64_t main_id;
    int32_t main_priority;
    /* NPDM thread capabilities. A zero core mask selects the three-core
     * baseline. A zero priority mask permits every priority. */
    uint64_t process_core_mask;
    uint64_t process_priority_mask;
    int32_t main_core;
    uint32_t (*validate_start)(void *opaque, uint64_t entry, uint64_t stack_top);
    /* Allocate zeroed guest TLS and TCB, then apply any static TLS template.
     * The TLS mapping must be writable through TLS +0x1f7. The runtime writes
     * the TCB pointer at +0x1f0. Callbacks must not re-enter this thread table. */
    bool (*allocate_tls)(void *opaque, uint64_t *tls, uint64_t *tcb);
    void (*release_tls)(void *opaque, uint64_t tls, uint64_t tcb);
    void (*dispatch)(void *opaque, Ctx *ctx, uint64_t entry);
    /* Use guarded Orbis worker stacks with flexible memory when it has room,
     * falling back to direct memory. */
    bool direct_worker_stacks;
    void (*worker_error)(void *opaque, uint32_t handle, const char *operation, int32_t error);
    /* Optional waitable table. Keep it alive until every thread is joined. */
    HznEvents *events;
    /* Optional host timer for sub-millisecond sleeps on the main thread. */
    bool (*use_precise_main_sleep)(void);
    uint64_t (*precise_sleep_clock)(void);
} HznThreadConfig;

HznThreads *hzn_threads_create(const HznThreadConfig *config);

#if defined(__PROSPERO__)
/* Temporarily bind the calling host thread to a core in its allowed set. */
void hzn_thread_test_affinity(unsigned cpu_rank, bool enabled);
unsigned hzn_thread_process_cpu_count(void);
#endif

/* Run before other SVC handlers so workers can observe process teardown at
 * every SVC. False means another service owns the call or CloseHandle value. */
bool hzn_thread_svc(HznThreads *threads, Ctx *ctx, uint32_t number);

/* Zero means an invalid handle. The current-thread pseudo-handle resolves
 * against the caller's context, including after a real handle is closed. */
uint64_t hzn_thread_id(HznThreads *threads, const Ctx *caller, uint32_t handle);
int32_t hzn_thread_priority(HznThreads *threads, const Ctx *caller);

/* Mark the process closing and wake workers that are in SleepThread. */
void hzn_threads_request_exit(HznThreads *threads);

/* Call from the host main thread before unmapping guest code, stack or TLS.
 * Requests cooperative worker exit at the next SVC and joins all workers.
 * No external caller may enter the table during shutdown. */
bool hzn_threads_destroy(HznThreads *threads);

#ifdef __cplusplus
}
#endif
/* R71: SleepThread calls longer than 17 ms, by guest thread and caller. */
typedef struct {
    uint64_t id, lr, ns, count;
} HznSleepSite;
extern HznSleepSite hzn_long_sleeps[16];

#endif
