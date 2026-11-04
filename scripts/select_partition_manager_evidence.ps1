<#
.SYNOPSIS
    Selects one run of Partition Manager evidence as current, by writing its index.

.DESCRIPTION
    Copies the rows of a run into artifacts/partition-manager-certification/evidence/current and
    writes index.json, which hashes each row's report and names the commit and the digest. Status
    and verification read only that index, so an older run in the repository, which stays there as
    history, can never be picked up as current.

    It refuses a row that does not hold, and it refuses evidence that holds a name the owner wants
    kept private (-DenyNames, from the credentials file, never from the repository). A row with no
    evidence stops it, unless that row accepts only a physical device, which a run with no device
    could not have run. The status lists that row as not run.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$RunDir,
    [string]$OutputRoot = 'artifacts\partition-manager-certification\evidence\current',
    [string[]]$DenyNames = @(),
    [string]$CodeDigest = '',
    [string]$MatrixPath = '',
    [switch]$AllowIncomplete
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($MatrixPath)) { $MatrixPath = Join-Path $projectRoot 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json' }
$matrix = Get-Content -LiteralPath $MatrixPath -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($CodeDigest)) {
    $CodeDigest = ((& python (Join-Path $PSScriptRoot 'partition_manager_code_digest.py') --root $projectRoot) | ConvertFrom-Json).code_digest
}
$output = if ([IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot } else { Join-Path $projectRoot $OutputRoot }

$rows = @()
$commits = @()
$completed = @()
foreach ($spec in @($matrix.vhd_scenarios) + @($matrix.external_gates)) {
    $rowDir = Join-Path $RunDir "$($spec.id)"
    if (-not (Test-Path -LiteralPath (Join-Path $rowDir 'report.json'))) {
        if ($AllowIncomplete -or (@($spec.ran_on.accepted) -join ',') -ceq 'physical') { continue }
        throw "The run has no evidence for $($spec.id)"
    }
    $reasons = @(Test-PmRowEvidence -Spec $spec -RowDir $rowDir -CurrentCodeDigest $CodeDigest)
    if ($reasons.Count -gt 0) { throw "$($spec.id) does not hold:`n$($reasons -join "`n")" }
    $report = Get-Content -LiteralPath (Join-Path $rowDir 'report.json') -Raw | ConvertFrom-Json
    $commits += "$($report.code.commit)"
    $completed += "$($report.completed_utc)"
    foreach ($name in $DenyNames) {
        $leaks = @(Get-ChildItem -LiteralPath $rowDir -Recurse -File | Select-String -SimpleMatch -Pattern $name -List)
        if ($leaks.Count -gt 0) { throw "$($spec.id) holds a private name in $($leaks[0].Path)" }
    }
    $rows += [ordered]@{ id = "$($spec.id)"; report_sha256 = (Get-FileHash -LiteralPath (Join-Path $rowDir 'report.json') -Algorithm SHA256).Hash.ToLowerInvariant() }
}
if ($rows.Count -eq 0) { throw 'The run holds no row' }
if (@($commits | Sort-Object -Unique).Count -ne 1) { throw "The rows come from more than one commit: $(@($commits | Sort-Object -Unique) -join ', ')" }

if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($row in $rows) { Copy-Item -LiteralPath (Join-Path $RunDir $row.id) -Destination $output -Recurse }
$index = [ordered]@{
    tool = 'partition-manager-evidence-index'
    schema_version = 1
    code_digest = $CodeDigest
    code_commit = $commits[0]
    completed_utc = ($completed | Sort-Object | Select-Object -Last 1)
    rows = $rows
}
[IO.File]::WriteAllText((Join-Path $output 'index.json'), ($index | ConvertTo-Json -Depth 6))
Write-Host "Selected $($rows.Count) rows from $RunDir as current evidence."
