/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_copy(bool domain)
{
    static const uint32_t session_reply[] = {
        0, 0x80000008, 2, 0x10400,
        HZN_SFCO, 0, 0, 0,
        0, 0, 0, 0,
    };
    static const uint32_t domain_reply[] = {
        0, 0x8000000c, 2, 0x10400,
        0, 0, 0, 0,
        HZN_SFCO, 0, 0, 0,
        0, 0, 0, 0,
    };
    const uint32_t *expected = domain ? domain_reply : session_reply;
    size_t bytes = domain ? sizeof domain_reply : sizeof session_reply;
    uint8_t tls[sizeof domain_reply + 1], unchanged[sizeof tls];
    memset(tls, 0xcc, sizeof tls);
    memcpy(unchanged, tls, sizeof tls);

    assert(!hzn_cmif_reply_copy_handle(tls, bytes - 1, domain, 0, 0x10400));
    assert(!memcmp(tls, unchanged, sizeof tls));
    assert(hzn_cmif_reply_copy_handle(tls, bytes, domain, 0, 0x10400));
    assert(!memcmp(tls, expected, bytes) && tls[bytes] == 0xcc);

    HznCmifRequest response;
    assert(hzn_cmif_parse(tls, bytes, &response));
    assert(response.raw == tls + 16 && response.type == 0);
    assert(response.has_copy_handle && response.copy_handle == 0x10400);
    assert(!response.has_pid && !response.has_buffer_descriptors);

    assert(hzn_cmif_reply_copy_handle(tls, bytes - 8, domain, 0x1234, 0x10400));
    assert(hzn_cmif_parse(tls, bytes - 8, &response));
    assert(!response.has_copy_handle && !response.has_pid);
    assert(hzn_cmif_read32(response.raw + (domain ? 24 : 8)) == 0x1234);
}

static void check_receive_descriptor(void)
{
    uint32_t tls[64] = {
        0x01000006, 0x8000000c, 1, 0, 0,
        0x12345678, 0x89abcdef, 0xab000114,
        HZN_SFCI, 1, 3, 0,
    };
    HznCmifRequest request;
    assert(!hzn_cmif_parse((uint8_t *)tls, sizeof tls, &request));
    assert(hzn_cmif_parse_with_output((uint8_t *)tls, 80, &request));
    assert(request.type == 6 && request.has_pid && !request.has_copy_handle);
    assert(request.has_buffer_descriptors && request.has_output_buffer);
    assert(request.output.address == UINT64_C(0x45a89abcdef));
    assert(request.output.size == UINT64_C(0xb12345678));
    assert(request.raw == (uint8_t *)(tls + 8) && request.raw_size == 48);
    assert(!hzn_cmif_parse_with_output((uint8_t *)tls, 79, &request));
    assert(!hzn_cmif_parse_with_output((uint8_t *)tls, 31, &request));

    tls[7] |= 1;
    assert(!hzn_cmif_parse_with_output((uint8_t *)tls, sizeof tls, &request));
    assert(hzn_cmif_parse_with_nonsecure_output((uint8_t *)tls, sizeof tls, &request));
    tls[7] |= 2;
    assert(!hzn_cmif_parse_with_nonsecure_output((uint8_t *)tls, sizeof tls, &request));
    tls[7] &= ~2u;
    tls[7] &= ~1u;
    tls[0] = 0x02000006;
    assert(!hzn_cmif_parse_with_output((uint8_t *)tls, sizeof tls, &request));
    tls[0] = 0x01100006;
    assert(!hzn_cmif_parse_with_output((uint8_t *)tls, sizeof tls, &request));
}

