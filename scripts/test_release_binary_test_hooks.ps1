<#
.SYNOPSIS
    Fails when the product libraries or the application carry partition/filesystem test hooks or
    the SAK_DEV_SOURCE_TREE escape hatch.

.DESCRIPTION
    Owner ruling 2026-10-24: test hooks and escape hatches do not live in production paths. The
    partition/filesystem engine's test-only seams (the *ForTesting entry points, the generated-
    image readers, the raw-target predicate, the executor's broker observer, the removed APFS
    evidence validator) are compiled only under SAK_PARTITION_FS_TEST_HOOKS, and the manifest
    resolver no longer reads SAK_DEV_SOURCE_TREE.

    A Release executable has no symbol table, so the function names are checked where they would
    be: the static product libraries every shipped binary links. A static library's linker member
    and each object's COFF string table name every external symbol it defines, so a hook compiled
    into the library is visible as its decorated name. The environment-variable name would be a
    string literal, so it is checked in the libraries and in the executable itself.

    Each file is also required to contain a symbol or literal that MUST be there, so a scan of
    the wrong file, an empty file or an unreadable file cannot pass by finding nothing.

    Parameters are separate and named on purpose: an array parameter passed through
    `pwsh -File` binds only its first element (AGENTS.md, "Ways a green run means nothing").
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$PartitionFsLibrary,
    [Parameter(Mandatory = $true)][string]$CoreLibrary,
    [Parameter(Mandatory = $true)][string]$ApplicationExe
)

$ErrorActionPreference = "Stop"

$chunkBytes = 64MB

# Which of $Tokens occur anywhere in the file. The libraries run to gigabytes (LTCG objects), so
# the file is read in chunks; each chunk keeps the previous chunk's tail so a token that straddles
# a boundary is still found. Latin-1 maps each byte to one char, so an ordinal search over a chunk
# finds an ASCII name exactly.
function Find-Tokens {
    param([string]$Path, [string[]]$Tokens)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Binary not found: $full"
    }
    $found = @{}
    $overlap = ($Tokens | Measure-Object -Property Length -Maximum).Maximum
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $buffer = New-Object byte[] ($chunkBytes + $overlap)
    $stream = [System.IO.File]::OpenRead($full)
    try {
        if ($stream.Length -eq 0) {
            throw "Binary is empty: $full"
        }
        $carry = 0
        while ($true) {
            $read = $stream.Read($buffer, $carry, $chunkBytes)
            if ($read -le 0) {
                break
            }
            $valid = $carry + $read
            $text = $latin1.GetString($buffer, 0, $valid)
            foreach ($token in $Tokens) {
                if (-not $found.ContainsKey($token) -and
                    $text.IndexOf($token, [System.StringComparison]::Ordinal) -ge 0) {
                    $found[$token] = $true
                }
            }
            $carry = [Math]::Min($overlap, $valid)
            [Array]::Copy($buffer, $valid - $carry, $buffer, 0, $carry)
        }
    } finally {
        $stream.Dispose()
    }
    return $found
}

function Assert-Scan {
    param(
        [string]$Path,
        [string[]]$MustContain,
        [string[]]$MustNotContain
    )
    $found = Find-Tokens -Path $Path -Tokens ($MustContain + $MustNotContain)
    $failures = @()
    foreach ($token in $MustContain) {
        if (-not $found.ContainsKey($token)) {
            $failures += "positive control missing (wrong or unreadable file?): $token"
        }
    }
    foreach ($token in $MustNotContain) {
        if ($found.ContainsKey($token)) {
            $failures += "test hook or escape hatch present: $token"
        }
    }
    if ($failures.Count -gt 0) {
        return @($failures | ForEach-Object { "{0}: {1}" -f (Split-Path -Leaf $Path), $_ })
    }
    Write-Host ("OK {0} ({1} forbidden tokens absent, {2} controls present)" -f `
            (Split-Path -Leaf $Path), $MustNotContain.Count, $MustContain.Count)
    return @()
}

$escapeHatch = "SAK_DEV_SOURCE_TREE"

# Decorated-name fragments. "<name>@<Class>@sak@@" is how MSVC spells a member of sak::<Class>.
$partitionFsHooks = @(
    "ForTesting@PartitionApfsWriter@sak@@",
    "ForTesting@PartitionApfsFileSystemReader@sak@@",
    "ForTesting@PartitionExtFileSystemReader@sak@@",
    "ForTesting@PartitionHfsFileSystemReader@sak@@",
    "?readGeneratedLiveCibAddr@PartitionApfsWriter@sak@@",
    "?readGeneratedChunkBitmapAddr@PartitionApfsWriter@sak@@",
    "?readGeneratedChunkFreeCount@PartitionApfsWriter@sak@@",
    "?readGeneratedVolumeOmapTreeLevel@PartitionApfsWriter@sak@@",
    "?readGeneratedExtentRefTreeLevel@PartitionApfsWriter@sak@@",
    "?readGeneratedSpacemanCibArrayEntry@PartitionApfsWriter@sak@@",
    "?readGeneratedIpFreeQueueGhosts@PartitionApfsWriter@sak@@",
    "?nextCrashSafeIpSlot@PartitionApfsWriter@sak@@",
    "?buildFsTreeNodeBlocks@PartitionApfsWriter@sak@@",
    "?buildObjectMapTreeBlocks@PartitionApfsWriter@sak@@",
    "?buildMainFreeQueueTreeBlocks@PartitionApfsWriter@sak@@",
    "?readMainFreeQueueTreeBlocks@PartitionApfsWriter@sak@@",
    "?validateImageOnlyExecutionEvidence@PartitionApfsWriter@sak@@",
    "?enterpriseCertificationRequirements@PartitionApfsWriter@sak@@",
    $escapeHatch
)
$coreHooks = @(
    "ForTesting@PartitionExecutor@sak@@",
    $escapeHatch
)

# Every file is scanned before failing, so one report names every hook that came back.
$allFailures = @()
$allFailures += Assert-Scan -Path $PartitionFsLibrary `
    -MustContain @(
        "?acceptsRawDeviceTargetPath@PartitionApfsWriter@sak@@",
        "?preflightExistingContainer@PartitionApfsWriter@sak@@",
        "?listDirectoryFromImage@PartitionApfsFileSystemReader@sak@@") `
    -MustNotContain $partitionFsHooks

$allFailures += Assert-Scan -Path $CoreLibrary `
    -MustContain @(
        "?execute@PartitionExecutor@sak@@",
        "?resolveRuntimeManifestPath@PartitionFileSystemToolManifest@sak@@") `
    -MustNotContain $coreHooks

$allFailures += Assert-Scan -Path $ApplicationExe `
    -MustContain @("tools/filesystem/manifest.json") `
    -MustNotContain @($escapeHatch)

if ($allFailures.Count -gt 0) {
    throw ("Release binaries carry test hooks or escape hatches:`n  " + ($allFailures -join "`n  "))
}
Write-Host "Release binaries carry no partition/filesystem test hooks and no SAK_DEV_SOURCE_TREE"
