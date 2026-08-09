<#
.SYNOPSIS
    Runs mbr2gpt validate/convert on the disposable BIOS boot VM system disk.

.DESCRIPTION
    Refuses to run outside a hypervisor guest, resolves the boot/system disk
    instead of defaulting to disk 0, pins the disk identity across validate,
    convert and post-conversion snapshots, and requires the conversion to have
    actually produced a GPT disk with an EFI system partition before it records
    success. Firmware switch and UEFI boot are NOT verified here; the report
    says so explicitly.
#>

[CmdletBinding()]
param(
    [ValidateRange(-1, 4095)] [int]$DiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\system-mbr2gpt",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Commands = @()

trap {
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        [pscustomobject]@{
            tool = "partition-manager-system-mbr2gpt-external-gate"
            status = "Failed"
            created_utc = (Get-Date).ToUniversalTime().ToString("o")
            error = $_.Exception.Message
            position = $_.InvocationInfo.PositionMessage
            stack = $_.ScriptStackTrace
            commands = $script:Commands
        } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $script:RunRoot "system-mbr2gpt-error.json") -Encoding UTF8
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

function Get-VirtualMachineIdentity {
    $system = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
    $product = Get-CimInstance -ClassName Win32_ComputerSystemProduct -ErrorAction Stop
    $identity = [pscustomobject]@{
        computer_name = [string]$env:COMPUTERNAME
        manufacturer = [string]$system.Manufacturer
        model = [string]$system.Model
        system_uuid = [string]$product.UUID
    }
    $marker = "$($identity.manufacturer) $($identity.model)".Trim()
    foreach ($name in @("VirtualBox", "innotek", "VMware", "QEMU", "KVM", "Xen", "Parallels", "Bochs", "Virtual Machine")) {
        if ($marker -like "*$name*") {
            return $identity
        }
    }
    throw "Refusing to run: '$marker' is not a known hypervisor guest. This gate converts the SYSTEM disk and only runs on the disposable BIOS boot VM."
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @()
    )

    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "$Name executable not found at its qualified path: $FilePath"
    }
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $started = Get-Date
    # Clear the inherited exit code so a command that never sets one cannot inherit a
    # stale zero and be recorded as a pass.
    $global:LASTEXITCODE = $null
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
    if ($null -eq $exitCode) {
        throw "$Name reported no exit code. Refusing to treat it as success. $($record.output)"
    }
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function Get-DiskSnapshot {
    param([int]$Number)
    $disk = Get-Disk -Number $Number -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $Number -ErrorAction Stop | Sort-Object Offset)) {
        $volume = $null
        if ($partition.DriveLetter) {
            $volume = Get-Volume -DriveLetter $partition.DriveLetter -ErrorAction Stop
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

function Assert-SameDisk {
    param(
        [Parameter(Mandatory = $true)] $Expected,
        [Parameter(Mandatory = $true)] $Actual,
        [Parameter(Mandatory = $true)] [string]$Stage,
        [switch]$IncludeUniqueId
    )
    $fields = @("disk_number", "friendly_name", "serial_number", "bus_type", "size_bytes")
    if ($IncludeUniqueId) {
        $fields += "unique_id"
    }
    foreach ($field in $fields) {
        if ([string]$Expected.$field -ne [string]$Actual.$field) {
            throw "Disk identity changed at $Stage ($field was '$($Expected.$field)', now '$($Actual.$field)'). Refusing to continue."
        }
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run elevated inside the VM."
}
$vmIdentity = Get-VirtualMachineIdentity
if (-not $Force) {
    throw "Pass -Force after confirming this is the disposable BIOS boot VM."
}

if ($DiskNumber -lt 0) {
    # Never silently assume disk 0: resolve the one boot/system disk, or refuse.
    $candidates = @(Get-Disk -ErrorAction Stop | Where-Object { $_.IsBoot -and $_.IsSystem })
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one boot/system disk to convert, found $($candidates.Count). Pass -DiskNumber explicitly."
    }
    $DiskNumber = [int]$candidates[0].Number
    Write-Host "Resolved boot/system disk $DiskNumber ($([string]$candidates[0].FriendlyName))."
}

$script:RunRoot = Join-Path $OutputRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "system-mbr2gpt-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$before = Get-DiskSnapshot -Number $DiskNumber
if (-not $before.is_boot -or -not $before.is_system -or $before.partition_style -ne "MBR") {
    throw "Refusing disk $DiskNumber. Expected disposable boot/system MBR disk."
}
if ($before.size_bytes -lt 60GB -or $before.size_bytes -gt 120GB) {
    throw "Refusing disk $DiskNumber size $($before.size_bytes); expected disposable 60-120GB VM OS disk."
}
if ([string]::IsNullOrWhiteSpace($before.serial_number) -and [string]::IsNullOrWhiteSpace($before.unique_id)) {
    throw "Disk $DiskNumber reports neither a serial number nor a unique id, so its identity cannot be pinned across the conversion."
}

$mbr2gpt = Join-Path $env:SystemRoot "System32\mbr2gpt.exe"
$validate = Invoke-NativeCommand -Name "mbr2gpt-validate" -FilePath $mbr2gpt -Arguments @("/validate", "/disk:$DiskNumber", "/allowFullOS")

# Disk numbers are rebindable. Prove the validated device is still the device we are
# about to convert; both snapshots are still MBR so unique_id must match too.
$beforeConvert = Get-DiskSnapshot -Number $DiskNumber
Assert-SameDisk -Expected $before -Actual $beforeConvert -Stage "mbr2gpt convert" -IncludeUniqueId
if ($beforeConvert.partition_style -ne "MBR") {
    throw "Disk $DiskNumber is no longer MBR before conversion (style $($beforeConvert.partition_style))."
}

$convert = Invoke-NativeCommand -Name "mbr2gpt-convert" -FilePath $mbr2gpt -Arguments @("/convert", "/disk:$DiskNumber", "/allowFullOS")
Update-HostStorageCache
Start-Sleep -Seconds 2
$after = Get-DiskSnapshot -Number $DiskNumber
# unique_id is derived from the partition table and legitimately changes on MBR->GPT,
# so it is deliberately excluded from the post-conversion identity pin.
Assert-SameDisk -Expected $before -Actual $after -Stage "post-conversion verification"
if ($after.partition_style -ne "GPT") {
    throw "mbr2gpt reported success but disk $DiskNumber is still $($after.partition_style)."
}
$efiPartitions = @($after.partitions | Where-Object { $_.type -eq "System" })
if ($efiPartitions.Count -eq 0) {
    throw "mbr2gpt reported success but disk $DiskNumber has no EFI system partition after conversion."
}

$commandsPath = Join-Path $script:RunRoot "system-mbr2gpt-commands.json"
$script:Commands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-system-mbr2gpt-external-gate"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $vmIdentity.computer_name
    vm_identity = $vmIdentity
    disk_number = $DiskNumber
    disk_serial_number = $before.serial_number
    disk_unique_id_before = $before.unique_id
    disk_unique_id_after = $after.unique_id
    before_partition_style = $before.partition_style
    after_partition_style = $after.partition_style
    before_layout = $before
    after_layout = $after
    efi_system_partition_count = $efiPartitions.Count
    mbr2gpt_validate_exit_code = $validate.exit_code
    mbr2gpt_convert_exit_code = $convert.exit_code
    firmware_switch_verified = $false
    uefi_boot_verified = $false
    certification_note = "In-OS conversion evidence only. Switching the VM firmware to UEFI and booting Windows must be certified separately."
    commands_path = $commandsPath
}
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $script:RunRoot "system-mbr2gpt-report.json") -Encoding UTF8
if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "System MBR2GPT conversion complete (firmware switch and UEFI boot still unverified): $(Join-Path $script:RunRoot "system-mbr2gpt-report.json")"
