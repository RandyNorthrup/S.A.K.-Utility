# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Re-pack (sign) the S.A.K. Utility browser-control extension into the committed CRX
# that the app force-installs on customer machines. Run this whenever anything under
# browser/extension changes so browser/dist/sak_browser_control.crx stays in sync.
#
# The signing key is NOT in the repo (a private key must never be committed). It lives
# outside the tree at %LOCALAPPDATA%\SAK\keys\sak_browser_control.signing-key.pem and
# MUST be backed up: losing it changes the extension id, which forces every installed
# machine to re-install under the new id. The public half of this key is pinned in
# browser/extension/manifest.json ("key"), so the unpacked-load id and the CRX id match.
#
# Usage:
#   ./pack-extension.ps1                       # sign with the default key -> browser/dist
#   ./pack-extension.ps1 -Key <path-to.pem>    # sign with an explicit key
#
# Expected extension id: ofodhfbipljnhenjjjpbdaglkjdphoec

[CmdletBinding()]
param(
    [string]$Chrome,
    [string]$Key,
    [string]$Out
)

$ErrorActionPreference = "Stop"

$ExpectedId = "ofodhfbipljnhenjjjpbdaglkjdphoec"
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir     = Join-Path $ScriptDir "extension"

if (-not $Key) {
    $Key = Join-Path $env:LOCALAPPDATA "SAK\keys\sak_browser_control.signing-key.pem"
}
if (-not (Test-Path $Key)) {
    throw "Signing key not found: $Key (restore your backed-up key, or pass -Key)."
}
if (-not $Out) {
    $Out = Join-Path $ScriptDir "dist\sak_browser_control.crx"
}
if (-not $Chrome) {
    $candidates = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
    )
    $Chrome = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Chrome -or -not (Test-Path $Chrome)) {
    throw "chrome.exe not found; pass -Chrome <path>."
}

# Pack a COPY so Chrome's <dir>.crx / <dir>.pem outputs never land in the source tree.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_ext_pack_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$stage = Join-Path $work "extension"
Copy-Item -Recurse $SrcDir $stage

try {
    & $Chrome "--pack-extension=$stage" "--pack-extension-key=$Key" "--no-message-box" | Out-Null
    Start-Sleep -Milliseconds 1500
    $producedCrx = Join-Path $work "extension.crx"
    if (-not (Test-Path $producedCrx)) { throw "Chrome did not produce a CRX at $producedCrx." }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
    Copy-Item $producedCrx $Out -Force
    Write-Host "Wrote $Out ($((Get-Item $Out).Length) bytes)"
    Write-Host "Expected extension id: $ExpectedId"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
