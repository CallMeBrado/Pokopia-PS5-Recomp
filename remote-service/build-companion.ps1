[CmdletBinding()]
param([string] $Distribution = (Join-Path $PSScriptRoot 'build/windows'))

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$name = 'PS4 Companion'
$settings = Join-Path $build 'settings'
$dist = $Distribution
$output = Join-Path $dist $name
$work = Join-Path $build 'companion-work'
$scratch = Join-Path $build 'companion-temp'

foreach ($file in 'client-ip', 'view-token') {
    if (!(Test-Path -LiteralPath (Join-Path $settings $file) -PathType Leaf)) {
        throw 'Build the PS4 LAN Service package first to create its matching PC settings.'
    }
}
New-Item -ItemType Directory -Force -Path $work, $scratch | Out-Null
$env:PYINSTALLER_CONFIG_DIR = Join-Path $work 'cache'
$env:TEMP = $scratch
$env:TMP = $scratch
$env:PYTHONDONTWRITEBYTECODE = '1'
$python = (Get-Command python.exe -ErrorAction Stop).Source
& $python -B -m PyInstaller --noconfirm --onedir --windowed --name $name `
    --distpath $dist --workpath $work --specpath $work `
    --icon (Join-Path $root '../assets/icon0.png') (Join-Path $root 'companion.py')
if ($LASTEXITCODE -ne 0) { throw "Companion build exited $LASTEXITCODE." }

$outputSettings = Join-Path $output 'settings'
New-Item -ItemType Directory -Force -Path $outputSettings | Out-Null
foreach ($file in 'client-ip', 'view-token') {
    Copy-Item -LiteralPath (Join-Path $settings $file) -Destination (Join-Path $outputSettings $file)
}
Get-Item -LiteralPath (Join-Path $output "$name.exe") | Select-Object FullName, Length
