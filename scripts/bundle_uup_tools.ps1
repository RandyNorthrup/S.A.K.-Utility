# Copyright (c) 2025 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: MIT
#
# bundle_uup_tools.ps1 - Downloads and bundles UUP dump converter tools
#
# This script downloads aria2c and the uup-converter-wimlib package,
# then extracts them into tools/uup/ for build-time bundling.
# These tools enable portable ISO building from UUP files at runtime.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts/bundle_uup_tools.ps1
#
# Required tools after bundling:
#   tools/uup/aria2c.exe          - Multi-connection download manager
#   tools/uup/uup-converter-wimlib/ - UUP-to-ISO converter (wimlib-based)
#     ├── convert-UUP.cmd              - Main conversion script
#     ├── wimlib-imagex.exe        - WIM image manipulation tool
#     └── (supporting DLLs/files)

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

$ConverterUrl = "https://uupdump.net/misc/uup-converter-wimlib-v120z.7z"
$ConverterSha256 = "9c03f6153c90859882e507cb727b9963f28c8bbf3e6eca51ff7ed286d5267c4c"

$SevenZrUrl = "https://uupdump.net/misc/7zr.exe"
$SevenZrSha256 = "72c98287b2e8f85ea7bb87834b6ce1ce7ce7f41a8c97a81b307d4d4bf900922b"

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
        [string]$Description
    )

    if ((Test-Path $OutputPath) -and -not $Force) {
        if (Test-FileHash -FilePath $OutputPath -ExpectedHash $ExpectedSha256) {
            Write-Status "$Description already exists and hash matches. Skipping download."
            return $true
        }
        Write-Status "$Description exists but hash mismatch. Re-downloading..."
        Remove-Item $OutputPath -Force
    }

    Write-Status "Downloading $Description..."
    Write-Status "  URL: $Url"

    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $webClient = New-Object System.Net.WebClient
        $webClient.DownloadFile($Url, $OutputPath)
        $webClient.Dispose()
    }
    catch {
        Write-Err "Failed to download $Description : $_"
        return $false
    }

    if (-not (Test-FileHash -FilePath $OutputPath -ExpectedHash $ExpectedSha256)) {
        Write-Err "SHA-256 hash mismatch for $Description"
        Write-Err "  Expected: $ExpectedSha256"
        $actualHash = (Get-FileHash -Path $OutputPath -Algorithm SHA256).Hash
        Write-Err "  Actual:   $actualHash"
        Remove-Item $OutputPath -Force
        return $false
    }

    Write-Success "$Description downloaded and verified."
    return $true
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

    # --- Step 2: Download 7zr.exe for extracting the converter ---
    $sevenZrPath = Join-Path $tempDir "7zr.exe"
    if (-not (Get-SecureDownload -Url $SevenZrUrl -OutputPath $sevenZrPath -ExpectedSha256 $SevenZrSha256 -Description "7zr.exe")) {
        throw "Failed to download 7zr.exe"
    }

    # --- Step 3: Download and extract uup-converter-wimlib ---
    $converterDir = Join-Path $OutputDir "converter"

    if ((Test-Path $converterDir) -and -not $Force) {
        $convertCmd = Join-Path $converterDir "convert-UUP.cmd"
        if (Test-Path $convertCmd) {
            Write-Status "UUP converter already present. Use -Force to re-download."
        }
        else {
            $Force = $true
        }
    }

    if (-not (Test-Path (Join-Path $converterDir "convert-UUP.cmd")) -or $Force) {
        $converter7z = Join-Path $tempDir "uup-converter-wimlib.7z"
        if (-not (Get-SecureDownload -Url $ConverterUrl -OutputPath $converter7z -ExpectedSha256 $ConverterSha256 -Description "uup-converter-wimlib")) {
            throw "Failed to download uup-converter-wimlib"
        }

        Write-Status "Extracting uup-converter-wimlib..."
        if (Test-Path $converterDir) {
            Remove-Item $converterDir -Recurse -Force
        }
        New-Item -ItemType Directory -Path $converterDir -Force | Out-Null

        $extractTemp = Join-Path $tempDir "converter_extract"
        New-Item -ItemType Directory -Path $extractTemp -Force | Out-Null

        & $sevenZrPath x $converter7z "-o$extractTemp" -y | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to extract uup-converter-wimlib (7zr exit code: $LASTEXITCODE)"
        }

        # Copy extracted files to converter directory
        $extractedFiles = Get-ChildItem -Path $extractTemp -Recurse
        foreach ($file in $extractedFiles) {
            $relativePath = $file.FullName.Substring($extractTemp.Length + 1)
            $destPath = Join-Path $converterDir $relativePath
            if ($file.PSIsContainer) {
                New-Item -ItemType Directory -Path $destPath -Force | Out-Null
            }
            else {
                $destDir = Split-Path $destPath -Parent
                if (-not (Test-Path $destDir)) {
                    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
                }
                Copy-Item -Path $file.FullName -Destination $destPath -Force
            }
        }

        Write-Success "UUP converter installed to $converterDir"
    }

    # --- Step 4: Verify installation ---
    Write-Status "Verifying installation..."

    $requiredFiles = @(
        (Join-Path $OutputDir "aria2c.exe"),
        (Join-Path $converterDir "convert-UUP.cmd")
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

    # List converter contents
    if (Test-Path $converterDir) {
        Write-Status "Converter contents:"
        $converterDirFull = (Resolve-Path $converterDir).Path
        Get-ChildItem -Path $converterDirFull -Recurse -File | ForEach-Object {
            $relativePath = $_.FullName.Substring($converterDirFull.Length + 1)
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
