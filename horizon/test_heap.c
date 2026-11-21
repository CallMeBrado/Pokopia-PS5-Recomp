/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint64_t base = UINT64_C(0x7800000000);

int main(void)
{
    HznHeap heap = {0};
    uint64_t guest_base = UINT64_MAX;

    assert(!hzn_heap_init(&heap, base + 0x4000, HZN_HEAP_GRANULE));
    assert(!hzn_heap_init(&heap, base, UINT64_MAX));
    assert(hzn_heap_init(&heap, base, 2 * HZN_HEAP_GRANULE));
    assert(hzn_heap_set_size(&heap, 0, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == base && !heap.mapping && !heap.size);
    assert(hzn_heap_set_size(&heap, 0x4000, &guest_base) == HZN_HEAP_INVALID_SIZE);
    assert(!guest_base && !heap.mapping && !heap.size);
    assert(hzn_heap_set_size(&heap, 3 * HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OUT_OF_MEMORY);
    assert(!guest_base && !heap.mapping && !heap.size);

    unsigned char *collision = VirtualAlloc((void *)(uintptr_t)base, HZN_HEAP_GRANULE,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(collision == (void *)(uintptr_t)base);
    collision[0] = 17;
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OUT_OF_MEMORY);
    assert(!guest_base && !heap.mapping && !heap.size && heap.last_host_error);
    assert(collision[0] == 17);
    assert(VirtualFree(collision, 0, MEM_RELEASE));

    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == base && heap.size == HZN_HEAP_GRANULE);
    unsigned char *bytes = (void *)(uintptr_t)guest_base;
    assert(!bytes[0] && !bytes[HZN_HEAP_GRANULE - 1]);
    bytes[HZN_HEAP_GRANULE - 1] = 42;
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == base && bytes[HZN_HEAP_GRANULE - 1] == 42);
    assert(hzn_heap_set_size(&heap, 2 * HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_INVALID_MEMORY_STATE);
    assert(!guest_base && heap.size == HZN_HEAP_GRANULE && bytes[HZN_HEAP_GRANULE - 1] == 42);
    assert(hzn_heap_set_size(&heap, 0, &guest_base) == HZN_HEAP_INVALID_MEMORY_STATE);
    assert(!guest_base && heap.size == HZN_HEAP_GRANULE && bytes[HZN_HEAP_GRANULE - 1] == 42);

    assert(hzn_heap_release(&heap));
    assert(hzn_heap_release(&heap));
    assert(!heap.size && !heap.mapping && !heap.backing_size && !heap.last_host_error);
    assert(hzn_heap_set_size(&heap, 2 * HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == base && heap.size == 2 * HZN_HEAP_GRANULE);
    assert(!((const unsigned char *)(uintptr_t)guest_base)[HZN_HEAP_GRANULE - 1]);
    assert(hzn_heap_release(&heap));

    puts("Horizon heap: budget, exact placement, collision rollback, idempotence, release passed");
    return 0;
}

