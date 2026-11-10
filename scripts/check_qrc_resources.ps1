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

# A zero-manifest result means the resource set was deleted or the scan broke; either way
# there is nothing to validate, so pass would be vacuous -- fail closed instead.
if (-not $qrcFiles -or @($qrcFiles).Count -eq 0) {
    throw "No .qrc manifests found under '$ProjectRoot'; the resource set is missing or the scan is broken."
}

$missing = @()

foreach ($qrc in $qrcFiles) {
    [xml]$xml = Get-Content -LiteralPath $qrc.FullName -Raw
    $qrcDir = Split-Path -Parent $qrc.FullName
    $fileNodes = $xml.SelectNodes("//file")

    foreach ($node in $fileNodes) {
        $relativePath = [string]$node.InnerText
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            # An empty <file> entry is a malformed manifest, not something to skip silently.
            $missing += [pscustomobject]@{
                Manifest = Resolve-Path -LiteralPath $qrc.FullName -Relative
                Missing = "(empty <file> entry)"
            }
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
