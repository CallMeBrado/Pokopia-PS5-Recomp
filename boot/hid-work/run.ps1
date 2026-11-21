$ErrorActionPreference = 'Stop'
$vs='C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($line in (& cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
}
$env:TEMP=$PSScriptRoot
$env:TMP=$PSScriptRoot
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$orbis=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang.exe'
$bundle='E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $clang /nologo /std:c17 /O2 /W4 /WX /I../../horizon /I../live-shared-check test_hid.c hid.c ../../horizon/cmif.c ../live-shared-check/live_shared.c /Fetest-hid.exe
    if ($LASTEXITCODE) { throw "HID host fixture compile failed ($LASTEXITCODE)." }
    & ./test-hid.exe
    if ($LASTEXITCODE) { throw "HID host fixture failed ($LASTEXITCODE)." }
    $flags=@('--target=x86_64-pc-freebsd12-elf','-march=btver2','-std=c11','-O2','-fPIC','-funwind-tables',
        '-D__PS4__','-DPS4','-D__ORBIS__','-D_BSD_SOURCE=1','-isysroot',"$bundle/sdk",
        '-isystem',"$bundle/orbis-compat/include",'-isystem',"$bundle/sdk/include",
        '-include','orbis_prefix.h','-Wall','-Wextra','-Werror')
    foreach ($name in 'hid','hid_ps4') {
        & $orbis @flags -I../../horizon -c "$name.c" -o "$name.orbis.o"
        if ($LASTEXITCODE) { throw "HID Orbis compile failed ($LASTEXITCODE) for $name." }
    }
} finally {
    Pop-Location
}

