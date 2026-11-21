/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stack_map.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint64_t source = UINT64_C(0x7500ff0000);
static const uint64_t stack_base = UINT64_C(0x7c00000000);
static const uint64_t stack_size = UINT64_C(0x100000000);

static MEMORY_BASIC_INFORMATION info(uint64_t address)
{
    MEMORY_BASIC_INFORMATION region;
    assert(VirtualQuery((void *)(uintptr_t)address, &region, sizeof region) == sizeof region);
    return region;
}

static void test_initialization_collision(void)
{
    HznStackMap map = {0};
    assert(!hzn_stack_map_init(&map, source + 0x1000, stack_base, stack_size));
    assert(!hzn_stack_map_init(&map, stack_base, stack_base, stack_size));
    unsigned char *host = VirtualAlloc((void *)(uintptr_t)source, 0x4000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(host == (void *)(uintptr_t)source);
    host[0] = 17;
    assert(!hzn_stack_map_init(&map, source, stack_base, stack_size));
    assert(host[0] == 17 && !map.unsafe && !map.source && map.last_host_error);
    assert(hzn_stack_map_release(&map));
    assert(host[0] == 17);
    assert(VirtualFree(host, 0, MEM_RELEASE));
}

static void test_alias_round_trip(void)
{
    HznStackMap map = {0};
    const uint64_t dst = stack_base + 0x15000;
    unsigned char *bytes = (void *)(uintptr_t)source;
    unsigned char *alias = (void *)(uintptr_t)dst;
    assert(hzn_stack_map_init(&map, source, stack_base, stack_size));
    assert(map.source == source && !map.alias && !bytes[0] && !bytes[0xfff]);
    assert(hzn_stack_map_map(&map, dst, source, 0x2000) == HZN_STACK_MAP_INVALID_SIZE);
    assert(hzn_stack_map_map(&map, dst + 8, source, 0x1000) == HZN_STACK_MAP_INVALID_ADDRESS);
    assert(hzn_stack_map_map(&map, dst, source + 0x1000, 0x1000) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_stack_map_map(&map, stack_base + stack_size, source, 0x1000) ==
        HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_stack_map_unmap(&map, dst, source, 0x1000) == HZN_STACK_MAP_INVALID_MEMORY_STATE);

    bytes[0] = 0x31;
    bytes[0xfff] = 0x39;
    bytes[0x1000] = 0x51;
    bytes[0x3fff] = 0x59;
    unsigned char *neighbor = VirtualAlloc((void *)(uintptr_t)(stack_base + 0x10000), 0x1000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(neighbor == (void *)(uintptr_t)(stack_base + 0x10000));
    neighbor[0] = 0x71;
    assert(hzn_stack_map_map(&map, dst, source, 0x1000) == HZN_STACK_MAP_OUT_OF_MEMORY);
    assert(neighbor[0] == 0x71 && bytes[0] == 0x31 && !map.alias && !map.unsafe);
    assert(VirtualFree(neighbor, 0, MEM_RELEASE));

    assert(hzn_stack_map_map(&map, dst, source, 0x1000) == HZN_STACK_MAP_OK);
    assert(map.alias == dst && alias[0] == 0x31 && alias[0xfff] == 0x39);
    assert(info(source).Protect == PAGE_NOACCESS);
    assert(info(source + 0x3fff).Protect == PAGE_NOACCESS);
    assert(info(dst).Protect == PAGE_READWRITE);
    assert(info(dst - 0x1000).State == MEM_RESERVE);
    assert(info(dst + 0x1000).State == MEM_RESERVE);
    assert(hzn_stack_map_map(&map, dst, source, 0x1000) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(hzn_stack_map_unmap(&map, dst + 0x1000, source, 0x1000) ==
        HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(info(source).Protect == PAGE_NOACCESS && map.alias == dst);

    alias[0] = 0x41;
    alias[0xfff] = 0x49;
    assert(hzn_stack_map_unmap(&map, dst, source, 0x1000) == HZN_STACK_MAP_OK);
    assert(!map.alias && !map.unsafe && info(dst).State == MEM_FREE);
    assert(info(source).Protect == PAGE_READWRITE);
    assert(bytes[0] == 0x41 && bytes[0xfff] == 0x49);
    assert(bytes[0x1000] == 0x51 && bytes[0x3fff] == 0x59);

    assert(hzn_stack_map_map(&map, dst, source, 0x1000) == HZN_STACK_MAP_OK);
    assert(hzn_stack_map_release(&map));
    assert(hzn_stack_map_release(&map));
    assert(info(source).State == MEM_FREE && info(dst).State == MEM_FREE);
}

int main(void)
{
    test_initialization_collision();
    test_alias_round_trip();
    puts("Horizon stack map: exact ownership, adjacent data, source protection, copy back, cleanup passed");
    return 0;
}
