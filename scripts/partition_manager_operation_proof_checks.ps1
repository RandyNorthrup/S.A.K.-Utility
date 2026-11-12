<#
.SYNOPSIS
    The operation proof's own readers: what a separate tool says about a volume, a partition table
    entry, or a file system image, compared with what an operation should have left.

.DESCRIPTION
    Dot-sourced by partition_manager_operation_proof_rows.ps1. Nothing here is the application's
    code or calls it. A Windows file system is read by Windows (Get-Volume, Win32_Volume, chkdsk's
    read-only scan, Repair-Volume -Scan). A partition table entry is read from the raw sectors. A
    non-Windows file system is copied off the disk into an image file and read there: its
    superblock or header parsed here, and the bundled third-party checker (e2fsck -fn, fsck_hfs -n)
    run on the copy, never on the disk. The images also go back to the host, where Linux's own
    e2fsprogs and apfsprogs read them again (scripts/run_partition_manager_operation_proof.ps1).

    A check is a name, whether it held, and what was read. A row's verdict is PASS only when every
    step did what the row expected and every check held.
#>

$Script:OpsMiB = [uint64]1048576
$Script:OpsGptBasicData = 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'
$Script:OpsGptLinuxData = '0fc63daf-8483-4772-8e79-3d69d8477de4'
# Parsed from text: Windows PowerShell reads 0x8000000000000000 as a negative Int64.
$Script:OpsGptHiddenAttribute = [Convert]::ToUInt64('4000000000000000', 16)
$Script:OpsGptNoDriveLetterAttribute = [Convert]::ToUInt64('8000000000000000', 16)
$Script:OpsImageChunkBytes = 4MB
# What e2fsck -n says, exit code 0 and all, about a file system larger than its partition: it
# answers its own "Abort?" with no and goes on (ops.resize-unknown-fs-below-fs, b28, 2026-11-11).
$Script:OpsE2fsckAlarm = 'likely to be corrupt|physical size of the device|Abort\? no'

function New-OpsCheck {
    param([Parameter(Mandatory = $true)] [string]$Name, [bool]$Passed, [AllowNull()] [object]$Detail = '')
    return [ordered]@{ name = $Name; passed = [bool]$Passed; detail = $Detail }
}

function Get-OpsChkdsk {
    <#
    .SYNOPSIS
        chkdsk without /f: it reads and reports, and repairs nothing. NTFS gets the online scan.
    #>
    param([Parameter(Mandatory = $true)] [string]$Letter, [Parameter(Mandatory = $true)] [string]$FileSystem)
    # @( ) keeps one argument an array: a lone string splatted goes in one character at a time.
    $arguments = @(if ($FileSystem -eq 'NTFS') { "${Letter}:"; '/scan' } else { "${Letter}:" })
    $output = & chkdsk.exe @arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    $lines = @($output -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 8)
    return [pscustomobject]@{ exit_code = [int]$code; tail = ($lines -join ' | ') }
}

function Test-OpsWindowsVolume {
    <#
    .SYNOPSIS
        Windows' own account of a volume against what the operation was asked for.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$Prefix,
        [Parameter(Mandatory = $true)] [string]$Letter,
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [AllowNull()] [object]$Label = $null,
        [uint64]$ClusterBytes = 0
    )
    $null = Get-PSDrive -PSProvider FileSystem
    $checks = @()
    $facts = try { Get-RbVolumeFacts -Letter $Letter } catch { $null }
    $checks += New-OpsCheck -Name "$Prefix volume is mounted at ${Letter}:" -Passed ($null -ne $facts) -Detail "$Letter"
    if ($null -eq $facts) { return , $checks }
    $checks += New-OpsCheck -Name "$Prefix file system is $FileSystem" -Passed ($facts.file_system -eq $FileSystem) -Detail $facts.file_system
    if ($null -ne $Label) { $checks += New-OpsCheck -Name "$Prefix label is '$Label'" -Passed ($facts.label -ceq $Label) -Detail $facts.label }
    if ($ClusterBytes -gt 0) { $checks += New-OpsCheck -Name "$Prefix cluster is $ClusterBytes bytes" -Passed ([uint64]$facts.cluster_bytes -eq $ClusterBytes) -Detail "$($facts.cluster_bytes)" }
    if ($FileSystem -eq 'ReFS') {
        $scan = "$(Repair-Volume -DriveLetter $Letter -Scan -ErrorAction SilentlyContinue)"
        $checks += New-OpsCheck -Name "$Prefix Repair-Volume -Scan finds no errors" -Passed ($scan -eq 'NoErrorsFound') -Detail $scan
    } else {
        $chkdsk = Get-OpsChkdsk -Letter $Letter -FileSystem $FileSystem
        $checks += New-OpsCheck -Name "$Prefix chkdsk read-only scan exits 0" -Passed ($chkdsk.exit_code -eq 0) -Detail $chkdsk
    }
    return , $checks
}

