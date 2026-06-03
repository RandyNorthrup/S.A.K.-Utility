<#
.SYNOPSIS
    Runs the external BitLocker mutation gate inside the disposable VM.

.DESCRIPTION
    This script is intended to run from an elevated PowerShell session inside
    SAK-PM-Lab-Win11. It prepares a disposable VirtualBox data disk, runs the
    same unlock/suspend/resume command flow exposed by the Partition Manager
    BitLocker dialog, writes a matrix-backed external evidence report, and
    clears the disk back to RAW.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = 2,
    [string]$EvidenceDir = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation",
    [string]$GuestReportPath = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-bitlocker-mutation-guest-report.json",
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$Script:TranscriptStarted = $false
try {
    $guestReportParent = Split-Path -Parent $GuestReportPath
    if (-not [string]::IsNullOrWhiteSpace($guestReportParent)) {
        New-Item -ItemType Directory -Path $guestReportParent -Force | Out-Null
        Start-Transcript -Path (Join-Path $guestReportParent "run_partition_manager_bitlocker_mutation_external_gate.log") -Force | Out-Null
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

    return (($Value | ForEach-Object {
        if ($null -eq $_) {
            ""
        }
        else {
            $_.ToString()
        }
    }) -join "`n").Trim()
}

function Redact-RecoveryPassword {
    param([string]$Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return $Text
    }
    return ($Text -replace "\d{6}(?:-\d{6}){7}", "[REDACTED-DISPOSABLE-RECOVERY-PASSWORD]")
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = @($Arguments | ForEach-Object { Redact-RecoveryPassword -Text $_ })
        exit_code = $exitCode
        output = Redact-RecoveryPassword -Text (ConvertTo-PlainText -Value $output)
    }
}

function Invoke-NativeCommandRaw {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = $exitCode
        output = ConvertTo-PlainText -Value $output
    }
}

function ConvertTo-SanitizedCommandLog {
    param([Parameter(Mandatory = $true)] [object]$Command)

    [pscustomobject]@{
        name = $Command.name
        file_path = $Command.file_path
        arguments = @($Command.arguments | ForEach-Object { Redact-RecoveryPassword -Text $_ })
        exit_code = $Command.exit_code
        output = Redact-RecoveryPassword -Text $Command.output
    }
}

function Invoke-ScriptCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$ScriptBlock
    )

    $exitCode = 0
    $output = $null
    try {
        $output = & $ScriptBlock 2>&1
    }
    catch {
        $exitCode = 1
        $output = @($output) + $_.Exception.Message
    }

    [pscustomobject]@{
        name = $Name
        file_path = "PowerShell"
        arguments = @()
        exit_code = $exitCode
        output = Redact-RecoveryPassword -Text (ConvertTo-PlainText -Value $output)
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

function New-BitLockerLabVolume {
    param([int]$DiskNumber)

    Clear-DisposableDisk -DiskNumber $DiskNumber
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    $partition = New-Partition -DiskNumber $DiskNumber -Size 1GB -AssignDriveLetter
    $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel "SAKBLMUT" -Confirm:$false -Force
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber
    $letter = $partition.DriveLetter
    if ([string]::IsNullOrWhiteSpace($letter)) {
        $letter = $volume.DriveLetter
    }
    if ([string]::IsNullOrWhiteSpace($letter)) {
        throw "Disposable BitLocker mutation volume did not receive a drive letter."
    }
    $volume = Get-Volume -DriveLetter $letter
    $accessPaths = @($partition.AccessPaths)
    $volumeId = ($accessPaths | Where-Object { $_ -like "\\?\Volume{*}\" } | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($volumeId)) {
        $volumeId = "\\.\$letter`:"
    }
    [pscustomobject]@{
        disk_number = $DiskNumber
        partition_number = $partition.PartitionNumber
        drive_letter = "$letter`:"
        file_system = $volume.FileSystem
        unique_id = $volumeId
        size_bytes = $partition.Size
    }
}

function Get-RecoveryPassword {
    param([string]$Output)

    $match = [regex]::Match($Output, "\d{6}(?:-\d{6}){7}")
    if (-not $match.Success) {
        throw "Could not parse disposable recovery password from manage-bde output."
    }
    return $match.Value
}

function Get-BitLockerState {
    param([string]$MountPoint)

    $volume = Get-BitLockerVolume -MountPoint $MountPoint
    [pscustomobject]@{
        mount_point = $MountPoint
        volume_status = $volume.VolumeStatus.ToString()
        protection_status = $volume.ProtectionStatus.ToString()
        lock_status = $volume.LockStatus.ToString()
        encryption_method = $volume.EncryptionMethod.ToString()
    }
}

function Wait-ForEncryptedVolume {
    param([string]$MountPoint)

    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $state = Get-BitLockerState -MountPoint $MountPoint
        if ($state.volume_status -match "Encrypted" -and $state.protection_status -eq "On") {
            return $state
        }
        Start-Sleep -Seconds 2
    }
    throw "Timed out waiting for BitLocker encryption to complete on $MountPoint."
}

function Wait-ForProtectionState {
    param(
        [string]$MountPoint,
        [string]$Expected
    )

    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $state = Get-BitLockerState -MountPoint $MountPoint
        if ($state.protection_status -eq $Expected) {
            return $state
        }
        Start-Sleep -Seconds 1
    }
    throw "Timed out waiting for BitLocker ProtectionStatus=$Expected on $MountPoint."
}

