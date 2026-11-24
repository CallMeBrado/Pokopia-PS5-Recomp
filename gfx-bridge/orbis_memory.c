/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(__PROSPERO__)
/* The native process record uses BSD types in the pinned platform headers. */
#undef _XOPEN_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "orbis_memory.h"
#include "../horizon/heap.h"

#include <inttypes.h>
#include <orbis/libkernel.h>

#if defined(__PROSPERO__)
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

uint64_t pokopia_orbis_process_rss_bytes(void)
{
    const pid_t pid = getpid();
    const int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc process;
    size_t available = sizeof process;
    if (sysctl(mib, sizeof mib / sizeof mib[0], &process, &available, NULL, 0) != 0 ||
        available != sizeof process || process.ki_structsize != (int)sizeof process || process.ki_pid != pid ||
        process.ki_rssize <= 0)
        return 0;
    const long page_bytes = sysconf(_SC_PAGESIZE);
    return page_bytes > 0 ? (uint64_t)process.ki_rssize * (uint64_t)page_bytes : 0;
}

#endif

/* The bundled OpenOrbis header declares these output arguments by value.
 * Bind to the same exports using the pointer ABI from orbis-compat. */
extern int32_t orbis_direct_available(off_t start, off_t end, size_t alignment,
    off_t *offset, size_t *size) __asm__("sceKernelAvailableDirectMemorySize");
extern int32_t orbis_flexible_available(size_t *size)
    __asm__("sceKernelAvailableFlexibleMemorySize");

#ifdef POKOPIA_ORBIS_MEMORY_TRACE
#include <pthread.h>
#include <stdatomic.h>

typedef enum { ALLOC, RELEASE, MAP_DIRECT, MAP_FLEXIBLE, MMAP, UNMAP, STACK } MemoryOperation;
typedef struct {
    uint64_t caller, physical, address;
    size_t bytes, alignment;
    int32_t result, argument;
    MemoryOperation operation;
} MemoryEvent;

/* Record without calling libc or allocating memory inside the allocator.
 * The loader drains this queue at phase, frame, and termination boundaries. */
enum { TRACE_CAPACITY = 4096, TRACE_BATCH = 32 };
static MemoryEvent trace[TRACE_CAPACITY];
static size_t trace_start, trace_count, trace_dropped;
static uint64_t trace_number;
static atomic_bool trace_enabled;
static atomic_flag trace_lock = ATOMIC_FLAG_INIT;
static atomic_flag drain_lock = ATOMIC_FLAG_INIT;

static void lock_trace(atomic_flag *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire))
        __builtin_ia32_pause();
}

static void unlock_trace(atomic_flag *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}

static void record_event(MemoryEvent event)
{
    if (!atomic_load_explicit(&trace_enabled, memory_order_relaxed))
        return;
    lock_trace(&trace_lock);
    if (trace_count < TRACE_CAPACITY) {
        trace[(trace_start + trace_count) % TRACE_CAPACITY] = event;
        ++trace_count;
    } else {
        ++trace_dropped;
    }
    unlock_trace(&trace_lock);
}

void pokopia_orbis_memory_trace_start(void)
{
    atomic_store_explicit(&trace_enabled, true, memory_order_relaxed);
}

void pokopia_orbis_memory_trace_flush(FILE *log)
{
    static const char *const operations[] = {
        "alloc", "release", "map-direct", "map-flexible", "mmap", "unmap", "stack"
    };
    lock_trace(&drain_lock);
    for (;;) {
        MemoryEvent batch[TRACE_BATCH];
        lock_trace(&trace_lock);
        size_t count = trace_count < TRACE_BATCH ? trace_count : TRACE_BATCH;
        for (size_t i = 0; i < count; ++i)
            batch[i] = trace[(trace_start + i) % TRACE_CAPACITY];
        trace_start = (trace_start + count) % TRACE_CAPACITY;
        trace_count -= count;
        size_t dropped = trace_dropped;
        trace_dropped = 0;
        unlock_trace(&trace_lock);
        for (size_t i = 0; i < count; ++i) {
            const MemoryEvent *event = &batch[i];
            fprintf(log, "memtrace native seq=%" PRIu64 " op=%s rc=%#08" PRIx32
                " bytes=%zu align=%#zx phys=%#" PRIx64 " va=%#" PRIx64 " arg=%d caller=%#" PRIx64 "\n",
                ++trace_number, operations[event->operation], (uint32_t)event->result, event->bytes,
                event->alignment, event->physical, event->address, event->argument, event->caller);
        }
        if (dropped)
            fprintf(log, "memtrace native dropped=%zu\n", dropped);
        if (count < TRACE_BATCH)
            break;
    }
    unlock_trace(&drain_lock);
}

