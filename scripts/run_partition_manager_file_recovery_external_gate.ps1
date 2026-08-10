<#
.SYNOPSIS
    Runs the external file-level Data Recovery gate inside the disposable VM.

.DESCRIPTION
    This script is intended to run from an elevated PowerShell session inside
    SAK-PM-Lab-Win11. It prepares a disposable VirtualBox data disk, runs the
    partition_file_recovery_certifier helper against the raw volume, writes the
    matrix-backed external evidence report, and clears the disk back to RAW.
#>

[CmdletBinding()]
param(
    # No default: this gate wipes a whole physical disk back to RAW, and a defaulted "1"
    # meant a no-argument run destroyed whatever happened to be disk 1. The disk must be
    # named explicitly on every invocation.
    [int]$TargetDiskNumber = 0,
    [string]$CertifierPath = "\\vboxsvr\sakrepo\build\Release\partition_file_recovery_certifier.exe",
    [string]$EvidenceDir = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery",
    [string]$GuestReportPath = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-file-recovery-guest-report.json",
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if ($TargetDiskNumber -lt 1) {
    throw "TargetDiskNumber is required and must be >= 1. This gate destroys every partition on that physical disk; run Get-Disk and pass the disposable disk's number explicitly rather than relying on a default."
}

$Script:TranscriptStarted = $false
try {
    $guestReportParent = Split-Path -Parent $GuestReportPath
    if (-not [string]::IsNullOrWhiteSpace($guestReportParent)) {
        New-Item -ItemType Directory -Path $guestReportParent -Force | Out-Null
        Start-Transcript -Path (Join-Path $guestReportParent "run_partition_manager_file_recovery_external_gate.log") -Force | Out-Null
        $Script:TranscriptStarted = $true
    }
}
catch {
    $Script:TranscriptStarted = $false
}

trap {
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
    }
    throw $_
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-PlainText {
    param([object[]]$Value)

    return (($Value | ForEach-Object { $_.ToString() }) -join "`n").Trim()
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $exitCode = $null
    $output = $null
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        # A launch failure (missing/invalid executable) leaves $LASTEXITCODE holding a
        # prior command's value; report it as a failure so a stale 0 cannot look like success.
        $output = $_.Exception.Message
        $exitCode = -1
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($null -eq $exitCode) {
        $exitCode = -1
    }
    [pscustomobject]@{
        file_path = $FilePath
        arguments = $Arguments
        exit_code = $exitCode
        output = ConvertTo-PlainText -Value $output
    }
}

function Assert-DisposableDisk {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Target disk $($Disk.Number) is boot/system disk."
    }
    if ($Disk.FriendlyName -notlike "VBOX*") {
        throw "Target disk $($Disk.Number) is not a VBOX disposable disk: $($Disk.FriendlyName)"
    }
    if ($Disk.Size -lt 3GB -or $Disk.Size -gt 5GB) {
        throw "Target disk $($Disk.Number) size is outside disposable 4 GB lab range: $($Disk.Size)"
    }
}

function Get-DiskSnapshot {
    param([int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{
            partition_number = $_.PartitionNumber
            drive_letter = $_.DriveLetter
            offset_bytes = $_.Offset
            size_bytes = $_.Size
            type = $_.Type
        }
    })
    [pscustomobject]@{
        number = $disk.Number
        friendly_name = $disk.FriendlyName
        size_bytes = $disk.Size
        partition_style = $disk.PartitionStyle.ToString()
        is_boot = $disk.IsBoot
        is_system = $disk.IsSystem
        partitions = $partitions
    }
}

