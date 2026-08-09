<#
.SYNOPSIS
    Downloads and bundles iPerf3 for S.A.K. Utility.
.DESCRIPTION
    Downloads the official iPerf3 Windows build, extracts iperf3.exe and its
    supporting DLL, and places them in tools/iperf3/.
.PARAMETER Force
    Re-download even if iperf3.exe is already present.
.PARAMETER DestinationPath
    Override the default destination (tools/iperf3). Must be a rooted local
    directory that is not a reparse point: the script writes into it and runs
    an iperf3.exe found there.
.PARAMETER ExpectedSha256
    Pinned SHA-256 of the downloaded release archive. When supplied (or when
    $PinnedSha256 below is filled in) a mismatch aborts the bundle.
#>

param(
    [string]$DestinationPath = "",
    [string]$ExpectedSha256 = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# -- Fail-closed helpers -------------------------------------------------------

# The destination is written with -Force and an iperf3.exe found there is EXECUTED
# for the version check, so it may only be a rooted local directory that is not a
# junction/symlink redirecting us somewhere else.
function Assert-SafeBundleDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Destination path is empty"
    }
    if ($Path.StartsWith("\\")) {
        throw "Destination path must be local, not a UNC/network path: $Path"
    }
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        throw "Destination path must be rooted: $Path"
    }
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not $item.PSIsContainer) {
            throw "Destination path exists and is not a directory: $Path"
        }
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Destination path is a reparse point (junction/symlink); refusing: $Path"
        }
    }
}

# Never execute through a redirection or a directory entry.
function Assert-SafeExecutable {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Executable not found: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) {
        throw "Executable path is a directory: $Path"
    }
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Executable is a reparse point (junction/symlink); refusing to run: $Path"
    }
}

# Returns the trimmed "iperf <version> ..." line, or $null when the executable
# could not be run or exited non-zero. Exit status is never ignored.
function Get-Iperf3VersionLine {
    param([Parameter(Mandatory = $true)][string]$ExePath)

    Assert-SafeExecutable -Path $ExePath
    $global:LASTEXITCODE = 0
    try {
        $output = & $ExePath --version 2>&1
    } catch {
        return $null
    }
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    $match = $output | Select-String -Pattern '^iperf\s' | Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }
    return $match.ToString().Trim()
}

Write-Host "=== iPerf3 Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

$ToolName = "iperf3"
$Version = "3.21"

if ([string]::IsNullOrEmpty($DestinationPath)) {
    $DestinationPath = Join-Path $PSScriptRoot "..\tools\iperf3"
}
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)
Assert-SafeBundleDirectory -Path $DestinationPath

# Pinned SHA-256 of iperf-$Version-win64.zip. Leave empty ONLY until the release
# digest is recorded here; with a pin (or -ExpectedSha256) a mismatched archive
# aborts the bundle instead of being extracted and executed.
$PinnedSha256 = ""
$RequiredSha256 = $PinnedSha256
if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
    $RequiredSha256 = $ExpectedSha256
}
$RequiredSha256 = $RequiredSha256.Trim().ToUpperInvariant()
if ($RequiredSha256 -and ($RequiredSha256 -notmatch '^[0-9A-F]{64}$')) {
    Write-Host "ERROR: expected SHA-256 must be 64 hex characters" -ForegroundColor Red
    exit 1
}

$TempDir = Join-Path $env:TEMP "sak_bundle_iperf3"

# Official iPerf3 Windows 64-bit build from ar51an (upstream-endorsed mirror)
$DownloadUrl = "https://github.com/ar51an/iperf3-win-builds/releases/download/$Version/iperf-$Version-win64.zip"

Write-Host "Tool:        $ToolName v$Version" -ForegroundColor Yellow
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow
Write-Host ""

# -- Check existing installation -----------------------------------------------

$Iperf3Exe = Join-Path $DestinationPath "iperf3.exe"
$VersionPattern = '^iperf\s+' + [regex]::Escape($Version) + '(\D|$)'
if ((Test-Path -LiteralPath $Iperf3Exe) -and (-not $Force)) {
    # Only an existing bundle that actually RUNS and reports exactly $Version counts
    # as already bundled; anything else (bad exit status, no output, wrong version)
    # falls through to a re-download instead of silently passing.
    $existingVersion = Get-Iperf3VersionLine -ExePath $Iperf3Exe
    if ($existingVersion -and ($existingVersion -match $VersionPattern)) {
        Write-Host "iperf3 already bundled: $existingVersion" -ForegroundColor Green
        Write-Host "Use -Force to re-download." -ForegroundColor Gray
        exit 0
    }
    Write-Host "Existing iperf3.exe is missing, unrunnable, or not v$Version -- re-downloading..." -ForegroundColor Yellow
}

