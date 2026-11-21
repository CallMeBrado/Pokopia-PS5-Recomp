/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_CMIF_H
#define POKOPIA_HORIZON_CMIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HZN_SFCI = 0x49434653,
    HZN_SFCO = 0x4f434653,
    HZN_CMIF_SESSION_REPLY_BYTES = 40,
    HZN_CMIF_DOMAIN_REPLY_BYTES = 56,
};

typedef struct {
    uint64_t address, size;
} HznGuestBuffer;

typedef struct {
    uint32_t type, copy_handle;
    bool has_pid, has_copy_handle, has_buffer_descriptors;
    bool has_output_buffer;
    const uint8_t *raw;
    size_t raw_size;
    HznGuestBuffer input, output;
} HznCmifRequest;

uint32_t hzn_cmif_read32(const uint8_t *p);
uint64_t hzn_cmif_read64(const uint8_t *p);

bool hzn_cmif_parse(const uint8_t *tls, size_t size, HznCmifRequest *request);
/* Also accepts one normal receive (B) descriptor without input descriptors. */
bool hzn_cmif_parse_with_output(const uint8_t *tls, size_t size, HznCmifRequest *request);
/* Requires exactly one normal A input and one normal B output. */
bool hzn_cmif_parse_with_input_output(const uint8_t *tls, size_t size, HznCmifRequest *request);
/* Also permits mode 1 (nonsecure) for the receive descriptor. */
bool hzn_cmif_parse_with_nonsecure_output(const uint8_t *tls, size_t size, HznCmifRequest *request);
/* Accepts exactly one normal auto-select output (one B and one C descriptor).
 * Both may be empty for a zero-byte transfer. The selected guest range is
 * returned in output, with the CMIF size entry excluded from raw_size. */
bool hzn_cmif_parse_with_auto_output(const uint8_t *tls, size_t size, HznCmifRequest *request);
bool hzn_cmif_reply(uint8_t *tls, size_t size, bool domain, uint32_t result,
                    const uint32_t *output);

/* Data bytes belong to the SFCO payload, not the domain object list. */
bool hzn_cmif_reply_data(uint8_t *tls, size_t size, bool domain, uint32_t result,
                         const void *payload, size_t payload_size);
bool hzn_cmif_reply_copy_handle(uint8_t *tls, size_t size, bool domain,
                               uint32_t result, uint32_t handle);
bool hzn_cmif_reply_move_handle(uint8_t *tls, size_t size, bool domain,
                               uint32_t result, uint32_t handle);
bool hzn_cmif_reply_move_handle_data(uint8_t *tls, size_t size, uint32_t result,
                                   uint32_t handle, const void *payload, size_t payload_size);

#endif

