# Release Readiness

Current HFS+/APFS driver capability status is owned by
`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`. Historical milestone text below is not a
second status owner; release-facing Apple filesystem claims must mirror the gated matrix.

## Release bar (owner ruling 2026-10-24)

The next release ships when every box below is checked on the release bytes. This replaces the R5
definition of done (G10), whose open-ended items ("zero findings anywhere", "no surviving mutant
anywhere", full review-ledger coverage) could never close.

- [ ] CI (tier T2, see AGENTS.md) green on `main` for the release commit.
- [ ] Startup does not freeze the GUI: the window stays responsive while fonts load, including on
      a machine with thousands of installed fonts; stock Windows 11 VM cold start within budget.
- [ ] Clean-package smoke and startup end-to-end smoke pass (tier T3).
- [ ] Shipped binaries are Authenticode-signed. The Azure signing credential is valid to 2028
      (owner confirmed); the first signed release run is still to do.
- [ ] Windows 11 VM smoke passes on the release bytes.
- [ ] No known crash, data-loss or unpatched-security item remains in the list below.
- [x] Hardware-only claims (partition external gates, Apple kernel, Windows 10) are labeled
      uncertified by the computed claim level in `certification/`.

DESIGN DECISION (owner 2026-10-24): Windows 10 rig runs, the 18 external partition hardware gates,
Apple kernel/hardware certification, review-ledger coverage, mutation surface and clang-tidy naming
debt are not release requirements. Their claims stay labeled uncertified; the archived R5 document
keeps their history.

### Open crash, data-loss and security items (must be empty to release)

1. Startup GUI freeze: FIXED locally 2026-10-24 (GUI block 5.8-10.8 s -> under 0.15 s on a
   2491-family machine; see `docs/GUI_STARTUP_FIELD_NOTES.md`). Still to prove on release bytes:
   the stock Windows 11 VM run and the package smoke.
2. FIXED 2026-10-30, dependency security updates (owner: every dependency on its latest stable
   release). Qt 6.10.3 -> 6.12.0 LTS (6.10 support ended 2026-10-07; 6.12 is supported to
   2031-09-30 and is the last Qt release for Windows 10); CMake now requires 6.12, and CI installs
   it through aqtinstall master at a pinned commit because the released 3.3.0 cannot read the
   6.11+ repository layout. zlib 1.3.1 -> 1.3.2 and liblzma 5.8.2 -> 5.8.4: `vcpkg.json` pins
   them through its builtin-baseline (vcpkg 2026.10.16) and configure installs them into the
   build tree, so local and CI builds link the same versions (CI had taken whatever its runner
   image's vcpkg shipped, local builds a January checkout). Chocolatey 2.7.2 -> 2.7.5 (2.7.4:
   security log4net 3.3.1 and 7-Zip 26.02; 2.7.5: 7-Zip 26.05), from the community nupkg whose
   SHA-512 matches the feed. The hfsprogs OpenSSL 3.6 runtime (end of life 2026-11-01) is gone
   -- `newfs_hfs` used it only for SHA-1 over volume-ID entropy, now a public-domain header in
   the S.A.K. patch (which also had malformed hunks and could not reproduce the build);
   `msys-crypto-3.dll` (5.4 MB) left the package, and `msys-2.0.dll` is 3.6.12. iPerf3 3.21 ->
   3.22 (esnet 3.22 notes: server use-after-free CVE-2026-101283, auth heap overflows
   CVE-2026-101276, DoS loop CVE-2026-102253; the app runs iperf3 as a server).
   Behind latest on purpose: MSVC stays on VS 2022 / v143 (Qt 6.12 lists MSVC 2022 as its
   supported x64 compiler; VS 2026 is v145); `cygwin1.dll` 3.6.10 ships inside the pinned
   upstream iperf3 build and moves with the next one; the gate tools run on Python 3.14 (CI
   resolves the newest 3.14 patch) because pre-commit's PyYAML has no Python 3.15 Windows wheel
   yet. CMake: CI configures with a pinned 4.4.5 instead of the runner image's 3.31. Owner decisions
   (2026-10-31): the patched UUPMediaConverter is rebuilt from upstream `main` 885f2e36 on .NET 10
   LTS (supported to 2028-11-14) with its patch in the repository
   (`scripts/build_uup_media_converter.ps1` reproduces it bit for bit;
   `tools/uup/uupmc/BUILD-NOTES.md`), replacing the .NET 8.0.23 build whose support ends
   2026-11-10. It reports 3.1.9.2 only because upstream never bumped that string; the old and new
   sources both contain release 3.1.9.3 (e0c4ce0). Its `libwim-15.dll` moves from the wimlib
   1.14.4 that ManagedWimLib 2.6.0 carries to wimlib's own 1.14.5 build, after a harness on the
   converter's `WimLibImaging` read, exported, updated and extracted a test WIM through it.
   aria2c 1.37.0 (still the latest aria2) is now built here from source
   (`scripts/build_aria2.ps1`, `tools/uup/aria2/BUILD-NOTES.md`) instead of shipping the 2023
   official build with zlib 1.3, c-ares 1.19.1, Expat 2.5.0, SQLite 3.43.1 and libssh2 1.11.0: it
   links zlib 1.3.2, c-ares 1.34.9, Expat 2.9.0, SQLite 3.54.1 and libssh2 1.11.1 (WinCNG)
   statically, keeps Windows TLS and the Windows certificate store, and imports only Windows DLLs;
   it verified a GitHub download and refused expired, wrong-host, self-signed and untrusted-root
   certificates. Owner decision 2026-10-31, accepted risk: keep shipping the `cdimage.exe`
   upstream carries (Microsoft's unsigned CDIMAGE 2.53m marked "For Microsoft internal use only",
   no license, so its redistribution terms are unclear; `THIRD_PARTY_LICENSES.md`) rather than
   depend on the Windows ADK's `oscdimg.exe`.
3. FIXED 2026-10-30: downloaded third-party binaries without a hash pin. iperf3 and
   smartmontools are pinned (the bundled smartctl.exe is byte-identical to the one in the
   pinned, SignPath-signed 7.5 installer), and `scripts/sign-exe.ps1` pins
   Microsoft.ArtifactSigning.Client 1.0.146 (successor of the unlisted Trusted Signing client)
   by its nuget.org SHA-512 and checks the dlib's Authenticode signature before signtool loads it.
4. Review briefs TRIAGED 2026-10-31 (`docs/review_briefs/TRIAGE_2026-10-31.md`): all 140
   findings in the 38 brief files checked against dev 0b501108. 106 fixed, 12 gone with the code
   they described, 4 not a bug, 1 needs a runtime check, 17 still open (2 HIGH: a UNC path with a
   space leaking into saved chat titles, and a custom "High Performance" plan chosen over the
   built-in one), plus 9 new defects the briefs missed (2 HIGH: AI command tools join their
   kill-on-close Job only after launch, and the redaction misses `access_token=`,
   `client_secret=`, `AWS_SECRET_ACCESS_KEY=` and JSON `Authorization`/`Cookie` keys).
   FIXED 2026-10-31 (build lane), each with a test and a drill; the triage's "Fixed" section maps
   every item to its commit: redaction and chat titles d3aad6dc (TITLE-2, CRED-1R/NEW-2, CRED-8),
   credential store 3900a0a0 (CRED-3 to CRED-7; the key file now gets a protected DACL), power
   action a1ba827f (POWER-2, 5 to 9), system report dd77a031 (SYSREP-8, 10, 11), AI command
   containment c2edaa8a (NEW-1: JOB_LIST launch, refused without a Job), command guard 913d5be8
   (NEW-3 to NEW-5), dispatcher and panel 9ad0e6d5 (NEW-6 to NEW-9). Remaining: PANEL-6 (116
   nonconforming member names in the AI panel) is naming debt, which the owner ruled is not a
   release requirement. MCPS-9 needs a runtime run: test_ai_mcp_stdio_session and
   test_ai_mcp_session_pool from a clean package on the Windows 11 VM plus one assistant E2E
   win32_mcp health_check through the gateway (VMs offline). Found by the lane: NEW-10, the win32
   MCP server's own redaction list had the same compound-name gap. FIXED 2026-10-31 (second build
   lane) 4bda0e91: the redactor is its own library, sak_secret_redaction, which the AI layer and
   the win32 MCP server both link, so tool output reaching the model and anything the assistant
   persists go through one catalogue; a JSON tool result is redacted as a tree. The GUARD-6
   residual over-block is kept as a design decision (22290db6: splitting at braces would let a
   loop-variable registry scan through), and the command-guard mutation catalog now runs in full
   (81f31c2e). Found and fixed on the way: NEW-11, a saved Wi-Fi passphrase that
   `netsh ... key=clear` prints as `Key Content` was not redacted (7115b623). The triage's
   "Second build lane" section has the detail.
5. Real defects found by the 2026-10-24 behavior audit:
   - FIXED (76ca7a52): `ProcessRunner` killed descendants still alive about 1 s after the root
     exited and marked the run failed, so NSIS uninstallers (which relaunch from %TEMP%) and
     Chocolatey installs were reported as failed. Runs take a child-process policy; the native
     uninstaller and Chocolatey install/uninstall/upgrade wait, bounded and cancellable, for the
     whole Job, and a family that outlives the wait is stopped and reported, never silently.
   - FIXED (4e47cf6c): a second uninstall within 24 h was refused because Windows' once-per-day
     restore-point limit was a failure. A restore point from the last 24 h is reused and named;
     the batch restore point is made off the GUI thread.
   - FIXED (550766d0): the AI session list went empty above 512 session folders. It now lists the
     512 most recently active and a retention policy keeps the newest 500 (AI package below).
   - FIXED (04109495): the default backup path stored BitLocker recovery keys on the volume they
     unlock. The panel asks for a destination each time and the action (in process and in the
     elevated helper) refuses a folder on a BitLocker volume whose key is in the backup.
   - FIXED (4f3af0fc): the iperf3 server ran outside any Job, so a crash leaked it and its inbound
     firewall rule. It runs in a kill-on-close Job; rule names carry the owner's process ID and a
     server start sweeps rules whose owner is gone. Under CTest (itself a Job) the test can check
     only that the server's QProcess holds its Job; OS-level membership is checked outside CTest.
   - FIXED (c4edbb04): the settings-screenshot action force-killed every Settings window,
     including the user's, ran tasklist per page and slept fixed times. It finds Settings by
     EnumWindows, waits for the window to answer, and closes (WM_CLOSE) only a window it opened.
