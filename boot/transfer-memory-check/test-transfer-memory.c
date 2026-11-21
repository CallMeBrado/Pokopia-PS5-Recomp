#include "transfer_memory.h"

#include <assert.h>
#include <stdio.h>

static unsigned queries;
static const uint64_t base = UINT64_C(0x7800123000);
static const uint64_t limit = UINT64_C(0x7800223000);

static bool owner_range(uint64_t address, uint64_t size)
{
    ++queries;
    return address >= base && address < limit && size <= limit - address;
}

int main(void)
{
    HznTransferMemoryTable table = {0};
    uint32_t handle = UINT32_MAX, result = UINT32_MAX;
    assert(hzn_transfer_memory_create(&table, base + 1, 0x1000, 0, owner_range, &handle) == 0xcc01);
    assert(!handle && !queries);
    assert(hzn_transfer_memory_create(&table, base, 0, 0, owner_range, &handle) == 0xca01);
    assert(hzn_transfer_memory_create(&table, base, 0x1001, 0, owner_range, &handle) == 0xca01);
    assert(hzn_transfer_memory_create(&table, UINT64_MAX - 0xfff, 0x1000, 0, owner_range, &handle) == 0xd401);
    assert(hzn_transfer_memory_create(&table, base, 0x1000, 2, owner_range, &handle) == 0xd801);
    assert(!handle && !queries);
    assert(hzn_transfer_memory_create(&table, base - 0x1000, 0x1000, 0, owner_range, &handle) == 0xd401);
    assert(hzn_transfer_memory_create(&table, limit - 0x1000, 0x2000, 0, owner_range, &handle) == 0xd401);
    assert(!handle && queries == 2);

    assert(!hzn_transfer_memory_create(&table, base, 0x2000, 1, owner_range, &handle));
    const uint32_t first = handle;
    assert(first == HZN_TRANSFER_MEMORY_HANDLE_BASE);
    const HznTransferMemory *entry = hzn_transfer_memory_lookup(&table, first);
    assert(entry && entry->address == base && entry->size == 0x2000 && entry->permission == 1);
    assert(hzn_transfer_memory_create(&table, base + 0x1000, 0x2000, 3, owner_range, &handle) == 0xd401);
    assert(!handle && hzn_transfer_memory_lookup(&table, first) == entry);
    assert(!hzn_transfer_memory_create(&table, base + 0x2000, 0x1000, 0, owner_range, &handle));
    assert(handle == first + 1);
    assert(!hzn_transfer_memory_close(&table, 0x40000, &result) && result == UINT32_MAX);
    assert(hzn_transfer_memory_close(&table, first, &result) && !result);
    assert(!hzn_transfer_memory_lookup(&table, first));
    assert(hzn_transfer_memory_close(&table, first, &result) && result == 0xe401);
    assert(!hzn_transfer_memory_create(&table, base, 0x2000, 3, owner_range, &handle));
    assert(handle == first + 2 && hzn_transfer_memory_lookup(&table, handle)->permission == 3);

    for (unsigned i = 0; i < HZN_TRANSFER_MEMORY_LIMIT - 2; ++i)
        assert(!hzn_transfer_memory_create(&table, base + 0x3000 + 0x1000 * i, 0x1000,
                                           0, owner_range, &handle));
    assert(hzn_transfer_memory_create(&table, limit - 0x1000, 0x1000, 0, owner_range, &handle) == 0xce01);
    assert(!handle && hzn_transfer_memory_close(&table, first + 1, &result) && !result);
    assert(!hzn_transfer_memory_create(&table, limit - 0x1000, 0x1000, 0, owner_range, &handle));
    assert(handle == first + HZN_TRANSFER_MEMORY_LIMIT + 1);
    puts("PASS transfer memory validation, backing identity, overlap, close, stale handles and resource limit");
}
