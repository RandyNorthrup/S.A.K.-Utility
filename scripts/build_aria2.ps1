# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# build_aria2.ps1 - Rebuilds the bundled tools/uup/aria2c.exe from source.
#
# The sources are the tarballs kept in tools/uup/aria2/ (aria2 and every library linked into
# it). This script:
#
#   1. verifies each tarball, and any aria2-*-sak-*.patch, against the SHA-256 pinned below;
#   2. checks that the MSYS2 UCRT64 toolchain is the pinned one (tools/filesystem/_build/msys64
#      by default, the repo's gitignored build environment);
#   3. runs tools/uup/aria2/build_aria2.sh in that environment at BelowNormal priority with -Jobs
#      parallel make jobs;
#   4. refuses the result unless aria2c.exe imports Windows system DLLs only, and prints its
#      SHA-256 and `aria2c --version`.
#
# -Install copies the built aria2c.exe over tools/uup/aria2c.exe. Update the pin in
# scripts/bundle_uup_tools.ps1 when it changes. See tools/uup/aria2/BUILD-NOTES.md.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/build_aria2.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/build_aria2.ps1 -Install

[CmdletBinding()]
param(
    [string]$MsysRoot = (Join-Path $PSScriptRoot "..\tools\filesystem\_build\msys64"),
    [string]$WorkDir = (Join-Path $env:TEMP "sak-aria2-build"),
    [ValidateRange(1, 64)]
    [int]$Jobs = 2,
    [ValidateSet("Normal", "BelowNormal", "Idle")]
    [string]$Priority = "BelowNormal",
    # Build with a UCRT64 toolchain other than the pinned one (the output will not match the
    # pinned executable; record the new versions in BUILD-NOTES.md).
    [switch]$AllowToolchainChange,
    [switch]$Install
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Pins --------------------------------------------------------------------------------------

# Source tarballs in tools/uup/aria2/. Provenance (signature or independent checksum) is in
# BUILD-NOTES.md beside them.
$SourcePins = [ordered]@{
    "aria2-1.37.0.tar.xz"            = "60A420AD7085EB616CB6E2BDF0A7206D68FF3D37FB5A956DC44242EB2F79B66B"
    "zlib-1.3.2.tar.xz"              = "D7A0654783A4DA529D1BB793B7AD9C3318020AF77667BCAE35F95D0E42A792F3"
    "c-ares-1.34.9.tar.gz"           = "9364B70A12EB59A06830FB488D9357F90100DFF0D98B23EA3593A183D43E6F2F"
    "expat-2.9.0.tar.xz"             = "1E6371862CC31999B368C3B89B49994F0677E1BAB5F1B2B85AE3741F5D803051"
    "sqlite-autoconf-3540100.tar.gz" = "926EF8D3EAB93FBF6E06011901273EEC076B25594A4E1CDADD50C987F4F6D64E"
    "libssh2-1.11.1.tar.xz"          = "9954CB54C4F548198A7CBEBAD248BDC87DD64BD26185708A294B2B50771E3769"
}
# S.A.K. patches applied to the aria2 tree, if any. Every aria2-*-sak-*.patch present must be
# pinned here, so a stray patch file cannot change the build unnoticed.
$PatchPins = [ordered]@{
}

# MSYS2 UCRT64 packages that make up the toolchain (pacman -Q).
$ToolchainPins = [ordered]@{
    "mingw-w64-ucrt-x86_64-gcc"         = "16.2.0-6"
    "mingw-w64-ucrt-x86_64-binutils"    = "2.47-4"
    "mingw-w64-ucrt-x86_64-crt"         = "14.0.0.r569.g34eeafb7d-1"
    "mingw-w64-ucrt-x86_64-headers"     = "14.0.0.r569.g34eeafb7d-1"
    "mingw-w64-ucrt-x86_64-winpthreads" = "14.0.0.r569.g34eeafb7d-1"
    "mingw-w64-ucrt-x86_64-pkgconf"     = "1~3.0.10-1"
}

# SHA-256 of the aria2c.exe this script produced for the commit that pinned it.
$ExpectedExeSha256 = "0094FC1C68AE8BFB009B955874BB590A9CDCC4449A80A94DC3912AE2E5350A80"

# Windows DLLs aria2c.exe may import. Anything else would have to ship beside it.
$AllowedImports = @(
    "ADVAPI32.dll", "bcrypt.dll", "CRYPT32.dll", "IPHLPAPI.DLL", "KERNEL32.dll", "Secur32.dll",
    "SHELL32.dll", "WS2_32.dll", "WSOCK32.dll"
)

# --- Helpers -----------------------------------------------------------------------------------

function Write-Step {
    param([string]$Message)
    Write-Host "[aria2 build] $Message" -ForegroundColor Cyan
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Invoke-MsysBash {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string]$What
    )
    $global:LASTEXITCODE = 0
    & $script:bash -lc $Command
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "$What failed with exit code $code"
    }
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Not a drive-letter path: $full"
    }
    return "/" + $Matches[1].ToLowerInvariant() + "/" + ($Matches[2] -replace '\\', '/')
}

