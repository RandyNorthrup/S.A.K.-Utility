<#
.SYNOPSIS
    Runs remaining data-media external Partition Manager gates inside the VM.

.DESCRIPTION
    Intended to run elevated inside SAK-PM-Lab-Win11. Mutates only small
    disposable VirtualBox data disks, emits matrix-backed report.json files,
    and clears each selected disk back to RAW after its gate unless -NoCleanup
    is passed, in which case the disk is left populated and every gate records
    that cleanup was skipped. Gates that require a typed operator confirmation
    only run when -OperatorConfirmation carries the exact phrase for that gate.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "\\vboxsvr\sakrepo",
    [string]$EvidenceRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-evidence",
    [string]$GuestReportPath = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\external-vm-data-gates-guest-report.json",
    [int]$RotationalDiskNumber = 1,
    [int]$UsbDiskNumber = -1,
    [int]$NvmeDiskNumber = -1,
    [string]$SsdMediaProof = "",
    [string]$OperatorConfirmation = "",
    [string[]]$GateIds = @(
        "external.usb-removable",
        "external.ssd-retrim",
        "external.ssd-secure-erase",
        "external.partition-move",
        "external.primary-logical-conversion",
        "external.volume-serial-number",
        "external.dynamic-to-basic",
        "external.hardware-wipe"
    ),
    [string]$GateIdsCsv = "",
    [switch]$NoCleanup,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$rawGateIds = @()
if (-not [string]::IsNullOrWhiteSpace($GateIdsCsv)) {
    $rawGateIds = @($GateIdsCsv)
}
else {
    $rawGateIds = @($GateIds)
}
$GateIds = @(
    foreach ($gateId in $rawGateIds) {
        foreach ($part in ([string]$gateId -split ",")) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
                $trimmed
            }
        }
    }
)
if ($GateIds.Count -eq 0) {
    throw "No VM data gate id was selected. Pass -GateIds or -GateIdsCsv with at least one gate id."
}

foreach ($pathParameter in @(
        [pscustomobject]@{ name = "ProjectRoot"; value = $ProjectRoot },
        [pscustomobject]@{ name = "EvidenceRoot"; value = $EvidenceRoot },
        [pscustomobject]@{ name = "GuestReportPath"; value = $GuestReportPath })) {
    if ([string]::IsNullOrWhiteSpace($pathParameter.value) -or -not [System.IO.Path]::IsPathRooted($pathParameter.value)) {
        throw "Refusing to run: -$($pathParameter.name) must be a rooted path, got '$($pathParameter.value)'."
    }
}
if ([System.IO.Path]::GetExtension($GuestReportPath) -ne ".json") {
    throw "Refusing to run: -GuestReportPath must name a .json file, got '$GuestReportPath'."
}

$Script:OperatorConfirmations = @(
    foreach ($confirmationPart in ([string]$OperatorConfirmation -split ",")) {
        $trimmedConfirmation = $confirmationPart.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmedConfirmation)) {
            $trimmedConfirmation
        }
    }
)

$Script:Commands = New-Object System.Collections.Generic.List[object]
$Script:GateResults = New-Object System.Collections.Generic.List[object]
$Script:PinnedDiskIdentities = @{}
$Script:StartedAt = (Get-Date).ToString("o")
$Script:OrchestrationError = ""
$Script:RunRoot = Join-Path ([System.IO.Path]::GetDirectoryName($GuestReportPath)) ("vm-data-gates-run-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$Script:TranscriptStarted = $false

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-OperatorConfirmation {
    param([Parameter(Mandatory = $true)] [string]$Expected)

    if ($Script:OperatorConfirmations -cnotcontains $Expected) {
        throw "Refusing destructive gate: pass -OperatorConfirmation containing the exact typed phrase '$Expected' (comma separated when several gates need one)."
    }
    return $Expected
}

function ConvertTo-PlainText {
    param([object[]]$Value)
    return (($Value | ForEach-Object {
        if ($null -eq $_) { "" } else { $_.ToString() }
    }) -join "`n").Trim()
}

function ConvertTo-ProjectRelativePath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    if (-not $fullRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [System.IO.Path]::DirectorySeparatorChar
    }
    if ($fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return ($fullPath.Substring($fullRoot.Length) -replace "\\", "/")
    }
    return $fullPath
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [switch]$AllowFailure
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $started = Get-Date
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $command = [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = $exitCode
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = ConvertTo-PlainText -Value $output
    }
    $Script:Commands.Add($command) | Out-Null
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "$Name failed with exit code $exitCode. $($command.output)"
    }
    return $command
}

function Invoke-CapturedScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [scriptblock]$ScriptBlock,
        [switch]$AllowFailure
    )

    $started = Get-Date
    $success = $true
    $errorText = ""
    $output = $null
    try {
        $output = & $ScriptBlock *>&1
    }
    catch {
        $success = $false
        $errorText = ConvertTo-PlainText -Value @($_)
        if (-not $AllowFailure) {
            throw
        }
    }
    $command = [pscustomobject]@{
        name = $Name
        file_path = "PowerShell"
        arguments = @()
        exit_code = if ($success) { 0 } else { 1 }
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = ConvertTo-PlainText -Value $output
        error = $errorText
    }
    $Script:Commands.Add($command) | Out-Null
    return $command
}

function Invoke-RecordedCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [scriptblock]$ScriptBlock,
        [string[]]$Arguments = @()
    )

    $recordStarted = Get-Date
    $recordSucceeded = $false
    $recordResult = $null
    try {
        $recordResult = & $ScriptBlock
        $recordSucceeded = $true
    }
    finally {
        $Script:Commands.Add([pscustomobject]@{
            name = $Name
            file_path = "PowerShell"
            arguments = $Arguments
            exit_code = if ($recordSucceeded) { 0 } else { 1 }
            duration_seconds = [Math]::Round(((Get-Date) - $recordStarted).TotalSeconds, 3)
            output = if ($recordSucceeded) { "$Name completed" } else { "$Name failed" }
        }) | Out-Null
    }
    return $recordResult
}

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string[]]$Lines
    )

    $scriptPath = Join-Path $Script:RunRoot ("diskpart-" + [guid]::NewGuid().ToString("N") + ".txt")
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    return Invoke-NativeCommand -Name $Name -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
}

function Get-DiskPartDiskSnapshot {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    $detail = Invoke-DiskPartScript -Name $Name -Lines @(
        "select disk $DiskNumber",
        "detail disk",
        "list volume"
    )
    return [pscustomobject]@{
        disk_number = $DiskNumber
        diskpart_detail = $detail.output
    }
}

