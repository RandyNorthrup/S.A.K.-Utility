<#
.SYNOPSIS
    Creates a Partition Manager certification gap report.

.DESCRIPTION
    Reads certification-status.json and the certification matrix, then writes
    machine-readable JSON plus Markdown that list every incomplete VHD or
    external gate with required evidence and safety contracts.
#>

[CmdletBinding()]
param(
    [string]$StatusPath = "artifacts\partition-manager-certification\readiness\certification-status.json",
    [string]$OutputPath = "",
    [string]$MarkdownPath = "",
    [switch]$Force,
    [switch]$Quiet
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

function Get-ScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Matrix,
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($Matrix.vhd_scenarios + $Matrix.external_gates |
        Where-Object { $_.id -eq $Id })
    if ($matches.Count -ne 1) {
        throw "Certification matrix missing or duplicate scenario: $Id"
    }
    return $matches[0]
}

function New-GapEntry {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Matrix,
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Category
    )

    $spec = Get-ScenarioSpec -Matrix $Matrix -Id $Id
    [ordered]@{
        id = $Id
        category = $Category
        name = $spec.name
        status = "Incomplete"
        required_evidence_keys = @($spec.required_evidence_keys)
        required_evidence_values = if ($null -eq $spec.PSObject.Properties["required_evidence_values"]) {
            $null
        }
        else {
            $spec.required_evidence_values
        }
        safety_contract = @($spec.safety_contract)
        required_action = if ($Category -eq "vhd_data_disk") {
            "Run the elevated disposable-VHD certification matrix and keep the generated report."
        }
        else {
            "Run the disposable VM/hardware/lab gate, fill the per-gate report.json, import reports into the external manifest, and attach evidence_path or evidence_url."
        }
    }
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

function New-NextCommandList {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Status,
        [Parameter(Mandatory = $true)]
        [string]$StatusDirectory,
        [string[]]$VhdGapIds,
        [string[]]$ExternalGapIds
    )

    $commands = New-Object System.Collections.Generic.List[string]
    $strictCertificationRoot = if ($Status.claims.vhd_data_disk_certified) {
        $StatusDirectory
    }
    else {
        Join-Path $ProjectRoot "artifacts\partition-manager-certification\vhd-strict"
    }
    $strictCertificationRootArg = ConvertTo-ProjectRelativePath -Path $strictCertificationRoot
    $externalManifestArg = ConvertTo-ProjectRelativePath -Path (Join-Path $strictCertificationRoot "external-evidence.json")
    $externalImportedManifestArg = ConvertTo-ProjectRelativePath -Path (Join-Path $strictCertificationRoot "external-evidence.imported.json")
    $externalChecklistArg = ConvertTo-ProjectRelativePath -Path (Join-Path $strictCertificationRoot "external-evidence.checklist.md")
    $externalEvidenceRootArg = ConvertTo-ProjectRelativePath -Path (Join-Path $strictCertificationRoot "external-evidence")

    if ($VhdGapIds.Count -gt 0) {
        $commands.Add("powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_partition_manager_vhd_certification_strict.ps1 -RelaunchElevated")
    }

    if ($ExternalGapIds.Count -gt 0) {
        $commands.Add("powershell -NoProfile -ExecutionPolicy Bypass -File scripts\new_partition_manager_external_evidence_manifest.ps1 -OutputPath $externalManifestArg -ChecklistPath $externalChecklistArg -EvidenceRoot $externalEvidenceRootArg -CreateEvidenceDirectories -Force")
        $commands.Add("powershell -NoProfile -ExecutionPolicy Bypass -File scripts\update_partition_manager_external_manifest_from_reports.ps1 -ManifestPath $externalManifestArg -EvidenceRoot $externalEvidenceRootArg -OutputPath $externalImportedManifestArg -RequireAllReports -Force")
        $commands.Add("powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_partition_manager_hardware_certification_strict.ps1 -CertificationRoot $strictCertificationRootArg -ExternalEvidenceManifest $externalImportedManifestArg -ExternalEvidenceChecklist $externalChecklistArg -ExternalEvidenceRoot $externalEvidenceRootArg")
    }

    return $commands.ToArray()
}

