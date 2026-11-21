$ErrorActionPreference = 'Stop'
$probeDir = $PSScriptRoot
$env:TEMP = $probeDir
$env:TMP = $probeDir
$sdk = 'E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1/sdk/include'
$vs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC'
$cl = "$vs/Tools/Llvm/x64/bin/clang-cl.exe"
$msvc = "$vs/Tools/MSVC/14.50.35717"
$kits = 'C:/Program Files (x86)/Windows Kits/10'
$winVer = '10.0.26100.0'
$env:LIB = "$msvc/lib/x64;$kits/Lib/$winVer/ucrt/x64;$kits/Lib/$winVer/um/x64"
$env:INCLUDE = "$msvc/include;$kits/Include/$winVer/ucrt;$kits/Include/$winVer/shared;$kits/Include/$winVer/um"
$env:PATH = "$msvc/bin/Hostx64/x64;$env:PATH"

Push-Location $probeDir
try {
    & $cl /nologo /std:c++20 /O2 /EHsc /W4 /WX "/clang:-idirafter" "/clang:$sdk" `
        ps4-pcm-stream.cpp ps4-pcm-native.cpp test-pcm-stream.cpp /Fe:test-pcm-stream.exe
    if ($LASTEXITCODE -ne 0) { throw "compile/link exit $LASTEXITCODE" }
    & ./test-pcm-stream.exe
    if ($LASTEXITCODE -ne 0) { throw "test exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
