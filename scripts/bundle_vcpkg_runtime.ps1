<#
.SYNOPSIS
    Copies dynamic vcpkg runtime DLLs needed by the Windows build.

.DESCRIPTION
    Release builds that use the dynamic x64-windows vcpkg triplet can depend on
    runtime DLLs such as z.dll, bz2.dll, and liblzma.dll. This script copies the
    known runtime DLL names from the vcpkg triplet bin directory into a build or
    package directory. Missing aliases are ignored because vcpkg names differ by
    port and version; runtime dependency verification catches any DLL that is
    actually required but still missing.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationPath,

    [string]$Triplet = "x64-windows",

    [string]$VcpkgRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = $env:VCPKG_ROOT
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = "C:\vcpkg"
}

$destination = Resolve-Path -LiteralPath $DestinationPath
$vcpkgBin = Join-Path $VcpkgRoot "installed\$Triplet\bin"

if (-not (Test-Path -LiteralPath $vcpkgBin -PathType Container)) {
    Write-Host "vcpkg runtime directory not found: $vcpkgBin"
    Write-Host "No dynamic vcpkg runtime DLLs bundled."
    return
}

$runtimeDlls = @(
    "z.dll",
    "zlib1.dll",
    "bz2.dll",
    "libbz2.dll",
    "liblzma.dll",
    "lzma.dll"
)

$copied = @()
foreach ($dll in $runtimeDlls) {
    $source = Join-Path $vcpkgBin $dll
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $destination.Path -Force
        $copied += $dll
    }
}

if ($copied.Count -eq 0) {
    Write-Host "No dynamic vcpkg runtime DLLs found in $vcpkgBin."
} else {
    Write-Host "Bundled vcpkg runtime DLL(s): $($copied -join ', ')"
}
