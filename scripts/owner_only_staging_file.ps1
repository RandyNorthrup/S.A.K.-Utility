<#
.SYNOPSIS
    Creates and verifies plaintext staging files with an owner-only Windows ACL.
#>

. (Join-Path $PSScriptRoot 'process_capture.ps1')

function Get-CurrentOwnerSid {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    if ($null -eq $identity.User) {
        throw 'Current Windows identity has no user SID.'
    }
    return $identity.User
}

function ConvertTo-SecurityIdentifier([object]$Identity) {
    if ($Identity -is [System.Security.Principal.SecurityIdentifier]) {
        return $Identity
    }
    if ($Identity -is [System.Security.Principal.IdentityReference]) {
        return $Identity.Translate([System.Security.Principal.SecurityIdentifier])
    }
    $text = [string]$Identity
    if ($text -match '^S-\d-(?:\d+-)+\d+$') {
        return [System.Security.Principal.SecurityIdentifier]::new($text)
    }
    $account = [System.Security.Principal.NTAccount]::new($text)
    return $account.Translate([System.Security.Principal.SecurityIdentifier])
}

function Assert-DirectStagingPath([string]$Path) {
    $candidate = [System.IO.Path]::GetFullPath($Path)
    $current = $candidate
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
                throw "Plaintext staging path crosses a reparse point: $current"
            }
            if ($current -ne $candidate -and -not $item.PSIsContainer) {
                throw "Plaintext staging parent is not a directory: $current"
            }
        }
        $parent = [System.IO.Directory]::GetParent($current)
        if ($null -eq $parent) {
            break
        }
        $next = $parent.FullName
        if ($next.Equals($current, [System.StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $next
    }
}

function Get-OwnerOnlyAclError([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "Owner-only staging file does not exist: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        return "Owner-only staging file is a reparse point: $Path"
    }

    $expectedSid = Get-CurrentOwnerSid
    $sections = [System.Security.AccessControl.AccessControlSections]::Owner -bor
        [System.Security.AccessControl.AccessControlSections]::Access
    $acl = [System.Security.AccessControl.FileSecurity]::new($Path, $sections)
    $ownerSid = $acl.GetOwner([System.Security.Principal.SecurityIdentifier])
    if ($ownerSid -ne $expectedSid) {
        return "Owner-only staging file owner is '$ownerSid', expected '$expectedSid': $Path"
    }
    if (-not $acl.AreAccessRulesProtected) {
        return "Owner-only staging file still inherits permissions: $Path"
    }

    $rules = @($acl.GetAccessRules(
        $true,
        $true,
        [System.Security.Principal.SecurityIdentifier]
    ))
    if ($rules.Count -ne 1) {
        return "Owner-only staging file has $($rules.Count) access rules, expected exactly one: $Path"
    }
    $rule = $rules[0]
    $ruleSid = ConvertTo-SecurityIdentifier -Identity $rule.IdentityReference
    if ($rule.IsInherited -or
        $rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow -or
        $ruleSid -ne $expectedSid -or
        (($rule.FileSystemRights -band
                [System.Security.AccessControl.FileSystemRights]::FullControl) -ne
            [System.Security.AccessControl.FileSystemRights]::FullControl)) {
        return "Owner-only staging file has an unexpected access rule: $Path"
    }
    return ''
}

function Assert-OwnerOnlyStagingFile([string]$Path) {
    Assert-DirectStagingPath -Path $Path
    $errorMessage = Get-OwnerOnlyAclError -Path $Path
    if (-not [string]::IsNullOrWhiteSpace($errorMessage)) {
        throw $errorMessage
    }
}

function Resolve-IcaclsPath([string]$ExplicitPath) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "icacls executable not found: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
        throw 'SystemRoot is not set; refusing to resolve icacls by name.'
    }
    $systemIcacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    if (-not (Test-Path -LiteralPath $systemIcacls -PathType Leaf)) {
        throw "System icacls executable not found: $systemIcacls"
    }
    return $systemIcacls
}

function Invoke-StagingIcacls([string]$IcaclsPath, [string[]]$Arguments) {
    $result = Invoke-CapturedProcess -FilePath $IcaclsPath -Arguments $Arguments
    if ($result.exit_code -ne 0) {
        $details = @($result.stdout, $result.stderr) | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        }
        throw "icacls failed with exit code $($result.exit_code): $($details -join ' ')"
    }
}

