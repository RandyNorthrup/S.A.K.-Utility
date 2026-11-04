<#
.SYNOPSIS
    Runs Partition Manager certification rows on a disposable machine, through the real app path.

.DESCRIPTION
    Each row arranges its fixture with Windows' own tools, runs each operation as a person's Apply
    does, and then reads the disk back with scripts/partition_manager_readback.ps1. An operation
    goes through scripts' step client: the app's own request building, its queue and its checks,
    and the real elevated helper, started by a process under a standard-user token, the way the app
    runs. The row writes what it read back. It decides nothing: the verifier checks the readback
    against certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json afterwards.

    It runs only as administrator, in a VirtualBox guest or on a physical PC named as disposable by
    the approval the rig's PcPrepare writes. On any other machine it stops before it reads a disk.

    Physical disks are named by a pin file, read from the machine that holds them. A row on a real
    device stops unless the device matches the pin in model, serial and size immediately before the
    step. -DataRowsOnDevice puts the data rows that allow it on the pinned hard drive, in small
    partitions, in place of a VHD. A row says what it ran on from what Windows reports at that moment,
    and which machine it ran on.

.PARAMETER Rows
    Row ids to run. Boot rows run one phase at a time, driven from the host.
.PARAMETER StageRoot
    Folder holding the step client as sak_utility.exe, the real sak_elevated_helper.exe beside it,
    their DLLs, and build-receipt.json.
.PARAMETER OutputRoot
    Where each row's evidence goes: <OutputRoot>\<row id>\report.json and what it lists.
.PARAMETER PhysicalPins
    JSON file: { "ssd": {pin}, "hdd": {pin} }, each pin holding unique_id, serial_number, model,
    size_bytes and the words a person typed.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string[]]$Rows,
    [Parameter(Mandatory = $true)] [string]$StageRoot,
    [Parameter(Mandatory = $true)] [string]$OutputRoot,
    [string]$WorkRoot = 'C:\SAKCert\work',
    [string]$PhysicalPins = '',
    [string]$Phase = '',
    [uint64]$DataDiskBytes = 0,
    [switch]$DataRowsOnDevice
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'partition_manager_readback.ps1')
. (Join-Path $PSScriptRoot 'partition_manager_row_evidence.ps1')

Assert-RbDisposableMachine
$Script:InVirtualBox = Test-RbInVirtualBox -Facts (Get-RbMachineFacts)
$Script:VhdKind = if ($Script:InVirtualBox) { 'VHDX in a VM' } else { 'VHDX on a physical PC' }
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This arranges fixtures and reads raw disks, so it runs as administrator.'
}

$Script:RunId = [Guid]::NewGuid().ToString()
$Script:Receipt = Get-Content -LiteralPath (Join-Path $StageRoot 'build-receipt.json') -Raw | ConvertFrom-Json
$Script:GptBasicData = 'gpt:{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'
$Script:Marker = 'SAK-CERT-MARKER-20261104'
$Script:ClientTask = 'SAKCertClient'
$Script:FixtureTiming = @{}

