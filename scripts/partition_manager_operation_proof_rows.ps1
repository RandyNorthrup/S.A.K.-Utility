<#
.SYNOPSIS
    Operation-proof rows: each Partition Manager operation a person can reach, run through the app's
    own path, then read back by tools that share no code with it.

.DESCRIPTION
    Dot-sourced by run_partition_manager_app_certification.ps1 when it is given -OperationProof. It
    adds rows named ops.* to the same table the certification rows live in, and uses the same
    helpers: a fixture made with Windows' tools, each operation run by Invoke-CertStep through the
    step client (the panel's request functions or, where the panel builds a payload inline, a copy
    of that code the step's report names), the controller's queue and Apply, and the real elevated
    helper; then a readback.

    It changes nothing the certification uses. No ops.* id is in
    certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json, the evidence goes to a folder the
    evidence selector never reads, and the claim level never sees it. A step carries
    operation_proof in its job, and only such a job reaches the step client's operation-proof
    builders and routes.

    What a row may change: virtual disk files it makes under -WorkRoot, and nothing else. Before
    every step it pins the target by the VHD's path, its size and the disk number Windows gave it,
    and on testpc2 also checks that -WorkRoot sits on the spare Samsung, by model, serial and size.

    A row returns its media and its readback, which holds checks: each a fact a separate tool read,
    and whether it is what the operation should have left. scripts/run_partition_manager_operation_proof.ps1
    turns them into the results table.
#>

. (Join-Path $PSScriptRoot 'partition_manager_operation_proof_checks.ps1')

$Script:OpsPinLog = New-Object System.Collections.ArrayList
$Script:OpsStepFailures = New-Object System.Collections.ArrayList

function Assert-OpsWorkRootOnSpare {
    # On a physical PC the virtual disks live on the one drive the owner gave to this, which
    # machine.json beside the work folder names by model, serial and size (written by the driver
    # from a file outside the repository: serial numbers stay out of it). On a VirtualBox guest the
    # whole machine is a throwaway.
    if ($Script:InVirtualBox) { return }
    $file = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($WorkRoot))) 'machine.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Operation proof on a physical PC needs $file to name its writable drive" }
    $machine = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if ($env:COMPUTERNAME -cne "$($machine.name)") { throw "$file names $($machine.name), and this is $env:COMPUTERNAME" }
    $letter = (Split-Path -Qualifier (Resolve-Path -LiteralPath $WorkRoot).Path).TrimEnd(':')
    $partition = Get-Partition -DriveLetter $letter -ErrorAction Stop
    $disk = Get-Disk -Number $partition.DiskNumber
    $serial = "$($disk.SerialNumber)".Trim().TrimEnd('.')
    if ("$($disk.FriendlyName)" -cne "$($machine.spare_model)" -or $serial -cne "$($machine.spare_serial)" -or "$([uint64]$disk.Size)" -ne "$($machine.spare_bytes)") {
        throw "$WorkRoot is on $($disk.FriendlyName) $($disk.Size), not the drive $file names"
    }
    if ($disk.IsBoot -or $disk.IsSystem) { throw "$WorkRoot is on the disk Windows runs from" }
}

function Get-OpsVhdFor {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    foreach ($path in @($Script:RowVhds)) {
        $image = Get-DiskImage -ImagePath $path -ErrorAction SilentlyContinue
        if ($image -and $image.Attached -and [int]$image.Number -eq $DiskNumber) { return $image }
    }
    return $null
}

function Assert-OpsVhdPin {
    <#
    .SYNOPSIS
        The disk about to be changed is one of this row's own virtual disk files: the path it was
        made at, under -WorkRoot, attached as this number, of the size it was made, and a virtual
        disk by what Windows says of it. Asked again before every step.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    Assert-OpsWorkRootOnSpare
    $image = Get-OpsVhdFor -DiskNumber $DiskNumber
    if ($null -eq $image) { throw "Disk $DiskNumber is not a virtual disk this row made" }
    $root = (Resolve-Path -LiteralPath $WorkRoot).Path.TrimEnd('\') + '\'
    if (-not "$($image.ImagePath)".StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw "$($image.ImagePath) is not under $WorkRoot" }
    $facts = Get-RbDiskFacts -DiskNumber $DiskNumber
    if ([uint64]$facts.size_bytes -ne [uint64]$image.Size) { throw "Disk $DiskNumber is $($facts.size_bytes) bytes, and its VHD $($image.Size)" }
    if ($facts.listed_by_get_disk) {
        $disk = Get-Disk -Number $DiskNumber
        if ("$($disk.FriendlyName)" -ne 'Msft Virtual Disk' -or "$($disk.BusType)" -ne 'File Backed Virtual' -or $disk.IsBoot -or $disk.IsSystem) {
            throw "Disk $DiskNumber is $($disk.FriendlyName) on $($disk.BusType), not a file-backed virtual disk"
        }
    }
    $pin = [ordered]@{ disk_number = $DiskNumber; vhd = "$($image.ImagePath)"; size_bytes = "$($image.Size)"; at_utc = (Get-Date).ToUniversalTime().ToString('o') }
    [void]$Script:OpsPinLog.Add($pin)
    return $pin
}

function Assert-OpsStepPins {
    <#
    .SYNOPSIS
        Every disk a step names is one of this row's own virtual disks, pinned again now. Called by
        Invoke-CertStep in operation-proof mode, so a certification row run as a regression is held
        to the same pins. In a VirtualBox guest a VDI the row was given passes through to the
        certification's own pin check.
    #>
    param([Parameter(Mandatory = $true)] [object]$Pin, [AllowNull()] [object]$SecondPin)
    foreach ($wanted in @($Pin, $SecondPin)) {
        if ($null -eq $wanted) { continue }
        $number = -1
        foreach ($path in @($Script:RowVhds)) {
            $image = Get-DiskImage -ImagePath $path -ErrorAction SilentlyContinue
            if (-not ($image -and $image.Attached)) { continue }
            $facts = Get-RbDiskFacts -DiskNumber ([int]$image.Number)
            if ("$($facts.unique_id)" -ceq "$($wanted.unique_id)" -and "$($facts.size_bytes)" -eq "$($wanted.size_bytes)") { $number = [int]$image.Number }
        }
        if ($number -ge 0) { Assert-OpsVhdPin -DiskNumber $number | Out-Null; continue }
        if ($Script:InVirtualBox) { continue }
        throw "A step names $($wanted.model) $($wanted.size_bytes) bytes, which is not one of this row's virtual disks"
    }
}

function Close-OpsRowVolumes {
    <#
    .SYNOPSIS
        Before the row's virtual disks are detached: every volume on them flushed, then each disk
        taken offline, so no volume is still mounted (and no paging I/O in flight) when its disk
        goes. The readers here close every handle they open, in their own finally blocks.
    #>
    # Each lettered volume is flushed, then dismounted with its letter removed (mountvol /P), which
    # does not wait on a scanner's handle the way taking the whole disk offline did (about three
    # minutes a row on testpc2). A disk with no letters left is then taken offline.
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    $clock = [Diagnostics.Stopwatch]::StartNew()
    foreach ($path in @($Script:RowVhds)) {
        try {
            $image = Get-DiskImage -ImagePath $path -ErrorAction Stop
            if (-not $image.Attached) { continue }
            $number = [int]$image.Number
            foreach ($partition in @(Get-Partition -DiskNumber $number -ErrorAction SilentlyContinue)) {
                if ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter)) {
                    $letter = "$($partition.DriveLetter)"
                    Write-VolumeCache -DriveLetter $letter -ErrorAction SilentlyContinue
                    $said = & mountvol.exe "${letter}:" /P 2>&1 | Out-String
                    if ($LASTEXITCODE -ne 0) { Write-Host "mountvol ${letter}: /P said: $($said.Trim())" }
                }
            }
            $disk = Get-Disk -Number $number -ErrorAction SilentlyContinue
            if ($disk -and -not $disk.IsOffline) { Set-Disk -Number $number -IsOffline $true -ErrorAction Stop }
        } catch { Write-Host "Could not take the volumes of $path down before detaching it: $($_.Exception.Message)" }
    }
    Write-Host "$($Script:RowId): volumes down for detach in $([int]$clock.Elapsed.TotalSeconds) s"
    Start-Sleep -Seconds 2
}

function Save-OpsRowEvents {
    <#
    .SYNOPSIS
        The storage events Windows logged while the row ran, detach included, into the row's
        folder: disk 51 (paging error), Ntfs 50 and 55 (lost write, corruption), 140, 153, 157.
    #>
    param([string]$SinceUtc)
    try {
        $since = [DateTime]::Parse($SinceUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToLocalTime()
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Id = 50, 51, 55, 129, 140, 153, 157 } -ErrorAction SilentlyContinue |
                Sort-Object TimeCreated | ForEach-Object {
                    [ordered]@{ time = $_.TimeCreated.ToString('o'); id = $_.Id; provider = $_.ProviderName; message = (($_.Message -split "`r?`n")[0]) }
                })
        [IO.File]::WriteAllText((Join-Path $Script:RowDir 'windows-events.json'), (ConvertTo-Json -InputObject @($events) -Depth 4))
        if ($events.Count -gt 0) { Write-Host "$($Script:RowId): $($events.Count) storage event(s) logged during the row (windows-events.json)" }
    } catch { Write-Host "Could not read the row's storage events: $($_.Exception.Message)" }
}

function New-OpsDisk {
    param([Parameter(Mandatory = $true)] [string]$Name, [int]$SizeMB = 1024, [ValidateSet('GPT', 'MBR', 'RAW')] [string]$Style = 'GPT')
    Assert-OpsWorkRootOnSpare
    $disk = New-CertVhd -Name "$($Script:RowId.Replace('ops.', ''))-$Name" -SizeMB $SizeMB
    if ($Style -ne 'RAW') { Initialize-Disk -Number $disk -PartitionStyle $Style; Start-Sleep -Seconds 1 }
    return $disk
}

function Get-OpsMedia {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    return Get-CertMedia -DiskNumber $DiskNumber -Class 'virtual_disk' -Kind $Script:VhdKind
}

function Save-OpsFailureReadback {
    # What the disk looked like when a step failed: the row's report keeps only the failure.
    param([int]$DiskNumber, [string]$Step)
    try {
        $state = [ordered]@{ step = $Step; snapshot = (Get-CertSnapshot -DiskNumber $DiskNumber) }
        [IO.File]::WriteAllText((Join-Path $Script:RowDir "failure-$Step.json"), (ConvertTo-Json -InputObject $state -Depth 16))
    } catch { Write-Host "Could not read the disk after $Step failed: $($_.Exception.Message)" }
}

