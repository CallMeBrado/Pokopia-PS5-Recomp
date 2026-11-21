"""Read guest callers at temporary debugger breakpoints in an isolated process."""

import ctypes as C
from ctypes import wintypes as W
import runpy
import struct
import subprocess
from pathlib import Path
import time

scratch = Path(__file__).resolve().parent
helpers = runpy.run_path(str(scratch / "reference-compare.py"))
reader = runpy.run_path(str(scratch / "read-boot-state.py"))
kernel, snapshot = (reader[name] for name in ("kernel", "snapshot"))

THUNKS = {0x2D1AE0: "elements", 0x2D1D30: "base-vertex", 0x2D1F80: "instanced"}
CONTINUE = 0x00010002
NOT_HANDLED = 0x80010001
CONTEXT_FLAGS = 0x00100003  # AMD64 control and integer registers


class Event(C.Structure):
    _fields_ = [("code", W.DWORD), ("process_id", W.DWORD), ("thread_id", W.DWORD),
                ("unused", W.DWORD), ("data", C.c_byte * 176)]


kernel.WaitForDebugEvent.argtypes = (C.POINTER(Event), W.DWORD)
kernel.ContinueDebugEvent.argtypes = (W.DWORD, W.DWORD, W.DWORD)
kernel.DebugActiveProcess.argtypes = (W.DWORD,)
kernel.DebugActiveProcessStop.argtypes = (W.DWORD,)
kernel.GetThreadContext.argtypes = (W.HANDLE, C.c_void_p)
kernel.SetThreadContext.argtypes = (W.HANDLE, C.c_void_p)
kernel.GetFinalPathNameByHandleW.argtypes = (W.HANDLE, W.LPWSTR, W.DWORD, W.DWORD)
kernel.WriteProcessMemory.argtypes = kernel.ReadProcessMemory.argtypes
kernel.FlushInstructionCache.argtypes = (W.HANDLE, C.c_void_p, C.c_size_t)


def check(result):
    if not result:
        raise C.WinError(C.get_last_error())
    return result


def context(thread):
    buffer = C.create_string_buffer(1248)
    address = (C.addressof(buffer) + 15) & ~15
    value = (C.c_byte * 1232).from_address(address)
    struct.pack_into("<I", value, 48, CONTEXT_FLAGS)
    check(kernel.GetThreadContext(thread, address))
    return buffer, address, value


def read(process, address, spec):
    size = struct.calcsize(spec)
    buffer = C.create_string_buffer(size)
    check(kernel.ReadProcessMemory(process, address, buffer, size, None))
    return struct.unpack(spec, buffer.raw)


def write(process, address, data):
    check(kernel.WriteProcessMemory(process, address, C.c_char_p(data), len(data), None))


def arm(process, base, enabled):
    for offset in THUNKS:
        address = base + offset
        expected, replacement = (0x55, b"\xcc") if enabled else (0xCC, b"\x55")
        if read(process, address, "<B")[0] != expected:
            raise RuntimeError(f"unexpected NVN thunk instruction at {address:#x}")
        write(process, address, replacement)
        check(kernel.FlushInstructionCache(process, address, 1))


def walk_frames(process, frame):
    out = []
    while len(out) < 10 and 0x7C00000000 <= frame < 0x7D00000000 and not frame & 7:
        caller, return_pc = read(process, frame, "<2Q")
        out.append(return_pc)
        if not 0 < caller - frame < 0x200000:
            break
        frame = caller
    return ",".join(f"{pc:#x}" for pc in out)


