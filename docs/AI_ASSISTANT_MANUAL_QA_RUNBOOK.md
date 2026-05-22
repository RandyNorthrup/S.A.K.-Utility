# AI Assistant Manual QA Runbook

Use this when starting hands-on testing of the AI Assistant panel.

## Build Under Test

- App: `build\Release\sak_utility.exe`
- Helper: `build\Release\sak_elevated_helper.exe`
- Automated baseline: passed `128/128` tests on May 20, 2026.
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
- `turn_replay.jsonl` updates with compact run/tool replay records.

## Pass 2: Local Diagnostic Toolings

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

## Pass 3B: Provider Gateway and App Control

Prompt:

```text
show provider status for the bundled MCP and documentation providers
```

Expected:

- `sak_provider_gateway` runs before raw shell probing.
- Microsoft Learn MCP and Context7 show as HTTP providers requiring network.
- Microsoft Learn MCP reports no auth required.
- Context7 reports no auth required and is used for public docs lookup.
- `docs_query` can search Microsoft Learn and resolve Context7 library ids
  through public Streamable HTTP MCP without any bundled API key.
- `win32_mcp` shows available only when
  `tools\mcp\win32-mcp-server\win32-mcp-server.exe` is bundled.
- `tools\mcp\win32-mcp-server\THIRD_PARTY_LICENSES.txt` is present in bundled
  builds and should be reviewed when updating Win32 MCP dependencies.
- Planned providers such as browser/GitHub report planned or disabled instead
  of pretending they are ready.
- `operation=win32_mcp_call` can call bundled Win32 MCP tools when local tools
  are enabled. Read-only tools use the Win32 MCP `read_only` profile; interactive
  tools follow the selected AI access mode.
- Repeated provider/tool failures become visible as structured
  `failure_class` values. After repeated failures, the tool result should report
  `health_suppressed` instead of retrying variants of the same broken path.
- Unsupported or missing provider/app-control paths fail before execution with
  `availability_denied`, not after speculative shell probing.

Prompt:

```text
use Microsoft docs to look up Win32 UI Automation InvokePattern, then use
Context7 to resolve Qt docs
```

Expected:

- The assistant uses `sak_provider_gateway` with `operation=docs_query`.
- Microsoft Learn returns documentation search results with content URLs.
- Context7 returns candidate library ids such as Qt documentation ids.
- No Context7 API key is requested, read, logged, or bundled.

Prompt:

```text
use Win32 MCP to list visible windows matching SAK
```

Expected:

- The assistant uses `sak_provider_gateway` with
  `operation=win32_mcp_call`, `tool_name=list_windows`, and a text filter.
- The result includes visible window metadata.
- No GUI mutation is performed for this read-only observation.

Prompt:

```text
can you run a SUPERAntiSpyware quick scan?
```

Expected:

- The assistant checks the `superantispyware` app manifest before launching
  executables.
- It reports that quick/full/update actions are not yet validated for
  non-interactive execution.
- It does not reinstall SUPERAntiSpyware, brute-force helper EXEs, or exhaust
  tool iterations.
- If the model asks `sak_package_manager` to install/upgrade/uninstall during
  this scan request, tool policy blocks the call and reports the scan/install
  intent mismatch.
- It offers a manual GUI path or another scanner with a supported manifest,
  such as Microsoft Defender.

## Pass 3C: Session Search and Replay Trace

Prompt:

```text
search previous AI sessions for SUPERAntiSpyware checksum mismatch
```

Expected:

- Session search finds transcript and command-index hits when prior sessions
  contain matching text.
- The assistant uses `sak_session_search`; it does not grep binary artifacts or
  broad filesystem trees.
- Hits identify session title, source (`manifest`, `transcript`, or `command`),
  and a compact snippet.
- No raw binary logs are dumped during search.

After any AI run, inspect session files:

- `turn_replay.jsonl` contains compact records with `run_id`, `event_type`,
  `status`, prompt hash, model when known, tool name when known, and metadata.
- `trace.jsonl` remains the full trace; `turn_replay.jsonl` is the replayable
  summary used for QA reproduction.

Prompt:

```text
run a Microsoft Defender quick scan
```

Expected:

- The assistant checks the `windows_defender` app manifest.
- Because `quick_scan` is manifest-supported, it uses
  `sak_provider_gateway` with `operation=app_run_action`.
- The action runs `Start-MpScan -ScanType QuickScan` through the app action
  execution path.
- Assisted mode asks before execution. Unattended mode proceeds under the
  existing access policy.
- Result records command id, exit code, stdout/stderr, requested action, and
  evidence hints.

Prompt:

```text
run Windows System File Checker verify-only
```

Expected:

- The assistant checks the `windows_sfc` app manifest.
- Because `verify_only` is manifest-supported, it uses
  `sak_provider_gateway` with `operation=app_run_action`.
- The action runs `sfc.exe /verifyonly`; it does not run repair mode.
- `scan_repair` is reported unsupported until an explicit repair workflow with
  restore-point handling is selected.

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
- Reports generate as readable technician/customer handoffs, not raw chat dumps.
- HTML reports lead with executive summary, evidence snapshot, prioritized
  findings, risks/evidence gaps, next steps, work performed, and timeline.
- Raw transcript excerpts are present only as an appendix for audit/debugging.
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

## Failure Triage

| Failure class | Meaning | First check |
| --- | --- | --- |
| `policy_denied` | Tool request violated active access/package policy. | Confirm prompt intent and selected access mode. Scan requests must not install. |
| `availability_denied` | Tool preflight failed before execution. | Open Run Details and check provider/app manifest, missing binary, unsupported action, or invalid args. |
| `health_suppressed` | Tool/provider is in health-ledger backoff after repeated failures. | Run Details shows disabled-until, last failure class, latency, and last error. |
| `handler_missing` | Model requested a known tool without a registered runner. | Treat as app bug; tool dispatcher registration is incomplete. |
| `timeout` | Tool/provider transport did not complete in time. | Check process tree cleanup, provider health, and artifact logs. |
| `checksum_mismatch` | Package/download integrity check failed. | Do not bypass checksums or run cached installers. Report expected/actual hashes and stop. |

## Ready Criteria

Manual QA can proceed when:

- App launches from `build\Release\sak_utility.exe`.
- Release package creation refuses missing MCP/provider/app-control files:
  `win32-mcp-server.exe`, its `THIRD_PARTY_LICENSES.txt`, `providers.json`,
  `windows_defender.json`, `superantispyware.json`, and `windows_sfc.json`.
- Clean extracted release folder has no `*.local.json`, no `tools\mcp\_build`,
  and no Chocolatey runtime state under `tools\chocolatey\lib-bad`, `cache`, or
  `temp`.
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
