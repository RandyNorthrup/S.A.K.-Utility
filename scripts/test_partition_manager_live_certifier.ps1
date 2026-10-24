<#
.SYNOPSIS
    Non-destructive contract tests for the controller-driven VHD certifier.

.DESCRIPTION
    Runs pure admission checks and rejected CLI requests. The only files created are
    ordinary text fixtures in a fresh artifact directory; no VHD is created or attached,
    and no controller operation, raw-disk write, or network reconfiguration is requested.
    A validate-only request against two deliberately invalid VHD files must fail closed.
#>

[CmdletBinding()]
param(
    [string]$CertifierPath = "",
    [string]$OutputRoot = "artifacts\partition-manager-certification\tool-tests\controller-live"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
. (Join-Path $PSScriptRoot "process_capture.ps1")

function Assert-Contract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Certifier {
    param([string[]]$Arguments)
    $options = @{
        FilePath = $Script:Certifier
        Arguments = $Arguments
        WorkingDirectory = $Script:CaseRoot
        TimeoutSeconds = 20
    }
    return Invoke-CapturedProcess @options
}

function Read-JsonOutput {
    param([string]$Text)
    Assert-Contract (-not [string]::IsNullOrWhiteSpace($Text)) "Certifier returned no JSON"
    $value = $Text | ConvertFrom-Json
    Assert-Contract ($value -is [pscustomobject]) "Certifier JSON must be an object"
    return $value
}

function Assert-Text {
    param($Actual, [string]$Expected, [string]$Name)
    Assert-Contract ($Actual -is [string] -and $Actual -ceq $Expected) "$Name is not '$Expected'"
}

function Assert-Boolean {
    param($Actual, [bool]$Expected, [string]$Name)
    $matches = $Actual -is [bool] -and $Actual -eq $Expected
    Assert-Contract $matches "$Name is not a JSON boolean $Expected"
}

function Test-PureAdmissionContract {
    $result = Invoke-Certifier -Arguments @("--self-test")
    Assert-Contract (-not $result.timed_out -and $result.exit_code -eq 0) `
        "Pure guard self-test failed: $($result.stdout) $($result.stderr)"
    $report = Read-JsonOutput $result.stdout
    Assert-Text $report.tool "partition-manager-controller-live-certifier" "self-test tool"
    Assert-Text $report.status "Passed" "self-test status"
    Assert-Boolean $report.self_test $true "self_test"
    Assert-Boolean $report.storage_accessed $false "storage_accessed"
    $expected = @(
        "literal-config-positive", "confirmation-required", "validation-only-is-nondestructive",
        "raw-target-cli-refused", "aliased-paths-refused", "missing-run-identity-refused",
        "traversal-refused", "outside-fixture-root-refused", "disk-facts-positive",
        "boot-disk-refused", "system-disk-refused", "missing-disk-identity-refused",
        "large-disk-refused", "attachment-disk-mismatch-refused", "vhd-type-positive",
        "vhdx-type-positive", "iso-type-refused", "unknown-type-refused",
        "permanent-attachment-access", "attachment-backing-store-read-only"
    )
    foreach ($mode in @("restore", "offline-restore", "offline-readonly-restore",
                         "offline-partition-image", "busy-source-refusal")) {
        $expected += @("proof-mode-roundtrip-$mode", "proof-report-isolation-$mode",
                       "proof-validation-only-$mode", "offline-workflow-$mode", "restore-workflow-$mode")
    }
    $expected += @(
        "unknown-proof-mode-refused", "empty-proof-mode-refused",
        "busy-proof-status-isolated", "busy-owned-open-refusal-positive",
        "busy-owned-lock-refusal-positive", "busy-success-refused", "busy-dry-run-refused",
        "busy-cancelled-refused", "busy-timeout-refused", "busy-no-step-refused", "busy-multiple-steps-refused",
        "busy-skipped-step-refused", "busy-passed-step-refused", "busy-other-operation-refused",
        "busy-other-error-refused", "busy-verification-witness-refused",
        "busy-sample-verification-witness-refused", "busy-restore-operation-refused"
    )
    $expected += @(
        "image-range-whole-positive", "image-range-empty-refused", "image-range-offset-overflow-refused",
        "image-range-end-overflow-refused", "partition-image-scope-pinned", "whole-image-scope-pinned",
        "accepted-request-positive", "accepted-request-mode-drift-refused", "accepted-request-kind-drift-refused",
        "accepted-request-offset-drift-refused", "accepted-request-size-drift-refused",
        "accepted-request-disk-drift-refused", "accepted-request-partition-drift-refused",
        "accepted-request-guid-drift-refused",
        "exact-range-hash-positive", "exact-range-short-read-refused"
    )
    $expected += @(
        "worker-proof-identity-positive", "worker-proof-missing-schema_version-refused",
        "worker-proof-missing-scenario_id-refused", "worker-proof-missing-proof_mode-refused",
        "worker-proof-missing-validation_only-refused"
    )
    $expected += @(
        "offline-fixture-scope-positive", "offline-fixture-unknown-partition-refused",
        "offline-fixture-unknown-type-refused", "offline-fixture-msr-volume-refused",
        "offline-fixture-missing-source-refused", "offline-fixture-duplicate-source-refused"
    )
    $expected += @("offline-fixture-inventory-msr-positive", "offline-fixture-msr-lookup-failure-refused",
                   "offline-fixture-msr-lookup-error-refused")
    foreach ($field in @('guid', 'letter', 'label', 'filesystem', 'source', 'details', 'health',
                         'total', 'free', 'bitlocker-enabled', 'bitlocker-locked', 'dirty-known',
                         'dirty-set', 'dirty-error', 'bitlocker-known', 'bitlocker-error')) {
        $expected += "offline-fixture-advisory-$field-refused"
    }
    foreach ($cancelled in @(0, 1)) {
        foreach ($timedOut in @(0, 1)) {
            $expected += @("execution-cancelled-boolean-$cancelled-$timedOut",
                           "execution-timed-out-boolean-$cancelled-$timedOut")
        }
    }
    $checks = @($report.checks)
    Assert-Contract ($checks.Count -eq $expected.Count) "Pure guard check count changed"
    foreach ($name in $expected) {
        $matches = @($checks | Where-Object { $_.name -ceq $name })
        Assert-Contract ($matches.Count -eq 1) "Missing or duplicate guard check: $name"
        Assert-Boolean $matches[0].passed $true $name
    }
}

function Get-BaseArguments {
    return @(
        "--source-vhd", $Script:SourcePath,
        "--target-vhd", $Script:TargetPath,
        "--output-dir", $Script:CaseRoot,
        "--run-id", $Script:RunId
    )
}

function Assert-Rejected {
    param([string]$Name, [string[]]$Arguments, [string]$ExpectedError)
    $result = Invoke-Certifier -Arguments $Arguments
    Assert-Contract (-not $result.timed_out) "$Name timed out instead of rejecting"
    Assert-Contract ($result.exit_code -ne 0) "$Name unexpectedly succeeded"
    $output = $result.stdout + [Environment]::NewLine + $result.stderr
    Assert-Contract ($output.Contains($ExpectedError)) "$Name did not report its expected guard"
    $imageExists = Test-Path -LiteralPath (Join-Path $Script:CaseRoot "controller-source.img")
    Assert-Contract (-not $imageExists) "$Name created a raw image"
    $reportExists = Test-Path -LiteralPath (
        Join-Path $Script:CaseRoot "controller-restore-report.json")
    Assert-Contract (-not $reportExists) "$Name advanced to evidence publication"
    $supplementExists = Test-Path -LiteralPath (
        Join-Path $Script:CaseRoot "controller-image-proof-report.json")
    Assert-Contract (-not $supplementExists) "$Name published supplementary evidence"
}

function Test-RejectedCliContracts {
    $baseArguments = Get-BaseArguments
    Assert-Rejected "missing confirmation" $baseArguments "--confirm-disposable-vhds"
    Assert-Rejected "guessed disk number" @("--disk-number", "0") "Unknown option"
    $duplicate = $baseArguments + @("--source-vhd", $Script:SourcePath)
    Assert-Rejected "duplicate option" $duplicate "Duplicate option"
    Assert-Rejected "unknown proof mode" ($baseArguments + @("--proof-mode", "legacy")) "Unknown --proof-mode"
    Assert-Rejected "empty proof mode" ($baseArguments + @("--proof-mode=")) "Unknown --proof-mode"
    Assert-Rejected "duplicate proof mode" ($baseArguments + @(
        "--proof-mode", "restore", "--proof-mode", "offline-restore")) "Duplicate option"
    Assert-Rejected "unvalued proof mode" ($baseArguments + @("--proof-mode")) "Missing value"
    $rawArguments = @(
        "--source-vhd", $Script:SourcePath, "--target-vhd", "\\.\PhysicalDrive0",
        "--output-dir", $Script:CaseRoot, "--run-id", $Script:RunId,
        "--confirm-disposable-vhds"
    )
    Assert-Rejected "raw disk path" $rawArguments "Every path must be literal"
    $sameArguments = @(
        "--source-vhd", $Script:SourcePath, "--target-vhd", $Script:SourcePath.ToUpperInvariant(),
        "--output-dir", $Script:CaseRoot, "--run-id", $Script:RunId,
        "--confirm-disposable-vhds"
    )
    Assert-Rejected "same source and target" $sameArguments "must be distinct"
    $uncontained = $baseArguments + @(
        "--confirm-disposable-vhds", "--contained-worker", "--containment-job",
        "Local\SAK-Partition-Cert-$($Script:RunId)-absent"
    )
    Assert-Rejected "uncontained direct worker" $uncontained "Worker is not inside"
}

function Test-InvalidVirtualDisks {
    param([string]$Mode = "restore", [string]$Scenario = "vhd.image-restore", [int]$Schema = 1)
    $arguments = (Get-BaseArguments) + @("--validate-only")
    if ($Mode -cne "restore") { $arguments += @("--proof-mode", $Mode) }
    $result = Invoke-Certifier -Arguments $arguments
    Assert-Contract (-not $result.timed_out -and $result.exit_code -ne 0) "Invalid VHDs succeeded"
    $report = Read-JsonOutput $result.stdout
    Assert-Text $report.status "Failed" "invalid VHD status"
    Assert-Boolean $report.validation_only $true "validation_only"
    Assert-Boolean $report.controller_executed $false "controller_executed"
    Assert-Boolean $report.controller_attempted $false "controller_attempted"
    Assert-Boolean $report.source_preparation_started $false "source_preparation_started"
    Assert-Boolean $report.mutations_may_have_started $false "mutations_may_have_started"
    Assert-Text $report.proof_mode $Mode "proof_mode"
    Assert-Text $report.scenario_id $Scenario "scenario_id"
    Assert-Contract ($report.schema_version -eq $Schema) "Wrong proof schema"
    Assert-Boolean $report.worker_descendants_finished $true "worker_descendants_finished"
    Assert-Contract ($report.operations -is [System.Array]) "Operations must be a JSON array"
    Assert-Contract (@($report.operations).Count -eq 0) "Invalid VHD request executed an operation"
    $sourceHashValid = $report.source_digest -is [string] -and
        $report.source_digest -match '^[0-9a-fA-F]{64}$'
    Assert-Contract $sourceHashValid "Build-bound source digest is missing"
    $binaryDirectory = Split-Path -Parent $Script:Certifier
    $sidecar = Join-Path $binaryDirectory "partition_manager_live_certifier.sources.txt"
    Assert-Contract (Test-Path -LiteralPath $sidecar -PathType Leaf) "Source sidecar is missing"
    $sidecarHash = (Get-FileHash -LiteralPath $sidecar -Algorithm SHA256).Hash
    Assert-Contract ($sidecarHash -ieq $report.source_digest) "Raw source sidecar hash mismatch"
    $binaryHashValid = $report.certifier_binary_sha256 -is [string] -and
        $report.certifier_binary_sha256 -match '^[0-9a-fA-F]{64}$'
    Assert-Contract $binaryHashValid "Executable digest is missing"
    $imageExists = Test-Path -LiteralPath (Join-Path $Script:CaseRoot "controller-source.img")
    Assert-Contract (-not $imageExists) "Validate-only request created an image"
    $expectedFile = if ($Mode -ceq "restore") { "controller-restore-report.json" } else {
        "controller-image-proof-report.json"
    }
    Assert-Contract (Test-Path -LiteralPath (Join-Path $Script:CaseRoot $expectedFile) -PathType Leaf) `
        "Mode report was not durably published to its isolated filename"
    foreach ($forbidden in @("source_attributes_before_preparation", "independent_source_lock_held",
                             "source_image", "source_range_verified")) {
        Assert-Contract ($report.PSObject.Properties.Name -cnotcontains $forbidden) `
            "Validate-only mode advanced to $forbidden"
    }
}

function Test-SupplementaryValidationOnly {
    $originalRoot = $Script:CaseRoot
    $originalSource = $Script:SourcePath
    $originalTarget = $Script:TargetPath
    $modes = @(
        @{ Mode = "offline-restore"; Scenario = "vhd.image-restore-offline" },
        @{ Mode = "offline-readonly-restore"; Scenario = "vhd.image-restore-offline-readonly" },
        @{ Mode = "offline-partition-image"; Scenario = "vhd.partition-image-create-offline" },
        @{ Mode = "busy-source-refusal"; Scenario = "vhd.image-create-busy-refusal" }
    )
    try {
        foreach ($mode in $modes) {
            $Script:CaseRoot = Join-Path $originalRoot $mode.Mode
            New-Item -ItemType Directory -Path $Script:CaseRoot -ErrorAction Stop | Out-Null
            $Script:SourcePath = Join-Path $Script:CaseRoot "source.vhdx"
            $Script:TargetPath = Join-Path $Script:CaseRoot "target.vhdx"
            Copy-Item -LiteralPath $originalSource -Destination $Script:SourcePath -ErrorAction Stop
            Copy-Item -LiteralPath $originalTarget -Destination $Script:TargetPath -ErrorAction Stop
            Test-InvalidVirtualDisks -Mode $mode.Mode -Scenario $mode.Scenario -Schema 2
            Assert-Contract ((Get-FileHash -LiteralPath $Script:SourcePath -Algorithm SHA256).Hash -ceq
                (Get-FileHash -LiteralPath $originalSource -Algorithm SHA256).Hash) `
                "Supplementary validate-only changed its ordinary source file"
            Assert-Contract ((Get-FileHash -LiteralPath $Script:TargetPath -Algorithm SHA256).Hash -ceq
                (Get-FileHash -LiteralPath $originalTarget -Algorithm SHA256).Hash) `
                "Supplementary validate-only changed its ordinary target file"
            Assert-Contract (-not (Test-Path -LiteralPath (
                Join-Path $Script:CaseRoot "controller-restore-report.json"))) `
                "Supplementary mode published a standard restore report"
        }
    } finally {
        $Script:CaseRoot = $originalRoot
        $Script:SourcePath = $originalSource
        $Script:TargetPath = $originalTarget
    }
}

