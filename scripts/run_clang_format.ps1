<#
.SYNOPSIS
    Runs clang-format with repository defaults.

.DESCRIPTION
    Locates clang-format from PATH, a local virtual environment, standalone LLVM,
    or Visual Studio's bundled LLVM tools. This avoids hardcoding one Visual
    Studio edition in pre-commit while keeping formatting strict.
#>

param(
    [switch]$Check,
    [string[]]$Files,
    # Fail closed when the resolved file list is EMPTY. Pre-commit hands this script the staged
    # files, so an empty list there legitimately means "nothing C++ was staged" and exiting 0 is
    # correct. A whole-tree caller (CI) is the opposite case: an empty list means the file
    # discovery broke, and reporting a green gate that formatted nothing is exactly the
    # false-green this repo has been burned by twice (the cppcheck -Files comma-join, and this
    # script invoked with no arguments at all). CI passes -RequireFiles so that cannot happen.
    [switch]$RequireFiles
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

if ($args.Count -gt 0) {
    $Files += $args
}

function Find-ClangFormat {
    $fromPath = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $candidatePaths = @(
        (Join-Path $ProjectRoot ".venv\Scripts\clang-format.exe"),
        "$env:ProgramFiles\LLVM\bin\clang-format.exe",
        "${env:ProgramFiles(x86)}\LLVM\bin\clang-format.exe"
    )

    $editions = @("Community", "Professional", "Enterprise", "BuildTools", "Preview")
    foreach ($edition in $editions) {
        $candidatePaths += "$env:ProgramFiles\Microsoft Visual Studio\2022\$edition\VC\Tools\Llvm\bin\clang-format.exe"
        $candidatePaths += "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\$edition\VC\Tools\Llvm\bin\clang-format.exe"
    }

    foreach ($candidate in $candidatePaths) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    throw "clang-format.exe was not found. Install LLVM or Visual Studio Build Tools."
}

$cppExtensions = @(".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx")
$cppFiles = @()
foreach ($file in $Files) {
    $extension = [System.IO.Path]::GetExtension($file).ToLowerInvariant()
    if ($cppExtensions -contains $extension -and
        (Test-Path -LiteralPath $file -PathType Leaf)) {
        $cppFiles += $file
    }
}

if ($cppFiles.Count -eq 0) {
    if ($RequireFiles) {
        Write-Error ("No C++ files resolved, but -RequireFiles was specified. A whole-tree run " +
                     "that finds nothing is a BROKEN gate, not a passing one.")
        exit 1
    }
    Write-Host "No C++ files to format."
    exit 0
}

# Require the repository .clang-format so the gate cannot pass against clang-format's
# built-in fallback style when the config is absent.
$clangFormatConfig = Join-Path $ProjectRoot ".clang-format"
if (-not (Test-Path -LiteralPath $clangFormatConfig -PathType Leaf)) {
    throw "Repository .clang-format configuration not found at $clangFormatConfig; refusing to format with clang-format's built-in fallback style."
}

$clangFormat = Find-ClangFormat

# Batch the file list. A whole-tree run passes ~900 paths, which overruns the Windows
# command-line limit and fails with "The filename or extension is too long" -- a failure that
# looks nothing like a formatting violation and would have to be diagnosed from scratch on a CI
# runner. Batching keeps every invocation well inside the limit, and the worst exit code across
# the batches is what the gate reports, so a violation in ANY batch still fails the run.
$batchSize = 100
$worstExit = 0
for ($offset = 0; $offset -lt $cppFiles.Count; $offset += $batchSize) {
    $batch = $cppFiles[$offset..([Math]::Min($offset + $batchSize - 1, $cppFiles.Count - 1))]
    if ($Check) {
        & $clangFormat -style=file -fallback-style=none --dry-run -Werror -- $batch
    } else {
        & $clangFormat -style=file -fallback-style=none -i -- $batch
    }
    if ($LASTEXITCODE -ne 0) {
        $worstExit = $LASTEXITCODE
    }
}

if ($worstExit -ne 0) {
    exit $worstExit
}

Write-Host "clang-format passed ($($cppFiles.Count) file(s))."
