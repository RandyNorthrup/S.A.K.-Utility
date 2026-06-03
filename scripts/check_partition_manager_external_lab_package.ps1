<#
.SYNOPSIS
    Verifies the generated Partition Manager external lab evidence package.

.DESCRIPTION
    Static/non-mutating release gate. The package contains one run sheet per
    external VM/hardware/lab gate. It does not prove destructive evidence; it
    proves the lab handoff directories remain synchronized with the matrix and
    manifest so operators collect the right artifacts before strict claims.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\readiness\external-evidence",
    [string]$ManifestPath = "artifacts\partition-manager-certification\readiness\external-evidence.template.json"
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

function Assert-TextContainsAny {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string[]]$Patterns,
        [Parameter(Mandatory = $true)]
        [string]$SourceName,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    foreach ($pattern in $Patterns) {
        if ($Text.Contains($pattern)) {
            return
        }
    }

    throw "$SourceName missing $Description"
}

function ConvertTo-RequirementText {
    param(
        [object]$Result,
        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    if ($null -eq $Result.PSObject.Properties["required_evidence_values"] -or
        $null -eq $Result.required_evidence_values) {
        return ""
    }

    $requirement = $Result.required_evidence_values.PSObject.Properties[$Key]
    if ($null -eq $requirement) {
        return ""
    }
    if ($null -ne $requirement.Value.PSObject.Properties["allowed_values"]) {
        return "Allowed: " + ((@($requirement.Value.allowed_values) | ForEach-Object { $_.ToString() }) -join ", ")
    }
    if ($null -ne $requirement.Value.PSObject.Properties["contains_all"]) {
        return "Must include: " + ((@($requirement.Value.contains_all) | ForEach-Object { $_.ToString() }) -join ", ")
    }
    return ""
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
        [object]$Template,
        [object]$Gate
    )

    if ($null -eq $Gate.PSObject.Properties["required_evidence_values"]) {
        return ($null -eq $Template.required_evidence_values)
    }
    if ($null -eq $Template.PSObject.Properties["required_evidence_values"] -or $null -eq $Template.required_evidence_values) {
        return $false
    }

    foreach ($requirementProperty in @($Gate.required_evidence_values.PSObject.Properties)) {
        $actual = $Template.required_evidence_values.PSObject.Properties[$requirementProperty.Name]
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

function Get-DisplayPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return $Path.Replace("\", "/")
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    Assert-Condition -Condition (Test-Path -LiteralPath $matrixPath -PathType Leaf) -Message "Certification matrix missing: docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedManifestPath = Resolve-ProjectPath -Path $ManifestPath
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedManifestPath -PathType Leaf) -Message "Partition Manager external evidence manifest missing: $ManifestPath"
    $manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($manifest.tool -eq "partition-manager-external-certification") -Message "Unexpected external evidence manifest tool"
    Assert-Condition -Condition ($manifest.schema_version -eq 1) -Message "Unexpected external evidence manifest schema_version"

    $resolvedEvidenceRoot = Resolve-ProjectPath -Path $EvidenceRoot
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedEvidenceRoot -PathType Container) -Message "Partition Manager external lab package missing: $EvidenceRoot"

    $manifestIds = @($manifest.results | ForEach-Object { $_.id })
    $matrixIds = @($matrix.external_gates | ForEach-Object { $_.id })
    Assert-Condition -Condition ($manifestIds.Count -eq $matrixIds.Count) -Message "External manifest gate count mismatch: expected $($matrixIds.Count), found $($manifestIds.Count)"
    foreach ($id in $matrixIds) {
        Assert-Condition -Condition ($manifestIds -contains $id) -Message "External manifest missing gate: $id"
    }

    $displayRoot = Get-DisplayPath -Path $EvidenceRoot
    $displayResolvedRoot = Get-DisplayPath -Path $resolvedEvidenceRoot
    $manifestPathOptions = @(
        "- Manifest: $ManifestPath",
        "- Manifest: $resolvedManifestPath"
    ) | Sort-Object -Unique
    foreach ($gate in @($matrix.external_gates)) {
        $gateDirectory = Join-Path $resolvedEvidenceRoot $gate.id
        Assert-Condition -Condition (Test-Path -LiteralPath $gateDirectory -PathType Container) -Message "External lab package gate directory missing: $($gate.id)"

        $readmePath = Join-Path $gateDirectory "README.md"
        Assert-Condition -Condition (Test-Path -LiteralPath $readmePath -PathType Leaf) -Message "External lab package run sheet missing: $($gate.id)"
        $readme = Get-Content -LiteralPath $readmePath -Raw
        $sourceName = "$EvidenceRoot/$($gate.id)/README.md"

        Assert-TextContains -Text $readme -Pattern "# $($gate.id) - $($gate.name)" -SourceName $sourceName
        Assert-TextContainsAny -Text $readme -Patterns $manifestPathOptions -SourceName $sourceName -Description "manifest path"
        Assert-TextContains -Text $readme -Pattern "- Matrix: docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json" -SourceName $sourceName
        Assert-TextContainsAny -Text $readme -Patterns @(
            "- Evidence directory: $displayRoot/$($gate.id)",
            "- Evidence directory: $displayResolvedRoot/$($gate.id)"
        ) -SourceName $sourceName -Description "evidence directory"
        Assert-TextContainsAny -Text $readme -Patterns @(
            "- Suggested evidence_path: $displayRoot/$($gate.id)/report.json",
            "- Suggested evidence_path: $displayResolvedRoot/$($gate.id)/report.json"
        ) -SourceName $sourceName -Description "suggested evidence path"
        Assert-TextContainsAny -Text $readme -Patterns @(
            "- Report template: $displayRoot/$($gate.id)/report.template.json",
            "- Report template: $displayResolvedRoot/$($gate.id)/report.template.json"
        ) -SourceName $sourceName -Description "report template path"
        Assert-TextContains -Text $readme -Pattern "- Rule: use disposable VMs/media only. Do not run destructive steps on production disks." -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "- Rule: set JSON status to Passed only after post-operation verification is complete." -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "## Safety Contract" -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "## Required Evidence" -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "## Artifacts" -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "Copy ``report.template.json`` to ``report.json`` and fill it after the lab run, or attach an equivalent evidence report." -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "Fill every manifest evidence key with a non-empty value." -SourceName $sourceName
        Assert-TextContains -Text $readme -Pattern "Fill either evidence_path with an existing local artifact path or evidence_url with an absolute HTTPS URL." -SourceName $sourceName

        $reportTemplatePath = Join-Path $gateDirectory "report.template.json"
        Assert-Condition -Condition (Test-Path -LiteralPath $reportTemplatePath -PathType Leaf) -Message "External lab package report template missing: $($gate.id)"
        $reportTemplate = Get-Content -LiteralPath $reportTemplatePath -Raw | ConvertFrom-Json
        Assert-Condition -Condition ($reportTemplate.tool -eq "partition-manager-external-evidence-report") -Message "Unexpected report template tool for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.schema_version -eq 1) -Message "Unexpected report template schema_version for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.gate_id -eq $gate.id) -Message "Report template gate ID mismatch for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.gate_name -eq $gate.name) -Message "Report template gate name mismatch for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.status -eq "Pending") -Message "Report template status mismatch for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.manifest -eq $ManifestPath) -Message "Report template manifest path mismatch for $($gate.id)"
        Assert-Condition -Condition ($reportTemplate.certification_matrix -eq "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json") -Message "Report template matrix path mismatch for $($gate.id)"
        Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($reportTemplate.safety_contract) -Expected @($gate.safety_contract)) -Message "Report template safety contract mismatch for $($gate.id)"
        Assert-Condition -Condition (Test-ArrayContainsAll -Actual @($reportTemplate.required_evidence_keys) -Expected @($gate.required_evidence_keys)) -Message "Report template evidence keys mismatch for $($gate.id)"
        Assert-Condition -Condition (Test-RequiredEvidenceValuesMatch -Template $reportTemplate -Gate $gate) -Message "Report template required evidence values mismatch for $($gate.id)"

        foreach ($contract in @($gate.safety_contract)) {
            Assert-TextContains -Text $readme -Pattern ("- [ ] ``{0}``" -f $contract) -SourceName $sourceName
        }

        foreach ($key in @($gate.required_evidence_keys)) {
            $requirementText = ConvertTo-RequirementText -Result $gate -Key $key
            $row = "| ``$key`` | $requirementText |  |"
            Assert-TextContains -Text $readme -Pattern $row -SourceName $sourceName
            Assert-Condition -Condition ($null -ne $reportTemplate.evidence.PSObject.Properties[$key]) -Message "Report template evidence placeholder missing: $($gate.id) $key"
        }
    }

    Write-Host "Partition Manager external lab package passed: $($matrixIds.Count) gate run sheets and report templates verified."
}
finally {
    Pop-Location
}
