/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_out.h"
#include "ps4-pcm-stream.h"

#include <stdlib.h>
#include <string.h>

enum {
    OUT_NOT_FOUND = 153 | (1 << 9),
    OUT_OPERATION_FAILED = 153 | (2 << 9),
    OUT_BUFFER_FULL = 153 | (8 << 9),
    OUT_OF_RESOURCE = 0xce01,
    INVALID_HANDLE = 0xe401,
    INVALID_COMBINATION = 0xe801,
    DEVICE_NAME_BYTES = 0x100,
    PCM_SAMPLE_RATE = 48000,
    PCM_CHANNELS = 2,
    PCM_INT16 = 2,
    PCM_FRAME_BYTES = PCM_CHANNELS * sizeof(int16_t),
    MAX_RELEASED = 64,
    STOPPED = 1,
    STARTED = 0,
};

struct PokopiaAudioOut {
    Ps4PcmStream *stream;
    HznEvents *events;
    uint64_t event_key;
};

static const char output_name[DEVICE_NAME_BYTES] = "DeviceOut";

static void update_buffer_event(void *opaque, bool signaled)
{
    PokopiaAudioOut *out = opaque;
    hzn_events_set_signaled(out->events, out->event_key, signaled);
}

static void *output_buffer(const PokopiaAudioGuest *guest, const HznGuestBuffer *buffer, size_t bytes,
                           const uint8_t *tls, size_t tls_size)
{
    if (!buffer->address || buffer->size < bytes)
        return NULL;
    void *result = guest->write(guest->opaque, buffer->address, bytes);
    if (!result)
        return NULL;
    const uintptr_t target = (uintptr_t)result, message = (uintptr_t)tls;
    if (target >= message ? target - message < tls_size : message - target < bytes)
        return NULL;
    return result;
}

bool pokopia_audio_out_open(const HznCmifRequest *request, uint8_t *tls, size_t tls_size,
                           const PokopiaAudioGuest *guest, HznEvents *events, uint32_t session_handle,
                           PokopiaAudioOut **out)
{
    *out = NULL;
    if (!request->has_pid || !request->has_copy_handle || !request->has_output_buffer || request->raw_size < 32)
        return false;
    if (request->copy_handle != UINT32_C(0xffff8001))
        return hzn_cmif_reply(tls, tls_size, false, INVALID_HANDLE, NULL);
    const uint32_t rate = hzn_cmif_read32(request->raw + 16);
    const uint32_t channels = hzn_cmif_read32(request->raw + 20) & 0xffff;
    if ((rate && rate != PCM_SAMPLE_RATE) || (channels && channels != PCM_CHANNELS))
        return hzn_cmif_reply(tls, tls_size, false, OUT_OPERATION_FAILED, NULL);
    if (request->input.size != DEVICE_NAME_BYTES || request->output.size != DEVICE_NAME_BYTES)
        return hzn_cmif_reply(tls, tls_size, false, INVALID_COMBINATION, NULL);
    const char *name = guest->read(guest->opaque, request->input.address, DEVICE_NAME_BYTES);
    void *name_out = output_buffer(guest, &request->output, DEVICE_NAME_BYTES, tls, tls_size);
    if (!name || !name_out)
        return hzn_cmif_reply(tls, tls_size, false, OUT_OPERATION_FAILED, NULL);
    if (name[0] && memcmp(name, output_name, sizeof "DeviceOut"))
        return hzn_cmif_reply(tls, tls_size, false, OUT_NOT_FOUND, NULL);
    PokopiaAudioOut *stream = session_handle ? calloc(1, sizeof *stream) : NULL;
    if (!stream)
        return hzn_cmif_reply(tls, tls_size, false, OUT_OF_RESOURCE, NULL);
    stream->events = events;
    stream->event_key = UINT64_C(0x4155444f00000000) | session_handle;
    stream->stream = ps4_pcm_create(update_buffer_event, stream);
    if (!stream->stream) {
        free(stream);
        return hzn_cmif_reply(tls, tls_size, false, OUT_OF_RESOURCE, NULL);
    }
    const uint32_t format[] = { PCM_SAMPLE_RATE, PCM_CHANNELS, PCM_INT16, STOPPED };
    if (!hzn_cmif_reply_move_handle_data(tls, tls_size, 0, session_handle, format, sizeof format)) {
        pokopia_audio_out_close(stream);
        return false;
    }
    memcpy(name_out, output_name, sizeof output_name);
    *out = stream;
    return true;
}

void pokopia_audio_out_close(PokopiaAudioOut *out)
{
    if (!out)
        return;
    ps4_pcm_destroy(out->stream);
    free(out);
}

