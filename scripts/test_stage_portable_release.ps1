# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$capture = Join-Path $ProjectRoot "scripts\process_capture.ps1"
$stageScript = Join-Path $ProjectRoot "scripts\stage_portable_release.ps1"
$buildRoot = Join-Path $ProjectRoot "build\Release"
$packageName = "SAK-Stage-Test-$PID"
$packageRoot = Join-Path $buildRoot $packageName

foreach ($required in @($capture, $stageScript, (Join-Path $buildRoot "sak_utility.exe"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Portable-stage regression test input missing: $required"
    }
}
if (Test-Path -LiteralPath $packageRoot) {
    throw "Portable-stage regression target unexpectedly exists: $packageRoot"
}

. $capture
try {
    $hostPath = (Get-Process -Id $PID).Path
    $result = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $stageScript,
        "-BuildDir",
        $buildRoot,
        "-PackageName",
        $packageName
    ) -TimeoutSeconds 180
    if ($result.timed_out -or $result.exit_code -ne 0) {
        throw ("Portable stage failed. exit=$($result.exit_code), timed_out=$($result.timed_out), " +
            "stdout=$($result.stdout), stderr=$($result.stderr)")
    }

    foreach ($relative in @(
            "sak_utility.exe",
            "zlib1.dll",
            "data\ai\providers\providers.json",
            "tools\filesystem\manifest.json")) {
        $path = Join-Path $packageRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Portable stage omitted required regression artifact: $relative"
        }
    }
}
finally {
    if (Test-Path -LiteralPath $packageRoot -PathType Container) {
        Remove-Item -LiteralPath $packageRoot -Recurse -Force
    }
}

Write-Host "Portable-stage regression test passed: named child parameters and artifacts verified."
exit 0
