# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Migrate OS. partition_os_migration_script.cpp puts its parameters and the native code in front of this:
# $sakSourceDisk, $sakTargetDisk, $sakSourcePin, $sakTargetPin, $sakScheme, $sakRoute, $sakVerify,
# $sakAllowNoSnapshot, $sakScratchOffset, $sakScratchSize and $sakPartitions.
function Assert-SakPinnedDisk([uint32]$number, $pin, [string]$role) {
  $disk = Get-Disk -Number $number -ErrorAction Stop
  if (-not [string]::Equals("$($disk.UniqueId)".Trim(), $pin.UniqueId, [StringComparison]::OrdinalIgnoreCase) -or
      -not [string]::Equals("$($disk.SerialNumber)".Trim(), $pin.Serial, [StringComparison]::Ordinal) -or
      [uint64]$disk.Size -ne [uint64]$pin.Size) {
    throw "The $role disk is not the one chosen when this was queued, so nothing was written"
  }
  return $disk
}
function Assert-SakNoBitLocker {
  $namespace = 'root/CIMV2/Security/MicrosoftVolumeEncryption'
  if (-not (Get-CimInstance -Namespace 'root/CIMV2/Security' -ClassName '__NAMESPACE' -ErrorAction Stop |
            Where-Object { $_.Name -eq 'MicrosoftVolumeEncryption' })) { return }
  $encrypted = @(Get-CimInstance -Namespace $namespace -ClassName Win32_EncryptableVolume -ErrorAction Stop |
    Where-Object { $_.ConversionStatus -ne 0 -or $_.ProtectionStatus -ne 0 })
  foreach ($partition in @(Get-Partition -DiskNumber $sakSourceDisk -ErrorAction Stop)) {
    foreach ($volume in $encrypted) {
      if (@($partition.AccessPaths) -contains "$($volume.DeviceID)") {
        throw ('{0} is encrypted with BitLocker, so nothing was written. Turn BitLocker off for this volume, migrate, and turn it on again on the new drive.' -f $(if ($volume.DriveLetter) { $volume.DriveLetter } else { 'A volume on the source disk' }))
      }
    }
  }
}
function Assert-SakMigrationStart {
  $source = Assert-SakPinnedDisk $sakSourceDisk $sakSourcePin 'source'
  $target = Assert-SakPinnedDisk $sakTargetDisk $sakTargetPin 'target'
  if (-not ($source.IsBoot -or $source.IsSystem)) { throw 'The source disk is not the disk Windows is running from' }
  if ($target.IsBoot -or $target.IsSystem) { throw 'The target disk is the disk Windows is running from' }
  if ($target.IsReadOnly) { throw 'The target disk is read-only' }
  if ("$($target.BusType)" -eq 'Spaces') { throw 'The target disk is part of Storage Spaces' }
  if ($source.LogicalSectorSize -ne $target.LogicalSectorSize) {
    throw ('The source disk uses {0}-byte sectors and the target {1}-byte sectors. A file system cannot move between them.' -f $source.LogicalSectorSize, $target.LogicalSectorSize)
  }
  $live = @(Get-Partition -DiskNumber $sakSourceDisk -ErrorAction Stop | Sort-Object Offset)
  if ($live.Count -ne $sakPartitions.Count) { throw 'The source disk''s partitions changed since this was queued, so nothing was written' }
  for ($i = 0; $i -lt $live.Count; ++$i) {
    if ([int64]$live[$i].Offset -ne $sakPartitions[$i].SourceOffset -or [int64]$live[$i].Size -ne $sakPartitions[$i].SourceSize) {
      throw 'The source disk''s partitions changed since this was queued, so nothing was written'
    }
    $sakPartitions[$i].SourceNumber = [uint32]$live[$i].PartitionNumber
    $sakPartitions[$i].VolumePath = @(@($live[$i].AccessPaths) | Where-Object { $_ -like '\\?\Volume*' }) | Select-Object -First 1
  }
  Assert-SakNoBitLocker
}

