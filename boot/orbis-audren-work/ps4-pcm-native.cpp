/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps4-pcm-native.h"

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

namespace ps4_pcm_native {

int32_t Open(const Diagnostics &diagnostics) {
    diagnostics.Begin("sceUserServiceInitialize");
    const int32_t user = sceUserServiceInitialize(nullptr);
    diagnostics.End("sceUserServiceInitialize", user);
    if (user < 0 && static_cast<uint32_t>(user) != ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        return user;
    }
    diagnostics.Begin("sceAudioOutInit");
    const int32_t init = sceAudioOutInit();
    diagnostics.End("sceAudioOutInit", init);
    if (init < 0 && static_cast<uint32_t>(init) != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) {
        return init;
    }
    diagnostics.Begin("sceAudioOutOpen");
    const int32_t port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
        BlockFrames, SampleRate, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    diagnostics.End("sceAudioOutOpen", port);
    return port == 0 ? static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_INVALID_PORT) : port;
}

int32_t Submit(int32_t port, const int16_t *samples) { return sceAudioOutOutput(port, samples); }

/* This is the completion barrier used in OpenOrbis's audio-wav sample. */
int32_t Wait(int32_t port) { return sceAudioOutOutput(port, nullptr); }

int32_t Close(int32_t port) { return sceAudioOutClose(port); }

}  // namespace ps4_pcm_native
