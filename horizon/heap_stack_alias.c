/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap_stack_alias.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
enum { HOST_RESERVE_ALIGNMENT = 0x10000 };
#else
enum { HOST_RESERVE_ALIGNMENT = 0x4000 };
#endif

static int32_t release_destination(HznHeapStackAliasSlot *slot)
{
#ifdef _WIN32
    if (!VirtualFree(slot->backing.address, 0, MEM_RELEASE))
        return (int32_t)GetLastError();
    slot->backing.address = NULL;
    return 0;
#else
    return hzn_orbis_stack_backing_close(&slot->backing, slot->reservation_size);
#endif
}

static int32_t protect_source(HznHeapStackAliasSlot *slot, bool inaccessible)
{
    void *const source = (void *)(uintptr_t)slot->range.source;
    const size_t size = slot->source_protection_size;
#ifdef _WIN32
    DWORD previous;
    const DWORD protection = inaccessible ? PAGE_NOACCESS : PAGE_READWRITE;
    const int32_t result = VirtualProtect(source, size, protection, &previous)
        ? 0 : (int32_t)GetLastError();
#else
    const int protection = inaccessible ? PROT_NONE : PROT_READ | PROT_WRITE;
    const int32_t result = sceKernelMprotect(source, size, protection);
#endif
    if (!result)
        slot->source_protected = inaccessible;
    return result;
}

static int32_t reserve_destination(HznHeapStackAliasSlot *slot, uint64_t base)
{
    void *const requested = (void *)(uintptr_t)base;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwPageSize != HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT ||
        info.dwAllocationGranularity != HOST_RESERVE_ALIGNMENT)
        return ERROR_NOT_SUPPORTED;
    void *memory = VirtualAlloc(requested, slot->reservation_size, MEM_RESERVE, PAGE_NOACCESS);
    if (!memory)
        return (int32_t)GetLastError();
    slot->backing.address = memory;
    if (memory != requested)
        return ERROR_INVALID_ADDRESS;
    void *const target = (void *)(uintptr_t)slot->range.destination;
    void *committed = VirtualAlloc(target, (size_t)slot->range.size, MEM_COMMIT, PAGE_READWRITE);
    if (committed != target)
        return committed ? ERROR_INVALID_ADDRESS : (int32_t)GetLastError();
    return 0;
#else
    return hzn_orbis_stack_backing_open(&slot->backing, requested, slot->reservation_size);
#endif
}

static bool overlaps(uint64_t first, uint64_t first_size, uint64_t second, uint64_t second_size)
{
    return first < second + second_size && second < first + first_size;
}

