[CmdletBinding()]
param(
    [string] $BuildTools = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sdk = Join-Path $root 'remote-service/upstream/ps4-payload-sdk'
$llvm = Join-Path $BuildTools 'VC/Tools/Llvm/x64/bin'
$work = (New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot 'build')).FullName
$env:TEMP = $work
$env:TMP = $work

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE" }
}

$flags = @('-target', 'x86_64-scie-ps4', '-isysroot', $sdk, '-isystem', (Join-Path $sdk 'target/include'),
    '-fvisibility-nodllstorageclass=default', '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '-fPIC', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror')
foreach ($name in 'probe', 'entry') {
    Invoke-Checked (Join-Path $llvm 'clang.exe') ($flags + @('-c', (Join-Path $PSScriptRoot "$name.c"),
        '-o', (Join-Path $work "$name.o")))
}
$libs = Join-Path $sdk 'target/lib'
$elf = Join-Path $work 'pokopia-partial-release-r2.elf'
Invoke-Checked (Join-Path $llvm 'ld.lld.exe') @('-m', 'elf_x86_64', '-pie', '--eh-frame-hdr', '-z',
    'max-page-size=0x4000', '--script', (Join-Path $sdk 'ldscripts/elf_x86_64.x'), '-o', $elf,
    '-L', $libs, (Join-Path $libs 'crt1.o'), (Join-Path $work 'probe.o'), (Join-Path $work 'entry.o'),
    '-lc', '-ldl', '-lpthread', '-lkernel_web', '-lSceLibcInternal')

$inspect = Join-Path $llvm 'llvm-readobj.exe'
Invoke-Checked $inspect @('--file-headers', '--program-headers', '--needed-libs', '--dyn-symbols', $elf) |
    Set-Content -LiteralPath (Join-Path $work 'elf-inspection.txt')

$developer = Join-Path $BuildTools 'Common7/Tools/VsDevCmd.bat'
$settings = & cmd.exe /d /s /c ('call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f $developer)
if ($LASTEXITCODE -ne 0) { throw 'VS developer environment unavailable' }
foreach ($line in $settings) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$env:TEMP = $work
$env:TMP = $work
Push-Location -LiteralPath $work
try {
    Invoke-Checked (Join-Path $llvm 'clang-cl.exe') @('/nologo', '/O2', '/std:c11', '/W4', '/WX',
        (Join-Path $PSScriptRoot 'probe.c'), (Join-Path $PSScriptRoot 'test_probe.c'), '/Fe:test-probe.exe')
    Invoke-Checked (Join-Path $work 'test-probe.exe') @() | Set-Content -LiteralPath (Join-Path $work 'host-test.log')
} finally {
    Pop-Location
}
Get-FileHash -LiteralPath $elf -Algorithm SHA256
