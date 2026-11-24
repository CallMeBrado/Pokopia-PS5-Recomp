# SPDX-License-Identifier: GPL-3.0-or-later
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Launcher,
    [Parameter(Mandatory = $true)] [string] $Out,
    [Parameter(Mandatory = $true)] [ValidatePattern('\A[0-9a-fA-F]{64}\z')] [string] $Sha256
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this extractor with Windows PowerShell 5.1 (powershell.exe).'
}

$source = (Resolve-Path -LiteralPath $Launcher).ProviderPath
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
if ([string]::Equals($source, $destination, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The launcher and output ZIP must have different paths.'
}

# Reflection-only loading makes the manifest resources available without running
# the launcher. Verify the bytes on disk before publishing the sibling file.
$assembly = [Reflection.Assembly]::ReflectionOnlyLoadFrom($source)
$resource = $assembly.GetManifestResourceStream('PokopiaPS5Builder.ToolchainZip')
if ($null -eq $resource) {
    throw 'The launcher has no embedded PokopiaPS5Builder.ToolchainZip resource.'
}
$temporary = $null
try {
    $directory = [IO.Path]::GetDirectoryName($destination)
    [void] [IO.Directory]::CreateDirectory($directory)
    $part = [IO.Path]::Combine($directory, ".toolchain-$([Guid]::NewGuid().ToString('N')).part")
    $stream = [IO.File]::Open($part, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $temporary = $part
    try {
        $resource.CopyTo($stream)
    } finally {
        $stream.Dispose()
    }

    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $inputStream = [IO.File]::OpenRead($temporary)
        try {
            $actual = [BitConverter]::ToString($hash.ComputeHash($inputStream)).Replace('-', '')
        } finally {
            $inputStream.Dispose()
        }
    } finally {
        $hash.Dispose()
    }
    if ($actual -ine $Sha256) {
        throw "Embedded toolchain SHA-256 mismatch. Expected $Sha256, found $actual."
    }
    if ([IO.File]::Exists($destination)) {
        [IO.File]::Replace($temporary, $destination, [Management.Automation.Language.NullString]::Value)
    } else {
        [IO.File]::Move($temporary, $destination)
    }
    $temporary = $null
    Write-Output "Verified toolchain SHA-256 $actual"
} finally {
    $resource.Dispose()
    if ($null -ne $temporary) {
        [IO.File]::Delete($temporary)
    }
}
