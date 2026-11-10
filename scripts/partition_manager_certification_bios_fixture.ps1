<#
.SYNOPSIS
    Builds a real BIOS install of Windows on a throwaway MBR VDI, for the boot rows to start from.

.DESCRIPTION
    Runs elevated in the disposable VM. Images this Windows onto two throwaway disks named by their
    exact sizes: a target that ends up System Reserved, active, then Windows, and a scratch disk for
    the image. The image is captured from a third disk the rig attached: a clone of this VM's own
    system disk, made with the VM off, so nothing running here can take it away mid-capture. This is a fixture and not the application's path: the rows that use
    it test what the application then does to this disk.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [uint64]$TargetBytes,
    [Parameter(Mandatory = $true)] [uint64]$ScratchBytes,
    [string]$NativeSource = 'C:\SAKCert\scripts\os_migration_native.cs'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'partition_manager_readback.ps1')
if (-not (Test-RbInVirtualBox -Facts (Get-RbMachineFacts))) { throw 'This builds a BIOS install for the VM rig, and runs only in a VM.' }

function Get-Throwaway {
    param([uint64]$Bytes)
    $found = @(Get-Disk | Where-Object { [uint64]$_.Size -eq $Bytes })
    if ($found.Count -ne 1) { throw "Disk of $Bytes bytes: found $($found.Count)" }
    $disk = $found[0]
    if ($disk.IsBoot -or $disk.IsSystem -or "$($disk.BusType)" -ne 'SATA' -or "$($disk.FriendlyName)" -notmatch 'VBOX HARDDISK') { throw "Disk $($disk.Number) is not a throwaway VDI" }
    if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false }
    if ("$($disk.PartitionStyle)" -ne 'RAW') { Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false }
    return $disk
}

function Get-SourceClone {
    # The clone is this Windows' disk, so it is the one other disk of exactly the boot disk's size.
    $boot = @(Get-Disk | Where-Object { $_.IsBoot })
    if ($boot.Count -ne 1) { throw "Expected one boot disk: found $($boot.Count)" }
    $found = @(Get-Disk | Where-Object {
            -not $_.IsBoot -and -not $_.IsSystem -and [uint64]$_.Size -eq [uint64]$boot[0].Size -and
            "$($_.FriendlyName)" -match 'VBOX HARDDISK' })
    if ($found.Count -ne 1) { throw "The clone of this Windows' disk: found $($found.Count)" }
    return $found[0]
}

function Clear-Letter {
    param([object]$Disk, [object]$Partition)
    $now = Get-Partition -DiskNumber $Disk.Number -PartitionNumber $Partition.PartitionNumber
    if ([char]::IsLetter($now.DriveLetter)) {
        Remove-PartitionAccessPath -DiskNumber $Disk.Number -PartitionNumber $Partition.PartitionNumber -AccessPath "$($now.DriveLetter):\"
    }
}

