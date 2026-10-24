<#
.SYNOPSIS
    Non-destructive contracts for controller-driven restore routing and cleanup.
.DESCRIPTION
    Imports only named production functions through the PowerShell AST. Storage and
    live-certifier boundaries are mocked. Harmless native exit-code and in-process
    failure fixtures exercise checked steps. A disposable non-Git package executes only
    the producer's metadata-only mode, without RunVhdDataDiskMatrix. No storage mutation runs.
.PARAMETER DryProducerOnly
    Runs only the disposable non-Git metadata/return contract. RunGuardDrills adds its two
    isolated counterexamples; no restore scenario or storage operation is enabled.
#>
[CmdletBinding()]
param(
    [string]$HarnessPath = "",
    [string]$OutputRoot = "artifacts\partition-manager-certification\tool-tests\controller-routing",
    [switch]$RunGuardDrills,
    [switch]$DryProducerOnly
)
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ([string]::IsNullOrWhiteSpace($HarnessPath)) { $HarnessPath = Join-Path $PSScriptRoot "run_partition_manager_destructive_certification.ps1" }
$HarnessPath = (Resolve-Path -LiteralPath $HarnessPath -ErrorAction Stop).Path
$Script:ProductionText = Get-Content -LiteralPath $HarnessPath -Raw -ErrorAction Stop
if (-not [IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot = Join-Path $ProjectRoot $OutputRoot }
$Script:FixtureRoot = Join-Path $OutputRoot ([guid]::NewGuid().ToString("D"))
New-Item -ItemType Directory -Path $Script:FixtureRoot -ErrorAction Stop | Out-Null

function Assert-Routing {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Assert-Throws {
    param([scriptblock]$Body, [string]$Expected)
    $message = ""
    try { & $Body | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Routing ($message.Contains($Expected)) "Expected refusal '$Expected'; got '$message'"
}
function Get-RoutingFunctionText {
    param([string]$Text, [string]$Name)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    Assert-Routing ($errors.Count -eq 0) "Production harness parse failed"
    $nodes = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $Name
    }, $true))
    Assert-Routing ($nodes.Count -eq 1) "Expected exactly one production function: $Name"
    return $nodes[0].Extent.Text
}
function Import-RoutingFunctions {
    param([string]$FunctionSourceText, [string[]]$FunctionNames)
    foreach ($functionName in $FunctionNames) { . ([scriptblock]::Create((Get-RoutingFunctionText $FunctionSourceText $functionName))) }
}
function New-RoutingState {
    $runId = [guid]::NewGuid().ToString("D")
    return @{
        run_id = $runId; directory = $Script:FixtureRoot
        source = @{ path = (Join-Path $Script:FixtureRoot "source.vhdx"); size_mb = 768 }
        target = @{ path = (Join-Path $Script:FixtureRoot "target.vhdx"); size_mb = 768 }
        certifier = @{ path = "C:\fixture\certifier.exe"; binary_hash = ("a" * 64); source_digest = ("b" * 64) }
        report_path = (Join-Path $Script:FixtureRoot ($runId + ".json"))
        report_reference = "controller-restore-$runId/controller-restore-report.json"
        source_marker = "fixture-sentinel"; source_marker_hash = ("c" * 64); target_marker = "target-marker"
        cleanup_allowed = $false; worker_descendants_finished = $null; preservation_reason = ""; report = $null
    }
}
function New-RoutingReport {
    param([hashtable]$State)
    $operations = @()
    foreach ($type in @("Create Image", "Restore Image")) {
        $id = [guid]::NewGuid().ToString("D")
        $operations += @{
            type = $type; operation_id = $id; queued_script_sha256 = ("d" * 64)
            review_fingerprint = "queue-fingerprint"; layout_hash = "layout-hash"
            execution = @{ success = $true; dry_run = $false; steps = @(@{ operation_id = $id; success = $true; skipped = $false }) }
        }
    }
    $report = @{
        tool = "partition-manager-controller-live-certifier"; schema_version = 1; scenario_id = "vhd.image-restore"
        run_id = $State.run_id; source_vhd = $State.source.path; target_vhd = $State.target.path
        certifier_binary_sha256 = $State.certifier.binary_hash; source_digest = $State.certifier.source_digest
        validation_only = $false; worker_descendants_finished = $true; status = "Passed"
        controller_executed = $true; execution_engine = "PartitionManagerController"
        source_dismounted_before_restore = $true; sentinel_verified = $true; target_overwritten = $true
        operations = $operations; source_image = (Join-Path $State.directory "controller-source.img")
        source_image_sha256 = ("e" * 64); sentinel_sha256 = $State.source_marker_hash
        target_after = @{ vhd_path = $State.target.path; disk_unique_id = "restored-target-id" }
    }
    return $report | ConvertTo-Json -Depth 20 | ConvertFrom-Json
}
function Save-RoutingReport {
    param([object]$Report, [hashtable]$State)
    $json = $Report | ConvertTo-Json -Depth 30
    [IO.File]::WriteAllText($State.report_path, $json, [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ stdout = $json; stderr = ""; exit_code = 0; timed_out = $false }
}
function Test-NonGitProducerReturn {
    param([string]$Text = $Script:ProductionText)
    . ([scriptblock]::Create($Script:StrictSteps))
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $package = Join-Path $temporary ('sak-nongit-producer-' + [guid]::NewGuid().ToString('N'))
    Assert-Routing (-not $package.StartsWith($ProjectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) `
        'Non-Git fixture must be outside the repository'
    Assert-Routing (-not (Test-Path -LiteralPath $package)) 'Non-Git fixture already exists'
    New-Item -ItemType Directory -Path (Join-Path $package scripts) -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $package certification) -ErrorAction Stop | Out-Null
    try {
        $producer = Join-Path $package 'scripts/run_partition_manager_destructive_certification.ps1'
        [IO.File]::WriteAllText($producer, $Text, [Text.UTF8Encoding]::new($false))
        foreach ($name in @('partition_manager_storage.ps1', 'process_capture.ps1')) {
            [IO.File]::Copy((Join-Path $PSScriptRoot $name), (Join-Path $package "scripts/$name"))
        }
        [IO.File]::Copy((Join-Path $ProjectRoot 'certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json'),
            (Join-Path $package 'certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json'))
        $directOutput = Join-Path $package direct
        $global:LASTEXITCODE = 83
        # No RunVhdDataDiskMatrix, RelaunchElevated or helper executable is supplied.
        $records = @(& $producer -OutputRoot $directOutput 2>&1)
        $scriptSucceeded = $?
        $producerExit = $LASTEXITCODE
        Assert-Routing ($scriptSucceeded -and $producerExit -eq 0) `
            "Optional Git metadata leaked producer failure: exit=$producerExit succeeded=$scriptSucceeded"
        Assert-Routing (@($records | Where-Object { $_ -is [Management.Automation.ErrorRecord] }).Count -eq 0) `
            'Optional Git metadata leaked a PowerShell error record'
        $reports = @(Get-ChildItem -LiteralPath $directOutput -Recurse -File -Filter 'partition-manager-certification-report.json')
        Assert-Routing ($reports.Count -eq 1) 'Metadata-only package run did not produce exactly one report'
        $report = Get-Content -LiteralPath $reports[0].FullName -Raw | ConvertFrom-Json
        Assert-Routing ($report.status -ceq 'NotRun' -and $report.run_vhd_data_disk_matrix -eq $false -and
            $report.summary.passed -eq 0 -and $report.summary.failed -eq 0 -and $report.summary.skipped -eq 12) `
            'Metadata-only run changed scenario execution or evidence status'
        Assert-Routing ($report.provenance.git_head -like 'unavailable*') 'Fixture unexpectedly discovered a Git repository'
        Assert-Routing (@($report.provenance.git_probes).Count -eq 1 -and
            $report.provenance.git_probes[0].exit_code -ne 0 -and
            $report.provenance.git_probes[0].stderr -like '*not a git repository*' -and
            -not [string]::IsNullOrWhiteSpace($report.provenance.git_error)) 'Actual optional Git failure reason was not retained'
        Invoke-CheckedStep -Body { & $producer -OutputRoot (Join-Path $package checked) } `
            -Name 'metadata-only producer' -Workflow 'fixture' | Out-Null
        $global:LASTEXITCODE = 83
        & $producer -OutputRoot (Join-Path $package required) -RequireVhdDataDiskEvidence | Out-Null
        $requiredExit = $LASTEXITCODE
        Assert-Routing ($requiredExit -eq 2) 'Missing required VHD evidence did not preserve its declared exit2'
        Assert-Routing (@(Get-ChildItem -LiteralPath $package -Recurse -File -Filter '*.vhd*').Count -eq 0) `
            'Metadata-only fixture unexpectedly created VHD media'
        $report | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $Script:FixtureRoot 'non-git-return-report.json') -Encoding UTF8
    } finally {
        $resolved = [IO.Path]::GetFullPath($package)
        Assert-Routing ([IO.Path]::GetDirectoryName($resolved) -ceq $temporary -and
            [IO.Path]::GetFileName($resolved) -cmatch '^sak-nongit-producer-[0-9a-f]{32}$') 'Unsafe non-Git fixture cleanup root'
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    Write-Host 'PASS non-Git metadata-only producer return, diagnostics and strict-step contract'
}

function Test-ProvenanceGuardDrills {
    $exitAnchor = '    exit 0'
    Assert-Routing (($Script:ProductionText.Split(@($exitAnchor), [StringSplitOptions]::None).Count - 1) -eq 1) `
        'Success return mutation anchor is missing or ambiguous'
    $missingExit = $Script:ProductionText.Replace($exitAnchor, '    # Deliberately missing success exit in disposable counterexample')
    Assert-Throws { Test-NonGitProducerReturn $missingExit } 'Optional Git metadata leaked producer failure'
    Write-Host 'KILLED producer-success-return'
    $captureAnchor = '            $result.probes += $probe'
    Assert-Routing (($Script:ProductionText.Split(@($captureAnchor), [StringSplitOptions]::None).Count - 1) -eq 1) `
        'Git diagnostic capture mutation anchor is missing or ambiguous'
    $lostDiagnostic = $Script:ProductionText.Replace($captureAnchor, "            `$probe.stderr = ''`n$captureAnchor")
    Assert-Throws { Test-NonGitProducerReturn $lostDiagnostic } 'Actual optional Git failure reason was not retained'
    Write-Host 'KILLED producer-git-diagnostic-retention'
}

function Test-RoutingCleanup {
    param([string]$Text, [switch]$FailAfterUnknownStop)
    $priorStop = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
    $priorStopValue = if ($null -ne $priorStop) { $priorStop.Value } else { $null }
    # Each fixture owns a simulated run. Production keeps its stop sticky; only the
    # fixture boundary restores the caller's exact variable presence and value.
    $Script:VhdSafetyStopReason = ''
    try {
        . Import-RoutingFunctions $Text @("Remove-DisposableVhdDisk", "Complete-ControllerRestoreCleanup")
        $KeepVhd = $false
        $Script:Calls = [Collections.Generic.List[string]]::new()
        $Script:DetachFails = $false
        function Test-Path { param($LiteralPath) return $true }
        function Dismount-SakDiskImageIfAttached {
            param($Path)
            $Script:Calls.Add("detach:$Path")
            if ($Script:DetachFails) { throw "fixture detach failure" }
        }
        function Remove-Item { param($LiteralPath, [switch]$Force) $Script:Calls.Add("delete:$LiteralPath") }
        Remove-DisposableVhdDisk @{ path = "fixture.vhdx" }
        Assert-Routing (($Script:Calls -join ",") -ceq "detach:fixture.vhdx,delete:fixture.vhdx") "Detached-success cleanup did not delete exactly once"
        $Script:Calls.Clear()
        $KeepVhd = $true
        Remove-DisposableVhdDisk @{ path = "fixture.vhdx" }
        Assert-Routing (($Script:Calls -join ",") -ceq "detach:fixture.vhdx") "KeepVhd must retain the detached file"
        $KeepVhd = $false
        $Script:Calls.Clear()
        $Script:DetachFails = $true
        Assert-Throws { Remove-DisposableVhdDisk @{ path = "fixture.vhdx" } } "fixture detach failure"
        Assert-Routing (($Script:Calls -join ",") -ceq "detach:fixture.vhdx") "Deleted VHD after failed detach"
        $Script:Calls.Clear()
        $state = @{ cleanup_allowed = $true; source = @{ path = "source" }; target = @{ path = "target" } }
        $errors = Complete-ControllerRestoreCleanup $state
        Assert-Routing ($errors.Count -eq 2 -and ($Script:Calls -join ",") -ceq "detach:target,detach:source") "Both cleanup failures must remain visible"
        foreach ($flag in @($false, $null)) {
            $Script:Calls.Clear()
            $state.cleanup_allowed = $flag
            $state.report_path = "fixture-report.json"
            Complete-ControllerRestoreCleanup $state | Out-Null
            Assert-Routing ($Script:Calls.Count -eq 0) "Unconfirmed worker triggered fixture cleanup"
            Assert-Routing ($state.preservation_reason.Contains("preserved")) "Preservation reason is missing"
            Assert-Routing ($Script:VhdSafetyStopReason.Contains($state.preservation_reason)) "Unknown cleanup did not set the production run stop"
            # Force a real assertion after production changed the variable, proving
            # exceptional fixture exit restores state as well as normal completion.
            Assert-Routing (-not $FailAfterUnknownStop) "Injected cleanup fixture assertion failure"
        }
    } finally {
        if ($null -eq $priorStop) { Remove-Variable VhdSafetyStopReason -Scope Script }
        else { $Script:VhdSafetyStopReason = $priorStopValue }
    }
}
function Test-RoutingCleanupIsolation {
    param([string]$Text)
    $outerStop = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
    $outerStopValue = if ($null -ne $outerStop) { $outerStop.Value } else { $null }
    $variants = @(
        @{ name = 'absent'; exists = $false; value = $null },
        @{ name = 'null'; exists = $true; value = $null },
        @{ name = 'empty'; exists = $true; value = '' },
        @{ name = 'prior-stop'; exists = $true; value = 'earlier run safety stop' },
        @{ name = 'mixed-case'; exists = $true; value = 'earlier mixed-case stop'; variable_name = 'vHdSaFeTyStOpReAsOn' }
    )
    try {
        foreach ($variant in $variants) {
            foreach ($failAssertion in @($false, $true)) {
                if ($null -ne (Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' })) {
                    Remove-Variable VhdSafetyStopReason -Scope Script
                }
                $variableName = if ($variant.ContainsKey('variable_name')) { $variant.variable_name } else { 'VhdSafetyStopReason' }
                if ($variant.exists) { Set-Variable -Name $variableName -Value $variant.value -Scope Script }
                $failure = $null
                try {
                    if ($failAssertion) { Test-RoutingCleanup $Text -FailAfterUnknownStop }
                    else { Test-RoutingCleanup $Text }
                } catch { $failure = $_ }
                if ($failAssertion) {
                    Assert-Routing ($null -ne $failure -and $failure.Exception.Message -ceq 'Injected cleanup fixture assertion failure') 'Cleanup isolation did not exercise the intended assertion failure'
                } else { Assert-Routing ($null -eq $failure) "Cleanup isolation baseline failed: $failure" }
                $after = Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' }
                $context = "$($variant.name), assertion_failure=$failAssertion"
                Assert-Routing (($null -ne $after) -eq $variant.exists) "Cleanup fixture leaked run-stop variable presence: $context"
                if ($variant.exists) {
                    Assert-Routing ([object]::Equals($after.Value, $variant.value)) "Cleanup fixture changed prior run-stop value: $context"
                    Assert-Routing ($after.Name -ceq $variableName) "Cleanup fixture changed prior run-stop variable name: $context"
                }
            }
        }
    } finally {
        if ($null -ne (Get-Variable -Scope Script | Where-Object { $_.Name -ieq 'VhdSafetyStopReason' })) {
            Remove-Variable VhdSafetyStopReason -Scope Script
        }
        if ($null -ne $outerStop) { Set-Variable -Name $outerStop.Name -Value $outerStopValue -Scope Script }
    }
}
function Test-RoutingReport {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Assert-CertificationCondition", "Assert-ControllerRestoreBoolean", "Test-ControllerRestorePath", "Assert-ControllerRestoreIdentity", "Read-ControllerRestoreReport")
    $MaximumControllerReportBytes = 4MB
    $state = New-RoutingState
    $report = New-RoutingReport $state
    $captured = Save-RoutingReport $report $state
    Read-ControllerRestoreReport $state $captured | Out-Null
    Assert-Routing ($state.cleanup_allowed -and $state.worker_descendants_finished) "Observed quiescence did not allow cleanup"
    foreach ($bad in @($false, "true", $null)) {
        $state.cleanup_allowed = $false
        $report.worker_descendants_finished = $bad
        $captured = Save-RoutingReport $report $state
        Assert-Throws { Read-ControllerRestoreReport $state $captured } "worker_descendants_finished"
        Assert-Routing (-not $state.cleanup_allowed) "Invalid quiescence authorized cleanup"
    }
    $report.PSObject.Properties.Remove("worker_descendants_finished")
    $captured = Save-RoutingReport $report $state
    Assert-Throws { Read-ControllerRestoreReport $state $captured } "worker_descendants_finished"
    foreach ($field in @("run_id", "source_vhd", "target_vhd", "certifier_binary_sha256", "source_digest")) {
        $report = New-RoutingReport $state
        $report.$field = "wrong"
        $captured = Save-RoutingReport $report $state
        Assert-Throws { Read-ControllerRestoreReport $state $captured } "mismatch"
        Assert-Routing (-not $state.cleanup_allowed) "$field mismatch authorized cleanup"
    }
    $report = New-RoutingReport $state
    $captured = Save-RoutingReport $report $state
    $captured.timed_out = $true
    Assert-Throws { Read-ControllerRestoreReport $state $captured } "outer deadline"
    $captured.timed_out = $false
    $captured.stdout = '{"different":true}'
    Assert-Throws { Read-ControllerRestoreReport $state $captured } "disagree"
    $state.report_path = Join-Path $Script:FixtureRoot "absent-report.json"
    Assert-Throws { Read-ControllerRestoreReport $state $captured } "report is missing"
}
function Test-RoutingOperations {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Assert-CertificationCondition", "Assert-ControllerRestoreBoolean", "Assert-ControllerRestoreOperations")
    $state = New-RoutingState
    $report = New-RoutingReport $state
    Assert-ControllerRestoreOperations $report
    $report.operations = @($report.operations[0])
    Assert-Throws { Assert-ControllerRestoreOperations $report } "exactly two"
    $report = New-RoutingReport $state
    $report.operations[1].execution.dry_run = $true
    Assert-Throws { Assert-ControllerRestoreOperations $report } "dry_run"
    $report = New-RoutingReport $state
    $report.operations[1].execution.steps[0].skipped = $true
    Assert-Throws { Assert-ControllerRestoreOperations $report } "skipped"
    $report = New-RoutingReport $state
    $report.operations[0].execution.steps[0].operation_id = "other"
    Assert-Throws { Assert-ControllerRestoreOperations $report } "identity mismatch"
    $report = New-RoutingReport $state
    $report.operations[0].queued_script_sha256 = ""
    Assert-Throws { Assert-ControllerRestoreOperations $report } "digest is missing"
}
function Test-RoutingDiskBinding {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Assert-CertificationCondition", "Assert-ControllerRestoreBoolean", "Get-ControllerFixtureDisk")
    $Script:DiskFacts = [pscustomobject]@{ Number = 7; IsBoot = $false; IsSystem = $false; Size = 768MB }
    $Script:DiskCopies = 1
    function Get-AttachedVhdDisk {
        param($Path)
        Assert-Routing ($Path -ceq "exact-fixture.vhdx") "Disk resolution did not use exact VHD path"
        for ($index = 0; $index -lt $Script:DiskCopies; $index++) { $Script:DiskFacts }
    }
    $vhd = @{ path = "exact-fixture.vhdx"; size_mb = 768 }
    Assert-Routing ((Get-ControllerFixtureDisk $vhd).Number -eq 7) "Exact VHD positive binding failed"
    foreach ($field in @("IsBoot", "IsSystem")) {
        $Script:DiskFacts.$field = $true
        Assert-Throws { Get-ControllerFixtureDisk $vhd } $field
        $Script:DiskFacts.$field = $false
    }
    $Script:DiskFacts.Size = 769MB
    Assert-Throws { Get-ControllerFixtureDisk $vhd } "size mismatch"
    $Script:DiskFacts.Size = 768MB
    $Script:DiskCopies = 2
    Assert-Throws { Get-ControllerFixtureDisk $vhd } "ambiguous"
}
function Test-RoutingLaunch {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Invoke-ControllerRestore")
    $ControllerCertifierTimeoutSeconds = 930
    $state = New-RoutingState
    $state.cleanup_allowed = $true
    $Script:Capture = $null
    function Invoke-CapturedProcess {
        param($FilePath, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        $Script:Capture = @{ file = $FilePath; args = $Arguments; directory = $WorkingDirectory; timeout = $TimeoutSeconds }
        throw "fixture launch uncertainty"
    }
    Assert-Throws { Invoke-ControllerRestore $state } "fixture launch uncertainty"
    Assert-Routing (-not $state.cleanup_allowed) "Launch uncertainty allowed fixture cleanup"
    Assert-Routing ($Script:Capture.file -ceq $state.certifier.path -and $Script:Capture.timeout -eq 930) "Wrong helper or unbounded process launch"
    $expected = @("--source-vhd", $state.source.path, "--target-vhd", $state.target.path, "--output-dir", $state.directory, "--run-id", $state.run_id, "--confirm-disposable-vhds")
    Assert-Routing (($Script:Capture.args -join "|") -ceq ($expected -join "|")) "Controller route did not pass exact VHD paths and run identity"
    $body = Get-RoutingFunctionText $Text "Invoke-VhdImageRestoreScenario"
    Assert-Routing (-not $body.Contains("Copy-Item") -and $body.Contains("Invoke-ControllerRestore")) "Restore still uses a container-copy surrogate"
}
function Test-RoutingSelector {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Invoke-VhdScenario")
    $RunVhdDataDiskMatrix = $true
    $VhdScenarioId = "vhd.image-restore"
    $Script:SelectedResults = @()
    function Test-Administrator { return $true }
    function Assert-EvidenceContract { param($Id, $Evidence) }
    function New-CertificationResult { param($Id, $Name, $Status, $Message, $Evidence) return [pscustomobject]@{ id = $Id; status = $Status } }
    function Add-CertificationResult { param($Result) $Script:SelectedResults += $Result }
    Invoke-VhdScenario "vhd.image-clone" "not selected" { throw "NONSELECTED BODY EXECUTED" }
    Assert-Routing ($Script:SelectedResults.Count -eq 1 -and $Script:SelectedResults[0].status -ceq "Skipped") "Focused run executed a nonselected scenario"
    Invoke-VhdScenario "vhd.image-restore" "selected" { return @{ proof = $true } }
    Assert-Routing ($Script:SelectedResults[1].status -ceq "Passed") "Selected scenario did not run"
    $VhdScenarioId = ""
    Invoke-VhdScenario "vhd.image-clone" "default" { return @{ proof = $true } }
    Assert-Routing ($Script:SelectedResults[2].status -ceq "Passed") "Default all-scenario route changed"
}
function Test-RoutingIndependentVerification {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Assert-CertificationCondition", "Test-ControllerRestorePath", "Test-ControllerRestoredTarget")
    $state = New-RoutingState
    $state.report = New-RoutingReport $state
    $Script:MarkerPresent = $false
    $Script:SentinelHash = $state.source_marker_hash
    $Script:StorageCalls = @()
    function Get-DiskImage { param($ImagePath) return [pscustomobject]@{ Attached = $false } }
    function Dismount-SakDiskImageIfAttached { param($Path) $Script:StorageCalls += "detach:$Path" }
    function Mount-DiskImage { param($ImagePath, $ErrorAction) $Script:StorageCalls += "attach:$ImagePath" }
    function Get-ControllerFixtureDisk { param($Vhd) return [pscustomobject]@{ Number = 7; UniqueId = "restored-target-id" } }
    function Get-Partition { param($DiskNumber, $ErrorAction) return [pscustomobject]@{ PartitionNumber = 2 } }
    function Get-SakPartitionVolume { param($Partition) return [pscustomobject]@{ FileSystemLabel = "SAKRESTSRC"; FileSystem = "NTFS" } }
    function Get-CertificationDriveLetter { param($DiskNumber, $PartitionNumber) return "T" }
    function Join-Path { param($Path, $ChildPath) return [IO.Path]::Combine($Path, $ChildPath) }
    function Get-Content { param($LiteralPath, [switch]$Raw, $ErrorAction) return "fixture-sentinel" }
    function Test-Path { param($LiteralPath) return $Script:MarkerPresent }
    function Get-FileHash {
        param($LiteralPath, $Algorithm)
        $hash = if ($LiteralPath.EndsWith("sak-partition-manager-restore.txt")) { $Script:SentinelHash } elseif ($LiteralPath.EndsWith(".img")) { "e" * 64 } else { "f" * 64 }
        return [pscustomobject]@{ Hash = $hash }
    }
    $evidence = Test-ControllerRestoredTarget $state
    Assert-Routing ($evidence.independent_verification -and $evidence.target_disk_number -eq 7) "Independent verification evidence missing"
    Assert-Routing ($evidence.controller_report -ceq $state.report_reference -and -not [IO.Path]::IsPathRooted($evidence.controller_report)) "Controller report reference must be portable under enclosing report directory"
    $expected = @(("detach:" + $state.target.path), ("attach:" + $state.target.path))
    Assert-Routing (($Script:StorageCalls -join "|") -ceq ($expected -join "|")) "Independent check did not reattach only the exact target"
    $Script:MarkerPresent = $true
    Assert-Throws { Test-ControllerRestoredTarget $state } "pre-restore target marker"
    $Script:MarkerPresent = $false
    $Script:SentinelHash = "0" * 64
    Assert-Throws { Test-ControllerRestoredTarget $state } "sentinel content/hash mismatch"
}

function Test-StrictStepStatus {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Invoke-CheckedStep")
    $stepOptions = @{ Name = "fixture" }
    if ((Get-RoutingFunctionText $Text "Invoke-CheckedStep").Contains('[string]$Workflow')) {
        $stepOptions.Workflow = "fixture"
    }
    $output = @(Invoke-CheckedStep @stepOptions -Body { "normal-output" })
    Assert-Routing ($output.Count -eq 1 -and $output[0] -ceq "normal-output") "Checked step changed successful output"
    $native = Join-Path ([Environment]::SystemDirectory) "cmd.exe"
    Invoke-CheckedStep @stepOptions -Body { & $native /d /c exit 0 }
    Assert-Throws { Invoke-CheckedStep @stepOptions -Body { & $native /d /c exit 7 } } "step failed"
    $childPath = Join-Path $Script:FixtureRoot "nonterminating-child.ps1"
    [IO.File]::WriteAllText($childPath, "Write-Error 'fixture child error' -ErrorAction Continue", [Text.UTF8Encoding]::new($false))
    Assert-Throws { Invoke-CheckedStep @stepOptions -Body { & $childPath } } "step failed"
}

function Test-StrictArtifactGuards {
    param([string]$Text)
    . Import-RoutingFunctions $Text @("Invoke-CheckedStep", "Clear-GeneratedArtifact", "Assert-GeneratedArtifact", "Invoke-GeneratedArtifactStep")
    $path = Join-Path $Script:FixtureRoot ([guid]::NewGuid().ToString("D") + ".json")
    [IO.File]::WriteAllText($path, "stale", [Text.UTF8Encoding]::new($false))
    Assert-Throws { Invoke-GeneratedArtifactStep -Workflow fixture -Name noop -Paths @($path) -Body {} } "did not produce"
    Assert-Routing (-not (Test-Path -LiteralPath $path)) "No-op producer retained stale artifact"
    Invoke-GeneratedArtifactStep -Workflow fixture -Name positive -Paths @($path) -Body {
        [IO.File]::WriteAllText($path, "fresh", [Text.UTF8Encoding]::new($false))
    }
    Assert-Routing ([IO.File]::ReadAllText($path) -ceq "fresh") "Positive artifact was not regenerated"
    $folder = Join-Path $Script:FixtureRoot ([guid]::NewGuid().ToString("D"))
    New-Item -ItemType Directory -Path $folder -ErrorAction Stop | Out-Null
    Assert-Throws { Clear-GeneratedArtifact $folder } "not a direct file"
    function Remove-Item { param($LiteralPath, [switch]$Force, $ErrorAction) }
    Assert-Throws { Clear-GeneratedArtifact $path } "Could not clear stale"
}

function Get-StrictWrapperAst {
    param([string]$Text)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    Assert-Routing ($errors.Count -eq 0) "Strict wrapper parse failed"
    return $ast
}

function Get-StrictArtifactInvocation {
    param([object]$Ast, [string]$Name, [string]$Body)
    $matches = @($Ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -in @("Invoke-CheckedStep", "Invoke-GeneratedArtifactStep") -and
            $node.Extent.Text.Contains('-Name "' + $Name + '"')
    }, $true))
    Assert-Routing ($matches.Count -eq 1) "Expected one artifact producer: $Name"
    $bodyNode = @($matches[0].CommandElements | Where-Object { $_ -is [Management.Automation.Language.ScriptBlockExpressionAst] })
    Assert-Routing ($bodyNode.Count -eq 1) "Expected one bounded producer body"
    return $matches[0].Extent.Text.Replace($bodyNode[0].Extent.Text, $Body)
}

function Test-StrictWrapperArtifacts {
    param([string]$Text, [string]$Steps, [string]$Kind)
    . Import-RoutingFunctions $Steps @("Invoke-CheckedStep", "Clear-GeneratedArtifact", "Assert-GeneratedArtifact", "Invoke-GeneratedArtifactStep")
    $ast = Get-StrictWrapperAst $Text
    $prefix = if ($Kind -eq "vhd") { "" } else { "hardware " }
    $paths = @{}
    foreach ($name in @("statusPath", "gapReportPath", "gapMarkdownPath", "bundlePath", "preflightPath")) {
        $paths[$name] = Join-Path $Script:FixtureRoot ($Kind + "-" + $name + "-" + [guid]::NewGuid().ToString("D") + ".json")
        Set-Variable -Name $name -Value $paths[$name]
    }
    $cases = @(
        @{ name = ($prefix + "certification status"); keys = @("statusPath") },
        @{ name = ($prefix + "certification gap report"); keys = @("gapReportPath", "gapMarkdownPath") },
        @{ name = ($prefix + "certification artifact bundle"); keys = @("bundlePath") }
    )
    if ($Kind -eq "vhd") { $cases += @{ name = "VHD preflight"; keys = @("preflightPath") } }
    foreach ($case in $cases) {
        $expectedPaths = @($case.keys | ForEach-Object { $paths[$_] })
        foreach ($path in $expectedPaths) { [IO.File]::WriteAllText($path, "stale") }
        $call = Get-StrictArtifactInvocation $ast $case.name '{}'
        Assert-Throws { & ([scriptblock]::Create($call)) } "did not produce"
        foreach ($path in $expectedPaths) { Assert-Routing (-not (Test-Path -LiteralPath $path)) "Stale wrapper artifact survived: $path" }
        $fresh = '{ foreach ($path in $expectedPaths) { [IO.File]::WriteAllText($path, "fresh") } }'
        & ([scriptblock]::Create((Get-StrictArtifactInvocation $ast $case.name $fresh)))
        foreach ($path in $expectedPaths) { Assert-Routing ([IO.File]::ReadAllText($path) -ceq "fresh") "Wrapper artifact was not freshly generated" }
    }
}

function Test-StrictWrapperThreading {
    param([string]$Text, [string]$Kind)
    $ast = Get-StrictWrapperAst $Text
    $parameters = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
    foreach ($name in @("ControllerCertifierPath", "ControllerSourceManifestPath")) {
        Assert-Routing ($parameters -contains $name) "Strict wrapper lacks parameter $name"
    }
    $copies = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -in @("Invoke-CheckedStep", "Clear-GeneratedArtifact", "Assert-GeneratedArtifact")
    }, $true))
    Assert-Routing ($copies.Count -eq 0 -and $Text.Contains('partition_manager_certification_steps.ps1')) "Strict wrapper retains a separate step/artifact implementation"
    foreach ($command in @("verify_partition_manager_certification.ps1", "get_partition_manager_certification_status.ps1")) {
        $calls = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -ceq ("scripts\" + $command)
        }, $true))
        Assert-Routing ($calls.Count -eq 1) "Strict wrapper consumer call missing: $command"
        foreach ($name in @("ControllerCertifierPath", "ControllerSourceManifestPath")) {
            Assert-Routing ($calls[0].Extent.Text.Contains("-$name " + '$' + $name)) "Strict wrapper does not forward $name to $command"
        }
    }
    if ($Kind -eq "vhd") {
        $assignments = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.AssignmentStatementAst] -and
                $node.Left.Extent.Text -ceq '$harnessArgs'
        }, $true))
        Assert-Routing ($assignments.Count -eq 1) "Expected one producer argument map"
        $resolvedOutputRoot = $Script:FixtureRoot
        $VhdSizeMB = 768
        $ControllerCertifierPath = "C:\selected\helper.exe"
        . ([scriptblock]::Create($assignments[0].Extent.Text))
        Assert-Routing ($harnessArgs.ControllerCertifierPath -ceq $ControllerCertifierPath) "Strict VHD wrapper does not forward selected helper to producer"
    }
}

