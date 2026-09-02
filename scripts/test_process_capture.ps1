<#
.SYNOPSIS
    Self-tests scripts/process_capture.ps1 under the current PowerShell host.
#>

param(
    [ValidateSet("all", "file-management", "allocate-free-space", "bitlocker",
        "os-migration-vbox", "os-migration-screenshot", "linux-metadata", "vm-data",
        "file-recovery", "ext-filesystem-vm", "offline-bios-bcd")]
    [string]$MigrationCase = "all"
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "process_capture.ps1")

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-CaptureMigration {
    param(
        [string]$Name,
        [string]$RelativePath,
        [string[]]$RequiredPatterns,
        [string[]]$LegacyPatterns
    )

    $path = Join-Path (Split-Path -Parent $PSScriptRoot) $RelativePath
    $text = Get-Content -LiteralPath $path -Raw -ErrorAction Stop
    foreach ($pattern in $RequiredPatterns) {
        Assert-True $text.Contains($pattern) "$Name does not use shared capture contract: $pattern"
    }
    foreach ($pattern in $LegacyPatterns) {
        Assert-True (-not $text.Contains($pattern)) "$Name retained legacy capture: $pattern"
    }
}

$shell = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName

$captured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-Command",
    "[Console]::Out.Write('stdout marker'); " +
        "[Console]::Error.Write('stderr marker'); exit 7"
)
Assert-Equal $captured.exit_code 7 "Non-zero child exit code was not captured"
Assert-Equal $captured.stdout "stdout marker" "Standard output was not captured"
Assert-Equal $captured.stderr "stderr marker" "Standard error was not captured"
Assert-True (-not $captured.timed_out) "Completed child was reported as timed out"

$stdinCaptured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command", "[Console]::In.ReadToEnd()"
) -InputText "stdin marker" -AllowedExitCodes @(0)
Assert-Equal $stdinCaptured.stdout "stdin marker" "Standard input was not delivered"

$pipePayloadSize = 131072
$pipeInput = 'i' * $pipePayloadSize
$duplexCaptured = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command",
    "[Console]::Out.Write(('o' * $pipePayloadSize)); " +
        "[Console]::Out.Flush(); " +
        "[Console]::Out.Write([Console]::In.ReadToEnd())"
) -InputText $pipeInput -TimeoutSeconds 10 -AllowedExitCodes @(0)
Assert-True (-not $duplexCaptured.timed_out) "Duplex output/stdin child deadlocked"
Assert-Equal $duplexCaptured.stdout.Length ($pipePayloadSize * 2) (
    "Duplex output/stdin payload was truncated"
)

$allowedRejected = $false
try {
    Invoke-CapturedProcess -FilePath $shell -Arguments @(
        "-NoProfile", "-NonInteractive", "-Command", "exit 7"
    ) -AllowedExitCodes @(0) | Out-Null
} catch {
    $allowedRejected = $_.Exception.Message -match 'Command failed \(7\)'
}
Assert-True $allowedRejected "Disallowed exit code did not fail closed"

$largeOutput = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile",
    "-NonInteractive",
    "-Command",
    "[Console]::Out.Write(('O' * 200000)); " +
        "[Console]::Error.Write(('E' * 200000)); exit 0"
) -AllowedExitCodes @(0)
Assert-Equal $largeOutput.stdout.Length 200000 "Large standard output was truncated"
Assert-Equal $largeOutput.stderr.Length 200000 "Large standard error was truncated"

$timed = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 5"
) -TimeoutSeconds 1
Assert-True $timed.timed_out "Timeout did not terminate and flag the child"

$escapedShell = $shell.Replace("'", "''")
$treeCommand = @"
`$child = Start-Process -FilePath '$escapedShell' -ArgumentList @(
    '-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Seconds 30'
) -PassThru -WindowStyle Hidden
Write-Output `$child.Id
Start-Sleep -Seconds 30
"@
$treeTimed = Invoke-CapturedProcess -FilePath $shell -Arguments @(
    "-NoProfile", "-NonInteractive", "-Command", $treeCommand
) -TimeoutSeconds 1
Assert-True $treeTimed.timed_out "Process-tree timeout was not reported"
$descendantPid = [int]$treeTimed.stdout
$descendantAlive = $true
try {
    [void][System.Diagnostics.Process]::GetProcessById($descendantPid)
}
catch [System.ArgumentException] {
    $descendantAlive = $false
}
Assert-True (-not $descendantAlive) "Timed-out descendant process remained alive"

