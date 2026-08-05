param(
    [Parameter(Mandatory=$true)]
    [string]$PackageRoot,

    [string]$RepoRoot = ""
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "Portable release smoke failed: $Message"
}

$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$required = @(
    'sak_utility.exe',
    'sak_elevated_helper.exe',
    'sak_apfs_writer_cli.exe',
    'sak_hfs_writer_cli.exe',
    'Qt6Core.dll',
    'Qt6Widgets.dll',
    'platforms/qwindows.dll',
    'tools/filesystem/manifest.json',
    'data/ai/providers/providers.json',
    'data/ai/app_manifests/windows_defender.json',
    'data/ai/app_manifests/superantispyware.json',
    'data/ai/app_manifests/windows_sfc.json'
)

foreach ($rel in $required) {
    $path = Join-Path $root $rel
    if (!(Test-Path -LiteralPath $path)) {
        Fail "missing required portable file: $rel"
    }
}

if (Get-ChildItem -LiteralPath $root -Recurse -Filter '*.local.json' -ErrorAction SilentlyContinue) {
    Fail 'local provider/app config leaked into package'
}
if (Test-Path -LiteralPath (Join-Path $root 'tools/mcp/_build')) {
    Fail 'tools/mcp/_build leaked into package'
}
if (Test-Path -LiteralPath (Join-Path $root 'tools/filesystem/_build')) {
    Fail 'tools/filesystem/_build leaked into package'
}
foreach ($rel in @('data/ai_sessions', 'data/temp', 'data/logs', 'data/config', '_logs')) {
    if (Test-Path -LiteralPath (Join-Path $root $rel)) {
        Fail "mutable runtime data leaked into package: $rel"
    }
}
foreach ($rel in @('tools/chocolatey/lib-bad', 'tools/chocolatey/cache', 'tools/chocolatey/temp')) {
    if (Test-Path -LiteralPath (Join-Path $root $rel)) {
        Fail "Chocolatey mutable runtime state leaked into package: $rel"
    }
}

$providersPath = Join-Path $root 'data/ai/providers/providers.json'
$providers = Get-Content -LiteralPath $providersPath -Raw | ConvertFrom-Json
foreach ($provider in @($providers.providers)) {
    if ($provider.transport -eq 'stdio') {
        if ([System.IO.Path]::IsPathRooted([string]$provider.command)) {
            Fail "provider '$($provider.id)' uses absolute command path: $($provider.command)"
        }
        $command = Join-Path $root ([string]$provider.command)
        if (!(Test-Path -LiteralPath $command)) {
            Fail "provider '$($provider.id)' command missing: $($provider.command)"
        }
    }
    foreach ($bad in @('api_key', 'auth_token')) {
        if ($provider.PSObject.Properties.Name -contains $bad) {
            Fail "provider '$($provider.id)' contains forbidden secret field: $bad"
        }
    }
}

if ($RepoRoot) {
    $repo = (Resolve-Path -LiteralPath $RepoRoot).Path
    $textExtensions = @('.json', '.txt', '.md', '.ini', '.ps1')
    $textFiles = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $textExtensions -contains $_.Extension.ToLowerInvariant() }
    foreach ($file in $textFiles) {
        $content = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
        if ($content -and $content.Contains($repo)) {
            Fail "dev repo path found in packaged text file: $($file.FullName.Substring($root.Length + 1))"
        }
    }
}

# Runtime dependency verification. This used to invoke the verifier and DISCARD its exit
# code, then print "smoke passed" regardless - so a package missing a required DLL, i.e.
# one that cannot start on a clean machine, passed the very check that exists to catch
# that. It also treated an absent verifier as success. Both are fail-open, and both are
# refused here: a check that did not run is not a check that passed.
if (-not $RepoRoot) {
    Write-Host "FAIL: -RepoRoot is required to locate the runtime dependency verifier." -ForegroundColor Red
    exit 1
}
$runtimeVerifier = Join-Path (Resolve-Path -LiteralPath $RepoRoot).Path `
    'scripts/verify_windows_runtime_dependencies.ps1'
if (-not (Test-Path -LiteralPath $runtimeVerifier)) {
    Write-Host "FAIL: runtime dependency verifier not found at $runtimeVerifier" -ForegroundColor Red
    exit 1
}

# Child process: a .ps1 invoked with the call operator only sets $LASTEXITCODE when it
# calls exit, so an in-process call cannot distinguish "returned normally" from "left the
# last native tool's code lying around".
& pwsh -NoProfile -File $runtimeVerifier -RootDir $root -PrimaryExe 'sak_utility.exe'
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: runtime dependency verification failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit 1
}

Write-Host "Portable release smoke passed: $root"
exit 0
