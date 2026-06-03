<#
.SYNOPSIS
    Repairs UEFI boot files on an attached disposable offline Windows disk.

.DESCRIPTION
    Runs elevated inside SAK-PM-Lab-Win11. Targets a non-system cloned OS disk,
    assigns temporary letters to its EFI and Windows partitions, runs bcdboot,
    writes fallback BOOTX64.EFI, and records command/layout evidence.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-uefi-repair",
    [string]$ExpectedTargetLabel = "OS migration cloned target disk",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Commands = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        $errorPath = Join-Path $script:RunRoot "offline-uefi-repair-error.json"
        [pscustomobject]@{
            tool = "partition-manager-offline-uefi-bcdboot-repair"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            commands = $script:Commands
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $errorPath -Encoding UTF8
    }
    if ($script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $script:TranscriptStarted = $false
    }
    break
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [switch]$AllowFailure
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $started = Get-Date
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $record = [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = $exitCode
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
    }
    $script:Commands += $record
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string[]]$Lines
    )

    $scriptPath = Join-Path $script:RunRoot ("diskpart-" + [guid]::NewGuid().ToString("N") + ".txt")
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    return Invoke-NativeCommand -Name $Name -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    foreach ($letter in @("S", "W", "R", "T", "U", "V", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "No available drive letter."
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Offset)) {
        $volume = $null
        if ($partition.DriveLetter) {
            $volume = Get-Volume -DriveLetter $partition.DriveLetter -ErrorAction SilentlyContinue
        }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
            file_system = if ($null -ne $volume) { [string]$volume.FileSystem } else { "" }
            label = if ($null -ne $volume) { [string]$volume.FileSystemLabel } else { "" }
        }
    }
    return [pscustomobject]@{
        disk_number = [int]$disk.Number
        friendly_name = [string]$disk.FriendlyName
        serial_number = [string]$disk.SerialNumber
        unique_id = [string]$disk.UniqueId
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        is_offline = [bool]$disk.IsOffline
        is_read_only = [bool]$disk.IsReadOnly
        partitions = $partitions
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run elevated inside the VM."
}
if (-not $Force) {
    throw "Pass -Force after confirming the attached disk is the disposable target clone."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "offline-uefi-repair-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$candidateDisks = @(Get-Disk | Where-Object {
    -not $_.IsBoot -and -not $_.IsSystem -and
    $_.Size -gt 60GB -and $_.Size -lt 120GB
} | Sort-Object Number)

if ($TargetDiskNumber -lt 0) {
    if ($candidateDisks.Count -ne 1) {
        throw "Expected exactly one non-system 60-120GB target disk, found $($candidateDisks.Count)."
    }
    $TargetDiskNumber = [int]$candidateDisks[0].Number
}

$before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($before.is_boot -or $before.is_system) {
    throw "Refusing boot/system disk $TargetDiskNumber."
}
if ($before.size_bytes -lt 60GB -or $before.size_bytes -gt 120GB) {
    throw "Refusing disk $TargetDiskNumber size $($before.size_bytes); expected disposable cloned OS disk."
}

$newDiskGuid = [guid]::NewGuid().ToString("D")
Invoke-DiskPartScript -Name "prepare-target-disk" -Lines @(
    "select disk $TargetDiskNumber",
    "uniqueid disk id=$newDiskGuid",
    "attributes disk clear readonly",
    "online disk noerr",
    "detail disk",
    "list partition",
    "list volume"
) | Out-Null
Update-HostStorageCache

$efiPartition = Get-Partition -DiskNumber $TargetDiskNumber | Where-Object { [string]$_.Type -eq "System" } | Sort-Object Size | Select-Object -First 1
if ($null -eq $efiPartition) {
    throw "No EFI System partition found on disk $TargetDiskNumber."
}
$windowsPartition = Get-Partition -DiskNumber $TargetDiskNumber |
    Where-Object { [string]$_.Type -match "Basic|IFS|Microsoft Basic Data" -and $_.Size -gt 20GB } |
    Sort-Object Size -Descending |
    Select-Object -First 1
if ($null -eq $windowsPartition) {
    throw "No Windows-sized partition found on disk $TargetDiskNumber."
}

$efiLetter = if ($efiPartition.DriveLetter) { [string]$efiPartition.DriveLetter } else { Get-AvailableDriveLetter }
if (-not $efiPartition.DriveLetter) {
    try {
        Add-PartitionAccessPath -DiskNumber $TargetDiskNumber -PartitionNumber $efiPartition.PartitionNumber -AccessPath "${efiLetter}:"
    }
    catch {
        Invoke-DiskPartScript -Name "assign-efi-letter" -Lines @(
            "select disk $TargetDiskNumber",
            "select partition $($efiPartition.PartitionNumber)",
            "assign letter=$efiLetter"
        ) | Out-Null
    }
}
$windowsLetter = if ($windowsPartition.DriveLetter) { [string]$windowsPartition.DriveLetter } else { Get-AvailableDriveLetter }
if (-not $windowsPartition.DriveLetter) {
    try {
        Add-PartitionAccessPath -DiskNumber $TargetDiskNumber -PartitionNumber $windowsPartition.PartitionNumber -AccessPath "${windowsLetter}:"
    }
    catch {
        Invoke-DiskPartScript -Name "assign-windows-letter" -Lines @(
            "select disk $TargetDiskNumber",
            "select partition $($windowsPartition.PartitionNumber)",
            "assign letter=$windowsLetter"
        ) | Out-Null
    }
}

$windowsPath = "${windowsLetter}:\Windows"
if (-not (Test-Path -LiteralPath $windowsPath -PathType Container)) {
    throw "Windows directory not found at $windowsPath."
}

$beforeBcd = "${efiLetter}:\EFI\Microsoft\Boot\BCD"
$brokenBackup = "${efiLetter}:\EFI\Microsoft\Boot\BCD.sak-before-repair"
if ((Test-Path -LiteralPath $beforeBcd -PathType Leaf) -and -not (Test-Path -LiteralPath $brokenBackup -PathType Leaf)) {
    Copy-Item -LiteralPath $beforeBcd -Destination $brokenBackup -Force
}

$bcdboot = Invoke-NativeCommand -Name "bcdboot-uefi" -FilePath "bcdboot.exe" -Arguments @($windowsPath, "/s", "${efiLetter}:", "/f", "UEFI")
$fallbackDir = "${efiLetter}:\EFI\BOOT"
New-Item -ItemType Directory -Path $fallbackDir -Force | Out-Null
$bootmgfw = "${efiLetter}:\EFI\Microsoft\Boot\bootmgfw.efi"
$fallbackBoot = Join-Path $fallbackDir "BOOTX64.EFI"
if (Test-Path -LiteralPath $bootmgfw -PathType Leaf) {
    Copy-Item -LiteralPath $bootmgfw -Destination $fallbackBoot -Force
}

$after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
$logPath = Join-Path $script:RunRoot "offline-uefi-repair-report.json"
$commandsPath = Join-Path $script:RunRoot "offline-uefi-repair-commands.json"
$script:Commands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-offline-uefi-bcdboot-repair"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    expected_target = $ExpectedTargetLabel
    target_disk_number = $TargetDiskNumber
    new_disk_guid = $newDiskGuid
    efi_drive = "${efiLetter}:"
    windows_drive = "${windowsLetter}:"
    before_layout = $before
    after_layout = $after
    bcdboot_exit_code = $bcdboot.exit_code
    fallback_bootx64_exists = (Test-Path -LiteralPath $fallbackBoot -PathType Leaf)
    commands_path = $commandsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $logPath -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "Offline UEFI repair complete: $logPath"
