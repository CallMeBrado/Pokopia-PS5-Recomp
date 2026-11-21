/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_DEBUG_PAD_H
#define POKOPIA_HORIZON_DEBUG_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HZN_DEBUG_PAD_BUTTON_MASK ((UINT64_C(1) << 35) - 1)
enum { HZN_DEBUG_PAD_MAX_HOLD_MS = 5000, HZN_NPAD_CONNECTED = 1 };

/* Decoded host command. The caller parses the transport independently. */
typedef struct {
    uint64_t seq, buttons;
    int16_t lx, ly, rx, ry;
    uint32_t hold_ms;
} HznDebugPadCommand;

typedef struct {
    uint64_t buttons;
    int16_t lx, ly, rx, ry;
    uint32_t attributes;
} HznDebugPadInput;

typedef struct {
    HznDebugPadInput virtual_input;
    uint64_t last_seq, clock_ms, started_ms;
    uint32_t hold_ms;
} HznDebugPad;

typedef enum {
    HZN_DEBUG_PAD_APPLIED,
    HZN_DEBUG_PAD_STALE,
    HZN_DEBUG_PAD_INVALID,
} HznDebugPadResult;

/* The caller zero-initializes the state, serializes access, and supplies a local
 * monotonic clock. Sequences start above zero. A zero hold releases immediately
 * and requires neutral buttons and sticks. */
HznDebugPadResult hzn_debug_pad_apply(HznDebugPad *pad, const HznDebugPadCommand *command,
                                     uint64_t now_ms);

/* While a command is held, virtual input takes precedence. After release,
 * a connected physical pad passes through. A virtual-only pad stays connected
 * with neutral controls after its first accepted command. */
HznDebugPadInput hzn_debug_pad_sample(HznDebugPad *pad, const HznDebugPadInput *physical,
                                     uint64_t now_ms);

/* One Horizon HidNpadCommonState payload. The guest ABI and supported hosts
 * use little-endian data. The caller owns the sampling number and publication. */
typedef struct {
    uint64_t sampling_number, buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, reserved;
} HznNpadCommonState;

HznNpadCommonState hzn_debug_pad_npad_state(HznDebugPadInput input, uint64_t sampling_number);

#ifdef __cplusplus
}
#endif
#endif