function Open-SakSnapshots {
  $wanted = @($sakPartitions | Where-Object { $_.Snapshot })
  $unsnapped = @($sakPartitions | Where-Object { $_.Kind -eq 'Data' -and -not $_.Snapshot })
  $reason = $null
  try {
    $set = New-Object SakMigration.SnapshotSet
    $script:sakSnapshotSet = $set
    $devices = $set.Create([string[]]@($wanted | ForEach-Object { [string]$_.VolumePath }))
    for ($i = 0; $i -lt $wanted.Count; ++$i) {
      $wanted[$i].Device = $devices[$i]
      Write-Output ('Snapshot of volume {0}: {1}' -f $wanted[$i].SourceNumber, $devices[$i])
    }
    foreach ($writer in $set.FailedWriters) { Write-Output "WARNING: VSS writer $writer reported a failure. Its data is copied as it stood on disk." }
  } catch { $reason = $_.Exception.GetBaseException().Message }
  if ($reason -and -not $sakAllowNoSnapshot) {
    throw "Windows could not take a snapshot of the running disk: $reason. Nothing was written to the target. Check that the Volume Shadow Copy service can start and that each volume has free space, or run the wizard again and choose to copy without a snapshot."
  }
  if ($reason -and $sakRoute -eq 'Image') {
    throw "Windows could not take a snapshot of the running disk: $reason. Nothing was written to the target. A smaller target is filled from a snapshot and has no other way."
  }
  if ($reason) {
    Write-Output "WARNING: no snapshot: $reason. Copying the running disk as you agreed. Files that change during the copy can be inconsistent on the target, and it may not start."
    $wanted | ForEach-Object { $_.Device = $null }
  }
  if ($unsnapped.Count -gt 0) {
    if (-not $sakAllowNoSnapshot) {
      throw "Volume $($unsnapped[0].SourceNumber) ($($unsnapped[0].FileSystem)) cannot have a snapshot, so it would be copied while in use. Nothing was written to the target. Run the wizard again and choose to copy without a snapshot, or take that volume off the source disk."
    }
    Write-Output 'WARNING: a volume with no snapshot support is copied while in use, as you agreed.'
  }
}
function Assert-SakWindowsFits {
  $windows = $sakPartitions | Where-Object { $_.Kind -eq 'Windows' } | Select-Object -First 1
  if ($sakRoute -ne 'Image') { return }
  $volume = [SakMigration.Io]::OpenRead($windows.Device)
  try { $used = [SakMigration.Copy]::UsedBytes($volume) } finally { $volume.Dispose() }
  Write-Output ('Windows volume in use: {0} bytes; room on the target: {1} bytes' -f $used, $windows.InitialSize)
  if ($used -gt $windows.InitialSize) { throw 'The Windows volume grew past what this target was planned to hold, so nothing was written' }
}

# A partition that will be formatted must not get a letter while raw, or Windows offers on screen to
# format it. Taken off again in Set-SakFormattedPartitionVisible.
function New-SakLayoutEntry($from, [int64]$offset, [int64]$length, [bool]$hideRaw) {
  $entry = New-Object SakMigration.LayoutEntry
  $entry.Offset = $offset
  $entry.Length = $length
  $entry.PartitionId = [Guid]::NewGuid()
  $entry.GptType = $from.GptType
  $entry.Attributes = $from.Attributes
  $entry.Name = $from.Name
  $entry.MbrType = $from.MbrType
  $entry.Boot = $from.Boot
  if ($hideRaw) {
    $entry.Attributes = [uint64]$entry.Attributes -bor [uint64]'9223372036854775808'
    if ($sakScheme -eq 'MBR') { $entry.MbrType = 0x17 }
  }
  return $entry
}
function Write-SakTargetLayout {
  $script:sakTargetTouched = $true
  Set-Disk -Number $sakTargetDisk -IsOffline $true -ErrorAction Stop
  $script:sakTargetOffline = $true
  if (-not (Get-Disk -Number $sakTargetDisk -ErrorAction Stop).IsOffline) {
    throw 'Could not take the target disk offline; a program may be holding it'
  }
  $handle = [SakMigration.Io]::OpenWrite("\\.\PhysicalDrive$sakTargetDisk")
  try {
    $sourceHandle = [SakMigration.Io]::OpenRead("\\.\PhysicalDrive$sakSourceDisk")
    try { $live = [SakMigration.Layout]::Read($sourceHandle) } finally { $sourceHandle.Dispose() }
    $layout = New-Object SakMigration.DiskLayout
    $layout.Gpt = $sakScheme -eq 'GPT'
    $layout.Signature = [SakMigration.Layout]::NewSignature()
    $layout.DiskId = [Guid]::NewGuid()
    foreach ($p in $sakPartitions) {
      $from = $live.Entries | Where-Object { $_.Offset -eq $p.SourceOffset } | Select-Object -First 1
      if (-not $from) { throw "Source partition at byte $($p.SourceOffset) is missing from the disk's layout" }
      $layout.Entries.Add((New-SakLayoutEntry $from $p.TargetOffset $p.InitialSize ($sakRoute -eq 'Image' -and $p.Kind -eq 'Windows')))
      if ($sakRoute -eq 'Image' -and $p.Kind -eq 'Windows') {
        $scratch = New-SakLayoutEntry $from $sakScratchOffset $sakScratchSize $true
        $scratch.Boot = $false
        $layout.Entries.Add($scratch)
      }
    }
    $zero = New-Object byte[] 1048576
    [SakMigration.Io]::Write($handle, 0, $zero, $zero.Length)
    [SakMigration.Io]::Write($handle, [int64]$sakTargetPin.Size - $zero.Length, $zero, $zero.Length)
    [SakMigration.Layout]::Create($handle, $layout.Gpt, $layout.Signature, $layout.DiskId)
    [SakMigration.Layout]::Write($handle, $layout)
    $back = [SakMigration.Layout]::Read($handle)
    foreach ($written in $layout.Entries) {
      if (-not ($back.Entries | Where-Object { $_.Offset -eq $written.Offset -and $_.Length -eq $written.Length })) {
        throw "The target's partition table did not take the partition at byte $($written.Offset)"
      }
    }
    Write-Output ('New layout written: {0} partitions, new disk identity' -f $layout.Entries.Count)
  } finally { $handle.Dispose() }
}

