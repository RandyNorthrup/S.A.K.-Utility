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

# The certification runner has loaded partition_manager_readback.ps1 first: its readers and its
# shared settle (Wait-RbVolumeScanSettle) are used here and in the checks.
if (-not (Get-Command Wait-RbVolumeScanSettle -ErrorAction SilentlyContinue)) {
    throw 'Load partition_manager_readback.ps1 before partition_manager_operation_proof_rows.ps1'
}
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

$Script:RowDevice = $null

function Get-OpsDestructiveDisk {
    <#
    .SYNOPSIS
        The one physical drive the coordinator gave the suite for what a virtual disk file cannot
        do (a full format: 2026-11-12). machine.json names it by model, serial and size, beside the
        spare it already names; the row may use it only until Clear-OpsDestructiveDisk.
    #>
    if ($Script:InVirtualBox) { throw 'A VirtualBox guest has no destructive drive' }
    $file = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($WorkRoot))) 'machine.json'
    $machine = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if (-not $machine.PSObject.Properties['destructive_serial']) { throw "$file names no destructive drive" }
    $Script:RowDevice = [pscustomobject]@{ number = -1; model = "$($machine.destructive_model)"; serial = "$($machine.destructive_serial)"; bytes = "$($machine.destructive_bytes)" }
    $found = @(Get-Disk | Where-Object { "$($_.FriendlyName)" -ceq $Script:RowDevice.model -and "$($_.SerialNumber)".Trim().TrimEnd('.') -ceq $Script:RowDevice.serial -and "$([uint64]$_.Size)" -eq $Script:RowDevice.bytes })
    if ($found.Count -ne 1) { $Script:RowDevice = $null; throw "The destructive drive $file names is here $($found.Count) times, not once" }
    $Script:RowDevice.number = [int]$found[0].Number
    Assert-OpsDevicePin -DiskNumber $Script:RowDevice.number
    return $Script:RowDevice.number
}

function Assert-OpsDevicePin {
    # The destructive drive, pinned again by model, serial and size, and never the disk Windows runs from.
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    if ($null -eq $Script:RowDevice -or $Script:RowDevice.number -ne $DiskNumber) { throw "Disk $DiskNumber is not the destructive drive this row was given" }
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ("$($disk.FriendlyName)" -cne $Script:RowDevice.model -or "$($disk.SerialNumber)".Trim().TrimEnd('.') -cne $Script:RowDevice.serial -or "$([uint64]$disk.Size)" -ne $Script:RowDevice.bytes) {
        throw "Disk $DiskNumber is no longer the destructive drive: $($disk.FriendlyName) $($disk.Size)"
    }
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.IsReadOnly) { throw "Disk $DiskNumber is a boot, system or read-only disk" }
}

function Clear-OpsDestructiveDisk { $Script:RowDevice = $null }

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
        if ($null -ne $Script:RowDevice -and $Script:RowDevice.number -ge 0) {
            $facts = Get-RbDiskFacts -DiskNumber $Script:RowDevice.number
            if ("$($facts.unique_id)" -ceq "$($wanted.unique_id)" -and "$($facts.size_bytes)" -eq "$($wanted.size_bytes)") { Assert-OpsDevicePin -DiskNumber $Script:RowDevice.number; continue }
        }
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
    # -NoSettle: the volsnap drills time the removal themselves (Drill D none, E 10 s, F only the
    # product's own settle), so they take the disk down at once.
    param([switch]$NoSettle)
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    # A disk removed within seconds of an online scan (chkdsk /scan here, Repair-Volume -Scan in the
    # app) can leave volsnap's timer queued in a freed device: testpc2 bugchecked so in Drill D and
    # again in the final run at 11:59 (2026-11-12). Drill E's 10 s settle held for 60 minutes under
    # load; the harness waits 15 s, as the product now does, before any volume goes down, through
    # the readback's shared settle.
    if (-not $NoSettle) { Wait-RbVolumeScanSettle }
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
    } elseif ($null -ne $Script:RowDevice -and $Script:RowDevice.number -eq $Disk) {
        Assert-OpsDevicePin -DiskNumber $Disk
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
Add-OpsRow 'ops.create-refs-assistant' {
    # Windows 11 Pro cannot format ReFS outside a Dev Drive. Since opsfix (F4) the assistant's Create
    # is refused before anything is written, and says why; b28 made a partition and left it RAW.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $before = Get-CertSnapshot -DiskNumber $disk
    Invoke-OpsStep -Disk $disk -Step 'create' -Operation 'Create' -Route 'assistant' -ReadBackOnFailure -Expect 'refused' -RefusalContains 'cannot format ReFS' -Choices @{
        size_mib = 256; free_before_mib = 0; file_system = 'ReFS'; allocation_unit_bytes = 4096
        partition_type = $Script:GptBasicData; label = 'OPSREFS'; full_format = $false
    } | Out-Null
    $after = Get-CertSnapshot -DiskNumber $disk
    $checks = @(New-OpsCheck -Name 'the raw table holds no data partition' -Passed (@($after.data_partitions).Count -eq 0) -Detail @($after.data_partitions).Count)
    $checks += New-OpsCheck -Name 'the partition table did not change' -Passed ("$($before.raw.gpt_entries_hex)" -eq "$($after.raw.gpt_entries_hex)") -Detail ''
    return Complete-OpsRow -Media $media -Checks $checks
}
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
    if ($FileSystem -in 'FAT32', 'exFAT') {
        # Windows cannot shrink FAT32 or exFAT; since opsfix f5e55dfd7 (F6) the app says so at Add to
        # Queue, in its validator, script and dialog, and changes nothing.
        Invoke-OpsStep -Disk $disk -Step 'shrink' -Operation 'Resize' -Select @{ partition_number = $volume.partition_number } -Choices @{ target_size_mib = 256 } -Expect 'refused' -RefusalContains 'Windows cannot shrink FAT32 or exFAT partitions' -ReadBackOnFailure | Out-Null
        $checks = @(Test-OpsExtent -Name 'the raw table still holds it at 512 MiB' -DiskNumber $disk -Offset $offset -SizeBytes (512 * $Script:OpsMiB))
        $letter = Wait-OpsLetter -DiskNumber $disk -Offset $offset
        if ($letter) {
            $checks += Test-OpsFilesSame -Name 'every file is still there, byte for byte' -Before $before -After (Get-RbFileManifest -Root "${letter}:\fixture")
            $checks += Test-OpsWindowsVolume -Prefix 'the untouched volume:' -Letter $letter -FileSystem $FileSystem -Label 'OPSSHRINK'
        } else { $checks += New-OpsCheck -Name 'the volume is still mounted' -Passed $false -Detail '' }
        return Complete-OpsRow -Media $media -Checks $checks
    }
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

function Get-OpsTableEntry {
    # The table entry at an offset, straight from the disk's sectors, in one shape for both styles:
    # on GPT its type, attribute bits, id and name; on MBR its type byte.
    param([int]$DiskNumber, [uint64]$Offset, [ValidateSet('GPT', 'MBR')] [string]$Style)
    if ($Style -eq 'MBR') {
        $mbr = Get-OpsMbrEntry -DiskNumber $DiskNumber -OffsetBytes $Offset
        if ($null -eq $mbr) { return $null }
        return [pscustomobject]@{ type = "$($mbr.type)"; partition_id = ''; attributes = ''; name = '' }
    }
    $gpt = Get-OpsGptEntry -DiskNumber $DiskNumber -OffsetBytes $Offset
    if ($null -eq $gpt) { return $null }
    return [pscustomobject]@{ type = "$($gpt.type)"; partition_id = "$($gpt.partition_id)"; attributes = ('0x{0:X16}' -f [uint64]$gpt.attributes); name = "$($gpt.name)" }
}

function Get-OpsPartitionLetters {
    # Every drive-letter path Windows has for the partition at an offset.
    param([int]$DiskNumber, [uint64]$Offset)
    Update-HostStorageCache
    $partition = Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Where-Object { [uint64]$_.Offset -eq $Offset } | Select-Object -First 1
    if (-not $partition) { return @('(Windows lists no partition there)') }
    $letters = @()
    if ($partition.DriveLetter -and [char]::IsLetter([char]$partition.DriveLetter)) { $letters += "$($partition.DriveLetter):\" }
    $letters += @($partition.AccessPaths | Where-Object { $_ -match '^[A-Za-z]:\\' })
    return @($letters | Select-Object -Unique)
}

# The console watcher: a separate process, so a step that blocks the runner does not stop it. Every
# second until the stop file appears: a screenshot of the whole console, the drive letters Windows
# shows, and each visible titled top-level window with its process.
$Script:OpsWatchScript = @'
param([string]$Dir, [string]$StopFile)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -TypeDefinition 'using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class OpsWindows {
    private delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc proc, IntPtr data);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int most);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    public static List<string> Visible() {
        List<string> found = new List<string>();
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            if (!IsWindowVisible(window)) { return true; }
            StringBuilder text = new StringBuilder(256);
            GetWindowText(window, text, 256);
            if (text.Length == 0) { return true; }
            uint process;
            GetWindowThreadProcessId(window, out process);
            found.Add(process.ToString() + "|" + text.ToString());
            return true;
        }, IntPtr.Zero);
        return found;
    }
}'
# Each sample is one JSON line, written as it is taken, so a watcher stopped hard still leaves
# every sample it took. Frames are JPEG at no more than 1280 pixels wide: a full PNG of
# testpc2's console was 6.4 MB.
$jpeg = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' } | Select-Object -First 1
$quality = New-Object Drawing.Imaging.EncoderParameters 1
$quality.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), ([long]70)
$lines = Join-Path $Dir 'samples.jsonl'
$frame = 0
while (-not (Test-Path -LiteralPath $StopFile) -and $frame -lt 900) {
    $started = Get-Date
    $frame++
    $shot = ''
    try {
        $bounds = [Windows.Forms.SystemInformation]::VirtualScreen
        $bitmap = New-Object Drawing.Bitmap $bounds.Width, $bounds.Height
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
            $scale = [Math]::Min(1.0, 1280.0 / $bounds.Width)
            $small = New-Object Drawing.Bitmap ([int]($bounds.Width * $scale)), ([int]($bounds.Height * $scale))
            $drawing = [Drawing.Graphics]::FromImage($small)
            try {
                $drawing.DrawImage($bitmap, 0, 0, $small.Width, $small.Height)
                $shot = 'frame-{0:D3}.jpg' -f $frame
                $small.Save((Join-Path $Dir $shot), $jpeg, $quality)
            } finally { $drawing.Dispose(); $small.Dispose() }
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    } catch { $shot = "error: $($_.Exception.Message)" }
    $letters = @([IO.DriveInfo]::GetDrives() | ForEach-Object { "$($_.Name)" })
    $names = @{}
    foreach ($p in Get-Process) { $names[[int]$p.Id] = "$($p.ProcessName)" }
    $windows = @([OpsWindows]::Visible() | ForEach-Object {
        $parts = "$_" -split '\|', 2
        $owner = $names[[int]$parts[0]]
        "$(if ($owner) { $owner } else { $parts[0] })|$($parts[1])"
    } | Select-Object -Unique)
    $sample = [ordered]@{ at = $started.ToString('HH:mm:ss.fff'); shot = $shot; letters = $letters; windows = $windows }
    Add-Content -LiteralPath $lines -Value (ConvertTo-Json -InputObject $sample -Depth 3 -Compress)
    if ($frame -eq 1) { Set-Content -LiteralPath (Join-Path $Dir 'ready') -Value 'ready' }
    $rest = 1000 - ((Get-Date) - $started).TotalMilliseconds
    if ($rest -gt 0) { Start-Sleep -Milliseconds ([int]$rest) }
}
'@

