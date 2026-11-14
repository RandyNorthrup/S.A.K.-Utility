<#
.SYNOPSIS
    Parses PowerShell scripts and fails on syntax errors, on one variable written under two casings,
    or on $LASTEXITCODE read after a native command's pipeline was cut short.

.DESCRIPTION
    Static only: it parses each script and runs nothing, so it catches broken scripts quickly in
    pre-commit without executing installer, bundle, signing, or release logic.

    PowerShell variable names ignore case, so $Chains and $chains are one variable. The final
    certification run once kept the chains script's path in $Chains and then wrote its plan to
    $chains, and launched the plan's first entry as a script (2026-11-09). This check fails when one
    scope writes a variable -- by assignment, as a foreach variable, or as a parameter -- under two
    spellings that differ only in case. A scope is the script (with $script: writes from anywhere, and
    the script-level writes of every script it dot-sources), $global:, or one function or method.

    It also fails when a pipeline that starts with a native command is cut short by Select-Object
    -First or -Index and its scope then reads $LASTEXITCODE before any other native command runs:
    PowerShell 7 does not record that exit code (see Find-TruncatedNativeExitCode).
#>

# PositionalBinding=$false + ValueFromRemainingArguments. With a plain [string[]]$Files this
# script received ONLY the first path pre-commit handed it and silently ignored the rest --
# proven here directly: a file with `function Broken( {` in second position exited 0 with
# "PowerShell syntax check passed." See the note in scripts/run_cppcheck.ps1 for the
# measurement behind this.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Set-Location $ProjectRoot

function Test-IsVariableWrite {
    # True when this variable is written here: assigned (also typed, or in a list), a foreach
    # variable, or a parameter.
    param([System.Management.Automation.Language.VariableExpressionAst]$Variable)
    $node = $Variable
    $parent = $Variable.Parent
    if ($parent -is [System.Management.Automation.Language.ConvertExpressionAst] -and $parent.Child -eq $node) {
        $node = $parent
        $parent = $parent.Parent
    }
    if ($parent -is [System.Management.Automation.Language.ArrayLiteralAst]) {
        $node = $parent
        $parent = $parent.Parent
    }
    if ($parent -is [System.Management.Automation.Language.AssignmentStatementAst]) { return ($parent.Left -eq $node) }
    if ($parent -is [System.Management.Automation.Language.ForEachStatementAst]) { return ($parent.Variable -eq $Variable) }
    if ($parent -is [System.Management.Automation.Language.ParameterAst]) { return ($parent.Name -eq $Variable) }
    return $false
}

function Get-VariableWriteScope {
    # The scope a write lands in: 'script', 'global', or the function or method around it.
    param([System.Management.Automation.Language.VariableExpressionAst]$Variable)
    $path = $Variable.VariablePath
    if ($path.IsScript) { return 'script' }
    if ($path.IsGlobal) { return 'global' }
    $node = $Variable.Parent
    while ($null -ne $node) {
        if ($node -is [System.Management.Automation.Language.FunctionDefinitionAst]) {
            return "function $($node.Name) (line $($node.Extent.StartLineNumber))"
        }
        if ($node -is [System.Management.Automation.Language.FunctionMemberAst]) {
            return "method $($node.Name) (line $($node.Extent.StartLineNumber))"
        }
        $node = $node.Parent
    }
    return 'script'
}

function Get-DotSourcedScript {
    # The scripts this one dot-sources by a literal name: they share its script scope.
    param([System.Management.Automation.Language.Ast]$Ast, [string]$Path)
    $folder = Split-Path -Parent $Path
    $commands = $Ast.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.CommandAst] -and
            $node.InvocationOperator -eq [System.Management.Automation.Language.TokenKind]::Dot
        }, $true)
    foreach ($command in $commands) {
        $match = [regex]::Match($command.CommandElements[0].Extent.Text, '((?:[\w.-]+[\\/])*[\w.-]+\.ps1)')
        if (-not $match.Success) { continue }
        # "$PSScriptRoot\x.ps1" matches as PSScriptRoot\x.ps1: try the name with fewer leading folders too.
        $segments = @($match.Groups[1].Value -split '[\\/]')
        $found = $null
        for ($skip = 0; $skip -lt $segments.Count -and -not $found; $skip++) {
            $relative = $segments[$skip..($segments.Count - 1)] -join '\'
            foreach ($base in @($folder, $ProjectRoot)) {
                $candidate = Join-Path $base $relative
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $found = (Resolve-Path -LiteralPath $candidate).Path
                    break
                }
            }
        }
        if ($found) { $found }
    }
}

