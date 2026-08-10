# SAK Utility Build Verification Script

param(
    [switch]$FullClean = $false,
    [switch]$SkipBuild = $false
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "===========================================================================" -ForegroundColor Cyan
Write-Host "SAK Utility Build Verification" -ForegroundColor Cyan
Write-Host "===========================================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location $ProjectRoot

Write-Host "[1/7] Verifying required configuration files..." -ForegroundColor Yellow

$RequiredFiles = @(
    "CMakeLists.txt",
    "VERSION",
    "cmake/version.h.in",
    "scripts/check_release_readiness.ps1",
    "scripts/stage_portable_release.ps1",
    "scripts/create_release_archive.ps1"
)

$AllFilesExist = $true
foreach ($file in $RequiredFiles) {
    if (Test-Path $file) {
        Write-Host "  [OK] $file" -ForegroundColor Green
    } else {
        Write-Host "  [X] $file - MISSING!" -ForegroundColor Red
        $AllFilesExist = $false
    }
}

if (-not $AllFilesExist) {
    Write-Host "ERROR: Required files are missing!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[2/7] Verifying all 7 action source files..." -ForegroundColor Yellow

$ActionFiles = @(
    "src/actions/optimize_power_settings_action.cpp",
    "src/actions/verify_system_files_action.cpp",
    "src/actions/check_disk_errors_action.cpp",
    "src/actions/reset_network_action.cpp",
    "src/actions/generate_system_report_action.cpp",
    "src/actions/screenshot_settings_action.cpp",
    "src/actions/backup_bitlocker_keys_action.cpp"
)

$ActionCount = 0
foreach ($file in $ActionFiles) {
    if (Test-Path $file) { $ActionCount++ }
}

Write-Host "  Found: $ActionCount / $($ActionFiles.Count) action files" -ForegroundColor $(if ($ActionCount -eq $ActionFiles.Count) { "Green" } else { "Red" })

if ($ActionCount -ne $ActionFiles.Count) {
    Write-Host "ERROR: One or more action source files are missing!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[3/7] Verifying CMakeLists.txt references all actions..." -ForegroundColor Yellow

$CMakeContent = Get-Content "CMakeLists.txt" -Raw
$MissingCount = 0
foreach ($file in $ActionFiles) {
    if ($CMakeContent -notmatch [regex]::Escape($file)) { $MissingCount++ }
}

if ($MissingCount -eq 0) {
    Write-Host "  [OK] All $($ActionFiles.Count) action files referenced" -ForegroundColor Green
} else {
    Write-Host "  [X] Missing $MissingCount references" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[4/7] Verifying vcpkg configuration..." -ForegroundColor Yellow

$VcpkgRoot = $env:VCPKG_ROOT
$VcpkgToolchain = $null
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    # No guessed-default fallback: an unset VCPKG_ROOT is reported as such, not silently
    # coerced to C:\vcpkg (which could point the build at a stale/foreign tree). The vcpkg
    # toolchain is optional here, so this is informational, not fatal.
    Write-Host "  [i] VCPKG_ROOT not set - vcpkg toolchain will not be used" -ForegroundColor Gray
} else {
    # Verify the actual toolchain file, not merely that the directory exists (an empty
    # directory must not read as a usable vcpkg).
    $VcpkgToolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
    if (Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf) {
        Write-Host "  [OK] vcpkg toolchain found" -ForegroundColor Green
    } else {
        # VCPKG_ROOT is set but has no usable toolchain: fail closed rather than continue
        # and later claim a validated build environment.
        Write-Host "  [X] VCPKG_ROOT set but vcpkg.cmake missing under $VcpkgRoot" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "[5/7] Verifying Qt installation..." -ForegroundColor Yellow

function Test-QtRoot {
    param([string]$Root)
    if ([string]::IsNullOrWhiteSpace($Root)) { return $false }
    # A real Qt prefix carries qmake and the Qt6 CMake package config; a bare existing
    # directory must NOT count as a Qt installation.
    return (Test-Path -LiteralPath (Join-Path $Root "bin\qmake.exe") -PathType Leaf) -or
           (Test-Path -LiteralPath (Join-Path $Root "lib\cmake\Qt6") -PathType Container)
}

$QtRoot = $env:Qt6_DIR
if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    $QtCandidates = @(
        'C:\Qt\6.10.3\msvc2022_64',
        'C:\Qt\6.5.3\msvc2019_64'
    )
    # Discovery only: pick a candidate that is actually a Qt install. No coercion to a
    # nonexistent default -- if none is real, $QtRoot stays empty and we fail closed below.
    $QtRoot = $QtCandidates | Where-Object { Test-QtRoot -Root $_ } | Select-Object -First 1
}

if (Test-QtRoot -Root $QtRoot) {
    Write-Host "  [OK] Qt found at $QtRoot" -ForegroundColor Green
} else {
    # Qt is required to build: fail closed rather than continue to a false success.
    Write-Host "  [X] Qt not found - set Qt6_DIR to a Qt 6 install root" -ForegroundColor Red
    exit 1
}

# Track what actually ran so the final summary cannot claim work it skipped.
$ConfigRan = $false
$BuildRan = $false

# Resolve cmake to a real executable (not a caller-defined alias/function) and fail closed
# if it is absent. A native exe also keeps $LASTEXITCODE meaningful for the checks below.
$CMakeExe = $null
if ($FullClean -or (-not $SkipBuild -and (Test-Path "build"))) {
    $CMakeExe = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $CMakeExe) {
        Write-Host "  [X] cmake executable not found on PATH" -ForegroundColor Red
        exit 1
    }
}

if ($FullClean) {
    Write-Host ""
    Write-Host "[6/7] Performing full clean build..." -ForegroundColor Yellow

    if (Test-Path "build") {
        Remove-Item -Path "build" -Recurse -Force
        Write-Host "  [OK] Build directory removed" -ForegroundColor Green
    }

    $CMakeArgs = @(
        "-B", "build",
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DCMAKE_PREFIX_PATH=$QtRoot"
    )
    if ($null -ne $VcpkgToolchain -and (Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf)) {
        $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
        $CMakeArgs += "-DVCPKG_APPLOCAL_DEPS=OFF"
    }

    & $CMakeExe.Source @CMakeArgs

    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [OK] CMake configuration successful" -ForegroundColor Green
        $ConfigRan = $true
    } else {
        Write-Host "  [X] CMake configuration failed" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host ""
    Write-Host "[6/7] Skipping clean build" -ForegroundColor Gray
}

if (-not $SkipBuild -and (Test-Path "build")) {
    Write-Host ""
    Write-Host "[7/7] Building main executable..." -ForegroundColor Yellow

    # Do not build a foreign/attacker-planted build tree: prove it was configured for THIS
    # checkout before invoking the compiler on it. A pre-existing build/ from another source
    # otherwise compiles and "passes" silently.
    $CacheFile = Join-Path "build" "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $CacheFile -PathType Leaf)) {
        Write-Host "  [X] build\CMakeCache.txt missing - configure with -FullClean first" -ForegroundColor Red
        exit 1
    }
    $homeLine = Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' | Select-Object -First 1
    $cacheHome = if ($null -ne $homeLine) { $homeLine.Line -replace '^CMAKE_HOME_DIRECTORY:INTERNAL=', '' } else { '' }
    $normCacheHome = ($cacheHome -replace '\\', '/').TrimEnd('/').ToLowerInvariant()
    $normProjectRoot = ($ProjectRoot -replace '\\', '/').TrimEnd('/').ToLowerInvariant()
    if ($normCacheHome -ne $normProjectRoot) {
        Write-Host "  [X] build tree was configured for a different source: '$cacheHome'" -ForegroundColor Red
        exit 1
    }

    & $CMakeExe.Source --build build --config Release --target sak_utility --parallel

    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [OK] Build successful" -ForegroundColor Green
        $BuildRan = $true
    } else {
        Write-Host "  [X] Build failed" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host ""
    Write-Host "[7/7] Skipping build" -ForegroundColor Gray
}

Write-Host ""
Write-Host "===========================================================================" -ForegroundColor Cyan
Write-Host "Verification Complete" -ForegroundColor Cyan
Write-Host "===========================================================================" -ForegroundColor Cyan
Write-Host ""
# The three checks below are guaranteed by the hard exits above (missing files, action-file
# mismatch, unresolved CMake references, or missing Qt each already exit 1), so reaching here
# means they passed. Build/configuration is only claimed when it actually ran.
Write-Host "[OK] All required files present" -ForegroundColor Green
Write-Host "[OK] All 7 action files verified" -ForegroundColor Green
Write-Host "[OK] CMakeLists.txt properly configured" -ForegroundColor Green
if ($ConfigRan) {
    Write-Host "[OK] CMake configuration succeeded" -ForegroundColor Green
}
if ($BuildRan) {
    Write-Host "[OK] Build succeeded" -ForegroundColor Green
} else {
    Write-Host "[i] Build not run (use -FullClean to configure; omit -SkipBuild and ensure a build tree exists to compile)" -ForegroundColor Gray
}
Write-Host ""
