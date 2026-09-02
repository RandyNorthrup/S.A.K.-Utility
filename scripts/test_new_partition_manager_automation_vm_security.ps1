# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

[CmdletBinding()]
param(
    [string]$HelperScript = '',
    [string]$AutomationScript = '',
    [string]$Case = 'all'
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $PSCommandPath
if ([string]::IsNullOrWhiteSpace($HelperScript)) {
    $HelperScript = Join-Path $ScriptRoot 'owner_only_staging_file.ps1'
}
if ([string]::IsNullOrWhiteSpace($AutomationScript)) {
    $AutomationScript = Join-Path $ScriptRoot 'new_partition_manager_automation_vm.ps1'
}
. $HelperScript

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Should-Run([string]$Name) {
    return $Case -eq 'all' -or $Case -eq $Name
}

function Assert-Fails([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    $failed = $false
    try {
        & $Action
    }
    catch {
        $failed = $_.Exception.Message -match $Pattern
        if (-not $failed) {
            throw "$Message (wrong failure: $($_.Exception.Message))"
        }
    }
    if (-not $failed) {
        throw "$Message (operation succeeded)"
    }
}

function New-DirectoryJunction([string]$Path, [string]$Target) {
    $output = & "$env:SystemRoot\System32\cmd.exe" /d /c mklink /J $Path $Target 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create ACL-test junction: $($output -join ' ')"
    }
}

function Invoke-AutomationNativeCase([string]$FakeVBox) {
    $hostPath = (Get-Process -Id $PID).Path
    return Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', $AutomationScript, '-VBoxManage', $FakeVBox, '-TestNativeCaptureOnly'
    )
}

$ProjectRoot = Split-Path -Parent $ScriptRoot
$testRoot = Join-Path $ProjectRoot "build\automation-vm-security-tests-$PID"
$outsideRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sak-vm-security-outside-$PID"
$junctions = [System.Collections.Generic.List[string]]::new()
$script:cleanupLock = $null
$fakeVariables = @(
    'SAK_FAKE_ICACLS_EXIT',
    'SAK_FAKE_ICACLS_APPLY_REAL',
    'SAK_REAL_ICACLS'
)

try {
    foreach ($path in @($testRoot, $outsideRoot)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        }
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }

    $fakeSource = @'
using System;
using System.Diagnostics;

