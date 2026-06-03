<#
.SYNOPSIS
    Repairs BIOS/MBR boot files on an attached disposable offline Windows disk.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-bios-repair",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Commands = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        [pscustomobject]@{
            tool = "partition-manager-offline-bios-bcdboot-repair"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            commands = $script:Commands
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $script:RunRoot "offline-bios-repair-error.json") -Encoding UTF8
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
    param([string]$Name, [string]$FilePath, [string[]]$Arguments = @())
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
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function Invoke-DiskPartScript {
    param([string]$Name, [string[]]$Lines)
    $scriptPath = Join-Path $script:RunRoot ("diskpart-" + [guid]::NewGuid().ToString("N") + ".txt")
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    return Invoke-NativeCommand -Name $Name -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    foreach ($letter in @("W", "T", "R", "S", "U", "V", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "No available drive letter."
}

function Get-DiskSnapshot {
    param([int]$DiskNumber)
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
    throw "Pass -Force after confirming the attached disk is the disposable BIOS target."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "offline-bios-repair-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$candidateDisks = @(Get-Disk | Where-Object {
    -not $_.IsBoot -and -not $_.IsSystem -and
    $_.PartitionStyle -eq "MBR" -and
    $_.Size -gt 60GB -and $_.Size -lt 120GB
} | Sort-Object Number)
if ($TargetDiskNumber -lt 0) {
    if ($candidateDisks.Count -ne 1) {
        throw "Expected exactly one non-system MBR 60-120GB target disk, found $($candidateDisks.Count)."
    }
    $TargetDiskNumber = [int]$candidateDisks[0].Number
}

$before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($before.is_boot -or $before.is_system -or $before.partition_style -ne "MBR") {
    throw "Refusing non-disposable or non-MBR disk $TargetDiskNumber."
}

Invoke-DiskPartScript -Name "prepare-target-disk" -Lines @(
    "select disk $TargetDiskNumber",
    "attributes disk clear readonly",
    "online disk noerr",
    "select partition 1",
    "active",
    "detail disk",
    "list partition",
    "list volume"
) | Out-Null
Update-HostStorageCache

$windowsPartition = Get-Partition -DiskNumber $TargetDiskNumber |
    Where-Object { [string]$_.Type -match "IFS|Basic|Microsoft Basic Data" -and $_.Size -gt 20GB } |
    Sort-Object Size -Descending |
    Select-Object -First 1
if ($null -eq $windowsPartition) {
    throw "No Windows-sized partition found on disk $TargetDiskNumber."
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

$targetRoot = "${windowsLetter}:"
$windowsPath = "$targetRoot\Windows"
if (-not (Test-Path -LiteralPath $windowsPath -PathType Container)) {
    throw "Windows directory not found at $windowsPath."
}

$bcdboot = Invoke-NativeCommand -Name "bcdboot-bios" -FilePath "bcdboot.exe" -Arguments @($windowsPath, "/s", $targetRoot, "/f", "BIOS")
$bootsect = Invoke-NativeCommand -Name "bootsect-bios" -FilePath "bootsect.exe" -Arguments @("/nt60", $targetRoot, "/mbr", "/force")
$bcdEnum = Invoke-NativeCommand -Name "bcdedit-store-enum" -FilePath "bcdedit.exe" -Arguments @("/store", "$targetRoot\Boot\BCD", "/enum", "all")

$after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
$commandsPath = Join-Path $script:RunRoot "offline-bios-repair-commands.json"
$script:Commands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-offline-bios-bcdboot-repair"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    target_disk_number = $TargetDiskNumber
    target_drive = $targetRoot
    before_layout = $before
    after_layout = $after
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
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "offline-bios-repair-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "Offline BIOS repair complete: $(Join-Path $script:RunRoot "offline-bios-repair-report.json")"
