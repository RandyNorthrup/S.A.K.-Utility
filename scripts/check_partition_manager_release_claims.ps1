<#
.SYNOPSIS
    Verifies Partition Manager release/documentation certification claims match evidence.

.DESCRIPTION
    This non-destructive gate reads the machine-readable certification status and
    scans release-facing documentation for unsupported certification claims. It
    also verifies required caveat text remains present while the claim level is
    below hardware certification, and rejects stale blocker text once strict
    hardware certification is complete.
#>

[CmdletBinding()]
param(
    [string]$StatusPath = "",
    [string]$CertificationRoot = "artifacts\partition-manager-certification\readiness",
    [string]$ExternalEvidenceManifest = "",
    [string[]]$ClaimFiles = @(
        "README.md",
        "CHANGELOG.md",
        "docs\PRODUCTION_GRADE_AUDIT.md",
        "docs\PARTITION_MANAGER_PANEL_PLAN.md",
        "docs\PARTITION_MANAGER_CERTIFICATION.md",
        "docs\RELEASE_READINESS.md"
    )
)

$ErrorActionPreference = "Stop"

function Resolve-ProjectRoot {
    return Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return Get-Content -LiteralPath $resolved.Path -Raw | ConvertFrom-Json
}

function Resolve-StatusPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($StatusPath)) {
        return (Resolve-Path -LiteralPath $StatusPath -ErrorAction Stop).Path
    }

    $effectiveCertificationRoot = $CertificationRoot
    $effectiveExternalEvidenceManifest = $ExternalEvidenceManifest
    if ($CertificationRoot -eq "artifacts\partition-manager-certification\readiness" -and
        [string]::IsNullOrWhiteSpace($ExternalEvidenceManifest)) {
        $strictRoot = "artifacts\partition-manager-certification\vhd-strict"
        $strictManifest = "artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json"
        if ((Test-Path -LiteralPath (Join-Path $ProjectRoot $strictRoot) -PathType Container) -and
            (Test-Path -LiteralPath (Join-Path $ProjectRoot $strictManifest) -PathType Leaf)) {
            $effectiveCertificationRoot = $strictRoot
            $effectiveExternalEvidenceManifest = $strictManifest
        }
    }

    $statusOutputPath = Join-Path $ProjectRoot "artifacts\partition-manager-certification\claim-check-status.json"
    $arguments = @{
        CertificationRoot = $effectiveCertificationRoot
        OutputPath = $statusOutputPath
        Quiet = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($effectiveExternalEvidenceManifest)) {
        $arguments.ExternalEvidenceManifest = $effectiveExternalEvidenceManifest
    }

    & (Join-Path $ProjectRoot "scripts\get_partition_manager_certification_status.ps1") @arguments
    if (-not $?) {
        throw "Could not compute Partition Manager certification status"
    }
    return $statusOutputPath
}

function Get-ClaimLines {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot,
        [Parameter(Mandatory = $true)]
        [string[]]$Files
    )

    $claimLines = @()
    foreach ($relativePath in $Files) {
        $path = Join-Path $ProjectRoot $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Partition Manager claim file missing: $relativePath"
        }

        $lineNumber = 0
        foreach ($line in Get-Content -LiteralPath $path) {
            ++$lineNumber
            $claimLines += [pscustomobject]@{
                path = $relativePath
                line_number = $lineNumber
                text = $line
            }
        }
    }
    return $claimLines
}

function Test-AllowedCertificationContext {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    $allowedPattern =
        "(?i)(not|until|before|requires?|remain(?:s|ing)?|blocked|partial|gate|external|claim levels?|claim[-_ ]level|claim[-_ ]wording|emits|counts only|deterministic|do not mark|can say|when|without|incomplete|CodeCompleteOnly|certification matrix|certification-status|vhd_data_disk_certified|hardware_certified|every disposable-VHD)"
    return $Line -match $allowedPattern
}

