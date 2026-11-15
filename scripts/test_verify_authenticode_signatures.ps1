# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Proves the release publisher pin wherever the release checks it, on real signed binaries.

.DESCRIPTION
    A signature cannot be faked for a test, and a mocked one proves nothing, so the fixtures are
    real Authenticode-signed files copied under first-party names: the bundled choco.exe, signed by
    Chocolatey Software, and a Windows system DLL, signed by Microsoft.

    The pin is written once, $SakPublisherSubject in authenticode_signer.ps1, and checked in three
    places: verify_authenticode_signatures.ps1 (the verify and publish jobs, and the signed readiness
    gate), the filesystem tool re-pin, and release.yml's sign job, which has no checkout and takes it
    from the build job's "publisher" output. Each is run here as it runs in the release, and each must
    accept a signer's full subject and refuse its CN and O alone and any subject that differs in
    locality, state or country: v0.9.3-rc.2 was signed by a certificate whose subject carries those,
    and a pin of CN and O alone refused every first-party binary at verify.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
. (Join-Path $ProjectRoot "scripts\authenticode_signer.ps1")
$Verifier = Join-Path $ProjectRoot "scripts\verify_authenticode_signatures.ps1"
$Repin = Join-Path $ProjectRoot ".github\release-tools\repin_signed_filesystem_tools.ps1"
$Workflow = Join-Path $ProjectRoot ".github\workflows\release.yml"
$ChocoSource = Join-Path $ProjectRoot "tools\chocolatey\choco.exe"
# This host, not a hard-coded edition: a child of the other edition inherits a PSModulePath that
# cannot load Get-AuthenticodeSignature.
$HostExecutable = (Get-Process -Id $PID).Path
$FirstParty = @(
    "sak_utility.exe",
    "sak_elevated_helper.exe",
    "sak_apfs_writer_cli.exe",
    "sak_hfs_writer_cli.exe"
)

function Get-Subject([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne "Valid") {
        throw "Fixture is not validly signed: $Path ($($signature.Status))"
    }
    return $signature.SignerCertificate.Subject
}

