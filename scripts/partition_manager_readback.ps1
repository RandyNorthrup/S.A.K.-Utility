<#
.SYNOPSIS
    Reads a disk back with no code from the application, for Partition Manager certification.

.DESCRIPTION
    What a certification row claims is checked against this, not against the application's word,
    the way fsck proved what the Apple writers wrote. It reads the partition table straight off the
    device and parses it here, asks Windows' own tools about volumes and files, and scans raw bytes
    for fixtures a wipe must have destroyed. It shares no code with the application.

    The parsers at the top touch nothing, so the verifier can run them on any machine over the raw
    bytes a snapshot stored, and catch a snapshot whose summary does not match its own bytes. The
    functions below them read real disks. They run only inside the disposable certification VM.
#>

Set-StrictMode -Version 2.0

$Script:RbSectorBytes = 512
$Script:RbGptEntryCountLimit = 256
$Script:RbGptEntryBytes = 128

function ConvertFrom-RbHex {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Hex)
    if (($Hex.Length % 2) -ne 0 -or $Hex -notmatch '^[0-9A-Fa-f]*$') {
        throw 'Readback bytes are not hexadecimal'
    }
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        $bytes[$index] = [Convert]::ToByte($Hex.Substring($index * 2, 2), 16)
    }
    return , $bytes
}

function ConvertTo-RbHex {
    param([Parameter(Mandatory = $true)] [byte[]]$Bytes)
    return ([BitConverter]::ToString($Bytes)).Replace('-', '')
}

function Read-RbMbrEntries {
    param([byte[]]$Sector)
    $entries = @()
    for ($slot = 0; $slot -lt 4; $slot++) {
        $at = 446 + ($slot * 16)
        $type = [int]$Sector[$at + 4]
        if ($type -eq 0) { continue }
        $entries += [pscustomobject]@{
            active = ($Sector[$at] -eq 0x80)
            type = $type
            first_lba = [uint64][BitConverter]::ToUInt32($Sector, $at + 8)
            sector_count = [uint64][BitConverter]::ToUInt32($Sector, $at + 12)
        }
    }
    return , $entries
}

