param(
    [Parameter(Mandatory = $true)]
    [int]$DiskNumber,

    [Parameter(Mandatory = $true)]
    [int]$PartitionNumber,

    [string]$ExpectedSerialNumber = "",
    [string]$ExpectedFriendlyNamePattern = "",
    [string]$VolumeName = "SAK APFS Raw Proof",
    [string]$RelabeledVolumeName = "SAK APFS Raw Relabeled",
    [string]$WriteFileName = "sak-apfs-raw-proof.txt",
    [string]$WriteFileText = "SAK APFS raw write and repair proof.",
    [string]$DirectoryName = "SAK Raw Proof Folder",
    [string]$ChildFileName = "sak-apfs-child-proof.txt",
    [string]$ChildFileText = "SAK APFS root-directory child-file proof.",
    [uint64]$PatchOffsetBytes = 8,
    [string]$PatchFileText = "PATCHED",
    [uint64]$ChildPatchOffsetBytes = 8,
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

function Add-Arg {
    param([System.Collections.Generic.List[string]]$ArgumentValues, [string]$Name, [string]$Value)
    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        $ArgumentValues.Add($Name)
        $ArgumentValues.Add($Value)
    }
}

function ConvertTo-QuotedProcessArgument {
    param([string]$Value)
    if ($null -eq $Value) {
        return '""'
    }
    $escaped = $Value -replace '`', '``' -replace '"', '`"'
    return '"' + $escaped + '"'
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentValues)
    return (($ArgumentValues | ForEach-Object { ConvertTo-QuotedProcessArgument -Value $_ }) -join " ")
}

$runner = Join-Path $PSScriptRoot "run_partition_manager_apfs_raw_format_validation.ps1"
$argsList = [System.Collections.Generic.List[string]]::new()
$argsList.Add("-NoProfile")
$argsList.Add("-ExecutionPolicy")
$argsList.Add("Bypass")
$argsList.Add("-File")
$argsList.Add($runner)
$argsList.Add("-DiskNumber")
$argsList.Add([string]$DiskNumber)
$argsList.Add("-PartitionNumber")
$argsList.Add([string]$PartitionNumber)
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
$argsList.Add([string]$PatchOffsetBytes)
Add-Arg -ArgumentValues $argsList -Name "-PatchFileText" -Value $PatchFileText
$argsList.Add("-ChildPatchOffsetBytes")
$argsList.Add([string]$ChildPatchOffsetBytes)
Add-Arg -ArgumentValues $argsList -Name "-ChildPatchFileText" -Value $ChildPatchFileText
Add-Arg -ArgumentValues $argsList -Name "-CertifierPath" -Value $CertifierPath
Add-Arg -ArgumentValues $argsList -Name "-ApfsWriterCliPath" -Value $ApfsWriterCliPath
Add-Arg -ArgumentValues $argsList -Name "-OutputRoot" -Value $OutputRoot
if ($Force) {
    $argsList.Add("-Force")
}

if (Test-IsAdmin) {
    & powershell.exe @argsList
    exit $LASTEXITCODE
}

$startInfo = @{
    FilePath = "powershell.exe"
    ArgumentList = (ConvertTo-ProcessArgumentString -ArgumentValues $argsList.ToArray())
    Verb = "RunAs"
}
if ($NoWait) {
    Start-Process @startInfo
    exit 0
}

$startInfo["PassThru"] = $true
$process = Start-Process @startInfo
$process.WaitForExit()
exit $process.ExitCode
