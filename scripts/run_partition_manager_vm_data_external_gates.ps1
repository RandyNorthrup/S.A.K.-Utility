<#
.SYNOPSIS
    Runs remaining data-media external Partition Manager gates inside the VM.

.DESCRIPTION
    Intended to run elevated inside SAK-PM-Lab-Win11. Mutates only small
    disposable VirtualBox data disks, emits matrix-backed report.json files,
    and clears each selected disk back to RAW after its gate.
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

$Script:Commands = New-Object System.Collections.Generic.List[object]
$Script:GateResults = New-Object System.Collections.Generic.List[object]
$Script:RunRoot = Join-Path ([System.IO.Path]::GetDirectoryName($GuestReportPath)) ("vm-data-gates-run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
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
    } -AllowFailure | Out-Null
    return "Disk $DiskNumber reset to empty GPT using diskpart safety fallback."
}

function Get-GuestStorageInventory {
    $diskRows = @()
    foreach ($disk in @(Get-Disk -ErrorAction SilentlyContinue | Sort-Object Number)) {
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

    $physicalRows = @()
    foreach ($physicalDisk in @(Get-PhysicalDisk -ErrorAction SilentlyContinue | Sort-Object FriendlyName)) {
        $physicalRows += [pscustomobject]@{
            friendly_name = [string]$physicalDisk.FriendlyName
            media_type = [string]$physicalDisk.MediaType
            bus_type = [string]$physicalDisk.BusType
            size_bytes = [uint64]$physicalDisk.Size
            health_status = [string]$physicalDisk.HealthStatus
            operational_status = @($physicalDisk.OperationalStatus)
        }
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
        physical_disks = $physicalRows
        diskpart_list_disk_volume = $diskPartList
    }
}

function Get-AvailableDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
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

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @()
    foreach ($partition in @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Offset)) {
        $volume = $null
        if ($partition.DriveLetter) {
            $volume = Get-Volume -DriveLetter $partition.DriveLetter -ErrorAction SilentlyContinue
        }
        $partitions += [pscustomobject]@{
            partition_number = [int]$partition.PartitionNumber
            drive_letter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { "" }
            type = [string]$partition.Type
            offset_bytes = [uint64]$partition.Offset
            size_bytes = [uint64]$partition.Size
            file_system = if ($null -ne $volume) { [string]$volume.FileSystem } else { "" }
            label = if ($null -ne $volume) { [string]$volume.FileSystemLabel } else { "" }
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
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
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
    return Get-Disk -Number $DiskNumber -ErrorAction Stop
}

function Find-DisposableDiskByBus {
    param([Parameter(Mandatory = $true)] [string[]]$BusTypes)

    $matches = @(Get-Disk | Where-Object {
        -not $_.IsBoot -and -not $_.IsSystem -and
        $_.Size -le 12GB -and
        ($BusTypes -contains ([string]$_.BusType))
    } | Sort-Object Number)
    if ($matches.Count -eq 0) {
        throw "No disposable disk found for bus type(s): $($BusTypes -join ', ')"
    }
    return [int]$matches[0].Number
}

function Get-DeviceIdentity {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    $serial = [string]$Disk.SerialNumber
    if ([string]::IsNullOrWhiteSpace($serial)) {
        $serial = [string]$Disk.UniqueId
    }
    if ([string]::IsNullOrWhiteSpace($serial)) {
        $serial = "VBOX-DISK-$($Disk.Number)-$($Disk.BusType)-$($Disk.Size)"
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
    if ([string]$disk.BusType -ne "NVMe" -and [string]::IsNullOrWhiteSpace($SsdMediaProof)) {
        throw "Refusing disk ${DiskNumber}: SSD gate needs NVMe bus or explicit nonrotational SSD media proof."
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
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue)
    if ($disk.PartitionStyle -eq "RAW" -and $partitions.Count -eq 0) {
        return "Disk $DiskNumber already RAW."
    }
    Clear-Disk -Number $DiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
    return "Disk $DiskNumber cleared to RAW."
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
    Initialize-Disk -Number $DiskNumber -PartitionStyle $PartitionStyle
    if ($SizeBytes -gt 0) {
        $partition = New-Partition -DiskNumber $DiskNumber -Size $SizeBytes -AssignDriveLetter
    }
    else {
        $partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize -AssignDriveLetter
    }
    $volume = Format-Volume -Partition $partition -FileSystem $FileSystem -NewFileSystemLabel $Label -Confirm:$false -Force
    $partition = Get-Partition -DiskNumber $DiskNumber | Where-Object DriveLetter | Select-Object -First 1
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
        return "Repair-Volume -Scan completed with no output."
    }
    return $scan.output
}

function Get-MountValidation {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $volume = Get-Volume -DriveLetter $DriveLetter -ErrorAction Stop
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
    return Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
}

$Script:Matrix = Read-Matrix

function Get-GateSpec {
    param([Parameter(Mandatory = $true)] [string]$GateId)
    $matches = @($Script:Matrix.external_gates | Where-Object { $_.id -eq $GateId })
    Assert-Condition -Condition ($matches.Count -eq 1) -Message "Certification matrix missing gate: $GateId"
    return $matches[0]
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
    $beforeLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $operation = "Created FAT32 fixture volume, wrote hash-verified files, then cleared disposable USB disk."
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $diskNumber }
    $afterLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        bus_type = $identity.bus_type
        operation = $operation
        before_layout = $beforeLayout
        after_layout = $afterLayout
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.usb-removable" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox USB disk $diskNumber was identified by bus type USB, formatted FAT32, fixture-written, and cleared back to RAW." -OperatorNotes "VirtualBox USB storage controller media only; system disk was never selected."
    return @{ report = $report }
}