function ConvertFrom-RbPartitionTable {
    <#
    .SYNOPSIS
        Parses what the first sectors of a disk say, and nothing else.
    .PARAMETER Raw
        lba0_hex, plus gpt_header_hex and gpt_entries_hex on GPT, plus ebr_hex for each extended
        boot record on MBR, as Read-RbRawTable stored them.
    #>
    param(
        [Parameter(Mandatory = $true)] [object]$Raw,
        [int]$SectorBytes = $Script:RbSectorBytes
    )

    $sector = ConvertFrom-RbHex -Hex ([string]$Raw.lba0_hex)
    if ($sector.Length -lt 512) { throw 'Readback holds less than a whole first sector' }
    $signed = ($sector[510] -eq 0x55 -and $sector[511] -eq 0xAA)
    $mbrEntries = if ($signed) { Read-RbMbrEntries -Sector $sector } else { @() }
    $protective = @($mbrEntries | Where-Object { $_.type -eq 0xEE }).Count -gt 0
    if (-not $signed) {
        return [pscustomobject]@{ style = 'RAW'; disk_id = ''; partitions = @() }
    }

    if ($protective) {
        $header = ConvertFrom-RbHex -Hex ([string]$Raw.gpt_header_hex)
        if ($header.Length -lt 92 -or [Text.Encoding]::ASCII.GetString($header, 0, 8) -cne 'EFI PART') {
            throw 'Disk has a protective MBR and no GPT header'
        }
        $count = [BitConverter]::ToUInt32($header, 80)
        $entryBytes = [BitConverter]::ToUInt32($header, 84)
        if ($entryBytes -ne $Script:RbGptEntryBytes -or $count -gt $Script:RbGptEntryCountLimit) {
            throw "GPT header holds an unexpected table: $count entries of $entryBytes bytes"
        }
        $table = ConvertFrom-RbHex -Hex ([string]$Raw.gpt_entries_hex)
        $partitions = @()
        for ($slot = 0; $slot -lt $count; $slot++) {
            $at = $slot * $Script:RbGptEntryBytes
            if ($at + $Script:RbGptEntryBytes -gt $table.Length) { break }
            $typeBytes = New-Object byte[] 16
            [Array]::Copy($table, $at, $typeBytes, 0, 16)
            $type = ([Guid]::new($typeBytes)).ToString()
            if ($type -eq '00000000-0000-0000-0000-000000000000') { continue }
            $idBytes = New-Object byte[] 16
            [Array]::Copy($table, $at + 16, $idBytes, 0, 16)
            $first = [BitConverter]::ToUInt64($table, $at + 32)
            $last = [BitConverter]::ToUInt64($table, $at + 40)
            $partitions += [pscustomobject]@{
                slot = $slot + 1
                type = $type
                partition_id = ([Guid]::new($idBytes)).ToString()
                offset_bytes = [uint64]($first * $SectorBytes)
                size_bytes = [uint64](($last - $first + 1) * $SectorBytes)
            }
        }
        $diskIdBytes = New-Object byte[] 16
        [Array]::Copy($header, 56, $diskIdBytes, 0, 16)
        return [pscustomobject]@{
            style = 'GPT'
            disk_id = ([Guid]::new($diskIdBytes)).ToString()
            partitions = @($partitions | Sort-Object offset_bytes)
        }
    }

    $partitions = @()
    $extendedTypes = @(0x05, 0x0F)
    foreach ($entry in $mbrEntries) {
        $partitions += [pscustomobject]@{
            layout = if ($extendedTypes -contains $entry.type) { 'extended' } else { 'primary' }
            active = [bool]$entry.active
            type = '{0:X2}' -f $entry.type
            offset_bytes = [uint64]($entry.first_lba * $SectorBytes)
            size_bytes = [uint64]($entry.sector_count * $SectorBytes)
        }
    }
    $hasEbr = $null -ne $Raw.PSObject.Properties['ebr']
    foreach ($ebr in @($(if ($hasEbr) { $Raw.ebr } else { @() }))) {
        $bytes = ConvertFrom-RbHex -Hex ([string]$ebr.hex)
        if ($bytes.Length -lt 512 -or $bytes[510] -ne 0x55 -or $bytes[511] -ne 0xAA) {
            throw 'Extended boot record is not signed'
        }
        foreach ($entry in (Read-RbMbrEntries -Sector $bytes)) {
            if ($extendedTypes -contains $entry.type) { continue }
            $partitions += [pscustomobject]@{
                layout = 'logical'
                active = $false
                type = '{0:X2}' -f $entry.type
                offset_bytes = [uint64]([uint64]$ebr.lba * $SectorBytes + $entry.first_lba * $SectorBytes)
                size_bytes = [uint64]($entry.sector_count * $SectorBytes)
            }
        }
    }
    return [pscustomobject]@{
        style = 'MBR'
        disk_id = ('{0:X8}' -f [BitConverter]::ToUInt32($sector, 440))
        partitions = @($partitions | Sort-Object offset_bytes)
    }
}

function Assert-RbInDisposableVm {
    # Everything below reads and scans disks. The owner's PC is not somewhere this runs.
    $system = Get-CimInstance -ClassName Win32_ComputerSystem
    if ("$($system.Model)" -cne 'VirtualBox' -or "$($system.Manufacturer)" -notmatch 'innotek') {
        throw "This runs only inside a VirtualBox certification VM, not on '$($system.Manufacturer) $($system.Model)'."
    }
}

function Read-RbBytes {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [uint64]$Offset,
        [Parameter(Mandatory = $true)] [int]$Length
    )
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $stream.Position = [int64]$Offset
        $buffer = New-Object byte[] $Length
        $total = 0
        while ($total -lt $Length) {
            $read = $stream.Read($buffer, $total, $Length - $total)
            if ($read -le 0) { break }
            $total += $read
        }
        if ($total -ne $Length) { throw "Short read of disk ${DiskNumber} at ${Offset}: $total of $Length" }
        return , $buffer
    } finally { $stream.Dispose() }
}

