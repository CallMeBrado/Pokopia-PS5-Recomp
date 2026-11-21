/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stack_map.h"

#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <orbis/libkernel.h>
#include <sys/mman.h>
#endif

enum { SOURCE_HOST_BYTES = 0x4000, SOURCE_ALIGNMENT = 0x10000 };
#ifdef _WIN32
enum { ALIAS_RESERVE_BYTES = 0x10000 };
#else
enum { ALIAS_RESERVE_BYTES = 0x4000, ORBIS_MAP_ANONYMOUS = 0x1000 };
#endif

static int32_t release_host(void *base, size_t bytes)
{
#ifdef _WIN32
    (void)bytes;
    return VirtualFree(base, 0, MEM_RELEASE) ? 0 : (int32_t)GetLastError();
#else
    return sceKernelMunmap(base, bytes);
#endif
}

static int32_t protect_source(void *base, bool accessible)
{
#ifdef _WIN32
    DWORD previous;
    DWORD protection = accessible ? PAGE_READWRITE : PAGE_NOACCESS;
    return VirtualProtect(base, SOURCE_HOST_BYTES, protection, &previous)
        ? 0 : (int32_t)GetLastError();
#else
    int protection = accessible ? PROT_READ | PROT_WRITE : PROT_NONE;
    return sceKernelMprotect(base, SOURCE_HOST_BYTES, protection);
#endif
}

static bool allocate_exact(HznStackMap *map, uint64_t base, size_t reserve_bytes,
                           size_t offset, size_t commit_bytes, void **reservation)
{
    void *const wanted = (void *)(uintptr_t)base;
#ifdef _WIN32
    void *memory = VirtualAlloc(wanted, reserve_bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!memory) {
        map->last_host_error = (int32_t)GetLastError();
        return false;
    }
    const int32_t wrong_address = ERROR_INVALID_ADDRESS;
#else
    (void)offset;
    (void)commit_bytes;
    /* The SDK's musl header defines MAP_ANON using the Linux constant.
     * Orbis passes FreeBSD's 0x1000 to libkernel mmap. */
    void *memory = mmap(wanted, reserve_bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | ORBIS_MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        map->last_host_error = errno;
        return false;
    }
    const int32_t wrong_address = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
#endif

    *reservation = memory;
    int32_t result = memory == wanted ? 0 : wrong_address;
#ifdef _WIN32
    if (result == 0) {
        void *const target = (uint8_t *)memory + offset;
        void *committed = VirtualAlloc(target, commit_bytes, MEM_COMMIT, PAGE_READWRITE);
        if (committed != target)
            result = committed ? wrong_address : (int32_t)GetLastError();
    }
#endif
    if (result == 0)
        return true;

    const int32_t cleanup = release_host(memory, reserve_bytes);
    if (cleanup == 0)
        *reservation = NULL;
    else
        map->unsafe = true;
    map->last_host_error = cleanup ? cleanup : result;
    return false;
}

bool hzn_stack_map_init(HznStackMap *map, uint64_t source, uint64_t stack_base, uint64_t stack_size)
{
    if (!map || !source || source % SOURCE_ALIGNMENT ||
        source > UINTPTR_MAX - SOURCE_HOST_BYTES ||
        !stack_base || stack_base % HZN_STACK_ALIAS_BYTES ||
        stack_size < HZN_STACK_ALIAS_BYTES || stack_size % HZN_STACK_ALIAS_BYTES ||
        stack_base > UINTPTR_MAX || stack_size > UINTPTR_MAX - stack_base)
        return false;
    if (source < stack_base + stack_size && stack_base < source + SOURCE_HOST_BYTES)
        return false;

    *map = (HznStackMap){ .stack_base = stack_base, .stack_size = stack_size };
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwPageSize != HZN_STACK_ALIAS_BYTES ||
        info.dwAllocationGranularity != ALIAS_RESERVE_BYTES)
        return false;
#endif
    if (!allocate_exact(map, source, SOURCE_HOST_BYTES, 0, SOURCE_HOST_BYTES, &map->source_host))
        return false;
    map->source = source;
    return true;
}

static uint32_t validate(const HznStackMap *map, uint64_t dst, uint64_t src, uint64_t size)
{
    if ((dst | src) % HZN_STACK_ALIAS_BYTES)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if (size != HZN_STACK_ALIAS_BYTES)
        return HZN_STACK_MAP_INVALID_SIZE;
    if (!map || map->unsafe || !map->source_host || src != map->source ||
        dst < map->stack_base || dst - map->stack_base > map->stack_size - size)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    return HZN_STACK_MAP_OK;
}

uint32_t hzn_stack_map_map(HznStackMap *map, uint64_t dst, uint64_t src, uint64_t size)
{
    if (map && !map->unsafe)
        map->last_host_error = 0;
    const uint32_t check = validate(map, dst, src, size);
    if (check != HZN_STACK_MAP_OK)
        return check;
    if (map->alias_host)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;

    const uint64_t base = dst & ~(uint64_t)(ALIAS_RESERVE_BYTES - 1);
    if (base < map->stack_base || map->stack_size < ALIAS_RESERVE_BYTES ||
        base - map->stack_base > map->stack_size - ALIAS_RESERVE_BYTES)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    if (!allocate_exact(map, base, ALIAS_RESERVE_BYTES, (size_t)(dst - base),
            HZN_STACK_ALIAS_BYTES, &map->alias_host))
        return HZN_STACK_MAP_OUT_OF_MEMORY;

    memcpy((void *)(uintptr_t)dst, map->source_host, HZN_STACK_ALIAS_BYTES);
    const int32_t result = protect_source(map->source_host, false);
    if (result != 0) {
        const int32_t cleanup = release_host(map->alias_host, ALIAS_RESERVE_BYTES);
        if (cleanup == 0)
            map->alias_host = NULL;
        else
            map->unsafe = true;
        map->last_host_error = cleanup ? cleanup : result;
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    map->alias = dst;
    return HZN_STACK_MAP_OK;
}

uint32_t hzn_stack_map_unmap(HznStackMap *map, uint64_t dst, uint64_t src, uint64_t size)
{
    if (map && !map->unsafe)
        map->last_host_error = 0;
    const uint32_t check = validate(map, dst, src, size);
    if (check != HZN_STACK_MAP_OK)
        return check;
    if (!map->alias_host || map->alias != dst)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;

    int32_t result = protect_source(map->source_host, true);
    map->last_host_error = result;
    if (result != 0)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    memcpy(map->source_host, (const void *)(uintptr_t)dst, HZN_STACK_ALIAS_BYTES);
    result = release_host(map->alias_host, ALIAS_RESERVE_BYTES);
    if (result != 0) {
        const int32_t rollback = protect_source(map->source_host, false);
        map->last_host_error = rollback ? rollback : result;
        map->unsafe = rollback != 0;
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }

    map->alias_host = NULL;
    map->alias = 0;
    return HZN_STACK_MAP_OK;
}

bool hzn_stack_map_release(HznStackMap *map)
{
    if (!map)
        return false;
    if (map->alias_host) {
        map->last_host_error = release_host(map->alias_host, ALIAS_RESERVE_BYTES);
        if (map->last_host_error != 0)
            return false;
        map->alias_host = NULL;
        map->alias = 0;
    }
    if (map->source_host) {
        map->last_host_error = release_host(map->source_host, SOURCE_HOST_BYTES);
        if (map->last_host_error != 0) {
            map->unsafe = true;
            return false;
        }
    }
    *map = (HznStackMap){0};
    return true;
}