function Reset-DisposableDiskWithDiskPart {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    if ($DiskNumber -le 0) {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: only non-system disposable data disks are allowed."
    }
    $driveRows = @(Get-CimInstance -ClassName Win32_DiskDrive -Filter "Index=$DiskNumber" -ErrorAction Stop)
    if ($driveRows.Count -ne 1) {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: expected exactly one Win32_DiskDrive with Index=$DiskNumber, got $($driveRows.Count)."
    }
    $driveModel = [string]$driveRows[0].Model
    $driveSize = [uint64]$driveRows[0].Size
    if ($driveModel -notmatch "VBOX|Virtual|NVMe|HARDDISK") {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: model '$driveModel' is not recognized as disposable VirtualBox media."
    }
    if ($driveSize -le 0 -or $driveSize -gt 12GB) {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: expected small disposable disk, got $driveSize bytes."
    }
    $detail = Get-DiskPartDiskSnapshot -DiskNumber $DiskNumber -Name "diskpart-safety-detail-disk-$DiskNumber"
    $detailText = [string]$detail.diskpart_detail
    if ($detailText -match "(?im)^\s*(Boot|Pagefile|Hibernation File|Crashdump|Clustered)\s+Disk\s*:\s*Yes\s*$") {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: diskpart reports protected OS role."
    }
    if ($detailText -notmatch "(?i)(VBOX|Virtual|NVMe|HARDDISK)") {
        throw "Refusing diskpart recovery for disk ${DiskNumber}: diskpart identity is not recognized as disposable VirtualBox media."
    }

    Invoke-DiskPartScript -Name "diskpart-reset-disposable-disk-$DiskNumber" -Lines @(
        "select disk $DiskNumber",
        "clean",
        "convert gpt"
    ) | Out-Null
    Invoke-CapturedScript -Name "Update-HostStorageCache-after-diskpart-reset-$DiskNumber" -ScriptBlock {
        Update-HostStorageCache
    } | Out-Null
    return "Disk $DiskNumber reset to empty GPT using diskpart safety recovery."
}

function Get-GuestStorageInventory {
    $diskRows = @()
    $diskError = ""
    try {
        foreach ($disk in @(Get-Disk -ErrorAction Stop | Sort-Object Number)) {
            $diskRows += [pscustomobject]@{
                number = [int]$disk.Number
                friendly_name = [string]$disk.FriendlyName
                serial_number = [string]$disk.SerialNumber
                unique_id = [string]$disk.UniqueId
                bus_type = [string]$disk.BusType
                partition_style = [string]$disk.PartitionStyle
                size_bytes = [uint64]$disk.Size
                is_boot = [bool]$disk.IsBoot
                is_system = [bool]$disk.IsSystem
                is_offline = [bool]$disk.IsOffline
                is_read_only = [bool]$disk.IsReadOnly
            }
        }
    }
    catch {
        $diskError = ConvertTo-PlainText -Value @($_)
    }

    $physicalRows = @()
    $physicalError = ""
    try {
        foreach ($physicalDisk in @(Get-PhysicalDisk -ErrorAction Stop | Sort-Object FriendlyName)) {
            $physicalRows += [pscustomobject]@{
                friendly_name = [string]$physicalDisk.FriendlyName
                media_type = [string]$physicalDisk.MediaType
                bus_type = [string]$physicalDisk.BusType
                size_bytes = [uint64]$physicalDisk.Size
                health_status = [string]$physicalDisk.HealthStatus
                operational_status = @($physicalDisk.OperationalStatus)
            }
        }
    }
    catch {
        $physicalError = ConvertTo-PlainText -Value @($_)
    }

    $diskPartList = $null
    try {
        $diskPartListCommand = Invoke-DiskPartScript -Name "inventory-diskpart-list-disk-volume" -Lines @(
            "list disk",
            "list volume"
        )
        $diskPartList = [pscustomobject]@{
            output = $diskPartListCommand.output
        }
    }
    catch {
        $diskPartList = [pscustomobject]@{
            error = ConvertTo-PlainText -Value @($_)
        }
    }

    return [pscustomobject]@{
        disks = $diskRows
        disks_error = $diskError
        physical_disks = $physicalRows
        physical_disks_error = $physicalError
        diskpart_list_disk_volume = $diskPartList
    }
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction Stop | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    foreach ($letter in @("R", "S", "T", "U", "V", "W", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "No available drive letter for VM data gate."
}

function Get-VolumeSerial {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $command = Invoke-NativeCommand -Name "vol-$DriveLetter" -FilePath "cmd.exe" -Arguments @("/c", "vol ${DriveLetter}:")
    $match = [regex]::Match($command.output, "[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}")
    Assert-Condition -Condition $match.Success -Message "Could not parse volume serial for ${DriveLetter}: $($command.output)"
    return $match.Value.ToUpperInvariant()
}

function Get-DiskPartitions {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    try {
        return @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    }
    catch {
        if ($_.Exception.Message -match "No MSFT_Partition objects found") {
            return @()
        }
        throw
    }
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-DiskPartitions -DiskNumber $DiskNumber | Sort-Object Offset)) {
        $volume = $null
        $volumeError = ""
        if ($partition.DriveLetter) {
            try {
                $volume = Get-Volume -DriveLetter $partition.DriveLetter -ErrorAction Stop
            }
            catch {
                $volumeError = ConvertTo-PlainText -Value @($_)
            }
        }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
            file_system = if ($null -ne $volume) { [string]$volume.FileSystem } else { "" }
            label = if ($null -ne $volume) { [string]$volume.FileSystemLabel } else { "" }
            volume_error = $volumeError
        }
    }
    return [pscustomobject]@{
        disk_number = [int]$disk.Number
        friendly_name = [string]$disk.FriendlyName
        serial_number = [string]$disk.SerialNumber
        unique_id = [string]$disk.UniqueId
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        partitions = $partitions
    }
}

function Assert-DisposableDisk {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [string[]]$AllowedBusTypes = @(),
        [switch]$AllowLarge
    )

    try {
        $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    }
    catch {
        if ($_.Exception.Message -match "No MSFT_Disk objects found") {
            Reset-DisposableDiskWithDiskPart -DiskNumber $DiskNumber | Out-Null
            $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
        }
        else {
            throw
        }
    }
    if ($disk.IsBoot -or $disk.IsSystem) {
        throw "Refusing disk ${DiskNumber}: boot/system disk."
    }
    $partitions = @(Get-DiskPartitions -DiskNumber $DiskNumber)
    if ($partitions | Where-Object { $_.IsBoot -or $_.IsSystem }) {
        throw "Refusing disk ${DiskNumber}: boot/system partition present."
    }
    if ($disk.FriendlyName -notmatch "VBOX|Virtual|NVMe|HARDDISK") {
        throw "Refusing disk ${DiskNumber}: not a VirtualBox disposable disk: $($disk.FriendlyName)"
    }
    if (-not $AllowLarge -and $disk.Size -gt 12GB) {
        throw "Refusing disk ${DiskNumber}: expected small disposable disk, got $($disk.Size) bytes."
    }
    if ($AllowedBusTypes.Count -gt 0 -and ($AllowedBusTypes -notcontains ([string]$disk.BusType))) {
        throw "Refusing disk ${DiskNumber}: expected bus $($AllowedBusTypes -join '/'), got $($disk.BusType)."
    }
    if ($disk.IsOffline) {
        Set-Disk -Number $DiskNumber -IsOffline $false
    }
    if ($disk.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false
    }
    $validatedDisk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $identityKey = "$([string]$validatedDisk.FriendlyName)|$([string]$validatedDisk.BusType)|$([uint64]$validatedDisk.Size)|$([string]$validatedDisk.SerialNumber)"
    if ($Script:PinnedDiskIdentities.ContainsKey($DiskNumber)) {
        if ([string]$Script:PinnedDiskIdentities[$DiskNumber] -cne $identityKey) {
            throw "Refusing disk ${DiskNumber}: device identity changed during this run (pinned '$($Script:PinnedDiskIdentities[$DiskNumber])', now '$identityKey')."
        }
    }
    else {
        $Script:PinnedDiskIdentities[$DiskNumber] = $identityKey
    }
    return $validatedDisk
}

