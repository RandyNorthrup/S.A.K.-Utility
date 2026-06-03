<#
.SYNOPSIS
    Imports completed Partition Manager external evidence reports into a manifest.

.DESCRIPTION
    Reads per-gate report.json files generated from report.template.json and
    writes an updated external evidence manifest. This does not mutate disks and
    does not certify evidence by itself; it only copies matrix-validated report
    payloads into the machine-verified manifest shape used by the strict
    hardware handoff. When a checklist path is supplied, the importer also
    writes a paired checklist copy that references the imported manifest path so
    strict handoff verification can keep the manifest/checklist contract intact.
#>

[CmdletBinding()]
param(
    [string]$ManifestPath = "artifacts\partition-manager-certification\vhd-strict\external-evidence.json",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vhd-strict\external-evidence",
    [string]$OutputPath = "",
    [string]$ChecklistPath = "",
    [string]$OutputChecklistPath = "",
    [switch]$RequireAllReports,
    [switch]$Force,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

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

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $ProjectRoot $Path
}

function ConvertTo-ProjectRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $projectRootFull = [System.IO.Path]::GetFullPath($ProjectRoot)
    if ($fullPath.StartsWith($projectRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $fullPath.Substring($projectRootFull.Length).TrimStart("\")
    }
    return $fullPath
}

function Write-ImportedChecklist {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceChecklistPath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationChecklistPath,
        [Parameter(Mandatory = $true)]
        [string]$ImportedManifestPath
    )

    $resolvedSource = Resolve-ProjectPath -Path $SourceChecklistPath
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedSource -PathType Leaf) -Message "External evidence checklist missing: $SourceChecklistPath"

    $resolvedDestination = Resolve-ProjectPath -Path $DestinationChecklistPath
    if ((Test-Path -LiteralPath $resolvedDestination) -and -not $Force) {
        throw "External checklist import output already exists: $resolvedDestination. Use -Force to overwrite."
    }

    $destinationParent = Split-Path -Parent $resolvedDestination
    if (-not [string]::IsNullOrWhiteSpace($destinationParent)) {
        New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    }

    $manifestRelativePath = ConvertTo-ProjectRelativePath -Path $ImportedManifestPath
    $checklistText = Get-Content -LiteralPath $resolvedSource -Raw
    Assert-Condition -Condition ($checklistText -match "(?m)^- Manifest: .+$") -Message "External evidence checklist missing manifest line: $SourceChecklistPath"

    $manifestLine = "- Manifest: $manifestRelativePath"
    $updatedChecklist = [regex]::Replace(
        $checklistText,
        "(?m)^- Manifest: .+$",
        [System.Text.RegularExpressions.MatchEvaluator] { param($match) $manifestLine }
    )
    $importedManifest = Get-Content -LiteralPath (Resolve-ProjectPath -Path $ImportedManifestPath) -Raw | ConvertFrom-Json
    foreach ($result in @($importedManifest.results)) {
        $headingPattern = "(?m)^## " + [regex]::Escape($result.id) + " - .+$"
        $headingLine = "## $($result.id) - $($result.name)"
        $updatedChecklist = [regex]::Replace(
            $updatedChecklist,
            $headingPattern,
            [System.Text.RegularExpressions.MatchEvaluator] { param($match) $headingLine }
        )
    }
    $updatedChecklist | Set-Content -LiteralPath $resolvedDestination -Encoding UTF8
    return $resolvedDestination
}