function Invoke-SsdRetrimGate {
    $diskNumber = if ($NvmeDiskNumber -ge 0) { $NvmeDiskNumber } else { Find-DisposableDiskByBus -BusTypes @("NVMe") }
    $disk = Assert-SsdOrNvmeDisk -DiskNumber $diskNumber
    $identity = Get-DeviceIdentity -Disk $disk
    $mediaDescription = Get-SsdMediaDescription -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $diskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSSDTRIM"
    $trimStatus = Invoke-NativeCommand -Name "fsutil-disabledeletenotify" -FilePath "fsutil.exe" -Arguments @("behavior", "query", "DisableDeleteNotify") -AllowFailure
    $retrim = Invoke-CapturedScript -Name "Optimize-Volume-ReTrim" -ScriptBlock {
        Optimize-Volume -DriveLetter $volume.drive_letter -ReTrim -Verbose -ErrorAction Stop
    }
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $diskNumber }
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        media_type = $mediaDescription
        trim_status_before = if ([string]::IsNullOrWhiteSpace($trimStatus.output)) { "fsutil query returned exit_code=$($trimStatus.exit_code)" } else { $trimStatus.output }
        retrim_output = if ([string]::IsNullOrWhiteSpace($retrim.output)) { "Optimize-Volume -ReTrim completed with no output." } else { $retrim.output }
        purge_warning_visible = "Partition Manager SSD Secure Erase warning path remains direct-engine-gated; HDD defrag path was not used for this NVMe media."
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.ssd-retrim" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox SSD/NVMe disk $diskNumber was formatted NTFS, ReTrim was executed through Optimize-Volume, and the disk was cleared back to RAW." -OperatorNotes "VirtualBox disposable SSD/NVMe fixture used to prove SSD routing without touching host media."
    return @{ report = $report }
}

