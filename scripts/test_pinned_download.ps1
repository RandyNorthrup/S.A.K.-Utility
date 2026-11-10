# What the CI toolchain's pinned download (.github/actions/windows-build-env/pinned_download.ps1) retries,
# what it refuses, and when it uses a cached copy. Invoke-WebRequest is replaced by a function that answers
# from a script, so no network is used and the waits are zero.
param()

$ErrorActionPreference = 'Stop'

$helper = Join-Path $PSScriptRoot '..\.github\actions\windows-build-env\pinned_download.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("sak-pinned-download-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAILED: $Message" }
}

$payload = [Text.Encoding]::ASCII.GetBytes('pinned tool archive')
$pin = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($payload)).Replace('-', '').ToLowerInvariant()
# The stand-in runs inside the helper's script scope, so it reads this table through the call chain.
$state = @{ Answers = @(); Calls = 0 }

# Stands in for the cmdlet: each call takes the next answer, an HTTP status to fail with, 'timeout', or 'ok'
# (writes the payload), or 'wrong' (writes other bytes).
function Invoke-WebRequest {
    param([string]$Uri, [string]$OutFile, [int]$TimeoutSec)
    $answer = $state.Answers[$state.Calls]
    $state.Calls++
    switch ($answer) {
        'ok' { [IO.File]::WriteAllBytes($OutFile, $payload); return }
        'wrong' { [IO.File]::WriteAllBytes($OutFile, [Text.Encoding]::ASCII.GetBytes('something else')); return }
        'timeout' { throw [System.Threading.Tasks.TaskCanceledException]::new('The request was canceled due to the configured HttpClient.Timeout') }
        default {
            $response = [System.Net.Http.HttpResponseMessage]::new([System.Net.HttpStatusCode][int]$answer)
            throw [Microsoft.PowerShell.Commands.HttpResponseException]::new("Response status code $answer", $response)
        }
    }
}

function Invoke-Helper([string[]]$Answers, [string]$Directory) {
    $state.Answers = $Answers
    $state.Calls = 0
    $result = [pscustomobject]@{ Path = $null; Error = $null }
    try {
        $result.Path = & $helper -Url 'https://example.invalid/tool.zip' -Sha256 $pin -Directory $Directory `
            -Name 'tool.zip' -BackoffSeconds @(0, 0, 0) 6>$null
    } catch {
        $result.Error = $_.Exception.Message
    }
    return $result
}

try {
    # A 504 and a timeout are an outage: tried again, and the third try is used.
    $dir = Join-Path $work 'retry'
    $r = Invoke-Helper @('504', 'timeout', 'ok') $dir
    Assert-True ($null -eq $r.Error) "an outage then a good download failed: $($r.Error)"
    Assert-True ($state.Calls -eq 3) "expected 3 tries, made $($state.Calls)"
    Assert-True ((Get-FileHash -LiteralPath $r.Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $pin) 'the file is not the pinned one'

    # Four tries in all, then the outage is the error.
    $r = Invoke-Helper @('503', '502', '504', '500', 'ok') (Join-Path $work 'outage')
    Assert-True ($null -ne $r.Error -and $r.Error -match 'attempt 4 of 4') "a lasting outage was not refused after 4 tries: $($r.Error)"
    Assert-True ($state.Calls -eq 4) "expected 4 tries, made $($state.Calls)"
    Assert-True (-not (Test-Path (Join-Path $work 'outage\tool.zip'))) 'a failed download left a file behind'

    # A 404 is not an outage: refused at once.
    $r = Invoke-Helper @('404', 'ok') (Join-Path $work 'missing')
    Assert-True ($null -ne $r.Error -and $state.Calls -eq 1) "a 404 was tried again ($($state.Calls) tries): $($r.Error)"

    # A file that is not the pin is the wrong file, not an outage: refused at once, and removed.
    $r = Invoke-Helper @('wrong', 'ok') (Join-Path $work 'tampered')
    Assert-True ($null -ne $r.Error -and $r.Error -match 'does not match the pin') "a wrong file was not refused: $($r.Error)"
    Assert-True ($state.Calls -eq 1) "a wrong file was tried again ($($state.Calls) tries)"
    Assert-True (-not (Test-Path (Join-Path $work 'tampered\tool.zip'))) 'the wrong file was left in the cache folder'

    # A cached copy that is the pin is used without a download.
    $r = Invoke-Helper @('500') $dir
    Assert-True ($null -eq $r.Error -and $state.Calls -eq 0) "the cached pinned copy was not used ($($state.Calls) downloads): $($r.Error)"

    # A cached copy that is not the pin is replaced.
    [IO.File]::WriteAllBytes((Join-Path $dir 'tool.zip'), [Text.Encoding]::ASCII.GetBytes('stale'))
    $r = Invoke-Helper @('ok') $dir
    Assert-True ($null -eq $r.Error -and $state.Calls -eq 1) "a stale cached copy was not replaced: $($r.Error)"
    Assert-True ((Get-FileHash -LiteralPath $r.Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $pin) 'the replaced file is not the pinned one'

    'pinned download: retries outages, refuses 4xx and wrong files, uses only a pinned cached copy'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