function Get-OpsGptEntry {
    <#
    .SYNOPSIS
        One GPT entry straight from the disk's sectors: its type, attributes and extent.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [uint64]$OffsetBytes)
    $header = Read-RbBytes -DiskNumber $DiskNumber -Offset 512 -Length 512
    if ([Text.Encoding]::ASCII.GetString($header, 0, 8) -cne 'EFI PART') { throw "Disk $DiskNumber has no GPT header" }
    $entryLba = [BitConverter]::ToUInt64($header, 72)
    $count = [BitConverter]::ToUInt32($header, 80)
    $size = [BitConverter]::ToUInt32($header, 84)
    $table = Read-RbBytes -DiskNumber $DiskNumber -Offset ($entryLba * 512) -Length ([int]($count * $size))
    for ($slot = 0; $slot -lt $count; $slot++) {
        $at = $slot * $size
        $first = [BitConverter]::ToUInt64($table, $at + 32)
        if ($first * 512 -ne $OffsetBytes) { continue }
        $typeBytes = New-Object byte[] 16
        [Array]::Copy($table, $at, $typeBytes, 0, 16)
        return [pscustomobject]@{
            slot = $slot + 1
            type = ([Guid]::new($typeBytes)).ToString()
            attributes = [BitConverter]::ToUInt64($table, $at + 48)
            first_lba = $first
            last_lba = [BitConverter]::ToUInt64($table, $at + 40)
        }
    }
    return $null
}

function Get-OpsMbrEntry {
    <#
    .SYNOPSIS
        One primary MBR entry straight from sector 0: its boot flag and type byte.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [uint64]$OffsetBytes)
    $sector = Read-RbBytes -DiskNumber $DiskNumber -Offset 0 -Length 512
    for ($slot = 0; $slot -lt 4; $slot++) {
        $at = 446 + ($slot * 16)
        $first = [uint64][BitConverter]::ToUInt32($sector, $at + 8)
        if ($sector[$at + 4] -eq 0 -or $first * 512 -ne $OffsetBytes) { continue }
        return [pscustomobject]@{ slot = $slot + 1; boot_flag = ('{0:X2}' -f $sector[$at]); type = ('{0:X2}' -f $sector[$at + 4]) }
    }
    return $null
}

function Copy-OpsPartitionImage {
    <#
    .SYNOPSIS
        Copies a partition's bytes off the disk into an image file, so a checker reads the copy and
        never the disk. Returns where it went and the copy's SHA-256.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [int]$PartitionNumber, [Parameter(Mandatory = $true)] [string]$Name)
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    $folder = Join-Path $OutputRoot 'images'
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $path = Join-Path $folder "$($Script:RowId)-$Name.img"
    $source = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $target = [IO.File]::Create($path)
    try {
        $source.Position = [int64]$partition.Offset
        $buffer = New-Object byte[] $Script:OpsImageChunkBytes
        $left = [uint64]$partition.Size
        while ($left -gt 0) {
            $want = [int][Math]::Min([uint64]$buffer.Length, $left)
            $read = $source.Read($buffer, 0, $want)
            if ($read -le 0) { throw "Short read of disk $DiskNumber partition $PartitionNumber" }
            $target.Write($buffer, 0, $read)
            $left -= [uint64]$read
        }
    } finally { $target.Dispose(); $source.Dispose() }
    return [pscustomobject]@{
        path = $path
        name = (Split-Path -Leaf $path)
        offset_bytes = [uint64]$partition.Offset
        size_bytes = [uint64]$partition.Size
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Read-OpsImageBytes {
    param([Parameter(Mandatory = $true)] [string]$Path, [uint64]$Offset, [int]$Length)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $stream.Position = [int64]$Offset
        $buffer = New-Object byte[] $Length
        $read = $stream.Read($buffer, 0, $Length)
        if ($read -ne $Length) { throw "Short read of $Path at $Offset" }
        return , $buffer
    } finally { $stream.Dispose() }
}

function Get-OpsBigEndian32 {
    param([byte[]]$Bytes, [int]$At)
    return [uint32](([uint32]$Bytes[$At] -shl 24) -bor ([uint32]$Bytes[$At + 1] -shl 16) -bor ([uint32]$Bytes[$At + 2] -shl 8) -bor [uint32]$Bytes[$At + 3])
}

function Get-OpsTool {
    param([Parameter(Mandatory = $true)] [string]$Relative)
    $path = Join-Path $StageRoot $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "The stage has no $Relative" }
    return $path
}

