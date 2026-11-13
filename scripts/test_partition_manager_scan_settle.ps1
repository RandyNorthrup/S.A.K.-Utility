<#
.SYNOPSIS
    Proves that no script in scripts/ takes a disk down within 15 s of an online scan.

.DESCRIPTION
    volsnap.sys (Windows 11 26100) can leave a timer queued in a volume's device object after an
    online scan (Repair-Volume -Scan, chkdsk), and removing the whole disk within seconds
    (Dismount-DiskImage, Set-Disk -IsOffline, mountvol /P, a VirtualBox detach) frees the object
    under it: Windows bugchecks (operation proof's crash investigation, 2026-11-12). The
    certification readback scanned a volume and the rows then detached their VHDX, the pattern of
    those crashes.

    Every online scan in scripts/, code or the text of a job run elsewhere, must sit in a try whose
    finally calls Wait-RbVolumeScanSettle (partition_manager_readback.ps1), with nothing that takes
    a disk down between the scan and that finally. The readback's own scan and the shared wait then
    run against stand-ins, passing and throwing, and the rig's copy of the wait is run on its own.
    Nothing here touches a disk.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptsRoot = Split-Path -Parent $PSCommandPath

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )
    if (-not $Condition) { throw $Message }
}

# An online scan, as code or as text: Repair-Volume with -Scan, or chkdsk run (any arguments).
$ScanPattern = '(?i)\bRepair-Volume\b[^\r\n]*\s-Scan\b|(?i)(&\s*|^\s*|\$\(\s*|\|\s*)chkdsk(\.exe)?\b'
# What takes a volume or a disk down.
$TakeDownPattern = '(?i)Dismount-DiskImage|Dismount-VHD|-IsOffline\s+\$true|\bmountvol\b|--medium\s+none|\bcontrolvm\b|detach\s+vdisk|Remove-PartitionAccessPath'
# How many lines a scan's try may open before it, and how many lines its finally may come after.
$TryWindowLines = 3
$FinallyWindowLines = 6

function Get-CommentRanges {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    return @($tokens | Where-Object { $_.Kind -eq [System.Management.Automation.Language.TokenKind]::Comment } |
        ForEach-Object { , @($_.Extent.StartOffset, $_.Extent.EndOffset) })
}

function Get-ScanSettleProblems {
    <# Every online scan in one file, and what is wrong with the wait after it. #>
    param([Parameter(Mandatory = $true)] [string]$Path)
    $text = [IO.File]::ReadAllText($Path)
    $comments = Get-CommentRanges -Path $Path
    $lines = $text -split "`n"
    $lineStarts = New-Object System.Collections.Generic.List[int]
    $offset = 0
    foreach ($line in $lines) { $lineStarts.Add($offset); $offset += $line.Length + 1 }
    $problems = New-Object System.Collections.Generic.List[string]
    $scans = 0
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $match = [regex]::Match($lines[$index], $ScanPattern)
        if (-not $match.Success) { continue }
        $at = $lineStarts[$index] + $match.Index
        if (@($comments | Where-Object { $at -ge $_[0] -and $at -lt $_[1] }).Count -gt 0) { continue }
        $scans++
        $where = '{0}:{1}: {2}' -f (Split-Path -Leaf $Path), ($index + 1), $lines[$index].Trim()
        $tryFound = $false
        for ($back = $index; $back -ge [Math]::Max(0, $index - $TryWindowLines); $back--) {
            if ($lines[$back] -match '\btry\s*\{') { $tryFound = $true; break }
        }
        $finallyAt = -1
        for ($ahead = $index; $ahead -le [Math]::Min($lines.Count - 1, $index + $FinallyWindowLines); $ahead++) {
            if ($lines[$ahead] -match '\bfinally\s*\{[^\r\n]*\bWait-RbVolumeScanSettle\b' -or
                ($lines[$ahead] -match '\bfinally\s*\{\s*$' -and $ahead + 1 -lt $lines.Count -and $lines[$ahead + 1] -match '^\s*Wait-RbVolumeScanSettle\b')) {
                $finallyAt = $ahead
                break
            }
        }
        if (-not $tryFound) { $problems.Add("an online scan not inside a try: $where") }
        if ($finallyAt -lt 0) {
            $problems.Add("an online scan with no 'finally { Wait-RbVolumeScanSettle }' after it: $where")
            continue
        }
        for ($between = $index; $between -le $finallyAt; $between++) {
            if ($lines[$between] -match $TakeDownPattern) {
                $problems.Add(('a disk taken down before the scan settles: {0} (line {1})' -f $where, ($between + 1)))
            }
        }
    }
    return [pscustomobject]@{ Scans = $scans; Problems = @($problems) }
}

