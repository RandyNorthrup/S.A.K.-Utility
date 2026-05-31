<#
.SYNOPSIS
    Fails when production code keeps raw style literals outside style constants.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
$allowedFiles = @(
    "include/sak/color_constants.h",
    "include/sak/style_constants.h",
    "include/sak/report_style_constants.h"
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

Push-Location $repo
try {
    $files = Get-ChildItem -Path "src", "include" -Recurse -File -Include *.cpp, *.h |
        Where-Object {
            $repoPath = Convert-ToRepoPath $_.FullName
            $allowedFiles -notcontains $repoPath
        }

    $rawStyleLiteralPatterns = @(
        'style\s*=',
        '<style\b',
        '</style>',
        'background(?:-color)?\s*:',
        'border(?:-(?:radius|color|bottom|top|left|right))?\s*:',
        'color\s*:',
        'font-(?:size|family|weight)\s*:',
        'padding(?:-(?:left|right|top|bottom))?\s*:',
        'margin(?:-(?:left|right|top|bottom))?\s*:',
        'qlineargradient',
        'rgba?\s*\('
    )
    $violations = New-Object System.Collections.Generic.List[string]

    function Test-StyleLiteral([string]$Line) {
        foreach ($pattern in $rawStyleLiteralPatterns) {
            if ($Line -match $pattern) {
                return $true
            }
        }
        return $false
    }

    foreach ($file in $files) {
        $repoPath = Convert-ToRepoPath $file.FullName
        $lines = Get-Content -LiteralPath $file.FullName
        $inRawLiteral = $false
        $rawLiteralStart = 0
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            $line = $lines[$i]
            $trimmed = $line.TrimStart()
            if ($trimmed.StartsWith("//") -or $trimmed.StartsWith("*")) {
                continue
            }

            if ($inRawLiteral) {
                if (Test-StyleLiteral $line) {
                    $violations.Add(("{0}:{1}:raw style literal: {2}" -f $repoPath,
                            ($i + 1), $line.Trim()))
                }
                if ($line -match '\)[A-Za-z0-9_]*";') {
                    $inRawLiteral = $false
                }
                continue
            }

            if ($line -match 'R"[A-Za-z0-9_]*\(') {
                $inRawLiteral = $true
                $rawLiteralStart = $i + 1
                if (Test-StyleLiteral $line) {
                    $violations.Add(("{0}:{1}:raw style literal: {2}" -f $repoPath,
                            ($rawLiteralStart), $line.Trim()))
                }
                if ($line -match '\)[A-Za-z0-9_]*";') {
                    $inRawLiteral = $false
                }
                continue
            }

            if ($line -match 'setStyleSheet\s*\(') {
                $start = $i
                $call = $line.Trim()
                while ($i -lt ($lines.Count - 1) -and $call -notmatch ';\s*(//.*)?$') {
                    ++$i
                    $call += " " + $lines[$i].Trim()
                }
                $normalized = $call -replace '\s+', ' '
                if ($normalized -match 'setStyleSheet\s*\(\s*(QString|QStringLiteral|QLatin1String|std::|"|R")' -or
                    $normalized -match 'setStyleSheet\s*\([^;]*\.arg\s*\(') {
                    $violations.Add(("{0}:{1}:raw setStyleSheet literal: {2}" -f $repoPath,
                            ($start + 1), $normalized))
                }
                continue
            }

            if ($line -match '"' -and $line -notmatch 'sak::log' -and (Test-StyleLiteral $line)) {
                $violations.Add(("{0}:{1}:raw style literal: {2}" -f $repoPath,
                        ($i + 1), $line.Trim()))
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Error "Raw style literal found outside style constants:`n$($violations -join "`n")"
        exit 1
    }

    Write-Host "Style-literal check passed."
}
finally {
    Pop-Location
}