function Start-OpsConsoleWatch {
    # Starts the watcher and waits for its first sample, which is what the rest are compared with.
    param([string]$Name)
    $dir = Join-Path $Script:RowDir "watch-$Name"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = Join-Path $dir 'watch.ps1'
    Set-Content -LiteralPath $file -Value $Script:OpsWatchScript -Encoding ASCII
    $stop = Join-Path $dir 'stop'
    $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    # What the watcher says goes beside its frames: a watcher that cannot start leaves its reason.
    $process = Start-Process -FilePath $shell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$file`"", '-Dir', "`"$dir`"", '-StopFile', "`"$stop`"") -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir 'watch-out.txt') -RedirectStandardError (Join-Path $dir 'watch-err.txt')
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Test-Path -LiteralPath (Join-Path $dir 'ready')) -and -not $process.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    return [pscustomobject]@{ dir = $dir; stop = $stop; process = $process }
}

function Stop-OpsConsoleWatch {
    # What changed on the console while the step ran, against the watcher's first sample.
    param([object]$Watch)
    Set-Content -LiteralPath $Watch.stop -Value 'stop'
    if (-not $Watch.process.WaitForExit(60000)) { try { $Watch.process.Kill() } catch { Write-Host "Could not stop the console watcher: $($_.Exception.Message)" } }
    $path = Join-Path $Watch.dir 'samples.jsonl'
    # @() around the whole if: an if statement hands on an empty array as nothing at all.
    $samples = @(if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllLines($path) | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json } })
    $said = @(Get-ChildItem -LiteralPath $Watch.dir -Filter 'watch-*.txt' -ErrorAction SilentlyContinue | ForEach-Object { [IO.File]::ReadAllLines($_.FullName) } | Where-Object { $_ } | Select-Object -Last 20)
    $summary = [ordered]@{ folder = Split-Path -Leaf $Watch.dir; samples = $samples.Count; watcher_said = $said; screenshots = 0; screenshot_errors = @(); first = ''; last = ''; letters_that_appeared = @(); windows_that_appeared = @() }
    if ($samples.Count -eq 0) { return [pscustomobject]$summary }
    $first = $samples[0]
    $summary.screenshots = @($samples | Where-Object { "$($_.shot)" -like 'frame-*' }).Count
    $summary.screenshot_errors = @($samples | Where-Object { "$($_.shot)" -like 'error:*' } | ForEach-Object { "$($_.shot)" } | Select-Object -Unique)
    $summary.first = "$($first.at)"
    $summary.last = "$($samples[-1].at)"
    $appeared = [System.Collections.Generic.List[string]]::new()
    foreach ($sample in $samples) {
        foreach ($letter in @($sample.letters)) { if ($letter -notin @($first.letters) -and -not ($appeared -like "letter $letter *")) { $appeared.Add("letter $letter first seen $($sample.at)") } }
    }
    $summary.letters_that_appeared = @($appeared)
    $windows = [System.Collections.Generic.List[string]]::new()
    foreach ($sample in $samples) {
        foreach ($window in @($sample.windows)) { if ($window -notin @($first.windows) -and -not ($windows -like "$([WildcardPattern]::Escape($window)) (first seen *")) { $windows.Add("$window (first seen $($sample.at))") } }
    }
    $summary.windows_that_appeared = @($windows)
    return [pscustomobject]$summary
}

function Invoke-OpsExtResizeKept {
    <#
    .SYNOPSIS
        ext4 grown and shrunk through the app, reading what a resize must keep: the partition's
        type, on GPT also its attribute bits, id and name; no drive letter; a file system that
        fills the new size and checks whole.
    .DESCRIPTION
        Windows resizes only a data partition, so a build that gives the partition a Windows type
        for the resize and puts its own back could let Windows mount it on the way, and Explorer
        would then offer to format it. Every second of each resize the console is photographed and
        the drive letters and windows Windows shows are listed (watch-grow, watch-shrink).
    #>
    param([ValidateSet('GPT', 'MBR')] [string]$Style)
    $disk = New-OpsDisk -Name 'disk' -Style $Style
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    $wantedType = if ($Style -eq 'GPT') { '0fc63daf-8483-4772-8e79-3d69d8477de4' } else { '83' }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'ext4'; allocation_unit_bytes = 0; label = 'OPSKEPT'; full_format = $false } | Out-Null
    $formatted = Get-OpsTableEntry -DiskNumber $disk -Offset $part.offset -Style $Style
    $checks = @(New-OpsCheck -Name "after format: the table gives it type $wantedType" -Passed ("$($formatted.type)" -eq $wantedType) -Detail $formatted)
    $letters = @(Get-OpsPartitionLetters -DiskNumber $disk -Offset $part.offset)
    $checks += New-OpsCheck -Name 'after format: the partition has no drive letter' -Passed ($letters.Count -eq 0) -Detail $letters
    $read = Test-OpsNonNativePartition -Prefix 'after format:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'ext4' -Label 'OPSKEPT' -Name 'format'
    $checks += $read.checks
    $images = @($read.image)
    $watched = [ordered]@{}
    foreach ($resize in @(@{ name = 'grow'; mib = 256 }, @{ name = 'shrink'; mib = 160 })) {
        $name = $resize.name
        $watch = Start-OpsConsoleWatch -Name $name
        try {
            Invoke-OpsStep -Disk $disk -Step $name -Operation 'Resize' -Select $select -Choices @{ target_size_mib = $resize.mib; administrator_scan = $true } -ReadBackOnFailure | Out-Null
        } finally { $seen = Stop-OpsConsoleWatch -Watch $watch }
        $watched[$name] = $seen
        $checks += New-OpsCheck -Name "${name}: the console was photographed every second of it" -Passed ($seen.screenshots -ge 2 -and @($seen.screenshot_errors).Count -eq 0) -Detail $seen
        $checks += New-OpsCheck -Name "${name}: no drive letter appeared while it ran" -Passed (@($seen.letters_that_appeared).Count -eq 0) -Detail @($seen.letters_that_appeared)
        $checks += New-OpsCheck -Name "${name}: no Explorer window or prompt opened while it ran" -Passed (@($seen.windows_that_appeared | Where-Object { $_ -like 'explorer|*' }).Count -eq 0) -Detail @($seen.windows_that_appeared)
        $checks += Test-OpsExtent -Name "${name}: the raw table holds it at $($resize.mib) MiB" -DiskNumber $disk -Offset $part.offset -SizeBytes ([uint64]$resize.mib * $Script:OpsMiB)
        $entry = Get-OpsTableEntry -DiskNumber $disk -Offset $part.offset -Style $Style
        $checks += New-OpsCheck -Name "${name}: the type is still $wantedType" -Passed ("$($entry.type)" -eq $wantedType) -Detail $entry
        if ($Style -eq 'GPT') {
            $kept = ($null -ne $entry -and $entry.attributes -eq $formatted.attributes -and $entry.partition_id -eq $formatted.partition_id -and $entry.name -ceq $formatted.name)
            $checks += New-OpsCheck -Name "${name}: the attribute bits, id and name are as Format left them" -Passed $kept -Detail "$($formatted.attributes) $($formatted.partition_id) '$($formatted.name)' / $($entry.attributes) $($entry.partition_id) '$($entry.name)'"
        }
        $letters = @(Get-OpsPartitionLetters -DiskNumber $disk -Offset $part.offset)
        $checks += New-OpsCheck -Name "${name}: the partition has no drive letter" -Passed ($letters.Count -eq 0) -Detail $letters
        $read = Test-OpsNonNativePartition -Prefix "after ${name}:" -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'ext4' -Label 'OPSKEPT' -Name $name
        $checks += $read.checks
        $images += $read.image
    }
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($images | Where-Object { $_ }); formatted_entry = $formatted; console = $watched })
}

