<#
.SYNOPSIS
    Whole-project cppcheck regression gate (R5-G10-3), plus a stale-inline-suppression reveal.

.DESCRIPTION
    THE HOLE THIS CLOSES. The cppcheck pre-commit hook runs run_cppcheck.ps1 -Files, i.e.
    the CHANGED FILES of the commit. That is the right shape for a commit hook and the wrong
    shape for the claim "cppcheck clean project-wide": a finding in a file nobody has touched
    is invisible until somebody touches it, and then it blocks THEIR commit. Measured on
    2026-08-30, 84 such findings sat in the tree while every commit passed. The CI workflow
    does carry a whole-tree cppcheck step, but before this local twin existed it was scoped to
    src/**/*.cpp (no headers, no tests), and CI had not executed for 776 commits (R5-G21-7 is an
    owner cost decision), so nothing that actually RAN was enforcing the claim.

    PASS 1 -- whole project. Runs run_cppcheck.ps1 with no -Files, so cppcheck walks src/,
    include/, and tests/ as directories (src/third_party excluded). Any finding fails the gate.

    PASS 2 -- stale-inline-suppression reveal. cppcheck's own unmatchedSuppression is the
    only check that reports a suppression matching nothing, and it is suppressed globally in
    cppcheck_suppressions.txt as "meta-noise from the suppression file itself". That is true
    of the FILE-level entries -- one may legitimately match no active finding in a clean or
    configuration-specific run -- and false of the INLINE ones, where matching nothing means
    the suppression is dead code hiding the fact that a check no longer fires. So this pass runs
    against a stripped copy of the suppressions list with two entries removed:

      unmatchedSuppression -- the signal itself; leaving it in reports nothing, by definition.
      useStlAlgorithm      -- a global entry that MASKS its own three inline sites. With it
                              in place those three inline suppressions match nothing (the
                              global one already silenced the check) and read as stale when
                              they are live. Verified: removed, all three fire.

    Pass 2 is deliberately given an explicit .cpp FILE LIST, not directories. A header
    analyzed on its own instantiates nothing, so every check inside it comes back silent and
    every live suppression in it reads as dead -- a header suppression can only be judged
    through a .cpp that includes it. Findings other than unmatchedSuppression are ignored in
    this pass; stripping useStlAlgorithm re-exposes a recorded owner style decision that is
    not this gate's business.

    FALSE-GREEN TRIPWIRE. cppcheck degrades silently: an unparseable macro in a translation
    unit drops it to "Active checkers: 4/186" and it still exits 0 with no findings. That is
    how 65 of 238 test files were once unanalyzed while the gate reported a pass. Both passes
    therefore assert cppcheck's own checkersReport line shows at least 170 of its 186
    checkers active, and fail if the line is absent entirely.

    COST. Two exhaustive cppcheck passes over the tree, on the order of twenty minutes total.
    That is why this is manual-stage rather than per-commit -- the per-commit `cppcheck` hook
    stays scoped to changed files and stays fast. Pass 2 is given every first-party .cpp rather
    than only the ~21 files that currently contain a suppression: a suppression inside a HEADER
    can only be judged through a .cpp that includes it, and working out which .cpp exercises a
    given header means walking transitive includes. Getting that wrong reports a live
    suppression as dead, so the scope is deliberately the safe one.

.PARAMETER SkipRevealPass
    Run only the whole-project pass. For iterating on findings; the gate itself always runs
    both, and CI must never pass this.

.EXAMPLE
    scripts/cppcheck_whole_project_gate.ps1
#>

