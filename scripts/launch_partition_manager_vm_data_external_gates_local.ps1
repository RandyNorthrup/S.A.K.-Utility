<#
.SYNOPSIS
    Stages and launches remaining VM data external gates inside SAK-PM-Lab-Win11.

.DESCRIPTION
    Run from the non-elevated VM desktop. Copies the runner to a local staging
    directory, runs it elevated, then copies report.json evidence back to the
    shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-vm-data-external-gates",
    [int]$RotationalDiskNumber = 1,
    [int]$UsbDiskNumber = -1,
    [int]$NvmeDiskNumber = -1,
    [string]$SsdMediaProof = "",
    [string[]]$GateIds = @(
        "external.usb-removable",
        "external.ssd-retrim",
        "external.ssd-secure-erase",
        "external.partition-move",
        "external.primary-logical-conversion",
        "external.volume-serial-number",
        "external.dynamic-to-basic",
        "external.hardware-wipe"
    ),
    [string]$GateIdsCsv = "",
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$rawGateIds = @()
if (-not [string]::IsNullOrWhiteSpace($GateIdsCsv)) {
    $rawGateIds = @($GateIdsCsv)
}
else {
    $rawGateIds = @($GateIds)
}
$GateIds = @(
    foreach ($gateId in $rawGateIds) {
        foreach ($part in ([string]$gateId -split ",")) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
                $trimmed
            }
        }
    }
)

$sharedEvidenceRoot = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-vm-data-gates-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\vm-data-gates-stage-debug"
$localEvidenceRoot = Join-Path $StageRoot "external-evidence"
$localGuestReport = Join-Path $StageRoot "external-vm-data-gates-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_vm_data_external_gates.ps1"

function Copy-StageEvidenceBack {
    New-Item -ItemType Directory -Path $sharedEvidenceRoot -Force | Out-Null
    foreach ($gateId in $GateIds) {
        $sharedGateDir = Join-Path $sharedEvidenceRoot $gateId
        $localGateDir = Join-Path $localEvidenceRoot $gateId
        New-Item -ItemType Directory -Path $sharedGateDir -Force | Out-Null
        foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
            $stalePath = Join-Path $sharedGateDir $staleReport
            if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
                Remove-Item -LiteralPath $stalePath -Force
            }
        }
        if (Test-Path -LiteralPath $localGateDir -PathType Container) {
            Copy-Item -Path (Join-Path $localGateDir "*") -Destination $sharedGateDir -Recurse -Force
        }
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
    Write-Host "Local staged VM data evidence copied back."
    return
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidenceRoot -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidenceRoot -Force | Out-Null

foreach ($gateId in $GateIds) {
    $localGateDir = Join-Path $localEvidenceRoot $gateId
    New-Item -ItemType Directory -Path $localGateDir -Force | Out-Null
    foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
        $stalePath = Join-Path $localGateDir $staleReport
        if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
            Remove-Item -LiteralPath $stalePath -Force
        }
    }
}

Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_vm_data_external_gates.ps1") -Destination $localRunner -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-ProjectRoot", "`"$SharedRoot`"",
    "-EvidenceRoot", "`"$localEvidenceRoot`"",
    "-GuestReportPath", "`"$localGuestReport`"",
    "-RotationalDiskNumber", $RotationalDiskNumber,
    "-Force"
)
if ($UsbDiskNumber -ge 0) {
    $argumentList += "-UsbDiskNumber"
    $argumentList += $UsbDiskNumber
}
if ($NvmeDiskNumber -ge 0) {
    $argumentList += "-NvmeDiskNumber"
    $argumentList += $NvmeDiskNumber
}
if (-not [string]::IsNullOrWhiteSpace($SsdMediaProof)) {
    $argumentList += "-SsdMediaProof"
    $argumentList += "`"$SsdMediaProof`""
}
if ($GateIds.Count -gt 0) {
    $argumentList += "-GateIdsCsv"
    $argumentList += "`"$($GateIds -join ",")`""
}

$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated VM data external gates exited with code $($process.ExitCode). Evidence copied when available."
}

Write-Host "Local staged VM data external gates completed."
