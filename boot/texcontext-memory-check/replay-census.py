import ctypes as c
import struct
import sys

kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.restype = c.c_void_p
kernel.ReadProcessMemory.argtypes = (c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p)
kernel.VirtualQueryEx.argtypes = (c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t)
kernel.VirtualQueryEx.restype = c.c_size_t
h = kernel.OpenProcess(0x410, False, int(sys.argv[1]))
if not h:
    raise OSError(c.get_last_error())

class Region(c.Structure):
    _fields_ = [('base',c.c_void_p),('allocation',c.c_void_p),('allocation_protect',c.c_uint),
                ('partition_id',c.c_ushort),('size',c.c_size_t),('state',c.c_uint),
                ('protect',c.c_uint),('kind',c.c_uint)]

def read(a,s):
    if not a:
        return b''
    b = c.create_string_buffer(s)
    return b.raw if kernel.ReadProcessMemory(h,a,b,s,None) else b''

def v(a,s=8):
    return int.from_bytes(read(a,s),'little')

def regions(start, end):
    info=Region()
    while start<end and kernel.VirtualQueryEx(h,start,c.byref(info),c.sizeof(info)):
        base=info.base or 0
        if info.state==0x1000 and info.kind==0x20000 and info.protect in (4,8):
            yield base,info.size
        start=base+info.size

import re

base, image_size = int(sys.argv[2]), int(sys.argv[3])
needle = re.compile(br'[\x00-\xff][\x00-\xff][\x00-\xff]\x10\x7f\x00\x00\x00')
ready = []
for address,size in sorted([*regions(0x10000,0x7000000000),*regions(0x8000000000,0x7fffffffffff)],key=lambda r:r[1]):
    if size>0x2000000:
        continue
    raw = read(address,size)
    for match in needle.finditer(raw):
        pos=match.start()
        if pos<160 or pos%8!=0:
            continue
        next_handle=struct.unpack_from('<Q',raw,pos)[0]
        callbacks=struct.unpack_from('<10Q',raw,pos-144)
        if not all(base<=callback<base+image_size for callback in callbacks):
            continue
        pointer=address+pos-160
        ready.append((pointer,next_handle))
for replay,next_handle in ready:
    record=v(replay+96)
    n=0
    cap=0
    live=0
    finalized=0
    empty=0
    seen=set()
    encoder_counts={}
    while record and record not in seen:
        words=read(record,4808)
        if not words: break
        seen.add(record)
        next_record,encoder,device,handle=struct.unpack_from('<4Q',words)
        ops,count,capacity=struct.unpack_from('<3Q',words,4776)
        if not 0x7000000000<=encoder<0x7e00000000 or count>capacity or capacity>1<<20:
            print('bad record',hex(record),hex(encoder),count,capacity)
            break
        n+=1
        cap+=capacity
        live+=count
        finalized+=bool(handle)
        empty+=not count
        encoder_counts[encoder]=encoder_counts.get(encoder,0)+1
        record=next_record
    total=n*4808+cap*952
    print(f'replay={replay:#x} handles_issued={(next_handle-0x7f10000000)//8} retained={n} ended={finalized} empty={empty} ops={live} cap={cap} total={total} MiB={total/(1<<20):.2f}')
    print('top encoders:',[(hex(k),v) for k,v in sorted(encoder_counts.items(),key=lambda item:-item[1])[:12]])
print(f'found {len(ready)}')