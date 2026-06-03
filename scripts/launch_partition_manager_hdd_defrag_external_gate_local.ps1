<#
.SYNOPSIS
    Stages and launches the HDD defrag external gate inside the VM.

.DESCRIPTION
    Run this from the non-elevated SAK-PM-Lab-Win11 desktop. It copies the gate
    runner from the VirtualBox shared repo to local staging, runs it elevated,
    then copies the resulting evidence back into the shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-hdd-defrag-gate",
    [int]$TargetDiskNumber = 1,
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-hdd-defrag-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\hdd-defrag-stage-debug"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-hdd-defrag-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_hdd_defrag_external_gate.ps1"

function Copy-StageEvidenceBack {
    New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null
    foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
        $stalePath = Join-Path $sharedEvidence $staleReport
        if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
            Remove-Item -LiteralPath $stalePath -Force
        }
    }
    if (Test-Path -LiteralPath $localEvidence -PathType Container) {
        Copy-Item -Path (Join-Path $localEvidence "*") -Destination $sharedEvidence -Recurse -Force
    }
    if (Test-Path -LiteralPath $localGuestReport -PathType Leaf) {
        Copy-Item -LiteralPath $localGuestReport -Destination $sharedGuestReport -Force
    }
    if (Test-Path -LiteralPath $StageRoot -PathType Container) {
        New-Item -ItemType Directory -Path $sharedStageDebug -Force | Out-Null
        Copy-Item -Path (Join-Path $StageRoot "*") -Destination $sharedStageDebug -Recurse -Force
    }
}

if ($CopyBackOnly) {
    Copy-StageEvidenceBack
    Write-Host "Local staged HDD defrag evidence copied back."
    return
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidence -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null

foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
    $stalePath = Join-Path $localEvidence $staleReport
    if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
        Remove-Item -LiteralPath $stalePath -Force
    }
}

Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_hdd_defrag_external_gate.ps1") -Destination $localRunner -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-TargetDiskNumber", $TargetDiskNumber,
    "-Force",
    "-EvidenceDir", "`"$localEvidence`"",
    "-GuestReportPath", "`"$localGuestReport`""
)

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated HDD defrag gate exited with code $($process.ExitCode). Evidence copied when available."
}

Write-Host "Local staged HDD defrag external gate completed."
