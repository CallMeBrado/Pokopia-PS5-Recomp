/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hid.h"

#include <math.h>
#include <time.h>

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
    const int handle = scePadOpen(user, 0, 0, NULL);
    /* R96: the DualSense ignores scePadSetVibration until switched to
     * vibration mode 2 (1 and 2 are accepted; only 2 was felt). */
    if (handle >= 0)
        (void)scePadSetVibrationMode(handle, 2);
    return handle;
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
    /* R96: the game drives Switch 2 HD rumble, mostly at amplitudes 0.01-0.2,
     * which the DualSense's motors at the same scale do not make felt (0.18
     * was 46/255). Below 0.005 is off; above, start at a felt floor and rise
     * 2.5x as fast, so light and heavy effects stay distinct. */
    if (amplitude < 0.005f)
        return 0;
    const float scaled = 0.25f + 0.75f * (amplitude * 2.5f < 1.0f ? amplitude * 2.5f : 1.0f);
    return (uint8_t)(scaled * UINT8_MAX + 0.5f);
}

int hzn_hid_ps4_vibration_result;

/* R96: rumble mode experiments (see the loader's debug/rumble request).
 * kind 1: scePadSetVibrationMode, 2: scePadSetPS4BcVibrationMode,
 * 3: scePadEnablePs4CompatibleMode; then a one-second full-strength pulse. */
int hzn_hid_ps4_rumble_mode(int handle, int kind, int mode)
{
    if (handle < 0)
        return -1;
    switch (kind) {
    case 1: return scePadSetVibrationMode(handle, mode);
    case 2: return scePadSetPS4BcVibrationMode(handle, mode);
    case 3: return scePadEnablePs4CompatibleMode(handle);
    case 4: { /* light bar 0xRRGGBB: does any output reach the pad? */
        const OrbisPadColor color = {(uint8_t)(mode >> 16), (uint8_t)(mode >> 8), (uint8_t)mode, 255};
        return scePadSetLightBar(handle, &color);
    }
    case 5: return scePadSetVibrationStrength(handle, mode);
    case 8: { /* strength `mode` (0-255) re-sent every 16 ms for a second */
        const OrbisPadVibeParam level = {.lgMotor = (uint8_t)mode, .smMotor = (uint8_t)mode};
        int sent = 0;
        for (int i = 0; i < 62; ++i) {
            sent |= scePadSetVibration(handle, &level);
            nanosleep(&(struct timespec){.tv_nsec = 16000000}, NULL);
        }
        return sent;
    }
    case 6: { /* the vibration mode `mode`, then full strength re-sent every 16 ms for a second */
        const int set = mode ? scePadSetVibrationMode(handle, mode) : 0;
        const OrbisPadVibeParam full = {.lgMotor = 255, .smMotor = 255};
        int sent = 0;
        for (int i = 0; i < 62; ++i) {
            sent |= scePadSetVibration(handle, &full);
            nanosleep(&(struct timespec){.tv_nsec = 16000000}, NULL);
        }
        return set ? set : sent;
    }
    default: return 0;
    }
}

int hzn_hid_ps4_pulse(int handle, uint8_t strength)
{
    const OrbisPadVibeParam vibration = {.lgMotor = strength, .smMotor = strength};
    return handle < 0 ? -1 : scePadSetVibration(handle, &vibration);
}

/* R96: the PS5 drops a vibration value that is not re-sent within a few
 * frames (a single set at full strength was never felt). The last value is
 * kept here and hzn_hid_ps4_refresh repeats it while it is non-zero. */
static OrbisPadVibeParam vibration_current;
static int vibration_handle = -1;

void hzn_hid_ps4_refresh(void)
{
    const OrbisPadVibeParam vibration = vibration_current;
    if (vibration_handle >= 0 && (vibration.lgMotor || vibration.smMotor))
        (void)scePadSetVibration(vibration_handle, &vibration);
}

bool hzn_hid_ps4_vibrate(int handle, float amplitude_low, float amplitude_high)
{
    if (handle < 0)
        return false;
    const OrbisPadVibeParam vibration = {
        .lgMotor = motor_amplitude(amplitude_low),
        .smMotor = motor_amplitude(amplitude_high),
    };
    vibration_current = vibration;
    vibration_handle = handle;
    hzn_hid_ps4_vibration_result = scePadSetVibration(handle, &vibration); /* R96: logged by the loader */
    return hzn_hid_ps4_vibration_result == 0;
}

void hzn_hid_ps4_close(int handle)
{
    if (handle >= 0) {
        (void)hzn_hid_ps4_vibrate(handle, 0.0f, 0.0f);
        vibration_handle = -1;
        (void)scePadClose(handle);
    }
}
