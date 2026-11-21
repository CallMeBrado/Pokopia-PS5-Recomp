#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include "../../horizon/heap_sparse.h"

enum { CHUNK = 0x10000, PAGE = 0x1000, WORKERS = 8 };

static DWORD WINAPI read_first_touch(void *opaque)
{
    const volatile unsigned char *byte = opaque;
    return *byte;
}

int main(void)
{
    HznHeap heap = {0};
    uint64_t base = UINT64_C(0x7800000000);
    assert(hzn_heap_init(&heap, base, HZN_HEAP_GRANULE));
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &base) == HZN_HEAP_OK);
    unsigned char *bytes = (void *)(uintptr_t)base;
    MEMORY_BASIC_INFORMATION page;
    assert(VirtualQuery(bytes, &page, sizeof page) && page.State == MEM_RESERVE);

    HANDLE workers[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        workers[i] = CreateThread(NULL, 0, read_first_touch, bytes + 3 * CHUNK, 0, NULL);
        assert(workers[i]);
    }
    assert(WaitForMultipleObjects(WORKERS, workers, TRUE, 30000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        DWORD result = 1;
        assert(GetExitCodeThread(workers[i], &result) && !result && CloseHandle(workers[i]));
    }
    assert(hzn_heap_sparse_prepare_io(bytes + CHUNK - 32, 64));
    assert(VirtualQuery(bytes, &page, sizeof page) && page.State == MEM_COMMIT);
    assert(VirtualQuery(bytes + CHUNK, &page, sizeof page) && page.State == MEM_COMMIT);
    assert(VirtualQuery(bytes + 2 * CHUNK, &page, sizeof page) && page.State == MEM_RESERVE);
    assert(!hzn_heap_sparse_prepare_io(bytes + HZN_HEAP_GRANULE - 32, 64));
    assert(!hzn_heap_sparse_prepare_io((void *)(uintptr_t)(base - 32), 64));
    assert(hzn_heap_sparse_prepare_io(&page, sizeof page));

    DWORD previous;
    assert(VirtualProtect(bytes + PAGE, PAGE, PAGE_NOACCESS, &previous));
    assert(!hzn_heap_sparse_prepare_io(bytes + PAGE - 8, 16));
    assert(hzn_heap_sparse_prepare_io(bytes + PAGE + PAGE, 1));
    bool rejected = false;
    __try {
        (void)*(volatile unsigned char *)(bytes + PAGE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rejected = GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION;
    }
    assert(rejected);
    assert(VirtualQuery(bytes + PAGE, &page, sizeof page) && page.Protect == PAGE_NOACCESS);
    assert(hzn_heap_release(&heap));
    assert(hzn_heap_release(&heap));
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &base) == HZN_HEAP_OK);
    assert(VirtualQuery(bytes, &page, sizeof page) && page.State == MEM_RESERVE);
    assert(hzn_heap_release(&heap));
    puts("Sparse heap concurrent first touch, I/O backing, protected alias and reuse passed");
    return 0;
}
