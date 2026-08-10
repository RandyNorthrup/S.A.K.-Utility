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
$WslExe = Join-Path $env:SystemRoot "System32\wsl.exe"
$FsutilExe = Join-Path $env:SystemRoot "System32\fsutil.exe"

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

    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "$Name could not resolve executable '$FilePath'."
    }
    $started = Get-Date
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $exitCode = $null
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($null -eq $exitCode) {
        throw "$Name did not produce an exit code (launch failed)."
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
        -FilePath $WslExe `
        -Arguments @("-d", $DistroName, "-u", "root", "--", "bash", "-lc",
            "set -o pipefail; printf '%s' '$encoded' | base64 -d | bash") `
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
        $resolved = (Resolve-Path -LiteralPath $CertifierPath).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "CertifierPath must reference an existing file: $resolved"
        }
        if ([IO.Path]::GetExtension($resolved).ToLowerInvariant() -ne ".exe") {
            throw "CertifierPath must reference a .exe: $resolved"
        }
        return $resolved
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
        -FilePath $FsutilExe `
        -Arguments @("file", "createnew", $Path, [string]$SizeBytes) | Out-Null
}

function ConvertTo-WslPath {
    param([Parameter(Mandatory = $true)] [string]$WindowsPath)

    $record = Invoke-RecordedCommand -Name "wslpath" `
        -FilePath $WslExe `
        -Arguments @("-d", $DistroName, "--", "wslpath", "-a", ($WindowsPath -replace "\\", "/"))
    return $record.output.Trim()
}

function Format-LinuxImage {
    param(
        [Parameter(Mandatory = $true)] [string]$FileSystem,
        [Parameter(Mandatory = $true)] [string]$LinuxPath
    )

    $label = if ($FileSystem -eq "xfs") { "SAK_XFS" } else { "SAK_BTRFS" }
    # Escape any single quote so a path (or hostile wslpath output) cannot break out
    # of the single-quoted root Bash argument and inject commands.
    $escapedPath = $LinuxPath.Replace("'", "'\''")
    $formatCommand = if ($FileSystem -eq "xfs") {
        "mkfs.xfs -f -L '$label' '$escapedPath'"
    }
    else {
        "mkfs.btrfs -f -L '$label' '$escapedPath'"
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
    $probe = Get-Content -LiteralPath $OutputPath -Raw | ConvertFrom-Json
    if ($null -eq $probe -or $probe -isnot [pscustomobject]) {
        throw "Probe certifier for $FileSystem produced non-object JSON at $OutputPath."
    }
    if ([string]::IsNullOrWhiteSpace([string]$probe.detected_file_system)) {
        throw "Probe certifier for $FileSystem produced no detected_file_system at $OutputPath."
    }
    return $probe
}

if (@($FileSystems).Count -eq 0) {
    throw "FileSystems must contain at least one approved filesystem (xfs or btrfs)."
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$EvidenceRoot = Join-Path $ProjectRoot $EvidenceRoot
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $EvidenceRoot "report.json"
}
# Unique run directory so a same-second/concurrent run can never reuse or read a
# prior run's probe reports as if they were freshly produced.
$runRoot = Join-Path $EvidenceRoot ("run-" + (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $runRoot | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $ReportPath) -Force | Out-Null
# Invalidate any prior main report up front so a crash before the final write leaves
# no stale Passed evidence behind (absence, never a preserved pass).
if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
    Remove-Item -LiteralPath $ReportPath -Force
}

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
        -FilePath $WslExe `
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
        # Pin that the freshly created image is a regular, non-reparse file of the
        # expected size before handing its path to root WSL mkfs (which force-formats
        # whatever the translated path resolves to).
        $imageItem = Get-Item -LiteralPath $imagePath -Force
        if ($imageItem.PSIsContainer -or (($imageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Disposable image path is not a regular file: $imagePath"
        }
        if ([uint64]$imageItem.Length -ne $ImageSizeBytes) {
            throw "Disposable image size mismatch for $imagePath (expected $ImageSizeBytes, got $($imageItem.Length))."
        }
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
    certifier_sha256 = if (-not [string]::IsNullOrWhiteSpace($certifier)) { (Get-FileHash -LiteralPath $certifier -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
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
