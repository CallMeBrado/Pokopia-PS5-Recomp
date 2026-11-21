[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [switch] $SdkOnly,
    [switch] $Graphics,
    [switch] $Ps4Memory,
    [switch] $TraceMemory,
    [switch] $SparseHeap,
    [switch] $LinkOnly,
    [string] $ShaderCache,
    [ValidateRange(1, 96)] [int] $GameBootRevision = 1,
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if ($Graphics -and $SdkOnly) { throw 'The graphics boot requires the game modules.' }
if ($Ps4Memory -and !$Graphics) { throw 'PS4 memory sites are tested with the graphics boot.' }
if ($TraceMemory -and !$Graphics) { throw 'Memory tracing requires the graphics boot.' }
if ($SparseHeap -and !$Ps4Memory) { throw 'Sparse heap testing requires the PS4 memory profile.' }
if ($ShaderCache -and !$Graphics) { throw 'Only a graphics boot uses a shader cache.' }
if ($GameBootRevision -ne 1 -and !$Ps4Memory) { throw 'Only the PS4 game boot has revisions.' }
$exeFsPath = (Resolve-Path -LiteralPath $ExeFS).ProviderPath
$recompilerPath = (Resolve-Path -LiteralPath $Recompiler).ProviderPath
foreach ($script in 'cpu-probe/generate.py', 'loader-probe/prepare.py') {
    & python (Join-Path $root $script) $exeFsPath $recompilerPath
    if ($LASTEXITCODE -ne 0) { throw "Guest image generation failed ($LASTEXITCODE)." }
}

$sources = @(
    'loader-probe/loader_probe.c',
    'cpu-probe/generated/rtld_7700000000.c',
    'horizon/get_info.c',
    'horizon/entropy.c',
    'horizon/sm.c',
    'horizon/lm.c',
    'horizon/cmif.c',
    'horizon/applet.c',
    'horizon/aoc.c',
    'horizon/pctl.c',
    'horizon/time_service.c',
    'horizon/readonly_page.c',
    'horizon/live_shared.c',
    'horizon/heap.c',
    'horizon/transfer_memory.c',
    'horizon/stack_map.c',
    'horizon/heap_stack_alias.c',
    'horizon/thread.c',
    'horizon/event.c',
    'horizon/key_sync.c',
    'boot/boot.c',
    'boot/host.c'
) | ForEach-Object { Join-Path $root $_ }
$archives = @((Join-Path $root 'sdk-bringup/libpokopia-sdk-worker-orbis.a'))
$options = @{}
$name = 'worker'
if ($Graphics) {
    $name = 'graphics-boot'
    $archives = @((Join-Path $root 'sdk-bringup/libpokopia-sdk-graphics-orbis.a'))
    foreach ($source in 'vk_adapter.c', 'vk_copy.c', 'vk_program.c', 'vk_texel.c', 'vi_adapter.c', 'nvn_front.c', 'nvn_window.c', 'nvn_replay.c',
                        'shader_cache.c', 'texture_upload.c', 'live_texture_upload.c', 'live_draw.c', 'live_compute.c', 'live_present.c',
                        'game_capture.c', 'fsp.c', 'romfs.c', 'account.c', 'orbis_memory.c') {
        $sources += Join-Path $root "gfx-bridge/$source"
    }
    if ($TraceMemory) { $sources += Join-Path $root 'gfx-bridge/vk_ledger.c' }
    if ($SparseHeap) { $sources += Join-Path $root 'horizon/heap_sparse_orbis.c' }
    $sources += (Join-Path $root 'gpu-present/present.c'), (Join-Path $root 'gpu-present/frame_capture.c'),
                (Join-Path $root 'gpu-present/debug_mailbox.c'), (Join-Path $root 'horizon/stack_source.c'),
                (Join-Path $root 'horizon/audio_device.c'), (Join-Path $root 'horizon/audio_out.c'),
                (Join-Path $root 'horizon/hwopus.c'), (Join-Path $root 'horizon/bsd_ssl.c'),
                (Join-Path $root 'horizon/stpl.c'), (Join-Path $root 'horizon/offline_app.c'),
                (Join-Path $root 'horizon/hid.c'), (Join-Path $root 'horizon/hid_ps4.c'),
                (Join-Path $root 'horizon/save_store.c')
    $options.VulkanBundle = Join-Path $root 'gpu-probe/deps/orbis-sdk-v1'
    $options.AudioOutput = $true
    $options.OpusSource = Join-Path $root 'upstream/opus-1.6.1'
    if ($ShaderCache) { $options.ShaderAssets = $ShaderCache }
    $pinned = Join-Path $root 'upstream/switch2-recomp-0.2.0'
    $nvn = Join-Path $pinned 'runtime/src/gfx/nvn'
    $options.ProbeCppSource = @(
        (Join-Path $root 'gfx-bridge/nvn_guest_layout.cpp'),
        (Join-Path $root 'horizon/ps4-pcm-stream.cpp'), (Join-Path $root 'horizon/ps4-pcm-native.cpp'),
        (Join-Path $nvn 'nvn_texture_layout.cpp'),
        (Join-Path $nvn 'nvn_format.cpp'),
        (Join-Path $pinned 'tools/tiling/src/tegra_tiling.cpp')
    )
    $options.ProbeCppInclude = @(
        (Join-Path $root 'gfx-bridge/nvn-layout-include'),
        (Join-Path $pinned 'runtime/include'), $nvn,
        (Join-Path $nvn 'third_party'), (Join-Path $pinned 'tools/tiling/include')
    )
}
if (!$SdkOnly) {
    $mainArchive = if ($Ps4Memory) { 'libpokopia-main-ps4-memory-orbis.a' } else { 'libpokopia-main-orbis.a' }
    $archives += Join-Path $root "game-bringup/$mainArchive"
    $archives += Join-Path $root 'game-bringup/libpokopia-subsdk0-orbis.a'
}
if ($Ps4Memory) {
    $name = if ($GameBootRevision -eq 1) { 'game-boot' } else { "game-boot-r$GameBootRevision" }
    $options.GameBootRevision = $GameBootRevision
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "out/$name" }

& (Join-Path $root 'toolchain/build-probe.ps1') `
    -ProbeSource $sources -ProbeInclude (Join-Path $recompilerPath 'include') `
    -LoaderAssets (Join-Path $root 'loader-probe/generated') `
    -BootLibraries $archives -OutputDirectory $OutputDirectory -TraceMemory:$TraceMemory -SparseHeap:$SparseHeap -LinkOnly:$LinkOnly @options
