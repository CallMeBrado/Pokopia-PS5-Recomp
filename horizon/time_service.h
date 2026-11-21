/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_TIME_SERVICE_H
#define POKOPIA_HORIZON_TIME_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HZN_TIME_PAGE_BYTES = 0x1000 };

typedef struct {
    /* Sample both clocks at the same point. The steady sample must use the
     * same counter and conversion as guest CNTVCT_EL0. */
    uint64_t tick_ns;
    int64_t utc_ns;
    uint8_t source_id[16];
    void *opaque;
    /* Return a fresh read-only handle to the initialized process time page. */
    uint32_t (*copy_shared_read)(void *opaque);
    /* Resolve a validated writable guest range for time-zone conversion output. */
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
} HznTimeConfig;

typedef struct {
    void *opaque;
    uint32_t (*copy_shared_read)(void *opaque);
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
    uint32_t next_id, timezone_ids[8];
    /* R59: ISystemClock / ISteadyClock objects, by domain id; kind 1 = system, 2 = steady. */
    uint32_t clock_ids[8];
    uint8_t clock_kinds[8];
    bool domain, root_open, closed;
} HznTime;

typedef enum { HZN_TIME_REPLY, HZN_TIME_BOUNDARY } HznTimeReplyKind;

typedef struct {
    HznTimeReplyKind kind;
    uint32_t result, command, object_id;
    const char *object_type;
} HznTimeReply;

/* Initialize the backing page before any guest mapping is exposed. */
bool hzn_time_init_page(void *page, size_t size, const HznTimeConfig *config);

/* One instance per time:u session. The caller serializes session dispatch. */
bool hzn_time_init(HznTime *time, const HznTimeConfig *config);
HznTimeReply hzn_time_send_sync(HznTime *time, void *tls, size_t size);

#ifdef __cplusplus
}
#endif
#endif
