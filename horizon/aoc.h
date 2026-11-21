/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_AOC_H
#define POKOPIA_HORIZON_AOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Validate every byte the service will write before returning a host pointer. */
typedef void *(*HznAocWrite)(void *opaque, uint64_t address, size_t bytes);
/* Copy the shared list-change event. The offline catalog remains fixed. */
typedef uint32_t (*HznAocCopyEvent)(void *opaque);

typedef struct {
    const uint32_t *content_ids;
    uint32_t content_count;
    HznAocWrite write_guest;
    HznAocCopyEvent copy_event;
    void *opaque;
} HznAoc;

typedef enum { HZN_AOC_REPLY, HZN_AOC_BOUNDARY } HznAocReplyKind;

typedef struct {
    HznAocReplyKind kind;
    uint32_t result;
    uint32_t command;
    uint32_t object_id;
} HznAocReply;

/* Keep the catalog of DLC indices in list order and alive for the session. */
bool hzn_aoc_init(HznAoc *aoc, const uint32_t *content_ids, size_t content_count);

/* The caller serializes IPC. A boundary leaves the request unchanged. */
HznAocReply hzn_aoc_send_sync(HznAoc *aoc, void *tls, size_t tls_size);

#ifdef __cplusplus
}
#endif
#endif
