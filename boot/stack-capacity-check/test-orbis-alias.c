
#include "stack_source.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>

enum { COUNT = 128, ORIGINAL_STRIDE = 0x8000, DESTINATION_STRIDE = 0x10000, STACK_SIZE = 0x6000 };
static unsigned protected_ranges, restored_ranges;

void *__real_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset);
void *__wrap_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
    if (flags & 0x1000) {
        assert(!((uintptr_t)address & 0x3fff) && !(size & 0x3fff));
        flags = (flags & ~0x1000) | MAP_ANONYMOUS;
    }
    return __real_mmap(address, size, prot, flags, fd, offset);
}

int32_t sceKernelMprotect(void *address, size_t size, int protection)
{
    assert(!((uintptr_t)address & 0x3fff) && size == 0x8000);
    if (protection == PROT_NONE)
        ++protected_ranges;
    else
        ++restored_ranges;
    return mprotect(address, size, protection) ? -errno : 0;
}

int32_t sceKernelMunmap(void *address, size_t size)
{
    assert(!((uintptr_t)address & 0x3fff) && size == 0x8000);
    return munmap(address, size) ? -errno : 0;
}

static uint8_t *bytes(uint64_t address) { return (uint8_t *)(uintptr_t)address; }

int main(void)
{
    const uint64_t original_base = UINT64_C(0x7810000000), source_base = UINT64_C(0x7e10000000);
    const uint64_t destination_base = UINT64_C(0x7c90000000), source_stride = 0x400000;
    const size_t original_size = COUNT * ORIGINAL_STRIDE;
    assert(mmap(bytes(original_base), original_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == bytes(original_base));
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
    assert(munmap(bytes(original_base), original_size) == 0);
    puts("PASS Orbis mapping branch with real POSIX pages, 16 KiB ABI protection and 128 exact-address copybacks");
}
