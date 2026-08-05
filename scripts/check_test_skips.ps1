<#
.SYNOPSIS
    Makes skipped tests visible and holds them to a reviewed baseline.

.DESCRIPTION
    ctest reports a test executable as Passed when every one of its test
    functions called QSKIP. The suite line still reads "222/222 tests passed"
    and nothing anywhere says how much was actually exercised. That is how a
    subsystem quietly stops being tested: a precondition (an env var, a fixture
    file, an installed tool) goes missing, every function short-circuits, and
    the gate stays green.

    This script reads the per-test QtTest logs that tests/CMakeLists.txt makes
    every test write to build/test_results/<test>.txt, and compares the skips it
    finds against tests/skip_baseline.txt, which records every skip that has
    been reviewed and accepted along with the reason it is acceptable.

    It fails on ALL THREE kinds of drift:

      * A skip that is not in the baseline. Something stopped running and
        nobody decided that was OK.
      * A baseline entry that did not skip on this run. The entry is stale;
        either the skip was fixed (delete the line) or it is now conditional in
        a way the baseline no longer describes.
      * A registered test with no log at all, or a log with no Totals line.
        That means the binary died before finishing, which ctest can still
        score as Passed if the exit code happened to be zero.

    Stale-entry detection is the half that matters most over time. Without it a
    baseline only ever grows and turns into a list nobody can justify.

    WHY FILE LOGS AND NOT THE CTEST OUTPUT: on Windows, QtTest's plain logger
    does not reach a redirected stdout - not through a pipe and not through a
    file handle - so a ctest log contains no QtTest records at all, whether the
    test passed or crashed. Only "-o <file>,txt" produces them.

.PARAMETER ResultsDir
    Directory holding the per-test logs. Defaults to build/test_results.

.PARAMETER UpdateBaseline
    Rewrite tests/skip_baseline.txt from this run instead of checking against
    it. Reasons for entries that already exist are preserved; genuinely new
    entries are written with a NEEDS-REASON marker that the check itself
    rejects, so the file cannot be regenerated into a rubber stamp.

.EXAMPLE
    pwsh scripts/check_test_skips.ps1
    pwsh scripts/check_test_skips.ps1 -UpdateBaseline
