<#
.SYNOPSIS
    Verifies strict explicit GUI accessibility coverage.
#>

param(
    [string]$Root = ".",
    [ValidateRange(300, 2147483647)]
    [int]$MinimumExplicitAccessors = 300,
    [string]$ExePath = "",
    # The package that ships (a clean extract of the release zip). The audit then runs a copy of it,
    # in a folder that holds the package and nothing else (New-PackageAuditCopy says why).
    [string]$PackageRoot = "",
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"

$processCaptureScript = Join-Path $PSScriptRoot "process_capture.ps1"
if (-not (Test-Path -LiteralPath $processCaptureScript -PathType Leaf)) {
    throw "Captured-process helper not found: $processCaptureScript"
}
. $processCaptureScript

# What the audit left, as one flat list of lines: its own output, then the program's stdout and
# stderr. An array that held the stdout and stderr arrays printed "System.Object[]" for each, and
# the "Fatal: ..." line an elevated program writes to stderr when it refuses to start was lost (the
# v0.9.3-rc.1 release dry run, 2026-11-14).
function Get-AuditFailureLines([object]$Process, [string]$AuditPath, [int]$Tail) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $AuditPath) {
        foreach ($line in @(Get-Content -LiteralPath $AuditPath -Tail $Tail)) {
            $lines.Add([string]$line)
        }
    } else {
        $lines.Add("No audit output written.")
    }
    foreach ($stream in @("stdout", "stderr")) {
        $lines.Add("$($stream.ToUpperInvariant()):")
        foreach ($line in @(([string]$Process.$stream) -split '\r?\n' | Select-Object -Last $Tail)) {
            $lines.Add([string]$line)
        }
    }
    return , $lines.ToArray()
}

# build/Release holds the test programs, the release zip and the clean-*-extract folders beside the
# app. Running as administrator (a hosted runner always does), the app checks every program file in
# its folder and below, and refuses to start while one is not part of S.A.K., before it writes a
# line of the audit: the v0.9.3-rc.1 dry run failed so. The copy holds the package and nothing
# else, and the package itself gets no data folder from the run. Returns the new folder.
function New-PackageAuditCopy([string]$Package) {
    $copy = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_accessibility_package_" +
        [System.Guid]::NewGuid().ToString("N"))
    if (Test-Path -LiteralPath $copy) {
        throw "Accessibility audit package copy already exists: ${copy}"
    }
    New-Item -ItemType Directory -Path $copy | Out-Null
    foreach ($entry in @(Get-ChildItem -LiteralPath $Package -Force)) {
        if ($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "The package holds a link, which ships in no package: $($entry.FullName)"
        }
        Copy-Item -LiteralPath $entry.FullName -Destination $copy -Recurse -Force
    }
    return $copy
}