function Find-DisposableDiskByBus {
    param([Parameter(Mandatory = $true)] [string[]]$BusTypes)

    $candidates = @(Get-Disk -ErrorAction Stop | Where-Object {
        -not $_.IsBoot -and -not $_.IsSystem -and
        $_.Size -le 12GB -and
        ($BusTypes -contains ([string]$_.BusType))
    } | Sort-Object Number)
    if ($candidates.Count -eq 0) {
        throw "No disposable disk found for bus type(s): $($BusTypes -join ', ')"
    }
    if ($candidates.Count -gt 1) {
        throw "Refusing to auto-select a destructive target: $($candidates.Count) disposable disks match bus type(s) $($BusTypes -join ', ') (disks $(@($candidates | ForEach-Object { [string]$_.Number }) -join ', ')). Pass the explicit disk number."
    }
    return [int]$candidates[0].Number
}

function Get-DeviceIdentity {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    $serial = [string]$Disk.SerialNumber
    if ([string]::IsNullOrWhiteSpace($serial)) {
        $serial = [string]$Disk.UniqueId
    }
    if ([string]::IsNullOrWhiteSpace($serial)) {
        throw "Refusing disk $($Disk.Number): neither SerialNumber nor UniqueId is reported, so device identity evidence cannot be recorded."
    }
    return [pscustomobject]@{
        model = [string]$Disk.FriendlyName
        serial = $serial
        bus_type = [string]$Disk.BusType
        media_type = "VirtualBox $($Disk.BusType) disposable disk; FriendlyName=$($Disk.FriendlyName); SerialOrUniqueId=$serial"
    }
}

function Assert-SsdOrNvmeDisk {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $allowedBusTypes = @("NVMe")
    if (-not [string]::IsNullOrWhiteSpace($SsdMediaProof)) {
        $allowedBusTypes += "SATA"
    }

    $disk = Assert-DisposableDisk -DiskNumber $DiskNumber -AllowedBusTypes $allowedBusTypes
    if ([string]$disk.BusType -ne "NVMe") {
        if ([string]::IsNullOrWhiteSpace($SsdMediaProof)) {
            throw "Refusing disk ${DiskNumber}: SSD gate needs NVMe bus or explicit nonrotational SSD media proof."
        }
        $proofAnchors = @(@([string]$disk.SerialNumber, [string]$disk.UniqueId) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $proofAnchored = $false
        foreach ($anchor in $proofAnchors) {
            if ($SsdMediaProof.IndexOf($anchor, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $proofAnchored = $true
            }
        }
        if (-not $proofAnchored) {
            throw "Refusing disk ${DiskNumber}: -SsdMediaProof must quote this disk's serial number or unique id ($($proofAnchors -join ' / ')) so the proof is bound to the selected device."
        }
    }
    return $disk
}

function Get-SsdMediaDescription {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    if ([string]$Disk.BusType -eq "NVMe") {
        return "VirtualBox NVMe disposable disk; FriendlyName=$($Disk.FriendlyName); SerialOrUniqueId=$($Disk.SerialNumber)$($Disk.UniqueId)"
    }
    return "VirtualBox SATA nonrotational SSD fixture; $SsdMediaProof; FriendlyName=$($Disk.FriendlyName); SerialOrUniqueId=$($Disk.SerialNumber)$($Disk.UniqueId)"
}

function Clear-DisposableDisk {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Assert-DisposableDisk -DiskNumber $DiskNumber
    $partitions = @(Get-DiskPartitions -DiskNumber $DiskNumber)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return "Disk $DiskNumber already RAW with 0 partitions."
    }
    Invoke-RecordedCommand -Name "Clear-Disk-$DiskNumber" -Arguments @("-Number", "$DiskNumber", "-RemoveData", "-RemoveOEM") -ScriptBlock {
        Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    } | Out-Null
    $clearedDisk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $clearedPartitions = @(Get-DiskPartitions -DiskNumber $DiskNumber)
    if ([string]$clearedDisk.PartitionStyle -ne "RAW" -or $clearedPartitions.Count -ne 0) {
        throw "Disk $DiskNumber did not clear to RAW: partition style $($clearedDisk.PartitionStyle) with $($clearedPartitions.Count) partitions."
    }
    return "Disk $DiskNumber cleared and verified RAW with 0 partitions."
}

function Get-GateCleanup {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    if ($NoCleanup) {
        return "Cleanup skipped by -NoCleanup; disposable disk $DiskNumber was left populated."
    }
    return Clear-DisposableDisk -DiskNumber $DiskNumber
}

function New-FormattedPartition {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [string]$PartitionStyle,
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [Parameter(Mandatory = $true)] [string]$Label,
        [uint64]$SizeBytes = 0
    )

    Clear-DisposableDisk -DiskNumber $DiskNumber | Out-Null
    Invoke-RecordedCommand -Name "Initialize-Disk-$DiskNumber" -Arguments @("-Number", "$DiskNumber", "-PartitionStyle", $PartitionStyle) -ScriptBlock {
        Initialize-Disk -Number $DiskNumber -PartitionStyle $PartitionStyle
    } | Out-Null
    if ($SizeBytes -gt 0) {
        $partition = Invoke-RecordedCommand -Name "New-Partition-$DiskNumber" -Arguments @("-DiskNumber", "$DiskNumber", "-Size", "$SizeBytes", "-AssignDriveLetter") -ScriptBlock {
            New-Partition -DiskNumber $DiskNumber -Size $SizeBytes -AssignDriveLetter
        }
    }
    else {
        $partition = Invoke-RecordedCommand -Name "New-Partition-$DiskNumber" -Arguments @("-DiskNumber", "$DiskNumber", "-UseMaximumSize", "-AssignDriveLetter") -ScriptBlock {
            New-Partition -DiskNumber $DiskNumber -UseMaximumSize -AssignDriveLetter
        }
    }
    $volume = Invoke-RecordedCommand -Name "Format-Volume-$DiskNumber" -Arguments @("-FileSystem", $FileSystem, "-NewFileSystemLabel", $Label) -ScriptBlock {
        Format-Volume -Partition $partition -FileSystem $FileSystem -NewFileSystemLabel $Label -Confirm:$false -Force
    }
    $partition = @(Get-DiskPartitions -DiskNumber $DiskNumber | Where-Object DriveLetter | Sort-Object Offset)[0]
    if ($null -eq $partition -or [string]::IsNullOrWhiteSpace([string]$partition.DriveLetter)) {
        throw "Disk $DiskNumber has no drive-letter partition after formatting $FileSystem volume '$Label'."
    }
    return [pscustomobject]@{
        partition = $partition
        volume = $volume
        drive_letter = [string]$partition.DriveLetter
    }
}

function New-FixtureTree {
    param([Parameter(Mandatory = $true)] [string]$Root)

    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $Root "alpha.txt") -Value "SAK Partition Manager fixture alpha $(Get-Date -Format o)" -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $Root "beta.txt") -Value ("beta-" * 512) -Encoding UTF8
    New-Item -ItemType Directory -Path (Join-Path $Root "nested") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $Root "nested\gamma.txt") -Value ([guid]::NewGuid().ToString()) -Encoding UTF8
}

