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
    [string]$ExpectedSha256 = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Assert-AuthenticodeValid {
    param([Parameter(Mandatory = $true)] [string]$Path)

    # Fail closed on any binary we did not build ourselves before it is executed,
    # extracted, or bundled: an Authenticode status other than Valid means the file is
    # unsigned or has been tampered with. This is the trust gate that does not depend on
    # a secret; a pinned SHA-256 (see -ExpectedSha256) is an additional defense.
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    if ($sig.Status -ne "Valid") {
        throw "Authenticode check failed for '$Path': status=$($sig.Status). Refusing to trust an unsigned or tampered binary."
    }
    Write-Host "Authenticode OK: $Path" -ForegroundColor DarkGray
}

Write-Host "=== smartmontools Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

$ToolName = "smartmontools"
$Version = "7.5"

if ([string]::IsNullOrEmpty($DestinationPath)) {
    $DestinationPath = Join-Path $PSScriptRoot "..\tools\smartmontools"
}
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)

# Unique per-run temp directory: a fixed, predictable %TEMP% path lets a concurrent or
# malicious process pre-create or swap files between download, verification, extraction,
# and copy, and could collide with the destination. A fresh GUID-named directory closes
# that window and cannot overlap the destination.
$TempDir = Join-Path $env:TEMP ("sak_bundle_smartmontools_" + [guid]::NewGuid().ToString("N"))

# Official SourceForge direct download (Windows installer, includes 64-bit binaries)
$DownloadUrl = "https://downloads.sourceforge.net/project/smartmontools/smartmontools/$Version/smartmontools-$Version.win32-setup.exe"

Write-Host "Tool:        $ToolName v$Version" -ForegroundColor Yellow
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow
Write-Host ""

# -- Check existing installation -----------------------------------------------

$SmartctlExe = Join-Path $DestinationPath "smartctl.exe"
if ((Test-Path -LiteralPath $SmartctlExe) -and (-not $Force)) {
    try {
        # Do not trust a pre-existing destination binary blindly: verify its signature
        # and that it reports the expected version before short-circuiting. On any
        # failure fall through to a fresh, re-verified download rather than accepting it.
        Assert-AuthenticodeValid -Path $SmartctlExe
        $existingVersion = & $SmartctlExe --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
        if ($LASTEXITCODE -ne 0 -or -not $existingVersion -or "$existingVersion" -notmatch [regex]::Escape($Version)) {
            throw "Existing smartctl.exe did not report version $Version (exit $LASTEXITCODE)."
        }
        Write-Host "smartctl already bundled: $existingVersion" -ForegroundColor Green
        Write-Host "Use -Force to re-download." -ForegroundColor Gray
        exit 0
    } catch {
        Write-Host "Existing smartctl.exe failed trust/version check, re-downloading..." -ForegroundColor Yellow
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

    # Resolve curl.exe from System32 by absolute path so an attacker-writable PATH entry
    # cannot redirect the download through a malicious curl. --fail makes curl exit
    # non-zero on an HTTP error body instead of writing it as a "successful" file.
    $SystemCurl = Join-Path $env:SystemRoot "System32\curl.exe"
    if (Test-Path -LiteralPath $SystemCurl) {
        & $SystemCurl -L --fail -o $InstallerPath $DownloadUrl --silent --show-error
        if ($LASTEXITCODE -ne 0) {
            throw "curl download failed with exit code $LASTEXITCODE"
        }
    } else {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $InstallerPath -UseBasicParsing -MaximumRedirection 10
    }

    if (-not (Test-Path -LiteralPath $InstallerPath)) {
        throw "Download failed: installer file not found"
    }

    $FileSize = (Get-Item -LiteralPath $InstallerPath).Length
    Write-Host "Downloaded: $([math]::Round($FileSize / 1MB, 2)) MB" -ForegroundColor Green

    # -- Verify SHA-256 and Authenticode ---------------------------------------

    $ActualHash = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash
    Write-Host ""
    Write-Host "SHA-256: $ActualHash" -ForegroundColor Yellow
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
        if ($ActualHash -ne $ExpectedSha256.Trim().ToUpperInvariant()) {
            throw "SHA-256 mismatch: expected $ExpectedSha256 but downloaded $ActualHash."
        }
        Write-Host "SHA-256 matches pinned value." -ForegroundColor Green
    }
    # Fail closed on an unsigned or tampered installer before it is executed or extracted.
    Assert-AuthenticodeValid -Path $InstallerPath

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
    if (-not (Test-Path -LiteralPath $SmartctlSrc)) {
        # Try alternate paths
        $SmartctlSrc = Get-ChildItem -LiteralPath $ExtractDir -Recurse -Filter "smartctl.exe" |
            Select-Object -First 1 -ExpandProperty FullName
        if (-not $SmartctlSrc) {
            Write-Host "Contents of extract dir:" -ForegroundColor DarkYellow
            Get-ChildItem -LiteralPath $ExtractDir -Recurse | ForEach-Object { Write-Host "  $($_.FullName)" }
            throw "smartctl.exe not found after extraction"
        }
    }

    # The extracted path can come from a recursive search, so bind trust to the binary
    # itself: fail closed unless the selected smartctl.exe carries a valid signature.
    Assert-AuthenticodeValid -Path $SmartctlSrc
    Copy-Item -LiteralPath $SmartctlSrc -Destination $SmartctlExe -Force
    Write-Host "Copied smartctl.exe to $DestinationPath" -ForegroundColor Green

    # -- Copy license file ------------------------------------------------------

    $LicenseSrc = Join-Path $ExtractDir "doc\COPYING"
    if (-not (Test-Path -LiteralPath $LicenseSrc)) {
        $LicenseSrc = Get-ChildItem -LiteralPath $ExtractDir -Recurse -Filter "COPYING" |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($LicenseSrc -and (Test-Path -LiteralPath $LicenseSrc)) {
        Copy-Item -LiteralPath $LicenseSrc -Destination (Join-Path $DestinationPath "COPYING") -Force
        Write-Host "Copied license file (GPLv2)" -ForegroundColor Green
    } else {
        Write-Host "WARNING: License file (COPYING) not found in extracted files" -ForegroundColor DarkYellow
    }

    # -- Create README ----------------------------------------------------------

    @"
smartmontools v$Version
=======================
Bundled for S.A.K. Utility diagnostic panel.

License: GNU General Public License v2.0 (GPLv2) - see THIRD_PARTY_LICENSES.md
Source:  https://www.smartmontools.org/
GitHub:  https://github.com/smartmontools/smartmontools

smartctl is used by the Diagnostic & Benchmarking panel to query
S.M.A.R.T. health data from storage devices (SATA, NVMe, USB).
"@ | Set-Content (Join-Path $DestinationPath "README.txt") -Encoding UTF8

    # -- Verify -----------------------------------------------------------------

    Write-Host ""
    # Fail closed: the bundle is only complete if the copied binary runs, exits 0, and
    # reports the expected version. A swallowed warning here would let an unusable or
    # wrong binary be declared "bundled".
    $VersionOutput = & $SmartctlExe --version 2>&1 | Select-String "smartctl" | Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or -not $VersionOutput -or "$VersionOutput" -notmatch [regex]::Escape($Version)) {
        throw "Bundled smartctl.exe failed verification: exit=$LASTEXITCODE version='$VersionOutput' (expected $Version)."
    }
    Write-Host "Successfully bundled: $VersionOutput" -ForegroundColor Green

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