function Initialize-CertGuest {
    # The app's helper asks for elevation through a consent prompt, which nobody in this session
    # can answer. Windows' own setting answers it, and every report says this was on.
    $policy = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
    # With UAC off every process is elevated, and "the client was not elevated" could not be true.
    if ((Get-ItemProperty -Path $policy -Name EnableLUA).EnableLUA -ne 1) { throw 'UAC is off on this machine, so no standard-user token exists to run the client under' }
    Set-ItemProperty -Path $policy -Name ConsentPromptBehaviorAdmin -Value 0 -Type DWord
    Set-ItemProperty -Path $policy -Name PromptOnSecureDesktop -Value 0 -Type DWord
    New-Item -ItemType Directory -Force -Path $WorkRoot, $OutputRoot | Out-Null
    & icacls.exe $WorkRoot /grant '*S-1-5-32-545:(OI)(CI)M' /Q | Out-Null
    & icacls.exe $OutputRoot /grant '*S-1-5-32-545:(OI)(CI)M' /Q | Out-Null
    & icacls.exe $StageRoot /grant '*S-1-5-32-545:(OI)(CI)M' /Q | Out-Null
    # The BUILTIN\Users grants are for the standard-user client, which writes its own report and log.
    $stub = Join-Path $WorkRoot 'client_stub.ps1'
    [IO.File]::WriteAllText($stub, @"
`$ErrorActionPreference = 'Continue'
`$job = Get-Content -LiteralPath '$WorkRoot\client-job.txt' -Raw
`$job = `$job.Trim()
`$p = Start-Process -FilePath '$StageRoot\sak_utility.exe' -ArgumentList '--scenario-step', ('"' + `$job + '"') -WorkingDirectory '$StageRoot' -WindowStyle Hidden -RedirectStandardOutput "`$job.out" -RedirectStandardError "`$job.err" -PassThru
`$null = `$p.Handle
"`$(`$p.Id)" | Set-Content -LiteralPath "`$job.pid"
`$p.WaitForExit()
"`$(`$p.ExitCode)" | Set-Content -LiteralPath "`$job.done"
"@)
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$stub`""
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 24) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $Script:ClientTask -Action $action -Principal $principal -Settings $settings -Force | Out-Null
}

function Get-CertRig {
    $system = Get-CimInstance -ClassName Win32_ComputerSystem
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    return [ordered]@{
        machine_manufacturer = "$($system.Manufacturer)"
        machine_model = "$($system.Model)"
        firmware = "$env:firmware_type"
        os_caption = "$($os.Caption)"
        os_build = "$($os.BuildNumber)"
        consent_prompt_answered_by_policy = $true
    }
}

function Get-CertDeviceKind {
    # How a real device reaches this Windows, from where it is running and what Windows says now.
    param([Parameter(Mandatory = $true)] [string]$BusType)
    if ($Script:InVirtualBox) { return "$BusType device passed through to a VM" }
    if ($BusType -ceq 'USB') { return 'USB on a physical PC' }
    return "$BusType inside a physical PC"
}

function Assert-CertBootRowInVm {
    # A boot row changes the Windows it boots from, and proves itself by starting that disk in a VM
    # of its own. On a physical PC nothing here could prove that, or put the machine back.
    if (-not $Script:InVirtualBox) { throw 'Boot rows run on the VM rig. No chain here proves a boot on a physical PC' }
}

function Get-CertPin {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    $disk = Get-RbDiskFacts -DiskNumber $DiskNumber
    $serial = $disk.serial
    $model = $disk.model
    $size = [uint64]$disk.size_bytes
    return [ordered]@{
        unique_id = $disk.unique_id
        serial_number = $serial
        model = $model
        size_bytes = "$size"
        confirmation = "CERTIFY $model $serial $size"
    }
}

function Get-CertMedia {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [string]$Class, [string]$Kind = '')
    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $physical = Get-PhysicalDisk | Where-Object { "$($_.DeviceId)" -eq "$DiskNumber" } | Select-Object -First 1
    return [ordered]@{
        class = $Class
        kind = $Kind
        model = "$($disk.FriendlyName)"
        bus_type = "$($disk.BusType)"
        media_type = if ($physical) { "$($physical.MediaType)" } else { '' }
        size_bytes = "$([uint64]$disk.Size)"
        serial_sha256 = (Get-CertSha256OfText "$($disk.SerialNumber)".Trim())
    }
}

function Get-CertSha256OfText {
    param([AllowEmptyString()] [string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function New-CertVhd {
    # A file-backed virtual disk inside the guest. It goes away with the row.
    param([Parameter(Mandatory = $true)] [string]$Name, [int]$SizeMB = 1024)
    $path = Join-Path $WorkRoot "$Name.vhdx"
    if (Test-Path -LiteralPath $path) { throw "Refusing to reuse a certification VHD: $path" }
    $script = Join-Path $WorkRoot "$Name.diskpart.txt"
    @("create vdisk file=`"$path`" maximum=$SizeMB type=expandable", 'attach vdisk') |
        Set-Content -LiteralPath $script -Encoding ASCII
    $output = & diskpart.exe /s $script 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "diskpart could not make $path`: $output" }
    $Script:RowVhds.Add($path)
    Start-Sleep -Seconds 2
    Update-HostStorageCache
    $image = Get-DiskImage -ImagePath $path
    $disk = Get-Disk -Number $image.Number
    if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false }
    if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false }
    return [int]$disk.Number
}

