# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Verifies pre-commit gate coverage has a CI route.

.DESCRIPTION
    Prose gate inventories drift. This check reads the live pre-commit config and every CI
    workflow, then fails when a custom pre-commit gate script has no direct or documented
    indirect CI route. It also verifies generic pre-commit hygiene hooks and full-history
    secret scanning have CI twins.

    This is one direction of parity: CI-only build/runtime gates require local manual routes,
    audited separately under R5-G21-2. Passing this script must not be read as clean-runner
    proof; only an executed CI run can provide that.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$RepositoryRoot = "",
    [string]$PreCommitConfig = "",
    [string]$WorkflowRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($PreCommitConfig)) {
    $PreCommitConfig = Join-Path $RepositoryRoot ".pre-commit-config.yaml"
}
if ([string]::IsNullOrWhiteSpace($WorkflowRoot)) {
    $WorkflowRoot = Join-Path $RepositoryRoot ".github\workflows"
}

foreach ($required in @($PreCommitConfig, $WorkflowRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Gate parity input does not exist: $required"
    }
}

function Normalize-Slashes([string]$Text) {
    return $Text.Replace("\", "/")
}

$configText = Get-Content -LiteralPath $PreCommitConfig -Raw
$workflowFiles = @(Get-ChildItem -LiteralPath $WorkflowRoot -File |
    Where-Object { $_.Extension -in @(".yml", ".yaml") })
if ($workflowFiles.Count -eq 0) {
    throw "Gate parity found zero CI workflow files under $WorkflowRoot."
}
$workflowText = Normalize-Slashes (($workflowFiles | ForEach-Object {
            Get-Content -LiteralPath $_.FullName -Raw
        }) -join "`n")

$scriptPattern = 'scripts[/\\][A-Za-z0-9_.-]+\.(?:ps1|py)'
$customScripts = @([regex]::Matches($configText, $scriptPattern) |
    ForEach-Object { Normalize-Slashes $_.Value } | Sort-Object -Unique)
if ($customScripts.Count -eq 0) {
    throw "Gate parity found zero custom gate scripts in $PreCommitConfig."
}

$indirectRoutes = @{
    "scripts/run_cppcheck.ps1" = "scripts/cppcheck_whole_project_gate.ps1"
}
$problems = [System.Collections.Generic.List[string]]::new()

foreach ($script in $customScripts) {
    if ($workflowText.Contains($script)) {
        continue
    }
    if ($indirectRoutes.ContainsKey($script)) {
        $wrapper = $indirectRoutes[$script]
        $wrapperPath = Join-Path $RepositoryRoot $wrapper.Replace("/", "\")
        $wrapperText = if (Test-Path -LiteralPath $wrapperPath -PathType Leaf) {
            Normalize-Slashes (Get-Content -LiteralPath $wrapperPath -Raw)
        }
        else {
            ""
        }
        $scriptLeaf = [System.IO.Path]::GetFileName($script)
        if ($workflowText.Contains($wrapper) -and $wrapperText.Contains($scriptLeaf)) {
            continue
        }
        $problems.Add("$script has no valid indirect CI route through $wrapper.")
        continue
    }
    $problems.Add("$script is a pre-commit gate with no CI workflow reference.")
}

$hygieneScript = "scripts/check_repository_hygiene.ps1"
$hygienePath = Join-Path $RepositoryRoot $hygieneScript.Replace("/", "\")
$hygieneText = if (Test-Path -LiteralPath $hygienePath -PathType Leaf) {
    Get-Content -LiteralPath $hygienePath -Raw
}
else {
    ""
}
$builtInHooks = @(
    "trailing-whitespace",
    "end-of-file-fixer",
    "check-yaml",
    "check-json",
    "check-toml",
    "check-added-large-files",
    "check-merge-conflict",
    "check-case-conflict"
)
if (-not $workflowText.Contains($hygieneScript)) {
    $problems.Add("CI does not invoke $hygieneScript for generic pre-commit hooks.")
}
foreach ($hookId in $builtInHooks) {
    if ($configText -notmatch "(?m)^\s+- id:\s+$([regex]::Escape($hookId))\s*$") {
        $problems.Add("Pre-commit config no longer declares built-in hook $hookId.")
    }
    $hygienePattern = '(?m)^\s+"{0}",?\s*$' -f [regex]::Escape($hookId)
    if ($hygieneText -notmatch $hygienePattern) {
        $problems.Add("$hygieneScript does not run built-in hook $hookId.")
    }
}

if ($configText -notmatch '(?m)^\s+- id:\s+gitleaks-staged\s*$') {
    $problems.Add("Pre-commit config no longer declares gitleaks-staged.")
}
if ($workflowText -notmatch '(?m)gitleaks\s+detect\b') {
    $problems.Add("CI has no gitleaks full-history detect command.")
}
if ($workflowText -notmatch '(?m)fetch-depth:\s*0\s*$') {
    $problems.Add("CI secret scan does not fetch full history (fetch-depth: 0 missing).")
}

if ($problems.Count -gt 0) {
    Write-Host "GATE PARITY FAILED: $($problems.Count) problem(s)." -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}

$successMessage = ("Gate parity passed: {0} custom script(s), {1} generic hook(s), " +
    "and full-history gitleaks have CI routes.") -f $customScripts.Count, $builtInHooks.Count
Write-Host $successMessage
exit 0
