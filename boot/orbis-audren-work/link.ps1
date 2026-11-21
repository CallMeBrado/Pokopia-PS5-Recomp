$ErrorActionPreference = 'Stop'
$probeDir = $PSScriptRoot
$bundle = 'E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
$lib = "$bundle/sdk/lib"
$overlay = "$bundle/orbis-compat"
$lld = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin/ld.lld.exe'

Push-Location $probeDir
try {
    & ./compile.ps1 -Source link-probe.cpp -Object link-probe.o
    if ($LASTEXITCODE -ne 0) { throw "compile exit $LASTEXITCODE" }
    & $lld -m elf_x86_64 -pie --script "$overlay/cmake/orbis-tls.ld" --eh-frame-hdr --no-rosegment `
        -o pcm-stream-orbis.elf -L $lib "$lib/crt1.o" link-probe.o ps4-pcm-stream.o ps4-pcm-native.o `
        compile-probe.o orbis-audio-device.o --whole-archive "$overlay/build/liborbis-compat.a" --no-whole-archive `
        --start-group '-lc++abi' '-lc++' -lunwind -lSceAudioOut -lSceUserService -lc -lkernel --end-group
    if ($LASTEXITCODE -ne 0) { throw "link exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
