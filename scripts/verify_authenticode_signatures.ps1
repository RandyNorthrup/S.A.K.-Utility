<#
.SYNOPSIS
    Verifies Authenticode signatures for packaged Windows binaries.

.DESCRIPTION
    Every packaged .exe and .dll must carry a valid signature. The binaries this project builds
    (sak_*) must also carry exactly the publisher sakutility.com names, so a run signed by anything
    else stops here instead of being published. Bundled third-party tools keep their own vendor's
    signature: choco.exe has to stay Chocolatey-signed, because the app refuses to run it otherwise.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$RootDir,

    # The subject of the certificate the first-party binaries must be signed with.
    [string]$ExpectedPublisher = "CN=Randy Northrup, O=Randy Northrup"
)

$ErrorActionPreference = "Stop"

# Plain output as well as the error record: a redirected error record wraps at the host's width.
function Stop-Verification([string]$Message) {
    Write-Host $Message
    Write-Error $Message
    exit 1
}

$root = Resolve-Path -LiteralPath $RootDir
# -Force so hidden/system .exe/.dll are enumerated too: without it an unsigned hidden binary
# would be skipped and the gate would still report success (fail-open).
$files = Get-ChildItem -LiteralPath $root.Path -Recurse -File -Force |
    Where-Object { $_.Extension -in ".exe", ".dll" }

if ($files.Count -eq 0) {
    Stop-Verification "No .exe or .dll files found under $($root.Path)"
}

# The package always carries these. A tree without them is not the package, and would otherwise
# pass the publisher check by having nothing to check.
$requiredFirstParty = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "sak_apfs_writer_cli.exe",
    "sak_hfs_writer_cli.exe"
)
$missing = @($requiredFirstParty | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $root.Path $_) -PathType Leaf)
    })
if ($missing.Count -gt 0) {
    Stop-Verification "First-party binaries missing from $($root.Path): $($missing -join ', ')"
}

$invalid = @()
$wrongPublisher = @()
foreach ($file in $files) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    $relative = $file.FullName.Substring($root.Path.Length + 1)
    $signer = if ($signature.SignerCertificate) {
        $signature.SignerCertificate.Subject
    } else {
        ""
    }

    Write-Host ("{0,-12} {1} {2}" -f $signature.Status, $relative, $signer)
    if ($signature.Status -ne "Valid") {
        $invalid += [pscustomobject]@{
            Path   = $relative
            Status = $signature.Status
        }
    } elseif ($file.Name -like "sak_*" -and $signer -cne $ExpectedPublisher) {
        $wrongPublisher += [pscustomobject]@{
            Path    = $relative
            Subject = $signer
        }
    }
}

if ($invalid.Count -gt 0) {
    Write-Host ""
    $invalid | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Stop-Verification "Unsigned or invalid packaged binaries found: $($invalid.Count)"
}

if ($wrongPublisher.Count -gt 0) {
    Write-Host ""
    $wrongPublisher | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    Stop-Verification "First-party binaries not signed by '$ExpectedPublisher': $($wrongPublisher.Count)"
}

Write-Host "All packaged .exe and .dll files have valid Authenticode signatures, and the first-party binaries are signed by $ExpectedPublisher."
