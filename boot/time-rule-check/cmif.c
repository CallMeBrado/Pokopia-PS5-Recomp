/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmif.h"

#include <string.h>

uint32_t hzn_cmif_read32(const uint8_t *p)
{
    uint32_t result;
    memcpy(&result, p, sizeof result);
    return result;
}

uint64_t hzn_cmif_read64(const uint8_t *p)
{
    uint64_t result;
    memcpy(&result, p, sizeof result);
    return result;
}

typedef enum { NO_OUTPUT, NORMAL_OUTPUT, NONSECURE_OUTPUT, AUTO_OUTPUT, INPUT_OUTPUT } OutputMode;

static bool parse_request(const uint8_t *tls, size_t size, HznCmifRequest *request,
                          OutputMode output_mode)
{
    if (size < 8)
        return false;
    uint32_t h0 = hzn_cmif_read32(tls);
    uint32_t h1 = hzn_cmif_read32(tls + 4);
    unsigned pointers = (h0 >> 16) & 15;
    unsigned sends = (h0 >> 20) & 15;
    unsigned receives = (h0 >> 24) & 15;
    unsigned receive_mode = (h1 >> 10) & 15;
    if (pointers > 1 || sends > 1 || (h0 >> 28))
        return false;
    if (output_mode == INPUT_OUTPUT && (pointers || sends != 1 || receives != 1 ||
        (h1 & UINT32_C(0x7ffffc00))))
        return false;
    if (output_mode == AUTO_OUTPUT) {
        uint32_t type = h0 & 0xffff;
        if (receive_mode != 3 || receives != 1 || (h1 >> 14) || (type != 4 && type != 6))
            return false;
    } else if (receive_mode) {
        return false;
    }
    if (receives && (output_mode == NO_OUTPUT || receives != 1 || pointers ||
        (sends && output_mode != INPUT_OUTPUT)))
        return false;

    *request = (HznCmifRequest){ .type = h0 & 0xffff,
                                 .has_buffer_descriptors = pointers || sends || receives,
                                 .has_output_buffer = receives != 0 };
    size_t pos = 8;
    if (h1 >> 31) {
        if (size - pos < 4)
            return false;
        uint32_t special = hzn_cmif_read32(tls + pos);
        if (special > 3)
            return false; /* At most one copy handle, no moved handles. */
        pos += 4;
        if (special & 1) {
            if (size - pos < 8)
                return false;
            request->has_pid = true;
            pos += 8; /* The kernel supplies the PID, not the guest. */
        }
        if (special & 2) {
            if (size - pos < 4)
                return false;
            request->has_copy_handle = true;
            request->copy_handle = hzn_cmif_read32(tls + pos);
            pos += 4;
        }
    }

    if (pointers) {
        if (size - pos < 8)
            return false;
        uint32_t bits = hzn_cmif_read32(tls + pos);
        request->input = (HznGuestBuffer){
            hzn_cmif_read32(tls + pos + 4) | ((uint64_t)((bits >> 12) & 15) << 32) |
                                            ((uint64_t)((bits >> 6) & 7) << 36),
            bits >> 16,
        };
        pos += 8;
    }
    if (sends) {
        if (size - pos < 12)
            return false;
        uint32_t bits = hzn_cmif_read32(tls + pos + 8);
        if (output_mode == INPUT_OUTPUT && (bits & UINT32_C(0x00ffffe3)))
            return false;
        uint64_t address = hzn_cmif_read32(tls + pos + 4) |
                           ((uint64_t)((bits >> 28) & 15) << 32) |
                           ((uint64_t)((bits >> 2) & 7) << 36);
        if (address)
            request->input = (HznGuestBuffer){
                address, hzn_cmif_read32(tls + pos) | ((uint64_t)((bits >> 24) & 15) << 32),
            };
        pos += 12;
    }
    if (receives) {
        if (size - pos < 12)
            return false;
        uint32_t bits = hzn_cmif_read32(tls + pos + 8);
        if ((bits & 3) > (output_mode == NONSECURE_OUTPUT ? 1u : 0u))
            return false;
        if (output_mode == INPUT_OUTPUT && (bits & UINT32_C(0x00ffffe0)))
            return false;
        request->output = (HznGuestBuffer){
            hzn_cmif_read32(tls + pos + 4) | ((uint64_t)((bits >> 28) & 15) << 32) |
                                            ((uint64_t)((bits >> 2) & 0x3fffff) << 36),
            hzn_cmif_read32(tls + pos) | ((uint64_t)((bits >> 24) & 15) << 32),
        };
        pos += 12;
    }

    size_t bytes = (h1 & 0x3ff) * 4;
    size_t pad = (16 - pos % 16) % 16;
    if (bytes > size - pos || pad > bytes)
        return false;
    request->raw = tls + pos + pad;
    request->raw_size = bytes - pad;
    if (output_mode == AUTO_OUTPUT) {
        /* A single variable C descriptor follows the HIPC data words. Its
         * u16 CMIF size entry occupies the start of the final data word. */
        if (request->raw_size < 4 || size - pos - bytes < 8)
            return false;
        const uint8_t *entry = tls + pos + bytes;
        uint32_t high_and_size = hzn_cmif_read32(entry + 4);
        HznGuestBuffer pointer = {
            hzn_cmif_read32(entry) | ((uint64_t)(high_and_size & 0xffff) << 32),
            high_and_size >> 16,
        };
        bool have_alias = request->output.address || request->output.size;
        if ((have_alias && (pointer.address || pointer.size)) ||
            (request->output.size && !request->output.address) || (pointer.size && !pointer.address))
            return false;
        if (!have_alias) {
            uint16_t pointer_size;
            memcpy(&pointer_size, entry - 4, sizeof pointer_size);
            if ((pointer.address || pointer.size) && pointer_size != pointer.size)
                return false;
            request->output = pointer;
        }
        request->raw_size -= 4;
    }
    return true;
}

