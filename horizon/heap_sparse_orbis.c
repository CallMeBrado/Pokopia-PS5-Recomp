/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "heap_sparse.h"

#include <errno.h>
#include <inttypes.h>
#include <orbis/libkernel.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum {
    CHUNK_BYTES = 64 * 1024,
    BLOCK_BYTES = 32 * 1024 * 1024,
    ORBIS_MAP_NO_OVERWRITE = 0x80,
    ORBIS_SIGBUS = 10,
    ORBIS_PAGE_TRAP = 12,
    PAGE_PRESENT = 1,
    PAGE_RESERVED_BIT = 8,
    PAGE_INSTRUCTION = 16
};

typedef struct {
    off_t offset;
    size_t bytes;
} DirectBlock;

/* SetHeapSize defines one logical guest heap for this process. Signal-time
 * bookkeeping is allocated before publishing it and stays outside that heap. */
static HznHeap *owner;
static atomic_bool active;
static atomic_flag heap_lock = ATOMIC_FLAG_INIT;
static uint8_t *mapped;
static DirectBlock *blocks;
static size_t direct_limit, block_count, physical_bytes, block_used, mapped_chunks;
static size_t hole, released_prefix;
static void *clear_alias;
static size_t clear_bytes;
static int32_t allocation_error;
static bool segv_installed, bus_installed;
static struct sigaction old_segv, old_bus;

#ifdef POKOPIA_ORBIS_MEMORY_TRACE
/* The tracer records each physical block allocation. Report the many virtual
 * submaps as a running total so they cannot displace physical events. */
extern int32_t __real_sceKernelMapDirectMemory(void **, size_t, int32_t, int32_t, off_t, size_t);
extern int32_t __real_sceKernelMunmap(void *, size_t);
#define map_chunk_direct __real_sceKernelMapDirectMemory
#define unmap_chunk_direct __real_sceKernelMunmap
#else
#define map_chunk_direct sceKernelMapDirectMemory
#define unmap_chunk_direct sceKernelMunmap
#endif

static void lock_heap(void)
{
    while (atomic_flag_test_and_set_explicit(&heap_lock, memory_order_acquire))
        __builtin_ia32_pause();
}

static void unlock_heap(void)
{
    atomic_flag_clear_explicit(&heap_lock, memory_order_release);
}

static bool allocate_block(void)
{
    off_t physical;
    const size_t remaining = owner->size - physical_bytes;
    size_t bytes = remaining < BLOCK_BYTES ? remaining : BLOCK_BYTES;
    int32_t result;
    for (;;) {
        result = sceKernelAllocateDirectMemory(0, (off_t)direct_limit, bytes,
                                               CHUNK_BYTES, ORBIS_KERNEL_WB_ONION, &physical);
        if (!result)
            break;
        if ((result != (int32_t)ORBIS_KERNEL_ERROR_ENOMEM &&
             result != (int32_t)ORBIS_KERNEL_ERROR_EAGAIN) || bytes == CHUNK_BYTES) {
            allocation_error = result;
            return false;
        }
        bytes = (bytes / 2) & ~(size_t)(CHUNK_BYTES - 1);
    }
    blocks[block_count++] = (DirectBlock){ physical, bytes };
    physical_bytes += bytes;
    block_used = 0;

    /* Initialize the block through a private alias before any guest can
     * touch it. Clearing after publishing a submap can erase a racing write. */
    clear_alias = NULL;
    result = sceKernelMapDirectMemory(&clear_alias, bytes, ORBIS_KERNEL_PROT_CPU_RW,
                                      0, physical, CHUNK_BYTES);
    if (result) {
        clear_alias = NULL;
        allocation_error = result;
        return false;
    }
    clear_bytes = bytes;
    memset(clear_alias, 0, bytes);
    result = sceKernelMunmap(clear_alias, bytes);
    if (result) {
        allocation_error = result;
        return false;
    }
    clear_alias = NULL;
    return true;
}

/* The lock covers the reservation split, physical ownership and publication.
 * A failed replacement leaves one known hole, which teardown skips. */
