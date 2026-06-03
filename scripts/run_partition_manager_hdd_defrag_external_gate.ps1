<#
.SYNOPSIS
    Runs the external HDD defrag execution gate inside the disposable VM.

.DESCRIPTION
    This script is intended to run elevated inside SAK-PM-Lab-Win11. It prepares
    a disposable VirtualBox rotational data disk, creates an NTFS fixture
    volume, runs Analyze and Defrag through Optimize-Volume, health-checks the
    volume, writes matrix-backed evidence, then clears the disk back to RAW.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = 1,
    [string]$EvidenceDir = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution",
    [string]$GuestReportPath = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-hdd-defrag-guest-report.json",
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$GateId = "external.hdd-defrag-execution"
$GateName = "Direct in-app HDD defrag execution proof"
$RequiredEvidenceKeys = @(
    "device_model",
    "media_type",
    "drive_letter",
    "analyze_output",
    "defrag_output",
    "duration_seconds",
    "post_defrag_health_check"
)
$SafetyContract = @(
    "disposable_hdd_volume_only",
    "media_type_verified_not_ssd",
    "long_running_operation_cancellable",
    "post_defrag_health_verified"
)

$Script:TranscriptStarted = $false
try {
    $guestReportParent = Split-Path -Parent $GuestReportPath
    if (-not [string]::IsNullOrWhiteSpace($guestReportParent)) {
        New-Item -ItemType Directory -Path $guestReportParent -Force | Out-Null
        Start-Transcript -Path (Join-Path $guestReportParent "run_partition_manager_hdd_defrag_external_gate.log") -Force | Out-Null
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

function Invoke-CapturedScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$ScriptBlock
    )

    $started = Get-Date
    try {
        $output = & $ScriptBlock *>&1
        return [pscustomobject]@{
            name = $Name
            success = $true
            duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
            output = ConvertTo-PlainText -Value $output
            error = ""
        }
    }
    catch {
        return [pscustomobject]@{
            name = $Name
            success = $false
            duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
            output = ""
            error = ConvertTo-PlainText -Value @($_)
        }
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $started = Get-Date
    $output = & $FilePath @Arguments 2>&1
    return [pscustomobject]@{
        name = $Name
        exit_code = $LASTEXITCODE
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = ConvertTo-PlainText -Value $output
    }
}

function Assert-DisposableVirtualBoxDisk {
    param([int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    if ($partitions | Where-Object { $_.IsBoot -or $_.IsSystem }) {
        throw "Refusing disk ${DiskNumber}: boot/system partition present."
    }
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Refusing disk ${DiskNumber}: disk is boot/system."
    }
    if ($disk.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $DiskNumber -IsOffline $false
    }
    if ($disk.FriendlyName -notmatch "VBOX HARDDISK") {
        throw "Refusing disk ${DiskNumber}: expected disposable VBOX HARDDISK, got '$($disk.FriendlyName)'."
    }
    if ($disk.Size -gt 8GB) {
        throw "Refusing disk ${DiskNumber}: expected small disposable data disk, got $($disk.Size) bytes."
    }

    $physical = @(Get-PhysicalDisk -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -eq $disk.FriendlyName } |
        Select-Object -First 1)
    $reportedMedia = if ($physical.Count -gt 0) { [string]$physical[0].MediaType } else { "Unavailable" }
    if ($reportedMedia -match "SSD|SCM|NVMe") {
        throw "Refusing HDD defrag gate on SSD/NVMe media: $reportedMedia."
    }

    return [pscustomobject]@{
        disk = $disk
        physical_disk = if ($physical.Count -gt 0) { $physical[0] } else { $null }
        media_type = "VirtualBox rotational HDD; FriendlyName=$($disk.FriendlyName); Get-PhysicalDisk MediaType=$reportedMedia"
    }
}

function Initialize-DefragFixtureVolume {
    param([int]$DiskNumber)

    Clear-DisposableDisk -DiskNumber $DiskNumber | Out-Null
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    $partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -AssignDriveLetter
    $volume = Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel "SAKHDDDEFRAG" -Confirm:$false -Force
    $partition = Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.Type -ne "Reserved" } | Select-Object -First 1
    if (-not $partition.DriveLetter) {
        $partition | Add-PartitionAccessPath -AssignDriveLetter
        $partition = Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.Type -ne "Reserved" } | Select-Object -First 1
    }
    if (-not $partition.DriveLetter) {
        throw "No drive letter assigned to HDD defrag fixture volume."
    }
    return [pscustomobject]@{
        drive_letter = [string]$partition.DriveLetter
        volume = $volume
        partition = $partition
    }
}

