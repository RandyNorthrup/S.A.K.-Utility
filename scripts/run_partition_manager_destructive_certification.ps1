<#
.SYNOPSIS
    Runs Partition Manager destructive certification scenarios on disposable VHD media.

.DESCRIPTION
    This harness never targets existing physical disks. It creates temporary VHDX
    files, attaches them, mutates only those VHD-backed disks, verifies the result,
    and writes a JSON evidence report. Boot, USB, SSD purge, and real hardware
    scenarios stay explicit external certification gates.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification",
    [switch]$RunVhdDataDiskMatrix,
    [switch]$RelaunchElevated,
    [switch]$RequireVhdDataDiskEvidence,
    [switch]$KeepVhd,
    [string]$ControllerCertifierPath = "",
    [string]$VhdScenarioId = "",
    [int]$VhdSizeMB = 768
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "partition_manager_storage.ps1")
. (Join-Path $PSScriptRoot "process_capture.ps1")

$MinimumVhdSizeMB = 512
$CreatePartitionBytes = 128MB
$ResizePartitionBytes = 192MB
$ConvertPartitionBytes = 256MB
$RecoveryPartitionBytes = 96MB
$PartitionCloneBytes = 16MB
$PartitionCloneMarkerBytes = 4096
$QuickPartitionReservedBytes = 33MB
$QuickPartitionCount = 2
$QuickPartitionCustomSizes = @(96MB, 128MB, 160MB)
$QuickPartitionMbrCount = 4
$SizeToleranceBytes = 1MB
$BasicDataGptType = "{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"
$Script:Results = @()
$Script:StartedUtc = (Get-Date).ToUniversalTime()
$Script:RunRoot = $null
$ControllerCertifierTimeoutSeconds = 930
$MaximumControllerReportBytes = 4MB
# The conversion fixture is 256 MiB; a stalled prompt must not hold the matrix forever.
$FatConversionTimeoutSeconds = 120
$Script:VhdSafetyStopReason = ""

function Resolve-ProjectRoot {
    return Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }

    return (Join-Path (Resolve-ProjectRoot) $Path)
}

function Read-CertificationMatrix {
    $matrixPath = Join-Path (Resolve-ProjectRoot) "certification\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }
    return $matrix
}

$Script:CertificationMatrix = Read-CertificationMatrix
if (-not [string]::IsNullOrWhiteSpace($VhdScenarioId) -and
    @($Script:CertificationMatrix.vhd_scenarios.id) -cnotcontains $VhdScenarioId) {
    throw "Unknown VHD scenario id: $VhdScenarioId"
}

function Get-CertificationScenarioSpec {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id
    )

    $matches = @($Script:CertificationMatrix.vhd_scenarios + $Script:CertificationMatrix.external_gates |
        Where-Object { $_.id -eq $Id })
    if ($matches.Count -eq 0) {
        return $null
    }
    if ($matches.Count -gt 1) {
        throw "Duplicate certification matrix entries for id '$Id'"
    }
    return $matches[0]
}

function New-CertificationResult {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Status,
        [string]$Message = "",
        [object]$Evidence = $null
    )

    $spec = Get-CertificationScenarioSpec -Id $Id
    $requiredEvidenceKeys = if ($null -eq $spec) { @() } else { @($spec.required_evidence_keys) }
    $requiredEvidenceValues = if ($null -eq $spec -or $null -eq $spec.PSObject.Properties["required_evidence_values"]) {
        $null
    }
    else {
        $spec.required_evidence_values
    }
    $safetyContract = if ($null -eq $spec) { @() } else { @($spec.safety_contract) }

    [pscustomobject]@{
        id = $Id
        name = $Name
        status = $Status
        message = $Message
        required_evidence_keys = $requiredEvidenceKeys
        required_evidence_values = $requiredEvidenceValues
        safety_contract = $safetyContract
        evidence = $Evidence
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
    }
}

function Add-CertificationResult {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    $Script:Results += $Result
    $color = switch ($Result.status) {
        "Passed" { "Green" }
        "Failed" { "Red" }
        "Skipped" { "Yellow" }
        "ExternalGate" { "Cyan" }
        default { "White" }
    }
    Write-Host "[$($Result.status)] $($Result.id) - $($Result.name)" -ForegroundColor $color
    if (-not [string]::IsNullOrWhiteSpace($Result.message)) {
        Write-Host "  $($Result.message)"
    }
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-ProcessArgument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    # Follow the CommandLineToArgvW rules exactly: double every run of backslashes that
    # precedes a double-quote and escape that quote, then double a run of trailing
    # backslashes so the closing quote is not itself escaped. A naive C-style \" leaves a
    # malicious OutputRoot able to corrupt argument boundaries or inject extra switches.
    $escaped = [System.Text.RegularExpressions.Regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [System.Text.RegularExpressions.Regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Invoke-ElevatedRelaunch {
    $hostPath = (Get-Process -Id $PID).Path
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (ConvertTo-ProcessArgument -Value $PSCommandPath),
        "-OutputRoot",
        (ConvertTo-ProcessArgument -Value $OutputRoot),
        "-RunVhdDataDiskMatrix",
        "-VhdSizeMB",
        $VhdSizeMB.ToString()
    )

    if ($KeepVhd) {
        $arguments += "-KeepVhd"
    }
    if ($RequireVhdDataDiskEvidence) {
        $arguments += "-RequireVhdDataDiskEvidence"
    }
    if (-not [string]::IsNullOrWhiteSpace($ControllerCertifierPath)) {
        $arguments += @("-ControllerCertifierPath", (ConvertTo-ProcessArgument $ControllerCertifierPath))
    }
    if (-not [string]::IsNullOrWhiteSpace($VhdScenarioId)) {
        $arguments += @("-VhdScenarioId", (ConvertTo-ProcessArgument $VhdScenarioId))
    }

    Write-Host "Relaunching Partition Manager certification in an elevated PowerShell window..."
    $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
}

if ($RunVhdDataDiskMatrix -and $RelaunchElevated -and -not (Test-Administrator)) {
    Invoke-ElevatedRelaunch
}

function Assert-CertificationCondition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-SystemToolPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    # Resolve under the real Windows system directory ([System.Environment]::SystemDirectory,
    # backed by GetSystemDirectory), NEVER %PATH% or the mutable $env:SystemRoot, so an
    # elevated launch cannot be redirected to an attacker-planted diskpart.exe / convert.exe.
    # Fail closed if the directory or the tool cannot be resolved.
    $systemDir = [System.Environment]::SystemDirectory
    if ([string]::IsNullOrWhiteSpace($systemDir)) {
        throw "Cannot resolve the Windows system directory for '$Name'"
    }
    $path = Join-Path $systemDir $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required system tool not found: $path"
    }
    return $path
}

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Lines
    )

    $scriptPath = Join-Path $Script:RunRoot "diskpart-$([guid]::NewGuid()).txt"
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    $diskpartPath = Resolve-SystemToolPath -Name "diskpart.exe"
    $output = & $diskpartPath /s $scriptPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "diskpart failed with exit code $LASTEXITCODE`n$($output -join [Environment]::NewLine)"
    }
    return ($output -join [Environment]::NewLine)
}

function Get-AttachedVhdDisk {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            $disk = Get-DiskImage -ImagePath $Path -ErrorAction Stop | Get-Disk -ErrorAction Stop
            if ($null -ne $disk) {
                return $disk
            }
        }
        catch {
            Start-Sleep -Milliseconds 250
        }
    }
    throw "Attached VHD disk not found for $Path"
}

