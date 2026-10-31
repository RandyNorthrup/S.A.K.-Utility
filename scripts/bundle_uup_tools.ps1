# Copyright (c) 2025 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bundle_uup_tools.ps1 - Verifies the bundled UUP tools against pinned SHA-256 hashes.
#
# tools/uup holds aria2c.exe (UUP and Linux ISO downloads) and the patched UUPMediaConverter
# (UUP set -> ISO) with the companion files it reads at run time. Every file under tools/uup is
# pinned below. The script fails when a pinned file is missing or differs from its pin, and when
# a file is present that nothing pins (a leftover no gate would ever look at again). It changes
# nothing on a tree that verifies, so running it twice is the same as running it once.
#
# aria2c.exe alone can be restored here: when it is missing it is extracted from the pinned
# official release zip and then held to the same pin as everything else.
#
# What the converter reads, relative to UUPMediaConverter.exe:
#   CDImage\cdimage.exe    ISO writer (UUPMediaCreator CDImageWrapper)
#   Settings\Settings.ini  setup-media file list and edition names (IniReader)
#   libwim-15.dll          wimlib, loaded through ManagedWimLib (WimLibImaging)
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts/bundle_uup_tools.ps1

param(
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\tools\uup")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Pins --------------------------------------------------------------------------------------

# Path relative to tools/uup -> SHA-256 of the exact bytes that ship.
$Pins = [ordered]@{
    # aria2 1.37.0 official Windows build (aria2-1.37.0-win-64bit-build1.zip).
    "aria2c.exe"                      = "BE2099C214F63A3CB4954B09A0BECD6E2E34660B886D4C898D260FEBFE9D70C2"
    # Patched UUPMediaCreator 3.1.9.2 (upstream 71a24c3), self-contained single-file on .NET 8.
    "uupmc/UUPMediaConverter.exe"     = "8BC3145ABDF4A894A809C804F5416817F611667F55C9C6F0403C840E722FCA60"
    # wimlib 1.14.4, from the ManagedWimLib 2.6.0 NuGet package.
    "uupmc/libwim-15.dll"             = "592431EF475A2093F0D8FB60DB532E6B5CC31760084F4FC1F47ABB0A58502328"
    # Upstream src/UnifiedUpdatePlatform.Media.Creator/CDImage/cdimage.exe (CDIMAGE 2.53m).
    "uupmc/CDImage/cdimage.exe"       = "A3B761293D213BE7FE6F01526CE4B3C18DCC8F95B92E24488011837377DF6582"
    # Upstream Settings.ini at 885f2e36 (git blob 65dff311, includes PR #194's six files).
    "uupmc/Settings/Settings.ini"     = "3447F26FFAED3CA2FB84090B4AF6CF022D4EA4E23E7371CDD9F5980EBB165684"
}

$Aria2Url = "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip"
$Aria2ZipSha256 = "67D015301EEF0B612191212D564C5BB0A14B5B9C4796B76454276A4D28D9B288"

# --- Functions ---------------------------------------------------------------------------------

function Write-Status {
    param([string]$Message)
    Write-Host "[UUP Tools] $Message" -ForegroundColor Cyan
}

function Write-Success {
    param([string]$Message)
    Write-Host "[UUP Tools] $Message" -ForegroundColor Green
}

function Write-Err {
    param([string]$Message)
    Write-Host "[UUP Tools] ERROR: $Message" -ForegroundColor Red
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

# Restores a missing aria2c.exe from the pinned official zip. The extracted file is verified
# against its own pin by the main loop, like every other file.
function Restore-Aria2 {
    param([Parameter(Mandatory = $true)][string]$Destination)

    $tempDir = Join-Path $env:TEMP "sak_uup_bundle_$(Get-Random)"
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    try {
        $zip = Join-Path $tempDir "aria2.zip"
        Write-Status "aria2c.exe missing; downloading $Aria2Url"
        $global:LASTEXITCODE = 0
        & curl.exe --fail --location --silent --show-error --output $zip $Aria2Url
        if ($LASTEXITCODE -ne 0) {
            throw "Download failed: curl.exe exited with code $LASTEXITCODE"
        }
        $zipSha = Get-Sha256 -Path $zip
        if ($zipSha -ne $Aria2ZipSha256) {
            throw "SHA-256 mismatch for the aria2 zip: expected $Aria2ZipSha256, got $zipSha"
        }
        Expand-Archive -LiteralPath $zip -DestinationPath $tempDir -Force
        $extracted = Join-Path $tempDir "aria2-1.37.0-win-64bit-build1\aria2c.exe"
        if (-not (Test-Path -LiteralPath $extracted -PathType Leaf)) {
            throw "aria2c.exe not found in the pinned zip"
        }
        Copy-Item -LiteralPath $extracted -Destination $Destination -Force
        Write-Success "aria2c.exe restored from the pinned release zip"
    }
    finally {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- Main --------------------------------------------------------------------------------------

Write-Status "=== UUP tools verification ==="
if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
    throw "UUP tools directory not found: $OutputDir"
}
$root = (Resolve-Path -LiteralPath $OutputDir).Path
Write-Status "Directory: $root"

$aria2Path = Join-Path $root "aria2c.exe"
if (-not (Test-Path -LiteralPath $aria2Path)) {
    Restore-Aria2 -Destination $aria2Path
}

$problems = @()
foreach ($relative in $Pins.Keys) {
    $path = Join-Path $root ($relative -replace '/', '\')
    # -PathType Leaf so a directory bearing the file's name cannot satisfy the pin.
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $problems += "MISSING: $relative"
        continue
    }
    $actual = Get-Sha256 -Path $path
    if ($actual -ne $Pins[$relative]) {
        $problems += "HASH MISMATCH: $relative (expected $($Pins[$relative]), got $actual)"
        continue
    }
    $sizeKB = [math]::Round((Get-Item -LiteralPath $path).Length / 1KB, 1)
    Write-Success "  OK: $relative ($sizeKB KB)"
}

# Every file present must be pinned: a stale leftover (an old nested CDImage\CDImage, a .NET
# Framework .dll.config, a tool nothing launches) would otherwise ship unexamined.
foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
    $relative = $file.FullName.Substring($root.Length).TrimStart('\') -replace '\\', '/'
    if (-not $Pins.Contains($relative)) {
        $problems += "UNPINNED FILE: $relative"
    }
}

if ($problems.Count -gt 0) {
    foreach ($problem in $problems) {
        Write-Err $problem
    }
    throw "UUP tools verification failed: $($problems.Count) problem(s) in $root"
}
Write-Success "=== All $($Pins.Count) UUP tool files match their pinned SHA-256 ==="
