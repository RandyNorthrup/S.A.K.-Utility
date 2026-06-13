<#
.SYNOPSIS
    Validates physical Apple HFS+/APFS partition detection read-only.

.DESCRIPTION
    Finds Apple HFS and APFS GPT partitions on a non-boot, non-system disk,
    probes them through partition device aliases, and writes evidence JSON.
    This script never formats, repairs, mounts, or writes the target disk.
#>

[CmdletBinding()]
param(
    [int]$DiskNumber = -1,
    [string]$CertifierPath = "",
    [string]$OutputRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-filesystem-physical",
    [string]$HfsBrowsePath = "/",
    [uint64]$HfsFileProbeMaxBytes = 1048576,
    [switch]$RequireHfsFileProof,
    [uint64]$HfsAttributeProbeMaxBytes = 1048576,
    [switch]$RequireHfsAttributeProof,
    [string]$ApfsBrowsePath = "/",
    [uint64]$ApfsFileProbeMaxBytes = 1048576,
    [int]$ApfsCandidateSearchMaxDirectories = 128,
    [switch]$RequireApfsFileProof,
    [uint64]$ApfsExportMaxEntries = 64,
    [uint64]$ApfsExportMaxFileBytes = 67108864,
    [uint64]$ApfsExportMaxTotalBytes = 536870912,
    [switch]$RequireApfsExportProof,
    [switch]$AllowMissingHfs,
    [switch]$ProbeAllApfsPartitions,
    [switch]$RequireAllApfsPartitions,
    [switch]$AllowBootOrSystemDisk
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$AppleHfsGptType = "{48465300-0000-11aa-aa11-00306543ecac}"
$AppleApfsGptType = "{7c3457ef-0000-11aa-aa11-00306543ecac}"

function Resolve-CertifierPath {
    param([string]$Path)

    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    }

    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function ConvertTo-ResolvedOutputRoot {
    param([string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $ProjectRoot $Path
}

function New-ReportBase {
    param([string]$OutputPath)

    [ordered]@{
        schema_version = 1
        gate_id = "external.apple-filesystem-physical"
        status = "Failed"
        started_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        finished_at_utc = ""
        output_root = $OutputPath
        destructive = $false
        writes_target_media = $false
        validation_requirements = [ordered]@{
            require_hfs_file_proof = [bool]$RequireHfsFileProof
            require_hfs_attribute_proof = [bool]$RequireHfsAttributeProof
            require_apfs_file_proof = [bool]$RequireApfsFileProof
            require_apfs_export_proof = [bool]$RequireApfsExportProof
            allow_missing_hfs = [bool]$AllowMissingHfs
            probe_all_apfs_partitions = [bool]$ProbeAllApfsPartitions
            require_all_apfs_partitions = [bool]$RequireAllApfsPartitions
        }
        probe_limits = [ordered]@{
            hfs_browse_path = $HfsBrowsePath
            hfs_file_probe_max_bytes = "$HfsFileProbeMaxBytes"
            hfs_attribute_probe_max_bytes = "$HfsAttributeProbeMaxBytes"
            apfs_browse_path = $ApfsBrowsePath
            apfs_file_probe_max_bytes = "$ApfsFileProbeMaxBytes"
            apfs_candidate_search_max_directories = $ApfsCandidateSearchMaxDirectories
            apfs_export_max_entries = "$ApfsExportMaxEntries"
            apfs_export_max_file_bytes = "$ApfsExportMaxFileBytes"
            apfs_export_max_total_bytes = "$ApfsExportMaxTotalBytes"
        }
        disk = $null
        results = @()
        hfs_file_proof = $null
        hfs_attribute_proof = $null
        apfs_file_proof = $null
        apfs_export_proof = $null
        apfs_partition_proofs = @()
        artifacts = @()
        error = ""
    }
}

function New-NotRunProof {
    param([string]$Reason)

    [ordered]@{
        status = "NotRun"
        error = $Reason
        artifacts = @()
        output = @()
    }
}

function Assert-SafeDisk {
    param([Parameter(Mandatory = $true)] [object]$Disk)

    if ($AllowBootOrSystemDisk) {
        return
    }
    if ($Disk.IsBoot -or $Disk.IsSystem) {
        throw "Refusing to probe boot/system disk $($Disk.Number). Pass -AllowBootOrSystemDisk only for controlled lab proof."
    }
}

function Select-TargetDisk {
    $allDisks = @(Get-Disk | Sort-Object Number)
    if ($DiskNumber -ge 0) {
        $disk = $allDisks | Where-Object { $_.Number -eq $DiskNumber } | Select-Object -First 1
        if (-not $disk) {
            throw "Disk $DiskNumber was not found."
        }
        Assert-SafeDisk -Disk $disk
        return $disk
    }

    $candidateDisks = @($allDisks | Where-Object {
            $diskPartitions = @(Get-Partition -DiskNumber $_.Number -ErrorAction SilentlyContinue)
            -not $_.IsBoot -and
            -not $_.IsSystem -and
            $_.PartitionStyle -eq "GPT" -and
            @($diskPartitions | Where-Object {
                    "$($_.GptType)".ToLowerInvariant() -in @($AppleHfsGptType, $AppleApfsGptType)
                }).Count -gt 0
        })
    if ($candidateDisks.Count -ne 1) {
        throw "Expected exactly one non-boot GPT disk with Apple partitions; found $($candidateDisks.Count). Pass -DiskNumber."
    }
    return $candidateDisks[0]
}

function Invoke-Certifier {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string[]]$Arguments
    )

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $global:LASTEXITCODE = 0
        $ErrorActionPreference = "Continue"
        $output = & $Path @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        [pscustomobject]@{
            exit_code = $LASTEXITCODE
            output = @($output)
        }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Invoke-PartitionProbe {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [int]$Disk,
        [Parameter(Mandatory = $true)] [object]$Partition,
        [Parameter(Mandatory = $true)] [string]$ExpectedFileSystem,
        [Parameter(Mandatory = $true)] [string]$ReportPath,
        [string[]]$ExtraArguments = @()
    )

    $targetPath = "\\?\GLOBALROOT\Device\Harddisk$Disk\Partition$($Partition.PartitionNumber)"
    $arguments = @(
        "--input", $targetPath,
        "--output", $ReportPath,
        "--expect", $ExpectedFileSystem,
        "--require-sane")
    $arguments += $ExtraArguments
    $result = Invoke-Certifier -Path $Certifier -Arguments $arguments
    $probeReport = if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
        Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    }
    else {
        $null
    }

    [ordered]@{
        partition_number = $Partition.PartitionNumber
        gpt_type = "$($Partition.GptType)"
        offset_bytes = "$($Partition.Offset)"
        size_bytes = "$($Partition.Size)"
        expected_file_system = $ExpectedFileSystem
        target_path = $targetPath
        report_path = $ReportPath
        exit_code = $result.exit_code
        output = @($result.output)
        status = if ($null -ne $probeReport) { $probeReport.status } else { "Failed" }
        detected_file_system = if ($null -ne $probeReport) { $probeReport.detected_file_system } else { "" }
        total_bytes = if ($null -ne $probeReport) { $probeReport.total_bytes } else { "" }
        free_bytes = if ($null -ne $probeReport) { $probeReport.free_bytes } else { "" }
        details = if ($null -ne $probeReport) { @($probeReport.details) } else { @() }
    }
}

