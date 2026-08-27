# AI Multi-Agent Workflow and Workflow Library Plan

**Version**: 0.1
**Date**: May 14, 2026
**Status**: Research Complete; Execution Roadmap Locked; v1 Implementation Complete; Ready For Manual QA
**Parent Feature**: AI Assistant Panel
**Target App**: S.A.K. Utility
**Primary Language**: C++23
**UI Framework**: Qt 6
**Feature Isolation**: Behind `SAK_ENABLE_AI_ASSISTANT`

---

## Purpose

The current AI Assistant panel has a single user-facing agent, a prompt template
combo, local tools, context chips, OpenAI web search, report generation, SAK
package manager tooling, SAK offline downloader tooling, and cancellation for
the active model/tool run.

This phase replaces simple prompt templates with complete workflow templates and
adds a native multi-agent execution layer:

- The main agent remains the user-facing overseer.
- The overseer plans work, delegates bounded subtasks to specialist subagents,
  monitors progress, handles failures, asks the human when needed, and writes the
  final answer.
- Workflow templates define not just a prompt, but phases, subagents,
  instructions files, skills, required software, tool permissions, checks,
  troubleshooting branches, reporting, and cleanup.
- The Stop/Cancel path cancels the main agent, all queued/running subagents,
  model requests, tool calls, local commands, offline package jobs, and pending
  approvals.

This document is the implementation plan for that phase.

---

## Implementation Progress

Last updated: May 14, 2026.

Completed:

- Added `AiOrchestrator` with phase engine, parallel grouping of read-only
  delegate phases, mutating-phase serialization, conditional skip/fallback,
  and child-cancel to run-cancel detection. Tests use fake subagent runner +
  fake tool executor; orchestrator stays decoupled from the live panel.
- Added `AiSubagentRunner` with structured task/result contracts, fake-model
  test harness, hierarchical cancellation, and token-budget enforcement.
- Added `AiWorkflowTemplate` and `AiWorkflowStore`.
- Added built-in Qt resource loading for `resources/ai/workflows/*.json`.
- Added user workflow override loading from the app data workflow directory.
- Added validation and unit tests for workflow parsing, invalid templates,
  built-in catalog loading, and user override behavior.
- Added a v1 catalog seed with 10 workflows, instruction Markdown files, and
  skill Markdown files.
- Replaced immediate prompt insertion with a workflow selector row:
  - choose workflow
  - preview details
  - attach workflow
- Added removable workflow, instruction, and skill chips in the composer.
- Hooked workflow instructions and skills into the existing context attachment
  path so they are sent with the next model request.
- Added `AiTraceStore` with session-local `trace.jsonl` persistence.
- Added `AiRunState` for durable run/phase/tool count state shape.
- Added `AiRunStateStore` and panel-side `run_state.json` snapshot writes on
  every run start / tool transition / run completion.
- Added hierarchical `CancellationToken` for run/phase/agent/tool cancellation.
- Added per-turn and per-tool-call child cancellation tokens so Stop cancels
  in-flight dispatches before each tool fires.
- Added `AiToolPolicy` for local tool policy decisions, risky-change detection,
  leases, and restore-point recommendations.
- Added `AiToolDispatcher` and routed screenshot, download, package-manager,
  and offline-downloader tool calls through it with policy evaluation before
  every dispatch. Shell tool calls also evaluate the policy before broker
  startup.
- Wired mutating package/offline downloader calls through `AiLeaseManager`.
- Added shell-command mutating leases before broker startup so arbitrary
  PowerShell/cmd/process actions share the same single-writer guard.
- Wired the workflow runner into the panel: attached workflows now run through
  `AiSubagentRunner` and `AiOrchestrator` instead of becoming inert prompt text.
- Added workflow input binding from the user request into orchestrator context
  so tool phases can resolve `app_name`, `app_list`, `query`, and optional
  `output_dir`.
- Added workflow-aware package resolution. Package install/check/verify phases
  search the SAK package manager first; offline installer/bundle phases search
  the SAK offline downloader first, select package IDs, and pass structured
  package arrays into the actual tool action.
- Hardened workflow package resolution with `AiPackageSelection`: exact
  package-id/title matches are selected, ambiguous matches produce structured
  candidates and route through the human-decision recovery path, and no
  candidate path avoids guessing.
- Added a SAK-first fallback eval for the offline-installer workflow: when the
  built-in downloader phase fails, fallback research runs after that failure
  and cleanup still runs.
- Added package-manager lookup fallback for the install workflow: read-only SAK
  lookup failures degrade to official-source research, while actual install
  failures remain human-gated.
- Added no-op cleanup handling for cleanup phases that only need to preserve
  artifacts and record cleanup status.
- Fixed the parallel delegate capture bug by copying each phase into its worker
  task before launching `std::async`.
- Added guarded workflow UI callbacks with `QPointer` and destructor-side
  cancellation so closing the panel does not leave worker callbacks targeting a
  destroyed widget.
- Fixed synchronous OpenAI model-client failure/cancel handling so subagent model
  calls do not hang when a request fails before the local event loop starts or
  when cancellation fires during the wait.
- Widened context/instruction/workflow chips and kept remove controls visible so
  attached context is readable.
- Added a chat-header activity indicator for thinking/tool/workflow states.
- Added prompt-history cycling with Up/Down from the chat input.
- Aligned primary/danger chat controls with the shared SAK theme button styles.
- Added custom chat session renaming and custom names for new chats.
- Moved generated artifacts under a chat-title-named artifact directory.
- Added per-session `memory.md` working memory and inject it into future turns
  as low-priority session context.
- Added production steering behavior: prompts submitted during an active run can
  be applied as steering, queued as the next request, or used to stop-and-queue
  a replacement without launching a competing run.
- Added live workflow observability: the orchestrator emits phase-start events
  before long-running model/tool work, the chat header shows a workflow progress
  bar, Run Details opens automatically on workflow start, the status bar receives
  half-step progress updates, and `activity.jsonl` records `running` phase
  events before phase completion.
- Added artifact-folder migration when a current chat is renamed. Older direct
  artifact subfolder migration was removed because only the current session
  layout is supported during this pre-release test cycle.
- Added working-memory trimming so `memory.md` stays bounded during long
  technician sessions.
- Added an expanded multi-agent harness research pass covering LangGraph,
  AutoGen, CrewAI, CAMEL, MetaGPT, LlamaIndex, AutoGPT, and OpenAI Agents SDK.
- Added a follow-up UX/observability research pass after real workflow testing:
  - LangGraph streams run state using typed `updates`, `custom`, `tasks`, and
    debug events and treats persistent thread ids as the resume/debug cursor.
  - LangGraph interrupts persist graph state and surface a JSON-serializable
    human prompt while waiting for resume input.
  - AutoGen Studio exposes live message streaming between agents, visual message
    flow, and pause/stop controls in its playground UI.
  - OpenHands exposes live status monitoring, pause/resume, mid-run user
    message steering, and custom visualizers that consume every conversation
    event for progress display.
  - OpenAI Agents SDK tracing records LLM generations, tool calls, handoffs,
    guardrails, and custom events as a complete run trace.
  - Design decision: SAK workflows must never rely on completion-only updates;
    every phase needs a visible start event, current state, activity record,
    cancel-aware terminal state, and resumable run id.
- Added concrete agent activity states, trace event fields, subagent task
  contract requirements, workflow behavior matrix, recovery matrix, and real
  model smoke-test procedure.
- Added an opt-in live OpenAI Responses smoke test to
  `test_openai_responses_client`. It skips by default, uses environment keys or
  the saved encrypted app-directory key, redacts failures, and verifies a
  sentinel response plus token usage when enabled.
- Added normalized `activity.jsonl` session logging through `TraceStore`.
  Every panel trace event now also writes a production activity event with
  session/run ids, activity kind, state, summary, workflow/phase/agent/tool
  fields, artifact/evidence refs, metadata, and token usage.
- Workflow phase completions now emit `workflow_phase` trace/activity events
  with workflow id, phase id/type, agent id, duration, tool result, error,
  recovery action, and recovery reason. The run-state snapshot also tracks the
  current phase plus completed workflow subagent/tool counts.
- Delegate/subagent phase completions now surface structured summary, findings,
  actions, risks, human questions, next steps, artifacts, citations, and
  evidence refs in the transcript, working memory, and normalized activity
  metadata instead of only showing a compact `phase ok` line.
- Added a compact Run Details view in the existing right rail. It reads
  `activity.jsonl`, filters to the active run when possible, shows the current
  run state, and lists the latest activity events without adding a separate
  activity tab.
- Hardened the seeded workflow catalog so every built-in workflow has an
  explicit report, verification/review, always-run cleanup shape, and risky
  workflows include approval/restore gating language. Added CI eval coverage to
  keep that structure from regressing.
- Added workflow required-input gating before workflow launch. Required
  `string`, `list`, and `path` inputs are validated from the user prompt and,
  when missing, requested before the workflow starts so installs/downloads do
  not run from empty guesses.
- Missing workflow-input prompts now write `waiting_for_human` run-state
  snapshots and normalized `workflow_input` activity events before the modal is
  shown, then resolve as completed/cancelled events after the user answers.
- Added `activity.jsonl` timeline export to the generated AI session report.
- Added `AiRecoveryPolicy`, a native recovery classifier that turns phase/tool/
  subagent failures into durable decisions: retry, reassign, continue degraded,
  ask human, or abort. Failed orchestrator phases persist `recovery_decision`
  metadata and the orchestrator now acts on those decisions.
- Added active recovery execution in `AiOrchestrator`: transient failures retry
  once with first-failure audit metadata, offline/downloader fallback failures
  continue degraded so conditional fallback phases can run, risky mutating
  failures pause as `waiting_for_human`, and critic failures are reassigned to
  the overseer path for review.
- Added durable human-gate persistence: command approvals, restore-point offers,
  restore-point failure decisions, workflow input waits, and workflow recovery
  pauses now write typed pending gate state into `run_state.json` and append
  transitions to `human_gates.jsonl`.
- Run Details and generated reports now surface pending/resolved human gates so
  waits and decisions survive session reopen and can be audited separately from
  the transcript.
- Workflow input gates can now resume the exact same run after session reopen:
  pending input metadata stores the workflow id, original user request, partial
  input values, and run id; the Resume Waiting Run action answers the gate,
  gathers remaining inputs, and launches the workflow with the preserved run id.
- Approval gates created during OpenAI local tool turns now persist enough
  pending tool-turn state to resume after session reopen. Command approvals,
  restore-point offers, and restore-point-failure continue gates can restore
  the same response id, tool-call list, prior outputs, call index, and run id;
  approval continues the pending tool call, while rejection/cancel returns a
  structured tool output to the model.
- Workflow-recovery gates now persist resume state for the paused orchestrator:
  run id, workflow id, original user request, resolved workflow inputs, phase
  history, and resume-start phase index. Resume Waiting Run can continue from
  the next phase with the same run id without replaying already-run phases, or
  abort and write a report.
- Upgraded `memory.md` initialization to a structured working-memory template:
  `Pinned Facts`, `Current Task`, `Decisions`, `Open Questions`, `Artifacts`,
  and `Resolved History`. Legacy/trimmed memory preserves these sections.
- Upgraded memory trimming into section-aware compaction: pinned/current/
  decision/open/artifact sections are preserved, while only resolved history is
  aggressively compacted.
- Generated reports now include a redacted Working Memory section and workflow
  runs automatically write a report artifact at completion/cancel/failure.
- Replaced the modal workflow-details preview with a non-modal inline details
  panel in the right rail (phases, agents, requirements, instructions, skills,
  acceptance criteria).
