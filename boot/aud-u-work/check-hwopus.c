/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hwopus.h"
#include "cmif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static HznHwopus child;
static uint32_t config_words[0x118 / 4] = { 48000, 2, 2, 0 };

static uint32_t clone_session(void *opaque, const HznHwopus *source)
{
    assert(!opaque);
    hzn_hwopus_clone(&child, source);
    return 0x10018;
}

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    assert(!opaque);
    return address == 0x12345000 && size == sizeof config_words ? config_words : NULL;
}

static uint32_t work_size(HznHwopus *manager, uint32_t error)
{
    uint32_t tls[64] = {
        0x00010006, 12, 0x01180000, 0x12345000, 0x00100001, 1, 0, 0,
        HZN_SFCI, 1, 9, 0,
    };
    const HznHwopusReply reply = hzn_hwopus_send_sync(manager, (uint8_t *)tls, sizeof tls);
    assert(reply.handled && !reply.result && tls[8] == HZN_SFCO && tls[10] == error);
    return error ? 0 : tls[12];
}

int main(void)
{
    HznHwopus manager;
    const HznHwopusConfig config = { .read_guest = read_guest, .clone_session = clone_session };
    hzn_hwopus_init(&manager, &config);
    uint32_t tls[64] = { 7, 8, 0, 0, HZN_SFCI, 1, 0, 0 };
    HznHwopusReply reply = hzn_hwopus_send_sync(&manager, (uint8_t *)tls, sizeof tls);
    assert(reply.handled && !reply.result && tls[4] == HZN_SFCO && tls[8] == 1);
    const uint32_t clone[] = { 7, 8, 0, 0, HZN_SFCI, 1, 2, 0 };
    memcpy(tls, clone, sizeof clone);
    reply = hzn_hwopus_send_sync(&manager, (uint8_t *)tls, sizeof tls);
    assert(reply.handled && !reply.result && tls[2] == 0x20 && tls[3] == 0x10018);
    assert(work_size(&manager, 0) == 40960);
    hzn_hwopus_close(&manager);
    config_words[2] = 1;
    config_words[3] = 1;
    assert(work_size(&child, 0) == 28672);
    hzn_hwopus_clone(&manager, &child);
    const uint32_t close_object[] = { 6, 8, 0, 0, 2, 1, 0, 0 };
    memcpy(tls, close_object, sizeof close_object);
    reply = hzn_hwopus_send_sync(&manager, (uint8_t *)tls, sizeof tls);
    assert(reply.handled && !reply.result && tls[8] == HZN_SFCO && tls[10] == 0);
    assert(work_size(&child, 0xf601) == 0);
    hzn_hwopus_close(&manager);
    hzn_hwopus_close(&child);
    puts("PASS Opus size queries, move-handle clone, and shared domain close.");
}
