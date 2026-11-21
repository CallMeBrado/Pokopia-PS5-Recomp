/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_LM_H
#define POKOPIA_HORIZON_LM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t object_id;
    uint64_t pid;
    size_t used;
    uint8_t severity;
    bool open;
    char text[5001];
} HznLmLogger;

typedef struct {
    uint64_t process_id;
    uint32_t next_object_id, destination;
    bool domain, root_open;
    HznLmLogger loggers[4];
} HznLm;

typedef const void *(*HznLmRead)(void *opaque, uint64_t address, size_t bytes);

typedef enum {
    HZN_LM_REPLY,
    HZN_LM_BOUNDARY,
} HznLmReplyKind;

typedef struct {
    HznLmReplyKind kind;
    uint32_t result;
    uint32_t command;
    uint32_t object_id;
} HznLmReply;

/* Each lm service handle owns one instance, serialized by the dispatcher. */
void hzn_lm_init(HznLm *lm, uint64_t guest_pid);

/* The callback must validate the entire readable guest range before returning a
 * pointer. A boundary leaves the TLS request buffer unchanged. */
HznLmReply hzn_lm_send_sync(HznLm *lm, void *tls, size_t tls_size,
                            HznLmRead read_guest, void *opaque, FILE *log);

#ifdef __cplusplus
}
#endif
#endif
