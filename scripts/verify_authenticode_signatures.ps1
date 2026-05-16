<#
.SYNOPSIS
    Verifies Authenticode signatures for packaged Windows binaries.
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$RootDir
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path -LiteralPath $RootDir
$files = Get-ChildItem -LiteralPath $root.Path -Recurse -File |
    Where-Object { $_.Extension -in ".exe", ".dll" }

if ($files.Count -eq 0) {
    Write-Error "No .exe or .dll files found under $($root.Path)"
    exit 1
}

$invalid = @()
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
    }
}

if ($invalid.Count -gt 0) {
    Write-Host ""
    Write-Error "Unsigned or invalid packaged binaries found: $($invalid.Count)"
    $invalid | Format-Table -AutoSize | Out-String | Write-Error
    exit 1
}

Write-Host "All packaged .exe and .dll files have valid Authenticode signatures."
