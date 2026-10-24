<#
.SYNOPSIS
    Splits the configured Release build and its T2 CTest selection into CI shards.

.DESCRIPTION
    Building every target of this tree on one hosted Windows runner takes well over an hour,
    because each of the ~250 test executables compiles its own copy of the first-party sources
    and links with LTCG. CI therefore spreads the build across several runners. Every runner
    configures the same tree and runs this script with the same inputs, so every runner
    computes the same plan; the plan digest it prints lets the CI summary job prove that.

    Each shard builds the BASE set (sak_utility, the helpers it depends on and the deployment
    targets that stage data next to it), so build/Release on every shard is populated the way
    a full local build populates it. The remaining targets are distributed by compile-unit
    count. A T2 test runs on the shard that builds every executable its command references.

    The plan fails closed. Every target in ALL_BUILD must be built by some shard, every selected
    test must land on exactly one shard, and an empty selection or an unparseable project is an
    error rather than an empty plan.

.PARAMETER BuildDir
    The configured Visual Studio build directory.

.PARAMETER ShardCount
    Number of shards.

.PARAMETER ShardIndex
    Zero-based shard to emit. Omit to print only the summary.

.PARAMETER LabelRegex
    CTest label regex for the tier this plan serves.

.PARAMETER OutFile
    Where to write this shard's JSON (targets, tests, digest).

.PARAMETER SelectedTestsOnly
    Plan only the base set and the executables the selected tests run, instead of the whole
    default build. For a tier (T4) whose job is its own tests; the T2 plan never passes it,
    because T2 is also the proof that every target builds.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][ValidateRange(1, 64)][int]$ShardCount,
    [ValidateRange(-1, 63)][int]$ShardIndex = -1,
    [Parameter(Mandatory = $true)][string]$LabelRegex,
    [string]$OutFile = "",
    [switch]$SelectedTestsOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# An executable link under /LTCG performs the code generation that /GL deferred, so a small
# test target still costs noticeably more than its compile-unit count. This weight is a
# balancing estimate only; it never decides WHETHER a target is built.
$LinkWeight = 3
$BaseRoot = "sak_utility"

$buildFull = (Resolve-Path -LiteralPath $BuildDir).Path
$allBuild = Join-Path $buildFull "ALL_BUILD.vcxproj"
if (-not (Test-Path -LiteralPath $allBuild -PathType Leaf)) {
    throw "ALL_BUILD.vcxproj not found in $buildFull; configure with the Visual Studio generator first."
}

$msbuildNs = @{ m = "http://schemas.microsoft.com/developer/msbuild/2003" }

function Read-Project([string]$path) {
    [xml]$xml = [System.IO.File]::ReadAllText($path)
    $units = @(Select-Xml -Xml $xml -XPath "//m:ItemGroup/m:ClCompile[@Include]" -Namespace $msbuildNs).Count
    $refs = @(Select-Xml -Xml $xml -XPath "//m:ProjectReference[@Include]" -Namespace $msbuildNs |
        ForEach-Object { $_.Node.GetAttribute("Include") })
    $exe = @(Select-Xml -Xml $xml -XPath "//m:PropertyGroup/m:ConfigurationType" -Namespace $msbuildNs |
        Where-Object { $_.Node.InnerText -eq "Application" }).Count -gt 0
    return [pscustomobject]@{ Units = $units; RefPaths = $refs; IsExe = $exe }
}

# Every target the default build produces, keyed by name, with its direct references.
$projects = @{}
$pending = [System.Collections.Generic.Queue[string]]::new()
foreach ($ref in (Read-Project $allBuild).RefPaths) { $pending.Enqueue($ref) }
$allTargets = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
foreach ($ref in (Read-Project $allBuild).RefPaths) {
    $name = [IO.Path]::GetFileNameWithoutExtension($ref)
    if ($name -ne "ZERO_CHECK") { $null = $allTargets.Add($name) }
}
while ($pending.Count -gt 0) {
    $path = $pending.Dequeue()
    $name = [IO.Path]::GetFileNameWithoutExtension($path)
    if ($name -eq "ZERO_CHECK" -or $projects.ContainsKey($name)) { continue }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Referenced project missing: $path" }
    $info = Read-Project $path
    $deps = @($info.RefPaths | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) } |
        Where-Object { $_ -ne "ZERO_CHECK" })
    $projects[$name] = [pscustomobject]@{ Units = $info.Units; Deps = $deps; IsExe = $info.IsExe }
    foreach ($ref in $info.RefPaths) { $pending.Enqueue($ref) }
}
if ($allTargets.Count -eq 0) { throw "ALL_BUILD references no targets; refusing an empty plan." }
if (-not $projects.ContainsKey($BaseRoot)) { throw "Base target $BaseRoot is not part of ALL_BUILD." }

