[CmdletBinding()]
param([ValidateSet('All', 'Portable', 'Ledger')][string]$Scope = 'All')

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $PSCommandPath
$ProjectRoot = Split-Path -Parent $ScriptRoot
. (Join-Path $ScriptRoot 'owner_only_staging_file.ps1')

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-FalseGreenFixturePath([string]$Nonce) {
    Assert-True ($Nonce -cmatch '^[0-9a-f]{32}$') 'Invalid false-green fixture nonce'
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    return Join-Path $temporary ('sak-false-green-command-tests-' + $Nonce)
}

function Assert-FalseGreenFixtureRoot([string]$Root) {
    $leaf = [IO.Path]::GetFileName($Root)
    Assert-True ($leaf -cmatch '^sak-false-green-command-tests-(?<nonce>[0-9a-f]{32})$') 'Unsafe false-green fixture root'
    $expected = Get-FalseGreenFixturePath $Matches.nonce
    Assert-True ($Root -ceq $expected -and [IO.Path]::GetFullPath($Root) -ceq $Root) 'Unsafe false-green fixture containment'
    Assert-DirectStagingPath $Root
}

function New-FalseGreenFixtureRoot([string]$Nonce = ([guid]::NewGuid().ToString('N'))) {
    $root = Get-FalseGreenFixturePath $Nonce
    Assert-FalseGreenFixtureRoot $root
    # New-Item without Force refuses an existing file or directory. No prior PID-named
    # tree is selected, and a failed creation never hands cleanup authority to the caller.
    [void](New-Item -ItemType Directory -Path $root -ErrorAction Stop)
    return $root
}

function Remove-FalseGreenFixtureRoot([string]$Root) {
    Assert-FalseGreenFixtureRoot $Root
    Assert-True (Test-Path -LiteralPath $Root -PathType Container) 'Owned false-green fixture disappeared'
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction Stop
}

function Test-FalseGreenFixtureOwnership {
    $nonce = [guid]::NewGuid().ToString('N')
    $owned = New-FalseGreenFixtureRoot $nonce
    $canary = Join-Path $owned 'existing-directory-canary.txt'
    $bytes = [Text.Encoding]::ASCII.GetBytes('Owned proof canary: preserve through every refused operation.')
    [IO.File]::WriteAllBytes($canary, $bytes)
    $link = Get-FalseGreenFixturePath ([guid]::NewGuid().ToString('N'))
    try {
        $refused = $false
        try { [void](New-FalseGreenFixtureRoot $nonce) } catch { $refused = $true }
        Assert-True $refused 'Existing fixture directory was reused'
        foreach ($bad in @(
            (Join-Path $ProjectRoot ([IO.Path]::GetFileName($owned))),
            ($owned + '\..\' + [IO.Path]::GetFileName($owned)),
            (Join-Path ([IO.Path]::GetDirectoryName($owned)) 'sak-false-green-command-tests-1234')
        )) {
            $failure = ''
            try { Remove-FalseGreenFixtureRoot $bad } catch { $failure = $_.Exception.Message }
            Assert-True ($failure.StartsWith('Unsafe false-green fixture')) 'Unsafe cleanup did not refuse its exact boundary'
        }
        [void](New-Item -ItemType Junction -Path $link -Target $owned -ErrorAction Stop)
        $failure = ''
        try { Remove-FalseGreenFixtureRoot $link } catch { $failure = $_.Exception.Message }
        Assert-True ($failure.Contains('reparse point')) 'Cleanup accepted a substituted root junction'
        Assert-True ((Get-Item -LiteralPath $link -Force).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Refused root junction was removed'
        Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($canary)) -ceq
            [Convert]::ToBase64String($bytes)) 'Existing directory canary was overwritten or deleted'
    } finally {
        if (Test-Path -LiteralPath $link) {
            # This exact link was created above; unlink only, with no recursive traversal.
            Assert-True ([IO.Path]::GetDirectoryName($link) -ceq [IO.Path]::GetDirectoryName($owned)) 'Unsafe ownership-proof link'
            Assert-True ((Get-Item -LiteralPath $link -Force).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) 'Ownership-proof link changed type'
            [IO.Directory]::Delete($link)
        }
        Remove-FalseGreenFixtureRoot $owned
    }
    Assert-True (-not (Test-Path -LiteralPath $owned)) 'Owned fixture cleanup left its directory'
    Write-Host 'PASS false-green fixture ownership: fresh GUID, no reuse, exact containment, root reparse refusal, preserved canary, owned cleanup'
}

function Invoke-Script([string]$Path, [string[]]$Arguments) {
    $hostPath = (Get-Process -Id $PID).Path
    $allArguments = @(
        '-NoProfile', '-NonInteractive', '-File', $Path
    ) + $Arguments
    return Invoke-CapturedProcess -FilePath $hostPath -Arguments $allArguments
}

function Get-PolicyTargetFunctionAst([string]$Path, [string]$Name) {
    $source = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
    Assert-True (@($errors).Count -eq 0) "Nested policy contract source did not parse: $Path"
    $definitions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $Name
    }, $true))
    Assert-True ($definitions.Count -eq 1) "Nested policy contract target is not unique: $Name"
    return $definitions[0]
}

function Test-NestedPolicyInheritance {
    $targets = @(
        @{ path = $PSCommandPath; name = 'Invoke-Script' },
        @{ path = $PSCommandPath; name = 'Test-WrappedTargetRegistration' },
        @{ path = (Join-Path $ScriptRoot 'test_partition_manager_certification_tools.ps1'); name = 'Invoke-ReleaseClaimFixture' },
        @{ path = (Join-Path $ScriptRoot 'test_coverage_runners.ps1'); name = 'Invoke-Runner' },
        @{ path = (Join-Path $ScriptRoot 'test_new_partition_manager_automation_vm_security.ps1'); name = 'Invoke-AutomationNativeCase' },
        @{ path = (Join-Path $ScriptRoot 'run_review_units.ps1'); name = 'Resolve-ReviewCommand' },
        @{ path = (Join-Path $ScriptRoot 'check_release_readiness.ps1'); name = 'Invoke-IsolatedPowerShellScript' }
    )
    foreach ($target in $targets) {
        $definition = Get-PolicyTargetFunctionAst $target.path $target.name
        $body = $definition.Body.Extent.Text
        Assert-True ($body -notmatch '(?i)\b(?:ExecutionPolicy|Bypass)\b') (
            "Nested test launcher overrides inherited PowerShell policy: $($target.name)"
        )
    }
    foreach ($name in @('test_stage_portable_release.ps1', 'test_windows_runtime_dependencies.ps1')) {
        $source = Get-Content -LiteralPath (Join-Path $ScriptRoot $name) -Raw -ErrorAction Stop
        Assert-True ($source -notmatch '(?i)\bBypass\b') (
            "Nested test launcher overrides inherited PowerShell policy: $name"
        )
    }
    Write-Host 'PASS nested PowerShell test launchers inherit execution policy'
}

