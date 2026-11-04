<#
.SYNOPSIS
    What each Partition Manager certification row does: its fixture, its steps, what it reads back.

.DESCRIPTION
    Dot-sourced by run_partition_manager_app_certification.ps1, which holds the helpers. A body
    returns its media and its readback, and the matrix says what the readback must show. A fixture
    is made with Windows' own tools, because a fixture is not what is under test. Every change a row
    claims goes through Invoke-CertStep.
#>

$Script:CertMiB = [uint64]1048576

function New-CertGptVhd {
    param([string]$Name, [int]$SizeMB = 1024)
    $disk = New-CertVhd -Name $Name -SizeMB $SizeMB
    Initialize-Disk -Number $disk -PartitionStyle GPT
    return $disk
}

function Get-CertReadPin {
    <#
    .SYNOPSIS
        The disk a row may change, named by the pin file or by its exact size on the virtual bus.
        Asked again immediately before every step, because disk numbers change.
    #>
    param([Parameter(Mandatory = $true)] [ValidateSet('ssd', 'hdd', 'any')] [string]$Role)
    if (-not [string]::IsNullOrWhiteSpace($PhysicalPins)) {
        $pins = Get-Content -LiteralPath $PhysicalPins -Raw | ConvertFrom-Json
        $wanted = $pins.$Role
        if ($null -eq $wanted) { throw "The pin file names no $Role disk" }
        $found = @(Get-Disk | Where-Object {
                "$($_.FriendlyName)" -ceq "$($wanted.model)" -and [uint64]$_.Size -eq [uint64]$wanted.size_bytes -and
                "$($_.SerialNumber)".Trim() -ceq "$($wanted.serial_number)" })
        if ($found.Count -ne 1) { throw "Expected one disk that is $($wanted.model), $($wanted.size_bytes) bytes, that serial: found $($found.Count)" }
        $disk = $found[0]
        if ($disk.IsBoot -or $disk.IsSystem) { throw 'The pinned disk runs this Windows' }
        if ("$($disk.BusType)" -cne "$($wanted.bus_type)") { throw "The pinned disk is on $($disk.BusType), not $($wanted.bus_type)" }
        $pin = Get-CertPin -DiskNumber $disk.Number
        if ($pin.confirmation -cne "$($wanted.confirmation)") { throw 'The typed confirmation does not name this disk' }
        return [pscustomobject]@{ number = [int]$disk.Number; pin = $pin; class = 'physical'; kind = (Get-CertDeviceKind -BusType "$($disk.BusType)") }
    }
    if ($DataDiskBytes -eq 0) { throw 'This row needs -PhysicalPins or -DataDiskBytes' }
    $found = @(Get-Disk | Where-Object { [uint64]$_.Size -eq $DataDiskBytes })
    if ($found.Count -ne 1) { throw "Expected one disk of $DataDiskBytes bytes: found $($found.Count)" }
    $disk = $found[0]
    if ($disk.IsBoot -or $disk.IsSystem -or "$($disk.FriendlyName)" -notmatch 'VBOX HARDDISK') { throw "Disk $($disk.Number) is not a throwaway VDI" }
    return [pscustomobject]@{ number = [int]$disk.Number; pin = (Get-CertPin -DiskNumber $disk.Number); class = 'virtual_machine'; kind = 'VirtualBox VDI' }
}

function Get-CertDataFixture {
    <#
    .SYNOPSIS
        Where a data row makes its volumes: a VHD in the guest, or small partitions on the pinned
        hard drive. It never makes one larger than the row needs, and leaves the rest of that drive
        alone: old data remains on it, and nothing here reads it.
    #>
    param([string]$Name, [int]$SizeMB = 1024, [string]$Style = 'GPT')
    if ($DataRowsOnDevice) {
        $found = Get-CertReadPin -Role 'hdd'
        $Script:RowDeviceRole = 'hdd'
        return [pscustomobject]@{ number = $found.number; media = (Get-CertMedia -DiskNumber $found.number -Class $found.class -Kind $found.kind) }
    }
    $disk = New-CertVhd -Name $Name -SizeMB $SizeMB
    Initialize-Disk -Number $disk -PartitionStyle $Style
    return [pscustomobject]@{ number = $disk; media = (Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind) }
}

function Remove-CertDevicePartitions {
    # What a row made on the pinned drive goes away again. Its EFI partition stays, by type.
    if (-not $Script:RowDeviceRole) { return }
    $found = Get-CertReadPin -Role $Script:RowDeviceRole
    foreach ($partition in @(Get-Partition -DiskNumber $found.number -ErrorAction SilentlyContinue | Where-Object { "$($_.Type)" -eq 'Basic' })) {
        Remove-Partition -DiskNumber $found.number -PartitionNumber $partition.PartitionNumber -Confirm:$false
    }
    $Script:RowDeviceRole = ''
}

function Get-CertFirstFreeOffset {
    param([int]$DiskNumber)
    $end = [uint64]1048576
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)) {
        $thisEnd = [uint64]$partition.Offset + [uint64]$partition.Size
        if ($thisEnd -gt $end) { $end = $thisEnd }
    }
    $mib = [uint64]1048576
    if (($end % $mib) -ne 0) { $end = $end + $mib - ($end % $mib) }
    return $end
}

function Clear-CertWholeDisk {
    # Only a disk a row owns outright: a VDI, or the NVMe the owner gave to this.
    param([int]$DiskNumber)
    $disk = Get-Disk -Number $DiskNumber
    if ($disk.IsOffline) { Set-Disk -Number $DiskNumber -IsOffline $false }
    if ($disk.IsReadOnly) { Set-Disk -Number $DiskNumber -IsReadOnly $false }
    if ("$($disk.PartitionStyle)" -ne 'RAW') { Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false }
    Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
    Start-Sleep -Seconds 2
}

function Initialize-CertBlankDisk {
    # A new VDI has no table, and a fixture needs one. A disk that has one keeps it.
    param([int]$DiskNumber)
    if ("$((Get-Disk -Number $DiskNumber).PartitionStyle)" -eq 'RAW') {
        Initialize-Disk -Number $DiskNumber -PartitionStyle GPT
        Start-Sleep -Seconds 2
    }
}

function Write-CertRawMarkers {
    <#
    .SYNOPSIS
        Puts a marker at chosen places across a whole disk, the way nothing else would put it there.
    #>
    param([int]$DiskNumber, [int]$Count = 64)
    $disk = Get-Disk -Number $DiskNumber
    $size = [uint64]$disk.Size
    # Past the table and Windows' reserved partition: Windows refuses a raw write to a partition.
    $start = [uint64](32MB)
    $step = [uint64](($size - $start - 16MB) / [uint64]$Count)
    $step -= $step % 4096
    $offsets = New-Object System.Collections.Generic.List[uint64]
    $bytes = New-Object byte[] 4096
    $marker = [Text.Encoding]::ASCII.GetBytes($Script:Marker)
    [Array]::Copy($marker, $bytes, $marker.Length)
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        for ($index = 0; $index -lt $Count; $index++) {
            $offset = $start + ($step * [uint64]$index)
            $stream.Position = [int64]$offset
            $stream.Write($bytes, 0, $bytes.Length)
            $offsets.Add($offset)
        }
        $stream.Flush()
    } finally { $stream.Dispose() }
    return , $offsets.ToArray()
}

function Write-CertMarkerFile {
    param([string]$Letter, [string]$Name = 'marker.bin', [int]$Kilobytes = 256)
    $line = [Text.Encoding]::ASCII.GetBytes($Script:Marker)
    $bytes = New-Object byte[] ($Kilobytes * 1024)
    for ($at = 0; $at -lt $bytes.Length; $at += 512) { [Array]::Copy($line, 0, $bytes, $at, $line.Length) }
    [IO.File]::WriteAllBytes("${Letter}:\$Name", $bytes)
    Write-VolumeCache -DriveLetter $Letter
}

function Set-CertDiskOffline {
    param([int]$DiskNumber)
    Set-Disk -Number $DiskNumber -IsOffline $true
    Start-Sleep -Seconds 2
}

function Set-CertDiskOnlineWithLetter {
    param([int]$DiskNumber, [int]$Index = 0)
    Set-Disk -Number $DiskNumber -IsOffline $false
    Start-Sleep -Seconds 3
    Update-HostStorageCache
    $partition = Get-CertDataPartition -DiskNumber $DiskNumber -Index $Index
    if (-not ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter))) {
        Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -AssignDriveLetter
        Start-Sleep -Seconds 2
        $partition = Get-CertDataPartition -DiskNumber $DiskNumber -Index $Index
    }
    return Get-CertPartitionLetter -Partition $partition
}

