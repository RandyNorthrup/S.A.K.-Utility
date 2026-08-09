<#
.SYNOPSIS
    Stages and launches the file-recovery external gate from inside the VM.

.DESCRIPTION
    Run this from the non-elevated VM desktop. It copies the gate runner,
    certifier, and runtime DLLs from the VirtualBox shared repo to a local
    staging directory, runs the VM-side gate script elevated, then copies the
    resulting evidence back into the shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-file-recovery-gate",
    [string]$TargetDiskNumber = "",
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-file-recovery-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\file-recovery-stage-debug"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-file-recovery-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_file_recovery_external_gate.ps1"
$localCertifier = Join-Path $StageRoot "partition_file_recovery_certifier.exe"

function Assert-LocalStagePath {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [AllowNull()]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "StageRoot must be an explicit local staging path."
    }
    if ($Path -match '["\x00-\x1f]') {
        throw "StageRoot contains characters that cannot cross the elevation boundary: $Path"
    }
    if ($Path -notmatch '^[A-Za-z]:\\') {
        throw "StageRoot must be a drive-qualified local path: $Path"
    }
}

# The staged runner and certifier are executed elevated, so the staging directory
# must not remain writable by every local user between the copy and the UAC prompt.
function New-RestrictedStageDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    Assert-LocalStagePath -Path $Path
    $directory = New-Item -ItemType Directory -Path $Path -Force
    if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Staging directory is a reparse point: $($directory.FullName)"
    }

    $acl = Get-Acl -LiteralPath $directory.FullName
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($rule in @($acl.Access)) {
        if (-not $rule.IsInherited) {
            [void]$acl.RemoveAccessRule($rule)
        }
    }
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
    $trustees = @(
        [System.Security.Principal.SecurityIdentifier]::new("S-1-5-18"),
        [System.Security.Principal.SecurityIdentifier]::new("S-1-5-32-544"),
        ([Security.Principal.WindowsIdentity]::GetCurrent()).User
    )
    foreach ($trustee in $trustees) {
        $acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new(
                $trustee,
                [System.Security.AccessControl.FileSystemRights]::FullControl,
                $inheritance,
                [System.Security.AccessControl.PropagationFlags]::None,
                [System.Security.AccessControl.AccessControlType]::Allow))
    }
    Set-Acl -LiteralPath $directory.FullName -AclObject $acl
    return $directory.FullName
}

function Copy-VerifiedStagedFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,
        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        throw "Missing shared runtime file: $SourcePath"
    }
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    $sourceHash = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash
    $stagedHash = (Get-FileHash -LiteralPath $DestinationPath -Algorithm SHA256).Hash
    if ($sourceHash -ne $stagedHash) {
        throw "Staged copy does not match its source: $DestinationPath"
    }
    return $stagedHash
}

function Assert-StagedFileUnchanged {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedHash
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Staged file disappeared before elevation: $Path"
    }
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($hash -ne $ExpectedHash) {
        throw "Staged file changed after staging; refusing to run it elevated: $Path"
    }
}

# Returns $true only when both the evidence directory content and the guest report
# were published, so a caller can never treat an empty copy-back as proof.
function Copy-StageEvidenceBack {
    $copiedEvidence = $false
    if (Test-Path -LiteralPath $localEvidence -PathType Container) {
        $entries = @(Get-ChildItem -LiteralPath $localEvidence -Force)
        if ($entries.Count -gt 0) {
            New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null
            foreach ($entry in $entries) {
                Copy-Item -LiteralPath $entry.FullName -Destination $sharedEvidence -Recurse -Force
            }
            $copiedEvidence = $true
        }
    }

    $copiedGuestReport = $false
    if (Test-Path -LiteralPath $localGuestReport -PathType Leaf) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $sharedGuestReport) -Force | Out-Null
        Copy-Item -LiteralPath $localGuestReport -Destination $sharedGuestReport -Force
        $copiedGuestReport = $true
    }

    if (Test-Path -LiteralPath $StageRoot -PathType Container) {
        New-Item -ItemType Directory -Path $sharedStageDebug -Force | Out-Null
        foreach ($entry in @(Get-ChildItem -LiteralPath $StageRoot -Force)) {
            Copy-Item -LiteralPath $entry.FullName -Destination $sharedStageDebug -Recurse -Force
        }
    }

    return ($copiedEvidence -and $copiedGuestReport)
}

Assert-LocalStagePath -Path $StageRoot

if ($CopyBackOnly) {
    $published = Copy-StageEvidenceBack
    if (-not $published) {
        throw "No staged file-recovery evidence and guest report under $StageRoot; refusing to report a successful copy-back."
    }
    Write-Host "Local staged file-recovery evidence copied back."
    return
}

if ($TargetDiskNumber -notmatch '^[1-9][0-9]{0,3}$') {
    throw "-TargetDiskNumber must be an explicit decimal disk number of 1 or higher. This gate runs the elevated child with -Force and refuses to default to a destructive target."
}

if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
    throw "SystemRoot is not set; refusing to resolve the elevated interpreter by name."
}
$powerShellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powerShellExe -PathType Leaf)) {
    throw "Windows PowerShell was not found at its system path: $powerShellExe"
}

New-RestrictedStageDirectory -Path $StageRoot | Out-Null

# A fresh run must not be able to publish the previous run's evidence as its own.
if (Test-Path -LiteralPath $localEvidence -PathType Container) {
    Remove-Item -LiteralPath $localEvidence -Recurse -Force
}
if (Test-Path -LiteralPath $localGuestReport -PathType Leaf) {
    Remove-Item -LiteralPath $localGuestReport -Force
}
New-Item -ItemType Directory -Path $localEvidence -Force | Out-Null
New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null

$stagedRunnerHash = Copy-VerifiedStagedFile `
    -SourcePath (Join-Path $SharedRoot "scripts\run_partition_manager_file_recovery_external_gate.ps1") `
    -DestinationPath $localRunner

$runtimeFiles = @(
    "partition_file_recovery_certifier.exe",
    "Qt6Core.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "concrt140.dll"
)

$stagedRuntimeHashes = @{}
foreach ($runtimeFile in $runtimeFiles) {
    $source = Join-Path $SharedRoot "build\Release\$runtimeFile"
    $stagedRuntimeHashes[$runtimeFile] = Copy-VerifiedStagedFile `
        -SourcePath $source `
        -DestinationPath (Join-Path $StageRoot $runtimeFile)
}

# Re-verify immediately before the UAC prompt: nothing staged may change in between.
Assert-StagedFileUnchanged -Path $localRunner -ExpectedHash $stagedRunnerHash
foreach ($runtimeFile in $runtimeFiles) {
    Assert-StagedFileUnchanged -Path (Join-Path $StageRoot $runtimeFile) -ExpectedHash $stagedRuntimeHashes[$runtimeFile]
}

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-TargetDiskNumber", $TargetDiskNumber,
    "-Force",
    "-CertifierPath", "`"$localCertifier`"",
    "-EvidenceDir", "`"$localEvidence`"",
    "-GuestReportPath", "`"$localGuestReport`""
)

$process = Start-Process -FilePath $powerShellExe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($null -eq $process) {
    throw "Elevated file-recovery gate did not start."
}

$published = Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated file-recovery gate exited with code $($process.ExitCode). Evidence copied when available."
}
if (-not $published) {
    throw "Elevated file-recovery gate exited 0 but produced no evidence directory content or guest report; refusing to report a completed gate."
}

Write-Host "Local staged file-recovery external gate completed."