param(
    [switch]$SkipRevealPass
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RunCppcheck = Join-Path $PSScriptRoot "run_cppcheck.ps1"

# cppcheck reported 183/186 active checkers over this tree on 2026-08-30. The floor is set
# just under that: it has to catch the catastrophic degradation (4/186) without going red
# every time a cppcheck release adds or retires a checker.
$kMinimumActiveCheckers = 170

# Findings are one per line ending in [checkId]. checkersReport is cppcheck's own
# informational summary, not a finding.
$kFindingPattern = '\[([a-zA-Z0-9_]+)\]$'

function Assert-CppcheckActuallyRan {
    param(
        [string[]]$Lines,
        [string]$PassName
    )

    $report = $Lines | Where-Object { $_ -match 'Active checkers:\s*(\d+)/(\d+)' } | Select-Object -First 1
    if (-not $report) {
        Write-Host ""
        Write-Host "GATE FAILED ($PassName): cppcheck emitted no 'Active checkers' line." -ForegroundColor Red
        Write-Host "  A run that reports nothing AND cannot say how much of itself was enabled is"
        Write-Host "  not evidence of a clean tree. Check that --enable=all reached cppcheck."
        exit 1
    }

    $null = $report -match 'Active checkers:\s*(\d+)/(\d+)'
    $active = [int]$Matches[1]
    $total = [int]$Matches[2]
    if ($active -lt $kMinimumActiveCheckers) {
        Write-Host ""
        Write-Host "GATE FAILED ($PassName): only $active/$total checkers were active." -ForegroundColor Red
        Write-Host "  cppcheck degrades SILENTLY -- an unparseable macro abandons the translation"
        Write-Host "  unit and it still exits 0 finding nothing. Minimum is $kMinimumActiveCheckers."
        exit 1
    }

    Write-Host "  cppcheck ran with $active/$total checkers active." -ForegroundColor DarkGray
}

function Get-FirstPartySources {
    # src/**/*.cpp plus tests/**/*.cpp. Vendored code is out of scope and is never edited
    # here, so a finding in it is not actionable.
    $roots = @((Join-Path $ProjectRoot "src"), (Join-Path $ProjectRoot "tests"))
    $found = @()
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $found += Get-ChildItem -LiteralPath $root -Recurse -Filter *.cpp -File |
            Where-Object { $_.FullName -notmatch '\\(third_party|3rdparty|external|_archived)\\' } |
            ForEach-Object { $_.FullName }
    }
    return $found
}

# ---------------------------------------------------------------------------
# Pass 1 -- whole project (src/, include/, and tests/ as directories)
# ---------------------------------------------------------------------------
Write-Host "PASS 1/2: cppcheck whole project (src/ + include/ + tests/)..." -ForegroundColor Cyan

$pass1Errors = Join-Path $ProjectRoot "build\cppcheck_whole_project.txt"
& $RunCppcheck -ErrorsFile $pass1Errors | Out-Null
$pass1ExitCode = $LASTEXITCODE

if (-not (Test-Path -LiteralPath $pass1Errors)) {
    Write-Host "GATE FAILED: cppcheck wrote no output file ($pass1Errors)." -ForegroundColor Red
    Write-Host "  Exit code was $pass1ExitCode. The run did not happen; this is not a pass."
    exit 1
}

$pass1Lines = @(Get-Content -LiteralPath $pass1Errors)
Assert-CppcheckActuallyRan -Lines $pass1Lines -PassName "whole project"

$pass1Findings = $pass1Lines | Where-Object {
    ($_ -match $kFindingPattern) -and ($Matches[1] -ne 'checkersReport')
}

if ($pass1Findings.Count -gt 0) {
    Write-Host ""
    Write-Host "GATE FAILED: $($pass1Findings.Count) whole-project cppcheck finding(s)." -ForegroundColor Red
    Write-Host "  Each of these is a landmine for whoever next edits that file: the pre-commit"
    Write-Host "  hook runs the same checks on CHANGED files, so it will block their commit for"
    Write-Host "  a finding they did not introduce."
    Write-Host ""
    $pass1Findings | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}

Write-Host "PASS 1 clean: 0 whole-project findings." -ForegroundColor Green

if ($SkipRevealPass) {
    Write-Host ""
    Write-Host "Reveal pass SKIPPED by -SkipRevealPass. This is NOT a gate pass." -ForegroundColor Yellow
    exit 0
}

