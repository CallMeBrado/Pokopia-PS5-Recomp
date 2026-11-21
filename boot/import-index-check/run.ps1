$ErrorActionPreference = 'Stop'
$vs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$clang = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$headers = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp/include'
$developerEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE) { throw "Visual Studio environment setup failed ($LASTEXITCODE)." }
foreach ($line in $developerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
Push-Location -LiteralPath $PSScriptRoot
try {
    & $clang /nologo /std:c17 /O2 /W4 /WX /DPOKOPIA_BOOT_GAME /D_CRT_SECURE_NO_WARNINGS "/I$headers" test_import_index.c ../boot.c ../../horizon/get_info.c ../../horizon/heap.c ../../horizon/entropy.c /Fetest-import-index.exe
    if ($LASTEXITCODE) { throw "Import index test compilation failed ($LASTEXITCODE)." }
    & ./test-import-index.exe
    if ($LASTEXITCODE) { throw "Import index test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
