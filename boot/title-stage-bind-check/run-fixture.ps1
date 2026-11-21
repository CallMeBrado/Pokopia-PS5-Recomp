$ErrorActionPreference = 'Stop'
$vs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($line in (& cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
}
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
$clang = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
Push-Location -LiteralPath $PSScriptRoot
try {
    $bridge = '../../gfx-bridge'
    $sources = @('test-stage-bind.c', "$bridge/nvn_front.c", "$bridge/vk_program.c", "$bridge/shader_cache.c", "$bridge/vk_texel.c")
    & $clang /nologo /std:c17 /O2 /Gy /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I../../gfx-bridge /I../../gpu-probe/deps/orbis-sdk-v1/mesa/include @sources /Fetest-stage-bind.exe /link /OPT:REF
    if ($LASTEXITCODE) { throw "Stage bind test compile failed ($LASTEXITCODE)." }
    & ./test-stage-bind.exe
    if ($LASTEXITCODE) { throw "Stage bind test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
