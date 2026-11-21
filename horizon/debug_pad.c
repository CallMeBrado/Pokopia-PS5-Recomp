/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debug_pad.h"

#include <stddef.h>

_Static_assert(sizeof(HznNpadCommonState) == 0x28, "Horizon Npad common state size");
_Static_assert(offsetof(HznNpadCommonState, buttons) == 0x08, "Horizon Npad buttons offset");
_Static_assert(offsetof(HznNpadCommonState, lx) == 0x10, "Horizon Npad left stick offset");
_Static_assert(offsetof(HznNpadCommonState, rx) == 0x18, "Horizon Npad right stick offset");
_Static_assert(offsetof(HznNpadCommonState, attributes) == 0x20, "Horizon Npad attributes offset");

static uint64_t observe_time(HznDebugPad *pad, uint64_t now_ms)
{
    if (now_ms > pad->clock_ms)
        pad->clock_ms = now_ms;
    return pad->clock_ms;
}

HznDebugPadResult hzn_debug_pad_apply(HznDebugPad *pad, const HznDebugPadCommand *command,
                                     uint64_t now_ms)
{
    if (!command || command->hold_ms > HZN_DEBUG_PAD_MAX_HOLD_MS ||
        (command->buttons & ~HZN_DEBUG_PAD_BUTTON_MASK) ||
        command->lx == INT16_MIN || command->ly == INT16_MIN ||
        command->rx == INT16_MIN || command->ry == INT16_MIN ||
        (!command->hold_ms && (command->buttons || command->lx || command->ly ||
                               command->rx || command->ry)))
        return HZN_DEBUG_PAD_INVALID;
    if (command->seq <= pad->last_seq)
        return HZN_DEBUG_PAD_STALE;

    pad->virtual_input = (HznDebugPadInput){
        .buttons = command->buttons,
        .lx = command->lx, .ly = command->ly, .rx = command->rx, .ry = command->ry,
        .attributes = HZN_NPAD_CONNECTED,
    };
    pad->last_seq = command->seq;
    pad->started_ms = observe_time(pad, now_ms);
    pad->hold_ms = command->hold_ms;
    return HZN_DEBUG_PAD_APPLIED;
}

HznDebugPadInput hzn_debug_pad_sample(HznDebugPad *pad, const HznDebugPadInput *physical,
                                     uint64_t now_ms)
{
    uint64_t elapsed = observe_time(pad, now_ms) - pad->started_ms;
    if (elapsed < pad->hold_ms)
        return pad->virtual_input;
    if (physical && (physical->attributes & HZN_NPAD_CONNECTED))
        return *physical;
    return (HznDebugPadInput){.attributes = pad->virtual_input.attributes};
}

static uint64_t stick_direction(int32_t axis, unsigned negative, unsigned positive)
{
    const int32_t threshold = INT16_MAX / 2;
    if (axis < -threshold)
        return UINT64_C(1) << negative;
    if (axis > threshold)
        return UINT64_C(1) << positive;
    return 0;
}

static int32_t npad_axis(int16_t axis)
{
    /* Horizon sticks use the symmetric -32767 through 32767 range. */
    return axis == INT16_MIN ? -INT16_MAX : axis;
}

HznNpadCommonState hzn_debug_pad_npad_state(HznDebugPadInput input, uint64_t sampling_number)
{
    HznNpadCommonState state = {.sampling_number = sampling_number};
    if (!(input.attributes & HZN_NPAD_CONNECTED))
        return state;

    state.lx = npad_axis(input.lx);
    state.ly = npad_axis(input.ly);
    state.rx = npad_axis(input.rx);
    state.ry = npad_axis(input.ry);
    state.attributes = input.attributes;
    state.buttons = (input.buttons & HZN_DEBUG_PAD_BUTTON_MASK) |
                    stick_direction(state.lx, 16, 18) | stick_direction(state.ly, 19, 17) |
                    stick_direction(state.rx, 20, 22) | stick_direction(state.ry, 23, 21);
    return state;
}
