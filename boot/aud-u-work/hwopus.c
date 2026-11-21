/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hwopus.h"
#include "cmif.h"

#include <opus_multistream.h>
#include <stdlib.h>
#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    DOMAIN_ROOT = 1,
    MULTISTREAM_EXEX_CONFIG_BYTES = 0x118,
    GET_MULTISTREAM_WORK_SIZE_EXEX = 9,
    GUEST_PAGE_SIZE = 0x1000,
};

struct HznHwopusDomain {
    size_t refs;
    bool root_open;
};

void hzn_hwopus_init(HznHwopus *manager, const HznHwopusConfig *config)
{
    *manager = (HznHwopus){ .config = *config };
}

void hzn_hwopus_clone(HznHwopus *manager, const HznHwopus *source)
{
    *manager = *source;
    if (manager->domain)
        ++manager->domain->refs;
}

void hzn_hwopus_close(HznHwopus *manager)
{
    if (manager->domain && --manager->domain->refs == 0)
        free(manager->domain);
    *manager = (HznHwopus){0};
}

static HznHwopusReply handled(uint32_t result)
{
    return (HznHwopusReply){ .handled = true, .result = result };
}

static HznHwopusReply wrote(bool ok)
{
    return handled(ok ? 0 : INVALID_COMBINATION);
}

static HznHwopusReply boundary(uint32_t command)
{
    return (HznHwopusReply){ .command = command };
}

static HznHwopusReply control(HznHwopus *manager, uint8_t *tls, size_t size,
                             const HznCmifRequest *request)
{
    const uint8_t *raw = request->raw;
    if (request->raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI || request->has_pid ||
        request->has_copy_handle || request->has_buffer_descriptors)
        return handled(INVALID_COMBINATION);
    const uint32_t command = hzn_cmif_read32(raw + 8);
    if (command == 0) {
        if (size < HZN_CMIF_SESSION_REPLY_BYTES + sizeof(uint32_t))
            return handled(INVALID_COMBINATION);
        if (!manager->domain) {
            manager->domain = calloc(1, sizeof *manager->domain);
            if (!manager->domain)
                return wrote(hzn_cmif_reply(tls, size, false, OUT_OF_RESOURCE, NULL));
            *manager->domain = (HznHwopusDomain){ .refs = 1, .root_open = true };
        }
        const uint32_t root = DOMAIN_ROOT;
        return wrote(hzn_cmif_reply(tls, size, false, 0, &root));
    }
    if (command == 2 || command == 4) {
        if (request->raw_size < (command == 4 ? 20u : 16u) || size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return handled(INVALID_COMBINATION);
        if (!manager->config.clone_session)
            return boundary(command);
        const uint32_t handle = manager->config.clone_session(manager->config.opaque, manager);
        return wrote(hzn_cmif_reply_move_handle(tls, size, false, handle ? 0 : OUT_OF_RESOURCE, handle));
    }
    if (command == 3) {
        const uint32_t pointer_buffer_size = 0x8000;
        return wrote(hzn_cmif_reply(tls, size, false, 0, &pointer_buffer_size));
    }
    return boundary(command);
}

static HznHwopusReply work_size(HznHwopus *manager, uint8_t *tls, size_t size,
                               const HznCmifRequest *request, bool domain)
{
    if (request->has_pid || request->has_copy_handle || request->input.size != MULTISTREAM_EXEX_CONFIG_BYTES ||
        hzn_cmif_read32(tls) != (0x00010000u | request->type))
        return handled(INVALID_COMBINATION);
    const uint8_t *params = manager->config.read_guest
        ? manager->config.read_guest(manager->config.opaque, request->input.address, MULTISTREAM_EXEX_CONFIG_BYTES)
        : NULL;
    if (!params)
        return handled(INVALID_ADDRESS);
    const uint32_t rate = hzn_cmif_read32(params);
    const uint32_t channels = hzn_cmif_read32(params + 4);
    const uint32_t streams = hzn_cmif_read32(params + 8);
    const uint32_t coupled = hzn_cmif_read32(params + 12);
    if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        !channels || channels > 255 || !streams || streams > 255 || coupled > streams || streams + coupled > 255)
        return wrote(hzn_cmif_reply(tls, size, domain, INVALID_COMBINATION, NULL));
    const opus_int32 decoder_bytes = opus_multistream_decoder_get_size((int)streams, (int)coupled);
    if (decoder_bytes <= 0)
        return wrote(hzn_cmif_reply(tls, size, domain, INVALID_COMBINATION, NULL));
    /* The guest backs the decoder state with page-aligned transfer memory. */
    const uint32_t aligned_bytes = ((uint32_t)decoder_bytes + GUEST_PAGE_SIZE - 1) & ~(GUEST_PAGE_SIZE - 1u);
    return wrote(hzn_cmif_reply_data(tls, size, domain, 0, &aligned_bytes, sizeof aligned_bytes));
}

HznHwopusReply hzn_hwopus_send_sync(HznHwopus *manager, uint8_t *tls, size_t size)
{
    if (!tls || size < 8)
        return handled(INVALID_COMBINATION);
    const uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return handled(0);
    }
    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return handled(INVALID_COMBINATION);
    if (request.type == 5 || request.type == 7)
        return control(manager, tls, size, &request);
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX);

    const bool domain = manager->domain != NULL;
    const uint8_t *cmif = request.raw;
    if (domain) {
        if (request.raw_size < 16)
            return handled(INVALID_COMBINATION);
        const uint32_t header = hzn_cmif_read32(request.raw);
        const bool root = manager->domain->root_open && hzn_cmif_read32(request.raw + 4) == DOMAIN_ROOT;
        if (header == 2) {
            if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
                return handled(INVALID_COMBINATION);
            const bool reply = hzn_cmif_reply(tls, size, true, root ? 0 : INVALID_OBJECT, NULL);
            if (reply && root)
                manager->domain->root_open = false;
            return wrote(reply);
        }
        const size_t in_size = header >> 16;
        if ((header & 0xffff) != 1 || in_size < 16 || in_size > request.raw_size - 16)
            return handled(INVALID_COMBINATION);
        if (!root)
            return wrote(hzn_cmif_reply(tls, size, true, INVALID_OBJECT, NULL));
        cmif += 16;
    } else if (request.raw_size < 16) {
        return handled(INVALID_COMBINATION);
    }
    if (hzn_cmif_read32(cmif) != HZN_SFCI)
        return handled(INVALID_COMBINATION);
    const uint32_t command = hzn_cmif_read32(cmif + 8);
    if (command != GET_MULTISTREAM_WORK_SIZE_EXEX)
        return boundary(command);
    if (domain && (hzn_cmif_read32(request.raw) >> 16) != 16)
        return handled(INVALID_COMBINATION);
    return work_size(manager, tls, size, &request, domain);
}
