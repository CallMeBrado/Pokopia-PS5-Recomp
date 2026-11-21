
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef _WIN32
#include "stack_source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t flexible = 32 * 1024 * 1024;
static unsigned mappings, owned, anonymous_calls, direct_calls;
static bool fail_anonymous, fail_release;
static off_t next_physical;

typedef struct { void *address, *reservation; size_t bytes; off_t physical; } TestMapping;
typedef struct { off_t physical; size_t bytes; bool owned, mapped; } TestAllocation;
static TestMapping host[8];
static TestAllocation physical_memory[12];

static uint8_t *bytes(uint64_t address) { return (uint8_t *)(uintptr_t)address; }

static TestAllocation *allocation(off_t physical)
{
    for (size_t i = 0; i < sizeof physical_memory / sizeof physical_memory[0]; ++i)
        if (physical_memory[i].owned && physical_memory[i].physical == physical)
            return &physical_memory[i];
    assert(false);
    return NULL;
}

static void *host_map(void *requested, size_t size, off_t physical)
{
    assert(!((uintptr_t)requested & 0x3fff) && !(size & 0x3fff));
    void *base = (void *)((uintptr_t)requested & ~(uintptr_t)0xffff);
    const size_t reserve_bytes = (uintptr_t)requested - (uintptr_t)base + size;
    void *reservation = VirtualAlloc(base, reserve_bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!reservation)
        return NULL;
    assert(reservation == base);
    void *mapped = VirtualAlloc(requested, size, MEM_COMMIT, PAGE_READWRITE);
    assert(mapped == requested);
    for (size_t i = 0; i < sizeof host / sizeof host[0]; ++i) {
        if (host[i].address)
            continue;
        host[i] = (TestMapping){mapped, reservation, size, physical};
        ++mappings;
        return mapped;
    }
    assert(false);
    return NULL;
}

int32_t sceKernelAvailableFlexibleMemorySize(size_t *bytes)
{
    *bytes = flexible;
    return 0;
}

void *mmap(void *address, size_t size, int prot, int flags, int fd, int64_t offset)
{
    assert(prot == 3 && flags == 0x1002 && fd == -1 && !offset);
    ++anonymous_calls;
    if (fail_anonymous) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    void *mapped = host_map(address, size, -1);
    if (!mapped)
        errno = EINVAL;
    return mapped ? mapped : MAP_FAILED;
}

size_t sceKernelGetDirectMemorySize(void) { return 0x20000000; }

int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t size, size_t align, int32_t type, off_t *physical)
{
    assert(!start && end == 0x20000000 && !(size & 0x3fff) && align == 0x4000 && type == 0);
    for (size_t i = 0; i < sizeof physical_memory / sizeof physical_memory[0]; ++i) {
        if (physical_memory[i].owned)
            continue;
        physical_memory[i] = (TestAllocation){next_physical, size, true, false};
        *physical = next_physical;
        next_physical += (off_t)size;
        ++owned;
        return 0;
    }
    assert(false);
    return -1;
}

int32_t sceKernelMapDirectMemory(void **address, size_t size, int32_t protection, int32_t flags, off_t physical, size_t align)
{
    assert(protection == 3 && flags == 0x90 && align == 0x4000);
    TestAllocation *record = allocation(physical);
    assert(record->bytes == size && !record->mapped);
    ++direct_calls;
    void *mapped = host_map(*address, size, physical);
    if (!mapped)
        return (int32_t)0x80020011U;
    assert(mapped == *address);
    memset(mapped, 0xdd, size);
    record->mapped = true;
    return 0;
}

int32_t sceKernelMprotect(void *address, size_t size, int protection)
{
    assert(!((uintptr_t)address & 0x3fff) && !(size & 0x3fff));
    DWORD old;
    return VirtualProtect(address, size, protection == PROT_NONE ? PAGE_NOACCESS : PAGE_READWRITE, &old) ?
        0 : (int32_t)GetLastError();
}