function New-DisposableVhdDisk {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [string]$Directory = $Script:RunRoot
    )

    $safeName = $Name -replace "[^A-Za-z0-9_.-]", "-"
    $path = Join-Path $Directory "$safeName.vhdx"
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite an existing certification VHD: $path"
    }

    $size = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
    Invoke-DiskPartScript -Lines @(
        "create vdisk file=`"$path`" maximum=$size type=expandable",
        "attach vdisk"
    ) | Out-Null

    try {
        $disk = Get-AttachedVhdDisk -Path $path
        if ($disk.IsOffline) {
            Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction Stop
        }
        if ($disk.IsReadOnly) {
            Set-Disk -Number $disk.Number -IsReadOnly $false -ErrorAction Stop
        }

        [pscustomobject]@{
            path = $path
            disk_number = [int]$disk.Number
            size_mb = $size
            name = $safeName
        }
    }
    catch {
        # Creation is not transactional: the vdisk is already attached by the diskpart
        # script above, so any failure while resolving it or bringing it online/read-write
        # must dismount it rather than leak an attached disk. Then re-throw the real error.
        $preparationError = $_
        try {
            Dismount-SakDiskImageIfAttached -Path $path
        }
        catch {
            throw "VHD preparation failed: $($preparationError.Exception.Message) Cleanup dismount also failed: $($_.Exception.Message)"
        }
        throw $preparationError
    }
}

function Remove-DisposableVhdDisk {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Vhd
    )

    Dismount-SakDiskImageIfAttached -Path $Vhd.path
    if (-not $KeepVhd -and (Test-Path -LiteralPath $Vhd.path)) {
        Remove-Item -LiteralPath $Vhd.path -Force
    }
}

function Get-FreeCertificationDriveLetter {
    $used = @(Get-Volume -ErrorAction Stop |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_.DriveLetter) } |
        ForEach-Object { $_.DriveLetter.ToString().ToUpperInvariant() })

    foreach ($letter in @("T", "U", "V", "W", "X", "Y", "Z")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "No free certification drive letter found"
}

function Get-CertificationDriveLetter {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber,
        [Parameter(Mandatory = $true)]
        [int]$PartitionNumber
    )

    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    try {
        $volume = $partition | Get-Volume -ErrorAction Stop
    }
    catch {
        if ($_.CategoryInfo.Category -ne [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            throw
        }
        $volume = $null
    }
    if ($null -ne $volume -and -not [string]::IsNullOrWhiteSpace($volume.DriveLetter)) {
        return $volume.DriveLetter.ToString().ToUpperInvariant()
    }

    $letter = Get-FreeCertificationDriveLetter
    Add-PartitionAccessPath -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -AccessPath "$letter`:\" -ErrorAction Stop
    return $letter
}

function Invoke-FatVolumeConversion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DriveLetter,
        [Parameter(Mandatory = $true)]
        [string]$VolumeLabel,
        [Parameter(Mandatory = $true)]
        [hashtable]$State
    )

    Assert-CertificationCondition -Condition ($DriveLetter -cmatch '\A[A-Z]\z') -Message "Invalid conversion drive letter"
    $convertPath = Resolve-SystemToolPath -Name "convert.exe"
    $State.cleanup_allowed = $false
    $captured = Invoke-CapturedProcess -FilePath $convertPath `
        -Arguments @("$DriveLetter`:", '/FS:NTFS', '/NoSecurity', '/X') `
        -InputText ($VolumeLabel + [Environment]::NewLine) -TimeoutSeconds $FatConversionTimeoutSeconds
    $output = @($captured.stdout, $captured.stderr) -join [Environment]::NewLine
    if ($captured.timed_out) {
        throw "convert.exe exceeded the $FatConversionTimeoutSeconds second conversion deadline`n$output"
    }
    # A completed native failure is safe to clean. A capture error/timeout is not proof
    # that every worker stopped, so leave the fixture attached for explicit recovery.
    $State.cleanup_allowed = $true
    if ($captured.exit_code -ne 0) {
        throw "convert.exe failed with exit code $($captured.exit_code)`n$output"
    }
    return $output
}

function Set-CertificationDiskOffline {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber
    )

    Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction Stop
    Set-Disk -Number $DiskNumber -IsOffline $true -ErrorAction Stop
}

function Set-CertificationDiskOnline {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber
    )

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ($disk.IsOffline) {
        Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction Stop
    }
    if ($disk.IsReadOnly) {
        Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction Stop
    }
}

function Open-RawCertificationDisk {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber,
        [Parameter(Mandatory = $true)]
        [System.IO.FileAccess]$Access
    )

    $path = "\\.\PhysicalDrive$DiskNumber"
    return [System.IO.File]::Open($path,
        [System.IO.FileMode]::Open,
        $Access,
        [System.IO.FileShare]::ReadWrite)
}

function New-CertificationPattern {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [int]$Size = $PartitionCloneMarkerBytes
    )

    $bytes = New-Object byte[] $Size
    $seed = [System.Text.Encoding]::ASCII.GetBytes($Text)
    if ($Size -le 0 -or $seed.Length -eq 0) {
        return $bytes
    }
    # Tile the seed across the buffer by repeated doubling via Buffer.BlockCopy. This is
    # byte-for-byte identical to $bytes[$i] = $seed[$i % $seed.Length] (each doubling copies
    # a whole-seed-multiple region), but avoids a multi-million-iteration PowerShell loop so
    # a full 16 MiB clone pattern can be generated cheaply.
    $seedLen = [Math]::Min($seed.Length, $Size)
    [System.Buffer]::BlockCopy($seed, 0, $bytes, 0, $seedLen)
    $filled = $seedLen
    while ($filled -lt $Size) {
        $copy = [Math]::Min($filled, $Size - $filled)
        [System.Buffer]::BlockCopy($bytes, 0, $bytes, $filled, $copy)
        $filled += $copy
    }
    return $bytes
}

function Test-ByteArrayHashEqual {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Expected,
        [Parameter(Mandatory = $true)]
        [byte[]]$Actual
    )

    # Compare potentially large regions (a full multi-MiB clone) by SHA-256 so verification
    # covers every byte without a per-byte PowerShell loop. Length is checked first.
    if ($Expected.Length -ne $Actual.Length) {
        return $false
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $expectedHash = $sha.ComputeHash($Expected)
        $actualHash = $sha.ComputeHash($Actual)
    }
    finally {
        $sha.Dispose()
    }
    for ($i = 0; $i -lt $expectedHash.Length; $i++) {
        if ($expectedHash[$i] -ne $actualHash[$i]) {
            return $false
        }
    }
    return $true
}

function Write-RawCertificationBytes {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber,
        [Parameter(Mandatory = $true)]
        [uint64]$Offset,
        [Parameter(Mandatory = $true)]
        [byte[]]$Bytes
    )

    $stream = Open-RawCertificationDisk -DiskNumber $DiskNumber -Access ([System.IO.FileAccess]::ReadWrite)
    try {
        [void]$stream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }
}

function Read-RawCertificationBytes {
    param(
        [Parameter(Mandatory = $true)]
        [int]$DiskNumber,
        [Parameter(Mandatory = $true)]
        [uint64]$Offset,
        [Parameter(Mandatory = $true)]
        [int]$Size
    )

    $buffer = New-Object byte[] $Size
    $stream = Open-RawCertificationDisk -DiskNumber $DiskNumber -Access ([System.IO.FileAccess]::Read)
    try {
        [void]$stream.Seek([int64]$Offset, [System.IO.SeekOrigin]::Begin)
        $read = 0
        while ($read -lt $Size) {
            $chunk = $stream.Read($buffer, $read, $Size - $read)
            if ($chunk -le 0) {
                throw "Unexpected end of raw disk while reading Disk $DiskNumber at offset $Offset"
            }
            $read += $chunk
        }
    }
    finally {
        $stream.Dispose()
    }
    return $buffer
}

function Copy-RawCertificationBytes {
    param(
        [Parameter(Mandatory = $true)]
        [int]$SourceDiskNumber,
        [Parameter(Mandatory = $true)]
        [uint64]$SourceOffset,
        [Parameter(Mandatory = $true)]
        [int]$TargetDiskNumber,
        [Parameter(Mandatory = $true)]
        [uint64]$TargetOffset,
        [Parameter(Mandatory = $true)]
        [uint64]$Bytes
    )

    $bufferBytes = 1MB
    $sourceStream = Open-RawCertificationDisk -DiskNumber $SourceDiskNumber -Access ([System.IO.FileAccess]::Read)
    $targetStream = Open-RawCertificationDisk -DiskNumber $TargetDiskNumber -Access ([System.IO.FileAccess]::ReadWrite)
    try {
        [void]$sourceStream.Seek([int64]$SourceOffset, [System.IO.SeekOrigin]::Begin)
        [void]$targetStream.Seek([int64]$TargetOffset, [System.IO.SeekOrigin]::Begin)
        $buffer = New-Object byte[] $bufferBytes
        $remaining = $Bytes
        while ($remaining -gt 0) {
            $take = [int][Math]::Min([uint64]$bufferBytes, $remaining)
            $read = $sourceStream.Read($buffer, 0, $take)
            if ($read -le 0) {
                throw "Source ended during partition-region clone"
            }
            $targetStream.Write($buffer, 0, $read)
            $remaining -= [uint64]$read
        }
        $targetStream.Flush()
    }
    finally {
        $targetStream.Dispose()
        $sourceStream.Dispose()
    }
}