function Get-OpsReader {
    # A reader that is not part of the app sits beside the stage, never in it: the app's elevation
    # check refuses an unknown program in its own folder.
    param([Parameter(Mandatory = $true)] [string]$Name)
    $path = Join-Path "$StageRoot.readers" $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "No $Name in $StageRoot.readers" }
    return $path
}

function Invoke-OpsTool {
    <#
    .SYNOPSIS
        Runs a bundled checker on an image copy and keeps its exit code and the end of what it said.
    #>
    param([Parameter(Mandatory = $true)] [string]$Tool, [string[]]$Arguments = @(), [string]$Alarm = '')
    $ErrorActionPreference = 'Continue'
    $output = & $Tool @Arguments 2>&1 | ForEach-Object { "$_" } | Out-String
    $code = $LASTEXITCODE
    $all = @($output -split "`r?`n" | Where-Object { $_.Trim() })
    $lines = @($all | Select-Object -Last 12)
    # Lines anywhere in the output that fail the check whatever the exit code says.
    $alarms = @(if ($Alarm) { $all | Where-Object { $_ -match $Alarm } | ForEach-Object { $_.Trim() } })
    return [pscustomobject]@{ tool = (Split-Path -Leaf $Tool); arguments = ($Arguments -join ' '); exit_code = [int]$code; tail = ($lines -join ' | '); alarms = ($alarms -join ' | ') }
}

function Get-OpsExtSuperblock {
    <#
    .SYNOPSIS
        The ext superblock, parsed here from the image: which of ext2/3/4 it is, its size and label.
    #>
    param([Parameter(Mandatory = $true)] [string]$Path)
    $sb = Read-OpsImageBytes -Path $Path -Offset 1024 -Length 1024
    $magic = [BitConverter]::ToUInt16($sb, 56)
    if ($magic -ne 0xEF53) { return [pscustomobject]@{ magic = ('{0:X4}' -f $magic); kind = '' } }
    $blockSize = [uint64](1024 -shl [BitConverter]::ToUInt32($sb, 24))
    $compat = [BitConverter]::ToUInt32($sb, 92)
    $incompat = [BitConverter]::ToUInt32($sb, 96)
    $journal = ($compat -band 0x4) -ne 0
    $extents = ($incompat -band 0x40) -ne 0
    $kind = if ($extents) { 'ext4' } elseif ($journal) { 'ext3' } else { 'ext2' }
    $blocks = [uint64][BitConverter]::ToUInt32($sb, 4)
    if (($incompat -band 0x80) -ne 0) { $blocks += ([uint64][BitConverter]::ToUInt32($sb, 336)) -shl 32 }
    return [pscustomobject]@{
        magic = 'EF53'
        kind = $kind
        has_journal = $journal
        extents = $extents
        block_size = $blockSize
        file_system_bytes = [uint64]($blocks * $blockSize)
        state_clean = (([BitConverter]::ToUInt16($sb, 58) -band 0x1) -ne 0)
        label = ([Text.Encoding]::ASCII.GetString($sb, 120, 16)).TrimEnd([char]0)
    }
}

