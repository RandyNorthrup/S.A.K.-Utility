# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Who signed a file, checked against who must have. Dot-sourced by sign-exe.ps1.

.DESCRIPTION
    A valid Authenticode signature says only that SOMEONE signed the file. sign-exe.ps1 hands
    signtool a dlib that runs with signing rights, and then declares the exe signed; both need the
    signer's identity checked, not just the signature's validity. The subjects are compared
    exactly: a certificate whose subject merely starts with an allowed one is someone else.
#>

# The release publisher: the full subject of the Artifact Signing certificate, compared exactly.
# This is the one place it is written. sign-exe.ps1 checks the exe it signed against it, and
# verify_authenticode_signatures.ps1, the filesystem tool re-pin and release.yml's sign job (through
# the build job's "publisher" output) check the package against it. The certificate carries the
# validated identity's locality, state and country as well as CN and O; v0.9.3-rc.2's verify job
# refused every first-party binary because this held CN and O only.
$SakPublisherSubject = "CN=Randy Northrup, O=Randy Northrup, L=Ventura, S=California, C=US"

# Who signs the Artifact Signing client (Microsoft.ArtifactSigning.Client 1.0.146): its dlib and
# its own DLLs are "Microsoft Corporation"; the VC runtime DLLs it carries are "Microsoft Windows".
$MicrosoftSignerSubjects = @(
    "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US",
    "CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
)

# Why @LiteralPath may not be trusted as signed by one of @AllowedSubjects, or $null when it may.
function Get-AuthenticodeSignerRefusal {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string[]]$AllowedSubjects
    )
    $signature = Get-AuthenticodeSignature -LiteralPath $LiteralPath
    if ($signature.Status -ne "Valid") {
        return "$LiteralPath is not validly signed ($($signature.Status))"
    }
    if (-not $signature.SignerCertificate) {
        return "$LiteralPath reports a valid signature but carries no signer certificate"
    }
    $subject = $signature.SignerCertificate.Subject
    foreach ($allowed in $AllowedSubjects) {
        if ($subject -ceq $allowed) {
            return $null
        }
    }
    return "$LiteralPath is signed by '$subject', not by $($AllowedSubjects -join ' or ')"
}

# Why the exe sign-exe.ps1 just signed is not signed as it must be, or $null when it is: a valid
# signature, by @Publisher, with the trusted timestamp signtool was asked for.
function Get-SignedExeRefusal {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string]$Publisher
    )
    $refusal = Get-AuthenticodeSignerRefusal -LiteralPath $LiteralPath -AllowedSubjects @($Publisher)
    if ($refusal) {
        return $refusal
    }
    if (-not (Get-AuthenticodeSignature -LiteralPath $LiteralPath).TimeStamperCertificate) {
        return "$LiteralPath is not timestamped (a trusted timestamp was requested with /tr)"
    }
    return $null
}
