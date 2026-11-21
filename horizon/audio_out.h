/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_AUDIO_OUT_H
#define POKOPIA_AUDIO_OUT_H

#include "cmif.h"
#include "event.h"

typedef struct PokopiaAudioOut PokopiaAudioOut;

typedef struct {
    void *opaque;
    const void *(*read)(void *, uint64_t, size_t);
    void *(*write)(void *, uint64_t, size_t);
} PokopiaAudioGuest;

bool pokopia_audio_out_open(const HznCmifRequest *request, uint8_t *tls, size_t tls_size,
                           const PokopiaAudioGuest *guest, HznEvents *events, uint32_t session_handle,
                           PokopiaAudioOut **out);
void pokopia_audio_out_close(PokopiaAudioOut *out);
bool pokopia_audio_out_request(PokopiaAudioOut *out, uint32_t command, const HznCmifRequest *request,
                              uint8_t *tls, size_t tls_size, const PokopiaAudioGuest *guest);

#endif
