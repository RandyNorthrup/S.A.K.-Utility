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

    # NOT $matches: that is PowerShell's automatic regex-capture variable, and clobbering it
    # here would corrupt any -match result the callers below rely on.
    $scenarioMatches = @($Matrix.vhd_scenarios + $Matrix.external_gates |
        Where-Object { $_.id -eq $Id })
    Assert-Condition -Condition ($scenarioMatches.Count -eq 1) -Message "Certification matrix missing or duplicate scenario: $Id"
    return $scenarioMatches[0]
}

function Test-SchemaVersion {
    param(
        [object]$Value
    )

    # Fail closed: a schema version must be the INTEGER 1 -- not the string "1", not $null
    # coerced by [int], and not an array whose comparison result [bool] coerces to $true.
    return (($Value -is [int]) -or ($Value -is [long])) -and ([int]$Value -eq 1)
}

function Get-RequiredCount {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Container,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    # Fail closed: a missing or wrong-typed count must NOT coerce to 0 via [int]. Two absent
    # counts would otherwise compare 0 -eq 0 and pass, so a gap report with no summary object
    # at all would satisfy every count check below.
    Assert-Condition -Condition ($null -ne $Container) -Message "$Context is missing"
    $property = $Container.PSObject.Properties[$Name]
    Assert-Condition -Condition ($null -ne $property) -Message "$Context is missing $Name"
    $value = $property.Value
    Assert-Condition -Condition (($value -is [int]) -or ($value -is [long])) -Message "$Context $Name is not an integer: $value"
    Assert-Condition -Condition ([int]$value -ge 0) -Message "$Context $Name is negative: $value"
    return [int]$value
}

