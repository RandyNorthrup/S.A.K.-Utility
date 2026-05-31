<#
.SYNOPSIS
    Fails when GUI code adds raw color tokens outside theme/style constants.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
$allowedFiles = @(
    "include/sak/color_constants.h",
    "include/sak/style_constants.h",
    "include/sak/report_style_constants.h",
    "include/sak/email_constants.h"
)

function Convert-ToRepoPath([string]$Path) {
    $full = (Resolve-Path -LiteralPath $Path).Path
    $root = $repo.TrimEnd([System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if ($full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        $relative = $full.Substring($root.Length).TrimStart(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)
    } else {
        $relative = $full
    }
    return ($relative -replace '\\', '/')
}

function Test-InEmbeddedHtmlBlock([string]$repoPath, [string]$line, [ref]$state) {
    if ($repoPath -ne "src/gui/main_window.cpp") {
        return $false
    }
    if ($line -match 'constexpr char k(About|Credits)TabHtml\[\] = R"') {
        $state.Value = $true
        return $true
    }
    if ($state.Value -and $line -match '^\)SAK(ABOUT|CREDITS)";') {
        $state.Value = $false
        return $true
    }
    return [bool]$state.Value
}

Push-Location $repo
try {
    $files = Get-ChildItem -Path "src/gui", "include/sak" -Recurse -File -Include *.cpp, *.h |
        Where-Object {
            $repoPath = Convert-ToRepoPath $_.FullName
            $allowedFiles -notcontains $repoPath
        }

    $rawColorPattern = '#[0-9A-Fa-f]{3,8}\b|rgba?\s*\('
    $violations = New-Object System.Collections.Generic.List[string]

    foreach ($file in $files) {
        $repoPath = Convert-ToRepoPath $file.FullName
        $inHtml = $false
        $lines = Get-Content -LiteralPath $file.FullName
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            $line = $lines[$i]
            $trimmed = $line.TrimStart()
            if (Test-InEmbeddedHtmlBlock $repoPath $line ([ref]$inHtml)) {
                continue
            }
            if ($trimmed.StartsWith("//") -or $trimmed.StartsWith("*") -or
                $trimmed.StartsWith("#define")) {
                continue
            }
            if ($line -match $rawColorPattern) {
                $violations.Add(("{0}:{1}:{2}" -f $repoPath, ($i + 1), $line.Trim()))
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Error "Raw GUI color token found outside theme constants:`n$($violations -join "`n")"
        exit 1
    }

    Write-Host "GUI style-token check passed."
}
finally {
    Pop-Location
}