function Invoke-OpsStep {
    <#
    .SYNOPSIS
        One operation through the app's path on a pinned virtual disk, as an operation-proof job.
    #>
    param(
        [Parameter(Mandatory = $true)] [int]$Disk,
        [Parameter(Mandatory = $true)] [string]$Step,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [hashtable]$Select = @{},
        [hashtable]$Choices = @{},
        [ValidateSet('', 'panel', 'assistant')] [string]$Route = '',
        [int]$CancelAfterSeconds = 0,
        [string]$Expect = 'success',
        [string]$RefusalContains = '',
        [int]$TimeoutSeconds = 1800,
        [switch]$Elevated,
        # A disk the VM rig gave the row (a VDI), already pinned by Get-CertReadPin. Only in a guest.
        [object]$GivenPin = $null,
        # A step whose failure still leaves something worth reading back (a Format whose tool ran
        # and whose Apply then failed for another reason): the failure becomes a failed check and
        # the row reads the disk anyway. The row's verdict is FAIL either way.
        [switch]$ReadBackOnFailure
    )
    if ($null -ne $GivenPin) {
        if (-not $Script:InVirtualBox) { throw 'Only a VirtualBox guest hands a row its disk' }
    } else { Assert-OpsVhdPin -DiskNumber $Disk | Out-Null }
    $pin = if ($null -ne $GivenPin) { $GivenPin } else { Get-CertPin -DiskNumber $Disk }
    $fields = @{ operation_proof = $true }
    if ($Route) { $fields.route = $Route }
    if ($CancelAfterSeconds -gt 0) { $fields.cancel_after_seconds = $CancelAfterSeconds }
    try {
        return Invoke-CertStep -Step $Step -Operation $Operation -Pin $pin -Select $Select -Choices $Choices `
            -Expect $Expect -RefusalContains $RefusalContains -TimeoutSeconds $TimeoutSeconds -Elevated:$Elevated -ProofFields $fields
    } catch {
        Save-OpsFailureReadback -DiskNumber $Disk -Step $Step
        if (-not $ReadBackOnFailure) { throw }
        [void]$Script:OpsStepFailures.Add([pscustomobject]@{ row = $Script:RowId; check = (New-OpsCheck -Name "step $Step ($Operation) did what the row expected" -Passed $false -Detail $_.Exception.Message) })
        return $null
    }
}

function Get-OpsRawPartition {
    # The partition at an offset, as the raw table says, beside Windows' number for it.
    param([int]$DiskNumber, [uint64]$Offset)
    $snapshot = Get-CertSnapshot -DiskNumber $DiskNumber
    $raw = @($snapshot.table.partitions | Where-Object { [uint64]$_.offset_bytes -eq $Offset })
    $windows = @($snapshot.partitions | Where-Object { [uint64]$_.offset_bytes -eq $Offset })
    return [pscustomobject]@{ raw = $(if ($raw.Count -eq 1) { $raw[0] } else { $null }); windows = $(if ($windows.Count -eq 1) { $windows[0] } else { $null }); snapshot = $snapshot }
}

function New-OpsRawPartition {
    # A partition with no file system and no letter, made with Windows' tools.
    param([int]$DiskNumber, [int]$SizeMB, [uint64]$Offset = 0)
    $arguments = @{ DiskNumber = $DiskNumber; Size = ([uint64]$SizeMB * $Script:OpsMiB) }
    if ($Offset -gt 0) { $arguments.Offset = $Offset }
    $partition = New-Partition @arguments
    Start-Sleep -Seconds 1
    return [pscustomobject]@{ partition_number = [int]$partition.PartitionNumber; offset = [uint64]$partition.Offset }
}

function Wait-OpsLetter {
    # The letter Windows gave the partition at Offset, once its file system is readable.
    param([int]$DiskNumber, [uint64]$Offset)
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        Update-HostStorageCache
        $partition = Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Where-Object { [uint64]$_.Offset -eq $Offset } | Select-Object -First 1
        if ($partition -and $partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter)) {
            $letter = "$($partition.DriveLetter)"
            $null = Get-PSDrive -PSProvider FileSystem
            if ([IO.Directory]::Exists("${letter}:\")) { return $letter }
        }
        Start-Sleep -Seconds 2
    }
    return ''
}

function Test-OpsNonNativePartition {
    <#
    .SYNOPSIS
        Reads a non-Windows file system the app wrote: from a copy of the partition for ext, HFS+
        and APFS, from the device for swap. Returns the checks and the image it read, if any.
    #>
    param([string]$Prefix, [int]$DiskNumber, [int]$PartitionNumber, [string]$FileSystem, [object]$Label = $null, [string]$Name)
    $fs = $FileSystem.ToLowerInvariant()
    if ($fs -in 'linux swap', 'swap') {
        return [pscustomobject]@{ checks = @(Test-OpsSwap -Prefix $Prefix -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -Label $Label); image = $null }
    }
    $image = Copy-OpsPartitionImage -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -Name $Name
    $checks = switch -Regex ($fs) {
        '^ext[234]$' { Test-OpsExtImage -Prefix $Prefix -Image $image -Kind $fs -Label $Label }
        '^(hfs\+|hfsplus)$' { Test-OpsHfsImage -Prefix $Prefix -Image $image -Kind 'HFS+' }
        '^hfsx$' { Test-OpsHfsImage -Prefix $Prefix -Image $image -Kind 'HFSX' }
        '^apfs$' { Test-OpsApfsImage -Prefix $Prefix -Image $image }
        default { throw "No reader for $FileSystem" }
    }
    $image | Add-Member -NotePropertyName expect -NotePropertyValue $fs -Force
    return [pscustomobject]@{ checks = @($checks); image = $image }
}

function Test-OpsExtent {
    # The raw table still holds a partition at Offset of SizeBytes.
    param([string]$Name, [int]$DiskNumber, [uint64]$Offset, [uint64]$SizeBytes)
    $found = Get-OpsRawPartition -DiskNumber $DiskNumber -Offset $Offset
    $size = if ($found.raw) { [uint64]$found.raw.size_bytes } else { 0 }
    return New-OpsCheck -Name $Name -Passed ($size -eq $SizeBytes) -Detail "offset $Offset size $size, wanted $SizeBytes"
}

function Complete-OpsRow {
    param([object]$Media, [object[]]$Checks, [System.Collections.IDictionary]$Extra = @{})
    # Only this row's: a row that stopped early never reached here to clear its own.
    $all = @($Script:OpsStepFailures | Where-Object { $_.row -ceq $Script:RowId } | ForEach-Object { $_.check }) + @($Checks | ForEach-Object { $_ })
    $independent = [ordered]@{ checks = $all; pins = @($Script:OpsPinLog) }
    foreach ($key in $Extra.Keys) { $independent[$key] = $Extra[$key] }
    $Script:OpsPinLog.Clear()
    $Script:OpsStepFailures.Clear()
    return @{ media = $Media; independent = $independent }
}

function Get-OpsVolumeMiB {
    # FAT32 needs at least 65,525 clusters, so with 4 KiB clusters a volume under about 260 MiB is
    # one Windows refuses to format. A FAT32 fixture is made larger; the rest keep the row's size.
    param([string]$FileSystem, [int]$MiB)
    if ($FileSystem -eq 'FAT32' -and $MiB -lt 384) { return 384 }
    return $MiB
}

function Add-OpsRow {
    param([Parameter(Mandatory = $true)] [string]$Id, [Parameter(Mandatory = $true)] [scriptblock]$Body)
    if ($Id -notlike 'ops.*') { throw "An operation-proof row is named ops.*: $Id" }
    $Script:CertRowBodies[$Id] = $Body
}

# --- Create ---------------------------------------------------------------------------------------

function Invoke-OpsCreateWindows {
    param([string]$FileSystem, [string]$Label, [uint64]$ClusterBytes, [string]$Route = '')
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $mib = Get-OpsVolumeMiB -FileSystem $FileSystem -MiB 256
    Invoke-OpsStep -Disk $disk -Step 'create' -Operation 'Create' -Route $Route -ReadBackOnFailure -Choices @{
        size_mib = $mib; free_before_mib = 0; file_system = $FileSystem; allocation_unit_bytes = $ClusterBytes
        partition_type = $Script:GptBasicData; label = $Label; full_format = $false
    } | Out-Null
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $data = @($snapshot.data_partitions)
    $checks = @(New-OpsCheck -Name 'the raw table holds one data partition' -Passed ($data.Count -eq 1) -Detail $data.Count)
    if ($data.Count -eq 1) {
        $checks += New-OpsCheck -Name "it is $mib MiB" -Passed ([uint64]$data[0].size_bytes -eq [uint64]$mib * $Script:OpsMiB) -Detail "$($data[0].size_bytes)"
        $checks += New-OpsCheck -Name 'its type is basic data' -Passed ("$($data[0].type)" -eq $Script:OpsGptBasicData) -Detail "$($data[0].type)"
        $letter = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$data[0].offset_bytes)
        $checks += New-OpsCheck -Name 'Windows mounted it with a letter' -Passed ([bool]$letter) -Detail $letter
        if ($letter) { $checks += Test-OpsWindowsVolume -Prefix 'the new volume:' -Letter $letter -FileSystem $FileSystem -Label $Label -ClusterBytes $ClusterBytes }
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

function Invoke-OpsAssistantCreateRefused {
    # The assistant's partition.apply_operation, asked to Create a non-Windows file system from
    # the fields its tool documents (file system, label, size). Coordinator's ruling, 2026-11-11:
    # the app refuses that early, says to create a Windows file system and then Format, and writes
    # nothing. The expected words follow that guidance ("Format").
    param([string]$FileSystem, [object]$Label)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $before = Get-CertSnapshot -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -Step 'create' -Operation 'Create' -Route 'assistant' -ReadBackOnFailure -Expect 'refused' -RefusalContains 'Format' -Choices @{
        size_mib = 128; free_before_mib = 0; file_system = $FileSystem; allocation_unit_bytes = 0
        partition_type = ''; label = "$Label"; full_format = $false
    } | Out-Null
    $after = Get-CertSnapshot -DiskNumber $disk
    $unchanged = "$($before.raw.gpt_entries_hex)" -eq "$($after.raw.gpt_entries_hex)" -and "$($before.raw.lba0_hex)" -eq "$($after.raw.lba0_hex)"
    $checks = @(New-OpsCheck -Name 'nothing was written: the partition table is as it was' -Passed $unchanged -Detail @($after.data_partitions).Count)
    return Complete-OpsRow -Media $media -Checks $checks
}

function Invoke-OpsAssistantTwoStep {
    # What the assistant does instead: Create a Windows file system, then Format it as the
    # non-Windows one, each from its tool's documented fields only (the core request functions'
    # payloads), then read back like the panel's Format rows.
    param([string]$FileSystem, [object]$Label)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -Step 'create' -Operation 'Create' -Route 'assistant' -Choices @{
        size_mib = 128; free_before_mib = 0; file_system = 'NTFS'; allocation_unit_bytes = 0
        partition_type = $Script:GptBasicData; label = 'OPSSTEP1'; full_format = $false
    } | Out-Null
    $data = @((Get-CertSnapshot -DiskNumber $disk).data_partitions)
    if ($data.Count -ne 1) { throw "The assistant's Create left $($data.Count) data partitions" }
    $offset = [uint64]$data[0].offset_bytes
    $number = [int](Get-Partition -DiskNumber $disk | Where-Object { [uint64]$_.Offset -eq $offset }).PartitionNumber
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Route 'assistant' -ReadBackOnFailure -Select @{ partition_number = $number } -Choices @{
        file_system = $FileSystem; allocation_unit_bytes = 0; label = "$Label"; full_format = $false
    } | Out-Null
    $checks = @(Test-OpsExtent -Name 'the partition kept its place and size' -DiskNumber $disk -Offset $offset -SizeBytes (128 * $Script:OpsMiB))
    $read = Test-OpsNonNativePartition -Prefix "the formatted $FileSystem`:" -DiskNumber $disk -PartitionNumber $number -FileSystem $FileSystem -Label $Label -Name 'two-step'
    $checks += $read.checks
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($read.image | Where-Object { $_ }) })
}

Add-OpsRow 'ops.create-exfat' { Invoke-OpsCreateWindows -FileSystem 'exFAT' -Label 'OPSEXFAT' -ClusterBytes 4096 }
Add-OpsRow 'ops.create-fat32' { Invoke-OpsCreateWindows -FileSystem 'FAT32' -Label 'OPSFAT32' -ClusterBytes 4096 }
Add-OpsRow 'ops.create-refs-assistant' { Invoke-OpsCreateWindows -FileSystem 'ReFS' -Label 'OPSREFS' -ClusterBytes 4096 -Route 'assistant' }
Add-OpsRow 'ops.create-ext2-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'ext2' -Label 'OPSEXT2' }
Add-OpsRow 'ops.create-ext3-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'ext3' -Label 'OPSEXT3' }
Add-OpsRow 'ops.create-ext4-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'ext4' -Label 'OPSEXT4' }
Add-OpsRow 'ops.create-hfsplus-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'HFS+' -Label 'OPSHFS' }
Add-OpsRow 'ops.create-hfsx-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'HFSX' -Label 'OPSHFSX' }
Add-OpsRow 'ops.create-apfs-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'APFS' -Label 'OPSAPFS' }
Add-OpsRow 'ops.create-swap-assistant' { Invoke-OpsAssistantCreateRefused -FileSystem 'Linux swap' -Label 'OPSSWAP' }
Add-OpsRow 'ops.assistant-two-step-ext2' { Invoke-OpsAssistantTwoStep -FileSystem 'ext2' -Label 'OPSEXT2' }
Add-OpsRow 'ops.assistant-two-step-ext3' { Invoke-OpsAssistantTwoStep -FileSystem 'ext3' -Label 'OPSEXT3' }
Add-OpsRow 'ops.assistant-two-step-ext4' { Invoke-OpsAssistantTwoStep -FileSystem 'ext4' -Label 'OPSEXT4' }
Add-OpsRow 'ops.assistant-two-step-hfsplus' { Invoke-OpsAssistantTwoStep -FileSystem 'HFS+' -Label 'OPSHFS' }
Add-OpsRow 'ops.assistant-two-step-hfsx' { Invoke-OpsAssistantTwoStep -FileSystem 'HFSX' -Label 'OPSHFSX' }
Add-OpsRow 'ops.assistant-two-step-apfs' { Invoke-OpsAssistantTwoStep -FileSystem 'APFS' -Label 'OPSAPFS' }
Add-OpsRow 'ops.assistant-two-step-swap' { Invoke-OpsAssistantTwoStep -FileSystem 'Linux swap' -Label 'OPSSWAP' }

