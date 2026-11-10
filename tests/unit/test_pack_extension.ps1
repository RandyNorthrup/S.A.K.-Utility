# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Unit test for the verification seams in browser/pack-extension.ps1.
#
# WHY THIS EXISTS
# The packer used to validate its output with a single Test-Path and then PRINT the
# extension id as a claim. A pack that failed, was truncated, or was signed with a
# different key could be copied over browser/dist/sak_browser_control.crx and reported as
# success -- and because the installer force-lists a hardcoded id, that artifact installs
# on no customer machine at all. These cases pin the checks that make that unreachable.
#
# The functions under test are loaded out of the packer's own AST, so the assertions run
# against the shipped script rather than a copy of it. Nothing here launches Chrome or
# writes into the tree: every fixture is derived from the committed CRX in a temp dir.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath))
$PackScript = Join-Path $ProjectRoot 'browser\pack-extension.ps1'
$CommittedCrx = Join-Path $ProjectRoot 'browser\dist\sak_browser_control.crx'
$ManifestPath = Join-Path $ProjectRoot 'browser\extension\manifest.json'
$ExpectedId = 'ofodhfbipljnhenjjjpbdaglkjdphoec'

foreach ($required in @($PackScript, $CommittedCrx, $ManifestPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        Write-Error "test_pack_extension cannot run: missing $required"
        exit 1
    }
}

