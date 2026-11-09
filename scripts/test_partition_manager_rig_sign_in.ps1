<#
.SYNOPSIS
    Proves the certification rig takes a sign-in as done only when Windows says someone is signed in.

.DESCRIPTION
    Runs scripts/partition_manager_rig_sign_in.ps1 with stand-ins for typing and for reading who is
    signed in. No VM, no Mac and no network: the final run of 2026-11-08 stopped because one typed
    sign-in did not land and nothing checked, so this checks that something does.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'partition_manager_rig_sign_in.ps1')

$Script:Failures = New-Object System.Collections.Generic.List[string]
$Script:Passes = 0

function Assert-That {
    param([bool]$Condition, [string]$Name, [string]$Detail = '')
    if ($Condition) { $Script:Passes++ } else { $Script:Failures.Add("$Name $Detail") }
}

# A sign-in that never lands stops it, by name, after every try.
$Script:Typed = 0
$why = ''
try {
    Invoke-RigSignIn -VmName 'sakcert-test' -TypeSignIn { $Script:Typed++ } -SignedInUsers { '' } -Attempts 3 -WaitSeconds 0 -PollSeconds 0 | Out-Null
} catch {
    $why = "$($_.Exception.Message)"
}
Assert-That ($why -match 'sakcert-test did not sign in') 'A sign-in that never lands stops as "did not sign in"' $why
Assert-That ($Script:Typed -eq 3) 'and it was tried every time' "typed $Script:Typed"

# One that lands on a later try is signed in, and is not typed again.
$Script:Typed = 0
$users = Invoke-RigSignIn -VmName 'sakcert-test' -TypeSignIn { $Script:Typed++ } -SignedInUsers { if ($Script:Typed -ge 2) { 'Technician' } } -Attempts 4 -WaitSeconds 0 -PollSeconds 0
Assert-That ($users -ceq 'Technician') 'A sign-in that lands on the second try is signed in' $users
Assert-That ($Script:Typed -eq 2) 'and is typed no more' "typed $Script:Typed"

# A Windows that signed itself in is not typed into.
$Script:Typed = 0
$users = Invoke-RigSignIn -VmName 'sakcert-test' -TypeSignIn { $Script:Typed++ } -SignedInUsers { 'Technician' } -WaitSeconds 0 -PollSeconds 0
Assert-That ($users -ceq 'Technician' -and $Script:Typed -eq 0) 'Someone already signed in means nothing is typed' "typed $Script:Typed"

# It waits for Windows to say so, rather than taking the typing as the answer.
$Script:Typed = 0
$Script:Reads = 0
$users = Invoke-RigSignIn -VmName 'sakcert-test' -TypeSignIn { $Script:Typed++ } -SignedInUsers { $Script:Reads++; if ($Script:Reads -ge 4) { 'Technician' } } -Attempts 1 -WaitSeconds 3 -PollSeconds 1
Assert-That ($users -ceq 'Technician' -and $Script:Typed -eq 1) 'One sign-in that Windows reports a little later is enough' "typed $Script:Typed, read $Script:Reads"

# Every place the rig signs in goes through it.
$rig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'run_partition_manager_certification_rig.ps1') -Raw
$typedDirectly = @([regex]::Matches($rig, 'Invoke-GuestLogin -VmName')).Count
Assert-That ($typedDirectly -eq 1) 'The rig types a sign-in only inside the routine that checks it' "found $typedDirectly"
$checked = @([regex]::Matches($rig, 'Invoke-RigVmSignIn -VmName')).Count
Assert-That ($checked -eq 4) 'The main VM, a boot proof, a kept proof VM and its facts all sign in through it' "found $checked"

if ($Script:Failures.Count -gt 0) {
    $Script:Failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "$($Script:Failures.Count) sign-in check(s) failed, $($Script:Passes) held"
}
Write-Host "Certification rig sign-in: $($Script:Passes) checks held."