# --- Format ---------------------------------------------------------------------------------------

function Invoke-OpsFormat {
    # Over a lettered NTFS volume, as a person reformats one: that is the case a dismount must handle.
    param([string]$FileSystem, [object]$Label, [uint64]$ClusterBytes = 0)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $mib = Get-OpsVolumeMiB -FileSystem $FileSystem -MiB 256
    $old = New-CertVolume -DiskNumber $disk -SizeBytes ([uint64]$mib * $Script:OpsMiB) -Label 'OPSOLD'
    Write-CertMarkerFile -Letter $old.letter
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $old.partition_number).Offset
    $choices = @{ file_system = $FileSystem; allocation_unit_bytes = $ClusterBytes; label = "$Label"; full_format = $false }
    if ($FileSystem -eq 'Linux swap') { $choices.linux_swap_page_size_bytes = '4096' }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select @{ partition_number = $old.partition_number } -Choices $choices -ReadBackOnFailure | Out-Null
    $checks = @(Test-OpsExtent -Name 'the partition kept its place and size' -DiskNumber $disk -Offset $offset -SizeBytes ([uint64]$mib * $Script:OpsMiB))
    $images = @()
    if ($FileSystem -in 'NTFS', 'exFAT', 'FAT32') {
        $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
        $checks += New-OpsCheck -Name 'Windows mounted it with a letter' -Passed ([bool]$letter) -Detail $letter
        if ($letter) {
            $checks += Test-OpsWindowsVolume -Prefix 'the formatted volume:' -Letter $letter -FileSystem $FileSystem -Label $Label -ClusterBytes $ClusterBytes
            $checks += New-OpsCheck -Name 'the old file is gone' -Passed (-not (Test-Path -LiteralPath "${letter}:\marker.bin")) -Detail ''
        }
    } else {
        $number = [int](Get-Partition -DiskNumber $disk | Where-Object { [uint64]$_.Offset -eq $offset }).PartitionNumber
        $read = Test-OpsNonNativePartition -Prefix "the formatted $FileSystem`:" -DiskNumber $disk -PartitionNumber $number -FileSystem $FileSystem -Label $Label -Name 'formatted'
        $checks += $read.checks
        $images = @($read.image | Where-Object { $_ })
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = $images; after = (Get-CertSnapshot -DiskNumber $disk) })
}

Add-OpsRow 'ops.format-exfat' { Invoke-OpsFormat -FileSystem 'exFAT' -Label 'OPSEXFAT' -ClusterBytes 4096 }
Add-OpsRow 'ops.format-fat32' { Invoke-OpsFormat -FileSystem 'FAT32' -Label 'OPSFAT32' -ClusterBytes 4096 }
Add-OpsRow 'ops.format-ntfs' { Invoke-OpsFormat -FileSystem 'NTFS' -Label 'OPSNTFS' -ClusterBytes 8192 }
Add-OpsRow 'ops.format-ext2' { Invoke-OpsFormat -FileSystem 'ext2' -Label 'OPSEXT2' }
Add-OpsRow 'ops.format-ext3' { Invoke-OpsFormat -FileSystem 'ext3' -Label 'OPSEXT3' }
Add-OpsRow 'ops.format-ext4' { Invoke-OpsFormat -FileSystem 'ext4' -Label 'OPSEXT4' }
Add-OpsRow 'ops.format-swap' { Invoke-OpsFormat -FileSystem 'Linux swap' -Label 'OPSSWAP' }
Add-OpsRow 'ops.format-hfsplus' { Invoke-OpsFormat -FileSystem 'HFS+' -Label 'OPSHFS' }
Add-OpsRow 'ops.format-hfsx' { Invoke-OpsFormat -FileSystem 'HFSX' -Label 'OPSHFSX' }
Add-OpsRow 'ops.format-apfs' { Invoke-OpsFormat -FileSystem 'APFS' -Label 'OPSAPFS' }

# --- Resize ---------------------------------------------------------------------------------------

function Invoke-OpsShrinkWindows {
    param([string]$FileSystem)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (512 * $Script:OpsMiB) -FileSystem $FileSystem -Label 'OPSSHRINK' -ClusterBytes 4096
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $before = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 12 -MaxKilobytes 1024
    Invoke-OpsStep -Disk $disk -Step 'shrink' -Operation 'Resize' -Select @{ partition_number = $volume.partition_number } -Choices @{ target_size_mib = 256 } | Out-Null
    $checks = @(Test-OpsExtent -Name 'the raw table holds it at 256 MiB' -DiskNumber $disk -Offset $offset -SizeBytes (256 * $Script:OpsMiB))
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    if ($letter) {
        $facts = Get-RbVolumeFacts -Letter $letter
        $checks += New-OpsCheck -Name 'Windows sees a volume under 256 MiB' -Passed ([uint64]$facts.size_bytes -le 256 * $Script:OpsMiB -and [uint64]$facts.size_bytes -gt 200 * $Script:OpsMiB) -Detail "$($facts.size_bytes)"
        $checks += Test-OpsFilesSame -Name 'every file is still there, byte for byte' -Before $before -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'the shrunk volume:' -Letter $letter -FileSystem $FileSystem -Label 'OPSSHRINK'
    } else { $checks += New-OpsCheck -Name 'the shrunk volume is mounted' -Passed $false -Detail '' }
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.shrink-ntfs' { Invoke-OpsShrinkWindows -FileSystem 'NTFS' }
Add-OpsRow 'ops.shrink-fat32' { Invoke-OpsShrinkWindows -FileSystem 'FAT32' }
Add-OpsRow 'ops.shrink-exfat' { Invoke-OpsShrinkWindows -FileSystem 'exFAT' }
Add-OpsRow 'ops.shrink-refs' { Invoke-OpsShrinkWindows -FileSystem 'ReFS' }

function Invoke-OpsExtResize {
    # Formats a bare partition with the app (the fixture Windows cannot make), then grows and
    # shrinks it, reading the file system from a copy after each step.
    param([string]$Kind)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    $images = @()
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = $Kind; allocation_unit_bytes = 0; label = 'OPSRESIZE'; full_format = $false } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after format:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem $Kind -Label 'OPSRESIZE' -Name 'format'
    $checks = @($read.checks); $images += $read.image
    Invoke-OpsStep -Disk $disk -Step 'grow' -Operation 'Resize' -Select $select -Choices @{ target_size_mib = 256; administrator_scan = $true } | Out-Null
    $checks += Test-OpsExtent -Name 'grow: the raw table holds it at 256 MiB' -DiskNumber $disk -Offset $part.offset -SizeBytes (256 * $Script:OpsMiB)
    $read = Test-OpsNonNativePartition -Prefix 'after grow:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem $Kind -Label 'OPSRESIZE' -Name 'grow'
    $checks += $read.checks; $images += $read.image
    Invoke-OpsStep -Disk $disk -Step 'shrink' -Operation 'Resize' -Select $select -Choices @{ target_size_mib = 160; administrator_scan = $true } | Out-Null
    $checks += Test-OpsExtent -Name 'shrink: the raw table holds it at 160 MiB' -DiskNumber $disk -Offset $part.offset -SizeBytes (160 * $Script:OpsMiB)
    $read = Test-OpsNonNativePartition -Prefix 'after shrink:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem $Kind -Label 'OPSRESIZE' -Name 'shrink'
    $checks += $read.checks; $images += $read.image
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($images | Where-Object { $_ }) })
}

Add-OpsRow 'ops.resize-ext2' { Invoke-OpsExtResize -Kind 'ext2' }
Add-OpsRow 'ops.resize-ext3' { Invoke-OpsExtResize -Kind 'ext3' }
Add-OpsRow 'ops.resize-ext4' { Invoke-OpsExtResize -Kind 'ext4' }

Add-OpsRow 'ops.resize-unknown-fs-below-fs' {
    # F7 (coordinator, 2026-11-11): a partition whose file system the app has not identified must
    # never be resized, least of all shrunk. The step starts from the standard scan, which cannot
    # read a raw partition, and asks to shrink a 128 MiB ext4 partition to 64 MiB. The row expects
    # a refusal and reads the disk back either way: the extent, and e2fsck on a copy.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'ext4'; allocation_unit_bytes = 0; label = 'OPSKEEP'; full_format = $false } | Out-Null
    Invoke-OpsStep -Disk $disk -Step 'shrink' -Operation 'Resize' -Select $select -Choices @{ target_size_mib = 64 } -Expect 'refused' -RefusalContains "could not identify this partition's file system" -ReadBackOnFailure | Out-Null
    $checks = @(Test-OpsExtent -Name 'the partition is still 128 MiB' -DiskNumber $disk -Offset $part.offset -SizeBytes (128 * $Script:OpsMiB))
    $read = Test-OpsNonNativePartition -Prefix 'afterwards:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'ext4' -Label 'OPSKEEP' -Name 'after'
    $checks += $read.checks
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($read.image | Where-Object { $_ }) })
}

Add-OpsRow 'ops.apfs-container-resize' {
    # An APFS container written into 128 MiB, the partition grown to 256 MiB with Windows' tools,
    # then the app's "Resize container to fill partition".
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'APFS'; allocation_unit_bytes = 0; label = 'OPSAPFS'; full_format = $false } | Out-Null
    Resize-Partition -DiskNumber $disk -PartitionNumber $part.partition_number -Size (256 * $Script:OpsMiB)
    $before = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'before'
    $beforeBlocks = [BitConverter]::ToUInt64((Read-OpsImageBytes -Path $before.path -Offset 0 -Length 4096), 40)
    Invoke-OpsStep -Disk $disk -Step 'resize' -Operation 'ApfsResizeContainer' -Select $select -Choices @{ administrator_scan = $true } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after resize:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'APFS' -Name 'after'
    $block = Read-OpsImageBytes -Path $read.image.path -Offset 0 -Length 4096
    $blockSize = [uint64][BitConverter]::ToUInt32($block, 36)
    $afterBlocks = [BitConverter]::ToUInt64($block, 40)
    $checks = @($read.checks)
    $checks += New-OpsCheck -Name 'the container now fills the 256 MiB partition' -Passed ($afterBlocks * $blockSize -gt 250 * $Script:OpsMiB -and $afterBlocks * $blockSize -le 256 * $Script:OpsMiB) -Detail "$beforeBlocks -> $afterBlocks blocks of $blockSize"
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($before, $read.image) })
}

# --- Check and repair -----------------------------------------------------------------------------

