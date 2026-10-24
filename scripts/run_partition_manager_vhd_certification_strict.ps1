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
    [string]$ExternalEvidenceChecklist = "",
    [string]$ControllerCertifierPath = "build\Release\partition_manager_live_certifier.exe",
    [string]$ControllerSourceManifestPath = "build\Release\partition_manager_live_certifier.sources.txt"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "partition_manager_certification_report.ps1")
. (Join-Path $PSScriptRoot "partition_manager_certification_steps.ps1")
. (Join-Path $PSScriptRoot "process_capture.ps1")

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
        (ConvertTo-WindowsProcessArgument -Argument $PSCommandPath),
        "-OutputRoot",
        (ConvertTo-WindowsProcessArgument -Argument $OutputRoot),
        "-VhdSizeMB",
        $VhdSizeMB.ToString()
    )

    if ($KeepVhd) {
        $arguments += "-KeepVhd"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $arguments += "-ExternalEvidenceManifest"
        $arguments += (ConvertTo-WindowsProcessArgument -Argument $ExternalEvidenceManifest)
    }
    if (-not [string]::IsNullOrWhiteSpace($ExternalEvidenceChecklist)) {
        $arguments += "-ExternalEvidenceChecklist"
        $arguments += (ConvertTo-WindowsProcessArgument -Argument $ExternalEvidenceChecklist)
    }

    $arguments += @("-ControllerCertifierPath", (ConvertTo-WindowsProcessArgument $ControllerCertifierPath))
    $arguments += @("-ControllerSourceManifestPath", (ConvertTo-WindowsProcessArgument $ControllerSourceManifestPath))

    Write-Host "Relaunching strict Partition Manager VHD certification in an elevated PowerShell window..."
    $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    exit $process.ExitCode
}

Push-Location $ProjectRoot
try {
    if ($RelaunchElevated -and -not (Test-Administrator)) {
        Invoke-ElevatedRelaunch
    }

    $resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null
    $Script:StrictFailurePath = Join-Path $resolvedOutputRoot "strict-vhd-certification-error.txt"
    if (Test-Path -LiteralPath $Script:StrictFailurePath) {
        Remove-Item -LiteralPath $Script:StrictFailurePath -Force -ErrorAction Stop
    }

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
        Invoke-CheckedStep -Workflow "strict VHD" -Name "external evidence scaffold" -Body {
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

    Invoke-CheckedStep -Workflow "strict VHD" -Name "external checklist" -Body {
        & scripts\check_partition_manager_external_checklist.ps1 `
            -ChecklistPath $externalChecklistPath `
            -ManifestPath $externalManifestPath
    }
    if ([string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        Invoke-CheckedStep -Workflow "strict VHD" -Name "external lab package" -Body {
            & scripts\check_partition_manager_external_lab_package.ps1 `
                -EvidenceRoot $externalEvidenceRoot `
                -ManifestPath $externalManifestPath
        }
    }

    $preflightPath = Join-Path $resolvedOutputRoot "vhd-preflight.json"
    Invoke-GeneratedArtifactStep -Workflow "strict VHD" -Name "VHD preflight" -Paths @($preflightPath) -Body {
        & scripts\test_partition_manager_vhd_preflight.ps1 `
            -OutputRoot $resolvedOutputRoot `
            -OutputPath $preflightPath `
            -RequireAdministrator `
            -RequireReady `
            -Quiet
    }
    Invoke-CheckedStep -Workflow "strict VHD" -Name "VHD preflight report verification" -Body {
        & scripts\check_partition_manager_vhd_preflight_report.ps1 -ReportPath $preflightPath
    }

    $harnessArgs = @{
        OutputRoot = $resolvedOutputRoot
        RunVhdDataDiskMatrix = $true
        RequireVhdDataDiskEvidence = $true
        VhdSizeMB = $VhdSizeMB
        ControllerCertifierPath = $ControllerCertifierPath
    }
    if ($KeepVhd) {
        $harnessArgs.KeepVhd = $true
    }
    if ($RelaunchElevated) {
        $harnessArgs.RelaunchElevated = $true
    }

    Invoke-CheckedStep -Workflow "strict VHD" -Name "destructive disposable-VHD matrix" -Body {
        & scripts\run_partition_manager_destructive_certification.ps1 @harnessArgs
    }

    $reportPath = Resolve-LatestCertificationReport -Root $resolvedOutputRoot
    Invoke-CheckedStep -Workflow "strict VHD" -Name "strict VHD verification" -Body {
        & scripts\verify_partition_manager_certification.ps1 `
            -ControllerCertifierPath $ControllerCertifierPath `
            -ControllerSourceManifestPath $ControllerSourceManifestPath `
            -ReportPath $reportPath `
            -ExternalEvidenceManifest $externalManifestPath `
            -RequireVhdDataDiskEvidence
    }

    $statusPath = Join-Path $resolvedOutputRoot "certification-status.json"
    Invoke-GeneratedArtifactStep -Workflow "strict VHD" -Name "certification status" -Paths @($statusPath) -Body {
        & scripts\get_partition_manager_certification_status.ps1 `
            -ControllerCertifierPath $ControllerCertifierPath `
            -ControllerSourceManifestPath $ControllerSourceManifestPath `
            -ReportPath $reportPath `
            -ExternalEvidenceManifest $externalManifestPath `
            -OutputPath $statusPath `
            -Quiet
    }

    $gapReportPath = Join-Path $resolvedOutputRoot "certification-gap-report.json"
    $gapMarkdownPath = Join-Path $resolvedOutputRoot "certification-gap-report.md"
    Invoke-GeneratedArtifactStep -Workflow "strict VHD" -Name "certification gap report" -Paths @($gapReportPath, $gapMarkdownPath) -Body {
        & scripts\new_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -OutputPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath `
            -Force `
            -Quiet
    }
    Invoke-CheckedStep -Workflow "strict VHD" -Name "certification gap report verification" -Body {
        & scripts\check_partition_manager_certification_gap_report.ps1 `
            -StatusPath $statusPath `
            -GapReportPath $gapReportPath `
            -MarkdownPath $gapMarkdownPath
    }

    $bundlePath = Join-Path $resolvedOutputRoot "certification-artifact-bundle.json"
    Invoke-GeneratedArtifactStep -Workflow "strict VHD" -Name "certification artifact bundle" -Paths @($bundlePath) -Body {
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
    Invoke-CheckedStep -Workflow "strict VHD" -Name "certification artifact bundle verification" -Body {
        & scripts\check_partition_manager_certification_bundle.ps1 -BundlePath $bundlePath
    }

    $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    if ($status.claim_level -isnot [string]) {
        throw "Strict VHD certification status has a non-string claim_level; refusing certification."
    }
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
