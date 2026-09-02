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
    [string]$QtBin = 'C:\Qt\6.10.3\msvc2022_64\bin',
    [string]$BuildDir = 'build',
    [string]$OpenCppCoveragePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'process_capture.ps1')

function Resolve-OpenCppCoverage {
    param([string]$ExplicitPath)
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "OpenCppCoverage executable not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $onPath = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $fixed = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe'
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    throw "OpenCppCoverage not found. Install it: choco install opencppcoverage -y"
}

function Resolve-ProjectPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

function Assert-SafeCoverageOutputDirectory([string]$Path, [string]$RootPath) {
    $root = [System.IO.Path]::GetFullPath($RootPath).TrimEnd('\', '/')
    $candidate = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $rootItem = Get-Item -LiteralPath $root -Force -ErrorAction Stop
    if (-not $rootItem.PSIsContainer -or
        ($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Coverage build directory is not a direct, non-reparse directory: $root"
    }
    $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Coverage output must be a strict descendant of the build directory: $candidate"
    }

    $current = $candidate
    while ($current.Length -gt $root.Length) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                throw "Coverage output path crosses a reparse point: $current"
            }
            if (-not $item.PSIsContainer) {
                throw "Coverage output path component is not a directory: $current"
            }
        }
        $parent = [System.IO.Directory]::GetParent($current)
        if ($null -eq $parent) {
            throw "Coverage output parent walk escaped the build directory: $candidate"
        }
        $current = $parent.FullName.TrimEnd('\', '/')
    }
}

function Remove-StaleCoveragePath([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -and
        ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to remove redirected coverage output directory: $Path"
    }
    Remove-Item -LiteralPath $Path -Force -Recurse:$item.PSIsContainer -ErrorAction Stop
    if (Test-Path -LiteralPath $Path) {
        throw "Coverage output cleanup did not remove: $Path"
    }
}

$occ = Resolve-OpenCppCoverage -ExplicitPath $OpenCppCoveragePath
$buildPath = Resolve-ProjectPath -Path $BuildDir
$testModuleDir = Join-Path $buildPath "tests\$Config"
if (-not (Test-Path -LiteralPath $testModuleDir)) {
    throw "No $Config test binaries at $testModuleDir. Build them first: " +
          "cmake --build $BuildDir --config $Config --target <test targets>"
}

$absOut = Resolve-ProjectPath -Path $OutDir
Assert-SafeCoverageOutputDirectory -Path $absOut -RootPath $buildPath
New-Item -ItemType Directory -Force -Path $absOut | Out-Null
Assert-SafeCoverageOutputDirectory -Path $absOut -RootPath $buildPath
$coberturaXml = Join-Path $absOut 'coverage.xml'
$htmlDir = Join-Path $absOut 'html'

function Format-Rate([int]$covered, [int]$valid) {
    if ($valid -eq 0) { return 'n/a' }
    return '{0,6:N2}%' -f (100.0 * $covered / $valid)
}

$previousPath = $env:PATH
try {
    Assert-SafeCoverageOutputDirectory -Path $absOut -RootPath $buildPath
    Remove-StaleCoveragePath -Path $coberturaXml
    Remove-StaleCoveragePath -Path $htmlDir

    # Qt runtime DLLs must resolve for the instrumented test binaries to start.
    if (Test-Path -LiteralPath $QtBin -PathType Container) {
        $env:PATH = "$QtBin;$env:PATH"
    }

    Write-Host "Running coverage over tests matching /$TestRegex/ ($Config)..."
    $coverageArgs = @(
        '--sources', (Join-Path $RepoRoot 'src'),
        '--sources', (Join-Path $RepoRoot 'include'),
        '--modules', $testModuleDir,
        '--export_type', "cobertura:$coberturaXml",
        '--export_type', "html:$htmlDir",
        '--cover_children',
        '--quiet',
        '--', 'ctest', '--test-dir', $buildPath, '-C', $Config, '-R', $TestRegex,
        '--output-on-failure'
    )
    $captured = Invoke-CapturedProcess -FilePath $occ -Arguments $coverageArgs
    if (-not [string]::IsNullOrWhiteSpace($captured.stdout)) {
        Write-Host $captured.stdout
    }
    if ($captured.exit_code -ne 0) {
        throw ("OpenCppCoverage/CTest failed with exit code {0}.`n{1}" -f
               $captured.exit_code, $captured.stderr)
    }
    if (-not (Test-Path -LiteralPath $coberturaXml -PathType Leaf)) {
        throw "Coverage run produced no report at $coberturaXml"
    }
    $htmlIndex = Join-Path $htmlDir 'index.html'
    if (-not (Test-Path -LiteralPath $htmlIndex -PathType Leaf)) {
        throw "Coverage run produced no HTML report at $htmlIndex"
    }

    # Aggregate covered/total lines per top-level source subsystem from the fresh report.
    [xml]$report = Get-Content -LiteralPath $coberturaXml -Raw -ErrorAction Stop
    $bySubsystem = @{}
    $totalCovered = 0
    $totalValid = 0
    $classes = @($report.SelectNodes('/coverage/packages/package/classes/class'))
    foreach ($class in $classes) {
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
        if ($file -match '\\src\\([^\\]+)\\') { $subsystem = "src\$($matches[1])" }
        elseif ($file -match '\\include\\sak\\') { $subsystem = 'include\sak' }
        if (-not $bySubsystem.ContainsKey($subsystem)) {
            $bySubsystem[$subsystem] = [pscustomobject]@{ Covered = 0; Valid = 0 }
        }
        $bySubsystem[$subsystem].Covered += $covered
        $bySubsystem[$subsystem].Valid += $valid
    }
    if ($totalValid -le 0) {
        throw "Coverage report contains zero measured first-party lines: $coberturaXml"
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
    Write-Host ('{0,-32}  {1,7}/{2,-7}   {3}' -f 'TOTAL (measured files)',
        $totalCovered, $totalValid, (Format-Rate $totalCovered $totalValid))
    Write-Host ''
    Write-Host "HTML report: $htmlDir\index.html"
    Write-Host "Cobertura:   $coberturaXml"
}
catch {
    Remove-StaleCoveragePath -Path $coberturaXml
    Remove-StaleCoveragePath -Path $htmlDir
    throw
}
finally {
    $env:PATH = $previousPath
}
