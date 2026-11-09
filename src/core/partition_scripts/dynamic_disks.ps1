# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

# Get-Disk does not list a dynamic disk, so a scan built on it hid a real disk from the person
# looking at it. Win32_DiskDrive lists every disk, and a disk it lists and Get-Disk does not is looked
# at directly. Nothing here changes anything, and a standard user may run it: the handles ask for no
# access, and these four questions need none.
# Compiling the probe costs a second or more, every scan, so it is compiled only when a disk needs it.
function Initialize-SakDynamicDiskProbe {
if (-not ('Sak.DynamicDiskProbe' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace Sak {
public sealed class DiskFacts {
    public string Style = "";
    public string Id = "";
    public bool IsDynamic;
    public ulong LengthBytes;
    public bool IsWritable;
}
public sealed class VolumeRun {
    public int Disk;
    public ulong Offset;
    public ulong Length;
}
public static class DynamicDiskProbe {
    private const uint GeometryEx = 0x000700A0;
    private const uint LayoutEx = 0x00070050;
    private const uint Writable = 0x00070024;
    private const uint VolumeExtents = 0x00560000;
    private const uint ShareReadWrite = 3;
    private const uint OpenExisting = 3;
    private const int BufferBytes = 65536;
    private const int LayoutHeaderBytes = 48;
    private const int PartitionEntryBytes = 144;
    private const int PartitionTypeOffset = 32;
    private const int DiskSizeOffset = 24;
    private const int ExtentHeaderBytes = 8;
    private const int ExtentBytes = 24;
    private const byte MbrLdmType = 0x42;
    private static readonly Guid LdmMetadata = new Guid("5808c8aa-7e8f-42e0-85d2-e1e90434cfb3");
    private static readonly Guid LdmData = new Guid("af9b60a0-1431-4f62-bc68-3311714a69ad");
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input,
        uint inputSize, byte[] output, uint outputSize, out uint returned, IntPtr overlapped);
    private static SafeFileHandle Open(string path) {
        SafeFileHandle handle = CreateFile(path, 0, ShareReadWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open " + path);
        return handle;
    }
    private static byte[] Ask(SafeFileHandle handle, uint code, string what) {
        byte[] buffer = new byte[BufferBytes];
        uint returned;
        if (!DeviceIoControl(handle, code, IntPtr.Zero, 0, buffer, BufferBytes, out returned, IntPtr.Zero)) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read " + what);
        }
        return buffer;
    }
    private static bool HasLdmEntry(byte[] layout, bool gpt) {
        uint count = BitConverter.ToUInt32(layout, 4);
        for (uint index = 0; index < count; index++) {
            long entry = LayoutHeaderBytes + (index * PartitionEntryBytes);
            if (entry + PartitionEntryBytes > layout.Length) return false;
            int type = (int)entry + PartitionTypeOffset;
            if (gpt) {
                byte[] raw = new byte[16];
                Array.Copy(layout, type, raw, 0, raw.Length);
                Guid id = new Guid(raw);
                if (id == LdmMetadata || id == LdmData) return true;
            } else if (layout[type] == MbrLdmType) {
                return true;
            }
        }
        return false;
    }
    public static DiskFacts Disk(int number) {
        using (SafeFileHandle handle = Open("\\\\.\\PhysicalDrive" + number)) {
            DiskFacts facts = new DiskFacts();
            facts.LengthBytes = BitConverter.ToUInt64(Ask(handle, GeometryEx, "the disk's size"), DiskSizeOffset);
            byte[] layout = Ask(handle, LayoutEx, "the disk's layout");
            uint style = BitConverter.ToUInt32(layout, 0);
            bool gpt = style == 1;
            facts.Style = gpt ? "GPT" : (style == 0 ? "MBR" : "RAW");
            if (gpt) {
                byte[] raw = new byte[16];
                Array.Copy(layout, 8, raw, 0, raw.Length);
                facts.Id = new Guid(raw).ToString("D");
            } else {
                facts.Id = BitConverter.ToUInt32(layout, 8).ToString("x8");
            }
            facts.IsDynamic = style <= 1 && HasLdmEntry(layout, gpt);
            uint returned;
            facts.IsWritable = DeviceIoControl(handle, Writable, IntPtr.Zero, 0, null, 0, out returned, IntPtr.Zero);
            return facts;
        }
    }
    public static VolumeRun[] Runs(string volumePath) {
        using (SafeFileHandle handle = Open(volumePath)) {
            byte[] extents = Ask(handle, VolumeExtents, "the volume's extents");
            uint count = BitConverter.ToUInt32(extents, 0);
            List<VolumeRun> runs = new List<VolumeRun>();
            for (uint index = 0; index < count; index++) {
                int entry = (int)(ExtentHeaderBytes + (index * ExtentBytes));
                if (entry + ExtentBytes > extents.Length) break;
                VolumeRun run = new VolumeRun();
                run.Disk = (int)BitConverter.ToUInt32(extents, entry);
                run.Offset = BitConverter.ToUInt64(extents, entry + 8);
                run.Length = BitConverter.ToUInt64(extents, entry + 16);
                runs.Add(run);
            }
            return runs.ToArray();
        }
    }
}
}
'@
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
