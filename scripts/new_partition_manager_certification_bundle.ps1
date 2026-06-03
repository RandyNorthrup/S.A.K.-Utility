<#
.SYNOPSIS
    Creates a Partition Manager certification artifact bundle manifest.

.DESCRIPTION
    Links the current certification status, harness report, VHD preflight,
    gap report, external evidence manifest, and lab checklist with SHA-256
    hashes so release readiness can prove those artifacts belong together.
#>

[CmdletBinding()]
param(
    [string]$CertificationRoot = "artifacts\partition-manager-certification\readiness",
    [string]$StatusPath = "",
    [string]$GapReportPath = "",
    [string]$GapMarkdownPath = "",
    [string]$VhdPreflightPath = "",
    [string]$ExternalEvidenceManifest = "",
    [string]$ExternalEvidenceChecklist = "",
    [string]$OutputPath = "",
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

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return Get-Content -LiteralPath $resolved.Path -Raw | ConvertFrom-Json
}

function New-ArtifactEntry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Kind,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    $item = Get-Item -LiteralPath $resolved.Path -ErrorAction Stop
    [ordered]@{
        kind = $Kind
        path = $resolved.Path
        sha256 = (Get-FileHash -LiteralPath $resolved.Path -Algorithm SHA256).Hash
        bytes = [int64]$item.Length
        last_write_utc = $item.LastWriteTimeUtc.ToString("o")
    }
}

Push-Location $ProjectRoot
try {
    $resolvedRoot = Resolve-ProjectPath -Path $CertificationRoot
    if ([string]::IsNullOrWhiteSpace($StatusPath)) {
        $StatusPath = Join-Path $resolvedRoot "certification-status.json"
    }
    if ([string]::IsNullOrWhiteSpace($GapReportPath)) {
        $GapReportPath = Join-Path $resolvedRoot "certification-gap-report.json"
    }
    if ([string]::IsNullOrWhiteSpace($GapMarkdownPath)) {
        $GapMarkdownPath = Join-Path $resolvedRoot "certification-gap-report.md"
    }
    if ([string]::IsNullOrWhiteSpace($VhdPreflightPath)) {
        $VhdPreflightPath = Join-Path $resolvedRoot "vhd-preflight.json"
    }
    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceChecklist)) {
        $ExternalEvidenceChecklist = Join-Path $resolvedRoot "external-evidence.checklist.md"
    }
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $resolvedRoot "certification-artifact-bundle.json"
    }

    $resolvedStatusPath = (Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $StatusPath) -ErrorAction Stop).Path
    $status = Read-JsonFile -Path $resolvedStatusPath
    $gap = Read-JsonFile -Path (Resolve-ProjectPath -Path $GapReportPath)
    $preflight = Read-JsonFile -Path (Resolve-ProjectPath -Path $VhdPreflightPath)
    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $ExternalEvidenceManifest = $status.external_gates.manifest_path
    }

    $artifacts = @(
        New-ArtifactEntry -Kind "certification_status" -Path $resolvedStatusPath
        New-ArtifactEntry -Kind "certification_report" -Path $status.report.path
        New-ArtifactEntry -Kind "certification_gap_report_json" -Path (Resolve-ProjectPath -Path $GapReportPath)
        New-ArtifactEntry -Kind "certification_gap_report_markdown" -Path (Resolve-ProjectPath -Path $GapMarkdownPath)
        New-ArtifactEntry -Kind "vhd_preflight" -Path (Resolve-ProjectPath -Path $VhdPreflightPath)
        New-ArtifactEntry -Kind "external_evidence_manifest" -Path (Resolve-ProjectPath -Path $ExternalEvidenceManifest)
        New-ArtifactEntry -Kind "external_evidence_checklist" -Path (Resolve-ProjectPath -Path $ExternalEvidenceChecklist)
    )

    $bundle = [ordered]@{
        tool = "partition-manager-certification-artifact-bundle"
        schema_version = 1
        generated_utc = (Get-Date).ToUniversalTime().ToString("o")
        project_root = $ProjectRoot
        certification_root = $resolvedRoot
        claim_level = $status.claim_level
        status_path = $resolvedStatusPath
        source_report_path = $status.report.path
        external_manifest_path = $status.external_gates.manifest_path
        summary = [ordered]@{
            vhd_data_disk = [ordered]@{
                passed = [int]$status.vhd_data_disk.passed
                required = [int]$status.vhd_data_disk.required
                incomplete = @($status.vhd_data_disk.incomplete_ids).Count
            }
            external_gates = [ordered]@{
                passed = [int]$status.external_gates.passed
                required = [int]$status.external_gates.required
                incomplete = @($status.external_gates.incomplete_ids).Count
            }
            vhd_preflight = [ordered]@{
                ready = [bool]$preflight.ready_for_vhd_certification
                administrator = [bool]$preflight.administrator
                blockers = @($preflight.blockers)
            }
            gap_report = [ordered]@{
                gaps = @($gap.gaps).Count
                claim_level = $gap.claim_level
            }
        }
        artifacts = $artifacts
    }

    $resolvedOutputPath = Resolve-ProjectPath -Path $OutputPath
    if ((Test-Path -LiteralPath $resolvedOutputPath) -and -not $Force) {
        throw "Certification artifact bundle already exists: $resolvedOutputPath. Use -Force to overwrite."
    }
    $parent = Split-Path -Parent $resolvedOutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $bundle | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resolvedOutputPath -Encoding UTF8

    if (-not $Quiet) {
        Write-Host "Partition Manager certification artifact bundle created: $resolvedOutputPath"
    }
}
finally {
    Pop-Location
}
