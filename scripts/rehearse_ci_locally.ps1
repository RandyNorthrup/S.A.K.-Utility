# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Rehearse the CI-only checks locally, for free.
#
# WHY THIS EXISTS
# GitHub Actions minutes cost money, and Windows runners bill at a 2x multiplier,
# so pushes here are deliberate and infrequent. That is a sound cost decision, but
# it has a consequence: every clean-environment problem accumulated since the last
# push arrives in a single paid run, and discovering them one at a time through a
# red-fix-push-red cycle is the single most expensive way to find them.
#
# The cost-correct answer is not to push more often. It is to run the checks that
# only CI performs HERE, where they are free, so the paid run has the best chance
# of passing first time.
#
# WHAT CI CATCHES THAT A NORMAL LOCAL BUILD CANNOT
#   1. FRESH CLONE       - only committed files exist. Anything gitignored or
#                          untracked that the build quietly needs is missing.
#   2. PACKAGED ARTIFACT - the release zip, extracted clean. In-tree ctest runs
#                          binaries surrounded by every DLL on this machine, so
#                          missing DLLs, missing resources and wrong relative
#                          paths are invisible to it by construction.
#   3. FULL HISTORY      - gitleaks scans all history; the pre-commit hook only
#                          ever sees staged files.
#   4. CLEAN RUNNER      - no Qt, no vcpkg, no LLVM, no Python, no bundled tools.
#                          This one CANNOT be reproduced on a developer machine,
#                          and is the residual risk this script does not remove.
#
# Fails closed: any failing phase stops the run with a non-zero exit code. A phase
# that cannot run (missing tool, missing input) is a FAILURE, never a silent skip,
# because "the rehearsal passed" has to mean the checks actually executed.

[CmdletBinding()]
param(
    # Also clone HEAD into a scratch directory and build THAT, so only committed
    # files are present. This is the only way to catch the gitignored-input class
    # locally. It costs a full second build, so it is opt-in for the fast loop and
    # should always be used before the real push.
    [switch]$FreshClone,

    # Where the fresh clone is built. Kept outside the repo so it cannot pollute it.
    [string]$ScratchRoot = "$env:TEMP\sak-ci-rehearsal",

    # Skip the packaging chain (use when only the fresh-clone build is wanted).
    [switch]$SkipPackaging
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$failures = New-Object System.Collections.Generic.List[string]

# Rebuild PATH from the registry before anything runs. A CI runner starts from a fresh
# login environment; a long-lived local shell does not, so a tool installed after that
# shell started (ripgrep is the recurring one) is absent here and present on CI. The
# gates fail closed on a missing tool - correctly - so without this the rehearsal
# reports a tool problem that CI would never see, and hides whatever came after it.
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:PATH = (@($machinePath, $userPath) | Where-Object { $_ }) -join ';'
$phases = New-Object System.Collections.Generic.List[string]

function Start-Phase([string]$name) {
    Write-Host ''
    Write-Host "=== $name ===" -ForegroundColor Cyan
    $phases.Add($name)
}

function Assert-LastExit([string]$what) {
    if ($LASTEXITCODE -ne 0) {
        $failures.Add("$what (exit $LASTEXITCODE)")
        Write-Host "FAILED: $what (exit $LASTEXITCODE)" -ForegroundColor Red
    }
}

<#
.SYNOPSIS
    Run a repo .ps1 in a CHILD pwsh and return its real exit code.

.DESCRIPTION
    Do not call these scripts with the call operator. `& script.ps1` runs the script
    in THIS process, and PowerShell does not set $LASTEXITCODE for a script that
    returns without calling exit - the variable keeps whatever the last NATIVE
    command left behind. Several of these scripts shell out to dumpbin, 7z and git,
    so a script that printed "created successfully" was reported here as
    "FAILED (exit 1)" purely because some tool inside it had returned non-zero on a
    probe it handled.

    A child process has an unambiguous exit code, and it is also how the workflow
    invokes them, so the rehearsal matches CI rather than approximating it.
#>
function Invoke-RepoScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [string[]]$ScriptArgs = @()
    )
    # Out-Host, not a bare call. A PowerShell function returns EVERYTHING that reaches the
    # pipeline, so without this the child's stdout is prepended to the exit code and the
    # caller receives an Object[] instead of an int. Out-Host writes the child's output
    # where a reader expects it while keeping it out of the return value.
    & pwsh -NoProfile -File $ScriptPath @ScriptArgs | Out-Host
    return $LASTEXITCODE
}

function Assert-ScriptExit([string]$what, [int]$code) {
    if ($code -ne 0) {
        $failures.Add("$what (exit $code)")
        Write-Host "FAILED: $what (exit $code)" -ForegroundColor Red
    }
}

# ---------------------------------------------------------------------------
# Version, used for the package name exactly as the workflow computes it.
# ---------------------------------------------------------------------------
$versionFile = Join-Path $RepoRoot 'VERSION'
if (-not (Test-Path $versionFile)) {
    Write-Error "Cannot rehearse: $versionFile is missing, so the package name CI builds cannot be reproduced."
    exit 1
}
$appVersion = (Get-Content $versionFile -Raw).Trim()
$pkgName = "SAK-Utility-v$appVersion"
Write-Host "Rehearsing CI for version $appVersion (package $pkgName)"

