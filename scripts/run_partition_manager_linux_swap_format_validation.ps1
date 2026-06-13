<#
.SYNOPSIS
    Validates original S.A.K. Linux swap format metadata against S.A.K. and Linux probes.

.DESCRIPTION
    Creates a disposable image file, writes a SWAPSPACE2 v1 header with the same field
    layout used by the Partition Manager queue/apply Linux swap formatter, verifies S.A.K.
    detection with partition_filesystem_probe_certifier.exe, then verifies Linux userspace
    recognition through WSL blkid and swaplabel. This is non-destructive certification
    evidence only; the app has no WSL runtime dependency.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ".",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-validation",
    [string]$ReportPath = "",
    [string]$DistroName = "archlinux",
    [uint64]$ImageSizeBytes = 128MB,
    [ValidateSet(4096, 8192, 16384, 65536)] [int]$PageSizeBytes = 4096,
    [string]$Label = "SAKSWAP",
    [string]$CertifierPath = "",
    [switch]$KeepImage
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

function ConvertTo-WslPath {
    param([Parameter(Mandatory = $true)] [string]$WindowsPath)

    $record = Invoke-RecordedCommand -Name "wslpath" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "wslpath", "-a", ($WindowsPath -replace "\\", "/"))
    return $record.output.Trim()
}

function Write-UInt32Le {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Buffer,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint32]$Value
    )

    $bytes = [BitConverter]::GetBytes($Value)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, 4)
}

function ConvertTo-UuidText {
    param([Parameter(Mandatory = $true)] [byte[]]$Bytes)

    if ($Bytes.Length -ne 16) {
        throw "UUID requires exactly 16 bytes."
    }
    $hex = ($Bytes | ForEach-Object { $_.ToString("x2") }) -join ""
    return "{0}-{1}-{2}-{3}-{4}" -f $hex.Substring(0, 8),
        $hex.Substring(8, 4),
        $hex.Substring(12, 4),
        $hex.Substring(16, 4),
        $hex.Substring(20, 12)
}

function Write-LinuxSwapHeader {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [int]$PageSize,
        [Parameter(Mandatory = $true)] [string]$VolumeLabel
    )

    $info = Get-Item -LiteralPath $Path -ErrorAction Stop
    $pageCount = [uint64][Math]::Floor([double]$info.Length / [double]$PageSize)
    if ($pageCount -lt 2) {
        throw "Linux swap image is too small."
    }
    if ($pageCount -gt [uint64]4294967295) {
        throw "Linux swap page count exceeds v1 header limit."
    }

    $label = $VolumeLabel.Trim()
    if ($label.Length -gt 16) {
        $label = $label.Substring(0, 16)
    }
    $header = New-Object byte[] $PageSize
    Write-UInt32Le -Buffer $header -Offset 1024 -Value ([uint32]1)
    Write-UInt32Le -Buffer $header -Offset 1028 -Value ([uint32]($pageCount - 1))
    Write-UInt32Le -Buffer $header -Offset 1032 -Value ([uint32]0)
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $uuidBytes = New-Object byte[] 16
    try {
        $rng.GetBytes($uuidBytes)
    }
    finally {
        $rng.Dispose()
    }
    [Array]::Copy($uuidBytes, 0, $header, 1036, 16)
    $labelBytes = [System.Text.Encoding]::ASCII.GetBytes($label)
    $labelLength = [Math]::Min($labelBytes.Length, 16)
    if ($labelLength -gt 0) {
        [Array]::Copy($labelBytes, 0, $header, 1052, $labelLength)
    }
    $signatureBytes = [System.Text.Encoding]::ASCII.GetBytes("SWAPSPACE2")
    [Array]::Copy($signatureBytes, 0, $header, $PageSize - 10, 10)

    $stream = [System.IO.File]::Open($Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite)
    try {
        [void]$stream.Seek(0, [System.IO.SeekOrigin]::Begin)
        $stream.Write($header, 0, $header.Length)
        $stream.Flush()
    }
    finally {
        $stream.Dispose()
    }

    return [pscustomobject]@{
        page_size_bytes = $PageSize
        page_count = [string]$pageCount
        last_page = [string]([uint64]$pageCount - 1)
        label = $label
        uuid = ConvertTo-UuidText -Bytes $uuidBytes
    }
}

