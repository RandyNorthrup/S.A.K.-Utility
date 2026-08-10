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

# Fail closed on an -SsdMediaProof that contains a double quote: it is forwarded verbatim
# into the elevated argument list and an embedded quote could break the argument boundary
# and inject or alter child-script switches.
if ($SsdMediaProof -match '"') {
    throw "Invalid -SsdMediaProof: a double-quote character is not allowed."
}

$rawGateIds = @()
if (-not [string]::IsNullOrWhiteSpace($GateIdsCsv)) {
    $rawGateIds = @($GateIdsCsv)
}
else {
    $rawGateIds = @($GateIds)
}
# Validate every gate ID against a strict allowlist before it is ever used to build an
# evidence directory path (Join-Path) or forwarded to the elevated runner. A gate ID
# carrying a path separator, "..", wildcard, or quote could escape the evidence root or
# break the elevated argument list. Fail closed on any invalid segment; de-duplicate so a
# repeated ID cannot rerun a destructive gate or overwrite its evidence twice.
$seenGateIds = @{}
$GateIds = @(
    foreach ($gateId in $rawGateIds) {
        foreach ($part in ([string]$gateId -split ",")) {
            $trimmed = $part.Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed)) {
                continue
            }
            if ($trimmed -notmatch '^[A-Za-z0-9._-]+$' -or $trimmed -match '\.\.') {
                throw "Invalid external gate ID '$trimmed': only letters, digits, '.', '_' and '-' are allowed, and '..' is rejected."
            }
            if (-not $seenGateIds.ContainsKey($trimmed)) {
                $seenGateIds[$trimmed] = $true
                $trimmed
            }
        }
    }
)
if ($GateIds.Count -eq 0) {
    throw "No valid gate IDs resolved from -GateIds/-GateIdsCsv; refusing to launch. A blank or comma-only selector would otherwise let the elevated runner fall back to its full destructive default gate set."
}

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
        # Only touch the shared gate evidence when this run actually produced fresh local
        # evidence for the gate. Otherwise the stale-report deletion below would remove the
        # prior shared reports with no replacement to copy in -- destroying evidence.
        if (-not (Test-Path -LiteralPath $localGateDir -PathType Container)) {
            continue
        }
        New-Item -ItemType Directory -Path $sharedGateDir -Force | Out-Null
        foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
            $stalePath = Join-Path $sharedGateDir $staleReport
            if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
                Remove-Item -LiteralPath $stalePath -Force
            }
        }
        Copy-Item -Path (Join-Path $localGateDir "*") -Destination $sharedGateDir -Recurse -Force
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

# Pin the elevated launch to the System32 Windows PowerShell so a hijacked PATH cannot
# redirect the RunAs to an attacker-planted powershell.exe.
$systemPowerShell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $systemPowerShell -PathType Leaf)) {
    throw "System PowerShell not found at expected path: $systemPowerShell"
}
$process = Start-Process -FilePath $systemPowerShell -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated VM data external gates exited with code $($process.ExitCode). Evidence copied when available."
}

Write-Host "Local staged VM data external gates completed."