function Copy-SakPartition($p) {
  $target = [SakMigration.Io]::OpenWrite("\\.\PhysicalDrive$sakTargetDisk")
  try {
    if ($p.Device) {
      $source = [SakMigration.Io]::OpenRead($p.Device)
      $sourceBase = [int64]0
      $ntfs = "$($p.FileSystem)" -eq 'NTFS'
      $ranges = if ($ntfs) { [SakMigration.Copy]::UsedRanges($source) } else { [SakMigration.Copy]::Whole($p.SourceSize) }
    } else {
      $source = [SakMigration.Io]::OpenRead("\\.\PhysicalDrive$sakSourceDisk")
      $sourceBase = $p.SourceOffset
      $ranges = [SakMigration.Copy]::Whole($p.SourceSize)
    }
    try {
      [SakMigration.Copy]::Run($source, $sourceBase, $target, $p.TargetOffset, $ranges, "$($p.Kind) partition")
      if ($p.Device -and $ntfs) { [SakMigration.Copy]::BackupBootSector($source, $target, $p.TargetOffset) }
      # A running volume with no snapshot keeps changing, so comparing it would only report that.
      if ($p.Device -or $p.Kind -ne 'Windows') {
        [SakMigration.Copy]::Verify($source, $sourceBase, $target, $p.TargetOffset, $ranges, $sakVerify -like 'Full*')
        Write-Output "$($p.Kind) partition: verified ($sakVerify)"
      } else {
        Write-Output "$($p.Kind) partition: not compared, because it is running with no snapshot"
      }
    } finally { $source.Dispose() }
  } finally { $target.Dispose() }
}
function Copy-SakPartitions {
  foreach ($p in $sakPartitions) {
    if ($sakRoute -eq 'Image' -and $p.Kind -eq 'Windows') { continue }
    Copy-SakPartition $p
  }
}
function Set-SakTargetOnline {
  Set-Disk -Number $sakTargetDisk -IsOffline $false -ErrorAction Stop
  $script:sakTargetOffline = $false
  Update-HostStorageCache -ErrorAction Stop
  $deadline = (Get-Date).AddMinutes(2)
  while ((Get-Date) -lt $deadline) {
    $waiting = @(Get-Partition -DiskNumber $sakTargetDisk -ErrorAction Stop | Where-Object {
      "$($_.Type)" -ne 'Reserved' -and -not (@($_.AccessPaths) | Where-Object { $_ -like '\\?\Volume*' }) })
    if ($waiting.Count -eq 0) { return }
    Start-Sleep -Seconds 1
  }
  throw 'The target disk''s volumes did not appear after it came online'
}
function Get-SakTargetPartition([int64]$offset) {
  $partition = Get-Partition -DiskNumber $sakTargetDisk -ErrorAction Stop | Where-Object { [int64]$_.Offset -eq $offset } | Select-Object -First 1
  if (-not $partition) { throw "The target has no partition at byte $offset" }
  return $partition
}
function Grow-SakWindows {
  $windows = $sakPartitions | Where-Object { $_.Kind -eq 'Windows' } | Select-Object -First 1
  $partition = Get-SakTargetPartition $windows.TargetOffset
  $room = Get-PartitionSupportedSize -DiskNumber $sakTargetDisk -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
  $wanted = [Math]::Min([int64]$room.SizeMax, $windows.FinalSize)
  if ($wanted -gt [int64]$partition.Size) {
    Resize-Partition -DiskNumber $sakTargetDisk -PartitionNumber $partition.PartitionNumber -Size $wanted -ErrorAction Stop
    Write-Output ('Windows partition now {0} bytes' -f $wanted)
  }
}

