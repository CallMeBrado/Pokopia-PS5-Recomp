[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
    [string] $Recompiler = (Join-Path (Split-Path -Parent $PSScriptRoot) 'upstream/switch2-recomp-0.2.0/recomp'),
    [ValidateSet('main', 'subsdk0')] [string[]] $Module = @('subsdk0', 'main'),
    [ValidateRange(1, 12)] [int] $Jobs = 10,
    [switch] $Ps4Memory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$objects = Join-Path $PSScriptRoot 'obj-host-game'
$clang = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$archiver = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/llvm-lib.exe'
$symbols = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin/llvm-nm.exe'
$dev = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Recompiler 'include'
$specs = @{
    main = @{
        Units = 231
        SHA256 = '8A8CE87B4EDFCE88C6260DA5DD80D6CAFC03A761EEBC842D7A3476E353A8336F'
    }
    subsdk0 = @{
        Units = 5
        SHA256 = '2E91FFC8B6F036FFD34FDB3C3DC280DEC80956308E34EA866167650842C9F3BE'
    }
}

$devEnvironment = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $dev)
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment could not be loaded.' }
foreach ($line in $devEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

New-Item -ItemType Directory -Path $objects -Force | Out-Null
$env:TEMP = $objects
$env:TMP = $objects
$flags = @(
    '/nologo', '/O1', '/std:c++20', '/bigobj', '/clang:-march=btver2', '/clang:-mcx16',
    '/clang:-fno-exceptions', '/clang:-fno-rtti', '/clang:-ffp-contract=off', '/I', $include
)

foreach ($name in $Module) {
    $spec = $specs[$name]
    $suffix = if ($Ps4Memory -and $name -eq 'main') { '-ps4-memory' } else { '' }
    $lift = Join-Path $root "game-bringup/lift/$name$suffix"
    $stamp = (Get-Content -LiteralPath (Join-Path $lift 'input-sha256.txt') -Raw).Trim()
    $inputHash = (Get-FileHash -LiteralPath (Join-Path $ExeFS $name) -Algorithm SHA256).Hash
    if ($stamp -ne $spec.SHA256 -or $inputHash -ne $spec.SHA256) {
        throw "Invalid $name lift or input for Pokopia update 655360."
    }

    $units = @(Get-ChildItem -LiteralPath $lift -Filter '*.cpp' | Sort-Object Name)
    if ($units.Count -ne $spec.Units) {
        throw "Expected $($spec.Units) lifted $name C++ units, found $($units.Count)."
    }

    $moduleObjects = Join-Path $objects "$name$suffix"
    New-Item -ItemType Directory -Path $moduleObjects -Force | Out-Null
    Write-Output "Compiling $name for Windows ($($units.Count) translation units, up to $Jobs jobs)"
    $results = $units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $compiler = $using:clang
        $options = $using:flags
        $object = Join-Path $using:moduleObjects ($_.BaseName + '.obj')
        $diagnostics = & $compiler @options /c $_.FullName "/Fo$object" 2>&1
        [pscustomobject]@{
            Source = $_.Name
            Object = $object
            ExitCode = $LASTEXITCODE
            Diagnostics = $diagnostics
        }
    }
    $failed = @($results | Where-Object ExitCode -ne 0)
    if ($failed.Count -gt 0) {
        $failed | Select-Object -First 10 | Format-List Source, Diagnostics
        throw "$name host compilation failed in $($failed.Count) translation units."
    }
    if (@($results).Count -ne $units.Count) { throw "$name host compilation produced an incomplete result." }

    $members = @($results | Sort-Object Source | Select-Object -ExpandProperty Object)
    $archiveName = "pokopia-$name$suffix-host.lib"
    $candidate = Join-Path $moduleObjects $archiveName
    & $archiver "/out:$candidate" @members
    if ($LASTEXITCODE -ne 0) { throw "$name host archive failed ($LASTEXITCODE)." }

    $index = & $symbols --print-armap $candidate | Select-String "^nxmod_$name(?:_abi)? in "
    if ($LASTEXITCODE -ne 0 -or @($index).Count -ne 2) {
        throw "$name archive lacks its module descriptor or ABI export."
    }

    $archive = Join-Path $PSScriptRoot $archiveName
    Move-Item -LiteralPath $candidate -Destination $archive -Force
    [pscustomobject]@{
        Path = $archive
        ObjectCount = $members.Count
        Bytes = (Get-Item -LiteralPath $archive).Length
        SHA256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    } | Format-List
}