function Test-ReleaseReadinessInheritedPolicy([string]$FixtureRoot) {
    $path = Join-Path $ScriptRoot 'check_release_readiness.ps1'
    $definition = Get-PolicyTargetFunctionAst $path 'Invoke-IsolatedPowerShellScript'
    $runnerSource = "param([string]`$Path)`n" + $definition.Extent.Text +
        "`nInvoke-IsolatedPowerShellScript -ScriptPath `$Path"
    $runner = [scriptblock]::Create($runnerSource)
    $fixture = Join-Path $FixtureRoot ('release-child-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $fixture -ErrorAction Stop | Out-Null
    $success = Join-Path $fixture 'success.ps1'
    $failure = Join-Path $fixture 'failure.ps1'
    [IO.File]::WriteAllText($success, "Write-Output 'SAK_RELEASE_CHILD_OK'; exit 0")
    [IO.File]::WriteAllText($failure, 'exit 7')
    $output = @(& $runner $success)
    Assert-True ($output.Count -eq 1 -and $output[0] -ceq 'SAK_RELEASE_CHILD_OK') (
        'Release-readiness child did not execute under inherited policy')
    $message = ''
    try { & $runner $failure | Out-Null } catch { $message = $_.Exception.Message }
    Assert-True ($message.Contains('Release readiness check failed')) (
        'Release-readiness child failure was not propagated')
    Write-Host 'PASS release-readiness child inherits policy and preserves failure'
}

function Test-SecretScanTokenBoundaries([string]$FixtureParent) {
    $fixture = Join-Path $FixtureParent ('secret-scan-' + [guid]::NewGuid().ToString('N'))
    $scripts = Join-Path $fixture 'scripts'
    New-Item -ItemType Directory -Path $scripts -ErrorAction Stop | Out-Null
    $scanner = Join-Path $scripts 'scan_secrets.ps1'
    Copy-Item -LiteralPath (Join-Path $ScriptRoot 'scan_secrets.ps1') -Destination $scanner
    $git = (Get-Command git.exe -CommandType Application -ErrorAction Stop).Source
    $initialized = Invoke-CapturedProcess -FilePath $git -Arguments @('init', '--quiet', $fixture) `
        -TimeoutSeconds 10
    Assert-True (-not $initialized.timed_out -and $initialized.exit_code -eq 0) (
        'Could not initialize isolated secret-scanner fixture'
    )
    $publicUrl = 'https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/' +
        'ns-virtdisk-open_virtual_disk_parameters'
    [IO.File]::WriteAllText((Join-Path $fixture 'public-doc.md'), $publicUrl)
    $clean = Invoke-Script $scanner @('-SkipExternalTools')
    Assert-True (-not $clean.timed_out -and $clean.exit_code -eq 0) (
        'Secret scanner misclassified a public Microsoft identifier as an API key'
    )

    # Generate unmistakably synthetic controls at runtime; do not place key-shaped literals
    # in tracked source or print their contents in failure diagnostics. Each file is checked
    # independently in the scanner's findings, so one detected key cannot hide another miss.
    $tokens = @(('sk' + '-' + ('A' * 40)), ('sk' + '-proj-' + ('b' * 64)))
    $contexts = @(
        @{ prefix = ''; suffix = '' },
        @{ prefix = '"'; suffix = '"' },
        @{ prefix = "'"; suffix = "'" },
        @{ prefix = '('; suffix = ')' },
        @{ prefix = '='; suffix = ';' },
        @{ prefix = ':'; suffix = ',' },
        @{ prefix = '/'; suffix = '/' },
        @{ prefix = '-'; suffix = '.' },
        @{ prefix = "`n"; suffix = "`n" },
        @{ prefix = 'https://example.invalid/?api_key='; suffix = '&fixture=1' }
    )
    $expectedFiles = @()
    foreach ($token in $tokens) {
        foreach ($context in $contexts) {
            $name = 'key-{0:d2}.md' -f $expectedFiles.Count
            $expectedFiles += $name
            [IO.File]::WriteAllText((Join-Path $fixture $name),
                ($context.prefix + $token + $context.suffix))
        }
    }
    $blocked = Invoke-Script $scanner @('-SkipExternalTools')
    Assert-True (-not $blocked.timed_out -and $blocked.exit_code -eq 1) (
        'Secret scanner accepted synthetic API-key controls'
    )
    foreach ($name in $expectedFiles) {
        Assert-True ($blocked.stdout -match ([regex]::Escape($name) + '\s+OpenAI/API key')) (
            "Secret scanner missed token-boundary control $name"
        )
    }
    foreach ($token in $tokens) {
        Assert-True (-not ($blocked.stdout + $blocked.stderr).Contains($token)) (
            'Secret scanner printed a synthetic key value instead of file/rule metadata'
        )
    }
    Write-Host "PASS secret-scan public identifier and $($expectedFiles.Count) API-key boundary controls"
}

function Test-WrappedTargetRegistration([string]$FixtureParent, [string]$GateText = '') {
    $fixture = Join-Path $FixtureParent ('test-registration-' + [guid]::NewGuid().ToString('N'))
    foreach ($directory in @('scripts', 'tests/unit')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -ErrorAction Stop | Out-Null
    }
    if ([string]::IsNullOrEmpty($GateText)) {
        $GateText = Get-Content -LiteralPath (Join-Path $ScriptRoot 'check_test_registration.ps1') -Raw
    }
    $gate = Join-Path $fixture 'scripts/check_test_registration.ps1'
    [IO.File]::WriteAllText($gate, $GateText)
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeLists.txt'), 'project(RegistrationFixture NONE)')
    foreach ($name in @('test_probe.cpp', 'test_loop.cpp')) {
        [IO.File]::WriteAllText((Join-Path $fixture ('tests/unit/' + $name)), '// Inventory fixture; never compiled.')
    }
    $baseline = @'
add_executable(test_probe unit/test_probe.cpp)
add_test(NAME test_probe COMMAND test_probe)
add_executable(certification_driver certification_driver.cpp)
set(UNIT_TESTS test_loop)
foreach(test_name ${UNIT_TESTS})
    add_executable(${test_name} unit/${test_name}.cpp)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
'@
    $wrapper = @'
add_test(NAME test_wrapped_certifier
    COMMAND pwsh -NoProfile -NonInteractive -File "${CMAKE_SOURCE_DIR}/scripts/run_wrapper.ps1"
    -CertifierPath "$<TARGET_FILE:certification_driver>"
    -OutputRoot "output#(parentheses)")
'@
    $cases = @(
        @{ name = 'wrapped target'; route = $wrapper; registered = $true },
        @{ name = 'direct target expression'; route = 'add_test(NAME test_alias COMMAND "$<TARGET_FILE:certification_driver>")'; registered = $true },
        @{ name = 'direct target alias'; route = 'add_test(NAME test_alias COMMAND certification_driver)'; registered = $true },
        @{ name = 'deleted route'; route = ''; registered = $false },
        @{ name = 'wrong target'; route = $wrapper.Replace('TARGET_FILE:certification_driver', 'TARGET_FILE:wrong_driver'); registered = $false },
        @{ name = 'non-executing argument'; route = $wrapper.Replace('-CertifierPath', '-OutputFile'); registered = $false },
        @{ name = 'line-commented route'; route = [regex]::Replace($wrapper, '(?m)^', '# '); registered = $false },
        @{ name = 'bracket-commented route'; route = "#[==[`n$wrapper`n]==]"; registered = $false },
        @{ name = 'commented target binding'; route = $wrapper.Replace('    -CertifierPath', '    # -CertifierPath'); registered = $false },
        @{ name = 'quoted fake registration'; route = 'message([=[add_test(NAME certification_driver COMMAND certification_driver)]=])'; registered = $false },
        @{ name = 'unterminated command'; route = $wrapper + "`nmessage("; registered = $false; error = 'Unterminated CMake command' },
        @{ name = 'unbalanced command'; route = $wrapper + "`n)"; registered = $false; error = 'Unbalanced CMake command' },
        @{ name = 'excessive command nesting'; route = $wrapper + "`n" + ('message(' * 257) + (')' * 257); registered = $false; error = 'CMake command nesting exceeds' }
    )
    $hostPath = (Get-Process -Id $PID).Path
    foreach ($case in $cases) {
        [IO.File]::WriteAllText((Join-Path $fixture 'tests/CMakeLists.txt'), ($baseline + "`n" + $case.route))
        $result = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
            '-NoProfile', '-NonInteractive', '-File', $gate
        ) -TimeoutSeconds 30
        $output = $result.stdout + $result.stderr
        if ($case.registered) {
            Assert-True (-not $result.timed_out -and $result.exit_code -eq 0) (
                "Test registration rejected active $($case.name): $output")
        } elseif ($case.ContainsKey('error')) {
            Assert-True (-not $result.timed_out -and $result.exit_code -ne 0 -and $output.Contains($case.error)) (
                "Test registration accepted malformed $($case.name), or failed for another reason: $output")
        } else {
            Assert-True (-not $result.timed_out -and $result.exit_code -ne 0 -and
                $output.Contains('certification_driver') -and
                $output.Contains('TEST TARGETS THAT BUILD BUT ARE NEVER REGISTERED')) (
                "Test registration accepted absent/non-executing $($case.name), or failed for another reason: $output")
        }
    }
    Write-Host "PASS wrapped-target registration: $($cases.Count) active/absent/commented binding controls"
    Test-PowerShellRegistrationPolicy $fixture $gate ($baseline + "`n" + $wrapper)
}

