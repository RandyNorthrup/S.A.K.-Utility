<#
.SYNOPSIS
    Asserts that every tool the quality gates depend on is present, and records its version.

.DESCRIPTION
    A gate that cannot run is not a gate. Before this preflight existed, ripgrep was absent
    from the development machine and three gates (blocking patterns, accessibility patterns,
    logged message boxes) could not execute at all. Each of those scripts fails closed on its
    own, so nothing was silently passing, but the absence went unreported until someone
    happened to run them by hand.

    This script makes a missing tool an immediate, explicit failure and prints the resolved
    path and version of every tool so CI logs carry the toolchain used for that run.

    Fails closed: any missing required tool exits 1.

.PARAMETER IncludeOptional
    Also report optional tools. Missing optional tools are reported but do not fail.
#>

param(
    [switch]$IncludeOptional
)

$ErrorActionPreference = "Stop"

# Required: a gate depends on each of these and cannot run without it.
$RequiredTools = @(
    @{ Name = "rg"; Purpose = "blocking patterns, accessibility, logged message boxes"; VersionArgs = @("--version") },
    @{ Name = "python"; Purpose = "magic-number and lizard complexity gates"; VersionArgs = @("--version") },
    @{ Name = "clang-format"; Purpose = "formatting gate"; VersionArgs = @("--version") },
    @{ Name = "cppcheck"; Purpose = "static analysis gate"; VersionArgs = @("--version") },
    @{ Name = "cmake"; Purpose = "build and test gates"; VersionArgs = @("--version") },
    @{ Name = "git"; Purpose = "repository gates"; VersionArgs = @("--version") },
    # The browser-control service worker is ~3300 lines of privileged JavaScript that no
    # C/C++ gate can see. node runs its unit suite and the lizard JavaScript pass, so
    # without it that whole surface silently returns to being ungated (R5-G21-9).
    @{ Name = "node"; Purpose = "browser extension unit tests and JavaScript complexity gate"; VersionArgs = @("--version") }
)

# Optional: used by deeper analysis that is not yet part of every run.
$OptionalTools = @(
    @{ Name = "clang-tidy"; Purpose = "clang-tidy analysis gate"; VersionArgs = @("--version") },
    @{ Name = "ninja"; Purpose = "compile_commands.json generation for clang-tidy"; VersionArgs = @("--version") }
)

function Resolve-Tool {
    param([string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    # Fall back to well-known install locations that are not always on PATH.
    $candidates = @(
        "$env:ProgramFiles\LLVM\bin\$Name.exe",
        "$env:ProgramFiles\Cppcheck\$Name.exe",
        "$env:ProgramFiles\CMake\bin\$Name.exe",
        (Join-Path $PSScriptRoot "..\.venv\Scripts\$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Get-ToolVersion {
    param([string]$Path, [string[]]$VersionArgs)

    try {
        $raw = & $Path @VersionArgs 2>&1 | Out-String
        $first = ($raw -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
        return $first.Trim()
    } catch {
        return "(version query failed)"
    }
}

$missing = @()

Write-Host "Quality-gate toolchain preflight" -ForegroundColor Cyan
Write-Host ""

foreach ($tool in $RequiredTools) {
    $path = Resolve-Tool -Name $tool.Name
    if (-not $path) {
        $missing += $tool
        Write-Host ("  MISSING   {0,-14} needed by: {1}" -f $tool.Name, $tool.Purpose) -ForegroundColor Red
        continue
    }
    $version = Get-ToolVersion -Path $path -VersionArgs $tool.VersionArgs
    Write-Host ("  OK        {0,-14} {1}" -f $tool.Name, $version) -ForegroundColor Green
    Write-Host ("            {0}" -f $path) -ForegroundColor DarkGray
}

if ($IncludeOptional) {
    Write-Host ""
    Write-Host "Optional tools:" -ForegroundColor Cyan
    foreach ($tool in $OptionalTools) {
        $path = Resolve-Tool -Name $tool.Name
        if (-not $path) {
            Write-Host ("  absent    {0,-14} would enable: {1}" -f $tool.Name, $tool.Purpose) -ForegroundColor Yellow
            continue
        }
        $version = Get-ToolVersion -Path $path -VersionArgs $tool.VersionArgs
        Write-Host ("  OK        {0,-14} {1}" -f $tool.Name, $version) -ForegroundColor Green
    }
}

Write-Host ""
if ($missing.Count -gt 0) {
    $names = ($missing | ForEach-Object { $_.Name }) -join ", "
    Write-Error "Quality-gate toolchain incomplete. Missing required tool(s): $names"
    exit 1
}

Write-Host "PASSED: every required gate tool is present." -ForegroundColor Green
exit 0