function Get-CertWindowsEvents {
    # Windows' own record that it ran the storage optimizer, since a moment before the step.
    param([datetime]$Since, [string]$Pattern)
    $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Microsoft-Windows-Defrag'; StartTime = $Since } -ErrorAction SilentlyContinue)
    return @($events | Where-Object { $_.Message -match $Pattern }).Count
}

function Get-CertBitLocker {
    param([string]$Letter)
    $volume = Get-BitLockerVolume -MountPoint "${Letter}:"
    return [ordered]@{ locked = ("$($volume.LockStatus)" -eq 'Locked'); protection = "$($volume.ProtectionStatus)"; status = "$($volume.VolumeStatus)" }
}

function Get-CertRefusal {
    # Where a refusal comes from depends on what the scan could read: the queue, or Apply.
    param([object]$Report)
    $reasons = @()
    foreach ($name in 'queue_refusal_reasons', 'apply_refusal_reasons') {
        if ($null -ne $Report.PSObject.Properties[$name]) { $reasons += @($Report.$name) }
    }
    return ($reasons -join ' | ')
}

function New-CertBitLockerVolume {
    param([int]$DiskNumber, [string]$Password)
    $volume = New-CertVolume -DiskNumber $DiskNumber -SizeBytes (512 * $Script:CertMiB) -Label 'SAKBL'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    $secure = ConvertTo-SecureString $Password -AsPlainText -Force
    Enable-BitLocker -MountPoint "$($volume.letter):" -PasswordProtector -Password $secure -UsedSpaceOnly -SkipHardwareTest -EncryptionMethod Aes128 -WarningAction SilentlyContinue | Out-Null
    $deadline = (Get-Date).AddMinutes(10)
    while ("$((Get-BitLockerVolume -MountPoint "$($volume.letter):").VolumeStatus)" -ne 'FullyEncrypted') {
        if ((Get-Date) -gt $deadline) { throw 'BitLocker did not finish encrypting the fixture' }
        Start-Sleep -Seconds 3
    }
    # Its warning prints the recovery password, and a job log is not where that goes.
    Add-BitLockerKeyProtector -MountPoint "$($volume.letter):" -RecoveryPasswordProtector -WarningAction SilentlyContinue | Out-Null
    $recovery = ((Get-BitLockerVolume -MountPoint "$($volume.letter):").KeyProtector | Where-Object KeyProtectorType -eq 'RecoveryPassword' | Select-Object -First 1).RecoveryPassword
    return [pscustomobject]@{ partition_number = $volume.partition_number; letter = $volume.letter; files = $files; recovery_password = $recovery; secure = $secure }
}

function New-CertRecoveryFixture {
    # A file with each signature the carver looks for, its bytes known, in clusters of its own.
    param([string]$Folder)
    New-Item -ItemType Directory -Force -Path $Folder | Out-Null
    $random = [Random]::new(77)
    $pad = New-Object byte[] 200000
    $random.NextBytes($pad)
    $sets = @(
        @{ name = 'photo.jpg'; head = [byte[]](0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 0x4A, 0x46, 0x49, 0x46); tail = [byte[]](0xFF, 0xD9) },
        # A PNG is a chain of chunks, so the body is one: its length, IDAT, the bytes, and a CRC.
        @{ name = 'picture.png'; head = ([byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A) + [byte[]](0x00, 0x03, 0x0D, 0x40) + [Text.Encoding]::ASCII.GetBytes('IDAT')); tail = [byte[]](0, 0, 0, 0, 0, 0, 0, 0, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82) },
        @{ name = 'paper.pdf'; head = [Text.Encoding]::ASCII.GetBytes('%PDF-1.4 '); tail = [Text.Encoding]::ASCII.GetBytes("`n%%EOF") }
    )
    $hashes = @()
    foreach ($set in $sets) {
        # The body must not hold anything that looks like this file's own end.
        $body = $pad.Clone()
        for ($index = 0; $index -lt $body.Length; $index++) { if ($body[$index] -ge 0x80 -or $body[$index] -lt 0x20) { $body[$index] = 0x41 } }
        $bytes = New-Object byte[] ($set.head.Length + $body.Length + $set.tail.Length)
        [Array]::Copy($set.head, 0, $bytes, 0, $set.head.Length)
        [Array]::Copy($body, 0, $bytes, $set.head.Length, $body.Length)
        [Array]::Copy($set.tail, 0, $bytes, $set.head.Length + $body.Length, $set.tail.Length)
        $path = Join-Path $Folder $set.name
        [IO.File]::WriteAllBytes($path, $bytes)
        $hashes += (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    # A PDF that starts 1000 bytes into a file that is not one, so not on a sector. A raw device reads
    # only whole sectors, and this is the one a carver has to read around.
    $inner = [Text.Encoding]::ASCII.GetBytes('%PDF-1.7 ' + ('B' * 50000) + "`n%%EOF")
    $filler = [byte[]](@(0x41) * 1000)
    $container = New-Object byte[] ($filler.Length + $inner.Length + 3000)
    for ($index = 0; $index -lt $container.Length; $index++) { $container[$index] = 0x41 }
    [Array]::Copy($inner, 0, $container, $filler.Length, $inner.Length)
    [IO.File]::WriteAllBytes((Join-Path $Folder 'container.bin'), $container)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hashes += ([BitConverter]::ToString($sha.ComputeHash($inner))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    return , $hashes
}

function Get-CertBootSectorSerial {
    param([int]$DiskNumber, [uint64]$PartitionOffset)
    $sector = Read-RbBytes -DiskNumber $DiskNumber -Offset $PartitionOffset -Length 512
    return '{0:X8}' -f [BitConverter]::ToUInt32($sector, 0x48)
}

function Get-CertRowBody {
    param([Parameter(Mandatory = $true)] [string]$Id)
    if (-not $Script:CertRowBodies.ContainsKey($Id)) { throw "No such row: $Id" }
    return $Script:CertRowBodies[$Id]
}

$Script:CertRowBodies = @{}

$Script:CertRowBodies['vhd.create-format-resize-delete'] = {
    $disk = New-CertGptVhd -Name 'cfrd'
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    Invoke-CertStep -Step 'create' -Operation 'Create' -Pin $pin -Choices @{
        size_mib = 256; free_before_mib = 0; file_system = 'NTFS'; allocation_unit_bytes = 4096
        partition_type = $Script:GptBasicData; label = 'SAKCERT'; full_format = $false
    } | Out-Null
    $snapshots = [ordered]@{ create = Get-CertSnapshot -DiskNumber $disk }
    $partition = Get-CertDataPartition -DiskNumber $disk
    $letter = Get-CertPartitionLetter -Partition $partition
    $createVolume = Get-RbVolumeFacts -Letter $letter
    $createHealth = Test-RbVolumeHealth -Letter $letter
    Write-CertMarkerFile -Letter $letter
    Invoke-CertStep -Step 'format' -Operation 'Format' -Pin $pin -Select @{ partition_number = [int]$partition.PartitionNumber } -Choices @{
        file_system = 'NTFS'; allocation_unit_bytes = 8192; label = 'SAKFMT'; full_format = $false
    } | Out-Null
    $snapshots.format = Get-CertSnapshot -DiskNumber $disk
    $partition = Get-CertDataPartition -DiskNumber $disk
    $letter = Get-CertPartitionLetter -Partition $partition
    $formatVolume = Get-RbVolumeFacts -Letter $letter
    $formatMarker = Test-Path -LiteralPath "${letter}:\marker.bin"
    $formatHealth = Test-RbVolumeHealth -Letter $letter
    Invoke-CertStep -Step 'resize' -Operation 'Resize' -Pin $pin -Select @{ partition_number = [int]$partition.PartitionNumber } -Choices @{ target_size_mib = 384 } | Out-Null
    $snapshots.resize = Get-CertSnapshot -DiskNumber $disk
    $resizeVolume = Get-RbVolumeFacts -Letter $letter
    $resizeHealth = Test-RbVolumeHealth -Letter $letter
    Invoke-CertStep -Step 'delete' -Operation 'Delete' -Pin $pin -Select @{ partition_number = [int]$partition.PartitionNumber } | Out-Null
    $snapshots.delete = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; create_volume = $createVolume; create_health = $createHealth
            format_volume = $formatVolume; format_marker_present = $formatMarker; format_health = $formatHealth
            resize_volume = $resizeVolume; resize_health = $resizeHealth } }
}

$Script:CertRowBodies['vhd.fat32-to-ntfs'] = {
    $disk = New-CertGptVhd -Name 'fat32'
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (512 * $Script:CertMiB) -FileSystem 'FAT32' -Label 'SAKFAT' -ClusterBytes 4096
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    $before = Get-RbVolumeFacts -Letter $volume.letter
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'convert' -Operation 'ConvertFileSystem' -Pin $pin -Select @{ partition_number = $volume.partition_number } | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; volume_before = $before; volume_after = (Get-RbVolumeFacts -Letter $volume.letter)
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "$($volume.letter):\fixture")
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

function Invoke-CertQuickPartition {
    param([hashtable]$Options, [string]$Style, [string]$Name, [int]$OldSizeMB = 128)
    $disk = New-CertVhd -Name $Name
    Initialize-Disk -Number $disk -PartitionStyle $Style
    $old = New-CertVolume -DiskNumber $disk -SizeBytes ($OldSizeMB * $Script:CertMiB) -Label 'SAKOLD'
    Write-CertMarkerFile -Letter $old.letter
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    Invoke-CertStep -Step 'layout' -Operation 'QuickPartition' -Pin $pin -Choices $Options | Out-Null
    $after = Get-CertSnapshot -DiskNumber $disk
    $summary = Get-CertVolumesSummary -DiskNumber $disk
    return [pscustomobject]@{ disk = $disk; media = $media; after = $after; summary = $summary; old_letter = $old.letter }
}

$Script:CertRowBodies['vhd.quick-partition'] = {
    $run = Invoke-CertQuickPartition -Style 'GPT' -Name 'qp' -Options @{
        partition_count = 4; partition_scheme = 'GPT'; size_mode = 'equal'; file_system = 'NTFS'
        allocation_unit_bytes = '4096'; label_prefix = 'SAKQP'; full_format = $false
    }
    $data = @($run.after.data_partitions)
    $sizes = @($data | ForEach-Object { [uint64]$_.size_bytes })
    $equal = @($sizes | Select-Object -First ($sizes.Count - 1) | Sort-Object -Unique).Count -eq 1
    $contiguous = $true
    for ($index = 1; $index -lt $data.Count; $index++) {
        if ([uint64]$data[$index].offset_bytes -ne ([uint64]$data[$index - 1].offset_bytes + [uint64]$data[$index - 1].size_bytes)) { $contiguous = $false }
    }
    return @{ media = $run.media; independent = [ordered]@{
            snapshots = [ordered]@{ after = $run.after }
            old_partition_still_there = (@(@($run.after.partitions) | Where-Object { $_.volume -and $_.volume.label -ceq 'SAKOLD' }).Count -gt 0)
            equal_slices = $equal; contiguous = $contiguous
            ntfs_volumes = $run.summary.ntfs_volumes; labels = $run.summary.labels; unhealthy_volumes = $run.summary.unhealthy_volumes } }
}

$Script:CertRowBodies['vhd.quick-partition-custom'] = {
    $run = Invoke-CertQuickPartition -Style 'GPT' -Name 'qpc' -Options @{
        partition_count = 3; partition_scheme = 'GPT'; size_mode = 'custom'; file_system = 'NTFS'
        allocation_unit_bytes = '4096'; label_prefix = 'SAKQP'; full_format = $false
        custom_labels = @('ALPHA', 'BRAVO', 'CHARLIE')
        custom_size_bytes = @("$(128 * $Script:CertMiB)", "$(192 * $Script:CertMiB)", "$(256 * $Script:CertMiB)")
    }
    return @{ media = $run.media; independent = [ordered]@{
            snapshots = [ordered]@{ after = $run.after }
            labels = $run.summary.labels; unhealthy_volumes = $run.summary.unhealthy_volumes } }
}

$Script:CertRowBodies['vhd.quick-partition-mbr'] = {
    $run = Invoke-CertQuickPartition -Style 'MBR' -Name 'qpm' -Options @{
        partition_count = 4; partition_scheme = 'MBR'; size_mode = 'equal'; file_system = 'NTFS'
        allocation_unit_bytes = '4096'; label_prefix = 'SAKMB'; full_format = $false
    }
    $data = @($run.after.data_partitions)
    $noOverlap = $true
    for ($index = 1; $index -lt $data.Count; $index++) {
        if ([uint64]$data[$index].offset_bytes -lt ([uint64]$data[$index - 1].offset_bytes + [uint64]$data[$index - 1].size_bytes)) { $noOverlap = $false }
    }
    return @{ media = $run.media; independent = [ordered]@{
            snapshots = [ordered]@{ after = $run.after }
            no_overlap = $noOverlap
            ntfs_volumes = $run.summary.ntfs_volumes; labels = $run.summary.labels; unhealthy_volumes = $run.summary.unhealthy_volumes } }
}

$Script:CertRowBodies['vhd.adjacent-extend'] = {
    $disk = New-CertGptVhd -Name 'extend'
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:CertMiB) -Label 'SAKEXT'
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'extend' -Operation 'Resize' -Pin $pin -Select @{ partition_number = $volume.partition_number } -Choices @{ target_size_mib = 512 } | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; volume_after = (Get-RbVolumeFacts -Letter $volume.letter)
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "$($volume.letter):\fixture")
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