function Invoke-OpsCheckWindows {
    param([string]$FileSystem)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes ([uint64](Get-OpsVolumeMiB -FileSystem $FileSystem -MiB 256) * $Script:OpsMiB) -FileSystem $FileSystem -Label 'OPSCHECK' -ClusterBytes 4096
    $before = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $tableBefore = Get-CertSnapshot -DiskNumber $disk
    $step = Invoke-OpsStep -Disk $disk -Step 'check' -Operation 'CheckFileSystem' -Select @{ partition_number = $volume.partition_number }
    $tableAfter = Get-CertSnapshot -DiskNumber $disk
    $said = (@($step.execution.steps) | ForEach-Object { "$($_.stdout)" }) -join ' '
    $checks = @(New-OpsCheck -Name 'the check reported a result' -Passed ([bool]$said.Trim()) -Detail ($said.Substring(0, [Math]::Min(400, $said.Length))))
    $checks += New-OpsCheck -Name 'the partition table did not change' -Passed ($tableBefore.raw.lba0_hex -eq $tableAfter.raw.lba0_hex -and "$($tableBefore.raw.gpt_entries_hex)" -eq "$($tableAfter.raw.gpt_entries_hex)") -Detail ''
    $checks += Test-OpsFilesSame -Name 'every file is unchanged' -Before $before -After (Get-RbFileManifest -Root "$($volume.letter):\fixture")
    $checks += Test-OpsWindowsVolume -Prefix 'after the check:' -Letter $volume.letter -FileSystem $FileSystem -Label 'OPSCHECK'
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.check-ntfs' { Invoke-OpsCheckWindows -FileSystem 'NTFS' }
Add-OpsRow 'ops.check-fat32' { Invoke-OpsCheckWindows -FileSystem 'FAT32' }
Add-OpsRow 'ops.check-exfat' { Invoke-OpsCheckWindows -FileSystem 'exFAT' }

function Invoke-OpsRepairNonNative {
    # The app formats a bare partition, then its read-only check and its repair run on it. The
    # repair of a clean file system must leave it clean.
    param([string]$FileSystem)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = $FileSystem; allocation_unit_bytes = 0; label = 'OPSREPAIR'; full_format = $false } | Out-Null
    $checks = @()
    $check = Invoke-OpsStep -Disk $disk -Step 'check' -Operation 'CheckNonNativeFileSystem' -Select $select -Choices @{ administrator_scan = $true }
    $checks += New-OpsCheck -Name 'the read-only check ran its tool and it passed' -Passed ([bool]$check.file_system_check.success) -Detail $check.file_system_check
    Invoke-OpsStep -Disk $disk -Step 'repair' -Operation 'RepairFileSystem' -Select $select -Choices @{ administrator_scan = $true } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after repair:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem $FileSystem -Name 'repaired'
    $checks += $read.checks
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($read.image | Where-Object { $_ }) })
}

Add-OpsRow 'ops.repair-ext2' { Invoke-OpsRepairNonNative -FileSystem 'ext2' }
Add-OpsRow 'ops.repair-ext3' { Invoke-OpsRepairNonNative -FileSystem 'ext3' }
Add-OpsRow 'ops.repair-ext4' { Invoke-OpsRepairNonNative -FileSystem 'ext4' }
Add-OpsRow 'ops.repair-hfsplus' { Invoke-OpsRepairNonNative -FileSystem 'HFS+' }
Add-OpsRow 'ops.repair-hfsx' { Invoke-OpsRepairNonNative -FileSystem 'HFSX' }
Add-OpsRow 'ops.repair-apfs' { Invoke-OpsRepairNonNative -FileSystem 'APFS' }

Add-OpsRow 'ops.scan-file-systems' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $one = New-CertVolume -DiskNumber $disk -SizeBytes (192 * $Script:OpsMiB) -Label 'OPSSCAN1'
    $two = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:OpsMiB) -FileSystem 'FAT32' -Label 'OPSSCAN2'
    $filesOne = Write-RbFixtureTree -Root "$($one.letter):\fixture" -FileCount 6
    $filesTwo = Write-RbFixtureTree -Root "$($two.letter):\fixture" -FileCount 6
    $tableBefore = Get-CertSnapshot -DiskNumber $disk
    $step = Invoke-OpsStep -Disk $disk -Step 'scan' -Operation 'ScanFileSystems'
    $tableAfter = Get-CertSnapshot -DiskNumber $disk
    $said = (@($step.execution.steps) | ForEach-Object { "$($_.stdout)" }) -join ' '
    $checks = @(New-OpsCheck -Name 'the scan reported a result' -Passed ([bool]$said.Trim()) -Detail ($said.Substring(0, [Math]::Min(400, $said.Length))))
    $checks += New-OpsCheck -Name 'the partition table did not change' -Passed ("$($tableBefore.raw.gpt_entries_hex)" -eq "$($tableAfter.raw.gpt_entries_hex)") -Detail ''
    $checks += Test-OpsFilesSame -Name 'NTFS files unchanged' -Before $filesOne -After (Get-RbFileManifest -Root "$($one.letter):\fixture")
    $checks += Test-OpsFilesSame -Name 'FAT32 files unchanged' -Before $filesTwo -After (Get-RbFileManifest -Root "$($two.letter):\fixture")
    return Complete-OpsRow -Media $media -Checks $checks
}

# --- Letters, labels, flags and types ---------------------------------------------------------------

Add-OpsRow 'ops.set-letter-and-label' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSLBL'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 6
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter)" })
    $newLetter = @('Q', 'P', 'O', 'N', 'M', 'L', 'K') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
    Invoke-OpsStep -Disk $disk -Step 'letter' -Operation 'SetDriveLetter' -Select @{ partition_number = $volume.partition_number } -Choices @{ new_drive_letter = $newLetter } | Out-Null
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    $checks = @(New-OpsCheck -Name "the volume is now at ${newLetter}:" -Passed ($letter -eq $newLetter) -Detail $letter)
    $checks += New-OpsCheck -Name "the old letter $($volume.letter): is free" -Passed (-not (Test-Path -LiteralPath "$($volume.letter):\")) -Detail ''
    Invoke-OpsStep -Disk $disk -Step 'label' -Operation 'SetPartitionLabel' -Select @{ partition_number = $volume.partition_number } -Choices @{ label = 'OPSRENAMED' } | Out-Null
    if ($letter) {
        $checks += Test-OpsWindowsVolume -Prefix 'after both:' -Letter $letter -FileSystem 'NTFS' -Label 'OPSRENAMED'
        $checks += Test-OpsFilesSame -Name 'every file is unchanged' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture")
    }
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.hide-unhide-gpt' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSHIDE'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'hide' -Operation 'SetPartitionHidden' -Select $select -Choices @{ hidden = $true } | Out-Null
    $entry = Get-OpsGptEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name 'hide: the GPT entry has the hidden attribute' -Passed (($entry.attributes -band $Script:OpsGptHiddenAttribute) -ne 0) -Detail ('0x{0:X16}' -f $entry.attributes))
    $checks += New-OpsCheck -Name 'hide: it has no letter' -Passed (-not (Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).DriveLetter -or [int][char](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).DriveLetter -eq 0) -Detail ''
    Invoke-OpsStep -Disk $disk -Step 'unhide' -Operation 'SetPartitionHidden' -Select $select -Choices @{ hidden = $false } | Out-Null
    $entry = Get-OpsGptEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name 'unhide: the hidden attribute is clear' -Passed (($entry.attributes -band $Script:OpsGptHiddenAttribute) -eq 0) -Detail ('0x{0:X16}' -f $entry.attributes)
    $checks += New-OpsCheck -Name 'unhide: Windows says it is not hidden' -Passed (-not (Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).IsHidden) -Detail ''
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.hide-unhide-mbr' {
    $disk = New-OpsDisk -Name 'disk' -Style 'MBR'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSHIDE'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    $before = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    Invoke-OpsStep -Disk $disk -Step 'hide' -Operation 'SetPartitionHidden' -Select $select -Choices @{ hidden = $true } | Out-Null
    $hidden = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name 'hide: the MBR type byte became hidden NTFS (17)' -Passed ($hidden.type -eq '17') -Detail "$($before.type) -> $($hidden.type)")
    Invoke-OpsStep -Disk $disk -Step 'unhide' -Operation 'SetPartitionHidden' -Select $select -Choices @{ hidden = $false } | Out-Null
    $shown = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name 'unhide: the MBR type byte is NTFS (07) again' -Passed ($shown.type -eq '07') -Detail $shown.type
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.set-active-mbr' {
    $disk = New-OpsDisk -Name 'disk' -Style 'MBR'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSACT'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'active' -Operation 'SetPartitionActive' -Select $select -Choices @{ active = $true } | Out-Null
    $on = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name 'active: the MBR boot flag is 80' -Passed ($on.boot_flag -eq '80') -Detail $on.boot_flag)
    Invoke-OpsStep -Disk $disk -Step 'inactive' -Operation 'SetPartitionActive' -Select $select -Choices @{ active = $false } | Out-Null
    $off = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name 'inactive: the MBR boot flag is 00' -Passed ($off.boot_flag -eq '00') -Detail $off.boot_flag
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.type-id-gpt' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSTYPE'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'linux' -Operation 'SetPartitionTypeId' -Select $select -Choices @{ type_id = '{0FC63DAF-8483-4772-8E79-3D69D8477DE4}' } | Out-Null
    $entry = Get-OpsGptEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name 'the GPT entry type is Linux data' -Passed ($entry.type -eq $Script:OpsGptLinuxData) -Detail $entry.type)
    Invoke-OpsStep -Disk $disk -Step 'basic' -Operation 'SetPartitionTypeId' -Select $select -Choices @{ type_id = '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}' } | Out-Null
    $entry = Get-OpsGptEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name 'the GPT entry type is basic data again' -Passed ($entry.type -eq $Script:OpsGptBasicData) -Detail $entry.type
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.type-id-mbr' {
    # The dialog asks for "New MBR type or GPT GUID". MBR types are written in hex, so a
    # technician types 83 for Linux or 0x83; both must give 0x83.
    $disk = New-OpsDisk -Name 'disk' -Style 'MBR'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSTYPE'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'hex-prefixed' -Operation 'SetPartitionTypeId' -Select $select -Choices @{ type_id = '0x83' } | Out-Null
    $entry = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name "typed 0x83: the MBR type byte is 83" -Passed ($entry.type -eq '83') -Detail $entry.type)
    Invoke-OpsStep -Disk $disk -Step 'back-to-ntfs' -Operation 'SetPartitionTypeId' -Select $select -Choices @{ type_id = '0x07' } | Out-Null
    $entry = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name "typed 0x07: the MBR type byte is 07" -Passed ($entry.type -eq '07') -Detail $entry.type
    Invoke-OpsStep -Disk $disk -Step 'bare-hex' -Operation 'SetPartitionTypeId' -Select $select -Choices @{ type_id = '83' } | Out-Null
    $entry = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks += New-OpsCheck -Name "typed 83: the MBR type byte is 83" -Passed ($entry.type -eq '83') -Detail $entry.type
    return Complete-OpsRow -Media $media -Checks $checks
}

# --- Disk-wide ------------------------------------------------------------------------------------