function ConvertTo-UInt64OrZero {
    param([object]$Value)

    try {
        return [uint64]$Value
    }
    catch {
        return [uint64]0
    }
}

function Select-HfsFileCandidate {
    param(
        [object[]]$Entries,
        [uint64]$MaxBytes
    )

    @($Entries | Where-Object {
            $name = "$($_.name)"
            $size = ConvertTo-UInt64OrZero -Value $_.size_bytes
            $_.regular_file -eq $true -and
            $size -gt 0 -and
            $size -le $MaxBytes -and
            -not $name.StartsWith(".")
        } | Select-Object -First 1)
}

function Select-HfsDirectoryCandidate {
    param([object[]]$Entries)

    @($Entries | Where-Object {
            $name = "$($_.name)"
            $_.directory -eq $true -and -not $name.StartsWith(".")
        } | Sort-Object name | Select-Object -First 1)
}

function Invoke-HfsFileProof {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [string]$HfsProbeReportPath,
        [Parameter(Mandatory = $true)] [string]$OutputRoot,
        [uint64]$MaxBytes
    )

    $artifacts = @()
    $proof = [ordered]@{
        status = "NotRun"
        selected_file_path = ""
        bytes_read = "0"
        sha256 = ""
        artifacts = @()
        output = @()
        error = ""
    }
    if (-not (Test-Path -LiteralPath $HfsProbeReportPath -PathType Leaf)) {
        $proof.error = "HFS probe report missing."
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $hfsProbeReport = Get-Content -LiteralPath $HfsProbeReportPath -Raw | ConvertFrom-Json
    $entries = @($hfsProbeReport.hfs_listing.entries)
    $candidate = Select-HfsFileCandidate -Entries $entries -MaxBytes $MaxBytes | Select-Object -First 1
    if (-not $candidate) {
        $directory = Select-HfsDirectoryCandidate -Entries $entries | Select-Object -First 1
        if ($directory) {
            $nestedListPath = Join-Path $OutputRoot "hfs-list-file-candidate-directory.json"
            $nestedResult = Invoke-Certifier -Path $Certifier -Arguments @(
                "--input", $TargetPath,
                "--output", $nestedListPath,
                "--expect", "HFS+",
                "--require-sane",
                "--hfs-list-path", "$($directory.path)")
            $artifacts += $nestedListPath
            $proof.output += @($nestedResult.output)
            if ($nestedResult.exit_code -eq 0 -and (Test-Path -LiteralPath $nestedListPath -PathType Leaf)) {
                $nestedReport = Get-Content -LiteralPath $nestedListPath -Raw | ConvertFrom-Json
                $candidate = Select-HfsFileCandidate -Entries @($nestedReport.hfs_listing.entries) -MaxBytes $MaxBytes |
                    Select-Object -First 1
            }
        }
    }

    if (-not $candidate) {
        $proof.status = "Failed"
        $proof.error = "No regular HFS+ file within probe byte cap was found."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readPath = Join-Path $OutputRoot "hfs-read-file-proof.json"
    $readResult = Invoke-Certifier -Path $Certifier -Arguments @(
        "--input", $TargetPath,
        "--output", $readPath,
        "--expect", "HFS+",
        "--require-sane",
        "--hfs-read-file", "$($candidate.path)",
        "--hfs-read-max-bytes", "$MaxBytes")
    $artifacts += $readPath
    $proof.output += @($readResult.output)
    if ($readResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $readPath -PathType Leaf)) {
        $proof.status = "Failed"
        $proof.selected_file_path = "$($candidate.path)"
        $proof.error = "HFS+ file read proof failed."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readReport = Get-Content -LiteralPath $readPath -Raw | ConvertFrom-Json
    $proof.status = $readReport.hfs_read_file.status
    $proof.selected_file_path = $readReport.hfs_read_file.path
    $proof.bytes_read = "$($readReport.hfs_read_file.bytes_read)"
    $proof.sha256 = $readReport.hfs_read_file.sha256
    $proof.artifacts = $artifacts
    if ($proof.status -ne "Passed") {
        $proof.error = "HFS+ file read proof returned status $($proof.status)."
    }
    return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
}

function Select-HfsAttributeCandidate {
    param(
        [object[]]$Records,
        [object[]]$Metadata
    )

    foreach ($record in @($Records)) {
        if ($record.readable -ne $true) {
            continue
        }
        $size = ConvertTo-UInt64OrZero -Value $record.size_bytes
        if ($size -eq 0) {
            continue
        }
        return [pscustomobject]@{
            name = "$($record.name)"
            file_id = "$($record.file_id)"
            description = "$($record.storage) attribute size $size bytes"
        }
    }

    foreach ($item in @($Metadata)) {
        $text = "$item"
        $match = [regex]::Match($text, '^(?<name>.+) on file-id (?<fileId>\d+): (?<description>.+)$')
        if (-not $match.Success) {
            continue
        }
        $description = $match.Groups["description"].Value
        if (-not $description.Contains("attribute size")) {
            continue
        }
        return [pscustomobject]@{
            name = $match.Groups["name"].Value
            file_id = $match.Groups["fileId"].Value
            description = $description
        }
    }
    return $null
}

function Invoke-HfsAttributeProof {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [string]$OutputRoot,
        [uint64]$MaxBytes
    )

    $artifacts = @()
    $proof = [ordered]@{
        status = "NotRun"
        file_id = ""
        attribute_name = ""
        storage = ""
        bytes_read = "0"
        sha256 = ""
        artifacts = @()
        output = @()
        error = ""
    }

    $checkPath = Join-Path $OutputRoot "hfs-check-attribute-scan.json"
    $checkResult = Invoke-Certifier -Path $Certifier -Arguments @(
        "--input", $TargetPath,
        "--output", $checkPath,
        "--expect", "HFS+",
        "--require-sane",
        "--hfs-check")
    $artifacts += $checkPath
    $proof.output += @($checkResult.output)
    if ($checkResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $checkPath -PathType Leaf)) {
        $proof.status = "Failed"
        $proof.error = "HFS+ attribute scan proof failed."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $checkReport = Get-Content -LiteralPath $checkPath -Raw | ConvertFrom-Json
    $candidate = Select-HfsAttributeCandidate `
        -Records @($checkReport.hfs_check.attribute_records) `
        -Metadata @($checkReport.hfs_check.attribute_metadata)
    if (-not $candidate) {
        $proof.status = "Failed"
        $proof.error = "No readable HFS+ attribute candidate was found."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readPath = Join-Path $OutputRoot "hfs-read-attribute-proof.json"
    $readResult = Invoke-Certifier -Path $Certifier -Arguments @(
        "--input", $TargetPath,
        "--output", $readPath,
        "--expect", "HFS+",
        "--require-sane",
        "--hfs-read-attribute-file-id", "$($candidate.file_id)",
        "--hfs-read-attribute-name", "$($candidate.name)",
        "--hfs-read-max-bytes", "$MaxBytes")
    $artifacts += $readPath
    $proof.output += @($readResult.output)
    if ($readResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $readPath -PathType Leaf)) {
        $proof.status = "Failed"
        $proof.file_id = "$($candidate.file_id)"
        $proof.attribute_name = "$($candidate.name)"
        $proof.error = "HFS+ attribute read proof failed."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readReport = Get-Content -LiteralPath $readPath -Raw | ConvertFrom-Json
    $proof.status = $readReport.hfs_read_attribute.status
    $proof.file_id = "$($readReport.hfs_read_attribute.file_id)"
    $proof.attribute_name = $readReport.hfs_read_attribute.attribute_name
    $proof.storage = $readReport.hfs_read_attribute.storage
    $proof.bytes_read = "$($readReport.hfs_read_attribute.bytes_read)"
    $proof.sha256 = $readReport.hfs_read_attribute.sha256
    $proof.artifacts = $artifacts
    if ($proof.status -ne "Passed") {
        $proof.error = "HFS+ attribute read proof returned status $($proof.status)."
    }
    return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
}

function Select-ApfsFileCandidate {
    param(
        [object[]]$Entries,
        [uint64]$MaxBytes
    )

    @($Entries | Where-Object {
            $name = "$($_.name)"
            $size = ConvertTo-UInt64OrZero -Value $_.size_bytes
            $_.regular_file -eq $true -and
            $size -gt 0 -and
            $size -le $MaxBytes -and
            -not $name.StartsWith(".")
        } | Select-Object -First 1)
}

function Add-ApfsDirectoryCandidates {
    param(
        [object[]]$Entries,
        [Parameter(Mandatory = $true)] [object]$Queue,
        [Parameter(Mandatory = $true)] [object]$Seen
    )

    foreach ($directory in @($Entries | Where-Object {
                $_.directory -eq $true
            } | Sort-Object path)) {
        $path = "$($directory.path)"
        if ([string]::IsNullOrWhiteSpace($path)) {
            continue
        }
        if ($Seen.Add($path)) {
            $Queue.Enqueue($path)
        }
    }
}

function Find-ApfsFileCandidate {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [string]$OutputRoot,
        [object[]]$Entries,
        [uint64]$MaxBytes,
        [int]$MaxDirectories
    )

    $artifacts = @()
    $output = @()
    $candidate = Select-ApfsFileCandidate -Entries $Entries -MaxBytes $MaxBytes |
        Select-Object -First 1
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    Add-ApfsDirectoryCandidates -Entries $Entries -Queue $queue -Seen $seen

    $directoriesScanned = 0
    $directoryLimit = [Math]::Max(0, $MaxDirectories)
    while (-not $candidate -and $queue.Count -gt 0 -and $directoriesScanned -lt $directoryLimit) {
        $directory = $queue.Dequeue()
        $directoriesScanned++
        $nestedListPath = Join-Path $OutputRoot ("apfs-list-file-candidate-{0:D3}.json" -f $directoriesScanned)
        $nestedResult = Invoke-Certifier -Path $Certifier -Arguments @(
            "--input", $TargetPath,
            "--output", $nestedListPath,
            "--expect", "APFS",
            "--require-sane",
            "--apfs-list-path", $directory)
        $artifacts += $nestedListPath
        $output += @($nestedResult.output)
        if ($nestedResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $nestedListPath -PathType Leaf)) {
            continue
        }

        $nestedReport = Get-Content -LiteralPath $nestedListPath -Raw | ConvertFrom-Json
        $nestedEntries = @($nestedReport.apfs_listing.entries)
        $candidate = Select-ApfsFileCandidate -Entries $nestedEntries -MaxBytes $MaxBytes |
            Select-Object -First 1
        if (-not $candidate) {
            Add-ApfsDirectoryCandidates -Entries $nestedEntries -Queue $queue -Seen $seen
        }
    }

    [pscustomobject]@{
        candidate = $candidate
        artifacts = $artifacts
        output = $output
        directories_scanned = $directoriesScanned
        directories_queued = $seen.Count
    }
}