$Script:CertRowBodies['vhd.recovered-partition-restore'] = {
    $disk = New-CertGptVhd -Name 'recover'
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:CertMiB) -Label 'SAKREC' -Offset (128 * $Script:CertMiB)
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    Write-VolumeCache -DriveLetter $volume.letter
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    $old = @($snapshots.before.data_partitions)[0]
    Remove-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number -Confirm:$false
    Start-Sleep -Seconds 2
    $snapshots.deleted = Get-CertSnapshot -DiskNumber $disk
    $scan = Invoke-CertStep -Step 'scan' -Operation 'PartitionRecoveryScan' -Pin $pin -Choices @{ scan_mode = 'Quick' }
    $said = (@($scan.execution.steps) | ForEach-Object { "$($_.stdout)" }) -join "`n"
    Invoke-CertStep -Step 'restore' -Operation 'RestoreRecoveredPartition' -Pin $pin -Choices @{
        offset_bytes = "$($old.offset_bytes)"; size_bytes = "$($old.size_bytes)"; type_id = "{$($old.type)}"
    } | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    $partition = Get-CertDataPartition -DiskNumber $disk
    $letter = if ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter)) { "$($partition.DriveLetter)" } else {
        Add-PartitionAccessPath -DiskNumber $disk -PartitionNumber $partition.PartitionNumber -AssignDriveLetter
        Start-Sleep -Seconds 2
        Get-CertPartitionLetter -Partition (Get-CertDataPartition -DiskNumber $disk)
    }
    Wait-CertVolumeReady -Letter $letter -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; scan_found_the_partition = ($said -match "$($old.offset_bytes)")
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "${letter}:\fixture")
            health = (Test-RbVolumeHealth -Letter $letter) } }
}

$Script:CertRowBodies['vhd.empty-style-conversion'] = {
    $disk = New-CertGptVhd -Name 'style'
    $pin = Get-CertPin -DiskNumber $disk
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'to_mbr' -Operation 'ConvertPartitionStyle' -Pin $pin -Choices @{ target_style = 'MBR' } | Out-Null
    $snapshots.mbr = Get-CertSnapshot -DiskNumber $disk
    Invoke-CertStep -Step 'to_gpt' -Operation 'ConvertPartitionStyle' -Pin (Get-CertPin -DiskNumber $disk) -Choices @{ target_style = 'GPT' } | Out-Null
    $snapshots.gpt = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{ snapshots = $snapshots } }
}

