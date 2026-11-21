from pathlib import Path

p=Path('replay-census.py')
s=p.read_text()
s=s.replace('import ctypes as c','import ctypes as c\nfrom collections import Counter')
s=s.replace('    seen=set()\n    cursor=first','    seen=set()\n    kinds=Counter()\n    cursor=first',1)
s=s.replace('''        raw=read(cursor,32)
        if not raw:''','''        address=cursor
        raw=read(address,32)
        if not raw:''',1)
s=s.replace('''        ended+=1
        ops+=count''','''        if count:
            records=read(address+32,count*952)
            if len(records)!=count*952:
                raise OSError('could not read list payload')
            kinds.update(struct.unpack_from('<I',records,j*952)[0] for j in range(count))
        ended+=1
        ops+=count''',1)
s=s.replace("print(f'found {len(ready)}')",'''    print("operation counts",dict(sorted(kinds.items())))
    assert all(0 <= key <= 6 for key in kinds)
print(f'found {len(ready)}')''')
p.write_text(s)
