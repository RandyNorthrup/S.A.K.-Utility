<#
.SYNOPSIS
    Verifies a Partition Manager certification gap report.

.DESCRIPTION
    Ensures the generated gap report exactly matches certification-status.json
    and the certification matrix so missing destructive certification work cannot
    drift from release readiness output.
#>

[CmdletBinding()]
param(
    [string]$GapReportPath = "artifacts\partition-manager-certification\readiness\certification-gap-report.json",
    [string]$MarkdownPath = "artifacts\partition-manager-certification\readiness\certification-gap-report.md",
    [string]$StatusPath = "artifacts\partition-manager-certification\readiness\certification-status.json"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path $ProjectRoot $Path)
}

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-ScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Matrix,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($Matrix.vhd_scenarios + $Matrix.external_gates |
        Where-Object { $_.id -eq $Id })
    Assert-Condition -Condition ($matches.Count -eq 1) -Message "Certification matrix missing or duplicate scenario: $Id"
    return $matches[0]
}

function Test-ArrayContainsAll {
    param(
        [object[]]$Actual,
        [object[]]$Expected
    )

    $actualStrings = @($Actual | ForEach-Object { $_.ToString() })
    foreach ($item in @($Expected)) {
        if ($actualStrings -notcontains $item.ToString()) {
            return $false
        }
    }
    return $true
}

