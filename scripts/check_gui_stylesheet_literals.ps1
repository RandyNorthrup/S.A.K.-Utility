<#
.SYNOPSIS
    Fails when production GUI code passes raw stylesheet literals directly to setStyleSheet.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
$allowedFiles = @(
    "include/sak/style_constants.h",
    "include/sak/report_style_constants.h",
    "src/gui/windows11_theme.cpp"
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
    $files = Get-ChildItem -Path "src/gui", "include/sak" -Recurse -File -Include *.cpp, *.h |
        Where-Object {
            $repoPath = Convert-ToRepoPath $_.FullName
            $allowedFiles -notcontains $repoPath
        }

    $violations = New-Object System.Collections.Generic.List[string]
    foreach ($file in $files) {
        $repoPath = Convert-ToRepoPath $file.FullName
        $lines = Get-Content -LiteralPath $file.FullName
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            if ($lines[$i] -notmatch 'setStyleSheet\s*\(') {
                continue
            }

            $start = $i
            $call = $lines[$i].Trim()
            while ($i -lt ($lines.Count - 1) -and $call -notmatch ';\s*(//.*)?$') {
                ++$i
                $call += " " + $lines[$i].Trim()
            }
            $normalized = $call -replace '\s+', ' '
            if ($normalized -match 'setStyleSheet\s*\(\s*(QString|QStringLiteral|QLatin1String|std::|"|R")' -or
                $normalized -match 'setStyleSheet\s*\([^;]*\.arg\s*\(') {
                $violations.Add(("{0}:{1}:{2}" -f $repoPath, ($start + 1), $normalized))
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Error "Raw setStyleSheet literal found outside style constants:`n$($violations -join "`n")"
        exit 1
    }

    Write-Host "GUI stylesheet-literal check passed."
}
finally {
    Pop-Location
}