# A package's program was built after the commit it was built from. The checkout's file times say
# when it was checked out, which in the release workflow's verify job is after the build and the
# signing, so for a package the commit's own time is the bound. Returns it, in UTC.
function Get-HeadCommitTimeUtc([string]$Repository) {
    $seconds = @(& git -C $Repository log -1 --format=%ct HEAD)
    if ($LASTEXITCODE -ne 0 -or $seconds.Count -ne 1 -or $seconds[0] -notmatch '^\d+$') {
        throw "Could not read HEAD's commit time in ${Repository} (git exit $LASTEXITCODE)"
    }
    return [System.DateTimeOffset]::FromUnixTimeSeconds([int64]$seconds[0]).UtcDateTime
}

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    # The forbidden auto-fill/fallback tokens are checked across the whole GUI surface,
    # not only in the theme file: fallback logic moved into any other src/gui or
    # include/sak translation unit would otherwise pass a check that claims "no fallback".
    $themePath = "src/gui/windows11_theme.cpp"
    if (-not (Test-Path -LiteralPath $themePath -PathType Leaf)) {
        throw "Accessibility theme source not found: ${themePath}"
    }
    $scanRoots = @("src/gui", "include/sak")
    $scanFiles = @(Get-ChildItem -Path $scanRoots -Recurse -File -Include *.cpp, *.h)
    if ($scanFiles.Count -eq 0) {
        throw "Accessibility source scan found no files under: $($scanRoots -join ', ')"
    }
    foreach ($file in $scanFiles) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        foreach ($forbidden in @(
                "applyAccessibility",
                "inferAccessibleName",
                "sakAccessibilityApplied",
                "inferTooltip",
                "sakTooltipsApplied")) {
            if ($text -match [regex]::Escape($forbidden)) {
                throw "Accessibility fallback/auto-fill token forbidden in $($file.FullName): ${forbidden}"
            }
        }
    }

    # rg exit 0 means "matches found"; anything else (1 = no match, 2 = scan error) means the
    # count below would be partial or empty, so the gate must not grade against it.
    # Pin the native rg (R5-G21-4): an alias/function/script named rg could emit nothing and
    # leave a stale 0 in $LASTEXITCODE, passing the gate without ever searching.
    $rg = @(Get-Command rg -CommandType Application -ErrorAction SilentlyContinue)[0]
    if (-not $rg) {
        throw "Required tool missing: rg (native executable)"
    }
    $accessorMatches = & $rg.Source --count-matches "setAccessible(Name|Description)?\s*\(|setAccessible\s*\(" src/gui include/sak
    $rgExit = $LASTEXITCODE
    if ($rgExit -ne 0) {
        throw "Accessibility accessor scan failed: rg exited with ${rgExit}"
    }
    $count = 0
    foreach ($line in $accessorMatches) {
        $count += [int](($line -split ':')[-1])
    }
    if ($count -lt $MinimumExplicitAccessors) {
        throw "Accessibility accessor count ${count} below required minimum ${MinimumExplicitAccessors}"
    }

    # A press that opens a dialog must return first, or a screen reader cannot read the dialog.
    & python (Join-Path $PSScriptRoot "check_dialog_buttons_deferred.py") --root $repo
    if ($LASTEXITCODE -ne 0) {
        throw "A button or menu action opens a dialog inside its press"
    }

    $fromPackage = -not [string]::IsNullOrWhiteSpace($PackageRoot)
    if ($fromPackage -and -not [string]::IsNullOrWhiteSpace($ExePath)) {
        throw "Give -ExePath or -PackageRoot, not both: the audit runs one program."
    }
    if ($fromPackage) {
        if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
            throw "Accessibility runtime audit package not found: ${PackageRoot}"
        }
        $ExePath = Join-Path (Resolve-Path -LiteralPath $PackageRoot).Path "sak_utility.exe"
    }
    if ([string]::IsNullOrWhiteSpace($ExePath)) {
        $ExePath = Join-Path $repo "build/Release/sak_utility.exe"
    }
    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        throw "Accessibility runtime audit executable not found: ${ExePath}"
    }
    $exeItem = Get-Item -LiteralPath $ExePath -Force
    if ($exeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Accessibility runtime audit executable is a reparse point: ${ExePath}"
    }
    $ExePath = $exeItem.FullName
    if ($ExePath.StartsWith("\\")) {
        throw "Accessibility runtime audit executable must be a local path: ${ExePath}"
    }
    # Bind the runtime evidence to the source that was just inspected: an executable older
    # than the newest audited source proves nothing about the current tree. A package's program is
    # bound to HEAD's commit instead (Get-HeadCommitTimeUtc says why).
    if ($fromPackage) {
        $commitTime = Get-HeadCommitTimeUtc $repo
        if ($exeItem.LastWriteTimeUtc -lt $commitTime) {
            throw "Accessibility runtime audit executable ${ExePath} is older than HEAD's commit ($($commitTime.ToString('o'))); it was not built from this checkout."
        }
    } else {
        $newestSource = $scanFiles | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($exeItem.LastWriteTimeUtc -lt $newestSource.LastWriteTimeUtc) {
            throw "Accessibility runtime audit executable ${ExePath} is older than $($newestSource.FullName); rebuild before running this gate."
        }
    }

    $runId = [System.Guid]::NewGuid().ToString("N")
    # Exclusive creation of a fresh scratch directory: anything already sitting at this path
    # was not written by this run, so its contents can never be read back as pass evidence.
    $auditDir = Join-Path ([System.IO.Path]::GetTempPath()) "sak_accessibility_audit_${runId}"
    if (Test-Path -LiteralPath $auditDir) {
        throw "Accessibility audit scratch directory already exists: ${auditDir}"
    }
    New-Item -ItemType Directory -Path $auditDir | Out-Null
    $auditPath = Join-Path $auditDir "audit.txt"

    $packageCopy = $null
    try {
        $auditExe = $ExePath
        if ($fromPackage) {
            $packageCopy = New-PackageAuditCopy (Split-Path -Parent $ExePath)
            $auditExe = Join-Path $packageCopy "sak_utility.exe"
            Write-Host "Accessibility runtime audit runs a copy of the package: ${packageCopy}"
        }
        $auditProcess = Invoke-CapturedProcess -FilePath $auditExe -Arguments @(
            "--accessibility-audit",
            "--no-splash",
            "--accessibility-audit-output=$auditPath"
        ) -WorkingDirectory $repo -TimeoutSeconds $TimeoutSeconds
    } finally {
        if ($null -ne $packageCopy) {
            Remove-Item -LiteralPath $packageCopy -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $packageCopy) {
                Write-Warning "Could not remove the audit's package copy: ${packageCopy}"
            }
        }
    }
    if ($auditProcess.timed_out) {
        $failure = Get-AuditFailureLines -Process $auditProcess -AuditPath $auditPath -Tail 80
        throw "Accessibility runtime audit timed out after ${TimeoutSeconds}s.`nAUDIT:`n$($failure -join "`n")"
    }
    # Assigned, not the output of an if: a one-line audit, as the app writes, came out of an if as a
    # string, and [0] of a string is its first character.
    $auditOutput = @()
    if (Test-Path -LiteralPath $auditPath) {
        $auditOutput = @(Get-Content -LiteralPath $auditPath)
    }
    $exitCode = $auditProcess.exit_code
    # A string, never "nothing": no line at all compared as an empty collection, which -notmatch
    # answers with nothing, and a program that wrote no audit would have passed this check.
    $auditHeader = if ($auditOutput.Count -gt 0) { [string]$auditOutput[0] } else { "" }
    if ($auditHeader -notmatch '^SAK_ACCESSIBILITY_AUDIT_OK\b') {
        $failure = Get-AuditFailureLines -Process $auditProcess -AuditPath $auditPath -Tail 80
        throw "Accessibility runtime audit failed with exit code ${exitCode}:`n$($failure -join "`n")"
    }
    # An OK header is not sufficient on its own: the process must also have finished cleanly
    # and must not have reported a failure anywhere later in the same audit output.
    if ($exitCode -ne 0) {
        $failure = Get-AuditFailureLines -Process $auditProcess -AuditPath $auditPath -Tail 80
        throw "Accessibility runtime audit printed OK but exited with code ${exitCode}:`n$($failure -join "`n")"
    }
    if (@($auditOutput | Select-String -SimpleMatch -Pattern "SAK_ACCESSIBILITY_AUDIT_FAILED").Count -gt 0) {
        throw "Accessibility runtime audit reported a failure after its OK header:`n$($auditOutput -join "`n")"
    }

    Write-Host "Accessibility pattern check passed (${count} explicit accessors, no fallback)."
}
finally {
    Pop-Location
}
