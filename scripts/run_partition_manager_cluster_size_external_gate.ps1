<#
.SYNOPSIS
    Runs the external cluster-size change gate on disposable VHD media.

.DESCRIPTION
    Creates a temporary VHDX, formats an NTFS fixture volume, seeds files with
    ACLs and alternate data streams, backs up the fixture tree, reformats the
    same volume with a different allocation unit size, restores the tree,
    verifies hashes, ACLs, ADS metadata, and health, writes the matrix-backed
    external evidence report, then dismounts and removes the VHD.
#>

[CmdletBinding()]
param(
    [string]$OutputRoot = "artifacts\partition-manager-certification\vm-lab",
    [string]$EvidenceDir = "",
    [string]$GuestReportPath = "",
    [int]$VhdSizeMB = 512,
    [int]$VolumeSizeMB = 384,
    [int]$TargetAllocationUnitBytes = 16384,
    [switch]$RelaunchElevated,
    [switch]$KeepVhd,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$MinimumVhdSizeMB = 384
$Script:RunRoot = $null
$Script:TranscriptStarted = $false

trap {
    $errorText = ($_ | Format-List * -Force | Out-String)
    if (-not [string]::IsNullOrWhiteSpace($Script:RunRoot) -and
        (Test-Path -LiteralPath $Script:RunRoot -PathType Container)) {
        $errorText | Set-Content -LiteralPath (Join-Path $Script:RunRoot "cluster-size-error.txt") -Encoding UTF8
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
        "-VolumeSizeMB",
        $VolumeSizeMB.ToString(),
        "-TargetAllocationUnitBytes",
        $TargetAllocationUnitBytes.ToString()
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

    Write-Host "Relaunching Cluster Size external gate in an elevated PowerShell window..."
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

function Assert-DisposableDisk {
    param(
        [Parameter(Mandatory = $true)] [object]$Disk,
        [Parameter(Mandatory = $true)] [string]$ExpectedPath
    )

    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Target disk $($Disk.Number) is boot/system disk."
    }
    $imageDisk = Get-DiskImage -ImagePath $ExpectedPath -ErrorAction Stop | Get-Disk -ErrorAction Stop
    if ([int]$imageDisk.Number -ne [int]$Disk.Number) {
        throw "Mounted VHD disk mismatch. Expected $($Disk.Number), got $($imageDisk.Number)."
    }
    $requestedSizeMB = [Math]::Max($VhdSizeMB, $MinimumVhdSizeMB)
    if ($Disk.Size -lt ($MinimumVhdSizeMB * 1MB) -or $Disk.Size -gt (($requestedSizeMB + 64) * 1MB)) {
        throw "Target VHD size outside disposable range: $($Disk.Size)"
    }
}

function New-DisposableVhdDisk {
    $path = Join-Path $Script:RunRoot "cluster-size.vhdx"
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

function Get-FreeCertificationDriveLetter {
    $used = @(Get-Volume -ErrorAction SilentlyContinue |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_.DriveLetter) } |
        ForEach-Object { $_.DriveLetter.ToString().ToUpperInvariant() })
    foreach ($letter in @("T", "U", "V", "W", "X", "Y", "Z")) {
        if ($used -notcontains $letter) {
            return $letter
        }
    }
    throw "Need one free certification drive letter."
}

function Initialize-LabVolume {
    param(
        [Parameter(Mandatory = $true)] [object]$Vhd,
        [Parameter(Mandatory = $true)] [string]$DriveLetter
    )

    Initialize-Disk -Number $Vhd.disk_number -PartitionStyle GPT -ErrorAction Stop
    $partition = New-Partition -DiskNumber $Vhd.disk_number -Size ($VolumeSizeMB * 1MB) -DriveLetter $DriveLetter -ErrorAction Stop
    Format-Volume -DriveLetter $DriveLetter -FileSystem NTFS -NewFileSystemLabel "SAKCLUSTER" -Confirm:$false -Force | Out-Null
    Start-Sleep -Milliseconds 500
    return Get-Partition -DiskNumber $Vhd.disk_number -PartitionNumber $partition.PartitionNumber
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

function Get-AllocationUnitSize {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

    $command = Invoke-NativeCommand -Name "fsutil-ntfsinfo" -FilePath "fsutil.exe" -Arguments @("fsinfo", "ntfsinfo", "$DriveLetter`:")
    if ($command.exit_code -ne 0) {
        throw "fsutil ntfsinfo failed with exit code $($command.exit_code)."
    }
    $match = [regex]::Match($command.output, "Bytes Per Cluster\s*:\s*(\d+)")
    if (-not $match.Success) {
        throw "Unable to parse Bytes Per Cluster from fsutil output."
    }
    return [int]$match.Groups[1].Value
}

function New-SeedTree {
    param([Parameter(Mandatory = $true)] [string]$Root)

    New-Item -ItemType Directory -Path (Join-Path $Root "cert-data\nested") -Force | Out-Null
    "SAK cluster-size fixture $(Get-Date -Format o)" |
        Set-Content -LiteralPath (Join-Path $Root "cert-data\fixture.txt") -Encoding UTF8
    "Nested fixture" |
        Set-Content -LiteralPath (Join-Path $Root "cert-data\nested\child.txt") -Encoding UTF8
    $bytes = New-Object byte[] 32768
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    [System.IO.File]::WriteAllBytes((Join-Path $Root "cert-data\payload.bin"), $bytes)
    Set-Content -LiteralPath (Join-Path $Root "cert-data\fixture.txt") -Stream "sakmeta" -Value "alternate stream payload" -Encoding UTF8

    $secureDir = Join-Path $Root "cert-data\nested"
    $acl = Get-Acl -LiteralPath $secureDir
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        [Security.Principal.WindowsIdentity]::GetCurrent().Name,
        "ReadAndExecute",
        "ContainerInherit,ObjectInherit",
        "None",
        "Allow")
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $secureDir -AclObject $acl
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)] [string]$BasePath,
        [Parameter(Mandatory = $true)] [string]$Path
    )

    $base = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $base.EndsWith("\")) {
        $base += "\"
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    return ($full.Substring($base.Length) -replace "\\", "/")
}

