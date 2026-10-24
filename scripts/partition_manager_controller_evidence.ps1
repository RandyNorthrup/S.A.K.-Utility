<#
.SYNOPSIS
    Validates current, app-driven CreateImage/RestoreImage certification evidence.
.DESCRIPTION
    Dot-source this library, then call Assert-PartitionManagerControllerEvidence with all four
    explicit paths. Expected binary and source manifest paths must come from the caller's build
    selection, never from report claims. This library reads evidence; it never executes storage
    operations, rewrites reports, accepts legacy Copy-Item proof, or guesses artifact locations.
    Supplementary schema-2 reports use Assert-PartitionManagerImageProofEvidence with separately
    selected mode, validation-only intent and independent fixture/artifact expectations. They
    never enter the default schema-1 restore route. AdmissionOnly is not runtime proof.
#>

function Assert-PmControllerCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Controller evidence refused: $Message" }
}

function Get-PmControllerProperty {
    param([object]$Object, [string]$Name)
    Assert-PmControllerCondition ($Object -is [pscustomobject]) "Expected JSON object for $Name"
    $property = $Object.PSObject.Properties[$Name]
    Assert-PmControllerCondition ($null -ne $property) "Missing required field: $Name"
    return ,$property.Value
}

function Assert-PmControllerBoolean {
    param([object]$Value, [bool]$Expected, [string]$Name)
    Assert-PmControllerCondition ($Value -is [bool] -and $Value -eq $Expected) `
        "$Name must be the JSON boolean $Expected"
}

function Assert-PmControllerText {
    param([object]$Value, [string]$Expected, [string]$Name)
    Assert-PmControllerCondition ($Value -is [string] -and $Value -ceq $Expected) `
        "$Name must equal '$Expected'"
}

function Assert-PmControllerHash {
    param([object]$Value, [string]$Name)
    Assert-PmControllerCondition ($Value -is [string] -and $Value -cmatch '^[0-9a-f]{64}\z') `
        "$Name must be a lowercase SHA-256 digest"
}

function Assert-PmControllerInteger {
    param([object]$Value, [long]$Expected, [string]$Name)
    $integer = $Value -is [int] -or $Value -is [long]
    Assert-PmControllerCondition ($integer -and $Value -eq $Expected) "$Name has invalid integer value"
}

function Assert-PmControllerUuid {
    param([object]$Value, [string]$Name)
    $parsed = [guid]::Empty
    $valid = $Value -is [string] -and [guid]::TryParse($Value, [ref]$parsed) -and
        $parsed -ne [guid]::Empty -and $parsed.ToString('D') -ceq $Value
    Assert-PmControllerCondition $valid "$Name must be a canonical nonempty UUID"
}

function Assert-PmControllerDirectPath {
    param([string]$Path)
    $current = $Path
    for ($depth = 0; $depth -lt 128; $depth++) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-PmControllerCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Reparse path: $current"
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { return }
        $current = $parent.FullName
    }
    throw 'Controller evidence refused: path ancestor bound exceeded'
}

function Resolve-PmControllerLocalPath {
    param([string]$Path, [ValidateSet('File', 'Directory')] [string]$Kind)
    Assert-PmControllerCondition (-not [string]::IsNullOrWhiteSpace($Path) -and
        $Path -match '^[A-Za-z]:[\\/]' -and $Path.Substring(2) -notmatch ':' -and
        $Path -notmatch '[\x00-\x1f]') 'An explicit local drive-qualified path is required'
    $native = $Path.Replace('/', '\')
    if ($Kind -eq 'Directory' -and $native.Length -gt 3) { $native = $native.TrimEnd('\') }
    $full = [IO.Path]::GetFullPath($native)
    Assert-PmControllerCondition ($native -ceq $full) "Noncanonical path: $Path"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-PmControllerCondition (($Kind -eq 'Directory') -eq $item.PSIsContainer) `
        "Expected $Kind at $full"
    Assert-PmControllerDirectPath $full
    return $full
}

