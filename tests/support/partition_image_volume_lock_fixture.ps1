# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScriptPath,
    [switch]$RunGuardDrills
)

$ErrorActionPreference = 'Stop'

function Assert-ImageFixture([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-ImageScriptAst([string]$Text) {
    if ($null -eq $script:ImageAstCache) {
        $script:ImageAstCache = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    }
    if ($script:ImageAstCache.ContainsKey($Text)) { return $script:ImageAstCache[$Text] }
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $Text, [ref]$tokens, [ref]$errors)
    Assert-ImageFixture ($errors.Count -eq 0) 'Generated image script has PowerShell syntax errors'
    # Cache only a successfully parsed, exact source string. Every different mutant gets
    # its own AST; there is no path-based or stale-source fallback during guard drills.
    $script:ImageAstCache.Add($Text,$ast)
    return $ast
}

function Get-ImageNativeCommand([string]$Text, [string]$ClassName = 'ImageVolumeLock') {
    $ast = Get-ImageScriptAst $Text
    $commands = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Add-Type' -and
        $node.Extent.Text.Contains('class ' + $ClassName)
    }, $true))
    Assert-ImageFixture ($commands.Count -eq 1) "Expected one emitted native type: $ClassName"
    return $commands[0].Extent.Text
}

function Assert-ImageReadBufferContract {
    $bytes = [byte[]](17, 29, 43)
    $stream = [IO.MemoryStream]::new($bytes)
    try {
        $buffer = Read-SakExact $stream $bytes.Length
        Assert-ImageFixture ($buffer -is [byte[]]) 'Read-SakExact boxed its byte buffer into Object[]'
        Assert-ImageFixture ([Convert]::ToBase64String($buffer) -ceq
            [Convert]::ToBase64String($bytes)) 'Read-SakExact changed fixture bytes'
        $empty = Read-SakExact $stream 0
        Assert-ImageFixture ($empty -is [byte[]] -and $empty.Length -eq 0) 'Read-SakExact lost the empty byte buffer'
    } finally { $stream.Dispose() }
}

function Test-ImageComparerContract([type]$Type) {
    $method = $Type.GetMethod('FirstMismatch')
    Assert-ImageFixture ($null -ne $method -and $method.IsStatic -and $method.ReturnType -eq [int]) (
        'Compiled byte comparer signature changed'
    )
    $same = [byte[]](1, 2, 3, 4, 5)
    $cases = @(
        @{ name = 'equal'; left = $same; right = $same; count = 5; expected = -1 },
        @{ name = 'empty'; left = [byte[]]@(); right = [byte[]]@(); count = 0; expected = -1 },
        @{ name = 'prefix-only'; left = $same; right = [byte[]](1, 2, 3, 99, 99); count = 3; expected = -1 },
        @{ name = 'first'; left = $same; right = [byte[]](99, 2, 3, 4, 5); count = 5; expected = 0 },
        @{ name = 'middle'; left = $same; right = [byte[]](1, 2, 99, 4, 5); count = 5; expected = 2 },
        @{ name = 'last'; left = $same; right = [byte[]](1, 2, 3, 4, 99); count = 5; expected = 4 },
        @{ name = 'first-of-many'; left = $same; right = [byte[]](1, 99, 3, 99, 5); count = 5; expected = 1 },
        @{ name = 'null-left'; left = $null; right = $same; count = 0; invalid = $true },
        @{ name = 'null-right'; left = $same; right = $null; count = 0; invalid = $true },
        @{ name = 'negative-count'; left = $same; right = $same; count = -1; invalid = $true },
        @{ name = 'left-bound'; left = [byte[]](1); right = $same; count = 2; invalid = $true },
        @{ name = 'right-bound'; left = $same; right = [byte[]](1); count = 2; invalid = $true }
    )
    foreach ($case in $cases) {
        $arguments = [object[]]::new(3)
        $arguments[0] = $case.left; $arguments[1] = $case.right; $arguments[2] = [int]$case.count
        $failure = $null
        $result = $null
        try { $result = $method.Invoke($null, $arguments) }
        catch {
            $failure = $_.Exception
            while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
        }
        if ($case.invalid) {
            Assert-ImageFixture ($failure -is [ArgumentException]) "Comparer control $($case.name) lost bounded refusal"
        } else {
            Assert-ImageFixture ($null -eq $failure -and $result -eq $case.expected) (
                "Comparer control $($case.name) returned the wrong first mismatch"
            )
        }
    }
    Write-Host 'PASS actual byte comparer: twelve value/bounds controls'
}

function Test-ImageVerificationFunctions([string]$Text) {
    foreach ($name in @('Read-SakExact', 'Assert-SakFullCopy', 'Assert-SakSampleCopy')) {
        . ([scriptblock]::Create((Get-ImageFunctionText $Text $name)))
    }
    Assert-ImageReadBufferContract
    $bufferBytes = 128
    $cases = @(
        @{ mode = 'Full'; count = 300; mismatch = -1 },
        @{ mode = 'Full'; count = 300; mismatch = 0 },
        @{ mode = 'Full'; count = 300; mismatch = 257 },
        @{ mode = 'Full'; count = 300; mismatch = 299 },
        @{ mode = 'Sample'; count = 1024; mismatch = -1 },
        @{ mode = 'Sample'; count = 1024; mismatch = 0 },
        @{ mode = 'Sample'; count = 1024; mismatch = 479 },
        @{ mode = 'Sample'; count = 1024; mismatch = 1023 },
        @{ mode = 'Sample'; count = 1024; mismatch = 200; outsideSample = $true },
        @{ mode = 'Full'; count = 300; mismatch = -1; short = $true },
        @{ mode = 'Sample'; count = 1024; mismatch = -1; short = $true }
    )
    foreach ($case in $cases) {
        $sourceStart = 5
        $targetStart = 7
        $left = [byte[]]::new($sourceStart + $case.count)
        $right = [byte[]]::new($targetStart + $case.count)
        for ($index = 0; $index -lt $case.count; $index++) {
            $left[$sourceStart + $index] = $index % 251
            $right[$targetStart + $index] = $index % 251
        }
        if ($case.mismatch -ge 0) { $right[$targetStart + $case.mismatch] = 255 }
        if ($case.short) { $right = $right[0..($right.Length - 2)] }
        $from = [IO.MemoryStream]::new($left)
        $to = [IO.MemoryStream]::new($right)
        $failure = ''
        try {
            & ('Assert-Sak' + $case.mode + 'Copy') $from $to $case.count $sourceStart $targetStart
        } catch { $failure = $_.Exception.Message }
        finally { $from.Dispose(); $to.Dispose() }
        $expected = ''
        if ($case.short) { $expected = 'Unexpected end of stream during verification' }
        elseif ($case.mismatch -ge 0 -and -not $case.outsideSample) {
            $expected = "$($case.mode) verification mismatch at byte $($case.mismatch)"
        }
        Assert-ImageFixture ($failure -ceq $expected) (
            "Verification control $($case.mode)/$($case.mismatch)/short=$($case.short) expected '$expected'; got '$failure'"
        )
    }
    Write-Host 'PASS actual Full/Sample verification: eleven offset/chunk/sample controls'
}

function Test-ImageComparerDrills([string]$Command, [string]$Text) {
    $nativeDrills = @(
        @{ name = 'left-null'; from = 'left == null || '; to = ''; case = 'null-left' },
        @{ name = 'right-null'; from = 'right == null || '; to = ''; case = 'null-right' },
        @{ name = 'negative-count'; from = 'count < 0 ||'; to = ''; case = 'negative-count' },
        @{ name = 'left-bound'; from = 'count > left.Length || '; to = ''; case = 'left-bound' },
        @{ name = 'right-bound'; from = 'count > right.Length'; to = 'count > Math.Max(count, 0)'; case = 'right-bound' },
        @{ name = 'prefix-count'; from = 'index < count'; to = 'index < left.Length'; case = 'prefix-only' },
        @{ name = 'last-byte'; from = 'index < count'; to = 'index < count - 1'; case = 'last' },
        @{ name = 'first-mismatch'; from = 'return index;'; to = 'return count - 1;'; case = 'first' },
        @{ name = 'equal-result'; from = 'return -1;'; to = 'return 0;'; case = 'equal' }
    )
    for ($index = 0; $index -lt $nativeDrills.Count; $index++) {
        $drill = $nativeDrills[$index]
        Assert-ImageFixture ($Command.Contains($drill.from)) "Comparer mutation anchor absent: $($drill.name)"
        $namespace = 'SakComparerDrill' + $index
        $changed = $Command.Replace($drill.from, $drill.to).Replace('namespace Sak {', "namespace $namespace {")
        & ([scriptblock]::Create($changed)) | Out-Null
        $caught = ''
        try { Test-ImageComparerContract (($namespace + '.CloneByteComparer') -as [type]) }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture ($caught.Contains('Comparer control ' + $drill.case)) "Comparer guard survived: $($drill.name)"
        Write-Host "KILLED comparer-$($drill.name)"
    }
    $scriptDrills = @(
        @{ name = 'typed-read-buffer'; from = 'return ,$buf'; to = 'return $buf'; failure = 'boxed its byte buffer' },
        @{ name = 'full-global-offset'; from = '$pos + [uint64]$mismatch'; to = '[uint64]$mismatch'; failure = 'Full/257' },
        @{ name = 'sample-global-offset'; from = '$point + [uint64]$mismatch'; to = '[uint64]$mismatch'; failure = 'Sample/479' },
        @{ name = 'sample-head'; from = '@([uint64]0, $middle, $tail)'; to = '@($middle, $tail)'; failure = 'Sample/0' },
        @{ name = 'sample-middle'; from = '@([uint64]0, $middle, $tail)'; to = '@([uint64]0, $tail)'; failure = 'Sample/479' },
        @{ name = 'sample-tail'; from = '@([uint64]0, $middle, $tail)'; to = '@([uint64]0, $middle)'; failure = 'Sample/1023' }
    )
    foreach ($drill in $scriptDrills) {
        Assert-ImageFixture ($Text.Contains($drill.from)) "Verification mutation anchor absent: $($drill.name)"
        $caught = ''
        try { Test-ImageVerificationFunctions ($Text.Replace($drill.from, $drill.to)) }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture ($caught.Contains($drill.failure)) "Verification guard survived: $($drill.name)"
        Write-Host "KILLED verification-$($drill.name)"
    }
}

function Get-ImageFunctionText([string]$Text, [string]$Name) {
    if ($null -eq $script:ImageFunctionCache) {
        $script:ImageFunctionCache = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    }
    if (-not $script:ImageFunctionCache.ContainsKey($Text)) {
        $script:ImageFunctionCache.Add($Text,[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal))
    }
    $cache = $script:ImageFunctionCache[$Text]
    if ($cache.ContainsKey($Name)) { return $cache[$Name] }
    $ast = Get-ImageScriptAst $Text
    $functions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq $Name
    }, $true))
    Assert-ImageFixture ($functions.Count -eq 1) "Expected one emitted image function: $Name"
    $cache.Add($Name,$functions[0].Extent.Text)
    return $functions[0].Extent.Text
}

