# AI Assistant Manual QA Runbook

Use this when starting hands-on testing of the AI Assistant panel.

## Build Under Test

- App: `build\Release\sak_utility.exe`
- Helper: `build\Release\sak_elevated_helper.exe`
- Automated baseline: passed `111/111` tests on May 14, 2026.
- Smoke script: `scripts\ai_smoke_checklist.ps1 -RunAutomated -RunLiveOpenAI`
  passed on May 14, 2026.

## Start

```powershell
.\build\Release\sak_utility.exe
```

Before testing:

- Open the AI panel.
- Load or enter an OpenAI API key.
- Use a fresh chat session named for the test, such as
  `QA AI Assistant 2026-05-14`.
- Set access mode to `Assisted Full Access` unless a test explicitly says
  otherwise.
- Keep the app log visible.

## Pass 1: Chat and Context UX

1. Create a new chat and rename it.
2. Attach a screenshot or document as context.
3. Attach a Markdown instruction file.
4. Confirm context and instruction chips are readable, color-coded, and removable.
5. Send a simple question with the attachments.
6. Confirm activity indicator, response, usage/status bar, artifacts dropdown,
   and Run Details all update.
7. Use Up/Down in the input box and confirm prompt history cycles correctly.

Expected:

- API key is never displayed.
- Chips fit and remove cleanly.
- Artifacts are stored under a folder named after the chat session.
- `memory.md`, `activity.jsonl`, `trace.jsonl`, and `run_state.json` update.

## Pass 2: Local Diagnostic Tooling

Prompt:

```text
check my hard drive
```

Expected:

- The assistant uses local tools instead of only giving instructions.
- Read-only PowerShell runs first.
- Command output is logged and stored as artifacts.
- Run Details shows model/tool/phase activity.
- The final answer summarizes actual evidence and next steps.

## Pass 3: SAK-First Package Tooling

Prompt:

```text
search the package manager for Firefox but do not install anything
```

Expected:

- `sak_package_manager` is attempted before web fallback.
- No install occurs.
- Results include package candidates or a clear package-tool failure.

Prompt:

```text
find an offline installer for Firefox but do not install it
```

Expected:

- `sak_offline_downloader` is attempted before generic web download fallback.
- If an artifact is downloaded, it appears in the artifacts dropdown and report.
- No installer is run.

## Pass 3A: Technician Tool Download/Use/Cleanup

Use `Technician Tool Assisted Task` with a harmless official HTTPS test tool or
small diagnostic utility.

Expected:

- Source review records vendor/source, signature/checksum guidance, and risk.
- `download_file` stores the tool under session artifacts and records path,
  size, SHA-256, and source URL.
- Download verification runs before execution.
- Running the downloaded tool requires approval according to access mode.
- Post-run verification records exit code and task evidence.
- Cleanup removes the downloaded tool file after preserving logs, hashes,
  source URL, and generated reports.

## Pass 4: Workflow Input Gates

Attach `Install App Now`, then prompt:

```text
install the app
```

Expected:

- The workflow pauses before running package actions.
- A focused clarification gate asks which app/package is meant.
- Closing and reopening the app restores `Resume Waiting Run`.
- Answering the gate resumes the same run id.

## Pass 5: Approval and Restore Gates

Use Assisted Full Access.

Prompt:

```text
run a harmless PowerShell command to show the current date, then explain the result
```

Expected:

- Approval appears before local execution.
- Reject returns a structured tool result.
- Approve runs the command and continues the model turn.

For restore-point behavior, use a deliberately risky but controlled prompt and
cancel at the restore dialog unless you intend to proceed:

```text
prepare to run a system-changing repair command, but stop and ask me before anything changes
```

Expected:

- Risk is detected.
- Restore-point offer appears before the risky action.
- Cancel leaves a durable human-gate/activity record.

## Pass 6: Workflow Runs

Run these workflows in separate fresh chats:

- `Full PC Health Check`
- `Drive Health Deep Check`
- `Network Connectivity Repair`
- `Printer Troubleshooting`
- `Startup Performance Triage`
- `Technician Service Report`

Expected:

- Each workflow shows phases in Run Details.
- Run Details opens automatically when the workflow starts.
- The chat header progress bar and global status bar show current phase
  progress before the first phase completes.
- `Drive Health Deep Check` should advance past `collect_health`; that phase is
  now a bounded PowerShell evidence-collection tool phase, not a model delegate.
- Read-only diagnostic workflow phases should not offer restore points.
- Read-only PowerShell workflow phases should remain non-admin and should not
  contain mutating commands such as service restarts, DNS cache clears, SFC,
  DISM repair, registry writes, or `chkdsk /f`.
- System-change workflow phases should ask before proceeding and offer restore
  point handling, but they should only elevate when the workflow command
  explicitly requires admin.
- `activity.jsonl` records `running` phase events before completed/failed
  phase events.
- Read-only phases run before repair/system-change phases.
- Analysis/report subagents should reference actual prior tool output or
  artifact paths, not generic instructions.
- If a model returns an empty `failed` status without explanation, the workflow
  should continue degraded and record the issue as a risk instead of aborting.
- PowerShell log output should keep the command id prefix on every multiline
  output line.
- Run Details opens in the chat area under the header, not inside the side rail.
- Reports generate with transcript, activity, artifacts, and memory.
- Reports generate a readable HTML handoff plus markdown source.
- Cleanup preserves evidence and records cleanup status.
- Stop cancels active model/tool/workflow work and preserves partial artifacts.

## Pass 7: Steering While Busy

Start a workflow, then submit another prompt while it is running.

Expected:

- The panel offers or applies the intended steering behavior.
- It does not launch competing system-changing work in parallel.
- Queued/replacement work is visible and traceable.

## Defect Notes

For each issue, record:

- Chat session name.
- Run id from Run Details.
- Prompt used.
- Access mode.
- Expected result.
- Actual result.
- Relevant artifact/report path.
- Whether Stop/Resume/Report still worked afterward.

## Ready Criteria

Manual QA can proceed when:

- App launches from `build\Release\sak_utility.exe`.
- API key status shows loaded without displaying the key.
- A simple chat turn works.
- A read-only local tool turn works.
- Run Details and artifacts update.
- Stop remains responsive.
- Catalog evals pass for every seeded workflow: bounded PowerShell tool
  metadata, structured delegate outputs, read-only/non-mutating diagnostics,
  unique catalog IDs/resources, technician report standards, post-tool
  verification, downloaded-tool cleanup shape, and degraded handling for empty
  model failure payloads.