def trace(proc, log):
    event = Event()
    threads = {}
    process = base = 0
    stopped = False
    start = time.monotonic()
    observed = set()
    loading_counts = set()
    hits = 0
    while time.monotonic() - start < 30:
        if not kernel.WaitForDebugEvent(C.byref(event), 100):
            if C.get_last_error() == 121:
                continue
            raise C.WinError(C.get_last_error())
        status = CONTINUE
        try:
            if event.code == 3:  # CREATE_PROCESS_DEBUG_EVENT
                file, process, thread = struct.unpack_from("<3Q", event.data)
                threads[event.thread_id] = thread
                if file:
                    kernel.CloseHandle(file)
            elif event.code == 2:  # CREATE_THREAD_DEBUG_EVENT
                thread, = struct.unpack_from("<Q", event.data)
                threads[event.thread_id] = thread
            elif event.code == 4:  # EXIT_THREAD_DEBUG_EVENT
                threads.pop(event.thread_id, None)
            elif event.code == 5:  # EXIT_PROCESS_DEBUG_EVENT
                return
            elif event.code == 6:  # LOAD_DLL_DEBUG_EVENT
                file, address = struct.unpack_from("<2Q", event.data)
                if file:
                    name = C.create_unicode_buffer(1024)
                    kernel.GetFinalPathNameByHandleW(file, name, len(name), 0)
                    kernel.CloseHandle(file)
                    if name.value.lower().endswith("\\pkp_rt.dll"):
                        base = address
                        print(f"NVN debugger breakpoint module base {base:#x}", file=log, flush=True)
                        arm(process, base, True)
            elif event.code == 1:  # EXCEPTION_DEBUG_EVENT
                code, _, _, address = struct.unpack_from("<II2Q", event.data)
                if code == 0x80000003 and address - base in THUNKS:
                    hits += 1
                    thread = threads[event.thread_id]
                    buffer, registers, value = context(thread)
                    ctx, = struct.unpack_from("<Q", value, 128)  # RCX
                    command, mode, kind, count, indices = read(process, ctx, "<5Q")
                    fp, lr = read(process, ctx + 29 * 8, "<2Q")
                    call = (address - base, lr, count)
                    if call not in observed or hits <= 16 or hits % 240 == 0:
                        observed.add(call)
                        print(f"{time.monotonic() - start:.3f}s #{hits} {THUNKS[call[0]]} cmd={command:#x} lr={lr:#x} "
                              f"mode={mode} type={kind} count={count} indices={indices:#x} "
                              f"frames={walk_frames(process, fp)}", file=log, flush=True)
                    if mode == 4 and kind == 1 and count in (6, 12) and 0x796D000000 <= indices < 0x796E000000:
                        loading_counts.add(count)
                        print(f"  loading state {snapshot(proc.pid)}", file=log, flush=True)
                    # All three breakpoint instructions are the same one-byte push rbp.
                    # Apply its native stack effect and resume at the following instruction.
                    rsp, rbp = struct.unpack_from("<2Q", value, 152)
                    write(process, rsp - 8, struct.pack("<Q", rbp))
                    struct.pack_into("<Q", value, 152, rsp - 8)
                    struct.pack_into("<Q", value, 248, address + 1)
                    check(kernel.SetThreadContext(thread, registers))
                    if loading_counts == {6, 12}:
                        arm(process, base, False)
                        stopped = True
                elif code != 0x80000003:
                    status = NOT_HANDLED
        finally:
            check(kernel.ContinueDebugEvent(event.process_id, event.thread_id, status))
        if stopped:
            check(kernel.DebugActiveProcessStop(proc.pid))
            return
    proc.terminate()


run, env = helpers["prepare_reference"](False, "-draw-callers-verified")
with (run / "stdout.txt").open("wb") as stdout, (run / "stderr.txt").open("wb") as stderr, \
     (run / "draw-callers.txt").open("w", encoding="ascii") as log:
    executable = str(helpers["reference"] / "pkp.exe")
    with subprocess.Popen([executable, "--headless", "--max-seconds", "25", "--log", "reference.log"],
                          cwd=run, env=env, creationflags=subprocess.CREATE_NO_WINDOW,
                          stdout=stdout, stderr=stderr) as proc:
        try:
            time.sleep(6)
            check(kernel.DebugActiveProcess(proc.pid))
            trace(proc, log)
        except BaseException:
            proc.terminate()
            raise
print(run)