function Assert-LastExit {
    param([string]$What)
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

"start $(Get-Date -Format HH:mm:ss)"
Add-Type -TypeDefinition (Get-Content -LiteralPath $NativeSource -Raw)
$target = Get-Throwaway -Bytes $TargetBytes
$scratch = Get-Throwaway -Bytes $ScratchBytes
Initialize-Disk -Number $target.Number -PartitionStyle MBR
$system = New-Partition -DiskNumber $target.Number -Size 524288000 -IsActive
Format-Volume -Partition $system -FileSystem NTFS -NewFileSystemLabel 'System Reserved' -Confirm:$false | Out-Null
$windows = New-Partition -DiskNumber $target.Number -UseMaximumSize
Format-Volume -Partition $windows -FileSystem NTFS -Confirm:$false | Out-Null
Initialize-Disk -Number $scratch.Number -PartitionStyle GPT
$scratchPartition = New-Partition -DiskNumber $scratch.Number -UseMaximumSize
Format-Volume -Partition $scratchPartition -FileSystem NTFS -NewFileSystemLabel 'SCRATCH' -Confirm:$false | Out-Null
Clear-Letter -Disk $target -Partition $system
Clear-Letter -Disk $target -Partition $windows
Clear-Letter -Disk $scratch -Partition $scratchPartition
Add-PartitionAccessPath -DiskNumber $target.Number -PartitionNumber $system.PartitionNumber -AccessPath 'S:\'
Add-PartitionAccessPath -DiskNumber $target.Number -PartitionNumber $windows.PartitionNumber -AccessPath 'W:\'
Add-PartitionAccessPath -DiskNumber $scratch.Number -PartitionNumber $scratchPartition.PartitionNumber -AccessPath 'T:\'

# The capture used to read this Windows through a shadow copy, for half an hour or more. Twice on
# 2026-11-08 a System Restore snapshot came in, System Restore put C:'s shadow storage back to 1%,
# and volsnap deleted the capture's copy (DISM 0x80070015, then 0x80070006). So the source is an
# offline clone: it is onlined once, which gives it a new signature for the one it shares with this
# disk, set read-only, and read through its Windows partition's own device, which needs no letter.
$source = Get-SourceClone
$online = Join-Path $env:TEMP 'sak-bios-source.txt'
@("select disk $($source.Number)", 'online disk noerr', 'attributes disk clear readonly noerr') |
    Set-Content -LiteralPath $online -Encoding ASCII
& diskpart.exe /s $online | Select-Object -Last 3
Update-HostStorageCache
if ((Get-Disk -Number $source.Number).IsOffline) { throw "The clone, disk $($source.Number), did not come online" }
@("select disk $($source.Number)", 'attributes disk set readonly') | Set-Content -LiteralPath $online -Encoding ASCII
& diskpart.exe /s $online | Select-Object -Last 1
Assert-LastExit -What 'Setting the clone read-only'
Remove-Item -LiteralPath $online
$sourceWindows = @(Get-Partition -DiskNumber $source.Number | Where-Object { "$($_.Type)" -eq 'Basic' } |
        Sort-Object Size -Descending | Select-Object -First 1)
if ($sourceWindows.Count -ne 1) { throw "The clone, disk $($source.Number), has no Windows partition" }
$raw = "\Device\Harddisk$($source.Number)\Partition$($sourceWindows[0].PartitionNumber)"
New-Item -ItemType Directory -Force -Path 'T:\scratch' | Out-Null
[SakMigration.Io]::MapDrive('R:', $raw)
try {
    if (-not (Test-Path -LiteralPath 'R:\Windows\System32\config\SYSTEM')) { throw "$raw holds no Windows" }
    "capture from $raw, disk $($source.Number) read-only, $(Get-Date -Format HH:mm:ss)"
    & dism.exe /English /Capture-Image '/ImageFile:T:\b.wim' '/CaptureDir:R:\' '/Name:BIOS source' /Compress:fast /EA '/ScratchDir:T:\scratch' | Select-Object -Last 2
    Assert-LastExit -What 'DISM capture'
} finally {
    [SakMigration.Io]::UnmapDrive('R:', $raw)
    Set-Disk -Number $source.Number -IsOffline $true
}
"apply $(Get-Date -Format HH:mm:ss)"
& dism.exe /English /Apply-Image '/ImageFile:T:\b.wim' /Index:1 '/ApplyDir:W:\' /EA /Compact | Select-Object -Last 2
Assert-LastExit -What 'DISM apply'
& bcdboot.exe 'W:\Windows' /s 'S:' /f BIOS
Assert-LastExit -What 'bcdboot'
& bootsect.exe /nt60 'S:' /mbr
Assert-LastExit -What 'bootsect'

# The image holds this Windows' list of drive letters: C: has to mean the new partition.
$disk = Get-Disk -Number $target.Number
$windowsNow = Get-Partition -DiskNumber $target.Number -PartitionNumber $windows.PartitionNumber
$bytes = [BitConverter]::GetBytes([uint32]$disk.Signature) + [BitConverter]::GetBytes([uint64]$windowsNow.Offset)
$hex = -join ($bytes | ForEach-Object { $_.ToString('X2') })
& reg.exe load 'HKLM\SAK_BIOS_SOURCE' 'W:\Windows\System32\config\SYSTEM'
Assert-LastExit -What 'Loading the hive'
& reg.exe add 'HKLM\SAK_BIOS_SOURCE\MountedDevices' /v '\DosDevices\C:' /t REG_BINARY /d $hex /f
Assert-LastExit -What 'Setting C:'
[gc]::Collect()
& reg.exe unload 'HKLM\SAK_BIOS_SOURCE'
Assert-LastExit -What 'Unloading the hive'
Remove-PartitionAccessPath -DiskNumber $target.Number -PartitionNumber $system.PartitionNumber -AccessPath 'S:\'
Remove-PartitionAccessPath -DiskNumber $target.Number -PartitionNumber $windows.PartitionNumber -AccessPath 'W:\'
Remove-PartitionAccessPath -DiskNumber $scratch.Number -PartitionNumber $scratchPartition.PartitionNumber -AccessPath 'T:\'
Set-Disk -Number $target.Number -IsOffline $true
"end $(Get-Date -Format HH:mm:ss)"
