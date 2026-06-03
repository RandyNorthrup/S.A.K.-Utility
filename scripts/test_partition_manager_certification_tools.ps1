<#
.SYNOPSIS
    Self-tests Partition Manager certification verifier and claim-level tools.

.DESCRIPTION
    Builds synthetic certification reports and external evidence manifests under
    artifacts/. No VHDs are created and no disks are mutated.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification\tool-tests"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$CertificationMatrix = Get-Content -LiteralPath (Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json") -Raw | ConvertFrom-Json
if ($CertificationMatrix.schema_version -ne 1) {
    throw "Unsupported Partition Manager certification matrix schema_version: $($CertificationMatrix.schema_version)"
}
$ExpectedVhdScenarioIds = @($CertificationMatrix.vhd_scenarios | ForEach-Object { $_.id })
$ExpectedExternalGateIds = @($CertificationMatrix.external_gates | ForEach-Object { $_.id })

function Get-ScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($CertificationMatrix.vhd_scenarios + $CertificationMatrix.external_gates |
        Where-Object { $_.id -eq $Id })
    if ($matches.Count -eq 0) {
        throw "Certification matrix missing scenario: $Id"
    }
    return $matches[0]
}

function New-SyntheticEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $spec = Get-ScenarioSpec -Id $Id
    $evidence = [ordered]@{}
    foreach ($key in @($spec.required_evidence_keys)) {
        if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
            $requirement = $spec.required_evidence_values.PSObject.Properties[$key]
            if ($null -ne $requirement) {
                if ($null -ne $requirement.Value.PSObject.Properties["allowed_values"]) {
                    $evidence[$key] = @($requirement.Value.allowed_values)[0]
                    continue
                }
                if ($null -ne $requirement.Value.PSObject.Properties["contains_all"]) {
                    $evidence[$key] = @($requirement.Value.contains_all)
                    continue
                }
            }
        }
        $evidence[$key] = switch ($key) {
            "sentinel_verified" { $true }
            "target_overwritten" { $true }
            "signature_verified" { $true }
            "outside_marker_preserved" { $true }
            "remaining_partitions" { 0 }
            "partition_count" { 1 }
            "mbr2gpt_validate_exit_code" { 0 }
            "mbr2gpt_convert_exit_code" { 0 }
            default { "synthetic-$key" }
        }
    }
    return [pscustomobject]$evidence
}

function New-ScenarioResult {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $spec = Get-ScenarioSpec -Id $Id
    $requiredEvidenceValues = if ($null -eq $spec.PSObject.Properties["required_evidence_values"]) {
        $null
    }
    else {
        $spec.required_evidence_values
    }
    [ordered]@{
        id = $Id
        name = $spec.name
        status = $Status
        message = ""
        required_evidence_keys = @($spec.required_evidence_keys)
        required_evidence_values = $requiredEvidenceValues
        safety_contract = @($spec.safety_contract)
        evidence = if ($Status -eq "Passed" -and $Id.StartsWith("vhd.")) {
            New-SyntheticEvidence -Id $Id
        }
        else {
            $null
        }
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    }
}

