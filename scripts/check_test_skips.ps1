<#
.SYNOPSIS
    Makes skipped tests visible and holds them to a reviewed baseline.

.DESCRIPTION
    First scans every C++ test source and compares QSKIP site counts with
    tests/skip_site_inventory.txt. This catches dormant branches that did not
    execute in the latest run, and rejects QSKIP inside unsupported-platform
    preprocessor arms.

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

.PARAMETER StaticOnly
    Run only the source inventory and dead-platform-arm checks. Does not require
    CTest result logs.

.EXAMPLE
    pwsh scripts/check_test_skips.ps1
    pwsh scripts/check_test_skips.ps1 -StaticOnly
    pwsh scripts/check_test_skips.ps1 -UpdateBaseline
#>
[CmdletBinding()]
param(
    [string]$ResultsDir,
    [string]$RegistrationJsonPath,
    [switch]$UpdateBaseline,
    [switch]$StaticOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$baselinePath = Join-Path $repoRoot 'tests/skip_baseline.txt'
$staticInventoryPath = Join-Path $repoRoot 'tests/skip_site_inventory.txt'
$needsReasonMarker = 'NEEDS-REASON'
if (-not $ResultsDir) { $ResultsDir = Join-Path $repoRoot 'build/test_results' }

function Write-Section($text) {
    Write-Host ''
    Write-Host $text -ForegroundColor Cyan
    Write-Host ('-' * $text.Length) -ForegroundColor Cyan
}

function Read-StaticSkipInventory {
    $inventory = @{}
    if (-not (Test-Path -LiteralPath $staticInventoryPath -PathType Leaf)) {
        Write-Host "FAIL: no static QSKIP inventory at $staticInventoryPath." -ForegroundColor Red
        exit 1
    }
    foreach ($line in [System.IO.File]::ReadLines($staticInventoryPath)) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        $equals = $trimmed.IndexOf('=')
        $separator = $trimmed.IndexOf('|')
        if ($equals -lt 1 -or $separator -lt ($equals + 2)) {
            Write-Host "FAIL: malformed static QSKIP inventory line: $trimmed" -ForegroundColor Red
            exit 1
        }
        $path = $trimmed.Substring(0, $equals).Trim().Replace('\', '/')
        $countText = $trimmed.Substring($equals + 1, $separator - $equals - 1).Trim()
        $reason = $trimmed.Substring($separator + 1).Trim()
        $count = 0
        if (-not [int]::TryParse($countText, [ref]$count) -or $count -le 0 -or
            [string]::IsNullOrWhiteSpace($reason)) {
            Write-Host "FAIL: invalid count/reason in static QSKIP inventory: $trimmed" -ForegroundColor Red
            exit 1
        }
        if ($inventory.ContainsKey($path)) {
            Write-Host "FAIL: duplicate static QSKIP inventory path: $path" -ForegroundColor Red
            exit 1
        }
        $inventory[$path] = [pscustomobject]@{ Count = $count; Reason = $reason }
    }
    return $inventory
}

function Test-PlatformCondition([string]$condition) {
    return $condition -match '(?:^|[^A-Za-z0-9])_WIN32\b|\bQ_OS_WIN\b|\bSAK_PLATFORM_WINDOWS\b'
}

function Test-PlatformStack([System.Collections.Generic.List[bool]]$stack) {
    foreach ($entry in $stack) {
        if ($entry) { return $true }
    }
    return $false
}

function Get-StaticSkipState {
    $current = @{}
    $deadPlatformArms = [System.Collections.Generic.List[string]]::new()
    $testsRoot = Join-Path $repoRoot 'tests'
    $sourceFiles = @(Get-ChildItem -LiteralPath $testsRoot -Recurse -File |
        Where-Object {
            $_.Extension -in @('.cpp', '.h') -and
            $_.FullName -notmatch '[\\/]third_party[\\/]'
        })
    foreach ($file in $sourceFiles) {
        $relative = $file.FullName.Substring($repoRoot.Length).TrimStart('\', '/').Replace('\', '/')
        $source = [System.IO.File]::ReadAllText($file.FullName)
        $siteCount = [regex]::Matches($source, '\bQSKIP\s*\(').Count
        if ($siteCount -gt 0) { $current[$relative] = $siteCount }

        $platformStack = [System.Collections.Generic.List[bool]]::new()
        $lineNo = 0
        foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
            $lineNo++
            if ($line -match '^\s*#\s*(?:if|ifdef|ifndef)\b(?<condition>.*)$') {
                $inherited = Test-PlatformStack $platformStack
                $platformStack.Add($inherited -or (Test-PlatformCondition $Matches.condition))
                continue
            }
            if ($line -match '^\s*#\s*elif\b(?<condition>.*)$') {
                if ($platformStack.Count -gt 0) {
                    $outer = $false
                    for ($index = 0; $index -lt $platformStack.Count - 1; $index++) {
                        if ($platformStack[$index]) { $outer = $true; break }
                    }
                    $platformStack[$platformStack.Count - 1] =
                        $platformStack[$platformStack.Count - 1] -or $outer -or
                        (Test-PlatformCondition $Matches.condition)
                }
                continue
            }
            if ($line -match '^\s*#\s*endif\b') {
                if ($platformStack.Count -gt 0) {
                    $platformStack.RemoveAt($platformStack.Count - 1)
                }
                continue
            }
            if ($line -match '\bQSKIP\s*\(' -and (Test-PlatformStack $platformStack)) {
                $deadPlatformArms.Add("${relative}:${lineNo}")
            }
        }
    }
    return [pscustomobject]@{ Current = $current; DeadPlatformArms = $deadPlatformArms }
}

Write-Section 'Checking static QSKIP inventory'
$staticInventory = Read-StaticSkipInventory
$staticState = Get-StaticSkipState
$staticFailures = 0

$unreviewedStatic = @($staticState.Current.Keys |
    Where-Object { -not $staticInventory.ContainsKey($_) } | Sort-Object)
$staleStatic = @($staticInventory.Keys |
    Where-Object { -not $staticState.Current.ContainsKey($_) } | Sort-Object)
$changedStatic = @($staticState.Current.Keys | Where-Object {
        $staticInventory.ContainsKey($_) -and
        $staticInventory[$_].Count -ne $staticState.Current[$_]
    } | Sort-Object)

if ($unreviewedStatic.Count -gt 0) {
    $staticFailures += $unreviewedStatic.Count
    Write-Host 'UNINVENTORIED STATIC QSKIP FILES:' -ForegroundColor Red
    foreach ($path in $unreviewedStatic) {
        Write-Host "  $path ($($staticState.Current[$path]) site(s))" -ForegroundColor Red
    }
}
if ($staleStatic.Count -gt 0) {
    $staticFailures += $staleStatic.Count
    Write-Host 'STALE STATIC QSKIP INVENTORY FILES:' -ForegroundColor Red
    foreach ($path in $staleStatic) { Write-Host "  $path" -ForegroundColor Red }
}
if ($changedStatic.Count -gt 0) {
    $staticFailures += $changedStatic.Count
    Write-Host 'STATIC QSKIP SITE-COUNT DRIFT:' -ForegroundColor Red
    foreach ($path in $changedStatic) {
        Write-Host ("  {0}: inventoried {1}, source {2}" -f $path,
                    $staticInventory[$path].Count,
                    $staticState.Current[$path]) -ForegroundColor Red
    }
}
if ($staticState.DeadPlatformArms.Count -gt 0) {
    $staticFailures += $staticState.DeadPlatformArms.Count
    Write-Host 'QSKIP SITES IN DEAD APPLICATION-PLATFORM ARMS:' -ForegroundColor Red
    foreach ($site in $staticState.DeadPlatformArms) { Write-Host "  $site" -ForegroundColor Red }
}
if ($staticFailures -gt 0) {
    Write-Host "STATIC SKIP AUDIT FAILED: $staticFailures discrepancy/discrepancies." -ForegroundColor Red
    exit 1
}
Write-Host ("Static QSKIP inventory passed: {0} file(s), {1} site(s), no dead platform arms." -f
            $staticState.Current.Count,
            (($staticState.Current.Values | Measure-Object -Sum).Sum))
if ($StaticOnly) { exit 0 }

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

function Read-RegisteredQtLogNames {
    if ($RegistrationJsonPath) {
        if (-not (Test-Path -LiteralPath $RegistrationJsonPath -PathType Leaf)) {
            throw "CTest registration JSON not found: $RegistrationJsonPath"
        }
        $registrationText = Get-Content -Raw -LiteralPath $RegistrationJsonPath
    } else {
        $ctest = Get-Command ctest -ErrorAction Stop
        $buildDir = Split-Path -Parent $ResultsDir
        $registrationText = (& $ctest.Source --test-dir $buildDir -C Release `
            --show-only=json-v1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "ctest registration query failed with exit code $LASTEXITCODE"
        }
    }
    $registration = $registrationText | ConvertFrom-Json
    $names = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $resultsFull = [IO.Path]::GetFullPath($ResultsDir).TrimEnd('\', '/')
    foreach ($test in @($registration.tests)) {
        $command = @($test.command)
        for ($index = 0; $index -lt $command.Count - 1; $index++) {
            if ($command[$index] -ne '-o') { continue }
            $outputSpec = [string]$command[$index + 1]
            if (-not $outputSpec.EndsWith(',txt', [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $outputPath = $outputSpec.Substring(0, $outputSpec.Length - 4)
            if ($outputPath -eq '-') { continue }
            $outputFull = [IO.Path]::GetFullPath($outputPath)
            if ([IO.Path]::GetDirectoryName($outputFull).TrimEnd('\', '/') -ne $resultsFull) {
                continue
            }
            if ([IO.Path]::GetFileNameWithoutExtension($outputFull) -ne [string]$test.name) {
                throw "CTest log name does not match registered test '$($test.name)': $outputFull"
            }
            $null = $names.Add([string]$test.name)
        }
    }
    if ($names.Count -eq 0) {
        throw 'CTest registration exposes no Qt per-test logs; refusing an empty skip audit'
    }
    return $names
}

$registeredQtLogs = Read-RegisteredQtLogNames
$allLogs = @(Get-ChildItem -LiteralPath $ResultsDir -Filter '*.txt' -File)
$logs = @($allLogs | Where-Object {
        $registeredQtLogs.Contains([IO.Path]::GetFileNameWithoutExtension($_.Name))
    })
$staleLogCount = $allLogs.Count - $logs.Count
$missingLogs = @($registeredQtLogs | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $ResultsDir "$_.txt") -PathType Leaf)
    } | Sort-Object)

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
Write-Host "  stale removed-test logs ignored: $staleLogCount"
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

if ($missingLogs.Count -gt 0) {
    $failures += $missingLogs.Count
    Write-Host ''
    Write-Host "REGISTERED QT TESTS WITH NO LOG ($($missingLogs.Count)):" -ForegroundColor Red
    foreach ($name in $missingLogs) { Write-Host "  $name" -ForegroundColor Red }
}

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