#>
[CmdletBinding()]
param(
    [string]$ResultsDir,
    [switch]$UpdateBaseline
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$baselinePath = Join-Path $repoRoot 'tests/skip_baseline.txt'
$needsReasonMarker = 'NEEDS-REASON'
if (-not $ResultsDir) { $ResultsDir = Join-Path $repoRoot 'build/test_results' }

function Write-Section($text) {
    Write-Host ''
    Write-Host $text -ForegroundColor Cyan
    Write-Host ('-' * $text.Length) -ForegroundColor Cyan
}

if (-not (Test-Path -LiteralPath $ResultsDir)) {
    Write-Host "FAIL: results directory not found: $ResultsDir" -ForegroundColor Red
    Write-Host 'Run the suite first (ctest --test-dir build -C Release).' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# Parse the per-test QtTest logs.
#
#   ********* Start testing of ErrorCodeTests *********
#   PASS   : ErrorCodeTests::initTestCase()
#   SKIP   : SomeTests::someFunction() reason text
#   Totals: 6 passed, 0 failed, 3 skipped, 0 blacklisted, 3ms
# ---------------------------------------------------------------------------

Write-Section 'Parsing per-test logs'

$logs = @(Get-ChildItem -LiteralPath $ResultsDir -Filter '*.txt' -File)
if ($logs.Count -eq 0) {
    Write-Host "FAIL: no per-test logs in $ResultsDir." -ForegroundColor Red
    Write-Host 'Every add_test must pass -o "<dir>/<name>.txt,txt"; see tests/CMakeLists.txt.' -ForegroundColor Red
    exit 1
}

$skipRe = '^SKIP\s+:\s*(?<func>\S+)\s*(?<reason>.*)$'
$totalsRe = '^Totals:\s*(?<passed>\d+)\s+passed,\s*(?<failed>\d+)\s+failed,\s*(?<skipped>\d+)\s+skipped'

$observed = @{}
$totals = @{}
$noTotals = @()

foreach ($log in $logs) {
    $testName = [System.IO.Path]::GetFileNameWithoutExtension($log.Name)
    $sawTotals = $false
    foreach ($line in [System.IO.File]::ReadLines($log.FullName)) {
        $m = [regex]::Match($line, $skipRe)
        if ($m.Success) {
            $func = $m.Groups['func'].Value.TrimEnd('(', ')')
            $key = "${testName}::${func}"
            if (-not $observed.ContainsKey($key)) {
                $observed[$key] = $m.Groups['reason'].Value.Trim()
            }
            continue
        }
        $m = [regex]::Match($line, $totalsRe)
        if ($m.Success) {
            $sawTotals = $true
            $totals[$testName] = [pscustomobject]@{
                Passed  = [int]$m.Groups['passed'].Value
                Failed  = [int]$m.Groups['failed'].Value
                Skipped = [int]$m.Groups['skipped'].Value
            }
        }
    }
    if (-not $sawTotals) { $noTotals += $testName }
}

$totalSkipped = 0
$totalPassed = 0
foreach ($t in $totals.Values) { $totalSkipped += $t.Skipped; $totalPassed += $t.Passed }

$fullySkipped = @()
foreach ($name in $totals.Keys) {
    if ($totals[$name].Skipped -gt 0 -and $totals[$name].Passed -eq 0) { $fullySkipped += $name }
}

Write-Host ''
Write-Host 'SUITE SKIP SUMMARY' -ForegroundColor Yellow
Write-Host "  per-test logs found         : $($logs.Count)"
Write-Host "  test functions passed       : $totalPassed"
Write-Host "  test functions skipped      : $totalSkipped"
Write-Host "  distinct skipping functions : $($observed.Count)"
if ($totalPassed + $totalSkipped -gt 0) {
    $pct = [math]::Round(100.0 * $totalSkipped / ($totalPassed + $totalSkipped), 2)
    Write-Host "  skipped share of executed   : $pct%"
}
if ($fullySkipped.Count -gt 0) {
    Write-Host ''
    Write-Host '  BINARIES THAT SKIPPED EVERYTHING (ctest still scores these Passed):' -ForegroundColor Red
    foreach ($n in ($fullySkipped | Sort-Object)) { Write-Host "    $n" -ForegroundColor Red }
}

# ---------------------------------------------------------------------------
# Baseline.
# ---------------------------------------------------------------------------

function Read-Baseline {
    $map = @{}
    if (-not (Test-Path -LiteralPath $baselinePath)) { return $map }
    foreach ($line in [System.IO.File]::ReadLines($baselinePath)) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        $idx = $trimmed.IndexOf('=')
        if ($idx -lt 1) {
            Write-Host "FAIL: malformed baseline line (expected 'key = reason'): $trimmed" -ForegroundColor Red
            exit 1
        }
        $map[$trimmed.Substring(0, $idx).Trim()] = $trimmed.Substring($idx + 1).Trim()
    }
    return $map
}

$baseline = Read-Baseline

if ($UpdateBaseline) {
    Write-Section 'Rewriting the baseline'
    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine('# Reviewed test skips.')
    [void]$sb.AppendLine('#')
    [void]$sb.AppendLine('# One line per test function that called QSKIP, in the form')
    [void]$sb.AppendLine('#   <test_binary>::<TestClass::testFunction> = why this skip is acceptable')
    [void]$sb.AppendLine('#')
    [void]$sb.AppendLine('# scripts/check_test_skips.ps1 fails when a skip appears that is not listed')
    [void]$sb.AppendLine('# here AND when a line here did not skip on the last run. Regenerate with')
    [void]$sb.AppendLine("# -UpdateBaseline, then replace every $needsReasonMarker with a real reason;")
    [void]$sb.AppendLine('# the check rejects the marker, so the file cannot become a rubber stamp.')
    [void]$sb.AppendLine('#')
    [void]$sb.AppendLine('# A skip is only acceptable when the thing it needs genuinely cannot exist on')
    [void]$sb.AppendLine('# a build machine. "Hard to set up" is not a reason - it is a fixture to write.')
    [void]$sb.AppendLine('')
    $newCount = 0
    foreach ($key in ($observed.Keys | Sort-Object)) {
        if ($baseline.ContainsKey($key) -and $baseline[$key] -notlike "*$needsReasonMarker*") {
            [void]$sb.AppendLine("$key = $($baseline[$key])")
        } else {
            $newCount++
            [void]$sb.AppendLine("$key = $needsReasonMarker  # observed reason: $($observed[$key])")
        }
    }
    [System.IO.File]::WriteAllText($baselinePath, $sb.ToString())
    Write-Host "Wrote $($observed.Count) entries to $baselinePath ($newCount need a reason)."
    if ($newCount -gt 0) {
        Write-Host "Replace every $needsReasonMarker before committing." -ForegroundColor Yellow
    }
    exit 0
}

Write-Section 'Checking against the reviewed baseline'

if (-not (Test-Path -LiteralPath $baselinePath)) {
    Write-Host "FAIL: no baseline at $baselinePath." -ForegroundColor Red
    Write-Host 'Run with -UpdateBaseline, then give every entry a reason.' -ForegroundColor Red
    exit 1
}

$failures = 0

if ($noTotals.Count -gt 0) {
    $failures += $noTotals.Count
    Write-Host ''
    Write-Host "TESTS THAT NEVER REPORTED TOTALS ($($noTotals.Count)) - the binary did not finish:" -ForegroundColor Red
    foreach ($n in ($noTotals | Sort-Object)) { Write-Host "  $n" -ForegroundColor Red }
}

$unreviewed = @($observed.Keys | Where-Object { -not $baseline.ContainsKey($_) } | Sort-Object)
if ($unreviewed.Count -gt 0) {
    $failures += $unreviewed.Count
    Write-Host ''
    Write-Host "UNREVIEWED SKIPS ($($unreviewed.Count)) - these ran nowhere and nobody signed off:" -ForegroundColor Red
    foreach ($k in $unreviewed) {
        Write-Host "  $k" -ForegroundColor Red
        Write-Host "      reason given by the test: $($observed[$k])"
    }
}

$noReason = @($baseline.Keys | Where-Object { $baseline[$_] -like "*$needsReasonMarker*" } | Sort-Object)
if ($noReason.Count -gt 0) {
    $failures += $noReason.Count
    Write-Host ''
    Write-Host "BASELINE ENTRIES WITHOUT A REASON ($($noReason.Count)):" -ForegroundColor Red
    foreach ($k in $noReason) { Write-Host "  $k" -ForegroundColor Red }
}

$stale = @($baseline.Keys | Where-Object { -not $observed.ContainsKey($_) } | Sort-Object)
if ($stale.Count -gt 0) {
    $failures += $stale.Count
    Write-Host ''
    Write-Host "STALE BASELINE ENTRIES ($($stale.Count)) - listed as skipping, but did not skip:" -ForegroundColor Red
    foreach ($k in $stale) {
        Write-Host "  $k" -ForegroundColor Red
        Write-Host '      Delete the line if the skip was fixed. If the skip is conditional,'
        Write-Host '      the condition belongs in the reason so the next reader can tell.'
    }
}

Write-Host ''
if ($failures -gt 0) {
    Write-Host "SKIP AUDIT FAILED: $failures discrepancies." -ForegroundColor Red
    exit 1
}

Write-Host "SKIP AUDIT PASSED: $($observed.Count) skips, all reviewed, none stale." -ForegroundColor Green
exit 0
