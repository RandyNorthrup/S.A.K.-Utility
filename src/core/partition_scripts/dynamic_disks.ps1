# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Get-Disk does not list a dynamic disk, so a scan built on it hid a real disk from the person
# looking at it. Win32_DiskDrive lists every disk, and a disk it lists and Get-Disk does not is looked
# at directly. Nothing here changes anything, and a standard user may run it: the handles ask for no
# access, and these four questions need none.
# The probe's types ship in sak_script_types.dll, loaded ahead of this script; nothing is compiled.
function Initialize-SakDynamicDiskProbe {
if (-not ('Sak.DynamicDiskProbe' -as [type])) {
  throw "S.A.K.'s script types (sak_script_types.dll) are not loaded; the dynamic-disk probe is one of them"
}
}

# Each mounted volume and the disks it lies on. A volume Windows will not answer for is left out: the
# disk then shows no volume there, and a conversion needs one.
function Get-SakVolumeRuns {
  $found = @()
  $volumes = @()
  try {
    $volumes = @(Get-Volume -ErrorAction Stop | Where-Object { $_.Path })
  } catch {
    return $found
  }
  foreach ($volume in $volumes) {
    try {
      $runs = @([Sak.DynamicDiskProbe]::Runs("$($volume.Path)".TrimEnd('\')))
    } catch {
      continue
    }
    if ($runs.Count -gt 0) { $found += [pscustomobject]@{ Volume = $volume; Runs = $runs } }
  }
  return $found
}

# Which disks the running Windows lies on. If that cannot be found out, a dynamic disk reads as one of
# them, and the checks refuse it: Apply's own scan, which runs as administrator, asks again.
function Get-SakRunningDiskNumbers {
  param([object[]]$VolumeRuns, [int[]]$Candidates)
  $boot = @()
  $system = @()
  $systemRoot = "$env:SystemDrive".TrimEnd(':', '\')
  try {
    $flagged = @(Get-CimInstance -ClassName Win32_Volume -Filter 'SystemVolume=TRUE OR BootVolume=TRUE' -ErrorAction Stop)
  } catch {
    return [pscustomobject]@{ Boot = $Candidates; System = $Candidates }
  }
  foreach ($entry in $VolumeRuns) {
    $path = "$($entry.Volume.Path)"
    $disks = @($entry.Runs | ForEach-Object { $_.Disk })
    if ("$($entry.Volume.DriveLetter)" -eq $systemRoot) { $boot += $disks }
    foreach ($flag in $flagged) {
      if ("$($flag.DeviceID)" -ne $path) { continue }
      if ($flag.BootVolume) { $boot += $disks }
      if ($flag.SystemVolume) { $system += $disks }
    }
  }
  return [pscustomobject]@{ Boot = @($boot | Select-Object -Unique); System = @($system | Select-Object -Unique) }
}

# One row per volume on the disk, in the shape Get-Partition gives. A dynamic disk has no partitions
# of its own to list: the table on it holds one entry for the whole disk.
function Get-SakDynamicVolumes {
  param([int]$DiskNumber, [object[]]$VolumeRuns, [object]$Running)
  $number = 0
  foreach ($entry in $VolumeRuns) {
    $here = @($entry.Runs | Where-Object { $_.Disk -eq $DiskNumber })
    if ($here.Count -eq 0) { continue }
    $number++
    $first = [uint64]($here | ForEach-Object { $_.Offset } | Sort-Object | Select-Object -First 1)
    $total = [uint64]0
    foreach ($run in $here) { $total += [uint64]$run.Length }
    [pscustomobject]@{
      PartitionNumber = $number
      Guid = ''
      Type = 'Dynamic'
      GptType = ''
      Offset = $first
      Size = $total
      DriveLetter = "$($entry.Volume.DriveLetter)"
      AccessPaths = @("$($entry.Volume.Path)")
      IsActive = $false
      IsBoot = @($Running.Boot) -contains $DiskNumber
      IsSystem = @($Running.System) -contains $DiskNumber
      IsReadOnly = $false
    }
  }
}

# The disks Get-Disk leaves out, in the shape Get-Disk gives, with IsDynamic set.
function Get-SakDynamicDisks {
  $listed = @(Get-Disk | ForEach-Object { [int]$_.Number })
  # Not being able to ask leaves the list as Get-Disk gave it. A scan that shows the other disks is
  # better than none, and a disk not shown cannot be changed.
  try {
    $hidden = @(Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop | Where-Object { $listed -notcontains [int]$_.Index })
  } catch {
    return
  }
  if ($hidden.Count -eq 0) { return }
  Initialize-SakDynamicDiskProbe
  $facts = @{}
  foreach ($drive in $hidden) {
    try {
      $probe = [Sak.DynamicDiskProbe]::Disk([int]$drive.Index)
    } catch {
      continue
    }
    if ($probe.IsDynamic) { $facts[[int]$drive.Index] = $probe }
  }
  if ($facts.Count -eq 0) { return }
  $volumeRuns = @(Get-SakVolumeRuns)
  $running = Get-SakRunningDiskNumbers -VolumeRuns $volumeRuns -Candidates @($facts.Keys)
  foreach ($drive in $hidden) {
    $number = [int]$drive.Index
    if (-not $facts.ContainsKey($number)) { continue }
    $probe = $facts[$number]
    $spans = @($volumeRuns | Where-Object {
        $disks = @($_.Runs | ForEach-Object { $_.Disk } | Select-Object -Unique)
        ($disks -contains $number) -and $disks.Count -gt 1
      }).Count -gt 0
    [pscustomobject]@{
      Number = $number
      UniqueId = "dynamic:$($probe.Id)"
      FriendlyName = "$($drive.Model)"
      SerialNumber = "$($drive.SerialNumber)".Trim()
      HealthStatus = if ("$($drive.Status)" -eq 'OK') { 'Healthy' } else { "$($drive.Status)" }
      OperationalStatus = 'Online'
      BusType = "$($drive.InterfaceType)"
      MediaType = ''
      PartitionStyle = $probe.Style
      Size = [uint64]$probe.LengthBytes
      IsBoot = @($running.Boot) -contains $number
      IsSystem = @($running.System) -contains $number
      IsReadOnly = -not $probe.IsWritable
      IsOffline = $false
      IsDynamic = $true
      VolumeSpansDisks = $spans
      DynamicPartitions = @(Get-SakDynamicVolumes -DiskNumber $number -VolumeRuns $volumeRuns -Running $running)
    }
  }
}