function Test-OpsExtImage {
    param(
        [Parameter(Mandatory = $true)] [string]$Prefix,
        [Parameter(Mandatory = $true)] [object]$Image,
        [Parameter(Mandatory = $true)] [string]$Kind,
        [AllowNull()] [object]$Label = $null
    )
    $sb = Get-OpsExtSuperblock -Path $Image.path
    $checks = @(New-OpsCheck -Name "$Prefix superblock says $Kind" -Passed ($sb.kind -eq $Kind) -Detail $sb)
    if ($sb.kind) {
        $checks += New-OpsCheck -Name "$Prefix file system fits its partition" -Passed ($sb.file_system_bytes -le $Image.size_bytes -and $sb.file_system_bytes -ge ($Image.size_bytes - 4 * $Script:OpsMiB)) -Detail "$($sb.file_system_bytes) of $($Image.size_bytes)"
        if ($null -ne $Label) { $checks += New-OpsCheck -Name "$Prefix label is '$Label'" -Passed ($sb.label -ceq $Label) -Detail $sb.label }
    }
    $e2fsck = Invoke-OpsTool -Tool (Get-OpsTool 'tools\filesystem\e2fsprogs\e2fsck.exe') -Arguments @('-fn', $Image.path) -Alarm $Script:OpsE2fsckAlarm
    $checks += New-OpsCheck -Name "$Prefix bundled e2fsck -fn on a copy exits 0 and finds the file system whole" -Passed ($e2fsck.exit_code -eq 0 -and -not $e2fsck.alarms) -Detail $e2fsck
    return , $checks
}

function Test-OpsHfsImage {
    param(
        [Parameter(Mandatory = $true)] [string]$Prefix,
        [Parameter(Mandatory = $true)] [object]$Image,
        [Parameter(Mandatory = $true)] [ValidateSet('HFS+', 'HFSX')] [string]$Kind
    )
    $header = Read-OpsImageBytes -Path $Image.path -Offset 1024 -Length 512
    $signature = [Text.Encoding]::ASCII.GetString($header, 0, 2)
    $wanted = if ($Kind -eq 'HFSX') { 'HX' } else { 'H+' }
    $blockSize = Get-OpsBigEndian32 -Bytes $header -At 40
    $total = Get-OpsBigEndian32 -Bytes $header -At 44
    $checks = @(New-OpsCheck -Name "$Prefix volume header signature is $wanted" -Passed ($signature -ceq $wanted) -Detail $signature)
    $checks += New-OpsCheck -Name "$Prefix volume fits its partition" -Passed (([uint64]$blockSize * $total) -le $Image.size_bytes -and $blockSize -gt 0) -Detail "$total blocks of $blockSize"
    $fsck = Invoke-OpsTool -Tool (Get-OpsTool 'tools\filesystem\hfsprogs\fsck_hfs.exe') -Arguments @('-n', '-f', $Image.path)
    $checks += New-OpsCheck -Name "$Prefix bundled fsck_hfs -n -f on a copy exits 0" -Passed ($fsck.exit_code -eq 0) -Detail $fsck
    return , $checks
}

function Test-OpsApfsImage {
    param(
        [Parameter(Mandatory = $true)] [string]$Prefix,
        [Parameter(Mandatory = $true)] [object]$Image
    )
    $block = Read-OpsImageBytes -Path $Image.path -Offset 0 -Length 4096
    $magic = [Text.Encoding]::ASCII.GetString($block, 32, 4)
    $checks = @(New-OpsCheck -Name "$Prefix container superblock magic is NXSB" -Passed ($magic -ceq 'NXSB') -Detail $magic)
    # The probe certifier reads with the app's own reader, so it is the weakest of the checks here;
    # the host's apfsck and macOS's fsck_apfs read the same image independently.
    $output = Join-Path $OutputRoot "images\$($Image.name).probe.json"
    $probe = Invoke-OpsTool -Tool (Get-OpsReader 'partition_filesystem_probe_certifier.exe') -Arguments @('--input', $Image.path, '--output', $output, '--expect', 'APFS', '--require-sane')
    $checks += New-OpsCheck -Name "$Prefix probe certifier reads it as sane APFS" -Passed ($probe.exit_code -eq 0) -Detail $probe
    return , $checks
}

