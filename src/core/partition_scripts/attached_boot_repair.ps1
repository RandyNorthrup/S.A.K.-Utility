# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Boot Repair of a disk that is not the one Windows runs from. partition_os_migration_script.cpp puts
# $sakRepairDisk and $sakRepairMode in front of this, and the caller pins the disk's identity.
# Which volumes hold a Windows, read from each one with a temporary letter. A volume with no file
# system gets no letter: Windows would offer on screen to format it.
function Find-SakAttachedWindows($partitions) {
  $found = New-Object System.Collections.ArrayList
  foreach ($partition in $partitions) {
    if (@('System', 'Reserved', 'Recovery') -contains "$($partition.Type)") { continue }
    $volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
    if (-not $volume -or -not $volume.FileSystem) { continue }
    $letter = Add-SakTargetLetter $partition
    if ((Test-Path -LiteralPath "${letter}:\Windows\System32\config\SYSTEM") -and
        ((Test-Path -LiteralPath "${letter}:\Windows\System32\winload.efi") -or
         (Test-Path -LiteralPath "${letter}:\Windows\System32\winload.exe"))) {
      [void]$found.Add([pscustomobject]@{ Partition = $partition; Letter = $letter })
    }
  }
  return ,$found
}
function Get-SakAttachedSystemPartition($disk, $partitions, $windowsPartition) {
  if ($sakRepairMode -eq 'UEFI') {
    $esp = @($partitions | Where-Object { "$($_.GptType)" -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' }) | Select-Object -First 1
    if (-not $esp) { throw "UEFI boot repair needs an EFI System Partition on Disk $($disk.Number), and there is none" }
    return $esp
  }
  $active = @($partitions | Where-Object { $_.IsActive }) | Select-Object -First 1
  if ($active) { return $active }
  $reserved = @($partitions | Where-Object { "$($_.PartitionNumber)" -ne "$($windowsPartition.PartitionNumber)" -and
    "$((Get-Volume -Partition $_ -ErrorAction SilentlyContinue).FileSystemLabel)" -eq 'System Reserved' }) | Select-Object -First 1
  $choice = if ($reserved) { $reserved } else { $windowsPartition }
  Set-Partition -DiskNumber $disk.Number -PartitionNumber $choice.PartitionNumber -IsActive $true -ErrorAction Stop
  Write-Output "Disk $($disk.Number) had no active partition. Partition $($choice.PartitionNumber) is now active."
  return Get-Partition -DiskNumber $disk.Number -PartitionNumber $choice.PartitionNumber -ErrorAction Stop
}
# The letter this Windows calls its own, from its own registry. Empty when that cannot be read.
function Get-SakWindowsOwnLetter([string]$windowsRoot) {
  $key = 'HKLM\SAK_REPAIR_SOFTWARE'
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & (Join-Path $script:sakSystem32 'reg.exe') load $key (Join-Path $windowsRoot 'System32\config\SOFTWARE') 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { return '' }
    $line = & (Join-Path $script:sakSystem32 'reg.exe') query "$key\Microsoft\Windows NT\CurrentVersion" /v SystemRoot 2>&1 | Out-String
    & (Join-Path $script:sakSystem32 'reg.exe') unload $key 2>&1 | Out-Null
    if ($line -match 'REG_SZ\s+([A-Za-z]):\\') { return $Matches[1].ToUpperInvariant() }
    return ''
  } finally { $ErrorActionPreference = $previous }
}

$disk = Get-Disk -Number $sakRepairDisk -ErrorAction Stop
if ($disk.IsBoot -or $disk.IsSystem) {
  throw "Disk $sakRepairDisk is the disk Windows is running from, so this repair does not run on it. Repair of the running Windows is a separate choice."
}
if ($disk.IsReadOnly) { throw "Disk $sakRepairDisk is read-only" }
$expected = if ($sakRepairMode -eq 'UEFI') { 'GPT' } else { 'MBR' }
if ("$($disk.PartitionStyle)" -ne $expected) {
  throw "$sakRepairMode boot repair needs a $expected disk, and Disk $sakRepairDisk is $($disk.PartitionStyle)"
}
$wasOffline = [bool]$disk.IsOffline
if ($wasOffline) { Set-Disk -Number $sakRepairDisk -IsOffline $false -ErrorAction Stop; Start-Sleep -Seconds 3 }
$before = @(Get-Partition -DiskNumber $sakRepairDisk -ErrorAction Stop | Where-Object { [char]::IsLetter($_.DriveLetter) } | ForEach-Object { $_.PartitionNumber })
try {
  $partitions = @(Get-Partition -DiskNumber $sakRepairDisk -ErrorAction Stop)
  $windows = Find-SakAttachedWindows $partitions
  if ($windows.Count -eq 0) {
    throw "No Windows was found on Disk $sakRepairDisk. If its volume is encrypted with BitLocker, unlock it first and run this again."
  }
  if ($windows.Count -gt 1) {
    throw "More than one Windows is on Disk ${sakRepairDisk}: partitions $(($windows | ForEach-Object { $_.Partition.PartitionNumber }) -join ', '). This repair handles one at a time and will not guess."
  }
  $windowsRoot = "$($windows[0].Letter):\Windows"
  Write-Output "Found Windows on Disk $sakRepairDisk, partition $($windows[0].Partition.PartitionNumber)"
  $system = Get-SakAttachedSystemPartition $disk $partitions $windows[0].Partition
  Invoke-SakTargetBootFiles $system $windowsRoot $sakRepairMode
  $own = Get-SakWindowsOwnLetter $windowsRoot
  if ($own) {
    Set-SakTargetDriveLetters $windowsRoot (Get-Disk -Number $sakRepairDisk -ErrorAction Stop) @{ $own = (Get-Partition -DiskNumber $sakRepairDisk -PartitionNumber $windows[0].Partition.PartitionNumber -ErrorAction Stop) }
  } else {
    Write-Output 'WARNING: could not read which drive letter this Windows uses, so its letter list was left as it was.'
  }
  Write-Output "Boot files rebuilt for the Windows on Disk $sakRepairDisk. Start from that disk to check."
} finally {
  foreach ($partition in @(Get-Partition -DiskNumber $sakRepairDisk -ErrorAction Stop)) {
    if ([char]::IsLetter($partition.DriveLetter) -and $before -notcontains $partition.PartitionNumber) {
      Remove-PartitionAccessPath -DiskNumber $sakRepairDisk -PartitionNumber $partition.PartitionNumber -AccessPath "$($partition.DriveLetter):\" -ErrorAction Stop
    }
  }
  if ($wasOffline) { Set-Disk -Number $sakRepairDisk -IsOffline $true -ErrorAction Stop }
}