$closureCache = @{}
# The leading comma on each return keeps PowerShell from unrolling the set into an array.
function Get-Closure([string]$name) {
    if ($closureCache.ContainsKey($name)) { return , $closureCache[$name] }
    $set = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $stack = [System.Collections.Generic.Stack[string]]::new()
    $stack.Push($name)
    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        if (-not $set.Add($current)) { continue }
        foreach ($dep in $projects[$current].Deps) { $stack.Push($dep) }
    }
    $closureCache[$name] = $set
    return , $set
}

function Get-Cost([System.Collections.Generic.HashSet[string]]$names) {
    $cost = 0
    foreach ($n in $names) {
        $cost += $projects[$n].Units
        if ($projects[$n].IsExe) { $cost += $LinkWeight }
    }
    return $cost
}

# BASE: sak_utility plus every compile-free target of the default build (the staging and
# deployment targets), with their closures. Every shard builds it.
$baseRoots = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
$null = $baseRoots.Add($BaseRoot)
foreach ($t in $allTargets) {
    if ($projects[$t].Units -eq 0) { $null = $baseRoots.Add($t) }
}
$base = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($root in $baseRoots) { $base.UnionWith((Get-Closure $root)) }

# The tier's tests, and the build executables each one's Release command references.
$ctest = (Get-Command ctest -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$selection = (& $ctest --test-dir $buildFull -C Release --show-only=json-v1 -L $LabelRegex) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "ctest --show-only failed with exit code $LASTEXITCODE" }
$selectedNames = @(($selection | ConvertFrom-Json).tests | ForEach-Object { [string]$_.name } | Sort-Object -Unique)
if ($selectedNames.Count -eq 0) { throw "Label regex '$LabelRegex' selects no tests; refusing an empty plan." }
$selectedSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$selectedNames, [StringComparer]::Ordinal)

$required = @{}
$exeRe = [regex]'[/\\]Release[/\\](?<name>[A-Za-z0-9_.+-]+)\.exe"'
$addTestRe = [regex]'^\s*add_test\(\[=\[(?<name>[^\]]+)\]=\](?<rest>.*)$'
$testFiles = @(Get-ChildItem -LiteralPath $buildFull -Recurse -Filter CTestTestfile.cmake -File)
foreach ($file in $testFiles) {
    foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
        $m = $addTestRe.Match($line)
        if (-not $m.Success) { continue }
        $name = $m.Groups["name"].Value
        if (-not $selectedSet.Contains($name)) { continue }
        if (-not $required.ContainsKey($name)) {
            $required[$name] = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        }
        foreach ($exe in $exeRe.Matches($m.Groups["rest"].Value)) {
            $target = $exe.Groups["name"].Value
            if ($projects.ContainsKey($target)) { $null = $required[$name].Add($target) }
        }
    }
}
foreach ($name in $selectedNames) {
    if (-not $required.ContainsKey($name)) { throw "Selected test $name has no add_test line in any CTestTestfile.cmake." }
}

# Work items: tests that share a non-base executable are one item; targets no selected test
# needs are items of their own, so the union of shards is still the whole default build.
$owner = @{}
$items = [System.Collections.Generic.List[object]]::new()
foreach ($name in $selectedNames) {
    $own = @($required[$name] | Where-Object { -not $base.Contains($_) })
    if ($own.Count -eq 0) { continue }
    $merged = $null
    foreach ($t in $own) { if ($owner.ContainsKey($t)) { $merged = $owner[$t]; break } }
    if ($null -eq $merged) {
        $merged = [pscustomobject]@{
            Targets = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
            Tests = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        }
        $items.Add($merged)
    }
    $null = $merged.Tests.Add($name)
    foreach ($t in $own) {
        if ($owner.ContainsKey($t) -and -not [object]::ReferenceEquals($owner[$t], $merged)) {
            $other = $owner[$t]
            foreach ($x in $other.Targets) { $null = $merged.Targets.Add($x); $owner[$x] = $merged }
            foreach ($x in $other.Tests) { $null = $merged.Tests.Add($x) }
            $null = $items.Remove($other)
        }
        $null = $merged.Targets.Add($t)
        $owner[$t] = $merged
    }
}
foreach ($t in $allTargets) {
    if ($SelectedTestsOnly -or $base.Contains($t) -or $owner.ContainsKey($t)) { continue }
    $solo = [pscustomobject]@{
        Targets = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
        Tests = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    }
    $null = $solo.Targets.Add($t)
    $owner[$t] = $solo
    $items.Add($solo)
}

