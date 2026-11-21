"""Read a running Windows boot probe's guest dispatcher and loader queue."""

import ctypes
from ctypes import wintypes
import struct
import sys

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.ReadProcessMemory.argtypes = (
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
)
kernel.ReadProcessMemory.restype = wintypes.BOOL
kernel.CloseHandle.argtypes = (wintypes.HANDLE,)


def snapshot(pid, include_jobs=False):
    process = kernel.OpenProcess(0x10, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())

    def read(address, spec):
        if not address:
            return (0,) * len(struct.unpack(spec, bytes(struct.calcsize(spec))))
        size = struct.calcsize(spec)
        buffer = ctypes.create_string_buffer(size)
        if not kernel.ReadProcessMemory(process, address, buffer, size, None):
            raise OSError(f"guest address {address:#x}: {ctypes.WinError(ctypes.get_last_error())}")
        return struct.unpack(spec, buffer.raw)

    def pointer(address):
        return read(address, "<Q")[0]

    try:
        dispatcher = pointer(pointer(0x71034840E8))
        if not dispatcher:
            return "dispatcher absent"
        state = read(dispatcher + 0x84828, "<I")[0]
        loader = pointer(pointer(0x7103484618))
        work = pointer(loader + 16) if loader else 0
        queue = pointer(pointer(0x7103484DA8))
        fields = [f"state={state}"]
        if queue and work:
            vtable = pointer(work)
            first_id, last_id, _, ready, error = read(work + 24, "<IIQBB")
            _, length = read(queue + 0x8058, "<2Q")
            fields += [f"work_fn={pointer(vtable + 16):#x}", f"work_id={first_id:#x}:{last_id:#x}",
                       f"work_ready={ready}", f"work_error={error}", f"work_pending={length}"]
        system = pointer(pointer(0x7103483BC8))
        if system:
            fields += [f"system_mode={read(system + 0x327D44, '<I')[0]}"]
        ui = pointer(0x7103561710)
        if ui:
            fields += [f"ui_flags={read(ui + 16, '<I')[0]:#x}",
                       f"ui_phase={read(ui + 88, '<2I')}"]
        app = pointer(0x7103561898)
        if app:
            fields += [f"app_modes={read(app + 24, '<2I')}"]
        secondary = pointer(pointer(0x7103483FD8))
        if secondary:
            fields += [f"secondary_state={read(secondary + 52, '<b')[0]}",
                       f"secondary_vtable={pointer(secondary):#x}",
                       f"secondary_lifecycle={read(secondary + 40, '<I')[0]}"]
        resources = pointer(0x71035AEBF8)
        if resources:
            slot = resources + 0x50098
            pending_queue = pointer(slot + 48)
            if pending_queue:
                head, queued = read(pending_queue + 0x10018, "<2Q")
                fields += [f"resource_queue={pending_queue:#x}", f"resource_head={head}",
                           f"resource_active={read(resources + 24, '<I')[0]}", f"resource_queued={queued}",
                           f"resource_sem={read(slot + 28, '<I')[0]}"]
                records, entries, capacity = read(pending_queue, "<3Q")
                if include_jobs and records and entries and capacity:
                    state_word, selector, request, pending, guard = read(resources + 0x503D8, "<2I3B")
                    fields += [f"resource_gate_mode={state_word:#x}", f"resource_gate_selector={selector:#x}",
                               f"resource_gate_request={request}", f"resource_gate_pending={pending}",
                               f"resource_gate_lock={guard}"]
                    for offset in (-1, 0, 1):
                        index = min((head + offset) % capacity, entries - 1)
                        callback, data = read(records + index * 16, "<2Q")
                        header = read(data, "<4Q") if 0x7000000000 <= data < 0x8000000000 else ()
                        fields += [f"job{offset:+d}={callback:#x}/{data:#x}/" + ":".join(f"{word:016x}" for word in header)]
        scene = pointer(pointer(0x7103484180))
        if scene:
            active, pending = read(scene + 16, "<2Q")
            active_vtable = pointer(active)
            pending_vtable = pointer(pending)
            fields += [f"scene_tick={pointer(active_vtable + 32) if active_vtable else 0:#x}",
                       f"scene_init={pointer(pending_vtable + 16) if pending_vtable else 0:#x}"]
            if active_vtable and pointer(active_vtable + 32) == 0x7100244A80:
                scene_flags, scene_state, _, scene_option = read(active + 48, "<IBBB")
                fields += [f"scene_state={scene_state}", f"scene_flags={scene_flags:#x}",
                           f"scene_option={scene_option}"]
        handlers = read(0x7103396DD8, "<128Q")
        if 0x71017C4C8C in handlers:
            fields += [f"draw_packet={handlers.index(0x71017C4C8C):#x}"]
        return " ".join(fields)
    finally:
        kernel.CloseHandle(process)


if __name__ == "__main__":
    print(snapshot(int(sys.argv[1]), "--jobs" in sys.argv[2:]))
