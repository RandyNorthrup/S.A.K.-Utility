<#
.SYNOPSIS
    Run clang-tidy over first-party translation units using the Ninja database.

.DESCRIPTION
    Reads build-tidy/compile_commands.json (produced by
    scripts/generate_compile_commands.ps1), de-duplicates it to ONE entry per
    first-party translation unit -- the Visual Studio build compiles each source
    once per target, so a source appears many times and would otherwise be
    re-analyzed many times -- and writes build-tidy-fp/compile_commands.json.

    clang-tidy then runs against that de-duplicated database INSIDE the MSVC
    developer environment (via vcvars64), which is required so the Windows SDK and
    STL system headers resolve; without it clang emits a wall of
    clang-diagnostic-error for every missing <windows.h>.

    Use -Checks to scope a remediation wave to one check, and -Fix to apply
    clang-tidy's fix-its across all translation units (run-clang-tidy merges the
    replacements). ALWAYS rebuild and run ctest after a -Fix run; a fix that
    compiles is not proof it preserves behavior.

.EXAMPLE
    scripts/run_clang_tidy.ps1 -Checks 'readability-math-missing-parentheses' -Fix
#>

param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$TidyBuildDir = "build-tidy",
    [string]$FirstPartyDir = "build-tidy-fp",
    [string]$Checks = "",
    [switch]$Fix,
    [int]$Jobs = 6,
    [string]$LogPath = "",
    [string]$FileFilter = "",
    [string]$AutogenTarget = "sak_utility_autogen",
    # PrepareOnly writes the filtered, analysis-safe compilation database but does not
    # launch clang-tidy. The registered regression test uses this seam under PowerShell
    # 5.1 and 7 so compile-command sanitization cannot silently regress.
    [switch]$PrepareOnly,
    # Drop translation units whose (forward-slashed) path matches this regex BEFORE the
    # database is written, so they are never analyzed. Applied in PowerShell, not as a
    # run-clang-tidy positional regex, because a negative-lookahead file filter does not
    # survive the PowerShell -> cmd.exe -> python round-trip. Used by the naming gate to
    # isolate its zero-debt non-core pass.
    [string]$ExcludeFilter = "",
    # Keep only translation units whose forward-slashed path matches this regex. This is applied
    # before writing the analysis database, so health gates can prove the selected TU count rather
    # than asking run-clang-tidy to filter a larger database after the fact.
    [string]$IncludeFilter = "",
    # Force every selected TU to use one root config. Core normally inherits src/core/.clang-tidy,
    # which disables naming while debt remains; the core clean-file ratchet explicitly uses the
    # root config so readability-identifier-naming is observable there.
    [string]$ConfigFile = ""
    ,[switch]$AllowAnalyzerFindings
    ,[ValidateRange(-1, 255)] [int]$AnalyzerExitCodeForTesting = -1
)

$ErrorActionPreference = "Stop"

function Remove-AnalysisIncompatibleDriverFlags {
    param([Parameter(Mandatory = $true)] [object]$Entry)

    # These flags control cl.exe's build driver rather than C/C++ language semantics.
    # clang-tidy invokes clang's frontend directly, where they are unused-command-line
    # errors. Remove only those proven driver-only flags from the analysis COPY of the
    # database. Source build commands remain untouched.
    $driverOnlyPattern = '(?i)^/(?:GL|MP(?:\d+)?|Zc:preprocessor|external:anglebrackets)$'
    $removed = 0

    if ($Entry.PSObject.Properties.Name -contains "arguments") {
        $keptArguments = [System.Collections.Generic.List[string]]::new()
        foreach ($argument in @($Entry.arguments)) {
            if ([string]$argument -match $driverOnlyPattern) {
                $removed++
                continue
            }
            $keptArguments.Add([string]$argument)
        }
        $Entry.arguments = @($keptArguments)
        return $removed
    }

    if ($Entry.PSObject.Properties.Name -notcontains "command") {
        throw "Compilation database entry has neither command nor arguments: $($Entry.file)"
    }

    $commandPattern = '(?i)(?<!\S)/(?:GL|MP(?:\d+)?|Zc:preprocessor|external:anglebrackets)(?=\s|$)'
    $matches = [regex]::Matches([string]$Entry.command, $commandPattern)
    $Entry.command = [regex]::Replace([string]$Entry.command, $commandPattern, "")
    return $matches.Count
}