function Get-DirectoryHashManifest {
    param([Parameter(Mandatory = $true)] [string]$Root)

    $resolved = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    @(Get-ChildItem -LiteralPath $resolved.Path -Recurse -File | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{
            relative_path = Get-RelativePath -BasePath $resolved.Path -Path $_.FullName
            length_bytes = [uint64]$_.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
}

function Get-AclManifest {
    param([Parameter(Mandatory = $true)] [string]$Root)

    $resolved = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    @(Get-ChildItem -LiteralPath $resolved.Path -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $acl = Get-Acl -LiteralPath $_.FullName
        $rules = @($acl.Access | Sort-Object IdentityReference, FileSystemRights, AccessControlType | ForEach-Object {
            [pscustomobject]@{
                identity = $_.IdentityReference.ToString()
                rights = $_.FileSystemRights.ToString()
                type = $_.AccessControlType.ToString()
                inherited = [bool]$_.IsInherited
                inheritance = $_.InheritanceFlags.ToString()
                propagation = $_.PropagationFlags.ToString()
            }
        })
        [pscustomobject]@{
            relative_path = Get-RelativePath -BasePath $resolved.Path -Path $_.FullName
            item_type = if ($_.PSIsContainer) { "Directory" } else { "File" }
            access_rules = $rules
        }
    })
}

function Get-AlternateStreamManifest {
    param([Parameter(Mandatory = $true)] [string]$Root)

    $resolved = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    $entries = New-Object System.Collections.Generic.List[object]
    Get-ChildItem -LiteralPath $resolved.Path -Recurse -File | Sort-Object FullName | ForEach-Object {
        $file = $_
        $streams = @(Get-Item -LiteralPath $file.FullName -Stream * -ErrorAction SilentlyContinue |
            Where-Object { $_.Stream -ne ':$DATA' } |
            Sort-Object Stream)
        foreach ($stream in $streams) {
            $content = Get-Content -LiteralPath $file.FullName -Stream $stream.Stream -Raw -ErrorAction Stop
            $bytes = [Text.Encoding]::UTF8.GetBytes([string]$content)
            $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)).Replace("-", "")
            $entries.Add([pscustomobject]@{
                relative_path = Get-RelativePath -BasePath $resolved.Path -Path $file.FullName
                stream = $stream.Stream
                length_bytes = [uint64]$stream.Length
                sha256 = $sha
            })
        }
    }
    $output = @()
    foreach ($entry in $entries) {
        $output += $entry
    }
    return $output
}

