# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

[CmdletBinding()]
param(
    [switch]$ContractOnly,
    [switch]$ArchiveOnly,
    [switch]$SkipArchiveContracts,
    [string]$ArchiveScriptPath = "",
    [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
if ($ArchiveOnly -and $SkipArchiveContracts) {
    throw 'ArchiveOnly and SkipArchiveContracts cannot be combined'
}
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
. (Join-Path $ProjectRoot "scripts\path_reparse_guard.ps1")
$requestedBuildDirectory = if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    Join-Path $ProjectRoot "build"
} elseif ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $ProjectRoot $BuildDirectory
}
$resolvedBuildDirectory = [IO.Path]::GetFullPath($requestedBuildDirectory)
$allowedBuildDirectory = [IO.Path]::GetFullPath((Join-Path $ProjectRoot "build"))
if ($resolvedBuildDirectory -ne $allowedBuildDirectory -and
    -not $resolvedBuildDirectory.StartsWith(
        $allowedBuildDirectory + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Portable-stage test build directory must stay under the repository build tree"
}
[void](Assert-StageDirectDirectoryPath -Path $resolvedBuildDirectory `
    -Description 'Portable-stage test build directory' -RequireExisting $true)
$capture = Join-Path $ProjectRoot "scripts\process_capture.ps1"
$stageScript = Join-Path $ProjectRoot "scripts\stage_portable_release.ps1"
$buildRoot = Join-Path $resolvedBuildDirectory "Release"
[void](Assert-StageDirectDirectoryPath -Path $buildRoot `
    -Description 'Portable-stage test Release directory' -RequireExisting $true)
$packageName = "SAK-Stage-Test-$PID"
$packageRoot = Join-Path $buildRoot $packageName
$deployScript = Join-Path $ProjectRoot "scripts\deploy_qt_runtime.ps1"
$classifierScript = Join-Path $ProjectRoot "scripts\windows_runtime_dlls.ps1"
$archiveScript = if ([string]::IsNullOrWhiteSpace($ArchiveScriptPath)) {
    Join-Path $ProjectRoot "scripts\create_release_archive.ps1"
} else { [IO.Path]::GetFullPath($ArchiveScriptPath) }

function Assert-StageContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Expect-StageFailure {
    param([scriptblock]$Body, [string]$Expected)
    $message = ""
    try { & $Body | Out-Null } catch { $message = $_.Exception.Message }
    Assert-StageContract ($message.Contains($Expected)) "Expected '$Expected'; got '$message'"
}
function Get-StageAst {
    param([string]$Text)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    Assert-StageContract ($errors.Count -eq 0) "Stage source parse failed"
    return $ast
}
function Import-StageFunctions {
    param([string]$FunctionSource, [string[]]$FunctionNames)
    $functionAst = Get-StageAst $FunctionSource
    foreach ($functionName in $FunctionNames) {
        $nodes = @($functionAst.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $functionName
        }, $true))
        Assert-StageContract ($nodes.Count -eq 1) "Missing stage function: $functionName"
        . ([scriptblock]::Create($nodes[0].Extent.Text))
    }
}
function New-StageContractDirectory {
    param([string]$Root)
    $path = Join-Path $Root ([guid]::NewGuid().ToString("D"))
    New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
    return $path
}
function Copy-StageFixtureBuildInputs([string]$SourceRoot, [string]$DestinationRoot) {
    # Clone only input classes the stager can consume. Copying the whole Release
    # tree also cloned every historical staged package and ZIP, making this
    # regression grow by gigabytes and approach its fixed CTest deadline.
    [void](Get-StageDirectTreeItem -Root $SourceRoot -Description 'Portable-stage fixture source')
    [void](Assert-StageDirectDirectoryPath -Path $DestinationRoot `
        -Description 'Portable-stage fixture destination' -RequireExisting $true)
    $runtimeDirectories = @('platforms', 'styles', 'imageformats', 'iconengines',
        'tls', 'networkinformation', 'generic', 'tools', 'data')
    $runtimeFiles = @('sak_utility.exe', 'sak_elevated_helper.exe',
        'sak_apfs_writer_cli.exe', 'sak_hfs_writer_cli.exe',
        'sak_browser_control.crx', 'sak_splash.png', 'icon.ico')
    foreach ($sourceItem in (Get-ChildItem -LiteralPath $SourceRoot -Force -ErrorAction Stop)) {
        if ($sourceItem.PSIsContainer) {
            if ($runtimeDirectories -notcontains $sourceItem.Name) { continue }
        } elseif ($runtimeFiles -notcontains $sourceItem.Name -and
            $sourceItem.Extension -ine '.dll') {
            continue
        }
        Copy-Item -LiteralPath $sourceItem.FullName -Destination $DestinationRoot -Recurse -Force -ErrorAction Stop
    }
}
function Test-StageFixtureBuildInputs([string]$Root) {
    $fixture = New-StageContractDirectory $Root
    $source = Join-Path $fixture 'source'
    $destination = Join-Path $fixture 'destination'
    New-Item -ItemType Directory -Path $source, $destination -ErrorAction Stop | Out-Null
    foreach ($name in @('platforms', 'tools', 'data', 'SAK-Historical-Package', 'scratch')) {
        New-Item -ItemType Directory -Path (Join-Path $source $name) -ErrorAction Stop | Out-Null
    }
    foreach ($relative in @('sak_utility.exe', 'Qt6Core.dll', 'platforms/qwindows.dll',
            'tools/required.txt', 'data/required.txt',
            'SAK-Historical-Package/sak_utility.exe', 'scratch/obsolete.txt',
            'test_old.exe', 'SAK-Historical-Windows-x64.zip', 'SHA256SUMS.txt')) {
        [IO.File]::WriteAllText((Join-Path $source $relative), "fixture:$relative")
    }
    Copy-StageFixtureBuildInputs $source $destination
    foreach ($relative in @('sak_utility.exe', 'Qt6Core.dll', 'platforms/qwindows.dll',
            'tools/required.txt', 'data/required.txt')) {
        Assert-StageContract (Test-Path -LiteralPath (Join-Path $destination $relative) -PathType Leaf) (
            "Stage fixture omitted runtime input: $relative")
    }
    foreach ($relative in @('SAK-Historical-Package', 'scratch', 'test_old.exe',
            'SAK-Historical-Windows-x64.zip', 'SHA256SUMS.txt')) {
        Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $destination $relative))) (
            "Stage fixture copied irrelevant build artifact: $relative")
    }
    Write-Host 'PASS private stage fixture copies runtime inputs, not historical build artifacts'
}
function Test-StageFixtureCopyMutants([string]$Root) {
    $ast = Get-StageAst (Get-Content -LiteralPath $PSCommandPath -Raw)
    $definitions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Copy-StageFixtureBuildInputs'
    }, $true))
    Assert-StageContract ($definitions.Count -eq 1) 'Stage fixture copy mutation target is not unique'
    $source = $definitions[0].Extent.Text
    $cases = @(
        @{ name = 'historical-package-directory'; from = 'if ($runtimeDirectories -notcontains $sourceItem.Name) { continue }'; to = 'if ($false) { continue }'; expected = 'SAK-Historical-Package' },
        @{ name = 'test-executable'; from = '$sourceItem.Extension -ine ''.dll'') {'; to = '$sourceItem.Extension -notin @(''.dll'', ''.exe'')) {'; expected = 'test_old.exe' },
        @{ name = 'historical-zip'; from = '$sourceItem.Extension -ine ''.dll'') {'; to = '$sourceItem.Extension -notin @(''.dll'', ''.zip'')) {'; expected = 'SAK-Historical-Windows-x64.zip' },
        @{ name = 'old-checksum-manifest'; from = 'if ($runtimeFiles -notcontains $sourceItem.Name -and'; to = 'if ($runtimeFiles -notcontains $sourceItem.Name -and $sourceItem.Name -cne ''SHA256SUMS.txt'' -and'; expected = 'SHA256SUMS.txt' }
    )
    foreach ($case in $cases) {
        Assert-StageContract ([regex]::Matches($source, [regex]::Escape($case.from)).Count -eq 1) (
            "Stage fixture copy mutation anchor missing: $($case.name)")
        $mutant = $source.Replace($case.from, $case.to)
        $scope = [scriptblock]::Create(
            "param([string]`$Root)`n$mutant`nTest-StageFixtureBuildInputs `$Root")
        $failure = ''
        try { & $scope $Root | Out-Null } catch { $failure = $_.Exception.Message }
        Assert-StageContract ($failure.Contains($case.expected)) (
            "Stage fixture copy mutant did not hit $($case.expected): $($case.name): $failure")
        Write-Host "KILLED stage fixture copy $($case.name)"
    }
}
function Test-StageReparseBoundary {
    param([string]$Root)
    $fixture = New-StageContractDirectory $Root
    $outside = Join-Path $fixture 'outside-target'
    $junction = Join-Path $fixture 'build-junction'
    New-Item -ItemType Directory -Path $outside -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $outside 'Release') -ErrorAction Stop | Out-Null
    $sentinel = Join-Path $outside 'sentinel.bin'
    [IO.File]::WriteAllBytes($sentinel, [byte[]](17, 34, 51, 68, 85))
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
    New-Item -ItemType Junction -Path $junction -Target $outside -ErrorAction Stop | Out-Null
    $nestedJunction = $null
    $targetJunction = $null
    try {
        $hostPath = (Get-Process -Id $PID).Path
        $testResult = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
            '-NoProfile', '-NonInteractive', '-File', $PSCommandPath,
            '-ArchiveOnly', '-BuildDirectory', $junction
        ) -TimeoutSeconds 20 -HiddenWindow
        Assert-StageContract (-not $testResult.timed_out -and $testResult.exit_code -ne 0 -and
            $testResult.stderr.Contains('crosses a reparse point')) (
            'Stage regression accepted a reparse BuildDirectory: ' + $testResult.stderr)
        Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $outside 'test_results'))) (
            'Stage regression wrote through a reparse BuildDirectory')
        if (-not $ArchiveOnly) {
            $stageResult = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', $stageScript,
                '-BuildDir', (Join-Path $junction 'Release'),
                '-PackageName', ('SAK-Stage-Reparse-' + $PID)
            ) -TimeoutSeconds 20 -HiddenWindow
            Assert-StageContract (-not $stageResult.timed_out -and $stageResult.exit_code -ne 0 -and
                $stageResult.stderr.Contains('crosses a reparse point')) (
                'Portable stager accepted a reparse Release ancestor: ' + $stageResult.stderr)
            $directBuild = Join-Path $fixture 'direct-build'
            New-Item -ItemType Directory -Path $directBuild -ErrorAction Stop | Out-Null
            $directRelease = Join-Path $directBuild 'Release'
            New-Item -ItemType Directory -Path $directRelease -ErrorAction Stop | Out-Null
            $nestedPackage = Join-Path $directRelease 'SAK-Nested-Reparse'
            New-Item -ItemType Directory -Path $nestedPackage -ErrorAction Stop | Out-Null
            $nestedJunction = Join-Path $nestedPackage 'nested-junction'
            New-Item -ItemType Junction -Path $nestedJunction -Target $outside -ErrorAction Stop | Out-Null
            $nestedResult = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', $stageScript,
                '-BuildDir', $directRelease, '-PackageName', 'SAK-Nested-Reparse'
            ) -TimeoutSeconds 20 -HiddenWindow
            Assert-StageContract (-not $nestedResult.timed_out -and $nestedResult.exit_code -ne 0 -and
                $nestedResult.stderr.Contains('contains a reparse point')) (
                'Portable stager accepted a nested delete-target junction: ' + $nestedResult.stderr)
            Assert-StageContract (Test-Path -LiteralPath $nestedPackage -PathType Container) (
                'Nested reparse refusal removed the preexisting package')
            $targetJunction = Join-Path $directRelease 'SAK-Target-Reparse'
            New-Item -ItemType Junction -Path $targetJunction -Target $outside -ErrorAction Stop | Out-Null
            $targetResult = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', $stageScript,
                '-BuildDir', $directRelease, '-PackageName', 'SAK-Target-Reparse'
            ) -TimeoutSeconds 20 -HiddenWindow
            Assert-StageContract (-not $targetResult.timed_out -and $targetResult.exit_code -ne 0 -and
                $targetResult.stderr.Contains('Refusing to remove reparse point')) (
                'Portable stager accepted a delete target junction: ' + $targetResult.stderr)
        }
        Assert-StageContract ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ceq $sentinelHash) (
            'Reparse refusal changed the outside sentinel')
    }
    finally {
        foreach ($link in @($nestedJunction, $targetJunction, $junction)) {
            if ([string]::IsNullOrWhiteSpace($link)) { continue }
            $linkItem = Get-Item -LiteralPath $link -Force -ErrorAction Stop
            Assert-StageContract (($linkItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) (
                'Owned junction was replaced before cleanup')
            [IO.Directory]::Delete($link, $false)
        }
        Assert-StageContract ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ceq $sentinelHash) (
            'Junction cleanup changed the outside sentinel')
    }
    Write-Output 'PASS portable stage and test refuse owned build junction without changing outside data'
}
function New-ArchiveContractFixture {
    param([string]$Root)
    $fixtureBuild = New-StageContractDirectory $Root
    $name = "ArchiveContract"
    $package = Join-Path $fixtureBuild $name
    $files = @("sak_utility.exe", "sak_elevated_helper.exe", "data/ai/providers/providers.json",
        "data/ai/app_manifests/windows_defender.json", "data/ai/app_manifests/superantispyware.json",
        "data/ai/app_manifests/windows_sfc.json")
    foreach ($relative in $files) {
        $path = Join-Path $package $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "archive-fixture:$relative")
    }
    return @{ build = $fixtureBuild; package = $package; name = $name; files = $files
        zip = (Join-Path $fixtureBuild "$name-Windows-x64.zip")
        checksums = (Join-Path $fixtureBuild "SHA256SUMS.txt")
        catalog = (Join-Path $package "signing-catalog.txt") }
}

function Get-ArchiveContractTree {
    param([string]$Root)
    return (@(Get-ChildItem -LiteralPath $Root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($Root.Length + 1)
        if ($_.PSIsContainer) { "$relative|directory" }
        else { "$relative|file|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" }
    }) -join "`n")
}

