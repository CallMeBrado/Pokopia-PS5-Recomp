/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_STPL_H
#define POKOPIA_HORIZON_STPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HZN_STPL_HID_BYTES = 0x84000 };

typedef struct {
    void *opaque;
    uint32_t (*open_user)(void *opaque);
    uint32_t (*copy_hid_memory)(void *opaque, size_t bytes);
} HznStplConfig;

typedef struct {
    bool handled;
    uint32_t result, command;
} HznStplReply;

/* A manager opens an ordinary IUserService session. The copied handle
 * identifies read-only, initially zero GameShare input memory. The caller
 * owns both kinds of handles and serializes the service calls. */
HznStplReply hzn_stpl_send_sync(bool user, const HznStplConfig *config, uint8_t *tls, size_t size);

#endif
