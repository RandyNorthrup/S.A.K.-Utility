# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Verifies signatures and signed-release readiness for current VERSION package.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$PackageRoot = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $versionPath = Join-Path $ProjectRoot "VERSION"
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw "Signed release VERSION file not found: $versionPath"
    }
    $version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
        throw "Signed release VERSION is malformed: $version"
    }
    $PackageRoot = Join-Path $ProjectRoot "build\Release\SAK-Utility-v$version"
}
if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "Signed release package not found: $PackageRoot"
}

& (Join-Path $PSScriptRoot "verify_authenticode_signatures.ps1") -RootDir $PackageRoot
$signatureExit = $LASTEXITCODE
if ($signatureExit -ne 0) {
    throw "Signed release signature verification exited $signatureExit."
}
& (Join-Path $PSScriptRoot "check_release_readiness.ps1") `
    -PackageRoot $PackageRoot -RequireSignedPackage
$readinessExit = $LASTEXITCODE
if ($readinessExit -ne 0) {
    throw "Signed release readiness exited $readinessExit."
}

Write-Host "Signed release verification passed: $PackageRoot"
exit 0
