// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// The native types S.A.K.'s elevated PowerShell scripts use, built into sak_script_types.dll with
// the program (C# 5: the .NET Framework compiler every Windows has, the one Windows PowerShell 5.1
// itself uses). The scripts used to compile each of these with Add-Type -TypeDefinition, which has
// csc.exe write a source file and a DLL into the temporary folder and load that DLL: a window in
// which another account could change what is about to run as administrator on a drive that keeps
// no ACLs (owner, 2026-11-11). This DLL is a program file like any other: verified and held with
// the rest before an elevation (sak::program_integrity), and loaded from its bytes
// ([Reflection.Assembly]::Load), so a downloaded copy marked as coming from the internet loads too.

using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

// Restore Image resolves the image it locked to its real path (it was an Add-Type of its own).
public static class SakLockedFilePath {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetFinalPathNameByHandle(IntPtr handle, StringBuilder path, uint length,
        uint flags);
}

namespace Sak {

// Get-SakVolumeGuid: a path's volume identity, so backup-root checks compare volumes, not text.
public static class VolApi {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetVolumePathName(string f, StringBuilder v, uint c);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetVolumeNameForVolumeMountPoint(string m, StringBuilder v, uint c);
}

// The bounded disk scan ends its own process at once when a storage call never answers.
public static class ScanProcess {
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")]
    public static extern bool TerminateProcess(IntPtr process, uint exitCode);
}

// Verification reads what was written straight from the medium, past the system cache (unbuffered
// reads in whole 4 MiB chunks from a chunk boundary into a VirtualAlloc buffer).
public sealed class MediaReadStream : Stream {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, IntPtr buffer, int count, out int read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(SafeFileHandle h, long distance, out long position, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileSizeEx(SafeFileHandle h, out long size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr input, int inputLength, out long output, int outputLength, out int returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint type);
    const uint GenericRead = 0x80000000, ShareRead = 1, OpenExisting = 3;
    public const uint NoBuffering = 0x20000000;
    const uint SequentialScan = 0x08000000, DiskGetLengthInfo = 0x7405C;
    const uint MemCommitReserve = 0x3000, PageReadWrite = 0x04, MemRelease = 0x8000;
    public const int ChunkBytes = 4 * 1024 * 1024;
    readonly SafeFileHandle handle;
    readonly long length;
    readonly bool disk;
    IntPtr chunk;
    long chunkStart = -1;
    int chunkFill;
    long position;
    public MediaReadStream(string path, bool isDisk) {
        disk = isDisk;
        handle = CreateFileW(path, GenericRead, ShareRead, IntPtr.Zero, OpenExisting, NoBuffering | SequentialScan, IntPtr.Zero);
        if (handle.IsInvalid) throw new IOException("Could not open " + path + " to verify it: Windows error " + Marshal.GetLastWin32Error());
        long size;
        int returned;
        if (!(disk ? DeviceIoControl(handle, DiskGetLengthInfo, IntPtr.Zero, 0, out size, 8, out returned, IntPtr.Zero) : GetFileSizeEx(handle, out size))) {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException("Could not read the size of " + path + " to verify it: Windows error " + error);
        }
        length = size;
        chunk = VirtualAlloc(IntPtr.Zero, (UIntPtr)ChunkBytes, MemCommitReserve, PageReadWrite);
        if (chunk == IntPtr.Zero) {
            handle.Dispose();
            throw new IOException("No memory to verify " + path);
        }
    }
    public override bool CanRead { get { return true; } }
    public override bool CanSeek { get { return true; } }
    public override bool CanWrite { get { return false; } }
    public override long Length { get { return length; } }
    public override long Position {
        get { return position; }
        set { if (value < 0) throw new ArgumentOutOfRangeException("value"); position = value; }
    }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) {
        long start = origin == SeekOrigin.Begin ? 0 : origin == SeekOrigin.Current ? position : length;
        Position = start + offset;
        return position;
    }
    public override void SetLength(long value) { throw new NotSupportedException(); }
    public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    public override int Read(byte[] buffer, int offset, int count) {
        if (position >= length || count <= 0) return 0;
        long start = position - position % ChunkBytes;
        if (start != chunkStart) Load(start);
        int at = (int)(position - chunkStart);
        int take = Math.Min(count, chunkFill - at);
        if (take <= 0) return 0;
        Marshal.Copy(new IntPtr(chunk.ToInt64() + at), buffer, offset, take);
        position += take;
        return take;
    }
    void Load(long start) {
        chunkStart = -1;
        long moved;
        if (!SetFilePointerEx(handle, start, out moved, 0)) throw new IOException("Verification could not reach byte " + start + ": Windows error " + Marshal.GetLastWin32Error());
        int want = disk ? (int)Math.Min((long)ChunkBytes, length - start) : ChunkBytes;
        int read;
        if (!ReadFile(handle, chunk, want, out read, IntPtr.Zero)) throw new IOException("Verification could not read byte " + start + ": Windows error " + Marshal.GetLastWin32Error());
        chunkFill = (int)Math.Min((long)read, length - start);
        chunkStart = start;
    }
    protected override void Dispose(bool disposing) {
        if (chunk != IntPtr.Zero) { VirtualFree(chunk, UIntPtr.Zero, MemRelease); chunk = IntPtr.Zero; }
        if (disposing) handle.Dispose();
        base.Dispose(disposing);
    }
}

// Exact first-mismatch reporting without enumerating hundreds of millions of bytes in PowerShell.
public static class CloneByteComparer {
    public static int FirstMismatch(byte[] left, byte[] right, int count) {
        if (left == null || right == null || count < 0 ||
            count > left.Length || count > right.Length) {
            throw new ArgumentException("Clone comparison requires complete bounded buffers");
        }
        for (int index = 0; index < count; ++index) {
            if (left[index] != right[index]) return index;
        }
        return -1;
    }
}

// Create Image: FSCTL_LOCK_VOLUME flushes cached data and holds the filesystem quiescent until its
// handle closes. A read-sharing physical-disk handle alone does not stop NTFS writes.
public static class ImageVolumeLock {
    private const uint GenericRead = 0x80000000;
    private const uint ShareReadWrite = 3;
    private const uint OpenExisting = 3;
    private const uint LockVolume = 0x00090018;
    private const uint AllowExtendedDASD = 0x00090083;
    private const uint StorageDeviceNumber = 0x002D1080;
    private const uint GetDiskAttributes = 0x000700F0;
    private const uint DiskDeviceType = 7;
    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceNumber { public uint Type; public uint Disk; public uint Partition; }
    [StructLayout(LayoutKind.Sequential)]
    private struct DiskAttributes { public uint Version; public uint Reserved; public ulong Flags; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, out DeviceNumber output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, out DiskAttributes output, uint outputSize,
        out uint returned, IntPtr overlapped);
    public static SafeFileHandle OpenDiskControl(string path, uint disk) {
        SafeFileHandle handle = CreateFile(path, 0, ShareReadWrite,
            IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        try {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Create Image cannot open its source disk identity handle: " + path);
            Verify(handle, disk, 0, false);
            return handle;
        } catch { handle.Dispose(); throw; }
    }
    public static ulong ReadDiskAttributes(SafeFileHandle handle) {
        DiskAttributes actual;
        uint returned;
        uint required = (uint)Marshal.SizeOf(typeof(DiskAttributes));
        if (!DeviceIoControl(handle, GetDiskAttributes, IntPtr.Zero, 0, out actual,
            required, out returned, IntPtr.Zero)) throw new Win32Exception(
                Marshal.GetLastWin32Error(), "Create Image cannot query source disk attributes");
        if (returned != required || actual.Version != required) throw new InvalidOperationException(
            "Create Image source disk attributes reply is invalid");
        return actual.Flags;
    }
    public static SafeFileHandle Acquire(string path) {
        SafeFileHandle handle = CreateFile(path, GenericRead, ShareReadWrite,
            IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
        try {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Create Image cannot open the source volume for exclusive capture: " + path);
            uint returned;
            if (!DeviceIoControl(handle, LockVolume, IntPtr.Zero, 0, IntPtr.Zero, 0,
                out returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Create Image cannot lock the source volume. Close its open files; running " +
                "system/pagefile volumes require an offline capture. No live-copy fallback: " + path);
            return handle;
        } catch { handle.Dispose(); throw; }
    }
    public static void Verify(SafeFileHandle handle, uint disk, uint partition, bool checkPartition) {
        DeviceNumber actual;
        uint returned;
        uint required = (uint)Marshal.SizeOf(typeof(DeviceNumber));
        if (!DeviceIoControl(handle, StorageDeviceNumber, IntPtr.Zero, 0, out actual,
            required, out returned, IntPtr.Zero)) throw new Win32Exception(
                Marshal.GetLastWin32Error(), "Create Image cannot identify the opened source");
        if (returned != required || actual.Type != DiskDeviceType || actual.Disk != disk ||
            (checkPartition && actual.Partition != partition)) throw new InvalidOperationException(
                "Create Image opened source device identity mismatch");
    }
    public static void AllowExtendedRead(SafeFileHandle handle) {
        // The caller reads exactly the pinned partition size, including its final sectors.
        uint returned;
        if (!DeviceIoControl(handle, AllowExtendedDASD, IntPtr.Zero, 0, IntPtr.Zero, 0,
            out returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Create Image cannot enable complete source partition reads");
    }
}

// Check File System on a drive Windows does not snapshot, a removable one: Repair-Volume -Scan and
// chkdsk /scan both need the snapshot, and stop with "A snapshot error occurred while scanning
// this drive" (testpc1's NTFS USB stick, 2026-11-13). chkdsk then reads the volume live and read
// only, so nothing else may have it open while it does. FSCTL_LOCK_VOLUME fails while any other
// handle is open, and the drive is then called in use, not checked; FSCTL_DISMOUNT_VOLUME has
// Windows write out what it holds and drop the file system; closing the handle leaves the
// volume to chkdsk, which mounts it again for itself. Nothing on the volume is written.
public static class ScanVolumeLock {
    private const uint GenericRead = 0x80000000;
    private const uint ShareReadWrite = 3;
    private const uint OpenExisting = 3;
    private const uint LockVolume = 0x00090018;
    private const uint DismountVolume = 0x00090020;
    private const int AccessDenied = 5;
    private const int SharingViolation = 32;
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    public static string InUseMessage(string letter) {
        return letter.ToUpperInvariant() + ": is in use, so it was not checked. Close the files " +
            "and programs using it, then check it again. Nothing was changed.";
    }
    public static void LockAndDismount(string letter) {
        if (letter == null || letter.Length != 1 || !char.IsLetter(letter[0])) {
            throw new ArgumentException("Check File System needs a drive letter, not: " + letter);
        }
        string path = "\\\\.\\" + letter.ToUpperInvariant() + ":";
        using (SafeFileHandle handle = CreateFile(path, GenericRead, ShareReadWrite, IntPtr.Zero,
            OpenExisting, 0, IntPtr.Zero)) {
            if (handle.IsInvalid) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    letter.ToUpperInvariant() + ": could not be opened to check it");
            }
            uint returned;
            if (!DeviceIoControl(handle, LockVolume, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned,
                IntPtr.Zero)) {
                int error = Marshal.GetLastWin32Error();
                if (error == AccessDenied || error == SharingViolation) {
                    throw new InvalidOperationException(InUseMessage(letter));
                }
                throw new Win32Exception(error, letter.ToUpperInvariant() +
                    ": could not be locked to check it");
            }
            if (!DeviceIoControl(handle, DismountVolume, IntPtr.Zero, 0, IntPtr.Zero, 0,
                out returned, IntPtr.Zero)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), letter.ToUpperInvariant() +
                    ": could not be taken offline to check it");
            }
        }
    }
}

// Copy Partition holds its source volume with the same lock as Create Image, and says so in its own
// words. The script used to compile a renamed copy of ImageVolumeLock with Add-Type.
public static class CopyVolumeLock {
    static Exception Reworded(Exception e) {
        string message = e.Message.Replace("Create Image", "Copy Partition");
        Win32Exception win32 = e as Win32Exception;
        if (win32 != null) return new Win32Exception(win32.NativeErrorCode, message);
        return new InvalidOperationException(message, e);
    }
    public static SafeFileHandle OpenDiskControl(string path, uint disk) {
        try { return ImageVolumeLock.OpenDiskControl(path, disk); } catch (Exception e) { throw Reworded(e); }
    }
    public static ulong ReadDiskAttributes(SafeFileHandle handle) {
        try { return ImageVolumeLock.ReadDiskAttributes(handle); } catch (Exception e) { throw Reworded(e); }
    }
    public static SafeFileHandle Acquire(string path) {
        try { return ImageVolumeLock.Acquire(path); } catch (Exception e) { throw Reworded(e); }
    }
    public static void Verify(SafeFileHandle handle, uint disk, uint partition, bool checkPartition) {
        try { ImageVolumeLock.Verify(handle, disk, partition, checkPartition); } catch (Exception e) { throw Reworded(e); }
    }
    public static void AllowExtendedRead(SafeFileHandle handle) {
        try { ImageVolumeLock.AllowExtendedRead(handle); } catch (Exception e) { throw Reworded(e); }
    }
}

// HFS+ Format, Repair and file changes copy the staged image onto the partition. Windows mounts the
// file system it recognizes (NTFS, FAT32, exFAT) on a partition as soon as it is opened, drive
// letter or not. Through that file system's handle the partition's last sectors are out of reach (a
// write there comes back short, and FileStream does not notice), and the file system can still
// write its own metadata over the copy. So the partition is taken first, as the C++ writers do
// (sak::openFileOrRawDeviceReadWrite): whole-partition (extended DASD) access, the lock, which
// Windows refuses while anything else has it open, and a dismount, so the file system lets go now
// and Windows mounts the new one afresh when the stream closes. A staged image file opens as a file.
public static class RawPartitionWrite {
    private const uint GenericReadWrite = 0xC0000000;
    private const uint ShareRead = 1;
    private const uint OpenExisting = 3;
    private const uint WriteThrough = 0x80000000;
    private const uint LockVolume = 0x00090018;
    private const uint DismountVolume = 0x00090020;
    private const uint AllowExtendedDASD = 0x00090083;
    private const int SharingViolation = 32;
    private const int Attempts = 50;
    private const int RetryMs = 200;
    private const int EdgeBytes = 1024 * 1024;
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    public static bool IsPartitionPath(string path) {
        return path.StartsWith(@"\\?\GLOBALROOT\Device\Harddisk", StringComparison.OrdinalIgnoreCase) &&
            !path.EndsWith(@"\Partition0", StringComparison.OrdinalIgnoreCase);
    }
    static Exception InUse(int error, string path) {
        return new Win32Exception(error,
            "The partition is in use, so Windows will not hand it over: close File Explorer " +
            "windows and programs using it, then try again: " + path);
    }
    // Opened write-through, then whole-partition access, the lock and the dismount. A partition
    // Windows has just mounted or formatted is often opened for a moment by Explorer or the
    // antivirus, so a sharing violation or a refused lock is tried again for ten seconds: a FAT32
    // volume refused the lock for about three seconds after a file on it was closed (testpc1,
    // 2026-11-11), longer than the two seconds tried before.
    static SafeFileHandle Take(string path) {
        if (!IsPartitionPath(path)) throw new ArgumentException("Not a partition: " + path);
        SafeFileHandle handle = null;
        int openError = 0;
        for (int attempt = 0; attempt < Attempts; ++attempt) {
            if (attempt > 0) System.Threading.Thread.Sleep(RetryMs);
            handle = CreateFile(path, GenericReadWrite, ShareRead, IntPtr.Zero, OpenExisting,
                WriteThrough, IntPtr.Zero);
            if (!handle.IsInvalid) break;
            openError = Marshal.GetLastWin32Error();
            handle.Dispose();
            handle = null;
            if (openError != SharingViolation) break;
        }
        if (handle == null) {
            if (openError == SharingViolation) throw InUse(openError, path);
            throw new Win32Exception(openError, "Cannot open the partition to write it: " + path);
        }
        try {
            uint returned;
            // Best effort: RAW may decline it, and needs no extension.
            DeviceIoControl(handle, AllowExtendedDASD, IntPtr.Zero, 0, IntPtr.Zero, 0,
                out returned, IntPtr.Zero);
            bool locked = false;
            int lockError = 0;
            for (int attempt = 0; attempt < Attempts && !locked; ++attempt) {
                if (attempt > 0) System.Threading.Thread.Sleep(RetryMs);
                locked = DeviceIoControl(handle, LockVolume, IntPtr.Zero, 0, IntPtr.Zero, 0,
                    out returned, IntPtr.Zero);
                if (!locked) lockError = Marshal.GetLastWin32Error();
            }
            if (!locked) throw InUse(lockError, path);
            if (!DeviceIoControl(handle, DismountVolume, IntPtr.Zero, 0, IntPtr.Zero, 0,
                out returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Windows would not let go of the partition's file system: " + path);
            return handle;
        } catch { handle.Dispose(); throw; }
    }
    // The partition taken (above) as a stream; a staged image file opens as a file.
    public static FileStream Open(string path) {
        if (!IsPartitionPath(path)) {
            return new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite);
        }
        return new TakenPartitionStream(Take(path));
    }
    // Before a bundled tool (mke2fs, e2fsck, resize2fs) or the APFS writer opens the partition
    // itself: proves nothing else has it open and makes Windows let go of its file system now.
    // Replaces Dismount-Volume, which Windows PowerShell 5.1 does not have.
    public static void Release(string path) {
        using (SafeFileHandle handle = Take(path)) { }
    }
    // Before a format by a bundled tool: Windows mounts the file system it knows again when the
    // tool opens the partition, and through it the tool could not write. The first and the last
    // MiB are zeroed through the taken partition, so Windows finds no file system there.
    public static void ClearFileSystem(string path, long sizeBytes) {
        if (sizeBytes < 2L * EdgeBytes) throw new ArgumentException("Partition too small to clear: " + path);
        using (TakenPartitionStream stream = new TakenPartitionStream(Take(path))) {
            byte[] zeros = new byte[EdgeBytes];
            stream.Seek(0, SeekOrigin.Begin);
            stream.Write(zeros, 0, zeros.Length);
            stream.Seek(sizeBytes - EdgeBytes, SeekOrigin.Begin);
            stream.Write(zeros, 0, zeros.Length);
            stream.Flush(true);
        }
    }
}

// A partition taken by RawPartitionWrite, written through to the device. Once its file system is
// dismounted, FAT and exFAT answer a flush with "The device is not ready" (21). That one answer,
// only on a partition RawPartitionWrite took and dismounted, is accepted only when every block
// written since the last flush reads back exactly as written (SHA-256 of what each block should
// hold): a write that did not reach the device fails the flush instead of passing as tolerated.
public sealed class TakenPartitionStream : FileStream {
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FlushFileBuffers(SafeFileHandle handle);
    private const int NotReady = 21;
    private const int Block = 4096;
    private readonly System.Collections.Generic.Dictionary<long, byte[]> written =
        new System.Collections.Generic.Dictionary<long, byte[]>();
    public TakenPartitionStream(SafeFileHandle handle) : base(handle, FileAccess.ReadWrite, Block, false) { }
    static byte[] Sha256(byte[] data, int offset, int count) {
        using (System.Security.Cryptography.SHA256 sha = System.Security.Cryptography.SHA256.Create()) {
            return sha.ComputeHash(data, offset, count);
        }
    }
    byte[] ReadBlockNow(long index) {
        long position = Position;
        base.Flush(false);
        byte[] content = new byte[Block];
        Seek(index * Block, SeekOrigin.Begin);
        int have = 0;
        while (have < Block) {
            int read = base.Read(content, have, Block - have);
            if (read <= 0) break;
            have += read;
        }
        Seek(position, SeekOrigin.Begin);
        if (have < Block) throw new IOException("Could not read the partition block at " + (index * Block));
        return content;
    }
    // What each block will hold once this write lands: the written bytes, over what the block
    // held before where the write covers only part of it.
    void Record(long start, byte[] array, int offset, int count) {
        long end = start + count;
        for (long index = start / Block; index * Block < end; ++index) {
            long blockStart = index * Block;
            if (blockStart >= start && blockStart + Block <= end) {
                written[index] = Sha256(array, offset + (int)(blockStart - start), Block);
                continue;
            }
            byte[] content = ReadBlockNow(index);
            long from = Math.Max(start, blockStart);
            long to = Math.Min(end, blockStart + Block);
            Buffer.BlockCopy(array, offset + (int)(from - start), content, (int)(from - blockStart), (int)(to - from));
            written[index] = Sha256(content, 0, Block);
        }
    }
    public override void Write(byte[] array, int offset, int count) {
        Record(Position, array, offset, count);
        base.Write(array, offset, count);
    }
    public override void WriteByte(byte value) {
        Write(new byte[] { value }, 0, 1);
    }
    void VerifyWritten() {
        long position = Position;
        foreach (System.Collections.Generic.KeyValuePair<long, byte[]> entry in written) {
            byte[] content = ReadBlockNow(entry.Key);
            byte[] actual = Sha256(content, 0, Block);
            for (int i = 0; i < actual.Length; ++i) {
                if (actual[i] != entry.Value[i]) throw new IOException(
                    "The partition did not flush, and the block at offset " + (entry.Key * Block) +
                    " does not read back as written");
            }
        }
        Seek(position, SeekOrigin.Begin);
        written.Clear();
    }
    public override void Flush(bool flushToDisk) {
        base.Flush(false);
        if (!flushToDisk) return;
        if (FlushFileBuffers(SafeFileHandle)) { written.Clear(); return; }
        int error = Marshal.GetLastWin32Error();
        if (error != NotReady) throw new Win32Exception(error, "Could not flush the partition");
        VerifyWritten();
    }
}

// HFS+ staging marks its image sparse and later copies only the ranges written. fsutil printed
// those ranges with labels Windows translates, so a non-English Windows could not be read; this
// asks the file system directly (FSCTL_SET_SPARSE, FSCTL_QUERY_ALLOCATED_RANGES).
public static class SparseFiles {
    private const uint GenericRead = 0x80000000, GenericWrite = 0x40000000;
    private const uint ShareRead = 1, ShareWrite = 2;
    private const uint OpenExisting = 3;
    private const uint SetSparseCode = 0x000900C4;
    private const uint QueryAllocatedRanges = 0x000940CF;
    private const int MoreData = 234;
    private const int RangesPerCall = 512;
    [StructLayout(LayoutKind.Sequential)]
    private struct Range { public long Offset; public long Length; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        ref Range input, uint inputSize, [Out] Range[] output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileSizeEx(SafeFileHandle handle, out long size);
    static SafeFileHandle OpenFile(string path, uint access) {
        SafeFileHandle handle = CreateFile(path, access, ShareRead | ShareWrite, IntPtr.Zero,
            OpenExisting, 0, IntPtr.Zero);
        if (handle.IsInvalid) {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new Win32Exception(error, "Cannot open the staged image: " + path);
        }
        return handle;
    }
    public static void SetSparse(string path) {
        using (SafeFileHandle handle = OpenFile(path, GenericRead | GenericWrite)) {
            uint returned;
            if (!DeviceIoControl(handle, SetSparseCode, IntPtr.Zero, 0, IntPtr.Zero, 0,
                out returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Cannot mark the staged image sparse: " + path);
        }
    }
    // Offset and length pairs, flattened: [offset0, length0, offset1, length1, ...].
    public static long[] AllocatedRanges(string path) {
        using (SafeFileHandle handle = OpenFile(path, GenericRead)) {
            long size;
            if (!GetFileSizeEx(handle, out size)) throw new Win32Exception(
                Marshal.GetLastWin32Error(), "Cannot read the staged image's size: " + path);
            System.Collections.Generic.List<long> flat = new System.Collections.Generic.List<long>();
            Range query = new Range();
            query.Offset = 0;
            query.Length = size;
            Range[] found = new Range[RangesPerCall];
            uint rangeBytes = (uint)Marshal.SizeOf(typeof(Range));
            while (query.Length > 0) {
                uint returned;
                bool ok = DeviceIoControl(handle, QueryAllocatedRanges, ref query, rangeBytes,
                    found, rangeBytes * RangesPerCall, out returned, IntPtr.Zero);
                int error = ok ? 0 : Marshal.GetLastWin32Error();
                if (!ok && error != MoreData) throw new Win32Exception(error,
                    "Cannot list the staged image's written ranges: " + path);
                int count = (int)(returned / rangeBytes);
                for (int index = 0; index < count; ++index) {
                    flat.Add(found[index].Offset);
                    flat.Add(found[index].Length);
                }
                if (ok || count == 0) break;
                long next = found[count - 1].Offset + found[count - 1].Length;
                query.Length = size - next;
                query.Offset = next;
            }
            return flat.ToArray();
        }
    }
}

// Files and folders an elevated script writes, opened so no link can redirect them (security brief
// 4a and 4b; the C++ side is sak::no_follow). A file is created new, never through a name that
// already exists or a link, and checked to be exactly the path named; a folder is held so it, and
// every folder above it, cannot be renamed or replaced while the script works in it.
public static class SafeFiles {
    const uint GenericRead = 0x80000000, GenericWrite = 0x40000000;
    const uint ReadAttributes = 0x80, ListDirectory = 0x01, Traverse = 0x20, Synchronize = 0x00100000;
    const uint ShareRead = 1, ShareWrite = 2, ShareDelete = 4;
    const uint CreateNewDisposition = 1, OpenExisting = 3;
    const uint BackupSemantics = 0x02000000, OpenReparsePoint = 0x00200000;
    const uint AttributeDirectory = 0x10, AttributeReparsePoint = 0x400;
    const int FileAttributeTagInfo = 9;
    const uint NameSurrogateBit = 0x20000000;
    const int FileBasicInfo = 0;
    [StructLayout(LayoutKind.Sequential)]
    struct AttributeTag { public uint Attributes; public uint ReparseTag; }
    [StructLayout(LayoutKind.Sequential)]
    struct BasicInfo {
        public long CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
        public uint Attributes, Padding;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder path,
        uint length, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int infoClass,
        out AttributeTag info, uint size);
    [DllImport("kernel32.dll", SetLastError = true, EntryPoint = "GetFileInformationByHandleEx")]
    static extern bool GetFileBasicInfo(SafeFileHandle handle, int infoClass, out BasicInfo info,
        uint size);

    static string Comparable(string path) {
        return Path.GetFullPath(path).TrimEnd('\\');
    }

    // Where the handle really is: "C:\dir\file" or "\\server\share\dir\file".
    public static string FinalPath(SafeFileHandle handle) {
        var buffer = new StringBuilder(32768);
        uint length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
        if (length == 0 || length >= buffer.Capacity) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot tell where a path really is");
        }
        string path = buffer.ToString();
        if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return @"\\" + path.Substring(8);
        if (path.StartsWith(@"\\?\")) return path.Substring(4);
        return path;
    }

    // What a path must resolve to when nothing below its drive or share root is a link: the root
    // as Windows resolves it (a mapped drive's share, a SUBST drive's folder) plus the rest.
    static string ExpectedPath(string full) {
        string root = Path.GetPathRoot(full);
        using (SafeFileHandle rootHandle = CreateFileW(root, ReadAttributes, ShareRead | ShareWrite | ShareDelete,
                   IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero)) {
            if (rootHandle.IsInvalid) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open " + root);
            }
            string resolved = FinalPath(rootHandle).TrimEnd('\\');
            string rest = full.Substring(root.Length).TrimStart('\\');
            return rest.Length == 0 ? resolved : resolved + "\\" + rest;
        }
    }

    static void RequireOrdinary(SafeFileHandle handle, string path, bool directory) {
        // FAT32 and exFAT refuse FileAttributeTagInfo (measured on the VM, 2026-11-11); there the
        // attributes come from FileBasicInfo and the tag is 0, since they hold no reparse point.
        AttributeTag tag;
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, out tag, 8)) {
            int tagError = Marshal.GetLastWin32Error();
            BasicInfo basic;
            if (!GetFileBasicInfo(handle, FileBasicInfo, out basic, 40) ||
                (basic.Attributes & AttributeReparsePoint) != 0) {
                throw new Win32Exception(tagError, "Cannot read " + path);
            }
            tag.Attributes = basic.Attributes;
            tag.ReparseTag = 0;
        }
        if ((tag.Attributes & AttributeReparsePoint) != 0 && (tag.ReparseTag & NameSurrogateBit) != 0) {
            throw new IOException(path + " is a link (a junction or symbolic link), so it was not used");
        }
        if (directory != ((tag.Attributes & AttributeDirectory) != 0)) {
            throw new IOException(path + (directory ? " is not a folder" : " is not a file"));
        }
    }

