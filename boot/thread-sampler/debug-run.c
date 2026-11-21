#include <stdbool.h>
#define main sample_main
#include "sample.c"
#undef main
#include "../../sdk-bringup/include/nxrecomp/ctx.h"

static bool stock_trace;

static void save_audio_input(HANDLE process, const Ctx *guest)
{
    if (guest->fn != UINT64_C(0x71011cf6c8) || guest->x[30] != UINT64_C(0x710124cd24))
        return;
    const uint64_t addresses[] = {guest->x[21], guest->x[17]};
    const char *paths[] = {
        stock_trace ? "E:/Pokopia-PS4/boot/thread-sampler/stock-input-x21.bin" :
                      "E:/Pokopia-PS4/boot/thread-sampler/r16-input-x21.bin",
        stock_trace ? "E:/Pokopia-PS4/boot/thread-sampler/stock-input-x17.bin" :
                      "E:/Pokopia-PS4/boot/thread-sampler/r16-input-x17.bin",
    };
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t words[32];
        SIZE_T bytes;
        uint64_t address = addresses[i];
        if (address < guest->x[21] || address > guest->x[12] || sizeof words > guest->x[12] - address ||
            !ReadProcessMemory(process, (const void *)(uintptr_t)address, words, sizeof words, &bytes) ||
            bytes != sizeof words)
            continue;
        FILE *file;
        if (fopen_s(&file, paths[i], "wb") != 0)
            continue;
        fwrite(words, 1, sizeof words, file);
        fclose(file);
        printf("Saved audio input x%u @0x%" PRIx64 "; tag=0x%08" PRIx32 " length=0x%08" PRIx32 "\n",
               i ? 17 : 21, address, words[0], words[1]);
        if (i)
            printf("Chunk count=%" PRIu32 " ids=0x%08" PRIx32 " starts=0x%08" PRIx32 " sizes=0x%08" PRIx32 "\n",
                words[6], words[7], words[8], words[9]);
    }
}

static void print_guest(HANDLE process, uint64_t pointer)
{
    Ctx guest;
    SIZE_T bytes;
    if (!pointer || !ReadProcessMemory(process, (const void *)(uintptr_t)pointer, &guest, sizeof guest, &bytes) ||
        bytes != sizeof guest || guest.fn < UINT64_C(0x7100000000) || guest.fn >= UINT64_C(0x7700100000) ||
        guest.x[31] < UINT64_C(0x7c00000000) || guest.x[31] >= UINT64_C(0x7e00000000))
        return;
    printf("Ctx pointer=0x%" PRIx64 " fn=0x%" PRIx64 " NZCV=%u%u%u%u\n",
           pointer, guest.fn, guest.N, guest.Z, guest.C, guest.V);
    for (unsigned i = 0; i < 32; ++i)
        printf(" x%-2u=%016" PRIx64 "%s", i, guest.x[i], i % 4 == 3 ? "\n" : "");
    save_audio_input(process, &guest);
    if (stock_trace && guest.fn == UINT64_C(0x71011cf6c8) &&
        guest.x[30] == UINT64_C(0x710124cd24)) {
        MEMORY_BASIC_INFORMATION memory;
        if (VirtualQueryEx(process, (const void *)(uintptr_t)guest.x[6], &memory, sizeof memory))
            printf("Derived x6 region base=%p bytes=0x%zx state=0x%lx protect=0x%lx\n",
                   memory.BaseAddress, memory.RegionSize, (unsigned long)memory.State, (unsigned long)memory.Protect);
    }
}