function Update-LabPackageManifestReferences {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LabEvidenceRoot,
        [Parameter(Mandatory = $true)]
        [string]$ImportedManifestPath
    )

    $resolvedRoot = Resolve-ProjectPath -Path $LabEvidenceRoot
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedRoot -PathType Container) -Message "External evidence root missing: $LabEvidenceRoot"

    $manifestRelativePath = ConvertTo-ProjectRelativePath -Path $ImportedManifestPath
    $importedManifest = Get-Content -LiteralPath (Resolve-ProjectPath -Path $ImportedManifestPath) -Raw | ConvertFrom-Json
    $resultById = @{}
    foreach ($result in @($importedManifest.results)) {
        $resultById[$result.id] = $result
    }
    $updatedCount = 0
    foreach ($gateDirectory in @(Get-ChildItem -LiteralPath $resolvedRoot -Directory)) {
        $gate = $resultById[$gateDirectory.Name]
        $readmePath = Join-Path $gateDirectory.FullName "README.md"
        if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
            $readmeText = Get-Content -LiteralPath $readmePath -Raw
            if ($null -ne $gate) {
                $headingLine = "# $($gate.id) - $($gate.name)"
                $readmeText = [regex]::Replace(
                    $readmeText,
                    "(?m)^# " + [regex]::Escape($gate.id) + " - .+$",
                    [System.Text.RegularExpressions.MatchEvaluator] { param($match) $headingLine }
                )
            }
            if ($readmeText -match "(?m)^- Manifest: .+$") {
                $manifestLine = "- Manifest: $manifestRelativePath"
                $readmeText = [regex]::Replace(
                    $readmeText,
                    "(?m)^- Manifest: .+$",
                    [System.Text.RegularExpressions.MatchEvaluator] { param($match) $manifestLine }
                )
                $readmeText | Set-Content -LiteralPath $readmePath -Encoding UTF8
            }
        }

        $templatePath = Join-Path $gateDirectory.FullName "report.template.json"
        if (Test-Path -LiteralPath $templatePath -PathType Leaf) {
            $template = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
            Set-JsonProperty -Object $template -Name "manifest" -Value $manifestRelativePath
            if ($null -ne $gate) {
                Set-JsonProperty -Object $template -Name "gate_name" -Value $gate.name
                Set-JsonProperty -Object $template -Name "safety_contract" -Value @($gate.safety_contract)
                Set-JsonProperty -Object $template -Name "required_evidence_keys" -Value @($gate.required_evidence_keys)
                Set-JsonProperty -Object $template -Name "required_evidence_values" -Value $gate.required_evidence_values
                $evidence = [ordered]@{}
                foreach ($key in @($gate.required_evidence_keys)) {
                    $evidence[$key] = ""
                }
                Set-JsonProperty -Object $template -Name "evidence" -Value $evidence
            }
            $template | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $templatePath -Encoding UTF8
        }
        $updatedCount++
    }

    return $updatedCount
}

function Get-ScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Matrix,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($Matrix.external_gates | Where-Object { $_.id -eq $Id })
    Assert-Condition -Condition ($matches.Count -eq 1) -Message "Certification matrix missing or duplicated external gate: $Id"
    return $matches[0]
}

function Set-JsonProperty {
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

function Test-EvidenceValuePresent {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return $false
    }
    if ($Value -is [string]) {
        return -not [string]::IsNullOrWhiteSpace($Value)
    }
    if ($Value -is [System.Array]) {
        return $Value.Count -gt 0
    }
    if ($Value -is [pscustomobject]) {
        return @($Value.PSObject.Properties).Count -gt 0
    }
    return $true
}

function ConvertTo-EvidenceValueStrings {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return @()
    }
    if ($Value -is [System.Array]) {
        return @($Value | ForEach-Object { $_.ToString() })
    }
    return @($Value.ToString())
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

function Assert-RequiredEvidenceValuesMatch {
    param(
        [object]$Report,
        [object]$Spec
    )

    if ($null -eq $Spec.PSObject.Properties["required_evidence_values"]) {
        Assert-Condition -Condition ($null -eq $Report.required_evidence_values) -Message "Report should not carry required evidence values for $($Spec.id)"
        return
    }
    Assert-Condition -Condition ($null -ne $Report.PSObject.Properties["required_evidence_values"] -and $null -ne $Report.required_evidence_values) -Message "Report missing required evidence values for $($Spec.id)"

    foreach ($requirementProperty in @($Spec.required_evidence_values.PSObject.Properties)) {
        $actual = $Report.required_evidence_values.PSObject.Properties[$requirementProperty.Name]
        Assert-Condition -Condition ($null -ne $actual) -Message "Report required evidence values for $($Spec.id) missing $($requirementProperty.Name)"
        if ($null -ne $requirementProperty.Value.PSObject.Properties["allowed_values"]) {
            Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($actual.Value.allowed_values) -Expected @($requirementProperty.Value.allowed_values)) -Message "Report allowed evidence values mismatch for $($Spec.id) $($requirementProperty.Name)"
        }
        if ($null -ne $requirementProperty.Value.PSObject.Properties["contains_all"]) {
            Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($actual.Value.contains_all) -Expected @($requirementProperty.Value.contains_all)) -Message "Report required evidence values mismatch for $($Spec.id) $($requirementProperty.Name)"
        }
    }
}

