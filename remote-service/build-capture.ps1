[CmdletBinding()]
param(
    [string] $Ambilight = (Join-Path $PSScriptRoot 'upstream/PS4-Ambilight-4.0.0'),
    [string] $GoldHenSdk,
    [string] $OpenOrbis = (Join-Path $PSScriptRoot '../gpu-probe/deps/orbis-sdk-v1/sdk'),
    [string] $WindowsOpenOrbis = (Join-Path $PSScriptRoot '../toolchain/OpenOrbis/PS4Toolchain'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [string] $OutputDirectory = (Join-Path $PSScriptRoot 'build/capture')
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE"
    }
}

if (!$GoldHenSdk) {
    $sdkDirectories = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'upstream') -Directory |
        Where-Object Name -Like 'GoldHEN_Plugins_SDK-*')
    if ($sdkDirectories.Count -ne 1) {
        throw 'Supply -GoldHenSdk with the extracted GoldHEN_Plugins_SDK source directory.'
    }
    $GoldHenSdk = $sdkDirectories[0].FullName
}

$ambilightSource = (Resolve-Path -LiteralPath $Ambilight).ProviderPath
$sdkSource = (Resolve-Path -LiteralPath $GoldHenSdk).ProviderPath
$orbis = (Resolve-Path -LiteralPath $OpenOrbis).ProviderPath
$windowsTools = (Resolve-Path -LiteralPath $WindowsOpenOrbis).ProviderPath
$llvmTools = (Resolve-Path -LiteralPath $Llvm).ProviderPath
$output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$work = Join-Path $output 'obj'
$patched = Join-Path $work 'goldhen-sdk'
$shell = Join-Path $ambilightSource 'shell-feed'
$sister = Join-Path $ambilightSource 'plugin'
$common = Join-Path $ambilightSource 'common'
$captureSources = @('capture_frame.c', 'capture_stream.c') | ForEach-Object {
    (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot $_)).ProviderPath
}

# Apply Ambilight's hardware-tested patch to a build-local copy. Keep the
# approved SDK source intact so subsequent builds apply the same patch.
foreach ($directory in 'include', 'source', 'crt') {
    $destination = (New-Item -ItemType Directory -Force -Path (Join-Path $patched $directory)).FullName
    Get-ChildItem -LiteralPath (Join-Path $sdkSource $directory) | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Recurse -Force
    }
}
$patch = Join-Path $ambilightSource 'patches/goldhen-sdk-detour64-fix.patch'
Push-Location -LiteralPath $patched
try {
    Invoke-Checked 'git' @('apply', '--no-index', '--whitespace=nowarn', $patch)
}
finally {
    Pop-Location
}

$compiler = Join-Path $llvmTools 'clang.exe'
$cxx = Join-Path $llvmTools 'clang++.exe'
$systemHeaders = @('-isysroot', $orbis, '-isystem', "$orbis/include")
$orbisFlags = @('--target=x86_64-pc-freebsd12-elf', '-fPIC', '-funwind-tables') + $systemHeaders
$sdkFlags = $orbisFlags + @('-DDEBUG=0', '-D__USE_KLOG__', '-I', "$patched/include")
$crt = Join-Path $work 'crtprx.o'
Invoke-Checked $compiler (@('--target=x86_64-pc-linux-gnu', '-ffreestanding', '-nostdlib',
    '-fno-builtin', '-fPIC') + $systemHeaders + @('-c', "$patched/crt/crtprx.c", '-o', $crt))

$sdkObjects = foreach ($source in Get-ChildItem -LiteralPath "$patched/source" -File | Sort-Object Name) {
    if ($source.Extension -notin '.c', '.cpp') { continue }
    $object = Join-Path $work "sdk-$($source.BaseName).o"
    $tool = if ($source.Extension -eq '.cpp') { $cxx } else { $compiler }
    Invoke-Checked $tool ($sdkFlags + @('-c', $source.FullName, '-o', $object))
    $object
}
$hookLibrary = Join-Path $work 'libGoldHEN_Hook.a'
Invoke-Checked "$llvmTools/llvm-ar.exe" (@('--format=bsd', 'rcs', $hookLibrary, $crt) + $sdkObjects)

