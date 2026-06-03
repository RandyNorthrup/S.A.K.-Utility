<#
.SYNOPSIS
    Verifies the generated Partition Manager external lab checklist.

.DESCRIPTION
    Static/non-mutating release gate. The JSON manifest is the machine-verified
    evidence source, but the Markdown checklist is what lab operators follow.
    This script ensures the checklist is current with the certification matrix.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$ChecklistPath = "artifacts\partition-manager-certification\readiness\external-evidence.checklist.md",
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

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    Assert-Condition -Condition (Test-Path -LiteralPath $matrixPath -PathType Leaf) -Message "Certification matrix missing: docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    Assert-Condition -Condition ($matrix.schema_version -eq 1) -Message "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"

    $resolvedChecklistPath = Resolve-ProjectPath -Path $ChecklistPath
    Assert-Condition -Condition (Test-Path -LiteralPath $resolvedChecklistPath -PathType Leaf) -Message "Partition Manager external checklist missing: $ChecklistPath"
    $checklist = Get-Content -LiteralPath $resolvedChecklistPath -Raw

    $sourceName = $ChecklistPath
    Assert-TextContains -Text $checklist -Pattern "# Partition Manager External VM/Hardware Certification Checklist" -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "- Manifest: $ManifestPath" -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "- Matrix: docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json" -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "- Rule: use disposable VMs/media only. Do not run destructive steps on production disks." -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "## Completion Rules" -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "Every required evidence key is filled in the JSON manifest with a non-empty value." -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "Every passed gate has an existing local ``evidence_path`` or absolute HTTP/HTTPS ``evidence_url``." -SourceName $sourceName
    Assert-TextContains -Text $checklist -Pattern "HardwareCertified" -SourceName $sourceName

    $expectedHeadings = @($matrix.external_gates | ForEach-Object { "## $($_.id) - $($_.name)" })
    $actualHeadings = @([regex]::Matches($checklist, "(?m)^## external\.[^\r\n]+") | ForEach-Object { $_.Value })
    Assert-Condition -Condition ($actualHeadings.Count -eq $expectedHeadings.Count) -Message "External checklist gate heading count mismatch: expected $($expectedHeadings.Count), found $($actualHeadings.Count)"

    foreach ($heading in $actualHeadings) {
        Assert-Condition -Condition ($expectedHeadings -contains $heading) -Message "External checklist has unexpected gate heading: $heading"
    }

    foreach ($gate in @($matrix.external_gates)) {
        $heading = "## $($gate.id) - $($gate.name)"
        Assert-TextContains -Text $checklist -Pattern $heading -SourceName $sourceName
        Assert-TextContains -Text $checklist -Pattern "artifacts/partition-manager-certification/external/$($gate.id)/" -SourceName $sourceName

        foreach ($contract in @($gate.safety_contract)) {
            Assert-TextContains -Text $checklist -Pattern ("- [ ] ``{0}``" -f $contract) -SourceName $sourceName
        }

        foreach ($key in @($gate.required_evidence_keys)) {
            $requirementText = ConvertTo-RequirementText -Result $gate -Key $key
            $row = "| ``$key`` | $requirementText |  |"
            Assert-TextContains -Text $checklist -Pattern $row -SourceName $sourceName
        }
    }

    $payloadSectionCount = @([regex]::Matches($checklist, "(?m)^Required evidence payload:\r?$")).Count
    Assert-Condition -Condition ($payloadSectionCount -eq $expectedHeadings.Count) -Message "External checklist payload section count mismatch: expected $($expectedHeadings.Count), found $payloadSectionCount"

    Write-Host "Partition Manager external checklist passed: $($expectedHeadings.Count) external gates verified."
}
finally {
    Pop-Location
}
