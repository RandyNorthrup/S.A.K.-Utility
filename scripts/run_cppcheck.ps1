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

.PARAMETER SuppressionsFile
    Optional override for the suppressions list. Defaults to the repository's
    cppcheck_suppressions.txt. cppcheck_whole_project_gate.ps1 passes a stripped copy
    (unmatchedSuppression and useStlAlgorithm removed) for its stale-suppression reveal
    pass -- see that script's header for why those two entries have to come out.

.PARAMETER ErrorsFile
    Optional override for where cppcheck's stderr is written. Defaults to
    build/cppcheck_errors.txt. The gate runs two passes and needs each one's findings kept
    apart; without this the second pass overwrites the first pass's evidence.

.PARAMETER EnabledChecks
    Cppcheck check group to enable. Defaults to all. The dead-code gate requests only
    unusedFunction so its candidate stream is bounded and machine-parseable.

.PARAMETER Jobs
    Cppcheck worker count. Defaults to 4. unusedFunction requires a single-process run unless
    a cppcheck build directory is supplied, so the dead-code gate explicitly passes 1.

.PARAMETER SuppressFindingOutput
    Keep findings in ErrorsFile without echoing every line. Used by gates that parse the file
    themselves and print only their actionable subset.
#>

# THE PARAMETER BLOCK IS LOAD-BEARING. Read this before changing it.
#
# `powershell.exe -File script.ps1 -Files a.cpp b.cpp c.cpp` does NOT hand the whole run of
# values to a [string[]] parameter. It binds `a.cpp` and then treats b.cpp and c.cpp as
# separate positional arguments. Measured, with a plain `param([string[]]$Files)`:
#
#     -File probe.ps1 -Files a.cpp b.cpp c.cpp d.cpp   ->   Files = a.cpp
#     -File probe.ps1        a.cpp b.cpp c.cpp d.cpp   ->   Files = a.cpp
#
# Silently. No error. That is exactly how the pre-commit hooks call these scripts, so for as
# long as this signature stood, the cppcheck hook analyzed ONE FILE per invocation while
# printing PASSED -- which is a large part of why 84 whole-project findings accumulated under a
# gate advertised as "cppcheck on changed files".
#
# Adding SuppressionsFile and ErrorsFile made the same bug briefly LOUD rather than silent:
# the second staged file bound to $SuppressionsFile, and cppcheck died with
# `Failed to add suppression. Invalid id "namespace sak {"`. Loud was lucky. The dangerous
# shape is a value that happens to be a plausible path.
#
# PositionalBinding=$false alone is not the fix either -- it turns the silent drop into a hard
# "A positional parameter cannot be found that accepts argument 'b.cpp'" for the hook's normal
# case. ValueFromRemainingArguments is the piece that makes it work: every unbound value is
# collected into $Files, while -SuppressionsFile and -ErrorsFile can only be reached by name.
#
# Verified against BOTH call styles this repo uses:
#   pre-commit  : -File run_cppcheck.ps1 a.cpp b.cpp c.cpp        -> all three
#   CI / manual : & ./run_cppcheck.ps1 -Files $arr -ErrorsFile x  -> all of $arr
#
# The hook entry in .pre-commit-config.yaml must NOT pass `-Files`: naming the parameter binds
# only the value right after it and strands the rest.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files,

    [string]$SuppressionsFile,

    [string]$ErrorsFile,

    [ValidateSet("all", "unusedFunction")]
    [string]$EnabledChecks = "all",

    [ValidateRange(1, 64)]
    [int]$Jobs = 4,

    [switch]$SuppressFindingOutput
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ([string]::IsNullOrWhiteSpace($SuppressionsFile)) {
    $SuppressionsFile = Join-Path $ProjectRoot "cppcheck_suppressions.txt"
} elseif (-not (Test-Path -LiteralPath $SuppressionsFile -PathType Leaf)) {
    # Fail closed. A missing override would otherwise silently fall through to the
    # "no suppressions file" branch below and run with NO suppressions at all, which
    # reports hundreds of known tool limits and reads as a catastrophic regression.
    throw "SuppressionsFile not found: $SuppressionsFile"
}

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
    "--enable=$EnabledChecks"       # All checks normally; a named focused gate may narrow this
    "--check-level=exhaustive"      # Deepest analysis (slowest, most thorough)
    "--inconclusive"                # Report uncertain findings
    "--std=c++23"                   # C++23 standard
    "--error-exitcode=1"            # Exit 1 on any error/warning
    "--inline-suppr"                # Respect // cppcheck-suppress comments
    "--language=c++"                # Force C++ language
    "--max-configs=12"              # Check up to 12 configurations
    "--quiet"                       # Suppress progress messages
    "-j", "$Jobs"                   # Parallel normally; unusedFunction gate deliberately uses 1
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

    # Hand the paths to cppcheck through --file-list, NOT as positional arguments.
    #
    # Windows CreateProcess caps a command line at 32,767 characters. The whole-project gate's
    # reveal pass resolves 558 first-party sources, whose absolute paths alone are 41,549
    # characters -- so passing them positionally silently fails to launch cppcheck, and the
    # caller sees an empty findings file rather than an error. That is how a gate reports a
    # clean run having analyzed nothing; it was caught by a planted-defect drill, not by any
    # ordinary run, because every list short enough to test by hand fits.
    #
    # This is unconditional rather than a length check on purpose: a size-dependent branch is
    # exactly the shape that works in every small test and breaks only at full scale.
    #
    # The option-shaped filter above still matters. Values in a file list are never parsed as
    # options, so the file itself cannot inject one -- but the filter is what guarantees that,
    # and it also keeps the two branches honest about what a "file" is.
    # The list is written with an ASCII encoding, so a path carrying a byte above 0x7F would be
    # silently replaced with '?' and cppcheck would look for a file that does not exist -- one
    # fewer file analyzed, no error. Refuse instead. (The gate's own run is PID-suffixed so two
    # concurrent invocations, e.g. the pre-commit hook and the manual gate, cannot clobber each
    # other's list and analyze the wrong set.)
    $nonAscii = $CppFiles | Where-Object { $_ -match '[^\x00-\x7F]' }
    if ($nonAscii) {
        throw ("Refusing to build a cppcheck file list: these paths contain non-ASCII bytes " +
               "and would be corrupted by the ASCII-encoded list: " + ($nonAscii -join ', '))
    }

    # Written to the system temp dir, not into the repo: this runs inside the pre-commit hook,
    # and a hook that writes a file under the working tree is a hook that can be reported as
    # having modified the commit. PID-suffixed so the hook and a concurrent manual gate run
    # cannot overwrite each other's list and silently analyze the wrong set.
    $FileListPath = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_cppcheck_files_{0}.txt" -f $PID)
    Set-Content -LiteralPath $FileListPath -Value $CppFiles -Encoding ascii
    $CppcheckArgs += "--file-list=$FileListPath"
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

