<#
.SYNOPSIS
    Prepares portable MCP server payload folders for S.A.K. Utility builds.

.DESCRIPTION
    This script creates the expected bundle layout under tools/mcp/. The SAK
    build copies tools/ into the release folder, so anything placed here ships
    with the portable app. Win32 MCP must be packaged as a self-contained
    executable before unattended desktop automation can be enabled.
#>

param(
    [string]$DestinationRoot = "tools/mcp",
    [switch]$BuildWin32,
    [string]$Python = "python",
    [string]$Win32Package = "win32-mcp-server",
    [string]$Win32SourcePath = "",
    [string]$Win32SourceUri = "https://github.com/RandyNorthrup/win32-mcp-server.git",
    [string]$Win32SourceRef = "main",
    [switch]$AllowPyPiFallback,
    [switch]$KeepBuildArtifacts
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = Join-Path $root $DestinationRoot
$win32 = Join-Path $dest "win32-mcp-server"
$win32Exe = Join-Path $win32 "win32-mcp-server.exe"

function Resolve-Win32PackageSpec {
    param(
        [string]$ExplicitSourcePath,
        [string]$PackageName,
        [string]$SourceUri,
        [string]$SourceRef,
        [string]$BuildRoot,
        [bool]$AllowFallback
    )

    $candidates = @()
    if ($ExplicitSourcePath) {
        $candidates += $ExplicitSourcePath
    }
    $candidates += @(
        (Join-Path $root "..\win-mcp"),
        (Join-Path $root "..\win32-mcp-server")
    )

    foreach ($candidate in $candidates) {
        $resolved = Resolve-Path $candidate -ErrorAction SilentlyContinue
        if ($resolved -and (Test-Path (Join-Path $resolved "pyproject.toml"))) {
            return @{
                Spec = $resolved.Path
                Source = $resolved.Path
            }
        }
    }

    if ($SourceUri) {
        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git) {
            $cloneRoot = Join-Path $BuildRoot "source"
            $clonePath = Join-Path $cloneRoot "win32-mcp-server"
            New-Item -ItemType Directory -Force -Path $cloneRoot | Out-Null
            if (Test-Path $clonePath) {
                Remove-Item -Recurse -Force $clonePath
            }

            Write-Host "Cloning Win32 MCP source: $SourceUri"
            if ($SourceRef) {
                & $git.Source clone --depth 1 --branch $SourceRef $SourceUri $clonePath
            } else {
                & $git.Source clone --depth 1 $SourceUri $clonePath
            }
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to clone Win32 MCP source from $SourceUri"
            }
            if (!(Test-Path (Join-Path $clonePath "pyproject.toml"))) {
                throw "Cloned Win32 MCP source is missing pyproject.toml: $clonePath"
            }
            return @{
                Spec = $clonePath
                Source = $clonePath
            }
        }
        if (!$AllowFallback) {
            throw "git is required to clone $SourceUri. Install git or pass -AllowPyPiFallback."
        }
    }

    if (!$AllowFallback) {
        throw "Win32 MCP source not found locally and PyPI fallback is disabled. Expected ..\win-mcp, ..\win32-mcp-server, or a cloneable -Win32SourceUri."
    }

    return @{
        Spec = $PackageName
        Source = "PyPI:$PackageName"
    }
}

New-Item -ItemType Directory -Force -Path $win32 | Out-Null

$readme = @"
SAK MCP bundle layout
=====================

Expected portable files:

  tools/mcp/win32-mcp-server/win32-mcp-server.exe

Build/package win32-mcp-server as a self-contained Windows executable before
shipping unattended GUI automation. SAK provider manifests already point to this
path and the build copies tools/ into the release directory.

Remote MCP providers do not need local runtime files:

  Microsoft Learn MCP: https://learn.microsoft.com/api/mcp
  Context7 MCP:        https://mcp.context7.com/mcp

Those endpoints still require network access at runtime. Microsoft Learn MCP
does not require authentication. Context7 can be used without an API key for
public documentation lookups.
"@

Set-Content -LiteralPath (Join-Path $dest "README.txt") -Value $readme -Encoding UTF8

