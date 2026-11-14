# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Fails when Microsoft Defender detects anything in the folders given: the repository's tests and
# scripts trees and the build's program folder (what a package is staged from). Defender on
# testpc2 quarantined tests/support/partition_image_volume_lock_fixture.ps1 as
# Trojan:PowerShell/Fleisnam.E (2026-11-11), which broke the build there, and would break any
# builder or CI runner with Defender on; a file S.A.K. ships or generates that Defender flags would
# break a technician's run the same way. Each folder is scanned with -DisableRemediation, so a
# detection is reported and nothing is quarantined. Defender must be there and running: a machine
# without it, or with its antivirus service off, proves nothing and fails, unless
# -AllowMissingDefender is given, when it says so plainly and passes (the nightly job then writes a
# warning into its summary).
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
$why = ''
if (-not (Test-Path -LiteralPath $scanner -PathType Leaf)) {
    $why = "its scanner is not at $scanner"
} else {
    try {
        $status = Get-MpComputerStatus -ErrorAction Stop
        if (-not ($status.AMServiceEnabled -and $status.AntivirusEnabled)) {
            $why = "its service is $(if ($status.AMServiceEnabled) { 'on' } else { 'off' }) and its antivirus $(if ($status.AntivirusEnabled) { 'on' } else { 'off' })"
        } else {
            Write-Host "Microsoft Defender: engine $($status.AMEngineVersion), signatures $($status.AntivirusSignatureVersion), real-time protection $(if ($status.RealTimeProtectionEnabled) { 'on' } else { 'off' })"
        }
    } catch {
        $why = "Get-MpComputerStatus failed: $($_.Exception.Message)"
    }
}
if ($why) {
    if ($AllowMissingDefender) {
        Write-Host "Microsoft Defender is not running here ($why): NOTHING WAS SCANNED"
        exit 0
    }
    throw "Microsoft Defender is not running here ($why), so nothing was scanned"
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
