/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lm.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    DOMAIN_ROOT = 1,
    LOG_HEADER_SIZE = 0x18,
    MAX_PACKET_SIZE = 1 << 20,
};

static HznLmReply reply_svc(uint32_t result)
{
    return (HznLmReply){ .kind = HZN_LM_REPLY, .result = result };
}

static HznLmReply boundary(uint32_t command, uint32_t object_id)
{
    return (HznLmReply){ .kind = HZN_LM_BOUNDARY, .command = command, .object_id = object_id };
}

static HznLmReply reply_cmif(uint8_t *tls, size_t size, bool domain,
                              uint32_t result, const uint32_t *output)
{
    return reply_svc(hzn_cmif_reply(tls, size, domain, result, output) ? 0 : INVALID_COMBINATION);
}

void hzn_lm_init(HznLm *lm, uint64_t guest_pid)
{
    *lm = (HznLm){ .process_id = guest_pid, .next_object_id = 2, .destination = 1 };
}

static HznLmLogger *find_logger(HznLm *lm, uint32_t id)
{
    for (size_t i = 0; i < sizeof lm->loggers / sizeof lm->loggers[0]; ++i)
        if (lm->loggers[i].object_id == id)
            return &lm->loggers[i];
    return NULL;
}

static HznLmReply open_logger(HznLm *lm, uint8_t *tls, size_t size, uint64_t pid)
{
    HznLmLogger *logger = find_logger(lm, 0);
    if (!logger || lm->next_object_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    uint32_t id = lm->next_object_id;
    HznLmReply reply = reply_cmif(tls, size, true, 0, &id);
    if (!reply.result) {
        *logger = (HznLmLogger){ .object_id = id, .pid = pid };
        ++lm->next_object_id;
    }
    return reply;
}

static bool read_uleb(const uint8_t **p, const uint8_t *end, uint64_t *value)
{
    *value = 0;
    for (unsigned shift = 0; *p < end && shift < 64; shift += 7) {
        unsigned byte = *(*p)++;
        if (shift == 63 && byte > 1)
            return false;
        *value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

static void emit_message(HznLmLogger *logger, FILE *log)
{
    while (logger->used && logger->text[logger->used - 1] == ' ')
        --logger->used;
    if (log && logger->used) {
        logger->text[logger->used] = 0;
        fprintf(log, "[guestlog severity=%u] %s\n", logger->severity, logger->text);
        fflush(log);
    }
    logger->open = false;
    logger->used = 0;
}

static void log_packet(HznLmLogger *logger, const uint8_t *packet, size_t size, FILE *log)
{
    if (size < LOG_HEADER_SIZE || hzn_cmif_read32(packet + 0x14) != size - LOG_HEADER_SIZE) {
        if (log)
            fputs("[lm] dropped malformed log packet\n", log);
        return;
    }

    const uint8_t flags = packet[0x10];
    if (!logger->open || (flags & 1)) {
        logger->open = true;
        logger->used = 0;
        logger->severity = packet[0x12];
    }

    const uint8_t *p = packet + LOG_HEADER_SIZE;
    const uint8_t *end = packet + size;
    size_t limit = sizeof logger->text - 1;
    while (p < end) {
        uint64_t type, bytes;
        if (!read_uleb(&p, end, &type) || !read_uleb(&p, end, &bytes) ||
            bytes > (uint64_t)(end - p)) {
            if (log)
                fputs("[lm] dropped malformed log chunk\n", log);
            break;
        }
        if (type == 2) {
            size_t count = (size_t)bytes;
            if (count > limit - logger->used)
                count = limit - logger->used;
            for (size_t i = 0; i < count; ++i) {
                unsigned ch = p[i];
                logger->text[logger->used++] = ch < 0x20 || ch == 0x7f ? ' ' : (char)ch;
            }
        }
        p += (size_t)bytes;
    }

    if ((flags & 2) || logger->used == limit)
        emit_message(logger, log);
}

HznLmReply hzn_lm_send_sync(HznLm *lm, void *buffer, size_t size,
                            HznLmRead read_guest, void *opaque, FILE *log)
{
    if (!buffer)
        return reply_svc(INVALID_ADDRESS);
    if (size < 8)
        return reply_svc(INVALID_COMBINATION);

    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return reply_svc(0);
    }

    HznCmifRequest request = { 0 };
    if (!hzn_cmif_parse(tls, size, &request) || request.has_copy_handle)
        return reply_svc(INVALID_COMBINATION);

    const uint8_t *raw = request.raw;
    uint32_t command;
    if (request.type == 5 || request.type == 7) {
        if (request.raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI)
            return reply_svc(INVALID_COMBINATION);
        command = hzn_cmif_read32(raw + 8);
        if (command == 3) {
            uint32_t pointer_buffer_size = 0x8000;
            return reply_cmif(tls, size, false, 0, &pointer_buffer_size);
        }
        if (command == 0) {
            uint32_t id = DOMAIN_ROOT;
            HznLmReply reply = reply_cmif(tls, size, false, 0, &id);
            if (!reply.result && !lm->domain) {
                lm->domain = true;
                lm->root_open = true;
            }
            return reply;
        }
        return boundary(command, 0);
    }

    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0);
    if (!lm->domain)
        return boundary(request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI
                        ? hzn_cmif_read32(raw + 8) : UINT32_MAX, 0);
    if (request.raw_size < 16)
        return reply_svc(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    HznLmLogger *logger = id > DOMAIN_ROOT ? find_logger(lm, id) : NULL;
    if (raw[0] == 2) {
        HznLmReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result) {
            if (id == DOMAIN_ROOT)
                lm->root_open = false;
            if (logger)
                *logger = (HznLmLogger){ 0 };
        }
        return reply;
    }

    size_t in_size = hzn_cmif_read32(raw) >> 16;
    if (raw[0] != 1 || raw[1] || in_size < 16 ||
        in_size > request.raw_size - 16 || hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return reply_svc(INVALID_COMBINATION);
    command = hzn_cmif_read32(raw + 24);
    const uint8_t *args = raw + 32;

    if (id == DOMAIN_ROOT && lm->root_open) {
        if (command != 0)
            return boundary(command, id);
        if (in_size < 24)
            return reply_svc(INVALID_COMBINATION);
        uint64_t pid = hzn_cmif_read64(args);
        if (!pid && request.has_pid)
            pid = lm->process_id;
        return open_logger(lm, tls, size, pid);
    }
    if (!logger)
        return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
    if (command == 1) {
        if (in_size < 20)
            return reply_svc(INVALID_COMBINATION);
        uint32_t destination = hzn_cmif_read32(args);
        HznLmReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            lm->destination = destination;
        return reply;
    }
    if (command != 0)
        return boundary(command, id);

    const void *packet = NULL;
    HznGuestBuffer input = request.input;
    if (input.address && input.size && input.size <= MAX_PACKET_SIZE) {
        if (!read_guest || !(packet = read_guest(opaque, input.address, (size_t)input.size)))
            return reply_svc(INVALID_ADDRESS);
    }
    if (size < HZN_CMIF_DOMAIN_REPLY_BYTES)
        return reply_svc(INVALID_COMBINATION);
    if (packet)
        log_packet(logger, packet, (size_t)input.size, log);
    return reply_cmif(tls, size, true, 0, NULL);
}


