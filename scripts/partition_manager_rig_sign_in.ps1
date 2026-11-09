<#
.SYNOPSIS
    Signs in to a certification VM through its keyboard, and proves the sign-in took.

.DESCRIPTION
    Dot-sourced by run_partition_manager_certification_rig.ps1. Windows can report that nobody is
    signed in before its sign-in screen is drawn, and keys typed then are lost. So typing is never
    taken as signing in: after each try this waits for Windows to name a signed-in user, tries
    again, and stops with "did not sign in" when it never does. The elevated task the rows run in
    needs a signed-in session, and without one it quietly runs nothing.

    The caller passes how to type the sign-in and how to read who is signed in, so a test can run
    this with no VM.
#>

function Invoke-RigSignIn {
    param(
        [Parameter(Mandatory = $true)] [string]$VmName,
        [Parameter(Mandatory = $true)] [scriptblock]$TypeSignIn,
        # Returns who Windows says is signed in, or nothing.
        [Parameter(Mandatory = $true)] [scriptblock]$SignedInUsers,
        [int]$Attempts = 4,
        [int]$WaitSeconds = 90,
        [int]$PollSeconds = 10
    )
    $users = "$(& $SignedInUsers)".Trim()
    if ($users) { return $users }
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        & $TypeSignIn | Out-Null
        $deadline = (Get-Date).AddSeconds($WaitSeconds)
        do {
            $users = "$(& $SignedInUsers)".Trim()
            if ($users) { return $users }
            if ((Get-Date) -lt $deadline) { Start-Sleep -Seconds $PollSeconds }
        } while ((Get-Date) -lt $deadline)
    }
    throw "$VmName did not sign in: $Attempts sign-ins were typed, and Windows reported no one signed in"
}