function Invoke-OpsInitialize {
    param([ValidateSet('GPT', 'MBR')] [string]$Style)
    $disk = New-OpsDisk -Name 'disk' -Style 'RAW'
    $media = Get-OpsMedia -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -Step 'initialize' -Operation 'InitializeDisk' -Choices @{ target_style = $Style } | Out-Null
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks = @(New-OpsCheck -Name "the raw table is $Style" -Passed ($snapshot.table.style -eq $Style) -Detail $snapshot.table.style)
    $checks += New-OpsCheck -Name 'it holds no data partition' -Passed (@($snapshot.data_partitions).Count -eq 0) -Detail @($snapshot.data_partitions).Count
    $checks += New-OpsCheck -Name "Windows reports $Style" -Passed ($snapshot.partition_style_reported -eq $Style) -Detail $snapshot.partition_style_reported
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

Add-OpsRow 'ops.initialize-gpt' { Invoke-OpsInitialize -Style 'GPT' }
Add-OpsRow 'ops.initialize-mbr' { Invoke-OpsInitialize -Style 'MBR' }

Add-OpsRow 'ops.delete-all' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    foreach ($label in 'OPSDEL1', 'OPSDEL2', 'OPSDEL3') { New-CertVolume -DiskNumber $disk -SizeBytes (128 * $Script:OpsMiB) -Label $label | Out-Null }
    Invoke-OpsStep -Disk $disk -Step 'delete-all' -Operation 'DeleteAllPartitions' | Out-Null
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks = @(New-OpsCheck -Name 'the raw table holds no data partition' -Passed (@($snapshot.data_partitions).Count -eq 0) -Detail @($snapshot.data_partitions).Count)
    $checks += New-OpsCheck -Name 'the disk is still GPT' -Passed ($snapshot.table.style -eq 'GPT') -Detail $snapshot.table.style
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

Add-OpsRow 'ops.merge' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $target = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSMERGEA'
    $source = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSMERGEB'
    $targetFiles = Write-RbFixtureTree -Root "$($target.letter):\fixture" -FileCount 6
    $sourceFiles = Write-RbFixtureTree -Root "$($source.letter):\fixture" -FileCount 6 -Seed 7
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $target.partition_number).Offset
    Invoke-OpsStep -Disk $disk -Step 'merge' -Operation 'Merge' -Select @{ partition_number = $target.partition_number } -Choices @{ source_partition_number = $source.partition_number } -TimeoutSeconds 3600 | Out-Null
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks = @(New-OpsCheck -Name 'one data partition is left' -Passed (@($snapshot.data_partitions).Count -eq 1) -Detail @($snapshot.data_partitions).Count)
    $checks += Test-OpsExtent -Name 'it spans both, 512 MiB' -DiskNumber $disk -Offset $offset -SizeBytes (512 * $Script:OpsMiB)
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    if ($letter) {
        $checks += Test-OpsFilesSame -Name "the kept volume's files are unchanged" -Before $targetFiles -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $moved = "${letter}:\Merged-Partition-$($source.partition_number)\fixture"
        $after = if (Test-Path -LiteralPath $moved) { Get-RbFileManifest -Root $moved } else { $null }
        $checks += Test-OpsFilesSame -Name "the merged volume's files are in Merged-Partition-$($source.partition_number)" -Before $sourceFiles -After $after
        $checks += Test-OpsWindowsVolume -Prefix 'the merged volume:' -Letter $letter -FileSystem 'NTFS'
    } else { $checks += New-OpsCheck -Name 'the merged volume is mounted' -Passed $false -Detail '' }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

Add-OpsRow 'ops.split' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (512 * $Script:OpsMiB) -Label 'OPSSPLIT'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 6 -MaxKilobytes 512
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    Invoke-OpsStep -Disk $disk -Step 'split' -Operation 'Split' -Select @{ partition_number = $volume.partition_number } -Choices @{ first_size_mib = 256 } -TimeoutSeconds 3600 | Out-Null
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $data = @($snapshot.data_partitions)
    $checks = @(New-OpsCheck -Name 'two data partitions' -Passed ($data.Count -eq 2) -Detail $data.Count)
    $checks += Test-OpsExtent -Name 'the first is 256 MiB where the volume was' -DiskNumber $disk -Offset $offset -SizeBytes (256 * $Script:OpsMiB)
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    if ($letter) {
        $checks += Test-OpsFilesSame -Name 'the files are still on the first' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'the first:' -Letter $letter -FileSystem 'NTFS'
    }
    if ($data.Count -eq 2) {
        $second = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$data[1].offset_bytes)
        $checks += New-OpsCheck -Name 'the second has a file system Windows mounts' -Passed ([bool]$second) -Detail $second
        if ($second) { $checks += Test-OpsWindowsVolume -Prefix 'the second:' -Letter $second -FileSystem 'NTFS' }
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

# --- Wipes ----------------------------------------------------------------------------------------

Add-OpsRow 'ops.wipe-partition' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $keep = New-CertVolume -DiskNumber $disk -SizeBytes (128 * $Script:OpsMiB) -Label 'OPSKEEP'
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSWIPE'
    Write-CertMarkerFile -Letter $volume.letter -Kilobytes 1024
    $kept = Write-RbFixtureTree -Root "$($keep.letter):\fixture" -FileCount 6
    $partition = Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number
    $before = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
    Invoke-OpsStep -Disk $disk -Step 'wipe' -Operation 'WipePartition' -Select @{ partition_number = $volume.partition_number } -TimeoutSeconds 3600 | Out-Null
    $after = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
    $checks = @(New-OpsCheck -Name 'the marker was on the partition before' -Passed ($before.marker_hits -gt 0) -Detail $before)
    $checks += New-OpsCheck -Name 'no copy of the marker is left anywhere in the partition' -Passed ($after.marker_hits -eq 0) -Detail $after
    $checks += Test-OpsFilesSame -Name 'the other volume is unchanged' -Before $kept -After (Get-RbFileManifest -Root "$($keep.letter):\fixture")
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ after = (Get-CertSnapshot -DiskNumber $disk) })
}

Add-OpsRow 'ops.wipe-free-space' {
    # cipher /w through the app: a deleted file's bytes must not survive, and live files must.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSFREE'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 6
    Write-CertMarkerFile -Letter $volume.letter -Kilobytes 2048
    Remove-Item -LiteralPath "$($volume.letter):\marker.bin"
    Write-VolumeCache -DriveLetter $volume.letter
    $partition = Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number
    $before = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
    Invoke-OpsStep -Disk $disk -Step 'wipe-free' -Operation 'WipeFreeSpace' -Select @{ partition_number = $volume.partition_number } -TimeoutSeconds 3600 | Out-Null
    $after = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
    $checks = @(New-OpsCheck -Name "the deleted file's bytes were on the disk before" -Passed ($before.marker_hits -gt 0) -Detail $before)
    $checks += New-OpsCheck -Name "none of the deleted file's bytes are left" -Passed ($after.marker_hits -eq 0) -Detail $after
    $checks += Test-OpsFilesSame -Name 'every live file is unchanged' -Before $files -After (Get-RbFileManifest -Root "$($volume.letter):\fixture")
    $checks += New-OpsCheck -Name "cipher's work folder is gone" -Passed (-not (Test-Path -LiteralPath "$($volume.letter):\EFSTMPWP")) -Detail ''
    $checks += Test-OpsWindowsVolume -Prefix 'after the wipe:' -Letter $volume.letter -FileSystem 'NTFS' -Label 'OPSFREE'
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.wipe-disk' {
    $disk = New-OpsDisk -Name 'disk' -SizeMB 512
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSWIPED'
    Write-CertMarkerFile -Letter $volume.letter -Kilobytes 1024
    $before = Find-RbMarkerAnywhere -DiskNumber $disk -Marker $Script:Marker
    Invoke-OpsStep -Disk $disk -Step 'wipe' -Operation 'WipeDisk' -TimeoutSeconds 3600 | Out-Null
    $after = Find-RbMarkerAnywhere -DiskNumber $disk -Marker $Script:Marker
    $checks = @(New-OpsCheck -Name 'the marker was on the disk before' -Passed ($before.marker_hits -gt 0) -Detail $before)
    $checks += New-OpsCheck -Name 'no copy of the marker is left on the disk' -Passed ($after.marker_hits -eq 0) -Detail $after
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks += New-OpsCheck -Name 'no data partition is left' -Passed (@($snapshot.data_partitions).Count -eq 0) -Detail @($snapshot.data_partitions).Count
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

# --- Rebuilds kept in a backup folder: current generated text, and the FAT/exFAT live-copy branch ---

function Invoke-OpsClusterOrSerial {
    param([ValidateSet('ChangeClusterSize', 'ChangeVolumeSerialNumber')] [string]$Operation, [string]$FileSystem)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $mib = if ($FileSystem -eq 'FAT32') { 640 } else { 384 }
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes ([uint64]$mib * $Script:OpsMiB) -FileSystem $FileSystem -Label 'OPSREBUILD' -ClusterBytes 4096
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $before = Get-RbVolumeFacts -Letter $volume.letter
    $choices = @{ backup_directory = (New-CertBackupFolder); confirmed = $true }
    # FAT32 at 640 MiB holds 81,920 clusters of 8 KiB; 16 KiB would be too few.
    $newCluster = if ($FileSystem -eq 'FAT32') { 8192 } else { 16384 }
    if ($Operation -eq 'ChangeClusterSize') { $choices += @{ file_system = $FileSystem; allocation_unit_bytes = $newCluster; label = 'OPSREBUILD' } }
    Invoke-OpsStep -Disk $disk -Step 'rebuild' -Operation $Operation -Select @{ partition_number = $volume.partition_number } -Choices $choices -TimeoutSeconds 3600 | Out-Null
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    $checks = @(New-OpsCheck -Name 'the volume is mounted afterwards' -Passed ([bool]$letter) -Detail $letter)
    if ($letter) {
        $after = Get-RbVolumeFacts -Letter $letter
        if ($Operation -eq 'ChangeClusterSize') {
            $checks += New-OpsCheck -Name "the cluster is now $newCluster bytes" -Passed ([uint64]$after.cluster_bytes -eq $newCluster) -Detail "$($before.cluster_bytes) -> $($after.cluster_bytes)"
        } else {
            $checks += New-OpsCheck -Name 'the serial number changed' -Passed ($after.serial_number -ne $before.serial_number) -Detail "$($before.serial_number) -> $($after.serial_number)"
        }
        $checks += Test-OpsFilesSame -Name 'every file came back, byte for byte' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'afterwards:' -Letter $letter -FileSystem $FileSystem -Label 'OPSREBUILD'
    }
    $leftovers = @(Get-ChildItem -LiteralPath $choices.backup_directory -Recurse -Force -ErrorAction SilentlyContinue)
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ backup_folder_entries_after = $leftovers.Count })
}

Add-OpsRow 'ops.cluster-ntfs' { Invoke-OpsClusterOrSerial -Operation 'ChangeClusterSize' -FileSystem 'NTFS' }
Add-OpsRow 'ops.cluster-fat32' { Invoke-OpsClusterOrSerial -Operation 'ChangeClusterSize' -FileSystem 'FAT32' }
Add-OpsRow 'ops.cluster-exfat' { Invoke-OpsClusterOrSerial -Operation 'ChangeClusterSize' -FileSystem 'exFAT' }
Add-OpsRow 'ops.serial-ntfs' { Invoke-OpsClusterOrSerial -Operation 'ChangeVolumeSerialNumber' -FileSystem 'NTFS' }
Add-OpsRow 'ops.serial-fat32' { Invoke-OpsClusterOrSerial -Operation 'ChangeVolumeSerialNumber' -FileSystem 'FAT32' }
Add-OpsRow 'ops.serial-exfat' { Invoke-OpsClusterOrSerial -Operation 'ChangeVolumeSerialNumber' -FileSystem 'exFAT' }

function Invoke-OpsMove {
    param([string]$FileSystem, [int]$CancelAfterSeconds = 0)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $base = Get-CertFirstFreeOffset -DiskNumber $disk
    $requested = $base + (64 * $Script:OpsMiB)
    $mib = Get-OpsVolumeMiB -FileSystem $FileSystem -MiB 192
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes ([uint64]$mib * $Script:OpsMiB) -FileSystem $FileSystem -Label 'OPSMOVE' -Offset ($base + (256 * $Script:OpsMiB))
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 24 -MaxKilobytes 4096
    $shadowsBefore = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue).Count
    $backup = New-CertBackupFolder
    $expect = if ($CancelAfterSeconds -gt 0) { 'cancelled' } else { 'success' }
    $step = Invoke-OpsStep -Disk $disk -Step 'move' -Operation 'MovePartition' -Select @{ partition_number = $volume.partition_number } -Choices @{
        target_offset_mib = ($requested / $Script:OpsMiB); backup_directory = $backup; confirmed = $true
    } -CancelAfterSeconds $CancelAfterSeconds -Expect $expect -TimeoutSeconds 3600
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks = @()
    if ($CancelAfterSeconds -eq 0) {
        $checks += Test-OpsExtent -Name 'it starts where asked, same size' -DiskNumber $disk -Offset $requested -SizeBytes ([uint64]$mib * $Script:OpsMiB)
        $letter = Wait-OpsLetter -DiskNumber $disk -Offset $requested
    } else {
        $last = @($step.execution.steps)[-1]
        $checks += New-OpsCheck -Name 'the batch says it was stopped' -Passed ([bool]$step.execution.cancelled) -Detail ([ordered]@{ removed = $last.stop_cleanup_removed; not_removed = $last.stop_cleanup_not_removed; may_still_run = $last.may_still_run_in_windows })
        $checks += New-OpsCheck -Name 'the cleanup could remove everything it made' -Passed (@($last.stop_cleanup_not_removed).Count -eq 0) -Detail @($last.stop_cleanup_not_removed)
        $letter = ''
        foreach ($at in @($offset, $requested)) { if (-not $letter) { $letter = Wait-OpsLetter -DiskNumber $disk -Offset $at } }
    }
    $checks += New-OpsCheck -Name 'no shadow copy is left behind' -Passed (@(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue).Count -le $shadowsBefore) -Detail "$shadowsBefore before"
    $mappings = @(Get-OpsShadowMappings)
    $checks += New-OpsCheck -Name 'no drive letter is left mapped to a shadow copy' -Passed ($mappings.Count -eq 0) -Detail ($mappings -join ' | ')
    if ($letter) {
        $checks += Test-OpsFilesSame -Name 'every file is there, byte for byte' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'afterwards:' -Letter $letter -FileSystem $FileSystem -Label 'OPSMOVE'
    } else {
        $checks += New-OpsCheck -Name 'the volume is mounted afterwards' -Passed $false -Detail ''
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot; backup_folder = $backup })
}