static void exception_context(HANDLE process, DWORD thread_id, const EXCEPTION_RECORD *exception)
{
    printf("Exception 0x%08lx native=0x%p thread=%lu\n", (unsigned long)exception->ExceptionCode,
        exception->ExceptionAddress, (unsigned long)thread_id);
    if (exception->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && exception->NumberParameters >= 2) {
        const uintptr_t address = exception->ExceptionInformation[1];
        printf("AV kind=%" PRIuPTR " address=0x%" PRIxPTR "\n", exception->ExceptionInformation[0], address);
        MEMORY_BASIC_INFORMATION memory;
        if (VirtualQueryEx(process, (const void *)address, &memory, sizeof memory))
            printf("AV region base=%p bytes=0x%zx state=0x%lx protect=0x%lx\n",
                memory.BaseAddress, memory.RegionSize, (unsigned long)memory.State, (unsigned long)memory.Protect);
    }
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
    if (thread && GetThreadContext(thread, &ctx)) {
        const uint64_t registers[] = {ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp,
            ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
        const char *names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        for (unsigned i = 0; i < 16; ++i)
            printf(" %s=%016" PRIx64 "%s", names[i], registers[i], i % 4 == 3 ? "\n" : "");
        for (unsigned i = 0; i < 16; ++i)
            print_guest(process, registers[i]);
        STACKFRAME64 frame = {
            .AddrPC = {.Offset = ctx.Rip, .Mode = AddrModeFlat},
            .AddrStack = {.Offset = ctx.Rsp, .Mode = AddrModeFlat},
            .AddrFrame = {.Offset = ctx.Rbp, .Mode = AddrModeFlat},
        };
        print_frame(process, frame.AddrPC.Offset);
        DWORD64 prior_pc = frame.AddrPC.Offset, prior_stack = frame.AddrStack.Offset;
        for (unsigned i = 0; i < 40; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, NULL,
                            SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset)
                break;
            if (frame.AddrPC.Offset == prior_pc && frame.AddrStack.Offset == prior_stack) {
                if (!i)
                    continue;
                break;
            }
            print_frame(process, frame.AddrPC.Offset);
            prior_pc = frame.AddrPC.Offset;
            prior_stack = frame.AddrStack.Offset;
        }
    }
    if (thread)
        CloseHandle(thread);
    fflush(stdout);
}

static DWORD64 get_symbol(HANDLE process, const char *name)
{
    union {
        SYMBOL_INFO symbol;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    } storage = {0};
    storage.symbol.SizeOfStruct = sizeof(SYMBOL_INFO);
    storage.symbol.MaxNameLen = MAX_SYM_NAME;
    if (!SymFromName(process, name, &storage.symbol)) {
        fprintf(stderr, "Missing symbol %s: %lu\n", name, GetLastError());
        return 0;
    }
    return storage.symbol.Address;
}

static bool patch_code(HANDLE process, DWORD64 address, BYTE value)
{
    SIZE_T size;
    if (!WriteProcessMemory(process, (void *)(uintptr_t)address, &value, sizeof value, &size) ||
        size != sizeof value)
        return false;
    return FlushInstructionCache(process, (const void *)(uintptr_t)address, sizeof value);
}

static bool set_breakpoints(HANDLE process, DWORD64 points[5], BYTE original[5])
{
    DWORD64 parser = get_symbol(process, "f_710124ccc0");
    DWORD64 caller = get_symbol(process, "f_710115cbf0");
    if (!parser || !caller)
        return false;
    points[0] = parser;
    points[1] = parser + 0x756;
    points[2] = caller + 0x3a9;
    points[3] = get_symbol(process, "pokopia-graphics-host!stop");
    points[4] = get_symbol(process, "pokopia_guest_probe");
    for (unsigned i = 0; i < 5; ++i) {
        if (!points[i])
            continue;
        SIZE_T size;
        if (!ReadProcessMemory(process, (const void *)(uintptr_t)points[i], &original[i], 1, &size) ||
            size != 1 || original[i] == 0xcc || !patch_code(process, points[i], 0xcc)) {
            fprintf(stderr, "Could not arm breakpoint %u at 0x%" PRIx64 " (%lu)\n",
                    i, (uint64_t)points[i], GetLastError());
            points[i] = 0;
        }
    }
    printf("Software breakpoints armed: parser=0x%" PRIx64 " read=0x%" PRIx64
           " return=0x%" PRIx64 " stop=0x%" PRIx64 " probe=0x%" PRIx64 "\n",
           (uint64_t)points[0], (uint64_t)points[1], (uint64_t)points[2], (uint64_t)points[3],
           (uint64_t)points[4]);
    fflush(stdout);
    return true;
}