Add-OpsRow 'ops.resize-ext4-gpt-kept' { Invoke-OpsExtResizeKept -Style 'GPT' }
Add-OpsRow 'ops.resize-ext4-mbr-watched' { Invoke-OpsExtResizeKept -Style 'MBR' }

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
    # The app's two steps (opsfix, 2026-11-12): Resize grows the APFS partition in place into the
    # free space after it, and "Resize container to fill partition" then grows the container.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'APFS'; allocation_unit_bytes = 0; label = 'OPSAPFS'; full_format = $false } | Out-Null
    $typeBefore = "$((Get-Partition -DiskNumber $disk -PartitionNumber $part.partition_number).GptType)"
    $grow = Invoke-OpsStep -Disk $disk -Step 'grow' -Operation 'Resize' -Select $select -Choices @{ target_size_mib = 256; administrator_scan = $true } -ReadBackOnFailure
    $checks = @(Test-OpsExtent -Name 'the partition grew in place to 256 MiB' -DiskNumber $disk -Offset $part.offset -SizeBytes (256 * $Script:OpsMiB))
    $checks += New-OpsCheck -Name 'the grow kept the APFS partition type' -Passed ("$((Get-Partition -DiskNumber $disk -PartitionNumber $part.partition_number).GptType)" -eq $typeBefore) -Detail $typeBefore
    # A grow that failed left no report to read (the failure is already a check of its own).
    $preview = if ($null -ne $grow) { "$(@($grow.queued_operations)[0].summary) $(@($grow.execution.steps | ForEach-Object { $_.stdout }) -join ' ')" } else { '' }
    $checks += New-OpsCheck -Name 'the grow says to resize the container next' -Passed ($preview -match 'Resize container to fill partition') -Detail ($preview.Substring(0, [Math]::Min(300, $preview.Length)))
    $before = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'before'
    $beforeBlocks = [BitConverter]::ToUInt64((Read-OpsImageBytes -Path $before.path -Offset 0 -Length 4096), 40)
    Invoke-OpsStep -Disk $disk -Step 'resize' -Operation 'ApfsResizeContainer' -Select $select -Choices @{ administrator_scan = $true } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after resize:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem 'APFS' -Name 'after'
    $block = Read-OpsImageBytes -Path $read.image.path -Offset 0 -Length 4096
    $blockSize = [uint64][BitConverter]::ToUInt32($block, 36)
    $afterBlocks = [BitConverter]::ToUInt64($block, 40)
    $checks += @($read.checks)
    $checks += New-OpsCheck -Name 'the container now fills the 256 MiB partition' -Passed ($afterBlocks * $blockSize -gt 250 * $Script:OpsMiB -and $afterBlocks * $blockSize -le 256 * $Script:OpsMiB) -Detail "$beforeBlocks -> $afterBlocks blocks of $blockSize"
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ images = @($before, $read.image) })
}

function Invoke-OpsResizeRefused {
    # Resizes S.A.K. refuses (opsfix, 2026-11-12): an APFS shrink, and any HFS+ resize. The step
    # expects the refusal, and the row reads back that nothing changed: the extent and the whole
    # partition's bytes.
    param([string]$FileSystem, [int]$FromMiB, [int]$ToMiB, [string]$Says)
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB $FromMiB
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = $FileSystem; allocation_unit_bytes = 0; label = 'OPSKEEP'; full_format = $false } | Out-Null
    $before = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'before'
    Invoke-OpsStep -Disk $disk -Step 'resize' -Operation 'Resize' -Select $select -Choices @{ target_size_mib = $ToMiB; administrator_scan = $true } -Expect 'refused' -RefusalContains $Says -ReadBackOnFailure | Out-Null
    $checks = @(Test-OpsExtent -Name "the partition is still $FromMiB MiB" -DiskNumber $disk -Offset $part.offset -SizeBytes ([uint64]$FromMiB * $Script:OpsMiB))
    $after = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'after'
    $checks += New-OpsCheck -Name 'not one byte of the partition changed' -Passed ($before.sha256 -eq $after.sha256) -Detail "$($before.sha256) / $($after.sha256)"
    Remove-Item -LiteralPath $before.path, $after.path -Force -ErrorAction SilentlyContinue
    return Complete-OpsRow -Media $media -Checks $checks
}

Add-OpsRow 'ops.resize-apfs-shrink-refused' { Invoke-OpsResizeRefused -FileSystem 'APFS' -FromMiB 256 -ToMiB 128 -Says 'S.A.K. cannot shrink APFS partitions' }
Add-OpsRow 'ops.resize-hfsplus-refused' { Invoke-OpsResizeRefused -FileSystem 'HFS+' -FromMiB 128 -ToMiB 256 -Says 'S.A.K. cannot resize HFS+ partitions' }

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
    $complained = (@($step.execution.steps) | ForEach-Object { "$($_.stderr)" }) -join ' '
    # The preview line alone is not a result: on b28 a FAT32 or exFAT check reported success with
    # stderr "Repair-Volume : The specified file system is not supported" and checked nothing (F16).
    $checks = @(New-OpsCheck -Name 'the check ran and said the file system is clean' -Passed ($said -match 'NoErrorsFound|found no problems') -Detail ($said.Substring(0, [Math]::Min(400, $said.Length))))
    $checks += New-OpsCheck -Name 'the check wrote nothing to stderr' -Passed (-not $complained.Trim()) -Detail ($complained.Substring(0, [Math]::Min(300, $complained.Length)))
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
    # The panel offers APFS no read-only tool: its Check Non-Windows File System shows the
    # container's metadata at once (showImmediateMetadataCheck), which the step client does not
    # drive, and the tool route it would take says "No approved read-only checker is mapped for
    # 'apfs'" (final run, 2026-11-12). So APFS gets the repair only.
    $readOnly = 'not offered for APFS: the panel shows the container metadata instead'
    if ($FileSystem -ne 'APFS') {
        $check = Invoke-OpsStep -Disk $disk -Step 'check' -Operation 'CheckNonNativeFileSystem' -Select $select -Choices @{ administrator_scan = $true }
        $checks += New-OpsCheck -Name 'the read-only check ran its tool and it passed' -Passed ([bool]$check.file_system_check.success) -Detail $check.file_system_check
        $readOnly = 'run'
    }
    Invoke-OpsStep -Disk $disk -Step 'repair' -Operation 'RepairFileSystem' -Select $select -Choices @{ administrator_scan = $true } | Out-Null
    $read = Test-OpsNonNativePartition -Prefix 'after repair:' -DiskNumber $disk -PartitionNumber $part.partition_number -FileSystem $FileSystem -Name 'repaired'
    $checks += $read.checks
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ read_only_check = $readOnly; images = @($read.image | Where-Object { $_ }) })
}

Add-OpsRow 'ops.repair-ext2' { Invoke-OpsRepairNonNative -FileSystem 'ext2' }
Add-OpsRow 'ops.repair-ext3' { Invoke-OpsRepairNonNative -FileSystem 'ext3' }
Add-OpsRow 'ops.repair-ext4' { Invoke-OpsRepairNonNative -FileSystem 'ext4' }
Add-OpsRow 'ops.repair-hfsplus' { Invoke-OpsRepairNonNative -FileSystem 'HFS+' }
Add-OpsRow 'ops.repair-hfsx' { Invoke-OpsRepairNonNative -FileSystem 'HFSX' }
Add-OpsRow 'ops.repair-apfs' { Invoke-OpsRepairNonNative -FileSystem 'APFS' }

Add-OpsRow 'ops.check-ext4-larger-than-partition' {
    # F11 (coordinator, 2026-11-11): e2fsck -n exits 0 on an ext file system longer than its
    # partition and says so only in words. The app's read-only check must not call that healthy,
    # and must not write. The fixture is what F7 left on b28: a 128 MiB ext4 written by the app,
    # its partition cut to 64 MiB, here with Windows' own Resize-Partition.
    $disk = New-OpsDisk -Name 'disk'
    $media = Get-OpsMedia -DiskNumber $disk
    $part = New-OpsRawPartition -DiskNumber $disk -SizeMB 128
    $select = @{ partition_number = $part.partition_number }
    Invoke-OpsStep -Disk $disk -Step 'format' -Operation 'Format' -Select $select -Choices @{ file_system = 'ext4'; allocation_unit_bytes = 0; label = 'OPSCUT'; full_format = $false } | Out-Null
    # Windows resizes only data partitions, and the app's ext4 Format gives it the Linux type: the
    # fixture cuts it as basic data and puts the type back.
    $linuxType = (Get-Partition -DiskNumber $disk -PartitionNumber $part.partition_number).GptType
    Set-Partition -DiskNumber $disk -PartitionNumber $part.partition_number -GptType "{$Script:OpsGptBasicData}" -ErrorAction Stop
    Resize-Partition -DiskNumber $disk -PartitionNumber $part.partition_number -Size (64 * $Script:OpsMiB) -ErrorAction Stop
    Set-Partition -DiskNumber $disk -PartitionNumber $part.partition_number -GptType $linuxType -ErrorAction Stop
    $before = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'before'
    $check = Invoke-OpsStep -Disk $disk -Step 'check' -Operation 'CheckNonNativeFileSystem' -Select $select -Choices @{ administrator_scan = $true } -Expect 'failed' -RefusalContains 'physical size of the device' -ReadBackOnFailure
    $said = if ($null -ne $check) { $check.file_system_check } else { $null }
    $checks = @(New-OpsCheck -Name 'the read-only check did not call it healthy' -Passed ($null -ne $said -and -not [bool]$said.success) -Detail $said)
    $after = Copy-OpsPartitionImage -DiskNumber $disk -PartitionNumber $part.partition_number -Name 'after'
    $checks += New-OpsCheck -Name 'the read-only check wrote nothing' -Passed ($before.sha256 -eq $after.sha256) -Detail "$($before.sha256) / $($after.sha256)"
    Remove-Item -LiteralPath $before.path, $after.path -Force -ErrorAction SilentlyContinue
    return Complete-OpsRow -Media $media -Checks $checks
}

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
    $complained = (@($step.execution.steps) | ForEach-Object { "$($_.stderr)" }) -join ' '
    # Both volumes must have been checked: the NTFS one by Repair-Volume, and the FAT32 one, which
    # Repair-Volume refuses (F16), by whatever the app uses for FAT. Text alone proved nothing.
    $verdicts = ([regex]::Matches($said, 'NoErrorsFound|found no problems')).Count
    $checks = @(New-OpsCheck -Name 'the scan said each of the two volumes is clean' -Passed ($verdicts -ge 2) -Detail "$verdicts verdicts: $($said.Substring(0, [Math]::Min(300, $said.Length)))")
    $checks += New-OpsCheck -Name 'the scan wrote nothing to stderr' -Passed (-not $complained.Trim()) -Detail ($complained.Substring(0, [Math]::Min(300, $complained.Length)))
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
    # Two partitions: on MBR, Windows' hidden and no-default-letter attributes are the whole disk's,
    # and Set-Partition -IsHidden on one hid both (testpc2, merged build 9170b149, F30). Only the
    # one the person chose may go, by its type byte; the other keeps its letter.
    $disk = New-OpsDisk -Name 'disk' -Style 'MBR'
    $media = Get-OpsMedia -DiskNumber $disk
    $volume = New-CertVolume -DiskNumber $disk -SizeBytes (256 * $Script:OpsMiB) -Label 'OPSHIDE'
    $other = New-CertVolume -DiskNumber $disk -SizeBytes (128 * $Script:OpsMiB) -Label 'OPSSTAY'
    $offset = [uint64](Get-Partition -DiskNumber $disk -PartitionNumber $volume.partition_number).Offset
    $select = @{ partition_number = $volume.partition_number }
    $before = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    Invoke-OpsStep -Disk $disk -Step 'hide' -Operation 'SetPartitionHidden' -Select $select -Choices @{ hidden = $true } | Out-Null
    $hidden = Get-OpsMbrEntry -DiskNumber $disk -OffsetBytes $offset
    $checks = @(New-OpsCheck -Name 'hide: the MBR type byte became hidden NTFS (17)' -Passed ($hidden.type -eq '17') -Detail "$($before.type) -> $($hidden.type)")
    Update-HostStorageCache
    $stay = Get-Partition -DiskNumber $disk -PartitionNumber $other.partition_number
    $checks += New-OpsCheck -Name 'hide: the other partition on the disk is not hidden and keeps its letter' -Passed (-not $stay.IsHidden -and "$($stay.DriveLetter)" -eq "$($other.letter)") -Detail "hidden $($stay.IsHidden), letter '$($stay.DriveLetter)', was '$($other.letter)'"
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
    # A wipe is a full format, which Windows refuses on a thin-provisioned disk, and a virtual
    # disk file is one: the app says so and changes nothing (seen on testpc2, 2026-11-12). The
    # wipe itself needs a real disk: external.wipe-partition-physical.
    Invoke-OpsStep -Disk $disk -Step 'wipe' -Operation 'WipePartition' -Select @{ partition_number = $volume.partition_number } -TimeoutSeconds 3600 -Expect 'refused' -RefusalContains 'thin-provisioned' -ReadBackOnFailure | Out-Null
    $after = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
    $checks = @(New-OpsCheck -Name 'the marker was on the partition before' -Passed ($before.marker_hits -gt 0) -Detail $before)
    $checks += New-OpsCheck -Name 'the refused wipe left the marker where it was' -Passed ($after.marker_hits -eq $before.marker_hits) -Detail $after
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
    $tableBefore = Get-CertSnapshot -DiskNumber $disk
    # Refused on a virtual disk file, as Wipe Partition is (above); the wipe itself is
    # external.wipe-disk-physical.
    Invoke-OpsStep -Disk $disk -Step 'wipe' -Operation 'WipeDisk' -TimeoutSeconds 3600 -Expect 'refused' -RefusalContains 'thin-provisioned' -ReadBackOnFailure | Out-Null
    $after = Find-RbMarkerAnywhere -DiskNumber $disk -Marker $Script:Marker
    $checks = @(New-OpsCheck -Name 'the marker was on the disk before' -Passed ($before.marker_hits -gt 0) -Detail $before)
    $checks += New-OpsCheck -Name 'the refused wipe left the marker where it was' -Passed ($after.marker_hits -eq $before.marker_hits) -Detail $after
    $snapshot = Get-CertSnapshot -DiskNumber $disk
    $checks += New-OpsCheck -Name 'the partition table did not change' -Passed ("$($tableBefore.raw.gpt_entries_hex)" -eq "$($snapshot.raw.gpt_entries_hex)") -Detail @($snapshot.data_partitions).Count
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ snapshot = $snapshot })
}