function Assert-EvidenceValueRequirement {
    param(
        [object]$Evidence,
        [string]$Key,
        [object]$Requirement,
        [string]$GateId
    )

    $actualValues = ConvertTo-EvidenceValueStrings -Value $Evidence.PSObject.Properties[$Key].Value
    if ($null -ne $Requirement.PSObject.Properties["allowed_values"]) {
        $allowedValues = @($Requirement.allowed_values | ForEach-Object { $_.ToString() })
        $matched = @($actualValues | Where-Object { $allowedValues -contains $_ }).Count -gt 0
        Assert-Condition -Condition $matched -Message "Report evidence value for $GateId $Key must be one of: $($allowedValues -join ', ')"
    }
    if ($null -ne $Requirement.PSObject.Properties["contains_all"]) {
        foreach ($requiredValue in @($Requirement.contains_all | ForEach-Object { $_.ToString() })) {
            Assert-Condition -Condition ($actualValues -contains $requiredValue) -Message "Report evidence value for $GateId $Key missing required value: $requiredValue"
        }
    }
}

function Assert-ExternalReport {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [Parameter(Mandatory = $true)]
        [object]$Spec,
        [Parameter(Mandatory = $true)]
        [string]$ReportPath
    )

    Assert-Condition -Condition ($Report.tool -eq "partition-manager-external-evidence-report") -Message "Unexpected external evidence report tool: $ReportPath"
    Assert-Condition -Condition ($Report.schema_version -eq 1) -Message "Unexpected external evidence report schema_version for $($Spec.id)"
    Assert-Condition -Condition ($Report.gate_id -eq $Spec.id) -Message "External evidence report gate ID mismatch: expected $($Spec.id), got $($Report.gate_id)"
    Assert-Condition -Condition ($Report.gate_name -eq $Spec.name) -Message "External evidence report gate name mismatch for $($Spec.id)"
    Assert-Condition -Condition ($Report.status -eq "Passed") -Message "External evidence report must be Passed before import: $($Spec.id)=$($Report.status)"
    Assert-Condition -Condition ($Report.certification_matrix -eq "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json") -Message "External evidence report matrix mismatch for $($Spec.id)"
    Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($Report.required_evidence_keys) -Expected @($Spec.required_evidence_keys)) -Message "External evidence report keys mismatch for $($Spec.id)"
    Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($Report.safety_contract) -Expected @($Spec.safety_contract)) -Message "External evidence report safety contract mismatch for $($Spec.id)"
    Assert-RequiredEvidenceValuesMatch -Report $Report -Spec $Spec
    Assert-Condition -Condition ($null -ne $Report.evidence) -Message "External evidence report missing evidence payload for $($Spec.id)"
    Assert-Condition -Condition (Test-EvidenceValuePresent -Value $Report.verification_summary) -Message "External evidence report missing verification_summary for $($Spec.id)"

    foreach ($key in @($Spec.required_evidence_keys)) {
        $property = $Report.evidence.PSObject.Properties[$key]
        Assert-Condition -Condition ($null -ne $property) -Message "External evidence report missing evidence key: $($Spec.id) $key"
        Assert-Condition -Condition (Test-EvidenceValuePresent -Value $property.Value) -Message "External evidence report missing evidence value: $($Spec.id) $key"
        if ($null -ne $Spec.PSObject.Properties["required_evidence_values"]) {
            $requirement = $Spec.required_evidence_values.PSObject.Properties[$key]
            if ($null -ne $requirement) {
                Assert-EvidenceValueRequirement -Evidence $Report.evidence -Key $key -Requirement $requirement.Value -GateId $Spec.id
            }
        }
    }
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedManifestPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $ManifestPath) -ErrorAction Stop
    $resolvedEvidenceRoot = Resolve-ProjectPath -Path $EvidenceRoot
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedEvidenceRoot -PathType Container) -Message "External evidence root missing: $EvidenceRoot"

    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $manifestDirectory = Split-Path -Parent $resolvedManifestPath.Path
        $manifestName = [System.IO.Path]::GetFileNameWithoutExtension($resolvedManifestPath.Path)
        $OutputPath = Join-Path $manifestDirectory "$manifestName.imported.json"
    }
    $resolvedOutputPath = Resolve-ProjectPath -Path $OutputPath
    if ((Test-Path -LiteralPath $resolvedOutputPath) -and -not $Force) {
        throw "External manifest import output already exists: $resolvedOutputPath. Use -Force to overwrite."
    }
    $outputParent = Split-Path -Parent $resolvedOutputPath
    if (-not [string]::IsNullOrWhiteSpace($outputParent)) {
        New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
    }

    $manifest = Get-Content -LiteralPath $resolvedManifestPath.Path -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($manifest.tool -eq "partition-manager-external-certification") -Message "Unexpected external evidence manifest tool"
    Assert-Condition -Condition ($manifest.schema_version -eq 1) -Message "Unexpected external evidence manifest schema_version"

    $importedCount = 0
    $missingReports = New-Object System.Collections.Generic.List[string]
    foreach ($gate in @($matrix.external_gates)) {
        $manifestResult = @($manifest.results | Where-Object { $_.id -eq $gate.id })
        Assert-Condition -Condition ($manifestResult.Count -eq 1) -Message "Manifest missing or duplicated external gate: $($gate.id)"
        $result = $manifestResult[0]
        Set-JsonProperty -Object $result -Name "name" -Value $gate.name
        Set-JsonProperty -Object $result -Name "required_evidence_keys" -Value @($gate.required_evidence_keys)
        if ($null -ne $gate.PSObject.Properties["required_evidence_values"]) {
            Set-JsonProperty -Object $result -Name "required_evidence_values" -Value $gate.required_evidence_values
        }
        else {
            Set-JsonProperty -Object $result -Name "required_evidence_values" -Value $null
        }
        Set-JsonProperty -Object $result -Name "safety_contract" -Value @($gate.safety_contract)

        $reportPath = Join-Path (Join-Path $resolvedEvidenceRoot $gate.id) "report.json"
        if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
            $missingReports.Add($gate.id)
            continue
        }

        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        Assert-ExternalReport -Report $report -Spec $gate -ReportPath $reportPath
        Set-JsonProperty -Object $result -Name "status" -Value "Passed"
        Set-JsonProperty -Object $result -Name "required_evidence_values" -Value $report.required_evidence_values
        Set-JsonProperty -Object $result -Name "evidence" -Value $report.evidence
        Set-JsonProperty -Object $result -Name "evidence_path" -Value (ConvertTo-ProjectRelativePath -Path $reportPath)
        Set-JsonProperty -Object $result -Name "evidence_url" -Value ""
        Set-JsonProperty -Object $result -Name "completed_utc" -Value (Get-Date).ToUniversalTime().ToString("o")
        Set-JsonProperty -Object $result -Name "notes" -Value "Imported from $((ConvertTo-ProjectRelativePath -Path $reportPath).Replace('\', '/'))."
        $importedCount++
    }

    if ($RequireAllReports -and $missingReports.Count -gt 0) {
        throw "Missing external evidence report(s): $($missingReports -join ', ')"
    }

    Set-JsonProperty -Object $manifest -Name "imported_utc" -Value (Get-Date).ToUniversalTime().ToString("o")
    Set-JsonProperty -Object $manifest -Name "imported_report_count" -Value $importedCount
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8
    $updatedLabPackageCount = Update-LabPackageManifestReferences -LabEvidenceRoot $EvidenceRoot -ImportedManifestPath $resolvedOutputPath

    $writtenChecklistPath = ""
    if (-not [string]::IsNullOrWhiteSpace($ChecklistPath)) {
        if ([string]::IsNullOrWhiteSpace($OutputChecklistPath)) {
            $outputManifestDirectory = Split-Path -Parent $resolvedOutputPath
            $outputManifestName = [System.IO.Path]::GetFileNameWithoutExtension($resolvedOutputPath)
            $OutputChecklistPath = Join-Path $outputManifestDirectory "$outputManifestName.checklist.md"
        }

        $writtenChecklistPath = Write-ImportedChecklist `
            -SourceChecklistPath $ChecklistPath `
            -DestinationChecklistPath $OutputChecklistPath `
            -ImportedManifestPath $resolvedOutputPath
    }

    if (-not $Quiet) {
        Write-Host "Partition Manager external manifest import written: $resolvedOutputPath"
        Write-Host "Partition Manager external lab package manifest references updated: $updatedLabPackageCount"
        if (-not [string]::IsNullOrWhiteSpace($writtenChecklistPath)) {
            Write-Host "Partition Manager external checklist import written: $writtenChecklistPath"
        }
        Write-Host "Imported reports: $importedCount"
        if ($missingReports.Count -gt 0) {
            Write-Host "Missing reports: $($missingReports -join ', ')"
        }
    }
}
finally {
    Pop-Location
}
