# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Fails when Microsoft Defender detects anything in the folders given: the repository's tests and
# scripts trees and the build's program folder (what a package is staged from). Defender on
# testpc2 quarantined tests/support/partition_image_volume_lock_fixture.ps1 as
# Trojan:PowerShell/Fleisnam.E (2026-11-11), which broke the build there, and would break any
# builder or CI runner with Defender on; a file S.A.K. ships or generates that Defender flags would
# break a technician's run the same way. Each folder is scanned with -DisableRemediation, so a
# detection is reported and nothing is quarantined. The scanner must be there: a machine without
# Defender proves nothing, and says so, unless -AllowMissingDefender is given.
#
# Usage (paths positional; see AGENTS.md on -File array binding):
#   pwsh -File scripts/test_defender_clean.ps1 <folder> [<folder> ...] [-AllowMissingDefender]
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$AllowMissingDefender,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Folders
)
$ErrorActionPreference = 'Stop'
if (-not $Folders -or $Folders.Count -eq 0) { throw 'Name at least one folder to scan' }
$scanner = Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
if (-not (Test-Path -LiteralPath $scanner -PathType Leaf)) {
    if ($AllowMissingDefender) {
        Write-Host "Microsoft Defender is not installed ($scanner): nothing was scanned"
        exit 0
    }
    throw "Microsoft Defender's scanner is not at $scanner, so nothing was scanned"
}
$failed = @()
foreach ($folder in $Folders) {
    $full = (Resolve-Path -LiteralPath $folder).Path
    $output = & $scanner -Scan -ScanType 3 -File $full -DisableRemediation 2>&1 | Out-String
    $code = $LASTEXITCODE
    $summary = ($output.Trim() -replace '\s+', ' ')
    # MpCmdRun: 0 when nothing was found, 2 when something was; anything else did not scan.
    if ($code -eq 0) {
        Write-Host "clean: $full"
    } elseif ($code -eq 2) {
        Write-Host "DETECTED in ${full}: $summary"
        $failed += $full
    } else {
        Write-Host "the scan of $full did not run (exit $code): $summary"
        $failed += $full
    }
}
if ($failed.Count -gt 0) {
    throw ("Defender found something in, or could not scan: " + ($failed -join ', '))
}
Write-Host "Defender found nothing in $($Folders.Count) folder(s)"
