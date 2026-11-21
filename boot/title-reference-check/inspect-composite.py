from pathlib import Path
import hashlib
import os
import struct
import sys

root = Path(__file__).resolve().parents[2]
scratch = Path(__file__).resolve().parent
source = root / 'frame-reference-655360/shader_cache/spirv'
packed = root / 'boot/shader-cache-655360'
vs = 'caa3cba58abf8cc9'
fs = '8526214507533768'

def words_as_string(words):
    return struct.pack(f'<{len(words)}I', *words).split(b'\0', 1)[0].decode('ascii')

def reflect(data):
    words = struct.unpack(f'<{len(data)//4}I', data)
    assert words[0] == 0x07230203
    print('SPIR-V version', hex(words[1]))
    names, decor, members, variables, constants, types, access = {}, {}, {}, {}, {}, {}, []
    i = 5
    while i < len(words):
        size, op = words[i] >> 16, words[i] & 0xffff
        assert size and i + size <= len(words)
        operands = words[i + 1:i + size]
        if op == 5: names[operands[0]] = words_as_string(operands[1:])
        elif op == 10: print('extension', words_as_string(operands))
        elif op == 15:
            name = words_as_string(operands[2:])
            print('entry', operands[0], name, 'interfaces', operands[2 + (len(name) + 4)//4:])
        elif op == 17: print('capability', operands[0])
        elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32): types[operands[0]] = (op, operands[1:])
        elif op == 43: constants[operands[1]] = operands[2:]
        elif op == 59: variables[operands[1]] = (operands[2], operands[0])
        elif op in (65, 66): access.append(operands)
        elif op == 71: decor.setdefault(operands[0], {})[operands[1]] = operands[2:]
        elif op == 72: members.setdefault((operands[0], operands[1]), {})[operands[2]] = operands[3:]
        i += size
    for var, (storage, ptr) in variables.items():
        if storage not in (0, 1, 2, 3): continue
        print('var', var, names.get(var), 'storage', storage, 'decor', decor.get(var, {}),
              'type', types[ptr], 'members', {index: d for (type_id, index), d in members.items() if type_id == types[ptr][1][-1]})
        for _, _, base, *indices in access:
            if base == var:
                print('   access', [constants.get(index, f'%{index}') for index in indices])

rows = {line.split('\t')[0]: line.split('\t') for line in (packed/'results.tsv').read_text(encoding='utf-8').splitlines()[1:]}
with (packed/'shaders.spvpack').open('rb') as pack:
    for key in (vs, fs):
        data = (source / (key + '.spv')).read_bytes()
        pack.seek(int(rows[key][-1]))
        assert len(data) == int(rows[key][6]) and data == pack.read(len(data))
        print(key, len(data), hashlib.sha256(data).hexdigest())
        reflect(data)
if '--prepare-dump' in sys.argv:
    target = scratch / 'selective-cache'
    target.mkdir(exist_ok=True)
    for key in rows:
        if key == vs:
            continue
        name = key + '.spv'
        dst = target / name
        if not dst.exists() and (source / name).exists():
            os.link(source / name, dst)
    (target/'results.tsv').write_bytes((source/'results.tsv').read_bytes())
