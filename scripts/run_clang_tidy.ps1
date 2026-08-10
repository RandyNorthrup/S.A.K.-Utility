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
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

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
    foreach ($e in $entries) {
        $f = ($e.file -replace '\\', '/')
        if ($f -notmatch $want -or $f -match $skip) { continue }
        $key = $f.ToLowerInvariant()
        if ($seen.Add($key)) { $kept.Add($e) }
    }
    if ($kept.Count -eq 0) { throw "No first-party entries found in $db." }
    $fpDir = Join-Path $Root $FirstPartyDir
    New-Item -ItemType Directory -Force -Path $fpDir | Out-Null
    $kept | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $fpDir "compile_commands.json") -Encoding UTF8
    Write-Host "first-party translation units: $($kept.Count)"

    # Locate the developer environment and the LLVM tools.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
    $runTidy = "${env:ProgramFiles}\LLVM\bin\run-clang-tidy"
    $tidyBin = "${env:ProgramFiles}\LLVM\bin\clang-tidy.exe"
    foreach ($t in @($vcvars, $tidyBin)) {
        if (-not (Test-Path $t)) { throw "required tool missing: $t" }
    }

    if (-not $LogPath) { $LogPath = Join-Path $Root "$FirstPartyDir/clang-tidy.log" }
    $tidyArgs = "-p `"$fpDir`" -clang-tidy-binary `"$tidyBin`" -j $Jobs -quiet"
    if ($Checks) { $tidyArgs += " -checks=`"-*,$Checks`"" }
    if ($Fix) { $tidyArgs += " -fix" }

    $cmd = "call `"$vcvars`" >nul 2>&1 && python `"$runTidy`" $tidyArgs"
    Write-Host "running clang-tidy (checks='$Checks' fix=$Fix) -> $LogPath"
    & cmd.exe /c $cmd *> $LogPath
    $code = $LASTEXITCODE
    Write-Host "run-clang-tidy exit $code (non-zero simply means findings remain; see $LogPath)"
}
finally {
    Pop-Location
}
