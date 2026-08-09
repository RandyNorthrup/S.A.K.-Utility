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
$Script:RunLock = $null

function Close-RunLock {
    if ($null -ne $Script:RunLock) {
        $Script:RunLock.Dispose()
        $Script:RunLock = $null
    }
}

trap {
    $originalError = $_
    $errorText = ($originalError | Format-List * -Force | Out-String)
    if (-not [string]::IsNullOrWhiteSpace($Script:RunRoot) -and (Test-Path -LiteralPath $Script:RunRoot -PathType Container)) {
        try {
            $errorText | Set-Content -LiteralPath (Join-Path $Script:RunRoot "allocate-free-space-error.txt") -Encoding UTF8
        }
        catch {
            Write-Warning "Failed to write trap evidence file: $($_.Exception.Message)"
        }
    }
    if ($Script:TranscriptStarted) {
        try {
            Stop-Transcript | Out-Null
        }
        catch {
            Write-Warning "Failed to stop transcript: $($_.Exception.Message)"
        }
        $Script:TranscriptStarted = $false
    }
    try {
        Close-RunLock
    }
    catch {
        Write-Warning "Failed to release run lock: $($_.Exception.Message)"
    }
    throw $originalError
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

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    # Windows CommandLineToArgvW rules: a run of backslashes is only escaped when
    # it precedes a double quote, so quoting has to be built character by character.
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]'\') {
            $backslashes++
            continue
        }
        if ($character -eq [char]'"') {
            [void]$builder.Append([char]'\', ($backslashes * 2) + 1)
            [void]$builder.Append([char]'"')
        }
        else {
            if ($backslashes -gt 0) {
                [void]$builder.Append([char]'\', $backslashes)
            }
            [void]$builder.Append($character)
        }
        $backslashes = 0
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append([char]'\', $backslashes * 2)
    }
    [void]$builder.Append('"')
    return $builder.ToString()
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

function Assert-NoReparsePointBelowRoot {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Root
    )

    $rootKey = [System.IO.Path]::GetFullPath($Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $current = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    while (-not [string]::IsNullOrEmpty($current) -and
        -not $current.Equals($rootKey, [System.StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $current) {
            $attributes = [System.IO.File]::GetAttributes($current)
            if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to operate through a reparse point. Path=$current Root=$rootKey"
            }
        }
        $parent = [System.IO.Path]::GetDirectoryName($current)
        if ([string]::IsNullOrEmpty($parent) -or $parent.TrimEnd([System.IO.Path]::DirectorySeparatorChar) -eq $current) {
            break
        }
        $current = $parent.TrimEnd([System.IO.Path]::DirectorySeparatorChar)
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
    Assert-NoReparsePointBelowRoot -Path $fullPath -Root $fullRoot
}

function ConvertTo-PlainText {
    param([object[]]$Value)

    return ((@($Value) | ForEach-Object {
        if ($null -eq $_) { "" } else { $_.ToString() }
    }) -join "`n").Trim()
}

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)] [string]$Name)

    $path = Join-Path ([System.Environment]::SystemDirectory) $Name
    if (-not [System.IO.File]::Exists($path)) {
        throw "Required system tool not found: $path"
    }
    return $path
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @()
    )

    if (-not [System.IO.Path]::IsPathRooted($FilePath) -or -not [System.IO.File]::Exists($FilePath)) {
        throw "Refusing to run '$Name' from an unresolved command name: $FilePath"
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = $null
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($null -eq $exitCode) {
        throw "Native command '$Name' did not report an exit code: $FilePath`n$(ConvertTo-PlainText -Value $output)"
    }
    [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = [int]$exitCode
        output = ConvertTo-PlainText -Value $output
    }
}

function Assert-RobocopySuccess {
    param([Parameter(Mandatory = $true)] [object]$Command)

    if ($null -eq $Command.exit_code) {
        throw "robocopy did not report an exit code for $($Command.name)."
    }
    $exitCode = [int]$Command.exit_code
    if ($exitCode -lt 0 -or $exitCode -ge 8) {
        throw "robocopy failed for $($Command.name) with exit code $exitCode."
    }
    if (($exitCode -band 4) -ne 0) {
        throw "robocopy reported mismatched files or directories for $($Command.name) with exit code $exitCode."
    }
}

