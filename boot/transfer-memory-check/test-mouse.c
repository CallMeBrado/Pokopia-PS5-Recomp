#include "cmif.h"
#include "hid.h"
#include "transfer_memory.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t nmouse_owner = 0x7831d24000;
static const uint64_t jcms_owner = 0x7831d31000;
static const uint64_t aruid = 0x51;
static _Alignas(8) uint8_t nmouse_memory[0xc000], jcms_memory[0x4000];

static bool owner_range(uint64_t address, uint64_t size)
{
    return (address == nmouse_owner && size == sizeof nmouse_memory) ||
           (address == jcms_owner && size == sizeof jcms_memory);
}

static void *get_transfer_memory(void *opaque, uint32_t handle, uint64_t size)
{
    const HznTransferMemory *memory = hzn_transfer_memory_lookup(opaque, handle);
    if (!memory || memory->permission != 1 || size > memory->size)
        return NULL;
    return memory->address == nmouse_owner ? nmouse_memory : jcms_memory;
}

static void put32(uint8_t *bytes, uint32_t value) { memcpy(bytes, &value, sizeof value); }
static void put64(uint8_t *bytes, uint64_t value) { memcpy(bytes, &value, sizeof value); }

static uint8_t *request(uint8_t *tls, uint32_t command, uint32_t words, uint32_t handle)
{
    memset(tls, 0, 0x100);
    put32(tls, 6);
    put32(tls + 4, 0x80000000 | words);
    put32(tls + 8, handle ? 3 : 1);
    if (handle)
        put32(tls + 20, handle);
    put32(tls + 32, HZN_SFCI);
    put32(tls + 36, 1);
    put32(tls + 40, command);
    return tls + 48;
}

static void assert_reply(HznHidReply reply, const uint8_t *tls, uint32_t command, uint32_t result)
{
    assert(reply.handled && !reply.result && reply.command == command);
    assert(hzn_cmif_read32(tls + 16) == HZN_SFCO && hzn_cmif_read32(tls + 24) == result);
}

static void check_rings(const uint8_t *memory, size_t initialized, size_t total,
                        size_t first, size_t stride, size_t count)
{
    for (size_t i = 0; i < total; ++i) {
        const bool buffer_count = i >= first + 8 && i < first + stride * count &&
                                  (i - first) % stride == 8;
        const uint8_t expected = i >= initialized ? 0xa5 : buffer_count ? 17 : 0;
        assert(memory[i] == expected);
    }
}

int main(void)
{
    HznTransferMemoryTable table = {0};
    uint32_t mouse_handle, jcms_handle;
    assert(!hzn_transfer_memory_create(&table, nmouse_owner, sizeof nmouse_memory, 1, owner_range, &mouse_handle));
    assert(!hzn_transfer_memory_create(&table, jcms_owner, sizeof jcms_memory, 1, owner_range, &jcms_handle));
    HznHidState state;
    hzn_hid_init(&state, aruid);
    const HznHidConfig config = {.opaque = &table, .get_transfer_memory = get_transfer_memory};
    memset(nmouse_memory, 0xa5, sizeof nmouse_memory);
    memset(jcms_memory, 0xa5, sizeof jcms_memory);
    uint8_t tls[0x100];
    uint8_t *args = request(tls, 3139, 16, mouse_handle);
    put32(args + 4, 1920);
    put32(args + 8, 1080);
    put32(args + 12, 1);
    put64(args + 16, aruid);
    put64(args + 24, sizeof nmouse_memory + 1);
    assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3139, 0xe401);
    assert(nmouse_memory[0] == 0xa5 && !state.nmouse_initialized);

    args = request(tls, 3139, 16, mouse_handle);
    put32(args + 4, 1920);
    put32(args + 8, 1080);
    put32(args + 12, 1);
    put64(args + 16, aruid);
    put64(args + 24, sizeof nmouse_memory);
    assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3139, 0);
    assert(state.nmouse_initialized && hzn_transfer_memory_lookup(&table, mouse_handle));
    check_rings(nmouse_memory, 0x8e70, sizeof nmouse_memory, 0x220, 0x818, 17);

    args = request(tls, 3110, 12, jcms_handle);
    put64(args, aruid);
    put64(args + 8, sizeof jcms_memory);
    assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3110, 0);
    assert(hzn_transfer_memory_lookup(&table, jcms_handle));
    check_rings(jcms_memory, 16 * 0x2c8, sizeof jcms_memory, 0, 0x2c8, 16);

    args = request(tls, 3138, 12, 0);
    put32(args, 6);
    put64(args + 8, aruid);
    assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3138, 0);
    assert(state.nmouse_sources == 6);
    for (unsigned side = 0; side < 2; ++side) {
        args = request(tls, 3136 + side, 12, 0);
        put64(args, aruid);
        put64(args + 8, 0x503040201 + side);
        assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3136 + side, 0);
        assert(state.nmouse_buttons[side] == 0x503040201 + side);
    }
    args = request(tls, 3131, 10, 0);
    put64(args, aruid);
    assert_reply(hzn_hid_send_sync(&state, HZN_HID_SERVER, &config, tls, sizeof tls), tls, 3131, 0);
    assert(!state.nmouse_initialized && !state.nmouse_sources && !state.nmouse_buttons[0] && !state.nmouse_buttons[1]);
    puts("PASS HID Nmouse and Jcms typed transfer memory, exact ring bytes and mouse configuration");
}
