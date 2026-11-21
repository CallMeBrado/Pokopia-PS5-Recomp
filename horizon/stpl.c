/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stpl.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    CREATE_USER_SERVICE = 0,
    GET_HID_SHARED_MEMORY = 300,
    GET_REQUIRED_MEMORY_SIZE = 1,
    WORK_PAGE_BYTES = 0x1000,
    BOOT_USER_MODE = 0x12,
    HANDLE_REPLY_BYTES = HZN_CMIF_SESSION_REPLY_BYTES + 8,
};

static HznStplReply svc_result(uint32_t result, uint32_t command)
{
    return (HznStplReply){.handled = true, .result = result, .command = command};
}

HznStplReply hzn_stpl_send_sync(bool user, const HznStplConfig *config, uint8_t *tls, size_t size)
{
    if (!tls)
        return svc_result(INVALID_ADDRESS, UINT32_MAX);
    if (size < 8)
        return svc_result(INVALID_COMBINATION, UINT32_MAX);
    if (hzn_cmif_read32(tls) == 2 && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return svc_result(0, UINT32_MAX);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return svc_result(INVALID_COMBINATION, UINT32_MAX);
    if (request.raw_size < 16 || hzn_cmif_read32(request.raw) != HZN_SFCI)
        return (HznStplReply){.command = UINT32_MAX};

    const uint32_t command = hzn_cmif_read32(request.raw + 8);
    if (request.type != 6 || hzn_cmif_read32(request.raw + 4) != 1 ||
        (command != (user ? GET_HID_SHARED_MEMORY : CREATE_USER_SERVICE) &&
         !(user && command == GET_REQUIRED_MEMORY_SIZE)))
        return (HznStplReply){.command = command};

    if (user && command == GET_REQUIRED_MEMORY_SIZE) {
        if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors || request.raw_size != 32)
            return svc_result(INVALID_COMBINATION, command);
        const uint8_t type = request.raw[16];
        uint32_t guests = hzn_cmif_read32(request.raw + 20);
        if (guests > 3)
            guests = 3;
        const uint32_t bytes = (type == 1 ? guests : 1 + guests * 3) * WORK_PAGE_BYTES;
        return svc_result(hzn_cmif_reply_data(tls, size, false, 0, &bytes, sizeof bytes)
                          ? 0 : INVALID_COMBINATION, command);
    }

    /* These are the two direct-session calls in SDK 0x75006b73dc and
     * 0x75006b885c. The HIPC word count leaves eight trailing pad bytes. */
    if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
        request.raw_size != (user ? 24u : 28u) ||
        (!user && hzn_cmif_read32(request.raw + 16) != BOOT_USER_MODE))
        return svc_result(INVALID_COMBINATION, command);

    const uint64_t hid_output[] = {HZN_STPL_HID_BYTES, 0};
    if (size < HANDLE_REPLY_BYTES + (user ? sizeof hid_output : 0))
        return svc_result(INVALID_COMBINATION, command);

    uint32_t handle = 0;
    if (user && config && config->copy_hid_memory)
        handle = config->copy_hid_memory(config->opaque, HZN_STPL_HID_BYTES);
    else if (!user && config && config->open_user)
        handle = config->open_user(config->opaque);
    const uint32_t result = handle ? 0 : OUT_OF_RESOURCE;
    const bool replied = user ?
        hzn_cmif_reply_copy_handle_data(tls, size, result, handle, hid_output, sizeof hid_output) :
        hzn_cmif_reply_move_handle(tls, size, false, result, handle);
    return svc_result(replied ? 0 : INVALID_COMBINATION, command);
}