function Set-OwnerOnlyStagingAcl([string]$Path, [string]$IcaclsPath) {
    $ownerSid = Get-CurrentOwnerSid
    $sidText = $ownerSid.Value
    Invoke-StagingIcacls -IcaclsPath $IcaclsPath -Arguments @(
        $Path, '/setowner', "*$sidText", '/L', '/Q'
    )
    Invoke-StagingIcacls -IcaclsPath $IcaclsPath -Arguments @(
        $Path, '/inheritance:r', '/grant:r', ("*{0}:F" -f $sidText), '/L', '/Q'
    )
    Assert-OwnerOnlyStagingFile -Path $Path
}

function Remove-FailedStagingFile(
    [string]$Path,
    [System.Management.Automation.ErrorRecord]$Failure,
    [scriptblock]$BeforeCleanupForTesting
) {
    if ($null -ne $BeforeCleanupForTesting) {
        & $BeforeCleanupForTesting $Path
    }
    try {
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
        }
        if (Test-Path -LiteralPath $Path) {
            throw "Cleanup left the staging file present: $Path"
        }
    }
    catch {
        throw ("Owner-only staging failed: " + $Failure.Exception.Message +
               "; cleanup also failed: " + $_.Exception.Message)
    }
}

function New-OwnerOnlyAsciiStagingFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Content,
        [string]$IcaclsPath = '',
        [scriptblock]$BeforeCreateForTesting,
        [scriptblock]$BeforeWriteForTesting,
        [scriptblock]$AfterWriteBeforeVerifyForTesting,
        [scriptblock]$BeforeCleanupForTesting
    )

    if ($Content -match '[^\x00-\x7F]') {
        throw 'Plaintext staging content must be 7-bit ASCII; refusing lossy replacement.'
    }
    $contentBytes = [System.Text.Encoding]::ASCII.GetBytes($Content)
    $absolutePath = [System.IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $absolutePath
    if ([string]::IsNullOrWhiteSpace($parent)) {
        throw "Plaintext staging file requires a parent directory: $Path"
    }
    Assert-DirectStagingPath -Path $parent
    New-Item -ItemType Directory -Path $parent -Force -ErrorAction Stop | Out-Null
    Assert-DirectStagingPath -Path $parent
    if (Test-Path -LiteralPath $absolutePath) {
        throw "Refusing to overwrite an existing plaintext staging file: $absolutePath"
    }

    $resolvedIcacls = Resolve-IcaclsPath -ExplicitPath $IcaclsPath
    $stream = $null
    $createdFile = $false
    try {
        if ($null -ne $BeforeCreateForTesting) {
            & $BeforeCreateForTesting $absolutePath
        }
        $stream = [System.IO.FileStream]::new(
            $absolutePath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        $createdFile = $true
        Set-OwnerOnlyStagingAcl -Path $absolutePath -IcaclsPath $resolvedIcacls
        if ($null -ne $BeforeWriteForTesting) {
            & $BeforeWriteForTesting $absolutePath
        }
        $stream.Write($contentBytes, 0, $contentBytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        if ($null -ne $AfterWriteBeforeVerifyForTesting) {
            & $AfterWriteBeforeVerifyForTesting $absolutePath
        }
        Assert-OwnerOnlyStagingFile -Path $absolutePath
        return (Resolve-Path -LiteralPath $absolutePath).Path
    }
    catch {
        $failure = $_
        if ($null -ne $stream) {
            $stream.Dispose()
            $stream = $null
        }
        if ($createdFile) {
            $cleanup = @{
                Path = $absolutePath
                Failure = $failure
                BeforeCleanupForTesting = $BeforeCleanupForTesting
            }
            Remove-FailedStagingFile @cleanup
        }
        throw $failure
    }
}

function Assert-ReusableOwnerOnlyTextFile([string]$Path, [int64]$MaxBytes = 4096) {
    $absolutePath = [System.IO.Path]::GetFullPath($Path)
    Assert-OwnerOnlyStagingFile -Path $absolutePath
    $length = (Get-Item -LiteralPath $absolutePath -Force -ErrorAction Stop).Length
    if ($length -le 0 -or $length -gt $MaxBytes) {
        throw "Reusable owner-only staging file size $length is outside 1..$MaxBytes bytes: $absolutePath"
    }
    return (Resolve-Path -LiteralPath $absolutePath).Path
}