function Assert-NoUnsupportedClaims {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$ClaimLines,
        [Parameter(Mandatory = $true)]
        [string]$ClaimLevel
    )

    $forbiddenPatterns = @()
    if ($ClaimLevel -eq "CodeCompleteOnly") {
        $forbiddenPatterns += "(?i)\bVhdDataDiskCertified\b"
        $forbiddenPatterns += "(?i)\bVHD[- ]certified\b"
    }
    if ($ClaimLevel -ne "HardwareCertified") {
        $forbiddenPatterns += "(?i)\bHardwareCertified\b"
        $forbiddenPatterns += "(?i)\bhardware[- ]certified\b"
        $forbiddenPatterns += "(?i)\bfully certified\b"
    }

    foreach ($line in $ClaimLines) {
        foreach ($pattern in $forbiddenPatterns) {
            if ($line.text -match $pattern -and -not (Test-AllowedCertificationContext -Line $line.text)) {
                throw "Unsupported Partition Manager certification claim at $($line.path):$($line.line_number): $($line.text)"
            }
        }
    }
}

function Assert-RequiredCaveats {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$ClaimLines,
        [Parameter(Mandatory = $true)]
        [string]$ClaimLevel
    )

    $allText = (($ClaimLines | ForEach-Object { $_.text }) -join "`n") -replace "\s+", " "
    $required = @()
    if ($ClaimLevel -eq "HardwareCertified") {
        $required = @(
            "HardwareCertified",
            "strict VHD proof has 12/12",
            "18/18 external VM/hardware/lab gates passed"
        )

        $forbidden = @(
            "destructive VM certification remains a release gate",
            "still require external VM/hardware/lab evidence",
            "hardware certification remains blocked",
            "claims still wait on the full external VM/hardware/lab matrix",
            "remaining 12 external VM/hardware/lab gates",
            "External VM/hardware/lab gates remain incomplete"
        )
        foreach ($phrase in $forbidden) {
            $normalizedPhrase = $phrase -replace "\s+", " "
            if ($allText.Contains($normalizedPhrase)) {
                throw "Stale Partition Manager certification blocker remains in release-facing docs: $phrase"
            }
        }
    }
    else {
        $required = @(
            "destructive VM certification remains a release gate",
            "System MBR-to-GPT, OS migration reboot proof, UEFI/BIOS boot repair, removable USB, SSD/NVMe, SSD secure erase, non-adjacent partition move, primary/logical, volume serial-number, dynamic-to-basic, and physical wipe cases still require external VM/hardware/lab evidence",
            "destructive certification remains partial",
            "CodeCompleteOnly"
        )

        if ($ClaimLevel -eq "CodeCompleteOnly") {
            $required += "VHD or VM/hardware/lab evidence is incomplete"
        }
        $required += "Do not mark the Partition Manager destructive paths fully certified until those reports exist"
    }

    foreach ($phrase in $required) {
        $normalizedPhrase = $phrase -replace "\s+", " "
        if (-not $allText.Contains($normalizedPhrase)) {
            throw "Partition Manager certification caveat missing from release-facing docs: $phrase"
        }
    }
}

$projectRoot = Resolve-ProjectRoot
Push-Location $projectRoot
try {
    $resolvedStatusPath = Resolve-StatusPath -ProjectRoot $projectRoot
    $status = Read-JsonFile -Path $resolvedStatusPath
    if ($status.tool -ne "partition-manager-certification-status") {
        throw "Unexpected Partition Manager certification status tool: $($status.tool)"
    }
    if ([string]::IsNullOrWhiteSpace($status.claim_level)) {
        throw "Partition Manager certification status missing claim_level"
    }

    $claimLines = @(Get-ClaimLines -ProjectRoot $projectRoot -Files $ClaimFiles)
    Assert-NoUnsupportedClaims -ClaimLines $claimLines -ClaimLevel $status.claim_level
    Assert-RequiredCaveats -ClaimLines $claimLines -ClaimLevel $status.claim_level

    Write-Host "Partition Manager release claims passed for claim level: $($status.claim_level)"
}
finally {
    Pop-Location
}