#define TRACE_CALLER ((uintptr_t)__builtin_return_address(0) - (uintptr_t)pokopia_orbis_memory_census)

int32_t __real_sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t *);
int32_t __real_sceKernelReleaseDirectMemory(off_t, size_t);
int32_t __real_sceKernelMapDirectMemory(void **, size_t, int32_t, int32_t, off_t, size_t);
int32_t __real_sceKernelMapFlexibleMemory(void **, size_t, int32_t, int);
int32_t __real_sceKernelMmap(void *, size_t, int32_t, int32_t, int32_t, off_t, void **);
int32_t __real_sceKernelMunmap(void *, size_t);
int32_t __real_scePthreadCreate(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *, const char *);

int32_t __wrap_sceKernelAllocateDirectMemory(off_t start, off_t end, size_t bytes, size_t alignment,
                                           int32_t memory_type, off_t *physical)
{
    int32_t result = __real_sceKernelAllocateDirectMemory(start, end, bytes, alignment, memory_type, physical);
    record_event((MemoryEvent){ .operation = ALLOC, .caller = TRACE_CALLER, .bytes = bytes, .alignment = alignment,
        .result = result, .argument = memory_type, .physical = result == 0 && physical ? (uint64_t)*physical : 0 });
    return result;
}

int32_t __wrap_sceKernelReleaseDirectMemory(off_t physical, size_t bytes)
{
    int32_t result = __real_sceKernelReleaseDirectMemory(physical, bytes);
    record_event((MemoryEvent){ .operation = RELEASE, .caller = TRACE_CALLER, .bytes = bytes,
        .result = result, .physical = (uint64_t)physical });
    return result;
}

int32_t __wrap_sceKernelMapDirectMemory(void **address, size_t bytes, int32_t protection, int32_t flags,
                                       off_t physical, size_t alignment)
{
    int32_t result = __real_sceKernelMapDirectMemory(address, bytes, protection, flags, physical, alignment);
    record_event((MemoryEvent){ .operation = MAP_DIRECT, .caller = TRACE_CALLER, .bytes = bytes, .alignment = alignment,
        .result = result, .argument = flags, .physical = (uint64_t)physical,
        .address = result == 0 && address ? (uintptr_t)*address : 0 });
    return result;
}

int32_t __wrap_sceKernelMapFlexibleMemory(void **address, size_t bytes, int32_t protection, int flags)
{
    int32_t result = __real_sceKernelMapFlexibleMemory(address, bytes, protection, flags);
    record_event((MemoryEvent){ .operation = MAP_FLEXIBLE, .caller = TRACE_CALLER, .bytes = bytes,
        .result = result, .argument = flags, .address = result == 0 && address ? (uintptr_t)*address : 0 });
    return result;
}

int32_t __wrap_sceKernelMmap(void *address, size_t bytes, int32_t protection, int32_t flags,
                           int32_t fd, off_t offset, void **mapped)
{
    int32_t result = __real_sceKernelMmap(address, bytes, protection, flags, fd, offset, mapped);
    record_event((MemoryEvent){ .operation = MMAP, .caller = TRACE_CALLER, .bytes = bytes,
        .result = result, .argument = flags, .address = result == 0 && mapped ? (uintptr_t)*mapped : 0 });
    return result;
}

int32_t __wrap_sceKernelMunmap(void *address, size_t bytes)
{
    int32_t result = __real_sceKernelMunmap(address, bytes);
    record_event((MemoryEvent){ .operation = UNMAP, .caller = TRACE_CALLER, .bytes = bytes,
        .result = result, .address = (uintptr_t)address });
    return result;
}

