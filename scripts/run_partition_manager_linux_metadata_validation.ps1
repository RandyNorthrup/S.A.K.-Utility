<#
.SYNOPSIS
    Validates S.A.K. XFS/Btrfs raw metadata detection against Linux-created images.

.DESCRIPTION
    Creates disposable XFS and Btrfs images from a Linux environment, then runs
    partition_filesystem_probe_certifier.exe against each image from Windows.
    This is certification evidence only; the app never depends on WSL, Linux
    ISOs, or Linux runtime tools.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ".",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-metadata-validation",
    [string]$ReportPath = "",
    [string]$DistroName = "archlinux",
    [string[]]$FileSystems = @("xfs", "btrfs"),
    [uint64]$ImageSizeBytes = 512MB,
    [string]$CertifierPath = "",
    [switch]$KeepImages
)

$ErrorActionPreference = "Stop"

function ConvertTo-PlainText {
    param([object[]]$Value)
    return (($Value | ForEach-Object {
        if ($null -eq $_) { "" } else { $_.ToString() }
    }) -join "`n").Trim()
}

function Invoke-RecordedCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [int[]]$AcceptedExitCodes = @(0)
    )

    $started = Get-Date
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    $record = [pscustomobject]@{
        name = $Name
        file_path = $FilePath
        arguments = $Arguments
        exit_code = [int]$exitCode
        duration_seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        output = ConvertTo-PlainText -Value $output
    }
    $Script:Commands.Add($record) | Out-Null
    if ($AcceptedExitCodes -notcontains [int]$exitCode) {
        throw "$Name failed with exit code $exitCode. $($record.output)"
    }
    return $record
}

function Invoke-WslScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Script,
        [int[]]$AcceptedExitCodes = @(0)
    )

    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Script))
    return Invoke-RecordedCommand -Name $Name `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "-u", "root", "--", "sh", "-lc",
            "printf '%s' '$encoded' | base64 -d | bash") `
        -AcceptedExitCodes $AcceptedExitCodes
}

function ConvertTo-RedactedReportJson {
    param([Parameter(Mandatory = $true)] [object]$Report)

    $json = $Report | ConvertTo-Json -Depth 16
    $windowsRoot = $ProjectRoot.Replace("\", "\\")
    $forwardRoot = $ProjectRoot.Replace("\", "/")
    $json = $json.Replace($windowsRoot, "<repo>")
    $json = $json.Replace($forwardRoot, "<repo>")

    if ($ProjectRoot -match "^([A-Za-z]):\\(.+)$") {
        $wslRoot = "/mnt/" + $Matches[1].ToLowerInvariant() + "/" + ($Matches[2] -replace "\\", "/")
        $json = $json.Replace($wslRoot, "<repo>")
    }
    return $json
}

function Resolve-Certifier {
    if (-not [string]::IsNullOrWhiteSpace($CertifierPath)) {
        return (Resolve-Path -LiteralPath $CertifierPath).Path
    }
    $candidate = Join-Path $ProjectRoot "build\Release\partition_filesystem_probe_certifier.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }
    throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
}

function Assert-ApprovedFileSystem {
    param([Parameter(Mandatory = $true)] [string]$FileSystem)

    if (@("xfs", "btrfs") -notcontains $FileSystem.ToLowerInvariant()) {
        throw "FileSystems may only contain xfs or btrfs for this validation lane."
    }
}

function New-Image {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [uint64]$SizeBytes
    )

    Invoke-RecordedCommand -Name "create-disposable-image" `
        -FilePath "fsutil.exe" `
        -Arguments @("file", "createnew", $Path, [string]$SizeBytes) | Out-Null
}

function ConvertTo-WslPath {
    param([Parameter(Mandatory = $true)] [string]$WindowsPath)

    $record = Invoke-RecordedCommand -Name "wslpath" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "wslpath", "-a", ($WindowsPath -replace "\\", "/"))
    return $record.output.Trim()
}