function Assert-ByteArrayEqual {
    param(
        [Parameter(Mandatory = $true)]
        [byte[]]$Expected,
        [Parameter(Mandatory = $true)]
        [byte[]]$Actual,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Assert-CertificationCondition -Condition ($Expected.Length -eq $Actual.Length) -Message $Message
    for ($i = 0; $i -lt $Expected.Length; $i++) {
        if ($Expected[$i] -ne $Actual[$i]) {
            throw "$Message at byte $i"
        }
    }
}

function Assert-EvidenceContract {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [object]$Evidence
    )

    # A scenario is only "Passed" when it produced the exact evidence its matrix entry
    # requires. Without this the matrix's required_evidence_keys / required_evidence_values
    # are decorative and any non-throwing body -- including one returning $null or an
    # incomplete hashtable -- would still be recorded as Passed.
    $spec = Get-CertificationScenarioSpec -Id $Id
    if ($null -eq $spec) {
        throw "No certification matrix entry for scenario '$Id'"
    }
    if ($null -eq $Evidence) {
        throw "Scenario '$Id' produced no evidence"
    }

    foreach ($key in @($spec.required_evidence_keys)) {
        $present = $false
        $value = $null
        if ($Evidence -is [System.Collections.IDictionary]) {
            $present = $Evidence.Contains($key)
            if ($present) {
                $value = $Evidence[$key]
            }
        }
        else {
            $prop = $Evidence.PSObject.Properties[$key]
            if ($null -ne $prop) {
                $present = $true
                $value = $prop.Value
            }
        }
        if (-not $present -or $null -eq $value) {
            throw "Scenario '$Id' is missing required evidence key '$key'"
        }
    }

    $valuesProp = $spec.PSObject.Properties["required_evidence_values"]
    if ($null -ne $valuesProp -and $null -ne $valuesProp.Value) {
        foreach ($valueSpec in $valuesProp.Value.PSObject.Properties) {
            $key = $valueSpec.Name
            $allowed = @($valueSpec.Value.allowed_values)
            $actual = if ($Evidence -is [System.Collections.IDictionary]) {
                $Evidence[$key]
            }
            else {
                $Evidence.$key
            }
            if ($allowed -notcontains $actual) {
                throw "Scenario '$Id' evidence '$key'='$actual' is not an allowed value"
            }
        }
    }
}

function Invoke-VhdScenario {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
    )

    if (-not [string]::IsNullOrWhiteSpace($Script:VhdSafetyStopReason)) {
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Skipped" -Message $Script:VhdSafetyStopReason)
        return
    }
    if (-not [string]::IsNullOrWhiteSpace($VhdScenarioId) -and $Id -cne $VhdScenarioId) {
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Skipped" -Message "Not selected by focused VHD scenario run: $VhdScenarioId")
        return
    }
    if (-not $RunVhdDataDiskMatrix) {
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Skipped" -Message "Run with -RunVhdDataDiskMatrix to mutate disposable VHD media.")
        return
    }
    if (-not (Test-Administrator)) {
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Skipped" -Message "Administrator shell required for VHD attach and disk mutation.")
        return
    }

    try {
        $evidence = & $Body
        Assert-EvidenceContract -Id $Id -Evidence $evidence
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Passed" -Evidence $evidence)
    }
    catch {
        $failureEvidence = @{ stack = $_.ScriptStackTrace }
        if ($_.Exception.Data.Contains("certification_evidence")) {
            $failureEvidence.controller_restore = $_.Exception.Data["certification_evidence"]
        }
        if ($_.Exception.Data.Contains("fat_conversion_evidence")) {
            $failureEvidence.fat_conversion = $_.Exception.Data["fat_conversion_evidence"]
        }
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Failed" -Message $_.Exception.Message -Evidence $failureEvidence)
    }
}

