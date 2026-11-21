/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_HWOPUS_H
#define POKOPIA_HORIZON_HWOPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HznHwopus HznHwopus;
typedef struct HznHwopusDomain HznHwopusDomain;

typedef struct {
    void *opaque;
    const void *(*read_guest)(void *, uint64_t, size_t);
    uint32_t (*clone_session)(void *, const HznHwopus *);
} HznHwopusConfig;

struct HznHwopus {
    HznHwopusConfig config;
    HznHwopusDomain *domain;
};

typedef struct {
    bool handled;
    uint32_t result, command;
} HznHwopusReply;

void hzn_hwopus_init(HznHwopus *manager, const HznHwopusConfig *config);
/* Caller serializes session operations and retains each clone until close. */
void hzn_hwopus_clone(HznHwopus *manager, const HznHwopus *source);
void hzn_hwopus_close(HznHwopus *manager);
HznHwopusReply hzn_hwopus_send_sync(HznHwopus *manager, uint8_t *tls, size_t size);

#endif