Add-OpsRow 'ops.move-ntfs' { Invoke-OpsMove -FileSystem 'NTFS' }
Add-OpsRow 'ops.move-fat32' { Invoke-OpsMove -FileSystem 'FAT32' }
Add-OpsRow 'ops.stop-cleanup-move' { Invoke-OpsMove -FileSystem 'NTFS' -CancelAfterSeconds 20 }

Add-OpsRow 'ops.stop-cleanup-wipe-free-space' {
    # Stop in the middle of cipher /w: the app must remove cipher's folder and say what it removed.
    $disk = New-OpsDisk -Name 'disk' -SizeMB 2048
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (1536 * $Script:OpsMiB) -Label 'OPSSTOP'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 6
    $step = Invoke-OpsStep -Disk $disk -Step 'wipe-free' -Operation 'WipeFreeSpace' -Select @{ partition_number = $volume.partition_number } -CancelAfterSeconds 15 -Expect 'cancelled' -TimeoutSeconds 3600
    Start-Sleep -Seconds 5
    $last = @($step.execution.steps)[-1]
    $checks = @(New-OpsCheck -Name 'the batch says it was stopped' -Passed ([bool]$step.execution.cancelled) -Detail ([ordered]@{ removed = $last.stop_cleanup_removed; not_removed = $last.stop_cleanup_not_removed; may_still_run = $last.may_still_run_in_windows }))
    $checks += New-OpsCheck -Name "cipher's work folder is gone" -Passed (-not (Test-Path -LiteralPath "$($volume.letter):\EFSTMPWP")) -Detail ''
    $checks += New-OpsCheck -Name 'no cipher process is left running' -Passed (@(Get-Process -Name cipher -ErrorAction SilentlyContinue).Count -eq 0) -Detail ''
    $checks += Test-OpsFilesSame -Name 'every live file is unchanged' -Before $files -After (Get-RbFileManifest -Root "$($volume.letter):\fixture")
    return Complete-OpsRow -Media $media -Checks $checks
}

function Invoke-OpsAllocate {
    # The donor after the target is backed up, shrunk or recreated, and restored; the target grows
    # in place. A FAT32 donor takes the live-copy branch (no shadow copy on FAT).
    param([string]$DonorFileSystem)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $target = New-CertVolume -DiskNumber $disk -SizeBytes (192 * $Script:OpsMiB) -Label 'OPSTARGET'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $target.partition_number).Offset
    $files = Write-RbFixtureTree -Root "$($target.letter):\fixture" -FileCount 8
    $donor = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:OpsMiB) -FileSystem $DonorFileSystem -Label 'OPSDONOR'
    $donorFiles = Write-RbFixtureTree -Root "$($donor.letter):\fixture" -FileCount 8
    Invoke-OpsStep -Disk $disk -Step 'allocate' -Operation 'AllocateFreeSpace' -Select @{ partition_number = $target.partition_number } -Choices @{
        allocate_mib = 64; backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 3600 | Out-Null
    $checks = @(Test-OpsExtent -Name 'the target grew by 64 MiB in place' -DiskNumber $disk -Offset $offset -SizeBytes (256 * $Script:OpsMiB))
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
    if ($letter) { $checks += Test-OpsFilesSame -Name "the target's files are unchanged" -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture") }
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $donorNow = @($snapshot.partitions | Where-Object { $_.volume -and $_.volume.label -eq 'OPSDONOR' })
    $checks += New-OpsCheck -Name 'the donor is 64 MiB smaller' -Passed ($donorNow.Count -eq 1 -and [uint64]$donorNow[0].size_bytes -eq 320 * $Script:OpsMiB) -Detail @($donorNow | ForEach-Object { $_.size_bytes })
    if ($donorNow.Count -eq 1 -and $donorNow[0].drive_letter) {
        $checks += Test-OpsFilesSame -Name "the donor's files came back, byte for byte" -Before $donorFiles -After (Get-RbFileManifest -Root "$($donorNow[0].drive_letter):\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'the donor:' -Letter "$($donorNow[0].drive_letter)" -FileSystem $DonorFileSystem -Label 'OPSDONOR'
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

Add-OpsRow 'ops.allocate-after' { Invoke-OpsAllocate -DonorFileSystem 'NTFS' }
Add-OpsRow 'ops.allocate-after-fat32' { Invoke-OpsAllocate -DonorFileSystem 'FAT32' }

function Invoke-OpsQuickPartition {
    # Quick Partition with a FAT file system: the certification rows ran it with NTFS only.
    param([string]$FileSystem, [string]$Prefix)
    $disk = New-OpsDisk -Name 'disk' -SizeMB 2048
    $media = Get-OpsMedia -DiskNumber $disk
    $old = New-CertVolume -DiskNumber $disk -SizeBytes (128 * $Script:OpsMiB) -Label 'SAKOLD'
    Write-CertMarkerFile -Letter $old.letter
    Invoke-OpsStep -Disk $disk -Step 'layout' -Operation 'QuickPartition' -Choices @{
        partition_count = 3; partition_scheme = 'GPT'; size_mode = 'equal'; file_system = $FileSystem
        allocation_unit_bytes = '4096'; label_prefix = $Prefix; full_format = $false
    } -TimeoutSeconds 3600 | Out-Null
    $run = [pscustomobject]@{ media = $media; after = (Get-CertSnapshot -DiskNumber $disk) }
    $data = @($run.after.data_partitions)
    $checks = @(New-OpsCheck -Name 'three data partitions' -Passed ($data.Count -eq 3) -Detail $data.Count)
    foreach ($partition in @($run.after.partitions | Where-Object { $_.volume })) {
        $checks += Test-OpsWindowsVolume -Prefix "$($partition.drive_letter):" -Letter "$($partition.drive_letter)" -FileSystem $FileSystem -ClusterBytes 4096
    }
    $checks += New-OpsCheck -Name 'the old volume is gone' -Passed (@($run.after.partitions | Where-Object { $_.volume -and $_.volume.label -ceq 'SAKOLD' }).Count -eq 0) -Detail ''
    return Complete-OpsRow -Media $run.media -Checks $checks -Extra ([ordered]@{ after = $run.after })
}

Add-OpsRow 'ops.quick-partition-exfat' { Invoke-OpsQuickPartition -FileSystem 'exFAT' -Prefix 'OPSQX' }
Add-OpsRow 'ops.quick-partition-fat32' { Invoke-OpsQuickPartition -FileSystem 'FAT32' -Prefix 'OPSQF' }

function Invoke-OpsPrimaryLogical {
    param([ValidateSet('logical', 'primary')] [string]$To)
    $disk = New-OpsDisk -Name 'disk' -Style 'MBR'
    $media = Get-OpsMedia -DiskNumber $disk
    if ($To -eq 'primary') {
        # A logical partition, made by diskpart the way a person's other tools make one.
        $letter = Get-CertFreeLetter
        $script = Join-Path $WorkRoot "$($Script:RowId)-logical.diskpart.txt"
        @("select disk $disk", 'create partition extended size=768', 'create partition logical size=384', 'format fs=ntfs quick label=OPSLOGIC', "assign letter=$letter") | Set-Content -LiteralPath $script -Encoding ASCII
        $output = & diskpart.exe /s $script 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "diskpart could not make the logical fixture: $output" }
        Start-Sleep -Seconds 3
        Update-HostStorageCache
        $partition = Get-Partition -DiskNumber $disk | Where-Object { $_.DriveLetter -eq $letter }
        $volume = [pscustomobject]@{ partition_number = [int]$partition.PartitionNumber; letter = $letter }
    } else {
        $volume = New-CertVolume -DiskNumber $disk -SizeBytes (384 * $Script:OpsMiB) -Label 'OPSLOGIC'
    }
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $before = Get-CertSnapshot -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -Step "to-$To" -Operation 'ConvertPrimaryLogical' -Select @{ partition_number = $volume.partition_number } -Choices @{
        target_layout = $To; backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 3600 | Out-Null
    $after = Get-CertSnapshot -DiskNumber $disk
    $data = @($after.table.partitions | Where-Object { $_.layout -ne 'extended' })
    $checks = @(New-OpsCheck -Name "the raw table holds it as $To" -Passed ($data.Count -eq 1 -and $data[0].layout -eq $To) -Detail @($data | ForEach-Object { "$($_.layout) $($_.type) $($_.size_bytes)" }))
    $letter = if ($data.Count -eq 1) { Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$data[0].offset_bytes) } else { '' }
    if ($letter) {
        $checks += Test-OpsFilesSame -Name 'every file came back, byte for byte' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'afterwards:' -Letter $letter -FileSystem 'NTFS' -Label 'OPSLOGIC'
    } else { $checks += New-OpsCheck -Name 'the volume is mounted afterwards' -Passed $false -Detail '' }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ before = $before; after = $after })
}

Add-OpsRow 'ops.to-logical' { Invoke-OpsPrimaryLogical -To 'logical' }
Add-OpsRow 'ops.to-primary' { Invoke-OpsPrimaryLogical -To 'primary' }

# --- Images, recovery, inventory -------------------------------------------------------------------

Add-OpsRow 'ops.image-locked-volumes' {
    # Create Image of an online disk whose volumes are mounted: the LockedVolumes mode.
    $disk = New-OpsDisk -Name 'disk' -SizeMB 512
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSIMAGE'
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    $image = Join-Path $WorkRoot "$($Script:RowId)-image.img"
    if (Test-Path -LiteralPath $image) { Remove-Item -LiteralPath $image -Force }
    $step = Invoke-OpsStep -Disk $disk -Step 'image' -Operation 'CreateImage' -Choices @{ image_path = $image } -TimeoutSeconds 3600
    $checks = @(New-OpsCheck -Name 'the image file exists and is the disk size' -Passed ((Test-Path -LiteralPath $image) -and [uint64](Get-Item -LiteralPath $image).Length -eq [uint64](Get-Disk -Number $disk).Size) -Detail $(if (Test-Path -LiteralPath $image) { (Get-Item -LiteralPath $image).Length } else { 0 }))
    $mode = "$(@($step.queued_operations)[0].payload.source_capture_mode)"
    $checks += New-OpsCheck -Name 'the step captured in LockedVolumes mode' -Passed ($mode -ceq 'LockedVolumes') -Detail $mode
    if (Test-Path -LiteralPath $image) {
        # The image's own table and file system, read by mounting a copy read-only as a VHD.
        $copy = Join-Path $WorkRoot "$($Script:RowId)-image-check.vhd"
        $vhd = Join-Path $WorkRoot "$($Script:RowId)-image-check.diskpart.txt"
        @("create vdisk file=`"$copy`" maximum=$([int]((Get-Item -LiteralPath $image).Length / $Script:OpsMiB)) type=fixed") | Set-Content -LiteralPath $vhd -Encoding ASCII
        & diskpart.exe /s $vhd | Out-Null
        $stream = [IO.File]::Open($copy, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
        try { $source = [IO.File]::OpenRead($image); try { $source.CopyTo($stream) } finally { $source.Dispose() } } finally { $stream.Dispose() }
        # The copy carries the source's disk id, and Windows takes a disk offline when two share
        # one: the source goes first.
        Dismount-DiskImage -ImagePath (Get-OpsVhdFor -DiskNumber $disk).ImagePath | Out-Null
        Mount-DiskImage -ImagePath $copy -Access ReadOnly | Out-Null
        $Script:RowVhds.Add($copy)
        Start-Sleep -Seconds 3
        $number = [int](Get-DiskImage -ImagePath $copy).Number
        if ((Get-Disk -Number $number).IsOffline) { Set-Disk -Number $number -IsOffline $false; Start-Sleep -Seconds 2 }
        $part = @(Get-Partition -DiskNumber $number | Where-Object { $_.Type -eq 'Basic' })[0]
        if (-not $part.DriveLetter -or [int][char]$part.DriveLetter -eq 0) { Add-PartitionAccessPath -DiskNumber $number -PartitionNumber $part.PartitionNumber -AssignDriveLetter -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
        $part = Get-Partition -DiskNumber $number -PartitionNumber $part.PartitionNumber
        $checks += Test-OpsFilesSame -Name "the image holds the volume's files, byte for byte" -Before $files -After (Get-RbFileManifest -Root "$($part.DriveLetter):\fixture")
        Remove-Item -LiteralPath $image -Force
    }
    return Complete-OpsRow -Media $media -Checks $checks
}

function Invoke-OpsRecovery {
    param([ValidateSet('GPT', 'MBR')] [string]$Style, [ValidateSet('Quick', 'Full')] [string]$ScanMode)
    $disk = New-OpsDisk -Name 'disk' -Style $Style
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSREC' -Offset (128 * $Script:OpsMiB)
    $files = Write-RbFixtureTree -Root "$($volume.letter):\fixture" -FileCount 8
    Write-VolumeCache -DriveLetter $volume.letter
    $old = @((Get-CertSnapshot -DiskNumber $disk).data_partitions)[0]
    Remove-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number -Confirm:$false
    Start-Sleep -Seconds 2
    $scan = Invoke-OpsStep -Disk $disk -Step 'scan' -Operation 'PartitionRecoveryScan' -Choices @{ scan_mode = $ScanMode } -TimeoutSeconds 3600
    $said = (@($scan.execution.steps) | ForEach-Object { "$($_.stdout)" }) -join "`n"
    # As a person types it from the scan: a GPT type in braces, an MBR type in hex.
    $typeId = if ($Style -eq 'GPT') { "{$($old.type)}" } else { "0x$($old.type)" }
    Invoke-OpsStep -Disk $disk -Step 'restore' -Operation 'RestoreRecoveredPartition' -Choices @{ offset_bytes = "$($old.offset_bytes)"; size_bytes = "$($old.size_bytes)"; type_id = $typeId } | Out-Null
    $checks = @(New-OpsCheck -Name 'the scan named the lost partition' -Passed ($said -match "$($old.offset_bytes)") -Detail "offset $($old.offset_bytes)")
    $checks += Test-OpsExtent -Name 'the raw table holds it again, same place and size' -DiskNumber $disk -Offset ([uint64]$old.offset_bytes) -SizeBytes ([uint64]$old.size_bytes)
    $partition = Get-CertDataPartition -DiskNumber $disk
    if (-not ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter))) {
        Add-PartitionAccessPath -DiskNumber $disk -PartitionNumber $partition.PartitionNumber -AssignDriveLetter
        Start-Sleep -Seconds 2
    }
    $letter = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$old.offset_bytes)
    if ($letter) { $checks += Test-OpsFilesSame -Name 'every file is back, byte for byte' -Before $files -After (Get-RbFileManifest -Root "${letter}:\fixture") }
    else { $checks += New-OpsCheck -Name 'the restored volume mounts' -Passed $false -Detail '' }
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.recovery-full-scan-gpt' { Invoke-OpsRecovery -Style 'GPT' -ScanMode 'Full' }
Add-OpsRow 'ops.recovery-restore-mbr' { Invoke-OpsRecovery -Style 'MBR' -ScanMode 'Quick' }

Add-OpsRow 'ops.inventory-elevated' {
    # Scan Disks with administrator rights: the inventory read in the helper, against Get-Disk.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSINV' | Out-Null
    $step = Invoke-OpsStep -Disk $disk -Step 'scan' -Operation 'ElevatedInventoryScan'
    $listed = @($step.elevated_inventory.disks)
    $windows = @(Get-Disk)
    $checks = @(New-OpsCheck -Name 'the helper scan lists every disk Get-Disk lists' -Passed ($listed.Count -eq $windows.Count) -Detail "$($listed.Count) of $($windows.Count)")
    $mine = @($listed | Where-Object { [int]$_.disk_number -eq $disk })
    $checks += New-OpsCheck -Name "it lists this row's disk with its partition" -Passed ($mine.Count -eq 1 -and [int]$mine[0].partition_count -ge 1) -Detail $mine
    $checks += New-OpsCheck -Name 'the scan ran with no warning' -Passed (@($step.elevated_inventory.warnings).Count -eq 0) -Detail @($step.elevated_inventory.warnings)
    return Complete-OpsRow -Media $media -Checks $checks
}

# --- APFS volume operations ------------------------------------------------------------------------

Add-OpsRow 'ops.apfs-label-and-snapshots' {
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    $images = @()
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'APFS'; allocation_unit_bytes = 0; label = 'OPSAPFS'; full_format = $false } | Out-Null
    Invoke-OpsStep -Disk $disk -Step 'label' -Operation 'ApfsChangeVolumeLabel' -Select $select -Choices @{ label = 'OPSRENAMED'; administrator_scan = $true } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after the label:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'APFS' -Name 'label'
    $checks = @($read.checks); $images += $read.image
    $probe = Get-Content -LiteralPath (Join-Path $OutputRoot "images\$($read.image.name).probe.json") -Raw -ErrorAction SilentlyContinue
    $checks += New-OpsCheck -Name "the probe reads the volume name OPSRENAMED" -Passed ("$probe" -match 'OPSRENAMED') -Detail ''
    foreach ($snap in @(@{ step = 'snap-create'; op = 'ApfsSnapshotCreate' }, @{ step = 'snap-revert'; op = 'ApfsSnapshotRevert' }, @{ step = 'snap-delete'; op = 'ApfsSnapshotDelete' })) {
        $wantedName = 'opsproof-snap'
        Invoke-OpsStep -Disk $disk -Step $snap.step -Operation $snap.op -Select $select -Choices @{ apfs_snapshot_name = $wantedName; administrator_scan = $true } | Out-Null
        $read = Test-OpsNonNativePartition -Prefix "after $($snap.step):" -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'APFS' -Name $snap.step
        $checks += $read.checks; $images += $read.image
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($images | Where-Object { $_ }) })
}

