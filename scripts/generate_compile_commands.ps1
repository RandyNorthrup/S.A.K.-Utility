<#
.SYNOPSIS
    Emit compile_commands.json for clang-tidy by configuring a parallel Ninja tree.

.DESCRIPTION
    The project builds with the Visual Studio generator, which cannot produce a
    compilation database. clang-tidy needs one, so this configures a SEPARATE
    Ninja tree (build-tidy/) with CMAKE_EXPORT_COMPILE_COMMANDS=ON. Two settings
    are essential and were learned the hard way:

      * -DCMAKE_CXX_SCAN_FOR_MODULES=OFF. Without it every compile command carries
        an '@<tu>.obj.modmap' response-file argument that only exists AFTER a
        build; clang-tidy then reports one clang-diagnostic-error per translation
        unit ('no such file ... .modmap') -- the phantom "parse error" wall.

      * The vcpkg triplet and Qt prefix must match the real build, or the CRT and
        stale-dependency guards in CMakeLists.txt refuse to configure. Both are
        read from the existing Visual Studio cache so this never drifts from what
        actually ships.

    Run scripts/run_clang_tidy.ps1 afterwards; it de-duplicates this database to
    one entry per first-party translation unit and runs clang-tidy in the MSVC
    environment (so the Windows SDK and STL headers resolve).
#>

param(
    [string]$Root = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$VsBuildDir = "build",
    [string]$TidyBuildDir = "build-tidy"
)

$ErrorActionPreference = "Stop"

function Read-CacheValue([string]$cache, [string]$key) {
    if (-not (Test-Path $cache)) { return $null }
    $line = Select-String -LiteralPath $cache -Pattern "^$([regex]::Escape($key)):[^=]*=(.*)$" |
        Select-Object -First 1
    if ($null -eq $line) { return $null }
    return $line.Matches[0].Groups[1].Value.Trim()
}

Push-Location $Root
try {
    $cache = Join-Path $Root "$VsBuildDir/CMakeCache.txt"
    if (-not (Test-Path $cache)) {
        throw "No $VsBuildDir/CMakeCache.txt. Configure the normal Visual Studio build first; this script mirrors its Qt prefix and vcpkg triplet."
    }
    $qtPrefix = Read-CacheValue $cache "CMAKE_PREFIX_PATH"
    $toolchain = Read-CacheValue $cache "CMAKE_TOOLCHAIN_FILE"
    $triplet = Read-CacheValue $cache "VCPKG_TARGET_TRIPLET"
    if (-not $qtPrefix) { throw "CMAKE_PREFIX_PATH not found in the Visual Studio cache." }

    # Locate the MSVC developer environment via vswhere, then this run's compiler.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere; install the Visual Studio Installer." }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw "vswhere located no Visual Studio with the C++ toolset." }
    $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsRoot." }

    $ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
    if (-not $ninja) {
        $venvNinja = Join-Path $Root ".venv\Scripts\ninja.exe"
        if (Test-Path $venvNinja) { $ninja = $venvNinja }
    }
    if (-not $ninja) { throw "ninja not found on PATH or in .venv\Scripts." }

    # A changed triplet does not re-resolve cached absolute dependency paths, so
    # start from a clean tree (the CMakeLists stale-dependency guard enforces this).
    $tidyPath = Join-Path $Root $TidyBuildDir
    if (Test-Path $tidyPath) { Remove-Item -Recurse -Force $tidyPath }

    $args = @(
        "-S", ".", "-B", $TidyBuildDir, "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",
        "-DCMAKE_C_COMPILER=cl",
        "-DCMAKE_CXX_COMPILER=cl",
        "-DCMAKE_PREFIX_PATH=$qtPrefix",
        "-DQT_NO_PRIVATE_MODULE_WARNING=ON"
    )
    if ($toolchain) { $args += "-DCMAKE_TOOLCHAIN_FILE=$toolchain" }
    if ($triplet) { $args += "-DVCPKG_TARGET_TRIPLET=$triplet" }

    # cmake must run inside the MSVC environment so cl.exe and the SDK are found.
    $quoted = ($args | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $cmd = "call `"$vcvars`" >nul && cmake $quoted"
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)." }

    $db = Join-Path $tidyPath "compile_commands.json"
    if (-not (Test-Path $db)) { throw "configure reported success but $db is missing." }
    Write-Host "compile_commands.json written: $db"
}
finally {
    Pop-Location
}