Push-Location $Root
try {
    $db = Join-Path $Root "$TidyBuildDir/compile_commands.json"
    if (-not (Test-Path $db)) {
        throw "No $TidyBuildDir/compile_commands.json. Run scripts/generate_compile_commands.ps1 first."
    }

    # De-duplicate to one entry per first-party (src/ or include/) source file.
    $skip = '[\\/](build|build-tidy|build-tidy-fp|third_party|3rdparty|external|_archived|vcpkg|CMakeFiles)[\\/]'
    $want = '[\\/](src|include)[\\/]'
    $entries = Get-Content -Raw -LiteralPath $db | ConvertFrom-Json
    $seen = [System.Collections.Generic.HashSet[string]]::new()
    $kept = [System.Collections.Generic.List[object]]::new()
    $removedDriverFlags = 0
    $prunedMissingSources = 0
    foreach ($e in $entries) {
        $f = ($e.file -replace '\\', '/')
        if ($f -notmatch $want -or $f -match $skip) { continue }
        if ($IncludeFilter -and $f -notmatch $IncludeFilter) { continue }
        if ($ExcludeFilter -and $f -match $ExcludeFilter) { continue }
        $sourcePath = [string]$e.file
        if (-not [IO.Path]::IsPathRooted($sourcePath)) {
            $sourcePath = Join-Path ([string]$e.directory) $sourcePath
        }
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            $prunedMissingSources++
            continue
        }
        $key = $f.ToLowerInvariant()
        if ($seen.Add($key)) {
            $removedDriverFlags += Remove-AnalysisIncompatibleDriverFlags -Entry $e
            $kept.Add($e)
        }
    }
    if ($kept.Count -eq 0) { throw "No first-party entries found in $db." }
    $fpDir = Join-Path $Root $FirstPartyDir
    New-Item -ItemType Directory -Force -Path $fpDir | Out-Null
    # Write UTF-8 WITHOUT a BOM: Windows PowerShell's -Encoding UTF8 prepends a BOM, and
    # run-clang-tidy's json.load then fails with 'Expecting value: line 1 column 1'.
    $json = $kept | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText((Join-Path $fpDir "compile_commands.json"), $json,
        [System.Text.UTF8Encoding]::new($false))
    Write-Host "first-party translation units: $($kept.Count)"
    Write-Host "analysis-only driver flags removed: $removedDriverFlags"
    Write-Host "stale missing-source entries pruned: $prunedMissingSources"

    if ($PrepareOnly) {
        Write-Host "analysis compilation database prepared: $fpDir"
        return
    }

    if ($AnalyzerExitCodeForTesting -ge 0) {
        if (-not $LogPath) { $LogPath = Join-Path $Root "$FirstPartyDir/clang-tidy.log" }
        Set-Content -LiteralPath $LogPath -Value 'injected analyzer result' -Encoding ascii
        $code = $AnalyzerExitCodeForTesting
        Write-Host "run-clang-tidy injected exit $code"
        if ($code -ne 0 -and (-not $AllowAnalyzerFindings -or $code -ne 1)) {
            exit $code
        }
        return
    }

    # Locate the developer environment and the LLVM tools.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
    $runTidy = "${env:ProgramFiles}\LLVM\bin\run-clang-tidy"
    $tidyBin = "${env:ProgramFiles}\LLVM\bin\clang-tidy.exe"
    $applyBin = "${env:ProgramFiles}\LLVM\bin\clang-apply-replacements.exe"
    foreach ($t in @($vcvars, $tidyBin, $applyBin)) {
        if (-not (Test-Path $t)) { throw "required tool missing: $t" }
    }

    if (-not [string]::IsNullOrWhiteSpace($AutogenTarget)) {
        $autogenBuildDir = Join-Path $Root $TidyBuildDir
        $autogenCommand = "call `"$vcvars`" >nul 2>&1 && cmake --build " +
            "`"$autogenBuildDir`" --target `"$AutogenTarget`""
        Write-Host "preparing generated analysis inputs: $AutogenTarget"
        & cmd.exe /c $autogenCommand
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build generated analysis inputs for $AutogenTarget."
        }
    }

    if (-not $LogPath) { $LogPath = Join-Path $Root "$FirstPartyDir/clang-tidy.log" }
    $tidyArgs = "-p `"$fpDir`" -clang-tidy-binary `"$tidyBin`" -j $Jobs -quiet"
    if ($Checks) { $tidyArgs += " -checks=`"-*,$Checks`"" }
    if ($ConfigFile) { $tidyArgs += " -config-file=`"$ConfigFile`"" }
    if ($Fix) { $tidyArgs += " -fix -clang-apply-replacements-binary `"$applyBin`"" }

    # A trailing positional regex restricts run-clang-tidy to matching source paths, so a
    # large check (e.g. readability-identifier-naming) can be applied one directory at a time.
    if ($FileFilter) { $tidyArgs += " `"$FileFilter`"" }

    $cmd = "call `"$vcvars`" >nul 2>&1 && python `"$runTidy`" $tidyArgs"
    Write-Host "running clang-tidy (checks='$Checks' fix=$Fix) -> $LogPath"
    & cmd.exe /c $cmd *> $LogPath
    $code = $LASTEXITCODE
    Write-Host "run-clang-tidy exit $code (see $LogPath)"
    if ($code -ne 0 -and (-not $AllowAnalyzerFindings -or $code -ne 1)) {
        exit $code
    }
}
finally {
    Pop-Location
}
