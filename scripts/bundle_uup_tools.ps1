# Copyright (c) 2025 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# bundle_uup_tools.ps1 - Downloads and bundles UUP dump support tools
#
# This script downloads aria2c and verifies that the patched UUPMediaConverter
# (checked into the repo) is present. aria2c is the only runtime dependency
# that must be downloaded; the converter binary ships in tools/uup/uupmc/.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts/bundle_uup_tools.ps1
#
# Required tools after bundling:
#   tools/uup/aria2c.exe              - Multi-connection download manager
#   tools/uup/uupmc/                  - Patched UUP Media Creator
#     ├── UUPMediaConverter.exe        - UUP-to-ISO conversion utility
#     ├── UUPDownload.exe              - UUP download utility
#     ├── libwim-15.dll                - WIM library
#     └── CDImage/cdimage.exe          - CD/DVD image builder

param(
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\tools\uup"),
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Configuration ---
$Aria2Version = "1.37.0"
$Aria2Url = "https://github.com/aria2/aria2/releases/download/release-$Aria2Version/aria2-$Aria2Version-win-64bit-build1.zip"
$Aria2Sha256 = "67D015301EEF0B612191212D564C5BB0A14B5B9C4796B76454276A4D28D9B288"

# --- Functions ---

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

function Test-FileHash {
    param(
        [string]$FilePath,
        [string]$ExpectedHash
    )
    $actualHash = (Get-FileHash -Path $FilePath -Algorithm SHA256).Hash
    return $actualHash -eq $ExpectedHash.ToUpper()
}

function Get-SecureDownload {
    param(
        [string]$Url,
        [string]$OutputPath,
        [string]$ExpectedSha256,
        [string]$Description,
        [int]$MaxRetries = 3
    )

    if ((Test-Path $OutputPath) -and -not $Force) {
        if (Test-FileHash -FilePath $OutputPath -ExpectedHash $ExpectedSha256) {
            Write-Status "$Description already exists and hash matches. Skipping download."
            return $true
        }
        Write-Status "$Description exists but hash mismatch. Re-downloading..."
        Remove-Item $OutputPath -Force
    }

    for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
        Write-Status "Downloading $Description (attempt $attempt/$MaxRetries)..."
        Write-Status "  URL: $Url"

        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $webClient = New-Object System.Net.WebClient
            $webClient.DownloadFile($Url, $OutputPath)
            $webClient.Dispose()
        }
        catch {
            Write-Err "Download attempt $attempt failed: $_"
            if ($attempt -lt $MaxRetries) {
                $delay = $attempt * 10
                Write-Status "Retrying in $delay seconds..."
                Start-Sleep -Seconds $delay
                continue
            }
            return $false
        }

        if (Test-FileHash -FilePath $OutputPath -ExpectedHash $ExpectedSha256) {
            Write-Success "$Description downloaded and verified."
            return $true
        }

        Write-Err "SHA-256 hash mismatch for $Description (attempt $attempt)"
        Write-Err "  Expected: $ExpectedSha256"
        $actualHash = (Get-FileHash -Path $OutputPath -Algorithm SHA256).Hash
        Write-Err "  Actual:   $actualHash"
        Remove-Item $OutputPath -Force

        if ($attempt -lt $MaxRetries) {
            $delay = $attempt * 10
            Write-Status "Retrying in $delay seconds..."
            Start-Sleep -Seconds $delay
        }
    }

    return $false
}

# --- Main ---

Write-Status "=== UUP Dump Tools Bundler ==="
Write-Status "Output directory: $OutputDir"

# Create output directory
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$tempDir = Join-Path $env:TEMP "sak_uup_bundle_$(Get-Random)"
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

try {
    # --- Step 1: Download and extract aria2c ---
    $aria2Zip = Join-Path $tempDir "aria2.zip"
    $aria2Exe = Join-Path $OutputDir "aria2c.exe"

    if ((Test-Path $aria2Exe) -and -not $Force) {
        Write-Status "aria2c.exe already present. Use -Force to re-download."
    }
    else {
        if (-not (Get-SecureDownload -Url $Aria2Url -OutputPath $aria2Zip -ExpectedSha256 $Aria2Sha256 -Description "aria2c")) {
            throw "Failed to download aria2c"
        }

        Write-Status "Extracting aria2c..."
        Expand-Archive -Path $aria2Zip -DestinationPath $tempDir -Force

        $aria2ExtractDir = Get-ChildItem -Path $tempDir -Directory -Filter "aria2-*" | Select-Object -First 1
        if (-not $aria2ExtractDir) {
            throw "Could not find extracted aria2 directory"
        }

        Copy-Item -Path (Join-Path $aria2ExtractDir.FullName "aria2c.exe") -Destination $aria2Exe -Force
        Write-Success "aria2c.exe installed to $aria2Exe"
    }

    # --- Step 2: Verify patched UUPMediaConverter (checked into repo) ---
    $uupmcDir = Join-Path $OutputDir "uupmc"
    $uupmcCritical = @("UUPMediaConverter.exe", "UUPDownload.exe", "libwim-15.dll")

    Write-Status "Verifying patched UUPMediaConverter..."
    foreach ($file in $uupmcCritical) {
        $filePath = Join-Path $uupmcDir $file
        if (-not (Test-Path $filePath)) {
            Write-Err "MISSING: $filePath"
            Write-Err "UUPMediaConverter binaries must be checked into the repo at tools/uup/uupmc/"
            throw "Patched UUPMediaConverter not found: $file"
        }
        $sizeKB = [math]::Round((Get-Item $filePath).Length / 1024, 1)
        Write-Success "  OK: $file ($sizeKB KB)"
    }

    # --- Step 3: Final verification ---
    Write-Status "Verifying installation..."

    $requiredFiles = @(
        (Join-Path $OutputDir "aria2c.exe"),
        (Join-Path $uupmcDir "UUPMediaConverter.exe"),
        (Join-Path $uupmcDir "UUPDownload.exe")
    )

    $allPresent = $true
    foreach ($file in $requiredFiles) {
        if (Test-Path $file) {
            Write-Success "  OK: $file"
        }
        else {
            Write-Err "  MISSING: $file"
            $allPresent = $false
        }
    }

    # List uupmc contents
    if (Test-Path $uupmcDir) {
        Write-Status "UUP Media Creator contents:"
        $uupmcDirFull = (Resolve-Path $uupmcDir).Path
        Get-ChildItem -Path $uupmcDirFull -Recurse -File | ForEach-Object {
            $relativePath = $_.FullName.Substring($uupmcDirFull.Length + 1)
            $sizeKB = [math]::Round($_.Length / 1024, 1)
            Write-Status "  $relativePath ($sizeKB KB)"
        }
    }

    if ($allPresent) {
        Write-Success "=== All UUP tools bundled successfully ==="
    }
    else {
        throw "Some required files are missing"
    }
}
finally {
    # Clean up temp directory
    if (Test-Path $tempDir) {
        Remove-Item $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
