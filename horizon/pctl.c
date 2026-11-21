/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pctl.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    DOMAIN_ROOT = 1,
};

static const char factory_type[] = "IParentalControlServiceFactory";
static const char service_type[] = "IParentalControlService";

void hzn_pctl_init(HznPctl *pctl)
{
    *pctl = (HznPctl){ .next_object_id = DOMAIN_ROOT + 1 };
}

static HznPctlReply svc_result(uint32_t result)
{
    return (HznPctlReply){ .kind = HZN_PCTL_REPLY, .result = result };
}

static HznPctlReply boundary(uint32_t command, uint32_t id, const char *type)
{
    return (HznPctlReply){ .kind = HZN_PCTL_BOUNDARY, .command = command,
                           .object_id = id, .object_type = type };
}

static HznPctlReply reply_cmif(uint8_t *tls, size_t size, bool domain, uint32_t result,
                               const uint32_t *output)
{
    return svc_result(hzn_cmif_reply(tls, size, domain, result, output) ? 0 : INVALID_COMBINATION);
}

static HznPctlObject *find_object(HznPctl *pctl, uint32_t id)
{
    for (size_t i = 0; i < sizeof pctl->objects / sizeof pctl->objects[0]; ++i)
        if (pctl->objects[i].id == id)
            return &pctl->objects[i];
    return NULL;
}

static HznPctlReply create_service(HznPctl *pctl, uint8_t *tls, size_t size, bool initialize)
{
    HznPctlObject *slot = find_object(pctl, 0);
    if (!slot || pctl->next_object_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    uint32_t id = pctl->next_object_id;
    HznPctlReply reply = reply_cmif(tls, size, true, 0, &id);
    if (!reply.result) {
        *slot = (HznPctlObject){ .id = id, .initialized = initialize };
        ++pctl->next_object_id;
    }
    return reply;
}

HznPctlReply hzn_pctl_send_sync(HznPctl *pctl, void *buffer, size_t size)
{
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        hzn_pctl_init(pctl);
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    const uint8_t *raw = request.raw;
    if (request.type == 5 || request.type == 7) {
        if (request.raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI ||
            request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        uint32_t command = hzn_cmif_read32(raw + 8);
        if (command == 3) {
            const uint16_t bytes = 0x8000;
            return svc_result(hzn_cmif_reply_data(tls, size, false, 0, &bytes, sizeof bytes)
                              ? 0 : INVALID_COMBINATION);
        }
        if (command == 0) {
            const uint32_t id = DOMAIN_ROOT;
            HznPctlReply reply = reply_cmif(tls, size, false, 0, &id);
            if (!reply.result && !pctl->domain) {
                pctl->domain = true;
                pctl->root_open = true;
            }
            return reply;
        }
        return boundary(command, 0, factory_type);
    }
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0, factory_type);
    if (!pctl->domain)
        return boundary(request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI
                        ? hzn_cmif_read32(raw + 8) : UINT32_MAX, 0, factory_type);
    if (request.raw_size < 16)
        return svc_result(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    bool root = id == DOMAIN_ROOT && pctl->root_open;
    HznPctlObject *object = id > DOMAIN_ROOT ? find_object(pctl, id) : NULL;
    uint32_t domain_header = hzn_cmif_read32(raw);
    if (domain_header == 2) {
        if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        HznPctlReply reply = reply_cmif(tls, size, true, root || object ? 0 : INVALID_OBJECT, NULL);
        if (!reply.result) {
            if (root)
                pctl->root_open = false;
            if (object)
                *object = (HznPctlObject){ 0 };
        }
        return reply;
    }

    size_t in_size = domain_header >> 16;
    if ((domain_header & 0xffff) != 1 || in_size < 16 || in_size > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(raw + 24);
    if (root) {
        if (command != 0 && command != 1)
            return boundary(command, id, factory_type);
        /* CreateService and CreateServiceWithoutInitialize take ClientProcessId. */
        if (in_size != 24 || !request.has_pid || request.has_copy_handle ||
            request.has_buffer_descriptors || hzn_cmif_read64(raw + 32) != 0)
            return svc_result(INVALID_COMBINATION);
        return create_service(pctl, tls, size, command == 0);
    }
    if (!object)
        return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
    /* R87: permission checks and queries (Check*, Confirm*, End*, Is*, 1001-1099;
     * 1017 came after a save and quit). Nothing is restricted on this console:
     * success, and zero for any boolean or value read back. */
    if (command > 1000 && command < 1100 && !request.has_buffer_descriptors && !request.has_copy_handle) {
        static const uint8_t zero[16];
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, zero, sizeof zero) ? 0 : INVALID_COMBINATION);
    }
    if (command != 1)
        return boundary(command, id, service_type);
    if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
        return svc_result(INVALID_COMBINATION);
    HznPctlReply reply = reply_cmif(tls, size, true, 0, NULL);
    if (!reply.result)
        object->initialized = true;
    return reply;
}
