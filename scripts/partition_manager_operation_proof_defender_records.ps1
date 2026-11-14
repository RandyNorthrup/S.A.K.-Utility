<#
.SYNOPSIS
    What Windows recorded about a failed DISM capture in the VirtualBox guest, read before the VM
    goes back to its snapshot: DISM's error lines, Defender's detections, threats and events, and
    the shadow-copy deletions. Read-only; runs elevated in the guest through the rig's GuestJob.

.DESCRIPTION
    On 2026-11-12 the BIOS install's capture and the Image-route migration stopped with DISM exit
    225 ("the file contains a virus or potentially unwanted software"), and the records went with
    the snapshot the run restores afterwards.
#>
$ErrorActionPreference = 'Continue'
function Format-Event($Event, [int]$Length) {
    $text = $Event.Message -replace '\s+', ' '
    "$($Event.TimeCreated.ToString('o')) $($Event.Id) $($text.Substring(0, [Math]::Min($Length, $text.Length)))"
}
"--- DISM error lines (last 40)"
Get-Content -LiteralPath "$env:SystemRoot\Logs\DISM\dism.log" -Tail 6000 -ErrorAction SilentlyContinue |
    Where-Object { $_ -match ', Error |virus|unwanted|0x800700e1|0x800701b1' } | Select-Object -Last 40
"--- Defender detections"
Get-MpThreatDetection -ErrorAction SilentlyContinue | Sort-Object InitialDetectionTime -Descending | Select-Object -First 15 |
    ForEach-Object { "$($_.InitialDetectionTime.ToString('o')) id=$($_.ThreatID) process=$($_.ProcessName) resources=$(@($_.Resources) -join ';')" }
"--- Defender threats"
Get-MpThreat -ErrorAction SilentlyContinue | ForEach-Object { "id=$($_.ThreatID) name=$($_.ThreatName) severity=$($_.SeverityID)" }
"--- Defender events (detections, actions, scans), last 6 hours"
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; StartTime = (Get-Date).AddHours(-6) } -ErrorAction SilentlyContinue |
    Where-Object { $_.Id -in 1006, 1007, 1008, 1015, 1116, 1117, 1118, 1119 } | Select-Object -First 20 | ForEach-Object { Format-Event $_ 1200 }
"--- volsnap: shadow copies deleted or aborted, last 6 hours"
Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'volsnap'; StartTime = (Get-Date).AddHours(-6) } -ErrorAction SilentlyContinue |
    Select-Object -First 20 | ForEach-Object { Format-Event $_ 300 }
"--- Windows Update installs, last 6 hours"
Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WindowsUpdateClient'; StartTime = (Get-Date).AddHours(-6) } -ErrorAction SilentlyContinue |
    Select-Object -First 10 | ForEach-Object { Format-Event $_ 200 }
