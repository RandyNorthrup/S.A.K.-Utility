# AI Assistant Production Smoke Tests

Run before release candidates with `SAK_ENABLE_AI_ASSISTANT` enabled.

For broader hands-on QA before release smoke, use
`docs/AI_ASSISTANT_MANUAL_QA_RUNBOOK.md`.

## Automated Baseline

```powershell
cmake -S . -B build
cmake --build build --config Release --target sak_utility
ctest --test-dir build -C Release -R "test_ai_|test_openai_responses_client" --output-on-failure
ctest --test-dir build -C Release --output-on-failure
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"
powershell -ExecutionPolicy Bypass -File scripts/stage_portable_release.ps1 -BuildDir build\Release -PackageName $packageName
powershell -ExecutionPolicy Bypass -File scripts/create_release_archive.ps1 -BuildDir build\Release -PackageName $packageName
$extract = "build\Release\clean-readiness-extract"
Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $extract | Out-Null
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract -Force
powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 -PackageRoot $extract
```

## Live Model Smoke

Set either `OPENAI_API_KEY` or save a key through the AI panel.

```powershell
$env:SAK_RUN_OPENAI_LIVE_TESTS = "1"
ctest --test-dir build -C Release -R "^test_openai_responses_client$" --output-on-failure
Remove-Item Env:\SAK_RUN_OPENAI_LIVE_TESTS -ErrorAction SilentlyContinue
```

Expected:

- Test skips when no key exists.
- With a key, response contains the sentinel text and token usage.
- Failures are redacted.

## Harmless SAK Tool Smoke

Use Assisted Full Access.

- Ask: `search the package manager for Firefox but do not install anything`
- Expected: SAK package manager search runs before any web fallback.
- Ask: `find an offline installer for Firefox but do not install it`
- Expected: SAK offline downloader search/download path is attempted before web fallback.
- Confirm generated artifacts appear under the current chat artifact folder.

## Live Workflow Progress Smoke

Start any multi-phase workflow, such as `Full PC Health Check`.

Expected:

- The chat header shows an animated activity label and a workflow progress bar
  immediately after launch.
- Run Details opens automatically.
- The first phase appears as `running` before it completes.
- The global status bar shows the current phase and non-zero active
  agent/tool counts while work is in flight.
- `activity.jsonl` contains `workflow_phase` events with `running` before the
  matching `completed`, `failed`, `waiting_for_human`, or `skipped` event.
- Pressing Stop changes the status to cancelling/cancelled and clears active
  agent/tool counts once the worker returns.

## Durable Gate Resume Smoke

For each case:

1. Start the action until the gate is visible.
2. Close SAK.
3. Reopen SAK and the same chat session.
4. Confirm `Resume Waiting Run` is visible.
5. Resume, answer the gate, and confirm Run Details keeps the same run id.

Cases:

- Missing or ambiguous workflow input.
- Command approval in Assisted Full Access.
- Restore-point offer for a risky change.
- Restore-point failure continue decision.
- Workflow recovery gate after a risky/mutating phase failure.

Expected:

- `run_state.json` restores the pending gate.
- `human_gates.jsonl` contains waiting and resolved records.
- `activity.jsonl` shows waiting/resumed/resolved states.
- Reject/cancel returns a structured result to the model or marks the workflow failed.
- Approve/continue resumes without rerunning already-completed phases.

## Cleanup Policy Smoke

- Confirm cleanup phases preserve reports, screenshots, command logs, downloaded installers, and rollback evidence.
- Confirm cleanup only removes workflow-owned temporary scratch folders when explicitly configured.
- Confirm generated report lists cleanup failures/debt instead of hiding them.

## Release Gate

- Full suite passes.
- Secret, license, blocking-pattern, and release-readiness gates pass.
- Manual gate resume cases pass.
- Live model smoke passes or is explicitly skipped due to no key.
- Package/offline smoke uses built-in SAK tools first.
- Report generated for a completed, failed, cancelled, and resumed workflow.