function Invoke-ArchiveContractChild {
    param([hashtable]$Fixture, [switch]$Clean, [switch]$Recover, [string]$ArchiveOverride = "")
    $selectedArchive = if ([string]::IsNullOrWhiteSpace($ArchiveOverride)) { $archiveScript } else { $ArchiveOverride }
    $arguments = @("-NoProfile", "-NonInteractive", "-File", $selectedArchive,
        "-BuildDir", $Fixture.build, "-PackageName", $Fixture.name)
    if ($Clean) { $arguments += "-CleanMutableRuntimeData" }
    if ($Recover) { $arguments += "-RecoverInterrupted" }
    return Invoke-CapturedProcess -FilePath (Get-Process -Id $PID).Path -Arguments $arguments `
        -TimeoutSeconds 30 -HiddenWindow
}

function Test-ReleaseArchiveCompressionFailurePreservesOutputs {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $baseline = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) `
        "Archive compression-fault baseline failed: $($baseline.stderr)"
    [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'),
        'archive-fixture:compression-fault-version')
    [IO.File]::WriteAllText($fixture.catalog, 'retained signing catalog')
    $drill = New-StageContractDirectory $Root
    $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
    Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
        -Destination $drill -ErrorAction Stop
    $source = Get-Content -LiteralPath $copiedArchive -Raw
    $anchor = "Compress-Archive -Path (Join-Path `$candidatePackage '*') -DestinationPath `$candidateZip"
    Assert-StageContract ([regex]::Matches($source, [regex]::Escape($anchor)).Count -eq 1) `
        'Archive compression failure mutation anchor absent or duplicated'
    $mutant = $source.Replace($anchor, "throw 'Injected archive compression failure'")
    [IO.File]::WriteAllText($copiedArchive, $mutant)
    $before = Get-ArchiveContractTree $fixture.build
    $result = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
        $result.stderr.Contains('Injected archive compression failure')) `
        "Archive compression mutant did not reach intended failure: $($result.stderr)"
    Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
        'Archive compression failure changed previous release bytes/tree'
    Write-Host 'PASS archive compression failure preserves previous release bytes/tree'
}