function Format-LinuxImage {
    param(
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [Parameter(Mandatory = $true)] [string]$LinuxPath
    )

    $label = if ($FileSystem -eq "xfs") { "SAK_XFS" } else { "SAK_BTRFS" }
    $formatCommand = if ($FileSystem -eq "xfs") {
        "mkfs.xfs -f -L '$label' '$LinuxPath'"
    }
    else {
        "mkfs.btrfs -f -L '$label' '$LinuxPath'"
    }
    Invoke-WslScript -Name "linux-format-$FileSystem" -Script @"
set -euo pipefail
$formatCommand
sync
"@ | Out-Null
}

function Invoke-ProbeCertifier {
    param(
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [Parameter(Mandatory = $true)] [string]$ImagePath,
        [Parameter(Mandatory = $true)] [string]$OutputPath,
        [Parameter(Mandatory = $true)] [string]$Certifier
    )

    $expected = if ($FileSystem -eq "xfs") { "XFS" } else { "Btrfs" }
    Invoke-RecordedCommand -Name "sak-probe-certifier-$FileSystem" `
        -FilePath $Certifier `
        -Arguments @("--input", $ImagePath, "--output", $OutputPath, "--expect", $expected, "--require-sane") | Out-Null
    return Get-Content -LiteralPath $OutputPath -Raw | ConvertFrom-Json
}

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$EvidenceRoot = Join-Path $ProjectRoot $EvidenceRoot
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $EvidenceRoot "report.json"
}
$runRoot = Join-Path $EvidenceRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null

$Script:Commands = [System.Collections.Generic.List[object]]::new()
$started = (Get-Date).ToUniversalTime().ToString("o")
$status = "Failed"
$errorText = ""
$imageResults = [System.Collections.Generic.List[object]]::new()
$imageCleanup = [System.Collections.Generic.List[string]]::new()

try {
    foreach ($fileSystem in $FileSystems) {
        Assert-ApprovedFileSystem -FileSystem $fileSystem
    }
    $certifier = Resolve-Certifier
    Invoke-RecordedCommand -Name "wsl-uname" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "uname", "-a") | Out-Null
    Invoke-WslScript -Name "linux-metadata-tool-preflight" -Script @"
set -euo pipefail
command -v mkfs.xfs
command -v mkfs.btrfs
id
"@ | Out-Null

    foreach ($fileSystem in $FileSystems) {
        $normalized = $fileSystem.ToLowerInvariant()
        $imagePath = Join-Path $runRoot "sak-$normalized-metadata.img"
        $probePath = Join-Path $runRoot "sak-$normalized-probe-report.json"
        New-Image -Path $imagePath -SizeBytes $ImageSizeBytes
        $linuxPath = ConvertTo-WslPath -WindowsPath $imagePath
        Format-LinuxImage -FileSystem $normalized -LinuxPath $linuxPath
        $probe = Invoke-ProbeCertifier -FileSystem $normalized `
            -ImagePath $imagePath `
            -OutputPath $probePath `
            -Certifier $certifier
        $imageResults.Add([pscustomobject]@{
            file_system = $normalized
            image_path = $imagePath
            linux_image_path = $linuxPath
            probe_report_path = $probePath
            image_sha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
            detected_file_system = $probe.detected_file_system
            total_bytes = $probe.total_bytes
            free_bytes = $probe.free_bytes
            details = @($probe.details)
        }) | Out-Null
        if (-not $KeepImages) {
            Remove-Item -LiteralPath $imagePath -Force
            $imageCleanup.Add("Removed disposable $normalized metadata image.") | Out-Null
        }
    }
    $status = "Passed"
}
catch {
    $status = "Failed"
    $errorText = ConvertTo-PlainText -Value @($_)
}

$report = [pscustomobject]@{
    schema_version = 1
    gate_id = "external.linux-metadata-validation"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    distro_name = $DistroName
    file_systems = @($FileSystems)
    image_size_bytes = $ImageSizeBytes
    certifier_path = $certifier
    image_results = @($imageResults.ToArray())
    image_cleanup = @($imageCleanup.ToArray())
    commands = @($Script:Commands.ToArray())
    error = $errorText
}
$reportJson = ConvertTo-RedactedReportJson -Report $report
$reportJson | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$reportJson | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Linux metadata validation failed. Report: $ReportPath`n$errorText"
}

Write-Host "Linux metadata validation passed: $($FileSystems -join ', ') via WSL distro $DistroName"
Write-Host "Report: $ReportPath"