function Remove-CertVhds {
    foreach ($path in @($Script:RowVhds)) {
        # A row's own failure matters more than its tidying up, so this says what it left and goes on.
        try {
            Dismount-DiskImage -ImagePath $path -ErrorAction Stop | Out-Null
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        } catch { Write-Warning "A VHD was left behind: $path. $($_.Exception.Message)" }
    }
    $Script:RowVhds.Clear()
}

function Get-CertFreeLetter {
    $used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { "$($_.DriveLetter)" })
    foreach ($candidate in 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'R', 'S', 'P', 'Q') {
        if ($used -notcontains $candidate) { return $candidate }
    }
    throw 'No free drive letter'
}

function New-CertVolume {
    # A fixture volume, made with Windows' tools. What the app later does to it is the test.
    param(
        [Parameter(Mandatory = $true)] [int]$DiskNumber,
        [Parameter(Mandatory = $true)] [uint64]$SizeBytes,
        [string]$FileSystem = 'NTFS',
        [string]$Label = 'SAKFIX',
        [int]$ClusterBytes = 4096,
        [uint64]$Offset = 0
    )
    $letter = Get-CertFreeLetter
    $arguments = @{ DiskNumber = $DiskNumber; Size = $SizeBytes; DriveLetter = $letter }
    if ($Offset -gt 0) { $arguments.Offset = $Offset }
    $partition = New-Partition @arguments
    Format-Volume -Partition $partition -FileSystem $FileSystem -NewFileSystemLabel $Label -AllocationUnitSize $ClusterBytes -Confirm:$false -Force | Out-Null
    Start-Sleep -Seconds 1
    return [pscustomobject]@{ partition_number = [int]$partition.PartitionNumber; letter = $letter }
}