function Read-RbRawTable {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    $first = Read-RbBytes -DiskNumber $DiskNumber -Offset 0 -Length 512
    $raw = [ordered]@{ lba0_hex = ConvertTo-RbHex -Bytes $first }
    $entries = @()
    if ($first[510] -eq 0x55 -and $first[511] -eq 0xAA) { $entries = Read-RbMbrEntries -Sector $first }
    if (@($entries | Where-Object { $_.type -eq 0xEE }).Count -gt 0) {
        $header = Read-RbBytes -DiskNumber $DiskNumber -Offset 512 -Length 512
        $raw.gpt_header_hex = ConvertTo-RbHex -Bytes $header
        $entryLba = [BitConverter]::ToUInt64($header, 72)
        $count = [Math]::Min([BitConverter]::ToUInt32($header, 80), $Script:RbGptEntryCountLimit)
        $raw.gpt_entries_hex = ConvertTo-RbHex -Bytes (Read-RbBytes -DiskNumber $DiskNumber `
            -Offset ($entryLba * 512) -Length ($count * $Script:RbGptEntryBytes))
        return [pscustomobject]$raw
    }
    $extended = @($entries | Where-Object { $_.type -eq 0x05 -or $_.type -eq 0x0F } | Select-Object -First 1)
    $chain = @()
    if ($extended.Count -eq 1) {
        $base = [uint64]$extended[0].first_lba
        $next = $base
        for ($hop = 0; $hop -lt 32; $hop++) {
            $ebr = Read-RbBytes -DiskNumber $DiskNumber -Offset ($next * 512) -Length 512
            $chain += [pscustomobject]@{ lba = [uint64]$next; hex = (ConvertTo-RbHex -Bytes $ebr) }
            $link = @((Read-RbMbrEntries -Sector $ebr) | Where-Object { $_.type -eq 0x05 -or $_.type -eq 0x0F })
            if ($link.Count -eq 0) { break }
            $next = $base + [uint64]$link[0].first_lba
        }
    }
    $raw.ebr = @($chain)
    return [pscustomobject]$raw
}

function Get-RbVolumeFacts {
    param([Parameter(Mandatory = $true)] [string]$Letter)
    $volume = Get-Volume -DriveLetter $Letter -ErrorAction Stop
    $wmi = Get-CimInstance -ClassName Win32_Volume -Filter "DriveLetter='${Letter}:'"
    return [pscustomobject]@{
        drive_letter = $Letter
        file_system = "$($volume.FileSystem)"
        label = "$($volume.FileSystemLabel)"
        size_bytes = [uint64]$volume.Size
        free_bytes = [uint64]$volume.SizeRemaining
        cluster_bytes = [uint64]$wmi.BlockSize
        health = "$($volume.HealthStatus)"
        serial_number = '{0:X8}' -f [uint32]$wmi.SerialNumber
    }
}

function Get-RbDiskFacts {
    <#
    .SYNOPSIS
        What Windows says about a disk. Get-Disk does not list a dynamic one, so that comes from
        Win32_DiskDrive, and its true length from the device: Win32_DiskDrive rounds a size down.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    $disk = Get-Disk -Number $DiskNumber -ErrorAction SilentlyContinue
    if ($disk) {
        return [pscustomobject]@{
            model = "$($disk.FriendlyName)"; serial = "$($disk.SerialNumber)".Trim(); bus_type = "$($disk.BusType)"
            size_bytes = [uint64]$disk.Size; sector_bytes = [int]$disk.LogicalSectorSize
            partition_style = "$($disk.PartitionStyle)"; is_offline = [bool]$disk.IsOffline; unique_id = "$($disk.UniqueId)".Trim()
            listed_by_get_disk = $true
        }
    }
    $drive = Get-CimInstance -ClassName Win32_DiskDrive -Filter "Index=$DiskNumber" -ErrorAction Stop
    if (-not $drive) { throw "No disk $DiskNumber" }
    $first = Read-RbBytes -DiskNumber $DiskNumber -Offset 0 -Length 512
    $gpt = $first[450] -eq 0xEE
    $id = if ($gpt) {
        $header = Read-RbBytes -DiskNumber $DiskNumber -Offset 512 -Length 512
        $guid = New-Object byte[] 16
        [Array]::Copy($header, 56, $guid, 0, 16)
        ([Guid]$guid).ToString('D')
    } else {
        ([BitConverter]::ToUInt32($first, 440)).ToString('x8')
    }
    return [pscustomobject]@{
        model = "$($drive.Model)"; serial = "$($drive.SerialNumber)".Trim(); bus_type = "$($drive.InterfaceType)"
        size_bytes = (Get-RbDeviceLengthBytes -DiskNumber $DiskNumber); sector_bytes = [int]$drive.BytesPerSector
        partition_style = if ($gpt) { 'GPT' } else { 'MBR' }; is_offline = $false; unique_id = "dynamic:$id"
        listed_by_get_disk = $false
    }
}

function Get-RbDeviceLengthBytes {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    if (-not ('RbDeviceLength' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RbDeviceLength {
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputSize, out long output, uint outputSize, out uint returned, IntPtr overlapped);
    public static long Of(SafeFileHandle handle) {
        long length; uint returned;
        if (!DeviceIoControl(handle, 0x0007405C, IntPtr.Zero, 0, out length, 8, out returned, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return length;
    }
}
'@
    }
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try { return [uint64][RbDeviceLength]::Of($stream.SafeFileHandle) } finally { $stream.Dispose() }
}

function Get-RbDiskSnapshot {
    <#
    .SYNOPSIS
        One disk as a separate tool sees it: its raw table, parsed here, and what Windows reports.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    Update-HostStorageCache
    $disk = Get-RbDiskFacts -DiskNumber $DiskNumber
    $raw = Read-RbRawTable -DiskNumber $DiskNumber
    $parsed = ConvertFrom-RbPartitionTable -Raw $raw -SectorBytes ([int]$disk.sector_bytes)
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)) {
        $letter = if ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter)) { "$($partition.DriveLetter)" } else { '' }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
            type = "$($partition.Type)"
            drive_letter = $letter
            volume = if ($letter) { Get-RbVolumeFacts -Letter $letter } else { $null }
        }
    }
    return [pscustomobject]@{
        taken_utc = (Get-Date).ToUniversalTime().ToString('o')
        disk_number = $DiskNumber
        model = "$($disk.model)"
        bus_type = "$($disk.bus_type)"
        size_bytes = [uint64]$disk.size_bytes
        sector_bytes = [int]$disk.sector_bytes
        partition_style_reported = "$($disk.partition_style)"
        is_offline = [bool]$disk.is_offline
        raw = $raw
        table = $parsed
        partitions = $partitions
    }
}

function Get-RbFileManifest {
    <#
    .SYNOPSIS
        Every file under a root, with its length and SHA-256, from Windows' own hashing.
    #>
    param([Parameter(Mandatory = $true)] [string]$Root)
    $lines = New-Object System.Collections.Generic.List[string]
    # A letter another process just assigned is not a drive here until PowerShell looks again.
    $null = Get-PSDrive -PSProvider FileSystem
    $rootPath = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    foreach ($file in @(Get-ChildItem -LiteralPath $rootPath -Recurse -Force -File -ErrorAction Stop |
            Where-Object { $_.FullName -notmatch '\\System Volume Information\\|\\\$RECYCLE\.BIN\\' })) {
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $relative = $file.FullName.Substring($rootPath.Length + 1)
        $lines.Add(("{0}|{1}|{2}" -f $relative, $file.Length, $hash))
    }
    $sorted = @($lines | Sort-Object)
    $sha = [Security.Cryptography.SHA256]::Create()
    $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($sorted -join "`n")))).Replace('-', '').ToLowerInvariant()
    $sha.Dispose()
    return [pscustomobject]@{ root = $rootPath; file_count = $sorted.Count; digest = $digest }
}

function Test-RbVolumeHealth {
    <#
    .SYNOPSIS
        Windows' own check of a volume: the repair scan, chkdsk's read-only scan, and the dirty bit.
    #>
    param([Parameter(Mandatory = $true)] [string]$Letter)
    $scan = "$(Repair-Volume -DriveLetter $Letter -Scan -ErrorAction Stop)"
    $check = & chkdsk.exe "${Letter}:" /scan 2>&1 | Out-String
    $checkExit = $LASTEXITCODE
    $dirty = & fsutil.exe dirty query "${Letter}:" 2>&1 | Out-String
    return [pscustomobject]@{
        repair_scan = $scan
        chkdsk_exit_code = [int]$checkExit
        dirty = ($dirty -notmatch 'NOT Dirty')
    }
}

function Write-RbFixtureTree {
    <#
    .SYNOPSIS
        Files a row expects to survive: varied sizes, a deep path, a Unicode name, known contents.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [int]$FileCount = 24,
        [int]$MaxKilobytes = 2048,
        [int]$Seed = 20261104
    )
    $random = [Random]::new($Seed)
    $deep = Join-Path $Root 'sub dir\one\two\three'
    New-Item -ItemType Directory -Force -Path $deep | Out-Null
    for ($index = 0; $index -lt $FileCount; $index++) {
        $folder = if ($index % 3 -eq 0) { $deep } else { $Root }
        $name = if ($index % 5 -eq 0) { "fixture-caf$([char]0xE9)-$index.bin" } else { "fixture-$index.bin" }
        $bytes = New-Object byte[] ($random.Next(1, $MaxKilobytes) * 1024)
        $random.NextBytes($bytes)
        [IO.File]::WriteAllBytes((Join-Path $folder $name), $bytes)
    }
    return Get-RbFileManifest -Root $Root
}

function Find-RbMarkerHits {
    <#
    .SYNOPSIS
        How many chosen places on a disk still hold a marker. Reads only those places, and stores
        only counts: nothing read from the disk reaches evidence.
    #>
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [string]$Marker,
        [Parameter(Mandatory = $true)] [uint64[]]$Offsets
    )
    $wanted = [Text.Encoding]::ASCII.GetBytes($Marker)
    $hits = 0
    $zero = 0
    foreach ($offset in $Offsets) {
        $bytes = Read-RbBytes -DiskNumber $DiskNumber -Offset $offset -Length 4096
        $match = $true
        for ($index = 0; $index -lt $wanted.Length; $index++) {
            if ($bytes[$index] -ne $wanted[$index]) { $match = $false; break }
        }
        if ($match) { $hits++ }
        if (@($bytes | Where-Object { $_ -ne 0 }).Count -eq 0) { $zero++ }
    }
    return [pscustomobject]@{ places_read = $Offsets.Count; marker_hits = $hits; all_zero_places = $zero }
}

