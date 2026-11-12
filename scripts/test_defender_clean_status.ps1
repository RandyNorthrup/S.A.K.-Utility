# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# test_defender_clean.ps1 must never pass on a machine where Defender is not running: its scan
# would report nothing and look clean. This runs it in a child PowerShell where Get-MpComputerStatus
# answers "antivirus off" (a function of that name shadows the cmdlet), so no scan starts:
#   - without -AllowMissingDefender it fails, saying Defender is not running and nothing was scanned;
#   - with it, it passes and says NOTHING WAS SCANNED, which the nightly job turns into a warning.
# No Defender setting is read or changed; the folder it is given is an empty temporary one.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot 'test_defender_clean.ps1'
$folder = Join-Path ([IO.Path]::GetTempPath()) "sak-defender-status-$PID"
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$failures = [System.Collections.Generic.List[string]]::new()

function Invoke-WithDefenderOff {
    param([string[]]$Switches)
    $stub = 'function Get-MpComputerStatus { [pscustomobject]@{ AMServiceEnabled = $true; ' +
        'AntivirusEnabled = $false; RealTimeProtectionEnabled = $false } }'
    $call = "& '$($script.Replace("'", "''"))' '$($folder.Replace("'", "''"))' $($Switches -join ' ')"
    $output = & (Get-Process -Id $PID).Path -NoProfile -NonInteractive -Command "$stub; $call" 2>&1
    return [pscustomobject]@{ code = $LASTEXITCODE; text = (@($output | ForEach-Object { "$_" }) -join "`n") }
}

try {
    $strict = Invoke-WithDefenderOff -Switches @()
    if ($strict.code -eq 0) { $failures.Add("passed with Defender's antivirus off: $($strict.text)") }
    if ($strict.text -notmatch 'not running here \(its service is on and its antivirus off\), so nothing was scanned') {
        $failures.Add("did not say why nothing was scanned: $($strict.text)")
    }
    if ($strict.text -match 'clean:|found nothing') { $failures.Add("reported a scan that never ran: $($strict.text)") }

    $allowed = Invoke-WithDefenderOff -Switches @('-AllowMissingDefender')
    if ($allowed.code -ne 0) { $failures.Add("-AllowMissingDefender did not pass: $($allowed.text)") }
    if ($allowed.text -notmatch 'NOTHING WAS SCANNED') { $failures.Add("-AllowMissingDefender did not say so: $($allowed.text)") }
} finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    throw "$($failures.Count) Defender status check(s) failed"
}
Write-Host 'PASS: with Defender off, the scan fails, or passes saying nothing was scanned only when allowed'
