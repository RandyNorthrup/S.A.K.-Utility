<#
.SYNOPSIS
    Verifies Partition Manager certification matrix integrity and doc/tool coverage.

.DESCRIPTION
    Static release gate. It does not mutate disks. It prevents certification IDs,
    evidence keys, value requirements, safety contracts, docs, and harness coverage
    from drifting apart before release readiness creates any reports.
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

function Get-RepoText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $path = Join-Path $ProjectRoot $RelativePath
    Assert-Condition -Condition (Test-Path -LiteralPath $path -PathType Leaf) -Message "Matrix integrity file missing: $RelativePath"
    return Get-Content -LiteralPath $path -Raw
}

function Assert-TextContains {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Pattern,
        [Parameter(Mandatory = $true)]
        [string]$SourceName
    )

    Assert-Condition -Condition $Text.Contains($Pattern) -Message "$SourceName missing '$Pattern'"
}

function ConvertTo-RequiredStringArray {
    param(
        [object]$Value,
        [Parameter(Mandatory = $true)]
        [string]$FieldName,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $items = @($Value | ForEach-Object {
            if ($null -eq $_) {
                ""
            }
            else {
                $_.ToString()
            }
        })
    Assert-Condition -Condition ($items.Count -gt 0) -Message "$Id has no $FieldName values"
    foreach ($item in $items) {
        Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($item)) -Message "$Id has blank $FieldName value"
    }

    return $items
}

function Assert-UniqueStrings {
    param(
        [string[]]$Values,
        [Parameter(Mandatory = $true)]
        [string]$FieldName,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $duplicates = @($Values | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    Assert-Condition -Condition ($duplicates.Count -eq 0) -Message "$Id has duplicate $FieldName value(s): $($duplicates -join ', ')"
}

function Assert-EvidenceValueContracts {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Scenario,
        [Parameter(Mandatory = $true)]
        [string[]]$EvidenceKeys
    )

    $contractProperty = $Scenario.PSObject.Properties["required_evidence_values"]
    if ($null -eq $contractProperty -or $null -eq $contractProperty.Value) {
        return
    }

    foreach ($requirementProperty in @($contractProperty.Value.PSObject.Properties)) {
        $key = $requirementProperty.Name
        Assert-Condition -Condition ($EvidenceKeys -contains $key) -Message "$($Scenario.id) required_evidence_values key '$key' is not listed in required_evidence_keys"

        $requirement = $requirementProperty.Value
        $contractNames = @($requirement.PSObject.Properties | ForEach-Object { $_.Name })
        Assert-Condition -Condition ($contractNames.Count -gt 0) -Message "$($Scenario.id) evidence value requirement for '$key' is empty"

        foreach ($contractName in $contractNames) {
            Assert-Condition -Condition (@("allowed_values", "contains_all") -contains $contractName) -Message "$($Scenario.id) evidence value requirement for '$key' uses unsupported contract '$contractName'"
            $values = ConvertTo-RequiredStringArray -Value $requirement.PSObject.Properties[$contractName].Value -FieldName "required_evidence_values.$key.$contractName" -Id $Scenario.id
            Assert-UniqueStrings -Values $values -FieldName "required_evidence_values.$key.$contractName" -Id $Scenario.id
        }
    }
}

function Assert-ScenarioIntegrity {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Scenario,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedPrefix,
        [Parameter(Mandatory = $true)]
        [string]$GroupName
    )

    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Scenario.id)) -Message "$GroupName scenario missing id"
    Assert-Condition -Condition ($Scenario.id.StartsWith($ExpectedPrefix, [System.StringComparison]::Ordinal)) -Message "$GroupName scenario '$($Scenario.id)' must start with '$ExpectedPrefix'"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($Scenario.name)) -Message "$($Scenario.id) missing name"

    $evidenceKeys = ConvertTo-RequiredStringArray -Value $Scenario.required_evidence_keys -FieldName "required_evidence_keys" -Id $Scenario.id
    $safetyContracts = ConvertTo-RequiredStringArray -Value $Scenario.safety_contract -FieldName "safety_contract" -Id $Scenario.id
    Assert-UniqueStrings -Values $evidenceKeys -FieldName "required_evidence_keys" -Id $Scenario.id
    Assert-UniqueStrings -Values $safetyContracts -FieldName "safety_contract" -Id $Scenario.id
    Assert-EvidenceValueContracts -Scenario $Scenario -EvidenceKeys $evidenceKeys
}

