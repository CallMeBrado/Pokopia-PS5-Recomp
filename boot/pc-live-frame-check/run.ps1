[CmdletBinding()]
param(
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [switch] $FullGame,
    [switch] $Graphics,
    [switch] $Ps4Memory,
    [string] $Ps4MemoryOverlay,
    [switch] $HeadlessPresent,
    [switch] $CapturePostLoading,
    [switch] $DebugSymbols,
    [switch] $TraceBootPhase,
    [switch] $TraceLoadingPackets,
    [switch] $TraceTextureBoundary,
    [string] $TexContextTag,
    [switch] $BuildOnly,
    [ValidateRange(1, 600)] [int] $TimeoutSeconds = 20,
    [ValidateRange(1, 120)] [int] $FrameCheckInterval = 1,
    [ValidateRange(0, 12288)] [int] $DesktopMemoryMiB,
    [string] $RomfsRoot = 'G:/pokopia_dump_v2-0-1/updates/655360/romfs',
    [string] $ShaderCache
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$originalBoot = Join-Path $root 'boot'
if ($Ps4Memory -and !$Graphics) { throw 'PS4 memory sites are tested with the graphics diagnostic.' }
if ($Ps4MemoryOverlay -and !$Ps4Memory) { throw 'A PS4 memory overlay requires the PS4-memory profile.' }
if ($HeadlessPresent -and !$Graphics) { throw 'Headless present requires the graphics diagnostic.' }
if ($CapturePostLoading -and !$HeadlessPresent) { throw 'Post-loading capture requires headless present.' }
if ($TraceBootPhase -and !$HeadlessPresent) { throw 'The boot phase trace requires headless present.' }
if ($TraceLoadingPackets -and (!$TraceBootPhase -or !$Ps4Memory)) { throw 'The packet trace requires the PS4-memory boot phase trace.' }
if ($TraceTextureBoundary -and !$TraceBootPhase) { throw 'The texture tail trace requires the boot phase trace.' }
if ($TexContextTag -and (!$HeadlessPresent -or !$Ps4Memory)) { throw 'The texture context trace requires headless present and the PS4-memory profile.' }
$texContextSources = @()
if ($TexContextTag) {
    & python (Join-Path $PSScriptRoot 'prepare-texcontext.py') $TexContextTag
    if ($LASTEXITCODE -ne 0) { throw 'Texture context overlay generation failed.' }
    $texContextSources = @('185', '050', '136') | ForEach-Object { Join-Path $PSScriptRoot "texcontext-c_$_.cpp" }
}
$obj = Join-Path $PSScriptRoot 'obj' 
$env:TEMP = $obj
$env:TMP = $obj
$clang = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$dev = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Recompiler 'include'
$loader = Join-Path $root 'loader-probe/loader_probe.c'
$replay = Join-Path $root 'gfx-bridge/nvn_replay.c'
function Add-TraceHook([string] $Source, [string] $Before, [string] $After) {
    $Before = $Before.Replace("`r`n", "`n")
    if (!$Source.Contains($Before)) { throw "Boot trace source marker missing: $Before" }
    $Source.Replace($Before, $After.Replace("`r`n", "`n"))
}
if ($HeadlessPresent) {
    $loaderSource = (Get-Content -LiteralPath $loader -Raw).Replace("`r`n", "`n")
    $present = '    nvn_window_release(windows, x[1], index);'
    $markerOffset = $loaderSource.IndexOf('/* The headless test consumes completed images')
    if ($markerOffset -lt 0) { throw 'Could not find the Windows headless present in the loader.' }
    $presentOffset = $loaderSource.IndexOf($present, $markerOffset)
    if ($presentOffset -lt 0) { throw 'Could not find the Windows NVN window release.' }
    $loaderSource = $loaderSource.Insert($presentOffset,
        "    pokopia_pc_frame_check(&graphics, texture, probe_log);`n")
    $draw = '    if (result != VK_SUCCESS || serial <= 16'
    $drawMarkerOffset = $loaderSource.IndexOf('static bool execute_nvn_draw(')
    if ($drawMarkerOffset -lt 0) { throw 'Could not find the Windows NVN draw callback.' }
    $drawOffset = $loaderSource.IndexOf($draw, $drawMarkerOffset)
    if ($drawOffset -lt 0) { throw 'Could not find the Windows NVN draw log.' }
    $loaderSource = $loaderSource.Insert($drawOffset,
        "    if (result == VK_SUCCESS)`n        pokopia_pc_frame_draw(draw, serial, probe_log);`n")
    $loaderSource = $loaderSource.Replace('#include <inttypes.h>',
        "#include <inttypes.h>`n#include `"pc-frame-check.h`"")
    if ($TraceBootPhase) {
        $loaderSource = Add-TraceHook $loaderSource '#include "pc-frame-check.h"' "#include `"pc-frame-check.h`"`n#include `"pc-boot-trace.h`""
        $loaderSource = Add-TraceHook $loaderSource '    return pokopia_vk_program_link(program, shaders, code, count, probe_log);' @'
    const NvnFrontProgramLinkResult result = pokopia_vk_program_link(program, shaders, code, count, probe_log);
    pokopia_pc_trace_program(program, result, probe_log);
    return result;
'@
        $loaderSource = Add-TraceHook $loaderSource '    pokopia_pc_frame_check(&graphics, texture, probe_log);' @'
    const GuestBoundary *trace_scope = pokopia_host_thread();
    const Ctx *trace_context = trace_scope ? trace_scope->context : NULL;
    pokopia_pc_trace_present(probe_log, read_guest, trace_context ? trace_context->tpidrro : 0,
                            trace_context ? trace_context->x[30] : 0, trace_context ? trace_context->x[29] : 0);
    pokopia_pc_frame_check(&graphics, texture, probe_log);
'@
        $loaderSource = Add-TraceHook $loaderSource '    uint32_t request_type = rd32(context->tpidrro) & 0xffffu;' @'
    uint32_t request_type = rd32(context->tpidrro) & 0xffffu;
    if (service)
        pokopia_pc_trace_ipc(service->type, handle, context->tpidrro, tls);
'@
        $loaderSource = Add-TraceHook $loaderSource @'
    if (hzn_event_svc(events, number, context->x, waiter_id))
        return;
'@ @'
    const bool trace_event_wait = number == 0x18 && (int64_t)context->x[3] < 0;
    if (trace_event_wait)
        pokopia_pc_trace_wait_start(context->tpidrro, number, context->x[30], context->x[29], context->x[1], context->x[2]);
    const bool event_handled = hzn_event_svc(events, number, context->x, waiter_id);
    if (trace_event_wait)
        pokopia_pc_trace_wait_end(context->tpidrro);
    if (event_handled)
        return;
'@
        $loaderSource = Add-TraceHook $loaderSource @'
        if (hzn_key_sync_svc(key_sync, number, context->x, id, priority))
            return;
'@ @'
        const bool trace_key_wait = (number == 0x1c || number == 0x34) && (int64_t)context->x[3] < 0;
        if (trace_key_wait)
            pokopia_pc_trace_wait_start(context->tpidrro, number, context->x[30], context->x[29], context->x[0], context->x[1]);
        const bool key_handled = hzn_key_sync_svc(key_sync, number, context->x, id, priority);
        if (trace_key_wait)
            pokopia_pc_trace_wait_end(context->tpidrro);
        if (key_handled)
            return;
'@
        $replaySource = (Get-Content -LiteralPath $replay -Raw).Replace("`r`n", "`n")
        $replaySource = Add-TraceHook $replaySource '#include "vk_program.h"' "#include `"vk_program.h`"`n#include `"pc-boot-trace.h`""
        $replaySource = Add-TraceHook $replaySource 'static NvnFrontReplyKind set_render_targets(NvnReplay *replay, const uint64_t x[32])' @'
static void trace_attachment(NvnReplay *replay, const char *kind, uint64_t address, uint64_t view_address)
{
    NvnFrontTextureInfo info = {0};
    const bool found = nvn_front_get_texture(replay->config.front, address, &info);
    const NvnFrontTextureConfig *config = &info.create_info.config;
    fprintf(stdout, "PC attachment failure %s address=%#llx exists=%u native=%p format=%#x target=%#x "
        "extent=%ux%ux%u levels=%u view=%#llx", kind, (unsigned long long)address, (unsigned)found,
        info.native, config->format, config->target, config->width, config->height, config->depth,
        config->levels, (unsigned long long)view_address);
    const uint8_t *view = view_address ? replay->config.read_guest(replay->config.opaque, view_address, 16) : NULL;
    if (view) {
        fputs(" bytes=", stdout);
        for (unsigned i = 0; i < 16; ++i)
            fprintf(stdout, "%02x", view[i]);
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static NvnFrontReplyKind set_render_targets(NvnReplay *replay, const uint64_t x[32])
'@
        $replaySource = Add-TraceHook $replaySource @'
                !select_target_layer(replay, views[i], &target, &layers[i]))
                return NVN_FRONT_BOUNDARY;
'@ @'
                !select_target_layer(replay, views[i], &target, &layers[i])) {
                trace_attachment(replay, "color", targets[i], views[i]);
                return NVN_FRONT_BOUNDARY;
            }
'@
        $replaySource = Add-TraceHook $replaySource @'
            !select_target_layer(replay, x[5], &target, &depth_layer))
            return NVN_FRONT_BOUNDARY;
'@ @'
            !select_target_layer(replay, x[5], &target, &depth_layer)) {
            trace_attachment(replay, "depth", x[4], x[5]);
            return NVN_FRONT_BOUNDARY;
        }
'@
        $replaySource = Add-TraceHook $replaySource @'
    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), x[4] ? 16 : 0))
        return NVN_FRONT_BOUNDARY;