function Invoke-ApfsFileProof {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [string]$ApfsProbeReportPath,
        [Parameter(Mandatory = $true)] [string]$OutputRoot,
        [uint64]$MaxBytes,
        [int]$MaxCandidateDirectories
    )

    $artifacts = @()
    $proof = [ordered]@{
        status = "NotRun"
        selected_file_path = ""
        bytes_read = "0"
        sha256 = ""
        candidate_directories_scanned = 0
        candidate_directories_queued = 0
        artifacts = @()
        output = @()
        error = ""
    }
    if (-not (Test-Path -LiteralPath $ApfsProbeReportPath -PathType Leaf)) {
        $proof.error = "APFS probe report missing."
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $apfsProbeReport = Get-Content -LiteralPath $ApfsProbeReportPath -Raw | ConvertFrom-Json
    $entries = @($apfsProbeReport.apfs_listing.entries)
    $candidateSearch = Find-ApfsFileCandidate `
        -Certifier $Certifier `
        -TargetPath $TargetPath `
        -OutputRoot $OutputRoot `
        -Entries $entries `
        -MaxBytes $MaxBytes `
        -MaxDirectories $MaxCandidateDirectories
    $candidate = $candidateSearch.candidate
    $artifacts += @($candidateSearch.artifacts)
    $proof.output += @($candidateSearch.output)
    $proof.candidate_directories_scanned = $candidateSearch.directories_scanned
    $proof.candidate_directories_queued = $candidateSearch.directories_queued

    if (-not $candidate) {
        $proof.status = "Failed"
        $proof.error = "No regular APFS file within probe byte cap was found after scanning $($proof.candidate_directories_scanned) directories."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readPath = Join-Path $OutputRoot "apfs-read-file-proof.json"
    $readResult = Invoke-Certifier -Path $Certifier -Arguments @(
        "--input", $TargetPath,
        "--output", $readPath,
        "--expect", "APFS",
        "--require-sane",
        "--apfs-read-file", "$($candidate.path)",
        "--apfs-read-max-bytes", "$MaxBytes")
    $artifacts += $readPath
    $proof.output += @($readResult.output)
    if ($readResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $readPath -PathType Leaf)) {
        $proof.status = "Failed"
        $proof.selected_file_path = "$($candidate.path)"
        $proof.error = "APFS file read proof failed."
        $proof.artifacts = $artifacts
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $readReport = Get-Content -LiteralPath $readPath -Raw | ConvertFrom-Json
    $proof.status = $readReport.apfs_read_file.status
    $proof.selected_file_path = $readReport.apfs_read_file.path
    $proof.bytes_read = "$($readReport.apfs_read_file.bytes_read)"
    $proof.sha256 = $readReport.apfs_read_file.sha256
    $proof.artifacts = $artifacts
    if ($proof.status -ne "Passed") {
        $proof.error = "APFS file read proof returned status $($proof.status)."
    }
    return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
}

function Get-ApfsParentPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -eq "/") {
        return "/"
    }
    $lastSlash = $Path.LastIndexOf("/")
    if ($lastSlash -le 0) {
        return "/"
    }
    return $Path.Substring(0, $lastSlash)
}

