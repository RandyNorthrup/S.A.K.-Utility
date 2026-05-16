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
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

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
    Write-Host "No C++ files to format."
    exit 0
}

$clangFormat = Find-ClangFormat
if ($Check) {
    & $clangFormat --dry-run -Werror -- $cppFiles
} else {
    & $clangFormat -i -- $cppFiles
}

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "clang-format passed."
