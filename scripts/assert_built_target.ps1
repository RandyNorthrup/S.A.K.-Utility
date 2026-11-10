<#
.SYNOPSIS
    Fail-closed check that a test is about to exercise the binary the configured build just
    produced, not a missing, misplaced or stale one.

.DESCRIPTION
    Dot-source this file and call Assert-FreshBuiltTarget. It refuses:

      - an empty path, or a path to nothing;
      - a target that is not inside the release directory the caller named (when one is given);
      - a target OLDER than any product library beside it (sak_*.lib). The executable links every
        one of those, so a library newer than the executable means the last build rebuilt the
        library and then did not relink -- the "Build FAILED, ctest runs the previous .exe" trap
        (AGENTS.md, "Ways a green run means nothing");
      - a directory with no product library at all, since freshness then cannot be shown.

    Tests used to hard-code build\Release\sak_utility.exe, so a tree configured anywhere else
    either failed with "input missing" or, worse, tested whatever old binary sat there.
#>

function Assert-FreshBuiltTarget {
    [CmdletBinding(PositionalBinding = $false)]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Path,
        [AllowEmptyString()][string]$ReleaseDir = ""
    )
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Built target path is empty; the CTest registration must pass `$<TARGET_FILE:...>"
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Built target is missing: $Path"
    }
    $target = Get-Item -LiteralPath $Path
    $targetDir = $target.Directory.FullName.TrimEnd('\')
    if (-not [string]::IsNullOrWhiteSpace($ReleaseDir)) {
        if (-not (Test-Path -LiteralPath $ReleaseDir -PathType Container)) {
            throw "Release directory is missing: $ReleaseDir"
        }
        $release = (Get-Item -LiteralPath $ReleaseDir).FullName.TrimEnd('\')
        if ($targetDir -ine $release) {
            throw "Built target $($target.FullName) is not in the release directory $release"
        }
    }
    $libraries = @(Get-ChildItem -LiteralPath $targetDir -Filter 'sak_*.lib' -File -ErrorAction Stop)
    if ($libraries.Count -eq 0) {
        throw "No product library beside $($target.FullName); cannot show the target is fresh"
    }
    $newest = $libraries | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newest.LastWriteTimeUtc -gt $target.LastWriteTimeUtc) {
        throw ("Built target is stale: $($target.Name) ($($target.LastWriteTimeUtc.ToString('o'))) " +
            "is older than $($newest.Name) ($($newest.LastWriteTimeUtc.ToString('o'))); " +
            "the last build did not relink it")
    }
    return $target.FullName
}