function Compare-JsonManifests {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Expected,
        [Parameter(Mandatory = $true)] [object[]]$Actual
    )

    $expectedJson = @($Expected | ForEach-Object { $_ | ConvertTo-Json -Depth 12 -Compress })
    $actualJson = @($Actual | ForEach-Object { $_ | ConvertTo-Json -Depth 12 -Compress })
    $expectedSorted = @($expectedJson | Sort-Object)
    $actualSorted = @($actualJson | Sort-Object)
    $errors = New-Object System.Collections.Generic.List[string]
    if ($expectedSorted.Count -ne $actualSorted.Count) {
        $errors.Add("count:$($expectedSorted.Count)!=$($actualSorted.Count)")
    }
    $max = [Math]::Max($expectedSorted.Count, $actualSorted.Count)
    for ($i = 0; $i -lt $max; $i++) {
        $expectedValue = if ($i -lt $expectedSorted.Count) { $expectedSorted[$i] } else { "<missing>" }
        $actualValue = if ($i -lt $actualSorted.Count) { $actualSorted[$i] } else { "<missing>" }
        if ($expectedValue -ne $actualValue) {
            $errors.Add("mismatch:$i")
        }
    }
    [pscustomobject]@{
        matched = ($errors.Count -eq 0)
        expected_count = $expectedSorted.Count
        actual_count = $actualSorted.Count
        errors = @($errors.ToArray())
    }
}

function Invoke-RepairScan {
    param([Parameter(Mandatory = $true)] [string]$DriveLetter)

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
        gate_id = "external.cluster-size-change"
        gate_name = "Existing-volume cluster-size change proof"
        status = $Status
        manifest = "artifacts\partition-manager-certification\vm-lab\external-evidence.json"
        certification_matrix = "docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json"
        suggested_evidence_path = "artifacts/partition-manager-certification/vm-lab/external-evidence/external.cluster-size-change/report.json"
        safety_contract = @(
            "disposable_volume_only",
            "full_file_tree_copied",
            "metadata_preserved",
            "hash_acl_stream_validation_passed"
        )
        required_evidence_keys = @(
            "volume_id",
            "before_allocation_unit_size",
            "after_allocation_unit_size",
            "file_count",
            "acl_validation",
            "alternate_stream_validation",
            "file_hash_validation",
            "rollback_or_backup_evidence"
        )
        required_evidence_values = $null
        evidence = $Evidence
        artifacts = $Artifacts
        verification_summary = $VerificationSummary
        operator_notes = $OperatorNotes
    }
}

if ($RelaunchElevated -and -not (Test-IsAdmin)) {
    Invoke-ElevatedRelaunch
}
if (-not (Test-IsAdmin)) {
    throw "Run from an elevated PowerShell session, or pass -RelaunchElevated."
}
if ($TargetAllocationUnitBytes -notin @(512, 1024, 2048, 4096, 8192, 16384, 32768, 65536)) {
    throw "Unsupported target allocation unit size: $TargetAllocationUnitBytes"
}
if ($VhdSizeMB -lt ($VolumeSizeMB + 96)) {
    throw "VhdSizeMB too small for requested volume size."
}

