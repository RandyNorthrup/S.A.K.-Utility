# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$verifier = Join-Path $ProjectRoot "scripts\verify_windows_runtime_dependencies.ps1"
$releaseRoot = Join-Path $ProjectRoot "build\Release"

foreach ($required in @($verifier, (Join-Path $releaseRoot "sak_utility.exe"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Runtime-dependency regression test input missing: $required"
    }
}

$hostPath = (Get-Process -Id $PID).Path
& $hostPath -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $verifier `
    -RootDir $releaseRoot -PrimaryExe "sak_utility.exe"
$verifyExit = $LASTEXITCODE
if ($verifyExit -ne 0) {
    throw "Runtime-dependency verifier rejected the built app (exit $verifyExit)."
}

Write-Host "Runtime-dependency regression test passed for built sak_utility.exe."
exit 0