# --- Main --------------------------------------------------------------------------------------

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceDir = Join-Path $repoRoot "tools\uup\aria2"
$recipe = Join-Path $sourceDir "build_aria2.sh"

foreach ($name in $SourcePins.Keys) {
    $path = Join-Path $sourceDir $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Source tarball missing: $path"
    }
    $actual = Get-Sha256 -Path $path
    if ($actual -ne $SourcePins[$name]) {
        throw "SHA-256 mismatch for ${name}: expected $($SourcePins[$name]), got $actual"
    }
    Write-Step "verified $name"
}
foreach ($patch in @(Get-ChildItem -LiteralPath $sourceDir -File -Filter "aria2-*-sak-*.patch")) {
    if (-not $PatchPins.Contains($patch.Name)) {
        throw "Unpinned patch in ${sourceDir}: $($patch.Name)"
    }
    $actual = Get-Sha256 -Path $patch.FullName
    if ($actual -ne $PatchPins[$patch.Name]) {
        throw "SHA-256 mismatch for $($patch.Name): expected $($PatchPins[$patch.Name]), got $actual"
    }
    Write-Step "verified $($patch.Name)"
}
foreach ($name in $PatchPins.Keys) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceDir $name) -PathType Leaf)) {
        throw "Pinned patch missing: $name"
    }
}

$MsysRoot = [System.IO.Path]::GetFullPath($MsysRoot)
$script:bash = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $script:bash -PathType Leaf)) {
    throw ("MSYS2 not found at $MsysRoot. Extract the MSYS2 base archive there and install the " +
        "toolchain: pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf make")
}
if (-not [System.IO.Path]::IsPathRooted($WorkDir)) {
    throw "WorkDir must be an absolute path: $WorkDir"
}
if (Test-Path -LiteralPath $WorkDir) {
    $item = Get-Item -LiteralPath $WorkDir -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "WorkDir is a reparse point; refusing to build through it: $WorkDir"
    }
}

