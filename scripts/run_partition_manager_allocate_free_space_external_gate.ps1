<#
.SYNOPSIS
    Runs the external Allocate Free Space gate on disposable VHD media.

.DESCRIPTION
    Creates a temporary VHDX, prepares adjacent source and donor NTFS volumes,
    backs up the donor to host storage, deletes the donor partition, extends the
    source into part of the released space, recreates the donor from the
    remaining space, restores donor data, verifies hashes and mounts, writes the
    matrix-backed external evidence report, then dismounts the VHD.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification\vm-lab",
    [string]$EvidenceDir = "",
    [string]$GuestReportPath = "",
    [int]$VhdSizeMB = 768,
    [int]$SourceSizeMB = 192,
    [int]$DonorSizeMB = 384,
    [int]$AllocateMB = 128,
    [switch]$RelaunchElevated,
    [switch]$KeepVhd,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$MinimumVhdSizeMB = 640
$SizeToleranceBytes = 2MB
$Script:RunRoot = $null
$Script:TranscriptStarted = $false

trap {
    $errorText = ($_ | Format-List * -Force | Out-String)
    if (-not [string]::IsNullOrWhiteSpace($Script:RunRoot) -and (Test-Path -LiteralPath $Script:RunRoot -PathType Container)) {
        $errorText | Set-Content -LiteralPath (Join-Path $Script:RunRoot "allocate-free-space-error.txt") -Encoding UTF8
    }
    if ($Script:TranscriptStarted) {
        Stop-Transcript | Out-Null
        $Script:TranscriptStarted = $false
    }
    throw $_
}

function Resolve-ProjectRoot {
    return Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Resolve-ProjectPath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Resolve-ProjectRoot) $Path))
}

function ConvertTo-ProjectRelativePath {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $projectRoot = Resolve-ProjectRoot
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($projectRoot)
    if (-not $fullRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [System.IO.Path]::DirectorySeparatorChar
    }
    if ($fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return ($fullPath.Substring($fullRoot.Length) -replace "\\", "/")
    }
    return $fullPath
}

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory = $true)] [string]$Value)

    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
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
        "-VhdSizeMB",
        $VhdSizeMB.ToString(),
        "-SourceSizeMB",
        $SourceSizeMB.ToString(),
        "-DonorSizeMB",
        $DonorSizeMB.ToString(),
        "-AllocateMB",
        $AllocateMB.ToString()
    )
    if (-not [string]::IsNullOrWhiteSpace($EvidenceDir)) {
        $arguments += "-EvidenceDir"
        $arguments += (ConvertTo-ProcessArgument -Value $EvidenceDir)
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestReportPath)) {
        $arguments += "-GuestReportPath"
        $arguments += (ConvertTo-ProcessArgument -Value $GuestReportPath)
    }
    if ($KeepVhd) {
        $arguments += "-KeepVhd"
    }
    if ($Force) {
        $arguments += "-Force"
    }

    Write-Host "Relaunching Allocate Free Space external gate in an elevated PowerShell window..."
    $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
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

function Assert-PathUnderRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Root
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($Root)
    if (-not $fullRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [System.IO.Path]::DirectorySeparatorChar
    }
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside expected root. Path=$fullPath Root=$fullRoot"
    }
}

function ConvertTo-PlainText {
    param([object[]]$Value)

    return (($Value | ForEach-Object { $_.ToString() }) -join "`n").Trim()
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @()
    )

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = $exitCode
        output = ConvertTo-PlainText -Value $output
    }
}

function Assert-RobocopySuccess {
    param([Parameter(Mandatory = $true)] [object]$Command)

    if ($Command.exit_code -gt 7) {
        throw "robocopy failed for $($Command.name) with exit code $($Command.exit_code)."
    }
}

function Invoke-DiskPartScript {
    param([Parameter(Mandatory = $true)] [string[]]$Lines)

    $scriptPath = Join-Path $Script:RunRoot "diskpart-$([guid]::NewGuid()).txt"
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    $command = Invoke-NativeCommand -Name "diskpart" -FilePath "diskpart.exe" -Arguments @("/s", $scriptPath)
    if ($command.exit_code -ne 0) {
        throw "diskpart failed with exit code $($command.exit_code)`n$($command.output)"
    }
    return $command
}

