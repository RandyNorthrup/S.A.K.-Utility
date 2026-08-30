<#
.SYNOPSIS
    Verifies strict explicit GUI accessibility coverage.
#>

param(
    [string]$Root = ".",
    [ValidateRange(300, 2147483647)]
    [int]$MinimumExplicitAccessors = 300,
    [string]$ExePath = "",
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"

$processCaptureScript = Join-Path $PSScriptRoot "process_capture.ps1"
if (-not (Test-Path -LiteralPath $processCaptureScript -PathType Leaf)) {
    throw "Captured-process helper not found: $processCaptureScript"
}
. $processCaptureScript

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
    # than the newest audited source proves nothing about the current tree.
    $newestSource = $scanFiles | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($exeItem.LastWriteTimeUtc -lt $newestSource.LastWriteTimeUtc) {
        throw "Accessibility runtime audit executable ${ExePath} is older than $($newestSource.FullName); rebuild before running this gate."
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

    $auditProcess = Invoke-CapturedProcess -FilePath $ExePath -Arguments @(
        "--accessibility-audit",
        "--no-splash",
        "--accessibility-audit-output=$auditPath"
    ) -WorkingDirectory $repo -TimeoutSeconds $TimeoutSeconds
    if ($auditProcess.timed_out) {
        $stdoutOutput = @($auditProcess.stdout -split '\r?\n' | Select-Object -Last 80)
        $stderrOutput = @($auditProcess.stderr -split '\r?\n' | Select-Object -Last 80)
        $auditOutput = if (Test-Path -LiteralPath $auditPath) {
            Get-Content -LiteralPath $auditPath -Tail 80
        } else {
            @("No audit output written.")
        }
        throw "Accessibility runtime audit timed out after ${TimeoutSeconds}s.`nAUDIT:`n$($auditOutput -join "`n")`nSTDOUT:`n$($stdoutOutput -join "`n")`nSTDERR:`n$($stderrOutput -join "`n")"
    }
    $auditOutput = if (Test-Path -LiteralPath $auditPath) {
        Get-Content -LiteralPath $auditPath
    } else {
        $stdoutOutput = @($auditProcess.stdout -split '\r?\n' | Select-Object -Last 80)
        $stderrOutput = @($auditProcess.stderr -split '\r?\n' | Select-Object -Last 80)
        @("No audit output written.", "STDOUT:", $stdoutOutput, "STDERR:", $stderrOutput)
    }
    $exitCode = $auditProcess.exit_code
    $auditHeader = ($auditOutput | Select-Object -First 1)
    if ($auditHeader -notmatch '^SAK_ACCESSIBILITY_AUDIT_OK\b') {
        throw "Accessibility runtime audit failed with exit code ${exitCode}:`n$($auditOutput -join "`n")"
    }
    # An OK header is not sufficient on its own: the process must also have finished cleanly
    # and must not have reported a failure anywhere later in the same audit output.
    if ($exitCode -ne 0) {
        throw "Accessibility runtime audit printed OK but exited with code ${exitCode}:`n$($auditOutput -join "`n")"
    }
    if (@($auditOutput | Select-String -SimpleMatch -Pattern "SAK_ACCESSIBILITY_AUDIT_FAILED").Count -gt 0) {
        throw "Accessibility runtime audit reported a failure after its OK header:`n$($auditOutput -join "`n")"
    }

    Write-Host "Accessibility pattern check passed (${count} explicit accessors, no fallback)."
}
finally {
    Pop-Location
}
