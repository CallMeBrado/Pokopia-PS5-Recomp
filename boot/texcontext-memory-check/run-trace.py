import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import time

project = Path('E:/Pokopia-PS4')
run = Path(__file__).resolve().parent
exe = run / 'pokopia-graphics-sparse-host.exe'
opus = project / 'remote-service/chiaki-ng/1.10.1/chiaki-ng-Win'
env = dict(os.environ, POKOPIA_DESKTOP_MEMORY_MIB='3306', POKOPIA_RENDER_RESOLUTION='720',
           POKOPIA_ROMFS_ROOT='G:/pokopia_dump_v2-0-1/updates/655360/romfs',
           POKOPIA_SHADER_CACHE=str(project / 'boot/shader-cache-655360'), POKOPIA_SAVE_ROOT=str(run / 'save'),
           TMP=str(run / 'temp'), TEMP=str(run / 'temp'), PATH=str(opus) + os.pathsep + os.environ['PATH'])
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
kernel.ReadProcessMemory.restype = w.BOOL
kernel.CloseHandle.argtypes = [w.HANDLE]
with (run / 'stdout.log').open('wb') as stdout, (run / 'stderr.log').open('wb') as stderr, (run / 'queue.log').open('w', buffering=1) as trace:
    with subprocess.Popen([str(exe)], cwd=project / 'loader-probe', env=env, stdout=stdout, stderr=stderr,
                          creationflags=subprocess.CREATE_NO_WINDOW) as proc:
        handle = kernel.OpenProcess(0x410, False, proc.pid)
        if not handle:
            proc.terminate()
            raise OSError(c.get_last_error())

        def read(addr, size):
            if not 0x7000000000 <= addr < 0x8000000000:
                return b''
            buf = c.create_string_buffer(size)
            got = c.c_size_t()
            return bytes(buf) if kernel.ReadProcessMemory(handle, addr, buf, size, c.byref(got)) and got.value == size else b''

        def value(addr, size=8):
            raw = read(addr, size)
            return int.from_bytes(raw, 'little') if raw else None

        def pointer(addr):
            return value(addr) or 0

        def hex_value(val):
            return '-' if val is None else f'{val:#x}'

        start = time.monotonic()
        last = None
        seen_threads = set()
        last_stack = -20

        def trace_stack(dependency, manager, queue, records, entries, capacity, head):
            for kind in (1, 11):
                obj = pointer(manager + 0x951d8 + kind*8)
                vtable = pointer(obj)
                print(f'  kind{kind} obj={obj:#x} vtable={vtable:#x} load={pointer(vtable + 16):#x}', file=trace)
            if head and capacity and entries and records:
                record = records + 16 * min((head - 1) % capacity, entries - 1)
                job = pointer(record + 8)
                print(f'  active job callback={pointer(record):#x} data={job:#x} raw={read(job,40).hex()}', file=trace)
            for i in range(1, 96):
                tls = 0x7e00000000 + i*0x10000
                thread = pointer(tls + 0x1f8)
                base, size = pointer(thread+80), pointer(thread+88)
                arg, entry = pointer(thread+96), pointer(thread+104)
                callback = pointer(arg+504)
                end = base + size
                length = min(size, 8192)
                stack = read(end-length, length)
                values = [int.from_bytes(stack[i:i+8], 'little') for i in range(0,len(stack),8)]
                if not any(0x710087c500 <= v < 0x710087d030 for v in values) and callback != 0x710087c500:
                    continue
                print(f'  worker tls={tls:#x} sdk={thread:#x} arg={arg:#x} entry={entry:#x} callback={callback:#x} name={read(arg+464,40)!r}', file=trace)
                for n,val in enumerate(values):
                    if ((0x7100000000 <= val < 0x7102800000) or (0x7500000000 <= val < 0x7500c00000) or
                        (0x7c80000000 <= val < 0x7c90000000) or end-length <= val < end):
                        addr = end-length+n*8
                        print(f'    frame {addr:#x}={val:#x}', file=trace)
        try:
            while proc.poll() is None and time.monotonic() - start < 345:
                dependency = pointer(0x7103561920)
                manager = pointer(0x71035aebf8)
                worker_manager = pointer(pointer(0x7103487828))
                slot = manager + 0x50098
                queue = pointer(slot + 48)
                head = value(queue + 0x10018)
                queued = value(queue + 0x10020)
                records, entries, capacity = (pointer(queue + i * 8) for i in range(3))
                row = (dependency, value(dependency + 40, 4), value(dependency + 52, 1), manager, worker_manager,
                       value(manager + 24, 4), value(0x71035ae92c, 4), value(manager + 0x503e1, 1), queue, head, queued,
                       value(slot + 56, 4), value(slot + 28, 4), value(slot + 36, 4), value(slot + 40, 4))
                elapsed = time.monotonic() - start
                if row[1] == 2 and row[5] == 1 and elapsed - last_stack >= 8:
                    print(f'{elapsed:7.2f} dependency active stack', file=trace)
                    trace_stack(dependency, manager, queue, records, entries, capacity, head)
                    last_stack = elapsed
                if row != last:
                    print(f'{elapsed:7.2f} C/state/ready root/root2/active ctrl/init queue/head/jobs pend/sem/mutex/key',
                          ' '.join(map(hex_value, row)), file=trace)
                    if queued and head is not None and entries and capacity and records:
                        for i in range(min(queued, 3)):
                            record = records + 16 * min((head + i) % capacity, entries - 1)
                            callback = pointer(record)
                            job = pointer(record + 8)
                            print(f'  job{i} callback={callback:#x} data={job:#x} raw={read(job,40).hex()}', file=trace)
                    if row[1] == 2 and (last is None or last[1] != 2):
                        print(f'dependency state2 after {elapsed:.1f}s, jobs={queued}, active={row[5]}, sem={row[12]}', flush=True)
                    last = row
                for i in range(2, 67):
                    if i in seen_threads:
                        continue
                    tls = 0x7e00000000 + i * 0x10000
                    thread = pointer(tls + 0x1f8)
                    stack = pointer(thread + 80)
                    size = pointer(thread + 88)
                    entry = pointer(stack + size - 32)
                    if entry:
                        print(f'{elapsed:7.2f} thread tls={tls:#x} object={thread:#x} first_entry={entry:#x}', file=trace)
                        seen_threads.add(i)
                time.sleep(0.15)
        finally:
            kernel.CloseHandle(handle)
            if proc.poll() is None:
                proc.terminate()
        print('diagnostic exit', proc.wait(), flush=True)
