<#
.SYNOPSIS
    Fetches one pinned CI download and prints the path of a file whose SHA-256 is the pin.

.DESCRIPTION
    A copy already in -Directory (restored by actions/cache, keyed by the pin) is used when its
    SHA-256 is the pin; one that is not is deleted and downloaded again. A download is tried up to
    four times, waiting 10, 30 and 60 s between tries, when the server answers with an HTTP 5xx or
    does not answer at all (a timeout or a dropped connection): on 2026-11-08 GitHub's release
    downloads answered "504 Gateway Time-out" for a while, and every T2 shard failed in its
    Toolchain step before building anything. Any other HTTP error fails at once. Every downloaded
    copy is hashed before it is used, and a hash that is not the pin fails at once: that is the
    wrong file, not an outage, so it is never retried.

.EXAMPLE
    $zip = & "$env:GITHUB_ACTION_PATH/pinned_download.ps1" -Url $url -Sha256 $env:RG_SHA256 `
        -Directory (Join-Path $env:RUNNER_TEMP 'pinned-downloads/ripgrep') -Name 'ripgrep.zip'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Url,
    [Parameter(Mandatory)] [string]$Sha256,
    [Parameter(Mandatory)] [string]$Directory,
    [Parameter(Mandatory)] [string]$Name,
    [int[]]$BackoffSeconds = @(10, 30, 60),
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
$pin = $Sha256.ToLowerInvariant()
if ($pin -notmatch '^[0-9a-f]{64}$') { throw "$Name pin '$Sha256' is not a SHA-256" }
New-Item -ItemType Directory -Force -Path $Directory | Out-Null
$file = Join-Path $Directory $Name

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

if (Test-Path -LiteralPath $file -PathType Leaf) {
    if ((Get-Sha256 $file) -eq $pin) {
        Write-Host "${Name}: the cached copy is the pinned file"
        return $file
    }
    Write-Host "${Name}: the cached copy is not the pinned file; downloading it again"
    Remove-Item -LiteralPath $file -Force
}

$attempts = $BackoffSeconds.Count + 1
for ($attempt = 1; $attempt -le $attempts; $attempt++) {
    try {
        Invoke-WebRequest -Uri $Url -OutFile $file -TimeoutSec $TimeoutSeconds
    } catch {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        $status = $null
        if ($null -ne $_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        # No answer (a timeout or a dropped connection) or a server error is an outage; a 4xx is not.
        $outage = ($null -eq $status) -or ($status -ge 500)
        if (-not $outage -or $attempt -eq $attempts) {
            throw "${Name}: download failed on attempt $attempt of ${attempts}: $($_.Exception.Message)"
        }
        $wait = $BackoffSeconds[$attempt - 1]
        Write-Host "${Name}: attempt $attempt of $attempts failed ($($_.Exception.Message)); trying again in $wait s"
        Start-Sleep -Seconds $wait
        continue
    }
    $hash = Get-Sha256 $file
    if ($hash -ne $pin) {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        throw "$Name hash $hash does not match the pin"
    }
    Write-Host "${Name}: downloaded on attempt $attempt and matches the pin"
    return $file
}
