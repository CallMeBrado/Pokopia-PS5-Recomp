$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).ProviderPath
$toolchain = Join-Path $root 'toolchain/OpenOrbis/PS4Toolchain'
$compat = Join-Path $root 'gpu-probe/deps/orbis-sdk-v1/orbis-compat'
$llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin'
$tools = Join-Path $toolchain 'bin/windows'
$lib = Join-Path $toolchain 'lib'
$sample = Join-Path $toolchain 'samples/hello_world'
$build = Join-Path $PSScriptRoot 'build'
$env:OO_PS4_TOOLCHAIN = $toolchain
$env:DOTNET_ROLL_FORWARD = 'Major'

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE) { throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE" }
}

$null = New-Item -ItemType Directory -Force -Path $build
Push-Location -LiteralPath $build
try {
    $null = New-Item -ItemType Directory -Force -Path 'obj','sce_sys/about','sce_module'
    $env:TEMP = (Resolve-Path -LiteralPath 'obj').ProviderPath
    $env:TMP = $env:TEMP
    Copy-Item -LiteralPath (Join-Path $root 'assets/icon0.png') -Destination 'sce_sys/icon0.png'
    Copy-Item -LiteralPath (Join-Path $sample 'sce_sys/about/right.sprx') -Destination 'sce_sys/about/right.sprx'
    Copy-Item -LiteralPath (Join-Path $sample 'sce_module/libc.prx') -Destination 'sce_module/libc.prx'
    Copy-Item -LiteralPath (Join-Path $sample 'sce_module/libSceFios2.prx') -Destination 'sce_module/libSceFios2.prx'

    Invoke-Checked (Join-Path $llvm 'clang.exe') @(
        '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-std=c11', '-O2', '-fPIC',
        '-funwind-tables', '-Wall', '-Wextra', '-Werror',
        '-isystem', (Join-Path $compat 'include'), '-isystem', (Join-Path $toolchain 'include'),
        '-c', (Join-Path $PSScriptRoot 'main.c'), '-o', 'obj/main.o'
    )
    Invoke-Checked (Join-Path $llvm 'ld.lld.exe') @(
        '-m', 'elf_x86_64', '-pie', '--script', (Join-Path $toolchain 'link.x'),
        '--eh-frame-hdr', '-o', 'obj/memory-proof-r4.elf', '-L', $lib,
        (Join-Path $lib 'crt1.o'), 'obj/main.o', '-lc', '-lkernel'
    )
    Invoke-Checked (Join-Path $tools 'create-fself.exe') @(
        '--in', 'obj/memory-proof-r4.elf', '--out', 'obj/memory-proof-r4.oelf',
        '--eboot', 'eboot.bin', '--paid', '0x3800000000000011'
    )

    $pkg = Join-Path $tools 'PkgTool.Core.exe'
    $sfo = 'sce_sys/param.sfo'
    $titleId = 'BREW49151'
    $contentId = 'IV0000-BREW49151_00-POKOPIAPROBE0201'
    Invoke-Checked $pkg @('sfo_new', $sfo)
    $entries = @(
        @('APP_TYPE', 'Integer', '4', '1'),
        @('APP_VER', 'Utf8', '8', '01.00'),
        @('ATTRIBUTE', 'Integer', '4', '0'),
        @('CATEGORY', 'Utf8', '4', 'gd'),
        @('CONTENT_ID', 'Utf8', '48', $contentId),
        @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
        @('SYSTEM_VER', 'Integer', '4', '0'),
        @('TITLE', 'Utf8', '128', 'Pokopia PS4 Memory Probe R4'),
        @('TITLE_ID', 'Utf8', '12', $titleId),
        @('VERSION', 'Utf8', '8', '01.00')
    )
    foreach ($entry in $entries) {
        $key, $type, $size, $value = $entry
        Invoke-Checked $pkg @('sfo_setentry', $sfo, $key, '--type', $type, '--maxsize', $size, '--value', $value)
    }
    $files = @(
        'eboot.bin', $sfo, 'sce_sys/about/right.sprx', 'sce_sys/icon0.png',
        'sce_module/libc.prx', 'sce_module/libSceFios2.prx'
    )
    Invoke-Checked (Join-Path $tools 'create-gp4.exe') @(
        '-out', 'pkg.gp4', "--content-id=$contentId", '--files', ($files -join ' ')
    )
    Invoke-Checked $pkg @('pkg_build', 'pkg.gp4', '.')
    $output = Get-Item -LiteralPath "$contentId.pkg"
    Invoke-Checked $pkg @('pkg_validate', $output.FullName)
    [pscustomobject]@{
        Package = $output.FullName
        Bytes = $output.Length
        SHA256 = (Get-FileHash -LiteralPath $output.FullName -Algorithm SHA256).Hash
    } | Format-List
} finally {
    Pop-Location
}