function Read-PmControllerBoundedBytes {
    param([string]$Path, [int]$MaximumBytes)
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        Assert-PmControllerCondition ($stream.Length -le $MaximumBytes) "Input exceeds byte limit: $Path"
        $memory = [IO.MemoryStream]::new()
        try {
            $buffer = New-Object byte[] 8192
            while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                Assert-PmControllerCondition ($memory.Length + $read -le $MaximumBytes) `
                    "Input grew beyond byte limit: $Path"
                $memory.Write($buffer, 0, $read)
            }
            return ,$memory.ToArray()
        } finally { $memory.Dispose() }
    } finally { $stream.Dispose() }
}

function Get-PmControllerSha256 {
    param([string]$Path)
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-PmControllerBytesSha256 {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-PmControllerRequiredSources {
    return @(
        'CMakeLists.txt', 'tests/CMakeLists.txt', 'cmake/SakSourceFingerprint.cmake',
        'scripts/verify_source_fingerprint.py',
        'tests/certification/partition_manager_live_certifier.cpp',
        'src/core/file_recovery_engine.cpp', 'src/core/apfs_crypto.cpp', 'src/core/apfs_keybag.cpp',
        'src/core/partition_apfs_file_system_reader.cpp', 'src/core/partition_apfs_writer.cpp',
        'src/core/partition_ext_file_system_reader.cpp', 'src/core/partition_hfs_file_system_reader.cpp',
        'src/core/partition_hfs_file_system_writer.cpp', 'src/core/partition_raw_device_io.cpp',
        'src/core/partition_manager_types.cpp', 'src/core/partition_file_system_detector.cpp',
        'src/core/partition_file_system_registry.cpp', 'src/core/partition_file_system_tool_manifest.cpp',
        'src/core/partition_file_system_tool_runner.cpp', 'src/core/storage_inventory_worker.cpp',
        'src/core/elevation_broker.cpp', 'src/core/partition_safety_validator.cpp',
        'src/core/partition_script_builder.cpp', 'src/core/partition_executor.cpp',
        'src/core/partition_operation_planner.cpp', 'src/core/partition_operation_queue.cpp',
        'src/core/process_runner.cpp', 'src/core/logger.cpp', 'src/core/elevation_manager.cpp',
        'src/core/file_hash.cpp', 'src/core/partition_manager_controller.cpp',
        'src/core/partition_report_generator.cpp', 'src/core/sak_lzvn_shim.c',
        'third_party/lzfse/src/lzfse_decode.c', 'third_party/lzfse/src/lzfse_decode_base.c',
        'third_party/lzfse/src/lzfse_encode.c', 'third_party/lzfse/src/lzfse_encode_base.c',
        'third_party/lzfse/src/lzfse_fse.c', 'third_party/lzfse/src/lzvn_decode_base.c',
        'third_party/lzfse/src/lzvn_encode_base.c'
    )
}

function Resolve-PmControllerSourcePath {
    param([string]$Root, [string]$Relative, [hashtable]$DirectoryCache)
    $parts = $Relative.Split('/')
    $unsafe = [string]::IsNullOrWhiteSpace($Relative) -or $Relative.Length -gt 4096 -or
        $Relative -match '[\\:\x00-\x1f]' -or
        @($parts | Where-Object { $_ -in @('', '.', '..') -or $_ -match '[ .]$' }).Count -gt 0
    Assert-PmControllerCondition (-not $unsafe) "Unsafe source-manifest path: $Relative"
    $current = $Root
    foreach ($part in $parts) {
        if (-not $DirectoryCache.ContainsKey($current)) {
            $entries = @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)
            Assert-PmControllerCondition ($entries.Count -le 16384) 'Source directory entry bound exceeded'
            $names = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
            foreach ($entry in $entries) { $names.Add($entry.Name, $entry) }
            $DirectoryCache[$current] = $names
        }
        $names = $DirectoryCache[$current]
        Assert-PmControllerCondition ($names.ContainsKey($part)) "Missing/noncanonical source path: $Relative"
        $item = $names[$part]
        Assert-PmControllerCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Reparse source path: $Relative"
        $current = $item.FullName
    }
    Assert-PmControllerCondition (-not $item.PSIsContainer) "Source record names a directory: $Relative"
    return $current
}

function Get-PmControllerCurrentHeaders {
    param([string]$Root, [string]$RelativeDirectory = 'include/sak',
        [string]$TreeName = 'Public-header', [string[]]$Extensions = @('.h', '.hpp'))
    $include = Join-Path $Root $RelativeDirectory
    Assert-PmControllerDirectPath $include
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($include)
    $headers = [Collections.Generic.List[string]]::new()
    $visited = 0
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Dequeue() -Force -ErrorAction Stop) {
            $visited++
            Assert-PmControllerCondition ($visited -le 16384) "$TreeName tree entry bound exceeded"
            Assert-PmControllerCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
                "$TreeName tree contains a reparse entry"
            if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
            elseif ($item.Extension -in $Extensions) {
                $headers.Add($item.FullName.Substring($Root.Length + 1).Replace('\', '/'))
            }
        }
    }
    Assert-PmControllerCondition ($headers.Count -gt 0) "Current $TreeName universe is empty"
    return ,$headers.ToArray()
}

function Assert-PmControllerSourceManifest {
    param([string]$ManifestPath, [string]$Root, [string]$ExpectedDigest)
    [byte[]]$bytes = Read-PmControllerBoundedBytes $ManifestPath (4 * 1024 * 1024)
    Assert-PmControllerCondition ((Get-PmControllerBytesSha256 $bytes) -ceq $ExpectedDigest) `
        'Source digest does not match build-produced manifest'
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $text = $utf8.GetString($bytes)
    Assert-PmControllerCondition ($text.Length -gt 0 -and $text.EndsWith("`n") -and
        -not $text.Contains("`r") -and [int]$text[0] -ne 0xfeff) 'Source manifest must be canonical UTF-8/LF records'
    $records = $text.Substring(0, $text.Length - 1).Split("`n")
    Assert-PmControllerCondition ($records.Count -gt 0 -and $records.Count -le 8192) 'Invalid source record count'
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $canonicalPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $cache = @{}
    foreach ($record in $records) {
        Assert-PmControllerCondition ($record -cmatch '^(?<path>[^:]+):(?<hash>[0-9a-f]{64})$') `
            'Malformed source-manifest record'
        $relative, $digest = $Matches.path, $Matches.hash
        Assert-PmControllerCondition ($paths.Add($relative)) "Duplicate source path: $relative"
        [void]$canonicalPaths.Add($relative)
        $source = Resolve-PmControllerSourcePath $Root $relative $cache
        Assert-PmControllerCondition ((Get-PmControllerSha256 $source) -ceq $digest) "Source changed: $relative"
    }
    foreach ($required in Get-PmControllerRequiredSources) {
        Assert-PmControllerCondition ($canonicalPaths.Contains($required)) "Required source omitted: $required"
    }
    $headers = Get-PmControllerCurrentHeaders $Root
    foreach ($header in $headers) {
        Assert-PmControllerCondition ($canonicalPaths.Contains($header)) "Current public header omitted: $header"
    }
    $codecHeaders = Get-PmControllerCurrentHeaders $Root -RelativeDirectory 'third_party/lzfse/src' `
        -TreeName 'Codec-header' -Extensions @('.h')
    foreach ($header in $codecHeaders) {
        Assert-PmControllerCondition ($canonicalPaths.Contains($header)) "Current codec header omitted: $header"
    }
    return [pscustomobject]@{ SourceFiles = $paths.Count; PublicHeaders = $headers.Count; CodecHeaders = $codecHeaders.Count }
}

function Get-PmControllerOperationBinding {
    param([object]$Operation, [string]$ExpectedType, [string]$TargetUniqueId)
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'type') $ExpectedType 'operation type'
    $id = Get-PmControllerProperty $Operation 'operation_id'
    Assert-PmControllerUuid $id 'operation_id'
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'target_unique_id') $TargetUniqueId 'target_unique_id'
    foreach ($field in @('review_fingerprint', 'layout_hash', 'queued_script_sha256')) {
        Assert-PmControllerHash (Get-PmControllerProperty $Operation $field) $field
    }
    $revision = Get-PmControllerProperty $Operation 'review_revision'
    $parsedRevision = [uint64]0
    Assert-PmControllerCondition ($revision -is [string] -and $revision -cmatch '^[1-9][0-9]*\z' -and
        [uint64]::TryParse($revision, [ref]$parsedRevision)) 'Invalid reviewed queue revision'
    $execution = Get-PmControllerProperty $Operation 'execution'
    Assert-PmControllerInteger (Get-PmControllerProperty $execution 'schema_version') 1 'execution schema'
    Assert-PmControllerUuid (Get-PmControllerProperty $execution 'batch_id') 'execution batch_id'
    Assert-PmControllerBoolean (Get-PmControllerProperty $execution 'dry_run') $false 'execution dry_run'
    Assert-PmControllerText (Get-PmControllerProperty $execution 'before_layout_hash') $Operation.layout_hash 'before layout'
    Assert-PmControllerHash (Get-PmControllerProperty $execution 'after_layout_hash') 'after_layout_hash'
    $steps = Get-PmControllerProperty $execution 'steps'
    Assert-PmControllerCondition ($steps -is [array] -and $steps.Count -eq 1) 'Execution must contain exactly one step'
    $step = $steps[0]
    Assert-PmControllerText (Get-PmControllerProperty $step 'operation_id') $id 'executed operation_id'
    Assert-PmControllerBoolean (Get-PmControllerProperty $step 'skipped') $false 'step skipped'
    return [pscustomobject]@{ Id = $id; Execution = $execution; Step = $step }
}

