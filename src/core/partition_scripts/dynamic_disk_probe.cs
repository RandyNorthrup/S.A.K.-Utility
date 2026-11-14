// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// The dynamic-disk probe the storage scan and Migrate OS use (src/core/partition_scripts/
// dynamic_disks.ps1). It used to be compiled by Add-Type into the temporary folder of the
// process that ran the scan, elevated or not; it is built into sak_script_types.dll with the
// scripts' other types (security brief 4a).
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
