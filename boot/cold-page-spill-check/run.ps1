$ErrorActionPreference = 'Stop'
$vs = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($line in (& cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev))) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$env:TEMP = $PSScriptRoot
$env:TMP = $PSScriptRoot
$clang = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $clang /nologo /std:c17 /O2 /W4 /WX /c cold_heap_windows.c cold_store.c heap_stack_alias.c
    if ($LASTEXITCODE) { throw "Cold heap source compile failed ($LASTEXITCODE)." }
    & $clang /nologo /std:c17 /O2 /W4 /WX test-cold-spill.c cold_store.obj onecore.lib /Fetest-cold-spill.exe
    if ($LASTEXITCODE) { throw "Cold heap test link failed ($LASTEXITCODE)." }
    & ./test-cold-spill.exe
    if ($LASTEXITCODE) { throw "Cold heap test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}

