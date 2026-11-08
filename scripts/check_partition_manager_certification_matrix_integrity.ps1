<#
.SYNOPSIS
    Verifies the Partition Manager certification matrix against what can actually run it.

.DESCRIPTION
    Static release gate. It changes no disk. The matrix is generated, so it must be what its
    generator writes; each row must have a body that can run it, each step an operation the step
    client can build, and each check a test the verifier knows. A row nothing can run, or a check
    nothing can evaluate, would otherwise sit in the matrix reading as a promise.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Assert-Condition {
    param([Parameter(Mandatory = $true)] [bool]$Condition, [Parameter(Mandatory = $true)] [string]$Message)
    if (-not $Condition) { throw $Message }
}

$knownTests = @('equals', 'equals_path', 'differs_from_path', 'spins_by_rotation_path', 'at_least', 'is_true', 'is_false', 'contains')
$knownClasses = @('virtual_disk', 'virtual_machine', 'physical')
$knownExpectations = @('success', 'refused', 'failed')

$matrix = Get-Content -LiteralPath (Join-Path $ProjectRoot 'certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json') -Raw | ConvertFrom-Json
Assert-Condition ($matrix.schema_version -eq 2) "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

& python (Join-Path $ProjectRoot 'scripts\generate_partition_manager_matrix.py') --root $ProjectRoot --check | Out-Null
Assert-Condition ($LASTEXITCODE -eq 0) 'The certification matrix is not what scripts/generate_partition_manager_matrix.py writes. It is never edited by hand.'

$bodies = Get-Content -LiteralPath (Join-Path $ProjectRoot 'scripts\partition_manager_certification_row_bodies.ps1') -Raw
$stepClient = Get-Content -LiteralPath (Join-Path $ProjectRoot 'tests\certification\partition_manager_scenario_step.cpp') -Raw
$vhdRows = @($matrix.vhd_scenarios)
$externalRows = @($matrix.external_gates)
Assert-Condition ($vhdRows.Count -gt 0) 'Certification matrix has no VHD scenarios'
Assert-Condition ($externalRows.Count -gt 0) 'Certification matrix has no external gates'

$seen = @{}
foreach ($row in @($vhdRows + $externalRows)) {
    $id = "$($row.id)"
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($id)) 'A row has no id'
    Assert-Condition (-not $seen.ContainsKey($id)) "The matrix names $id twice"
    $seen[$id] = $true
    Assert-Condition (-not [string]::IsNullOrWhiteSpace("$($row.name)")) "$id has no name"
    Assert-Condition (-not [string]::IsNullOrWhiteSpace("$($row.not_proved)")) "$id does not say what it leaves unproved"
    Assert-Condition ($bodies.Contains("CertRowBodies['$id']")) "$id has no row body in partition_manager_certification_row_bodies.ps1"
    Assert-Condition (@('unelevated', 'elevated') -ccontains "$($row.client)") "$id names an unknown client: $($row.client)"
    $accepted = @($row.ran_on.accepted)
    Assert-Condition ($accepted.Count -gt 0) "$id accepts no media"
    foreach ($class in $accepted) { Assert-Condition ($knownClasses -ccontains "$class") "$id accepts an unknown media class: $class" }
    Assert-Condition ($row.ran_on.physical_claim -is [bool]) "$id does not say whether its claim needs a real device"
    if ($row.ran_on.physical_claim) { Assert-Condition ($accepted -ccontains 'physical') "$id claims a device and does not accept one" }
    $steps = @($row.steps)
    Assert-Condition ($steps.Count -gt 0) "$id runs nothing"
    $stepIds = @{}
    foreach ($step in $steps) {
        Assert-Condition (-not $stepIds.ContainsKey("$($step.id)")) "$id names step $($step.id) twice"
        $stepIds["$($step.id)"] = $true
        Assert-Condition ($knownExpectations -ccontains "$($step.expect)") "$id step $($step.id) expects $($step.expect)"
        Assert-Condition ($stepClient.Contains("QStringLiteral(`"$($step.operation)`")")) "$id step $($step.id) asks for $($step.operation), which the step client cannot build"
    }
    $checks = @($row.checks)
    Assert-Condition ($checks.Count -gt 0) "$id checks nothing, so nothing could fail it"
    $contracts = @{}
    foreach ($check in $checks) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace("$($check.contract)")) "$id has a check with no name"
        Assert-Condition (-not $contracts.ContainsKey("$($check.contract)")) "$id names the check $($check.contract) twice"
        $contracts["$($check.contract)"] = $true
        $tests = @($check.PSObject.Properties | Where-Object { $knownTests -ccontains $_.Name })
        Assert-Condition ($tests.Count -eq 1) "$id check $($check.contract) must hold exactly one test"
    }
}

Write-Host "Partition Manager certification matrix integrity passed: $($vhdRows.Count) VHD rows, $($externalRows.Count) other rows."
