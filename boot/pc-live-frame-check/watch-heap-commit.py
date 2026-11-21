"""Measure committed pages in one running Windows probe's sparse guest heap."""

import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import sys
import time


class MemoryInfo(C.Structure):
    _fields_ = [("base", C.c_void_p), ("allocation_base", C.c_void_p), ("allocation_protect", W.DWORD),
                ("partition", W.WORD), ("size", C.c_size_t), ("state", W.DWORD),
                ("protect", W.DWORD), ("kind", W.DWORD)]


kernel = C.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = (W.DWORD, W.BOOL, W.DWORD)
kernel.OpenProcess.restype = W.HANDLE
kernel.CloseHandle.argtypes = (W.HANDLE,)
kernel.VirtualQueryEx.argtypes = (W.HANDLE, C.c_void_p, C.POINTER(MemoryInfo), C.c_size_t)
kernel.VirtualQueryEx.restype = C.c_size_t
kernel.GetExitCodeProcess.argtypes = (W.HANDLE, C.POINTER(W.DWORD))


def guest_commit(process, base, size):
    info = MemoryInfo()
    cursor, end, committed = base, base + size, 0
    while cursor < end:
        if not kernel.VirtualQueryEx(process, cursor, C.byref(info), C.sizeof(info)):
            return None
        next_region = min(info.base + info.size, end)
        if info.allocation_base == base and info.state == 0x1000:  # MEM_COMMIT
            committed += next_region - cursor
        cursor = next_region
    return committed


pid, size, log_path, frame_path = sys.argv[1:5]
duration = int(sys.argv[5]) if len(sys.argv) > 5 else 330
process = kernel.OpenProcess(0x400, False, int(pid))  # PROCESS_QUERY_INFORMATION
if not process:
    raise C.WinError(C.get_last_error())
try:
    start = time.monotonic()
    previous = None
    status = W.DWORD()
    with Path(log_path).open("w", encoding="ascii") as log:
        while kernel.GetExitCodeProcess(process, C.byref(status)) and status.value == 259 and time.monotonic() - start < duration:
            count = guest_commit(process, 0x7800000000, int(size, 0))
            if count is None:
                break
            state = count, Path(frame_path).is_file()
            if state != previous:
                print(f"elapsed_s={time.monotonic() - start:.2f} committed_bytes={count} "
                      f"committed_MiB={count / 1048576:.2f} captured={int(state[1])}", file=log, flush=True)
                previous = state
            time.sleep(1)
finally:
    kernel.CloseHandle(process)