function Clear-DisposableDisk {
    param([int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber
    # Re-validate before every wipe: cleanup runs from a finally block that is also
    # reached after Assert-DisposableDisk rejects a disk, so an unchecked wipe here
    # would destroy the very boot/system/non-VBOX/oversized disk validation refused.
    Assert-DisposableDisk -Disk $disk
    if ($disk.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $DiskNumber -IsOffline $false
    }
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return
    }
    Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false
}

function New-RecoveryVolume {
    param([int]$DiskNumber)

    Clear-DisposableDisk -DiskNumber $DiskNumber
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    $partition = New-Partition -DiskNumber $DiskNumber -Size 512MB -AssignDriveLetter
    $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel "SAKREC" -Confirm:$false -Force
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber
    $letter = $partition.DriveLetter
    if ([string]::IsNullOrWhiteSpace($letter)) {
        $letter = $volume.DriveLetter
    }
    if ([string]::IsNullOrWhiteSpace($letter)) {
        throw "Disposable recovery volume did not receive a drive letter."
    }
    $volume = Get-Volume -DriveLetter $letter
    $accessPaths = @($partition.AccessPaths)
    $volumeId = @($accessPaths | Where-Object { $_ -like "\\?\Volume{*}\" } | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($volumeId)) {
        $volumeId = "\\.\$letter`:"
    }
    [pscustomobject]@{
        disk_number = $DiskNumber
        partition_number = $partition.PartitionNumber
        drive_letter = $letter
        file_system = $volume.FileSystem
        unique_id = $volumeId
        size_bytes = $partition.Size
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run from an elevated PowerShell session inside the disposable VM."
}
if (-not (Test-Path -LiteralPath $CertifierPath -PathType Leaf)) {
    throw "Certifier not found: $CertifierPath"
}

New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
$startedAt = Get-Date
$commands = New-Object System.Collections.Generic.List[object]
$blockers = New-Object System.Collections.Generic.List[string]
$cleanupErrors = New-Object System.Collections.Generic.List[string]
$restoreNtfsDeleteNotify = $false
$before = $null
$after = $null
$volume = $null
$status = "Failed"

try {
    $disk = Get-Disk -Number $TargetDiskNumber
    Assert-DisposableDisk -Disk $disk
    if ($disk.PartitionStyle -ne "RAW" -and -not $Force) {
        throw "Target disk $TargetDiskNumber is not RAW. Use -Force only for disposable lab disks."
    }
    $before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
    $volume = New-RecoveryVolume -DiskNumber $TargetDiskNumber

    $deleteNotifyBefore = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "query", "DisableDeleteNotify")
    $commands.Add($deleteNotifyBefore)
    if ($deleteNotifyBefore.exit_code -ne 0) {
        # Prior state is unknown; restore to the Windows default (delete-notify enabled)
        # so a failed query cannot leave TRIM silently disabled on the lab machine.
        $restoreNtfsDeleteNotify = $true
    }
    elseif ($deleteNotifyBefore.output -match "NTFS DisableDeleteNotify\s*=\s*0" -or
        $deleteNotifyBefore.output -match "DisableDeleteNotify\s*=\s*0") {
        $restoreNtfsDeleteNotify = $true
    }
    $disableDeleteNotify = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "set", "DisableDeleteNotify", "NTFS", "1")
    $commands.Add($disableDeleteNotify)
    if ($disableDeleteNotify.exit_code -ne 0) {
        $disableDeleteNotifyLegacy = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "set", "DisableDeleteNotify", "1")
        $commands.Add($disableDeleteNotifyLegacy)
    }
    # Do not trust either set's exit code: re-query and fail closed unless delete-notify
    # is actually disabled, so the gate can never certify recovery under live TRIM.
    $deleteNotifyVerify = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "query", "DisableDeleteNotify")
    $commands.Add($deleteNotifyVerify)
    if ($deleteNotifyVerify.exit_code -ne 0 -or
        $deleteNotifyVerify.output -notmatch "DisableDeleteNotify\s*=\s*1") {
        throw "Failed to disable NTFS delete-notify; the file-recovery gate requires TRIM disabled and will not certify under an unverified state."
    }

    $reportPath = Join-Path $EvidenceDir "report.json"
    if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
        # A reused evidence directory must not let a previous run's report stand in as
        # this run's proof; remove it so post-run existence means this run wrote it.
        Remove-Item -LiteralPath $reportPath -Force
    }
    $certifierArgs = @(
        "--drive", "$($volume.drive_letter):",
        "--volume-id", $volume.unique_id,
        "--file-system", $volume.file_system,
        "--output-dir", $EvidenceDir,
        "--manifest", "artifacts\partition-manager-certification\vm-lab\external-evidence.json",
        "--suggested-evidence-path", "artifacts/partition-manager-certification/vm-lab/external-evidence/external.file-level-data-recovery/report.json"
    )
    $certifier = Invoke-NativeCommand -FilePath $CertifierPath -Arguments $certifierArgs
    $commands.Add($certifier)
    if ($certifier.exit_code -ne 0) {
        throw "File recovery certifier failed with exit code $($certifier.exit_code)."
    }
    $reportPath = Join-Path $EvidenceDir "report.json"
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "Certifier did not write external evidence report: $reportPath"
    }
    $status = "Passed"
}
catch {
    $blockers.Add($_.Exception.Message)
}
finally {
    if (-not $NoCleanup) {
        try {
            Clear-DisposableDisk -DiskNumber $TargetDiskNumber
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
            if ($status -eq "Passed") {
                $status = "Failed"
                $reportPath = Join-Path $EvidenceDir "report.json"
                $failedCleanupPath = Join-Path $EvidenceDir "report.failed-cleanup.json"
                if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
                    Move-Item -LiteralPath $reportPath -Destination $failedCleanupPath -Force
                }
            }
        }
    }
    if ($restoreNtfsDeleteNotify) {
        try {
            $restoreDeleteNotify = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "set", "DisableDeleteNotify", "NTFS", "0")
            $commands.Add($restoreDeleteNotify)
            if ($restoreDeleteNotify.exit_code -ne 0) {
                $restoreDeleteNotifyLegacy = Invoke-NativeCommand -FilePath "fsutil.exe" -Arguments @("behavior", "set", "DisableDeleteNotify", "0")
                $commands.Add($restoreDeleteNotifyLegacy)
                if ($restoreDeleteNotifyLegacy.exit_code -ne 0) {
                    $cleanupErrors.Add("Failed to restore NTFS DisableDeleteNotify (exit $($restoreDeleteNotifyLegacy.exit_code)).")
                }
            }
        }
        catch {
            $cleanupErrors.Add("Failed to restore NTFS DisableDeleteNotify: $($_.Exception.Message)")
        }
    }
    try {
        $after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
    }
    catch {
        $cleanupErrors.Add($_.Exception.Message)
    }
}