function Assert-ImageNativeContract([type]$NativeType) {
    Assert-ImageFixture ($null -ne $NativeType) 'Emitted image-lock native type was not compiled'
    $privateStatic = [Reflection.BindingFlags]'NonPublic,Static'
    # These values derive from the Windows SDK CTL_CODE/access/disposition definitions,
    # independently of the generated implementation under test.
    $constants = [ordered]@{
        GenericRead = [uint32]2147483648
        ShareReadWrite = [uint32]3
        OpenExisting = [uint32]3
        LockVolume = [uint32]589848
        AllowExtendedDASD = [uint32]589955
        StorageDeviceNumber = [uint32]2953344
        GetDiskAttributes = [uint32]458992
        DiskDeviceType = [uint32]7
    }
    foreach ($entry in $constants.GetEnumerator()) {
        $field = $NativeType.GetField($entry.Key, $privateStatic)
        Assert-ImageFixture ($null -ne $field -and $field.IsLiteral -and
            $field.FieldType -eq [uint32] -and
            $field.GetRawConstantValue() -eq $entry.Value) (
            "Native image-lock constant mismatch: $($entry.Key)"
        )
    }
    $create = $NativeType.GetMethod('CreateFile', $privateStatic)
    Assert-ImageFixture ($null -ne $create -and
        $create.ReturnType -eq [Microsoft.Win32.SafeHandles.SafeFileHandle]) (
        'Native CreateFile must return an owning SafeFileHandle'
    )
    $imports = @($NativeType.GetMethods($privateStatic) | Where-Object {
        $_.Name -ceq 'CreateFile' -or $_.Name -ceq 'DeviceIoControl'
    })
    Assert-ImageFixture ($imports.Count -eq 4) 'Native image-lock import overload count changed'
    foreach ($method in $imports) {
        $attributes = @($method.GetCustomAttributes([Runtime.InteropServices.DllImportAttribute], $false))
        Assert-ImageFixture ($attributes.Count -eq 1 -and
            $attributes[0].Value -ieq 'kernel32.dll' -and $attributes[0].SetLastError) (
            "Native import must retain Windows error status: $($method.Name)"
        )
        if ($method.Name -ceq 'CreateFile') {
            Assert-ImageFixture ($attributes[0].CharSet -eq [Runtime.InteropServices.CharSet]::Unicode) (
                'Native CreateFile must use the Unicode Windows API'
            )
        } else {
            Assert-ImageFixture ($method.ReturnType -eq [bool] -and
                $method.GetParameters()[0].ParameterType -eq
                    [Microsoft.Win32.SafeHandles.SafeFileHandle]) (
                'Native DeviceIoControl must use the held SafeFileHandle and report success'
            )
        }
    }
    $acquire = $NativeType.GetMethod('Acquire')
    Assert-ImageFixture ($null -ne $acquire -and $acquire.IsStatic -and
        $acquire.ReturnType -eq [Microsoft.Win32.SafeHandles.SafeFileHandle] -and
        $acquire.GetParameters().Count -eq 1 -and
        $acquire.GetParameters()[0].ParameterType -eq [string]) (
        'Native Acquire must accept a literal path and return a SafeFileHandle'
    )
    $attributes = $NativeType.GetNestedType('DiskAttributes',[Reflection.BindingFlags]::NonPublic)
    Assert-ImageFixture ($null -ne $attributes -and [Runtime.InteropServices.Marshal]::SizeOf([Activator]::CreateInstance($attributes)) -eq 16 -and
        $attributes.GetField('Version').FieldType -eq [uint32] -and
        $attributes.GetField('Reserved').FieldType -eq [uint32] -and
        $attributes.GetField('Flags').FieldType -eq [uint64]) 'Native disk-attributes ABI is not the 16-byte Windows structure'
}

function Test-ImageNativeOrdinaryFileRefusal([type]$NativeType) {
    $directory = [IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $ScriptPath).Path)
    $path = Join-Path $directory ('ordinary-lock-fixture-' + [guid]::NewGuid().ToString('N') + '.txt')
    $content = [byte[]](11, 22, 33, 44)
    [IO.File]::WriteAllBytes($path, $content)
    try {
        $failure = $null
        $unexpectedHandle = $null
        try {
            $unexpectedHandle = $NativeType.GetMethod('Acquire').Invoke($null, @($path))
        } catch {
            $failure = $_.Exception
        } finally {
            if ($null -ne $unexpectedHandle) { $unexpectedHandle.Dispose() }
        }
        Assert-ImageFixture ($null -ne $failure) 'Native image-volume lock accepted an ordinary file'
        Assert-ImageFixture ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -ceq
            [Convert]::ToBase64String($content)) 'Failed volume lock changed an ordinary file'
        $exclusive = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None)
        $exclusive.Dispose()
        Write-Host 'PASS actual native image-lock ordinary-file refusal and handle cleanup'
    } finally {
        Remove-Item -LiteralPath $path -Force -ErrorAction Stop
    }
}

function Get-ImageMainText([string]$Text) {
    $ast = Get-ImageScriptAst $Text
    $start = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
        $_.Left.Extent.Text -ceq '$sakImageResources'
    })
    $end = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.IfStatementAst] -and
        $_.Extent.Text.Contains('Create Image source lock cleanup failed:')
    })
    Assert-ImageFixture ($start.Count -eq 1 -and $end.Count -eq 1 -and
        $end[0].Extent.EndOffset -gt $start[0].Extent.StartOffset) (
        'Generated image lock/copy/cleanup main boundary is absent or ambiguous'
    )
    return $Text.Substring($start[0].Extent.StartOffset,
        $end[0].Extent.EndOffset - $start[0].Extent.StartOffset)
}

function Get-ImageLayoutDiagnostic([string]$Text) {
    $ast = Get-ImageScriptAst $Text
    $blocks = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -ceq '$sakImageLayoutKey -cne $sakImageLockedLayoutKey'
    }, $true))
    Assert-ImageFixture ($blocks.Count -eq 1) 'Expected one source-layout mismatch diagnostic'
    return $blocks[0].Extent.Text
}

function Test-ImageLayoutDiagnostic([string]$Text) {
    $block = [scriptblock]::Create((Get-ImageLayoutDiagnostic $Text))
    $cases = @(
        @{ before = 'same'; after = 'same' },
        @{ before = '{"before":"one"}'; after = '{"after":"two"}' },
        @{ before = ('B' * 5003); after = ('A' * 6007) }
    )
    foreach ($case in $cases) {
        $sakImageLayoutKey = $case.before
        $sakImageLockedLayoutKey = $case.after
        $failure = ''
        try { & $block } catch { $failure = $_.Exception.Message }
        $expected = ''
        if ($case.before -cne $case.after) {
            $before = $case.before.Substring(0, [Math]::Min(4096, $case.before.Length))
            $after = $case.after.Substring(0, [Math]::Min(4096, $case.after.Length))
            $expected = 'Create Image source layout changed while acquiring volume locks. ' +
                "Layout keys (up to 4096 characters each): Before ($($case.before.Length) characters): $before; " +
                "After ($($case.after.Length) characters): $after"
        }
        Assert-ImageFixture ($failure -ceq $expected) (
            'Layout diagnostic did not retain both original lengths and independently bounded before/after keys'
        )
    }
    Write-Host 'PASS actual layout diagnostic: equal, distinct and independently capped keys'
}

function Test-ImageLayoutDiagnosticDrills([string]$Text) {
    $block = Get-ImageLayoutDiagnostic $Text
    $brokenFormat = $block.Replace("throw (('Create", "throw ('Create").Replace(
        "characters): {4}') -f", "characters): {4}' -f")
    Assert-ImageFixture ($brokenFormat -cne $block) 'Diagnostic grouping mutation anchor absent'
    $mutants = @(@{ name = 'format-grouping'; text = $brokenFormat })
    foreach ($variable in @('sakImageLayoutKey', 'sakImageLockedLayoutKey')) {
        $anchor = '[Math]::Min($sakImageDiagnosticLimit, $' + $variable + '.Length)'
        Assert-ImageFixture ($block.Contains($anchor)) "Diagnostic cap anchor absent: $variable"
        $mutants += @{ name = $variable + '-cap'; text = $block.Replace($anchor, ('$' + $variable + '.Length')) }
    }
    foreach ($mutant in $mutants) {
        $caught = ''
        try { Test-ImageLayoutDiagnostic ($Text.Replace($block, $mutant.text)) }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture ($caught.Contains('Layout diagnostic did not retain')) (
            "Diagnostic guard survived or failed infrastructure: $($mutant.name)"
        )
        Write-Host "KILLED image-volume diagnostic-$($mutant.name)"
    }
}

function Test-ImageLayoutModeRequired([string]$Text) {
    . ([scriptblock]::Create((Get-ImageFunctionText $Text 'Get-SakImageSourceLayout')))
    function Get-SakOptionalDiskPartitions { throw 'fixture inventory query without explicit phase' }
    $failure = ''
    try { Get-SakImageSourceLayout | Out-Null } catch { $failure = $_.Exception.Message }
    Assert-ImageFixture ($failure -ceq 'Create Image source layout query requires an explicit volume-resolution phase') (
        'Source layout accepted or prompted for an omitted volume-resolution phase'
    )
    Write-Host 'PASS source-layout phase omission refuses without querying storage or prompting'
}