function Write-CertificationReport {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ReportStatus,
        [Parameter(Mandatory = $true)]
        [hashtable]$VhdStatuses
    )

    $results = @()
    foreach ($id in $ExpectedVhdScenarioIds) {
        $results += New-ScenarioResult -Id $id -Status $VhdStatuses[$id]
    }
    foreach ($id in $ExpectedExternalGateIds) {
        $results += New-ScenarioResult -Id $id -Status "ExternalGate"
    }

    $report = [ordered]@{
        tool = "partition-manager-destructive-certification"
        schema_version = 1
        status = $ReportStatus
        started_utc = (Get-Date).ToUniversalTime().ToString("o")
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME
        project_root = $ProjectRoot
        output_root = Split-Path -Parent $Path
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        run_vhd_data_disk_matrix = ($VhdStatuses.Values -contains "Passed")
        elevated_relaunch_requested = $false
        require_vhd_data_disk_evidence = $false
        keep_vhd = $false
        vhd_size_mb = 768
        administrator = $false
        summary = [ordered]@{
            passed = @($results | Where-Object { $_.status -eq "Passed" }).Count
            failed = @($results | Where-Object { $_.status -eq "Failed" }).Count
            skipped = @($results | Where-Object { $_.status -eq "Skipped" }).Count
            external_gates = @($results | Where-Object { $_.status -eq "ExternalGate" }).Count
        }
        results = $results
    }

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-ExternalManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Status,
        [string]$EvidencePath = "",
        [string]$EvidenceUrl = ""
    )

    $manifest = [ordered]@{
        tool = "partition-manager-external-certification"
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        project_root = $ProjectRoot
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        results = @($ExpectedExternalGateIds | ForEach-Object {
                $spec = Get-ScenarioSpec -Id $_
                [ordered]@{
                    id = $_
                    name = $spec.name
                    status = $Status
                    required_evidence_keys = @($spec.required_evidence_keys)
                    required_evidence_values = if ($null -eq $spec.PSObject.Properties["required_evidence_values"]) {
                        $null
                    }
                    else {
                        $spec.required_evidence_values
                    }
                    safety_contract = @($spec.safety_contract)
                    evidence = if ($Status -eq "Passed") {
                        New-SyntheticEvidence -Id $_
                    }
                    else {
                        $null
                    }
                    evidence_path = $EvidencePath
                    evidence_url = $EvidenceUrl
                    notes = "synthetic certification-tool self-test"
                }
            })
    }

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-VhdStatusMap {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $map = @{}
    foreach ($id in $ExpectedVhdScenarioIds) {
        $map[$id] = $Status
    }
    return $map
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

function Assert-ClaimLevel {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ReportPath,
        [string]$ManifestPath = "",
        [Parameter(Mandatory = $true)]
        [string]$ExpectedClaimLevel,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $statusPath = Write-ClaimStatus `
        -ReportPath $ReportPath `
        -ManifestPath $ManifestPath `
        -Name $Name
    $summary = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($summary.claim_level -eq $ExpectedClaimLevel) -Message "$Name expected $ExpectedClaimLevel, got $($summary.claim_level)"
}

function Write-ClaimStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ReportPath,
        [string]$ManifestPath = "",
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $statusPath = Join-Path $Script:RunRoot "$Name-status.json"
    $arguments = @{
        ReportPath = $ReportPath
        OutputPath = $statusPath
        Quiet = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($ManifestPath)) {
        $arguments.ExternalEvidenceManifest = $ManifestPath
    }

    & (Join-Path $ProjectRoot "scripts\get_partition_manager_certification_status.ps1") @arguments
    return $statusPath
}

function Assert-ToolFails {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    try {
        & $Body
        throw "$Name did not fail"
    }
    catch {
        if ($_.Exception.Message -eq "$Name did not fail") {
            throw
        }
    }
}

function Set-FirstRequiredEvidenceValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $key = @((Get-ScenarioSpec -Id $Result.id).required_evidence_keys)[0]
    $property = $Result.evidence.PSObject.Properties[$key]
    if ($null -eq $property) {
        throw "Synthetic result missing required evidence key: $($Result.id) $key"
    }
    $property.Value = $Value
}

function Set-ObjectProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [object]$Value
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
    else {
        $property.Value = $Value
    }
}

function ConvertTo-ProjectRelativePath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $projectRootFull = [System.IO.Path]::GetFullPath($ProjectRoot)
    if (-not $projectRootFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $projectRootFull += [System.IO.Path]::DirectorySeparatorChar
    }
    if ($fullPath.StartsWith($projectRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $fullPath.Substring($projectRootFull.Length)
    }
    return $fullPath
}

function Write-ExternalEvidenceReports {
    param(
        [Parameter(Mandatory = $true)]
        [string]$EvidenceRoot,
        [Parameter(Mandatory = $true)]
        [string]$ArtifactPath
    )

    foreach ($id in $ExpectedExternalGateIds) {
        $gateDirectory = Join-Path $EvidenceRoot $id
        $templatePath = Join-Path $gateDirectory "report.template.json"
        $reportPath = Join-Path $gateDirectory "report.json"
        $report = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
        Set-ObjectProperty -Object $report -Name "status" -Value "Passed"
        Set-ObjectProperty -Object $report -Name "completed_utc" -Value (Get-Date).ToUniversalTime().ToString("o")
        Set-ObjectProperty -Object $report -Name "evidence" -Value (New-SyntheticEvidence -Id $id)
        Set-ObjectProperty -Object $report -Name "artifacts" -Value @($ArtifactPath)
        Set-ObjectProperty -Object $report -Name "verification_summary" -Value "synthetic external report import self-test"
        Set-ObjectProperty -Object $report -Name "operator_notes" -Value "synthetic report"
        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    }
}

Push-Location $ProjectRoot
try {
    $resolvedOutputRoot = if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
        $OutputRoot
    }
    else {
        Join-Path $ProjectRoot $OutputRoot
    }
    $Script:RunRoot = Join-Path $resolvedOutputRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null

    $vhdPreflightPath = Join-Path $Script:RunRoot "vhd-preflight.json"
    & scripts\test_partition_manager_vhd_preflight.ps1 `
        -OutputRoot $Script:RunRoot `
        -OutputPath $vhdPreflightPath `
        -RequireAdministrator `
        -Quiet
    Assert-Condition -Condition (Test-Path -LiteralPath $vhdPreflightPath) -Message "VHD preflight report was not generated"
    & scripts\check_partition_manager_vhd_preflight_report.ps1 -ReportPath $vhdPreflightPath

    $scaffoldManifest = Join-Path $Script:RunRoot "generated-external-scaffold.json"
    $scaffoldChecklist = Join-Path $Script:RunRoot "generated-external-checklist.md"
    $scaffoldEvidenceRoot = Join-Path $Script:RunRoot "generated-external-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $scaffoldManifest `
        -ChecklistPath $scaffoldChecklist `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -CreateEvidenceDirectories `
        -Force
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldManifest) -Message "external scaffold manifest was not generated"
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldChecklist) -Message "external scaffold checklist was not generated"
    Assert-Condition -Condition (Test-Path -LiteralPath $scaffoldEvidenceRoot -PathType Container) -Message "external scaffold evidence package was not generated"
    $generatedManifest = Get-Content -LiteralPath $scaffoldManifest -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($generatedManifest.results).Count -eq $ExpectedExternalGateIds.Count) -Message "external scaffold manifest gate count mismatch"
    $generatedChecklist = Get-Content -LiteralPath $scaffoldChecklist -Raw
    foreach ($id in $ExpectedExternalGateIds) {
        Assert-Condition -Condition ($generatedChecklist.Contains($id)) -Message "external scaffold checklist missing gate ID: $id"
        $gate = $generatedManifest.results | Where-Object { $_.id -eq $id }
        Assert-Condition -Condition ($null -ne $gate) -Message "external scaffold manifest missing gate: $id"
        foreach ($key in @((Get-ScenarioSpec -Id $id).required_evidence_keys)) {
            Assert-Condition -Condition ($null -ne $gate.evidence.PSObject.Properties[$key]) -Message "external scaffold manifest missing evidence key: $id $key"
            Assert-Condition -Condition ($generatedChecklist.Contains($key)) -Message "external scaffold checklist missing evidence key: $id $key"
        }
        foreach ($contract in @((Get-ScenarioSpec -Id $id).safety_contract)) {
            Assert-Condition -Condition ($generatedChecklist.Contains($contract)) -Message "external scaffold checklist missing safety contract: $id $contract"
        }
        $spec = Get-ScenarioSpec -Id $id
        if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
            Assert-Condition -Condition ($null -ne $gate.required_evidence_values) -Message "external scaffold manifest missing required evidence value contract: $id"
            foreach ($requirementProperty in @($spec.required_evidence_values.PSObject.Properties)) {
                Assert-Condition -Condition ($null -ne $gate.required_evidence_values.PSObject.Properties[$requirementProperty.Name]) -Message "external scaffold manifest missing required evidence value key: $id $($requirementProperty.Name)"
                foreach ($allowedValue in @($requirementProperty.Value.allowed_values)) {
                    Assert-Condition -Condition ($generatedChecklist.Contains($allowedValue.ToString())) -Message "external scaffold checklist missing allowed evidence value: $id $allowedValue"
                }
            }
        }
    }
    & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $scaffoldChecklist -ManifestPath $scaffoldManifest
    & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot $scaffoldEvidenceRoot -ManifestPath $scaffoldManifest

    Assert-ToolFails -Name "missing lab package verifier" -Body {
        & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot (Join-Path $Script:RunRoot "missing-external-evidence") -ManifestPath $scaffoldManifest
    }

    $badLabManifest = Join-Path $Script:RunRoot "bad-external-lab-scaffold.json"
    $badLabChecklist = Join-Path $Script:RunRoot "bad-external-lab-checklist.md"
    $badLabRoot = Join-Path $Script:RunRoot "bad-external-lab-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $badLabManifest `
        -ChecklistPath $badLabChecklist `
        -EvidenceRoot $badLabRoot `
        -CreateEvidenceDirectories `
        -Force
    $badLabReadme = Join-Path $badLabRoot "$($ExpectedExternalGateIds[0])\README.md"
    $badLabText = (Get-Content -LiteralPath $badLabReadme -Raw).Replace(
        "Fill every manifest evidence key with a non-empty value.",
        "Fill stale placeholder values.")
    $badLabText | Set-Content -LiteralPath $badLabReadme -Encoding UTF8
    Assert-ToolFails -Name "stale lab package verifier" -Body {
        & scripts\check_partition_manager_external_lab_package.ps1 -EvidenceRoot $badLabRoot -ManifestPath $badLabManifest
    }

    $firstExternalGate = Get-ScenarioSpec -Id $ExpectedExternalGateIds[0]
    $missingChecklistGate = Join-Path $Script:RunRoot "external-checklist-missing-gate.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $missingChecklistGate -Force
    $missingChecklistGateText = (Get-Content -LiteralPath $missingChecklistGate -Raw).Replace(
        "## $($firstExternalGate.id) - $($firstExternalGate.name)",
        "## external.stale-placeholder - Stale placeholder gate")
    $missingChecklistGateText | Set-Content -LiteralPath $missingChecklistGate -Encoding UTF8
    Assert-ToolFails -Name "missing checklist gate verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $missingChecklistGate -ManifestPath $scaffoldManifest
    }

    $missingChecklistEvidence = Join-Path $Script:RunRoot "external-checklist-missing-evidence.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $missingChecklistEvidence -Force
    $firstExternalEvidenceKey = @($firstExternalGate.required_evidence_keys)[0]
    $missingChecklistEvidenceText = (Get-Content -LiteralPath $missingChecklistEvidence -Raw).Replace(
        "| ``$firstExternalEvidenceKey`` |  |  |",
        "| ``stale_$firstExternalEvidenceKey`` |  |  |")
    $missingChecklistEvidenceText | Set-Content -LiteralPath $missingChecklistEvidence -Encoding UTF8
    Assert-ToolFails -Name "missing checklist evidence verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $missingChecklistEvidence -ManifestPath $scaffoldManifest
    }

    $valueChecklistGateId = @($ExpectedExternalGateIds | Where-Object {
            $null -ne (Get-ScenarioSpec -Id $_).PSObject.Properties["required_evidence_values"]
        } | Select-Object -First 1)[0]
    $valueChecklistGate = Get-ScenarioSpec -Id $valueChecklistGateId
    $valueChecklistRequirement = @($valueChecklistGate.required_evidence_values.PSObject.Properties)[0]
    $valueChecklistAllowed = @($valueChecklistRequirement.Value.allowed_values)[0].ToString()
    $badChecklistValue = Join-Path $Script:RunRoot "external-checklist-bad-required-value.md"
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $badChecklistValue -Force
    $badChecklistValueText = (Get-Content -LiteralPath $badChecklistValue -Raw).Replace(
        "Allowed: $valueChecklistAllowed",
        "Allowed: stale-mode")
    $badChecklistValueText | Set-Content -LiteralPath $badChecklistValue -Encoding UTF8
    Assert-ToolFails -Name "bad checklist required value verifier" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $badChecklistValue -ManifestPath $scaffoldManifest
    }

    $pendingManifest = Join-Path $Script:RunRoot "external-pending.json"
    Write-ExternalManifest -Path $pendingManifest -Status "Pending"

    $evidenceFile = Join-Path $Script:RunRoot "external-evidence.txt"
    Set-Content -LiteralPath $evidenceFile -Value "synthetic external evidence" -Encoding ASCII
    $passedManifest = Join-Path $Script:RunRoot "external-passed.json"
    Write-ExternalManifest -Path $passedManifest -Status "Passed" -EvidencePath $evidenceFile
    Write-ExternalEvidenceReports -EvidenceRoot $scaffoldEvidenceRoot -ArtifactPath $evidenceFile
    $importedFromReportsManifest = Join-Path $Script:RunRoot "external-imported-from-reports.json"
    & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
        -ManifestPath $scaffoldManifest `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -OutputPath $importedFromReportsManifest `
        -RequireAllReports `
        -Force `
        -Quiet
    $importedFromReports = Get-Content -LiteralPath $importedFromReportsManifest -Raw | ConvertFrom-Json
    Assert-Condition -Condition ([int]$importedFromReports.imported_report_count -eq $ExpectedExternalGateIds.Count) -Message "external report import count mismatch"
    foreach ($result in @($importedFromReports.results)) {
        Assert-Condition -Condition ($result.status -eq "Passed") -Message "external report import did not mark result passed: $($result.id)"
        Assert-Condition -Condition ($result.evidence_path -like "*$($result.id)*report.json") -Message "external report import did not point evidence_path at report.json: $($result.id)"
    }

    $staleImportManifest = Join-Path $Script:RunRoot "stale-external-report-import.json"
    $staleImportChecklist = Join-Path $Script:RunRoot "stale-external-report-import.checklist.md"
    $staleImportedManifest = Join-Path $Script:RunRoot "stale-external-report-import.imported.json"
    $staleImportedChecklist = Join-Path $Script:RunRoot "stale-external-report-import.imported.checklist.md"
    Copy-Item -LiteralPath $scaffoldManifest -Destination $staleImportManifest -Force
    Copy-Item -LiteralPath $scaffoldChecklist -Destination $staleImportChecklist -Force
    $staleGate = Get-ScenarioSpec -Id "external.allocate-free-space"
    $staleManifest = Get-Content -LiteralPath $staleImportManifest -Raw | ConvertFrom-Json
    $staleResult = @($staleManifest.results | Where-Object { $_.id -eq $staleGate.id })[0]
    $staleResult.name = "Allocate Free Space donor-volume move/extend proof"
    $staleManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $staleImportManifest -Encoding UTF8
    $staleChecklistText = (Get-Content -LiteralPath $staleImportChecklist -Raw).Replace(
        "## $($staleGate.id) - $($staleGate.name)",
        "## $($staleGate.id) - Allocate Free Space donor-volume move/extend proof")
    $staleChecklistText | Set-Content -LiteralPath $staleImportChecklist -Encoding UTF8
    & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
        -ManifestPath $staleImportManifest `
        -EvidenceRoot $scaffoldEvidenceRoot `
        -OutputPath $staleImportedManifest `
        -ChecklistPath $staleImportChecklist `
        -OutputChecklistPath $staleImportedChecklist `
        -RequireAllReports `
        -Force `
        -Quiet
    $normalizedImport = Get-Content -LiteralPath $staleImportedManifest -Raw | ConvertFrom-Json
    $normalizedResult = @($normalizedImport.results | Where-Object { $_.id -eq $staleGate.id })[0]
    Assert-Condition -Condition ($normalizedResult.name -eq $staleGate.name) -Message "external report import did not normalize stale result name"
    $staleImportedManifestRelative = ConvertTo-ProjectRelativePath -Path $staleImportedManifest
    & scripts\check_partition_manager_external_checklist.ps1 -ChecklistPath $staleImportedChecklist -ManifestPath $staleImportedManifestRelative
    $normalizedChecklistText = Get-Content -LiteralPath $staleImportedChecklist -Raw
    Assert-Condition -Condition ($normalizedChecklistText.Contains("## $($staleGate.id) - $($staleGate.name)")) -Message "external report import did not normalize stale checklist heading"

    $badImportManifest = Join-Path $Script:RunRoot "bad-external-report-import.json"
    $badImportChecklist = Join-Path $Script:RunRoot "bad-external-report-import.checklist.md"
    $badImportRoot = Join-Path $Script:RunRoot "bad-external-report-import"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $badImportManifest `
        -ChecklistPath $badImportChecklist `
        -EvidenceRoot $badImportRoot `
        -CreateEvidenceDirectories `
        -Force
    $badReportGateId = $ExpectedExternalGateIds[0]
    $badReportTemplate = Join-Path $badImportRoot "$badReportGateId\report.template.json"
    $badReportPath = Join-Path $badImportRoot "$badReportGateId\report.json"
    $badReport = Get-Content -LiteralPath $badReportTemplate -Raw | ConvertFrom-Json
    Set-ObjectProperty -Object $badReport -Name "status" -Value "Passed"
    Set-ObjectProperty -Object $badReport -Name "evidence" -Value (New-SyntheticEvidence -Id $badReportGateId)
    Set-ObjectProperty -Object $badReport -Name "artifacts" -Value @($evidenceFile)
    Set-ObjectProperty -Object $badReport -Name "verification_summary" -Value "synthetic bad report"
    $badReportFirstKey = @((Get-ScenarioSpec -Id $badReportGateId).required_evidence_keys)[0]
    $badReport.evidence.PSObject.Properties[$badReportFirstKey].Value = ""
    $badReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badReportPath -Encoding UTF8
    Assert-ToolFails -Name "bad external report import" -Body {
        & scripts\update_partition_manager_external_manifest_from_reports.ps1 `
            -ManifestPath $badImportManifest `
            -EvidenceRoot $badImportRoot `
            -OutputPath (Join-Path $Script:RunRoot "bad-external-report-import-output.json") `
            -Force `
            -Quiet
    }

    $badUrlManifest = Join-Path $Script:RunRoot "external-bad-url.json"
    Write-ExternalManifest -Path $badUrlManifest -Status "Passed" -EvidenceUrl "not-a-url"

    $badRequiredValueManifest = Join-Path $Script:RunRoot "external-bad-required-value.json"
    Copy-Item -LiteralPath $passedManifest -Destination $badRequiredValueManifest -Force
    $badRequiredValue = Get-Content -LiteralPath $badRequiredValueManifest -Raw | ConvertFrom-Json
    $valueConstrainedGateId = @($ExpectedExternalGateIds | Where-Object {
            $null -ne (Get-ScenarioSpec -Id $_).PSObject.Properties["required_evidence_values"]
        } | Select-Object -First 1)[0]
    $valueConstrainedResult = @($badRequiredValue.results | Where-Object { $_.id -eq $valueConstrainedGateId })[0]
    $valueRequirementKey = @((Get-ScenarioSpec -Id $valueConstrainedGateId).required_evidence_values.PSObject.Properties)[0].Name
    $valueConstrainedResult.evidence.PSObject.Properties[$valueRequirementKey].Value = "wrong-mode"
    $badRequiredValue | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $badRequiredValueManifest -Encoding UTF8

    $missingExternalEvidenceManifest = Join-Path $Script:RunRoot "external-missing-evidence.json"
    Write-ExternalManifest -Path $missingExternalEvidenceManifest -Status "Passed" -EvidencePath $evidenceFile
    $badExternalEvidence = Get-Content -LiteralPath $missingExternalEvidenceManifest -Raw | ConvertFrom-Json
    $badExternalEvidence.results[0].evidence = $null
    $badExternalEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $missingExternalEvidenceManifest -Encoding UTF8

    $codeCompleteReport = Join-Path $Script:RunRoot "code-complete-report.json"
    Write-CertificationReport -Path $codeCompleteReport -ReportStatus "NotRun" -VhdStatuses (New-VhdStatusMap -Status "Skipped")
    Assert-ClaimLevel -ReportPath $codeCompleteReport -ManifestPath $pendingManifest -ExpectedClaimLevel "CodeCompleteOnly" -Name "code-complete"
    $codeCompleteGapStatus = Write-ClaimStatus -ReportPath $codeCompleteReport -ManifestPath $pendingManifest -Name "code-complete-gap"
    $codeCompleteGapJson = Join-Path $Script:RunRoot "code-complete-gap-report.json"
    $codeCompleteGapMarkdown = Join-Path $Script:RunRoot "code-complete-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $codeCompleteGapStatus `
        -OutputPath $codeCompleteGapJson `
        -MarkdownPath $codeCompleteGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $codeCompleteGapStatus `
        -GapReportPath $codeCompleteGapJson `
        -MarkdownPath $codeCompleteGapMarkdown
    $codeCompleteBundleJson = Join-Path $Script:RunRoot "code-complete-certification-bundle.json"
    & scripts\new_partition_manager_certification_bundle.ps1 `
        -CertificationRoot $Script:RunRoot `
        -StatusPath $codeCompleteGapStatus `
        -GapReportPath $codeCompleteGapJson `
        -GapMarkdownPath $codeCompleteGapMarkdown `
        -VhdPreflightPath $vhdPreflightPath `
        -ExternalEvidenceManifest $pendingManifest `
        -ExternalEvidenceChecklist $scaffoldChecklist `
        -OutputPath $codeCompleteBundleJson `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $codeCompleteBundleJson
    $codeCompleteGap = Get-Content -LiteralPath $codeCompleteGapJson -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($codeCompleteGap.gaps).Count -eq ($ExpectedVhdScenarioIds.Count + $ExpectedExternalGateIds.Count)) -Message "code-complete gap count mismatch"
    Assert-Condition -Condition ((@($codeCompleteGap.gaps | ForEach-Object { $_.id }) -contains "vhd.image-restore") -and (@($codeCompleteGap.gaps | ForEach-Object { $_.id }) -contains "external.hardware-wipe")) -Message "code-complete gap report missing expected terminal gates"
    $codeCompleteGapCommands = (@($codeCompleteGap.next_commands) -join "`n")
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "code-complete gap report missing strict VHD next command"
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "code-complete gap report missing external report import next command"
    Assert-Condition -Condition ($codeCompleteGapCommands.Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "code-complete gap report missing strict hardware next command"
    Assert-Condition -Condition (-not $codeCompleteGapCommands.Contains("run_partition_manager_destructive_certification.ps1")) -Message "code-complete gap report contains stale direct destructive harness command"

    $vhdReport = Join-Path $Script:RunRoot "vhd-report.json"
    Write-CertificationReport -Path $vhdReport -ReportStatus "Partial" -VhdStatuses (New-VhdStatusMap -Status "Passed")
    Assert-ClaimLevel -ReportPath $vhdReport -ManifestPath $pendingManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "vhd"
    $vhdGapStatus = Write-ClaimStatus -ReportPath $vhdReport -ManifestPath $pendingManifest -Name "vhd-gap"
    $vhdGapJson = Join-Path $Script:RunRoot "vhd-gap-report.json"
    $vhdGapMarkdown = Join-Path $Script:RunRoot "vhd-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $vhdGapStatus `
        -OutputPath $vhdGapJson `
        -MarkdownPath $vhdGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $vhdGapStatus `
        -GapReportPath $vhdGapJson `
        -MarkdownPath $vhdGapMarkdown
    $vhdGap = Get-Content -LiteralPath $vhdGapJson -Raw | ConvertFrom-Json
    $vhdGapCommands = (@($vhdGap.next_commands) -join "`n")
    Assert-Condition -Condition (@($vhdGap.gaps).Count -eq $ExpectedExternalGateIds.Count) -Message "vhd-certified gap count mismatch"
    Assert-Condition -Condition (-not $vhdGapCommands.Contains("run_partition_manager_vhd_certification_strict.ps1")) -Message "vhd-certified gap report should not ask to rerun VHD"
    Assert-Condition -Condition ($vhdGapCommands.Contains("new_partition_manager_external_evidence_manifest.ps1")) -Message "vhd-certified gap report missing external scaffold command"
    Assert-Condition -Condition ($vhdGapCommands.Contains("update_partition_manager_external_manifest_from_reports.ps1")) -Message "vhd-certified gap report missing external report import command"
    Assert-Condition -Condition ($vhdGapCommands.Contains("run_partition_manager_hardware_certification_strict.ps1")) -Message "vhd-certified gap report missing strict hardware next command"

    $legacyExternalPlaceholderReport = Join-Path $Script:RunRoot "legacy-external-placeholder-report.json"
    Copy-Item -LiteralPath $vhdReport -Destination $legacyExternalPlaceholderReport -Force
    $legacyReport = Get-Content -LiteralPath $legacyExternalPlaceholderReport -Raw | ConvertFrom-Json
    $legacyBootResult = @($legacyReport.results | Where-Object { $_.id -eq "external.boot-repair-uefi" })[0]
    $legacyBootResult.id = "external.boot-repair"
    $legacyBootResult.name = "UEFI and BIOS boot repair with intentionally broken BCD"
    $legacyBootResult.required_evidence_values = $null
    $legacyBootResult.safety_contract = @("disposable_boot_vm_only", "pre_repair_snapshot", "commands_logged", "post_repair_boot_verified")
    $legacyReport.results = @($legacyReport.results | Where-Object { $_.id -ne "external.boot-repair-bios" })
    $legacyReport.summary.external_gates = [int]$legacyReport.summary.external_gates - 1
    $legacyReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $legacyExternalPlaceholderReport -Encoding UTF8

    $hardwareReport = Join-Path $Script:RunRoot "hardware-report.json"
    Write-CertificationReport -Path $hardwareReport -ReportStatus "Passed" -VhdStatuses (New-VhdStatusMap -Status "Passed")
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $passedManifest -ExpectedClaimLevel "HardwareCertified" -Name "hardware"
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $importedFromReportsManifest -ExpectedClaimLevel "HardwareCertified" -Name "hardware-imported-reports"
    & scripts\verify_partition_manager_certification.ps1 `
        -ReportPath $hardwareReport `
        -ExternalEvidenceManifest $importedFromReportsManifest `
        -RequireVhdDataDiskEvidence `
        -RequireExternalGateEvidence
    $hardwareStrictScaffold = Join-Path $Script:RunRoot "hardware-strict-external-evidence.json"
    $hardwareStrictChecklist = Join-Path $Script:RunRoot "hardware-strict-external-evidence.checklist.md"
    $hardwareStrictEvidenceRoot = Join-Path $Script:RunRoot "hardware-strict-external-evidence"
    & scripts\new_partition_manager_external_evidence_manifest.ps1 `
        -OutputPath $hardwareStrictScaffold `
        -ChecklistPath $hardwareStrictChecklist `
        -EvidenceRoot $hardwareStrictEvidenceRoot `
        -CreateEvidenceDirectories `
        -Force
    $hardwareStrictManifest = Get-Content -LiteralPath $hardwareStrictScaffold -Raw | ConvertFrom-Json
    foreach ($result in @($hardwareStrictManifest.results)) {
        $result.status = "Passed"
        $result.evidence = New-SyntheticEvidence -Id $result.id
        $result.evidence_path = $evidenceFile
        $result.notes = "synthetic strict hardware certification self-test"
    }
    $hardwareStrictManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $hardwareStrictScaffold -Encoding UTF8
    & scripts\run_partition_manager_hardware_certification_strict.ps1 `
        -CertificationRoot $Script:RunRoot `
        -ReportPath $hardwareReport `
        -ExternalEvidenceManifest $hardwareStrictScaffold `
        -ExternalEvidenceChecklist $hardwareStrictChecklist `
        -ExternalEvidenceRoot $hardwareStrictEvidenceRoot `
        -OutputRoot (Join-Path $Script:RunRoot "hardware-strict-output")
    Assert-ToolFails -Name "strict hardware pending evidence verifier" -Body {
        & scripts\run_partition_manager_hardware_certification_strict.ps1 `
            -CertificationRoot $Script:RunRoot `
            -ReportPath $hardwareReport `
            -ExternalEvidenceManifest $pendingManifest `
            -ExternalEvidenceChecklist $scaffoldChecklist `
            -ExternalEvidenceRoot $scaffoldEvidenceRoot `
            -OutputRoot (Join-Path $Script:RunRoot "hardware-strict-pending-output")
    }
    $hardwareGapStatus = Write-ClaimStatus -ReportPath $hardwareReport -ManifestPath $passedManifest -Name "hardware-gap"
    $hardwareGapJson = Join-Path $Script:RunRoot "hardware-gap-report.json"
    $hardwareGapMarkdown = Join-Path $Script:RunRoot "hardware-gap-report.md"
    & scripts\new_partition_manager_certification_gap_report.ps1 `
        -StatusPath $hardwareGapStatus `
        -OutputPath $hardwareGapJson `
        -MarkdownPath $hardwareGapMarkdown `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_gap_report.ps1 `
        -StatusPath $hardwareGapStatus `
        -GapReportPath $hardwareGapJson `
        -MarkdownPath $hardwareGapMarkdown
    $hardwareBundleJson = Join-Path $Script:RunRoot "hardware-certification-bundle.json"
    & scripts\new_partition_manager_certification_bundle.ps1 `
        -CertificationRoot $Script:RunRoot `
        -StatusPath $hardwareGapStatus `
        -GapReportPath $hardwareGapJson `
        -GapMarkdownPath $hardwareGapMarkdown `
        -VhdPreflightPath $vhdPreflightPath `
        -ExternalEvidenceManifest $passedManifest `
        -ExternalEvidenceChecklist $scaffoldChecklist `
        -OutputPath $hardwareBundleJson `
        -Force `
        -Quiet
    & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $hardwareBundleJson
    $hardwareGap = Get-Content -LiteralPath $hardwareGapJson -Raw | ConvertFrom-Json
    Assert-Condition -Condition (@($hardwareGap.gaps).Count -eq 0) -Message "hardware gap report should have no gaps"

    $failedStatus = New-VhdStatusMap -Status "Passed"
    $failedStatus[$ExpectedVhdScenarioIds[0]] = "Failed"
    $failedReport = Join-Path $Script:RunRoot "failed-report.json"
    Write-CertificationReport -Path $failedReport -ReportStatus "Failed" -VhdStatuses $failedStatus
    Assert-ClaimLevel -ReportPath $failedReport -ManifestPath $passedManifest -ExpectedClaimLevel "FailedEvidence" -Name "failed"

    & scripts\verify_partition_manager_certification.ps1 -ReportPath $codeCompleteReport -ExternalEvidenceManifest $pendingManifest
    & scripts\verify_partition_manager_certification.ps1 -ReportPath $legacyExternalPlaceholderReport -RequireVhdDataDiskEvidence
    & scripts\verify_partition_manager_certification.ps1 -ReportPath $hardwareReport -ExternalEvidenceManifest $passedManifest -RequireVhdDataDiskEvidence -RequireExternalGateEvidence

    $extraIdReport = Join-Path $Script:RunRoot "extra-id-report.json"
    Copy-Item -LiteralPath $codeCompleteReport -Destination $extraIdReport -Force
    $badReport = Get-Content -LiteralPath $extraIdReport -Raw | ConvertFrom-Json
    $extra = $badReport.results[0].PSObject.Copy()
    $extra.id = "vhd.unexpected-extra"
    $badReport.results += $extra
    $badReport.summary.skipped = [int]$badReport.summary.skipped + 1
    $badReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $extraIdReport -Encoding UTF8
    Assert-ToolFails -Name "unexpected ID verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $extraIdReport
    }

    $missingEvidenceReport = Join-Path $Script:RunRoot "missing-evidence-report.json"
    Copy-Item -LiteralPath $hardwareReport -Destination $missingEvidenceReport -Force
    $badEvidence = Get-Content -LiteralPath $missingEvidenceReport -Raw | ConvertFrom-Json
    $badEvidence.results[0].evidence = $null
    $badEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $missingEvidenceReport -Encoding UTF8
    Assert-ToolFails -Name "missing evidence verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $missingEvidenceReport -RequireVhdDataDiskEvidence
    }

    $blankEvidenceReport = Join-Path $Script:RunRoot "blank-evidence-report.json"
    Copy-Item -LiteralPath $hardwareReport -Destination $blankEvidenceReport -Force
    $blankEvidence = Get-Content -LiteralPath $blankEvidenceReport -Raw | ConvertFrom-Json
    Set-FirstRequiredEvidenceValue -Result $blankEvidence.results[0] -Value ""
    $blankEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $blankEvidenceReport -Encoding UTF8
    Assert-ClaimLevel -ReportPath $blankEvidenceReport -ManifestPath $pendingManifest -ExpectedClaimLevel "CodeCompleteOnly" -Name "blank-vhd-evidence"
    Assert-ToolFails -Name "blank evidence verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $blankEvidenceReport -RequireVhdDataDiskEvidence
    }

    Assert-ToolFails -Name "bad URL verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $hardwareReport -ExternalEvidenceManifest $badUrlManifest -RequireExternalGateEvidence
    }

    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $badRequiredValueManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "bad-required-external-value"
    Assert-ToolFails -Name "bad required evidence value verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $hardwareReport -ExternalEvidenceManifest $badRequiredValueManifest -RequireExternalGateEvidence
    }

    Assert-ToolFails -Name "missing external evidence verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $hardwareReport -ExternalEvidenceManifest $missingExternalEvidenceManifest -RequireExternalGateEvidence
    }

    $blankExternalEvidenceManifest = Join-Path $Script:RunRoot "external-blank-evidence.json"
    Copy-Item -LiteralPath $passedManifest -Destination $blankExternalEvidenceManifest -Force
    $blankExternalEvidence = Get-Content -LiteralPath $blankExternalEvidenceManifest -Raw | ConvertFrom-Json
    Set-FirstRequiredEvidenceValue -Result $blankExternalEvidence.results[0] -Value ""
    $blankExternalEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $blankExternalEvidenceManifest -Encoding UTF8
    Assert-ClaimLevel -ReportPath $hardwareReport -ManifestPath $blankExternalEvidenceManifest -ExpectedClaimLevel "VhdDataDiskCertified" -Name "blank-external-evidence"
    Assert-ToolFails -Name "blank external evidence verifier" -Body {
        & scripts\verify_partition_manager_certification.ps1 -ReportPath $hardwareReport -ExternalEvidenceManifest $blankExternalEvidenceManifest -RequireExternalGateEvidence
    }

    Write-Host "Partition Manager certification tool self-test passed: $Script:RunRoot"
}
finally {
    Pop-Location
}