function Invoke-DiskPartScript {
    param([Parameter(Mandatory = $true)] [string[]]$Lines)

    $scriptPath = Join-Path $Script:RunRoot "diskpart-$([guid]::NewGuid()).txt"
    Assert-PathUnderRoot -Path $scriptPath -Root $Script:RunRoot
    Set-Content -LiteralPath $scriptPath -Value ($Lines -join [Environment]::NewLine) -Encoding ASCII
    $command = Invoke-NativeCommand -Name "diskpart" -FilePath (Resolve-SystemToolPath -Name "diskpart.exe") -Arguments @("/s", $scriptPath)
    if ($command.exit_code -ne 0) {
        throw "diskpart failed with exit code $($command.exit_code)`n$($command.output)"
    }
    return $command
}

function Get-AttachedVhdDisk {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $lastError = $null
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try {
            $disk = Get-DiskImage -ImagePath $Path -ErrorAction Stop | Get-Disk -ErrorAction Stop
            if ($null -ne $disk) {
                return $disk
            }
            $lastError = "Get-Disk returned no disk for the attached image."
        }
        catch {
            $lastError = $_.Exception.Message
            Start-Sleep -Milliseconds 250
        }
    }
    throw "Attached VHD disk not found for $Path. Last error: $lastError"
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

    $size = $VhdSizeMB
    $diskpartCommand = Invoke-DiskPartScript -Lines @(
        "create vdisk file=`"$path`" maximum=$size type=expandable",
        "attach vdisk"
    )
    $commands.Add($diskpartCommand)

    try {
        $disk = Get-AttachedVhdDisk -Path $path
        # Prove the disk is this image and is not boot/system before mutating its state.
        Assert-VhdDiskIdentity -Vhd ([pscustomobject]@{ path = $path; disk_number = [int]$disk.Number })
        if ($disk.IsOffline) {
            Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction Stop
        }
        if ($disk.IsReadOnly) {
            Set-Disk -Number $disk.Number -IsReadOnly $false -ErrorAction Stop
        }
        $disk = Get-AttachedVhdDisk -Path $path
        Assert-DisposableDisk -Disk $disk -ExpectedPath $path
        Assert-Condition -Condition (-not $disk.IsOffline) -Message "Disposable VHD disk $($disk.Number) is still offline."
        Assert-Condition -Condition (-not $disk.IsReadOnly) -Message "Disposable VHD disk $($disk.Number) is still read-only."
    }
    catch {
        $creationError = $_
        try {
            Dismount-DiskImage -ImagePath $path -ErrorAction Stop | Out-Null
            if (-not $KeepVhd -and (Test-Path -LiteralPath $path)) {
                Assert-PathUnderRoot -Path $path -Root $Script:RunRoot
                Remove-Item -LiteralPath $path -Force
            }
        }
        catch {
            Write-Warning "Rollback of the partially created disposable VHD failed: $($_.Exception.Message)"
        }
        throw $creationError
    }

    [pscustomobject]@{
        path = $path
        disk_number = [int]$disk.Number
        size_mb = $size
        name = $safeName
    }
}

function Remove-DisposableVhdDisk {
    param([Parameter(Mandatory = $true)] [object]$Vhd)

    $dismountError = $null
    try {
        Dismount-DiskImage -ImagePath $Vhd.path -ErrorAction Stop | Out-Null
        $image = Get-DiskImage -ImagePath $Vhd.path -ErrorAction Stop
        Assert-Condition -Condition (-not $image.Attached) -Message "Disposable VHD is still attached after dismount: $($Vhd.path)"
    }
    catch {
        $dismountError = $_
    }
    if ($null -eq $dismountError -and -not $KeepVhd -and (Test-Path -LiteralPath $Vhd.path)) {
        Assert-PathUnderRoot -Path $Vhd.path -Root $Script:RunRoot
        Assert-Condition -Condition ([System.IO.Path]::GetExtension($Vhd.path) -eq ".vhdx") -Message "Refusing to remove non-VHDX artifact: $($Vhd.path)"
        Remove-Item -LiteralPath $Vhd.path -Force
    }
    if ($null -ne $dismountError) {
        throw $dismountError
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
    $requestedSizeMB = $VhdSizeMB
    if ($Disk.Size -lt ($MinimumVhdSizeMB * 1MB) -or $Disk.Size -gt (($requestedSizeMB + 64) * 1MB)) {
        throw "Target VHD size outside disposable range: $($Disk.Size)"
    }
}

function Assert-VhdDiskIdentity {
    param([Parameter(Mandatory = $true)] [object]$Vhd)

    $image = Get-DiskImage -ImagePath $Vhd.path -ErrorAction Stop
    Assert-Condition -Condition ([bool]$image.Attached) -Message "Disposable VHD is no longer attached: $($Vhd.path)"
    $imageDisk = $image | Get-Disk -ErrorAction Stop
    Assert-Condition -Condition ($null -ne $imageDisk) -Message "Disposable VHD no longer exposes a disk: $($Vhd.path)"
    Assert-Condition -Condition ([int]$imageDisk.Number -eq [int]$Vhd.disk_number) -Message "Disposable VHD disk number changed. Expected $($Vhd.disk_number), got $($imageDisk.Number)."
    Assert-Condition -Condition (-not ($imageDisk.IsBoot -or $imageDisk.IsSystem)) -Message "Refusing to operate on boot/system disk $($imageDisk.Number)."
}

function Assert-DriveLetterOnDisk {
    param(
        [Parameter(Mandatory = $true)] [string]$DriveLetter,
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [int]$PartitionNumber
    )

    $partition = Get-Partition -DriveLetter $DriveLetter -ErrorAction Stop
    Assert-Condition -Condition ([int]$partition.DiskNumber -eq $DiskNumber) -Message "Drive $DriveLetter is on disk $($partition.DiskNumber), not disposable disk $DiskNumber."
    Assert-Condition -Condition ([int]$partition.PartitionNumber -eq $PartitionNumber) -Message "Drive $DriveLetter no longer maps to partition $PartitionNumber on disk $DiskNumber."
}

function Get-FreeCertificationDriveLetters {
    param([int]$Count = 2)

    $used = @(Get-Volume -ErrorAction Stop |
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
    $volumeId = [string](@($partition.AccessPaths) | Where-Object { $_ -like "\\?\Volume{*}\" } | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($volumeId)) {
        throw "No volume GUID access path for disk $DiskNumber partition $PartitionNumber (drive $DriveLetter); volume identity cannot be recorded."
    }
    return $volumeId
}

function Get-DiskSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $partitions = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop | ForEach-Object {
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
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
    }
    finally {
        $rng.Dispose()
    }
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

    if (-not (Get-Command -Name "Repair-Volume" -CommandType Cmdlet -ErrorAction SilentlyContinue)) {
        throw "Repair-Volume cmdlet is not available; the filesystem health evidence this gate reports cannot be collected."
    }

    $output = Repair-Volume -DriveLetter $DriveLetter -Scan -ErrorAction Stop
    $outputText = ConvertTo-PlainText -Value @($output)
    if ($outputText -ne "NoErrorsFound") {
        throw "Repair-Volume scan of drive $DriveLetter reported '$outputText' instead of NoErrorsFound."
    }
    [pscustomobject]@{
        drive_letter = $DriveLetter
        status = "Completed"
        output = $outputText
    }
}

function ConvertTo-EvidenceOrSentinel {
    param([object]$Value = $null)

    if ($null -ne $Value) {
        return $Value
    }
    return [pscustomobject]@{
        verified = $false
        status = "not-verified"
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

    Assert-VhdDiskIdentity -Vhd $Vhd
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
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    throw "OutputRoot must be a non-empty path."
}
if ($AllocateMB -le 0 -or $SourceSizeMB -le 0 -or $DonorSizeMB -le 0) {
    throw "Source, donor, and allocate sizes must be positive."
}
if ($AllocateMB -ge ($DonorSizeMB - 64)) {
    throw "AllocateMB must leave at least 64 MB for recreated donor volume."
}
if ($VhdSizeMB -lt $MinimumVhdSizeMB) {
    throw "VhdSizeMB must be at least $MinimumVhdSizeMB MB for disposable lab media."
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
Assert-PathUnderRoot -Path $EvidenceDir -Root $resolvedOutputRoot
Assert-PathUnderRoot -Path $GuestReportPath -Root $resolvedOutputRoot
$stageRoot = Join-Path $resolvedOutputRoot "allocate-free-space-stage-debug"
$Script:RunRoot = Join-Path $stageRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $GuestReportPath) -Force | Out-Null
Assert-PathUnderRoot -Path $Script:RunRoot -Root $stageRoot

try {
    $Script:RunLock = [System.IO.File]::Open(
        (Join-Path $stageRoot "gate.lock"),
        [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
}
catch {
    throw "Another Allocate Free Space external gate run holds $stageRoot; concurrent runs would overwrite shared evidence. $($_.Exception.Message)"
}

Start-Transcript -Path (Join-Path $Script:RunRoot "run_partition_manager_allocate_free_space_external_gate.log") -Force | Out-Null
$Script:TranscriptStarted = $true

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
    Assert-DriveLetterOnDisk -DriveLetter $sourceLetter -DiskNumber $vhd.disk_number -PartitionNumber $layout.source.PartitionNumber
    Assert-DriveLetterOnDisk -DriveLetter $donorLetter -DiskNumber $vhd.disk_number -PartitionNumber $layout.donor.PartitionNumber
    New-SeedFiles -SourceRoot "$sourceLetter`:\" -DonorRoot "$donorLetter`:\"

    $sourceBefore = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $layout.source.PartitionNumber
    $donorBefore = Get-Partition -DiskNumber $vhd.disk_number -PartitionNumber $layout.donor.PartitionNumber
    $sourceVolumeId = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber -DriveLetter $sourceLetter
    $donorVolumeId = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $donorBefore.PartitionNumber -DriveLetter $donorLetter
    $beforeLayout = Get-DiskSnapshot -DiskNumber $vhd.disk_number
    $sourceManifestBefore = Get-DirectoryHashManifest -Root "$sourceLetter`:\"

    $backupRoot = Join-Path $Script:RunRoot "donor-backup"
    Assert-PathUnderRoot -Path $backupRoot -Root $Script:RunRoot
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $beforeManifest = Get-DirectoryHashManifest -Root "$donorLetter`:\"
    $beforeManifestPath = Join-Path $EvidenceDir "donor-manifest-before.json"
    $beforeManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $beforeManifestPath -Encoding UTF8

    $backup = Invoke-NativeCommand -Name "backup-donor" -FilePath (Resolve-SystemToolPath -Name "robocopy.exe") -Arguments @("$donorLetter`:\", $backupRoot, "/MIR", "/R:1", "/W:1", "/NP")
    $commands.Add($backup)
    Assert-RobocopySuccess -Command $backup
    $backupManifest = Get-DirectoryHashManifest -Root $backupRoot
    $backupComparison = Compare-HashManifest -Expected $beforeManifest -Actual $backupManifest
    Assert-Condition -Condition $backupComparison.matched -Message "Donor backup does not match the donor volume; refusing to delete the donor partition. Errors: $(@($backupComparison.errors) -join ', ')"

    Assert-VhdDiskIdentity -Vhd $vhd
    Assert-DriveLetterOnDisk -DriveLetter $donorLetter -DiskNumber $vhd.disk_number -PartitionNumber $donorBefore.PartitionNumber
    Remove-Partition -DiskNumber $vhd.disk_number -PartitionNumber $donorBefore.PartitionNumber -Confirm:$false -ErrorAction Stop
    Start-Sleep -Milliseconds 500

    $allocatedBytes = [uint64]($AllocateMB * 1MB)
    $sourceTargetSize = [uint64]$sourceBefore.Size + $allocatedBytes
    Assert-VhdDiskIdentity -Vhd $vhd
    Assert-DriveLetterOnDisk -DriveLetter $sourceLetter -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber
    Resize-Partition -DiskNumber $vhd.disk_number -PartitionNumber $sourceBefore.PartitionNumber -Size $sourceTargetSize -ErrorAction Stop
    Start-Sleep -Milliseconds 500

    $donorTargetSize = [uint64]$donorBefore.Size - $allocatedBytes
    Assert-VhdDiskIdentity -Vhd $vhd
    $donorAfterCreate = New-Partition -DiskNumber $vhd.disk_number -Size $donorTargetSize -DriveLetter $donorLetter -ErrorAction Stop
    Format-Volume -DriveLetter $donorLetter -FileSystem NTFS -NewFileSystemLabel "SAKALLOC_DONOR" -Confirm:$false -Force | Out-Null
    Start-Sleep -Milliseconds 500
    Assert-DriveLetterOnDisk -DriveLetter $donorLetter -DiskNumber $vhd.disk_number -PartitionNumber $donorAfterCreate.PartitionNumber
    $donorVolumeIdAfterRecreate = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $donorAfterCreate.PartitionNumber -DriveLetter $donorLetter

    $restore = Invoke-NativeCommand -Name "restore-donor" -FilePath (Resolve-SystemToolPath -Name "robocopy.exe") -Arguments @($backupRoot, "$donorLetter`:\", "/MIR", "/R:1", "/W:1", "/NP")
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

    $sourceManifestAfter = Get-DirectoryHashManifest -Root "$sourceLetter`:\"
    $sourceComparison = Compare-HashManifest -Expected $sourceManifestBefore -Actual $sourceManifestAfter
    Assert-Condition -Condition $sourceComparison.matched -Message "Source volume data did not survive the extend. Errors: $(@($sourceComparison.errors) -join ', ')"

    $sourceScan = Invoke-RepairScan -DriveLetter $sourceLetter
    $donorScan = Invoke-RepairScan -DriveLetter $donorLetter
    $afterLayout = Get-DiskSnapshot -DiskNumber $vhd.disk_number
    Assert-Condition -Condition (@($afterLayout.partitions).Count -eq @($beforeLayout.partitions).Count) -Message "Disk $($vhd.disk_number) has $(@($afterLayout.partitions).Count) partitions after the operation, expected $(@($beforeLayout.partitions).Count)."
    Assert-Condition -Condition (-not ($afterLayout.is_boot -or $afterLayout.is_system)) -Message "Disk $($vhd.disk_number) reports boot/system after the operation."

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
        backup_matched_donor_before_delete = $backupComparison.matched
        backup_expected_file_count = $backupComparison.expected_count
        backup_actual_file_count = $backupComparison.actual_count
        backup_errors = @($backupComparison.errors)
        source_matched = $sourceComparison.matched
        source_expected_file_count = $sourceComparison.expected_count
        source_actual_file_count = $sourceComparison.actual_count
        source_errors = @($sourceComparison.errors)
    }
    $mountValidation = [pscustomobject]@{
        source_drive_letter = "$sourceLetter`:"
        donor_drive_letter = "$donorLetter`:"
        source_file_system = $sourceVolumeAfter.FileSystem
        donor_file_system = $donorVolumeAfter.FileSystem
        source_repair_scan = $sourceScan
        donor_repair_scan = $donorScan
        donor_volume_id_after_recreate = $donorVolumeIdAfterRecreate
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
        before_layout = ConvertTo-EvidenceOrSentinel -Value $beforeLayout
        after_layout = ConvertTo-EvidenceOrSentinel -Value $afterLayout
        volume_size_delta = ConvertTo-EvidenceOrSentinel -Value $volumeSizeDelta
        file_hash_validation = ConvertTo-EvidenceOrSentinel -Value $fileHashValidation
        mount_validation = ConvertTo-EvidenceOrSentinel -Value $mountValidation
        rollback_or_backup_evidence = ConvertTo-EvidenceOrSentinel -Value $rollbackEvidence
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
            $cleanupError = $_
            $cleanupErrors.Add($cleanupError.Exception.Message)
            if ($status -eq "Passed") {
                $status = "Failed"
                $failedCleanupPath = Join-Path $EvidenceDir "report.failed-cleanup.json"
                if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
                    Move-Item -LiteralPath $reportPath -Destination $failedCleanupPath -Force
                }
                $rollbackEvidence.cleanup_mode = "VHD cleanup failed: $($cleanupError.Exception.Message)"
                $cleanupReport = New-ExternalReport `
                    -Status "Failed" `
                    -Evidence $evidence `
                    -Artifacts $artifacts `
                    -VerificationSummary "Allocate Free Space external gate verified the operation but failed to clean up the disposable VHD." `
                    -OperatorNotes $cleanupError.Exception.Message
                $cleanupReport | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $failedCleanupPath -Encoding UTF8
            }
        }
    }
}

if ($Script:TranscriptStarted) {
    try {
        Stop-Transcript | Out-Null
    }
    catch {
        $cleanupErrors.Add("Failed to stop transcript: $($_.Exception.Message)")
        if ($status -eq "Passed") {
            $status = "Failed"
            if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
                Move-Item -LiteralPath $reportPath -Destination (Join-Path $EvidenceDir "report.failed-cleanup.json") -Force
            }
        }
    }
    $Script:TranscriptStarted = $false
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
    $guestReportError = $_.Exception.Message
    $fallbackPath = "$GuestReportPath.write-error.txt"
    "Failed to write guest report: $guestReportError" | Set-Content -LiteralPath $fallbackPath -Encoding UTF8
    if ($status -eq "Passed" -and (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        Move-Item -LiteralPath $reportPath -Destination (Join-Path $EvidenceDir "report.failed-cleanup.json") -Force
    }
    $status = "Failed"
    Write-Error "Required guest evidence was not written: $guestReportError" -ErrorAction Continue
}

Close-RunLock

if ($status -ne "Passed") {
    Write-Error "External Allocate Free Space gate failed. Report: $GuestReportPath" -ErrorAction Continue
    exit 1
}

Write-Host "External Allocate Free Space gate passed. Report: $GuestReportPath"
exit 0
