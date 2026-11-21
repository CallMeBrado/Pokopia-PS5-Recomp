/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "heap_sparse.h"
#include "../cold-page-spill-check/cold_store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <orbis/libkernel.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    CHUNK_BYTES = 64 * 1024,
    BLOCK_BYTES = 32 * 1024 * 1024,
    ORBIS_MAP_NO_OVERWRITE = 0x80,
    ORBIS_SIGBUS = 10,
    ORBIS_PAGE_TRAP = 12,
    PAGE_PRESENT = 1,
    PAGE_WRITE = 2,
    PAGE_RESERVED_BIT = 8,
    PAGE_INSTRUCTION = 16
};

enum { DIRECT_CHUNKS = BLOCK_BYTES / CHUNK_BYTES, BITS_PER_WORD = 64 };
/* CHUNK_COLD: contents live only in the cold file. Its virtual range is a bare
 * reservation and its physical slice is released. A fault reads it back into
 * fresh direct memory. Parked pages are never file-mapped: R40-R42 hard-locked
 * the console after ~13k private 64 KiB file mappings (~860 MiB). */
enum { CHUNK_EMPTY, CHUNK_DIRECT, CHUNK_COLD };

typedef struct {
    off_t offset;
    size_t bytes;
    uint64_t released[DIRECT_CHUNKS / BITS_PER_WORD];
} DirectBlock;

typedef struct {
    uint32_t block, protection_epoch;
    uint32_t slot; /* cold file slot + 1, assigned on first park and reused */
    uint16_t slice;
    uint8_t state;
} GuestChunk;

_Static_assert(CHUNK_BYTES == (int)HZN_COLD_PAGE_BYTES, "cold slots match sparse chunks");

/* SetHeapSize defines one logical guest heap for this process. Signal-time
 * bookkeeping is allocated before publishing it and stays outside that heap. */
static HznHeap *owner;
static atomic_bool active;
static atomic_flag heap_lock = ATOMIC_FLAG_INIT;
static GuestChunk *chunks;
static HznColdStore cold_store;
static char cold_path[96];
static size_t cold_chunks, cold_released, cold_failures, cold_restored, cold_next_slot;
static int32_t cold_rollback_error, cold_reserve_error;

/* R57: parking copies a chunk here under the locks and leaves the slow disk
 * write to hzn_heap_sparse_cold_flush(), which runs with no lock held. Until a
 * staged chunk is written, restore_cold() reads it from here, not the file.
 * Only the parking thread adds entries or flushes, so entries are stable while
 * flush writes them; restore_cold() only reads them, under the heap lock. When
 * staging is full, parking falls back to writing the file directly. */
enum { COLD_STAGING_SLOTS = 32 };
static uint8_t cold_staging[COLD_STAGING_SLOTS][CHUNK_BYTES] __attribute__((aligned(64)));
static size_t cold_staged_slot[COLD_STAGING_SLOTS];
static size_t cold_staged_count, cold_staged_total, cold_direct_writes;

static int staged_index(size_t slot)
{
    for (size_t i = 0; i < cold_staged_count; ++i)
        if (cold_staged_slot[i] == slot)
            return (int)i;
    return -1;
}
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
    blocks[block_count++] = (DirectBlock){ .offset = physical, .bytes = bytes };
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

static void failed_park_rollback(int32_t result);

static bool next_slice(off_t *physical)
{
    if ((!block_count || block_used == blocks[block_count - 1].bytes) && !allocate_block())
        return false;
    *physical = blocks[block_count - 1].offset + (off_t)block_used;
    return true;
}

/* The lock covers the reservation split, physical ownership and publication.
 * A failed replacement leaves one known hole, which teardown skips. */
static bool publish_chunk(size_t index, off_t physical)
{
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
    GuestChunk *chunk = &chunks[index];
    chunk->block = (uint32_t)(block_count - 1);
    chunk->slice = (uint16_t)(block_used / CHUNK_BYTES);
    chunk->state = CHUNK_DIRECT;
    ++mapped_chunks;
    block_used += CHUNK_BYTES;
    return true;
}

static bool materialize(size_t index)
{
    if (chunks[index].state != CHUNK_EMPTY)
        return true;
    off_t physical;
    return !allocation_error && next_slice(&physical) && publish_chunk(index, physical);
}

/* Fill a fresh slice through a private alias, then publish it, so no other
 * guest thread can observe the chunk before its saved contents arrive. */