$savedEnv = @{}
$buildEnv = @{ MSYSTEM = "UCRT64"; CHERE_INVOKING = "1"; MSYS2_PATH_TYPE = "minimal" }
foreach ($name in $buildEnv.Keys) {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    [Environment]::SetEnvironmentVariable($name, $buildEnv[$name], "Process")
}
$self = [System.Diagnostics.Process]::GetCurrentProcess()
$savedPriority = $self.PriorityClass
Push-Location $repoRoot
try {
    # Toolchain check.
    $installed = @{}
    $global:LASTEXITCODE = 0
    $query = & $script:bash -lc ("pacman -Q " + ($ToolchainPins.Keys -join " "))
    if ($LASTEXITCODE -ne 0) {
        throw "pacman -Q failed; install the pinned UCRT64 toolchain (see BUILD-NOTES.md)"
    }
    foreach ($line in $query) {
        $parts = "$line".Trim() -split '\s+'
        if ($parts.Count -eq 2) { $installed[$parts[0]] = $parts[1] }
    }
    $drift = @()
    foreach ($pkg in $ToolchainPins.Keys) {
        $have = if ($installed.ContainsKey($pkg)) { $installed[$pkg] } else { "(missing)" }
        Write-Step ("toolchain {0} {1}" -f $pkg, $have)
        if ($have -ne $ToolchainPins[$pkg]) {
            $drift += "$pkg is $have, pinned $($ToolchainPins[$pkg])"
        }
    }
    if ($drift.Count -gt 0) {
        $message = "UCRT64 toolchain differs from the pinned one:`n  " + ($drift -join "`n  ")
        if (-not $AllowToolchainChange) {
            throw ($message + "`nPass -AllowToolchainChange to build anyway.")
        }
        Write-Host "[aria2 build] $message" -ForegroundColor Yellow
    }

    # Child processes inherit a BelowNormal or Idle priority class from their parent.
    $self.PriorityClass = [System.Diagnostics.ProcessPriorityClass]$Priority
    $command = "bash '{0}' '{1}' '{2}' {3}" -f (ConvertTo-MsysPath $recipe),
        (ConvertTo-MsysPath $sourceDir), (ConvertTo-MsysPath $WorkDir), $Jobs
    Write-Step "running $command"
    Invoke-MsysBash -Command $command -What "build_aria2.sh"
}
finally {
    $self.PriorityClass = $savedPriority
    Pop-Location
    foreach ($name in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], "Process")
    }
}

$builtExe = Join-Path $WorkDir "aria2c.exe"
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
    throw "build_aria2.sh reported success but produced no $builtExe"
}

# Imports: only Windows system DLLs and the UCRT API sets.
$objdump = Join-Path $MsysRoot "ucrt64\bin\objdump.exe"
$global:LASTEXITCODE = 0
$headers = & $objdump -p $builtExe
if ($LASTEXITCODE -ne 0) {
    throw "objdump -p failed on $builtExe"
}
$imports = @($headers | ForEach-Object {
        if ("$_" -match '^\s*DLL Name:\s*(\S+)') { $Matches[1] }
    } | Sort-Object -Unique)
if ($imports.Count -eq 0) {
    throw "objdump listed no imports for $builtExe; refusing to treat that as verified"
}
$unexpected = @($imports | Where-Object {
        ($_ -notlike "api-ms-win-crt-*") -and ($AllowedImports -notcontains $_)
    })
if ($unexpected.Count -gt 0) {
    throw "aria2c.exe imports non-system DLLs: $($unexpected -join ', ')"
}
Write-Step ("imports: " + ($imports -join ", "))

$global:LASTEXITCODE = 0
$version = & $builtExe --version
if ($LASTEXITCODE -ne 0) {
    throw "aria2c --version exited $LASTEXITCODE"
}
$version | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" }

$exeSha = Get-Sha256 -Path $builtExe
Write-Step ("aria2c.exe {0} ({1} bytes)" -f $exeSha, (Get-Item -LiteralPath $builtExe).Length)
if ($exeSha -eq $ExpectedExeSha256) {
    Write-Step "Reproduced the pinned executable bit for bit."
} else {
    Write-Host "[aria2 build] Built executable differs from the pinned $ExpectedExeSha256." -ForegroundColor Yellow
}

if ($Install) {
    $target = Join-Path $repoRoot "tools\uup\aria2c.exe"
    Copy-Item -LiteralPath $builtExe -Destination $target -Force
    if ((Get-Sha256 -Path $target) -ne $exeSha) {
        throw "Copy to $target did not produce the built bytes"
    }
    Write-Step "Installed $target"
    Write-Step "Update the aria2c.exe pin in scripts/bundle_uup_tools.ps1 and this script if it changed."
}
