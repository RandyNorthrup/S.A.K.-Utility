<#
.SYNOPSIS
    Strict cppcheck analysis for S.A.K. Utility.

.DESCRIPTION
    Runs cppcheck with the strictest settings:
      - All checks enabled (style, performance, portability, warning, information)
      - Exhaustive check level (deepest analysis)
      - Inconclusive findings reported
      - C/C++ language inferred from each input (C++ translation units use C++23)
      - Error exit code on any finding
      - Inline suppressions respected
      - Exact Qt test-slot functionStatic findings source-validated, classified, and reported

    Usage:
        .\scripts\run_cppcheck.ps1                       # Full project scan
        .\scripts\run_cppcheck.ps1 -Files file1.c,file2.cpp  # Specific files
        .\scripts\run_cppcheck.ps1 -Files (git diff ...)  # Changed files only

.PARAMETER Files
    Optional list of specific files to check. If omitted, scans src/, include/, and tests/.

.PARAMETER SuppressionsFile
    Optional override for the suppressions list. Defaults to the repository's
    cppcheck_suppressions.txt. cppcheck_whole_project_gate.ps1 passes a stripped copy
    (unmatchedSuppression and useStlAlgorithm removed) for its stale-suppression reveal
    pass -- see that script's header for why those two entries have to come out.

.PARAMETER ErrorsFile
    Optional override for where cppcheck's stderr is written. Without an override, a
    process-unique system-temp file is used and removed after capture. The gate runs two
    passes and supplies explicit paths because it needs each one's findings kept apart.

.PARAMETER EnabledChecks
    Cppcheck check group to enable. Defaults to all. The dead-code gate requests only
    unusedFunction so its candidate stream is bounded and machine-parseable.

.PARAMETER Jobs
    Cppcheck worker count. Defaults to 4. unusedFunction requires a single-process run unless
    a cppcheck build directory is supplied, so the dead-code gate explicitly passes 1.

.PARAMETER SuppressFindingOutput
    Keep findings in ErrorsFile without echoing every line. Used by gates that parse the file
    themselves and print only their actionable subset.

.PARAMETER PrintInputScope
    Test-only seam. Prints the resolved scope and exits before looking up or running cppcheck.
    The pre-commit hook never passes this switch.
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

    [switch]$SuppressFindingOutput,

    # Test seam: resolve and print the real cppcheck input scope, then exit before tool lookup.
    # The pre-commit hook never passes this switch.
    [switch]$PrintInputScope
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$processCaptureScript = Join-Path $PSScriptRoot "process_capture.ps1"
if (-not (Test-Path -LiteralPath $processCaptureScript -PathType Leaf)) {
    throw "Required process capture helper not found: $processCaptureScript"
}
. $processCaptureScript

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

