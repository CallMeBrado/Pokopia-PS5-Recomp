from pathlib import Path
import re

scratch = Path(__file__).resolve().parent
root = scratch.parent.parent
lift = root / 'game-bringup/lift/main-ps4-memory'

hooks = {
    'c_185.cpp': {0x7101ff6274: True},
    'c_050.cpp': {0x710089dc18: True, 0x710089dc44: True},
    'c_136.cpp': {0x710179ce2c: False, 0x710179ce50: False, 0x710179ce64: False, 0x710179ce84: False,
                  0x710179ceb0: False, 0x710179cd08: True, 0x710179cd5c: True,
                  0x710179c7d8: True, 0x710179c858: True, 0x710179c8c4: True, 0x710179c8f8: True,
                  0x710179ca10: True, 0x710179ca9c: False, 0x710179cacc: False, 0x710179caf4: False,
                  0x710179cb34: False, 0x710179cb58: False, 0x710179cb90: False, 0x710179cbc0: False},
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
    (scratch / unit).write_text(source)

loader = (root / 'loader-probe/loader_probe.c').read_text()
loader = re.sub(r'(#include ")\.\./', rf'\g<1>{root.as_posix()}/', loader)
anchor = 'int nx_site_midhook(Ctx *context, uint64_t address)\n{'
if loader.count(anchor) != 1:
    raise ValueError('missing graphics midhook insertion point')
loader = loader.replace(anchor, (scratch / 'trace-hook.inc').read_text() + '\n' + anchor + '\n    if (trace_texcontext(context, address))\n        return 0;')
(scratch / 'loader_trace.c').write_text(loader)

runner = (root / 'boot/sparse-heap-integration/run.ps1').read_text()
runner = runner.replace("(Join-Path $root 'loader-probe/loader_probe.c')", "(Join-Path $PSScriptRoot 'loader_trace.c')", 1)
runner = runner.replace('    [switch] $DebugSymbols,', '    [switch] $DebugSymbols,\n    [switch] $BuildOnly,')
runner = runner.replace('    $cppObjects = @()', '''    $cppObjects = @()
    foreach ($unit in 'c_185.cpp', 'c_050.cpp', 'c_136.cpp') {
        $file = Join-Path $PSScriptRoot $unit
        $object = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($unit) + '.obj')
        if (!(Test-Path -LiteralPath $object) -or (Get-Item -LiteralPath $object).LastWriteTime -lt (Get-Item -LiteralPath $file).LastWriteTime) {
            & $clang /nologo /O2 /std:c++20 /EHsc /DNOMINMAX /clang:-march=btver2 /clang:-ffp-contract=off /I $include /I (Join-Path $root 'game-bringup/lift/main-ps4-memory') /c $file "/Fo$object"
            if ($LASTEXITCODE -ne 0) { throw "Desktop TexContext trace overlay compile failed ($LASTEXITCODE)." }
        }
        $cppObjects += $object
    }''')
runner = runner.replace('$logName = if ($Ps4Memory)', 'if ($BuildOnly) { return }\n\n$logName = if ($Ps4Memory)', 1)
runner = runner.replace('if ($Ps4Memory) { $archives +=', 'if ($Ps4Memory) { $archives +=', 1)
(scratch / 'run.ps1').write_text(runner)
