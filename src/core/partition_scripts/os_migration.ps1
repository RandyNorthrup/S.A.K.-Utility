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
  # An MBR target gets BIOS boot code at the end: checked before anything is written.
  if ($sakScheme -eq 'MBR') { Assert-SakBootsectPresent }
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
  Write-SakShadowStorageReport (Get-SakShadowStorage "$($windows.VolumePath)") "$($windows.Letter)" ([uint64]$used)
}

# The image route captures from a snapshot Windows keeps in the volume's shadow-copy storage, and
# Windows deletes it when that storage fills (operation proof, 2026-11-12). The storage is reported
# before anything is written, with a warning when it is small; S.A.K. never changes it.
function Get-SakShadowStorage([string]$volumePath) {
  try { $all = @(Get-CimInstance -ClassName Win32_ShadowStorage -ErrorAction Stop) } catch { return $null }
  return @($all | Where-Object { "$($_.Volume.DeviceID)" -eq $volumePath }) | Select-Object -First 1
}
# partition_os_migration.cpp's shadowStorageTooSmallForCapture: under a quarter of the bytes captured.
function Test-SakShadowStorageSmall([uint64]$max, [uint64]$used, [uint64]$windowsUsed) {
  if ($max -eq [uint64]::MaxValue) { return $false }
  $left = if ($max -gt $used) { $max - $used } else { [uint64]0 }
  return $left -lt [Math]::Floor($windowsUsed / 4)
}
function Write-SakShadowStorageReport($storage, [string]$letter, [uint64]$windowsUsed) {
  $volume = if ($letter) { "${letter}:" } else { 'the Windows volume' }
  if (-not $storage) { Write-Output "Shadow-copy storage for $volume could not be read; Windows sizes it when it keeps the snapshot."; return }
  $max = [uint64]$storage.MaxSpace
  $used = [uint64]$storage.UsedSpace
  $limit = if ($max -eq [uint64]::MaxValue) { 'no limit' } else { 'up to {0:N1} GB' -f ($max / 1GB) }
  Write-Output ('Shadow-copy storage for {0} {1}, {2:N1} GB in use' -f $volume, $limit, ($used / 1GB))
  if (Test-SakShadowStorageSmall $max $used $windowsUsed) {
    Write-Output ('WARNING: that leaves {0:N1} GB for the capture, less than a quarter of the {1:N1} GB in use on {2}. While DISM reads every file, Windows keeps the old copy of each block that changes on {2} there, and if it fills, Windows deletes the snapshot and the capture stops (Windows Update can change several GB). Let Windows Update finish first, or give it more room: vssadmin resize shadowstorage /for={2} /on={2} /maxsize=10%' -f ((($max - [Math]::Min($max, $used))) / 1GB), ($windowsUsed / 1GB), $volume)
  }
}