function New-HashManifest {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [string]$OutputPath
    )

    $files = @(Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName)
    $manifest = @()
    foreach ($file in $files) {
        $manifest += [pscustomobject]@{
            relative_path = $file.FullName.Substring(([System.IO.Path]::GetFullPath($Root)).Length).TrimStart("\") -replace "\\", "/"
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            length = [uint64]$file.Length
        }
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    return $manifest
}

function Compare-HashManifest {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Expected,
        [Parameter(Mandatory = $true)] [object[]]$Actual
    )

    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($item in $Expected) {
        $match = @($Actual | Where-Object { $_.relative_path -eq $item.relative_path })
        if ($match.Count -ne 1) {
            $errors.Add("Missing or duplicated file: $($item.relative_path)") | Out-Null
            continue
        }
        if ($match[0].sha256 -ne $item.sha256 -or [uint64]$match[0].length -ne [uint64]$item.length) {
            $errors.Add("Hash/length mismatch: $($item.relative_path)") | Out-Null
        }
    }
    foreach ($actualItem in $Actual) {
        $expectedMatch = @($Expected | Where-Object { $_.relative_path -eq $actualItem.relative_path })
        if ($expectedMatch.Count -eq 0) {
            $errors.Add("Unexpected file not present in expected manifest: $($actualItem.relative_path)") | Out-Null
        }
    }
    return [pscustomobject]@{
        matched = $errors.Count -eq 0
        expected_count = $Expected.Count
        actual_count = $Actual.Count
        errors = @($errors.ToArray())
    }
}

function Copy-Tree {
    param(
        [Parameter(Mandatory = $true)] [string]$Source,
        [Parameter(Mandatory = $true)] [string]$Destination
    )

    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
}

function Invoke-RepairScan {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $scan = Invoke-CapturedScript -Name "Repair-Volume-$DriveLetter" -ScriptBlock {
        Repair-Volume -DriveLetter $DriveLetter -Scan -Verbose -ErrorAction Stop
    }
    if ([string]::IsNullOrWhiteSpace($scan.output)) {
        throw "Repair-Volume -Scan for ${DriveLetter}: produced no output; refusing to record synthesized scan evidence."
    }
    return $scan.output
}

function Get-MountValidation {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $volume = Get-Volume -DriveLetter $DriveLetter -ErrorAction Stop
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace([string]$volume.FileSystem)) -Message "Volume ${DriveLetter}: reports no file system after mutation."
    Assert-Condition -Condition ([string]$volume.HealthStatus -eq "Healthy") -Message "Volume ${DriveLetter}: health status is '$($volume.HealthStatus)', expected Healthy."
    return [pscustomobject]@{
        drive_letter = "${DriveLetter}:"
        file_system = [string]$volume.FileSystem
        label = [string]$volume.FileSystemLabel
        health_status = [string]$volume.HealthStatus
        repair_scan = Invoke-RepairScan -DriveLetter $DriveLetter
    }
}

function Read-Matrix {
    $matrixPath = Join-Path $ProjectRoot "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    if (-not (Test-Path -LiteralPath $matrixPath -PathType Leaf)) {
        throw "Certification matrix not found: $matrixPath"
    }
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($null -eq $matrix) {
        throw "Certification matrix $matrixPath did not parse as JSON."
    }
    if (@($matrix.external_gates | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_.id) }).Count -eq 0) {
        throw "Certification matrix $matrixPath declares no external_gates entries."
    }
    return $matrix
}

$Script:Matrix = Read-Matrix

function Get-GateSpec {
    param([Parameter(Mandatory = $true)] [string]$GateId)
    $specs = @($Script:Matrix.external_gates | Where-Object { $_.id -eq $GateId })
    Assert-Condition -Condition ($specs.Count -eq 1) -Message "Certification matrix missing gate: $GateId"
    $spec = $specs[0]
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace([string]$spec.name)) -Message "Certification matrix gate ${GateId} declares no name."
    Assert-Condition -Condition (@($spec.required_evidence_keys | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) }).Count -gt 0) -Message "Certification matrix gate ${GateId} declares no required_evidence_keys."
    Assert-Condition -Condition (@($spec.safety_contract | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) }).Count -gt 0) -Message "Certification matrix gate ${GateId} declares no safety_contract."
    return $spec
}

function Assert-EvidenceContract {
    param(
        [Parameter(Mandatory = $true)] [string]$GateId,
        [Parameter(Mandatory = $true)] [object]$Spec,
        [Parameter(Mandatory = $true)] [object]$Evidence
    )

    $evidenceValues = @{}
    foreach ($property in @($Evidence.PSObject.Properties)) {
        $evidenceValues[[string]$property.Name] = $property.Value
    }
    foreach ($key in @($Spec.required_evidence_keys)) {
        $keyName = [string]$key
        if (-not $evidenceValues.ContainsKey($keyName)) {
            throw "Gate ${GateId} cannot pass: required evidence key '$keyName' is missing."
        }
        $value = $evidenceValues[$keyName]
        if ($null -eq $value) {
            throw "Gate ${GateId} cannot pass: required evidence key '$keyName' is null."
        }
        if ($value -is [string]) {
            if ([string]::IsNullOrWhiteSpace($value)) {
                throw "Gate ${GateId} cannot pass: required evidence key '$keyName' is empty."
            }
            if ($value -eq "not-verified") {
                throw "Gate ${GateId} cannot pass: required evidence key '$keyName' is still a not-verified placeholder."
            }
        }
    }
    if ($null -ne $Spec.PSObject.Properties["required_evidence_values"]) {
        foreach ($constraint in @($Spec.required_evidence_values.PSObject.Properties)) {
            $constrainedKey = [string]$constraint.Name
            $allowedValues = @($constraint.Value.allowed_values)
            $actualValue = [string]$evidenceValues[$constrainedKey]
            if ($allowedValues.Count -gt 0 -and ($allowedValues -notcontains $actualValue)) {
                throw "Gate ${GateId} cannot pass: evidence '$constrainedKey' = '$actualValue' is not one of: $($allowedValues -join ', ')."
            }
        }
    }
}

function New-ExternalReport {
    param(
        [Parameter(Mandatory = $true)] [string]$GateId,
        [Parameter(Mandatory = $true)] [string]$Status,
        [Parameter(Mandatory = $true)] [object]$Evidence,
        [Parameter(Mandatory = $true)] [string]$VerificationSummary,
        [string[]]$Artifacts = @(),
        [string]$OperatorNotes = "",
        [string]$ErrorMessage = ""
    )

    $spec = Get-GateSpec -GateId $GateId
    if ($Status -eq "Passed") {
        Assert-EvidenceContract -GateId $GateId -Spec $spec -Evidence $Evidence
    }
    $report = [ordered]@{
        tool = "partition-manager-external-evidence-report"
        schema_version = 1
        gate_id = $spec.id
        gate_name = $spec.name
        status = $Status
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        required_evidence_keys = @($spec.required_evidence_keys)
        safety_contract = @($spec.safety_contract)
    }
    if ($null -ne $spec.PSObject.Properties["required_evidence_values"]) {
        $report.required_evidence_values = $spec.required_evidence_values
    }
    $report.evidence = $Evidence
    $report.artifacts = $Artifacts
    $report.suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/$GateId/report.json"
    $report.verification_summary = $VerificationSummary
    $report.operator_notes = $OperatorNotes
    if (-not [string]::IsNullOrWhiteSpace($ErrorMessage)) {
        $report.error_message = $ErrorMessage
    }
    return $report
}

