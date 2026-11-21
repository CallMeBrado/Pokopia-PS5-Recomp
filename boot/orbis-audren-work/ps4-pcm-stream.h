/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_PS4_PCM_STREAM_H
#define POKOPIA_PS4_PCM_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ps4PcmStream Ps4PcmStream;
/* Sets the release event's level. The callback cannot call back into this stream. */
typedef void (*Ps4PcmEvent)(void *opaque, bool signaled);
/* Reports a firmware call's entry and exit. The callback cannot call into the stream. */
typedef void (*Ps4PcmDiagnostic)(void *opaque, const char *operation, bool entering, int32_t result);

typedef enum Ps4PcmAppendResult {
    PS4_PCM_APPEND_OK,
    PS4_PCM_APPEND_FULL,
    PS4_PCM_APPEND_FAILED,
} Ps4PcmAppendResult;

Ps4PcmStream *ps4_pcm_create(Ps4PcmEvent event, void *opaque);
Ps4PcmStream *ps4_pcm_create_with_diagnostics(Ps4PcmEvent event, Ps4PcmDiagnostic diagnostic, void *opaque);
void ps4_pcm_destroy(Ps4PcmStream *stream);
int32_t ps4_pcm_start(Ps4PcmStream *stream);
int32_t ps4_pcm_stop(Ps4PcmStream *stream);
bool ps4_pcm_started(Ps4PcmStream *stream);

/* Takes an owned copy of interleaved 48 kHz signed16 stereo. Null PCM means silence. */
Ps4PcmAppendResult ps4_pcm_append(Ps4PcmStream *stream, uint64_t tag, const void *pcm, uint32_t frames);
uint32_t ps4_pcm_take_released(Ps4PcmStream *stream, uint64_t *tags, uint32_t max);
bool ps4_pcm_contains(Ps4PcmStream *stream, uint64_t tag);
uint32_t ps4_pcm_queued(Ps4PcmStream *stream);
uint64_t ps4_pcm_played(Ps4PcmStream *stream);
bool ps4_pcm_flush(Ps4PcmStream *stream);
void ps4_pcm_set_volume(Ps4PcmStream *stream, float volume);
float ps4_pcm_get_volume(Ps4PcmStream *stream);
/* Last asynchronous native error, cleared by a successful Start. */
int32_t ps4_pcm_error(Ps4PcmStream *stream);

#ifdef __cplusplus
}
#endif
#endif
