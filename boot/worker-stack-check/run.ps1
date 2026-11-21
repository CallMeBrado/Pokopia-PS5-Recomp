[CmdletBinding()]
param(
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [switch] $FullGame,
    [switch] $Graphics,
    [switch] $Ps4Memory,
    [switch] $HeadlessPresent,
    [switch] $DebugSymbols,
    [ValidateRange(1, 600)] [int] $TimeoutSeconds = 20,
    [ValidateRange(0, 12288)] [int] $DesktopMemoryMiB,
    [string] $RomfsRoot = 'G:/pokopia_dump_v2-0-1/updates/655360/romfs',
    [string] $ShaderCache
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$originalBoot = Join-Path $root 'boot'
if ($Ps4Memory -and !$Graphics) { throw 'PS4 memory sites are tested with the graphics diagnostic.' }
if ($HeadlessPresent -and !$Graphics) { throw 'Headless present requires the graphics diagnostic.' }
$obj = Join-Path $PSScriptRoot 'obj'
$env:TEMP = $obj
$env:TMP = $obj
$clang = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$dev = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Recompiler 'include'
$inputs = @(
    (Join-Path $root 'loader-probe/loader_probe.c'),
    (Join-Path $root 'cpu-probe/generated/rtld_7700000000.c'),
    (Join-Path $root 'horizon/get_info.c'),
    (Join-Path $root 'horizon/entropy.c'),
    (Join-Path $root 'horizon/sm.c'),
    (Join-Path $root 'horizon/lm.c'),
    (Join-Path $root 'horizon/cmif.c'),
    (Join-Path $root 'horizon/applet.c'),
    (Join-Path $root 'horizon/aoc.c'),
    (Join-Path $root 'horizon/pctl.c'),
    (Join-Path $root 'horizon/time_service.c'),
    (Join-Path $root 'horizon/readonly_page.c'),
    (Join-Path $root 'horizon/heap.c'),
    (Join-Path $root 'horizon/heap_sparse_windows.c'),
    (Join-Path $root 'horizon/stack_map.c'),
    (Join-Path $root 'horizon/heap_stack_alias.c'),
    (Join-Path $root 'horizon/thread.c'),
    (Join-Path $root 'horizon/event.c'),
    (Join-Path $root 'horizon/key_sync.c'),
    (Join-Path $originalBoot 'boot.c'),
    (Join-Path $originalBoot 'host.c')
)
$archives = @((Join-Path $originalBoot 'pokopia-sdk-host-worker.lib'))
$definitions = @('/DPOKOPIA_BOOT_PROBE', '/DPOKOPIA_SPARSE_HEAP')
$linkOptions = @()
if ($DebugSymbols) {
    $definitions += '/Z7'
    $linkOptions += '/link', '/DEBUG:FULL'
}
$includes = @('/I', $include, '/I', (Join-Path $root 'loader-probe'), '/I', (Join-Path $root 'horizon'), '/I', (Join-Path $root 'gfx-bridge'))
$cppInputs = @()
$cppIncludes = @()
if ($Graphics) {
    $archives = @((Join-Path $originalBoot 'pokopia-sdk-host-graphics.lib'))
    $definitions += '/DPOKOPIA_GFX', '/DPOKOPIA_AUDIO_OUT', '/DPOKOPIA_HWOPUS', '/DPOKOPIA_VK_MEMORY_TRACE'
    if ($HeadlessPresent) { $definitions += '/DPOKOPIA_HOST_HEADLESS_PRESENT' }
    $includes += @('/I', $PSScriptRoot, '/I', (Join-Path $root 'gpu-probe/deps/orbis-sdk-v1/mesa/include'))
    $inputs += Join-Path $root 'gfx-bridge/vk_ledger.c'
    foreach ($source in 'vk_adapter.c', 'vk_copy.c', 'vk_program.c', 'vk_texel.c', 'vi_adapter.c', 'nvn_front.c', 'nvn_window.c', 'nvn_replay.c',
                        'shader_cache.c', 'texture_upload.c', 'live_draw.c', 'live_compute.c',
                        'romfs.c') {
        $inputs += Join-Path $root "gfx-bridge/$source"
    }
    $inputs += Join-Path $root 'horizon/stack_source.c'
    $inputs += @(
        (Join-Path $root 'gfx-bridge/account.c'),
        (Join-Path $root 'horizon/save_store.c'),
        (Join-Path $root 'gfx-bridge/fsp.c'),
        (Join-Path $root 'horizon/hid.c'),
        (Join-Path $root 'horizon/live_shared.c'),
        (Join-Path $root 'gfx-bridge/live_texture_upload.c'),
        (Join-Path $root 'horizon/audio_device.c'),
        (Join-Path $root 'horizon/audio_out.c'),
        (Join-Path $root 'horizon/bsd_ssl.c'),
        (Join-Path $root 'horizon/offline_app.c'),
        (Join-Path $root 'horizon/stpl.c'),
        (Join-Path $root 'horizon/hwopus.c')
    )
    $includes += @('/I', (Join-Path $root 'upstream/opus-1.6.1/include'))
    $archives += Join-Path $originalBoot 'aud-u-work/opus-test.lib'
    $pinned = Join-Path $root 'upstream/switch2-recomp-0.2.0'
    $nvn = Join-Path $pinned 'runtime/src/gfx/nvn'
    $cppInputs = @(
        (Join-Path $root 'horizon/ps4-pcm-stream.cpp'),
        (Join-Path $originalBoot 'aud-u-work/ps4-pcm-native-paced-production.cpp'),
        (Join-Path $root 'gfx-bridge/nvn_guest_layout.cpp'),
        (Join-Path $nvn 'nvn_texture_layout.cpp'),
        (Join-Path $nvn 'nvn_format.cpp'),
        (Join-Path $pinned 'tools/tiling/src/tegra_tiling.cpp')
    )
    $cppIncludes = @(
        '/I', (Join-Path $root 'gfx-bridge/nvn-layout-include'),
        '/I', (Join-Path $pinned 'runtime/include'), '/I', $nvn,
        '/I', (Join-Path $nvn 'third_party'), '/I', (Join-Path $pinned 'tools/tiling/include')
    )
    $env:POKOPIA_ROMFS_ROOT = (Resolve-Path -LiteralPath $RomfsRoot).ProviderPath
    $env:POKOPIA_SHADER_CACHE = if ($ShaderCache) { (Resolve-Path -LiteralPath $ShaderCache).ProviderPath } else { $null }
    $env:POKOPIA_DESKTOP_MEMORY_MIB = if ($DesktopMemoryMiB) { "$DesktopMemoryMiB" } else { $null }
    $env:POKOPIA_SAVE_ROOT = Join-Path $PSScriptRoot 'save-probe'
} elseif ($DesktopMemoryMiB) {
    throw 'A desktop memory override requires the graphics diagnostic build.'
}
if ($FullGame -or $Graphics) {
    $mainArchive = if ($Ps4Memory) { 'pokopia-main-ps4-memory-host.lib' } else { 'pokopia-main-host.lib' }
    if ($Ps4Memory) { $archives += Join-Path $originalBoot 'thread-wait-check/obj-shader-overlay/gpu-pool-256-diagnostic.obj' }
    $archives += Join-Path $originalBoot $mainArchive
    $archives += Join-Path $originalBoot 'pokopia-subsdk0-host.lib'
    $definitions += '/DPOKOPIA_BOOT_GAME'
}
$name = if ($Graphics) { 'graphics' } else { 'boot' }
$boot = Join-Path $PSScriptRoot "pokopia-$name-sparse-host.exe"

$devEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment could not be loaded.' }
foreach ($line in $devEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:TEMP = $obj
$env:TMP = $obj
if ($Graphics) {
    $opusDirectory = Join-Path $root 'remote-service/chiaki-ng/1.10.1/chiaki-ng-Win'
    $env:Path = "$opusDirectory;$env:Path"
}
New-Item -ItemType Directory -Force -Path $obj | Out-Null
Push-Location -LiteralPath $obj
try {
    $cppObjects = @()
    foreach ($file in $cppInputs) {
        $object = [IO.Path]::GetFileNameWithoutExtension($file) + '.obj'
        & $clang /nologo /O2 /std:c++20 /EHsc /DNOMINMAX /clang:-march=btver2 /c @includes @cppIncludes $file "/Fo$object"
        if ($LASTEXITCODE -ne 0) { throw "Desktop graphics layout build failed ($LASTEXITCODE)." }
        $cppObjects += $object
    }
    & $clang /nologo /O2 /std:c11 /clang:-march=btver2 @definitions @includes @inputs @cppObjects @archives "/Fe$boot" @linkOptions
    if ($LASTEXITCODE -ne 0) { throw "Desktop boot build failed ($LASTEXITCODE)." }
}
finally {
    Pop-Location
}

$logName = if ($Ps4Memory) { "$name-ps4-memory" } else { $name }
if ($HeadlessPresent) { $logName = "$logName-headless" }
if ($DesktopMemoryMiB) { $logName = "$logName-$DesktopMemoryMiB" }
$log = Join-Path $PSScriptRoot "sparse-$logName.log"
$errors = Join-Path $PSScriptRoot "sparse-$logName.err"
$process = Start-Process -WindowStyle Hidden -FilePath $boot -WorkingDirectory (Join-Path $root 'loader-probe') `
    -PassThru -RedirectStandardOutput $log -RedirectStandardError $errors
if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id
    Get-Content -LiteralPath $log, $errors
    throw "Desktop boot probe exceeded $TimeoutSeconds seconds."
}
Get-Content -LiteralPath $log, $errors
"Desktop probe exited $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Desktop boot probe failed with exit code $($process.ExitCode)."
}
