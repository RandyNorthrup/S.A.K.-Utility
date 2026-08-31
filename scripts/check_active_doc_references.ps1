<#
.SYNOPSIS
    Validates local references in tracked active documentation.

.DESCRIPTION
    Scans only tracked top-level docs/*.md files. Documents below docs/archive and
    docs/future are status buckets, not active work, so their contents are not scanned;
    active documents may still link to tracked files in either bucket.

    Local Markdown link targets resolve relative to the document containing the link.
    Backticked docs/*.md paths resolve from the repository root. Every local target must
    remain inside the repository and match a git-tracked path with exact case.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
Set-Location $ProjectRoot

function Get-TrackedPaths {
    $paths = @(& git -C $ProjectRoot ls-files)
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed while enumerating tracked documentation targets."
    }
    return @($paths | ForEach-Object { $_.Replace('\', '/') })
}

function Get-ReferenceTarget {
    param([System.Text.RegularExpressions.Match]$Match)

    if ($Match.Groups["angle"].Success) {
        return $Match.Groups["angle"].Value
    }
    return $Match.Groups["plain"].Value
}

function Test-ExternalOrFragmentTarget {
    param([string]$Target)

    return $Target.StartsWith("#", [System.StringComparison]::Ordinal) -or
        $Target.StartsWith("//", [System.StringComparison]::Ordinal) -or
        $Target -match '^[A-Za-z][A-Za-z0-9+.-]*:'
}

function Get-ResolvedReference {
    param(
        [string]$SourceRelative,
        [string]$Target,
        [bool]$RepositoryRelative
    )

    $cleanTarget = $Target.Trim()
    if ($cleanTarget.Length -ge 2 -and $cleanTarget[0] -eq '<' -and
        $cleanTarget[$cleanTarget.Length - 1] -eq '>') {
        $cleanTarget = $cleanTarget.Substring(1, $cleanTarget.Length - 2)
    }
    $cleanTarget = $cleanTarget -replace '\\([\\`*{}\[\]()#+.!_-])', '$1'
    $cleanTarget = [System.Uri]::UnescapeDataString($cleanTarget)
    $cleanTarget = ($cleanTarget -split '[?#]', 2)[0]

    if ([string]::IsNullOrWhiteSpace($cleanTarget)) {
        return $null
    }
    if ($cleanTarget.StartsWith("/", [System.StringComparison]::Ordinal) -or
        $cleanTarget.StartsWith("\", [System.StringComparison]::Ordinal) -or
        $cleanTarget -match '^[A-Za-z]:') {
        return [pscustomobject]@{ Error = "absolute path escapes repository"; Relative = $cleanTarget }
    }

    $basePath = $ProjectRoot
    if (-not $RepositoryRelative) {
        $sourcePath = Join-Path $ProjectRoot ($SourceRelative.Replace('/', [IO.Path]::DirectorySeparatorChar))
        $basePath = Split-Path -Parent $sourcePath
    }
    $nativeTarget = $cleanTarget.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $fullPath = [IO.Path]::GetFullPath((Join-Path $basePath $nativeTarget))
    $rootWithSeparator = $ProjectRoot.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $pathComparison = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    if (-not $fullPath.StartsWith($rootWithSeparator, $pathComparison)) {
        return [pscustomobject]@{ Error = "path escapes repository"; Relative = $fullPath }
    }
    $relative = $fullPath.Substring($rootWithSeparator.Length).Replace('\', '/')
    return [pscustomobject]@{ Error = ""; Relative = $relative }
}

$trackedPaths = @(Get-TrackedPaths)
$trackedExact = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
$trackedCaseFolded = @{}
foreach ($path in $trackedPaths) {
    [void]$trackedExact.Add($path)
    $folded = $path.ToLowerInvariant()
    if (-not $trackedCaseFolded.ContainsKey($folded)) {
        $trackedCaseFolded[$folded] = $path
    }
}

$activeDocuments = @($trackedPaths | Where-Object { $_ -match '^docs/[^/]+\.md$' } | Sort-Object)
if ($activeDocuments.Count -eq 0) {
    throw "Active-document reference gate found zero tracked top-level docs/*.md files."
}

$markdownLinkPattern = [regex]'!?\[[^\]]*\]\(\s*(?:<(?<angle>[^>]+)>|(?<plain>[^\s)]+))(?:\s+(?:"[^"]*"|''[^'']*''|\([^)]*\)))?\s*\)'
$referenceDefinitionPattern = [regex]'^\s{0,3}\[[^\]]+\]:\s*(?:<(?<angle>[^>]+)>|(?<plain>\S+))'
$codeSpanPattern = [regex]'`(?<code>[^`\r\n]+)`'
$backtickedDocPattern = [regex]'^docs/[A-Za-z0-9_. /-]+\.md$'
$fencePattern = [regex]'^\s{0,3}(?<marker>`{3,}|~{3,})'
$errors = [System.Collections.Generic.List[string]]::new()
$referenceCount = 0

function Test-LocalReference {
    param(
        [string]$SourceRelative,
        [int]$LineNumber,
        [string]$Target,
        [bool]$RepositoryRelative
    )

    if ([string]::IsNullOrWhiteSpace($Target) -or
        (Test-ExternalOrFragmentTarget -Target $Target.Trim())) {
        return
    }

    $script:referenceCount++
    try {
        $resolved = Get-ResolvedReference -SourceRelative $SourceRelative -Target $Target `
            -RepositoryRelative $RepositoryRelative
    } catch {
        $errors.Add("${SourceRelative}:${LineNumber}: invalid target '$Target': $($_.Exception.Message)")
        return
    }
    if ($null -eq $resolved) {
        return
    }
    if (-not [string]::IsNullOrWhiteSpace($resolved.Error)) {
        $errors.Add("${SourceRelative}:${LineNumber}: $($resolved.Error): '$Target'")
        return
    }

    $relative = [string]$resolved.Relative
    if ($trackedExact.Contains($relative)) {
        return
    }
    $folded = $relative.ToLowerInvariant()
    if ($trackedCaseFolded.ContainsKey($folded)) {
        $errors.Add("${SourceRelative}:${LineNumber}: case mismatch '$Target' resolves to " +
            "'$relative'; tracked path is '$($trackedCaseFolded[$folded])'")
        return
    }

    $fullPath = Join-Path $ProjectRoot ($relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
    if (Test-Path -LiteralPath $fullPath) {
        $errors.Add("${SourceRelative}:${LineNumber}: untracked local target '$Target' resolves to '$relative'")
    } else {
        $errors.Add("${SourceRelative}:${LineNumber}: missing local target '$Target' resolves to '$relative'")
    }
}

foreach ($document in $activeDocuments) {
    $documentPath = Join-Path $ProjectRoot ($document.Replace('/', [IO.Path]::DirectorySeparatorChar))
    $lines = @(Get-Content -LiteralPath $documentPath)
    $inFence = $false
    $fenceCharacter = ''
    $fenceLength = 0

    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = [string]$lines[$index]
        $lineNumber = $index + 1
        $fence = $fencePattern.Match($line)
        if ($fence.Success) {
            $marker = $fence.Groups["marker"].Value
            $markerCharacter = $marker[0]
            if (-not $inFence) {
                $inFence = $true
                $fenceCharacter = $markerCharacter
                $fenceLength = $marker.Length
            } elseif ($markerCharacter -eq $fenceCharacter -and $marker.Length -ge $fenceLength) {
                $inFence = $false
                $fenceCharacter = ''
                $fenceLength = 0
            }
            continue
        }
        if ($inFence) {
            continue
        }

        foreach ($match in $markdownLinkPattern.Matches($line)) {
            $target = Get-ReferenceTarget -Match $match
            Test-LocalReference -SourceRelative $document -LineNumber $lineNumber `
                -Target $target -RepositoryRelative $false
        }
        $definition = $referenceDefinitionPattern.Match($line)
        if ($definition.Success) {
            $target = Get-ReferenceTarget -Match $definition
            Test-LocalReference -SourceRelative $document -LineNumber $lineNumber `
                -Target $target -RepositoryRelative $false
        }
        foreach ($match in $codeSpanPattern.Matches($line)) {
            $target = $match.Groups["code"].Value.Trim()
            if ($backtickedDocPattern.IsMatch($target)) {
                Test-LocalReference -SourceRelative $document -LineNumber $lineNumber `
                    -Target $target -RepositoryRelative $true
            }
        }
    }
}

if ($referenceCount -eq 0) {
    throw "Active-document reference gate found zero local references in $($activeDocuments.Count) documents."
}
if ($errors.Count -gt 0) {
    Write-Host "Active-document reference gate failed with $($errors.Count) broken reference(s):"
    foreach ($errorMessage in @($errors | Sort-Object -Unique)) {
        Write-Host "  $errorMessage"
    }
    exit 1
}

Write-Host ("Active-document references passed: {0} documents, {1} local references." -f `
    $activeDocuments.Count, $referenceCount)
exit 0