# Why a capture stopped, when Windows says: DISM reports only its own code. 433 (ERROR_NO_SUCH_DEVICE)
# with a volsnap 33 or 36 for the volume is the snapshot Windows deleted; 225 (ERROR_VIRUS_INFECTED)
# is an antivirus blocking a file, which dism.log names. Empty when neither says more.
function Get-SakVolsnapLimitEvents([datetime]$since) {
  @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'volsnap'; Id = 33, 36; StartTime = $since } -ErrorAction SilentlyContinue)
}
function Get-SakDismFileErrors([string]$logPath, [datetime]$since) {
  if (-not $logPath -or -not (Test-Path -LiteralPath $logPath)) { return }
  $stream = New-Object IO.FileStream($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  $reader = New-Object IO.StreamReader($stream)
  try { $lines = $reader.ReadToEnd() -split "`r?`n" } finally { $reader.Dispose() }
  $pattern = '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}), Error\s+DISM\s+DISM WIM Provider: PID=\d+ (.+) \(HRESULT=(0x[0-9A-Fa-f]{8})\) - CWimManager::WimProviderMsgLogCallback'
  foreach ($line in $lines) {
    if ($line -notmatch $pattern) { continue }
    $when = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture)
    if ($when -ge $since) { [pscustomobject]@{ Time = $when; Path = $Matches[2]; HResult = $Matches[3] } }
  }
}
function Get-SakCaptureFailureMessage([int]$Code, [datetime]$Since, [string]$SourceLetter, [string]$SnapshotLetter, [string]$DismLog, [object[]]$VolsnapEvents) {
  $volume = if ($SourceLetter) { "${SourceLetter}:" } else { 'the Windows volume' }
  if ($Code -eq 433) {
    $deleted = @($VolsnapEvents | Where-Object { $_ -and @(33, 36) -contains [int]$_.Id -and $_.TimeCreated -ge $Since -and "$($_.Message)" -match ('volume ' + [regex]::Escape($volume)) })
    if ($deleted.Count -eq 0) { return '' }
    return ('Windows deleted the snapshot S.A.K. was copying from because shadow-copy storage on {0} was full (Windows Update or restore points can use it). Nothing on {0} changed. Try again after Windows Update finishes, enlarge shadow storage (vssadmin resize shadowstorage /for={0} /on={0} /maxsize=10%), or migrate to a target at least as large as the Windows partition, which is copied block by block instead of through an image.' -f $volume)
  }
  if ($Code -eq 225) {
    $blocked = @(Get-SakDismFileErrors $DismLog $Since | Where-Object { $_.HResult -eq '0x800700E1' }) | Select-Object -First 1
    $file = if ($blocked) { [string]$blocked.Path } else { '' }
    if ($file -and $SnapshotLetter -and $file.StartsWith("${SnapshotLetter}:\", [StringComparison]::OrdinalIgnoreCase)) { $file = $volume + $file.Substring(2) }
    $what = if ($file) { $file } else { "a file (DISM names it in $DismLog)" }
    return ('Your antivirus (Microsoft Defender on most PCs) blocked {0} while DISM was capturing Windows, so the capture stopped. Nothing on {1} changed. Look the file up in Windows Security''s protection history: if it is a threat, deal with it before you migrate this Windows; if you trust it, allow it there, then run Migrate OS again.' -f $what, $volume)
  }
  return ''
}

# A partition that will be formatted must not get a letter while raw, or Windows offers on screen to
# format it. GPT has an attribute for that. MBR does not, and a hidden type leaves it with no volume at
# all, until Format-SakTargetVolume changes the type just before formatting it.
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
      $check = New-Object SakMigration.CopyCheck -ArgumentList ([int64[]]$ranges), ($sakVerify -like 'Full*')
      [SakMigration.Copy]::Run($source, $sourceBase, $target, $p.TargetOffset, $ranges, "$($p.Kind) partition", $check)
      if ($p.Device -and $ntfs) { [SakMigration.Copy]::BackupBootSector($source, $target, $p.TargetOffset) }
      [SakMigration.Copy]::Verify($target, $p.TargetOffset, $ranges, $check)
      Write-Output "$($p.Kind) partition: verified ($sakVerify)"
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
      "$($_.Type)" -ne 'Reserved' -and $_.MbrType -ne 0x17 -and -not (@($_.AccessPaths) | Where-Object { $_ -like '\\?\Volume*' }) })
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
  $script:sakDismExit = $code
  if ($code -ne 0) { throw ('DISM failed with exit code {0}: {1}' -f $code, (($tail | Select-Object -Last 4) -join ' ')) }
}
function Format-SakTargetVolume($partition, [string]$label) {
  if ($sakScheme -eq 'MBR') {
    Set-Partition -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -MbrType 7 -ErrorAction Stop
    $deadline = (Get-Date).AddMinutes(2)
    while (-not (@((Get-SakTargetPartition $partition.Offset).AccessPaths) | Where-Object { $_ -like '\\?\Volume*' })) {
      if ((Get-Date) -gt $deadline) { throw "The partition at byte $($partition.Offset) got no volume when its type changed" }
      Start-Sleep -Milliseconds 300
    }
    $partition = Get-SakTargetPartition $partition.Offset
  }
  Format-Volume -Partition $partition -FileSystem NTFS -NewFileSystemLabel $label -Confirm:$false -ErrorAction Stop | Out-Null
}
function Invoke-SakImageRoute {
  $windows = $sakPartitions | Where-Object { $_.Kind -eq 'Windows' } | Select-Object -First 1
  $winPartition = Get-SakTargetPartition $windows.TargetOffset
  $scratchPartition = Get-SakTargetPartition $sakScratchOffset
  $label = "$((Get-Volume -Partition (Get-Partition -DiskNumber $sakSourceDisk -PartitionNumber $windows.SourceNumber)).FileSystemLabel)"
  Format-SakTargetVolume $winPartition $label
  Format-SakTargetVolume $scratchPartition 'SAKSCRATCH'
  if ($sakScheme -eq 'GPT') {
    Set-Partition -DiskNumber $sakTargetDisk -PartitionNumber $winPartition.PartitionNumber -NoDefaultDriveLetter $false -ErrorAction Stop
  }
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
  $captureStart = (Get-Date).AddSeconds(-5)
  $script:sakDismExit = 0
  try {
    Invoke-SakDism (@('/Capture-Image', "/ImageFile:$image", "/CaptureDir:${snapshotLetter}:\", '/Name:SAK Migrate OS', '/Compress:fast', '/EA', "/ConfigFile:$config") + $verifyArgs)
  } catch {
    $explained = Get-SakCaptureFailureMessage -Code $script:sakDismExit -Since $captureStart -SourceLetter "$($windows.Letter)" -SnapshotLetter $snapshotLetter -DismLog (Join-Path $env:SystemRoot 'Logs\DISM\dism.log') -VolsnapEvents (Get-SakVolsnapLimitEvents $captureStart)
    if ($explained) { throw ('{0} (DISM exit code {1})' -f $explained, $script:sakDismExit) }
    throw
  }
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
