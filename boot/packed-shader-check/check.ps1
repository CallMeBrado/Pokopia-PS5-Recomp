$ErrorActionPreference = 'Stop'
$probeRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$probeVs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$probeClang = Join-Path $probeVs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$probeDev = Join-Path $probeVs 'Common7/Tools/VsDevCmd.bat'
$probeEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $probeDev)
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment failed' }
foreach ($probeLine in $probeEnvironment) {
    if ($probeLine -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
$probeGraphics = Join-Path $probeRoot 'gfx-bridge'
$probeUpstream = Join-Path $probeRoot 'upstream/switch2-recomp-0.2.0'
$probeNvn = Join-Path $probeUpstream 'runtime/src/gfx/nvn'
$probeIncludes = @('/I', $probeGraphics, '/I', (Join-Path $probeRoot 'gpu-probe/deps/orbis-sdk-v1/mesa/include'))
$probeCppIncludes = @('/I', (Join-Path $probeGraphics 'nvn-layout-include'), '/I', (Join-Path $probeUpstream 'runtime/include'),
    '/I', $probeNvn, '/I', (Join-Path $probeNvn 'third_party'), '/I', (Join-Path $probeUpstream 'tools/tiling/include'))
$probeCppInputs = @((Join-Path $probeGraphics 'nvn_guest_layout.cpp'), (Join-Path $probeNvn 'nvn_texture_layout.cpp'),
    (Join-Path $probeNvn 'nvn_format.cpp'), (Join-Path $probeUpstream 'tools/tiling/src/tegra_tiling.cpp'))
$probeObjects = @()
Push-Location -LiteralPath $PSScriptRoot
try {
    foreach ($probeSource in $probeCppInputs) {
        $probeObject = [IO.Path]::GetFileNameWithoutExtension($probeSource) + '.obj'
        & $probeClang /nologo /O2 /std:c++20 /EHsc /c @probeIncludes @probeCppIncludes $probeSource "/Fo$probeObject"
        if ($LASTEXITCODE -ne 0) { throw "Layout compile failed ($LASTEXITCODE)" }
        $probeObjects += $probeObject
    }
    $probeInputs = @('check.c', (Join-Path $probeGraphics 'vk_adapter.c'), (Join-Path $probeGraphics 'vk_texel.c'), (Join-Path $probeGraphics 'shader_cache.c'))
    & $probeClang /nologo /O2 /std:c11 /DTEST_VULKAN @probeIncludes @probeInputs @probeObjects /Fecheck.exe
    if ($LASTEXITCODE -ne 0) { throw "Adapter check compile failed ($LASTEXITCODE)" }
    $env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
    $env:VK_LOADER_DRIVERS_SELECT = 'amd-vulkan64.json'
    & ./check.exe (Join-Path $probeGraphics 'generated/startup-spirv-first-three') (Join-Path $probeRoot 'frame-reference-655360/shader_cache/missing') 'cases'
    if ($LASTEXITCODE -ne 0) { throw "Adapter check failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}