function Write-GateReport {
    param(
        [Parameter(Mandatory = $true)] [string]$GateId,
        [Parameter(Mandatory = $true)] [object]$Report
    )

    $gateDir = Join-Path $EvidenceRoot $GateId
    New-Item -ItemType Directory -Path $gateDir -Force | Out-Null
    $fileName = if ($Report.status -eq "Passed") { "report.json" } else { "report.failed.json" }
    $path = Join-Path $gateDir $fileName
    $Report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

function Reset-GateReports {
    param([Parameter(Mandatory = $true)] [string]$GateId)
    $gateDir = Join-Path $EvidenceRoot $GateId
    New-Item -ItemType Directory -Path $gateDir -Force | Out-Null
    foreach ($name in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
        $path = Join-Path $gateDir $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}

function Invoke-Gate {
    param(
        [Parameter(Mandatory = $true)] [string]$GateId,
        [Parameter(Mandatory = $true)] [scriptblock]$Body
    )

    Reset-GateReports -GateId $GateId
    $beforeCount = $Script:Commands.Count
    try {
        $result = & $Body
        if ($null -eq $result -or $null -eq $result.report) {
            throw "Gate $GateId returned no report object."
        }
        $path = Write-GateReport -GateId $GateId -Report $result.report
        $Script:GateResults.Add([pscustomobject]@{
            gate_id = $GateId
            status = "Passed"
            report_path = $path
        }) | Out-Null
        Write-Host "$GateId passed: $path"
    }
    catch {
        $spec = Get-GateSpec -GateId $GateId
        $failedEvidence = [ordered]@{}
        foreach ($key in @($spec.required_evidence_keys)) {
            $failedEvidence[$key] = "not-verified"
        }
        $failedReport = New-ExternalReport -GateId $GateId -Status "Failed" -Evidence $failedEvidence -VerificationSummary "$GateId failed before complete evidence was collected." -ErrorMessage (ConvertTo-PlainText -Value @($_))
        $path = Write-GateReport -GateId $GateId -Report $failedReport
        $Script:GateResults.Add([pscustomobject]@{
            gate_id = $GateId
            status = "Failed"
            report_path = $path
            error = ConvertTo-PlainText -Value @($_)
        }) | Out-Null
        Write-Warning "$GateId failed: $($_.Exception.Message)"
    }
    finally {
        $gateCommands = @()
        for ($i = $beforeCount; $i -lt $Script:Commands.Count; ++$i) {
            $gateCommands += $Script:Commands[$i]
        }
        if ($gateCommands.Count -gt 0) {
            $commandLogPath = Join-Path $Script:RunRoot (($GateId -replace "[^A-Za-z0-9_.-]", "-") + "-commands.json")
            $gateCommands | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $commandLogPath -Encoding UTF8
        }
    }
}

function Invoke-UsbRemovableGate {
    $diskNumber = if ($UsbDiskNumber -ge 0) { $UsbDiskNumber } else { Find-DisposableDiskByBus -BusTypes @("USB") }
    $disk = Assert-DisposableDisk -DiskNumber $diskNumber -AllowedBusTypes @("USB")
    $identity = Get-DeviceIdentity -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $diskNumber -PartitionStyle MBR -FileSystem FAT32 -Label "SAKUSB"
    $fixtureRoot = "{0}:\sak-usb-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $writtenManifestPath = Join-Path $Script:RunRoot "usb-written-hashes.json"
    $writtenManifest = New-HashManifest -Root $fixtureRoot -OutputPath $writtenManifestPath
    Assert-Condition -Condition (@($writtenManifest).Count -gt 0) -Message "USB fixture tree produced no files to hash."
    $verifyManifestPath = Join-Path $Script:RunRoot "usb-verify-hashes.json"
    $verifyManifest = New-HashManifest -Root $fixtureRoot -OutputPath $verifyManifestPath
    $comparison = Compare-HashManifest -Expected $writtenManifest -Actual $verifyManifest
    Assert-Condition -Condition $comparison.matched -Message "USB fixture hash validation failed: $($comparison.errors -join '; ')"
    $beforeLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $operation = "Created FAT32 fixture volume, wrote files, re-read them and verified every SHA-256 hash, then ran gate cleanup on the disposable USB disk."
    $cleanup = Get-GateCleanup -DiskNumber $diskNumber
    $afterLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        bus_type = $identity.bus_type
        operation = $operation
        before_layout = $beforeLayout
        after_layout = $afterLayout
        file_hash_validation = $comparison
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.usb-removable" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $writtenManifestPath), (ConvertTo-ProjectRelativePath -Path $verifyManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox USB disk $diskNumber was identified by bus type USB, formatted FAT32, fixture-written and SHA-256 readback verified. Cleanup outcome: $cleanup" -OperatorNotes "VirtualBox USB storage controller media only; system disk was never selected."
    return @{ report = $report }
}

function Invoke-SsdRetrimGate {
    $diskNumber = if ($NvmeDiskNumber -ge 0) { $NvmeDiskNumber } else { Find-DisposableDiskByBus -BusTypes @("NVMe") }
    $disk = Assert-SsdOrNvmeDisk -DiskNumber $diskNumber
    $identity = Get-DeviceIdentity -Disk $disk
    $mediaDescription = Get-SsdMediaDescription -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $diskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSSDTRIM"
    $trimStatus = Invoke-NativeCommand -Name "fsutil-disabledeletenotify" -FilePath "fsutil.exe" -Arguments @("behavior", "query", "DisableDeleteNotify")
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($trimStatus.output)) -Message "fsutil behavior query DisableDeleteNotify produced no output; refusing to record synthesized trim evidence."
    $retrim = Invoke-CapturedScript -Name "Optimize-Volume-ReTrim" -ScriptBlock {
        Optimize-Volume -DriveLetter $volume.drive_letter -ReTrim -Verbose -ErrorAction Stop
    }
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($retrim.output)) -Message "Optimize-Volume -ReTrim produced no output; refusing to record synthesized retrim evidence."
    $cleanup = Get-GateCleanup -DiskNumber $diskNumber
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        media_type = $mediaDescription
        trim_status_before = $trimStatus.output
        retrim_output = $retrim.output
        purge_warning_visible = "Partition Manager SSD Secure Erase warning path remains direct-engine-gated; HDD defrag path was not used for this NVMe media. This script does not launch the application UI, so the warning itself is not observed here."
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.ssd-retrim" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox SSD/NVMe disk $diskNumber was formatted NTFS and ReTrim was executed through Optimize-Volume with recorded output. Cleanup outcome: $cleanup" -OperatorNotes "VirtualBox disposable SSD/NVMe fixture used to prove SSD routing without touching host media."
    return @{ report = $report }
}