Add-OpsRow 'ops.wipe-partition-physical' {
    # Wipe Partition is a full format, which Windows allows only on a disk that is not
    # thin-provisioned. The coordinator gave the suite the destructive-test drive machine.json
    # names (2026-11-12): a 2 GiB partition in the free space near its start, a marker written
    # there, the app's Wipe Partition, and the whole partition read back from the device for the
    # marker. The drive reports pending sectors: an I/O error on it is the drive's, not the app's.
    $disk = Get-OpsDestructiveDisk
    $made = $null
    try {
        $media = Get-CertMedia -DiskNumber $disk -Class 'physical_disk' -Kind 'destructive-test drive'
        $existing = @(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Sort-Object Offset)
        $end = [uint64]0
        foreach ($p in $existing) { $end = [Math]::Max($end, [uint64]$p.Offset + [uint64]$p.Size) }
        $start = [uint64]([Math]::Ceiling($end / $Script:OpsMiB) * $Script:OpsMiB)
        if ($start -lt $Script:OpsMiB) { $start = $Script:OpsMiB }
        if ($start -gt 1GB) { throw "No free space near the start of the destructive drive (first free byte $start); nothing was made" }
        Assert-OpsDevicePin -DiskNumber $disk
        $made = New-Partition -DiskNumber $disk -Offset $start -Size 2GB -AssignDriveLetter -ErrorAction Stop
        Start-Sleep -Seconds 3
        Format-Volume -Partition $made -FileSystem NTFS -NewFileSystemLabel 'OPSWIPE' -Confirm:$false -ErrorAction Stop | Out-Null
        $partition = Get-Partition -DiskNumber $disk -PartitionNumber $made.PartitionNumber
        Write-CertMarkerFile -Letter $partition.DriveLetter -Kilobytes 1024
        $before = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
        Invoke-OpsStep -Disk $disk -Step 'wipe' -Operation 'WipePartition' -Select @{ partition_number = $made.PartitionNumber } -TimeoutSeconds 3600 -ReadBackOnFailure | Out-Null
        Assert-OpsDevicePin -DiskNumber $disk
        $after = Find-OpsMarkerInRange -DiskNumber $disk -Offset ([uint64]$partition.Offset) -Length ([uint64]$partition.Size)
        $checks = @(New-OpsCheck -Name 'the marker was on the partition before' -Passed ($before.marker_hits -gt 0) -Detail $before)
        $checks += New-OpsCheck -Name 'no copy of the marker is left anywhere in the partition' -Passed ($after.marker_hits -eq 0) -Detail $after
        $checks += Test-OpsExtent -Name 'the partition is where it was, the same size' -DiskNumber $disk -Offset ([uint64]$partition.Offset) -SizeBytes ([uint64]$partition.Size)
        $kept = @(Get-Partition -DiskNumber $disk | Where-Object { $_.PartitionNumber -ne $made.PartitionNumber } | Sort-Object Offset | ForEach-Object { "$($_.Offset):$($_.Size)" }) -join ' '
        $was = @($existing | ForEach-Object { "$($_.Offset):$($_.Size)" }) -join ' '
        $checks += New-OpsCheck -Name "the drive's other partitions are unchanged" -Passed ($kept -eq $was) -Detail "$was / $kept"
        return Complete-OpsRow -Media $media -Checks $checks
    } finally {
        if ($null -ne $made) {
            Assert-OpsDevicePin -DiskNumber $disk
            Remove-Partition -DiskNumber $disk -PartitionNumber $made.PartitionNumber -Confirm:$false -ErrorAction SilentlyContinue
        }
        Clear-OpsDestructiveDisk
    }
}