static bool materialize(size_t index)
{
    if (mapped[index])
        return true;
    if (allocation_error)
        return false;
    if ((!block_count || block_used == blocks[block_count - 1].bytes) && !allocate_block())
        return false;

    const off_t physical = blocks[block_count - 1].offset + (off_t)block_used;
    void *const requested = (void *)(uintptr_t)(owner->base + index * CHUNK_BYTES);
    int32_t result = unmap_chunk_direct(requested, CHUNK_BYTES);
    if (result) {
        allocation_error = result;
        return false;
    }
    void *address = requested;
    result = map_chunk_direct(&address, CHUNK_BYTES, ORBIS_KERNEL_PROT_CPU_RW,
                              MAP_FIXED | ORBIS_MAP_NO_OVERWRITE, physical, CHUNK_BYTES);
    if (result || address != requested) {
        hole = index;
        allocation_error = result ? result : (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
        if (!result) {
            clear_alias = address;
            clear_bytes = CHUNK_BYTES;
        }
        return false;
    }
    mapped[index] = 1;
    ++mapped_chunks;
    block_used += CHUNK_BYTES;
    return true;
}

static uintptr_t writable_mapping_end(uintptr_t address)
{
    OrbisKernelVirtualQueryInfo region;
    if (sceKernelVirtualQuery((const void *)address, 0, &region, sizeof region) ||
        (uintptr_t)region.unk01 > address || address >= (uintptr_t)region.unk02 ||
        !region.isCommitted || !region.isDirectMemory ||
        (region.unk04 & ORBIS_KERNEL_PROT_CPU_RW) != ORBIS_KERNEL_PROT_CPU_RW)
        return 0;
    return (uintptr_t)region.unk02;
}
static bool recover_heap_fault(int signal, const siginfo_t *info, const void *context)
{
    if (!atomic_load_explicit(&active, memory_order_acquire) ||
        (uintptr_t)info < 0x4000 || (uintptr_t)context < 0x4000)
        return false;
    if (signal == SIGSEGV ? (info->si_code != SEGV_MAPERR && info->si_code != SEGV_ACCERR)
                          : info->si_code != ORBIS_PAGE_TRAP)
        return false;
    const uintptr_t address = (uintptr_t)info->si_addr;
    if (address < owner->base || address - owner->base >= owner->size)
        return false;

    /* The kernel delivers the Orbis AMD64 frame, whereas OpenOrbis supplies
     * musl's ucontext definition. Match selectors and CR2 before using its
     * hardware page-fault error. The SDK crash reporter measured these offsets. */
    enum { TRAP = 24, ADDRESS = 25, ERROR = 27, CS = 29, SS = 32 };
    const uint64_t *frame = context;
    if (frame[CS] != 0x43 || frame[SS] != 0x3b || frame[ADDRESS] != address ||
        (uint32_t)frame[TRAP] != ORBIS_PAGE_TRAP ||
        (frame[ERROR] & (PAGE_PRESENT | PAGE_RESERVED_BIT | PAGE_INSTRUCTION)))
        return false;

    const size_t index = (address - owner->base) / CHUNK_BYTES;
    lock_heap();
    bool recovered;
    if (mapped[index]) {
        /* An in-flight fault may have raced a successful map. A deliberately
         * protected stack alias also faults, so require current RW access. */
        recovered = writable_mapping_end(address) != 0;
    } else {
        recovered = materialize(index);
    }
    unlock_heap();
    return recovered;
}

static void heap_fault(int signal, siginfo_t *info, void *context)
{
    const int saved_errno = errno;
    if (recover_heap_fault(signal, info, context)) {
        errno = saved_errno;
        return;
    }
    /* Preserve first touch for other workers if a previous handler can
     * recover this fault. The default action sees the original retrap. */
    const struct sigaction *previous = signal == SIGSEGV ? &old_segv : &old_bus;
    errno = saved_errno;
    if (previous->sa_handler == SIG_DFL || previous->sa_handler == SIG_IGN) {
        const struct sigaction fallback = { .sa_handler = SIG_DFL };
        if (sigaction(signal, &fallback, NULL) != 0)
            _Exit(128 + signal);
    } else if (previous->sa_flags & SA_SIGINFO) {
        previous->sa_sigaction(signal, info, context);
    } else {
        previous->sa_handler(signal);
    }
}

static bool detach_handler(int signal, const struct sigaction *previous, bool *installed)
{
    if (*installed && sigaction(signal, previous, NULL) != 0)
        return false;
    *installed = false;
    return true;
}

bool hzn_heap_sparse_release(HznHeap *heap)
{
    if (owner != heap)
        return !heap->backing_size;

    atomic_store_explicit(&active, false, memory_order_release);
    if (!detach_handler(SIGSEGV, &old_segv, &segv_installed) ||
        !detach_handler(ORBIS_SIGBUS, &old_bus, &bus_installed)) {
        heap->last_host_error = errno;
        return false;
    }
    heap->size = 0;
    const size_t hole_offset = hole * CHUNK_BYTES;
    while (released_prefix < heap->backing_size) {
        if (hole != SIZE_MAX && released_prefix == hole_offset) {
            released_prefix += CHUNK_BYTES;
            continue;
        }
        size_t end = hole != SIZE_MAX && released_prefix < hole_offset ? hole_offset : heap->backing_size;
        int32_t result = sceKernelMunmap((uint8_t *)heap->mapping + released_prefix, end - released_prefix);
        if (result) {
            heap->last_host_error = result;
            return false;
        }
        released_prefix = end;
    }
    if (clear_alias) {
        int32_t result = sceKernelMunmap(clear_alias, clear_bytes);
        if (result) {
            heap->last_host_error = result;
            return false;
        }
        clear_alias = NULL;
    }
    while (block_count) {
        const DirectBlock *block = &blocks[block_count - 1];
        int32_t result = sceKernelReleaseDirectMemory(block->offset, block->bytes);
        if (result) {
            heap->last_host_error = result;
            return false;
        }
        physical_bytes -= block->bytes;
        --block_count;
    }
    free(mapped);
    free(blocks);
    mapped = NULL;
    blocks = NULL;
    owner = NULL;
    heap->mapping = NULL;
    heap->backing_size = 0;
    return true;
}

bool hzn_heap_sparse_reserve(HznHeap *heap, size_t size)
{
    if (owner) {
        heap->last_host_error = (int32_t)ORBIS_KERNEL_ERROR_EBUSY;
        return false;
    }
    direct_limit = sceKernelGetDirectMemorySize();
    if (!direct_limit || direct_limit > INT64_MAX) {
        heap->last_host_error = (int32_t)ORBIS_KERNEL_ERROR_ENOMEM;
        return false;
    }

    /* Reserve without replacement and accept only the guest's fixed address. */
    void *const requested = (void *)(uintptr_t)heap->base;
    void *address = requested;
    int32_t result = sceKernelReserveVirtualRange(&address, size, 0, CHUNK_BYTES);
    if (result) {
        heap->last_host_error = result;
        return false;
    }
    heap->mapping = address;
    heap->backing_size = size;
    owner = heap;
    hole = SIZE_MAX;
    released_prefix = 0;
    mapped_chunks = 0;
    allocation_error = 0;
    const size_t chunks = size / CHUNK_BYTES;
    if (address != requested) {
        result = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
    } else {
        mapped = calloc(chunks, sizeof *mapped);
        blocks = calloc(chunks, sizeof *blocks);
        if (!mapped || !blocks)
            result = (int32_t)ORBIS_KERNEL_ERROR_ENOMEM;
    }
    if (!result) {
        struct sigaction action = {0};
        action.sa_sigaction = heap_fault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        segv_installed = sigaction(SIGSEGV, &action, &old_segv) == 0;
        bus_installed = segv_installed && sigaction(ORBIS_SIGBUS, &action, &old_bus) == 0;
        if (!bus_installed)
            result = errno;
    }
    if (result) {
        if (hzn_heap_sparse_release(heap))
            heap->last_host_error = result;
        return false;
    }
    heap->size = size;
    atomic_store_explicit(&active, true, memory_order_release);
    return true;
}

bool hzn_heap_sparse_prepare_io(void *buffer, size_t bytes)
{
    if (!bytes || !atomic_load_explicit(&active, memory_order_acquire))
        return true;
    const uintptr_t address = (uintptr_t)buffer;
    if (bytes > UINTPTR_MAX - address)
        return false;
    const uintptr_t end = address + bytes;
    const uint64_t heap_end = owner->base + owner->size;
    if (end <= owner->base || address >= heap_end)
        return true;
    if (address < owner->base || end > heap_end)
        return false;
    const size_t offset = address - owner->base;
    const size_t last = (offset + bytes - 1) / CHUNK_BYTES;
    lock_heap();
    bool prepared = true;
    for (size_t index = offset / CHUNK_BYTES; index <= last; ++index) {
        if (!materialize(index)) {
            prepared = false;
            break;
        }
    }
    unlock_heap();
    if (!prepared)
        return false;
    for (uintptr_t at = address; at < end;) {
        at = writable_mapping_end(at);
        if (!at)
            return false;
    }
    return true;
}

void hzn_heap_sparse_report(FILE *log, const char *phase)
{
    if (!atomic_load_explicit(&active, memory_order_acquire))
        return;
    lock_heap();
    const size_t committed = mapped_chunks * CHUNK_BYTES, allocated = physical_bytes, count = block_count;
    const int32_t error = allocation_error;
    unlock_heap();
    fprintf(log, "Heap sparse phase=%s chunk=%d committed=%zu physical=%zu blocks=%zu "
                 "reserved=%" PRIu64 " rc=%#" PRIx32 "\n",
            phase, CHUNK_BYTES, committed, allocated, count, owner->size, (uint32_t)error);
}