# ---------------------------------------------------------------------------
# PHASE 1 - fresh clone build. Only committed files.
# ---------------------------------------------------------------------------
if ($FreshClone) {
    Start-Phase 'Fresh clone build (committed files only)'
    if (Test-Path $ScratchRoot) {
        Remove-Item $ScratchRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null
    $cloneDir = Join-Path $ScratchRoot 'repo'

    # Clone from the local repository, not the remote: the point is to test the
    # COMMITTED state of this working tree, including commits never pushed.
    git clone --local --no-hardlinks "$RepoRoot" "$cloneDir" 2>&1 | Out-Null
    Assert-LastExit 'git clone of the local repository'

    if (Test-Path $cloneDir) {
        $missing = @()
        foreach ($needed in @('CMakeLists.txt', 'tests/CMakeLists.txt', 'VERSION')) {
            if (-not (Test-Path (Join-Path $cloneDir $needed))) { $missing += $needed }
        }
        if ($missing.Count -gt 0) {
            $failures.Add("Fresh clone is missing committed files: $($missing -join ', ')")
        }

        $cloneBuild = Join-Path $cloneDir 'build'
        # Mirror the workflow's configure line, including the declared triplet.
        cmake -B $cloneBuild -S $cloneDir -G "Visual Studio 17 2022" -A x64 `
            -DCMAKE_PREFIX_PATH="$env:SAK_QT_PREFIX" `
            -DVCPKG_APPLOCAL_DEPS=OFF `
            -DVCPKG_TARGET_TRIPLET=x64-windows `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
        Assert-LastExit 'cmake configure of the fresh clone'

        cmake --build $cloneBuild --config Release --parallel
        Assert-LastExit 'cmake build of the fresh clone'
    }
}

# ---------------------------------------------------------------------------
# PHASE 2 - the packaging chain, exactly as the workflow runs it. This is the
# part that is currently RED in CI, and every step of it is a repo script.
# ---------------------------------------------------------------------------
if (-not $SkipPackaging) {
    $buildDir = Join-Path $RepoRoot 'build/Release'
    if (-not (Test-Path (Join-Path $buildDir 'sak_utility.exe'))) {
        Write-Error "Cannot rehearse packaging: $buildDir/sak_utility.exe is missing. Build Release first."
        exit 1
    }

    Start-Phase 'Stage portable release'
    Assert-ScriptExit 'stage_portable_release.ps1' (Invoke-RepoScript (Join-Path $RepoRoot 'scripts/stage_portable_release.ps1') @('-BuildDir', $buildDir, '-PackageName', $pkgName))

    Start-Phase 'Create release archive'
    Assert-ScriptExit 'create_release_archive.ps1' (Invoke-RepoScript (Join-Path $RepoRoot 'scripts/create_release_archive.ps1') @('-BuildDir', $buildDir, '-PackageName', $pkgName))

    $zipPath = Join-Path $buildDir "$pkgName-Windows-x64.zip"
    if (-not (Test-Path $zipPath)) {
        $failures.Add("Release archive was not produced at $zipPath")
    } else {
        # Extract clean. Everything after this point sees ONLY what shipped in the
        # zip, which is the whole point of the exercise.
        $extract = Join-Path $buildDir 'clean-smoke-extract'
        if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $extract | Out-Null
        Expand-Archive -LiteralPath $zipPath -DestinationPath $extract -Force

        Start-Phase 'Smoke test clean portable package'
        Assert-ScriptExit 'verify_portable_release_smoke.ps1' (Invoke-RepoScript (Join-Path $RepoRoot 'scripts/verify_portable_release_smoke.ps1') @('-PackageRoot', $extract, '-RepoRoot', $RepoRoot))

        Start-Phase 'Startup E2E smoke from clean extracted package'
        Assert-ScriptExit 'run_portable_e2e_smoke.ps1' (Invoke-RepoScript (Join-Path $RepoRoot 'scripts/run_portable_e2e_smoke.ps1') @('-PackageRoot', $extract))

        Start-Phase 'Release readiness gate from clean extracted package'
        Assert-ScriptExit 'check_release_readiness.ps1' (Invoke-RepoScript (Join-Path $RepoRoot 'scripts/check_release_readiness.ps1') @('-PackageRoot', $extract))
    }
}

# ---------------------------------------------------------------------------
# PHASE 3 - full-history secret scan. The pre-commit hook only sees staged files,
# so a secret committed months ago is invisible to it forever.
# ---------------------------------------------------------------------------
Start-Phase 'Full-history secret scan (gitleaks)'
$gitleaks = (Get-Command gitleaks -ErrorAction SilentlyContinue)
if ($null -eq $gitleaks) {
    # Fail rather than skip: this is one of the two gates currently red in CI, so
    # "not checked" must not read as "passed".
    $failures.Add('gitleaks is not installed, so the full-history secret scan did NOT run. Install it (winget install gitleaks) - this is one of the two gates currently failing in CI.')
    Write-Host 'FAILED: gitleaks not installed; full-history scan did not run.' -ForegroundColor Red
} else {
    & $gitleaks.Source detect --source $RepoRoot --redact --no-banner
    Assert-LastExit 'gitleaks detect over full history'
}

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== CI rehearsal summary ===' -ForegroundColor Cyan
Write-Host "Phases run: $($phases.Count)"
if (-not $FreshClone) {
    Write-Host 'NOTE: -FreshClone was not used, so the committed-files-only build was NOT exercised.' -ForegroundColor Yellow
    Write-Host '      Run with -FreshClone before the real push.' -ForegroundColor Yellow
}
Write-Host 'RESIDUAL RISK this cannot cover: a genuinely clean runner with no Qt, vcpkg,' -ForegroundColor Yellow
Write-Host '      LLVM, Python or bundled tools preinstalled.' -ForegroundColor Yellow

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host "CI REHEARSAL FAILED - $($failures.Count) problem(s):" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'Fix these BEFORE pushing. Each one would otherwise cost a paid CI run to discover.'
    exit 1
}

Write-Host ''
Write-Host 'CI rehearsal passed.' -ForegroundColor Green
exit 0