function Invoke-OpsVolumeChurnDrill {
    <#
    .SYNOPSIS
        The volsnap drill (coordinator, 2026-11-12): does S.A.K.'s own work on a real USB disk that
        stays plugged in, with Driver Verifier off, reach the kernel-timer fault of the bugchecks?
        Crash 3's freed object was volsnap's volume device, with a timer queued in it, freed when
        its volume was removed seconds after an online scan.
    .DESCRIPTION
        Each round, through the app: Create NTFS, write files, Check File System (Repair-Volume
        -Scan), then -PauseSeconds, then Hide (the first volume removal), Unhide, Format ext4,
        Format NTFS, write files, and with -WithMove the VSS-backed Move (a snapshot, then the volume
        removed and made again), then Delete. Every step's time goes to the runner's log and the
        row's evidence; a step that fails is noted and the round goes on. The drive is the
        destructive one machine.json names, pinned before every step and before the cleanup.
    #>
    param([int]$Minutes, [int]$PauseSeconds, [switch]$WithMove, [switch]$GuardRemovals)
    $drillStarted = Get-Date
    $disk = Get-OpsDestructiveDisk
    $log = [System.Collections.Generic.List[string]]::new()
    $failed = [System.Collections.Generic.List[string]]::new()
    $stopped = ''
    $media = $null
    $kept = ''
    $round = 0
    $skipped = 0
    $missedCreates = 0
    $write = { param([string]$Text) $line = "$((Get-Date).ToString('HH:mm:ss')) $Text"; $log.Add($line); Write-Host "  drill $line" }
    $run = {
        param([string]$StepName, [string]$OperationName, [hashtable]$Where, [hashtable]$With)
        & $write "$StepName start"
        $report = Invoke-OpsStep -Disk $disk -Step $StepName -Operation $OperationName -Select $Where -Choices $With -TimeoutSeconds 3600 -ReadBackOnFailure
        if ($null -eq $report) { $failed.Add($StepName); & $write "$StepName FAILED" } else { & $write "$StepName done" }
    }
    # Drill C's candidate defence: before a volume is removed, wait until no snapshot is being
    # made (no VSS 'snapshot creation initiated' in the last 15 s, and the volume's shadow copies
    # unchanged over 2 s), then lock and dismount the volume, which flushes it through volsnap.
    $guard = {
        param([hashtable]$Where, [string]$Before)
        $part = Get-Partition -DiskNumber $disk -PartitionNumber $Where.partition_number -ErrorAction SilentlyContinue
        $volumePath = @($part.AccessPaths | Where-Object { $_ -like '\\?\Volume*' }) | Select-Object -First 1
        if (-not $volumePath) { & $write "guard before ${Before}: no volume to guard"; return }
        $waitedFrom = Get-Date
        while ($true) {
            $recent = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VSS'; Id = 8231; StartTime = (Get-Date).AddSeconds(-15) } -ErrorAction SilentlyContinue)
            $first = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue | Where-Object { $_.VolumeName -eq $volumePath }).Count
            Start-Sleep -Seconds 2
            $second = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue | Where-Object { $_.VolumeName -eq $volumePath }).Count
            if ($recent.Count -eq 0 -and $first -eq $second) { break }
            if (((Get-Date) - $waitedFrom).TotalMinutes -gt 10) { & $write "guard before ${Before}: a snapshot still looked in flight after 10 min"; break }
        }
        $result = Invoke-OpsLockAndDismount -VolumePath $volumePath
        & $write ("guard before {0}: waited {1:N1} s, {2}" -f $Before, ((Get-Date) - $waitedFrom).TotalSeconds, $result)
    }
    try {
        $media = Get-CertMedia -DiskNumber $disk -Class 'physical_disk' -Kind 'destructive-test drive'
        $kept = @(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Sort-Object Offset | ForEach-Object { "$($_.Offset):$($_.Size)" }) -join ' '
        if (@(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Where-Object { $_.Type -eq 'Basic' }).Count -gt 0) { throw 'The destructive drive already holds a data partition; the drill makes and deletes its own and touches nothing else' }
        $base = Get-CertFirstFreeOffset -DiskNumber $disk
        $deadline = (Get-Date).AddMinutes($Minutes)
        while ((Get-Date) -lt $deadline) {
            $round++
            & $write "round $round"
            & $run "create-$round" 'Create' @{} @{ size_mib = 2048; free_before_mib = 0; file_system = 'NTFS'; allocation_unit_bytes = 4096; partition_type = $Script:GptBasicData; label = 'OPSCHURN'; full_format = $false }
            $part = @(Get-Partition -DiskNumber $disk | Where-Object { $_.Type -eq 'Basic' })
            # A Create that made nothing (on a busy PC the app's disk scan can stall before it
            # queues anything) costs its round, not the drill; three in a row stop it.
            if ($part.Count -eq 0) {
                $skipped++
                $missedCreates++
                & $write "round ${round}: Create made no partition; the round is skipped ($missedCreates in a row)"
                if ($missedCreates -ge 3) { throw "round ${round}: three Creates in a row made no partition" }
                continue
            }
            $missedCreates = 0
            if ($part.Count -ne 1) { throw "round ${round}: the drive holds $($part.Count) data partitions after Create" }
            $select = @{ partition_number = [int]$part[0].PartitionNumber }
            $letter = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$part[0].Offset)
            if ($letter) { Write-RbFixtureTree -Root "${letter}:\fixture" -FileCount 12 -MaxKilobytes 2048 | Out-Null; Write-VolumeCache -DriveLetter $letter }
            & $run "scan-$round" 'CheckFileSystem' $select @{}
            if ($PauseSeconds -gt 0) { & $write "pause $PauseSeconds s"; Start-Sleep -Seconds $PauseSeconds }
            if ($GuardRemovals) { & $guard $select "hide-$round" }
            & $run "hide-$round" 'SetPartitionHidden' $select @{ hidden = $true }
            & $run "unhide-$round" 'SetPartitionHidden' $select @{ hidden = $false }
            if ($GuardRemovals) { & $guard $select "ext4-$round" }
            & $run "ext4-$round" 'Format' $select @{ file_system = 'ext4'; allocation_unit_bytes = 0; label = 'OPSCHURN'; full_format = $false }
            & $run "ntfs-$round" 'Format' $select @{ file_system = 'NTFS'; allocation_unit_bytes = 4096; label = 'OPSCHURN'; full_format = $false; administrator_scan = $true }
            if ($WithMove) {
                $now = Get-Partition -DiskNumber $disk -PartitionNumber $select.partition_number
                if (-not ($now.DriveLetter -and [char]::IsLetter([char]$now.DriveLetter))) { Add-PartitionAccessPath -DiskNumber $disk -PartitionNumber $now.PartitionNumber -AssignDriveLetter -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
                $letter = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$now.Offset)
                if ($letter) { Write-RbFixtureTree -Root "${letter}:\fixture" -FileCount 12 -MaxKilobytes 2048 | Out-Null; Write-VolumeCache -DriveLetter $letter }
                & $run "move-$round" 'MovePartition' $select @{ target_offset_mib = [int](($base / $Script:OpsMiB) + 4096); backup_directory = (New-CertBackupFolder); confirmed = $true }
                $part = @(Get-Partition -DiskNumber $disk | Where-Object { $_.Type -eq 'Basic' })
                if ($part.Count -eq 1) { $select = @{ partition_number = [int]$part[0].PartitionNumber } }
            }
            if ($GuardRemovals) { & $guard $select "delete-$round" }
            & $run "delete-$round" 'Delete' $select @{}
            foreach ($left in @(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Where-Object { $_.Type -eq 'Basic' -or $_.Type -eq 'Unknown' })) {
                Assert-OpsDevicePin -DiskNumber $disk
                Remove-Partition -DiskNumber $disk -PartitionNumber $left.PartitionNumber -Confirm:$false -ErrorAction SilentlyContinue
                & $write "round ${round}: removed a partition the round left ($($left.Type))"
            }
        }
    } catch {
        $stopped = $_.Exception.Message
        & $write "stopped: $stopped"
    } finally {
        if ($null -ne $Script:RowDevice) {
            foreach ($left in @(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Where-Object { $_.Type -ne 'Reserved' -and $_.Type -ne 'System' -and ([uint64]$_.Offset -lt 8GB) })) {
                Assert-OpsDevicePin -DiskNumber $disk
                Remove-Partition -DiskNumber $disk -PartitionNumber $left.PartitionNumber -Confirm:$false -ErrorAction SilentlyContinue
            }
        }
        Clear-OpsDestructiveDisk
    }
    $checks = @(New-OpsCheck -Name 'the drill ran its time without stopping' -Passed (-not $stopped) -Detail $(if ($stopped) { $stopped } else { "$round rounds, $skipped skipped" }))
    $after = @(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Sort-Object Offset | ForEach-Object { "$($_.Offset):$($_.Size)" }) -join ' '
    $checks += New-OpsCheck -Name "the drive's own partitions are as they were" -Passed ($after -eq $kept) -Detail "$kept / $after"
    $extra = [ordered]@{ minutes = $Minutes; pause_seconds = $PauseSeconds; with_move = [bool]$WithMove; guard_removals = [bool]$GuardRemovals; rounds = $round; skipped_rounds = $skipped; failed_steps = @($failed); log = @($log) }
    $events = Get-OpsDrillEvents -Since $drillStarted
    foreach ($key in $events.Keys) { $extra[$key] = $events[$key] }
    return Complete-OpsRow -Media $media -Checks $checks -Extra $extra
}

function Get-OpsDrillEvents {
    # Every snapshot made during a drill, on any volume, and by whom: VSS's 'creation initiated'
    # (8231, with the process), its other events, volsnap's, and the shadow copies left.
    param([datetime]$Since)
    $vss = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VSS'; StartTime = $Since } -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object { $said = $_.Message -replace '\s+', ' '; "$($_.TimeCreated.ToString('HH:mm:ss')) VSS $($_.Id) $($said.Substring(0, [Math]::Min(260, $said.Length)))" })
    $volsnap = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'volsnap'; StartTime = $Since } -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object { $said = $_.Message -replace '\s+', ' '; "$($_.TimeCreated.ToString('HH:mm:ss')) volsnap $($_.Id) $($said.Substring(0, [Math]::Min(200, $said.Length)))" })
    $shadows = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue | ForEach-Object { "$($_.InstallDate) $($_.VolumeName) $($_.ID)" })
    return [ordered]@{ vss_events = $vss; volsnap_events = $volsnap; shadow_copies = $shadows }
}

Add-OpsRow 'ops.drill-a-scan-then-remove' { Invoke-OpsVolumeChurnDrill -Minutes 90 -PauseSeconds 0 -WithMove }
Add-OpsRow 'ops.drill-b-pause-before-remove' { Invoke-OpsVolumeChurnDrill -Minutes 90 -PauseSeconds 180 }
Add-OpsRow 'ops.drill-c-guarded-remove' { Invoke-OpsVolumeChurnDrill -Minutes 90 -PauseSeconds 0 -GuardRemovals }

