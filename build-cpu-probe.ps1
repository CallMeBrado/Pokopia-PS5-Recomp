[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp'
)

$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot 'cpu-probe'
$exeFsPath = (Resolve-Path -LiteralPath $ExeFS).ProviderPath
$recompilerPath = (Resolve-Path -LiteralPath $Recompiler).ProviderPath
& python (Join-Path $probe 'generate.py') $exeFsPath $recompilerPath
if ($LASTEXITCODE -ne 0) {
    throw "Generating the local Pokopia CPU probe failed ($LASTEXITCODE)."
}

$sources = @(
    (Join-Path $probe 'guest_probe.c'),
    (Join-Path $probe 'rtld_boot.c'),
    (Join-Path $probe 'generated/rtld_7700000000.c'),
    (Join-Path $probe 'generated/rtld_2000000000.c')
)
& (Join-Path $PSScriptRoot 'toolchain/build-probe.ps1') `
    -ProbeSource $sources -ProbeInclude (Join-Path $recompilerPath 'include')