static void check_auto_output(void)
{
    uint32_t tls[64] = {0x01000006, 0x0c0f};
    tls[8] = 0x180001;
    tls[9] = 4;
    tls[12] = HZN_SFCI;
    tls[13] = 1;
    tls[14] = 11;
    tls[19] = 0xbeef0088; /* 16-bit pointer length plus unused alignment. */
    tls[20] = 0xf1234560;
    tls[21] = 0x0088007c;
    const uint8_t *wire = (const uint8_t *)tls;
    HznCmifRequest request;
    assert(!hzn_cmif_parse(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_output(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_nonsecure_output(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_auto_output(wire, 87, &request));
    assert(hzn_cmif_parse_with_auto_output(wire, 88, &request));
    assert(request.type == 6 && request.has_buffer_descriptors && request.has_output_buffer);
    assert(request.raw == wire + 32 && request.raw_size == 44);
    assert(request.output.address == UINT64_C(0x7cf1234560) && request.output.size == 0x88);

    tls[19] ^= 1;
    assert(!hzn_cmif_parse_with_auto_output(wire, sizeof tls, &request));
    tls[19] ^= 1;
    tls[2] = 0x88;
    tls[3] = 0xf1234560;
    tls[4] = 0xc000001c;
    assert(!hzn_cmif_parse_with_auto_output(wire, sizeof tls, &request));
    tls[20] = 0;
    tls[21] = 0;
    /* Some SDK clients leave the unused size entry untouched on B fallback. */
    assert(hzn_cmif_parse_with_auto_output(wire, sizeof tls, &request));
    assert(request.output.address == UINT64_C(0x7cf1234560) && request.output.size == 0x88);
    tls[4] |= 1;
    assert(!hzn_cmif_parse_with_auto_output(wire, sizeof tls, &request));
}

static void check_pointer_output(void)
{
    uint32_t tls[64] = { 6, 0x0c0f, [4] = 0x00180001, [5] = 2, [8] = HZN_SFCI,
        [9] = 1, [10] = 202, [12] = 0x0a0b07ea, [13] = 0x00223701,
        [16] = 0x00430008, [17] = 0x0010f658, [18] = 0x0008007c };
    const uint8_t *wire = (const uint8_t *)tls;
    HznCmifRequest request;
    assert(!hzn_cmif_parse(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_output(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_auto_output(wire, sizeof tls, &request));
    assert(!hzn_cmif_parse_with_pointer_output(wire, 75, &request));
    assert(hzn_cmif_parse_with_pointer_output(wire, 76, &request));
    assert(request.type == 6 && request.has_buffer_descriptors && request.has_output_buffer);
    assert(request.raw == wire + 16 && request.raw_size == 48);
    assert(request.output.address == UINT64_C(0x7c0010f658) && request.output.size == 8);

    tls[16] ^= 1;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
    tls[16] ^= 1;
    tls[0] = 0x00010006;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
    tls[0] = 0x00100006;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
    tls[0] = 0x01000006;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
    tls[0] = 6;
    tls[1] |= 0x80000000;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
    tls[1] &= ~0x80000000u;
    tls[17] = 0;
    tls[18] = 0x00080000;
    assert(!hzn_cmif_parse_with_pointer_output(wire, sizeof tls, &request));
}

static void check_move(void)
{
    uint32_t tls[16];
    memset(tls, 0xcc, sizeof tls);
    assert(!hzn_cmif_reply_move_handle((uint8_t *)tls, 47, false, 0, 0x10400));
    assert(tls[0] == 0xcccccccc);
    assert(hzn_cmif_reply_move_handle((uint8_t *)tls, 48, false, 0, 0x10400));
    assert(tls[0] == 0 && tls[1] == 0x80000008 && tls[2] == 0x20 && tls[3] == 0x10400);
    assert(tls[4] == HZN_SFCO && tls[6] == 0 && tls[12] == 0xcccccccc);
    assert(hzn_cmif_reply_move_handle((uint8_t *)tls, 40, false, 0x1234, 0x10400));
    assert(tls[1] == 8 && tls[2] == 0 && tls[3] == 0 && tls[6] == 0x1234);
}

int main(void)
{
    check_copy(false);
    check_copy(true);
    check_receive_descriptor();
    check_auto_output();
    check_pointer_output();
    check_move();
    puts("PASS Horizon CMIF: copied and moved handles, opt-in receive descriptors");
    return 0;
}