function Invoke-OpsVhdDetachDrill {
    <#
    .SYNOPSIS
        Drill D (coordinator, 2026-11-12): is removing the whole disk the ingredient the volsnap
        bugchecks need? All four came while this harness read a volume back and detached its
        virtual disk, seconds after a scan; drills A to C remove volumes, never a disk.
    .DESCRIPTION
        Each round, on a new virtual disk file under -WorkRoot: the app's Create (NTFS), files,
        the app's Check File System, then exactly what the harness does at the end of a row: the
        read-only readback (Get-RbVolumeFacts and chkdsk /scan), each volume flushed and taken
        down with mountvol /P, the disk taken offline, the file detached and deleted. Every step's
        time goes to the runner's log and the row's evidence.
    #>
    param([int]$Minutes, [switch]$WithLoad, [int]$SettleSeconds = 0, [switch]$NoReadback)
    $drillStarted = Get-Date
    $log = [System.Collections.Generic.List[string]]::new()
    $failed = [System.Collections.Generic.List[string]]::new()
    $stopped = ''
    $round = 0
    $skipped = 0
    $missedCreates = 0
    $readbacks = 0
    $dirty = [System.Collections.Generic.List[string]]::new()
    $media = $null
    $load = $null
    $loadLog = @()
    $write = { param([string]$Text) $line = "$((Get-Date).ToString('HH:mm:ss')) $Text"; $log.Add($line); Write-Host "  drill $line" }
    try {
        if ($WithLoad) {
            $load = Start-OpsSyntheticLoad
            & $write "load started (pid $($load.process.Id)); it fills memory over its first minutes"
        }
        $deadline = (Get-Date).AddMinutes($Minutes)
        while ((Get-Date) -lt $deadline) {
            $round++
            & $write "round $round$(if ($load) { "; load: $(Get-OpsSyntheticLoadSample -Load $load)" })"
            $disk = New-OpsDisk -Name "round$round" -SizeMB 2048
            if ($null -eq $media) { $media = Get-OpsMedia -DiskNumber $disk }
            & $write "create-$round start"
            $report = Invoke-OpsStep -Disk $disk -Step "create-$round" -Operation 'Create' -Choices @{ size_mib = 1536; free_before_mib = 0; file_system = 'NTFS'; allocation_unit_bytes = 4096; partition_type = $Script:GptBasicData; label = 'OPSDETACH'; full_format = $false } -TimeoutSeconds 3600 -ReadBackOnFailure
            & $write $(if ($null -eq $report) { "create-$round FAILED" } else { "create-$round done" })
            $part = @(Get-Partition -DiskNumber $disk | Where-Object { $_.Type -eq 'Basic' })
            if ($part.Count -ne 1) {
                if ($null -eq $report) { $failed.Add("create-$round") }
                $skipped++
                $missedCreates++
                & $write "round ${round}: Create made $($part.Count) data partitions; the disk is detached and the round skipped ($missedCreates in a row)"
                Close-OpsRowVolumes -NoSettle
                Remove-CertVhds
                if ($missedCreates -ge 3) { throw "round ${round}: three Creates in a row made no partition" }
                continue
            }
            $missedCreates = 0
            $letter = Wait-OpsLetter -DiskNumber $disk -Offset ([uint64]$part[0].Offset)
            if ($letter) { Write-RbFixtureTree -Root "${letter}:\fixture" -FileCount 12 -MaxKilobytes 2048 | Out-Null; Write-VolumeCache -DriveLetter $letter }
            & $write "scan-$round start"
            $report = Invoke-OpsStep -Disk $disk -Step "scan-$round" -Operation 'CheckFileSystem' -Select @{ partition_number = [int]$part[0].PartitionNumber } -TimeoutSeconds 3600 -ReadBackOnFailure
            if ($null -eq $report) { $failed.Add("scan-$round"); & $write "scan-$round FAILED" } else { & $write "scan-$round done" }
            # Drill F: the disk goes the moment the app says its check is done, as a technician's
            # would; the product's own settle is what is being proved.
            if ($NoReadback) { & $write "round ${round}: no harness readback, detach at once" }
            elseif ($letter) {
                & $write "readback-$round start"
                # -NoSettle: this drill times the removal itself (Drill D none, Drill E -SettleSeconds).
                $read = @(Test-OpsWindowsVolume -Prefix "round ${round}:" -Letter $letter -FileSystem 'NTFS' -Label 'OPSDETACH' -NoSettle)
                $readbacks++
                foreach ($check in $read) { if (-not $check.passed) { $dirty.Add("$($check.name): $($check.detail)") } }
                & $write "readback-$round done"
            } else { & $write "round ${round}: no letter, so no readback" }
            # Drill E: a settle after the last online scan of the round, before the disk goes.
            if ($SettleSeconds -gt 0) { & $write "settle $SettleSeconds s"; Start-Sleep -Seconds $SettleSeconds }
            & $write "detach-$round start"
            Close-OpsRowVolumes -NoSettle
            Remove-CertVhds
            & $write "detach-$round done"
        }
    } catch {
        $stopped = $_.Exception.Message
        & $write "stopped: $stopped"
    } finally {
        if ($load) { $loadLog = Stop-OpsSyntheticLoad -Load $load; & $write 'load stopped' }
    }
    $checks = @(New-OpsCheck -Name 'the drill ran its time without stopping' -Passed (-not $stopped) -Detail $(if ($stopped) { $stopped } else { "$round rounds, $skipped skipped" }))
    if ($WithLoad) {
        # The load is part of what was asked: a drill whose load never reached A's levels is no answer.
        $low = @($loadLog | Where-Object { $_ -match 'free (\d+) MB' -and [int]$Matches[1] -le 2500 }).Count
        $checks += New-OpsCheck -Name 'the load held memory near A run 2 (2.5 GB free or less) for most samples' -Passed ($loadLog.Count -gt 0 -and $low -ge [Math]::Ceiling($loadLog.Count * 0.6)) -Detail "$low of $($loadLog.Count) samples"
    }
    if (-not $NoReadback) { $checks += New-OpsCheck -Name "every readback found the volume as made, and chkdsk's scan clean" -Passed ($readbacks -gt 0 -and $dirty.Count -eq 0) -Detail "$readbacks readbacks; $(@($dirty) -join ' | ')" }
    $left = @(Get-ChildItem -LiteralPath $WorkRoot -Filter "$($Script:RowId.Replace('ops.', ''))-round*.vhdx" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $checks += New-OpsCheck -Name 'no virtual disk file was left behind' -Passed ($left.Count -eq 0) -Detail $left
    $extra = [ordered]@{ minutes = $Minutes; with_load = [bool]$WithLoad; settle_seconds = $SettleSeconds; harness_readback = (-not $NoReadback); rounds = $round; skipped_rounds = $skipped; failed_steps = @($failed); log = @($log); load_samples = @($loadLog) }
    $events = Get-OpsDrillEvents -Since $drillStarted
    foreach ($key in $events.Keys) { $extra[$key] = $events[$key] }
    return Complete-OpsRow -Media $media -Checks $checks -Extra $extra
}

# Drill D's synthetic load (coordinator, 2026-11-12), to match drill A run 2, which ran beside a
# build: memory down to about 1.5 GB free, every CPU busy, steady writes on W: outside the virtual
# disk folder. One process of its own, so it outlives no row: the drill stops it, and a bugcheck
# ends it. Every 30 s it logs free memory, the commit headroom, CPU and W:'s write rate.
$Script:OpsLoadScript = @'
param([string]$Dir, [string]$StopFile, [int]$Spinners, [int]$WriteMBps)
$ErrorActionPreference = 'Continue'
Add-Type -ReferencedAssemblies 'System.Core' -TypeDefinition 'using System; using System.Collections.Generic; using System.Diagnostics; using System.IO; using System.IO.MemoryMappedFiles;
public static class OpsLoad {
    public static void Spin(int milliseconds) { Stopwatch clock = Stopwatch.StartNew(); double x = 1; while (clock.ElapsedMilliseconds < milliseconds) { x = Math.Sqrt(x + 12345.678); } }
    public static void Touch(byte[] block) { for (int i = 0; i < block.Length; i += 4096) { block[i]++; } }
    // File-backed memory, as a linker maps its inputs: it holds physical memory without commit.
    static MemoryMappedFile map;
    static List<MemoryMappedViewAccessor> views = new List<MemoryMappedViewAccessor>();
    public static void Open(string path, long bytes) { using (FileStream file = new FileStream(path, FileMode.Create, FileAccess.ReadWrite)) { file.SetLength(bytes); } map = MemoryMappedFile.CreateFromFile(path, FileMode.Open, null, bytes, MemoryMappedFileAccess.ReadWrite); }
    public static int Views { get { return views.Count; } }
    public static void Grow(long viewBytes) { MemoryMappedViewAccessor view = map.CreateViewAccessor((long)views.Count * viewBytes, viewBytes, MemoryMappedFileAccess.Read); views.Add(view); TouchView(view, viewBytes); }
    public static void Shrink() { MemoryMappedViewAccessor view = views[views.Count - 1]; views.RemoveAt(views.Count - 1); view.Dispose(); }
    static long TouchView(MemoryMappedViewAccessor view, long viewBytes) { long sum = 0; for (long i = 0; i < viewBytes; i += 4096) { sum += view.ReadByte(i); } return sum; }
    public static long TouchViews(long viewBytes) { long sum = 0; foreach (MemoryMappedViewAccessor view in views) { sum += TouchView(view, viewBytes); } return sum; }
    public static void Close() { foreach (MemoryMappedViewAccessor view in views) { view.Dispose(); } views.Clear(); if (map != null) { map.Dispose(); } }
}'
$state = [hashtable]::Synchronized(@{ stop = $false })
$shells = @()
for ($i = 0; $i -lt $Spinners; $i++) {
    $shell = [powershell]::Create()
    [void]$shell.AddScript({ param($Shared) while (-not $Shared.stop) { [OpsLoad]::Spin(1000) } }).AddArgument($state)
    $shells += [pscustomobject]@{ shell = $shell; handle = $shell.BeginInvoke() }
}
$writer = [powershell]::Create()
[void]$writer.AddScript({
        param($Shared, $Folder, $Rate)
        $buffer = New-Object byte[] (4MB)
        (New-Object Random 7).NextBytes($buffer)
        $n = 0
        while (-not $Shared.stop) {
            $n++
            $path = Join-Path $Folder ('load-{0}.bin' -f ($n % 2))
            $stream = [IO.File]::Open($path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
            try {
                $written = 0
                while (-not $Shared.stop -and $written -lt 2GB) {
                    $started = [Diagnostics.Stopwatch]::StartNew()
                    $stream.Write($buffer, 0, $buffer.Length)
                    $written += $buffer.Length
                    $wait = [int](1000 * $buffer.Length / ($Rate * 1MB)) - [int]$started.ElapsedMilliseconds
                    if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
                }
            } finally { $stream.Dispose() }
        }
    }).AddArgument($state).AddArgument($Dir).AddArgument($WriteMBps)
$writerHandle = $writer.BeginInvoke()
$blocks = New-Object System.Collections.Generic.List[byte[]]
$mapViews = 32
[OpsLoad]::Open((Join-Path $Dir 'map.bin'), [long]$mapViews * 256MB)
$log = Join-Path $Dir 'load.log'
$nextSample = Get-Date
while (-not (Test-Path -LiteralPath $StopFile)) {
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    $freeMB = [int]($os.FreePhysicalMemory / 1KB)
    $commitMB = [int]($os.FreeVirtualMemory / 1KB)
    # Fill toward 1.5 GB free: private memory while the commit limit allows it (never closer than
    # 2.5 GB to it: the page file here is small), then a mapped file, which needs no commit; give
    # back when Windows runs short.
    if ($freeMB -gt 2000 -and $commitMB -gt 3000) { $block = New-Object byte[] (256MB); [OpsLoad]::Touch($block); $blocks.Add($block) }
    elseif ($freeMB -gt 1800 -and [OpsLoad]::Views -lt $mapViews) { [OpsLoad]::Grow(256MB) }
    elseif ($freeMB -lt 1000 -and [OpsLoad]::Views -gt 0) { [OpsLoad]::Shrink() }
    elseif (($freeMB -lt 1000 -or $commitMB -lt 2500) -and $blocks.Count -gt 0) { $blocks.RemoveAt($blocks.Count - 1); [GC]::Collect() }
    foreach ($block in $blocks) { [OpsLoad]::Touch($block) }
    [void][OpsLoad]::TouchViews(256MB)
    if ((Get-Date) -ge $nextSample) {
        $nextSample = (Get-Date).AddSeconds(30)
        $cpu = $null; $w = $null
        try {
            $counters = (Get-Counter -Counter '\Processor(_Total)\% Processor Time', '\LogicalDisk(W:)\Disk Write Bytes/sec' -ErrorAction Stop).CounterSamples
            $cpu = [int]$counters[0].CookedValue; $w = [int]($counters[1].CookedValue / 1MB)
        } catch { }
        Add-Content -LiteralPath $log -Value ('{0} free {1} MB, commit headroom {2} MB, held {3} MB private and {4} MB mapped, cpu {5}%, W: writes {6} MB/s' -f (Get-Date).ToString('HH:mm:ss'), $freeMB, $commitMB, ($blocks.Count * 256), ([OpsLoad]::Views * 256), $cpu, $w)
    }
    Start-Sleep -Seconds 2
}
$state.stop = $true
[OpsLoad]::Close()
foreach ($s in $shells) { try { [void]$s.shell.EndInvoke($s.handle) } catch { } ; $s.shell.Dispose() }
try { [void]$writer.EndInvoke($writerHandle) } catch { }
$writer.Dispose()
Remove-Item -LiteralPath (Join-Path $Dir 'load-0.bin'), (Join-Path $Dir 'load-1.bin'), (Join-Path $Dir 'map.bin') -Force -ErrorAction SilentlyContinue
'stopped' | Set-Content -LiteralPath (Join-Path $Dir 'stopped')
'@

function Start-OpsSyntheticLoad {
    # The load's folder is beside the work folder, not in it: the drill's virtual disks stay alone.
    $dir = Join-Path (Split-Path -Parent ((Resolve-Path -LiteralPath $WorkRoot).Path.TrimEnd('\'))) 'load'
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = Join-Path $dir 'load.ps1'
    Set-Content -LiteralPath $file -Value $Script:OpsLoadScript -Encoding ASCII
    $stop = Join-Path $dir 'stop'
    $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $spinners = [Math]::Max(1, [int](Get-CimInstance -ClassName Win32_ComputerSystem).NumberOfLogicalProcessors)
    $process = Start-Process -FilePath $shell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$file`"", '-Dir', "`"$dir`"", '-StopFile', "`"$stop`"", '-Spinners', $spinners, '-WriteMBps', 22) -WindowStyle Hidden -PassThru
    return [pscustomobject]@{ dir = $dir; stop = $stop; process = $process; log = (Join-Path $dir 'load.log') }
}

function Get-OpsSyntheticLoadSample {
    param([object]$Load)
    if ($Load.process.HasExited) { return 'the load process has exited' }
    $last = @(Get-Content -LiteralPath $Load.log -ErrorAction SilentlyContinue | Select-Object -Last 1)
    if ($last.Count -eq 0) { return 'no sample yet' }
    return "$($last[0])"
}

function Stop-OpsSyntheticLoad {
    # Returns every sample the load logged.
    param([object]$Load)
    Set-Content -LiteralPath $Load.stop -Value 'stop'
    if (-not $Load.process.WaitForExit(120000)) { try { $Load.process.Kill() } catch { Write-Host "Could not stop the load: $($_.Exception.Message)" } }
    # Plain strings: a line from Get-Content carries its PSDrive and PSProvider, which ConvertTo-Json
    # follows to the depth the evidence is written at (32): Drill E's report ran for 15 minutes
    # and 7 GB before it was stopped (2026-11-12).
    $samples = @(if (Test-Path -LiteralPath $Load.log) { [IO.File]::ReadAllLines($Load.log) })
    Remove-Item -LiteralPath $Load.dir -Recurse -Force -ErrorAction SilentlyContinue
    return , $samples
}

Add-OpsRow 'ops.drill-d-scan-then-detach-vhd' { Invoke-OpsVhdDetachDrill -Minutes 150 -WithLoad }
Add-OpsRow 'ops.drill-d-idle' { Invoke-OpsVhdDetachDrill -Minutes 150 }
Add-OpsRow 'ops.drill-e-settle-10s' { Invoke-OpsVhdDetachDrill -Minutes 60 -WithLoad -SettleSeconds 10 }
Add-OpsRow 'ops.drill-e-settle-30s' { Invoke-OpsVhdDetachDrill -Minutes 60 -WithLoad -SettleSeconds 30 }
Add-OpsRow 'ops.drill-f-app-settle' { Invoke-OpsVhdDetachDrill -Minutes 60 -WithLoad -NoReadback }

function Invoke-OpsLockAndDismount {
    # FSCTL_LOCK_VOLUME, then FSCTL_DISMOUNT_VOLUME, on the volume's own device, then the handle is
    # closed: the file system flushes and lets go, and the volume stays dismounted until next used.
    param([string]$VolumePath)
    if (-not ('OpsVolumeLock' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class OpsVolumeLock {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint code, IntPtr input, uint inputSize, IntPtr output, uint outputSize, out uint returned, IntPtr overlapped);
    public static string LockAndDismount(string path) {
        using (var handle = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (handle.IsInvalid) { return "open failed " + Marshal.GetLastWin32Error(); }
            uint returned;
            bool locked = false;
            for (int attempt = 0; attempt < 20 && !locked; attempt++) {
                locked = DeviceIoControl(handle, 0x00090018, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned, IntPtr.Zero);
                if (!locked) { System.Threading.Thread.Sleep(500); }
            }
            bool dismounted = DeviceIoControl(handle, 0x00090020, IntPtr.Zero, 0, IntPtr.Zero, 0, out returned, IntPtr.Zero);
            return "locked " + locked + ", dismounted " + dismounted;
        }
    }
}
'@
    }
    return [OpsVolumeLock]::LockAndDismount($VolumePath.TrimEnd('\'))
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
    # Get-OpsShadowMappings returns its list whole (return , $found): wrapped in @() again it was a
    # list of one list, never empty, and this check failed with "System.Object[]" (b28).
    $mappings = Get-OpsShadowMappings
    $checks += New-OpsCheck -Name 'no drive letter is left mapped to a shadow copy' -Passed (@($mappings).Count -eq 0) -Detail (@($mappings) -join ' | ')
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
    $checks += New-OpsCheck -Name 'the boot manager entry points at bootmgfw.efi' -Passed ($bcd -match '(?i)\\EFI\\Microsoft\\Boot\\bootmgfw\.efi') -Detail ($bcd.Trim() -split "`r?`n" | Select-Object -First 8)
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ files_before = $before })
}

Add-OpsRow 'ops.migrate-image-route' {
    # Migrate OS onto a disk too small for the Windows partition and large enough for what it
    # holds: the planner's Image route (DISM). The host then starts the target alone.
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    Set-Content -LiteralPath 'C:\SAKCert\source-marker.txt' -Value $Script:RunId -Encoding ASCII
    $step = Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'migrate' -Operation 'MigrateOs' -Choices @{ verify_mode = 'Sample verification'; allow_no_snapshot = $false } -TimeoutSeconds 10800 -ReadBackOnFailure
    if ($null -eq $step) {
        # What stopped it, from Windows' own records, kept before the VM goes back to its snapshot.
        # On b36 DISM stopped with 225, "the file contains a virus or potentially unwanted
        # software", and the log and Defender's list went with the snapshot.
        $dism = @(Get-Content -LiteralPath "$env:SystemRoot\Logs\DISM\dism.log" -Tail 600 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Error|Warning|0x8|virus|unwanted|blocked' } | Select-Object -Last 30 | ForEach-Object { "$_" })
        $detections = @(Get-MpThreatDetection -ErrorAction SilentlyContinue | Sort-Object InitialDetectionTime -Descending | Select-Object -First 10 | ForEach-Object { "$($_.InitialDetectionTime.ToString('o')) $($_.ThreatID) $($_.ProcessName) $(@($_.Resources) -join ';')" })
        $threats = @(Get-MpThreat -ErrorAction SilentlyContinue | ForEach-Object { "$($_.ThreatID) $($_.ThreatName)" })
        # Defender's own log names the file, the threat and the time (1116 detected, 1117 acted).
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; Id = 1116, 1117; StartTime = (Get-Date).AddHours(-6) } -ErrorAction SilentlyContinue | Select-Object -First 20 | ForEach-Object { "$($_.TimeCreated.ToString('o')) $($_.Id) $(($_.Message -replace '\s+', ' ').Substring(0, [Math]::Min(900, ($_.Message -replace '\s+', ' ').Length)))" })
        return Complete-OpsRow -Media $media -Checks @() -Extra ([ordered]@{ dism_log = $dism; defender_detections = $detections; defender_threats = $threats; defender_events = $events })
    }
    $plan = @($step.queued_operations)[0].payload.migration_plan
    $checks = @(New-OpsCheck -Name 'the plan took the Image route' -Passed ("$($plan.route)" -eq 'Image') -Detail "$($plan.route)")
    # F23's preflight: before writing, the script says how much shadow-copy storage C: has.
    $said = (@($step.execution.steps) | ForEach-Object { "$($_.stdout)" }) -join "`n"
    $checks += New-OpsCheck -Name "before writing, the script reported C:'s shadow-copy storage" -Passed ($said -match 'Shadow-copy storage for C: ') -Detail @($said -split "`r?`n" | Where-Object { $_ -match 'Shadow-copy storage|^WARNING: that leaves' })
    $after = Get-CertSnapshot -DiskNumber $found.number
    $checks += New-OpsCheck -Name 'the target holds a partition table with a Windows-sized data partition' -Passed (@($after.data_partitions).Count -ge 1) -Detail @($after.data_partitions | ForEach-Object { "$($_.type) $($_.size_bytes)" })
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ source_marker = $Script:RunId; after = $after })
}

Add-OpsRow 'ops.migrate-image-shadow-full' {
    <#
    .SYNOPSIS
        F23 (opsfix e142bd5b, coordinator 2026-11-12): the Image route when Windows deletes the
        capture's snapshot because shadow-copy storage on C: is full, as Windows Update did on the
        VM. Before anything is written the script must report C:'s shadow-copy storage and warn that
        it is small; the capture must then stop with S.A.K.'s own account of why, and C: must not
        change.
    .DESCRIPTION
        The fixture: shadow-copy storage for C: at its minimum (vssadmin, 320 MB), and a 3 GB file
        on C: that a separate process overwrites in place, over and over, for the whole step. A
        block overwritten after the snapshot is made is copied into shadow storage first, so the
        storage fills within seconds of the capture starting and volsnap deletes the snapshot. The
        VM goes back to its snapshot after the run; the row still puts the storage limit back.
    #>
    Assert-CertBootRowInVm
    $found = Get-CertReadPin -Role 'any'
    $media = Get-CertMedia -DiskNumber $found.number -Class 'virtual_machine' -Kind 'Windows on a VirtualBox VDI'
    $source = [int](@(Get-Disk | Where-Object IsBoot)[0].Number)
    $sourceBefore = Get-CertSnapshot -DiskNumber $source
    $shadowsBefore = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue).Count
    $storageBefore = (& vssadmin.exe list shadowstorage /for=C: 2>&1 | Out-String).Trim()
    $resized = (& vssadmin.exe resize shadowstorage /for=C: /on=C: /maxsize=320MB 2>&1 | Out-String).Trim()
    $storageSmall = (& vssadmin.exe list shadowstorage /for=C: 2>&1 | Out-String).Trim()
    $churn = 'C:\SAKCert\churn.bin'
    $buffer = New-Object byte[] (4MB)
    (New-Object Random 11).NextBytes($buffer)
    $stream = [IO.File]::Open($churn, [IO.FileMode]::Create, [IO.FileAccess]::Write)
    try { for ($i = 0; $i -lt 768; $i++) { $stream.Write($buffer, 0, $buffer.Length) } } finally { $stream.Dispose() }
    $writer = Join-Path $OutputRoot 'churn-writer.ps1'
    $stop = Join-Path $OutputRoot 'churn-stop'
    Remove-Item -LiteralPath $stop -Force -ErrorAction SilentlyContinue
    Set-Content -LiteralPath $writer -Encoding ASCII -Value @'
param([string]$File, [string]$StopFile)
$buffer = New-Object byte[] (4MB)
$pass = 0
while (-not (Test-Path -LiteralPath $StopFile)) {
    $pass++
    for ($i = 0; $i -lt $buffer.Length; $i += 4096) { $buffer[$i] = [byte]($pass % 251) }
    $stream = [IO.File]::Open($File, [IO.FileMode]::Open, [IO.FileAccess]::Write)
    try {
        while ($stream.Position -lt $stream.Length -and -not (Test-Path -LiteralPath $StopFile)) { $stream.Write($buffer, 0, $buffer.Length); $stream.Flush($true) }
    } finally { $stream.Dispose() }
}
'@
    $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $process = Start-Process -FilePath $shell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$writer`"", '-File', "`"$churn`"", '-StopFile', "`"$stop`"") -WindowStyle Hidden -PassThru
    $started = Get-Date
    $step = $null
    try {
        $step = Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'migrate' -Operation 'MigrateOs' -Choices @{ verify_mode = 'Sample verification'; allow_no_snapshot = $false } -Expect 'failed' -RefusalContains 'Windows deleted the snapshot S.A.K. was copying from because shadow-copy storage on C: was full' -TimeoutSeconds 10800 -ReadBackOnFailure
    } finally {
        Set-Content -LiteralPath $stop -Value 'stop'
        if (-not $process.WaitForExit(60000)) { try { $process.Kill() } catch { Write-Host "Could not stop the churn writer: $($_.Exception.Message)" } }
        Remove-Item -LiteralPath $churn -Force -ErrorAction SilentlyContinue
        $restored = (& vssadmin.exe resize shadowstorage /for=C: /on=C: /maxsize=10% 2>&1 | Out-String).Trim()
    }
    # A step that did not do what the row expected comes back as nothing (its failure is a check of
    # its own already); what it printed is still in its report beside the row.
    $report = Join-Path $Script:RowDir 'migrate.client.json'
    if ($null -eq $step -and (Test-Path -LiteralPath $report)) { $step = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json }
    $said = if ($null -ne $step) { (@($step.execution.steps) | ForEach-Object { "$($_.stdout)`n$($_.stderr)`n$($_.error_message)" }) -join "`n" } else { '' }
    $checks = @(New-OpsCheck -Name 'the fixture: shadow-copy storage for C: was set to its minimum' -Passed ($storageSmall -match '320 MB') -Detail $storageSmall)
    $preflight = @($said -split "`r?`n" | Where-Object { $_ -match 'Shadow-copy storage for C:|^WARNING: that leaves' })
    $checks += New-OpsCheck -Name "before writing, the script reported C:'s shadow-copy storage" -Passed ($said -match 'Shadow-copy storage for C: ') -Detail $preflight
    $checks += New-OpsCheck -Name 'and warned that what is left is under a quarter of the bytes in use' -Passed ($said -match 'WARNING: that leaves .* less than a quarter of the') -Detail $preflight
    $volsnap = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'volsnap'; StartTime = $started } -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object { $text = $_.Message -replace '\s+', ' '; "$($_.TimeCreated.ToString('HH:mm:ss')) volsnap $($_.Id) $($text.Substring(0, [Math]::Min(240, $text.Length)))" })
    $checks += New-OpsCheck -Name 'Windows logged that it deleted the snapshot (volsnap 33 or 36)' -Passed (@($volsnap | Where-Object { $_ -match ' volsnap (33|36) ' }).Count -gt 0) -Detail $volsnap
    $sourceAfter = Get-CertSnapshot -DiskNumber $source
    $checks += New-OpsCheck -Name "C:'s disk table is as it was" -Passed ((ConvertTo-Json -InputObject $sourceBefore.table -Depth 6 -Compress) -eq (ConvertTo-Json -InputObject $sourceAfter.table -Depth 6 -Compress)) -Detail "$(@($sourceAfter.table.partitions).Count) partitions"
    $shadowsAfter = @(Get-CimInstance -ClassName Win32_ShadowCopy -ErrorAction SilentlyContinue).Count
    $checks += New-OpsCheck -Name 'no shadow copy of the attempt was left' -Passed ($shadowsAfter -le $shadowsBefore) -Detail "$shadowsBefore before, $shadowsAfter after"
    # When the account did not come: whether DISM had failed before Windows logged the snapshot's
    # end, to the millisecond, from the event log and dism.log.
    $eventTimes = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'volsnap'; StartTime = $started } -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object { "$($_.TimeCreated.ToString('HH:mm:ss.fff')) volsnap $($_.Id) record $($_.RecordId)" })
    $dismErrors = @(Get-Content -LiteralPath "$env:SystemRoot\Logs\DISM\dism.log" -Tail 400 -ErrorAction SilentlyContinue | Where-Object { $_ -match ', Error ' } | Select-Object -Last 12 | ForEach-Object { "$_" })
    return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ storage_before = $storageBefore; resize = $resized; storage_small = $storageSmall; storage_restored = $restored; volsnap_events = $volsnap; volsnap_event_times = $eventTimes; dism_errors = $dismErrors; preflight = $preflight })
}

