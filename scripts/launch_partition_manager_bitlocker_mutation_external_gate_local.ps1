<#
.SYNOPSIS
    Stages and launches the BitLocker mutation external gate inside the VM.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-bitlocker-mutation-gate",
    [int]$TargetDiskNumber = 2,
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-bitlocker-mutation-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\bitlocker-mutation-stage-debug"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-bitlocker-mutation-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_bitlocker_mutation_external_gate.ps1"

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
    Write-Host "Local staged BitLocker mutation evidence copied back."
    return
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidence -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_bitlocker_mutation_external_gate.ps1") -Destination $localRunner -Force

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
    throw "Elevated BitLocker mutation gate exited with code $($process.ExitCode). Evidence copied when available."
}

Write-Host "Local staged BitLocker mutation external gate completed."
