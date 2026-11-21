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

wanted = int(sys.argv[2],0)
needle = struct.pack('<Q', wanted)
ready = []
for address,size in sorted([*regions(0x10000,0x7000000000),*regions(0x8000000000,0x7fffffffffff)],key=lambda r:r[1]):
    if size>0x2000000:
        continue
    raw = read(address,size)
    begin=0
    while (pos:=raw.find(needle,begin))>=0:
        begin=pos+8
        if pos<808 or pos+64>len(raw) or pos%8!=0:
            continue
        ctx=address+pos-808
        sp=v(ctx+248); fn=v(ctx+848)
        if 0x7c00000000<=sp<0x7e00000000 and 0x7100000000<=fn<0x7700100000:
            ready.append(ctx)
for ctx in ready:
    words=struct.unpack('<32Q',read(ctx,256))
    print(f'ctx={ctx:#x} tls={wanted:#x} fn={v(ctx+848):#x} active import={v(ctx+856):#x} import_ret={v(ctx+864):#x}')
    print(' '.join(f'x{i}={word:#x}' for i,word in enumerate(words)))
    fp=words[29]
    while words[31]<=fp<words[31]+0x10000:
        nx,lr=v(fp),v(fp+8)
        print(f' fp={fp:#x} next={nx:#x} ret={lr:#x}')
        if nx<=fp:
            break
        fp=nx
print(f'found {len(ready)}')
