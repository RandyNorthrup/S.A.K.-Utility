<#
.SYNOPSIS
    Verifies strict explicit GUI accessibility coverage.
#>

param(
    [string]$Root = ".",
    [int]$MinimumExplicitAccessors = 300,
    [string]$ExePath = "",
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    $themePath = "src/gui/windows11_theme.cpp"
    $theme = Get-Content -LiteralPath $themePath -Raw
    foreach ($forbidden in @(
            "applyAccessibility",
            "inferAccessibleName",
            "sakAccessibilityApplied",
            "inferTooltip",
            "sakTooltipsApplied")) {
        if ($theme -match [regex]::Escape($forbidden)) {
            throw "Accessibility fallback/auto-fill token forbidden in ${themePath}: ${forbidden}"
        }
    }

    $matches = & rg --count-matches "setAccessible(Name|Description)?\s*\(|setAccessible\s*\(" src/gui include/sak
    $count = 0
    foreach ($line in $matches) {
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

    $runId = [System.Guid]::NewGuid().ToString("N")
    $auditPath = Join-Path ([System.IO.Path]::GetTempPath()) "sak_accessibility_audit_${runId}.txt"
    $stdoutPath = Join-Path ([System.IO.Path]::GetTempPath()) "sak_accessibility_audit_${runId}.stdout.txt"
    $stderrPath = Join-Path ([System.IO.Path]::GetTempPath()) "sak_accessibility_audit_${runId}.stderr.txt"
    Remove-Item -LiteralPath $auditPath -Force -ErrorAction SilentlyContinue

    $proc = Start-Process -FilePath $ExePath `
        -ArgumentList @("--accessibility-audit", "--no-splash", "--accessibility-audit-output=$auditPath") `
        -PassThru `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -WorkingDirectory $repo
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        $pidText = $proc.Id
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        $stdoutOutput = if (Test-Path -LiteralPath $stdoutPath) {
            Get-Content -LiteralPath $stdoutPath -Tail 80
        } else {
            @()
        }
        $stderrOutput = if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath -Tail 80
        } else {
            @()
        }
        $auditOutput = if (Test-Path -LiteralPath $auditPath) {
            Get-Content -LiteralPath $auditPath -Tail 80
        } else {
            @("No audit output written.")
        }
        throw "Accessibility runtime audit timed out after ${TimeoutSeconds}s (pid ${pidText}).`nAUDIT:`n$($auditOutput -join "`n")`nSTDOUT:`n$($stdoutOutput -join "`n")`nSTDERR:`n$($stderrOutput -join "`n")"
    }
    $proc.Refresh()
    $auditOutput = if (Test-Path -LiteralPath $auditPath) {
        Get-Content -LiteralPath $auditPath
    } else {
        $stdoutOutput = if (Test-Path -LiteralPath $stdoutPath) {
            Get-Content -LiteralPath $stdoutPath -Tail 80
        } else {
            @()
        }
        $stderrOutput = if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath -Tail 80
        } else {
            @()
        }
        @("No audit output written.", "STDOUT:", $stdoutOutput, "STDERR:", $stderrOutput)
    }
    $auditHeader = ($auditOutput | Select-Object -First 1)
    if ($auditHeader -notmatch '^SAK_ACCESSIBILITY_AUDIT_OK\b') {
        $exitCode = if ($null -eq $proc.ExitCode) { "unknown" } else { $proc.ExitCode }
        throw "Accessibility runtime audit failed with exit code ${exitCode}:`n$($auditOutput -join "`n")"
    }

    Write-Host "Accessibility pattern check passed (${count} explicit accessors, no fallback)."
}
finally {
    Pop-Location
}
