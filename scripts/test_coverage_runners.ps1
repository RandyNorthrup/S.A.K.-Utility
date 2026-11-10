# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
.SYNOPSIS
    Hermetic fail-closed tests for line- and branch-coverage runners.
#>

[CmdletBinding()]
param(
    [string]$LineCoverageScript = '',
    [string]$BranchCoverageScript = '',
    [string]$Case = 'all'
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $PSCommandPath
if ([string]::IsNullOrWhiteSpace($LineCoverageScript)) {
    $LineCoverageScript = Join-Path $ScriptRoot 'run_coverage.ps1'
}
if ([string]::IsNullOrWhiteSpace($BranchCoverageScript)) {
    $BranchCoverageScript = Join-Path $ScriptRoot 'run_branch_coverage.ps1'
}
$ProjectRoot = Split-Path -Parent $ScriptRoot
. (Join-Path $ScriptRoot 'process_capture.ps1')

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Should-Run([string]$Name) {
    return $Case -eq 'all' -or $Case -eq $Name
}

function Set-FakeMode {
    param(
        [int]$OccExit = 0,
        [int]$CtestExit = 0,
        [int]$ProfdataExit = 0,
        [int]$CovReportExit = 0,
        [int]$CovShowExit = 0,
        [int]$LineReportMode = 1,
        [bool]$WriteProfile = $false,
        [bool]$WriteSummary = $true,
        [bool]$WriteShow = $true,
        [string]$LineXml = '',
        [string]$LineHtml = '',
        [string]$Merged = '',
        [string]$CovLog = '',
        [string]$ProfilePath = ''
    )
    $env:SAK_FAKE_OCC_EXIT = [string]$OccExit
    $env:SAK_FAKE_CTEST_EXIT = [string]$CtestExit
    $env:SAK_FAKE_PROFDATA_EXIT = [string]$ProfdataExit
    $env:SAK_FAKE_COV_REPORT_EXIT = [string]$CovReportExit
    $env:SAK_FAKE_COV_SHOW_EXIT = [string]$CovShowExit
    $env:SAK_FAKE_LINE_REPORT_MODE = [string]$LineReportMode
    $env:SAK_FAKE_WRITE_PROFILE = if ($WriteProfile) { '1' } else { '0' }
    $env:SAK_FAKE_WRITE_SUMMARY = if ($WriteSummary) { '1' } else { '0' }
    $env:SAK_FAKE_WRITE_SHOW = if ($WriteShow) { '1' } else { '0' }
    $env:SAK_FAKE_LINE_XML = $LineXml
    $env:SAK_FAKE_LINE_HTML = $LineHtml
    $env:SAK_FAKE_MERGED = $Merged
    $env:SAK_FAKE_COV_LOG = $CovLog
    $env:SAK_FAKE_PROFILE_PATH = $ProfilePath
}

function Invoke-Runner([string]$Script, [string[]]$Arguments) {
    $hostPath = (Get-Process -Id $PID).Path
    $prefix = @(
        '-NoProfile', '-NonInteractive', '-File', $Script
    )
    return Invoke-CapturedProcess -FilePath $hostPath -Arguments ($prefix + $Arguments)
}

function Assert-Failed([object]$Result, [string]$Pattern, [string]$Message) {
    Assert-True ($Result.exit_code -ne 0) "$Message (exit was zero)"
    $diagnostic = $Result.stdout + [Environment]::NewLine + $Result.stderr
    Assert-True ($diagnostic -match $Pattern) "$Message (diagnostic: $diagnostic)"
}

function Initialize-LineBuild([string]$Root) {
    $build = Join-Path $Root 'line-build'
    New-Item -ItemType Directory -Force -Path (Join-Path $build 'tests\RelWithDebInfo') |
        Out-Null
    return $build
}

function Initialize-BranchBuild([string]$Root, [string]$FakeExe) {
    $build = Join-Path $Root 'branch-build'
    $tests = Join-Path $build 'tests'
    New-Item -ItemType Directory -Force -Path $tests | Out-Null
    Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Encoding ascii -Value (
        'ENABLE_COVERAGE:BOOL=ON'
    )
    Copy-Item -LiteralPath $FakeExe -Destination (Join-Path $tests 'test_fake.exe') -Force
    return $build
}

function Invoke-LineCase([string]$Build, [string]$Out, [string]$Occ, [string]$FakeTools) {
    $arguments = @(
        '-TestRegex', '^test_fake$', '-BuildDir', $Build, '-OutDir', $Out,
        '-QtBin', $FakeTools, '-OpenCppCoveragePath', $Occ
    )
    return Invoke-Runner -Script $LineCoverageScript -Arguments $arguments
}

function Invoke-BranchCase {
    param(
        [string]$Build,
        [string]$Out,
        [string]$Ctest,
        [string]$Profdata,
        [string]$Cov,
        [string]$FakeTools
    )
    $arguments = @(
        '-TestRegex', '^test_fake$', '-BuildDir', $Build, '-OutDir', $Out,
        '-QtBin', $FakeTools, '-CtestPath', $Ctest,
        '-LlvmProfdataPath', $Profdata, '-LlvmCovPath', $Cov
    )
    return Invoke-Runner -Script $BranchCoverageScript -Arguments $arguments
}

function New-DirectoryJunction([string]$Path, [string]$Target) {
    $output = & "$env:SystemRoot\System32\cmd.exe" /d /c mklink /J $Path $Target 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create coverage-output junction: $($output -join ' ')"
    }
}