function New-ScratchFolder {
    $root = Join-Path ([IO.Path]::GetTempPath()) ("sak-signature-test-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $root | Out-Null
    $script:fixtures += $root
    return $root
}

function New-PackageFixture {
    param([string[]]$Names = $FirstParty)
    $root = New-ScratchFolder
    foreach ($name in $Names) {
        Copy-Item -LiteralPath $ChocoSource -Destination (Join-Path $root $name)
    }
    return $root
}

# A script in its own process, as every caller runs it: only the exit code and text count.
function Invoke-Child {
    param([string]$Script, [string[]]$Arguments = @())
    $output = & $HostExecutable -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Script @Arguments 2>&1 |
        Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Invoke-Verifier {
    param([string]$Root, [string]$ExpectedPublisher = "")
    $arguments = @("-RootDir", $Root)
    if ($ExpectedPublisher) {
        $arguments += @("-ExpectedPublisher", $ExpectedPublisher)
    }
    return Invoke-Child $Verifier $arguments
}

function Assert-Refused($Result, [string]$Expected, [string]$Case) {
    if ($Result.ExitCode -eq 0) {
        throw "${Case}: accepted.`n$($Result.Output)"
    }
    # A redirected error record wraps at the child's width, behind a "|" gutter, so the reason is
    # looked for with whitespace and gutters taken out of both.
    $flat = $Result.Output -replace '[\s|]', ''
    if ($flat.IndexOf(($Expected -replace '[\s|]', ''), [StringComparison]::Ordinal) -lt 0) {
        throw "${Case}: refused, but not because of '$Expected'.`n$($Result.Output)"
    }
}

function Assert-Accepted($Result, [string]$Case) {
    if ($Result.ExitCode -ne 0) {
        throw "${Case}: refused.`n$($Result.Output)"
    }
}

# The subjects a pin must refuse, made from a signer's full subject: its CN and O alone (the
# v0.9.3-rc.2 pin had that shape), and the same subject in another locality, state or country.
function Get-RefusedSubjects([string]$Subject) {
    $at = $Subject.IndexOf(", L=", [StringComparison]::Ordinal)
    if ($at -lt 0 -or $Subject -notmatch ', S=[^,]+, C=[A-Z]{2}$') {
        throw "The fixture's subject carries no locality, state and country to vary: $Subject"
    }
    return [ordered]@{
        "CN and O only"     = $Subject.Substring(0, $at)
        "another locality"  = [regex]::Replace($Subject, ', L=[^,]+', ', L=Elsewhere')
        "another state"     = [regex]::Replace($Subject, ', S=[^,]+', ', S=Elsewhere')
        "another country"   = [regex]::Replace($Subject, ', C=[A-Z]{2}$', ', C=ZZ')
    }
}

# One step's run script and env from a workflow file, as the runner sees them: the block under
# "run: |" with its indentation removed. Line-based, for the shape release.yml uses.
function Get-WorkflowStep([string]$Job, [string]$Name) {
    $lines = [IO.File]::ReadAllLines($Workflow)
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -ceq "  ${Job}:") { $start = $i; break }
    }
    if ($start -lt 0) { throw "release.yml has no job '$Job'" }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^  [A-Za-z0-9_-]+:\s*$') { $end = $i; break }
    }
    $at = -1
    for ($i = $start; $i -lt $end; $i++) {
        if ($lines[$i] -ceq "    - name: $Name") { $at = $i; break }
    }
    if ($at -lt 0) { throw "release.yml's $Job job has no step '$Name'" }
    $stepEnv = @{}
    $run = $null
    for ($i = $at + 1; $i -lt $end -and $lines[$i] -notmatch '^    - '; $i++) {
        if ($lines[$i] -ceq "      env:") {
            for ($j = $i + 1; $j -lt $end -and $lines[$j] -match '^        ([A-Za-z_][A-Za-z0-9_]*): (.*)$'; $j++) {
                $stepEnv[$Matches[1]] = $Matches[2]
            }
        }
        if ($lines[$i] -ceq "      run: |") {
            $body = @()
            for ($j = $i + 1; $j -lt $end -and ($lines[$j] -match '^        ' -or $lines[$j].Trim() -eq ''); $j++) {
                $body += if ($lines[$j].Length -ge 8) { $lines[$j].Substring(8) } else { "" }
            }
            $run = ($body -join "`r`n").TrimEnd()
        }
    }
    if ($null -eq $run) { throw "release.yml's step '$Name' has no 'run: |' block" }
    return [pscustomobject]@{ Run = $run; Env = $stepEnv }
}

# A shell: pwsh step as the runner runs it: Stop, the body, then the exit code of the last native
# command, in the given folder with the given environment.
function Invoke-WorkflowStep($Step, [hashtable]$Environment, [string]$WorkingDirectory) {
    $file = Join-Path (New-ScratchFolder) "step.ps1"
    $text = "`$ErrorActionPreference = 'stop'`r`n" + $Step.Run +
        "`r`nif ((Test-Path -LiteralPath variable:\LASTEXITCODE)) { exit `$LASTEXITCODE }`r`n"
    [IO.File]::WriteAllText($file, $text, [Text.UTF8Encoding]::new($true))
    $saved = @{}
    foreach ($key in $Environment.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, $Environment[$key])
    }
    Push-Location -LiteralPath $WorkingDirectory
    try {
        return Invoke-Child $file
    } finally {
        Pop-Location
        foreach ($key in $saved.Keys) {
            [Environment]::SetEnvironmentVariable($key, $saved[$key])
        }
    }
}

# The pin is a certificate's full subject: the maintainer's CN and O, and the locality, state and
# country the certificate carries. The real value is proved against the signed package (see
# docs/RELEASE_READINESS.md); this keeps it from going back to CN and O alone.
if ($SakPublisherSubject -cnotmatch '^CN=Randy Northrup, O=Randy Northrup, L=[^,]+, S=[^,]+, C=[A-Z]{2}$') {
    throw "The release publisher is not a full certificate subject: '$SakPublisherSubject'"
}

$chocoSubject = Get-Subject $ChocoSource
$refusedSubjects = Get-RefusedSubjects $chocoSubject
# Whichever of these this Windows has signed by someone other than Chocolatey.
$MicrosoftSource = @("ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll", "ntdll.dll", "kernel32.dll") |
    ForEach-Object { Join-Path $env:windir "System32\$_" } |
    Where-Object {
        (Test-Path -LiteralPath $_ -PathType Leaf) -and
        (Get-AuthenticodeSignature -LiteralPath $_).Status -eq "Valid"
    } |
    Select-Object -First 1
