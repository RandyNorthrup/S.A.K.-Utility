<#
.SYNOPSIS
    Regression gate for readability-identifier-naming across first-party translation units.

.DESCRIPTION
    Runs one healthy zero-finding pass over non-core first-party TUs and one healthy measurement
    pass over every src/core TU. Core still carries measured debt, so only files listed in
    scripts/clang_tidy_core_naming_clean.txt must remain at zero. The manifest is a ratchet: an
    entry must exist, be analyzed, and stay finding-free.

    Both passes reject compiler/frontend processing errors and incomplete TU execution. A core
    count is published to core-naming-summary.json only after that health contract passes, so an
    analyzer that silently skipped files cannot refresh the measurement.

    Requires a configured Visual Studio build/ tree (for the cache the Ninja
    database generator reads) and clang-tidy on PATH via Program Files\LLVM.

.EXAMPLE
    scripts/clang_tidy_naming_gate.ps1
#>

param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$LogPath = "",
    [string]$FirstPartyDir = "build-tidy-fp",
    [string]$RunClangTidyScript = (Join-Path $PSScriptRoot "run_clang_tidy.ps1"),
    [string]$CoreLogPath = "",
    [string]$CoreFirstPartyDir = "build-tidy-core",
    [string]$CoreManifestPath = (Join-Path $PSScriptRoot "clang_tidy_core_naming_clean.txt")
)

$ErrorActionPreference = "Stop"

if (-not $LogPath) {
    $LogPath = Join-Path $Root "$FirstPartyDir/naming-gate.log"
}
if (-not $CoreLogPath) {
    $CoreLogPath = Join-Path $Root "$CoreFirstPartyDir/core-naming-gate.log"
}
$coreSummaryPath = Join-Path $Root "$CoreFirstPartyDir/core-naming-summary.json"
# Never leave a prior count looking current after this invocation fails before publication.
if (Test-Path -LiteralPath $coreSummaryPath) { Remove-Item -LiteralPath $coreSummaryPath -Force }

function Invoke-NamingAnalysis {
    param(
        [string]$AnalysisDir,
        [string]$AnalysisLog,
        [string]$IncludeFilter = "",
        [string]$ExcludeFilter = "",
        [string]$ConfigFile = ""
    )
    $arguments = @{
        Root = $Root
        Checks = 'readability-identifier-naming'
        FirstPartyDir = $AnalysisDir
        LogPath = $AnalysisLog
        AllowAnalyzerFindings = $true
    }
    if ($IncludeFilter) { $arguments.IncludeFilter = $IncludeFilter }
    if ($ExcludeFilter) { $arguments.ExcludeFilter = $ExcludeFilter }
    if ($ConfigFile) { $arguments.ConfigFile = $ConfigFile }
    & $RunClangTidyScript @arguments
}