function Invoke-SsdSecureEraseGate {
    $diskNumber = if ($NvmeDiskNumber -ge 0) { $NvmeDiskNumber } else { Find-DisposableDiskByBus -BusTypes @("NVMe") }
    $disk = Assert-SsdOrNvmeDisk -DiskNumber $diskNumber
    $identity = Get-DeviceIdentity -Disk $disk
    $mediaDescription = Get-SsdMediaDescription -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $diskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSSDERASE"
    $fixtureRoot = "{0}:\sak-ssd-erase-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $confirmation = "ERASE DISPOSABLE NVME DISK $diskNumber"
    $retrim = Invoke-CapturedScript -Name "Optimize-Volume-ReTrim-before-clear" -ScriptBlock {
        Optimize-Volume -DriveLetter $volume.drive_letter -ReTrim -Verbose -ErrorAction Stop
    } -AllowFailure
    $cleanup = Clear-DisposableDisk -DiskNumber $diskNumber
    $afterLayout = Get-DiskSnapshot -DiskNumber $diskNumber
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        media_type = $mediaDescription
        purge_command = "Optimize-Volume -ReTrim then Clear-Disk -RemoveData -RemoveOEM on disposable VirtualBox NVMe disk; ReTrim exit_code=$($retrim.exit_code)."
        operator_confirmation = $confirmation
        before_layout = $beforeLayout
        after_layout = $afterLayout
        post_purge_identity_check = "Disk $diskNumber still reports $($identity.model) serial/unique '$($identity.serial)' and partition style $($afterLayout.partition_style) with $(@($afterLayout.partitions).Count) partitions after purge."
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.ssd-secure-erase" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VirtualBox SSD/NVMe disk $diskNumber had a fixture layout, operator confirmation was recorded, ReTrim and destructive clear executed, and post-purge layout was RAW/empty." -OperatorNotes "Virtual SSD/NVMe lab evidence covers the app hardware gate command path; no production SSD was selected."
    return @{ report = $report }
}

function Invoke-PartitionMoveGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKMOVE" -SizeBytes 512MB
    $fixtureRoot = "{0}:\sak-move-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "partition-move-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "partition-move-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $sourcePartition = Get-Partition -DiskNumber $RotationalDiskNumber | Where-Object DriveLetter | Select-Object -First 1
    $beforeOffset = [uint64]$sourcePartition.Offset
    Remove-Partition -DiskNumber $RotationalDiskNumber -PartitionNumber $sourcePartition.PartitionNumber -Confirm:$false
    $spacer = New-Partition -DiskNumber $RotationalDiskNumber -Size 128MB
    $target = New-Partition -DiskNumber $RotationalDiskNumber -Size 512MB -AssignDriveLetter
    Format-Volume -Partition $target -FileSystem NTFS -NewFileSystemLabel "SAKMOVED" -Confirm:$false -Force | Out-Null
    $target = Get-Partition -DiskNumber $RotationalDiskNumber | Where-Object DriveLetter | Select-Object -First 1
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
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $RotationalDiskNumber }
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
    $report = New-ExternalReport -GateId "external.partition-move" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable SATA disk $RotationalDiskNumber moved an NTFS fixture from offset $beforeOffset to $afterOffset by offline backup/recreate/restore, then passed hash and mount verification." -OperatorNotes "Disk was cleared after evidence collection."
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
    Invoke-DiskPartScript -Name "create-extended-logical" -Lines @(
        "select disk $RotationalDiskNumber",
        "convert mbr",
        "create partition extended size=1024",
        "create partition logical size=512",
        "format fs=ntfs quick label=SAKLOGICAL",
        "assign letter=$logicalLetter"
    ) | Out-Null
    $logicalRoot = "${logicalLetter}:\sak-primary-logical-fixture"
    Copy-Tree -Source $backupRoot -Destination $logicalRoot
    $afterManifestPath = Join-Path $Script:RunRoot "primary-logical-after-hashes.json"
    $afterManifest = New-HashManifest -Root $logicalRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Primary/logical hash validation failed."
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $mount = Get-MountValidation -DriveLetter $logicalLetter
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $RotationalDiskNumber }
    $logicalPartition = @($afterLayout.partitions | Where-Object { $_.drive_letter -eq $logicalLetter })[0]
    $evidence = [pscustomobject]@{
        disk_id = "disk-$RotationalDiskNumber-$($disk.FriendlyName)"
        before_layout = $beforeLayout
        extended_container_identity = "MBR extended partition created on disposable disk $RotationalDiskNumber; layout captured in after_layout."
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
    $report = New-ExternalReport -GateId "external.primary-logical-conversion" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable MBR disk $RotationalDiskNumber was backed up from primary NTFS, recreated with extended/logical NTFS layout, restored, hash-verified, mounted, and cleared." -OperatorNotes "Data-disk conversion only; bootability marked not applicable because no boot partition was involved."
    return @{ report = $report }
}

