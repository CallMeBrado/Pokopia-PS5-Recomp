/* SPDX-License-Identifier: GPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cold_store.h"

static bool fail_save, fail_load, fail_release, fail_map, fail_rollback;
static HANDLE save_started, load_started, resume_io;
static const void *fail_alias_slice, *fail_alias_restore;

static bool save_checked(const HznColdStore *store, size_t slot, const void *page)
{
    if (save_started) {
        assert(SetEvent(save_started));
        assert(WaitForSingleObject(resume_io, 5000) == WAIT_OBJECT_0);
    }
    if (!fail_save)
        return hzn_cold_store_save(store, slot, page);
    const LARGE_INTEGER offset = {.QuadPart = (int64_t)slot * HZN_COLD_PAGE_BYTES};
    HANDLE file = (HANDLE)store->file;
    DWORD written = 0;
    assert(SetFilePointerEx(file, offset, NULL, FILE_BEGIN));
    assert(WriteFile(file, page, HZN_COLD_PAGE_BYTES / 2, &written, NULL));
    assert(written == HZN_COLD_PAGE_BYTES / 2);
    return false;
}

static bool load_checked(const HznColdStore *store, size_t slot, void *page)
{
    if (fail_load) {
        memset(page, 0xa5, HZN_COLD_PAGE_BYTES / 2);
        return false;
    }
    const bool loaded = hzn_cold_store_load(store, slot, page);
    if (load_started) {
        assert(SetEvent(load_started));
        assert(WaitForSingleObject(resume_io, 5000) == WAIT_OBJECT_0);
    }
    return loaded;
}

static BOOL WINAPI free_checked(LPVOID address, SIZE_T bytes, DWORD type)
{
    if (fail_release && bytes == HZN_COLD_PAGE_BYTES && type == (MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    return VirtualFree(address, bytes, type);
}

static PVOID WINAPI map_checked(HANDLE file, HANDLE process, PVOID address, ULONG64 offset, SIZE_T size,
    ULONG type, ULONG protect, PMEM_EXTENDED_PARAMETER parameters, ULONG count)
{
    if (fail_map) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return MapViewOfFile3(file, process, address, offset, size, type, protect, parameters, count);
}

static BOOL WINAPI protect_checked(LPVOID address, SIZE_T bytes, DWORD protect, PDWORD old)
{
    if ((fail_rollback && protect == PAGE_READWRITE) ||
        (address == fail_alias_slice && protect == PAGE_NOACCESS) ||
        (address == fail_alias_restore && protect == PAGE_READWRITE)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return VirtualProtect(address, bytes, protect, old);
}

#define hzn_cold_store_save save_checked
#define hzn_cold_store_load load_checked
#define VirtualFree free_checked
#define MapViewOfFile3 map_checked
#define VirtualProtect protect_checked
#include "cold_heap_windows.c"
#include "heap_stack_alias.c"
#undef hzn_cold_store_save
#undef hzn_cold_store_load
#undef VirtualFree
#undef MapViewOfFile3
#undef VirtualProtect

static const uint64_t heap_base = UINT64_C(0x7800000000);
enum { CHUNK = HZN_COLD_PAGE_BYTES, CHUNKS = 4 };

static uint8_t pattern(size_t index, uint8_t seed)
{
    return (uint8_t)(seed ^ index ^ (index >> 6) ^ (index >> 12));
}

static void fill(volatile uint8_t *page, uint8_t seed)
{
    for (size_t i = 0; i < CHUNK; ++i)
        page[i] = pattern(i, seed);
}

static void check(const volatile uint8_t *page, uint8_t seed)
{
    for (size_t i = 0; i < CHUNK; ++i)
        assert(page[i] == pattern(i, seed));
}

static MEMORY_BASIC_INFORMATION mapping(const void *address)
{
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(address, &info, sizeof info));
    return info;
}

typedef struct { uint8_t *address; uint8_t seed; } Task;

static DWORD WINAPI spill(void *argument)
{
    return (DWORD)hzn_heap_sparse_cold_park(argument, CHUNK);
}

static DWORD WINAPI write_page(void *argument)
{
    const Task *task = argument;
    fill(task->address, task->seed);
    return 0;
}

static DWORD WINAPI read_page(void *argument)
{
    const Task *task = argument;
    check(task->address, task->seed);
    return 0;
}

static HANDLE task(LPTHREAD_START_ROUTINE work, void *argument)
{
    HANDLE thread = CreateThread(NULL, 0, work, argument, 0, NULL);
    assert(thread);
    return thread;
}

static void join(HANDLE thread, DWORD code)
{
    DWORD actual = 0;
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(GetExitCodeThread(thread, &actual) && actual == code);
    assert(CloseHandle(thread));
}

static void check_cross_chunk_alias(HznHeap *heap)
{
    uint8_t *source = (uint8_t *)heap->mapping + 0x1c000;
    uint8_t *next_chunk = source + 0x4000;
    const size_t bytes = 0xc000;
    uint8_t *destination = (uint8_t *)(uintptr_t)UINT64_C(0x7c5ad1f000);
    assert(hzn_heap_sparse_prepare_io(source, bytes));
    assert(mapping(source).AllocationBase != mapping(next_chunk).AllocationBase);
    source[0] = 0x61;
    source[bytes - 1] = 0x62;
    HznHeapStackAliases aliases = {0};
    fail_alias_slice = next_chunk;
    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(!aliases.count && !aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(mapping(source).Protect == PAGE_READWRITE && mapping(next_chunk).Protect == PAGE_READWRITE);
    assert(source[0] == 0x61 && source[bytes - 1] == 0x62);

    fail_alias_restore = source;
    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(aliases.count == 1 && aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(mapping(source).Protect == PAGE_NOACCESS && mapping(next_chunk).Protect == PAGE_READWRITE);
    fail_alias_slice = fail_alias_restore = NULL;
    assert(hzn_heap_stack_alias_release(&aliases));
    assert(mapping(source).Protect == PAGE_READWRITE && mapping(next_chunk).Protect == PAGE_READWRITE);

    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) == HZN_STACK_MAP_OK);
    assert(mapping(source).Protect == PAGE_NOACCESS && mapping(next_chunk).Protect == PAGE_NOACCESS);
    assert(mapping(source - 1).Protect == PAGE_READWRITE && mapping(source + bytes).Protect == PAGE_READWRITE);
    assert(destination[0] == 0x61 && destination[bytes - 1] == 0x62);
    destination[0] = 0x83;
    destination[bytes - 1] = 0x84;
    assert(hzn_heap_stack_alias_unmap(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) == HZN_STACK_MAP_OK);
    assert(!aliases.count && !aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(source[0] == 0x83 && source[bytes - 1] == 0x84);
    puts("PASS exact 0xc000 cross-chunk stack MapMemory protection, copyback and rollback");
}

int main(void)
{
    char path[64];
    assert(snprintf(path, sizeof path, "spill-%lu.tmp", (unsigned long)GetCurrentProcessId()) > 0);
    HznHeap heap = {.base = heap_base};
    assert(hzn_heap_sparse_cold_open(path));
    assert(!hzn_heap_sparse_cold_open(path));
    assert(hzn_heap_sparse_reserve(&heap, CHUNKS * CHUNK));
    uint8_t *first = heap.mapping;
    uint8_t *middle = first + CHUNK;
    uint8_t *last = first + 2 * CHUNK;
    assert((uintptr_t)first == heap_base && mapping(middle).State == MEM_RESERVE);
    assert(*(volatile uint8_t *)middle == 0 && mapping(middle).State == MEM_COMMIT);
    fill(first, 0x21);
    fill(middle, 0x42);
    fill(last, 0x63);

    assert(hzn_heap_sparse_cold_park(first + 1, CHUNK - 1) == 0);
    assert(hzn_heap_sparse_cold_park(first + 1, 2 * CHUNK) == CHUNK);
    assert(mapping(first).State == MEM_COMMIT && mapping(last).State == MEM_COMMIT);
    assert(mapping(middle).State == MEM_RESERVE);
    assert(hzn_heap_sparse_cold_park(middle, CHUNK) == 0);
    check(middle, 0x42);
    assert(mapping(middle).State == MEM_COMMIT && mapping(middle).Protect == PAGE_READWRITE);
    check(first, 0x21);
    check(last, 0x63);
    puts("PASS exact 64 KiB park, raw read restore, unchanged neighboring chunks, and one-time accounting");

    fill(middle, 0x37);
    fail_save = true;
    assert(hzn_heap_sparse_cold_park(middle, CHUNK) == 0);
    assert(mapping(middle).State == MEM_COMMIT && mapping(middle).Protect == PAGE_READWRITE);
    check(middle, 0x37);
    fail_rollback = true;
    assert(hzn_heap_sparse_cold_park(middle, CHUNK) == 0);
    assert(mapping(middle).Protect == PAGE_READONLY);
    fail_rollback = fail_save = false;
    fill(middle, 0x31);
    assert(mapping(middle).Protect == PAGE_READWRITE);
    fail_release = true;
    assert(hzn_heap_sparse_cold_park(first, CHUNK) == 0);
    assert(mapping(first).State == MEM_COMMIT && mapping(first).Protect == PAGE_READWRITE);
    fail_release = false;
    check(first, 0x21);
    puts("PASS partial write, failed release, and protection rollback preserve resident bytes");

    assert(hzn_heap_sparse_cold_park(middle, CHUNK) == CHUNK);
    fail_load = true;
    assert(!hzn_heap_sparse_prepare_io(middle, CHUNK));
    assert(mapping(middle).State == MEM_RESERVE);
    fail_load = false;
    fail_map = true;
    assert(!hzn_heap_sparse_prepare_io(middle, CHUNK));
    assert(mapping(middle).State == MEM_RESERVE);
    fail_map = false;
    assert(hzn_heap_sparse_prepare_io(middle, CHUNK));
    check(middle, 0x31);
    assert(!hzn_heap_sparse_prepare_io(first - 1, 2));
    assert(!hzn_heap_sparse_prepare_io(first + CHUNKS * CHUNK - 1, 2));
    DWORD old;
    assert(VirtualProtect(last + 4096, 4096, PAGE_NOACCESS, &old));
    assert(hzn_heap_sparse_cold_park(last, CHUNK) == 0);
    assert(!hzn_heap_sparse_prepare_io(last, CHUNK));
    assert(VirtualProtect(last + 4096, 4096, old, &old));
    check(last, 0x63);
    puts("PASS incomplete restore or map failure leaves page parked, exact retry, and protected alias exclusion");

    resume_io = CreateEventW(NULL, TRUE, FALSE, NULL);
    save_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(resume_io && save_started);
    HANDLE saver = task(spill, middle);
    assert(WaitForSingleObject(save_started, 5000) == WAIT_OBJECT_0);
    assert(mapping(middle).Protect == PAGE_READONLY);
    Task writer_data = {middle, 0x94};
    HANDLE writer = task(write_page, &writer_data);
    assert(WaitForSingleObject(writer, 100) == WAIT_TIMEOUT);
    assert(SetEvent(resume_io));
    join(saver, CHUNK);
    join(writer, 0);
    assert(CloseHandle(save_started));
    save_started = NULL;
    check(middle, 0x94);
    puts("PASS concurrent writer waits for protected snapshot and resumes without losing writes");

    assert(hzn_heap_sparse_cold_park(middle, CHUNK) == CHUNK);
    assert(ResetEvent(resume_io));
    load_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(load_started);
    HANDLE reader1 = task(read_page, &writer_data);
    assert(WaitForSingleObject(load_started, 5000) == WAIT_OBJECT_0);
    HANDLE reader2 = task(read_page, &writer_data);
    assert(WaitForSingleObject(reader2, 100) == WAIT_TIMEOUT);
    assert(mapping(middle).State == MEM_RESERVE);
    assert(SetEvent(resume_io));
    join(reader1, 0);
    join(reader2, 0);
    assert(CloseHandle(load_started) && CloseHandle(resume_io));
    load_started = resume_io = NULL;
    puts("PASS racing readers see no partially restored bytes");

    check_cross_chunk_alias(&heap);
    hzn_heap_sparse_report(stdout, "fixture");
    assert(hzn_heap_sparse_release(&heap));
    assert(!heap.mapping && !heap.size && !heap.backing_size && mapping(first).State == MEM_FREE);
    assert(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    assert(hzn_heap_sparse_reserve(&heap, CHUNKS * CHUNK));
    first = heap.mapping;
    fill(first, 0xa9);
    assert(hzn_heap_sparse_cold_park(first, CHUNK) == 0);
    check(first, 0xa9);
    assert(hzn_heap_sparse_release(&heap));
    puts("PASS teardown releases all views and spill file; disabled path uses original sparse heap");
    return 0;
}

