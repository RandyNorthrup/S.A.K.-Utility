<#
.SYNOPSIS
    Stages and runs the AI Assistant live E2E smoke from the VirtualBox VM.

.DESCRIPTION
    Run this from the Windows VM desktop that has the repo mounted as
    \\vboxsvr\sakrepo. It stages the smoke runner locally, runs package-only
    app/client checks against the shared Release package, and writes evidence
    back to artifacts\ai-assistant-vm-smoke in the shared repo.

    This is intentionally non-destructive: no elevation, no disk changes, no app
    installs, no service edits, and no cleanup outside the smoke artifact folder.
#>

[CmdletBinding()]
param(
    [string]$SharedRoot = "\\vboxsvr\sakrepo",
    [string]$StageRoot = "$env:PUBLIC\sak-ai-assistant-vm-smoke",
    [string]$OpenAIKeyFile = "temp\openaikey.md",
    [string]$PackageRoot = "build\Release",
    [int]$StartupTimeoutSeconds = 60,
    [switch]$SkipAccessibilityAudit
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $SharedRoot -PathType Container)) {
    throw "Shared repo root not found: $SharedRoot"
}

$sharedRunner = Join-Path $SharedRoot "scripts\run_ai_assistant_vm_smoke.ps1"
if (-not (Test-Path -LiteralPath $sharedRunner -PathType Leaf)) {
    throw "AI smoke runner not found: $sharedRunner"
}

$keyPath = if ([System.IO.Path]::IsPathRooted($OpenAIKeyFile)) {
    $OpenAIKeyFile
}
else {
    Join-Path $SharedRoot $OpenAIKeyFile
}
if (-not (Test-Path -LiteralPath $keyPath -PathType Leaf)) {
    throw "OpenAI key file not found. Expected path: $keyPath"
}

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
$localRunner = Join-Path $StageRoot "run_ai_assistant_vm_smoke.ps1"
Copy-Item -LiteralPath $sharedRunner -Destination $localRunner -Force

$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$localRunner`"",
    "-ProjectRoot", "`"$SharedRoot`"",
    "-PackageRoot", "`"$PackageRoot`"",
    "-OpenAIKeyFile", "`"$keyPath`"",
    "-StartupTimeoutSeconds", $StartupTimeoutSeconds,
    "-SkipBuild",
    "-SkipCTest",
    "-AllowLiveModelSmoke"
)
if ($SkipAccessibilityAudit) {
    $argumentList += "-SkipAccessibilityAudit"
}

$logRoot = Join-Path $SharedRoot "artifacts\ai-assistant-vm-smoke"
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$launcherLog = Join-Path $logRoot ("launch-ai-assistant-vm-smoke-{0}.log" -f (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss"))

$process = Start-Process `
    -FilePath "powershell.exe" `
    -ArgumentList $argumentList `
    -WorkingDirectory $StageRoot `
    -RedirectStandardOutput "$launcherLog.out.txt" `
    -RedirectStandardError "$launcherLog.err.txt" `
    -WindowStyle Hidden `
    -Wait `
    -PassThru

Get-Content -LiteralPath "$launcherLog.out.txt", "$launcherLog.err.txt" -ErrorAction SilentlyContinue |
    Set-Content -LiteralPath $launcherLog -Encoding UTF8
Remove-Item -LiteralPath "$launcherLog.out.txt", "$launcherLog.err.txt" -Force -ErrorAction SilentlyContinue

if ($process.ExitCode -ne 0) {
    throw "AI Assistant VM smoke exited with code $($process.ExitCode). Log: $launcherLog"
}

Write-Host "AI Assistant VM smoke completed. Log: $launcherLog"
