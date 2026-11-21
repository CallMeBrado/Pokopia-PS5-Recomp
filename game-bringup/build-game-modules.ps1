[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = (Join-Path (Split-Path -Parent $PSScriptRoot) 'upstream/switch2-recomp-0.2.0/recomp'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [ValidateSet('main', 'subsdk0')] [string[]] $Module = @('subsdk0', 'main'),
    [ValidateRange(1, 6)] [int] $Jobs = 6,
    [switch] $ReuseLift,
    [string] $MemorySites
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $project 'toolchain/OpenOrbis/PS4Toolchain'
$overlay = Join-Path $project 'sdk-bringup/include'
$temp = Join-Path $PSScriptRoot 'temp'
New-Item -ItemType Directory -Path $temp -Force | Out-Null
$env:TEMP = $temp
$env:TMP = $temp
$env:PYTHONDONTWRITEBYTECODE = '1'

$hashes = [ordered]@{
    main = '8A8CE87B4EDFCE88C6260DA5DD80D6CAFC03A761EEBC842D7A3476E353A8336F'
    sdk = '7C12078E9F853F5D77F395155F7308F264FA6E93FADAEBA8DCA525F4483C09B2'
    subsdk0 = '2E91FFC8B6F036FFD34FDB3C3DC280DEC80956308E34EA866167650842C9F3BE'
    rtld = '166D3B75EC5559CD241CBACF03DBF16C267A2CBFC95C33C41BA02D3A03C5616F'
    'main.npdm' = 'DE024CC0DCDF3480B1FCA447344FCE0A35F49AAFFE842C25E5FB4C6D359D42B4'
}
foreach ($file in $hashes.Keys) {
    $input = Join-Path $ExeFS $file
    if ((Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash -ne $hashes[$file]) {
        throw "Invalid Pokopia update 655360 ExeFS input: $file"
    }
}

$clang = Join-Path $Llvm 'clang.exe'
$ar = Join-Path $Llvm 'llvm-ar.exe'
$memorySitePath = if ($MemorySites) { (Resolve-Path -LiteralPath $MemorySites).ProviderPath }
$compileFlags = @(
    '--driver-mode=g++', '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-mcx16',
    '-std=c++20', '-O1', '-fPIC', '-fno-exceptions', '-fno-rtti', '-ffp-contract=off',
    '-ffunction-sections', '-fdata-sections',
    '-isystem', (Join-Path $sdk 'include/c++/v1'),
    '-isystem', (Join-Path $sdk 'include'),
    '-I', $overlay
)
$specs = @{
    main = @{ Base = '0x7100000000'; Index = 0; Shards = 230 }
    subsdk0 = @{ Base = '0x7600000000'; Index = 2; Shards = 4 }
}
foreach ($name in $Module) {
    $spec = $specs[$name]
    $suffix = if ($name -eq 'main' -and $memorySitePath) { '-ps4-memory' } else { '' }
    $generated = Join-Path $PSScriptRoot "lift/$name$suffix"
    $objects = Join-Path $PSScriptRoot "obj/$name$suffix"
    $state = Join-Path $generated 'input-sha256.txt'
    $siteState = Join-Path $generated 'sites-sha256.txt'
    New-Item -ItemType Directory -Path @($generated, $objects) -Force | Out-Null
    if ($ReuseLift -and (Test-Path -LiteralPath $state)) {
        if ((Get-Content -LiteralPath $state -Raw).Trim() -ne $hashes[$name]) {
            throw "Cached lift input does not match verified $name."
        }
        if ($suffix -and (Get-Content -LiteralPath $siteState -Raw).Trim() -ne
            (Get-FileHash -LiteralPath $memorySitePath -Algorithm SHA256).Hash) {
            throw 'Cached memory profile lift does not match its site list.'
        }
    }
    else {
        $input = (Resolve-Path -LiteralPath (Join-Path $ExeFS $name)).ProviderPath
        $options = @(
            '-m', 'nxrecomp', 'lift', $input, $generated, '--name', $name,
            '--base', $spec.Base, '--index', $spec.Index, '--game', 'pkp',
            '--shards', $spec.Shards, '--jobs', $Jobs
        )
        if ($name -eq 'main') {
            $sites = Join-Path $Recompiler 'games/pkp/main.sites'
            if ($suffix) {
                $combinedSites = Join-Path $generated 'main.sites'
                @(Get-Content -LiteralPath $sites; Get-Content -LiteralPath $memorySitePath) |
                    Set-Content -LiteralPath $combinedSites -Encoding ascii
                $sites = $combinedSites
            }
            $options += @('--sites', $sites)
        }
        Push-Location -LiteralPath $Recompiler
        try {
            & python @options
            if ($LASTEXITCODE -ne 0) { throw "$name lift failed with exit code $LASTEXITCODE" }
        }
        finally {
            Pop-Location
        }
        Set-Content -LiteralPath $state -Value $hashes[$name] -Encoding ascii
        if ($suffix) {
            Set-Content -LiteralPath $siteState -Value (Get-FileHash -LiteralPath $memorySitePath).Hash -Encoding ascii
        }
    }

    $units = @(Get-ChildItem -LiteralPath $generated -Filter '*.cpp' | Sort-Object Name)
    if ($units.Count -ne ($spec.Shards + 1)) {
        throw "$name lift produced $($units.Count) C++ units, expected $($spec.Shards + 1)"
    }

    Write-Output "Cross-compiling $name ($($units.Count) translation units, up to $Jobs jobs)"
    $results = $units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $compiler = $using:clang
        $options = $using:compileFlags
        $output = Join-Path $using:objects ($_.BaseName + '.o')
        $diagnostics = & $compiler @options -c $_.FullName -o $output 2>&1
        [pscustomobject]@{
            Source = $_.Name
            Object = $output
            ExitCode = $LASTEXITCODE
            Diagnostics = $diagnostics
        }
    }
    $errors = @($results | Where-Object ExitCode -ne 0)
    if ($errors.Count -gt 0) {
        $errors | Format-List Source, Diagnostics | Out-String -Width 280 | Write-Output
        throw "$name cross-compilation failed in $($errors.Count) translation units"
    }

    $archive = Join-Path $PSScriptRoot "libpokopia-$name$suffix-orbis.a"
    $members = @($results | Sort-Object Source | Select-Object -ExpandProperty Object)
    & $ar rcs $archive @members
    if ($LASTEXITCODE -ne 0) { throw "$name archive failed with exit code $LASTEXITCODE" }
    [pscustomobject]@{
        Path = $archive
        ObjectCount = $members.Count
        Bytes = (Get-Item -LiteralPath $archive).Length
        SHA256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    } | Format-List
}