function Test-StrictVhdRelaunch {
    param([string]$Text)
    $processText = Get-Content -LiteralPath (Join-Path $PSScriptRoot "process_capture.ps1") -Raw
    . Import-RoutingFunctions $processText @("ConvertTo-WindowsProcessArgument")
    $body = Get-RoutingFunctionText $Text "Invoke-ElevatedRelaunch"
    Assert-Routing ($body.Contains('exit $process.ExitCode')) "Relaunch exit boundary changed"
    . ([scriptblock]::Create($body.Replace('exit $process.ExitCode', 'return $process.ExitCode')))
    $OutputRoot = 'C:\fixture with spaces\'
    $VhdSizeMB = 768
    $KeepVhd = $true
    $ExternalEvidenceManifest = ""
    $ExternalEvidenceChecklist = ""
    $ControllerCertifierPath = 'C:\rig files\helper.exe'
    $ControllerSourceManifestPath = 'C:\rig files\manifest.txt'
    $Script:RelaunchCapture = $null
    function Get-Process { param($Id) return [pscustomobject]@{ Path = "fixture-powershell.exe" } }
    function Start-Process {
        param($FilePath, $ArgumentList, $Verb, $WindowStyle, [switch]$Wait, [switch]$PassThru)
        $Script:RelaunchCapture = @{ args = $ArgumentList; style = $WindowStyle; verb = $Verb }
        return [pscustomobject]@{ ExitCode = 0 }
    }
    Invoke-ElevatedRelaunch | Out-Null
    $args = $Script:RelaunchCapture.args
    foreach ($pair in @(
        @("-ControllerCertifierPath", '"C:\rig files\helper.exe"'),
        @("-ControllerSourceManifestPath", '"C:\rig files\manifest.txt"'),
        @("-OutputRoot", '"C:\fixture with spaces\\"')
    )) {
        $index = [Array]::IndexOf($args, $pair[0])
        Assert-Routing ($index -ge 0 -and $args[$index + 1] -ceq $pair[1]) "Elevated relaunch lost or misquoted $($pair[0])"
    }
    Assert-Routing ($Script:RelaunchCapture.style -ceq "Hidden" -and $Script:RelaunchCapture.verb -ceq "RunAs") "Relaunch must be hidden and retain elevation"
}

