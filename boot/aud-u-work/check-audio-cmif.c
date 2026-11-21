/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint32_t recorded[24] = {
        0x01100006, 0x8000000c, 3, 0, 0, 0xffff8001,
        0x100, 0x00ad74e6, 0x5000001c, 0x100, 0x0c49fba8, 0xc000001c,
        HZN_SFCI, 1, 1, 0, 48000, 0xcafe0002, 0x51, 0,
    };
    uint32_t words[24];
    uint8_t *tls = (uint8_t *)words;
    HznCmifRequest request;
    memcpy(words, recorded, sizeof words);
    assert(!hzn_cmif_parse(tls, sizeof words, &request));
    assert(!hzn_cmif_parse_with_output(tls, sizeof words, &request));
    assert(!hzn_cmif_parse_with_input_output(tls, sizeof words - 1, &request));
    assert(hzn_cmif_parse_with_input_output(tls, sizeof words, &request));
    assert(request.type == 6 && request.has_pid && request.copy_handle == 0xffff8001);
    assert(request.raw == tls + 0x30 && request.raw_size == 48);
    assert(request.input.address == UINT64_C(0x7500ad74e6) && request.input.size == 0x100);
    assert(request.output.address == UINT64_C(0x7c0c49fba8) && request.output.size == 0x100);
    words[8] |= 1;
    assert(!hzn_cmif_parse_with_input_output(tls, sizeof words, &request));
    words[8] = recorded[8];
    words[11] |= 1 << 5;
    assert(!hzn_cmif_parse_with_input_output(tls, sizeof words, &request));
    words[11] = recorded[11];
    words[1] |= 3 << 10;
    assert(!hzn_cmif_parse_with_input_output(tls, sizeof words, &request));

    const uint32_t format[] = { 48000, 2, 2, 1 };
    const uint32_t expected[] = {
        0, 0x8000000c, 0x20, 0x10016, HZN_SFCO, 0, 0, 0,
        48000, 2, 2, 1, 0, 0, 0, 0,
    };
    memset(words, 0xaa, sizeof words);
    assert(!hzn_cmif_reply_move_handle_data(tls, sizeof expected - 1, 0, 0x10016, format, sizeof format));
    assert(words[0] == 0xaaaaaaaa);
    assert(hzn_cmif_reply_move_handle_data(tls, sizeof expected, 0, 0x10016, format, sizeof format));
    assert(!memcmp(words, expected, sizeof expected) && words[16] == 0xaaaaaaaa);
    puts("AudioOut recorded A+B parse and data-plus-moved-handle reply passed.");
}
