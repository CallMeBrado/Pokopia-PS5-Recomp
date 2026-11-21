/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debug_pad.h"

#include <assert.h>
#include <stdio.h>

static void expect_input(HznDebugPadInput input, uint64_t buttons, int16_t lx, int16_t ly,
                         int16_t rx, int16_t ry, uint32_t attributes)
{
    assert(input.buttons == buttons && input.attributes == attributes);
    assert(input.lx == lx && input.ly == ly && input.rx == rx && input.ry == ry);
}

int main(void)
{
    const uint64_t a = UINT64_C(1) << 0;
    const uint64_t dpad_down = UINT64_C(1) << 15;
    const uint64_t last_valid_button = UINT64_C(1) << 34;
    const uint32_t wired = HZN_NPAD_CONNECTED | (1u << 1);
    const HznDebugPadInput physical = {.buttons = dpad_down, .ly = 321, .attributes = wired};
    HznDebugPad pad = {0};
    expect_input(hzn_debug_pad_sample(&pad, NULL, 100), 0, 0, 0, 0, 0, 0);
    expect_input(hzn_debug_pad_sample(&pad, &physical, 100), dpad_down, 0, 321, 0, 0, wired);

    HznDebugPadCommand command = {
        .seq = 7, .buttons = a | last_valid_button,
        .lx = -32767, .ly = 16384, .rx = 16383, .ry = -16384, .hold_ms = 100,
    };
    assert(hzn_debug_pad_apply(&pad, &command, 110) == HZN_DEBUG_PAD_APPLIED);
    HznDebugPadInput virtual_input = hzn_debug_pad_sample(&pad, &physical, 140);
    expect_input(virtual_input, a | last_valid_button, -32767, 16384, 16383, -16384, HZN_NPAD_CONNECTED);
    HznNpadCommonState state = hzn_debug_pad_npad_state(virtual_input, UINT64_C(0x1122334455667788));
    assert(state.sampling_number == UINT64_C(0x1122334455667788));
    assert(state.buttons == (a | last_valid_button | (UINT64_C(1) << 16) |
                            (UINT64_C(1) << 17) | (UINT64_C(1) << 23)));
    assert(state.lx == -32767 && state.ly == 16384 && state.rx == 16383 && state.ry == -16384);
    assert(state.attributes == HZN_NPAD_CONNECTED && state.reserved == 0);

    assert(hzn_debug_pad_apply(&pad, &command, 200) == HZN_DEBUG_PAD_STALE);
    HznDebugPadCommand invalid = {.seq = 8, .buttons = UINT64_C(1) << 35, .hold_ms = 100};
    assert(hzn_debug_pad_apply(&pad, &invalid, 201) == HZN_DEBUG_PAD_INVALID);
    invalid.buttons = 0;
    invalid.hold_ms = HZN_DEBUG_PAD_MAX_HOLD_MS + 1;
    assert(hzn_debug_pad_apply(&pad, &invalid, 202) == HZN_DEBUG_PAD_INVALID);
    invalid.hold_ms = 100;
    invalid.rx = INT16_MIN;
    assert(hzn_debug_pad_apply(&pad, &invalid, 203) == HZN_DEBUG_PAD_INVALID);
    expect_input(hzn_debug_pad_sample(&pad, &physical, 209),
                 a | last_valid_button, -32767, 16384, 16383, -16384, HZN_NPAD_CONNECTED);
    expect_input(hzn_debug_pad_sample(&pad, &physical, 210), dpad_down, 0, 321, 0, 0, wired);
    expect_input(hzn_debug_pad_sample(&pad, NULL, 199), 0, 0, 0, 0, 0, HZN_NPAD_CONNECTED);

    command = (HznDebugPadCommand){.seq = 8, .hold_ms = HZN_DEBUG_PAD_MAX_HOLD_MS};
    assert(hzn_debug_pad_apply(&pad, &command, 220) == HZN_DEBUG_PAD_APPLIED);
    expect_input(hzn_debug_pad_sample(&pad, &physical, 221), 0, 0, 0, 0, 0, HZN_NPAD_CONNECTED);
    command = (HznDebugPadCommand){.seq = 9, .buttons = a};
    assert(hzn_debug_pad_apply(&pad, &command, 222) == HZN_DEBUG_PAD_INVALID);
    command.buttons = 0;
    assert(hzn_debug_pad_apply(&pad, &command, 222) == HZN_DEBUG_PAD_APPLIED);
    expect_input(hzn_debug_pad_sample(&pad, &physical, 222), dpad_down, 0, 321, 0, 0, wired);

    command = (HznDebugPadCommand){.seq = 10, .buttons = a, .hold_ms = 10};
    assert(hzn_debug_pad_apply(&pad, &command, UINT64_MAX - 10) == HZN_DEBUG_PAD_APPLIED);
    expect_input(hzn_debug_pad_sample(&pad, NULL, UINT64_MAX - 1), a, 0, 0, 0, 0, HZN_NPAD_CONNECTED);
    expect_input(hzn_debug_pad_sample(&pad, NULL, UINT64_MAX), 0, 0, 0, 0, 0, HZN_NPAD_CONNECTED);

    state = hzn_debug_pad_npad_state((HznDebugPadInput){.buttons = a, .lx = -10}, 1);
    assert(state.sampling_number == 1 && state.buttons == 0 && state.lx == 0 && state.attributes == 0);
    state = hzn_debug_pad_npad_state((HznDebugPadInput){.lx = INT16_MIN, .attributes = wired}, 2);
    assert(state.lx == -32767 && state.attributes == wired && state.buttons == (UINT64_C(1) << 16));

    puts("PASS debug pad: guest Npad layout, monotonic hold/release, replay and physical passthrough");
    return 0;
}
