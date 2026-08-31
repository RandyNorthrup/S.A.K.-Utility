<#
.SYNOPSIS
    Regression gate for readability-identifier-naming across all first-party
    translation units EXCEPT src/core.

.DESCRIPTION
    The R5-G12 naming remediation drove readability-identifier-naming to zero in
    every first-party directory except src/core, which is a documented, permanent
    carve-out (src/core/.clang-tidy disables the check there because clang-tidy's
    rename fix is unreliable on the multi-thousand-line lambda-heavy raw-filesystem
    engines). This gate re-runs the check on the remaining 147 non-core TUs and
    FAILS if a single "invalid case style" finding reappears, so the convention
    cannot silently regress.

    It is deliberately naming-only (a -checks override, not the full .clang-tidy),
    because other enabled checks (function-size, avoid-c-arrays, the pro-bounds
    container check) are not yet at zero and would be a false failure here. src/core
    is excluded with -ExcludeFilter (a PowerShell-side path filter) rather than a
    run-clang-tidy positional regex, because a negative-lookahead file filter does
    not survive the PowerShell -> cmd.exe -> python round-trip.

    Requires a configured Visual Studio build/ tree (for the cache the Ninja
    database generator reads) and clang-tidy on PATH via Program Files\LLVM.

.EXAMPLE
    scripts/clang_tidy_naming_gate.ps1
#>

param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$LogPath = "",
    [string]$FirstPartyDir = "build-tidy-fp",
    [string]$RunClangTidyScript = (Join-Path $PSScriptRoot "run_clang_tidy.ps1")
)

$ErrorActionPreference = "Stop"

if (-not $LogPath) {
    $LogPath = Join-Path $Root "$FirstPartyDir/naming-gate.log"
}

# Ensure the Ninja compilation database exists; the VS generator cannot emit one.
$db = Join-Path $Root "build-tidy/compile_commands.json"
if (-not (Test-Path $db)) {
    Write-Host "build-tidy/compile_commands.json missing; generating it..."
    & (Join-Path $PSScriptRoot "generate_compile_commands.ps1")
    if ($LASTEXITCODE -ne 0) { throw "generate_compile_commands.ps1 failed ($LASTEXITCODE)" }
}

Write-Host "Running readability-identifier-naming gate (all first-party TUs except src/core)..."
& $RunClangTidyScript `
    -Root $Root `
    -Checks 'readability-identifier-naming' `
    -ExcludeFilter 'src/core/' `
    -FirstPartyDir $FirstPartyDir `
    -LogPath $LogPath

# run-clang-tidy's own exit code only means "findings remain"; decide pass/fail by
# parsing the log so the gate is deterministic regardless of that code.
if (-not (Test-Path $LogPath)) { throw "gate log not written: $LogPath" }
$lines = Get-Content -LiteralPath $LogPath
$text = $lines -join "`n"

# A finding-free log is not proof unless every expected translation unit completed.
# Read the exact database clang-tidy consumed, then match run-clang-tidy's own summary
# and per-TU completion records against it. Any compiler/frontend failure is fatal even
# when naming itself reports zero findings.
$analysisDb = Join-Path $Root "$FirstPartyDir/compile_commands.json"
if (-not (Test-Path -LiteralPath $analysisDb)) {
    Write-Host "NAMING GATE FAILED: analysis compilation database was not written: $analysisDb"
    exit 1
}

try {
    $analysisDocument = Get-Content -Raw -LiteralPath $analysisDb | ConvertFrom-Json
}
catch {
    Write-Host "NAMING GATE FAILED: analysis compilation database is invalid JSON: $($_.Exception.Message)"
    exit 1
}

if ($null -eq $analysisDocument) {
    $expectedCount = 0
}
elseif ($analysisDocument -is [System.Collections.ICollection]) {
    $expectedCount = $analysisDocument.Count
}
else {
    $expectedCount = 1
}
if ($expectedCount -le 0) {
    Write-Host "NAMING GATE FAILED: analysis compilation database contains zero translation units."
    exit 1
}

$processingErrors = @($lines | Where-Object { $_ -match 'Error while processing' })
$compilerErrors = @($lines | Where-Object {
    $_ -match '(?i)(?:^|:\s)(?:fatal\s+)?error:' -or $_ -match '\[clang-diagnostic-error\]'
})
if ($processingErrors.Count -gt 0 -or $compilerErrors.Count -gt 0) {
    Write-Host "NAMING GATE FAILED: clang-tidy/compiler processing error(s) detected."
    @($processingErrors + $compilerErrors) |
        Sort-Object -Unique |
        Select-Object -First 40 |
        ForEach-Object { Write-Host "  $_" }
    exit 1
}

$summaryMatches = [regex]::Matches(
    $text,
    'Running clang-tidy.*?for\s+(\d+)\s+files\s+out\s+of\s+(\d+)\s+in compilation database')
if ($summaryMatches.Count -ne 1) {
    Write-Host "NAMING GATE FAILED: expected one clang-tidy translation-unit summary, found $($summaryMatches.Count)."
    exit 1
}

$selectedCount = [int]$summaryMatches[0].Groups[1].Value
$databaseCount = [int]$summaryMatches[0].Groups[2].Value
if ($selectedCount -ne $expectedCount -or $databaseCount -ne $expectedCount) {
    Write-Host "NAMING GATE FAILED: expected $expectedCount translation units, runner selected " `
        "$selectedCount of $databaseCount."
    exit 1
}

$progressMatches = [regex]::Matches($text, '(?m)^\[\s*(\d+)\s*/\s*(\d+)\s*\]')
$completed = [System.Collections.Generic.HashSet[int]]::new()
$progressTotalMismatch = $false
$progressIndexInvalid = $false
foreach ($progress in $progressMatches) {
    $index = [int]$progress.Groups[1].Value
    $total = [int]$progress.Groups[2].Value
    if ($total -ne $expectedCount) { $progressTotalMismatch = $true }
    if ($index -lt 1 -or $index -gt $expectedCount) { $progressIndexInvalid = $true }
    $null = $completed.Add($index)
}
if ($progressTotalMismatch -or $progressIndexInvalid -or
    $progressMatches.Count -ne $expectedCount -or $completed.Count -ne $expectedCount) {
    Write-Host "NAMING GATE FAILED: clang-tidy completed $($completed.Count) of $expectedCount translation units."
    exit 1
}

$findings = $lines | Where-Object { $_ -match 'invalid case style for' }
# One finding may be reported by several TUs; de-duplicate on file:line:col.
$unique = $findings |
    ForEach-Object { if ($_ -match '^(.*?):(\d+):(\d+):') { "$($Matches[1]):$($Matches[2]):$($Matches[3])" } } |
    Sort-Object -Unique

if ($unique.Count -gt 0) {
    Write-Host ""
    Write-Host "NAMING GATE FAILED: $($unique.Count) readability-identifier-naming finding(s) outside src/core."
    Write-Host "Every non-core first-party TU must stay at zero. Offending locations:"
    $unique | Select-Object -First 60 | ForEach-Object { Write-Host "  $_" }
    if ($unique.Count -gt 60) { Write-Host "  ... and $($unique.Count - 60) more" }
    exit 1
}

Write-Host "NAMING GATE PASSED: 0 readability-identifier-naming findings outside src/core."
exit 0
