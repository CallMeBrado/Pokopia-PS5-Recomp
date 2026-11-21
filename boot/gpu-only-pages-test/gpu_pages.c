#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gpu_pages.h"

#include <inttypes.h>
#include <stdatomic.h>

enum { PAGE_BYTES = 0x4000, CAPACITY = 4096 };

typedef struct {
    uint64_t owner, base, end;
    _Atomic uint64_t protected_base, protected_end;
    GpuPagesKind kind;
    uint32_t pool_flags;
    bool shape_eligible, bound, written, tried, cpu_transfer;
} Range;

/* Mutations are serialized by the graphics dispatch lock. The VEH can read
 * the published interval even when a fault interrupts a graphics callback. */
static Range ranges[CAPACITY];
static size_t live, peak, armed_total, restored_alias, restored_release, cpu_calls;
static unsigned bound, written, no_pages, excluded_shape, excluded_overlap, excluded_cpu,
    excluded_outside, failed, missed;
static bool cpu_visible_enabled;

static const char *const kinds[] = {
    "texture", "buffer", "texture-descriptors", "sampler-descriptors", "command", "control", "scratch",
};

static bool intersects(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return a < d && c < b;
}

static Range *lookup(GpuPagesKind kind, uint64_t owner)
{
    for (size_t i = 0; i < CAPACITY; ++i)
        if (ranges[i].owner == owner && ranges[i].kind == kind)
            return &ranges[i];
    return NULL;
}

static bool unprotect(FILE *log, Range *range, const char *reason)
{
    uint64_t base = atomic_load(&range->protected_base);
    uint64_t end = atomic_load(&range->protected_end);
    if (!base)
        return true;
    DWORD old_protect;
    if (!VirtualProtect((void *)(uintptr_t)base, (SIZE_T)(end - base), PAGE_READWRITE, &old_protect)) {
        fprintf(log, "GPU page restore failed base=%#" PRIx64 " error=%lu\n", base, GetLastError());
        ++failed;
        return false;
    }
    atomic_store(&range->protected_base, 0);
    const size_t pages = (size_t)((end - base) / PAGE_BYTES);
    live -= pages;
    fprintf(log, "GPU page restored reason=%s base=%#" PRIx64 " pages=%zu\n", reason, base, pages);
    return true;
}

bool gpu_pages_allow_alias(FILE *log, GpuPagesKind kind, uint64_t base, uint64_t bytes)
{
    if (!base || !bytes || bytes > UINT64_MAX - base)
        return true;
    for (size_t i = 0; i < CAPACITY; ++i) {
        Range *r = &ranges[i];
        const uint64_t first = atomic_load(&r->protected_base), last = atomic_load(&r->protected_end);
        if (!first || !intersects(base, base + bytes, first, last))
            continue;
        fprintf(log, "GPU page new alias kind=%s base=%#" PRIx64 " bytes=%#" PRIx64 "\n",
                kinds[kind], base, bytes);
        if (!unprotect(log, r, "alias"))
            return false;
        restored_alias += (size_t)((last - first) / PAGE_BYTES);
    }
    return true;
}

bool gpu_pages_add(FILE *log, GpuPagesKind kind, uint64_t owner, uint64_t base, uint64_t bytes,
                   uint32_t pool_flags, bool shape_eligible)
{
    if (!owner || !base || !bytes || bytes > UINT64_MAX - base)
        return true;
    Range *available = NULL;
    for (size_t i = 0; i < CAPACITY; ++i) {
        Range *r = &ranges[i];
        if (!r->owner && !available)
            available = r;
        if (r->owner == owner && r->kind == kind && r->base == base && r->end == base + bytes)
            return true;
    }
    if (!gpu_pages_allow_alias(log, kind, base, bytes))
        return false;
    if (!available) {
        ++missed;
        return false;
    }
    available->owner = owner;
    available->base = base;
    available->end = base + bytes;
    available->kind = kind;
    available->pool_flags = pool_flags;
    available->shape_eligible = shape_eligible;
    return true;
}

bool gpu_pages_remove(FILE *log, GpuPagesKind kind, uint64_t owner)
{
    for (size_t i = 0; i < CAPACITY; ++i) {
        Range *r = &ranges[i];
        if (r->owner != owner || r->kind != kind)
            continue;
        const uint64_t base = atomic_load(&r->protected_base);
        if (base) {
            const uint64_t end = atomic_load(&r->protected_end);
            if (!unprotect(log, r, "texture lifetime ended"))
                return false;
            restored_release += (size_t)((end - base) / PAGE_BYTES);
        }
        r->owner = 0;
        r->bound = r->written = r->tried = r->cpu_transfer = false;
    }
    return true;
}

void gpu_pages_bound(FILE *log, uint64_t owner)
{
    Range *r = lookup(GPU_PAGES_TEXTURE, owner);
    if (!r || r->bound)
        return;
    r->bound = true;
    ++bound;
    fprintf(log, "GPU page target binding base=%#" PRIx64 " bytes=%#" PRIx64 " pool_flags=%#x\n",
            r->base, r->end - r->base, r->pool_flags);
}

void gpu_pages_cpu_texture(FILE *log, uint64_t owner, const char *call)
{
    Range *r = lookup(GPU_PAGES_TEXTURE, owner);
    if (!r)
        return;
    r->cpu_transfer = true;
    ++cpu_calls;
    if (atomic_load(&r->protected_base)) {
        fprintf(log, "GPU page CPU texture request call=%s base=%#" PRIx64 "\n", call, r->base);
        fflush(log);
    }
}