function Test-ReleaseArchiveSameNameRearchive {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $first = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $first.timed_out -and $first.exit_code -eq 0) `
        "Initial archive fixture failed: $($first.stderr)"
    $oldZipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
    [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'), 'archive-fixture:second-version')
    [IO.File]::WriteAllText($fixture.catalog, 'retained signing catalog')
    $catalogHash = (Get-FileHash -LiteralPath $fixture.catalog -Algorithm SHA256).Hash
    $second = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $second.timed_out -and $second.exit_code -eq 0) `
        "Same-name archive replacement failed: $($second.stderr)"
    $newZipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
    Assert-StageContract ($newZipHash -cne $oldZipHash -and
        (Get-Content -LiteralPath $fixture.checksums -Raw).Contains($newZipHash)) `
        'Same-name rearchive did not publish changed ZIP and matching checksums'
    Assert-StageContract ((Get-FileHash -LiteralPath $fixture.catalog -Algorithm SHA256).Hash -ceq $catalogHash) `
        'Same-name rearchive changed signing catalog source'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($fixture.zip)
    try {
        $appEntry = @($zip.Entries | Where-Object { $_.FullName.Replace('\', '/') -ceq 'sak_utility.exe' })
        Assert-StageContract ($appEntry.Count -eq 1) 'Same-name ZIP app entry missing or duplicated'
        $reader = [IO.StreamReader]::new($appEntry[0].Open())
        try { $contents = $reader.ReadToEnd() } finally { $reader.Dispose() }
        Assert-StageContract ($contents -ceq 'archive-fixture:second-version') `
            'Same-name ZIP did not contain changed app bytes'
        Assert-StageContract (@($zip.Entries | Where-Object {
            $_.FullName.Replace('\', '/') -ceq 'signing-catalog.txt'
        }).Count -eq 0) 'Same-name ZIP included signing catalog'
    } finally { $zip.Dispose() }
    Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock')) -and
        @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*').Count -eq 0) `
        'Same-name rearchive left transaction residue'
    Write-Host 'PASS same-name rearchive publishes new bytes and retains source signing catalog'
}

function Test-ReleaseArchiveNewNamePreservesPriorZip {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $priorZip = Join-Path $fixture.build 'Prior-Windows-x64.zip'
    [IO.File]::WriteAllBytes($priorZip, [byte[]](17, 34, 51, 68, 85))
    $priorHash = (Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($fixture.checksums,
        "SHA256 Checksums`nPrior-Windows-x64.zip:  $priorHash`n")
    $result = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -eq 0) `
        "New-name archive with prior release failed: $($result.stderr)"
    Assert-StageContract ((Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash -ceq $priorHash) `
        'New-name archive changed prior ZIP'
    $newHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
    Assert-StageContract ((Get-Content -LiteralPath $fixture.checksums -Raw).Contains($newHash) -and
        -not (Get-Content -LiteralPath $fixture.checksums -Raw).Contains($priorHash)) `
        'New-name archive did not publish a matching new manifest'
    Write-Host 'PASS new-name archive preserves prior ZIP and publishes matching new manifest'
}

function Test-ReleaseArchiveNewNamePublishFailurePreservesPriorRelease {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $priorZip = Join-Path $fixture.build 'Prior-Windows-x64.zip'
    [IO.File]::WriteAllBytes($priorZip, [byte[]](17, 34, 51, 68, 85))
    $priorHash = (Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($fixture.checksums,
        "SHA256 Checksums`nPrior-Windows-x64.zip:  $priorHash`n")
    [IO.File]::WriteAllText($fixture.catalog, 'retained signing catalog')
    $before = Get-ArchiveContractTree $fixture.build
    $drill = New-StageContractDirectory $Root
    $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
    Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
        -Destination $drill -ErrorAction Stop
    $source = Get-Content -LiteralPath $copiedArchive -Raw
    $anchor = '    Publish-ArchiveOutput $candidateChecksums $checksumsPath'
    Assert-StageContract ([regex]::Matches($source, [regex]::Escape($anchor)).Count -eq 1) `
        'New-name checksum publish mutation anchor absent or duplicated'
    $mutant = $source.Replace($anchor,
        "    throw 'Injected new-name checksum publication failure'`r`n$anchor")
    [IO.File]::WriteAllText($copiedArchive, $mutant)
    $result = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
        $result.stderr.Contains('Injected new-name checksum publication failure')) `
        "New-name checksum publish fault missed intended branch: $($result.stderr)"
    Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
        'New-name checksum publish failure changed previous release bytes/tree'
    Write-Host 'PASS new-name checksum publish fault removes only unpublished new ZIP'
}

function Test-ReleaseArchivePublishFailurePreservesOutputs {
    param([string]$Root)
    foreach ($kind in @('zip', 'checksum')) {
        $fixture = New-ArchiveContractFixture $Root
        $baseline = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) `
            "Archive $kind fault baseline failed: $($baseline.stderr)"
        [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'), "archive-fixture:$kind-new")
        [IO.File]::WriteAllText($fixture.catalog, "retained-signing-catalog:$kind")
        $before = Get-ArchiveContractTree $fixture.build
        $drill = New-StageContractDirectory $Root
        $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
        Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
        Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
            -Destination $drill -ErrorAction Stop
        $source = Get-Content -LiteralPath $copiedArchive -Raw
        $anchor = if ($kind -eq 'zip') {
            '    Publish-ArchiveOutput $candidateZip $zipPath'
        } else {
            '    Publish-ArchiveOutput $candidateChecksums $checksumsPath'
        }
        Assert-StageContract ([regex]::Matches($source, [regex]::Escape($anchor)).Count -eq 1) `
            "Archive $kind publish mutation anchor absent or duplicated"
        $fault = "Injected $kind publication failure"
        $mutant = $source.Replace($anchor, "    throw '$fault'`r`n$anchor")
        [IO.File]::WriteAllText($copiedArchive, $mutant)
        $result = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
        Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
            $result.stderr.Contains($fault)) `
            "Archive $kind publish fault missed intended branch: $($result.stderr)"
        Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
            "Archive $kind publish fault changed previous release bytes/tree"
        Write-Host "PASS archive $kind publish fault restores previous release bytes/tree"
    }
}

function Test-ReleaseArchivePendingTransactionRefused {
    param([string]$Root)
    foreach ($kind in @('transaction', 'lock')) {
        $fixture = New-ArchiveContractFixture $Root
        $baseline = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) `
            "Archive pending-marker baseline failed: $($baseline.stderr)"
        [IO.File]::WriteAllText($fixture.catalog, "retained-signing-catalog:$kind")
        if ($kind -eq 'transaction') {
            $pending = Join-Path $fixture.build '.sak-archive-txn-owned-fixture'
            New-Item -ItemType Directory -Path $pending -ErrorAction Stop | Out-Null
            [IO.File]::WriteAllText((Join-Path $pending 'recovery.txt'), 'owned pending transaction')
            $expected = 'Unresolved archive transaction'
        } else {
            [IO.File]::WriteAllText((Join-Path $fixture.build '.sak-archive-publish.lock'),
                'owned unresolved publication lock')
            $expected = 'Unresolved archive publication lock'
        }
        $before = Get-ArchiveContractTree $fixture.build
        $result = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
            $result.stderr.Contains($expected)) `
            "Archive $kind recovery marker did not fail closed: $($result.stderr)"
        Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
            "Archive $kind recovery marker changed previous release bytes/tree"
        $unversionedRecovery = Invoke-ArchiveContractChild $fixture -Recover
        Assert-StageContract (-not $unversionedRecovery.timed_out -and
            $unversionedRecovery.exit_code -ne 0 -and
            (Get-ArchiveContractTree $fixture.build) -ceq $before) (
            "Archive recovery accepted unversioned $kind marker")
        Write-Host "PASS archive refuses unresolved $kind marker without changing previous outputs"
    }
}
function Test-ReleaseArchivePreparationRecovery([string]$Root) {
    $fixture = New-ArchiveContractFixture $Root
    $baseline = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) (
        "Archive recovery baseline failed: $($baseline.stderr)")
    $zipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
    $checksumsHash = (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash
    $transactionLeaf = '.sak-archive-txn-' + [guid]::NewGuid().ToString('N')
    $transaction = Join-Path $fixture.build $transactionLeaf
    New-Item -ItemType Directory -Path $transaction -ErrorAction Stop | Out-Null
    $record = @('SAK_ARCHIVE_TXN_V1', "package=$($fixture.name)",
        "transaction=$transactionLeaf", 'previous_zip_present=1',
        "previous_zip_sha256=$zipHash", 'previous_checksums_present=1',
        "previous_checksums_sha256=$checksumsHash", 'PREPARED')
    $lockPath = Join-Path $fixture.build '.sak-archive-publish.lock'
    [IO.File]::WriteAllText($lockPath,
        ($record -join "`n") + "`n", [Text.Encoding]::ASCII)
    $hostPath = (Get-Process -Id $PID).Path
    $wrongName = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        '-NoProfile', '-NonInteractive', '-File', $archiveScript,
        '-BuildDir', $fixture.build, '-PackageName', 'Other', '-RecoverInterrupted'
    ) -TimeoutSeconds 30 -HiddenWindow
    Assert-StageContract (-not $wrongName.timed_out -and $wrongName.exit_code -ne 0 -and
        $wrongName.stderr.Contains('different package or transaction') -and
        (Test-Path -LiteralPath $transaction)) (
        'Archive recovery accepted a different package name')
    $combinedMode = Invoke-ArchiveContractChild $fixture -Recover -Clean
    Assert-StageContract (-not $combinedMode.timed_out -and $combinedMode.exit_code -ne 0 -and
        $combinedMode.stderr.Contains('cannot also clean package runtime data') -and
        (Test-Path -LiteralPath $transaction)) (
        'Archive recovery combined with mutable runtime cleanup')
    $held = [IO.File]::Open($lockPath, [IO.FileMode]::Open,
                           [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $blocked = Invoke-ArchiveContractChild $fixture -Recover
        Assert-StageContract (-not $blocked.timed_out -and $blocked.exit_code -ne 0 -and
            (Test-Path -LiteralPath $transaction)) (
            'Archive recovery acquired a live publication lock')
    } finally { $held.Dispose() }
    $recovered = Invoke-ArchiveContractChild $fixture -Recover
    Assert-StageContract (-not $recovered.timed_out -and $recovered.exit_code -eq 0) (
        "Prepared archive transaction was not recovered: $($recovered.stderr)")
    Assert-StageContract ((Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash -ceq $zipHash -and
        (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash -ceq $checksumsHash) (
        'Prepared archive recovery changed prior release bytes')
    Assert-StageContract (-not (Test-Path -LiteralPath $transaction) -and
        -not (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock'))) (
        'Prepared archive recovery left transaction markers')
    Write-Host 'PASS prepared archive crash marker recovers old coherent pair'
}
function Test-ReleaseArchiveLockCreateRacePreservesForeignLock([string]$Root,
                                                             [switch]$RemoveOwnershipGuard) {
    $fixture = New-ArchiveContractFixture $Root
    $baseline = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) (
        "Archive lock-race baseline failed: $($baseline.stderr)")
    $zipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
    $checksumsHash = (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash
    $drill = New-StageContractDirectory $Root
    $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
    Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
        -Destination $drill -ErrorAction Stop
    $source = Get-Content -LiteralPath $copiedArchive -Raw
    $anchor = '    $lockStream = [IO.File]::Open($lockPath,'
    Assert-StageContract ([regex]::Matches($source, [regex]::Escape($anchor)).Count -eq 1) (
        'Archive lock-create race anchor is missing or duplicated')
    $mutant = $source.Replace($anchor,
        "    [IO.File]::WriteAllText(`$lockPath, 'race-owner-lock')`r`n$anchor")
    if ($RemoveOwnershipGuard) {
        $guard = 'if ($ownsLock -and $cleanupAllowed -and (Test-Path -LiteralPath $lockPath -PathType Leaf)) {'
        Assert-StageContract ([regex]::Matches($mutant, [regex]::Escape($guard)).Count -eq 1) (
            'Archive lock ownership mutation anchor is missing or duplicated')
        $mutant = $mutant.Replace($guard,
            'if ($cleanupAllowed -and (Test-Path -LiteralPath $lockPath -PathType Leaf)) {')
    }
    [IO.File]::WriteAllText($copiedArchive, $mutant)
    $result = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
    $lockPath = Join-Path $fixture.build '.sak-archive-publish.lock'
    $lockText = if (Test-Path -LiteralPath $lockPath -PathType Leaf) {
        Get-Content -LiteralPath $lockPath -Raw
    } else { '' }
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
        $lockText -ceq 'race-owner-lock') (
        'Archive lock-create race deleted or changed a competing publisher lock')
    Assert-StageContract ((Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash -ceq
        $zipHash -and
        (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash -ceq $checksumsHash -and
        @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*').Count -eq 0) (
        'Archive lock-create race changed previous release or created a transaction')
    Write-Host 'PASS archive lock-create race preserves competing publisher lock'
}
function New-ArchiveCrashMutant([string]$Root, [string]$Anchor) {
    $drill = New-StageContractDirectory $Root
    $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
    Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
        -Destination $drill -ErrorAction Stop
    $source = Get-Content -LiteralPath $copiedArchive -Raw
    Assert-StageContract ([regex]::Matches($source, [regex]::Escape($Anchor)).Count -eq 1) (
        'Archive hard-crash mutation anchor is missing or duplicated')
    $mutant = $source.Replace($Anchor,
        "    [Diagnostics.Process]::GetCurrentProcess().Kill()`r`n$Anchor")
    [IO.File]::WriteAllText($copiedArchive, $mutant)
    return $copiedArchive
}
function Assert-ArchiveInjectedCrash([object]$Result, [string]$Context) {
    Assert-StageContract (-not $Result.timed_out -and $Result.exit_code -eq -1) (
        "$Context injected crash was not reached: exit=$($Result.exit_code), " +
        "timed_out=$($Result.timed_out), stderr=$($Result.stderr)")
}
function Test-ArchiveInjectedCrashOutcomeContract {
    $unrelatedFailure = [pscustomobject]@{
        timed_out = $false
        exit_code = 1
        stderr = 'synthetic pre-crash error'
    }
    $red = ''
    try { Assert-ArchiveInjectedCrash $unrelatedFailure 'Synthetic' }
    catch { $red = $_.Exception.Message }
    Assert-StageContract ($red.Contains('Synthetic injected crash was not reached')) (
        "Unrelated child failure was accepted as injected crash: $red")
}
function Test-ReleaseArchiveHardCrashRecovery([string]$Root) {
    foreach ($kind in @('same-name', 'new-name')) {
        $fixture = New-ArchiveContractFixture $Root
        $priorZip = ''
        if ($kind -eq 'same-name') {
            $baseline = Invoke-ArchiveContractChild $fixture
            Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) (
                "Same-name crash baseline failed: $($baseline.stderr)")
            $previousZipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
            [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'),
                'archive-fixture:after-crash')
        } else {
            $priorZip = Join-Path $fixture.build 'Prior-Windows-x64.zip'
            [IO.File]::WriteAllBytes($priorZip, [byte[]](17, 34, 51, 68, 85))
            $priorHash = (Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash
            [IO.File]::WriteAllText($fixture.checksums,
                "SHA256 Checksums`nPrior-Windows-x64.zip:  $priorHash`n")
            $previousZipHash = ''
        }
        $previousChecksumsHash = (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash
        $anchor = '    Publish-ArchiveOutput $candidateChecksums $checksumsPath'
        $copiedArchive = New-ArchiveCrashMutant $Root $anchor
        $crash = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
        Assert-ArchiveInjectedCrash $crash "Archive $kind"
        $pending = @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*')
        Assert-StageContract ($pending.Count -eq 1 -and
            (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock'))) (
            "Archive $kind crash left no recoverable transaction markers")
        Assert-StageContract ((Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash -ceq
            $previousChecksumsHash) "Archive $kind crash changed old checksum manifest"
        if ($kind -eq 'same-name') {
            Assert-StageContract ((Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash -cne
                $previousZipHash) 'Same-name crash never published candidate ZIP'
        } else {
            Assert-StageContract ((Test-Path -LiteralPath $fixture.zip -PathType Leaf) -and
                (Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash -ceq $priorHash) (
                'New-name crash did not leave the expected candidate and prior ZIPs')
        }
        $recovered = Invoke-ArchiveContractChild $fixture -Recover
        Assert-StageContract (-not $recovered.timed_out -and $recovered.exit_code -eq 0 -and
            $recovered.stdout.Contains('previous pair')) (
            "Archive $kind hard-crash recovery failed: $($recovered.stderr)")
        Assert-StageContract ((Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash -ceq
            $previousChecksumsHash) "Archive $kind recovery changed old manifest"
        if ($kind -eq 'same-name') {
            Assert-StageContract ((Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash -ceq
                $previousZipHash) 'Same-name hard-crash recovery failed to restore old ZIP'
        } else {
            Assert-StageContract (-not (Test-Path -LiteralPath $fixture.zip) -and
                (Get-FileHash -LiteralPath $priorZip -Algorithm SHA256).Hash -ceq $priorHash) (
                'New-name hard-crash recovery failed to remove only candidate ZIP')
        }
        Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock')) -and
            @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*').Count -eq 0) (
            "Archive $kind hard-crash recovery left transaction markers")
        Write-Host "PASS archive $kind hard crash restores previous release pair"
    }
}
function Test-ReleaseArchiveCommittedCrashRecovery([string]$Root) {
    foreach ($kind in @('before-cleanup', 'lock-only')) {
        $fixture = New-ArchiveContractFixture $Root
        $baseline = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) (
            "Archive committed-crash baseline failed: $($baseline.stderr)")
        $oldZipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
        $oldChecksumsHash = (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash
        [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'),
            "archive-fixture:committed-$kind")
        $anchor = if ($kind -eq 'before-cleanup') {
            '    Assert-ArchiveOutputState $zipPath ([pscustomobject]@{ present = $true; sha256 = $zipHash })'
        } else {
            '    if ($null -ne $lockStream) { $lockStream.Dispose() }'
        }
        $copiedArchive = New-ArchiveCrashMutant $Root $anchor
        $crash = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
        Assert-ArchiveInjectedCrash $crash "Archive committed-$kind"
        $newZipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
        $newChecksumsHash = (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash
        Assert-StageContract ($newZipHash -cne $oldZipHash -and
            $newChecksumsHash -cne $oldChecksumsHash -and
            (Get-Content -LiteralPath $fixture.checksums -Raw).Contains($newZipHash)) (
            "Archive committed-$kind crash did not publish a coherent candidate pair")
        $pending = @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*')
        Assert-StageContract ($pending.Count -eq $(if ($kind -eq 'lock-only') { 0 } else { 1 }) -and
            (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock'))) (
            "Archive committed-$kind crash left unexpected markers")
        $recovered = Invoke-ArchiveContractChild $fixture -Recover
        Assert-StageContract (-not $recovered.timed_out -and $recovered.exit_code -eq 0 -and
            $recovered.stdout.Contains('candidate pair')) (
            "Archive committed-$kind recovery failed: $($recovered.stderr)")
        Assert-StageContract ((Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash -ceq
            $newZipHash -and
            (Get-FileHash -LiteralPath $fixture.checksums -Algorithm SHA256).Hash -ceq
            $newChecksumsHash) "Archive committed-$kind recovery changed candidate bytes"
        Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $fixture.build '.sak-archive-publish.lock')) -and
            @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*').Count -eq 0) (
            "Archive committed-$kind recovery left markers")
        Write-Host "PASS archive committed-$kind crash keeps verified candidate pair"
    }
}
function Test-ReleaseArchiveTamperedCrashRecoveryRefused([string]$Root,
                                                         [string]$RecoveryOverride = '',
                                                         [string[]]$Kinds = @('public-zip', 'public-checksums', 'saved-zip', 'saved-checksums')) {
    foreach ($kind in $Kinds) {
        $fixture = New-ArchiveContractFixture $Root
        $baseline = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) (
            "Archive tamper baseline failed: $($baseline.stderr)")
        [IO.File]::WriteAllText((Join-Path $fixture.package 'sak_utility.exe'),
            "archive-fixture:tampered-$kind")
        $copiedArchive = New-ArchiveCrashMutant $Root `
            '    Publish-ArchiveOutput $candidateChecksums $checksumsPath'
        $crash = Invoke-ArchiveContractChild $fixture -ArchiveOverride $copiedArchive
        Assert-ArchiveInjectedCrash $crash "Archive $kind tamper fixture"
        $pending = @(Get-ChildItem -LiteralPath $fixture.build -Directory -Force -Filter '.sak-archive-txn-*')
        Assert-StageContract ($pending.Count -eq 1) "Archive $kind tamper fixture lacks transaction"
        $target = switch ($kind) {
            'public-zip' { $fixture.zip }
            'public-checksums' { $fixture.checksums }
            'saved-zip' { Join-Path $pending[0].FullName 'previous.zip' }
            'saved-checksums' { Join-Path $pending[0].FullName 'previous-checksums.txt' }
        }
        [IO.File]::WriteAllText($target, "unrecognized recovery bytes:$kind")
        $before = Get-ArchiveContractTree $fixture.build
        $recovered = Invoke-ArchiveContractChild $fixture -Recover `
            -ArchiveOverride $RecoveryOverride
        $expected = if ($kind.StartsWith('saved-')) {
            'snapshot is missing or changed'
        } else {
            'refuses unknown output bytes'
        }
        Assert-StageContract (-not $recovered.timed_out -and $recovered.exit_code -ne 0 -and
            $recovered.stderr.Contains($expected)) (
            "Archive $kind tamper did not refuse with the intended guard: $($recovered.stderr)")
        Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) (
            "Archive $kind tamper refusal changed transaction or public outputs")
        Write-Host "PASS archive hard-crash recovery refuses $kind tamper"
    }
}
function Test-ReleaseArchiveRecoveryGuardMutants([string]$Root) {
    $cases = @(
        @{ name = 'unknown-public-output'; from = 'if ($Current.present -and'; to = 'if ($false -and $Current.present -and'; kind = 'public-zip' },
        @{ name = 'changed-snapshot'; from = 'if (-not (Test-ArchiveStateMatch $actual $Expected)) {'; to = 'if ($false -and -not (Test-ArchiveStateMatch $actual $Expected)) {'; kind = 'saved-zip' }
    )
    foreach ($case in $cases) {
        $drill = New-StageContractDirectory $Root
        $copiedArchive = Join-Path $drill 'create_release_archive.ps1'
        Copy-Item -LiteralPath $archiveScript -Destination $copiedArchive -ErrorAction Stop
        Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts/path_reparse_guard.ps1') `
            -Destination $drill -ErrorAction Stop
        $source = Get-Content -LiteralPath $copiedArchive -Raw
        Assert-StageContract ([regex]::Matches($source, [regex]::Escape($case.from)).Count -eq 1) (
            "Archive recovery guard mutant target is not unique: $($case.name)")
        [IO.File]::WriteAllText($copiedArchive,
            $source.Replace($case.from, $case.to))
        $red = ''
        try {
            Test-ReleaseArchiveTamperedCrashRecoveryRefused $Root `
                -RecoveryOverride $copiedArchive -Kinds @($case.kind)
        } catch { $red = $_.Exception.Message }
        Assert-StageContract ($red.Contains("Archive $($case.kind) tamper did not refuse")) (
            "Archive recovery guard mutant did not turn its own assertion red: $($case.name): $red")
        Write-Host "KILLED archive recovery guard $($case.name)"
    }
}

function Test-ReleaseArchiveInvalidPriorManifestRefused {
    param([string]$Root)
    foreach ($kind in @('bad-hash', 'missing-zip')) {
        $fixture = New-ArchiveContractFixture $Root
        $baseline = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $baseline.timed_out -and $baseline.exit_code -eq 0) `
            "Archive prior-pair baseline failed: $($baseline.stderr)"
        if ($kind -eq 'bad-hash') {
            $zipHash = (Get-FileHash -LiteralPath $fixture.zip -Algorithm SHA256).Hash
            $manifest = Get-Content -LiteralPath $fixture.checksums -Raw
            Assert-StageContract ($manifest.Contains($zipHash)) 'Archive baseline manifest did not cite ZIP'
            [IO.File]::WriteAllText($fixture.checksums,
                $manifest.Replace($zipHash, ('0' * 64)))
        } else {
            Remove-Item -LiteralPath $fixture.zip -Force -ErrorAction Stop
        }
        $before = Get-ArchiveContractTree $fixture.build
        $result = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
            $result.stderr.Contains('Previous checksum manifest')) `
            "Archive $kind prior pair was accepted or failed for unrelated reason: $($result.stderr)"
        Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
            "Archive $kind prior pair refusal changed previous release bytes/tree"
        Write-Host "PASS archive refuses $kind prior pair without changing previous outputs"
    }
}

function Test-CleanReleaseArchive {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $result = Invoke-ArchiveContractChild $fixture
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -eq 0) ("Clean archive failed: " + $result.stderr)
    Assert-StageContract (Test-Path -LiteralPath $fixture.checksums -PathType Leaf) "Archive omitted checksums"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($fixture.zip)
    try {
        foreach ($relative in $fixture.files) {
            $entries = @($zip.Entries | Where-Object { $_.FullName.Replace('\', '/') -ceq $relative })
            Assert-StageContract ($entries.Count -eq 1) "Archive omitted or duplicated required artifact: $relative"
            $reader = [IO.StreamReader]::new($entries[0].Open())
            try { $content = $reader.ReadToEnd() } finally { $reader.Dispose() }
            Assert-StageContract ($content -ceq "archive-fixture:$relative") "Archive changed required artifact bytes: $relative"
        }
    } finally { $zip.Dispose() }
    Write-Host "PASS clean real release ZIP contains required fixture bytes"
}

function Test-ReleaseArchiveCrashRefusal {
    param([string]$Root)
    foreach ($shape in @("populated-directory", "empty-directory", "file")) {
        foreach ($clean in @($false, $true)) {
            $fixture = New-ArchiveContractFixture $Root
            $crashes = Join-Path $fixture.package "data/crashes"
            if ($shape -eq "file") { [IO.File]::WriteAllText($crashes, "retained-crash-file") }
            else {
                New-Item -ItemType Directory -Path $crashes | Out-Null
                if ($shape -eq "populated-directory") {
                    [IO.File]::WriteAllText((Join-Path $crashes "fixture.dmp"), "retained-crash-evidence")
                }
            }
            foreach ($path in @($fixture.zip, $fixture.checksums, $fixture.catalog)) {
                [IO.File]::WriteAllText($path, "retained-release-metadata:$([IO.Path]::GetFileName($path))")
            }
            $before = Get-ArchiveContractTree $fixture.build
            $result = Invoke-ArchiveContractChild $fixture -Clean:$clean
            Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0) "Archive accepted crash evidence: $shape, cleanup=$clean"
            Assert-StageContract ($result.stderr.Contains("Refusing to archive package with crash evidence")) ("Archive failed for an unrelated reason: " + $result.stderr)
            Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) "Crash refusal changed preexisting release bytes/tree: $shape, cleanup=$clean"
            Write-Host "PASS archive preserves crash evidence and existing outputs: $shape, cleanup=$clean"
        }
    }
}

function Test-ReleaseArchivePreflightPreservation {
    param([string]$Root)
    foreach ($kind in @('chocolatey-log', 'missing-required')) {
        $fixture = New-ArchiveContractFixture $Root
        if ($kind -eq 'missing-required') {
            Remove-Item -LiteralPath (Join-Path $fixture.package 'sak_utility.exe') -Force -ErrorAction Stop
            $expected = 'Refusing to archive package missing required file'
        } else {
            $logs = Join-Path $fixture.package 'tools/chocolatey/logs'
            New-Item -ItemType Directory -Path $logs -Force -ErrorAction Stop | Out-Null
            [IO.File]::WriteAllText((Join-Path $logs 'choco.summary.log'), 'owned mutable log')
            $expected = 'Refusing to archive package with Chocolatey runtime logs'
        }
        foreach ($path in @($fixture.zip, $fixture.checksums, $fixture.catalog)) {
            [IO.File]::WriteAllText($path, "retained-release-metadata:$([IO.Path]::GetFileName($path))")
        }
        $before = Get-ArchiveContractTree $fixture.build
        $result = Invoke-ArchiveContractChild $fixture
        Assert-StageContract (-not $result.timed_out -and $result.exit_code -ne 0 -and
            $result.stderr.Contains($expected)) `
            "Archive $kind refusal failed for an unrelated reason: $($result.stderr)"
        Assert-StageContract ((Get-ArchiveContractTree $fixture.build) -ceq $before) `
            "Archive $kind refusal changed preexisting release bytes/tree"
        Write-Host "PASS archive $kind refusal preserves previous outputs"
    }
}

function Test-ReleaseArchiveLogCleanup {
    param([string]$Root)
    $fixture = New-ArchiveContractFixture $Root
    $logs = Join-Path $fixture.package 'tools/chocolatey/logs'
    New-Item -ItemType Directory -Path $logs -Force -ErrorAction Stop | Out-Null
    [IO.File]::WriteAllText((Join-Path $logs 'choco.summary.log'), 'owned mutable log')
    $result = Invoke-ArchiveContractChild $fixture -Clean
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -eq 0) `
        "Archive clean mode failed to remove owned Chocolatey logs: $($result.stderr)"
    Assert-StageContract ((Test-Path -LiteralPath $logs -PathType Container) -and
        @(Get-ChildItem -LiteralPath $logs -Force -ErrorAction Stop).Count -eq 0) `
        'Archive clean mode retained Chocolatey runtime logs'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($fixture.zip)
    try {
        Assert-StageContract (@($zip.Entries | Where-Object {
            $_.Name -ne '' -and $_.FullName.Replace('\', '/') -like 'tools/chocolatey/logs/*'
        }).Count -eq 0) 'Archive ZIP retained Chocolatey runtime logs'
    } finally { $zip.Dispose() }
    Write-Host 'PASS archive clean mode removes Chocolatey logs before ZIP creation'
}

function Test-PortableRuntimeSelection {
    param([string]$Text, [string]$Root)
    $buildRoot = New-StageContractDirectory $Root
    $packageRoot = New-StageContractDirectory $Root
    foreach ($name in @("Qt6Core.dll", "Qt6Test.dll", "msvcp140.dll", "vcruntime140.dll", "opengl32sw.dll", "icuuc.dll", "icuin.dll", "icu.dll")) {
        [IO.File]::WriteAllText((Join-Path $buildRoot $name), "ordinary selection fixture")
    }
    . Import-StageFunctions $Text @("Copy-OptionalFile")
    $Script:SelectedDlls = [Collections.Generic.List[string]]::new()
    function Copy-Item {
        param($LiteralPath, $Destination, [switch]$Force)
        $Script:SelectedDlls.Add([IO.Path]::GetFileName($LiteralPath))
    }
    function New-Item { param($ItemType, [switch]$Force, $Path) }
    $ast = Get-StageAst $Text
    $assignment = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$qtDlls'
    }, $true))
    Assert-StageContract ($assignment.Count -eq 1) "Qt selection assignment changed"
    . ([scriptblock]::Create($assignment[0].Extent.Text))
    foreach ($variable in @('$dll', '$dllName', '$pattern')) {
        $loops = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.ForEachStatementAst] -and $node.Variable.Extent.Text -ceq $variable
        }, $true))
        Assert-StageContract ($loops.Count -eq 1) "Runtime selection loop changed: $variable"
        . ([scriptblock]::Create($loops[0].Extent.Text))
    }
    foreach ($name in @("icuuc.dll", "icuin.dll", "icu.dll")) {
        Assert-StageContract (-not $Script:SelectedDlls.Contains($name)) "Portable selection copied Windows ICU shim: $name"
    }
    foreach ($name in @("Qt6Core.dll", "msvcp140.dll", "vcruntime140.dll", "opengl32sw.dll")) {
        Assert-StageContract ($Script:SelectedDlls.Contains($name)) "Portable selection omitted required runtime: $name"
    }
    Assert-StageContract (-not $Script:SelectedDlls.Contains("Qt6Test.dll")) "Portable selection copied Qt Test runtime"
    $calls = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq "Invoke-RequiredScript" -and
            $node.Extent.Text.Contains("verify_windows_runtime_dependencies.ps1")
    }, $true))
    Assert-StageContract ($calls.Count -eq 1) "Expected one final portable dependency verifier call"
    $Script:VerifierArguments = $null
    function Invoke-RequiredScript {
        param($ScriptPath, $ScriptArguments)
        $Script:VerifierArguments = $ScriptArguments
    }
    $repoRootPath = $ProjectRoot
    . ([scriptblock]::Create($calls[0].Extent.Text))
    Assert-StageContract ($Script:VerifierArguments.RequireNoSystemDlls -is [bool] -and $Script:VerifierArguments.RequireNoSystemDlls) "Portable stage did not require OS-shim refusal"
}

function New-QtPlanFixture {
    param([string]$Root)
    $fixture = New-StageContractDirectory $Root
    $qtRoot = Join-Path $fixture "qt"
    $bin = Join-Path $qtRoot "bin"
    $plugin = Join-Path $qtRoot "plugins\platforms"
    $destination = Join-Path $fixture "package"
    $metadata = Join-Path $qtRoot "mkspecs"
    New-Item -ItemType Directory -Path $bin,$plugin,$destination,$metadata -ErrorAction Stop | Out-Null
    foreach ($name in @("Qt6Core.dll", "icuuc78.dll", "windeployqt.exe")) { [IO.File]::WriteAllText((Join-Path $bin $name), "ordinary Qt fixture " + $name) }
    [IO.File]::WriteAllText((Join-Path $plugin "qwindows.dll"), "ordinary plugin fixture")
    [IO.File]::WriteAllText((Join-Path $destination "fixture.exe"), "ordinary app fixture")
    [IO.File]::WriteAllText((Join-Path $metadata "qmodule.pri"), "QT.global_private.enabled_features = relocatable winsdkicu")
    $context = @{
        tool = (Join-Path $bin "windeployqt.exe"); application = (Join-Path $destination "fixture.exe")
        qt_root = $qtRoot; destination = $destination; system_directory = [Environment]::SystemDirectory
    }
    $files = @(
        @{ source = (Join-Path $bin "Qt6Core.dll"); target = $destination },
        @{ source = (Join-Path $plugin "qwindows.dll"); target = (Join-Path $destination "platforms") },
        @{ source = (Join-Path $bin "icuuc78.dll"); target = $destination },
        @{ source = (Join-Path ([Environment]::SystemDirectory) "icuuc.dll"); target = $destination }
    )
    $plan = @{ files = $files } | ConvertTo-Json -Depth 5 | ConvertFrom-Json
    return @{ context = $context; plan = $plan; root = $fixture }
}
function Copy-QtPlanObject {
    param([object]$Plan)
    return $Plan | ConvertTo-Json -Depth 6 | ConvertFrom-Json
}
function Test-QtInstallPlan {
    param([string]$Text, [string]$Root)
    . Import-StageFunctions $Text @(
        "Test-IsSystemDll", "Assert-NoPackageSystemDlls", "Assert-QtDeployment",
        "Get-QtDeploymentFullPath", "Test-QtDeploymentChildPath", "Assert-QtDeploymentDirectPath",
        "Get-QtDeploymentContext", "Get-ValidatedQtDeploymentPlan", "Copy-QtRuntimePlan"
    )
    $MaximumQtPlanEntries = 4096
    Assert-StageContract ((Get-QtDeploymentFullPath 'C:\') -ceq 'C:\') "Drive-qualified root lost its separator"
    $fixture = New-QtPlanFixture $Root
    $context = Get-QtDeploymentContext $fixture.context.tool $fixture.context.application
    $plan = Get-ValidatedQtDeploymentPlan $fixture.plan $context
    Assert-StageContract ($plan.files.Count -eq 3 -and $plan.windows_provided.Count -eq 1 -and $plan.windows_provided[0] -ieq "icuuc.dll") "Validated plan did not separate OS ICU from private ICU"
    Copy-QtRuntimePlan $plan $context
    foreach ($name in @("Qt6Core.dll", "icuuc78.dll", "platforms\qwindows.dll")) {
        Assert-StageContract (Test-Path -LiteralPath (Join-Path $context.destination $name) -PathType Leaf) "Validated copy omitted runtime/plugin: $name"
    }
    Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $context.destination "icuuc.dll"))) "Validated copy packaged an OS shim"
    $debugCore = Join-Path $context.qt_root "bin\Qt6Cored.dll"
    [IO.File]::WriteAllText($debugCore, "ordinary debug Core fixture")
    $debugPlan = Copy-QtPlanObject $fixture.plan
    $debugPlan.files[0].source = $debugCore
    $debugResult = Get-ValidatedQtDeploymentPlan $debugPlan $context
    Assert-StageContract (@($debugResult.files | Where-Object { $_.target.EndsWith("\Qt6Cored.dll") }).Count -eq 1) "Plan forced Release instead of preserving debug Core"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].target = Join-Path $context.destination "..\escape"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "Path traversal"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].target = $context.destination + "-escape"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "target escaped"
    $outside = Join-Path $fixture.root "outside.dll"
    [IO.File]::WriteAllText($outside, "ordinary outside-source fixture")
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].source = $outside
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "source escaped"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files += $bad.files[1]
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "duplicate destination"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[0].target = Join-Path $context.destination "platforms"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "Core runtime must be deployed next"
    $impostor = Join-Path $context.qt_root "bin\icuuc.dll"
    [IO.File]::WriteAllText($impostor, "ordinary OS-name impostor fixture")
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[3].source = $impostor
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "OS-named Qt dependency"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].target = "\\server\share\plugins"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "literal local"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].target = $context.destination + "\streams:alternate"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "literal local"
    foreach ($suffix in @("dot.", "space ", "wild*card")) {
        Expect-StageFailure { Get-QtDeploymentFullPath (Join-Path $context.destination $suffix) } "invalid Windows path component"
    }
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[1].target = Join-Path $context.destination "Qt6Core.dll"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "target directory is a file"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files = @()
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "entry count"
    $bad = Copy-QtPlanObject $fixture.plan
    $bad.files[0] | Add-Member -NotePropertyName unexpected -NotePropertyValue "no"
    Expect-StageFailure { Get-ValidatedQtDeploymentPlan $bad $context } "source and target only"
    $Script:QtReparseFixture = ""
    function Get-Item {
        param($LiteralPath, [switch]$Force, $ErrorAction)
        if ($LiteralPath -ieq $Script:QtReparseFixture) {
            return [pscustomobject]@{ Attributes = [IO.FileAttributes]::ReparsePoint }
        }
        return Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath -Force -ErrorAction Stop
    }
    foreach ($path in @($fixture.plan.files[0].source, $context.destination)) {
        $Script:QtReparseFixture = $path
        Expect-StageFailure { Get-ValidatedQtDeploymentPlan $fixture.plan $context } "Reparse path"
    }
    $Script:QtReparseFixture = ""
    $features = Join-Path $context.qt_root "mkspecs\qmodule.pri"
    [IO.File]::WriteAllText($features, "QT.global_private.enabled_features = winsdkicu")
    Expect-StageFailure { Get-QtDeploymentContext $context.tool $context.application } "relocatable Qt build"
    $leak = Join-Path $context.destination "icuuc.dll"
    [IO.File]::WriteAllText($leak, "ordinary preexisting OS-shim fixture")
    Expect-StageFailure { Copy-QtRuntimePlan $plan $context } "app-local Windows system DLL"
    Assert-StageContract (Test-Path -LiteralPath $leak) "Install helper deleted an existing OS shim"
    $corruptFixture = New-QtPlanFixture $Root
    $corruptPlan = Get-ValidatedQtDeploymentPlan $corruptFixture.plan $corruptFixture.context
    function Copy-Item {
        param($LiteralPath, $Destination, [switch]$Force, $ErrorAction)
        [IO.File]::WriteAllText($Destination, "injected copy corruption")
    }
    Expect-StageFailure { Copy-QtRuntimePlan $corruptPlan $corruptFixture.context } "hash mismatch"
}

function Test-ActualQtPlan {
    $cachePath = Join-Path $resolvedBuildDirectory "CMakeCache.txt"
    $matches = @(Get-Content -LiteralPath $cachePath -ErrorAction Stop | Where-Object { $_ -cmatch '^WINDEPLOYQT:FILEPATH=' })
    Assert-StageContract ($matches.Count -eq 1) "Configured windeployqt path is absent or ambiguous"
    $tool = $matches[0].Substring("WINDEPLOYQT:FILEPATH=".Length)
    $hostPath = (Get-Process -Id $PID).Path
    $options = @{
        FilePath = $hostPath
        Arguments = @("-NoProfile", "-NonInteractive", "-File", $deployScript,
            "-WindeployqtPath", $tool, "-ApplicationPath", (Join-Path $buildRoot "sak_utility.exe"), "-PlanOnly")
        TimeoutSeconds = 90
    }
    $result = Invoke-CapturedProcess @options
    if (-not [string]::IsNullOrWhiteSpace($result.stderr)) { [Console]::Error.WriteLine($result.stderr) }
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -eq 0) "Pinned windeployqt plan validation failed: $($result.stderr)"
    $plan = $result.stdout | ConvertFrom-Json
    Assert-StageContract ($plan.files -is [Array] -and $plan.files.Count -gt 0) "Actual Qt plan has no copy records"
    Assert-StageContract (@($plan.files | Where-Object { [IO.Path]::GetFileName($_.source) -ceq "Qt6Core.dll" }).Count -eq 1) "Actual Qt plan omitted release Core"
    Assert-StageContract (@($plan.files | Where-Object { [IO.Path]::GetFileName($_.source) -ceq "qwindows.dll" -and $_.target_directory.EndsWith("\platforms") }).Count -eq 1) "Actual Qt plan lost platform plugin directory"
    Assert-StageContract (@($plan.files | Where-Object { [IO.Path]::GetFileName($_.source) -ieq "icuuc.dll" }).Count -eq 0) "Actual Qt plan copied Windows ICU"
    Write-Host "PASS actual Qt JSON plan: $($plan.files.Count) selected files; Windows-provided: $($plan.windows_provided -join ', ')"
}
function Test-QtPlannerInvocation {
    param([string]$Text)
    . Import-StageFunctions $Text @("Assert-QtDeployment", "Get-QtRuntimePlan")
    $MaximumQtPlanBytes = 4MB
    $QtPlannerTimeoutSeconds = 60
    $context = @{ tool = "C:\selected\windeployqt.exe"; application = "C:\install\app.exe"; destination = "C:\install" }
    $Script:PlannerTimeout = $false
    $Script:PlannerExit = 0
    $Script:PlannerArgs = $null
    function Invoke-CapturedProcess {
        param($FilePath, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        Assert-StageContract ($FilePath -ceq "C:\selected\windeployqt.exe" -and $WorkingDirectory -ceq "C:\install" -and $TimeoutSeconds -eq 60) "Planner lost explicit tool/directory/deadline"
        $Script:PlannerArgs = $Arguments
        return [pscustomobject]@{ timed_out = $Script:PlannerTimeout; exit_code = $Script:PlannerExit; stdout = '{"files":[]}'; stderr = "" }
    }
    function Get-ValidatedQtDeploymentPlan { param($Plan, $Context) return $Plan }
    Get-QtRuntimePlan $context | Out-Null
    $expected = @("--dry-run", "--json", "--no-translations", "--no-compiler-runtime", $context.application)
    Assert-StageContract (($Script:PlannerArgs -join "|") -ceq ($expected -join "|")) "Planner arguments changed debug/release auto-detection or dry-run planning"
    $Script:PlannerExit = 7
    Expect-StageFailure { Get-QtRuntimePlan $context } "planning failed"
    $Script:PlannerExit = 0
    $Script:PlannerTimeout = $true
    Expect-StageFailure { Get-QtRuntimePlan $context } "planning failed"
}

function Get-CMakeQtInstallLaunch {
    param([string]$FixtureRoot, [string]$CMakePath)
    $cmakeText = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw
    $blocks = [regex]::Matches($cmakeText, '(?s)install\(CODE "\s*(?<code>execute_process\(.*?Validated Qt deployment failed.*?endif\(\))\s*"\)')
    Assert-StageContract ($blocks.Count -eq 1) "Expected one validated Qt install launch block"
    # Materialize the production install string without configuring or modifying the shared
    # build. The executable paths stay real; only the installed helper/payload are fixtures.
    $code = $blocks[0].Groups['code'].Value.Replace('\"', '"').Replace('\${', '${')
    $variables = @{
        '${CMAKE_COMMAND}' = $CMakePath
        '${SAK_QT_DEPLOY_POWERSHELL}' = (Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell\v1.0\powershell.exe')
        '${CMAKE_CURRENT_SOURCE_DIR}' = $FixtureRoot
        '${WINDEPLOYQT}' = (Join-Path $FixtureRoot 'selected\windeployqt.exe')
        '${CMAKE_INSTALL_PREFIX}' = $FixtureRoot
    }
    foreach ($entry in $variables.GetEnumerator()) {
        $code = $code.Replace($entry.Key, $entry.Value.Replace('\', '/'))
    }
    return $code
}

function Test-CMakeQtInstallLaunch {
    param([string]$Root)
    $fixture = New-StageContractDirectory $Root
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $powerShellCore = (Get-Command pwsh.exe -ErrorAction Stop).Source
    $coreEnvironment = Invoke-CapturedProcess -FilePath $powerShellCore -Arguments @(
        '-NoProfile', '-NonInteractive', '-Command', '[Environment]::GetEnvironmentVariable("PSModulePath", "Process")'
    ) -TimeoutSeconds 30
    Assert-StageContract (-not $coreEnvironment.timed_out -and $coreEnvironment.exit_code -eq 0 -and
        $coreEnvironment.stdout -match '[\\/]PowerShell[\\/]7[\\/]Modules') "PowerShell 7 module-path prerequisite unavailable"
    New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts'),(Join-Path $fixture 'bin') | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture 'bin\sak_utility.exe'), 'ordinary CMake launch fixture')
    $probe = @'
param([string]$WindeployqtPath, [string]$ApplicationPath)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Expected selected Windows PowerShell host' }
if (-not $WindeployqtPath.EndsWith('/selected/windeployqt.exe')) { throw 'Qt tool argument changed' }
$hash = Get-FileHash -LiteralPath $ApplicationPath -Algorithm SHA256
if ($hash.Hash.Length -ne 64) { throw 'Install child hash command did not complete' }
Write-Output 'CMAKE_INSTALL_HASH_OK'
'@
    [IO.File]::WriteAllText((Join-Path $fixture 'scripts\deploy_qt_runtime.ps1'), $probe)
    $launchPath = Join-Path $fixture 'install-launch.cmake'
    [IO.File]::WriteAllText($launchPath, (Get-CMakeQtInstallLaunch $fixture $cmake))
    $previousModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Process')
    try {
        # A PowerShell-to-PowerShell launch sanitizes this path and hides the defect. CMake
        # is the actual parent here, retaining the real PowerShell 7 module environment.
        [Environment]::SetEnvironmentVariable('PSModulePath', $coreEnvironment.stdout, 'Process')
        $result = Invoke-CapturedProcess -FilePath $cmake -Arguments @('-P', $launchPath) -TimeoutSeconds 30
    }
    finally {
        [Environment]::SetEnvironmentVariable('PSModulePath', $previousModulePath, 'Process')
    }
    Assert-StageContract (-not $result.timed_out -and $result.exit_code -eq 0 -and
        $result.stdout.Contains('CMAKE_INSTALL_HASH_OK')) ("CMake install child failed under inherited PowerShell 7 modules: " +
        "exit=$($result.exit_code); stdout=$($result.stdout); stderr=$($result.stderr)")
    Write-Host 'PASS actual CMake install launch under inherited PowerShell 7 module path'
}

function Test-CMakeToolsInstall {
    param([string]$Root)
    $fixture = New-StageContractDirectory $Root
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $cmakeText = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw
    $blocks = [regex]::Matches($cmakeText, '(?s)install\(DIRECTORY "\$\{CMAKE_CURRENT_SOURCE_DIR\}/tools/"\s+DESTINATION bin/tools.*?\)')
    Assert-StageContract ($blocks.Count -eq 1) "Expected one production tools-install block"
    $retained = @('vendor/tool.exe', 'vendor/runtime.dll', 'vendor/LICENSE.txt', 'filesystem/manifest.json',
        'vendor/_builder/retained.txt', 'vendor/build/retained.txt')
    $excluded = @('_build/direct.txt', 'filesystem/_build/nested.txt')
    foreach ($relative in ($retained + $excluded)) {
        $path = Join-Path $fixture ('tools/' + $relative)
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, ('fixture:' + $relative))
    }
    $projectText = "cmake_minimum_required(VERSION 3.28)`nproject(SakToolsInstallContract NONE)`n" + $blocks[0].Value
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeLists.txt'), $projectText)
    $fixtureBuild = Join-Path $fixture 'cmake-build'
    $configure = Invoke-CapturedProcess -FilePath $cmake -Arguments @(
        '-S', $fixture, '-B', $fixtureBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64'
    ) -TimeoutSeconds 60
    Assert-StageContract (-not $configure.timed_out -and $configure.exit_code -eq 0) (
        'Tools-install fixture configure failed: ' + $configure.stdout + $configure.stderr)
    $prefix = Join-Path $fixture 'install'
    $install = Invoke-CapturedProcess -FilePath $cmake -Arguments @(
        '--install', $fixtureBuild, '--config', 'Release', '--prefix', $prefix
    ) -TimeoutSeconds 30
    Assert-StageContract (-not $install.timed_out -and $install.exit_code -eq 0) (
        'Tools-install fixture failed: ' + $install.stdout + $install.stderr)
    foreach ($relative in $retained) {
        $path = Join-Path $prefix ('bin/tools/' + $relative)
        Assert-StageContract (Test-Path -LiteralPath $path -PathType Leaf) "Install omitted required tool or near-match: $relative"
        Assert-StageContract ((Get-Content -LiteralPath $path -Raw) -ceq ('fixture:' + $relative)) "Install changed tool fixture bytes: $relative"
    }
    foreach ($relative in $excluded) {
        Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $prefix ('bin/tools/' + $relative)))) "Raw install leaked exact _build scratch: $relative"
        Assert-StageContract (Test-Path -LiteralPath (Join-Path $fixture ('tools/' + $relative)) -PathType Leaf) "Install removed source scratch: $relative"
    }
    Write-Host 'PASS actual tools install excludes exact _build scratch and preserves runtime/near-match files'
}

function Test-PortableBrowserExtension {
    param([string]$StageText, [string]$SmokeText, [string]$Root)
    $fixture = New-StageContractDirectory $Root
    $buildRoot = New-StageContractDirectory $fixture
    $packageRoot = New-StageContractDirectory $fixture
    $sourceCrx = Join-Path $Script:buildRoot 'sak_browser_control.crx'
    Copy-Item -LiteralPath $sourceCrx -Destination $buildRoot -ErrorAction Stop
    . Import-StageFunctions $StageText @('Copy-RequiredFile')
    $ast = Get-StageAst $StageText
    $copies = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq 'Copy-RequiredFile' -and
            $node.Extent.Text.Contains('"sak_browser_control.crx"')
    }, $true))
    Assert-StageContract ($copies.Count -eq 1) 'Portable stager omitted required signed browser extension copy'
    . ([scriptblock]::Create($copies[0].Extent.Text))
    $stagedCrx = Join-Path $packageRoot 'sak_browser_control.crx'
    Assert-StageContract ((Get-FileHash -LiteralPath $sourceCrx -Algorithm SHA256).Hash -ceq
        (Get-FileHash -LiteralPath $stagedCrx -Algorithm SHA256).Hash) 'Portable stage changed signed browser extension bytes'
    $buildRoot = New-StageContractDirectory $fixture
    Expect-StageFailure { . ([scriptblock]::Create($copies[0].Extent.Text)) } 'Required file missing'
    $critical = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$critical'
    }, $true))
    Assert-StageContract ($critical.Count -eq 1) 'Expected one staging critical-artifact list'
    . ([scriptblock]::Create($critical[0].Extent.Text))
    Assert-StageContract ($critical -contains 'sak_browser_control.crx') 'Staging critical check omitted browser extension'
    $smokeAst = Get-StageAst $SmokeText
    $required = @($smokeAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$required'
    }, $true))
    Assert-StageContract ($required.Count -eq 1) 'Expected one portable-smoke artifact list'
    . ([scriptblock]::Create($required[0].Extent.Text))
    Assert-StageContract ($required -contains 'sak_browser_control.crx') 'Portable smoke omitted browser extension'
}

function Invoke-BrowserPackagingGuardDrills {
    param([string]$StageText, [string]$SmokeText, [string]$Root)
    Test-PortableBrowserExtension $StageText $SmokeText $Root
    $drills = @(
        @{ name = 'browser-extension-copy'; target = 'stage'; from = 'Copy-RequiredFile (Join-Path $buildRoot "sak_browser_control.crx") $packageRoot'; expected = 'omitted required signed browser extension copy' },
        @{ name = 'browser-extension-stage-critical'; target = 'stage'; from = '    "sak_browser_control.crx",'; expected = 'Staging critical check omitted browser extension' },
        @{ name = 'browser-extension-smoke-critical'; target = 'smoke'; from = "    'sak_browser_control.crx',"; expected = 'Portable smoke omitted browser extension' }
    )
    foreach ($drill in $drills) {
        $source = if ($drill.target -eq 'stage') { $StageText } else { $SmokeText }
        Assert-StageContract ($source.Contains($drill.from)) "Browser packaging mutation anchor missing: $($drill.name)"
        $mutantStage = if ($drill.target -eq 'stage') { $source.Replace($drill.from, '') } else { $StageText }
        $mutantSmoke = if ($drill.target -eq 'smoke') { $source.Replace($drill.from, '') } else { $SmokeText }
        Expect-StageFailure { Test-PortableBrowserExtension $mutantStage $mutantSmoke $Root } $drill.expected
        Write-Host "KILLED $($drill.name)"
    }
    Write-Host 'PASS signed browser extension bytes, required input and both package gates'
}

function Invoke-QtPackagingGuardDrills {
    param([string]$StageText, [string]$DeploymentText, [string]$Root)
    $selectionDrills = @(
        @{ name = "qt-selection-excludes-os-shims"; from = '-Filter "Qt6*.dll"'; to = '-Filter "*.dll"'; expected = "copied Windows ICU shim" },
        @{ name = "crt-selection-excludes-os-shims"; from = '"vcruntime*.dll", "msvcp*.dll", "concrt*.dll"'; to = '"*.dll", "msvcp*.dll", "concrt*.dll"'; expected = "copied Windows ICU shim" },
        @{ name = "portable-package-refusal-mode"; from = 'RequireNoSystemDlls = $true'; to = 'RequireNoSystemDlls = $false'; expected = "did not require OS-shim refusal" }
    )
    foreach ($drill in $selectionDrills) {
        Assert-StageContract ($StageText.Contains($drill.from)) "Portable mutation anchor missing"
        Expect-StageFailure { Test-PortableRuntimeSelection ($StageText.Replace($drill.from, $drill.to)) $Root } $drill.expected
        Write-Host "KILLED $($drill.name)"
    }
    $planDrills = @(
        @{ name = "plan-target-boundary"; from = '(Test-QtDeploymentChildPath $targetDirectory $Context.destination -AllowRoot)'; to = '($targetDirectory.Length -gt 0)'; expected = "Expected 'target escaped'" },
        @{ name = "plan-source-boundary"; from = '(Test-QtDeploymentChildPath $source $Context.qt_root)'; to = '($source.Length -gt 0)'; expected = "Expected 'source escaped'" },
        @{ name = "plan-os-source-authority"; from = '((Split-Path -Parent $source).Equals($Context.system_directory, [StringComparison]::OrdinalIgnoreCase))'; to = '($source.Length -gt 0)'; expected = "Expected 'OS-named Qt dependency'" },
        @{ name = "plan-private-icu-preserved"; from = 'return $systemDlls -contains $lower'; to = 'return ($systemDlls -contains $lower) -or $lower.StartsWith("icu")'; expected = "OS-named Qt dependency" },
        @{ name = "plan-unique-targets"; from = '($targets.Add($target))'; to = '($targets.Add($target) -or $targets.Contains($target))'; expected = "Expected 'duplicate destination'" },
        @{ name = "plan-core-placement"; from = '($cores[0].target_directory.Equals($Context.destination, [StringComparison]::OrdinalIgnoreCase))'; to = '($cores[0].target_directory.Length -gt 0)'; expected = "Expected 'Core runtime must be deployed next'" },
        @{ name = "plan-relocatable-required"; from = '\brelocatable\b'; to = '\bwinsdkicu\b'; expected = "Expected 'relocatable Qt build'" },
        @{ name = "plan-reparse-refusal"; from = '(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)'; to = '(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ge 0)'; expected = "Expected 'Reparse path'" },
        @{ name = "plan-directory-type"; from = '(Test-Path -LiteralPath $targetDirectory -PathType Container)'; to = '(Test-Path -LiteralPath $targetDirectory)'; expected = "Expected 'target directory is a file'" },
        @{ name = "plan-record-shape"; from = '@($entry.PSObject.Properties).Count -eq 2'; to = '@($entry.PSObject.Properties).Count -ge 2'; expected = "Expected 'source and target only'" },
        @{ name = "install-copy-integrity"; from = '($sourceHash -ceq $targetHash)'; to = '($sourceHash -ceq $sourceHash)'; expected = "Expected 'hash mismatch'" },
        @{ name = "install-existing-os-shim-refusal"; from = 'if ($leaked.Count -gt 0)'; to = 'if ($leaked.Count -lt 0)'; expected = "Expected 'app-local Windows system DLL'" }
    )
    foreach ($drill in $planDrills) {
        Assert-StageContract ($DeploymentText.Contains($drill.from)) "Plan mutation anchor missing: $($drill.name)"
        Expect-StageFailure { Test-QtInstallPlan ($DeploymentText.Replace($drill.from, $drill.to)) $Root } $drill.expected
        Write-Host "KILLED $($drill.name)"
    }
    $invocationDrills = @(
        @{ name = "planner-build-auto-detection"; from = 'Arguments = @("--dry-run", "--json"'; to = 'Arguments = @("--release", "--dry-run", "--json"'; expected = "Planner arguments changed" },
        @{ name = "planner-exit-refusal"; from = '$captured.exit_code -eq 0'; to = '$captured.exit_code -eq $captured.exit_code'; expected = "Expected 'planning failed'" },
        @{ name = "planner-timeout-refusal"; from = '-not $captured.timed_out -and '; to = ''; expected = "Expected 'planning failed'" }
    )
    foreach ($drill in $invocationDrills) {
        Assert-StageContract ($DeploymentText.Contains($drill.from)) "Planner invocation mutation anchor missing"
        Expect-StageFailure { Test-QtPlannerInvocation ($DeploymentText.Replace($drill.from, $drill.to)) } $drill.expected
        Write-Host "KILLED $($drill.name)"
    }
}

$requiredInputs = @($capture, $archiveScript)
if (-not $ArchiveOnly) { $requiredInputs += @($stageScript, (Join-Path $buildRoot "sak_utility.exe")) }
foreach ($required in $requiredInputs) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Portable-stage regression test input missing: $required"
    }
}
if (-not $ArchiveOnly -and (Test-Path -LiteralPath $packageRoot)) {
    throw "Portable-stage regression target unexpectedly exists: $packageRoot"
}
$archiveTarget = Join-Path $buildRoot "$packageName-Windows-x64.zip"
$checksumTarget = Join-Path $buildRoot "SHA256SUMS.txt"
$checksumWasPresent = Test-Path -LiteralPath $checksumTarget -PathType Leaf
$checksumBefore = if ($checksumWasPresent) {
    (Get-FileHash -LiteralPath $checksumTarget -Algorithm SHA256).Hash
} else { "" }
if (-not $ArchiveOnly -and -not $ContractOnly) {
    if (Test-Path -LiteralPath $archiveTarget) {
        throw "Portable-stage regression target unexpectedly exists: $archiveTarget"
    }
    if (-not $checksumWasPresent -and (Test-Path -LiteralPath $checksumTarget)) {
        throw "Portable-stage checksum target has an invalid type: $checksumTarget"
    }
}

. $capture
$contractRoot = Join-Path $resolvedBuildDirectory (
    "test_results\portable_stage_contracts\" + [guid]::NewGuid().ToString("D"))
New-Item -ItemType Directory -Path $contractRoot -ErrorAction Stop | Out-Null
if (-not $ArchiveOnly) {
    Test-StageFixtureBuildInputs $contractRoot
    Test-StageFixtureCopyMutants $contractRoot
}
Test-StageReparseBoundary $contractRoot
if (-not $SkipArchiveContracts) {
    Test-ArchiveInjectedCrashOutcomeContract
    Test-CleanReleaseArchive $contractRoot
    Test-ReleaseArchiveCrashRefusal $contractRoot
    Test-ReleaseArchivePreflightPreservation $contractRoot
    Test-ReleaseArchiveLogCleanup $contractRoot
    Test-ReleaseArchiveCompressionFailurePreservesOutputs $contractRoot
    Test-ReleaseArchiveSameNameRearchive $contractRoot
    Test-ReleaseArchiveNewNamePreservesPriorZip $contractRoot
    Test-ReleaseArchiveNewNamePublishFailurePreservesPriorRelease $contractRoot
    Test-ReleaseArchivePublishFailurePreservesOutputs $contractRoot
    Test-ReleaseArchivePendingTransactionRefused $contractRoot
    Test-ReleaseArchivePreparationRecovery $contractRoot
    Test-ReleaseArchiveLockCreateRacePreservesForeignLock $contractRoot
    $lockRaceRed = ''
    try { Test-ReleaseArchiveLockCreateRacePreservesForeignLock $contractRoot -RemoveOwnershipGuard }
    catch { $lockRaceRed = $_.Exception.Message }
    Assert-StageContract ($lockRaceRed.Contains('competing publisher lock')) (
        "Archive lock ownership guard reversal did not go red: $lockRaceRed")
    Write-Host 'KILLED archive lock ownership guard reversal'
    Test-ReleaseArchiveHardCrashRecovery $contractRoot
    Test-ReleaseArchiveCommittedCrashRecovery $contractRoot
    Test-ReleaseArchiveTamperedCrashRecoveryRefused $contractRoot
    Test-ReleaseArchiveRecoveryGuardMutants $contractRoot
    Test-ReleaseArchiveInvalidPriorManifestRefused $contractRoot
}
if ($ArchiveOnly) {
    Write-Host "Archive-only run complete; no app or full portable staging was invoked."
    exit 0
}
$stageText = Get-Content -LiteralPath $stageScript -Raw
$deploymentText = (Get-Content -LiteralPath $deployScript -Raw) + [Environment]::NewLine +
    (Get-Content -LiteralPath $classifierScript -Raw)
$proofHashes = @{}
foreach ($path in @($stageScript, $deployScript, $classifierScript, (Join-Path ([Environment]::SystemDirectory) "icuuc.dll"))) {
    $proofHashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
$buildIcuPath = Join-Path $buildRoot "icuuc.dll"
if (Test-Path -LiteralPath $buildIcuPath -PathType Leaf) {
    $proofHashes[$buildIcuPath] = (Get-FileHash -LiteralPath $buildIcuPath -Algorithm SHA256).Hash
}
Test-PortableRuntimeSelection $stageText $contractRoot
Test-QtInstallPlan $deploymentText $contractRoot
Test-QtPlannerInvocation $deploymentText
Test-CMakeQtInstallLaunch $contractRoot
Test-CMakeToolsInstall $contractRoot
Invoke-BrowserPackagingGuardDrills $stageText (Get-Content -LiteralPath (Join-Path $ProjectRoot 'scripts\verify_portable_release_smoke.ps1') -Raw) $contractRoot
Test-ActualQtPlan
Invoke-QtPackagingGuardDrills $stageText $deploymentText $contractRoot
foreach ($path in $proofHashes.Keys) {
    Assert-StageContract ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $proofHashes[$path]) "Source or existing ICU file changed during packaging contracts: $path"
}
Write-Host "PASS portable selection/package mode and validated Qt install-plan contracts"
if ($ContractOnly) {
    Write-Host "Contract-only run complete; full portable staging was not invoked."
    exit 0
}
$stageSandbox = Join-Path $contractRoot "full-stage"
$stageBuildRoot = Join-Path $stageSandbox "Release"
New-Item -ItemType Directory -Path $stageBuildRoot -ErrorAction Stop | Out-Null
Copy-StageFixtureBuildInputs $buildRoot $stageBuildRoot
$stagePackageRoot = Join-Path $stageBuildRoot $packageName
$stageArchiveTarget = Join-Path $stageBuildRoot "$packageName-Windows-x64.zip"
$stageChecksumTarget = Join-Path $stageBuildRoot "SHA256SUMS.txt"
if (Test-Path -LiteralPath $stageArchiveTarget) {
    throw "Private stage archive target unexpectedly exists: $stageArchiveTarget"
}
[IO.File]::WriteAllBytes($stageArchiveTarget, [byte[]](17, 34, 51, 68, 85))
[IO.File]::WriteAllBytes($stageChecksumTarget, [byte[]](85, 68, 51, 34, 17))
$stageArchiveBefore = (Get-FileHash -LiteralPath $stageArchiveTarget -Algorithm SHA256).Hash
$stageChecksumBefore = (Get-FileHash -LiteralPath $stageChecksumTarget -Algorithm SHA256).Hash
try {
    $hostPath = (Get-Process -Id $PID).Path
    $result = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        "-NoProfile",
        "-NonInteractive",
        "-File",
        $stageScript,
        "-BuildDir",
        $stageBuildRoot,
        "-PackageName",
        $packageName
    ) -TimeoutSeconds 180
    if ($result.timed_out -or $result.exit_code -ne 0) {
        throw ("Portable stage failed. exit=$($result.exit_code), timed_out=$($result.timed_out), " +
            "stdout=$($result.stdout), stderr=$($result.stderr)")
    }

    foreach ($relative in @(
            "sak_utility.exe",
            "sak_browser_control.crx",
            "zlib1.dll",
            "data\ai\providers\providers.json",
            "tools\filesystem\manifest.json")) {
        $path = Join-Path $stagePackageRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Portable stage omitted required regression artifact: $relative"
        }
    }
    # Nothing reads portable.ini (portable mode is the writable data/ folder beside the exe); it
    # was staged into every package anyway.
    Assert-StageContract (-not (Test-Path -LiteralPath (Join-Path $stagePackageRoot 'portable.ini'))) `
        "Portable stage shipped the unused portable.ini"
    $chocolateyLogs = Join-Path $stagePackageRoot 'tools/chocolatey/logs'
    Assert-StageContract ((Test-Path -LiteralPath $chocolateyLogs -PathType Container) -and
        @(Get-ChildItem -LiteralPath $chocolateyLogs -Force -ErrorAction Stop).Count -eq 0) `
        "Portable stage retained Chocolatey runtime logs: $chocolateyLogs"
    Assert-StageContract ((Test-Path -LiteralPath $stageArchiveTarget -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $stageArchiveTarget -Algorithm SHA256).Hash -ceq $stageArchiveBefore)) `
        "Portable stage removed or modified a preexisting ZIP: $stageArchiveTarget"
    Assert-StageContract ((Test-Path -LiteralPath $stageChecksumTarget -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $stageChecksumTarget -Algorithm SHA256).Hash -ceq $stageChecksumBefore)) `
        "Portable stage removed or modified a preexisting checksum manifest: $stageChecksumTarget"

    $hiddenExe = Join-Path $stageBuildRoot 'sak_utility.exe.test-hidden'
    if (Test-Path -LiteralPath $hiddenExe) {
        throw "Portable-stage failure fixture target already exists: $hiddenExe"
    }
    Move-Item -LiteralPath (Join-Path $stageBuildRoot 'sak_utility.exe') -Destination $hiddenExe -ErrorAction Stop
    $failure = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
        '-NoProfile', '-NonInteractive', '-File',
        $stageScript, '-BuildDir', $stageBuildRoot, '-PackageName', "$packageName-Failure"
    ) -TimeoutSeconds 30
    Assert-StageContract (-not $failure.timed_out -and $failure.exit_code -ne 0 -and
        $failure.stderr.Contains('Required file missing')) `
        "Portable-stage missing-app failure did not refuse: $($failure.stderr)"
    Assert-StageContract ((Test-Path -LiteralPath $stageArchiveTarget -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $stageArchiveTarget -Algorithm SHA256).Hash -ceq $stageArchiveBefore)) `
        "Failed portable stage removed or modified a preexisting ZIP: $stageArchiveTarget"
    Assert-StageContract ((Test-Path -LiteralPath $stageChecksumTarget -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $stageChecksumTarget -Algorithm SHA256).Hash -ceq $stageChecksumBefore)) `
        "Failed portable stage removed or modified a preexisting checksum manifest: $stageChecksumTarget"
}
finally {
    if (Test-Path -LiteralPath $stageSandbox -PathType Container) {
        [void](Assert-StageSafeDeleteTarget $stageSandbox "Container")
        Remove-Item -LiteralPath $stageSandbox -Recurse -Force -ErrorAction Stop
    }
}

Assert-StageContract (-not (Test-Path -LiteralPath $archiveTarget)) `
    "Portable stage unexpectedly created an archive target: $archiveTarget"
if ($checksumWasPresent) {
    Assert-StageContract ((Test-Path -LiteralPath $checksumTarget -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $checksumTarget -Algorithm SHA256).Hash -ceq $checksumBefore)) `
        "Portable stage modified an existing checksum manifest: $checksumTarget"
} else {
    Assert-StageContract (-not (Test-Path -LiteralPath $checksumTarget)) `
        "Portable stage unexpectedly created a checksum manifest: $checksumTarget"
}

Write-Host "Portable-stage regression test passed: named child parameters and artifacts verified."
exit 0