function Get-AttachedVhdDisk {
    param([Parameter(Mandatory = $true)] [string]$Path)

    for ($attempt = 0; $attempt -lt 30; $attempt++) {
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
    param([Parameter(Mandatory = $true)] [string]$Name)

    $safeName = $Name -replace "[^A-Za-z0-9_.-]", "-"
    $path = Join-Path $Script:RunRoot "$safeName.vhdx"
    Assert-PathUnderRoot -Path $path -Root $Script:RunRoot
    if (Test-Path -LiteralPath $path) {
        if (-not $Force) {
            throw "VHD already exists. Use -Force only for disposable lab artifacts: $path"
        }
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
    Assert-DisposableDisk -Disk $disk -ExpectedPath $path

    [pscustomobject]@{
        path = $path
        disk_number = [int]$disk.Number
        size_mb = $size
        name = $safeName
    }
}

function Remove-DisposableVhdDisk {
    param([Parameter(Mandatory = $true)] [object]$Vhd)

    try {
        Dismount-DiskImage -ImagePath $Vhd.path -ErrorAction SilentlyContinue | Out-Null
    }
    finally {
        if (-not $KeepVhd -and (Test-Path -LiteralPath $Vhd.path)) {
            Assert-PathUnderRoot -Path $Vhd.path -Root $Script:RunRoot
            Assert-Condition -Condition ([System.IO.Path]::GetExtension($Vhd.path) -eq ".vhdx") -Message "Refusing to remove non-VHDX artifact: $($Vhd.path)"
            Remove-Item -LiteralPath $Vhd.path -Force
        }
    }
}

function Assert-DisposableDisk {
    param(
        [Parameter(Mandatory = $true)] [object]$Disk,
        [Parameter(Mandatory = $true)] [string]$ExpectedPath
    )

    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Target disk $($Disk.Number) is boot/system disk."
    }
    $diskImage = Get-DiskImage -ImagePath $ExpectedPath -ErrorAction Stop
    $imageDisk = $diskImage | Get-Disk -ErrorAction Stop
    if ([int]$imageDisk.Number -ne [int]$Disk.Number) {
        throw "Mounted VHD disk mismatch. Expected $($Disk.Number), got $($imageDisk.Number)."
    }
    $requestedSizeMB = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
    if ($Disk.Size -lt ($MinimumVhdSizeMB * 1MB) -or $Disk.Size -gt (($requestedSizeMB + 64) * 1MB)) {
        throw "Target VHD size outside disposable range: $($Disk.Size)"
    }
}

function Get-FreeCertificationDriveLetters {
    param([int]$Count = 2)

    $used = @(Get-Volume -ErrorAction SilentlyContinue |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_.DriveLetter) } |
        ForEach-Object { $_.DriveLetter.ToString().ToUpperInvariant() })
    $letters = @()
    foreach ($letter in @("T", "U", "V", "W", "X", "Y", "Z")) {
        if ($used -notcontains $letter) {
            $letters += $letter
            if ($letters.Count -eq $Count) {
                return $letters
            }
        }
    }
    throw "Need $Count free certification drive letters."
}

function Get-PartitionVolumeId {
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [int]$PartitionNumber,
        [Parameter(Mandatory = $true)] [string]$DriveLetter
    )

    $partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -ErrorAction Stop
    $volumeId = @($partition.AccessPaths | Where-Object { $_ -like "\\?\Volume{*}\" } | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($volumeId)) {
        $volumeId = "\\.\$DriveLetter`:"
    }
    return $volumeId
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | ForEach-Object {
        $volume = $null
        try {
            $volume = $_ | Get-Volume -ErrorAction Stop
        }
        catch {
            $volume = $null
        }

        $gptType = ""
        if ($_.PSObject.Properties["GptType"]) {
            $gptType = $_.GptType.ToString()
        }

        [pscustomobject]@{
            partition_number = $_.PartitionNumber
            drive_letter = $_.DriveLetter
            offset_bytes = [uint64]$_.Offset
            size_bytes = [uint64]$_.Size
            type = $_.Type.ToString()
            gpt_type = $gptType
            access_paths = @($_.AccessPaths)
            file_system = if ($null -ne $volume) { $volume.FileSystem } else { "" }
            file_system_label = if ($null -ne $volume) { $volume.FileSystemLabel } else { "" }
        }
    })
    [pscustomobject]@{
        disk_number = $disk.Number
        friendly_name = $disk.FriendlyName
        unique_id = $disk.UniqueId
        size_bytes = [uint64]$disk.Size
        partition_style = $disk.PartitionStyle.ToString()
        is_boot = $disk.IsBoot
        is_system = $disk.IsSystem
        partitions = $partitions
    }
}

