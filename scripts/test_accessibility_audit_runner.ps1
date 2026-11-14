<#
.SYNOPSIS
    The accessibility runtime audit says why a program that refuses to start did, and the release
    readiness gate audits the package that ships, from a folder that holds the package only.

.DESCRIPTION
    The v0.9.3-rc.1 release dry run (2026-11-14) stopped at its readiness gate with "Accessibility
    runtime audit failed with exit code 1: No audit output written. STDOUT: System.Object[] STDERR:
    System.Object[]". The gate ran build/Release/sak_utility.exe, which holds the test programs, the
    release zip and the clean-*-extract folders. A hosted runner is elevated, and an elevated S.A.K.
    checks every program file in its folder and refuses to start while one is not part of S.A.K.,
    before it writes a line of the audit; its "Fatal: ..." reason went to stderr, and the script
    printed the stderr array as "System.Object[]".

    A stand-in sak_utility.exe (compiled here by Windows PowerShell's Add-Type) refuses the same way
    when a stray program sits in its folder, writing a "Fatal:" line to stderr, and otherwise writes
    an OK audit and where it ran. Nothing here starts S.A.K. itself.
      1. A refusal reaches the failure text: the "Fatal:" line, and no "System.Object[]". A program
         that exits 0 having written no audit fails the check.
      2. With -PackageRoot the audit runs a copy of the package in a folder of its own, removes it,
         and leaves the package as it was; a package program older than HEAD's commit is refused.
      3. check_release_readiness.ps1 passes the package to the audit, after the signatures of a
         signed package are checked.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptsRoot = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptsRoot
$AuditScript = Join-Path $ScriptsRoot 'check_accessibility_patterns.ps1'

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )
    if (-not $Condition) { throw $Message }
}

$StandInSource = @'
using System;
using System.IO;
using System.Linq;

public static class StandIn {
    public static int Main(string[] args) {
        string dir = Path.GetDirectoryName(System.Reflection.Assembly.GetEntryAssembly().Location);
        string evidence = Environment.GetEnvironmentVariable("SAK_AUDIT_STANDIN_EVIDENCE");
        // As an elevated S.A.K. does: no start while a program in its folder is not part of it.
        string stray = Directory.GetFiles(dir, "*.exe", SearchOption.AllDirectories)
            .FirstOrDefault(f => !string.Equals(f, Path.Combine(dir, "sak_utility.exe"),
                                                StringComparison.OrdinalIgnoreCase));
        if (stray != null) {
            Console.Out.WriteLine("stand-in stdout before the refusal");
            Console.Error.WriteLine("Fatal: S.A.K. won't run as administrator while " + stray +
                                    " is in its folder; it isn't part of S.A.K. (it isn't part of S.A.K.)");
            return 1;
        }
        if (Environment.GetEnvironmentVariable("SAK_AUDIT_STANDIN_SILENT") == "1") {
            return 0;  // ends cleanly having written no audit at all
        }
        const string prefix = "--accessibility-audit-output=";
        string output = args.Where(a => a.StartsWith(prefix)).Select(a => a.Substring(prefix.Length))
                            .FirstOrDefault();
        string[] entries = Directory.GetFileSystemEntries(dir, "*", SearchOption.AllDirectories)
                               .Select(e => "ENTRY " + e.Substring(dir.Length + 1)).ToArray();
        string[] lines = new[] { "SAK_ACCESSIBILITY_AUDIT_OK stand-in", "RAN-FROM " + dir }
                             .Concat(entries).ToArray();
        File.WriteAllLines(output, lines);
        if (!string.IsNullOrEmpty(evidence)) { File.WriteAllLines(evidence, lines); }
        // As the app does: its runtime data beside it.
        Directory.CreateDirectory(Path.Combine(dir, "data", "logs"));
        return 0;
    }
}
'@