foreach ($item in $items) {
    $closure = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($t in $item.Targets) { $closure.UnionWith((Get-Closure $t)) }
    $closure.ExceptWith($base)
    $item | Add-Member -NotePropertyName Closure -NotePropertyValue $closure
    $item | Add-Member -NotePropertyName Cost -NotePropertyValue (Get-Cost $closure)
    $item | Add-Member -NotePropertyName Key -NotePropertyValue (@($item.Targets)[0])
}

# Longest-processing-time greedy: biggest item first, onto the shard whose load grows least.
$shards = @(for ($i = 0; $i -lt $ShardCount; $i++) {
        [pscustomobject]@{
            Index = $i
            Built = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            Targets = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
            Tests = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
            Load = 0
        }
    })
foreach ($item in ($items | Sort-Object -Property @{ Expression = "Cost"; Descending = $true }, @{ Expression = "Key"; Descending = $false })) {
    $best = $null
    $bestLoad = [int]::MaxValue
    foreach ($shard in $shards) {
        $extra = [System.Collections.Generic.HashSet[string]]::new($item.Closure, [StringComparer]::Ordinal)
        $extra.ExceptWith($shard.Built)
        $load = $shard.Load + (Get-Cost $extra)
        if ($load -lt $bestLoad) { $best = $shard; $bestLoad = $load }
    }
    $best.Built.UnionWith($item.Closure)
    $best.Load = $bestLoad
    foreach ($t in $item.Targets) { $null = $best.Targets.Add($t) }
    foreach ($t in $item.Tests) { $null = $best.Tests.Add($t) }
}

# Tests that need only the base set run on the shard with the fewest tests (ties: lowest index).
foreach ($name in $selectedNames) {
    $own = @($required[$name] | Where-Object { -not $base.Contains($_) })
    if ($own.Count -gt 0) { continue }
    $target = $shards | Sort-Object -Property @{ Expression = { $_.Tests.Count } }, Index | Select-Object -First 1
    $null = $target.Tests.Add($name)
}

# Fail closed: whole default build covered (unless -SelectedTestsOnly), every selected test
# exactly once.
$covered = [System.Collections.Generic.HashSet[string]]::new($base, [StringComparer]::Ordinal)
foreach ($shard in $shards) { $covered.UnionWith($shard.Built) }
$missingTargets = @($allTargets | Where-Object { -not $covered.Contains($_) })
if (-not $SelectedTestsOnly -and $missingTargets.Count -gt 0) {
    throw "Plan leaves default-build targets unbuilt: $($missingTargets -join ', ')"
}
$assigned = @($shards | ForEach-Object { @($_.Tests) })
if ($assigned.Count -ne $selectedNames.Count -or @($assigned | Sort-Object -Unique).Count -ne $selectedNames.Count) {
    throw "Plan assigned $($assigned.Count) tests for $($selectedNames.Count) selected; each must run exactly once."
}

$baseCost = Get-Cost $base
$canonical = ($shards | ForEach-Object {
        "$($_.Index):" + (@($_.Targets) -join ",") + "|" + (@($_.Tests) -join ",")
    }) -join "`n"
$sha = [System.Security.Cryptography.SHA256]::Create()
$digest = -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes(
                "$LabelRegex|selectedOnly=$([bool]$SelectedTestsOnly)`n" + (@($baseRoots) -join ",") + "`n" + $canonical)) | ForEach-Object { $_.ToString("x2") })

Write-Host ("Build plan: {0} default-build targets, {1} in the base set (cost {2}), {3} selected tests, {4} shards" -f
    $allTargets.Count, $base.Count, $baseCost, $selectedNames.Count, $ShardCount)
foreach ($shard in $shards) {
    Write-Host ("  shard {0}: cost {1} (+ base {2}), {3} targets, {4} tests" -f
        $shard.Index, $shard.Load, $baseCost, $shard.Targets.Count, $shard.Tests.Count)
}
Write-Host "Plan digest: $digest"

if ($ShardIndex -ge 0) {
    if ($ShardIndex -ge $ShardCount) { throw "ShardIndex $ShardIndex is outside 0..$($ShardCount - 1)." }
    $mine = $shards[$ShardIndex]
    $buildTargets = @(@($baseRoots) + @($mine.Targets) | Sort-Object -Unique)
    $result = [ordered]@{
        digest = $digest
        shard_index = $ShardIndex
        shard_count = $ShardCount
        label_regex = $LabelRegex
        build_targets = $buildTargets
        tests = @($mine.Tests)
        tests_regex = "^(" + (@($mine.Tests) -join "|") + ")$"
    }
    $json = $result | ConvertTo-Json -Depth 4
    if ($OutFile) {
        # Resolve against the PowerShell location, not the .NET process directory (AGENTS.md).
        $outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
        [System.IO.File]::WriteAllText($outPath, $json)
        Write-Host "Shard $ShardIndex plan written to $OutFile"
    } else {
        $json
    }
}
