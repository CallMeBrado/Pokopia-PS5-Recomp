/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aoc.h"
#include "cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t output_address = UINT64_C(0x7123456000);

typedef struct {
    uint32_t output[4];
    unsigned copies, writes;
} TestAocHost;

static void *write_guest(void *opaque, uint64_t address, size_t bytes)
{
    TestAocHost *host = opaque;
    ++host->writes;
    return address == output_address && bytes <= sizeof host->output ? host->output : NULL;
}

static uint32_t copy_event(void *opaque)
{
    TestAocHost *host = opaque;
    return 0x31000 + ++host->copies;
}

static void set_call(uint32_t tls[64], uint32_t command, bool with_pid)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = with_pid ? 0x8000000a : 8;
    tls[2] = with_pid;
    uint32_t *cmif = tls + (with_pid ? 8 : 4);
    cmif[0] = HZN_SFCI;
    cmif[1] = 1;
    cmif[2] = command;
}

static void set_list(uint32_t tls[64], uint32_t offset, uint32_t max, uint64_t address)
{
    set_call(tls, 3, true);
    tls[0] |= 0x1000000;
    tls[1] = 0x8000000c;
    tls[5] = max * 4;
    tls[6] = (uint32_t)address;
    tls[7] = (uint32_t)((address >> 36) << 2) | (uint32_t)((address >> 32) & 15) << 28;
    tls[12] = offset;
    tls[13] = max;
}

static void check_count_reply(const uint32_t tls[64], uint32_t count)
{
    assert(tls[0] == 0 && tls[1] == 9 && tls[4] == HZN_SFCO && tls[6] == 0);
    assert(tls[8] == count);
}

int main(void)
{
    HznAoc aoc;
    uint32_t tls[64];
    assert(!hzn_aoc_init(&aoc, NULL, 1));
    assert(hzn_aoc_init(&aoc, NULL, 0));
    set_call(tls, 2, true);
    assert(hzn_aoc_send_sync(&aoc, tls, 60).result == 0);
    check_count_reply(tls, 0);
    set_list(tls, 0, 0, 0);
    assert(hzn_aoc_send_sync(&aoc, tls, 80).result == 0);
    check_count_reply(tls, 0);
    set_call(tls, 50, false);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xe801);
    set_call(tls, 50, true);
    tls[12] = 1;
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xe801);
    tls[12] = 0;
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0);
    assert(tls[1] == 8 && tls[4] == HZN_SFCO && tls[6] == 0);

    const uint32_t catalog[] = { 1, 7, 25 };
    TestAocHost host = { .output = { 0xeeee, 0xeeee, 0xeeee, 0xeeee } };
    assert(hzn_aoc_init(&aoc, catalog, sizeof catalog / sizeof catalog[0]));
    aoc.write_guest = write_guest;
    aoc.copy_event = copy_event;
    aoc.opaque = &host;

    set_call(tls, 2, true);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0);
    check_count_reply(tls, 3);
    set_call(tls, 2, false);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xe801);

    set_list(tls, 1, 3, output_address);
    assert(hzn_aoc_send_sync(&aoc, tls, 79).result == 0xe801);
    assert(tls[0] == 0x1000006 && host.writes == 0);
    assert(hzn_aoc_send_sync(&aoc, tls, 80).result == 0);
    check_count_reply(tls, 2);
    assert(host.writes == 1 && host.output[0] == 7 && host.output[1] == 25 && host.output[2] == 0xeeee);
    set_list(tls, 0, 1, output_address);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0);
    check_count_reply(tls, 1);
    assert(host.writes == 2 && host.output[0] == 1);
    set_list(tls, 3, 2, output_address);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0);
    check_count_reply(tls, 0);
    assert(host.writes == 2);

    set_list(tls, 0, 2, output_address);
    tls[5] = 4;
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xe801 && host.writes == 2);
    set_list(tls, 0, 2, 0x1234);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xcc01 && host.writes == 3);

    set_call(tls, 8, false);
    assert(hzn_aoc_send_sync(&aoc, tls, 47).result == 0xe801 && host.copies == 0);
    assert(hzn_aoc_send_sync(&aoc, tls, 48).result == 0);
    assert(tls[1] == 0x80000008 && tls[2] == 2 && tls[3] == 0x31001);
    assert(tls[4] == HZN_SFCO && tls[6] == 0);
    set_call(tls, 10, false);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0xe801 && host.copies == 1);
    set_call(tls, 10, true);
    assert(hzn_aoc_send_sync(&aoc, tls, sizeof tls).result == 0);
    assert(tls[1] == 0x80000008 && tls[2] == 2 && tls[3] == 0x31002);
    assert(tls[4] == HZN_SFCO && tls[6] == 0);

    uint32_t saved[64];
    set_call(tls, 5, true);
    memcpy(saved, tls, sizeof saved);
    HznAocReply reply = hzn_aoc_send_sync(&aoc, tls, sizeof tls);
    assert(reply.kind == HZN_AOC_BOUNDARY && reply.command == 5 && reply.object_id == 0);
    assert(!memcmp(saved, tls, sizeof saved));
    set_call(tls, 50, true);
    memcpy(saved, tls, sizeof saved);
    reply = hzn_aoc_send_sync(&aoc, tls, sizeof tls);
    assert(reply.kind == HZN_AOC_BOUNDARY && reply.command == 50);
    assert(!memcmp(saved, tls, sizeof saved));

    puts("PASS Horizon aoc:u: catalog count, paged lists, list-change event and empty mount check");
    return 0;
}
