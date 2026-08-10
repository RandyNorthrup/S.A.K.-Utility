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

# -- Local config (not tracked by git) ---------------------------
# Copy scripts/sign-config.template.ps1 to scripts/sign-config.ps1
# and fill in your subscription ID.
$configPath = Join-Path $PSScriptRoot "sign-config.ps1"
if (-not (Test-Path $configPath)) {
    Write-Error "Missing local config: $configPath`nCopy sign-config.template.ps1 to sign-config.ps1 and fill in your SubscriptionId."
    exit 1
}
. $configPath

# Fail closed on a missing or malformed subscription id from local config. A
# blank or non-GUID value would otherwise fall through to 'az account set' and
# silently keep whatever subscription was previously active.
if ([string]::IsNullOrWhiteSpace($SubscriptionId)) {
    Write-Error "SubscriptionId is not set. Copy sign-config.template.ps1 to sign-config.ps1 and fill in your SubscriptionId."
    exit 1
}
$parsedSubscriptionId = [guid]::Empty
if (-not [guid]::TryParse($SubscriptionId, [ref]$parsedSubscriptionId)) {
    Write-Error "SubscriptionId '$SubscriptionId' is not a valid GUID."
    exit 1
}

# -- Configuration ------------------------------------------------
$AccountName      = "scnetsolutions"
$ProfileName      = "SAKUtility"
$Endpoint         = "https://wus.codesigning.azure.net/"
$TimestampUrl     = "http://timestamp.acs.microsoft.com"

# -- Resolve exe path --------------------------------------------
$resolvedPath = Resolve-Path $ExePath -ErrorAction SilentlyContinue
if (-not $resolvedPath) {
    Write-Error "Executable not found: $ExePath"
    Write-Host "Run 'cmake --build build --config Release' first."
    exit 1
}
$ExePath = $resolvedPath.Path
$exeItem = Get-Item -LiteralPath $ExePath -ErrorAction SilentlyContinue
if (-not $exeItem -or $exeItem.PSIsContainer) {
    Write-Error "Refusing to sign '$ExePath': not a regular file."
    exit 1
}
if ($exeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
    Write-Error "Refusing to sign '$ExePath': reparse point (symlink/junction) is not a trusted signing target."
    exit 1
}
Write-Host "Signing: $ExePath"

# -- Check Azure CLI ---------------------------------------------
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
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to select Azure subscription $SubscriptionId (az account set exit $LASTEXITCODE)."
    exit 1
}
Write-Host "Using subscription: $SubscriptionId"

# -- Locate signtool ---------------------------------------------
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

# No PATH fallback: signtool must come from an installed Windows SDK, otherwise an
# attacker-planted signtool.exe earlier on PATH could be run with signing rights.
# Fail closed when the SDK copy is absent.
if (-not $signtool) {
    Write-Error "signtool.exe not found under the Windows SDK. Install Windows SDK: https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/"
    exit 1
}
Write-Host "Using signtool: $signtool"

# -- Install Azure Trusted Signing dlib ---------------------------
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

# -- Create metadata JSON for Trusted Signing --------------------
$metadataJson = Join-Path $dlibDir "metadata.json"
$jsonContent = @{
    Endpoint               = $Endpoint
    CodeSigningAccountName = $AccountName
    CertificateProfileName = $ProfileName
} | ConvertTo-Json
# Write UTF-8 without BOM (required by Azure.CodeSigning.Dlib)
[System.IO.File]::WriteAllText($metadataJson, $jsonContent, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Metadata: $metadataJson"

# -- Sign ---------------------------------------------------------
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

# -- Verify -------------------------------------------------------
Write-Host ""
Write-Host "=== Verifying signature ==="
$sig = Get-AuthenticodeSignature $ExePath
Write-Host "Status:    $($sig.Status)"
Write-Host "Signer:    $($sig.SignerCertificate.Subject)"
Write-Host "Issuer:    $($sig.SignerCertificate.Issuer)"
Write-Host "Timestamp: $($sig.TimeStamperCertificate.Subject)"

if ($sig.Status -ne 'Valid') {
    Write-Warning "Signature status is '$($sig.Status)' -- check Trusted Signing configuration."
    exit 1
}
if (-not $sig.SignerCertificate) {
    Write-Error "Signature reports Valid but carries no signer certificate; refusing to treat as signed."
    exit 1
}
if (-not $sig.TimeStamperCertificate) {
    Write-Error "Signature is not timestamped (a trusted timestamp was requested with /tr); refusing to treat as signed."
    exit 1
}
Write-Host ""
Write-Host "Code signing completed successfully!" -ForegroundColor Green
