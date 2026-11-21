[CmdletBinding()]
param(
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [switch] $FullGame,
    [switch] $Graphics,
    [switch] $Ps4Memory,
    [switch] $HeadlessPresent,
    [switch] $SparseHeap,
    [switch] $DebugSymbols,
    [ValidateRange(1, 600)] [int] $TimeoutSeconds = 20,
    [ValidateRange(0, 12288)] [int] $DesktopMemoryMiB,
    [string] $RomfsRoot = 'G:/pokopia_dump_v2-0-1/updates/655360/romfs',
    [string] $ShaderCache
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Ps4Memory -and !$Graphics) { throw 'PS4 memory sites are tested with the graphics diagnostic.' }
if ($HeadlessPresent -and !$Graphics) { throw 'Headless present requires the graphics diagnostic.' }
$obj = Join-Path $PSScriptRoot $(if ($SparseHeap) { 'obj-host-sparse' } else { 'obj-host' })
New-Item -ItemType Directory -Force -Path $obj | Out-Null
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
    (Join-Path $root 'horizon/live_shared.c'),
    (Join-Path $root 'horizon/heap.c'),
    (Join-Path $root 'horizon/transfer_memory.c'),
    (Join-Path $root 'horizon/stack_map.c'),
    (Join-Path $root 'horizon/heap_stack_alias.c'),
    (Join-Path $root 'horizon/thread.c'),
    (Join-Path $root 'horizon/event.c'),
    (Join-Path $root 'horizon/key_sync.c'),
    (Join-Path $PSScriptRoot 'boot.c'),
    (Join-Path $PSScriptRoot 'host.c')
)
$archives = @((Join-Path $PSScriptRoot 'pokopia-sdk-host-worker.lib'))
$definitions = @('/DPOKOPIA_BOOT_PROBE')
if ($SparseHeap) {
    $definitions += '/DPOKOPIA_SPARSE_HEAP'
    $inputs += Join-Path $root 'horizon/heap_sparse_windows.c'
}
$linkOptions = @()
if ($DebugSymbols) {
    $definitions += '/Z7'
    $linkOptions += '/link', '/DEBUG:FULL'
}
$includes = @('/I', $include)
$cppInputs = @()
$cppIncludes = @()
if ($Graphics) {
    $archives = @((Join-Path $PSScriptRoot 'pokopia-sdk-host-graphics.lib'))
    $definitions += '/DPOKOPIA_GFX'
    if ($HeadlessPresent) { $definitions += '/DPOKOPIA_HOST_HEADLESS_PRESENT' }
    $includes += @('/I', (Join-Path $root 'gpu-probe/deps/orbis-sdk-v1/mesa/include'))
    foreach ($source in 'vk_adapter.c', 'vk_copy.c', 'vk_program.c', 'vk_texel.c', 'vi_adapter.c', 'nvn_front.c', 'nvn_window.c', 'nvn_replay.c',
                        'shader_cache.c', 'texture_upload.c', 'live_texture_upload.c', 'live_draw.c', 'live_compute.c',
                        'fsp.c', 'romfs.c', 'account.c') {
        $inputs += Join-Path $root "gfx-bridge/$source"
    }
    foreach ($source in 'stack_source.c', 'bsd_ssl.c', 'stpl.c', 'offline_app.c', 'hid.c', 'save_store.c') {
        $inputs += Join-Path $root "horizon/$source"
    }
    $pinned = Join-Path $root 'upstream/switch2-recomp-0.2.0'
    $nvn = Join-Path $pinned 'runtime/src/gfx/nvn'
    $cppInputs = @(
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
    $env:POKOPIA_SAVE_ROOT = Join-Path $PSScriptRoot 'host-save'
    $env:POKOPIA_SHADER_CACHE = if ($ShaderCache) { (Resolve-Path -LiteralPath $ShaderCache).ProviderPath } else { $null }
    $env:POKOPIA_DESKTOP_MEMORY_MIB = if ($DesktopMemoryMiB) { "$DesktopMemoryMiB" } else { $null }
} elseif ($DesktopMemoryMiB) {
    throw 'A desktop memory override requires the graphics diagnostic build.'
}
if ($FullGame -or $Graphics) {
    $mainArchive = if ($Ps4Memory) { 'pokopia-main-ps4-memory-host.lib' } else { 'pokopia-main-host.lib' }
    $archives += Join-Path $PSScriptRoot $mainArchive
    $archives += Join-Path $PSScriptRoot 'pokopia-subsdk0-host.lib'
    $definitions += '/DPOKOPIA_BOOT_GAME'
}
$name = if ($Graphics) { 'graphics' } else { 'boot' }
if ($SparseHeap) { $name = "$name-sparse" }
$boot = Join-Path $PSScriptRoot "pokopia-$name-host.exe"

$devEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment could not be loaded.' }
foreach ($line in $devEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:TEMP = $obj
$env:TMP = $obj
Push-Location -LiteralPath $obj
try {
    $cppObjects = @()
    foreach ($file in $cppInputs) {
        $object = [IO.Path]::GetFileNameWithoutExtension($file) + '.obj'
        & $clang /nologo /O2 /std:c++20 /EHsc /clang:-march=btver2 /c @includes @cppIncludes $file "/Fo$object"
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
$log = Join-Path $PSScriptRoot "host-$logName.log"
$errors = Join-Path $PSScriptRoot "host-$logName.err"
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
