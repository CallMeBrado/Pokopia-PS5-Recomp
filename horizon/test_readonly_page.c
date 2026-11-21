/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "readonly_page.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint64_t base = UINT64_C(0x7600200000);
static const uint64_t address = UINT64_C(0x7600203000);

static MEMORY_BASIC_INFORMATION region_info(uint64_t location)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery((const void *)(uintptr_t)location, &info, sizeof info) == sizeof info);
    return info;
}

/* ReadProcessMemory copies into a local destination without temporarily
 * changing its protection, unlike WriteProcessMemory. */
static bool accepts_host_write(void *destination)
{
    const uint8_t value = 0xcc;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), &value, destination, sizeof value, &copied) &&
        copied == sizeof value;
}

static void test_validation_and_collision(void)
{
    HznReadonlyPage page = {0};
    const uint8_t initial[HZN_READONLY_PAGE_BYTES] = {0};
    assert(!hzn_readonly_page_map(NULL, address, initial));
    assert(!hzn_readonly_page_unmap(NULL));
    assert(!hzn_readonly_page_map(&page, address, NULL));
    assert(!hzn_readonly_page_map(&page, 0, initial));
    assert(!hzn_readonly_page_map(&page, address + 1, initial));
    assert(!hzn_readonly_page_map(&page, UINT64_MAX & ~UINT64_C(0xfff), initial));
    assert(!page.address && !page.unsafe && !page.last_host_error);

    uint8_t *neighbor = VirtualAlloc((void *)(uintptr_t)base, HZN_READONLY_PAGE_BYTES,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(neighbor == (void *)(uintptr_t)base);
    neighbor[0] = 0x61;
    assert(!hzn_readonly_page_map(&page, address, initial));
    assert(!page.address && !page.unsafe && page.last_host_error);
    assert(hzn_readonly_page_unmap(&page));
    assert(neighbor[0] == 0x61 && region_info(base).Protect == PAGE_READWRITE);
    assert(VirtualFree(neighbor, 0, MEM_RELEASE));
}

static void test_readonly_lifecycle(void)
{
    HznReadonlyPage page = {0};
    uint8_t initial[HZN_READONLY_PAGE_BYTES];
    for (size_t i = 0; i < sizeof initial; ++i)
        initial[i] = (uint8_t)(i ^ (i >> 5));

    assert(hzn_readonly_page_map(&page, address, initial));
    assert(page.address == address && !page.unsafe && !page.last_host_error);
    MEMORY_BASIC_INFORMATION info = region_info(address);
    assert(info.State == MEM_COMMIT && info.Protect == PAGE_READONLY);
    assert(info.BaseAddress == (void *)(uintptr_t)address);
    assert(info.AllocationBase == (void *)(uintptr_t)base && info.RegionSize == sizeof initial);
    assert(region_info(address - sizeof initial).State == MEM_RESERVE);
    assert(region_info(address + sizeof initial).State == MEM_RESERVE);

    uint8_t *mapped = (void *)(uintptr_t)address;
    assert(memcmp(mapped, initial, sizeof initial) == 0);
    uint8_t writable = 0;
    assert(accepts_host_write(&writable) && writable == 0xcc);
    assert(!accepts_host_write(mapped));
    assert(!accepts_host_write(mapped + sizeof initial - 1));
    assert(memcmp(mapped, initial, sizeof initial) == 0);

    initial[0] = 0x23;
    initial[sizeof initial - 1] = 0x47;
    assert(!hzn_readonly_page_map(&page, address, initial));
    assert(mapped[0] != initial[0] && mapped[sizeof initial - 1] != initial[sizeof initial - 1]);

    assert(hzn_readonly_page_unmap(&page));
    assert(hzn_readonly_page_unmap(&page));
    assert(!page.address && !page.unsafe && !page.last_host_error);
    assert(region_info(base).State == MEM_FREE && region_info(address).State == MEM_FREE);
    assert(hzn_readonly_page_map(&page, address, initial));
    assert(memcmp(mapped, initial, sizeof initial) == 0);
    assert(hzn_readonly_page_unmap(&page));
}

static void test_large_empty_range(void)
{
    HznReadonlyPage page = {0};
    const size_t size = 0x84000;
    assert(!hzn_readonly_span_map(&page, address, 0, NULL));
    assert(!hzn_readonly_span_map(&page, address, size + 1, NULL));
    assert(hzn_readonly_span_map(&page, address, size, NULL));
    const uint8_t *mapped = (const uint8_t *)(uintptr_t)address;
    assert(region_info(address).RegionSize == size);
    for (size_t i = 0; i < size; ++i)
        assert(mapped[i] == 0);
    assert(!accepts_host_write((void *)(uintptr_t)(address + size - 1)));
    assert(hzn_readonly_page_unmap(&page));
    assert(region_info(address + size - 1).State == MEM_FREE);
}

int main(void)
{
    test_validation_and_collision();
    test_readonly_lifecycle();
    test_large_empty_range();
    puts("Horizon read-only page: exact placement, collision safety, copy, write protection and remap passed");
    return 0;
}