# ---------------------------------------------------------------------------
# Pass 2 -- stale-inline-suppression reveal
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "PASS 2/2: stale-inline-suppression reveal (unmatchedSuppression enabled)..." -ForegroundColor Cyan

$realSuppressions = Join-Path $ProjectRoot "cppcheck_suppressions.txt"
if (-not (Test-Path -LiteralPath $realSuppressions -PathType Leaf)) {
    Write-Host "GATE FAILED: cppcheck_suppressions.txt is missing." -ForegroundColor Red
    exit 1
}

# Strip the two entries that make the reveal impossible. Matched as whole lines so a
# comment mentioning either name is left alone.
$stripped = Get-Content -LiteralPath $realSuppressions |
    Where-Object { $_.Trim() -notin @('unmatchedSuppression', 'useStlAlgorithm') }

$revealSuppressions = Join-Path $ProjectRoot "build\cppcheck_suppressions_reveal.txt"
$revealDir = Split-Path -Parent $revealSuppressions
if (-not (Test-Path -LiteralPath $revealDir -PathType Container)) {
    New-Item -ItemType Directory -Path $revealDir -Force | Out-Null
}
Set-Content -LiteralPath $revealSuppressions -Value $stripped -Encoding ascii

$sources = Get-FirstPartySources
if ($sources.Count -eq 0) {
    # The exact false-green shape called out for the CI clang-format step: a file list that
    # resolves to nothing makes run_cppcheck.ps1 exit 0 with "No C++ files to check."
    Write-Host "GATE FAILED: the reveal pass resolved to ZERO source files." -ForegroundColor Red
    Write-Host "  An empty file list is a green run that checked nothing, not a clean tree."
    exit 1
}

Write-Host "  Reveal pass over $($sources.Count) first-party sources." -ForegroundColor DarkGray

$pass2Errors = Join-Path $ProjectRoot "build\cppcheck_suppression_reveal.txt"
& $RunCppcheck -Files $sources -SuppressionsFile $revealSuppressions -ErrorsFile $pass2Errors | Out-Null

if (-not (Test-Path -LiteralPath $pass2Errors)) {
    Write-Host "GATE FAILED: the reveal pass wrote no output file ($pass2Errors)." -ForegroundColor Red
    exit 1
}

$pass2Lines = @(Get-Content -LiteralPath $pass2Errors)
Assert-CppcheckActuallyRan -Lines $pass2Lines -PassName "suppression reveal"

# Only unmatchedSuppression matters here. Removing useStlAlgorithm above deliberately
# re-exposes a recorded owner style decision, and those findings are not this gate's
# business -- pass 1 is what enforces the finding count.
$stale = $pass2Lines | Where-Object { $_ -match '\[unmatchedSuppression\]$' }

# A file-level entry legitimately matches nothing (the tests-scoped entries match nothing in
# a run that analyzes no test). Only INLINE suppressions -- which cppcheck reports against a
# real file and line -- are dead code. Anything reported at nofile:0:0 is a file-level entry.
$staleInline = $stale | Where-Object { $_ -notmatch '^nofile:' }

if ($staleInline.Count -gt 0) {
    Write-Host ""
    Write-Host "GATE FAILED: $($staleInline.Count) stale inline cppcheck suppression(s)." -ForegroundColor Red
    Write-Host "  Each of these // cppcheck-suppress comments matches nothing: the check it"
    Write-Host "  names no longer fires at that line. Delete the suppression. Leaving it in"
    Write-Host "  place silences a check that WILL matter after the next edit to that line."
    Write-Host ""
    $staleInline | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}

Write-Host "PASS 2 clean: 0 stale inline suppressions." -ForegroundColor Green
Write-Host ""
Write-Host "CPPCHECK WHOLE-PROJECT GATE PASSED." -ForegroundColor Green
exit 0