- Expanded the panel status bar to include phase id and active/done agent
  counts alongside turn/session/tool totals.
- Wired the current AI panel to write trace events for sessions, runs, model
  calls, tool queues, local tools, failures, and cancellation.
- Wired Stop to cancel the active run token, model request, active command,
  offline worker, and pending tool turn.
- Moved the panel's risky-command detection onto the shared tool policy helper.
- Added `AiWorkflowClarifier` and wired it into workflow launch so vague app or
  package inputs are clarified as durable workflow-input gates before tools run.
- Added workflow evals for cleanup failure and report failure branches.
- Expanded the built-in workflow catalog from 10 to 13 workflows with network
  connectivity repair, startup performance triage, and printer troubleshooting.
- Added `docs/AI_ASSISTANT_PRODUCTION_SMOKE_TESTS.md` and
  `scripts/ai_smoke_checklist.ps1` for release-gate manual smoke coverage,
  durable gate resume checks, harmless SAK tool smoke, and opt-in live model
  validation.
- Added phase history to Run Details so recent per-phase agent/tool state is
  visible without opening a separate activity tab.
- Replaced the plain text Run Details dump with a themed right-rail timeline:
  summary tiles, pending human gate panel, phase timeline, and activity
  timeline with state badges, tags, errors, questions, token counts, and
  artifact/evidence counts.
- Added explicit workflow phase arguments so catalog `tool_action` steps can
  carry concrete tool payloads instead of relying on prompt text.
- Added workflow PowerShell execution through the panel tool dispatcher. Workflow
  PowerShell phases now run through policy evaluation, risky-action approval or
  restore-point handling, mutating leases, command logging, artifacts, working
  memory, and root-run cancellation.
- Hardened all seeded `run_powershell` workflow phases with explicit commands,
  timeouts, output caps, and admin flags. Startup triage now keeps unsafe
  "disable startup item" work as a human-approved plan instead of blindly
  executing a static command.
- Added eval coverage that fails any seeded workflow `run_powershell` phase
  missing an explicit `arguments.command`.
- Fixed the workflow stall found during Drive Health Deep Check manual QA:
  model calls have a hard timeout, stale open-on-close run snapshots are marked
  cancelled on reload, and workflow tool phases marshal safely back to the UI
  thread before touching panel-owned Qt objects. The production workflow runner
  now uses a per-subagent OpenAI model-client factory, so workflow-declared
  read-only delegate phases can run in parallel without sharing a signal-based
  Responses client instance.
- Converted seeded evidence/search phases that need real local data from model
  delegates into concrete `tool_action` steps. Drive, PC health, BSOD, Windows
  Update, clean uninstall, install, offline download, and offline bundle
  workflows now collect SAK/PowerShell evidence first, then pass capped prior
  phase results into subagents as shared workflow context for analysis/reporting.
- Added workflow placeholder substitution for tool arguments such as
  `${app_name}`, with PowerShell-safe single-quote escaping for workflow command
  payloads.
- Added multi-query package/offline search support so bundle workflows can
  search every requested app through SAK tooling before any fallback research.
- Fixed the next Drive Health manual QA failure: subagent shared workflow
  context is now compacted before model calls, token budget overruns are
  recorded as risks instead of failing otherwise valid subagent results, and
  read-only diagnostic workflow phases no longer trigger restore-point prompts.
- Fixed the next Drive Health model-contract failure: subagent prompts now
  always specify the concrete standard result JSON shape, `status=failed`
  without summary/error/content is treated as degraded empty output instead of
  aborting the workflow, and multiline PowerShell chunks are prefixed per line
  in the log.
- Moved Run Details out of the side rail. It now opens inline in the chat area
  under the header, keeping the side rail focused on session, agent, workflow,
  access, and key controls.
- Hardened workflow-wide coding rules from manual QA discoveries. Catalog evals
  now verify every seeded workflow has bounded PowerShell command metadata,
  structured delegate outputs, non-admin/non-mutating read-only diagnostics,
  and degraded continuation for empty model failure payloads. Workflow risk
  gating no longer forces admin elevation; elevation follows explicit command
  metadata while system-change phases still ask/offer restore handling.
- Completed the technician workflow hardening pass. Added catalog-wide checks
  for duplicate workflow/phase/agent IDs, missing instruction/skill resources,
  technician-grade report prompts, post-tool verification, and tool-download/
  run/verify/cleanup sequencing. Added `Technician Tool Assisted Task`, which
  reviews an official HTTPS tool source, downloads it to session artifacts,
  verifies path/hash/source, runs only after approval, verifies the result, and
  removes the downloaded tool while preserving logs/reports. Session report
  generation now writes a readable HTML handoff plus markdown source and
  includes artifact inventory.

Verified:

- `cmake --build build --config Release --target sak_utility`
- `ctest --test-dir build -C Release -R "test_ai" --output-on-failure`
  passed `17/17` AI tests on May 14, 2026 after the workflow-stall fixes.
- `ctest --test-dir build -C Release --output-on-failure` passed `111/111`
  full tests on May 14, 2026 after the same fixes.
- `ctest --test-dir build -C Release --output-on-failure` passed `111/111`
  full tests again after the inline Run Details and token-budget fixes.
- `ctest --test-dir build -C Release --output-on-failure` passed `111/111`
  full tests again after the degraded empty-subagent-output and log-prefix fixes.
- `ctest --test-dir build -C Release --output-on-failure` passed `111/111`
  full tests again after the workflow-wide catalog guards and risk/elevation
  fix.
- `ctest --test-dir build -C Release --output-on-failure` passed `111/111`
  full tests after the technician workflow/reporting hardening pass.

