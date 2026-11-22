/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_HID_H
#define POKOPIA_HORIZON_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HZN_HID_MEMORY_BYTES = 0x40000, HZN_HID_STYLE_FULLKEY = 1, HZN_HID_VIBRATION_DEVICES = 32 };

typedef struct {
    float amplitude_low, frequency_low, amplitude_high, frequency_high;
} HznHidVibrationValue;

typedef struct {
    uint32_t handle;
    HznHidVibrationValue value;
} HznHidVibrationDevice;

typedef struct {
    uint64_t aruid, handheld_activation_mode;
    uint64_t nmouse_buttons[2];
    uint32_t nmouse_sources;
    uint32_t styles, ids;
    HznHidVibrationDevice vibration_devices[HZN_HID_VIBRATION_DEVICES];
    size_t vibration_device_count;
    bool npad_active, touch_active, nmouse_initialized;
} HznHidState;

typedef struct {
    void *opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t bytes);
    uint32_t (*open_applet_resource)(void *opaque, uint64_t aruid);
    uint32_t (*copy_hid_memory)(void *opaque, size_t bytes);
    uint32_t (*open_vibration_list)(void *opaque);
    bool (*vibrate)(void *opaque, float amplitude_low, float amplitude_high);
    void *(*get_transfer_memory)(void *opaque, uint32_t handle, uint64_t size);
} HznHidConfig;

typedef enum {
    HZN_HID_SERVER, HZN_HID_APPLET_RESOURCE, HZN_HID_VIBRATION_LIST,
} HznHidInterface;

typedef struct {
    bool handled;
    uint32_t result, command;
} HznHidReply;

/* Share one state between root and applet resource sessions. Serialize
 * service calls and publication. aruid is the caller applet's registered ID. */
void hzn_hid_init(HznHidState *state, uint64_t aruid);
HznHidReply hzn_hid_send_sync(HznHidState *state, HznHidInterface endpoint, const HznHidConfig *config,
                             uint8_t *tls, size_t size);

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    bool connected;
} HznHidPad;

/* Both functions take the host-writable view of the 0x40000 HID object.
 * Initialize before exposing the first guest mapping. Serialize publishers
 * and keep this view alive while any guest mapping is accessible. */
bool hzn_hid_memory_init(void *memory, size_t bytes);
void hzn_hid_publish(void *memory, const HznHidState *hid, const HznHidPad *pad);

#ifndef _WIN32
/* Open one standard pad for the active PS4 user. Negative means unavailable. */
int hzn_hid_ps4_open(void);
/* R92: Cross acts as A and Circle as B (Western confirm), instead of by position. */
extern bool hzn_hid_ps4_swap_ab;
bool hzn_hid_ps4_read(int handle, HznHidPad *pad);
bool hzn_hid_ps4_vibrate(int handle, float amplitude_low, float amplitude_high);
/* R96: re-sends the current non-zero vibration (call every ~16 ms). */
void hzn_hid_ps4_refresh(void);
void hzn_hid_ps4_close(int handle);
#endif

#endif

