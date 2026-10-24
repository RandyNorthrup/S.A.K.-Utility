# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Runs generic pre-commit hygiene hooks over the whole tracked tree.

.DESCRIPTION
    Pre-commit runs these hooks on changed files. CI must run the same checks over the whole
    tree so a direct push or skipped local hook cannot bypass whitespace, syntax, conflict,
    filename-case, or large-file checks.

    Every hook id is fixed below and must match the T1 standard hooks in
    .pre-commit-config.yaml. A missing Python/pre-commit installation or any non-zero hook
    result fails closed.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

function Resolve-Python {
    if (-not [string]::IsNullOrWhiteSpace($Python)) {
        if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) {
            throw "Repository hygiene Python executable does not exist: $Python"
        }
        return (Resolve-Path -LiteralPath $Python).Path
    }

    $command = Get-Command python -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    $venvPython = Join-Path $ProjectRoot ".venv\Scripts\python.exe"
    if (Test-Path -LiteralPath $venvPython -PathType Leaf) {
        return (Resolve-Path -LiteralPath $venvPython).Path
    }
    throw "Repository hygiene requires Python with pre-commit installed."
}

$pythonPath = Resolve-Python
& $pythonPath -m pre_commit --version | Out-Host
$versionExit = $LASTEXITCODE
if ($versionExit -ne 0) {
    throw "pre-commit version query exited $versionExit; repository hygiene cannot run."
}

$hookIds = @(
    "trailing-whitespace",
    "end-of-file-fixer",
    "check-yaml",
    "check-json",
    "check-toml",
    "check-added-large-files",
    "check-merge-conflict",
    "check-case-conflict"
)

Push-Location $ProjectRoot
try {
    foreach ($hookId in $hookIds) {
        Write-Host "Repository hygiene: $hookId"
        & $pythonPath -m pre_commit run $hookId --all-files
        $hookExit = $LASTEXITCODE
        if ($hookExit -ne 0) {
            throw "Repository hygiene hook $hookId exited $hookExit."
        }
    }
}
finally {
    Pop-Location
}

Write-Host "Repository hygiene passed: $($hookIds.Count) whole-tree hook(s)."
exit 0