function Assert-PmControllerOperation {
    param([object]$Operation, [string]$ExpectedType, [string]$TargetUniqueId)
    $binding = Get-PmControllerOperationBinding $Operation $ExpectedType $TargetUniqueId
    Assert-PmControllerBoolean (Get-PmControllerProperty $binding.Execution 'success') $true 'execution success'
    $step = $binding.Step
    Assert-PmControllerBoolean (Get-PmControllerProperty $step 'success') $true 'step success'
    Assert-PmControllerText (Get-PmControllerProperty $step 'error_message') '' 'step error_message'
    $stdout = Get-PmControllerProperty $step 'stdout'
    Assert-PmControllerCondition ($stdout -is [string] -and
        $stdout.Contains('Running full clone verification')) 'Missing full-copy verification witness'
    return $binding.Id
}

function Get-PmControllerAttachment {
    param([object]$Report, [string]$Field)
    $value = Get-PmControllerProperty $Report $Field
    Assert-PmControllerUuid (Get-PmControllerProperty $value 'vhd_identifier') "$Field vhd_identifier"
    foreach ($flag in @('is_boot', 'is_system')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $value $flag) $false "$Field $flag"
    }
    $id = Get-PmControllerProperty $value 'disk_unique_id'
    Assert-PmControllerCondition ($id -is [string] -and -not [string]::IsNullOrWhiteSpace($id) -and
        $id -ceq $id.Trim()) "$Field disk identity is missing/noncanonical"
    $number = Get-PmControllerProperty $value 'disk_number'
    $integer = $number -is [int] -or $number -is [long]
    Assert-PmControllerCondition ($integer -and $number -ge 0 -and $number -le [int]::MaxValue) `
        "$Field disk number is invalid"
    Assert-PmControllerText (Get-PmControllerProperty $value 'physical_path') `
        ("\\.\PhysicalDrive" + $number) "$Field physical path"
    $size = Get-PmControllerProperty $value 'size_bytes'
    $parsedSize = [uint64]0
    Assert-PmControllerCondition ($size -is [string] -and $size -cmatch '^[1-9][0-9]*\z' -and
        [uint64]::TryParse($size, [ref]$parsedSize) -and
        $parsedSize -ge 512MB -and $parsedSize -le 1024MB) "$Field VHD size is outside fixture bounds"
    $path = Get-PmControllerProperty $value 'vhd_path'
    $literal = $path -is [string] -and $path -match '^[A-Za-z]:[\\/]' -and
        $path.Substring(2) -notmatch ':' -and $path -notmatch '[\x00-\x1f]' -and
        [IO.Path]::GetExtension($path) -in @('.vhd', '.vhdx')
    Assert-PmControllerCondition $literal "$Field VHD path is not literal/local"
    Assert-PmControllerCondition ($path.Replace('/', '\') -ceq [IO.Path]::GetFullPath($path)) `
        "$Field VHD path is not canonical"
    return $value
}

function Get-PmControllerAttachmentPair {
    param([object]$Report)
    $source = Get-PmControllerAttachment $Report source
    $target = Get-PmControllerAttachment $Report target_before
    Assert-PmControllerCondition ($source.disk_unique_id -ine $target.disk_unique_id -and
        $source.vhd_identifier -ine $target.vhd_identifier -and
        $source.disk_number -ne $target.disk_number -and $source.vhd_path -ine $target.vhd_path) `
        'Distinct source and target identities are required'
    Assert-PmControllerText $source.size_bytes $target.size_bytes 'source/target size'
    Assert-PmControllerText (Get-PmControllerProperty $Report source_vhd) $source.vhd_path 'source VHD binding'
    Assert-PmControllerText (Get-PmControllerProperty $Report target_vhd) $target.vhd_path 'target VHD binding'
    return [pscustomobject]@{ Source = $source; Target = $target }
}

function Assert-PmControllerAttachmentBindings {
    param([object]$Report)
    $bindings = Get-PmControllerAttachmentPair $Report
    $after = Get-PmControllerAttachment $Report target_after
    foreach ($field in @('vhd_identifier', 'physical_path', 'vhd_path', 'size_bytes', 'disk_number')) {
        Assert-PmControllerCondition ($after.$field -ceq $bindings.Target.$field) "Restored target binding changed: $field"
    }
    Assert-PmControllerText (Get-PmControllerProperty $Report pre_restore_marker) `
        ('target-before-restore:' + $Report.run_id) 'pre-restore marker binding'
    return $bindings
}

function Assert-PmControllerQuiescence {
    param([object]$Report)
    $quiescence = Get-PmControllerProperty $Report 'job_quiescence'
    Assert-PmControllerText (Get-PmControllerProperty $quiescence 'last_active_processes') '0' `
        'observed remaining job processes'
    $initial = Get-PmControllerProperty $quiescence 'initial_active_processes'
    $initialCount = [uint32]0
    Assert-PmControllerCondition ($initial -is [string] -and $initial -cmatch '^(0|[1-9][0-9]*)\z' -and
        [uint32]::TryParse($initial, [ref]$initialCount)) 'Invalid initial job-process observation'
    $wait = Get-PmControllerProperty $quiescence 'wait_ms'
    Assert-PmControllerCondition (($wait -is [int] -or $wait -is [long]) -and $wait -ge 0) `
        'Invalid job-quiescence wait observation'
    Assert-PmControllerCondition ($null -eq $quiescence.PSObject.Properties['termination_requested']) `
        'Job termination request is not a successful completion'
}

function Read-PmControllerBoundReport {
    param([string]$ReportPath, [string]$ExpectedBinaryPath, [string]$SourceManifestPath, [string]$ProjectRoot)
    $reportFile = Resolve-PmControllerLocalPath $ReportPath File
    $binary = Resolve-PmControllerLocalPath $ExpectedBinaryPath File
    $manifest = Resolve-PmControllerLocalPath $SourceManifestPath File
    $root = Resolve-PmControllerLocalPath $ProjectRoot Directory
    Assert-PmControllerCondition ([IO.Path]::GetExtension($binary) -ieq '.exe') 'Expected binary must be an executable path'
    Assert-PmControllerCondition ($binary -ine $manifest -and $binary -ine $reportFile -and
        $manifest -ine $reportFile) 'Report, binary and source-manifest roles must not alias'
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    [byte[]]$bytes = Read-PmControllerBoundedBytes $reportFile (4 * 1024 * 1024)
    $report = $utf8.GetString($bytes) | ConvertFrom-Json -ErrorAction Stop
    return [pscustomobject]@{
        Report = $report; Bytes = $bytes; ReportPath = $reportFile
        BinaryPath = $binary; SourceManifestPath = $manifest; ProjectRoot = $root
    }
}

function Get-PmControllerBuildReceipt {
    param([object]$Bound)
    $report = $Bound.Report
    foreach ($field in @('source_digest', 'certifier_binary_sha256')) {
        Assert-PmControllerHash (Get-PmControllerProperty $report $field) $field
    }
    $binaryHash = Get-PmControllerSha256 $Bound.BinaryPath
    Assert-PmControllerCondition ($binaryHash -ceq $report.certifier_binary_sha256) 'Expected binary hash mismatch'
    $sourceProof = Assert-PmControllerSourceManifest $Bound.SourceManifestPath $Bound.ProjectRoot $report.source_digest
    return [pscustomobject]@{
        Validated = $true; RunId = $report.run_id; BinarySha256 = $binaryHash
        SourceDigest = $report.source_digest; SourceFiles = $sourceProof.SourceFiles
        PublicHeaders = $sourceProof.PublicHeaders; ReportPath = $Bound.ReportPath
        CodecHeaders = $sourceProof.CodecHeaders
        BinaryPath = $Bound.BinaryPath; SourceManifestPath = $Bound.SourceManifestPath
        ReportSha256 = (Get-PmControllerBytesSha256 $Bound.Bytes); Report = $report
    }
}

function Assert-PartitionManagerControllerEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string]$ReportPath,
        [Parameter(Mandatory = $true)] [string]$ExpectedBinaryPath,
        [Parameter(Mandatory = $true)] [string]$SourceManifestPath,
        [Parameter(Mandatory = $true)] [string]$ProjectRoot
    )
    $bound = Read-PmControllerBoundReport $ReportPath $ExpectedBinaryPath $SourceManifestPath $ProjectRoot
    $report = $bound.Report
    Assert-PmControllerText (Get-PmControllerProperty $report 'tool') `
        'partition-manager-controller-live-certifier' 'tool'
    Assert-PmControllerText (Get-PmControllerProperty $report 'execution_engine') 'PartitionManagerController' 'execution_engine'
    Assert-PmControllerText (Get-PmControllerProperty $report 'scenario_id') 'vhd.image-restore' 'scenario_id'
    Assert-PmControllerInteger (Get-PmControllerProperty $report 'schema_version') 1 'report schema'
    Assert-PmControllerText (Get-PmControllerProperty $report 'status') 'Passed' 'status'
    Assert-PmControllerUuid (Get-PmControllerProperty $report 'run_id') 'run_id'
    Assert-PmControllerCondition ($null -eq $report.PSObject.Properties['self_test']) 'Self-test is not runtime evidence'
    Assert-PmControllerBoolean (Get-PmControllerProperty $report 'validation_only') $false 'validation_only'
    foreach ($field in @('controller_executed', 'worker_descendants_finished', 'mutations_may_have_started',
                         'source_dismounted_before_restore', 'sentinel_verified', 'target_overwritten')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $report $field) $true $field
    }
    Assert-PmControllerInteger (Get-PmControllerProperty $report 'worker_exit_code') 0 'worker_exit_code'
    Assert-PmControllerQuiescence $report
    foreach ($field in @('timed_out', 'cancelled')) {
        if ($null -ne $report.PSObject.Properties[$field]) {
            Assert-PmControllerBoolean $report.$field $false $field
        }
    }
    foreach ($field in @('source_image_sha256', 'sentinel_sha256')) {
        Assert-PmControllerHash (Get-PmControllerProperty $report $field) $field
    }
    $receipt = Get-PmControllerBuildReceipt $bound
    $bindings = Assert-PmControllerAttachmentBindings $report
    $operations = Get-PmControllerProperty $report 'operations'
    Assert-PmControllerCondition ($operations -is [array] -and $operations.Count -eq 2) `
        'Exactly CreateImage and RestoreImage controller operations are required'
    $createId = Assert-PmControllerOperation $operations[0] 'Create Image' $bindings.Source.disk_unique_id
    $restoreId = Assert-PmControllerOperation $operations[1] 'Restore Image' $bindings.Target.disk_unique_id
    Assert-PmControllerCondition ($createId -cne $restoreId) 'Create/restore operation identities must differ'
    return $receipt
}

function Resolve-PartitionManagerControllerBuildInputs {
    param([string]$ProjectRoot, [string]$ControllerCertifierPath, [string]$ControllerSourceManifestPath)
    # An explicitly selected build tree keeps the binary and source manifest paired.
    # This is not a search or report-directed selection. Missing artifacts still refuse.
    $buildRoot = 'build'
    $selectedBuildRoot = [Environment]::GetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', 'Process')
    if (-not [string]::IsNullOrEmpty($selectedBuildRoot)) {
        Assert-PmControllerCondition (-not [string]::IsNullOrWhiteSpace($selectedBuildRoot)) `
            'SAK_CERTIFIER_BUILD_ROOT must name a build directory'
        $buildRoot = $selectedBuildRoot
    }
    $releaseRoot = Join-Path $buildRoot 'Release'
    $binary = if ([string]::IsNullOrEmpty($ControllerCertifierPath)) {
        Join-Path $releaseRoot 'partition_manager_live_certifier.exe'
    } else { $ControllerCertifierPath }
    $manifest = if ([string]::IsNullOrEmpty($ControllerSourceManifestPath)) {
        Join-Path $releaseRoot 'partition_manager_live_certifier.sources.txt'
    } else { $ControllerSourceManifestPath }
    $paths = @($binary, $manifest) | ForEach-Object {
        if ([IO.Path]::IsPathRooted($_)) { [IO.Path]::GetFullPath($_) }
        else { [IO.Path]::GetFullPath((Join-Path $ProjectRoot $_)) }
    }
    return @{ ExpectedBinaryPath = $paths[0]; SourceManifestPath = $paths[1]; ProjectRoot = $ProjectRoot }
}

