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
    [int]$VhdSizeMB = 768
)

$ErrorActionPreference = "Stop"

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
    $matrixPath = Join-Path (Resolve-ProjectRoot) "docs\PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    if ($matrix.schema_version -ne 1) {
        throw "Unsupported Partition Manager certification matrix schema_version: $($matrix.schema_version)"
    }
    return $matrix
}

$Script:CertificationMatrix = Read-CertificationMatrix

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

    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }

    return $Value
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

function Invoke-DiskPartScript {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Lines
    )

    $scriptPath = Join-Path $Script:RunRoot "diskpart-$([guid]::NewGuid()).txt"
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    $output = & diskpart.exe /s $scriptPath 2>&1
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
        [string]$Name
    )

    $safeName = $Name -replace "[^A-Za-z0-9_.-]", "-"
    $path = Join-Path $Script:RunRoot "$safeName.vhdx"
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }

    $size = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
    Invoke-DiskPartScript -Lines @(
        "create vdisk file=`"$path`" maximum=$size type=expandable",
        "attach vdisk"
    ) | Out-Null

    $disk = Get-AttachedVhdDisk -Path $path
    Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number $disk.Number -IsReadOnly $false -ErrorAction Stop

    [pscustomobject]@{
        path = $path
        disk_number = [int]$disk.Number
        size_mb = $size
        name = $safeName
    }
}

function Remove-DisposableVhdDisk {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Vhd
    )

    try {
        Dismount-DiskImage -ImagePath $Vhd.path -ErrorAction SilentlyContinue | Out-Null
    }
    finally {
        if (-not $KeepVhd -and (Test-Path -LiteralPath $Vhd.path)) {
            Remove-Item -LiteralPath $Vhd.path -Force
        }
    }
}

function Get-FreeCertificationDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue |
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
    $volume = $partition | Get-Volume -ErrorAction SilentlyContinue
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
        [string]$VolumeLabel
    )

    Assert-CertificationCondition -Condition ($DriveLetter -match '^[A-Z]$') -Message "Invalid conversion drive letter"
    $convertPath = Join-Path $env:SystemRoot "System32\convert.exe"
    $processInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $processInfo.FileName = $convertPath
    $processInfo.Arguments = "$DriveLetter`: /FS:NTFS /NoSecurity /X"
    $processInfo.RedirectStandardInput = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::Start($processInfo)
    $process.StandardInput.WriteLine($VolumeLabel)
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $output = @($stdout, $stderr) -join [Environment]::NewLine
    if ($process.ExitCode -ne 0) {
        throw "convert.exe failed with exit code $($process.ExitCode)`n$output"
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

    Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction SilentlyContinue
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
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = $seed[$i % $seed.Length]
    }
    return $bytes
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

function Invoke-VhdScenario {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
    )

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
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Passed" -Evidence $evidence)
    }
    catch {
        Add-CertificationResult -Result (New-CertificationResult -Id $Id -Name $Name -Status "Failed" -Message $_.Exception.Message -Evidence @{ stack = $_.ScriptStackTrace })
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
        $remaining = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction SilentlyContinue |
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
    try {
        Initialize-Disk -Number $vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
        $partition = New-Partition -DiskNumber $vhd.disk_number -Size $ConvertPartitionBytes -AssignDriveLetter -ErrorAction Stop
        $letter = Get-CertificationDriveLetter -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber
        $volumeLabel = "SAKFAT"
        Format-Volume -DriveLetter $letter -FileSystem FAT32 -NewFileSystemLabel $volumeLabel -Confirm:$false -Force -ErrorAction Stop | Out-Null
        Invoke-FatVolumeConversion -DriveLetter $letter -VolumeLabel $volumeLabel | Out-Null
        $volume = Get-Volume -DriveLetter $letter -ErrorAction Stop
        Assert-CertificationCondition -Condition ($volume.FileSystem -eq "NTFS") -Message "FAT32 to NTFS verification failed"
        return @{
            disk_number = $vhd.disk_number
            drive_letter = $letter
            final_file_system = $volume.FileSystem
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $vhd
    }
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
        $partitions = @(Get-Partition -DiskNumber $vhd.disk_number -ErrorAction SilentlyContinue)
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

function Invoke-VhdImageRestoreScenario {
    $source = New-DisposableVhdDisk -Name "image-restore-source"
    $target = New-DisposableVhdDisk -Name "image-restore-target"
    try {
        Initialize-Disk -Number $source.disk_number -PartitionStyle GPT -ErrorAction Stop
        $sourcePartition = New-Partition -DiskNumber $source.disk_number -UseMaximumSize -AssignDriveLetter -ErrorAction Stop
        $sourceLetter = Get-CertificationDriveLetter -DiskNumber $source.disk_number -PartitionNumber $sourcePartition.PartitionNumber
        Format-Volume -DriveLetter $sourceLetter -FileSystem NTFS -NewFileSystemLabel "SAKRESTSRC" -Confirm:$false -Force -ErrorAction Stop | Out-Null
        $sentinelPath = "$sourceLetter`:\sak-partition-manager-restore.txt"
        "partition-manager-restore-certification" | Set-Content -LiteralPath $sentinelPath -Encoding ASCII

        Initialize-Disk -Number $target.disk_number -PartitionStyle GPT -ErrorAction Stop
        $targetPartition = New-Partition -DiskNumber $target.disk_number -UseMaximumSize -AssignDriveLetter -ErrorAction Stop
        $targetLetter = Get-CertificationDriveLetter -DiskNumber $target.disk_number -PartitionNumber $targetPartition.PartitionNumber
        Format-Volume -DriveLetter $targetLetter -FileSystem NTFS -NewFileSystemLabel "SAKRESTDST" -Confirm:$false -Force -ErrorAction Stop | Out-Null
        $preRestoreMarker = "target-before-restore"
        $targetMarkerPath = "$targetLetter`:\sak-partition-manager-pre-restore.txt"
        $preRestoreMarker | Set-Content -LiteralPath $targetMarkerPath -Encoding ASCII

        Dismount-DiskImage -ImagePath $source.path -ErrorAction Stop | Out-Null
        Dismount-DiskImage -ImagePath $target.path -ErrorAction Stop | Out-Null
        Copy-Item -LiteralPath $source.path -Destination $target.path -Force
        Mount-DiskImage -ImagePath $target.path -ErrorAction Stop | Out-Null

        $targetDisk = Get-AttachedVhdDisk -Path $target.path
        $target.disk_number = [int]$targetDisk.Number
        $restoredPartition = Get-Partition -DiskNumber $target.disk_number | Where-Object { $_.Type -ne "Reserved" } | Select-Object -First 1
        $restoredLetter = Get-CertificationDriveLetter -DiskNumber $target.disk_number -PartitionNumber $restoredPartition.PartitionNumber
        $restoredSentinel = "$restoredLetter`:\sak-partition-manager-restore.txt"
        $content = Get-Content -LiteralPath $restoredSentinel -Raw -ErrorAction Stop
        $markerStillExists = Test-Path -LiteralPath "$restoredLetter`:\sak-partition-manager-pre-restore.txt"
        Assert-CertificationCondition -Condition ($content.Trim() -eq "partition-manager-restore-certification") -Message "Image restore sentinel verification failed"
        Assert-CertificationCondition -Condition (-not $markerStillExists) -Message "Image restore did not overwrite pre-restore target marker"

        return @{
            source_image = $source.path
            target_image = $target.path
            target_disk_number = $target.disk_number
            pre_restore_marker = $preRestoreMarker
            sentinel_verified = $true
            target_overwritten = $true
        }
    }
    finally {
        Remove-DisposableVhdDisk -Vhd $target
        Remove-DisposableVhdDisk -Vhd $source
    }
}

function Invoke-PartitionCloneRegionScenario {
    $source = New-DisposableVhdDisk -Name "partition-clone-region-source"
    $target = New-DisposableVhdDisk -Name "partition-clone-region-target"
    try {
        Initialize-Disk -Number $source.disk_number -PartitionStyle GPT -ErrorAction Stop
        $sourcePartition = New-Partition -DiskNumber $source.disk_number -Size $PartitionCloneBytes -GptType $BasicDataGptType -ErrorAction Stop
        Initialize-Disk -Number $target.disk_number -PartitionStyle GPT -ErrorAction Stop
        $markerPartition = New-Partition -DiskNumber $target.disk_number -Size 32MB -GptType $BasicDataGptType -ErrorAction Stop

        $sourceOffset = [uint64]$sourcePartition.Offset
        $targetMarkerOffset = [uint64]$markerPartition.Offset
        $targetOffset = [uint64]($markerPartition.Offset + $markerPartition.Size + 1MB)
        $sourceSignature = New-CertificationPattern -Text "SAK-PARTITION-CLONE-REGION-SOURCE"
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
        Assert-ByteArrayEqual -Expected $sourceSignature -Actual $clonedSignature -Message "Partition clone region signature mismatch"
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
        Set-CertificationDiskOnline -DiskNumber $target.disk_number
        Set-CertificationDiskOnline -DiskNumber $source.disk_number
        Remove-DisposableVhdDisk -Vhd $target
        Remove-DisposableVhdDisk -Vhd $source
    }
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
    elseif ($RequireVhdDataDiskEvidence -and $RunVhdDataDiskMatrix -and $skippedVhd.Count -gt 0) {
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

    $report = [ordered]@{
        tool = "partition-manager-destructive-certification"
        schema_version = 1
        status = $status
        started_utc = $Script:StartedUtc.ToString("o")
        completed_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME
        project_root = $projectRoot
        output_root = $Script:RunRoot
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        run_vhd_data_disk_matrix = [bool]$RunVhdDataDiskMatrix
        elevated_relaunch_requested = [bool]$RelaunchElevated
        require_vhd_data_disk_evidence = [bool]$RequireVhdDataDiskEvidence
        keep_vhd = [bool]$KeepVhd
        vhd_size_mb = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
        administrator = Test-Administrator
        summary = [ordered]@{
            passed = $passed.Count
            failed = $failed.Count
            skipped = $skipped.Count
            external_gates = $external.Count
        }
        results = $Script:Results
    }

    $reportPath = Join-Path $Script:RunRoot "partition-manager-certification-report.json"
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Host "Partition Manager certification report: $reportPath"

    if ($failed.Count -gt 0) {
        exit 1
    }
    if ($RequireVhdDataDiskEvidence -and $RunVhdDataDiskMatrix -and $skippedVhd.Count -gt 0) {
        Write-Host "VHD data-disk evidence required, but $($skippedVhd.Count) VHD scenario(s) were skipped." -ForegroundColor Red
        exit 2
    }
}
finally {
    Pop-Location
}
