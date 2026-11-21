[CmdletBinding()]
param(
    [string] $Toolchain = (Join-Path $PSScriptRoot 'OpenOrbis/PS4Toolchain'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [string[]] $ProbeSource,
    [string[]] $ProbeInclude,
    [string[]] $ProbeCppSource,
    [string[]] $ProbeCppInclude,
    [string] $LoaderAssets,
    [string] $ShaderAssets,
    [string] $SdkProbeObject,
    [string[]] $BootLibraries,
    [string] $VulkanBundle,
    [switch] $AudioOutput,
    [switch] $TraceMemory,
    [switch] $SparseHeap,
    [switch] $LinkOnly,
    [string] $OpusSource,
    [ValidateRange(0, 96)] [int] $GameBootRevision = 0,
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
if ($GameBootRevision -and !$VulkanBundle) { throw 'The game boot package requires the Vulkan backend.' }
if ($ProbeCppSource -and !$VulkanBundle) { throw 'Graphics C++ sources require the bundled Orbis SDK.' }
if ($ShaderAssets -and !$VulkanBundle) { throw 'Packaged shaders require the Vulkan backend.' }
if ($AudioOutput -and !$VulkanBundle) { throw 'Native audio output requires the bundled Orbis SDK.' }
if ($OpusSource -and !$AudioOutput) { throw 'Opus support requires native audio output.' }
if ($TraceMemory -and !$VulkanBundle) { throw 'Memory tracing requires the Vulkan backend.' }
if ($SparseHeap -and !$VulkanBundle) { throw 'Sparse heap testing requires the Vulkan backend.' }
$env:DOTNET_ROLL_FORWARD = 'Major'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'out'
}

$toolchain = (Resolve-Path -LiteralPath $Toolchain).ProviderPath
$llvm = (Resolve-Path -LiteralPath $Llvm).ProviderPath
$env:OO_PS4_TOOLCHAIN = $toolchain
$clang = Join-Path $llvm 'clang.exe'
$clangCpp = Join-Path $llvm 'clang++.exe'
$linker = Join-Path $llvm 'ld.lld.exe'
$bin = Join-Path $toolchain 'bin/windows'
$makeSelf = Join-Path $bin 'create-fself.exe'
$makeGp4 = Join-Path $bin 'create-gp4.exe'
$pkgTool = Join-Path $bin 'PkgTool.Core.exe'
$sample = Join-Path $toolchain 'samples/hello_world'
$libRoot = Join-Path $toolchain 'lib'
$linkScript = Join-Path $toolchain 'link.x'

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE"
    }
}

