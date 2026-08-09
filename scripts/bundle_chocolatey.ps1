# bundle_chocolatey.ps1
# Downloads and bundles portable Chocolatey
param(
    [string]$DestinationPath = ".\tools\chocolatey",
    [string]$ExpectedInstallScriptSha256 = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# The community install script is Authenticode signed by Chocolatey Software. The bundle
# refuses to execute a downloaded installer that is unsigned, tampered, or signed by anyone
# else, instead of trusting it because the TLS fetch succeeded.
$ChocoInstallScriptPublisher = "Chocolatey Software, Inc"

function Test-ReparsePointPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $item = Get-Item -LiteralPath $Path -Force
    return (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq
        [System.IO.FileAttributes]::ReparsePoint)
}

function Get-BundledChocoExe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    # bin\choco.exe is the shim the app launches; the installer's root copy is accepted only
    # when the shim is absent. Whichever is used must be a real file, never a directory and
    # never a reparse point pointing somewhere else.
    foreach ($candidate in @((Join-Path $Root "bin\choco.exe"), (Join-Path $Root "choco.exe"))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            if (Test-ReparsePointPath -Path $candidate) {
                throw "Bundled choco.exe is a reparse point: $candidate"
            }
            return (Get-Item -LiteralPath $candidate -Force).FullName
        }
    }
    return ""
}

function Assert-BundledChocoWorks {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePath
    )

    # Existence is not proof of a working bundle: run the executable and require both a zero
    # exit code and a real version number before the bundle is reported successful.
    $global:LASTEXITCODE = 0
    $versionOutput = & $ExePath --version
    if ($LASTEXITCODE -ne 0) {
        throw "choco.exe --version failed with exit code ${LASTEXITCODE}: $ExePath"
    }
    $version = @($versionOutput) | Where-Object { $_ -match '^\d+\.\d+' } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "choco.exe --version did not report a version number: $ExePath"
    }
    return [string]$version
}

function Write-BundleSummary {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    Write-Host "Version: $Version" -ForegroundColor Gray
    $SizeMB = [math]::Round((Get-ChildItem -LiteralPath $Root -Recurse -File |
            Measure-Object -Property Length -Sum).Sum / 1MB, 2)
    Write-Host "Total size: $SizeMB MB" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Chocolatey portable bundled successfully!" -ForegroundColor Green
    Write-Host "Location: $Root" -ForegroundColor Gray
}

Write-Host "=== Chocolatey Portable Bundle Script ===" -ForegroundColor Cyan
Write-Host ""

if ([string]::IsNullOrWhiteSpace($DestinationPath)) {
    throw "DestinationPath must not be empty"
}
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)
$DestinationParent = Split-Path -Parent $DestinationPath
if ([string]::IsNullOrWhiteSpace($DestinationParent)) {
    throw "Refusing to use a drive root as the Chocolatey bundle destination: $DestinationPath"
}
Write-Host "Destination: $DestinationPath" -ForegroundColor Yellow

if (Test-Path -LiteralPath $DestinationPath) {
    # -Recurse -Force deletion must not be redirected through a junction or symlink, and
    # -LiteralPath keeps a destination containing [] or * from expanding as a wildcard.
    if (Test-ReparsePointPath -Path $DestinationPath) {
        throw "Chocolatey bundle destination is a reparse point; refusing to delete through it: $DestinationPath"
    }
    if ($Force) {
        Write-Host "Removing existing Chocolatey installation..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $DestinationPath -Recurse -Force
    } else {
        $ExistingExe = Get-BundledChocoExe -Root $DestinationPath
        if ([string]::IsNullOrWhiteSpace($ExistingExe)) {
            throw "Destination exists but contains no choco.exe: $DestinationPath. Re-run with -Force to re-download."
        }
        $ExistingVersion = Assert-BundledChocoWorks -ExePath $ExistingExe
        Write-Host "Chocolatey already bundled at: $DestinationPath (version $ExistingVersion)" -ForegroundColor Green
        Write-Host "Use -Force to re-download and replace" -ForegroundColor Gray
        exit 0
    }
}

Write-Host "Preparing destination parent directory..." -ForegroundColor Yellow
New-Item -ItemType Directory -Path $DestinationParent -Force | Out-Null

# A unique, exclusively created download directory: a predictable shared name lets another
# process pre-create it (or a junction with that name) and swap the installer between the
# download and the execution below.
$TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("choco_portable_download_" +
    [System.IO.Path]::GetRandomFileName())
