#include "live_shared.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { HID_BYTES = 0x40000 };
static const uint64_t base = UINT64_C(0x7600200000);
static const uint64_t address = UINT64_C(0x7600203000);

static MEMORY_BASIC_INFORMATION region(const void *address)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(address, &info, sizeof info) == sizeof info);
    return info;
}

static bool accepts_write(void *destination)
{
    const uint8_t value = 0xa7;
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), &value, destination, sizeof value, &count) && count == sizeof value;
}

static void check_collision(uint64_t occupied)
{
    HznLiveShared shared = {0};
    uint8_t *other = VirtualAlloc((void *)(uintptr_t)occupied, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(other == (void *)(uintptr_t)occupied);
    memset(other, 0x36, 0x1000);
    DWORD before, after;
    assert(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (unsigned i = 0; i < 8; ++i) {
        assert(!hzn_live_shared_map(&shared, address, HID_BYTES));
        assert(!shared.address && !shared.size && !shared.writer && !shared.mapped_bytes && !shared.unsafe);
        assert(shared.last_host_error);
        assert(region(other).Protect == PAGE_READWRITE);
        assert(other[0] == 0x36 && other[0xfff] == 0x36);
    }
    assert(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after);
    assert(hzn_live_shared_unmap(&shared));
    assert(VirtualFree(other, 0, MEM_RELEASE));
}

static void check_alias(uint64_t address)
{
    HznLiveShared shared = {0};
    assert(hzn_live_shared_map(&shared, address, HID_BYTES));
    assert(shared.address == address && shared.size == HID_BYTES && shared.writer && !shared.unsafe && !shared.last_host_error);
    uint8_t *guest = (void *)(uintptr_t)address;
    uint8_t *writer = shared.writer;
    assert(guest != writer);
    assert(region(guest).Protect == PAGE_READONLY && region(guest + HID_BYTES - 1).Protect == PAGE_READONLY);
    assert(region(writer).Protect == PAGE_READWRITE && region(writer + HID_BYTES - 1).Protect == PAGE_READWRITE);
    if (address != base)
        assert(region((void *)(uintptr_t)base).Protect == PAGE_NOACCESS);
    for (size_t i = 0; i < HID_BYTES; ++i)
        assert(guest[i] == 0 && writer[i] == 0);

    for (size_t i = 0; i < HID_BYTES; ++i)
        writer[i] = (uint8_t)(i ^ (i >> 5));
    assert(!memcmp(guest, writer, HID_BYTES));
    assert(!accepts_write(guest) && !accepts_write(guest + HID_BYTES - 1));
    assert(accepts_write(writer + 0x1000) && guest[0x1000] == 0xa7);
    assert(!hzn_live_shared_map(&shared, address, HID_BYTES) && shared.writer == writer);

    assert(hzn_live_shared_unmap(&shared));
    assert(hzn_live_shared_unmap(&shared));
    assert(!shared.address && !shared.size && !shared.writer && !shared.mapped_bytes && !shared.unsafe);
    assert(region(guest).State == MEM_FREE && region(guest + HID_BYTES - 1).State == MEM_FREE);
    assert(region(writer).State == MEM_FREE);
}

int main(void)
{
    HznLiveShared shared = {0};
    assert(!hzn_live_shared_map(NULL, address, HID_BYTES));
    assert(!hzn_live_shared_unmap(NULL));
    assert(!hzn_live_shared_map(&shared, 0, HID_BYTES));
    assert(!hzn_live_shared_map(&shared, address + 1, HID_BYTES));
    assert(!hzn_live_shared_map(&shared, address, HID_BYTES + 1));
    assert(!hzn_live_shared_map(&shared, UINT64_MAX & ~UINT64_C(0xfff), HID_BYTES));
    assert(!shared.address && !shared.unsafe && !shared.last_host_error);
    check_collision(base);
    check_collision(base + HID_BYTES);
    check_alias(base);
    check_alias(address);
    check_alias(base + 0xf000);
    puts("PASS live HID section aliases: 0x40000 visibility, read-only guest, collisions and remapping");
    return 0;
}