function Test-StrictVhdClaimType {
    param([string]$Text)
    $ast = Get-StrictWrapperAst $Text
    $conditions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Extent.Text.Contains('$status.claim_level')
    }, $true))
    $check = [scriptblock]::Create(($conditions | ForEach-Object { $_.Extent.Text }) -join [Environment]::NewLine)
    foreach ($valid in @("VhdDataDiskCertified", "HardwareCertified")) {
        $status = [pscustomobject]@{ claim_level = $valid }
        & $check
    }
    $status = [pscustomobject]@{ claim_level = @("VhdDataDiskCertified") }
    Assert-Throws { & $check } "non-string claim_level"
}

$Script:StrictSteps = Get-Content -LiteralPath (Join-Path $PSScriptRoot "partition_manager_certification_steps.ps1") -Raw
$Script:StrictVhd = Get-Content -LiteralPath (Join-Path $PSScriptRoot "run_partition_manager_vhd_certification_strict.ps1") -Raw
$Script:StrictHardware = Get-Content -LiteralPath (Join-Path $PSScriptRoot "run_partition_manager_hardware_certification_strict.ps1") -Raw
Test-NonGitProducerReturn
if ($RunGuardDrills) { Test-ProvenanceGuardDrills }
if ($DryProducerOnly) { return }
$cases = @("Cleanup", "CleanupIsolation", "Report", "Operations", "DiskBinding", "Launch", "Selector", "IndependentVerification")
foreach ($case in $cases) {
    & ("Test-Routing" + $case) $Script:ProductionText
    Write-Host "PASS $case"
}
Test-StrictStepStatus $Script:StrictSteps
Test-StrictArtifactGuards $Script:StrictSteps
Test-StrictWrapperArtifacts $Script:StrictVhd $Script:StrictSteps "vhd"
Test-StrictWrapperArtifacts $Script:StrictHardware $Script:StrictSteps "hardware"
Test-StrictWrapperThreading $Script:StrictVhd "vhd"
Test-StrictWrapperThreading $Script:StrictHardware "hardware"
Test-StrictVhdRelaunch $Script:StrictVhd
Test-StrictVhdClaimType $Script:StrictVhd
Write-Host "PASS strict wrapper steps, artifact regeneration, path threading, relaunch and claim type"
if ($RunGuardDrills) {
    $unsafeCleanup = @'
function Remove-DisposableVhdDisk {
    param([object]$Vhd)
    try { Dismount-SakDiskImageIfAttached -Path $Vhd.path }
    finally {
        if (-not $KeepVhd -and (Test-Path -LiteralPath $Vhd.path)) {
            Remove-Item -LiteralPath $Vhd.path -Force
        }
    }
}
'@
    $old = Get-RoutingFunctionText $Script:ProductionText "Remove-DisposableVhdDisk"
    $mutant = $Script:ProductionText.Replace($old, $unsafeCleanup)
    Assert-Throws { Test-RoutingCleanup $mutant } "Deleted VHD after failed detach"
    Write-Host "KILLED unsafe-finally-cleanup"
    $drills = @(
        @{ name = "typed-quiescence"; fn = "Assert-ControllerRestoreBoolean"; from = '$Value -is [bool] -and '; to = ''; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "run-identity"; fn = "Assert-ControllerRestoreIdentity"; from = '$Report.run_id -ceq $State.run_id'; to = '$Report.run_id -ceq $Report.run_id'; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "source-path-binding"; fn = "Assert-ControllerRestoreIdentity"; from = 'Test-ControllerRestorePath $Report.source_vhd $State.source.path'; to = 'Test-ControllerRestorePath $State.source.path $State.source.path'; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "target-path-binding"; fn = "Assert-ControllerRestoreIdentity"; from = 'Test-ControllerRestorePath $Report.target_vhd $State.target.path'; to = 'Test-ControllerRestorePath $State.target.path $State.target.path'; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "binary-hash-binding"; fn = "Assert-ControllerRestoreIdentity"; from = '$Report.certifier_binary_sha256 -ieq $State.certifier.binary_hash'; to = '$Report.certifier_binary_sha256 -ieq $Report.certifier_binary_sha256'; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "raw-manifest-binding"; fn = "Assert-ControllerRestoreIdentity"; from = '$Report.source_digest -ieq $State.certifier.source_digest'; to = '$Report.source_digest -ieq $Report.source_digest'; test = "Test-RoutingReport"; error = "Expected refusal" },
        @{ name = "fixture-boot-refusal"; fn = "Get-ControllerFixtureDisk"; from = '$disk[0].IsBoot $false'; to = '$disk[0].IsBoot $disk[0].IsBoot'; test = "Test-RoutingDiskBinding"; error = "Expected refusal" },
        @{ name = "fixture-system-refusal"; fn = "Get-ControllerFixtureDisk"; from = '$disk[0].IsSystem $false'; to = '$disk[0].IsSystem $disk[0].IsSystem'; test = "Test-RoutingDiskBinding"; error = "Expected refusal" },
        @{ name = "fixture-size-binding"; fn = "Get-ControllerFixtureDisk"; from = '([uint64]$Vhd.size_mb * 1MB)'; to = '[uint64]$disk[0].Size'; test = "Test-RoutingDiskBinding"; error = "Expected refusal" },
        @{ name = "fixture-single-binding"; fn = "Get-ControllerFixtureDisk"; from = '$disk.Count -eq 1'; to = '$disk.Count -ge 1'; test = "Test-RoutingDiskBinding"; error = "Expected refusal" },
        @{ name = "launch-preservation"; fn = "Invoke-ControllerRestore"; from = '$State.cleanup_allowed = $false'; to = '$State.cleanup_allowed = $true'; test = "Test-RoutingLaunch"; error = "Launch uncertainty" },
        @{ name = "uncertain-cleanup"; fn = "Complete-ControllerRestoreCleanup"; from = 'if (-not $State.cleanup_allowed)'; to = 'if ($State.cleanup_allowed -and -not $State.cleanup_allowed)'; test = "Test-RoutingCleanup"; error = "Unconfirmed worker" },
        @{ name = "restore-dry-run"; fn = "Assert-ControllerRestoreOperations"; from = 'Assert-ControllerRestoreBoolean $operation.execution.dry_run $false "execution.dry_run"'; to = 'Assert-ControllerRestoreBoolean $operation.execution.dry_run $operation.execution.dry_run "execution.dry_run"'; test = "Test-RoutingOperations"; error = "Expected refusal" },
        @{ name = "focused-selection"; fn = "Invoke-VhdScenario"; from = '$Id -cne $VhdScenarioId'; to = '$Id -ceq $VhdScenarioId'; test = "Test-RoutingSelector"; error = "nonselected scenario" },
        @{ name = "independent-overwrite"; fn = "Test-ControllerRestoredTarget"; from = '(-not $markerPresent)'; to = '($markerPresent -eq $markerPresent)'; test = "Test-RoutingIndependentVerification"; error = "Expected refusal" }
    )
    foreach ($drill in $drills) {
        $original = Get-RoutingFunctionText $Script:ProductionText $drill.fn
        Assert-Routing ($original.Contains($drill.from)) "Mutation anchor absent: $($drill.name)"
        $mutantText = $Script:ProductionText.Replace($original, $original.Replace($drill.from, $drill.to))
        Assert-Throws { & $drill.test $mutantText } $drill.error
        Write-Host "KILLED $($drill.name)"
    }
    $stepDrills = @(
        @{ name = "powershell-error-record"; from = 'if ($_ -is [System.Management.Automation.ErrorRecord])'; to = 'if ($_ -is [System.Management.Automation.ErrorRecord] -and $_ -is [string])'; test = "Test-StrictStepStatus" },
        @{ name = "native-exit-status"; from = '$exitCode -ne 0'; to = '$exitCode -ne $exitCode'; test = "Test-StrictStepStatus" },
        @{ name = "clear-before-produce"; from = 'foreach ($path in $Paths) { Clear-GeneratedArtifact -Path $path }'; to = 'foreach ($path in $Paths) { if (-not (Test-Path -LiteralPath $path)) { Clear-GeneratedArtifact -Path $path } }'; test = "Test-StrictArtifactGuards" },
        @{ name = "require-regenerated-file"; from = 'if (-not (Test-Path -LiteralPath $Path -PathType Leaf))'; to = 'if ((Test-Path -LiteralPath $Path -PathType Leaf) -and -not (Test-Path -LiteralPath $Path -PathType Leaf))'; test = "Test-StrictArtifactGuards" }
    )
    foreach ($drill in $stepDrills) {
        Assert-Routing ($Script:StrictSteps.Contains($drill.from)) "Shared step mutation anchor missing"
        $mutant = $Script:StrictSteps.Replace($drill.from, $drill.to)
        Assert-Throws { & $drill.test $mutant } "Expected refusal"
        Write-Host "KILLED $($drill.name)"
    }
    foreach ($kind in @("vhd", "hardware")) {
        $text = if ($kind -eq "vhd") { $Script:StrictVhd } else { $Script:StrictHardware }
        $ast = Get-StrictWrapperAst $text
        $producers = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq "Invoke-GeneratedArtifactStep"
        }, $true))
        foreach ($producer in $producers) {
            $broken = $producer.Extent.Text.Replace("Invoke-GeneratedArtifactStep", "Invoke-CheckedStep")
            $broken = [regex]::Replace($broken, '\s+-Paths\s+@\([^)]*\)', '')
            $mutant = $text.Replace($producer.Extent.Text, $broken)
            Assert-Throws { Test-StrictWrapperArtifacts $mutant $Script:StrictSteps $kind } "Expected refusal"
            Write-Host "KILLED $kind artifact route $($producer.CommandElements[4].Extent.Text)"
        }
        foreach ($command in @("verify_partition_manager_certification.ps1", "get_partition_manager_certification_status.ps1")) {
            $calls = @($ast.FindAll({ param($node)
                $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq ("scripts\" + $command)
            }, $true))
            Assert-Routing ($calls.Count -eq 1) "Consumer mutation call missing"
            foreach ($name in @("ControllerCertifierPath", "ControllerSourceManifestPath")) {
                $anchor = "-$name " + '$' + $name
                $mutant = $text.Replace($calls[0].Extent.Text, $calls[0].Extent.Text.Replace($anchor, ""))
                Assert-Throws { Test-StrictWrapperThreading $mutant $kind } "does not forward"
                Write-Host "KILLED $kind $command $name forwarding"
            }
        }
    }
    $producerMutation = $Script:StrictVhd.Replace('        ControllerCertifierPath = $ControllerCertifierPath', '')
    Assert-Throws { Test-StrictWrapperThreading $producerMutation "vhd" } "selected helper to producer"
    Write-Host "KILLED strict producer helper forwarding"
    foreach ($name in @("ControllerCertifierPath", "ControllerSourceManifestPath")) {
        $anchor = '    $arguments += @("-' + $name + '", (ConvertTo-WindowsProcessArgument $' + $name + '))'
        Assert-Routing ($Script:StrictVhd.Contains($anchor)) "Relaunch path mutation anchor missing"
        $mutant = $Script:StrictVhd.Replace($anchor, "")
        Assert-Throws { Test-StrictVhdRelaunch $mutant } "lost or misquoted"
        Write-Host "KILLED elevated $name forwarding"
    }
    $quoteMutation = $Script:StrictVhd.Replace('(ConvertTo-WindowsProcessArgument -Argument $OutputRoot)', '(''\"'' + $OutputRoot + ''\"'')')
    Assert-Throws { Test-StrictVhdRelaunch $quoteMutation } "lost or misquoted"
    Write-Host "KILLED elevated trailing-slash quoting"
    $claimMutation = $Script:StrictVhd.Replace('$status.claim_level -isnot [string]', '$status.claim_level -isnot [string] -and $status.claim_level -is [string]')
    Assert-Throws { Test-StrictVhdClaimType $claimMutation } "Expected refusal"
    Write-Host "KILLED array-valued strict VHD claim"
}
Write-Host "Controller restore routing: $($cases.Count + 9) groups passed. Only metadata-only producer mode ran; no storage mutation."