$sourceFiles = @((Join-Path $PSScriptRoot 'main.c'))
$compileFlags = @(
    '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-std=c11',
    '-O2', '-fPIC', '-funwind-tables', '-isystem', (Join-Path $toolchain 'include')
)
foreach ($include in $ProbeInclude) {
    $compileFlags += @('-I', (Resolve-Path -LiteralPath $include).ProviderPath)
}
if ($LoaderAssets) {
    $loaderAssetDirectory = (Resolve-Path -LiteralPath $LoaderAssets).ProviderPath
    $compileFlags += '-DPOKOPIA_LOADER_PROBE'
}
if ($SdkProbeObject) {
    if (!$LoaderAssets -or !$ProbeSource) {
        throw 'SDK probes require loader assets and source.'
    }
    $sdkObjectPath = (Resolve-Path -LiteralPath $SdkProbeObject).ProviderPath
    $compileFlags += '-DPOKOPIA_SDK_PROBE'
}
if ($BootLibraries) {
    if (!$LoaderAssets -or !$ProbeSource -or $SdkProbeObject -or $BootLibraries.Count -notin @(1, 3)) {
        throw 'Boot probes require loader assets, source, and the SDK or all three translated archives.'
    }
    $bootPaths = @($BootLibraries | ForEach-Object { (Resolve-Path -LiteralPath $_).ProviderPath })
    $compileFlags += '-DPOKOPIA_BOOT_PROBE'
    if ($BootLibraries.Count -eq 3) { $compileFlags += '-DPOKOPIA_BOOT_GAME' }
}
if ($VulkanBundle) {
    if ($BootLibraries.Count -ne 3) { throw 'Vulkan boot requires the full game archives.' }
    $bundle = (Resolve-Path -LiteralPath $VulkanBundle).ProviderPath
    $sdk = Join-Path $bundle 'sdk'
    $overlay = Join-Path $bundle 'orbis-compat'
    $mesa = Join-Path $bundle 'mesa'
    $radv = Join-Path $mesa 'build-orbis'
    $libRoot = Join-Path $sdk 'lib'
    $linkScript = Join-Path $overlay 'cmake/orbis-tls.ld'
    $orbisFlags = @(
        '-D__PS4__', '-DPS4', '-D__ORBIS__', '-D_BSD_SOURCE=1', '-isysroot', $sdk,
        '-isystem', (Join-Path $overlay 'include'), '-isystem', (Join-Path $sdk 'include'),
        '-isystem', (Join-Path $mesa 'include'), '-include', 'orbis_prefix.h'
    )
    $compileFlags += @('-DPOKOPIA_GFX', '-isystem', (Join-Path $mesa 'include'))
    if ($TraceMemory) { $compileFlags += @('-DPOKOPIA_ORBIS_MEMORY_TRACE', '-DPOKOPIA_VK_MEMORY_TRACE') }
    if ($SparseHeap) { $compileFlags += '-DPOKOPIA_SPARSE_HEAP' }
    if ($GameBootRevision) { $compileFlags += "-DPOKOPIA_GAME_BOOT=$GameBootRevision" }
}
if ($ProbeSource) {
    $compileFlags += '-DPOKOPIA_GUEST_PROBE'
    foreach ($file in $ProbeSource) {
        $sourceFiles += (Resolve-Path -LiteralPath $file).ProviderPath
    }
}
if ($AudioOutput) { $compileFlags += '-DPOKOPIA_AUDIO_OUT' }
if ($OpusSource) {
    $opus = (Resolve-Path -LiteralPath $OpusSource).ProviderPath
    $opusBuild = Join-Path (Split-Path -Parent $PSScriptRoot) 'boot/opus-orbis'
    & (Join-Path $PSScriptRoot 'build-opus.ps1') -Source $opus -Build $opusBuild -Llvm $llvm -Sdk $sdk
    $opusLibrary = Join-Path $opusBuild 'libopus.a'
    $compileFlags += @('-DPOKOPIA_HWOPUS', '-I', (Join-Path $opus 'include'))
}
$cppFiles = @(foreach ($file in $ProbeCppSource) { (Resolve-Path -LiteralPath $file).ProviderPath })
$cppIncludes = @(foreach ($include in $ProbeCppInclude) { (Resolve-Path -LiteralPath $include).ProviderPath })

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Push-Location -LiteralPath $OutputDirectory
try {
    New-Item -ItemType Directory -Force -Path 'obj' | Out-Null
    $env:TEMP = (Resolve-Path -LiteralPath 'obj').ProviderPath
    $env:TMP = $env:TEMP
    if (!$LinkOnly) {
        New-Item -ItemType Directory -Force -Path @('sce_sys/about', 'sce_module') | Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../../../assets/icon0.png') -Destination 'sce_sys/icon0.png'
        Copy-Item -LiteralPath (Join-Path $sample 'sce_sys/about/right.sprx') -Destination 'sce_sys/about/right.sprx'
        Copy-Item -LiteralPath (Join-Path $sample 'sce_module/libc.prx') -Destination 'sce_module/libc.prx'
        Copy-Item -LiteralPath (Join-Path $sample 'sce_module/libSceFios2.prx') -Destination 'sce_module/libSceFios2.prx'
    }

    $objects = @()
    foreach ($file in $sourceFiles) {
        $object = 'obj/source{0}.o' -f $objects.Count
        $flags = $compileFlags
        if ((Split-Path -Leaf $file) -in @('thread.c', 'heap_sparse_orbis.c')) {
            $signalOverlay = if ($VulkanBundle) { $overlay } else {
                Join-Path $PSScriptRoot '../gpu-probe/deps/orbis-sdk-v1/orbis-compat'
            }
            $flags = @('-isystem', (Join-Path $signalOverlay 'include')) + $flags
        }
        Invoke-Checked $clang ($flags + @('-c', $file, '-o', $object))
        $objects += $object
    }
    if ($cppFiles.Count) {
        $cppFlags = @(
            '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-std=c++20',
            '-O2', '-fPIC', '-funwind-tables', '-fno-exceptions', '-fno-rtti',
            '-isystem', (Join-Path $sdk 'include/c++/v1')
        ) + $orbisFlags
        foreach ($include in $cppIncludes) { $cppFlags += @('-I', $include) }
        foreach ($file in $cppFiles) {
            $object = 'obj/source{0}.o' -f $objects.Count
            Invoke-Checked $clangCpp ($cppFlags + @('-c', $file, '-o', $object))
            $objects += $object
        }
    }

    if ($SdkProbeObject) {
        $objects += $sdkObjectPath
    }
    if ($VulkanBundle) {
        $vulkanFlags = @(
            '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-std=c11', '-O2', '-fPIC', '-funwind-tables'
        ) + $orbisFlags
        foreach ($source in 'vkloader', 'vkthunks') {
            $object = "obj/$source.o"
            Invoke-Checked $clang ($vulkanFlags + @('-c', (Join-Path $overlay "vkloader/$source.c"), '-o', $object))
            $objects += $object
        }
    }
    $linkArgs = @(
        '-m', 'elf_x86_64', '-pie', '--script', $linkScript,
        '--eh-frame-hdr', '-o', 'obj/pokopia-probe.elf', '-L', $libRoot,
        (Join-Path $libRoot 'crt1.o')
    ) + $objects
    if ($TraceMemory) {
        $linkArgs += @(
            '--wrap=sceKernelAllocateDirectMemory', '--wrap=sceKernelReleaseDirectMemory',
            '--wrap=sceKernelMapDirectMemory', '--wrap=sceKernelMapFlexibleMemory',
            '--wrap=sceKernelMmap', '--wrap=sceKernelMunmap', '--wrap=scePthreadCreate'
        )
    }
    if ($VulkanBundle) {
        $linkArgs += @(
            '--no-rosegment', '--whole-archive', (Join-Path $overlay 'build/liborbis-compat.a'),
            (Join-Path $radv 'src/amd/vulkan/libvulkan_radeon.a'), '--no-whole-archive',
            (Join-Path $radv 'subprojects/zlib-1.3.1/libz.a'), '-lSceGnmDriver', '-lSceVideoOut', '-lScePad'
        )
    }
    if ($AudioOutput) { $linkArgs += '-lSceAudioOut', '-lSceUserService' }
    if ($OpusSource) { $linkArgs += $opusLibrary }
    if ($BootLibraries) {
        $linkArgs += @('--start-group') + $bootPaths + @('-lc++abi', '-lc++', '-lunwind', '-lc', '-lkernel', '--end-group')
    } else {
        $linkArgs += @('-lc', '-lkernel')
    }
    Invoke-Checked $linker $linkArgs
    if ($LinkOnly) {
        Get-Item -LiteralPath 'obj/pokopia-probe.elf'
        return
    }
    Invoke-Checked $makeSelf @(
        '--in', 'obj/pokopia-probe.elf', '--out', 'obj/pokopia-probe.oelf',
        '--eboot', 'eboot.bin', '--paid', '0x3800000000000011'
    )

    $title = 'Pokopia PS4 Memory Probe R3'
    $titleId = 'BREW49150'
    $contentId = 'IV0000-BREW49150_00-POKOPIAPROBE0200'
    if ($LoaderAssets) {
        $title = 'Pokopia PS4 Loader Probe'
        $titleId = 'BREW48992'
        $contentId = 'IV0000-BREW48992_00-POKOPIAPROBE0002'
    }
    if ($SdkProbeObject) {
        $title = 'Pokopia PS4 SDK Probe'
        $titleId = 'BREW48993'
        $contentId = 'IV0000-BREW48993_00-POKOPIAPROBE0003'
    }
    if ($BootLibraries) {
        $title = 'Pokopia PS4 Worker Probe'
        $titleId = 'BREW48999'
        $contentId = 'IV0000-BREW48999_00-POKOPIAPROBE0009'
    }
    if ($VulkanBundle) {
        $title = 'Pokopia PS4 Graphics Boot'
        $titleId = 'BREW49000'
        $contentId = 'IV0000-BREW49000_00-POKOPIAPROBE0010'
    }
    if ($GameBootRevision) {
        $title = if ($GameBootRevision -eq 1) { 'Pokopia PS4 Game Boot' } else { "Pokopia PS4 Game Boot R$GameBootRevision" }
        $titleId = 'BREW{0}' -f (49003 + $GameBootRevision)
        $contentId = 'IV0000-{0}_00-POKOPIAPROBE{1:0000}' -f $titleId, (13 + $GameBootRevision)
    }
    $sfo = 'sce_sys/param.sfo'
    Invoke-Checked $pkgTool @('sfo_new', $sfo)
    $entries = @(
        @('APP_TYPE', 'Integer', '4', '1'),
        @('APP_VER', 'Utf8', '8', '01.00'),
        @('ATTRIBUTE', 'Integer', '4', '0'),
        @('CATEGORY', 'Utf8', '4', 'gd'),
        @('CONTENT_ID', 'Utf8', '48', $contentId),
        @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
        @('SYSTEM_VER', 'Integer', '4', '0'),
        @('TITLE', 'Utf8', '128', $title),
        @('TITLE_ID', 'Utf8', '12', $titleId),
        @('VERSION', 'Utf8', '8', '01.00')
    )
    foreach ($entry in $entries) {
        $key, $type, $size, $value = $entry
        Invoke-Checked $pkgTool @('sfo_setentry', $sfo, $key, '--type', $type, '--maxsize', $size, '--value', $value)
    }

    $packageFiles = @(
        'eboot.bin', $sfo, 'sce_sys/about/right.sprx', 'sce_sys/icon0.png',
        'sce_module/libc.prx', 'sce_module/libSceFios2.prx'
    )
    $packageDirectories = @()
    if ($OpusSource) {
        Copy-Item -LiteralPath (Join-Path $opus 'COPYING') -Destination 'opus-license.txt'
        $packageFiles += 'opus-license.txt'
    }
    if ($LoaderAssets) {
        $packageDirectories += 'modules'
        New-Item -ItemType Directory -Force -Path 'modules' | Out-Null
        foreach ($asset in 'main.bin', 'sdk.bin', 'subsdk0.bin', 'rtld.bin') {
            Copy-Item -LiteralPath (Join-Path $loaderAssetDirectory $asset) -Destination "modules/$asset"
            $packageFiles += "modules/$asset"
        }
    }
    if ($ShaderAssets) {
        $shaderDirectory = (Resolve-Path -LiteralPath $ShaderAssets).ProviderPath
        $manifest = Get-Item -LiteralPath (Join-Path $shaderDirectory 'results.tsv')
        $shaders = @(Get-ChildItem -LiteralPath $shaderDirectory -File |
            Where-Object { $_.Name -cmatch '^[0-9a-f]{16}\.spv$' })
        if (!$shaders.Count -or $manifest.PSIsContainer) { throw 'Invalid shader cache directory.' }
        $packageDirectories += 'shaders'
        New-Item -ItemType Directory -Force -Path 'shaders' | Out-Null
        foreach ($asset in @($manifest) + $shaders) {
            Copy-Item -LiteralPath $asset.FullName -Destination "shaders/$($asset.Name)"
            $packageFiles += "shaders/$($asset.Name)"
        }
    }
    Invoke-Checked $makeGp4 @('-out', 'pkg.gp4', "--content-id=$contentId", '--files', ($packageFiles -join ' '))
    if ($packageDirectories.Count) {
        $projectPath = (Resolve-Path -LiteralPath 'pkg.gp4').ProviderPath
        [xml] $project = Get-Content -Raw -LiteralPath $projectPath
        foreach ($directory in $packageDirectories) {
            $entry = $project.CreateElement('dir')
            $entry.SetAttribute('targ_name', $directory)
            [void] $project.psproject.rootdir.AppendChild($entry)
        }
        $project.Save($projectPath)
    }
    Invoke-Checked $pkgTool @('pkg_build', 'pkg.gp4', '.')
    $package = Get-Item -LiteralPath "$contentId.pkg"
    Invoke-Checked $pkgTool @('pkg_validate', $package.FullName)
    [pscustomobject]@{
        Package = $package.FullName
        Bytes = $package.Length
        SHA256 = (Get-FileHash -LiteralPath $package.FullName -Algorithm SHA256).Hash
    }
}
finally {
    Pop-Location
}





