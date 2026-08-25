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
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-LlvmTool {
    param([string]$Name)
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $fixed = Join-Path $LlvmBin "$Name.exe"
    if (Test-Path -LiteralPath $fixed) { return $fixed }
    throw "$Name not found. Install LLVM (winget install LLVM.LLVM) or pass -LlvmBin."
}

$profdata = Resolve-LlvmTool -Name 'llvm-profdata'
$cov = Resolve-LlvmTool -Name 'llvm-cov'

$buildPath = Join-Path $RepoRoot $BuildDir
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

$outPath = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $outPath | Out-Null
Get-ChildItem -LiteralPath $outPath -Filter '*.profraw' -ErrorAction SilentlyContinue |
    Remove-Item -Force

$env:PATH = "$QtBin;$env:PATH"
# %p keeps each test process in its own file: a single fixed path would have every executable
# overwrite the previous one and the merge would cover only the last test that ran.
$env:LLVM_PROFILE_FILE = (Join-Path $outPath 'sak-%p.profraw')

Write-Host "Running instrumented tests (regex: $TestRegex) ..."
Push-Location $buildPath
try {
    & ctest -R $TestRegex --output-on-failure 2>&1 | Select-Object -Last 12
    $ctestExit = $LASTEXITCODE
} finally {
    Pop-Location
}
Write-Host "ctest exit: $ctestExit"

$raws = Get-ChildItem -LiteralPath $outPath -Filter '*.profraw'
if ($raws.Count -eq 0) {
    throw "No .profraw files were produced. The binaries are not instrumented, or " +
          "LLVM_PROFILE_FILE did not reach the test processes."
}
Write-Host "Merging $($raws.Count) profile(s) ..."
$merged = Join-Path $outPath 'merged.profdata'
& $profdata merge -sparse @($raws.FullName) -o $merged
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed." }

# Every instrumented test executable contributes its own coverage mapping, so they all have to be
# named -- llvm-cov reads the mapping out of the binaries, not the profile.
$exes = Get-ChildItem -LiteralPath (Join-Path $buildPath 'tests') -Filter 'test_*.exe' -Recurse
if ($exes.Count -eq 0) { throw "No test executables found under $buildPath/tests." }
$objArgs = @()
foreach ($exe in ($exes | Select-Object -Skip 1)) { $objArgs += @('-object', $exe.FullName) }

$summary = Join-Path $outPath 'BRANCH-SUMMARY.txt'
& $cov report $exes[0].FullName @objArgs '--instr-profile' $merged '--show-branch-summary' `
    '--ignore-filename-regex' '(Qt[\\/]|vcpkg[\\/]|tests[\\/]|third_party[\\/])' |
    Out-File -FilePath $summary -Encoding ascii
Write-Host "Wrote $summary"

# The actionable artifact: every branch in first-party code that was never taken in one
# direction. `--show-branches count` prints "Branch (LINE:COL): [True: N, False: M]" under its
# source line, so a zero on either side is an arm no test reached.
Write-Host "Extracting missed branches ..."
$showRaw = & $cov show $exes[0].FullName @objArgs '--instr-profile' $merged `
    '--show-branches' 'count' `
    '--ignore-filename-regex' '(Qt[\\/]|vcpkg[\\/]|tests[\\/]|third_party[\\/])' 2>&1

$report = Join-Path $outPath 'MISSED-BRANCH-REPORT.txt'
$lines = $showRaw -split "`r?`n"
$currentFile = ''
$missed = 0
$out = New-Object System.Collections.Generic.List[string]
$out.Add("Branches never taken in one direction, over tests matching: $TestRegex")
$out.Add("A 'True: 0' arm is a condition no test ever satisfied; 'False: 0' is one no test ever")
$out.Add("falsified. Both are guards the suite cannot distinguish from their own absence.")
$out.Add('')
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    # llvm-cov prints each file as a bare path followed by ':'
    if ($line -match '^([A-Za-z]:[\\/].+?):$') { $currentFile = $Matches[1]; continue }
    if ($line -match 'Branch\s+\((\d+):(\d+)\):\s+\[True:\s*(\S+?),\s*False:\s*(\S+?)\]') {
        $srcLine = $Matches[1]; $srcCol = $Matches[2]
        $trueCount = $Matches[3]; $falseCount = $Matches[4]
        if ($trueCount -eq '0' -or $falseCount -eq '0') {
            $missed++
            $arm = if ($trueCount -eq '0') { 'never TRUE ' } else { 'never FALSE' }
            # Walk back to the nearest numbered source line for context.
            $context = ''
            for ($j = $i - 1; $j -ge 0 -and $j -gt $i - 12; $j--) {
                if ($lines[$j] -match '^\s*\d+\|\s*[\d\.kMG]*\|(.*)$') {
                    $context = $Matches[1].Trim(); break
                }
            }
            $out.Add(("{0}:{1}:{2}  {3}  {4}" -f $currentFile, $srcLine, $srcCol, $arm, $context))
        }
    }
}
$out.Insert(3, "TOTAL MISSED BRANCHES: $missed")
$out | Out-File -FilePath $report -Encoding ascii
Write-Host ""
Write-Host "TOTAL MISSED BRANCHES: $missed"
Write-Host "Wrote $report"
if ($ctestExit -ne 0) {
    Write-Host "NOTE: ctest exited $ctestExit; the coverage above is over a run with failures."
}
