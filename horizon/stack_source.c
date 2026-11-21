/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stack_source.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int32_t unmap_source(HznStackBacking *backing, size_t size)
{
#ifdef _WIN32
    (void)size;
    if (!VirtualFree(backing->address, 0, MEM_RELEASE))
        return (int32_t)GetLastError();
    backing->address = NULL;
    return 0;
#else
    return hzn_orbis_stack_backing_close(backing, size);
#endif
}

static size_t source_reservation_size(uint64_t size)
{
    const uint64_t mask = HZN_STACK_SOURCE_ALIGNMENT - 1;
    return (size_t)((size + mask) & ~mask);
}

static int32_t map_source(HznStackSourceSlot *slot)
{
    void *const requested = (void *)(uintptr_t)slot->range.address;
    const size_t size = source_reservation_size(slot->range.size);
#ifdef _WIN32
    void *mapped = VirtualAlloc(requested, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mapped)
        return (int32_t)GetLastError();
    slot->backing.address = mapped;
    return mapped == requested ? 0 : ERROR_INVALID_ADDRESS;
#else
    return hzn_orbis_stack_backing_open(&slot->backing, requested, size);
#endif
}

static bool overlaps(uint64_t first, uint64_t first_size, uint64_t second, uint64_t second_size)
{
    return first < second + second_size && second < first + first_size;
}

bool hzn_stack_sources_init(HznStackSources *sources, uint64_t base, uint64_t slot_size)
{
    if (!sources || sources->base || !base || base % HZN_STACK_SOURCE_SLOT_ALIGNMENT ||
        !slot_size || slot_size % HZN_STACK_SOURCE_SLOT_ALIGNMENT || base > UINTPTR_MAX ||
        slot_size > (UINTPTR_MAX - base) / HZN_STACK_SOURCE_LIMIT)
        return false;

    *sources = (HznStackSources){.base = base, .slot_size = slot_size};
    return true;
}

const HznStackSourceRange *hzn_stack_source_at(const HznStackSources *sources, size_t slot)
{
    if (!sources || slot >= HZN_STACK_SOURCE_LIMIT || !sources->slots[slot].range.address)
        return NULL;
    return &sources->slots[slot].range;
}

uint32_t hzn_stack_source_prepare(HznStackSources *sources, uint64_t thread_object,
                                 uint64_t original, uint64_t size, uint64_t *address)
{
    if (address)
        *address = 0;
    if (!sources || !sources->base || sources->unsafe || !address)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    sources->last_host_error = 0;
    if (!size || size % HZN_STACK_ALIAS_BYTES || size > sources->slot_size || size > SIZE_MAX)
        return HZN_STACK_MAP_INVALID_SIZE;
    if (!thread_object || !original || original % HZN_STACK_ALIAS_BYTES ||
        original > UINTPTR_MAX || size > UINTPTR_MAX - original)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if (overlaps(original, size, sources->base, sources->slot_size * HZN_STACK_SOURCE_LIMIT))
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;

    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT; ++i) {
        const HznStackSourceRange *range = hzn_stack_source_at(sources, i);
        if (range && (range->thread_object == thread_object ||
                      overlaps(original, size, range->original, range->size)))
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }

    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT; ++i) {
        if (hzn_stack_backing_owned(&sources->slots[i].backing))
            continue;
        HznStackSourceSlot slot = {
            .range = {thread_object, original, sources->base + i * sources->slot_size, size},
        };
        const int32_t result = map_source(&slot);
        if (result == 0) {
            memcpy(slot.backing.address, (const void *)(uintptr_t)original, (size_t)size);
            sources->slots[i] = slot;
            sources->last_host_error = 0;
            ++sources->count;
            *address = slot.range.address;
            return HZN_STACK_MAP_OK;
        }

        const int32_t cleanup = hzn_stack_backing_owned(&slot.backing) ?
            unmap_source(&slot.backing, source_reservation_size(size)) : 0;
        sources->last_host_error = cleanup ? cleanup : result;
        if (cleanup) {
            /* The wrong host address was never published as guest storage. */
            slot.range.address = 0;
            sources->slots[i] = slot;
            sources->unsafe = true;
            break;
        }
    }
    return HZN_STACK_MAP_OUT_OF_MEMORY;
}

static bool alias_active(const HznHeapStackAliases *aliases, const HznStackSourceRange *range)
{
    for (size_t i = 0; i < aliases->count; ++i) {
        const HznHeapStackAliasRange *alias = hzn_heap_stack_alias_at(aliases, i);
        if (alias && overlaps(range->address, range->size, alias->source, alias->size))
            return true;
    }
    return false;
}

static uint32_t release_slot(HznStackSources *sources, const HznHeapStackAliases *aliases, size_t i)
{
    HznStackSourceSlot *slot = &sources->slots[i];
    const HznStackSourceRange *range = &slot->range;
    if (range->address) {
        if (alias_active(aliases, range))
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
        if (slot->backing.address)
            memcpy((void *)(uintptr_t)range->original, slot->backing.address, (size_t)range->size);
    }

    const int32_t result = unmap_source(&slot->backing, source_reservation_size(range->size));
    if (result != 0) {
        sources->last_host_error = result;
        sources->unsafe = true;
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    if (range->address)
        --sources->count;
    *slot = (HznStackSourceSlot){0};
    return HZN_STACK_MAP_OK;
}

uint32_t hzn_stack_source_release(HznStackSources *sources, const HznHeapStackAliases *aliases,
                                 uint64_t thread_object)
{
    if (!sources || !aliases || aliases->unsafe || sources->unsafe || !thread_object)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    sources->last_host_error = 0;
    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT; ++i) {
        const HznStackSourceRange *range = hzn_stack_source_at(sources, i);
        if (range && range->thread_object == thread_object)
            return release_slot(sources, aliases, i);
    }
    return HZN_STACK_MAP_OK;
}

bool hzn_stack_sources_release(HznStackSources *sources, const HznHeapStackAliases *aliases)
{
    if (!sources || !aliases || aliases->unsafe)
        return false;
    sources->last_host_error = 0;
    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT; ++i) {
        if (hzn_stack_backing_owned(&sources->slots[i].backing) &&
            release_slot(sources, aliases, i) != HZN_STACK_MAP_OK)
            return false;
    }
    *sources = (HznStackSources){0};
    return true;
}