function New-SeedFiles {
    param(
        [Parameter(Mandatory = $true)] [string]$DonorRoot,
        [Parameter(Mandatory = $true)] [string]$SourceRoot
    )

    New-Item -ItemType Directory -Path (Join-Path $DonorRoot "nested") -Force | Out-Null
    "SAK allocate free space donor fixture $(Get-Date -Format o)" |
        Set-Content -LiteralPath (Join-Path $DonorRoot "donor-fixture.txt") -Encoding UTF8
    "Nested donor fixture" |
        Set-Content -LiteralPath (Join-Path $DonorRoot "nested\child.txt") -Encoding UTF8
    $bytes = New-Object byte[] 65536
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    [System.IO.File]::WriteAllBytes((Join-Path $DonorRoot "payload.bin"), $bytes)
    "SAK allocate free space source fixture $(Get-Date -Format o)" |
        Set-Content -LiteralPath (Join-Path $SourceRoot "source-fixture.txt") -Encoding UTF8
}

function Get-DirectoryHashManifest {
    param([Parameter(Mandatory = $true)] [string]$Root)

    $resolved = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    $basePath = $resolved.Path
    if (-not $basePath.EndsWith("\")) {
        $basePath += "\"
    }

    @(Get-ChildItem -LiteralPath $resolved.Path -Recurse -File | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{
            relative_path = ($_.FullName.Substring($basePath.Length) -replace "\\", "/")
            length_bytes = [uint64]$_.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
}

function Compare-HashManifest {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Expected,
        [Parameter(Mandatory = $true)] [object[]]$Actual
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $expectedByPath = @{}
    foreach ($entry in $Expected) {
        $expectedByPath[$entry.relative_path] = $entry
    }
    $actualByPath = @{}
    foreach ($entry in $Actual) {
        $actualByPath[$entry.relative_path] = $entry
    }
    foreach ($path in $expectedByPath.Keys) {
        if (-not $actualByPath.ContainsKey($path)) {
            $errors.Add("missing:$path")
            continue
        }
        $expectedEntry = $expectedByPath[$path]
        $actualEntry = $actualByPath[$path]
        if ($expectedEntry.length_bytes -ne $actualEntry.length_bytes -or $expectedEntry.sha256 -ne $actualEntry.sha256) {
            $errors.Add("mismatch:$path")
        }
    }
    foreach ($path in $actualByPath.Keys) {
        if (-not $expectedByPath.ContainsKey($path)) {
            $errors.Add("unexpected:$path")
        }
    }
    [pscustomobject]@{
        matched = ($errors.Count -eq 0)
        errors = @($errors)
        expected_count = @($Expected).Count
        actual_count = @($Actual).Count
    }
}

function Invoke-RepairScan {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    if (-not (Get-Command Repair-Volume -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{
            drive_letter = $DriveLetter
            status = "Skipped"
            message = "Repair-Volume command not available"
        }
    }

    $output = Repair-Volume -DriveLetter $DriveLetter -Scan -ErrorAction Stop
    [pscustomobject]@{
        drive_letter = $DriveLetter
        status = "Completed"
        output = ConvertTo-PlainText -Value @($output)
    }
}

function New-ExternalReport {
    param(
        [Parameter(Mandatory = $true)] [string]$Status,
        [Parameter(Mandatory = $true)] [object]$Evidence,
        [Parameter(Mandatory = $true)] [string[]]$Artifacts,
        [Parameter(Mandatory = $true)] [string]$VerificationSummary,
        [Parameter(Mandatory = $true)] [string]$OperatorNotes
    )

    [pscustomobject]@{
        tool = "partition-manager-external-evidence-report"
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString("o")
        gate_id = "external.allocate-free-space"
        gate_name = "Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof"
        status = $Status
        manifest = "artifacts\partition-manager-certification\vm-lab\external-evidence.json"
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/external.allocate-free-space/report.json"
        safety_contract = @(
            "disposable_source_and_donor_volumes_only",
            "source_donor_identity_recorded",
            "before_after_layout_verified",
            "mount_and_file_hash_verified"
        )
        required_evidence_keys = @(
            "source_volume_id",
            "donor_volume_id",
            "before_layout",
            "after_layout",
            "volume_size_delta",
            "file_hash_validation",
            "mount_validation",
            "rollback_or_backup_evidence"
        )
        required_evidence_values = $null
        evidence = $Evidence
        artifacts = $Artifacts
        verification_summary = $VerificationSummary
        operator_notes = $OperatorNotes
    }
}

function Initialize-LabVolumes {
    param(
        [Parameter(Mandatory = $true)] [object]$Vhd,
        [Parameter(Mandatory = $true)] [string]$SourceLetter,
        [Parameter(Mandatory = $true)] [string]$DonorLetter
    )

    Initialize-Disk -Number $Vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
    $source = New-Partition -DiskNumber $Vhd.disk_number -Size ($SourceSizeMB * 1MB) -DriveLetter $SourceLetter -ErrorAction Stop
    Format-Volume -DriveLetter $SourceLetter -FileSystem NTFS -NewFileSystemLabel "SAKALLOC_SRC" -Confirm:$false -Force | Out-Null
    $donor = New-Partition -DiskNumber $Vhd.disk_number -Size ($DonorSizeMB * 1MB) -DriveLetter $DonorLetter -ErrorAction Stop
    Format-Volume -DriveLetter $DonorLetter -FileSystem NTFS -NewFileSystemLabel "SAKALLOC_DONOR" -Confirm:$false -Force | Out-Null

    Start-Sleep -Milliseconds 500
    [pscustomobject]@{
        source = Get-Partition -DiskNumber $Vhd.disk_number -PartitionNumber $source.PartitionNumber
        donor = Get-Partition -DiskNumber $Vhd.disk_number -PartitionNumber $donor.PartitionNumber
    }
}

if ($RelaunchElevated -and -not (Test-IsAdmin)) {
    Invoke-ElevatedRelaunch
}
if (-not (Test-IsAdmin)) {
    throw "Run from an elevated PowerShell session, or pass -RelaunchElevated."
}
if ($AllocateMB -le 0 -or $SourceSizeMB -le 0 -or $DonorSizeMB -le 0) {
    throw "Source, donor, and allocate sizes must be positive."
}
if ($AllocateMB -ge ($DonorSizeMB - 64)) {
    throw "AllocateMB must leave at least 64 MB for recreated donor volume."
}
if ($VhdSizeMB -lt ($SourceSizeMB + $DonorSizeMB + 96)) {
    throw "VhdSizeMB too small for requested source/donor sizes."
}

$resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
if ([string]::IsNullOrWhiteSpace($EvidenceDir)) {
    $EvidenceDir = Join-Path $resolvedOutputRoot "external-evidence\external.allocate-free-space"
}
else {
    $EvidenceDir = Resolve-ProjectPath -Path $EvidenceDir
}
if ([string]::IsNullOrWhiteSpace($GuestReportPath)) {
    $GuestReportPath = Join-Path $resolvedOutputRoot "external-allocate-free-space-guest-report.json"
}
else {
    $GuestReportPath = Resolve-ProjectPath -Path $GuestReportPath
}
$Script:RunRoot = Join-Path $resolvedOutputRoot ("allocate-free-space-stage-debug\run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $GuestReportPath) -Force | Out-Null

try {
    Start-Transcript -Path (Join-Path $Script:RunRoot "run_partition_manager_allocate_free_space_external_gate.log") -Force | Out-Null
    $Script:TranscriptStarted = $true
}
catch {
    $Script:TranscriptStarted = $false
}

foreach ($staleReport in @("report.json", "report.failed.json", "report.failed-cleanup.json")) {
    $stalePath = Join-Path $EvidenceDir $staleReport
    if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
        Remove-Item -LiteralPath $stalePath -Force
    }
}

$startedAt = Get-Date
$commands = New-Object System.Collections.Generic.List[object]
$blockers = New-Object System.Collections.Generic.List[string]
$cleanupErrors = New-Object System.Collections.Generic.List[string]
$status = "Failed"
$vhd = $null
$beforeLayout = $null
$afterLayout = $null
$sourceVolumeId = "not-created"
$donorVolumeId = "not-created"
$fileHashValidation = $null
$mountValidation = $null
$rollbackEvidence = $null
$volumeSizeDelta = $null
$reportPath = Join-Path $EvidenceDir "report.json"

try {
    $letters = Get-FreeCertificationDriveLetters -Count 2
    $sourceLetter = $letters[0]
    $donorLetter = $letters[1]
    $vhd = New-DisposableVhdDisk -Name "allocate-free-space"
    $layout = Initialize-LabVolumes -Vhd $vhd -SourceLetter $sourceLetter -DonorLetter $donorLetter
    New-SeedFiles -SourceRoot "$sourceLetter`:\" -DonorRoot "$donorLetter`:\"

    $sourceBefore = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $layout.source.PartitionNumber
    $donorBefore = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $layout.donor.PartitionNumber
    $sourceVolumeId = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber -DriveLetter $sourceLetter
    $donorVolumeId = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $donorBefore.PartitionNumber -DriveLetter $donorLetter
    $beforeLayout = Get-DiskSnapshot -DiskNumber $vhd.disk_number

    $backupRoot = Join-Path $Script:RunRoot "donor-backup"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $beforeManifest = Get-DirectoryHashManifest -Root "$donorLetter`:\"
    $beforeManifestPath = Join-Path $EvidenceDir "donor-manifest-before.json"
    $beforeManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $beforeManifestPath -Encoding UTF8

    $backup = Invoke-NativeCommand -Name "backup-donor" -FilePath "robocopy.exe" -Arguments @("$donorLetter`:\", $backupRoot, "/MIR", "/R:1", "/W:1", "/NP")
    $commands.Add($backup)
    Assert-RobocopySuccess -Command $backup

    Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $donorBefore.PartitionNumber -Confirm:$false -ErrorAction Stop
    Start-Sleep -Milliseconds 500

    $allocatedBytes = [uint64]($AllocateMB * 1MB)
    $sourceTargetSize = [uint64]$sourceBefore.Size + $allocatedBytes
    Resize-Partition -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber -Size $sourceTargetSize -ErrorAction Stop
    Start-Sleep -Milliseconds 500

    $donorTargetSize = [uint64]$donorBefore.Size - $allocatedBytes
    $donorAfterCreate = New-Partition -DiskNumber $vhd.disk_number -Size $donorTargetSize -DriveLetter $donorLetter -ErrorAction Stop
    Format-Volume -DriveLetter $donorLetter -FileSystem NTFS -NewFileSystemLabel "SAKALLOC_DONOR" -Confirm:$false -Force | Out-Null
    Start-Sleep -Milliseconds 500

    $restore = Invoke-NativeCommand -Name "restore-donor" -FilePath "robocopy.exe" -Arguments @($backupRoot, "$donorLetter`:\", "/MIR", "/R:1", "/W:1", "/NP")
    $commands.Add($restore)
    Assert-RobocopySuccess -Command $restore

    $afterManifest = Get-DirectoryHashManifest -Root "$donorLetter`:\"
    $afterManifestPath = Join-Path $EvidenceDir "donor-manifest-after.json"
    $afterManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $afterManifestPath -Encoding UTF8
    $manifestComparison = Compare-HashManifest -Expected $beforeManifest -Actual $afterManifest
    Assert-Condition -Condition $manifestComparison.matched -Message "Donor hash manifest did not match after recreate/restore."

    $sourceAfter = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber
    $donorAfter = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $donorAfterCreate.PartitionNumber
    Assert-Condition -Condition ([Math]::Abs([int64]($sourceAfter.Size - $sourceTargetSize)) -le $SizeToleranceBytes) -Message "Source partition did not grow by requested amount."
    Assert-Condition -Condition ([Math]::Abs([int64]($donorAfter.Size - $donorTargetSize)) -le $SizeToleranceBytes) -Message "Donor partition did not shrink by requested amount."

    $sourceVolumeAfter = Get-Volume -DriveLetter $sourceLetter -ErrorAction Stop
    $donorVolumeAfter = Get-Volume -DriveLetter $donorLetter -ErrorAction Stop
    Assert-Condition -Condition ($sourceVolumeAfter.FileSystem -eq "NTFS") -Message "Source volume not mounted as NTFS after resize."
    Assert-Condition -Condition ($donorVolumeAfter.FileSystem -eq "NTFS") -Message "Donor volume not mounted as NTFS after recreate."

    $sourceScan = Invoke-RepairScan -DriveLetter $sourceLetter
    $donorScan = Invoke-RepairScan -DriveLetter $donorLetter
    $afterLayout = Get-DiskSnapshot -DiskNumber $vhd.disk_number

    $volumeSizeDelta = [pscustomobject]@{
        source_before_bytes = [uint64]$sourceBefore.Size
        source_after_bytes = [uint64]$sourceAfter.Size
        source_delta_bytes = [int64]($sourceAfter.Size - $sourceBefore.Size)
        donor_before_bytes = [uint64]$donorBefore.Size
        donor_after_bytes = [uint64]$donorAfter.Size
        donor_delta_bytes = [int64]($donorAfter.Size - $donorBefore.Size)
        requested_allocate_bytes = $allocatedBytes
        tolerance_bytes = $SizeToleranceBytes
    }
    $fileHashValidation = [pscustomobject]@{
        matched = $manifestComparison.matched
        expected_file_count = $manifestComparison.expected_count
        actual_file_count = $manifestComparison.actual_count
        errors = @($manifestComparison.errors)
        before_manifest_path = ConvertTo-ProjectRelativePath -Path $beforeManifestPath
        after_manifest_path = ConvertTo-ProjectRelativePath -Path $afterManifestPath
    }
    $mountValidation = [pscustomobject]@{
        source_drive_letter = "$sourceLetter`:"
        donor_drive_letter = "$donorLetter`:"
        source_file_system = $sourceVolumeAfter.FileSystem
        donor_file_system = $donorVolumeAfter.FileSystem
        source_repair_scan = $sourceScan
        donor_repair_scan = $donorScan
    }
    $rollbackEvidence = [pscustomobject]@{
        backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
        donor_backup_command = "robocopy donor -> backup /MIR /R:1 /W:1"
        donor_restore_command = "robocopy backup -> donor /MIR /R:1 /W:1"
        cleanup_mode = if ($KeepVhd) { "VHD dismounted and retained" } else { "VHD dismounted and removed" }
        run_root = ConvertTo-ProjectRelativePath -Path $Script:RunRoot
    }

    $evidence = [pscustomobject]@{
        source_volume_id = $sourceVolumeId
        donor_volume_id = $donorVolumeId
        before_layout = $beforeLayout
        after_layout = $afterLayout
        volume_size_delta = $volumeSizeDelta
        file_hash_validation = $fileHashValidation
        mount_validation = $mountValidation
        rollback_or_backup_evidence = $rollbackEvidence
    }
    $artifacts = @(
        (ConvertTo-ProjectRelativePath -Path $GuestReportPath),
        (ConvertTo-ProjectRelativePath -Path $beforeManifestPath),
        (ConvertTo-ProjectRelativePath -Path $afterManifestPath),
        (ConvertTo-ProjectRelativePath -Path (Join-Path $Script:RunRoot "run_partition_manager_allocate_free_space_external_gate.log"))
    )
    $report = New-ExternalReport `
        -Status "Passed" `
        -Evidence $evidence `
        -Artifacts $artifacts `
        -VerificationSummary "Disposable VHD source volume $sourceVolumeId and adjacent donor volume $donorVolumeId were created, donor data was backed up off the VHD, donor partition was deleted, source was extended by $allocatedBytes bytes, donor was recreated from remaining space, donor data was restored, SHA256 manifest comparison passed, and both volumes remounted as NTFS." `
        -OperatorNotes "Run used only a generated disposable VHDX under the certification artifact root; no physical disk numbers were selected by operator input."
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    $status = "Passed"
}
catch {
    $blockers.Add(($_ | Format-List * -Force | Out-String).Trim())
    $failedEvidence = [pscustomobject]@{
        source_volume_id = $sourceVolumeId
        donor_volume_id = $donorVolumeId
        before_layout = if ($null -ne $beforeLayout) { $beforeLayout } else { "not-verified" }
        after_layout = if ($null -ne $afterLayout) { $afterLayout } else { "not-verified" }
        volume_size_delta = if ($null -ne $volumeSizeDelta) { $volumeSizeDelta } else { "not-verified" }
        file_hash_validation = if ($null -ne $fileHashValidation) { $fileHashValidation } else { "not-verified" }
        mount_validation = if ($null -ne $mountValidation) { $mountValidation } else { "not-verified" }
        rollback_or_backup_evidence = if ($null -ne $rollbackEvidence) { $rollbackEvidence } else { "not-verified" }
    }
    $failedReport = New-ExternalReport `
        -Status "Failed" `
        -Evidence $failedEvidence `
        -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) `
        -VerificationSummary "Allocate Free Space external gate failed." `
        -OperatorNotes $_.Exception.Message
    $failedReport | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $EvidenceDir "report.failed.json") -Encoding UTF8
}
finally {
    if ($vhd) {
        try {
            Remove-DisposableVhdDisk -Vhd $vhd
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
            if ($status -eq "Passed") {
                $status = "Failed"
                $failedCleanupPath = Join-Path $EvidenceDir "report.failed-cleanup.json"
                if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
                    Move-Item -LiteralPath $reportPath -Destination $failedCleanupPath -Force
                }
            }
        }
    }
}

$vhdPathForReport = ""
$vhdDiskNumberForReport = $null
if ($vhd) {
    $vhdPathForReport = $vhd.path
    $vhdDiskNumberForReport = [int]$vhd.disk_number
}
$completedAtForReport = (Get-Date).ToString("o")
$userForReport = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$isAdminForReport = Test-IsAdmin
$commandsForReport = @()
foreach ($command in $commands) {
    $commandsForReport += $command
}
$blockersForReport = @()
foreach ($blocker in $blockers) {
    $blockersForReport += $blocker.ToString()
}
$cleanupErrorsForReport = @()
foreach ($cleanupError in $cleanupErrors) {
    $cleanupErrorsForReport += $cleanupError.ToString()
}

$guestReport = [pscustomobject]@{
    schema_version = 1
    tool = "sak-vhd-external-allocate-free-space-gate"
    status = $status
    vm_id = $env:COMPUTERNAME
    started_at = $startedAt.ToString("o")
    completed_at = $completedAtForReport
    user = $userForReport
    is_admin = $isAdminForReport
    vhd_path = $vhdPathForReport
    vhd_disk_number = $vhdDiskNumberForReport
    evidence_dir = $EvidenceDir
    source_volume_id = $sourceVolumeId
    donor_volume_id = $donorVolumeId
    before_layout = $beforeLayout
    after_layout = $afterLayout
    volume_size_delta = $volumeSizeDelta
    file_hash_validation = $fileHashValidation
    mount_validation = $mountValidation
    rollback_or_backup_evidence = $rollbackEvidence
    commands = $commandsForReport
    blockers = $blockersForReport
    cleanup_errors = $cleanupErrorsForReport
}

try {
    $guestReport | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $GuestReportPath -Encoding UTF8
}
catch {
    $fallbackPath = "$GuestReportPath.write-error.txt"
    "Failed to write guest report: $($_.Exception.Message)" | Set-Content -LiteralPath $fallbackPath -Encoding UTF8
}

if ($Script:TranscriptStarted) {
    Stop-Transcript | Out-Null
    $Script:TranscriptStarted = $false
}

if ($status -ne "Passed") {
    Write-Error "External Allocate Free Space gate failed. Report: $GuestReportPath"
    exit 1
}

Write-Host "External Allocate Free Space gate passed. Report: $GuestReportPath"
exit 0
