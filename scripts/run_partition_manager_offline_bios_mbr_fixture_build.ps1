<#
.SYNOPSIS
    Builds a disposable BIOS/MBR Windows boot fixture from an offline Windows disk.

.DESCRIPTION
    Runs elevated inside SAK-PM-Lab-Win11. Copies an attached offline Windows
    volume to an attached blank 60-120GB target disk, partitions the target as
    BIOS/MBR, writes BIOS boot files, and records command/layout evidence.
#>

[CmdletBinding()]
param(
    [int]$SourceDiskNumber = -1,
    [int]$TargetDiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\bios-mbr-fixture",
    [switch]$ResumeExistingTarget,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Commands = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        [pscustomobject]@{
            tool = "partition-manager-offline-bios-mbr-fixture-build"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            commands = $script:Commands
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $script:RunRoot "bios-mbr-fixture-error.json") -Encoding UTF8
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
        [int[]]$AllowedExitCodes = @(0)
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
    if ($AllowedExitCodes -notcontains $exitCode) {
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
    foreach ($letter in @("W", "T", "R", "S", "V", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
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

function Resolve-WindowsPartition {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Size -Descending)) {
        if ([string]$partition.Type -notmatch "Basic|IFS|Microsoft Basic Data") {
            continue
        }
        $letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
        if ([string]::IsNullOrWhiteSpace($letter)) {
            $letter = Get-AvailableDriveLetter
            try {
                Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "${letter}:"
            }
            catch {
                Invoke-DiskPartScript -Name "assign-source-windows-letter" -Lines @(
                    "select disk $DiskNumber",
                    "select partition $($partition.PartitionNumber)",
                    "assign letter=$letter"
                ) | Out-Null
            }
        }
        if (Test-Path -LiteralPath "${letter}:\Windows" -PathType Container) {
            return [pscustomobject]@{
                partition_number = [int]$partition.PartitionNumber
                drive_letter = $letter
                windows_path = "${letter}:\Windows"
            }
        }
    }
    return $null
}

function Resolve-TargetPartition {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $partition = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue |
        Where-Object { [string]$_.Type -match "Basic|IFS|Microsoft Basic Data" } |
        Sort-Object Size -Descending |
        Select-Object -First 1)
    if ($partition.Count -eq 0) {
        return $null
    }

    $letter = if ($partition[0].DriveLetter) { [string]$partition[0].DriveLetter } else { "" }
    if ([string]::IsNullOrWhiteSpace($letter)) {
        $letter = Get-AvailableDriveLetter
        try {
            Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition[0].PartitionNumber -AccessPath "${letter}:"
        }
        catch {
            Invoke-DiskPartScript -Name "assign-target-letter" -Lines @(
                "select disk $DiskNumber",
                "select partition $($partition[0].PartitionNumber)",
                "assign letter=$letter"
            ) | Out-Null
        }
    }

    return [pscustomobject]@{
        partition_number = [int]$partition[0].PartitionNumber
        drive_letter = $letter
        root = "${letter}:"
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run elevated inside the VM."
}
if (-not $Force) {
    throw "Pass -Force after confirming both attached disks are disposable."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "bios-mbr-fixture-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$candidates = @(Get-Disk | Where-Object {
    -not $_.IsBoot -and -not $_.IsSystem -and $_.Size -gt 60GB -and $_.Size -lt 120GB
} | Sort-Object Number)

if ($SourceDiskNumber -lt 0) {
    foreach ($candidate in $candidates) {
        if ($candidate.PartitionStyle -eq "GPT" -and (Resolve-WindowsPartition -DiskNumber $candidate.Number)) {
            $SourceDiskNumber = [int]$candidate.Number
            break
        }
    }
}

if ($TargetDiskNumber -lt 0) {
    foreach ($candidate in $candidates) {
        $candidatePartitions = @(Get-Partition -DiskNumber $candidate.Number -ErrorAction SilentlyContinue)
        $isBlankTarget = $candidate.PartitionStyle -eq "RAW" -or $candidatePartitions.Count -eq 0
        $isResumeTarget = $ResumeExistingTarget -and $candidate.PartitionStyle -eq "MBR" -and $candidatePartitions.Count -gt 0
        if ($candidate.Number -ne $SourceDiskNumber -and ($isBlankTarget -or $isResumeTarget)) {
            $TargetDiskNumber = [int]$candidate.Number
            break
        }
    }
}

if ($SourceDiskNumber -lt 0 -or $TargetDiskNumber -lt 0 -or $SourceDiskNumber -eq $TargetDiskNumber) {
    throw "Could not identify distinct offline source Windows disk and blank target disk."
}

$sourceBefore = Get-DiskSnapshot -DiskNumber $SourceDiskNumber
$targetBefore = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($sourceBefore.is_boot -or $sourceBefore.is_system -or $targetBefore.is_boot -or $targetBefore.is_system) {
    throw "Refusing boot/system disk. Source=$SourceDiskNumber Target=$TargetDiskNumber"
}

$sourceWindows = Resolve-WindowsPartition -DiskNumber $SourceDiskNumber
if ($null -eq $sourceWindows) {
    throw "Source Windows partition not found on disk $SourceDiskNumber."
}

$targetPartition = $null
if ($ResumeExistingTarget -and $targetBefore.partition_style -eq "MBR" -and $targetBefore.partitions.Count -gt 0) {
    $targetPartition = Resolve-TargetPartition -DiskNumber $TargetDiskNumber
    if ($null -ne $targetPartition) {
        Invoke-DiskPartScript -Name "ensure-target-mbr-active" -Lines @(
            "select disk $TargetDiskNumber",
            "select partition $($targetPartition.partition_number)",
            "active",
            "detail disk",
            "list partition",
            "list volume"
        ) | Out-Null
    }
}

if ($null -eq $targetPartition) {
    $targetLetter = Get-AvailableDriveLetter
    Invoke-DiskPartScript -Name "partition-target-mbr" -Lines @(
        "select disk $TargetDiskNumber",
        "clean",
        "convert mbr",
        "create partition primary",
        "format fs=ntfs quick label=SAKBIOS",
        "active",
        "assign letter=$targetLetter",
        "detail disk",
        "list partition",
        "list volume"
    ) | Out-Null
    Update-HostStorageCache
    $targetRoot = "${targetLetter}:"
}
else {
    $targetRoot = $targetPartition.root
}

$robocopyLog = Join-Path $script:RunRoot "robocopy-windows-volume.log"
$robocopy = Invoke-NativeCommand `
    -Name "robocopy-windows-volume" `
    -FilePath "robocopy.exe" `
    -Arguments @(
        "$($sourceWindows.drive_letter):\",
        "$targetRoot\",
        "/MIR",
        "/COPYALL",
        "/B",
        "/XJ",
        "/R:1",
        "/W:1",
        "/MT:8",
        "/NFL",
        "/NDL",
        "/NJH",
        "/NJS",
        "/NP",
        "/LOG:$robocopyLog",
        "/XD",
        "$($sourceWindows.drive_letter):\System Volume Information",
        "$($sourceWindows.drive_letter):\`$Recycle.Bin",
        "WindowsApps",
        "/XF",
        "pagefile.sys",
        "hiberfil.sys",
        "swapfile.sys"
    ) `
    -AllowedExitCodes @(0, 1, 2, 3, 4, 5, 6, 7)

$bcdboot = Invoke-NativeCommand -Name "bcdboot-bios" -FilePath "bcdboot.exe" -Arguments @("$targetRoot\Windows", "/s", "$targetRoot", "/f", "BIOS")
$bootsect = Invoke-NativeCommand -Name "bootsect-bios" -FilePath "bootsect.exe" -Arguments @("/nt60", "$targetRoot", "/mbr", "/force")
$bcdEnum = Invoke-NativeCommand -Name "bcdedit-store-enum" -FilePath "bcdedit.exe" -Arguments @("/store", "$targetRoot\Boot\BCD", "/enum", "all")

$sourceAfter = Get-DiskSnapshot -DiskNumber $SourceDiskNumber
$targetAfter = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
$commandsPath = Join-Path $script:RunRoot "bios-mbr-fixture-commands.json"
$script:Commands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-offline-bios-mbr-fixture-build"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    source_disk_number = $SourceDiskNumber
    target_disk_number = $TargetDiskNumber
    source_windows_drive = "$($sourceWindows.drive_letter):"
    target_windows_drive = $targetRoot
    source_before = $sourceBefore
    source_after = $sourceAfter
    target_before = $targetBefore
    target_after = $targetAfter
    robocopy_exit_code = $robocopy.exit_code
    bcdboot_exit_code = $bcdboot.exit_code
    bootsect_exit_code = $bootsect.exit_code
    bcd_store_enum_exit_code = $bcdEnum.exit_code
    boot_files = @{
        bootmgr_exists = (Test-Path -LiteralPath "$targetRoot\bootmgr" -PathType Leaf)
        bcd_exists = (Test-Path -LiteralPath "$targetRoot\Boot\BCD" -PathType Leaf)
        windows_exists = (Test-Path -LiteralPath "$targetRoot\Windows" -PathType Container)
    }
    commands_path = $commandsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "bios-mbr-fixture-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "BIOS/MBR fixture build complete: $(Join-Path $script:RunRoot "bios-mbr-fixture-report.json")"