function Assert-DynamicMatrixReader {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Patterns
    )

    $text = Get-RepoText -RelativePath $RelativePath
    foreach ($pattern in $Patterns) {
        Assert-TextContains -Text $text -Pattern $pattern -SourceName $RelativePath
    }
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    Assert-Condition -Condition (Test-Path -LiteralPath $matrixPath -PathType Leaf) -Message "Certification matrix missing: docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $vhdScenarios = @($matrix.vhd_scenarios)
    $externalGates = @($matrix.external_gates)
    Assert-Condition -Condition ($vhdScenarios.Count -gt 0) -Message "Certification matrix has no VHD scenarios"
    Assert-Condition -Condition ($externalGates.Count -gt 0) -Message "Certification matrix has no external gates"

    foreach ($scenario in $vhdScenarios) {
        Assert-ScenarioIntegrity -Scenario $scenario -ExpectedPrefix "vhd." -GroupName "VHD"
    }
    foreach ($gate in $externalGates) {
        Assert-ScenarioIntegrity -Scenario $gate -ExpectedPrefix "external." -GroupName "External"
    }

    $allIds = @($vhdScenarios + $externalGates | ForEach-Object { $_.id })
    Assert-UniqueStrings -Values $allIds -FieldName "certification scenario id" -Id "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"

    $certificationDoc = Get-RepoText -RelativePath "docs\PARTITION_MANAGER_CERTIFICATION.md"
    foreach ($scenario in @($vhdScenarios + $externalGates)) {
        Assert-TextContains -Text $certificationDoc -Pattern $scenario.id -SourceName "docs\PARTITION_MANAGER_CERTIFICATION.md"
        Assert-TextContains -Text $certificationDoc -Pattern $scenario.name -SourceName "docs\PARTITION_MANAGER_CERTIFICATION.md"
    }

    $harness = Get-RepoText -RelativePath "scripts\run_partition_manager_destructive_certification.ps1"
    foreach ($scenario in $vhdScenarios) {
        Assert-TextContains -Text $harness -Pattern $scenario.id -SourceName "scripts\run_partition_manager_destructive_certification.ps1"
        Assert-TextContains -Text $harness -Pattern $scenario.name -SourceName "scripts\run_partition_manager_destructive_certification.ps1"
    }

    Assert-DynamicMatrixReader -RelativePath "scripts\run_partition_manager_destructive_certification.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "CertificationMatrix.external_gates")
    Assert-DynamicMatrixReader -RelativePath "scripts\new_partition_manager_external_evidence_manifest.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "matrix.external_gates", "required_evidence_keys", "safety_contract", "Write-LabChecklist", "Write-EvidencePackage", "Write-GateReportTemplate", "report.template.json", "CreateEvidenceDirectories")
    Assert-DynamicMatrixReader -RelativePath "scripts\verify_partition_manager_certification.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "ExpectedVhdScenarioIds", "ExpectedExternalGateIds", "required_evidence_values")
    Assert-DynamicMatrixReader -RelativePath "scripts\get_partition_manager_certification_status.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "ExpectedVhdScenarioIds", "ExpectedExternalGateIds", "Test-EvidenceValueRequirement")
    Assert-DynamicMatrixReader -RelativePath "scripts\test_partition_manager_certification_tools.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "ExpectedVhdScenarioIds", "ExpectedExternalGateIds", "required_evidence_values")
    Assert-DynamicMatrixReader -RelativePath "scripts\check_partition_manager_external_checklist.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "external_gates", "required_evidence_keys", "safety_contract", "required_evidence_values")
    Assert-DynamicMatrixReader -RelativePath "scripts\check_partition_manager_external_lab_package.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "external_gates", "required_evidence_keys", "safety_contract", "required_evidence_values", "EvidenceRoot", "report.template.json", "partition-manager-external-evidence-report")
    Assert-DynamicMatrixReader -RelativePath "scripts\update_partition_manager_external_manifest_from_reports.ps1" -Patterns @("PARTITION_MANAGER_CERTIFICATION_MATRIX.json", "external_gates", "required_evidence_keys", "safety_contract", "required_evidence_values", "report.json", "partition-manager-external-evidence-report")
    Assert-DynamicMatrixReader -RelativePath "scripts\run_partition_manager_hardware_certification_strict.ps1" -Patterns @("RequireVhdDataDiskEvidence", "RequireExternalGateEvidence", "HardwareCertified", "check_partition_manager_external_checklist.ps1", "check_partition_manager_external_lab_package.ps1", "hardware-certification-artifact-bundle.json")

    Write-Host "Partition Manager certification matrix integrity passed: $($vhdScenarios.Count) VHD scenarios, $($externalGates.Count) external gates."
}
finally {
    Pop-Location
}
