#include "hid_ps4.c"
#include <assert.h>
#include <stdio.h>

static int sequence, last_set, closed, reply;
static OrbisPadVibeParam last;

int32_t sceUserServiceInitialize(void *options) { (void)options; return 0; }
int32_t sceUserServiceGetForegroundUser(int32_t *user) { *user = 4; return 0; }
int32_t sceUserServiceGetInitialUser(int32_t *user) { *user = 4; return 0; }
int32_t scePadInit(void) { return 0; }
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, void *param)
{
    assert(user == 4 && !type && !index && !param);
    return 17;
}
int32_t scePadReadState(int32_t handle, OrbisPadData *data)
{
    (void)handle;
    (void)data;
    return -1;
}
int32_t scePadSetVibration(int32_t handle, const OrbisPadVibeParam *param)
{
    assert(handle == 17 && param);
    last = *param;
    last_set = ++sequence;
    return reply;
}
int32_t scePadClose(int32_t handle)
{
    assert(handle == 17);
    closed = ++sequence;
    return 0;
}

int main(void)
{
    const int pad = hzn_hid_ps4_open();
    assert(pad == 17);
    assert(!hzn_hid_ps4_vibrate(-1, 1.0f, 1.0f) && !sequence);
    assert(hzn_hid_ps4_vibrate(pad, 0.5f, 0.25f));
    assert(last.lgMotor == 128 && last.smMotor == 64);
    assert(hzn_hid_ps4_vibrate(pad, -0.1f, 1.1f));
    assert(last.lgMotor == 0 && last.smMotor == 255);
    assert(hzn_hid_ps4_vibrate(pad, NAN, INFINITY));
    assert(last.lgMotor == 0 && last.smMotor == 0);
    assert(hzn_hid_ps4_vibrate(pad, 1.0f, 0.5f));
    assert(last.lgMotor == 255 && last.smMotor == 128);
    reply = -1;
    assert(!hzn_hid_ps4_vibrate(pad, 0.75f, 1.0f));
    hzn_hid_ps4_close(-1);
    assert(!closed);
    hzn_hid_ps4_close(pad);
    assert(last.lgMotor == 0 && last.smMotor == 0 && closed == last_set + 1);
    puts("PASS vibration channel mapping, amplitude bounds, error propagation and motor stop before close");
}