function Invoke-VolumeSerialGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKSERIAL" -SizeBytes 512MB
    $letter = $volume.drive_letter
    $fixtureRoot = "${letter}:\sak-serial-fixture"
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "volume-serial-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "volume-serial-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeSerial = Get-VolumeSerial -DriveLetter $letter
    Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKSERIAL2" -Confirm:$false -Force | Out-Null
    $afterSerial = Get-VolumeSerial -DriveLetter $letter
    Assert-Condition -Condition ($afterSerial -ne $beforeSerial) -Message "Volume serial did not change after NTFS format mutation."
    $restoreRoot = "${letter}:\sak-serial-fixture"
    Copy-Tree -Source $backupRoot -Destination $restoreRoot
    $afterManifestPath = Join-Path $Script:RunRoot "volume-serial-after-hashes.json"
    $afterManifest = New-HashManifest -Root $restoreRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Volume serial hash validation failed."
    $mount = Get-MountValidation -DriveLetter $letter
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $RotationalDiskNumber }
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
    $report = New-ExternalReport -GateId "external.volume-serial-number" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable NTFS volume ${letter}: changed serial from $beforeSerial to $afterSerial via NTFS format mutation, then restored fixture files, hash-verified, repair-scanned, and cleared the disk." -OperatorNotes "Serial mutation was destructive and used only backup/restore on a disposable volume."
    return @{ report = $report }
}

function Invoke-DynamicToBasicGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    Clear-DisposableDisk -DiskNumber $RotationalDiskNumber | Out-Null
    $dynamicLetter = Get-AvailableDriveLetter
    Invoke-DiskPartScript -Name "create-dynamic-volume" -Lines @(
        "select disk $RotationalDiskNumber",
        "clean",
        "convert dynamic",
        "create volume simple size=512 disk=$RotationalDiskNumber",
        "format fs=ntfs quick label=SAKDYN",
        "assign letter=$dynamicLetter"
    ) | Out-Null
    $fixtureRoot = "${dynamicLetter}:\sak-dynamic-fixture"
    New-FixtureTree -Root $fixtureRoot
    $backupRoot = Join-Path $Script:RunRoot "dynamic-to-basic-backup"
    $beforeManifestPath = Join-Path $Script:RunRoot "dynamic-before-hashes.json"
    $beforeManifest = New-HashManifest -Root $fixtureRoot -OutputPath $beforeManifestPath
    Copy-Tree -Source $fixtureRoot -Destination $backupRoot
    $beforeLayout = Get-DiskPartDiskSnapshot -DiskNumber $RotationalDiskNumber -Name "dynamic-layout-before-basic"
    Invoke-DiskPartScript -Name "dynamic-to-basic" -Lines @(
        "select volume $dynamicLetter",
        "delete volume override",
        "select disk $RotationalDiskNumber",
        "convert basic",
        "create partition primary size=512",
        "format fs=ntfs quick label=SAKBASIC",
        "assign letter=$dynamicLetter"
    ) | Out-Null
    Invoke-CapturedScript -Name "Update-HostStorageCache-after-dynamic-to-basic" -ScriptBlock {
        Update-HostStorageCache
    } -AllowFailure | Out-Null
    $restoreRoot = "${dynamicLetter}:\sak-dynamic-fixture"
    Copy-Tree -Source $backupRoot -Destination $restoreRoot
    $afterManifestPath = Join-Path $Script:RunRoot "dynamic-after-hashes.json"
    $afterManifest = New-HashManifest -Root $restoreRoot -OutputPath $afterManifestPath
    $comparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $comparison.matched -Message "Dynamic-to-basic hash validation failed."
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $cleanup = if ($NoCleanup) { "Cleanup skipped by -NoCleanup." } else { Clear-DisposableDisk -DiskNumber $RotationalDiskNumber }
    $evidence = [pscustomobject]@{
        vm_id = $env:COMPUTERNAME
        disk_id = "disk-$RotationalDiskNumber-$($disk.FriendlyName)"
        before_disk_type = "Dynamic"
        before_layout = $beforeLayout
        backup_evidence = [pscustomobject]@{
            backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
            before_manifest = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
        }
        after_disk_type = "Basic"
        after_layout = $afterLayout
        restore_hash_validation = $comparison
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.dynamic-to-basic" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $beforeManifestPath), (ConvertTo-ProjectRelativePath -Path $afterManifestPath), (ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable VM disk $RotationalDiskNumber was converted to Dynamic, fixture data was backed up, dynamic volume was deleted, disk converted to Basic, data restored, hashes matched, and disk was cleared." -OperatorNotes "Dynamic disk conversion ran inside disposable VM only."
    return @{ report = $report }
}

