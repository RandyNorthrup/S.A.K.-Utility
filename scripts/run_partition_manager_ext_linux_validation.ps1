<#
.SYNOPSIS
    Validates S.A.K.'s bundled ext tools against Linux e2fsprogs and loop mount.

.DESCRIPTION
    Creates a disposable ext image with the bundled Windows e2fsprogs tools,
    grows and shrinks it with bundled resize2fs, validates it from a Linux environment, then
    writes a small Linux-side fixture through a loop mount and rechecks the image
    from both Linux and Windows. This is certification evidence only; the app
    never depends on WSL, Linux ISOs, or Linux runtime tools.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ".",
    [string]$EvidenceRoot = "artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-linux-validation",
    [string]$ReportPath = "",
    [string]$DistroName = "archlinux",
    [string]$FileSystem = "ext4",
    [uint64]$InitialSizeBytes = 64MB,
    [uint64]$GrownSizeBytes = 96MB,
    [uint64]$ShrunkSizeBytes = 80MB,
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

function Get-ToolRecord {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$TargetFileSystem
    )

    $matches = @($Manifest.tools | Where-Object {
        $_.id -eq $ToolId -and
        @($_.operations) -contains $Operation -and
        @($_.file_systems) -contains $TargetFileSystem
    })
    if ($matches.Count -ne 1) {
        throw "Manifest does not approve exactly one $ToolId tool for $Operation/$TargetFileSystem."
    }
    return $matches[0]
}