if ([string]::IsNullOrEmpty($MicrosoftSource)) {
    throw "No validly signed system DLL found to use as the second publisher's fixture"
}
if ($chocoSubject -ceq (Get-Subject $MicrosoftSource)) {
    throw "The two fixtures must come from different publishers"
}

$fixtures = @()
try {
    # -- 1. verify_authenticode_signatures.ps1 ------------------------------------------------------
    # The default pin is the release publisher: first-party binaries signed by anyone else are
    # refused, however valid.
    $root = New-PackageFixture
    Assert-Refused (Invoke-Verifier $root) "not signed by '$SakPublisherSubject'" `
        "verifier: wrong publisher under the default pin"

    # The same files pass once the pin is their signer's full subject: the status check is not the
    # reason for the refusal above.
    Assert-Accepted (Invoke-Verifier $root $chocoSubject) "verifier: the signer's full subject"

    # The signer's CN and O alone, or its subject anywhere else, is not the signer.
    foreach ($case in $refusedSubjects.GetEnumerator()) {
        Assert-Refused (Invoke-Verifier $root $case.Value) "not signed by" "verifier: $($case.Key)"
    }

    # One first-party binary from another publisher is enough.
    $root = New-PackageFixture
    Copy-Item -LiteralPath $MicrosoftSource -Destination (Join-Path $root "sak_extra.dll")
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "sak_extra.dll" "verifier: any sak_ binary is pinned"

    # Bundled third-party tools keep their own vendor's signature: choco.exe must stay
    # Chocolatey-signed, so a pin over the whole package could never pass.
    $root = New-PackageFixture
    New-Item -ItemType Directory -Path (Join-Path $root "tools") | Out-Null
    Copy-Item -LiteralPath $MicrosoftSource -Destination (Join-Path $root "tools\vendor.dll")
    Assert-Accepted (Invoke-Verifier $root $chocoSubject) "verifier: third-party binary with its own signature"

    # A tree without the package's binaries is not the package.
    $root = New-PackageFixture -Names @("sak_utility.exe")
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "First-party binaries missing" "verifier: missing binaries"

    # Unsigned binaries are still refused.
    $root = New-PackageFixture
    [IO.File]::WriteAllBytes((Join-Path $root "unsigned.dll"), [byte[]](0x4D, 0x5A, 0, 0))
    Assert-Refused (Invoke-Verifier $root $chocoSubject) "Unsigned or invalid" "verifier: unsigned binary"

    # -- 2. The filesystem tool re-pin --------------------------------------------------------------
    # A package whose one pinned tool changed when it was signed: the manifest pins other bytes, and
    # the file is a signed binary. The re-pin accepts the change only from the release publisher.
    function New-RepinFixture {
        $root = New-ScratchFolder
        $tools = Join-Path $root "package\tools\filesystem"
        New-Item -ItemType Directory -Path (Join-Path $tools "fixture") | Out-Null
        Copy-Item -LiteralPath $ChocoSource -Destination (Join-Path $tools "fixture\tool.exe")
        $upstream = ("ab" * 32)
        $manifest = '{ "tools": [ { "id": "fixture", "relative_path": "fixture/tool.exe", "binary_sha256": "' +
            $upstream + '", "runtime_files": [] } ] }'
        [IO.File]::WriteAllText((Join-Path $tools "manifest.json"), $manifest, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $root "repository-manifest.json"), $manifest, [Text.UTF8Encoding]::new($false))
        return $root
    }
    function Invoke-Repin([string]$Root, [string]$ExpectedPublisher = "") {
        $arguments = @("-PackageRoot", (Join-Path $Root "package"),
            "-RepositoryManifest", (Join-Path $Root "repository-manifest.json"),
            "-MappingPath", (Join-Path $Root "mapping.txt"), "-Version", "0.0.0")
        if ($ExpectedPublisher) {
            $arguments += @("-ExpectedPublisher", $ExpectedPublisher)
        }
        return Invoke-Child $Repin $arguments
    }

    Assert-Refused (Invoke-Repin (New-RepinFixture)) "expected '$SakPublisherSubject'" `
        "re-pin: wrong publisher under the default pin"
    $root = New-RepinFixture
    Assert-Accepted (Invoke-Repin $root $chocoSubject) "re-pin: the signer's full subject"
    $repinned = [IO.File]::ReadAllText((Join-Path $root "package\tools\filesystem\manifest.json"))
    $signedHash = (Get-FileHash -LiteralPath $ChocoSource -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($repinned -notmatch $signedHash) {
        throw "re-pin: accepted, but the packaged manifest does not pin the signed file`n$repinned"
    }
    foreach ($case in $refusedSubjects.GetEnumerator()) {
        Assert-Refused (Invoke-Repin (New-RepinFixture) $case.Value) "without a valid release signature" `
            "re-pin: $($case.Key)"
    }

    # -- 3. release.yml: the build job hands the pin to the sign job, which checks what it signed ----
    $workflowText = [IO.File]::ReadAllText($Workflow)
    if ($workflowText -notmatch '(?m)^      publisher: \$\{\{ steps\.publisher\.outputs\.publisher \}\}\r?$') {
        throw "release.yml: the build job does not hand on the publisher step's output"
    }
    $output = Join-Path (New-ScratchFolder) "github_output.txt"
    New-Item -ItemType File -Path $output | Out-Null
    $publisherStep = Get-WorkflowStep "build" "Read the release publisher"
    Assert-Accepted (Invoke-WorkflowStep $publisherStep @{ GITHUB_OUTPUT = $output } $ProjectRoot) `
        "release.yml: the build job's publisher step"
    $handedOn = @(Get-Content -LiteralPath $output -Encoding UTF8 | Where-Object { $_ -like "publisher=*" })
    if ($handedOn.Count -ne 1 -or $handedOn[0] -cne "publisher=$SakPublisherSubject") {
        throw "release.yml: the build job hands on '$($handedOn -join ' | ')', not the release publisher"
    }

    $signCheck = Get-WorkflowStep "sign" "Check the signatures"
    if ($signCheck.Env["PUBLISHER"] -cne '${{ needs.build.outputs.publisher }}') {
        throw "release.yml: the sign job's check does not take the build job's publisher: '$($signCheck.Env["PUBLISHER"])'"
    }
    function New-SignedPackageFixture([string[]]$Catalog = $FirstParty) {
        $runnerTemp = New-ScratchFolder
        $package = Join-Path $runnerTemp "sak-package"
        New-Item -ItemType Directory -Path (Join-Path $package "tools") | Out-Null
        foreach ($name in $FirstParty + "tools\vendor.exe") {
            Copy-Item -LiteralPath $ChocoSource -Destination (Join-Path $package $name)
        }
        $entries = @($Catalog + "tools\vendor.exe" | ForEach-Object { ".\$_" })
        [IO.File]::WriteAllLines((Join-Path $package "signing-catalog.txt"), [string[]]$entries)
        return $runnerTemp
    }
    function Invoke-SignCheck([string]$RunnerTemp, [string]$Publisher) {
        return Invoke-WorkflowStep $signCheck @{ RUNNER_TEMP = $RunnerTemp; PUBLISHER = $Publisher } $RunnerTemp
    }

    $runnerTemp = New-SignedPackageFixture
    Assert-Accepted (Invoke-SignCheck $runnerTemp $chocoSubject) "sign job: the signer's full subject"
    Assert-Refused (Invoke-SignCheck $runnerTemp $SakPublisherSubject) "not by the release publisher" `
        "sign job: another signer under the release publisher"
    foreach ($case in $refusedSubjects.GetEnumerator()) {
        Assert-Refused (Invoke-SignCheck $runnerTemp $case.Value) "not by the release publisher" "sign job: $($case.Key)"
    }
    Assert-Refused (Invoke-SignCheck $runnerTemp "") "handed on no release publisher" "sign job: no publisher"
    $runnerTemp = New-SignedPackageFixture -Catalog @($FirstParty | Where-Object { $_ -ne "sak_utility.exe" })
    Assert-Refused (Invoke-SignCheck $runnerTemp $chocoSubject) "does not name sak_utility.exe" `
        "sign job: a catalog without sak_utility.exe"
}
finally {
    foreach ($fixture in $fixtures) {
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Release publisher pin tests passed."
