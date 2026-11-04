// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Compiled by Add-Type inside the elevated PowerShell that runs Migrate OS. It has to live in that
// process for one reason above the others: a snapshot taken through IVssBackupComponents belongs to
// the process that asked for it, so Windows releases it when that process ends, however it ends.
// Cancel stops the process through its kill-on-close Job, and a finally block never runs.
// C# 5: this is the compiler Windows PowerShell 5.1 has.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace SakMigration {

[StructLayout(LayoutKind.Sequential)]
public struct VssSnapshotProp {
    public Guid SnapshotId; public Guid SnapshotSetId; public int SnapshotsCount;
    public IntPtr DeviceObject; public IntPtr OriginalVolume; public IntPtr OriginatingMachine;
    public IntPtr ServiceMachine; public IntPtr ExposedName; public IntPtr ExposedPath;
    public Guid ProviderId; public int Attributes; public long CreationTimestamp; public int Status;
}
[ComImport, Guid("507C37B4-CF5B-4e95-B0AF-14EB9767467E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IVssAsync {
    [PreserveSig] int Cancel();
    [PreserveSig] int Wait(uint milliseconds);
    [PreserveSig] int QueryStatus(out int status, IntPtr reserved);
}
// vsbackup.h, in vtable order. Slots this code never calls keep their place with an IntPtr shape.
[ComImport, Guid("665c1d5f-c218-414d-a05d-7fef5f9d5c86"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IVssBackupComponents {
    [PreserveSig] int GetWriterComponentsCount(out uint count);
    [PreserveSig] int GetWriterComponents(uint writer, out IntPtr components);
    [PreserveSig] int InitializeForBackup([MarshalAs(UnmanagedType.BStr)] string xml);
    [PreserveSig] int SetBackupState([MarshalAs(UnmanagedType.U1)] bool selectComponents, [MarshalAs(UnmanagedType.U1)] bool bootable, int backupType, [MarshalAs(UnmanagedType.U1)] bool partial);
    [PreserveSig] int InitializeForRestore(IntPtr xml);
    [PreserveSig] int SetRestoreState(int restoreType);
    [PreserveSig] int GatherWriterMetadata(out IVssAsync asyncResult);
    [PreserveSig] int GetWriterMetadataCount(out uint count);
    [PreserveSig] int GetWriterMetadata(uint writer, out Guid instance, out IntPtr metadata);
    [PreserveSig] int FreeWriterMetadata();
    [PreserveSig] int AddComponent(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e);
    [PreserveSig] int PrepareForBackup(out IVssAsync asyncResult);
    [PreserveSig] int AbortBackup();
    [PreserveSig] int GatherWriterStatus(out IVssAsync asyncResult);
    [PreserveSig] int GetWriterStatusCount(out uint count);
    [PreserveSig] int FreeWriterStatus();
    [PreserveSig] int GetWriterStatus(uint writer, out Guid instance, out Guid writerClass, [MarshalAs(UnmanagedType.BStr)] out string name, out int state, out int failure);
    [PreserveSig] int SetBackupSucceeded(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, int f);
    [PreserveSig] int SetBackupOptions(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, IntPtr f);
    [PreserveSig] int SetSelectedForRestore(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, int f);
    [PreserveSig] int SetRestoreOptions(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, IntPtr f);
    [PreserveSig] int SetAdditionalRestores(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, int f);
    [PreserveSig] int SetPreviousBackupStamp(IntPtr a, IntPtr b, int c, IntPtr d, IntPtr e, IntPtr f);
    [PreserveSig] int SaveAsXML(out IntPtr xml);
    [PreserveSig] int BackupComplete(out IVssAsync asyncResult);
    [PreserveSig] int AddAlternativeLocationMapping(IntPtr a, int b, IntPtr c, IntPtr d, IntPtr e, IntPtr f, int g, IntPtr h);
    [PreserveSig] int AddRestoreSubcomponent(IntPtr a, int b, IntPtr c, IntPtr d, IntPtr e, IntPtr f, int g);
    [PreserveSig] int SetFileRestoreStatus(IntPtr a, int b, IntPtr c, IntPtr d, int e);
    [PreserveSig] int AddNewTarget(IntPtr a, int b, IntPtr c, IntPtr d, IntPtr e, IntPtr f, int g, IntPtr h);
    [PreserveSig] int SetRangesFilePath(IntPtr a, int b, IntPtr c, IntPtr d, uint e, IntPtr f);
    [PreserveSig] int PreRestore(out IVssAsync asyncResult);
    [PreserveSig] int PostRestore(out IVssAsync asyncResult);
    [PreserveSig] int SetContext(int context);
    [PreserveSig] int StartSnapshotSet(out Guid setId);
    [PreserveSig] int AddToSnapshotSet([MarshalAs(UnmanagedType.LPWStr)] string volume, Guid provider, out Guid snapshotId);
    [PreserveSig] int DoSnapshotSet(out IVssAsync asyncResult);
    [PreserveSig] int DeleteSnapshots(Guid source, int sourceType, int force, out int deleted, out Guid notDeleted);
    [PreserveSig] int ImportSnapshots(out IVssAsync asyncResult);
    [PreserveSig] int BreakSnapshotSet(Guid setId);
    [PreserveSig] int GetSnapshotProperties(Guid snapshotId, out VssSnapshotProp prop);
    [PreserveSig] int Query(Guid a, int b, int c, out IntPtr d);
    [PreserveSig] int IsVolumeSupported(Guid provider, [MarshalAs(UnmanagedType.LPWStr)] string volume, out int supported);
    [PreserveSig] int DisableWriterClasses(ref Guid classes, uint count);
}

// One snapshot per volume, taken together, held by a thread of its own in the COM multithreaded
// apartment. VSS_CTX_BACKUP is not persistent: Windows releases the set when this process ends.
public sealed class SnapshotSet {
    const int ContextBackup = 0, BackupTypeCopy = 5;
    // Left alone, this writer deletes what Windows lists under FilesNotToSnapshot from the snapshot,
    // and that list holds Outlook's OST mail caches and Windows Update's downloads. A backup program
    // can do without those. A customer's new drive cannot: an OST can hold mail that has not synced.
    static readonly Guid ShadowCopyOptimizationWriter = new Guid("4dc3bdd4-ab48-4d07-adb0-3bee2926fd7f");
    [DllImport("vssapi.dll")] static extern int CreateVssBackupComponentsInternal(out IVssBackupComponents backup);
    [DllImport("vssapi.dll")] static extern void VssFreeSnapshotPropertiesInternal(ref VssSnapshotProp prop);
    [DllImport("ole32.dll")] static extern int CoInitializeSecurity(IntPtr sd, int auth, IntPtr svc, IntPtr res, uint authn, uint imp, IntPtr list, uint caps, IntPtr res3);
    readonly ManualResetEvent created = new ManualResetEvent(false);
    readonly ManualResetEvent release = new ManualResetEvent(false);
    readonly ManualResetEvent finished = new ManualResetEvent(false);
    string[] volumes; string[] devices; string failure; Thread worker;
    public List<string> FailedWriters = new List<string>();
    static void Check(int hr, string what) {
        if (hr < 0) throw new InvalidOperationException(what + " failed with VSS error 0x" + hr.ToString("X8"));
    }
    static void Await(IVssAsync pending, string what) {
        Check(pending.Wait(0xFFFFFFFF), what);
        int status;
        Check(pending.QueryStatus(out status, IntPtr.Zero), what);
        Check(status, what);
        Marshal.ReleaseComObject(pending);
    }
    public string[] Create(string[] volumePaths) {
        volumes = volumePaths;
        devices = new string[volumePaths.Length];
        worker = new Thread(Run);
        worker.SetApartmentState(ApartmentState.MTA);
        worker.IsBackground = true;
        worker.Start();
        created.WaitOne();
        if (failure != null) throw new InvalidOperationException(failure);
        return devices;
    }
    public void Release() {
        if (worker == null) return;
        release.Set();
        finished.WaitOne();
    }
    void TakeSnapshots(IVssBackupComponents b) {
        IVssAsync a;
        Check(b.InitializeForBackup(null), "InitializeForBackup");
        Guid optimization = ShadowCopyOptimizationWriter;
        Check(b.DisableWriterClasses(ref optimization, 1), "DisableWriterClasses");
        Check(b.SetContext(ContextBackup), "SetContext");
        Check(b.SetBackupState(false, false, BackupTypeCopy, false), "SetBackupState");
        Check(b.GatherWriterMetadata(out a), "GatherWriterMetadata"); Await(a, "GatherWriterMetadata");
        Guid setId; Check(b.StartSnapshotSet(out setId), "StartSnapshotSet");
        Guid[] ids = new Guid[volumes.Length];
        for (int i = 0; i < volumes.Length; ++i) Check(b.AddToSnapshotSet(volumes[i], Guid.Empty, out ids[i]), "AddToSnapshotSet " + volumes[i]);
        Check(b.PrepareForBackup(out a), "PrepareForBackup"); Await(a, "PrepareForBackup");
        Check(b.DoSnapshotSet(out a), "DoSnapshotSet"); Await(a, "DoSnapshotSet");
        for (int i = 0; i < volumes.Length; ++i) {
            VssSnapshotProp p; Check(b.GetSnapshotProperties(ids[i], out p), "GetSnapshotProperties");
            devices[i] = Marshal.PtrToStringUni(p.DeviceObject);
            VssFreeSnapshotPropertiesInternal(ref p);
        }
        ReadWriterStatus(b);
    }
    void ReadWriterStatus(IVssBackupComponents b) {
        IVssAsync a; uint count;
        Check(b.GatherWriterStatus(out a), "GatherWriterStatus"); Await(a, "GatherWriterStatus");
        Check(b.GetWriterStatusCount(out count), "GetWriterStatusCount");
        for (uint i = 0; i < count; ++i) {
            Guid instance, writerClass; string name; int state, hr;
            Check(b.GetWriterStatus(i, out instance, out writerClass, out name, out state, out hr), "GetWriterStatus");
            if (hr < 0) FailedWriters.Add(name + " (0x" + hr.ToString("X8") + ")");
        }
    }
    void Run() {
        IVssBackupComponents b = null;
        try {
            // Too late is fine: then something earlier in this process already set it, and VSS has
            // run under that here. Anything VSS needs and did not get comes back from its own calls.
            CoInitializeSecurity(IntPtr.Zero, -1, IntPtr.Zero, IntPtr.Zero, 6, 2, IntPtr.Zero, 0, IntPtr.Zero);
            Check(CreateVssBackupComponentsInternal(out b), "CreateVssBackupComponents");
            TakeSnapshots(b);
            created.Set();
            release.WaitOne();
            IVssAsync done;
            Check(b.BackupComplete(out done), "BackupComplete"); Await(done, "BackupComplete");
        } catch (Exception e) {
            failure = e.Message;
            created.Set();
        } finally {
            if (b != null) Marshal.ReleaseComObject(b);
            finished.Set();
        }
    }
}

public static class Io {
    const uint GenericRead = 0x80000000, GenericWrite = 0x40000000, ShareReadWrite = 3, OpenExisting = 3;
    const uint RawTargetPath = 1, RemoveDefinition = 2, ExactMatch = 4;
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] input, int inputLength, byte[] output, int outputLength, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, byte[] buffer, int count, out int read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(SafeFileHandle h, byte[] buffer, int count, out int written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool DefineDosDeviceW(uint flags, string name, string target);
    static SafeFileHandle Open(string path, uint access) {
        SafeFileHandle h = CreateFileW(path, access, ShareReadWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("Could not open " + path + ": Windows error " + Marshal.GetLastWin32Error());
        return h;
    }
    public static SafeFileHandle OpenRead(string path) { return Open(path, GenericRead); }
    // Compact OS keeps Windows' own files compressed behind the file system. Those files open normally
    // and read back whole, so the only way to tell is to ask for their external backing.
    // A refusal means no: the file has no backing, or its volume has no overlay filter at all.
    public static bool IsExternallyBacked(string path) {
        const uint ReadAttributes = 0x80, BackupSemantics = 0x02000000, GetExternalBacking = 0x90310;
        using (SafeFileHandle h = CreateFileW(path, ReadAttributes, ShareReadWrite, IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero)) {
            if (h.IsInvalid) throw new IOException("Could not open " + path + ": Windows error " + Marshal.GetLastWin32Error());
            byte[] info = new byte[1024];
            int returned;
            return DeviceIoControl(h, GetExternalBacking, null, 0, info, info.Length, out returned, IntPtr.Zero);
        }
    }
    public static SafeFileHandle OpenWrite(string path) { return Open(path, GenericRead | GenericWrite); }
    public static bool Control(SafeFileHandle h, uint code, byte[] input, byte[] output, int moreDataError) {
        int returned;
        if (DeviceIoControl(h, code, input, input == null ? 0 : input.Length, output, output == null ? 0 : output.Length, out returned, IntPtr.Zero)) return true;
        int error = Marshal.GetLastWin32Error();
        if (error == moreDataError) return false;
        throw new IOException("Device control 0x" + code.ToString("X") + " failed: Windows error " + error);
    }
    static void Seek(SafeFileHandle h, long offset) {
        long position;
        if (!SetFilePointerEx(h, offset, out position, 0)) throw new IOException("Seek to " + offset + " failed: Windows error " + Marshal.GetLastWin32Error());
    }
    public static void Read(SafeFileHandle h, long offset, byte[] buffer, int count) {
        Seek(h, offset);
        int read;
        if (!ReadFile(h, buffer, count, out read, IntPtr.Zero)) throw new IOException("Read at " + offset + " failed: Windows error " + Marshal.GetLastWin32Error());
        if (read != count) throw new IOException("Read at " + offset + " returned " + read + " of " + count + " bytes");
    }
    public static void Write(SafeFileHandle h, long offset, byte[] buffer, int count) {
        Seek(h, offset);
        int written;
        if (!WriteFile(h, buffer, count, out written, IntPtr.Zero)) throw new IOException("Write at " + offset + " failed: Windows error " + Marshal.GetLastWin32Error());
        if (written != count) throw new IOException("Write at " + offset + " wrote " + written + " of " + count + " bytes");
    }
    public static void Flush(SafeFileHandle h) {
        if (!FlushFileBuffers(h)) throw new IOException("Flush failed: Windows error " + Marshal.GetLastWin32Error());
    }
    // A drive letter for a snapshot device, in this logon session. DISM takes a drive, not a device.
    public static void MapDrive(string letter, string device) {
        if (!DefineDosDeviceW(RawTargetPath, letter, device)) throw new IOException("Could not give the snapshot a drive letter: Windows error " + Marshal.GetLastWin32Error());
    }
    public static void UnmapDrive(string letter, string device) {
        if (!DefineDosDeviceW(RawTargetPath | RemoveDefinition | ExactMatch, letter, device)) throw new IOException("Could not take the snapshot's drive letter back: Windows error " + Marshal.GetLastWin32Error());
    }
}

public sealed class LayoutEntry {
    public long Offset; public long Length;
    public Guid GptType; public Guid PartitionId; public ulong Attributes; public string Name;
    public byte MbrType; public bool Boot;
}
public sealed class DiskLayout { public bool Gpt; public int Signature; public Guid DiskId; public List<LayoutEntry> Entries = new List<LayoutEntry>(); }
public static class Layout {
    const uint CreateDiskCode = 0x7C058, SetLayoutCode = 0x7C054, GetLayoutCode = 0x70050, UpdateCode = 0x70140;
    const int HeaderBytes = 48, EntryBytes = 144, MaxEntries = 128, MbrEntriesMultiple = 4, SectorForHidden = 512;
    public static int NewSignature() { int v = BitConverter.ToInt32(Guid.NewGuid().ToByteArray(), 0); return v == 0 ? 1 : v; }
    public static void Create(SafeFileHandle disk, bool gpt, int signature, Guid diskId) {
        byte[] input = new byte[24];
        BitConverter.GetBytes(gpt ? 1 : 0).CopyTo(input, 0);
        if (gpt) { diskId.ToByteArray().CopyTo(input, 4); BitConverter.GetBytes(MaxEntries).CopyTo(input, 20); }
        else BitConverter.GetBytes(signature).CopyTo(input, 4);
        Io.Control(disk, CreateDiskCode, input, null, 0);
        Io.Control(disk, UpdateCode, null, null, 0);
    }
    static byte[] Bytes(byte[] from, int at) { byte[] b = new byte[16]; Array.Copy(from, at, b, 0, 16); return b; }
    public static DiskLayout Read(SafeFileHandle disk) {
        byte[] buf = new byte[HeaderBytes + EntryBytes * MaxEntries];
        Io.Control(disk, GetLayoutCode, null, buf, 0);
        DiskLayout layout = new DiskLayout();
        layout.Gpt = BitConverter.ToInt32(buf, 0) == 1;
        layout.Signature = BitConverter.ToInt32(buf, 8);
        layout.DiskId = new Guid(Bytes(buf, 8));
        int count = BitConverter.ToInt32(buf, 4);
        for (int i = 0; i < count; ++i) {
            int at = HeaderBytes + EntryBytes * i;
            LayoutEntry e = new LayoutEntry();
            e.Offset = BitConverter.ToInt64(buf, at + 8);
            e.Length = BitConverter.ToInt64(buf, at + 16);
            if (e.Length == 0) continue;
            if (layout.Gpt) {
                e.GptType = new Guid(Bytes(buf, at + 32));
                e.PartitionId = new Guid(Bytes(buf, at + 48));
                e.Attributes = BitConverter.ToUInt64(buf, at + 64);
                e.Name = Encoding.Unicode.GetString(buf, at + 72, 72).TrimEnd('\0');
            } else {
                e.MbrType = buf[at + 32];
                e.Boot = buf[at + 33] != 0;
            }
            layout.Entries.Add(e);
        }
        return layout;
    }
    static void WriteEntry(byte[] buf, int index, LayoutEntry e, bool gpt) {
        int at = HeaderBytes + EntryBytes * index;
        BitConverter.GetBytes(gpt ? 1 : 0).CopyTo(buf, at);
        BitConverter.GetBytes(e.Offset).CopyTo(buf, at + 8);
        BitConverter.GetBytes(e.Length).CopyTo(buf, at + 16);
        BitConverter.GetBytes(index + 1).CopyTo(buf, at + 24);
        buf[at + 28] = 1;
        if (gpt) {
            e.GptType.ToByteArray().CopyTo(buf, at + 32);
            e.PartitionId.ToByteArray().CopyTo(buf, at + 48);
            BitConverter.GetBytes(e.Attributes).CopyTo(buf, at + 64);
            byte[] name = Encoding.Unicode.GetBytes(e.Name ?? "");
            Array.Copy(name, 0, buf, at + 72, Math.Min(name.Length, 70));
        } else {
            buf[at + 32] = e.MbrType;
            buf[at + 33] = (byte)(e.Boot ? 1 : 0);
            buf[at + 34] = 1;
            BitConverter.GetBytes((int)(e.Offset / SectorForHidden)).CopyTo(buf, at + 36);
        }
    }
    public static void Write(SafeFileHandle disk, DiskLayout layout) {
        int slots = layout.Gpt ? layout.Entries.Count : Math.Max(layout.Entries.Count, MbrEntriesMultiple);
        byte[] buf = new byte[HeaderBytes + EntryBytes * slots];
        BitConverter.GetBytes(layout.Gpt ? 1 : 0).CopyTo(buf, 0);
        BitConverter.GetBytes(slots).CopyTo(buf, 4);
        if (layout.Gpt) { layout.DiskId.ToByteArray().CopyTo(buf, 8); BitConverter.GetBytes(MaxEntries).CopyTo(buf, 40); }
        else BitConverter.GetBytes(layout.Signature).CopyTo(buf, 8);
        for (int i = 0; i < layout.Entries.Count; ++i) WriteEntry(buf, i, layout.Entries[i], layout.Gpt);
        Io.Control(disk, SetLayoutCode, buf, null, 0);
        Io.Control(disk, UpdateCode, null, null, 0);
    }
}

// Offset/length pairs, in order. Runs no further apart than the gap become one.
public sealed class RangeList {
    readonly long gap;
    readonly List<long> ranges = new List<long>();
    long start = -1, end = -1;
    public RangeList(long mergeGap) { gap = mergeGap; }
    public void Add(long at, long length) {
        if (start >= 0 && at - end > gap) Close();
        if (start < 0) start = at;
        end = at + length;
    }
    void Close() { ranges.Add(start); ranges.Add(end - start); start = -1; }
    public long[] ToArray() { if (start >= 0) Close(); return ranges.ToArray(); }
}

// What a copy read, kept so that the target can be checked against it afterwards. The source cannot be
// asked again: a snapshot keeps no copy of a free cluster or of the page file, so what it reads there
// follows the running volume, and Windows writing to one would read as a bad copy. A corruption check,
// not a security one, so a plain 64-bit FNV-1a, which needs no provider a FIPS policy can refuse.
public sealed class CopyCheck {
    const long SampleBlocks = 64;
    readonly long blocks, stride;
    readonly List<ulong> sums = new List<ulong>();
    public CopyCheck(long[] ranges, bool full) {
        for (int r = 1; r < ranges.Length; r += 2) blocks += (ranges[r] + Copy.BlockBytes - 1) / Copy.BlockBytes;
        stride = full ? 1 : Math.Max(1, (blocks + SampleBlocks - 1) / SampleBlocks);
    }
    public bool Wanted(long block) { return block % stride == 0 || block == blocks - 1; }
    public void Note(long block, byte[] data, int count) { if (Wanted(block)) sums.Add(Sum(data, count)); }
    public ulong Noted(int index) { return sums[index]; }
    public static ulong Sum(byte[] data, int count) {
        ulong sum = 14695981039346656037UL;
        for (int i = 0; i < count; ++i) { sum ^= data[i]; sum *= 1099511628211UL; }
        return sum;
    }
}

public static class Copy {
    const uint VolumeDataCode = 0x90064, VolumeBitmapCode = 0x9006F;
    const int MoreData = 234, BitmapChunk = 1024 * 1024;
    const long QuietBelowBytes = 1024L * 1024L * 1024L, MergeGap = 1024 * 1024;
    public const int BlockBytes = 4 * 1024 * 1024;
    public static long[] Whole(long size) { return new long[] { 0, size }; }
    // Bytes in use on an NTFS volume, from its own counters.
    public static long UsedBytes(SafeFileHandle volume) {
        byte[] data = new byte[512];
        Io.Control(volume, VolumeDataCode, null, data, 0);
        return (BitConverter.ToInt64(data, 16) - BitConverter.ToInt64(data, 24)) * BitConverter.ToInt32(data, 44);
    }
    // NTFS keeps a second copy of its boot sector in the sector past its last cluster. A snapshot
    // device ends at the last cluster, so the copy is made from the first sector, which is the same.
    public static void BackupBootSector(SafeFileHandle volume, SafeFileHandle target, long targetBase) {
        byte[] data = new byte[512];
        Io.Control(volume, VolumeDataCode, null, data, 0);
        long sectors = BitConverter.ToInt64(data, 8);
        int bytesPerSector = BitConverter.ToInt32(data, 40);
        byte[] sector = new byte[bytesPerSector];
        Io.Read(volume, 0, sector, bytesPerSector);
        Io.Write(target, targetBase + sectors * bytesPerSector, sector, bytesPerSector);
        Io.Flush(target);
    }
    // The volume's allocated clusters as offset/length pairs, close neighbours merged to spare a seek.
    public static long[] UsedRanges(SafeFileHandle volume) {
        byte[] data = new byte[512];
        Io.Control(volume, VolumeDataCode, null, data, 0);
        long cluster = BitConverter.ToInt32(data, 44);
        RangeList ranges = new RangeList(MergeGap);
        byte[] start = new byte[8];
        byte[] output = new byte[BitmapChunk + 16];
        long lcn = 0;
        for (bool more = true; more;) {
            BitConverter.GetBytes(lcn).CopyTo(start, 0);
            more = !Io.Control(volume, VolumeBitmapCode, start, output, MoreData);
            long first = BitConverter.ToInt64(output, 0), clusters = BitConverter.ToInt64(output, 8);
            long count = Math.Min(clusters, (long)BitmapChunk * 8);
            for (long i = 0; i < count; ++i) {
                if ((output[16 + i / 8] & (1 << (int)(i % 8))) != 0) ranges.Add((first + i) * cluster, cluster);
            }
            lcn = first + count;
        }
        return ranges.ToArray();
    }
    static long Total(long[] ranges) { long total = 0; for (int i = 1; i < ranges.Length; i += 2) total += ranges[i]; return total; }
    public static void Run(SafeFileHandle source, long sourceBase, SafeFileHandle target, long targetBase, long[] ranges, string label, CopyCheck check) {
        byte[] buffer = new byte[BlockBytes];
        long total = Total(ranges), done = 0, block = 0; int shown = -5;
        for (int r = 0; r + 1 < ranges.Length; r += 2) {
            for (long at = ranges[r], left = ranges[r + 1]; left > 0; ++block) {
                int take = (int)Math.Min(left, BlockBytes);
                Io.Read(source, sourceBase + at, buffer, take);
                check.Note(block, buffer, take);
                Io.Write(target, targetBase + at, buffer, take);
                at += take; left -= take; done += take;
                int percent = (int)(done * 100 / total);
                if (total >= QuietBelowBytes && percent >= shown + 5) { shown = percent; Console.Out.WriteLine(label + ": " + percent + "% of " + total + " bytes"); }
            }
        }
        Io.Flush(target);
    }
    // Reads the blocks the check kept a note of back from the target.
    public static void Verify(SafeFileHandle target, long targetBase, long[] ranges, CopyCheck check) {
        byte[] buffer = new byte[BlockBytes];
        long block = 0; int noted = 0;
        for (int r = 0; r + 1 < ranges.Length; r += 2) {
            for (long at = ranges[r], left = ranges[r + 1]; left > 0; ++block) {
                int take = (int)Math.Min(left, BlockBytes);
                if (check.Wanted(block)) {
                    Io.Read(target, targetBase + at, buffer, take);
                    if (CopyCheck.Sum(buffer, take) != check.Noted(noted++)) throw new IOException("Verification found a difference in the " + take + " bytes at target byte " + (targetBase + at));
                }
                at += take; left -= take;
            }
        }
    }
}
}