function Invoke-ApfsExportProof {
    param(
        [Parameter(Mandatory = $true)] [string]$Certifier,
        [Parameter(Mandatory = $true)] [string]$TargetPath,
        [Parameter(Mandatory = $true)] [object]$ApfsFileProof,
        [Parameter(Mandatory = $true)] [string]$OutputRoot,
        [uint64]$MaxEntries,
        [uint64]$MaxFileBytes,
        [uint64]$MaxTotalBytes
    )

    $artifacts = @()
    $proof = [ordered]@{
        status = "NotRun"
        source_path = ""
        files_exported = 0
        directories_exported = 0
        symlinks_skipped = 0
        entries_scanned = 0
        bytes_exported = "0"
        artifacts = @()
        output = @()
        error = ""
    }
    if ($ApfsFileProof.status -ne "Passed" -or [string]::IsNullOrWhiteSpace($ApfsFileProof.selected_file_path)) {
        $proof.error = "APFS file proof must pass before export proof."
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $sourcePath = Get-ApfsParentPath -Path $ApfsFileProof.selected_file_path
    $exportOutput = Join-Path $OutputRoot "apfs-export-proof-output"
    $exportReport = Join-Path $OutputRoot "apfs-export-proof.json"
    if (Test-Path -LiteralPath $exportOutput) {
        Remove-Item -LiteralPath $exportOutput -Recurse -Force
    }
    New-Item -ItemType Directory -Path $exportOutput -Force | Out-Null
    $exportResult = Invoke-Certifier -Path $Certifier -Arguments @(
        "--input", $TargetPath,
        "--output", $exportReport,
        "--expect", "APFS",
        "--require-sane",
        "--apfs-export-path", $sourcePath,
        "--apfs-export-output", $exportOutput,
        "--apfs-export-max-entries", "$MaxEntries",
        "--apfs-export-max-file-bytes", "$MaxFileBytes",
        "--apfs-export-max-total-bytes", "$MaxTotalBytes")
    $artifacts += $exportReport
    $proof.output += @($exportResult.output)
    $proof.source_path = $sourcePath
    if ($exportResult.exit_code -ne 0 -or -not (Test-Path -LiteralPath $exportReport -PathType Leaf)) {
        $proof.status = "Failed"
        $proof.error = "APFS directory export proof failed."
        $proof.artifacts = $artifacts
        Remove-Item -LiteralPath $exportOutput -Recurse -Force -ErrorAction SilentlyContinue
        return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
    }

    $report = Get-Content -LiteralPath $exportReport -Raw | ConvertFrom-Json
    $proof.status = $report.apfs_export.status
    $proof.files_exported = $report.apfs_export.files_exported
    $proof.directories_exported = $report.apfs_export.directories_exported
    $proof.symlinks_skipped = $report.apfs_export.symlinks_skipped
    $proof.entries_scanned = $report.apfs_export.entries_scanned
    $proof.bytes_exported = "$($report.apfs_export.bytes_exported)"
    $proof.artifacts = $artifacts
    if ($proof.status -ne "Passed") {
        $proof.error = "APFS directory export proof returned status $($proof.status)."
    }
    Remove-Item -LiteralPath $exportOutput -Recurse -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ proof = $proof; artifacts = $artifacts }
}

