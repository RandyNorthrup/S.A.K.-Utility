# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Line-coverage measurement over the test suite using OpenCppCoverage (MSVC).
#
# WHY THIS EXISTS
# CODEX_REVIEW_5 MEASURED GAP 3: the suite had no coverage measurement of any kind, so
# "N tests pass" had an unknown denominator -- there was no evidence about what fraction of
# the raw-filesystem engines, the elevation boundary, or the AI tool policy the tests
# actually execute. This closes that: it runs a chosen set of tests under OpenCppCoverage
# and reports the real covered/total line counts, overall and per source subsystem.
#
# REQUIREMENTS
#   * OpenCppCoverage on PATH or at "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
#     (choco install opencppcoverage).
#   * A RelWithDebInfo build of the targeted tests (they carry the PDBs coverage needs;
#     the default Release build strips them). Build with:
#       cmake --build build --config RelWithDebInfo --target <test targets>
#   * Qt's bin directory (for the runtime DLLs) is added to PATH automatically below.
#
# USAGE
#   scripts/run_coverage.ps1 -TestRegex 'test_(pst|mbox|iso)_' -OutDir build/coverage
#
# It writes cobertura XML + an HTML report to -OutDir and prints the subsystem table. It is
# intentionally NOT a pre-commit hook: instrumented runs are far too slow for every commit.

[CmdletBinding()]
param(
    [string]$TestRegex = '.',
    [string]$Config = 'RelWithDebInfo',
    [string]$OutDir = 'build/coverage',
    [string]$QtBin = 'C:\Qt\6.10.3\msvc2022_64\bin'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-OpenCppCoverage {
    $onPath = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $fixed = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe'
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    throw "OpenCppCoverage not found. Install it: choco install opencppcoverage -y"
}

$occ = Resolve-OpenCppCoverage
$buildDir = Join-Path $RepoRoot 'build'
$testModuleDir = Join-Path $buildDir "tests\$Config"
if (-not (Test-Path -LiteralPath $testModuleDir)) {
    throw "No $Config test binaries at $testModuleDir. Build them first: " +
          "cmake --build build --config $Config --target <test targets>"
}

$absOut = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $absOut | Out-Null
$coberturaXml = Join-Path $absOut 'coverage.xml'
$htmlDir = Join-Path $absOut 'html'

# Qt runtime DLLs must resolve for the instrumented test binaries to start.
if (Test-Path -LiteralPath $QtBin) {
    $env:PATH = "$QtBin;$env:PATH"
}

Write-Host "Running coverage over tests matching /$TestRegex/ ($Config)..."
& $occ `
    --sources "$RepoRoot\src" `
    --sources "$RepoRoot\include" `
    --modules "$testModuleDir" `
    --export_type "cobertura:$coberturaXml" `
    --export_type "html:$htmlDir" `
    --cover_children `
    --quiet `
    -- ctest --test-dir "$buildDir" -C $Config -R $TestRegex --output-on-failure | Out-Null

if (-not (Test-Path -LiteralPath $coberturaXml)) {
    throw "Coverage run produced no report at $coberturaXml"
}

# Aggregate covered/total lines per top-level source subsystem from the cobertura report.
[xml]$report = Get-Content -LiteralPath $coberturaXml
$bySubsystem = @{}
$totalCovered = 0
$totalValid = 0

foreach ($class in $report.coverage.packages.package.classes.class) {
    $file = ($class.filename -replace '/', '\')
    $covered = 0
    $valid = 0
    foreach ($line in $class.lines.line) {
        $valid++
        if ([int]$line.hits -gt 0) { $covered++ }
    }
    $totalCovered += $covered
    $totalValid += $valid

    $subsystem = 'other'
    if ($file -match '\\src\\([^\\]+)\\')      { $subsystem = "src\$($matches[1])" }
    elseif ($file -match '\\include\\sak\\')   { $subsystem = 'include\sak' }
    if (-not $bySubsystem.ContainsKey($subsystem)) {
        $bySubsystem[$subsystem] = [pscustomobject]@{ Covered = 0; Valid = 0 }
    }
    $bySubsystem[$subsystem].Covered += $covered
    $bySubsystem[$subsystem].Valid += $valid
}

function Format-Rate([int]$covered, [int]$valid) {
    if ($valid -eq 0) { return 'n/a' }
    return '{0,6:N2}%' -f (100.0 * $covered / $valid)
}

Write-Host ''
Write-Host 'Subsystem                         Covered/Total     Line rate'
Write-Host '--------------------------------  ---------------   ---------'
foreach ($key in ($bySubsystem.Keys | Sort-Object)) {
    $entry = $bySubsystem[$key]
    Write-Host ('{0,-32}  {1,7}/{2,-7}   {3}' -f $key, $entry.Covered, $entry.Valid,
        (Format-Rate $entry.Covered $entry.Valid))
}
Write-Host '--------------------------------  ---------------   ---------'
Write-Host ('{0,-32}  {1,7}/{2,-7}   {3}' -f 'TOTAL (measured files)', $totalCovered, $totalValid,
    (Format-Rate $totalCovered $totalValid))
Write-Host ''
Write-Host "HTML report: $htmlDir\index.html"
Write-Host "Cobertura:   $coberturaXml"
