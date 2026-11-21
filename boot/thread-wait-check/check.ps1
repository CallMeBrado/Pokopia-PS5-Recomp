$ErrorActionPreference = 'Stop'
$vc = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC'
$llvm = "$vc/Tools/Llvm/x64/bin"
$msvc = "$vc/Tools/MSVC/14.50.35717"
$kits = 'C:/Program Files (x86)/Windows Kits/10'
$kitVersion = '10.0.26100.0'
$env:LIB = "$msvc/lib/x64;$kits/Lib/$kitVersion/ucrt/x64;$kits/Lib/$kitVersion/um/x64"
$env:INCLUDE = "$msvc/include;$kits/Include/$kitVersion/ucrt;$kits/Include/$kitVersion/shared;$kits/Include/$kitVersion/um"
$env:PATH = "$msvc/bin/Hostx64/x64;$env:PATH"
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
$bundle = 'E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
$sdk = "$bundle/sdk"
$overlay = "$bundle/orbis-compat"
Push-Location $PSScriptRoot
try {
    & "$llvm/clang-cl.exe" /nologo /std:c11 /O2 /W4 /WX /I../../sdk-bringup/include test_thread.c thread.c event.c /Fe:thread-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop thread build exit $LASTEXITCODE" }
    & ./thread-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop thread test exit $LASTEXITCODE" }
    & "$llvm/clang-cl.exe" /nologo /std:c11 /O2 /W4 /WX test_event.c event.c /Fe:event-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop event build exit $LASTEXITCODE" }
    & ./event-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop event test exit $LASTEXITCODE" }
    foreach ($source in @('event', 'thread', 'test_thread')) {
        & "$llvm/clang.exe" --target=x86_64-pc-freebsd12-elf -march=btver2 -std=c11 -O2 -fPIC -Wall -Wextra -Werror `
            -D__PS4__ -DPS4 -D__ORBIS__ -D_BSD_SOURCE=1 -isysroot $sdk -I../../sdk-bringup/include `
            -isystem "$overlay/include" -isystem "$sdk/include" -include orbis_prefix.h -c "$source.c" -o "$source.o"
        if ($LASTEXITCODE -ne 0) { throw "Orbis build exit $LASTEXITCODE" }
    }
    & "$llvm/ld.lld.exe" -m elf_x86_64 -pie --script "$overlay/cmake/orbis-tls.ld" --eh-frame-hdr --no-rosegment `
        -o thread-check.elf -L "$sdk/lib" "$sdk/lib/crt1.o" event.o thread.o test_thread.o -lc -lkernel -lScePosix
    if ($LASTEXITCODE -ne 0) { throw "Orbis link exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