function Write-GapMarkdown {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# Partition Manager Certification Gap Report")
    $lines.Add("")
    $lines.Add("- Generated UTC: $($Report.generated_utc)")
    $lines.Add("- Claim level: $($Report.claim_level)")
    $lines.Add("- Status file: $($Report.status_path)")
    $lines.Add("- Certification matrix: $($Report.certification_matrix)")
    $lines.Add("")
    $lines.Add("## Summary")
    $lines.Add("")
    $lines.Add("| Area | Passed | Required | Incomplete |")
    $lines.Add("| --- | ---: | ---: | ---: |")
    $lines.Add("| Disposable VHD data-disk gates | $($Report.summary.vhd_data_disk.passed) | $($Report.summary.vhd_data_disk.required) | $($Report.summary.vhd_data_disk.incomplete) |")
    $lines.Add("| External VM/hardware/lab gates | $($Report.summary.external_gates.passed) | $($Report.summary.external_gates.required) | $($Report.summary.external_gates.incomplete) |")
    $lines.Add("")
    $lines.Add("## Next Commands")
    $lines.Add("")
    foreach ($command in @($Report.next_commands)) {
        $lines.Add("- ``$command``")
    }
    $lines.Add("")

    if (@($Report.gaps).Count -eq 0) {
        $lines.Add("## Gaps")
        $lines.Add("")
        $lines.Add("No certification gaps remain.")
        $lines.Add("")
    }
    else {
        foreach ($gap in @($Report.gaps)) {
            $lines.Add("## $($gap.id) - $($gap.name)")
            $lines.Add("")
            $lines.Add("- Category: $($gap.category)")
            $lines.Add("- Required action: $($gap.required_action)")
            $lines.Add("")
            $lines.Add("Safety contract:")
            foreach ($contract in @($gap.safety_contract)) {
                $lines.Add("- ``$contract``")
            }
            $lines.Add("")
            $lines.Add("Required evidence keys:")
            foreach ($key in @($gap.required_evidence_keys)) {
                $lines.Add("- ``$key``")
            }
            $lines.Add("")
        }
    }

    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

Push-Location $ProjectRoot
try {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }

    $resolvedStatusPath = Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $StatusPath) -ErrorAction Stop
    $status = Get-Content -LiteralPath $resolvedStatusPath.Path -Raw | ConvertFrom-Json
    if ($status.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification status schema_version: $($status.schema_version)"
    }

    $statusDirectory = Split-Path -Parent $resolvedStatusPath.Path
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $statusDirectory "certification-gap-report.json"
    }
    if ([string]::IsNullOrWhiteSpace($MarkdownPath)) {
        $MarkdownPath = Join-Path $statusDirectory "certification-gap-report.md"
    }

    $resolvedOutputPath = Resolve-ProjectPath -Path $OutputPath
    $resolvedMarkdownPath = Resolve-ProjectPath -Path $MarkdownPath
    Ensure-WriteTarget -Path $resolvedOutputPath -Kind "Certification gap report"
    Ensure-WriteTarget -Path $resolvedMarkdownPath -Kind "Certification gap Markdown report"

    $vhdGapIds = @($status.vhd_data_disk.incomplete_ids | ForEach-Object { $_.ToString() })
    $externalGapIds = @($status.external_gates.incomplete_ids | ForEach-Object { $_.ToString() })
    $gaps = @()
    foreach ($id in $vhdGapIds) {
        $gaps += New-GapEntry -Matrix $matrix -Id $id -Category "vhd_data_disk"
    }
    foreach ($id in $externalGapIds) {
        $gaps += New-GapEntry -Matrix $matrix -Id $id -Category "external_gate"
    }

    $nextCommands = @(New-NextCommandList -Status $status -StatusDirectory $statusDirectory -VhdGapIds $vhdGapIds -ExternalGapIds $externalGapIds)
    $report = [ordered]@{
        tool = "partition-manager-certification-gap-report"
        schema_version = 1
        generated_utc = (Get-Date).ToUniversalTime().ToString("o")
        claim_level = $status.claim_level
        status_path = $resolvedStatusPath.Path
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        source_report_path = $status.report.path
        external_manifest_path = $status.external_gates.manifest_path
        summary = [ordered]@{
            vhd_data_disk = [ordered]@{
                passed = [int]$status.vhd_data_disk.passed
                required = [int]$status.vhd_data_disk.required
                incomplete = $vhdGapIds.Count
            }
            external_gates = [ordered]@{
                passed = [int]$status.external_gates.passed
                required = [int]$status.external_gates.required
                incomplete = $externalGapIds.Count
            }
        }
        next_commands = $nextCommands
        gaps = @($gaps)
    }

    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8
    Write-GapMarkdown -Report ([pscustomobject]$report) -Path $resolvedMarkdownPath

    if (-not $Quiet) {
        Write-Host "Partition Manager certification gap report created: $resolvedOutputPath"
        Write-Host "Partition Manager certification gap Markdown created: $resolvedMarkdownPath"
    }
}
finally {
    Pop-Location
}
