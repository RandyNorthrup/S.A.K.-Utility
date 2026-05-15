<#
.SYNOPSIS
Prints the AI Assistant production smoke checklist and optionally runs automated tests.

.PARAMETER RunAutomated
Run configure, build, AI tests, and full CTest suite.

.PARAMETER RunLiveOpenAI
Runs the opt-in live OpenAI smoke test. Requires OPENAI_API_KEY or saved SAK credential.
#>

param(
    [switch]$RunAutomated,
    [switch]$RunLiveOpenAI
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
Write-Host ""
Write-Host "Harmless tool smoke:"
Write-Host "  - Package manager search for Firefox, no install"
Write-Host "  - Offline installer search/download for Firefox, no install"
Write-Host ""

if ($RunAutomated) {
    cmake -S . -B build
    cmake --build build --config Release --target sak_utility
    ctest --test-dir build -C Release -R "test_ai_|test_openai_responses_client" --output-on-failure
    ctest --test-dir build -C Release --output-on-failure
}

if ($RunLiveOpenAI) {
    $env:SAK_RUN_OPENAI_LIVE_TESTS = "1"
    try {
        ctest --test-dir build -C Release -R "^test_openai_responses_client$" --output-on-failure
    } finally {
        Remove-Item Env:\SAK_RUN_OPENAI_LIVE_TESTS -ErrorAction SilentlyContinue
    }
}
