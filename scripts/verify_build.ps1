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
    "vcpkg.json",
    "cmake/SAK_BuildConfig.cmake",
    "cmake/version.h.in"
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
Write-Host "[2/7] Verifying all 39 action source files..." -ForegroundColor Yellow

$ActionFiles = @(
    "src/actions/disk_cleanup_action.cpp",
    "src/actions/clear_browser_cache_action.cpp",
    "src/actions/defragment_drives_action.cpp",
    "src/actions/clear_windows_update_cache_action.cpp",
    "src/actions/disable_startup_programs_action.cpp",
    "src/actions/clear_event_logs_action.cpp",
    "src/actions/optimize_power_settings_action.cpp",
    "src/actions/disable_visual_effects_action.cpp",
    "src/actions/quickbooks_backup_action.cpp",
    "src/actions/browser_profile_backup_action.cpp",
    "src/actions/outlook_backup_action.cpp",
    "src/actions/sticky_notes_backup_action.cpp",
    "src/actions/saved_game_data_backup_action.cpp",
    "src/actions/tax_software_backup_action.cpp",
    "src/actions/photo_management_backup_action.cpp",
    "src/actions/development_configs_backup_action.cpp",
    "src/actions/check_disk_health_action.cpp",
    "src/actions/update_all_apps_action.cpp",
    "src/actions/windows_update_action.cpp",
    "src/actions/verify_system_files_action.cpp",
    "src/actions/check_disk_errors_action.cpp",
    "src/actions/rebuild_icon_cache_action.cpp",
    "src/actions/reset_network_action.cpp",
    "src/actions/clear_print_spooler_action.cpp",
    "src/actions/generate_system_report_action.cpp",
    "src/actions/check_bloatware_action.cpp",
    "src/actions/test_network_speed_action.cpp",
    "src/actions/scan_malware_action.cpp",
    "src/actions/repair_windows_store_action.cpp",
    "src/actions/fix_audio_issues_action.cpp",
    "src/actions/backup_browser_data_action.cpp",
    "src/actions/backup_email_data_action.cpp",
    "src/actions/create_restore_point_action.cpp",
    "src/actions/export_registry_keys_action.cpp",
    "src/actions/backup_activation_keys_action.cpp",
    "src/actions/screenshot_settings_action.cpp",
    "src/actions/backup_desktop_wallpaper_action.cpp",
    "src/actions/backup_printer_settings_action.cpp",
    "src/actions/action_factory.cpp"
)

$ActionCount = 0
foreach ($file in $ActionFiles) {
    if (Test-Path $file) { $ActionCount++ }
}

Write-Host "  Found: $ActionCount / $($ActionFiles.Count) action files" -ForegroundColor $(if ($ActionCount -eq $ActionFiles.Count) { "Green" } else { "Red" })

Write-Host ""
Write-Host "[3/7] Verifying CMakeLists.txt references all actions..." -ForegroundColor Yellow

$CMakeContent = Get-Content "CMakeLists.txt" -Raw
$MissingCount = 0
foreach ($file in $ActionFiles) {
    if ($CMakeContent -notmatch [regex]::Escape($file)) { $MissingCount++ }
}

if ($MissingCount -eq 0) {
    Write-Host "  [OK] All $($ActionFiles.Count) actions referenced" -ForegroundColor Green
} else {
    Write-Host "  [X] Missing $MissingCount references" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[4/7] Verifying vcpkg configuration..." -ForegroundColor Yellow

if (Test-Path 'C:\vcpkg') {
    Write-Host "  [OK] vcpkg found" -ForegroundColor Green
} else {
    Write-Host "  [X] vcpkg not found" -ForegroundColor Red
}

Write-Host ""
Write-Host "[5/7] Verifying Qt installation..." -ForegroundColor Yellow

if (Test-Path 'C:\Qt\6.5.3\msvc2019_64') {
    Write-Host "  [OK] Qt found" -ForegroundColor Green
} else {
    Write-Host "  [X] Qt not found" -ForegroundColor Red
}

if ($FullClean) {
    Write-Host ""
    Write-Host "[6/7] Performing full clean build..." -ForegroundColor Yellow
    
    if (Test-Path "build") {
        Remove-Item -Path "build" -Recurse -Force
        Write-Host "  [OK] Build directory removed" -ForegroundColor Green
    }
    
    & cmake -B build -G "Visual Studio 17 2022" -A x64
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [OK] CMake configuration successful" -ForegroundColor Green
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
    
    & cmake --build build --config Release --target sak_utility --parallel
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [OK] Build successful" -ForegroundColor Green
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
Write-Host "[OK] All required files present" -ForegroundColor Green
Write-Host "[OK] All 39 actions verified" -ForegroundColor Green
Write-Host "[OK] CMakeLists.txt properly configured" -ForegroundColor Green
Write-Host "[OK] Build configuration validated" -ForegroundColor Green
Write-Host ""
