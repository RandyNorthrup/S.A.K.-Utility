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
$CppcheckExe = "C:\Program Files\Cppcheck\cppcheck.exe"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$SuppressionsFile = Join-Path $ProjectRoot "cppcheck_suppressions.txt"

if (-not (Test-Path $CppcheckExe)) {
    Write-Error "cppcheck not found at $CppcheckExe. Install from https://cppcheck.sourceforge.io/"
    exit 1
}

# ---------------------------------------------------------------------------
# Build command arguments — STRICTEST settings
# ---------------------------------------------------------------------------
$CppcheckArgs = @(
    "--enable=all"                  # All check categories
    "--check-level=exhaustive"      # Deepest analysis (slowest, most thorough)
    "--inconclusive"                # Report uncertain findings
    "--std=c++23"                   # C++23 standard
    "--error-exitcode=1"            # Exit 1 on any error/warning
    "--force"                       # Check all configurations
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

# Preprocessor defines matching our build
$CppcheckArgs += @(
    "-DSAK_PLATFORM_WINDOWS"
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
    "-DQTEST_SET_MAIN_SOURCE_PATH="
)

# ---------------------------------------------------------------------------
# Determine files to check
# ---------------------------------------------------------------------------
if ($Files -and $Files.Count -gt 0) {
    # Filter to C++ files only
    $CppFiles = $Files | Where-Object {
        $ext = [System.IO.Path]::GetExtension($_).ToLower()
        $ext -in @(".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx")
    }
    if ($CppFiles.Count -eq 0) {
        Write-Host "No C++ files to check."
        exit 0
    }
    $CppcheckArgs += $CppFiles
} else {
    # Full project scan
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
