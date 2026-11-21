/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aoc.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    COUNT_ADD_ON_CONTENT = 2,
    LIST_ADD_ON_CONTENT = 3,
    GET_LIST_CHANGED_EVENT = 8,
    GET_LIST_CHANGED_EVENT_WITH_PID = 10,
    CHECK_ADD_ON_CONTENT_MOUNT_STATUS = 50,
};

bool hzn_aoc_init(HznAoc *aoc, const uint32_t *content_ids, size_t content_count)
{
    if (content_count > INT32_MAX || (content_count && !content_ids))
        return false;
    *aoc = (HznAoc){ .content_ids = content_ids, .content_count = (uint32_t)content_count };
    return true;
}

static HznAocReply svc_result(uint32_t result)
{
    return (HznAocReply){ .kind = HZN_AOC_REPLY, .result = result };
}

static HznAocReply boundary(uint32_t command)
{
    return (HznAocReply){ .kind = HZN_AOC_BOUNDARY, .command = command };
}

static HznAocReply list_content(HznAoc *aoc, uint8_t *tls, size_t size,
                                const HznCmifRequest *request)
{
    /* ListAddOnContent takes the offset, maximum count and a ClientProcessId. */
    if (!request->has_pid || !request->has_output_buffer || request->raw_size < 32 ||
        size < HZN_CMIF_SESSION_REPLY_BYTES + sizeof(uint32_t))
        return svc_result(INVALID_COMBINATION);
    uint32_t offset = hzn_cmif_read32(request->raw + 16);
    uint32_t maximum = hzn_cmif_read32(request->raw + 20);
    if ((uint64_t)maximum * sizeof(uint32_t) > request->output.size)
        return svc_result(INVALID_COMBINATION);

    uint32_t count = offset < aoc->content_count ? aoc->content_count - offset : 0;
    if (count > maximum)
        count = maximum;
    if (count) {
        if (!aoc->write_guest)
            return boundary(LIST_ADD_ON_CONTENT);
        size_t bytes = count * sizeof(uint32_t);
        void *output = aoc->write_guest(aoc->opaque, request->output.address, bytes);
        if (!output)
            return svc_result(INVALID_ADDRESS);
        uintptr_t dest = (uintptr_t)output, reply = (uintptr_t)tls;
        if (dest >= reply ? dest - reply < size : reply - dest < bytes)
            return svc_result(INVALID_ADDRESS);
        memcpy(output, aoc->content_ids + offset, bytes);
    }
    return svc_result(hzn_cmif_reply(tls, size, false, 0, &count) ? 0 : INVALID_COMBINATION);
}

HznAocReply hzn_aoc_send_sync(HznAoc *aoc, void *buffer, size_t size)
{
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse_with_output(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    if (request.raw_size < 16 || hzn_cmif_read32(request.raw) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(request.raw + 8);
    if (request.type != 4 && request.type != 6)
        return boundary(command);

    switch (command) {
    case COUNT_ADD_ON_CONTENT:
        if (!request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            request.raw_size < 24)
            return svc_result(INVALID_COMBINATION);
        return svc_result(hzn_cmif_reply(tls, size, false, 0, &aoc->content_count)
                          ? 0 : INVALID_COMBINATION);
    case LIST_ADD_ON_CONTENT:
        if (request.has_copy_handle)
            return svc_result(INVALID_COMBINATION);
        return list_content(aoc, tls, size, &request);
    case CHECK_ADD_ON_CONTENT_MOUNT_STATUS:
        /* SDK ClientProcessId is PID plus a zero u64 input. The mount check is
         * complete with an empty inventory. A populated catalog needs mounts. */
        if (!request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            request.raw_size != 28 || hzn_cmif_read64(request.raw + 16) != 0)
            return svc_result(INVALID_COMBINATION);
        if (aoc->content_count)
            return boundary(command);
        return svc_result(hzn_cmif_reply(tls, size, false, 0, NULL) ? 0 : INVALID_COMBINATION);
    case GET_LIST_CHANGED_EVENT:
    case GET_LIST_CHANGED_EVENT_WITH_PID: {
        bool with_pid = command == GET_LIST_CHANGED_EVENT_WITH_PID;
        if (request.has_pid != with_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            request.raw_size < (with_pid ? 24u : 16u) || size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return svc_result(INVALID_COMBINATION);
        if (!aoc->copy_event)
            return boundary(command);
        uint32_t handle = aoc->copy_event(aoc->opaque);
        uint32_t result = handle ? 0 : OUT_OF_RESOURCE;
        return svc_result(hzn_cmif_reply_copy_handle(tls, size, false, result, handle)
                          ? 0 : INVALID_COMBINATION);
    }
    default:
        return boundary(command);
    }
}