static bool restore_cold(size_t index)
{
    GuestChunk *chunk = &chunks[index];
    off_t physical;
    if (allocation_error || !chunk->slot || !next_slice(&physical))
        return false;
    void *alias = NULL;
    int32_t result = map_chunk_direct(&alias, CHUNK_BYTES, ORBIS_KERNEL_PROT_CPU_RW, 0, physical, CHUNK_BYTES);
    if (result) {
        failed_park_rollback(result);
        return false;
    }
    const int staged = staged_index(chunk->slot - 1);
    bool loaded = true;
    if (staged >= 0)
        memcpy(alias, cold_staging[staged], CHUNK_BYTES); /* not yet on disk */
    else
        loaded = hzn_cold_store_load(&cold_store, chunk->slot - 1, alias);
    result = unmap_chunk_direct(alias, CHUNK_BYTES);
    if (result) {
        clear_alias = alias;
        clear_bytes = CHUNK_BYTES;
        failed_park_rollback(result);
        return false;
    }
    if (!loaded) {
        failed_park_rollback((int32_t)ORBIS_KERNEL_ERROR_EFAULT);
        return false;
    }
    if (!publish_chunk(index, physical)) {
        cold_rollback_error = allocation_error;
        return false;
    }
    --cold_chunks;
    ++cold_restored;
    return true;
}

