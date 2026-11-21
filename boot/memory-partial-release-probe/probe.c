#include "probe.h"

#include <inttypes.h>
#include <stdarg.h>

enum { CPU_READ = 1, CPU_RW = 3, WB_ONION = 0 };
static const DirectOffset system_physical_base = INT64_C(0x2000000000);

typedef struct {
    DirectOffset base;
    bool usable;
} SearchSpace;

static void report(FILE *log, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fputc('\n', log);
    fflush(log);
}

static uint8_t pattern(size_t offset, uint8_t seed)
{
    return (uint8_t)(seed ^ offset ^ (offset >> 6) ^ (offset >> 12));
}

static void fill_slice(uint8_t *address, uint8_t seed)
{
    for (size_t i = 0; i < PROBE_SLICE; ++i)
        address[i] = pattern(i, seed);
}

static bool check_slice(FILE *log, const char *phase, uint8_t *address, uint8_t seed)
{
    void *start = NULL, *end = NULL;
    int32_t protection = 0;
    const int32_t query = sceKernelQueryMemoryProtection(address, &start, &end, &protection);
    const uintptr_t target = (uintptr_t)address;
    if (query || (protection & CPU_READ) == 0 || (uintptr_t)start > target ||
        (uintptr_t)end <= target || (uintptr_t)end - target < PROBE_SLICE) {
        report(log, "%s mapping query=%#" PRIx32 " range=%p..%p prot=%#" PRIx32,
               phase, (uint32_t)query, start, end, (uint32_t)protection);
        return false;
    }
    for (size_t i = 0; i < PROBE_SLICE; ++i) {
        const uint8_t want = pattern(i, seed);
        if (address[i] != want) {
            report(log, "%s guard mismatch at %#zx expected=%#x actual=%#x", phase, i, want, address[i]);
            return false;
        }
    }
    report(log, "%s all %u bytes intact", phase, PROBE_SLICE);
    return true;
}

static int32_t allocate(FILE *log, const char *phase, DirectOffset start, DirectOffset end,
                        size_t bytes, DirectOffset *offset)
{
    *offset = -1;
    const int32_t result = sceKernelAllocateDirectMemory(start, end, bytes, PROBE_SLICE, WB_ONION, offset);
    report(log, "%s search=%#" PRIx64 "..%#" PRIx64 " size=%#zx result=%#" PRIx32 " physical=%#" PRIx64,
           phase, (uint64_t)start, (uint64_t)end, bytes, (uint32_t)result, (uint64_t)*offset);
    return result;
}

static bool unmap(FILE *log, void *address, size_t bytes)
{
    const int32_t result = sceKernelMunmap(address, bytes);
    report(log, "unmap %p size=%#zx result=%#" PRIx32, address, bytes, (uint32_t)result);
    return result == 0;
}

static bool release(FILE *log, DirectOffset offset, size_t bytes)
{
    const int32_t result = sceKernelReleaseDirectMemory(offset, bytes);
    report(log, "release %#" PRIx64 " size=%#zx result=%#" PRIx32,
           (uint64_t)offset, bytes, (uint32_t)result);
    return result == 0;
}

static bool guard_reported_free(FILE *log, const char *name, DirectOffset start, DirectOffset search_base)
{
    DirectOffset offset = -1;
    size_t bytes = 0;
    const DirectOffset search = start - search_base;
    const int32_t result = sceKernelAvailableDirectMemorySize(search, search + PROBE_SLICE, PROBE_SLICE, &offset, &bytes);
    report(log, "%s physical availability result=%#" PRIx32 " start=%#" PRIx64
           " free-address=%#" PRIx64 " free-bytes=%#zx", name, (uint32_t)result,
           (uint64_t)start, (uint64_t)offset, bytes);
    return result == 0 && (offset == start || offset == search) && bytes >= PROBE_SLICE;
}

static bool valid_offset(DirectOffset offset, size_t bytes)
{
    return offset >= 0 && offset % PROBE_SLICE == 0 && bytes <= (uint64_t)(INT64_MAX - offset);
}

static bool discard_unexpected(FILE *log, DirectOffset offset, DirectOffset original)
{
    /* Avoid releasing an overlapping allocation if an unexpected result makes ownership ambiguous. */
    return valid_offset(offset, PROBE_SLICE) &&
           (offset + PROBE_SLICE <= original || offset >= original + PROBE_BYTES) &&
           release(log, offset, PROBE_SLICE);
}