bool gpu_pages_written(FILE *log, uint64_t owner, bool full, uint64_t heap_base, uint64_t heap_size,
                       const char *call)
{
    Range *r = lookup(GPU_PAGES_TEXTURE, owner);
    if (!r || !r->bound)
        return true;
    if (!r->written) {
        r->written = true;
        ++written;
    }
    if (!full || r->tried)
        return true;
    r->tried = true;
    const uint64_t base = (r->base + PAGE_BYTES - 1) & ~(uint64_t)(PAGE_BYTES - 1);
    const uint64_t end = r->end & ~(uint64_t)(PAGE_BYTES - 1);
    const char *reason = NULL;
    if (r->base < heap_base || r->end - heap_base > heap_size) {
        ++excluded_outside;
        reason = "outside-guest-heap";
    } else if (!r->shape_eligible) {
        ++excluded_shape;
        reason = "shape";
    } else if (base >= end) {
        ++no_pages;
        reason = "no-full-pages";
    } else if (r->cpu_transfer) {
        ++excluded_cpu;
        reason = "prior-cpu-texel-access";
    }
    for (size_t i = 0; !reason && i < CAPACITY; ++i) {
        const Range *alias = &ranges[i];
        if (alias != r && alias->owner && intersects(base, end, alias->base, alias->end)) {
            ++excluded_overlap;
            reason = kinds[alias->kind];
        }
    }
    if (reason) {
        fprintf(log, "GPU page excluded base=%#" PRIx64 " bytes=%#" PRIx64 " reason=%s\n",
                r->base, r->end - r->base, reason);
        return true;
    }
    for (uint64_t address = base; address < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)(uintptr_t)address, &info, sizeof info) ||
            info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE ||
            (uintptr_t)info.AllocationBase != heap_base) {
            ++excluded_outside;
            fprintf(log, "GPU page excluded base=%#" PRIx64 " bytes=%#" PRIx64 " reason=non-heap-mapping\n",
                    r->base, r->end - r->base);
            return true;
        }
        address = (uintptr_t)info.BaseAddress + info.RegionSize;
    }
    const bool cpu_visible = (r->pool_flags & 7) != 1;
    DWORD old_protect;
    atomic_store(&r->protected_end, end);
    atomic_store(&r->protected_base, base);
    if (!VirtualProtect((void *)(uintptr_t)base, (SIZE_T)(end - base), PAGE_NOACCESS, &old_protect)) {
        atomic_store(&r->protected_base, 0);
        fprintf(log, "GPU page protect failed base=%#" PRIx64 " error=%lu\n", base, GetLastError());
        ++failed;
        return false;
    }
    const size_t pages = (size_t)((end - base) / PAGE_BYTES);
    live += pages;
    if (live > peak)
        peak = live;
    armed_total += pages;
    cpu_visible_enabled |= cpu_visible;
    fprintf(log, "GPU page armed call=%s base=%#" PRIx64 " end=%#" PRIx64
                 " pages=%zu pool_flags=%#x cpu_visible=%d old_prot=%#lx\n",
            call, base, end, pages, r->pool_flags, cpu_visible, old_protect);
    fflush(log);
    return true;
}

void gpu_pages_report(FILE *log, const char *phase)
{
    unsigned live_objects[sizeof kinds / sizeof kinds[0]] = {0};
    unsigned observed_without_full_write = 0;
    for (size_t i = 0; i < CAPACITY; ++i) {
        const Range *r = &ranges[i];
        if (r->owner)
            ++live_objects[r->kind];
        if (r->owner && r->bound && !r->tried)
            ++observed_without_full_write;
    }
    fprintf(log, "GPU page proof [%s] 16KiB live=%zu peak=%zu armed=%zu alias_restored=%zu lifetime_restored=%zu"
                 " bound=%u written=%u no_full_write=%u prior_cpu_calls=%zu cpu_visible_armed=%d"
                 " excluded(no-page=%u shape=%u alias=%u cpu=%u outside=%u) failed=%u missed=%u\n",
            phase, live, peak, armed_total, restored_alias, restored_release, bound, written,
            observed_without_full_write, cpu_calls, cpu_visible_enabled,
            no_pages, excluded_shape, excluded_overlap, excluded_cpu, excluded_outside, failed, missed);
    fputs("GPU page known aliases:", log);
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i)
        fprintf(log, " %s=%u", kinds[i], live_objects[i]);
    fputc('\n', log);
    fflush(log);
}

void gpu_pages_fault(uintptr_t address, uintptr_t operation, uintptr_t rip)
{
    for (size_t i = 0; i < CAPACITY; ++i) {
        const uint64_t base = atomic_load(&ranges[i].protected_base);
        const uint64_t end = atomic_load(&ranges[i].protected_end);
        if (!base || address < base || address >= end)
            continue;
        char line[230];
        const int length = snprintf(line, sizeof line, "GPU page fault address=%#" PRIxPTR " operation=%" PRIuPTR
            " host_rip=%#" PRIxPTR " protected=[%#" PRIx64 ",%#" PRIx64 ")\n",
            address, operation, rip, base, end);
        if (length > 0 && (size_t)length < sizeof line) {
            DWORD written_bytes;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)length, &written_bytes, NULL);
        }
        return;
    }
}