6. FIXED 2026-10-25: defects the first T2 CI runs found, each reproducing in a fresh Git for
   Windows clone (default `core.autocrlf=true`), which is also how CI checks out and builds:
   - Built-in AI workflows and skills did not load from a CRLF checkout: `loadResourceFile`
     (`src/ai/ai_store_file_loader.cpp`) read in `QIODevice::Text` mode, which drops every CR,
     then required the bytes read to equal `QFile::size()`, so a CI-built release would have
     shipped with no built-in workflows or skills. It now reads binary; CRLF fixtures pin it
     (`test_ai_skill_store::store_loadsCrlfResource`,
     `test_ai_workflow_store::loadCrlfResourceWorkflow`).
   - `test_partition_manager_panel` matched source text containing `"\n"` against raw bytes;
     its source reads now fold CRLF (`sourceTextReadIgnoresLineEndings`).
   - `test_browser_bridge_security` searched the DACL's SDDL for the user's SID string, which
     SDDL writes as `LA` for the built-in Administrator (a hosted runner's account); it now
     compares the user ACE in SDDL's own form (`security_userTrusteeMatchesSddlAliases`).
7. Left open by the lifecycle package (2026-10-25), each with why:
   - Unbounded GUI-thread joins outside that package's list remain, each needing its own owner
     custody: `network_diagnostic_controller.cpp` (~238, ~1305), the QuickActionController
     completion join (`joinCurrentThread`; load-bearing, after the run's own completion), the AI
     panel's network, command and offline thread joins, AppInstallationPanel's search futures,
     DiagnosticController's scan futures, and the stack-thread joins in
     `advanced_search_controller`, `port_scanner` and `network_transfer_runner`.
   - FIXED (2d7de3b0): assistant app actions (`sak_app_action`) observe Stop and exit. Every
     async action and every quick action takes the caller's cancel route; see the AI package below.
   - FIXED (55492ced, 273e27f1, a99462a7): the AI tool handlers read the chat, access
     mode and cancel token through `this`. They run on a per-call context now (AI package below).
     Teardown still goes through `AiAssistantPanel::release`, which keeps the panel's long-lived
     services alive until the handlers drain.
   - FIXED (c507b338): a Stop during a `run_powershell` command left the run at Cancelling after
     the command finished; `onBrokerFinished`/`onRequestFinished` now settle a pending stop.
   - The exit prompt's per-panel registrations are covered by review, not by a test each; the
     registry, the prompt and the main window's use of it are tested.
8. FIXED 2026-10-31: Windows ISO builds could not finish. UUPMediaConverter starts
   `CDImage\cdimage.exe` beside itself to write the ISO (upstream `CDImageWrapper.cs`; a missing
   file makes `Process.Start` throw and the step return failure), but the bundle had it at
   `tools/uup/uupmc/CDImage/CDImage/cdimage.exe`, so by static reading every conversion failed
   at its last step. The converter also read a `Settings\Settings.ini` from before upstream PR
   #194, so six setup files were left off the media (`pnppropmig.dll`, `UnattendMgr.dll`,
   `WinSetupBoot.hiv/.sys`, `WinSetupMon.hiv/.sys`), while the current file sat unread in a
   nested `Settings\Settings\`. The layout is now the one the converter reads, and
   `scripts/bundle_uup_tools.ps1` (also run by the release workflow over the copied bundle) pins
   every file under `tools/uup` by SHA-256 and rejects any file it does not pin. Removed as
   unused under the 2026-10-24 ruling: `UUPDownload.exe` (nothing launches it; downloads use
   aria2c) and three .NET Framework `*.dll.config` binding-redirect files that .NET ignores (the
   converter runs its help and its failure paths without them). Still to prove: an elevated
   conversion of a real UUP set on the Windows 11 VM (VM smoke).

### Fixed by the lifecycle package (2026-10-25)

- ProcessRunner's unproved Job cleanup is report + contain (the run fails with the diagnostic,
  the process and armed Job go to `sak::ContainedProcessQuarantine`) instead of qFatal.
  `~WindowsProcessJob` logs and closes (kill-on-close) on live members or a failed query; the
  installed-launch-hook and CreateProcess-modifier checks stay fatal as programming-error
  invariants. bc578b0c
- Worker teardown never terminates: `sak::ThreadCustody`, `WorkerBase::release` and
  `sak::WorkerPtr` retain an overrunning worker (signals severed) until it finishes. The
  unbounded GUI waits in the profile backup/restore workers, QuickActionController teardown and
  the Image Flasher USB thread are gone, and the assistant's uninstall/cleanup drivers lost their
  `terminate()`. A running worker destroyed directly past its bound is the remaining invariant.
  2e0d3a43
- AI panel: in-flight Win32 MCP calls are cancellable (Job terminate, prompt return), the async
  tool work holds the dispatcher and MCP pool by shared ownership, and teardown with live work is
  report + contain through `AiAssistantPanel::release` instead of a 30 s freeze and qFatal.
  f5ff2283
- Closing the window or Run as Admin with work running asks first ("<operation> is running.
  Cancel it and exit?"), then cancels cooperatively with progress and proceeds once it has
  stopped (`sak::RunningOperations`, `sak::RunningWorkExitGuard`). e0a5b3ef

### AI package (2026-10-25, owner-approved)

Done:

- No live token-count POSTs: the context readout shows the last response's input-token usage;
  `countInputTokens` and the 700 ms typing timer are gone. 1d4574d8
- Least-privilege default access mode (Chat and Research). No access mode is persisted, so there
  is no saved preference to honour. 1d4574d8
- Generate System Report runs as a read-only assistant action (no approval or lease):
  `QuickAction::isReadOnly`. dd4802a5
- Tool circuit breaker is per chat session, in memory: the 24 h ledger file, its pinned-handle
  file I/O, its three file hooks and the persistence-only tests are gone; a new or opened chat
  resets it (`resetToolHealthForSession`). An old `ai/tool_health_ledger.json` left in a data
  folder is no longer read. 6a0a9e8a
- Execution broker: one single-shot deadline timer armed with the remaining timeout replaces the
  200 ms re-armed poll. Pooled MCP servers (the full-access Win32 MCP child included) are closed
  after 5 minutes unused (`kAiMcpSessionIdleReap`): a single-shot reap armed after each
  provider-gateway call runs off the GUI thread, skips in-flight calls, and hands a session whose
  cleanup cannot be proved to `sak::ContainedProcessQuarantine` (no qFatal, admissions not
  blocked). c7402083
- Trace streams: nothing in the tree read or exported `trace.jsonl` or `turn_replay.jsonl` (only
  the manual QA runbook mentioned them; it now points at the activity log), so they are no longer
  written. `activity.jsonl` is the one AI audit stream. 7bedf152
- Transcript view: appending a message, expanding one and changing the activity line touch only
  that row; only a loaded transcript is rendered in one pass. Starting a new chat now clears the
  old bubbles at once (they used to stay until the next message). c97c0c3d
- Session picker: each manifest records `has_prompt` (set when a user prompt is appended; a
  legacy manifest is answered from its transcript once and the flag written back), the picker
  lists manifests only on the thread pool, and the listing loads the 512 most recently active
  folders (`kAiSessionListLimit`; up to 65,536 enumerated) instead of failing above 512.
  Retention (documented default `kAiSessionRetentionCount` = 500): starting a chat deletes
  store-created session folders beyond the newest 500 -- at most 8 per start
  (`kAiSessionPrunePerStartLimit`, least recently active first, so a store far over the limit
  never freezes the window) -- never the current one and never a folder the store did not
  create. 550766d0
- The accessibility audit builds the real AI panel; the constructor's audit early return is gone.
  2f451519
- A Stop during a PowerShell command settles to Cancelled when the command ends. c507b338
- `app_run_action` retired (item 8, owner-delegated decision). SUPERAntiSpyware's Win32
  desktop-control recipe is dropped with no AI-layer replacement. Microsoft Defender quick scan,
  full scan and signature update are registry actions (`action.microsoft_defender_*`,
  `MicrosoftDefenderAction`): MpCmdRun.exe comes from the platform folder Defender registers
  (`HKLM\SOFTWARE\Microsoft\Windows Defender\InstallLocation`), accepted only as a version folder
  under `ProgramData\Microsoft\Windows Defender\Platform` or as `Program Files\Windows
  Defender`, and the actions elevate through the quick-action elevated-helper route
  (`QuickActionController::startElevated` -> `sak_elevated_helper` tasks). Exit 0 is
  clean (scan) or up to date (update); a scan's exit 2 is "threats need attention or scan
  errors", never clean; any other code is a failure with its value. A cancelled or timed-out scan
  is also cancelled in the Defender service (`MpCmdRun -Scan -Cancel`). Bounds: 2 h quick, 4 h
  full, 30 min update. SFC (`action.verify_system_files`) has one 60-minute bound: each SFC/DISM
  step is cut to what is left, and the registry thunk waits each quick action's own declared
  bound (`QuickAction::executionTimeoutMs`) instead of a generic 30 minutes. The gateway, tool
  runner, policy, tool schema and planner no longer know `app_run_action`,
  `app_run_action_plan`, `app_manifest` or `app_capabilities`; the manifests, their qrc
  entries, the win32_gui step runner, `SAK_AI_POLICY_DISK_OVERRIDE` and the mtime cache are gone,
  and with the disk override gone the `data/ai` staging nothing read any more left CMake, the
  stage/archive/smoke scripts and release.yml (the smoke checks the embedded catalog's stdio
  commands against the package instead). Found and fixed on the way: the elevated helper sent
  nothing while a task step ran, and the broker reads each frame with a 30 s timeout, so any
  elevated step silent for longer (SFC, a scan) was read as a lost helper and abandoned; the
  helper now sends a Heartbeat every 10 s while a task runs. 254c7160
- Assistant app actions observe Stop (item 10). `AppActionRegistry` passes the caller's cancel
  route to cancellable thunks; `AsyncActionInvocation` never starts work a Stop already
  cancelled, and on a Stop during the run calls the site's stop request and waits up to 15 s
  (`kAppActionStopGraceMs`) for the operation's own result before settling as cancelled. Every
  async action is cancellable (file search, duplicate finder, organize preview and organize,
  benchmarks, ping, traceroute, mtr, port scan, flash, partition apply, uninstalls, leftover
  cleanup), and the quick actions cancel on their controller, which reaches the elevated helper.
  The panel passes its tool-run cancel check to every app-action invoke, so Stop and exit both
  reach a running action. 2d7de3b0
- `*ForTesting` hooks are out of the AI libraries (item 11b): the store file hooks, credential
  wipe observers, MCP/model-client thread starters, the MCP/Responses/registry `*ForTesting`
  entry points and the panel's workflow wipe observer and approval-prompt hook compile only
  under `SAK_AI_TEST_HOOKS`, into the tests that drive them (`SAK_AI_*_TEST_HOOK_SOURCES`).
  `test_release_binary_test_hooks` scans `sak_ai_core.lib` and `sak_gui.lib` for any
  `ForTesting@` symbol and the approval hook, and the binaries for
  `SAK_AI_POLICY_DISK_OVERRIDE`. fa5db306
- Found by T2 on 0fa17dce, FIXED (010d02f0): the session picker's off-thread listing (550766d0)
  read every manifest without the store's lock, so a rename or prompt save replacing the open
  session's manifest could fail with "Could not commit manifest: Access is denied" (Windows
  refuses to replace a file another handle has open). The listing reads under the read lock;
  `test_ai_conversation_store` holds a manifest open in the read and renames the session.
- AI tool handlers run on a per-call context (item 7, item 10 rest). `ai::AiToolCallContext` is
  captured on the GUI thread when a call is dispatched (a chat tool, a workflow launch, a
  sub-agent's tool) and owned by the call: the chat's id and a const store bound to that chat
  (`ConversationStore::sessionSnapshot`), the access mode and its policy ceiling, the run's cancel
  token, the user's request, and the lease `AiToolDispatcher` took for the dispatch. Every handler
  reads those from it instead of from the panel. Another chat opened under a running call, or the
  panel released under it, cannot redirect it: its artifacts land in the chat it ran for, its
  result is dropped and its turn ends (`finishAsyncBuiltInToolCall`), its later prompts are
  declined without being shown, and its command and memory records (which write the manifest, so
  they stay on the panel's store) are dropped. Const is the design here: a store holding a stale
  session record could otherwise undo a rename by writing the manifest. App-action staging is a
  local of the call and the action's folder is per calling thread
  (`AppActionService::OutputDirectoryScope`); it was a member every chat change replaced under a
  running action. The panel's long-lived services stay on the panel: nothing replaces them while
  it lives, and `release` keeps it alive until its tool work stops. Tests:
  `sessionSwitchDuringToolKeepsItsWritesInItsOwnChat`,
  `releaseDuringToolKeepsItsWritesInItsOwnChat`, the same two through the real `sak_app_action`
  handler, `toolCallKeepsTheAccessModeItWasDispatchedWith`, `toolCallGatesDeclineOnceItsChatCloses`,
  `toolCallRecordsDropOnceItsChatCloses`, `workflowPowerShellStopsOnItsCallsToken`,
  `workflowAndSubagentToolsRunWithTheirLaunchContext`. Drilled guard by guard. 55492ced,
  273e27f1, a99462a7, 015b44af
- The delegate_subagent model-factory seam is out of production: the panel's
  `m_delegateSubagentModelFactoryOverride` member is gone, and the tests set
  `aiDelegateSubagentModelFactoryTestHook`, a function that exists only under `SAK_AI_TEST_HOOKS`
  (a function rather than a member, so hooked and product objects agree on the class layout).
  `test_release_binary_test_hooks` refuses its symbol in `sak_gui.lib`. 3c0d25cb
- Proof for the two above and the dead-code removal below, at f4727896: full build, T2-label suite
  278/278, dead-code gate (308 files), stage, archive, clean-extract portable smoke and startup
  E2E, accessibility audit (unelevated), the ci.yml static gates. The readiness gate stops at the
  certification state block in this worktree, which lacks the main checkout's external evidence.

Open, each with why:

- The context covers the tool handlers, not the run around them. A model response, a workflow
  phase's transcript line and the broker's command result are persisted on the GUI thread into
  whichever chat is open, so the refusal to switch chats while the panel is busy is still what
  keeps those right. A running tool no longer depends on that refusal; the run's own records do.
- An artifact whose full path would pass MAX_PATH is refused (found while testing this package):
  `createAtomicStoreTempFile` opens its temp file by plain path, so a long install folder, chat
  title and action id together fail with "Could not create artifact temporary file ... (Win32
  error 3)". The call reports the error and the action's output is lost; nothing is written
  elsewhere. A long-path prefix on the temp path is the likely fix.
- The Defender actions and the elevated-helper heartbeat are unit-tested (result mapping, path
  rule, elevated task mapping, heartbeat protocol) but not run end to end here: a live run needs a
  UAC prompt and a real scan, which the T2 runners and this host's unattended lane cannot give.
  First live check is the Windows 11 VM smoke.

### Behavior audit 2026-10-24 -- owner decisions

A read-only audit looked for code that makes the app behave unlike a normal Windows tool, or
that exists for reasons that no longer apply. Owner decisions (2026-10-24):

- Approved as a class: remove dead code, dialogs and config keys; non-Windows `#else` branches;
  duplicate implementations (three zip paths, four NuGet feed clients, two Generate Report
  features, two Qt deploy routes, three quick-action controllers); package files nothing loads
  (`UUPDownload.exe`, Mesa `opengl32sw.dll`, `D3Dcompiler_47.dll`, unused Qt plugins and CRT
  DLLs, dev scripts, Chocolatey dev state, `portable.ini`); the ignored AI policy staging;
  test-only hooks in release builds; env-var escape hatches. Each is proved unused first.
- Approved UI changes: ask before exit when work is running and cancel it cooperatively; theme
  follows Windows with one saved Light/Dark/System setting (the per-panel Dark switches go);
  1px borders replace the app-wide drop-shadow effect. The default tab is unchanged.
- Approved disk-tool changes: rename features to what they do (e.g. "Surface Test" is a
  filesystem check, "SSD Secure Erase" is TRIM plus overwrite) and run read-only checks
  immediately; one elevated helper per session instead of a UAC prompt per operation; remove the
  USB maker's bcdboot step and duplicate "set active" and wait for the volume instead of fixed
  sleeps; remove the internal certification checklist from the Secure Erase dialog.
- Approved AI changes: no live token-count POSTs while typing (show the last response's usage);
  least-privilege default access mode ("Chat and Research"); the tool circuit breaker is
  per-session in memory instead of persisted for 24 h.
- Decided (owner delegated): keep `sak_app_action` (the core AppActionRegistry seam the GUI and
  assistant share) and retire the AI-only manifest path `app_run_action` and
  `resources/ai/app_manifests`, moving any action the registry lacks into it. Done (254c7160).
- Behavior fixes needing no decision: poll sensors only while their tab is visible; drive
  rescans on device-change notifications instead of a 5 s timer; program sizes from the
  registry's EstimatedSize; WiFi, restore-point and AI session-list work off the GUI thread;
  incremental transcript and log-window updates; settings in the portable INI, never the
  registry; TEMP redirected only for the children that need it; one data root with no silent
  `%LOCALAPPDATA%` fallback; log pruning at startup; a declined UAC prompt is not an error;
  smoke and audit modes run the same startup work users get; info-level startup breadcrumbs
  move to debug.

#### Disk-tool package status (2026-10-24)

- DONE (6942b5b6): honest names. "Surface Test" is "Scan File Systems", "Partition Alignment" /
  "Align All Partitions" is "TRIM SSD Volume" (the disk-row entry, which could only refuse, is
  gone), "SSD Secure Erase" is "Erase SSD (TRIM + Overwrite)" with a method row saying it is not
  an ATA/NVMe sanitize, "Change Serial Number" is "Reformat for New Serial Number". Enum IDs and
  the certification matrix are unchanged: the matrix name of `external.ssd-secure-erase` is bound
  by its imported evidence report (`gate_name`), so renaming it would break that evidence.
  Check File System and Scan File Systems run immediately through
  `PartitionManagerController::runReadOnlyOperation`, which refuses anything the planner does
  not classify read-only. Found and fixed on the way: a non-Windows "check" (the bundled tool's
  repair) was classified read-only and shown as "Read-only" in the Apply review.
  Decided (owner 2026-10-25), DONE (d7a8c134): Scan File Systems is disk-level only (ribbon
  enabled for a disk; disk menu only; the script builder refuses a partition target), and Check
  File System is the per-partition scan. Both stay read-only and immediate. The unallocated
  menu's Scan File Systems, which could only fail for want of a drive letter, is gone.
- DONE (6942b5b6): the erase dialog no longer shows the certification lab's checklist or its
  Copy button; the non-Windows metadata "check" is labelled as last-disk-scan results (re-running
  needs the raw access only the elevated scan has, so it is not re-run from the dialog).
- DONE (799fe10d): one elevated helper per session for Partition Manager's scans, apply
  preflight/verification scans and queued operations (`ElevationBroker::sessionBroker`; other
  inventory callers -- File Management, Organizer, Advanced Search -- share it too). A helper
  near its 5-minute idle timeout is retired before a task is sent, never handed one. Still per
  feature: the AI panel keeps one long-lived broker for elevated PowerShell, and the application's
  one quick-action controller owns one broker for every quick action, kept for its lifetime
  (a4136249; until then each of four controllers started a broker per elevated run and closed it
  when the run ended). Not changed, with
  reasons: the Image Flasher writes raw disks in-process and so requires the whole app elevated
  (restart-as-admin gate); SMART runs smartctl unelevated and reports "re-run with administrator
  privileges". Routing either through the helper needs a new privileged helper task (raw image
  streaming, smartctl execution), which is new attack surface, not a straightforward change.
  Live UAC-count proof needs an interactive VM run; CTest covers the reuse policy and the
  executor wiring (the inventory worker's switch to the session broker is by review).
- DONE (8f7875ef): the USB maker has no bcdboot step (install media has no Windows directory for
  it to use) and sets "active" once, at partition creation; fixed sleeps are replaced by a
  bounded wait (60 s) for the new partition's volume and the drive letter, failing with an error
  that names the disk. It still extracts with the 7z.exe inside the bundled portable Chocolatey
  (`tools/chocolatey/tools/7z.exe`), whose advisories are tracked in item 2 above.
- DONE (4bd3468c): the drive scanner has no 5 s timer; it rescans on disk and volume
  device-interface arrival/removal (`GUID_DEVINTERFACE_DISK`, `GUID_DEVINTERFACE_VOLUME`) through
  a 750 ms single-shot debounce, and re-runs once if a change lands mid-scan. Smoke and audit
  modes now start it.
- DONE (780e67a7): the APFS writer's three always-set attestations are one `writable` flag;
  raw opt-in, target confirmation and the evidence ID still gate. The test-only execution-
  evidence validator and the unread `required_evidence` prose are deleted.
- DONE (780e67a7): partition/filesystem test hooks compile only under
  `SAK_PARTITION_FS_TEST_HOOKS`; `test_release_binary_test_hooks` scans the release libraries and
  the exe. Other `*ForTesting` functions outside the partition/filesystem headers (path_utils,
  permission_manager, process_runner, smart_disk_analyzer, app_readonly_actions, AI panel) are
  not in this package and remain.
- DONE (92ffd385): `SAK_DEV_SOURCE_TREE` and the smartctl `tools/generic` fallback are gone
  (the assistant's workflow check used the same fallback; it now asks the analyzer).
- OPEN: HFS+ partition edits through Partition Manager still stage the whole partition in
  %TEMP%. The per-file HFS+ edit dialogs already moved to File Management, which writes the raw
  partition directly; the Partition Manager `Hfs*` operation types are reached only through the
  queue API, and the staging path is also what runs bundled `newfs_hfs`/`fsck_hfs` format and
  repair, which need a file. Moving the zero-skip copy into C++ with a free-space check, and
  retiring or rerouting the `Hfs*` script path, changes a raw write-back on real partitions; it
  cannot be proved safe with image-only tests in this change, so it was left fail-closed as is.
- FIXED (dcc0c67c; found 2026-10-25): `test_startup_budget` and
  `test_windows_runtime_dependencies` hard-coded `build\Release\sak_utility.exe`, so in a tree
  configured anywhere else they tested whatever binary sat in `build\Release` (a stale-binary
  trap) or failed with "input missing". The CTest registrations now pass
  `$<TARGET_FILE:sak_utility>` (and its directory as the release dir), and
  `scripts/assert_built_target.ps1` refuses an empty, missing, out-of-tree or stale target (older
  than the newest `sak_*.lib` beside it); `test_assert_built_target` pins each refusal.
  `scripts/check_startup_budget.ps1` run by hand still defaults to `build\Release`.

#### Behavior package status (2026-10-25)

- DONE (4947cf97): the thermal readout polls only while the Diagnostics panel is shown
  (start on show, stop on hide) at its own `kThermalReadoutPollIntervalMs` (5 s; it borrowed the
  2 s peer-discovery constant), the query runs on a pool thread, a missing sensor is warned about
  once (again only if it changes), a failing query once until it succeeds, and the smoke/audit
  skip is gone (`test_diagnostic_benchmark_panel`, `test_thermal_monitor`). Kept the PowerShell
  seam rather than in-process WMI: one of the three sources is `nvidia-smi`, and the reliability
  counters come from Storage cmdlets, so WMI in-process would cover only the CPU zone.
- DONE (334f0463): the program list no longer walks install folders (Win32 or UWP); it shows
  the registry's EstimatedSize (0 = unknown). Program Properties has an Install Folder Size row
  with a Measure button that walks that one folder on a pool thread, cancelled when the dialog
  closes, and cached per folder for the session (`test_program_enumerator`,
  `test_advanced_uninstall_panel`).
- DONE (8c5da388): the continuous WiFi scan's first scan runs on the controller's worker thread
  (it ran synchronously on the GUI thread inside `startContinuousScan`), and the Network panel's
  adapter scan runs in smoke and audit modes too, still off-thread
  (`test_network_diagnostic_controller`, `test_network_diagnostic_panel`).
- DONE (4cfc4f7c): the detachable log window stays on the screen's work area when opened beside
  a maximized main window, opens without activating (no `activateWindow`/`raise`), is not
  rebuilt on tab switches while hidden (refilled once on show, in one edit block), and the
  uncalled capped `appendLog()` is deleted (`test_detachable_log_window`). The MainWindow wiring
  (`onTabChanged` calling `replaceLog`) is by review: no test constructs MainWindow.
- DONE (e77adb72): one info-level startup line, `Main window displayed - application ready in N
  ms` (from process creation); main's construct/show/font timings and the Organizer, App
  Installation and Diagnostics panel construction breadcrumbs are debug, and the 7-line banner
  is one version line. `test_startup_budget` checks the real app's log. `MainWindow::setupUi`'s
  phase lines followed in ecaf2b99 (UI package status below).
- DONE (76167775): `AppInstallationPanel` no longer skips its connections and Chocolatey
  initialization under the accessibility audit (UI package status below).

#### Portable and lifecycle package status (2026-10-25)

- DONE (1cd0e976): no registry settings, no process-wide TEMP, one data root. UI state that 35
  bare `QSettings` and the Quick Actions block wrote to HKEY_CURRENT_USER goes to
  `data/config/ui_state.ini` (`sak::PortableSettings`) or ConfigManager; existing HKCU values are
  not migrated, only no longer written. TMP/TEMP are no longer redirected for the whole app;
  the Chocolatey children get `data/temp/session-<pid>`, removed at exit, and startup removes
  session folders of dead processes. The data root is decided once with no fallback: an
  unwritable portable folder is an error the user is told about, and the app exits. The unused
  `portable.ini` is no longer staged or in the repo. Drill note: the "no process-wide TMP"
  source check sits after the stale-session check in the same test, so its drill was masked by
  that one; the stale-session drill went red.
- DONE (0fe1805c): old session logs are pruned when the logger starts (newest 30, 200 MB).
- DONE (72878a61): declining the UAC prompt from Run as Administrator no longer shows
  "Elevation Failed". The decision function is tested; the MainWindow call site is by review.
- Fixes for the 2026-10-24 audit defects above: 76ca7a52, 4e47cf6c, 04109495, 4f3af0fc,
  c4edbb04.

#### UI package status (2026-10-31)

- DONE (c2a22e87): the theme follows Windows. One saved setting, `ui/theme` in ConfigManager's
  portable `data/config/Utility.ini` (`system`, `light` or `dark`; unset or unreadable means
  System), chosen from the status bar's Theme button (a System / Light / Dark menu). System follows
  the Windows app theme live through `QStyleHints::colorSchemeChanged`; Light and Dark are requested
  from Qt (`QStyleHints::setColorScheme`), so the title bar follows too. The Dark switch that
  `MainWindow` added beside every panel's Log switch is gone; it was never saved, so there were no
  keys to remove. Every built tab renders in both themes: inline styles name colours through
  `palette()`, painted colours come from the palette at paint time, and the status and note tints
  have dark counterparts. A review of every tab and inner tab in both themes (45 each) found two
  light-only colours, both fixed: the dark palette's Dark role, which the File Explorer uses for its
  secondary text, was a surface shade; and the disabled-button fill was an opaque light slate.
  `test_app_theme` switches the real window live, checks no widget keeps a colour of the other theme
  in its own style sheet, checks muted text and disabled buttons in each theme, and can save every
  tab as PNG for review.
- DONE (ac7640a4): 1px borders replace the app-wide drop shadow. The application filter that put a
  `QGraphicsDropShadowEffect` on group boxes, cards and rounded filled widgets (the whole File
  Explorer matched) and the InfoButton popup's own effect are gone; every surface they decorated
  already draws a 1px token border (`test_app_theme`).
- DONE (51daad6b): shortcuts and the wheel. The Ctrl+Shift+1 collision was one of five: the main
  window's application-wide Ctrl+1..9, Ctrl+Shift+1, Ctrl+Tab and Ctrl+Shift+Tab were each also a
  File Explorer binding, so with focus in the explorer Qt fired neither. A window shortcut now steps
  aside while the focused panel binds the same sequence. There was no application-wide wheel filter:
  the main tab bar switched panels on the wheel and the AI panel kept a per-widget copy for seven
  controls. One policy (`sak::ui::installWheelPolicy`) replaces both: the wheel never moves focus,
  an unfocused combo/spin box/slider passes the wheel to the view it sits in, a tab bar never
  switches on the wheel (`test_window_input`).
- DONE (ecaf2b99): `MainWindow::setupUi`'s phase lines, the per-panel "creating"/"initialized"
  lines, the lazy-tab build times and the smoke-only breadcrumbs (MainWindow and AI Assistant) are
  debug. The one info line is `Main window displayed - application ready in N ms`; the smoke-only
  `MainWindow startup: all tool panels materialized` stays info because the portable e2e smoke
  requires it. `test_startup_budget` refuses each breadcrumb in the real app's log.
- DONE (76167775): `AppInstallationPanel` runs its connections and Chocolatey initialization in
  every mode, and so do the remaining audit-only skips (MainWindow's window-state load/save and
  tab-bar wiring, the OST Converter's settings). `test_app_installation_panel`; the accessibility
  audit passes.
- DONE (b051f99c): one instance per data folder (`sak::SingleInstanceGuard`). A second launch on the
  same data folder asks the running instance to restore and come to the foreground, and exits 0; a
  copy in another folder runs side by side. Two session-local named objects named after the data
  folder's final path (junctions and case resolved, SHA-256): a mutex the instance owns, and an
  auto-reset event a second launch sets. Both carry a same-user DACL with a Medium no-write-up
  label, so an elevated instance and an unelevated launch can open each other's objects. Elevation
  handoff: Run as Admin (and the elevation gate) relaunches `sak_utility.exe` through
  `ShellExecuteEx` "runas" with the current arguments plus `--elevated-relaunch`, then quits. The
  relaunch waits up to 60 s on the mutex instead of signalling; the old instance never releases it,
  so ownership passes only when that process has ended -- after ConfigManager's settings are written
  in static destruction -- and the administrator copy never reads `Utility.ini` while the old one
  writes it. A declined UAC prompt leaves the old instance running and still owning the mutex. The
  elevated helper (`sak_elevated_helper.exe`) and the win32 MCP / browser-host modes of
  `sak_utility.exe` (which return before any of this) are not guarded. A user's own "Run as
  administrator" while an unelevated instance runs is a second launch: it brings that window
  forward, whose Run as Admin button does the handoff. `test_single_instance_guard`,
  `test_single_instance_app` (the real app: a second launch exits 0 and says so; an
  `--elevated-relaunch` launch waits for the first instance and then starts); the UAC path itself
  needs an interactive run.

#### Dedup package status (2026-11-01)

The duplicate implementations approved for removal as a class (behavior audit above). Each group
kept the most hardened copy, routed every caller to it and deleted the rest; lines are the
deletions of that group's commits (`git show --numstat`).

- DONE (af313a2f; -2848 lines): zip. Kept the explorer's in-process engine (FileExplorerArchiveService:
  bounded central directory, entry/expansion caps, zip-slip and junction refusal, rollback). The
  .nupkg extract/repack (.NET ZipFile in a PowerShell child) now calls it; a skipped entry fails a
  package. UserDataManager's Compress-/Expand-Archive backup API had no caller since dec179d8 and
  is deleted with its test; its one live function (scanForAppData) moved into its only caller.
  Chocolatey 2.7.5 installs a repacked .nupkg (directory entries, no OPC parts). The advanced
  search's own zip reader is a different job (reads entry text from an already-bounded buffer for
  search, never writes) and stays.
- DONE (7d17abbe; -577 lines): NuGet. Kept the paginated FindPackagesById client (every rel=next
  page, same origin, well-formed body), now PackageInternalizationEngine::fetchFeedVersions; version
  resolution, both .nupkg hash lookups and both verified downloads use it. Fixed by the merge:
  resolution and the hash lookup read page 1 only. Version resolution keeps its 3 attempts (a Stop
  now ends the wait); the hash is looked up before the download.
- DONE (1bfcc2e8; -218 lines): Qt deploy. Kept scripts/deploy_qt_runtime.ps1 (validated windeployqt
  plan), now called by stage_portable_release.ps1 on the staged exe; the unused CMake install route
  is deleted. The package no longer ships platforms/qoffscreen.dll (a test-suite plugin the
  stager's directory copy swept in).
- DONE (472bc697; -243 lines): elevation. One check (ElevationManager::isElevated, four forwarding
  wrappers and canElevate deleted), one UAC request (launchElevated, shared by the relaunch and the
  elevated helper; a decline is elevation_denied for both), one relaunch (requestElevatedRestart,
  shared by Run as Admin and every elevation gate: a gate now asks about running work first and
  shows nothing when UAC is declined). Broker ownership is unchanged; see the quick-action item.
- NOT a duplicate, left as is: the two Generate Report features. The Diagnostics panel's
  "Generate HTML Report" (`DiagnosticReportGenerator`) renders the results of scans the technician
  ran -- benchmarks, SMART verdicts, stress test, findings, ticket fields -- as HTML/JSON/CSV;
  "Generate Report" (`GenerateSystemReportAction`, also the assistant's read-only
  `action.generate_system_report`) collects a live inventory -- Windows licence status, uptime,
  network adapters, volumes -- into a text file. Five hardware fields overlap; neither can replace
  the other without new collection work, so neither is redundant as the approval requires. The
  shared name and the fixed `C:/SAK_Backups` were decided under "behaves like a normal Windows
  tool" and are done: see the cleanup package status below.
- DONE (harness d61b093c, consolidation a4136249): the three quick-action controllers. The
  harness came first: each panel takes the controller it runs on, the output folder lives on the
  QuickAction base, ElevationBroker's task entry points are virtual and the controller builds its
  broker through a factory only `SAK_CORE_TEST_HOOKS` builds can replace, and
  `test_quick_action_panels` drives every panel flow (click, confirmation, folder choice, run,
  completion, exit cancel) through a real controller with fake actions and a fake broker
  (`tests/unit/quick_action_fakes.h`), passing on the per-panel controllers before anything was
  consolidated. Then MainWindow creates one controller with the ten built-in actions
  (`createAppQuickActionController`) and hands it to the three panels and the assistant (a
  shared_ptr: the assistant panel can outlive the window while its workers drain). A request is
  a `QuickActionRun` {action, owner, output folder}; every signal carries its run and callers
  react only to their own owner token (`connectRunsOf`; a fixed token per panel, a fresh one per
  assistant call); every request ends in exactly one `runFinished`, with the run's own result by
  value. Fixed by it, each with its drill: the cross-caller folder (a queued run wrote into the
  folder set last on the shared action object; it is now applied from the run as it starts), the
  assistant bridge settling on another caller's run of the same action and its Stop/timeout
  cancelling whatever ran, a refused BitLocker folder silently falling back to the previous one
  (an unsafe folder now refuses the run before the helper), results read back from an action a
  later run could be writing, and action warnings emitted to nobody. A request while another
  runs is queued and announced ("Queued: X - waits for Y to finish"). Removed with the per-panel
  controllers, each with no product caller: the scan route (`scanAction`), `getActionsByCategory`,
  `setLoggingEnabled`, `setBackupLocation`, `isExecuting` and `require_confirmation`.
  Broker ownership (decided here): the controller is the one owner of the broker its
  administrator actions use, created on the first elevated run and kept for the controller's
  lifetime, so a session's quick actions share one helper and one UAC prompt while the helper is
  inside its idle timeout (ElevationBroker retires one near it). No other feature uses it. A
  cancel of the run using it reaches the helper task (a panel's cancel on exit through
  RunningOperations, the assistant's Stop or timeout); destroying the controller at exit cancels
  the running task, joins within the 10 s bound and destroys the broker, which shuts the helper
  down; an overrunning worker goes to ThreadCustody with its action and the broker. Live UAC
  behaviour (one prompt for two queued administrator actions) needs an interactive VM run;
  CTest covers reuse, per-owner cancel and exit cancel with the fake broker. Proof at 7cac75ea:
  full build, T2-label suite 279/279, stage, archive, clean-extract portable smoke and startup
  E2E, accessibility audit and startup budget (build tree, unelevated), the ci.yml static gates.
  The readiness gate stops at the certification state block in this worktree, which lacks the
  main checkout's external evidence.

Fixed in the kept copies on the way (each its own guard and drill): the archive engine wrote
archives QZipWriter cannot encode (65,536+ entries read back as 0, 4 GiB+ wrapped), and followed
directory junctions when compressing (2b98a864).

#### Cleanup package status (2026-11-01)

Dead code, package files nothing loads, repository leftovers and two owner-ruling fixes, approved
as a class (behavior audit above). Each removal was proved unused first; lines are the commit's
deletions (`git show --numstat`; whitespace-insensitive where a block was only unwrapped).

- DONE (0249712e; -1,320 lines): dead dialogs, functions and settings. `SettingsDialog` and
  `LogViewer` (nothing included or built either), `MainWindow::setProgressVisible` and
  `QuickActionController::scanAllActions` (no caller), User Migration's "Settings" popup (its
  thread count, MD5 verification and backup location were read by no backup code), the settings
  keys only those used or nothing read (`backup/*`, `quick_actions/*`, `organizer/preview_mode`,
  `duplicate/*`, `ui/restore_window_geometry`, which nothing set) and the unconnected
  `ConfigManager::settingChanged`. `ConfigManager::removeRetiredKeys` drops them from an existing
  `Utility.ini` on load (the precedent was `show_system_drive_warning`).
- DONE (30aa413a; -647 / +102 lines): non-Windows `#else` branches, the `SAK_PLATFORM_WINDOWS` /
  `PLATFORM_WINDOWS` machinery and `SAK_WIN_REGISTRY_TESTS` (64 files). The build-system lint now
  forbids a platform conditional in every first-party source instead of a 26-file allowlist.
- DONE (9d173ce0; -79 lines): the env-var escape hatch `SAK_LEFTOVER_TECHNICIAN_OVERRIDE` and the
  `technician_override` argument it honoured; leftover cleanup's proof-of-scan has no bypass. No
  other escape hatch is left in a production path: the remaining environment reads are Windows
  locations (`SystemRoot`, `LOCALAPPDATA` and the like), the win32 MCP mode switch the AI layer
  sets for its own child, and the two startup-smoke variables below.
- DONE (abd4eebd): package diet. Package 158 files / 209,978,320 bytes (200.3 MB) -> 148 files /
  184,497,349 bytes (176.0 MB); ZIP 103,401,619 -> 93,315,042 bytes. Left out, each proved unused by
  dumpbin and by the modules a staged app loads (accessibility audit, interactive session, live
  HTTPS): Mesa `opengl32sw.dll`, `D3Dcompiler_47.dll`, `generic/qtuiotouchplugin`,
  `networkinformation/qnetworklistmanager`, `tls/qcertonlybackend`, `concrt140.dll`,
  `msvcp140_codecvt_ids.dll`, the retired `tools/mcp` README, and Chocolatey's runtime state.
  Kept because they load: `qwindows`, `qmodernwindowsstyle`, `qsvgicon`, the gif/ico/jpeg/svg
  image formats and `qschannelbackend`. The stager's dependency check now covers every binary the
  app process loads (`-WholePackage`) and found `qwindows.dll` importing `mscms.dll`, which the
  classifier lacked. Proof on the trimmed package: stage, archive, extract, portable smoke,
  startup E2E, runtime dependencies, accessibility audit (`missing=0 checked=1950`), and an HTTPS
  request through the package's own Qt runtime (`test_linux_iso_downloader`
  `testDirectUrlReachable_Ubuntu`, run from a package copy, loaded only `qschannelbackend`).
- FIXED on the way (abd4eebd): `check_startup_budget.ps1` forces Qt's offscreen platform, which a
  package has not shipped since 1bfcc2e8, so against a package it sat at Qt's missing-plugin
  message box and reported a hung startup after 60 s (also on the untrimmed package). It refuses
  such a folder at once; the gate stays a build-tree gate (4.9 s of 15 s here), and a package is
  timed by `run_portable_e2e_smoke.ps1`.
- DONE (7e163c49; -1,127 lines): repository leftovers. Dev scripts `tools/count_methods.py` and
  `tools/decompose_execute.py` (both broken, and shipped because tools/ is copied wholesale),
  `package_mappings.json` with the test-only `PackageMatcher::exportMappings`/`importMappings`,
  `.vsignore` (no tool reads it), `scripts/create-release-tag.ps1` (unreferenced; it accepted tags
  the release rule forbids), six icons no code names and `resources/icon.icns`, stale
  `.gitignore` rules (and the UUP converter `.exe` exception, which the later `*.exe` rule
  overrode), the unreachable GCC/GNU-driver CMake branches with `SAK_ENABLE_MARCH_NATIVE`, the
  `ENABLE_UBSAN`/`ENABLE_TSAN` options (one only printed SKIPPED, the other could only fail
  configure) and the unread `SAK_BUILD_TYPE`. Configure now refuses a GNU-style driver. The local
  Chocolatey state (`tools/chocolatey/logs`, `bin/_processed.txt`, `bin/RefreshEnv.cmd`) was
  deleted from the main checkout and is excluded from the build's tools copy (abd4eebd).
- DONE (3dd86c2e): the two Diagnostics reports have distinct names -- the Scan Report ("Export
  Scan Report", HTML/JSON/CSV of the scans run) and the System Inventory Report (the live-inventory
  quick action; the assistant's id is now `action.system_inventory_report`) -- and the Full Suite
  step that only aggregates results is "Aggregate Results". The inventory report asks for a folder
  each time (starting in Documents) instead of writing to a fixed `C:/SAK_Backups`, and the
  completion status names the saved file; the assistant keeps writing to the session's artifacts.
- DONE (47dc2624): the intermittent `sessionPickerListsSessionsOffTheGuiThread` was the test's wait
  (a count of 1 that the constructor's draft row already satisfied), reproduced on demand and
  fixed; the suspected product race does not occur (see the infrastructure item below).
- Not found: the "job-object loop guard" named for this package. Nothing in the tree, docs (archive
  included), ignored files or commit messages since 2026-10-20 carries that name or describes a
  loop guard around a Job object. Closest: `ContainedProcessQuarantine::retryRetained` (a retry
  over quarantined Jobs) has no product caller, only the MCP test fixture's drain; quarantined
  owners stay until exit by design (kill-on-close), so it was left.
- DONE (a6b5f4ef): `ConfigManager::resetToDefaults`, and `ConfigManager::clear` (also test-only),
  are `resetToDefaultsForTesting` / `clearForTesting`, compiled only under `SAK_CORE_TEST_HOOKS`
  into the two config suites (the fa5db306 pattern); `test_release_binary_test_hooks` refuses a
  `ForTesting` member of ConfigManager in sak_core.lib. Moving them was simpler than removing them:
  the suites need a way to rebuild the defaults from outside the class.
- DONE (790d5577): Screenshot Settings asks for a folder every run (starting in Documents) instead
  of writing to the fixed `C:/SAK_Backups`, and its completion (and BitLocker Key Backup's) names
  the folder it saved to. A capture folder it cannot create now ends the run before any Settings
  page opens; it was logged and every capture then failed on its own.
- Left, with reasons: `SAK_STARTUP_SMOKE_HEADLESS` and `SAK_STARTUP_SMOKE_CI_HEADLESS` are
  two names for the gates' headless smoke mode, not an escape hatch (one name would do; the e2e
  smoke's tests pin both); seven File Explorer icon-registry entries
  (`view-*-28`, `favorite`, `properties-security`) are requested by nothing but a test.
- FIXED (19b9aeab; the product was never broken): `test_linux_iso_downloader`
  `testGitHubVersionCheck_Network`. Its "15 s wait" called `processEvents(AllEvents, 100)` 150
  times, which returns as soon as no event is pending, so it gave up in milliseconds and said "did
  not respond within 15 seconds"; the catalog's destructor then aborted the still-pending reply,
  and that abort was the logged "Network metadata request cancelled". The app's version check
  (LinuxDistroCatalog::checkLatestVersion, reached from a GitHub-hosted ISO download) runs in the
  GUI event loop and was fine. The test now waits with `QTest::qWaitFor` past the product's own
  30 s transfer timeout and reports the check's own error. Live run against api.github.com
  (opt-in, once): Ventoy v1.1.18 resolved in 344 ms.
- Fixed-drive-path sweep (2026-11-01), every `"X:\"`/`"X:/"` literal in product code. Treated the
  way the inventory report was: no fixed location is assumed.
  - DONE (790d5577): Screenshot Settings' `C:/SAK_Backups` (above); MainWindow and the panel carry
    no `C:/SAK_Backups` any more.
  - DONE (7cac75ea): leftover cleanup's protected system folders were written for a C: install only
    (`LeftoverScanner::kProtectedPaths`); with Windows, ProgramData or Users elsewhere only the
    container NAME stayed Risky (the leaf check), so a leftover under `D:\ProgramData\Microsoft`
    was classified like any other folder. They are now also rebased onto this machine's folders
    from the known-folder API (`protectedPathsFor(liveSystemFolders())`); the C: entries stay.
  - DONE (7cac75ea): Partition Manager's Rebuild MBR / Boot Repair passed `C:\Windows` to bcdboot
    and the script builder fell back to it when none was named. The panel passes the running
    system's Windows folder (GetWindowsDirectoryW via `windowsDirPath`), and the builder refuses a
    boot repair with no `windows_path`.
  - DONE (7cac75ea): the email file scanner skipped `C:\Backups` and `C:\Email Backups` on the claim
    that the profile paths covered them; it scans every writable drive's.
  - DONE (7cac75ea): `StressTestConfig::disk_test_drive` defaulted to `C:\`; it has no default, and
    the worker already refused an empty target (every caller names the technician's drive).
  - Left, with reasons: `systemDriveLetter()` in `app_mutating_actions.cpp` reads
    `%SystemDrive%` and falls back to `C` when it is unset (Windows sets it for every process,
    and the flash refusal it feeds also checks the disk's own system/boot/EFI flags);
    `leftover_cleanup_guard.h` reads `%SystemRoot%` with a `C:/Windows` default, but only to ADD
    a non-standard Windows folder name -- the `windows` tree is refused on every drive by
    `cleanupCriticalTreeRegex` either way; the BitLocker recovery document's
    `Unlock-BitLocker -MountPoint 'C:'` is an example line for the reader, not a path the app
    uses. The first two would be better read from GetWindowsDirectoryW; neither assumes C: when
    Windows reports its own location.
- DONE (3c08e191, 28fee6ed), found here (dead code, the approved class). `QuickAction::scan()`:
  no caller in `src` or `include`, directly, through `&Action::scan`, a connect or
  `invokeMethod("scan")`; only `test_quick_action`'s stub called it. Removed from the interface and
  the eight actions, with what only it fed (`ScanResult`, `lastScanResult`, `scanProgress`,
  `scanComplete`, the `Scanning` and `Ready` statuses, `scanResultFor`). The scan logic `execute()`
  uses stayed: volume detection, drive enumeration, the active-plan query. `quick_action_result_io`:
  no includer outside its own suite, so it and the suite are gone.

### Open release infrastructure

- Intermittent, cause unidentified: the full-history trufflehog scan in `scripts/scan_secrets.ps1`
  exited 1 twice on 2026-11-01 with no finding and no "finished scanning" line, and passed on the
  four runs after that with the same flags. It fails closed, so the cost is a repeated push or
  readiness run, not a missed secret. Every run also logs "a detector ignored the context timeout"
  for its PrivateKey detector on `tools/uup/aria2/libssh2-1.11.1.tar.xz`, passing or not; the two
  are not shown to be related.
- Tiered gates: the local tiers (T1 commit hooks, T1p pre-push, manual-stage T2-T4 twins), the
  meta-gate retirements, CTest labels, resource locks and parallel runs are in place
  (2026-10-24). Done 2026-10-24: `.github/workflows/ci.yml` runs T2 on every push to `dev` and
  `main` and every PR (sharded build, anchored label selection); `release.yml` is T3 on tags;
  `nightly.yml` is T4. Open: `release.yml` and `nightly.yml` can be dispatched only from the
  default branch, so neither has run yet.
- Intermittent, cause unidentified: once in a `-j 8` full suite (2026-10-24)
  `test_advanced_uninstall_controller` failed because `ConfigManager::sync` got a settings access
  error on the shared `data/config/Utility.ini`, at the serial tail with no other CTest test
  running; seven serial reruns of the same sequence passed and Defender real-time protection is
  off. If a user's machine can hold that file the same way, a settings save fails visibly
  rather than silently, but it fails.
  Investigated 2026-10-25 (still open, not reproduced): inside one process ConfigManager is the
  only object on `Utility.ini` (UI state now has its own `ui_state.ini`, and the Quick Actions
  settings moved into ConfigManager). A second writer exists only across processes: two app
  instances at once (there is no single-instance guard), or CTest suites sharing
  `build/Release/data`. Every suite found writing there is in the `portable_data_root` lock;
  `test_file_management_explorer_panel` joined it when it started writing `ui_state.ini`.
  2026-10-31 (b051f99c): the app-instance route is closed. A second launch on one data folder no
  longer starts a second instance, and Run as Admin no longer overlaps the closing instance (which
  could still be saving its window state to `Utility.ini` while the elevated copy started and read
  it): the elevated copy waits until the old process has exited. What remains is the CTest route:
  the test executables are not the app, take no instance claim, and share `build/Release/data` with
  any app started from that folder during a run -- the `portable_data_root` lock orders the suites
  among themselves, not against a developer's running app. Whether such an app was behind the one
  2026-10-24 failure cannot be established now; the item stays open until it recurs with a cause or
  a run of full suites shows it gone.
- FIXED 2026-11-01, a test timing assumption, not a product race:
  `test_ai_assistant_panel_tool_dispatch` `sessionPickerListsSessionsOffTheGuiThread` failed once
  under `-j 6` (`itemText(0)` did not start with "Listed chat"). The panel's constructor lists the
  default store, which can leave exactly one row (the "AI Session  (new)" draft), and the case
  waited for a count of 1, so a slow pool passed the wait before its own listing landed.
  Reproduced on demand by holding the listing 1.5 s in the manifest-read hook (the same assertion
  failed); the case now waits for its own row. The suspected product race, a replaced listing's
  queued finished() making the GUI thread wait on the new listing's result(), does not occur: a
  probe that left the first finished() queued, replaced the future and held the new listing 5 s
  returned from the event loop at once (Qt drops the replaced future's call-out).
- One shared core library so first-party sources compile once instead of once per test target.
- Done 2026-10-24: the Current Certification State block in `docs/PARTITION_MANAGER_CERTIFICATION.md`
  is generated from the computed claim level and both certification matrices, the T3 check fails
  when it is stale, and the regex-over-prose claim checks are retired.
- Done 2026-10-30: `vcpkg.json` pins zlib, bzip2 and liblzma through its builtin-baseline,
  installed per build tree; the CI binary cache is keyed on the runner image, the triplet and
  vcpkg.json with no catch-all restore key. clang-format and the nightly clang-tidy are one LLVM
  release, 23.1.3 (`requirements-gates.txt` for CI and the local `.venv`; `nightly.yml`).
  cppcheck is 2.22.0, its MSI pinned by SHA-256 in `.github/actions/windows-build-env`; the
  whole-project gate floor is 180 of its 189 checkers (2.22 runs 186 over this tree).
- Done 2026-10-24: certifier and writer-CLI script tests delete their run folder when they pass,
  keep failures for diagnosis, and cap leftover failed runs at the newest three
  (`scripts/run_folder_retention.ps1`). They once held over 1 TB under `build/test_results`.
- Refresh `THIRD_PARTY_LICENSES.md` against what actually ships (MSVC runtime, 7-Zip, cygwin1.dll
  and others are missing; Qt section names 6.5). Mesa opengl32sw and D3Dcompiler no longer ship
  (abd4eebd).

## Current gate boundary: Wave 69 (2026-10-24)

Commits 6e71d676 (held waves 32-68), a8b64bbd (MCP containment), 7567e23f (test environment
fixes) and ac21ea2d (secret-scan tool resolution) were pushed to `origin/dev`. Gate on the final
tree: all-target Release build exit 0, 249/249 native CTests (build/wave69-full-ctest.stdout.log)
and 51/51 script CTests after the script fixes (build/wave69c-script-ctest.stdout.log). No
package, VM, accessibility or signing gate has run on these bytes.

## Previous gate boundary: Wave 68 source candidate (2026-09-25)

W67c is the last complete local gate: Release build and 300/300 CTests passed,
but its fresh native package smoke timed out. Wave 68 has only targeted app
builds and copied portable-runner contract tests; its native 30-second smoke
still fails. Font warmup/census removal and a narrower alias-only removal
were both restored under the owner's keep-only-if-verified condition. A
targeted Release app rebuild passed for restored source, but no complete
source gate followed. Current-binary package, accessibility, Windows 11 VM,
Windows 10, hardware, security and signing gates remain open. The archived R5
document holds item history; `docs/GUI_STARTUP_FIELD_NOTES.md` owns the timing detail. Future
plans under `docs/future/` remain parked.

## Historical package/VM proof: Wave 44 (2026-09-23)

The archived R5 document holds item history. Wave 44 passed a full
VS2022 Release build and all 287 registered CTests. Fresh portable ZIP staging,
static/runtime dependency checks, native Windows startup from staged and
extracted folders, and exact 159-file ZIP/extract hash comparison passed. On
the Windows 11 x64 VM, the transferred ZIP hash matched, 159 files extracted,
package startup smoke passed, and the app's accessibility audit reported
`SAK_ACCESSIBILITY_AUDIT_OK missing=0 checked=1950`. Evidence is under
`build/wave44-*`, including `build/wave44-vm-windows-startup-evidence.md`.
The retained Wave44 ZIP and `build/wave44-real-package-archive.log` still hold
its archive and binary hashes. A Wave45 staging-test attempt removed the old
global `Release/SHA256SUMS.txt`; it must not be cited as a current manifest.

Wave 45 changes ProcessRunner and the elevated helper after these package/VM
receipts. Wave 44 is historical proof, not proof of the current binary.
Neither wave certifies interactive
console UI, Windows 10, raw disks, external Apple media, hardware, live model
calls, signing, or the whole release. Seven vendor `/w` override warnings and
their 48 revealed strict diagnostics remain open, as do ProcessRunner process-
tree certification and portable-staging path replacement. The 25 reviewed local Qt
skips are not treated as executed external or destructive tests.

## Wave 48 local/Windows 11 scope (2026-09-24, not release certification)

The current-source VS2022 Release build and all 287 CTests passed. An earlier
full run failed 1/287 when integer-rounded MCP cleanup timed out before its
deadline; passing the original QDeadlineTimer to the semaphore fixed that
defect. Focused test passed 20 repeats and a copied-source early-return drill
went green/red/restored-green. Skip audit found 25 reviewed skips, none stale.
Whole-tree cppcheck, stale-inline-suppression reveal, dead-code probe, full
repository secret scan, and extracted-package Gitleaks scan passed.

Archive now builds and verifies candidate ZIP/checksum before publication,
retains prior outputs for checked rollback, and refuses incoherent prior
manifests or unresolved recovery markers. Owned PS7/5.1 fixtures exercised
compression and separate ZIP/checksum publish failures, same-name and new-name
releases, and copied-source rollback/prior-binding red drills. Fresh 157-file
ZIP had exact extraction hashes, no bundled logs, static/runtime check pass,
and native startup pass. Windows 11 VM matched ZIP/app hashes, passed native
startup and accessibility audit (`SAK_ACCESSIBILITY_AUDIT_OK missing=0
checked=1950`), and left no owned process or crash/temp/log items. Exact
evidence is `build/wave48-transaction-package-vm-evidence.md`.

One other fresh local stage of this same app binary timed out under the
unchanged 30-second native startup gate. Its later local/VM successes do not
erase that failure. Cold/intermittent startup is INCOMPLETE and blocks a
reliable release claim. Two-file archive publication is not crash-atomic;
interrupted transactions fail closed pending explicit recovery. The staging
path-swap race also remains open. Wave47 is historical for the older app and
archive script, not proof of current bytes.

Subsequent smoke-only startup phase diagnostics now flush the before/after Qt
and font-query boundaries, and the portable runner requires ordered markers.
Its 41 mocked contracts and copied-guard red drill pass. These newer source
bytes completed an all-target Release build and fresh 157-file stage/static
check. The first native stage then timed out under the unchanged 30-second
limit while complete CTest was concurrently running. Its flushed log stops at
the first GUI-thread font-family query after successful Qt initialization.
The complete CTest subsequently passed 287/287. Two independent fresh stages
of the same app hash passed the native smoke under PowerShell 7 and Windows
PowerShell 5.1 after CTest ended. See `build/wave49-startup-phase-evidence.md`.
The same W49 ZIP passed exact 157-file local extraction hashes, static/native
smoke, and extracted-directory Gitleaks. Windows 11 VM transfer/extraction
hashes matched, native smoke passed, and accessibility audit reported
`missing=0 checked=1950`; two runtime log items were retained after the audit,
so post-audit pristine state is not claimed. The earlier first-run timeout is
not resolved. Wave48 binary receipts are historical for these bytes.
Another uncontended fresh default-backend extract also timed out at the first
font query. An explicit-GDI arm passed, but a following A/B/B/A same-binary
comparison passed all four arms; no backend switch is justified yet. The
unchanged 30-second native startup reliability gate remains a release blocker.
After those W49 binary receipts, source-only cleanup removed the ineffective
DirectWrite override and escape hatch. Its old-code red/new-code green source
contract passed on both PowerShell hosts. W50 then passed a full Release
build, 287/287 CTests, current ZIP hash/extraction, and Windows 11 VM
native/accessibility checks. The first fresh local ZIP extraction still timed
out at the unchanged 30-second native gate: its forced font-family query
took 28839 ms. See `build/wave50-baseline-evidence.md`. W49 receipts are
historical for W50 bytes; the underlying timeout remains unresolved.
The owner-approved W51 lazy-font candidate compiled and passed 12 matched
fresh-ZIP native A/B/B/A runs, but whole-startup means were nearly equal and
neither arm reproduced the rare timeout. Under the owner's keep-only-if-
verified condition, it was reverted to exact W50 source hashes. The current
all-target build now exits 0, but the rebuilt app SHA256 is
`128403965F367238348DFA64B7CC0703FB68A6014E6F7169A9BBBBF656D6612C`.
The version header embeds build date/time; W50 package/VM receipts do not
certify these new bytes. W52 now has exact 157-file local extraction hashes,
static/runtime checks, an extracted-directory Gitleaks pass, and Windows 11
VM hash-matched native startup plus accessibility audit (`missing=0
checked=1950`). Final complete CTest passed 287/287; skip audit found 25
reviewed skips. The uncontended fresh local ZIP-extract native smoke passed in
22.823 seconds under the unchanged 30-second limit. See
`build/wave52-restored-evidence.md`. Earlier first-run timeouts remain a
release blocker; this later pass does not establish reliable startup.
Wave53 then removed five unnecessary nested test-launch policy overrides;
the app hash stayed unchanged, so W52 package/VM proof still binds to its
binary. The complete Release build and 287/287 CTest passed for the latest
test-script bytes; skip audit found 25 reviewed skips, none stale. Other
policy overrides and the startup reliability blocker remain open.
Wave54 removed nine additional test-only policy overrides with focused
red/green guards and real PS7/PS5 and Python test passes. The app binary is
unchanged; its full Release build and 287/287 CTests passed before Wave55.
Wave55 then repaired the portable-stage regression fixture's unbounded copy
of historical Release packages/ZIPs. Its unchanged 240-second CTest had
approached timeout at 221.77 seconds; the bounded fixture passed a focused
registered run in 84.74 seconds, and the full PS7/PS5 scripts passed with
independent exclusion drills. The first full gate failed 1/287 when a
different combined false-green test reached its 60-second timeout; staging
passed in 105.64 seconds under the same slow run. Portable and Ledger scopes
were then registered separately on PS7/PS5 without increasing their limits;
focused runs pass. The new full Release build and 289/289 CTests passed; in
that slow full run the split scopes took 16.55-22.83 seconds and bounded
staging 94.76 seconds under their unchanged limits. Skip audit found 25
reviewed skips. The actual production path-swap/crash-recovery risks are
unaffected.
Wave56 removed two more unneeded policy overrides from the review-unit and
release-readiness child launchers. Exact red/green guards plus isolated
success/failure and review timeout fixtures passed on PS7/PS5. No destructive
readiness workflow was invoked. The latest all-target Release build and
289/289 CTests passed, with 25 reviewed skips and none stale; hardware/VM
policy overrides remain individually open.
Wave59 removed the unnecessary Bypass flag from the VirtualBox guest-admin
inline probe. Its exact guard was red/green and the automation-VM security
fixture passed under PS7/PS5. No provisioning or destructive VM path was
executed. Its all-target Release build passed, but full CTest failed 1/290
when the second 18,043,854,848-byte APFS scratch image could not resize on
the system temporary drive. The failure log is retained at
`build/wave59-partition-core-failure.txt`; this is not a green release gate.
Wave60 routes that large test's images through the explicit
`SAK_TEST_IMAGE_SCRATCH_ROOT`. A focused run on D: (`Storage 4TB`, exFAT)
allocated both images but hit Qt Test's 300-second function timeout. The
failed-run images were removed after exact path/content checks; the log is
retained at `build/wave60-apfs-focused.txt`. With the documented Qt Test
timeout raised to 900000 ms, the same focused case passed on D: in 518182 ms;
its temporary directory cleaned itself up. The subsequent full D: diagnostic
was stopped after 174/290 tests: its partition-core test had 46 failed
assertions, and 13 test executables had failed under an all-exFAT temporary
environment. This is not a complete gate. The same full partition-core test
passed 322 assertions, zero failures on E: NTFS in 138731 ms. A direct
capability-based copy fix now has a successful targeted build, old-red/new-
green 8 KiB copy proof on D: exFAT, 4/4 APFS reader fuzz assertions on D:,
and 322 passing partition-core assertions on E: NTFS. It is not yet a full
gate: complete latest-source Release/CTest on E:, capacity/failure-cleanup
proof, and current-binary VM/package checks remain open. A separate new-image
sizing path still marks sparse only as a best effort, then resizes without a
non-sparse capacity preflight; its partial-image failure cleanup is also open
under R5-IDX-37. The copy fix does not close that create path.
Wave61 local gate then passed an all-target Release build and all 290 CTests
with temporary files on E: NTFS, in 1904.85 seconds. The skip audit found
5242 passed Qt functions and 25 reviewed skips, none stale. The built app
SHA256 is `8DD34358DBD3A5C2F9FDA5C5BEA2513689D9AA651AC0842856DF82C0F68D0C4A`.
The D: exFAT copy regression and APFS reader fuzz case passed on the fixed
binary. The fresh W61 package staged 157 files; ZIP SHA256
`3F4D4D893C5966F11277C2A9745E3FA762C83EBA226B5740D926166B555817D6`
matched on `WIN-11-VM`, and all 157 extracted member hashes matched locally
and on the VM. Local and VM native startup passed. The VM accessibility audit
exited zero with `missing=0 checked=1950`; its fetched status artifact hash
matched. The VM retained two log items/three extra runtime files after the
audit, not a pristine post-audit package. Details:
`build/wave61-local-and-vm-evidence.md`. Dense-capacity/failure-cleanup
drills, the separate APFS new-image capacity defect, reliable cold startup,
hardware and release gates remain open. This is local/package/Windows 11 VM
proof only, not full-app certification.
R5-IDX-38 also blocks file-backed APFS and HFS+ durability claims: the current
`flushDeviceBuffers` QFile branch flushes Qt buffers but does not call the
Windows `FlushFileBuffers` API for the OS file cache. Current green tests and
package startup do not prove those image writes durable across power loss.
New APFS image-format paths also close/hash the QFile without a checked OS
flush; a hash of cached bytes is not durable-write evidence.
Wave62 removed four blanket execution-policy bypasses from the certification
gap report's suggested commands. The real generated-list assertion went red
on the old text and the full certification-tool self-test passed under PS7
and Windows PowerShell 5.1 without overrides. No VHD or hardware command was
run. Host-side runbook commands now inherit policy; fresh extraction uses a
single-use path rather than deleting a fixed folder. The two UNC in-guest
Bypass examples still require separate proof or a direct fix. A fresh
complete Release/CTest gate for the changed script/test bytes is still
required; the W61 app/ZIP/VM proof binds unchanged binaries only.
That W62 all-target build exited zero, but complete CTest failed 1/290:
`test_process_capture` reached its existing 60-second timeout. A focused
registered rerun also timed out at a different subcase. W63 splits its core,
ownership, deadline, output, stdin and environment contracts into 12
separately bounded PS7/PS5 CTests (300 total), with no limit increase or
assertion drop. The focused 12/12 run passed. An initial CMake loop gave a
false static doc count (292 vs 300 live); explicit registrations now make
both doc-accuracy and live CTest JSON count 300. Full Release/CTest and skip
audit were still open at that point. W62 is not a green gate.
The first explicit W63 scoped run failed 1/12 in PS7 output. A repeated
output run showed the fixture's two-second Python receipt setup expired
before `parent.json` appeared; the test had hidden that original exception.
Controlled three-second parent and termination-utility receipt delays proved
the old waits red and bounded ten-second fixture-only waits green. Neither
production capture deadlines nor the 60-second CTest limits changed. Focused
PS7/PS5 output scopes passed after the fix. Each was then repeated five
times (10/10 green) under unchanged limits.
The final W63 all-target Release build exited zero and all 300 CTests passed
in 1613.35 seconds. Skip audit: 5242 passed Qt functions, 25 reviewed skips,
none stale. The app SHA stayed
`8DD34358DBD3A5C2F9FDA5C5BEA2513689D9AA651AC0842856DF82C0F68D0C4A`,
but the rebuilt helper SHA is now
`467AFBB372A9E8327531F3C8F0F7355E05B1FFD17380C504463A2BD098F1D778`.
Fresh W63 package/VM checks now bind the new helper: ZIP SHA256
`142B5B089986DA7C856FED559F67CF50A1447FB7A73E170EEA13742798FF3927`,
157/157 extracted member hashes matched locally and on Windows 11 VM,
native startup passed in both places, and the VM accessibility audit reported
`missing=0 checked=1950` with a fetched matching status hash. Two VM log
items/three extra runtime files remained after audit; see
`build/wave63-local-and-vm-evidence.md`. R5-IDX-35 through R5-IDX-38,
R5-IDX-40 and external/release gates remain open.
Wave64 has a gated shared APFS/HFS+ QFile flush fix. A read-only handle
test went red on the old Qt-only flush and green when the helper called
Windows `FlushFileBuffers` after Qt flush. The newly created APFS format path
still needs its own checked flush. The full partition-core test passed 322
assertions on E: and targeted cppcheck passed. The all-target Release build
then exited zero and all 300 CTests passed in 1924.96 seconds, with 25
reviewed skips and none stale. Current app/helper SHA256 values are
`3EE453FF4F755DAE13BB5D5421DD13C2E5A9B7A6A3B469F55F12BDA14B988FD3`
and `67B7E4546D11841B9F605046E4DE0EDEE14F43607FBA5992D475759F2EE0C622`.
Whole-project cppcheck passed with zero normal findings, 183/186 active
checkers and zero stale inline suppressions; dead-code probing found zero
high-confidence unique definitions. Fresh W64 ZIP SHA256
`04966536B01DBDA45D9D861F5EC229B255615FD8327E1AAD7A8DEAC72AB63BD5`
had 157/157 matching extracted files locally and on Windows 11 VM. Native
startup and VM accessibility (`missing=0 checked=1950`) passed on current
app/helper hashes. Two VM log items/three extra runtime files remained
after audit; see `build/wave64-local-and-vm-evidence.md`. This is not
physical power-loss certification, and the new APFS image-format path still
lacks its own checked OS flush.
Staging took 142.70 seconds under 240, and archive recovery took 171.41
under 180, versus 65.18/73.28 in W63; this sharply reduced timing
margin is open as R5-IDX-41, not a reason to raise limits. An isolated
focused rerun passed in 77.34/104.81 seconds, so the slow-run cause remains
unidentified. W63 package/VM proof is historical for the W64 binaries.
Wave65 moves the `/dev/sda` raw-looking-input assertion out of an inactive
non-Windows test arm and removes only that dead branch. A compile-successful
mutation of the production classifier made the now-active Windows test red;
exact source restoration rebuilt it green. This is a test-coverage fix, not
Linux application support. Full Release/CTest and current-package binding
were still open at that point.
The caller audit then found both Windows raw-device openers passing `/dev/...`
through to `CreateFileW`. Separate non-existent-path read/write tests were
red on the old OS error, and a whitespace-padded `/dev/sda` classifier test
was red. A shared canonical raw-open check now refuses these before native
open. Uppercase `/DEV/...`, backslash `\dev\...`, and padded mixed-case
spellings then made the classifier/read/write tests red on the old code;
normalized case-insensitive classification now routes them to that same
refusal. All three targeted tests and the full 324-function partition-core
test pass on E:, with no scratch residue. The all-target Release build and
300/300 CTests passed in 2361.54 seconds; skip audit found 5244 passed Qt
functions and 25 reviewed skips, none stale. Current app/helper SHA256 are
`9F52E92C8C35EAAE6047CC664B724BDF8E92983E7DB81D5FD2EC8DB621A8AC96`
and `332CFC4B96BDCAA73A72B30B9F880932F3C1290B6BBCBB8613D368C91B354DB0`.
Whole-project cppcheck passed with 183/186 active checkers, zero normal
findings and zero stale inline suppressions; dead-code probing found zero
high-confidence unique definitions. Fresh W65 ZIP SHA256
`7406BD0CEB3688E4DF2BBE1A4239B95D05E5AE2EF7B6A0B822CC4750EC4CDE45`
had 157/157 matching extracted members locally and on Windows 11 VM.
Native startup and accessibility (`missing=0 checked=1950`) passed on
current app/helper hashes. Two VM log items/three extra runtime files
remained after audit; see `build/wave65-local-and-vm-evidence.md`. W64
binary receipts are historical. The raw-path item is closed, but APFS/HFS+
capacity/durability, staging reliability and external/release gates remain.
W66's read-only cppcheck reveal removed the two broad missing-include
exceptions only in a private scan. It exposed 260 project and 5753 system
include diagnostics; 252 project names were generated Qt MOC files, while
the eight non-MOC names resolved under the real generated/vendor include
roots. The live suppressions and gate were not changed. Direct include
routing and a narrow, red-drilled MOC classifier remain open under R5-IDX-33.
The subsequent narrow QR include-root change removed its private-reveal
diagnostic, while a live LZFSE root candidate exposed a finding in vendored
code and was rolled back without a suppression. The all-target W66 Release
build and complete 300/300 CTest passed; whole-tree cppcheck, dead-code and
skip audits passed too (`build/wave66-local-evidence.md`). The rebuilt
app/helper hashes exactly match the W65 package-bound binaries, and neither
changed QA file is a ZIP member. W65 native/VM runtime receipts still bind
those binaries; the unproved LZFSE/generated/system include routing and
global algorithm-style exception remain under R5-IDX-33.
W67 subsequently changed `ai_command_guard.cpp`, a test source and the
registered QR reveal test. Focused builds/tests and private analyzer reveals
pass, but complete W67 CTest timed out `test_review_ledger` at 180 seconds
(299/300 passed, exit 8); the full gate is RED and package/VM binding for
these code bytes remains OPEN. W65/W66 runtime receipts are historical for the current
source candidate. A transient cppcheck capture-file cleanup lock remains
unfixed. The R5 tracking document was reconstructed from a verified Wave 41
Git object after a full-disk write failure; its recovery overlay, not older
historical checked markers, controls current claims. The three certifier
self-tests also retained unbounded image run folders in `build/test_results`;
successful-run cleanup is open and external evidence under `artifacts/` was
not deleted.
W67b then passed `test_review_ledger` in 85.99 seconds in the complete suite,
but `test_mutation_runner` failed its exact descendant-exit postcondition:
the complete gate remained 299/300, exit 8. Bounded Job member handle pinning
and fail-closed negative guards are now a focused-tested candidate under
R5-IDX-45. The full gate and current package/VM binding remain open.
W67c then passed the complete all-target Release build and 300/300 CTests
(skip audit: 5244 pass, 25 reviewed skips, none stale). Whole-tree cppcheck
and dead-code gates passed; the algorithm-style reveal fell to 280 without a
new suppression. A fresh 157-file stage passed static/runtime checks, but
its default 30-second native startup smoke timed out at the first font
database query (`build/wave67c-package-native.log`). One log remained, no app
process. Current package/VM and release readiness stay RED/OPEN; do not cite
W65 native/VM receipts for the changed W67c app binary.
Historical Wave57 adds explicit recovery for versioned interrupted archive transactions:
normal publication still refuses stale markers, while an operator-requested
recovery validates lock/snapshot/output hashes and either keeps a complete
candidate pair or restores the prior pair. Owned hard-crash/tamper fixtures
pass under PS7/PS5. Archive and staging scopes are separately registered
(290 CTests), with unchanged limits. Full Release build and 290/290 CTests
passed. The current-script W57 ZIP has exact 157-file local extraction,
static/runtime/native smoke and Gitleaks passes; Windows 11 VM ZIP/app/helper
hashes matched, with native smoke and accessibility audit passing. See
`build/wave57-archive-recovery-evidence.md`. Staging path replacement,
physical crash durability, elevated-helper live action and reliable cold
startup are not certified by those checks.

Interrupted archive recovery is operator-requested, not automatic. After
confirming no publisher is running, keep the exact lock and transaction
directory intact and invoke the archive script with the original build root
and package name plus `-RecoverInterrupted`. The command refuses an absent,
active, unversioned, wrong-name or tampered record; never delete the marker
or force a new archive over an unproved pair. Recheck the resulting ZIP and
`SHA256SUMS.txt` hashes before using them. `-CleanMutableRuntimeData` cannot
be combined with recovery. This runbook is for versioned process-interrupted
transactions only, not a power-loss certification or a path-swap workaround.
No W51 candidate full-suite or VM certification is credited.

This proves only local plus one VM path. Windows 10, visible interactive UI,
raw-disk and adapter APPLY, external media/hardware, live model, signing, and
the release matrix remain open. ProcessRunner cancellation and timeout cleanup
have separate copied-source red drills with outer Job containment; the
elevated-helper live action path has not been exercised on the VM. Do not
archive R5-P7-20 or mark the release certified on this scope.

## PUBLISHED CREDENTIAL: macOS cert-rig VM password (accepted risk, SETTLED)

The section that stood here described this as a PRE-PUSH blocker and stated that the finding was
being left failing rather than allowlisted. Both statements stopped being true on 2026-08-26 and
nothing updated them, which is exactly the staleness R5-IDX-7 tracks -- and worse here, because a
reader checking the project's security posture would have been told a control was armed that had
been deliberately, and correctly, stood down. Recorded as R5-IDX-20.

- WHAT: the macOS cert-rig VM password and its SSH hostkey fingerprint, committed in
  `docs/APFS_LIVE_RECERT_FOLLOWONS.md` (introduced at commit 08035cf1, flagged by
  `scripts/scan_secrets.ps1` on a full-history run as `generic-api-key`).
- WORKING TREE: redacted 2026-08-25. The doc points at the UNTRACKED `temp/creds.md`, so no
  FUTURE commit carries the credential. The historical blob is untouched and always will be.
- PUBLISHED. The branch was pushed to `origin/dev` on 2026-08-27 and commit 08035cf1 is an
  ancestor of it, so the credential is on GitHub now. This was an owner decision, not an
  accident: the VM is reachable only from inside his LAN, so he accepted the exposure as low
  risk rather than rewrite 1594 commits ("scrubbing the history sounds bad"). Options (2)
  rewrite history and (3) never push are therefore both spent.
- ALLOWLISTED, with authorization. The fingerprint
  `08035cf1...:docs/APFS_LIVE_RECERT_FOLLOWONS.md:generic-api-key:109` is entry 4 in
  `.gitleaksignore`, recorded there as a REAL credential accepted as low risk rather than
  dressed up as a false positive. It is one exact `commit:file:rule:line` fingerprint, so it
  silences that one historical finding and nothing else; a path glob or a bare rule name there
  would disarm the scanner for every future secret in the file, and that is still forbidden.
- WHAT NOW STOPS THE NEXT ONE: the `gitleaks-staged` pre-commit hook, added alongside the
  allowlist entry, scans the staged diff before a commit exists. The full-history scan could
  only ever report a credential that had already been committed; this one refuses it at the
  point it would enter history.
- NOT BEING ROTATED, and that is a decision rather than an omission. The owner ruled on
  2026-08-27 that the password stays: the certification-rig VMs are reachable only from this
  machine's local network, are not exposed on any external route, and hold nothing beyond the
  rigs themselves. The published string is a credential to a LAN-only target, which is the same
  basis on which the allowlist entry was authorized the day before. Nothing is waiting on
  anyone, and no agent should re-raise it as an open action.
- WHAT WOULD CHANGE THAT: if a cert rig is ever reachable from outside the LAN, or reused for
  anything beyond certification, the risk this decision accepted no longer holds and the
  password must be rotated before that happens. That is the condition to check, not the calendar.
- Tracked as R5-IDX-20, now closed.

## Automated Gates

Required before a release candidate is published:

- Full Release build succeeds.
- Full CTest suite passes with zero failures.
- Secret scan passes.
- Blocking-pattern guard passes.
- Accessibility, raw style-token, raw style-literal, GUI magic-number, global magic-number, logged dialog, Partition Manager certification-matrix-integrity/commercial-destructive-feature-matrix/external-checklist/external-lab-package/certification-gap-report/VHD-preflight/certification-artifact-bundle/feature-matrix/evidence-payload/release-claim/strict-handoff, and Lizard gates pass.
- Third-party license audit passes.
- Full driver-level APFS (A1-A8) and HFS+/HFSX (H1-H8) write is Apple-certified - Apple `fsck_apfs`/`fsck_hfs` + macOS-kernel mount at every milestone, with physical-USB destructive + crash-interruption + rollback at the A8/H8 track gates (evidence `external.apfs-a8-physical`, `external.hfs-h8-physical`). Capability status is owned by `certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`; the archived driver plan is its narrative realization record; this is a distinct scope from the Partition Manager destructive-operations claim level.
- Partition Manager non-native filesystem tools remain blocked unless
  `tools/filesystem/manifest.json` has pinned id/version/upstream/license/source-hash/binary-hash/file-system/operation metadata,
  matching bundled binary hashes, and hashed `runtime_files` for companion
  DLL/license/source/helper artifacts. CMake requires the manifest at configure
  time, `scripts/check_partition_filesystem_tool_manifest.ps1` rejects missing
  metadata, path traversal, missing binaries, hash mismatches, and unmanifested
  files under `tools/filesystem`, `scripts/check_third_party_licenses.ps1`
  requires license coverage for any approved manifest tool, and portable
  staging/smoke checks require the manifest in the release package. Current
  manifest approval includes e2fsprogs 1.47.4 `e2fsck` for ext2/ext3/ext4
  `check-read-only` and confirmed `repair`, `mke2fs` for confirmed `format`,
  and `resize2fs` for confirmed `resize`; it also includes hfsprogs
  540.1.linux3-6+sak-msys `newfs_hfs` for HFS+/HFSX confirmed sparse-staged
  `format` and `fsck_hfs` for HFS+/HFSX confirmed sparse-staged `repair`.
  Confirmed ext format, repair, adjacent grow, and same-start shrink now route
  through Pending Operations, safety validation, Apply review, elevated
  PowerShell execution, manifest/hash revalidation, and explicit confirmation,
  with destructive VM raw-media proof and destructive physical USB proof passed
  on 2026-06-05. HFS+/HFSX format/repair routes through the same queue,
  raw-target identity, manifest/hash, Apply review, and confirmation model with
  sparse staging, image-level tool proof, and physical raw-partition HFS mutation
  proof recorded. ext includes
  read-only directory listing, selected-file extraction, bounded recursive
  directory export through original parser code, and allowlisted elevated
  read-only raw-probe retry when the normal process cannot open
  `\\.\PhysicalDriveN`; Linux swap includes read-only header
  version/page-size/UUID/label metadata plus confirmed original SWAPSPACE2 v1
  format through Pending Operations, raw target identity, Apply review,
  generated UUID/label metadata, page-size selection, and explicit
  confirmation; XFS/Btrfs includes read-only superblock metadata, lightweight
  sanity notes, and original-code metadata consistency reports from captured
  probe data; APFS includes read-only container superblock, checkpoint ring
  indexes/windows, object-map OID, visible object-map tree-anchor metadata,
  visible referenced root/non-root B-tree node-header metadata, root
  `btree_info_t` metadata, volume-OID metadata, bounded probe-window
  volume-superblock candidate metadata, referenced-object-header preflight for
  known OIDs found in the probe window, lightweight
  block-geometry/checkpoint-window sanity notes, original-code metadata
  consistency reports, metadata object-checksum validation on APFS object
  blocks, volume browse, selected-file extraction, and bounded recursive
  directory export; HFS+/HFSX includes read-only header metadata,
  catalog consistency checks, attributes B-tree key scans with fork/inline
  metadata reporting, catalog listing, selected data/resource-fork extraction,
  selected attribute-value extraction, bounded recursive directory export with
  `.rsrc` sidecars, catalog/data/resource/attribute extents-overflow
  resolution, image-only data-fork overwrite, data/resource-fork
  allocated-block grow/shrink replacement, bounded initial-extent allocation
  growth, explicit zero-length truncate, constrained empty-file and empty-folder
  create/delete, single-leaf catalog rename/move, bounded file create with data-fork allocation, allocated-file
  delete, optional released-block zeroing for file/folder-tree delete, bounded folder-tree delete, inline-attribute and fork-backed-attribute
  replacement/growth with read-back verification through `sak_hfs_writer_cli.exe`,
  selected raw-partition staged data/resource-fork, single-leaf catalog rename/move, bounded create/delete with optional released-block zeroing, and
  inline/fork-backed-attribute mutation including bounded growth through the HFS File queue/apply path, and
  confirmed sparse-staged `newfs_hfs` format plus `fsck_hfs` repair. HFS File
  Apply must stage the direct or wrapped HFS logical volume span, not the whole
  containing partition, and physical proof is recorded at
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`
  with data/resource/inline-attribute raw read-back hashes plus optional secure
  released-block zeroing proof for file/folder-tree delete and final root-listing
  absence. Journaled HFS+
  `fsck_hfs` exit code 8 may be tolerated only on this explicit journaled
  HFS File mutation workflow and only with final read-back proof. APFS has a compiled fail-closed writer certification preflight,
  original Fletcher-64 APFS object checksum calculation/stamping/verification,
  deterministic image-only mutation plan generation, generated-layout
  queue/apply root-file, empty root-directory, root-directory child-file, and
  volume-label mutation proof; the full A1-A8 APFS driver track - inline
  compression, credential-gated encryption, in-place file + directory mutation,
  snapshots, multi-volume, clones/sparse/hard-links, and in-chunk resize - plus
  the H1-H8 HFS+ track (streaming B-trees, underflow merge, hard-links/symlinks,
  attribute overflow records, big-endian journal replay, wrapper write,
  decmpfs read+write) are Apple-certified (see
  `docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Deep XFS/Btrfs tool checks and
  XFS/Btrfs writes remain blocked; APFS Fusion/Tier2, container shrink +
  chunk-adding grow, and arbitrary non-generated Apple-media mutation at the
  Apply layer stay fail-closed by design.
- APFS writer infrastructure is release-allowed only while fail-closed:
  `PartitionApfsWriter` may compute/stamp/verify object checksums, build a
  deterministic image-only mutation plan shape, build a new APFS image-only
  format scratch image for certification, validate structured image-only
  execution evidence, and expose only generated-layout create/format,
  checksum-repair, bounded root-file write/patch/delete, empty
  root-directory create/delete, root-directory child-file write/patch/delete,
  volume-label change, snapshot create/delete/revert, file clone,
  file hard-link, and in-chunk container resize actions through UI/queue/apply.
  (Corrected 2026-08-30: the list ended at volume-label change and was closed
  with "only", but `partition_manager_panel.cpp:10285-10291` reaches snapshot
  create/delete/revert and container resize from the APFS Container dialog, and
  `ApfsCloneRootFile`/`ApfsHardlinkRootFile` are declared operation types. Those
  shipped with the A1-A8 certification, so the code is not exceeding policy --
  but a readiness doc that UNDERSTATES which destructive operations are
  UI-reachable is wrong in the direction that matters, since a reviewer using it
  to decide what needs certifying would never look at snapshot-revert or resize.) The certifier-generated format image must detect as
  APFS, report nonzero spaceman free bytes, round-trip the requested volume name,
  list the generated root directory, and read back the seeded multi-block proof
  file through the read-only APFS browser. The certifier-only write lane may copy
  a generated APFS image to a separate scratch image, add, replace, byte-range
  patch, or delete one bounded root regular file, add/patch/delete one bounded
  regular child file inside a supported generated root directory, preserve
  existing bounded root regular files/directories, zero the previous generated
  file-data region on delete, and prove create/add/replace/patch read-back plus
  delete negative-read-back, empty-directory read-back, and sibling preservation
  for generated images.
  The hidden existing-image format lane may overwrite only after destructive
  wipe confirmation and must prove post-format APFS detection plus empty-root
  browse. The raw-capable target API and read/write raw-device opener must stay
  fail-closed unless the request carries raw-target opt-in, non-image-only writer
  options, destructive evidence, and raw hardware certification evidence. The
  APFS raw-format validation lane must run only through
  `scripts/run_partition_manager_apfs_raw_format_validation.ps1` or its UAC
  launcher on pinned expendable APFS GPT media; passing this lane is required
  before any APFS raw format/write/patch/delete, empty-root-directory create/delete,
  root-directory-child-file write/patch/delete, volume-label change, or repair
  certifier claim can be made. The previous 2026-06-12 PDT / 2026-06-13 UTC
  51 GB JMicron run `run-20260612-192652` is retained as Windows-side evidence
  only because Apple kernel validation later rejected the large generated
  spaceman geometry. Current Windows-side proof passed on JMicron serial
  `DD56419883A5B`, disk 2 partition 2, 134,217,728 bytes, at
  `artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`;
  Apple-native validation has since been completed across the full A1-A8 driver
  track, incl. the A8 physical-USB destructive/crash/rollback gate
  (`external.apfs-a8-physical`), so the 128 MiB JMicron run is retained as the
  I1-era Windows-side evidence the multi-CIB/CAB geometry later superseded. The accepted lane uses bounded APFS detection/root-listing readback, generated-layout-gated
  root-file write/readback, generated-layout raw root-file byte-range patch,
  generated-layout empty root-directory create/delete with empty-listing and
  root-absence proof, root-directory child-file write/read/patch/delete proof with
  SHA-256 `f22535d6b86d4744537a0f58cdfc613162efa764610a9b191b53d5762a63566e`,
  patched child-file SHA-256
  `1d45dc05a258a2e29a7b77cdf5a6763e8f6ac138ee0c4066d6aaf6115eb45d27`,
  non-empty directory delete blocker proof, intentional generated metadata block
  199 checksum corruption, in-place checksum repair across the generated
  metadata block set with
  patched-file read-back, generated-layout raw volume-label change with relabeled
  root-listing proof and patched-file hash preservation, raw root-file delete
  with negative read-back, and an intentional full raw-device SHA-256 skip on a
  one-spaceman-chunk target. The
  certifier-only repair lane may
  restamp recognized APFS metadata object checksums in a separate generated
  scratch image and must prove corrupt-browse failure before repair plus
  post-repair listing/read-back; it is not raw-media APFS repair approval.
- APFS read-only browsing must fail closed on corrupt APFS metadata object
  checksums. File extent payload blocks are not validated as APFS object blocks.
- File Management cross-filesystem proof is destructive and remains outside
  CTest. The latest live lane wrote to pinned expendable physical media through
  `scripts\run_file_management_live_filesystem_certification.ps1`: APFS on
  JMicron serial `DD56419883A5B`, disk 2 partition 2, passed File Explorer
  create-directory, write/read, duplicate finder scan, advanced search,
  delete-file, negative read, and delete-directory at
  `artifacts\file-management-live-certification\disk2-apfs-script-after-fix\file-management-live-certification.json`;
  HFS+ on Best Buy serial `DD564198838A8`, disk 3 partition 3, passed the same
  lane plus rename at
  `artifacts\file-management-live-certification\disk3-hfs-script-after-fix\file-management-live-certification.json`.
  APFS destructive File Management proof is Apple-native certified across the
  A1-A8 driver track (incl. the A8 physical-USB gate, `external.apfs-a8-physical`);
  the earlier 128 MiB Windows-side run is retained as I1-era evidence.
  File Organizer generic moves remain local/mounted-file-API only.
- 2026-07-12 File Explorer command-route certification closed the Files-like
  plan's live-cert lane. The bridge now write-enables any known-size in-range
  APFS raw/image target (size superblock-derived for raw partitions),
  S.A.K.-generated and real Apple-created (foreign) alike. Evidence: foreign-APFS
  full destructive command route on a 7.45 TB real newfs_apfs container with
  Apple-derived `apfsck -cw` EXIT 0 clean
  (`artifacts\file-management-live-certification\disk2-foreign-apfs-destructive\`);
  HFS+ destructive command route (adds rename/read-after-rename) on a 256 MiB
  Apple-HFS partition with bundled `fsck_hfs` and probe `--hfs-check` clean
  before and after
  (`artifacts\file-management-live-certification\disk1-hfs-file-management-cert\`);
  ext4 read-only copy-out and XFS/Btrfs metadata-only blockers on WSL-built
  images
  (`artifacts\file-management-live-certification\ro-ext-xfs-btrfs-20260712\`).
  Unknown-size / out-of-range / snapshot-frozen / Fusion / locked-encrypted APFS
  targets stay read-only with exact blockers. Full local CTest is 257/257
  (corrected 2026-08-30; this said 142/142, the same stale figure the README
  test badge carried).
- Partition Manager ext/HFS+/APFS image browsers must open both normal image
  files and read-only Windows raw partition aliases through
  `openFileOrRawDeviceReadOnly`; physical Apple proof requires HFS+ and APFS
  root listing and can require selected-file read proof, HFS+ selected-attribute
  proof, plus APFS bounded export proof with
  `scripts/run_partition_manager_physical_apple_probe_validation.ps1 -RequireHfsFileProof -RequireHfsAttributeProof -RequireApfsFileProof -RequireApfsExportProof`.
  Current physical proof read `/.fseventsd/0000000000d34037` from the
  expendable APFS partition and recorded SHA-256
  `d5b58364da8b5f9e7996bcc9494db9f93a76484fc137e5aa6153809b8935c0d4`;
  APFS bounded export proof exported `/.fseventsd` with 6 files,
  0 directories, 6 scanned entries, and 67906 bytes.
  (Corrected 2026-08-30: these figures said the APFS proof read `/Fonts/00TT.TTF` at 25056 bytes with sha `d075a134...` and exported `/Fonts` with 63 files / 1 directory / 64 scanned entries / 21161830 bytes. That file and hash are the HFS+ proof's -- `hfs-read-00tt.json` in the same evidence directory records them against HFS+ -- so an HFS+ read was attributed to APFS. `/Fonts/00TT.TTF` does exist on the APFS partition (`apfs-list-file-candidate-directory.json` lists it at 25056 bytes; the operator copied files to both Apple partitions), but no APFS READ of it with a recorded hash exists, and the `/Fonts` export figures appear in no artifact at all. The APFS proof picks its subject by bounded candidate search, so the final run selected `/.fseventsd` instead. Quote the artifact, not the filename: this proof rewrites its own subject every run.) The same report records HFS+
  detection, root metadata, `total_bytes=127724052480`,
  `free_bytes=125197647872`, and selected-file proof for
  `/polyhavenassets_blendermarket_v1.2.0  (Blender 4.5+).zip` with SHA-256
  `72222f9f83d177ea9a2970edb756ab9f3f9f6b00a12f47af63259f215d970709`.
  HFS+ selected-attribute proof also passed by reading inline attribute
  `com.apple.decmpfs` for file ID 1894 at 16 bytes with SHA-256
  `e2aed0d76e90c81c39f9916d56a8f1631c440aec356877ea7dae197b069ea64f`; the
  report records `validation_requirements` showing HFS file, HFS attribute,
  APFS file, and APFS export proofs were required.
  The five-partition APFS physical lane also passes the all-partition APFS
  file/export proof flags using bounded recursive candidate search, including
  hidden Apple metadata directories when no visible regular file is present.
- APFS free-space reporting must stay verified both for probe-window
  space-manager objects and for checkpoint data outside the first 2 MiB probe
  window. The current physical regression command is
  `scripts\run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber 3 -AllowMissingHfs -ProbeAllApfsPartitions -RequireAllApfsPartitions`;
  it passed against five APFS partitions on a second expendable non-boot USB
  disk and recorded nonzero `free_bytes` for every APFS partition under
  `artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-apfs-multipart-physical\report.json`.
- Partition Manager non-native write payloads must use the exact selected
  `\\?\GLOBALROOT\Device\HarddiskN\PartitionM` raw partition alias. Missing,
  forged, or mismatched paths are blocked by safety validation and script
  generation before Apply, and queued ext repair keeps the destructive target
  field read-only.
- Partition Manager ext browse is read-only and preserves symlink metadata by
  displaying targets and exporting `.symlink.txt` sidecars; it does not follow
  symlinks or create links on Windows.
- QRC resource verification passes.
- Portable package smoke passes from a clean extracted folder.
- Startup E2E smoke passes from the packaged folder.
- All packaged `.exe` and `.dll` files have valid Authenticode signatures.
- `SHA256SUMS.txt` is regenerated after signing.

Use:

```powershell
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"

powershell -NoProfile -File scripts/stage_portable_release.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

powershell -NoProfile -File scripts/create_release_archive.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

$extract = Join-Path 'build\Release' ('clean-readiness-extract-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $extract) { throw "Extraction target already exists: $extract" }
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract

powershell -NoProfile -File scripts/check_release_readiness.ps1 `
  -PackageRoot $extract `
  -RequireSignedPackage
```

`check_release_readiness.ps1` expects a clean package folder. Startup smoke
creates normal runtime folders such as `data\logs` and `data\temp`, so run the
aggregate readiness gate against a fresh extract when validating the ZIP.

For unsigned local preflight, run the same command without
`-RequireSignedPackage`; signed package validation remains required before
publication.

Partition Manager certification defaults to a non-mutating plan-only harness run
so release readiness can run on ordinary CI and developer shells. When a strict
elevated disposable-VHD report already exists, verify that archived VHD evidence
against that report instead of generating a new plan-only report:

Default readiness also writes `external-evidence.template.json`,
`external-evidence.checklist.md`, an `external-evidence/` per-gate lab package,
`vhd-preflight.json`,
`certification-gap-report.json`, `certification-gap-report.md`, and
`certification-artifact-bundle.json` under the selected certification root, then
verifies that the Markdown checklist contains every external gate, safety
contract, required evidence key, required value hint, and artifact path from the
same certification matrix. The JSON manifest remains the verifier input; the
Markdown checklist is the human lab run sheet generated from the same matrix.
The lab package contains one run-sheet directory per external gate and is
verified by `scripts/check_partition_manager_external_lab_package.ps1`; each
directory includes a `report.template.json` scaffold for the local evidence
report. After lab operators save completed per-gate `report.json` files, run
`scripts/update_partition_manager_external_manifest_from_reports.ps1` to import
them into the external manifest; the importer rejects wrong IDs, stale matrix
contracts, missing evidence keys, blank values, wrong required values, and
missing verification summaries. Pass `-ChecklistPath` and
`-OutputChecklistPath` so the importer writes a paired checklist for the
imported manifest; it also refreshes the per-gate run-sheet README and
`report.template.json` manifest references under the evidence root. The package
does not count as evidence until the imported JSON manifest is filled with
passed, non-empty payload values plus real artifact paths or HTTPS URLs.
The gap report is the current machine-verified list of incomplete VHD and
external gates, including evidence keys, safety contracts, and next commands.
Those next commands are status-aware: once strict VHD evidence is complete, the
report moves operators to external evidence collection, per-gate `report.json`
import, and the strict hardware handoff instead of asking for another VHD run.
The artifact bundle hashes the current status, harness report, VHD preflight,
gap report, external manifest, and lab checklist so release handoff artifacts
cannot silently drift apart.
The VHD preflight report records whether the current host has administrator
rights, required Storage cmdlets/tools, enough temporary drive letters, and
enough workspace for the elevated disposable-VHD certification run. Default
readiness records blockers without failing a non-admin developer shell; use
`scripts/test_partition_manager_vhd_preflight.ps1 -RequireAdministrator
-RequireReady` immediately before starting a strict VHD run.
For the strict elevated VHD handoff, run
`scripts/run_partition_manager_vhd_certification_strict.ps1 -RelaunchElevated`;
it fails unless strict VHD evidence verifies and every disposable-VHD scenario
passes.

```powershell
powershell -NoProfile -File scripts/check_release_readiness.ps1 `
  -PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict `
  -RequirePartitionVhdEvidence
```

Add `-PartitionExternalEvidenceManifest <path>`,
`-PartitionExternalEvidenceChecklist <path>`, and
`-RequirePartitionExternalEvidence` only after every external VM/hardware/lab
gate is filled with passed, non-empty evidence and the paired checklist verifies
against the same matrix. Strict external evidence fails without the checklist.
For the final strict hardware evidence handoff, run
`scripts/run_partition_manager_hardware_certification_strict.ps1` with the
strict VHD certification root, external manifest, checklist, and evidence
directory. It is non-mutating and requires `HardwareCertified` status before
any release claim uses that wording.
If `certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` gains a VHD scenario,
the matrix-integrity guard requires the harness and certification documentation
to include the new scenario before readiness can pass. Rerun the elevated VHD
matrix before using `-RequirePartitionVhdEvidence`.

## Clean VM Manual QA

Run on a fresh Windows 10/11 x64 VM with no repo checkout and no developer
environment variables:

Latest VM smoke evidence: on 2026-06-02 PDT, VirtualBox 7.2.8 VM
`SAK-PM-Lab-Win11` booted Windows 11 25H2 and launched
`\\vboxsvr\sakrepo\build\Release\sak_utility.exe` from a read-only shared
folder. `SAK_STARTUP_SMOKE_OK` passed, and the visible Partition Manager UI
rendered 3 disks, including two RAW 4 GB data disks, in
`artifacts\partition-manager-certification\vm-lab\sak-utility-share-partition-manager-mouse-fixed.png`.
That smoke proved read-only shared-folder startup and non-admin inventory. It
did not prove destructive guest mutation because the guest token was not
administrator.

Follow-up destructive VM smoke passed on 2026-06-02 07:44 PDT in the same VM
after UAC elevation produced `is_admin=true`. The smoke mutated only disposable
4 GB VirtualBox data disks 1 and 2 after boot/system/model/size guards, verified
GPT NTFS create/format/resize/delete/clear on disk 1, verified MBR
FAT32-to-NTFS conversion and clear on disk 2, and left both data disks RAW,
non-boot, and non-system. Evidence:
`artifacts\partition-manager-certification\vm-lab\partition-vm-admin-report.json`.
This does not replace the 18 external VM/hardware/lab gates required for
the hardware-certified claim level.

AI Assistant VM live E2E passed on 2026-06-03 PDT / 2026-06-04 UTC in the
same `SAK-PM-Lab-Win11` Windows 11 VM from shared root `\\vboxsvr\sakrepo`.
The package-only harness read the OpenAI key from the provided key file without
writing it to reports, then passed `test_openai_responses_client.exe` with live
Responses API plain-response and function tool-loop smoke, portable startup
smoke, runtime accessibility audit, and the AI smoke checklist. Evidence:
`artifacts\ai-assistant-vm-smoke\run-20260604-033445\ai-assistant-vm-smoke-report.json`.

AI Assistant current-binary host/live quality pass completed on 2026-06-04 UTC:
Release `sak_utility`, `test_ai_subagent_runner`, `test_ai_chat_title`,
`test_ai_prompt_assembler`, `test_ai_orchestrator`, `test_ai_workflow_evals`, and
`test_openai_responses_client` built successfully; targeted CTest passed 7/7;
live OpenAI plain response and function tool-loop CTest passed using the key
file without printing the key; host package/live smoke passed at
`artifacts\ai-assistant-vm-smoke\run-20260604-060003\ai-assistant-vm-smoke-report.json`.
The latest local follow-up on 2026-06-07 PDT / 2026-06-08 UTC passed the full
Release CTest suite at 136/136, release readiness, `git diff --check`, and the
Release app/helper rebuild after the APFS empty root-directory writer slice,
HFS secure-delete physical proof, docs sync, and lizard refactors.
The follow-up multi-agent quality pass removed the production UI serial-only
cap for workflow delegates: read-only delegate phases can now run up to three
parallel subagents with a fresh OpenAI model client per subagent, while
mutating and conditional phases remain serialized by the orchestrator.
The latest chat-continuity pass now persists assistant response ids into
transcript metadata and restores the last assistant response id when a saved
chat is reopened, preserving Responses API `previous_response_id` continuity
for follow-up turns instead of detaching the reopened chat from server-side
conversation state.
The latest UI polish pass also adds result-bubble copy buttons, Enter-to-send
with Ctrl+Enter newline behavior, New Chat draft reset semantics, visible
background-agent counts, workflow-selection role preview without a role dropdown,
first-prompt role inference with explicit user role-switch support, and an exact composer-side
`Ctx: x/y` context-window meter tied to the selected model through OpenAI
`/v1/responses/input_tokens` so operators can judge when summary/report
compaction is needed.
The current local package proof staged `S.A.K.-Utility-0.9.1.9` (STALE: `VERSION`
is 0.9.2.0 and the recipe below produces `SAK-Utility-v0.9.2.0`; this proof
predates the version bump and needs re-running before release), passed portable
dependency smoke, passed portable startup E2E smoke, passed package-root release
readiness, and rebuilt `S.A.K.-Utility-0.9.1.9-Windows-x64.zip` after the
refactors.
Current staged-package hashes:
- `sak_utility.exe`: `E587E51085199FF18027AF43883DFAA9FD6841FE2129DE0F72A953A7357D9277`
- `sak_elevated_helper.exe`: `EC64933550AC73948F6113E4F7E800EF3BF4EDD084409168F330465DCF53B045`
- `S.A.K.-Utility-0.9.1.9-Windows-x64.zip`: `1DA03D804D0A9EA87DB950BDE49D70482A4BE2D6DB59E532C75F0403D9819261`
The earlier current-binary VM rerun attempted a blank-password guest-control
login and failed before launching the in-guest harness. Do not use blank
guest-control auth for this VM. The fixed host path uses a real guest password
file resolved from the VM unattended metadata or `temp\vm-auth`, then passes it
to `VBoxManage guestcontrol --passwordfile`. For production-grade noninteractive
VM gates, the token must already be a direct administrator token. Keypress-based
UAC acceptance is fallback-only and must be enabled explicitly with
`-AllowUacKeypressFallback`; normal reruns should use the dedicated automation
VM created by `scripts/new_partition_manager_automation_vm.ps1`. This is not a
blocker for non-destructive AI Assistant readiness because the current-binary
local and host package/live smoke passed.

Partition Manager ext filesystem write support has a dedicated destructive VM
gate. Preferred host-driven run:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_vm_gate_host.ps1 `
  -Gate ext-filesystem `
  -DiskNumber 1 `
  -FileSystem ext4
```

The host launcher resolves the real VM guest password file, validates
guest-control auth, requires a direct administrator token, runs the destructive
runner through guestcontrol, and waits for a fresh report. If the legacy lab VM
only provides a filtered token, the launcher fails before deleting stale
evidence unless `-AllowUacKeypressFallback` is supplied explicitly. Manual
in-guest launch is still valid from the `SAK-PM-Lab-Win11` desktop:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  \\vboxsvr\sakrepo\scripts\launch_partition_manager_ext_filesystem_vm_gate_local.ps1 `
  -DiskNumber 1 `
  -FileSystem ext4
```

The elevated runner verifies the bundled e2fsprogs hashes, formats a raw ext4
partition, runs the confirmed repair command path, grows the partition with
`resize2fs`, shrinks the file system before shrinking the partition, rechecks
the filesystem, clears the disposable disk, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
Latest evidence passed on 2026-06-05 against
`\\?\GLOBALROOT\Device\Harddisk1\Partition2` with exit code 0 for format,
repair, read-only checks, grow, shrink, and cleanup. Keep future non-ext writes
blocked until operation-specific proof exists.
An unattended direct-admin rerun also passed in dedicated VM
`SAK-PM-Automation-Win11` on 2026-06-05 PDT / 2026-06-06 UTC. The host report
`artifacts\partition-manager-certification\vm-lab\host-launch\ext-filesystem-20260605224315.json`
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`; the gate report is
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
The host launcher now retries Guest Additions desktop readiness before starting
guestcontrol work so a cold-boot VM does not fail on one early `waitrunlevel`
miss.

Linux swap raw-partition automation rerun passed on 2026-06-05 PDT /
2026-06-06 UTC in dedicated VM `SAK-PM-Automation-Win11`. The VM creation report
records `direct_admin_guestcontrol_verified=true`, the host launch report
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`, and the gate report records `status=Passed`,
`detected_file_system=Linux swap`, raw target
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, and cleanup back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\automation-vm\SAK-PM-Automation-Win11-20260605210356.json`,
`artifacts\partition-manager-certification\vm-lab\host-launch\linux-swap-20260605220305.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.

Linux compatibility validation for the same bundle passed on 2026-06-07 UTC with
`scripts/run_partition_manager_ext_linux_validation.ps1 -DistroName archlinux`
for ext2, ext3, and ext4. The harness used Windows-bundled `mke2fs` and
`resize2fs`, then Linux `e2fsck`, `dumpe2fs`, and loop mount accepted each
grown/shrunk image, wrote a fixture file, rechecked clean, and bundled Windows
`e2fsck` rechecked clean after the Linux write. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext2-linux-validation\report.json`,
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext3-linux-validation\report.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext4-linux-validation\report.json`.
This validation does not create a runtime WSL dependency.

Linux-created XFS/Btrfs metadata validation passed again on 2026-06-07 UTC with
`scripts/run_partition_manager_linux_metadata_validation.ps1 -DistroName archlinux`.
The harness used Linux `mkfs.xfs` and `mkfs.btrfs` to create disposable images,
then `partition_filesystem_probe_certifier.exe` validated S.A.K. raw detection,
metadata fields, and sanity lines for XFS and Btrfs from Windows. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-metadata-validation\report.json`.
This validation does not create a runtime WSL dependency. It supports the
in-app original read-only metadata consistency check from captured probe data,
but does not approve deep XFS/Btrfs tool checks, format, repair, or write
support.

Filesystem probe helper offset validation is now part of the local release
gate. `partition_filesystem_probe_certifier.exe` accepts
`--input-offset-bytes`, reports `input_offset_bytes` in JSON, and reads from the
offset for regular files and raw Windows device paths. It also accepts
`--hfs-check` for HFS+/HFSX consistency and attribute metadata output and
HFS+ selected-attribute read arguments for file ID/name proof when matching
attributes exist on media. The generated-fixture
self-test `scripts/test_partition_filesystem_probe_certifier.ps1` is wired into
CTest as `test_partition_filesystem_probe_certifier` and into release readiness;
it creates a tiny padded ext2 fixture, verifies deterministic offset report
fields, and confirms invalid offset input fails. A lab-only Sonoma 14 DMG HFS+
fixture also passed at byte offset 134250496 with sane HFS+ header metadata.
The unpacked Tahoe installer `SharedSupport.dmg` is a compressed UDIF with an
HFS+ payload and is useful for future container-extraction validation only; it
is not bundled or required at runtime.

Physical Apple filesystem validation is optional and read-only:

```powershell
powershell -NoProfile -File `
  scripts\run_partition_manager_physical_apple_probe_validation.ps1 `
  -DiskNumber 2
```

The current physical run passed on 2026-06-07 UTC against a
non-boot external USB NVMe GPT disk with one Apple HFS partition and one Apple
APFS partition. The script used `GLOBALROOT` partition aliases, detected HFS+
and APFS with sane metadata, required HFS file, HFS attribute, APFS file, and
APFS export proof, and wrote
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-filesystem-physical\report.json`.
The latest APFS physical probe also verifies free-space reporting from the APFS
space manager: `total_bytes=127992487936`, `free_bytes=127866097664`.
This optional gate is not part of the 18-gate commercial destructive matrix and
does not approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth, compressed-file writes,
arbitrary APFS mutation, non-generated APFS file-write, or APFS
resize behavior. The APFS writer helper exposes generated/minimal APFS
create/format, generated-image root-file create/replace/byte-range patch/delete,
empty root-directory create/delete, root-directory child-file write/patch/delete,
and volume-label change with read-back, directory-empty, old/new-label, or
negative-read-back hashes, generated-layout
raw root-file write/patch/delete, empty root-directory create/delete, and
root-directory child-file write/patch/delete plus volume-label change
queue/apply routes, and generated-layout checksum repair only. Production helper
physical proof is Apple-native certified across the A1-A8 driver track to a
24 TiB cap, incl. the A8 physical-USB destructive/crash/rollback gate
(`external.apfs-a8-physical`). The earlier 128 MiB JMicron Windows-side run
(`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`)
and `run-20260612-192652` are retained as the I1-era Windows-side evidence the
multi-CIB/CAB geometry later superseded.
The production helper does not blanket-enable protected/compressed/snapshot/
multi-volume APFS mutation; each is reached only through its own certified
operation-specific route (A3 snapshots, A4 multi-volume, A5 compression, A6
credential-gated encryption - all Apple-certified), never blanket-on.
Fusion/Tier2 multi-device stays out of scope and unsupported incompatible
feature flags remain fail-closed.
APFS and HFS+/HFSX are now full driver-level write-certified (A1-A8 / H1-H8 -
see `docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Release wording must keep
XFS/Btrfs to read-only metadata, and may state full driver-level APFS + HFS+/HFSX
read/write with the standing hard boundaries: APFS Fusion/Tier2 multi-device out
of scope, encryption credential-gated, sealed-system-volume writes behind a typed
seal-invalidation confirmation, APFS writes gated to S.A.K. generated-layout
containers at the Apply layer, and APFS container shrink + chunk-adding grow as
documented follow-ons.

APFS image-offset proof also passed on 2026-06-06 UTC against
`temp\Sonoma 14.dmg` using `--input-offset-bytes 1233310720`; evidence:
`artifacts\partition-manager-certification\tool-tests\sonoma-apfs-offset-probe.json`.
The Tahoe `SharedSupport.dmg` `NXSB` hits were rejected as direct APFS proof
because no 4096-byte-aligned superblock hit with sane block geometry was found.

HFS+/HFSX physical destructive tool proof uses
`scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1` and
`scripts/run_partition_manager_physical_hfsprogs_validation.ps1`. The launcher
uses Windows `runas` UAC auth, not keypress automation; the runner requires
admin, `-Force`, non-boot/non-system disk guards, USB media by default, and
serial or friendly-name guards for large disks. The proof formats sparse staging
images as HFS+, HFSX, then HFS+ again by default, runs `fsck_hfs`
repair/final read-only checks on those images, copies allocated metadata ranges
to the selected raw partition, verifies S.A.K. HFS detection with
`partition_filesystem_probe_certifier.exe` after each case, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfsprogs-physical-destructive\report.json`.

Physical cross-filesystem destructive validation is optional and must target
known expendable external media:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1 `
  -DiskNumber 2 `
  -ExpectedSerialNumber DD564198838A8 `
  -ExpectedFriendlyNamePattern "Best Buy|NS-PCNVMEHDE" `
  -Force
```

The launcher uses Windows UAC `runas` auth, not keypress automation. The
elevated runner requires admin, `-Force`, non-boot/non-system checks, USB media
by default, and serial/friendly-name guards for large disks. The current run
passed on 2026-06-05 PDT / 2026-06-06 UTC: ext2, ext3, and ext4 were formatted,
repaired, read-only checked, grown, shrunk, and probe-verified after each stage;
Linux swap SWAPSPACE2 metadata was written, reread, probe-verified, and the disk
was cleared back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive\report.json`.

Linux swap format compatibility validation passed on 2026-06-05 PDT with
`scripts/run_partition_manager_linux_swap_format_validation.ps1 -DistroName archlinux`.
The harness wrote an original SWAPSPACE2 v1 header to a disposable image using
the same metadata layout as the app formatter, then S.A.K.
`partition_filesystem_probe_certifier.exe`, Linux `blkid`, and Linux
`swaplabel` all recognized the image as swap with matching label, UUID, version,
and page-size metadata. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-validation\report.json`.
This validation does not create a runtime WSL dependency and does not replace a
future raw-disposable-partition VM proof if Linux swap format is raised to a
separate destructive-media certification gate.

Linux swap raw-partition VM proof now has a dedicated destructive runner and
launcher. Preferred host-driven run:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_vm_gate_host.ps1 `
  -Gate linux-swap `
  -DiskNumber 1 `
  -PageSizeBytes 4096
```

Manual in-guest launch is still valid from the `SAK-PM-Lab-Win11` desktop when
guest UAC is available:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  \\vboxsvr\sakrepo\scripts\launch_partition_manager_linux_swap_vm_gate_local.ps1 `
  -DiskNumber 1 `
  -PageSizeBytes 4096
```

The elevated runner writes original SWAPSPACE2 v1 metadata to an app-style raw
partition target, rereads and verifies the header, runs
`partition_filesystem_probe_certifier.exe` against the raw target, clears the
disposable disk back to RAW, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.
Latest evidence passed on 2026-06-05 PDT against
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`; the host launch report is under
`artifacts\partition-manager-certification\vm-lab\host-launch\` and redacts the
password value. This gate is separate from the image-level Linux
`blkid`/`swaplabel` validation above.

Eighteen matrix-backed external gates now pass in VM-lab evidence. The first
imported batch covered:
`external.bitlocker`, `external.bitlocker-mutation`,
`external.file-level-data-recovery`, `external.allocate-free-space`,
`external.cluster-size-change`, `external.hdd-defrag-execution`,
`external.usb-removable`, `external.ssd-retrim`,
`external.ssd-secure-erase`, `external.partition-move`,
`external.primary-logical-conversion`, `external.volume-serial-number`,
`external.dynamic-to-basic`, and `external.hardware-wipe`.
Evidence lives under
`artifacts\partition-manager-certification\vm-lab\external-evidence\`, with
imported manifest
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`
and paired imported checklist
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`.
The imported status was reported as `HardwareCertified` at the June checkpoint: 18/18 external
gates and 12/12 VHD scenarios were recorded as passed. Current R5 verification rejects the old
image-restore proof because it copied VHD containers instead of exercising the application
controller. The replacement controller-driven report and supplementary refusal/offline fixtures
passed host validation at their frozen source checkpoint. After the CTest launch repair changed
the bound build configuration, the rebuilt helper repeated the complete VHD matrix and five
supplementary cases; current-source host validation passed again.
 The September 8 checkpoint supported `VhdDataDiskCertified`, not a permanent current claim.
 The 2026-09-19 `CodeCompleteOnly` (11/12) state was a source-freshness refusal after the
 MCP header changed; the earlier report remains historical. On 2026-09-25 the Windows 11 VM
 strict disposable-VHD run passed 12/12 scenarios with the W68 controller binary and its
 403-file source manifest. The host independently verified the copied report under
 `artifacts/partition-manager-certification/vhd-strict/run-20260925-121253/` against that same
 explicit binary/manifest. The current VHD-only claim is `VhdDataDiskCertified`; no external
 manifest is implicitly selected.
 The historical 18-row external manifest is retained but is not fresh current-application proof.
Release remains incomplete on current external/hardware and whole-application E2E evidence.
The historical external-gate batch completed on 2026-06-03 PDT:

- `external.os-migration-reboot`: target-only cloned OS VM booted with UEFI
  firmware to Guest Additions run level 3.
- `external.boot-repair-uefi`: disposable UEFI BCD/fallback-loader removal
  produced a no-boot symptom, and offline `bcdboot` repair restored boot.
- `external.boot-repair-bios`: disposable BIOS/MBR `\Boot\BCD` removal
  produced Windows Recovery `0xc000000f`, and offline `bcdboot` plus
  `bootsect` restored boot.
- `external.system-mbr2gpt`: disposable BIOS/MBR boot VM validated and
  converted disk 0 with `mbr2gpt` exit code 0, changed MBR to GPT, and the
  converted disk booted under a fresh EFI VM to Guest Additions run level 3
  after VirtualBox UEFI boot-file normalization.

Strict release readiness with
`-RequirePartitionVhdEvidence -RequirePartitionExternalEvidence` passed at that checkpoint;
that historical run does not itself prove the current source or the stronger image-restore contract.
The historical machine-readable release evidence snapshot is tracked under
`artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\`
and `artifacts\partition-manager-certification\vm-lab\external-evidence\` so a
clean checkouts retain the provenance. That snapshot alone no longer supports a current
`HardwareCertified` claim; use the current matrix and R5 evidence requirements.

1. Extract the release ZIP into a clean folder.
2. Run `sak_utility.exe`.
3. Confirm the app starts without Qt/plugin/runtime errors.
4. Confirm `data\logs` is created under the portable folder.
5. Open AI Assistant and confirm provider diagnostics render.
6. Run a package search request and verify no install occurs.
7. Run package install request only after explicit install wording.
8. Run Network Diagnostics ping/DNS/port-scan safe tools.
9. Run Diagnostics hardware scan and generate a report.
10. Run Backup wizard dry path through validation without writing outside the selected target.
11. Run App Management scan/export.
12. Confirm About panel includes third-party attribution.
13. Close and reopen the app; verify settings and sessions remain portable.
14. Verify no files are written under the source repo or developer profile except standard Windows logs.

## Rollback And Versioning

- Release tags must use `v<major>.<minor>.<patch>.<build>` and match `VERSION`.
- A release is rollback-safe only when the previous signed ZIP remains available.
- If a release is pulled, mark the GitHub release as draft or delete the asset,
  publish a replacement patch version, and document the reason in release notes.
- Never overwrite a published ZIP for the same tag. Rebuilds require a new tag.
- Keep at least one previous known-good release artifact available.

## App-Control Rule

App-control workflows ship only when the manifest action is backed by either:

- a documented CLI with tested arguments, or
- a tested Win32 GUI automation workflow with stable selectors and failure
  evidence.

If neither exists, the manifest must expose observation/status only and fail
explicitly for the unsupported action.
