<#
.SYNOPSIS
    Strict cppcheck analysis for S.A.K. Utility.

.DESCRIPTION
    Runs cppcheck with the strictest settings:
      - All checks enabled (style, performance, portability, warning, information)
      - Exhaustive check level (deepest analysis)
      - Inconclusive findings reported
      - C++23 standard
      - Error exit code on any finding
      - Inline suppressions respected

    Usage:
        .\scripts\run_cppcheck.ps1                       # Full project scan
        .\scripts\run_cppcheck.ps1 -Files file1.cpp      # Specific files
        .\scripts\run_cppcheck.ps1 -Files (git diff ...)  # Changed files only

.PARAMETER Files
    Optional list of specific files to check. If omitted, scans src/ and include/.
#>
param(
    [string[]]$Files
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$SuppressionsFile = Join-Path $ProjectRoot "cppcheck_suppressions.txt"

function Find-Cppcheck {
    $fromPath = Get-Command cppcheck.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $candidatePaths = @(
        "$env:ProgramFiles\Cppcheck\cppcheck.exe",
        "${env:ProgramFiles(x86)}\Cppcheck\cppcheck.exe"
    )

    foreach ($candidate in $candidatePaths) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    throw "cppcheck.exe was not found. Install from https://cppcheck.sourceforge.io/"
}

$CppcheckExe = Find-Cppcheck

# ---------------------------------------------------------------------------
# Build command arguments -- STRICTEST settings
# ---------------------------------------------------------------------------
$CppcheckArgs = @(
    "--enable=all"                  # All check categories
    "--check-level=exhaustive"      # Deepest analysis (slowest, most thorough)
    "--inconclusive"                # Report uncertain findings
    "--std=c++23"                   # C++23 standard
    "--error-exitcode=1"            # Exit 1 on any error/warning
    "--inline-suppr"                # Respect // cppcheck-suppress comments
    "--language=c++"                # Force C++ language
    "--max-configs=12"              # Check up to 12 configurations
    "--quiet"                       # Suppress progress messages
    "-j", "4"                       # Parallel checking (4 threads)
    "-I", (Join-Path $ProjectRoot "include")
    "-I", (Join-Path $ProjectRoot "src")
)

# Add suppressions file if it exists
if (Test-Path $SuppressionsFile) {
    $CppcheckArgs += "--suppressions-list=$SuppressionsFile"
}

# Platform-specific: Windows 64-bit
$CppcheckArgs += "--platform=win64"

# Preprocessor defines matching our build.
#
# _WIN32 and _MSC_VER are defined implicitly by MSVC, never on a command line, so cppcheck
# does not see them unless they are supplied here. Without _WIN32 (and previously with
# --force, which checks EVERY #ifdef configuration) cppcheck analyzed the non-Windows
# branches of code that is compiled Windows-only. Those branches are frequently a bare
# "return false", which made cppcheck report a long list of phantom always-true conditions
# on real guards - including twelve on the elevated-pipe boundary - while spending its
# analysis budget on code that never compiles. --force is deliberately NOT used: this
# application is Windows-only, so the single Windows configuration is the real one.
$CppcheckArgs += @(
    "-DSAK_PLATFORM_WINDOWS"
    "-DQ_OS_WIN"
    "-D_WIN32"
    "-D_MSC_VER=1939"
    "-DQT_NO_KEYWORDS"
    "-DWIN32"
    "-D_WIN64"
    "-D_WINDOWS"
    "-DUNICODE"
    "-D_UNICODE"
    # Qt macros that cppcheck doesn't understand natively
    "-DQ_OBJECT="
    "-DQ_SLOTS="
    "-DQ_SIGNALS=protected"
    "-DQ_EMIT="
    "-DQ_ENUM(x)="
    "-DQTEST_SET_MAIN_SOURCE_PATH="
    # Q_DECLARE_METATYPE is the SAME failure as the lower-case keywords below, reached through a
    # different door: cppcheck reports it as `unknownMacro`, which it treats as a CRITICAL error
    # and which abandons the WHOLE translation unit ("Active checkers: 4/186") while this script
    # still prints "PASSED: cppcheck analysis clean" and exits 0. It sits in production headers
    # (quick_action.h, email_types.h, network_diagnostic_types.h and eight more), so every test
    # that includes one was unanalyzed: 65 of the 238 files under tests/unit, measured.
    "-DQ_DECLARE_METATYPE(x)="
)

# The lower-case spellings (slots, signals, emit) are deliberately NOT defined here.
# -DQT_NO_KEYWORDS above tells Qt not to define them, so a translation unit that still spells
# them lower-case hands cppcheck an unknown token inside a class body; cppcheck does not report
# that as a syntax error, it silently degrades to "Active checkers: 4/186" and finds nothing.
# Defining them looks like the fix and is not: they are ordinary identifiers elsewhere in this
# codebase (partition_apfs_writer.cpp declares a local QVector named "slots"), so -Dslots= turns
# real production code into a syntax error and takes the whole run down with it. The root cause
# is fixed at the source instead -- every test class spells its test section Q_SLOTS.

# ---------------------------------------------------------------------------
# Determine files to check
# ---------------------------------------------------------------------------
if ($Files -and $Files.Count -gt 0) {
    # Filter to real C++ source files only. Each value is passed straight to cppcheck, so
    # reject anything option-shaped (leading '-') or that does not exist as a real file on
    # disk. Without this, a value such as "--suppress=*:*.cpp" or "--addon=evil.cpp" ends in
    # a C++ extension and would be handed to cppcheck as an option, not a file.
    $CppFiles = $Files | Where-Object {
        $candidate = $_
        if ([string]::IsNullOrWhiteSpace($candidate)) { return $false }
        if ($candidate.StartsWith("-")) { return $false }
        $ext = [System.IO.Path]::GetExtension($candidate).ToLower()
        ($ext -in @(".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx")) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)
    }
    if ($CppFiles.Count -eq 0) {
        Write-Host "No C++ files to check."
        exit 0
    }
    $CppcheckArgs += $CppFiles
} else {
    # Full project scan -- exclude third-party code
    $CppcheckArgs += "-i"
    # Nested, not Join-Path $ProjectRoot "src" "third_party": the three-argument form needs the
    # -AdditionalChildPath parameter added in PowerShell 7, and the pre-commit hook (and the
    # documented manual invocation) run this script under Windows PowerShell 5.1, where the third
    # positional argument is an error -- so the whole-project branch died with a PowerShell
    # argument exception before cppcheck was ever launched.
    $CppcheckArgs += (Join-Path (Join-Path $ProjectRoot "src") "third_party")
    $CppcheckArgs += (Join-Path $ProjectRoot "src")
    $CppcheckArgs += (Join-Path $ProjectRoot "include")
}

# ---------------------------------------------------------------------------
# Execute
# ---------------------------------------------------------------------------
Write-Host "Running cppcheck (strictest settings)..." -ForegroundColor Cyan
Write-Host "  Command: cppcheck $($CppcheckArgs -join ' ')" -ForegroundColor DarkGray

$process = Start-Process -FilePath $CppcheckExe `
    -ArgumentList $CppcheckArgs `
    -NoNewWindow -PassThru -Wait `
    -RedirectStandardError (Join-Path $ProjectRoot "build\cppcheck_errors.txt")

$exitCode = $process.ExitCode
$errFile = Join-Path $ProjectRoot "build\cppcheck_errors.txt"

if (Test-Path $errFile) {
    $errors = Get-Content $errFile -ErrorAction SilentlyContinue
    if ($errors) {
        Write-Host ""
        Write-Host "cppcheck findings:" -ForegroundColor Red
        $errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    }
}

if ($exitCode -ne 0) {
    Write-Host ""
    Write-Host "FAILED: cppcheck found issues (exit code $exitCode)" -ForegroundColor Red
    exit $exitCode
}

Write-Host "PASSED: cppcheck analysis clean" -ForegroundColor Green
exit 0
