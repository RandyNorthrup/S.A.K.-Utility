<#
.SYNOPSIS
    Downloads and bundles iPerf3 for S.A.K. Utility.
.DESCRIPTION
    Downloads the official iPerf3 Windows build, extracts iperf3.exe and its
    supporting DLL, and places them in tools/iperf3/.
.PARAMETER Force
    Re-download even if iperf3.exe is already present.
.PARAMETER DestinationPath
    Override the default destination (tools/iperf3).
#>

param(
    [string]$DestinationPath = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

Write-Host "=== iPerf3 Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

$ToolName = "iperf3"
$Version = "3.20"

if ([string]::IsNullOrEmpty($DestinationPath)) {
    $DestinationPath = Join-Path $PSScriptRoot "..\tools\iperf3"
}
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)

$TempDir = Join-Path $env:TEMP "sak_bundle_iperf3"

# Official iPerf3 Windows 64-bit build from ar51an (upstream-endorsed mirror)
$DownloadUrl = "https://github.com/ar51an/iperf3-win-builds/releases/download/$Version/iperf-$Version-win64.zip"

Write-Host "Tool:        $ToolName v$Version" -ForegroundColor Yellow
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow
Write-Host ""

# -- Check existing installation -----------------------------------------------

$Iperf3Exe = Join-Path $DestinationPath "iperf3.exe"
if ((Test-Path $Iperf3Exe) -and (-not $Force)) {
    try {
        $existingVersion = & $Iperf3Exe --version 2>&1 | Select-String "iperf" | Select-Object -First 1
        Write-Host "iperf3 already bundled: $existingVersion" -ForegroundColor Green
        Write-Host "Use -Force to re-download." -ForegroundColor Gray
        exit 0
    } catch {
        Write-Host "Existing iperf3.exe found but failed version check, re-downloading..." -ForegroundColor Yellow
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

    $ZipPath = Join-Path $TempDir "iperf3.zip"
    Write-Host ""
    Write-Host "Downloading iPerf3 v$Version..." -ForegroundColor Cyan
    Write-Host "URL: $DownloadUrl" -ForegroundColor Gray

    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    $curlExe = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curlExe) {
        & curl.exe -L -o $ZipPath $DownloadUrl --silent --show-error
    } else {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing
    }

    if (-not (Test-Path $ZipPath)) {
        throw "Download failed: zip file not found"
    }

    $FileSize = (Get-Item $ZipPath).Length
    Write-Host "Downloaded: $([math]::Round($FileSize / 1MB, 2)) MB" -ForegroundColor Green

    # -- Verify SHA-256 ---------------------------------------------------------

    $ActualHash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash
    Write-Host ""
    Write-Host "SHA-256: $ActualHash" -ForegroundColor Yellow
    Write-Host "NOTE: Verify this hash against the official release page." -ForegroundColor DarkYellow

    # -- Extract ----------------------------------------------------------------

    Write-Host ""
    Write-Host "Extracting iperf3..." -ForegroundColor Cyan

    $ExtractDir = Join-Path $TempDir "extracted"
    Expand-Archive -Path $ZipPath -DestinationPath $ExtractDir -Force

    # -- Copy iperf3.exe and cygwin DLL -----------------------------------------

    $Iperf3Src = Get-ChildItem -Path $ExtractDir -Recurse -Filter "iperf3.exe" |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $Iperf3Src) {
        Write-Host "Contents of extract dir:" -ForegroundColor DarkYellow
        Get-ChildItem -Path $ExtractDir -Recurse | ForEach-Object { Write-Host "  $($_.FullName)" }
        throw "iperf3.exe not found after extraction"
    }

    Copy-Item $Iperf3Src $Iperf3Exe -Force
    Write-Host "Copied iperf3.exe to $DestinationPath" -ForegroundColor Green

    # Copy companion DLLs (cygwin1.dll or similar runtime DLLs shipped with the build)
    $CompanionDlls = Get-ChildItem -Path (Split-Path $Iperf3Src) -Filter "*.dll" -ErrorAction SilentlyContinue
    foreach ($dll in $CompanionDlls) {
        Copy-Item $dll.FullName (Join-Path $DestinationPath $dll.Name) -Force
        Write-Host "Copied $($dll.Name) to $DestinationPath" -ForegroundColor Green
    }

    # -- Copy license file ------------------------------------------------------

    $LicenseSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "LICENSE*" |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $LicenseSrc) {
        $LicenseSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "COPYING*" |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($LicenseSrc -and (Test-Path $LicenseSrc)) {
        Copy-Item $LicenseSrc (Join-Path $DestinationPath "LICENSE") -Force
        Write-Host "Copied license file" -ForegroundColor Green
    } else {
        Write-Host "WARNING: License file not found in extracted files" -ForegroundColor DarkYellow
    }

    # -- Create README ----------------------------------------------------------

    @"
iPerf3 v$Version
=================
Bundled for S.A.K. Utility network diagnostic panel.

License: BSD 3-Clause — see LICENSE
Source:  https://github.com/esnet/iperf
Builds:  https://github.com/ar51an/iperf3-win-builds

iPerf3 is used by the Network Diagnostic panel to measure LAN bandwidth
between two machines (client/server mode).
"@ | Set-Content (Join-Path $DestinationPath "README.txt") -Encoding UTF8

    # -- Verify -----------------------------------------------------------------

    Write-Host ""
    try {
        $VersionOutput = & $Iperf3Exe --version 2>&1 | Select-String "iperf" | Select-Object -First 1
        Write-Host "Successfully bundled: $VersionOutput" -ForegroundColor Green
    } catch {
        Write-Host "WARNING: iperf3.exe copied but version check failed" -ForegroundColor DarkYellow
    }

    Write-Host ""
    Write-Host "=== Bundle complete ===" -ForegroundColor Green
    Write-Host "iperf3.exe is ready at: $Iperf3Exe" -ForegroundColor Cyan

} catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Stack trace: $($_.ScriptStackTrace)" -ForegroundColor DarkRed
    exit 1

} finally {
    if (Test-Path $TempDir) {
        Write-Host "Cleaning up temporary files..." -ForegroundColor Gray
        Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
