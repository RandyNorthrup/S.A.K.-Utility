<#
.SYNOPSIS
    Stages a clean portable S.A.K. Utility release folder from a build output.

.DESCRIPTION
    This script is the single source of truth for portable package assembly.
    It copies only release runtime files, bundled tools, manifests, and docs,
    then removes machine-local runtime state before archive/signing steps.
#>

param(
    [string]$BuildDir = "build\Release",

    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredPath([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Copy-RequiredFile([string]$Source, [string]$DestinationDirectory) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required file missing: $Source"
    }
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    Copy-Item -LiteralPath $Source -Destination $DestinationDirectory -Force
    Write-Host "  Bundled: $([System.IO.Path]::GetFileName($Source))"
}

function Copy-OptionalFile([string]$Source, [string]$DestinationDirectory) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
        Copy-Item -LiteralPath $Source -Destination $DestinationDirectory -Force
        Write-Host "  Bundled: $([System.IO.Path]::GetFileName($Source))"
    }
}

function Copy-RequiredDirectoryContents([string]$SourceDirectory, [string]$DestinationDirectory, [string]$Description) {
    if (-not (Test-Path -LiteralPath $SourceDirectory -PathType Container)) {
        throw "$Description directory missing: $SourceDirectory"
    }
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    Copy-Item -Path (Join-Path $SourceDirectory "*") -Destination $DestinationDirectory -Recurse -Force
    Write-Host "  Bundled: $Description"
}