$Script:CertRowBodies['vhd.clear-disk-wipe'] = {
    # A wipe is a full format, which Windows does not allow on a thin disk, and every VHDX reports
    # thin. So the refusal is proved on a VHDX, with its data still there after, and the wipe on a VDI.
    $thin = New-CertGptVhd -Name 'wipe-thin' -SizeMB 256
    $thinVolume = New-CertVolume -DiskNumber $thin -SizeBytes (128 * $Script:CertMiB) -Label 'SAKTHIN'
    $thinFiles = Write-RbFixtureTree -Root "$($thinVolume.letter):\fixture"
    Write-VolumeCache -DriveLetter $thinVolume.letter
    $thinTableBefore = (Read-RbRawTable -DiskNumber $thin | ConvertTo-Json -Depth 4 -Compress)
    Invoke-CertStep -Step 'refused_on_thin' -Operation 'WipeDisk' -Pin (Get-CertPin -DiskNumber $thin) -Expect 'failed' -RefusalContains 'thin-provisioned' | Out-Null
    $thinTableAfter = (Read-RbRawTable -DiskNumber $thin | ConvertTo-Json -Depth 4 -Compress)
    $thinFilesAfter = Get-RbFileManifest -Root "$($thinVolume.letter):\fixture"

    $found = Get-CertReadPin -Role 'any'
    Clear-CertWholeDisk -DiskNumber $found.number
    $found = Get-CertReadPin -Role 'any'
    $disk = $found.number
    $media = Get-CertMedia -DiskNumber $disk -Class $found.class -Kind $found.kind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (512 * $Script:CertMiB) -Label 'SAKWIPE'
    Write-CertMarkerFile -Letter $volume.letter -Kilobytes 4096
    $before = Find-RbMarkerAnywhere -DiskNumber $disk -Marker $Script:Marker
    $found = Get-CertReadPin -Role 'any'
    Invoke-CertStep -Step 'wipe' -Operation 'WipeDisk' -Pin $found.pin -TimeoutSeconds 1800 | Out-Null
    $after = Find-RbMarkerAnywhere -DiskNumber $disk -Marker $Script:Marker
    return @{ media = $media; independent = [ordered]@{
            thin_table_before = $thinTableBefore; thin_table_after = $thinTableAfter
            thin_files_before = $thinFiles; thin_files_after = $thinFilesAfter
            snapshots = [ordered]@{ after = (Get-CertSnapshot -DiskNumber $disk) }
            scan_before = $before; scan_after = $after; disk_size_bytes = "$((Get-Disk -Number $disk).Size)" } }
}

$Script:CertRowBodies['vhd.image-clone'] = {
    $source = New-CertGptVhd -Name 'clone-source' -SizeMB 512
    $target = New-CertGptVhd -Name 'clone-target' -SizeMB 768
    $media = Get-CertMedia -DiskNumber $source -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $source -SizeBytes (256 * $Script:CertMiB) -Label 'SAKCLONE'
    $filesSource = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    Write-VolumeCache -DriveLetter $volume.letter
    $sourceBytes = [uint64](Get-Disk -Number $source).Size
    $sourcePartition = Get-Partition -DiskNumber $source -PartitionNumber $volume.partition_number
    $patternOffset = [uint64]$sourcePartition.Offset + [uint64]$sourcePartition.Size
    $patternBytes = [uint64](128 * $Script:CertMiB)
    Write-CertRawPattern -DiskNumber $source -Offset $patternOffset -Length $patternBytes
    Set-Partition -DiskNumber $source -PartitionNumber $volume.partition_number -IsReadOnly $true
    Set-CertDiskOffline -DiskNumber $source
    $pinSource = Get-CertPin -DiskNumber $source
    $pinTarget = Get-CertPin -DiskNumber $target
    $outsideOffset = $sourceBytes + (64 * $Script:CertMiB)
    Set-CertDiskOffline -DiskNumber $target
    $markerStream = [IO.File]::Open("\\.\PhysicalDrive$target", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        $block = New-Object byte[] 4096
        $text = [Text.Encoding]::ASCII.GetBytes($Script:Marker)
        [Array]::Copy($text, $block, $text.Length)
        $markerStream.Position = [int64]$outsideOffset
        $markerStream.Write($block, 0, 4096)
        $markerStream.Flush()
    } finally { $markerStream.Dispose() }
    $targetBefore = Get-RbRangeSha256 -DiskNumber $target -Offset 0 -Length $sourceBytes
    Set-Disk -Number $target -IsOffline $false
    Start-Sleep -Seconds 2
    $sourceBefore = Get-RbRangeSha256 -DiskNumber $source -Offset 0 -Length $sourceBytes
    Invoke-CertStep -Step 'clone' -Operation 'CloneDisk' -Pin $pinSource -SecondPin $pinTarget -Choices @{ verify_mode = 'Full verification' } -TimeoutSeconds 1800 | Out-Null
    $sourceHash = Get-RbRangeSha256 -DiskNumber $source -Offset 0 -Length $sourceBytes
    if (-not (Get-Disk -Number $target).IsOffline) { Set-CertDiskOffline -DiskNumber $target }
    $targetAfter = Get-RbRangeSha256 -DiskNumber $target -Offset 0 -Length $sourceBytes
    $outside = Find-RbMarkerHits -DiskNumber $target -Marker $Script:Marker -Offsets @($outsideOffset)
    $differences = @(Get-RbRangeDifferences -LeftPath "\\.\PhysicalDrive$source" -RightPath "\\.\PhysicalDrive$target" -Length $sourceBytes)
    $patternOnTarget = Get-RbRangeSha256 -DiskNumber $target -Offset $patternOffset -Length $patternBytes
    $patternOnSource = Get-RbRangeSha256 -DiskNumber $source -Offset $patternOffset -Length $patternBytes
    $letter = Set-CertDiskOnlineWithLetter -DiskNumber $target
    return @{ media = $media; independent = [ordered]@{
            source_hash_before = $sourceBefore; source_hash = $sourceHash; target_hash_before = $targetBefore; target_hash_after = $targetAfter
            pattern_bytes = "$patternBytes"; pattern_on_source = $patternOnSource; pattern_on_target = $patternOnTarget
            differences = $differences
            difference_summary = (Get-CertDifferenceSummary -Differences $differences -Length $sourceBytes -VolumeOffset ([uint64]$sourcePartition.Offset) -VolumeBytes ([uint64]$sourcePartition.Size))
            outside_marker_kept = ($outside.marker_hits -eq 1)
            files_source = $filesSource; files_target = (Get-RbFileManifest -Root "${letter}:\fixture") } }
}

$Script:CertRowBodies['vhd.image-restore'] = {
    $source = New-CertGptVhd -Name 'image-source' -SizeMB 512
    $target = New-CertGptVhd -Name 'image-target' -SizeMB 768
    $media = Get-CertMedia -DiskNumber $source -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $source -SizeBytes (256 * $Script:CertMiB) -Label 'SAKIMG'
    $filesSource = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    Write-VolumeCache -DriveLetter $volume.letter
    $sourcePartition = Get-Partition -DiskNumber $source -PartitionNumber $volume.partition_number
    $patternOffset = [uint64]$sourcePartition.Offset + [uint64]$sourcePartition.Size
    $patternBytes = [uint64](128 * $Script:CertMiB)
    Write-CertRawPattern -DiskNumber $source -Offset $patternOffset -Length $patternBytes
    Set-Partition -DiskNumber $source -PartitionNumber $volume.partition_number -IsReadOnly $true
    Set-CertDiskOffline -DiskNumber $source
    $sourceBytes = [uint64](Get-Disk -Number $source).Size
    $sourceHash = Get-RbRangeSha256 -DiskNumber $source -Offset 0 -Length $sourceBytes
    $image = Join-Path $WorkRoot 'restore-source.img'
    Remove-Item -LiteralPath $image -ErrorAction SilentlyContinue
    Invoke-CertStep -Step 'create_image' -Operation 'CreateImage' -Pin (Get-CertPin -DiskNumber $source) -Choices @{ image_path = $image } -TimeoutSeconds 1800 | Out-Null
    $imageHash = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
    $targetBefore = Get-RbRangeSha256 -DiskNumber $target -Offset 0 -Length $sourceBytes
    Invoke-CertStep -Step 'restore_image' -Operation 'RestoreImage' -Pin (Get-CertPin -DiskNumber $target) -Choices @{ image_path = $image } -TimeoutSeconds 1800 | Out-Null
    if (-not (Get-Disk -Number $target).IsOffline) { Set-CertDiskOffline -DiskNumber $target }
    $targetAfter = Get-RbRangeSha256 -DiskNumber $target -Offset 0 -Length $sourceBytes
    $differences = @(Get-RbRangeDifferences -LeftPath $image -RightPath "\\.\PhysicalDrive$target" -Length $sourceBytes)
    $patternOnTarget = Get-RbRangeSha256 -DiskNumber $target -Offset $patternOffset -Length $patternBytes
    $patternOnSource = Get-RbRangeSha256 -DiskNumber $source -Offset $patternOffset -Length $patternBytes
    $letter = Set-CertDiskOnlineWithLetter -DiskNumber $target
    Set-Partition -DiskNumber $target -PartitionNumber (Get-CertDataPartition -DiskNumber $target).PartitionNumber -IsReadOnly $false
    Set-CertDiskOffline -DiskNumber $target
    $letter = Set-CertDiskOnlineWithLetter -DiskNumber $target
    $files = Get-RbFileManifest -Root "${letter}:\fixture"
    $health = Test-RbVolumeHealth -Letter $letter
    Remove-Item -LiteralPath $image -ErrorAction SilentlyContinue
    return @{ media = $media; independent = [ordered]@{
            source_hash = $sourceHash; image_hash = $imageHash; target_hash_before = $targetBefore; target_hash_after = $targetAfter
            pattern_bytes = "$patternBytes"; pattern_on_source = $patternOnSource; pattern_on_target = $patternOnTarget
            differences = $differences
            difference_summary = (Get-CertDifferenceSummary -Differences $differences -Length $sourceBytes -VolumeOffset ([uint64]$sourcePartition.Offset) -VolumeBytes ([uint64]$sourcePartition.Size))
            files_source = $filesSource; files_target = $files; health = $health } }
}

