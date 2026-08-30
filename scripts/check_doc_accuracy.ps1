# Doc-accuracy gate for tests/README.md. The failure this closes: tests/README.md
# asserted a test count that did not exist, and that inflated coverage claim is
# exactly what hid nine dead (unregistered) test files. Any FACTUAL claim a reader
# would trust about the test suite must therefore be machine-verified against the
# real ctest registration and the real test_*.cpp files -- the same discipline the
# partition-filesystem tool-manifest gate already enforces for bundled binaries.
#
# What is verified (fail closed on any mismatch, or on a required claim gone missing):
#   1. "<N> registered CTest tests" / "Registered Tests: <N>" == count of add_test(NAME)
#      registrations in tests/CMakeLists.txt (foreach lists expanded).
#   2. "<N> C++ test source files" (Overview + footer) == count of test_*.cpp under tests/.
#   3. "(<N> C++ files, including actions/)" == count of test_*.cpp under tests/unit/.
#   4. "End-to-end workflow tests (<N> files)" == count of test_*.cpp under tests/integration/.
#   5. Every coverage-table test name resolves to a registered ctest test.
#   6. Every test_*.cpp filename drawn in the directory tree exists on disk.
#
# The registered count is the static registration count for the documented Windows
# platform: every conditional add_test in this suite is guarded by WIN32 / find_program
# (pwsh, node) / TARGET / EXISTS, all satisfied on the Windows build the README targets.
#
# An unreadable README or CMakeLists is an error, not a silent pass.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$readmePath = Join-Path $root "tests\README.md"
$cmakePath = Join-Path $root "tests\CMakeLists.txt"
$testsRoot = Join-Path $root "tests"
$unitDir = Join-Path $testsRoot "unit"
$integrationDir = Join-Path $testsRoot "integration"

function Fail-Missing {
    param([string]$What, [string]$Path)
    Write-Host "Doc-accuracy gate FAILED: $What is missing: $Path"
    exit 1
}

if (-not (Test-Path -LiteralPath $readmePath -PathType Leaf)) {
    Fail-Missing "tests/README.md" $readmePath
}
if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
    Fail-Missing "tests/CMakeLists.txt" $cmakePath
}

# Resolve a CMake set(<Var> a b c) list to its concrete names.
function Get-CMakeVarList {
    param([string]$Text, [string]$Var)
    $pattern = "(?ms)\bset\s*\(\s*$([regex]::Escape($Var))\b(.*?)\)"
    $m = [regex]::Match($Text, $pattern)
    if (-not $m.Success) { return @() }
    return @($m.Groups[1].Value -split '\s+' | Where-Object { $_ -ne '' })
}

# Resolve a foreach(<Var> ...) header to the concrete test names it iterates. A token
# that is itself a ${LISTVAR} reference is expanded through its set() definition.
function Get-ForeachNames {
    param([string]$Text, [string]$Var)
    $pattern = "(?ms)\bforeach\s*\(\s*$([regex]::Escape($Var))\b(.*?)\)"
    $m = [regex]::Match($Text, $pattern)
    if (-not $m.Success) { return @() }
    $names = @()
    foreach ($tok in ($m.Groups[1].Value -split '\s+' | Where-Object { $_ -ne '' })) {
        if ($tok -match '^\$\{(.+)\}$') { $names += Get-CMakeVarList $Text $matches[1] }
        else { $names += $tok }
    }
    return $names
}

# The set of registered ctest test names. A literal add_test(NAME foo) contributes
# "foo"; an add_test(NAME ${var}) contributes every name its foreach expands to.
function Get-RegisteredTests {
    param([string]$Text)
    $set = New-Object "System.Collections.Generic.HashSet[string]"
    foreach ($m in [regex]::Matches($Text, '(?m)add_test\s*\(\s*NAME\s+(\S+)')) {
        $tok = $m.Groups[1].Value
        if ($tok -match '^\$\{(.+)\}$') {
            foreach ($n in (Get-ForeachNames $Text $matches[1])) { [void]$set.Add($n) }
        } else {
            [void]$set.Add($tok)
        }
    }
    return $set
}

function Get-CppTestCount {
    param([string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir)) { return 0 }
    return @(Get-ChildItem -LiteralPath $Dir -Recurse -File -Filter 'test_*.cpp').Count
}

# Verify every occurrence of a numeric claim matches reality; a claim gone missing
# is itself a failure so the gate cannot be defeated by deleting the sentence.
function Add-NumberClaimViolations {
    param([string]$Text, [string]$Pattern, [int]$Expected, [string]$Label, $Violations)
    $matchesFound = [regex]::Matches($Text, $Pattern)
    if ($matchesFound.Count -eq 0) {
        [void]$Violations.Add("MISSING CLAIM ($Label): absent from README (reality is $Expected)")
        return
    }
    foreach ($m in $matchesFound) {
        $actual = [int]$m.Groups[1].Value
        if ($actual -ne $Expected) {
            [void]$Violations.Add("${Label}: README says $actual, reality is $Expected")
        }
    }
}

# Every coverage-table row names a test in its first column; that test must be a real
# registered ctest test (an optional test_ prefix covers helper-exe rows like the
# partition_filesystem_probe_certifier, registered as test_partition_filesystem_...).
function Add-TableNameViolations {
    param([string]$Text, $Registered, $Violations)
    foreach ($line in ($Text -split "`n")) {
        if ($line -match '^\|\s*([A-Za-z0-9_]+)\s*\|') {
            $name = $matches[1]
            if ($name -eq 'Test') { continue }
            if (-not ($Registered.Contains($name) -or $Registered.Contains("test_$name"))) {
                [void]$Violations.Add("COVERAGE TABLE names an unregistered test: '$name'")
            }
        }
    }
}

