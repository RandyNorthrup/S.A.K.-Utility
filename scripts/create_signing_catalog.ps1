<#
.SYNOPSIS
    Creates an Azure Artifact Signing catalog for unsigned PE files.

.DESCRIPTION
    Scans a staged package directory for .exe and .dll files. Files that do not
    already have a valid Authenticode signature are written to a catalog file
    using paths relative to the catalog location, which is the format expected
    by azure/artifact-signing-action.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$RootDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path -LiteralPath $RootDir
$resolvedOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath
} else {
    Join-Path (Get-Location) $OutputPath
}

$outputParent = Split-Path -Parent $resolvedOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputParent)) {
    New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
}

$files = Get-ChildItem -LiteralPath $root.Path -Recurse -File |
    Where-Object { $_.Extension -in ".exe", ".dll" }
$catalogEntries = @()

foreach ($file in $files) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -eq "Valid") {
        continue
    }

    $relative = $file.FullName.Substring($root.Path.Length + 1).Replace("/", "\")
    $catalogEntries += ".\$relative"
    Write-Host "Will sign: $relative ($($signature.Status))"
}

[System.IO.File]::WriteAllLines(
    $resolvedOutputPath,
    $catalogEntries,
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Signing catalog written: $OutputPath"
Write-Host "Files requiring signature: $($catalogEntries.Count)"

if ($catalogEntries.Count -eq 0) {
    Write-Warning "No unsigned .exe or .dll files were found."
}