$guestReport = [pscustomobject]@{
    schema_version = 1
    tool = "sak-vm-external-file-recovery-gate"
    status = $status
    vm_id = $env:COMPUTERNAME
    started_at = $startedAt.ToString("o")
    completed_at = (Get-Date).ToString("o")
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    is_admin = Test-IsAdmin
    cleanup_skipped = [bool]$NoCleanup
    target_disk_number = $TargetDiskNumber
    certifier_path = $CertifierPath
    evidence_dir = $EvidenceDir
    source_volume = $volume
    before = $before
    after = $after
    commands = @($commands)
    blockers = @($blockers)
    cleanup_errors = @($cleanupErrors)
}

$guestReportWritten = $true
try {
    $guestReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8
}
catch {
    $guestReportWritten = $false
    $fallbackPath = "$GuestReportPath.write-error.txt"
    "Failed to write guest report: $($_.Exception.Message)" | Set-Content -LiteralPath $fallbackPath -Encoding UTF8
}
if ($status -ne "Passed" -or -not $guestReportWritten) {
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $Script:TranscriptStarted = $false
    }
    if ($guestReportWritten) {
        Write-Error "External file recovery gate failed. Report: $GuestReportPath"
    }
    else {
        Write-Error "External file recovery gate could not write its required JSON evidence: $GuestReportPath"
    }
    exit 1
}

if ($Script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $Script:TranscriptStarted = $false
}
Write-Host "External file recovery gate passed. Report: $GuestReportPath"
exit 0
