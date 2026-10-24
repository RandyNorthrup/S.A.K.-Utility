<#
.SYNOPSIS
    Selects Partition Manager certification evidence by recorded run time.
.DESCRIPTION
    A completed_utc value takes precedence. Reports without completion may use started_utc.
    An unreadable report or timestamp refuses selection; filesystem modification time is never
    evidence of when a certification run happened. Callers retain their own report validation.
    Reformatting, checking out, or repointing an old report changes its mtime without rerunning
    certification, so mtime ordering can silently promote stale evidence over a newer run.
#>

function ConvertTo-CertificationRunTime {
    param([AllowNull()] [object]$Value)

    # PowerShell 7 converts ISO JSON values into DateTime, while Windows PowerShell 5.1 leaves
    # strings. Preserve a parsed value's zone: converting it to display text first loses Kind.
    if ($Value -is [datetimeoffset]) { return $Value.UtcDateTime }
    if ($Value -is [datetime]) {
        if ($Value.Kind -eq [DateTimeKind]::Unspecified) {
            return [datetime]::SpecifyKind($Value, [DateTimeKind]::Utc)
        }
        return $Value.ToUniversalTime()
    }
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value)) {
        throw "Recorded certification time is missing or is not a timestamp"
    }
    return [datetime]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::AdjustToUniversal -bor
        [Globalization.DateTimeStyles]::AssumeUniversal)
}

function Resolve-LatestCertificationReport {
    param([Parameter(Mandatory = $true)] [string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) {
        throw "CertificationRoot must name a report directory"
    }
    $resolvedRoot = Resolve-Path -LiteralPath $Root -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolvedRoot.Path -PathType Container)) {
        throw "CertificationRoot must name a report directory"
    }
    $reports = @(Get-ChildItem -LiteralPath $resolvedRoot.Path -Recurse -File `
        -Filter "partition-manager-certification-report.json" -ErrorAction Stop)
    if ($reports.Count -eq 0) {
        throw "No Partition Manager certification reports found under $($resolvedRoot.Path)"
    }
    $dated = foreach ($report in $reports) {
        try {
            $parsed = Get-Content -LiteralPath $report.FullName -Raw -ErrorAction Stop |
                ConvertFrom-Json -ErrorAction Stop
            $completed = $parsed.PSObject.Properties["completed_utc"]
            $started = $parsed.PSObject.Properties["started_utc"]
            $recorded = if ($null -ne $completed -and $null -ne $completed.Value -and
                ($completed.Value -isnot [string] -or $completed.Value.Length -gt 0)) {
                $completed.Value
            } elseif ($null -ne $started) {
                $started.Value
            } else {
                $null
            }
            $stamp = ConvertTo-CertificationRunTime -Value $recorded
        } catch {
            throw ("Certification report cannot be ordered, it has no readable completed_utc/started_utc: " +
                $report.FullName)
        }
        [pscustomobject]@{ Path = $report.FullName; Stamp = $stamp }
    }
    return (@($dated | Sort-Object Stamp -Descending))[0].Path
}
