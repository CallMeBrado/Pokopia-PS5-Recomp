/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "offline_app.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    DOMAIN_ROOT = 1,
    NIFM_CREATE_GENERAL_SERVICE = 5,
    BCAT_CREATE_SERVICE = 0,
};

void hzn_offline_app_init(HznOfflineApp *service, HznOfflineAppKind kind,
                          HznOfflineAppClone clone, void *opaque)
{
    *service = (HznOfflineApp){ .kind = kind, .clone = clone, .opaque = opaque,
                                .next_id = DOMAIN_ROOT + 1, .root_open = true };
}

static HznOfflineAppReply svc(uint32_t result)
{
    return (HznOfflineAppReply){ .result = result };
}

static HznOfflineAppReply boundary(HznOfflineAppKind kind, uint32_t command, uint32_t id)
{
    const bool child = id > DOMAIN_ROOT;
    const char *name = kind == HZN_OFFLINE_NIFM_STATIC ? (child ? "NIFM IGeneralService" : "NIFM IStaticService") :
        (child ? "BCAT IBcatService" : "BCAT IServiceCreator");
    return (HznOfflineAppReply){ .command = command, .object_id = id, .boundary_type = name };
}

static HznOfflineAppReply reply(bool ok)
{
    return svc(ok ? 0 : INVALID_COMBINATION);
}

static uint32_t *find_object(HznOfflineApp *service, uint32_t id)
{
    for (size_t i = 0; i < sizeof service->objects / sizeof service->objects[0]; ++i)
        if (service->objects[i] == id)
            return &service->objects[i];
    return NULL;
}

static HznOfflineAppReply create_object(HznOfflineApp *service, uint8_t *tls, size_t size)
{
    uint32_t *slot = find_object(service, 0);
    if (!slot || service->next_id == UINT32_MAX)
        return reply(hzn_cmif_reply(tls, size, true, OUT_OF_RESOURCE, NULL));
    const uint32_t id = service->next_id;
    if (!hzn_cmif_reply(tls, size, true, 0, &id))
        return svc(INVALID_COMBINATION);
    *slot = service->next_id++;
    return svc(0);
}

static HznOfflineAppReply control(HznOfflineApp *service, uint32_t command,
                                  const HznCmifRequest *request, uint8_t *tls, size_t size)
{
    if (request->has_pid || request->has_copy_handle || request->has_buffer_descriptors)
        return svc(INVALID_COMBINATION);
    switch (command) {
    case 0: {
        const uint32_t id = DOMAIN_ROOT;
        if (!hzn_cmif_reply(tls, size, false, 0, &id))
            return svc(INVALID_COMBINATION);
        service->domain = true;
        return svc(0);
    }
    case 2: {
        if (size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return svc(INVALID_COMBINATION);
        uint32_t handle = service->clone ? service->clone(service->opaque, service) : 0;
        return reply(hzn_cmif_reply_move_handle(tls, size, false, handle ? 0 : OUT_OF_RESOURCE, handle));
    }
    case 3: {
        const uint16_t capacity = 0x8000;
        return reply(hzn_cmif_reply_data(tls, size, false, 0, &capacity, sizeof capacity));
    }
    default:
        return boundary(service->kind, command, 0);
    }
}

HznOfflineAppReply hzn_offline_app_send(HznOfflineApp *service, void *buffer, size_t size)
{
    if (!service || !buffer)
        return svc(INVALID_ADDRESS);
    if (size < 8)
        return svc(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    const uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return svc(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request) || request.raw_size < 16)
        return svc(INVALID_COMBINATION);
    const uint8_t *raw = request.raw;
    if (request.type == 5 || request.type == 7) {
        if (hzn_cmif_read32(raw) != HZN_SFCI)
            return svc(INVALID_COMBINATION);
        return control(service, hzn_cmif_read32(raw + 8), &request, tls, size);
    }
    if (request.type != 4 && request.type != 6)
        return boundary(service->kind, UINT32_MAX, 0);

    uint32_t id = DOMAIN_ROOT;
    size_t in_size = request.raw_size;
    if (service->domain) {
        const uint32_t header = hzn_cmif_read32(raw);
        id = hzn_cmif_read32(raw + 4);
        uint32_t *child = id > DOMAIN_ROOT ? find_object(service, id) : NULL;
        const bool root = id == DOMAIN_ROOT && service->root_open;
        if (header == 2) {
            if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
                return svc(INVALID_COMBINATION);
            if (!hzn_cmif_reply(tls, size, true, root || child ? 0 : INVALID_OBJECT, NULL))
                return svc(INVALID_COMBINATION);
            if (root)
                service->root_open = false;
            if (child)
                *child = 0;
            return svc(0);
        }
        in_size = header >> 16;
        if ((header & 0xffff) != 1 || in_size < 16 || in_size > request.raw_size - 16)
            return svc(INVALID_COMBINATION);
        if (!root && !child)
            return reply(hzn_cmif_reply(tls, size, true, INVALID_OBJECT, NULL));
        raw += 16;
    }
    if (hzn_cmif_read32(raw) != HZN_SFCI)
        return svc(INVALID_COMBINATION);

    const uint32_t command = hzn_cmif_read32(raw + 8);
    const uint32_t create = service->kind == HZN_OFFLINE_NIFM_STATIC ? NIFM_CREATE_GENERAL_SERVICE : BCAT_CREATE_SERVICE;
    if (id != DOMAIN_ROOT || command != create)
        return boundary(service->kind, command, id);
    if (!service->domain || !request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
        in_size != 24 || hzn_cmif_read64(raw + 16))
        return svc(INVALID_COMBINATION);
    return create_object(service, tls, size);
}