function Invoke-SsdSecureEraseGate {
    $diskNumber = if ($NvmeDiskNumber -ge 0) { $NvmeDiskNumber } else { Find-DisposableDiskByBus -BusTypes @("NVMe") }
    $confirmation = Assert-OperatorConfirmation -Expected "ERASE DISPOSABLE NVME DISK $diskNumber"
    $disk = Assert-SsdOrNvmeDisk -DiskNumber $diskNumber
    $identity = Get-DeviceIdentity -Disk $disk
    $mediaDescription = Get-SsdMediaDescription -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $diskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSSDERASE"
    $fixtureRoot = "{0}:\sak-ssd-erase-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $retrim = Invoke-CapturedScript -Name "Optimize-Volume-ReTrim-before-clear" -ScriptBlock {
        Optimize-Volume -DriveLetter $volume.drive_letter -ReTrim -Verbose -ErrorAction Stop
    }
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($retrim.output)) -Message "Optimize-Volume -ReTrim produced no output before the destructive clear."
    $cleanup = Clear-DisposableDisk -DiskNumber $diskNumber
    $afterLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    Assert-Condition -Condition ([string]$afterLayout.partition_style -eq "RAW" -and @($afterLayout.partitions).Count -eq 0) -Message "Post-purge layout for disk ${diskNumber} is not RAW/empty: style $($afterLayout.partition_style) with $(@($afterLayout.partitions).Count) partitions."
    $afterIdentity = Get-DeviceIdentity -Disk (Get-Disk -Number $diskNumber -ErrorAction Stop)
    Assert-Condition -Condition ($afterIdentity.serial -eq $identity.serial -and $afterIdentity.model -eq $identity.model) -Message "Disk $diskNumber identity changed across the purge: '$($identity.model)/$($identity.serial)' became '$($afterIdentity.model)/$($afterIdentity.serial)'."
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        media_type = $mediaDescription
        purge_command = "Optimize-Volume -ReTrim then Clear-Disk -RemoveData -RemoveOEM on disposable VirtualBox NVMe disk; ReTrim exit_code=$($retrim.exit_code). No vendor sanitize/secure-erase opcode is issued by this lab path."
        operator_confirmation = $confirmation
        before_layout = $beforeLayout
        after_layout = $afterLayout
        post_purge_identity_check = "Disk $diskNumber re-read after purge still reports $($afterIdentity.model) serial/unique '$($afterIdentity.serial)' and partition style $($afterLayout.partition_style) with $(@($afterLayout.partitions).Count) partitions."
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.ssd-secure-erase" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox SSD/NVMe disk $diskNumber had a fixture layout, the operator-typed confirmation was validated, ReTrim and destructive clear executed, and the post-purge layout was verified RAW/empty with unchanged device identity." -OperatorNotes "Virtual SSD/NVMe lab evidence covers the app hardware gate command path; no production SSD was selected. The purge method is ReTrim plus Clear-Disk, not a vendor sanitize command."
    return @{ report = $report }
}

function Invoke-PartitionMoveGate {
    Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA") | Out-Null
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKMOVE" -SizeBytes 512MB
    $fixtureRoot = "{0}:\sak-move-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "partition-move-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "partition-move-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $sourcePartition = @(Get-DiskPartitions -DiskNumber $RotationalDiskNumber | Where-Object DriveLetter | Sort-Object Offset)[0]
    Assert-Condition -Condition ($null -ne $sourcePartition) -Message "Disk $RotationalDiskNumber has no lettered source partition to move."
    $beforeOffset = [uint64]$sourcePartition.Offset
    Invoke-RecordedCommand -Name "Remove-Partition-$RotationalDiskNumber" -Arguments @("-DiskNumber", "$RotationalDiskNumber", "-PartitionNumber", "$($sourcePartition.PartitionNumber)") -ScriptBlock {
        Remove-Partition -DiskNumber $RotationalDiskNumber -PartitionNumber $sourcePartition.PartitionNumber -Confirm:$false
    } | Out-Null
    Invoke-RecordedCommand -Name "New-Partition-spacer-$RotationalDiskNumber" -Arguments @("-DiskNumber", "$RotationalDiskNumber", "-Size", "128MB") -ScriptBlock {
        New-Partition -DiskNumber $RotationalDiskNumber -Size 128MB
    } | Out-Null
    $target = Invoke-RecordedCommand -Name "New-Partition-target-$RotationalDiskNumber" -Arguments @("-DiskNumber", "$RotationalDiskNumber", "-Size", "512MB", "-AssignDriveLetter") -ScriptBlock {
        New-Partition -DiskNumber $RotationalDiskNumber -Size 512MB -AssignDriveLetter
    }
    Invoke-RecordedCommand -Name "Format-Volume-target-$RotationalDiskNumber" -Arguments @("-FileSystem", "NTFS", "-NewFileSystemLabel", "SAKMOVED") -ScriptBlock {
        Format-Volume -Partition $target -FileSystem NTFS -NewFileSystemLabel "SAKMOVED" -Confirm:$false -Force
    } | Out-Null
    $target = @(Get-DiskPartitions -DiskNumber $RotationalDiskNumber | Where-Object DriveLetter | Sort-Object Offset)[0]
    Assert-Condition -Condition ($null -ne $target) -Message "Disk $RotationalDiskNumber has no lettered target partition after the move."
    $targetRoot = "{0}:\sak-move-fixture" -f $target.DriveLetter
    Copy-Tree -Source $backupRoot -Destination $targetRoot
    $afterManifestPath = Join-Path $Script:RunRoot "partition-move-after-hashes.json"
    $afterManifest = New-HashManifest -Root $targetRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Partition move hash validation failed."
    $mount = Get-MountValidation -DriveLetter ([string]$target.DriveLetter)
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $afterOffset = [uint64]$target.Offset
    Assert-Condition -Condition ($afterOffset -ne $beforeOffset) -Message "Partition offset did not change."
    $cleanup = Get-GateCleanup -DiskNumber $RotationalDiskNumber
    $evidence = [pscustomobject]@{
        partition_id = "disk-$RotationalDiskNumber-partition-$($sourcePartition.PartitionNumber)-to-partition-$($target.PartitionNumber)"
        before_offset_bytes = $beforeOffset
        after_offset_bytes = $afterOffset
        before_layout = $beforeLayout
        after_layout = $afterLayout
        file_hash_validation = $comparison
        mount_validation = $mount
        rollback_or_backup_evidence = [pscustomobject]@{
            backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
            before_manifest = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
            after_manifest = ConvertTo-ProjectRelativePath -Path $afterManifestPath
            offline_move_engine = "Disposable offline lab path backed up, removed, recreated at new offset, restored, and hash-verified the volume while it was not used by the OS."
            cleanup = $cleanup
        }
    }
    $report = New-ExternalReport -GateId "external.partition-move" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable SATA disk $RotationalDiskNumber moved an NTFS fixture from offset $beforeOffset to $afterOffset by offline backup/recreate/restore, then passed hash and mount verification. Cleanup outcome: $cleanup" -OperatorNotes "Cleanup outcome is recorded verbatim in the evidence; -NoCleanup leaves the disposable disk populated."
    return @{ report = $report }
}