- `cmake -S . -B build`
- `cmake --build build --config Release --target test_ai_workflow_store`
- `cmake --build build --config Release --target test_ai_cancellation_token`
- `cmake --build build --config Release --target test_ai_run_state`
- `cmake --build build --config Release --target test_ai_human_gate_store`
- `cmake --build build --config Release --target test_ai_run_state_store`
- `cmake --build build --config Release --target test_ai_package_selection`
- `cmake --build build --config Release --target test_ai_trace_store`
- `cmake --build build --config Release --target test_ai_tool_policy`
- `cmake --build build --config Release --target test_ai_tool_dispatcher`
- `cmake --build build --config Release --target test_ai_subagent_runner`
- `cmake --build build --config Release --target test_ai_orchestrator`
- `cmake --build build --config Release --target test_ai_workflow_store test_ai_workflow_evals test_ai_workflow_clarifier sak_utility`
- `cmake --build build --config Release --target sak_utility`
- `ctest --test-dir build -C Release -R "test_ai_" --output-on-failure`
- `ctest --test-dir build -C Release -R "test_ai_|test_openai_responses_client" --output-on-failure`
- `ctest --test-dir build -C Release -R "^test_openai_responses_client$" --output-on-failure`
- `ctest --test-dir build -C Release --output-on-failure` (111/111 passed)
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\ai_smoke_checklist.ps1 -RunAutomated -RunLiveOpenAI`

Remaining implementation follow-up:

- No blocking implementation items remain for v1.
- Manual QA is the next step. Use `docs/AI_ASSISTANT_MANUAL_QA_RUNBOOK.md`
  and `docs/AI_ASSISTANT_PRODUCTION_SMOKE_TESTS.md`.

---

## Research-Derived Execution Roadmap

This section is the control rail for implementation. No new multi-agent feature
work should be started unless it maps to a milestone below, has acceptance
criteria, and updates this document when complete.

### Source-Derived Architecture Decisions

| Research source / pattern | What it does well | SAK decision |
| --- | --- | --- |
| OpenAI Agents SDK manager pattern | A manager agent calls specialist agents as tools and keeps ownership of the final answer. | Use one human-facing overseer. Specialists never take over the chat. |
| OpenAI handoffs pattern | Transfers conversation control to another agent. | Do not use as default. SAK needs one accountable operator-facing agent. |
| OpenAI tracing / runner loop | Repeated model/tool turns with spans for tools, handoffs, guardrails, and outputs. | Keep a native C++ runner/orchestrator and write `trace.jsonl` plus normalized `activity.jsonl`. |
| Anthropic orchestrator-worker guidance | Lead agent decomposes work and parallel workers investigate bounded subtasks. | Use orchestrator-worker only where it adds value: diagnostics, research, verification, and report review. |
| Anthropic evaluator-optimizer pattern | A checker reviews generated work and improves quality. | Add critic/review phases to risky or report-heavy workflows. Reassign critic failure to overseer. |
| LangGraph persistence/checkpoints | Long-running graph state can pause, inspect, resume, and recover. | Persist every run to `run_state.json`; model human gates as durable pauses. |
| LangGraph interrupts / human-in-loop | Work pauses at decision points and resumes with a recorded answer. | Approval, restore-point, ambiguity, and missing-input gates must write activity events before asking. |
| AutoGen group chat / termination | Multi-agent work needs explicit speaker choice and stop conditions. | Avoid open-ended agent chat. SAK workflows use finite phases, max retries, max subagents, token budgets, and cancellation tokens. |
| CrewAI Flows / Tasks / Memory | Workflows are explicit tasks with state and memory. | Workflow JSON is the source of truth: role, phases, required tools, instructions, skills, cleanup, reporting, acceptance criteria. |
| LlamaIndex human-in-loop | High-risk operations should ask humans at tool boundaries. | Guard local tools and destructive actions at dispatcher/lease level, not only prompt level. |
| OpenAI API safety identifiers | Stable privacy-preserving identifiers help isolate abuse handling to one user/session instead of the whole organization. | Send a hashed local AI session/run identifier in `safety_identifier` for Responses calls. |

### Non-Negotiable Design Rules

- The overseer is always the only user-facing agent.
- Every workflow must be finite: no free-form multi-agent chat loops.
- Every local action must pass through `AiToolDispatcher` and `AiToolPolicy`.
- Mutating local actions must acquire a lease before execution.
- Potentially destructive changes must offer or record restore-point handling.
- Ambiguous package/app names must ask the human instead of choosing a first result.
- Built-in SAK tools must be attempted before web fallback for package install,
  offline installer, and offline bundle workflows.
- Every model call, tool call, phase, approval, cancellation, report, and cleanup
  must write durable activity.
- Stop must cancel the root run token and every child subagent/tool/model path.
- Generated reports must be reconstructible from session files.

### Milestone Gate Policy

Each milestone has four gates:

- **Implementation gate**: code and workflow templates are updated.
- **Persistence gate**: run state, trace, activity, memory, and artifacts are
  written or intentionally not applicable.
- **UX gate**: the user can see what happened and what the agent needs next.
- **Eval gate**: deterministic tests cover success, failure, cancellation, and
  fallback behavior where applicable.

A milestone is not complete until all four gates pass.

### M0: Research Lock and Architecture Map

Status: Complete.

Goal: freeze the researched architecture so implementation is not improvised.

Completed deliverables:

- Research summary with primary sources.
- Native C++ architecture chosen over embedding another agent runtime.
- Overseer-worker pattern selected.
- Handoffs rejected as default behavior.
- Finite workflow phases selected over open-ended group chat.
- Local persistence model selected: session folder, transcript, trace,
  activity, run state, memory, artifacts.

Acceptance:

- Architecture choices trace directly to source research.
- The doc names what SAK is borrowing and what it is intentionally not using.

### M1: Workflow Template System

Status: Complete for v1.

Goal: replace generic prompt templates with complete task workflows.

Action items:

- Define workflow schema/data model.
- Load built-in workflow JSON from Qt resources.
- Load user overrides from app data.
- Validate required fields and skip invalid templates safely.
- Include agents, phases, instructions, skills, required software, risk,
  cancellation policy, acceptance criteria, reporting, and cleanup.
- Seed at least 10 high-value workflows.

Acceptance:

- Workflow catalog loads at startup.
- Invalid workflow files do not crash the app.
- Each seeded workflow states required tools, safety gates, report phase,
  verification/review phase, and cleanup phase.

Eval requirements:

- Valid/invalid workflow parsing tests.
- Built-in catalog loading tests.
- User override tests.
- Seeded workflow shape tests.

### M2: Workflow UI and Context UX

Status: Complete for v1.

Goal: make workflows selectable and inspectable without hiding behavior.

Action items:

- Replace prompt insertion with workflow selector and Add button.
- Show inline workflow details: phases, agents, risks, requirements,
  instructions, skills, acceptance criteria.
- Represent workflow, instruction, skill, file, image, and document context as
  removable chips.
- Support custom session names.
- Store artifacts under a directory named after the chat session.
- Add activity indicator and prompt history Up/Down cycling.
- Keep AI panel styling aligned with the SAK theme.

Acceptance:

- User can inspect what a workflow will do before running it.
- Attached context is visible, removable, and color-coded by type.
- Main chat remains usable without selecting a workflow.

Deferred v2 backlog:

- More visible live per-subagent state in the right rail.
- Better dense summaries for long subagent outputs.

### M3: Orchestrator and Subagent Runtime

Status: Complete for v1.

Goal: run finite multi-agent workflows with deterministic control.

Action items:

- Build `AiOrchestrator` with overseer, delegate, tool, and cleanup phases.
- Build `AiSubagentRunner` with structured task/result contracts.
- Support read-only parallel delegate groups.
- Serialize mutating phases.
- Support conditional fallback phases using success/failure flags.
- Keep results in deterministic original phase order.
- Add max subagent parallelism and token budgets.
- Reassign critic/review failures to overseer where safe.

Acceptance:

- A workflow runs end to end through orchestrator.
- Two read-only subagents can run in parallel.
- Mutating work runs one phase at a time.
- Conditional fallback runs only after the triggering failure flag exists.

Eval requirements:

- Sequential phase test.
- Parallel read-only test.
- Mutating serialization test.
- Condition skip/fallback tests.
- Failure stop/recovery tests.
- Cancellation propagation tests.

Deferred v2 backlog:

- Per-subagent retry policy beyond basic retry count.
- Harder wall-clock timeout/preemption for stuck model calls.

### M4: Tool Policy, Leases, and Safety Gates

Status: Complete for v1 gate persistence and same-run resume coverage for
workflow-input, OpenAI tool-turn approval/restore, and workflow-recovery gates.

Goal: make arbitrary PC access powerful but controlled by explicit gates.

Action items:

- Route screenshot, download, package manager, offline downloader, shell,
  PowerShell, cmd, and process calls through policy evaluation.
- Add `AiLeaseManager` so mutating local work has a single-writer guard.
- Detect risky commands and recommend restore points.
- Ask approval in assisted mode.
- Offer restore point before destructive or rollback-worthy actions.
- Write approval/restore decisions to activity and run state.
- Ensure user cancellation during gates cancels child work.
- Persist typed human-gate wait/decision records to `human_gates.jsonl`.
- Restore pending gate state into Run Details when a session is reopened.

Acceptance:

- Read-only tools run without mutating leases.
- Mutating tools require leases.
- Risky work has approval/restore handling.
- Tool denials are visible to the user and recoverable by the overseer.

V1 status:

- None for v1. Manual reopen/resume smoke coverage is documented in
  `docs/AI_ASSISTANT_PRODUCTION_SMOKE_TESTS.md` and scripted by
  `scripts/ai_smoke_checklist.ps1`.

### M5: Workflow Input Clarification and Ambiguity Handling

Status: Complete for v1.

Goal: prevent wrong actions from guessed user intent.

Action items:

- Extract required inputs from the user request.
- Ask focused questions for missing string/list/path inputs before launch.
- Treat ambiguous package/app names as human-decision gates.
- Add package selection helper that allows exact matches and rejects ambiguous
  multi-candidate matches.
- Add overseer clarification step for unclear task scope before tool phases.
- Store resolved inputs in run metadata, activity, and memory.
- Store pending/resolved input gates in `run_state.json` and `human_gates.jsonl`.

Acceptance:

- Install/download workflows never choose first package result blindly.
- Missing app list or output directory pauses before workflow launch.
- Human answer becomes part of the same task context.

Eval requirements:

- Missing input test.
- Ambiguous package test.
- Exact package-id/title match test.
- Bundle destination/app-list test.

V1 status:

- None for v1. `AiWorkflowClarifier` catches missing and ambiguous app/package
  inputs before tool phases and records the prompt as a durable input gate.

### M6: Memory, Artifacts, and Reports

Status: Complete for v1.

Goal: give each session durable, inspectable working memory and outputs.

Action items:

- Create per-session `memory.md`.
- Use stable sections: Pinned Facts, Current Task, Decisions, Open Questions,
  Artifacts, Resolved History.
- Compact memory by preserving high-value sections and trimming resolved
  history first.
- Include memory in prompts as low-priority session context.
- Put artifacts under the chat-session artifact folder.
- Generate reports with transcript, activity timeline, artifacts, and redacted
  working memory.

Acceptance:

- Long sessions keep important facts after trimming.
- Reports include decisions, open questions, artifacts, and activity.
- Renaming a chat migrates or maps artifact folders safely.

Deferred v2 backlog:

- Cleanup phases stay preserve-only for v1. Safe workflow-specific cleanup
  tools can be added later after manual QA proves the retention policy is right.

### M7: SAK-First Tool Workflows and Fallbacks

Status: Complete for v1 install/offline paths.

Goal: use SAK's built-in package/offline tools before web fallback.

Action items:

- Package install/check/verify phases search SAK package manager first.
- Offline installer/bundle phases search SAK offline downloader first.
- Resolve package IDs with exact/ambiguous rules.
- Continue degraded to official web research only after built-in read-only
  lookup/download path fails.
- Keep actual install failures human-gated.
- Record fallback source quality in reports.

Acceptance:

- Offline installer fallback research runs only after SAK downloader failure.
- Install workflow official-source research runs only after SAK package lookup
  failure.
- Install phase is skipped when the package precheck failed.

Eval requirements:

- SAK-first success path.
- Offline downloader failure fallback path.
- Package lookup failure fallback path.
- Ambiguous package human-gate path.

### M8: Observability and Run Details

Status: Complete for v1.

Goal: make every agent activity inspectable and reportable.

Action items:

- Write `trace.jsonl` for spans.
- Write normalized `activity.jsonl` for user/report timelines.
- Write `run_state.json` on every state transition.
- Show status bar totals for model/tool/session usage and phase/subagent counts.
- Add Run Details right-rail view with summary, phase timeline, and activity
  timeline.
- Include activity timeline in generated reports.

Acceptance:

- User can tell whether the agent is thinking, calling a tool, waiting for a
  human, recovering, cancelling, reporting, or cleaning up.
- Reports can be rebuilt from session files.
- Activity survives app restart.

### M9: Evaluation Harness and Real-Model Smoke Tests

Status: Complete for v1.

Goal: prove behavior without spending tokens in normal CI, while allowing local
real-model validation.

Action items:

- Keep deterministic fake-model unit tests in normal CI.
- Keep opt-in live OpenAI Responses smoke test.
- Add opt-in real-model workflow planner smoke test with fake local tools.
- Add opt-in harmless package/offline downloader evals.
- Add report/cleanup failure evals.
- Add cancelled subagent evals.
- Add regression tests for durable gate resume when implemented.

Completed coverage:

- Deterministic fake-model workflow evals run in normal CI.
- Opt-in live OpenAI Responses smoke test can use environment or saved key.
- Report failure and cleanup failure branches have workflow eval coverage.
- Cancelled subagent/workflow path has eval coverage.
- Manual live workflow/package/offline smoke checklist is documented and
  scripted.

Acceptance:

- Normal CI spends no tokens.
- Local opt-in tests can use saved key or temporary key.
- Smoke tests verify model list, response creation, token usage, workflow
  prompt quality, SAK-first behavior, and report generation.

### M10: Production Hardening and Release Gate

Status: Complete for v1 release-candidate checklist.

Goal: decide when the v1 feature can be considered production-grade.

Action items:

- Run full CTest suite.
- Run AI/OpenAI slice.
- Run manual app smoke test:
  - open AI panel
  - load API key
  - create/rename session
  - attach instruction/context files
  - run read-only diagnostic prompt
  - run workflow with missing input
  - cancel an active workflow
  - generate report
  - inspect artifacts and memory
- Run optional real-model workflow smoke test.
- Review logs for secret redaction.
- Confirm no API key appears in UI, logs, reports, activity, trace, or memory.
- Confirm OpenAI request payload tests include `safety_identifier` without raw
  user identity or raw local session IDs.
- Confirm feature remains isolated behind `SAK_ENABLE_AI_ASSISTANT`.

Release gate:

- `sak_utility` builds.
- Full test suite passes.
- Manual smoke test passes.
- No known unsafe first-result package selection.
- No known orphan subagent/tool path after Stop.
- Known limitations are listed in the plan and release notes.

### Work Queue

Priority order from here:

1. Manual QA using `docs/AI_ASSISTANT_MANUAL_QA_RUNBOOK.md`.
2. Release-candidate smoke using `docs/AI_ASSISTANT_PRODUCTION_SMOKE_TESTS.md`.

### No-Freestyle Implementation Rule

Before changing code for this feature, update or reference:

- milestone id,
- action item,
- expected files,
- acceptance check,
- eval/test command.

After changing code, update:

- milestone status,
- completed bullet,
- verification command/result,
- any remaining follow-up.

---

## Research Summary

Primary sources reviewed:

- OpenAI Agents SDK orchestration docs:
  https://openai.github.io/openai-agents-python/multi_agent/
- OpenAI Agents SDK agent docs:
  https://openai.github.io/openai-agents-python/agents/
- OpenAI Agents SDK handoffs docs:
  https://openai.github.io/openai-agents-python/handoffs/
- OpenAI Agents SDK guardrails docs:
  https://openai.github.io/openai-agents-python/guardrails/
- OpenAI Agents SDK tracing docs:
  https://openai.github.io/openai-agents-python/tracing/
- OpenAI Agents SDK JS running agents docs:
  https://openai.github.io/openai-agents-js/guides/running-agents/
- Anthropic multi-agent research system:
  https://www.anthropic.com/engineering/multi-agent-research-system
- Anthropic building effective agents:
  https://www.anthropic.com/engineering/building-effective-agents
- OpenAI conversation state guide:
  https://platform.openai.com/docs/guides/conversation-state?api-mode=responses
- OpenAI prompt caching guide:
  https://platform.openai.com/docs/guides/prompt-caching
- OpenAI Cookbook session memory example:
  https://cookbook.openai.com/examples/agents_sdk/session_memory
- LangGraph multi-agent handoffs:
  https://docs.langchain.com/oss/python/langchain/multi-agent/handoffs
- LangGraph persistence and checkpointing:
  https://docs.langchain.com/oss/python/langgraph/persistence
- LangGraph interrupts / human-in-the-loop:
  https://docs.langchain.com/oss/python/langgraph/interrupts
- AutoGen SelectorGroupChat:
  https://microsoft.github.io/autogen/0.4.4/user-guide/agentchat-user-guide/selector-group-chat.html
- AutoGen termination conditions:
  https://microsoft.github.io/autogen/0.4.8/user-guide/agentchat-user-guide/tutorial/termination.html
- CrewAI introduction, Flows, Tasks, and Memory:
  https://docs.crewai.com/en/introduction
  https://docs.crewai.com/en/concepts/flows
  https://docs.crewai.com/en/concepts/tasks
  https://docs.crewai.com/en/concepts/memory
- CAMEL Workforce and role-playing workers:
  https://docs.camel-ai.org/key_modules/workforce
  https://docs.camel-ai.org/reference/camel.societies.role_playing
- MetaGPT:
  https://github.com/FoundationAgents/MetaGPT
- LlamaIndex human-in-the-loop and multi-agent patterns:
  https://developers.llamaindex.ai/python/framework/understanding/agent/human_in_the_loop/
  https://developers.llamaindex.ai/python/framework/understanding/agent/multi_agent/
- AutoGPT blocks / integrations:
  https://agpt.co/docs/integrations

Key findings:

- OpenAI documents two core multi-agent patterns: **manager/agents-as-tools**
  and **handoffs**. Manager mode fits SAK best because one main agent can own the
  final answer, combine specialist outputs, and enforce shared guardrails.
- Handoffs are useful when a specialist should take over the conversation. SAK
  should not use this as the default because the user wants one overseer agent
  that stays accountable and human-facing.
- OpenAI's runner model is a loop: call model, inspect output, return final
  output or execute tools/handoffs, append results, repeat until done or max
  turns. SAK already owns a smaller version of this loop and should extend it in
  C++ rather than introduce a Python/TypeScript runtime.
- OpenAI's running-agent docs call out `maxTurns`, cancellation via an abort
  signal, sessions, tracing, tool concurrency limits, and error handlers. SAK
  needs native equivalents: max agent turns, cancellation tokens, session
  persistence, trace spans, concurrency caps, and error-to-model feedback.
- OpenAI guardrail docs distinguish workflow boundaries: input guardrails,
  output guardrails, and tool guardrails run at different points. For SAK,
  critical controls must wrap tools and subagent boundaries, not rely only on one
  top-level prompt.
- OpenAI tracing docs record model calls, tools, handoffs, guardrails, and custom
  events as spans. SAK should store a local trace tree in the AI session artifacts
  and log major events to the shared app log.
- Anthropic's production multi-agent research system uses an orchestrator-worker
  pattern where a lead agent coordinates parallel specialist subagents. Their
  reported eval rubric includes factual accuracy, citation accuracy,
  completeness, source quality, and tool efficiency. SAK should adapt this into
  workflow evals and final report checks.
- Anthropic's general agent guidance recommends simple composable patterns first:
  prompt chaining, routing, parallelization, orchestrator-workers, and
  evaluator-optimizer. SAK should avoid "agent sprawl" by using multi-agent only
  where the workflow benefits from parallel specialists or independent checks.
- OpenAI's conversation-state guidance supports three relevant approaches:
  app-managed history, `previous_response_id`, and durable Conversations. SAK
  currently uses local transcripts plus `previous_response_id`; the local
  `memory.md` file gives SAK an app-owned memory layer that remains inspectable,
  portable, and reportable.
- OpenAI documents context-window pressure and compaction for long-running
  conversations. SAK should keep the session memory concise, pin important
  facts, and eventually compact old tool chatter instead of replaying every
  transcript line to every agent.
- OpenAI prompt caching works best when stable instructions and examples stay at
  the front of the prompt and variable session data comes later. SAK should keep
  global/tool policy instructions stable, then append workflow, memory, context
  chips, and current user steering below them.
- The Agents SDK session-memory example frames the session as the memory object.
  SAK's equivalent should be a session folder containing transcript, trace,
  run-state, artifacts, and a human-readable working memory file.
- LangGraph treats handoffs as state changes and uses checkpoints to pause,
  resume, and inspect long-running workflows. SAK should keep this behavior
  native: every run has `run_state.json`, every approval is a durable pause, and
  every resume uses the same run/session id.
- LangGraph interrupts are the right mental model for destructive-action gates:
  the workflow pauses, records the requested decision, waits for human input,
  then resumes from persisted state. Any side effect before an interrupt must be
  idempotent or already finished.
- AutoGen's SelectorGroupChat is useful for research-style dynamic collaboration,
  but it depends on speaker selection and termination conditions to avoid loops.
  SAK should borrow its explicit role descriptions and termination limits, not
  use free group chat as the default control plane for PC mutation.
- CrewAI's production guidance separates precise Flow control from autonomous
  Crews. SAK should do the same: workflow JSON owns the state machine, while
  subagents solve bounded tasks inside a phase.
- CrewAI Tasks require a clear description, expected output, assigned agent,
  scoped tools, optional context, optional human input, and optional output file.
  SAK subagent tasks should keep the same required shape.
- CrewAI Memory highlights provenance, read barriers, and drain-on-shutdown.
  SAK's session memory should append asynchronously only if reads are guarded by
  a flush barrier before prompt construction and before report generation.
- CAMEL Workforce uses decomposition, assignment, parallel execution, persisted
  task results, and failure recovery. This maps directly to SAK's overseer,
  phase planner, read-only parallel lane, run state, and recovery decisions.
- CAMEL role-playing workers are useful for debate, design review, or competing
  repair-path analysis. They should be optional critic/research phases, not a
  general replacement for deterministic workflow phases.
- MetaGPT's key idea is SOP-driven collaboration with structured intermediate
  artifacts. SAK workflows should be treated as technician SOPs: each phase must
  produce named evidence, decisions, artifacts, and acceptance checks.
- LlamaIndex documents three multi-agent options: built-in handoff workflow,
  orchestrator/subagents-as-tools, and custom planner. It recommends the
  orchestrator pattern when the app needs one place to control sequence and
  custom logic. This is the closest match for SAK.
- AutoGPT's block model is useful as a UI/product reference: users understand
  visible blocks, inputs, conditions, and outputs. SAK should keep workflows
  inspectable as phase cards/details rather than hiding behavior inside prompts.

Design conclusion:

SAK should implement **Overseer + bounded subagents-as-tools** first. Handoffs can
be added later as an optional routing mode, but the main production path should
keep the overseer in control of user interaction, risky actions, cancellation,
budget, reporting, and cleanup.

### Open-Source Harness Decisions

| Harness | Useful Pattern | SAK Decision |
| --- | --- | --- |
| OpenAI Agents SDK | Agents, tools, handoffs, sessions, tracing, guardrails | Keep the Responses client and native C++ loop; mirror guardrails and traces locally. |
| LangGraph | Explicit graph state, checkpointing, interrupts, state-based handoffs | Use `run_state.json`, phase states, human gates, and resumable run ids. |
| AutoGen | Specialist role descriptions, selector/group chat, termination conditions | Use clear agent descriptions and hard stop conditions; avoid default free-form group chat for mutating work. |
| CrewAI | Flow controls state; Crews solve delegated work; Tasks define expected output | Workflow JSON remains the source of truth; subagent tasks are typed and bounded. |
| CAMEL Workforce | Decompose, assign, execute, persist result, recover failure | Add overseer recovery decisions and structured failure records per phase. |
| MetaGPT | SOPs plus role-specific artifacts | Treat each SAK workflow as a technician SOP with evidence and deliverables. |
| LlamaIndex | Orchestrator-as-tools for control, handoffs for fast prototypes | Keep subagents as tools under one overseer. |
| AutoGPT | Blocks, conditions, visible workflow construction | Keep workflows inspectable and phase-based; no external low-code runtime. |

SAK should not vendor any of these runtimes into v1. The app already has the
hard part these harnesses wrap: local Windows tools, package manager actions,
offline downloader actions, elevated command routing, session storage, and a Qt
UI. Adding a Python/Node orchestrator would split authority over the PC. The
right production move is to borrow the architecture patterns and keep the
runtime native.

### Historical Phase Tracker

The source-of-truth roadmap is now **Research-Derived Execution Roadmap** above.
This phase tracker remains as historical detail and should be kept in sync when
milestone work is completed.

This turns the research into concrete engineering work. Priority order matters:
each layer gives the next layer durable state, safety gates, and testable
behavior.

#### Phase A: Observability and Run Control

Status: Complete for v1.

- Complete: `trace.jsonl` stores model/tool/run spans.
- Complete: `run_state.json` stores current workflow/run status.
- Complete: `activity.jsonl` stores normalized activity events for overseer,
  subagent, tool, approval, memory, report, and cleanup work.
- Complete: workflow phase completions write normalized activity events and
  update the durable run-state snapshot.
- Complete: generated session reports include an Activity section rebuilt from
  `activity.jsonl`.
- Complete: themed Run Details view reads `activity.jsonl` and shows active or
  recent run state, phase timeline, and activity timeline in the existing right
  rail without adding a separate tab.

Acceptance:

- Every model call, workflow phase, subagent result, tool call, cancel, failure,
  approval, report, and cleanup step has one durable activity event.
- User-visible status and generated reports can be rebuilt from session files.
- Stop produces `cancelled` activity for active model/tool/workflow work.

#### Phase B: Workflow Input Clarification

Status: Complete for v1.

- Complete: before launching a workflow, validate required values from the
  workflow schema.
- Complete: if a required string/list/path value is missing, ask one focused
  question or path prompt before starting the workflow.
- Complete: resolved workflow inputs are stored in transcript metadata and in
  the `activity.jsonl` stream.
- Complete: missing-input waits are stored as durable `waiting_for_human`
  run-state snapshots plus `workflow_input` activity events while the modal is
  open.
- Complete: resume same workflow/run after answer rather than starting a new
  uncontrolled run.

Acceptance:

- `Install App Now` does not guess package when product name is ambiguous.
- `Build Offline Deployment Bundle` asks for app list/destination when missing.
- `Clean Uninstall` asks for exact target when multiple apps match.

#### Phase C: Memory Model v2

Status: Complete for v1.

- Complete: keep `memory.md` as human-readable source of truth.
- Complete: new sessions initialize generated sections: `Pinned Facts`,
  `Current Task`, `Decisions`, `Open Questions`, `Artifacts`,
  `Resolved History`.
- Complete: legacy and trimmed memory are preserved under resolved history.
- Complete: compaction preserves pinned/open/current/decision/artifact sections
  before trimming resolved history.
- Complete: prompt construction and generated reports read the working memory
  before continuing.

Acceptance:

- Long sessions keep important task facts after trimming.
- Subagents receive current task state without replaying whole transcript.
- Reports list decisions and open questions from memory.

#### Phase D: Recovery and Human Gates

Status: Core active recovery and durable gate resume complete for v1.

- Complete: added reusable recovery classifier for phase failure.
- Complete: orchestrator records recovery decisions on failed phase metadata.
- Complete: retry action re-executes once and preserves first-failure metadata.
- Complete: degraded-continue action keeps failure flags for fallback phases
  while avoiding an abort.
- Complete: ask-human action returns durable `waiting_for_human` run status and
  preserves the failure reason.
- Complete: reassign action routes critic/review failure to the overseer phase
  path and stores reassignment metadata.
- Complete: abort action still stops the workflow and runs eligible always-run
  cleanup phases.
- Complete: command approvals, restore-point offers, restore-point creation,
  restore-point skips, restore-point failures, and continue-after-failure
  decisions write durable activity events and run-state snapshots.
- Complete: OpenAI tool-turn approval and restore-point gates persist pending
  tool state and resume the same run beyond the original modal gate.
- Complete: workflow-recovery human answers can resume the same orchestrator
  run from the next phase without rerunning prior phases.

Acceptance:

- Tool denial, invalid JSON, timeout, package ambiguity, source fallback, and
  partial repair all produce deterministic recovery behavior.
- Cancel during approval cancels child work and writes partial-state note.

#### Phase E: Workflow Catalog Hardening

Status: Complete for v1.

- Complete: each seeded workflow now has report,
  verification/review, and always-run cleanup structure.
- Complete: risky workflows include approval/restore-point gate language.
- Complete: CI eval asserts seeded workflow report/verify/cleanup shape,
  acceptance criteria, and risky approval/restore gates.
- Complete: package ambiguity resolver and unit coverage prevent first-result
  guessing for install/download workflow tool phases.
- Complete: offline-installer fallback eval proves web research runs only after
  the built-in SAK downloader path fails.
- Complete: install workflow fallback eval proves official-source research runs
  after SAK package lookup failure and the install step is skipped.
- Complete: expanded the catalog with network connectivity repair, startup
  performance triage, and printer troubleshooting workflows.
- Complete: added eval fixtures for cancelled workflows and cleanup/report
  failure branches.

Acceptance:

- CI catches workflow missing report/cleanup/verification.
- Offline installer and install workflows prove SAK built-in tools run before
  web fallback.
- Each workflow states required software, safety gates, artifacts, and cleanup.

#### Phase F: Real-Model and Tool Evals

Status: Complete for v1.

- Complete: opt-in live Responses smoke test.
- Complete: manual smoke checklist covers opt-in workflow planner behavior,
  harmless package/offline downloader queries, live model call, gate resume,
  cleanup, and release checks.

Acceptance:

- Normal CI never spends tokens.
- Local opt-in tests prove saved key, model list, response creation, token usage,
  workflow prompt quality, and SAK-first behavior.

### Agent Activity Model

Every overseer, subagent, tool, and approval step should write a normalized
activity event to the trace store and a short user-readable line to the shared
app log.

States:

- `queued`: work accepted but not started.
- `briefing`: context packet and prompt are being built.
- `running_model`: model request is active.
- `running_tool`: local tool, package manager, offline downloader, download, or
  screenshot action is active.
- `waiting_human`: approval, missing input, restore-point decision, or conflict
  resolution is waiting on the user.
- `verifying`: acceptance checks are running.
- `reporting`: final report or handoff summary is being generated.
- `cleaning`: cleanup phase is running.
- `complete`: phase/run finished successfully.
- `failed`: unrecovered failure.
- `cancelled`: user Stop or parent cancellation reached this unit.

Minimum event fields:

```json
{
  "timestamp": "2026-05-13T20:00:00Z",
  "session_id": "session_uuid",
  "run_id": "run_uuid",
  "parent_id": "phase_or_run_id",
  "activity_id": "agent_or_tool_span",
  "kind": "overseer | subagent | tool | approval | memory | report | cleanup",
  "workflow_id": "download_offline_installer",
  "phase_id": "direct_download",
  "agent_id": "package_agent",
  "state": "running_tool",
  "summary": "Downloading Chrome offline installer through SAK offline downloader.",
  "tool_name": "sak_offline_downloader",
  "token_usage": {"input": 0, "output": 0, "total": 0},
  "artifact_refs": ["artifacts/My Chat/downloads/chrome.exe"],
  "evidence_refs": ["trace:span_123", "command:cmd_004"],
  "question_for_human": "",
  "recovery_action": "",
  "error": ""
}
```

Rules:

- The overseer owns the only user-facing transcript answer.
- Subagents never start local mutating tools directly; they request a tool phase
  or return a recommendation to the overseer.
- Read-only delegate phases may run in parallel.
- Mutating tool phases require the single-writer lease lane.
- Risky/destructive phases present the restore-point option before proceeding.
- Cancellation walks the token tree from run to phase to subagent/tool.
- A cancelled run keeps artifacts and writes a partial-state report note.
- Every failure becomes one of: retry, reassign, degraded continue, ask human,
  or abort.

### Subagent Task Contract

Each delegated task must be complete enough that a subagent can work without
guessing:

- `objective`: one bounded outcome.
- `role`: exact specialist role.
- `workflow_id` and `phase_id`: traceable source.
- `context_packet`: current user request, relevant memory, attached files,
  previous phase outputs, and artifact references.
- `instructions`: Markdown instruction file ids plus any user instruction chips.
- `skills`: skill ids that describe how to do the work.
- `tool_policy`: read-only, mutating-with-lease, research-only, report-only, or
  no-tools.
- `expected_output`: JSON schema plus a plain-language completion definition.
- `budget`: max wall-clock, max model turns, max tokens, max tool calls.
- `stop_condition`: complete, blocked, failed, cancelled, or ask-human.
- `verification`: acceptance checks the overseer will run against the result.

### Workflow Catalog Behavior Matrix

| Workflow | Agent Shape | Built-In Tool Priority | Human Gate | Completion Requirements |
| --- | --- | --- | --- | --- |
| Full PC Health Check | diagnostic, critic, report | read-only PowerShell and SAK diagnostics first | ask before any repair suggestion becomes action | evidence collected, critic pass, report/cleanup recorded |
| Drive Health Deep Check | diagnostic, repair | SMART/PowerShell evidence before chkdsk repair | ask before `chkdsk /f`, firmware tools, or destructive tests | drive inventory, health summary, risk level, report |
| Download Offline Installer | package, research, report | SAK offline downloader first, package manager second, web fallback third | ask if vendor fallback or unsigned/non-official source is needed | exact path, source, hash, cleanup |
| Install App Now | package, report | SAK package manager first, offline bundle second, web fallback only if needed | restore-point option before install/upgrade | installed-version verification and report |
| Build Offline Deployment Bundle | package, report | SAK offline downloader `build_bundle` first | ask before large downloads or destination overwrite | manifest, hashes, bundle path, cleanup |
| Windows Update Repair | diagnostic, repair, critic | read-only logs first, repair commands through lease | restore-point option before component reset/repair | update evidence, repair result, verification, critic |
| BSOD Investigation | diagnostic, research, critic | event logs and dump metadata first | ask before installing analyzers or collecting large dumps | bugcheck summary, likely cause, next tests |
| Clean Uninstall | diagnostic, package, cleanup | package manager uninstall first, leftover scanner second | restore-point option before uninstall and leftover removal | uninstall result, leftover review, final verification |
| Security Advisory Check | research, critic | official/vendor/security sources first | ask before downloading tools or changing system | cited advisory brief and risk summary |
| Technician Service Report | report, critic | local transcript/trace/artifacts first | ask if redaction choices are ambiguous | report draft, redaction pass, final report path |

### Workflow Recovery Matrix

| Failure | Overseer Behavior |
| --- | --- |
| Missing required input | Pause in `waiting_human` and ask for the smallest missing detail. |
| Package ambiguity | Ask package agent for ranked candidates; show top choices to human if install/download is mutating or costly. |
| SAK package/offline tool no result | Record SAK-first attempt, then allow research/web fallback. |
| Tool denied by policy | Return denial to overseer, select safer tool, request approval, or abort. |
| Model invalid JSON | Retry once with schema-only repair prompt, then fail the phase. |
| Subagent timeout | Cancel child token, preserve partial summary, retry/reassign if safe. |
| Conflicting subagent results | Run critic phase, then ask human if conflict affects mutation. |
| Partial install/uninstall/repair | Stop parallel work, collect state, offer report, ask whether to continue, roll back, or abort. |
| Report generation fails | Keep markdown fallback generated from transcript and trace. |
| Cleanup fails | Preserve artifacts, log cleanup debt, include it in final answer/report. |

### Real Model Smoke Testing

Normal CI must not spend API tokens. Live model calls are opt-in:

```powershell
$env:SAK_AI_REAL_MODEL_TEST = "1"
# Optional. If omitted, the test uses OPENAI_API_KEY, then SAK_OPENAI_API_KEY,
# then the saved encrypted app-directory key.
$env:SAK_AI_REAL_MODEL = "gpt-5.4-mini"
ctest --test-dir build -C Release -R test_openai_responses_client --output-on-failure
```

Expected behavior:

- If `SAK_AI_REAL_MODEL_TEST` is not `1`, the live test skips.
- If no key is found, the live test skips without printing secrets.
- If a key exists, the test lists models, picks a chat-capable GPT model, sends a
  minimal Responses API prompt, verifies the sentinel response, and verifies
  token usage is returned.
- Failures redact bearer/API-key-like text before reaching test output.

### Steering and Overlapping Prompts

If the user submits a follow-up prompt while another task is in progress, SAK
must not silently launch a second independent workflow that can fight the first
one. The safe default is:

1. Record the new prompt as **Steering** in transcript, trace, and `memory.md`.
2. Inject steering into the next model/tool-continuation turn when possible.
3. Keep current local command/offline worker leases intact until they finish or
   the user presses Stop.
4. Present explicit choices: apply steering, queue after this run, or stop and
   queue the new request.

This mirrors the manager-agent design: the overseer owns one active run, one
tool queue, and one mutating lease lane. Steering changes the overseer's
instructions; it does not create uncontrolled parallel mutation.

### Session Memory Files

Each chat session should own a central working-memory file:

- Location: `<session>/memory.md`.
- Purpose: concise facts and run continuity for the overseer and subagents.
- Contents: user requests, steering messages, selected workflow, key findings,
  important tool results, artifacts, decisions, open questions, and final
  outcome.
- Prompt use: injected below stable system/tool policy instructions and below
  current attached context, never treated as higher-priority instructions.
- Growth policy: append-only for v1, later compact into sections:
  `Pinned Facts`, `Current Task`, `Decisions`, `Open Questions`, `Artifacts`,
  `Resolved History`.

Session artifacts should live under a directory named after the chat session:
`<session>/artifacts/<chat name>/...`. The immutable session id remains the
database/storage key; the chat title is the technician-facing artifact folder.

---

## Product Goals

- Convert "prompt library" into a workflow library.
- Let a workflow insert:
  - main task prompt
  - phase-specific prompts
  - Markdown instruction files
  - SAK skills
  - required software checks
  - tool priority rules
  - safety gates
  - troubleshooting branches
  - verification steps
  - report and cleanup steps
- Let the overseer delegate to subagents while preserving a single user-facing
  chat experience.
- Make subagent activity visible without clutter:
  - status bar summary
  - collapsible run details
  - shared app log entries
  - artifacts dropdown
  - final report section
- Make cancellation complete and predictable.
- Make failures actionable:
  - retry
  - reassign to different agent
  - continue with degraded mode
  - ask the human
  - abort safely
- Preserve arbitrary full-access capability, but route risky changes through
  overseer policy, restore-point prompts, and durable audit logs.

---

## Non-Goals

- Do not replace the existing single-agent flow immediately.
- Do not add Python, Node, or an external agent framework runtime to the app.
- Do not let every subagent freely mutate the PC in parallel.
- Do not make "skills" executable code by default. Skills are instruction and
  workflow knowledge unless explicitly tied to existing SAK tools or approved
  local commands.
- Do not require cloud-hosted Agent Builder workflows. SAK remains a local C++ app
  using the Responses API and local orchestration.

---

## Core Design

### Agent Roles

The system has one overseer and many specialist subagents.

#### Overseer Agent

The overseer is the only agent that directly talks to the human by default.

Responsibilities:

- Understand the user's goal.
- Select or refine a workflow.
- Decompose the task into phases.
- Delegate bounded subtasks to subagents.
- Track subagent progress, budget, confidence, artifacts, and failures.
- Decide whether to retry, reassign, continue, or ask the human.
- Own risky system-changing decisions.
- Own final synthesis, user updates, report generation, and cleanup.
- Cancel all children when the user presses Stop.

#### Subagents

Subagents are scoped workers. They return structured results to the overseer.

Default subagents:

- `diagnostic_agent`
  - Reads system state, logs, hardware status, installed apps, services, event
    logs, and command output.
- `research_agent`
  - Uses web search and official/vendor docs. Prefers primary sources.
- `package_agent`
  - Uses `sak_package_manager` and `sak_offline_downloader` first for software
    search, install, uninstall, direct downloads, and offline bundles.
- `repair_agent`
  - Proposes repair steps and can execute approved or leased repair actions.
- `security_agent`
  - Reviews suspicious behavior, Defender state, startup entries, downloads,
    network listeners, persistence points, and risk.
- `report_agent`
  - Converts trace, findings, citations, command output, and artifacts into
    technician/customer reports.
- `cleanup_agent`
  - Identifies temporary files, downloaded tools, logs, scheduled tasks,
    services, restore points, and changes that need cleanup or handoff notes.
- `critic_agent`
  - Evaluates the plan/output against workflow acceptance criteria before the
    overseer gives the final answer.

Subagents should not all be active for every workflow. Workflows declare the team
they need.

### Preferred Pattern: Agents as Tools

Each subagent is exposed to the overseer as a local tool:

```text
delegate_to_agent(agent_id, task, context_refs, expected_output_schema,
                  tool_policy, timeout_seconds, token_budget)
