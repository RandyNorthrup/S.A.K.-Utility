<#
.SYNOPSIS
    Breaks UEFI boot files on an attached disposable offline Windows disk.

.DESCRIPTION
    Runs elevated inside SAK-PM-Lab-Win11. Targets a non-system cloned OS disk,
    assigns a temporary EFI drive letter, backs up/removes the Microsoft BCD
    store and fallback BOOTX64.EFI, then records layout and command evidence.
#>

[CmdletBinding()]
param(
    [int]$TargetDiskNumber = -1,
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-uefi-break",
    [string]$ExpectedTargetLabel = "OS migration cloned target disk",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$script:TranscriptStarted = $false
$script:RunRoot = ""
$script:Actions = @()

trap {
    # Capture the REAL failure first: the evidence writes below are themselves fallible
    # (network OutputRoot, full disk), and a secondary failure must never replace the
    # destructive-operation error that brought us here.
    $originalError = $_
    if (-not [string]::IsNullOrWhiteSpace($script:RunRoot)) {
        try {
            $errorPath = Join-Path $script:RunRoot "offline-uefi-break-error.json"
            [pscustomobject]@{
                tool = "partition-manager-offline-uefi-bcd-break"
                status = "Failed"
                created_utc = (Get-Date).ToUniversalTime().ToString("o")
                error = $originalError.Exception.Message
                position = $originalError.InvocationInfo.PositionMessage
                stack = $originalError.ScriptStackTrace
                actions = $script:Actions
            } | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $errorPath -Encoding UTF8
        }
        catch {
            Write-Warning "Could not write the error report: $($_.Exception.Message)"
        }
    }
    if ($script:TranscriptStarted) {
        try {
            Stop-Transcript | Out-Null
        }
        catch {
            Write-Warning "Could not stop the transcript: $($_.Exception.Message)"
        }
        $script:TranscriptStarted = $false
    }
    break
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-Action {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Detail
    )
    $script:Actions += [pscustomobject]@{
        timestamp = (Get-Date).ToString("o")
        name = $Name
        detail = $Detail
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    # Clear the inherited exit code first: a command that never STARTS leaves the PREVIOUS
    # command's $LASTEXITCODE in place, and a stale 0 would be read as success.
    $global:LASTEXITCODE = $null
    try {
        $output = & $FilePath @Arguments 2>&1
        $started = $?
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $outputText = (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
    # Evidence records the command line and exit code, not just the (possibly empty) output.
    Add-Action -Name $Name -Detail ("$FilePath $($Arguments -join ' ') (exit $exitCode)`n$outputText").Trim()
    if (-not $started -or $null -eq $exitCode) {
        throw "$Name did not run: $FilePath could not be started. $outputText"
    }
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode."
    }
}

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string[]]$Lines
    )

    $scriptPath = Join-Path $script:RunRoot ("diskpart-" + [guid]::NewGuid().ToString("N") + ".txt")
    $lineText = $Lines -join [Environment]::NewLine
    Set-Content -LiteralPath $scriptPath -Value $lineText -Encoding ASCII
    # Link the generated DiskPart command file into the evidence; the report otherwise
    # names neither the script nor the commands it ran.
    Add-Action -Name "$Name-diskpart-script" -Detail "$scriptPath`n$lineText"
    Invoke-NativeCommand -Name $Name -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
}

function Get-AvailableDriveLetter {
    # A Get-Volume failure must not silently degrade into an EMPTY used-letter list (which
    # would hand back a letter that is already mounted). Test-Path is the independent second
    # check and also sees network/SUBST mappings Get-Volume never reports.
    $used = @(Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    foreach ($letter in @("S", "W", "R", "T", "U", "V", "X", "Y", "Z", "P", "Q", "L", "M", "N")) {
        if ($used -notcontains $letter -and -not (Test-Path -LiteralPath "${letter}:")) {
            return $letter
        }
    }
    throw "No available drive letter."
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
        disk_guid = [string]$disk.Guid
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
        size_bytes = [uint64]$disk.Size
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
        is_offline = [bool]$disk.IsOffline
        is_read_only = [bool]$disk.IsReadOnly
        partitions = $partitions
    }
}

if (-not (Test-IsAdmin)) {
    throw "Run elevated inside the VM."
}
if (-not $Force) {
    throw "Pass -Force after confirming the attached disk is the disposable target clone."
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    throw "OutputRoot must name the evidence directory; it cannot be empty."
}
if ([string]::IsNullOrWhiteSpace($ExpectedTargetLabel)) {
    throw "ExpectedTargetLabel must describe the disposable target; it cannot be empty."
}

