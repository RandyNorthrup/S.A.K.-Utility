<#
.SYNOPSIS
    Stages and launches the HDD defrag external gate inside the VM.

.DESCRIPTION
    Run this from the non-elevated SAK-PM-Lab-Win11 desktop. It copies the gate
    runner from the VirtualBox shared repo to local staging, runs it elevated,
    then copies the resulting evidence back into the shared repository.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-hdd-defrag-gate",
    [string]$TargetDiskNumber = "",
    [switch]$CopyBackOnly
)

$ErrorActionPreference = "Stop"

$sharedEvidence = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution"
$sharedGuestReport = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\external-hdd-defrag-guest-report.json"
$sharedStageDebug = Join-Path $SharedRoot "artifacts\partition-manager-certification\vm-lab\hdd-defrag-stage-debug"
$localEvidence = Join-Path $StageRoot "evidence"
$localGuestReport = Join-Path $StageRoot "external-hdd-defrag-guest-report.json"
$localRunner = Join-Path $StageRoot "run_partition_manager_hdd_defrag_external_gate.ps1"
$staleReportNames = @("report.json", "report.failed.json", "report.failed-cleanup.json")

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

# The staged runner is executed elevated, so the staging directory must not remain
# writable by every local user between the copy and the UAC prompt.
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
        throw "Missing shared source file: $SourcePath"
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
# were published. Shared reports are purged only once a replacement is in hand, so a
# copy-back with nothing staged can never destroy the proof it fails to replace.
function Copy-StageEvidenceBack {
    $localEntries = @()
    if (Test-Path -LiteralPath $localEvidence -PathType Container) {
        $localEntries = @(Get-ChildItem -LiteralPath $localEvidence -Force)
    }

    $copiedEvidence = $false
    if ($localEntries.Count -gt 0) {
        New-Item -ItemType Directory -Path $sharedEvidence -Force | Out-Null
        foreach ($staleReport in $staleReportNames) {
            $stalePath = Join-Path $sharedEvidence $staleReport
            if (Test-Path -LiteralPath $stalePath -PathType Leaf) {
                Remove-Item -LiteralPath $stalePath -Force
            }
        }
        foreach ($entry in $localEntries) {
            Copy-Item -LiteralPath $entry.FullName -Destination $sharedEvidence -Recurse -Force
        }
        $copiedEvidence = $true
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
        throw "No staged HDD defrag evidence and guest report under $StageRoot; refusing to report a successful copy-back."
    }
    Write-Host "Local staged HDD defrag evidence copied back."
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
    -SourcePath (Join-Path $SharedRoot "scripts\run_partition_manager_hdd_defrag_external_gate.ps1") `
    -DestinationPath $localRunner

# Re-verify immediately before the UAC prompt: nothing staged may change in between.
Assert-StagedFileUnchanged -Path $localRunner -ExpectedHash $stagedRunnerHash

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-TargetDiskNumber", $TargetDiskNumber,
    "-Force",
    "-EvidenceDir", "`"$localEvidence`"",
    "-GuestReportPath", "`"$localGuestReport`""
)

$process = Start-Process -FilePath $powerShellExe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($null -eq $process) {
    throw "Elevated HDD defrag gate did not start."
}

$published = Copy-StageEvidenceBack

if ($process.ExitCode -ne 0) {
    throw "Elevated HDD defrag gate exited with code $($process.ExitCode). Evidence copied when available."
}
if (-not $published) {
    throw "Elevated HDD defrag gate exited 0 but produced no evidence directory content or guest report; refusing to report a completed gate."
}

Write-Host "Local staged HDD defrag external gate completed."