function Test-ArrayMatchesExactly {
    param(
        [object[]]$Actual,
        [object[]]$Expected
    )

    # Fail closed: the gap report must mirror the matrix EXACTLY. A subset-only check let an
    # extra, duplicated or re-cased entry through while the caller's message still claimed the
    # sets matched -- which silently weakens the very contract this gate publishes.
    $actualStrings = @(@($Actual) | ForEach-Object { $_.ToString() })
    $expectedStrings = @(@($Expected) | ForEach-Object { $_.ToString() })
    if ($actualStrings.Count -ne $expectedStrings.Count) {
        return $false
    }
    foreach ($item in $expectedStrings) {
        if ($actualStrings -cnotcontains $item) {
            return $false
        }
    }
    foreach ($item in $actualStrings) {
        if ($expectedStrings -cnotcontains $item) {
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
            if (-not (Test-ArrayMatchesExactly -Actual @($actual.Value.allowed_values) -Expected @($requirementProperty.Value.allowed_values))) {
                return $false
            }
        }
        if ($null -ne $requirementProperty.Value.PSObject.Properties["contains_all"]) {
            if (-not (Test-ArrayMatchesExactly -Actual @($actual.Value.contains_all) -Expected @($requirementProperty.Value.contains_all))) {
                return $false
            }
        }
    }
    return $true
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition (Test-SchemaVersion -Value $matrix.schema_version) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedStatusPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $StatusPath) -ErrorAction Stop
    $resolvedGapPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $GapReportPath) -ErrorAction Stop
    $resolvedMarkdownPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $MarkdownPath) -ErrorAction Stop
    $status = Get-Content -LiteralPath $resolvedStatusPath.Path -Raw | ConvertFrom-Json
    $gapReport = Get-Content -LiteralPath $resolvedGapPath.Path -Raw | ConvertFrom-Json
    $markdown = Get-Content -LiteralPath $resolvedMarkdownPath.Path -Raw

    Assert-Condition -Condition ($gapReport.tool -eq "partition-manager-certification-gap-report") -Message "Unexpected gap report tool"
    Assert-Condition -Condition (Test-SchemaVersion -Value $gapReport.schema_version) -Message "Unexpected gap report schema_version"

    # Fail closed: the status file is EVIDENCE, not a trusted oracle. Verify its identity and
    # schema (the generator does the same) before any of its fields decide this gate, and take
    # every compared value as an exact STRING -- an array-valued field would otherwise pass a
    # collection comparison that [bool]$Condition coerces to $true.
    Assert-Condition -Condition (($status.tool -is [string]) -and ($status.tool -ceq "partition-manager-certification-status")) -Message "Unexpected certification status tool: $($status.tool)"
    Assert-Condition -Condition (Test-SchemaVersion -Value $status.schema_version) -Message "Unsupported Partition Manager certification status schema_version: $($status.schema_version)"
    Assert-Condition -Condition (($status.claim_level -is [string]) -and -not [string]::IsNullOrWhiteSpace($status.claim_level)) -Message "Certification status has a missing or non-string claim_level"
    Assert-Condition -Condition ($gapReport.claim_level -is [string]) -Message "Gap report has a non-string claim_level"
    Assert-Condition -Condition ($gapReport.claim_level -ceq $status.claim_level) -Message "Gap report claim level mismatch"
    Assert-Condition -Condition (($gapReport.status_path -is [string]) -and ($gapReport.status_path -eq $resolvedStatusPath.Path)) -Message "Gap report status_path mismatch"
    Assert-Condition -Condition (($gapReport.source_report_path -is [string]) -and ($status.report.path -is [string]) -and ($gapReport.source_report_path -eq $status.report.path)) -Message "Gap report source report path mismatch"
    Assert-Condition -Condition (($gapReport.external_manifest_path -is [string]) -and ($status.external_gates.manifest_path -is [string]) -and ($gapReport.external_manifest_path -eq $status.external_gates.manifest_path)) -Message "Gap report external manifest path mismatch"

    $statusVhdPassed = Get-RequiredCount -Container $status.vhd_data_disk -Name "passed" -Context "Certification status vhd_data_disk"
    $statusVhdRequired = Get-RequiredCount -Container $status.vhd_data_disk -Name "required" -Context "Certification status vhd_data_disk"
    $statusExternalPassed = Get-RequiredCount -Container $status.external_gates -Name "passed" -Context "Certification status external_gates"
    $statusExternalRequired = Get-RequiredCount -Container $status.external_gates -Name "required" -Context "Certification status external_gates"
    $gapVhdPassed = Get-RequiredCount -Container $gapReport.summary.vhd_data_disk -Name "passed" -Context "Gap report summary vhd_data_disk"
    $gapVhdRequired = Get-RequiredCount -Container $gapReport.summary.vhd_data_disk -Name "required" -Context "Gap report summary vhd_data_disk"
    $gapVhdIncomplete = Get-RequiredCount -Container $gapReport.summary.vhd_data_disk -Name "incomplete" -Context "Gap report summary vhd_data_disk"
    $gapExternalPassed = Get-RequiredCount -Container $gapReport.summary.external_gates -Name "passed" -Context "Gap report summary external_gates"
    $gapExternalRequired = Get-RequiredCount -Container $gapReport.summary.external_gates -Name "required" -Context "Gap report summary external_gates"
    $gapExternalIncomplete = Get-RequiredCount -Container $gapReport.summary.external_gates -Name "incomplete" -Context "Gap report summary external_gates"

    Assert-Condition -Condition ($gapVhdPassed -eq $statusVhdPassed) -Message "VHD passed count mismatch"
    Assert-Condition -Condition ($gapVhdRequired -eq $statusVhdRequired) -Message "VHD required count mismatch"
    Assert-Condition -Condition ($gapExternalPassed -eq $statusExternalPassed) -Message "External passed count mismatch"
    Assert-Condition -Condition ($gapExternalRequired -eq $statusExternalRequired) -Message "External required count mismatch"

    $expectedVhdGapIds = @($status.vhd_data_disk.incomplete_ids | ForEach-Object { $_.ToString() })
    $expectedExternalGapIds = @($status.external_gates.incomplete_ids | ForEach-Object { $_.ToString() })

    # Claim invariants. Every expected gate is either passed or incomplete, so the counts must
    # add up; a certification claim with ZERO required gates is meaningless; and a claim level
    # must be backed by the counts it asserts. Without these, a fabricated all-zero status with
    # a matching gap report would sail through this gate as HardwareCertified.
    Assert-Condition -Condition ($statusVhdRequired -gt 0) -Message "Certification status claims zero required VHD gates"
    Assert-Condition -Condition ($statusExternalRequired -gt 0) -Message "Certification status claims zero required external gates"
    Assert-Condition -Condition (($statusVhdPassed + $expectedVhdGapIds.Count) -eq $statusVhdRequired) -Message "Certification status VHD passed+incomplete does not equal required"
    Assert-Condition -Condition (($statusExternalPassed + $expectedExternalGapIds.Count) -eq $statusExternalRequired) -Message "Certification status external passed+incomplete does not equal required"
    if ($status.claim_level -ceq "HardwareCertified") {
        Assert-Condition -Condition (($expectedVhdGapIds.Count -eq 0) -and ($expectedExternalGapIds.Count -eq 0)) -Message "Certification status claims HardwareCertified with incomplete gates"
    }
    if ($status.claim_level -ceq "VhdDataDiskCertified") {
        Assert-Condition -Condition ($expectedVhdGapIds.Count -eq 0) -Message "Certification status claims VhdDataDiskCertified with incomplete VHD gates"
    }

    $expectedGapIds = @($expectedVhdGapIds + $expectedExternalGapIds | Sort-Object)
    $actualGapIds = @($gapReport.gaps | ForEach-Object { $_.id.ToString() } | Sort-Object)
    Assert-Condition -Condition ($actualGapIds.Count -eq $expectedGapIds.Count) -Message "Gap count mismatch"
    for ($index = 0; $index -lt $expectedGapIds.Count; $index++) {
        Assert-Condition -Condition ($actualGapIds[$index] -eq $expectedGapIds[$index]) -Message "Gap ID mismatch: expected $($expectedGapIds[$index]), got $($actualGapIds[$index])"
    }

    Assert-Condition -Condition ($gapVhdIncomplete -eq $expectedVhdGapIds.Count) -Message "VHD incomplete count mismatch"
    Assert-Condition -Condition ($gapExternalIncomplete -eq $expectedExternalGapIds.Count) -Message "External incomplete count mismatch"
    # Match the generator's actual header line, not a bare token that any stray text satisfies.
    Assert-Condition -Condition ($markdown.Contains("- Claim level: $($gapReport.claim_level)")) -Message "Gap Markdown missing claim level line"
    Assert-Condition -Condition (-not $markdown.Contains("run_partition_manager_destructive_certification.ps1")) -Message "Gap Markdown contains stale direct destructive harness command instead of strict VHD handoff"

    # Fail closed: a null, blank or empty-object entry is MALFORMED evidence, not something to
    # drop silently -- dropping it let a "must not contain" branch below pass by luck.
    $rawNextCommands = @($gapReport.next_commands)
    $nextCommands = @($rawNextCommands | Where-Object {
            $null -ne $_ -and -not [string]::IsNullOrWhiteSpace($_.ToString()) -and $_.ToString() -ne "@{}"
        } | ForEach-Object { $_.ToString() })
    Assert-Condition -Condition ($nextCommands.Count -eq $rawNextCommands.Count) -Message "Gap report next_commands contains a null, blank or empty entry"
    # The stale destructive harness was rejected in Markdown only, so the JSON could still hand
    # an operator the command this gate exists to keep out of the flow.
    Assert-Condition -Condition (-not (($nextCommands -join "`n").Contains("run_partition_manager_destructive_certification.ps1"))) -Message "Gap report next_commands contains stale direct destructive harness command instead of strict VHD handoff"
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
        Assert-Condition -Condition (Test-ArrayMatchesExactly -Actual @($gap.required_evidence_keys) -Expected @($spec.required_evidence_keys)) -Message "Gap evidence keys mismatch for $($gap.id)"
        Assert-Condition -Condition (Test-ArrayMatchesExactly -Actual @($gap.safety_contract) -Expected @($spec.safety_contract)) -Message "Gap safety contract mismatch for $($gap.id)"
        Assert-Condition -Condition (Test-RequiredEvidenceValuesMatch -Gap $gap -Spec $spec) -Message "Gap required evidence values mismatch for $($gap.id)"
        Assert-Condition -Condition (($gap.required_action -is [string]) -and -not [string]::IsNullOrWhiteSpace($gap.required_action)) -Message "Gap missing required_action for $($gap.id)"
        # Structural Markdown checks, not bare token presence: one unrelated line mentioning an
        # ID or an evidence key used to satisfy this gate while the gap section itself was
        # missing. These are the exact lines new_partition_manager_certification_gap_report.ps1
        # writes for each gap.
        Assert-Condition -Condition ($markdown.Contains("## $($gap.id) - $($spec.name)")) -Message "Gap Markdown missing section heading for: $($gap.id)"
        Assert-Condition -Condition ($markdown.Contains("- Required action: $($gap.required_action)")) -Message "Gap Markdown missing required action for: $($gap.id)"
        foreach ($key in @($spec.required_evidence_keys)) {
            Assert-Condition -Condition ($markdown.Contains("- ``$key``")) -Message "Gap Markdown missing evidence key: $($gap.id) $key"
        }
        foreach ($contract in @($spec.safety_contract)) {
            Assert-Condition -Condition ($markdown.Contains("- ``$contract``")) -Message "Gap Markdown missing safety contract: $($gap.id) $contract"
        }
    }

    Write-Host "Partition Manager certification gap report passed: $($actualGapIds.Count) incomplete gates verified."
}
finally {
    Pop-Location
}
