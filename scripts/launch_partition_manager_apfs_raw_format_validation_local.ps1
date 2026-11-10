param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[1-9][0-9]{0,3}$')]
    [string]$DiskNumber,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[1-9][0-9]{0,3}$')]
    [string]$PartitionNumber,

    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$VolumeName = "SAK APFS Raw Proof",
    [string]$RelabeledVolumeName = "SAK APFS Raw Relabeled",
    [string]$WriteFileName = "sak-apfs-raw-proof.txt",
    [string]$WriteFileText = "SAK APFS raw write and repair proof.",
    [string]$DirectoryName = "SAK Raw Proof Folder",
    [string]$ChildFileName = "sak-apfs-child-proof.txt",
    [string]$ChildFileText = "SAK APFS root-directory child-file proof.",
    [ValidatePattern('^[0-9]{1,19}$')]
    [string]$PatchOffsetBytes = "8",
    [string]$PatchFileText = "PATCHED",
    [ValidatePattern('^[0-9]{1,19}$')]
    [string]$ChildPatchOffsetBytes = "8",
    [string]$ChildPatchFileText = "CHILD",
    [string]$CertifierPath = "",
    [string]$ApfsWriterCliPath = "",
    [string]$OutputRoot = "",
    [switch]$Force,
    [switch]$NoWait
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]$identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-ForwardableValue {
    param([string]$Name, [string]$Value)
    if ($Value -match "[\x00-\x1f]") {
        throw "Refusing to forward control characters across the elevation boundary in $Name."
    }
}

function Add-Arg {
    param([System.Collections.Generic.List[string]]$ArgumentValues, [string]$Name, [string]$Value)
    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        Assert-ForwardableValue -Name $Name -Value $Value
        $ArgumentValues.Add($Name)
        $ArgumentValues.Add($Value)
    }
}

# Windows native command lines are parsed by CommandLineToArgvW, which escapes with
# backslashes, not PowerShell backticks. Quoting the way that parser expects is what
# stops a value such as 'x" -Force' from closing its own quoted region and injecting
# an extra argument into the elevated child.
function ConvertTo-QuotedProcessArgument {
    param([string]$Value)
    if ([string]::IsNullOrEmpty($Value)) {
        return '""'
    }

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $pendingBackslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $pendingBackslashes++
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append('\' * (($pendingBackslashes * 2) + 1))
            $pendingBackslashes = 0
            [void]$builder.Append('"')
            continue
        }
        if ($pendingBackslashes -gt 0) {
            [void]$builder.Append('\' * $pendingBackslashes)
            $pendingBackslashes = 0
        }
        [void]$builder.Append($character)
    }
    [void]$builder.Append('\' * ($pendingBackslashes * 2))
    [void]$builder.Append('"')
    return $builder.ToString()
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentValues)
    return (($ArgumentValues | ForEach-Object { ConvertTo-QuotedProcessArgument -Value $_ }) -join " ")
}

function Resolve-ExistingToolPath {
    param([string]$Name, [string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    Assert-ForwardableValue -Name $Name -Value $Value
    $resolved = Resolve-Path -LiteralPath $Value -ErrorAction Stop
    $item = Get-Item -LiteralPath $resolved.Path -Force -ErrorAction Stop
    if ($item -isnot [System.IO.FileInfo]) {
        throw "$Name does not name a file: $($resolved.Path)"
    }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Name is a reparse point: $($resolved.Path)"
    }
    return $item.FullName
}

# An unattended destructive run must pin the target by stable identity: disk numbers
# renumber across hot-plug and reboot.
if ($Force -and [string]::IsNullOrWhiteSpace($ExpectedSerialNumber) -and [string]::IsNullOrWhiteSpace($ExpectedFriendlyNamePattern)) {
    throw "-Force requires -ExpectedSerialNumber or -ExpectedFriendlyNamePattern so the destructive target disk is pinned by identity, not by number alone."
}

if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
    throw "SystemRoot is not set; refusing to resolve the elevated interpreter by name."
}
$powerShellExe = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
if (-not (Test-Path -LiteralPath $powerShellExe -PathType Leaf)) {
    throw "Windows PowerShell was not found at its system path: $powerShellExe"
}

