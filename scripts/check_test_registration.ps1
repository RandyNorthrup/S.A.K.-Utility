# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Gate: every test source must build, and every test target must run.
#
# WHY THIS EXISTS
# Nine test files -- tests/unit/test_network_share_browser.cpp,
# test_active_connections_monitor.cpp, test_bundled_tools_manager.cpp,
# test_drive_unmounter.cpp, test_image_source.cpp,
# test_network_adapter_inspector.cpp, test_network_diagnostic_controller.cpp,
# test_port_scanner.cpp and test_uninstall_worker.cpp -- existed in the tree,
# were listed in tests/README.md, and had NO target in any CMakeLists. Roughly
# 89 test slots had therefore never compiled and never executed, while earlier
# remediation campaigns cited assertions inside them as evidence that findings
# were fixed. Three of those files were last touched by remediation commits.
#
# The rule that produced this gate: an assertion is only evidence once it runs.
#
# Three directions are checked, because each is a way for a test to stop running:
#   1. a test source with no add_executable  -> it never builds
#   2. a test target with no add_test        -> it builds but ctest never runs it
#   3. a test_*.ps1 with no add_test         -> it needs no build, so nothing else
#                                               would ever notice it does not run
#
# Direction 3 was added after two PowerShell guard tests landed in tests/unit/ during
# the R5 campaign. This gate scanned only test_*.cpp, so they sat in the tree looking
# like coverage while ctest never invoked them -- the same failure this gate exists to
# stop, one file extension over.
#
# Fails closed: a missing file, an unreadable CMakeLists, or zero discovered
# tests is an error, never a silent pass.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$TestsDir = Join-Path $ProjectRoot 'tests'
$TestsCMake = Join-Path $TestsDir 'CMakeLists.txt'
$RootCMake = Join-Path $ProjectRoot 'CMakeLists.txt'

foreach ($required in @($TestsDir, $TestsCMake, $RootCMake)) {
    if (-not (Test-Path $required)) {
        Write-Error "Test registration gate cannot run: missing $required"
        exit 1
    }
}

# Targets that are deliberately NOT ctest entries. Each one drives real hardware
# or requires elevation, so it is run by hand during certification rather than in
# the suite. Anything not on this list must be registered with add_test.
$IntentionallyUnregistered = @(
    'file_management_live_certifier',
    'flash_live_certifier',
    'partition_file_recovery_certifier',
    'partition_filesystem_probe_certifier'
)

# The "is it built" question looks at both files, because a test source may be
# referenced from either. The "is it registered" question looks ONLY at the tests
# CMakeLists: the root file declares the application and CLI targets
# (sak_utility, sak_elevated_helper, the writer CLIs), which are programs rather
# than tests and must never be expected to appear in ctest.
$testsCMakeText = Get-Content $TestsCMake -Raw
$cmakeText = $testsCMakeText + "`n" + (Get-Content $RootCMake -Raw)

$sources = @(Get-ChildItem -Path $TestsDir -Recurse -File -Filter 'test_*.cpp')
if ($sources.Count -eq 0) {
    Write-Error "Test registration gate found no test_*.cpp files at all; refusing to pass."
    exit 1
}

$declaredTargets = @([regex]::Matches($testsCMakeText, '(?m)^\s*add_executable\(\s*([A-Za-z0-9_]+)') |
    ForEach-Object { $_.Groups[1].Value })
$registeredTests = @([regex]::Matches($testsCMakeText, 'add_test\(\s*NAME\s+([A-Za-z0-9_$\{\}]+)') |
    ForEach-Object { $_.Groups[1].Value })

# Targets and add_test names can both come from a foreach(test_name ...) loop, in
# which case the literal name never appears and the CMake text carries
# "${test_name}" instead. When such a loop is present, a name listed inside it
# counts as registered.
# The loop is written as:
#     set(UNIT_TESTS test_a test_b)
#     foreach(test_name ${UNIT_TESTS})
#         ... unit/${test_name}.cpp ... add_test(NAME ${test_name} ...)
# so the names live in the set() block, not inside the foreach parentheses.
# Only treat set() contents as loop-built when the file really does compile
# "${test_name}.cpp"; otherwise an unrelated list could silently exempt a test.
$loopRegistered = @()
if ($testsCMakeText -match '\$\{test_name\}\.cpp') {
    foreach ($m in [regex]::Matches($testsCMakeText, '(?s)set\(\s*[A-Za-z_][A-Za-z0-9_]*\s+(.*?)\)')) {
        foreach ($n in [regex]::Matches($m.Groups[1].Value, '\btest_[A-Za-z0-9_]+\b')) {
            $loopRegistered += $n.Value
        }
    }
}

if ($declaredTargets.Count -eq 0) {
    Write-Error "Test registration gate parsed zero add_executable targets; refusing to pass."
    exit 1
}

