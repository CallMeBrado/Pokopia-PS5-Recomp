#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../../../sdk-bringup/include/nxrecomp/ctx.h"

static void print_frame(HANDLE process, DWORD64 address)
{
    union {
        SYMBOL_INFO symbol;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    } symbol_storage = {0};
    SYMBOL_INFO *symbol = &symbol_storage.symbol;
    symbol->SizeOfStruct = sizeof(*symbol);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement;
    IMAGEHLP_MODULE64 module = {.SizeOfStruct = sizeof(module)};
    IMAGEHLP_LINE64 line = {.SizeOfStruct = sizeof(line)};
    DWORD line_displacement;
    SymGetModuleInfo64(process, address, &module);
    printf("  %016" PRIx64 " %s", (uint64_t)address, module.ModuleName);
    if (SymFromAddr(process, address, &displacement, symbol))
        printf("!%s+0x%" PRIx64, symbol->Name, (uint64_t)displacement);
    else if (module.BaseOfImage)
        printf("+0x%" PRIx64, (uint64_t)(address - module.BaseOfImage));
    if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
        printf(" %s:%lu", line.FileName, (unsigned long)line.LineNumber);
    putchar('\n');
}

static bool print_guest_context(HANDLE process, uint64_t address)
{
    if (address < 0x10000 || address >= 0x7fffffffffff || (address & 0xf) != 0)
        return false;
    Ctx guest;
    SIZE_T bytes;
    if (!ReadProcessMemory(process, (const void *)(uintptr_t)address, &guest, sizeof guest, &bytes) ||
        bytes != sizeof guest || guest.fn < UINT64_C(0x7100000000) || guest.fn >= UINT64_C(0x7700100000) ||
        guest.x[31] < UINT64_C(0x7c00000000) || guest.x[31] >= UINT64_C(0x7e00000000) ||
        guest.tpidrro < UINT64_C(0x7e00000000) || guest.tpidrro >= UINT64_C(0x7e00800000))
        return false;
    printf("  guest ctx=%#" PRIx64 " tls=%#" PRIx64 " fn=%#" PRIx64 " pc-return=%#" PRIx64 " sp=%#" PRIx64 "\n",
           address, guest.tpidrro, guest.fn, guest.x[30], guest.x[31]);
    for (unsigned i = 0; i < 32; ++i)
        printf(" x%-2u=%016" PRIx64 "%s", i, guest.x[i], i % 4 == 3 ? "\n" : "");
    uint64_t fp = guest.x[29];
    for (unsigned i = 0; i < 45 && fp >= guest.x[31] && fp < guest.x[31] + 0x10000; ++i) {
        uint64_t frame[2];
        if (!ReadProcessMemory(process, (const void *)(uintptr_t)fp, frame, sizeof frame, &bytes) || bytes != sizeof frame)
            break;
        printf("    guest frame fp=%#" PRIx64 " next=%#" PRIx64 " ret=%#" PRIx64 "\n", fp, frame[0], frame[1]);
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    return true;
}

static void sample(HANDLE process, DWORD thread_id)
{
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    if (!thread)
        return;
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER kernel_time = {0}, user_time = {0};
    if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        kernel_time.LowPart = kernel.dwLowDateTime;
        kernel_time.HighPart = kernel.dwHighDateTime;
        user_time.LowPart = user.dwLowDateTime;
        user_time.HighPart = user.dwHighDateTime;
    }
    WCHAR *description = NULL;
    GetThreadDescription(thread, &description);
    printf("Thread %lu kernel_ms=%" PRIu64 " user_ms=%" PRIu64, (unsigned long)thread_id,
           (uint64_t)(kernel_time.QuadPart / 10000), (uint64_t)(user_time.QuadPart / 10000));
    if (description && description[0])
        printf(" name=%ls", description);
    putchar('\n');
    if (description)
        LocalFree(description);
    if (SuspendThread(thread) == (DWORD)-1) {
        CloseHandle(thread);
        return;
    }
    CONTEXT context = {.ContextFlags = CONTEXT_FULL};
    if (GetThreadContext(thread, &context)) {
        const uint64_t registers[] = {context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi, context.Rdi,
            context.Rbp, context.R8, context.R9, context.R10, context.R11, context.R12, context.R13, context.R14, context.R15};
        bool printed = false;
        for (unsigned i = 0; i < sizeof registers / sizeof registers[0] && !printed; ++i)
            printed = print_guest_context(process, registers[i]);
        if (!printed) {
            uint64_t pointers[1024];
            SIZE_T bytes;
            if (ReadProcessMemory(process, (const void *)(uintptr_t)context.Rsp, pointers, sizeof pointers, &bytes))
                for (size_t i = 0; i < bytes / sizeof pointers[0] && !printed; ++i)
                    printed = print_guest_context(process, pointers[i]);
        }
        STACKFRAME64 frame = {
            .AddrPC = {.Offset = context.Rip, .Mode = AddrModeFlat},
            .AddrStack = {.Offset = context.Rsp, .Mode = AddrModeFlat},
            .AddrFrame = {.Offset = context.Rbp, .Mode = AddrModeFlat},
        };
        print_frame(process, frame.AddrPC.Offset);
        DWORD64 previous_pc = frame.AddrPC.Offset, previous_stack = frame.AddrStack.Offset;
        for (unsigned i = 0; i < 42; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, NULL,
                            SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset)
                break;
            if (frame.AddrPC.Offset == previous_pc && frame.AddrStack.Offset == previous_stack) {
                if (!i)
                    continue;
                break;
            }
            print_frame(process, frame.AddrPC.Offset);
            previous_pc = frame.AddrPC.Offset;
            previous_stack = frame.AddrStack.Offset;
        }
    }
    ResumeThread(thread);
    CloseHandle(thread);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    DWORD pid = strtoul(argv[1], NULL, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        fprintf(stderr, "OpenProcess failed %lu\n", GetLastError());
        return 2;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_UNDNAME |
                  SYMOPT_NO_PROMPTS | SYMOPT_LOAD_LINES | SYMOPT_INCLUDE_32BIT_MODULES);
    if (!SymInitialize(process, argv[2], TRUE)) {
        fprintf(stderr, "SymInitialize failed %lu\n", GetLastError());
        CloseHandle(process);
        return 2;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid)
                sample(process, entry.th32ThreadID);
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(snapshot);
    SymCleanup(process);
    CloseHandle(process);
    return 0;
}