static uint32_t validate(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size)
{
    if (!aliases || aliases->unsafe)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    aliases->last_host_error = 0;
    if (!dst || !src || dst % HZN_STACK_ALIAS_BYTES || src % HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if (!size || size % HZN_STACK_ALIAS_BYTES || size > SIZE_MAX)
        return HZN_STACK_MAP_INVALID_SIZE;
    if (dst > UINTPTR_MAX || size > UINTPTR_MAX - dst ||
        src > UINTPTR_MAX || size > UINTPTR_MAX - src)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    return HZN_STACK_MAP_OK;
}

static uint32_t map_alias(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size, bool isolated)
{
    const uint32_t checked = validate(aliases, dst, src, size);
    if (checked != HZN_STACK_MAP_OK)
        return checked;
    const uint64_t source_mask = (isolated ? HZN_HEAP_STACK_ALIAS_ISOLATED_ALIGNMENT :
        HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT) - 1;
    if (src & source_mask)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if ((!isolated && (size & source_mask)) || size > SIZE_MAX - source_mask)
        return HZN_STACK_MAP_INVALID_SIZE;
    const uint64_t source_size = (size + source_mask) & ~source_mask;
    if (source_size > UINTPTR_MAX - src)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if (overlaps(dst, size, src, source_size))
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;

    for (size_t i = 0; i < aliases->count; ++i) {
        const HznHeapStackAliasSlot *slot = &aliases->slots[i];
        const HznHeapStackAliasRange *other = &slot->range;
        if (overlaps(src, source_size, other->source, slot->source_protection_size) ||
            overlaps(src, source_size, other->destination, other->size) ||
            overlaps(dst, size, other->source, slot->source_protection_size) ||
            overlaps(dst, size, other->destination, other->size))
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    if (aliases->count == HZN_HEAP_STACK_ALIAS_LIMIT)
        return HZN_STACK_MAP_OUT_OF_MEMORY;

    const uint64_t mask = HOST_RESERVE_ALIGNMENT - 1;
    const uint64_t base = dst & ~mask;
    if (!base || dst + size > UINTPTR_MAX - mask)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    const uint64_t end = (dst + size + mask) & ~mask;
    if (end - base > SIZE_MAX)
        return HZN_STACK_MAP_INVALID_SIZE;
    HznHeapStackAliasSlot slot = {
        .range = {src, dst, size}, .reservation_size = (size_t)(end - base),
        .source_protection_size = (size_t)source_size,
    };
    uint32_t failure = HZN_STACK_MAP_OUT_OF_MEMORY;
    int32_t result = reserve_destination(&slot, base);
    if (!result) {
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)size);
        result = protect_source(&slot, true);
        failure = HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    if (result != 0) {
        const int32_t cleanup = hzn_stack_backing_owned(&slot.backing) ? release_destination(&slot) : 0;
        if (cleanup) {
            aliases->slots[aliases->count++] = slot;
            aliases->unsafe = true;
        }
        aliases->last_host_error = cleanup ? cleanup : result;
        return failure;
    }

    slot.active = true;
    aliases->slots[aliases->count++] = slot;
    return HZN_STACK_MAP_OK;
}

uint32_t hzn_heap_stack_alias_map(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size)
{
    return map_alias(aliases, dst, src, size, false);
}

uint32_t hzn_heap_stack_alias_map_isolated(HznHeapStackAliases *aliases,
                                         uint64_t dst, uint64_t src, uint64_t size)
{
    return map_alias(aliases, dst, src, size, true);
}

/* Restore source access before copyback. If releasing the destination fails,
 * keep the logical alias valid by reapplying the source protection. */
static bool remove_alias(HznHeapStackAliases *aliases, HznHeapStackAliasSlot *slot)
{
    if (slot->source_protected) {
        const int32_t result = protect_source(slot, false);
        if (result != 0) {
            aliases->last_host_error = result;
            return false;
        }
    }
    if (slot->active)
        memcpy((void *)(uintptr_t)slot->range.source, (const void *)(uintptr_t)slot->range.destination,
               (size_t)slot->range.size);

    const int32_t result = release_destination(slot);
    if (result == 0)
        return true;
    if (!slot->backing.address) {
        /* The bytes were copied back and the virtual mapping is gone. Keep
         * ownership of the physical allocation for a teardown retry. */
        slot->active = false;
        aliases->last_host_error = result;
        aliases->unsafe = true;
        return false;
    }
    const int32_t rollback = slot->active ? protect_source(slot, true) : 0;
    aliases->last_host_error = rollback ? rollback : result;
    if (rollback)
        aliases->unsafe = true;
    return false;
}

uint32_t hzn_heap_stack_alias_unmap(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size)
{
    const uint32_t checked = validate(aliases, dst, src, size);
    if (checked != HZN_STACK_MAP_OK)
        return checked;
    for (size_t i = 0; i < aliases->count; ++i) {
        HznHeapStackAliasSlot *slot = &aliases->slots[i];
        const HznHeapStackAliasRange *range = &slot->range;
        if (range->source != src || range->destination != dst || range->size != size)
            continue;
        if (!remove_alias(aliases, slot))
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
        *slot = aliases->slots[--aliases->count];
        aliases->slots[aliases->count] = (HznHeapStackAliasSlot){0};
        return HZN_STACK_MAP_OK;
    }
    return size % HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT ?
        HZN_STACK_MAP_INVALID_SIZE : HZN_STACK_MAP_INVALID_MEMORY_STATE;
}

const HznHeapStackAliasRange *hzn_heap_stack_alias_at(const HznHeapStackAliases *aliases, size_t index)
{
    if (!aliases || index >= aliases->count || !aliases->slots[index].active)
        return NULL;
    return &aliases->slots[index].range;
}

bool hzn_heap_stack_alias_release(HznHeapStackAliases *aliases)
{
    if (!aliases)
        return false;
    while (aliases->count) {
        HznHeapStackAliasSlot *slot = &aliases->slots[aliases->count - 1];
        if (!remove_alias(aliases, slot)) {
            aliases->unsafe = true;
            return false;
        }
        --aliases->count;
    }
    *aliases = (HznHeapStackAliases){0};
    return true;
}
