/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_OFFLINE_APP_H
#define POKOPIA_HORIZON_OFFLINE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HZN_OFFLINE_NIFM_STATIC,
    HZN_OFFLINE_BCAT_CREATOR,
} HznOfflineAppKind;

typedef struct HznOfflineApp HznOfflineApp;

/* A CMIF clone references the same domain table until all handles are closed. */
typedef uint32_t (*HznOfflineAppClone)(void *opaque, HznOfflineApp *shared);

struct HznOfflineApp {
    HznOfflineAppKind kind;
    HznOfflineAppClone clone;
    void *opaque;
    uint32_t next_id, objects[8];
    bool domain, root_open;
};

typedef struct {
    uint32_t result, command, object_id;
    const char *boundary_type;
} HznOfflineAppReply;

void hzn_offline_app_init(HznOfflineApp *service, HznOfflineAppKind kind,
                          HznOfflineAppClone clone, void *opaque);
HznOfflineAppReply hzn_offline_app_send(HznOfflineApp *service, void *buffer, size_t size);

#endif
