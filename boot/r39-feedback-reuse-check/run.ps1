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
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes /c ../../gfx-bridge/live_draw.c ../../gfx-bridge/vk_adapter.c
    if ($LASTEXITCODE) { throw "Depth replay compile failed ($LASTEXITCODE)." }
    & $clang /nologo /std:c17 /O2 /W4 /WX /D_CRT_SECURE_NO_WARNINGS @includes test-post-depth.c live_draw.obj vk_adapter.obj ../../gfx-bridge/vk_texel.c ../../gfx-bridge/texture_upload.c /Fetest-post-depth.exe
    if ($LASTEXITCODE) { throw "Post-depth replay test compile failed ($LASTEXITCODE)." }
    & ./test-post-depth.exe
    if ($LASTEXITCODE) { throw "Post-depth replay test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}