bool run_direct_memory_proof(FILE *log)
{
    const size_t capacity = sceKernelGetDirectMemorySize();
    if (capacity < PROBE_BYTES || capacity > INT64_MAX) {
        report(log, "FAIL invalid direct-memory capacity=%#zx", capacity);
        return false;
    }
    const DirectOffset limit = (DirectOffset)capacity;
    DirectOffset whole, acquired = -1;
    if (allocate(log, "allocate initial", 0, limit, PROBE_BYTES, &whole) || !valid_offset(whole, PROBE_BYTES)) {
        report(log, "FAIL could not allocate the initial block");
        return false;
    }

    const DirectOffset middle = whole + PROBE_SLICE;
    const DirectOffset right = middle + PROBE_SLICE;
    /* Firmware 9.00's system-process allocation returned an absolute offset
     * in this bank despite a low search interval. Exact reacquisition is the
     * proof of the selected search coordinate system. Map/release always use
     * the original returned offsets. */
    SearchSpace spaces[2] = {0};
    size_t space_count = 0;
    if (whole >= system_physical_base && limit >= PROBE_BYTES &&
        whole - system_physical_base <= limit - PROBE_BYTES)
        spaces[space_count++].base = system_physical_base;
    spaces[space_count++].base = 0;
    DirectOffset search_base = 0;
    void *original_view = NULL, *middle_view = NULL;
    bool hole = false, split = false, middle_owned = false, verified = false;
    bool left_free = false, right_free = false;
    int32_t result = sceKernelMapDirectMemory(&original_view, PROBE_BYTES, CPU_RW, 0, whole, PROBE_SLICE);
    report(log, "map original %p result=%#" PRIx32, original_view, (uint32_t)result);
    if (result) {
        original_view = NULL;
        goto cleanup;
    }
    if (!original_view || (uintptr_t)original_view % PROBE_PAGE)
        goto cleanup;
    uint8_t *const left_guard = original_view;
    uint8_t *const right_guard = left_guard + 2 * PROBE_SLICE;
    fill_slice(left_guard, 0x41);
    fill_slice(left_guard + PROBE_SLICE, 0x62);
    fill_slice(right_guard, 0x83);
    if (!check_slice(log, "initial left", left_guard, 0x41) || !check_slice(log, "initial right", right_guard, 0x83))
        goto cleanup;

    size_t usable_spaces = 0;
    for (size_t i = 0; i < space_count; ++i) {
        SearchSpace *space = &spaces[i];
        if (allocate(log, "occupied middle control", middle - space->base, right - space->base,
                     PROBE_SLICE, &acquired)) {
            space->usable = true;
            ++usable_spaces;
        } else if (!discard_unexpected(log, acquired, whole)) {
            report(log, "FAIL occupied interval returned an overlapping or invalid allocation");
            goto cleanup;
        }
    }
    if (!usable_spaces) {
        report(log, "FAIL no exact search coordinate passed the occupied control");
        goto cleanup;
    }
    if (!unmap(log, left_guard + PROBE_SLICE, PROBE_SLICE))
        goto cleanup;
    hole = true;
    if (!check_slice(log, "after middle unmap left", left_guard, 0x41) ||
        !check_slice(log, "after middle unmap right", right_guard, 0x83) ||
        !release(log, middle, PROBE_SLICE))
        goto cleanup;
    split = true;
    if (!check_slice(log, "after middle release left", left_guard, 0x41) ||
        !check_slice(log, "after middle release right", right_guard, 0x83))
        goto cleanup;

    for (size_t i = 0; i < space_count; ++i) {
        const SearchSpace *space = &spaces[i];
        if (!space->usable || allocate(log, "reacquire exact middle", middle - space->base, right - space->base,
                                       PROBE_SLICE, &acquired))
            continue;
        if (acquired == middle) {
            middle_owned = true;
            search_base = space->base;
            break;
        }
        if (!discard_unexpected(log, acquired, whole)) {
            report(log, "FAIL exact search returned an overlapping or invalid allocation");
            goto cleanup;
        }
    }
    if (!middle_owned) {
        report(log, "FAIL neither coordinate system reacquired the exact middle");
        goto cleanup;
    }
    report(log, "exact search physical base=%#" PRIx64, (uint64_t)search_base);
    left_free = guard_reported_free(log, "left guard", whole, search_base);
    right_free = guard_reported_free(log, "right guard", right, search_base);
    if (left_free || right_free) {
        report(log, "FAIL a retained physical guard was reported free");
        goto cleanup;
    }
    result = sceKernelMapDirectMemory(&middle_view, PROBE_SLICE, CPU_RW, 0, middle, PROBE_SLICE);
    report(log, "map reacquired middle %p result=%#" PRIx32, middle_view, (uint32_t)result);
    if (result) {
        middle_view = NULL;
        goto cleanup;
    }
    if (!middle_view || (uintptr_t)middle_view % PROBE_PAGE)
        goto cleanup;
    fill_slice(middle_view, 0xc4);
    verified = check_slice(log, "reacquired middle", middle_view, 0xc4) &&
               check_slice(log, "after middle remap left", left_guard, 0x41) &&
               check_slice(log, "after middle remap right", right_guard, 0x83);

cleanup:;
    const bool middle_unmapped = !middle_view || unmap(log, middle_view, PROBE_SLICE);
    const bool middle_clean = !middle_owned || (middle_unmapped && release(log, middle, PROBE_SLICE));
    bool left_unmapped = true, right_unmapped = true;
    if (original_view && hole) {
        left_unmapped = unmap(log, original_view, PROBE_SLICE);
        right_unmapped = unmap(log, (uint8_t *)original_view + 2 * PROBE_SLICE, PROBE_SLICE);
    } else if (original_view) {
        left_unmapped = right_unmapped = unmap(log, original_view, PROBE_BYTES);
    }
    bool original_clean;
    if (split) {
        const bool left_clean = left_unmapped && (left_free || release(log, whole, PROBE_SLICE));
        const bool right_clean = right_unmapped && (right_free || release(log, right, PROBE_SLICE));
        original_clean = left_clean && right_clean;
    } else {
        original_clean = left_unmapped && right_unmapped && release(log, whole, PROBE_BYTES);
    }
    if (!middle_clean || !original_clean) {
        report(log, "FAIL cleanup incomplete middle=%u original=%u", (unsigned)middle_clean, (unsigned)original_clean);
        return false;
    }
    if (!verified) {
        report(log, "FAIL partial release proof did not finish; owned memory released");
        return false;
    }

    if (allocate(log, "verify exact whole block free", whole - search_base, whole + PROBE_BYTES - search_base,
                 PROBE_BYTES, &acquired)) {
        report(log, "FAIL exact whole block could not be reacquired");
        return false;
    }
    if (!valid_offset(acquired, PROBE_BYTES) || !release(log, acquired, PROBE_BYTES) || acquired != whole) {
        report(log, "FAIL exact whole block cleanup verification failed");
        return false;
    }
    report(log, "PASS middle 64 KiB released and reacquired exactly; guards intact; full 192 KiB returned");
    return true;
}
