from pathlib import Path
import sys

scratch = Path(__file__).resolve().parent
root = scratch.parent.parent
lift = root / 'game-bringup/lift/main-ps4-memory'

tag = int(sys.argv[1], 16)
if not 0 < tag < 1 << 64:
    raise ValueError('Texture context tag must be an unsigned 64-bit hexadecimal value')

hooks = {
    'c_185.cpp': {0x7101ff6274: True},
    'c_050.cpp': {0x710089dc18: True, 0x710089dc44: True},
    'c_136.cpp': {0x710179ce2c: False, 0x710179ce50: False, 0x710179ce64: False, 0x710179ce84: False,
                  0x710179ceb0: False, 0x710179cd08: True, 0x710179cd5c: True,
                  0x710179c7d8: True, 0x710179c858: True, 0x710179c8c4: True, 0x710179c8f8: True,
                  0x710179ca10: True, 0x710179ca9c: False, 0x710179cacc: False, 0x710179caf4: False,
                  0x710179cb34: False, 0x710179cb58: False, 0x710179cb90: True, 0x710179cbc0: False},
}
for unit, markers in hooks.items():
    source = (lift / unit).read_text()
    for ret, need_before in markers.items():
        m = f'c->x[30] = {ret:#x}ULL; '
        if source.count(m) == 0:
            raise ValueError(f'{unit}: missing lifted call with LR {ret:#x}')
        out = []
        for line in source.splitlines(keepends=True):
            if m not in line:
                out.append(line)
                continue
            start = line.index(m)
            end = line.index(';', start+len(m)) + 1
            if need_before:
                line = line[:start] + f'(void)nx_site_midhook(c, {ret-4:#x}ULL); ' + line[start:]
                end += len(f'(void)nx_site_midhook(c, {ret-4:#x}ULL); ')
            if ret != 0x7101ff6274:
                line = line[:end] + f' (void)nx_site_midhook(c, {ret:#x}ULL);' + line[end:]
            out.append(line)
        source = ''.join(out)
    if unit == 'c_136.cpp':
        marker = '  { uint64_t t = c->x[8]; c->x[30] = 0x7101797e64ULL; call_indirect(c, t); }'
        if source.count(marker) != 1:
            raise ValueError(f'{unit}: expected one guest texture pool allocator call')
        source = source.replace(marker, '  (void)nx_site_midhook(c, 0x7101797e60ULL);\n'
                                + marker + '\n  (void)nx_site_midhook(c, 0x7101797e64ULL);')
    (scratch / f'texcontext-{unit}').write_text(source)

trace = (scratch / 'trace-texcontext.inc').read_text(encoding='utf-8-sig')
(scratch / 'trace-texcontext-loader.inc').write_text(trace.replace('TEXCONTEXT_TAG', f'UINT64_C({tag:#x})'))