public static class FakeIcacls {
    static string Quote(string value) {
        return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }
    public static int Main(string[] args) {
        if (Environment.GetEnvironmentVariable("SAK_FAKE_ICACLS_APPLY_REAL") == "1") {
            ProcessStartInfo start = new ProcessStartInfo();
            start.FileName = Environment.GetEnvironmentVariable("SAK_REAL_ICACLS");
            start.Arguments = String.Join(" ", Array.ConvertAll(args, Quote));
            start.UseShellExecute = false;
            Process child = Process.Start(start);
            child.WaitForExit();
            if (child.ExitCode != 0) return child.ExitCode;
        }
        int result;
        return Int32.TryParse(
            Environment.GetEnvironmentVariable("SAK_FAKE_ICACLS_EXIT"),
            out result
        ) ? result : 0;
    }
}
'@
    $sourcePath = Join-Path $testRoot 'fake-icacls.cs'
    $fakeIcacls = Join-Path $testRoot 'fake-icacls.exe'
    Set-Content -LiteralPath $sourcePath -Value $fakeSource -Encoding ascii
    $csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path -LiteralPath $csc -PathType Leaf)) {
        throw "C# compiler not found: $csc"
    }
    $compile = Invoke-CapturedProcess -FilePath $csc -Arguments @(
        '/nologo', '/target:exe', "/out:$fakeIcacls", $sourcePath
    )
    if ($compile.exit_code -ne 0 -or
        -not (Test-Path -LiteralPath $fakeIcacls -PathType Leaf)) {
        throw "Could not compile fake icacls: $($compile.stderr)"
    }
    $realIcacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    $env:SAK_REAL_ICACLS = $realIcacls

    if (Should-Run 'vbox_native_exit') {
        $env:SAK_FAKE_ICACLS_APPLY_REAL = '0'
        $env:SAK_FAKE_ICACLS_EXIT = '11'
        $result = Invoke-AutomationNativeCase -FakeVBox $fakeIcacls
        $diagnostic = $result.stdout + [Environment]::NewLine + $result.stderr
        Assert-True ($result.exit_code -ne 0 -and
            $diagnostic -match 'VBoxManage failed with exit code 11') (
            'VBoxManage native failure was not propagated'
        )
    }
    if (Should-Run 'vbox_native_success') {
        $env:SAK_FAKE_ICACLS_APPLY_REAL = '0'
        $env:SAK_FAKE_ICACLS_EXIT = '0'
        $result = Invoke-AutomationNativeCase -FakeVBox $fakeIcacls
        Assert-True ($result.exit_code -eq 0 -and
            $result.stdout -match 'native capture self-test passed') (
            'VBoxManage native success path failed'
        )
    }

    $secret = 'VM-PASSWORD-SENTINEL-ONLY-FOR-TEST'
    $positivePath = Join-Path $testRoot 'positive.password'
    if (Should-Run 'positive') {
        $script:beforeWriteObserved = $false
        $beforeWrite = {
            param($path)
            Assert-True ((Get-Item -LiteralPath $path -Force).Length -eq 0) (
                'Password content existed before ACL verification'
            )
            Assert-OwnerOnlyStagingFile -Path $path
            $script:beforeWriteObserved = $true
        }
        $createArguments = @{
            Path = $positivePath
            Content = $secret
            BeforeWriteForTesting = $beforeWrite
        }
        $created = New-OwnerOnlyAsciiStagingFile @createArguments
        Assert-True $script:beforeWriteObserved 'Pre-write ACL checkpoint did not execute'
        Assert-True ((Get-Content -LiteralPath $created -Raw) -eq $secret) (
            'Secure password content changed'
        )
        Assert-OwnerOnlyStagingFile -Path $created
    }
    else {
        New-OwnerOnlyAsciiStagingFile -Path $positivePath -Content $secret | Out-Null
    }

    if (Should-Run 'native_exit') {
        $path = Join-Path $testRoot 'native-exit.password'
        $env:SAK_FAKE_ICACLS_APPLY_REAL = '1'
        $env:SAK_FAKE_ICACLS_EXIT = '9'
        $script:cleanupObserved = $false
        $cleanupHook = {
            param($cleanupPath)
            $script:cleanupObserved = Test-Path -LiteralPath $cleanupPath
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                IcaclsPath = $fakeIcacls
                BeforeCleanupForTesting = $cleanupHook
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'icacls failed with exit code 9' 'Non-zero icacls exit was ignored'
        Assert-True $script:cleanupObserved 'ACL failure did not enter cleanup'
        Assert-True (-not (Test-Path -LiteralPath $path)) 'ACL failure left plaintext file'
    }

    if (Should-Run 'non_ascii_content') {
        $path = Join-Path $testRoot 'non-ascii.password'
        Assert-Fails {
            $nonAscii = 'p' + [char]0x00E4 + 'ssword'
            New-OwnerOnlyAsciiStagingFile -Path $path -Content $nonAscii | Out-Null
        } 'ASCII' 'Non-ASCII staging content was silently rewritten'
        Assert-True (-not (Test-Path -LiteralPath $path)) (
            'Non-ASCII staging refusal created a file'
        )
    }

    if (Should-Run 'acl_verification') {
        $path = Join-Path $testRoot 'verification.password'
        $env:SAK_FAKE_ICACLS_APPLY_REAL = '0'
        $env:SAK_FAKE_ICACLS_EXIT = '0'
        $writeCheckpoint = {
            throw 'Unverified ACL reached the write checkpoint'
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                IcaclsPath = $fakeIcacls
                BeforeWriteForTesting = $writeCheckpoint
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'Owner-only staging file' 'Unverified ACL was accepted'
        Assert-True (-not (Test-Path -LiteralPath $path)) 'ACL verification failure left file'
    }

    if (Should-Run 'prewrite_failure_cleanup') {
        $path = Join-Path $testRoot 'prewrite-failure.password'
        $hook = {
            param($hookPath)
            Assert-OwnerOnlyStagingFile -Path $hookPath
            Assert-True ((Get-Item -LiteralPath $hookPath -Force).Length -eq 0) (
                'Injected pre-write failure observed content'
            )
            throw 'injected-before-write'
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                BeforeWriteForTesting = $hook
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'injected-before-write' 'Pre-write failure did not abort'
        Assert-True (-not (Test-Path -LiteralPath $path)) 'Pre-write failure left file'
    }

    if (Should-Run 'postwrite_acl_verification') {
        $path = Join-Path $testRoot 'postwrite-acl.password'
        $tamperHook = {
            param($tamperPath)
            $tamper = Invoke-CapturedProcess -FilePath $realIcacls -Arguments @(
                $tamperPath, '/inheritance:e', '/L', '/Q'
            )
            Assert-True ($tamper.exit_code -eq 0) 'Could not inject post-write ACL drift'
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                AfterWriteBeforeVerifyForTesting = $tamperHook
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'Owner-only staging file' 'Post-write ACL drift was accepted'
        Assert-True (-not (Test-Path -LiteralPath $path)) 'Post-write ACL drift left file'
    }

    if (Should-Run 'cleanup_failure') {
        $path = Join-Path $testRoot 'cleanup-failure.password'
        $env:SAK_FAKE_ICACLS_APPLY_REAL = '0'
        $env:SAK_FAKE_ICACLS_EXIT = '7'
        $lockHook = {
            param($lockPath)
            $script:cleanupLock = [System.IO.FileStream]::new(
                $lockPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read
            )
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                IcaclsPath = $fakeIcacls
                BeforeCleanupForTesting = $lockHook
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'cleanup also failed' 'Cleanup failure was hidden'
        Assert-True ((Get-Item -LiteralPath $path -Force).Length -eq 0) (
            'Cleanup failure exposed password content'
        )
        $script:cleanupLock.Dispose()
        $script:cleanupLock = $null
        Remove-Item -LiteralPath $path -Force -ErrorAction Stop
    }

    if (Should-Run 'reuse_insecure') {
        $path = Join-Path $testRoot 'insecure-existing.password'
        Set-Content -LiteralPath $path -Value $secret -NoNewline -Encoding ascii
        Assert-Fails {
            Assert-ReusableOwnerOnlyTextFile -Path $path | Out-Null
        } 'Owner-only staging file' 'Insecure existing password file was accepted'
    }

    if (Should-Run 'reuse_secure') {
        $resolved = Assert-ReusableOwnerOnlyTextFile -Path $positivePath
        Assert-True ($resolved -eq (Resolve-Path -LiteralPath $positivePath).Path) (
            'Secure existing password file did not resolve'
        )
    }

    if (Should-Run 'reuse_size_bounds') {
        $empty = Join-Path $testRoot 'empty.password'
        [System.IO.File]::WriteAllBytes($empty, [byte[]]@())
        Set-OwnerOnlyStagingAcl -Path $empty -IcaclsPath $realIcacls
        Assert-Fails {
            Assert-ReusableOwnerOnlyTextFile -Path $empty | Out-Null
        } 'outside 1..4096' 'Empty reusable password was accepted'

        $oversize = Join-Path $testRoot 'oversize.password'
        New-OwnerOnlyAsciiStagingFile -Path $oversize -Content ('X' * 4097) | Out-Null
        Assert-Fails {
            Assert-ReusableOwnerOnlyTextFile -Path $oversize | Out-Null
        } 'outside 1..4096' 'Oversized reusable password was accepted'
    }

    if (Should-Run 'no_overwrite') {
        $path = Join-Path $testRoot 'existing.password'
        Set-Content -LiteralPath $path -Value 'keep' -NoNewline -Encoding ascii
        Assert-Fails {
            New-OwnerOnlyAsciiStagingFile -Path $path -Content $secret | Out-Null
        } 'Refusing to overwrite' 'Existing password file was overwritten'
        Assert-True ((Get-Content -LiteralPath $path -Raw) -eq 'keep') (
            'Existing password content changed'
        )
    }

    if (Should-Run 'create_race_no_overwrite') {
        $path = Join-Path $testRoot 'raced.password'
        $raceHook = {
            param($racePath)
            Set-Content -LiteralPath $racePath -Value 'attacker-file' -NoNewline -Encoding ascii
        }
        Assert-Fails {
            $createArguments = @{
                Path = $path
                Content = $secret
                BeforeCreateForTesting = $raceHook
            }
            New-OwnerOnlyAsciiStagingFile @createArguments | Out-Null
        } 'already exists' 'Create race overwrote a planted file'
        Assert-True (Test-Path -LiteralPath $path) (
            'Create-race cleanup deleted a file this run did not create'
        )
        Assert-True ((Get-Content -LiteralPath $path -Raw) -eq 'attacker-file') (
            'Create-race cleanup deleted or changed a file this run did not create'
        )
    }

    if (Should-Run 'reparse_parent') {
        $target = Join-Path $outsideRoot 'junction-target'
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        $junction = Join-Path $testRoot 'junction-parent'
        New-DirectoryJunction -Path $junction -Target $target
        $junctions.Add($junction)
        $outsidePassword = Join-Path $target 'redirected.password'
        Assert-Fails {
            New-OwnerOnlyAsciiStagingFile -Path (
                Join-Path $junction 'redirected.password'
            ) -Content $secret | Out-Null
        } 'crosses a reparse point' 'Reparse parent was accepted'
        Assert-True (-not (Test-Path -LiteralPath $outsidePassword)) (
            'Reparse parent exposed password outside staging tree'
        )
    }

    Write-Host "Automation VM security tests passed under $($PSVersionTable.PSVersion)."
}
finally {
    if ($null -ne $script:cleanupLock) {
        $script:cleanupLock.Dispose()
        $script:cleanupLock = $null
    }
    foreach ($junction in $junctions) {
        if (Test-Path -LiteralPath $junction) {
            & "$env:SystemRoot\System32\cmd.exe" /d /c rmdir $junction | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Could not remove ACL-test junction: $junction"
            }
        }
    }
    foreach ($name in $fakeVariables) {
        if (Test-Path -LiteralPath "Env:\$name") {
            Remove-Item "Env:\$name" -ErrorAction Stop
        }
    }
    foreach ($path in @($testRoot, $outsideRoot)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        }
    }
}
