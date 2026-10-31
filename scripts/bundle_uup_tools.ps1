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
    "aria2c.exe"                                    = "BE2099C214F63A3CB4954B09A0BECD6E2E34660B886D4C898D260FEBFE9D70C2"
    # UUPMediaCreator 885f2e36 + the S.A.K. patch, self-contained single-file on .NET 10.0.14.
    # Rebuilt bit for bit by scripts/build_uup_media_converter.ps1 (uupmc/BUILD-NOTES.md).
    "uupmc/UUPMediaConverter.exe"                   = "D5C609FEA3A3F90AF5D119990E46CA7F5725726A8EFC690C8AE47DB63EFE8165"
    "uupmc/UUPMediaCreator-885f2e36-sak.patch"      = "C4D3C17C2258A4C501EFE5E0DC8717B8D5356AF738CF95C387E61CD463457922"
    "uupmc/BUILD-NOTES.md"                          = "08C263DF93285CDC598911FC2EEB55223C437C8EA8ED190C01381B04066D84EA"
    # wimlib 1.14.5, the DLL from wimlib's wimlib-1.14.5-windows-x86_64-bin.zip (zip SHA-256
    # 2F446D6FA3866582175F1A22A7BE198EEEE0AEC7ABA5B4E04AD25C99EAE2D265), with its signed source
    # tarball and license files.
    "uupmc/libwim-15.dll"                           = "BA853EE1E3FC5F5798581F02E8E066BA07A0A2375F0BF444FE981431FD508495"
    "uupmc/wimlib/wimlib-1.14.5.tar.gz"             = "84221A3ABD5B91228F15F8E6065C335A336237B5738197B75BF419EEA561A194"
    "uupmc/wimlib/wimlib-1.14.5.tar.gz.sig"         = "2ABB5FE21D16EB84C9C7260BB971F2CE04923DA491011F0E9211DA0ADE06B20B"
    "uupmc/wimlib/COPYING.txt"                      = "ED5CC16630B6AA5EDAF9CFD440DBF5EDA8E2257471752EAD93634AC8EB102583"
    "uupmc/wimlib/COPYING.LGPL.txt"                 = "B14C366C41CC9F62DC20EC1A0885F278CF9681076D98027012A910FCA5E89639"
    "uupmc/wimlib/COPYING.GPLv3.txt"                = "230184F60BAE2FEAF244F10A8BAC053C8FF33A183BCC365B4D8B876D2B7F4809"
    "uupmc/wimlib/COPYING.libdivsufsort-lite.txt"   = "6783C2BE541A8871BCCFD0E9A26253DEF3A32922FB1DA5344B6BBD43CBAB851F"
    # Upstream src/UnifiedUpdatePlatform.Media.Creator/CDImage/cdimage.exe (CDIMAGE 2.53m).
    "uupmc/CDImage/cdimage.exe"                     = "A3B761293D213BE7FE6F01526CE4B3C18DCC8F95B92E24488011837377DF6582"
    # Upstream Settings.ini at 885f2e36 (git blob 65dff311, includes PR #194's six files).
    "uupmc/Settings/Settings.ini"                   = "3447F26FFAED3CA2FB84090B4AF6CF022D4EA4E23E7371CDD9F5980EBB165684"
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
