<#
.SYNOPSIS
    Validates bundled HFS+/HFSX format and repair tools on disposable images.

.DESCRIPTION
    Non-mutating release gate for the Partition Manager HFS tool bundle. The
    script creates disposable image files, formats them with bundled newfs_hfs,
    checks/repairs them with bundled fsck_hfs, then verifies S.A.K. raw HFS
    detection and consistency reporting with partition_filesystem_probe_certifier.
#>

param(
    [string]$ProjectRoot = ".",
    [string]$OutputRoot = "artifacts\partition-manager-certification\tool-tests\hfsprogs-image-validation",
    [int64]$ImageSizeBytes = 67108864,
    [switch]$KeepImages
)

$ErrorActionPreference = "Stop"

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function ConvertTo-ProjectRelativePath([string]$Path, [string]$Root) {
    $resolvedPath = if (Test-Path -LiteralPath $Path) {
        (Resolve-Path -LiteralPath $Path).Path
    } else {
        [System.IO.Path]::GetFullPath($Path)
    }
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    if ($resolvedPath.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $resolvedPath.Substring($resolvedRoot.Length).TrimStart('\', '/').Replace('\', '/')
    }
    return $resolvedPath
}

function New-ImageFile([string]$Path, [int64]$LengthBytes) {
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $stream.SetLength($LengthBytes)
    }
    finally {
        $stream.Dispose()
    }
}

function Resolve-ToolPath([string]$Root, [string]$RelativePath) {
    $bundleRoot = Join-Path $Root "tools\filesystem"
    $sourcePath = Join-Path $bundleRoot $RelativePath
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $sourcePath).Path
    }
    # Fail closed: a bundle that EXISTS but is missing this tool must never silently downgrade
    # to a build-output copy -- the gate would then certify a different artifact than the one
    # that ships. The build\Release location stays available only for a tree with no bundle yet.
    if (Test-Path -LiteralPath $bundleRoot -PathType Container) {
        throw "Bundled filesystem tool missing from tools\filesystem: $RelativePath"
    }
    $buildPath = Join-Path (Join-Path $Root "build\Release\tools\filesystem") $RelativePath
    if (Test-Path -LiteralPath $buildPath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $buildPath).Path
    }
    throw "Bundled filesystem tool missing: $RelativePath"
}

function Resolve-ProbeCertifier([string]$Root) {
    $candidate = Join-Path $Root "build\Release\partition_filesystem_probe_certifier.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }
    throw "partition_filesystem_probe_certifier.exe was not found. Build target partition_filesystem_probe_certifier first."
}