# Load the packer's function definitions without executing its body (which would demand a
# signing key and Chrome). Dot-sourcing each definition's own text defines it here.
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($PackScript, [ref]$null, [ref]$parseErrors)
if ($parseErrors -and $parseErrors.Count -gt 0) {
    Write-Error "test_pack_extension cannot run: $PackScript failed to parse."
    exit 1
}
$definitions = @($ast.FindAll(
    { $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false))
foreach ($definition in $definitions) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

$script:Failures = 0
$script:Checks = 0

function Assert-That {
    param([bool]$Condition, [string]$Message)
    $script:Checks++
    if (-not $Condition) {
        $script:Failures++
        Write-Host "FAIL: $Message" -ForegroundColor Red
    }
}

function Assert-Throws {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    $script:Checks++
    try {
        & $Action | Out-Null
    } catch {
        if ($_.Exception.Message -match $Pattern) { return }
        $script:Failures++
        Write-Host "FAIL: $Message (threw '$($_.Exception.Message)', wanted /$Pattern/)" -ForegroundColor Red
        return
    }
    $script:Failures++
    Write-Host "FAIL: $Message (nothing was thrown)" -ForegroundColor Red
}

foreach ($needed in @('ConvertTo-ExtensionId', 'Get-CrxExtensionId',
                      'Assert-CrxArchiveComplete', 'Resolve-CrxOutputPath',
                      'Get-ProcessDiagnostics', 'Format-NativeArgument', 'Invoke-ChromePack')) {
    Assert-That ([bool](Get-Command $needed -ErrorAction SilentlyContinue)) `
        "$PackScript must define $needed so the packed artifact is verified, not announced"
}
if ($script:Failures -gt 0) {
    Write-Error "test_pack_extension: the packer does not expose its verification seams."
    exit 1
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("sak_pack_test_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    $good = [System.IO.File]::ReadAllBytes($CommittedCrx)

    # 1. The shipped artifact really carries the id the installer force-lists.
    Assert-That ((Get-CrxExtensionId -Path $CommittedCrx) -eq $ExpectedId) `
        "committed CRX must derive $ExpectedId from its CRX3 header public key"
    Assert-That ($null -eq (Assert-CrxArchiveComplete -Path $CommittedCrx)) `
        "committed CRX must pass the archive-completeness check"

    # 2. The manifest's pinned key and the signing key must agree, or the unpacked-load id
    #    and the CRX id differ.
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    Assert-That ((ConvertTo-ExtensionId -PublicKeyDer ([System.Convert]::FromBase64String($manifest.key))) -eq $ExpectedId) `
        "manifest.json pinned key must derive $ExpectedId"

    # 3. An empty output -- what a failed pack plus an interrupted copy leaves behind.
    $empty = Join-Path $work 'empty.crx'
    [System.IO.File]::WriteAllBytes($empty, [byte[]]@())
    Assert-Throws { Get-CrxExtensionId -Path $empty } 'too short' `
        "a zero-byte CRX must be rejected"

    # 4. Any non-CRX payload at the output path.
    $notCrx = Join-Path $work 'notcrx.crx'
    [System.IO.File]::WriteAllBytes($notCrx, [byte[]](0x50, 0x4B, 0x03, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    Assert-Throws { Get-CrxExtensionId -Path $notCrx } 'Cr24 magic' `
        "a file without the Cr24 magic must be rejected"

    # 5. Truncated inside the CRX3 header.
    $stub = Join-Path $work 'stub.crx'
    [System.IO.File]::WriteAllBytes($stub, [byte[]]$good[0..299])
    Assert-Throws { Get-CrxExtensionId -Path $stub } 'leaves no archive' `
        "a CRX whose declared header runs past the file must be rejected"

    # 6. Truncated AFTER the header: the magic, the version and the whole signature header
    #    are intact and the id still derives, so only the archive check catches it. This is
    #    exactly the interrupted-copy case that used to overwrite the good artifact.
    $cut = Join-Path $work 'cut.crx'
    [System.IO.File]::WriteAllBytes($cut, [byte[]]$good[0..4999])
    Assert-That ((Get-CrxExtensionId -Path $cut) -eq $ExpectedId) `
        "a tail-truncated CRX still derives the pinned id, so the envelope check alone is not enough"
    Assert-Throws { Assert-CrxArchiveComplete -Path $cut } 'truncated' `
        "a CRX truncated after its header must be rejected as incomplete"

    # 7. A CRX signed with some other key: the derived id must not be the pinned one.
    $wrongKey = Join-Path $work 'wrongkey.crx'
    $flipped = [byte[]]$good.Clone()
    $flipped[40] = [byte]($flipped[40] -bxor 0xFF)
    [System.IO.File]::WriteAllBytes($wrongKey, $flipped)
    Assert-That ((Get-CrxExtensionId -Path $wrongKey) -ne $ExpectedId) `
        "a CRX carrying a different public key must not derive the pinned id"

    # 8. A CRX2 (or anything not CRX3) cannot be verified, so it must not be shipped.
    $crx2 = Join-Path $work 'crx2.crx'
    $oldVersion = [byte[]]$good.Clone()
    $oldVersion[4] = 2
    [System.IO.File]::WriteAllBytes($crx2, $oldVersion)
    Assert-Throws { Get-CrxExtensionId -Path $crx2 } 'only CRX3' `
        "a non-CRX3 container must be rejected"

    # 9. -Out handling: a bare filename must resolve to a real absolute path (it used to
    #    abort the run at New-Item after Chrome had already packed), a directory must be
    #    refused (it used to write <dir>\extension.crx and print 'Wrote <dir> (1 bytes)'),
    #    and a non-.crx name must be refused.
    $resolved = Resolve-CrxOutputPath -Path 'package.crx' -BaseDirectory $work
    Assert-That ([System.IO.Path]::IsPathRooted($resolved)) `
        "a bare -Out filename must resolve to an absolute path"
    Assert-That ((Split-Path -Parent $resolved).Length -gt 0) `
        "a resolved -Out must have a parent directory to create"
    Assert-Throws { Resolve-CrxOutputPath -Path $work -BaseDirectory $work } 'existing directory' `
        "a directory-valued -Out must be refused"
    Assert-Throws { Resolve-CrxOutputPath -Path 'package.txt' -BaseDirectory $work } 'must name a .crx' `
        "a non-.crx -Out must be refused"

    # 10. The Chrome run itself. A real browser and a signing key are not needed to prove how
    #     this script treats the process it launches -- only a program that behaves the way a
    #     failing Chrome does. These cases used to be asserted by grepping the shipped text for
    #     "WaitForExit(" and "ExitCode -ne 0", which passes just as happily when the code around
    #     those tokens is dead.
    $stage = Join-Path $work 'extension'
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $keyStub = Join-Path $work 'stub.pem'
    Set-Content -LiteralPath $keyStub -Value 'not-a-key' -Encoding ASCII

    # Chrome returns RESULT_CODE_PACK_EXTENSION_ERROR (22) for a bad key or manifest and explains
    # itself on stderr. Both the code and that explanation must reach the operator: an exit code
    # alone does not say which of the two went wrong.
    $failChrome = Join-Path $work 'fake-chrome-fail.cmd'
    Set-Content -LiteralPath $failChrome -Encoding ASCII -Value @(
        '@echo off',
        'echo Extension packing failed: private key is invalid 1>&2',
        'exit /b 22')
    Assert-Throws {
        Invoke-ChromePack -ChromePath $failChrome -StageDir $stage -KeyPath $keyStub `
            -WorkDir $work -TimeoutMs 30000
    } 'exit 22' "a nonzero Chrome exit must fail the pack"
    Assert-Throws {
        Invoke-ChromePack -ChromePath $failChrome -StageDir $stage -KeyPath $keyStub `
            -WorkDir $work -TimeoutMs 30000
    } 'private key is invalid' "Chrome's own account of the failure must reach the operator"

    # A clean exit that produced nothing is the case that used to copy a stale artifact forward.
    $silentChrome = Join-Path $work 'fake-chrome-silent.cmd'
    Set-Content -LiteralPath $silentChrome -Encoding ASCII -Value @('@echo off', 'exit /b 0')
    Assert-Throws {
        Invoke-ChromePack -ChromePath $silentChrome -StageDir $stage -KeyPath $keyStub `
            -WorkDir $work -TimeoutMs 30000
    } 'did not produce a CRX' "a Chrome run that wrote no CRX must fail the pack"

    # A wedged Chrome must be bounded AND killed as a TREE. The fake starts a genuinely separate
    # child process ("start", not "call", so it survives its parent) which writes a marker after a
    # delay, then hangs itself. Killing only the launcher leaves that child running -- still
    # holding the staging directory the cleanup is about to delete -- and the marker appears.
    $marker = Join-Path $work 'orphan-marker.txt'
    $hangChild = Join-Path $work 'fake-chrome-child.cmd'
    Set-Content -LiteralPath $hangChild -Encoding ASCII -Value @(
        '@echo off',
        'ping -n 5 127.0.0.1 >nul',
        "echo orphan-survived> ""$marker""")
    $hangChrome = Join-Path $work 'fake-chrome-hang.cmd'
    Set-Content -LiteralPath $hangChrome -Encoding ASCII -Value @(
        '@echo off',
        "start """" /b ""$hangChild""",
        'ping -n 30 127.0.0.1 >nul')
    Assert-Throws {
        Invoke-ChromePack -ChromePath $hangChrome -StageDir $stage -KeyPath $keyStub `
            -WorkDir $work -TimeoutMs 1000
    } 'did not exit within' "a Chrome that never exits must be bounded by the timeout"
    Start-Sleep -Seconds 6
    Assert-That (-not (Test-Path -LiteralPath $marker)) `
        "the timeout must kill Chrome's whole process tree, not just the launcher"

    # 11. The committed CRX must match the source it was packed from. A CRX is a signed blob,
    #     so editing browser/extension changes what a developer loads unpacked while every
    #     customer keeps running whatever was last packed -- and nothing checked that before
    #     this case. It caught a real 76-character drift the first time it ran.
    #     Line endings are normalized: core.autocrlf rewrites the working copy on checkout
    #     while the CRX is marked binary in .gitattributes and keeps the bytes it was packed
    #     with, so a byte-exact compare would fail on eol settings rather than on staleness.
    $archiveOffset = 12 + [long][BitConverter]::ToUInt32($good, 8)
    $crxZipPath = Join-Path $work 'committed-archive.zip'
    [System.IO.File]::WriteAllBytes($crxZipPath, [byte[]]$good[$archiveOffset..($good.Length - 1)])
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $crxZip = [System.IO.Compression.ZipFile]::OpenRead($crxZipPath)
    try {
        $packedNames = @($crxZip.Entries | ForEach-Object { $_.FullName } | Sort-Object)
        $sourceNames = @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'browser\extension') -File |
            ForEach-Object { $_.Name } | Sort-Object)
        Assert-That (($packedNames -join ',') -eq ($sourceNames -join ',')) `
            ("the committed CRX must contain exactly the extension's source files -- packed " +
             "[$($packedNames -join ', ')], source [$($sourceNames -join ', ')]. A file added " +
             "to browser/extension reaches no customer until the CRX is repacked.")
        foreach ($name in $sourceNames) {
            $entry = $crxZip.Entries | Where-Object { $_.FullName -eq $name }
            if (-not $entry) { continue }
            $entryStream = $entry.Open()
            $entryBytes = New-Object System.IO.MemoryStream
            try { $entryStream.CopyTo($entryBytes) } finally { $entryStream.Dispose() }
            $packedText = [System.Text.Encoding]::UTF8.GetString($entryBytes.ToArray()) -replace "`r`n", "`n"
            $sourceText = [System.IO.File]::ReadAllText(
                (Join-Path $ProjectRoot "browser\extension\$name")) -replace "`r`n", "`n"
            Assert-That ($packedText -eq $sourceText) `
                ("browser/dist/sak_browser_control.crx is STALE: its $name differs from " +
                 "browser/extension/$name. Re-run browser/pack-extension.ps1 -- a source edit " +
                 "does not reach a customer until the signed CRX is repacked.")
        }
    } finally {
        $crxZip.Dispose()
    }

    # 12. Chrome's own account of a failure must reach the operator.
    $log = Join-Path $work 'pack-stdout.txt'
    Set-Content -LiteralPath $log -Value 'Extension packing failed: private key is invalid' -NoNewline
    Assert-That ((Get-ProcessDiagnostics -LogPaths @($log, (Join-Path $work 'missing.txt'))) -match 'private key is invalid') `
        "chrome's stdout must be carried into the failure message"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

if ($script:Failures -gt 0) {
    Write-Error ("test_pack_extension: {0} of {1} checks failed." -f $script:Failures, $script:Checks)
    exit 1
}

Write-Host ("test_pack_extension: {0} checks passed." -f $script:Checks)
exit 0
