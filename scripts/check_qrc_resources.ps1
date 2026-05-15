<#
.SYNOPSIS
    Verifies that every Qt resource manifest entry points to an existing file.

.DESCRIPTION
    Parses all repository .qrc files and fails if any <file> entry is missing.
    This catches CI-breaking resource omissions before CMake/AutoRCC runs.
#>

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

$qrcFiles = Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Filter "*.qrc" -File |
    Where-Object { $_.FullName -notmatch "\\(build|_archived|\.git)\\" }

$missing = @()

foreach ($qrc in $qrcFiles) {
    [xml]$xml = Get-Content -LiteralPath $qrc.FullName -Raw
    $qrcDir = Split-Path -Parent $qrc.FullName
    $fileNodes = $xml.SelectNodes("//file")

    foreach ($node in $fileNodes) {
        $relativePath = [string]$node.InnerText
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }

        $resourcePath = Join-Path $qrcDir $relativePath
        if (-not (Test-Path -LiteralPath $resourcePath -PathType Leaf)) {
            $missing += [pscustomobject]@{
                Manifest = Resolve-Path -LiteralPath $qrc.FullName -Relative
                Missing = $relativePath
            }
        }
    }
}

if ($missing.Count -gt 0) {
    Write-Host "Qt resource manifest check failed:" -ForegroundColor Red
    $missing | Format-Table -AutoSize | Out-String | Write-Host
    exit 1
}

Write-Host "Qt resource manifests clean." -ForegroundColor Green