if ($BuildWin32) {
    $buildRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sak_mcp_bundle\win32-mcp-server"
    if ((Test-Path $buildRoot) -and !$KeepBuildArtifacts) {
        Remove-Item -Recurse -Force $buildRoot
    }
    New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

    $packageSpec = Resolve-Win32PackageSpec `
        -ExplicitSourcePath $Win32SourcePath `
        -PackageName $Win32Package `
        -SourceUri $Win32SourceUri `
        -SourceRef $Win32SourceRef `
        -BuildRoot $buildRoot `
        -AllowFallback ([bool]$AllowPyPiFallback)
    $venv = Join-Path $buildRoot ".venv"
    $launcher = Join-Path $buildRoot "win32_mcp_server_launcher.py"
    $distRoot = Join-Path $buildRoot "dist"
    $stagedWin32Exe = Join-Path $distRoot "win32-mcp-server.exe"
    $py = Join-Path $venv "Scripts\python.exe"
    $pyinstaller = Join-Path $venv "Scripts\pyinstaller.exe"

    & $Python -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "Failed to create Python venv with $Python" }

    Write-Host "Win32 MCP source: $($packageSpec.Source)"
    & $py -m pip install --upgrade pip pyinstaller $packageSpec.Spec
    if ($LASTEXITCODE -ne 0) { throw "Failed to install PyInstaller and $($packageSpec.Spec)" }

    # PyAutoGUI depends on MouseInfo for its standalone mouse-info helper, which
    # win32-mcp-server does not use. MouseInfo is GPLv3+, so keep it out of the
    # portable runtime bundle and out of the generated runtime license inventory.
    & $py -m pip uninstall -y MouseInfo | Out-Host

    @"
from win32_mcp_server import main

if __name__ == "__main__":
    main()
"@ | Set-Content -LiteralPath $launcher -Encoding UTF8

    & $pyinstaller --onefile --clean --noupx --exclude-module MouseInfo --exclude-module mouseinfo --name win32-mcp-server --distpath $distRoot --workpath (Join-Path $buildRoot "work") --specpath $buildRoot $launcher
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed to build win32-mcp-server.exe" }
    if (!(Test-Path $stagedWin32Exe)) { throw "Expected output missing: $stagedWin32Exe" }

    $versionOut = Join-Path $buildRoot "version.out.txt"
    $versionErr = Join-Path $buildRoot "version.err.txt"
    Remove-Item $versionOut, $versionErr -Force -ErrorAction SilentlyContinue
    $versionProcess = Start-Process -FilePath $stagedWin32Exe `
        -ArgumentList @("--version") `
        -NoNewWindow `
        -Wait `
        -PassThru `
        -RedirectStandardOutput $versionOut `
        -RedirectStandardError $versionErr
    $versionExit = $versionProcess.ExitCode
    $version = (Get-Content -LiteralPath $versionOut -Raw -ErrorAction SilentlyContinue).Trim()
    if ($versionExit -ne 0) {
        $stderr = (Get-Content -LiteralPath $versionErr -Raw -ErrorAction SilentlyContinue).Trim()
        throw "win32-mcp-server.exe --version failed: $stderr"
    }
    Write-Host "Win32 MCP smoke: $version"

    $toolsOut = Join-Path $buildRoot "tools.out.txt"
    $toolsErr = Join-Path $buildRoot "tools.err.txt"
    Remove-Item $toolsOut, $toolsErr -Force -ErrorAction SilentlyContinue
    $toolsProcess = Start-Process -FilePath $stagedWin32Exe `
        -ArgumentList @("--list-tools") `
        -NoNewWindow `
        -Wait `
        -PassThru `
        -RedirectStandardOutput $toolsOut `
        -RedirectStandardError $toolsErr
    $listToolsExit = $toolsProcess.ExitCode
    $listTools = Get-Content -LiteralPath $toolsOut -Raw -ErrorAction SilentlyContinue
    if ($listToolsExit -ne 0) {
        $stderr = (Get-Content -LiteralPath $toolsErr -Raw -ErrorAction SilentlyContinue).Trim()
        throw "win32-mcp-server.exe --list-tools failed: $stderr"
    }
    if ($listTools -notmatch "uia_inspect_window" -or $listTools -notmatch "click_text") {
        throw "win32-mcp-server.exe tool list did not include expected automation tools"
    }

    $sourceLicense = Join-Path $packageSpec.Source "LICENSE"
    if (Test-Path $sourceLicense) {
        Copy-Item -Force $sourceLicense (Join-Path $win32 "LICENSE.win32-mcp-server.txt")
    }

    if ($packageSpec.Source -like "PyPI:*") {
        $sourceLabel = $packageSpec.Source
    } elseif ($packageSpec.Source -like "$buildRoot*") {
        $sourceLabel = "$Win32SourceUri@$Win32SourceRef"
    } else {
        $sourceLabel = "local source checkout selected by scripts/bundle_mcp_servers.ps1"
    }

    @"