static bool clear_breakpoint(HANDLE process, DWORD thread_id, DWORD64 point, BYTE original)
{
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
    if (!thread)
        return false;
    CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
    bool ok = GetThreadContext(thread, &ctx) && patch_code(process, point, original);
    if (ok) {
        ctx.Rip = point;
        ok = SetThreadContext(thread, &ctx);
    }
    CloseHandle(thread);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7)
        return 2;
    stock_trace = argc == 7 && !strcmp(argv[6], "stock");
    SECURITY_ATTRIBUTES security = {.nLength = sizeof security, .bInheritHandle = TRUE};
    HANDLE output = CreateFileA(argv[4], GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE errors = CreateFileA(argv[5], GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not redirect child streams: %lu\n", GetLastError());
        return 2;
    }
    STARTUPINFOA startup = {.cb = sizeof startup, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = input,
        .hStdOutput = output, .hStdError = errors};
    PROCESS_INFORMATION child;
    if (!CreateProcessA(argv[1], NULL, NULL, NULL, TRUE, DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW,
                        NULL, argv[2], &startup, &child)) {
        fprintf(stderr, "Could not create debug child: %lu\n", GetLastError());
        return 2;
    }
    CloseHandle(output);
    CloseHandle(errors);
    CloseHandle(input);
    printf("Debug child PID %lu\n", (unsigned long)child.dwProcessId);
    fflush(stdout);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_UNDNAME |
                  SYMOPT_NO_PROMPTS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(child.hProcess, argv[3], FALSE))
        fprintf(stderr, "SymInitialize failed %lu\n", GetLastError());
    DWORD exit_code = 2;
    DWORD64 points[5] = {0};
    BYTE original[5] = {0};
    bool armed = false;
    bool parser_returned = false;
    DEBUG_EVENT event;
    while (WaitForDebugEvent(&event, 60000)) {
        DWORD reply = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            SymLoadModuleEx(child.hProcess, NULL, argv[1], NULL,
                            (DWORD64)(uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage, 0, NULL, 0);
            if (event.u.CreateProcessInfo.hFile)
                CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile)
                CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *info = &event.u.Exception;
            const DWORD code = info->ExceptionRecord.ExceptionCode;
            if (stock_trace && code == EXCEPTION_BREAKPOINT && info->dwFirstChance) {
                const DWORD64 pc = (DWORD64)(uintptr_t)info->ExceptionRecord.ExceptionAddress;
                if (!armed) {
                    printf("Initial debugger thread %lu, primary %lu\n",
                           (unsigned long)event.dwThreadId, (unsigned long)child.dwThreadId);
                    armed = set_breakpoints(child.hProcess, points, original);
                } else {
                    unsigned index = 0;
                    while (index < 5 && points[index] != pc)
                        ++index;
                    if (index < 5 && clear_breakpoint(child.hProcess, event.dwThreadId, points[index], original[index])) {
                        static const char *labels[] = {"stock parser entry", "stock corresponding native read",
                            "stock parser returned", "stock host STOP", "stock probe entry"};
                        printf("Hit %s\n", labels[index]);
                        points[index] = 0;
                        exception_context(child.hProcess, event.dwThreadId, &info->ExceptionRecord);
                        parser_returned |= index == 2;
                        if (index == 3 && event.dwThreadId != child.dwThreadId)
                            sample(child.hProcess, child.dwThreadId);
                    } else {
                        reply = DBG_EXCEPTION_NOT_HANDLED;
                    }
                }
            } else if (code != EXCEPTION_BREAKPOINT || !info->dwFirstChance) {
                reply = DBG_EXCEPTION_NOT_HANDLED;
                if (!info->dwFirstChance) {
                    SymRefreshModuleList(child.hProcess);
                    exception_context(child.hProcess, event.dwThreadId, &info->ExceptionRecord);
                }
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            exit_code = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            break;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, reply))
            break;
    }
    if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0)
        TerminateProcess(child.hProcess, 2);
    SymCleanup(child.hProcess);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    printf("Debug child exit 0x%08lx\n", (unsigned long)exit_code);
    return stock_trace ? !parser_returned : (exit_code == 0xc0000005u ? 0 : 1);
}