function Invoke-CreateFormatResizeDeleteScenario {
    $vhd = New-DisposableVhdDisk -Name "create-format-resize-delete"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $vhd.disk_number -Size $CreatePartitionBytes -AssignDriveLetter -ErrorAction Stop
        $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
        Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKCERT" -Confirm:$false -Force -ErrorAction Stop | Out-Null
        Resize-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -Size $ResizePartitionBytes -ErrorAction Stop
        $resized = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
        Assert-CertificationCondition -Condition ([Math]::Abs([int64]$resized.Size - [int64]$ResizePartitionBytes) -le $SizeToleranceBytes) -Message "Resize verification failed"
        Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -Confirm:$false -ErrorAction Stop
        $remaining = @(Get-SakDiskPartitions -DiskNumber $vhd.disk_number |
            Where-Object { $_.Type -ne "Reserved" })
        Assert-CertificationCondition -Condition ($remaining.Count -eq 0) -Message "Delete verification failed: data partition remained"
        return @{
            disk_number = $vhd.disk_number
            initial_size_bytes = $CreatePartitionBytes
            resized_size_bytes = [uint64]$resized.Size
            remaining_partitions = $remaining.Count
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-FatToNtfsScenario {
    $vhd = New-DisposableVhdDisk -Name "fat32-to-ntfs"
    $state = @{ cleanup_allowed = $true }
    $failure = $null
    $evidence = $null
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $vhd.disk_number -Size $ConvertPartitionBytes -AssignDriveLetter -ErrorAction Stop
        $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
        $volumeLabel = "SAKFAT"
        Format-Volume -DriveLetter $letter -FileSystem FAT32 -NewFileSystemLabel $volumeLabel -Confirm:$false -Force -ErrorAction Stop | Out-Null
        Invoke-FatVolumeConversion -DriveLetter $letter -VolumeLabel $volumeLabel -State $state | Out-Null
        $volume = Get-Volume -DriveLetter $letter -ErrorAction Stop
        Assert-CertificationCondition -Condition ($volume.FileSystem -eq "NTFS") -Message "FAT32 to NTFS verification failed"
        $evidence = @{
            disk_number = $vhd.disk_number
            drive_letter = $letter
            final_file_system = $volume.FileSystem
        }
    }
    catch { $failure = $_ }
    $cleanupError = ''
    if ($state.cleanup_allowed) {
        try { Remove-DisposableVhdDisk -Vhd $vhd }
        catch { $cleanupError = $_.Exception.Message }
    } else {
        $Script:VhdSafetyStopReason = "Conversion completion is unverified; VHD remains attached and preserved: $($vhd.path). Further destructive scenarios refused."
    }
    if ($null -ne $failure -or $cleanupError -ne '') {
        $message = if ($null -ne $failure) { $failure.Exception.Message } else { 'Conversion fixture cleanup failed' }
        if ($cleanupError -ne '') { $message += "; cleanup: $cleanupError" }
        if (-not $state.cleanup_allowed) { $message += "; $Script:VhdSafetyStopReason" }
        $inner = if ($null -ne $failure) { $failure.Exception } else { $null }
        $exception = [InvalidOperationException]::new($message, $inner)
        $exception.Data['fat_conversion_evidence'] = @{
            vhd_path = $vhd.path; disk_number = $vhd.disk_number
            cleanup_allowed = $state.cleanup_allowed; cleanup_error = $cleanupError
            preservation_reason = $Script:VhdSafetyStopReason
        }
        throw $exception
    }
    return $evidence
}

function Invoke-QuickPartitionScenario {
    $vhd = New-DisposableVhdDisk -Name "quick-partition"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        New-Partition -DiskNumber $vhd.disk_number -Size 64MB -ErrorAction Stop | Out-Null
        $parts = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Sort-Object PartitionNumber -Descending)
        foreach ($part in $parts) {
            Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $part.PartitionNumber -Confirm:$false -ErrorAction Stop
        }

        $disk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        $usableBytes = [uint64]([Math]::Floor(($disk.Size - $QuickPartitionReservedBytes) / 1MB) * 1MB)
        $sliceBytes = [uint64]([Math]::Floor(($usableBytes / $QuickPartitionCount) / 1MB) * 1MB)
        $offsetBytes = [uint64]1MB
        $created = @()
        for ($i = 0; $i -lt $QuickPartitionCount; $i++) {
            $sizeBytes = if ($i -eq ($QuickPartitionCount - 1)) {
                [uint64]($usableBytes - ($sliceBytes * $i))
            }
            else {
                $sliceBytes
            }
            $partition = New-Partition -DiskNumber $vhd.disk_number -Offset $offsetBytes -Size $sizeBytes -AssignDriveLetter -GptType $BasicDataGptType -ErrorAction Stop
            $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
            Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKQP$($i + 1)" -Confirm:$false -Force -ErrorAction Stop | Out-Null
            $created += [pscustomobject]@{ partition_number = $partition.PartitionNumber; size_bytes = [uint64]$partition.Size; drive_letter = $letter }
            $offsetBytes += $sizeBytes
        }

        $finalPartitions = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Where-Object { $_.Type -ne "Reserved" })
        Assert-CertificationCondition -Condition ($finalPartitions.Count -eq $QuickPartitionCount) -Message "Quick Partition did not create expected partition count"
        $expectedSizes = @($created | ForEach-Object { [uint64]$_.size_bytes })
        for ($i = 0; $i -lt $finalPartitions.Count; $i++) {
            Assert-CertificationCondition -Condition ([Math]::Abs([int64]$finalPartitions[$i].Size - [int64]$expectedSizes[$i]) -le $SizeToleranceBytes) -Message "Quick Partition size verification failed"
        }

        return @{
            disk_number = $vhd.disk_number
            partition_count = $finalPartitions.Count
            expected_slice_bytes = $sliceBytes
            created = $created
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-AdjacentExtendScenario {
    $vhd = New-DisposableVhdDisk -Name "adjacent-extend"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $vhd.disk_number -Size $CreatePartitionBytes -AssignDriveLetter -ErrorAction Stop
        $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
        Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKEXT" -Confirm:$false -Force -ErrorAction Stop | Out-Null
        $scratch = New-Partition -DiskNumber $vhd.disk_number -Size 64MB -ErrorAction Stop
        Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $scratch.PartitionNumber -Confirm:$false -ErrorAction Stop
        Resize-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -Size $ResizePartitionBytes -ErrorAction Stop
        $extended = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
        Assert-CertificationCondition -Condition ([Math]::Abs([int64]$extended.Size - [int64]$ResizePartitionBytes) -le $SizeToleranceBytes) -Message "Adjacent extend verification failed"
        return @{
            disk_number = $vhd.disk_number
            partition_number = $partition.PartitionNumber
            initial_size_bytes = $CreatePartitionBytes
            extended_size_bytes = [uint64]$extended.Size
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-QuickPartitionCustomScenario {
    $vhd = New-DisposableVhdDisk -Name "quick-partition-custom"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        New-Partition -DiskNumber $vhd.disk_number -Size 64MB -ErrorAction Stop | Out-Null
        $parts = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Sort-Object PartitionNumber -Descending)
        foreach ($part in $parts) {
            Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $part.PartitionNumber -Confirm:$false -ErrorAction Stop
        }

        $offsetBytes = [uint64]1MB
        $created = @()
        for ($i = 0; $i -lt $QuickPartitionCustomSizes.Count; $i++) {
            $sizeBytes = [uint64]$QuickPartitionCustomSizes[$i]
            $label = "SAKQC$($i + 1)"
            $partition = New-Partition -DiskNumber $vhd.disk_number -Offset $offsetBytes -Size $sizeBytes -AssignDriveLetter -GptType $BasicDataGptType -ErrorAction Stop
            $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
            Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel $label -Confirm:$false -Force -ErrorAction Stop | Out-Null
            $volume = Get-Volume -DriveLetter $letter -ErrorAction Stop
            Assert-CertificationCondition -Condition ($volume.FileSystemLabel -eq $label) -Message "Custom Quick Partition label verification failed"
            $created += [pscustomobject]@{ partition_number = $partition.PartitionNumber; size_bytes = [uint64]$partition.Size; drive_letter = $letter; label = $label }
            $offsetBytes += $sizeBytes
        }

        $finalPartitions = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Where-Object { $_.Type -ne "Reserved" } |
            Sort-Object Offset)
        Assert-CertificationCondition -Condition ($finalPartitions.Count -eq $QuickPartitionCustomSizes.Count) -Message "Custom Quick Partition did not create expected partition count"
        for ($i = 0; $i -lt $QuickPartitionCustomSizes.Count; $i++) {
            Assert-CertificationCondition -Condition ([Math]::Abs([int64]$finalPartitions[$i].Size - [int64]$QuickPartitionCustomSizes[$i]) -le $SizeToleranceBytes) -Message "Custom Quick Partition size verification failed"
        }

        return @{
            disk_number = $vhd.disk_number
            partition_count = $finalPartitions.Count
            requested_sizes = $QuickPartitionCustomSizes
            created = $created
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-QuickPartitionMbrScenario {
    $vhd = New-DisposableVhdDisk -Name "quick-partition-mbr"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle MBR -ErrorAction Stop
        New-Partition -DiskNumber $vhd.disk_number -Size 64MB -ErrorAction Stop | Out-Null
        $parts = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Sort-Object PartitionNumber -Descending)
        foreach ($part in $parts) {
            Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $part.PartitionNumber -Confirm:$false -ErrorAction Stop
        }

        $disk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        $usableBytes = [uint64]([Math]::Floor(($disk.Size - $QuickPartitionReservedBytes) / 1MB) * 1MB)
        $sliceBytes = [uint64]([Math]::Floor(($usableBytes / $QuickPartitionMbrCount) / 1MB) * 1MB)
        $offsetBytes = [uint64]1MB
        $created = @()
        for ($i = 0; $i -lt $QuickPartitionMbrCount; $i++) {
            $sizeBytes = if ($i -eq ($QuickPartitionMbrCount - 1)) {
                [uint64]($usableBytes - ($sliceBytes * $i))
            }
            else {
                $sliceBytes
            }
            $partition = New-Partition -DiskNumber $vhd.disk_number -Offset $offsetBytes -Size $sizeBytes -AssignDriveLetter -MbrType IFS -ErrorAction Stop
            $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
            Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKQM$($i + 1)" -Confirm:$false -Force -ErrorAction Stop | Out-Null
            $created += [pscustomobject]@{ partition_number = $partition.PartitionNumber; size_bytes = [uint64]$partition.Size; drive_letter = $letter }
            $offsetBytes += $sizeBytes
        }

        $finalDisk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        $finalPartitions = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction Stop |
            Where-Object { $_.Type -ne "Reserved" } |
            Sort-Object Offset)
        $dataPartitions = @($finalPartitions | Where-Object { $_.Type -ne "Extended" })
        $extendedContainers = @($finalPartitions | Where-Object { $_.Type -eq "Extended" })
        Assert-CertificationCondition -Condition ($finalDisk.PartitionStyle -eq "MBR") -Message "Quick Partition MBR style verification failed"
        $partitionSummary = ($finalPartitions | ForEach-Object {
            "P$($_.PartitionNumber):$($_.Type):$($_.Size)"
        }) -join ", "
        Assert-CertificationCondition -Condition ($dataPartitions.Count -eq $QuickPartitionMbrCount) -Message "Quick Partition MBR expected four data partitions, found $($dataPartitions.Count): $partitionSummary"
        $expectedSizes = @($created | ForEach-Object { [uint64]$_.size_bytes })
        for ($i = 0; $i -lt $dataPartitions.Count; $i++) {
            Assert-CertificationCondition -Condition ([Math]::Abs([int64]$dataPartitions[$i].Size - [int64]$expectedSizes[$i]) -le $SizeToleranceBytes) -Message "Quick Partition MBR size verification failed"
        }

        return @{
            disk_number = $vhd.disk_number
            partition_style = $finalDisk.PartitionStyle
            partition_count = $dataPartitions.Count
            expected_slice_bytes = $sliceBytes
            created = $created
            extended_container_count = $extendedContainers.Count
            partition_types = @($finalPartitions | ForEach-Object { $_.Type.ToString() })
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-RecoveredPartitionRestoreScenario {
    $vhd = New-DisposableVhdDisk -Name "recovered-partition-restore"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $vhd.disk_number -Size $RecoveryPartitionBytes -GptType $BasicDataGptType -ErrorAction Stop
        $offset = [uint64]$partition.Offset
        $size = [uint64]$partition.Size
        Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -Confirm:$false -ErrorAction Stop
        $restored = New-Partition -DiskNumber $vhd.disk_number -Offset $offset -Size $size -GptType $BasicDataGptType -ErrorAction Stop
        Assert-CertificationCondition -Condition ([uint64]$restored.Offset -eq $offset) -Message "Recovered partition offset mismatch"
        Assert-CertificationCondition -Condition ([uint64]$restored.Size -eq $size) -Message "Recovered partition size mismatch"
        return @{
            disk_number = $vhd.disk_number
            offset_bytes = $offset
            size_bytes = $size
            type_id = $BasicDataGptType
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-EmptyStyleConversionScenario {
    $vhd = New-DisposableVhdDisk -Name "empty-style-conversion"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        Clear-Disk -Number $vhd.disk_number -RemoveData -Confirm:$false -ErrorAction Stop
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle MBR -ErrorAction Stop
        $mbrDisk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        Assert-CertificationCondition -Condition ($mbrDisk.PartitionStyle -eq "MBR") -Message "MBR conversion verification failed"
        Clear-Disk -Number $vhd.disk_number -RemoveData -Confirm:$false -ErrorAction Stop
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $gptDisk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        Assert-CertificationCondition -Condition ($gptDisk.PartitionStyle -eq "GPT") -Message "GPT conversion verification failed"
        return @{
            disk_number = $vhd.disk_number
            final_partition_style = $gptDisk.PartitionStyle.ToString()
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-ClearDiskWipeScenario {
    $vhd = New-DisposableVhdDisk -Name "clear-disk-wipe"
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        New-Partition -DiskNumber $vhd.disk_number -Size $CreatePartitionBytes -ErrorAction Stop | Out-Null
        Clear-Disk -Number $vhd.disk_number -RemoveData -Confirm:$false -ErrorAction Stop
        $disk = Get-Disk -Number $vhd.disk_number -ErrorAction Stop
        $partitions = @(Get-SakDiskPartitions -DiskNumber $vhd.disk_number)
        Assert-CertificationCondition -Condition ($disk.PartitionStyle -eq "RAW") -Message "Clear-Disk did not leave RAW disk"
        Assert-CertificationCondition -Condition ($partitions.Count -eq 0) -Message "Clear-Disk left partitions behind"
        return @{
            disk_number = $vhd.disk_number
            final_partition_style = $disk.PartitionStyle.ToString()
            remaining_partitions = $partitions.Count
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
}

function Invoke-VhdImageCloneScenario {
    $source = New-DisposableVhdDisk -Name "image-clone-source"
    $targetPath = Join-Path $Script:RunRoot "image-clone-target.vhdx"
    $target = $null
    try {
        Initialize-Disk -Number $source.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $source.disk_number -UseMaximumSize -AssignDriveLetter -ErrorAction Stop
        $letter = Get-CertificationDriveLetter -DiskNumber $source.disk_number -PartitionNumber $partition.PartitionNumber
        Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel "SAKCLONE" -Confirm:$false -Force -ErrorAction Stop | Out-Null
        $sentinelPath = "$letter`:\sak-partition-manager-certification.txt"
        "partition-manager-certification" | Set-Content -LiteralPath $sentinelPath -Encoding ASCII
        Dismount-DiskImage -ImagePath $source.path -ErrorAction Stop | Out-Null

        Copy-Item -LiteralPath $source.path -Destination $targetPath -Force
        Mount-DiskImage -ImagePath $targetPath -ErrorAction Stop | Out-Null
        $targetDisk = Get-AttachedVhdDisk -Path $targetPath
        $target = [pscustomobject]@{ path = $targetPath; disk_number = [int]$targetDisk.Number; name = "image-clone-target" }
        $targetPartition = Get-Partition -DiskNumber $target.disk_number | Where-Object { $_.Type -ne "Reserved" } | Select-Object -First 1
        $targetLetter = Get-CertificationDriveLetter -DiskNumber $target.disk_number -PartitionNumber $targetPartition.PartitionNumber
        $targetSentinel = "$targetLetter`:\sak-partition-manager-certification.txt"
        $content = Get-Content -LiteralPath $targetSentinel -Raw -ErrorAction Stop
        Assert-CertificationCondition -Condition ($content.Trim() -eq "partition-manager-certification") -Message "Image clone sentinel verification failed"
        return @{
            source_image = $source.path
            target_image = $targetPath
            target_disk_number = $target.disk_number
            sentinel_verified = $true
        }
    }
    finally {
        if ($null -ne $target) {
            Remove-DisposableVhdDisk -Vhd $target
        }
        Remove-DisposableVhdDisk -Vhd $source
        if (-not $KeepVhd -and (Test-Path -LiteralPath $targetPath)) {
            Remove-Item -LiteralPath $targetPath -Force
        }
    }
}

function Assert-ControllerRestoreBoolean {
    param($Value, [bool]$Expected, [string]$Name)
    Assert-CertificationCondition ($Value -is [bool] -and $Value -eq $Expected) "Controller report '$Name' must be boolean $Expected"
}

function Test-ControllerRestorePath {
    param($Actual, [string]$Expected)
    if ($Actual -isnot [string] -or $Actual -notmatch '^[A-Za-z]:[\\/]') { return $false }
    return [System.IO.Path]::GetFullPath($Actual) -ieq [System.IO.Path]::GetFullPath($Expected)
}

function Resolve-ControllerCertifier {
    Assert-CertificationCondition (-not [string]::IsNullOrWhiteSpace($ControllerCertifierPath)) "ControllerCertifierPath is required for vhd.image-restore; no surrogate restore is supported"
    $path = (Resolve-Path -LiteralPath (Resolve-ProjectPath $ControllerCertifierPath) -ErrorAction Stop).Path
    Assert-CertificationCondition (Test-Path -LiteralPath $path -PathType Leaf) "Controller certifier is not a file"
    Assert-CertificationCondition ([System.IO.Path]::GetExtension($path) -ieq ".exe") "Controller certifier must be a Windows executable"
    $manifest = Join-Path (Split-Path -Parent $path) "partition_manager_live_certifier.sources.txt"
    Assert-CertificationCondition (Test-Path -LiteralPath $manifest -PathType Leaf) "Controller source manifest is missing"
    return @{
        path = $path
        binary_hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        source_digest = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash
    }
}

function New-ControllerRestoreDirectory {
    param([string]$RunId)
    $root = (Resolve-Path -LiteralPath $Script:RunRoot -ErrorAction Stop).Path
    Assert-CertificationCondition ($root -match '^[A-Za-z]:[\\/]') "Controller fixtures require a guest-local drive-qualified directory"
    $ancestor = Get-Item -LiteralPath $root -Force -ErrorAction Stop
    while ($null -ne $ancestor) {
        Assert-CertificationCondition (($ancestor.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) "Controller fixture directory has a reparse ancestor"
        $ancestor = $ancestor.Parent
    }
    $path = Join-Path $root ("controller-restore-" + $RunId)
    Assert-CertificationCondition (-not (Test-Path -LiteralPath $path)) "Controller fixture directory already exists"
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}

function Get-ControllerFixtureDisk {
    param([object]$Vhd)
    $disk = @(Get-AttachedVhdDisk -Path $Vhd.path)
    Assert-CertificationCondition ($disk.Count -eq 1) "Disposable VHD disk mapping is ambiguous"
    Assert-ControllerRestoreBoolean $disk[0].IsBoot $false "fixture.IsBoot"
    Assert-ControllerRestoreBoolean $disk[0].IsSystem $false "fixture.IsSystem"
    Assert-CertificationCondition ([uint64]$disk[0].Size -eq ([uint64]$Vhd.size_mb * 1MB)) "Disposable VHD size mismatch"
    return $disk[0]
}

function Initialize-ControllerRestoreVolume {
    param([object]$Vhd, [string]$Label, [string]$MarkerName, [string]$Marker)
    $disk = Get-ControllerFixtureDisk $Vhd
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT -ErrorAction Stop | Out-Null
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter -ErrorAction Stop
    $letter = Get-CertificationDriveLetter -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber
    Format-Volume -DriveLetter $letter -FileSystem NTFS -NewFileSystemLabel $Label -Confirm:$false -Force -ErrorAction Stop | Out-Null
    $path = Join-Path ($letter + ':\') $MarkerName
    Set-Content -LiteralPath $path -Value $Marker -Encoding ASCII -ErrorAction Stop
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Assert-ControllerRestoreIdentity {
    param([object]$Report, [hashtable]$State)
    Assert-CertificationCondition ($Report -is [pscustomobject]) "Controller evidence must be a JSON object"
    Assert-CertificationCondition ($Report.tool -ceq "partition-manager-controller-live-certifier" -and $Report.schema_version -eq 1) "Controller report tool/schema mismatch"
    Assert-CertificationCondition ($Report.scenario_id -ceq "vhd.image-restore" -and $Report.run_id -ceq $State.run_id) "Controller report scenario/run identity mismatch"
    Assert-CertificationCondition ((Test-ControllerRestorePath $Report.source_vhd $State.source.path) -and (Test-ControllerRestorePath $Report.target_vhd $State.target.path)) "Controller report VHD paths mismatch"
    Assert-CertificationCondition ($Report.certifier_binary_sha256 -is [string] -and $Report.certifier_binary_sha256 -ieq $State.certifier.binary_hash) "Controller report executable hash mismatch"
    Assert-CertificationCondition ($Report.source_digest -is [string] -and $Report.source_digest -ieq $State.certifier.source_digest) "Controller report raw source manifest hash mismatch"
    Assert-ControllerRestoreBoolean $Report.validation_only $false "validation_only"
}

function Read-ControllerRestoreReport {
    param([hashtable]$State, [object]$Captured)
    Assert-CertificationCondition (-not $Captured.timed_out) "Controller certifier exceeded outer deadline; preserve VHDs"
    Assert-CertificationCondition (Test-Path -LiteralPath $State.report_path -PathType Leaf) "Controller report is missing; preserve VHDs"
    $length = (Get-Item -LiteralPath $State.report_path -ErrorAction Stop).Length
    Assert-CertificationCondition ($length -gt 0 -and $length -le $MaximumControllerReportBytes) "Controller report size is invalid; preserve VHDs"
    $report = Get-Content -LiteralPath $State.report_path -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json
    $stdoutReport = $Captured.stdout | ConvertFrom-Json
    Assert-CertificationCondition ($stdoutReport -is [pscustomobject]) "Controller stdout must contain one JSON report"
    $fileJson = $report | ConvertTo-Json -Depth 100 -Compress
    $stdoutJson = $stdoutReport | ConvertTo-Json -Depth 100 -Compress
    Assert-CertificationCondition ($fileJson -ceq $stdoutJson) "Controller stdout and stored report disagree; preserve VHDs"
    Assert-ControllerRestoreIdentity $report $State
    $State.report = $report
    Assert-ControllerRestoreBoolean $report.worker_descendants_finished $true "worker_descendants_finished"
    # Cleanup authority is independent of Passed: a correlated, stopped failure can be cleaned.
    $State.cleanup_allowed = $true
    $State.worker_descendants_finished = $true
    $State.preservation_reason = ""
    return $report
}

function Assert-ControllerRestoreOperations {
    param([object]$Report)
    Assert-CertificationCondition ($Report.operations -is [System.Array] -and @($Report.operations).Count -eq 2) "Controller must record exactly two operations"
    $types = @("Create Image", "Restore Image")
    for ($index = 0; $index -lt $types.Count; $index++) {
        $operation = $Report.operations[$index]
        Assert-CertificationCondition ($operation.type -ceq $types[$index]) "Controller operation order/type mismatch"
        Assert-CertificationCondition (-not [string]::IsNullOrWhiteSpace($operation.operation_id)) "Controller operation identity is missing"
        Assert-CertificationCondition ($operation.queued_script_sha256 -match '^[0-9a-fA-F]{64}$') "Queued script digest is missing"
        Assert-CertificationCondition (-not [string]::IsNullOrWhiteSpace($operation.review_fingerprint) -and -not [string]::IsNullOrWhiteSpace($operation.layout_hash)) "Reviewed queue provenance is missing"
        Assert-ControllerRestoreBoolean $operation.execution.success $true "execution.success"
        Assert-ControllerRestoreBoolean $operation.execution.dry_run $false "execution.dry_run"
        Assert-CertificationCondition ($operation.execution.steps -is [System.Array] -and @($operation.execution.steps).Count -eq 1) "Operation must have exactly one execution step"
        $step = $operation.execution.steps[0]
        Assert-CertificationCondition ($step.operation_id -ceq $operation.operation_id) "Execution step identity mismatch"
        Assert-ControllerRestoreBoolean $step.success $true "step.success"
        Assert-ControllerRestoreBoolean $step.skipped $false "step.skipped"
    }
    Assert-CertificationCondition ($Report.operations[0].operation_id -cne $Report.operations[1].operation_id) "Controller operation identities are duplicated"
}

function Invoke-ControllerRestore {
    param([hashtable]$State)
    $arguments = @(
        "--source-vhd", $State.source.path, "--target-vhd", $State.target.path,
        "--output-dir", $State.directory, "--run-id", $State.run_id, "--confirm-disposable-vhds"
    )
    $captureOptions = @{
        FilePath = $State.certifier.path
        Arguments = $arguments
        WorkingDirectory = $State.directory
        TimeoutSeconds = $ControllerCertifierTimeoutSeconds
    }
    $State.cleanup_allowed = $false
    $State.preservation_reason = "Controller launch began; descendant completion is not yet confirmed"
    $captured = Invoke-CapturedProcess @captureOptions
    Set-Content -LiteralPath (Join-Path $State.directory "controller.stdout.log") -Value $captured.stdout -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $State.directory "controller.stderr.log") -Value $captured.stderr -Encoding UTF8
    $report = Read-ControllerRestoreReport $State $captured
    Assert-CertificationCondition ($captured.exit_code -eq 0 -and $report.status -ceq "Passed") "Controller restore failed: $($report.error)"
    Assert-CertificationCondition ($report.execution_engine -ceq "PartitionManagerController") "Unexpected restore execution engine"
    Assert-ControllerRestoreBoolean $report.controller_executed $true "controller_executed"
    Assert-ControllerRestoreBoolean $report.source_dismounted_before_restore $true "source_dismounted_before_restore"
    Assert-ControllerRestoreBoolean $report.sentinel_verified $true "sentinel_verified"
    Assert-ControllerRestoreBoolean $report.target_overwritten $true "target_overwritten"
    Assert-ControllerRestoreOperations $report
    $actualBinaryHash = (Get-FileHash -LiteralPath $State.certifier.path -Algorithm SHA256).Hash
    Assert-CertificationCondition ($actualBinaryHash -ieq $State.certifier.binary_hash) "Controller executable changed during execution"
}

function Test-ControllerRestoredTarget {
    param([hashtable]$State)
    $sourceImage = Get-DiskImage -ImagePath $State.source.path -ErrorAction Stop
    Assert-CertificationCondition (-not $sourceImage.Attached) "Source VHD must stay detached during restored-target verification"
    Dismount-SakDiskImageIfAttached -Path $State.target.path
    Mount-DiskImage -ImagePath $State.target.path -ErrorAction Stop | Out-Null
    $disk = Get-ControllerFixtureDisk $State.target
    $expected = $State.report.target_after
    Assert-CertificationCondition (-not [string]::IsNullOrWhiteSpace($expected.disk_unique_id) -and [string]$disk.UniqueId -ieq $expected.disk_unique_id) "Reattached target disk identity differs from controller result"
    Assert-CertificationCondition (Test-ControllerRestorePath $expected.vhd_path $State.target.path) "Controller target attachment path mismatch"
    $volumes = @(Get-Partition -DiskNumber $disk.Number -ErrorAction Stop | ForEach-Object {
        $volume = Get-SakPartitionVolume -Partition $_
        if ($null -ne $volume -and $volume.FileSystemLabel -ceq "SAKRESTSRC" -and $volume.FileSystem -ieq "NTFS") {
            [pscustomobject]@{ partition = $_; volume = $volume }
        }
    })
    Assert-CertificationCondition ($volumes.Count -eq 1) "Restored target must contain exactly one source-labelled NTFS volume"
    $letter = Get-CertificationDriveLetter -DiskNumber $disk.Number -PartitionNumber $volumes[0].partition.PartitionNumber
    $root = $letter + ':\'
    $sentinelPath = Join-Path $root "sak-partition-manager-restore.txt"
    $content = Get-Content -LiteralPath $sentinelPath -Raw -ErrorAction Stop
    $hash = (Get-FileHash -LiteralPath $sentinelPath -Algorithm SHA256).Hash
    Assert-CertificationCondition ($content.Trim() -ceq $State.source_marker -and $hash -ieq $State.source_marker_hash) "Independent restored sentinel content/hash mismatch"
    Assert-CertificationCondition ($hash -ieq $State.report.sentinel_sha256) "Controller and independent sentinel hashes disagree"
    $markerPresent = Test-Path -LiteralPath (Join-Path $root "sak-partition-manager-pre-restore.txt")
    Assert-CertificationCondition (-not $markerPresent) "Independent check found the pre-restore target marker"
    $rawImage = Join-Path $State.directory "controller-source.img"
    Assert-CertificationCondition (Test-ControllerRestorePath $State.report.source_image $rawImage) "Controller raw-image path mismatch"
    $imageHash = (Get-FileHash -LiteralPath $rawImage -Algorithm SHA256).Hash
    Assert-CertificationCondition ($imageHash -ieq $State.report.source_image_sha256) "Independent raw-image digest mismatch"
    $State.target.disk_number = [int]$disk.Number
    return @{
        source_image = $rawImage
        source_image_sha256 = $imageHash
        target_image = $State.target.path
        target_disk_number = [int]$disk.Number
        pre_restore_marker = $State.target_marker
        sentinel_verified = $true
        sentinel_sha256 = $hash
        target_overwritten = $true
        independent_verification = $true
        execution_engine = $State.report.execution_engine
        controller_executed = $true
        worker_descendants_finished = $true
        controller_run_id = $State.run_id
        controller_report = $State.report_reference
        controller_report_sha256 = (Get-FileHash -LiteralPath $State.report_path -Algorithm SHA256).Hash
        source_digest = $State.report.source_digest
        certifier_binary_sha256 = $State.report.certifier_binary_sha256
        create_image_operation_id = $State.report.operations[0].operation_id
        restore_image_operation_id = $State.report.operations[1].operation_id
    }
}

function Complete-ControllerRestoreCleanup {
    param([hashtable]$State)
    $errors = @()
    if (-not $State.cleanup_allowed) {
        $State.preservation_reason = "Descendant completion was not verified; both VHD files/attachments are preserved. Report: $($State.report_path)"
        $Script:VhdSafetyStopReason = $State.preservation_reason + " Further destructive scenarios refused."
        Write-Warning $State.preservation_reason
        return ,$errors
    }
    foreach ($role in @("target", "source")) {
        $vhd = $State[$role]
        if ($null -eq $vhd) { continue }
        try {
            Remove-DisposableVhdDisk -Vhd $vhd
        }
        catch {
            $errors += $role + ": " + $_.Exception.Message
        }
    }
    return ,$errors
}

function Invoke-VhdImageRestoreScenario {
    $certifier = Resolve-ControllerCertifier
    Assert-CertificationCondition ($VhdSizeMB -ge 512 -and $VhdSizeMB -le 1024) "Controller restore fixtures must be 512-1024 MiB"
    $runId = [guid]::NewGuid().ToString("D")
    $directory = New-ControllerRestoreDirectory $runId
    $state = @{
        certifier = $certifier; run_id = $runId; directory = $directory
        source = $null; target = $null; report = $null; cleanup_allowed = $true
        worker_descendants_finished = $null
        report_path = (Join-Path $directory "controller-restore-report.json")
        report_reference = "controller-restore-$runId/controller-restore-report.json"
        source_marker = "partition-manager-restore-certification:$runId"
        target_marker = "target-before-restore:$runId"
        preservation_reason = ""
    }
    $failure = $null
    $evidence = $null
    try {
        $state.source = New-DisposableVhdDisk -Name "image-restore-source" -Directory $directory
        $state.target = New-DisposableVhdDisk -Name "image-restore-target" -Directory $directory
        $state.source_marker_hash = Initialize-ControllerRestoreVolume $state.source "SAKRESTSRC" "sak-partition-manager-restore.txt" $state.source_marker
        Initialize-ControllerRestoreVolume $state.target "SAKRESTDST" "sak-partition-manager-pre-restore.txt" $state.target_marker | Out-Null
        Invoke-ControllerRestore $state
        $evidence = Test-ControllerRestoredTarget $state
    }
    catch {
        $failure = $_
    }
    $cleanupErrors = Complete-ControllerRestoreCleanup $state
    if ($null -ne $failure -or $cleanupErrors.Count -gt 0) {
        $message = if ($null -ne $failure) { $failure.Exception.Message } else { "Controller fixture cleanup failed" }
        if ($cleanupErrors.Count -gt 0) { $message += "; cleanup: " + ($cleanupErrors -join "; ") }
        if (-not $state.cleanup_allowed) { $message += "; " + $state.preservation_reason }
        $exception = [System.InvalidOperationException]::new($message)
        $exception.Data["certification_evidence"] = @{
            report_path = $state.report_path; run_id = $state.run_id
            source_vhd = $state.source.path; target_vhd = $state.target.path
            worker_descendants_finished = $state.worker_descendants_finished
            preservation_reason = $state.preservation_reason; cleanup_errors = $cleanupErrors
            controller_report = $state.report
        }
        throw $exception
    }
    return $evidence
}

function Invoke-PartitionCloneRegionScenario {
    # Acquire both disks INSIDE the try so a failure creating the target cannot leak an
    # already-attached source (the finally below null-guards each disposal).
    $source = $null
    $target = $null
    try {
        $source = New-DisposableVhdDisk -Name "partition-clone-region-source"
        $target = New-DisposableVhdDisk -Name "partition-clone-region-target"
        Initialize-Disk -Number $source.disk_number -PartitionStyle GPT -ErrorAction Stop
        $sourcePartition = New-Partition -DiskNumber $source.disk_number -Size $PartitionCloneBytes -GptType $BasicDataGptType -ErrorAction Stop
        Initialize-Disk -Number $target.disk_number -PartitionStyle GPT -ErrorAction Stop
        $markerPartition = New-Partition -DiskNumber $target.disk_number -Size 32MB -GptType $BasicDataGptType -ErrorAction Stop

        $sourceOffset = [uint64]$sourcePartition.Offset
        $targetMarkerOffset = [uint64]$markerPartition.Offset
        $targetOffset = [uint64]($markerPartition.Offset + $markerPartition.Size + 1MB)
        # Fill the WHOLE clone region with a non-zero pattern (not just a 4 KiB marker) so
        # the read-back below verifies every copied byte, not merely the first block, and
        # corruption anywhere inside the cloned region is caught.
        $sourceSignature = New-CertificationPattern -Text "SAK-PARTITION-CLONE-REGION-SOURCE" -Size $PartitionCloneBytes
        $outsideMarker = New-CertificationPattern -Text "SAK-PARTITION-CLONE-REGION-OUTSIDE"

        Set-CertificationDiskOffline -DiskNumber $source.disk_number
        Set-CertificationDiskOffline -DiskNumber $target.disk_number
        Write-RawCertificationBytes -DiskNumber $source.disk_number -Offset $sourceOffset -Bytes $sourceSignature
        Write-RawCertificationBytes -DiskNumber $target.disk_number -Offset $targetMarkerOffset -Bytes $outsideMarker
        Copy-RawCertificationBytes -SourceDiskNumber $source.disk_number `
            -SourceOffset $sourceOffset `
            -TargetDiskNumber $target.disk_number `
            -TargetOffset $targetOffset `
            -Bytes $PartitionCloneBytes
        $clonedSignature = Read-RawCertificationBytes -DiskNumber $target.disk_number `
            -Offset $targetOffset `
            -Size $sourceSignature.Length
        $preservedMarker = Read-RawCertificationBytes -DiskNumber $target.disk_number `
            -Offset $targetMarkerOffset `
            -Size $outsideMarker.Length
        Assert-CertificationCondition -Condition (Test-ByteArrayHashEqual -Expected $sourceSignature -Actual $clonedSignature) -Message "Partition clone region signature mismatch across the full $($sourceSignature.Length)-byte region"
        Assert-ByteArrayEqual -Expected $outsideMarker -Actual $preservedMarker -Message "Partition clone region overwrote bytes outside target region"

        return @{
            source_disk_number = $source.disk_number
            target_disk_number = $target.disk_number
            source_offset_bytes = $sourceOffset
            target_offset_bytes = $targetOffset
            clone_size_bytes = $PartitionCloneBytes
            signature_verified = $true
            outside_marker_preserved = $true
        }
    }
    finally {
        if ($null -ne $target) {
            Set-CertificationDiskOnline -DiskNumber $target.disk_number
            Remove-DisposableVhdDisk -Vhd $target
        }
        if ($null -ne $source) {
            Set-CertificationDiskOnline -DiskNumber $source.disk_number
            Remove-DisposableVhdDisk -Vhd $source
        }
    }
}

function Get-CertificationGitProvenance {
    param([string]$ProjectRoot)
    $result = [ordered]@{ head = 'unavailable'; dirty = $null; probes = @(); error = '' }
    try {
        $git = Get-Command git.exe -CommandType Application -ErrorAction Stop
        foreach ($operation in @('head', 'status')) {
            $query = if ($operation -eq 'head') { @('rev-parse', 'HEAD') } else { @('status', '--porcelain') }
            $probe = Invoke-CapturedProcess -FilePath $git.Source -Arguments (@('-C', $ProjectRoot) + $query) `
                -WorkingDirectory $ProjectRoot -TimeoutSeconds 10
            $result.probes += $probe
            if ($probe.timed_out -or $probe.exit_code -ne 0) {
                $result.error = "Git $operation probe failed (exit=$($probe.exit_code), timed_out=$($probe.timed_out)): $($probe.stderr)"
                return [pscustomobject]$result
            }
            if ($operation -eq 'head') {
                if ($probe.stdout -cnotmatch '^([0-9a-f]{40}|[0-9a-f]{64})$') {
                    $result.error = 'Git revision probe did not return one canonical object ID; raw output retained in git_probes'
                    return [pscustomobject]$result
                }
                $result.head = $probe.stdout
            } else {
                $result.dirty = -not [string]::IsNullOrWhiteSpace($probe.stdout)
            }
        }
    } catch {
        $result.error = $_.Exception.Message
    }
    return [pscustomobject]$result
}

function Add-ExternalCertificationGates {
    foreach ($gate in @($Script:CertificationMatrix.external_gates)) {
        Add-CertificationResult -Result (New-CertificationResult -Id $gate.id -Name $gate.name -Status "ExternalGate" -Message "Requires disposable VM, hardware, or lab evidence; not run by this VHD harness.")
    }
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    $resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null
    $Script:RunRoot = Join-Path $resolvedOutputRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null

    Invoke-VhdScenario -Id "vhd.create-format-resize-delete" -Name "Create, format, resize, and delete data partition" -Body ${function:Invoke-CreateFormatResizeDeleteScenario}
    Invoke-VhdScenario -Id "vhd.fat32-to-ntfs" -Name "FAT32 to NTFS in-place conversion" -Body ${function:Invoke-FatToNtfsScenario}
    Invoke-VhdScenario -Id "vhd.quick-partition" -Name "Quick Partition equal-size data-disk layout" -Body ${function:Invoke-QuickPartitionScenario}
    Invoke-VhdScenario -Id "vhd.quick-partition-custom" -Name "Quick Partition custom size and label layout" -Body ${function:Invoke-QuickPartitionCustomScenario}
    Invoke-VhdScenario -Id "vhd.quick-partition-mbr" -Name "Quick Partition MBR four-data-partition layout" -Body ${function:Invoke-QuickPartitionMbrScenario}
    Invoke-VhdScenario -Id "vhd.adjacent-extend" -Name "Adjacent free-space Extend Partition Wizard path" -Body ${function:Invoke-AdjacentExtendScenario}
    Invoke-VhdScenario -Id "vhd.recovered-partition-restore" -Name "Recovered partition write-back candidate restore" -Body ${function:Invoke-RecoveredPartitionRestoreScenario}
    Invoke-VhdScenario -Id "vhd.empty-style-conversion" -Name "Empty data disk GPT/MBR conversion" -Body ${function:Invoke-EmptyStyleConversionScenario}
    Invoke-VhdScenario -Id "vhd.clear-disk-wipe" -Name "Clear-level non-system disk wipe" -Body ${function:Invoke-ClearDiskWipeScenario}
    Invoke-VhdScenario -Id "vhd.image-clone" -Name "Offline VHD image clone and sentinel verification" -Body ${function:Invoke-VhdImageCloneScenario}
    Invoke-VhdScenario -Id "vhd.image-restore" -Name "Offline VHD image restore and overwrite verification" -Body ${function:Invoke-VhdImageRestoreScenario}
    Invoke-VhdScenario -Id "vhd.partition-clone-region" -Name "Partition clone to raw target region" -Body ${function:Invoke-PartitionCloneRegionScenario}
    Add-ExternalCertificationGates

    $failed = @($Script:Results | Where-Object { $_.status -eq "Failed" })
    $passed = @($Script:Results | Where-Object { $_.status -eq "Passed" })
    $skipped = @($Script:Results | Where-Object { $_.status -eq "Skipped" })
    $external = @($Script:Results | Where-Object { $_.status -eq "ExternalGate" })
    $skippedVhd = @($skipped | Where-Object { $_.id -like "vhd.*" })
    $status = if ($failed.Count -gt 0) {
        "Failed"
    }
    elseif ($RequireVhdDataDiskEvidence -and $skippedVhd.Count -gt 0) {
        # Fail closed whenever evidence was demanded but a VHD scenario did not produce it,
        # INCLUDING the case where -RunVhdDataDiskMatrix was omitted (every VHD scenario is
        # then Skipped). Requiring the matrix switch here would let a no-evidence run exit 0.
        "Incomplete"
    }
    elseif ($passed.Count -eq 0) {
        "NotRun"
    }
    elseif ($external.Count -gt 0 -or $skipped.Count -gt 0) {
        "Partial"
    }
    else {
        "Passed"
    }

    # Provenance so the report can prove which code and native tools produced the evidence.
    # Values that cannot be resolved are recorded as "unavailable" (honest, non-fatal) rather
    # than omitted; none of this affects the pass/fail decision above.
    $provenance = [ordered]@{
        script_path = $PSCommandPath
        script_sha256 = "unavailable"
        git_head = "unavailable"
        git_dirty = $null
        git_probes = @()
        git_error = ""
        os_version = [System.Environment]::OSVersion.Version.ToString()
        clr_version = [System.Environment]::Version.ToString()
        powershell_version = $PSVersionTable.PSVersion.ToString()
        diskpart_path = "unavailable"
        diskpart_sha256 = "unavailable"
        convert_path = "unavailable"
        convert_sha256 = "unavailable"
    }
    try {
        $provenance.script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    }
    catch {
        $provenance.script_sha256 = "unavailable: $($_.Exception.Message)"
    }
    # Optional Git probes use captured streams, not the producer's native exit/error channel.
    # Non-Git source packages retain the actual diagnostic without changing scenario status.
    $gitProvenance = Get-CertificationGitProvenance -ProjectRoot $projectRoot
    $provenance.git_head = $gitProvenance.head
    $provenance.git_dirty = $gitProvenance.dirty
    $provenance.git_probes = $gitProvenance.probes
    $provenance.git_error = $gitProvenance.error
    try {
        $diskpartToolPath = Resolve-SystemToolPath -Name "diskpart.exe"
        $provenance.diskpart_path = $diskpartToolPath
        $provenance.diskpart_sha256 = (Get-FileHash -LiteralPath $diskpartToolPath -Algorithm SHA256).Hash
        $convertToolPath = Resolve-SystemToolPath -Name "convert.exe"
        $provenance.convert_path = $convertToolPath
        $provenance.convert_sha256 = (Get-FileHash -LiteralPath $convertToolPath -Algorithm SHA256).Hash
    }
    catch {
        $provenance.native_tool_error = $_.Exception.Message
    }

    $report = [ordered]@{
        tool = "partition-manager-destructive-certification"
        schema_version = 1
        status = $status
        started_utc = $Script:StartedUtc.ToString("o")
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME
        project_root = $projectRoot
        output_root = $Script:RunRoot
        certification_matrix = "certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        run_vhd_data_disk_matrix = [bool]$RunVhdDataDiskMatrix
        elevated_relaunch_requested = [bool]$RelaunchElevated
        require_vhd_data_disk_evidence = [bool]$RequireVhdDataDiskEvidence
        keep_vhd = [bool]$KeepVhd
        selected_vhd_scenario = $VhdScenarioId
        controller_certifier_path = $ControllerCertifierPath
        vhd_size_mb = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
        administrator = Test-Administrator
        provenance = $provenance
        summary = [ordered]@{
            passed = $passed.Count
            failed = $failed.Count
            skipped = $skipped.Count
            external_gates = $external.Count
        }
        results = $Script:Results
    }

    $reportPath = Join-Path $Script:RunRoot "partition-manager-certification-report.json"
    $report | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Host "Partition Manager certification report: $reportPath"

    if ($failed.Count -gt 0) {
        exit 1
    }
    if ($RequireVhdDataDiskEvidence -and $skippedVhd.Count -gt 0) {
        Write-Host "VHD data-disk evidence required, but $($skippedVhd.Count) VHD scenario(s) were skipped (was -RunVhdDataDiskMatrix supplied, elevated?)." -ForegroundColor Red
        exit 2
    }
    # This script owns its return contract; optional native metadata and the caller's earlier
    # LASTEXITCODE must not leak into a successfully generated report's completion status.
    exit 0
}
finally {
    Pop-Location
}
