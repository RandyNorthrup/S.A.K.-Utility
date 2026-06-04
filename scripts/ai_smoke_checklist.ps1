<#
.SYNOPSIS
Prints the AI Assistant production smoke checklist and optionally runs automated tests.

.PARAMETER RunAutomated
Run configure, build, AI tests, and full CTest suite.

.PARAMETER RunLiveOpenAI
Runs the opt-in live OpenAI smoke test. Requires OPENAI_API_KEY or saved SAK credential.

.PARAMETER OpenAIKeyFile
Optional local file containing an OpenAI key for the live smoke. The key is read
only into the child-process environment and is never printed.
#>

param(
    [switch]$RunAutomated,
    [switch]$RunLiveOpenAI,
    [string]$OpenAIKeyFile = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $RepoRoot

Write-Host "AI Assistant smoke checklist"
Write-Host "Docs: docs/AI_ASSISTANT_PRODUCTION_SMOKE_TESTS.md"
Write-Host ""
Write-Host "Manual gates to verify:"
Write-Host "  - Ambiguous workflow input"
Write-Host "  - Command approval"
Write-Host "  - Restore-point offer"
Write-Host "  - Restore-point failure continue"
Write-Host "  - Workflow recovery gate"
Write-Host "  - Adversarial prompt injection / ambiguous mutation gate"
Write-Host "  - Result-bubble copy icon copies one redacted bubble"
Write-Host "  - Enter submits; Ctrl+Enter inserts newline"
Write-Host "  - New Chat clears old chat and shows AI Session (new) until first-prompt auto-rename"
Write-Host "  - Header shows Agents active/completed; composer shows exact Ctx x/y beside Send"
Write-Host "  - Session role is plain text; workflow, first prompt, and explicit role-switch all update it"
Write-Host ""
Write-Host "Harmless tool smoke:"
Write-Host "  - Package manager search for Firefox, no install"
Write-Host "  - Offline installer search/download for Firefox, no install"
Write-Host "  - Live Responses API plain response, exact input-token count, and function tool-loop, when key is available"
Write-Host ""

if ($RunAutomated) {
    cmake -S . -B build
    cmake --build build --config Release --target sak_utility
    ctest --test-dir build -C Release -R "test_ai_|test_openai_responses_client" --output-on-failure
    ctest --test-dir build -C Release --output-on-failure
}

if ($RunLiveOpenAI) {
    $previousOpenAIKey = [Environment]::GetEnvironmentVariable("OPENAI_API_KEY", "Process")
    $env:SAK_RUN_OPENAI_LIVE_TESTS = "1"
    try {
        if (-not [string]::IsNullOrWhiteSpace($OpenAIKeyFile)) {
            $raw = Get-Content -LiteralPath $OpenAIKeyFile -Raw
            $candidate = ($raw -split "`r?`n" |
                ForEach-Object { $_.Trim() } |
                Where-Object { $_ -match "^sk-" -or $_ -match "^OPENAI_API_KEY\s*=" } |
                Select-Object -First 1)
            if ($candidate -match "^OPENAI_API_KEY\s*=") {
                $candidate = ($candidate -replace "^OPENAI_API_KEY\s*=\s*", "").Trim().Trim('"').Trim("'")
            }
            if ([string]::IsNullOrWhiteSpace($candidate) -or $candidate -notmatch "^sk-") {
                throw "OpenAI key file did not contain a usable key line."
            }
            [Environment]::SetEnvironmentVariable("OPENAI_API_KEY", $candidate, "Process")
        }
        ctest --test-dir build -C Release -R "^test_openai_responses_client$" --output-on-failure
    } finally {
        [Environment]::SetEnvironmentVariable("OPENAI_API_KEY", $previousOpenAIKey, "Process")
        Remove-Item Env:\SAK_RUN_OPENAI_LIVE_TESTS -ErrorAction SilentlyContinue
    }
}