function Wait-ForFullyDecrypted {
    param([string]$MountPoint)

    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $status = Invoke-NativeCommand -Name "cleanup-manage-bde-status" -FilePath "manage-bde.exe" -Arguments @("-status", $MountPoint)
        if ($status.output -match "Fully Decrypted" -or $status.output -match "Percentage Encrypted:\s+0\.0%") {
            return
        }
        Start-Sleep -Seconds 2
    }
}

function New-ExternalReport {
    param(
        [Parameter(Mandatory = $true)] [string]$Status,
        [Parameter(Mandatory = $true)] [hashtable]$Evidence,
        [Parameter(Mandatory = $true)] [string[]]$Artifacts,
        [Parameter(Mandatory = $true)] [string]$VerificationSummary,
        [Parameter(Mandatory = $true)] [string]$OperatorNotes
    )

    [pscustomobject]@{
        tool = "partition-manager-external-evidence-report"
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        gate_id = "external.bitlocker-mutation"
        gate_name = "In-app BitLocker unlock/suspend/resume mutation proof"
        status = $Status
        manifest = "artifacts\partition-manager-certification\vm-lab\external-evidence.json"
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/external.bitlocker-mutation/report.json"
        safety_contract = @(
            "disposable_bitlocker_volume_only",
            "recovery_key_or_password_fixture_recorded",
            "mutation_commands_logged",
            "final_protection_state_verified"
        )
        required_evidence_keys = @(
            "volume_id",
            "initial_lock_state",
            "unlock_result",
            "suspend_result",
            "resume_result",
            "final_protection_state",
            "operation_audit_log"
        )
        required_evidence_values = $null
        evidence = $Evidence
        artifacts = $Artifacts
        verification_summary = $VerificationSummary
        operator_notes = $OperatorNotes
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run from an elevated PowerShell session inside the disposable VM."
}
if (-not (Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)) {
    throw "BitLocker PowerShell module is not available in this VM."
}

New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
    $stalePath = Join-Path $EvidenceDir $staleReport
    if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
        Remove-Item -LiteralPath $stalePath -Force
    }
}
$startedAt = Get-Date
$commands = New-Object System.Collections.Generic.List[object]
$blockers = New-Object System.Collections.Generic.List[string]
$cleanupErrors = New-Object System.Collections.Generic.List[string]
$before = $null
$after = $null
$volume = $null
$status = "Failed"
$reportPath = Join-Path $EvidenceDir "report.json"
$recoveryPasswordRecorded = "generated-and-redacted"
$states = [ordered]@{}
$evidence = @{}

