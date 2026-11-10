<#
.SYNOPSIS
    Fails on high-confidence dead first-party function definitions in src/*.cpp.

.DESCRIPTION
    cppcheck's unusedFunction stream is intentionally noisy in this Qt application: test slots
    run through the meta-object system, and public/member APIs can be called across translation
    units in ways its parser does not resolve. This gate takes the reliable intersection:

      1. cppcheck parsed the definition and reported unusedFunction;
      2. the definition is in first-party src/*.cpp, not a public header or a test;
      3. the exact identifier occurs once across src/include/tests -- its definition, with no
         declaration, caller, function-pointer use, connect expression, or test reference.

    The intersection found the R5-G10-6 dead set with zero false positives. Header-only public
    API is deliberately outside this gate and remains an explicit review decision.

    Every run adds a private temporary unused function and requires cppcheck to report it. If
    unusedFunction is suppressed, disabled by parallel mode, or silently stops running, the
    gate fails instead of certifying zero findings.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$SourceRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot "src"))
$RunCppcheck = Join-Path $PSScriptRoot "run_cppcheck.ps1"
$rg = @(Get-Command rg -CommandType Application -ErrorAction SilentlyContinue)[0]
if (-not $rg) {
    throw "Required tool missing: rg (native executable)"
}

function Is-FirstPartySource([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.StartsWith("-")) {
        return $false
    }
    if ([System.IO.Path]::GetExtension($Path).ToLowerInvariant() -ne ".cpp") {
        return $false
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    $insideSource = $full.StartsWith(
        $SourceRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)
    return $insideSource -and $full -notmatch '\\(third_party|3rdparty|external|_archived)\\'
}

if ($Files -and $Files.Count -gt 0) {
    $CppFiles = @($Files | Where-Object { Is-FirstPartySource $_ } |
        ForEach-Object { [System.IO.Path]::GetFullPath($_) } | Sort-Object -Unique)
    if ($CppFiles.Count -eq 0) {
        Write-Host "Dead-code gate: no first-party src/*.cpp files to check."
        exit 0
    }
} else {
    $CppFiles = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -Filter *.cpp -File |
        Where-Object { Is-FirstPartySource $_.FullName } |
        ForEach-Object { $_.FullName } | Sort-Object -Unique)
    if ($CppFiles.Count -eq 0) {
        throw "Dead-code gate resolved to zero first-party src/*.cpp files."
    }
}

$temporaryRoot = [System.IO.Path]::GetTempPath()
$probePath = Join-Path $temporaryRoot "sak_dead_code_probe_$PID.cpp"
$probeName = "sakDeadCodeGateProbe"
$suppressionsPath = Join-Path $temporaryRoot "sak_dead_code_suppressions_$PID.txt"
$errorsPath = Join-Path $temporaryRoot "sak_dead_code_findings_$PID.txt"

try {
    Set-Content -LiteralPath $probePath -Encoding ascii -Value @(
        "namespace {",
        "void $probeName() {}",
        "}"
    )
    $suppressions = Get-Content -LiteralPath (Join-Path $ProjectRoot "cppcheck_suppressions.txt") |
        Where-Object { $_.Trim() -ne "unusedFunction" }
    Set-Content -LiteralPath $suppressionsPath -Value $suppressions -Encoding ascii

    & $RunCppcheck -Files @($CppFiles + $probePath) -SuppressionsFile $suppressionsPath `
        -ErrorsFile $errorsPath -EnabledChecks unusedFunction -Jobs 1 -SuppressFindingOutput |
        Out-Null
    $cppcheckExit = $LASTEXITCODE
    if ($cppcheckExit -ne 1) {
        throw "Dead-code cppcheck run exited $cppcheckExit; expected 1 from the planted probe."
    }
    if (-not (Test-Path -LiteralPath $errorsPath -PathType Leaf)) {
        throw "Dead-code cppcheck run wrote no findings file: $errorsPath"
    }
    $lines = @(Get-Content -LiteralPath $errorsPath)
    if (-not ($lines | Where-Object {
                $_ -match ([regex]::Escape($probeName) + ".*\[unusedFunction\]$")
            })) {
        throw "Dead-code checker did not report its planted probe; refusing a false green."
    }

    $candidates = [System.Collections.Generic.SortedSet[string]]::new()
    foreach ($line in $lines) {
        if ($line -notmatch '^(.*):([0-9]+):[0-9]+:.*function ''([A-Za-z_][A-Za-z0-9_]*)'' is never used\. \[unusedFunction\]$') {
            continue
        }
        $path = $Matches[1]
        $lineNumber = $Matches[2]
        $name = $Matches[3]
        if (-not (Is-FirstPartySource $path)) {
            continue
        }
        $occurrences = @(& $rg.Source -o -F -w --glob '!src/third_party/**' --glob '!build*/**' `
            --glob '!artifacts/**' --glob '!temp/**' -- $name `
            (Join-Path $ProjectRoot "src") (Join-Path $ProjectRoot "include") `
            (Join-Path $ProjectRoot "tests") 2>$null)
        if ($LASTEXITCODE -notin @(0, 1)) {
            throw "rg failed while counting references for $name (exit $LASTEXITCODE)."
        }
        if ($occurrences.Count -eq 1) {
            $relative = $path.Substring($ProjectRoot.Length).TrimStart('\', '/')
            [void]$candidates.Add("${relative}:${lineNumber} -- $name")
        }
    }

    if ($candidates.Count -gt 0) {
        Write-Host "DEAD-CODE GATE FAILED: $($candidates.Count) high-confidence dead function(s)." `
            -ForegroundColor Red
        $candidates | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        exit 1
    }
    Write-Host "Dead-code gate passed: $($CppFiles.Count) source file(s), zero unique definitions."
    exit 0
}
finally {
    foreach ($temporaryPath in @($probePath, $suppressionsPath, $errorsPath)) {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}
