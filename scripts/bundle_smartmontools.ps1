<#
.SYNOPSIS
    Downloads and bundles smartmontools (smartctl.exe) for S.A.K. Utility.
.DESCRIPTION
    Downloads the official smartmontools Windows installer from SourceForge,
    extracts smartctl.exe, and places it in tools/smartmontools/.
.PARAMETER Force
    Re-download even if smartctl.exe is already present.
.PARAMETER DestinationPath
    Override the default destination (tools/smartmontools).
#>

param(
    [string]$DestinationPath = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

Write-Host "=== smartmontools Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

$ToolName = "smartmontools"
$Version = "7.4"

if ([string]::IsNullOrEmpty($DestinationPath)) {
    $DestinationPath = Join-Path $PSScriptRoot "..\tools\smartmontools"
}
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)

$TempDir = Join-Path $env:TEMP "sak_bundle_smartmontools"

# Official SourceForge direct download (Windows installer, includes 64-bit binaries)
$DownloadUrl = "https://downloads.sourceforge.net/project/smartmontools/smartmontools/$Version/smartmontools-$Version-1.win32-setup.exe"

Write-Host "Tool:        $ToolName v$Version" -ForegroundColor Yellow
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow
Write-Host ""

# -- Check existing installation -----------------------------------------------

$SmartctlExe = Join-Path $DestinationPath "smartctl.exe"
if ((Test-Path $SmartctlExe) -and (-not $Force)) {
    try {
        $existingVersion = & $SmartctlExe --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
        Write-Host "smartctl already bundled: $existingVersion" -ForegroundColor Green
        Write-Host "Use -Force to re-download." -ForegroundColor Gray
        exit 0
    } catch {
        Write-Host "Existing smartctl.exe found but failed version check, re-downloading..." -ForegroundColor Yellow
    }
}

# -- Prepare directories -------------------------------------------------------

Write-Host "Creating directories..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $DestinationPath | Out-Null

if (Test-Path $TempDir) {
    Remove-Item -Path $TempDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

try {
    # -- Download ---------------------------------------------------------------

    $InstallerPath = Join-Path $TempDir "smartmontools-setup.exe"
    Write-Host ""
    Write-Host "Downloading smartmontools v$Version from SourceForge..." -ForegroundColor Cyan
    Write-Host "URL: $DownloadUrl" -ForegroundColor Gray

    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    # curl.exe handles SourceForge HTTP redirects more reliably than Invoke-WebRequest
    $curlExe = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curlExe) {
        & curl.exe -L -o $InstallerPath $DownloadUrl --silent --show-error
    } else {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $InstallerPath -UseBasicParsing -MaximumRedirection 10
    }

    if (-not (Test-Path $InstallerPath)) {
        throw "Download failed: installer file not found"
    }

    $FileSize = (Get-Item $InstallerPath).Length
    Write-Host "Downloaded: $([math]::Round($FileSize / 1MB, 2)) MB" -ForegroundColor Green

    # -- Verify SHA-256 ---------------------------------------------------------

    $ActualHash = (Get-FileHash -Path $InstallerPath -Algorithm SHA256).Hash
    Write-Host ""
    Write-Host "SHA-256: $ActualHash" -ForegroundColor Yellow
    Write-Host "NOTE: Verify this hash against the official release page." -ForegroundColor DarkYellow

    # -- Extract ----------------------------------------------------------------

    Write-Host ""
    Write-Host "Extracting smartctl.exe..." -ForegroundColor Cyan

    $ExtractDir = Join-Path $TempDir "extracted"
    New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null

    # Try 7-Zip first (handles NSIS extraction without elevation)
    $SevenZip = $null
    foreach ($path in @("C:\Program Files\7-Zip\7z.exe", "C:\Program Files (x86)\7-Zip\7z.exe")) {
        if (Test-Path $path) { $SevenZip = $path; break }
    }
    if (-not $SevenZip) {
        $SevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    }

    if ($SevenZip) {
        Write-Host "Using 7-Zip for extraction (no elevation required)..." -ForegroundColor Gray
        & $SevenZip x -o"$ExtractDir" -y $InstallerPath | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "7-Zip extraction failed with exit code $LASTEXITCODE"
        }
    } else {
        # Fallback: NSIS silent install (requires elevation)
        Write-Host "7-Zip not found, attempting NSIS silent install..." -ForegroundColor DarkYellow
        $process = Start-Process -FilePath $InstallerPath `
            -ArgumentList "/S", "/D=$ExtractDir" `
            -Wait -PassThru -NoNewWindow
        if ($process.ExitCode -ne 0) {
            throw "Installer exited with code $($process.ExitCode)"
        }
    }

    # Wait briefly for filesystem to settle
    Start-Sleep -Seconds 2

    # -- Copy smartctl.exe ------------------------------------------------------

    $SmartctlSrc = Join-Path $ExtractDir "bin\smartctl.exe"
    if (-not (Test-Path $SmartctlSrc)) {
        # Try alternate paths
        $SmartctlSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "smartctl.exe" |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $SmartctlSrc) {
            Write-Host "Contents of extract dir:" -ForegroundColor DarkYellow
            Get-ChildItem -Path $ExtractDir -Recurse | ForEach-Object { Write-Host "  $($_.FullName)" }
            throw "smartctl.exe not found after extraction"
        }
    }

    Copy-Item $SmartctlSrc $SmartctlExe -Force
    Write-Host "Copied smartctl.exe to $DestinationPath" -ForegroundColor Green

    # -- Copy license file ------------------------------------------------------

    $LicenseSrc = Join-Path $ExtractDir "doc\COPYING"
    if (-not (Test-Path $LicenseSrc)) {
        $LicenseSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "COPYING" |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($LicenseSrc -and (Test-Path $LicenseSrc)) {
        Copy-Item $LicenseSrc (Join-Path $DestinationPath "COPYING") -Force
        Write-Host "Copied license file (GPLv2)" -ForegroundColor Green
    } else {
        Write-Host "WARNING: License file (COPYING) not found in extracted files" -ForegroundColor DarkYellow
    }

    # -- Create README ----------------------------------------------------------

    @"
smartmontools v$Version
=======================
Bundled for S.A.K. Utility diagnostic panel.

License: GNU General Public License v2.0 (GPLv2) - see COPYING
Source:  https://www.smartmontools.org/
GitHub:  https://github.com/smartmontools/smartmontools

smartctl is used by the Diagnostic & Benchmarking panel to query
S.M.A.R.T. health data from storage devices (SATA, NVMe, USB).
"@ | Set-Content (Join-Path $DestinationPath "README.txt") -Encoding UTF8

    # -- Verify -----------------------------------------------------------------

    Write-Host ""
    try {
        $VersionOutput = & $SmartctlExe --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
        Write-Host "Successfully bundled: $VersionOutput" -ForegroundColor Green
    } catch {
        Write-Host "WARNING: smartctl.exe copied but version check failed" -ForegroundColor DarkYellow
    }

    Write-Host ""
    Write-Host "=== Bundle complete ===" -ForegroundColor Green
    Write-Host "smartctl.exe is ready at: $SmartctlExe" -ForegroundColor Cyan

} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Stack trace: $($_.ScriptStackTrace)" -ForegroundColor DarkRed
    exit 1

} finally {
    # Cleanup temp directory (uninstaller only needed for NSIS method, skip for 7-Zip)
    if (Test-Path $TempDir) {
        Write-Host "Cleaning up temporary files..." -ForegroundColor Gray
        Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
