/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_device.h"

#include <string.h>

enum {
    LIST_NAMES = 0, GET_ACTIVE_NAME = 3, QUERY_SYSTEM_EVENT = 4, GET_ACTIVE_CHANNELS = 5,
    LIST_NAMES_AUTO = 6, GET_ACTIVE_NAME_AUTO = 10, QUERY_INPUT_EVENT = 11, QUERY_OUTPUT_EVENT = 12,
    GET_ACTIVE_OUTPUT_NAME = 13, LIST_OUTPUT_NAMES = 14,
    INVALID_ADDRESS = 0xcc01, INVALID_COMBINATION = 0xe801, OUT_OF_RESOURCE = 0xce01,
    DEVICE_NAME_BYTES = 0x100,
};
static const uint64_t audio_device_event_base = UINT64_C(0x41554400);
/* The renderer bridge exposes the PS4's stereo TV route as its output device. */
static const char output_name[DEVICE_NAME_BYTES] = "AudioTvOutput";

static bool write_output_name(const HznGuestBuffer *output, const uint8_t *tls, size_t tls_size,
                              void *(*write_guest)(void *, uint64_t, size_t))
{
    if (!write_guest || output->size < sizeof output_name)
        return false;
    void *name = write_guest(NULL, output->address, sizeof output_name);
    const uintptr_t target = (uintptr_t)name, reply = (uintptr_t)tls;
    if (!name || (target >= reply ? target - reply < tls_size : reply - target < sizeof output_name))
        return false;
    memcpy(name, output_name, sizeof output_name);
    return true;
}

bool pokopia_audio_device_request(uint32_t command, const HznCmifRequest *request,
                                  uint8_t *tls, size_t tls_size, HznEvents *events,
                                  void *(*write_guest)(void *, uint64_t, size_t))
{
    if (request->has_copy_handle)
        return false;
    switch (command) {
    case LIST_NAMES:
    case LIST_NAMES_AUTO:
    case LIST_OUTPUT_NAMES: {
        if (!request->has_output_buffer)
            return false;
        const uint32_t count = request->output.size >= sizeof output_name ? 1 : 0;
        if (count && !write_output_name(&request->output, tls, tls_size, write_guest))
            return hzn_cmif_reply(tls, tls_size, false, INVALID_ADDRESS, NULL);
        return hzn_cmif_reply(tls, tls_size, false, 0, &count);
    }
    case GET_ACTIVE_NAME:
    case GET_ACTIVE_NAME_AUTO:
    case GET_ACTIVE_OUTPUT_NAME:
        if (!request->has_output_buffer || request->output.size < sizeof output_name)
            return hzn_cmif_reply(tls, tls_size, false, INVALID_COMBINATION, NULL);
        return hzn_cmif_reply(tls, tls_size, false,
            write_output_name(&request->output, tls, tls_size, write_guest) ? 0 : INVALID_ADDRESS, NULL);
    case QUERY_SYSTEM_EVENT:
    case QUERY_INPUT_EVENT:
    case QUERY_OUTPUT_EVENT: {
        if (request->has_buffer_descriptors || !events || tls_size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return false;
        const uint32_t handle = hzn_events_copy_read(events, audio_device_event_base + command);
        return hzn_cmif_reply_copy_handle(tls, tls_size, false, handle ? 0 : OUT_OF_RESOURCE, handle);
    }
    case GET_ACTIVE_CHANNELS: {
        if (request->has_buffer_descriptors)
            return false;
        const uint32_t channels = 2;
        return hzn_cmif_reply(tls, tls_size, false, 0, &channels);
    }
    default:
        return false;
    }
}