# A second-resolution timestamp alone collides: two runs started in the same second would
# share a run directory, transcript, action log and ESP backup names, mixing or overwriting
# each other's evidence. Bind every per-run name to one unique token.
$runToken = [guid]::NewGuid().ToString("N").Substring(0, 8)
$script:RunRoot = Join-Path $OutputRoot ((Get-Date -Format "yyyyMMdd-HHmmss") + "-" + $runToken)
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
Start-Transcript -Path (Join-Path $script:RunRoot "offline-uefi-break-transcript.log") -Force | Out-Null
$script:TranscriptStarted = $true

$candidateDisks = @(Get-Disk | Where-Object {
    -not $_.IsBoot -and -not $_.IsSystem -and
    $_.Size -gt 60GB -and $_.Size -lt 120GB
} | Sort-Object Number)

if ($TargetDiskNumber -lt 0) {
    if ($candidateDisks.Count -ne 1) {
        throw "Expected exactly one non-system 60-120GB target disk, found $($candidateDisks.Count)."
    }
    $TargetDiskNumber = [int]$candidateDisks[0].Number
}

$before = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($before.is_boot -or $before.is_system) {
    throw "Refusing boot/system disk $TargetDiskNumber."
}
# Same bounds as the autodiscovery filter above (exclusive on both ends), so an explicitly
# named disk can never be accepted at a size autodiscovery would have rejected.
if ($before.size_bytes -le 60GB -or $before.size_bytes -ge 120GB) {
    throw "Refusing disk $TargetDiskNumber size $($before.size_bytes); expected disposable cloned OS disk (60-120GB, exclusive)."
}

$newDiskGuid = [guid]::NewGuid().ToString("D")
Invoke-DiskPartScript -Name "prepare-target-disk" -Lines @(
    "select disk $TargetDiskNumber",
    "uniqueid disk id=$newDiskGuid",
    "attributes disk clear readonly",
    "online disk noerr",
    "detail disk",
    "list partition",
    "list volume"
)
Update-HostStorageCache

# DiskPart reports success even when an individual command in the script failed -- and
# "online disk noerr" is explicitly told to continue -- so prove the postconditions instead
# of trusting the exit code. The report claims the new GUID; verify the disk actually took it.
$prepared = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
if ($prepared.is_boot -or $prepared.is_system) {
    throw "Disk $TargetDiskNumber is a boot/system disk after prepare-target-disk; refusing to continue."
}
if ($prepared.is_read_only) {
    throw "Disk $TargetDiskNumber is still read-only after prepare-target-disk; refusing to continue."
}
if ($prepared.is_offline) {
    throw "Disk $TargetDiskNumber is still offline after prepare-target-disk; refusing to continue."
}
if ($prepared.partition_style -eq "GPT") {
    if ([string]::IsNullOrWhiteSpace($prepared.disk_guid) -or
        ([guid]$prepared.disk_guid) -ne ([guid]$newDiskGuid)) {
        throw "Disk $TargetDiskNumber GUID was not changed to $newDiskGuid (observed '$($prepared.disk_guid)')."
    }
}
Add-Action -Name "verify-prepared-disk" -Detail "Disk $TargetDiskNumber is online and writable; GUID $($prepared.disk_guid)."

$efiPartition = Get-Partition -DiskNumber $TargetDiskNumber | Where-Object { [string]$_.Type -eq "System" } | Sort-Object Size | Select-Object -First 1
if ($null -eq $efiPartition) {
    throw "No EFI System partition found on disk $TargetDiskNumber."
}

$efiLetter = if ($efiPartition.DriveLetter) { [string]$efiPartition.DriveLetter } else { Get-AvailableDriveLetter }
if (-not $efiPartition.DriveLetter) {
    try {
        Add-PartitionAccessPath -DiskNumber $TargetDiskNumber -PartitionNumber $efiPartition.PartitionNumber -AccessPath "${efiLetter}:"
    }
    catch {
        # Record the real failure before falling back to DiskPart; the postcondition check
        # below is what actually proves the letter landed on the intended partition.
        Add-Action -Name "assign-efi-letter-access-path-failed" -Detail $_.Exception.Message
        Invoke-DiskPartScript -Name "assign-efi-letter" -Lines @(
            "select disk $TargetDiskNumber",
            "select partition $($efiPartition.PartitionNumber)",
            "assign letter=$efiLetter"
        )
    }
    Add-Action -Name "assign-efi-letter" -Detail "Assigned ${efiLetter}: to EFI partition $($efiPartition.PartitionNumber)."
}

# Resolve the letter BACK to the selected partition before anything is deleted through it:
# an assignment that silently landed on another volume would otherwise take that volume's
# boot files with it.
Update-HostStorageCache
$assignedPartition = Get-Partition -DiskNumber $TargetDiskNumber -PartitionNumber $efiPartition.PartitionNumber -ErrorAction Stop
if ([string]$assignedPartition.DriveLetter -ne $efiLetter) {
    throw "Drive letter ${efiLetter}: does not resolve to disk $TargetDiskNumber partition $($efiPartition.PartitionNumber) (observed '$([string]$assignedPartition.DriveLetter)')."
}
if ([string]$assignedPartition.Type -ne "System") {
    throw "Disk $TargetDiskNumber partition $($efiPartition.PartitionNumber) is no longer an EFI System partition."
}
Add-Action -Name "verify-efi-letter" -Detail "${efiLetter}: resolves to disk $TargetDiskNumber partition $($efiPartition.PartitionNumber) (type System)."

