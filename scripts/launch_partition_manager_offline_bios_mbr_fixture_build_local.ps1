<#
.SYNOPSIS
    Stages and launches offline BIOS/MBR fixture build elevated inside the VM.

.DESCRIPTION
    Refuses to run outside a hypervisor guest, validates every path input,
    stages the runner into a fresh ACL-protected directory, and hands the
    elevated child a hash-pinned command. SourceDiskNumber/TargetDiskNumber are
    forwarded only when supplied so the operator can pin the target disks
    instead of leaving the runner to auto-select.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-bios-mbr-fixture",
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\bios-mbr-fixture",
    [ValidateRange(-1, 4095)] [int]$SourceDiskNumber = -1,
    [ValidateRange(-1, 4095)] [int]$TargetDiskNumber = -1
)

$ErrorActionPreference = "Stop"

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

function Assert-LaunchPath {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value
    )
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Name is empty. Pass an explicit rooted path."
    }
    if ($Value.Contains('"')) {
        throw "$Name contains a double quote and cannot be handed to an elevated child: $Value"
    }
    if (-not [System.IO.Path]::IsPathRooted($Value)) {
        throw "$Name must be a rooted path: $Value"
    }
    return $Value
}

function Assert-VirtualMachineGuest {
    $system = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
    $identity = "$($system.Manufacturer) $($system.Model)".Trim()
    foreach ($marker in @("VirtualBox", "innotek", "VMware", "QEMU", "KVM", "Xen", "Parallels", "Bochs", "Virtual Machine")) {
        if ($identity -like "*$marker*") {
            return $identity
        }
    }
    throw "Refusing to run: '$identity' is not a known hypervisor guest. This launcher starts destructive elevated work and only runs inside the certification VM."
}

function Protect-StageDirectory {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $acl = Get-Acl -LiteralPath $Path
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($rule in @($acl.Access | Where-Object { -not $_.IsInherited })) {
        $acl.RemoveAccessRuleSpecific($rule)
    }
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
    $identities = @(
        [System.Security.Principal.WindowsIdentity]::GetCurrent().User,
        (New-Object System.Security.Principal.SecurityIdentifier -ArgumentList "S-1-5-32-544"),
        (New-Object System.Security.Principal.SecurityIdentifier -ArgumentList "S-1-5-18")
    )
    foreach ($identity in $identities) {
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule -ArgumentList @(
                    $identity,
                    [System.Security.AccessControl.FileSystemRights]::FullControl,
                    $inheritance,
                    [System.Security.AccessControl.PropagationFlags]::None,
                    [System.Security.AccessControl.AccessControlType]::Allow)))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Get-ElevatedLogTail {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return " No elevated console log was written to $Path."
    }
    $tail = (Get-Content -LiteralPath $Path -Tail 40 -ErrorAction Stop) -join "`n"
    if ([string]::IsNullOrWhiteSpace($tail)) {
        return " Elevated console log $Path is empty."
    }
    return "`nElevated console output (tail):`n$tail"
}

function Assert-FreshEvidence {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [datetime]$SinceUtc
    )
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Elevated run reported success but the evidence root is missing: $Root"
    }
    $cutoff = $SinceUtc.AddMinutes(-2)
    $fresh = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction Stop |
            Where-Object { $_.LastWriteTimeUtc -ge $cutoff })
    if ($fresh.Count -eq 0) {
        throw "Elevated run reported success but wrote no evidence under $Root after $($SinceUtc.ToString('o'))."
    }
}

$sharedRoot = Assert-LaunchPath -Name "SharedRoot" -Value $SharedRoot
$stageRoot = Assert-LaunchPath -Name "StageRoot" -Value $StageRoot
$outputRoot = Assert-LaunchPath -Name "OutputRoot" -Value $OutputRoot
Write-Host "Lab VM identity: $(Assert-VirtualMachineGuest)"

$sourceScript = Join-Path $sharedRoot "scripts\run_partition_manager_offline_bios_mbr_fixture_build.ps1"
if (-not (Test-Path -LiteralPath $sourceScript -PathType Leaf)) {
    throw "Runner not found on the shared root: $sourceScript"
}

# %PUBLIC% is writable by every local user, so a predictable reused stage path can be
# swapped for attacker content between the copy and the elevated open. Stage into a
# fresh unpredictable directory whose ACL is locked to this user, Administrators and
# SYSTEM, then pin the staged bytes by hash.
$runStage = Join-Path $stageRoot ([Guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $runStage) {
    throw "Stage directory already exists: $runStage"
}
$stageDirectory = New-Item -ItemType Directory -Path $runStage
if ($stageDirectory.Attributes.HasFlag([System.IO.FileAttributes]::ReparsePoint)) {
    throw "Stage directory is a reparse point: $runStage"
}
Protect-StageDirectory -Path $stageDirectory.FullName

$localScript = Join-Path $stageDirectory.FullName "run_partition_manager_offline_bios_mbr_fixture_build.ps1"
Copy-Item -LiteralPath $sourceScript -Destination $localScript
$sourceHash = (Get-FileHash -LiteralPath $sourceScript -Algorithm SHA256).Hash
$stagedHash = (Get-FileHash -LiteralPath $localScript -Algorithm SHA256).Hash
if ($stagedHash -ne $sourceHash) {
    throw "Staged runner does not match the shared source (source $sourceHash, staged $stagedHash)."
}

$childLog = Join-Path $stageDirectory.FullName "elevated-console.log"
$runnerArgs = "-OutputRoot $(Quote-PowerShellLiteral $outputRoot) -ResumeExistingTarget -Force"
if ($SourceDiskNumber -ge 0) {
    $runnerArgs += " -SourceDiskNumber $SourceDiskNumber"
}
if ($TargetDiskNumber -ge 0) {
    $runnerArgs += " -TargetDiskNumber $TargetDiskNumber"
}

$command = @(
    '$ErrorActionPreference = "Stop"',
    "`$expectedHash = $(Quote-PowerShellLiteral $stagedHash)",
    "`$actualHash = (Get-FileHash -LiteralPath $(Quote-PowerShellLiteral $localScript) -Algorithm SHA256).Hash",
    "if (`$actualHash -ne `$expectedHash) { throw ('Staged runner changed between staging and elevation: ' + $(Quote-PowerShellLiteral $localScript)) }",
    "try {",
    "    & $(Quote-PowerShellLiteral $localScript) $runnerArgs *>&1 | Tee-Object -FilePath $(Quote-PowerShellLiteral $childLog)",
    "}",
    "catch {",
    "    (`$_ | Out-String) | Add-Content -LiteralPath $(Quote-PowerShellLiteral $childLog)",
    "    throw",
    "}",
    "if (`$LASTEXITCODE) { exit `$LASTEXITCODE }"
) -join "`n"
$encodedCommand = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

$powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powershell -PathType Leaf)) {
    throw "Windows PowerShell was not found at its System32 path: $powershell"
}

$launchUtc = (Get-Date).ToUniversalTime()
$process = Start-Process -FilePath $powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-EncodedCommand", $encodedCommand
)
if ($process.ExitCode -ne 0) {
    throw "Elevated BIOS/MBR fixture build exited with code $($process.ExitCode).$(Get-ElevatedLogTail -Path $childLog)"
}
Assert-FreshEvidence -Root $outputRoot -SinceUtc $launchUtc

Write-Host "Elevated BIOS/MBR fixture build completed. Console log: $childLog"
