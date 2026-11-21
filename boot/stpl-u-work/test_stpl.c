#include "stpl.h"
#include "cmif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned opened, copied;
    bool exhausted;
} Allocator;

static uint32_t open_user(void *opaque)
{
    Allocator *allocator = opaque;
    ++allocator->opened;
    return allocator->exhausted ? 0 : 0x8123;
}

static uint32_t copy_hid(void *opaque, size_t bytes)
{
    Allocator *allocator = opaque;
    ++allocator->copied;
    assert(bytes == 0x84000);
    return allocator->exhausted ? 0 : 0x9142;
}

static void request(uint8_t tls[256], uint32_t type, uint32_t command, bool factory)
{
    const uint32_t words[] = {type, factory ? 9u : 8u, 0, 0, HZN_SFCI, 1, command, 0x10203040, 0x12};
    memset(tls, 0xa5, 256);
    memcpy(tls, words, factory ? sizeof words : sizeof words - 4);
}

int main(void)
{
    uint8_t tls[256], untouched[256];
    Allocator allocator = {0};
    const HznStplConfig config = {.opaque = &allocator, .open_user = open_user, .copy_hid_memory = copy_hid};

    request(tls, 6, 0, true);
    HznStplReply reply = hzn_stpl_send_sync(false, &config, tls, sizeof tls);
    assert(reply.handled && !reply.result && reply.command == 0);
    assert(allocator.opened == 1 && !allocator.copied);
    assert(hzn_cmif_read32(tls) == 0 && hzn_cmif_read32(tls + 4) == 0x80000008);
    assert(hzn_cmif_read32(tls + 8) == 0x20 && hzn_cmif_read32(tls + 12) == 0x8123);
    assert(hzn_cmif_read32(tls + 16) == HZN_SFCO && !hzn_cmif_read32(tls + 24));

    request(tls, 6, 300, false);
    reply = hzn_stpl_send_sync(true, &config, tls, sizeof tls);
    assert(reply.handled && !reply.result && reply.command == 300 && allocator.copied == 1);
    assert(hzn_cmif_read32(tls) == 0 && hzn_cmif_read32(tls + 4) == 0x8000000c);
    assert(hzn_cmif_read32(tls + 8) == 2 && hzn_cmif_read32(tls + 12) == 0x9142);
    assert(hzn_cmif_read32(tls + 16) == HZN_SFCO && !hzn_cmif_read32(tls + 24));
    assert(hzn_cmif_read64(tls + 32) == 0x84000 && !hzn_cmif_read64(tls + 40));
    assert(!hzn_cmif_read64(tls + 48) && !hzn_cmif_read64(tls + 56) && tls[64] == 0xa5);

    request(tls, 6, 300, false);
    memcpy(untouched, tls, sizeof tls);
    reply = hzn_stpl_send_sync(true, &config, tls, 63);
    assert(reply.handled && reply.result == 0xe801 && allocator.copied == 1);
    assert(!memcmp(untouched, tls, sizeof tls));

    request(tls, 6, 0, true);
    tls[32] = 0x13;
    memcpy(untouched, tls, sizeof tls);
    reply = hzn_stpl_send_sync(false, &config, tls, sizeof tls);
    assert(reply.handled && reply.result == 0xe801 && allocator.opened == 1);
    assert(!memcmp(untouched, tls, sizeof tls));

    request(tls, 7, 0, false);
    memcpy(untouched, tls, sizeof tls);
    reply = hzn_stpl_send_sync(false, &config, tls, sizeof tls);
    assert(!reply.handled && !memcmp(untouched, tls, sizeof tls));
    request(tls, 6, 2, false);
    memcpy(untouched, tls, sizeof tls);
    reply = hzn_stpl_send_sync(true, &config, tls, sizeof tls);
    assert(!reply.handled && reply.command == 2 && !memcmp(untouched, tls, sizeof tls));

    memset(tls, 0, 8);
    tls[0] = 2;
    assert(hzn_stpl_send_sync(false, &config, tls, sizeof tls).handled && !hzn_cmif_read64(tls));

    request(tls, 6, 1, false);
    const uint32_t h1 = 10, type_mode = 0x100, guests = 1;
    memcpy(tls + 4, &h1, 4);
    memcpy(tls + 32, &type_mode, 4);
    memcpy(tls + 36, &guests, 4);
    reply = hzn_stpl_send_sync(true, &config, tls, sizeof tls);
    assert(reply.handled && !reply.result && reply.command == 1);
    assert(hzn_cmif_read32(tls + 4) == 9 && hzn_cmif_read32(tls + 16) == HZN_SFCO &&
           !hzn_cmif_read32(tls + 24) && hzn_cmif_read32(tls + 32) == 0x4000);

    allocator.exhausted = true;
    request(tls, 6, 300, false);
    reply = hzn_stpl_send_sync(true, &config, tls, sizeof tls);
    assert(reply.handled && !reply.result && allocator.copied == 2);
    assert(hzn_cmif_read32(tls + 4) == 8 && hzn_cmif_read32(tls + 16) == HZN_SFCO &&
           hzn_cmif_read32(tls + 24) == 0xce01);
    puts("PASS stpl:u ordinary factory and HID shared-memory CMIF replies, close, validation, and handle exhaustion");
}