$migrationCases = @(
    @{
        name = "file-management"
        path = "scripts/run_file_management_live_filesystem_certification.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            'Invoke-CapturedProcess -FilePath $certifier -Arguments $certifierArgs',
            '$certifierProcess.exit_code',
            '$certifierProcess.stdout',
            '$certifierProcess.stderr'
        )
        legacy = @(
            '$certifierOutput = & $certifier @certifierArgs 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "allocate-free-space"
        path = "scripts/run_partition_manager_allocate_free_space_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            'Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$global:LASTEXITCODE = $null',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "bitlocker"
        path = "scripts/run_partition_manager_bitlocker_mutation_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$raw = Invoke-NativeCommandRaw',
            'ConvertTo-SanitizedCommandLog -Command $raw'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "os-migration-vbox"
        path = "scripts/run_partition_manager_os_migration_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $VBoxManage -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $VBoxManage @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "os-migration-screenshot"
        path = "scripts/run_partition_manager_os_migration_external_gate.ps1"
        required = @(
            '$screenshotProcess = Invoke-CapturedProcess -FilePath $VBoxManage -Arguments @(',
            '$screenshotProcess.exit_code',
            '$screenshotProcess.stdout',
            '$screenshotProcess.stderr'
        )
        legacy = @(
            '$screenshotOutput = & $VBoxManage controlvm',
            '$ErrorActionPreference = "Continue"',
            'SilentlyContinue'
        )
    },
    @{
        name = "linux-metadata"
        path = "scripts/run_partition_manager_linux_metadata_validation.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "vm-data"
        path = "scripts/run_partition_manager_vm_data_external_gates.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "file-recovery"
        path = "scripts/run_partition_manager_file_recovery_external_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr',
            '$FsutilExe = Join-Path ([System.Environment]::SystemDirectory) "fsutil.exe"',
            '-FilePath $FsutilExe',
            'Failed to disable NTFS DisableDeleteNotify with modern fsutil syntax',
            'Failed to restore NTFS DisableDeleteNotify with modern fsutil syntax'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue',
            '$disableDeleteNotifyLegacy',
            '$restoreDeleteNotifyLegacy',
            '@("behavior", "set", "DisableDeleteNotify", "1")',
            '@("behavior", "set", "DisableDeleteNotify", "0")',
            "Do not trust either set's exit code"
        )
    },
    @{
        name = "ext-filesystem-vm"
        path = "scripts/run_partition_manager_ext_filesystem_vm_gate.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    },
    @{
        name = "offline-bios-bcd"
        path = "scripts/run_partition_manager_offline_bios_bcd_break.ps1"
        required = @(
            '. (Join-Path $PSScriptRoot "process_capture.ps1")',
            '$captured = Invoke-CapturedProcess -FilePath $FilePath -Arguments $Arguments',
            '$captured.exit_code',
            '$captured.stdout',
            '$captured.stderr'
        )
        legacy = @(
            '$output = & $FilePath @Arguments 2>&1',
            '$ErrorActionPreference = "Continue"',
            '$LASTEXITCODE',
            'SilentlyContinue'
        )
    }
)
foreach ($case in $migrationCases) {
    if ($MigrationCase -eq "all" -or $MigrationCase -eq $case.name) {
        Assert-CaptureMigration -Name $case.name -RelativePath $case.path `
            -RequiredPatterns $case.required -LegacyPatterns $case.legacy
    }
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "bitlocker") {
    Assert-CaptureMigration -Name "bitlocker-launcher" `
        -RelativePath "scripts/launch_partition_manager_bitlocker_mutation_external_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_bitlocker_mutation_external_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "vm-data") {
    Assert-CaptureMigration -Name "vm-data-launcher" `
        -RelativePath "scripts/launch_partition_manager_vm_data_external_gates_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_vm_data_external_gates.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "file-recovery") {
    Assert-CaptureMigration -Name "file-recovery-launcher" `
        -RelativePath "scripts/launch_partition_manager_file_recovery_external_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_file_recovery_external_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1',
            '$stagedStorageHash = Copy-VerifiedStagedFile',
            '$stagedCaptureHash = Copy-VerifiedStagedFile'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "ext-filesystem-vm") {
    Assert-CaptureMigration -Name "ext-filesystem-vm-launcher" `
        -RelativePath "scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_ext_filesystem_vm_gate.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}
if ($MigrationCase -eq "all" -or $MigrationCase -eq "offline-bios-bcd") {
    Assert-CaptureMigration -Name "offline-bios-bcd-launcher" `
        -RelativePath "scripts/launch_partition_manager_offline_bios_bcd_break_local.ps1" `
        -RequiredPatterns @(
            'run_partition_manager_offline_bios_bcd_break.ps1',
            'partition_manager_storage.ps1',
            'process_capture.ps1'
        ) -LegacyPatterns @()
}

Write-Host "Process capture tests passed under $($PSVersionTable.PSVersion); migration=$MigrationCase."