function ConvertTo-WindowsProcessArgument([string]$Argument) {
    if ($null -eq $Argument) {
        return '""'
    }
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($char in $Argument.ToCharArray()) {
        if ($char -eq '\') {
            $backslashes += 1
            continue
        }
        if ($char -eq '"') {
            [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($char)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-ProcessArguments([string[]]$Arguments) {
    return ($Arguments | ForEach-Object { ConvertTo-WindowsProcessArgument -Argument $_ }) -join ' '
}

function Invoke-CapturedProcess([string]$FilePath, [string[]]$Arguments, [int[]]$AllowedExitCodes) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = Join-ProcessArguments -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $result = [ordered]@{
        file = $FilePath
        args = $Arguments
        exit_code = $process.ExitCode
        stdout = $stdout.Trim()
        stderr = $stderr.Trim()
    }
    Assert-Condition -Condition ($AllowedExitCodes -contains $process.ExitCode) `
        -Message "Command failed ($($process.ExitCode)): $FilePath $($Arguments -join ' ')`n$stderr"
    return $result
}

function Read-Json([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Invoke-HfsImageCase(
    [string]$CaseName,
    [string]$ExpectedFileSystem,
    [string]$ImagePath,
    [string]$ReportPath,
    [string]$NewfsHfs,
    [string]$FsckHfs,
    [string]$ProbeCertifier,
    [int64]$ImageSizeBytes
) {
    New-ImageFile -Path $ImagePath -LengthBytes $ImageSizeBytes
    $label = if ($ExpectedFileSystem -eq "HFSX") { "SAK_HFSX_IMG" } else { "SAK_HFS_IMG" }
    $newfsArgs = @()
    if ($ExpectedFileSystem -eq "HFSX") {
        $newfsArgs += "-s"
    }
    $newfsArgs += @("-v", $label, $ImagePath)

    $case = [ordered]@{
        case = $CaseName
        expected_file_system = $ExpectedFileSystem
        image_path = $ImagePath
        image_size_bytes = $ImageSizeBytes
        format = Invoke-CapturedProcess -FilePath $NewfsHfs -Arguments $newfsArgs -AllowedExitCodes @(0)
        check_read_only = $null
        repair = $null
        repair_retry = $null
        post_repair_check_read_only = $null
        probe = $null
        detected_file_system = $null
        status = "Failed"
    }

    $initialAllowedExitCodes = if ($ExpectedFileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
    $case.check_read_only = Invoke-CapturedProcess -FilePath $FsckHfs -Arguments @("-n", "-f", $ImagePath) -AllowedExitCodes $initialAllowedExitCodes
    $repairAllowedExitCodes = if ($ExpectedFileSystem -eq "HFSX") { @(0, 8) } else { @(0) }
    $case.repair = Invoke-CapturedProcess -FilePath $FsckHfs -Arguments @("-p", "-f", $ImagePath) -AllowedExitCodes $repairAllowedExitCodes
    $case.post_repair_check_read_only = Invoke-CapturedProcess -FilePath $FsckHfs -Arguments @("-n", "-f", $ImagePath) -AllowedExitCodes $initialAllowedExitCodes
    if ($ExpectedFileSystem -eq "HFSX" -and $case.post_repair_check_read_only.exit_code -ne 0) {
        $case.repair_retry = Invoke-CapturedProcess -FilePath $FsckHfs -Arguments @("-p", "-f", $ImagePath) -AllowedExitCodes @(0)
        $case.post_repair_check_read_only = Invoke-CapturedProcess -FilePath $FsckHfs -Arguments @("-n", "-f", $ImagePath) -AllowedExitCodes @(0)
    }
    # State the invariant instead of leaving it to the allowed-exit-code tables above: the
    # HFSX tolerance of exit 8 covers the KNOWN newfs quirk on the earlier steps only, and a
    # case may never be marked Passed unless the final read-only check came back clean.
    Assert-Condition -Condition ($case.post_repair_check_read_only.exit_code -eq 0) `
        -Message "$CaseName post-repair consistency check did not reach a clean exit code"
    $case.probe = Invoke-CapturedProcess -FilePath $ProbeCertifier -Arguments @(
        "--input", $ImagePath,
        "--output", $ReportPath,
        "--expect", $ExpectedFileSystem,
        "--hfs-check"
    ) -AllowedExitCodes @(0)

    $probeReport = Read-Json -Path $ReportPath
    # Fail closed on the EXACT string. A PowerShell collection comparison such as
    # @("HFS+","bogus") -eq "HFS+" returns the matching element, which [bool]$Condition then
    # coerces to $true -- so malformed or wrong-typed probe evidence would satisfy this gate.
    Assert-Condition -Condition (($probeReport.detected_file_system -is [string]) -and ($probeReport.detected_file_system -ceq $ExpectedFileSystem)) `
        -Message "$CaseName probe detected '$($probeReport.detected_file_system)', expected '$ExpectedFileSystem'"
    Assert-Condition -Condition (($null -ne $probeReport.hfs_check) -and ($probeReport.hfs_check.status -is [string]) -and ($probeReport.hfs_check.status -ceq "Passed")) `
        -Message "$CaseName HFS consistency proof did not pass"

    $case.detected_file_system = $probeReport.detected_file_system
    $case.status = "Passed"
    return $case
}

$repo = (Resolve-Path -LiteralPath $ProjectRoot).Path
# Fail closed: pin the tool tree to THIS script's own repository. Without it a caller-supplied
# -ProjectRoot could name any directory that merely mimics the layout, and this gate would run
# -- and then hash-attest -- three attacker-chosen executables.
Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($PSCommandPath)) `
    -Message "Cannot verify the project root: this script's own path is unknown."
$repoPrefix = $repo.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
Assert-Condition -Condition ($PSCommandPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) `
    -Message "ProjectRoot '$repo' does not contain this script; refusing to run tools from an unrelated tree."

# The output root is deleted -Recurse -Force below, so it must canonically be a STRICT
# descendant of the project root: "." or ".." would otherwise delete the repository or its
# parent, and a junction here would delete through the link.
$outputRootPath = [System.IO.Path]::GetFullPath((Join-Path $repo $OutputRoot))
Assert-Condition -Condition ($outputRootPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) `
    -Message "OutputRoot must stay inside the project root: $OutputRoot"
if (Test-Path -LiteralPath $outputRootPath) {
    $existingOutputRoot = Get-Item -LiteralPath $outputRootPath -Force
    Assert-Condition -Condition ($existingOutputRoot.PSIsContainer) `
        -Message "OutputRoot exists and is not a directory: $outputRootPath"
    Assert-Condition -Condition (-not ($existingOutputRoot.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) `
        -Message "OutputRoot is a reparse point; refusing to delete through a link: $outputRootPath"
    Remove-Item -LiteralPath $outputRootPath -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outputRootPath | Out-Null

$newfsHfs = Resolve-ToolPath -Root $repo -RelativePath "hfsprogs\newfs_hfs.exe"
$fsckHfs = Resolve-ToolPath -Root $repo -RelativePath "hfsprogs\fsck_hfs.exe"
$probeCertifier = Resolve-ProbeCertifier -Root $repo

$report = [ordered]@{
    schema_version = 1
    gate_id = "tool.hfsprogs-image-validation"
    created_utc = (Get-Date).ToUniversalTime().ToString("o")
    project_root = $repo
    output_root = ConvertTo-ProjectRelativePath -Path $outputRootPath -Root $repo
    tools = [ordered]@{
        newfs_hfs = [ordered]@{
            path = ConvertTo-ProjectRelativePath -Path $newfsHfs -Root $repo
            sha256 = (Get-FileHash -LiteralPath $newfsHfs -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        fsck_hfs = [ordered]@{
            path = ConvertTo-ProjectRelativePath -Path $fsckHfs -Root $repo
            sha256 = (Get-FileHash -LiteralPath $fsckHfs -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        partition_filesystem_probe_certifier = [ordered]@{
            path = ConvertTo-ProjectRelativePath -Path $probeCertifier -Root $repo
            sha256 = (Get-FileHash -LiteralPath $probeCertifier -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    cases = @()
    status = "Failed"
}

$cases = @(
    @{ name = "hfsplus"; expected = "HFS+"; image = "hfsplus.img"; probe = "hfsplus.probe.json" },
    @{ name = "hfsx"; expected = "HFSX"; image = "hfsx.img"; probe = "hfsx.probe.json" }
)

try {
    foreach ($case in $cases) {
        $imagePath = Join-Path $outputRootPath $case.image
        $probePath = Join-Path $outputRootPath $case.probe
        $caseReport = Invoke-HfsImageCase `
            -CaseName $case.name `
            -ExpectedFileSystem $case.expected `
            -ImagePath $imagePath `
            -ReportPath $probePath `
            -NewfsHfs $newfsHfs `
            -FsckHfs $fsckHfs `
            -ProbeCertifier $probeCertifier `
            -ImageSizeBytes $ImageSizeBytes
        $caseReport.image_path = ConvertTo-ProjectRelativePath -Path $imagePath -Root $repo
        $caseReport.probe_report_path = ConvertTo-ProjectRelativePath -Path $probePath -Root $repo
        $report.cases += [pscustomobject]$caseReport
    }
    $report.status = "Passed"
}
finally {
    if (-not $KeepImages) {
        foreach ($case in $cases) {
            $imagePath = Join-Path $outputRootPath $case.image
            if (Test-Path -LiteralPath $imagePath -PathType Leaf) {
                Remove-Item -LiteralPath $imagePath -Force
            }
        }
    }
    $reportPath = Join-Path $outputRootPath "report.json"
    $report.report_path = ConvertTo-ProjectRelativePath -Path $reportPath -Root $repo
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}

Assert-Condition -Condition ($report.status -eq "Passed") -Message "HFS image validation failed: $reportPath"
Write-Host "HFS image validation passed: $reportPath"
