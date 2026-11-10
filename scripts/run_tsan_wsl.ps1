<#
.SYNOPSIS
    Build and run the ThreadSanitizer harness inside WSL archlinux (R5-G23-1).

.DESCRIPTION
    TSan does not exist on any Windows toolchain -- MSVC has none, clang-cl does not implement
    -fsanitize=thread -- so the Windows build can never produce a TSan binary. It IS reachable
    through the WSL archlinux this repo already uses for apfsck: clang and qt6-base are installed
    there, and the cross-platform half of the codebase compiles against them.

    SCOPE, stated plainly: this runs the portable threaded units listed in
    tests/tsan/CMakeLists.txt under real race detection. Most workers include <windows.h> and
    cannot be built here at all, so a clean run means "no race in those units", NOT "no race in
    the application".

    HOW A REPORT IS JUDGED
    The distro Qt is not instrumented, and QtTest starts a WatchDog QThread whose
    condition_variable teardown produces two or three ThreadSanitizer reports on every run no
    matter what code is under test. Those reports contain frames only from libQt6Test/libQt6Core,
    libstdc++ and libc.

    A suppressions file was tried first and REJECTED: `called_from_lib` matches the library that
    called into the interceptor, which for these reports is libstdc++, not Qt -- so the entries
    silently matched nothing while appearing to work. Worse, without llvm-symbolizer every frame
    is "??:?" and NO suppression can match, which is a fail-open that looks identical to a clean
    run.

    So the rule here is positive, not subtractive: a report FAILS the run if any of its frames
    names first-party source (a path under this repository). Everything else is printed under a
    clearly-labelled third-party heading and counted, never hidden. A race of ours that merely
    passes through Qt still names our file, so it still fails.

.PARAMETER Distro
    WSL distribution to build in. Default archlinux.

.EXAMPLE
    ./scripts/run_tsan_wsl.ps1
#>
param(
    [string]$Distro = "archlinux",
    # NOT /tmp: WSL shuts the distro down when idle and /tmp there is wiped, so a build placed in
    # /tmp disappears between invocations and the next run silently rebuilds from scratch.
    [string]$BuildDir = "/root/sak-tsan-build"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$RepoLeaf = Split-Path -Leaf $RepoRoot
$drive = $RepoRoot.Substring(0, 1).ToLower()
$WslRepo = "/mnt/$drive" + $RepoRoot.Substring(2).Replace("\", "/")

# One row per target in tests/tsan/CMakeLists.txt: the executable and the QtTest function that
# actually drives its threads. A target whose test is single-threaded proves nothing about races
# and must not be listed -- a green run from a one-thread test is not evidence.
$Targets = @(
    @{ Name = "tsan_logger"; Function = "concurrentWrites_noCorruption" }
)

function Invoke-Wsl([string]$Command) {
    $output = wsl -d $Distro -u root -- bash -lc $Command 2>&1 | Out-String
    return @{ Output = $output; ExitCode = $LASTEXITCODE }
}

# Split raw TSan output into report blocks and decide which ones implicate first-party code.
function Split-TsanReports([string]$Text, [string]$RepoMarker) {
    $reports = @()
    $current = $null
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match "WARNING: ThreadSanitizer: (.+?) \(pid=") {
            if ($null -ne $current) { $reports += $current }
            $current = [ordered]@{ Kind = $Matches[1]; Lines = @($line); FirstParty = $false }
        } elseif ($null -ne $current) {
            $current.Lines += $line
            if ($line -like "*$RepoMarker*") { $current.FirstParty = $true }
            if ($line -match "^SUMMARY: ThreadSanitizer:") {
                $reports += $current
                $current = $null
            }
        }
    }
    if ($null -ne $current) { $reports += $current }
    return $reports
}

Write-Host "TSan harness (R5-G23-1): WSL $Distro"

# A symbolizer is REQUIRED, not a nicety: without it every frame resolves to "??:?", first-party
# code becomes indistinguishable from library code, and this script would report a real race as
# third-party noise. Fail closed rather than run blind.
if ((Invoke-Wsl "command -v llvm-symbolizer").ExitCode -ne 0) {
    Write-Error ("TSAN HARNESS FAILED: llvm-symbolizer is not installed in $Distro. Without it " +
                 "TSan cannot symbolize frames and a first-party race is indistinguishable from " +
                 "Qt's own. Install with: wsl -d $Distro -u root -- pacman -S llvm")
    exit 1
}

$configure = Invoke-Wsl "cd '$WslRepo' && cmake -S tests/tsan -B '$BuildDir' -G Ninja -DCMAKE_CXX_COMPILER=clang++ 2>&1 | tail -5"
if ($configure.ExitCode -ne 0) {
    Write-Host $configure.Output
    Write-Error "TSAN HARNESS FAILED: cmake configure. Is qt6-base installed in $Distro?"
    exit 1
}
$build = Invoke-Wsl "cmake --build '$BuildDir' 2>&1 | tail -15"
if ($build.ExitCode -ne 0) {
    Write-Host $build.Output
    Write-Error "TSAN HARNESS FAILED: build."
    exit 1
}
Write-Host "  build ok"

$firstPartyFailures = @()
$thirdPartyCount = 0
foreach ($target in $Targets) {
    $name = $target.Name
    $fn = $target.Function
    # halt_on_error=0 so every race in the run is reported, not just the first.
    $run = Invoke-Wsl ("cd /tmp && TSAN_OPTIONS='halt_on_error=0' QT_QPA_PLATFORM=offscreen " +
                       "'$BuildDir/$name' $fn 2>&1")
    $text = $run.Output
    $testPassed = $text -match "Totals: \d+ passed, 0 failed"
    $reports = Split-TsanReports -Text $text -RepoMarker $RepoLeaf
    $ours = @($reports | Where-Object { $_.FirstParty })
    $theirs = @($reports | Where-Object { -not $_.FirstParty })
    $thirdPartyCount += $theirs.Count

    if (-not $testPassed) {
        $firstPartyFailures += "$name (the test itself failed)"
        Write-Host $text
    }
    foreach ($report in $ours) {
        $firstPartyFailures += "$name : $($report.Kind)"
        Write-Host ($report.Lines -join "`n")
    }
    $verdict = if ($ours.Count -eq 0 -and $testPassed) { "clean" } else { "FAILED" }
    Write-Host ("  {0} {1}: {2} (first-party reports: {3}; third-party/Qt reports ignored: {4})" `
                -f $name, $fn, $verdict, $ours.Count, $theirs.Count)
    foreach ($report in $theirs) {
        Write-Host "      third-party: $($report.Kind)"
    }
}

Write-Host ""
if ($firstPartyFailures.Count -eq 0) {
    Write-Host (("TSAN CLEAN: {0} unit(s), no data race, thread leak or lock-order inversion in " +
                 "first-party code. {1} third-party report(s) from the uninstrumented Qt/QtTest " +
                 "watchdog were seen and ignored by design.") -f $Targets.Count, $thirdPartyCount)
    Write-Host "SCOPE: only the units in tests/tsan/CMakeLists.txt -- not whole-application coverage."
    exit 0
}
Write-Error ("TSAN FAILED: " + ($firstPartyFailures -join "; "))
exit 1
