<#
.SYNOPSIS
    Shared strict certification step and fresh-artifact guards.
.DESCRIPTION
    Both VHD and hardware wrappers use these guards. Native exit codes and
    PowerShell command status must agree, and generated outputs must come from
    the current step rather than an earlier successful run.
#>
function Invoke-CheckedStep {
    param(
        [Parameter(Mandatory = $true)] [scriptblock]$Body,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Workflow
    )
    $global:LASTEXITCODE = 0
    & $Body 2>&1 | ForEach-Object {
        # Scriptblock completion can reset $? after a nonterminating child error.
        # Preserve normal output, but never let an emitted PowerShell error become green.
        if ($_ -is [System.Management.Automation.ErrorRecord]) {
            throw [System.InvalidOperationException]::new(
                "Partition Manager $Workflow certification step failed: $Name (PowerShell error: $_)",
                $_.Exception)
        }
        $_
    }
    # A failed in-process script without an explicit exit can leave LASTEXITCODE at zero.
    $stepSucceeded = $?
    $exitCode = $LASTEXITCODE
    if (-not $stepSucceeded -or $exitCode -ne 0) {
        throw "Partition Manager $Workflow certification step failed: $Name (exit code $exitCode, succeeded=$stepSucceeded)"
    }
}

function Clear-GeneratedArtifact {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Generated certification artifact is not a direct file: $Path"
        }
        Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    if (Test-Path -LiteralPath $Path) {
        throw "Could not clear stale certification artifact before regenerating it: $Path"
    }
}

function Assert-GeneratedArtifact {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Workflow
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Partition Manager $Workflow certification step '$Name' did not produce $Path"
    }
}

function Invoke-GeneratedArtifactStep {
    param(
        [Parameter(Mandatory = $true)] [scriptblock]$Body,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Workflow,
        [Parameter(Mandatory = $true)] [string[]]$Paths
    )
    if ($Paths.Count -eq 0) { throw "Generated certification step requires output paths" }
    foreach ($path in $Paths) { Clear-GeneratedArtifact -Path $path }
    Invoke-CheckedStep -Body $Body -Name $Name -Workflow $Workflow
    foreach ($path in $Paths) { Assert-GeneratedArtifact -Path $path -Name $Name -Workflow $Workflow }
}
