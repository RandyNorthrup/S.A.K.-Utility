<#
.SYNOPSIS
    Sign SAK Utility executable using Azure Trusted Signing.

.DESCRIPTION
    Signs build/Release/sak_utility.exe via Azure Trusted Signing (Code Signing).
    Requires:
      - Azure CLI installed and logged in (az login)
      - Access to the "scnetsolutions" Trusted Signing account in resource group "SAK"

.PARAMETER ExePath
    Path to the executable to sign. Defaults to build/Release/sak_utility.exe.

.EXAMPLE
    .\scripts\sign-exe.ps1
    .\scripts\sign-exe.ps1 -ExePath "C:\path\to\sak_utility.exe"
#>

param(
    [string]$ExePath = "build\Release\sak_utility.exe"
)

$ErrorActionPreference = "Stop"

# ── Configuration ────────────────────────────────────────────────
$AccountName      = "scnetsolutions"
$ProfileName      = "SAKUtility"
$Endpoint         = "https://wus.codesigning.azure.net/"
$TimestampUrl     = "http://timestamp.acs.microsoft.com"
$SubscriptionId   = "781cdf6a-5b53-4e2b-9317-1b04c712466d"

# ── Resolve exe path ────────────────────────────────────────────
$resolvedPath = Resolve-Path $ExePath -ErrorAction SilentlyContinue
if (-not $resolvedPath) {
    Write-Error "Executable not found: $ExePath"
    Write-Host "Run 'cmake --build build --config Release' first."
    exit 1
}
$ExePath = $resolvedPath.Path
Write-Host "Signing: $ExePath"

# ── Check Azure CLI ─────────────────────────────────────────────
$azCmd = Get-Command az -ErrorAction SilentlyContinue
if (-not $azCmd) {
    Write-Error "Azure CLI (az) not found. Install from https://aka.ms/installazurecliwindows"
    exit 1
}

# Verify login
$null = az account show 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "Not logged in to Azure. Running 'az login'..."
    az login
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Azure login failed."
        exit 1
    }
}

# Set subscription
az account set --subscription $SubscriptionId
Write-Host "Using subscription: $SubscriptionId"

# ── Locate signtool ─────────────────────────────────────────────
$signtool = $null

# Check Windows SDK paths
$sdkPaths = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe",
    "${env:ProgramFiles}\Windows Kits\10\bin\*\x64\signtool.exe"
)

foreach ($pattern in $sdkPaths) {
    $found = Get-ChildItem $pattern -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) {
        $signtool = $found.FullName
        break
    }
}

if (-not $signtool) {
    # Try PATH
    $signtool = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
}

if (-not $signtool) {
    Write-Error "signtool.exe not found. Install Windows SDK: https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/"
    exit 1
}
Write-Host "Using signtool: $signtool"

# ── Install Azure Trusted Signing dlib ───────────────────────────
# The Trusted Signing dlib (Azure.CodeSigning.Dlib) is required by signtool.
# Install via NuGet if not already present.
$dlibDir = Join-Path $PSScriptRoot "..\build\trusted-signing-dlib"
$dlibDll = Join-Path $dlibDir "bin\x64\Azure.CodeSigning.Dlib.dll"

if (-not (Test-Path $dlibDll)) {
    Write-Host "Installing Azure Trusted Signing dlib..."
    New-Item -ItemType Directory -Force -Path $dlibDir | Out-Null

    $nugetUrl = "https://www.nuget.org/api/v2/package/Microsoft.Trusted.Signing.Client"
    $nugetZip = Join-Path $dlibDir "Microsoft.Trusted.Signing.Client.zip"

    Invoke-WebRequest -Uri $nugetUrl -OutFile $nugetZip
    Expand-Archive -Path $nugetZip -DestinationPath $dlibDir -Force
    Remove-Item $nugetZip -ErrorAction SilentlyContinue

    if (-not (Test-Path $dlibDll)) {
        Write-Error "Failed to extract Azure.CodeSigning.Dlib.dll from NuGet package."
        exit 1
    }
    Write-Host "Dlib installed: $dlibDll"
}

# ── Create metadata JSON for Trusted Signing ────────────────────
$metadataJson = Join-Path $dlibDir "metadata.json"
$jsonContent = @{
    Endpoint               = $Endpoint
    CodeSigningAccountName = $AccountName
    CertificateProfileName = $ProfileName
} | ConvertTo-Json
# Write UTF-8 without BOM (required by Azure.CodeSigning.Dlib)
[System.IO.File]::WriteAllText($metadataJson, $jsonContent, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Metadata: $metadataJson"

# ── Sign ─────────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Signing executable ==="
& $signtool sign /v `
    /fd SHA256 `
    /tr $TimestampUrl `
    /td SHA256 `
    /dlib $dlibDll `
    /dmdf $metadataJson `
    $ExePath

if ($LASTEXITCODE -ne 0) {
    Write-Error "Signing failed (exit code $LASTEXITCODE)."
    exit 1
}

# ── Verify ───────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Verifying signature ==="
$sig = Get-AuthenticodeSignature $ExePath
Write-Host "Status:    $($sig.Status)"
Write-Host "Signer:    $($sig.SignerCertificate.Subject)"
Write-Host "Issuer:    $($sig.SignerCertificate.Issuer)"
Write-Host "Timestamp: $($sig.TimeStamperCertificate.Subject)"

if ($sig.Status -eq 'Valid') {
    Write-Host ""
    Write-Host "Code signing completed successfully!" -ForegroundColor Green
} else {
    Write-Warning "Signature status is '$($sig.Status)' -- check Trusted Signing configuration."
    exit 1
}
