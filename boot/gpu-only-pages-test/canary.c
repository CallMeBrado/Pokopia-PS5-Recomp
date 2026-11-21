#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>

#include "gpu_pages.h"

static volatile LONG observed;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        InterlockedIncrement(&observed);
        gpu_pages_fault((uintptr_t)record->ExceptionInformation[1], (uintptr_t)record->ExceptionInformation[0],
                        (uintptr_t)record->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    enum { PAGE = 0x4000 };
    unsigned char *heap = VirtualAlloc(NULL, 4 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(heap && (uintptr_t)heap % PAGE == 0);
    PVOID handler = AddVectoredExceptionHandler(1, on_fault);
    assert(handler);
    assert(gpu_pages_add(stdout, GPU_PAGES_TEXTURE, 1, (uintptr_t)heap + 8, 4 * PAGE - 16, 0xa2, true));
    gpu_pages_bound(stdout, 1);
    assert(gpu_pages_written(stdout, 1, true, (uintptr_t)heap, 4 * PAGE, "canary"));
    heap[0] = 1;
    heap[4 * PAGE - 1] = 1;
    volatile unsigned char *protected = heap + PAGE;
    __try {
        (void)*protected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        assert(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION);
    }
    __try {
        *protected = 42;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        assert(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION);
    }
    assert(observed == 2);
    assert(gpu_pages_add(stdout, GPU_PAGES_BUFFER, 2, (uintptr_t)protected, 4, 0, false));
    *protected = 42;
    assert(*protected == 42);
    assert(gpu_pages_remove(stdout, GPU_PAGES_TEXTURE, 1));
    gpu_pages_report(stdout, "canary");
    assert(RemoveVectoredExceptionHandler(handler));
    assert(VirtualFree(heap, 0, MEM_RELEASE));
    return 0;
}