function Get-VariableWrite {
    # Every variable write in a script, with its scope; with -ScriptScopeOnly, only those in the
    # script scope (what a dot-sourcing script shares). Follows dot-sourced scripts once each.
    param([string]$Path, [hashtable]$Seen, [switch]$ScriptScopeOnly)
    if ($Seen.ContainsKey($Path)) { return }
    $Seen[$Path] = $true
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    $variables = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)
    foreach ($variable in $variables) {
        $variablePath = $variable.VariablePath
        $scoped = $variablePath.IsScript -or $variablePath.IsGlobal -or $variablePath.IsLocal -or $variablePath.IsPrivate
        # env:, function: and other drives are not variables of a scope; $null discards.
        if ($variablePath.IsDriveQualified -and -not $scoped) { continue }
        if (-not (Test-IsVariableWrite -Variable $variable)) { continue }
        $name = $variablePath.UserPath -replace '^(?i:script|global|local|private):', ''
        if ($name -eq 'null') { continue }
        $scope = Get-VariableWriteScope -Variable $variable
        if ($ScriptScopeOnly -and $scope -ne 'script') { continue }
        [pscustomobject]@{ Scope = $scope; Name = $name; Line = $variable.Extent.StartLineNumber; Path = $Path }
    }
    foreach ($included in @(Get-DotSourcedScript -Ast $ast -Path $Path)) {
        Get-VariableWrite -Path $included -Seen $Seen -ScriptScopeOnly
    }
}

function Find-VariableCaseCollision {
    # One finding per scope and name that is written under more than one spelling.
    param([string]$Path)
    $writes = @(Get-VariableWrite -Path $Path -Seen @{})
    $groups = $writes | Group-Object -Property { "$($_.Scope)|$($_.Name.ToLowerInvariant())" }
    foreach ($group in $groups) {
        $spellings = @($group.Group | Group-Object -Property Name -CaseSensitive | Sort-Object -Property { $_.Group[0].Line })
        if ($spellings.Count -lt 2) { continue }
        $scope = $group.Group[0].Scope
        $owner = if ($scope -eq 'script' -or $scope -eq 'global') { "The $scope scope" } else { $scope.Substring(0, 1).ToUpperInvariant() + $scope.Substring(1) }
        $places = foreach ($spelling in $spellings) {
            $first = $spelling.Group[0]
            $where = if ($first.Path -eq $Path) { "line $($first.Line)" } else { "$(Split-Path -Leaf $first.Path) line $($first.Line)" }
            "`$$($spelling.Name) ($where)"
        }
        [pscustomobject]@{
            File = $Path
            Line = $spellings[0].Group[0].Line
            Column = 0
            Message = "$owner writes one variable as $($places -join ' and '): PowerShell names ignore case, so they are the same variable"
        }
    }
}

# PowerShell 7 does not record the exit code of a native command whose pipeline Select-Object
# -First (or -Index) stops early: $LASTEXITCODE keeps its previous value, $null in a new session.
# bundle_smartmontools.ps1 read it after `& smartctl --version | ... | Select-Object -First 1`,
# so its check of a correctly pinned smartctl.exe failed on every fresh CI session and the script
# re-bundled into the certified checkout (2026-11-10). Windows PowerShell 5.1 sets it, which is
# why the same check passed on a workstation. Capture the output first, then read the exit code.
$NativeExtensionPattern = '(?i)\.(exe|com|cmd|bat)$'
$KnownAliases = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($alias in Get-Alias) { [void]$KnownAliases.Add($alias.Name) }

function Test-IsNativeCommand {
    # A command this check cannot rule out as native: called with & or . (a path or a variable),
    # named with an executable extension, or a bare word that is neither Verb-Noun nor an alias.
    param([System.Management.Automation.Language.CommandAst]$Command)
    $nameElement = $Command.CommandElements[0]
    if ($Command.InvocationOperator -ne [System.Management.Automation.Language.TokenKind]::Unknown) {
        return -not ($nameElement -is [System.Management.Automation.Language.ScriptBlockExpressionAst])
    }
    $name = $Command.GetCommandName()
    if ([string]::IsNullOrEmpty($name)) { return $false }
    if ($name -match $NativeExtensionPattern) { return $true }
    if ($name.Contains('-') -or $KnownAliases.Contains($name)) { return $false }
    return $name -match '^[\w.]+$'
}

function Test-StopsPipelineEarly {
    # Select-Object (or its alias select) with -First or -Index ends the upstream commands early.
    param([System.Management.Automation.Language.CommandAst]$Command)
    $name = $Command.GetCommandName()
    if ($name -notin @('Select-Object', 'select')) { return $false }
    foreach ($element in $Command.CommandElements) {
        if ($element -is [System.Management.Automation.Language.CommandParameterAst] -and
            $element.ParameterName -match '^(?i)(f|fi|fir|firs|first|i|in|ind|inde|index)$') {
            return $true
        }
    }
    return $false
}

