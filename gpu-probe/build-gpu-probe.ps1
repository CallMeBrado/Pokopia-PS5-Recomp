[CmdletBinding()]
param(
    [string] $ShaderWork = 'G:/Pokemon Pokopia [0400A3A01EE4C000]/0400A3A01EE4C000-work/shaders-20261106',
    [string] $Bundle = (Join-Path $PSScriptRoot 'deps/orbis-sdk-v1'),
    [string] $WindowsOpenOrbis = (Join-Path $PSScriptRoot '../toolchain/OpenOrbis/PS4Toolchain'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin'
)

$ErrorActionPreference = 'Stop'
function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE"
    }
}

$bundle = (Resolve-Path -LiteralPath $Bundle).ProviderPath
$sdk = Join-Path $bundle 'sdk'
$overlay = Join-Path $bundle 'orbis-compat'
$mesa = Join-Path $bundle 'mesa'
$radv = Join-Path $mesa 'build-orbis'
$windowsTools = (Resolve-Path -LiteralPath $WindowsOpenOrbis).ProviderPath
$sample = Join-Path $windowsTools 'samples/hello_world'
$env:OO_PS4_TOOLCHAIN = $windowsTools
$env:DOTNET_ROLL_FORWARD = 'Major'

Push-Location -LiteralPath $PSScriptRoot
try {
    $stage = 'generated'
    Invoke-Checked 'python' @('-B', 'embed-fixture.py', $ShaderWork, "$stage/fixture-data.h")
    $compileFlags = @(
        '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-std=c11',
        '-D__PS4__', '-DPS4', '-D__ORBIS__', '-D_BSD_SOURCE=1',
        '-O2', '-fPIC', '-funwind-tables', '-isysroot', $sdk,
        '-isystem', "$overlay/include", '-isystem', "$sdk/include",
        '-isystem', "$mesa/include", '-include', 'orbis_prefix.h', '-I', $stage
    )
    $objects = foreach ($source in @('composite.c', "$overlay/vkloader/vkloader.c", "$overlay/vkloader/vkthunks.c")) {
        $object = "$stage/$([IO.Path]::GetFileNameWithoutExtension($source)).o"
        Invoke-Checked "$Llvm/clang.exe" ($compileFlags + @('-c', $source, '-o', $object))
        $object
    }
    $elf = "$stage/composite.elf"
    $linkFlags = @(
        '-m', 'elf_x86_64', '-pie', '--script', "$overlay/cmake/orbis-tls.ld",
        '--eh-frame-hdr', '--no-rosegment', '-o', $elf, '-L', "$sdk/lib",
        '--whole-archive', "$overlay/build/liborbis-compat.a", '--no-whole-archive'
    )
    $libraries = @(
        '--whole-archive', "$radv/src/amd/vulkan/libvulkan_radeon.a", '--no-whole-archive',
        "$radv/subprojects/zlib-1.3.1/libz.a", '-lSceGnmDriver', '-lSceVideoOut',
        '-lc', '-lkernel', '-lc++', "$sdk/lib/crt1.o"
    )
    Invoke-Checked "$Llvm/ld.lld.exe" ($linkFlags + $objects + $libraries)

    $elf = (Resolve-Path -LiteralPath $elf).ProviderPath
    New-Item -ItemType Directory -Force -Path "$stage/package/sce_sys/about", "$stage/package/sce_module" | Out-Null
    Push-Location -LiteralPath "$stage/package"
    try {
        $packTools = Join-Path $windowsTools 'bin/windows'
        $pkgTool = Join-Path $packTools 'PkgTool.Core.exe'
        Invoke-Checked "$packTools/create-fself.exe" @(
            '--in', $elf, '--out', 'composite.oelf', '--eboot', 'eboot.bin', '--paid', '0x3800000000000011'
        )
        $files = @('sce_sys/about/right.sprx', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx')
        foreach ($file in $files) {
            Copy-Item -LiteralPath (Join-Path $sample $file) -Destination $file
        }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../assets/icon0.png') -Destination 'sce_sys/icon0.png'
        $files += 'sce_sys/icon0.png'
        $sfo = 'sce_sys/param.sfo'
        $contentId = 'IV0000-BREW48994_00-POKOPIAPROBE0004'
        Invoke-Checked $pkgTool @('sfo_new', $sfo)
        $entries = @(
            @('APP_TYPE', 'Integer', '4', '1'),
            @('APP_VER', 'Utf8', '8', '01.00'),
            @('ATTRIBUTE', 'Integer', '4', '0'),
            @('CATEGORY', 'Utf8', '4', 'gd'),
            @('CONTENT_ID', 'Utf8', '48', $contentId),
            @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
            @('SYSTEM_VER', 'Integer', '4', '0'),
            @('TITLE', 'Utf8', '128', 'Pokopia PS4 GPU Probe'),
            @('TITLE_ID', 'Utf8', '12', 'BREW48994'),
            @('VERSION', 'Utf8', '8', '01.00')
        )
        foreach ($entry in $entries) {
            $key, $type, $size, $value = $entry
            Invoke-Checked $pkgTool @('sfo_setentry', $sfo, $key, '--type', $type, '--maxsize', $size, '--value', $value)
        }
        $files += @('eboot.bin', $sfo)
        Invoke-Checked "$packTools/create-gp4.exe" @('-out', 'pkg.gp4', "--content-id=$contentId", '--files', ($files -join ' '))
        Invoke-Checked $pkgTool @('pkg_build', 'pkg.gp4', '.')
        $pkg = Get-Item -LiteralPath "$contentId.pkg"
        Invoke-Checked $pkgTool @('pkg_validate', $pkg.FullName)
        [pscustomobject]@{
            Package = $pkg.FullName
            Bytes = $pkg.Length
            SHA256 = (Get-FileHash -LiteralPath $pkg.FullName -Algorithm SHA256).Hash
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    Pop-Location
}
