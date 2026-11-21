/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hid.h"

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

    /* A/Cross and B/Circle follow the PS4 confirm/cancel convention.
     * Remaining entries follow the FullKey button-bit order. */
    static const uint32_t buttons[] = {
        ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3, ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1,
        ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_OPTIONS, ORBIS_PAD_BUTTON_TOUCH_PAD,
        ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_RIGHT, ORBIS_PAD_BUTTON_DOWN,
    };
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
        if (data.buttons & buttons[i])
            pad->buttons |= UINT64_C(1) << i;
    pad->lx = stick_axis(data.leftStick.x);
    pad->ly = -stick_axis(data.leftStick.y);
    pad->rx = stick_axis(data.rightStick.x);
    pad->ry = -stick_axis(data.rightStick.y);
    pad->connected = true;
    return true;
}

void hzn_hid_ps4_close(int handle)
{
    if (handle >= 0)
        (void)scePadClose(handle);
}
