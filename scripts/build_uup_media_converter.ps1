# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# build_uup_media_converter.ps1 - Rebuilds the bundled UUPMediaConverter.exe from source.
#
# tools/uup/uupmc/UUPMediaConverter.exe is OSTooling/UUPMediaCreator at a pinned upstream commit
# plus tools/uup/uupmc/UUPMediaCreator-885f2e36-sak.patch, published self-contained single-file
# for win-x64 with a pinned .NET SDK. This script reproduces that build:
#
#   1. verifies the patch against its pinned SHA-256;
#   2. clones upstream and checks out the pinned commit (LF working tree, so the compiler sees
#      the same source bytes on every machine);
#   3. applies the patch, writes global.json (SDK pinned, no roll-forward) and a NuGet.config
#      that names nuget.org only;
#   4. restores into an isolated package folder and refuses to build unless every package it
#      downloaded matches the SHA-512 pinned below;
#   5. runs dotnet publish (Release, win-x64, self-contained, PublishSingleFile) without a
#      second restore;
#   6. stops if the files the publish emits beside the executable (libwim-15.dll,
#      CDImage\cdimage.exe, Settings\Settings.ini) differ from their pins below, and prints the
#      SHA-256 of the built executable next to the pinned one.
#
# -Install copies the built UUPMediaConverter.exe over tools/uup/uupmc/UUPMediaConverter.exe.
# Nothing else in tools/uup/uupmc is written; every file there is pinned by
# scripts/bundle_uup_tools.ps1. See tools/uup/uupmc/BUILD-NOTES.md.
#
# The heavy steps run at BelowNormal priority with -MaxCpuCount MSBuild nodes (default 2) so the
# build can share a workstation with other build lanes.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/build_uup_media_converter.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/build_uup_media_converter.ps1 -Install

