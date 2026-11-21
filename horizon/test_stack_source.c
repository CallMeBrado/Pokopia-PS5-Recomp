/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"
#include "stack_source.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const uint64_t heap_base = UINT64_C(0x7800000000);
static const uint64_t arena = UINT64_C(0x7e20000000);
static const uint64_t stride = UINT64_C(0x100000);
static const uint64_t first = UINT64_C(0x780009e000);
static const uint64_t first_size = UINT64_C(0x60000);
static const uint64_t second = UINT64_C(0x780011f000);
static const uint64_t second_size = UINT64_C(0xc000);
static const uint64_t first_thread = UINT64_C(0x780009dab8);
static const uint64_t second_thread = UINT64_C(0x780011e080);

static uint8_t *bytes(uint64_t address)
{
    return (uint8_t *)(uintptr_t)address;
}

static MEMORY_BASIC_INFORMATION region(uint64_t address)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(bytes(address), &info, sizeof info) == sizeof info);
    return info;
}

static void test_sources(void)
{
    HznStackSources sources = {0};
    HznHeapStackAliases aliases = {0};
    assert(!hzn_stack_sources_init(&sources, arena + 0x1000, stride));
    assert(!hzn_stack_sources_init(&sources, arena, UINT64_MAX & ~UINT64_C(0xffff)));
    assert(hzn_stack_sources_init(&sources, arena, stride));

    uint8_t *collision = VirtualAlloc(bytes(arena), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(collision == bytes(arena));
    collision[0] = 0xb0;
    bytes(first_thread)[0] = 0xa0;
    bytes(first)[-1] = 0xa1;
    bytes(first)[0] = 0x21;
    bytes(first)[first_size - 1] = 0x22;
    bytes(first)[first_size] = 0xa2;
    bytes(second)[0] = 0x31;

    uint64_t first_source = 1, second_source = 1;
    assert(hzn_stack_source_prepare(&sources, first_thread, first + 1, first_size, &first_source) ==
           HZN_STACK_MAP_INVALID_ADDRESS && !first_source);
    assert(hzn_stack_source_prepare(&sources, first_thread, first, 0x3001, &first_source) ==
           HZN_STACK_MAP_INVALID_SIZE && !first_source);
    assert(hzn_stack_source_prepare(&sources, first_thread, first, first_size, &first_source) == HZN_STACK_MAP_OK);
    assert(first_source == arena + stride && sources.count == 1 && !sources.last_host_error);
    assert(!hzn_stack_source_at(&sources, 0));
    const HznStackSourceRange *range = hzn_stack_source_at(&sources, 1);
    assert(range && range->thread_object == first_thread && range->original == first);
    assert(range->address == first_source && range->size == first_size);
    assert(bytes(first_source)[0] == 0x21 && bytes(first_source)[first_size - 1] == 0x22);
    assert(collision[0] == 0xb0 && region(arena).Protect == PAGE_READWRITE);
    assert(hzn_stack_source_prepare(&sources, second_thread, first + 0x4000, second_size, &second_source) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE && !second_source);
    assert(hzn_stack_source_prepare(&sources, first_thread, second, second_size, &second_source) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE && !second_source);
    assert(hzn_stack_source_prepare(&sources, second_thread, second, second_size, &second_source) == HZN_STACK_MAP_OK);
    assert(second_source == arena + 2 * stride && sources.count == 2);
    assert(bytes(second_source)[0] == 0x31);

    const uint64_t first_dst = UINT64_C(0x7c90f2a000);
    const uint64_t retry_dst = UINT64_C(0x7ccdd51000);
    assert(hzn_heap_stack_alias_map(&aliases, first_dst, first_source, first_size) == HZN_STACK_MAP_OK);
    assert(region(first_source).Protect == PAGE_NOACCESS && region(first).Protect == PAGE_READWRITE);
    assert(bytes(first_dst)[0] == 0x21 && bytes(first_dst)[first_size - 1] == 0x22);
    assert(hzn_stack_source_release(&sources, &aliases, first_thread) == HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(!hzn_stack_sources_release(&sources, &aliases) && sources.count == 2);
    assert(bytes(first_thread)[0] == 0xa0 && bytes(first)[-1] == 0xa1 && bytes(first)[first_size] == 0xa2);
    bytes(first_dst)[0] = 0x41;
    assert(hzn_heap_stack_alias_unmap(&aliases, first_dst, first_source, first_size) == HZN_STACK_MAP_OK);
    assert(region(first_dst).State == MEM_FREE && region(first_source).Protect == PAGE_READWRITE);
    assert(bytes(first_source)[0] == 0x41 && bytes(first)[0] == 0x21);

    /* The SDK can undo a speculative map and use the same source again. */
    assert(hzn_heap_stack_alias_map(&aliases, retry_dst, first_source, first_size) == HZN_STACK_MAP_OK);
    assert(bytes(retry_dst)[0] == 0x41 && bytes(retry_dst)[first_size - 1] == 0x22);
    bytes(retry_dst)[first_size - 1] = 0x42;
    assert(hzn_heap_stack_alias_unmap(&aliases, retry_dst, first_source, first_size) == HZN_STACK_MAP_OK);
    assert(hzn_stack_source_release(&sources, &aliases, first_thread) == HZN_STACK_MAP_OK);
    assert(hzn_stack_source_release(&sources, &aliases, first_thread) == HZN_STACK_MAP_OK);
    assert(sources.count == 1 && !hzn_stack_source_at(&sources, 1));
    assert(region(first_source).State == MEM_FREE);
    assert(bytes(first)[0] == 0x41 && bytes(first)[first_size - 1] == 0x42);
    assert(bytes(first_thread)[0] == 0xa0 && bytes(first)[-1] == 0xa1 && bytes(first)[first_size] == 0xa2);
    assert(hzn_stack_source_prepare(&sources, first_thread, first, first_size, &first_source) == HZN_STACK_MAP_OK);
    assert(first_source == arena + stride && bytes(first_source)[0] == 0x41);

    assert(hzn_heap_stack_alias_release(&aliases));
    assert(hzn_stack_sources_release(&sources, &aliases));
    assert(hzn_stack_sources_release(&sources, &aliases));
    assert(!sources.count && !sources.base && !sources.unsafe);
    assert(region(first_source).State == MEM_FREE && region(second_source).State == MEM_FREE);
    assert(bytes(second)[0] == 0x31 && collision[0] == 0xb0);
    assert(VirtualFree(collision, 0, MEM_RELEASE));
}

static void test_small_source(void)
{
    HznStackSources sources = {0};
    HznHeapStackAliases aliases = {0};
    const uint64_t original = UINT64_C(0x7800136000);
    const uint64_t thread = UINT64_C(0x7800135210);
    const uint64_t destination = UINT64_C(0x7c90f2a000);
    const uint64_t guest_size = HZN_STACK_ALIAS_BYTES;
    assert(hzn_stack_sources_init(&sources, arena, stride));

    bytes(thread)[0] = 0xa0;
    bytes(original)[-1] = 0xa1;
    bytes(original)[0] = 0x21;
    bytes(original)[guest_size - 1] = 0x22;
    bytes(original)[guest_size] = 0xa2;
    uint64_t source;
    assert(hzn_stack_source_prepare(&sources, thread, original, guest_size, &source) == HZN_STACK_MAP_OK);
    const HznStackSourceRange *range = hzn_stack_source_at(&sources, 0);
    assert(range && range->size == guest_size && range->address == source);
    assert(region(source).RegionSize >= HZN_STACK_SOURCE_ALIGNMENT);
    assert(bytes(source)[guest_size] == 0);
    bytes(source)[guest_size] = 0x99;

    assert(hzn_heap_stack_alias_map_isolated(&aliases, destination, source, guest_size) == HZN_STACK_MAP_OK);
    const HznHeapStackAliasRange *alias = hzn_heap_stack_alias_at(&aliases, 0);
    assert(alias && alias->size == guest_size);
    assert(region(source).Protect == PAGE_NOACCESS);
    assert(region(source + guest_size).Protect == PAGE_NOACCESS);
    assert(region(destination + guest_size).State == MEM_RESERVE);
    assert(bytes(destination)[0] == 0x21 && bytes(destination)[guest_size - 1] == 0x22);
    assert(bytes(thread)[0] == 0xa0 && bytes(original)[guest_size] == 0xa2);
    bytes(destination)[guest_size - 1] = 0x42;
    assert(hzn_heap_stack_alias_unmap(&aliases, destination, source, guest_size) == HZN_STACK_MAP_OK);
    assert(region(source + guest_size).Protect == PAGE_READWRITE);
    assert(bytes(source)[guest_size] == 0x99);
    assert(hzn_stack_source_release(&sources, &aliases, thread) == HZN_STACK_MAP_OK);
    assert(region(source).State == MEM_FREE && region(destination).State == MEM_FREE);
    assert(bytes(original)[guest_size - 1] == 0x42);
    assert(bytes(thread)[0] == 0xa0 && bytes(original)[-1] == 0xa1 && bytes(original)[guest_size] == 0xa2);
    assert(hzn_heap_stack_alias_release(&aliases));
    assert(hzn_stack_sources_release(&sources, &aliases));
}


static void test_worker_capacity(void)
{
    enum { worker_slots = 128, original_stride = 0x8000, destination_stride = 0x10000, stack_size = 0x6000 };
    const uint64_t originals = UINT64_C(0x7810000000), destinations = UINT64_C(0x7c90000000);
    const uint64_t source_arena = UINT64_C(0x7e10000000), source_stride = 0x400000;
    HznStackSources sources = {0};
    HznHeapStackAliases aliases = {0};
    assert(HZN_STACK_SOURCE_LIMIT == worker_slots);
    assert(hzn_stack_sources_init(&sources, source_arena, source_stride));
    assert(VirtualAlloc(bytes(originals), (worker_slots + 1) * original_stride,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == bytes(originals));

    for (unsigned i = 0; i < worker_slots; ++i) {
        const uint64_t thread = originals + i * original_stride;
        const uint64_t original = thread + 0x1000, destination = destinations + i * destination_stride + 0x1000;
        bytes(original)[0] = (uint8_t)i;
        bytes(original)[stack_size - 1] = 0x55;
        uint64_t source;
        assert(hzn_stack_source_prepare(&sources, thread, original, stack_size, &source) == HZN_STACK_MAP_OK);
        assert(source == source_arena + i * source_stride);
        assert(hzn_heap_stack_alias_map_isolated(&aliases, destination, source, stack_size) == HZN_STACK_MAP_OK);
        assert(region(source).Protect == PAGE_NOACCESS && region(source + 0x7000).Protect == PAGE_NOACCESS);
        assert(bytes(destination)[0] == (uint8_t)i && bytes(destination)[stack_size - 1] == 0x55);
        bytes(destination)[0] = (uint8_t)(i ^ 0x80);
        bytes(destination)[stack_size - 1] = 0xa1;
    }
    assert(aliases.count == worker_slots && sources.count == worker_slots);

    const uint64_t extra_thread = originals + worker_slots * original_stride;
    const uint64_t extra_destination = destinations + worker_slots * destination_stride + 0x1000;
    uint64_t extra_source = 1;
    assert(hzn_stack_source_prepare(&sources, extra_thread, extra_thread + 0x1000, stack_size, &extra_source) ==
           HZN_STACK_MAP_OUT_OF_MEMORY && !extra_source);
    assert(hzn_heap_stack_alias_map(&aliases, extra_destination, extra_thread, 0x4000) == HZN_STACK_MAP_OUT_OF_MEMORY);
    assert(!aliases.last_host_error && region(extra_destination).State == MEM_FREE);

    const uint64_t freed_thread = originals + 64 * original_stride;
    const uint64_t freed_source = source_arena + 64 * source_stride;
    assert(hzn_heap_stack_alias_unmap(&aliases, destinations + 64 * destination_stride + 0x1000,
                                    freed_source, stack_size) == HZN_STACK_MAP_OK);
    assert(hzn_stack_source_release(&sources, &aliases, freed_thread) == HZN_STACK_MAP_OK);
    assert(hzn_stack_source_prepare(&sources, extra_thread, extra_thread + 0x1000, stack_size, &extra_source) ==
           HZN_STACK_MAP_OK && extra_source == freed_source);
    assert(hzn_heap_stack_alias_map_isolated(&aliases, extra_destination, extra_source, stack_size) == HZN_STACK_MAP_OK);
    bytes(extra_destination)[0] = 0x72;

    assert(hzn_heap_stack_alias_release(&aliases));
    assert(hzn_stack_sources_release(&sources, &aliases));
    for (unsigned i = 0; i < worker_slots; ++i) {
        const uint8_t *original = bytes(originals + i * original_stride + 0x1000);
        assert(original[0] == (uint8_t)(i ^ 0x80) && original[stack_size - 1] == 0xa1);
    }
    assert(bytes(extra_thread + 0x1000)[0] == 0x72);
    assert(VirtualFree(bytes(originals), 0, MEM_RELEASE));
}

int main(void)
{
    HznHeap heap = {0};
    uint64_t guest_base;
    assert(hzn_heap_init(&heap, heap_base, HZN_HEAP_GRANULE));
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &guest_base) == HZN_HEAP_OK);
    assert(guest_base == heap_base);
    test_sources();
    test_small_source();
    test_worker_capacity();
    assert(hzn_heap_release(&heap));
    puts("PASS aligned stack sources, exact placement, neighbor safety, map retry and copyback");
    return 0;
}