function Assert-PmControllerEquivalentPath {
    param([object]$Actual, [object]$Expected, [string]$Name)
    foreach ($path in @($Actual, $Expected)) {
        Assert-PmControllerCondition ($path -is [string] -and $path -match '^[A-Za-z]:[\\/]' -and
            $path.Substring(2) -notmatch ':' -and $path -notmatch '[\x00-\x1f]') "$Name is not a local literal path"
        Assert-PmControllerCondition ($path.Replace('/', '\') -ceq [IO.Path]::GetFullPath($path)) `
            "$Name is not a canonical path"
    }
    Assert-PmControllerCondition ([string]::Equals([IO.Path]::GetFullPath($Actual),
        [IO.Path]::GetFullPath($Expected), [StringComparison]::OrdinalIgnoreCase)) "$Name binding mismatch"
}

function Assert-PartitionManagerControllerScenarioEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [object]$Evidence,
        [Parameter(Mandatory = $true)] [string]$ExpectedBinaryPath,
        [Parameter(Mandatory = $true)] [string]$SourceManifestPath,
        [Parameter(Mandatory = $true)] [string]$ProjectRoot,
        [Parameter(Mandatory = $true)] [string]$ReportDirectory
    )
    Assert-PmControllerText (Get-PmControllerProperty $Evidence 'execution_engine') `
        'PartitionManagerController' 'scenario execution_engine'
    foreach ($field in @('controller_executed', 'worker_descendants_finished', 'independent_verification',
                         'sentinel_verified', 'target_overwritten')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $Evidence $field) $true "scenario $field"
    }
    $reference = Get-PmControllerProperty $Evidence 'controller_report'
    Assert-PmControllerCondition ($reference -is [string] -and -not [string]::IsNullOrWhiteSpace($reference) -and
        $reference.Length -le 4096 -and $reference -notmatch '[:\x00-\x1f]') 'Invalid portable controller report reference'
    $parts = $reference.Replace('\', '/').Split('/')
    Assert-PmControllerCondition (@($parts | Where-Object {
        $_ -in @('', '.', '..') -or $_ -match '[ .]$'
    }).Count -eq 0) 'Controller report reference must be canonical and remain inside its evidence directory'
    $directory = Resolve-PmControllerLocalPath $ReportDirectory Directory
    $arguments = @{
        ReportPath = [IO.Path]::GetFullPath((Join-Path $directory $reference))
        ExpectedBinaryPath = $ExpectedBinaryPath; SourceManifestPath = $SourceManifestPath; ProjectRoot = $ProjectRoot
    }
    $receipt = Assert-PartitionManagerControllerEvidence @arguments
    $report = $receipt.Report
    $hashes = @{
        controller_report_sha256 = $receipt.ReportSha256; source_digest = $receipt.SourceDigest
        certifier_binary_sha256 = $receipt.BinarySha256; source_image_sha256 = $report.source_image_sha256
        sentinel_sha256 = $report.sentinel_sha256
    }
    foreach ($field in $hashes.Keys) {
        $value = Get-PmControllerProperty $Evidence $field
        Assert-PmControllerCondition ($value -is [string] -and $value -match '^[0-9a-fA-F]{64}$' -and
            $value -ieq $hashes[$field]) "Scenario $field binding mismatch"
    }
    Assert-PmControllerText (Get-PmControllerProperty $Evidence 'controller_run_id') $receipt.RunId 'scenario run ID'
    Assert-PmControllerText (Get-PmControllerProperty $Evidence 'create_image_operation_id') `
        $report.operations[0].operation_id 'scenario CreateImage ID'
    Assert-PmControllerText (Get-PmControllerProperty $Evidence 'restore_image_operation_id') `
        $report.operations[1].operation_id 'scenario RestoreImage ID'
    Assert-PmControllerText (Get-PmControllerProperty $Evidence 'pre_restore_marker') `
        $report.pre_restore_marker 'scenario marker'
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $Evidence 'source_image') `
        (Get-PmControllerProperty $report 'source_image') 'scenario source image'
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $Evidence 'target_image') `
        $report.target_after.vhd_path 'scenario target image'
    # The producer independently detaches and reattaches the target after the helper exits.
    # Its fresh disk number may differ; the retained VHD path is the stable target binding.
    $number = Get-PmControllerProperty $Evidence 'target_disk_number'
    Assert-PmControllerCondition (($number -is [int] -or $number -is [long]) -and
        $number -ge 0 -and $number -le [int]::MaxValue) 'Scenario reattached disk number is invalid'
    return $receipt
}