[CmdletBinding()]
param(
    # Short default: the upstream tree has deep paths and MSBuild adds more under obj/.
    [string]$WorkDir = (Join-Path $env:TEMP "sak-uupmc-build"),
    [string]$Dotnet = "dotnet",
    [ValidateRange(1, 64)]
    [int]$MaxCpuCount = 2,
    [ValidateSet("Normal", "BelowNormal", "Idle")]
    [string]$Priority = "BelowNormal",
    [switch]$Install
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# --- Pins --------------------------------------------------------------------------------------

$UpstreamRepo = "https://github.com/OSTooling/UUPMediaCreator.git"
# main as of 2026-10-31: "project: Upgrade to .NET 10" (2026-04-10).
$UpstreamCommit = "885f2e3603d515918773c904e778f331bb239d1b"
$PatchName = "UUPMediaCreator-885f2e36-sak.patch"
$PatchSha256 = "C4D3C17C2258A4C501EFE5E0DC8717B8D5356AF738CF95C387E61CD463457922"

# Latest .NET 10 SDK on 2026-10-31 (10.0.403, runtime 10.0.14, from
# https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json). The runtime
# packed into the self-contained executable is the runtime pack this SDK selects (10.0.14).
$SdkVersion = "10.0.403"

$ProjectPath = "src/Applications/UUPMediaConverter/UUPMediaConverter.csproj"
$RuntimeIdentifier = "win-x64"

# Every package restore downloads, as "<id lowercase>/<version>" = SHA-512 (base64) of the
# .nupkg, which is what NuGet writes to <id>.<version>.nupkg.sha512 and what nuget.org serves
# as the package hash. A restore that downloads anything else, or anything different, stops
# the build before any package code runs.
$NuGetPins = [ordered]@{
    "commandlineparser/2.9.1"                                = "TzZORclmjH58xqkitIjz+lIwM8INekMmlPCmrwXOUo6gSB2DdeL08QMsaZA0e0gDzpoOSAaMb+FexG+xJU8IXQ=="
    "ini-parser-new/2.6.2"                                   = "kNXzJnEJtoxxWe9c8XP4KdGObyj4UvnaBlLoV0LbjFmnVCD8NERK7WWc0vv+6c+jeoga3KbekviiktALTYWYkQ=="
    "jcrci.ltrdata.discutils.core/1.0.31"                    = "mafrO8lbNCzeM3p5uVr0Ufq7okDIivnywlAzDDA4ObYVAjN/XW/PniHtvk56ttFN/VkCi2GS4zCFpDjUEnWl6g=="
    "jcrci.ltrdata.discutils.ntfs/1.0.31"                    = "GQJRTREwTuezSpXLb3ytIaSDTlbm2Iuh4kuBivN1cGg3yYO/gxvirNXTAgo3ILXQRrXAy17XLqEEajPnuKtzxg=="
    "jcrci.ltrdata.discutils.registry/1.0.31"                = "t+OjwYYcDFkQJly0MI+SuDS+GBcDfhTYuMbbCyd9njvtl3fhiZo91OM6ER22j/tAj/L2ZzuaDts5g8vu9T5b8A=="
    "jcrci.ltrdata.discutils.streams/1.0.31"                 = "BRbCvxpyQ6etbp3Op9Huvce5opOhcxJM5UsqWSY1PpSynz8TXwJrK5UKr84MgqndZ3T+90N/0ZmvuXS70mdNXQ=="
    "jcrci.ltrdata.discutils.vhd/1.0.31"                     = "z9itqaNb2o5Tr2KTjLwQ9lG8Bx1XO+OkvtMdt1//w/JDYPEcFrJTxWLhPC9MOaej/Q0UJeAYRIT4qDAZ4HCq6A=="
    "joveler.dynloader/2.3.1"                                = "daHkddep+EjNqTxEapR3lJFIHZYbPSdi031711dd9vyl2FE0g1hC+Ex23nqjmWX5Xk3uyAJDioFst5sOKirn0g=="
    "ltrdata.extensions/1.0.1"                               = "UL4gy1ufCBLrd/AoESBptZXPsnOWt5gybHgfG5UDw9PhjpWQ+WqMm53AVoYi8uwQimA9pcoWJ00xXvP5CDzU7g=="
    "managedwimlib/2.6.0"                                    = "z6LDvKRtNTi5TXntx5kp0NL0Df1zPEPSynWd+97OV91otQJIwy6I+dv3rZeEC6r3BI2Rgt/0RZRcvB4WJS9UOA=="
    "microsoft.aspnetcore.app.runtime.win-x64/10.0.14"       = "1k8096BmENepiqExfgA3ndME94v7RqMqTqLrNMyN+FpqD7WD6jKL9tYrYWi4WKNCAgvRMFVlAidzA3s/FDmIQg=="
    "microsoft.bcl.hashcode/1.1.1"                           = "UMESB/MTHGSZ1lc9M8CVHt7NupRLzQh2K1qRK5pMnNOETUx/4ZaGwhliM2XW93QIGKglIAzkV1sUwHmx9/xvPg=="
    "microsoft.csharp/4.7.0"                                 = "LJaYhRX5VxTUuD9WUPGD3GpWTgs89SVfoOPvSEdt66tL3lQvny9sR/ZiC3px1qUV5EFebS44i2CBeiliHVaQ3w=="
    "microsoft.net.illink.tasks/10.0.14"                     = "YgO/Gkx7Ebb/0FIjQoNRdqn2e1IuzCZaiT3KzPkIIbTvQ17QRGfYNGBCDwkcMOtx2JOzWn0Kwqnuh6SdbSfZqw=="
    "microsoft.netcore.app.runtime.win-x64/10.0.14"          = "mLWGagTP5n7nMfJQI2XpXRYgvPcnyYvAc8ZiDfum8ShM0UcrPDyfW4Lqo45UtlCHCucERUHQuMTI6ybVweCN9A=="
    "microsoft.win32.registry/6.0.0-preview.5.21301.5"       = "CLLk8ubWDWrRcU7Cki1wl0pIz8MsScuWZFDPoH51WXRhUoFiLhjB+jA28jzScnUyj0F9Z1Jx6rwvoXRjv07X8A=="
    "microsoft.windowsdesktop.app.runtime.win-x64/10.0.14"   = "Hxj4llZzFIss9eBt/EKnxessXLi3/wj6Cwrqp2ZL1j74Pmg8VM+IZd9B7bWGt8flKre7z6cJwoRY5YpwJqX44Q=="
    "sharpziplib/1.4.2"                                      = "TggBlumyBCfRtaviOUPWR8NlYRJHz1cdPPJXWAi+cpS5lXBQrtV/o0Mm74llu7MU4DlEaTIWUC0OuZAlPfyU9A=="
    "system.data.datasetextensions/4.6.0-preview3.19128.7"   = "NwqVPC0Wi1WMVDhRRm+cxQksmS8MKmrsF+K8wYNP7yaNRysN0YR9gKV/yVUAWwHixmjYYFJCR/AafAVDOdlxcA=="
    "system.security.principal.windows/6.0.0-preview.5.21301.5" = "yPemPfyDrs5hW0A1AiO5ErOrW2AAo7vhBmGaHx/XpYvQQ+20n07iul4EI6PAT9ygxzhsMrmmVpN32eoh7wz2cg=="
}

# SHA-256 of the executable this script produced for the commit that pinned it. The build is
# deterministic (see BUILD-NOTES.md), so a rebuild on another machine is expected to match.
$ExpectedExeSha256 = "D5C609FEA3A3F90AF5D119990E46CA7F5725726A8EFC690C8AE47DB63EFE8165"

# The other files the publish emits that the converter reads at run time. A change in any of
# them means upstream (or a package) changed what the converter is built with, so the build
# stops and the bundle's pins get reviewed. Only the executable is installed from a build.
$PublishCompanionPins = [ordered]@{
    # wimlib 1.14.4 from the ManagedWimLib 2.6.0 package. Not shipped: the bundle ships wimlib's
    # own 1.14.5 Windows build instead (BUILD-NOTES.md, "wimlib").
    "libwim-15.dll"         = "592431EF475A2093F0D8FB60DB532E6B5CC31760084F4FC1F47ABB0A58502328"
    # Shipped as is.
    "CDImage\cdimage.exe"   = "A3B761293D213BE7FE6F01526CE4B3C18DCC8F95B92E24488011837377DF6582"
    "Settings\Settings.ini" = "3447F26FFAED3CA2FB84090B4AF6CF022D4EA4E23E7371CDD9F5980EBB165684"
}

# --- Helpers -----------------------------------------------------------------------------------

function Write-Step {
    param([string]$Message)
    Write-Host "[UUPMediaConverter build] $Message" -ForegroundColor Cyan
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$What
    )
    $global:LASTEXITCODE = 0
    & $FilePath @Arguments
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "$What failed with exit code $code"
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

# Every package NuGet put in the isolated folder, as "<id>/<version>" -> SHA-512 (base64).
function Get-RestoredPackages {
    param([Parameter(Mandatory = $true)][string]$PackagesRoot)
    $found = [ordered]@{}
    $hashFiles = @(Get-ChildItem -LiteralPath $PackagesRoot -Recurse -File -Filter "*.nupkg.sha512")
    foreach ($hashFile in ($hashFiles | Sort-Object FullName)) {
        $version = $hashFile.Directory.Name
        $id = $hashFile.Directory.Parent.Name
        $found["$id/$version"] = (Get-Content -LiteralPath $hashFile.FullName -Raw).Trim()
    }
    return $found
}

function Assert-RestoredPackagesPinned {
    param([Parameter(Mandatory = $true)][string]$PackagesRoot)
    $restored = Get-RestoredPackages -PackagesRoot $PackagesRoot
    if ($restored.Count -eq 0) {
        throw "Restore produced no packages under $PackagesRoot; refusing to treat that as verified"
    }
    $problems = @()
    foreach ($key in $restored.Keys) {
        if (-not $NuGetPins.Contains($key)) {
            $problems += "unpinned package downloaded: $key"
        } elseif ($NuGetPins[$key] -ne $restored[$key]) {
            $problems += "SHA-512 mismatch for ${key}: pinned $($NuGetPins[$key]), got $($restored[$key])"
        }
    }
    foreach ($key in $NuGetPins.Keys) {
        if (-not $restored.Contains($key)) {
            $problems += "pinned package was not restored: $key"
        }
    }
    if ($problems.Count -gt 0) {
        Write-Host "Restored packages (review before updating the pins):" -ForegroundColor Yellow
        foreach ($key in $restored.Keys) {
            Write-Host ("    `"{0}`" = `"{1}`"" -f $key, $restored[$key])
        }
        throw ("NuGet restore does not match the pinned package set:`n  " + ($problems -join "`n  "))
    }
    Write-Step "All $($restored.Count) restored packages match their pinned SHA-512."
}

# --- Main --------------------------------------------------------------------------------------

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$uupmcDir = Join-Path $repoRoot "tools\uup\uupmc"
$patchPath = Join-Path $uupmcDir $PatchName

if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) {
    throw "Patch not found: $patchPath"
}
$actualPatchSha = Get-Sha256 -Path $patchPath
if ($actualPatchSha -ne $PatchSha256) {
    throw "Patch SHA-256 mismatch: expected $PatchSha256, got $actualPatchSha ($patchPath)"
}
Write-Step "Patch verified: $PatchName ($actualPatchSha)"

if (-not [System.IO.Path]::IsPathRooted($WorkDir)) {
    throw "WorkDir must be an absolute path: $WorkDir"
}
if (Test-Path -LiteralPath $WorkDir) {
    $item = Get-Item -LiteralPath $WorkDir -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "WorkDir is a reparse point; refusing to delete through it: $WorkDir"
    }
    Write-Step "Removing previous work directory $WorkDir"
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

$srcDir = Join-Path $WorkDir "src"
$packagesDir = Join-Path $WorkDir "nuget"
$publishDir = Join-Path $WorkDir "publish"

# --- Source ---
Write-Step "Cloning $UpstreamRepo"
Invoke-Native -FilePath "git" -What "git clone" -Arguments @(
    "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-c", "core.longpaths=true",
    "clone", "--quiet", "--no-checkout", $UpstreamRepo, $srcDir)
foreach ($setting in @(@("core.autocrlf", "false"), @("core.eol", "lf"), @("core.longpaths", "true"))) {
    Invoke-Native -FilePath "git" -What "git config $($setting[0])" -Arguments @(
        "-C", $srcDir, "config", $setting[0], $setting[1])
}
Invoke-Native -FilePath "git" -What "git checkout $UpstreamCommit" -Arguments @(
    "-C", $srcDir, "checkout", "--quiet", "--detach", $UpstreamCommit)
$head = (& git -C $srcDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $UpstreamCommit) {
    throw "Checked-out commit is '$head', expected $UpstreamCommit"
}
Write-Step "Upstream at $head"

Invoke-Native -FilePath "git" -What "git apply --check" -Arguments @(
    "-C", $srcDir, "apply", "--check", $patchPath)
Invoke-Native -FilePath "git" -What "git apply" -Arguments @("-C", $srcDir, "apply", $patchPath)
Write-Step "Patch applied"

$globalJson = @"
{
  "sdk": {
    "version": "$SdkVersion",
    "rollForward": "disable"
  }
}
"@
[System.IO.File]::WriteAllText((Join-Path $srcDir "global.json"), $globalJson.Replace("`r`n", "`n"))
$nugetConfig = @"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" protocolVersion="3" />
  </packageSources>
</configuration>
"@
[System.IO.File]::WriteAllText((Join-Path $srcDir "NuGet.config"), $nugetConfig.Replace("`r`n", "`n"))

# --- Build environment (this process and its children only) ---
$savedEnv = @{}
$buildEnv = @{
    NUGET_PACKAGES                    = $packagesDir
    DOTNET_CLI_TELEMETRY_OPTOUT       = "1"
    DOTNET_NOLOGO                     = "1"
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
    MSBUILDDISABLENODEREUSE           = "1"
}
foreach ($name in $buildEnv.Keys) {
    $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
    [Environment]::SetEnvironmentVariable($name, $buildEnv[$name], "Process")
}
$self = [System.Diagnostics.Process]::GetCurrentProcess()
$savedPriority = $self.PriorityClass
Push-Location $srcDir
try {
    # Child processes inherit a BelowNormal or Idle priority class from their parent.
    $self.PriorityClass = [System.Diagnostics.ProcessPriorityClass]$Priority

    $sdkReported = ((& $Dotnet --version) | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $sdkReported -ne $SdkVersion) {
        throw ("The pinned .NET SDK $SdkVersion is not available to '$Dotnet' (it reported " +
            "'$sdkReported'). Install it per-user without elevation, e.g.: " +
            "dotnet-install.ps1 -Version $SdkVersion -InstallDir <dir> -NoPath, then pass " +
            "-Dotnet <dir>\dotnet.exe")
    }
    Write-Step ".NET SDK $sdkReported"

    $common = @("-r", $RuntimeIdentifier, "-maxcpucount:$MaxCpuCount", "-nodeReuse:false",
        "--disable-build-servers")
    Write-Step "Restoring into $packagesDir"
    Invoke-Native -FilePath $Dotnet -What "dotnet restore" -Arguments (@(
            "restore", $ProjectPath, "-p:SelfContained=true", "-p:PublishSingleFile=true",
            "-p:_IsPublishing=true") + $common)
    Assert-RestoredPackagesPinned -PackagesRoot $packagesDir

    Write-Step "Publishing (Release, $RuntimeIdentifier, self-contained, single file)"
    Invoke-Native -FilePath $Dotnet -What "dotnet publish" -Arguments (@(
            "publish", $ProjectPath, "-c", "Release", "--self-contained",
            "-p:PublishSingleFile=true", "-p:ContinuousIntegrationBuild=true", "--no-restore",
            "-o", $publishDir) + $common)
}
finally {
    $self.PriorityClass = $savedPriority
    Pop-Location
    foreach ($name in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], "Process")
    }
}