```

The overseer receives structured output, not freeform chat:

```json
{
  "status": "complete | failed | blocked | cancelled",
  "summary": "...",
  "findings": [],
  "actions_taken": [],
  "artifacts": [],
  "citations": [],
  "risks": [],
  "questions_for_human": [],
  "recommended_next_steps": [],
  "confidence": 0.0
}
```

Why this pattern:

- One agent keeps control of human interaction.
- The overseer can compare conflicting subagent results.
- Risky tools can be centralized.
- Cancellation and trace accounting are easier.
- SAK can add subagents without changing the user-facing chat model.

### Optional Later Pattern: Handoffs

Handoffs can be added later for specialized "take over this chat" situations, such
as a guided report writer or guided uninstall assistant. If added, handoff tools
must still route through the run controller so cancellation, budget, and traces
remain global.

---

## Workflow Library

The prompt library becomes a workflow template library.

### Workflow Template Requirements

Each workflow must define:

- ID, title, role, category, description.
- User-facing task starter prompt.
- Required context types:
  - screenshots
  - logs
  - documents
  - instruction Markdown
  - app/package names
  - target paths/drives
- Required software and SAK tools.
- Agent team and tool permissions.
- Phase list.
- Per-phase prompts and instruction files.
- Branching/troubleshooting rules.
- Risk and restore-point rules.
- Acceptance criteria.
- Report format.
- Cleanup plan.
- Cancellation plan.

### Proposed JSON Shape

```json
{
  "schema_version": 1,
  "id": "offline_installer_download",
  "title": "Download Offline Installer",
  "role": "Software Deployment Technician",
  "category": "Software",
  "description": "Find and download offline installer artifacts using SAK tools first.",
  "starter_prompt": "Download an offline installer for {{app_name}}.",
  "required_inputs": [
    {"id": "app_name", "label": "Application name", "type": "string", "required": true}
  ],
  "required_software": [
    {
      "id": "sak_offline_downloader",
      "kind": "sak_tool",
      "required": true,
      "install_policy": "built_in_only"
    },
    {
      "id": "chocolatey_portable",
      "kind": "bundled_tool",
      "required": true,
      "install_policy": "already_bundled"
    }
  ],
  "instructions": [
    "resources/ai/instructions/software_install_policy.md",
    "resources/ai/instructions/offline_downloader_priority.md"
  ],
  "skills": [
    "software-package-selection",
    "artifact-verification",
    "customer-handoff-report"
  ],
  "agents": [
    {
      "id": "package_agent",
      "model_policy": "fast_reasoning",
      "tool_policy": "package_tools_only",
      "token_budget": 8000
    },
    {
      "id": "research_agent",
      "model_policy": "web_research",
      "tool_policy": "web_read_only",
      "token_budget": 12000
    },
    {
      "id": "report_agent",
      "model_policy": "writer",
      "tool_policy": "no_local_execution",
      "token_budget": 6000
    }
  ],
  "phases": [
    {
      "id": "clarify",
      "type": "overseer",
      "prompt": "Confirm app name, edition, architecture, and offline use case if missing.",
      "completion": "All required inputs are known or the human chose defaults."
    },
    {
      "id": "package_search",
      "type": "delegate",
      "agent": "package_agent",
      "prompt": "Search SAK package manager/offline downloader for candidate packages.",
      "expected_output": "package_candidates"
    },
    {
      "id": "direct_download",
      "type": "tool_action",
      "tool": "sak_offline_downloader",
      "operation": "direct_download",
      "risk": "download_only",
      "completion": "Installer files are saved to artifacts with paths and hashes."
    },
    {
      "id": "fallback_research",
      "type": "delegate",
      "agent": "research_agent",
      "condition": "direct_download_failed",
      "prompt": "Find official vendor offline installer sources and checksum guidance."
    },
    {
      "id": "report",
      "type": "delegate",
      "agent": "report_agent",
      "prompt": "Create customer-ready download summary with file paths, hashes, and sources."
    },
    {
      "id": "cleanup",
      "type": "cleanup",
      "prompt": "Remove temp extraction dirs, keep requested artifacts, and report anything left."
    }
  ],
  "acceptance_criteria": [
    "Built-in SAK downloader/package path was tried before web fallback.",
    "Artifacts include path, size, and SHA-256 where available.",
    "If vendor web fallback was used, source URLs are official or clearly flagged.",
    "Final response states what was downloaded and where."
  ],
  "cancel_policy": {
    "cancel_children": true,
    "cancel_tools": true,
    "preserve_partial_artifacts": true,
    "report_partial_state": true
  }
}
```

### Storage

Built-in resources:

```text
resources/ai/workflows/*.json
resources/ai/instructions/*.md
resources/ai/skills/*.md
resources/ai/schemas/*.json
```

Portable user overrides:

```text
<app>/data/ai/workflows/*.json
<app>/data/ai/instructions/*.md
<app>/data/ai/skills/*.md
```

Session copies:

```text
<session>/workflow.json
<session>/instructions.jsonl
<session>/skills.jsonl
<session>/trace.jsonl
<session>/subagents.jsonl
<session>/artifacts/
```

The workflow chosen for a run must be copied into the session folder so reports
remain reproducible even after templates change.

---

## Skill System

SAK skills are not executable by default. They are versioned instruction bundles
used by workflows and agents.

Skill file:

```text
resources/ai/skills/drive-health-diagnostics.md
```

Suggested front matter:

```yaml
---
id: drive-health-diagnostics
version: 1
title: Drive Health Diagnostics
applies_to:
  - diagnostic_agent
  - repair_agent
required_tools:
  - run_powershell
  - take_screenshot
  - smartctl
risk_level: read_only
---
```

Body:

- Goals.
- What evidence to collect.
- Preferred commands/tools.
- What not to do.
- How to interpret output.
- Escalation/stop conditions.
- Verification.
- Report notes.

Skill load order:

1. System safety and SAK execution policy.
2. Agent base instructions.
3. Workflow instructions.
4. Skill instructions.
5. User-attached Markdown instruction chips.
6. User task.
7. Untrusted evidence and web content.

Skills should be inspectable in the UI before run start.

---

## Required Software Handling

Workflow templates define required software/tool capabilities. Before phase
execution, the overseer runs a requirements check.

Requirement kinds:

- `sak_tool`
  - Examples: `sak_package_manager`, `sak_offline_downloader`, `take_screenshot`.
- `bundled_tool`
  - Examples: Chocolatey portable, smartctl, aria2c, 7z.
- `windows_component`
  - Examples: PowerShell, DISM, SFC, Defender, Event Viewer APIs.
- `external_package`
  - Examples: vendor diagnostic utilities.

Install policies:

- `built_in_only`
  - If missing, fail or degrade; do not download.
- `already_bundled`
  - Expected under app `tools/`; report missing bundle.
- `ask_before_install`
  - Ask human before installing.
- `auto_install_in_unattended`
  - Only if access mode allows it and workflow risk policy permits it.
- `manual_only`
  - Tell human what is needed.

Requirement check result:

```json
{
  "status": "ready | degraded | blocked",
  "missing": [],
  "available": [],
  "degraded_modes": [],
  "questions_for_human": []
}
```

The requirement checker should run before expensive model calls when possible.

---

## Tool Permission Model

Subagents get narrow tool policies. The overseer grants leases for risky actions.

Tool policy examples:

- `no_local_execution`
  - web/search/context only.
- `read_only_pc`
  - read-only PowerShell, screenshots, logs, inventory.
- `package_tools_only`
  - SAK package/offline tools only.
- `download_only`
  - `download_file`, `sak_offline_downloader(direct_download)`.
- `mutating_requires_lease`
  - Subagent can propose actions; overseer must lease execution.
- `exclusive_mutating_executor`
  - One agent at a time can run system-changing commands.

Leases:

```json
{
  "lease_id": "lease_001",
  "agent_id": "repair_agent",
  "tool_scope": ["run_powershell", "sak_package_manager"],
  "risk_level": "system_change",
  "expires_at": "...",
  "restore_point_prompted": true,
  "human_approved": true
}
```

Rules:

- Multiple read-only subagents may run in parallel.
- Only one mutating tool lease can be active at a time.
- Restore-point prompt happens before the first risky lease in a session or when
  a workflow explicitly asks for a new restore point.
- The overseer can revoke a lease on cancel, timeout, contradiction, or human
  override.

---

## Orchestration Architecture

### New Components

```text
include/sak/ai/
  ai_workflow_template.h
  ai_workflow_store.h
  ai_agent_definition.h
  ai_orchestrator.h
  ai_subagent_runner.h
  ai_run_state.h
  ai_cancellation_token.h
  ai_trace_store.h
  ai_budget_manager.h
  ai_tool_policy.h
  ai_workflow_eval.h

src/ai/
  ai_workflow_template.cpp
  ai_workflow_store.cpp
  ai_orchestrator.cpp
  ai_subagent_runner.cpp
  ai_cancellation_token.cpp
  ai_trace_store.cpp
  ai_budget_manager.cpp
  ai_tool_policy.cpp
  ai_workflow_eval.cpp
```

### Responsibilities

`AiWorkflowStore`

- Load built-in workflow JSON.
- Load user workflow JSON.
- Validate schema.
- Merge/override by ID and version.
- Provide search/filter data for UI.

`AiOrchestrator`

- Own one top-level run.
- Build the overseer request.
- Execute workflow phases.
- Spawn subagent tasks.
- Track dependencies and phase status.
- Enforce budgets and max turns.
- Own cancellation tree.
- Emit status/log/trace events.

`AiSubagentRunner`

- Run one specialist model loop.
- Accept scoped context, instructions, tools, and output schema.
- Return structured result.
- Stream progress events to orchestrator.
- Respect cancellation.

`AiCancellationToken`

- Hierarchical token tree:
  - run token
  - phase tokens
  - subagent tokens
  - tool tokens
- Cancelling parent cancels children.
- Carries cancel reason and timestamp.

`AiTraceStore`

- Persist local trace spans:
  - run
  - phase
  - model call
  - subagent
  - tool call
  - guardrail
  - approval
  - artifact
  - cancellation
- Write JSONL to session folder.
- Emit summarized log lines to shared app log.

`AiBudgetManager`

- Per-run token budget.
- Per-subagent token budget.
- Per-workflow tool-call budget.
- Wall-clock budget.
- Max retry count.
- Max parallel subagents.
- Max mutating leases.

`AiToolPolicy`

- Validate every tool call before dispatch.
- Enforce access mode.
- Enforce workflow policy.
- Enforce subagent permissions.
- Detect risky changes.
- Route restore-point prompt and human approval.

`AiWorkflowEval`

- Run rule-based checks and optional model-based critique.
- Check acceptance criteria.
- Flag missing citations, missing verification, no cleanup, no report, or
  untried SAK-first tool policy.

### Current Code Impact

Current relevant surfaces:

- `include/sak/ai_assistant_panel.h`
- `src/gui/ai_assistant_panel.cpp`
- `include/sak/ai/openai_responses_client.h`
- `src/ai/openai_responses_client.cpp`
- `include/sak/ai/ai_execution_broker.h`
- `src/ai/ai_execution_broker.cpp`
- `include/sak/ai/ai_conversation_store.h`
- `src/ai/ai_conversation_store.cpp`

Expected refactor:

- Move current prompt-template list out of `AiAssistantPanel` into
  `AiWorkflowStore`.
- Keep simple prompt insertion as a compatibility mode.
- Move tool dispatch from `AiAssistantPanel::dispatchNextToolCall()` toward an
  `AiToolDispatcher` that can be used by overseer and subagents.
- Change `OpenAIResponsesClient` from one-reply-at-a-time to either:
  - a stateless request object per model call, or
  - a pooled runner that can support parallel subagents.
- Keep UI wiring in the panel, but move run state to `AiOrchestrator`.

---

## Cancellation Model

Cancellation must be explicit, recursive, and auditable.

### States

Run state:

```text
Idle
Planning
Running
WaitingForHuman
Cancelling
Cancelled
Completed
Failed
```

Subagent state:

```text
Queued
Starting
RunningModel
WaitingToolApproval
RunningTool
WaitingChild
Cancelling
Cancelled
Completed
Failed
TimedOut
```

Tool state:

```text
Queued
Running
Cancelling
Cancelled
Completed
Failed
TimedOut
```

### Stop Button Behavior

When user presses Stop:

1. Set run token to cancelled with reason `user_cancelled`.
2. Disable new model requests, subagent starts, and tool dispatch.
3. Cancel active OpenAI replies.
4. Cancel queued subagents without starting them.
5. Cancel running subagents.
6. Cancel local commands through `AiExecutionBroker::cancel()`.
7. Cancel elevated task through `ElevationBroker::cancelCurrentTask()`.
8. Cancel `OfflineDeploymentWorker`.
9. Cancel package manager jobs where possible; otherwise mark as
   `cancelling_waiting_for_process`.
10. Resolve pending approval dialogs as cancelled if they belong to the run.
11. Flush stdout/stderr buffers and artifacts.
12. Write trace cancellation spans.
13. Append transcript system note:
    `Run cancelled by user. Partial artifacts preserved.`
14. Update global status bar:
    `AI: Cancelled | Turn: ... | Session: ... | Tools: ...`

### Cancellation Output to Overseer

If cancellation occurs while the model loop is still active, do not continue
asking the model for synthesis unless the user explicitly asks for a partial
summary. The panel should show a local cancellation summary from run state.

Partial summary:

- What was running.
- What completed.
- What was cancelled.
- What artifacts exist.
- What system-changing actions may have partially completed.
- Whether manual follow-up is needed.

### Edge Cases

- If a tool ignores cancel, wait a short grace period, then terminate process if
  local and safe to kill.
- If an installer is mid-flight, do not assume rollback succeeded. Report partial
  state and ask the user before cleanup.
- If an elevated command cannot be interrupted, mark it `cancel_requested` and
  continue monitoring until it returns or times out.
- If app exits while cancelling, session trace must preserve latest known state.
- If network request cancellation returns no final model output, store a synthetic
  cancellation event instead of treating it as a model error.

---

## Failure Handling

The overseer monitors every child result.

Failure classes:

- Model request failed.
- Model invalid JSON/schema mismatch.
- Subagent timed out.
- Subagent exceeded token/tool budget.
- Tool call rejected by policy.
- Tool failed.
- Required software missing.
- Research source quality too low.
- Subagent results conflict.
- Action partially completed.
- Cleanup failed.
- Report criteria not met.

Overseer actions:

- Retry same subagent with corrected instructions.
- Reassign to a different subagent.
- Run a critic/evaluator pass.
- Ask human for missing input or permission.
- Continue in degraded mode.
- Abort workflow.

The overseer should ask the human when:

- Required input is ambiguous and reasonable defaults are risky.
- A required tool/software capability is missing.
- Two subagents disagree on a system-changing action.
- The next step may destroy data or make rollback difficult.
- A partial install/uninstall/repair needs manual judgment.

---

## UI Plan

### Right Rail

Replace `Common task prompt` with `Workflow`.

Controls:

- Workflow search/combo.
- Role filter.
- "Preview" / "Details" popover.
- "Start workflow" or "Insert starter" behavior:
  - If message box is empty, insert starter prompt.
  - If message exists, attach workflow to current message.
- Workflow status:
  - phase
  - active subagents
  - waiting for human / running / cancelling

### Composer

Existing context chips remain.

Add workflow chips:

- Workflow chip.
- Instruction chips auto-loaded by workflow.
- Skill chips auto-loaded by workflow.
- Required software status chip when degraded.

Color coding:

- Workflow: primary/accent.
- Instruction: info.
- Skill: success.
- Evidence/context: neutral/document colors.
- Missing/degraded requirement: warning.

### Chat Header

Keep artifacts dropdown and Report button.

Add collapsible "Run" dropdown:

- phases
- subagents
- active tool calls
- warnings
- budget
- cancellation state

### Shared Log

Log events:

- Workflow selected.
- Requirement check start/finish.
- Subagent start/end/fail/cancel.
- Tool start/end/fail/cancel.
- Human approval requested/result.
- Restore point offered/result.
- Cleanup start/end.

### Status Bar

Status detail should include:

```text
AI: Running | Phase: Diagnose | Agents: 2 active/5 done | Turn: ... | Session: ... | Tools: ...
```

---

## Workflow Catalog for First Implementation

### PC Technician

- Full PC Health Check
- Drive Health Deep Check
- Slow PC Triage
- BSOD Investigation
- Event Log Sweep
- Network Connectivity Check
- Printer Repair
- Battery/Laptop Health
- Thermal/Performance Triage
- Post-Repair Verification

### Windows Repair Technician

- Windows Update Repair
- Component Store Repair
- SFC/DISM Repair
- Boot Failure Triage
- Profile/Login Repair
- Store/UWP Repair
- Network Stack Repair
- Domain/VPN Connectivity
- Service Startup Failure
- Restore/Rollback Planning

### Software Deployment Technician

- Install App Now
- Download Offline Installer
- Build Offline Deployment Bundle
- Install From Offline Bundle
- Upgrade Existing App
- Clean Uninstall
- Repair Failed Installer
- Runtime Prerequisites
- Driver Installer Workflow
- Vendor Tool Verification

### Research Assistant

- Official Procedure Research
- Exact Error Code Research
- Security Advisory Check
- Vendor Driver Advisory
- Known Issue Timeline
- Compare Repair Paths
- Installer Source Verification
- End-of-Life / Support Status
- Forum Evidence Synthesis
- Citation Brief

### Customer Report Writer

- Technician Service Report
- Customer Handoff Summary
- Executive Risk Summary
- Evidence Appendix
- Before/After Report
- Incident Timeline
- Escalation Packet
- Command Audit Recap
- Maintenance Recommendations
- Compliance-Friendly Summary

---

## Example End-to-End Workflow

User asks:

```text
Download an offline installer for Google Chrome.
```

Flow:

1. Overseer selects `offline_installer_download`.
2. Requirement checker verifies SAK offline downloader and bundled Chocolatey.
3. Package agent searches SAK package/offline tooling for `google chrome`.
4. Package agent returns candidate `googlechrome`.
5. Overseer uses `sak_offline_downloader(direct_download)` before web fallback.
6. If direct download succeeds:
   - save artifacts
   - compute/report paths/hashes
   - run optional Defender scan if workflow enables it
7. If direct download fails:
   - research agent finds official vendor enterprise/offline installer page
   - overseer uses `download_file` only after explaining fallback
8. Report agent creates final summary.
9. Cleanup agent removes temp extraction folders, keeps requested artifacts.
10. Overseer final answer states exact file paths, hashes, source, and any caveat.

Acceptance:

- SAK-first rule was followed.
- Artifacts are in the session dropdown.
- Report includes source and hash.
- Cleanup status is recorded.

---

## Data Contracts

### Subagent Task

```json
{
  "task_id": "task_001",
  "run_id": "run_20260513_001",
  "workflow_id": "drive_health_deep_check",
  "phase_id": "collect_evidence",
  "agent_id": "diagnostic_agent",
  "objective": "Collect read-only disk health evidence.",
  "context_refs": ["ctx_001", "artifact_002"],
  "instructions_refs": ["drive-health-diagnostics"],
  "tool_policy": "read_only_pc",
  "timeout_seconds": 600,
  "token_budget": 8000,
  "expected_output_schema": "diagnostic_findings_v1"
}
```

### Subagent Result

```json
{
  "task_id": "task_001",
  "agent_id": "diagnostic_agent",
  "status": "complete",
  "summary": "Drive 0 reports healthy SMART status but has recent NTFS warnings.",
  "findings": [
    {
      "severity": "warning",
      "title": "Recent NTFS warning",
      "evidence_refs": ["cmd_003_stdout"],
      "recommendation": "Run read-only chkdsk scan before any repair."
    }
  ],
  "artifacts": ["artifacts/logs/cmd_003_stdout.txt"],
  "citations": [],
  "actions_taken": ["Get-PhysicalDisk", "Get-WinEvent"],
  "risks": [],
  "questions_for_human": [],
  "confidence": 0.82
}
```

### Trace Event

```json
{
  "timestamp": "2026-05-13T14:20:00Z",
  "run_id": "run_20260513_001",
  "parent_span_id": "span_run",
  "span_id": "span_task_001",
  "kind": "subagent",
  "name": "diagnostic_agent",
  "status": "completed",
  "duration_ms": 48120,
  "token_usage": {
    "input": 1200,
    "output": 620,
    "total": 1820
  },
  "metadata": {
    "workflow_id": "drive_health_deep_check",
    "phase_id": "collect_evidence"
  }
}
```

---

## Milestones

### Milestone 1: Workflow Schema and Store

Status: Complete for v1 parsing/loading. JSON Schema file validation can be
added later if we want external editor validation; current C++ validation is
covered by tests.

Deliverables:

- `AiWorkflowTemplate` data model.
- JSON schema validator.
- Built-in workflow loader.
- User override loader.
- Unit tests for valid/invalid templates.

Files:

- `include/sak/ai/ai_workflow_template.h`
- `src/ai/ai_workflow_template.cpp`
- `include/sak/ai/ai_workflow_store.h`
- `src/ai/ai_workflow_store.cpp`
- `resources/ai/workflows/*.json`
- `tests/unit/test_ai_workflow_store.cpp`

Acceptance:

- App loads workflow catalog at startup.
- Invalid templates are skipped with log warnings.
- Built-in and user templates merge predictably.

### Milestone 2: Workflow UI

Status: Complete for v1 workflow scope. Workflow selector, non-modal inline
Details panel (phases, agents, requirements, instructions, skills, acceptance
criteria), Add button, and workflow/instruction/skill chips all exist. Phase
and active/done agent counts are surfaced in the status bar. A richer live run
dropdown with per-subagent state is a follow-up UX improvement.

Deliverables:

- Replace prompt combo with workflow selector.
- Details preview with phases, agents, required tools, risks, instructions, and
  cleanup plan.
- Workflow/instruction/skill chips in composer.

Files:

- `include/sak/ai_assistant_panel.h`
- `src/gui/ai_assistant_panel.cpp`

Acceptance:

- User can select a workflow and see what it will do.
- Workflow chips can be removed before sending.
- Existing manual prompt flow still works.

### Milestone 3: Trace Store and Run State

Status: Complete for v1 workflow scope. `AiTraceStore`, `AiRunState`, and
`AiRunStateStore` exist with unit tests. The panel writes trace events for
sessions, runs, model calls, tools, failures, cancellation, and workflow phase
completion, and persists a `run_state.json` snapshot on each state transition.

Deliverables:

- `AiTraceStore`.
- `AiRunState` with phase/subagent/tool state.
- Session files for `trace.jsonl` and `subagents.jsonl`.
- Status bar integration for phase/agent counts.

Acceptance:

- Every workflow run creates trace events.
- Shared app log receives short readable entries.
- Final report can include trace summary.

### Milestone 4: Cancellation Token Tree

Status: Complete for v1 workflow scope. Hierarchical `CancellationToken`
exists with unit tests. The panel creates a root run token on send, child
tokens per tool turn, per tool call, per workflow phase, and per subagent. Stop
cancels the active run token (which propagates to all children), the active
OpenAI request, command broker, offline worker, pending tool turn, and
orchestrator/subagent phases.

Deliverables:

- `AiCancellationToken`.
- Parent/child cancellation propagation.
- Cancel state persisted to trace.
- Tests for queued, running, and nested cancellation.

Acceptance:

- Stop cancels main request, subagents, local commands, offline worker, and
  pending approvals.
- Partial artifacts remain accessible.
- Status bar and transcript show cancelled state.

### Milestone 5: Tool Dispatcher and Tool Policy

Status: Complete for v1 workflow scope. `AiToolPolicy` and the new
`AiToolDispatcher` (with handler registry, policy gate, and structured
`policy_denied` / `handler_missing` results) both have unit tests. The panel
registers handlers for screenshot, download, package-manager, and
offline-downloader tools and routes every call through the dispatcher, which
evaluates the policy first. Shell tool calls also evaluate the policy before
the broker starts. Mutating package/offline downloader calls and arbitrary
shell/process calls take a single-writer `AiLeaseManager` lease before system
changes run.

Deliverables:

- Extract tool dispatch from panel into `AiToolDispatcher`.
- Add `AiToolPolicy`.
- Add subagent tool policies and mutating leases.
- Add unit tests for allowed/blocked/risky calls.

Acceptance:

- Read-only subagents can run safe diagnostics.
- Mutating calls require overseer lease.
- Restore-point prompt still occurs before risky changes.

### Milestone 6: Subagent Runner

Status: Complete for v1 workflow scope. The runner is wired to the production
OpenAI Responses model client for panel workflows and covered by fake-model
unit tests.

Deliverables:

- `AiSubagentRunner` with structured `AiSubagentTask` input and
  `AiSubagentResult` output (status/summary/findings/artifacts/citations/
  risks/questions/confidence/usage).
- Abstract `IAiModelClient` so the runner is testable with a fake; production
  implementation wraps `OpenAIResponsesClient` and ships when the orchestrator
  lands.
- Cancellation: parent-token check pre-invoke, agent-scoped child token
  cancelled-during-invoke detection, structured `Cancelled` result.
- Token-budget enforcement: usage > budget marks the result `Failed` with a
  budget-exceeded error message.
- JSON round-trip helpers on `AiSubagentTask`, `AiSubagentFinding`,
  `AiSubagentResult` so trace/persistence can store them verbatim.
- Unit tests cover complete, malformed-JSON, model-failure, parent-cancelled,
  cancel-during-invoke, exceeded-budget, and JSON round-trip.

Deferred v2 backlog:

- Per-subagent retry policy beyond the current basic retry count.
- Hard preemption for model calls that exceed wall-clock timeout. Cancellation
  is wired; timeout currently depends on the request returning or explicit
  cancellation.

Acceptance:

- Overseer can delegate one bounded subtask and receive structured output.
- Subagent cancellation works while model request is active.
- Failed schema output returns a useful error to overseer.

### Milestone 7: Orchestrator

Status: Complete for v1 workflow scope. Core orchestrator has unit tests
against fake subagent runner + fake tool executor and is wired into the panel
when a workflow chip is attached.

2026-06-04 quality pass: Codex docs/repo research reinforced the current shape:
explicit subagent workflows, read-heavy parallelism, summarized subagent output,
and serialized mutation. The chat prompt now states those rules directly, and
`test_ai_prompt_assembler`, `test_ai_orchestrator`, and `test_ai_workflow_evals`
cover the policy and wiring.

2026-06-04 second quality pass: the panel no longer caps production workflow
delegates at one. It uses per-subagent OpenAI client isolation and a conservative
three-subagent cap, while the orchestrator still batches only consecutive
read-only delegate phases and serializes mutating or conditional phases.

Deliverables:

- `AiOrchestrator` with phase engine, group planner, and result type
  (`AiOrchestratorResult` including per-phase execution records and flag set).
- Phase types: `overseer` (callable handler), `delegate` (via
  `AiSubagentRunner`), `tool_action`/`cleanup` (via injected
  `IAiToolExecutor`).
- Group planner batches consecutive `delegate` phases on read-only-policy
  agents up to `max_parallel_subagents`. Mutating policies and any phase with
  a non-empty `condition` start a fresh group so flag dependencies remain
  correct.
- Conditional phases: evaluated at group start against the live flag set
  (`<phase_id>_succeeded` / `<phase_id>_failed`); skipped phases emit
  `AiPhaseExecution{skipped=true}` records in original order.
- Cancellation: per-phase child token off the root, post-phase root-token
  check, and a delegate subagent reporting `Cancelled` flips the run status
  to `Cancelled` rather than `Failed`.
- Failure handling: `stop_on_phase_failure` (default true) halts the run with
  `Failed` status and surfaces the first phase id + error message.
- Parallel execution uses `std::async` futures; results are spliced back in
  original phase order so callers see a deterministic execution list.
- Unit tests cover sequential, parallel grouping, mutating serialization,
  condition-skip, condition-runs-fallback-on-failure, fail-stop, root-token
  cancel, overseer-handler invocation, and tool executor wiring.

Deferred v2 backlog:

- Richer per-phase retries and timeout policy.
- Additional guided clarification widgets beyond the current durable modal gate.

Acceptance:

- A workflow runs multiple phases end-to-end. Complete.
- Two read-only subagents can run concurrently. Complete.
- Risky action phases run one at a time. Complete.

### Milestone 8: Workflow Catalog v1

Status: Complete for initial catalog seed and v1 orchestrator execution. The
catalog supplies workflow phases, agents, required tools, instructions, skills,
acceptance criteria, and cancellation policies to the live panel workflow path.

Deliverables:

- 10 high-value workflow JSON files.
- Instruction Markdown files.
- Skill Markdown files.
- Required software definitions.

First workflows:

- Full PC Health Check
- Drive Health Deep Check
- Download Offline Installer
- Install App Now
- Build Offline Deployment Bundle
- Windows Update Repair
- BSOD Investigation
- Security Advisory Check
- Clean Uninstall
- Technician Service Report

Acceptance:

- Each workflow has phases, agents, required tools, verification, report, cleanup,
  and cancellation policy.

### Milestone 9: Reporting and Cleanup Automation

Deliverables:

- Report agent workflow phase.
- Cleanup agent workflow phase.
- Artifact retention policy.
- Cleanup failure handling.

Acceptance:

- Completed workflow can generate a report automatically or by Report button.
- Cleanup preserves user-requested artifacts and removes temp folders.
- Report says what was cleaned and what remains.

### Milestone 10: Evals and Regression Tests

Deliverables:

- Golden workflow tests.
- Fake model runner for deterministic agent outputs.
- Fake tool dispatcher for success/failure/cancel simulations.
- Evals for SAK-first package/offline policy.
- Evals for cancellation propagation.

Acceptance:

- CI catches broken workflow schemas.
- CI catches missing cleanup/report phases.
- CI catches workflow runs that skip required built-in tools.
- CI catches cancel not reaching a child agent/tool.

---

## Test Plan

Unit tests:

- Workflow schema parsing.
- Workflow store merge/override.
- Skill/instruction load order.
- Requirement checker.
- Tool policy allow/block/risk.
- Cancellation token propagation.
- Trace event serialization.
- Budget accounting.

Integration tests:

- Single workflow with no subagents.
- Workflow with one subagent.
- Workflow with two parallel read-only subagents.
- Workflow with mutating lease.
- Workflow cancelled during:
  - main model request
  - queued subagent
  - running subagent model request
  - running PowerShell tool
  - running offline downloader
  - human approval dialog
- Workflow with missing bundled tool.
- Workflow with failed subagent and successful retry.
- Workflow with conflicting subagent results.

Manual tests:

- "Check my hard drive"
- "Download an offline installer for Chrome"
- "Install Firefox"
- "Fix Windows Update"
- "Generate a report"
- Press Stop in every phase.

---

## Risks and Mitigations

Risk: Multi-agent runs increase token cost.

Mitigation:

- Budget manager.
- Use subagents only when workflow needs them.
- Prefer cheap/fast model policy for narrow extraction.
- Summarize subagent context before model calls.

Risk: Subagents duplicate work.

Mitigation:

- Overseer gives explicit task boundaries.
- Trace store records assigned scope.
- Critic detects duplicate/low-value findings.

Risk: Parallel subagents fight over system state.

Mitigation:

- Parallelism only for read-only tools by default.
- Mutating lease serializes system-changing actions.

Risk: User cancels while installer is active.

Mitigation:

- Cancel signal, process termination where possible, partial-state report, no
  blind cleanup.

Risk: Prompt injection through web/docs/files.

Mitigation:

- Treat web/files as evidence only.
- Keep user files out of developer/system priority.
- Tool policy validates each call.
- Use structured outputs for subagent results.

Risk: Workflow templates become stale.

Mitigation:

- Template versioning.
- Golden workflow evals.
- Source links and last-reviewed dates in workflow metadata.

---

## Open Questions

- Should workflow templates be editable in-app in v2, or file-based only at
  first?
- Should subagents use the same selected model as the overseer by default, or
  allow workflow-defined model policies?
- Should report generation be automatic at workflow completion or only suggested?
- Should cleanup run automatically or ask before deleting temporary artifacts?
- Should user-provided Markdown instructions override workflow instructions or be
  appended after them as lower-priority task guidance?

---

## Recommended Implementation Order

The controlling order is the **Work Queue** in the Research-Derived Execution
Roadmap. This older list is retained as a coarse completion summary.

1. Done: Build workflow schema/store with tests.
2. Done: Move prompt templates into JSON workflows without changing runtime
   behavior.
3. Done: Add workflow UI preview and removable workflow/instruction/skill chips.
4. Done: Add trace store and cancellation token tree.
5. Done: Extract tool dispatcher/policy from panel and enforce `AiToolPolicy`.
6. Done: Add subagent runner and production Responses model wrapper.
7. Done: Add orchestrator with overseer/delegate/tool/cleanup phases.
8. Done: Add parallel read-only subagents with deterministic phase ordering.
9. Done: Add mutating leases and restore-point integration.
10. Done: Add report button and cleanup phase handling.
11. Done: Add workflow evals and cancellation regression tests.
12. Done: Add same-run resume for workflow-input, OpenAI tool approval/restore,
    and workflow-recovery gates.
13. Done: Add workflow clarification, cleanup/report failure evals, smoke
    checklist/script, Run Details phase history/timeline, and expanded catalog.
14. Done: Implementation is ready for manual QA and release-candidate smoke.
