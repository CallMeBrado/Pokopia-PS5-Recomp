
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef _WIN32
#include "stack_source.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

enum { COUNT = 128, ORIGINAL_STRIDE = 0x8000, DESTINATION_STRIDE = 0x10000, STACK_SIZE = 0x6000 };
static unsigned protected_ranges, restored_ranges;

void *mmap(void *address, size_t size, int prot, int flags, int fd, int64_t offset)
{
    assert(!((uintptr_t)address & 0x3fff) && !(size & 0x3fff));
    assert(prot == (PROT_READ | PROT_WRITE) && flags == (MAP_PRIVATE | 0x1000) && fd == -1 && !offset);
    void *mapped = VirtualAlloc(address, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mapped)
        errno = ENOMEM;
    return mapped ? mapped : MAP_FAILED;
}

int32_t sceKernelMprotect(void *address, size_t size, int protection)
{
    assert(!((uintptr_t)address & 0x3fff) && size == 0x8000);
    if (protection == PROT_NONE)
        ++protected_ranges;
    else
        ++restored_ranges;
    DWORD old;
    return VirtualProtect(address, size, protection == PROT_NONE ? PAGE_NOACCESS : PAGE_READWRITE, &old) ?
        0 : (int32_t)GetLastError();
}

int32_t sceKernelMunmap(void *address, size_t size)
{
    assert(!((uintptr_t)address & 0x3fff) && size == 0x8000);
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : (int32_t)GetLastError();
}

static uint8_t *bytes(uint64_t address) { return (uint8_t *)(uintptr_t)address; }

int main(void)
{
    const uint64_t original_base = UINT64_C(0x7810000000), source_base = UINT64_C(0x7e10000000);
    const uint64_t destination_base = UINT64_C(0x7c90000000), source_stride = 0x400000;
    const size_t original_size = COUNT * ORIGINAL_STRIDE;
    assert(VirtualAlloc(bytes(original_base), original_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) ==
           bytes(original_base));
    HznStackSources sources = {0};
    HznHeapStackAliases aliases = {0};
    assert(HZN_HEAP_STACK_ALIAS_LIMIT == COUNT);
    assert(hzn_stack_sources_init(&sources, source_base, source_stride));
    for (unsigned i = 0; i < COUNT; ++i) {
        const uint64_t thread = original_base + i * ORIGINAL_STRIDE;
        const uint64_t original = thread + 0x1000, dst = destination_base + i * DESTINATION_STRIDE + 0x1000;
        bytes(original)[0] = (uint8_t)i;
        uint64_t src;
        assert(hzn_stack_source_prepare(&sources, thread, original, STACK_SIZE, &src) == HZN_STACK_MAP_OK);
        assert(src == source_base + i * source_stride);
        assert(hzn_heap_stack_alias_map_isolated(&aliases, dst, src, STACK_SIZE) == HZN_STACK_MAP_OK);
        assert(bytes(dst)[0] == (uint8_t)i);
        bytes(dst)[0] ^= 0x80;
    }
    assert(aliases.count == COUNT && sources.count == COUNT && protected_ranges == COUNT);
    assert(hzn_heap_stack_alias_release(&aliases) && restored_ranges == COUNT);
    assert(hzn_stack_sources_release(&sources, &aliases));
    for (unsigned i = 0; i < COUNT; ++i)
        assert(bytes(original_base + i * ORIGINAL_STRIDE + 0x1000)[0] == (uint8_t)(i ^ 0x80));
    assert(VirtualFree(bytes(original_base), 0, MEM_RELEASE));
    puts("PASS Orbis mapping branch via Windows syscall shim, 16 KiB guards and 128 exact-address copybacks");
}
