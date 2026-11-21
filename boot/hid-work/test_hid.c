#include "hid.h"
#include "cmif.h"
#include "live_shared.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned resources, copies;
static const uint32_t ids[] = {0, 0x20, 0x10};

static uint32_t open_resource(void *opaque, uint64_t aruid)
{
    (void)opaque;
    assert(aruid == 0x51);
    ++resources;
    return 0x8123;
}

static uint32_t copy_hid(void *opaque, size_t bytes)
{
    (void)opaque;
    assert(bytes == HZN_HID_MEMORY_BYTES);
    ++copies;
    return 0x9142;
}

static const void *read_guest(void *opaque, uint64_t address, size_t bytes)
{
    (void)opaque;
    return address == 0x1000 && bytes == sizeof ids ? ids : NULL;
}

static void put32(uint8_t *p, uint32_t value) { memcpy(p, &value, sizeof value); }
static void put64(uint8_t *p, uint64_t value) { memcpy(p, &value, sizeof value); }

static void request(uint8_t tls[256], uint32_t type, uint32_t command, bool pid, bool inline_word, bool pointer)
{
    memset(tls, 0xa5, 256);
    uint8_t *raw = tls + (pid ? 32 : 16);
    put32(tls, type | (pointer ? 0x10000 : 0));
    put32(tls + 4, (pid ? (inline_word ? 12u : 10u) : 8u) | (pid ? 0x80000000 : 0));
    if (pid)
        put32(tls + 8, 1);
    if (pointer) {
        put32(tls + 20, sizeof ids << 16);
        put32(tls + 24, 0x1000);
    }
    put32(raw, HZN_SFCI);
    put32(raw + 4, 1);
    put32(raw + 8, command);
    put32(raw + 12, 0x11223344);
    if (pid)
        put64(raw + (inline_word ? 24 : 16), 0x51);
}

static void accepted(HznHidReply reply, uint32_t command)
{
    assert(reply.handled && !reply.result && reply.command == command);
}

int main(void)
{
    HznHidState hid;
    hzn_hid_init(&hid, 0x51);
    const HznHidConfig config = {.open_applet_resource = open_resource, .copy_hid_memory = copy_hid, .read_guest = read_guest};
    uint8_t tls[256];

    request(tls, 6, 0, true, false, false);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 0);
    assert(resources == 1 && !copies && hzn_cmif_read32(tls + 4) == 0x80000008);
    assert(hzn_cmif_read32(tls + 8) == 0x20 && hzn_cmif_read32(tls + 12) == 0x8123);
    assert(hzn_cmif_read32(tls + 16) == HZN_SFCO);

    request(tls, 6, 0, false, false, false);
    accepted(hzn_hid_send_sync(&hid, true, &config, tls, sizeof tls), 0);
    assert(copies == 1 && hzn_cmif_read32(tls + 4) == 0x80000008);
    assert(hzn_cmif_read32(tls + 8) == 2 && hzn_cmif_read32(tls + 12) == 0x9142);
    request(tls, 6, 0, true, false, false);
    put64(tls + 48, 0x52);
    assert(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls).result == 0xe801 && resources == 1);

    request(tls, 7, 3, false, false, false);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 3);
    assert(hzn_cmif_read32(tls + 32) == 0x8000);
    request(tls, 6, 102, true, false, true);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 102);
    assert(hid.ids == 0x301);
    request(tls, 6, 100, true, true, false);
    put32(tls + 48, 1);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 100);
    assert(hid.styles == 1);
    request(tls, 6, 109, true, true, false);
    put32(tls + 48, 2);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 109);
    assert(hid.npad_active);
    request(tls, 6, 11, true, false, false);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 11);
    assert(hid.touch_active);

    HznLiveShared shared = {0};
    assert(hzn_live_shared_map(&shared, UINT64_C(0x7ee2000000), HZN_HID_MEMORY_BYTES));
    assert(shared.writer && (uintptr_t)shared.writer != shared.address);
    assert(hzn_hid_memory_init(shared.writer, shared.size));
    const uint8_t *guest = (const uint8_t *)(uintptr_t)shared.address;
    const uint8_t *entry = guest + 0x9a00, *fullkey = entry + 0x28, *system_ext = entry + 0x1408, *touch = guest + 0x400;
    assert(hzn_cmif_read64(fullkey + 8) == 17 && !hzn_cmif_read64(fullkey + 0x18));
    assert(hzn_cmif_read64(system_ext + 8) == 17 && hzn_cmif_read64(touch + 8) == 17);
    assert(hzn_cmif_read64(guest + 0x9a00 + 9 * 0x5000 + 0x3a80 + 8) == 17);
    assert(hzn_cmif_read32(guest + 0x3e200 + 8) == 1);

    const HznHidPad sample = {.buttons = 1, .lx = -20000, .ry = 22000, .connected = true};
    for (unsigned i = 0; i < 20; ++i)
        hzn_hid_publish(shared.writer, &hid, &sample);
    assert(hzn_cmif_read32(entry) == 1 && hzn_cmif_read32(entry + 0x4188) == 1);
    assert(hzn_cmif_read64(fullkey + 0x10) == 3 && hzn_cmif_read64(fullkey + 0x18) == 16);
    const uint8_t *last = fullkey + 0x20 + 3 * 0x30;
    assert(hzn_cmif_read64(last) == 40 && hzn_cmif_read64(last + 8) == 20);
    assert(hzn_cmif_read64(last + 16) == (1u | (1u << 16) | (1u << 21)));
    assert((int32_t)hzn_cmif_read32(last + 24) == -20000 && hzn_cmif_read32(last + 36) == 22000);
    assert(hzn_cmif_read32(last + 40) == 1);
    assert(!memcmp(system_ext + 0x20 + 3 * 0x30, last, 0x30));
    assert(hzn_cmif_read64(touch + 0x10) == 3 && hzn_cmif_read64(touch + 0x18) == 16);

    request(tls, 6, 104, true, false, false);
    accepted(hzn_hid_send_sync(&hid, false, &config, tls, sizeof tls), 104);
    hzn_hid_publish(shared.writer, &hid, &sample);
    last = fullkey + 0x20 + 4 * 0x30;
    assert(!hzn_cmif_read32(entry) && hzn_cmif_read64(last) == 42 && !hzn_cmif_read64(last + 16));
    hzn_hid_publish(shared.writer, &hid, &sample);
    assert(hzn_cmif_read64(fullkey + 0x10) == 4);
    assert(hzn_live_shared_unmap(&shared));
    puts("PASS exact HID factory/copy handle, activation and IDs, and real shared-alias ring playback");
}
