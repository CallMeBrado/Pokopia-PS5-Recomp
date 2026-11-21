/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "time_service.h"
#include "cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t copy_memory(void *opaque)
{
    uint32_t *copies = opaque;
    return 0x40000 + ++*copies;
}

static const uint8_t *active_slot(const uint8_t *page, size_t offset, size_t slot, size_t size)
{
    uint32_t index = hzn_cmif_read32(page + offset);
    assert(index == 1);
    return page + offset + slot + (index & 1) * size;
}

static void check_page(const HznTimeConfig *config)
{
    uint8_t page[HZN_TIME_PAGE_BYTES + 1];
    memset(page, 0xa5, sizeof page);
    assert(!hzn_time_init_page(page, HZN_TIME_PAGE_BYTES - 1, config));
    assert(page[0] == 0xa5 && page[HZN_TIME_PAGE_BYTES - 1] == 0xa5);
    assert(hzn_time_init_page(page, sizeof page, config));
    assert(page[HZN_TIME_PAGE_BYTES - 1] == 0 && page[HZN_TIME_PAGE_BYTES] == 0xa5);

    const uint8_t *steady = active_slot(page, 0x000, 8, 0x18);
    const uint8_t *local = active_slot(page, 0x038, 8, 0x20);
    const uint8_t *network = active_slot(page, 0x080, 8, 0x20);
    const uint8_t *correction = active_slot(page, 0x0c8, 4, 1);
    const uint8_t *adjust = active_slot(page, 0x0d0, 8, 0x38);
    assert(!*correction);
    assert(!memcmp(local, network, 0x20));
    assert(!memcmp(steady + 8, config->source_id, 16));
    assert(!memcmp(local + 16, config->source_id, 16));
    assert(!memcmp(adjust + 40, config->source_id, 16));
    assert(hzn_cmif_read64(adjust) == config->tick_ns);
    assert(!hzn_cmif_read64(adjust + 8) && !hzn_cmif_read64(adjust + 16));
    assert(hzn_cmif_read64(steady) == hzn_cmif_read64(adjust + 24));
    assert(hzn_cmif_read64(adjust + 24) == hzn_cmif_read64(adjust + 32));
    assert(hzn_cmif_read64(local + 8) == config->tick_ns / 1000000000);

    /* The SDK adds the no-slew adjustment to its counter and the context's
     * whole-second offset to the resulting steady timespec. */
    int64_t phase = (int64_t)hzn_cmif_read64(adjust + 24);
    int64_t epoch = (int64_t)hzn_cmif_read64(local);
    assert((int64_t)config->tick_ns + phase + epoch * 1000000000 == config->utc_ns);
}

static void control(uint32_t *tls, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 7;
    tls[1] = 8;
    tls[4] = HZN_SFCI;
    tls[5] = 1;
    tls[6] = command;
}

static void domain_call(uint32_t *tls, uint32_t id, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = id;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = command;
}

static void calendar_call(uint32_t *tls, int64_t timestamp)
{
    domain_call(tls, 2, 101);
    tls[1] = 14;
    tls[4] = 0x180001;
    memcpy(tls + 12, &timestamp, sizeof timestamp);
}

static void expect_calendar(HznTime *time, uint32_t *tls, int64_t timestamp,
                            const uint8_t expected[8], uint32_t weekday, uint32_t yearday)
{
    calendar_call(tls, timestamp);
    assert(hzn_time_send_sync(time, tls, 256).result == 0);
    assert(tls[1] == 20 && tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    assert(!memcmp(tls + 12, expected, 8));
    assert(tls[14] == weekday && tls[15] == yearday);
    assert(!memcmp(tls + 16, "UTC\0\0\0\0", 8));
    assert(tls[18] == 0 && tls[19] == 0);
}

int main(void)
{
    uint32_t copies = 0;
    HznTimeConfig config = { .tick_ns = UINT64_C(123900000000), .utc_ns = INT64_C(1780000000125000000),
                             .source_id = { 0x40, 0x21, 0x7f }, .opaque = &copies,
                             .copy_shared_read = copy_memory };
    check_page(&config);
    config.utc_ns = -1;
    check_page(&config);
    uint8_t page[HZN_TIME_PAGE_BYTES];
    memset(page, 0xa5, sizeof page);
    config.tick_ns = INT64_MAX;
    assert(!hzn_time_init_page(page, sizeof page, &config));
    assert(page[0] == 0xa5);
    config.tick_ns = 0;
    memset(config.source_id, 0, sizeof config.source_id);
    assert(!hzn_time_init_page(page, sizeof page, &config));

    HznTime time;
    uint32_t tls[64];
    assert(hzn_time_init(&time, &config));
    control(tls, 3);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 0x8000);
    control(tls, 0);
    assert(hzn_time_send_sync(&time, tls, 40).result == 0xe801 && !time.domain);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 1);

    domain_call(tls, 1, 3);
    assert(hzn_time_send_sync(&time, tls, 56).result == 0xe801 && time.next_id == 2);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0);
    assert(tls[1] == 13 && tls[4] == 1 && tls[8] == HZN_SFCO && tls[12] == 2);

    domain_call(tls, 1, 20);
    assert(hzn_time_send_sync(&time, tls, 63).result == 0xe801 && copies == 0);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0 && copies == 1);
    assert(tls[1] == 0x8000000c && tls[2] == 2 && tls[3] == 0x40001);
    assert(tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);

    expect_calendar(&time, tls, 0, (uint8_t[]){ 0xb2, 0x07, 1, 1, 0, 0, 0, 0 }, 4, 0);
    expect_calendar(&time, tls, -1, (uint8_t[]){ 0xb1, 0x07, 12, 31, 23, 59, 59, 0 }, 3, 364);
    expect_calendar(&time, tls, 951825600, (uint8_t[]){ 0xd0, 0x07, 2, 29, 12, 0, 0, 0 }, 2, 59);
    expect_calendar(&time, tls, -2203891200, (uint8_t[]){ 0x6c, 0x07, 3, 1, 0, 0, 0, 0 }, 4, 59);
    calendar_call(tls, INT64_MAX);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0 && tls[10] == 0x70e74);
    calendar_call(tls, INT64_MIN);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0 && tls[10] == 0x70e74);

    domain_call(tls, 2, 101);
    uint32_t before[64];
    memcpy(before, tls, sizeof tls);
    HznTimeReply reply = hzn_time_send_sync(&time, tls, sizeof tls);
    assert(reply.kind == HZN_TIME_REPLY && reply.result == 0xe801 && !memcmp(before, tls, sizeof tls));
    calendar_call(tls, 0);
    assert(hzn_time_send_sync(&time, tls, 87).result == 0xe801);

    domain_call(tls, 2, 100);
    memcpy(before, tls, sizeof tls);
    reply = hzn_time_send_sync(&time, tls, sizeof tls);
    assert(reply.kind == HZN_TIME_BOUNDARY && reply.command == 100 && reply.object_id == 2);
    assert(!strcmp(reply.object_type, "ITimeZoneService") && !memcmp(before, tls, sizeof tls));

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = 2;
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0);
    domain_call(tls, 2, 101);
    assert(hzn_time_send_sync(&time, tls, sizeof tls).result == 0 && tls[10] == 0xf601);

    puts("PASS Horizon time:u: sampled clock page, startup domain calls and copied mapping handle");
    return 0;
}
