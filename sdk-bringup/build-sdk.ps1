[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [ValidateRange(1, 16)] [int] $Jobs = 6,
    [switch] $WorkerStack,
    [switch] $Graphics
)

$ErrorActionPreference = 'Stop'
$buildRoot = $PSScriptRoot
$openOrbis = Join-Path (Split-Path -Parent $buildRoot) 'toolchain/OpenOrbis/PS4Toolchain'
$suffix = if ($Graphics) { '-graphics' } elseif ($WorkerStack) { '-worker' } else { '' }
$generated = Join-Path $buildRoot "lift$suffix"
$objects = Join-Path $buildRoot "obj$suffix"
$localHeaders = Join-Path $buildRoot 'include/nxrecomp'
New-Item -ItemType Directory -Path @($generated, $objects, $localHeaders) -Force | Out-Null
$env:TEMP = $buildRoot
$env:TMP = $buildRoot
$env:PYTHONDONTWRITEBYTECODE = '1'

$sdk = (Resolve-Path -LiteralPath (Join-Path $ExeFS 'sdk')).ProviderPath
$buildHash = '7C12078E9F853F5D77F395155F7308F264FA6E93FADAEBA8DCA525F4483C09B2'
if ((Get-FileHash -LiteralPath $sdk -Algorithm SHA256).Hash -ne $buildHash) {
    throw 'The SDK input does not match Pokopia update 655360.'
}

$sites = @()
if ($WorkerStack -or $Graphics) {
    $siteFile = Join-Path $buildRoot "$(if ($Graphics) { 'graphics' } else { 'worker' }).sites"
    & python (Join-Path $buildRoot 'worker-site.py') $sdk $Recompiler $siteFile
    if ($LASTEXITCODE -ne 0) { throw "SDK worker site validation failed ($LASTEXITCODE)." }
    if ($Graphics) {
        $graphicsHeader = Join-Path (Split-Path -Parent $buildRoot) 'gfx-bridge/sdk-graphics.inc'
        & python (Join-Path $buildRoot 'graphics-sites.py') $sdk $Recompiler $siteFile $graphicsHeader
        if ($LASTEXITCODE -ne 0) { throw "SDK graphics site validation failed ($LASTEXITCODE)." }
    }
    $sites = @('--sites', $siteFile)
}
Push-Location -LiteralPath $Recompiler
try {
    & python -m nxrecomp lift $sdk $generated --name sdk --base 0x7500000000 --index 1 --game pkp --shards 45 --jobs $Jobs @sites
    if ($LASTEXITCODE -ne 0) { throw "SDK lift failed ($LASTEXITCODE)." }
}
finally {
    Pop-Location
}

$bitCast = @(
    '/* OpenOrbis libc++ predates std::bit_cast. Use Clang''s equivalent builtin. */',
    'template <typename To, typename From>',
    'constexpr To nx_orbis_bit_cast(const From& from) noexcept {',
    '    return __builtin_bit_cast(To, from);',
    '}',
    '',
    '#define NX_BASE_UNUSED 0'
) -join [Environment]::NewLine
foreach ($header in (Get-ChildItem -LiteralPath (Join-Path $Recompiler 'include/nxrecomp') -Filter '*.h')) {
    $data = (Get-Content -Raw -LiteralPath $header.FullName).Replace('std::bit_cast', 'nx_orbis_bit_cast')
    if ($header.Name -eq 'cxx.h') {
        $data = $data.Replace('#define NX_BASE_UNUSED 0', $bitCast)
    }
    $data | Set-Content -Encoding utf8NoBOM -LiteralPath (Join-Path $localHeaders $header.Name)
}

$clang = Join-Path $Llvm 'clang.exe'
$flags = @(
    '--driver-mode=g++', '--target=x86_64-pc-freebsd12-elf', '-march=btver2', '-mcx16',
    '-std=c++20', '-O1', '-fPIC', '-fno-exceptions', '-fno-rtti', '-ffp-contract=off',
    '-ffunction-sections', '-fdata-sections',
    '-isystem', (Join-Path $openOrbis 'include/c++/v1'),
    '-isystem', (Join-Path $openOrbis 'include'),
    '-I', (Split-Path -Parent $localHeaders)
)
$units = Get-ChildItem -LiteralPath $generated -Filter '*.cpp'
$results = $units | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $compiler = $using:clang
    $options = $using:flags
    $output = Join-Path $using:objects ($_.BaseName + '.o')
    $diagnostics = & $compiler @options -c $_.FullName -o $output 2>&1
    [pscustomobject]@{ Source = $_.Name; Object = $output; ExitCode = $LASTEXITCODE; Diagnostic = $diagnostics }
}
$failed = $results | Where-Object ExitCode -ne 0
if ($failed) {
    $failed | Format-List Source, Diagnostic
    throw 'Orbis SDK object compilation failed.'
}
$archive = Join-Path $buildRoot "libpokopia-sdk$suffix-orbis.a"
$members = @($results | Sort-Object Source | Select-Object -ExpandProperty Object)
& (Join-Path $Llvm 'llvm-ar.exe') rcs $archive @members
if ($LASTEXITCODE -ne 0) { throw "SDK archive failed ($LASTEXITCODE)." }
Get-Item -LiteralPath $archive | Select-Object FullName, Length
