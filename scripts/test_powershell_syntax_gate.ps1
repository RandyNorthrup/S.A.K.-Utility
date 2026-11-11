<#
.SYNOPSIS
    check_powershell_syntax.ps1 refuses what it exists to refuse, and passes what is fine.

.DESCRIPTION
    Each fixture is a small script written to a temporary folder and checked on its own, in a new
    process of the PowerShell running this test (the gate ends with exit).

    - $LASTEXITCODE read after a native command's pipeline is cut short by Select-Object -First or
      -Index: PowerShell 7 does not record that exit code, and bundle_smartmontools.ps1's check of a
      pinned smartctl.exe failed on every fresh CI session because of it (2026-11-10). Refused with a
      call operator, a bare tool name, the select alias and -Index; passed when the output is captured
      first, when the pipeline is all cmdlets, when another native command runs before the read, and
      when the read is in another function.
    - One variable written under two casings in one scope ($Chains and $chains, 2026-11-09). Refused,
      and passed when each function keeps its own.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$gate = (Resolve-Path (Join-Path $PSScriptRoot 'check_powershell_syntax.ps1')).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ('sak-powershell-syntax-gate-' + [guid]::NewGuid().ToString('N'))
$hostPath = (Get-Process -Id $PID).Path

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAILED: $Message" }
}

function Invoke-Gate([string]$Name, [string]$Text) {
    $path = Join-Path $work "$Name.ps1"
    [IO.File]::WriteAllText($path, $Text, [Text.UTF8Encoding]::new($false))
    $output = @(& $hostPath -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $gate $path 2>&1 | ForEach-Object { "$_" })
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Text = ($output -join "`n") }
}

$refused = [ordered]@{
    'native-call-operator-first' = @'
$exe = 'C:\tool.exe'
$line = & $exe --version 2>&1 | Select-String 'tool' | Select-Object -First 1
if ($LASTEXITCODE -ne 0) { throw 'failed' }
'@
    'bare-tool-name-select-alias' = @'
$line = git --version | select -first 1
"exit $LASTEXITCODE"
'@
    'executable-name' = @'
$line = cmd.exe /d /c ver | Select-Object -First 1
if ($LASTEXITCODE -ne 0) { throw 'failed' }
'@
    'index-parameter' = @'
function Get-ToolVersion([string]$Path) {
    $line = & $Path --version | Select-Object -Index 0
    return @{ Line = $line; Exit = $LASTEXITCODE }
}
'@
}
$passed = [ordered]@{
    'output-captured-first' = @'
$exe = 'C:\tool.exe'
$all = @(& $exe --version 2>&1)
$code = $LASTEXITCODE
$line = $all | Select-String 'tool' | Select-Object -First 1
'@
    'cmdlet-pipeline' = @'
$first = Get-ChildItem -LiteralPath $env:TEMP | Select-Object -First 1
"exit $LASTEXITCODE"
'@
    'another-native-command-first' = @'
$exe = 'C:\tool.exe'
$line = & $exe --version | Select-Object -First 1
& $exe --check
if ($LASTEXITCODE -ne 0) { throw 'failed' }
'@
    'read-in-another-function' = @'
function Get-ToolLine([string]$Path) { return (& $Path --version | Select-Object -First 1) }
function Get-LastCode { return $LASTEXITCODE }
'@
    'one-casing-per-function' = @'
function First { $Chains = 1; return $Chains }
function Second { $chains = 2; return $chains }
'@
}
$casingRefused = @'
$Chains = 'chains.ps1'
$chains = @('one', 'two')
'@

New-Item -ItemType Directory -Path $work | Out-Null
try {
    foreach ($name in $refused.Keys) {
        $result = Invoke-Gate $name $refused[$name]
        Assert-True ($result.ExitCode -ne 0) "the gate refuses $name`n$($result.Text)"
        Assert-True ($result.Text -match 'PowerShell 7 does not record that exit') "the refusal of $name names the cause`n$($result.Text)"
    }
    $result = Invoke-Gate 'two-casings' $casingRefused
    Assert-True ($result.ExitCode -ne 0) "the gate refuses one variable under two casings`n$($result.Text)"
    Assert-True ($result.Text -match 'PowerShell names ignore case') "the casing refusal names the cause`n$($result.Text)"
    foreach ($name in $passed.Keys) {
        $result = Invoke-Gate $name $passed[$name]
        Assert-True ($result.ExitCode -eq 0) "the gate passes $name`n$($result.Text)"
    }
    Write-Host ("PASS check_powershell_syntax.ps1 ($($PSVersionTable.PSEdition) $($PSVersionTable.PSVersion)): " +
        "$($refused.Count + 1) refused, $($passed.Count) passed")
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