int32_t sceKernelMunmap(void *address, size_t size)
{
    for (size_t i = 0; i < sizeof host / sizeof host[0]; ++i) {
        if (host[i].address != address)
            continue;
        assert(host[i].bytes == size && VirtualFree(host[i].reservation, 0, MEM_RELEASE));
        if (host[i].physical >= 0)
            allocation(host[i].physical)->mapped = false;
        host[i] = (TestMapping){0};
        --mappings;
        return 0;
    }
    assert(false);
    return -1;
}

int32_t sceKernelReleaseDirectMemory(off_t physical, size_t size)
{
    TestAllocation *record = allocation(physical);
    assert(!record->mapped && record->bytes == size);
    if (fail_release) {
        fail_release = false;
        return (int32_t)0x8002000cU;
    }
    record->owned = false;
    --owned;
    return 0;
}

static DWORD protection(uint64_t address)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(bytes(address), &info, sizeof info) == sizeof info);
    return info.Protect;
}

int main(void)
{
    const uint64_t original_base = UINT64_C(0x783aa10000), original = UINT64_C(0x783aa1d000);
    const uint64_t source_arena = UINT64_C(0x7e1c800000), destination = UINT64_C(0x7cc7036000);
    const uint64_t thread = original - 0x10, size = 0x100000;
    assert(VirtualAlloc(bytes(original_base), 0x120000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) ==
           bytes(original_base));
    HznStackSources sources = {0};
    HznHeapStackAliases aliases = {0};
    assert(hzn_stack_sources_init(&sources, source_arena, 0x400000));

    uint64_t source;
    assert(hzn_stack_source_prepare(&sources, thread, original, size, &source) == HZN_STACK_MAP_OK);
    assert(source == source_arena && anonymous_calls == 1 && !owned);
    assert(hzn_stack_source_release(&sources, &aliases, thread) == HZN_STACK_MAP_OK && !mappings);

    bytes(original)[0] = 0x41;
    bytes(original)[size - 1] = 0x42;
    fail_anonymous = true;
    assert(hzn_stack_source_prepare(&sources, thread, original, size, &source) == HZN_STACK_MAP_OK);
    assert(source == source_arena && anonymous_calls == 2 && owned == 1 && next_physical == size);
    flexible = 0;
    void *collision = VirtualAlloc(bytes(destination & ~UINT64_C(0xffff)), 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(collision);
    *(uint8_t *)collision = 0x73;
    assert(hzn_heap_stack_alias_map_isolated(&aliases, destination, source, size) == HZN_STACK_MAP_OUT_OF_MEMORY);
    assert(owned == 1 && mappings == 1 && !aliases.count && *(uint8_t *)collision == 0x73);
    assert(VirtualFree(collision, 0, MEM_RELEASE));

    assert(hzn_heap_stack_alias_map_isolated(&aliases, destination, source, size) == HZN_STACK_MAP_OK);
    assert(owned == 2 && mappings == 2 && direct_calls == 3 && anonymous_calls == 2);
    assert(protection(source) == PAGE_NOACCESS);
    assert(bytes(destination)[-1] == 0 && bytes(destination)[size] == 0);
    assert(bytes(destination)[0] == 0x41 && bytes(destination)[size - 1] == 0x42);
    bytes(destination)[0] = 0x55;

    fail_release = true;
    assert(hzn_heap_stack_alias_unmap(&aliases, destination, source, size) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(aliases.unsafe && aliases.count == 1 && !hzn_heap_stack_alias_at(&aliases, 0));
    assert(owned == 2 && mappings == 1 && protection(source) == PAGE_READWRITE && bytes(source)[0] == 0x55);
    assert(hzn_heap_stack_alias_release(&aliases) && owned == 1 && mappings == 1);

    fail_release = true;
    assert(hzn_stack_source_release(&sources, &aliases, thread) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(sources.unsafe && owned == 1 && !mappings && bytes(original)[0] == 0x55);
    assert(hzn_stack_sources_release(&sources, &aliases) && !owned && !mappings);
    assert(VirtualFree(bytes(original_base), 0, MEM_RELEASE));
    puts("PASS R33 exact Orbis stack addresses, flexible EINVAL fallback, collision isolation and partial-release retries");
}