@'
#define GIT_COMMIT "PS4-Ambilight-v4.0.0"
#define GIT_VER "pokopia-remote-capture"
#define GIT_NUM 0
#define BUILD_DATE "local build"
'@ | Set-Content -LiteralPath (Join-Path $work 'git_ver.h') -Encoding ascii

# Mirror shell-feed/Makefile's source list and deliberately keep its default
# optimization level for the hook/loader path. Include our capture transport.
$shellFlags = $orbisFlags + @('-D__FINAL__=1', '-D__USE_PRINTF__', '-DPOKOPIA_REMOTE_STREAM=1', '-Wall',
    '-I', "$patched/include", '-I', "$shell/include", '-I', "$sister/include",
    '-I', $common, '-I', $work, '-I', $PSScriptRoot)
$shellSources = @(Get-ChildItem -LiteralPath "$shell/source" -File | Where-Object Extension -In '.c', '.cpp' |
    Sort-Object Name | Select-Object -ExpandProperty FullName)
if ('shell_remote.c' -notin ($shellSources | ForEach-Object { Split-Path -Leaf $_ })) {
    throw 'The shell source directory must contain the Pokopia shell_remote.c integration.'
}
$sharedSources = @('tiling.c', 'pixel_formats.c', 'zones.c', 'gamma.c', 'color_processing.c', 'settings.c', 'config.c') |
    ForEach-Object { Join-Path "$sister/source" $_ }
$sources = $shellSources + $sharedSources + @("$common/plugin_common.c") + $captureSources
$objects = foreach ($source in $sources) {
    $object = Join-Path $work "feed-$([IO.Path]::GetFileNameWithoutExtension($source)).o"
    $tool = if ([IO.Path]::GetExtension($source) -eq '.cpp') { $cxx } else { $compiler }
    $flags = $shellFlags
    if ([IO.Path]::GetFileName($source) -eq 'capture_frame.c') {
        $flags += '-O2'
    }
    # The SDK errno header names the Musl accessor. The compositor's
    # firmware libkernel instead exports the FreeBSD accessor.
    if ([IO.Path]::GetFileName($source) -eq 'capture_stream.c') {
        $flags += '-D__errno_location=__error'
    }
    Invoke-Checked $tool ($flags + @('-c', $source, '-o', $object))
    $object
}

$elf = Join-Path $output 'ps4_shell_feed.elf'
$oelf = Join-Path $output 'ps4_shell_feed.oelf'
$prx = Join-Path $output 'ps4_shell_feed.prx'
Invoke-Checked "$llvmTools/ld.lld.exe" (@('-m', 'elf_x86_64', '-pie', '--script', "$orbis/link.x", '-e', '_init',
    '--eh-frame-hdr', '-L', "$orbis/lib", '-o', $elf, $crt) + $objects +
    @('-lSceLibcInternal', $hookLibrary, '-lkernel', '-lSceSysmodule'))

$previousToolchain = $env:OO_PS4_TOOLCHAIN
try {
    $env:OO_PS4_TOOLCHAIN = $windowsTools
    Invoke-Checked "$windowsTools/bin/windows/create-fself.exe" @(
        "--in=$elf", "--out=$oelf", "--lib=$prx", '--paid', '0x3800000000000011'
    )
}
finally {
    $env:OO_PS4_TOOLCHAIN = $previousToolchain
}

$artifact = Get-Item -LiteralPath $prx
$stream = $artifact.OpenRead()
try {
    $signature = [byte[]]::new(4)
    if ($stream.Read($signature, 0, 4) -ne 4 -or [BitConverter]::ToUInt32($signature, 0) -ne 0x1d3d154f) {
        throw "Invalid PS4 SELF signature in $prx"
    }
}
finally {
    $stream.Dispose()
}
[pscustomobject]@{
    Plugin = $artifact.FullName
    Bytes = $artifact.Length
    SHA256 = (Get-FileHash -LiteralPath $prx -Algorithm SHA256).Hash
}