static uint32_t append_buffer(Ps4PcmStream *stream, const HznCmifRequest *request,
                              const PokopiaAudioGuest *guest)
{
    const size_t descriptor_size = 0x28;
    if (request->input.size != descriptor_size)
        return OUT_OPERATION_FAILED;
    const uint8_t *desc = guest->read(guest->opaque, request->input.address, descriptor_size);
    if (!desc)
        return OUT_OPERATION_FAILED;
    const uint64_t address = hzn_cmif_read64(desc + 8);
    const uint64_t capacity = hzn_cmif_read64(desc + 16);
    const uint64_t bytes = hzn_cmif_read64(desc + 24);
    const uint64_t offset = hzn_cmif_read64(desc + 32);
    if (offset > capacity || bytes > capacity - offset || address > UINT64_MAX - offset ||
        bytes > SIZE_MAX || bytes % PCM_FRAME_BYTES || bytes / PCM_FRAME_BYTES > UINT32_MAX)
        return OUT_OPERATION_FAILED;
    const void *pcm = bytes ? guest->read(guest->opaque, address + offset, (size_t)bytes) : NULL;
    if (bytes && !pcm)
        return OUT_OPERATION_FAILED;
    const uint64_t tag = hzn_cmif_read64(request->raw + 16);
    switch (ps4_pcm_append(stream, tag, pcm, (uint32_t)(bytes / PCM_FRAME_BYTES))) {
    case PS4_PCM_APPEND_OK:
        return 0;
    case PS4_PCM_APPEND_FULL:
        return OUT_BUFFER_FULL;
    default:
        return OUT_OPERATION_FAILED;
    }
}

bool pokopia_audio_out_request(PokopiaAudioOut *out, uint32_t command, const HznCmifRequest *request,
                              uint8_t *tls, size_t tls_size, const PokopiaAudioGuest *guest)
{
    if (request->has_pid || request->has_copy_handle)
        return false;
    const bool append = command == 3 || command == 7;
    const bool release = command == 5 || command == 8;
    if (request->has_buffer_descriptors != (append || release) || request->has_output_buffer != release)
        return false;
    Ps4PcmStream *stream = out->stream;
    uint32_t result = 0;
    switch (command) {
    case 0: {
        const uint32_t state = ps4_pcm_started(stream) ? STARTED : STOPPED;
        return hzn_cmif_reply(tls, tls_size, false, 0, &state);
    }
    case 1:
        if (ps4_pcm_start(stream) != 0)
            result = OUT_OPERATION_FAILED;
        break;
    case 2:
        if (ps4_pcm_stop(stream) != 0)
            result = OUT_OPERATION_FAILED;
        break;
    case 3:
    case 7:
        if (request->raw_size < 24)
            return false;
        result = append_buffer(stream, request, guest);
        break;
    case 4: {
        if (tls_size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return false;
        const uint32_t handle = hzn_events_copy_read(out->events, out->event_key);
        return hzn_cmif_reply_copy_handle(tls, tls_size, false, handle ? 0 : OUT_OF_RESOURCE, handle);
    }
    case 5:
    case 8: {
        uint32_t count = 0;
        if (request->output.size >= sizeof(uint64_t)) {
            const uint64_t capacity = request->output.size / sizeof(uint64_t);
            const size_t slots = capacity < MAX_RELEASED ? (size_t)capacity : MAX_RELEASED;
            uint64_t tags[MAX_RELEASED] = {0};
            const size_t bytes = slots * sizeof tags[0];
            void *data = output_buffer(guest, &request->output, bytes, tls, tls_size);
            if (!data)
                return hzn_cmif_reply(tls, tls_size, false, OUT_OPERATION_FAILED, NULL);
            count = ps4_pcm_take_released(stream, tags, (uint32_t)slots);
            memcpy(data, tags, bytes);
        }
        return hzn_cmif_reply(tls, tls_size, false, 0, &count);
    }
    case 6: {
        if (request->raw_size < 24)
            return false;
        const uint8_t contains = ps4_pcm_contains(stream, hzn_cmif_read64(request->raw + 16));
        return hzn_cmif_reply_data(tls, tls_size, false, 0, &contains, sizeof contains);
    }
    case 9: {
        const uint32_t count = ps4_pcm_queued(stream);
        return hzn_cmif_reply(tls, tls_size, false, 0, &count);
    }
    case 10: {
        const uint64_t played = ps4_pcm_played(stream);
        return hzn_cmif_reply_data(tls, tls_size, false, 0, &played, sizeof played);
    }
    case 11: {
        const uint8_t flushed = ps4_pcm_flush(stream);
        return hzn_cmif_reply_data(tls, tls_size, false, 0, &flushed, sizeof flushed);
    }
    case 12: {
        if (request->raw_size < 20)
            return false;
        float volume;
        memcpy(&volume, request->raw + 16, sizeof volume);
        ps4_pcm_set_volume(stream, volume);
        break;
    }
    case 13: {
        const float volume = ps4_pcm_get_volume(stream);
        return hzn_cmif_reply_data(tls, tls_size, false, 0, &volume, sizeof volume);
    }
    default:
        return false;
    }
    return hzn_cmif_reply(tls, tls_size, false, result, NULL);
}