if ([string]::IsNullOrWhiteSpace($ErrorsFile)) {
    $ErrorsFile = Join-Path $ProjectRoot "build\cppcheck_errors.txt"
}
$errDir = Split-Path -Parent $ErrorsFile
if ($errDir -and -not (Test-Path -LiteralPath $errDir -PathType Container)) {
    New-Item -ItemType Directory -Path $errDir -Force | Out-Null
}

$process = Start-Process -FilePath $CppcheckExe `
    -ArgumentList $CppcheckArgs `
    -NoNewWindow -PassThru -Wait `
    -RedirectStandardError $ErrorsFile

$exitCode = $process.ExitCode
$errFile = $ErrorsFile

if ((Test-Path $errFile) -and -not $SuppressFindingOutput) {
    $errors = Get-Content $errFile -ErrorAction SilentlyContinue
    if ($errors) {
        Write-Host ""
        Write-Host "cppcheck findings:" -ForegroundColor Red
        $errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    }
}

if ($exitCode -ne 0) {
    if (-not $SuppressFindingOutput) {
        Write-Host ""
        Write-Host "FAILED: cppcheck found issues (exit code $exitCode)" -ForegroundColor Red
    }
    exit $exitCode
}

Write-Host "PASSED: cppcheck analysis clean" -ForegroundColor Green
exit 0