# ---------------------------------------------------------------------------
# Build command arguments -- STRICTEST settings
# ---------------------------------------------------------------------------
$CppcheckArgs = @(
    "--enable=$EnabledChecks"       # All checks normally; a named focused gate may narrow this
    "--check-level=exhaustive"      # Deepest analysis (slowest, most thorough)
    "--inconclusive"                # Report uncertain findings
    "--std=c++23"                   # C++23 for C++ TUs; cppcheck infers C from .c
    "--error-exitcode=1"            # Exit 1 on any error/warning
    "--inline-suppr"                # Respect // cppcheck-suppress comments
    "--max-configs=12"              # Check up to 12 configurations
    "--quiet"                       # Suppress progress messages
    "--template={file}:{line}:{column}: {severity}: {message} [{id}]"
    "-j", "$Jobs"                   # Parallel normally; unusedFunction gate deliberately uses 1
    "-I", (Join-Path $ProjectRoot "include")
    "-I", (Join-Path $ProjectRoot "src")
    # Resolve the tracked QR header used by first-party code.
    "-I", (Join-Path $ProjectRoot "src/third_party/qrcodegen")
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
$FileListPath = ""
$InputScopeKind = "whole-project-with-tests"
$InputScopePaths = @()
if ($Files -and $Files.Count -gt 0) {
    # Filter to real C/C++ source files only. Each value is passed straight to cppcheck, so
    # reject anything option-shaped (leading '-') or that does not exist as a real file on
    # disk. Without this, a value such as "--suppress=*:*.cpp" or "--addon=evil.cpp" ends in
    # a C/C++ extension and would be handed to cppcheck as an option, not a file.
    $SourceFiles = $Files | Where-Object {
        $candidate = $_
        if ([string]::IsNullOrWhiteSpace($candidate)) { return $false }
        if ($candidate.StartsWith("-")) { return $false }
        $ext = [System.IO.Path]::GetExtension($candidate).ToLower()
        ($ext -in @(".c", ".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx")) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)
    }
    if ($SourceFiles.Count -eq 0) {
        Write-Host "No C/C++ files to check."
        exit 0
    }

    # Resolve each path against PowerShell's location NOW. cppcheck is a child process and
    # [System.IO.Path]::GetFullPath is .NET; both resolve a relative path against the PROCESS
    # working directory, which Set-Location does not move. An in-process caller that had
    # Set-Location'd into a second checkout passed Test-Path above for its own file and then had
    # cppcheck analyze the same relative path under the process directory -- the other
    # checkout's copy -- and report on code it was never asked about.
    $SourceFiles = @($SourceFiles | ForEach-Object { (Resolve-Path -LiteralPath $_).ProviderPath })

    # A header is not a translation unit. Passing one to cppcheck by itself makes
    # unusedStructMember report every member whose use lives in the corresponding .cpp as dead.
    # Analyze changed headers through all real first-party translation units instead. Include
    # tests because support/test headers can be consumed only there. This is intentionally the
    # safe, expensive direction: a header edit must never regain a broad dead-member silence.
    $headerExtensions = @(".h", ".hpp", ".hxx")
    $containsHeader = @($SourceFiles | Where-Object {
        [System.IO.Path]::GetExtension($_).ToLower() -in $headerExtensions
    }).Count -gt 0

    if ($containsHeader) {
        $InputScopeKind = "whole-project-with-tests"
        $InputScopePaths = @(
            (Join-Path $ProjectRoot "src"),
            (Join-Path $ProjectRoot "include"),
            (Join-Path $ProjectRoot "tests")
        )
        foreach ($scopePath in $InputScopePaths) {
            if (-not (Test-Path -LiteralPath $scopePath -PathType Container)) {
                throw "Required cppcheck translation-unit scope is missing: $scopePath"
            }
        }
        $scopePrefixes = @($InputScopePaths | ForEach-Object {
            [System.IO.Path]::GetFullPath($_).TrimEnd('\', '/') +
                [System.IO.Path]::DirectorySeparatorChar
        })
        foreach ($header in @($SourceFiles | Where-Object {
            [System.IO.Path]::GetExtension($_).ToLower() -in $headerExtensions
        })) {
            $headerPath = [System.IO.Path]::GetFullPath($header)
            $insideFirstPartyScope = @($scopePrefixes | Where-Object {
                $headerPath.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase)
            }).Count -gt 0
            if (-not $insideFirstPartyScope) {
                throw "Changed header is outside the first-party cppcheck scope: $header"
            }
        }
        $CppcheckArgs += "-i"
        $CppcheckArgs += (Join-Path (Join-Path $ProjectRoot "src") "third_party")
        $CppcheckArgs += $InputScopePaths
    } else {
        $InputScopeKind = "file-list"
        $InputScopePaths = @($SourceFiles)

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
        $nonAscii = $SourceFiles | Where-Object { $_ -match '[^\x00-\x7F]' }
        if ($nonAscii) {
            throw ("Refusing to build a cppcheck file list: these paths contain non-ASCII bytes " +
                   "and would be corrupted by the ASCII-encoded list: " + ($nonAscii -join ', '))
        }

        if (-not $PrintInputScope) {
            # Written to the system temp dir, not into the repo: this runs inside the pre-commit
            # hook, and a hook that writes under the working tree can be reported as modifying the
            # commit. PID-suffixed so concurrent hook/manual runs cannot overwrite each other.
            $FileListPath = Join-Path ([System.IO.Path]::GetTempPath()) `
                ("sak_cppcheck_files_{0}.txt" -f $PID)
            Set-Content -LiteralPath $FileListPath -Value $SourceFiles -Encoding ascii
            $CppcheckArgs += "--file-list=$FileListPath"
        }
    }
} else {
    # Full first-party scan -- production sources, public headers, and test translation units.
    # Tests are executable C/C++ and can carry the same safety/style findings as production code;
    # omitting them made the whole-project claim false while the suppression reveal happened to
    # analyze them in a separate pass.
    $CppcheckArgs += "-i"
    # Nested, not Join-Path $ProjectRoot "src" "third_party": the three-argument form needs the
    # -AdditionalChildPath parameter added in PowerShell 7, and the pre-commit hook (and the
    # documented manual invocation) run this script under Windows PowerShell 5.1, where the third
    # positional argument is an error -- so the whole-project branch died with a PowerShell
    # argument exception before cppcheck was ever launched.
    $CppcheckArgs += (Join-Path (Join-Path $ProjectRoot "src") "third_party")
    $InputScopePaths = @(
        (Join-Path $ProjectRoot "src"),
        (Join-Path $ProjectRoot "include"),
        (Join-Path $ProjectRoot "tests")
    )
    foreach ($scopePath in $InputScopePaths) {
        if (-not (Test-Path -LiteralPath $scopePath -PathType Container)) {
            throw "Required cppcheck whole-project scope is missing: $scopePath"
        }
    }
    $CppcheckArgs += $InputScopePaths
}

if ($PrintInputScope) {
    Write-Output "CPPCHECK_INPUT_SCOPE=$InputScopeKind"
    foreach ($scopePath in $InputScopePaths) {
        Write-Output "CPPCHECK_INPUT_PATH=$scopePath"
    }
    exit 0
}

# ---------------------------------------------------------------------------
# Execute
# ---------------------------------------------------------------------------
$CppcheckExe = Find-Cppcheck
Write-Host "Running cppcheck (strictest settings)..." -ForegroundColor Cyan
Write-Host "  Command: cppcheck $($CppcheckArgs -join ' ')" -ForegroundColor DarkGray

if ([string]::IsNullOrWhiteSpace($ErrorsFile)) {
    # Pre-commit may start more than one hook process. A shared redirect file lets those
    # processes truncate and overwrite each other's diagnostics, producing NUL-corrupted
    # output and false failures with no actionable finding. Keep capture process-local.
    $ErrorsFile = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("sak_cppcheck_errors_{0}.txt" -f $PID)
    $removeDefaultErrorsFile = $true
} else {
    $removeDefaultErrorsFile = $false
}
$errDir = Split-Path -Parent $ErrorsFile
if ($errDir -and -not (Test-Path -LiteralPath $errDir -PathType Container)) {
    New-Item -ItemType Directory -Path $errDir -Force | Out-Null
}
$rawErrorsFile = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("sak_cppcheck_raw_errors_{0}.txt" -f $PID)
$classifier = Join-Path $PSScriptRoot "classify_cppcheck_qt_slots.py"

try {
    # Start-Process -PassThru can return a null ExitCode after redirected execution under
    # Windows PowerShell 5.1. Use the shared async-drain helper so a stale/null code cannot turn
    # a real cppcheck failure into an ambiguous result.
    $captured = Invoke-CapturedProcess -FilePath $CppcheckExe -Arguments $CppcheckArgs
    if (-not [string]::IsNullOrWhiteSpace($captured.stdout)) {
        Write-Host $captured.stdout
    }
    Set-Content -LiteralPath $rawErrorsFile -Value $captured.stderr -Encoding utf8

    # Do not hide functionStatic at cppcheck invocation. Qt Test callbacks declared under
    # Q_SLOTS are a real tool limitation: making them static removes them from Qt's meta-object
    # table. Source-classify only exact Class::method findings at their reported definition line.
    # Any ordinary helper, parser ambiguity, missing Python, or classifier failure stays red.
    $rawLines = @()
    if (Test-Path -LiteralPath $rawErrorsFile -PathType Leaf) {
        $rawLines = @(Get-Content -LiteralPath $rawErrorsFile)
    }
    $hasFunctionStatic = @($rawLines | Where-Object { $_ -match '\[functionStatic\]$' }).Count -gt 0
    $classifiedQtSlots = 0
    $classificationFailed = $false

    if ($hasFunctionStatic) {
        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python) {
            $python = Get-Command python -ErrorAction SilentlyContinue
        }

        if (-not $python -or -not (Test-Path -LiteralPath $classifier -PathType Leaf)) {
            Write-Host "Qt slot classification unavailable; functionStatic findings stay actionable." `
                -ForegroundColor Red
            $classificationFailed = $true
            Copy-Item -LiteralPath $rawErrorsFile -Destination $ErrorsFile -Force
        } else {
            $classifierOutput = @(& $python.Source $classifier `
                --input $rawErrorsFile --output $ErrorsFile --project-root $ProjectRoot)
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Qt slot classifier failed; functionStatic findings stay actionable." `
                    -ForegroundColor Red
                $classificationFailed = $true
                Copy-Item -LiteralPath $rawErrorsFile -Destination $ErrorsFile -Force
            } else {
                $classifiedLine = $classifierOutput |
                    Where-Object { $_ -match '^CLASSIFIED_QT_SLOT_FUNCTION_STATIC=(\d+)$' } |
                    Select-Object -First 1
                if (-not $classifiedLine) {
                    Write-Host "Qt slot classifier emitted no count; refusing a silent pass." `
                        -ForegroundColor Red
                    $classificationFailed = $true
                    Copy-Item -LiteralPath $rawErrorsFile -Destination $ErrorsFile -Force
                } else {
                    $classifiedQtSlots = [int]($classifiedLine -replace '^.*=', '')
                }
            }
        }
    } else {
        Copy-Item -LiteralPath $rawErrorsFile -Destination $ErrorsFile -Force
    }

    if (-not (Test-Path -LiteralPath $ErrorsFile -PathType Leaf)) {
        throw "Cppcheck diagnostic classification wrote no output file: $ErrorsFile"
    }
    try {
        $errors = @(Get-Content -LiteralPath $ErrorsFile -ErrorAction Stop)
    } catch {
        throw "Failed to read classified cppcheck diagnostics '$ErrorsFile': $($_.Exception.Message)"
    }
    $remainingSignals = @($errors | Where-Object {
        (-not [string]::IsNullOrWhiteSpace($_)) -and ($_ -notmatch '\[checkersReport\]$')
    })
    $rawSignals = @($rawLines | Where-Object {
        (-not [string]::IsNullOrWhiteSpace($_)) -and ($_ -notmatch '\[checkersReport\]$')
    })
    $hasCheckersReport = @($rawLines | Where-Object {
        $_ -match '\[checkersReport\]$'
    }).Count -gt 0

    $exitCode = $captured.exit_code
    # --error-exitcode=1 reserves exactly 1 for findings. Normalize only that code, only after
    # a readable classified stream proves every raw signal was one exact Qt slot finding, and
    # only when cppcheck emitted its completion report. Tool/startup/crash exits remain red.
    if (-not $classificationFailed -and $classifiedQtSlots -gt 0 -and
        $hasCheckersReport -and $rawSignals.Count -eq $classifiedQtSlots -and
        $exitCode -eq 1 -and $remainingSignals.Count -eq 0) {
        $exitCode = 0
    }
    if ($classifiedQtSlots -gt 0) {
        Write-Host ("  Classified $classifiedQtSlots exact Qt Q_SLOTS functionStatic " +
                    "finding(s); raw findings were analyzed, not blanket-suppressed.") `
            -ForegroundColor DarkGray
    }

    $errFile = $ErrorsFile

    if ((Test-Path $errFile) -and -not $SuppressFindingOutput) {
        if ($errors) {
            Write-Host ""
            Write-Host "cppcheck findings:" -ForegroundColor Red
            $errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        }
    }
} finally {
    if ($removeDefaultErrorsFile -and (Test-Path -LiteralPath $ErrorsFile -PathType Leaf)) {
        Remove-Item -LiteralPath $ErrorsFile -Force
    }
    if (-not [string]::IsNullOrWhiteSpace($FileListPath) -and
        (Test-Path -LiteralPath $FileListPath -PathType Leaf)) {
        Remove-Item -LiteralPath $FileListPath -Force
    }
    if (Test-Path -LiteralPath $rawErrorsFile -PathType Leaf) {
        Remove-Item -LiteralPath $rawErrorsFile -Force
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
