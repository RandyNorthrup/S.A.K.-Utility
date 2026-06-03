<#
.SYNOPSIS
    Runs the strict Partition Manager hardware-certification evidence verifier.

.DESCRIPTION
    Verifies that existing strict disposable-VHD evidence and external
    VM/hardware/lab evidence are complete enough to claim HardwareCertified.
    This script does not mutate disks. It fails unless all 12 VHD scenarios and
    all 18 external gates pass with matrix-complete evidence and artifact links.
#>

[CmdletBinding()]
param(
    [string]$CertificationRoot = "artifacts\partition-manager-certification\vhd-strict",
    [string]$ReportPath = "",
    [string]$ExternalEvidenceManifest = "",
    [string]$ExternalEvidenceChecklist = "",
    [string]$ExternalEvidenceRoot = "",
    [string]$OutputRoot = "",
    [switch]$SkipExternalLabPackageCheck
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

    return Join-Path $ProjectRoot $Path
}

function Invoke-CheckedStep {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $global:LASTEXITCODE = 0
    & $Body
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Partition Manager strict hardware certification step failed: $Name"
    }
}

function Resolve-LatestCertificationReport {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $resolvedRoot = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    $reports = @(Get-ChildItem -LiteralPath $resolvedRoot.Path -Recurse -Filter "partition-manager-certification-report.json" |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($reports.Count -eq 0) {
        throw "No Partition Manager certification reports found under $($resolvedRoot.Path)"
    }
    return $reports[0].FullName
}

Push-Location $ProjectRoot
try {
    $resolvedCertificationRoot = Resolve-ProjectPath -Path $CertificationRoot
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $OutputRoot = $resolvedCertificationRoot
    }
    $resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null

    $resolvedReportPath = if ([string]::IsNullOrWhiteSpace($ReportPath)) {
        Resolve-LatestCertificationReport -Root $resolvedCertificationRoot
    }
    else {
        (Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $ReportPath) -ErrorAction Stop).Path
    }

    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $candidateManifest = Join-Path $resolvedCertificationRoot "external-evidence.json"
        if (-not (Test-Path -LiteralPath $candidateManifest -PathType Leaf)) {
            throw "External evidence manifest required for HardwareCertified verification. Pass -ExternalEvidenceManifest with a completed manifest."
        }
        $ExternalEvidenceManifest = $candidateManifest
    }
    $externalManifestCheckPath = $ExternalEvidenceManifest
    $resolvedExternalManifest = (Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $ExternalEvidenceManifest) -ErrorAction Stop).Path

    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceChecklist)) {
        $manifestDirectory = Split-Path -Parent $resolvedExternalManifest
        $manifestName = [System.IO.Path]::GetFileNameWithoutExtension($resolvedExternalManifest)
        $candidateChecklist = Join-Path $manifestDirectory "$manifestName.checklist.md"
        if (-not (Test-Path -LiteralPath $candidateChecklist -PathType Leaf)) {
            $candidateChecklist = Join-Path $manifestDirectory "external-evidence.checklist.md"
        }
        $ExternalEvidenceChecklist = $candidateChecklist
    }
    $externalChecklistCheckPath = $ExternalEvidenceChecklist
    $resolvedExternalChecklist = (Resolve-Path -LiteralPath (Resolve-ProjectPath -Path $ExternalEvidenceChecklist) -ErrorAction Stop).Path

    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceRoot)) {
        $ExternalEvidenceRoot = Join-Path (Split-Path -Parent $resolvedExternalManifest) "external-evidence"
    }

    Invoke-CheckedStep -Name "external checklist" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 `
            -ChecklistPath $externalChecklistCheckPath `
            -ManifestPath $externalManifestCheckPath
    }

    if (-not $SkipExternalLabPackageCheck) {
        Invoke-CheckedStep -Name "external lab package" -Body {
            & scripts\check_partition_manager_external_lab_package.ps1 `
                -EvidenceRoot $ExternalEvidenceRoot `
                -ManifestPath $externalManifestCheckPath
        }
    }

    Invoke-CheckedStep -Name "strict hardware evidence verification" -Body {
        & scripts\verify_partition_manager_certification.ps1 `
            -ReportPath $resolvedReportPath `
            -ExternalEvidenceManifest $resolvedExternalManifest `
            -RequireVhdDataDiskEvidence `
            -RequireExternalGateEvidence
    }

    $statusPath = Join-Path $resolvedOutputRoot "hardware-certification-status.json"
    Invoke-CheckedStep -Name "hardware certification status" -Body {
        & scripts\get_partition_manager_certification_status.ps1 `
            -ReportPath $resolvedReportPath `
            -ExternalEvidenceManifest $resolvedExternalManifest `
            -OutputPath $statusPath `
            -Quiet
    }

    $gapReportPath = Join-Path $resolvedOutputRoot "hardware-certification-gap-report.json"
    $gapMarkdownPath = Join-Path $resolvedOutputRoot "hardware-certification-gap-report.md"
    Invoke-CheckedStep -Name "hardware certification gap report" -Body {
        & scripts\new_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -OutputPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath `
            -Force `
            -Quiet
    }
    Invoke-CheckedStep -Name "hardware certification gap verification" -Body {
        & scripts\check_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath
    }

    $bundlePath = Join-Path $resolvedOutputRoot "hardware-certification-artifact-bundle.json"
    $preflightPath = Join-Path $resolvedCertificationRoot "vhd-preflight.json"
    Invoke-CheckedStep -Name "hardware certification artifact bundle" -Body {
        & scripts\new_partition_manager_certification_bundle.ps1 `
            -CertificationRoot $resolvedCertificationRoot `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -GapMarkdownPath $gapMarkdownPath `
            -VhdPreflightPath $preflightPath `
            -ExternalEvidenceManifest $resolvedExternalManifest `
            -ExternalEvidenceChecklist $resolvedExternalChecklist `
            -OutputPath $bundlePath `
            -Force `
            -Quiet
    }
    Invoke-CheckedStep -Name "hardware certification artifact bundle verification" -Body {
        & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $bundlePath
    }

    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    if ($status.claim_level -ne "HardwareCertified") {
        throw "Strict hardware certification did not reach HardwareCertified. Claim level: $($status.claim_level). External incomplete: $(@($status.external_gates.incomplete_ids).Count)."
    }

    Write-Host "Partition Manager strict hardware certification passed: HardwareCertified"
    Write-Host "Report: $resolvedReportPath"
    Write-Host "Status: $statusPath"
    Write-Host "Bundle: $bundlePath"
}
finally {
    Pop-Location
}