function Test-OpsSwap {
    <#
    .SYNOPSIS
        A Linux swap header read from the partition: SWAPSPACE2 at the end of the first page,
        version 1, a page count that fits, and the label.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$Prefix,
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [int]$PartitionNumber,
        [int]$PageBytes = 4096,
        [AllowNull()] [object]$Label = $null
    )
    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber
    $page = Read-RbBytes -DiskNumber $DiskNumber -Offset ([uint64]$partition.Offset) -Length $PageBytes
    $signature = [Text.Encoding]::ASCII.GetString($page, $PageBytes - 10, 10)
    $version = [BitConverter]::ToUInt32($page, 1024)
    $lastPage = [uint64][BitConverter]::ToUInt32($page, 1028)
    $name = ([Text.Encoding]::ASCII.GetString($page, 1052, 16)).TrimEnd([char]0)
    $checks = @(New-OpsCheck -Name "$Prefix SWAPSPACE2 signature" -Passed ($signature -ceq 'SWAPSPACE2') -Detail $signature)
    $checks += New-OpsCheck -Name "$Prefix swap header version 1" -Passed ($version -eq 1) -Detail "$version"
    $checks += New-OpsCheck -Name "$Prefix swap pages fit the partition" -Passed ($lastPage -gt 0 -and ($lastPage + 1) * [uint64]$PageBytes -le [uint64]$partition.Size) -Detail "$lastPage"
    if ($null -ne $Label) { $checks += New-OpsCheck -Name "$Prefix swap label is '$Label'" -Passed ($name -ceq $Label) -Detail $name }
    return , $checks
}

function Find-OpsMarkerInRange {
    <#
    .SYNOPSIS
        Counts a marker in one byte range of a disk, read straight from the device.
    #>
    param([int]$DiskNumber, [uint64]$Offset, [uint64]$Length, [string]$Marker = $Script:Marker)
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $text = [Text.Encoding]::GetEncoding(28591)
        $pattern = $Marker
        $buffer = New-Object byte[] (1MB)
        $stream.Position = [int64]$Offset
        $left = $Length
        $hits = 0
        $previous = ''
        while ($left -gt 0) {
            $want = [int][Math]::Min([uint64]$buffer.Length, $left)
            $read = $stream.Read($buffer, 0, $want)
            if ($read -le 0) { throw "Short read of disk $DiskNumber" }
            $left -= [uint64]$read
            $chunk = $previous + $text.GetString($buffer, 0, $read)
            $from = 0
            while (($at = $chunk.IndexOf($pattern, $from, [StringComparison]::Ordinal)) -ge 0) { $hits++; $from = $at + $pattern.Length }
            $previous = if ($chunk.Length -ge $pattern.Length) { $chunk.Substring($chunk.Length - $pattern.Length + 1) } else { $chunk }
        }
        return [pscustomobject]@{ offset_bytes = $Offset; bytes_read = $Length; marker_hits = $hits }
    } finally { $stream.Dispose() }
}

function Test-OpsFilesSame {
    param([Parameter(Mandatory = $true)] [string]$Name, [object]$Before, [object]$After)
    $same = ($null -ne $Before -and $null -ne $After -and $Before.digest -eq $After.digest -and $Before.file_count -eq $After.file_count)
    return New-OpsCheck -Name $Name -Passed $same -Detail ([ordered]@{ before = $Before; after = $After })
}

function Get-OpsShadowMappings {
    <#
    .SYNOPSIS
        Drive letters that DefineDosDevice points at a shadow copy, as QueryDosDevice reports them
        to this logon session (the elevated Technician session the helper runs in).
    #>
    if (-not ('OpsDosDevice' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OpsDosDevice {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint QueryDosDevice(string name, StringBuilder target, int max);
    public static string Target(string name) {
        var buffer = new StringBuilder(4096);
        return QueryDosDevice(name, buffer, buffer.Capacity) == 0 ? "" : buffer.ToString();
    }
}
'@
    }
    $found = @()
    foreach ($letter in [char[]](65..90)) {
        $target = [OpsDosDevice]::Target("${letter}:")
        if ($target -match 'ShadowCopy') { $found += "${letter}: -> $target" }
    }
    return , $found
}
