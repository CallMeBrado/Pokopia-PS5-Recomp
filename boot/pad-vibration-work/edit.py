from pathlib import Path
p = Path('hid.h')
s = p.read_text()
s = s.replace('void hzn_hid_ps4_close(int handle);', '''/* Fixed-frequency DS4 motors take normalized low and high amplitudes. */
bool hzn_hid_ps4_vibrate(int handle, float amplitude_low, float amplitude_high);
void hzn_hid_ps4_close(int handle);''')
p.write_text(s)
p = Path('hid_ps4.c')
s = p.read_text()
s = s.replace('#include <orbis/Pad.h>', '#include <math.h>\n\n#include <orbis/Pad.h>')
s = s.replace('''void hzn_hid_ps4_close(int handle)
{
    if (handle >= 0)
        (void)scePadClose(handle);
}''', '''static uint8_t motor_amplitude(float amplitude)
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
}''')
p.write_text(s)
