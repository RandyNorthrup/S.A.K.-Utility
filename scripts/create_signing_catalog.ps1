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
# Fail closed: the catalog authorizes production signing, so the staging root must really be a
# directory (not a file, not a link into some other tree).
if (-not (Test-Path -LiteralPath $root.Path -PathType Container)) {
    throw "RootDir is not a directory: $RootDir"
}
$rootItem = Get-Item -LiteralPath $root.Path -Force
if ($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
    throw "RootDir is a reparse point; refusing to build a signing catalog through a link: $RootDir"
}
$rootPrefix = $root.Path.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar

$resolvedOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath
} else {
    Join-Path (Get-Location) $OutputPath
}
$resolvedOutputPath = [System.IO.Path]::GetFullPath($resolvedOutputPath)
# WriteAllLines TRUNCATES its target, so an OutputPath that names a binary would destroy that
# binary and replace it with catalog text.
if ([System.IO.Path]::GetExtension($resolvedOutputPath) -in @(".exe", ".dll")) {
    throw "OutputPath must not name a PE file: $OutputPath"
}
if (Test-Path -LiteralPath $resolvedOutputPath -PathType Container) {
    throw "OutputPath is a directory: $OutputPath"
}

$outputParent = Split-Path -Parent $resolvedOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputParent)) {
    New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
}

$files = @(Get-ChildItem -LiteralPath $root.Path -Recurse -File |
    Where-Object { $_.Extension -in ".exe", ".dll" })
if ($files.Count -eq 0) {
    throw "No .exe or .dll files were found under '$($root.Path)'; wrong staging directory?"
}
$catalogEntries = @()

foreach ($file in $files) {
    # Fail closed: never authorize a file reached through a link, or one whose real path
    # escapes RootDir -- the signer would then mutate bytes outside the staged package.
    if ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to enumerate a reparse point into the signing catalog: $($file.FullName)"
    }
    $fullPath = [System.IO.Path]::GetFullPath($file.FullName)
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to sign a file outside RootDir: $fullPath"
    }
    if ($fullPath -eq $resolvedOutputPath) {
        throw "OutputPath collides with a scanned binary: $fullPath"
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -eq "Valid") {
        continue
    }
    # Fail closed: ONLY a genuinely unsigned file may enter the catalog. HashMismatch,
    # NotTrusted, UnknownError and the malformed-format results mean the file is signed and
    # wrong (or unreadable) -- signing it would launder a tampered binary with the release
    # certificate, which is exactly the outcome this catalog exists to prevent.
    if ($signature.Status -ne "NotSigned") {
        throw "Refusing to sign '$($file.FullName)': Authenticode status is $($signature.Status). $($signature.StatusMessage)"
    }

    $relative = $fullPath.Substring($rootPrefix.Length).Replace("/", "\")
    $catalogEntries += ".\$relative"
    Write-Host "Will sign: $relative ($($signature.Status))"
}

# Fail closed: an empty catalog means nothing gets signed, and a release that ships unsigned
# because the scan found nothing must not report success.
if ($catalogEntries.Count -eq 0) {
    throw "No unsigned .exe or .dll files were found under '$($root.Path)'; refusing to write an empty signing catalog."
}

[System.IO.File]::WriteAllLines(
    $resolvedOutputPath,
    $catalogEntries,
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Signing catalog written: $OutputPath"
Write-Host "Files requiring signature: $($catalogEntries.Count)"