# Every test_*.cpp filename drawn in the first fenced block (the directory tree) must
# name a file that actually exists, so the tree can never depict phantom coverage.
function Add-TreeFileViolations {
    param([string]$Text, $FileNames, $Violations)
    $inBlock = $false
    $block = @()
    foreach ($line in ($Text -split "`n")) {
        if ($line -match '^\s*```') {
            if (-not $inBlock) { $inBlock = $true; continue } else { break }
        }
        if ($inBlock) { $block += $line }
    }
    $names = [regex]::Matches(($block -join "`n"), 'test_[a-z0-9_]+\.cpp') |
        ForEach-Object { $_.Value } | Sort-Object -Unique
    foreach ($n in $names) {
        if (-not $FileNames.Contains($n)) {
            [void]$Violations.Add("DIRECTORY TREE lists a nonexistent file: '$n'")
        }
    }
}

$readmeText = Get-Content -LiteralPath $readmePath -Raw
$cmakeText = Get-Content -LiteralPath $cmakePath -Raw

$registered = Get-RegisteredTests $cmakeText
$registeredCount = $registered.Count
$unitCount = Get-CppTestCount $unitDir
$integrationCount = Get-CppTestCount $integrationDir
$totalCppCount = Get-CppTestCount $testsRoot

$fileNames = New-Object "System.Collections.Generic.HashSet[string]"
Get-ChildItem -LiteralPath $testsRoot -Recurse -File -Filter 'test_*.cpp' |
    ForEach-Object { [void]$fileNames.Add($_.Name) }

$violations = New-Object "System.Collections.Generic.List[string]"

Add-NumberClaimViolations $readmeText '(\d+)\s+registered CTest tests' `
    $registeredCount "registered CTest tests (Overview)" $violations
Add-NumberClaimViolations $readmeText 'Registered Tests:\*\*\s*(\d+)' `
    $registeredCount "Registered Tests (footer)" $violations
Add-NumberClaimViolations $readmeText '(\d+)\s+C\+\+ test source files' `
    $totalCppCount "C++ test source files" $violations
Add-NumberClaimViolations $readmeText '\((\d+)\s+C\+\+ files, including actions' `
    $unitCount "unit C++ files (tree comment)" $violations
Add-NumberClaimViolations $readmeText 'End-to-end workflow tests\s*\((\d+)\s+files\)' `
    $integrationCount "integration files (tree comment)" $violations

Add-TableNameViolations $readmeText $registered $violations
Add-TreeFileViolations $readmeText $fileNames $violations

# ---------------------------------------------------------------------------
# 2. docs/CODEX_REVIEW_5_REMEDIATION.md -- its own checkbox tally.
#
# That file opens with a "Tally as of <date>: N [x] / M [~] / K [ ] MARKERS IN THIS
# FILE" line. It is the first thing a reader uses to judge how much is left, and it
# is hand-carried: on 2026-08-30 alone it went stale twice in one session, once when
# an item closed and once when an item was reopened, while the date stamp beside it
# stayed current. A number nobody re-runs is exactly the failure that file exists to
# record, so it is checked here instead of retyped.
# ---------------------------------------------------------------------------

$remediationPath = Join-Path $root "docs/CODEX_REVIEW_5_REMEDIATION.md"
if (Test-Path -LiteralPath $remediationPath) {
    $remediationLines = Get-Content -LiteralPath $remediationPath

    # Count only markers that OPEN a list item, matching how the file is read; a
    # "[x]" appearing mid-prose is not a checkbox.
    $doneCount = 0
    $partialCount = 0
    $openCount = 0
    foreach ($line in $remediationLines) {
        if ($line -match '^\s*- \[x\]') { $doneCount++ }
        elseif ($line -match '^\s*- \[~\]') { $partialCount++ }
        elseif ($line -match '^\s*- \[ \]') { $openCount++ }
    }

    $tallyPattern = '(\d+)\s*\[x\]\s*/\s*(\d+)\s*\[~\]\s*/\s*(\d+)\s*\[ \]'
    $tallyText = ($remediationLines -join "`n")
    $tallyMatch = [regex]::Match($tallyText, $tallyPattern)
    if (-not $tallyMatch.Success) {
        $violations.Add("CODEX_REVIEW_5_REMEDIATION.md: no 'N [x] / M [~] / K [ ]' tally line found")
    } else {
        $claimedDone = [int]$tallyMatch.Groups[1].Value
        $claimedPartial = [int]$tallyMatch.Groups[2].Value
        $claimedOpen = [int]$tallyMatch.Groups[3].Value
        if ($claimedDone -ne $doneCount) {
            $violations.Add(
                "CODEX_REVIEW_5_REMEDIATION.md tally claims $claimedDone [x], file has $doneCount")
        }
        if ($claimedPartial -ne $partialCount) {
            $violations.Add(
                "CODEX_REVIEW_5_REMEDIATION.md tally claims $claimedPartial [~], file has $partialCount")
        }
        if ($claimedOpen -ne $openCount) {
            $violations.Add(
                "CODEX_REVIEW_5_REMEDIATION.md tally claims $claimedOpen [ ], file has $openCount")
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "Doc-accuracy gate FAILED: a doc does not match reality --"
    $violations | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Reality: $registeredCount registered ctest tests, $totalCppCount C++ test"
    Write-Host "source files ($unitCount unit incl. actions/, $integrationCount integration)."
    Write-Host "Fix tests/README.md to match; do not weaken this gate."
    exit 1
}

Write-Host ("Doc-accuracy gate passed: tests/README.md matches reality " +
    "($registeredCount registered ctest tests, $totalCppCount C++ test source files, " +
    "$unitCount unit, $integrationCount integration); " +
    "CODEX_REVIEW_5_REMEDIATION.md tally matches its own markers.")
exit 0
