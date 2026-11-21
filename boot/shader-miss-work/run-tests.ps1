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
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes /c nvn_front.c nvn_replay.c vk_program.c shader_cache.c
    if ($LASTEXITCODE) { throw "Shader cache miss compile failed ($LASTEXITCODE)." }
    $layout = 'nvn_guest_layout.obj','nvn_texture_layout.obj','nvn_format.obj','tegra_tiling.obj' | ForEach-Object { "../packed-shader-check/$_" }
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes test_shader_miss.c nvn_front.obj vk_program.obj shader_cache.obj ../../gfx-bridge/vk_texel.c @layout /Fetest-shader-miss.exe
    if ($LASTEXITCODE) { throw "Shader-miss fixture compile failed ($LASTEXITCODE)." }
    & ./test-shader-miss.exe
    if ($LASTEXITCODE) { throw "Shader-miss fixture failed ($LASTEXITCODE)." }
    & $clang /nologo /std:c17 /O2 /D_CRT_SECURE_NO_WARNINGS @includes test_nvn_front.c nvn_front.obj shader_cache.obj @layout /Fetest-nvn-front.exe
    if ($LASTEXITCODE) { throw "NVN front fixture compile failed ($LASTEXITCODE)." }
    & ./test-nvn-front.exe
    if ($LASTEXITCODE) { throw "NVN front fixture failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}