'@ @'
    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), x[4] ? 16 : 0)) {
        NvnFrontCommandBufferInfo cmd = {0};
        nvn_front_get_command_buffer(replay->config.front, x[0], &cmd);
        fprintf(stdout, "PC attachment charge failure cmd=%llu/%llu ctrl=%llu/%llu segment=%u callback=%#llx\n",
            (unsigned long long)cmd.command_used, (unsigned long long)cmd.command_size,
            (unsigned long long)cmd.control_used, (unsigned long long)cmd.control_size,
            (unsigned)recording->command_segment, (unsigned long long)cmd.memory_callback);
        fflush(stdout);
        return NVN_FRONT_BOUNDARY;
    }
'@
        $replaySource = Add-TraceHook $replaySource @'
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
    }
'@ @'
    const char *trace_fragment = NULL;
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info = {0};
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
        if (stage == NVN_SHADER_FRAGMENT)
            trace_fragment = info.hash;
    }
'@
        $replaySource = Add-TraceHook $replaySource @'
    if (!indexed)
        return append_metered(replay, r, operation, 16, 0);
    const size_t bytes = !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);
    return append_metered(replay, r, operation, bytes, 0);
'@ @'
    const size_t bytes = !indexed ? 16 : !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);
    const NvnFrontReplyKind result = append_metered(replay, r, operation, bytes, 0);
    pokopia_pc_trace_draw_phase(trace_fragment, PC_TRACE_RECORD, r->targets[0], result);
    return result;
