[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin'
)

$ErrorActionPreference = 'Stop'
$work = Join-Path $PSScriptRoot 'sdk-bringup'
$openOrbis = Join-Path $PSScriptRoot 'toolchain/OpenOrbis/PS4Toolchain'
& (Join-Path $work 'build-sdk.ps1') -ExeFS $ExeFS -Recompiler $Recompiler -Llvm $Llvm
& python (Join-Path $work 'make-entry.py')
if ($LASTEXITCODE -ne 0) { throw "SDK startup probe generation failed ($LASTEXITCODE)." }

$object = Join-Path $work 'sdk-entry-probe.orbis.o'
& (Join-Path $Llvm 'clang.exe') --driver-mode=g++ --target=x86_64-pc-freebsd12-elf `
    -march=btver2 -mcx16 -std=c++20 -O1 -fPIC -fno-exceptions -fno-rtti -ffp-contract=off `
    -isystem (Join-Path $openOrbis 'include/c++/v1') -isystem (Join-Path $openOrbis 'include') `
    -I (Join-Path $work 'include') -c (Join-Path $work 'sdk_entry.cpp') -o $object
if ($LASTEXITCODE -ne 0) { throw "SDK startup probe compilation failed ($LASTEXITCODE)." }
& (Join-Path $PSScriptRoot 'build-loader-probe.ps1') -ExeFS $ExeFS -Recompiler $Recompiler -SdkProbeObject $object