    static void RequireResolvesTo(SafeFileHandle handle, string full) {
        string real = FinalPath(handle).TrimEnd('\\');
        if (!string.Equals(real, ExpectedPath(full).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) {
            throw new IOException("A link on the way to " + full + " leads to " + real + " instead, so it was not used");
        }
    }

    // A new file, created with CREATE_NEW without following a link, checked to be exactly @path.
    // Others may only read it while this stream is open.
    public static FileStream CreateNew(string path) {
        string full = Comparable(path);
        SafeFileHandle handle = CreateFileW(full, GenericRead | GenericWrite, ShareRead, IntPtr.Zero,
            CreateNewDisposition, OpenReparsePoint, IntPtr.Zero);
        if (handle.IsInvalid) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot create " + full);
        }
        try {
            RequireResolvesTo(handle, full);
            return new FileStream(handle, FileAccess.ReadWrite);
        } catch { handle.Dispose(); throw; }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string path);
    const uint InvalidAttributes = 0xFFFFFFFF;

    // CreateNew, after removing an ordinary file already at @path BY NAME (DeleteFile never follows
    // a link). A link or a folder at @path refuses. The caller holds the folder (HoldDirectory), so
    // the name cannot be swapped between the check and the create; if one appears anyway, CreateNew
    // refuses it.
    public static FileStream CreateReplacing(string path) {
        string full = Comparable(path);
        uint attributes = GetFileAttributesW(full);
        if (attributes != InvalidAttributes) {
            if ((attributes & AttributeReparsePoint) != 0) {
                throw new IOException(full + " is a link (a junction or symbolic link), so it was not replaced");
            }
            if ((attributes & AttributeDirectory) != 0) {
                throw new IOException(full + " is a folder");
            }
            File.SetAttributes(full, FileAttributes.Normal);
            File.Delete(full);
        }
        return CreateNew(full);
    }

