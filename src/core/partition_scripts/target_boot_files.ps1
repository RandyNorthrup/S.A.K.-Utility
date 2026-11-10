# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Shared by Migrate OS and Boot Repair of an attached disk: temporary letters on a disk that is not the
# one Windows runs from, and rebuilding that disk's boot files.
$script:sakSystem32 = [Environment]::SystemDirectory
function Invoke-SakTool([string]$name, [string[]]$toolArgs) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $lines = @(& (Join-Path $script:sakSystem32 $name) @toolArgs 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  foreach ($line in $lines) { if ($line.Trim()) { [Console]::Out.WriteLine($line) } }
  if ($code -ne 0) {
    throw ('{0} failed with exit code {1}: {2}' -f $name, $code, (($lines | Select-Object -Last 3) -join ' '))
  }
}
function Add-SakTargetLetter($partition) {
  $partition = Get-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
  if ([char]::IsLetter($partition.DriveLetter)) { return [string]$partition.DriveLetter }
  $letter = Get-SakFreeBootLetter 'S'
  Add-PartitionAccessPath -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "${letter}:\" -ErrorAction Stop | Out-Null
  return $letter
}
function Remove-SakTargetLetters([uint32]$diskNumber) {
  foreach ($partition in @(Get-Partition -DiskNumber $diskNumber -ErrorAction Stop)) {
    if ([char]::IsLetter($partition.DriveLetter)) {
      Remove-PartitionAccessPath -DiskNumber $diskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "$($partition.DriveLetter):\" -ErrorAction Stop
    }
  }
}

# Rebuilds the boot files of the Windows at $windowsRoot on its own disk's system partition. Never
# touches this PC's firmware boot list: the next restart must still start this PC's own Windows.
function Invoke-SakTargetBootFiles($systemPartition, [string]$windowsRoot, [string]$mode) {
  $letter = Add-SakTargetLetter $systemPartition
  $relative = if ($mode -eq 'UEFI') { 'EFI\Microsoft\Boot\BCD' } else { 'Boot\BCD' }
  $store = "${letter}:\$relative"
  # The old store names the partitions of the disk it came from. Kept beside the new one.
  if (Test-Path -LiteralPath $store) {
    $kept = "$store.sak-old"
    if (Test-Path -LiteralPath $kept) { Remove-Item -LiteralPath $kept -Force -ErrorAction Stop }
    Move-Item -LiteralPath $store -Destination $kept -Force -ErrorAction Stop
    Get-ChildItem -LiteralPath (Split-Path -Parent $store) -Force -Filter 'BCD.LOG*' -ErrorAction Stop | Remove-Item -Force -ErrorAction Stop
    Write-Output "Old boot store kept as $relative.sak-old"
  }
  $toolArgs = @($windowsRoot, '/s', "${letter}:", '/f', $mode)
  if ($mode -eq 'UEFI') {
    $help = (& (Join-Path $script:sakSystem32 'bcdboot.exe') '/?' | Out-String)
    $toolArgs += if ($help -match 'nofirmwaresync') { '/nofirmwaresync' } else { '/addlast' }
  }
  Invoke-SakTool 'bcdboot.exe' $toolArgs
  if ($mode -eq 'BIOS') { Invoke-SakTool 'bootsect.exe' @('/nt60', "${letter}:", '/mbr') }
  if (-not (Test-Path -LiteralPath $store)) { throw "bcdboot finished but $relative is not there" }
  # bcdboot leaves that switch in the store, where it would also stop the disk's own Windows from
  # adding itself to the boot list of the PC it ends up in.
  if ($toolArgs -contains '/nofirmwaresync') {
    try { Invoke-SakTool 'bcdedit.exe' @('/store', $store, '/deletevalue', '{bootmgr}', 'nofirmwaresync') }
    catch { Write-Output "WARNING: the new boot store keeps bcdboot's no-firmware setting: $($_.Exception.Message)" }
  }
}
# The drive letter Windows finds itself on comes from this list, keyed by its partition's identity.
# A rebuilt or re-signed disk has a new identity, and without this Windows starts on the wrong letter
# and nobody can sign in.
function Get-SakMountedDeviceValue($disk, $partition) {
  if ("$($disk.PartitionStyle)" -eq 'GPT') {
    $bytes = [Text.Encoding]::ASCII.GetBytes('DMIO:ID:') + ([Guid]$partition.Guid).ToByteArray()
  } else {
    $bytes = [BitConverter]::GetBytes([uint32]$disk.Signature) + [BitConverter]::GetBytes([uint64]$partition.Offset)
  }
  return -join ($bytes | ForEach-Object { $_.ToString('X2') })
}
function Set-SakTargetDriveLetters([string]$windowsRoot, $disk, $letterToPartition) {
  $hive = Join-Path $windowsRoot 'System32\config\SYSTEM'
  $key = 'HKLM\SAK_MIGRATE_TARGET'
  Invoke-SakTool 'reg.exe' @('load', $key, $hive)
  $script:sakHiveLoaded = $true
  try {
    foreach ($letter in @($letterToPartition.Keys)) {
      $hex = Get-SakMountedDeviceValue $disk $letterToPartition[$letter]
      Invoke-SakTool 'reg.exe' @('add', "$key\MountedDevices", '/v', "\DosDevices\${letter}:", '/t', 'REG_BINARY', '/d', $hex, '/f')
    }
  } finally {
    Invoke-SakTool 'reg.exe' @('unload', $key)
    $script:sakHiveLoaded = $false
  }
}