# -- Prepare directories -------------------------------------------------------

Write-Host "Creating directories..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $DestinationPath | Out-Null
# Re-check after the create: New-Item -Force accepts an existing junction as "already a
# directory", so a redirection planted between the check above and this line would send
# every copy below somewhere else.
Assert-SafeBundleDirectory -Path $DestinationPath

if (Test-Path -LiteralPath $TempDir) {
    # %TEMP%\sak_bundle_iperf3 is a predictable path; never recurse a delete through a
    # junction/symlink planted there.
    $tempItem = Get-Item -LiteralPath $TempDir -Force
    if ($tempItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        Write-Host "ERROR: temp directory is a reparse point; refusing to delete through it: $TempDir" -ForegroundColor Red
        exit 1
    }
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
        # --fail turns an HTTP error status into a non-zero exit instead of saving the
        # error body as the "archive"; the exit status is then checked, not assumed.
        $global:LASTEXITCODE = 0
        & $curlExe.Source --fail --location --silent --show-error --output $ZipPath $DownloadUrl
        if ($LASTEXITCODE -ne 0) {
            throw "Download failed: curl.exe exited with code $LASTEXITCODE"
        }
    } else {
        Invoke-WebRequest -Uri $DownloadUrl -OutFile $ZipPath -UseBasicParsing
    }

    if (-not (Test-Path -LiteralPath $ZipPath)) {
        throw "Download failed: zip file not found"
    }

    $FileSize = (Get-Item -LiteralPath $ZipPath).Length
    if ($FileSize -le 0) {
        throw "Download failed: zip file is empty"
    }
    Write-Host "Downloaded: $([math]::Round($FileSize / 1MB, 2)) MB" -ForegroundColor Green

    # -- Verify SHA-256 ---------------------------------------------------------

    $ActualHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToUpperInvariant()
    Write-Host ""
    if ($RequiredSha256) {
        if ($ActualHash -ne $RequiredSha256) {
            throw ("SHA-256 mismatch for ${DownloadUrl}: expected $RequiredSha256, got " +
                   "$ActualHash -- refusing to bundle an unverified archive")
        }
        Write-Host "SHA-256 verified: $ActualHash" -ForegroundColor Green
    } else {
        Write-Host "SHA-256: $ActualHash" -ForegroundColor Yellow
        Write-Host "WARNING: no pinned SHA-256 -- this archive is UNVERIFIED. Record the release digest in the PinnedSha256 constant above, or pass -ExpectedSha256." -ForegroundColor DarkYellow
    }

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

    # Copy companion DLLs (cygwin1.dll or similar runtime DLLs shipped with the build).
    # Enumeration errors are NOT swallowed: a directory we cannot read must abort the
    # bundle rather than silently produce an incomplete runtime. Whether the build needs
    # any DLL at all is proved by the strict version check at the end.
    $CompanionDlls = @(Get-ChildItem -LiteralPath (Split-Path $Iperf3Src) -Filter "*.dll" -File)
    foreach ($dll in $CompanionDlls) {
        Copy-Item $dll.FullName (Join-Path $DestinationPath $dll.Name) -Force
        Write-Host "Copied $($dll.Name) to $DestinationPath" -ForegroundColor Green
    }
    Write-Host "Companion DLLs copied: $($CompanionDlls.Count)" -ForegroundColor Gray

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

License: BSD 3-Clause -- see THIRD_PARTY_LICENSES.md
Source:  https://github.com/esnet/iperf
Builds:  https://github.com/ar51an/iperf3-win-builds

iPerf3 is used by the Network Diagnostic panel to measure LAN bandwidth
between two machines (client/server mode).
"@ | Set-Content (Join-Path $DestinationPath "README.txt") -Encoding UTF8

    # -- Verify -----------------------------------------------------------------

    Write-Host ""
    # The bundle is only "complete" when the copied binary actually runs, exits 0, and
    # reports the exact version this script bundled. A failed check aborts (exit 1).
    $VersionOutput = Get-Iperf3VersionLine -ExePath $Iperf3Exe
    if (-not $VersionOutput) {
        throw "Bundled iperf3.exe did not run successfully (--version produced no usable output)"
    }
    if ($VersionOutput -notmatch $VersionPattern) {
        throw "Bundled iperf3.exe reports '$VersionOutput' but v$Version was expected"
    }
    Write-Host "Successfully bundled: $VersionOutput" -ForegroundColor Green

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