$stamp = (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + $runToken
$bcdPath = "${efiLetter}:\EFI\Microsoft\Boot\BCD"
$bcdBackupPath = "${efiLetter}:\EFI\Microsoft\Boot\BCD.sak-broken-$stamp"
if (-not (Test-Path -LiteralPath $bcdPath -PathType Leaf)) {
    throw "BCD store not found at $bcdPath."
}
# Prove the backup is byte-identical BEFORE deleting the original, and prove the deletion
# happened afterwards -- the report claims both, so neither may be assumed.
$bcdHash = (Get-FileHash -LiteralPath $bcdPath -Algorithm SHA256).Hash
Copy-Item -LiteralPath $bcdPath -Destination $bcdBackupPath -Force
if (-not (Test-Path -LiteralPath $bcdBackupPath -PathType Leaf) -or
    (Get-FileHash -LiteralPath $bcdBackupPath -Algorithm SHA256).Hash -ne $bcdHash) {
    throw "BCD backup at $bcdBackupPath does not match the live store; refusing to remove the BCD."
}
Remove-Item -LiteralPath $bcdPath -Force
if (Test-Path -LiteralPath $bcdPath) {
    throw "BCD store is still present after removal: $bcdPath."
}
Add-Action -Name "remove-microsoft-bcd" -Detail "Backed up $bcdPath to $bcdBackupPath (SHA256 $bcdHash), then removed active BCD."

$fallbackBoot = "${efiLetter}:\EFI\BOOT\BOOTX64.EFI"
$fallbackBackupPath = ""
$fallbackRemoved = $false
if (Test-Path -LiteralPath $fallbackBoot -PathType Leaf) {
    $fallbackBackupPath = "${efiLetter}:\EFI\BOOT\BOOTX64.EFI.sak-broken-$stamp"
    $fallbackHash = (Get-FileHash -LiteralPath $fallbackBoot -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $fallbackBoot -Destination $fallbackBackupPath -Force
    if (-not (Test-Path -LiteralPath $fallbackBackupPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $fallbackBackupPath -Algorithm SHA256).Hash -ne $fallbackHash) {
        throw "Fallback loader backup at $fallbackBackupPath does not match the original; refusing to remove it."
    }
    Remove-Item -LiteralPath $fallbackBoot -Force
    if (Test-Path -LiteralPath $fallbackBoot) {
        throw "Fallback loader is still present after removal: $fallbackBoot."
    }
    $fallbackRemoved = $true
    Add-Action -Name "remove-fallback-loader" -Detail "Backed up $fallbackBoot to $fallbackBackupPath (SHA256 $fallbackHash), then removed fallback loader."
}
else {
    # Not a failure (some clones carry no fallback loader), but it must be visible rather
    # than leaving fallback_removed=false unexplained in the report.
    Add-Action -Name "fallback-loader-absent" -Detail "No fallback loader at $fallbackBoot; only the BCD break applies to this disk."
    Write-Warning "No fallback loader at $fallbackBoot -- fallback_removed stays false in the report."
}

$after = Get-DiskSnapshot -DiskNumber $TargetDiskNumber
$actionsPath = Join-Path $script:RunRoot "offline-uefi-break-actions.json"
$script:Actions | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $actionsPath -Encoding UTF8

$report = [ordered]@{
    tool = "partition-manager-offline-uefi-bcd-break"
    schema_version = 1
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    vm_id = $env:COMPUTERNAME
    expected_target = $ExpectedTargetLabel
    target_disk_number = $TargetDiskNumber
    new_disk_guid = $newDiskGuid
    efi_drive = "${efiLetter}:"
    before_layout = $before
    after_layout = $after
    bcd_path = $bcdPath
    bcd_backup_path = $bcdBackupPath
    bcd_removed = -not (Test-Path -LiteralPath $bcdPath -PathType Leaf)
    fallback_boot_path = $fallbackBoot
    fallback_backup_path = $fallbackBackupPath
    fallback_removed = $fallbackRemoved
    actions_path = $actionsPath
}
$reportPath = Join-Path $script:RunRoot "offline-uefi-break-report.json"
$report | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $reportPath -Encoding UTF8

# Never announce completion off a report whose own evidence says the break did not happen.
if (-not $report.bcd_removed) {
    throw "BCD store is still present at $bcdPath; the offline UEFI break did not complete."
}

if ($script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $script:TranscriptStarted = $false
}

Write-Host "Offline UEFI break complete: $reportPath"