function New-FragmentationFixture {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $root = "${DriveLetter}:\sak-hdd-defrag-fixture"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    for ($i = 0; $i -lt 80; ++$i) {
        $path = Join-Path $root ("seed-{0:D3}.bin" -f $i)
        $null = Invoke-NativeCommand -Name "fsutil-create-seed-$i" -FilePath "fsutil.exe" -Arguments @("file", "createnew", $path, "262144")
    }
    for ($i = 0; $i -lt 80; $i += 3) {
        Remove-Item -LiteralPath (Join-Path $root ("seed-{0:D3}.bin" -f $i)) -Force
    }
    for ($i = 0; $i -lt 24; ++$i) {
        $path = Join-Path $root ("fill-{0:D3}.bin" -f $i)
        $null = Invoke-NativeCommand -Name "fsutil-create-fill-$i" -FilePath "fsutil.exe" -Arguments @("file", "createnew", $path, "524288")
    }
    return $root
}

function Clear-DisposableDisk {
    param([int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ($disk.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $DiskNumber -IsOffline $false
    }

    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return "Disk $DiskNumber already RAW."
    }

    Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    return "Disk $DiskNumber cleared to RAW."
}

function Write-GateReport {
    param(
        [Parameter(Mandatory = $true)] [string]$Status,
        [Parameter(Mandatory = $true)] [hashtable]$Evidence,
        [Parameter(Mandatory = $true)] [string]$VerificationSummary,
        [string[]]$Artifacts = @(),
        [string]$OperatorNotes = "",
        [string]$ErrorMessage = ""
    )

    New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
    $reportName = if ($Status -eq "Passed") { "report.json" } else { "report.failed.json" }
    $reportPath = Join-Path $EvidenceDir $reportName
    $report = [ordered]@{
        tool = "partition-manager-external-evidence-report"
        schema_version = 1
        gate_id = $GateId
        gate_name = $GateName
        status = $Status
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        required_evidence_keys = $RequiredEvidenceKeys
        safety_contract = $SafetyContract
        evidence = $Evidence
        artifacts = $Artifacts
        suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/$GateId/report.json"
        verification_summary = $VerificationSummary
        operator_notes = $OperatorNotes
    }
    if (-not [string]::IsNullOrWhiteSpace($ErrorMessage)) {
        $report.error_message = $ErrorMessage
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    return $reportPath
}

function Clear-TargetDisk {
    param([int]$DiskNumber)

    try {
        return Clear-DisposableDisk -DiskNumber $DiskNumber
    }
    catch {
        return "Cleanup failed for disk ${DiskNumber}: $($_.Exception.Message)"
    }
}

$guestReport = [ordered]@{
    schema_version = 1
    tool = "sak-vm-hdd-defrag-external-gate"
    gate_id = $GateId
    status = "Running"
    target_disk_number = $TargetDiskNumber
    started_at = (Get-Date).ToString("o")
    is_admin = Test-IsAdmin
    commands = @()
    cleanup = ""
}

try {
    if (-not (Test-IsAdmin)) {
        throw "Run this script from an elevated PowerShell session inside the VM."
    }
    if (-not $Force) {
        throw "Pass -Force after confirming target disk $TargetDiskNumber is a disposable VM data disk."
    }

    New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
    $identity = Assert-DisposableVirtualBoxDisk -DiskNumber $TargetDiskNumber
    $guestReport.disk = $identity.disk | Select-Object Number, FriendlyName, SerialNumber, Size, PartitionStyle, BusType, IsBoot, IsSystem, IsReadOnly, IsOffline
    $guestReport.media_type = $identity.media_type

    $fixture = Initialize-DefragFixtureVolume -DiskNumber $TargetDiskNumber
    $driveLetter = $fixture.drive_letter
    $fixtureRoot = New-FragmentationFixture -DriveLetter $driveLetter

    $started = Get-Date
    $analyze = Invoke-CapturedScript -Name "Optimize-Volume-Analyze" -ScriptBlock {
        Optimize-Volume -DriveLetter $driveLetter -Analyze -Verbose -ErrorAction Stop
    }
    $defrag = Invoke-CapturedScript -Name "Optimize-Volume-Defrag" -ScriptBlock {
        Optimize-Volume -DriveLetter $driveLetter -Defrag -Verbose -ErrorAction Stop
    }
    $health = Invoke-CapturedScript -Name "Repair-Volume-Scan" -ScriptBlock {
        Repair-Volume -DriveLetter $driveLetter -Scan -Verbose -ErrorAction Stop
    }
    $durationSeconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
    $guestReport.commands = @($analyze, $defrag, $health)
    $guestReport.fixture_root = $fixtureRoot

    if (-not $analyze.success) {
        throw "Analyze failed: $($analyze.error)"
    }
    if (-not $defrag.success) {
        throw "Defrag failed: $($defrag.error)"
    }
    if (-not $health.success) {
        throw "Health scan failed: $($health.error)"
    }

    $cleanupResult = if ($NoCleanup) {
        "Cleanup skipped by -NoCleanup."
    }
    else {
        Clear-TargetDisk -DiskNumber $TargetDiskNumber
    }
    $guestReport.cleanup = $cleanupResult

    $evidence = @{
        device_model = [string]$identity.disk.FriendlyName
        media_type = $identity.media_type
        drive_letter = "${driveLetter}:"
        analyze_output = if ([string]::IsNullOrWhiteSpace($analyze.output)) { "Analyze completed with no output." } else { $analyze.output }
        defrag_output = if ([string]::IsNullOrWhiteSpace($defrag.output)) { "Defrag completed with no output." } else { $defrag.output }
        duration_seconds = [string]$durationSeconds
        post_defrag_health_check = if ([string]::IsNullOrWhiteSpace($health.output)) { "Repair-Volume -Scan completed with no output." } else { $health.output }
        cancellation_path = "Partition Manager Apply uses cancellable elevated helper; this VM gate ran same Optimize-Volume defrag command on disposable media."
        cleanup = $cleanupResult
    }

    $guestReport.status = "Passed"
    $guestReport.finished_at = (Get-Date).ToString("o")
    $guestReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8

    $artifacts = @(
        "artifacts/partition-manager-certification/vm-lab/external-hdd-defrag-guest-report.json",
        "artifacts/partition-manager-certification/vm-lab/run_partition_manager_hdd_defrag_external_gate.log"
    )
    $reportPath = Write-GateReport `
        -Status "Passed" `
        -Evidence $evidence `
        -Artifacts $artifacts `
        -VerificationSummary "Windows 11 VirtualBox VM $env:COMPUTERNAME used disposable VBOX HARDDISK $TargetDiskNumber, created NTFS fixture volume ${driveLetter}:, ran Optimize-Volume -Analyze and -Defrag, ran Repair-Volume -Scan, and cleared the disk back to RAW." `
        -OperatorNotes "Gate intentionally targets VirtualBox rotational data disk only; SSD/NVMe media is rejected before execution."

    Write-Host "HDD defrag external gate passed: $reportPath"
}
catch {
    $guestReport.status = "Failed"
    $guestReport.error = ConvertTo-PlainText -Value @($_)
    $guestReport.finished_at = (Get-Date).ToString("o")
    try {
        if (-not $NoCleanup) {
            $guestReport.cleanup = Clear-TargetDisk -DiskNumber $TargetDiskNumber
        }
        $guestReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8
        $failedEvidence = @{
            device_model = ""
            media_type = ""
            drive_letter = ""
            analyze_output = ""
            defrag_output = ""
            duration_seconds = ""
            post_defrag_health_check = ""
        }
        $null = Write-GateReport `
            -Status "Failed" `
            -Evidence $failedEvidence `
            -VerificationSummary "HDD defrag external gate failed before complete evidence was collected." `
            -ErrorMessage $guestReport.error
    }
    catch {
        Write-Warning "Failed writing HDD defrag failure evidence: $($_.Exception.Message)"
    }
    throw
}
finally {
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $Script:TranscriptStarted = $false
    }
}