    // Hold the folder @path: not a link, no link above it, and while held nobody can rename,
    // replace or remove it or any folder above it.
    public static SafeFileHandle HoldDirectory(string path) {
        string full = Comparable(path);
        SafeFileHandle handle = CreateFileW(full, ListDirectory | Traverse | ReadAttributes | Synchronize,
            ShareRead | ShareWrite, IntPtr.Zero, OpenExisting, BackupSemantics | OpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open " + full);
        }
        try {
            RequireOrdinary(handle, full, true);
            RequireResolvesTo(handle, full);
            return handle;
        } catch { handle.Dispose(); throw; }
    }

    // A new file @path holding exactly @bytes, then held read-only: written through a CREATE_NEW
    // handle that shares reading only, closed, reopened for reading sharing reading only, and read
    // back to prove nobody changed it in the moment between. While the returned handle is open
    // nobody else can open the file to write, rename or delete it, even on FAT; a reader that shares
    // writing (diskpart reads its /s script with _wfopen, which does; QFile does) still can.
    public static SafeFileHandle WriteHeldReadOnly(string path, byte[] bytes) {
        using (FileStream writer = CreateNew(path)) {
            writer.Write(bytes, 0, bytes.Length);
            writer.Flush(true);
        }
        SafeFileHandle held = HoldReadOnly(path);
        try {
            byte[] back = File.ReadAllBytes(Comparable(path));
            bool same = back.Length == bytes.Length;
            for (int i = 0; same && i < bytes.Length; ++i) same = back[i] == bytes[i];
            if (!same) throw new IOException(path + " changed after it was written");
            return held;
        } catch { held.Dispose(); throw; }
    }

