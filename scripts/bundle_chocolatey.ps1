# bundle_chocolatey.ps1
# Downloads and bundles portable Chocolatey
param(
    [string]$DestinationPath = ".\tools\chocolatey",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

Write-Host "=== Chocolatey Portable Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow

if (Test-Path $DestinationPath) {
    if ($Force) {
        Write-Host "Removing existing Chocolatey installation..." -ForegroundColor Yellow
        Remove-Item -Path $DestinationPath -Recurse -Force
    } else {
        Write-Host "Chocolatey already bundled at: $DestinationPath" -ForegroundColor Green
        Write-Host "Use -Force to re-download and replace" -ForegroundColor Gray
        exit 0
    }
}

Write-Host "Creating destination directory..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path $DestinationPath -Force | Out-Null

$TempDir = Join-Path $env:TEMP "choco_portable_download"
$ChocoZip = Join-Path $TempDir "chocolatey.zip"

Write-Host "Creating temporary directory: $TempDir" -ForegroundColor Yellow
if (Test-Path $TempDir) {
    Remove-Item -Path $TempDir -Recurse -Force
}
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

$ChocoUrl = "https://community.chocolatey.org/install.ps1"
Write-Host ""
Write-Host "Downloading Chocolatey installation script..." -ForegroundColor Cyan

try {
    $InstallScript = Join-Path $TempDir "install.ps1"
    Invoke-WebRequest -Uri $ChocoUrl -OutFile $InstallScript -UseBasicParsing
    Write-Host "Downloaded installation script" -ForegroundColor Green
    
    $env:ChocolateyInstall = $DestinationPath
    
    Write-Host ""
    Write-Host "Installing Chocolatey portable to: $DestinationPath" -ForegroundColor Cyan
    Write-Host "This may take a few minutes..." -ForegroundColor Gray
    
    & $InstallScript
    
    $ChocoExe = Join-Path $DestinationPath "bin\choco.exe"
    if (-not (Test-Path $ChocoExe)) {
        $ChocoExe = Join-Path $DestinationPath "choco.exe"
    }
    
    if (Test-Path $ChocoExe) {
        Write-Host ""
        Write-Host "Success! Chocolatey installed" -ForegroundColor Green
        
        $Version = & $ChocoExe --version 2>&1
        Write-Host "Version: $Version" -ForegroundColor Gray
        
        $SizeMB = [math]::Round((Get-ChildItem -Path $DestinationPath -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB, 2)
        Write-Host "Total size: $SizeMB MB" -ForegroundColor Gray
        
        Write-Host ""
        Write-Host "Chocolatey portable bundled successfully!" -ForegroundColor Green
        Write-Host "Location: $DestinationPath" -ForegroundColor Gray
        
    } else {
        Write-Host ""
        Write-Host "Installation failed: choco.exe not found" -ForegroundColor Red
        exit 1
    }
    
} catch {
    Write-Host ""
    Write-Host "Error downloading or installing Chocolatey:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
} finally {
    Write-Host ""
    Write-Host "Cleaning up temporary files..." -ForegroundColor Yellow
    if (Test-Path $TempDir) {
        Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "=== Bundle Complete ===" -ForegroundColor Cyan
