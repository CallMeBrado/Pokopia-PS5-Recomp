/* PS5 stand-in for OpenOrbis <orbis/Pad.h>. The probe confirmed the PS5 scePadReadState
 * report has the PS4 layout (buttons, sticks at +4, orientation quaternion at +12). */
#ifndef POKOPIA_PS5_ORBIS_PAD_H
#define POKOPIA_PS5_ORBIS_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum OrbisPadButton {
   ORBIS_PAD_BUTTON_L3 = 0x0002,
   ORBIS_PAD_BUTTON_R3 = 0x0004,
   ORBIS_PAD_BUTTON_OPTIONS = 0x0008,
   ORBIS_PAD_BUTTON_UP = 0x0010,
   ORBIS_PAD_BUTTON_RIGHT = 0x0020,
   ORBIS_PAD_BUTTON_DOWN = 0x0040,
   ORBIS_PAD_BUTTON_LEFT = 0x0080,
   ORBIS_PAD_BUTTON_L2 = 0x0100,
   ORBIS_PAD_BUTTON_R2 = 0x0200,
   ORBIS_PAD_BUTTON_L1 = 0x0400,
   ORBIS_PAD_BUTTON_R1 = 0x0800,
   ORBIS_PAD_BUTTON_TRIANGLE = 0x1000,
   ORBIS_PAD_BUTTON_CIRCLE = 0x2000,
   ORBIS_PAD_BUTTON_CROSS = 0x4000,
   ORBIS_PAD_BUTTON_SQUARE = 0x8000,
   ORBIS_PAD_BUTTON_TOUCH_PAD = 0x100000
};

#define ORBIS_PAD_PORT_TYPE_STANDARD 0
#define ORBIS_PAD_MAX_TOUCH_NUM 2

typedef struct vec_float3 { float x, y, z; } vec_float3;
typedef struct vec_float4 { float x, y, z, w; } vec_float4;
typedef struct stick { uint8_t x, y; } stick;
typedef struct analog { uint8_t l2, r2; } analog;

typedef struct OrbisPadTouch {
   uint16_t x, y;
   uint8_t finger;
   uint8_t pad[3];
} OrbisPadTouch;

typedef struct OrbisPadTouchData {
   uint8_t fingers;
   uint8_t pad1[3];
   uint32_t pad2;
   OrbisPadTouch touch[ORBIS_PAD_MAX_TOUCH_NUM];
} OrbisPadTouchData;

typedef struct OrbisPadData {
   uint32_t buttons;
   stick leftStick;
   stick rightStick;
   analog analogButtons;
   uint16_t padding;
   vec_float4 quat;
   vec_float3 vel;
   vec_float3 acell;
   OrbisPadTouchData touch;
   uint8_t connected;
   uint64_t timestamp;
   uint8_t ext[16];
   uint8_t count;
   uint8_t unknown[15];
   uint8_t ps5_reserve[64]; /* headroom in case the PS5 report is longer */
} OrbisPadData;

typedef struct OrbisPadVibeParam {
   uint8_t lgMotor;
   uint8_t smMotor;
} OrbisPadVibeParam;

int32_t scePadInit(void);
int32_t scePadOpen(int32_t userID, int32_t type, int32_t index, void *param);
int32_t scePadClose(int32_t handle);
int32_t scePadReadState(int32_t handle, OrbisPadData *data);
int32_t scePadSetVibration(int32_t handle, const OrbisPadVibeParam *param);
/* R96: PS5 libScePad. Signatures inferred; the DualSense ignored
 * scePadSetVibration (it returned 0) until switched to a compatible mode. */
int32_t scePadSetVibrationMode(int32_t handle, int32_t mode);
int32_t scePadSetPS4BcVibrationMode(int32_t handle, int32_t mode);
int32_t scePadEnablePs4CompatibleMode(int32_t handle);
typedef struct OrbisPadColor { uint8_t r, g, b, a; } OrbisPadColor;
int32_t scePadSetLightBar(int32_t handle, const OrbisPadColor *color);
int32_t scePadResetLightBar(int32_t handle);
int32_t scePadSetVibrationStrength(int32_t handle, int32_t strength);

#ifdef __cplusplus
}
#endif
#endif
