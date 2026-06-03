<#
.SYNOPSIS
    Runs the strict Partition Manager disposable-VHD certification workflow.

.DESCRIPTION
    Orchestrates the required elevated VHD evidence flow end to end: read-only
    preflight, destructive disposable-VHD matrix, strict verification, claim
    status, gap report, artifact bundle, and final VhdDataDiskCertified check.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification\vhd-strict",
    [int]$VhdSizeMB = 768,
    [switch]$KeepVhd,
    [switch]$RelaunchElevated,
    [string]$ExternalEvidenceManifest = "",
    [string]$ExternalEvidenceChecklist = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Script:StrictFailurePath = ""

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

function ConvertTo-ProcessArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }

    return $Value
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-ElevatedRelaunch {
    $hostPath = (Get-Process -Id $PID).Path
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (ConvertTo-ProcessArgument -Value $PSCommandPath),
        "-OutputRoot",
        (ConvertTo-ProcessArgument -Value $OutputRoot),
        "-VhdSizeMB",
        $VhdSizeMB.ToString()
    )

    if ($KeepVhd) {
        $arguments += "-KeepVhd"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $arguments += "-ExternalEvidenceManifest"
        $arguments += (ConvertTo-ProcessArgument -Value $ExternalEvidenceManifest)
    }
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceChecklist)) {
        $arguments += "-ExternalEvidenceChecklist"
        $arguments += (ConvertTo-ProcessArgument -Value $ExternalEvidenceChecklist)
    }

    Write-Host "Relaunching strict Partition Manager VHD certification in an elevated PowerShell window..."
    $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
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
        throw "Partition Manager strict VHD certification step failed: $Name"
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
    if ($RelaunchElevated -and -not (Test-Administrator)) {
        Invoke-ElevatedRelaunch
    }

    $resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null
    $Script:StrictFailurePath = Join-Path $resolvedOutputRoot "strict-vhd-certification-error.txt"
    Remove-Item -LiteralPath $Script:StrictFailurePath -Force -ErrorAction SilentlyContinue

    $externalManifestPath = if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        Join-Path $resolvedOutputRoot "external-evidence.template.json"
    }
    else {
        Resolve-ProjectPath -Path $ExternalEvidenceManifest
    }
    $externalChecklistPath = if ([string]::IsNullOrWhiteSpace($ExternalEvidenceChecklist)) {
        Join-Path $resolvedOutputRoot "external-evidence.checklist.md"
    }
    else {
        Resolve-ProjectPath -Path $ExternalEvidenceChecklist
    }
    $externalEvidenceRoot = Join-Path $resolvedOutputRoot "external-evidence"

    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        Invoke-CheckedStep -Name "external evidence scaffold" -Body {
            & scripts\new_partition_manager_external_evidence_manifest.ps1 `
                -OutputPath $externalManifestPath `
                -ChecklistPath $externalChecklistPath `
                -EvidenceRoot $externalEvidenceRoot `
                -CreateEvidenceDirectories `
                -Force
        }
    }
    else {
        if (-not (Test-Path -LiteralPath $externalManifestPath -PathType Leaf)) {
            throw "External evidence manifest missing: $externalManifestPath"
        }
        if (-not (Test-Path -LiteralPath $externalChecklistPath -PathType Leaf)) {
            throw "External evidence checklist missing: $externalChecklistPath"
        }
    }

    Invoke-CheckedStep -Name "external checklist" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 `
            -ChecklistPath $externalChecklistPath `
            -ManifestPath $externalManifestPath
    }
    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        Invoke-CheckedStep -Name "external lab package" -Body {
            & scripts\check_partition_manager_external_lab_package.ps1 `
                -EvidenceRoot $externalEvidenceRoot `
                -ManifestPath $externalManifestPath
        }
    }

    $preflightPath = Join-Path $resolvedOutputRoot "vhd-preflight.json"
    Invoke-CheckedStep -Name "VHD preflight" -Body {
        & scripts\test_partition_manager_vhd_preflight.ps1 `
            -OutputRoot $resolvedOutputRoot `
            -OutputPath $preflightPath `
            -RequireAdministrator `
            -RequireReady `
            -Quiet
    }
    Invoke-CheckedStep -Name "VHD preflight report verification" -Body {
        & scripts\check_partition_manager_vhd_preflight_report.ps1 -ReportPath $preflightPath
    }

    $harnessArgs = @{
        OutputRoot = $resolvedOutputRoot
        RunVhdDataDiskMatrix = $true
        RequireVhdDataDiskEvidence = $true
        VhdSizeMB = $VhdSizeMB
    }
    if ($KeepVhd) {
        $harnessArgs.KeepVhd = $true
    }
    if ($RelaunchElevated) {
        $harnessArgs.RelaunchElevated = $true
    }

    Invoke-CheckedStep -Name "destructive disposable-VHD matrix" -Body {
        & scripts\run_partition_manager_destructive_certification.ps1 @harnessArgs
    }

    $reportPath = Resolve-LatestCertificationReport -Root $resolvedOutputRoot
    Invoke-CheckedStep -Name "strict VHD verification" -Body {
        & scripts\verify_partition_manager_certification.ps1 `
            -ReportPath $reportPath `
            -ExternalEvidenceManifest $externalManifestPath `
            -RequireVhdDataDiskEvidence
    }

    $statusPath = Join-Path $resolvedOutputRoot "certification-status.json"
    Invoke-CheckedStep -Name "certification status" -Body {
        & scripts\get_partition_manager_certification_status.ps1 `
            -ReportPath $reportPath `
            -ExternalEvidenceManifest $externalManifestPath `
            -OutputPath $statusPath `
            -Quiet
    }

    $gapReportPath = Join-Path $resolvedOutputRoot "certification-gap-report.json"
    $gapMarkdownPath = Join-Path $resolvedOutputRoot "certification-gap-report.md"
    Invoke-CheckedStep -Name "certification gap report" -Body {
        & scripts\new_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -OutputPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath `
            -Force `
            -Quiet
    }
    Invoke-CheckedStep -Name "certification gap report verification" -Body {
        & scripts\check_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath
    }

    $bundlePath = Join-Path $resolvedOutputRoot "certification-artifact-bundle.json"
    Invoke-CheckedStep -Name "certification artifact bundle" -Body {
        & scripts\new_partition_manager_certification_bundle.ps1 `
            -CertificationRoot $resolvedOutputRoot `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -GapMarkdownPath $gapMarkdownPath `
            -VhdPreflightPath $preflightPath `
            -ExternalEvidenceManifest $externalManifestPath `
            -ExternalEvidenceChecklist $externalChecklistPath `
            -OutputPath $bundlePath `
            -Force `
            -Quiet
    }
    Invoke-CheckedStep -Name "certification artifact bundle verification" -Body {
        & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $bundlePath
    }

    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    if ($status.claim_level -ne "VhdDataDiskCertified" -and $status.claim_level -ne "HardwareCertified") {
        throw "Strict VHD workflow did not reach VhdDataDiskCertified. Claim level: $($status.claim_level)"
    }

    Write-Host "Partition Manager strict VHD certification passed: $($status.claim_level)"
    Write-Host "Report: $reportPath"
    Write-Host "Bundle: $bundlePath"
}
catch {
    $failurePath = $Script:StrictFailurePath
    if ([string]::IsNullOrWhiteSpace($failurePath)) {
        $failureRoot = Resolve-ProjectPath -Path $OutputRoot
        New-Item -ItemType Directory -Path $failureRoot -Force | Out-Null
        $failurePath = Join-Path $failureRoot "strict-vhd-certification-error.txt"
    }

    $lines = @(
        "Strict Partition Manager VHD certification failed.",
        "Generated UTC: $((Get-Date).ToUniversalTime().ToString("o"))",
        "OutputRoot: $OutputRoot",
        "Administrator: $(Test-Administrator)",
        "",
        "Exception:",
        $_.Exception.ToString(),
        "",
        "Script stack:",
        $_.ScriptStackTrace
    )
    $lines | Set-Content -LiteralPath $failurePath -Encoding UTF8
    Write-Host "Strict Partition Manager VHD certification error written: $failurePath"
    throw
}
finally {
    Pop-Location
}
