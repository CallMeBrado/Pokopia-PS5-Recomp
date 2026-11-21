/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_PCTL_H
#define POKOPIA_HORIZON_PCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    bool initialized;
} HznPctlObject;

typedef struct {
    uint32_t next_object_id;
    bool domain, root_open;
    HznPctlObject objects[16];
} HznPctl;

typedef enum { HZN_PCTL_REPLY, HZN_PCTL_BOUNDARY } HznPctlReplyKind;

typedef struct {
    HznPctlReplyKind kind;
    uint32_t result;
    uint32_t command;
    uint32_t object_id;
    const char *object_type;
} HznPctlReply;

/* One instance per service handle. The caller serializes IPC. */
void hzn_pctl_init(HznPctl *pctl);

/* A boundary leaves the request unchanged and identifies its target. */
HznPctlReply hzn_pctl_send_sync(HznPctl *pctl, void *tls, size_t tls_size);

#ifdef __cplusplus
}
#endif
#endif