function Get-PmImageProofPolicy {
    param([string]$Mode, [object]$ValidationOnly)
    Assert-PmControllerCondition ($ValidationOnly -is [bool]) 'ExpectedValidationOnly must be Boolean'
    $scenarios = @{
        'offline-restore' = 'vhd.image-restore-offline'
        'offline-readonly-restore' = 'vhd.image-restore-offline-readonly'
        'offline-partition-image' = 'vhd.partition-image-create-offline'
        'busy-source-refusal' = 'vhd.image-create-busy-refusal'
    }
    Assert-PmControllerCondition (@($scenarios.Keys | Where-Object { $_ -ceq $Mode }).Count -eq 1) `
        'ExpectedProofMode must select one explicit supplementary mode'
    $busy = $Mode -ceq 'busy-source-refusal'
    $restore = $Mode -cin @('offline-restore', 'offline-readonly-restore')
    return [pscustomobject]@{
        Mode = $Mode; Scenario = $scenarios[$Mode]; Admission = $ValidationOnly
        Busy = $busy; Restore = $restore; Partition = ($Mode -ceq 'offline-partition-image')
        Capture = $(if ($busy) { 'LockedVolumes' } else { 'OfflineDisk' })
        Count = $(if ($ValidationOnly) { 0 } elseif ($restore) { 2 } else { 1 })
        Status = $(if ($ValidationOnly) { 'Validated' } elseif ($busy) { 'RefusalVerified' } else { 'Passed' })
    }
}

function Get-PmImageUInt64 {
    param([object]$Value, [string]$Name)
    $number = [uint64]0
    Assert-PmControllerCondition ($Value -is [string] -and $Value -cmatch '^(0|[1-9][0-9]*)\z' -and
        [uint64]::TryParse($Value, [ref]$number)) "$Name must be canonical UInt64 text"
    return $number
}

function Assert-PmImageAbsentFields {
    param([object]$Report, [string[]]$Fields)
    foreach ($field in $Fields) {
        Assert-PmControllerCondition ($null -eq $Report.PSObject.Properties[$field]) `
            "Field is forbidden for this proof: $field"
    }
}

function Assert-PmImageProofEnvelope {
    param([object]$Report, [object]$Policy, [string]$RunId)
    Assert-PmControllerText (Get-PmControllerProperty $Report 'tool') `
        'partition-manager-controller-live-certifier' 'tool'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'execution_engine') `
        'PartitionManagerController' 'execution_engine'
    Assert-PmControllerInteger (Get-PmControllerProperty $Report 'schema_version') 2 'image proof schema'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'proof_mode') $Policy.Mode 'proof_mode'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'scenario_id') $Policy.Scenario 'scenario_id'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'status') $Policy.Status 'status'
    Assert-PmControllerUuid $RunId 'expected run_id'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'run_id') $RunId 'run_id'
    Assert-PmControllerBoolean (Get-PmControllerProperty $Report 'validation_only') $Policy.Admission 'validation_only'
    Assert-PmImageAbsentFields $Report @('self_test', 'error', 'worker_error', 'containment_error')
    foreach ($field in @('source_preparation_started', 'controller_attempted', 'mutations_may_have_started')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $Report $field) (-not $Policy.Admission) $field
    }
    Assert-PmControllerBoolean (Get-PmControllerProperty $Report 'controller_executed') `
        (-not $Policy.Admission -and -not $Policy.Busy) 'controller_executed'
    Assert-PmControllerInteger (Get-PmControllerProperty $Report 'worker_exit_code') 0 'worker_exit_code'
    Assert-PmControllerBoolean (Get-PmControllerProperty $Report 'worker_descendants_finished') $true 'worker_descendants_finished'
    Assert-PmControllerQuiescence $Report
    foreach ($field in @('timed_out', 'cancelled')) {
        if ($null -ne $Report.PSObject.Properties[$field]) {
            Assert-PmControllerBoolean $Report.$field $false $field
        }
    }
    $operations = Get-PmControllerProperty $Report 'operations'
    Assert-PmControllerCondition ($operations -is [array] -and $operations.Count -eq $Policy.Count) `
        'Image proof operation count mismatch'
}

