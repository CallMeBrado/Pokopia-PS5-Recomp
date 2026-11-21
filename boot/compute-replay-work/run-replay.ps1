$ErrorActionPreference = 'Stop'
$vs='C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($line in (& cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
}
$env:TEMP=$PSScriptRoot
$env:TMP=$PSScriptRoot
$clang=Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
Push-Location -LiteralPath $PSScriptRoot
try {
    $includes=@('/I.', '/I../../gfx-bridge','/I../../gpu-probe/deps/orbis-sdk-v1/mesa/include')
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes /c nvn_replay.c live_compute.c
    if ($LASTEXITCODE) { throw "Compute replay compile failed ($LASTEXITCODE)." }
    $support = 'vk_adapter.c', 'vk_texel.c', 'nvn_front.c', 'shader_cache.c' | ForEach-Object { "../../gfx-bridge/$_" }
    & $clang /nologo /std:c17 /O2 /D_CRT_SECURE_NO_WARNINGS @includes test_compute_replay.c live_compute.obj @support /Fetest-compute-replay.exe
    if ($LASTEXITCODE) { throw "Test compile failed ($LASTEXITCODE)." }
    & ./test-compute-replay.exe
    if ($LASTEXITCODE) { throw "Compute replay test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
