# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$capture = Join-Path $ProjectRoot "scripts\process_capture.ps1"
$rehearsal = Join-Path $ProjectRoot "scripts\rehearse_ci_locally.ps1"
foreach ($required in @($capture, $rehearsal)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "CI rehearsal path-test input missing: $required"
    }
}

. $capture
$hostPath = (Get-Process -Id $PID).Path
$positive = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
    "-NoProfile", "-NonInteractive", "-File", $rehearsal,
    "-ResolveFreshCloneInputsOnly"
) -TimeoutSeconds 30
if ($positive.timed_out -or $positive.exit_code -ne 0 -or
    $positive.stdout -notmatch 'Fresh clone inputs resolved') {
    throw ("Fresh-clone input resolution failed. exit=$($positive.exit_code), " +
        "stdout=$($positive.stdout), stderr=$($positive.stderr)")
}

$badVcpkg = Join-Path ([System.IO.Path]::GetTempPath()) "sak_bad_vcpkg_$PID"
New-Item -ItemType Directory -Path $badVcpkg -Force | Out-Null
try {
    $negative = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        "-NoProfile", "-NonInteractive", "-File", $rehearsal,
        "-ResolveFreshCloneInputsOnly", "-VcpkgRootPath", $badVcpkg
    ) -TimeoutSeconds 30
    if ($negative.timed_out -or $negative.exit_code -eq 0 -or
        $negative.stderr -notmatch 'vcpkg toolchain file not found') {
        throw ("Invalid explicit vcpkg root did not fail closed. exit=$($negative.exit_code), " +
            "stdout=$($negative.stdout), stderr=$($negative.stderr)")
    }
}
finally {
    Remove-Item -LiteralPath $badVcpkg -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "CI rehearsal path test passed: auto resolution green, invalid explicit root red."
exit 0
