from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import struct

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.ReadProcessMemory.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                     ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
kernel.ReadProcessMemory.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = (wintypes.HANDLE,)


@contextmanager
def open_readonly(pid):
    process = kernel.OpenProcess(0x10, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())

    def read(address, spec):
        size = struct.calcsize(spec)
        buffer = ctypes.create_string_buffer(size)
        if not kernel.ReadProcessMemory(process, address, buffer, size, None):
            raise OSError(f'guest address {address:#x}: {ctypes.WinError(ctypes.get_last_error())}')
        return struct.unpack(spec, buffer.raw)

    try:
        yield read
    finally:
        kernel.CloseHandle(process)