# A source counts as built only when it is referenced in a SOURCE position:
# either the literal "<name>.cpp" appears, or the stem is listed inside a
# foreach(test_name ...) loop that compiles "unit/${test_name}.cpp".
#
# Matching the bare stem anywhere is NOT sufficient and was rejected: the stem
# equals the target name, so it also appears in add_test, target_link_libraries
# and set_target_properties. A file removed from its target's source list would
# still have looked "built". Verified by deliberately orphaning a real test and
# confirming the loose form passed while this form fails.
$unbuilt = @()
foreach ($src in $sources) {
    $builtLiterally = $cmakeText -match [regex]::Escape($src.Name)
    $builtByLoop = $loopRegistered -contains $src.BaseName
    if (-not ($builtLiterally -or $builtByLoop)) {
        $unbuilt += $src.FullName.Substring($ProjectRoot.Length + 1)
    }
}

$unregistered = @()
foreach ($target in $declaredTargets) {
    if ($IntentionallyUnregistered -contains $target) { continue }
    if ($registeredTests -contains $target) { continue }
    if ($loopRegistered -contains $target) { continue }
    $unregistered += $target
}

# A PowerShell test has no build step, so "does it compile" says nothing about whether it
# runs. Its stem must appear as an add_test name -- either literally, or inside a
# foreach() list that registers "${<var>}.ps1".
# The loop must be matched as a whole -- its variable, its list, and its body -- and the body
# must register THAT variable as a .ps1. Testing the file for any "${x}.ps1" instead would let
# one such loop vouch for every other foreach in the file, so a .ps1 sharing a stem with some
# unrelated C++ target listed in one would be recorded as running when nothing runs it. That is
# the same silent non-execution this gate exists to catch.
$psLoopRegistered = @()
$loopPattern = '(?s)foreach\(\s*([A-Za-z_][A-Za-z0-9_]*)\s+([^)]*)\)(.*?)endforeach\(\)'
foreach ($m in [regex]::Matches($testsCMakeText, $loopPattern)) {
    $loopVar = $m.Groups[1].Value
    $loopBody = $m.Groups[3].Value
    if ($loopBody -notmatch ([regex]::Escape('${' + $loopVar + '}') + '\.ps1')) { continue }
    if ($loopBody -notmatch '\badd_test\b') { continue }
    foreach ($n in [regex]::Matches($m.Groups[2].Value, '\btest_[A-Za-z0-9_]+\b')) {
        $psLoopRegistered += $n.Value
    }
}

$psSources = @(Get-ChildItem -Path $TestsDir -Recurse -File -Filter 'test_*.ps1')
$psUnregistered = @()
foreach ($ps in $psSources) {
    if ($registeredTests -contains $ps.BaseName) { continue }
    if ($psLoopRegistered -contains $ps.BaseName) { continue }
    $psUnregistered += $ps.FullName.Substring($ProjectRoot.Length + 1)
}

$failed = $false

if ($unbuilt.Count -gt 0) {
    $failed = $true
    Write-Host ''
    Write-Host 'TEST SOURCES THAT NO CMakeLists BUILDS (they never compile or run):' -ForegroundColor Red
    $unbuilt | Sort-Object | ForEach-Object { Write-Host "  $_" }
}

if ($unregistered.Count -gt 0) {
    $failed = $true
    Write-Host ''
    Write-Host 'TEST TARGETS THAT BUILD BUT ARE NEVER REGISTERED WITH ctest:' -ForegroundColor Red
    $unregistered | Sort-Object | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host 'Add an add_test(NAME <target> COMMAND <target> ...) entry, or, if the target'
    Write-Host 'genuinely cannot run in the suite, add it to $IntentionallyUnregistered in'
    Write-Host 'this script WITH a comment stating why.'
}

if ($psUnregistered.Count -gt 0) {
    $failed = $true
    Write-Host ''
    Write-Host 'POWERSHELL TESTS THAT ctest NEVER RUNS:' -ForegroundColor Red
    $psUnregistered | Sort-Object | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host 'Add an add_test(NAME <stem> COMMAND pwsh -NoProfile -NonInteractive -File ...)'
    Write-Host 'entry in tests/CMakeLists.txt. A .ps1 test needs no build step, so nothing'
    Write-Host 'else will ever notice that it is not running.'
}

if ($failed) {
    Write-Host ''
    Write-Error ("Test registration gate failed: {0} unbuilt source(s), {1} unregistered target(s), {2} unregistered PowerShell test(s)." -f
                 $unbuilt.Count, $unregistered.Count, $psUnregistered.Count)
    exit 1
}

Write-Host ("Test registration: {0} test sources, all built; {1} targets, all registered ({2} certifiers exempt); {3} PowerShell test(s), all registered." -f
            $sources.Count, $declaredTargets.Count, $IntentionallyUnregistered.Count, $psSources.Count)
exit 0