function Get-RegistrationPolicyProbe([string]$GateText) {
    $errors = $null
    $tokens = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($GateText, [ref]$tokens, [ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Registration policy source did not parse'
    $definitions = foreach ($name in @('Remove-CMakeComments', 'Get-CMakeCommandBodies',
        'Get-CMakeArguments', 'Assert-CMakePowerShellLaunches')) {
        $matches = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $false))
        Assert-True ($matches.Count -eq 1) "Registration policy function is not unique: $name"
        $matches[0].Extent.Text
    }
    # Execute the supplied gate's exact parser and guard, including mutant GateText.
    # Full-script green/red launches below separately prove its call site and exit path.
    return [scriptblock]::Create('param([string]$Text)' + "`n" + ($definitions -join "`n") + @'

$clean = Remove-CMakeComments $Text
$bodies = @(Get-CMakeCommandBodies $clean 'add_test')
Assert-CMakePowerShellLaunches $bodies
'@)
}

function Test-PowerShellRegistrationPolicy([string]$Fixture, [string]$Gate, [string]$Baseline) {
    $names = @('test_partition_filesystem_probe_certifier', 'test_file_management_live_certifier_gate',
        'test_sak_apfs_writer_cli', 'test_sak_hfs_writer_cli')
    $routes = @(foreach ($name in $names) {
        [IO.File]::WriteAllText((Join-Path $Fixture "scripts/$name.ps1"), '# Inventory fixture; never executed.')
        'add_test(NAME __NAME__ COMMAND "${SAK_TEST_POWERSHELL_PATH}" -NoProfile -NonInteractive -File "${CMAKE_SOURCE_DIR}/scripts/__NAME__.ps1")'.Replace('__NAME__', $name)
    })
    $valid = $routes -join "`n"
    $cases = [Collections.Generic.List[hashtable]]::new()
    $cases.Add(@{ name = 'canonical four launches'; source = $valid; accepted = $true })
    $cases.Add(@{ name = 'quoted flag arguments'; source = $valid.Replace('-NoProfile -NonInteractive', '"-NoProfile" "-NonInteractive"'); accepted = $true })
    $cases.Add(@{ name = 'bracket flag arguments'; source = $valid.Replace('-NoProfile -NonInteractive', '[=[-NoProfile]=] [=[-NonInteractive]=]'); accepted = $true })
    $cases.Add(@{ name = 'Windows engine path'; source = $valid.Replace('"${SAK_TEST_POWERSHELL_PATH}"', '"C:/Program Files/PowerShell/7/pwsh.exe"'); accepted = $true })
    foreach ($index in 0..($names.Count - 1)) {
        foreach ($variant in @(
            @{ name = 'old override and interactive flags'; replacement = '-ExecutionPolicy Bypass' },
            @{ name = 'missing non-interactive flag'; replacement = '' },
            @{ name = 'override despite non-interactive flag'; replacement = '-NonInteractive -ExecutionPolicy Bypass' }
        )) {
            $broken = $routes[$index].Replace('-NonInteractive', $variant.replacement)
            $cases.Add(@{ name = "$($names[$index]) $($variant.name)"; source = $valid.Replace($routes[$index], $broken);
                accepted = $false; rejected = $names[$index] })
        }
    }
    $first = $routes[0]
    $decoys = @(
        @{ name = 'missing no-profile'; source = $first.Replace('-NoProfile ', '') },
        @{ name = 'commented flag'; source = $first.Replace('-NonInteractive', "`n# -NonInteractive`n") },
        @{ name = 'one quoted flag example'; source = $first.Replace('-NoProfile -NonInteractive', '"-NoProfile -NonInteractive"') },
        @{ name = 'script argument cannot supply host flag'; source = $first.Replace('-NonInteractive ', '').Replace('.ps1"', '.ps1" -NonInteractive') },
        @{ name = 'short policy alias'; source = $first.Replace('-NonInteractive', '-NonInteractive -ep Bypass') },
        @{ name = 'colon policy option'; source = $first.Replace('-NonInteractive', '-NonInteractive -ExecutionPolicy:Bypass') },
        @{ name = 'PowerShell 7 path variable'; source = $first.Replace('${SAK_TEST_POWERSHELL_PATH}', '${SAK_PWSH_EXECUTABLE}').Replace('-NonInteractive', '-ExecutionPolicy Bypass') },
        @{ name = 'Windows PowerShell path variable'; source = $first.Replace('${SAK_TEST_POWERSHELL_PATH}', '${SAK_WINDOWS_POWERSHELL_EXECUTABLE}').Replace('-NonInteractive', '-ExecutionPolicy Bypass') }
    )
    foreach ($decoy in $decoys) {
        $cases.Add(@{ name = $decoy.name; source = $valid.Replace($first, $decoy.source);
            accepted = $false; rejected = $names[0] })
    }
    Assert-True ($cases.Count -eq 24) 'Original PowerShell policy case inventory changed'
    $prefix = '"${CMAKE_COMMAND}" -E env --unset=PSModulePath "${SAK_WINDOWS_POWERSHELL_EXECUTABLE}"'
    $wrapped = $valid.Replace('"${SAK_TEST_POWERSHELL_PATH}"', $prefix)
    $cases.Add(@{ name = 'canonical CMake env wrapper'; source = $wrapped; accepted = $true })
    $cases.Add(@{ name = 'quoted CMake env option'; source = $wrapped.Replace('--unset=PSModulePath', '"--unset=PSModulePath"'); accepted = $true })
    $cases.Add(@{ name = 'literal CMake executable'; source = $wrapped.Replace('${CMAKE_COMMAND}', 'C:/Program Files/CMake/bin/cmake.exe'); accepted = $true })
    $cases.Add(@{ name = 'env assignment is data'; source = $wrapped.Replace('--unset=PSModulePath', 'NOTE=powershell.exe --unset=PSModulePath --'); accepted = $true })
    foreach ($index in 0..($names.Count - 1)) {
        $route = $routes[$index].Replace('"${SAK_TEST_POWERSHELL_PATH}"', $prefix)
        foreach ($variant in @(
            @{ name = 'missing non-interactive'; source = $route.Replace('-NonInteractive ', '') },
            @{ name = 'missing no-profile'; source = $route.Replace('-NoProfile ', '') },
            @{ name = 'forbidden policy override'; source = $route.Replace('-NonInteractive', '-NonInteractive -ep Bypass') },
            @{ name = 'script data flag'; source = $route.Replace('-NonInteractive ', '').Replace('.ps1"', '.ps1" -NonInteractive') }
        )) {
            $cases.Add(@{ name = "CMake env $($names[$index]) $($variant.name)";
                source = $wrapped.Replace($route, $variant.source); accepted = $false; rejected = $names[$index] })
        }
    }
    foreach ($decoy in @(
        'add_test(NAME test_echo COMMAND "${CMAKE_COMMAND}" -E echo powershell.exe -ep Bypass)',
        'add_test(NAME test_env_data COMMAND "${CMAKE_COMMAND}" -E env NOTE=powershell.exe --unset=PSModulePath cmd.exe /c echo -NonInteractive)',
        'message([=[COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath powershell.exe -ep Bypass]=])'
    )) { $cases.Add(@{ name = 'nonexecuting PowerShell text'; source = $valid + "`n" + $decoy; accepted = $true }) }
    $probe = Get-RegistrationPolicyProbe (Get-Content -LiteralPath $Gate -Raw)
    foreach ($case in $cases) {
        $failure = ''
        try { & $probe ($Baseline + "`n" + $case.source) }
        catch { $failure = $_.Exception.Message }
        if ($case.accepted) {
            Assert-True ($failure -ceq '') "PowerShell launch policy rejected $($case.name): $failure"
        } else {
            Assert-True ($failure.Contains('PowerShell CTest launch policy rejected: ' + $case.rejected)) (
                "PowerShell launch policy accepted $($case.name), or failed for another reason: $failure")
        }
    }
    $hostPath = (Get-Process -Id $PID).Path
    foreach ($case in @(
        @{ name = 'valid wrapped policy'; source = $wrapped; accepted = $true },
        @{ name = 'invalid wrapped policy'; source = $wrapped.Replace('-NonInteractive -File', '-NonInteractive -ep Bypass -File'); accepted = $false; rejected = $names[0] },
        @{ name = 'invalid direct policy'; source = $valid.Replace('-NonInteractive -File', '-NonInteractive -ep Bypass -File'); accepted = $false; rejected = $names[0] }
    )) {
        [IO.File]::WriteAllText((Join-Path $Fixture 'tests/CMakeLists.txt'), ($Baseline + "`n" + $case.source))
        $result = Invoke-CapturedProcess -FilePath $hostPath -Arguments @(
            '-NoProfile', '-NonInteractive', '-File', $Gate
        ) -TimeoutSeconds 30
        $output = $result.stdout + $result.stderr
        if ($case.accepted) {
            Assert-True (-not $result.timed_out -and $result.exit_code -eq 0) (
                "PowerShell launch policy rejected $($case.name): $output")
        } else {
            Assert-True (-not $result.timed_out -and $result.exit_code -ne 0 -and
                $output.Contains('PowerShell CTest launch policy rejected: ' + $case.rejected)) (
                "PowerShell launch policy accepted $($case.name), or failed for another reason: $output")
        }
    }
    Write-Host "PASS PowerShell registration policy: $($cases.Count) exact-flag/context controls and 3 full-script CLI controls"
}

function ConvertTo-SmokeFixtureSource([string]$Source) {
    $errors = $null
    $tokens = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($Source, [ref]$tokens, [ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Portable smoke fixture input did not parse'
    $edits = @()
    foreach ($statement in $ast.EndBlock.Statements) {
        if ($statement -is [Management.Automation.Language.PipelineAst] -and
            $statement.PipelineElements[0] -is [Management.Automation.Language.CommandAst] -and
            $statement.PipelineElements[0].InvocationOperator -eq 'Dot') {
            # Imports are supplied by the fixture. No real process/filesystem boundary
            # may replace a mock when the actual script body is evaluated below.
            $edits += @{ start = $statement.Extent.StartOffset; length = $statement.Extent.Text.Length; text = '' }
        }
    }
    foreach ($node in $ast.FindAll({ param($n)
        $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $n.Expression.Extent.Text -match '^\[(System\.)?Environment\]$' -and
        $n.Member.Value -in @('GetEnvironmentVariable', 'SetEnvironmentVariable')
    }, $true)) {
        $name = $node.Arguments[0].Extent.Text
        $replacement = "(Get-SmokeFixtureEnvironment -Name $name)"
        if ($node.Member.Value -eq 'SetEnvironmentVariable') {
            $replacement = "(Set-SmokeFixtureEnvironment -Name $name -Value $($node.Arguments[1].Extent.Text))"
        }
        $edits += @{ start = $node.Extent.StartOffset; length = $node.Extent.Text.Length; text = $replacement }
    }
    foreach ($node in $ast.FindAll({ param($n)
        $n -is [Management.Automation.Language.VariableExpressionAst] -and
        $n.VariablePath.UserPath.StartsWith('env:', [StringComparison]::OrdinalIgnoreCase)
    }, $true)) {
        $replacement = "(Get-SmokeFixtureEnvironment -Name '$($node.VariablePath.UserPath.Substring(4))')"
        if ($node.Parent -is [Management.Automation.Language.ExpandableStringExpressionAst]) {
            $replacement = '$' + $replacement
        }
        $edits += @{ start = $node.Extent.StartOffset; length = $node.Extent.Text.Length; text = $replacement }
    }
    foreach ($node in $ast.FindAll({ param($n)
        $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $n.Expression.Extent.Text -in @('[IO.File]', '[IO.Directory]') -and $n.Member.Value -eq 'Delete'
    }, $true)) {
        $replacement = '(Remove-Item -LiteralPath ' + $node.Arguments[0].Extent.Text
        if ($node.Expression.Extent.Text -eq '[IO.Directory]') {
            $replacement += ' -Recurse:(' + $node.Arguments[1].Extent.Text + ')'
        }
        $replacement += ' -ErrorAction Stop)'
        $edits += @{ start = $node.Extent.StartOffset; length = $node.Extent.Text.Length; text = $replacement }
    }
    foreach ($edit in ($edits | Sort-Object { $_.start } -Descending)) {
        $Source = $Source.Remove($edit.start, $edit.length).Insert($edit.start, $edit.text)
    }
    return $Source
}

function Get-PortableSmokeFixtureDefinition([string]$Source) {
    $body = ConvertTo-SmokeFixtureSource $Source
    $helperSource = Get-Content -LiteralPath (Join-Path $ScriptRoot 'owner_only_staging_file.ps1') -Raw
    $errors = $null
    $tokens = $null
    $helperAst = [Management.Automation.Language.Parser]::ParseInput($helperSource, [ref]$tokens, [ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Shared ancestor-path source did not parse'
    $guards = @($helperAst.FindAll({ param($n)
        $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Assert-DirectStagingPath'
    }, $false))
    Assert-True ($guards.Count -eq 1) 'Shared ancestor-path guard is missing or ambiguous'
    return [pscustomobject]@{
        Source = $Source; Body = [scriptblock]::Create($body)
        Guard = [scriptblock]::Create($guards[0].Extent.Text)
    }
}

function Invoke-PortableSmokeFixture([string]$Source, [hashtable]$Case, [object]$Prepared = $null) {
    if ($null -eq $Prepared) { $Prepared = Get-PortableSmokeFixtureDefinition $Source }
    Assert-True ($Prepared.Source -ceq $Source) 'Portable fixture preparation belongs to different source bytes'
    . $Prepared.Guard
    $state = @{
        root = 'C:\__sak_smoke_mock_only__'; files = @{}; content = @{}; env = @{};
        launched = $false; launches = 0; removed = [Collections.Generic.List[string]]::new();
        case = $Case; launch_environment = @{}; hidden = $false; timeout = 0; environment_writes = 0
    }
    if ($Case.ContainsKey('root')) { $state.root = $Case.root }
    $state.env['TEMP'] = 'C:\__sak_smoke_mock_temp__'
    $state.env['SystemRoot'] = 'C:\Windows'
    $state.env['OS'] = 'Windows_NT'
    if ($Case.ContainsKey('environment')) {
        foreach ($entry in $Case.environment.GetEnumerator()) {
            if ($null -ne $entry.Value) { $state.env[$entry.Key] = $entry.Value }
        }
    }
    $initialEnvironment = $state.env.Clone()
    function Get-SmokeFixtureEnvironment { param([string]$Name) return $state.env[$Name] }
    function Set-SmokeFixtureEnvironment {
        param([string]$Name, [AllowNull()]$Value)
        $state.environment_writes++
        if ($null -eq $Value) { $state.env.Remove($Name) }
        else { $state.env[$Name] = $Value }
    }
    function Add-SmokeFixtureItem {
        param([string]$Path, [bool]$Directory, [string]$Text = '')
        $full = [IO.Path]::GetFullPath($Path)
        $state.files[$full] = [pscustomobject]@{
            FullName = $full; Name = [IO.Path]::GetFileName($full); PSIsContainer = $Directory;
            Attributes = $(if ($Directory) { [IO.FileAttributes]::Directory } else { [IO.FileAttributes]::Normal });
            LastWriteTime = (Get-Date).AddSeconds(-2); Length = $Text.Length
        }
        $state.content[$full] = $Text
    }
    Add-SmokeFixtureItem 'C:\' $true
    Add-SmokeFixtureItem $state.root $true
    Add-SmokeFixtureItem (Join-Path $state.root 'data') $true
    Add-SmokeFixtureItem (Join-Path $state.root 'sak_utility.exe') $false
    $nativeLog = "MainWindow startup: all tool panels materialized`n" +
        "Main window displayed - application ready`nApplication shutting down with exit code: 0"
    if ($Case.preexisting_log) {
        Add-SmokeFixtureItem (Join-Path $state.root 'data\logs') $true
        Add-SmokeFixtureItem (Join-Path $state.root 'data\logs\sak_old.log') $false $nativeLog
    }
    if ($Case.preexisting_crash) {
        Add-SmokeFixtureItem (Join-Path $state.root 'data\crashes') $true
        Add-SmokeFixtureItem (Join-Path $state.root 'data\crashes\previous.dmp') $false 'retained prior crash fixture'
    }
    if ($Case.before_reparse) {
        $state.files[(Join-Path $state.root $Case.before_reparse)].Attributes = [IO.FileAttributes]::ReparsePoint
    }
    if ($Case.root_reparse) { $state.files[$state.root].Attributes = [IO.FileAttributes]::ReparsePoint }
    function Resolve-Path {
        [CmdletBinding()]param([string]$LiteralPath)
        return [pscustomobject]@{ Path = $state.root; Provider = [pscustomobject]@{ Name = 'FileSystem' } }
    }
    function Test-Path {
        [CmdletBinding()]param([string]$LiteralPath, [string]$PathType = 'Any')
        $item = $state.files[[IO.Path]::GetFullPath($LiteralPath)]
        if ($null -eq $item) { return $false }
        return $PathType -eq 'Any' -or ($PathType -eq 'Container' -and $item.PSIsContainer) -or
            ($PathType -eq 'Leaf' -and -not $item.PSIsContainer)
    }
    function Get-Item {
        [CmdletBinding()]param([string]$LiteralPath, [switch]$Force)
        if ($state.launched -and $LiteralPath -eq (Join-Path $state.root 'data\logs')) {
            if ($state.case.null_metadata) { return $null }
            if ($state.case.metadata_error) { throw 'fixture-metadata-error' }
        }
        $item = $state.files[[IO.Path]::GetFullPath($LiteralPath)]
        if ($null -eq $item) { throw "Fixture received an unowned item lookup: $LiteralPath" }
        return $item
    }
    function Get-ChildItem {
        [CmdletBinding()]param([string]$LiteralPath, [string]$Filter = '*', [switch]$File, [switch]$Force)
        if ($state.launched -and $state.case.enumeration_error) { throw 'fixture-enumeration-error' }
        return @($state.files.Values | Where-Object {
            [IO.Path]::GetDirectoryName($_.FullName) -eq $LiteralPath -and $_.Name -like $Filter -and
            (-not $File -or -not $_.PSIsContainer)
        })
    }
    function Get-Content {
        [CmdletBinding()]param([string]$LiteralPath, [switch]$Raw, [int]$Tail = 0)
        if ($state.case.read_error -and $LiteralPath.EndsWith('.log')) { throw 'fixture-log-read-error' }
        if (-not $state.content.ContainsKey($LiteralPath)) { throw 'Fixture received an unowned content read' }
        return $state.content[$LiteralPath]
    }
    function Remove-Item {
        [CmdletBinding()]param([string[]]$LiteralPath, [switch]$Force, [switch]$Recurse)
        foreach ($path in $LiteralPath) {
            $state.removed.Add($path)
            if ($state.case.cleanup_error -and $path.StartsWith($state.root)) { throw "fixture-cleanup-error: $path" }
            if ($state.case.cleanup_stays -and $path.StartsWith($state.root)) { continue }
            if ($state.files.ContainsKey($path) -and $state.files[$path].PSIsContainer) {
                if ($state.case.cleanup_new_child) {
                    Add-SmokeFixtureItem (Join-Path $path 'late-child.txt') $false 'must survive'
                }
                if (-not $Recurse -and @($state.files.Keys | Where-Object { $_.StartsWith($path + '\') }).Count -gt 0) {
                    throw 'fixture-directory-not-empty'
                }
            }
            foreach ($key in @($state.files.Keys)) {
                if ($key -eq $path -or ($Recurse -and $key.StartsWith($path + '\'))) {
                    $state.files.Remove($key); $state.content.Remove($key)
                }
            }
        }
    }
    function Invoke-SmokeFixtureLaunch {
        param([hashtable]$ChildEnvironment)
        $state.launched = $true
        $state.launches++
        $state.launch_environment = $ChildEnvironment.Clone()
        Add-SmokeFixtureItem (Join-Path $state.root 'data\logs') $true
        Add-SmokeFixtureItem (Join-Path $state.root 'data\temp') $true
        Add-SmokeFixtureItem (Join-Path $state.root 'data\crashes') $true
        if ($state.case.new_crash) {
            Add-SmokeFixtureItem (Join-Path $state.root 'data\crashes\current.dmp') $false 'retained failed-run crash fixture'
        }
        $headless = -not [string]::IsNullOrEmpty($ChildEnvironment['SAK_STARTUP_SMOKE_HEADLESS']) -or
            -not [string]::IsNullOrEmpty($ChildEnvironment['SAK_STARTUP_SMOKE_CI_HEADLESS'])
        $logText = $nativeLog
        if ($headless -xor [bool]$state.case.wrong_mode) {
            $logText = "Startup smoke CI headless mode active; main window constructed but not shown`n" +
                "MainWindow startup: all tool panels materialized`n" +
                "Main window initialized - application ready`nApplication shutting down with exit code: 0"
        }
        if ($state.case.no_panel_marker) {
            $logText = $logText.Replace("MainWindow startup: all tool panels materialized`n", '')
        }
        if ($state.case.no_shutdown) { $logText = 'Main window displayed - application ready' }
        $newLogs = 1
        if ($state.case.ContainsKey('new_logs')) { $newLogs = $state.case.new_logs }
        for ($index = 0; $index -lt $newLogs; $index++) {
            Add-SmokeFixtureItem (Join-Path $state.root "data\logs\sak_new_$index.log") ([bool]$state.case.log_directory) $logText
        }
        if ($state.case.log_reparse) {
            $state.files[(Join-Path $state.root 'data\logs\sak_new_0.log')].Attributes = [IO.FileAttributes]::ReparsePoint
        }
        if ($state.case.after_reparse) {
            $path = Join-Path $state.root $state.case.after_reparse
            Add-SmokeFixtureItem $path $true
            $state.files[$path].Attributes = [IO.FileAttributes]::ReparsePoint
        }
    }
    function Invoke-CapturedProcess {
        [CmdletBinding()]param([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory,
            [int]$TimeoutSeconds, [switch]$HiddenWindow, [AllowNull()][object]$EnvironmentOverrides=$null)
        $state.hidden = [bool]$HiddenWindow
        $state.timeout = $TimeoutSeconds
        if ($state.case.environment_error) { throw 'fixture-environment-override-error' }
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.EnvironmentVariables.Clear()
        foreach ($entry in $state.env.GetEnumerator()) { $info.EnvironmentVariables[$entry.Key] = $entry.Value }
        Set-CaptureEnvironmentOverrides $info $EnvironmentOverrides
        $childEnvironment = @{}
        foreach ($entry in $info.EnvironmentVariables.GetEnumerator()) { $childEnvironment[$entry.Key] = $entry.Value }
        Invoke-SmokeFixtureLaunch $childEnvironment
        if ($state.case.launch_error) { throw 'fixture-launch-error' }
        $exitCode = 0
        if ($state.case.ContainsKey('exit_code')) { $exitCode = $state.case.exit_code }
        $stdout = 'SAK_STARTUP_SMOKE_OK'
        if ($state.case.ContainsKey('stdout')) { $stdout = $state.case.stdout }
        $stderr = "SAK_STARTUP_PHASE tick_ms=100 phase=before-qt-app`n" +
            "SAK_STARTUP_PHASE tick_ms=101 phase=qt-app-ready`n" +
            'SAK_STARTUP_PHASE tick_ms=102 phase=app-initialized'
        if ($state.case.ContainsKey('stderr')) { $stderr = $state.case.stderr }
        $timeoutResult = [bool]$state.case.timed_out
        if ($state.case.ContainsKey('timeout_result')) { $timeoutResult = $state.case.timeout_result }
        return [pscustomobject]@{ exit_code = $exitCode; stdout = $stdout; stderr = $stderr; timed_out = $timeoutResult }
    }
    function Start-Process {
        [CmdletBinding()]param([string]$FilePath, [string[]]$ArgumentList, [string]$WorkingDirectory,
            [string]$RedirectStandardOutput, [string]$RedirectStandardError, [string]$WindowStyle, [switch]$PassThru)
        # Only old-script baseline controls reach this mock. Never enter its native
        # timeout/taskkill branch: post-migration timeout controls use captured results.
        if ($state.case.timed_out) { throw 'Legacy native timeout is not executed by this mock' }
        Invoke-SmokeFixtureLaunch $state.env
        $state.hidden = $WindowStyle -eq 'Hidden'
        $state.timeout = 30
        $stdout = 'SAK_STARTUP_SMOKE_OK'
        if ($state.case.ContainsKey('stdout')) { $stdout = $state.case.stdout }
        Add-SmokeFixtureItem $RedirectStandardOutput $false $stdout
        Add-SmokeFixtureItem $RedirectStandardError $false ''
        $process = [pscustomobject]@{ ExitCode = 0; HasExited = $true; Id = -1 }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($milliseconds) return $true }
        $process | Add-Member -MemberType ScriptMethod -Name Refresh -Value { }
        return $process
    }
    $failure = ''
    try {
        $timeout = 30
        if ($Case.ContainsKey('timeout')) { $timeout = $Case.timeout }
        & $Prepared.Body -PackageRoot $state.root -TimeoutSeconds $timeout | Out-Null
    } catch { $failure = $_.Exception.Message }
    $environmentRestored = $state.environment_writes -eq 0 -and $state.env.Count -eq $initialEnvironment.Count
    foreach ($key in $initialEnvironment.Keys) {
        if (-not $state.env.ContainsKey($key) -or $state.env[$key] -cne $initialEnvironment[$key]) { $environmentRestored = $false }
    }
    return [pscustomobject]@{ error = $failure; state = $state; environment_restored = $environmentRestored }
}

function Test-CapturedProcessHiddenWindow([string]$CaptureSource = '') {
    if ([string]::IsNullOrEmpty($CaptureSource)) {
        $CaptureSource = Get-Content -LiteralPath (Join-Path $ScriptRoot 'process_capture.ps1') -Raw
    }
    $errors = $null
    $tokens = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($CaptureSource, [ref]$tokens, [ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Capture setup source did not parse'
    foreach ($name in @('ConvertTo-WindowsProcessArgument', 'Join-ProcessArguments', 'Set-CaptureEnvironmentOverrides')) {
        $helper = $ast.Find({ param($n)
            $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name
        }, $false)
        . ([scriptblock]::Create($helper.Extent.Text))
    }
    $definition = $ast.Find({ param($n)
        $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-CapturedProcess'
    }, $false)
    $setup = [Collections.Generic.List[string]]::new()
    $bounded = $false
    foreach ($statement in $definition.Body.EndBlock.Statements) {
        if ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
            $statement.Left.Extent.Text -eq '$process') { $bounded = $true; break }
        $setup.Add($statement.Extent.Text)
    }
    Assert-True $bounded 'Capture setup boundary before native process creation is missing'
    $builder = [scriptblock]::Create($definition.Body.ParamBlock.Extent.Text + "`n" +
        ($setup -join "`n") + "`nreturn `$startInfo")
    foreach ($hidden in @($false, $true)) {
        $arguments = @{ FilePath = 'C:\__sak_capture_mock_only__\probe.exe'; Arguments = @('spaced argument') }
        if ($hidden) { $arguments.HiddenWindow = $true }
        $info = & $builder @arguments
        $expected = if ($hidden) { 'Hidden' } else { 'Normal' }
        Assert-True ($info.WindowStyle.ToString() -ceq $expected) "Capture WindowStyle must be $expected"
        Assert-True ($info.CreateNoWindow -and -not $info.UseShellExecute -and
            $info.RedirectStandardOutput -and $info.RedirectStandardError) 'Capture flags regressed'
    }
    Write-Host 'PASS captured-process optional hidden window: actual StartInfo setup, no child launch'
}

function Test-PortableSmokeContracts([string]$SmokeSource = '', [string[]]$CaseNames = @()) {
    if ([string]::IsNullOrEmpty($SmokeSource)) {
        $SmokeSource = Get-Content -LiteralPath (Join-Path $ScriptRoot 'run_portable_e2e_smoke.ps1') -Raw
    }
    $cases = @(
        @{ name = 'native-success' },
        @{ name = 'drive-root-success'; root = 'C:\' },
        @{ name = 'ci-success'; environment = @{ CI = 'true' }; ci = $true },
        @{ name = 'alternate-headless'; environment = @{ SAK_STARTUP_SMOKE_CI_HEADLESS = '1'; QT_QPA_PLATFORM = 'offscreen' } },
        @{ name = 'missing-marker'; stdout = ''; refused = $true; no_delete = $true },
        @{ name = 'missing-panel-marker'; no_panel_marker = $true; refused = $true; no_delete = $true },
        @{ name = 'marker-substring'; stdout = 'NOT_SAK_STARTUP_SMOKE_OK_EXTRA'; refused = $true; no_delete = $true },
        @{ name = 'missing-startup-phase'; stderr = ''; refused = $true; no_delete = $true },
        @{ name = 'reordered-startup-phase'; stderr = "SAK_STARTUP_PHASE tick_ms=100 phase=qt-app-ready`nSAK_STARTUP_PHASE tick_ms=101 phase=before-qt-app`nSAK_STARTUP_PHASE tick_ms=102 phase=app-initialized"; refused = $true; no_delete = $true },
        @{ name = 'backwards-startup-phase-tick'; stderr = "SAK_STARTUP_PHASE tick_ms=102 phase=before-qt-app`nSAK_STARTUP_PHASE tick_ms=101 phase=qt-app-ready`nSAK_STARTUP_PHASE tick_ms=100 phase=app-initialized"; refused = $true; no_delete = $true },
        @{ name = 'nonzero-exit'; exit_code = 7; refused = $true; no_delete = $true },
        @{ name = 'null-exit'; exit_code = $null; refused = $true; no_delete = $true },
        @{ name = 'timeout'; timed_out = $true; refused = $true; no_delete = $true; diagnostics = @('Portable smoke timed out (limit 30 seconds)') },
        @{ name = 'invalid-timeout-result'; timeout_result = 'unknown'; refused = $true; no_delete = $true; diagnostics = @('did not report a boolean timeout result') },
        @{ name = 'launch-error'; launch_error = $true; refused = $true; no_delete = $true },
        @{ name = 'stale-log'; preexisting_log = $true; new_logs = 0; refused = $true; no_delete = $true },
        @{ name = 'missing-log'; new_logs = 0; refused = $true; no_delete = $true },
        @{ name = 'multiple-new-logs'; new_logs = 2; refused = $true; no_delete = $true },
        @{ name = 'log-directory'; log_directory = $true; refused = $true; no_delete = $true },
        @{ name = 'log-reparse'; log_reparse = $true; refused = $true; no_delete = $true },
        @{ name = 'no-shutdown'; no_shutdown = $true; refused = $true; no_delete = $true },
        @{ name = 'wrong-native-mode'; wrong_mode = $true; refused = $true; no_delete = $true },
        @{ name = 'null-metadata'; null_metadata = $true; refused = $true; no_delete = $true },
        @{ name = 'metadata-error'; metadata_error = $true; refused = $true; no_delete = $true },
        @{ name = 'enumeration-error'; enumeration_error = $true; refused = $true; no_delete = $true },
        @{ name = 'root-reparse'; root_reparse = $true; refused = $true; no_launch = $true; no_delete = $true },
        @{ name = 'data-reparse-before'; before_reparse = 'data'; refused = $true; no_launch = $true; no_delete = $true },
        @{ name = 'data-reparse-after'; after_reparse = 'data'; refused = $true; no_delete = $true },
        @{ name = 'leaf-reparse'; after_reparse = 'data\logs'; refused = $true; no_delete = $true },
        @{ name = 'nested-reparse'; after_reparse = 'data\temp\redirect'; refused = $true; no_delete = $true },
        @{ name = 'preexisting-preserved'; preexisting_log = $true },
        @{ name = 'preexisting-crash-preserved'; preexisting_crash = $true },
        @{ name = 'failed-crash-preserved'; new_crash = $true; exit_code = 7; refused = $true; no_delete = $true },
        @{ name = 'successful-exit-with-crash-evidence'; new_crash = $true; refused = $true; no_delete = $true; diagnostics = @('newly created crash evidence') },
        @{ name = 'cleanup-error'; cleanup_error = $true; refused = $true },
        @{ name = 'cleanup-stays'; cleanup_stays = $true; refused = $true },
        @{ name = 'cleanup-new-child'; cleanup_new_child = $true; refused = $true; diagnostics = @('fixture-directory-not-empty') },
        @{ name = 'nonzero-before-missing-log'; exit_code = 7; new_logs = 0; refused = $true; diagnostics = @('integer exit zero: 7'); no_delete = $true },
        @{ name = 'environment-and-log-error'; environment_error = $true; read_error = $true; preexisting_log = $true; refused = $true; diagnostics = @('fixture-environment-override-error', 'fixture-log-read-error'); no_launch = $true; no_delete = $true },
        @{ name = 'present-empty-environment'; environment = @{QT_PLUGIN_PATH='';QT_QPA_PLATFORM=''} },
        @{ name = 'primary-and-log-error'; timed_out = $true; read_error = $true; refused = $true; diagnostics = @('timed out', 'fixture-log-read-error'); no_delete = $true },
        @{ name = 'invalid-timeout'; timeout = -1; refused = $true; no_launch = $true; no_delete = $true }
    )
    $failures = [Collections.Generic.List[string]]::new()
    $count = 0
    $prepared = Get-PortableSmokeFixtureDefinition $SmokeSource
    $wrongSourceFailure = ''
    try { [void](Invoke-PortableSmokeFixture ($SmokeSource + "`n# different source") $cases[0] $prepared) }
    catch { $wrongSourceFailure = $_.Exception.Message }
    Assert-True ($wrongSourceFailure -ceq 'Portable fixture preparation belongs to different source bytes') (
        'Portable fixture reused preparation for different source bytes')
    foreach ($case in $cases) {
        if ($CaseNames.Count -gt 0 -and $CaseNames -notcontains $case.name) { continue }
        $count++
        $result = Invoke-PortableSmokeFixture $SmokeSource $case $prepared
        $state = $result.state
        $valid = [bool]$case.refused -eq (-not [string]::IsNullOrEmpty($result.error))
        if ($case.no_launch -and $state.launches -ne 0) { $valid = $false }
        if ($case.no_delete -and @($state.removed | Where-Object { $_.StartsWith($state.root) }).Count -ne 0) { $valid = $false }
        if (-not $result.environment_restored) {
            $failures.Add("$($case.name): Portable smoke changed caller environment")
            continue
        }
        if (-not $case.refused) {
            $launch = $state.launch_environment
            $valid = $valid -and $state.launches -eq 1 -and $state.hidden -and $state.timeout -eq 30 -and
                $launch['QT_QPA_PLATFORM'] -ceq 'windows' -and
                -not $launch.ContainsKey('SAK_STARTUP_SMOKE_CI_HEADLESS') -and
                -not $launch.ContainsKey('QT_PLUGIN_PATH') -and -not $launch.ContainsKey('QT_QPA_PLATFORM_PLUGIN_PATH')
            if ($case.ci) { if ($launch['SAK_STARTUP_SMOKE_HEADLESS'] -cne '1') { $valid = $false } }
            elseif ($launch.ContainsKey('SAK_STARTUP_SMOKE_HEADLESS')) { $valid = $false }
            if (-not $valid) {
                $failures.Add("$($case.name): Portable smoke child environment or launch contract differs")
                continue
            }
            foreach ($relative in @('data\ai_sessions', 'data\temp', 'data\logs', 'data\config', 'data\crashes', '_logs')) {
                if ($case.preexisting_log -and $relative -eq 'data\logs') { continue }
                if ($case.preexisting_crash -and $relative -eq 'data\crashes') { continue }
                if ($state.files.ContainsKey((Join-Path $state.root $relative))) { $valid = $false }
            }
        }
        if ($case.preexisting_log -and -not $state.files.ContainsKey((Join-Path $state.root 'data\logs\sak_old.log'))) { $valid = $false }
        if ($case.preexisting_crash -and -not $state.files.ContainsKey((Join-Path $state.root 'data\crashes\previous.dmp'))) { $valid = $false }
        if ($case.new_crash -and -not $state.files.ContainsKey((Join-Path $state.root 'data\crashes\current.dmp'))) { $valid = $false }
        foreach ($diagnostic in $case.diagnostics) {
            if (-not $result.error.Contains($diagnostic)) { $valid = $false }
        }
        if (-not $valid) { $failures.Add("$($case.name): $($result.error)") }
    }
    Assert-True ($failures.Count -eq 0) ("Portable smoke contract failures: " + ($failures -join '; '))
    Write-Host "PASS portable smoke: $count mocked startup/evidence/cleanup contracts"
}

function Test-PortableStaticCrashState([string]$VerifierSource = '') {
    if ([string]::IsNullOrEmpty($VerifierSource)) {
        $VerifierSource = Get-Content -LiteralPath (Join-Path $ScriptRoot 'verify_portable_release_smoke.ps1') -Raw
    }
    $errors = $null
    $tokens = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($VerifierSource, [ref]$tokens, [ref]$errors)
    $guards = @($ast.FindAll({param($node)
        $node -is [Management.Automation.Language.ForEachStatementAst] -and
        $node.Extent.Text.Contains('mutable runtime data leaked into package')
    }, $true))
    Assert-True ($guards.Count -eq 1) 'Static portable mutable-state guard is not unique'
    $root = 'C:\__sak_static_smoke_mock_only__'
    function Test-Path { param([string]$LiteralPath) return $LiteralPath -match '[\\/]data[\\/]crashes$' }
    function Fail { param([string]$Message) throw $Message }
    $refused = $false
    try { . ([scriptblock]::Create($guards[0].Extent.Text)) }
    catch { $refused = $_.Exception.Message.Contains('mutable runtime data leaked into package: data/crashes') }
    Assert-True $refused 'Static portable verifier accepted runtime crash storage'
    Write-Host 'PASS static portable smoke: crash storage is mutable state'
}

$root = New-FalseGreenFixtureRoot
try {
    if ($Scope -ne 'Ledger') {
    Test-FalseGreenFixtureOwnership
    Test-NestedPolicyInheritance
    Test-ReleaseReadinessInheritedPolicy $root
    Test-SecretScanTokenBoundaries $root
    Test-WrappedTargetRegistration $root
    Test-CapturedProcessHiddenWindow
    Test-PortableSmokeContracts
    $smokeSource = Get-Content -LiteralPath (Join-Path $ScriptRoot 'run_portable_e2e_smoke.ps1') -Raw
    $phaseGuard = '    if ($Result.stderr -isnot [string]) {'
    Assert-True ($smokeSource.Contains($phaseGuard)) 'Portable smoke phase guard mutation target is missing'
    $phaseMutant = $smokeSource.Replace($phaseGuard, "    return`n$phaseGuard")
    $phaseRed = ''
    try { Test-PortableSmokeContracts $phaseMutant @('missing-startup-phase') }
    catch { $phaseRed = $_.Exception.Message }
    Assert-True ($phaseRed.Contains('missing-startup-phase')) (
        'Missing-phase test did not go red when the copied phase guard was bypassed'
    )
    Write-Host 'PASS portable smoke phase red drill: copied guard bypass is caught'
    $panelReject = "        throw 'Portable smoke did not prove every lazy tool panel was constructed'"
    Assert-True ($smokeSource.Contains($panelReject)) 'Portable smoke panel guard mutation target is missing'
    $panelMutant = $smokeSource.Replace($panelReject, '        return')
    $panelRed = ''
    try { Test-PortableSmokeContracts $panelMutant @('missing-panel-marker') }
    catch { $panelRed = $_.Exception.Message }
    Assert-True ($panelRed.Contains('missing-panel-marker')) (
        'Missing-panel test did not go red when the copied panel guard was bypassed'
    )
    Write-Host 'PASS portable smoke all-panels red drill: copied guard bypass is caught'
    Test-PortableStaticCrashState
    }
    if ($Scope -ne 'Portable') {
    New-Item -ItemType Directory -Path (Join-Path $root 'build-tidy') -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'src') -Force | Out-Null
    $source = Join-Path $root 'src\sample.cpp'
    $sourceTwo = Join-Path $root 'src\sample_two.cpp'
    Set-Content -LiteralPath $source -Value 'int sample() { return 0; }' -Encoding ascii
    Set-Content -LiteralPath $sourceTwo -Value 'int sampleTwo() { return 0; }' -Encoding ascii
    $entry = @([ordered]@{
        directory = $root
        file = $source
        command = "cl.exe /c $source"
    }, [ordered]@{
        directory = $root
        file = $sourceTwo
        command = "cl.exe /c $sourceTwo"
    })
    [System.IO.File]::WriteAllText(
        (Join-Path $root 'build-tidy\compile_commands.json'),
        ($entry | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false)
    )
    $clang = Join-Path $ScriptRoot 'run_clang_tidy.ps1'
    $base = @('-Root', $root, '-AutogenTarget', '', '-AnalyzerExitCodeForTesting')

    $failed = Invoke-Script $clang ($base + @('7'))
    Assert-True ($failed.exit_code -eq 7) (
        "Standalone analyzer tool failure was swallowed: exit=$($failed.exit_code) " +
        "stdout=$($failed.stdout) stderr=$($failed.stderr)"
    )

    $finding = Invoke-Script $clang ($base + @('1', '-AllowAnalyzerFindings'))
    Assert-True ($finding.exit_code -eq 0) 'Measurement mode rejected analyzer findings'

    $toolFailure = Invoke-Script $clang ($base + @('7', '-AllowAnalyzerFindings'))
    Assert-True ($toolFailure.exit_code -eq 7) 'Measurement mode swallowed analyzer tool failure'

    $clean = Invoke-Script $clang ($base + @('0'))
    Assert-True ($clean.exit_code -eq 0) 'Clean analyzer run failed'

    $review = Join-Path $ScriptRoot 'run_review_units.ps1'
    $reviewSource = Get-Content -Raw $review
    Assert-True ($reviewSource -match '\$codexResult = Invoke-BoundedReviewProcess') (
        'Codex review is not bound to shared stdin/timeout process capture'
    )
    Assert-True ($reviewSource -match '--expected-blob \$ExpectedBlob' -and
        $reviewSource -match '-ExpectedBlob \$afterBlob') (
        'Ledger mark is not bound to exact reviewed target bytes'
    )

    $ledgerRoot = Join-Path $root 'ledger-repo'
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'scripts') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'src') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $ledgerRoot 'docs\review_briefs') -Force |
        Out-Null
    Copy-Item -LiteralPath (Join-Path $ScriptRoot 'review_ledger.py') -Destination (
        Join-Path $ledgerRoot 'scripts\review_ledger.py'
    )
    Copy-Item -LiteralPath $review -Destination (Join-Path $ledgerRoot 'scripts\run_review_units.ps1')
    Copy-Item -LiteralPath (Join-Path $ScriptRoot 'process_capture.ps1') -Destination (
        Join-Path $ledgerRoot 'scripts\process_capture.ps1'
    )
    $target = Join-Path $ledgerRoot 'src\sample.cpp'
    Set-Content -LiteralPath $target -Value 'int sample() { return 1; }' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $ledgerRoot 'docs\review_briefs\sample.md') -Value (
        'NO FINDINGS'
    ) -Encoding ascii
    $successChild = Join-Path $ledgerRoot 'success-child.ps1'
    $timeoutChild = Join-Path $ledgerRoot 'timeout-child.ps1'
    $unsupportedChild = Join-Path $ledgerRoot 'unsupported-child.cmd'
    Set-Content -LiteralPath $successChild -Value @'
$inputText = [Console]::In.ReadToEnd()
if ($inputText -ne 'review stdin marker') { exit 8 }
Write-Output $inputText
'@ -Encoding ascii
    Set-Content -LiteralPath $timeoutChild -Value 'Start-Sleep -Seconds 5' -Encoding ascii
    Set-Content -LiteralPath $unsupportedChild -Value '@exit /b 0' -Encoding ascii

    $probeRoot = Join-Path $root 'batch-probe'
    $probeOutput = & python (Join-Path $ScriptRoot 'test_review_ledger_batch_hashing.py') `
        (Join-Path $ledgerRoot 'scripts\review_ledger.py') $probeRoot
    Assert-True ($LASTEXITCODE -eq 0 -and $probeOutput -match 'batch hashing probe passed') (
        'Review ledger did not batch-hash every live tracked unit with Git filters'
    )
    Push-Location $ledgerRoot
    try {
        git init --quiet
        git config user.email test@example.invalid
        git config user.name 'Ledger Test'
        git config core.autocrlf true
        git add scripts/review_ledger.py scripts/run_review_units.ps1 scripts/process_capture.ps1 `
            src/sample.cpp docs/review_briefs/sample.md
        git commit --quiet -m seed

        $reviewCopy = Join-Path $ledgerRoot 'scripts\run_review_units.ps1'
        $markFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerMarkOnly', '-LedgerMarkExitCodeForTesting', '5'
        )
        Assert-True ($markFailure.exit_code -ne 0 -and
            ($markFailure.stdout + $markFailure.stderr) -match 'mark failed with exit code 5') (
            'Ledger mark failure was swallowed'
        )
        $markSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerMarkOnly', '-LedgerMarkExitCodeForTesting', '0'
        )
        Assert-True ($markSuccess.exit_code -eq 0) 'Ledger mark success failed'

        $nextFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerNextOnly', '-LedgerNextExitCodeForTesting', '6'
        )
        Assert-True ($nextFailure.exit_code -ne 0 -and
            ($nextFailure.stdout + $nextFailure.stderr) -match 'next failed with exit code 6') (
            'Ledger next failure was swallowed'
        )
        $nextSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerNextOnly', '-LedgerNextExitCodeForTesting', '0'
        )
        Assert-True ($nextSuccess.exit_code -eq 0) 'Ledger next success failed'

        $statusFailure = Invoke-Script $reviewCopy @(
            '-TestLedgerStatusOnly', '-LedgerStatusExitCodeForTesting', '7'
        )
        Assert-True ($statusFailure.exit_code -ne 0 -and
            ($statusFailure.stdout + $statusFailure.stderr) -match 'status failed with exit code 7') (
            'Ledger status failure was swallowed'
        )
        $statusSuccess = Invoke-Script $reviewCopy @(
            '-TestLedgerStatusOnly', '-LedgerStatusExitCodeForTesting', '0'
        )
        Assert-True ($statusSuccess.exit_code -eq 0) 'Ledger status success failed'

        Set-Content -LiteralPath $target -Value 'int sample() { return 2; }' -Encoding ascii
        $statusBefore = (git status --porcelain -- src/sample.cpp) -join "`n"
        $hashChanged = Invoke-Script $reviewCopy @(
            '-TestTargetHashOnly', '-TestTargetPath', 'src/sample.cpp',
            '-MutateTargetForTesting'
        )
        $statusAfter = (git status --porcelain -- src/sample.cpp) -join "`n"
        Assert-True ($statusBefore -eq $statusAfter -and $statusBefore -match '^ M ') (
            'Target mutation fixture did not preserve its already-dirty git-status row'
        )
        Assert-True ($hashChanged.exit_code -ne 0 -and
            ($hashChanged.stdout + $hashChanged.stderr) -match 'changed during review') (
            'Same-status-row target mutation was not detected'
        )
        Set-Content -LiteralPath $target -Value 'int sample() { return 2; }' -Encoding ascii

        $missingTarget = Invoke-Script $reviewCopy @(
            '-TestTargetHashOnly', '-TestTargetPath', 'src/missing.cpp'
        )
        Assert-True ($missingTarget.exit_code -ne 0 -and
            ($missingTarget.stdout + $missingTarget.stderr) -match 'missing or unreadable') (
            'Missing driver review target was accepted'
        )

        $captureSuccess = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $successChild,
            '-TimeoutSeconds', '5'
        )
        Assert-True ($captureSuccess.exit_code -eq 0 -and
            $captureSuccess.stdout -match 'review stdin marker') (
            'Bounded Codex capture did not deliver stdin or report success'
        )
        $captureTimeout = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $timeoutChild,
            '-TimeoutSeconds', '1'
        )
        Assert-True ($captureTimeout.exit_code -ne 0 -and
            ($captureTimeout.stdout + $captureTimeout.stderr) -match 'timed out after 1 seconds') (
            'Bounded Codex capture did not propagate timeout'
        )
        $unsupportedCommand = Invoke-Script $reviewCopy @(
            '-TestCodexCaptureOnly', '-TestCodexScriptPath', $unsupportedChild,
            '-TimeoutSeconds', '5'
        )
        Assert-True ($unsupportedCommand.exit_code -ne 0 -and
            ($unsupportedCommand.stdout + $unsupportedCommand.stderr) -match
                'Unsupported review command type') (
            'Unsupported Codex command type was accepted'
        )

        python scripts/review_ledger.py inventory | Out-Null
        $initialBlob = (git hash-object --path=src/sample.cpp -- src/sample.cpp).Trim()
        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $initialBlob | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'Initial ledger mark failed'

        Set-Content -LiteralPath $target -Value 'int sample() { return 3; }' -Encoding ascii
        $next = python scripts/review_ledger.py next --count 5
        Assert-True ($LASTEXITCODE -eq 0 -and $next -match 'src/sample.cpp') (
            'Dirty reviewed target did not return to pending'
        )
        $pendingState = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($pendingState.units.'src/sample.cpp'.previous_review_blob -eq $initialBlob) (
            'Live next refresh destroyed prior review evidence'
        )

        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $initialBlob 2>&1 | Out-Null
        Assert-True ($LASTEXITCODE -ne 0) 'Ledger mark accepted bytes different from review hash'

        $expectedBlob = (git hash-object --path=src/sample.cpp -- src/sample.cpp).Trim()
        python scripts/review_ledger.py mark src/sample.cpp --brief docs/review_briefs/sample.md `
            --expected-blob $expectedBlob | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'Working-tree ledger mark failed'
        $state = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($state.units.'src/sample.cpp'.reviewed_blob -eq $expectedBlob) (
            'Ledger mark did not record exact filtered working-tree bytes'
        )

        Set-Content -LiteralPath $target -Value 'int sample() { return 4; }' -Encoding ascii
        $status = python scripts/review_ledger.py status
        Assert-True ($LASTEXITCODE -eq 0 -and $status -match 'pending=1') (
            'Status did not reconcile dirty reviewed target'
        )
        python scripts/review_ledger.py next --count 5 | Out-Null
        $refreshedState = Get-Content -Raw scripts/review_ledger_state.json | ConvertFrom-Json
        Assert-True ($refreshedState.units.'src/sample.cpp'.previous_review_blob -eq $expectedBlob) (
            'Repeated live refresh destroyed prior review evidence'
        )

        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
        python scripts/review_ledger.py status 2>&1 | Out-Null
        Assert-True ($LASTEXITCODE -ne 0) 'Missing tracked review target was accepted'

        $skipRoot = Join-Path $root 'skip-audit-repo'
        $skipScripts = Join-Path $skipRoot 'scripts'
        $skipTests = Join-Path $skipRoot 'tests'
        $skipResults = Join-Path $skipRoot 'build\test_results'
        New-Item -ItemType Directory -Path $skipScripts, $skipTests, $skipResults -Force |
            Out-Null
        Copy-Item -LiteralPath (Join-Path $ScriptRoot 'check_test_skips.ps1') -Destination (
            Join-Path $skipScripts 'check_test_skips.ps1'
        )
        Set-Content -LiteralPath (Join-Path $skipTests 'skip_site_inventory.txt') `
            -Value '# no static QSKIP sites in fixture' -Encoding ascii
        Set-Content -LiteralPath (Join-Path $skipTests 'skip_baseline.txt') -Value (
            'current_test::CurrentTests::external_optIn = external service is intentionally absent'
        ) -Encoding ascii
        $currentLog = Join-Path $skipResults 'current_test.txt'
        $removedLog = Join-Path $skipResults 'removed_test.txt'
        $qtLog = @'
********* Start testing of CurrentTests *********
SKIP   : CurrentTests::external_optIn() external service absent
Totals: 1 passed, 0 failed, 1 skipped, 0 blacklisted, 1ms
'@
        Set-Content -LiteralPath $currentLog -Value $qtLog -Encoding ascii
        Set-Content -LiteralPath $removedLog -Value ($qtLog.Replace('CurrentTests', 'RemovedTests')) `
            -Encoding ascii
        $registrationPath = Join-Path $skipRoot 'registration.json'
        $registration = [ordered]@{
            tests = @([ordered]@{
                    name = 'current_test'
                    command = @('current_test.exe', '-o', "$currentLog,txt", '-o', '-,txt')
                })
        }
        [IO.File]::WriteAllText(
            $registrationPath,
            ($registration | ConvertTo-Json -Depth 6),
            [Text.UTF8Encoding]::new($false)
        )
        $skipAudit = Join-Path $skipScripts 'check_test_skips.ps1'
        $skipClean = Invoke-Script $skipAudit @(
            '-ResultsDir', $skipResults, '-RegistrationJsonPath', $registrationPath
        )
        Assert-True ($skipClean.exit_code -eq 0 -and
            $skipClean.stdout -match 'stale removed-test logs ignored:\s*1') (
            'Skip audit counted a stale removed-test log'
        )
        Remove-Item -LiteralPath $currentLog -Force -ErrorAction Stop
        $skipMissing = Invoke-Script $skipAudit @(
            '-ResultsDir', $skipResults, '-RegistrationJsonPath', $registrationPath
        )
        Assert-True ($skipMissing.exit_code -ne 0 -and
            ($skipMissing.stdout + $skipMissing.stderr) -match 'REGISTERED QT TESTS WITH NO LOG') (
            'Skip audit accepted a missing current registered-test log'
        )
    }
    finally {
        Pop-Location
    }
    }
}
finally {
    Remove-FalseGreenFixtureRoot $root
}
Write-Host "False-green $Scope command tests passed under $($PSVersionTable.PSVersion)."