int32_t __wrap_scePthreadCreate(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*entry)(void *), void *arg, const char *name)
{
    size_t bytes = 0;
    void *stack = NULL;
    if (attributes && pthread_attr_getstack(attributes, &stack, &bytes) != 0) {
        stack = NULL;
        pthread_attr_getstacksize(attributes, &bytes);
    }
    int32_t result = __real_scePthreadCreate(thread, attributes, entry, arg, name);
    record_event((MemoryEvent){ .operation = STACK, .caller = TRACE_CALLER, .bytes = bytes,
        .result = result, .address = (uintptr_t)stack,
        .physical = (uintptr_t)entry - (uintptr_t)pokopia_orbis_memory_census });
    return result;
}

static void report_free_extents(FILE *log, size_t direct_total)
{
    enum { NATIVE_PAGE = 0x4000, MAX_SEARCH = 4096 };
    struct SearchRange { off_t start, end; } pending[64] = {{0, (off_t)direct_total}};
    size_t remaining = 1, total_free = 0, largest = 0, extents = 0;
    unsigned queries = 0;
    int32_t result = 0;
    while (remaining && queries < MAX_SEARCH) {
        const struct SearchRange range = pending[--remaining];
        off_t offset = 0;
        size_t bytes = 0;
        result = orbis_direct_available(range.start, range.end, NATIVE_PAGE, &offset, &bytes);
        ++queries;
        if (result == (int32_t)ORBIS_KERNEL_ERROR_ENOMEM) {
            result = 0;
            continue;
        }
        if (result || offset < range.start || offset >= range.end ||
            bytes < NATIVE_PAGE || bytes > (uint64_t)(range.end - offset) ||
            ((uint64_t)offset | bytes) % NATIVE_PAGE) {
            if (!result)
                result = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
            break;
        }
        total_free += bytes;
        if (largest < bytes)
            largest = bytes;
        ++extents;
        /* Search both sides. The kernel may return a later fit within the range. */
        if (remaining + 2 > sizeof pending / sizeof pending[0]) {
            result = (int32_t)ORBIS_KERNEL_ERROR_ENOMEM;
            break;
        }
        if (offset - range.start >= NATIVE_PAGE)
            pending[remaining++] = (struct SearchRange){ range.start, offset };
        off_t end = offset + (off_t)bytes;
        if (range.end - end >= NATIVE_PAGE)
            pending[remaining++] = (struct SearchRange){ end, range.end };
    }
    fprintf(log, "memtrace free direct_total=%zu free_bytes=%zu largest=%zu extents=%zu queries=%u complete=%u rc=%#" PRIx32 "\n",
        direct_total, total_free, largest, extents, queries, (unsigned)(!remaining && !result), (uint32_t)result);
}
#endif

void pokopia_orbis_memory_census(FILE *log, const char *phase)
{
#ifdef POKOPIA_ORBIS_MEMORY_TRACE
    pokopia_orbis_memory_trace_flush(log);
#endif
    const size_t direct_total = sceKernelGetDirectMemorySize();
    off_t direct_offset = 0;
    size_t direct_span = 0;
    size_t flexible_available = 0;

    /* The guest heap and RADV arena both require 2 MiB-aligned direct memory. */
    const int32_t direct_result = orbis_direct_available(0, (off_t)direct_total,
        HZN_HEAP_GRANULE, &direct_offset, &direct_span);
    const int32_t flexible_result = orbis_flexible_available(&flexible_available);

    fprintf(log, "Orbis memory [%s] direct total=%zu bytes alignment=0x%x "
        "query=0x%08" PRIx32, phase, direct_total, HZN_HEAP_GRANULE,
        (uint32_t)direct_result);
    if (direct_result == 0)
        fprintf(log, " span=%zu bytes offset=0x%" PRIx64, direct_span, (uint64_t)direct_offset);
    fprintf(log, "; flexible query=0x%08" PRIx32, (uint32_t)flexible_result);
    if (flexible_result == 0)
        fprintf(log, " available=%zu bytes", flexible_available);
    fputc('\n', log);
#ifdef POKOPIA_ORBIS_MEMORY_TRACE
    report_free_extents(log, direct_total);
#endif
    fflush(log);
}
