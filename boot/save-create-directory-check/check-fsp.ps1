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
    & "$llvm/clang-cl.exe" /nologo /std:c11 /O2 /W4 /WX ../../horizon/save_store.c ../../horizon/cmif.c ../../gfx-bridge/fsp.c ../../gfx-bridge/romfs.c check-fsp.c /Fe:save-fsp-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop build exit $LASTEXITCODE" }
    & ./save-fsp-check.exe
    if ($LASTEXITCODE -ne 0) { throw "Desktop test exit $LASTEXITCODE" }
} finally {
    Pop-Location
}

