<#
.SYNOPSIS
    Creates the Windows release ZIP and checksum file.
#>

param(
    [string]$BuildDir = "build\Release",

    [Parameter(Mandatory = $true)]
    [string]$PackageName
)

$ErrorActionPreference = "Stop"

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