function Get-OpsBiosSystemPartition {
    # The partition holding the BIOS boot files on an MBR Windows disk, found by its files and not
    # by its flag: a row clears the flag on purpose.
    param([int]$DiskNumber)
    # An attached Windows disk can come up offline (another disk holds its disk id, or the SAN
    # policy), and its volumes are out of reach until it is online, as the certification's own
    # boot rows found. If two disks share an id Windows gives this one a new id as it comes
    # online, which its BCD would not know: what the id was and is goes with the result.
    $disk = Get-Disk -Number $DiskNumber
    $online = [ordered]@{ was_offline = [bool]$disk.IsOffline; offline_reason = "$($disk.OfflineReason)"; was_read_only = [bool]$disk.IsReadOnly; disk_id_before = ('{0:X8}' -f [uint32]$disk.Signature); disk_id_after = '' }
    if ($disk.IsOffline) { Set-Disk -Number $DiskNumber -IsOffline $false; Start-Sleep -Seconds 3 }
    if ($disk.IsReadOnly) { Set-Disk -Number $DiskNumber -IsReadOnly $false }
    $online.disk_id_after = '{0:X8}' -f [uint32](Get-Disk -Number $DiskNumber).Signature
    $seen = @()
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
            return [pscustomobject]@{ number = [int]$partition.PartitionNumber; letter = $letter; offset = [uint64]$partition.Offset; letter_added = $added; online = $online }
        }
        $seen += "partition $($partition.PartitionNumber) $($partition.Type) letter '$letter'"
        if ($added -and $hasLetter) { Remove-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $partition.PartitionNumber -AccessPath "${letter}:\" -ErrorAction SilentlyContinue }
    }
    throw "Disk $DiskNumber has no partition with BIOS boot files ($(@($seen) -join '; '); $(($online.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))"
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

