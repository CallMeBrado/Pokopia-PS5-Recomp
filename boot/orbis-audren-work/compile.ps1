param(
    [string] $Source = 'compile-probe.cpp',
    [string] $Object = 'compile-probe.o'
)
$ErrorActionPreference='Stop'
$probeDir=$PSScriptRoot
$probeClang='C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe'
$probeBundle='E:/Pokopia-PS4/gpu-probe/deps/orbis-sdk-v1'
$probeSdk="$probeBundle/sdk"
$probeOverlay="$probeBundle/orbis-compat"
$probePinned='E:/Pokopia-PS4/upstream/switch2-recomp-0.2.0/runtime'
$probeFlags=@('--target=x86_64-pc-freebsd12-elf','-march=btver2','-std=c++20','-O2','-fPIC','-funwind-tables','-fno-exceptions','-fno-rtti',
 '-D__PS4__','-DPS4','-D__ORBIS__','-D_BSD_SOURCE=1','-isysroot',$probeSdk,'-isystem',"$probeSdk/include/c++/v1",
 '-isystem',"$probeOverlay/include",'-isystem',"$probeSdk/include",'-include','orbis_prefix.h','-I',"$probePinned/include",'-I',$probeDir,'-I','E:/Pokopia-PS4/sdk-bringup/include')
$env:TEMP=$probeDir
$env:TMP=$probeDir
& $probeClang @probeFlags -ferror-limit=20 -c (Join-Path $probeDir $Source) -o (Join-Path $probeDir $Object)
exit $LASTEXITCODE