Push-Location $ProjectRoot
try {
    $certifier = Resolve-CertifierPath -Path $CertifierPath
    $resolvedOutputRoot = ConvertTo-ResolvedOutputRoot -Path $OutputRoot
    New-Item -ItemType Directory -Path $resolvedOutputRoot -Force | Out-Null
    $reportPath = Join-Path $resolvedOutputRoot "report.json"
    $report = New-ReportBase -OutputPath $resolvedOutputRoot

    $disk = Select-TargetDisk
    $partitions = @($disk | Get-Partition | Sort-Object PartitionNumber)
    $hfsPartition = $partitions | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleHfsGptType } |
        Select-Object -First 1
    $apfsPartitions = @($partitions | Where-Object { "$($_.GptType)".ToLowerInvariant() -eq $AppleApfsGptType })
    if (-not $hfsPartition -and -not $AllowMissingHfs) {
        throw "Disk $($disk.Number) does not contain an Apple HFS partition."
    }
    if ($apfsPartitions.Count -eq 0) {
        throw "Disk $($disk.Number) does not contain an Apple APFS partition."
    }
    $selectedApfsPartitions = if ($ProbeAllApfsPartitions) {
        $apfsPartitions
    }
    else {
        @($apfsPartitions | Select-Object -First 1)
    }

    $report.disk = [ordered]@{
        number = $disk.Number
        friendly_name = $disk.FriendlyName
        serial_number = $disk.SerialNumber
        bus_type = "$($disk.BusType)"
        partition_style = "$($disk.PartitionStyle)"
        size_bytes = "$($disk.Size)"
        is_boot = [bool]$disk.IsBoot
        is_system = [bool]$disk.IsSystem
    }

    $results = @()
    $artifacts = @()
    if ($hfsPartition) {
        $hfsReportPath = Join-Path $resolvedOutputRoot "hfs-probe.json"
        $results += Invoke-PartitionProbe -Certifier $certifier -Disk $disk.Number -Partition $hfsPartition -ExpectedFileSystem "HFS+" -ReportPath $hfsReportPath -ExtraArguments @("--hfs-list-path", $HfsBrowsePath)
        $artifacts += $hfsReportPath
    }
    else {
        $hfsReportPath = ""
    }
    foreach ($partition in $selectedApfsPartitions) {
        $apfsReportPath = if ($ProbeAllApfsPartitions) {
            Join-Path $resolvedOutputRoot ("apfs-p{0}-probe.json" -f $partition.PartitionNumber)
        }
        else {
            Join-Path $resolvedOutputRoot "apfs-probe.json"
        }
        $results += Invoke-PartitionProbe -Certifier $certifier -Disk $disk.Number -Partition $partition -ExpectedFileSystem "APFS" -ReportPath $apfsReportPath -ExtraArguments @("--apfs-list-path", $ApfsBrowsePath)
        $artifacts += $apfsReportPath
    }
    $report.results = $results
    $hfsResult = $results | Where-Object { $_.expected_file_system -eq "HFS+" } | Select-Object -First 1
    if ($hfsResult) {
        $hfsFileProof = Invoke-HfsFileProof -Certifier $certifier -TargetPath $hfsResult.target_path -HfsProbeReportPath $hfsReportPath -OutputRoot $resolvedOutputRoot -MaxBytes $HfsFileProbeMaxBytes
        $report.hfs_file_proof = $hfsFileProof.proof
        $artifacts += @($hfsFileProof.artifacts)
        $hfsAttributeProof = Invoke-HfsAttributeProof -Certifier $certifier -TargetPath $hfsResult.target_path -OutputRoot $resolvedOutputRoot -MaxBytes $HfsAttributeProbeMaxBytes
        $report.hfs_attribute_proof = $hfsAttributeProof.proof
        $artifacts += @($hfsAttributeProof.artifacts)
    }
    else {
        $report.hfs_file_proof = New-NotRunProof -Reason "No Apple HFS partition was present on disk $($disk.Number)."
        $report.hfs_attribute_proof = New-NotRunProof -Reason "No Apple HFS partition was present on disk $($disk.Number)."
    }

    $apfsPartitionProofs = @()
    foreach ($apfsResult in @($results | Where-Object { $_.expected_file_system -eq "APFS" })) {
        $partitionOutputRoot = if ($ProbeAllApfsPartitions) {
            Join-Path $resolvedOutputRoot ("apfs-partition-{0}" -f $apfsResult.partition_number)
        }
        else {
            $resolvedOutputRoot
        }
        New-Item -ItemType Directory -Path $partitionOutputRoot -Force | Out-Null
        $apfsFileProof = Invoke-ApfsFileProof -Certifier $certifier -TargetPath $apfsResult.target_path -ApfsProbeReportPath $apfsResult.report_path -OutputRoot $partitionOutputRoot -MaxBytes $ApfsFileProbeMaxBytes -MaxCandidateDirectories $ApfsCandidateSearchMaxDirectories
        $artifacts += @($apfsFileProof.artifacts)
        $apfsExportProof = Invoke-ApfsExportProof -Certifier $certifier -TargetPath $apfsResult.target_path -ApfsFileProof $apfsFileProof.proof -OutputRoot $partitionOutputRoot -MaxEntries $ApfsExportMaxEntries -MaxFileBytes $ApfsExportMaxFileBytes -MaxTotalBytes $ApfsExportMaxTotalBytes
        $artifacts += @($apfsExportProof.artifacts)
        $apfsPartitionProofs += [ordered]@{
            partition_number = $apfsResult.partition_number
            target_path = $apfsResult.target_path
            probe_report_path = $apfsResult.report_path
            file_proof = $apfsFileProof.proof
            export_proof = $apfsExportProof.proof
        }
    }
    $report.apfs_partition_proofs = $apfsPartitionProofs
    $firstApfsPartitionProof = @($apfsPartitionProofs | Select-Object -First 1)
    $report.apfs_file_proof = if ($firstApfsPartitionProof.Count -gt 0) {
        $firstApfsPartitionProof[0].file_proof
    }
    else {
        New-NotRunProof -Reason "No Apple APFS partition proof was produced."
    }
    $report.apfs_export_proof = if ($firstApfsPartitionProof.Count -gt 0) {
        $firstApfsPartitionProof[0].export_proof
    }
    else {
        New-NotRunProof -Reason "No Apple APFS partition proof was produced."
    }
    $report.artifacts = $artifacts
    $failed = @($results | Where-Object { $_.status -ne "Passed" -or $_.exit_code -ne 0 })
    if ($RequireHfsFileProof -and $report.hfs_file_proof.status -ne "Passed") {
        $failed += [pscustomobject]@{ status = $report.hfs_file_proof.status; exit_code = 1 }
    }
    if ($RequireHfsAttributeProof -and $report.hfs_attribute_proof.status -ne "Passed") {
        $failed += [pscustomobject]@{ status = $report.hfs_attribute_proof.status; exit_code = 1 }
    }
    if ($RequireApfsFileProof -and $report.apfs_file_proof.status -ne "Passed") {
        $failed += [pscustomobject]@{ status = $report.apfs_file_proof.status; exit_code = 1 }
    }
    if ($RequireApfsExportProof -and $report.apfs_export_proof.status -ne "Passed") {
        $failed += [pscustomobject]@{ status = $report.apfs_export_proof.status; exit_code = 1 }
    }
    if ($RequireAllApfsPartitions) {
        foreach ($proof in @($report.apfs_partition_proofs)) {
            if ($RequireApfsFileProof -and $proof.file_proof.status -ne "Passed") {
                $failed += [pscustomobject]@{ status = $proof.file_proof.status; exit_code = 1 }
            }
            if ($RequireApfsExportProof -and $proof.export_proof.status -ne "Passed") {
                $failed += [pscustomobject]@{ status = $proof.export_proof.status; exit_code = 1 }
            }
        }
    }
    if ($failed.Count -gt 0) {
        $report.error = "One or more Apple filesystem physical probes failed."
    }
    else {
        $report.status = "Passed"
    }
    $report.finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    if ($report.status -ne "Passed") {
        throw $report.error
    }
    Write-Host "Apple filesystem physical probe validation passed: $reportPath"
}
catch {
    if ($null -ne $report) {
        $report.status = "Failed"
        $report.error = $_.Exception.Message
        $report.finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    }
    throw
}
finally {
    Pop-Location
}