$CertifierPath = Resolve-ExistingToolPath -Name "CertifierPath" -Value $CertifierPath
$ApfsWriterCliPath = Resolve-ExistingToolPath -Name "ApfsWriterCliPath" -Value $ApfsWriterCliPath

$runner = Join-Path $PSScriptRoot "run_partition_manager_apfs_raw_format_validation.ps1"
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw "Missing APFS raw format validation runner: $runner"
}

$argsList = [System.Collections.Generic.List[string]]::new()
$argsList.Add("-NoProfile")
$argsList.Add("-ExecutionPolicy")
$argsList.Add("Bypass")
$argsList.Add("-File")
$argsList.Add($runner)
$argsList.Add("-DiskNumber")
$argsList.Add($DiskNumber)
$argsList.Add("-PartitionNumber")
$argsList.Add($PartitionNumber)
Add-Arg -ArgumentValues $argsList -Name "-ExpectedSerialNumber" -Value $ExpectedSerialNumber
Add-Arg -ArgumentValues $argsList -Name "-ExpectedFriendlyNamePattern" -Value $ExpectedFriendlyNamePattern
Add-Arg -ArgumentValues $argsList -Name "-VolumeName" -Value $VolumeName
Add-Arg -ArgumentValues $argsList -Name "-RelabeledVolumeName" -Value $RelabeledVolumeName
Add-Arg -ArgumentValues $argsList -Name "-WriteFileName" -Value $WriteFileName
Add-Arg -ArgumentValues $argsList -Name "-WriteFileText" -Value $WriteFileText
Add-Arg -ArgumentValues $argsList -Name "-DirectoryName" -Value $DirectoryName
Add-Arg -ArgumentValues $argsList -Name "-ChildFileName" -Value $ChildFileName
Add-Arg -ArgumentValues $argsList -Name "-ChildFileText" -Value $ChildFileText
$argsList.Add("-PatchOffsetBytes")
$argsList.Add($PatchOffsetBytes)
Add-Arg -ArgumentValues $argsList -Name "-PatchFileText" -Value $PatchFileText
$argsList.Add("-ChildPatchOffsetBytes")
$argsList.Add($ChildPatchOffsetBytes)
Add-Arg -ArgumentValues $argsList -Name "-ChildPatchFileText" -Value $ChildPatchFileText
Add-Arg -ArgumentValues $argsList -Name "-CertifierPath" -Value $CertifierPath
Add-Arg -ArgumentValues $argsList -Name "-ApfsWriterCliPath" -Value $ApfsWriterCliPath
Add-Arg -ArgumentValues $argsList -Name "-OutputRoot" -Value $OutputRoot
if ($Force) {
    $argsList.Add("-Force")
}

if (Test-IsAdmin) {
    & $powerShellExe @argsList
    exit $LASTEXITCODE
}

$startInfo = @{
    FilePath = $powerShellExe
    ArgumentList = (ConvertTo-ProcessArgumentString -ArgumentValues $argsList.ToArray())
    Verb = "RunAs"
}
if ($NoWait) {
    $startInfo["PassThru"] = $true
    $launched = Start-Process @startInfo
    if ($null -eq $launched) {
        throw "Elevated APFS raw format validation did not start."
    }
    Write-Host "Elevated APFS raw format validation started as process $($launched.Id). -NoWait reports launch only, not the validation result; re-run without -NoWait for a pass or fail exit code."
    exit 0
}

$startInfo["PassThru"] = $true
$process = Start-Process @startInfo
if ($null -eq $process) {
    throw "Elevated APFS raw format validation did not start."
}
$process.WaitForExit()
exit $process.ExitCode