function Assert-EveryScanSettles {
    $self = [IO.Path]::GetFileName($PSCommandPath)
    $files = @(Get-ChildItem -LiteralPath $ScriptsRoot -Recurse -File -Include '*.ps1', '*.psm1' |
        Where-Object { $_.Name -ne $self })
    $problems = New-Object System.Collections.Generic.List[string]
    $scanned = New-Object System.Collections.Generic.List[string]
    foreach ($file in $files) {
        $result = Get-ScanSettleProblems -Path $file.FullName
        if ($result.Scans -gt 0) { $scanned.Add($file.Name) }
        foreach ($problem in $result.Problems) { $problems.Add($problem) }
    }
    if ($problems.Count -gt 0) { throw ($problems -join [Environment]::NewLine) }
    # The check has scans to read: the readback's and the rig's boot facts job.
    foreach ($expected in 'partition_manager_readback.ps1', 'run_partition_manager_certification_rig.ps1') {
        Assert-Condition -Condition ($scanned -contains $expected) -Message "no online scan found in $expected; scanned: $($scanned -join ', ')"
    }
    # A file that waits has the wait: it loads the readback, or copies the function from it.
    foreach ($file in $files) {
        $text = [IO.File]::ReadAllText($file.FullName)
        if ($file.Name -eq 'partition_manager_readback.ps1' -or $text -notmatch '\bWait-RbVolumeScanSettle\b') { continue }
        Assert-Condition `
            -Condition ($text -match 'partition_manager_readback\.ps1' -or $text -match "Get-RigReadbackFunctionText\s+-Name\s+'Wait-RbVolumeScanSettle'") `
            -Message "$($file.Name) calls Wait-RbVolumeScanSettle but neither loads the readback nor copies the function from it"
    }
    "OK every online scan in scripts/ settles before a disk can go ($($scanned -join ', '))"
}

function Assert-ReadbackScanSettles {
    $events = New-Object System.Collections.Generic.List[string]
    $runner = {
        param($ReadbackFile, $Sink, [bool]$FailScan)
        function Repair-Volume { param($DriveLetter, [switch]$Scan, $ErrorAction) $Sink.Add('scan'); if ($FailScan) { throw 'The volume has corruption' }; 'NoErrorsFound' }
        function chkdsk.exe { $Sink.Add('chkdsk'); $global:LASTEXITCODE = 0; 'stand-in chkdsk' }
        function fsutil.exe { $Sink.Add('fsutil'); 'Volume - X: is NOT Dirty' }
        function Start-Sleep { param([int]$Seconds) $Sink.Add("sleep $Seconds") }
        function Write-Host { param($Object) $Sink.Add('said') }
        . $ReadbackFile
        Test-RbVolumeHealth -Letter 'X'
    }
    $readback = Join-Path $ScriptsRoot 'partition_manager_readback.ps1'
    $health = & $runner $readback $events $false
    Assert-Condition -Condition (($events -join ',') -eq 'scan,chkdsk,said,sleep 15,fsutil') -Message "passing scan: $($events -join ',')"
    Assert-Condition -Condition ("$($health.repair_scan)" -eq 'NoErrorsFound') -Message "passing scan returned $($health | Out-String)"
    $events.Clear()
    $threw = ''
    try { & $runner $readback $events $true | Out-Null } catch { $threw = $_.Exception.Message }
    Assert-Condition -Condition (($events -join ',') -eq 'scan,said,sleep 15') -Message "throwing scan: $($events -join ',')"
    Assert-Condition -Condition ($threw -eq 'The volume has corruption') -Message "throwing scan reported '$threw'"
    'OK the readback waits 15 s after its scan, passed or thrown, and the error still surfaces'
}

function Assert-RigCopyStandsAlone {
    $rig = Join-Path $ScriptsRoot 'run_partition_manager_certification_rig.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($rig, [ref]$tokens, [ref]$errors)
    $copier = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-RigReadbackFunctionText' }, $true)
    Assert-Condition -Condition ($null -ne $copier) -Message 'the rig has no Get-RigReadbackFunctionText'
    # The copier resolves the readback beside its own file ($PSScriptRoot), as it does in the rig:
    # both go to a folder of their own, and the copier is loaded from a file there.
    $folder = Join-Path ([IO.Path]::GetTempPath()) ('sak-scan-settle-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $folder | Out-Null
    try {
        Copy-Item -LiteralPath (Join-Path $ScriptsRoot 'partition_manager_readback.ps1') -Destination $folder
        $copierPath = Join-Path $folder 'copier.ps1'
        [IO.File]::WriteAllText($copierPath, $copier.Extent.Text)
        $events = New-Object System.Collections.Generic.List[string]
        $runner = {
            param($CopierFile, $Sink)
            . $CopierFile
            $copied = Get-RigReadbackFunctionText -Name 'Wait-RbVolumeScanSettle'
            $job = [scriptblock]::Create(
                'param($Sink) function Start-Sleep { param([int]$Seconds) $Sink.Add("sleep $Seconds") }' +
                "`nfunction Write-Host { param(`$Object) `$Sink.Add('said') }`n" + $copied + "`nWait-RbVolumeScanSettle")
            & $job $Sink
        }
        & $runner $copierPath $events
    } finally {
        Remove-Item -LiteralPath $folder -Recurse -Force
    }
    Assert-Condition -Condition (($events -join ',') -eq 'said,sleep 15') -Message "the rig's copy of the wait: $($events -join ',')"
    'OK the rig copies the readback''s wait, and the copy stands alone'
}

Assert-EveryScanSettles
Assert-ReadbackScanSettles
Assert-RigCopyStandsAlone