function Assert-PmImagePinnedAttachment {
    param([object]$Actual, [object]$Expected, [string]$Name)
    foreach ($field in @('vhd_path', 'physical_path')) {
        if ($field -ceq 'vhd_path') {
            Assert-PmControllerEquivalentPath (Get-PmControllerProperty $Actual $field) `
                (Get-PmControllerProperty $Expected $field) "$Name $field"
        } else {
            Assert-PmControllerText (Get-PmControllerProperty $Actual $field) `
                (Get-PmControllerProperty $Expected $field) "$Name $field"
        }
    }
    foreach ($field in @('vhd_identifier', 'disk_unique_id', 'size_bytes', 'bus_type')) {
        $expectedValue = Get-PmControllerProperty $Expected $field
        Assert-PmControllerCondition ($expectedValue -is [string] -and
            -not [string]::IsNullOrWhiteSpace($expectedValue)) "Pinned $Name $field is missing"
        Assert-PmControllerText (Get-PmControllerProperty $Actual $field) $expectedValue "pinned $Name $field"
    }
    # Get-Disk can legitimately return no serial for a fresh VHD. Preserve that
    # exact observation; the field must still exist and be a string on both sides.
    $serial = Get-PmControllerProperty $Expected 'disk_serial_number'
    Assert-PmControllerCondition ($serial -is [string]) "Pinned $Name disk_serial_number must be a string"
    Assert-PmControllerText (Get-PmControllerProperty $Actual 'disk_serial_number') $serial "pinned $Name disk_serial_number"
    $number = Get-PmControllerProperty $Expected 'disk_number'
    Assert-PmControllerCondition (($number -is [int] -or $number -is [long]) -and
        $number -ge 0 -and $number -le [int]::MaxValue) "Pinned $Name disk number is invalid"
    Assert-PmControllerInteger (Get-PmControllerProperty $Actual 'disk_number') $number "pinned $Name disk number"
}

