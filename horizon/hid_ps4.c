/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hid.h"

#include <math.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

int hzn_hid_ps4_open(void)
{
    (void)sceUserServiceInitialize(NULL);
    int32_t user;
    int result = sceUserServiceGetForegroundUser(&user);
    if (result < 0)
        result = sceUserServiceGetInitialUser(&user);
    if (result < 0)
        return result;
    (void)scePadInit();
    return scePadOpen(user, 0, 0, NULL);
}

static int32_t stick_axis(uint8_t raw)
{
    const int32_t delta = (int32_t)raw - 128;
    return (delta * 32767) / (delta < 0 ? 128 : 127);
}

bool hzn_hid_ps4_swap_ab;

bool hzn_hid_ps4_read(int handle, HznHidPad *pad)
{
    if (!pad)
        return false;
    *pad = (HznHidPad){0};
    OrbisPadData data;
    if (handle < 0 || scePadReadState(handle, &data) < 0)
        return false;
    if (!data.connected)
        return true;

    /* Face buttons map by position, as the game lays its prompts out (its
     * interaction wheel puts A on the right): A/Circle (right), B/Cross
     * (bottom), X/Triangle (top), Y/Square (left). The UI glyphs follow
     * (gfx-bridge/ps_button_glyphs.h). Entries follow the FullKey button-bit order. */
    static const uint32_t buttons[] = {
        ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3, ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1,
        ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_OPTIONS, ORBIS_PAD_BUTTON_TOUCH_PAD,
        ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_RIGHT, ORBIS_PAD_BUTTON_DOWN,
    };
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (data.buttons & buttons[i])
            pad->buttons |= UINT64_C(1) << i;
    if (hzn_hid_ps4_swap_ab) {
        /* R92: swap the A and B bits (Circle and Cross). */
        const uint64_t a = pad->buttons & 1, b = (pad->buttons >> 1) & 1;
        pad->buttons = (pad->buttons & ~UINT64_C(3)) | (a << 1) | b;
    }
    pad->lx = stick_axis(data.leftStick.x);
    pad->ly = -stick_axis(data.leftStick.y);
    pad->rx = stick_axis(data.rightStick.x);
    pad->ry = -stick_axis(data.rightStick.y);
    pad->connected = true;
    return true;
}

static uint8_t motor_amplitude(float amplitude)
{
    if (!isfinite(amplitude) || amplitude <= 0.0f)
        return 0;
    if (amplitude >= 1.0f)
        return UINT8_MAX;
    return (uint8_t)(amplitude * UINT8_MAX + 0.5f);
}

bool hzn_hid_ps4_vibrate(int handle, float amplitude_low, float amplitude_high)
{
    if (handle < 0)
        return false;
    const OrbisPadVibeParam vibration = {
        .lgMotor = motor_amplitude(amplitude_low),
        .smMotor = motor_amplitude(amplitude_high),
    };
    return scePadSetVibration(handle, &vibration) == 0;
}

void hzn_hid_ps4_close(int handle)
{
    if (handle >= 0) {
        (void)hzn_hid_ps4_vibrate(handle, 0.0f, 0.0f);
        (void)scePadClose(handle);
    }
}
