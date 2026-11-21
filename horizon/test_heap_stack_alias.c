/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"
#include "heap_stack_alias.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint64_t heap_base = UINT64_C(0x7800000000);
static const uint64_t source = UINT64_C(0x780019e000);
static const uint64_t destination = UINT64_C(0x7c61319000);
static const uint64_t size = UINT64_C(0x60000);

static MEMORY_BASIC_INFORMATION region(uint64_t address)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery((void *)(uintptr_t)address, &info, sizeof info) == sizeof info);
    return info;
}

static uint8_t *bytes(uint64_t address)
{
    return (uint8_t *)(uintptr_t)address;
}

static void test_collision_and_validation(void)
{
    HznHeapStackAliases aliases = {0};
    const uint64_t occupied = UINT64_C(0x7c61110000);
    uint8_t *neighbor = VirtualAlloc(bytes(occupied), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(neighbor == bytes(occupied));
    neighbor[0] = 0x33;
    bytes(source)[0] = 0x71;

    assert(hzn_heap_stack_alias_map(&aliases, destination + 1, source, size) == HZN_STACK_MAP_INVALID_ADDRESS);
    assert(hzn_heap_stack_alias_map(&aliases, destination, source + 1, size) == HZN_STACK_MAP_INVALID_ADDRESS);
    assert(hzn_heap_stack_alias_map(&aliases, destination, source, size + 1) == HZN_STACK_MAP_INVALID_SIZE);
    assert(hzn_heap_stack_alias_map(&aliases, destination, source, 0) == HZN_STACK_MAP_INVALID_SIZE);
    assert(hzn_heap_stack_alias_map(&aliases, UINT64_MAX & ~UINT64_C(0xfff), source, size) ==
           HZN_STACK_MAP_INVALID_ADDRESS);
    assert(hzn_heap_stack_alias_map(&aliases, source, source, size) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_heap_stack_alias_unmap(&aliases, destination, source, size) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_heap_stack_alias_map(&aliases, occupied + 0x1000, source, size) == HZN_STACK_MAP_OUT_OF_MEMORY);
    assert(!aliases.count && !aliases.unsafe && aliases.last_host_error);
    assert(neighbor[0] == 0x33 && region(occupied).Protect == PAGE_READWRITE);
    assert(bytes(source)[0] == 0x71 && region(source).Protect == PAGE_READWRITE);
    assert(hzn_heap_stack_alias_release(&aliases));
    assert(VirtualFree(neighbor, 0, MEM_RELEASE));
}

static void test_failed_protect_releases_destination(void)
{
    HznHeapStackAliases aliases = {0};
    const uint64_t first = UINT64_C(0x7702000000);
    const uint64_t second = first + 0x10000;
    uint8_t *a = VirtualAlloc(bytes(first), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *b = VirtualAlloc(bytes(second), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(a == bytes(first) && b == bytes(second));
    a[0xffff] = 0x55;
    b[0] = 0x66;

    /* Readable across the boundary, but Windows cannot protect a range that
     * spans two separate reservations. The provisional alias must be freed. */
    assert(hzn_heap_stack_alias_map(&aliases, destination, first + 0xf000, 0x2000) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(!aliases.count && !aliases.unsafe && aliases.last_host_error);
    assert(region(destination).State == MEM_FREE);
    assert(a[0xffff] == 0x55 && b[0] == 0x66);
    assert(region(first + 0xf000).Protect == PAGE_READWRITE && region(second).Protect == PAGE_READWRITE);
    assert(VirtualFree(b, 0, MEM_RELEASE));
    assert(VirtualFree(a, 0, MEM_RELEASE));
}

static void test_multiple_aliases(void)
{
    HznHeapStackAliases aliases = {0};
    const uint64_t src2 = UINT64_C(0x7800203000);
    const uint64_t dst2 = UINT64_C(0x7c60205000);
    const uint64_t size2 = UINT64_C(0x9000);
    uint8_t *a = bytes(source), *b = bytes(src2);
    uint8_t *alias_a = bytes(destination), *alias_b = bytes(dst2);
    a[-1] = 0x81;
    a[0] = 0x21;
    a[size - 1] = 0x22;
    a[size] = 0x82;
    b[-1] = 0x91;
    b[0] = 0x31;
    b[size2 - 1] = 0x32;
    b[size2] = 0x92;

    assert(hzn_heap_stack_alias_map(&aliases, destination, source, size) == HZN_STACK_MAP_OK);
    assert(hzn_heap_stack_alias_map(&aliases, dst2, src2, size2) == HZN_STACK_MAP_OK);
    assert(aliases.count == 2 && !aliases.unsafe);
    const HznHeapStackAliasRange *first = hzn_heap_stack_alias_at(&aliases, 0);
    const HznHeapStackAliasRange *second = hzn_heap_stack_alias_at(&aliases, 1);
    assert(first && first->source == source && first->destination == destination && first->size == size);
    assert(second && second->source == src2 && second->destination == dst2 && second->size == size2);
    assert(!hzn_heap_stack_alias_at(&aliases, 2));

    assert(region(source).Protect == PAGE_NOACCESS && region(source + size - 1).Protect == PAGE_NOACCESS);
    assert(region(src2).Protect == PAGE_NOACCESS && region(src2 + size2 - 1).Protect == PAGE_NOACCESS);
    assert(region(destination).Protect == PAGE_READWRITE && region(dst2).Protect == PAGE_READWRITE);
    assert(region(destination - 0x1000).State == MEM_RESERVE && region(destination + size).State == MEM_RESERVE);
    assert(alias_a[0] == 0x21 && alias_a[size - 1] == 0x22);
    assert(alias_b[0] == 0x31 && alias_b[size2 - 1] == 0x32);
    assert(a[-1] == 0x81 && a[size] == 0x82 && b[-1] == 0x91 && b[size2] == 0x92);

    const uint64_t other = heap_base + 0x1000;
    assert(hzn_heap_stack_alias_map(&aliases, destination + 0x1000, other, 0x1000) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_heap_stack_alias_map(&aliases, UINT64_C(0x7c63120000), source + 0x1000, 0x1000) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_heap_stack_alias_map(&aliases, destination - 0x1000, other, 0x1000) ==
           HZN_STACK_MAP_OUT_OF_MEMORY);
    assert(hzn_heap_stack_alias_unmap(&aliases, destination, source, size - 0x1000) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);

    alias_a[0] = 0x41;
    alias_a[size - 1] = 0x42;
    alias_b[0] = 0x51;
    alias_b[size2 - 1] = 0x52;
    assert(hzn_heap_stack_alias_unmap(&aliases, destination, source, size) == HZN_STACK_MAP_OK);
    assert(aliases.count == 1 && hzn_heap_stack_alias_at(&aliases, 0)->source == src2);
    assert(region(destination).State == MEM_FREE && region(source).Protect == PAGE_READWRITE);
    assert(region(src2).Protect == PAGE_NOACCESS);
    assert(a[0] == 0x41 && a[size - 1] == 0x42);
    assert(a[-1] == 0x81 && a[size] == 0x82);

    assert(hzn_heap_stack_alias_release(&aliases));
    assert(hzn_heap_stack_alias_release(&aliases));
    assert(!aliases.count && !aliases.unsafe && !aliases.last_host_error);
    assert(region(dst2).State == MEM_FREE && region(src2).Protect == PAGE_READWRITE);
    assert(b[0] == 0x51 && b[size2 - 1] == 0x52);
    assert(b[-1] == 0x91 && b[size2] == 0x92);
    assert(hzn_heap_stack_alias_map(&aliases, destination, source, size) == HZN_STACK_MAP_OK);
    assert(alias_a[0] == 0x41 && alias_a[size - 1] == 0x42);
    assert(hzn_heap_stack_alias_release(&aliases));
}

int main(void)
{
    HznHeap heap = {0};
    uint64_t guest_base;
    assert(hzn_heap_init(&heap, heap_base, 2 * HZN_HEAP_GRANULE));
    assert(hzn_heap_set_size(&heap, 2 * HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == heap_base);

    test_collision_and_validation();
    test_failed_protect_releases_destination();
    test_multiple_aliases();

    assert(hzn_heap_release(&heap));
    puts("PASS heap stack aliases, exact placement, rollback, source guards, copyback and reuse");
    return 0;
}