function Get-CertHelperObservation {
    param([string]$ClientPidPath, [hashtable]$Seen)
    if (-not (Test-Path -LiteralPath $ClientPidPath)) { return }
    $clientPid = 0
    # The client's task may still be writing this. The next look will see it.
    $written = try { Get-Content -LiteralPath $ClientPidPath } catch { '' }
    if (-not [int]::TryParse("$written".Trim(), [ref]$clientPid)) { return }
    if (-not $Seen.ContainsKey('client_token_elevated')) {
        $elevated = Get-RbProcessElevation -ProcessId $clientPid
        if ($null -ne $elevated) { $Seen.client_token_elevated = $elevated; $Seen.client_pid = $clientPid }
    }
    $helpers = @(Get-CimInstance -ClassName Win32_Process -Filter "Name='sak_elevated_helper.exe'")
    foreach ($helper in $helpers) {
        # A process just started can list no path yet. The next look will see it.
        if ("$($helper.CommandLine)" -match "--parent-pid\s+$clientPid\b" -and -not $Seen.ContainsKey('helper_pid') -and $helper.ExecutablePath) {
            $Seen.helper_pid = [int]$helper.ProcessId
            $Seen.helper_path = "$($helper.ExecutablePath)"
            $Seen.helper_token_elevated = Get-RbProcessElevation -ProcessId ([int]$helper.ProcessId)
            $Seen.helper_sha256 = (Get-FileHash -LiteralPath "$($helper.ExecutablePath)" -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

function Invoke-CertClient {
    # Runs the client under this signed-in user's standard-user token, through a task, and watches
    # the helper it starts. Returns what Windows said about both.
    param([Parameter(Mandatory = $true)] [string]$JobPath, [int]$TimeoutSeconds)
    foreach ($suffix in '.done', '.pid', '.out', '.err') { if (Test-Path -LiteralPath "$JobPath$suffix") { Remove-Item -LiteralPath "$JobPath$suffix" } }
    Set-Content -LiteralPath (Join-Path $WorkRoot 'client-job.txt') -Value $JobPath -Encoding ASCII
    $started = & schtasks.exe /Run /TN $Script:ClientTask 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "The client task did not start: $started" }
    $seen = @{}
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds + 120)
    while (-not (Test-Path -LiteralPath "$JobPath.done")) {
        if ((Get-Date) -gt $deadline) { throw "The client did not finish within $TimeoutSeconds seconds: $JobPath" }
        Get-CertHelperObservation -ClientPidPath "$JobPath.pid" -Seen $seen
        Start-Sleep -Milliseconds 500
    }
    $seen.exit_code = [int]("$(Get-Content -LiteralPath "$JobPath.done")".Trim())
    return $seen
}

function Invoke-CertElevatedClient {
    # A row whose real path is the app running as administrator, with no helper in it.
    param([Parameter(Mandatory = $true)] [string]$JobPath, [int]$TimeoutSeconds)
    $process = Start-Process -FilePath (Join-Path $StageRoot 'sak_utility.exe') -ArgumentList '--scenario-step', "`"$JobPath`"" -WorkingDirectory $StageRoot -WindowStyle Hidden -RedirectStandardOutput "$JobPath.out" -RedirectStandardError "$JobPath.err" -PassThru
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { $process.Kill(); throw "The elevated client ran past $TimeoutSeconds seconds" }
    return @{ exit_code = [int]$process.ExitCode; client_token_elevated = $true; client_pid = [int]$process.Id }
}

function Invoke-CertStep {
    <#
    .SYNOPSIS
        One operation, the way the app does it. Stops the row if it did not do what was expected.
    #>
    param(
        [Parameter(Mandatory = $true)] [string]$Step,
        [Parameter(Mandatory = $true)] [string]$Operation,
        [Parameter(Mandatory = $true)] [object]$Pin,
        [object]$SecondPin = $null,
        [hashtable]$Select = @{},
        [hashtable]$Choices = @{},
        [string]$Expect = 'success',
        [string]$RefusalContains = '',
        [int]$TimeoutSeconds = 1800,
        [switch]$Elevated
    )
    $name = "$Step.client"
    $reportPath = Join-Path $Script:RowDir "$name.json"
    $jobPath = Join-Path $WorkRoot "$($Script:RowId)-$Step.job.json"
    $job = [ordered]@{
        scenario_id = $Script:RowId
        step = $Step
        run_id = $Script:RunId
        operation = $Operation
        report_path = $reportPath
        expect = $Expect
        refusal_contains = $RefusalContains
        confirmation = $Pin.confirmation
        pin = $Pin
        select = $Select
        choices = $Choices
        timeout_seconds = $TimeoutSeconds
        allow_elevated_client = [bool]$Elevated
    }
    if ($null -ne $SecondPin) { $job.second_pin = $SecondPin }
    [IO.File]::WriteAllText($jobPath, ($job | ConvertTo-Json -Depth 8))
    Write-Host "[$(Get-Date -Format HH:mm:ss)] $($Script:RowId) $Step ($Operation)"
    $seen = if ($Elevated) { Invoke-CertElevatedClient -JobPath $jobPath -TimeoutSeconds $TimeoutSeconds } else { Invoke-CertClient -JobPath $jobPath -TimeoutSeconds $TimeoutSeconds }
    Remove-Item -LiteralPath $jobPath
    Start-Sleep -Seconds 3
    Update-HostStorageCache
    [void]$Script:RowSteps.Add([ordered]@{
        id = $Step
        operation = $Operation
        expect = $Expect
        report = "$name.json"
        observed = $seen
    })
    if (-not (Test-Path -LiteralPath $reportPath)) {
        $err = if (Test-Path -LiteralPath "$jobPath.err") { Get-Content -LiteralPath "$jobPath.err" | Select-Object -Last 8 | Out-String } else { '' }
        throw "$Step wrote no report (exit $($seen.exit_code)): $err"
    }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.status -ne 'Passed') { throw "$Step failed: $($report.error)" }
    return $report
}

function Write-CertRawPattern {
    <#
    .SYNOPSIS
        Fills a range no partition holds with bytes that depend on where they are. Only a true raw
        copy carries them, and no file system touches them afterwards.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [uint64]$Offset, [Parameter(Mandatory = $true)] [uint64]$Length)
    $stream = [IO.File]::Open("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        $block = New-Object byte[] 1048576
        $random = New-Object Random 7
        $stream.Position = [int64]$Offset
        $left = $Length
        while ($left -gt 0) {
            $random.NextBytes($block)
            $count = [int][Math]::Min([uint64]$block.Length, $left)
            $stream.Write($block, 0, $count)
            $left -= [uint64]$count
        }
        $stream.Flush()
    } finally { $stream.Dispose() }
}

function Set-CertVolumeQuiet {
    <#
    .SYNOPSIS
        Remounts a fixture volume read-only. NTFS writes to its own log whenever it mounts, so a hash
        of a mounted volume is not stable. A read-only volume stays what it was, which lets a row
        compare it byte for byte, and the flag is in the table, outside the bytes being compared.
    #>
    param([Parameter(Mandatory = $true)] [int]$DiskNumber, [Parameter(Mandatory = $true)] [int]$PartitionNumber)
    Set-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber -IsReadOnly $true
    Set-Disk -Number $DiskNumber -IsOffline $true
    Set-Disk -Number $DiskNumber -IsOffline $false
    Start-Sleep -Seconds 3
    Update-HostStorageCache
}

function Get-CertDifferenceSummary {
    <#
    .SYNOPSIS
        Sorts where a copy differs from its source. Windows corrects a GPT copied to a larger disk,
        and writes inside a volume when it mounts it, as it does for a person's copy too. So those
        two are counted apart, and every other sector has to match. A row whose source was read-only
        has no writes inside the volume either, because its copy mounts read-only as well.
    #>
    param([AllowEmptyCollection()] [object[]]$Differences, [uint64]$Length, [uint64]$VolumeOffset, [uint64]$VolumeBytes)
    $tableEnd = [uint64]34 * 512
    $backupStart = $Length - ([uint64]33 * 512)
    $table = 0; $inVolume = [uint64]0; $elsewhere = 0
    foreach ($run in $Differences) {
        $offset = [uint64]$run.offset_bytes
        $end = $offset + [uint64]$run.length_bytes
        if ($end -le $tableEnd -or $offset -ge $backupStart) { $table++ }
        elseif ($offset -ge $VolumeOffset -and $end -le ($VolumeOffset + $VolumeBytes)) { $inVolume += [uint64]$run.length_bytes }
        else { $elsewhere++ }
    }
    return [ordered]@{ partition_table_runs = $table; bytes_inside_the_mounted_volume = "$inVolume"; runs_anywhere_else = $elsewhere }
}

function Get-CertSnapshot {
    param([Parameter(Mandatory = $true)] [int]$DiskNumber)
    return Add-RbDerivedFields -Snapshot (Get-RbDiskSnapshot -DiskNumber $DiskNumber)
}

function Get-CertDataPartition {
    # The fixture's own view of where a volume went, from Windows, for the next request.
    param([int]$DiskNumber, [int]$Index = 0)
    $parts = @(Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.Type -notin 'Reserved', 'System', 'Extended' } | Sort-Object Offset)
    if ($Index -ge $parts.Count) { throw "Disk $DiskNumber has no data partition $Index" }
    return $parts[$Index]
}

function Wait-CertVolumeReady {
    # A letter the helper just assigned reaches this process, and its file system mounts, a moment later.
    param([Parameter(Mandatory = $true)] [string]$Letter, [int]$DiskNumber = -1, [int]$Seconds = 45)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        Update-HostStorageCache
        $null = Get-PSDrive -PSProvider FileSystem
        if ([IO.Directory]::Exists("${Letter}:\")) { return }
        Start-Sleep -Seconds 2
    }
    $facts = if ($DiskNumber -ge 0) {
        try {
            (Get-Partition -DiskNumber $DiskNumber | ForEach-Object {
                    "$($_.PartitionNumber):$($_.Type):$($_.DriveLetter):$(@($_.AccessPaths) -join '+')" }) -join ' | '
        } catch { "unreadable: $($_.Exception.Message)" }
    } else { '' }
    throw "${Letter}: never became readable. Partitions: $facts"
}

function Get-CertPartitionLetter {
    param([object]$Partition)
    if (-not ($Partition.DriveLetter -and [char]::IsLetter([char]$Partition.DriveLetter))) { throw 'The partition has no drive letter' }
    return "$($Partition.DriveLetter)"
}

function Get-CertCluster {
    param([string]$Letter)
    return Get-RbVolumeFacts -Letter $Letter
}

function Get-CertVolumesSummary {
    param([int]$DiskNumber)
    $parts = @(Get-Partition -DiskNumber $DiskNumber | Where-Object { $_.DriveLetter -and [char]::IsLetter([char]$_.DriveLetter) } | Sort-Object Offset)
    $unhealthy = 0
    $ntfs = 0
    $labels = @()
    foreach ($part in $parts) {
        $letter = "$($part.DriveLetter)"
        $facts = Get-RbVolumeFacts -Letter $letter
        if ($facts.file_system -eq 'NTFS') { $ntfs++ }
        $labels += $facts.label
        $health = Test-RbVolumeHealth -Letter $letter
        if ($health.repair_scan -ne 'NoErrorsFound' -or $health.chkdsk_exit_code -ne 0 -or $health.dirty) { $unhealthy++ }
    }
    return [ordered]@{ volume_count = $parts.Count; ntfs_volumes = $ntfs; labels = ($labels -join ','); unhealthy_volumes = $unhealthy }
}

. (Join-Path $PSScriptRoot 'partition_manager_certification_row_bodies.ps1')

function Invoke-CertRow {
    param([Parameter(Mandatory = $true)] [string]$Id)
    $Script:RowId = $Id
    $Script:RowDir = Join-Path $OutputRoot $Id
    if (Test-Path -LiteralPath $Script:RowDir) { Remove-Item -LiteralPath $Script:RowDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Script:RowDir | Out-Null
    $Script:RowSteps = New-Object System.Collections.ArrayList
    $Script:RowVhds = New-Object System.Collections.Generic.List[string]
    $Script:RowDeviceRole = ''
    $started = (Get-Date).ToUniversalTime().ToString('o')
    $status = 'Passed'
    $failure = ''
    $independent = [ordered]@{}
    $media = $null
    try {
        $body = Get-CertRowBody -Id $Id
        $result = & $body
        $independent = $result.independent
        $media = $result.media
    } catch {
        $status = 'Failed'
        $failure = "$($_.Exception.Message) at $($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)"
        Write-Host "FAILED $Id : $failure"
    } finally {
        Remove-CertVhds
        try { Remove-CertDevicePartitions } catch { Write-Host "Could not tidy the device: $($_.Exception.Message)" }
    }
    $report = New-PmRowReport -RowId $Id -RowDir $Script:RowDir -RunId $Script:RunId -Status $status -Failure $failure `
        -StartedUtc $started -Receipt $Script:Receipt -Rig (Get-CertRig) -Media $media -Steps @($Script:RowSteps) -Independent $independent
    return $report
}

Initialize-CertGuest
$summaries = @()
foreach ($row in $Rows) {
    $report = Invoke-CertRow -Id $row
    $summaries += [pscustomobject]@{ id = $row; status = $report.status; failure = $report.failure }
}
$summaries | ForEach-Object { "ROW $($_.id): $($_.status) $($_.failure)" }
if (@($summaries | Where-Object { $_.status -ne 'Passed' }).Count -gt 0) { exit 1 }
exit 0