if ([string]::IsNullOrWhiteSpace($CertifierPath)) {
    $CertifierPath = Join-Path $ProjectRoot "build\Release\partition_manager_live_certifier.exe"
}
$Script:Certifier = (Resolve-Path -LiteralPath $CertifierPath -ErrorAction Stop).Path
if (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $ProjectRoot $OutputRoot
}
# Bounded retention: a passing run removes its own case folder and each run first caps the
# folders earlier failed runs left behind. See scripts/run_folder_retention.ps1.
. (Join-Path $PSScriptRoot "run_folder_retention.ps1")
$runFolderPattern = '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'
$null = Limit-RetainedTestRunFolders -OutputRoot $OutputRoot -NamePattern $runFolderPattern
$Script:RunId = [guid]::NewGuid().ToString("D")
$Script:CaseRoot = Join-Path ([System.IO.Path]::GetFullPath($OutputRoot)) $Script:RunId
New-Item -ItemType Directory -Path $Script:CaseRoot -ErrorAction Stop | Out-Null
$Script:SourcePath = Join-Path $Script:CaseRoot "source.vhdx"
$Script:TargetPath = Join-Path $Script:CaseRoot "target.vhdx"
Set-Content -LiteralPath $Script:SourcePath -Value "Ordinary file, not a VHD." -Encoding ASCII
Set-Content -LiteralPath $Script:TargetPath -Value "Another ordinary file, not a VHD." -Encoding ASCII
$sourceHash = (Get-FileHash -LiteralPath $Script:SourcePath -Algorithm SHA256).Hash
$targetHash = (Get-FileHash -LiteralPath $Script:TargetPath -Algorithm SHA256).Hash
Test-PureAdmissionContract
Test-RejectedCliContracts
Test-InvalidVirtualDisks
Test-SupplementaryValidationOnly
$sourceUnchanged = (Get-FileHash -LiteralPath $Script:SourcePath -Algorithm SHA256).Hash -ceq $sourceHash
$targetUnchanged = (Get-FileHash -LiteralPath $Script:TargetPath -Algorithm SHA256).Hash -ceq $targetHash
Assert-Contract $sourceUnchanged "Source ordinary file changed during rejected requests"
Assert-Contract $targetUnchanged "Target ordinary file changed during rejected requests"
Remove-TestRunFolder -RunFolder $Script:CaseRoot -OutputRoot $OutputRoot -NamePattern $runFolderPattern
Write-Output "Partition controller live-certifier non-destructive contracts passed."
