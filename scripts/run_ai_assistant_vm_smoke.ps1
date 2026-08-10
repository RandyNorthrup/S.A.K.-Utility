<#
.SYNOPSIS
    Runs non-destructive AI Assistant production smoke checks in a VM or local lab.

.DESCRIPTION
    Builds the app and AI test targets, runs the AI CTest slice, launches the
    portable app startup smoke, runs the accessibility audit, captures the AI
    manual checklist output, and writes JSON/Markdown evidence under
    artifacts\ai-assistant-vm-smoke.

    The script is safe for the existing Partition Manager VM method: it does not
    modify disks, partitions, services, packages, browser state, or user data.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = (Resolve-Path -LiteralPath ".").Path,
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$PackageRoot = "",
    [string]$OutputRoot = "artifacts\ai-assistant-vm-smoke",
    [string]$OpenAIKeyFile = "",
    [int]$StartupTimeoutSeconds = 45,
    [switch]$SkipBuild,
    [switch]$SkipCTest,
    [switch]$SkipAccessibilityAudit,
    [switch]$AllowLiveModelSmoke
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path $Root $Path)
}

function Resolve-FileSystemProviderPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not [string]::IsNullOrWhiteSpace($resolved.ProviderPath)) {
        return $resolved.ProviderPath
    }
    return $resolved.Path
}

function New-StepResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Status,
        [string]$Command = "",
        [string]$OutputPath = "",
        [int]$ExitCode = 0,
        [string]$ErrorText = ""
    )
    [ordered]@{
        name = $Name
        status = $Status
        command = $Command
        exit_code = $ExitCode
        output_path = $OutputPath
        error = $ErrorText
    }
}

function Read-OpenAIKeyFromFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )
    $resolved = Resolve-FileSystemProviderPath -Path $Path
    $raw = Get-Content -LiteralPath $resolved -Raw -ErrorAction Stop
    foreach ($line in ($raw -split "`r?`n")) {
        $candidate = $line.Trim()
        if ($candidate -match "^OPENAI_API_KEY\s*=") {
            $candidate = ($candidate -replace "^OPENAI_API_KEY\s*=\s*", "").Trim().Trim('"').Trim("'")
        }
        if ($candidate -match "^sk-") {
            return $candidate
        }
    }
    throw "OpenAI key file did not contain a usable key line."
}

function Test-PathQuiet {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Microsoft.PowerShell.Commands.TestPathType]$PathType = [Microsoft.PowerShell.Commands.TestPathType]::Any
    )
    try {
        return [bool](Test-Path -LiteralPath $Path -PathType $PathType -ErrorAction Stop)
    }
    catch {
        return $false
    }
}

