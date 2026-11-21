[CmdletBinding()]
param(
    [string] $Source = (Join-Path $PSScriptRoot 'upstream/Control4Free-1.3.1'),
    [string] $PayloadSdk = (Join-Path $PSScriptRoot 'upstream/ps4-payload-sdk'),
    [string] $Llvm = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin',
    [string] $OutputDirectory = (Join-Path $PSScriptRoot 'build/controller')
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Tool) exited $LASTEXITCODE" }
}

$sourcePath = (Resolve-Path -LiteralPath $Source).ProviderPath
$payloadPath = (Resolve-Path -LiteralPath $PayloadSdk).ProviderPath
$llvmPath = (Resolve-Path -LiteralPath $Llvm).ProviderPath
$output = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$payloadObjects = (New-Item -ItemType Directory -Force -Path (Join-Path $output 'payload-obj')).FullName
$clang = Join-Path $llvmPath 'clang.exe'
$ld = Join-Path $llvmPath 'ld.lld.exe'
$version = (Get-Content -Raw -LiteralPath (Join-Path $sourcePath 'VERSION')).Trim()
$payloadElf = Join-Path $output 'control4free.elf'

Push-Location -LiteralPath $sourcePath
try {
    $client = Join-Path $payloadObjects 'client.c'
    $assets = Join-Path $payloadObjects 'assets.c'
    Invoke-Checked python @('tools/embed_client.py', 'client/index.html', $client)
    Invoke-Checked python @('tools/embed_file.py', $assets,
        'c4fManifest=client/manifest.webmanifest', 'c4fIcon=client/icon-192.png')

    # Match ps4-payload-sdk's orbis-clang and orbis-ld wrappers using native LLVM.
    $payloadFlags = @('-target', 'x86_64-scie-ps4', '-isysroot', $payloadPath,
        '-isystem', (Join-Path $payloadPath 'target/include'), '-fvisibility-nodllstorageclass=default',
        '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-fPIC', '-std=gnu11', '-O2',
        '-Iinclude', '-Ivendor/jsmn', '-Ivendor/qrcodegen', '-Ivendor/mbedtls/include',
        '-DMBEDTLS_CONFIG_FILE=<c4f_mbedtls_config.h>', "-DC4F_VERSION=`"$version`"")
    $payloadSources = @('src/main.c', 'src/log.c', 'src/vda.c', 'src/klog_line.c', 'src/feedback.c',
        'src/web.c', 'src/net.c', 'src/rtc.c', 'src/stun.c', 'src/sctp.c', 'vendor/qrcodegen/qrcodegen.c',
        $client, $assets)
    $objects = foreach ($file in $payloadSources) {
        $obj = Join-Path $payloadObjects (([IO.Path]::GetFileNameWithoutExtension($file)) + '.o')
        Invoke-Checked $clang ($payloadFlags + @('-Wall', '-Wextra', '-Wpointer-arith', '-c', $file, '-o', $obj))
        $obj
    }
    $mbedtlsObjects = foreach ($file in Get-ChildItem -LiteralPath 'vendor/mbedtls/library' -Filter '*.c' | Sort-Object Name) {
        $obj = Join-Path $payloadObjects ('mbedtls-' + $file.BaseName + '.o')
        Invoke-Checked $clang ($payloadFlags + @('-c', $file.FullName, '-o', $obj))
        $obj
    }
    $payloadLibraries = Join-Path $payloadPath 'target/lib'
    Invoke-Checked $ld (@('-m', 'elf_x86_64', '-pie', '--eh-frame-hdr', '-z', 'max-page-size=0x4000',
        '--script', (Join-Path $payloadPath 'ldscripts/elf_x86_64.x'), '-o', $payloadElf,
        '-L', $payloadLibraries, (Join-Path $payloadLibraries 'crt1.o')) + $objects + $mbedtlsObjects +
        @('-lc', '-lScePad', '-lSceUserService', '-ldl', '-lpthread', '-lkernel_web', '-lSceLibcInternal', '-lSceNet'))
    Get-Item -LiteralPath $payloadElf
} finally {
    Pop-Location
}
