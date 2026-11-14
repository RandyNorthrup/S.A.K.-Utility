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

# Who signed the dlib signtool runs, and who signed the exe it produced: Get-AuthenticodeSignerRefusal,
# Get-SignedExeRefusal, $MicrosoftSignerSubjects and $SakPublisherSubject.
. (Join-Path $PSScriptRoot "authenticode_signer.ps1")

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

# -- Install the Artifact Signing client dlib ---------------------
# signtool loads Azure.CodeSigning.Dlib.dll from Microsoft.ArtifactSigning.Client, the
# successor of the now-unlisted Microsoft.Trusted.Signing.Client. The package is pinned by
# version and by the SHA-512 nuget.org records for it, in a per-version folder so an older
# install is never picked up.
$dlibPackage = "Microsoft.ArtifactSigning.Client"
$dlibVersion = "1.0.146"
$dlibSha512 = "/G0hG/VDIS6psW23b5t1V/ORIgn0PVAOtGj9w+hkPmAuI4pempfwlW50E2GtHscnz9+5Qz3hbC4mcb7ej2Yn1A=="
$dlibDir = Join-Path $PSScriptRoot "..\build\artifact-signing-client\$dlibVersion"
$dlibDll = Join-Path $dlibDir "bin\x64\Azure.CodeSigning.Dlib.dll"

if (-not (Test-Path $dlibDll)) {
    Write-Host "Installing $dlibPackage $dlibVersion..."
    New-Item -ItemType Directory -Force -Path $dlibDir | Out-Null

    $nugetUrl = "https://www.nuget.org/api/v2/package/$dlibPackage/$dlibVersion"
    $nugetZip = Join-Path $dlibDir "$dlibPackage.$dlibVersion.zip"

    Invoke-WebRequest -Uri $nugetUrl -OutFile $nugetZip -UseBasicParsing
    $sha512 = [Convert]::ToBase64String(
        [Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($nugetZip)))
    if ($sha512 -cne $dlibSha512) {
        Remove-Item $nugetZip -ErrorAction SilentlyContinue
        Write-Error "$dlibPackage $dlibVersion SHA-512 mismatch: expected $dlibSha512, got $sha512."
        exit 1
    }
    Expand-Archive -Path $nugetZip -DestinationPath $dlibDir -Force
    Remove-Item $nugetZip -ErrorAction SilentlyContinue

    if (-not (Test-Path $dlibDll)) {
        Write-Error "Failed to extract Azure.CodeSigning.Dlib.dll from $dlibPackage $dlibVersion."
        exit 1
    }
    Write-Host "Dlib installed: $dlibDll"
}

# signtool runs this DLL with signing rights, and it loads the DLLs beside it: refuse them unless
# each is signed by Microsoft. A valid signature only says that someone signed the file, so a dlib
# swapped for another signer's would have run with signing rights.
foreach ($dlibFile in @(Get-ChildItem -LiteralPath (Split-Path -Parent $dlibDll) -Filter *.dll -File -Force)) {
    $refusal = Get-AuthenticodeSignerRefusal -LiteralPath $dlibFile.FullName -AllowedSubjects $MicrosoftSignerSubjects
    if ($refusal) {
        Write-Error "Refusing the Artifact Signing client: $refusal"
        exit 1
    }
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

# Valid, by the publisher the project signs as, and timestamped: a valid signature from any other
# certificate the profile could hand out is not this project's.
$refusal = Get-SignedExeRefusal -LiteralPath $ExePath -Publisher $SakPublisherSubject
if ($refusal) {
    Write-Error "Refusing to treat the exe as signed: $refusal -- check the Trusted Signing configuration."
    exit 1
}
Write-Host ""
Write-Host "Code signing completed successfully!" -ForegroundColor Green