function Hide-OpsBootmgr {
    # The break: bootmgr renamed out of the way, to a name of its own each time. A disk an earlier
    # row broke and the app repaired still holds that row's renamed copy, which Windows will not
    # let even an administrator delete ("Access to the path is denied", 2026-11-12).
    param([object]$System)
    $name = 'bootmgr.opsproof-broken-{0}' -f (Get-Date).ToString('yyyyMMddHHmmss')
    if (Test-Path -LiteralPath "$($System.letter):\bootmgr") { Rename-Item -LiteralPath "$($System.letter):\bootmgr" -NewName $name -Force }
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
        Hide-OpsBootmgr -System $system
        $broken = Get-OpsBiosBootFacts -DiskNumber $found.number -System $system
        Remove-OpsAddedLetter -DiskNumber $found.number -System $system
        $kept | Add-Member -NotePropertyName facts_before -NotePropertyValue $before -Force
        $kept | Add-Member -NotePropertyName facts_broken -NotePropertyValue $broken -Force
        $kept | Add-Member -NotePropertyName disk_online -NotePropertyValue $system.online -Force
        ($kept | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $state
        $checks = @(New-OpsCheck -Name 'break: no partition is active and bootmgr is gone' -Passed ($broken.active_partitions -eq 0 -and -not $broken.bootmgr_present) -Detail $broken)
        # A new disk id would stop the disk starting by itself, and the next phase would not be
        # telling the break's effect from that one.
        $checks += New-OpsCheck -Name 'break: the disk kept its disk id coming online, so only the break can stop it starting' -Passed ($system.online.disk_id_before -eq $system.online.disk_id_after) -Detail $system.online
        return Complete-OpsRow -Media $media -Checks $checks -Extra ([ordered]@{ phase = 'break'; kept = $kept })
    }
    if ($Phase -ne 'repair') { throw "This row runs one phase at a time, break or repair, not '$Phase'" }
    Remove-OpsAddedLetter -DiskNumber $found.number -System $system
    Invoke-OpsStep -Disk $found.number -GivenPin $found.pin -Step 'repair' -Operation 'RepairBoot' -Choices @{ boot_mode = 'BIOS' } -TimeoutSeconds 900 | Out-Null
    $system = Get-OpsBiosSystemPartition -DiskNumber $found.number
    $after = Get-OpsBiosBootFacts -DiskNumber $found.number -System $system
    Remove-OpsAddedLetter -DiskNumber $found.number -System $system
    # A break phase that stopped kept nothing: that is a failed check here, and the repair's own
    # readback still goes into the evidence.
    $wasBroken = if ($kept.PSObject.Properties['facts_broken']) { $kept.facts_broken } else { $null }
    $checks = @(New-OpsCheck -Name 'break (earlier phase): no partition was active and bootmgr was gone' -Passed ($null -ne $wasBroken -and [int]$wasBroken.active_partitions -eq 0 -and -not $wasBroken.bootmgr_present) -Detail $(if ($null -ne $wasBroken) { $wasBroken } else { 'the break phase kept no facts: it stopped (see its failure in the runner log)' }))
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
    Hide-OpsBootmgr -System $system
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
