[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
)

$ErrorActionPreference = 'Stop'
$env:PYTHONDONTWRITEBYTECODE = '1'
$probe = $PSScriptRoot
Push-Location -LiteralPath $probe
try {
    $env:TEMP = $probe
    $env:TMP = $probe
    & python prepare.py $ExeFS $Recompiler
    if ($LASTEXITCODE -ne 0) { throw "NSO preparation failed ($LASTEXITCODE)." }
    $vs = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
    $clang = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
    $include = Join-Path $Recompiler 'include'
    $rtld = Join-Path (Split-Path -Parent $probe) 'cpu-probe/generated/rtld_7700000000.c'
    $build = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && "{1}" /nologo /O2 /clang:-march=btver2 /std:c11 /I"{2}" loader_probe.c "{3}" /Feloader-probe.exe'
    & cmd.exe /d /s /c ($build -f $vs, $clang, $include, $rtld)
    if ($LASTEXITCODE -ne 0) { throw "Loader probe build failed ($LASTEXITCODE)." }

    $log = Join-Path $probe 'loader-probe.log'
    $errorLog = Join-Path $probe 'loader-probe.err.log'
    $process = Start-Process -WindowStyle Hidden -FilePath (Join-Path $probe 'loader-probe.exe') -PassThru -RedirectStandardOutput $log -RedirectStandardError $errorLog
    if (!$process.WaitForExit(15000)) {
        Stop-Process -Id $process.Id
        Get-Content -LiteralPath $log, $errorLog
        throw 'Loader probe exceeded 15 seconds.'
    }
    Get-Content -LiteralPath $log, $errorLog
    if ($process.ExitCode -ne 0) { throw "Loader probe stopped ($($process.ExitCode))." }
}
finally {
    Pop-Location
}