$ChocoZip = Join-Path $TempDir "chocolatey.zip"

Write-Host "Creating temporary directory: $TempDir" -ForegroundColor Yellow
if (Test-Path -LiteralPath $TempDir) {
    throw "Temporary download directory already exists: $TempDir"
}
New-Item -ItemType Directory -Path $TempDir | Out-Null

$ChocoUrl = "https://community.chocolatey.org/install.ps1"
Write-Host ""
Write-Host "Downloading Chocolatey installation script..." -ForegroundColor Cyan

try {
    $InstallScript = Join-Path $TempDir "install.ps1"
    Invoke-WebRequest -Uri $ChocoUrl -OutFile $InstallScript -UseBasicParsing -MaximumRedirection 5
    if (-not (Test-Path -LiteralPath $InstallScript -PathType Leaf)) {
        throw "Chocolatey install script was not written to $InstallScript"
    }
    Write-Host "Downloaded installation script" -ForegroundColor Green

    $ActualSha256 = (Get-FileHash -LiteralPath $InstallScript -Algorithm SHA256).Hash
    Write-Host "Install script SHA-256: $ActualSha256" -ForegroundColor Gray
    if (-not [string]::IsNullOrWhiteSpace($ExpectedInstallScriptSha256)) {
        $Pinned = $ExpectedInstallScriptSha256.Trim()
        if ($ActualSha256 -ne $Pinned.ToUpperInvariant()) {
            throw "Chocolatey install script SHA-256 mismatch. Expected $Pinned, got $ActualSha256"
        }
        Write-Host "Install script matches the pinned SHA-256" -ForegroundColor Green
    }

    $Signature = Get-AuthenticodeSignature -LiteralPath $InstallScript
    if ($Signature.Status -ne "Valid") {
        throw "Chocolatey install script signature is not valid (status: $($Signature.Status))"
    }
    $Signer = if ($Signature.SignerCertificate) { $Signature.SignerCertificate.Subject } else { "" }
    if ($Signer -notlike "*$ChocoInstallScriptPublisher*") {
        throw "Chocolatey install script signed by an unexpected publisher: $Signer"
    }
    Write-Host "Install script signature verified: $Signer" -ForegroundColor Green

    $env:ChocolateyInstall = $DestinationPath
    $OriginalPath = $env:Path
    $env:Path = (($OriginalPath -split ';') |
        Where-Object { $_ -and ($_ -notmatch '\\chocolatey\\bin\\?$') }) -join ';'

    Write-Host ""
    Write-Host "Installing Chocolatey portable to: $DestinationPath" -ForegroundColor Cyan
    Write-Host "This may take a few minutes..." -ForegroundColor Gray

    & $InstallScript

    $ChocoExe = Get-BundledChocoExe -Root $DestinationPath
    if ([string]::IsNullOrWhiteSpace($ChocoExe)) {
        Write-Host ""
        Write-Host "Installation failed: choco.exe not found" -ForegroundColor Red
        exit 1
    }

    $Version = Assert-BundledChocoWorks -ExePath $ChocoExe
    Write-Host ""
    Write-Host "Success! Chocolatey installed" -ForegroundColor Green
    Write-BundleSummary -Root $DestinationPath -Version $Version

} catch {
    $ChocoExe = Get-BundledChocoExe -Root $DestinationPath
    if ([string]::IsNullOrWhiteSpace($ChocoExe)) {
        Write-Host ""
        Write-Host "Error downloading or installing Chocolatey:" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
        exit 1
    }

    Write-Host ""
    Write-Host "Chocolatey installer reported a post-install error:" -ForegroundColor DarkYellow
    Write-Host $_.Exception.Message -ForegroundColor DarkYellow
    Write-Host "Verifying the bundled choco.exe before accepting the bundle..." -ForegroundColor Yellow
    try {
        $Version = Assert-BundledChocoWorks -ExePath $ChocoExe
    } catch {
        Write-Host "Bundled choco.exe failed verification:" -ForegroundColor Red
        Write-Host $_.Exception.Message -ForegroundColor Red
        exit 1
    }
    Write-BundleSummary -Root $DestinationPath -Version $Version
} finally {
    if ($OriginalPath) {
        $env:Path = $OriginalPath
    }
    Write-Host ""
    Write-Host "Cleaning up temporary files..." -ForegroundColor Yellow
    if (Test-Path -LiteralPath $TempDir) {
        Remove-Item -LiteralPath $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "=== Bundle Complete ===" -ForegroundColor Cyan