function Invoke-SakDism([string[]]$toolArgs) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $tail = New-Object System.Collections.ArrayList
  $shown = -10
  try {
    & (Join-Path $script:sakSystem32 'dism.exe') /English @toolArgs 2>&1 | ForEach-Object {
      $line = "$_"
      if ($line -match '(\d+)\.\d%') {
        if ([int]$Matches[1] -ge $shown + 10) { $shown = [int]$Matches[1]; [Console]::Out.WriteLine("DISM: $shown%") }
      } elseif ($line.Trim()) { [void]$tail.Add($line) }
    }
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous }
  if ($code -ne 0) { throw ('DISM failed with exit code {0}: {1}' -f $code, (($tail | Select-Object -Last 4) -join ' ')) }
}
function Set-SakFormattedPartitionVisible($partition) {
  if ($sakScheme -eq 'GPT') {
    Set-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -NoDefaultDriveLetter $false -ErrorAction Stop
  } else {
    Set-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -MbrType 7 -ErrorAction Stop
  }
}
function Invoke-SakImageRoute {
  $windows = $sakPartitions | Where-Object { $_.Kind -eq 'Windows' } | Select-Object -First 1
  $winPartition = Get-SakTargetPartition $windows.TargetOffset
  $scratchPartition = Get-SakTargetPartition $sakScratchOffset
  $label = "$((Get-Volume -Partition (Get-Partition -DiskNumber $sakSourceDisk -PartitionNumber $windows.SourceNumber)).FileSystemLabel)"
  Format-Volume -Partition $winPartition -FileSystem NTFS -NewFileSystemLabel $label -Confirm:$false -ErrorAction Stop | Out-Null
  Format-Volume -Partition $scratchPartition -FileSystem NTFS -NewFileSystemLabel 'SAKSCRATCH' -Confirm:$false -ErrorAction Stop | Out-Null
  Set-SakFormattedPartitionVisible $winPartition
  $winLetter = Add-SakTargetLetter $winPartition
  $scratchLetter = Add-SakTargetLetter $scratchPartition
  $snapshotLetter = Get-SakFreeBootLetter 'R'
  $device = $windows.Device -replace '^\\\\\?\\GLOBALROOT', ''
  [SakMigration.Io]::MapDrive("${snapshotLetter}:", $device)
  $script:sakMappedLetter = "${snapshotLetter}:"
  $script:sakMappedDevice = $device
  $image = "${scratchLetter}:\sak-migrate.wim"
  $verifyArgs = if ($sakVerify -like 'Full*') { @('/Verify', '/CheckIntegrity') } else { @() }
  # DISM's own list, less Windows\CSC: the Offline Files cache can hold work that has not synced.
  $config = "${scratchLetter}:\sak-migrate.ini"
  Set-Content -LiteralPath $config -Encoding ASCII -Value @('[ExclusionList]', '\$ntfs.log', '\hiberfil.sys', '\pagefile.sys', '\swapfile.sys', '\System Volume Information', '\RECYCLER', '[CompressionExclusionList]', '*.mp3', '*.zip', '*.cab', '\WINDOWS\inf\*.pnf')
  Write-Output 'Capturing Windows from its snapshot to a temporary image on the target. DISM first reads the details of every file, which can take a long while with no progress shown.'
  # Files in the Compact state are stored whole in an image, so a plain apply makes them larger than
  # the volume they came from.
  $applyCompact = if ([SakMigration.Io]::IsExternallyBacked((Join-Path $script:sakSystem32 'cmd.exe'))) { @('/Compact') } else { @() }
  Invoke-SakDism (@('/Capture-Image', "/ImageFile:$image", "/CaptureDir:${snapshotLetter}:\", '/Name:SAK Migrate OS', '/Compress:fast', '/EA', "/ConfigFile:$config") + $verifyArgs)
  [SakMigration.Io]::UnmapDrive($script:sakMappedLetter, $script:sakMappedDevice)
  $script:sakMappedLetter = $null
  Write-Output 'Applying the image to the new Windows partition'
  if ($applyCompact) { Write-Output 'This Windows is in the Compact state, so the new copy is applied the same way.' }
  Invoke-SakDism (@('/Apply-Image', "/ImageFile:$image", '/Index:1', "/ApplyDir:${winLetter}:\", '/EA') + $applyCompact + $verifyArgs)
  Remove-PartitionAccessPath -DiskNumber $sakTargetDisk -PartitionNumber $scratchPartition.PartitionNumber -AccessPath "${scratchLetter}:\" -ErrorAction Stop
  Remove-Partition -DiskNumber $sakTargetDisk -PartitionNumber $scratchPartition.PartitionNumber -Confirm:$false -ErrorAction Stop
  Update-HostStorageCache -ErrorAction Stop
}

# Windows Recovery remembers its partition by the old disk's identity. This points the new Windows at
# the copy on its own disk. A failure here leaves a Windows that starts, so it is reported, not fatal.
function Set-SakTargetRecovery([string]$windowsRoot) {
  $recovery = $sakPartitions | Where-Object { $_.Kind -eq 'Recovery' } | Select-Object -First 1
  if (-not $recovery) { return }
  $letter = Add-SakTargetLetter (Get-SakTargetPartition $recovery.TargetOffset)
  $image = "${letter}:\Recovery\WindowsRE"
  if (-not (Test-Path -LiteralPath (Join-Path $image 'winre.wim'))) {
    Write-Output 'The recovery partition holds no Windows Recovery image, so there is nothing to point Windows at.'
    return
  }
  try { Invoke-SakTool 'reagentc.exe' @('/setreimage', '/path', $image, '/target', $windowsRoot) }
  catch { Write-Output "WARNING: Windows Recovery was not pointed at the new disk: $($_.Exception.Message)" }
}
function Set-SakTargetBoot {
  $disk = Get-Disk -Number $sakTargetDisk -ErrorAction Stop
  $windows = $sakPartitions | Where-Object { $_.Kind -eq 'Windows' } | Select-Object -First 1
  $winPartition = Get-SakTargetPartition $windows.TargetOffset
  $winLetter = Add-SakTargetLetter $winPartition
  $windowsRoot = "${winLetter}:\Windows"
  if (-not (Test-Path -LiteralPath "$windowsRoot\System32\config\SYSTEM")) { throw "The target has no Windows at $windowsRoot" }
  $mode = if ($sakScheme -eq 'GPT') { 'UEFI' } else { 'BIOS' }
  $system = $sakPartitions | Where-Object { $_.Kind -eq 'System' } | Select-Object -First 1
  $systemPartition = if ($system) { Get-SakTargetPartition $system.TargetOffset } else { $winPartition }
  Invoke-SakTargetBootFiles $systemPartition $windowsRoot $mode
  $letters = @{}
  foreach ($p in $sakPartitions) {
    if ($p.Letter) { $letters[$p.Letter] = Get-SakTargetPartition $p.TargetOffset }
  }
  Set-SakTargetDriveLetters $windowsRoot $disk $letters
  Set-SakTargetRecovery $windowsRoot
}
function Close-SakMigration {
  if ($script:sakHiveLoaded) {
    try { Invoke-SakTool 'reg.exe' @('unload', 'HKLM\SAK_MIGRATE_TARGET') } catch { Write-Output "WARNING: $($_.Exception.Message)" }
  }
  if ($script:sakMappedLetter) {
    try { [SakMigration.Io]::UnmapDrive($script:sakMappedLetter, $script:sakMappedDevice) } catch { Write-Output "WARNING: $($_.Exception.Message)" }
  }
  if ($script:sakTargetTouched) {
    try {
      if ($script:sakTargetOffline) { Set-Disk -Number $sakTargetDisk -IsOffline $false -ErrorAction Stop }
      Remove-SakTargetLetters $sakTargetDisk
    } catch { Write-Output "WARNING: $($_.Exception.Message)" }
  }
  if ($script:sakSnapshotSet) {
    try { $script:sakSnapshotSet.Release(); Write-Output 'Snapshots released' } catch { Write-Output "WARNING: $($_.Exception.Message)" }
  }
}
try {
  Assert-SakMigrationStart
  Open-SakSnapshots
  Assert-SakWindowsFits
  Write-SakTargetLayout
  Copy-SakPartitions
  Set-SakTargetOnline
  if ($sakRoute -eq 'Image') { Invoke-SakImageRoute }
  Grow-SakWindows
  Set-SakTargetBoot
} finally { Close-SakMigration }
Get-Partition -DiskNumber $sakTargetDisk -ErrorAction Stop | Sort-Object Offset | ForEach-Object {
  Write-Output ('Target partition {0}: {1}, {2} bytes at {3}, active={4}' -f $_.PartitionNumber, $_.Type, $_.Size, $_.Offset, $_.IsActive)
}
Write-Output 'Migrate OS finished. The new disk has its own identity, so both disks can stay attached. Choose it in the BIOS or UEFI boot order.'
Write-Output 'Windows Recovery on the new disk stays off until you run reagentc /enable once from the new disk.'