function Copy-DirectoryExcludingBuildScratch([string]$SourceDirectory, [string]$DestinationDirectory, [string]$Description) {
    if (-not (Test-Path -LiteralPath $SourceDirectory -PathType Container)) {
        throw "$Description directory missing: $SourceDirectory"
    }

    $sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path.TrimEnd('\', '/')
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    $destinationRoot = (Resolve-Path -LiteralPath $DestinationDirectory).Path.TrimEnd('\', '/')

    $stack = [System.Collections.Generic.Stack[string]]::new()
    $stack.Push($sourceRoot)

    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        $relative = $current.Substring($sourceRoot.Length).TrimStart('\', '/')
        $targetDirectory = if ([string]::IsNullOrEmpty($relative)) {
            $destinationRoot
        } else {
            Join-Path $destinationRoot $relative
        }
        New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null

        Get-ChildItem -LiteralPath $current -File -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $targetDirectory -Force
        }

        Get-ChildItem -LiteralPath $current -Directory -Force | ForEach-Object {
            if ($_.Name -ne "_build") {
                $stack.Push($_.FullName)
            }
        }
    }

    Write-Host "  Bundled: $Description"
}

function Remove-PathIfExists([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

$buildRoot = Resolve-RequiredPath $BuildDir "Build directory"
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$repoRootPath = Resolve-RequiredPath $RepoRoot "Repository root"

$packageRoot = Join-Path $buildRoot $PackageName
$zipPath = Join-Path $buildRoot "$PackageName-Windows-x64.zip"
$checksumPath = Join-Path $buildRoot "SHA256SUMS.txt"

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
Remove-PathIfExists $zipPath
Remove-PathIfExists $checksumPath
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

Write-Host "Staging portable release: $packageRoot"

Copy-RequiredFile (Join-Path $buildRoot "sak_utility.exe") $packageRoot
Copy-RequiredFile (Join-Path $buildRoot "sak_elevated_helper.exe") $packageRoot
Copy-RequiredFile (Join-Path $buildRoot "sak_apfs_writer_cli.exe") $packageRoot
Copy-RequiredFile (Join-Path $buildRoot "sak_hfs_writer_cli.exe") $packageRoot
Copy-RequiredFile (Join-Path $buildRoot "sak_splash.png") $packageRoot
Copy-OptionalFile (Join-Path $buildRoot "icon.ico") $packageRoot

$pluginDirs = @(
    "platforms",
    "styles",
    "imageformats",
    "iconengines",
    "tls",
    "networkinformation",
    "generic"
)
foreach ($dir in $pluginDirs) {
    $source = Join-Path $buildRoot $dir
    if (Test-Path -LiteralPath $source -PathType Container) {
        Copy-Item -LiteralPath $source -Destination $packageRoot -Recurse -Force
        $count = (Get-ChildItem -LiteralPath $source -Filter "*.dll" -Recurse -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Host "  Bundled: $dir/ ($count plugins)"
    }
}

$qtDlls = Get-ChildItem -LiteralPath $buildRoot -Filter "Qt6*.dll" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "Qt6Test.dll" }
foreach ($dll in $qtDlls) {
    Copy-Item -LiteralPath $dll.FullName -Destination $packageRoot -Force
}
Write-Host "  Bundled: $($qtDlls.Count) Qt DLL(s)"

foreach ($dllName in @("opengl32sw.dll", "D3Dcompiler_47.dll")) {
    Copy-OptionalFile (Join-Path $buildRoot $dllName) $packageRoot
}

foreach ($pattern in @("vcruntime*.dll", "msvcp*.dll", "concrt*.dll")) {
    Get-ChildItem -LiteralPath $buildRoot -Filter $pattern -File -ErrorAction SilentlyContinue |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Force
            Write-Host "  Bundled: $($_.Name)"
        }
}

& (Join-Path $repoRootPath "scripts/bundle_vcpkg_runtime.ps1") -DestinationPath $packageRoot

$toolsSource = Join-Path $buildRoot "tools"
if (-not (Test-Path -LiteralPath $toolsSource -PathType Container)) {
    throw "Bundled tools directory missing from build output: $toolsSource"
}
Copy-DirectoryExcludingBuildScratch $toolsSource (Join-Path $packageRoot "tools") "tools/"

$toolsRoot = Join-Path $packageRoot "tools"
Remove-PathIfExists (Join-Path $toolsRoot "mcp/_build")
Remove-PathIfExists (Join-Path $toolsRoot "filesystem/_build")
Get-ChildItem -LiteralPath $toolsRoot -Recurse -Filter "*.local.json" -ErrorAction SilentlyContinue |
    Remove-Item -Force

$chocoPath = Join-Path $toolsRoot "chocolatey"
if (Test-Path -LiteralPath $chocoPath -PathType Container) {
    foreach ($sub in @("lib-bad", "cache", "temp")) {
        Remove-PathIfExists (Join-Path $chocoPath $sub)
    }
    foreach ($sub in @("lib", ".chocolatey", "logs")) {
        New-Item -ItemType Directory -Force -Path (Join-Path $chocoPath $sub) | Out-Null
    }
}

Copy-RequiredDirectoryContents `
    (Join-Path $buildRoot "data/ai/providers") `
    (Join-Path $packageRoot "data/ai/providers") `
    "data/ai/providers"
Copy-RequiredDirectoryContents `
    (Join-Path $buildRoot "data/ai/app_manifests") `
    (Join-Path $packageRoot "data/ai/app_manifests") `
    "data/ai/app_manifests"
Get-ChildItem -LiteralPath (Join-Path $packageRoot "data/ai") -Recurse -Filter "*.local.json" -ErrorAction SilentlyContinue |
    Remove-Item -Force

Copy-RequiredFile (Join-Path $repoRootPath "README.md") $packageRoot
Copy-RequiredFile (Join-Path $repoRootPath "LICENSE") $packageRoot
Copy-OptionalFile (Join-Path $repoRootPath "CONTRIBUTING.md") $packageRoot
Copy-RequiredFile (Join-Path $repoRootPath "THIRD_PARTY_LICENSES.md") $packageRoot
New-Item -ItemType File -Path (Join-Path $packageRoot "portable.ini") -Force | Out-Null

foreach ($relativePath in @("data/ai_sessions", "data/temp", "data/logs", "_logs")) {
    Remove-PathIfExists (Join-Path $packageRoot $relativePath)
}

$critical = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "sak_splash.png",
    "Qt6Core.dll",
    "Qt6Widgets.dll",
    "platforms/qwindows.dll",
    "tools/filesystem/manifest.json",
    "tools/mcp/win32-mcp-server/win32-mcp-server.exe",
    "tools/mcp/win32-mcp-server/THIRD_PARTY_LICENSES.txt",
    "data/ai/providers/providers.json",
    "data/ai/app_manifests/windows_defender.json",
    "data/ai/app_manifests/superantispyware.json",
    "data/ai/app_manifests/windows_sfc.json"
)
$missing = $critical | Where-Object { -not (Test-Path -LiteralPath (Join-Path $packageRoot $_) -PathType Leaf) }
if ($missing) {
    throw "Critical files missing from staged package: $($missing -join ', ')"
}

$localFiles = Get-ChildItem -LiteralPath $packageRoot -Recurse -Filter "*.local.json" -ErrorAction SilentlyContinue
if ($localFiles) {
    throw "Local provider/app config leaked into package: $($localFiles[0].FullName)"
}

& (Join-Path $repoRootPath "scripts/verify_windows_runtime_dependencies.ps1") -RootDir $packageRoot -PrimaryExe "sak_utility.exe"

$files = Get-ChildItem -LiteralPath $packageRoot -Recurse -File
$totalSize = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host "Package staged successfully:"
Write-Host "  Path:       $packageRoot"
Write-Host "  Files:      $($files.Count)"
Write-Host "  Total size: $([math]::Round($totalSize / 1MB, 1)) MB"
