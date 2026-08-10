<#
.SYNOPSIS
    Creates the Windows release ZIP and checksum file.
#>

param(
    [string]$BuildDir = "build\Release",

    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [switch]$CleanMutableRuntimeData
)

$ErrorActionPreference = "Stop"

# $PackageName is joined onto $BuildDir and then used to delete, clean, archive, and hash
# paths. Require a single, safe path segment so it cannot escape $BuildDir via separators,
# "..", or wildcard metacharacters.
if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $PackageName.Contains('..')) {
    throw "Invalid PackageName '$PackageName': must be a single path segment of letters, digits, '.', '_', or '-' with no '..'."
}

$build = Resolve-Path -LiteralPath $BuildDir
$packageDir = Join-Path $build.Path $PackageName
$zipPath = Join-Path $build.Path "$PackageName-Windows-x64.zip"
$checksumsPath = Join-Path $build.Path "SHA256SUMS.txt"
$signingCatalog = Join-Path $packageDir "signing-catalog.txt"

if (-not (Test-Path -LiteralPath $packageDir -PathType Container)) {
    throw "Package directory not found: $packageDir"
}

if (Test-Path -LiteralPath $signingCatalog -PathType Leaf) {
    Remove-Item -LiteralPath $signingCatalog -Force
}
if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $checksumsPath -PathType Leaf) {
    Remove-Item -LiteralPath $checksumsPath -Force
}

$packagedLocalProviderFiles = Get-ChildItem -LiteralPath $packageDir -Recurse -Filter "*.local.json" -ErrorAction Stop
if ($packagedLocalProviderFiles) {
    throw "Refusing to archive package with local provider config file: $($packagedLocalProviderFiles[0].FullName)"
}

$requiredPackageFiles = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "data/ai/providers/providers.json",
    "data/ai/app_manifests/windows_defender.json",
    "data/ai/app_manifests/superantispyware.json",
    "data/ai/app_manifests/windows_sfc.json"
)
foreach ($relativePath in $requiredPackageFiles) {
    $fullPath = Join-Path $packageDir $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Refusing to archive package missing required file: $relativePath"
    }
    $requiredItem = Get-Item -LiteralPath $fullPath -Force
    if ($requiredItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to archive package: required file is a reparse point: $relativePath"
    }
    if ($requiredItem.Attributes -band ([System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System)) {
        # Compress-Archive silently excludes hidden/system entries, so such a file would
        # pass this presence check yet be absent from the ZIP.
        throw "Refusing to archive package: required file is hidden or system and would be excluded from the archive: $relativePath"
    }
    if ($requiredItem.Length -le 0) {
        throw "Refusing to archive package: required file is empty: $relativePath"
    }
}

$mcpBuildLeak = Join-Path $packageDir "tools/mcp/_build"
if (Test-Path -LiteralPath $mcpBuildLeak) {
    throw "Refusing to archive package with temporary MCP build artifacts: $mcpBuildLeak"
}

$mutableRuntimePaths = @("data/ai_sessions", "data/temp", "data/logs", "data/config", "_logs")
foreach ($relativePath in $mutableRuntimePaths) {
    $mutablePath = Join-Path $packageDir $relativePath
    if (Test-Path -LiteralPath $mutablePath) {
        if ($CleanMutableRuntimeData) {
            Remove-Item -LiteralPath $mutablePath -Recurse -Force
            Write-Host "Removed mutable runtime data before archiving: $relativePath"
            continue
        }
        throw "Refusing to archive package with mutable runtime data: $relativePath"
    }
}

$chocoPath = Join-Path $packageDir "tools/chocolatey"
if (Test-Path -LiteralPath $chocoPath -PathType Container) {
    foreach ($relativePath in @("lib-bad", "cache", "temp")) {
        $mutablePath = Join-Path $chocoPath $relativePath
        if (Test-Path -LiteralPath $mutablePath) {
            if ($CleanMutableRuntimeData) {
                Remove-Item -LiteralPath $mutablePath -Recurse -Force
                Write-Host "Removed Chocolatey runtime state before archiving: tools/chocolatey/$relativePath"
                continue
            }
            throw "Refusing to archive package with Chocolatey runtime state: tools/chocolatey/$relativePath"
        }
    }
}

Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath -Force

$exeHash = (Get-FileHash (Join-Path $packageDir "sak_utility.exe") -Algorithm SHA256).Hash
$helperHash = (Get-FileHash (Join-Path $packageDir "sak_elevated_helper.exe") -Algorithm SHA256).Hash
$zipHash = (Get-FileHash $zipPath -Algorithm SHA256).Hash

Write-Host ""
Write-Host "=== SHA256 Checksums ==="
Write-Host "sak_utility.exe        : $exeHash"
Write-Host "sak_elevated_helper.exe: $helperHash"
Write-Host "ZIP                    : $zipHash"

@"
SHA256 Checksums
================
sak_utility.exe:          $exeHash
sak_elevated_helper.exe:  $helperHash
$PackageName-Windows-x64.zip:  $zipHash
"@ | Out-File $checksumsPath -Encoding UTF8

Write-Host ""
Write-Host "Package archive created successfully: $zipPath"