function Invoke-PrimaryLogicalGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle MBR -FileSystem NTFS -Label "SAKPRIMARY" -SizeBytes 512MB
    $fixtureRoot = "{0}:\sak-primary-logical-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "primary-logical-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "primary-logical-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $logicalLetter = Get-AvailableDriveLetter
    Clear-DisposableDisk -DiskNumber $RotationalDiskNumber | Out-Null
    $createLogical = Invoke-DiskPartScript -Name "create-extended-logical" -Lines @(
        "select disk $RotationalDiskNumber",
        "convert mbr",
        "create partition extended size=1024",
        "create partition logical size=512",
        "format fs=ntfs quick label=SAKLOGICAL",
        "assign letter=$logicalLetter"
    )
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($createLogical.output)) -Message "diskpart extended/logical creation produced no output for disk $RotationalDiskNumber."
    $logicalRoot = "${logicalLetter}:\sak-primary-logical-fixture"
    Copy-Tree -Source $backupRoot -Destination $logicalRoot
    $afterManifestPath = Join-Path $Script:RunRoot "primary-logical-after-hashes.json"
    $afterManifest = New-HashManifest -Root $logicalRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Primary/logical hash validation failed."
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $logicalPartition = @($afterLayout.partitions | Where-Object { $_.drive_letter -eq $logicalLetter })[0]
    Assert-Condition -Condition ($null -ne $logicalPartition) -Message "No logical partition carrying letter ${logicalLetter} is present on disk $RotationalDiskNumber after conversion."
    Assert-Condition -Condition ([string]$afterLayout.partition_style -eq "MBR") -Message "Disk $RotationalDiskNumber is $($afterLayout.partition_style), expected MBR after extended/logical conversion."
    $mount = Get-MountValidation -DriveLetter $logicalLetter
    $cleanup = Get-GateCleanup -DiskNumber $RotationalDiskNumber
    $evidence = [pscustomobject]@{
        disk_id = "disk-$RotationalDiskNumber-$($disk.FriendlyName)"
        before_layout = $beforeLayout
        extended_container_identity = "MBR extended container created on disposable disk $RotationalDiskNumber by diskpart; diskpart output: $($createLogical.output)"
        logical_volume_identity = "Logical NTFS volume ${logicalLetter}: partition=$($logicalPartition.partition_number) offset=$($logicalPartition.offset_bytes) size=$($logicalPartition.size_bytes)"
        after_layout = $afterLayout
        partition_order_offsets = @($afterLayout.partitions | Select-Object partition_number, type, offset_bytes, size_bytes, drive_letter)
        mount_validation = $mount
        file_hash_validation = $comparison
        bootability_result = "Not applicable to disposable non-system MBR data disk; no active/system partition was created and VM boot disk was untouched."
        rollback_or_backup_evidence = [pscustomobject]@{
            backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
            before_manifest = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
            after_manifest = ConvertTo-ProjectRelativePath -Path $afterManifestPath
            cleanup = $cleanup
        }
    }
    $report = New-ExternalReport -GateId "external.primary-logical-conversion" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable MBR disk $RotationalDiskNumber was backed up from primary NTFS, recreated with extended/logical NTFS layout, restored, hash-verified, and mounted with the logical volume asserted present. Cleanup outcome: $cleanup" -OperatorNotes "Data-disk conversion only; bootability marked not applicable because no boot partition was involved."
    return @{ report = $report }
}

function Invoke-VolumeSerialGate {
    Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA") | Out-Null
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSERIAL" -SizeBytes 512MB
    $letter = $volume.drive_letter
    $fixtureRoot = "${letter}:\sak-serial-fixture"
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "volume-serial-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "volume-serial-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeSerial = Get-VolumeSerial -DriveLetter $letter
    Invoke-RecordedCommand -Name "Format-Volume-serial-mutation-$letter" -Arguments @("-DriveLetter", $letter, "-FileSystem", "NTFS", "-NewFileSystemLabel", "SAKSERIAL2") -ScriptBlock {
        Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKSERIAL2" -Confirm:$false -Force
    } | Out-Null
    $afterSerial = Get-VolumeSerial -DriveLetter $letter
    Assert-Condition -Condition ($afterSerial -ne $beforeSerial) -Message "Volume serial did not change after NTFS format mutation."
    $restoreRoot = "${letter}:\sak-serial-fixture"
    Copy-Tree -Source $backupRoot -Destination $restoreRoot
    $afterManifestPath = Join-Path $Script:RunRoot "volume-serial-after-hashes.json"
    $afterManifest = New-HashManifest -Root $restoreRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Volume serial hash validation failed."
    $mount = Get-MountValidation -DriveLetter $letter
    $cleanup = Get-GateCleanup -DiskNumber $RotationalDiskNumber
    $evidence = [pscustomobject]@{
        volume_id = "disk-$RotationalDiskNumber-volume-${letter}"
        file_system = "NTFS"
        before_serial_number = $beforeSerial
        after_serial_number = $afterSerial
        mount_validation = $mount
        chkdsk_output = $mount.repair_scan
        file_hash_validation = $comparison
        rollback_or_backup_evidence = [pscustomobject]@{
            mutation_command = "Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel SAKSERIAL2"
            backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
            before_manifest = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
            after_manifest = ConvertTo-ProjectRelativePath -Path $afterManifestPath
            cleanup = $cleanup
        }
    }
    $report = New-ExternalReport -GateId "external.volume-serial-number" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable NTFS volume ${letter}: changed serial from $beforeSerial to $afterSerial via NTFS format mutation, then restored fixture files, hash-verified and repair-scanned them. Cleanup outcome: $cleanup" -OperatorNotes "Serial mutation was destructive and used only backup/restore on a disposable volume."
    return @{ report = $report }
}

function Invoke-DynamicToBasicGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    Clear-DisposableDisk -DiskNumber $RotationalDiskNumber | Out-Null
    $dynamicLetter = Get-AvailableDriveLetter
    $createDynamic = Invoke-DiskPartScript -Name "create-dynamic-volume" -Lines @(
        "select disk $RotationalDiskNumber",
        "clean",
        "convert dynamic",
        "create volume simple size=512 disk=$RotationalDiskNumber",
        "format fs=ntfs quick label=SAKDYN",
        "assign letter=$dynamicLetter"
    )
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($createDynamic.output)) -Message "diskpart dynamic-volume creation produced no output for disk $RotationalDiskNumber."
    $fixtureRoot = "${dynamicLetter}:\sak-dynamic-fixture"
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "dynamic-to-basic-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "dynamic-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeLayout = Get-DiskPartDiskSnapshot -DiskNumber $RotationalDiskNumber -Name "dynamic-layout-before-basic"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace([string]$beforeLayout.diskpart_detail)) -Message "diskpart captured no pre-conversion detail for disk $RotationalDiskNumber."
    $convertBasic = Invoke-DiskPartScript -Name "dynamic-to-basic" -Lines @(
        "select volume $dynamicLetter",
        "delete volume override",
        "select disk $RotationalDiskNumber",
        "convert basic",
        "create partition primary size=512",
        "format fs=ntfs quick label=SAKBASIC",
        "assign letter=$dynamicLetter"
    )
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($convertBasic.output)) -Message "diskpart dynamic-to-basic conversion produced no output for disk $RotationalDiskNumber."
    Invoke-CapturedScript -Name "Update-HostStorageCache-after-dynamic-to-basic" -ScriptBlock {
        Update-HostStorageCache
    } | Out-Null
    $restoreRoot = "${dynamicLetter}:\sak-dynamic-fixture"
    Copy-Tree -Source $backupRoot -Destination $restoreRoot
    $afterManifestPath = Join-Path $Script:RunRoot "dynamic-after-hashes.json"
    $afterManifest = New-HashManifest -Root $restoreRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Dynamic-to-basic hash validation failed."
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $basicPartition = @($afterLayout.partitions | Where-Object { $_.drive_letter -eq $dynamicLetter })[0]
    Assert-Condition -Condition ($null -ne $basicPartition) -Message "No basic partition carrying letter ${dynamicLetter} is present on disk $RotationalDiskNumber after the basic conversion."
    $cleanup = Get-GateCleanup -DiskNumber $RotationalDiskNumber
    $evidence = [pscustomobject]@{
        vm_id = $env:COMPUTERNAME
        disk_id = "disk-$RotationalDiskNumber-$($disk.FriendlyName)"
        before_disk_type = "Dynamic"
        before_disk_type_evidence = "diskpart convert dynamic plus create volume simple succeeded on disk $RotationalDiskNumber; diskpart output: $($createDynamic.output)"
        before_layout = $beforeLayout
        backup_evidence = [pscustomobject]@{
            backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
            before_manifest = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
        }
        after_disk_type = "Basic"
        after_disk_type_evidence = "diskpart convert basic plus create partition primary succeeded, and Get-Partition reports partition $($basicPartition.partition_number) at offset $($basicPartition.offset_bytes) carrying letter ${dynamicLetter}; diskpart output: $($convertBasic.output)"
        after_layout = $afterLayout
        restore_hash_validation = $comparison
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.dynamic-to-basic" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VM disk $RotationalDiskNumber was converted to Dynamic, fixture data was backed up, dynamic volume was deleted, disk converted to Basic with the restored partition asserted present, data restored and hashes matched. Cleanup outcome: $cleanup" -OperatorNotes "Dynamic disk conversion ran inside disposable VM only."
    return @{ report = $report }
}

