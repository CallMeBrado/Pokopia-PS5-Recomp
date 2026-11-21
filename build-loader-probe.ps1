[CmdletBinding()]
param(
    [string] $ExeFS = 'G:/pokopia_dump_v2-0-1/updates/655360/exefs',
    [string] $Recompiler = 'C:/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp',
    [string] $SdkProbeObject
)

$ErrorActionPreference = 'Stop'
$cpuProbe = Join-Path $PSScriptRoot 'cpu-probe'
$loaderProbe = Join-Path $PSScriptRoot 'loader-probe'
$exeFsPath = (Resolve-Path -LiteralPath $ExeFS).ProviderPath
$recompilerPath = (Resolve-Path -LiteralPath $Recompiler).ProviderPath
foreach ($script in (Join-Path $cpuProbe 'generate.py'), (Join-Path $loaderProbe 'prepare.py')) {
    & python $script $exeFsPath $recompilerPath
    if ($LASTEXITCODE -ne 0) {
        throw "Probe input generation failed ($LASTEXITCODE)."
    }
}

$sources = @(
    (Join-Path $loaderProbe 'loader_probe.c'),
    (Join-Path $cpuProbe 'generated/rtld_7700000000.c')
)
$output = if ($SdkProbeObject) { 'out/sdk' } else { 'out/loader' }
& (Join-Path $PSScriptRoot 'toolchain/build-probe.ps1') `
    -ProbeSource $sources -ProbeInclude (Join-Path $recompilerPath 'include') `
    -LoaderAssets (Join-Path $loaderProbe 'generated') `
    -SdkProbeObject $SdkProbeObject -OutputDirectory (Join-Path $PSScriptRoot $output)

