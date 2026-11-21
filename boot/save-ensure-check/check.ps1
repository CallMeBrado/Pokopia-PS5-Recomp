$ErrorActionPreference = 'Stop'
$probeDir = $PSScriptRoot
$env:TEMP = $probeDir
$env:TMP = $probeDir
$vc = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC'
$llvm = "$vc/Tools/Llvm/x64/bin"
$msvc = "$vc/Tools/MSVC/14.50.35717"
$kits = 'C:/Program Files (x86)/Windows Kits/10'
$kitVersion = '10.0.26100.0'
$env:LIB = "$msvc/lib/x64;$kits/Lib/$kitVersion/ucrt/x64;$kits/Lib/$kitVersion/um/x64"
$env:INCLUDE = "$msvc/include;$kits/Include/$kitVersion/ucrt;$kits/Include/$kitVersion/shared;$kits/Include/$kitVersion/um"
$env:PATH = "$msvc/bin/Hostx64/x64;$env:PATH"
$bundle = 'E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
$sdk = "$bundle/sdk"
$overlay = "$bundle/orbis-compat"

Push-Location $probeDir
try {
    & "$llvm/clang-cl.exe" /nologo /std:c11 /O2 /W4 /WX overlay/horizon/save_store.c overlay/horizon/cmif.c overlay/gfx-bridge/fsp.c ../../gfx-bridge/romfs.c check.c /Fe:save-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop build exit $LASTEXITCODE" }
    & ./save-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop test exit $LASTEXITCODE" }
    foreach ($source in @('save_store', 'link')) {
        & "$llvm/clang.exe" --target=x86_64-pc-freebsd12-elf -march=btver2 -std=c11 -O2 -fPIC -Wall -Wextra -Werror `
            -D__PS4__ -DPS4 -D__ORBIS__ -D_BSD_SOURCE=1 -isysroot $sdk `
            -isystem "$overlay/include" -isystem "$sdk/include" -include orbis_prefix.h -I "$probeDir" -c "$source.c" -o "$source.o"
        if ($LASTEXITCODE -ne 0) { throw "Orbis build exit $LASTEXITCODE" }
    }
    & "$llvm/ld.lld.exe" -m elf_x86_64 -pie --script "$overlay/cmake/orbis-tls.ld" --eh-frame-hdr --no-rosegment `
        -o save-check.elf -L "$sdk/lib" "$sdk/lib/crt1.o" save_store.o link.o -lc -lkernel
    if ($LASTEXITCODE -ne 0) { throw "Orbis link exit $LASTEXITCODE" }
} finally {
    Pop-Location
}

