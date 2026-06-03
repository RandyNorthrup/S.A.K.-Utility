<#
.SYNOPSIS
    Stages and launches the file-recovery external gate from inside the VM.

.DESCRIPTION
    Run this from the non-elevated VM desktop. It copies the gate runner,
    certifier, and runtime DLLs from the VirtualBox shared repo to a local
    staging directory, runs the VM-side gate script elevated, then copies the
    resulting evidence back into the shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-file-recovery-gate",
    [int]$TargetDiskNumber = 1,
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-file-recovery-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\file-recovery-stage-debug"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-file-recovery-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_file_recovery_external_gate.ps1"
$localCertifier = Join-Path $StageRoot "partition_file_recovery_certifier.exe"

function Copy-StageEvidenceBack {
    New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null
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
    Write-Host "Local staged file-recovery evidence copied back."
    return
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidence -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_file_recovery_external_gate.ps1") -Destination $localRunner -Force

$runtimeFiles = @(
    "partition_file_recovery_certifier.exe",
    "Qt6Core.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "concrt140.dll"
)

foreach ($runtimeFile in $runtimeFiles) {
    $source = Join-Path $SharedRoot "build\Release\$runtimeFile"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Missing shared runtime file: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $StageRoot $runtimeFile) -Force
}

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-TargetDiskNumber", $TargetDiskNumber,
    "-Force",
    "-CertifierPath", "`"$localCertifier`"",
    "-EvidenceDir", "`"$localEvidence`"",
    "-GuestReportPath", "`"$localGuestReport`""
)

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated file-recovery gate exited with code $($process.ExitCode). Evidence copied when available."
}

Write-Host "Local staged file-recovery external gate completed."
