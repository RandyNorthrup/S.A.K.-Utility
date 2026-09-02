# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# BRANCH-coverage measurement over the test suite using clang-cl source-based instrumentation
# (R5-G14-16b).
#
# WHY THIS EXISTS, AND WHY LINE COVERAGE IS NOT ENOUGH
# scripts/run_coverage.ps1 measures LINE coverage with OpenCppCoverage, which is all MSVC can
# give. A line is "covered" the moment any part of it executes, so an untaken arm that shares a
# line with a taken one is invisible. That blind spot hid real defects:
#
#   include/sak/mbox_transfer_decoder.h measured 100% LINE covered while FOUR of its branches
#   had never executed --
#     * `hex1 == '\n'`                  the bare-LF soft break every Unix mailer writes
#     * `hex2 == '\n'`                  the CRLF arm of the soft-break ternary
#     * `ok_second` in `a && ok_second` short-circuited away by every malformed fixture
#     * a whitespace arm of the base64 unwrap, the step every wrapped MIME body takes
#
#   All four were genuine holes, found by hand in sweep b99 and pinned there. Re-measured after
#   those pins the same file reports 32/32 branches. Line coverage said 100% throughout.
#
# The recurring shape is a guard whose REFUSAL arm no fixture reaches, because every fixture
# supplies the benign shape the guard exists to reject. Branch coverage enumerates those
# mechanically instead of leaving them to be found by inspection.
#
# REQUIREMENTS
#   * clang-cl, llvm-profdata and llvm-cov (LLVM 22.1.1 is installed at C:\Program Files\LLVM).
#     MSVC cannot produce a coverage mapping, so the coverage tree MUST be a clang-cl one.
#   * A configured build tree with -DENABLE_COVERAGE=ON. The CMake option refuses to configure
#     under MSVC rather than emit uninstrumented binaries whose empty report would read as full
#     coverage. Configure it with:
#
#       cmake -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
#         -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
#         -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
#         -DCMAKE_RC_COMPILER="C:/Program Files/LLVM/bin/llvm-rc.exe" `
#         -DCMAKE_MT="C:/Program Files/LLVM/bin/llvm-mt.exe" `
#         -DCMAKE_PREFIX_PATH="C:/Qt/6.10.3/msvc2022_64;C:/vcpkg/installed/x64-windows" `
#         -DZLIB_INCLUDE_DIR="C:/vcpkg/installed/x64-windows/include" `
#         -DZLIB_LIBRARY="C:/vcpkg/installed/x64-windows/lib/zlib.lib" `
#         -DVCPKG_INSTALLED_DIR="C:/vcpkg/installed" -DVCPKG_TARGET_TRIPLET=x64-windows `
#         -DENABLE_COVERAGE=ON -DENABLE_ASAN=OFF -DENABLE_UBSAN=OFF
#
#     Ninja rather than the Visual Studio generator because the VS "ClangCL" platform toolset
#     component is not installed; the standalone LLVM is enough for Ninja.
#
# USAGE
#   scripts/run_branch_coverage.ps1 -TestRegex 'test_fuzz_' -OutDir build-cov/coverage
#
# It writes merged profile data and, more usefully, MISSED-BRANCH-REPORT.txt: every branch in
# src/ and include/ that the selected tests never took, as file:line:col with the source text.
# That file is the work list. Like the line-coverage script it is NOT a pre-commit hook --
# instrumented runs are far too slow for every commit.

[CmdletBinding()]
param(
    [string]$TestRegex = '.',
    [string]$BuildDir = 'build-cov',
    [string]$OutDir = 'build-cov/coverage',
    [string]$QtBin = 'C:\Qt\6.10.3\msvc2022_64\bin',
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin',
    [string]$CtestPath = '',
    [string]$LlvmProfdataPath = '',
    [string]$LlvmCovPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'process_capture.ps1')

function Resolve-LlvmTool {
    param([string]$Name, [string]$ExplicitPath)
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "$Name executable not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $fixed = Join-Path $LlvmBin "$Name.exe"
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    throw "$Name not found. Install LLVM (winget install LLVM.LLVM) or pass -LlvmBin."
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

function Resolve-Ctest([string]$ExplicitPath) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "CTest executable not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $resolved = Get-Command ctest.exe -CommandType Application -ErrorAction Stop
    return $resolved.Source
}