function Test-RequiredEvidenceValuesMatch {
    param(
        [object]$Gap,
        [object]$Spec
    )

    if ($null -eq $Spec.PSObject.Properties["required_evidence_values"]) {
        return ($null -eq $Gap.required_evidence_values)
    }
    if ($null -eq $Gap.PSObject.Properties["required_evidence_values"] -or $null -eq $Gap.required_evidence_values) {
        return $false
    }

    foreach ($requirementProperty in @($Spec.required_evidence_values.PSObject.Properties)) {
        $actual = $Gap.required_evidence_values.PSObject.Properties[$requirementProperty.Name]
        if ($null -eq $actual) {
            return $false
        }
        if ($null -ne $requirementProperty.Value.PSObject.Properties["allowed_values"]) {
            if (-not (Test-ArrayContainsAll -Actual @($actual.Value.allowed_values) -Expected @($requirementProperty.Value.allowed_values))) {
                return $false
            }
        }
        if ($null -ne $requirementProperty.Value.PSObject.Properties["contains_all"]) {
            if (-not (Test-ArrayContainsAll -Actual @($actual.Value.contains_all) -Expected @($requirementProperty.Value.contains_all))) {
                return $false
            }
        }
    }
    return $true
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedStatusPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $StatusPath) -ErrorAction Stop
    $resolvedGapPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $GapReportPath) -ErrorAction Stop
    $resolvedMarkdownPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $MarkdownPath) -ErrorAction Stop
    $status = Get-Content -LiteralPath $resolvedStatusPath.Path -Raw | ConvertFrom-Json
    $gapReport = Get-Content -LiteralPath $resolvedGapPath.Path -Raw | ConvertFrom-Json
    $markdown = Get-Content -LiteralPath $resolvedMarkdownPath.Path -Raw

    Assert-Condition -Condition ($gapReport.tool -eq "partition-manager-certification-gap-report") -Message "Unexpected gap report tool"
    Assert-Condition -Condition ($gapReport.schema_version -eq 1) -Message "Unexpected gap report schema_version"
    Assert-Condition -Condition ($gapReport.claim_level -eq $status.claim_level) -Message "Gap report claim level mismatch"
    Assert-Condition -Condition ($gapReport.status_path -eq $resolvedStatusPath.Path) -Message "Gap report status_path mismatch"
    Assert-Condition -Condition ($gapReport.source_report_path -eq $status.report.path) -Message "Gap report source report path mismatch"
    Assert-Condition -Condition ($gapReport.external_manifest_path -eq $status.external_gates.manifest_path) -Message "Gap report external manifest path mismatch"

    Assert-Condition -Condition ([int]$gapReport.summary.vhd_data_disk.passed -eq [int]$status.vhd_data_disk.passed) -Message "VHD passed count mismatch"
    Assert-Condition -Condition ([int]$gapReport.summary.vhd_data_disk.required -eq [int]$status.vhd_data_disk.required) -Message "VHD required count mismatch"
    Assert-Condition -Condition ([int]$gapReport.summary.external_gates.passed -eq [int]$status.external_gates.passed) -Message "External passed count mismatch"
    Assert-Condition -Condition ([int]$gapReport.summary.external_gates.required -eq [int]$status.external_gates.required) -Message "External required count mismatch"

    $expectedVhdGapIds = @($status.vhd_data_disk.incomplete_ids | ForEach-Object { $_.ToString() })
    $expectedExternalGapIds = @($status.external_gates.incomplete_ids | ForEach-Object { $_.ToString() })
    $expectedGapIds = @($expectedVhdGapIds + $expectedExternalGapIds | Sort-Object)
    $actualGapIds = @($gapReport.gaps | ForEach-Object { $_.id.ToString() } | Sort-Object)
    Assert-Condition -Condition ($actualGapIds.Count -eq $expectedGapIds.Count) -Message "Gap count mismatch"
    for ($index = 0; $index -lt $expectedGapIds.Count; $index++) {
        Assert-Condition -Condition ($actualGapIds[$index] -eq $expectedGapIds[$index]) -Message "Gap ID mismatch: expected $($expectedGapIds[$index]), got $($actualGapIds[$index])"
    }

    Assert-Condition -Condition ([int]$gapReport.summary.vhd_data_disk.incomplete -eq $expectedVhdGapIds.Count) -Message "VHD incomplete count mismatch"
    Assert-Condition -Condition ([int]$gapReport.summary.external_gates.incomplete -eq $expectedExternalGapIds.Count) -Message "External incomplete count mismatch"
    Assert-Condition -Condition ($markdown.Contains($gapReport.claim_level)) -Message "Gap Markdown missing claim level"
    Assert-Condition -Condition (-not $markdown.Contains("run_partition_manager_destructive_certification.ps1")) -Message "Gap Markdown contains stale direct destructive harness command instead of strict VHD handoff"

    $nextCommands = @($gapReport.next_commands | ForEach-Object { $_.ToString() })
    if ($expectedVhdGapIds.Count -gt 0) {
        Assert-Condition -Condition (($nextCommands -join "`n").Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "Gap report missing strict VHD handoff command"
        Assert-Condition -Condition ($markdown.Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "Gap Markdown missing strict VHD handoff command"
    }
    else {
        Assert-Condition -Condition (-not (($nextCommands -join "`n").Contains("run_partition_manager_vhd_certification_strict.ps1"))) -Message "Gap report should not ask to rerun VHD when VHD gates are complete"
    }

    if ($expectedExternalGapIds.Count -gt 0) {
        Assert-Condition -Condition (($nextCommands -join "`n").Contains("new_partition_manager_external_evidence_manifest.ps1")) -Message "Gap report missing external evidence scaffold command"
        Assert-Condition -Condition (($nextCommands -join "`n").Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "Gap report missing external report import command"
        Assert-Condition -Condition (($nextCommands -join "`n").Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "Gap report missing strict hardware handoff command"
        Assert-Condition -Condition ($markdown.Contains("new_partition_manager_external_evidence_manifest.ps1")) -Message "Gap Markdown missing external evidence scaffold command"
        Assert-Condition -Condition ($markdown.Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "Gap Markdown missing external report import command"
        Assert-Condition -Condition ($markdown.Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "Gap Markdown missing strict hardware handoff command"
    }
    else {
        Assert-Condition -Condition (-not (($nextCommands -join "`n").Contains("run_partition_manager_hardware_certification_strict.ps1"))) -Message "Gap report should not ask to rerun hardware handoff when external gates are complete"
    }

    foreach ($gap in @($gapReport.gaps)) {
        $spec = Get-ScenarioSpec -Matrix $matrix -Id $gap.id
        $expectedCategory = if ($gap.id.StartsWith("vhd.")) {
            "vhd_data_disk"
        }
        else {
            "external_gate"
        }
        Assert-Condition -Condition ($gap.category -eq $expectedCategory) -Message "Gap category mismatch for $($gap.id)"
        Assert-Condition -Condition ($gap.name -eq $spec.name) -Message "Gap name mismatch for $($gap.id)"
        Assert-Condition -Condition ($gap.status -eq "Incomplete") -Message "Gap status mismatch for $($gap.id)"
        Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($gap.required_evidence_keys) -Expected @($spec.required_evidence_keys)) -Message "Gap evidence keys mismatch for $($gap.id)"
        Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($gap.safety_contract) -Expected @($spec.safety_contract)) -Message "Gap safety contract mismatch for $($gap.id)"
        Assert-Condition -Condition (Test-RequiredEvidenceValuesMatch -Gap $gap -Spec $spec) -Message "Gap required evidence values mismatch for $($gap.id)"
        Assert-Condition -Condition ($markdown.Contains($gap.id)) -Message "Gap Markdown missing ID: $($gap.id)"
        foreach ($key in @($spec.required_evidence_keys)) {
            Assert-Condition -Condition ($markdown.Contains($key)) -Message "Gap Markdown missing evidence key: $($gap.id) $key"
        }
    }

    Write-Host "Partition Manager certification gap report passed: $($actualGapIds.Count) incomplete gates verified."
}
finally {
    Pop-Location
}