bool hzn_cmif_parse(const uint8_t *tls, size_t size, HznCmifRequest *request)
{
    return parse_request(tls, size, request, NO_OUTPUT);
}

bool hzn_cmif_parse_with_output(const uint8_t *tls, size_t size, HznCmifRequest *request)
{
    return parse_request(tls, size, request, NORMAL_OUTPUT);
}

bool hzn_cmif_parse_with_input_output(const uint8_t *tls, size_t size, HznCmifRequest *request)
{
    return parse_request(tls, size, request, INPUT_OUTPUT);
}

bool hzn_cmif_parse_with_nonsecure_output(const uint8_t *tls, size_t size, HznCmifRequest *request)
{
    return parse_request(tls, size, request, NONSECURE_OUTPUT);
}

bool hzn_cmif_parse_with_auto_output(const uint8_t *tls, size_t size, HznCmifRequest *request)
{
    return parse_request(tls, size, request, AUTO_OUTPUT);
}

static void write32(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof value);
}

static bool write_reply(uint8_t *tls, size_t size, bool domain, uint32_t result,
                        const void *payload, size_t payload_size, uint32_t objects,
                        uint32_t handle, uint32_t special)
{
    /* A single handle ends on the raw data's 16-byte boundary. The entire
     * alignment budget then follows the payload instead of straddling it. */
    size_t headers = domain ? 32 : 16;
    if (payload_size > 0x3ff * 4 - 16 - headers)
        return false;
    size_t raw_bytes = 16 + headers + ((payload_size + 3) & ~(size_t)3);
    size_t total = (special ? 16 : 8) + raw_bytes;
    if (size < total)
        return false;

    memset(tls, 0, total);
    write32(tls + 4, (uint32_t)(raw_bytes / 4) | (special ? 0x80000000u : 0));
    if (special) {
        write32(tls + 8, special);
        write32(tls + 12, handle);
    }
    if (domain)
        write32(tls + 16, objects);
    uint8_t *sfco = tls + (domain ? 32 : 16);
    write32(sfco, HZN_SFCO);
    write32(sfco + 8, result);
    if (payload_size)
        memcpy(sfco + 16, payload, payload_size);
    return true;
}

bool hzn_cmif_reply(uint8_t *tls, size_t size, bool domain, uint32_t result,
                    const uint32_t *output)
{
    return write_reply(tls, size, domain, result, output, output ? sizeof *output : 0,
                       domain && output ? 1 : 0, 0, 0);
}

bool hzn_cmif_reply_data(uint8_t *tls, size_t size, bool domain, uint32_t result,
                         const void *payload, size_t payload_size)
{
    return write_reply(tls, size, domain, result, payload, payload_size, 0, 0, 0);
}

bool hzn_cmif_reply_copy_handle(uint8_t *tls, size_t size, bool domain,
                               uint32_t result, uint32_t handle)
{
    return write_reply(tls, size, domain, result, NULL, 0, 0, handle, result ? 0 : 2);
}

bool hzn_cmif_reply_move_handle(uint8_t *tls, size_t size, bool domain,
                               uint32_t result, uint32_t handle)
{
    return write_reply(tls, size, domain, result, NULL, 0, 0, handle, result ? 0 : 0x20);
}

bool hzn_cmif_reply_move_handle_data(uint8_t *tls, size_t size, uint32_t result,
                                   uint32_t handle, const void *payload, size_t payload_size)
{
    if (result)
        return hzn_cmif_reply(tls, size, false, result, NULL);
    if (!handle || (!payload && payload_size))
        return false;
    return write_reply(tls, size, false, 0, payload, payload_size, 0, handle, 0x20);
}
