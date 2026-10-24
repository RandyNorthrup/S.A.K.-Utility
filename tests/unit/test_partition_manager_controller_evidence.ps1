<#
.SYNOPSIS
    Disposable, non-executing fixtures for controller certification evidence admission.
.DESCRIPTION
    The fake .exe is plain text and is never launched. No VHD, device, build, or VM action is
    performed. ValidatorPath permits independent source-mutation drills on a disposable copy;
    Case selects one exact guard so an unrelated refusal cannot be mistaken for regression proof.
.PARAMETER FixtureRoot
    Generates one explicitly synthetic fixture for consumer tests, without running this suite.
.PARAMETER FixtureProjectRoot
    Optional current source tree to hash read-only for that synthetic fixture. No source is changed.
.PARAMETER ListCases
    Emits the exact registered case names and diagnostics without creating fixture files.
#>
[CmdletBinding()]
param(
    [string]$ValidatorPath = '',
    [string]$Case = '',
    [string]$FixtureRoot = '',
    [string]$FixtureProjectRoot = '',
    [switch]$ListCases
)

$ErrorActionPreference = 'Stop'
if ($ValidatorPath -eq '') {
    $ValidatorPath = Join-Path $PSScriptRoot '../../scripts/partition_manager_controller_evidence.ps1'
}
. $ValidatorPath
$script:Utf8 = [Text.UTF8Encoding]::new($false, $true)
$script:Cases = [Collections.Generic.List[object]]::new()

function Assert-Fixture {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "FIXTURE ASSERTION: $Message" }
}

function Write-FixtureText {
    param([string]$Path, [string]$Text)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Text, $script:Utf8)
}

function Get-FixtureHash {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FixtureSources {
    # Independent contract inventory: do not obtain the expected universe from the validator.
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
        'src/core/partition_report_generator.cpp',
        'src/core/sak_lzvn_shim.c',
        'third_party/lzfse/src/lzfse_decode.c', 'third_party/lzfse/src/lzfse_decode_base.c',
        'third_party/lzfse/src/lzfse_encode.c', 'third_party/lzfse/src/lzfse_encode_base.c',
        'third_party/lzfse/src/lzfse_fse.c', 'third_party/lzfse/src/lzvn_decode_base.c',
        'third_party/lzfse/src/lzvn_encode_base.c',
        'third_party/lzfse/src/lzfse.h', 'third_party/lzfse/src/lzfse_encode_tables.h',
        'third_party/lzfse/src/lzfse_fse.h', 'third_party/lzfse/src/lzfse_internal.h',
        'third_party/lzfse/src/lzfse_tunables.h', 'third_party/lzfse/src/lzvn_decode_base.h',
        'third_party/lzfse/src/lzvn_encode_base.h',
        'include/sak/partition_manager_controller.h', 'include/sak/nested/fixture.hpp'
    )
}

function New-FixtureAttachment {
    param([string]$Name, [int]$Number, [string]$VhdId)
    return [pscustomobject]@{
        vhd_path = "C:\SAK-Cert\fixture\$Name.vhdx"; vhd_identifier = $VhdId
        physical_path = "\\.\PhysicalDrive$Number"; disk_number = $Number
        disk_unique_id = "fixture-$Name-id"; disk_serial_number = "fixture-$Name-serial"
        size_bytes = '536870912'; bus_type = 'File Backed Virtual'
        is_boot = $false; is_system = $false
    }
}

function New-FixtureOperation {
    param([string]$Type, [string]$Id, [string]$TargetId)
    return [pscustomobject]@{
        type = $Type; operation_id = $Id; review_revision = '1'; target_unique_id = $TargetId
        review_fingerprint = ('1' * 64); layout_hash = ('2' * 64); queued_script_sha256 = ('3' * 64)
        execution = [pscustomobject]@{
            schema_version = 1; batch_id = [guid]::NewGuid().ToString('D')
            success = $true; dry_run = $false; message = 'Completed'
            before_layout_hash = ('2' * 64); after_layout_hash = ('4' * 64)
            steps = @([pscustomobject]@{
                operation_id = $Id; summary = $Type; success = $true; skipped = $false
                error_message = ''; stdout = 'Running full clone verification'; stderr = ''
            })
        }
    }
}

function Set-FixtureManifest {
    param([object]$Fixture, [string[]]$Records)
    $text = ''
    if ($Records.Count -gt 0) { $text = ($Records -join "`n") + "`n" }
    Write-FixtureText $Fixture.Arguments.SourceManifestPath $text
    $Fixture.Report.source_digest = Get-FixtureHash $Fixture.Arguments.SourceManifestPath
}