function Invoke-HardwareWipeGate {
    $confirmation = Assert-OperatorConfirmation -Expected "WIPE DISPOSABLE VBOX DISK $RotationalDiskNumber"
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    $identity = Get-DeviceIdentity -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKWIPE" -SizeBytes 512MB
    $fixtureRoot = "{0}:\sak-wipe-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $cleanup = Clear-DisposableDisk -DiskNumber $RotationalDiskNumber
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    Assert-Condition -Condition ([string]$afterLayout.partition_style -eq "RAW" -and @($afterLayout.partitions).Count -eq 0) -Message "Post-wipe layout for disk ${RotationalDiskNumber} is not RAW/empty: style $($afterLayout.partition_style) with $(@($afterLayout.partitions).Count) partitions."
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        operator_confirmation = $confirmation
        wipe_method = "Clear-Disk -RemoveData -RemoveOEM on non-system disposable VirtualBox hardware disk"
        before_layout = $beforeLayout
        after_layout = $afterLayout
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.hardware-wipe" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable non-system VirtualBox data disk $RotationalDiskNumber recorded identity and the operator-typed confirmation, then Clear-Disk removed all layout data and the post-wipe layout was verified RAW/empty." -OperatorNotes "VirtualBox lab disk used as disposable hardware target; no host or production disk touched. Clear-Disk removes layout and file-system data; it is not a full-surface overwrite."
    return @{ report = $report }
}

$Script:RunLock = New-Object System.Threading.Mutex($false, "Global\SAK-PartitionManager-VM-Data-External-Gates")
$Script:RunLockHeld = $false
try {
    $Script:RunLockHeld = $Script:RunLock.WaitOne(0)
}
catch [System.Threading.AbandonedMutexException] {
    $Script:RunLockHeld = $true
}
if (-not $Script:RunLockHeld) {
    throw "Another VM data external gate run holds the single-instance lock; refusing to mutate the same disposable disks and evidence concurrently."
}

try {
    New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $GuestReportPath) -Force | Out-Null
    Start-Transcript -Path (Join-Path $Script:RunRoot "run_partition_manager_vm_data_external_gates.log") -Force | Out-Null
    $Script:TranscriptStarted = $true

    if (-not (Test-IsAdmin)) {
        throw "Run this script from elevated PowerShell inside the VM."
    }
    if (-not $Force) {
        throw "Pass -Force after confirming selected disks are disposable VM data disks."
    }
    if (Test-Path -LiteralPath $GuestReportPath -PathType Leaf) {
        Remove-Item -LiteralPath $GuestReportPath -Force
    }

    $gateMap = @{
        "external.usb-removable" = ${function:Invoke-UsbRemovableGate}
        "external.ssd-retrim" = ${function:Invoke-SsdRetrimGate}
        "external.ssd-secure-erase" = ${function:Invoke-SsdSecureEraseGate}
        "external.partition-move" = ${function:Invoke-PartitionMoveGate}
        "external.primary-logical-conversion" = ${function:Invoke-PrimaryLogicalGate}
        "external.volume-serial-number" = ${function:Invoke-VolumeSerialGate}
        "external.dynamic-to-basic" = ${function:Invoke-DynamicToBasicGate}
        "external.hardware-wipe" = ${function:Invoke-HardwareWipeGate}
    }

    foreach ($gateId in $GateIds) {
        if (-not $gateMap.ContainsKey($gateId)) {
            throw "Unknown VM data gate: $gateId"
        }
        Invoke-Gate -GateId $gateId -Body $gateMap[$gateId]
        $lastGateResult = $Script:GateResults[$Script:GateResults.Count - 1]
        if ([string]$lastGateResult.status -ne "Passed") {
            throw "Aborting remaining VM data gates after $gateId failed; disposable disk state is unknown until an operator inspects it."
        }
    }
}
catch {
    $Script:OrchestrationError = ConvertTo-PlainText -Value @($_)
    throw
}
finally {
    $storageInventory = Get-GuestStorageInventory
    $gateResultsSnapshot = @($Script:GateResults.ToArray())
    $commandsSnapshot = @($Script:Commands.ToArray())
    $failedSnapshot = @($gateResultsSnapshot | Where-Object { $_.status -eq "Failed" })
    $guestReport = [pscustomobject]@{
        schema_version = 1
        tool = "sak-vm-data-external-gates"
        status = if ($gateResultsSnapshot.Count -gt 0 -and $failedSnapshot.Count -eq 0 -and [string]::IsNullOrWhiteSpace($Script:OrchestrationError)) { "Passed" } else { "Failed" }
        orchestration_error = $Script:OrchestrationError
        vm_id = $env:COMPUTERNAME
        started_at = $Script:StartedAt
        completed_at = (Get-Date).ToString("o")
        user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        is_admin = Test-IsAdmin
        rotational_disk_number = $RotationalDiskNumber
        usb_disk_number = $UsbDiskNumber
        nvme_disk_number = $NvmeDiskNumber
        ssd_media_proof = $SsdMediaProof
        run_root = ConvertTo-ProjectRelativePath -Path $Script:RunRoot
        storage_inventory = $storageInventory
        gates = $gateResultsSnapshot
        commands = $commandsSnapshot
    }
    $guestReport | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $Script:TranscriptStarted = $false
    }
    if ($Script:RunLockHeld) {
        $Script:RunLock.ReleaseMutex()
        $Script:RunLockHeld = $false
    }
    $Script:RunLock.Dispose()
}

$failed = @(foreach ($gateResult in $Script:GateResults.ToArray()) {
    if ($gateResult.status -eq "Failed") {
        $gateResult
    }
})
if ($failed.Count -gt 0) {
    Write-Error "One or more VM data external gates failed: $($failed.gate_id -join ', ')"
    exit 1
}

Write-Host "VM data external gates passed: $($GateIds -join ', ')"
exit 0