function Get-PmImagePinnedFixture {
    param([object]$Report, [object]$Expected)
    $bindings = Get-PmControllerAttachmentPair $Report
    foreach ($field in @('source', 'target_before')) {
        $pinned = Get-PmControllerAttachment $Expected $field
        Assert-PmImagePinnedAttachment (Get-PmControllerProperty $Report $field) $pinned $field
    }
    $directory = Get-PmControllerProperty $Expected 'guest_directory'
    Assert-PmControllerEquivalentPath $directory $directory 'expected guest directory'
    foreach ($attachment in @($bindings.Source, $bindings.Target)) {
        Assert-PmControllerEquivalentPath ([IO.Path]::GetDirectoryName($attachment.vhd_path.Replace('/', '\'))) `
            $directory 'fixture VHD directory'
    }
    return [pscustomobject]@{
        Source = $bindings.Source; Target = $bindings.Target
        ImagePath = [IO.Path]::Combine($directory, 'controller-source.img')
    }
}

function Get-PmImagePartition {
    param([object]$Expected, [uint64]$DiskSize)
    $partition = Get-PmControllerProperty $Expected 'selected_partition'
    $number = Get-PmControllerProperty $partition 'number'
    Assert-PmControllerCondition (($number -is [int] -or $number -is [long]) -and
        $number -gt 0 -and $number -le [int]::MaxValue) 'Pinned partition number is invalid'
    $guid = Get-PmControllerProperty $partition 'guid'
    $parsed = [guid]::Empty
    Assert-PmControllerCondition ($guid -is [string] -and [guid]::TryParse($guid, [ref]$parsed) -and
        $parsed -ne [guid]::Empty -and ($guid -ieq $parsed.ToString('D') -or
        $guid -ieq $parsed.ToString('B'))) 'Pinned partition GUID is invalid'
    $offset = Get-PmImageUInt64 (Get-PmControllerProperty $partition 'offset_bytes') 'pinned partition offset'
    $size = Get-PmImageUInt64 (Get-PmControllerProperty $partition 'size_bytes') 'pinned partition size'
    Assert-PmControllerCondition ($offset -gt 0 -and $offset -le $DiskSize -and
        $size -gt 0 -and $size -le $DiskSize - $offset) 'Pinned partition range exceeds source'
    $volume = Get-PmControllerProperty $partition 'volume_path'
    Assert-PmControllerCondition ($volume -is [string] -and
        $volume -imatch '^\\\\\?\\Volume\{(?<volumeId>[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\}\z') `
        'Pinned volume path must be a GUID device path without trailing separator'
    Assert-PmControllerCondition ([guid]$Matches.volumeId -ne [guid]::Empty) 'Pinned volume GUID must not be empty'
    return $partition
}

function Assert-PmImageSourceAttributes {
    param([object]$Report, [object]$Expected, [object]$Policy)
    $initial = Get-PmImageUInt64 (Get-PmControllerProperty $Expected 'before_preparation_attributes') 'pinned source attributes'
    $before = Get-PmImageUInt64 (Get-PmControllerProperty $Report 'source_attributes_before_preparation') 'attributes before preparation'
    $capture = Get-PmImageUInt64 (Get-PmControllerProperty $Report 'source_attributes_before_capture') 'attributes before capture'
    $after = Get-PmImageUInt64 (Get-PmControllerProperty $Report 'source_attributes_after_capture') 'attributes after capture'
    Assert-PmControllerCondition ($before -eq $initial) 'Initial native attributes do not match pinned fixture'
    # The seeded source was writable/online before preparation. Preserve all other bits.
    Assert-PmControllerCondition (($initial -band [uint64]3) -eq 0) 'Seeded fixture must start online and writable'
    $prepared = $initial
    if (-not $Policy.Busy) { $prepared = $prepared -bor [uint64]1 }
    if ($Policy.Mode -ceq 'offline-readonly-restore') { $prepared = $prepared -bor [uint64]2 }
    Assert-PmControllerCondition ($capture -eq $prepared) 'Prepared native attributes do not match selected mode'
    Assert-PmControllerCondition ($after -eq $capture) 'Full source native attributes changed during capture'
    Assert-PmControllerBoolean (Get-PmControllerProperty $Report 'source_attributes_preserved') $true 'source_attributes_preserved'
    $fields = @('offline_preparation_source_locked', 'offline_preparation_source_dismounted', 'offline_preparation_lock_released')
    if ($Policy.Busy) { Assert-PmImageAbsentFields $Report $fields }
    else {
        foreach ($field in $fields) { Assert-PmControllerBoolean (Get-PmControllerProperty $Report $field) $true $field }
    }
}

function Assert-PmImageOperationTarget {
    param([object]$Operation, [string]$Kind, [object]$Partition, [string]$Size)
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'target_kind') $Kind 'target_kind'
    $number = 0; $guid = ''; $offset = '0'
    if ($Kind -ceq 'Partition') { $number = $Partition.number; $guid = $Partition.guid; $offset = $Partition.offset_bytes }
    Assert-PmControllerInteger (Get-PmControllerProperty $Operation 'target_partition_number') $number 'target_partition_number'
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'target_partition_guid') $guid 'target_partition_guid'
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'target_offset_bytes') $offset 'target_offset_bytes'
    Assert-PmControllerText (Get-PmControllerProperty $Operation 'target_size_bytes') $Size 'target_size_bytes'
}

function Assert-PmImageOperationFlags {
    param([object]$Operation)
    foreach ($field in @('execution_cancelled', 'execution_timed_out')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $Operation $field) $false $field
    }
}

function Assert-PmImagePayloadKeys {
    param([object]$Payload, [string[]]$Expected, [string]$Name)
    Assert-PmControllerCondition ($Payload -is [pscustomobject]) "$Name payload must be an object"
    $names = @($Payload.PSObject.Properties.Name)
    Assert-PmControllerCondition ($names.Count -eq $Expected.Count -and
        @($names | Where-Object { $_ -cnotin $Expected }).Count -eq 0) "$Name payload keys do not match accepted request"
}

function Assert-PmImageBusyOperation {
    param([object]$Operation, [string]$SourceId)
    $binding = Get-PmControllerOperationBinding $Operation 'Create Image' $SourceId
    Assert-PmControllerBoolean (Get-PmControllerProperty $binding.Execution 'success') $false 'busy execution success'
    Assert-PmControllerBoolean (Get-PmControllerProperty $binding.Step 'success') $false 'busy step success'
    $errorText = Get-PmControllerProperty $binding.Step 'error_message'
    $stderr = Get-PmControllerProperty $binding.Step 'stderr'
    $stdout = Get-PmControllerProperty $binding.Step 'stdout'
    Assert-PmControllerCondition ($errorText -is [string] -and $stderr -is [string] -and $stdout -is [string]) `
        'Busy step diagnostics must be strings'
    $errors = $stderr + $errorText
    Assert-PmControllerCondition ($errors.Contains('Create Image cannot open the source volume for exclusive capture:') -or
        $errors.Contains('Create Image cannot lock the source volume.')) 'Busy step did not refuse owned source acquisition'
    Assert-PmControllerCondition (-not $stdout.Contains('Running full clone verification') -and
        -not $stdout.Contains('Running sample clone verification')) 'Busy step reached copy verification'
    return $binding.Id
}

function Assert-PmImageCreateOperation {
    param([object]$Report, [object]$Pinned, [object]$Partition, [object]$Policy)
    $operation = $Report.operations[0]
    Assert-PmImageOperationFlags $operation
    if ($Policy.Busy) { $id = Assert-PmImageBusyOperation $operation $Pinned.Source.disk_unique_id }
    else { $id = Assert-PmControllerOperation $operation 'Create Image' $Pinned.Source.disk_unique_id }
    $size = $Pinned.Source.size_bytes; $offset = '0'; $kind = 'Disk'
    if ($Policy.Partition) { $size = $Partition.size_bytes; $offset = $Partition.offset_bytes; $kind = 'Partition' }
    Assert-PmImageOperationTarget $operation $kind $Partition $size
    $payload = Get-PmControllerProperty $operation 'accepted_payload'
    Assert-PmImagePayloadKeys $payload @('source_path', 'target_path', 'source_offset_bytes', 'source_size_bytes',
        'source_capture_mode', 'verify_mode') 'Create Image'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_path') $Pinned.Source.physical_path 'create source_path'
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $payload 'target_path') $Pinned.ImagePath 'create target_path'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_capture_mode') $Policy.Capture 'source_capture_mode'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'verify_mode') 'Full' 'create verify_mode'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_offset_bytes') $offset 'create source_offset_bytes'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_size_bytes') $size 'create source_size_bytes'
    return [pscustomobject]@{ Id = $id; Offset = $offset; Size = $size }
}

function Assert-PmImageArtifact {
    param([object]$Report, [object]$Pinned, [object]$Range, [string]$LocalImagePath, [object]$Bound)
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $Report 'source_image') $Pinned.ImagePath 'source_image'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'source_range_offset_bytes') $Range.Offset 'source_range_offset_bytes'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'source_range_size_bytes') $Range.Size 'source_range_size_bytes'
    foreach ($field in @('source_image_sha256', 'source_range_sha256_before', 'source_range_sha256_after')) {
        Assert-PmControllerHash (Get-PmControllerProperty $Report $field) $field
    }
    Assert-PmControllerText $Report.source_range_sha256_before $Report.source_image_sha256 'source range before hash'
    Assert-PmControllerText $Report.source_range_sha256_after $Report.source_image_sha256 'source range after hash'
    Assert-PmControllerBoolean (Get-PmControllerProperty $Report 'source_range_verified') $true 'source_range_verified'
    $image = Resolve-PmControllerLocalPath $LocalImagePath File
    Assert-PmControllerCondition ($image -ine $Bound.ReportPath -and $image -ine $Bound.BinaryPath -and
        $image -ine $Bound.SourceManifestPath) 'Image artifact must not alias bound inputs'
    $stream = [IO.FileStream]::new($image, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        Assert-PmControllerCondition ($stream.Length -eq [uint64]$Range.Size) 'Retained image length does not match source range'
        $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        Assert-PmControllerText $hash $Report.source_image_sha256 'retained image hash'
    } finally { $sha.Dispose(); $stream.Dispose() }
}