function Find-RbMarkerAnywhere {
    <#
    .SYNOPSIS
        Reads a whole small disk for a marker. For a VHD or a VDI, never for a real large drive.
    #>
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [string]$Marker,
        [uint64]$MaximumBytes = 4GB
    )
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ([uint64]$disk.Size -gt $MaximumBytes) { throw "Disk $DiskNumber is too large to read whole" }
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $wanted = [Text.Encoding]::ASCII.GetBytes($Marker)
        $buffer = New-Object byte[] (1MB)
        $carry = $wanted.Length - 1
        $hits = 0
        $nonZeroBlocks = 0
        $bytesRead = [uint64]0
        $text = [Text.Encoding]::GetEncoding(28591)
        $pattern = $text.GetString($wanted)
        $previous = ''
        $size = [uint64]$disk.Size
        while ($bytesRead -lt $size) {
            # A raw read past the end of a disk fails rather than coming back short.
            $want = [int][Math]::Min([uint64]$buffer.Length, $size - $bytesRead)
            $read = $stream.Read($buffer, 0, $want)
            if ($read -le 0) { throw "Short read of disk $DiskNumber at $bytesRead" }
            $bytesRead += [uint64]$read
            $chunk = $previous + $text.GetString($buffer, 0, $read)
            if ($chunk.Trim([char]0).Length -gt 0) { $nonZeroBlocks++ }
            $from = 0
            while (($at = $chunk.IndexOf($pattern, $from, [StringComparison]::Ordinal)) -ge 0) {
                $hits++
                $from = $at + $pattern.Length
            }
            $previous = if ($chunk.Length -gt $carry) { $chunk.Substring($chunk.Length - $carry) } else { $chunk }
        }
        return [pscustomobject]@{ bytes_read = $bytesRead; marker_hits = $hits; non_zero_blocks = $nonZeroBlocks }
    } finally { $stream.Dispose() }
}