$Script:CertRowBodies['vhd.partition-clone-region'] = {
    $source = New-CertGptVhd -Name 'region-source' -SizeMB 512
    $target = New-CertGptVhd -Name 'region-target' -SizeMB 768
    $media = Get-CertMedia -DiskNumber $source -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $source -SizeBytes (128 * $Script:CertMiB) -Label 'SAKSRC'
    Write-RbFixtureTree -Root "$($volume.letter):\fixture" | Out-Null
    Write-VolumeCache -DriveLetter $volume.letter
    $first = New-CertVolume -DiskNumber $target -SizeBytes (64 * $Script:CertMiB) -Label 'SAKA'
    Write-CertMarkerFile -Letter $first.letter
    $firstPartition = Get-Partition -DiskNumber $target -PartitionNumber $first.partition_number
    $regionOffset = [uint64]$firstPartition.Offset + [uint64]$firstPartition.Size
    $last = New-CertVolume -DiskNumber $target -SizeBytes (64 * $Script:CertMiB) -Label 'SAKB' -Offset ($regionOffset + (384 * $Script:CertMiB))
    Write-CertMarkerFile -Letter $last.letter
    Write-VolumeCache -DriveLetter $first.letter
    Write-VolumeCache -DriveLetter $last.letter
    Set-CertVolumeQuiet -DiskNumber $source -PartitionNumber $volume.partition_number
    Set-Partition -DiskNumber $target -PartitionNumber $first.partition_number -IsReadOnly $true
    Set-CertVolumeQuiet -DiskNumber $target -PartitionNumber $last.partition_number
    $firstPartition = Get-Partition -DiskNumber $target -PartitionNumber $first.partition_number
    $lastPartition = Get-Partition -DiskNumber $target -PartitionNumber $last.partition_number
    $sourcePartition = Get-Partition -DiskNumber $source -PartitionNumber $volume.partition_number
    $sourceBytes = [uint64]$sourcePartition.Size
    $firstBefore = Get-RbRangeSha256 -DiskNumber $target -Offset ([uint64]$firstPartition.Offset) -Length ([uint64]$firstPartition.Size)
    $lastBefore = Get-RbRangeSha256 -DiskNumber $target -Offset ([uint64]$lastPartition.Offset) -Length ([uint64]$lastPartition.Size)
    $regionBefore = Get-RbRangeSha256 -DiskNumber $target -Offset $regionOffset -Length $sourceBytes
    Invoke-CertStep -Step 'clone' -Operation 'ClonePartition' -Pin (Get-CertPin -DiskNumber $source) -SecondPin (Get-CertPin -DiskNumber $target) `
        -Select @{ partition_number = $volume.partition_number } -Choices @{ verify_mode = 'Full verification'; region_offset_bytes = "$regionOffset" } -TimeoutSeconds 1800 | Out-Null
    Set-CertDiskOffline -DiskNumber $source
    $sourceHash = Get-RbRangeSha256 -DiskNumber $source -Offset ([uint64]$sourcePartition.Offset) -Length $sourceBytes
    $regionAfter = Get-RbRangeSha256 -DiskNumber $target -Offset $regionOffset -Length $sourceBytes
    $firstAfter = Get-RbRangeSha256 -DiskNumber $target -Offset ([uint64]$firstPartition.Offset) -Length ([uint64]$firstPartition.Size)
    $lastAfter = Get-RbRangeSha256 -DiskNumber $target -Offset ([uint64]$lastPartition.Offset) -Length ([uint64]$lastPartition.Size)
    return @{ media = $media; independent = [ordered]@{
            source_hash = $sourceHash; region_hash_before = $regionBefore; region_hash_after = $regionAfter
            marker_before_kept = ($firstBefore -ceq $firstAfter); marker_after_kept = ($lastBefore -ceq $lastAfter) } }
}

$Script:CertRowBodies['external.usb-removable'] = {
    $found = Get-CertReadPin -Role 'hdd'
    Initialize-CertBlankDisk -DiskNumber $found.number
    $found = Get-CertReadPin -Role 'hdd'
    $media = Get-CertMedia -DiskNumber $found.number -Class $found.class -Kind $found.kind
    $busType = "$((Get-Disk -Number $found.number).BusType)"
    Invoke-CertStep -Step 'create' -Operation 'Create' -Pin $found.pin -Choices @{
        size_mib = 1024; free_before_mib = 0; file_system = 'NTFS'; allocation_unit_bytes = 4096
        partition_type = $Script:GptBasicData; label = 'SAKUSBA'; full_format = $false
    } -TimeoutSeconds 900 | Out-Null
    $snapshots = [ordered]@{ create = Get-CertSnapshot -DiskNumber $found.number }
    $partition = Get-CertDataPartition -DiskNumber $found.number
    $found = Get-CertReadPin -Role 'hdd'
    Invoke-CertStep -Step 'format' -Operation 'Format' -Pin $found.pin -Select @{ partition_number = [int]$partition.PartitionNumber } -Choices @{
        file_system = 'NTFS'; allocation_unit_bytes = 4096; label = 'SAKUSB'; full_format = $false
    } -TimeoutSeconds 900 | Out-Null
    $partition = Get-CertDataPartition -DiskNumber $found.number
    $letter = Get-CertPartitionLetter -Partition $partition
    $written = Write-RbFixtureTree -Root "${letter}:\fixture"
    Write-VolumeCache -DriveLetter $letter
    $readBack = Get-RbFileManifest -Root "${letter}:\fixture"
    $formatVolume = Get-RbVolumeFacts -Letter $letter
    $health = Test-RbVolumeHealth -Letter $letter
    $found = Get-CertReadPin -Role 'hdd'
    Invoke-CertStep -Step 'delete' -Operation 'Delete' -Pin $found.pin -Select @{ partition_number = [int]$partition.PartitionNumber } -TimeoutSeconds 900 | Out-Null
    $snapshots.delete = Get-CertSnapshot -DiskNumber $found.number
    return @{ media = $media; independent = [ordered]@{
            bus_type = $busType; snapshots = $snapshots; format_volume = $formatVolume
            files_written = $written; files_read_back = $readBack; health = $health } }
}

$Script:CertRowBodies['external.file-level-data-recovery'] = {
    $disk = New-CertGptVhd -Name 'recovery' -SizeMB 512
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:CertMiB) -Label 'SAKFILES'
    $folder = "$($volume.letter):\photos"
    $hashes = New-CertRecoveryFixture -Folder $folder
    Write-VolumeCache -DriveLetter $volume.letter
    Remove-Item -LiteralPath $folder -Recurse -Force
    Write-VolumeCache -DriveLetter $volume.letter
    Set-CertVolumeQuiet -DiskNumber $disk -PartitionNumber $volume.partition_number
    $partition = Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number
    $null = Get-PSDrive -PSProvider FileSystem
    $present = @(Get-ChildItem -LiteralPath "$($volume.letter):\" -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -in 'photo.jpg', 'picture.png', 'paper.pdf', 'container.bin' }).Count
    # The panel offers the selected volume's own path, and reads it with the volume mounted.
    $start = [uint64]$partition.Offset
    $size = [uint64]$partition.Size
    $hashBefore = Get-RbRangeSha256 -DiskNumber $disk -Offset $start -Length $size
    $destination = Join-Path $WorkRoot 'recovered-files'
    Remove-Item -LiteralPath $destination -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $report = Invoke-CertStep -Step 'recover' -Operation 'FileRecovery' -Pin (Get-CertPin -DiskNumber $disk) -Elevated -Choices @{
        source_path = "\\.\$($volume.letter):"; destination_directory = $destination
    } -TimeoutSeconds 1200
    $hashAfter = Get-RbRangeSha256 -DiskNumber $disk -Offset $start -Length $size
    $restored = @(Get-ChildItem -LiteralPath $destination -Recurse -File | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
    $missing = @($hashes | Where-Object { $restored -notcontains $_ }).Count
    Remove-Item -LiteralPath $destination -Recurse -Force -ErrorAction SilentlyContinue
    return @{ media = $media; independent = [ordered]@{
            deleted_files_present = $present
            volume_bytes = "$size"
            recovered = [ordered]@{ scan_bytes_read = "$($report.recovery_scan.bytes_read)"; scan_warnings = (@($report.recovery_scan.warnings) -join ' | '); restored_files = $restored.Count; missing_hashes = $missing }
            source_hash_before = $hashBefore; source_hash_after = $hashAfter } }
}

$Script:CertRowBodies['external.ssd-retrim'] = {
    $found = Get-CertReadPin -Role 'ssd'
    Clear-CertWholeDisk -DiskNumber $found.number
    $found = Get-CertReadPin -Role 'ssd'
    $media = Get-CertMedia -DiskNumber $found.number -Class $found.class -Kind $found.kind
    $volume = New-CertVolume -DiskNumber $found.number -SizeBytes (4096 * $Script:CertMiB) -Label 'SAKTRIM'
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture"
    # A file's clusters, then the file gone: whether those clusters read as zero after a ReTrim is a
    # thing to record about this device. It is not a requirement: a drive need not zero them.
    $patternPath = "$($volume.letter):\trim-pattern.bin"
    Write-CertMarkerFile -Letter $volume.letter -Name 'trim-pattern.bin' -Kilobytes 65536
    $partition = Get-Partition -DiskNumber $found.number -PartitionNumber $volume.partition_number
    $extent = & fsutil.exe file queryextents $patternPath 2>&1 | Select-Object -First 1
    $lcn = if ("$extent" -match 'LCN:\s*0x([0-9a-fA-F]+)') { [Convert]::ToUInt64($Matches[1], 16) } else { [uint64]0 }
    $cluster = [uint64](Get-RbVolumeFacts -Letter $volume.letter).cluster_bytes
    $probe = [uint64]$partition.Offset + ($lcn * $cluster)
    Remove-Item -LiteralPath $patternPath -Force
    Write-VolumeCache -DriveLetter $volume.letter
    $since = (Get-Date).AddSeconds(-5)
    $found = Get-CertReadPin -Role 'ssd'
    Invoke-CertStep -Step 'retrim' -Operation 'OptimizeSsd' -Pin $found.pin -Select @{ partition_number = $volume.partition_number } -TimeoutSeconds 1800 | Out-Null
    Start-Sleep -Seconds 5
    $zero = if ($lcn -gt 0) { (Find-RbMarkerHits -DiskNumber $found.number -Marker $Script:Marker -Offsets @($probe)) } else { $null }
    return @{ media = $media; independent = [ordered]@{
            media_type = $media.media_type
            retrim_logged = ((Get-CertWindowsEvents -Since $since -Pattern 'retrim') -gt 0)
            deleted_clusters_after_retrim = $zero
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "$($volume.letter):\fixture")
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

$Script:CertRowBodies['external.hdd-defrag-execution'] = {
    $found = Get-CertReadPin -Role 'hdd'
    Initialize-CertBlankDisk -DiskNumber $found.number
    $found = Get-CertReadPin -Role 'hdd'
    $media = Get-CertMedia -DiskNumber $found.number -Class $found.class -Kind $found.kind
    $volume = New-CertVolume -DiskNumber $found.number -SizeBytes (16384 * $Script:CertMiB) -Label 'SAKDEFRAG'
    $root = "$($volume.letter):\fixture"
    $filesBefore = Write-RbFixtureTree -Root $root -FileCount 64
    # Fragment it the ordinary way: fill with small files, delete every other one, write large ones.
    $scatter = "$($volume.letter):\scatter"
    New-Item -ItemType Directory -Force -Path $scatter | Out-Null
    $chunk = New-Object byte[] (64KB)
    for ($index = 0; $index -lt 2000; $index++) { [IO.File]::WriteAllBytes("$scatter\$index.bin", $chunk) }
    for ($index = 0; $index -lt 2000; $index += 2) { Remove-Item -LiteralPath "$scatter\$index.bin" -Force }
    $big = New-Object byte[] (32MB)
    [IO.File]::WriteAllBytes("$root\big.bin", $big)
    $filesBefore = Get-RbFileManifest -Root $root
    Write-VolumeCache -DriveLetter $volume.letter
    $since = (Get-Date).AddSeconds(-5)
    $found = Get-CertReadPin -Role 'hdd'
    Invoke-CertStep -Step 'defrag' -Operation 'DefragVolume' -Pin $found.pin -Select @{ partition_number = $volume.partition_number } -TimeoutSeconds 3600 | Out-Null
    Start-Sleep -Seconds 5
    $result = [ordered]@{
        media_type = $media.media_type
        defrag_logged = ((Get-CertWindowsEvents -Since $since -Pattern 'defrag') -gt 0)
        files_before = $filesBefore; files_after = (Get-RbFileManifest -Root $root)
        health = (Test-RbVolumeHealth -Letter $volume.letter)
    }
    $found = Get-CertReadPin -Role 'hdd'
    Remove-Partition -DiskNumber $found.number -PartitionNumber $volume.partition_number -Confirm:$false
    return @{ media = $media; independent = $result }
}

function Invoke-CertWholeDiskWipe {
    param([string]$Role, [hashtable]$Choices, [int]$TimeoutSeconds)
    $found = Get-CertReadPin -Role $Role
    Clear-CertWholeDisk -DiskNumber $found.number
    $found = Get-CertReadPin -Role $Role
    $media = Get-CertMedia -DiskNumber $found.number -Class $found.class -Kind $found.kind
    $number = $found.number
    $serialBefore = (Get-CertSha256OfText "$((Get-Disk -Number $number).SerialNumber)".Trim())
    # Markers go on first: Windows refuses a raw write to a mounted volume. The format then lands on
    # a few, so the proof is over the ones that are still there.
    $placed = Write-CertRawMarkers -DiskNumber $number
    $volume = New-CertVolume -DiskNumber $number -SizeBytes (1024 * $Script:CertMiB) -Label 'SAKWIPE'
    Write-RbFixtureTree -Root "$($volume.letter):\fixture" | Out-Null
    Write-VolumeCache -DriveLetter $volume.letter
    $offsets = @($placed | Where-Object { (Find-RbMarkerHits -DiskNumber $number -Marker $Script:Marker -Offsets @($_)).marker_hits -eq 1 })
    $before = Find-RbMarkerHits -DiskNumber $number -Marker $Script:Marker -Offsets $offsets
    $found = Get-CertReadPin -Role $Role
    $report = Invoke-CertStep -Step $(if ($Choices.ContainsKey('ssd_secure_erase') -and $Choices['ssd_secure_erase']) { 'erase' } else { 'wipe' }) -Operation 'WipeDisk' -Pin $found.pin -Choices $Choices -TimeoutSeconds $TimeoutSeconds
    $after = Find-RbMarkerHits -DiskNumber $number -Marker $Script:Marker -Offsets $offsets
    $warnings = @(@($report.queued_operations) | ForEach-Object { @($_.warnings) }) -join ' | '
    return @{ media = $media; independent = [ordered]@{
            media_type = $media.media_type; app_warnings = $warnings
            markers_before = $before; markers_after = $after
            snapshots = [ordered]@{ after = (Get-CertSnapshot -DiskNumber $number) }
            serial_sha256_before = $serialBefore
            serial_sha256_after = (Get-CertSha256OfText "$((Get-Disk -Number $number).SerialNumber)".Trim()) } }
}

$Script:CertRowBodies['external.ssd-secure-erase'] = {
    Invoke-CertWholeDiskWipe -Role 'ssd' -Choices @{ ssd_secure_erase = $true } -TimeoutSeconds 21600
}

$Script:CertRowBodies['external.hardware-wipe'] = {
    Invoke-CertWholeDiskWipe -Role 'ssd' -Choices @{} -TimeoutSeconds 21600
}

$Script:CertRowBodies['external.bitlocker'] = {
    $disk = New-CertGptVhd -Name 'bitlocker'
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertBitLockerVolume -DiskNumber $disk -Password 'R7-throwaway-pass-1'
    $letter = $volume.letter
    Lock-BitLocker -MountPoint "${letter}:" -ForceDismount | Out-Null
    Start-Sleep -Seconds 3
    $before = Get-CertBitLocker -Letter $letter
    $tableBefore = (Read-RbRawTable -DiskNumber $disk | ConvertTo-Json -Depth 4 -Compress)
    $report = Invoke-CertStep -Step 'refused_while_locked' -Operation 'Format' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $volume.partition_number } -Choices @{
        file_system = 'NTFS'; allocation_unit_bytes = 4096; label = 'SAKNOPE'; full_format = $false
    } -Expect 'refused' -RefusalContains 'BitLocker'
    $tableAfter = (Read-RbRawTable -DiskNumber $disk | ConvertTo-Json -Depth 4 -Compress)
    $after = Get-CertBitLocker -Letter $letter
    Unlock-BitLocker -MountPoint "${letter}:" -RecoveryPassword $volume.recovery_password | Out-Null
    Start-Sleep -Seconds 3
    $files = Get-RbFileManifest -Root "${letter}:\fixture"
    Disable-BitLocker -MountPoint "${letter}:" | Out-Null
    return @{ media = $media; independent = [ordered]@{
            bitlocker_before = $before; refusal = (Get-CertRefusal -Report $report); table_before = $tableBefore; table_after = $tableAfter
            bitlocker_after = $after; files_before = $volume.files; files_after = $files } }
}

$Script:CertRowBodies['external.bitlocker-mutation'] = {
    $disk = New-CertGptVhd -Name 'bitlocker2'
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertBitLockerVolume -DiskNumber $disk -Password 'R7-throwaway-pass-2'
    $letter = $volume.letter
    Lock-BitLocker -MountPoint "${letter}:" -ForceDismount | Out-Null
    Start-Sleep -Seconds 3
    $before = Get-CertBitLocker -Letter $letter
    $pin = Get-CertPin -DiskNumber $disk
    Invoke-CertStep -Step 'unlock' -Operation 'BitLockerUnlock' -Pin $pin -Select @{ partition_number = $volume.partition_number } -Choices @{ recovery_password = $volume.recovery_password } | Out-Null
    $unlocked = Get-CertBitLocker -Letter $letter
    Invoke-CertStep -Step 'suspend' -Operation 'BitLockerSuspend' -Pin $pin -Select @{ partition_number = $volume.partition_number } | Out-Null
    $suspended = Get-CertBitLocker -Letter $letter
    Invoke-CertStep -Step 'resume' -Operation 'BitLockerResume' -Pin $pin -Select @{ partition_number = $volume.partition_number } | Out-Null
    $resumed = Get-CertBitLocker -Letter $letter
    $files = Get-RbFileManifest -Root "${letter}:\fixture"
    $leaks = @(Get-ChildItem -LiteralPath $Script:RowDir -File | Where-Object { (Get-Content -LiteralPath $_.FullName -Raw) -match [regex]::Escape($volume.recovery_password) }).Count
    Disable-BitLocker -MountPoint "${letter}:" | Out-Null
    return @{ media = $media; independent = [ordered]@{
            bitlocker_before = $before; bitlocker_unlocked = $unlocked; bitlocker_suspended = $suspended; bitlocker_resumed = $resumed
            password_in_reports = $leaks; files_before = $volume.files; files_after = $files } }
}

function New-CertBackupFolder {
    $folder = Join-Path $WorkRoot "backup-$($Script:RowId)"
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    return $folder
}

$Script:CertRowBodies['external.allocate-free-space'] = {
    $fixture = Get-CertDataFixture -Name 'allocate'
    $disk = $fixture.number
    $media = $fixture.media
    $target = New-CertVolume -DiskNumber $disk -SizeBytes (192 * $Script:CertMiB) -Label 'SAKTARGET'
    $donor = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:CertMiB) -Label 'SAKDONOR'
    $targetBefore = Write-RbFixtureTree -Root "$($target.letter):\fixture" -FileCount 8
    $donorBefore = Write-RbFixtureTree -Root "$($donor.letter):\fixture" -FileCount 8
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'allocate' -Operation 'AllocateFreeSpace' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $target.partition_number } -Choices @{
        allocate_mib = 64; backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots
            target_files_before = $targetBefore; target_files_after = (Get-RbFileManifest -Root "$($target.letter):\fixture")
            donor_files_before = $donorBefore; donor_files_after = (Get-RbFileManifest -Root "$($donor.letter):\fixture")
            target_health = (Test-RbVolumeHealth -Letter $target.letter); donor_health = (Test-RbVolumeHealth -Letter $donor.letter) } }
}

$Script:CertRowBodies['external.partition-move'] = {
    $fixture = Get-CertDataFixture -Name 'move'
    $disk = $fixture.number
    $media = $fixture.media
    $base = Get-CertFirstFreeOffset -DiskNumber $disk
    $requested = $base + (64 * $Script:CertMiB)
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (192 * $Script:CertMiB) -Label 'SAKMOVE' -Offset ($base + (256 * $Script:CertMiB))
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'move' -Operation 'MovePartition' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $volume.partition_number } -Choices @{
        target_offset_mib = ($requested / $Script:CertMiB); backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; requested_offset_bytes = "$requested"
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "$($volume.letter):\fixture")
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

$Script:CertRowBodies['external.cluster-size-change'] = {
    $fixture = Get-CertDataFixture -Name 'cluster'
    $disk = $fixture.number
    $media = $fixture.media
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:CertMiB) -Label 'SAKCLUSTER'
    $root = "$($volume.letter):\fixture"
    Write-RbFixtureTree -Root $root -FileCount 8 | Out-Null
    $special = Join-Path $root 'metadata.txt'
    Set-Content -LiteralPath $special -Value 'main stream' -Encoding ASCII
    Set-Content -LiteralPath $special -Stream 'sak.cert' -Value 'alternate stream' -Encoding ASCII
    & icacls.exe $special /grant '*S-1-5-32-546:(R)' /Q | Out-Null
    $filesBefore = Get-RbFileManifest -Root $root
    # As plain text: what Get-Content returns also carries its provider, and that serialized forever.
    $streamBefore = "$(Get-Content -LiteralPath $special -Stream 'sak.cert' -Raw)"
    $aclBefore = (Get-Acl -LiteralPath $special).Sddl
    $before = Get-RbVolumeFacts -Letter $volume.letter
    Invoke-CertStep -Step 'change' -Operation 'ChangeClusterSize' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $volume.partition_number } -Choices @{
        file_system = 'NTFS'; allocation_unit_bytes = 16384; label = 'SAKCLUSTER'; backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    return @{ media = $media; independent = [ordered]@{
            volume_before = $before; volume_after = (Get-RbVolumeFacts -Letter $volume.letter)
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root $root)
            stream_before = $streamBefore; stream_after = "$(Get-Content -LiteralPath $special -Stream 'sak.cert' -Raw -ErrorAction SilentlyContinue)"
            acl_before = $aclBefore; acl_after = (Get-Acl -LiteralPath $special).Sddl
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

$Script:CertRowBodies['external.primary-logical-conversion'] = {
    $disk = New-CertVhd -Name 'logical'
    Initialize-Disk -Number $disk -PartitionStyle MBR
    $media = Get-CertMedia -DiskNumber $disk -Class 'virtual_disk' -Kind $Script:VhdKind
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:CertMiB) -Label 'SAKLOG'
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'to_logical' -Operation 'ConvertPrimaryLogical' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $volume.partition_number } -Choices @{
        target_layout = 'logical'; backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    $letter = Get-CertPartitionLetter -Partition (Get-CertDataPartition -DiskNumber $disk)
    return @{ media = $media; independent = [ordered]@{
            snapshots = $snapshots; files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "${letter}:\fixture")
            health = (Test-RbVolumeHealth -Letter $letter) } }
}

$Script:CertRowBodies['external.volume-serial-number'] = {
    $fixture = Get-CertDataFixture -Name 'serial'
    $disk = $fixture.number
    $media = $fixture.media
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:CertMiB) -Label 'SAKSERIAL'
    $filesBefore = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $sectorBefore = Get-CertBootSectorSerial -DiskNumber $disk -PartitionOffset $offset
    Invoke-CertStep -Step 'change' -Operation 'ChangeVolumeSerialNumber' -Pin (Get-CertPin -DiskNumber $disk) -Select @{ partition_number = $volume.partition_number } -Choices @{
        backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    return @{ media = $media; independent = [ordered]@{
            boot_sector_serial_before = $sectorBefore
            boot_sector_serial_after = (Get-CertBootSectorSerial -DiskNumber $disk -PartitionOffset $offset)
            volume_after = (Get-RbVolumeFacts -Letter $volume.letter)
            files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "$($volume.letter):\fixture")
            health = (Test-RbVolumeHealth -Letter $volume.letter) } }
}

$Script:CertRowBodies['external.dynamic-to-basic'] = {
    # Windows will not make a virtual disk file dynamic, so this row needs a VDI.
    $found = Get-CertReadPin -Role 'any'
    $disk = $found.number
    Initialize-Disk -Number $disk -PartitionStyle MBR
    $media = Get-CertMedia -DiskNumber $disk -Class $found.class -Kind $found.kind
    $letter = Get-CertFreeLetter
    $script = Join-Path $WorkRoot 'dynamic.diskpart.txt'
    @("select disk $disk", 'convert dynamic', 'create volume simple size=384', 'format fs=ntfs quick label=SAKDYN', "assign letter=$letter") |
        Set-Content -LiteralPath $script -Encoding ASCII
    $output = & diskpart.exe /s $script 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "diskpart could not make the dynamic fixture: $output" }
    Start-Sleep -Seconds 3
    Update-HostStorageCache
    $hiddenFromGetDisk = -not (Get-RbDiskFacts -DiskNumber $disk).listed_by_get_disk
    $filesBefore = Write-RbFixtureTree -Root "${letter}:\fixture" -FileCount 8
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $disk }
    Invoke-CertStep -Step 'convert' -Operation 'ConvertDynamicDiskToBasic' -Pin (Get-CertPin -DiskNumber $disk) -Choices @{
        backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 1800 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $disk
    return @{ media = $media; independent = [ordered]@{
            windows_listed_it_with_get_disk = (-not $hiddenFromGetDisk)
            listed_by_get_disk_after = (Get-RbDiskFacts -DiskNumber $disk).listed_by_get_disk
            snapshots = $snapshots; files_before = $filesBefore; files_after = (Get-RbFileManifest -Root "${letter}:\fixture")
            health = (Test-RbVolumeHealth -Letter $letter) } }
}

function Invoke-CertBreakBootFiles {
    # Breaks a throwaway Windows disk's boot files the way damage would: the row must then see that
    # it does not start. Nothing here is the app.
    param([int]$DiskNumber, [ValidateSet('UEFI', 'BIOS')] [string]$Mode)
    $disk = Get-Disk -Number $DiskNumber
    if ($disk.IsOffline) { Set-Disk -Number $DiskNumber -IsOffline $false; Start-Sleep -Seconds 3 }
    $letter = 'Y'
    if ($Mode -eq 'UEFI') {
        $system = Get-Partition -DiskNumber $DiskNumber | Where-Object { "$($_.GptType)" -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object -First 1
    } else {
        $system = Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.IsActive } | Select-Object -First 1
    }
    Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $system.PartitionNumber -AccessPath "${letter}:\"
    try {
        if ($Mode -eq 'UEFI') {
            Remove-Item -LiteralPath "${letter}:\EFI\Microsoft\Boot" -Recurse -Force
            Remove-Item -LiteralPath "${letter}:\EFI\Boot" -Recurse -Force
        } else {
            & takeown.exe /f "${letter}:\Boot" /r /d y 2>&1 | Out-Null
            & takeown.exe /f "${letter}:\bootmgr" 2>&1 | Out-Null
            & icacls.exe "${letter}:\Boot" /grant 'Administrators:F' /t /c 2>&1 | Out-Null
            & icacls.exe "${letter}:\bootmgr" /grant 'Administrators:F' 2>&1 | Out-Null
            & attrib.exe -s -h -r "${letter}:\bootmgr" 2>&1 | Out-Null
            & attrib.exe -s -h -r "${letter}:\Boot\*" /s /d 2>&1 | Out-Null
            Remove-Item -LiteralPath "${letter}:\bootmgr" -Force
            Remove-Item -LiteralPath "${letter}:\Boot" -Recurse -Force
        }
        # What the firmware or the boot code would load. EFI\Microsoft\Recovery holds a BCD of its own,
        # which starts nothing.
        $loaded = if ($Mode -eq 'UEFI') { @('EFI\Boot\bootx64.efi', 'EFI\Microsoft\Boot\bootmgfw.efi', 'EFI\Microsoft\Boot\BCD') } else { @('bootmgr', 'Boot\BCD') }
        $left = @($loaded | Where-Object { Test-Path -LiteralPath "${letter}:\$_" }).Count
    } finally {
        Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $system.PartitionNumber -AccessPath "${letter}:\"
    }
    if ($Mode -eq 'BIOS') {
        $handle = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
        try {
            $sector = New-Object byte[] 512
            [void]$handle.Read($sector, 0, 512)
            [Array]::Clear($sector, 0, 440)
            $handle.Position = 0
            $handle.Write($sector, 0, 512)
            $handle.Flush()
        } finally { $handle.Dispose() }
    }
    return $left
}

function Invoke-CertBootRepairPhase {
    param([ValidateSet('UEFI', 'BIOS')] [string]$Mode)
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    $state = Join-Path $OutputRoot "$($Script:RowId).phase.json"
    $independent = if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    if ($Phase -eq 'break') {
        $left = Invoke-CertBreakBootFiles -DiskNumber $found.number -Mode $Mode
        $independent | Add-Member -NotePropertyName broken_files_after -NotePropertyValue $left -Force
        ($independent | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $state
    } elseif ($Phase -eq 'repair') {
        Invoke-CertStep -Step 'repair' -Operation 'RepairBoot' -Pin $found.pin -Choices @{ boot_mode = $Mode } -TimeoutSeconds 900 | Out-Null
    } else {
        throw "A boot row runs one phase at a time, from the host: break or repair, not '$Phase'."
    }
    return @{ media = $media; independent = $independent }
}

$Script:CertRowBodies['external.boot-repair-uefi'] = { Invoke-CertBootRepairPhase -Mode 'UEFI' }
$Script:CertRowBodies['external.boot-repair-bios'] = { Invoke-CertBootRepairPhase -Mode 'BIOS' }

$Script:CertRowBodies['external.os-migration-reboot'] = {
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    Set-Content -LiteralPath 'C:\SAKCert\source-marker.txt' -Value $Script:RunId -Encoding ASCII
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $found.number }
    Invoke-CertStep -Step 'migrate' -Operation 'MigrateOs' -Pin $found.pin -Choices @{ verify_mode = 'Sample verification'; allow_no_snapshot = $false } -TimeoutSeconds 7200 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $found.number
    return @{ media = $media; independent = [ordered]@{ snapshots = $snapshots; source_marker = $Script:RunId } }
}

$Script:CertRowBodies['external.system-mbr2gpt'] = {
    # This runs in a VM that boots from the MBR disk it converts. The host then turns on EFI.
    Assert-CertBootRowInVm
    $disk = @(Get-Disk | Where-Object IsBoot)[0]
    if ("$($disk.PartitionStyle)" -ne 'MBR') { throw 'mbr2gpt needs a Windows running from an MBR disk' }
    $number = [int]$disk.Number
    $media = Get-CertMedia -DiskNumber $number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    $snapshots = [ordered]@{ before = Get-CertSnapshot -DiskNumber $number }
    Invoke-CertStep -Step 'convert' -Operation 'ConvertPartitionStyle' -Pin (Get-CertPin -DiskNumber $number) -Choices @{ target_style = 'GPT' } -TimeoutSeconds 1800 | Out-Null
    $snapshots.after = Get-CertSnapshot -DiskNumber $number
    return @{ media = $media; independent = [ordered]@{ snapshots = $snapshots } }
}