function New-EvidenceFixture {
    param([string]$Root, [string]$ExistingProjectRoot = '', [string[]]$ExistingRecords = @())
    $project = if ($ExistingProjectRoot -eq '') { Join-Path $Root project } else { $ExistingProjectRoot }
    if ($ExistingRecords.Count -gt 0) {
        Assert-Fixture ($ExistingProjectRoot -ne '') 'Shared fixture records require an explicit source root'
        $records = @($ExistingRecords)
    } else {
        $sources = if ($ExistingProjectRoot -eq '') { Get-FixtureSources } else {
            Get-FixtureSources | Where-Object { -not $_.EndsWith('.h') -and -not $_.EndsWith('.hpp') }
            foreach ($directory in @('include/sak', 'third_party/lzfse/src')) {
                Get-ChildItem -LiteralPath (Join-Path $project $directory) -Recurse -File |
                    Where-Object { $_.Extension -in @('.h', '.hpp') } |
                    ForEach-Object { $_.FullName.Substring($project.Length + 1).Replace('\', '/') }
            }
        }
        $records = @(foreach ($relative in $sources) {
            $file = Join-Path $project $relative
            if ($ExistingProjectRoot -eq '') { Write-FixtureText $file "fixture source $relative`n" }
            $relative + ':' + (Get-FixtureHash $file)
        })
    }
    $binary = Join-Path $Root 'build\certifier.exe'
    Write-FixtureText $binary "ordinary fixture text, never executed`n"
    $source = New-FixtureAttachment source 7 '51eb29f9-2b08-473f-9d0d-23a538c4f682'
    $target = New-FixtureAttachment target 8 '15df54a8-c95d-432b-91dc-e346f7469450'
    $runId = '4e72a8b6-eb28-45d9-a594-6b085c712060'
    $fixture = [pscustomobject]@{
        Root = $Root; Records = $records; SkipReportWrite = $false; ImageProof = $false
        Arguments = @{
            ReportPath = (Join-Path $Root 'report.json'); ExpectedBinaryPath = $binary
            SourceManifestPath = (Join-Path $Root 'build\certifier.inputs.txt'); ProjectRoot = $project
        }
        Report = [pscustomobject]@{
            tool = 'partition-manager-controller-live-certifier'; schema_version = 1
            execution_engine = 'PartitionManagerController'; scenario_id = 'vhd.image-restore'
            run_id = $runId; status = 'Passed'; validation_only = $false
            controller_executed = $true; worker_descendants_finished = $true; worker_exit_code = 0
            job_quiescence = [pscustomobject]@{
                initial_active_processes = '1'; last_active_processes = '0'; wait_ms = 25
            }
            mutations_may_have_started = $true; source_dismounted_before_restore = $true
            sentinel_verified = $true; target_overwritten = $true
            source_digest = ''; certifier_binary_sha256 = (Get-FixtureHash $binary)
            source_image_sha256 = ('5' * 64); sentinel_sha256 = ('6' * 64)
            source_image = 'C:/SAK-Cert/fixture/controller-source.img'
            source = $source; target_before = $target
            target_after = ($target | ConvertTo-Json | ConvertFrom-Json)
            source_vhd = $source.vhd_path; target_vhd = $target.vhd_path
            pre_restore_marker = "target-before-restore:$runId"
            operations = @(
                (New-FixtureOperation 'Create Image' 'f82d76c4-97cf-441d-bfce-c9033bdc9e44' $source.disk_unique_id),
                (New-FixtureOperation 'Restore Image' '5b5409ea-de3e-4b14-bca3-79d556e9157c' $target.disk_unique_id)
            )
        }
    }
    Set-FixtureManifest $fixture $records
    return $fixture
}

# Consumer integration reuses the same explicitly synthetic fixture, without running this suite.
# A supplied source root is read-only; only FixtureRoot receives the fake binary/report/sidecar.
if ($FixtureRoot -ne '') {
    $fixture = New-EvidenceFixture -Root ([IO.Path]::GetFullPath($FixtureRoot)) -ExistingProjectRoot $FixtureProjectRoot
    Write-FixtureText $fixture.Arguments.ReportPath ($fixture.Report | ConvertTo-Json -Depth 20)
    return $fixture
}
if ($FixtureProjectRoot -ne '') { throw 'FixtureProjectRoot requires FixtureRoot' }

function Add-EvidenceCase {
    param([string]$Name, [scriptblock]$Change, [string]$Diagnostic = '', [object]$Data = $null,
        [switch]$OwnSource)
    $script:Cases.Add([pscustomobject]@{
        Name = $Name; Change = $Change; Diagnostic = $Diagnostic; Data = $Data; OwnSource = [bool]$OwnSource
    })
}

function Add-ManifestRecordCase {
    param([string]$Name, [string]$Record, [string]$Diagnostic)
    Add-EvidenceCase $Name { param($f, $data) Set-FixtureManifest $f @($f.Records + $data) } $Diagnostic $Record
}

function Initialize-ImageEvidenceFixture {
    param([object]$Fixture, [string]$Mode, [bool]$Admission)
    $Fixture.ImageProof = $true
    $report = $Fixture.Report
    $report.schema_version = 2
    $scenarios = @{
        'offline-restore' = 'vhd.image-restore-offline'
        'offline-readonly-restore' = 'vhd.image-restore-offline-readonly'
        'offline-partition-image' = 'vhd.partition-image-create-offline'
        'busy-source-refusal' = 'vhd.image-create-busy-refusal'
    }
    $busy = $Mode -ceq 'busy-source-refusal'
    $restore = $Mode -cin @('offline-restore', 'offline-readonly-restore')
    $partition = $Mode -ceq 'offline-partition-image'
    $report.scenario_id = $scenarios[$Mode]
    $report | Add-Member NoteProperty proof_mode $Mode
    $report | Add-Member NoteProperty source_preparation_started (-not $Admission)
    $report | Add-Member NoteProperty controller_attempted (-not $Admission)
    $report.validation_only = $Admission
    $report.controller_executed = -not $Admission -and -not $busy
    $report.mutations_may_have_started = -not $Admission
    $report.status = if ($Admission) { 'Validated' } elseif ($busy) { 'RefusalVerified' } else { 'Passed' }
    if (-not $restore -or $Admission) {
        foreach ($field in @('target_after', 'sentinel_sha256', 'sentinel_verified', 'target_overwritten',
            'source_dismounted_before_restore', 'pre_restore_marker')) { $report.PSObject.Properties.Remove($field) }
        $report.operations = @($report.operations[0])
    }
    $expected = [pscustomobject]@{
        source = ($report.source | ConvertTo-Json | ConvertFrom-Json)
        target_before = ($report.target_before | ConvertTo-Json | ConvertFrom-Json)
        guest_directory = 'C:\SAK-Cert\fixture'; before_preparation_attributes = '0'
        sentinel_sha256 = ('6' * 64)
        selected_partition = [pscustomobject]@{
            number = 2; guid = '{11111111-1111-1111-1111-111111111111}'
            offset_bytes = '2048'; size_bytes = '1024'
            volume_path = '\\?\Volume{22222222-2222-2222-2222-222222222222}'
        }
    }
    $Fixture.Arguments.ExpectedProofMode = $Mode
    $Fixture.Arguments.ExpectedValidationOnly = $Admission
    $Fixture.Arguments.ExpectedRunId = $report.run_id
    $Fixture.Arguments.ExpectedFixture = $expected
    $Fixture.Arguments.ReportPath = Join-Path $Fixture.Root 'controller-image-proof-report.json'
    $offset = if ($partition) { '2048' } else { '0' }
    $size = if ($partition) { '1024' } else { $report.source.size_bytes }
    $image = 'C:\SAK-Cert\fixture\controller-source.img'
    foreach ($operation in $report.operations) {
        $create = $operation.type -ceq 'Create Image'
        $operation | Add-Member NoteProperty execution_cancelled $false
        $operation | Add-Member NoteProperty execution_timed_out $false
        $operation | Add-Member NoteProperty target_kind $(if ($create -and $partition) { 'Partition' } else { 'Disk' })
        $operation | Add-Member NoteProperty target_partition_number $(if ($create -and $partition) { 2 } else { 0 })
        $operation | Add-Member NoteProperty target_partition_guid $(if ($create -and $partition) { $expected.selected_partition.guid } else { '' })
        $operation | Add-Member NoteProperty target_offset_bytes $(if ($create) { $offset } else { '0' })
        $operation | Add-Member NoteProperty target_size_bytes $(if ($create) { $size } else { $report.target_before.size_bytes })
        if ($create) {
            $payload = [pscustomobject]@{
                source_path = $report.source.physical_path; target_path = $image
                source_offset_bytes = $offset; source_size_bytes = $size
                source_capture_mode = $(if ($busy) { 'LockedVolumes' } else { 'OfflineDisk' }); verify_mode = 'Full'
            }
        } else {
            $payload = [pscustomobject]@{
                source_path = $image; target_path = $report.target_before.physical_path
                source_size_bytes = $report.source.size_bytes; target_size_bytes = $report.target_before.size_bytes
                source_sha256 = ''; target_wipe_confirmed = $true; verify_mode = 'Full'
                target_disk_unique_id = $report.target_before.disk_unique_id
                target_disk_serial_number = $report.target_before.disk_serial_number
                target_disk_size_bytes = $report.target_before.size_bytes
            }
        }
        $operation | Add-Member NoteProperty accepted_payload $payload
    }
    if ($Admission -or $busy) {
        $report.PSObject.Properties.Remove('source_image')
        $report.PSObject.Properties.Remove('source_image_sha256')
        $Fixture.Arguments.ImageAbsenceEvidence = [pscustomobject]@{
            run_id = $report.run_id; report_sha256 = '__BIND_REPORT__'; image_path = $image; absent = $true
        }
    } else {
        # Whole-disk report negatives do not mutate image bytes. Reuse this run's one
        # generated immutable ordinary artifact; the validator still hashes real bytes.
        $localImage = if ($partition) { Join-Path $Fixture.Root 'retained-source.img' } else { $script:WholeImagePath }
        if ($partition -or -not (Test-Path -LiteralPath $localImage)) {
            $stream = [IO.FileStream]::new($localImage, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            try { $stream.SetLength([long]$size) } finally { $stream.Dispose() }
        }
        $Fixture.Arguments.LocalImagePath = $localImage
        $report.source_image = $image
        if ($partition) { $report.source_image_sha256 = Get-FixtureHash $localImage }
        else {
            if ($script:WholeImageHash -eq '') { $script:WholeImageHash = Get-FixtureHash $localImage }
            $report.source_image_sha256 = $script:WholeImageHash
        }
        $report | Add-Member NoteProperty source_range_offset_bytes $offset
        $report | Add-Member NoteProperty source_range_size_bytes $size
        $report | Add-Member NoteProperty source_range_sha256_before $report.source_image_sha256
        $report | Add-Member NoteProperty source_range_sha256_after $report.source_image_sha256
        $report | Add-Member NoteProperty source_range_verified $true
        if ($restore) { $report.operations[1].accepted_payload.source_sha256 = $report.source_image_sha256 }
    }
    if ($Admission) { $report.operations = @(); return }
    $attributes = if ($busy) { '0' } elseif ($Mode -ceq 'offline-readonly-restore') { '3' } else { '1' }
    $report | Add-Member NoteProperty source_attributes_before_preparation '0'
    $report | Add-Member NoteProperty source_attributes_before_capture $attributes
    $report | Add-Member NoteProperty source_attributes_after_capture $attributes
    $report | Add-Member NoteProperty source_attributes_preserved $true
    if ($busy) {
        $execution = $report.operations[0].execution
        $execution.success = $false
        $execution.steps[0].success = $false
        $execution.steps[0].stdout = ''
        $execution.steps[0].stderr = 'Create Image cannot lock the source volume. Windows error 21'
        foreach ($field in @('owned_source_lock_refusal', 'image_output_absent', 'independent_source_lock_held', 'independent_source_lock_released')) {
            $report | Add-Member NoteProperty $field $true
        }
        $report | Add-Member NoteProperty independent_source_lock_partition 2
        $report | Add-Member NoteProperty independent_source_lock_path $expected.selected_partition.volume_path
    } else {
        foreach ($field in @('offline_preparation_source_locked', 'offline_preparation_source_dismounted', 'offline_preparation_lock_released')) {
            $report | Add-Member NoteProperty $field $true
        }
    }
}

function Add-ImageEvidenceCase {
    param([string]$Name, [scriptblock]$Change, [string]$Diagnostic = '',
        [string]$Mode = 'offline-partition-image', [bool]$Admission = $false, [object]$Data = $null,
        [switch]$OwnSource)
    Add-EvidenceCase ('image-' + $Name) { param($f, $definition)
        Initialize-ImageEvidenceFixture $f $definition.Mode $definition.Admission
        & $definition.Change $f $definition.Data
    } $Diagnostic ([pscustomobject]@{ Mode = $Mode; Admission = $Admission; Change = $Change; Data = $Data }) -OwnSource:$OwnSource
}

function Set-ImageFixtureValue {
    param([object]$Fixture, [string]$Path, [object]$Value, [bool]$Remove = $false)
    $parts = $Path.Split('.')
    $current = $Fixture
    for ($index = 0; $index -lt $parts.Count - 1; $index++) {
        $part = $parts[$index]
        if ($current -is [array]) { $current = $current[[int]$part] }
        else { $current = $current.$part }
    }
    $leaf = $parts[-1]
    if ($Remove) { $current.PSObject.Properties.Remove($leaf) }
    elseif ($current -is [hashtable]) { $current[$leaf] = $Value }
    else { $current | Add-Member NoteProperty $leaf $Value -Force }
}

function Add-ImageFieldCase {
    param([string]$Name, [string]$Path, [object]$Value, [string]$Diagnostic,
        [string]$Mode = 'offline-partition-image', [bool]$Admission = $false, [bool]$Remove = $false)
    Add-ImageEvidenceCase $Name { param($f, $data)
        Set-ImageFixtureValue $f $data.Path $data.Value $data.Remove
    } $Diagnostic $Mode $Admission ([pscustomobject]@{ Path = $Path; Value = $Value; Remove = $Remove })
}

Add-EvidenceCase current-positive { param($f) } ''
Add-EvidenceCase explicit-build-root-selects-paired-inputs { param($f)
    $previous = [Environment]::GetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT',
            'build\wave42-release-vs2022', 'Process')
        $inputs = Resolve-PartitionManagerControllerBuildInputs `
            -ProjectRoot $f.Arguments.ProjectRoot -ControllerCertifierPath '' `
            -ControllerSourceManifestPath ''
        $release = Join-Path $f.Arguments.ProjectRoot 'build\wave42-release-vs2022\Release'
        Assert-Fixture ($inputs.ExpectedBinaryPath -ceq
            (Join-Path $release 'partition_manager_live_certifier.exe')) `
            'Explicit build root did not select the certifier binary'
        Assert-Fixture ($inputs.SourceManifestPath -ceq
            (Join-Path $release 'partition_manager_live_certifier.sources.txt')) `
            'Explicit build root did not select the source manifest'
    } finally {
        if ([string]::IsNullOrEmpty($previous)) {
            Remove-Item Env:\SAK_CERTIFIER_BUILD_ROOT -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', $previous, 'Process')
        }
    }
} ''
Add-EvidenceCase whitespace-build-root-refused { param($f)
    $previous = [Environment]::GetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', ' ', 'Process')
        $message = ''
        try {
            Resolve-PartitionManagerControllerBuildInputs -ProjectRoot $f.Arguments.ProjectRoot `
                -ControllerCertifierPath '' -ControllerSourceManifestPath '' | Out-Null
        } catch { $message = $_.Exception.Message }
        Assert-Fixture ($message.Contains('SAK_CERTIFIER_BUILD_ROOT must name a build directory')) `
            'Whitespace build root was accepted or failed for an unrelated reason'
    } finally {
        if ([string]::IsNullOrEmpty($previous)) {
            Remove-Item Env:\SAK_CERTIFIER_BUILD_ROOT -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable('SAK_CERTIFIER_BUILD_ROOT', $previous, 'Process')
        }
    }
} ''
Add-EvidenceCase explicit-whitespace-binary-not-default { param($f)
    $inputs = Resolve-PartitionManagerControllerBuildInputs -ProjectRoot $f.Arguments.ProjectRoot `
        -ControllerCertifierPath ' ' -ControllerSourceManifestPath $f.Arguments.SourceManifestPath
    $default = Join-Path $f.Arguments.ProjectRoot 'build\Release\partition_manager_live_certifier.exe'
    Assert-Fixture ($inputs.ExpectedBinaryPath -cne $default) 'Explicit whitespace binary selected default authority'
} ''
Add-EvidenceCase explicit-whitespace-manifest-not-default { param($f)
    $inputs = Resolve-PartitionManagerControllerBuildInputs -ProjectRoot $f.Arguments.ProjectRoot `
        -ControllerCertifierPath $f.Arguments.ExpectedBinaryPath -ControllerSourceManifestPath ' '
    $default = Join-Path $f.Arguments.ProjectRoot 'build\Release\partition_manager_live_certifier.sources.txt'
    Assert-Fixture ($inputs.SourceManifestPath -cne $default) 'Explicit whitespace manifest selected default authority'
} ''
Add-EvidenceCase restored-disk-metadata-positive { param($f)
    $f.Report.target_after.disk_unique_id = 'restored-source-disk-metadata'
} ''
Add-EvidenceCase legacy-copy-only { param($f)
    $f.Report = [pscustomobject]@{ scenario_id = 'vhd.image-restore'; status = 'Passed'; command = 'Copy-Item' }
} 'Missing required field: tool'
Add-EvidenceCase validation-only { param($f) $f.Report.validation_only = $true } 'validation_only must'
Add-EvidenceCase self-test { param($f)
    $f.Report | Add-Member NoteProperty self_test $true
} 'Self-test is not runtime evidence'
Add-EvidenceCase failed-report { param($f) $f.Report.status = 'Failed' } 'status must'
Add-EvidenceCase wrong-engine { param($f) $f.Report.execution_engine = 'Copy-Item' } 'execution_engine must'
Add-EvidenceCase wrong-scenario { param($f) $f.Report.scenario_id = 'vhd.clone' } 'scenario_id must'
# Supplementary proof is not default restore certification. Scenario-only rows retain the
# otherwise-valid schema-1/two-successful-operation report to isolate this exact identity
# guard. Full-shape rows also carry the helper's schema-2 identity and mode-specific outcome;
# their different filename and descriptive proof_mode are not treated as authority.
$supplementaryModes = @(
    @{ mode='offline-restore'; scenario='vhd.image-restore-offline' },
    @{ mode='offline-readonly-restore'; scenario='vhd.image-restore-offline-readonly' },
    @{ mode='offline-partition-image'; scenario='vhd.partition-image-create-offline' },
    @{ mode='busy-source-refusal'; scenario='vhd.image-create-busy-refusal' }
)
foreach ($mode in $supplementaryModes) {
    Add-EvidenceCase ('supplementary-scenario-' + $mode.mode) { param($f,$data)
        $f.Report.scenario_id = $data.scenario
        $f.Report | Add-Member NoteProperty proof_mode $data.mode
        $f.Arguments.ReportPath = Join-Path $f.Root 'controller-image-proof-report.json'
    } 'scenario_id must' $mode
    Add-EvidenceCase ('supplementary-report-' + $mode.mode) { param($f,$data)
        $f.Report.schema_version = 2
        $f.Report.scenario_id = $data.scenario
        $f.Report | Add-Member NoteProperty proof_mode $data.mode
        $f.Arguments.ReportPath = Join-Path $f.Root 'controller-image-proof-report.json'
        if ($data.mode -in @('offline-partition-image','busy-source-refusal')) {
            $f.Report.operations = @($f.Report.operations[0])
            $f.Report.source_dismounted_before_restore = $false
            $f.Report.target_overwritten = $false
        }
        if ($data.mode -ceq 'busy-source-refusal') {
            $f.Report.status = 'RefusalVerified'
            $f.Report.controller_executed = $false
            $f.Report | Add-Member NoteProperty controller_attempted $true
            $f.Report.operations[0].execution.success = $false
            $f.Report.operations[0].execution.steps[0].success = $false
            $f.Report.operations[0].execution.steps[0].error_message = 'Owned source volume acquisition refused'
            $f.Report.operations[0].execution.steps[0].stdout = ''
        }
    } 'scenario_id must' $mode
}
Add-EvidenceCase refusal-verified-status { param($f)
    # Keep every restore identity/operation valid so only Passed versus RefusalVerified
    # decides this row. An expected application refusal is not a successful restore.
    $f.Report.status = 'RefusalVerified'
    $f.Report | Add-Member NoteProperty proof_mode 'busy-source-refusal'
} 'status must'
Add-EvidenceCase wrong-schema { param($f) $f.Report.schema_version = 2 } 'report schema has invalid'
Add-EvidenceCase string-schema { param($f) $f.Report.schema_version = '1' } 'report schema has invalid'
Add-EvidenceCase invalid-run-id { param($f) $f.Report.run_id = 'not-a-run' } 'run_id must'
foreach ($field in @('controller_executed', 'worker_descendants_finished', 'mutations_may_have_started',
                    'source_dismounted_before_restore', 'sentinel_verified', 'target_overwritten')) {
    Add-EvidenceCase "false-$field" { param($f, $data) $f.Report.$data = $false } "$field must" $field
}
Add-EvidenceCase missing-descendant-proof { param($f)
    $f.Report.PSObject.Properties.Remove('worker_descendants_finished')
} 'Missing required field: worker_descendants_finished'
Add-EvidenceCase string-boolean { param($f) $f.Report.controller_executed = 'true' } 'controller_executed must'
Add-EvidenceCase worker-failed { param($f) $f.Report.worker_exit_code = 1 } 'worker_exit_code has invalid'
Add-EvidenceCase missing-job-observation { param($f)
    $f.Report.PSObject.Properties.Remove('job_quiescence')
} 'Missing required field: job_quiescence'
Add-EvidenceCase job-still-active { param($f)
    $f.Report.job_quiescence.last_active_processes = '1'
} 'observed remaining job processes must'
Add-EvidenceCase invalid-job-count { param($f)
    $f.Report.job_quiescence.initial_active_processes = 'unknown'
} 'Invalid initial job-process observation'
Add-EvidenceCase invalid-job-wait { param($f)
    $f.Report.job_quiescence.wait_ms = '0'
} 'Invalid job-quiescence wait observation'
Add-EvidenceCase job-termination-request { param($f)
    $f.Report.job_quiescence | Add-Member NoteProperty termination_requested $true
} 'Job termination request is not a successful completion'
foreach ($field in @('timed_out', 'cancelled')) {
    Add-EvidenceCase $field { param($f, $data) $f.Report | Add-Member NoteProperty $data $true } "$field must" $field
}
Add-EvidenceCase selected-binary-changed { param($f)
    Write-FixtureText $f.Arguments.ExpectedBinaryPath 'different binary'
} 'Expected binary hash mismatch'
Add-EvidenceCase report-binary-claim-changed { param($f)
    $f.Report.certifier_binary_sha256 = ('0' * 64)
} 'Expected binary hash mismatch'
Add-EvidenceCase independently-selected-binary { param($f)
    $other = Join-Path $f.Root 'unrelated.exe'
    Write-FixtureText $other 'unrelated binary'
    $f.Arguments.ExpectedBinaryPath = $other
} 'Expected binary hash mismatch'
Add-EvidenceCase stale-source-digest { param($f) $f.Report.source_digest = ('0' * 64) } 'Source digest does not match'
Add-EvidenceCase raw-crlf-manifest { param($f)
    Write-FixtureText $f.Arguments.SourceManifestPath (($f.Records -join "`r`n") + "`r`n")
} 'Source digest does not match'
Add-EvidenceCase bound-crlf-manifest { param($f)
    Write-FixtureText $f.Arguments.SourceManifestPath (($f.Records -join "`r`n") + "`r`n")
    $f.Report.source_digest = Get-FixtureHash $f.Arguments.SourceManifestPath
} 'Source manifest must be canonical'
Add-EvidenceCase empty-manifest { param($f) Set-FixtureManifest $f @() } 'Source manifest must be canonical'
Add-EvidenceCase no-final-lf { param($f)
    Write-FixtureText $f.Arguments.SourceManifestPath ($f.Records -join "`n")
    $f.Report.source_digest = Get-FixtureHash $f.Arguments.SourceManifestPath
} 'Source manifest must be canonical'
Add-EvidenceCase bom-manifest { param($f)
    Write-FixtureText $f.Arguments.SourceManifestPath ([char]0xfeff + ($f.Records -join "`n") + "`n")
    $f.Report.source_digest = Get-FixtureHash $f.Arguments.SourceManifestPath
} 'Source manifest must be canonical'
Add-EvidenceCase changed-source { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'src/core/partition_manager_controller.cpp') 'changed source'
} 'Source changed: src/core/partition_manager_controller.cpp' -OwnSource
Add-EvidenceCase changed-header { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'include/sak/nested/fixture.hpp') 'changed header'
} 'Source changed: include/sak/nested/fixture.hpp' -OwnSource
Add-EvidenceCase missing-header { param($f)
    Set-FixtureManifest $f @($f.Records | Where-Object { -not $_.StartsWith('include/sak/nested/fixture.hpp:') })
} 'Current public header omitted: include/sak/nested/fixture.hpp'
Add-EvidenceCase newly-added-header { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'include/sak/new.h') 'new public header'
} 'Current public header omitted: include/sak/new.h' -OwnSource
foreach ($required in @(Get-FixtureSources | Where-Object { -not $_.EndsWith('.h') -and -not $_.EndsWith('.hpp') })) {
    $name = 'required-' + $required.Replace('/', '_')
    Add-EvidenceCase $name { param($f, $data)
        Set-FixtureManifest $f @($f.Records | Where-Object { -not $_.StartsWith($data + ':') })
    } "Required source omitted: $required" $required
}
foreach ($header in @(Get-FixtureSources | Where-Object { $_.StartsWith('third_party/') -and $_.EndsWith('.h') })) {
    Add-EvidenceCase ('codec-header-' + [IO.Path]::GetFileName($header)) { param($f, $data)
        Set-FixtureManifest $f @($f.Records | Where-Object { -not $_.StartsWith($data + ':') })
    } "Current codec header omitted: $header" $header
}
Add-EvidenceCase new-codec-header { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'third_party/lzfse/src/nested/new.h') 'new linked codec header'
} 'Current codec header omitted: third_party/lzfse/src/nested/new.h' -OwnSource
Add-EvidenceCase changed-codec-source { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'third_party/lzfse/src/lzfse_decode.c') 'changed codec'
} 'Source changed: third_party/lzfse/src/lzfse_decode.c' -OwnSource
Add-EvidenceCase changed-codec-header { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'third_party/lzfse/src/lzfse.h') 'changed codec header'
} 'Source changed: third_party/lzfse/src/lzfse.h' -OwnSource
Add-EvidenceCase duplicate-path { param($f)
    Set-FixtureManifest $f @($f.Records + $f.Records[0])
} 'Duplicate source path'
Add-EvidenceCase case-duplicate-path { param($f)
    Set-FixtureManifest $f @($f.Records + $f.Records[0].ToUpperInvariant().Replace(
        $f.Records[0].Split(':')[1].ToUpperInvariant(), $f.Records[0].Split(':')[1]))
} 'Duplicate source path'
Add-EvidenceCase noncanonical-source-case { param($f)
    $records = @($f.Records)
    $records[0] = $records[0].Replace('CMakeLists.txt', 'cmakelists.txt')
    Set-FixtureManifest $f $records
} 'Missing/noncanonical source path'
Add-ManifestRecordCase traversal ('../outside.cpp:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase rooted-path ('/outside.cpp:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase backslash-path ('src\outside.cpp:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase ads-path ('src/outside.cpp:stream:' + ('0' * 64)) 'Malformed source-manifest record'
Add-ManifestRecordCase drive-path ('C:/outside.cpp:' + ('0' * 64)) 'Malformed source-manifest record'
Add-ManifestRecordCase empty-component ('src//outside.cpp:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase dot-component ('src/./outside.cpp:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase trailing-dot ('src/outside.cpp.:' + ('0' * 64)) 'Unsafe source-manifest path'
Add-ManifestRecordCase uppercase-hash ('src/outside.cpp:' + ('A' * 64)) 'Malformed source-manifest record'
Add-EvidenceCase wrong-source-target-binding { param($f)
    $f.Report.operations[0].target_unique_id = $f.Report.target_before.disk_unique_id
} 'target_unique_id must'
Add-EvidenceCase boot-target { param($f) $f.Report.target_before.is_boot = $true } 'target_before is_boot must'
Add-EvidenceCase system-source { param($f) $f.Report.source.is_system = $true } 'source is_system must'
Add-EvidenceCase duplicate-attachment { param($f)
    $f.Report.target_before.vhd_identifier = $f.Report.source.vhd_identifier
    $f.Report.target_after.vhd_identifier = $f.Report.source.vhd_identifier
} 'Distinct source and target identities'
Add-EvidenceCase target-attachment-changed { param($f)
    $f.Report.target_after.vhd_identifier = [guid]::NewGuid().ToString('D')
} 'Restored target binding changed: vhd_identifier'
Add-EvidenceCase wrong-physical-path { param($f)
    $f.Report.target_before.physical_path = '\\.\PhysicalDrive999'
} 'target_before physical path must'
Add-EvidenceCase oversized-disk { param($f) $f.Report.source.size_bytes = '2147483648' } 'outside fixture bounds'
Add-EvidenceCase raw-vhd-path { param($f) $f.Report.source.vhd_path = '\\.\PhysicalDrive7' } 'VHD path is not literal/local'
Add-EvidenceCase stale-marker { param($f) $f.Report.pre_restore_marker = 'target-before-restore:old' } 'pre-restore marker binding'
Add-EvidenceCase no-operations { param($f) $f.Report.operations = @() } 'Exactly CreateImage and RestoreImage'
Add-EvidenceCase one-operation { param($f) $f.Report.operations = @($f.Report.operations[0]) } 'Exactly CreateImage and RestoreImage'
Add-EvidenceCase scalar-operations { param($f) $f.Report.operations = $f.Report.operations[0] } 'Exactly CreateImage and RestoreImage'
Add-EvidenceCase reversed-operations { param($f)
    $f.Report.operations = @($f.Report.operations[1], $f.Report.operations[0])
} 'operation type must'
Add-EvidenceCase duplicate-operation-identity { param($f)
    $id = $f.Report.operations[0].operation_id
    $f.Report.operations[1].operation_id = $id
    $f.Report.operations[1].execution.steps[0].operation_id = $id
} 'Create/restore operation identities must differ'
Add-EvidenceCase invalid-review-revision { param($f)
    $f.Report.operations[0].review_revision = '0'
} 'Invalid reviewed queue revision'
Add-EvidenceCase invalid-review-fingerprint { param($f)
    $f.Report.operations[0].review_fingerprint = ''
} 'review_fingerprint must'
Add-EvidenceCase newline-review-fingerprint { param($f)
    $f.Report.operations[0].review_fingerprint += "`n"
} 'review_fingerprint must'
Add-EvidenceCase newline-review-revision { param($f)
    $f.Report.operations[0].review_revision += "`n"
} 'Invalid reviewed queue revision'
Add-EvidenceCase newline-disk-size { param($f)
    foreach ($field in @('source', 'target_before', 'target_after')) { $f.Report.$field.size_bytes += "`n" }
} 'Controller evidence refused: source VHD size is outside fixture bounds'
Add-EvidenceCase newline-job-count { param($f)
    $f.Report.job_quiescence.initial_active_processes += "`n"
} 'Invalid initial job-process observation'
foreach ($index in @(0, 1)) {
    Add-EvidenceCase "execution-failed-$index" { param($f, $data)
        $f.Report.operations[$data].execution.success = $false
    } 'execution success must' $index
    Add-EvidenceCase "dry-run-$index" { param($f, $data)
        $f.Report.operations[$data].execution.dry_run = $true
    } 'execution dry_run must' $index
    Add-EvidenceCase "layout-mismatch-$index" { param($f, $data)
        $f.Report.operations[$data].execution.before_layout_hash = ('9' * 64)
    } 'before layout must' $index
    Add-EvidenceCase "missing-steps-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps = @()
    } 'Execution must contain exactly one step' $index
    Add-EvidenceCase "extra-step-$index" { param($f, $data)
        $step = $f.Report.operations[$data].execution.steps[0]
        $f.Report.operations[$data].execution.steps = @($step, $step)
    } 'Execution must contain exactly one step' $index
    Add-EvidenceCase "step-failed-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps[0].success = $false
    } 'step success must' $index
    Add-EvidenceCase "step-skipped-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps[0].skipped = $true
    } 'step skipped must' $index
    Add-EvidenceCase "step-identity-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps[0].operation_id = [guid]::NewGuid().ToString('D')
    } 'executed operation_id must' $index
    Add-EvidenceCase "step-error-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps[0].error_message = 'failed verification'
    } 'step error_message must' $index
    Add-EvidenceCase "missing-full-verification-$index" { param($f, $data)
        $f.Report.operations[$data].execution.steps[0].stdout = 'Copied file'
    } 'Missing full-copy verification witness' $index
}
Add-EvidenceCase relative-binary-path { param($f)
    $f.Arguments.ExpectedBinaryPath = 'certifier.exe'
} 'An explicit local drive-qualified path is required'
Add-EvidenceCase aliased-manifest { param($f)
    $f.Arguments.SourceManifestPath = $f.Arguments.ExpectedBinaryPath
} 'roles must not alias'
Add-EvidenceCase oversized-report { param($f)
    Write-FixtureText $f.Arguments.ReportPath (' ' * (4MB + 1))
    $f.SkipReportWrite = $true
} 'Input exceeds byte limit'
Add-EvidenceCase oversized-manifest { param($f)
    Write-FixtureText $f.Arguments.SourceManifestPath (' ' * (4MB + 1))
} 'Input exceeds byte limit'
Add-EvidenceCase reparse-header-directory { param($f)
    $outside = Join-Path $f.Root 'outside'
    [void][IO.Directory]::CreateDirectory($outside)
    $link = Join-Path $f.Arguments.ProjectRoot 'include/sak/linked'
    [void](New-Item -ItemType Junction -Path $link -Target $outside)
} 'Public-header tree contains a reparse entry' -OwnSource

