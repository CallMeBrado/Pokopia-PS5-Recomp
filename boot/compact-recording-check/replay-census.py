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

image, image_size = int(sys.argv[2]), int(sys.argv[3])
needle = re.compile(br'[\x00-\xff][\x00-\xff][\x00-\xff]\x10\x7f\x00\x00\x00')
ready = []
for address,size in sorted([*regions(0x10000,0x7000000000),*regions(0x8000000000,0x7fffffffffff)],key=lambda r:r[1]):
    if size>0x2000000:
        continue
    raw = read(address,size)
    for match in needle.finditer(raw):
        pos=match.start()
        if pos<168 or pos%8!=0:
            continue
        callbacks=struct.unpack_from('<10Q',raw,pos-152)
        if all(image<=callback<image+image_size for callback in callbacks):
            ready.append(address+pos-168)
for replay in ready:
    header=read(replay+96,80)
    if not header:
        continue
    active, first=struct.unpack_from('<2Q',header)
    next_handle=struct.unpack_from('<Q',header,72)[0]
    ended=ops=empty=legacy_capacity=0
    seen=set()
    cursor=first
    while cursor and cursor not in seen:
        raw=read(cursor,32)
        if not raw:
            raise OSError('could not read compact list')
        seen.add(cursor)
        cursor,device,handle,count=struct.unpack('<4Q',raw)
        if not 0x7000000000 <= device < 0x7e00000000 or not 0x7f10000000 <= handle < 0x7f20000000:
            raise ValueError('invalid ended list owner or handle')
        ended+=1
        ops+=count
        empty+=not count
        if count:
            legacy_capacity+=max(8,1 << (count-1).bit_length())
    active_count=active_ops=active_capacity=0
    seen=set()
    cursor=active
    while cursor and cursor not in seen:
        raw=read(cursor,4800)
        if not raw:
            break
        seen.add(cursor)
        cursor,encoder,device=struct.unpack_from('<3Q',raw)
        if not 0x7000000000 <= encoder < 0x7e00000000 or not 0x7000000000 <= device < 0x7e00000000:
            break
        count,cap=struct.unpack_from('<2Q',raw,4776)
        if count>cap or cap>1<<20:
            break
        active_count+=1
        active_ops+=count
        active_capacity+=cap
    ended_bytes=ended*32+ops*952
    active_bytes=active_count*4800+active_capacity*952
    legacy_bytes=(ended+active_count)*4808+(legacy_capacity+active_capacity)*952
    print(f'replay={replay:#x} handles_issued={(next_handle-0x7f10000000)//8} ended={ended} empty={empty} ended_ops={ops} ended_bytes={ended_bytes} active={active_count} active_ops={active_ops} active_bytes={active_bytes} total_mib={(ended_bytes+active_bytes)/(1<<20):.2f} legacy_requested_mib={legacy_bytes/(1<<20):.2f} saved_mib={(legacy_bytes-ended_bytes-active_bytes)/(1<<20):.2f}')
print(f'found {len(ready)}')