$resolvedOutputRoot = Resolve-ProjectPath -Path $OutputRoot
if ([string]::IsNullOrWhiteSpace($EvidenceDir)) {
    $EvidenceDir = Join-Path $resolvedOutputRoot "external-evidence\external.cluster-size-change"
}
else {
    $EvidenceDir = Resolve-ProjectPath -Path $EvidenceDir
}
if ([string]::IsNullOrWhiteSpace($GuestReportPath)) {
    $GuestReportPath = Join-Path $resolvedOutputRoot "external-cluster-size-guest-report.json"
}
else {
    $GuestReportPath = Resolve-ProjectPath -Path $GuestReportPath
}
$Script:RunRoot = Join-Path $resolvedOutputRoot ("cluster-size-stage-debug\run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
New-Item -ItemType Directory -Path $Script:RunRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $GuestReportPath) -Force | Out-Null

try {
    Start-Transcript -Path (Join-Path $Script:RunRoot "run_partition_manager_cluster_size_external_gate.log") -Force | Out-Null
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
$driveLetter = ""
$volumeId = "not-created"
$beforeAllocationUnit = 0
$afterAllocationUnit = 0
$fileHashValidation = $null
$aclValidation = $null
$alternateStreamValidation = $null
$rollbackEvidence = $null
$repairScan = $null
$reportPath = Join-Path $EvidenceDir "report.json"

try {
    $driveLetter = Get-FreeCertificationDriveLetter
    $vhd = New-DisposableVhdDisk
    $partition = Initialize-LabVolume -Vhd $vhd -DriveLetter $driveLetter
    $volumeId = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -DriveLetter $driveLetter
    $root = "$driveLetter`:\"
    $fixtureRoot = Join-Path $root "cert-data"
    New-SeedTree -Root $root

    $beforeAllocationUnit = Get-AllocationUnitSize -DriveLetter $driveLetter
    Assert-Condition -Condition ($beforeAllocationUnit -ne $TargetAllocationUnitBytes) -Message "Initial allocation unit already equals target size."

    $beforeHash = Get-DirectoryHashManifest -Root $fixtureRoot
    $beforeAcl = Get-AclManifest -Root $fixtureRoot
    $beforeAds = Get-AlternateStreamManifest -Root $fixtureRoot
    Assert-Condition -Condition (@($beforeAds).Count -gt 0) -Message "ADS seed stream was not created."

    $beforeHashPath = Join-Path $EvidenceDir "file-hash-before.json"
    $beforeAclPath = Join-Path $EvidenceDir "acl-before.json"
    $beforeAdsPath = Join-Path $EvidenceDir "ads-before.json"
    $beforeHash | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $beforeHashPath -Encoding UTF8
    $beforeAcl | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $beforeAclPath -Encoding UTF8
    $beforeAds | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $beforeAdsPath -Encoding UTF8

    $backupRoot = Join-Path $Script:RunRoot "cluster-backup"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backup = Invoke-NativeCommand -Name "backup-cluster-fixture" -FilePath "robocopy.exe" -Arguments @($fixtureRoot, $backupRoot, "/MIR", "/COPY:DATS", "/DCOPY:DAT", "/R:1", "/W:1", "/NP")
    $commands.Add($backup)
    Assert-RobocopySuccess -Command $backup

    Format-Volume -DriveLetter $driveLetter -FileSystem NTFS -AllocationUnitSize $TargetAllocationUnitBytes -NewFileSystemLabel "SAKCLUSTER" -Confirm:$false -Force | Out-Null
    Start-Sleep -Milliseconds 500
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

    $restore = Invoke-NativeCommand -Name "restore-cluster-fixture" -FilePath "robocopy.exe" -Arguments @($backupRoot, $fixtureRoot, "/MIR", "/COPY:DATS", "/DCOPY:DAT", "/R:1", "/W:1", "/NP")
    $commands.Add($restore)
    Assert-RobocopySuccess -Command $restore

    $afterAllocationUnit = Get-AllocationUnitSize -DriveLetter $driveLetter
    Assert-Condition -Condition ($afterAllocationUnit -eq $TargetAllocationUnitBytes) -Message "Allocation unit size did not change to requested target."

    $afterHash = Get-DirectoryHashManifest -Root $fixtureRoot
    $afterAcl = Get-AclManifest -Root $fixtureRoot
    $afterAds = Get-AlternateStreamManifest -Root $fixtureRoot
    $afterHashPath = Join-Path $EvidenceDir "file-hash-after.json"
    $afterAclPath = Join-Path $EvidenceDir "acl-after.json"
    $afterAdsPath = Join-Path $EvidenceDir "ads-after.json"
    $afterHash | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $afterHashPath -Encoding UTF8
    $afterAcl | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $afterAclPath -Encoding UTF8
    $afterAds | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $afterAdsPath -Encoding UTF8

    $hashComparison = Compare-JsonManifests -Expected $beforeHash -Actual $afterHash
    $aclComparison = Compare-JsonManifests -Expected $beforeAcl -Actual $afterAcl
    $adsComparison = Compare-JsonManifests -Expected $beforeAds -Actual $afterAds
    Assert-Condition -Condition $hashComparison.matched -Message "File hash manifest mismatch after cluster-size change."
    Assert-Condition -Condition $aclComparison.matched -Message "ACL manifest mismatch after cluster-size change."
    Assert-Condition -Condition $adsComparison.matched -Message "ADS manifest mismatch after cluster-size change."

    $repairScan = Invoke-RepairScan -DriveLetter $driveLetter
    $fileHashValidation = [pscustomobject]@{
        matched = $hashComparison.matched
        expected_file_count = $hashComparison.expected_count
        actual_file_count = $hashComparison.actual_count
        errors = @($hashComparison.errors)
        before_manifest_path = ConvertTo-ProjectRelativePath -Path $beforeHashPath
        after_manifest_path = ConvertTo-ProjectRelativePath -Path $afterHashPath
    }
    $aclValidation = [pscustomobject]@{
        matched = $aclComparison.matched
        expected_entry_count = $aclComparison.expected_count
        actual_entry_count = $aclComparison.actual_count
        errors = @($aclComparison.errors)
        before_manifest_path = ConvertTo-ProjectRelativePath -Path $beforeAclPath
        after_manifest_path = ConvertTo-ProjectRelativePath -Path $afterAclPath
    }
    $alternateStreamValidation = [pscustomobject]@{
        matched = $adsComparison.matched
        expected_stream_count = $adsComparison.expected_count
        actual_stream_count = $adsComparison.actual_count
        errors = @($adsComparison.errors)
        before_manifest_path = ConvertTo-ProjectRelativePath -Path $beforeAdsPath
        after_manifest_path = ConvertTo-ProjectRelativePath -Path $afterAdsPath
    }
    $rollbackEvidence = [pscustomobject]@{
        backup_directory = ConvertTo-ProjectRelativePath -Path $backupRoot
        backup_command = "robocopy fixture -> backup /MIR /COPY:DATS /DCOPY:DAT /R:1 /W:1"
        restore_command = "robocopy backup -> fixture /MIR /COPY:DATS /DCOPY:DAT /R:1 /W:1"
        repair_scan = $repairScan
        cleanup_mode = if ($KeepVhd) { "VHD dismounted and retained" } else { "VHD dismounted and removed" }
        run_root = ConvertTo-ProjectRelativePath -Path $Script:RunRoot
    }

    $evidence = [pscustomobject]@{
        volume_id = [pscustomobject]@{
            before = $volumeId
            after = Get-PartitionVolumeId -DiskNumber $vhd.disk_number -PartitionNumber $partition.PartitionNumber -DriveLetter $driveLetter
            drive_letter = "$driveLetter`:"
        }
        before_allocation_unit_size = $beforeAllocationUnit
        after_allocation_unit_size = $afterAllocationUnit
        file_count = @($afterHash).Count
        acl_validation = $aclValidation
        alternate_stream_validation = $alternateStreamValidation
        file_hash_validation = $fileHashValidation
        rollback_or_backup_evidence = $rollbackEvidence
    }
    $artifacts = @(
        (ConvertTo-ProjectRelativePath -Path $GuestReportPath),
        (ConvertTo-ProjectRelativePath -Path $beforeHashPath),
        (ConvertTo-ProjectRelativePath -Path $afterHashPath),
        (ConvertTo-ProjectRelativePath -Path $beforeAclPath),
        (ConvertTo-ProjectRelativePath -Path $afterAclPath),
        (ConvertTo-ProjectRelativePath -Path $beforeAdsPath),
        (ConvertTo-ProjectRelativePath -Path $afterAdsPath),
        (ConvertTo-ProjectRelativePath -Path (Join-Path $Script:RunRoot "run_partition_manager_cluster_size_external_gate.log"))
    )
    $report = New-ExternalReport `
        -Status "Passed" `
        -Evidence $evidence `
        -Artifacts $artifacts `
        -VerificationSummary "Disposable VHD NTFS volume $volumeId changed allocation unit size from $beforeAllocationUnit to $afterAllocationUnit bytes after full fixture backup, reformat, restore, SHA256, ACL, ADS, and Repair-Volume validation." `
        -OperatorNotes "Run used only a generated disposable VHDX under the certification artifact root; no physical disk numbers were selected by operator input."
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    $status = "Passed"
}
catch {
    $blockers.Add(($_ | Format-List * -Force | Out-String).Trim())
    $failedEvidence = [pscustomobject]@{
        volume_id = $volumeId
        before_allocation_unit_size = $beforeAllocationUnit
        after_allocation_unit_size = $afterAllocationUnit
        file_count = 0
        acl_validation = if ($null -ne $aclValidation) { $aclValidation } else { "not-verified" }
        alternate_stream_validation = if ($null -ne $alternateStreamValidation) { $alternateStreamValidation } else { "not-verified" }
        file_hash_validation = if ($null -ne $fileHashValidation) { $fileHashValidation } else { "not-verified" }
        rollback_or_backup_evidence = if ($null -ne $rollbackEvidence) { $rollbackEvidence } else { "not-verified" }
    }
    $failedReport = New-ExternalReport `
        -Status "Failed" `
        -Evidence $failedEvidence `
        -Artifacts @((ConvertTo-ProjectRelativePath -Path $GuestReportPath)) `
        -VerificationSummary "Cluster Size external gate failed." `
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
    tool = "sak-vhd-external-cluster-size-gate"
    status = $status
    vm_id = $env:COMPUTERNAME
    started_at = $startedAt.ToString("o")
    completed_at = (Get-Date).ToString("o")
    user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    is_admin = Test-IsAdmin
    vhd_path = $vhdPathForReport
    vhd_disk_number = $vhdDiskNumberForReport
    evidence_dir = $EvidenceDir
    drive_letter = $driveLetter
    volume_id = $volumeId
    before_allocation_unit_size = $beforeAllocationUnit
    after_allocation_unit_size = $afterAllocationUnit
    file_hash_validation = $fileHashValidation
    acl_validation = $aclValidation
    alternate_stream_validation = $alternateStreamValidation
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
    Write-Error "External Cluster Size gate failed. Report: $GuestReportPath"
    exit 1
}

Write-Host "External Cluster Size gate passed. Report: $GuestReportPath"
exit 0