foreach ($mode in @('offline-restore', 'offline-readonly-restore', 'offline-partition-image', 'busy-source-refusal')) {
    Add-ImageEvidenceCase ('positive-' + $mode) { param($f) } -Mode $mode
}
Add-ImageEvidenceCase positive-admission { param($f) } -Mode 'offline-readonly-restore' -Admission $true
foreach ($mode in $supplementaryModes) {
    foreach ($admission in @($false, $true)) {
        $suffix = if ($admission) { '-admission' } else { '-runtime' }
        Add-ImageEvidenceCase ('empty-serials-' + $mode.mode + $suffix) { param($f)
            foreach ($field in @('source', 'target_before')) {
                $f.Report.$field.disk_serial_number = ''
                $f.Arguments.ExpectedFixture.$field.disk_serial_number = ''
            }
            if ($null -ne $f.Report.PSObject.Properties['target_after']) {
                $f.Report.target_after.disk_serial_number = ''
            }
            foreach ($operation in $f.Report.operations) {
                if ($operation.type -ceq 'Restore Image') {
                    $operation.accepted_payload.target_disk_serial_number = ''
                }
            }
        } -Mode $mode.mode -Admission $admission
    }
}
foreach ($field in @('source', 'target_before')) {
    foreach ($origin in @('Report', 'Arguments.ExpectedFixture')) {
        $prefix = if ($origin -ceq 'Report') { 'actual' } else { 'expected' }
        $diagnostic = if ($origin -ceq 'Report') { "pinned $field disk_serial_number must" } else {
            "Pinned $field disk_serial_number must be a string"
        }
        foreach ($bad in @(
            @{ name = 'null'; value = $null }, @{ name = 'number'; value = 0 },
            @{ name = 'boolean'; value = $false }, @{ name = 'array'; value = @('') })) {
            Add-ImageFieldCase ('serial-' + $field + '-' + $prefix + '-' + $bad.name) `
                ($origin + '.' + $field + '.disk_serial_number') $bad.value $diagnostic
        }
        Add-ImageFieldCase ('serial-' + $field + '-' + $prefix + '-missing') `
            ($origin + '.' + $field + '.disk_serial_number') $null 'Missing required field: disk_serial_number' -Remove $true
        Add-ImageFieldCase ('serial-' + $field + '-' + $prefix + '-empty-mismatch') `
            ($origin + '.' + $field + '.disk_serial_number') '' "pinned $field disk_serial_number must"
    }
}
Add-ImageFieldCase serial-restore-payload-missing 'Report.operations.1.accepted_payload.target_disk_serial_number' `
    $null 'Restore Image payload keys do not match accepted request' -Mode offline-restore -Remove $true
foreach ($bad in @(
    @{ name = 'null'; value = $null }, @{ name = 'number'; value = 0 },
    @{ name = 'array'; value = @('') }, @{ name = 'empty-mismatch'; value = '' })) {
    Add-ImageFieldCase ('serial-restore-payload-' + $bad.name) `
        'Report.operations.1.accepted_payload.target_disk_serial_number' $bad.value `
        'restore target_disk_serial_number must' -Mode offline-restore
}
Add-ImageEvidenceCase positive-other-attribute-bit { param($f)
    $f.Arguments.ExpectedFixture.before_preparation_attributes = '9223372036854775808'
    $f.Report.source_attributes_before_preparation = '9223372036854775808'
    $f.Report.source_attributes_before_capture = '9223372036854775809'
    $f.Report.source_attributes_after_capture = '9223372036854775809'
}
Add-ImageEvidenceCase positive-busy-open-error { param($f)
    $f.Report.operations[0].execution.steps[0].stderr = ''
    $f.Report.operations[0].execution.steps[0].error_message = 'Create Image cannot open the source volume for exclusive capture: access denied'
} -Mode busy-source-refusal
foreach ($mode in $supplementaryModes) {
    foreach ($other in $supplementaryModes) {
        if ($mode.mode -ceq $other.mode) { continue }
        Add-ImageFieldCase ('cross-mode-' + $mode.mode + '-as-' + $other.mode) `
            'Arguments.ExpectedProofMode' $other.mode 'proof_mode must' -Mode $mode.mode
    }
}
Add-ImageFieldCase caller-default-mode 'Arguments.ExpectedProofMode' 'restore' 'ExpectedProofMode must'
Add-ImageFieldCase caller-mode-case 'Arguments.ExpectedProofMode' 'OFFLINE-PARTITION-IMAGE' 'ExpectedProofMode must'
Add-ImageFieldCase caller-validation-string 'Arguments.ExpectedValidationOnly' 'false' 'ExpectedValidationOnly must'
Add-ImageFieldCase default-schema 'Report.schema_version' 1 'image proof schema has invalid'
Add-ImageFieldCase string-schema 'Report.schema_version' '2' 'image proof schema has invalid'
Add-ImageFieldCase default-scenario 'Report.scenario_id' 'vhd.image-restore' 'scenario_id must'
Add-ImageFieldCase report-mode-missing 'Report.proof_mode' $null 'Missing required field: proof_mode' -Remove $true
Add-ImageFieldCase wrong-tool 'Report.tool' 'surrogate' 'tool must'
Add-ImageFieldCase wrong-engine 'Report.execution_engine' 'Copy-Item' 'execution_engine must'
Add-ImageFieldCase wrong-status 'Report.status' 'RefusalVerified' 'status must'
Add-ImageFieldCase run-mismatch 'Report.run_id' '6d1e4f7e-a97c-42ad-bd3a-62e6a57150d7' 'run_id must'
Add-ImageFieldCase expected-run-invalid 'Arguments.ExpectedRunId' 'invalid' 'expected run_id must'
Add-ImageFieldCase validation-mismatch 'Report.validation_only' $true 'validation_only must'
foreach ($field in @('self_test', 'error', 'worker_error', 'containment_error')) {
    Add-ImageFieldCase ('forbidden-' + $field) ('Report.' + $field) '' "Field is forbidden for this proof: $field"
}
foreach ($field in @('source_preparation_started', 'controller_attempted', 'mutations_may_have_started',
    'controller_executed', 'worker_descendants_finished', 'source_attributes_preserved', 'source_range_verified',
    'offline_preparation_source_locked', 'offline_preparation_source_dismounted', 'offline_preparation_lock_released')) {
    Add-ImageFieldCase ('false-' + $field) ('Report.' + $field) $false "$field must"
    Add-ImageFieldCase ('string-' + $field) ('Report.' + $field) 'true' "$field must"
}
Add-ImageFieldCase worker-exit 'Report.worker_exit_code' 1 'worker_exit_code has invalid'
Add-ImageFieldCase worker-exit-string 'Report.worker_exit_code' '0' 'worker_exit_code has invalid'
Add-ImageFieldCase active-job 'Report.job_quiescence.last_active_processes' '1' 'observed remaining job processes must'
foreach ($field in @('cancelled', 'timed_out')) {
    Add-ImageFieldCase ('report-' + $field) ('Report.' + $field) $true "$field must"
}
Add-ImageFieldCase operation-count 'Report.operations' @() 'Image proof operation count mismatch'
Add-ImageEvidenceCase scalar-operation { param($f) $f.Report.operations = $f.Report.operations[0] } 'Image proof operation count mismatch'
Add-ImageFieldCase changed-binary-claim 'Report.certifier_binary_sha256' ('0' * 64) 'Expected binary hash mismatch'
Add-ImageFieldCase changed-source-claim 'Report.source_digest' ('0' * 64) 'Source digest does not match'
Add-ImageEvidenceCase changed-current-source { param($f)
    Write-FixtureText (Join-Path $f.Arguments.ProjectRoot 'src/core/partition_manager_controller.cpp') 'changed source'
} 'Source changed: src/core/partition_manager_controller.cpp' -OwnSource
foreach ($field in @('vhd_identifier', 'disk_unique_id', 'disk_serial_number', 'size_bytes', 'bus_type')) {
    $value = switch ($field) {
        vhd_identifier { '918e94af-778e-4fcb-995e-ce4e87c58ff6' }
        size_bytes { '805306368' }
        default { 'different' }
    }
    Add-ImageFieldCase ('pinned-source-' + $field) ('Arguments.ExpectedFixture.source.' + $field) $value "pinned source $field must"
}
Add-ImageFieldCase pinned-source-number 'Arguments.ExpectedFixture.source.disk_number' '7' 'Controller evidence refused: source disk number is invalid'
Add-ImageFieldCase pinned-source-boot 'Arguments.ExpectedFixture.source.is_boot' $true 'source is_boot must'
Add-ImageFieldCase pinned-target-system 'Arguments.ExpectedFixture.target_before.is_system' $true 'target_before is_system must'
Add-ImageFieldCase guest-directory 'Arguments.ExpectedFixture.guest_directory' 'C:\SAK-Cert\other' 'fixture VHD directory binding mismatch'
Add-ImageFieldCase guest-directory-traversal 'Arguments.ExpectedFixture.guest_directory' 'C:\SAK-Cert\fixture\..\fixture' 'expected guest directory is not a canonical path'
Add-ImageEvidenceCase source-self-consistent-forgery { param($f)
    $f.Report.source.disk_unique_id = 'other-source'
    $f.Report.operations[0].target_unique_id = 'other-source'
} 'pinned source disk_unique_id must'
Add-ImageFieldCase partition-number 'Arguments.ExpectedFixture.selected_partition.number' '2' 'Pinned partition number is invalid'
Add-ImageFieldCase partition-guid 'Arguments.ExpectedFixture.selected_partition.guid' '' 'Pinned partition GUID is invalid'
Add-ImageFieldCase partition-offset-overflow 'Arguments.ExpectedFixture.selected_partition.offset_bytes' '18446744073709551616' 'pinned partition offset must be canonical UInt64 text'
Add-ImageFieldCase partition-offset-zero 'Arguments.ExpectedFixture.selected_partition.offset_bytes' '0' 'Pinned partition range exceeds source'
Add-ImageFieldCase partition-size-zero 'Arguments.ExpectedFixture.selected_partition.size_bytes' '0' 'Pinned partition range exceeds source'
Add-ImageFieldCase partition-size-overflow 'Arguments.ExpectedFixture.selected_partition.size_bytes' '18446744073709551615' 'Pinned partition range exceeds source'
Add-ImageFieldCase volume-path 'Arguments.ExpectedFixture.selected_partition.volume_path' 'E:' 'Pinned volume path must'
Add-ImageFieldCase volume-trailing-separator 'Arguments.ExpectedFixture.selected_partition.volume_path' '\\?\Volume{22222222-2222-2222-2222-222222222222}\' 'Pinned volume path must'
Add-ImageFieldCase volume-newline 'Arguments.ExpectedFixture.selected_partition.volume_path' "\\?\Volume{22222222-2222-2222-2222-222222222222}`n" 'Pinned volume path must'
Add-ImageFieldCase volume-empty-guid 'Arguments.ExpectedFixture.selected_partition.volume_path' '\\?\Volume{00000000-0000-0000-0000-000000000000}' 'Pinned volume GUID must not be empty'
Add-ImageFieldCase attributes-newline 'Report.source_attributes_before_capture' "1`n" 'must be canonical UInt64 text'
foreach ($field in @('source_attributes_before_preparation', 'source_attributes_before_capture', 'source_attributes_after_capture')) {
    Add-ImageFieldCase ('numeric-' + $field) ('Report.' + $field) 1 'must be canonical UInt64 text'
    Add-ImageFieldCase ('noncanonical-' + $field) ('Report.' + $field) '01' 'must be canonical UInt64 text'
}
Add-ImageFieldCase initial-attribute-mismatch 'Report.source_attributes_before_preparation' '4' 'Initial native attributes do not match pinned fixture'
Add-ImageEvidenceCase initially-offline { param($f)
    $f.Arguments.ExpectedFixture.before_preparation_attributes = '1'
    $f.Report.source_attributes_before_preparation = '1'
} 'Seeded fixture must start online and writable'
Add-ImageEvidenceCase initially-readonly { param($f)
    $f.Arguments.ExpectedFixture.before_preparation_attributes = '2'
    $f.Report.source_attributes_before_preparation = '2'
} 'Seeded fixture must start online and writable'
Add-ImageFieldCase wrong-prepared-attributes 'Report.source_attributes_before_capture' '0' 'Prepared native attributes do not match selected mode'
Add-ImageFieldCase changed-other-attribute 'Report.source_attributes_after_capture' '5' 'Full source native attributes changed during capture'
Add-ImageFieldCase missing-readonly-preparation 'Report.source_attributes_before_capture' '1' 'Prepared native attributes do not match selected mode' -Mode offline-readonly-restore
Add-ImageFieldCase unexpected-readonly-preparation 'Report.source_attributes_before_capture' '3' 'Prepared native attributes do not match selected mode'
foreach ($field in @('execution_cancelled', 'execution_timed_out')) {
    Add-ImageFieldCase ('create-' + $field) ('Report.operations.0.' + $field) $true "$field must"
    Add-ImageFieldCase ('create-missing-' + $field) ('Report.operations.0.' + $field) $null "Missing required field: $field" -Remove $true
    Add-ImageFieldCase ('restore-' + $field) ('Report.operations.1.' + $field) $true "$field must" -Mode offline-restore
}
foreach ($row in @(
    @('kind', 'target_kind', 'Disk'), @('number', 'target_partition_number', 3),
    @('guid', 'target_partition_guid', '{33333333-3333-3333-3333-333333333333}'),
    @('offset', 'target_offset_bytes', '0'), @('size', 'target_size_bytes', '2048'))) {
    $diagnostic = if ($row[1] -ceq 'target_partition_number') { 'target_partition_number has invalid' } else { $row[1] + ' must' }
    Add-ImageFieldCase ('create-target-' + $row[0]) ('Report.operations.0.' + $row[1]) $row[2] $diagnostic
}
foreach ($row in @(
    @('source_path', '\\.\PhysicalDrive999', 'create source_path must'),
    @('target_path', 'C:\other.img', 'create target_path binding mismatch'),
    @('source_capture_mode', 'LockedVolumes', 'source_capture_mode must'),
    @('verify_mode', 'Sample', 'create verify_mode must'),
    @('source_offset_bytes', '0', 'create source_offset_bytes must'),
    @('source_size_bytes', '2048', 'create source_size_bytes must'))) {
    Add-ImageFieldCase ('create-payload-' + $row[0]) ('Report.operations.0.accepted_payload.' + $row[0]) $row[1] $row[2]
}
Add-ImageFieldCase source-image-path 'Report.source_image' 'C:\other.img' 'source_image binding mismatch'
Add-ImageFieldCase create-payload-extra 'Report.operations.0.accepted_payload.target_offset_bytes' '0' 'Create Image payload keys do not match accepted request'
Add-ImageFieldCase create-payload-missing 'Report.operations.0.accepted_payload.source_capture_mode' $null 'Create Image payload keys do not match accepted request' -Remove $true
Add-ImageFieldCase range-offset 'Report.source_range_offset_bytes' '0' 'source_range_offset_bytes must'
Add-ImageFieldCase range-size 'Report.source_range_size_bytes' '2048' 'source_range_size_bytes must'
Add-ImageFieldCase range-before-hash 'Report.source_range_sha256_before' ('0' * 64) 'source range before hash must'
Add-ImageFieldCase range-after-hash 'Report.source_range_sha256_after' ('0' * 64) 'source range after hash must'
Add-ImageFieldCase local-image-required 'Arguments.LocalImagePath' '' 'An explicit local drive-qualified path is required'
Add-ImageEvidenceCase local-image-alias { param($f) $f.Arguments.LocalImagePath = $f.Arguments.ReportPath } 'Image artifact must not alias bound inputs'
Add-ImageEvidenceCase local-image-length { param($f) Write-FixtureText $f.Arguments.LocalImagePath 'x' } 'Retained image length does not match source range'
Add-ImageEvidenceCase local-image-bytes { param($f) Write-FixtureText $f.Arguments.LocalImagePath ('x' * 1024) } 'retained image hash must'
Add-ImageFieldCase contradictory-absence 'Arguments.ImageAbsenceEvidence' ([pscustomobject]@{ absent = $true }) 'Positive image proof must not claim image absence'
foreach ($field in @('owned_source_lock_refusal', 'image_output_absent', 'independent_source_lock_held', 'independent_source_lock_released')) {
    Add-ImageFieldCase ('busy-false-' + $field) ('Report.' + $field) $false "$field must" -Mode busy-source-refusal
}
Add-ImageFieldCase busy-wrong-lock-partition 'Report.independent_source_lock_partition' 3 'independent_source_lock_partition has invalid' -Mode busy-source-refusal
Add-ImageFieldCase busy-wrong-lock-path 'Report.independent_source_lock_path' '\\?\Volume{33333333-3333-3333-3333-333333333333}' 'independent_source_lock_path must' -Mode busy-source-refusal
Add-ImageFieldCase busy-executed 'Report.controller_executed' $true 'controller_executed must' -Mode busy-source-refusal
Add-ImageFieldCase busy-execution-success 'Report.operations.0.execution.success' $true 'busy execution success must' -Mode busy-source-refusal
Add-ImageFieldCase busy-step-success 'Report.operations.0.execution.steps.0.success' $true 'busy step success must' -Mode busy-source-refusal
Add-ImageFieldCase busy-step-skipped 'Report.operations.0.execution.steps.0.skipped' $true 'step skipped must' -Mode busy-source-refusal
Add-ImageFieldCase busy-dry-run 'Report.operations.0.execution.dry_run' $true 'execution dry_run must' -Mode busy-source-refusal
Add-ImageFieldCase busy-other-error 'Report.operations.0.execution.steps.0.stderr' 'Unrelated failure' 'Busy step did not refuse owned source acquisition' -Mode busy-source-refusal
Add-ImageFieldCase busy-nonstring-diagnostics 'Report.operations.0.execution.steps.0.stderr' @('error') 'Busy step diagnostics must be strings' -Mode busy-source-refusal
foreach ($mode in @('full', 'sample')) {
    Add-ImageFieldCase ('busy-' + $mode + '-witness') 'Report.operations.0.execution.steps.0.stdout' `
        ('Running ' + $mode + ' clone verification') 'Busy step reached copy verification' -Mode busy-source-refusal
}
foreach ($row in @(
    @('run_id', '6d1e4f7e-a97c-42ad-bd3a-62e6a57150d7', 'absence run_id must'),
    @('report_sha256', ('0' * 64), 'absence report hash must'),
    @('image_path', 'C:\other.img', 'absence image path binding mismatch'),
    @('absent', $false, 'independent image absence must'))) {
    Add-ImageFieldCase ('busy-absence-' + $row[0]) ('Arguments.ImageAbsenceEvidence.' + $row[0]) $row[1] $row[2] -Mode busy-source-refusal
}
Add-ImageFieldCase busy-local-artifact 'Arguments.LocalImagePath' 'C:\other.img' 'Absent image proof must not select a local image artifact' -Mode busy-source-refusal
foreach ($field in @('target_after', 'sentinel_sha256', 'sentinel_verified', 'target_overwritten',
    'source_dismounted_before_restore', 'pre_restore_marker')) {
    Add-ImageFieldCase ('capture-forbidden-' + $field) ('Report.' + $field) $false "Field is forbidden for this proof: $field"
}
foreach ($field in @('source_image', 'source_image_sha256', 'source_range_offset_bytes', 'source_range_size_bytes',
    'source_range_sha256_before', 'source_range_sha256_after', 'source_range_verified')) {
    Add-ImageFieldCase ('busy-forbidden-' + $field) ('Report.' + $field) $false "Field is forbidden for this proof: $field" -Mode busy-source-refusal
}
foreach ($field in @('source_attributes_before_preparation', 'source_attributes_before_capture', 'source_attributes_after_capture',
    'source_attributes_preserved', 'offline_preparation_source_locked', 'offline_preparation_source_dismounted',
    'offline_preparation_lock_released', 'independent_source_lock_held', 'source_image', 'target_after')) {
    Add-ImageFieldCase ('admission-forbidden-' + $field) ('Report.' + $field) $false "Field is forbidden for this proof: $field" `
        -Mode offline-readonly-restore -Admission $true
}
foreach ($field in @('source_preparation_started', 'controller_attempted', 'controller_executed', 'mutations_may_have_started')) {
    Add-ImageFieldCase ('admission-' + $field) ('Report.' + $field) $true "$field must" -Mode offline-readonly-restore -Admission $true
}
Add-ImageFieldCase admission-runtime-claim 'Arguments.ExpectedValidationOnly' $false 'status must' -Mode offline-readonly-restore -Admission $true
foreach ($field in @('source_dismounted_before_restore', 'sentinel_verified', 'target_overwritten')) {
    Add-ImageFieldCase ('restore-' + $field) ('Report.' + $field) $false "$field must" -Mode offline-restore
}
Add-ImageFieldCase restore-sentinel-hash 'Report.sentinel_sha256' ('0' * 64) 'sentinel_sha256 must' -Mode offline-restore
Add-ImageEvidenceCase restore-repeated-operation { param($f)
    $id = $f.Report.operations[0].operation_id
    $f.Report.operations[1].operation_id = $id
    $f.Report.operations[1].execution.steps[0].operation_id = $id
} 'Create/restore operation identities must differ' -Mode offline-restore
foreach ($row in @(
    @('source_path', 'C:\other.img', 'restore source_path binding mismatch'),
    @('target_path', '\\.\PhysicalDrive999', 'restore target_path must'),
    @('source_size_bytes', '1024', 'restore source_size_bytes must'),
    @('target_size_bytes', '1024', 'restore target_size_bytes must'),
    @('source_sha256', ('0' * 64), 'restore source_sha256 must'),
    @('target_wipe_confirmed', $false, 'target_wipe_confirmed must'),
    @('target_disk_unique_id', 'other-id', 'restore target_disk_unique_id must'),
    @('target_disk_serial_number', 'other-serial', 'restore target_disk_serial_number must'),
    @('target_disk_size_bytes', '1024', 'restore target_disk_size_bytes must'),
    @('verify_mode', 'Sample', 'restore verify_mode must'))) {
    Add-ImageFieldCase ('restore-payload-' + $row[0]) ('Report.operations.1.accepted_payload.' + $row[0]) $row[1] $row[2] -Mode offline-restore
}
Add-ImageFieldCase restore-payload-extra 'Report.operations.1.accepted_payload.source_capture_mode' 'OfflineDisk' 'Restore Image payload keys do not match accepted request' -Mode offline-restore

function Invoke-EvidenceCase {
    param([object]$Definition, [string]$Root)
    if (-not $Definition.OwnSource) {
        # Report/sidecar-only cases share this run's immutable synthetic source bytes.
        # The production validator still hashes current files on every invocation.
        # Actual source/header mutation cases always receive their own source tree.
        if ($null -eq $script:SharedSourceInputs) {
            $script:SharedSourceInputs = New-EvidenceFixture (Join-Path ([IO.Path]::GetDirectoryName($Root)) 'shared-source-inputs')
        }
        $fixture = New-EvidenceFixture $Root $script:SharedSourceInputs.Arguments.ProjectRoot $script:SharedSourceInputs.Records
    } else { $fixture = New-EvidenceFixture $Root }
    & $Definition.Change $fixture $Definition.Data
    if (-not $fixture.SkipReportWrite) {
        Write-FixtureText $fixture.Arguments.ReportPath ($fixture.Report | ConvertTo-Json -Depth 20)
    }
    if ($fixture.ImageProof -and $fixture.Arguments.ContainsKey('ImageAbsenceEvidence') -and
        $fixture.Arguments.ImageAbsenceEvidence.report_sha256 -ceq '__BIND_REPORT__') {
        $fixture.Arguments.ImageAbsenceEvidence.report_sha256 = Get-FixtureHash $fixture.Arguments.ReportPath
    }
    $caught = $null
    $receipt = $null
    $arguments = $fixture.Arguments
    try {
        if ($fixture.ImageProof) { $receipt = Assert-PartitionManagerImageProofEvidence @arguments }
        else { $receipt = Assert-PartitionManagerControllerEvidence @arguments }
    }
    catch { $caught = $_.Exception.Message }
    if ($Definition.Diagnostic.Length -eq 0) {
        Assert-Fixture ($null -eq $caught) "$($Definition.Name): positive refused: $caught"
        Assert-Fixture ($receipt.Validated -eq $true -and $receipt.SourceFiles -eq 49 -and
            $receipt.PublicHeaders -eq 2 -and $receipt.CodecHeaders -eq 7) "$($Definition.Name): invalid receipt"
        if ($fixture.ImageProof) {
            Assert-Fixture ($receipt.ProofMode -ceq $arguments.ExpectedProofMode -and
                $receipt.RuntimeEvidence -eq (-not $arguments.ExpectedValidationOnly)) "$($Definition.Name): wrong proof receipt"
            if ($arguments.ExpectedValidationOnly) {
                Assert-Fixture ($receipt.ValidationKind -ceq 'AdmissionOnly') 'Admission-only receipt claims execution'
            }
        }
    } else {
        Assert-Fixture ($null -ne $caught) "$($Definition.Name): unsafe evidence accepted"
        Assert-Fixture ($caught.Contains($Definition.Diagnostic)) `
            "$($Definition.Name): wrong refusal: $caught"
    }
    Write-Output "PASS $($Definition.Name)"
}

function Assert-SharedFixtureSources {
    param([string]$Project)
    # Independent oracle: compare the actual bytes and complete directory membership
    # with the fixture's known construction, not a production-validator receipt/cache.
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($relative in Get-FixtureSources) {
        [void]$expected.Add($relative)
        $parent = [IO.Path]::GetDirectoryName($relative).Replace('\', '/')
        while ($parent) {
            [void]$directories.Add($parent)
            $parent = [IO.Path]::GetDirectoryName($parent).Replace('\', '/')
        }
    }
    $actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($Project)
    while ($pending.Count -gt 0) {
        foreach ($entry in [IO.DirectoryInfo]::new($pending.Dequeue()).EnumerateFileSystemInfos()) {
            Assert-Fixture (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Shared source acquired a reparse entry'
            $relative = $entry.FullName.Substring($Project.Length + 1).Replace('\', '/')
            if ($entry -is [IO.DirectoryInfo]) {
                Assert-Fixture ($directories.Remove($relative)) "Unexpected shared source directory: $relative"
                $pending.Enqueue($entry.FullName)
                continue
            }
            Assert-Fixture ($expected.Contains($relative) -and $actual.Add($relative)) "Unexpected shared source: $relative"
            $bytes = [IO.File]::ReadAllBytes($entry.FullName)
            $wanted = $script:Utf8.GetBytes("fixture source $relative`n")
            Assert-Fixture ([Convert]::ToBase64String($bytes) -ceq [Convert]::ToBase64String($wanted)) "Shared source changed: $relative"
        }
    }
    Assert-Fixture ($expected.Count -eq 49 -and $actual.SetEquals($expected) -and $directories.Count -eq 0) 'Shared source membership changed'
}

$selected = @($script:Cases | Where-Object { $Case -eq '' -or $_.Name -ceq $Case })
Assert-Fixture ($selected.Count -gt 0 -and ($Case -eq '' -or $selected.Count -eq 1)) 'Unknown or duplicate selected case'
if ($ListCases) {
    $selected | Select-Object Name, Diagnostic | ConvertTo-Json -Depth 3
    exit 0
}
$temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$caseRoot = Join-Path $temporary ('sak-controller-evidence-' + [guid]::NewGuid().ToString('N'))
Assert-Fixture (-not (Test-Path -LiteralPath $caseRoot)) 'Fixture root already exists'
[void][IO.Directory]::CreateDirectory($caseRoot)
$script:WholeImagePath = Join-Path $caseRoot 'ordinary-whole-source.img'
$script:WholeImageHash = ''
$script:SharedSourceInputs = $null
$fixtureWatch = [Diagnostics.Stopwatch]::StartNew()
$cleanupSeconds = 0.0
try {
    $index = 0
    foreach ($definition in $selected) {
        Invoke-EvidenceCase $definition (Join-Path $caseRoot ([string]$index))
        $index++
    }
    if ($null -ne $script:SharedSourceInputs) {
        Assert-SharedFixtureSources $script:SharedSourceInputs.Arguments.ProjectRoot
        Write-Output 'PASS immutable shared source bytes and exact membership'
    }
} finally {
    $cleanupWatch = [Diagnostics.Stopwatch]::StartNew()
    $resolved = [IO.Path]::GetFullPath($caseRoot)
    Assert-Fixture ($resolved.StartsWith($temporary + '\', [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetDirectoryName($resolved) -ceq $temporary -and
        [IO.Path]::GetFileName($resolved) -cmatch '^sak-controller-evidence-[0-9a-f]{32}$') 'Unsafe fixture cleanup root'
    # Remove only generated directory links themselves before recursive cleanup. Never traverse targets.
    foreach ($directory in Get-ChildItem -LiteralPath $resolved -Directory) {
        $link = Join-Path $directory.FullName 'project\include\sak\linked'
        if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link) }
    }
    # All entries are generated ordinary fixture files; the sole generated junction
    # was removed above. Avoid PowerShell provider overhead per file during deletion.
    [IO.Directory]::Delete($resolved, $true)
    $cleanupWatch.Stop()
    $cleanupSeconds = $cleanupWatch.Elapsed.TotalSeconds
}
$fixtureWatch.Stop()
# Success is published only after complete cleanup, never before a late cleanup timeout.
Write-Output "Controller evidence fixtures: $index passed"
Write-Output ("Fixture timing: total={0:F3}s cleanup={1:F3}s" -f $fixtureWatch.Elapsed.TotalSeconds, $cleanupSeconds)
