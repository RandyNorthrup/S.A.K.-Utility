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

function Resolve-SystemTool {
    param([string]$FileName)
    $path = Join-Path $env:SystemRoot (Join-Path "System32" $FileName)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required system tool not found: $path"
    }
    return $path
}

function Invoke-NativeCommand {
    param([string]$Name, [string]$FilePath, [string[]]$Arguments = @())
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $started = Get-Date
    $global:LASTEXITCODE = $null
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($null -eq $exitCode) {
        throw "$Name did not launch (no exit code from $FilePath)."
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
    return Invoke-NativeCommand -Name $Name -FilePath (Resolve-SystemTool "diskpart.exe") -Arguments @("/s", $scriptPath)
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction Stop | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
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
            is_active = [bool]$partition.IsActive
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

# Online the disk (read/write) so its filesystem can be inspected, but defer the
# partition-active mutation until the target is confirmed to be Windows. Only a
# genuinely offline disk is onlined, and WITHOUT "noerr", so a real online
# failure surfaces instead of being swallowed and then mutated past.
$prepLines = @(
    "select disk $TargetDiskNumber",
    "attributes disk clear readonly"
)
if ($before.is_offline) {
    $prepLines += "online disk"
}
$prepLines += @(
    "detail disk",
    "list partition",
    "list volume"
)
Invoke-DiskPartScript -Name "prepare-target-disk" -Lines $prepLines | Out-Null
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
        # Do not silence the cmdlet failure: record it as evidence before the
        # diskpart alternative runs so the two mechanisms stay distinguishable.
        $script:Commands += [pscustomobject]@{
            name = "add-partition-access-path-failed"
            file_path = ""
            arguments = @("${windowsLetter}:")
            exit_code = $null
            duration_seconds = 0
            output = $_.Exception.Message
        }
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

# Windows only installs on NTFS; require it before writing boot files so a
# non-Windows or wrong volume that merely holds a \Windows folder is refused.
$windowsVolume = Get-Volume -DriveLetter $windowsLetter -ErrorAction SilentlyContinue
if ($null -eq $windowsVolume -or [string]$windowsVolume.FileSystem -ne "NTFS") {
    throw "Target volume ${windowsLetter}: is not an NTFS Windows volume; refusing BIOS repair."
}

# Only now that the target is a confirmed Windows NTFS volume, mark that SAME
# partition active. Marking a fixed "partition 1" active would point BIOS at a
# System-Reserved partition that never receives the boot files this script
# writes to the Windows partition, leaving a standard layout unbootable.
Invoke-DiskPartScript -Name "activate-windows-partition" -Lines @(
    "select disk $TargetDiskNumber",
    "select partition $($windowsPartition.PartitionNumber)",
    "active"
) | Out-Null

$bcdboot = Invoke-NativeCommand -Name "bcdboot-bios" -FilePath (Resolve-SystemTool "bcdboot.exe") -Arguments @($windowsPath, "/s", $targetRoot, "/f", "BIOS")
$bootsect = Invoke-NativeCommand -Name "bootsect-bios" -FilePath ((Get-Command "bootsect.exe" -CommandType Application -ErrorAction Stop).Source) -Arguments @("/nt60", $targetRoot, "/mbr", "/force")
$bcdEnum = Invoke-NativeCommand -Name "bcdedit-store-enum" -FilePath (Resolve-SystemTool "bcdedit.exe") -Arguments @("/store", "$targetRoot\Boot\BCD", "/enum", "all")

$after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($after.unique_id -ne $before.unique_id) {
    throw "Disk identity changed during repair (before '$($before.unique_id)', after '$($after.unique_id)')."
}
$bootmgrExists = (Test-Path -LiteralPath "$targetRoot\bootmgr" -PathType Leaf)
$bcdExists = (Test-Path -LiteralPath "$targetRoot\Boot\BCD" -PathType Leaf)
$windowsExists = (Test-Path -LiteralPath "$targetRoot\Windows" -PathType Container)
if (-not $bootmgrExists -or -not $bcdExists) {
    throw "Boot files missing after repair (bootmgr=$bootmgrExists, bcd=$bcdExists); refusing to report success."
}
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
        bootmgr_exists = $bootmgrExists
        bcd_exists = $bcdExists
        windows_exists = $windowsExists
    }
    commands_path = $commandsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "offline-bios-repair-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "Offline BIOS repair complete: $(Join-Path $script:RunRoot "offline-bios-repair-report.json")"
