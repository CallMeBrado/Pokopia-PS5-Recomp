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
    $layout = 'nvn_guest_layout.obj','nvn_texture_layout.obj','nvn_format.obj','tegra_tiling.obj' | ForEach-Object { "../rgba32f-texture-check/$_" }
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes test_rg32f.c vk_adapter.c texture_upload.c ../../gfx-bridge/live_texture_upload.c ../../gfx-bridge/vk_texel.c @layout /Fetest-rg32f.exe
    if ($LASTEXITCODE) { throw "RG32F fixture compile failed ($LASTEXITCODE)." }
    & ./test-rg32f.exe
    if ($LASTEXITCODE) { throw "RG32F fixture failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}

