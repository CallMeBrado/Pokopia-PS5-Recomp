[CmdletBinding()]
param(
    [string] $Toolchain = (Join-Path $PSScriptRoot '../toolchain/OpenOrbis/PS4Toolchain'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [string] $ConsoleAddress = '172.30.0.99'
)

$ErrorActionPreference = 'Stop'
$Toolchain = (Resolve-Path -LiteralPath $Toolchain).ProviderPath
$Llvm = (Resolve-Path -LiteralPath $Llvm).ProviderPath
$env:OO_PS4_TOOLCHAIN = $Toolchain
$env:DOTNET_ROLL_FORWARD = 'Major'
$root = $PSScriptRoot
$stage = Join-Path $root 'build/package-v7'
$capture = Join-Path $root 'build/capture-r6/ps4_shell_feed.prx'
$captureR5 = Join-Path $root 'build/capture-r5/ps4_shell_feed.prx'
$captureR4 = Join-Path $root 'build/capture-r4/ps4_shell_feed.prx'
$captureR3 = Join-Path $root 'build/capture-r3/ps4_shell_feed.prx'
$captureR2 = Join-Path $root 'build/capture/ps4_shell_feed.prx'
$legacyCapture = Join-Path $root 'build/compat/capture-v1.prx'
$control = Join-Path $root 'build/controller/control4free.elf'
$bootstrap = Join-Path $root 'upstream/PS4-Ambilight-4.0.0/payloads/ps4-ambilight.bin'
$controlSource = Join-Path $root 'upstream/Control4Free-1.3.1'
$titleId = 'BREW49061'
$contentId = "IV0000-${titleId}_00-PS4LANSERVICE001"
$bin = Join-Path $Toolchain 'bin/windows'
$pkgTool = Join-Path $bin 'PkgTool.Core.exe'
$lib = Join-Path $Toolchain 'lib'
$sample = Join-Path $Toolchain 'samples/hello_world'

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE." }
}

