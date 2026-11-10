# S.A.K. Utility - AI Assistant Headless Dominion Plan

Status: APPROVED (Randy 2026-07-23). Wave order = safe-first (as planned).
        win32 MCP = keep GUI-automation slice only (drop overlaps, delete bundle).
Owner: AI harness hardening program
Scope: give the AI assistant full, headless command of the app's own
technician functionality - the assistant must be able to use the full
functionality of the app WITHOUT needing the GUI.

--------------------------------------------------------------------------

## 1. Goal (Randy's words)

"Full dominion. The assistant should be able to use all of the features
headless - the full functionality of the app without the assistant needing
the GUI." And the governing rule: "if any MCP-server functionality overlaps
with the app, always use the app code."

Concretely: the assistant can enumerate and invoke every meaningful
technician operation the app performs, driving the SAME code the human
technician's buttons drive, through the existing guardrails, with no QWidget
in the loop.

--------------------------------------------------------------------------

## 2. Key finding: the app is already ~85% headless

The worry was "business logic trapped in the GUI." A full audit (13 panels +
the service layer) shows that is largely NOT the case:

- The entire src/core and src/actions layer is GUI-free. A grep for QWidget,
  QDialog, or QMessageBox across src/core/*.cpp and src/actions/*.cpp returns
  ZERO files. Business logic already lives in headless controllers/workers.
- 8 of 13 panels are READY: every technician op already delegates to a
  headless src/core controller that takes explicit params and returns a
  result. The panel is a thin view.
- 4 are PARTIAL: the controller is headless, but a few entry points read UI
  selection state, or one slice of orchestration still lives in the widget.
- 1 is TRAPPED: wifi_manager - logic is static members of the QWidget (though
  a parallel headless wifi_profile_scanner already exists, just unused).

So the real work is NOT rewriting logic. It is:
  (A) making the panel-owned controllers reachable app-wide (no GUI needed),
  (B) building the exposure + guardrail layer the assistant calls, and
  (C) closing a small number of localized extraction gaps.

--------------------------------------------------------------------------

## 3. Current-state map (headless readiness per panel)

READY (logic in a headless src/core controller, callable with explicit params):
  - diagnostic_benchmark   -> DiagnosticController
  - advanced_uninstall     -> AdvancedUninstallController (+ enumerator/workers/restore-point)
  - vulnerability          -> VulnerabilityScanner (static, returns result struct)
  - partition_manager      -> PartitionManagerController (+ planner/queue/executor/inventory)
  - image_flasher          -> FlashCoordinator (+ DriveScanner/WindowsUSBCreator/ISO downloaders)
  - advanced_search        -> AdvancedSearchController (+ AdvancedSearchWorker)
  - organizer              -> OrganizerWorker / DuplicateFinderWorker (Config struct)
  - email_inspector        -> EmailInspectorController (+ parsers/search/export/report)

PARTIAL (controller headless; localized UI-bound or split orchestration):
  - network_diagnostic     -> NetworkDiagnosticController (11 workers) headless;
                              adapter-admin netsh helpers (static IP / DNS / DHCP /
                              enable / disable / rename) are inline widget methods.
  - app_installation       -> ChocolateyManager / AppInstallationWorker /
                              OfflineDeploymentWorker / PackageListManager headless;
                              install entry reads m_installQueue, search threading inline.
  - file_management_explorer -> FS primitives + FileExplorerTransferWorker headless;
                              paste/collision/undo-redo/flatten orchestration inline in widget.
  - user_migration         -> backup/restore QThread workers + QuickActions headless;
                              end-to-end backup/restore orchestration lives in GUI wizards;
                              injected UserDataManager handle is dead.

TRAPPED (no headless service; logic in the QWidget):
  - wifi_manager           -> payload/QR/script/plist/XML builders + netsh scan/install
                              are static members of the widget; wifi_profile_scanner.h
                              exists but is unused.

Confirmed already-headless service layer (the target-architecture template):
  - PartitionApfsWriter / PartitionHfsFileSystemWriter: pure static, already have
    standalone CLI exes (sak_apfs_writer_cli / sak_hfs_writer_cli).
  - ChocolateyManager, PackageListManager, OfflineDeploymentWorker,
    PackageInternalizationEngine: headless QObjects / plain classes.
  - QuickAction / QuickActionController + the 7 concrete actions: headless QObjects
    with scan()/execute(); no widget dependency.

Assistant reach today: ZERO into the app's own features. grep QuickAction across
src/ai returns nothing. app_run_action targets EXTERNAL vendor manifests (Defender/
SFC), not internal panels. run_workflow procedures drive generic tools (run_powershell)
per phase, never a controller. So this is a genuinely new capability.

--------------------------------------------------------------------------

## 4. The three real gaps

Gap 1 - Reachability. Controllers are owned by panels (m_controller members),
created when a tab loads. Nothing app-scoped holds them, so the assistant cannot
reach them without the GUI. Need an app-owned service access point.

Gap 2 - Exposure + guardrails. No registry of app actions and no assistant tool
to list/invoke them. Must plug into the existing guarded tool pipeline
(AiToolDispatcher: policy -> health -> availability -> lease) and add human-gate /
restore-point for destructive actions (which the built-in dispatch path does NOT
provide for free - only the command path does).

Gap 3 - Parameterization + extraction. Several READY entry points read UI
selection; a few PARTIAL/TRAPPED ops need their logic lifted into a headless
service. Narrow, localized, per-panel.

--------------------------------------------------------------------------

## 5. Target architecture

### 5.1 Headless service layer (mostly EXISTS)
Keep every technician operation's logic in a GUI-free src/core controller/worker/
service that takes explicit parameters and returns a structured result. This is
already true for 8/13 panels; sections 5 waves fill the rest.

### 5.2 App-scoped service access: AppServiceHub
A single app-owned object (created by MainWindow, outliving any tab) that holds or
lazily constructs the headless controllers independent of their panels. Panels get
their controller FROM the hub instead of owning it privately, so the GUI and the
assistant share one instance. Controllers that are cheap and stateless can be
constructed on demand; stateful ones (queues, scan results) are singletons in the hub.

Rationale: this is the smallest structural change that makes "no GUI in the loop"
true. It does not move logic; it moves ownership.

### 5.3 Action registry: AppActionRegistry
An app-scoped registry (model: FileExplorerCommandRegistry + QuickActionController)
mapping a stable action id to an AppAction descriptor (section 6) plus an invoke
thunk that calls the headless service. Panels/subsystems register their actions
into it at startup. This is the single enumerable surface the assistant sees.

### 5.4 Assistant tools (2 new local tools)
Mirror the existing app_manifest / app_run_action naming, but pointed at OUR
registry instead of external manifests:
  - sak_app_actions  (list): enumerate the registry - id, title, description,
    category, params schema, risk flags (read_only / mutating / destructive /
    requires_admin). Read-only, always available.
  - sak_run_app_action (invoke): run one action by id with validated arguments;
    returns the structured result. Guarded (section 5.5).

Plug-in points (already mapped):
  (i)   schema builders + localToolDefinitions() in openai_responses_client.cpp
  (ii)  AiToolCallRouter::kindForName (new kinds) + isAsyncBuiltInKind
  (iii) AiToolDispatcher registerHandler in registerToolHandlers() (ai_assistant_panel.cpp)
        + availability checker in registerToolAvailabilityCheckers()
  (iv)  isKnownLocalTool + evaluateToolPolicy risk classification (ai_tool_policy.cpp)

### 5.5 Guardrail flow
List path (sak_app_actions): read-only; policy/health/availability via
AiToolDispatcher; no gate.

Invoke path (sak_run_app_action):
  1. AiToolDispatcher::dispatch enforces policy -> health -> availability -> lease
     (free, existing).
  2. The handler reads the action descriptor's risk flags. For mutating/destructive
     actions it MUST replicate the human-gate + restore-point wiring the command
     path uses (confirmCommandWithUser / offerRestorePointIfNeeded), exactly as the
     provider-gateway runner does for app_run_action. Catastrophic actions force a
     human gate even in Unattended.
  3. GUI-thread marshal: controllers are QObjects with worker threads and are NOT
     thread-safe against the GUI. Async built-ins run on a worker thread, so the
     handler marshals the actual controller call onto the GUI thread
     (invokeOnGuiThreadBlocking) and bridges the controller's async completion
     signal back to a synchronous tool result (QEventLoop on the worker side).
  4. Loop detector + 12-turn cap already apply upstream.

Access-mode mapping: read-only actions allowed in all modes; mutating actions
require Assisted/Unattended per AiToolPolicy; destructive/admin actions always gate.

--------------------------------------------------------------------------

## 6. AppAction descriptor model

  struct AppAction {
    QString id;              // stable, e.g. "network.ping", "partition.apply_queue"
    QString title;           // human label
    QString description;     // one line, model-facing
    QString category;        // diagnostics | maintenance | disk | network | ...
    QJsonObject params;      // JSON-Schema for arguments (empty = no args)
    bool read_only;          // no system change
    bool mutating;           // changes system/files
    bool destructive;        // data loss / irreversible
    bool requires_admin;     // needs elevation
    // invoke: validated args -> structured result (via headless service)
  };

The 7 QuickActions map directly (parameterless, admin flag known). Controller ops
carry their existing param structs, described as JSON-Schema.

--------------------------------------------------------------------------

## 7. Wave plan (each wave: build + gates + headless tests + adversarial review + commit)

Wave 0 - Foundation (proves the whole pipeline end to end)
  - AppServiceHub skeleton (holds/constructs headless controllers app-scoped).
  - AppActionRegistry + AppAction descriptor.
  - sak_app_actions (list) + sak_run_app_action (invoke) tools, full guardrail
    wiring incl. human-gate/restore for destructive + GUI-thread marshal.
  - Seed with the 7 QuickActions (headless, parameterless, admin-aware).
  - Cert: assistant lists + runs a QuickAction end to end, gated correctly.

Wave 1 - READY read-only ops (low risk, high value, no destructive gate)
  - diagnostics: hardware scan, SMART scan; vulnerability scan; benchmarks (read).
  - network: ping / traceroute / mtr / dns query / adapter list / port scan.
  - partition: refresh inventory, dry-run queue.
  - search: advanced_search run; email open/search; organizer preview; dedup scan.
  Each op gets an explicit-param entry point on its (hub-scoped) controller.

Wave 2 - READY mutating ops (behind policy + gate)
  - advanced_uninstall: uninstall / forced / batch / clean leftovers.
  - partition: apply queue (create/delete/format/init), with restore point.
  - image_flasher: flash (destructive, system-drive guard preserved).
  - organizer execute; app_installation install (explicit list); email export.
  - network reset (QuickAction already covers most).

Wave 3 - PARTIAL extractions (lift the UI-bound slices into headless services)
  - network adapter-admin: move static-IP/DNS/DHCP/enable/disable/rename netsh
    helpers into EthernetConfigManager (or AdapterAdminService); register.
  - app_installation: MigrationReport-from-explicit-list overload; push search
    threading into ChocolateyManager.
  - file_management_explorer: extract paste/collision/undo-redo/flatten into a
    FileExplorerOperations service (primitives + transfer worker already headless).
  - user_migration: add a headless MigrationController wrapping the backup/restore
    workers (retire dead UserDataManager); register backup/restore.

Wave 4 - TRAPPED extraction
  - wifi_manager: lift payload/QR/script/plist/XML builders + netsh scan/install
    into a headless WifiNetworkService over WifiConfig lists (reuse
    wifi_profile_scanner); register generate/export/scan/install.

Track B - win32 MCP resolution (after dominion lands)
  - Apply the overlap rule: process/screenshot/window/monitor/inspection tools now
    route to app code (PowerShell / take_screenshot / app actions) - drop them.
  - Keep ONLY the genuinely-additive slice the app cannot do: desktop GUI
    automation of OTHER apps (click / type / press_key / hotkey / drag / scroll /
    clipboard / UI Automation / find-text-on-screen). If retained, own it natively
    (system APIs only, no bundle) - the parked sak_win32_mcp_server skeleton is the
    starting point but rebuilt around ONLY the gap tools. Delete the 40MB external
    Python+Tesseract bundle.
  - RESOLVED (Randy 2026-07-23): KEEP the GUI-automation slice only. Rebuild the
    native server around ONLY those gap tools (system APIs, no bundle) and delete
    the 40MB external Python+Tesseract exe.

--------------------------------------------------------------------------

## 8. Guardrails and safety (unchanged, reused)

All existing guardrails stay and are reused, never bypassed:
  - AiToolPolicy access modes; isKnownLocalTool hard-block for unknown names.
  - AiToolHealthLedger circuit breaker; AiLeaseManager single-flight leases.
  - Human gate (confirmCommandWithUser) + restore points for destructive actions;
    catastrophic actions force a gate even in Unattended.
  - Loop detector + 12-turn cap.
  - Every mutating action fails CLOSED if policy/lease/gate is unavailable.

--------------------------------------------------------------------------

## 9. Risks and decisions

- Ownership migration (panels -> AppServiceHub) touches panel constructors. Do it
  panel-by-panel, keeping the panel working, so no big-bang refactor.
- Async-to-sync bridging in the invoke handler must not deadlock the GUI thread
  (reuse the established worker-thread + BlockingQueued marshal pattern; the AI
  panel already does this for tool_action).
- Destructive-action gating must be proven per action, not assumed (adversarial
  review each wave).
- RESOLVED (Randy 2026-07-23): Wave ordering = safe-first as planned
  (foundation -> read-only -> mutating -> extractions -> wifi).
- RESOLVED (Randy 2026-07-23): win32 MCP = keep GUI-automation-only native slice,
  drop overlaps, delete the external bundle.

--------------------------------------------------------------------------

## Appendix - anchors

Guardrail pipeline: ai_assistant_panel.cpp dispatchNextToolCall / preparePendingToolCall /
dispatchBuiltInToolCall / startAsyncBuiltInToolCall / dispatchCommandToolCall;
AiToolDispatcher::dispatch (src/ai/ai_tool_dispatcher.cpp); AiToolCallRouter::kindForName
(src/ai/ai_tool_call_router.cpp); evaluateToolPolicy (src/ai/ai_tool_policy.cpp);
tool schemas + localToolDefinitions (src/ai/openai_responses_client.cpp).
Registry templates: FileExplorerCommandRegistry (include/sak/file_explorer_command_registry.h);
QuickActionController (include/sak/quick_action_controller.h).