static bool make_resident(size_t index)
{
    return chunks[index].state == CHUNK_COLD ? restore_cold(index) : materialize(index);
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

static bool slice_released(const DirectBlock *block, size_t slice)
{
    return (block->released[slice / BITS_PER_WORD] & (UINT64_C(1) << (slice % BITS_PER_WORD))) != 0;
}

static int32_t release_slices(DirectBlock *block, size_t first, size_t count)
{
    const off_t physical = block->offset + (off_t)(first * CHUNK_BYTES);
    const size_t bytes = count * CHUNK_BYTES;
    const int32_t result = sceKernelReleaseDirectMemory(physical, bytes);
    if (!result) {
        for (size_t slice = first; slice < first + count; ++slice)
            block->released[slice / BITS_PER_WORD] |= UINT64_C(1) << (slice % BITS_PER_WORD);
        physical_bytes -= bytes;
    }
    return result;
}

bool hzn_heap_sparse_cold_open(const char *path)
{
    if (!owner || cold_store.slots || !path || strlen(path) >= sizeof cold_path)
        return false;
    const int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return false;
    /* Keep a named vnode while mapping it. Reuse a stale file only after
     * acquiring an exclusive lock, including after a forced app close. */
    if (flock(fd, LOCK_EX | LOCK_NB) || ftruncate(fd, 0)) {
        close(fd);
        return false;
    }
    memcpy(cold_path, path, strlen(path) + 1);
    cold_store = (HznColdStore){.file = fd, .slots = owner->size / CHUNK_BYTES};
    return true;
}

int32_t hzn_heap_sparse_cold_error(void)
{
    lock_heap();
    const int32_t result = cold_rollback_error;
    unlock_heap();
    return result;
}

static bool close_cold_file(void)
{
    if (!cold_path[0])
        return true;
    if (unlink(cold_path) && errno != ENOENT)
        return false;
    cold_path[0] = 0;
    return hzn_cold_store_close(&cold_store);
}

static void failed_park_rollback(int32_t result)
{
    cold_rollback_error = allocation_error = result;
}

static void restore_parked_direct(size_t index, off_t physical, void *address, bool unmap_file)
{
    if (unmap_file) {
        const int32_t result = unmap_chunk_direct(address, CHUNK_BYTES);
        if (result) {
            failed_park_rollback(result);
            hole = SIZE_MAX;
            return;
        }
    }
    void *view = address;
    const int32_t result = map_chunk_direct(&view, CHUNK_BYTES, ORBIS_KERNEL_PROT_CPU_RW,
        MAP_FIXED | ORBIS_MAP_NO_OVERWRITE, physical, CHUNK_BYTES);
    if (!result && view == address) {
        hole = SIZE_MAX;
        return;
    }
    if (!result) {
        clear_alias = view;
        clear_bytes = CHUNK_BYTES;
    }
    hole = index;
    failed_park_rollback(result ? result : (int32_t)ORBIS_KERNEL_ERROR_EFAULT);
}

static bool park_chunk(size_t index)
{
    GuestChunk *chunk = &chunks[index];
    if (allocation_error || chunk->state != CHUNK_DIRECT)
        return false;
    const uintptr_t start = owner->base + index * CHUNK_BYTES;
    /* Skip partially protected or aliased stack chunks. A full direct mapping
     * is the only writable original backing eligible for replacement. */
    if (writable_mapping_end(start) < start + CHUNK_BYTES)
        return false;
    void *const address = (void *)start;
    const int32_t protect = sceKernelMprotect(address, CHUNK_BYTES, PROT_READ);
    if (protect) {
        ++cold_failures;
        return false;
    }
    ++chunk->protection_epoch;
    /* A dense slot index keeps file offsets independent of the virtual
     * address layout. A chunk keeps its slot, so the file never grows past
     * the number of distinct parked chunks. Partial writes never replace the
     * canonical view: the direct slice stays owned until the reservation lands. */
    if (!chunk->slot && cold_next_slot >= cold_store.slots) {
        sceKernelMprotect(address, CHUNK_BYTES, PROT_READ | PROT_WRITE);
        ++cold_failures;
        return false;
    }
    const size_t slot = chunk->slot ? chunk->slot - 1 : cold_next_slot;
    /* R57: stage the bytes in RAM (a fast copy) instead of writing the file
     * while the heap and host locks are held. */
    const int already = staged_index(slot);
    const int stage_at = already >= 0 ? already :
        cold_staged_count < COLD_STAGING_SLOTS ? (int)cold_staged_count : -1;
    if (stage_at >= 0)
        memcpy(cold_staging[stage_at], address, CHUNK_BYTES);
    else
        ++cold_direct_writes;
    if ((stage_at < 0 && !hzn_cold_store_save(&cold_store, slot, address)) ||
        unmap_chunk_direct(address, CHUNK_BYTES)) {
        const int32_t result = sceKernelMprotect(address, CHUNK_BYTES, PROT_READ | PROT_WRITE);
        if (result) {
            void *before = NULL, *end = NULL;
            int32_t protection = 0;
            if (sceKernelQueryMemoryProtection(address, &before, &end, &protection) ||
                (uintptr_t)before > start || (uintptr_t)end <= start)
                hole = index;
            failed_park_rollback(result);
        }
        ++cold_failures;
        return false;
    }

    DirectBlock *block = &blocks[chunk->block];
    const off_t physical = block->offset + (off_t)((size_t)chunk->slice * CHUNK_BYTES);
    /* Hold the virtual range with a bare reservation so no other mapping can
     * land in it. Faults on it fall through to restore_cold(). */
    void *view = address;
    int32_t result = sceKernelReserveVirtualRange(&view, CHUNK_BYTES, MAP_FIXED, CHUNK_BYTES);
    if (result) {
        view = address;
        result = sceKernelReserveVirtualRange(&view, CHUNK_BYTES, 0, CHUNK_BYTES);
    }
    if (result || view != address) {
        if (!cold_reserve_error)
            cold_reserve_error = result ? result : (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
        if (!result && view != address && unmap_chunk_direct(view, CHUNK_BYTES)) {
            clear_alias = view;
            clear_bytes = CHUNK_BYTES;
            hole = index;
            failed_park_rollback((int32_t)ORBIS_KERNEL_ERROR_EFAULT);
        } else {
            restore_parked_direct(index, physical, address, false);
        }
        ++cold_failures;
        return false;
    }
    if (!chunk->slot)
        chunk->slot = (uint32_t)++cold_next_slot;
    chunk->state = CHUNK_COLD;
    if (stage_at >= 0 && stage_at == (int)cold_staged_count) {
        cold_staged_slot[stage_at] = slot;
        ++cold_staged_count;
    }
    ++cold_chunks;
    --mapped_chunks;
    /* Keep ownership on release failure so teardown can still free it. */
    if (release_slices(block, chunk->slice, 1)) {
        ++cold_failures;
        return false;
    }
    ++cold_released;
    return true;
}

/* R57: write staged chunks to the cold file with no lock held. Call from the
 * parking thread after it releases the host lock. Returns the bytes written. */
size_t hzn_heap_sparse_cold_flush(void)
{
    if (!atomic_load_explicit(&active, memory_order_acquire))
        return 0;
    lock_heap();
    const size_t count = cold_staged_count;
    unlock_heap();
    for (size_t i = 0; i < count; ++i) {
        if (!hzn_cold_store_save(&cold_store, cold_staged_slot[i], cold_staging[i])) {
            /* The physical memory is already released: this data is lost. */
            lock_heap();
            failed_park_rollback((int32_t)ORBIS_KERNEL_ERROR_EFAULT);
            unlock_heap();
            return i * CHUNK_BYTES;
        }
    }
    lock_heap();
    cold_staged_count = 0;
    cold_staged_total += count;
    unlock_heap();
    return count * CHUNK_BYTES;
}

/* The loader's kernel lock serializes parking with FSP's prepare-and-read
 * pair. The heap lock handles racing translated CPU loads and stores. */
size_t hzn_heap_sparse_cold_park(void *address, size_t bytes)
{
    if (!cold_store.slots || !atomic_load_explicit(&active, memory_order_acquire))
        return 0;
    const uintptr_t start = (uintptr_t)address;
    const size_t mask = CHUNK_BYTES - 1;
    if (start < owner->base || start - owner->base >= owner->size ||
        bytes > owner->size - (start - owner->base))
        return 0;
    const size_t offset = start - owner->base;
    const size_t first = (offset + mask) / CHUNK_BYTES, end = (offset + bytes) / CHUNK_BYTES;
    size_t parked = 0;
    for (size_t index = first; index < end; ++index) {
        lock_heap();
        parked += park_chunk(index) ? CHUNK_BYTES : 0;
        unlock_heap();
    }
    return parked;
}

static bool retry_park_fault(uintptr_t address, uintptr_t instruction, uint32_t epoch)
{
    static _Thread_local struct {
        uintptr_t address, instruction;
        uint32_t epoch;
    } previous;
    if (!epoch || (previous.address == address && previous.instruction == instruction && previous.epoch == epoch))
        return false;
    previous.address = address;
    previous.instruction = instruction;
    previous.epoch = epoch;
    return true;
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
    enum { TRAP = 24, ADDRESS = 25, ERROR = 27, RIP = 28, CS = 29, SS = 32 };
    const uint64_t *frame = context;
    if (frame[CS] != 0x43 || frame[SS] != 0x3b || frame[ADDRESS] != address ||
        (uint32_t)frame[TRAP] != ORBIS_PAGE_TRAP ||
        (frame[ERROR] & (PAGE_RESERVED_BIT | PAGE_INSTRUCTION)))
        return false;

    const size_t index = (address - owner->base) / CHUNK_BYTES;
    lock_heap();
    bool recovered;
    if (chunks[index].state == CHUNK_COLD) {
        /* Parked, or parked while this write waited on the read-only snapshot. */
        recovered = restore_cold(index);
    } else if (frame[ERROR] & PAGE_PRESENT) {
        /* The trap raced the read-only snapshot of a park that rolled back.
         * A repeated trap with a valid view indicates a real pager fault. */
        recovered = (frame[ERROR] & PAGE_WRITE) && writable_mapping_end(address) != 0 &&
            retry_park_fault(address, frame[RIP], chunks[index].protection_epoch);
    } else if (chunks[index].state != CHUNK_EMPTY) {
        /* A fault can race a successful map. Keep intentional stack
         * protections intact by requiring the present writable range. */
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
        DirectBlock *block = &blocks[block_count - 1];
        const size_t count = block->bytes / CHUNK_BYTES;
        for (size_t slice = 0; slice < count;) {
            if (slice_released(block, slice)) {
                ++slice;
                continue;
            }
            const size_t first = slice++;
            while (slice < count && !slice_released(block, slice))
                ++slice;
            const int32_t result = release_slices(block, first, slice - first);
            if (result) {
                heap->last_host_error = result;
                return false;
            }
        }
        --block_count;
    }
    if (!close_cold_file()) {
        heap->last_host_error = errno;
        return false;
    }
    free(chunks);
    free(blocks);
    chunks = NULL;
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
    mapped_chunks = cold_chunks = cold_released = cold_failures = cold_restored = cold_next_slot = 0;
    allocation_error = cold_rollback_error = cold_reserve_error = 0;
    const size_t count = size / CHUNK_BYTES;
    if (address != requested) {
        result = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
    } else {
        chunks = calloc(count, sizeof *chunks);
        blocks = calloc(count, sizeof *blocks);
        if (!chunks || !blocks)
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
        if (!make_resident(index)) {
            prepared = false;
            break;
        }
    }
    for (uintptr_t at = address; prepared && at < end;) {
        at = writable_mapping_end(at);
        prepared = at != 0;
    }
    unlock_heap();
    return prepared;
}

void hzn_heap_sparse_report(FILE *log, const char *phase)
{
    if (!atomic_load_explicit(&active, memory_order_acquire))
        return;
    lock_heap();
    const size_t committed = mapped_chunks * CHUNK_BYTES, allocated = physical_bytes, count = block_count;
    const size_t file = cold_chunks * CHUNK_BYTES, reclaimed = cold_released * CHUNK_BYTES, failed = cold_failures;
    const size_t restored = cold_restored * CHUNK_BYTES, slots = cold_next_slot * CHUNK_BYTES;
    const size_t staged = cold_staged_total * CHUNK_BYTES, direct_writes = cold_direct_writes * CHUNK_BYTES;
    const int32_t error = allocation_error, reserve_error = cold_reserve_error;
    unlock_heap();
    fprintf(log, "Heap sparse phase=%s chunk=%d committed=%zu physical=%zu blocks=%zu "
                 "reserved=%" PRIu64 " rc=%#" PRIx32 " file_backed=%zu reclaimed=%zu spill_failures=%zu"
                 " restored=%zu cold_file=%zu reserve_rc=%#" PRIx32 " staged_writes=%zu direct_writes=%zu\n",
            phase, CHUNK_BYTES, committed, allocated, count, owner->size, (uint32_t)error, file, reclaimed, failed,
            restored, slots, (uint32_t)reserve_error, staged, direct_writes);
}