function Get-EnclosingScope {
    param([System.Management.Automation.Language.Ast]$Node)
    $current = $Node.Parent
    while ($null -ne $current) {
        if ($current -is [System.Management.Automation.Language.ScriptBlockAst]) { return $current }
        $current = $current.Parent
    }
    return $null
}

function Find-TruncatedNativeExitCode {
    # One finding per pipeline that starts with a native command, is cut short by Select-Object
    # -First/-Index, and whose scope next reads $LASTEXITCODE before any other native command runs.
    param([string]$Path)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    $commands = @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true))
    $exitReads = @($ast.FindAll({
                param($node)
                $node -is [System.Management.Automation.Language.VariableExpressionAst] -and
                ($node.VariablePath.UserPath -replace '^(?i:global|script|local):', '') -eq 'LASTEXITCODE' -and
                -not (Test-IsVariableWrite -Variable $node)
            }, $true))
    $pipelines = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.PipelineAst] }, $true)
    foreach ($pipeline in $pipelines) {
        $elements = @($pipeline.PipelineElements)
        if ($elements.Count -lt 2 -or -not ($elements[0] -is [System.Management.Automation.Language.CommandAst])) { continue }
        if (-not (Test-IsNativeCommand -Command $elements[0])) { continue }
        $stopper = $elements | Select-Object -Skip 1 | Where-Object {
            $_ -is [System.Management.Automation.Language.CommandAst] -and (Test-StopsPipelineEarly -Command $_)
        } | Select-Object -First 1
        if ($null -eq $stopper) { continue }
        $scope = Get-EnclosingScope -Node $pipeline
        $end = $pipeline.Extent.EndOffset
        $nextRead = $exitReads | Where-Object {
            $_.Extent.StartOffset -ge $end -and (Get-EnclosingScope -Node $_) -eq $scope
        } | Sort-Object { $_.Extent.StartOffset } | Select-Object -First 1
        if ($null -eq $nextRead) { continue }
        $nativeBetween = $commands | Where-Object {
            $_.Extent.StartOffset -ge $end -and $_.Extent.StartOffset -lt $nextRead.Extent.StartOffset -and
            (Get-EnclosingScope -Node $_) -eq $scope -and (Test-IsNativeCommand -Command $_)
        } | Select-Object -First 1
        if ($null -ne $nativeBetween) { continue }
        [pscustomobject]@{
            File = $Path
            Line = $pipeline.Extent.StartLineNumber
            Column = $pipeline.Extent.StartColumnNumber
            Message = "A native command's pipeline is cut short by '$($stopper.Extent.Text)', then line " +
                "$($nextRead.Extent.StartLineNumber) reads `$LASTEXITCODE: PowerShell 7 does not record that exit " +
                "code. Capture the output first (`$out = @(& tool ...)`), read `$LASTEXITCODE, then select."
        }
    }
}

if (-not $Files -or $Files.Count -eq 0) {
    $Files = git ls-files "*.ps1"
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed (exit $LASTEXITCODE); cannot enumerate PowerShell scripts."
    }
    if (-not $Files -or @($Files).Count -eq 0) {
        throw "git ls-files returned no .ps1 files; refusing to pass without scanning anything."
    }
}

$failures = @()
foreach ($file in $Files) {
    if (-not $file.EndsWith(".ps1", [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        continue
    }

    $tokens = $null
    $errors = $null
    $fullPath = (Resolve-Path -LiteralPath $file).Path
    [System.Management.Automation.Language.Parser]::ParseFile(
        $fullPath,
        [ref]$tokens,
        [ref]$errors
    ) | Out-Null

    foreach ($errorRecord in $errors) {
        $failures += [pscustomobject]@{
            File = $file
            Line = $errorRecord.Extent.StartLineNumber
            Column = $errorRecord.Extent.StartColumnNumber
            Message = $errorRecord.Message
        }
    }
    if ($errors.Count -eq 0) {
        $failures += @(Find-VariableCaseCollision -Path $fullPath | ForEach-Object { $_.File = $file; $_ })
        $failures += @(Find-TruncatedNativeExitCode -Path $fullPath | ForEach-Object { $_.File = $file; $_ })
    }
}

if ($failures.Count -gt 0) {
    Write-Host "PowerShell syntax check failed:" -ForegroundColor Red
    $failures | Format-Table -AutoSize -Wrap | Out-String -Width 4096 | Write-Host
    exit 1
}

Write-Host ("PowerShell syntax check passed: $(@($Files).Count) script(s) parse, no variable is written " +
    "under two casings, and no exit code is read after a native command's pipeline is cut short.") -ForegroundColor Green