$testRoot = Join-Path $ProjectRoot "build\coverage-runner-tests-$PID"
$outsideRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sak-coverage-outside-$PID"
$junctions = [System.Collections.Generic.List[string]]::new()
$fakeVariables = @(
    'SAK_FAKE_OCC_EXIT', 'SAK_FAKE_CTEST_EXIT', 'SAK_FAKE_PROFDATA_EXIT',
    'SAK_FAKE_COV_REPORT_EXIT', 'SAK_FAKE_COV_SHOW_EXIT', 'SAK_FAKE_LINE_REPORT_MODE',
    'SAK_FAKE_WRITE_PROFILE', 'SAK_FAKE_WRITE_SUMMARY', 'SAK_FAKE_WRITE_SHOW',
    'SAK_FAKE_LINE_XML', 'SAK_FAKE_LINE_HTML', 'SAK_FAKE_MERGED', 'SAK_FAKE_COV_LOG',
    'SAK_FAKE_PROFILE_PATH', 'SAK_FAKE_CTEST_PATH'
)

try {
    foreach ($path in @($testRoot, $outsideRoot)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
        }
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    $fakeTools = Join-Path $testRoot 'fake-tools'
    New-Item -ItemType Directory -Path $fakeTools -Force | Out-Null

    $fakeSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

public static class FakeCoverageTool {
    static int ExitValue(string name) {
        int value;
        return Int32.TryParse(Environment.GetEnvironmentVariable(name), out value) ? value : 0;
    }
    static void WriteLineReport() {
        string path = Environment.GetEnvironmentVariable("SAK_FAKE_LINE_XML");
        int mode = ExitValue("SAK_FAKE_LINE_REPORT_MODE");
        if (mode == 0 || String.IsNullOrEmpty(path)) return;
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        string xml = mode == 2
            ? "<coverage><packages /></coverage>"
            : "<coverage><packages><package><classes><class filename=\"C:\\repo\\src\\core\\fake.cpp\"><lines><line hits=\"1\" /></lines></class></classes></package></packages></coverage>";
        File.WriteAllText(path, xml);
        string html = Environment.GetEnvironmentVariable("SAK_FAKE_LINE_HTML");
        if (!String.IsNullOrEmpty(html) && mode != 3) {
            Directory.CreateDirectory(html);
            File.WriteAllText(Path.Combine(html, "index.html"), "fresh");
        }
    }
    static int RunCtest() {
        if (Environment.GetEnvironmentVariable("SAK_FAKE_WRITE_PROFILE") == "1") {
            File.WriteAllText(Environment.GetEnvironmentVariable("SAK_FAKE_PROFILE_PATH"), "raw");
        }
        Console.WriteLine("fake ctest");
        return ExitValue("SAK_FAKE_CTEST_EXIT");
    }
    static int RunOcc() {
        WriteLineReport();
        int ownExit = ExitValue("SAK_FAKE_OCC_EXIT");
        if (ownExit != 0) return ownExit;
        Process child = Process.Start(Environment.GetEnvironmentVariable("SAK_FAKE_CTEST_PATH"));
        child.WaitForExit();
        return child.ExitCode;
    }
    static int RunProfdata() {
        int result = ExitValue("SAK_FAKE_PROFDATA_EXIT");
        File.WriteAllText(Environment.GetEnvironmentVariable("SAK_FAKE_MERGED"), "merged");
        return result;
    }
    static int RunCov(string[] args) {
        string log = Environment.GetEnvironmentVariable("SAK_FAKE_COV_LOG");
        File.AppendAllText(log, String.Join("|", args) + Environment.NewLine);
        bool report = args.Length > 0 && args[0] == "report";
        int result = ExitValue(report ? "SAK_FAKE_COV_REPORT_EXIT" : "SAK_FAKE_COV_SHOW_EXIT");
        if (result != 0) return result;
        if (report && Environment.GetEnvironmentVariable("SAK_FAKE_WRITE_SUMMARY") == "1") {
            Console.WriteLine("Filename Regions Missed Regions Cover");
        }
        if (!report && Environment.GetEnvironmentVariable("SAK_FAKE_WRITE_SHOW") == "1") {
            Console.WriteLine("C:\\repo\\src\\core\\fake.cpp:");
            Console.WriteLine("   10|      1|if (value) {");
            Console.WriteLine("     |       |  Branch (10:5): [True: 0, False: 1]");
        }
        return 0;
    }
    public static int Main(string[] args) {
        string name = Path.GetFileNameWithoutExtension(Assembly.GetExecutingAssembly().Location);
        name = name.ToLowerInvariant();
        if (name == "opencppcoverage") return RunOcc();
        if (name == "ctest") return RunCtest();
        if (name == "llvm-profdata") return RunProfdata();
        if (name == "llvm-cov") return RunCov(args);
        return 97;
    }
}
'@
    $baseExe = Join-Path $fakeTools 'fake-coverage-tool.exe'
    $sourcePath = Join-Path $fakeTools 'fake-coverage-tool.cs'
    Set-Content -LiteralPath $sourcePath -Value $fakeSource -Encoding ascii
    $csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path -LiteralPath $csc -PathType Leaf)) {
        throw "C# compiler not found: $csc"
    }
    $compile = Invoke-CapturedProcess -FilePath $csc -Arguments @(
        '/nologo', '/target:exe', "/out:$baseExe", $sourcePath
    )
    if ($compile.exit_code -ne 0 -or -not (Test-Path -LiteralPath $baseExe -PathType Leaf)) {
        throw "Could not compile fake coverage tool: $($compile.stderr)"
    }
    foreach ($name in @('OpenCppCoverage.exe', 'ctest.exe', 'llvm-profdata.exe', 'llvm-cov.exe')) {
        Copy-Item -LiteralPath $baseExe -Destination (Join-Path $fakeTools $name) -Force
    }
    $occ = Join-Path $fakeTools 'OpenCppCoverage.exe'
    $ctest = Join-Path $fakeTools 'ctest.exe'
    $profdata = Join-Path $fakeTools 'llvm-profdata.exe'
    $cov = Join-Path $fakeTools 'llvm-cov.exe'
    $env:SAK_FAKE_CTEST_PATH = $ctest

    $lineBuild = Initialize-LineBuild -Root $testRoot
    if (Should-Run 'line_stale') {
        $out = Join-Path $lineBuild 'line-stale'
        New-Item -ItemType Directory -Force -Path (Join-Path $out 'html') | Out-Null
        $staleXml = '<coverage><packages><package><classes><class filename="C:\repo\src\core\' +
            'stale.cpp"><lines><line hits="1" /></lines></class></classes></package></packages>' +
            '</coverage>'
        Set-Content -LiteralPath (Join-Path $out 'coverage.xml') -Value $staleXml
        Set-Content -LiteralPath (Join-Path $out 'html\index.html') -Value 'stale'
        Set-FakeMode -LineReportMode 0 -LineXml (Join-Path $out 'coverage.xml') -LineHtml (
            Join-Path $out 'html'
        )
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-Failed $result 'produced no report' 'Stale line report was accepted'
        Assert-True (-not (Test-Path (Join-Path $out 'coverage.xml'))) 'Stale XML survived'
        Assert-True (-not (Test-Path (Join-Path $out 'html'))) 'Stale HTML survived'
    }
    if (Should-Run 'line_child_failure') {
        $out = Join-Path $lineBuild 'line-child-failure'
        Set-FakeMode -CtestExit 23 -LineReportMode 1 -LineXml (
            Join-Path $out 'coverage.xml'
        ) -LineHtml (Join-Path $out 'html')
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-Failed $result 'exit code 23' 'Child CTest failure was not propagated through OCC'
        Assert-True (-not (Test-Path (Join-Path $out 'coverage.xml'))) 'Failed run kept XML'
    }
    if (Should-Run 'line_tool_failure') {
        $out = Join-Path $lineBuild 'line-tool-failure'
        Set-FakeMode -OccExit 19 -LineReportMode 1 -LineXml (
            Join-Path $out 'coverage.xml'
        ) -LineHtml (Join-Path $out 'html')
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-Failed $result 'exit code 19' 'OpenCppCoverage failure was not propagated'
        Assert-True (-not (Test-Path (Join-Path $out 'coverage.xml'))) 'Failed OCC kept XML'
    }
    if (Should-Run 'line_empty') {
        $out = Join-Path $lineBuild 'line-empty'
        Set-FakeMode -LineReportMode 2 -LineXml (Join-Path $out 'coverage.xml') -LineHtml (
            Join-Path $out 'html'
        )
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-Failed $result 'zero measured' 'Empty line report was accepted'
    }
    if (Should-Run 'line_missing_html') {
        $out = Join-Path $lineBuild 'line-missing-html'
        Set-FakeMode -LineReportMode 3 -LineXml (Join-Path $out 'coverage.xml') -LineHtml (
            Join-Path $out 'html'
        )
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-Failed $result 'HTML report' 'Missing line-coverage HTML was accepted'
    }
    if (Should-Run 'line_positive') {
        $out = Join-Path $lineBuild 'line-positive'
        Set-FakeMode -LineReportMode 1 -LineXml (Join-Path $out 'coverage.xml') -LineHtml (
            Join-Path $out 'html'
        )
        $result = Invoke-LineCase $lineBuild $out $occ $fakeTools
        Assert-True ($result.exit_code -eq 0) "Valid line coverage failed: $($result.stderr)"
        Assert-True (Test-Path (Join-Path $out 'coverage.xml')) 'Fresh line XML missing'
        Assert-True (Test-Path (Join-Path $out 'html\index.html')) 'Fresh line HTML missing'
    }

    $branchBuild = Initialize-BranchBuild -Root $testRoot -FakeExe $env:ComSpec
    function Set-BranchMode {
        param(
            [string]$Out,
            [int]$CtestExit = 0,
            [int]$ProfdataExit = 0,
            [int]$ReportExit = 0,
            [int]$ShowExit = 0,
            [bool]$Profile = $true,
            [bool]$Summary = $true,
            [bool]$Show = $true
        )
        $mode = @{
            CtestExit = $CtestExit
            ProfdataExit = $ProfdataExit
            CovReportExit = $ReportExit
            CovShowExit = $ShowExit
            WriteProfile = $Profile
            WriteSummary = $Summary
            WriteShow = $Show
            Merged = Join-Path $Out 'merged.profdata'
            CovLog = Join-Path $Out 'cov-args.log'
            ProfilePath = Join-Path $Out 'sak-1234.profraw'
        }
        Set-FakeMode @mode
    }
    if (Should-Run 'branch_stale') {
        $out = Join-Path $branchBuild 'branch-stale'
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        Set-Content (Join-Path $out 'BRANCH-SUMMARY.txt') 'stale'
        Set-Content (Join-Path $out 'MISSED-BRANCH-REPORT.txt') 'stale'
        Set-BranchMode $out -Profile $false
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'No .profraw' 'Stale branch reports were accepted'
        Assert-True (-not (Test-Path (Join-Path $out 'BRANCH-SUMMARY.txt'))) (
            'Stale branch summary survived'
        )
    }
    if (Should-Run 'branch_ctest_failure') {
        $out = Join-Path $branchBuild 'branch-ctest-failure'
        Set-BranchMode $out -CtestExit 17
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'CTest run failed with exit code 17' 'Failed CTest produced reports'
        Assert-True (-not (Test-Path (Join-Path $out 'BRANCH-SUMMARY.txt'))) (
            'Failed CTest kept branch summary'
        )
    }
    if (Should-Run 'branch_profdata_failure') {
        $out = Join-Path $branchBuild 'branch-profdata-failure'
        Set-BranchMode $out -ProfdataExit 18
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'profdata merge failed.*18' 'Failed profdata produced reports'
    }
    if (Should-Run 'branch_report_failure') {
        $out = Join-Path $branchBuild 'branch-report-failure'
        Set-BranchMode $out -ReportExit 19
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'cov report failed.*19' 'Failed llvm-cov report was accepted'
    }
    if (Should-Run 'branch_show_failure') {
        $out = Join-Path $branchBuild 'branch-show-failure'
        Set-BranchMode $out -ShowExit 20
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'cov show failed.*20' 'Failed llvm-cov show was accepted'
        Assert-True (-not (Test-Path (Join-Path $out 'BRANCH-SUMMARY.txt'))) (
            'Partial branch summary was published'
        )
    }
    if (Should-Run 'branch_empty_summary') {
        $out = Join-Path $branchBuild 'branch-empty-summary'
        Set-BranchMode $out -Summary $false
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'no summary output' 'Empty llvm-cov summary was accepted'
    }
    if (Should-Run 'branch_empty_show') {
        $out = Join-Path $branchBuild 'branch-empty-show'
        Set-BranchMode $out -Show $false
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-Failed $result 'no annotated source output' 'Empty llvm-cov show was accepted'
    }
    if (Should-Run 'branch_positive') {
        $out = Join-Path $branchBuild 'branch-positive'
        Set-BranchMode $out
        $result = Invoke-BranchCase $branchBuild $out $ctest $profdata $cov $fakeTools
        Assert-True ($result.exit_code -eq 0) "Valid branch coverage failed: $($result.stderr)"
        $summary = Join-Path $out 'BRANCH-SUMMARY.txt'
        $report = Join-Path $out 'MISSED-BRANCH-REPORT.txt'
        Assert-True (Test-Path $summary) 'Fresh branch summary missing'
        Assert-True ((Get-Content -Raw $report) -match 'TOTAL MISSED BRANCHES: 1') (
            'Fresh missed-branch report is wrong'
        )
        $covArgs = Get-Content -Raw (Join-Path $out 'cov-args.log')
        $expectedFilter = '(Qt[\\/]|vcpkg[\\/]|tests[\\/]|third_party[\\/])'
        $filterCount = ([regex]::Matches($covArgs, [regex]::Escape($expectedFilter))).Count
        Assert-True ($filterCount -eq 2) (
            "First-party coverage filter was not preserved for report and show: " +
            "count=$filterCount expected=$expectedFilter actual=$covArgs"
        )
    }

    foreach ($runner in @('line', 'branch')) {
        $isLine = $runner -eq 'line'
        $build = if ($isLine) { $lineBuild } else { $branchBuild }
        if (Should-Run ($runner + '_outside')) {
            $outside = Join-Path $outsideRoot ($runner + '-outside')
            New-Item -ItemType Directory -Force -Path $outside | Out-Null
            $sentinel = Join-Path $outside 'sentinel.txt'
            Set-Content -LiteralPath $sentinel -Value 'keep' -Encoding ascii
            Set-FakeMode -LineReportMode 0
            if (-not $isLine) { Set-BranchMode $outside }
            $result = if ($isLine) {
                Invoke-LineCase $build $outside $occ $fakeTools
            } else {
                Invoke-BranchCase $build $outside $ctest $profdata $cov $fakeTools
            }
            Assert-Failed $result 'strict descendant' "Outside $runner output was accepted"
            Assert-True ((Get-Content -Raw $sentinel).Trim() -eq 'keep') (
                "Outside $runner output was modified"
            )
        }
        if (Should-Run ($runner + '_root')) {
            Set-FakeMode -LineReportMode 0
            if (-not $isLine) { Set-BranchMode $build }
            $result = if ($isLine) {
                Invoke-LineCase $build $build $occ $fakeTools
            } else {
                Invoke-BranchCase $build $build $ctest $profdata $cov $fakeTools
            }
            Assert-Failed $result 'strict descendant' "Build-root $runner output was accepted"
        }
        if (Should-Run ($runner + '_file_parent')) {
            $fileParent = Join-Path $build ($runner + '-file-parent')
            Set-Content -LiteralPath $fileParent -Value 'not a directory' -Encoding ascii
            $fileOutput = Join-Path $fileParent 'coverage'
            Set-FakeMode -LineReportMode 0
            if (-not $isLine) { Set-BranchMode $fileOutput }
            $result = if ($isLine) {
                Invoke-LineCase $build $fileOutput $occ $fakeTools
            } else {
                Invoke-BranchCase $build $fileOutput $ctest $profdata $cov $fakeTools
            }
            Assert-Failed $result 'component is not a directory' (
                "File-parent $runner output was accepted"
            )
        }
        if (Should-Run ($runner + '_build_reparse')) {
            $outsideBuildParent = Join-Path $outsideRoot ($runner + '-build-parent')
            New-Item -ItemType Directory -Force -Path $outsideBuildParent | Out-Null
            $outsideBuild = if ($isLine) {
                Initialize-LineBuild -Root $outsideBuildParent
            } else {
                Initialize-BranchBuild -Root $outsideBuildParent -FakeExe $env:ComSpec
            }
            $buildJunction = Join-Path $testRoot ($runner + '-build-reparse')
            New-DirectoryJunction $buildJunction $outsideBuild
            $junctions.Add($buildJunction)
            $reparseOutput = Join-Path $buildJunction 'coverage'
            Set-FakeMode -LineReportMode 0
            if (-not $isLine) { Set-BranchMode $reparseOutput }
            $result = if ($isLine) {
                Invoke-LineCase $buildJunction $reparseOutput $occ $fakeTools
            } else {
                Invoke-BranchCase $buildJunction $reparseOutput $ctest $profdata $cov $fakeTools
            }
            Assert-Failed $result 'build directory is not a direct, non-reparse directory' (
                "Reparse BuildDir for $runner was accepted"
            )
        }
        if (Should-Run ($runner + '_reparse')) {
            $target = Join-Path $outsideRoot ($runner + '-reparse-target')
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            $sentinel = Join-Path $target 'sentinel.txt'
            Set-Content -LiteralPath $sentinel -Value 'keep' -Encoding ascii
            $junction = Join-Path $build ($runner + '-reparse-output')
            New-DirectoryJunction $junction $target
            $junctions.Add($junction)
            Set-FakeMode -LineReportMode 0
            if (-not $isLine) { Set-BranchMode $junction }
            $result = if ($isLine) {
                Invoke-LineCase $build $junction $occ $fakeTools
            } else {
                Invoke-BranchCase $build $junction $ctest $profdata $cov $fakeTools
            }
            Assert-Failed $result 'crosses a reparse point' "Reparse $runner output was accepted"
            Assert-True ((Get-Content -Raw $sentinel).Trim() -eq 'keep') (
                "Reparse $runner output touched outside target"
            )
        }
    }

    Write-Host "Coverage runner tests passed under $($PSVersionTable.PSVersion)."
}
finally {
    foreach ($junction in $junctions) {
        if (Test-Path -LiteralPath $junction) {
            & "$env:SystemRoot\System32\cmd.exe" /d /c rmdir $junction | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Could not remove test junction: $junction"
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
