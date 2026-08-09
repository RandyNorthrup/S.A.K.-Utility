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
    # $LASTEXITCODE alone is not a complete status contract for an in-process .ps1 call: a
    # checker that fails without an explicit `exit` leaves the pre-seeded 0 in place. Take
    # the last statement's success ($?) as well, so a failing step cannot pass silently.
    $stepSucceeded = $?
    $exitCode = $LASTEXITCODE
    if (-not $stepSucceeded -or $exitCode -ne 0) {
        throw "Partition Manager strict hardware certification step failed: $Name (exit code $exitCode, succeeded=$stepSucceeded)"
    }
}

# A generated artifact must be proven to come from THIS run: clear it first, then require
# the producing step to have recreated it. Otherwise a step that quietly produced nothing
# leaves a stale HardwareCertified status/bundle for the later checks to consume.
function Clear-GeneratedArtifact {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Path) {
        throw "Could not clear stale certification artifact before regenerating it: $Path"
    }
}

function Assert-GeneratedArtifact {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Partition Manager strict hardware certification step '$Name' did not produce $Path"
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
    # An empty/whitespace CertificationRoot would silently resolve to the project root and
    # widen the recursive report search to the whole repository. Refuse it.
    if ([string]::IsNullOrWhiteSpace($CertificationRoot)) {
        throw "CertificationRoot must name the strict certification directory; it cannot be empty."
    }
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
    else {
        Write-Warning ("External lab package check SKIPPED (-SkipExternalLabPackageCheck). " +
            "A mandatory strict gate did not run, so this run does NOT establish a complete " +
            "HardwareCertified claim.")
    }

    Invoke-CheckedStep -Name "strict hardware evidence verification" -Body {
        & scripts\verify_partition_manager_certification.ps1 `
            -ReportPath $resolvedReportPath `
            -ExternalEvidenceManifest $resolvedExternalManifest `
            -RequireVhdDataDiskEvidence `
            -RequireExternalGateEvidence
    }

    $statusPath = Join-Path $resolvedOutputRoot "hardware-certification-status.json"
    Clear-GeneratedArtifact -Path $statusPath
    Invoke-CheckedStep -Name "hardware certification status" -Body {
        & scripts\get_partition_manager_certification_status.ps1 `
            -ReportPath $resolvedReportPath `
            -ExternalEvidenceManifest $resolvedExternalManifest `
            -OutputPath $statusPath `
            -Quiet
    }

    Assert-GeneratedArtifact -Path $statusPath -Name "hardware certification status"

    $gapReportPath = Join-Path $resolvedOutputRoot "hardware-certification-gap-report.json"
    $gapMarkdownPath = Join-Path $resolvedOutputRoot "hardware-certification-gap-report.md"
    Clear-GeneratedArtifact -Path $gapReportPath
    Clear-GeneratedArtifact -Path $gapMarkdownPath
    Invoke-CheckedStep -Name "hardware certification gap report" -Body {
        & scripts\new_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -OutputPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath `
            -Force `
            -Quiet
    }
    Assert-GeneratedArtifact -Path $gapReportPath -Name "hardware certification gap report"
    Assert-GeneratedArtifact -Path $gapMarkdownPath -Name "hardware certification gap report"
    Invoke-CheckedStep -Name "hardware certification gap verification" -Body {
        & scripts\check_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath
    }

    $bundlePath = Join-Path $resolvedOutputRoot "hardware-certification-artifact-bundle.json"
    $preflightPath = Join-Path $resolvedCertificationRoot "vhd-preflight.json"
    Clear-GeneratedArtifact -Path $bundlePath
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
    Assert-GeneratedArtifact -Path $bundlePath -Name "hardware certification artifact bundle"
    Invoke-CheckedStep -Name "hardware certification artifact bundle verification" -Body {
        & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $bundlePath
    }

    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    # claim_level must be the exact STRING: a single-element array compares -ne to an empty
    # (falsy) result, so an array-valued claim_level would slip through the check below.
    if ($status.claim_level -isnot [string]) {
        throw "Strict hardware certification status has a non-string claim_level; refusing to claim HardwareCertified."
    }
    if ($status.claim_level -ne "HardwareCertified") {
        throw "Strict hardware certification did not reach HardwareCertified. Claim level: $($status.claim_level). External incomplete: $(@($status.external_gates.incomplete_ids).Count)."
    }
    # A HardwareCertified claim while gates are still listed incomplete is a contradiction;
    # the claim level alone must not be the whole acceptance test.
    $incompleteGateIds = @($status.external_gates.incomplete_ids)
    if ($incompleteGateIds.Count -ne 0) {
        throw "Strict hardware certification reports HardwareCertified with $($incompleteGateIds.Count) incomplete external gate(s): $($incompleteGateIds -join ', ')."
    }

    Write-Host "Partition Manager strict hardware certification passed: HardwareCertified"
    if ($SkipExternalLabPackageCheck) {
        Write-Warning "Claim is INCOMPLETE: the external lab package check was skipped for this run."
    }
    Write-Host "Report: $resolvedReportPath"
    Write-Host "Status: $statusPath"
    Write-Host "Bundle: $bundlePath"
}
finally {
    Pop-Location
}