    // Reopen the file @path for reading and share it for reading only: while held, nobody can open
    // it to write, rename or delete it.
    public static SafeFileHandle HoldReadOnly(string path) {
        string full = Comparable(path);
        SafeFileHandle handle = CreateFileW(full, GenericRead, ShareRead, IntPtr.Zero, OpenExisting,
            OpenReparsePoint, IntPtr.Zero);
        if (handle.IsInvalid) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot hold " + full);
        }
        try {
            RequireOrdinary(handle, full, false);
            RequireResolvesTo(handle, full);
            return handle;
        } catch { handle.Dispose(); throw; }
    }
}

// robocopy cannot open a \\?\GLOBALROOT device path, so a backup reads its shadow copy through a
// drive letter mapped to the shadow device in this logon session only (DefineDosDevice), not
// through a directory link in the temporary folder.
public static class ShadowMapping {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool DefineDosDeviceW(uint flags, string device, string target);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint QueryDosDeviceW(string device, StringBuilder target, uint length);
    const uint RawTargetPath = 1, RemoveDefinition = 2, ExactMatchOnRemove = 4;
    const string ShadowDevicePrefix = @"\Device\HarddiskVolumeShadowCopy";

    // "\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy12" -> "\Device\HarddiskVolumeShadowCopy12".
    public static string NtDevice(string deviceObject) {
        const string globalRoot = @"\\?\GLOBALROOT";
        string device = deviceObject.StartsWith(globalRoot, StringComparison.OrdinalIgnoreCase)
            ? deviceObject.Substring(globalRoot.Length) : deviceObject;
        if (!device.StartsWith(ShadowDevicePrefix, StringComparison.OrdinalIgnoreCase)) {
            throw new ArgumentException("Not a shadow copy device: " + deviceObject);
        }
        return device;
    }

    static string Target(string letter) {
        var buffer = new StringBuilder(1024);
        return QueryDosDeviceW(letter, buffer, (uint)buffer.Capacity) == 0 ? null : buffer.ToString();
    }

    // Map a free letter (Z down to D) to the shadow copy; returns "X:".
    public static string Map(string deviceObject) {
        string device = NtDevice(deviceObject);
        for (char c = 'Z'; c >= 'D'; --c) {
            string letter = c + ":";
            if (Target(letter) != null) continue;
            if (DefineDosDeviceW(RawTargetPath, letter, device)) return letter;
        }
        throw new IOException("No free drive letter to read the shadow copy through");
    }

    // Remove exactly this mapping (never one that now names something else). False when it was
    // already gone.
    public static bool Unmap(string letter, string deviceObject) {
        string device = NtDevice(deviceObject);
        string current = Target(letter);
        if (current == null || !current.Split('\0')[0].Equals(device, StringComparison.OrdinalIgnoreCase)) return false;
        if (!DefineDosDeviceW(RawTargetPath | RemoveDefinition | ExactMatchOnRemove, letter, device)) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot remove the shadow copy mapping " + letter);
        }
        return true;
    }
}

}