function Read-HealthyNamingAnalysis {
    param([string]$Scope, [string]$AnalysisDir, [string]$AnalysisLog)

    if (-not (Test-Path -LiteralPath $AnalysisLog -PathType Leaf)) {
        Write-Host "NAMING GATE FAILED ($Scope): log not written: $AnalysisLog"
        exit 1
    }
    $lines = @(Get-Content -LiteralPath $AnalysisLog)
    $text = $lines -join "`n"
    $analysisDb = Join-Path $Root "$AnalysisDir/compile_commands.json"
    if (-not (Test-Path -LiteralPath $analysisDb -PathType Leaf)) {
        Write-Host "NAMING GATE FAILED ($Scope): analysis database not written: $analysisDb"
        exit 1
    }
    try {
        $analysisDocument = Get-Content -Raw -LiteralPath $analysisDb | ConvertFrom-Json
    }
    catch {
        Write-Host "NAMING GATE FAILED ($Scope): invalid analysis database: $($_.Exception.Message)"
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
        Write-Host "NAMING GATE FAILED ($Scope): analysis database contains zero translation units."
        exit 1
    }

    $processingErrors = @($lines | Where-Object { $_ -match 'Error while processing' })
    $compilerErrors = @($lines | Where-Object {
        $_ -notmatch '\[readability-identifier-naming' -and
        ($_ -match '(?i)(?:^|:\s)(?:fatal\s+)?error:' -or
         $_ -match '\[clang-diagnostic-error\]')
    })
    if ($processingErrors.Count -gt 0 -or $compilerErrors.Count -gt 0) {
        Write-Host "NAMING GATE FAILED ($Scope): clang-tidy/compiler processing errors detected."
        @($processingErrors + $compilerErrors) | Sort-Object -Unique | Select-Object -First 40 |
            ForEach-Object { Write-Host "  $_" }
        exit 1
    }

    $summaryMatches = [regex]::Matches(
        $text,
        'Running clang-tidy.*?for\s+(\d+)\s+files\s+out\s+of\s+(\d+)\s+in compilation database')
    if ($summaryMatches.Count -ne 1) {
        Write-Host "NAMING GATE FAILED ($Scope): expected one TU summary, got $($summaryMatches.Count)."
        exit 1
    }
    $selectedCount = [int]$summaryMatches[0].Groups[1].Value
    $databaseCount = [int]$summaryMatches[0].Groups[2].Value
    if ($selectedCount -ne $expectedCount -or $databaseCount -ne $expectedCount) {
        Write-Host "NAMING GATE FAILED ($Scope): expected $expectedCount TUs; runner selected " `
            "$selectedCount of $databaseCount."
        exit 1
    }

    $progressMatches = [regex]::Matches($text, '(?m)^\[\s*(\d+)\s*/\s*(\d+)\s*\]')
    $completed = [System.Collections.Generic.HashSet[int]]::new()
    $progressBad = $false
    foreach ($progress in $progressMatches) {
        $index = [int]$progress.Groups[1].Value
        $total = [int]$progress.Groups[2].Value
        if ($total -ne $expectedCount -or $index -lt 1 -or $index -gt $expectedCount) {
            $progressBad = $true
        }
        $null = $completed.Add($index)
    }
    if ($progressBad -or $progressMatches.Count -ne $expectedCount -or
        $completed.Count -ne $expectedCount) {
        Write-Host "NAMING GATE FAILED ($Scope): completed $($completed.Count) of $expectedCount TUs."
        exit 1
    }

    $findingMap = [ordered]@{}
    foreach ($line in $lines) {
        if ($line -match '^(?<file>.*?):(?<line>\d+):(?<column>\d+):.*invalid case style for.*\[readability-identifier-naming') {
            $key = "$($Matches.file):$($Matches.line):$($Matches.column)"
            if (-not $findingMap.Contains($key)) {
                $findingMap[$key] = [pscustomobject]@{
                    File = $Matches.file
                    Line = [int]$Matches.line
                    Column = [int]$Matches.column
                    Text = $line
                }
            }
        }
    }
    return [pscustomobject]@{
        TranslationUnits = $expectedCount
        AnalysisDocument = $analysisDocument
        Findings = @($findingMap.Values)
    }
}

function Read-CoreCleanManifest {
    if (-not (Test-Path -LiteralPath $CoreManifestPath -PathType Leaf)) {
        Write-Host "NAMING GATE FAILED (core): clean-TU manifest missing: $CoreManifestPath"
        exit 1
    }
    $entries = [System.Collections.Generic.List[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadLines($CoreManifestPath)) {
        $entry = $line.Trim().Replace('\', '/')
        if (-not $entry -or $entry.StartsWith('#')) { continue }
        if ($entry -notmatch '^src/core/.+\.cpp$') {
            Write-Host "NAMING GATE FAILED (core): invalid manifest entry: $entry"
            exit 1
        }
        if (-not $seen.Add($entry)) {
            Write-Host "NAMING GATE FAILED (core): duplicate manifest entry: $entry"
            exit 1
        }
        if (-not (Test-Path -LiteralPath (Join-Path $Root $entry) -PathType Leaf)) {
            Write-Host "NAMING GATE FAILED (core): manifest source missing: $entry"
            exit 1
        }
        $entries.Add($entry)
    }
    if ($entries.Count -eq 0) {
        Write-Host "NAMING GATE FAILED (core): clean-TU manifest is empty."
        exit 1
    }
    return @($entries)
}

# Ensure the Ninja compilation database exists; the VS generator cannot emit one.
$db = Join-Path $Root "build-tidy/compile_commands.json"
if (-not (Test-Path $db)) {
    Write-Host "build-tidy/compile_commands.json missing; generating it..."
    & (Join-Path $PSScriptRoot "generate_compile_commands.ps1")
    if ($LASTEXITCODE -ne 0) { throw "generate_compile_commands.ps1 failed ($LASTEXITCODE)" }
}

Write-Host "Running readability-identifier-naming gate (non-core first-party TUs)..."
Invoke-NamingAnalysis -AnalysisDir $FirstPartyDir -AnalysisLog $LogPath -ExcludeFilter '/src/core/'
$nonCore = Read-HealthyNamingAnalysis -Scope 'non-core' -AnalysisDir $FirstPartyDir -AnalysisLog $LogPath
if ($nonCore.Findings.Count -gt 0) {
    Write-Host ""
    Write-Host "NAMING GATE FAILED: $($nonCore.Findings.Count) finding(s) outside src/core."
    Write-Host "Every non-core first-party TU must stay at zero. Offending locations:"
    $nonCore.Findings | Select-Object -First 60 | ForEach-Object {
        Write-Host "  $($_.File):$($_.Line):$($_.Column)"
    }
    exit 1
}

Write-Host "Running readability-identifier-naming health/ratchet pass (all core TUs)..."
Invoke-NamingAnalysis -AnalysisDir $CoreFirstPartyDir -AnalysisLog $CoreLogPath `
    -IncludeFilter '/src/core/' -ConfigFile (Join-Path $Root '.clang-tidy')
$core = Read-HealthyNamingAnalysis -Scope 'core' -AnalysisDir $CoreFirstPartyDir `
    -AnalysisLog $CoreLogPath
$cleanCore = Read-CoreCleanManifest

$analyzedCore = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $core.AnalysisDocument) {
    $normalized = ([string]$entry.file).Replace('\', '/')
    foreach ($cleanPath in $cleanCore) {
        if ($normalized.EndsWith("/$cleanPath", [StringComparison]::OrdinalIgnoreCase)) {
            $null = $analyzedCore.Add($cleanPath)
        }
    }
}
$missingAnalysis = @($cleanCore | Where-Object { -not $analyzedCore.Contains($_) })
if ($missingAnalysis.Count -gt 0) {
    Write-Host "NAMING GATE FAILED (core): clean manifest TUs absent from analysis database:"
    $missingAnalysis | ForEach-Object { Write-Host "  $_" }
    exit 1
}

$cleanFindings = [System.Collections.Generic.List[object]]::new()
foreach ($finding in $core.Findings) {
    $normalized = ([string]$finding.File).Replace('\', '/')
    foreach ($cleanPath in $cleanCore) {
        if ($normalized.EndsWith("/$cleanPath", [StringComparison]::OrdinalIgnoreCase)) {
            $cleanFindings.Add($finding)
            break
        }
    }
}
if ($cleanFindings.Count -gt 0) {
    Write-Host "NAMING GATE FAILED (core): clean-TU ratchet regressed:"
    $cleanFindings | Select-Object -First 60 | ForEach-Object {
        Write-Host "  $($_.File):$($_.Line):$($_.Column)"
    }
    exit 1
}

$byFile = [ordered]@{}
foreach ($finding in $core.Findings) {
    $normalized = ([string]$finding.File).Replace('\', '/')
    $relative = $normalized
    $rootPrefix = $Root.Replace('\', '/').TrimEnd('/') + '/'
    if ($normalized.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $relative = $normalized.Substring($rootPrefix.Length)
    }
    if (-not $byFile.Contains($relative)) { $byFile[$relative] = 0 }
    $byFile[$relative]++
}
$summary = [ordered]@{
    schema_version = 1
    healthy = $true
    translation_units = $core.TranslationUnits
    finding_count = $core.Findings.Count
    finding_files = $byFile.Count
    clean_translation_units = $cleanCore.Count
    findings_by_file = $byFile
}
$summaryJson = $summary | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText($coreSummaryPath, $summaryJson, [Text.UTF8Encoding]::new($false))

$passMessage =
    ("NAMING GATE PASSED: non-core {0}/{0} TUs clean; core {1}/{1} TUs healthy, " +
     "{2} measured finding(s), {3} ratcheted clean TU(s).") -f
    $nonCore.TranslationUnits,
    $core.TranslationUnits,
    $core.Findings.Count,
    $cleanCore.Count
Write-Host $passMessage
exit 0