Portable win32-mcp-server bundle
================================

Executable:
  tools/mcp/win32-mcp-server/win32-mcp-server.exe

Source:
  $sourceLabel

Smoke:
  $version

SAK uses this stdio MCP server for manifest-gated Windows desktop observation
and automation. It is not a general permission bypass: mutating actions must
be routed through SAK policy and app-control manifests.
"@ | Set-Content -LiteralPath (Join-Path $win32 "README.txt") -Encoding UTF8

    $licenseReport = Join-Path $buildRoot "THIRD_PARTY_LICENSES.txt"
    @'
import importlib.metadata as md
import sys

out = sys.argv[1]
skip = {
    "altgraph",
    "mouseinfo",
    "pefile",
    "pip",
    "pyinstaller",
    "pyinstaller-hooks-contrib",
    "setuptools",
    "wheel",
}
blocked_fragments = ("agpl", "gpl", "lgpl")

def first_line(value: str) -> str:
    return " ".join(value.strip().split())[:220]

def license_value(meta) -> str:
    values = meta.get_all("License-Expression") or []
    if not values:
        values = meta.get_all("License") or []
    values = [first_line(v) for v in values if v and v.strip()]
    if values:
        return " | ".join(values)
    classifiers = [
        c.removeprefix("License ::").strip()
        for c in (meta.get_all("Classifier") or [])
        if c.startswith("License ::")
    ]
    if classifiers:
        return "; ".join(classifiers)
    return "UNKNOWN"

rows = []
for dist in md.distributions():
    name = dist.metadata.get("Name", "")
    if not name or name.lower() in skip:
        continue
    rows.append((name, dist.version, license_value(dist.metadata)))

blocked = []
for name, version, license_name in rows:
    lowered = license_name.lower()
    if any(fragment in lowered for fragment in blocked_fragments):
        blocked.append((name, version, license_name))

if blocked:
    for name, version, license_name in blocked:
        print(f"Blocked non-permissive Win32 MCP runtime dependency: {name} {version} {license_name}", file=sys.stderr)
    sys.exit(2)

with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("Win32 MCP Server bundled dependency license inventory\n")
    f.write("Generated by scripts/bundle_mcp_servers.ps1.\n")
    f.write("Review this file whenever win32-mcp-server or its Python dependencies change.\n")
    f.write("Build-only packages are omitted. PyInstaller is used only to build the exe; its bootloader has the PyInstaller GPL exception.\n")
    f.write("MouseInfo is intentionally excluded because win32-mcp-server does not use PyAutoGUI's mouse-info helper and MouseInfo is GPLv3+.\n\n")
    f.write("Package\tVersion\tLicense\n")
    for name, version, license_name in sorted(rows, key=lambda r: r[0].lower()):
        f.write(f"{name}\t{version}\t{license_name}\n")
'@ | & $py - $licenseReport
    if ($LASTEXITCODE -ne 0) { throw "Failed to generate Win32 MCP license report" }

    try {
        Copy-Item -Force $stagedWin32Exe $win32Exe
        Copy-Item -Force $licenseReport (Join-Path $win32 "THIRD_PARTY_LICENSES.txt")
    } catch {
        throw "Failed to replace $win32Exe. Close any running win32-mcp-server.exe process and retry. $($_.Exception.Message)"
    }

    if (!$KeepBuildArtifacts -and (Test-Path $buildRoot)) {
        Remove-Item -Recurse -Force $buildRoot
    }
}

Write-Host "MCP bundle folders ready: $dest"
if (Test-Path $win32Exe) {
    Write-Host "Win32 MCP bundled: $win32Exe"
} else {
    Write-Host "Missing until packaged: $win32Exe"
    Write-Host "Build it with: powershell -ExecutionPolicy Bypass -File scripts/bundle_mcp_servers.ps1 -BuildWin32"
}
