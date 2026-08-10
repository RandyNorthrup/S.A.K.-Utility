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
# Last resort: the conventional install location, but only when it actually exists -- never
# proceed against a guessed path that is absent, which would silently bundle nothing.
if ([string]::IsNullOrWhiteSpace($VcpkgRoot) -and (Test-Path -LiteralPath "C:\vcpkg" -PathType Container)) {
    $VcpkgRoot = "C:\vcpkg"
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "vcpkg root not found: pass -VcpkgRoot or set VCPKG_ROOT / VCPKG_INSTALLATION_ROOT."
}

# The triplet is joined straight into the source path; reject anything that is not a plain
# triplet token so a value like '..\..\Windows\System32' cannot redirect the copy source.
if ($Triplet -notmatch '^[A-Za-z0-9._-]+$') {
    throw "Invalid vcpkg triplet '$Triplet'."
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