function Assert-PmImageAbsenceObservation {
    param([object]$Observation, [object]$Bound, [object]$Pinned, [string]$LocalImagePath)
    Assert-PmControllerCondition ([string]::IsNullOrEmpty($LocalImagePath)) 'Absent image proof must not select a local image artifact'
    Assert-PmControllerText (Get-PmControllerProperty $Observation 'run_id') $Bound.Report.run_id 'absence run_id'
    Assert-PmControllerText (Get-PmControllerProperty $Observation 'report_sha256') `
        (Get-PmControllerBytesSha256 $Bound.Bytes) 'absence report hash'
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $Observation 'image_path') $Pinned.ImagePath 'absence image path'
    Assert-PmControllerBoolean (Get-PmControllerProperty $Observation 'absent') $true 'independent image absence'
}

function Assert-PmImageBusyProof {
    param([object]$Report, [object]$Partition)
    foreach ($field in @('owned_source_lock_refusal', 'image_output_absent', 'independent_source_lock_held', 'independent_source_lock_released')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $Report $field) $true $field
    }
    Assert-PmControllerInteger (Get-PmControllerProperty $Report 'independent_source_lock_partition') `
        $Partition.number 'independent_source_lock_partition'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'independent_source_lock_path') `
        $Partition.volume_path 'independent_source_lock_path'
}

function Assert-PmImageRestoreProof {
    param([object]$Report, [object]$Pinned, [object]$Expected, [string]$CreateId)
    [void](Assert-PmControllerAttachmentBindings $Report)
    foreach ($field in @('source_dismounted_before_restore', 'sentinel_verified', 'target_overwritten')) {
        Assert-PmControllerBoolean (Get-PmControllerProperty $Report $field) $true $field
    }
    $sentinel = Get-PmControllerProperty $Expected 'sentinel_sha256'
    Assert-PmControllerHash $sentinel 'pinned sentinel_sha256'
    Assert-PmControllerText (Get-PmControllerProperty $Report 'sentinel_sha256') $sentinel 'sentinel_sha256'
    $operation = $Report.operations[1]
    Assert-PmImageOperationFlags $operation
    $restoreId = Assert-PmControllerOperation $operation 'Restore Image' $Pinned.Target.disk_unique_id
    Assert-PmControllerCondition ($restoreId -cne $CreateId) 'Create/restore operation identities must differ'
    Assert-PmImageOperationTarget $operation 'Disk' $null $Pinned.Target.size_bytes
    $payload = Get-PmControllerProperty $operation 'accepted_payload'
    Assert-PmImagePayloadKeys $payload @('source_path', 'target_path', 'source_size_bytes', 'target_size_bytes',
        'source_sha256', 'target_wipe_confirmed', 'verify_mode', 'target_disk_unique_id',
        'target_disk_serial_number', 'target_disk_size_bytes') 'Restore Image'
    Assert-PmControllerEquivalentPath (Get-PmControllerProperty $payload 'source_path') $Pinned.ImagePath 'restore source_path'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'target_path') $Pinned.Target.physical_path 'restore target_path'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_size_bytes') $Pinned.Source.size_bytes 'restore source_size_bytes'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'target_size_bytes') $Pinned.Target.size_bytes 'restore target_size_bytes'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'source_sha256') $Report.source_image_sha256 'restore source_sha256'
    Assert-PmControllerBoolean (Get-PmControllerProperty $payload 'target_wipe_confirmed') $true 'target_wipe_confirmed'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'verify_mode') 'Full' 'restore verify_mode'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'target_disk_unique_id') $Pinned.Target.disk_unique_id 'restore target_disk_unique_id'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'target_disk_serial_number') $Pinned.Target.disk_serial_number 'restore target_disk_serial_number'
    Assert-PmControllerText (Get-PmControllerProperty $payload 'target_disk_size_bytes') $Pinned.Target.size_bytes 'restore target_disk_size_bytes'
}

function Assert-PartitionManagerImageProofEvidence {
    <# Caller expectations and absence observations come from retained independent fixture
       admission, never from the report. AdmissionOnly receipts cannot certify execution. #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string]$ReportPath,
        [Parameter(Mandatory = $true)] [string]$ExpectedBinaryPath,
        [Parameter(Mandatory = $true)] [string]$SourceManifestPath,
        [Parameter(Mandatory = $true)] [string]$ProjectRoot,
        [Parameter(Mandatory = $true)] [string]$ExpectedProofMode,
        [Parameter(Mandatory = $true)] [object]$ExpectedValidationOnly,
        [Parameter(Mandatory = $true)] [string]$ExpectedRunId,
        [Parameter(Mandatory = $true)] [object]$ExpectedFixture,
        [string]$LocalImagePath = '',
        [object]$ImageAbsenceEvidence = $null
    )
    $policy = Get-PmImageProofPolicy $ExpectedProofMode $ExpectedValidationOnly
    $bound = Read-PmControllerBoundReport $ReportPath $ExpectedBinaryPath $SourceManifestPath $ProjectRoot
    $report = $bound.Report
    Assert-PmImageProofEnvelope $report $policy $ExpectedRunId
    $receipt = Get-PmControllerBuildReceipt $bound
    $pinned = Get-PmImagePinnedFixture $report $ExpectedFixture
    $imageFields = @('source_image', 'source_image_sha256', 'source_range_offset_bytes', 'source_range_size_bytes',
        'source_range_sha256_before', 'source_range_sha256_after', 'source_range_verified')
    $lockFields = @('owned_source_lock_refusal', 'image_output_absent', 'independent_source_lock_held',
        'independent_source_lock_released', 'independent_source_lock_path', 'independent_source_lock_partition')
    $restoreFields = @('target_after', 'sentinel_sha256', 'sentinel_verified', 'target_overwritten',
        'source_dismounted_before_restore', 'pre_restore_marker')
    if ($policy.Admission -or -not $policy.Restore) { Assert-PmImageAbsentFields $report $restoreFields }
    if ($policy.Admission -or -not $policy.Busy) { Assert-PmImageAbsentFields $report $lockFields }
    if ($policy.Admission -or $policy.Busy) {
        Assert-PmImageAbsentFields $report $imageFields
        Assert-PmImageAbsenceObservation $ImageAbsenceEvidence $bound $pinned $LocalImagePath
    } else {
        Assert-PmControllerCondition ($null -eq $ImageAbsenceEvidence) 'Positive image proof must not claim image absence'
    }
    if ($policy.Admission) {
        Assert-PmImageAbsentFields $report @('source_attributes_before_preparation', 'source_attributes_before_capture',
            'source_attributes_after_capture', 'source_attributes_preserved', 'offline_preparation_source_locked',
            'offline_preparation_source_dismounted', 'offline_preparation_lock_released')
    } else {
        $diskSize = Get-PmImageUInt64 $pinned.Source.size_bytes 'source size'
        $partition = Get-PmImagePartition $ExpectedFixture $diskSize
        Assert-PmImageSourceAttributes $report $ExpectedFixture $policy
        $range = Assert-PmImageCreateOperation $report $pinned $partition $policy
        if ($policy.Restore) { Assert-PmImageRestoreProof $report $pinned $ExpectedFixture $range.Id }
        if ($policy.Busy) { Assert-PmImageBusyProof $report $partition }
        else { Assert-PmImageArtifact $report $pinned $range $LocalImagePath $bound }
    }
    $receipt | Add-Member NoteProperty ProofMode $policy.Mode
    $receipt | Add-Member NoteProperty RuntimeEvidence (-not $policy.Admission)
    $receipt | Add-Member NoteProperty ValidationKind $(if ($policy.Admission) { 'AdmissionOnly' } else { $policy.Status })
    return $receipt
}
