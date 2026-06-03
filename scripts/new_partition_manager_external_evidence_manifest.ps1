<#
.SYNOPSIS
    Creates a Partition Manager external certification evidence manifest scaffold.

.DESCRIPTION
    The generated manifest lists every external VM/hardware/lab certification gate.
    It starts as Pending evidence and is intended to be filled with local
    evidence_path or evidence_url entries after disposable VM/hardware/lab runs.
    Use -ChecklistPath to also generate a human lab checklist from the same
    certification matrix so required evidence and safety contracts cannot drift
    from the machine-verified manifest.
#>

[CmdletBinding()]
param(
    [string]$OutputPath = "artifacts\partition-manager-certification\external-evidence.template.json",
    [string]$ChecklistPath = "",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\external",
    [switch]$CreateEvidenceDirectories,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Push-Location $projectRoot
try {
    $matrixPath = Join-Path $projectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }

    function Resolve-ProjectPath {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Path
        )

        if ([System.IO.Path]::IsPathRooted($Path)) {
            return $Path
        }
        return (Join-Path $projectRoot $Path)
    }

    function Ensure-WriteTarget {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Path,
            [Parameter(Mandatory = $true)]
            [string]$Kind
        )

        if ((Test-Path -LiteralPath $Path) -and -not $Force) {
            throw "$Kind already exists: $Path. Use -Force to overwrite."
        }

        $parent = Split-Path -Parent $Path
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
    }

    $resolvedOutputPath = Resolve-ProjectPath -Path $OutputPath
    Ensure-WriteTarget -Path $resolvedOutputPath -Kind "External evidence manifest"

    $resolvedChecklistPath = ""
    if (-not [string]::IsNullOrWhiteSpace($ChecklistPath)) {
        $resolvedChecklistPath = Resolve-ProjectPath -Path $ChecklistPath
        Ensure-WriteTarget -Path $resolvedChecklistPath -Kind "External evidence checklist"
    }

    $resolvedEvidenceRoot = ""
    if ($CreateEvidenceDirectories) {
        if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
            throw "EvidenceRoot is required when -CreateEvidenceDirectories is used."
        }
        $resolvedEvidenceRoot = Resolve-ProjectPath -Path $EvidenceRoot
        New-Item -ItemType Directory -Path $resolvedEvidenceRoot -Force | Out-Null
    }

    function New-EvidenceTemplate {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Gate
        )

        $evidence = [ordered]@{}
        foreach ($key in @($Gate.required_evidence_keys)) {
            $evidence[$key] = ""
        }
        return $evidence
    }

    function Get-EvidenceValueRequirementText {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Result,
            [Parameter(Mandatory = $true)]
            [string]$Key
        )

        if ($null -eq $Result.required_evidence_values) {
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

    function Write-LabChecklist {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Manifest,
            [Parameter(Mandatory = $true)]
            [string]$Path
        )

        $lines = New-Object System.Collections.Generic.List[string]
        $lines.Add("# Partition Manager External VM/Hardware Certification Checklist")
        $lines.Add("")
        $lines.Add("- Generated UTC: $($Manifest.created_utc)")
        $lines.Add("- Manifest: $OutputPath")
        $lines.Add("- Matrix: $($Manifest.certification_matrix)")
        $lines.Add("- Scope: external boot, removable media, SSD/NVMe, BitLocker, partition move/allocation/metadata conversion, and physical wipe gates.")
        $lines.Add("- Rule: use disposable VMs/media only. Do not run destructive steps on production disks.")
        $lines.Add("")
        $lines.Add("## Completion Rules")
        $lines.Add("")
        $lines.Add("- [ ] Each gate below is run in the lab environment named by its safety contract.")
        $lines.Add("- [ ] Every required evidence key is filled in the JSON manifest with a non-empty value.")
        $lines.Add('- [ ] Every passed gate has an existing local `evidence_path` or absolute HTTP/HTTPS `evidence_url`.')
        $lines.Add('- [ ] `scripts/verify_partition_manager_certification.ps1 -RequireVhdDataDiskEvidence -RequireExternalGateEvidence` passes.')
        $lines.Add('- [ ] `scripts/get_partition_manager_certification_status.ps1` reports `HardwareCertified` before release wording uses that claim.')
        $lines.Add("")

        foreach ($result in @($Manifest.results)) {
            $lines.Add("## $($result.id) - $($result.name)")
            $lines.Add("")
            $lines.Add("Safety contract:")
            foreach ($contract in @($result.safety_contract)) {
                $lines.Add("- [ ] ``$contract``")
            }
            $lines.Add("")
            $lines.Add("Required evidence payload:")
            $lines.Add("")
            $lines.Add("| Key | Requirement | Recorded value |")
            $lines.Add("| --- | --- | --- |")
            foreach ($key in @($result.required_evidence_keys)) {
                $requirement = Get-EvidenceValueRequirementText -Result $result -Key $key
                $lines.Add("| ``$key`` | $requirement |  |")
            }
            $lines.Add("")
            $lines.Add("Artifacts:")
            $lines.Add("- [ ] Save command logs, screenshots, exported reports, or VM snapshot notes.")
            $lines.Add("- [ ] Put local artifacts under ``artifacts/partition-manager-certification/external/$($result.id)/`` or provide a stable HTTPS evidence URL.")
            $lines.Add('- [ ] Set JSON `status` to `Passed` only after post-operation verification is complete.')
            $lines.Add('- [ ] Fill `evidence_path` or `evidence_url` in the manifest.')
            $lines.Add("")
            $lines.Add("Operator notes:")
            $lines.Add("")
            $lines.Add("- ")
            $lines.Add("")
        }

        $lines | Set-Content -LiteralPath $Path -Encoding UTF8
    }

    function Get-DisplayPath {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Path
        )

        return $Path.Replace("\", "/")
    }

    function Write-GateRunSheet {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Result,
            [Parameter(Mandatory = $true)]
            [string]$Directory,
            [Parameter(Mandatory = $true)]
            [string]$ManifestPath,
            [Parameter(Mandatory = $true)]
            [string]$EvidenceRootPath
        )

        $readmePath = Join-Path $Directory "README.md"
        if ((Test-Path -LiteralPath $readmePath) -and -not $Force) {
            throw "External evidence run sheet already exists: $readmePath. Use -Force to overwrite."
        }

        $displayRoot = Get-DisplayPath -Path $EvidenceRootPath
        $displayDirectory = Get-DisplayPath -Path (Join-Path $EvidenceRootPath $Result.id)
        $suggestedEvidencePath = "$displayRoot/$($Result.id)/report.json"
        $reportTemplatePath = "$displayDirectory/report.template.json"

        $lines = New-Object System.Collections.Generic.List[string]
        $lines.Add("# $($Result.id) - $($Result.name)")
        $lines.Add("")
        $lines.Add("- Manifest: $ManifestPath")
        $lines.Add("- Matrix: docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json")
        $lines.Add("- Evidence directory: $displayDirectory")
        $lines.Add("- Suggested evidence_path: $suggestedEvidencePath")
        $lines.Add("- Report template: $reportTemplatePath")
        $lines.Add("- Rule: use disposable VMs/media only. Do not run destructive steps on production disks.")
        $lines.Add("- Rule: set JSON status to Passed only after post-operation verification is complete.")
        $lines.Add("")
        $lines.Add("## Safety Contract")
        $lines.Add("")
        foreach ($contract in @($Result.safety_contract)) {
            $lines.Add("- [ ] ``$contract``")
        }
        $lines.Add("")
        $lines.Add("## Required Evidence")
        $lines.Add("")
        $lines.Add("| Key | Requirement | Recorded value |")
        $lines.Add("| --- | --- | --- |")
        foreach ($key in @($Result.required_evidence_keys)) {
            $requirement = Get-EvidenceValueRequirementText -Result $Result -Key $key
            $lines.Add("| ``$key`` | $requirement |  |")
        }
        $lines.Add("")
        $lines.Add("## Artifacts")
        $lines.Add("")
        $lines.Add("- Copy ``report.template.json`` to ``report.json`` and fill it after the lab run, or attach an equivalent evidence report.")
        $lines.Add("- Save command logs, screenshots, exported reports, VM snapshot notes, and before/after layouts in this directory.")
        $lines.Add("- Keep source, target, disk, volume, and partition identities in the attached evidence.")
        $lines.Add("- Fill every manifest evidence key with a non-empty value.")
        $lines.Add("- Fill either evidence_path with an existing local artifact path or evidence_url with an absolute HTTPS URL.")
        $lines.Add("")
        $lines.Add("## Operator Notes")
        $lines.Add("")
        $lines.Add("- ")
        $lines.Add("")

        $lines | Set-Content -LiteralPath $readmePath -Encoding UTF8
    }

    function Write-GateReportTemplate {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Result,
            [Parameter(Mandatory = $true)]
            [string]$Directory,
            [Parameter(Mandatory = $true)]
            [string]$ManifestPath,
            [Parameter(Mandatory = $true)]
            [string]$EvidenceRootPath
        )

        $reportPath = Join-Path $Directory "report.template.json"
        if ((Test-Path -LiteralPath $reportPath) -and -not $Force) {
            throw "External evidence report template already exists: $reportPath. Use -Force to overwrite."
        }

        $template = [ordered]@{
            tool = "partition-manager-external-evidence-report"
            schema_version = 1
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            gate_id = $Result.id
            gate_name = $Result.name
            status = "Pending"
            manifest = $ManifestPath
            certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
            suggested_evidence_path = (Get-DisplayPath -Path "$EvidenceRootPath/$($Result.id)/report.json")
            safety_contract = @($Result.safety_contract)
            required_evidence_keys = @($Result.required_evidence_keys)
            required_evidence_values = if ($null -eq $Result.required_evidence_values) {
                $null
            }
            else {
                $Result.required_evidence_values
            }
            evidence = New-EvidenceTemplate -Gate $Result
            artifacts = @()
            verification_summary = ""
            operator_notes = ""
        }

        $template | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    }

    function Write-EvidencePackage {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Manifest,
            [Parameter(Mandatory = $true)]
            [string]$RootPath
        )

        foreach ($result in @($Manifest.results)) {
            $gateDirectory = Join-Path $RootPath $result.id
            New-Item -ItemType Directory -Path $gateDirectory -Force | Out-Null
            Write-GateRunSheet `
                -Result $result `
                -Directory $gateDirectory `
                -ManifestPath $OutputPath `
                -EvidenceRootPath $EvidenceRoot
            Write-GateReportTemplate `
                -Result $result `
                -Directory $gateDirectory `
                -ManifestPath $OutputPath `
                -EvidenceRootPath $EvidenceRoot
        }
    }

    $manifest = [ordered]@{
        tool = "partition-manager-external-certification"
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        project_root = $projectRoot
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        results = @($matrix.external_gates | ForEach-Object {
                [ordered]@{
                    id = $_.id
                    name = $_.name
                    status = "Pending"
                    required_evidence_keys = @($_.required_evidence_keys)
                    required_evidence_values = if ($null -eq $_.PSObject.Properties["required_evidence_values"]) {
                        $null
                    }
                    else {
                        $_.required_evidence_values
                    }
                    safety_contract = @($_.safety_contract)
                    evidence = New-EvidenceTemplate -Gate $_
                    evidence_path = ""
                    evidence_url = ""
                    notes = "Replace Pending with Passed, fill every evidence key, and attach evidence_path or evidence_url after disposable VM/hardware/lab certification."
                }
            })
    }

    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8
    Write-Host "Partition Manager external evidence manifest created: $resolvedOutputPath"
    if (-not [string]::IsNullOrWhiteSpace($resolvedChecklistPath)) {
        Write-LabChecklist -Manifest ([pscustomobject]$manifest) -Path $resolvedChecklistPath
        Write-Host "Partition Manager external evidence checklist created: $resolvedChecklistPath"
    }
    if ($CreateEvidenceDirectories) {
        Write-EvidencePackage -Manifest ([pscustomobject]$manifest) -RootPath $resolvedEvidenceRoot
        Write-Host "Partition Manager external evidence package created: $resolvedEvidenceRoot"
    }
}
finally {
    Pop-Location
}
