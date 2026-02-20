# Quick Test Script for ISO Downloader
# Run this instead of building the entire project

param(
    [switch]$Build,
    [switch]$Run,
    [switch]$All
)

$ErrorActionPreference = "Stop"
$TestName = "test_windows_iso_downloader"
$BuildDir = "build"

# Add Qt bin to PATH for DLL resolution
if ($env:Qt6_DIR) {
    $QtBin = Join-Path $env:Qt6_DIR "bin"
} else {
    $QtBin = "C:\Qt\6.5.3\msvc2019_64\bin"
}

if (Test-Path $QtBin) {
    $env:PATH += ";$QtBin"
}

if ($All) {
    $Build = $true
    $Run = $true
}

Write-Host "ISO Downloader Test Runner" -ForegroundColor Cyan
Write-Host "===========================" -ForegroundColor Cyan

if ($Build -or (-not $Run)) {
    Write-Host "`nBuilding test..." -ForegroundColor Yellow
    cmake --build $BuildDir --config Release --target $TestName
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        exit 1
    }
    Write-Host "Build successful!" -ForegroundColor Green
}

if ($Run -or $All) {
    Write-Host "`nRunning tests..." -ForegroundColor Yellow
    
    $TestExe = "$BuildDir\Release\Release\$TestName.exe"
    if (-not (Test-Path $TestExe)) {
        Write-Host "Test executable not found: $TestExe" -ForegroundColor Red
        exit 1
    }
    
    & $TestExe -v2
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`nAll tests passed!" -ForegroundColor Green
    } else {
        Write-Host "`nSome tests failed!" -ForegroundColor Red
        exit 1
    }
}

if (-not $Build -and -not $Run -and -not $All) {
    Write-Host @"

Usage:
  .\scripts\test-iso-downloader.ps1 -Build    # Just build the test
  .\scripts\test-iso-downloader.ps1 -Run      # Just run existing test
  .\scripts\test-iso-downloader.ps1 -All      # Build and run

Examples:
  # Quick iteration (already built):
  .\scripts\test-iso-downloader.ps1 -Run
  
  # Full cycle:
  .\scripts\test-iso-downloader.ps1 -All
"@ -ForegroundColor Gray
}
