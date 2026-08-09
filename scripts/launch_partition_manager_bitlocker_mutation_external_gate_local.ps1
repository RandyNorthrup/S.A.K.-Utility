<#
.SYNOPSIS
    Stages and launches the BitLocker mutation external gate inside the VM.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-bitlocker-mutation-gate",
    # No default: the non-CopyBackOnly path wipes this physical disk, and a defaulted "2"
    # meant a no-argument run targeted whatever was disk 2. Required below unless CopyBackOnly.
    [ValidateRange(0, 999)] [int]$TargetDiskNumber = 0,
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

function Assert-QualifiedPath {
    param(
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name must be a non-empty path."
    }
    if ($Path -match '["*?\[\]]') {
        throw "$Name contains characters that cannot be quoted or staged safely: $Path"
    }
    if ($Path -notmatch '^([A-Za-z]:[\\/]|\\\\[^\\])') {
        throw "$Name must be a fully qualified local or UNC path: $Path"
    }
}

function Assert-NoReparsePoint {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq [System.IO.FileAttributes]::ReparsePoint) {
        throw "$Name is a reparse point and cannot be trusted for elevated staging: $Path"
    }
}

function Get-TrustedPowerShellPath {
    if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        throw "SystemRoot is not set; refusing to elevate an unqualified powershell.exe."
    }
    $trusted = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $trusted -PathType Leaf)) {
        throw "Trusted Windows PowerShell not found: $trusted"
    }
    return $trusted
}

function Assert-DisposableTargetDisk {
    param([Parameter(Mandatory = $true)] [int]$Number)

    $disk = Get-Disk -Number $Number
    if ($null -eq $disk) {
        throw "Target disk $Number was not found; refusing to launch a destructive gate."
    }
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Target disk $Number is the boot/system disk; refusing to launch a destructive gate."
    }
    Write-Host ("Target disk {0} pinned: friendly='{1}' serial='{2}' bytes={3}" -f $Number, $disk.FriendlyName, $disk.SerialNumber, $disk.Size)
}

function Assert-PassedReport {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name was not produced: $Path"
    }
    $text = Get-Content -LiteralPath $Path -Raw
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "$Name is empty: $Path"
    }
    $report = $text | ConvertFrom-Json
    if ($report.status -ne "Passed") {
        throw "$Name did not report a passing status ('$($report.status)'): $Path"
    }
}

Assert-QualifiedPath -Path $SharedRoot -Name "SharedRoot"
Assert-QualifiedPath -Path $StageRoot -Name "StageRoot"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-bitlocker-mutation-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\bitlocker-mutation-stage-debug"
$sharedReport = Join-Path $sharedEvidence "report.json"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-bitlocker-mutation-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_bitlocker_mutation_external_gate.ps1"

function Copy-StageEvidenceBack {
    $haveEvidence = Test-Path -LiteralPath $localEvidence -PathType Container
    $haveGuestReport = Test-Path -LiteralPath $localGuestReport -PathType Leaf
    if (-not $haveEvidence -and -not $haveGuestReport) {
        Write-Host "No staged BitLocker mutation evidence under $StageRoot; shared evidence left untouched."
        return $false
    }

    New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null
    foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
        $stalePath = Join-Path $sharedEvidence $staleReport
        if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
            Remove-Item -LiteralPath $stalePath -Force
        }
    }
    if ($haveEvidence) {
        Copy-Item -Path (Join-Path $localEvidence "*") -Destination $sharedEvidence -Recurse -Force
    }
    if ($haveGuestReport) {
        Copy-Item -LiteralPath $localGuestReport -Destination $sharedGuestReport -Force
    }
    if (Test-Path -LiteralPath $StageRoot -PathType Container) {
        New-Item -ItemType Directory -Path $sharedStageDebug -Force | Out-Null
        Copy-Item -Path (Join-Path $StageRoot "*") -Destination $sharedStageDebug -Recurse -Force
    }
    return $true
}

if ($CopyBackOnly) {
    if (-not (Copy-StageEvidenceBack)) {
        throw "No staged BitLocker mutation evidence found under $StageRoot; nothing was copied back."
    }
    Assert-PassedReport -Path $sharedReport -Name "BitLocker mutation terminal report"
    Assert-PassedReport -Path $sharedGuestReport -Name "BitLocker mutation guest report"
    Write-Host "Local staged BitLocker mutation evidence copied back."
    return
}

if ($TargetDiskNumber -lt 1) {
    throw "TargetDiskNumber is required for the mutation run and must be >= 1. This gate destroys every partition on that physical disk; run Get-Disk and pass the disposable disk's number explicitly. (Use -CopyBackOnly to retrieve evidence without a disk.)"
}
Assert-DisposableTargetDisk -Number $TargetDiskNumber
Assert-NoReparsePoint -Path $StageRoot -Name "StageRoot"

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $localEvidence -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null

Assert-NoReparsePoint -Path $localEvidence -Name "Staged evidence directory"
Assert-NoReparsePoint -Path $localRunner -Name "Staged runner"

Copy-Item -LiteralPath (Join-Path $SharedRoot "scripts\run_partition_manager_bitlocker_mutation_external_gate.ps1") -Destination $localRunner -Force
if (-not (Test-Path -LiteralPath $localRunner -PathType Leaf)) {
    throw "Runner was not staged: $localRunner"
}
Assert-NoReparsePoint -Path $localRunner -Name "Staged runner"

$trustedPowerShell = Get-TrustedPowerShellPath

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-TargetDiskNumber", $TargetDiskNumber,
    "-Force",
    "-EvidenceDir", "`"$localEvidence`"",
    "-GuestReportPath", "`"$localGuestReport`""
)

$process = Start-Process -FilePath $trustedPowerShell -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
$published = Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated BitLocker mutation gate exited with code $($process.ExitCode). Evidence copied when available."
}
if (-not $published) {
    throw "Elevated BitLocker mutation gate exited 0 but staged no evidence under $StageRoot."
}

Assert-PassedReport -Path $sharedReport -Name "BitLocker mutation terminal report"
Assert-PassedReport -Path $sharedGuestReport -Name "BitLocker mutation guest report"

Write-Host "Local staged BitLocker mutation external gate completed."