# Compiles the stand-in with Windows PowerShell 5.1, whose Add-Type writes a console program.
function New-StandInProgram([string]$Path) {
    $source = Join-Path (Split-Path -Parent $Path) 'standin.cs'
    [IO.File]::WriteAllText($source, $StandInSource)
    $compile = Join-Path (Split-Path -Parent $Path) 'compile.ps1'
    [IO.File]::WriteAllText($compile, @"
`$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition ([IO.File]::ReadAllText('$source')) -Language CSharp ``
    -OutputAssembly '$Path' -OutputType ConsoleApplication -ReferencedAssemblies System.Core
"@)
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -NonInteractive -File $compile
    Assert-Condition ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $Path -PathType Leaf)) (
        "The stand-in did not compile (exit $LASTEXITCODE)")
    Remove-Item -LiteralPath $source, $compile -Force
}

# Runs the audit script and returns what it threw, or '' when it passed.
function Invoke-Audit([hashtable]$Arguments) {
    try {
        & $AuditScript -Root $RepoRoot @Arguments | Out-Host
        return ''
    } catch {
        return $_.Exception.Message
    }
}

function Get-RelativeEntries([string]$Folder) {
    return @(Get-ChildItem -LiteralPath $Folder -Recurse -Force | ForEach-Object {
            $_.FullName.Substring($Folder.Length + 1)
        } | Sort-Object)
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('sak-audit-runner-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$previousEvidence = $env:SAK_AUDIT_STANDIN_EVIDENCE
try {
    $built = Join-Path $work 'built'
    New-Item -ItemType Directory -Path $built | Out-Null
    $standIn = Join-Path $built 'sak_utility.exe'
    New-StandInProgram $standIn

    # 1. build/Release as the release job had it: the program beside one that is not part of it.
    $buildFolder = Join-Path $work 'build-release'
    New-Item -ItemType Directory -Path $buildFolder | Out-Null
    Copy-Item -LiteralPath $standIn -Destination (Join-Path $buildFolder 'sak_utility.exe')
    Copy-Item -LiteralPath $standIn -Destination (Join-Path $buildFolder 'test_stray.exe')
    $refused = Invoke-Audit @{ ExePath = (Join-Path $buildFolder 'sak_utility.exe') }
    Assert-Condition ($refused -match 'Fatal: S\.A\.K\. won''t run as administrator while .*test_stray\.exe is in its folder') (
        "The refusal's Fatal line did not reach the failure text:`n$refused")
    Assert-Condition ($refused -match 'stand-in stdout before the refusal') (
        "The program's stdout did not reach the failure text:`n$refused")
    Assert-Condition ($refused -notmatch 'System\.Object\[\]') (
        "The failure text printed an array as System.Object[]:`n$refused")
    Assert-Condition ($refused -match 'failed with exit code 1:') (
        "A program that refused was not reported as a failed audit:`n$refused")
    Write-Host 'PASS a program that refuses to start says why in the failure text'

    # A program that ends cleanly without writing an audit has not passed one.
    $env:SAK_AUDIT_STANDIN_SILENT = '1'
    try {
        $silent = Invoke-Audit @{ ExePath = $standIn }
    } finally {
        $env:SAK_AUDIT_STANDIN_SILENT = $null
    }
    Assert-Condition ($silent -match 'failed with exit code 0:\s+No audit output written\.') (
        "A program that wrote no audit was not refused:`n$silent")
    Write-Host 'PASS a program that writes no audit fails the check, though it exits 0'

    # 2. The package that ships: the audit runs a copy of it, in a folder of its own.
    $package = Join-Path $work 'clean-readiness-extract'
    New-Item -ItemType Directory -Path (Join-Path $package 'platforms') | Out-Null
    Copy-Item -LiteralPath $standIn -Destination (Join-Path $package 'sak_utility.exe')
    [IO.File]::WriteAllText((Join-Path $package 'platforms\qwindows.dll'), 'a plugin')
    $before = Get-RelativeEntries $package
    $evidence = Join-Path $work 'evidence.txt'
    $env:SAK_AUDIT_STANDIN_EVIDENCE = $evidence
    $passed = Invoke-Audit @{ PackageRoot = $package }
    Assert-Condition ($passed -eq '') "The audit of a clean package failed:`n$passed"
    $said = @(Get-Content -LiteralPath $evidence)
    $ranFrom = ($said | Where-Object { $_ -like 'RAN-FROM *' } | Select-Object -First 1).Substring(9)
    Assert-Condition (-not [string]::Equals($ranFrom, $package, [StringComparison]::OrdinalIgnoreCase)) (
        "The audit ran the package in place: $ranFrom")
    $entries = @($said | Where-Object { $_ -like 'ENTRY *' } | ForEach-Object { $_.Substring(6) } | Sort-Object)
    Assert-Condition (($entries -join '|') -eq ($before -join '|')) (
        "The folder the audit ran in held more or less than the package: $($entries -join ', ')")
    Assert-Condition (-not (Test-Path -LiteralPath $ranFrom)) "The audit's package copy was left: $ranFrom"
    Assert-Condition (((Get-RelativeEntries $package) -join '|') -eq ($before -join '|')) (
        'The audit changed the package (its runtime data landed there)')
    Write-Host 'PASS the package is audited from a copy that holds it only, and is left as it was'

    (Get-Item -LiteralPath (Join-Path $package 'sak_utility.exe')).LastWriteTimeUtc =
        [DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
    $stale = Invoke-Audit @{ PackageRoot = $package }
    Assert-Condition ($stale -match "older than HEAD's commit") "A package older than HEAD was not refused:`n$stale"
    Write-Host "PASS a package program older than HEAD's commit is refused"

    # 3. The readiness gate passes the package to the audit, after a signed package's signatures.
    $readiness = Get-Content -LiteralPath (Join-Path $ScriptsRoot 'check_release_readiness.ps1') -Raw
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($readiness, [ref]$tokens, [ref]$errors)
    Assert-Condition (@($errors).Count -eq 0) 'check_release_readiness.ps1 does not parse'
    $calls = @($ast.FindAll({ param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq 'Invoke-IsolatedPowerShellScript' -and
                $node.Extent.Text -match 'check_accessibility_patterns\.ps1'
            }, $true))
    Assert-Condition ($calls.Count -eq 1) 'The readiness gate does not run the accessibility audit once'
    $auditCall = $calls[0]
    Assert-Condition ($auditCall.Extent.Text -match '-Arguments\s+\$auditArguments') (
        'The readiness gate runs the audit without its arguments')
    $packageArguments = @($ast.FindAll({ param($node)
                $node -is [Management.Automation.Language.AssignmentStatementAst] -and
                $node.Left.Extent.Text -eq '$auditArguments' -and
                $node.Right.Extent.Text -match '"-PackageRoot",\s*\$package\.Path'
            }, $true))
    Assert-Condition ($packageArguments.Count -eq 1) 'The readiness gate does not pass -PackageRoot to the audit'
    $signatures = @($ast.FindAll({ param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                $node.CommandElements[0].Extent.Text -match 'verify_authenticode_signatures\.ps1$'
            }, $true))
    Assert-Condition ($signatures.Count -eq 1 -and
        $signatures[0].Extent.StartOffset -lt $auditCall.Extent.StartOffset) (
        "A signed package's signatures are not checked before the audit runs it")
    Write-Host 'PASS the readiness gate audits the package, after its signatures'
} finally {
    $env:SAK_AUDIT_STANDIN_EVIDENCE = $previousEvidence
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
