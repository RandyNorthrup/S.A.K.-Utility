# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$gate = Join-Path $ProjectRoot "scripts\check_startup_budget.ps1"
$capture = Join-Path $ProjectRoot "scripts\process_capture.ps1"
$exe = Join-Path $ProjectRoot "build\Release\sak_utility.exe"

foreach ($required in @($gate, $capture, $exe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Startup-budget regression test input missing: $required"
    }
}

$source = Get-Content -LiteralPath $gate -Raw
if ($source -match '\bStart-Process\b') {
    throw "Startup budget regressed to Start-Process redirected capture."
}
if ($source -notmatch '\bInvoke-CapturedProcess\b') {
    throw "Startup budget no longer uses the shared captured-process seam."
}

. $capture
$hostPath = (Get-Process -Id $PID).Path
$red = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-File",
    $gate,
    "-BudgetMs",
    "1",
    "-HardTimeoutSec",
    "60",
    "-ExePath",
    $exe
) -TimeoutSeconds 90
if ($red.timed_out -or $red.exit_code -eq 0 -or $red.stderr -notmatch 'Startup budget EXCEEDED') {
    throw ("Startup budget 1 ms drill did not fail for the budget guard. " +
        "exit=$($red.exit_code), timed_out=$($red.timed_out), stderr=$($red.stderr)")
}

$green = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-File",
    $gate,
    "-BudgetMs",
    "30000",
    "-HardTimeoutSec",
    "60",
    "-ExePath",
    $exe
) -TimeoutSeconds 90
if ($green.timed_out -or $green.exit_code -ne 0 -or
    $green.stdout -notmatch 'Startup budget passed') {
    throw ("Startup budget real-app control failed. exit=$($green.exit_code), " +
        "timed_out=$($green.timed_out), stdout=$($green.stdout), stderr=$($green.stderr)")
}

Write-Host "Startup budget regression test passed: capture seam pinned, 1 ms red, real app green."
exit 0
