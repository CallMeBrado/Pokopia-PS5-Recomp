[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Source,
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Llvm,
    [Parameter(Mandatory)] [string] $Sdk,
    [string] $CMakeTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake'
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Build | Out-Null
$env:TEMP = (Resolve-Path -LiteralPath $Build).ProviderPath
$env:TMP = $env:TEMP
$cmake = Join-Path $CMakeTools 'CMake/bin/cmake.exe'
$ninja = Join-Path $CMakeTools 'Ninja/ninja.exe'
$sdkInclude = Join-Path $Sdk 'include'
& $cmake -S $Source -B $Build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_VERSION=12 `
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
    "-DCMAKE_C_COMPILER=$Llvm/clang.exe" -DCMAKE_C_COMPILER_TARGET=x86_64-pc-freebsd12-elf `
    "-DCMAKE_C_COMPILER_AR=$Llvm/llvm-ar.exe" "-DCMAKE_C_COMPILER_RANLIB=$Llvm/llvm-ranlib.exe" `
    "-DCMAKE_AR=$Llvm/llvm-ar.exe" "-DCMAKE_RANLIB=$Llvm/llvm-ranlib.exe" "-DCMAKE_SYSROOT=$Sdk" `
    "-DCMAKE_C_FLAGS=-march=btver2 -D__ORBIS__ -D__PS4__ -isystem `"$sdkInclude`"" `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON `
    -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF `
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
if ($LASTEXITCODE) { throw "Opus configuration failed ($LASTEXITCODE)." }
& $cmake --build $Build --parallel 6
if ($LASTEXITCODE) { throw "Opus build failed ($LASTEXITCODE)." }