# --- VM rig rows ------------------------------------------------------------------------------------

Add-OpsRow 'ops.dynamic-to-basic' {
    # Windows will not make a virtual disk file dynamic, so this runs on a throwaway VDI the VM rig
    # attaches (-DataDiskBytes). The fixture is external.dynamic-to-basic's.
    $found = Get-CertReadPin -Role 'any'
    $disk = $found.number
    Initialize-Disk -Number $disk -PartitionStyle MBR
    $media = Get-CertMedia -DiskNumber $disk -Class $found.class -Kind $found.kind
    $letter = Get-CertFreeLetter
    $script = Join-Path $WorkRoot 'ops-dynamic.diskpart.txt'
    @("select disk $disk", 'convert dynamic', 'create volume simple size=384', 'format fs=ntfs quick label=OPSDYN', "assign letter=$letter") |
        Set-Content -LiteralPath $script -Encoding ASCII
    $output = & diskpart.exe /s $script 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "diskpart could not make the dynamic fixture: $output" }
    Start-Sleep -Seconds 3
    Update-HostStorageCache
    $files = Write-RbFixtureTree -Root "${letter}:\fixture" -FileCount 8
    $before = Get-CertSnapshot -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -GivenPin (Get-CertPin -DiskNumber $disk) -Step 'convert' -Operation 'ConvertDynamicDiskToBasic' -Choices @{
        backup_directory = (New-CertBackupFolder); confirmed = $true
    } -TimeoutSeconds 3600 | Out-Null
    $after = Get-CertSnapshot -DiskNumber $disk
    $checks = @(New-OpsCheck -Name 'the raw table holds no dynamic (LDM) entry' -Passed ([int]$after.dynamic_entry_count -eq 0) -Detail "$($before.dynamic_entry_count) -> $($after.dynamic_entry_count)")
    $checks += New-OpsCheck -Name 'Get-Disk lists it again, as basic' -Passed ([bool](Get-RbDiskFacts -DiskNumber $disk).listed_by_get_disk) -Detail ''
    $data = @($after.data_partitions)
    $mounted = if ($data.Count -eq 1) { Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$data[0].offset_bytes) } else { '' }
    if ($mounted) {
        $checks += Test-OpsFilesSame -Name 'every file came back, byte for byte' -Before $files -After (Get-RbFileManifest -Root "${mounted}:\fixture")
        $checks += Test-OpsWindowsVolume -Prefix 'afterwards:' -Letter $mounted -FileSystem 'NTFS' -Label 'OPSDYN'
    } else { $checks += New-OpsCheck -Name 'one basic data volume, mounted' -Passed $false -Detail $data.Count }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ before = $before; after = $after })
}

Add-OpsRow 'ops.restore-point' {
    # The assistant's diagnostics.create_restore_point: RestorePointManager in the app's own,
    # elevated process. It changes the running Windows, so it runs only in the VM.
    Assert-CertBootRowInVm
    $boot = [int](@(Get-Disk | Where-Object IsBoot)[0].Number)
    $media = Get-CertMedia -DiskNumber $boot -Class 'virtual_machine' -Kind 'the running Windows, in a VM'
    # Fixture, with Windows' own tools: System Restore on for C:, and no 24-hour throttle, so a
    # restore point the app asks for is one Windows makes now.
    Enable-ComputerRestore -Drive "$env:SystemDrive\"
    New-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRestore' -Name 'SystemRestorePointCreationFrequency' -PropertyType DWord -Value 0 -Force | Out-Null
    $before = @(Get-ComputerRestorePoint -ErrorAction SilentlyContinue)
    $description = "SAK opsproof $($Script:RunId.Substring(0, 8))"
    $started = Get-Date
    Invoke-OpsStep -Disk $boot -GivenPin (Get-CertPin -DiskNumber $boot) -Step 'restore-point' -Operation 'RestorePoint' -Choices @{ description = $description } -Elevated | Out-Null
    $after = @(Get-ComputerRestorePoint -ErrorAction SilentlyContinue)
    $made = @($after | Where-Object { $_.Description -eq $description })
    $checks = @(New-OpsCheck -Name "Windows lists one restore point named '$description'" -Passed ($made.Count -eq 1) -Detail $made.Count)
    $checks += New-OpsCheck -Name 'it is one more than before' -Passed ($after.Count -eq $before.Count + 1) -Detail "$($before.Count) -> $($after.Count)"
    if ($made.Count -eq 1) {
        $when = [Management.ManagementDateTimeConverter]::ToDateTime($made[0].CreationTime)
        $checks += New-OpsCheck -Name 'it was made during the step' -Passed ($when -ge $started.AddMinutes(-1)) -Detail $when.ToString('o')
    }
    return Complete-OpsRow -Media $media -Checks $checks
}

function Get-OpsEspLetter {
    # The running Windows' EFI system partition, given a letter with Windows' own mountvol.
    param([string]$Letter = 'Y')
    & mountvol.exe "${Letter}:" /s 2>&1 | Out-Null
    if (-not (Test-Path -LiteralPath "${Letter}:\EFI")) { throw "The EFI system partition did not mount at ${Letter}:" }
    return $Letter
}