function Remove-BranchCoverageFile([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    if (Test-Path -LiteralPath $Path) {
        throw "Coverage output cleanup did not remove: $Path"
    }
}

$profdata = Resolve-LlvmTool -Name 'llvm-profdata' -ExplicitPath $LlvmProfdataPath
$cov = Resolve-LlvmTool -Name 'llvm-cov' -ExplicitPath $LlvmCovPath
$ctest = Resolve-Ctest -ExplicitPath $CtestPath

$buildPath = Resolve-ProjectPath -Path $BuildDir
if (-not (Test-Path -LiteralPath $buildPath)) {
    throw "Coverage build tree '$buildPath' does not exist. Configure it first (see the header)."
}
# Fail closed on an uninstrumented tree: a report over binaries with no coverage mapping is
# empty, and an empty missed-branch list reads as "nothing uncovered" -- the exact false green
# this whole exercise exists to remove.
$cache = Join-Path $buildPath 'CMakeCache.txt'
if (-not (Select-String -LiteralPath $cache -Pattern '^ENABLE_COVERAGE:BOOL=ON' -Quiet)) {
    throw "'$BuildDir' was not configured with -DENABLE_COVERAGE=ON. Its binaries carry no " +
          "coverage mapping, so the report would be empty and read as full coverage."
}

$outPath = Resolve-ProjectPath -Path $OutDir
Assert-SafeCoverageOutputDirectory -Path $outPath -RootPath $buildPath
New-Item -ItemType Directory -Force -Path $outPath | Out-Null
Assert-SafeCoverageOutputDirectory -Path $outPath -RootPath $buildPath
$merged = Join-Path $outPath 'merged.profdata'
$summary = Join-Path $outPath 'BRANCH-SUMMARY.txt'
$report = Join-Path $outPath 'MISSED-BRANCH-REPORT.txt'
$summaryTemp = "$summary.tmp-$PID"
$reportTemp = "$report.tmp-$PID"
$previousPath = $env:PATH
$previousProfile = $env:LLVM_PROFILE_FILE

try {
    Assert-SafeCoverageOutputDirectory -Path $outPath -RootPath $buildPath
    foreach ($path in @($merged, $summary, $report, $summaryTemp, $reportTemp)) {
        Remove-BranchCoverageFile -Path $path
    }
    Get-ChildItem -LiteralPath $outPath -Filter '*.profraw' -File -ErrorAction Stop |
        Remove-Item -Force -ErrorAction Stop

    if (Test-Path -LiteralPath $QtBin -PathType Container) {
        $env:PATH = "$QtBin;$env:PATH"
    }
    # %p keeps each test process in its own file: a single fixed path would have every executable
    # overwrite the previous one and the merge would cover only the last test that ran.
    $env:LLVM_PROFILE_FILE = (Join-Path $outPath 'sak-%p.profraw')

    Write-Host "Running instrumented tests (regex: $TestRegex) ..."
    $ctestResult = Invoke-CapturedProcess -FilePath $ctest -WorkingDirectory $buildPath `
        -Arguments @('-R', $TestRegex, '--output-on-failure')
    @($ctestResult.stdout, $ctestResult.stderr) | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | ForEach-Object { $_ -split "`r?`n" } | Select-Object -Last 12 | Write-Host
    Write-Host "ctest exit: $($ctestResult.exit_code)"
    if ($ctestResult.exit_code -ne 0) {
        throw "Instrumented CTest run failed with exit code $($ctestResult.exit_code)."
    }

    $raws = @(Get-ChildItem -LiteralPath $outPath -Filter '*.profraw' -File -ErrorAction Stop)
    if ($raws.Count -eq 0) {
        throw "No .profraw files were produced. The binaries are not instrumented, or " +
              "LLVM_PROFILE_FILE did not reach the test processes."
    }
    Write-Host "Merging $($raws.Count) profile(s) ..."
    $profdataArgs = @('merge', '-sparse') + @($raws.FullName) + @('-o', $merged)
    $profdataResult = Invoke-CapturedProcess -FilePath $profdata -Arguments $profdataArgs
    if ($profdataResult.exit_code -ne 0 -or
        -not (Test-Path -LiteralPath $merged -PathType Leaf)) {
        throw "llvm-profdata merge failed with exit code $($profdataResult.exit_code)."
    }

    # Every instrumented test executable contributes its own coverage mapping, so they all have
    # to be named -- llvm-cov reads mappings from binaries, not the profile.
    $exes = @(Get-ChildItem -LiteralPath (Join-Path $buildPath 'tests') `
        -Filter 'test_*.exe' -File -Recurse -ErrorAction Stop)
    if ($exes.Count -eq 0) { throw "No test executables found under $buildPath/tests." }
    $objArgs = @()
    foreach ($exe in ($exes | Select-Object -Skip 1)) {
        $objArgs += @('-object', $exe.FullName)
    }
    $filterArgs = @('--ignore-filename-regex',
        '(Qt[\\/]|vcpkg[\\/]|tests[\\/]|third_party[\\/])')

    $reportArgs = @('report', $exes[0].FullName) + $objArgs +
        @('--instr-profile', $merged, '--show-branch-summary') + $filterArgs
    $summaryResult = Invoke-CapturedProcess -FilePath $cov -Arguments $reportArgs
    if ($summaryResult.exit_code -ne 0) {
        throw "llvm-cov report failed with exit code $($summaryResult.exit_code)."
    }
    if ([string]::IsNullOrWhiteSpace($summaryResult.stdout)) {
        throw 'llvm-cov report returned no summary output.'
    }

    # The actionable artifact: every branch in first-party code that was never taken in one
    # direction. `--show-branches count` prints each untaken arm under its source line.
    Write-Host "Extracting missed branches ..."
    $showArgs = @('show', $exes[0].FullName) + $objArgs +
        @('--instr-profile', $merged, '--show-branches', 'count') + $filterArgs
    $showResult = Invoke-CapturedProcess -FilePath $cov -Arguments $showArgs
    if ($showResult.exit_code -ne 0) {
        throw "llvm-cov show failed with exit code $($showResult.exit_code)."
    }
    if ([string]::IsNullOrWhiteSpace($showResult.stdout)) {
        throw 'llvm-cov show returned no annotated source output.'
    }

    $lines = $showResult.stdout -split "`r?`n"
    $currentFile = ''
    $missed = 0
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("Branches never taken in one direction, over tests matching: $TestRegex")
    $out.Add("A 'True: 0' arm is a condition no test ever satisfied; 'False: 0' is one no test ever")
    $out.Add("falsified. Both are guards the suite cannot distinguish from their own absence.")
    $out.Add('')
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^([A-Za-z]:[\\/].+?):$') {
            $currentFile = $Matches[1]
            continue
        }
        if ($line -match 'Branch\s+\((\d+):(\d+)\):\s+\[True:\s*(\S+?),\s*False:\s*(\S+?)\]') {
            $srcLine = $Matches[1]
            $srcCol = $Matches[2]
            $trueCount = $Matches[3]
            $falseCount = $Matches[4]
            if ($trueCount -eq '0' -or $falseCount -eq '0') {
                $missed++
                $arm = if ($trueCount -eq '0') { 'never TRUE ' } else { 'never FALSE' }
                $context = ''
                for ($j = $i - 1; $j -ge 0 -and $j -gt $i - 12; $j--) {
                    if ($lines[$j] -match '^\s*\d+\|\s*[\d\.kMG]*\|(.*)$') {
                        $context = $Matches[1].Trim()
                        break
                    }
                }
                $out.Add(("{0}:{1}:{2}  {3}  {4}" -f
                    $currentFile, $srcLine, $srcCol, $arm, $context))
            }
        }
    }
    $out.Insert(3, "TOTAL MISSED BRANCHES: $missed")
    [System.IO.File]::WriteAllText($summaryTemp, $summaryResult.stdout + "`r`n",
        [System.Text.Encoding]::ASCII)
    $out | Out-File -LiteralPath $reportTemp -Encoding ascii
    Move-Item -LiteralPath $summaryTemp -Destination $summary -Force -ErrorAction Stop
    Move-Item -LiteralPath $reportTemp -Destination $report -Force -ErrorAction Stop
    Write-Host "Wrote $summary"
    Write-Host ""
    Write-Host "TOTAL MISSED BRANCHES: $missed"
    Write-Host "Wrote $report"
}
catch {
    $failure = $_
    foreach ($path in @($merged, $summary, $report, $summaryTemp, $reportTemp)) {
        Remove-BranchCoverageFile -Path $path
    }
    Get-ChildItem -LiteralPath $outPath -Filter '*.profraw' -File -ErrorAction Stop |
        Remove-Item -Force -ErrorAction Stop
    throw $failure
}
finally {
    $env:PATH = $previousPath
    $env:LLVM_PROFILE_FILE = $previousProfile
}
