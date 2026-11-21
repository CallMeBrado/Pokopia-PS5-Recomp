#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>

#include "probe.h"

enum { PHYSICAL_START = 0x430000, PHYSICAL_LIMIT = 0x30000000 };
typedef enum { NORMAL, REJECT_MIDDLE_RELEASE, CORRUPT_LEFT_AFTER_RELEASE, REJECT_REACQUIRE, FREE_LEFT_AFTER_RELEASE } TestMode;
static TestMode mode;
static unsigned owned, mapped, release_calls, full_allocations;
static bool middle_released;
static uint8_t *memory;
static DirectOffset bank;
static bool absolute_search;

static unsigned mask_at(size_t offset, size_t bytes)
{
    assert(bytes && offset < PROBE_BYTES && bytes <= PROBE_BYTES - offset &&
           offset % PROBE_SLICE == 0 && bytes % PROBE_SLICE == 0);
    return ((1u << (bytes / PROBE_SLICE)) - 1u) << (offset / PROBE_SLICE);
}

size_t sceKernelGetDirectMemorySize(void)
{
    return PHYSICAL_LIMIT;
}

static bool normalize_search(DirectOffset *start, DirectOffset *end)
{
    if (bank && (!absolute_search || (*start == 0 && *end == PHYSICAL_LIMIT))) {
        if (*start < 0 || *end > PHYSICAL_LIMIT)
            return false;
        *start += bank;
        *end += bank;
    }
    return *start >= bank && *end <= bank + PHYSICAL_LIMIT;
}

int32_t sceKernelAllocateDirectMemory(DirectOffset start, DirectOffset end, size_t bytes,
                                     size_t alignment, int32_t type, DirectOffset *offset)
{
    assert(alignment == PROBE_SLICE && !type);
    if (!normalize_search(&start, &end))
        return -1;
    const DirectOffset first = bank + PHYSICAL_START, last = first + PROBE_BYTES;
    if (mode == REJECT_REACQUIRE && middle_released && start == first + PROBE_SLICE)
        return -1;
    for (DirectOffset at = first; at < last; at += PROBE_SLICE) {
        if (at < start || at + (DirectOffset)bytes > end || at + (DirectOffset)bytes > last)
            continue;
        const unsigned mask = mask_at((size_t)(at - first), bytes);
        if (owned & mask)
            continue;
        owned |= mask;
        *offset = at;
        full_allocations += bytes == PROBE_BYTES;
        return 0;
    }
    return -1;
}

int32_t sceKernelReleaseDirectMemory(DirectOffset offset, size_t bytes)
{
    const unsigned mask = mask_at((size_t)(offset - bank - PHYSICAL_START), bytes);
    assert((owned & mask) == mask && !(mapped & mask));
    ++release_calls;
    if (mask == 2 && !middle_released) {
        if (mode == REJECT_MIDDLE_RELEASE)
            return -1;
        if (mode == CORRUPT_LEFT_AFTER_RELEASE)
            memory[0] ^= 1;
        if (mode == FREE_LEFT_AFTER_RELEASE)
            owned &= ~1u;
        middle_released = true;
    }
    owned &= ~mask;
    return 0;
}

int32_t sceKernelAvailableDirectMemorySize(DirectOffset start, DirectOffset end, size_t alignment,
                                          DirectOffset *offset, size_t *bytes)
{
    assert(end - start == PROBE_SLICE && alignment == PROBE_SLICE);
    if (!normalize_search(&start, &end) || (owned & mask_at((size_t)(start - bank - PHYSICAL_START), PROBE_SLICE)))
        return -1;
    *offset = absolute_search ? start : start - bank;
    *bytes = PROBE_SLICE;
    return 0;
}

int32_t sceKernelMapDirectMemory(void **address, size_t bytes, int32_t protection, int32_t flags,
                                DirectOffset offset, size_t alignment)
{
    assert(!*address && protection == 3 && !flags && alignment == PROBE_SLICE);
    const size_t index = (size_t)(offset - bank - PHYSICAL_START);
    const unsigned mask = mask_at(index, bytes);
    assert((owned & mask) == mask && !(mapped & mask));
    mapped |= mask;
    *address = memory + index;
    return 0;
}

int32_t sceKernelMunmap(void *address, size_t bytes)
{
    const unsigned mask = mask_at((size_t)((uint8_t *)address - memory), bytes);
    assert((mapped & mask) == mask);
    mapped &= ~mask;
    return 0;
}

int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, int32_t *protection)
{
    const size_t offset = (size_t)((uint8_t *)address - memory);
    assert(offset < PROBE_BYTES);
    const size_t i = offset / PROBE_SLICE;
    if (!(mapped & (1u << i)))
        return -1;
    size_t first = i, last = i + 1;
    while (first && (mapped & (1u << (first - 1))))
        --first;
    while (last < 3 && (mapped & (1u << last)))
        ++last;
    *start = memory + first * PROBE_SLICE;
    *end = memory + last * PROBE_SLICE;
    *protection = 3;
    return 0;
}

static void check(TestMode next, bool expected, unsigned expected_releases, DirectOffset physical_bank, bool absolute)
{
    printf("Host mock case %u bank=%#llx absolute-search=%u\n", (unsigned)next,
           (unsigned long long)physical_bank, (unsigned)absolute);
    mode = next;
    bank = physical_bank;
    absolute_search = absolute;
    owned = mapped = release_calls = full_allocations = 0;
    middle_released = false;
    assert(run_direct_memory_proof(stdout) == expected);
    assert(!owned && !mapped && release_calls == expected_releases);
    assert(full_allocations == (expected ? 2u : 1u));
}

int main(void)
{
    memory = VirtualAlloc(NULL, PROBE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(memory);
    const DirectOffset high_bank = INT64_C(0x2000000000);
    check(NORMAL, true, 5, 0, false);
    check(NORMAL, true, 5, high_bank, false);
    check(NORMAL, true, 5, high_bank, true);
    check(REJECT_MIDDLE_RELEASE, false, 2, high_bank, false);
    check(CORRUPT_LEFT_AFTER_RELEASE, false, 3, high_bank, false);
    check(REJECT_REACQUIRE, false, 3, high_bank, true);
    check(FREE_LEFT_AFTER_RELEASE, false, 3, high_bank, false);
    assert(VirtualFree(memory, 0, MEM_RELEASE));
    puts("Host mock cases passed with no owned or mapped intervals remaining");
}
