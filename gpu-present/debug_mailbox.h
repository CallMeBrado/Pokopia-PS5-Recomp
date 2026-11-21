/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_DEBUG_MAILBOX_H
#define POKOPIA_DEBUG_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "../horizon/debug_pad.h"

#define POKOPIA_DEBUG_DIRECTORY "/data/pokopia/debug"
enum { POKOPIA_DEBUG_PATH_BYTES = 512 };

typedef struct {
    const char *directory;
    uint64_t capture_seq, pad_seq;
} PokopiaDebugMailbox;

bool pokopia_debug_mailbox_init(PokopiaDebugMailbox *mailbox, const char *directory);
bool pokopia_debug_capture(PokopiaDebugMailbox *mailbox, char path[POKOPIA_DEBUG_PATH_BYTES]);
bool pokopia_debug_pad(PokopiaDebugMailbox *mailbox, HznDebugPadCommand *command);
bool pokopia_debug_ack_pad(const PokopiaDebugMailbox *mailbox, uint64_t seq);

#endif
