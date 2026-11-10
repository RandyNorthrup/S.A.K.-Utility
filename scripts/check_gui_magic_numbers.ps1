<#
.SYNOPSIS
    Fails when GUI layout/sizing calls use raw numeric literals.
#>

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath $Root).Path
$allowedFiles = @(
    "include/sak/layout_constants.h",
    "include/sak/style_constants.h",
    "include/sak/report_style_constants.h",
    "include/sak/color_constants.h",
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

$patterns = @(
    'set(Fixed|Minimum|Maximum)(Width|Height|Size)\s*\([^;]*\b\d+\b',
    'setContentsMargins\s*\([^;]*\b\d+\b',
    'setSpacing\s*\(\s*\d+',
    'resize\s*\([^;]*\b\d+\b',
    'setIconSize\s*\([^;]*\b\d+\b',
    'setColumnWidth\s*\([^;]*\b\d+\b',
    'setSizes\s*\([^;]*\b\d+\b',
    'addSpacing\s*\(\s*\d+'
)

Push-Location $repo
try {
    # -Force so hidden source files are not silently skipped; @(...) so a single match is still
    # an array (Count/index behave) rather than a scalar.
    $files = @(Get-ChildItem -Path "src/gui", "include/sak" -Recurse -File -Force -Include *.cpp, *.h |
        Where-Object {
            $repoPath = Convert-ToRepoPath $_.FullName
            $allowedFiles -notcontains $repoPath
        })

    # Fail closed on an empty scan set: a real checkout always has non-allowlisted GUI sources, so
    # zero files means the expected trees were not found (wrong -Root, moved/renamed dirs). Passing
    # here would report "clean" having checked nothing.
    if ($files.Count -eq 0) {
        Write-Error "GUI magic-number check found no source files under src/gui or include/sak; refusing to pass vacuously (check -Root)."
        exit 1
    }

    $violations = New-Object System.Collections.Generic.List[string]
    foreach ($file in $files) {
        $repoPath = Convert-ToRepoPath $file.FullName
        # @(...) forces an array so a one-line file is scanned as a single line, not as an
        # array of its individual characters (Get-Content returns a scalar string for one line).
        $lines = @(Get-Content -LiteralPath $file.FullName)
        for ($i = 0; $i -lt $lines.Count; ++$i) {
            $line = $lines[$i]
            foreach ($pattern in $patterns) {
                if ($line -match $pattern) {
                    $violations.Add(("{0}:{1}:{2}" -f $repoPath, ($i + 1), $line.Trim()))
                    break
                }
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Error "Raw GUI layout/sizing numeric literal found:`n$($violations -join "`n")"
        exit 1
    }

    Write-Host "GUI magic-number check passed."
}
finally {
    Pop-Location
}