foreach ($inputFile in $capture, $captureR5, $captureR4, $captureR3, $captureR2, $legacyCapture, $control, $bootstrap) {
    if (!(Test-Path -LiteralPath $inputFile -PathType Leaf)) { throw "Missing package component $inputFile." }
}
$loaderHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash
if ($loaderHash -ne 'bda812ef59a106bdb35a7f8a68995770df570591487242955cb2ec640bab174b') {
    throw 'The compositor loader differs from the pinned v4.0.0 release.'
}
if ((Get-FileHash -LiteralPath $legacyCapture -Algorithm SHA256).Hash -ne
    '9341334c0e8bc7d8a5ae188d8cd46aaaa693b20018b52f6eaa66cf03e9164a88') {
    throw 'The upgrade reference is not the previous PS4 LAN Service capture plugin.'
}
Invoke-Checked 'python' @('-B', (Join-Path $root 'viewer.py'), '--prepare', '--host', $ConsoleAddress)
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Push-Location -LiteralPath $stage
try {
    New-Item -ItemType Directory -Force -Path 'obj', 'sce_sys/about', 'sce_module', 'assets' | Out-Null
    $sources = @((Join-Path $root 'launcher.c'), (Join-Path $controlSource 'launcher/sandbox.c'))
    $objects = @()
    foreach ($source in $sources) {
        $object = 'obj/' + [IO.Path]::GetFileNameWithoutExtension($source) + '.o'
        Invoke-Checked (Join-Path $Llvm 'clang.exe') @(
            '--target=x86_64-pc-freebsd12-elf', '-std=gnu11', '-O2', '-fPIC', '-funwind-tables',
            '-isysroot', $Toolchain, '-isystem', (Join-Path $Toolchain 'include'),
            '-I', (Join-Path $controlSource 'launcher'), '-Wall', '-Wextra', '-Werror',
            '-c', $source, '-o', $object
        )
        $objects += $object
    }
    Invoke-Checked (Join-Path $Llvm 'ld.lld.exe') (@(
        '-m', 'elf_x86_64', '-pie', '--script', (Join-Path $Toolchain 'link.x'),
        '--eh-frame-hdr', '-o', 'obj/lan-service.elf', '-L', $lib, (Join-Path $lib 'crt1.o')
    ) + $objects + @('-lc', '-lkernel', '-lSceSystemService', '-lSceNetCtl'))
    Invoke-Checked (Join-Path $bin 'create-fself.exe') @(
        '--in', 'obj/lan-service.elf', '--out', 'obj/lan-service.oelf',
        '--eboot', 'eboot.bin', '--paid', '0x3800000000000011'
    )

    Copy-Item -LiteralPath (Join-Path $sample 'sce_sys/about/right.sprx') -Destination 'sce_sys/about/right.sprx'
    Copy-Item -LiteralPath (Join-Path $root '../assets/icon0.png') -Destination 'sce_sys/icon0.png'
    foreach ($stub in 'libc.prx', 'libSceFios2.prx') {
        Copy-Item -LiteralPath (Join-Path $sample "sce_module/$stub") -Destination "sce_module/$stub"
    }
    $assets = @(
        $capture, $legacyCapture, $control, $bootstrap,
        (Join-Path $root 'build/settings/client-ip'), (Join-Path $root 'build/settings/view-token')
    )
    foreach ($asset in $assets) { Copy-Item -LiteralPath $asset -Destination 'assets' }
    Copy-Item -LiteralPath $captureR5 -Destination 'assets/capture-v5.prx'
    Copy-Item -LiteralPath $captureR4 -Destination 'assets/capture-v4.prx'
    Copy-Item -LiteralPath $captureR3 -Destination 'assets/capture-v3.prx'
    Copy-Item -LiteralPath $captureR2 -Destination 'assets/capture-v2.prx'

    $sfo = 'sce_sys/param.sfo'
    Invoke-Checked $pkgTool @('sfo_new', $sfo)
    $entries = @(
        @('APP_TYPE', 'Integer', '4', '1'),
        @('APP_VER', 'Utf8', '8', '01.06'),
        @('ATTRIBUTE', 'Integer', '4', '0'),
        @('CATEGORY', 'Utf8', '4', 'gd'),
        @('CONTENT_ID', 'Utf8', '48', $contentId),
        @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
        @('SYSTEM_VER', 'Integer', '4', '0'),
        @('TITLE', 'Utf8', '128', 'PS4 LAN Service'),
        @('TITLE_ID', 'Utf8', '12', $titleId),
        @('VERSION', 'Utf8', '8', '01.06')
    )
    foreach ($entry in $entries) {
        $key, $type, $size, $value = $entry
        Invoke-Checked $pkgTool @('sfo_setentry', $sfo, $key, '--type', $type, '--maxsize', $size, '--value', $value)
    }
    $packageFiles = @(
        'eboot.bin', $sfo, 'sce_sys/about/right.sprx', 'sce_sys/icon0.png',
        'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
        'assets/capture-v5.prx', 'assets/capture-v4.prx', 'assets/capture-v3.prx', 'assets/capture-v2.prx'
    ) + @($assets | ForEach-Object { 'assets/' + (Split-Path -Leaf $_) })
    Invoke-Checked (Join-Path $bin 'create-gp4.exe') @(
        '-out', 'pkg.gp4', "--content-id=$contentId", '--files', ($packageFiles -join ' ')
    )
    Invoke-Checked $pkgTool @('pkg_build', 'pkg.gp4', '.')
    $package = Get-Item -LiteralPath "$contentId.pkg"
    Invoke-Checked $pkgTool @('pkg_validate', $package.FullName)
    [pscustomobject]@{
        Package = $package.FullName
        Bytes = $package.Length
        SHA256 = (Get-FileHash -LiteralPath $package.FullName -Algorithm SHA256).Hash
    } | Format-List
}
finally {
    Pop-Location
}