function Resolve-ApprovedTool {
    param(
        [Parameter(Mandatory = $true)] [object]$Manifest,
        [Parameter(Mandatory = $true)] [string]$ToolId,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [string]$TargetFileSystem
    )

    $tool = Get-ToolRecord -Manifest $Manifest `
        -ToolId $ToolId `
        -Operation $Operation `
        -TargetFileSystem $TargetFileSystem
    $relativePath = [string]$tool.relative_path
    $path = Join-Path (Join-Path $ProjectRoot "tools\filesystem") $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $ProjectRoot ("build\Release\tools\filesystem\" + $relativePath)
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Approved tool not found: $relativePath"
    }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne ([string]$tool.binary_sha256).ToLowerInvariant()) {
        throw "Approved tool hash mismatch for $ToolId."
    }
    return [pscustomobject]@{
        id = $tool.id
        path = (Resolve-Path -LiteralPath $path).Path
        expected_sha256 = ([string]$tool.binary_sha256).ToLowerInvariant()
        actual_sha256 = $actualHash
    }
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

if (@("ext2", "ext3", "ext4") -notcontains $FileSystem.ToLowerInvariant()) {
    throw "FileSystem must be ext2, ext3, or ext4."
}
if ($GrownSizeBytes -le $InitialSizeBytes) {
    throw "GrownSizeBytes must be larger than InitialSizeBytes."
}
if ($ShrunkSizeBytes -le $InitialSizeBytes -or $ShrunkSizeBytes -ge $GrownSizeBytes) {
    throw "ShrunkSizeBytes must be between InitialSizeBytes and GrownSizeBytes."
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
$imagePath = Join-Path $runRoot "sak-ext-linux-validation.img"
$linuxImagePath = ""
$toolHashes = @()
$imageHashAfterWindows = ""
$imageHashAfterLinux = ""
$imageCleanup = ""

try {
    $manifestPath = Join-Path $ProjectRoot "tools\filesystem\manifest.json"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $mke2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "mke2fs" -Operation "format" -TargetFileSystem $FileSystem
    $e2fsck = Resolve-ApprovedTool -Manifest $manifest -ToolId "e2fsck" -Operation "repair" -TargetFileSystem $FileSystem
    $resize2fs = Resolve-ApprovedTool -Manifest $manifest -ToolId "resize2fs" -Operation "resize" -TargetFileSystem $FileSystem
    $toolHashes = @($mke2fs, $e2fsck, $resize2fs)

    Invoke-RecordedCommand -Name "wsl-uname" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "uname", "-a") | Out-Null
    Invoke-WslScript -Name "linux-tool-preflight" -Script @"
set -euo pipefail
command -v e2fsck
command -v dumpe2fs
command -v debugfs
command -v mount
id
"@ | Out-Null

    Invoke-RecordedCommand -Name "create-disposable-image" `
        -FilePath "fsutil.exe" `
        -Arguments @("file", "createnew", $imagePath, [string]$InitialSizeBytes) | Out-Null
    Invoke-RecordedCommand -Name "mke2fs-format-$FileSystem" `
        -FilePath $mke2fs.path `
        -Arguments @("-q", "-t", $FileSystem, "-F", "-L", "SAKLINUX", $imagePath) | Out-Null
    Invoke-RecordedCommand -Name "windows-e2fsck-after-format-$FileSystem" `
        -FilePath $e2fsck.path `
        -Arguments @("-n", "-f", $imagePath) | Out-Null
    Invoke-RecordedCommand -Name "grow-disposable-image" `
        -FilePath "fsutil.exe" `
        -Arguments @("file", "seteof", $imagePath, [string]$GrownSizeBytes) | Out-Null
    Invoke-RecordedCommand -Name "windows-resize2fs-grow-$FileSystem" `
        -FilePath $resize2fs.path `
        -Arguments @($imagePath) | Out-Null
    Invoke-RecordedCommand -Name "windows-e2fsck-after-grow-$FileSystem" `
        -FilePath $e2fsck.path `
        -Arguments @("-n", "-f", $imagePath) | Out-Null
    $shrunkKilobytes = [uint64][Math]::Floor([double]$ShrunkSizeBytes / 1KB)
    Invoke-RecordedCommand -Name "windows-e2fsck-readonly-before-shrink-$FileSystem" `
        -FilePath $e2fsck.path `
        -Arguments @("-n", "-f", $imagePath) | Out-Null
    Invoke-RecordedCommand -Name "windows-resize2fs-shrink-$FileSystem" `
        -FilePath $resize2fs.path `
        -Arguments @("-p", $imagePath, "$($shrunkKilobytes)K") | Out-Null
    Invoke-RecordedCommand -Name "shrink-disposable-image-container" `
        -FilePath "fsutil.exe" `
        -Arguments @("file", "seteof", $imagePath, [string]$ShrunkSizeBytes) | Out-Null
    Invoke-RecordedCommand -Name "windows-e2fsck-after-shrink-$FileSystem" `
        -FilePath $e2fsck.path `
        -Arguments @("-n", "-f", $imagePath) | Out-Null

    $imageHashAfterWindows = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $wslPathOutput = Invoke-RecordedCommand -Name "wslpath-image" `
        -FilePath "wsl.exe" `
        -Arguments @("-d", $DistroName, "--", "wslpath", "-a", ($imagePath -replace "\\", "/"))
    $linuxImagePath = $wslPathOutput.output.Trim()

    $linuxValidationScript = @"
set -euo pipefail
img='$linuxImagePath'
mnt=/mnt/sak-ext-linux-validation-`$`$
cleanup() {
  if mountpoint -q "`$mnt"; then umount "`$mnt"; fi
  rmdir "`$mnt" 2>/dev/null || true
}
trap cleanup EXIT
echo "linux-uname=`$(uname -a)"
e2fsck -f -n "`$img"
dumpe2fs -h "`$img" 2>/dev/null | grep -E 'Filesystem volume name|Filesystem state|Filesystem features|Block count|Block size'
mkdir -p "`$mnt"
mount -o loop,rw "`$img" "`$mnt"
printf 'SAK Linux validation fixture\n' > "`$mnt/sak-linux-proof.txt"
sync
test -s "`$mnt/sak-linux-proof.txt"
cat "`$mnt/sak-linux-proof.txt"
umount "`$mnt"
rmdir "`$mnt"
e2fsck -f -n "`$img"
debugfs -R 'stat /sak-linux-proof.txt' "`$img" 2>/dev/null
"@
    Invoke-WslScript -Name "linux-e2fsck-dumpe2fs-loopmount-write-$FileSystem" `
        -Script $linuxValidationScript | Out-Null
    $imageHashAfterLinux = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Invoke-RecordedCommand -Name "windows-e2fsck-after-linux-write-$FileSystem" `
        -FilePath $e2fsck.path `
        -Arguments @("-n", "-f", $imagePath) | Out-Null

    $status = "Passed"
}
catch {
    $status = "Failed"
    $errorText = ConvertTo-PlainText -Value @($_)
}
finally {
    if ((Test-Path -LiteralPath $imagePath -PathType Leaf) -and -not $KeepImage) {
        Remove-Item -LiteralPath $imagePath -Force
        $imageCleanup = "Removed disposable validation image."
    }
    elseif (Test-Path -LiteralPath $imagePath -PathType Leaf) {
        $imageCleanup = "Kept disposable validation image by -KeepImage."
    }
}

$report = [pscustomobject]@{
    schema_version = 1
    gate_id = "external.ext-linux-validation"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    distro_name = $DistroName
    file_system = $FileSystem
    image_path = $imagePath
    linux_image_path = $linuxImagePath
    initial_size_bytes = $InitialSizeBytes
    grown_size_bytes = $GrownSizeBytes
    shrunk_size_bytes = $ShrunkSizeBytes
    image_sha256_after_windows_tools = $imageHashAfterWindows
    image_sha256_after_linux_write = $imageHashAfterLinux
    image_cleanup = $imageCleanup
    tool_hashes = $toolHashes
    commands = @($Script:Commands.ToArray())
    error = $errorText
}
$reportJson = ConvertTo-RedactedReportJson -Report $report
$reportJson | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$reportJson | Set-Content -LiteralPath (Join-Path $runRoot "report.json") -Encoding UTF8

if ($status -ne "Passed") {
    throw "Linux ext validation failed. Report: $ReportPath`n$errorText"
}

Write-Host "Linux ext validation passed: $FileSystem via WSL distro $DistroName"
Write-Host "Report: $ReportPath"