function Get-RbRangeDifferences {
    <#
    .SYNOPSIS
        Where two byte ranges differ, each a disk or a file, as merged runs of sectors. Reports
        where and how much, and never the bytes.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$LeftPath,
        [uint64]$LeftOffset = 0,
        [Parameter(Mandatory = $true)] [string]$RightPath,
        [uint64]$RightOffset = 0,
        [Parameter(Mandatory = $true)] [uint64]$Length,
        [int]$MaximumRuns = 256
    )
    if (-not ('SakCertRangeCompare' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
public static class SakCertRangeCompare {
    private const int Sector = 512;
    private const int Block = 1048576;
    private static int Fill(FileStream stream, byte[] buffer, int count) {
        int have = 0;
        while (have < count) {
            int read = stream.Read(buffer, have, count - have);
            if (read <= 0) throw new EndOfStreamException("Short read of " + stream.Name);
            have += read;
        }
        return have;
    }
    public static long[][] Run(string left, long leftOffset, string right, long rightOffset, long length, int maximumRuns) {
        var runs = new List<long[]>();
        using (var a = new FileStream(left, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (var b = new FileStream(right, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            a.Position = leftOffset;
            b.Position = rightOffset;
            var ba = new byte[Block];
            var bb = new byte[Block];
            long done = 0;
            while (done < length) {
                int want = (int)Math.Min((long)Block, length - done);
                Fill(a, ba, want);
                Fill(b, bb, want);
                for (int sector = 0; sector < want; sector += Sector) {
                    int size = Math.Min(Sector, want - sector);
                    bool same = true;
                    for (int i = 0; i < size; i++) { if (ba[sector + i] != bb[sector + i]) { same = false; break; } }
                    if (same) continue;
                    long at = done + sector;
                    if (runs.Count > 0 && runs[runs.Count - 1][0] + runs[runs.Count - 1][1] == at) {
                        runs[runs.Count - 1][1] += size;
                    } else {
                        if (runs.Count >= maximumRuns) return runs.ToArray();
                        runs.Add(new long[] { at, size });
                    }
                }
                done += want;
            }
        }
        return runs.ToArray();
    }
}
'@
    }
    $runs = [SakCertRangeCompare]::Run($LeftPath, [int64]$LeftOffset, $RightPath, [int64]$RightOffset, [int64]$Length, $MaximumRuns)
    return @($runs | ForEach-Object { [pscustomobject]@{ offset_bytes = "$($_[0])"; length_bytes = "$($_[1])" } })
}

function Get-RbRangeSha256 {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [uint64]$Offset,
        [Parameter(Mandatory = $true)] [uint64]$Length
    )
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream.Position = [int64]$Offset
        $buffer = New-Object byte[] (4MB)
        $left = $Length
        while ($left -gt 0) {
            $want = [int][Math]::Min([uint64]$buffer.Length, $left)
            $read = $stream.Read($buffer, 0, $want)
            if ($read -le 0) { throw "Short read of disk ${DiskNumber} at $($stream.Position)" }
            [void]$sha.TransformBlock($buffer, 0, $read, $null, 0)
            $left -= [uint64]$read
        }
        [void]$sha.TransformFinalBlock($buffer, 0, 0)
        return ([BitConverter]::ToString($sha.Hash)).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-RbProcessElevation {
    <#
    .SYNOPSIS
        Whether a running process holds an elevated token, asked of Windows and not of the process.
    #>
    param([Parameter(Mandatory = $true)] [int]$ProcessId)
    if (-not ('RbToken' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RbToken {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int cls, out int value, int size, out int returned);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static int Elevated(int pid) {
        IntPtr process = OpenProcess(0x1000, false, pid);
        if (process == IntPtr.Zero) return -1;
        try {
            IntPtr token;
            if (!OpenProcessToken(process, 8, out token)) return -1;
            try {
                int value; int returned;
                if (!GetTokenInformation(token, 20, out value, 4, out returned)) return -1;
                return value;
            } finally { CloseHandle(token); }
        } finally { CloseHandle(process); }
    }
}
'@
    }
    $state = [RbToken]::Elevated($ProcessId)
    if ($state -lt 0) { return $null }
    return ($state -ne 0)
}