function Get-OpsBootFileFacts {
    param([string[]]$Paths)
    return @($Paths | ForEach-Object {
            $present = Test-Path -LiteralPath $_
            $signature = if ($present) { Get-AuthenticodeSignature -LiteralPath $_ } else { $null }
            [ordered]@{ path = $_; present = $present; signed_by = $(if ($signature) { "$($signature.SignerCertificate.Subject)" } else { '' }); status = $(if ($signature) { "$($signature.Status)" } else { '' }) }
        })
}

Add-OpsRow 'ops.repair-boot-running-uefi' {
    # Repair Boot of the Windows this VM runs. The break renames the boot manager on the ESP (the
    # BCD store is held open by Windows); the firmware could then find nothing to start. The host
    # restarts the VM afterwards and the row passes only if it signs in again.
    Assert-CertBootRowInVm
    if ("$env:firmware_type" -ne 'UEFI') { throw 'This row needs a UEFI guest' }
    $boot = [int](@(Get-Disk | Where-Object IsBoot)[0].Number)
    $media = Get-CertMedia -DiskNumber $boot -Class 'virtual_machine' -Kind 'the running Windows, in a VM'
    $esp = Get-OpsEspLetter
    $files = @("${esp}:\EFI\Microsoft\Boot\bootmgfw.efi", "${esp}:\EFI\Boot\bootx64.efi")
    $before = Get-OpsBootFileFacts -Paths $files
    foreach ($file in $files) { if (Test-Path -LiteralPath $file) { Rename-Item -LiteralPath $file -NewName ((Split-Path -Leaf $file) + '.opsproof-broken') } }
    $broken = Get-OpsBootFileFacts -Paths $files
    & mountvol.exe "${esp}:" /d 2>&1 | Out-Null
    Invoke-OpsStep -Disk $boot -GivenPin (Get-CertPin -DiskNumber $boot) -Step 'repair' -Operation 'RepairBoot' -Choices @{ boot_mode = 'UEFI'; windows_path = "$env:SystemRoot" } -TimeoutSeconds 900 | Out-Null
    $esp = Get-OpsEspLetter
    $after = Get-OpsBootFileFacts -Paths $files
    $bcd = & bcdedit.exe /enum '{bootmgr}' 2>&1 | Out-String
    & mountvol.exe "${esp}:" /d 2>&1 | Out-Null
    $checks = @(New-OpsCheck -Name 'the break removed both boot files' -Passed (@($broken | Where-Object { $_.present }).Count -eq 0) -Detail $broken)
    $checks += New-OpsCheck -Name 'the repair put both back, signed by Microsoft and valid' -Passed (@($after | Where-Object { $_.present -and $_.status -eq 'Valid' -and $_.signed_by -match 'Microsoft' }).Count -eq 2) -Detail $after
    $checks += New-OpsCheck -Name 'the boot manager entry points at bootmgfw.efi' -Passed ($bcd -match '(?i)\EFI\Microsoft\Boot\bootmgfw\.efi') -Detail ($bcd.Trim() -split "`r?`n" | Select-Object -First 8)
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ files_before = $before })
}

Add-OpsRow 'ops.migrate-image-route' {
    # Migrate OS onto a disk too small for the Windows partition and large enough for what it
    # holds: the planner's Image route (DISM). The host then starts the target alone.
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    Set-Content -LiteralPath 'C:\SAKCert\source-marker.txt' -Value $Script:RunId -Encoding ASCII
    $step = Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'migrate' -Operation 'MigrateOs' -Choices @{ verify_mode = 'Sample verification'; allow_no_snapshot = $false } -TimeoutSeconds 10800
    $plan = @($step.queued_operations)[0].payload.migration_plan
    $checks = @(New-OpsCheck -Name 'the plan took the Image route' -Passed ("$($plan.route)" -eq 'Image') -Detail "$($plan.route)")
    $after = Get-CertSnapshot -DiskNumber $found.number
    $checks += New-OpsCheck -Name 'the target holds a partition table with a Windows-sized data partition' -Passed (@($after.data_partitions).Count -ge 1) -Detail @($after.data_partitions | ForEach-Object { "$($_.type) $($_.size_bytes)" })
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ source_marker = $Script:RunId; after = $after })
}

function Get-OpsBiosSystemPartition {
    # The partition holding the BIOS boot files on an MBR Windows disk, found by its files and not
    # by its flag: a row clears the flag on purpose.
    param([int]$DiskNumber)
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber | Sort-Object Offset)) {
        $added = $false
        if (-not ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter))) {
            Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -AssignDriveLetter -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
            $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber
            $added = $true
        }
        $letter = "$($partition.DriveLetter)"
        $hasLetter = $letter -and [char]::IsLetter([char]$letter)
        if ($hasLetter -and ((Test-Path -LiteralPath "${letter}:\Boot\BCD") -or @(Get-ChildItem -LiteralPath "${letter}:\" -Filter 'bootmgr*' -Force -ErrorAction SilentlyContinue).Count -gt 0)) {
            return [pscustomobject]@{ number = [int]$partition.PartitionNumber; letter = $letter; offset = [uint64]$partition.Offset; letter_added = $added }
        }
        if ($added -and $hasLetter) { Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "${letter}:\" -ErrorAction SilentlyContinue }
    }
    throw "Disk $DiskNumber has no partition with BIOS boot files"
}

function Get-OpsBiosBootFacts {
    param([int]$DiskNumber, [object]$System)
    $entry = Get-OpsMbrEntry -DiskNumber $DiskNumber -OffsetBytes $System.offset
    $sector = Read-RbBytes -DiskNumber $DiskNumber -Offset 0 -Length 512
    $active = 0
    for ($slot = 0; $slot -lt 4; $slot++) { if ($sector[446 + $slot * 16] -eq 0x80) { $active++ } }
    return [ordered]@{
        system_partition_boot_flag = "$($entry.boot_flag)"
        active_partitions = $active
        bootmgr_present = (Test-Path -LiteralPath "$($System.letter):\bootmgr")
        bcd_present = (Test-Path -LiteralPath "$($System.letter):\Boot\BCD")
    }
}

function Remove-OpsAddedLetter {
    param([int]$DiskNumber, [object]$System)
    if ($System.letter_added) { Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $System.number -AccessPath "$($System.letter):\" -ErrorAction SilentlyContinue }
}

Add-OpsRow 'ops.repair-boot-attached-bios-no-active' {
    # Repair Boot of a BIOS Windows on an attached disk whose system partition is not active: the
    # branch where the script must find the system partition another way. Driven a phase at a time
    # by the ops-vm group, which starts the disk alone before, after the break and after the repair.
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    $state = Join-Path $OutputRoot "$($Script:RowId).phase.json"
    $kept = if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    $system = Get-OpsBiosSystemPartition -DiskNumber $found.number
    if ($Phase -eq 'break') {
        $before = Get-OpsBiosBootFacts -DiskNumber $found.number -System $system
        Set-Partition -DiskNumber $found.number -PartitionNumber $system.number -IsActive $false
        if (Test-Path -LiteralPath "$($system.letter):\bootmgr") { Rename-Item -LiteralPath "$($system.letter):\bootmgr" -NewName 'bootmgr.opsproof-broken' -Force }
        $broken = Get-OpsBiosBootFacts -DiskNumber $found.number -System $system
        Remove-OpsAddedLetter -DiskNumber $found.number -System $system
        $kept | Add-Member -NotePropertyName facts_before -NotePropertyValue $before -Force
        $kept | Add-Member -NotePropertyName facts_broken -NotePropertyValue $broken -Force
        ($kept | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $state
        $checks = @(New-OpsCheck -Name 'break: no partition is active and bootmgr is gone' -Passed ($broken.active_partitions -eq 0 -and -not $broken.bootmgr_present) -Detail $broken)
        return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ phase = 'break'; kept = $kept })
    }
    if ($Phase -ne 'repair') { throw "This row runs one phase at a time, break or repair, not '$Phase'" }
    Remove-OpsAddedLetter -DiskNumber $found.number -System $system
    Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'repair' -Operation 'RepairBoot' -Choices @{ boot_mode = 'BIOS' } -TimeoutSeconds 900 | Out-Null
    $system = Get-OpsBiosSystemPartition -DiskNumber $found.number
    $after = Get-OpsBiosBootFacts -DiskNumber $found.number -System $system
    Remove-OpsAddedLetter -DiskNumber $found.number -System $system
    $checks = @(New-OpsCheck -Name 'break (earlier phase): no partition was active and bootmgr was gone' -Passed ([int]$kept.facts_broken.active_partitions -eq 0 -and -not $kept.facts_broken.bootmgr_present) -Detail $kept.facts_broken)
    $checks += New-OpsCheck -Name 'repair: the system partition is active again' -Passed ($after.system_partition_boot_flag -eq '80' -and $after.active_partitions -eq 1) -Detail $after
    $checks += New-OpsCheck -Name 'repair: bootmgr and the BCD store are there' -Passed ($after.bootmgr_present -and $after.bcd_present) -Detail $after
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ phase = 'repair'; kept = $kept })
}

Add-OpsRow 'ops.repair-boot-running-bios' {
    # Repair Boot of the BIOS Windows this VM runs: the ops-vm group runs it inside a BIOS VM made
    # from the boot-bios install, then restarts that VM, which must sign in again.
    Assert-CertBootRowInVm
    if ("$env:firmware_type" -ne 'Legacy') { throw 'This row needs a BIOS guest' }
    $boot = [int](@(Get-Disk | Where-Object IsBoot)[0].Number)
    $media = Get-CertMedia -DiskNumber $boot -Class 'virtual_machine' -Kind 'the running Windows, in a BIOS VM'
    $system = Get-OpsBiosSystemPartition -DiskNumber $boot
    $before = Get-OpsBiosBootFacts -DiskNumber $boot -System $system
    if (Test-Path -LiteralPath "$($system.letter):\bootmgr") { Rename-Item -LiteralPath "$($system.letter):\bootmgr" -NewName 'bootmgr.opsproof-broken' -Force }
    $broken = Get-OpsBiosBootFacts -DiskNumber $boot -System $system
    Remove-OpsAddedLetter -DiskNumber $boot -System $system
    Invoke-OpsStep -Disk $boot -GivenPin (Get-CertPin -DiskNumber $boot) -Step 'repair' -Operation 'RepairBoot' -Choices @{ boot_mode = 'BIOS'; windows_path = "$env:SystemRoot" } -TimeoutSeconds 900 | Out-Null
    $system = Get-OpsBiosSystemPartition -DiskNumber $boot
    $after = Get-OpsBiosBootFacts -DiskNumber $boot -System $system
    Remove-OpsAddedLetter -DiskNumber $boot -System $system
    $checks = @(New-OpsCheck -Name 'the break removed bootmgr' -Passed (-not $broken.bootmgr_present) -Detail $broken)
    $checks += New-OpsCheck -Name 'the repair put bootmgr and the BCD store back on the active partition' -Passed ($after.bootmgr_present -and $after.bcd_present -and $after.system_partition_boot_flag -eq '80') -Detail $after
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ before = $before })
}

Add-OpsRow 'ops.migrate-mbr-bios' {
    # Migrate OS from a BIOS Windows on MBR to an attached disk; the host then starts the target
    # alone, with BIOS firmware.
    Assert-CertBootRowInVm
    if ("$env:firmware_type" -ne 'Legacy') { throw 'This row needs a BIOS guest' }
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    Set-Content -LiteralPath 'C:\SAKCert\source-marker.txt' -Value $Script:RunId -Encoding ASCII
    $step = Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'migrate' -Operation 'MigrateOs' -Choices @{ verify_mode = 'Sample verification'; allow_no_snapshot = $false } -TimeoutSeconds 10800
    $after = Get-CertSnapshot -DiskNumber $found.number
    $checks = @(New-OpsCheck -Name 'the target is MBR, as the source' -Passed ($after.table.style -eq 'MBR') -Detail $after.table.style)
    $checks += New-OpsCheck -Name 'the target has one active partition' -Passed (@($after.table.partitions | Where-Object { $_.active }).Count -eq 1) -Detail @($after.table.partitions | ForEach-Object { "$($_.type) active=$($_.active)" })
    $plan = @($step.queued_operations)[0].payload.migration_plan
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ source_marker = $Script:RunId; route = "$($plan.route)"; after = $after })
}