function Test-ImageLocatorContract([string]$Text) {
    . ([scriptblock]::Create((Get-ImageFunctionText $Text 'Get-SakImageVolumePath')))
    $volume = '\\?\Volume{44444444-4444-4444-4444-444444444444}\'
    $canonical = $volume.TrimEnd('\').ToLowerInvariant()
    $msr = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
    $basic = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    $cases = @(
        @{ name='canonical'; paths=@('E:\',$volume); drive=[char]'E'; type=$basic; value=$canonical },
        @{ name='duplicate-alias'; paths=@($volume,$volume.ToUpperInvariant()); drive=[char]'E'; type=$basic; value=$canonical },
        @{ name='msr-null-char'; paths=@($null,''); drive=[char]0; type=$msr; value='' },
        @{ name='msr-null'; paths=$null; drive=$null; type=$msr; value='' },
        @{ name='msr-volume-hint'; paths=@($volume); drive=[char]0; type=$msr; value=$canonical },
        @{ name='msr-drive-hint'; paths=@(); drive=[char]'E'; type=$msr; error='cannot resolve an exclusive' },
        @{ name='msr-path-hint'; paths=@('E:\'); drive=[char]0; type=$msr; error='cannot resolve an exclusive' },
        @{ name='msr-whitespace-hint'; paths=@(' '); drive=[char]0; type=$msr; error='cannot resolve an exclusive' },
        @{ name='basic-no-hints'; paths=@(); drive=[char]0; type=$basic; error='cannot resolve an exclusive' },
        @{ name='missing-type-no-hints'; paths=@(); drive=[char]0; type=$null; error='cannot resolve an exclusive' },
        @{ name='drive-only'; paths=@('E:\'); drive=[char]'E'; type=$basic; error='cannot resolve an exclusive' },
        @{ name='nonstring-path'; paths=@($volume,7); drive=[char]'E'; type=$basic; error='access path is not a string' },
        @{ name='invalid-guid'; paths=@('\\?\Volume{------------------------------------}\'); drive=[char]0; type=$basic; error='volume GUID is invalid' },
        @{ name='ambiguous-guid'; paths=@($volume,'\\?\Volume{55555555-5555-5555-5555-555555555555}\'); drive=[char]'E'; type=$basic; error='volume identity is ambiguous' }
    )
    foreach ($case in $cases) {
        $part = [pscustomobject]@{ AccessPaths=$case.paths; DriveLetter=$case.drive; GptType=$case.type }
        $failure=''; $result=$null
        try { $result=Get-SakImageVolumePath $part } catch { $failure=$_.Exception.Message }
        if ($case.error) {
            Assert-ImageFixture ($failure.Contains($case.error)) "Locator control $($case.name) lost expected refusal"
        } else {
            Assert-ImageFixture (-not $failure -and $result -ceq $case.value) "Locator control $($case.name) selected the wrong identity"
        }
    }
    Write-Host 'PASS primary AccessPaths locator: fourteen identity/MSR controls'
}

function Test-ImagePartitionCountContract([string]$Text) {
    . ([scriptblock]::Create((Get-ImageFunctionText $Text 'Assert-SakImagePartitionCount')))
    foreach ($count in @([int]2,[uint32]2,[long]2,[uint64]2)) {
        Assert-SakImagePartitionCount ([pscustomobject]@{NumberOfPartitions=$count}) 2
    }
    Assert-SakImagePartitionCount ([pscustomobject]@{NumberOfPartitions=[uint32]0}) 0
    foreach ($count in @($null,'2',[double]2,$true,-1,1,[uint64]::MaxValue)) {
        $failure=''
        try { Assert-SakImagePartitionCount ([pscustomobject]@{NumberOfPartitions=$count}) 2 }
        catch { $failure=$_.Exception.Message }
        Assert-ImageFixture ($failure.Contains('partition count is unknown or disagrees')) 'Partition-count control accepted an unknown, coerced or inconsistent count'
    }
    Write-Host 'PASS authoritative partition count: five positive and seven negative controls'
}

function Test-ImageLocatorDrills([string]$Text) {
    $drills = @(
        @{name='msr-type'; from='$msr -and -not $hasPathHint -and $noDrive'; to='-not $hasPathHint -and $noDrive'; case='basic-no-hints'},
        @{name='msr-path-hint'; from='$msr -and -not $hasPathHint -and $noDrive'; to='$msr -and $noDrive'; case='msr-path-hint'},
        @{name='msr-drive-hint'; from='$msr -and -not $hasPathHint -and $noDrive'; to='$msr -and -not $hasPathHint'; case='msr-drive-hint'},
        @{name='nul-drive'; from='$drive -eq [string][char]0'; to='$drive -eq ''never-nul'''; case='msr-null-char'},
        @{name='ambiguity'; from='$guids.Count -gt 1'; to='$guids.Count -lt 0'; case='ambiguous-guid'},
        @{name='path-type'; from='$path -isnot [string]'; to='$path -isnot [string] -and $false'; case='nonstring-path'},
        @{name='guid-parse'; from='[ref]$parsed))'; to='[ref]$parsed) -and $false)'; case='invalid-guid'}
    )
    foreach ($drill in $drills) {
        Assert-ImageFixture ($Text.Contains($drill.from)) "Locator guard anchor absent: $($drill.name)"
        $caught=''
        try { Test-ImageLocatorContract ($Text.Replace($drill.from,$drill.to)) } catch { $caught=$_.Exception.Message }
        Assert-ImageFixture ($caught.Contains('Locator control '+$drill.case)) "Locator guard survived: $($drill.name)"
        Write-Host "KILLED locator-$($drill.name)"
    }
    $countDrills = @(
        @{name='integer-type'; from='-not $integer -or '; to=''},
        @{name='enumeration-agreement'; from='[uint64]$count -ne [uint64]$Actual'; to='[uint64]$count -ne [uint64]$count'}
    )
    foreach ($drill in $countDrills) {
        Assert-ImageFixture ($Text.Contains($drill.from)) "Count guard anchor absent: $($drill.name)"
        $caught=''
        try { Test-ImagePartitionCountContract ($Text.Replace($drill.from,$drill.to)) } catch { $caught=$_.Exception.Message }
        Assert-ImageFixture ($caught.Contains('Partition-count control accepted')) "Count guard survived: $($drill.name)"
        Write-Host "KILLED partition-count-$($drill.name)"
    }
}

function Test-ImageRuntimeCase([string]$Text, [string]$Case, [string]$ExpectedFailure = '') {
    $parent = [IO.Path]::GetDirectoryName((Resolve-Path -LiteralPath $ScriptPath).Path)
    $root = Join-Path $parent ('image-runtime-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root -ErrorAction Stop | Out-Null
    $state = @{
        Case = $Case; LayoutQueries = 0; VolumeQueries = 0; IdentityQueries = 0; Opens = 0; Verified = $false
        Control = $null; AttributeQueries = 0; PhysicalReads = 0; LockedReads = 0
        Handles = [Collections.Generic.List[ImageFixtureResource]]::new()
        Streams = [Collections.Generic.List[IO.FileStream]]::new()
        Source = (Join-Path $root 'source.bin'); Destination = (Join-Path $root 'destination.bin')
    }
    [ImageFixtureResource]::ClosedOrder.Clear()
    try {
        $functions = @('Get-SakImageSourceLayout', 'Get-SakImageLayoutKey', 'Get-SakImageVolumePath',
            'Assert-SakImagePartitionCount', 'Assert-SakImageAttributeMode', 'Close-SakImageSourceLocks',
            'Assert-SakImageSourceIdentity', 'Get-SakOptionalDiskPartitions',
            'Get-SakOptionalPartitionVolume', 'Read-SakExact', 'Copy-SakBytes',
            'Assert-SakFullCopy', 'Assert-SakSampleCopy')
        foreach ($name in $functions) {
            . ([scriptblock]::Create((Get-ImageFunctionText $Text $name)))
        }
        Assert-ImageReadBufferContract
        $ast = Get-ImageScriptAst $Text
        foreach ($name in @('$sakImageSourceDisk', '$sakImageSourcePartition', '$sakImageSourceSize',
            '$sakImageCaptureMode', '$sakImageExpectedOffline', '$sakImageDiskPath',
            '$src', '$expectedBytes', '$sourceOffset')) {
            $assignment = @($ast.EndBlock.Statements | Where-Object {
                $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
                $_.Left.Extent.Text -ceq $name
            })
            Assert-ImageFixture ($assignment.Count -eq 1) "Missing source fixture value: $name"
            . ([scriptblock]::Create($assignment[0].Extent.Text))
        }
        Assert-ImageFixture ($sakImageSourceDisk -eq 7 -and $sakImageSourceSize -eq 4096 -and
            $sakImageSourcePartition -in @(0, 2)) 'C++ runtime fixture identity contract changed'
        Assert-ImageFixture ($sakImageCaptureMode -cin @('LockedVolumes', 'OfflineDisk')) 'Explicit source capture mode missing'
        $offline = $sakImageCaptureMode -ceq 'OfflineDisk'
        $expectedOffset = if ($offline -and $sakImageSourcePartition -ne 0) { 1024 } else { 0 }
        Assert-ImageFixture ($sourceOffset -eq $expectedOffset) 'Offline selected-partition offset was not emitted exactly'
        $dst = $state.Destination
        Assert-ImageFixture ($expectedBytes -eq $(if ($sakImageSourcePartition -eq 0) { 4096 } else { 1024 })) 'Pinned source byte count changed'
        $targetOffset = [uint64]0
        $verifyMode = 'Full'
        $bufferBytes = 128
        $content = [byte[]]::new([int]($expectedBytes + $sourceOffset))
        for ($index = 0; $index -lt $content.Length; $index++) { $content[$index] = $index % 251 }
        $expectedContent = [byte[]]($content[[int]$sourceOffset..([int]($sourceOffset + $expectedBytes) - 1)])
        if ($Case -eq 'copy-failure') { $content = $content[0..([int]$sourceOffset + 127)] }
        [IO.File]::WriteAllBytes($state.Source, $content)

        function Get-Disk {
            param($Number, $ErrorAction)
            Assert-ImageFixture ($Number -eq 7) 'Identity query escaped the selected fixture disk'
            $caller = (Get-PSCallStack | Select-Object -Skip 1 -First 1).FunctionName
            if ($caller -ceq 'Assert-SakImageSourceIdentity') { $state.IdentityQueries++ }
            $id = 'IMAGE-SOURCE'
            if ($caller -ceq 'Assert-SakImageSourceIdentity' -and (($Case -eq 'identity-before' -and $state.IdentityQueries -eq 1) -or
                ($Case -eq 'identity-after-lock' -and $state.IdentityQueries -eq 2) -or
                ($Case -eq 'identity-after-open' -and $state.IdentityQueries -eq 3) -or
                ($Case -eq 'identity-after-verify' -and $state.IdentityQueries -eq 4))) { $id = 'changed' }
            $count = 2
            if ($Case -in @('raw-empty', 'raw-empty-count-missing')) { $count = 0 }
            if ($Case -in @('msr-only', 'missing-partition')) { $count = 1 }
            if ($Case -eq 'duplicate-number') { $count = 3 }
            if ($Case -eq 'count-drift' -and $state.LayoutQueries -ge 2) { $count = 1 }
            if ($Case -in @('count-missing', 'raw-empty-count-missing')) { $count = $null }
            if ($Case -eq 'count-string') { $count = '2' }
            if ($Case -eq 'count-float') { $count = [double]2 }
            if ($Case -eq 'count-negative') { $count = -1 }
            if ($Case -eq 'count-mismatch') { $count = 3 }
            $isOffline = [object]$offline
            if ($Case -eq 'cim-offline-unknown') { $isOffline = $null }
            if ($Case -eq 'cim-offline-string') { $isOffline = 'true' }
            if ($Case -eq 'cim-mode-mismatch' -or ($Case -eq 'cim-offline-final-change' -and $state.IdentityQueries -ge 4)) { $isOffline = -not $offline }
            $boot = [object]$false; $system = [object]$false
            if ($Case -eq 'cim-boot-unknown') { $boot = $null }
            if ($Case -eq 'cim-system-string') { $system = 'false' }
            if ($Case -eq 'offline-boot-source') { $boot = $true }
            if ($Case -eq 'offline-system-source') { $system = $true }
            return [pscustomobject]@{ UniqueId = $id; SerialNumber = 'IMAGE-SERIAL'; Size = 4096;
                IsOffline = $isOffline; IsBoot = $boot; IsSystem = $system; NumberOfPartitions = $count }
        }
        function Get-Partition {
            param($DiskNumber, $PartitionNumber, $ErrorAction)
            Assert-ImageFixture ($DiskNumber -eq 7) 'Layout query escaped the selected fixture disk'
            if (-not $PSBoundParameters.ContainsKey('PartitionNumber')) { $state.LayoutQueries++ }
            $parts = @(
                [pscustomobject]@{ DiskNumber = 7; PartitionNumber = 1; Offset = 0; Size = 1024; Guid = '{22222222-2222-2222-2222-222222222222}'; GptType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'; MbrType = 7; DriveLetter = [char]'F'; AccessPaths = @('F:\', '\\?\Volume{33333333-3333-3333-3333-333333333333}\') },
                [pscustomobject]@{ DiskNumber = 7; PartitionNumber = 2; Offset = 1024; Size = 1024; Guid = '{11111111-1111-1111-1111-111111111111}'; GptType = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'; MbrType = 7; DriveLetter = [char]'E'; AccessPaths = @('E:\', '\\?\Volume{44444444-4444-4444-4444-444444444444}\') }
            )
            if ($Case -in @('msr-only', 'post-lock-volume-loss')) {
                $parts[0].GptType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
                $parts[0].DriveLetter = [char]0; $parts[0].AccessPaths = @($null)
            }
            if ($Case -eq 'msr-only') { $parts = @($parts[0]) }
            if ($Case -eq 'selected-msr-no-volume') {
                $parts[1].GptType = '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'
                $parts[1].DriveLetter = [char]0; $parts[1].AccessPaths = @($null)
            }
            if (-not $PSBoundParameters.ContainsKey('PartitionNumber') -and
                $Case -in @('raw-empty', 'raw-empty-count-missing', 'empty-enumeration-mismatch')) { return @() }
            if ($Case -eq 'missing-partition') { $parts = @($parts[0]) }
            if ($Case -eq 'wrong-disk') { $parts[1].DiskNumber = 8 }
            if ($Case -eq 'missing-number') { $parts[1].PartitionNumber = $null }
            if ($Case -eq 'duplicate-number') { $parts += $parts[1] }
            if ($Case -eq 'negative-offset') { $parts[1].Offset = -1 }
            if ($Case -eq 'zero-size') { $parts[1].Size = 0 }
            if ($Case -eq 'past-end') { $parts[1].Size = 4096 }
            if ($Case -eq 'no-volume') { $parts[1].AccessPaths = @(); $parts[1].DriveLetter = [char]0 }
            if ($Case -eq 'malformed-volume') { $parts[1].AccessPaths = @('not-a-volume') }
            if ($Case -eq 'ambiguous-volume') { $parts[1].AccessPaths += '\\?\Volume{55555555-5555-5555-5555-555555555555}\' }
            if ($Case -eq 'duplicate-volume') { $parts[1].AccessPaths = @('E:\', '\\?\Volume{33333333-3333-3333-3333-333333333333}\') }
            if ($state.LayoutQueries -ge 2) {
                if ($Case -in @('layout-drift', 'offset-drift')) { $parts[1].Offset = 1025 }
                if ($Case -eq 'size-drift') { $parts[1].Size = 1023 }
                if ($Case -eq 'guid-drift') { $parts[1].Guid = '{66666666-6666-6666-6666-666666666666}' }
                if ($Case -eq 'gpt-type-drift') { $parts[1].GptType = '{de94bba4-06d1-4d40-a16a-bfd50179d6ac}' }
                if ($Case -eq 'mbr-type-drift') { $parts[1].MbrType = 12 }
                if ($Case -eq 'count-drift') {
                    $parts = if ($sakImageSourcePartition -eq 0) { @($parts[1]) } else { @($parts[0]) }
                }
            }
            if ($state.LayoutQueries -ge 3 -and $Case -eq 'final-layout-drift') { $parts[1].Size = 1023 }
            if ($PSBoundParameters.ContainsKey('PartitionNumber')) {
                # Pinned identity remains unchanged for layout-only failures.
                $guid = if ($Case -eq 'missing-guid') { $null } else { '{11111111-1111-1111-1111-111111111111}' }
                return [pscustomobject]@{ PartitionNumber = 2; Offset = 1024; Size = 1024; Guid = $guid }
            }
            return $parts
        }
        function Get-Volume {
            [CmdletBinding()]
            param([Parameter(ValueFromPipeline = $true)]$InputObject)
            process {
                $state.VolumeQueries++
                if ($Case -eq 'busy-volume-zero-getvolume') { return }
                throw 'Source capture must not query Get-Volume for its primary locator'
            }
        }
        function Open-SakImageDiskControl([string]$Path) {
            Assert-ImageFixture ($Path -ceq '\\.\PhysicalDrive7') 'Control handle used the wrong physical disk path'
            if ($Case -eq 'control-open-failure') { throw 'fixture disk control open failed' }
            $control = [ImageFixtureResource]::new()
            $control.IsControl = $true; $control.OrderToken = -1; $control.VerifyCalls = 1
            $control.ThrowOnDispose = $Case -in @('cleanup-failure', 'primary-and-cleanup')
            $state.Control = $control
            return $control
        }
        function Get-SakImageDiskAttributes($Handle) {
            Assert-ImageFixture ($Handle -eq $state.Control -and -not $Handle.IsClosed) 'Attributes queried through a stale or wrong control handle'
            $state.AttributeQueries++
            if (($Case -eq 'attributes-query-before-failure' -and $state.AttributeQueries -eq 1) -or
                ($Case -eq 'attributes-query-after-failure' -and $state.AttributeQueries -eq 2)) {
                throw 'fixture native attributes query failed'
            }
            $flags = [uint64]$(if ($offline) { 1 } else { 0 })
            if ($Case -eq 'attributes-static-readonly') { $flags = $flags -bor [uint64]2 }
            if ($Case -eq 'native-mode-before-mismatch' -or
                ($Case -eq 'native-mode-after-mismatch' -and $state.AttributeQueries -eq 2)) {
                $flags = $flags -bxor [uint64]1
            }
            if ($state.AttributeQueries -ge 2) {
                Assert-ImageFixture ($state.Verified -and $state.Streams.Count -gt 0 -and
                    $state.Streams[0].CanRead -and $state.IdentityQueries -ge 4 -and
                    $state.Control.VerifyCalls -ge 2) 'Final source attributes were checked outside the held-input verification lifetime'
                if ($Case -eq 'attributes-other-flag-change') { $flags = $flags -bor [uint64]2 }
            }
            return $flags
        }
        function Open-SakImageVolumeLock([string]$Path) {
            Assert-ImageFixture ($Path -match '^\\\\\?\\Volume\{[0-9a-f-]{36}\}$') 'Lock path is not a canonical volume GUID'
            Assert-ImageFixture (-not $offline) 'OfflineDisk capture attempted a mounted-volume lock'
            if ($Case -eq 'busy-volume-zero-getvolume') { throw [ComponentModel.Win32Exception]::new(21, 'fixture source volume busy') }
            if ($Case -eq 'first-lock-failure' -or
                ($Case -eq 'second-lock-failure' -and $state.Handles.Count -eq 1)) {
                throw 'fixture lock failure'
            }
            $handle = [ImageFixtureResource]::new()
            $handle.OrderToken = $state.Handles.Count
            $handle.PartitionNumber = if ($Path.Contains('33333333')) { 1 } else { 2 }
            $handle.ThrowOnDispose = $Case -in @('cleanup-failure', 'primary-and-cleanup')
            $state.Handles.Add($handle)
            return $handle
        }
        function Assert-SakImageDevice($Handle, [uint32]$PartitionNumber, [bool]$CheckPartition) {
            Assert-ImageFixture (-not $Handle.IsClosed) 'Device mapping checked after handle closure'
            if ($Handle -is [ImageFixtureResource]) {
                if ($Handle.IsControl) {
                    Assert-ImageFixture (-not $CheckPartition -and $PartitionNumber -eq 0) 'Disk control mapping was checked as a volume'
                } else {
                    Assert-ImageFixture ($CheckPartition -and $PartitionNumber -eq $Handle.PartitionNumber) (
                        'Volume mapping did not pin the captured partition number'
                    )
                }
                $Handle.VerifyCalls++
                if (-not $Handle.IsControl -and $Case -eq 'held-mapping-drift' -and $Handle.VerifyCalls -eq 2) {
                    throw 'fixture held device mapping changed'
                }
                if (-not $Handle.IsControl -and $Case -eq 'held-mapping-final-drift' -and $Handle.VerifyCalls -eq 3) {
                    throw 'fixture final held device mapping changed'
                }
                if ($Handle.IsControl -and $Case -eq 'control-final-mapping-drift') { throw 'fixture disk control mapping changed' }
            }
            if ($Case -eq 'mapping-failure' -or ($Case -eq 'stream-mapping-failure' -and
                $Handle -is [Microsoft.Win32.SafeHandles.SafeFileHandle])) {
                throw 'fixture device mapping failure'
            }
        }
        function Assert-FixtureLocksHeld {
            Assert-ImageFixture ($null -ne $state.Control -and -not $state.Control.IsClosed) 'Disk identity handle was not held during byte capture'
            foreach ($handle in $state.Handles) {
                Assert-ImageFixture (-not $handle.IsClosed) 'Source lock released before copy/verification'
                Assert-ImageFixture ($handle.VerifyCalls -ge 2) 'Source lock mapping was not revalidated before reading bytes'
            }
            Assert-ImageFixture ($state.IdentityQueries -ge 2 -and $state.LayoutQueries -ge 2) (
                'Source bytes opened before post-lock identity/layout validation'
            )
        }
        function Open-SakRead([string]$Path) {
            Assert-FixtureLocksHeld
            $state.Opens++
            $file = $state.Source
            if ($Path -ceq $dst) {
                $file = $state.Destination
                $state.Verified = $true
                if ($Case -in @('verify-failure', 'verify-late-failure')) {
                    $bytes = [IO.File]::ReadAllBytes($file)
                    $mismatch = if ($Case -eq 'verify-late-failure') { 257 } else { 0 }
                    $bytes[$mismatch] = 255
                    [IO.File]::WriteAllBytes($file, $bytes)
                }
            } else {
                Assert-ImageFixture ($Path -ceq $src) 'Unexpected raw read boundary'
                $state.PhysicalReads++
            }
            $stream = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $state.Streams.Add($stream)
            return $stream
        }
        function Open-SakImageLockedRead($Handle) {
            Assert-ImageFixture (-not $offline) 'OfflineDisk capture opened a locked-volume stream'
            Assert-ImageFixture (-not $Handle.IsClosed) 'Partition read opened after lock closure'
            $state.LockedReads++
            $stream = Open-SakRead $src
            $state.PhysicalReads--
            return $stream
        }
        function Open-SakWrite([string]$Path) {
            Assert-FixtureLocksHeld
            Assert-ImageFixture ($Path -ceq $state.Destination) 'Write escaped ordinary fixture file'
            if ($Case -eq 'primary-and-cleanup') { throw 'fixture primary failure' }
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $state.Streams.Add($stream)
            return $stream
        }
        function Assert-SakRawWriteTarget([string]$Path) {
            Assert-ImageFixture ($Path -ceq $state.Destination) 'Raw target boundary was reached'
            return $null
        }
        function Restore-SakRawWriteTarget($Number) {
            Assert-ImageFixture ($null -eq $Number) 'Fixture attempted disk state restoration'
        }
        $failure = ''
        try { . ([scriptblock]::Create((Get-ImageMainText $Text))) | Out-Null }
        catch { $failure = $_.Exception.Message }
        if ($ExpectedFailure) {
            Assert-ImageFixture ($failure.Contains($ExpectedFailure)) "Case $Case expected '$ExpectedFailure'; got '$failure'"
        } else {
            Assert-ImageFixture (-not $failure) "Case $Case unexpectedly failed: $failure"
            Assert-ImageFixture ($state.Verified -and
                [Convert]::ToBase64String([IO.File]::ReadAllBytes($state.Destination)) -ceq
                [Convert]::ToBase64String($expectedContent)) 'Successful case did not copy and fully verify fixture bytes'
            $expectedLocks = if ($offline -or $Case -in @('raw-empty', 'msr-only')) { 0 }
                elseif ($Case -eq 'post-lock-volume-loss') { 1 }
                elseif ($sakImageSourcePartition -eq 0) { 2 } else { 1 }
            Assert-ImageFixture ($state.Handles.Count -eq $expectedLocks) 'Successful copy did not hold every source volume'
            Assert-ImageFixture ($state.VolumeQueries -eq 0) 'Source locator queried hidden filesystem metadata'
            Assert-ImageFixture ($state.AttributeQueries -eq 2 -and $state.LayoutQueries -eq 3 -and
                $state.IdentityQueries -eq 4 -and $state.Control.VerifyCalls -eq 2) 'Capture omitted a final source-state revalidation'
            foreach ($handle in $state.Handles) {
                Assert-ImageFixture ($handle.VerifyCalls -eq 3) 'Capture omitted a final held-volume mapping check'
            }
            $physical = $offline -or $sakImageSourcePartition -eq 0
            Assert-ImageFixture ($state.PhysicalReads -eq [int]$physical -and
                $state.LockedReads -eq [int](-not $physical)) 'Capture mode selected the wrong byte-read path'
        }
        if ($Case -eq 'identity-before') {
            Assert-ImageFixture ($state.Handles.Count -eq 0 -and $null -eq $state.Control) (
                'Initial identity refusal happened after a source handle was acquired'
            )
        }
        if ($Case -eq 'identity-after-lock') {
            Assert-ImageFixture ($state.Opens -eq 0) 'Post-lock identity refusal happened after source bytes opened'
        }
        if ($Case -eq 'identity-after-open') {
            Assert-ImageFixture (-not $state.Verified -and -not (Test-Path -LiteralPath $state.Destination)) (
                'Opened-source identity refusal happened after target bytes were written'
            )
        }
        foreach ($handle in $state.Handles) {
            Assert-ImageFixture ($handle.IsClosed -and $handle.DisposeCalls -eq 1) "Case $Case leaked or repeated source lock cleanup"
        }
        if ($null -ne $state.Control) {
            Assert-ImageFixture ($state.Control.IsClosed -and $state.Control.DisposeCalls -eq 1) "Case $Case leaked or repeated control-handle cleanup"
        }
        $expectedOrder = @()
        for ($index = $state.Handles.Count - 1; $index -ge 0; $index--) { $expectedOrder += $index }
        if ($null -ne $state.Control) { $expectedOrder += -1 }
        Assert-ImageFixture (([ImageFixtureResource]::ClosedOrder -join ',') -ceq
            ($expectedOrder -join ',')) "Case $Case released volume locks out of reverse acquisition order"
        foreach ($stream in $state.Streams) {
            Assert-ImageFixture (-not $stream.CanRead -and -not $stream.CanWrite) "Case $Case leaked source/output stream"
        }
        if ($Case -eq 'primary-and-cleanup') {
            Assert-ImageFixture ($failure.Contains('fixture dispose failure')) 'Cleanup failure hid or was hidden by primary failure'
        }
        Write-Host "PASS image-volume runtime $Case"
    } finally {
        foreach ($stream in $state.Streams) { $stream.Dispose() }
        foreach ($handle in $state.Handles) { $handle.ThrowOnDispose = $false; $handle.Dispose() }
        if ($null -ne $state.Control) { $state.Control.ThrowOnDispose = $false; $state.Control.Dispose() }
        $absolute = [IO.Path]::GetFullPath($root)
        Assert-ImageFixture ([IO.Path]::GetDirectoryName($absolute) -ceq $parent -and
            [IO.Path]::GetFileName($absolute) -cmatch '^image-runtime-[0-9a-f]{32}$') 'Unsafe runtime fixture cleanup root'
        Remove-Item -LiteralPath $absolute -Recurse -Force -ErrorAction Stop
    }
}

function Test-ImageGuardDrills([string]$Text, [bool]$PartitionRow) {
    $offlineRow = $Text -match '\$sakImageCaptureMode = ''OfflineDisk'''
    $keyFields = 'Select-Object PartitionNumber, Offset, Size, Guid, GptType, MbrType'
    $drills = @(
        @{ name = 'control-ownership-before-attributes'; from = '$sakImageResources.Add($sakImageDiskHandle)'; to = ''; case = 'attributes-query-before-failure'; failure = 'fixture native attributes query failed' },
        @{ name = 'stream-ownership-before-mapping'; from = '$sakImageResources.Add($in)'; to = ''; case = 'stream-mapping-failure'; failure = 'fixture device mapping failure' },
        @{ name = 'layout-recheck'; from = '$sakImageLayoutKey -cne $sakImageLockedLayoutKey'; to = '$sakImageLayoutKey -cne $sakImageLayoutKey'; case = 'layout-drift'; failure = 'layout changed' },
        @{ name = 'stable-key-offset'; from = $keyFields; to = 'Select-Object PartitionNumber, Size, Guid, GptType, MbrType'; case = 'offset-drift'; failure = 'layout changed' },
        @{ name = 'stable-key-size'; from = $keyFields; to = 'Select-Object PartitionNumber, Offset, Guid, GptType, MbrType'; case = 'size-drift'; failure = 'layout changed' },
        @{ name = 'stable-key-guid'; from = $keyFields; to = 'Select-Object PartitionNumber, Offset, Size, GptType, MbrType'; case = 'guid-drift'; failure = 'layout changed' },
        @{ name = 'stable-key-gpt-type'; from = $keyFields; to = 'Select-Object PartitionNumber, Offset, Size, Guid, MbrType'; case = 'gpt-type-drift'; failure = 'layout changed' },
        @{ name = 'stable-key-mbr-type'; from = $keyFields; to = 'Select-Object PartitionNumber, Offset, Size, Guid, GptType'; case = 'mbr-type-drift'; failure = 'layout changed' },
        @{ name = 'partial-acquisition-cleanup'; from = 'Close-SakImageSourceLocks $sakImageResources'; to = 'Close-SakImageSourceLocks @()'; case = 'attributes-query-before-failure'; failure = 'fixture native attributes query failed' },
        @{ name = 'primary-failure-propagation'; from = 'throw $sakImageFailure'; to = 'Write-Output $sakImageFailure'; case = 'copy-failure'; failure = 'Source ended before expected byte count' },
        @{ name = 'cleanup-continues-after-failure'; from = 'catch { Write-Output $_.Exception.Message }'; to = 'catch { throw }'; case = 'cleanup-failure'; failure = 'source lock cleanup failed' },
        @{ name = 'cleanup-error-retention'; from = "(`$sakImageCleanupErrors -join '; ')"; to = "''"; case = 'primary-and-cleanup'; failure = 'fixture primary failure' },
        @{ name = 'disk-binding'; from = '$part.DiskNumber -ne $sakImageSourceDisk'; to = '$part.DiskNumber -ne $part.DiskNumber'; case = 'wrong-disk'; failure = 'partition identity' },
        @{ name = 'partition-extent-bound'; from = '[uint64]$part.Size -gt ($sakImageSourceSize - [uint64]$part.Offset)'; to = '[uint64]$part.Size -gt [uint64]$part.Size'; case = 'past-end'; failure = 'geometry is invalid' },
        @{ name = 'cim-offline-type'; from = '$sakIdentityDisk.IsOffline -isnot [bool]'; to = '$sakIdentityDisk.IsOffline -isnot [bool] -and $false'; case = 'cim-offline-unknown'; failure = 'offline state is unknown' },
        @{ name = 'cim-mode-agreement'; from = '$sakIdentityDisk.IsOffline -ne $sakImageExpectedOffline'; to = '$sakIdentityDisk.IsOffline -ne $sakIdentityDisk.IsOffline'; case = 'cim-mode-mismatch'; failure = 'capture mode does not match' },
        @{ name = 'cim-boot-type'; from = '$sakIdentityDisk.IsBoot -isnot [bool]'; to = '$sakIdentityDisk.IsBoot -isnot [bool] -and $false'; case = 'cim-boot-unknown'; failure = 'system identity is unknown' },
        @{ name = 'cim-system-type'; from = '$sakIdentityDisk.IsSystem -isnot [bool]'; to = '$sakIdentityDisk.IsSystem -isnot [bool] -and $false'; case = 'cim-system-string'; failure = 'system identity is unknown' },
        @{ name = 'native-mode-agreement'; from = '$offline -ne $sakImageExpectedOffline'; to = '$offline -ne $offline'; case = 'native-mode-before-mismatch'; failure = 'native source offline state' },
        @{ name = 'native-final-mode-check'; from = 'Assert-SakImageAttributeMode $sakImageFinalAttributes'; to = ''; case = 'native-mode-after-mismatch'; failure = 'native source offline state' },
        @{ name = 'all-attributes-stable'; from = '$sakImageFinalAttributes -ne $sakImageOriginalAttributes'; to = '$sakImageFinalAttributes -ne $sakImageFinalAttributes'; case = 'attributes-other-flag-change'; failure = 'attributes changed during capture' },
        @{ name = 'control-final-mapping'; from = 'Assert-SakImageDevice $sakImageDiskHandle 0 $false'; to = ''; case = 'control-final-mapping-drift'; failure = 'fixture disk control mapping changed' },
        @{ name = 'final-layout-stable'; from = '(Get-SakImageLayoutKey $sakImageFinalLayout) -cne $sakImageLayoutKey'; to = '(Get-SakImageLayoutKey $sakImageFinalLayout) -cne (Get-SakImageLayoutKey $sakImageFinalLayout)'; case = 'final-layout-drift'; failure = 'partition identity changed during capture' },
        @{ name = 'postverify-before-input-disposal'; from = '$sakImageFinalAttributes = Get-SakImageDiskAttributes $sakImageDiskHandle'; to = '$in.Dispose(); $sakImageFinalAttributes = Get-SakImageDiskAttributes $sakImageDiskHandle'; case = 'success'; failure = '' }
    )
    if (-not $offlineRow) {
        $drills += @{ name = 'lock-ownership-before-mapping'; from = '$sakImageResources.Add($handle)'; to = ''; case = 'mapping-failure'; failure = 'fixture device mapping failure' }
        $drills += @{ name = 'stable-key-omits-mounted-volume'; from = $keyFields; to = ($keyFields+', VolumePath'); case = 'post-lock-volume-loss'; failure = '' }
        $drills += @{ name = 'reverse-release-order'; from = 'for ($index = $Resources.Count - 1; $index -ge 0; $index--)'; to = 'for ($index = 0; $index -lt $Resources.Count; $index++)'; case = 'success'; failure = '' }
    } else {
        $drills += @{ name = 'offline-boot-refusal'; from = '$sakIdentityDisk.IsBoot -or $sakIdentityDisk.IsSystem'; to = '$sakIdentityDisk.IsSystem'; case = 'offline-boot-source'; failure = 'refuses the current OS source disk' }
        $drills += @{ name = 'offline-system-refusal'; from = '$sakIdentityDisk.IsBoot -or $sakIdentityDisk.IsSystem'; to = '$sakIdentityDisk.IsBoot'; case = 'offline-system-source'; failure = 'refuses the current OS source disk' }
    }
    if ($PartitionRow) {
        if (-not $offlineRow) {
            $drills += @{ name = 'selected-volume-lock-required'; from = '$sakImageLocks.Count -ne 1'; to = '$sakImageLocks.Count -lt 0'; case = 'selected-msr-no-volume'; failure = 'source volume could not be locked' }
        } else {
            $drills += @{ name = 'offline-partition-physical-read'; from = '$sakImageSourcePartition -eq 0 -or $sakImageExpectedOffline'; to = '$sakImageSourcePartition -eq 0'; case = 'success'; failure = '' }
            $drills += @{ name = 'offline-partition-byte-offset'; from = '$in.Seek([int64]$sourceOffset,'; to = '$in.Seek([int64]0,'; case = 'success'; failure = '' }
        }
        if ((Get-ImageFunctionText $Text 'Assert-SakImageSourceIdentity').Contains('Partition identity mismatch (GUID)')) {
            $drills += @{ name = 'selected-guid-presence'; from = 'if (-not $p.Guid -or '; to = 'if ($p.Guid -and '; case = 'missing-guid'; failure = 'Partition identity mismatch (GUID)' }
        }
    } else {
        if (-not $offlineRow) {
            $drills += @{ name = 'all-volume-acquisition'; from = 'if (-not $volume.VolumePath) { continue }'; to = 'if ($volume.VolumePath) { continue }'; case = 'success'; failure = '' }
            $drills += @{ name = 'duplicate-volume-refusal'; from = '$volumePath -and -not $volumes.Add($volumePath)'; to = '$volumePath -and $volumes.Add($volumePath) -and $false'; case = 'duplicate-volume'; failure = 'volume identity repeats' }
        }
        $drills += @{ name = 'stable-key-partition-count'; from = $keyFields; to = ($keyFields+' | Select-Object -Last 1'); case = 'count-drift'; failure = 'layout changed' }
    }
    foreach ($drill in $drills) {
        Assert-ImageFixture ($Text.Contains($drill.from)) "Mutation anchor absent: $($drill.name)"
        $caught = ''
        try { Test-ImageRuntimeCase ($Text.Replace($drill.from, $drill.to)) $drill.case $drill.failure }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture (-not [string]::IsNullOrWhiteSpace($caught)) "Guard survived: $($drill.name)"
        Assert-ImageFixture (-not $caught.Contains('syntax errors') -and
            -not $caught.Contains('Mutation anchor')) "Guard drill failed infrastructure: $($drill.name)"
        Write-Host "KILLED image-volume $($drill.name)"
    }
    $phaseAnchor = "if (-not `$PSBoundParameters.ContainsKey('ResolveVolumes'))"
    Assert-ImageFixture ($Text.Contains($phaseAnchor)) 'Source-layout phase guard anchor missing'
    $caught = ''
    try { Test-ImageLayoutModeRequired ($Text.Replace($phaseAnchor, "if (`$PSBoundParameters.ContainsKey('ResolveVolumes') -and `$false)")) }
    catch { $caught = $_.Exception.Message }
    Assert-ImageFixture ($caught.Contains('Source layout accepted or prompted')) 'Source-layout explicit-phase guard survived'
    Write-Host 'KILLED image-volume explicit-layout-phase'
    $main = Get-ImageMainText $Text
    if (-not $offlineRow) {
        $heldCalls=[regex]::Matches($main,'(?m)^        Assert-SakImageDevice \$held.Handle \$held.PartitionNumber \$true\r?$')
        Assert-ImageFixture ($heldCalls.Count -eq 2) 'Expected pre-read and post-verification held-volume mapping checks'
        $heldCases=@('held-mapping-drift','held-mapping-final-drift')
        $heldFailures=@('fixture held device mapping changed','fixture final held device mapping changed')
        for($index=0;$index -lt $heldCalls.Count;$index++) {
            $changed=$main.Remove($heldCalls[$index].Index,$heldCalls[$index].Length)
            $caught=''
            try{Test-ImageRuntimeCase ($Text.Replace($main,$changed)) $heldCases[$index] $heldFailures[$index]}catch{$caught=$_.Exception.Message}
            Assert-ImageFixture (-not [string]::IsNullOrWhiteSpace($caught)) "Held-volume checkpoint survived: $index"
            Write-Host "KILLED image-volume $($heldCases[$index])"
        }
    }
    $calls = [regex]::Matches($main, '(?m)^    Assert-SakImageSourceIdentity\r?$')
    Assert-ImageFixture ($calls.Count -eq 4) 'Expected four source identity checkpoints'
    $identityCases = @('identity-before', 'identity-after-lock', 'identity-after-open', 'identity-after-verify')
    for ($index = 0; $index -lt $calls.Count; $index++) {
        $changed = $main.Remove($calls[$index].Index, $calls[$index].Length)
        $caught = ''
        try { Test-ImageRuntimeCase ($Text.Replace($main, $changed)) $identityCases[$index] 'Disk identity mismatch' }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture (-not [string]::IsNullOrWhiteSpace($caught)) "Identity checkpoint survived: $index"
        Write-Host "KILLED image-volume $($identityCases[$index])"
    }
    $native = Get-ImageNativeCommand $Text
    $fields = @('GenericRead', 'ShareReadWrite', 'OpenExisting', 'LockVolume',
        'AllowExtendedDASD', 'StorageDeviceNumber', 'GetDiskAttributes', 'DiskDeviceType')
    for ($index = 0; $index -lt $fields.Count; $index++) {
        $field = $fields[$index]
        $pattern = '(const\s+uint\s+' + $field + '\s*=\s*)[^;]+;'
        Assert-ImageFixture ([regex]::Matches($native, $pattern).Count -eq 1) "Native constant anchor absent: $field"
        $changed = [regex]::Replace($native, $pattern, '${1}0;')
        $namespace = 'SakImageFixtureDrill' + $index
        $changed = $changed.Replace('namespace Sak {', "namespace $namespace {")
        & ([scriptblock]::Create($changed)) | Out-Null
        $caught = ''
        try { Assert-ImageNativeContract (($namespace + '.ImageVolumeLock') -as [type]) }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture ($caught.Contains("constant mismatch: $field")) "Native constant guard survived: $field"
        Write-Host "KILLED image-volume native-$field"
    }
}

function Get-ImageManagedVerifyCommand([string]$Command, [string]$Namespace) {
    $pattern = '(?s)\[DllImport\("kernel32\.dll", SetLastError = true\)\]\s*' +
        '\[return: MarshalAs\(UnmanagedType.Bool\)\]\s*private static extern bool DeviceIoControl' +
        '\(SafeFileHandle handle, uint code,\s*IntPtr input, uint inputSize, out DeviceNumber output, ' +
        'uint outputSize,\s*out uint returned, IntPtr overlapped\);'
    Assert-ImageFixture ([regex]::Matches($Command, $pattern).Count -eq 1) (
        'Expected one device-number native boundary for managed verification controls'
    )
    $replacement = @'
    public static uint ProbeType = 7, ProbeDisk = 7, ProbePartition = 2, ProbeReturned = 12;
    public static bool ProbeSuccess = true;
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern void SetLastError(uint value);
    private static bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, out DeviceNumber output, uint outputSize,
        out uint returned, IntPtr overlapped) {
        if (code != 0x002D1080 || outputSize != 12) throw new InvalidOperationException("Native query contract mismatch");
        output = new DeviceNumber { Type = ProbeType, Disk = ProbeDisk, Partition = ProbePartition };
        returned = ProbeReturned;
        if (!ProbeSuccess) SetLastError(6);
        return ProbeSuccess;
    }
'@
    return [regex]::Replace($Command, $pattern, $replacement).Replace('namespace Sak {',
        "namespace $Namespace {")
}

function Test-ImageManagedVerifyCases([string]$Command, [string]$Namespace) {
    # Compile the real Verify body. Only its P/Invoke query boundary is replaced with
    # controlled managed output; no disk or volume is opened by these identity controls.
    & ([scriptblock]::Create((Get-ImageManagedVerifyCommand $Command $Namespace))) | Out-Null
    $type = ($Namespace + '.ImageVolumeLock') -as [type]
    Assert-ImageFixture ($null -ne $type) 'Managed native-boundary type was not compiled'
    $cases = @(
        @{ name = 'matching-device'; field = ''; value = 0; check = $true; failure = '' },
        @{ name = 'short-return'; field = 'ProbeReturned'; value = 8; check = $true; failure = 'identity mismatch' },
        @{ name = 'wrong-type'; field = 'ProbeType'; value = 8; check = $true; failure = 'identity mismatch' },
        @{ name = 'wrong-disk'; field = 'ProbeDisk'; value = 8; check = $true; failure = 'identity mismatch' },
        @{ name = 'wrong-partition'; field = 'ProbePartition'; value = 3; check = $true; failure = 'identity mismatch' },
        @{ name = 'whole-disk-partition-independent'; field = 'ProbePartition'; value = 3; check = $false; failure = '' },
        @{ name = 'ioctl-failure'; field = 'ProbeSuccess'; value = $false; check = $true; failure = 'cannot identify' }
    )
    $handle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr]::Zero, $false)
    try {
        foreach ($case in $cases) {
            foreach ($pair in @{ ProbeType = 7; ProbeDisk = 7; ProbePartition = 2; ProbeReturned = 12 }.GetEnumerator()) {
                $type.GetField($pair.Key).SetValue($null, [uint32]$pair.Value)
            }
            $type.GetField('ProbeSuccess').SetValue($null, $true)
            if ($case.field) {
                $value = if ($case.field -ceq 'ProbeSuccess') { [bool]$case.value } else { [uint32]$case.value }
                $type.GetField($case.field).SetValue($null, $value)
            }
            $failure = $null
            try { $type.GetMethod('Verify').Invoke($null, @($handle, [uint32]7, [uint32]2, [bool]$case.check)) }
            catch {
                $failure = $_.Exception
                while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
            }
            if ($case.failure) {
                Assert-ImageFixture ($null -ne $failure -and $failure.Message.Contains($case.failure)) (
                    "Native Verify rejected incorrectly or accepted $($case.name)"
                )
            } else {
                Assert-ImageFixture ($null -eq $failure) "Native Verify rejected $($case.name)"
            }
            if ($case.name -ceq 'ioctl-failure') {
                Assert-ImageFixture ($failure -is [ComponentModel.Win32Exception] -and
                    $failure.NativeErrorCode -eq 6) 'Native Verify lost the original Windows error code'
            }
        }
    } finally { $handle.Dispose() }
    Write-Host 'PASS actual native Verify body: seven managed-boundary controls'
}

function Test-ImageManagedVerifyDrills([string]$Command) {
    $drills = @(
        @{ name = 'returned-size'; from = 'returned != required'; to = 'returned != ProbeReturned'; case = 'short-return' },
        @{ name = 'device-type'; from = 'actual.Type != DiskDeviceType'; to = 'actual.Type != ProbeType'; case = 'wrong-type' },
        @{ name = 'disk-number'; from = 'actual.Disk != disk'; to = 'actual.Disk != ProbeDisk'; case = 'wrong-disk' },
        @{ name = 'partition-number'; from = 'actual.Partition != partition'; to = 'actual.Partition != ProbePartition'; case = 'wrong-partition' },
        @{ name = 'partition-check-selection'; from = 'checkPartition && actual.Partition'; to = 'actual.Partition'; case = 'whole-disk-partition-independent' }
    )
    $failureGate = [regex]::Match($Command, '(?s)if \(!DeviceIoControl\(handle, StorageDeviceNumber,.*?IntPtr.Zero\)\)')
    Assert-ImageFixture ($failureGate.Success) 'Native IOCTL failure guard anchor missing'
    $drills += @{ name = 'ioctl-failure'; from = $failureGate.Value;
        to = $failureGate.Value.Replace('))', ') && ProbeSuccess)'); case = 'ioctl-failure' }
    for ($index = 0; $index -lt $drills.Count; $index++) {
        $drill = $drills[$index]
        Assert-ImageFixture ($Command.Contains($drill.from)) "Native Verify anchor absent: $($drill.name)"
        $caught = ''
        try { Test-ImageManagedVerifyCases ($Command.Replace($drill.from, $drill.to)) ('SakImageVerifyDrill' + $index) }
        catch { $caught = $_.Exception.Message }
        Assert-ImageFixture ($caught.Contains('Native Verify rejected') -and
            $caught.Contains($drill.case)) "Native Verify guard survived or failed wrong control: $($drill.name)"
        Write-Host "KILLED image-volume native-verify-$($drill.name)"
    }
}

function Get-ImageManagedExtendedCommand([string]$Command, [string]$Namespace) {
    $pattern = '(?s)\[DllImport\("kernel32\.dll", SetLastError = true\)\]\s*' +
        '\[return: MarshalAs\(UnmanagedType.Bool\)\]\s*private static extern bool DeviceIoControl' +
        '\(SafeFileHandle handle, uint code,\s*IntPtr input, uint inputSize, IntPtr output, ' +
        'uint outputSize,\s*out uint returned, IntPtr overlapped\);'
    Assert-ImageFixture ([regex]::Matches($Command, $pattern).Count -eq 1) (
        'Expected one extended-read native boundary for managed controls'
    )
    $replacement = @'
    public static bool ProbeSuccess = true;
    public static int ProbeCalls = 0;
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern void SetLastError(uint value);
    private static bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped) {
        ++ProbeCalls;
        if (code != 0x00090083 || input != IntPtr.Zero || inputSize != 0 ||
            output != IntPtr.Zero || outputSize != 0 || overlapped != IntPtr.Zero) {
            throw new InvalidOperationException("Extended-read IOCTL argument contract mismatch");
        }
        returned = 0;
        if (!ProbeSuccess) SetLastError(6);
        return ProbeSuccess;
    }
'@
    return [regex]::Replace($Command, $pattern, $replacement).Replace('namespace Sak {',
        "namespace $Namespace {")
}

function Test-ImageManagedExtendedCases([string]$Command, [string]$Text, [string]$Namespace) {
    # The host-native DASD probe was not run: its tool invocation was rejected by policy.
    # These controls replace DeviceIoControl itself with managed output. The actual C#
    # failure check and PowerShell wrapper still execute, but no volume/device API is called.
    & ([scriptblock]::Create((Get-ImageManagedExtendedCommand $Command $Namespace))) | Out-Null
    $type = ($Namespace + '.ImageVolumeLock') -as [type]
    Assert-ImageFixture ($null -ne $type) 'Managed extended-read type was not compiled'
    $handle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new([IntPtr]::Zero, $false)
    try {
        $type.GetMethod('AllowExtendedRead').Invoke($null, @($handle))
        Assert-ImageFixture ($type.GetField('ProbeCalls').GetValue($null) -eq 1) (
            'Extended-read success did not issue the required managed IOCTL boundary'
        )
        $type.GetField('ProbeSuccess').SetValue($null, $false)
        $failure = $null
        try { $type.GetMethod('AllowExtendedRead').Invoke($null, @($handle)) }
        catch {
            $failure = $_.Exception
            while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
        }
        Assert-ImageFixture ($failure -is [ComponentModel.Win32Exception] -and
            $failure.NativeErrorCode -eq 6) 'Extended-read native failure guard lost refusal or error code'
        $wrapper = (Get-ImageFunctionText $Text 'Open-SakImageLockedRead').Replace(
            '[Sak.ImageVolumeLock]', ('[' + $Namespace + '.ImageVolumeLock]'))
        . ([scriptblock]::Create($wrapper))
        $failure = $null
        $stream = $null
        try { $stream = Open-SakImageLockedRead $handle }
        catch {
            $failure = $_.Exception
            while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
        } finally { if ($null -ne $stream) { $stream.Dispose() } }
        Assert-ImageFixture ($failure -is [ComponentModel.Win32Exception] -and
            $failure.NativeErrorCode -eq 6 -and
            $type.GetField('ProbeCalls').GetValue($null) -eq 3) (
            'Locked-read wrapper omitted extended-read refusal before FileStream construction'
        )
    } finally { $handle.Dispose() }
    Write-Host 'PASS actual AllowExtendedRead body and locked-read wrapper: three managed-boundary controls; native DASD NOT RUN'
}

function Test-ImageManagedExtendedDrills([string]$Command, [string]$Text) {
    $gate = [regex]::Match($Command, '(?s)if \(!DeviceIoControl\(handle, AllowExtendedDASD,.*?IntPtr.Zero\)\)')
    Assert-ImageFixture ($gate.Success) 'Extended-read failure guard anchor missing'
    $changed = $Command.Replace($gate.Value, $gate.Value.Replace('))', ') && ProbeSuccess)'))
    $caught = ''
    try { Test-ImageManagedExtendedCases $changed $Text 'SakExtendedGuardDrill' }
    catch { $caught = $_.Exception.Message }
    Assert-ImageFixture ($caught.Contains('Extended-read native failure guard')) 'Extended-read failure guard survived'
    Write-Host 'KILLED image-volume native-extended-read-failure'
    $call = '[Sak.ImageVolumeLock]::AllowExtendedRead($Handle)'
    Assert-ImageFixture ($Text.Contains($call)) 'Locked-read extended-read call anchor missing'
    $caught = ''
    try { Test-ImageManagedExtendedCases $Command ($Text.Replace($call, '')) 'SakExtendedWrapperDrill' }
    catch { $caught = $_.Exception.Message }
    Assert-ImageFixture ($caught.Contains('Locked-read wrapper omitted extended-read refusal')) 'Locked-read extended-read call survived'
    Write-Host 'KILLED image-volume locked-read-extended-call'
}

function Get-ImageManagedControlCommand([string]$Command, [string]$Namespace) {
    $createPattern = '(?s)\[DllImport\("kernel32\.dll", CharSet = CharSet.Unicode, SetLastError = true\)\]\s*' +
        'private static extern SafeFileHandle CreateFile\(string path, uint access, uint share,\s*' +
        'IntPtr security, uint disposition, uint flags, IntPtr template\);'
    $devicePattern = '(?s)\[DllImport\("kernel32\.dll", SetLastError = true\)\]\s*' +
        '\[return: MarshalAs\(UnmanagedType.Bool\)\]\s*private static extern bool DeviceIoControl' +
        '\(SafeFileHandle handle, uint code,\s*IntPtr input, uint inputSize, out DeviceNumber output, ' +
        'uint outputSize,\s*out uint returned, IntPtr overlapped\);'
    $attributePattern = $devicePattern.Replace('out DeviceNumber output', 'out DiskAttributes output')
    foreach ($pattern in @($createPattern,$devicePattern,$attributePattern)) {
        Assert-ImageFixture ([regex]::Matches($Command,$pattern).Count -eq 1) 'Control native-boundary signature changed'
    }
    $create = @'
    public static bool ProbeOpenInvalid = false, ProbeAttributesSuccess = true;
    public static uint ProbeDisk = 7, ProbeReturned = 16, ProbeVersion = 16;
    public static ulong ProbeFlags = 0x8000000000000003UL;
    public static int ProbeVerifyCalls = 0;
    public static SafeFileHandle ProbeHandle;
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern void SetLastError(uint value);
    private static SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template) {
        if (path != "fixture-disk" || access != 0 || share != 3 || disposition != 3 ||
            security != IntPtr.Zero || flags != 0 || template != IntPtr.Zero) {
            throw new InvalidOperationException("Control open must remain metadata-only with exact sharing");
        }
        ProbeHandle = new SafeFileHandle(ProbeOpenInvalid ? IntPtr.Zero : new IntPtr(12345), false);
        if (ProbeOpenInvalid) SetLastError(6);
        return ProbeHandle;
    }
'@
    $device = @'
    private static bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, out DeviceNumber output, uint outputSize,
        out uint returned, IntPtr overlapped) {
        ++ProbeVerifyCalls;
        if (handle != ProbeHandle || handle.IsInvalid || code != 0x002D1080 || outputSize != 12) {
            throw new InvalidOperationException("Control native identity query contract mismatch");
        }
        output = new DeviceNumber { Type = 7, Disk = ProbeDisk, Partition = 0 };
        returned = 12;
        return true;
    }
'@
    $attributes = @'
    private static bool DeviceIoControl(SafeFileHandle handle, uint code,
        IntPtr input, uint inputSize, out DiskAttributes output, uint outputSize,
        out uint returned, IntPtr overlapped) {
        if (handle != ProbeHandle || handle.IsClosed || code != 0x000700F0 || outputSize != 16 ||
            input != IntPtr.Zero || inputSize != 0 || overlapped != IntPtr.Zero) {
            throw new InvalidOperationException("Control native attributes query contract mismatch");
        }
        output = new DiskAttributes { Version = ProbeVersion, Reserved = 0, Flags = ProbeFlags };
        returned = ProbeReturned;
        if (!ProbeAttributesSuccess) SetLastError(6);
        return ProbeAttributesSuccess;
    }
'@
    $changed = [regex]::Replace($Command,$createPattern,$create)
    $changed = [regex]::Replace($changed,$devicePattern,$device)
    return [regex]::Replace($changed,$attributePattern,$attributes).Replace('namespace Sak {',"namespace $Namespace {")
}

function Test-ImageManagedControlCases([string]$Command,[string]$Namespace) {
    & ([scriptblock]::Create((Get-ImageManagedControlCommand $Command $Namespace))) | Out-Null
    $type = ($Namespace+'.ImageVolumeLock') -as [type]
    $open = $type.GetMethod('OpenDiskControl'); $read = $type.GetMethod('ReadDiskAttributes')
    Assert-ImageFixture ($null -ne $open -and $null -ne $read -and $read.ReturnType -eq [uint64]) 'Control native API contract changed'
    $handle=$null
    try {
        try { $handle=$open.Invoke($null,@('fixture-disk',[uint32]7)) }
        catch { throw 'Control positive metadata-only open failed' }
        Assert-ImageFixture ($null -ne $handle -and -not $handle.IsClosed -and
            $type.GetField('ProbeVerifyCalls').GetValue($null) -eq 1) 'Control open did not verify its native disk mapping'
        $expectedFlags=[uint64]::Parse('9223372036854775811')
        $value=$read.Invoke($null,@($handle))
        Assert-ImageFixture ($value -is [uint64] -and $value -eq $expectedFlags) 'Control attributes truncated the original flag bits'
        foreach ($case in @(@{field='ProbeReturned';value=[uint32]8;name='short-reply'},
            @{field='ProbeReturned';value=[uint32]20;name='long-reply'},
            @{field='ProbeVersion';value=[uint32]0;name='wrong-version'},
            @{field='ProbeAttributesSuccess';value=$false;name='query-failure'})) {
            $type.GetField('ProbeReturned').SetValue($null,[uint32]16)
            $type.GetField('ProbeVersion').SetValue($null,[uint32]16)
            $type.GetField('ProbeAttributesSuccess').SetValue($null,$true)
            $type.GetField($case.field).SetValue($null,$case.value)
            $failure=$null
            try { $read.Invoke($null,@($handle))|Out-Null }
            catch { $failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException} }
            if ($case.name -ceq 'query-failure') {
                Assert-ImageFixture ($failure -is [ComponentModel.Win32Exception] -and $failure.NativeErrorCode -eq 6) 'Control query-failure guard lost the Windows error'
            } else {
                Assert-ImageFixture ($failure -is [InvalidOperationException] -and $failure.Message.Contains('attributes reply is invalid')) (
                    "Control $($case.name) guard accepted a malformed reply"
                )
            }
        }
        $handle.Dispose();$handle=$null
        foreach ($case in @('invalid-open','wrong-disk')) {
            $type.GetField('ProbeOpenInvalid').SetValue($null,($case -ceq 'invalid-open'))
            $type.GetField('ProbeDisk').SetValue($null,[uint32]8)
            $type.GetField('ProbeVerifyCalls').SetValue($null,0)
            $failure=$null;$unexpected=$null
            try {$unexpected=$open.Invoke($null,@('fixture-disk',[uint32]7))}
            catch {$failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}}
            finally {if($null -ne $unexpected){$unexpected.Dispose()}}
            if($case -ceq 'invalid-open') {
                Assert-ImageFixture ($failure -is [ComponentModel.Win32Exception] -and $failure.NativeErrorCode -eq 6 -and
                    $failure.Message.Contains('cannot open its source disk identity handle') -and
                    $type.GetField('ProbeVerifyCalls').GetValue($null) -eq 0) 'Control invalid-open guard did not refuse before native querying'
            } else {
                Assert-ImageFixture ($failure -is [InvalidOperationException] -and $failure.Message.Contains('device identity mismatch')) (
                    'Control wrong-disk open bypassed native mapping verification'
                )
            }
            Assert-ImageFixture ($type.GetField('ProbeHandle').GetValue($null).IsClosed) "Control $case path leaked its metadata handle"
        }
    } finally {
        if($null -ne $handle){$handle.Dispose()}
        $last=$type.GetField('ProbeHandle').GetValue($null);if($null -ne $last){$last.Dispose()}
    }
    Write-Host 'PASS actual disk-control APIs: metadata-only open, mapping, cleanup and strict 64-bit attribute replies'
}

function Test-ImageManagedControlDrills([string]$Command) {
    $method=[regex]::Match($Command,'(?s)public static SafeFileHandle OpenDiskControl\(.*?\r?\n    \}').Value
    Assert-ImageFixture ($method.Length -gt 0) 'Native control method mutation scope missing'
    $gate=[regex]::Match($Command,'(?s)if \(!DeviceIoControl\(handle, GetDiskAttributes,.*?IntPtr.Zero\)\)').Value
    Assert-ImageFixture ($gate.Length -gt 0) 'Native attributes query mutation scope missing'
    $drills=@(
        @{name='metadata-only-access';from='CreateFile(path, 0, ShareReadWrite';to='CreateFile(path, GenericRead, ShareReadWrite';failure='Control positive metadata-only open failed'},
        @{name='reply-size';from='returned != required || actual.Version';to='returned != ProbeReturned || actual.Version';failure='Control short-reply'},
        @{name='reply-version';from='actual.Version != required';to='actual.Version != ProbeVersion';failure='Control wrong-version'},
        @{name='all-flag-bits';from='return actual.Flags;';to='return (uint)actual.Flags;';failure='Control attributes truncated'},
        @{name='query-error';from=$gate;to=$gate.Replace('))',') && ProbeAttributesSuccess)');failure='Control query-failure'},
        @{name='mapping-before-return';from=$method;to=$method.Replace('Verify(handle, disk, 0, false);','');failure='Control open did not verify'},
        @{name='invalid-open';from=$method;to=$method.Replace('if (handle.IsInvalid)','if (handle.IsInvalid && !handle.IsInvalid)');failure='Control invalid-open'},
        @{name='partial-open-cleanup';from=$method;to=$method.Replace('handle.Dispose(); throw;','throw;');failure='Control invalid-open path leaked'}
    )
    for($index=0;$index -lt $drills.Count;$index++) {
        $drill=$drills[$index];Assert-ImageFixture ($Command.Contains($drill.from)) "Control native mutation anchor missing: $($drill.name)"
        $caught=''
        try{Test-ImageManagedControlCases ($Command.Replace($drill.from,$drill.to)) ('SakControlDrill'+$index)}catch{$caught=$_.Exception.Message}
        Assert-ImageFixture ($caught.Contains($drill.failure)) "Control native guard survived or failed wrong assertion: $($drill.name)"
        Write-Host "KILLED image-volume native-control-$($drill.name)"
    }
}

$production = Get-Content -LiteralPath $ScriptPath -Raw -ErrorAction Stop
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$evidenceRoot = Join-Path $projectRoot ('build/test_results/partition_image_volume_lock/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $evidenceRoot -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $ScriptPath -Destination (Join-Path $evidenceRoot 'create-image.ps1') -ErrorAction Stop
Test-ImageLayoutDiagnostic $production
if ($RunGuardDrills) { Test-ImageLayoutDiagnosticDrills $production }
Test-ImageLayoutModeRequired $production
Test-ImageLocatorContract $production
Test-ImagePartitionCountContract $production
if ($RunGuardDrills) { Test-ImageLocatorDrills $production }
$nativeCommand = Get-ImageNativeCommand $production
$comparisonCommand = Get-ImageNativeCommand $production 'CloneByteComparer'
& ([scriptblock]::Create($comparisonCommand)) | Out-Null
Test-ImageComparerContract ('Sak.CloneByteComparer' -as [type])
Test-ImageVerificationFunctions $production
if ($RunGuardDrills) { Test-ImageComparerDrills $comparisonCommand $production }
& ([scriptblock]::Create($nativeCommand)) | Out-Null
$nativeType = 'Sak.ImageVolumeLock' -as [type]
Assert-ImageNativeContract $nativeType
Test-ImageNativeOrdinaryFileRefusal $nativeType
Write-Host 'PASS actual emitted native image-lock type and Windows API contract'
Test-ImageManagedVerifyCases $nativeCommand 'SakImageFixtureVerify'
if ($RunGuardDrills) { Test-ImageManagedVerifyDrills $nativeCommand }
Test-ImageManagedExtendedCases $nativeCommand $production 'SakImageExtendedFixture'
if ($RunGuardDrills) { Test-ImageManagedExtendedDrills $nativeCommand $production }
Test-ImageManagedControlCases $nativeCommand 'SakImageControlFixture'
if ($RunGuardDrills) { Test-ImageManagedControlDrills $nativeCommand }
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public sealed class ImageFixtureResource : IDisposable {
    public static readonly List<int> ClosedOrder = new List<int>();
    public bool IsClosed { get; private set; }
    public int DisposeCalls { get; private set; }
    public bool ThrowOnDispose { get; set; }
    public int OrderToken { get; set; }
    public int PartitionNumber { get; set; }
    public int VerifyCalls { get; set; }
    public bool IsControl { get; set; }
    public void Dispose() {
        ++DisposeCalls;
        IsClosed = true;
        ClosedOrder.Add(OrderToken);
        if (ThrowOnDispose) throw new InvalidOperationException("fixture dispose failure");
    }
}
'@
$cases = [ordered]@{
    success = ''; 'identity-before' = 'Disk identity mismatch';
    'identity-after-lock' = 'Disk identity mismatch'; 'identity-after-open' = 'Disk identity mismatch';
    'identity-after-verify' = 'Disk identity mismatch';
    'wrong-disk' = 'partition identity'; 'duplicate-number' = 'absent or ambiguous';
    'negative-offset' = 'geometry is invalid'; 'zero-size' = 'geometry is invalid';
    'past-end' = 'geometry is invalid'; 'layout-drift' = 'layout changed';
    'offset-drift' = 'layout changed'; 'size-drift' = 'layout changed'; 'guid-drift' = 'layout changed';
    'gpt-type-drift' = 'layout changed'; 'mbr-type-drift' = 'layout changed';
    'count-missing' = 'partition count is unknown'; 'count-string' = 'partition count is unknown';
    'count-float' = 'partition count is unknown'; 'count-negative' = 'partition count is unknown';
    'count-mismatch' = 'partition count is unknown'; 'empty-enumeration-mismatch' = 'partition count is unknown';
    'cim-offline-unknown' = 'offline state is unknown'; 'cim-offline-string' = 'offline state is unknown';
    'cim-mode-mismatch' = 'capture mode does not match'; 'cim-boot-unknown' = 'system identity is unknown';
    'cim-system-string' = 'system identity is unknown'; 'cim-offline-final-change' = 'capture mode does not match';
    'control-open-failure' = 'fixture disk control open failed';
    'attributes-query-before-failure' = 'fixture native attributes query failed';
    'attributes-query-after-failure' = 'fixture native attributes query failed';
    'native-mode-before-mismatch' = 'native source offline state'; 'native-mode-after-mismatch' = 'native source offline state';
    'attributes-other-flag-change' = 'attributes changed during capture'; 'attributes-static-readonly' = '';
    'control-final-mapping-drift' = 'fixture disk control mapping changed';
    'final-layout-drift' = 'partition identity changed during capture';
    'stream-mapping-failure' = 'fixture device mapping failure';
    'copy-failure' = 'Source ended before expected byte count'; 'verify-failure' = 'Full verification mismatch';
    'verify-late-failure' = 'Full verification mismatch at byte 257';
    'cleanup-failure' = 'source lock cleanup failed'; 'primary-and-cleanup' = 'fixture primary failure'
}
$partitionRow = $production -match '\$sakImageSourcePartition = \[uint32\]2'
$offlineRow = $production -match '\$sakImageCaptureMode = ''OfflineDisk'''
if ($offlineRow) {
    $cases['offline-boot-source'] = 'refuses the current OS source disk'
    $cases['offline-system-source'] = 'refuses the current OS source disk'
    $cases['no-volume'] = ''
} else {
    $cases['post-lock-volume-loss'] = ''
    $cases['busy-volume-zero-getvolume'] = 'fixture source volume busy'
    $cases['held-mapping-drift'] = 'fixture held device mapping changed'
    $cases['held-mapping-final-drift'] = 'fixture final held device mapping changed'
    $cases['first-lock-failure'] = 'fixture lock failure'
    $cases['mapping-failure'] = 'fixture device mapping failure'
    $cases['malformed-volume'] = 'cannot resolve an exclusive'
    $cases['ambiguous-volume'] = 'volume identity is ambiguous'
    $cases['no-volume'] = 'cannot resolve an exclusive'
}
if ($partitionRow) {
    $cases['missing-partition'] = 'absent or ambiguous'
    $cases['count-drift'] = 'absent or ambiguous'
    $guidPinned = (Get-ImageFunctionText $production 'Assert-SakImageSourceIdentity').Contains('Partition identity mismatch (GUID)')
    $cases['missing-guid'] = if ($guidPinned) { 'Partition identity mismatch (GUID)' } else { '' }
    if (-not $offlineRow) { $cases['selected-msr-no-volume'] = 'source volume could not be locked' }
} else {
    $cases['count-drift'] = 'layout changed'
    $cases['missing-number'] = 'partition identity'
    $cases['duplicate-number'] = 'partition identity'
    $cases['raw-empty'] = ''
    $cases['raw-empty-count-missing'] = 'partition count is unknown'
    $cases['msr-only'] = ''
    if (-not $offlineRow) {
        $cases['duplicate-volume'] = 'volume identity repeats'
        $cases['second-lock-failure'] = 'fixture lock failure'
    }
}
foreach ($case in $cases.GetEnumerator()) { Test-ImageRuntimeCase $production $case.Key $case.Value }
if ($RunGuardDrills) { Test-ImageGuardDrills $production $partitionRow }
Write-Host 'CREATE_IMAGE_SOURCE_LOCK_FIXTURE_PASSED'