'@
        $replaySource = Add-TraceHook $replaySource @'
        programs[stage] = program.native;
    }
    for (unsigned stage = 0; stage < 2; ++stage) {
'@ @'
        programs[stage] = program.native;
    }
    pokopia_pc_trace_draw_phase(stages[NVN_SHADER_FRAGMENT].hash,
        execute ? PC_TRACE_SUBMIT_EXECUTE : PC_TRACE_SUBMIT_VALIDATE, record->target, NVN_FRONT_HANDLED);
    for (unsigned stage = 0; stage < 2; ++stage) {
'@
        $replay = Join-Path $PSScriptRoot 'nvn_replay_trace.c'
        Set-Content -LiteralPath $replay -Value $replaySource -Encoding utf8
    }
    if ($TraceTextureBoundary) {
        $loaderSource = Add-TraceHook $loaderSource '    fprintf(probe_log, "NVN texture %s %ux%ux%u target=%#x format=%u mips=%u guest=%#" PRIx64' @'
    pokopia_pc_trace_texture_tail(probe_log, info->storage_address);
    fprintf(probe_log, "NVN texture %s %ux%ux%u target=%#x format=%u mips=%u guest=%#" PRIx64
'@
        $loaderSource = Add-TraceHook $loaderSource '    if (reply.kind == NVN_FRONT_BOUNDARY || reply.kind == NVN_FRONT_INVALID_GUEST) {' @'
    pokopia_pc_trace_nvn_tail(probe_log, &reply, context->x[30], context->tpidrro);
    if (reply.kind == NVN_FRONT_BOUNDARY || reply.kind == NVN_FRONT_INVALID_GUEST) {
'@
    }
    if ($TraceLoadingPackets) {
        $loaderSource = Add-TraceHook $loaderSource '    return render_entry_hook(context, address) || stack_entry_hook(context, address) || dispatch_graphics(context, address);' @'
    if (address == UINT64_C(0x71017b7b50) || address == UINT64_C(0x71017b7b7c)) {
        pokopia_pc_trace_packet(probe_log, address, context->x);
        return 0;
    }
    return render_entry_hook(context, address) || stack_entry_hook(context, address) || dispatch_graphics(context, address);
'@
    }
    if ($TexContextTag) {
        $anchor = "int nx_site_midhook(Ctx *context, uint64_t address)`n{"
        $textureTrace = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'trace-texcontext-loader.inc') -Raw
        $loaderSource = Add-TraceHook $loaderSource $anchor ($textureTrace + "`n$anchor`n    if (trace_texcontext(context, address))`n        return 0;")
    }
    $loaderSource = $loaderSource.Replace('#include "../', '#include "' + $root.Replace('\', '/') + '/')
    $loader = Join-Path $PSScriptRoot 'loader_capture.c'
    Set-Content -LiteralPath $loader -Value $loaderSource -Encoding utf8
    $env:POKOPIA_PC_FRAME_PPM = Join-Path $PSScriptRoot 'first-visible.ppm'
    $env:POKOPIA_PC_BROAD_PPM = if ($CapturePostLoading) { Join-Path $PSScriptRoot 'first-broad.ppm' } else { $null }
    $env:POKOPIA_PC_SCENE_PPM = if ($CapturePostLoading) { Join-Path $PSScriptRoot 'later-scene.ppm' } else { $null }
    $env:POKOPIA_PC_FRAME_INTERVAL = "$FrameCheckInterval"
    $outputs = @('first-visible.ppm', 'first-visible.png')
    if ($CapturePostLoading) { $outputs += 'first-broad.ppm', 'first-broad.png', 'later-scene.ppm', 'later-scene.png' }
    foreach ($name in $outputs) {
        $path = Join-Path $PSScriptRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
}
$inputs = @(
    $loader,
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
    (Join-Path $root 'horizon/transfer_memory.c'),
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
    $inputs += $replay
    if ($TraceBootPhase) { $inputs += Join-Path $PSScriptRoot 'pc-boot-trace.c' }
    foreach ($source in 'vk_adapter.c', 'vk_copy.c', 'vk_program.c', 'vk_texel.c', 'vi_adapter.c', 'nvn_front.c', 'nvn_window.c',
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
        $(if ($HeadlessPresent) { Join-Path $PSScriptRoot 'pc-frame-check.c' }
          else { Join-Path $root 'gfx-bridge/live_texture_upload.c' }),
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
    if ($TraceLoadingPackets) {
        $lift = Join-Path $root 'game-bringup/lift/main-ps4-memory'
        $packetSource = (Get-Content -LiteralPath (Join-Path $lift 'c_137.cpp') -Raw).Replace("`r`n", "`n")
        $packetSource = Add-TraceHook $packetSource '  c->fn = 0x71017b7b50ULL;' @'
  c->fn = 0x71017b7b50ULL;
  nx_hook_entry(c, 0x71017b7b50ULL);
'@
        $packetSource = Add-TraceHook $packetSource '  c->x[30] = 0x71017b7b7cULL; f_71017b69e0(c);' @'
  c->x[30] = 0x71017b7b7cULL; f_71017b69e0(c);
  nx_hook_entry(c, 0x71017b7b7cULL);
'@
        $packetPath = Join-Path $PSScriptRoot 'c_137_packet_trace.cpp'
        Set-Content -LiteralPath $packetPath -Value $packetSource -Encoding utf8
        $cppIncludes += @('/I', $lift)
        $cppInputs += $packetPath
    }
    if ($TexContextTag) {
        if (!$TraceLoadingPackets) { $cppIncludes += @('/I', (Join-Path $root 'game-bringup/lift/main-ps4-memory')) }
        $cppInputs += $texContextSources
    }
    $env:POKOPIA_ROMFS_ROOT = (Resolve-Path -LiteralPath $RomfsRoot).ProviderPath
    $env:POKOPIA_SHADER_CACHE = if ($ShaderCache) { (Resolve-Path -LiteralPath $ShaderCache).ProviderPath } else { $null }
    $env:POKOPIA_DESKTOP_MEMORY_MIB = if ($DesktopMemoryMiB) { "$DesktopMemoryMiB" } else { $null }
    $env:POKOPIA_SAVE_ROOT = Join-Path $PSScriptRoot 'save-probe'
} elseif ($DesktopMemoryMiB) {
    throw 'A desktop memory override requires the graphics diagnostic build.'
}
if ($FullGame -or $Graphics) {
    $mainArchive = if ($Ps4Memory) { 'pokopia-main-ps4-memory-host.lib' } else { 'pokopia-main-host.lib' }
    if ($Ps4Memory) {
        $archives += if ($Ps4MemoryOverlay) { (Resolve-Path -LiteralPath $Ps4MemoryOverlay).ProviderPath }
            else { Join-Path $originalBoot 'thread-wait-check/obj-shader-overlay/gpu-pool-256-diagnostic.obj' }
    }
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
        $cppOptions = if ($file -eq $packetPath -or $texContextSources -contains $file) {
            @('/O1', '/bigobj', '/clang:-mcx16', '/clang:-fno-exceptions', '/clang:-fno-rtti', '/clang:-ffp-contract=off')
        } else { @('/O2', '/EHsc') }
        & $clang /nologo /std:c++20 /DNOMINMAX /clang:-march=btver2 /c @cppOptions @includes @cppIncludes $file "/Fo$object"
        if ($LASTEXITCODE -ne 0) { throw "Desktop graphics layout build failed ($LASTEXITCODE)." }
        $cppObjects += $object
    }
    & $clang /nologo /O2 /std:c11 /clang:-march=btver2 @definitions @includes @inputs @cppObjects @archives "/Fe$boot" @linkOptions
    if ($LASTEXITCODE -ne 0) { throw "Desktop boot build failed ($LASTEXITCODE)." }
}
finally {
    Pop-Location
}

if ($BuildOnly) { return }

$logName = if ($Ps4Memory) { "$name-ps4-memory" } else { $name }
if ($HeadlessPresent) { $logName = "$logName-headless" }
if ($DesktopMemoryMiB) { $logName = "$logName-$DesktopMemoryMiB" }
if ($TexContextTag) { $logName = "$logName-texcontext" }
$log = Join-Path $PSScriptRoot "sparse-$logName.log"
$errors = Join-Path $PSScriptRoot "sparse-$logName.err"
$process = Start-Process -WindowStyle Hidden -FilePath $boot -WorkingDirectory (Join-Path $root 'loader-probe') `
    -PassThru -RedirectStandardOutput $log -RedirectStandardError $errors
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$boundary = $null
while (!$process.WaitForExit(1000) -and [DateTime]::UtcNow -lt $deadline) {
    $boundary = Select-String -LiteralPath $log -Pattern '^STOP ' | Select-Object -First 1
    if ($boundary) { break }
}
$timedOut = !$process.HasExited -and !$boundary
if (!$process.HasExited -and !($boundary -and $process.WaitForExit(1500))) {
    Stop-Process -Id $process.Id
    $process.WaitForExit()
}
if ($HeadlessPresent) {
    foreach ($ppm in $env:POKOPIA_PC_FRAME_PPM, $env:POKOPIA_PC_BROAD_PPM, $env:POKOPIA_PC_SCENE_PPM) {
        if (!$ppm -or !(Test-Path -LiteralPath $ppm)) { continue }
        $png = [IO.Path]::ChangeExtension($ppm, '.png')
        & python -c 'from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])' $ppm $png
        if ($LASTEXITCODE -eq 0) { "Saved $png" }
    }
}
Get-Content -LiteralPath $log -Tail 12
if ((Get-Item -LiteralPath $errors).Length) { Get-Content -LiteralPath $errors -Tail 12 }
if ($boundary) { throw "$($boundary.Line). See $log." }
if ($timedOut) { throw "Desktop boot probe exceeded $TimeoutSeconds seconds. See $log." }
"Desktop probe exited $($process.ExitCode)."
if ($process.ExitCode -ne 0) {
    throw "Desktop boot probe failed with exit code $($process.ExitCode)."
}