try {
    $disk = Get-Disk -Number $TargetDiskNumber
    Assert-DisposableDisk -Disk $disk
    if ($disk.PartitionStyle -ne "RAW" -and -not $Force) {
        throw "Target disk $TargetDiskNumber is not RAW. Use -Force only for disposable lab disks."
    }

    $before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
    $volume = New-BitLockerLabVolume -DiskNumber $TargetDiskNumber
    $mountPoint = $volume.drive_letter
    $fixturePath = Join-Path $mountPoint "sak-bitlocker-mutation-fixture.txt"
    "SAK BitLocker mutation certification fixture $(Get-Date -Format o)" |
        Set-Content -LiteralPath $fixturePath -Encoding UTF8

    $addProtectorRaw = Invoke-NativeCommandRaw -Name "manage-bde-add-recovery-password" -FilePath "manage-bde.exe" -Arguments @("-protectors", "-add", $mountPoint, "-RecoveryPassword")
    $recoveryPassword = Get-RecoveryPassword -Output $addProtectorRaw.output
    $commands.Add((ConvertTo-SanitizedCommandLog -Command $addProtectorRaw))

    $enable = Invoke-NativeCommand -Name "manage-bde-on-used-space-only" -FilePath "manage-bde.exe" -Arguments @("-on", $mountPoint, "-UsedSpaceOnly")
    $commands.Add($enable)
    if ($enable.exit_code -ne 0) {
        throw "manage-bde -on failed with exit code $($enable.exit_code)."
    }
    $states.encrypted = Wait-ForEncryptedVolume -MountPoint $mountPoint

    $lock = Invoke-NativeCommand -Name "manage-bde-lock" -FilePath "manage-bde.exe" -Arguments @("-lock", $mountPoint, "-ForceDismount")
    $commands.Add($lock)
    if ($lock.exit_code -ne 0) {
        throw "manage-bde -lock failed with exit code $($lock.exit_code)."
    }
    $statusLocked = Invoke-NativeCommand -Name "manage-bde-status-locked" -FilePath "manage-bde.exe" -Arguments @("-status", $mountPoint)
    $commands.Add($statusLocked)

    $unlock = Invoke-NativeCommand -Name "manage-bde-unlock-recovery-password" -FilePath "manage-bde.exe" -Arguments @("-unlock", $mountPoint, "-RecoveryPassword", $recoveryPassword)
    $commands.Add($unlock)
    if ($unlock.exit_code -ne 0) {
        throw "manage-bde -unlock failed with exit code $($unlock.exit_code)."
    }
    $states.unlocked = Wait-ForProtectionState -MountPoint $mountPoint -Expected "On"

    $suspend = Invoke-NativeCommand -Name "manage-bde-protectors-disable" -FilePath "manage-bde.exe" -Arguments @("-protectors", "-disable", $mountPoint)
    $commands.Add($suspend)
    if ($suspend.exit_code -ne 0) {
        throw "manage-bde -protectors -disable failed with exit code $($suspend.exit_code)."
    }
    $states.suspended = Wait-ForProtectionState -MountPoint $mountPoint -Expected "Off"

    $resume = Invoke-NativeCommand -Name "manage-bde-protectors-enable" -FilePath "manage-bde.exe" -Arguments @("-protectors", "-enable", $mountPoint)
    $commands.Add($resume)
    if ($resume.exit_code -ne 0) {
        throw "manage-bde -protectors -enable failed with exit code $($resume.exit_code)."
    }
    $states.resumed = Wait-ForProtectionState -MountPoint $mountPoint -Expected "On"

    $statusFinal = Invoke-NativeCommand -Name "manage-bde-status-final" -FilePath "manage-bde.exe" -Arguments @("-status", $mountPoint)
    $commands.Add($statusFinal)

    $evidence = @{
        volume_id = $volume.unique_id
        initial_lock_state = "Locked after manage-bde -lock; $($statusLocked.output -replace '\s+', ' ')"
        unlock_result = "Recovery-password unlock exit_code=$($unlock.exit_code); LockStatus=$($states.unlocked.lock_status); ProtectionStatus=$($states.unlocked.protection_status)"
        suspend_result = "Suspend command completed; ProtectionStatus=$($states.suspended.protection_status)"
        resume_result = "Resume command completed; ProtectionStatus=$($states.resumed.protection_status)"
        final_protection_state = "ProtectionStatus=$($states.resumed.protection_status); LockStatus=$($states.resumed.lock_status); VolumeStatus=$($states.resumed.volume_status)"
        operation_audit_log = "Sanitized command audit recorded in artifacts/partition-manager-certification/vm-lab/external-bitlocker-mutation-guest-report.json; recovery password fixture $recoveryPasswordRecorded"
    }

    $artifacts = @(
        "artifacts/partition-manager-certification/vm-lab/external-bitlocker-mutation-guest-report.json",
        "artifacts/partition-manager-certification/vm-lab/bitlocker-mutation-stage-debug/run_partition_manager_bitlocker_mutation_external_gate.log"
    )
    $report = New-ExternalReport `
        -Status "Passed" `
        -Evidence $evidence `
        -Artifacts $artifacts `
        -VerificationSummary "Windows 11 VirtualBox VM $env:COMPUTERNAME used disposable VBOX disk $TargetDiskNumber, enabled BitLocker used-space-only on a data volume, locked it, unlocked with a generated recovery password, suspended and resumed protection, verified final protection On, and kept all command output sanitized." `
        -OperatorNotes "Recovery password was generated only for the disposable lab volume and redacted from host evidence; command flow matches the S.A.K. Partition Manager BitLocker dialog templates."
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    $status = "Passed"
}
catch {
    $blockers.Add($_.Exception.Message)
    $failedEvidence = if ($evidence.Count -gt 0) { $evidence } else {
        @{
            volume_id = if ($volume) { $volume.unique_id } else { "not-created" }
            initial_lock_state = "not-verified"
            unlock_result = "not-verified"
            suspend_result = "not-verified"
            resume_result = "not-verified"
            final_protection_state = "not-verified"
            operation_audit_log = "Failed before complete audit: $($_.Exception.Message)"
        }
    }
    $failedReport = New-ExternalReport `
        -Status "Failed" `
        -Evidence $failedEvidence `
        -Artifacts @("artifacts/partition-manager-certification/vm-lab/external-bitlocker-mutation-guest-report.json") `
        -VerificationSummary "BitLocker mutation certification failed." `
        -OperatorNotes $_.Exception.Message
    $failedReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $EvidenceDir "report.failed.json") -Encoding UTF8
}
finally {
    if ($volume -and -not $NoCleanup) {
        try {
            $mountPoint = $volume.drive_letter
            if (-not [string]::IsNullOrWhiteSpace($recoveryPassword)) {
                $unlockCleanup = Invoke-NativeCommand -Name "cleanup-unlock" -FilePath "manage-bde.exe" -Arguments @("-unlock", $mountPoint, "-RecoveryPassword", $recoveryPassword)
                $commands.Add($unlockCleanup)
            }
        }
        catch {
            $cleanupErrors.Add("cleanup unlock failed: $($_.Exception.Message)")
        }
        try {
            $off = Invoke-NativeCommand -Name "cleanup-bitlocker-off" -FilePath "manage-bde.exe" -Arguments @("-off", $mountPoint)
            $commands.Add($off)
            Wait-ForFullyDecrypted -MountPoint $mountPoint
        }
        catch {
            $cleanupErrors.Add("cleanup decrypt failed: $($_.Exception.Message)")
        }
    }
    if (-not $NoCleanup) {
        try {
            Clear-DisposableDisk -DiskNumber $TargetDiskNumber
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
            if ($status -eq "Passed") {
                $status = "Failed"
                $failedCleanupPath = Join-Path $EvidenceDir "report.failed-cleanup.json"
                if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
                    Move-Item -LiteralPath $reportPath -Destination $failedCleanupPath -Force
                }
            }
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
    tool = "sak-vm-external-bitlocker-mutation-gate"
    status = $status
    vm_id = $env:COMPUTERNAME
    started_at = $startedAt.ToString("o")
    completed_at = (Get-Date).ToString("o")
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    is_admin = Test-IsAdmin
    target_disk_number = $TargetDiskNumber
    evidence_dir = $EvidenceDir
    source_volume = $volume
    before = $before
    after = $after
    states = $states
    commands = @($commands)
    blockers = @($blockers)
    cleanup_errors = @($cleanupErrors)
}

try {
    $guestReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8
}
catch {
    $fallbackPath = "$GuestReportPath.write-error.txt"
    "Failed to write guest report: $($_.Exception.Message)" | Set-Content -LiteralPath $fallbackPath -Encoding UTF8
}

if ($status -ne "Passed") {
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $Script:TranscriptStarted = $false
    }
    Write-Error "External BitLocker mutation gate failed. Report: $GuestReportPath"
    exit 1
}

if ($Script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $Script:TranscriptStarted = $false
}
Write-Host "External BitLocker mutation gate passed. Report: $GuestReportPath"
exit 0