function Invoke-ProbeCertifier {
    param(
        [Parameter(Mandatory = $true)] [string]$ImagePath,
        [Parameter(Mandatory = $true)] [string]$OutputPath,
        [Parameter(Mandatory = $true)] [string]$Certifier
    )

    Invoke-RecordedCommand -Name "sak-probe-certifier-linux-swap" `
        -FilePath $Certifier `
        -Arguments @("--input", $ImagePath, "--output", $OutputPath, "--expect", "Linux swap") | Out-Null
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
$imagePath = Join-Path $runRoot "sak-linux-swap-format.img"
$probePath = Join-Path $runRoot "sak-linux-swap-probe-report.json"
$header = $null
$probe = $null
$linuxPath = ""
$blkid = $null
$swaplabel = $null
$imageHash = ""
$cleanup = ""

try {
    $certifier = Resolve-Certifier
    Invoke-RecordedCommand -Name "wsl-uname" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "uname", "-a") | Out-Null
    Invoke-WslScript -Name "linux-swap-tool-preflight" -Script @"
set -euo pipefail
command -v blkid
command -v swaplabel
id
"@ | Out-Null

    Invoke-RecordedCommand -Name "create-disposable-swap-image" `
        -FilePath "fsutil.exe" `
        -Arguments @("file", "createnew", $imagePath, [string]$ImageSizeBytes) | Out-Null
    $header = Write-LinuxSwapHeader -Path $imagePath -PageSize $PageSizeBytes -VolumeLabel $Label
    $imageHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $probe = Invoke-ProbeCertifier -ImagePath $imagePath -OutputPath $probePath -Certifier $certifier
    $linuxPath = ConvertTo-WslPath -WindowsPath $imagePath
    $blkid = Invoke-WslScript -Name "linux-blkid-swap-probe" -Script @"
set -euo pipefail
blkid -p -o export '$linuxPath'
"@
    $swaplabel = Invoke-WslScript -Name "linux-swaplabel-probe" -Script @"
set -euo pipefail
swaplabel '$linuxPath'
"@
    if ($blkid.output -notmatch "TYPE=swap") {
        throw "Linux blkid did not report TYPE=swap."
    }
    if ($blkid.output -notmatch [regex]::Escape("LABEL=$($header.label)")) {
        throw "Linux blkid did not report expected swap label."
    }
    $status = "Passed"
}
catch {
    $status = "Failed"
    $errorText = ConvertTo-PlainText -Value @($_)
}
finally {
    if ((Test-Path -LiteralPath $imagePath -PathType Leaf) -and -not $KeepImage) {
        Remove-Item -LiteralPath $imagePath -Force
        $cleanup = "Disposable swap image removed."
    }
    elseif (Test-Path -LiteralPath $imagePath -PathType Leaf) {
        $cleanup = "Disposable swap image kept by -KeepImage."
    }
}

$report = [pscustomobject]@{
    schema_version = 1
    gate_id = "external.linux-swap-format-validation"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    distro_name = $DistroName
    image_path = $imagePath
    linux_image_path = $linuxPath
    image_size_bytes = [string]$ImageSizeBytes
    image_sha256 = $imageHash
    header = $header
    probe_report_path = $probePath
    probe = $probe
    blkid_output = if ($blkid) { $blkid.output } else { "" }
    swaplabel_output = if ($swaplabel) { $swaplabel.output } else { "" }
    commands = @($Script:Commands.ToArray())
    cleanup = $cleanup
    error = $errorText
}
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Linux swap format validation failed. Report: $ReportPath`n$errorText"
}

Write-Host "Linux swap format validation passed."
Write-Host "Report: $ReportPath"
