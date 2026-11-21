/* PS5 stand-in for OpenOrbis <orbis/AudioOut.h> (values from OpenOrbis _types/audio_out.h,
 * _types/errors.h and _types/user.h). */
#ifndef POKOPIA_PS5_ORBIS_AUDIOOUT_H
#define POKOPIA_PS5_ORBIS_AUDIOOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t OrbisUserServiceUserId;

enum OrbisAudioOutPort { ORBIS_AUDIO_OUT_PORT_TYPE_MAIN = 0 };
enum OrbisAudioOutParamFormat { ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO = 1 };

#define ORBIS_USER_SERVICE_USER_ID_SYSTEM 0xFF
#define ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED 0x80960003
#define ORBIS_AUDIO_OUT_ERROR_INVALID_PORT 0x80260003
#define ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT 0x8026000E

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(OrbisUserServiceUserId user, enum OrbisAudioOutPort type, int32_t index, uint32_t length,
                        uint32_t sample_rate, uint32_t param_type);
int32_t sceAudioOutOutput(int32_t port, const void *ptr);
int32_t sceAudioOutClose(int32_t port);

#ifdef __cplusplus
}
#endif
#endif
