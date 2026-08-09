<#
.SYNOPSIS
    Stages and launches offline BIOS bcdboot repair elevated inside the VM.

.DESCRIPTION
    This rewrites boot records on the offline Windows installation it finds, so it
    is a disposable-lab operation only. The run must be authorized explicitly with
    -Force, and the host must report as a virtual machine unless the operator
    deliberately overrides that with -AllowNonVirtualHost.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-offline-bios-repair",
    [string]$OutputRoot = "\\vboxsvr\sakrepo\artifacts\partition-manager-certification\vm-lab\offline-bios-repair",
    [switch]$Force,
    [switch]$AllowNonVirtualHost
)

$ErrorActionPreference = "Stop"

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

function Assert-OutputRootPath {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [AllowNull()]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "OutputRoot must be an explicit path."
    }
    if ($Path -match '["\x00-\x1f]') {
        throw "OutputRoot contains characters that cannot cross the elevation boundary: $Path"
    }
    if ($Path -match '^\\\\[?.]\\') {
        throw "OutputRoot must not be a device path: $Path"
    }
    if (($Path -notmatch '^[A-Za-z]:\\') -and ($Path -notmatch '^\\\\[^\\?.][^\\]*\\[^\\]')) {
        throw "OutputRoot must be a drive-qualified local path or a UNC share path: $Path"
    }
}

# The "inside the VM" contract in the synopsis has to be checked, not just written
# down: this rewrites boot records and must never land on a technician's own host.
function Assert-VirtualMachineHost {
    if ($AllowNonVirtualHost) {
        Write-Host "Virtual-machine check bypassed by -AllowNonVirtualHost."
        return
    }

    $system = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
    $identity = "$($system.Manufacturer) $($system.Model)"
    $markers = @(
        "VirtualBox",
        "innotek",
        "VMware",
        "Virtual Machine",
        "Virtual Platform",
        "QEMU",
        "KVM",
        "Bochs",
        "Parallels",
        "Xen",
        "Hyper-V"
    )
    foreach ($marker in $markers) {
        if ($identity -like "*$marker*") {
            return
        }
    }
    throw "Offline BIOS bcdboot repair is a disposable-lab operation and this host does not report as a virtual machine ($identity). Run it inside the lab VM, or pass -AllowNonVirtualHost to override deliberately."
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

# The staged script is executed elevated, so the staging directory must not remain
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

if (-not $Force) {
    throw "Offline BIOS bcdboot repair rewrites boot records on the offline Windows installation. Re-run with -Force to authorize the destructive repair explicitly."
}

Assert-LocalStagePath -Path $StageRoot
Assert-OutputRootPath -Path $OutputRoot
Assert-VirtualMachineHost

if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
    throw "SystemRoot is not set; refusing to resolve the elevated interpreter by name."
}
$powerShellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powerShellExe -PathType Leaf)) {
    throw "Windows PowerShell was not found at its system path: $powerShellExe"
}

New-RestrictedStageDirectory -Path $StageRoot | Out-Null
$localScript = Join-Path $StageRoot "run_partition_manager_offline_bios_bcdboot_repair.ps1"
$stagedScriptHash = Copy-VerifiedStagedFile `
    -SourcePath (Join-Path $SharedRoot "scripts\run_partition_manager_offline_bios_bcdboot_repair.ps1") `
    -DestinationPath $localScript

# Re-verify immediately before the UAC prompt: the staged script may not change in between.
$hashBeforeLaunch = (Get-FileHash -LiteralPath $localScript -Algorithm SHA256).Hash
if ($hashBeforeLaunch -ne $stagedScriptHash) {
    throw "Staged offline BIOS repair script changed after staging; refusing to run it elevated: $localScript"
}

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localScript`"",
    "-OutputRoot", "`"$OutputRoot`"",
    "-Force"
)

$process = Start-Process -FilePath $powerShellExe -Verb RunAs -Wait -PassThru -ArgumentList $argumentList
if ($null -eq $process) {
    throw "Elevated offline BIOS repair did not start."
}
if ($process.ExitCode -ne 0) {
    throw "Elevated offline BIOS repair exited with code $($process.ExitCode)."
}

Write-Host "Elevated offline BIOS repair completed."
