# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Verifies pre-commit gate coverage has a CI route.

.DESCRIPTION
    Prose gate inventories drift. This check reads the live pre-commit config and every CI
    workflow, then verifies both directions: custom/generic pre-commit gates have CI routes,
    and CI grading scripts have pre-commit or manual routes. It separately verifies full-history
    secret scanning. Passing this script is route proof, not clean-runner execution proof; only
    an executed CI run can provide that.
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
$normalizedConfig = Normalize-Slashes $configText

$scriptPattern = 'scripts[/\\][A-Za-z0-9_.-]+\.(?:ps1|py)'
$customScripts = @([regex]::Matches($configText, $scriptPattern) |
    ForEach-Object { Normalize-Slashes $_.Value } | Sort-Object -Unique)
if ($customScripts.Count -eq 0) {
    throw "Gate parity found zero custom gate scripts in $PreCommitConfig."
}

$indirectRoutes = @{
    "scripts/run_cppcheck.ps1" = "scripts/cppcheck_whole_project_gate.ps1"
}
$localAggregateRoutes = @{
    "scripts/verify_signed_release.ps1" = @(
        "scripts/verify_authenticode_signatures.ps1",
        "scripts/check_release_readiness.ps1"
    )
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
    if ($localAggregateRoutes.ContainsKey($script)) {
        $localPath = Join-Path $RepositoryRoot $script.Replace("/", "\")
        $localText = if (Test-Path -LiteralPath $localPath -PathType Leaf) {
            Normalize-Slashes (Get-Content -LiteralPath $localPath -Raw)
        }
        else {
            ""
        }
        $targets = @($localAggregateRoutes[$script])
        $complete = @($targets | Where-Object {
                $workflowText.Contains($_) -and
                $localText.Contains([System.IO.Path]::GetFileName($_))
            }).Count -eq $targets.Count
        if ($complete) {
            continue
        }
        $problems.Add("$script does not aggregate all declared CI gate routes.")
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

# Reverse direction: every CI script that grades the repository/build/package needs a local
# pre-commit/manual route. These are workflow actions, not gates; they produce inputs that later
# gates inspect and are deliberately excluded with an explicit name rather than a wildcard.
$ciNonGateScripts = @(
    "scripts/bundle_iperf3.ps1",
    "scripts/bundle_smartmontools.ps1",
    "scripts/bundle_uup_tools.ps1",
    "scripts/bundle_vcpkg_runtime.ps1",
    "scripts/create_release_archive.ps1",
    "scripts/create_signing_catalog.ps1",
    "scripts/stage_portable_release.ps1"
)
$ciScripts = @([regex]::Matches($workflowText, $scriptPattern) |
    ForEach-Object { Normalize-Slashes $_.Value } | Sort-Object -Unique)
$reverseIndirectRoutes = @{
    "scripts/verify_authenticode_signatures.ps1" = "scripts/verify_signed_release.ps1"
}
foreach ($script in $ciScripts) {
    if ($ciNonGateScripts -contains $script) {
        continue
    }
    # Generic built-ins already have direct pre-commit hooks; this is only their whole-tree CI
    # wrapper and is verified above.
    if ($script -eq $hygieneScript) {
        continue
    }
    if ($normalizedConfig.Contains($script)) {
        continue
    }
    if ($reverseIndirectRoutes.ContainsKey($script)) {
        $wrapper = $reverseIndirectRoutes[$script]
        $wrapperPath = Join-Path $RepositoryRoot $wrapper.Replace("/", "\")
        $wrapperText = if (Test-Path -LiteralPath $wrapperPath -PathType Leaf) {
            Normalize-Slashes (Get-Content -LiteralPath $wrapperPath -Raw)
        }
        else {
            ""
        }
        if ($normalizedConfig.Contains($wrapper) -and
            $wrapperText.Contains([System.IO.Path]::GetFileName($script))) {
            continue
        }
    }
    $problems.Add("$script is a CI gate with no pre-commit/manual route.")
}

foreach ($action in $ciNonGateScripts) {
    if (-not $workflowText.Contains($action)) {
        $problems.Add("CI non-gate action allowlist is stale; workflow no longer uses $action.")
    }
}

if ($problems.Count -gt 0) {
    Write-Host "GATE PARITY FAILED: $($problems.Count) problem(s)." -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}

$successMessage = ("Gate parity passed: {0} custom script(s), {1} generic hook(s), " +
    "{2} CI script route(s), and full-history gitleaks.") -f `
    $customScripts.Count, $builtInHooks.Count, ($ciScripts.Count - $ciNonGateScripts.Count)
Write-Host $successMessage
exit 0