# --- Outputs ---
$builtExe = Join-Path $publishDir "UUPMediaConverter.exe"
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
    throw "dotnet publish reported success but produced no $builtExe"
}

foreach ($companion in $PublishCompanionPins.Keys) {
    $built = Join-Path $publishDir $companion
    if (-not (Test-Path -LiteralPath $built -PathType Leaf)) {
        throw "Publish output is missing $companion"
    }
    $builtSha = Get-Sha256 -Path $built
    if ($builtSha -ne $PublishCompanionPins[$companion]) {
        throw ("Publish emitted a different $companion ($builtSha, pinned " +
            "$($PublishCompanionPins[$companion])); review it and the bundle pins")
    }
    Write-Step ("  {0,-24} {1}  (as pinned)" -f $companion, $builtSha)
}

$exeSha = Get-Sha256 -Path $builtExe
$exeSize = (Get-Item -LiteralPath $builtExe).Length
Write-Step ("  {0,-24} {1}  ({2} bytes)" -f "UUPMediaConverter.exe", $exeSha, $exeSize)
if ($exeSha -eq $ExpectedExeSha256) {
    Write-Step "Reproduced the pinned executable bit for bit."
} else {
    Write-Host "[UUPMediaConverter build] Built executable differs from the pinned $ExpectedExeSha256." -ForegroundColor Yellow
}

if ($Install) {
    $target = Join-Path $uupmcDir "UUPMediaConverter.exe"
    Copy-Item -LiteralPath $builtExe -Destination $target -Force
    if ((Get-Sha256 -Path $target) -ne $exeSha) {
        throw "Copy to $target did not produce the built bytes"
    }
    Write-Step "Installed $target"
    Write-Step "Update the UUPMediaConverter.exe pin in scripts/bundle_uup_tools.ps1 and this script if it changed."
}