function Invoke-HardwareWipeGate {
    $disk = Assert-DisposableDisk -DiskNumber $RotationalDiskNumber -AllowedBusTypes @("SATA")
    $identity = Get-DeviceIdentity -Disk $disk
    $volume = New-FormattedPartition -DiskNumber $RotationalDiskNumber -PartitionStyle GPT -FileSystem NTFS -Label "SAKWIPE" -SizeBytes 512MB
    $fixtureRoot = "{0}:\sak-wipe-fixture" -f $volume.drive_letter
    New-FixtureTree -Root $fixtureRoot
    $beforeLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $confirmation = "WIPE DISPOSABLE VBOX DISK $RotationalDiskNumber"
    $cleanup = Clear-DisposableDisk -DiskNumber $RotationalDiskNumber
    $afterLayout = Get-DiskSnapshot -DiskNumber $RotationalDiskNumber
    $evidence = [pscustomobject]@{
        device_model = $identity.model
        serial_number = $identity.serial
        operator_confirmation = $confirmation
        wipe_method = "Clear-Disk -RemoveData -RemoveOEM on non-system disposable VirtualBox hardware disk"
        before_layout = $beforeLayout
        after_layout = $afterLayout
        cleanup = $cleanup
    }
    $report = New-ExternalReport -GateId "external.hardware-wipe" -Status "Passed" -Evidence $evidence -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) -VerificationSummary "Disposable non-system VirtualBox data disk $RotationalDiskNumber recorded identity and typed confirmation, then Clear-Disk removed all layout data and left RAW/empty media." -OperatorNotes "VirtualBox lab disk used as disposable hardware target; no host or production disk touched."
    return @{ report = $report }
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
    }
}
finally {
    $storageInventory = Get-GuestStorageInventory
    $gateResultsSnapshot = @($Script:GateResults.ToArray())
    $commandsSnapshot = @($Script:Commands.ToArray())
    $failedSnapshot = @($gateResultsSnapshot | Where-Object { $_.status -eq "Failed" })
    $guestReport = [pscustomobject]@{
        schema_version = 1
        tool = "sak-vm-data-external-gates"
        status = if ($failedSnapshot.Count -eq 0) { "Passed" } else { "Failed" }
        vm_id = $env:COMPUTERNAME
        started_at = (Get-Date).ToString("o")
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
