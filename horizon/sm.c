/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sm.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_HANDLE = 0xe401,
    INVALID_COMBINATION = 0xe801,
    NOT_FOUND = 0xf201,
    INVALID_SERVICE_NAME = 0xc15,
    CLOSE_HIPC = 2,
    CLOSE_TIPC = 15,
    TIPC_BASE = 16,
    REGISTER_CLIENT = 0,
    GET_SERVICE_HANDLE = 1,
    DETACH_CLIENT = 4,
    TIPC_HEADER_BYTES = 8,
};

static const uint32_t special_header = UINT32_C(1) << 31;

static HznSmSession *find_session(HznSm *sm, uint32_t handle)
{
    if (!hzn_sm_owns_handle(sm, handle))
        return NULL;
    for (size_t i = 0; i < sizeof sm->sessions / sizeof sm->sessions[0]; ++i)
        if (sm->sessions[i].handle == handle)
            return &sm->sessions[i];
    return NULL;
}

bool hzn_sm_init(HznSm *sm, uint64_t guest_pid, uint32_t first_handle, uint32_t handle_limit)
{
    if (!sm || !guest_pid || !first_handle || first_handle >= handle_limit)
        return false;
    *sm = (HznSm){ .process_id = guest_pid, .first_handle = first_handle,
                   .next_handle = first_handle, .handle_limit = handle_limit };
    return true;
}

uint32_t hzn_sm_connect(HznSm *sm, const char *name, size_t name_bytes,
                        uint32_t *handle)
{
    if (!handle)
        return INVALID_ADDRESS;
    *handle = 0;
    if (!name || !name_bytes || !name[0])
        return INVALID_ADDRESS;
    if (name_bytes < sizeof "sm:" || memcmp(name, "sm:", sizeof "sm:") != 0)
        return NOT_FOUND;
    if (sm->next_handle == sm->handle_limit)
        return OUT_OF_RESOURCE;

    for (size_t i = 0; i < sizeof sm->sessions / sizeof sm->sessions[0]; ++i) {
        HznSmSession *session = &sm->sessions[i];
        if (!session->handle) {
            *handle = hzn_sm_allocate_handle(sm);
            *session = (HznSmSession){ .handle = *handle };
            return 0;
        }
    }
    return OUT_OF_RESOURCE;
}

uint32_t hzn_sm_allocate_handle(HznSm *sm)
{
    return sm->next_handle == sm->handle_limit ? 0 : sm->next_handle++;
}

bool hzn_sm_owns_handle(const HznSm *sm, uint32_t handle)
{
    return handle >= sm->first_handle && handle < sm->next_handle;
}

uint32_t hzn_sm_close_handle(HznSm *sm, uint32_t handle)
{
    HznSmSession *session = find_session(sm, handle);
    if (!session)
        return INVALID_HANDLE;
    *session = (HznSmSession){ 0 };
    return 0;
}

static uint32_t read_word(const uint8_t *bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static HznSmReply svc_result(uint32_t result)
{
    return (HznSmReply){ .kind = HZN_SM_REPLY, .result = result };
}

static HznSmReply reply_tipc(uint8_t *tls, size_t size, uint32_t result)
{
    /* TIPC replies have type 0 and a result in the first raw word.
     * There is no SFCI/SFCO header or CMIF alignment padding. */
    const uint32_t reply[] = { 0, 1, result };
    if (size < sizeof reply)
        return svc_result(INVALID_COMBINATION);
    memcpy(tls, reply, sizeof reply);
    return svc_result(0);
}

HznSmReply hzn_sm_move_service(HznSm *sm, void *tls, size_t size,
                              uint32_t *handle)
{
    if (!handle)
        return svc_result(INVALID_ADDRESS);
    *handle = 0;
    if (!tls)
        return svc_result(INVALID_ADDRESS);
    if (sm->next_handle == sm->handle_limit)
        return reply_tipc(tls, size, OUT_OF_RESOURCE);
    const uint32_t reply[] = { 0, special_header | 1, 0x20, sm->next_handle, 0 };
    if (size < sizeof reply)
        return svc_result(INVALID_COMBINATION);

    memcpy(tls, reply, sizeof reply);
    *handle = hzn_sm_allocate_handle(sm);
    return svc_result(0);
}

HznSmReply hzn_sm_send_sync(HznSm *sm, uint32_t handle, void *buffer, size_t size)
{
    HznSmSession *session = find_session(sm, handle);
    if (!session)
        return svc_result(INVALID_HANDLE);
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < TIPC_HEADER_BYTES)
        return svc_result(INVALID_COMBINATION);

    uint8_t *tls = buffer;
    uint32_t header0 = read_word(tls);
    uint32_t header1 = read_word(tls + 4);
    uint32_t type = header0 & 0xffffu;
    if (type == CLOSE_TIPC || type == CLOSE_HIPC) {
        if (header0 != type || header1 != 0)
            return svc_result(INVALID_COMBINATION);
        /* Release only acknowledges the server request. CloseHandle retires
         * the client handle, which the SDK still uses after this reply. */
        memset(tls, 0, TIPC_HEADER_BYTES);
        return svc_result(0);
    }

    uint32_t command = type >= TIPC_BASE ? type - TIPC_BASE : UINT32_MAX;
    HznSmReply boundary = { .kind = HZN_SM_COMMAND_BOUNDARY, .command = command };
    if (command == REGISTER_CLIENT || command == DETACH_CLIENT) {
        const bool sends_pid = header1 == special_header &&
                               size >= 20 && read_word(tls + 8) == 1;
        if (header0 != type || (!sends_pid &&
                                (command == REGISTER_CLIENT || header1 != 0)))
            return svc_result(INVALID_COMBINATION);
        HznSmReply reply = reply_tipc(tls, size, 0);
        if (!reply.result) {
            /* Horizon supplies the caller PID when it sees SendPid, regardless
             * of the bytes placed in the request by the guest. */
            session->registered_pid = command == REGISTER_CLIENT ? sm->process_id : 0;
        }
        return reply;
    }
    if (command != GET_SERVICE_HANDLE)
        return boundary;
    if (header0 != type || header1 != 2 || size < 16)
        return svc_result(INVALID_COMBINATION);
    if (!tls[TIPC_HEADER_BYTES])
        return reply_tipc(tls, size, INVALID_SERVICE_NAME);
    if (!session->registered_pid)
        return boundary;

    boundary.kind = HZN_SM_SERVICE_BOUNDARY;
    for (size_t i = 0; i < sizeof boundary.service - 1 && tls[TIPC_HEADER_BYTES + i]; ++i)
        boundary.service[i] = (char)tls[TIPC_HEADER_BYTES + i];
    return boundary;
}
