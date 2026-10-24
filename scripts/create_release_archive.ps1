<#
.SYNOPSIS
    Creates the Windows release ZIP and checksum file.
#>

param(
    [string]$BuildDir = "build\Release",

    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [switch]$CleanMutableRuntimeData,

    [switch]$RecoverInterrupted
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "path_reparse_guard.ps1")

function Get-ArchiveOutputState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return [pscustomobject]@{ present = $false; sha256 = "" }
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Archive output is not an ordinary file: $Path"
    }
    return [pscustomobject]@{
        present = $true
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Assert-ArchiveOutputState([string]$Path, [object]$Expected) {
    $actual = Get-ArchiveOutputState $Path
    if ($actual.present -ne $Expected.present -or
        ($Expected.present -and $actual.sha256 -cne $Expected.sha256)) {
        throw "Archive output changed while previous release metadata was in force: $Path"
    }
}

function Restore-ArchiveOutput([string]$Path,
                               [object]$Previous,
                               [string]$SavedCopy,
                               [string]$CandidateHash) {
    $current = Get-ArchiveOutputState $Path
    if ($Previous.present) {
        if ($current.present -and $current.sha256 -ceq $Previous.sha256) { return }
        if ((Get-FileHash -LiteralPath $SavedCopy -Algorithm SHA256).Hash -cne $Previous.sha256) {
            throw "Archive recovery copy changed: $SavedCopy"
        }
        Copy-Item -LiteralPath $SavedCopy -Destination $Path -Force -ErrorAction Stop
    } elseif ($current.present) {
        if ($current.sha256 -cne $CandidateHash) {
            throw "Archive output has unknown bytes; refusing recovery delete: $Path"
        }
        Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    Assert-ArchiveOutputState $Path $Previous
}

function Publish-ArchiveOutput([string]$Candidate,
                                [string]$Destination,
                                [string]$ReplaceBackup,
                                [object]$Previous) {
    Assert-ArchiveOutputState $Destination $Previous
    if ($Previous.present) {
        [IO.File]::Replace($Candidate, $Destination, $ReplaceBackup)
    } else {
        [IO.File]::Move($Candidate, $Destination)
    }
}

function Assert-PreviousChecksumManifest([string]$BuildRoot,
                                         [string]$CurrentZipPath,
                                         [string]$ManifestPath,
                                         [object]$PreviousZip,
                                         [object]$PreviousManifest) {
    if (-not $PreviousManifest.present) {
        if ($PreviousZip.present) {
            throw "Previous checksum manifest is missing for existing ZIP: $CurrentZipPath"
        }
        return
    }
    $contents = Get-Content -LiteralPath $ManifestPath -Raw -ErrorAction Stop
    $zipRows = [regex]::Matches($contents,
        '(?im)^(?<name>[A-Za-z0-9][A-Za-z0-9._-]*-Windows-x64\.zip):[ \t]+(?<hash>[A-F0-9]{64})[ \t]*\r?$')
    if ($zipRows.Count -ne 1) {
        throw "Previous checksum manifest must name exactly one ZIP: $ManifestPath"
    }
    $referencedZip = Join-Path $BuildRoot $zipRows[0].Groups['name'].Value
    $referencedState = Get-ArchiveOutputState $referencedZip
    if (-not $referencedState.present -or
        $referencedState.sha256 -cne $zipRows[0].Groups['hash'].Value.ToUpperInvariant()) {
        throw "Previous checksum manifest ZIP hash does not match: $referencedZip"
    }
    if ($PreviousZip.present -and
        -not [string]::Equals($referencedZip, $CurrentZipPath,
                             [StringComparison]::OrdinalIgnoreCase)) {
        throw "Previous checksum manifest does not own existing target ZIP: $CurrentZipPath"
    }
}

$ArchiveLockRecordByteLimit = 4096

function Test-ArchiveStateMatch([object]$Actual, [object]$Expected) {
    return $Actual.present -eq $Expected.present -and
        (-not $Expected.present -or $Actual.sha256 -ceq $Expected.sha256)
}

function Write-ArchiveLockRecord([IO.FileStream]$Stream, [string]$Record) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($Record)
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush($true)
}

function Format-ArchivePreparedRecord([string]$Name,
                                      [string]$TransactionLeaf,
                                      [object]$PreviousZip,
                                      [object]$PreviousChecksums) {
    $lines = @('SAK_ARCHIVE_TXN_V1', "package=$Name", "transaction=$TransactionLeaf",
        "previous_zip_present=$([int]$PreviousZip.present)",
        "previous_zip_sha256=$($PreviousZip.sha256)",
        "previous_checksums_present=$([int]$PreviousChecksums.present)",
        "previous_checksums_sha256=$($PreviousChecksums.sha256)", 'PREPARED')
    return ($lines -join "`n") + "`n"
}

function Format-ArchiveReadyRecord([string]$ZipHash, [string]$ChecksumsHash) {
    return "candidate_zip_sha256=$ZipHash`ncandidate_checksums_sha256=$ChecksumsHash`nREADY_COMPLETE`n"
}

function Get-ArchiveRecordValue([string]$Line, [string]$Key) {
    $prefix = "$Key="
    if (-not $Line.StartsWith($prefix, [StringComparison]::Ordinal)) {
        throw "Archive recovery lock record lacks $Key"
    }
    return $Line.Substring($prefix.Length)
}

function Get-ArchiveRecordState([string]$Present, [string]$Hash, [string]$Label) {
    if (@('0', '1') -cnotcontains $Present -or
        ($Present -ceq '1' -and $Hash -cnotmatch '^[A-F0-9]{64}$') -or
        ($Present -ceq '0' -and $Hash -cne '')) {
        throw "Archive recovery lock record has invalid $Label state"
    }
    return [pscustomobject]@{ present = ($Present -ceq '1'); sha256 = $Hash }
}

function Read-ArchiveLockRecord([IO.FileStream]$Stream, [string]$ExpectedName) {
    if ($Stream.Length -le 0 -or $Stream.Length -gt $ArchiveLockRecordByteLimit) {
        throw 'Archive recovery lock record has invalid length'
    }
    $length = [int]$Stream.Length
    $bytes = New-Object byte[] $length
    $Stream.Position = 0
    $readTotal = 0
    while ($readTotal -lt $length) {
        $read = $Stream.Read($bytes, $readTotal, $length - $readTotal)
        if ($read -le 0) { throw 'Archive recovery lock record ended early' }
        $readTotal += $read
    }
    if (@($bytes | Where-Object { $_ -gt 127 }).Count -ne 0) {
        throw 'Archive recovery lock record is not ASCII'
    }
    $contents = [Text.Encoding]::ASCII.GetString($bytes)
    if ($contents.Contains("`r")) { throw 'Archive recovery lock record has CR bytes' }
    $split = $contents.Split([char]10)
    if ($split.Count -lt 9 -or $split[-1] -cne '') {
        throw 'Archive recovery lock record is incomplete'
    }
    $lines = @($split[0..($split.Count - 2)])
    if ($lines[0] -cne 'SAK_ARCHIVE_TXN_V1' -or $lines[7] -cne 'PREPARED') {
        throw 'Archive recovery lock record has an unknown format'
    }
    $name = Get-ArchiveRecordValue $lines[1] 'package'
    $transaction = Get-ArchiveRecordValue $lines[2] 'transaction'
    if ($name -cne $ExpectedName -or
        $transaction -cnotmatch '^\.sak-archive-txn-[0-9a-f]{32}$') {
        throw 'Archive recovery lock record targets a different package or transaction'
    }
    $previousZip = Get-ArchiveRecordState `
        (Get-ArchiveRecordValue $lines[3] 'previous_zip_present') `
        (Get-ArchiveRecordValue $lines[4] 'previous_zip_sha256') 'previous ZIP'
    $previousChecksums = Get-ArchiveRecordState `
        (Get-ArchiveRecordValue $lines[5] 'previous_checksums_present') `
        (Get-ArchiveRecordValue $lines[6] 'previous_checksums_sha256') 'previous checksums'
    $ready = $false
    $candidateZipHash = ''
    $candidateChecksumsHash = ''
    if ($lines.Count -eq 11 -and $lines[10] -ceq 'READY_COMPLETE') {
        $candidateZipHash = Get-ArchiveRecordValue $lines[8] 'candidate_zip_sha256'
        $candidateChecksumsHash = Get-ArchiveRecordValue $lines[9] 'candidate_checksums_sha256'
        if ($candidateZipHash -cnotmatch '^[A-F0-9]{64}$' -or
            $candidateChecksumsHash -cnotmatch '^[A-F0-9]{64}$') {
            throw 'Archive recovery lock record has invalid candidate hashes'
        }
        $ready = $true
    } elseif ($lines.Count -ne 8) {
        throw 'Archive recovery lock record has incomplete publication state'
    }
    return [pscustomobject]@{
        transaction = $transaction
        previous_zip = $previousZip
        previous_checksums = $previousChecksums
        ready = $ready
        candidate_zip_sha256 = $candidateZipHash
        candidate_checksums_sha256 = $candidateChecksumsHash
    }
}

function Assert-ArchiveRecoverySnapshot([string]$Path, [object]$Expected) {
    $actual = Get-ArchiveOutputState $Path
    if (-not (Test-ArchiveStateMatch $actual $Expected)) {
        throw "Archive recovery snapshot is missing or changed: $Path"
    }
}

function Assert-ArchiveRecoveryOutputAllowed([string]$Path,
                                             [object]$Current,
                                             [object]$Previous,
                                             [string]$CandidateHash) {
    if ($Current.present -and $Current.sha256 -cne $Previous.sha256 -and
        $Current.sha256 -cne $CandidateHash) {
        throw "Archive recovery refuses unknown output bytes: $Path"
    }
}

function Invoke-ArchiveRecovery([string]$BuildRoot, [string]$Name) {
    $lockPath = Join-Path $BuildRoot '.sak-archive-publish.lock'
    $lockItem = Get-StagePathItem $lockPath
    if ($null -eq $lockItem -or $lockItem.PSIsContainer -or
        ($lockItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Archive recovery needs one ordinary publication lock: $lockPath"
    }
    $stream = [IO.File]::Open($lockPath, [IO.FileMode]::Open,
                             [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $recovered = $false
    $recoveredState = ''
    try {
        $record = Read-ArchiveLockRecord $stream $Name
        $transaction = Join-Path $BuildRoot $record.transaction
        $pending = @(Get-ChildItem -LiteralPath $BuildRoot -Directory -Force -Filter '.sak-archive-txn-*')
        if ($pending.Count -gt 1 -or
            ($pending.Count -eq 1 -and $pending[0].FullName -cne $transaction)) {
            throw 'Archive recovery found an unrelated transaction directory'
        }
        $transactionPresent = $pending.Count -eq 1
        if ($transactionPresent) {
            [void](Get-StageDirectTreeItem -Root $transaction -Description 'Archive recovery transaction')
        }
        $zipPath = Join-Path $BuildRoot "$Name-Windows-x64.zip"
        $checksumsPath = Join-Path $BuildRoot 'SHA256SUMS.txt'
        $currentZip = Get-ArchiveOutputState $zipPath
        $currentChecksums = Get-ArchiveOutputState $checksumsPath
        $oldPair = (Test-ArchiveStateMatch $currentZip $record.previous_zip) -and
            (Test-ArchiveStateMatch $currentChecksums $record.previous_checksums)
        $newPair = $record.ready -and $currentZip.present -and $currentChecksums.present -and
            $currentZip.sha256 -ceq $record.candidate_zip_sha256 -and
            $currentChecksums.sha256 -ceq $record.candidate_checksums_sha256
        if (-not $oldPair -and -not $newPair) {
            if (-not $record.ready -or -not $transactionPresent) {
                throw 'Archive recovery lacks a ready transaction or snapshots for mixed outputs'
            }
            $savedZip = Join-Path $transaction 'previous.zip'
            $savedChecksums = Join-Path $transaction 'previous-checksums.txt'
            Assert-ArchiveRecoverySnapshot $savedZip $record.previous_zip
            Assert-ArchiveRecoverySnapshot $savedChecksums $record.previous_checksums
            Assert-ArchiveRecoveryOutputAllowed $zipPath $currentZip `
                $record.previous_zip $record.candidate_zip_sha256
            Assert-ArchiveRecoveryOutputAllowed $checksumsPath $currentChecksums `
                $record.previous_checksums $record.candidate_checksums_sha256
            Restore-ArchiveOutput $zipPath $record.previous_zip $savedZip $record.candidate_zip_sha256
            Restore-ArchiveOutput $checksumsPath $record.previous_checksums `
                $savedChecksums $record.candidate_checksums_sha256
            $currentZip = Get-ArchiveOutputState $zipPath
            $currentChecksums = Get-ArchiveOutputState $checksumsPath
            if (-not (Test-ArchiveStateMatch $currentZip $record.previous_zip) -or
                -not (Test-ArchiveStateMatch $currentChecksums $record.previous_checksums)) {
                throw 'Archive recovery could not prove the restored previous pair'
            }
            $recoveredState = 'previous'
        } elseif ($newPair) {
            $recoveredState = 'candidate'
        } else {
            $recoveredState = 'previous'
        }
        Assert-PreviousChecksumManifest $BuildRoot $zipPath $checksumsPath `
            $currentZip $currentChecksums
        if ($transactionPresent) {
            $allowed = @('package', "$Name-Windows-x64.zip", 'SHA256SUMS.txt',
                'previous.zip', 'previous-checksums.txt', 'replace-backup.zip',
                'replace-backup-checksums.txt')
            $extra = @(Get-ChildItem -LiteralPath $transaction -Force -ErrorAction Stop |
                Where-Object { $allowed -notcontains $_.Name })
            if ($extra.Count -ne 0) {
                throw "Archive recovery refuses unknown transaction entry: $($extra[0].FullName)"
            }
            [void](Assert-StageSafeDeleteTarget $transaction 'Container')
            Remove-Item -LiteralPath $transaction -Recurse -Force -ErrorAction Stop
        }
        $recovered = $true
    } finally {
        $stream.Dispose()
    }
    if ($recovered) {
        [IO.File]::Delete($lockPath)
        Write-Host "Archive interrupted transaction recovered: $recoveredState pair"
    }
}

# $PackageName is joined onto $BuildDir and then used to delete, clean, archive, and hash
# paths. Require a single, safe path segment so it cannot escape $BuildDir via separators,
# "..", or wildcard metacharacters.
if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $PackageName.Contains('..')) {
    throw "Invalid PackageName '$PackageName': must be a single path segment of letters, digits, '.', '_', or '-' with no '..'."
}

$build = Resolve-Path -LiteralPath $BuildDir
[void](Assert-StageDirectDirectoryPath -Path $build.Path `
    -Description 'Archive build directory' -RequireExisting $true)
if ($RecoverInterrupted) {
    if ($CleanMutableRuntimeData) {
        throw 'Archive recovery cannot also clean package runtime data'
    }
    Invoke-ArchiveRecovery $build.Path $PackageName
    return
}
$packageDir = Join-Path $build.Path $PackageName
$zipPath = Join-Path $build.Path "$PackageName-Windows-x64.zip"
$checksumsPath = Join-Path $build.Path "SHA256SUMS.txt"
$signingCatalog = Join-Path $packageDir "signing-catalog.txt"

if (-not (Test-Path -LiteralPath $packageDir -PathType Container)) {
    throw "Package directory not found: $packageDir"
}

# Crash evidence must survive refusal, including explicit runtime-data cleanup.
# Check before replacing any existing release metadata or creating the archive.
$crashEvidencePath = Join-Path $packageDir "data/crashes"
if (Test-Path -LiteralPath $crashEvidencePath) {
    throw "Refusing to archive package with crash evidence: data/crashes"
}

$packagedLocalProviderFiles = Get-ChildItem -LiteralPath $packageDir -Recurse -Filter "*.local.json" -ErrorAction Stop
if ($packagedLocalProviderFiles) {
    throw "Refusing to archive package with local provider config file: $($packagedLocalProviderFiles[0].FullName)"
}

$requiredPackageFiles = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "data/ai/providers/providers.json",
    "data/ai/app_manifests/windows_defender.json",
    "data/ai/app_manifests/superantispyware.json",
    "data/ai/app_manifests/windows_sfc.json"
)
foreach ($relativePath in $requiredPackageFiles) {
    $fullPath = Join-Path $packageDir $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Refusing to archive package missing required file: $relativePath"
    }
    $requiredItem = Get-Item -LiteralPath $fullPath -Force
    if ($requiredItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to archive package: required file is a reparse point: $relativePath"
    }
    if ($requiredItem.Attributes -band ([System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System)) {
        # Compress-Archive silently excludes hidden/system entries, so such a file would
        # pass this presence check yet be absent from the ZIP.
        throw "Refusing to archive package: required file is hidden or system and would be excluded from the archive: $relativePath"
    }
    if ($requiredItem.Length -le 0) {
        throw "Refusing to archive package: required file is empty: $relativePath"
    }
}

$mcpBuildLeak = Join-Path $packageDir "tools/mcp/_build"
if (Test-Path -LiteralPath $mcpBuildLeak) {
    throw "Refusing to archive package with temporary MCP build artifacts: $mcpBuildLeak"
}

$mutableRuntimePaths = @("data/ai_sessions", "data/temp", "data/logs", "data/config", "_logs")
foreach ($relativePath in $mutableRuntimePaths) {
    $mutablePath = Join-Path $packageDir $relativePath
    if (Test-Path -LiteralPath $mutablePath) {
        if ($CleanMutableRuntimeData) {
            Remove-Item -LiteralPath $mutablePath -Recurse -Force
            Write-Host "Removed mutable runtime data before archiving: $relativePath"
            continue
        }
        throw "Refusing to archive package with mutable runtime data: $relativePath"
    }
}

$chocoPath = Join-Path $packageDir "tools/chocolatey"
if (Test-Path -LiteralPath $chocoPath -PathType Container) {
    foreach ($relativePath in @("lib-bad", "cache", "temp")) {
        $mutablePath = Join-Path $chocoPath $relativePath
        if (Test-Path -LiteralPath $mutablePath) {
            if ($CleanMutableRuntimeData) {
                Remove-Item -LiteralPath $mutablePath -Recurse -Force
                Write-Host "Removed Chocolatey runtime state before archiving: tools/chocolatey/$relativePath"
                continue
            }
            throw "Refusing to archive package with Chocolatey runtime state: tools/chocolatey/$relativePath"
        }
    }
    $chocoLogs = Join-Path $chocoPath "logs"
    if (Test-Path -LiteralPath $chocoLogs) {
        if (-not (Test-Path -LiteralPath $chocoLogs -PathType Container)) {
            throw "Refusing to archive package with invalid Chocolatey logs path: $chocoLogs"
        }
        $logItems = @(Get-ChildItem -LiteralPath $chocoLogs -Force -ErrorAction Stop)
        if ($logItems.Count -gt 0) {
            if (-not $CleanMutableRuntimeData) {
                throw "Refusing to archive package with Chocolatey runtime logs: $chocoLogs"
            }
            [void](Assert-StageSafeDeleteTarget $chocoLogs "Container")
            Remove-Item -LiteralPath $chocoLogs -Recurse -Force -ErrorAction Stop
            New-Item -ItemType Directory -Path $chocoLogs -ErrorAction Stop | Out-Null
            Write-Host "Removed Chocolatey runtime logs before archiving"
        }
    }
}

$sourceEntries = @(Get-StageDirectTreeItem -Root $packageDir -Description 'Release package')
$hiddenSourceEntries = @($sourceEntries | Where-Object {
    $_.Attributes -band ([IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System)
})
if ($hiddenSourceEntries.Count -ne 0) {
    throw "Refusing to archive hidden/system package entry: $($hiddenSourceEntries[0].FullName)"
}

$previousZip = Get-ArchiveOutputState $zipPath
$previousChecksums = Get-ArchiveOutputState $checksumsPath
Assert-PreviousChecksumManifest $build.Path $zipPath $checksumsPath $previousZip $previousChecksums
$pendingTransactions = @(Get-ChildItem -LiteralPath $build.Path -Directory -Force -Filter '.sak-archive-txn-*')
if ($pendingTransactions.Count -ne 0) {
    throw "Unresolved archive transaction in build directory: $($pendingTransactions[0].FullName)"
}
$lockPath = Join-Path $build.Path '.sak-archive-publish.lock'
if (Test-Path -LiteralPath $lockPath) {
    throw "Unresolved archive publication lock: $lockPath"
}

$lockStream = $null
$transactionDir = $null
$ownsLock = $false
$cleanupAllowed = $false
$publicationAttempted = $false
try {
    $transactionLeaf = '.sak-archive-txn-' + [guid]::NewGuid().ToString('N')
    $transactionDir = Join-Path $build.Path $transactionLeaf
    $lockStream = [IO.File]::Open($lockPath,
                                  [IO.FileMode]::CreateNew,
                                  [IO.FileAccess]::ReadWrite,
                                  [IO.FileShare]::None)
    $ownsLock = $true
    Write-ArchiveLockRecord $lockStream (Format-ArchivePreparedRecord `
        $PackageName $transactionLeaf $previousZip $previousChecksums)
    New-Item -ItemType Directory -Path $transactionDir -ErrorAction Stop | Out-Null

    $candidatePackage = Join-Path $transactionDir 'package'
    New-Item -ItemType Directory -Path $candidatePackage -ErrorAction Stop | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $packageDir -Force -ErrorAction Stop)) {
        Copy-Item -LiteralPath $entry.FullName -Destination $candidatePackage -Recurse -Force -ErrorAction Stop
    }
    $copiedCatalog = Join-Path $candidatePackage 'signing-catalog.txt'
    if (Test-Path -LiteralPath $copiedCatalog -PathType Leaf) {
        Remove-Item -LiteralPath $copiedCatalog -Force -ErrorAction Stop
    }

    $candidateZip = Join-Path $transactionDir "$PackageName-Windows-x64.zip"
    $candidateChecksums = Join-Path $transactionDir 'SHA256SUMS.txt'
    Compress-Archive -Path (Join-Path $candidatePackage '*') -DestinationPath $candidateZip
    $candidateZipItem = Get-Item -LiteralPath $candidateZip -ErrorAction Stop
    if ($candidateZipItem.Length -le 0) { throw 'Candidate release ZIP is empty' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipReader = [IO.Compression.ZipFile]::OpenRead($candidateZip)
    try {
        foreach ($relativePath in $requiredPackageFiles) {
            $entryName = $relativePath.Replace('\', '/')
            $matches = @($zipReader.Entries | Where-Object { $_.FullName.Replace('\', '/') -ceq $entryName })
            if ($matches.Count -ne 1 -or $matches[0].Length -ne
                (Get-Item -LiteralPath (Join-Path $candidatePackage $relativePath)).Length) {
                throw "Candidate release ZIP lacks exact required file: $relativePath"
            }
        }
    } finally { $zipReader.Dispose() }

    $exeHash = (Get-FileHash -LiteralPath (Join-Path $candidatePackage 'sak_utility.exe') -Algorithm SHA256).Hash
    $helperHash = (Get-FileHash -LiteralPath (Join-Path $candidatePackage 'sak_elevated_helper.exe') -Algorithm SHA256).Hash
    $zipHash = (Get-FileHash -LiteralPath $candidateZip -Algorithm SHA256).Hash
    @"
SHA256 Checksums
================
sak_utility.exe:          $exeHash
sak_elevated_helper.exe:  $helperHash
$PackageName-Windows-x64.zip:  $zipHash
"@ | Out-File -LiteralPath $candidateChecksums -Encoding UTF8
    $candidateChecksumsHash = (Get-FileHash -LiteralPath $candidateChecksums -Algorithm SHA256).Hash

    $savedZip = Join-Path $transactionDir 'previous.zip'
    $savedChecksums = Join-Path $transactionDir 'previous-checksums.txt'
    if ($previousZip.present) {
        Copy-Item -LiteralPath $zipPath -Destination $savedZip -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $savedZip -Algorithm SHA256).Hash -cne $previousZip.sha256) {
            throw 'Previous ZIP recovery copy does not match source'
        }
    }
    if ($previousChecksums.present) {
        Copy-Item -LiteralPath $checksumsPath -Destination $savedChecksums -ErrorAction Stop
        if ((Get-FileHash -LiteralPath $savedChecksums -Algorithm SHA256).Hash -cne $previousChecksums.sha256) {
            throw 'Previous checksum recovery copy does not match source'
        }
    }
    Assert-ArchiveOutputState $zipPath $previousZip
    Assert-ArchiveOutputState $checksumsPath $previousChecksums

    Write-ArchiveLockRecord $lockStream (Format-ArchiveReadyRecord $zipHash $candidateChecksumsHash)
    $publicationAttempted = $true
    Publish-ArchiveOutput $candidateZip $zipPath (Join-Path $transactionDir 'replace-backup.zip') $previousZip
    Publish-ArchiveOutput $candidateChecksums $checksumsPath `
        (Join-Path $transactionDir 'replace-backup-checksums.txt') $previousChecksums
    Assert-ArchiveOutputState $zipPath ([pscustomobject]@{ present = $true; sha256 = $zipHash })
    Assert-ArchiveOutputState $checksumsPath ([pscustomobject]@{ present = $true; sha256 = $candidateChecksumsHash })
    $cleanupAllowed = $true
} catch {
    $archiveError = $_.Exception.Message
    if ($publicationAttempted) {
        try {
            Restore-ArchiveOutput $zipPath $previousZip $savedZip $zipHash
            Restore-ArchiveOutput $checksumsPath $previousChecksums $savedChecksums $candidateChecksumsHash
            $cleanupAllowed = $true
        } catch {
            throw "Archive publish failed ($archiveError); recovery unproved ($($_.Exception.Message)); retained transaction: $transactionDir"
        }
    } else {
        try {
            Assert-ArchiveOutputState $zipPath $previousZip
            Assert-ArchiveOutputState $checksumsPath $previousChecksums
            $cleanupAllowed = $true
        } catch {
            throw "Archive preparation failed ($archiveError); old outputs changed ($($_.Exception.Message)); retained transaction: $transactionDir"
        }
    }
    throw
} finally {
    if ($cleanupAllowed -and $null -ne $transactionDir -and
        (Test-Path -LiteralPath $transactionDir -PathType Container)) {
        [void](Assert-StageSafeDeleteTarget $transactionDir 'Container')
        Remove-Item -LiteralPath $transactionDir -Recurse -Force -ErrorAction Stop
    }
    if ($null -ne $lockStream) { $lockStream.Dispose() }
    if ($ownsLock -and $cleanupAllowed -and (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
        [IO.File]::Delete($lockPath)
    }
}

Write-Host ''
Write-Host '=== SHA256 Checksums ==='
Write-Host "sak_utility.exe        : $exeHash"
Write-Host "sak_elevated_helper.exe: $helperHash"
Write-Host "ZIP                    : $zipHash"
Write-Host ''
Write-Host "Package archive created successfully: $zipPath"
