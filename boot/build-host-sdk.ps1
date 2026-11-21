[CmdletBinding()]
param(
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [ValidateRange(1, 12)] [int] $Jobs = 6,
    [switch] $WorkerStack,
    [switch] $Graphics
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$suffix = if ($Graphics) { '-graphics' } elseif ($WorkerStack) { '-worker' } else { '' }
$obj = Join-Path $PSScriptRoot "obj-host$suffix"
New-Item -ItemType Directory -Path $obj -Force | Out-Null
$env:TEMP = $obj
$env:TMP = $obj
$clang = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$archiver = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/llvm-lib.exe'
$dev = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Recompiler 'include'

$devEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment could not be loaded.' }
foreach ($line in $devEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$env:TEMP = $obj
$env:TMP = $obj

$sdk = Join-Path $root "sdk-bringup/lift$suffix"
$flags = @('/nologo', '/O1', '/std:c++20', '/clang:-march=btver2', '/clang:-mcx16',
           '/clang:-fno-exceptions', '/clang:-ffp-contract=off', '/I', $include)
$units = Get-ChildItem -LiteralPath $sdk -Filter '*.cpp'
$results = $units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $compiler = $using:clang
    $options = $using:flags
    $output = Join-Path $using:obj ($_.BaseName + '.obj')
    $diagnostic = & $compiler @options /c $_.FullName "/Fo$output" 2>&1
    [pscustomobject]@{ Source = $_.Name; Object = $output; ExitCode = $LASTEXITCODE; Diagnostic = $diagnostic }
}
$failed = $results | Where-Object ExitCode -ne 0
if ($failed) {
    $failed | Format-List Source,Diagnostic
    throw 'SDK host compilation failed.'
}
$members = @($results | Sort-Object Source | Select-Object -ExpandProperty Object)
$archive = Join-Path $PSScriptRoot "pokopia-sdk-host$suffix.lib"
& $archiver "/out:$archive" @members
if ($LASTEXITCODE -ne 0) { throw 'SDK host archive failed.' }
Get-Item -LiteralPath $archive | Select-Object FullName,Length
