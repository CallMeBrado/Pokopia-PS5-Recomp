$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).ProviderPath
$vs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$vsEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE) { throw 'Could not load the developer environment.' }
foreach ($line in $vsEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
$clang = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
Push-Location -LiteralPath $PSScriptRoot
try {
    $src = Get-Content -Raw -LiteralPath '../render-resolution-check/test_cluster_resolution.c'
    $start = $src.IndexOf('static void open_texel(')
    $end = $src.IndexOf('int main(void)', $start)
    if ($start -lt 0 -or $end -lt 0) { throw 'The reference fixture is missing its expected open_texel helper.' }
    $src = $src.Substring(0, $start) + "#include `"instrumented_texel.inc`"`r`n`r`n" + $src.Substring($end)
    $src = $src.Replace('len < sizeof path', '(size_t)len < sizeof path')
    $layoutStubs = $src.IndexOf('VkFormat nvn_guest_texture_vk_format(')
    if ($layoutStubs -ge 0) { $src = $src.Substring(0, $layoutStubs) }
    $src = $src.Replace('    PFN_vkDestroyShaderModule destroy', @'
    assert(allocations == frees && buffers_created == buffers_destroyed);
    puts("PASS every allocated texel was released");
    PFN_vkDestroyShaderModule destroy
'@)
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'test_lazy_texel.c'), $src)
    $includes = @('/I.', '/I../../gfx-bridge', '/I../../gpu-probe/deps/orbis-sdk-v1/mesa/include')
    $cFiles = @(
        'test_lazy_texel.c', '../../gfx-bridge/vk_texel.c',
        '../../gfx-bridge/live_compute.c', '../../gfx-bridge/vk_adapter.c'
    )
    $layoutObjects = Get-ChildItem -LiteralPath '../r36-image-pool-check' -Filter '*.obj' |
        Select-Object -ExpandProperty FullName
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes @cFiles @layoutObjects /Fetest-lazy-texel.exe
    if ($LASTEXITCODE) { throw "Fixture compile exited $LASTEXITCODE." }
    & ./test-lazy-texel.exe
    if ($LASTEXITCODE) { throw "Fixture exited $LASTEXITCODE." }
} finally {
    Pop-Location
}