function Get-GitHeadQuiet {
    param(
        [Parameter(Mandatory = $true)][string]$Root
    )
    try {
        $git = Get-Command git -ErrorAction Stop
        $head = & $git.Source -C $Root rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0) {
            $short = ($head | Select-Object -First 1)
            # Record whether the working tree is dirty so a report cannot claim a clean
            # source identity while uncommitted changes were what was actually exercised.
            $porcelain = & $git.Source -C $Root status --porcelain 2>$null
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace(($porcelain -join "`n"))) {
                return "$short-dirty"
            }
            return $short
        }
    }
    catch {
    }
    return ""
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [hashtable]$Environment = @{},
        [int]$TimeoutSeconds = 0
    )

    $commandLine = @($FilePath) + $Arguments
    $commandText = $commandLine -join " "
    "== $Name ==" | Set-Content -LiteralPath $LogPath -Encoding UTF8
    "Command: $commandText" | Add-Content -LiteralPath $LogPath -Encoding UTF8
    "WorkingDirectory: $WorkingDirectory" | Add-Content -LiteralPath $LogPath -Encoding UTF8
    "" | Add-Content -LiteralPath $LogPath -Encoding UTF8

    $stdout = "$LogPath.stdout.txt"
    $stderr = "$LogPath.stderr.txt"
    $previousEnvironment = @{}
    try {
        foreach ($entry in $Environment.GetEnumerator()) {
            $previousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, "Process")
            if ($null -eq $entry.Value) {
                [Environment]::SetEnvironmentVariable($entry.Key, $null, "Process")
            }
            else {
                [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value, "Process")
            }
        }

        $startArgs = @{
            FilePath = $FilePath
            WorkingDirectory = $WorkingDirectory
            RedirectStandardOutput = $stdout
            RedirectStandardError = $stderr
            WindowStyle = "Hidden"
            PassThru = $true
        }
        if ($Arguments.Count -gt 0) {
            $startArgs.ArgumentList = $Arguments
        }
        $process = Start-Process @startArgs

        # Honor the timeout: -Wait would block indefinitely, so wait explicitly and
        # terminate a child that overruns rather than hanging the gate and report forever.
        $timedOut = $false
        if ($TimeoutSeconds -gt 0) {
            if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                $timedOut = $true
                try { $process.Kill($true) } catch { try { $process.Kill() } catch { } }
                $process.WaitForExit(5000) | Out-Null
            }
        }
        else {
            $process.WaitForExit()
        }

        $process.Refresh()
        $outText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw } else { "" }
        $errText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw } else { "" }
        "STDOUT:`n$outText" | Add-Content -LiteralPath $LogPath -Encoding UTF8
        "STDERR:`n$errText" | Add-Content -LiteralPath $LogPath -Encoding UTF8
        if ($timedOut) {
            "TIMEOUT: command exceeded $TimeoutSeconds s and was terminated" | Add-Content -LiteralPath $LogPath -Encoding UTF8
            return New-StepResult -Name $Name -Status "failed" -Command $commandText -OutputPath $LogPath -ExitCode 124 -ErrorText "Command timed out after $TimeoutSeconds seconds"
        }
        $exitCode = if ($null -eq $process.ExitCode) { 1 } else { [int]$process.ExitCode }
        if ($exitCode -ne 0) {
            return New-StepResult -Name $Name -Status "failed" -Command $commandText -OutputPath $LogPath -ExitCode $exitCode -ErrorText "Command exited with code $exitCode"
        }
        return New-StepResult -Name $Name -Status "passed" -Command $commandText -OutputPath $LogPath -ExitCode $exitCode
    }
    finally {
        foreach ($entry in $previousEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
        }
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

function Write-MarkdownReport {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Report
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# AI Assistant VM Smoke Report")
    $lines.Add("")
    $lines.Add("- Status: $($Report.status)")
    $lines.Add("- Started: $($Report.started_at_utc)")
    $lines.Add("- Finished: $($Report.finished_at_utc)")
    $lines.Add("- Project root: $($Report.project_root)")
    $lines.Add("- Build dir: $($Report.build_dir)")
    $lines.Add("- Package root: $($Report.package_root)")
    $lines.Add("- Computer: $($Report.environment.computer_name)")
    $lines.Add("- User: $($Report.environment.user_name)")
    $lines.Add("- VM shared root detected: $($Report.environment.vm_shared_root_detected)")
    $lines.Add("")
    $lines.Add("## Steps")
    foreach ($step in $Report.steps) {
        $lines.Add("")
        $lines.Add("### $($step.name)")
        $lines.Add("")
        $lines.Add("- Status: $($step.status)")
        $lines.Add("- Exit code: $($step.exit_code)")
        if (-not [string]::IsNullOrWhiteSpace($step.output_path)) {
            $lines.Add("- Output: $($step.output_path)")
        }
        if (-not [string]::IsNullOrWhiteSpace($step.error)) {
            $lines.Add("- Error: $($step.error)")
        }
    }
    $lines.Add("")
    $lines.Add("## Live Model")
    $lines.Add("")
    $lines.Add("- Requested: $([bool]$Report.live_model_smoke_requested)")
    $lines.Add("- Key source: $($Report.live_model_key_source)")
    $lines.Add("- Note: live checks use the key only in child-process environment and do not write it to the report.")
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

$projectRootPath = Resolve-FileSystemProviderPath -Path $ProjectRoot
$buildRootPath = Resolve-RepoPath -Root $projectRootPath -Path $BuildDir
if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $buildRootPath $Configuration
}
$packageRootPath = Resolve-RepoPath -Root $projectRootPath -Path $PackageRoot
$outputRootPath = Resolve-RepoPath -Root $projectRootPath -Path $OutputRoot
$runStamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$runRoot = Join-Path $outputRootPath "run-$runStamp"
$logRoot = Join-Path $runRoot "logs"
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null

$steps = [System.Collections.Generic.List[object]]::new()
$started = (Get-Date).ToUniversalTime().ToString("o")
$openAiKey = ""
$openAiKeySource = "none"
if ($AllowLiveModelSmoke) {
    if (-not [string]::IsNullOrWhiteSpace($OpenAIKeyFile)) {
        $keyPath = Resolve-RepoPath -Root $projectRootPath -Path $OpenAIKeyFile
        $openAiKey = Read-OpenAIKeyFromFile -Path $keyPath
        $openAiKeySource = "key_file"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($env:OPENAI_API_KEY)) {
        $openAiKeySource = "environment"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($env:SAK_OPENAI_API_KEY)) {
        $openAiKeySource = "environment"
    }
    else {
        throw "AllowLiveModelSmoke requires OPENAI_API_KEY, SAK_OPENAI_API_KEY, saved app credential, or -OpenAIKeyFile."
    }
}

Push-Location $projectRootPath
try {
    $testTargets = @(
        "sak_utility",
        "test_openai_responses_client",
        "test_ai_prompt_assembler",
        "test_ai_workflow_store",
        "test_ai_workflow_evals",
        "test_ai_provider_registry",
        "test_ai_tool_policy",
        "test_ai_command_guard",
        "test_ai_tool_call_router",
        "test_ai_run_state_store",
        "test_ai_trace_store",
        "test_ai_transcript_view"
    )

    if ($SkipBuild) {
        $steps.Add((New-StepResult -Name "build_ai_targets" -Status "skipped" -Command "cmake --build $BuildDir --config $Configuration --target $($testTargets -join ' ')" -OutputPath ""))
    }
    else {
        $buildArgs = @("--build", $buildRootPath, "--config", $Configuration, "--target") + $testTargets
        $steps.Add((Invoke-LoggedCommand `
            -Name "build_ai_targets" `
            -FilePath "cmake" `
            -Arguments $buildArgs `
            -WorkingDirectory $projectRootPath `
            -LogPath (Join-Path $logRoot "build_ai_targets.log")))
    }

    $ctestEnv = @{}
    if ($AllowLiveModelSmoke) {
        $ctestEnv["SAK_RUN_OPENAI_LIVE_TESTS"] = "1"
        $ctestEnv["SAK_AI_REAL_MODEL_TEST"] = "1"
        if (-not [string]::IsNullOrWhiteSpace($openAiKey)) {
            $ctestEnv["OPENAI_API_KEY"] = $openAiKey
        }
    }
    else {
        # Fail closed: without the -AllowLiveModelSmoke opt-in, ensure an inherited ambient
        # opt-in variable cannot leak live network/API execution into the child while the
        # report records live_model_smoke_requested=false. Setting these to $null clears
        # them for the child (see Invoke-LoggedCommand's environment handling).
        $ctestEnv["SAK_RUN_OPENAI_LIVE_TESTS"] = $null
        $ctestEnv["SAK_AI_REAL_MODEL_TEST"] = $null
    }
    if ($SkipCTest) {
        $steps.Add((New-StepResult -Name "ai_ctest_slice" -Status "skipped"))
    }
    else {
        $ctestRegex = "^(test_openai_responses_client|test_ai_prompt_assembler|test_ai_workflow_store|test_ai_workflow_evals|test_ai_provider_registry|test_ai_tool_policy|test_ai_command_guard|test_ai_tool_call_router|test_ai_run_state_store|test_ai_trace_store|test_ai_transcript_view)$"
        $steps.Add((Invoke-LoggedCommand `
            -Name "ai_ctest_slice" `
            -FilePath "ctest" `
            -Arguments @("--test-dir", $buildRootPath, "-C", $Configuration, "-R", $ctestRegex, "--no-tests=error", "--output-on-failure") `
            -WorkingDirectory $projectRootPath `
            -Environment $ctestEnv `
            -LogPath (Join-Path $logRoot "ai_ctest_slice.log")))
    }

    if ($SkipCTest -and $AllowLiveModelSmoke) {
        $testExe = Join-Path $packageRootPath "test_openai_responses_client.exe"
        if (-not (Test-Path -LiteralPath $testExe -PathType Leaf)) {
            $steps.Add((New-StepResult -Name "live_client_executable" -Status "failed" -OutputPath $testExe -ExitCode 1 -ErrorText "test_openai_responses_client.exe not found in package root"))
        }
        else {
            $steps.Add((Invoke-LoggedCommand `
                -Name "live_client_executable" `
                -FilePath $testExe `
                -Arguments @() `
                -WorkingDirectory $packageRootPath `
                -Environment $ctestEnv `
                -LogPath (Join-Path $logRoot "live_client_executable.log") `
                -TimeoutSeconds 360))
        }
    }
    elseif ($SkipCTest) {
        $steps.Add((New-StepResult -Name "live_client_executable" -Status "skipped"))
    }

    $steps.Add((Invoke-LoggedCommand `
        -Name "portable_startup_smoke" `
        -FilePath "powershell.exe" `
        -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $projectRootPath "scripts\run_portable_e2e_smoke.ps1"), "-PackageRoot", $packageRootPath, "-TimeoutSeconds", [string]$StartupTimeoutSeconds) `
        -WorkingDirectory $projectRootPath `
        -LogPath (Join-Path $logRoot "portable_startup_smoke.log") `
        -TimeoutSeconds ($StartupTimeoutSeconds + 20)))

    if ($SkipAccessibilityAudit) {
        $steps.Add((New-StepResult -Name "accessibility_audit" -Status "skipped"))
    }
    else {
        $auditOut = Join-Path $runRoot "accessibility-audit-status.txt"
        $exe = Join-Path $packageRootPath "sak_utility.exe"
        $auditStep = Invoke-LoggedCommand `
            -Name "accessibility_audit" `
            -FilePath $exe `
            -Arguments @("--accessibility-audit", "--accessibility-audit-output=$auditOut", "--no-splash") `
            -WorkingDirectory $packageRootPath `
            -LogPath (Join-Path $logRoot "accessibility_audit.log") `
            -TimeoutSeconds 90
        if ($auditStep.status -eq "passed") {
            # Exit 0 is necessary but not sufficient: confirm the audit actually wrote its
            # status artifact and reported OK (SAK_ACCESSIBILITY_AUDIT_OK). Fail closed if
            # the artifact is missing, empty, or does not report a clean pass.
            $auditOk = $false
            if (Test-PathQuiet -Path $auditOut -PathType Leaf) {
                $firstLine = (Get-Content -LiteralPath $auditOut -TotalCount 1 -ErrorAction SilentlyContinue | Select-Object -First 1)
                if (-not [string]::IsNullOrWhiteSpace($firstLine) -and $firstLine.StartsWith("SAK_ACCESSIBILITY_AUDIT_OK")) {
                    $auditOk = $true
                }
            }
            if (-not $auditOk) {
                $auditStep.status = "failed"
                $auditStep.error = "Accessibility audit status artifact missing or not OK: $auditOut"
                $auditStep.output_path = $auditOut
            }
        }
        $steps.Add($auditStep)
    }

    $checklistStep = Invoke-LoggedCommand `
        -Name "manual_smoke_checklist" `
        -FilePath "powershell.exe" `
        -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $projectRootPath "scripts\ai_smoke_checklist.ps1")) `
        -WorkingDirectory $projectRootPath `
        -LogPath (Join-Path $logRoot "manual_smoke_checklist.log")
    if ($checklistStep.status -eq "passed") {
        # This step only PRINTS the manual checklist; a human still has to perform and sign
        # off the gates. Reporting a certifying "passed" from process exit alone would
        # overstate. Record it as informational (non-failing, but not a pass) instead.
        $checklistStep.status = "informational"
        $checklistStep.error = "Checklist printed for manual follow-up; not an automated pass."
    }
    $steps.Add($checklistStep)
}
finally {
    Pop-Location
}

# Fail closed: only an explicit "passed", "skipped" (operator opt-out) or "informational"
# (a printed, human-owned step) counts as non-failing. Any other value -- "failed", or a
# status a future code path forgot to set -- flips the overall gate to failed.
$failed = @($steps | Where-Object { $_.status -ne "passed" -and $_.status -ne "skipped" -and $_.status -ne "informational" })
$status = if ($failed.Count -eq 0) { "passed" } else { "failed" }
$report = [ordered]@{
    schema = "sak.ai.vm_smoke.v1"
    status = $status
    started_at_utc = $started
    finished_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    project_root = $projectRootPath
    build_dir = $buildRootPath
    package_root = $packageRootPath
    live_model_smoke_requested = [bool]$AllowLiveModelSmoke
    live_model_key_source = $openAiKeySource
    environment = [ordered]@{
        computer_name = $env:COMPUTERNAME
        user_name = $env:USERNAME
        os = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Caption)
        powershell = $PSVersionTable.PSVersion.ToString()
        vm_shared_root_detected = (Test-PathQuiet -Path "\\vboxsvr\sakrepo" -PathType Container)
        vbox_guest_service = (Get-Service -Name VBoxService -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Status)
        git_head = (Get-GitHeadQuiet -Root $projectRootPath)
    }
    steps = @($steps)
}

$jsonPath = Join-Path $runRoot "ai-assistant-vm-smoke-report.json"
$markdownPath = Join-Path $runRoot "ai-assistant-vm-smoke-report.md"
$latestJson = Join-Path $outputRootPath "latest-ai-assistant-vm-smoke-report.json"
$latestMarkdown = Join-Path $outputRootPath "latest-ai-assistant-vm-smoke-report.md"

$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
Write-MarkdownReport -Path $markdownPath -Report $report
Copy-Item -LiteralPath $jsonPath -Destination $latestJson -Force
Copy-Item -LiteralPath $markdownPath -Destination $latestMarkdown -Force

Write-Host "AI Assistant VM smoke $status"
Write-Host "Report: $jsonPath"
if ($status -ne "passed") {
    $failedNames = ($failed | ForEach-Object { $_.name }) -join ", "
    throw "AI Assistant VM smoke failed: $failedNames"
}
