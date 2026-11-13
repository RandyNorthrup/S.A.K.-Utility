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
- [x] No known crash, data-loss or unpatched-security item remains in the list below (2026-11-10). Two open
      non-defects stay tracked there: item 7's unbounded GUI-thread joins, a hang risk not observed, and item 4's
      MCPS-9, an evidence gap.
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
9. FIXED 2026-11-05, release blocker: a Windows install USB did not start a UEFI PC. The app wrote one NTFS partition
   and reported success, and UEFI firmware, which Windows 11 with Secure Boot needs, reads FAT32. It writes one FAT32
   partition and has Windows' DISM split a large install.wim into .swm parts, and its final check reads the result
   back (4505a84f, 6edd85d0). Built and read back on testpc1, and a byte-for-byte image of the stick started
   Windows Setup in a Hyper-V Generation 2 VM with Secure Boot, to its edition list; see UI review round 3 below.

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

### Site-claims audit fixes (2026-11-01)

A static audit of sakutility.com against the code found product defects. Each was confirmed in the
code first, and each fix landed with its own test and drills.

- FIXED: Chocolatey matching had no production caller, so restore offered nothing. The backup scan
  records matches from a reviewed table (`PackageMatcher`) and restore offers them, behind the
  administrator gate.
- DESIGN DECISION: matching is offline. `choco search` sorts alphabetically, takes about 2 s a query
  and the feed answers excess traffic with an hour of 429s, so the online fuzzy and search stages
  were removed instead of wired. An application the table does not know stays "(no match)". The
  table's ids were checked on the feed with `choco search <id> --exact` on 2026-11-01.
- FIXED: Image Flasher picker and refusals (ZIP, UDIF .dmg), network report route, Email Inspector
  search results and folder scope.
- FIXED, data-loss class: the capability matrix and the File Explorer note said a Fusion or
  multi-device APFS container fails closed, and no write path checked. `loadFsCommitContext` now
  refuses it. Unit-tested on a flagged image; no real Fusion container was available.
- FIXED: `scripts/verify_authenticode_signatures.ps1` requires the published subject on `sak_*`
  binaries. Still to prove: the first signed run, which is also the first time the pin meets the
  Azure profile's real subject.
- INCOMPLETE: a restore install through `choco install` has not run end to end. The test stops at
  the list handed to Chocolatey, because a real install changes the machine it runs on. To prove on
  the VM smoke.

### Backup, restore and file management fixes (2026-11-01)

Found by wiki writers reading the code, then confirmed by running it. Each landed with a test that
goes red without the fix. `test_user_profile_round_trip` runs a real backup through the backup
worker and restores it through the restore worker, which nothing did before.

- FIXED, data-loss class: the Application Data step copied nothing. Sources are now folders of the
  run, and the manifest records what was stored. Backups from older versions say they hold no copy.
- FIXED, crash: the restore wizard crashed on Select Folders for any backup with a folder ticked.
  `wholeRestoreWizardRunsOnARealBackup` now walks every page on a real backup.
- FIXED, data-loss class: restore verification failed every compressed or encrypted file and rolled
  back overwrites. Reproduced on real backups of each kind before the fix.
- FIXED: unticking a folder at restore invalidated the manifest checksum, so the restore refused the
  backup as damaged.
- FIXED: Replace and Merge were one code path. Replace overwrites, Merge follows the rule, neither
  removes a file the backup lacks. DESIGN DECISION: Replace does not delete, because a restore that
  removes a person's other files is a data-loss risk the label does not clearly promise.
- DESIGN DECISION: application data always replaces what is there, keeping each old file as
  `.sakbak`. Its files stand together, and a renamed `Bookmarks` is a restore that did nothing.
- FIXED, security: BitLocker Key Backup wrote plaintext keys to FAT32 and exFAT, and said the
  folder was restricted. Set-Acl reports success there and stores nothing (confirmed on an exFAT
  drive: the script printed SUCCESS and the folder stayed open to Everyone). The script now reads the
  result back, and the action checks the drive first. The read-back has no automated test, because
  no CI runner has an exFAT drive; it was run by hand on one, with the result recorded in the
  commit. DESIGN DECISION, owner 2026-11-02, reversing the refusal this round first made: a FAT32 or
  exFAT destination is allowed after a warning, because a technician often only carries keys across
  to another device. Cancel is the default and writes nothing. The result and the log say the keys
  were saved and are not protected. On NTFS a failed lock-down is still a failure, and a warning
  accepted for this run changes nothing there. DESIGN DECISION: a route with no person to answer the
  warning refuses and writes nothing. The assistant's approval covers running the action and never
  shows this warning, and Unattended has no one there at all, so its runs carry no acceptance. The
  helper takes an acceptance only as a JSON true in the payload, which only the panel's run sends.
- FIXED: path::string() throws on a character outside the system code page. That ended organizer
  and duplicate runs, and made the restore path guard refuse a whole backup. These paths now use
  UTF-8 (pathAsUtf8), with a real round trip per backup kind.
- Wrong on inspection: the wiki writers' "Scan Users without admin may find no accounts". NetUserEnum
  level 3 returned all 11 accounts here without elevation. The scan now asks for level 0 regardless,
  because the names are all it reads.
- INCOMPLETE: none of this has run against a real running browser, or on a real second PC. Locking is
  tested with an exclusive file handle, and the running-program prompt with a renamed process. To
  prove on the VM smoke: back up a real Chrome and Firefox profile, restore on another account, and
  open the bookmarks. Saved passwords are expected not to work there; that is Windows, not the app.

### Imaging and diagnostics fixes (2026-11-02)

Found by wiki writers reading the code, then confirmed where this unelevated PC allows it. Each landed
with a test that goes red without the fix. Nothing here wrote to a physical drive.

- FIXED: GZ, BZ2 and XZ images were all refused. They are decoded once on a pool thread before any
  disk goes offline, which gives the true size and refuses a truncated, corrupt or tar stream.
  Coordinator tests reach the unmount step against a disk number no machine has.
- FIXED: Quick verification could never pass on a compressed image (no seek), and a source that grew
  after approval had no stop short of the end of the drive. Both were unreachable until this landed.
- FIXED, confirmed with real bytes: SFC's redirected output is UTF-16 with no BOM. Captured from
  sfc.exe here, 0D 00 0D 00 0A 00 59 00; Windows PowerShell's default Get-Content returned 96 NULs in
  192 characters. The test runs real PowerShell over the same bytes.
- FIXED: Check Disk ignored Repair-Volume's result. Classified from Volume.RepairStatus in Windows'
  own Volume.cdxml; a name it does not know is not an answer. 30 s per drive became 30 min.
- FIXED: result.log reached no panel. One fix in the controller, which also covers the Network
  panel's Reset Network finding on the E list. The BitLocker log holds counts and paths, no keys.
- FIXED: ALL TESTS PASSED after a partial run. FIXED: stress test -1.0 degC and no warning.
- FIXED: Windows route from the ISO's label. It now looks inside with the bundled 7-Zip, checked
  against the real Win11 25H2 ISO (found, 53 ms), and against real Arch, Clonezilla, Ventoy and
  ShredOS ISOs (none). FIXED: "VERIFIED BOOTABLE" was untested and is gone.
- FIXED: Clonezilla (rolling, from the project's mirror), Ventoy (release-wide sha256.txt), ShredOS
  (default name; verify goes by upstream name). The three real ISOs were downloaded to E: and each
  matched its published hash: Clonezilla 3.3.3-37, Ventoy 1.1.18, ShredOS 0.43.1.
- CONFIRMED AT RUNTIME, unelevated here: smartctl reads the NVMe drive and gets Error=5 on SATA and
  USB, so "without admin" is per drive type, not all or nothing. The page and the message say so.

DESIGN DECISIONS:

- A compressed image is decoded for its size rather than trusting xz's index or gzip's trailer.
  gzip's trailer is modulo 4 GiB, bzip2 records nothing, and one path serves all three. It costs one
  extra pass; xz's index could make that instant for xz alone if it ever matters.
- SFC's verdict is English only and has no switch like DISM's /English. Reading CBS.log's [SR] lines
  might avoid that, and has not been tried on a real scan. Round 5 made this an unknown result rather
  than a failure.
- Stress test with no CPU sensor keeps running, with the warning on screen and in the report, rather
  than refusing every PC that cannot report a temperature. Only the CPU is watched.

INCOMPLETE, all need an administrator session on a VM. This PC's session is not elevated, and the
capture VM is held:

- Real flash of a compressed image. Attach a VHD of at least 2 GB, flash a real `.img.xz`, `.img.gz`
  and `.img.bz2` from the Image Flasher in each of Full, Quick and Off verification, and read the VHD
  back with `fc /b` against the unpacked image. No test here can open a raw disk.
- Real SFC on a healthy VM: Verify System Files must report "SFC found no integrity violations".
- Real Repair-Volume -Scan: Check Disk Errors must print `ScanNoErrorsFound` or `NoErrorsFound` for
  each NTFS volume, and show what a FAT32 or exFAT USB stick answers. That result word was read from
  the cmdlet's definition, not seen from a real run.
- Real Windows install media from Win11 25H2 and a rescue ISO without an install image: the first
  must say Extract ISO, the second Raw disk imaging.
- Real downloads through the app of Clonezilla, Ventoy and ShredOS, with aria2 and the verify step.
  URLs and hashes are proved above by download and hash, and the verify logic by tests on the real
  checksum files, but not through the dialog.
- Disk benchmark and disk stress on C: unelevated: must run, and leave nothing behind.

### Email items after round 4: NST, every email, attendees, repeats, journal, filters (2026-11-02)

The items the owner asked for after the section below. Same rules: tested against the owner's real
files on copies, nothing derived from them in the repository, anonymous counts only. libpff is the
independent reader for stores, Python's mailbox module for MBOX, and icalendar with dateutil for ICS.

| File | Folders | Messages, app and libpff | Converted | Left out, as not email | Failed | Attendees read, app and libpff | Repeating found | RRULE written | Unreadable | Journal |
|---|---|---|---|---|---|---|---|---|---|---|
| PST-1 | 34 | 16,886 both | 16,177 | 709 (200 contacts, 399 calendar, 62 tasks, 44 notes, 4 other) | 0 | 2 both, on meeting items; 0 on events | 64 | 63 | 1 | 0 |
| OST-1 | 57 | 1,316 both | 779 | 537 (255 contacts, 1 calendar, 12 tasks, 1 note, 268 other) | 0 | 1 both | 0 | 0 | 0 | 0 |
| OST-2 | 61 | 1,395 both | 1,289 | 106 (calendar) | 0 | 0 both | 13 | 13 | 0 | 0 |
| NST-1 | 24 | 0 both | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Each row's Converted, Left out and Failed add up to its Messages, and Python's mailbox module counts
the same number of messages in the output as Converted. PST-1 converted in 25 s with a peak of 127 MB.
Before these fixes OST-1 converted 774, OST-2 1,287 and PST-1 16,209: the first two left real email in
system folders out, and PST-1 also wrote 32 contacts, events and tasks filed in Deleted Items as mail.

Repeating events, read independently from the ICS file: dateutil accepted all 76 rules, and all 76 start
on their DTSTART. All 24 EXDATE dates and all 5 RECURRENCE-ID dates in PST-1 land on an occurrence of
their rule. No line is over 75 octets. Where Outlook also wrote its own English wording of the pattern
(19 of PST-1's 63), the frequency, weekday, month and position in it all agree with the rule every time,
and so does the time in the 18 that name one. The stored start agrees with the pattern in all 32 series
that know their zone. 44 of PST-1's rules store no time zone at all and are written with floating times.

The one that cannot be read is a yearly series in PST-1 whose pattern's first date is not on its own rule,
while its stored start, its clip start and Outlook's wording name another month. Nothing in the store
says which is right, so the event says so, the export log says so, and it is written once with a comment.

Search filters, against independent counts with no search text. Every filtered text search was a subset
of the plain search of the same word.

| Filter | PST-1, app and libpff | OST-1, app and libpff | OST-2, app and libpff |
|---|---|---|---|
| Email | the 10,000 limit, of 16,175 | 779 both | 1,289 both |
| Calendar, with meeting items | 401 both | 1 both | 106 both |
| Contact | 200 both | 255 both | 0 both |
| Task | 62 both | 12 both | 0 both |
| Note | 44 both | 1 both | 0 both |
| Journal | 0 both | 0 both | 0 both |
| Has attachment | 3,019 both | 21 both | 793 both |
| Email to 2018 | the 10,000 limit, of 16,175 | 17 both | 0 both |
| Email from 2019 | 0 both | 762 both | 1,289 both |

Converted MBOX files gave the same answers against Python's mailbox module, and Has attachment agrees
with it message by message. For messages, with attachments, to 2018 and from 2019: OST-1
has 779, 21, 17 and 762; OST-2 has 1,289, 793, 0 and 1,289; PST-1 has 16,177, 3,015, 16,177 and 0, where
the app's Email and to-2018 searches stop at the limit. Every other item type finds 0 there and says why. That
comparison found two bugs in reading an MBOX: Has attachment trusted a guess from the headers (766 hits
where OST-1 has 21), and a text file carrying a Content-ID was read as the message body, replacing the
real one, and never listed as an attachment (153 messages of OST-2). Both are fixed. The 10,000
limit was already applied after filtering, since it counts hits, and it stays that way, with a test.
On PST-1 a plain search for one common word stops at the limit. The same word with Has attachment finds
2,701, and 634 of those are items the plain search never reached. The count now says when the limit was
reached.

Decisions the owner may overrule:

1. **What counts as real email is a list of message classes:** IPM.Note and its kinds except rule
   templates, IPM.Post, IPM.Schedule.Meeting, delivery reports and a few more, in
   `PstParser::classifyMessageClass`. A class nobody has listed is left out and counted as other, and
   still opens in its folder. It used to count as mail.
2. **A contact, event or task filed in a mail folder is left out of the MBOX**, which is why PST-1's
   converted count fell by 32. Each is counted, and each still opens in Email Inspector.
3. **The Calendar window draws a repeating event once**, on its first date, with a mark and the pattern
   in words. It does not draw later dates. The export carries them all. Drawing them is a further change.
4. **A repeating event with no stored zone is written with floating times**, so it stays at its clock
   time wherever it is imported. A non-repeating all-day event with no zone stays in UTC as it was, since
   its local date cannot be known exactly: 6 of PST-1's 10 all-day events.
5. **A changed occurrence keeps its new time, subject and location.** Its own body, attendees and
   attachments live in a hidden attachment that is not read: a changed body is left out rather than
   shown wrong, and attendees come from the series.
6. **A filter with no search text lists what it matches.** Not asked for; it is what the empty search
   box would otherwise do nothing about.
7. **A date range judges events and journal entries by when they happen**, and an item with no date is
   left out of any date range. Calendar includes meeting items and Contact includes distribution lists.
8. **Export ALL Mail Folders takes real email only**, the same set as the converter.
9. **The calendar CSV gained Repeats and Attendees columns.**
10. **An MBOX part that names a file is an attachment, even when marked inline.** A sender that gave its
    message text a file name would show it as a file, not as the body. On an MBOX, Has attachment reads
    each multipart message to be sure, so it is slower there than the other filters.
11. **A series whose first date is off its own rule is unreadable, not moved to the next date that fits.**
    Moving it was tried first, and the other dates in the store showed that would have been wrong.

Not covered by a test, and INCOMPLETE where it needs the VM:

- MBOX in a real mail client, Thunderbird, and ICS in Thunderbird and in Outlook's own importer: proved on a store
  Outlook wrote, round 5c below.
- `harbor-lane-sample.pst` (`tests/tools/make_sample_pst.cpp`) is for screenshots only and must never be attached to
  Outlook. It has no valid header allocation fields and no allocation maps, and Outlook crashes on it: MSPST32's
  corruption check ends the process, which is not a defect in the app. libpff reads all of it.
- A real NST with messages: none exists here. The real one opens and holds none. Reading mail from one is
  proved on a generated store with the header a real NST has, in the older 512-byte page layout. The 4K
  layout real NST files use is proved by the real OSTs, which have the same header and version.
- The PST path's 10,000 limit is only exercised by the MBOX test, since the store builder cannot make a
  store that size. Both count hits the same way, and PST-1 shows it on real data.
- A real journal entry: none of the three stores has one. Outlook wrote one in round 5c below, and it reads right.
- The two time zone formats are read from generated bytes and from real stores by count. No real zone
  has been compared with Windows' own zone database.

Found and not fixed, because they are outside these items:

- Reading an MBOX does not decode non-English subjects and senders in the headers. The converter writes
  them as encoded words, correctly: Python reads them. Email Inspector shows the encoded form and a search
  for the real text does not find it.
- The MBOX list's Has attachments tooltip still uses the guess from the headers, because reading every
  message to fill a list would be slow. The filter no longer does.

### Email tools and application management fixes (2026-11-02)

The wiki writer's Email Tools and Application Management list, and the VM agent's MBOX count, on
`dev`. Tested against five of the owner's real mail files as well as generated stores. The real files
stay on this machine and nothing derived from them is in the repository; only the anonymous counts
below are. An independent reader, libpff, was used to check the folder and message counts.

| File | Folders | Messages, app and libpff | Attachments read | Search | Converted to MBOX | Failed | MBOX count matches |
|---|---|---|---|---|---|---|---|
| PST-1, 1.1 GB, Unicode, compressible | 34 | 16,886 both, per folder | 5,160 of 5,160 | 10,000 hits (the limit), 50 of 50 opened | 16,209 | 0 | yes |
| OST-1, 64 MB, Unicode 4K | 57 | 1,316 both, per folder | 37 of 37 | 757 hits, 50 of 50 opened | 774 | 0 | yes |
| OST-2, 111 MB, Unicode 4K | 61 | 1,395 both, per folder | 5,069 of 5,069 | 535 hits, 50 of 50 opened | 1,287 | 0 | yes |
| NST-1, 16 MB | 24 | 0 both | none | not run | refused then; opens and converts now, see above | n/a | n/a |
| ICS-1 | n/a | n/a | n/a | n/a | refused | n/a | n/a |

PST-1 converted in 21 to 30 s with a peak of 127 MB of memory. Before these fixes the app listed
14,350 of its 16,886 messages, and with only the reader fixed 1,114 of the 16,886 still failed to
convert. Each row's Converted plus the items left out as not mail equals its Messages. OST and PST behave the same. ICS
is not an input anywhere; the app only writes it. The counts in this table are from before the section
above, which replaced decisions 1 and 2 below on the owner's word.

Decisions the owner may overrule:

1. Replaced: NST files are accepted now.
2. Replaced: the converter decides by item now, in every folder.
3. **A message with no sender is written with `-` as the MBOX sender**, Thunderbird's own marker, and
   no From header. A message never sent uses the store's last-saved time on the separator line only.
4. **Tasks and notes were made browsable** rather than removing the promise from the header.
5. **A CISA finding is dropped only on NVD proof.** At most 20 findings per scan get NVD detail, so a
   patched app can still be listed beyond that.

Not covered by a test:

- The header and About text for the Vulnerability Scanner, and the Advanced scan label, are text.
- Moving a real mailbox file into Thunderbird or Outlook: see the section above.

### Network, assistant, help and leftover fixes (2026-11-02)

- FIXED: speed test and iPerf3 results wiped by the operation's end, confirmed at runtime on the
  capture VM. Stop and failure no longer show 0.00 Mbps, a failed latency no longer shows negative. A
  finished speed test reaches the report, which listed it as not run. One real speed test through the
  panel kept its result on screen and reached the report (one run only, about 24 MB). iPerf3 against a
  real server has not run; its label is tested with a result handed to the panel.
- FIXED: Reset Network's status bar left out the reboot. Its backup paths and step errors have reached
  the Log since the imaging round.
- FIXED: the adapter list read netsh's English words. It now comes from Get-NetAdapter. Compared with
  netsh on this PC, read-only: it lists all nine netsh did, plus Bluetooth and two Hyper-V switches,
  so the restore list and the assistant's adapter actions can see those too. A restore onto one is
  the person's choice, and the assistant still asks.
- FIXED: Backup and Restore Settings said Ethernet and work on any adapter. Flush Cache's tooltip
  claimed an administrator nothing asks for. F5 and Esc did not reach the Network Adapters tab.
- FIXED: Windows install ISO through `flash_image`, non-English SFC, Check Disk and network drives.
- FIXED: 85 buttons whose accessible name left out the words on them. The start-up audit now fails on
  this for every button built with the main window. Buttons in dialogs are not covered by that audit.
- REMOVED, each with no caller: `secure_memory_guard`, `locked_memory`, `lockMemory`,
  `generateSecureRandom`, `secureCompare`, `MigrationReport::generateReport`, and `removeMapping`,
  `hasMapping`, `getMapping` on `PackageMatcher`.

DESIGN DECISIONS, the owner may want to overrule:

- A button's accessible name must hold its label, and may add to it: "Stop HTTP speed test" for Stop.
  Equal names would give several panels two buttons with one name, which a screen reader cannot tell
  apart, and WCAG 2.5.3 asks only that the name contain the label.
- Bridge Connections was removed rather than made real. Creating a bridge is a network apply nothing
  here can certify, and the Windows window it opened is one menu away.
- Opening the log moves the main window to make room, when it is not maximized. A maximized window
  still has the log over its right side. Fitting that needs the log inside the main window, which is a
  layout change nobody asked for.
- Choosing Unattended Full Access asks once each time, and the choice is not remembered between runs,
  because the app already starts in Chat and Research.
- SFC's unreadable result stays unknown rather than being read from CBS.log's [SR] lines, which have
  never been seen from a real run here.

VM smoke, run on 2026-11-09 at f7619773 on the Windows 11 VM (`E:\SAK-Utility-Test-Scratch\vm-smoke\results.md`;
fix round S's proof under `E:\SAK-Utility-Test-Scratch\fixround-s\`). Each line that was INCOMPLETE here:

- PROVEN, and the buffer stays 64 MB: a real flash at 64 MB against 256 MB, testpc2's Samsung stick and a Debian live
  ISO (3.83 GB), three each. Medians 86.27 s and 85.17 s in all (write 37.83 s, 101.2 MB/s, against 36.87 s, 103.8
  MB/s): 1.3%, about a second, for four times the memory per target. `vm-smoke\tp2-flash-b64\`, `tp2-flash-b256\`.
- PROVEN: Flush Cache unelevated. The app's standard session said "DNS cache flushed"
  (`vm-smoke\cap-std2\vs_flush_cache_standard2.png`); as a real standard account `DnsFlushResolverCache` returned
  TRUE while `ipconfig /flushdns` said it needs elevation (`vm-smoke\logs\b_flush_standard_account.log`).
- PROVEN: Diagnose on a Windows with no msdt.exe opened Settings at Troubleshoot
  (`vm-smoke\shots\b_nomsdt_settings_opened.png`); with msdt.exe it opened the troubleshooter.
- STILL OPEN, not feasible on the VM: Restore Settings on a non-English Windows. A display language needs Windows
  Update and Features on Demand, which the guest's hygiene turns off.
- PROVEN: Backup Settings, Restore Settings refused in a standard session and applied elevated
  (`vm-smoke\cap-quick\`, `vm-smoke\shots\b_restore_elevated_state.png`), and, after fix round S, Reset Network on a
  static and a DHCP adapter, each with a restart (below). Still command building and result handling only: Set Static
  IP, Set DNS, Enable DHCP, enable, disable and rename from the adapter menu.
- PROVEN: SFC and Check Disk real runs through the helper from a standard session
  (`vm-smoke\cap-sfc\`, `vm-smoke\cap-std2\`).
- PROVEN: a real mapped drive through Check Disk, elevated: "Drive Y: - skipped, it is a network drive"
  (`vm-smoke\cap-elev\ve_check_disk_mapped.png`).
- STILL OPEN, by instruction: the assistant's Unattended prompt and the workflow notice through a real model run
  (a paid API).
- Also proven by the smoke: cold start, single instance and clean exit, limited and elevated; a Defender scan of
  the package; Mark of the Web (the unsigned publisher warning, no SmartScreen block); the helper's UAC start from a
  standard session and a 13-minute helper task; Chrome and Firefox profile backup with the running-browser prompt,
  restore onto another account, and the Chrome bookmark opened there (Firefox's is in the restored places.sqlite,
  its first-run card hid the Library). Not feasible there: a UUP conversion end to end (a 4-5 GB download and an
  hour's conversion on the throttling Mac) and a Windows USB (the Mac passes no USB device to the VM; proven on testpc1
  and a Hyper-V Gen 2 boot).
- STILL OPEN: the repeat of check A on the signed release bytes,
  `E:\SAK-Utility-Test-Scratch\vm-smoke\REPEAT_A_ON_SIGNED_BYTES.md`.
- DESIGN DECISION: the flasher ejects the stick after each flash ("Unmount drives on completion", on by default), so
  a second flash onto the same stick needs it replugged. That is what the setting promises.

### Leftovers of rounds 5 and 4b (2026-11-02)

**Tests and this PC's drives.** `tests/support/test_hardware_guard.cpp` is in every native `test_*`
binary. Without `SAK_TEST_ALLOW_REAL_HARDWARE=1`, which only ci.yml and nightly.yml set, it refuses any
open of a physical drive or of a volume other than the system drive's, from product code or Qt, and any
process whose command line queries a drive, scans or repairs Windows, changes the network or runs the
application, and any request for elevation. The test scripts that run the application exit 77, which CTest reports as skipped. A full
local `ctest` is therefore held off the USB drives by these guards, and not only by reading the tests.
What such a run can still touch:

- `QStorageInfo` (mounted volume names, labels and free space), which asks Windows and not the disk.
  `src/actions/generate_system_report_action.cpp`, `src/core/file_management_file_system.cpp`,
  `src/gui/email_file_scanner_dialog.cpp` and test fixtures use it. Qt's own open of each volume for its
  block size is refused like any other.
- `FindFirstVolumeW` and the PnP device list (`SetupDi*`), which name volumes and disks without opening
  them. The opens that follow are refused.
- The system drive's own volume (`\\.\C:`), by `QStorageInfo`'s block size query, with access 0.
- PowerShell tests run outside the guard. By reading them: the writer CLI tests leave out, without the
  variable, the checks that name `\\.\PhysicalDrive0` and `\\.\C:` (round 5c below), and the Create Image lock
  fixture mocks every storage cmdlet. None queries a real disk.
- `TEMP` and `SAK_TEST_IMAGE_SCRATCH_ROOT`, where the image tests write: whichever drive they name.

What the guard catches: a full run of the T2 labels on this PC, 298 tests, 296 passed and the 2 that run
the application skipped, in 218 s. It has 21 test binaries with calls refused, and each would have reached
a real drive before this. `test_image_flasher_panel`, `test_app_theme` and `test_accessibility_audit` open
every physical drive by building the Image Flasher; `test_drive_scanner`, `test_drive_unmounter` and
`test_flash_coordinator` open every volume; `test_app_theme` and
`test_ai_assistant_panel_tool_dispatch` run the sensor script, and the latter the inventory script;
`test_file_management_file_system` and `test_partition_manager_core` open GLOBALROOT disk paths; the rest
open a volume through `QStorageInfo`. They pass with those calls refused. Twelve test functions that only
the real answer can prove skip, and run in CI.

Decisions the owner may overrule:

1. **CI keeps reaching real hardware.** A hosted runner has no drive anyone cares about, and the real
   sensor and report scripts, the application's start-up and the real drive listing are only proved
   there. Tests fake what they can; a few run the real thing in CI only.
2. **A refused call fails with access denied rather than ending the test**, so a panel that lists drives
   at start-up carries on with none.
3. **An all-day event with no zone rounds to the nearest midnight**, as the owner asked. That is right
   from 11 hours behind UTC to 12 ahead. New Zealand in summer (+13), Samoa, Tonga and Kiribati land a
   day early when the event stores no zone: their 11:00 UTC is the same reading as American Samoa's.
4. **A word in a charset Windows does not know stays as written**, and so does broken encoding.
5. **Has attachments counts what opening the message lists**, which includes a picture in the body:
   a part that is not text, or that names a file.
6. **A button with words on it needs no accessible name of its own.** Qt reads its words, which is the
   rule the audit holds a named button to. The audit used to call such a button missing; adding a copy
   to the 100 or so in the dialogs would only let the two drift apart. A button with only an icon still
   needs a name.

Accessibility audit. The audit lives in `src/gui/accessibility_audit.cpp`; the application's audit mode
and `test_accessibility_audit` run the same code. It now holds a label to its text (a name that does not
contain the words hides them) and an image to a description, and it walks 12 dialogs and wizards beside
the main window: Email Attachments, Calendar, Contacts and File Scanner, File Properties, Image Flasher
Settings, Install Summary, Linux and Windows ISO download, Per-User Customization, and the Backup and
Restore wizards. It cannot reach dialogs built inside the function that runs them, 42 in all:
`partition_manager_panel.cpp` (13, plus `PartitionOperationDialog`, private to that file and built by 14
functions), `network_diagnostic_panel.cpp` (6), `advanced_uninstall_panel_dialogs.cpp` (5),
`file_management_explorer_panel.cpp`, `organizer_panel.cpp`, `wifi_manager_panel.cpp` (4 each),
`advanced_search_panel.cpp` (3), `ai_assistant_panel.cpp` (2), `user_profile_backup_wizard_pages.cpp` (1).
A label whose text is empty when the audit looks and which has both a name and a description would pass
it and hide its text later.

Zones and limits, proved here on synthetic data: `test_pst_recurrence` compares every zone in this PC's
Windows registry with Windows' own conversion, every hour of 2026, UTC to wall clock. It found two bugs in
the conversion: the hour before daylight time starts read as daylight, and a change on New Year's Eve
was judged by the wrong year's rule. `test_email_search_worker` holds the 10,000 limit on a PST.

INCOMPLETE:

- Import of an exported mailbox and ICS into a real mail client: done in round 5c below.
- The real application's accessibility audit over the new dialogs runs in T2 only, by standing rule.

### Round 5c: leftovers, and Outlook and Thunderbird as outside checks (2026-11-02)

**Did past local test runs ask for write access to the owner's real drives?** Measured on this tree with
every T2 label on this PC, the guard on, under `scripts/run_ctest_no_dialogs.ps1`, and a temporary log of every
device-namespace open, refused or not. 298 tests, 296 passed, 2 skipped (they run the application), in 197 s. The log's
263 refused opens are the run's 256 guard lines plus the 7 from the guard's own test, whose children's lines it swallows.

| What was opened | Query only (access 0) | Read | Write |
|---|---|---|---|
| Physical drives 0, 1, 2 | 66 | 0 | 0 |
| Volumes by GUID | 20 | 0 | 0 |
| Drive letters other than C: (D: 55, E: 111) | 166 | 0 | 0 |
| C: (let through) | 56 | 0 | 0 |
| GLOBALROOT partition paths | 0 | 4 | 0 |
| Names no PC has (`PhysicalDrive999`, `PhysicalDrive1023`, `NoSuchVolume999:`) | 0 | 0 | 3 |

The guard's own test adds 7 more, all refused, on names this PC does not have. No open asked for write on anything that
exists. The 3 that did ask are `test_flash_worker`, `test_flash_coordinator` and `test_drive_unmounter`, each proving the
missing-device path. Had one of those names existed, `FlashWorker` would have written the image: they rely on
Windows never numbering a drive that high.

A refused open hides what would have come next, so the rest is by reading, not by measuring. Each query
above has nothing after it that writes: the physical drive opens are `DriveScanner` building the Image
Flasher's list, and that suite empties the list and plants a fake row before anything can flash; the volume
opens ask which disk a volume is on, for drive numbers no PC has; the drive letters are `QStorageInfo` asking
a volume's block size. The GLOBALROOT reads are a size probe, on disks 4, 63 and 99, and this PC's numbered
drives stop at 2. Only `CreateFileW` from a test binary and from Qt is seen: by search, no first-party source
opens a device any other way. Older commits were not replayed.

Two script tests were outside the guard and aimed at the real disk, on every local run since August 2026:
`test_sak_apfs_writer_cli` ran `format-raw --target \\.\PhysicalDrive0 --allow-raw-target` and the same on `\\.\C:`,
and `test_sak_hfs_writer_cli` ran `delete-file-image` on both. By reading, nothing reached either disk for
write: the OS-disk guard in each CLI's `main()` runs first, refuses `PhysicalDrive0` by its number without opening
it, and opens `\\.\C:` twice with access 0 to ask its extents. `--confirm-target` was left out as well, as a second
wall. Those checks now run only in CI, and so does the same shape in T4's `test_partition_manager_live_certifier`,
so a local run no longer rests on a refusal with the drive Windows runs from behind it. Proved with a stand-in
CLI that records its arguments: 3 device paths under the old scripts, 0 under the new. CI still runs them.

`test_bandwidth_tester` tried a real `netsh advfirewall firewall add rule` on this PC on every local run. Unelevated
it failed, and no `SAK_Utility_iPerf3_*` rule is on this PC now. An elevated run would have added one and
removed it. The guard now refuses adding, deleting or setting a firewall rule, 2 refusals in this run, and refuses a
write to the system volume, which it had let through. Its line ends with the access mask and share mode.

Also read-only and run by every local T2 run, outside the guard's list: `Get-AppxProvisionedPackage -Online`,
`Get-ComputerRestorePoint`, `schtasks /query`, `netsh advfirewall firewall show`, `Get-NetAdapter`, `choco list --local-only`.

**How a local test run put boxes on the owner's screen, and why it cannot now.** Running the nightly's Debug suite
here, on purpose, as evidence for its first run, made test binaries raise "zd.dll was not found" boxes on the owner's
desktop. 34 of 287 tests had results when it was stopped, 4 of them that failure. Plain `ctest` resets the error
mode its children inherit: a probe under it read `0x0000` with the parent at `0x8003`, so setting the mode in a
parent does not reach the tests. `ctest --interactive-debug-mode 0` does, and the probe reads `0x0003`. The same
missing-DLL binary then exits `0xC0000135` in under a second, with no box, where it had waited up to 54 s on one.
`scripts/run_ctest_no_dialogs.ps1` runs suites that way on a PC someone is working at, gives every test the offscreen
platform, and refuses `SAK_TEST_ALLOW_REAL_HARDWARE`. The hardware guard sends crash and CRT assert boxes to stderr,
which no error mode reaches. Not covered: a helper program a test starts that is not itself a test binary has the
error mode but not the CRT redirect, and only a test binary's own Qt platform is known to be offscreen.

**Mutation catalogs and the workflows that have never run.** `run_all_mutation_catalogs.py --validate` passes: 61
catalogs. Seven mutants in five catalogs were re-pointed and each was built and run: all killed. `release.yml` and
`nightly.yml` are not on the default branch, so neither has run. Checked on this tree:

- T4 CTest labels, all 28 on this PC with the variable unset: passed. In CI they run with it set, as a standard user.
- The nightly's Debug and ASan job, run here, would have gone red on its first run, and now passes on this PC: 287
  tests, 286 passed, 1 skipped, 955 s. What it would have hit: `C1128` stopping the build in
  `test_ai_assistant_panel_tool_dispatch`; 31 test binaries importing `zd.dll` with only vcpkg's release `bin` on
  `PATH`; four suites looking for bundled tools beside an executable in `tests/Debug`; a reference into a temporary in
  `test_pst_parser` (ASan: heap-use-after-free) and an `error()` read on a success in `test_microsoft_defender_action`
  (a CRT assert in Debug, undefined in Release), both test bugs; and `test_resource_soak`'s 16 MB ceiling against
  30 MB of ASan's quarantine. Run with the offscreen platform for every test, where CI keeps the native one for three.
- `check_partition_manager_release_claims`, certification matrix integrity, commercial gate matrix, tool
  manifest, Qt resources, Apple capability matrix and third-party licenses: pass in this worktree.
- `check_dead_code` passes, 313 files. `cppcheck_whole_project_gate` passes.
- The signing action's input names match its pinned version. The Azure secrets date from 2026-02-27, and
  nothing here can tell whether the client secret has expired.
- `release.yml` built all 297 targets on one runner, about 90 minutes and 17 GB by the T2 shards, against a 180
  minute job and GitHub's stated 14 GB. It now builds what it ships and what readiness runs.

Still not proved: `release.yml`'s smoke steps, which run the app elevated where T2 runs it as a standard user, and
may not run on this PC by the owner's rules. The clang-tidy naming gate and the coverage job were not run here.

**Decisions the owner may overrule:**

1. **A release builds only what it ships and runs**, not the 280 test executables. If a readiness script ever
   needs another target, the build step is where to add it.
2. **Restore Settings starts on the adapter selected in the table.** It listed them alphabetically with the first
   chosen. The confirmation always named the target.
3. **A shown picture sits in multipart/related, and an EML's plain and HTML in multipart/alternative.** The MBOX
   and EML writers share `MimeLayout`, so they cannot differ on this. The rest of each writer is still its own.
4. **A distribution list's members go in a vCard NOTE,** because vCard 3.0 has no field for them.
5. **A single event exports in UTC.** The instant is right in both clients, and it comes back in the reader's zone,
   not Tokyo. A repeating event keeps its zone, because its daylight changes depend on it.

**Outlook, 2026-11-02, on a store Outlook wrote.** Outlook 16.0.20326, in a throwaway PST on E:, in a separate
profile with no account made by `outlook.exe /PIM` where it mattered. All content fictional. The PST held the owner's
address as the organizer of its meetings, so it was not committed, and it is deleted.

- The reader against what Outlook itself reports: 357 items, 0 errors. 326 mail in folders of 6 and 320, 24 calendar,
  2 contacts, 1 distribution list, 2 tasks, 1 note, 1 journal entry. Folder counts, senders, recipients, attachments
  and their Content-IDs, start and end, all-day, location, attendees and their roles, zones, every recurrence pattern
  with deleted and moved occurrences, the journal entry's type, start and duration, task status and dates all match.
- Found and fixed, all things the reader and its own builder had agreed on: every RTF-only body (notes, tasks, journal
  entries) ended in a NUL that reached exports; a contact's picture was never read, so the vCard's PHOTO line could
  never run; a distribution list's members were never read, its card had an empty heading, and exporting it to vCard
  failed. One difference stands: an embedded message saves as `.eml` where Outlook says `.msg`, since the file is one.
- The app's ICS through Outlook's own importer, the whole file into a calendar in the no-account profile: 90 occurrences
  written, 90 read back, 25 of 25 series identical, including a weekly series across this zone's change, a deleted and a
  moved occurrence, and zones east and west of UTC. `Namespace.OpenSharedItem`, which is not the importer, refuses a
  file holding a series and its override. Differences: a single event returns in the local zone (decision 5). The 2
  meetings gain an "Unknown" attendee, because the store holds only an Exchange path for their organizer, so the ICS
  has no ORGANIZER line.
- The raw all-day start on this PC's real zone: local midnight in UTC, 08:00Z in winter and 07:00Z in summer. That is
  what round 5b's all-day rule rests on.

Not proved by that: one Outlook, one zone. Mail items were first saved through the real profile and then moved in, so
they are Outlook's own bytes but not a store written only offline.

**What the Outlook check did to the owner's profile.** Three things went wrong, all recovered, all on this PC:

- Attaching a copy of the sample PST crashed Outlook, an access violation in `MSPST32.DLL` (`0xc0000005`, offset
  `0xf7d60`) with Outlook's own event 2000, "Header verification failed. bidNextB=0, bidNextP=0". It left a data file
  entry with no path in the profile, and Outlook then asked to configure it at every start. It was removed through Account
  Settings, Data Files, and the store count is back to 2.
- `folder.Items.Add` for a mail item saves to the default Drafts, not that folder. 326 fictional drafts sat in the real
  Drafts for about 10 minutes, Outlook connected, before being moved into the PST. Nothing was sent, and Outbox and Sent
  did not change. Nothing here can prove what the server held, or that it caught up.
- `outlook.exe /importprf` opened the real Inbox in a minimized window for about a minute. Nothing was touched in it.

At the end: store count 2, every default-folder count the same as before, a clean `Quit`, and 1 crash in the log.

**Thunderbird 159.0, 2026-11-02, on the capture VM.** A copy extracted from Mozilla's signed installer (hash and
signature checked), headless, with a new profile, no account, and every network pref pointed nowhere, driven through
Marionette from the guest. It read the real converter's MBOX and the real ICS writer's file from the store Outlook wrote:

- MBOX dropped into Local Folders: 326 of 326 messages. Non-English subject, sender, body and attachment name read
  right, attachments are listed, and a picture the HTML shows loads in place, 48 by 48, with no warning and no attachment
  listed. An earlier run on a generated store found that picture in `multipart/mixed`, showing its alt text with a
  remote-content warning; that is the fix in `MimeLayout`.
- ICS: 24 series, no orphans, 90 of 90 occurrences on the instants Outlook reports, 25 of 25 series, with the deleted week
  gone and the moved occurrence on its new date with its own subject and place. Attendees keep their role and reply.

The first run of that check was on this PC, headless, from a scratch copy. It moved to the VM by the owner's decision,
and the VM was restored to its `pre-thunderbird` snapshot afterward.

Not proved by that: one Thunderbird, one zone, and ImportExportTools NG was not used; a dropped file goes through the
same reader.

### Round 6: Partition Manager and the administrator helper (2026-11-03)

**The app could not talk to its own administrator helper from 2026-08-02 until this round.** Since `9aa76892` each
end of the pipe checks which program is on the other end. Each compared a path from Qt, with forward slashes, against
one from `QueryFullProcessImageNameW`, with backslashes. They never matched, so every task handed to the helper failed
with connection error 411. Every test of the check passed both paths in one spelling, and nothing connected the two
for real. A run of the real helper in the VM found it; the first run failed with 411 and the runs after the fix passed.
Fixed in `2a9573e4`. Read anything in this file between those dates that says a helper task worked with that in mind.

**What ran in the VM, on its own throwaway virtual disks.** The MacBook's `Windows 11` VM, 8 new VDIs on SATA, each
found by its exact size and bus right before anything changed it. The app's real code through a throwaway driver that
is not in the tree, with the real helper over the real pipe where marked. No window was involved, so this says nothing
about the dialogs. The VM went back to its snapshot afterward and the VDIs were deleted.

| Test | Result |
|---|---|
| BitLocker Key Backup to exFAT, no consent, in-process and through the helper | Refused, nothing created |
| The same, with consent | Saved, all three kinds of file, and the result and log say not protected |
| To FAT32 with consent | Saved, with the same words |
| To NTFS, in-process and through the helper | Saved; folder inherits nothing and a file is closed to ordinary users (in-process run) |
| To the BitLocker volume whose key is in the backup | Refused |
| `convert.exe G: /FS:NTFS /NoSecurity` as shipped, on a labeled FAT32 volume | Failed: "An incorrect volume label was entered for this drive", exit 4 |
| The app's convert script, labeled and unlabeled FAT32 | Exit 0, NTFS, label kept, file survived |
| Split with a larger free area elsewhere | New partition starts where the first ends, ends where the first used to, file survived |
| Recovery scan on NTFS, FAT32, exFAT as Windows formats them | Each found once, at its true offset |
| BIOS boot repair on this EFI VM's boot disk | Refused before writing: no active partition |
| The same script, its disk selection swapped to a throwaway MBR disk, S: held by another volume | Exit 0, a free letter used and taken away, S: left alone, `bootmgr` and `BCD` on the active partition |
| Defrag on a disk Windows calls `Unspecified`, model `VBOX HARDDISK` | Exit 0, NoErrorsFound |
| Data Recovery on raw `\\.\B:`, 2.08 GB, PDF at 693 MB | Read to the last byte, found, no warnings |
| Data Recovery on a source that will not open | Says it could not read |
| Helper task through the real pipe, 7,200 s asked, sleeping 3,700 s | Ran 3,701 s, succeeded, not timed out |

**How long a whole-source scan takes.** It first tried all three signatures at every byte: 11.5 minutes for those 2.08
GB in the VM, and about 8 MB/s on this PC with its image on USB, more than a week for 8 TB. `b430ffdc` looks for a
signature's first byte first: 45 s in the VM with the same result, and the test's 512 MB image went from 62.5 s to 4.6 s.

**INCOMPLETE, and why:**

- UEFI boot repair's real run. It writes to the Windows that is running, and the VM's OS disk was off limits this
  round. Its script text is tested and the free-letter function it shares with BIOS repair ran.
- BIOS boot repair's positive path ran only with the script's disk selection swapped, because it rightly refuses this
  EFI VM's boot disk. Neither branch was followed by a boot from that disk.
- OS migration, a full format of a large disk, and a clone past one hour were not run. The new limit was proved with
  a sleeping task through the real helper, and each script's limit by test.
- No dialog ran on a real desktop: BitLocker's warning, the Data Recovery notice and Not Added to the Queue are tested
  offscreen.
- The Partition Manager pages' screenshots are not retaken.

**DESIGN DECISIONS:**

- The long limit is 23 days, not scaled to the disk. Scaling needs a guess at the disk's speed, and is wrong on the slow
  one, which is the one that matters. Cancel is what ends a long job. Only layout changes keep a short limit.
- Defrag's script follows its validator: a model name with HDD or HARD counts as a hard disk, since a VBOX disk reports
  `Unspecified`. SSD and NVMe are refused first.
- Boot Repair repairs the running Windows, and the text now says so. Repairing a Windows on an attached disk needs a way
  to find that Windows' system partition, in both firmware branches, plus real boot tests.
- OS migration keeps copying a running disk, and warns so. A snapshot needs per-volume VSS, reading each volume from its
  shadow device and the raw regions around them, rebuilding the layout, and real boot tests. Too large for this round.
- BitLocker Key Backup's no-person routes (AI Assistant, Unattended) refuse FAT32 and exFAT.

**Writer B's items.** B1 limits, B2 wizard options, B3 and B4 Data Recovery, B7 refusals, B8 log text, B9 FAT in the
recovery scan, B10 and B11 APFS, B12 BIOS repair, B13 Split, B14 Check Now, B15 labels and B16 Create Image are fixed. B5 is
a warning only, as above. B6: "certified" is gone from user-visible text in Partition Manager, File Explorer, About, the
writer tools and the assistant's prompt. No claim level changed, and nothing under `certification/` changed.

**What no automated test covers.** Script text is asserted; the scripts themselves run only in the VM. Panel tests prove
what a dialog offers and what it sends, offscreen. Nothing connects a real helper over a real pipe in CI:
`identity_imageChecksCompareTheFileNotTheSlashes` tests the comparison, and this round's VM run is the only end-to-end
proof, which is how the defect above lived for three months.

### Round 6b: Migrate OS and boot repair, and what booted (2026-11-03)

**Migrate OS no longer copies a live disk, and Boot Repair repairs the disk you select.** Migrate OS used to copy a
running Windows byte for byte and warn. It now reads each volume from a VSS snapshot taken at the start, writes a new
partition table with its own identity, and fits the Windows partition to the target, then rebuilds the boot files and
Windows' list of drive letters. Boot Repair of a disk other than the one Windows runs from finds that disk's Windows
and system partition, and rebuilds its boot files, leaving this PC's firmware boot order alone. The code is
`src/core/partition_os_migration.cpp` and `src/core/partition_scripts`.

**How it was proved.** The MacBook's `Windows 11` VM migrated its own running Windows onto throwaway VDIs through the real
inventory scan, planner, queue, validator, executor and helper over the real pipe. The larger UEFI run went through the
real application window, wizard to Apply, driven by the capture harness. The others replaced only the wizard's dialogs,
with a driver that is not in the tree (`scratchpad\round6b`). Each target then started in a separate throwaway VirtualBox
VM with no other disk, and each screenshot came from VBoxManage. The checks from inside ran after signing in as the VM's
Technician account.

The VM's Windows is UEFI, so the BIOS runs needed a real BIOS install first. It was made by imaging that Windows from a
snapshot onto a 100 GiB MBR VDI with DISM, `bcdboot /f BIOS` and `bootsect`. It started in a VM with BIOS firmware and
reported `Firmware: Legacy`, and every BIOS run below had it as the running source.

| Target | Firmware | How filled | Verify | Took | Result |
|---|---|---|---|---|---|
| Larger, 320 GiB, real window | UEFI | Used blocks, then grown | Sample | About 3 min | Lock screen |
| Smaller, 80 GiB | UEFI | DISM image on the target | Sample | 74 min | Lock screen |
| Larger, 120 GiB | BIOS | Used blocks, then grown | Full | 225 s | Lock screen |
| Smaller, 80 GiB | BIOS | DISM image on the target | Sample | 110 min | Lock screen |

Inside each: `SystemDrive` is `C:`, the firmware is the one named, `Repair-Volume -Scan` says `NoErrorsFound`, and `C:` is not
dirty. On the UEFI targets `K:` kept its letter and a marker file, and a marker `.ost` and a marker in
`SoftwareDistribution` were there. The BIOS install has no data volume or recovery partition, so those were not exercised
on MBR. On the larger UEFI target the Windows partition grew to 340 GB, `{bootmgr}` no longer holds `nofirmwaresync`, and
hard links between `System32` and `WinSxS` survived. Everywhere, the first start logs an unexpected shutdown (41, 6008),
because the copy is of a running PC. On the used-block targets, UEFI and BIOS, Windows Search also stopped once at the
first start (7031, 7024), and it was running again when checked. Whether its index needed rebuilding was not looked at.

| Boot Repair | Before | Broken, on its own | Repair through the helper | After |
|---|---|---|---|---|
| UEFI, attached disk | Starts | `EFI\Microsoft\Boot` and `EFI\Boot` removed: "BdsDxe: No bootable option or device was found" | 26 s, temporary letters gone | Lock screen |
| BIOS, attached disk | Starts | `bootmgr`, `Boot` and the MBR's boot code removed: "No bootable medium found!" | 22 s, temporary letters gone | Lock screen |

This closes round 6's two INCOMPLETE boot repair items. The first BIOS attempt proved nothing and is not counted: the
script breaking the disk could not delete `bcdboot`'s files, so the "broken" disk still started. The script was fixed and
the disk that was then repaired really had failed to start.

| Refusal or stop | What happened |
|---|---|
| 20 GiB target | "Target disk is too small for the data on the Windows volume: 28.99 GB in use needs a disk of at least 56.84 GB". The disk stayed RAW |
| VSS service disabled | "Windows could not take a snapshot ... Nothing was written". The disk stayed RAW |
| VSS service disabled, check box ticked | The warning, and the copy goes ahead. Cancelled at 20 s, with the target left offline |
| BitLocker on `K:` | Refused by the queue. With the queue skipped, refused by the script: "K: is encrypted with BitLocker, so nothing was written" |

**What the first runs found, each fixed:**

- A snapshot taken the ordinary way is not the volume. Windows' Shadow Copy Optimization Writer deletes from it what the
  registry lists under `FilesNotToSnapshot`, which on this VM included `*.ost` under each profile's Outlook folder and
  all of `%windir%\SoftwareDistribution`. A target built from such a snapshot silently loses Outlook's cache, which can
  hold mail that has not synced. The migration disables that writer for its snapshot set.
- The VM's Windows is in the Compact state. A plain DISM apply stores files whole, so the image route ran out of room on
  a volume sized for the used data, with exit 112. It now applies with `/Compact` when the running Windows is compact,
  and what a smaller target has spare is shared between the new volume and the image.
- A snapshot device ends at the last cluster, so the backup NTFS boot sector past it cannot be read from there. It is
  written from the first sector, which is identical.
- DISM's default exclusions include `Windows\CSC`, the Offline Files cache, which can also hold work that has not synced.
  The capture uses that list without it.
- A failed Partition Manager job's message was "Command exited with code 1", with the script's reason only in the log.
  The message is now the script's own words.
- The whole script is 52 KB, past what a command line can hold. A long script now reads itself from standard input.
- Asking a snapshot a second time is not a check. A snapshot keeps no copy of a free cluster or of the page file, so what
  it reads there follows the running volume. The first migration through the real window stopped with "Verification found
  a difference at target byte 228065280": cluster 128 of `C:`, a free cluster Windows had just given to the prefetch file
  of the snapshot service itself. Comparing only clusters in use was not enough: a Full run stopped at a cluster of
  `pagefile.sys`. The copy was right both times, and the earlier driver runs passed by the timing of what Windows wrote.
  The copy now keeps a 64-bit checksum of what it read, block by block, and verification reads the target back against
  those: every block in Full, 64 spread over each partition in Sample. A copy made with no snapshot now gets verified
  too, which it could not before.
- On MBR a smaller target stopped with "The target disk's volumes did not appear after it came online". A raw partition
  is hidden so that Windows does not offer to format it. GPT has an attribute for that. MBR only has a hidden type, and
  that leaves the partition with no volume at all. Each partition's type now changes just before its format, and in the
  VM the volume came in under a second, the format took three, and no format prompt came up.

**The desktop check found two things no earlier test could.** Both came from looking at the capture harness's work in
the VM, and both are fixed.

- Every plain button in the application drew in the disabled grey, on or off, because the theme's `:disabled` went on
  the last selector of `QPushButton, QToolButton`. In the wizard a Finish that was off looked the same as a Cancel that
  was on. They now draw in the theme's colour. Every dialog in the application changes look, so existing pictures of
  dialogs are out of date.
- Qt's accessibility press on a tool button runs what it does inside the UI Automation call, so a wizard opened from the
  Partition Manager sidebar held every later UI Automation request until it closed: 25 second timeouts on every read. The
  sidebar actions, the context menu actions and Apply now queue what they do, and UI Automation read the wizard and
  Apply's review at once. Tool buttons on other panels that open a dialog behave as these did, and are not changed.

The BitLocker warning on an exFAT drive, both wizards' pages and both boot repair prompts were looked at in the real
application in the VM. The Copy Disk wizard still lists the disk Windows runs from as a target. The validator refuses it
after Finish ("Payload target disk is system, boot, read-only, dynamic, or Storage Spaces"), so nothing can be written to
it, but the wizard could say so sooner.

**CI now connects the real helper.** `test_elevated_helper_connection` copies itself beside a copy of the helper, as
`sak_utility.exe`, starts the helper the way the application does, and runs a harmless task over the real pipe. In the VM
it passed, and with the August path comparison put back it went RED with broker error 411. Its first T2 failed: CI's
suite runs under a standard-user token, where `runas` needs a consent prompt that nobody on a runner can answer, and it
waited two minutes and read as declined (broker error 409). So ctest skips it there and an elevated CI step of its own runs
it, and fails if it skipped. That step proves an elevated client and the helper accept each other on the runner. A
standard-user client getting past a real consent has only the VM runs as proof.

**Cancel.** A real Cancel through the helper stopped the script at once. The two snapshots were listed for 361 more
seconds and then gone, with nothing cleaning them up: they belong to the process, and Windows releases them when it
ends. The target stayed offline, which the wiki says.

**INCOMPLETE, and why:**

- Physical drives, NVMe and 4Kn. Every run was on VirtualBox SATA VDIs. This round had to leave the 8 TB and NVMe drives
  alone for round 7.
- Real firmware boot order, Secure Boot and a TPM in the target. The target VMs had neither, and each had one disk.
- A Windows 10 `bcdboot` with no `/nofirmwaresync`, and so the `/addlast` branch. It is text-tested only.
- Windows Recovery and a data volume on MBR, because the BIOS install has neither.
- A source with ReFS or FAT data volumes, OneDrive placeholders on the image route, a domain-joined source, a dual-boot
  source, and a real writer failure from SQL Server or the like.
- What a Cancel leaves when it lands while the target's SYSTEM hive is loaded or the snapshot has its temporary letter.

**DESIGN DECISIONS:**

- A target at least as large as the old Windows partition gets the used blocks from the snapshot and then a grown volume.
  A smaller one needs an NTFS relocator or a file-level copy, and DISM is Microsoft's own, so a smaller target goes through
  a DISM image. That image needs room on the target, so a target is refused unless it holds the used data, 5 percent or
  2 GiB of room, and an image reckoned at three quarters of the used data. **This is narrower than "refused only if too
  small for the used data".** A target that holds the data but not the image is refused with the number. Taking the image
  to a third disk, or to the source's free space, would close that gap: a folder choice in the wizard, plus cleaning it up
  after Cancel. Not done this round.
- Verification checks what reached the target, not what the source says now. A read from the source that returned wrong
  data and raised no error goes undetected. Asking the source again could not catch that reliably either, as above.
- BitLocker on any source volume is refused with the reason. A shadow device reads decrypted, so a copy would put a
  customer's data on the new drive unencrypted without saying so.
- Copying without a snapshot is a check box on the Options page, off by default. The script cannot ask anyone, so it stops,
  says why, and says where the box is. A smaller target refuses even with it ticked.
- Windows Recovery: the script points the new Windows at the recovery partition copied to its own disk, and it stays off
  until `reagentc /enable` is run once from the new disk, which the wiki says. In the VM that one command gave `Enabled`.
- bcdboot takes `/nofirmwaresync` where it has it, so this PC's boot order is never moved to a disk that is only attached.
  On a Windows whose bcdboot lacks it, `/addlast`.
- The bcdboot used is this PC's, never the one on an attached disk: running a program from a customer's disk with an
  administrator token would run whatever is on it.
- A disk with two Windows installs is refused rather than guessed at.
- A disk with extended or logical partitions, a dynamic or Storage Spaces disk, a Windows that boots from another disk, and
  a pair of disks with different sector sizes are refused, each with its reason.
- On MBR a partition gets a drive letter from this PC in the second before its format. The script uses that letter and
  takes it away at the end. Only AutoPlay's notice came up on screen.

**What no automated test covers.** The scripts' real work runs only in the VM. `test_partition_os_migration` plans, reads
the script's text, compiles its C#, and runs the copy and read-back code on ordinary files. The bitmap read, the snapshot,
`bcdboot`, DISM and every partition change have only the runs above as proof. No claim level changed, and nothing under
`certification/` changed.

**Writer B's B5 and the owner's defect 3 are closed.** Round 6's design decisions above about both no longer apply.

### UI review round 1: one theme, readable in light and dark (2026-11-04)

An independent review of every screen in light and dark (`findings-light.md`, `findings-dark.md`, 2026-11-04)
found 50 light items (S1-S26 systemic, F1-F50) and 24 dark ones (D1-D8, DF1-DF16). Fixed at the theme level where the
cause was shared, so both themes take each fix.

**Theme.** Check boxes show a tick (a style sheet cannot load a data: URL), partly checked a dash, a chosen option a
dot, and item-view boxes match. Selected input text, disabled inputs, placeholders, the focus frame in item views, row
selection (one opaque shade, no per-cell pieces), spin-box height, plain containers in white dialogs, link buttons in
the status bar, submenu arrows, border and fill shades, header weight while a row is selected, and File Explorer's
chrome states. In dark, plain buttons keep the primary fill (the dark QWidget rule came after the button rule) and
progress bars use the dark border.

**Contrast tokens (WCAG AA 4.5:1 for text, 3:1 for a control outline), darkened or lightened the least, same hue.**

| Token | Before | After | On |
|---|---|---|---|
| kColorTextMuted | #64748b, 4.36:1 | #606f85, 4.68:1 | page #f3f5f9 |
| kColorSuccess, kStatusColorSuccess | #16a34a, 3.02:1 | #117f3a, 4.67:1 | page |
| kStatusColorWarning | #d97706, 2.92:1 | #a65b05, 4.67:1 | page |
| kColorWarning | #ea580c, 3.26:1 | #bf480a, 4.64:1 | page |
| kColorError, kStatusColorError | #dc2626, 4.42:1 | #d62323, 4.67:1 | page |
| Primary button middle stop (white text) | 3.21:1 | 4.54:1 | white dialog |
| Selected tab gradient middle (white text) | 3.73:1 | 4.54:1 | white |
| Check box outline | #94a3b8, 2.56:1 | #7d8fa9, 3.02:1 | page |
| Dark check box outline | #475569, 1.9:1 | #60738e, 3.02:1 | dark page |
| Dark success / amber / warning / error / running / violet / error text | light values | #16a54b, #d97706, #f36013, #e66767, #5e8cf0, #a476f3, #e65757, each 4.5:1 or more | every dark surface |
| palette(dark), File Explorer secondary text | #94a3b8 light, #64748b dark | kColorTextMuted, kColorDarkTextSecondary | base |

Secondary, success, danger and Discord button stops moved the same way where white text was under 4.5:1. The unused
pause button tone is unchanged.

**Layout.** One Log switch per tool tab, at the left margin under the page (under the sub-tabs on Email Tools,
Application Management and Network Management), through `addLogFooter`. Image Flasher's step buttons are a fixed
footer. Sub-tab margins, form label columns, header labels that fit, rows that fit their editors, hidden form rows.

**Copy.** One dash style (" - "), no "[x]", "[OK]", "[shield]", "[save]", "[FOLDER]", "(i)" or "(!)" stand-ins, plurals that
agree, the About date, the converter's totals, drive names as Windows shows them ("Local Disk (C:)"), picked folders
with backslashes, install dates as dates. One wizard style (ModernStyle) for every wizard: left to Windows, the
Partition Manager wizards used the Aero frame in light and Qt's classic one in dark. The email ribbon's Attachments
button is named "Browse Attachments" for screen readers, apart from the Attachments tab.

**Stress test.** A 1-minute run kept going in the capture VM: a memory pass over gigabytes never checked for the end
of the run, and the stress threads crowded out the window and the run's clock. Passes check every 8 MiB; stress threads
run below normal priority.

**Not changed, by decision.** The Vulnerability Scanner's findings table is wider than the window and scrolls (F18).
Help and About have no Log switch: they log nothing. Faint File Management and Email Tools tab artwork (S25), tabs
half under the scroll arrows, and remaining items listed in the round's report are open.

**What no test here covers.** Pixel tests run offscreen on Qt's Windows 11 style, not on a real display. The proof
pictures from the MacBook VM are the check of what a person sees. The stress fix is unit-tested at the pass and thread
level; a full 1-minute run in the VM is the end-to-end check.

### Round 7: Partition Manager's certification now tests the app (2026-11-04)

**The old rows tested Windows and VirtualBox, not this app.** Evidence from before this round ran Windows' own commands, and much of it assumed a helper task worked during the months it could not (Round 6). Every row now goes through the app's own request building (`src/core/partition_operation_requests.cpp`), its controller and queue, and the real `sak_elevated_helper.exe`, from a client under a standard-user token, and a separate script reads the disk back. Old evidence stays in git and cannot be selected. `docs/PARTITION_MANAGER_CERTIFICATION.md` describes the rows, the levels and the procedure.

**No claim changed.** The level is `CodeCompleteOnly`, 0 of 12 and 0 of 18, until the final run. Trial evidence stayed in scratch.

**What the real path found.** Each of these was broken for a person using the app, and no earlier test could see it:

| Found by | What was wrong | Since |
|---|---|---|
| Every row | Apply was impossible from an unelevated app, because BitLocker and dirty state read as unknown and blocked it | always |
| `vhd.empty-style-conversion` | Empty GPT refused for its reserved partition, empty MBR stopped with an error | always |
| `vhd.partition-clone-region` | Copy Partition failed at the end of every mounted NTFS volume, and read it live | always |
| `vhd.clear-disk-wipe` | Wipe and full format on a thin disk failed with "Not Supported" after the disk had been cleared | always |
| Six backup-and-restore rows | Move, Allocate, Cluster Size, Primary/Logical, Volume Serial and Dynamic to Basic all stopped on NTFS | 2026-07-22 |
| `external.bitlocker` | One locked volume stopped every Apply, including the one that would unlock it | always |
| `vhd.recovered-partition-restore` | Windows blanks the start of a new partition, so a restored partition read as empty | always |
| `external.file-level-data-recovery` | Raw scans were always called partial, raw restore was impossible, the first JPEG was lost in NTFS's upcase table | always |
| `external.dynamic-to-basic` | Dynamic disks were missing from the table, so conversion could not be reached, and its diskpart lines were one line | always |
| `external.primary-logical-conversion` | A read-only partition could not be copied | always |
| `external.ssd-secure-erase` | Every disk scanned with no media type, so HDD defrag was refused on a hard drive whose name does not say so, and an SSD wipe never warned it is not a purge | v0.9.1.8 or before |

**The trial.** MacBook VM `Windows 11`, throwaway VDIs, through the real path. Commits differ between rows, because code changed as rows found things, and each row was checked against the digest it was built from.

| Row | Ran on | Result |
|---|---|---|
| The twelve `vhd.*` rows | VHDX in the VM; `vhd.clear-disk-wipe` on a throwaway VDI, because every VHDX reports thin | Accepted |
| `external.file-level-data-recovery`, `bitlocker`, `bitlocker-mutation`, `allocate-free-space`, `partition-move`, `cluster-size-change`, `primary-logical-conversion`, `volume-serial-number` | VHDX in the VM | Accepted |
| `external.dynamic-to-basic` | Throwaway VDI on SATA, because Windows will not make a VHDX dynamic | Accepted |
| `external.os-migration-reboot` | Running Windows to a throwaway VDI, which then started alone under UEFI with the source's marker | Accepted |
| `external.boot-repair-uefi`, `external.boot-repair-bios` | Throwaway VDI: it starts, its boot files are broken and it does not, the app repairs it from another Windows, and it starts again alone | Accepted |
| `external.system-mbr2gpt` | A BIOS Windows converted while running from it, then started under UEFI | Accepted |
| `external.ssd-retrim`, `external.ssd-secure-erase`, `external.hardware-wipe` | Throwaway VDI that Windows reports as SSD | Accepted, as a virtual machine only |
| `external.usb-removable` | Throwaway VDI behind a virtual USB controller | Accepted, as a virtual machine only |
| `external.hdd-defrag-execution` | Ran on a VDI, which Windows reports as `Unspecified` | Refused, correctly: it accepts only a real hard drive, and has not run on one |

One BIOS repair run is set aside: its last proof reached sign-in only after Enter was pressed at "Your PC couldn't turn off properly", because the rig had pulled the power on the proof VM before it. The rig now shuts proof VMs down from inside, and the rerun passed with nobody touching it.

**Real drives did not run.** The Seagate Expansion Desk 8 TB and the NS-PCNVMEHDE(-C) 256 GB adapter cannot reach Windows through VirtualBox on this Mac. Under root both showed `Captured`, but macOS refused authorization (`irc=0xe00002eb`), Windows' `USBSTOR` failed with Code 10 (`0xC0000001`, `0xC000000E`), and xHCI reset every two minutes. On a USB 2 controller VirtualBox refused both, `VERR_PDM_NO_USB_PORTS`, as SuperSpeed devices. A raw-disk link was not tried, by decision: it would write to real drives while showing Windows a `VBOX HARDDISK`, and only count as a virtual machine. Nothing was written to either drive. The device rows wait for disposable physical PCs.

**A physical PC as a target.** `-PcFile` sends the rig over ssh and scp instead of VBoxManage. The rows refuse any machine that is not a VirtualBox guest or named as disposable by its own id and typed words, the rig refuses a PC file that names the machine it runs on, and a board with no id of its own cannot be named. Boot rows and the virtual disk groups stay on the VM rig. It was tried for real with the VM standing in for a PC, over OpenSSH by key: the session came up elevated, PcPrepare checked it, a deploy, a refusal to pin the disk that runs Windows, and one row through the elevated task with its evidence back and verified. That row reports a VHDX in a VM, and nothing about hardware. No physical PC has run it.

**The defrag row accepts only a real hard drive.** Windows reports a VirtualBox disk's media type as `Unspecified`. Its check stays. The level for virtual machines now means every row a VM can run, and lists that row as not run.

**T3 checks the source digest.** A clean checkout rebuilds, so it cannot check a binary's hash. `scripts/partition_manager_code_digest.py` hashes every tracked file but docs, Markdown, evidence, workflows and tool configuration, with CRLF read as LF, and T3 checks evidence against it and against the commit being in this history. Docs and the site may change after a freeze. Code may not.

**What no test here covers.**

- No row presses the panel's dialogs. They reach the same request functions, and `test_partition_manager_panel` tests what they queue.
- The consent prompt is answered by Windows policy in every row, and each report says so. No row shows that a person pressing Yes behaves the same.
- A spanned dynamic volume is refused at Apply and no row exercises it.
- Nothing is signed. A forged set of evidence, consistent with itself, would pass.
- The plain-button sweep protects the click path, and was not measured again with a real screen reader.
- The raw device length and the whole-sector read in Data Recovery have no unit seam, so only `external.file-level-data-recovery` proves them. Each was broken in the source and the row run in the VM: it failed on both. The second survived until the row's fixture got a PDF that starts off a sector boundary, inside a file that is not one.
- `scanCurrentSystem(false)`, used by the AI actions, stays strict and was not moved to the new preflight.
- Internal SATA, NVMe and SMART have run on nothing.

### Round 8: what the review capture found when it drove the app (2026-11-05)

The review capture (`observations.json`, 2026-11-04) drove the real app in the MacBook VM `Windows 11` and found
functional defects behind its pictures. Each was reproduced first in the VM with the review package (03a63bbc), then
fixed, unit-tested, drilled (the fix broken in the source, the named test red: 58 of 58) and run again in the VM with
packages built from the fixed commits (81932cc3, fa97c091, aaf96ecf). Four functional items the UI review left (F17,
F33, F39, F48) joined the round. Proof pictures are in `E:\SAK-Utility-Test-Scratch\round8\proof` on the build PC.

| Item | What was wrong | Cause | In the VM after the fix |
|---|---|---|---|
| #40 Backup Wizard | Backup Settings showed Finish, which closed the wizard with nothing backed up | Execute Backup was added only when Settings was left, so Settings was the last page | Four sample profiles backed up to D: ("Backup completed successfully", 0 errors), then restored through the restore wizard ("Restore complete", 17 files, 0 errors) |
| #40, found | Every backup ended "not everything was copied", 12 errors in the VM | Windows' hidden `Documents\My Music`, `My Pictures` and `My Videos` links counted as errors | Same run: the links named as skipped; any other link still counts (B7-20) |
| #1 Disk scan | A standard scan with five or more drive letters timed out and listed no disks | `Get-BitLockerVolume` waits about 5 s per volume before refusing a standard user | Six lettered volumes, scan done in about 5 s |
| #79 Dynamic disk | Listed last, below the fold | Not as reported: it was in every scan. Disks were listed in discovery order | Disk 1 (dynamic) right after Disk 0 |
| #80 Erase SSD | An SSD read as Unspecified and was refused | Media type joined by model name; every disk is `VBOX HARDDISK` | Media type SSD, "Ready: queue ReTrim plus clear-level wipe" |
| #77 Manage BitLocker | "Unknown until Apply" in a standard session, so Unlock could not be offered | A standard scan cannot read BitLocker | Opening it read E: with administrator rights: "Protection on", "Unlocked" |
| #41 Windows USB | "No drive letter appeared" | The build relied on automatic mounting | Reached "Extracting Windows files" |
| #3 Restore Disk Image | "Target disk size is unknown" on every disk | Disk rows carried no size | Confirmation names the image and "Target: 128.00 GB" (answered No) |
| #70 Flash | "Failed to delete mount point", stick left without its letter | Disk set offline before its volumes were locked | Raw flash of a fresh FAT32 stick with a letter: "Flash completed successfully" |
| #39 Flash | Stick left offline, eject error 21, "Speed: 0.0 KB/s" (F48) | Brought online while the writer still held it; no retry; speed baseline 0 | Same run: "Ejected (safe to remove): 1". Speed is unit-tested only |
| #62 GParted, SystemRescue | "aria2c exited with code 22" | SourceForge refuses a browser user agent; code 22 unnamed | GParted at 76%, 68 MB/s |
| #75 Download progress | 0%, no size or speed for a whole download | aria2 1.37's sizes end in "B" and were not read | "22.5 MB / 6.07 GB", 3.2 MB/s |
| #63 Browse ext | "Unable to read ext inode" | Raw volume refused an unaligned seek on a read-only handle | Root of Rig M's ext4 listed |
| #81 Recovery scan | Findings in the status bar, log empty; the live E: listed as lost | Helper output forwarded as progress; scan did not know live partitions | Findings in the log; "Partition 2 ... is not lost"; two deleted volumes found |
| #42 Check Disk | FAT32 U: "Scan failed (not supported)" | `Repair-Volume -Scan` reads NTFS and ReFS only | "Drive U: - not checked: it is FAT32 ... Run chkdsk U: (without /f)" |
| #74 Advanced Uninstall | Ticks lost when the search changed; a provisioned app listed twice unexplained | Table rebuilt without its ticks; provisioned row unlabelled | 7-Zip and VLC ticked under two searches, both in the batch |
| #60 Offline bundle | Failure only in the log; status bar stuck at 80% | Progress counted successes only | "Incomplete: 9 succeeded, 1 failed / Failed: adobereader (checksum ...)", progress gone |
| #59 Screenshot Settings | Eight blank pictures | Window pixels copied from a compositor-drawn window | 12 of 15 real, distinct pages. The first fixed run saved every page after the first as System > About (found): fixed by waiting for each page to be drawn. Installed apps, Gaming and WiFi are reported as failed pages: the VM has no Wi-Fi adapter, and the other two never left the page before within three 10 s tries (not investigated further) |
| #2 Data Recovery | Disabled with nothing selected (also Disk Benchmark, Make Bootable Media) | Needed a target it does not use | Data Recovery Source opened with nothing selected |
| #52 Migrate OS | First disk preselected even when too small | No plan check | Disk 2, 128 GB, preselected |
| #56 Disk table | Free space at the start of a disk listed last | Partitions then gaps | Rows in disk order, as the map |
| #30 File Explorer | Details spoke for Home while showing C:\SAK-Samples | Target kept when a path left it | Local Disk (C:) selected, Root C:/ |
| #18 Advanced Search | Folder tree opened expanded | Root expanded on load | Starts closed |
| #76 Advanced Search | Unread files only in the file log | Reasons not passed on | Unit-tested; the harness could not drive the search tree |
| #47, F39 SMART | "could not be determined for this drive"; Type blank or "ATA"; "N/A" and "--"; raw smartctl text | Fixed string; two markers | "PhysicalDrive0 (VBOX HARDDISK): ...", one "N/A", USB adapter named |
| #15, DF14 Hardware | Uptime 0 min, battery health 0.0%, "(0/0 slots)" | PowerShell 5.1 writes dates as `/Date(ms)/`; no health reported; the slot text went in 74ca2860 | Uptime 11 min, "health unknown, On AC power" |
| #37, F48 Port scan | Response 4021 ms with a 3000 ms timeout | Banner wait counted in the response time | 7, 4 and 2 ms with banner grab on |
| F17 Thermal | "0 degC", bars "0 / 100" for N/A | Unread sensor shown as 0 | "N/A" labels and bars |
| F33 Recovery wizard | Finish enabled with "I understand" unticked | Acknowledgement on the page before Finish | Review: Finish disabled with the box unticked, enabled once it is ticked |

**Not as reported.** #79, above.

**The capture harness.** Its privacy gate (site repo, `scripts/capture`, not committed) read a version such as
2.0.35.0 as an IP address and refused the picture. It now accepts a dotted number shown as a version and still refuses
a public address: `Test-PrivacyRules.ps1`, 17 cases under PowerShell 7 and 5.1, four drills red. Every proof picture
above passed it.

**Found on the way, still open.** The Migrate OS review page's "Blocked:" reason sits in a label too short for it and
is clipped. The restore log prints its summary twice and leaves the current-file bar at 94%. Manage BitLocker offers
Suspend and Resume together for a protected volume. Check Disk's summary says "0 could not be checked" beside a drive
it names as not checked. The Linux ISO dialog's version line still nests brackets (F49). The nightly workflow that
runs the live download check is not on the default branch, so it does not run on schedule until `dev` is merged.

**What no test here covers.** The raw volume seek, releasing a flash's handle before bringing the disk back, the
per-volume BitLocker wait and Settings' drawing are Windows behaviour with no unit seam; the VM runs are their proof.
The BitLocker read's consent prompt was answered by Windows policy in the VM (`ConsentPromptBehaviorAdmin` 0), as in
the certification rows; nobody pressed Yes. The live download check fetched the start of each fixed catalog entry
from the build PC, not a whole image. Internal SATA, NVMe, real SMART and a real Wi-Fi adapter have run on nothing.

### UI review round 2: the stress test, the re-check's regressions, and testpc1 (2026-11-05)

**The stress test.** Round 1's fix was not enough. In the capture VM a 1-minute run took 266 s to stop, a second was
still going at 300 s, and Full Suite's stress step ran 37 minutes; on testpc1 two runs were still going after 7 to 8
minutes, and the window stopped answering UI Automation. MSVC runs `std::async` jobs on the process's shared Windows
thread pool, which lets one busy thread per logical CPU run at once, and the CPU-bound stress jobs filled it. Windows
delivers incoming COM calls such as UI Automation's through that pool, and Qt reads a child process's output through
it, so the CPU temperature read that the run's clock made every 5 s never returned. Proved on this PC on two pinned
cores, without the app and without a stress run: with one spinner per CPU plus one on `std::async`, a pool job had not
started after 8 s and a QProcess round trip had not returned after 15 s; on threads of their own, 0 ms and 25 ms. Every
stress job now runs on a thread of its own (`DedicatedThreads`), the temperature is read on its own thread and gives
up when the run ends, and the clock waits on nothing but itself. The CPU benchmark's parallel pass left the pool too.
Unit tests measure the wall clock on short runs: a 6 s run ends within 2 s of 6 s while the read hangs, and Stop at
1 s ends a 1-minute run within 2 s.

**The stress test on testpc1, from 333199b2's package.** Runs ended on time: 60.7 s for 1 minute (twice) and 300.7 s
for 5. The harness's UI Automation waits still did not return until each run ended, so Stop was never pressed. A probe
in the console session found the app's window answering at once (WM_NULL 0 ms, a UIA read 0 to 3 ms), and the whole
below-normal half of the machine starving: the harness and the probe run from Task Scheduler at BelowNormal, and so
did the app they started, and a `cmd /c exit` from that class took 19 to 35 s, against 37 ms from a Normal process.
CPU-only and CPU-plus-memory runs were alike (commit 4.5 and 13.2 of 16.8 GB, no page reads), so memory was not the
cause. Sampled over ssh mid-run, the stress threads had base priority 5 and current priority 7: Windows had boosted
the below-normal threads by 2 while the app had the foreground, above every BelowNormal process at 6, the thermal
reading's own PowerShell included (every poll timed out during the run). The stress threads now run at idle
priority with boosting off. `aBelowNormalThreadKeepsRunningWhileTheStressRuns` reproduces the relation without a
foreground window: with the old priority, 150 ms of below-normal work took 5218 ms while the stress held both test
cores; at idle priority it takes its own time.

**The stress test on testpc1, from 7c5a7186's package (idle priority).** A 1-minute default run (CPU and 8.8 GB of
memory) ended 60.5 s after Start. The harness, still at BelowNormal, read "Elapsed: 00:25" through UI Automation 25 s
after Start, with no OCR fallback. A 5-minute run was stopped through the Stop button when the harness read
"Elapsed: 00:15": the app logged the run's end 16 s after its start, the harness read "Status: FAILED - Cancelled by
user" within 1 s of its click, and the window's picture shows it. The thermal reading's PowerShell, which timed out on
every poll during a run before, timed out on none. The Thermal Monitor then read CPU and GPU "N/A - not reported by this
PC" (testpc1's ACPI zone answers not supported, and it has no NVIDIA GPU) and the hottest disk 45 C. SMART read the
Seagate WARN with "Pending sectors awaiting reallocation: 16", the SK hynix PASS, and the enclosure N/A ("SMART cannot
be read through this drive's USB adapter"). Both USB disks are linked at High Speed (USB 2, 480 Mb/s) although both
are USB 3 devices (bcdUSB 3.20 and 3.00), which is why D: BENCH reads at 28.7 MB/s.

**Regressions from round 1.** R1: the disk map's legend shares the Log switch's row and the map keeps room for three
disks. R2: a size bar's label takes the colour with the higher WCAG contrast on its fill, and a mid grey that neither
text colour reads on (the lightened unallocated shade, 4.26:1) is darkened only as far as AA needs. R3: Qt's Aero back
arrow is kept hidden, and every Partition Manager wizard page has a subtitle, so all wizards title pages in one header.

**What testpc1 found.** SMART read FAIL for 16 pending sectors on a drive whose own check passed; FAIL is now only the
drive's own verdict, counts are WARN, and the Health cell says why. Thermal said a bare N/A; it now says why, reads the
thermal zone through the counters a standard user may read, no longer loses every reading when a disk refuses a
standard user, and takes a drive's temperature from the SMART scan when Windows gives none. Timed on testpc1 itself:
its USB NVMe enclosure (Best Buy NS-PCNVMEHDE) takes 12 to 17 s to answer a reliability-counter request, with
temperature 0, and its ACPI thermal zone answers WBEM_E_NOT_SUPPORTED. Every poll ran out its 10 s limit, so the
internal NVMe (36 C) and the Seagate (43 C) read N/A too. Each CIM request now has a 2 s limit; a disk out of time says
"did not answer in time"; a disk with no reading is not asked again, remembered by its unique ID (or serial), until a
disk arrives or leaves or a hardware or SMART scan runs; and "not supported" reads as not reported. The script then runs
in 4.8 to 5.9 s cold on testpc1 and 2.7 to 3.1 s once the enclosure is skipped.

**The disk benchmark on D: BENCH.** It uses no shared pool. It is slow because the thermal poll kept asking the same
USB enclosure for its counters: on testpc1 a 512 MB write-through to D: took 27.6 s alone, twice, and 376 s while a loop
asked that disk for its counters. The 3eccb7c5 package's logs show the run created its 1 GB file and finished the
sequential read, then spent 9 minutes in the sequential write until the capture closed the app. From 333199b2's
package the run on D: finished in 7 min 22 s: 54 s to create the file, 107 s for three reads, 161 s for three
write-through passes, and 120 s of timed random passes, at 28.7 MB/s read and 19.1 MB/s write. A raw baseline with
nothing else asking the disk gave the same, 28.7 and 18.9 MB/s, so nothing contends for it now. The enclosure reads far
slower than in the 3eccb7c5 pass (730 MB/s, unbuffered in both builds), so its link has changed since then.

**Functional leftovers from round 8.** Check Disk Errors counts an unsupported drive with the ones it could not check.
The restore log prints its summary once, and both profile pages finish their bars. Manage BitLocker offers only the
action that applies, and a suspended volume, which read "Protection off" and could not be managed, reads "Protection
suspended" and offers Resume. Advanced Uninstall no longer leaves a 0% bar. The Migrate OS review scrolls instead of
cutting its "Blocked:" reason. F49 was confirmed fixed by the re-check.

**Not reached this round, with what each needs.** F16 (a fixed-width symbol column per suite step), F21 (popup width
caps on the AI combos), F23 and F24 (one scrolling File Explorer details column, Details wording), F27 (one Close tone
across six dialogs), F28, F29 and F31 (a monospace block in the log window for box reports and scripts), F30, F32, F35
(empty-state text on four tables), F36 (picker start folders), F38 (one byte formatter in the duplicate summary), F40,
F41, F46; S7, S15 (contacts, Vulnerability, defrag and properties cells), S17 (three forms' label columns), S21 (File
Explorer Properties, Advanced Search results, File Organizer target), S25; N6 to N9, N12 to N18; DF10, DF11, DF12;
Screenshot Settings' Wi-Fi page on a PC with no Wi-Fi adapter, and its Installed apps and Gaming pages, which need the
VM. F18 is unchanged by round 1's decision. Round 1's untested fixes (S17 Static IP and DNS, S21 pickers, S22 finish
messages, DF7, DF8, DF9, DF14, the flasher warning title, the network icon) still have no test.

**What no test here covers.** The stress fix end to end (a 1-minute default run, Stop, and UI Automation answering
while every CPU is busy) and the thermal and SMART behaviour on real sensors were proved on testpc1 (above), not by a
test here; nor is the foreground boost itself, which needs a real foreground window. The thermal script's own
handling of a CIM time-out, "not supported" and each disk's UniqueId is checked here only as text; it was run on
testpc1 by hand (timings above), and the real-script test runs only where real hardware is allowed. The hidden Aero
arrow has no test: Qt creates it only in a real DWM window, never offscreen. The ISO save paths, the Linux ISO ready
text, the traceroute plural and the context chip's text have no test either.

### UI review round 3: recovery, Windows USB, Verify, the dark re-check, and the mediums (2026-11-05)

From review 2's capture (`observations.json`, `still_broken` and `worse`), the light and dark re-checks and round 2's
open list, on dev at 99ac494a. Commits 2bd18d45 to de6c7c2b. Every fix has a test, and every test was drilled red with
its fix broken in the source (62 drills; the capture harness's 4 below). The proofs ran on testpc1 through the window
with the capture harness.

**Partition Recovery.** The scan never wrote anything, and it did not propose the capture's write-back: that page
takes what a technician types, and the capture typed a live partition's place (1048576, 1073741824). Writing it was
already refused twice, by the queue validator and by the helper's script before New-Partition; neither had a test
that ran it, and the validator's end could wrap round on a huge typed size. What the scan reported was wrong. A boot
sector at a 1 MiB step that did not start a live partition read as a candidate, so one inside a live partition (a
disk image stored on it) or a lost volume whose size runs into a live one was listed as lost. A live BitLocker volume
was called FAT32: BitLocker writes a FAT32-shaped boot sector, FAT32 at offset 82 included, under the OEM name
-FVE-FS-. Each finding now says whether it is a live partition, inside one, running into one, or a candidate, with
the size its boot sector gives; BitLocker is named; a short read ends the scan. Proved on testpc1 (531143c5's
package) on a throwaway 4 GB VHDX with phase 1's layout: Partition 1 live under BitLocker at 1048576, 1073741824
bytes, and beside it at 1074790400 an NTFS volume deleted from the table. The scan logged "Partition 1 is at byte
offset 1048576 (BitLocker) and is not lost" and "Candidate partition boot sector at byte offset 1074790400 (NTFS,
size 1073741824 bytes)", and the live place typed into the write-back page was refused: "Not added to the queue:
Recovered partition candidate overlaps an existing partition". A first try selected testpc1's own disk 0 by a wrong
key (End moves along a table row, not down it): the read-only scan ran there, and the typed write-back was refused by
the OS-disk block and the overlap check; nothing was written.

**Windows USB, the safety check.** It ran Get-Disk in a fresh PowerShell with 5 s to answer. Timed on testpc1 against
its USB DISK 3.0 stick: 2.3 to 2.9 s idle, 4.8 to 5.9 s with every logical CPU busy at normal priority. With
99ac494a's package and that load, the build failed at once through the window with the capture's message: "Could not
verify target disk 3 is safe to erase. The target drive may be left unusable." The check now waits 30 s; when it
cannot answer it says why (no answer in time, or what Windows said) and that the drive was not changed, and the panel
warns that the drive may be unusable only once the diskpart clean has started. With 531143c5's package under a
harsher load (the app at the harness's BelowNormal against 24 Normal-priority spinners) the check ran out its 30 s and
said so, with "The target drive was not changed"; unloaded, the build ran through. The capture VM case was not run
again: the VM was free, and pre-round8 was restored with a throwaway 32 GB VDI on a VirtualBox USB storage
controller, but started headless the guest would not take the harness's 1680x900 display mode, so the harness
refused to run. pre-round8 was restored again and the VDI deleted rather than fight the VM's display; the cause was
reproduced and fixed on testpc1 instead.

Found on the way: the stick as handed over, raw-written before, was listed "[SYSTEM DRIVE]" and could not be
flashed. Every path on a volume with no file system answers ERROR_UNRECOGNIZED_VOLUME (1005, measured on testpc1),
which the system check read as "Windows may be here". Such a volume holds no Windows; access denied, not ready and
the rest still fail closed. Also, Clear-Disk did not clear that stick (it kept a whole-disk partition at offset 0);
diskpart's clean did.

**Windows USB, the layout (release blocker, fixed).** The unloaded build on testpc1 said "Flash completed
successfully" and left one NTFS partition (MBR, active) with \EFI\BOOT\BOOTX64.EFI on NTFS and a 6,868,632,137-byte
install.wim. UEFI firmware reads FAT32, so no Windows 11 PC with Secure Boot would start from it. The USB is now one
FAT32 partition on MBR, active for BIOS PCs; on a drive over 32 GB the partition is 32 GB, the most diskpart formats
FAT32 (the app has no FAT32 formatter of its own, so the cap was chosen over writing one). An install.wim of 4 GiB or
more is left out of the 7-Zip extraction, taken out of the ISO into a work folder on the PC, and split by Windows' own
DISM (/Split-Image, 3800 MB parts, /CheckIntegrity) into install.swm, install2.swm and so on; each part is hashed and
copied. An image under 4 GiB is copied as it is; an install.esd that large cannot be split and is refused. The final
check reads the drive: FAT32, \EFI\BOOT\BOOTX64.EFI, no install.wim left, and every part's size and SHA-256 (4505a84f).

The first real run failed as soon as the extraction ended, after the erase, with "CRITICAL: No Windows install image
found": the check after the extraction still asked for the install.wim the extraction now leaves out (6edd85d0).
DISM's split with the app's own arguments, by hand on testpc1 on the same ISO: exit 0 in 49 s, two parts, and DISM
/Get-ImageInfo on install.swm lists all 11 editions. Then, through the window with 6edd85d0's package on the pinned
stick (Windows 11 25H2 ISO, 7,736,125,440 bytes), the build ran in 21 minutes (format 20 s, extraction 3 min, split
and copy 16 min, read-back 2 min) and said "Flash completed successfully". Read from testpc1 after the app's eject and
a media reload, so from the stick and not the file cache: MBR; one partition at 1 MiB, type 0x0C FAT32, active, label
WINDOWS; \EFI\BOOT\BOOTX64.EFI, bootmgr, boot\BCD and setup.exe present; no install.wim and no file of 4 GiB or more;
install.swm (3,984,050,148 bytes) and install2.swm (2,839,447,229 bytes) with the SHA-256 the app logged as it wrote
them; DISM /Get-ImageInfo on install.swm lists all 11 editions.

The stick was built again with de6c7c2b's package, the final code, in 22 minutes: "Flash completed successfully",
and the same layout read back after a media reload, with install.swm and install2.swm matching the SHA-256 the app
logged for that run (DISM's parts differ between splits). It then started a PC. Hyper-V is on testpc1 now, but
Windows will not take the stick offline ("Removable media cannot be set to offline"), so it cannot be passed
through; the stick was copied byte for byte, read-only, into a throwaway VHDX of its exact size (31,042,043,904
bytes, SHA-256 of the whole stick 97164b30...). A Generation 2 VM with Secure Boot on (the MicrosoftWindows
template) and a virtual TPM, with that VHDX as its only disk, started from the MBR FAT32 layout through
\EFI\BOOT\BOOTX64.EFI into Windows 11 Setup. Through the VM's screen (the Msvm thumbnail call) and keyboard:
"Select language settings", "Select keyboard settings", "Select setup option" (Install Windows 11), "Product key"
("I don't have a product key"), and "Select Image", which lists Windows 11 Home, Home N, Home Single Language,
Education, Education N, Pro and the rest: Setup read the split image. The VM and the VHDX were then removed; Setup
installed nothing.

That run also showed two things fixed after its package. The status bar read "Ready" beside the bar for the whole
run, because the creator's steps reached only the page (54bd3241). The bar sat at 13% from the start of the
extraction to the end of the copy, about 15 minutes: the build reads 7-Zip's progress from stdout and ran it with
-bsp2, which writes it to stderr (measured on testpc1 with the bundled 7-Zip 26.05; the progress lines were also
logged as a 7-Zip error). It runs with -bsp1, and the split moves the bar per step and per part (ddeaca96). In the
de6c7c2b build both showed: "Extracting Windows files... 12%" on the page and the status bar with the bar at 19%,
then "Step 2/3: Copying install image part 1 of 2..." at 52%, and no 7-Zip error in the log.

**Verify System Files.** ScanHealth had 10 minutes; it has 30 inside the run's one 60-minute bound, and its percent
reaches the status bar as DISM prints it. A ScanHealth that runs out of time after SFC passed is incomplete, not
failed ("SFC found no integrity violations. DISM did not finish its component store scan in 30 min, so this check is
incomplete."). The capture's "S.A.K. Log stayed at Execution started with the bar at 50%" was the picture taken while
the run was still going: 50% is the panel's own bar during ScanHealth. On testpc1 through the window (6edd85d0's
package): SFC ran (2.5 to 3.5 min, no violations) and the status bar read "DISM: Scanning component store...
60.4%". The whole run then ended "Failed: Verify System Files - SFC found no integrity violations. DISM did not
complete a component store assessment." DISM's log shows it did: ScanHealth reported "The component store is repairable.", and
RestoreHealth /LimitAccess failed with 0x800f0915, "The repair content could not be found anywhere" (both re-run by
hand there with the same words). testpc1's component store is damaged and cannot be repaired without a source. The
result now says so: "DISM found damage in the component store and could not repair it from this PC. DISM said: Error:
0x800f0915 The repair content could not be found anywhere. A repair needs a source, such as Windows Update or a
Windows ISO of the same version." (de6c7c2b, tested with that PC's own DISM output). With de6c7c2b's package the
same run on testpc1 took 7.5 minutes and ended with that wording, in the log and on the status bar.

**The status bar.** A job's message ran out after 5 s while its progress bar still showed the job, which read
"Ready" at 5%. While the bar shows unfinished work, its message stays until the next one or the end of the work.

**RD1, the double alias.** `cssColor()` mapped any value equal to a light token to its palette role, so a colour
already resolved for the dark theme (#f8fafc, the dark text, is also the light surface token) was mapped again. Only a
token (`const char*`) reaches the aliases now; `cssColor(const QString&)` is deleted, and the style helpers' QString
overloads take a resolved colour as it is. The sweep, by deleting those overloads and building, found four callers:
the chips (now right), the transcript's Expand toggle (white on the user bubble had become the dark base colour), and
two that held tokens in a QString (the API key marker, the install summary heading), now passed as tokens. Every
`htmlColor()` caller passes a token. **DN1:** search highlights carry dark text, through one format both highlight
paths use. **DN3:** Flash Another sits under the details instead of at the foot of a page as tall as the tallest.

**The mediums.** test_mbox_parser's flake was the hardware guard: Qt named a temporary file
test_mbox_parser.KQSCsi, and the guard refused `\\?\C:\...KQSCsi` for containing "scsi"; device names are matched
where they start the path. Linux ISO cancel now removes the .iso and .aria2 at once, and Yes to overwrite replaces
the file. BitLocker Key Backup and Screenshot Settings show what they saved and where. A disconnected adapter's
speed, the largest 64-bit value, reads as unknown, and "Disconnected" fits. The Benchmark Drive list names a volume
by its own size and selects its first row. A killed disk benchmark leaves no file (a delete-on-close handle for the
run, and leftovers of gone processes removed at the next run). The restore page's title and both Cancel buttons
follow its end. Close Programs First names the program and says when it runs with no window. A package installer that
fails its declared checksum names the upstream package, and the offline result's status bar names the failures.
Application data paths show backslashes.

**Already fixed in round 2 (dac2667b, with tests):** Quick Partition and Map Users cells (F4, N2), Max online (F7),
the Migrate review (F8), the calendar legend in both themes (S12, DN2), the disk map (R1; RD2 not re-checked by
picture), the Linux ISO ready text (N11, DF15), the ISO save paths (S21, still untested), the traceroute plural (now
tested). **Not defects:** provisioned packages listed twice (labelled on purpose), Manage BitLocker's elevation prompt
in a standard session (round 8's design).

**Capture harness (site repo `scripts/capture`, not committed).** The privacy gate takes a dotted number as a version
on a line labelled Version, Installed, Fixed in or Affected, or beside such a label; a public address on such a line
without the label's colon, or on the next line, still fails. Test-PrivacyRules.ps1: 10 new cases, pwsh 7 and 5.1;
four drills red. The site's Windows install USB page and its shot waits now describe the FAT32 drive (working tree).

**Not reached this round, with what each needs.** The Windows USB's own read-back: on testpc1 the second part read
back in 17.5 s against 101 s for the first, so it came largely from Windows' file cache; the final check proves what
was written, not that the stick holds it (an unbuffered read, FILE_FLAG_NO_BUFFERING, or a dismount before the
read). Linux ISO "0%" at 0.3% (the bar is whole percent: a finer range or a percent label). The power plan, check
disk and verify box reports in a proportional font (F29, F31: a monospace block in the log window). Property columns
in Inspect, Properties, Defrag, Manage BitLocker and the Erase SSD method (S15, DF12, DN6, N13: a property table that
sizes its first column to its longest label and wraps values). Copy Disk's target slivers (F32) and Create
Partition's marker over its label. The Wi-Fi Password row's grey block, File Explorer's Properties and Safety text and
"Root: C:/" (F23, S21, N19), Address Book's Company column, the attachments header, VM SMART wording that differs
between runs (31). The benchmark starting on C: was not reproduced (the list now selects its first row). The re-check
leftovers round 2 listed: S7, S17, S25, the rest of S15 and S21, F16, F21, F23, F24, F27, F28, F30, F35, F36, F38,
F40, F41, F46, part of F39 and F34, N6 to N9, N12 to N18, DF10, DF11, D2, DF2, DF5, DF16, DN4, DN5, DN7, DN8, and
Settings' screenshot item (#59).

**What no test here covers.** The Verify step's streaming through runProcessStreaming (the tests replace the step);
the panel's "The target drive was not changed" after a failed check (a GUI lambda); Close Programs' window check
(EnumWindows on a real desktop); the offline result's status bar text; the delete-on-close file under a real kill
(the test closes the handle as a kill does). For the Windows USB: the FAT32 check and the UEFI file in the final
verification, 7-Zip leaving install.wim out and writing its progress to stdout, DISM's split, and the drive booting
run only on a real drive and a real 7-Zip and DISM; they were proved on testpc1 (above), not by a test here.

**Left behind.** testpc1: Hyper-V on (the owner allowed it), no VM; the recovery rig VHDX, the stick image, the
round's scripts and the copied ISO deleted; the console at 1024x768; the USB DISK 3.0 stick holds the Windows 11
FAT32 install media de6c7c2b built; de6c7c2b's package in C:\SAK. Its component store is damaged (above) and was
left as found. The capture VM is powered off at pre-round8, its throwaway VDI deleted.

### UI review round 4: read-back from the media, the property table, and round 3's open list (2026-11-06)

From round 3's open list and review 2's findings, on dev at 87c04b54. Every fix has a test; every test was
drilled red with its fix broken in the source: 53 drills, 50 planted by a script that restores every byte and
touches the file, and three inside test_partition_image_volume_lock (a buffered open, an unaligned start and the
file-tail length). Two survived the first run and their tests were tightened, then went red: the Full Suite's mark
column (the offscreen platform has no fonts of its own, so the four marks came out one width; the test now
requires one fixed width with room for the widest), and the calendar sidebar's stretch (at 700 px there was no
spare height, and with some the labels grew instead of the gaps; the test now measures a taller dialog from top to
top). Judgement calls and items that were not defects are named below.

**Read-back from the media (release item).** A check that reads back what was just written proves nothing about
the drive when Windows answers the read from the pages it still holds from the write. Measured read-only on
testpc1's USB DISK 3.0 stick (pinned by model, serial and size 31042043904): install2.swm, 2,839,447,229
bytes on its FAT32 volume, read with an ordinary buffered handle took 44.3 s and then 0.5 s (5.4 GB/s, the cache),
and with FILE_FLAG_NO_BUFFERING 44.2 s (61.3 MB/s, the stick). The raw device \\.\PhysicalDrive3 read 1 GiB in 16.2 s
buffered, twice, and 16.2 s unbuffered: Windows does not cache a raw disk handle. So the paths at risk were the ones
that read a file back:
- the Windows USB's final check of each install.swm part,
- the backup's check of each stored file (plain copy and codec container),
- the restore's check of each restored file,
- the clone and image read-back when the target is a file (Create Image).
They now read through one reader that opens the file unbuffered (`sak::MediaReadBackFile`, `sak::sha256FromMedia`
in sak_foundation; `Sak.MediaReadStream` in the clone/image script). Such reads must start on a sector boundary, be
a whole number of sectors long and land in a sector-aligned buffer: both read whole 4 MiB chunks from a chunk
boundary into a VirtualAlloc buffer, a file asking for the whole chunk (the read past its end comes back short), a
disk only as far as its end. Raw image flashing (Linux, Clonezilla and Ventoy media included) already opened the
device with FILE_FLAG_NO_BUFFERING and verifies through that handle; Restore Image and Copy Disk to a raw disk read
the disk while it is held offline, so nothing was cached there either; they use the new reader too.
Tests: a recorder in the test binary's import table (`tests/support/create_file_recorder.h`) sees the flags each
CreateFileW call passed, and the Windows USB check, the backup's two checks, the restore's check and the flasher's
device open must each open with FILE_FLAG_NO_BUFFERING; test_media_read_back reads every byte back at sizes either
side of a sector and a chunk, seeks, and refuses writes; the clone script's stream is compiled and run on a real
file by test_partition_image_volume_lock, which asks Windows for the handle's mode (FILE_NO_INTERMEDIATE_BUFFERING)
and drills the open, the chunk alignment and the file-tail length.
Then the whole build through the window on testpc1, with a package of this round's read-back code (SHA-256
6267c214...32a575fa) on the pinned stick and the Windows 11 25H2 ISO (7,736,125,440 bytes): format 22 s, extraction,
split and copy 18 min, and the final check passed, "SUCCESS: ALL FINAL VERIFICATIONS PASSED". Straight after the copy
it read install.swm (3,984,050,148 bytes) back in 98 s and install2.swm (2,839,447,229 bytes) in 67 s (40.7 and 42.4
MB/s); round 3's buffered check had read the second part in 17.5 s. After the app's eject and a media reload
(IOCTL_STORAGE_LOAD_MEDIA), Get-FileHash on the stick gave the same SHA-256 the app logged for both parts
(18c25a16... and 48472366...), with BOOTX64.EFI, bootmgr, boot\BCD and setup.exe present and no install.wim. The
cost: the unbuffered check takes about 45 s longer on this stick than a cold buffered read of the same parts (70 s
and 50 s), most likely because it waits on each 4 MiB read in turn, with no read-ahead from Windows (not measured
further).

**The property table (S15, DF12, DN6, N13).** Inspect, Properties, Disk Defrag, Manage BitLocker and Erase SSD all
build their table in createPropertiesTable. Its label column now takes its longest label, values wrap and each row
grows to what it holds, and a value's tooltip holds it whole. Tested in the four dialog tests that open them (a
label column narrower than its labels, or a row shorter than its wrapped value, fails).

**The log window (F29, F31).** A message's first line is the message; the lines under it are what a program wrote
(the power plan and Check Disk reports drawn with | and =, a dry run's script). They go in a block of their own in
Consolas, which every Windows has, so asking for it never sends Qt through the font alias table, and the block does
not wrap; a line wider than the window scrolls sideways. A refill and a line added live lay out the same. Quick
action lines reached the Log as "[06:51:00] [2026-11-05T06:51:00] Optimize Power Settings: ..."; the panel stamps
the time, so the run's line no longer carries a second one (quick_actions.log keeps its date and time).

**Fixed, each with a test.** Linux ISO download: the bar reads tenths below 1% ("0.3%"), rounded down like the whole
percent. Partition table: fitting the columns to their contents took the stretch off the last one, so the header
stopped at "Status" (F30); the four tables that fit their columns stretch the last one again. Copy Disk and Apply
bars draw no label in a segment too narrow for three characters of it (F32), and Create Partition's size handle no
longer splits its label: the label moves to the widest stretch beside the handles. Wi-Fi Manager: the Password row
kept a layout's own margins and stood 10 px in from the other fields with its label above them. File Explorer:
Properties, Safety and Evidence are labels scrolled together by the details scroller, not three text boxes each
scrolling on its own (F23); with nothing selected the preview says "No file selected. Select one to see it here."
(F24); a Windows volume's Root, Path and Safety Identity read with backslashes, an ext, HFS+ or APFS path stays
its own (S21); the
Hashes tab's label column has the large spacing (N15). Advanced Search: a search with no matches sent the status
bar (0, 0), the busy indicator, and left it running (N6); result paths read with backslashes (S21). File Organizer's
starting target reads with backslashes (N19). Address Book: fixed widths that added up to more than the list's pane
put Company behind a scroll bar; the columns take what they hold. Duplicate summary: one byte formatter for the
total and the groups (F38). Benchmarks: measurements in the body weight and each group's score in the score style
(F40). Full Suite: each step's mark sits in a column of one width, so the names start at one place (F16). Forms:
the diagnostics report's Notes box, the OST converter's three filter fields and the Windows ISO dialog's two groups
each share one label column (S17). Empty lists say what goes in them: Wi-Fi Manager's saved networks, the Windows
ISO builds, Pending Operations, and the backup wizard's Wi-Fi page before and after a scan that finds none (F35).
AI: the workflow combo's tooltip names the chosen workflow whole (DF11); a workflow's name is an 11 pt heading in the
290 px pane, not 13 pt (N17). Advanced Uninstall: commands, keys and package names are read-only one-line fields,
whole and copyable (N7); the Uninstall button is the danger tone like Force and Batch Uninstall, and a word joiner
keeps "Notepad++" whole in the bold name (N8); a Store package's publisher is its certificate's organization or
common name, and a Store-signed ID stays as it is (N9); the list has a key for its orange and red names, in the words
of their tooltips (DF10). Calendar: today's week header is the primary dark blue in light (4.7:1) and the dark
theme's text colour in dark, on today's fill (DF2). SMART: a drive smartctl could not read and one it read with
nothing to judge by both say "<drive>: SMART health could not be determined." and then why (observation 31).

**Not defects, with the reason.** N4, the attachments header: fixed in round 2 with a test (everyHeaderFitsItsColumn).
DF5, "Free" at 4.19:1: round 2's readableSegmentFill darkens the fill to AA, tested in both themes. N12, Media type
"SSD" in Erase SSD and "Unspecified" in Properties and Defrag: all three print disk.media_type; the capture had set
the rig non-rotational for the Erase SSD shot. N18, "8 GB threshold": the warning and Settings read one config key;
the capture had lowered it to 8 GB for that shot. DN7: the legend's Unallocated swatch is filled with the map's free
shade, #707d8d, 4.27:1 on the dark page and 3.84:1 on the light one; the 2.12:1 was the base colour, which the
swatch no longer uses. F21: a combo's popup reaching past its window is how Windows' own combos behave; Qt keeps it
on the screen. S7 and DN8: round 1 (bce065ab) made the explorer's selection the one shade and removed the theme's
selection paint under it; the darker reading was a selected row under the pointer, not reproduced here.

**Not reached this round, with what each needs.** F27 (one Close tone across six dialogs: a decision on which tone
Close takes, then six call sites). F41 (Help and Image Flasher cards centre their titles, Backup's left-align: one
card builder). F46 (the Email Tools folders pane changes width between states: the splitter's sizes saved and
restored). F39's rest (SMART's Type column reads N/A where the inventory reads ATA: the SMART report needs the bus
type the inventory has). F34's title indent (backup wizards x=23, Partition Manager wizards x=12). S25 and DN4 (tab
icons faint on the tab colour, and fragments of tabs under the scroll arrows: icon assets with a stroke that
reaches 3:1 on both tab colours). DN5 (the Linux card's penguin is black on the dark card: a dark variant of the
icon). N14 (a VM's battery reads "1 - health unknown"; SMART lists VBOX HARDDISK four times without drive
numbers). N16 (calendar chips show only the time and an ellipsis: the chip painter puts the subject first). The
BitLocker key folder picker is left with no starting folder on purpose (the system drive is usually the volume the
keys unlock); Windows opens it where the app runs, which for a portable tool is the technician's drive. Screenshot
Settings' Installed apps and Gaming pages, and the Wi-Fi page on a PC with no Wi-Fi adapter (#59), need the VM,
which this round did not use. DF16 needs nothing more: round 2 hid Qt's Aero back arrow in every wizard, so neither
theme draws one.

**T2 caught one regression.** The first push of this round (T2 42214861764) failed the accessibility audit in two
shards. File Explorer's three detail sections, labels now, had kept the accessible names their text boxes had, and
a label's name is what a screen reader says in place of its text ("hides its text", three findings). They carry
those words as descriptions now. test_file_management_explorer_panel audits each section, and
test_accessibility_audit's whole-window audit passes; this round's local test list had left it out. The audit's
printout also showed the Safety pane's "Identity: C:/Users/Username" line, S21 again; it reads with backslashes now. Both
are drilled red.

**What no test here covers.** That a real drive's slowness shows: the tests prove each check opens its file with
FILE_FLAG_NO_BUFFERING and reads every byte, and the timing on the stick was measured on testpc1 (above). The
partition dialogs' layouts were checked offscreen at the test's window size, not on every DPI. The log's fixed-width
block was checked by its formats, not by a picture. Three call sites have no test of their own: the Linux ISO
dialog putting percentText() on its bar (the function is tested), the Store package scan calling
publisherFromSubject() (the function is tested), and the File Explorer target Properties box's Root line.

**Left behind.** testpc1: the USB DISK 3.0 stick holds the Windows 11 FAT32 install media this round built, its
media loaded again after the app's eject; this round's read-back package in C:\SAK; the Windows 11 25H2 ISO in
C:\SakIso, kept on purpose for the final capture and the VM smoke test (a read-only copy of the offload drive's
installer, which the coordinator approved); the console back at 1024x768; this round's scripts, scheduled task and
display state file removed. The capture VM was not used.

### UI review round 5: the last open items, Screenshot Settings' pages, and the flake hunt (2026-11-06)

From round 4's open list, on dev at c6402fc9. Every fix has a test, and every test was drilled red with its fix
broken in the source: 44 guards, each planted by a script that restores every byte and touches the file. The first
30 were planted one per test function; Screenshot Settings' 14 new ones and its 9 earlier ones ran again one at a
time on the final code, each with a build of its own. One earlier drill guarded the wait this round replaced, and
went with it. Two drills survived their first run: one mutation of the Image Flasher card hit the wrong label,
and was replaced by one that undoes the fix; and putting back the old File Management drawing passed the dark-tab check,
whose share at 3:1 its bright boxes carried, so the test now also fails an icon with a fifth of its ink under 1.5:1.
Both then went red. Judgement calls and items that were not defects are named below.

**Screenshot Settings (#59).** Two of its fifteen Settings links were not pages. Opened on testpc1 (Windows 11 25H2,
build 26200) the way the app opens them, `ms-settings:apps-features` and `ms-settings:gaming` each left System >
About on screen, which is why in the capture VM Installed apps and Gaming "never left the page before" in three
10 s tries apiece; `appsfeatures`, `installed-apps`, `gaming-gamemode` and `gaming-gamebar` opened their pages. The
capture VM runs the same build, and no Windows 10 PC is at hand, so the links are held against Microsoft's
published ms-settings list ("Launch Windows Settings", learn.microsoft.com, read 2026-11-06), which gives these
fifteen pages one name on Windows 10 and 11: each page has its published name first and, where Windows 11 also
answers to another, that name next (Network status: network-status, network; Installed apps: appsfeatures,
installed-apps; Gaming: gaming-gamemode, gaming-gamebar; Privacy: privacy, privacy-general). Gaming captures Game
Mode, the gaming setting that changes how the PC runs. The list says Settings has a Wi-Fi page only with a Wi-Fi
adapter: the run lists the adapters first (the GetAdaptersAddresses call Network Adapters uses) and, with none,
leaves that page out as "this PC has no Wi-Fi adapter". Pages a PC does not have are named in the result, the log
and the report (NOT ON THIS PC), and are not failures. "1 monitors detected" reads "1 monitor detected", and the
report names its folder with backslashes.

The pictures were wrong as well, which the run's own report did not show. In the capture VM, with this round's
first packages, the report read 14 of 14 saved, and opened one by one, two pictures were of their page
(Personalization and Privacy): About was saved as Game Bar, Activation as About, Display as Activation half drawn,
Power and sleep as Activation, Storage as Display, Sound as Storage, Notifications as Sound, Network status half
drawn, Ethernet as Network status, Installed apps as Ethernet, Windows Update as a loading ring and Game Mode as its
title over an empty page. Settings opens every link in its one window, and the wait took a picture as drawn once it
was not the last picture saved and was the same 0.3 s later. Measured with a probe that opened every page the way
the app does and saved every picture that changed (testpc1 and the capture VM, 2026-11-06): after a link About stayed 0.4 to 1.2 s on testpc1 and 5.8 to 19.4 s in the VM; a page drawn half
way held at most 0.65 s on testpc1, and in the VM Installed apps held its title over an empty page for 3.83 s, then
six of its 32 apps for 3.62 s. So a half-drawn page was saved, the page went on drawing, and the next page's wait
took the rest as the next page; and the first page had nothing before it to compare against. What else held 3 s or
more in the VM was the page drawn (Storage with a loading ring on one row, Personalization before its wallpaper
preview loaded, Privacy 17 px short of the end of its slide-in). Every page differed from About in 21 to 45 % of the
window; About differed from itself by at most 4.8 % while it slid in, and by under 0.9 % from one window to the
next; Settings exited each time its window was closed, on both machines.

So each page is now opened in a Settings window of its own, on About (the window before is closed first, with
WM_CLOSE). About is learned at the start from two windows of their own that settle on the same picture, and every
later window must settle on that About. The page's link is then opened in that window and compared against the
About it showed when the link was opened: the picture is saved once it differs from that About in more than a tenth
of the window and has held still for 8 s, twice the longest half-drawn hold measured (About itself, once known,
needs 2 s).
A link that leaves About on screen for the whole 60 s wait in all three tries is a page this Windows does not have;
the next name is tried, and a page none of whose names opens is reported as "this page does not exist on this
Windows". A blank window, or a page that never holds still, is a failed try. Nothing is saved for either. The run
does not start while a Settings window is open ("Settings is open: close it, then run Screenshot Settings again"),
so it never navigates or closes one the user had open; if the two About windows never agree, it stops before any
page ("Settings did not draw its About page alike twice"). A run took about four minutes on testpc1 and fifteen in
the VM.

Proved with package bb0c56e3 (this round's code at 1b74bca1), through the window, every saved picture opened and
looked at. On testpc1 (a Wi-Fi adapter): 15 of 15 saved in about four minutes, the result read "Captured 15/15
settings pages (1 monitor detected)", and all 15 pictures show the page their name says, drawn in full (Ethernet as
Settings opens it, scrolled to its adapter's details). In the capture VM (no Wi-Fi adapter), restored from
pre-round8: 14 of 14 saved in 14 min 51 s, the report's NOT ON THIS PC section read "WiFi_Settings: this PC has no
Wi-Fi adapter", and all 14 pictures show the page their name says. Thirteen are drawn in full; Personalization's
preview still shows the stand-in wallpaper Settings draws before it loads the PC's own (the probe saw it change
after 5.4 s; in this run it held past 8 s). Storage took 4 min 46 s against about half a minute for most pages: in
the VM its Temporary files row computes for over a minute, and the picture saved shows it computed.

Found on the way: Settings opened from a task with no foreground window (the probe's scheduled task) leaves its
page out of its frame until it is brought to the front; the app is the foreground process when a person starts a
run and brings Settings to the front itself, so this is the probe's condition, not the app's, and it made the
probe's new windows slower to appear (17 to 22 s in the VM) than the app's. The VM's display took 1600x900 for
this round's earlier runs and 1680x900 for the final one, which ran through the whole harness. The harness's
20-minute wait in the guest ran past its time (the VM's timers run slower than its clock under load), so the
pictures and the report were read from the run folder once the app had written them.

**Fixed, each with a test.** F27, one tone per role: the theme makes every push button primary, so a dialog
button box's Close or Cancel, and a message box's No or Cancel, came out in the colour of the action beside them.
A button whose role only leaves a dialog (reject or no) now takes the secondary tone when it is polished, unless
it carries a style of its own (a Cancel that stops running work is the danger tone, and keeps it); a wizard's
Back and Cancel are secondary, Next and Finish primary; the Installation Summary's and the backup filter
details' Close are secondary. The judgement call: Close, Cancel and No only leave, so they are secondary
everywhere, and a dialog whose one button is Close has no primary button, as in Windows' own dialogs; the
primary tone marks the one button that does the work. F41: Help, Backup and Restore and the Image Flasher build
their cards with the one card builder: a 48 px logo with the title and description centred under it (two of the
three layouts already were). F46: Email Tools' item count ("4 search result(s) (filtered)") sat beside the
paging controls and set the Items pane's narrowest width, so the Folders pane moved between 293 and 240 px; it
has a line of its own and wraps, and no text it shows moves the splitters. F39 and N14: the SMART table names
each drive by its number and model ("PhysicalDrive0 (VBOX HARDDISK)"), the one naming the messages under it
already used; a drive smartctl could not read takes its model and bus from the hardware scan; Type is the bus
as the hardware scan names it, so the two views agree; and every warning starts with the drive it is about
("High number of unsafe shutdowns: 630" did not say which). The Battery row leaves out a name with no letter
in it (VirtualBox's battery is "1") and says "Health not reported" for a battery that gives no capacities. N16:
calendar chips put the subject first and the start time after it ("Oven repair visit, 8:00 AM"), one label for
the month and week views. S25 and DN4: the File Management tab icon's lines were a quarter of a pixel at the
tab's 20 px; it is redrawn on the 20 px grid with 2 px lines and 1 px box outlines, in a dark slate for the light
tab and a light grey and blue drawing for the dark one; Email Tools' "@" is a darker blue in light and keeps its
light drawing in dark. Selected, the white tab icon leaves out the parts drawn in white or near it, so Help's
question mark and About's "i" show in the tab's blue instead of making blank discs. DN5: the Linux card's
penguin is drawn in slate grey in the dark theme. Icons drawn for a theme follow a change of theme at once (the
tab bar and the cards). Thermal: see the flake hunt.

**Already fixed, and not defects.** F34's title indent (backup wizard titles at x=23, Partition Manager's at
x=12): fixed in round 2 with R3. A QWizard page with a subtitle titles itself in the header and one without titles
itself in the page, at another indent; every Partition Manager wizard page has had a subtitle since round 2
(wizardsTitleEveryPageInTheHeader), and every backup and restore page sets one, so all wizards title their pages
in one place. Review 2's pictures were taken before that round. S25's fragments of tabs under the scroll arrows:
not a defect. With eleven tabs and a 1280 px window the tab strip scrolls, and a strip that scrolls shows the tab
it would scroll to cut at the arrow, as Windows Terminal's and Edge's do; the cut label is what says there is
more that way. The tab icons themselves (S25, DN4) are fixed above. On the light tab (#cfd8e3) most tab icons are
drawn in mid tones that stand under 3:1 from it but read as shapes beside their tab's name; only File Management
and Email Tools were faint, and those two are held to half their ink at 3:1 and under a fifth below 1.5:1, as
every icon is on the dark tab.

**The flake hunt.** Six runs of the T2 labels (312 tests, -j 6) on c6402fc9 before any change: every run green,
no test failed once, test_mbox_parser included (round 3's fix holds). thermalPollsOnlyWhileShown did not fail in
those runs; reading it found why it failed once in round 4: ThermalMonitor::isRunning() was the timer or the poll's
watcher, and between a poll finishing on the thread pool and its result reaching the GUI thread neither runs, so a
started monitor read as stopped when the check right after show() lost the race. isRunning() is now started and not
stopped, or stopped with its last poll still landing; the test waits for the pool without the event loop, so that
moment is checked every run (with the old isRunning() it fails every time). After the fixes, four more
runs at cd5378bd: all green. Ten runs in all, 312 tests each (two skipped in every run, test_startup_budget and
test_single_instance_app, which start the app), and no test failed once. The run before the push, with Screenshot
Settings' fix (1b74bca1), passed the same way.

**What no test here covers.** Screenshot Settings' run itself (opening and closing each Settings window, the wait
on the real window, the Wi-Fi adapter list and leaving out the WiFi page) is Windows behaviour with no unit seam:
the decisions on what the window showed (the watch, the picture comparison, the About agreement), the page list,
the outcome lists, the message, log and report are tested, the order of the steps (close, About, the link) is held
by reading the source, and the run was proved in the capture VM and on testpc1 (above). The settle and wait times
rest on one day's measurement on two machines; a PC slower than the capture VM could still take a page drawn half
way for drawn. The month and week painters each draw the chip label through the
tested eventChipLabel(); no picture of them is read. The dialog tone policy is tested on a button box, a message box
and a wizard of the test's own, not on every dialog in the app. The tab icons were read offscreen at 20 px, not on
every DPI. The accessibility runtime audit (the app started with --accessibility-audit) was left to T2: this round
started no app on the workstation; test_accessibility_audit's whole-window audit passed in every local run.

**Left behind.** testpc1: this round's package (bb0c56e3) in C:\SAK, as the capture harness installs it, with its
copy in C:\SAKCapture\pkg and the run's output in C:\SAKCapture\out; C:\SakIso and C:\SAKCapture with its two tasks,
kept; the console back at 1024x768; this round's scripts, probe pictures, scheduled tasks, display state file and
C:\SAK-Samples\Settings removed. The capture VM: powered off and restored to pre-round8; the Mac keeps the
harness's copy of the package in ~/sak-capture/pkg, as every run leaves it. On the workstation: the package under
E:\SAK-Utility-Test-Scratch\round5, and the probe timelines and every picture both runs saved in its fix59 folder.

### UI review round 6: the release-candidate capture, its four Highs, and the reviewer's re-check (2026-11-06)

From the release-candidate capture of 090cabe0 (review3 `observations.json`) and the independent re-check of the same
pictures (`ui-review\recheck3.md`), on dev at 090cabe0. Every fix has a test, and every test was drilled red with
its fix broken in the source: 45 guards, each planted by a script that restores every byte and touches the file,
one at a time with a build of the test's own target and the whole test binary run through the no-dialogs wrapper.
One survived its first run: taking out the Aero arrow's immediate hide() changed nothing a test sees, because the
event filter installed beside it hides the arrow at every show; it was replaced by putting back round 2's
inherits("QVistaBackButton") check, which went red. Judgement calls and items that were not defects are named
below.

**The false "Restore failed" (High).** All 1198 files came back, then "netsh interface ip set address ... failed for
adapter 'Ethernet': Contained process descendants remained after primary exit", and the page read "Restore failed",
"Restore Stopped" and "The restore did not finish." Measured on testpc1 with a probe that starts console programs the
way the app does (no console, so CREATE_NO_WINDOW, in a kill-on-close Job assigned while suspended): the only process
netsh (and cmd, whoami) leaves in its Job is conhost.exe, the console host Windows starts for it, and it leaves 0 to
20 ms after the program, unloaded, with every CPU busy, and with the Job capped at 0.5% CPU. The capture VM ran at
macOS CPU_Speed_Limit 20 with the app at BelowNormal, where round 2 measured a BelowNormal process taking 19 to 35 s
to exit; a console host that missed the 1 s grace there turned a netsh call that had done its work into a failure
(the adapter ended as Manual 10.0.2.15/24, as it was asked to be). The starvation itself was not reproduced on
testpc1, and `netsh interface ip set address` was not run anywhere: changing a live adapter is forbidden on every
machine. The rule applies to every runProcess and runPowerShell caller that starts a console program, which is all of
them, so it is fixed in the runner: when a run's grace ends with only System32\conhost.exe left in its Job, the hosts
are stopped and the run is judged on the program alone (`onlyConsoleHostsRemain`, `WindowsProcessJob::
memberImagePaths`, which lists only live members). A process Windows is still tearing down stays in the Job's list
with its exit code and time set before it is signaled, and refuses its image name (measured here on the Job test's
own family: exit code 0, WAIT_TIMEOUT, ERROR_ACCESS_DENIED); it counts as exited, so a netsh in that state cannot
turn the console-host rule back into a failure. Any other leftover still fails the run, as before. The restore then
reports per step: network settings are a line of their own in the summary ("Network settings applied: 0 of 1", "Not
applied: Ethernet settings for 'Ethernet'") instead of file errors, the summary's first line is "Restore finished, but
not everything was restored." when anything was not, the log gives netsh's own words from standard output, and a
restore that ran to its end says "Restore Finished with Problems" with what was not done ("1 network setting was not
applied"), the bars full; only a cancelled or unreadable restore says "Restore Stopped" (RG3).

**The recovery pre-fill (High).** Not a defect of the app: the Candidate Restore fields start empty, and the
capture's shot file typed 1048576 and 1073741824 (`wiki/shots/partition-manager.json`), as round 3 found. What was
wrong is that the wizard took that place to a review saying Apply would create it; only Finish, through the queue's
validator, refused it. The page now runs the queue's own check (`PartitionSafetyValidator::validate` on the operation
Finish would queue, against the inventory the wizard opened on) as each field changes, says in red why a place
cannot be written back, and keeps Next off. Proved on testpc1 with Rig A built on the disposable Seagate (pinned by
model, serial and size 8001563221504): MSR at 17408, E: 3 GB at 16 MiB, F: 2 GB at 3238002688, the VM's
layout exactly. With this round's package, at 1048576 for 1073741824 the page read "This candidate cannot be written
back: Recovered partition candidate overlaps an existing partition." in red with Next off, in light and dark; at
6442450944 for 1073741824, in the free space after F:, it took the place and its Review read "Write-back candidate:
offset 6442450944, size 1073741824". Only Disk 2 has that place free, so the pictures also show which disk the
wizard read. Finish was not pressed. The shot files now type a place in Rig A's free space (6442450944, 1 GiB).

**Convert Dynamic Disk to Basic on a basic disk (High).** Offered in the ribbon and the disk menu for any disk, and on
a basic MBR disk with one volume its window went on as if the volume were dynamic. Had the window been accepted, the
operation was refused at the queue ("Dynamic-to-basic conversion requires one mounted simple dynamic volume"), and
had it reached the helper, its script refuses a disk that is not dynamic ("Disk N is not a dynamic disk now") before
it backs anything up, deletes a volume or runs diskpart; nothing would have changed. The ribbon button and the menu
entry are grayed out for a basic disk with the reason as their tip, and the action itself warns instead of opening.
Proved on testpc1 with Rig D, the disposable USB DISK 3.0 stick (pinned by serial and size 31042043904)
as Disk 3, basic MBR with one NTFS volume: with Disk 3 picked, its menu shows Convert Dynamic Disk to Basic grayed
out. Nothing was converted. Windows does not make a USB disk dynamic, so a dynamic disk's case is the unit test's
and the capture VM's.

**The light back arrow (High).** Round 2 hid Qt's Aero back arrow by asking each child whether it inherits
"QVistaBackButton". That class has no Q_OBJECT, so its meta-object is QAbstractButton's and the question is always
false: the hider never matched anything, in either theme. Light draws the arrow and dark does not because Qt starts
a wizard in AeroStyle only when Windows is in light mode (QWindowsStyle's SH_WizardStyle), marks the arrow to be shown
then, and setting ModernStyle before the window exists does not take that back. Proved on testpc1 with a small Qt
6.12 program in Technician's session (light Windows): the arrow is a direct-child QAbstractButton, not hidden right
after construction, visible after show with the round 2 code, and gone when the wizard's one direct-child button whose
meta-object is QAbstractButton's own is hidden. `test_app_theme` reproduces it offscreen with Qt's Windows style.
With this round's package on testpc1, in light and in dark, the backup wizard's Select Users page, the restore
wizard's first page and the Partition Recovery pages draw no arrow; the reviewer's measure (pixels within 12 of
#a3a3a3 in x 0-40, y 25-70) finds 20 in review 3's light pictures of the first two and 0 in these.

**RG1, Offline Deploy 2200 px wide (regression from round 3).** The failure label named the package and its download
URL unwrapped. It wraps in the width the page has (size policy Ignored), with the whole text as its tooltip.
**adobereader**: not broken upstream. Its installers, downloaded from the package's own URLs, match the SHA-256 its
script declares (x64 6FF26E9D..., x86 D5ADBDD1...). The script assigns `$MUIchecksum` and passes `$MUIChecksum`;
PowerShell ignores the case and the parser did not, so the checksum compared was the text "$MUIChecksum" and every
bundle refused the installer as altered. Variable lookup ignores case now, and a checksum still naming a variable is
reported as one S.A.K. could not read, not as the vendor's change. It stays in the default lists.

**Mediums.**
- Compare Panes in the palette: not as reported. In the failing tries dual pane was off (the probe pictures show one
  pane and no "Active pane" line): the app reopens its last session's tabs and panes, and the shot's Dual Pane click
  is a toggle, so on a second run it closed the second pane; Compare Panes is rightly unavailable then. Fixed what was
  wrong: the palette said "There are no commands containing compare" for a command that exists; a match that cannot
  run is now listed greyed with what it needs ("Compare Panes - Enable dual pane first."), and Dual Pane shows a check
  while two panes are open. The shot needs say so.
- System Inventory Report on the VM: no wait that never ends was found. The window had no quick-action bar and no
  dialog open, so the run never started: the folder picker returned no folder, and that was said only in the Log. It
  is said in the status bar too. The collectors keep their own limits (60 s, 10 s, 10 s). The VM case was not run
  again.
- A Partition Manager scan that finds no disks and gives warnings says so on the disk map, the table and the status
  bar ("The disk scan found no disks: Process timed out. Click Scan Disks to try again.").
- Public Release: filtered to the builds UUP dump titles "Windows 11, version NNHN (build)"; read from listid.php on
  2026-11-06, the first five entries were Insider Preview builds. Edition starts on Pro, else Home, else on nothing
  (X18).
- Notepad++ leftovers: Notepad++ records no InstallLocation and calls itself "Notepad++ (64-bit x64)" (read on
  testpc1), so `%APPDATA%\Notepad++`, which its uninstaller keeps when settings are kept, matched no name and the
  list stayed hidden. The folder its uninstaller or icon sits in names the program when that folder's name ties to
  the display name. Not run again on testpc1.
- The log: every line is flushed as it is written, not only errors.
- Bus types (also X1): Storage Health takes the bus from MSFT_PhysicalDisk.BusType by disk number, and
  Win32_DiskDrive's own InterfaceType only when that has none; the model-name guess that typed the USB enclosure
  as NVMe is gone, and MediaType comes from the same row. Erase SSD calls an SSD behind USB "SSD behind a USB adapter".
  The PowerShell queries themselves were not run here (the hardware guard); the mapping is tested.
- Windows USB: the wait for the new partition is 3 minutes, each look has 10 s, the page shows the time waited, and
  the error says to replug and try another port or drive. Not run again on the VM.
- Stress test: a stop reads "STOPPED - Cancelled by user" in the panel, the log and the report (a warning, not a
  critical issue); every finished run shows its own length ("Elapsed: 01:00") and ends the status-bar bar.
- The Log switch is a check box named "Log" for screen readers and UI Automation, checked while the log shows.

**Lows and the reviewer's items fixed.** Bars: Apply review and Copy Disk draw no segment narrower than 24 px (X5).
Backslashes in File Explorer's dual-pane status, Advanced Search's preview and the disk benchmark's log lines. "(0
bytes in 169s)" reads "(169s)". The backup summary's "Files skipped" was true as a count but counts filtered folders
and Windows' compatibility links too: it reads "Items skipped (filters and Windows links)". Run Details said "no
tool/provider health records" above provider lines: "no tool health records yet" (X8). Resize/Move's empty Status row
is hidden (X4, first half). Email Inspector's Items pane starts at 480 px, not 400 (RG2). Download & Build asks UUP
dump for the file list once more, 3 s later, after HTTP 400, 429 or 5xx, and not when Cancel comes in that wait.

**Not defects.** The recovery pre-fill (above). Provisioned packages listed twice (labelled on purpose) and VirtualBox's
"Unspecified" media type stay as decided.

**Not reached this round, with what each needs.** SMART naming a USB-bridged drive only "PhysicalDriveN" when no
hardware scan has run (the SMART run reading Win32_DiskDrive's models itself). Small cuts: Advanced Search's status
bar, the AI status "Context: pe", the Address Book's Email column, Map Users' "Merge Into Destinati" (N2), adapter
Status "Disconnect..." (`network_diagnostic_panel.cpp` measures before its style sheet applies), the vulnerability
table's last column (F18), long Path lines in File Explorer's Properties (F23: they need to wrap), Resize/Move's
Support line clipped at its descenders (X4, second half), and Copy Disk's target copy, now 24 px but still without
room for its label; the Partition Manager table and map fold (R1). The reviewer's X2 (disabled AI icons), X6, X7, X9
to X17, X19 to X21, S7/DN8 (the file list's own selection shade), S22 (stale status messages, including the raw
flash's "Ready" beside 95%), S24 plurals, S25's faint light tab icons, F21, F27's AI Close, F36, F38, F40, N6, N9 in the
backup wizard, N17 and DF11. The help_smartscreen picture shows "Unknown publisher": the capture package was unsigned,
so the picture is stale once the release is signed (the T3 signing step), not a code defect.

**Seen once, cause not found.** test_file_management_explorer_panel ended once with 0xc0000374 (heap corruption)
in viewModesSwitchAndPersistExplorerSettings, in a -j 6 run of the 25 suites this round touches with the package
being staged beside it. It did not fail again in 8 runs on its own, 3 runs at -j 8 beside the heaviest suites, or
the full suite (312 tests, -j 6) before the push. This round changed nothing that test drives (view modes and the
listing); it stays open.

**What no test here covers.** The console-host rule's wiring in the runner's settle step: the tests prove the rule,
the Job listing on a real family, and that a real leftover still fails; a console host that outlives the grace was
seen nowhere but the VM and was not reproduced, so the path that stops it and passes the run has no test of its own.
The restore's network step was tested with netsh faked: no adapter was changed anywhere. The bus and media read from
MSFT_PhysicalDisk (a real PowerShell query) is tested by its mapping, not run. The light arrow's absence in a real DWM
window, the recovery page's refusal and the grayed Convert Dynamic were proved on testpc1 (above), not by a test here.
The palette and Dual Pane fixes were checked offscreen, not run through the capture again; Notepad++'s leftover list,
the System Inventory Report on the VM and the Windows USB 3-minute wait were not run again.

**Left behind.** testpc1: this round's package in C:\SAK, as the capture harness installs it, with its copy in
C:\SAKCapture\pkg and the runs' output in C:\SAKCapture\out; C:\SakIso and C:\SAKCapture with its two tasks, kept;
the console back at 1024x768 and Technician's apps theme back at dark; D: BENCH (32 GB NTFS) on the Best Buy NVMe,
as it was; Rig A stays on the disposable Seagate (MSR, E: SAMPLE, F: DONOR) and Rig D on the disposable USB stick
(H: BASICMBR); this round's scripts, probes, wizard probe and scheduled tasks removed. The capture VM was not used.
On the workstation: the package, the probe pictures and every proof picture under E:\SAK-Utility-Test-Scratch\round6.

### UI review round 7: the explorer heap corruption, and round 6's open list (2026-11-07)

From round 6's open list and the reviewer's re-check (`ui-review\recheck3.md`), on dev at 20e330ea. Every fix has a
test, and every test was drilled red with its fix broken in the source, one guard at a time, by a script that
restores every byte and touches the file: 42 guards. One more survived: the row-height half of N2 (Map
Users) passed with its line taken out, even under the app's theme, so the line was removed and the clipped
descenders stay open (below); the column half went red (19 px for a 337 px box). Judgement calls and items that were not defects are
named below.

**The heap corruption (High).** test_file_management_explorer_panel ended with 0xc0000374 about one loaded run in
30. Windows Error Reporting had kept round 6's minidump (2026-11-07 00:07), and a loop of 30 loaded Release runs here
(build load and an ASan run beside it) produced a second at run 17. The release binary has no PDB, but relinking the
same objects with /MAP alone gives a binary that differs in 4 bytes (time stamp and checksum), so its map names the
dump's frames. Both dumps show the same stack: QtTest, viewModesSwitchAndPersistExplorerSettings,
switchThroughViewModesAndVerifyVisibility's first statement, `listAction->trigger()`, and inside Qt6Gui the end of
QAction::activate, where the action's QPointer guard drops the shared weak reference with `lock xadd` and, only when
that was the last reference, tail-jumps to operator delete; that delete is the free that found the heap corrupt. The
reference is the last one only when the action was destroyed during its own trigger. It was: the pane's
selectionChanged handler calls updateActionButtons() synchronously, and that rebuilt the View menu with
menu->clear(), deleting every View action, the item size slider and its row, while one of them was still being
delivered; each rebuild also left an empty QActionGroup in the menu. Qt guards its own frames against a sender
deleted in its slot, so the exact write that damaged the heap is in Qt's code and was not pinned down; what the dumps
prove is the deletion at the point of failure. The View menu is now built once and refreshed in place (text,
enabled state, blockers, check marks, the slider's range and value), and nothing in it is deleted.
viewMenuActionsSurviveARefreshDuringTheirOwnTrigger changes the selection while the List action's triggered() is
still being delivered and checks the action and the slider survive and the menu holds one action group; with the old
code it fails every time. Proof under load: after the fix, 120 of 120 loaded Release runs clean (before it, 1 in 30
here and 1 in about a dozen in round 6). AddressSanitizer (the Debug build, with the runtime hooking the process
heap too) found nothing in six runs of the suite before the fix, solo and loaded; Qt's own code is not
instrumented, which is why the dump, not ASan, found it.

The same pattern elsewhere (a sweep of every menu clear, action removal and synchronous widget deletion in the GUI):
the breadcrumb deleted the segment button being clicked, and with it the overflow menu whose entry was being
triggered, because a segment's click navigates and navigating rebuilt the breadcrumb; old segments now leave the
breadcrumb at once and are deleted from the event loop. The Sort flyout kept one more hidden "Group by" submenu every
time it opened (clear() does not delete a submenu); the old one is deleted as the flyout is rebuilt. The New,
Selection, Sort and tab menus rebuild only as they open, the context menus are stack menus run with exec(), and the
AI transcript and Partition Manager rebuilds already used deleteLater.

The four explorer crash reports from 2026-11-04 (and three from test_diagnostic_benchmark_panel and two from
test_quick_action_panels) are not this: each is QtTest's watchdog ending a hung test, its main thread in
QDialog::exec, mostly QFileDialog::getExistingDirectory, opened by a press with nothing to answer it. They fell
between 01:39 and 03:26 while 7a1afcf4 (04:02, "Let a press return before its dialog opens, on every panel") was being
made: queuing each dialog-opening press broke test drivers that answered from a zero timer, and that commit rewrote
them to wait for the dialog's loop. No watchdog dump since.

**The AddressSanitizer pass over the GUI suites.** The Debug build of all 31 suites that link sak_gui ran under ASan
(the explorer suite seven times). One finding, a test defect: test_image_flasher_panel stopped on DriveScanner's
one-instance assertion, because init() makes a panel for every test and four tests added since built a second panel
beside it. In Release the assertion is compiled out, the second scanner took over the static instance its device
notifications go through, and its destructor then cleared it under the fixture's scanner. The app has one panel; the
tests use the fixture's. The suite now passes under ASan.

**Fixed, each with a test.** Storage Health reads Windows' model and bus for every physical disk once per run
(the hardware scanner's disk query without volumes), so a drive smartctl cannot read is named "PhysicalDrive1
(NS-PCNVMEHDE)" with its bus when no hardware scan has run, and every drive's bus is spelled as the inventory spells
it, "NVMe" not "NVME" (X1's rest). Temperatures have one style, the degree sign, in the SMART table, the stress
results, the SMART warning, the stress abort reason and the HTML report (X17). CPU Benchmark has a "Score:" line in
the score style, like Disk and Memory, and its two thread results are measurements (F40). The Disk I/O drive box says
"Run Scan Hardware first" until a scan fills it (X15). A running stress test says so in the status bar, the bar set
first so the message stays beside it (X3). AI page: disabled icons follow one rule in both themes, the drawing's
silhouette in the disabled label's colour, read as the icon is drawn (X2); the popups take the width their entries
need, without the fixed 420 px minimum (F21); the workflow pane's Close is secondary (F27); its headings use the
pane's sizes, which setMarkdown() had replaced with Qt's own (N17); the workflow picker has the pane's width, with
Add under it (DF11); the AI status ends in an ellipsis when the status bar squeezes it, with the whole text in its
tooltip (S22, "Context: pe"). Network: the adapter Status column takes its contents' width, measured as the theme
draws it (S15, "Disconnect..."; the old width was 168 px for 175 under the theme); a finished port scan puts its
result in the status bar (S22). Backup and restore: Map Users' merge-mode column takes its combo's width (N2, "Merge Into Destinati"); one
byte format on the restore folder page and its summary, the per-user dialog and both workers' summaries (X7); the
restore Wi-Fi page says once that a backup has none and spells Wi-Fi one way, as the backup page now does (X20, S24);
the last step's two options are one form, each box beside its label (X11); Installed Applications names a Store
app's publisher by its organisation, with Advanced Uninstall's helper (N9); Ethernet settings show addresses whole
(S15); "Select Users to Back Up" and "back up" as the verb in the wizard's and the worker's texts (low). Image
Flasher: a raw flash's steps reach the status bar and its bar ends with the run, completed, failed or cancelled (S22);
"1 drive available" (S24); the Windows ISO download bar counts bytes, with tenths below 1% (X19); the settings
dialog's Restore Defaults is secondary (X10). Vulnerability Scanner: ten columns that fit a 1280 px window beside
Patch Recommendation, which stretches and wraps (F18, S15), and the empty table says what to do (X12). File Explorer
and Advanced Search: the file list and the sidebar select in the theme's selection colour (S7/DN8); the preview
caption gives sizes as the list does and sits in from the pane's edge (X6); metadata dates read as the lists' times
(X16); an incomplete search's status fits beside the progress bar (N6). Partition Manager: Convert MBR/GPT names the
disk and its present style and starts on the other style (X21); the optimize and BitLocker dialogs' Copy and Open are
secondary, so the Queue action is the one primary button (X10), and the optimize row is "In-app optimization" (X9);
Quick Partition's count is "Partition count:" (X13); the operations list scrolls down only, so no horizontal bar
covers "Initialize Disk" (R1). Email Tools' file scan uses the app's byte format (F38). Advanced Uninstall's Version,
Size and Install Date take their contents' width (S15). "1 package", "1 file" and the administrator prompts' reasons
no longer repeat the requirement the prompt's first line states; the data-folder refusal's title is "S.A.K. Utility:
data folder" (S24).

**Before the push.** The T2 labels (product, release-tooling, tooling, cert) three times at -j 6, 312 tests
each, all green, and the whole-tree static gates clean.

**Not defects, and judgement calls.** The backup wizard's "< Back" on Select Users is right: that is its second page,
after the welcome page, and every wizard's first page has Back disabled (QWizard's own rule). X14, the log window over
the main window's right edge on testpc1: the owner's ruling keeps the log a detached window that snaps beside the main
window, and testpc1's screen is narrower than the two windows side by side, so it overlaps rather than going off
screen; it sits beside the window on any screen with room. S22 (a) and (b), "Explorer loaded" and "7 adapters found"
on other tabs: fixed in round 6 (the status bar clears on a tab change).

**Still open, with what each needs.** F23: Explorer's details lines are selectable labels, and a QLabel wraps only at spaces; a long path needs a
selectable text widget that wraps anywhere (zero-width break characters would go into every path a technician
copies). The horizontal bar keeps the whole line reachable meanwhile. X4's second half (Resize/Move's Support line
clipped at its descenders) and Copy Disk's 24 px target segment with no room for its label: both need the painter or
the dialog's layout reworked and a picture to judge them. S25: Benchmark, Application Management, Image Flasher and
Backup draw their light-tab icons in pale tones (1.2:1 to 1.6:1 on the tab); they need light-theme drawings, kept to
the rule round 5 set for File Management (half the ink at 3:1, under a fifth below 1.5:1). F36: the BitLocker folder
picker starts in the working folder (C:\SAK when launched from Explorer); Documents was ruled out in round 4 because
it is usually on the volume the keys unlock, and starting on "This PC" (Qt's clsid URL) is the proposal. X10's rest:
the QR dialogs put Cancel before the action and the batch uninstall's Remove Selected and Clear Queue are primary.
RG2 and S15's Email Items sender: round 6 widened the pane to 480 px; not measured again. The 146 other "(s)" plurals
in the GUI (the reviewer's listed ones are fixed). N2's clipped descenders: offscreen, themed or not, the
rows already fit the combos, so whatever clips them on testpc1 (DPI, the cell widget delegate) needs a picture
there.

**What no test here covers.** The administrator prompts' reasons, the data-folder title and the two package confirmations are wording with no test
of their own. The Windows ISO byte count is tested at the dialog; the builder's signal was not run (it needs a real
UUP download). The breadcrumb and Sort-flyout fixes are tested offscreen, not clicked in a real window. Whether a
long workflow name now fits the picker is a picture's question: this offscreen platform has no font files and its
stand-in font is about twice as wide as Segoe UI. No picture was taken this round: the visual checks above
(DF11's fit, N2's descenders, X4, Copy Disk, S25) are for the next capture.

**Left behind.** testpc1 and the capture VM were not used this round and are as round 6 left them. On the
workstation: the two minidumps, the cdb output and the relinked map under E:\SAK-Utility-Test-Scratch\round7\dump,
the loop and drill logs beside them, and the Debug (ASan) build in lane2's build tree.

### UI review round 8: round 7's open list, the plurals, and pictures of what rounds 6-8 changed (2026-11-07)

From round 7's open list and the coordinator's calls on it, on dev at 9c218abf. Every fix has a test, and every
test was drilled red with its fix broken in the source, one guard at a time, by a script that restores every
byte and touches the file: 27 guards, the plural test by a planted "attachment(s)". Two survived their first run.
Growing Resize/Move to its layout's height-for-width changed nothing a test sees, because the wrapped labels'
minimum heights already make the window grow; it was removed. Putting back only Application Management's pale
arrow left 51% of its ink at 3:1; putting back its whole old drawing went red (0%). The last two guards came after
review 4 (S25 below): putting back round 8's first thin-stroke drawing of Benchmark and Diagnostics, then of
Backup and Restore, each went red.

**Plurals (S24, app-wide).** 264 string literals in 57 files said "file(s)", "match(es)", "director(ies)" and
the like, in the panels, the AI assistant's tool results, the actions' reports and the log; the app loads no
translations, so Qt's tr("%n item(s)", "", n) showed "(s)" too. Counts are spelled with sak::countOf ("1 file",
"3 files"), moved from widget_helpers.h to format_utils.h so core messages use it; a sentence whose verb followed
the count was reworded so the verb does not depend on it ("Not cleaned: safety screening refused 2 selected
items."); a label with no count says what it means ("Step 2: Select Target Drives", "Export formats:", "Copy
Selected Rows"). The stress report's CSV header "Duration (s)" reads "Duration (seconds)". "http(s)" (a URL
scheme) stays. test_plural_placeholders reads every string literal under src/ and include/, comments left out,
and refuses any such placeholder; a runtime string is built from these literals, so it answers for every message
the code can build. Batch Uninstall's header and the program count said "1 programs" and count through countOf
too. One grandfathered duplicate message changed in both its places (the PST and MBOX search warnings), so its
entry in scripts/error_message_duplicate_baseline.json was edited by hand to the new text; the four stale entries
a regeneration would also have dropped were left for their own change.

**F23, File Explorer's details lines.** Properties, Safety and Evidence are read-only, frameless text boxes
(FileExplorerDetailsText, a QTextEdit) that wrap at a word boundary or, when a line has none, anywhere
(WrapAtWordBoundaryOrAnywhere), with no scroll bar of their own and as tall as their text at the width they are
given (heightForWidth on a copy of the document), so the details scroller scrolls them together as before. No
character is inserted: selecting and copying a path gives the path as it is. Each section is named for a screen
reader, beside its text.

**F36, the BitLocker key picker.** It opens on This PC: FOLDERID_ComputerFolder as a clsid URL, the form Qt
documents for a Windows virtual folder, given to a QFileDialog's setDirectoryUrl. It takes only file: answers,
so a virtual folder cannot come back as a destination. Qt's own dialog, which the test drives, cannot open a
virtual folder and says so; the test reads that warning as proof the place was handed over, and the Windows
picker opening on This PC is the testpc1 picture.

**X10's rest.** The single QR wizard puts Cancel after Next and after Generate, the batch QR dialog after Batch
Generate, and both Browse buttons are secondary, so the action is each page's one primary button. Batch
Uninstall's Remove Selected and Clear Queue are secondary beside the danger-toned Start Batch Uninstall.

**X4's second half, Resize/Move's Support line.** The Support line is a wrapped label in a form row, and the
form gave the row less than the label needs at the row's width: offscreen it held one line of the four the
text needed at 533 px, with the dialog's layout reporting it fitted (648 of 648 px). On testpc1, Move partition
start's two-line text lost its descenders. A partition operation dialog now gives each wrapped label, as its
minimum height, what its text needs at its width, on every layout change and resize; the window's minimum
follows from its layout's, so the dialog grows with it. A second guard written beside it, growing the dialog to
its layout's height-for-width, survived its drill for that reason and was removed.

**Copy Disk's target.** On a target much larger than the source, each copied partition was a 24 px segment too
narrow for any name. When the copied partitions cannot each be named where they land, the Target bar draws the
copy as one segment named "Copy of Disk N", wide enough for its name (segments now take a floor each), then
the space left; a target near the source's size still shows them one by one.

**N2's descenders.** A table's rows are as tall as its cell widgets ask (the vertical header resizes to contents
in placeCellWidgetsInWholeCells), so a themed combo box in Segoe UI keeps its descenders. Round 7 could not show
this offscreen because the platform's stand-in font is not Segoe UI; the test now loads Segoe UI from Windows'
fonts folder and draws in it at 9 pt, and without the fix the combo is 29 px in a row for one that needs 33.

**S25, the light tab icons.** Benchmark and Diagnostics, Application Management, Image Flasher and Backup and
Restore are redrawn for the light theme in darker tones of their own colours; their old drawings are kept as the
"-dark" files the dark theme already picks. Near-white parts that were holes in the white selected-tab drawing
stay holes (Application Management's tray is now open rather than pale), so the selected tabs look as before.
Measured as the reviewer did, on review 3's light app_main_window (ink: pixels that differ from the #cfd8e3
tab): Benchmark and Diagnostics median 1.17:1 with none of its ink at 3:1, Application Management 1.24:1 and
none, Image Flasher 1.44:1 and none, Backup and Restore 1.55:1 and none. After the first redraw, the test's
reading of the 20 px icons on the light tab: 63%, 80%, 92% and 71% of their ink at 3:1, and
tabIconsReadOnTheirTabsInBothThemes held these four in light too (half the ink at 3:1, under a fifth below
1.5:1), as it held File Management and Email Tools. Review 4's light app_main_window on testpc1 read them the
reviewer's way, every pixel an icon touches counted in full: Image Flasher median 3.53:1 with 81% at 3:1,
Application Management 3.53:1 and 61%, but Benchmark and Diagnostics 2.80:1 and 38% and Backup and Restore 2.78:1
and 47%. The test had weighed each pixel by its coverage, so a stroke a quarter of a pixel wide (the stethoscope's
tube) or a 2 px ring anti-aliased in a 3.3:1 blue (the arrow) counted for little; read pixel by pixel offscreen,
the two were 37% and 43%, as on the real tab. On the coordinator's call, a follow-up (46e51d00) gives the tube,
the badge and the arrow strokes and darkens the blues and the gold, and the test now also reads the four by the
reviewer's measure and wants more than half their pixels at 3:1 on the light tab: 62% and 65% offscreen. One
light app_main_window taken on testpc1 from that commit's package (SHA-256 2AFCC9FD...5615), read the same way
on the real tab: Benchmark and Diagnostics median 3.90:1 with 64% at 3:1, Backup and Restore 4.91:1 and 66%,
Image Flasher and Application Management unchanged (3.53:1, 81% and 61%). All four now have a median of 3:1 or
better.

**RG2.** No code change: round 6 widened the Items pane to 480 px. In review 4's email_inspector_message and
_headers on testpc1, in both themes, every sender in the Items pane reads whole at 1280x860 ("Marisol Quint",
"Ines Caldera") and so does the From / Name header; the pane's horizontal bar is for the columns beyond it.

**Before the push.** The suites these changes reach, then the T2 labels three times at -j 6 (334 tests each: the
label pattern also took in the 21 tooling-extended and cert-tooling tests), all green, and the whole-tree static
gates clean. The commit hooks' cppcheck then asked for a const reference in the Wi-Fi test's helper; that test
was rebuilt and rerun before the commit. T2 run 42395865259 on c759cba2 passed. For the S25 follow-up, the
theme test, the labels once more at -j 6 (334, green), and T2 run 42403749219 on 46e51d00, which passed.

**Not defects, and judgement calls.** The row-height fix is in placeCellWidgetsInWholeCells, so it reaches every
table with cell widgets: Map Users, the Wi-Fi saved networks and Quick Partition's table (the AI context list is a
list, not a table); their rows are sized to their contents. Copy Disk names partitions one by one whenever each
can be named where it lands, and otherwise draws the copy as one segment; the Source bar is unchanged. In the
light S25 drawings, two pale fills that were holes in the white selected-tab drawing (Application Management's
tray, Benchmark and Diagnostics' badge) are left out instead of darkened, so the selected tabs look as before.
Wiki pages that quote the old wording (check-drive-health "Scanned 2 drive(s)", extract-email-attachments
"Saved 12 attachment(s)", flash-iso-to-usb "Step 2: Select Target Drive(s)" and "target drive(s)",
wifi-qr-codes "Select export format(s):") need the new words; this round edits only the site's shot files.
Found while measuring and not in the item list: in light, Help and Support's and About's tab icons are pale
discs (0 of their ink at 3:1 on the tab, read by the same test), and Partition Manager's has 40% at 3:1.

**Still open.** Nothing from round 7's list is left in code; the pictures below judge N2, X4, Copy Disk, S25, F36
and RG2. Read pixel by pixel as the reviewer reads them, other light tab icons sit under half at 3:1: Email Tools
43%, Network Management 42% and Partition Manager 35% offscreen, and Help and Support's and About's pale discs 0.
None was in this round's list; the test reports them and holds only the four S25 redrew to the new measure.

**What no test here covers.** The plural test reads literals; whether each reworded sentence reads well is
review, not test (the suites' own assertions cover the messages they already pinned, now in both forms where one
line added it). The Windows folder picker opening on This PC is a picture, not a test (Qt's own dialog cannot
open a virtual folder). The S25 numbers above are offscreen at 20 px; the pictures are the real tab.

**Review 4: pictures of what rounds 6, 7 and 8 changed, on testpc1.** From the Release package built at
c759cba2 (SHA-256 38787171...CE28), installed by the capture harness, and the main worktree's sources at that
commit for its label map. 95 wiki shots in both themes, 190 rows: 186 pictures (93 light, 93 dark), each passed
through the harness's privacy gate and looked at by eye, in E:\SAK-Utility-Test-Scratch\capture\review4 with an
index.json in review 3's form that names the item or items each picture proves. What the round 8 items show:
bitlocker_output_folder and screenshot_settings_output (F23: a folder path wraps inside its word in the Details
pane, no horizontal bar); bitlocker_folder_prompt (F36: the Windows picker opens on This PC, the drives side by
side); net_wifi_qr_wizard, _qr_output and _batch_qr (X10: Cancel last, Browse secondary, one primary button) and
app_mgmt_uninstall_batch (X10 and "2 programs"); partition_manager_move_dialog (X4: the two-line Support text
whole, the window taller) beside _resize_dialog; partition_manager_copy_disk_review (Copy Disk: the 238 GB disk
onto the 7.28 TB Seagate reads "Copy of Disk 1" then Unallocated); restore_wizard_map_users (N2: Segoe UI combos
whole, "Merge" with its descender); email_inspector_message and _headers (RG2, above); app_main_window (S25,
above); and plurals throughout ("Found 1 saved Wi-Fi profile.", "6 recoverable files found", "23 files, 7 file
types", "3 drives available", "Step 2: Select Target Drives", "Writing to 1 drive...", "Select export
formats:"). The other rows are the round 6 and 7 screens, each with its items in the index. Not captured, in
either theme: app_mgmt_uninstall_leftovers (the shot really uninstalls Notepad++, whose own uninstaller asks
questions only a person answers; at the 600 s wait the list still read "1 program"; review 3 could not capture
it either) and restore_wizard_progress (a real restore applies network settings over the link the capture runs
through). 64 rows ran testpc1 variants, kept in the round's scratch and named in each row: its own disk and folder
names, "Windows (C:)" for the VM's "Local Disk (C:)" with SAK-Samples reached by type-ahead in a tree too long to
show it, AnyDesk for the absent VLC in Batch Uninstall (Apple Software Update's version 2.7.0.3 read to the
privacy gate as an address), a click and Home in the ISO dialog's Builds list for the operator's click, and
pauses where a menu or the edition list was slower than the steps. One site shot file was edited, uncommitted, and
recorded in shot_edits.json: help.json's wait for help_data_folder_error now reads "S.A.K. Utility: data folder",
the title round 7 gave the box. The gate masked the real addresses and MACs (Network Adapters, both Ethernet
pages), the saved network's SSID, Admin's profile path and the restore summary's computer name; link-local
169.254 addresses and the disposable drives' serial numbers stay, as in review 3. Found while capturing, not
defects of the app: the harness's 10-minute batch limit cut the first sample backup (Admin's 4.9 GB), so the
runs took a longer limit; the Image Flasher's "Unmount drives on completion" ejects the stick's media, and Windows
then lists it with no media and size 0, so a load-media request went to the pinned stick between flashes; on
this stick Clear-Disk left an MBR disk, not RAW, so Quick Partition and Convert MBR/GPT show "Keep MBR" and "(now
MBR)" where the VM showed a RAW disk; and the light run's Optimize Power left High performance, so the dark run's
first try had nothing to switch, and Balanced was restored before its retry.

**Left behind.** On testpc1: D: BENCH (32 GB NTFS) and Rig A on the disposable Seagate (E: SAMPLE, F: DONOR,
BitLocker off again on F:); the console at 1024x768; Technician's apps and system theme dark; the Balanced power
plan; C:\SakIso; C:\SAKCapture with its SAKCapture and SAKCaptureElevated tasks (its output folder emptied, and its
package cache now holding the S25 check's package); C:\SAK, the harness's install of the 46e51d00 package from
the S25 check; Notepad++, which the uninstall shots need; and the disposable USB DISK 3.0 stick holding the
sample Alpine image the flasher shots wrote, its media loaded. Removed: the account Sam and its profile, the
sample files and folders on C:, D: and E: (D:\SAK-Samples held the BitLocker key backup of the throwaway F: key,
the sample backups and the Settings screenshots with the real SSID), C:\Program Files\SAK-Test, the round's job
folder and job task. The capture VM was not used and its snapshot was not touched.

### Certification rig: a BIOS install that failed, and a run that went on (2026-11-08)

The second final run, at f0118642, passed vhd 12/12, data 5/5 and boot-uefi, then built its BIOS install and failed in a way the rig did not see. The fixture's guest job logged `JOB STOPPED: DISM capture failed with exit code 21`, and `Invoke-GuestElevated` returned that log without failing, so the group went on to start a disk with no Windows on it. The run was stopped by hand; testpc1 was not reached.

**Why DISM failed, twice.** The fixture captured this Windows through a VSS snapshot of C: mapped as R:, which takes half an hour or more. C:'s shadow storage limit was 5.10 GB, 1% of the volume. Volsnap event 33 deleted shadow copies to stay under it at 22:48, 22:50, 22:57 and 23:02:55; at 23:02:52 System Restore's scheduled task (`srtasks.exe ExecuteScheduledSPPCreation`) had made a new snapshot, and the deletion at 23:02:55 took the oldest, the capture's. dism.log at 23:02:58: `R:\Windows\WinSxS\...\Microsoft.PackageManagement.MetaProvider.PowerShell.dll (HRESULT=0x80070015)`. The first fix set C:'s shadow storage to 40% before the snapshot, and the rerun at b5906c13 logged 102 GB (40%) at 02:32. At 03:12:05 another scheduled snapshot came in (VSS 8231, `taskhostw.exe`), volsnap deleted three copies "below the user defined limit" at 03:12:08, the limit read 5.10 GB (1%) again afterwards, and DISM failed at 03:12:08 with 0x80070006 on `R:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell_ise.exe`. System Restore puts its own limit back. No setting made in the guest keeps a shadow copy safe for the length of a capture, so the fixture no longer uses one: the rig clones the VM's system disk to a throwaway VDI with the VM off (`CloneSystemDisk`, `VBoxManage clonemedium`) and attaches it as a third disk; the fixture onlines it once through diskpart, which gives it a new signature for the one it shares with C:, sets it read-only, maps R: to its Windows partition's own device, checks the SYSTEM hive is there, and captures from that offline copy, with DISM's scratch on T:. The clone is detached and deleted afterwards, on failure too. Two trials on the VM, at about 03:25 and 03:35 on 2026-11-09, before the run: `clonemedium` of the system disk took 231 s; the clone came up as `offline=True reason=Collision`; `online disk` gave it a new GUID and `attributes disk set readonly` left it `readonly=True`; R: mapped to `\Device\Harddisk1\Partition3` with `SYSTEM hive present: True`; and `dism /Capture-Image` of `R:\Windows\System32\drivers` (572 files) "exited 0 in 17 s; wim 59.7 MB", the image reporting `Size : 136,795,997 bytes`. A full capture of the clone was started in the first trial and stopped after three minutes by design; its WIM was still 0 MB then, so the length of a whole capture from the clone is not yet measured.

**A third stop: the VM's Windows bugchecked (observed rig fault).** The rerun at f7619773 stopped at 04:48 on 2026-11-09, seven minutes into the vhd group, when the rig's next guest command found "The guest execution service is not ready": Windows had restarted. WER and the minidump: `BugCheck 0x139 (0x3, 0xffffd20c93d71e90, 0xffffd20c93d71de8, 0x0)`; WinDbg 1.2611 `!analyze -v` (run on testpc1): `FAILURE_BUCKET_ID 0x139_3_CORRUPT_LIST_ENTRY_KTIMER_LIST_CORRUPTION_nt!KiCancelTimer`, process `dwm.exe`, image `ntkrnlmp.exe` 10.0.26100.9711, stack all `nt` from `NtWaitForMultipleObjects` through `KiSwapThread` to `KiCancelTimer+0x108`. A timer queued on the desktop compositor's thread was corrupted, which points at a graphics or timer user such as VBoxWddm, not at a storage driver or this app: no storage driver, vhdmp or app process is on the stack, and the row's step client (vhd.quick-partition-custom) had already logged its last scan and exited. In the same minutes Windows Update installed Microsoft.WindowsAppRuntime three times (04:46:29, 04:47:21, 04:47:52) and the Edge installer failed (04:47:04). Before the next run, after restoring `pre-round8`, the VM was quietened, and the run's log holds the same record: 04:58:30 policy `HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU` NoAutoUpdate=1 and `HKLM\SOFTWARE\Policies\Microsoft\WindowsStore` AutoDownload=2; wuauserv, UsoSvc, InstallService, edgeupdate and edgeupdatem stopped and Disabled; TrustedInstaller stopped and left on Manual, since a DISM or servicing step may need it; 04:58:34 task `\Microsoft\Windows\SystemRestore\SR` and the MicrosoftEdgeUpdate tasks Disabled, the one that was running stopped at 04:58:48. These are VM settings, not product or rig code, and the next restore of `pre-round8` undoes them.

**The rehearsal at f7619773, 2026-11-09.** Launched at 05:00 after that hygiene, it ran every group to the end in 409 minutes. On the VM: vhd 12/12 (38 min), data 5/5 (9 min), boot-uefi (22 min), boot-bios (99 min; the clone's full capture took 48 min, apply, bcdboot and bootsect 23 min). On testpc1: ssd-device 3/3 (232 min: ReTrim 1.2 min, the secure-erase overwrite 58.7 min at about 73 MB/s, the hardware wipe 172.2 min at about 25 MB/s with 19.1 MB/s sustained in places, the group 7.5 min inside its 240-min limit), hdd-device 2/2 (4.1 min; the defrag on the Seagate with Windows' media type `Unspecified` and the drive's 5425 rpm), and data-device 4/4 (4.5 min). Select accepted all 30 rows. The claim block it wrote said `CodeCompleteOnly`, because the worktree then held another fix round's uncommitted product edits and the status reads the tree's digest; against a clean checkout of f7619773 the same evidence computes `NamedDevicesValidated`, 12/12 and 18/18. A Reset Network defect found by the VM smoke the same day means another final run after its fix, so this run is a rehearsal: its evidence is kept outside the repository, and nothing under `certification/` or the claim block is bound to f7619773.

**Why the run went on.** `Get-RigJobFailure` now reads a guest job's log, and `Invoke-GuestElevated` fails the step on `JOB STOPPED`, `NOT DONE after`, a nonzero `exit`, or a log with no exit line, after showing the log. A run of rows may exit 1 for a failed row, because its group checks each row's report; a run of rows that stopped still brings back what it wrote, then fails. The BIOS build detaches its disks, deletes its scratch and its unbuilt disk, and stops its group when the fixture fails.

**A second reliance on the silence.** `Get-RigBootFacts` read a fixed `boot-facts.json` from the proof VM's disk. The UEFI and BIOS repair rows start the same disk twice, so a facts job that failed on the second start would have returned the first start's facts. It now deletes that file before the job, and a failed facts job fails the proof.

**Proof.** `test_partition_manager_rig_pc_target` gains ten checks: each failing log fails `GuestJob`, a clean one passes, rows that exit 1 come back, rows that stopped come back and fail, stale boot facts go first, and the fixture reads an offline clone, read-only, made with the VM off and deleted after. Eight drills, one guard each, each red in that test, the scripts restored byte for byte. Not tested here: the group's own cleanup after a failed BIOS build, which needs the VM.

### Partition Manager: a slow scan read as no disks, and the final run's two stops (2026-11-08)

The first final certification run at 17a40a4d stopped in its VM phase, on two faults. Neither touched testpc1.

**8 of 11 VHD rows: "The inventory scan found no disks".** Each step client's standard error said why: `Partition Manager inventory scan returned 0 disks, 1 warning` and `inventory warning: Process timed out`. It was the standard-user browsing scan, one powershell.exe under a 30 s limit (`kTimeoutProcessLongMs`), on a VM whose Mac host was throttled to 31-43% CPU. The guest's own clocks disagreed under that load: a client that ran 22 s by file time held a scan its timer stopped at 30 s. The same scan in the same VM, idle, with a fresh VHD as a row has one, took 10.4-11.7 s: `Get-PhysicalDisk` 4.3-5.3 s, compiling the dynamic-disk probe 0.8-1.4 s, loading the BitLocker module 0.4-0.8 s, partitions and volumes about 1.9 s. A product fault: a slow PC read as having no disks.

**The fix.** The scan writes a progress line on standard error as it starts each step, disk, partition and admin-only read (`SAK-SCAN-PROGRESS`). `InventoryScanLiveness` stops it only after 30 s with no progress line, or 5 minutes in all, for the browsing scan, for Apply's re-read in the elevated helper (whose progress frames carry the same lines, and whose quiet clock starts once the helper runs, not while UAC waits), and for the read-only scan. A scan stopped either way lists no disk, is marked `scan_did_not_finish`, and says "The disk scan did not finish: no progress for 30 s (last step: ...)" or "... within 300 s ..."; the panel adds "Click Scan Disks to try again." and Apply refuses on it as before. Each volume lookup and each disk's partition list runs as a storage job bounded at 20 s (`Invoke-SakBoundedQuery`), and fsutil's dirty-bit read and smartctl's rotation rate run bounded too (`Invoke-SakBoundedProgram`, shared with the defrag script), so one that does not answer is that volume's or that disk's error and the other disks still list. The panel shows "Scanning disk N of M" in the status bar and over the table. A standard-user scan no longer loads the BitLocker module, and the dynamic-disk probe compiles only when Get-Disk left a disk out. Not bounded: a per-volume BitLocker read and the first storage call that loads the module; a hang there trips the quiet limit, and the scan says it did not finish.

**Certification tooling.** The step client's refusal for an empty scan carries the scan's own warnings. A group stops at its first failed row, in the guest and in the group runner, and the rig brings back only rows that ran.

**boot-uefi: a sign-in that did not land.** The first boot proof of `external.boot-repair-uefi` reached sign-in, the rig typed the password once, and three minutes later the elevated task had not started: "is anyone signed in?". The screenshot taken before typing was black; Guest Additions report nobody signed in before the sign-in screen is drawn, and keys typed then are lost. The boot proof, a kept proof VM and its facts never checked that a sign-in landed, though the main VM's start did. `scripts/partition_manager_rig_sign_in.ps1` is now the one routine: type, wait up to 90 s for Windows to name a signed-in user, try four times, else stop as "did not sign in".

**Proof.** Added: `inventoryScan_aSlowScanThatKeepsMovingFinishes`, `inventoryScan_aScanThatGoesQuietDidNotFinish` (partial JSON written before the stall is not taken as a list), `inventoryScan_aScanPastItsCeilingDidNotFinish`, `inventoryScan_oneVolumeThatDoesNotAnswerIsThatVolumesError` (stand-in jobs, and a parse where one volume's lookup failed and both disks list), `inventoryScan_writesProgressAndSkipsWhatItCannotUse`, the panel's `aScanThatDidNotFinishSaysSo` and `aScanShowsWhichDiskItIsOn`, the step client's `step-empty-scan-says-why`, `test_partition_manager_rig_sign_in`, and the group stop in `test_partition_manager_rig_pc_target`. Eleven drills, one guard each, each red in its named test, sources restored byte for byte and rebuilt green.

### Partition Manager: a USB hard disk Windows cannot name, and the defrag row (2026-11-08)

The final certification's preflight found the defrag row could not pass on the rig's hard drive, and that a technician could not defrag it either. Windows reports the Seagate Expansion Desk 8 TB as `Unspecified`, because the USB bridge hides its type, and the validator, the defrag script and the matrix each required HDD. Windows' other sources do not settle it: `Win32_DiskDrive` says "External hard disk media" for SSDs in USB enclosures too, and the seek-penalty query fails on this bridge. smartctl does settle it, read-only on the PC: the Seagate reports 5425 rpm, and the Best Buy NVMe enclosure reports no rate. Owner's ruling, the same day: accept the drive's own rotation rate.

**The product.** A drive Windows reports as `Unspecified` counts as a hard disk only if it reports a nonzero rotation rate through the bundled smartctl. No rate stays not-HDD and is refused. A rate never overrules any other answer from Windows, and a model naming it an SSD still wins. Reading the rate takes administrator rights, so a standard-user scan leaves the drive pending: Media type and Optimization mode say "Unknown until Apply" and Queue Defrag is on, the same treatment as BitLocker. Apply's own scan asks the drive and the validator decides on that. One function, `Get-SakRotationRateRpm`, serves the scan and the defrag script, bounded at 30 s, and anything but a rate is 0. The on-screen strings are not new.

**The certification rule.** Changed only through `scripts/generate_partition_manager_matrix.py`. The check `windows_reports_a_hard_disk` is now `drive_reports_it_spins`: Windows reports HDD, or Windows reports `Unspecified` and the drive reports a nonzero rate. The row records Windows' media type exactly as Windows gives it and smartctl's `rotation_rate_rpm` as separate fields, and the verifier decides from both. No derived value goes in the Windows field. The rig stages the bundled smartctl with the binaries, because both the app and the rows now read it. A VirtualBox disk is still not accepted at all.

**Proof.** Added: `safetyValidator_defragTrustsTheDrivesOwnRotationRate` (USB hard disk allowed, USB SSD with no rate refused, Windows SSD with a rate refused, other Windows answers and a model naming an SSD not overruled), `safetyValidator_defragWaitsForApplyWhenTheScanCouldNotAsk`, `standardUserScanLeavesOnlyAnUnspecifiedDrivePending`, `scriptBuilder_defragAsksTheDriveOnlyWhenWindowsCannotSay`, `driveRotationRateFunction_readsWhatTheToolReports` (it runs the function against a stand-in that reads no disk), `applyDecidesDefragOnTheDrivesOwnRotationRate`, `diskDefragWaitsForApplyOnADriveWindowsCannotName`, and the `drive_reports_it_spins` cases in `test_partition_manager_row_evidence`, including a device with no rate failing. Sixteen drills, one guard each, each red in the named test, the sources restored byte for byte and rebuilt green.

**Not covered.** No test here runs smartctl against a real drive: the rows do that on the PC. The rate is what the drive says of itself, through its bridge, and a bridge that hides it leaves a real hard disk refused, as before.

### UI review round 10: review 5's findings, Cancel during a flash, and review 6 (2026-11-08)

From the independent review of round 9's pictures (`ui-review\recheck5.md`: 148 of 150 pictures read, five
regressions and 23 new findings) and the coordinator's item list, on dev at 5f01bfee. Every item was confirmed on
that commit first. Two commits: d8a44a0f (the items) and 823baafb (what review 6's first pictures showed d8a44a0f
had left). Every fix has a test, and every test was drilled red with its fix broken in the source, one guard at a
time, by a script that plants the mutant, builds, runs the tests, restores every byte in a finally and touches the
file (testpc2's W:\sak and lane2): 99 drills, 91 red at once. The eight that were not: rg6-send-built (the Send
button's icon at construction; updatePrimaryActionButton replaces it before the page shows, so the mutant changes
nothing); rg9-not-fitted (the first test called fitToHeight itself; it now checks the dialog as it opens: no scroll,
or as tall as the screen allows; red); z6-uninstall-refresh and z6-uninstall-two-primaries (the window-wide check
counts enabled buttons, and Advanced Uninstall's toolbar is disabled while its list loads; a direct test of the five
tools is red); z6-offline-install (Offline Deploy is a sub-tab the window-wide check does not open; a direct test is
red); z12-opaque (the frame alone; the list covers it, so the popup and its list both see-through is the defect, and
that mutant is red); z17-band (Qt's style sheet polish sets WA_StyledBackground itself; removing the transcript's
style sheet is the defect, red); z9-pane-bottoms (the inactive pane's second line no longer aligns the panes, the
footer sync does, and its mutant is red). Every guard's tests passed again on the restored code.

**Must-fix.**
- Z1, no Cancel while a flash wrote or waited for Windows to see a new Windows USB (safety).
  beginConfirmedFlash set the buttons before it showed the progress page, so updateNavigationButtons saw step 2:
  Cancel hidden, Back shown. The page switches first now: Step 3 shows a red Cancel and no Back for the whole
  write, the USB wait included. Cancel asks, No the default; then it is spent and the page says "Stopping the
  write..." until the writer says how it ended. The completion page reads "Flash cancelled" and what the drive
  holds: nothing written and the drive as it was, or writing had started and it needs flashing again or formatting
  (the raw writer's bytes-written count and the Windows USB writer's targetWasWritten() decide; a writer gone
  before it reported is taken to have written). Z3 with it: the write reports its progress from its start, so the
  status bar holds "Image Flasher: Writing to 1 drive..." for the whole write, as a running job's message does (it
  fell back to "Ready" after 5 s).
- RG9, the Windows ISO dialog ran past 768 and 900 px screens with its buttons below them. Its groups and progress
  sit in a frameless scroll area with Download, Cancel and Close below it, outside the scroll, and it opens at its
  content's height capped at the screen's available height less the window frame. Review 6 also found two filled
  buttons (Fetch Builds, Browse) beside the disabled Download & Build ISO: they are secondary.
- Z2, the light progress label (#0f172a) over a #3b82f6-#2563eb chunk at 2.6-3.5:1. The light chunk is
  #93c5fd-#60a5fa; dark keeps its deep fill and white label from the theme's gradient; the AI workflow bar takes the
  theme's.
- RG6, the AI page's Close/Stop (a blank square) and disabled Send and Rename Chat (hairlines): one-colour drawings
  of their own (button-close, -send, -rename, -attach), solid at 20 px enabled and disabled.
- RG7, Partition Manager's disk icon in dark: the table, action list, menus and disk map take sak::themedIcon, an
  icon engine that picks the theme's drawing when it paints.
- RG8, the backup's Installed Applications gave the Publisher what the longest names left: Version and Publisher fit
  their contents and the name takes the rest. The first test survived (one long name; Qt keeps a stretched section's
  old width when nothing is left); it now walks the name from a few words to wider than the page.

**Lows.**
- RG5 and Z7: each review-bar segment's floor is its label's width plus room; neighbours have a line between them;
  the map, legend and bars share one fill. Review 6 showed no line in the real Copy Disk wizard: the line took the
  preview's own Base colour, which a wizard page's style sheet makes transparent. It takes the application's base
  colour, and a test opens the real wizard in both themes and finds it. The table and map name a letterless
  partition "EFI", "MSR", "Recovery" or "Partition N".
- Y8's rest, App Data: name and path fit their contents, Size takes the rest, a wider path scrolls the list.
- Y20's rest and Z9, File Explorer. Type takes the name Windows gives the type, as File Explorer shows it ("Text
  Document", "PNG File", "File folder"; d8a44a0f's Qt MIME names read "Text/csv" on Windows). Name takes what the
  other columns leave, never under 160 px; a pane too narrow for that (about 320 px side by side with the details
  pane open) leaves out Tags, then Modified, then Size, and takes them back when it widens, never saving them hidden.
  Side by side, the panes' footers are held at one height, so both lists end on one line.
- Z4, dark tables: header in the border shade, a selected row #1e40af.
- Z5, a selected file's icon keeps its normal drawing on the light selection.
- Z6 and Z6a, one filled button per page or group box: Advanced Uninstall's Refresh, Batch, Select All, Deselect
  All and Settings; Network Adapters' backup and restore and the servers' Start; Advanced Search's regex, target and
  preference buttons; Offline Deploy's Install from Bundle; the wizards' Scan, Select All, Customize, Browse and
  Auto-Map; Wi-Fi Manager's Connect with Phone/Tablet and Scan Known Networks; the AI rail's browser extension button;
  the ISO dialog's Fetch Builds and Browse: secondary. Reset Network is red.
- Z8, Store apps by the DisplayName Windows' PackageManager gives, in the same PowerShell call; none, the name.
- Z10, the preview caption one gap below the Details/Preview switch, hidden when empty.
- Z11, the View menu's "Item size" in the border colour: the command bar's separator rule (`#fileExplorerCommandBar
  QFrame`) matched the label, a QFrame too, inside the button's menu. The rule names the separators; the row is on
  the menu's colour.
- Z12, the command palette is opaque (its style sheet now a style constant) and as tall as its results.
- Z13, a finished restore shows Finish only, its Cancel buttons disabled and hidden; round summary bullets.
- Z14, Benchmarks' Run buttons one gap below their bars, result lines at one pitch.
- Z15, every tab page paints its own background, so Log rows sit on the page colour.
- Z16, the AI status line: Context before the counts, the access mode as its box names it, Phase only while one runs,
  the turn and session in one form, and "none" for both until there is a turn (review 6: the idle line still ran
  past the status bar at 1280 px).
- Z17, the transcript band and chips take one fill; the chip's close X follows the theme.
- Z18, the light headers' icons for File Explorer and Advanced Search, Duplicate Finder, Advanced Uninstall, the
  Vulnerability Scanner, Network Adapters, Wi-Fi Manager and Benchmarks redrawn so their pale parts stand 3.2:1 on the
  page and their lines darker; the old drawings are the dark theme's. Action cards' logos are left as drawn.
- Z19, the leftovers are a "Leftover Items" group; "Loading installed programs..." only over an empty list.
- Z20, Email Inspector's page arrows together; the list starts unsorted.
- Z21, wrapped preview lines hang under their text.
- Z22, the findings table takes three quarters of the Vulnerability Scanner's split.
- Z23 and wording: Check Disk's summary one sentence a line; "Then select the users to back up."; "It cannot be
  written back."; a USB stick whose SMART a standard user cannot read is logged as a warning; Screenshot Settings
  names its file "Wi-Fi_Settings" (test_user_text_spelling refuses "WiFi" before a non-letter).

**Checks.** The raw flash's confirmation has No as its default and Enter answers No (drilled). Z6a, Z12 and Z17 are
held by the tests above; Z12 and Z17 are also pictured in review 6. Z6a's Select All and Deselect All, and Z19, sit
in Advanced Uninstall's leftovers step, which review 6 did not take (it needs a real uninstall): tests only.

**Review 6.** `capture\review6\{light,dark}\`, with `index.json` naming what each picture proves and the
package it came from: 35 shots of only the screens this round changed, in both themes, 70
pictures, each looked at; 44 on package r10a (d8a44a0f, SHA-256 1E4F53D9...4BDE) and 26 retaken on r10b (823baafb,
SHA-256 6E4306D6...6D8E) for the screens its commit changed. testpc2 took 64 of them; the VM took only the six
account-listing backup wizard pages (Select Users, Installed Applications, App Data) and is back at its pre-round8
snapshot, powered off. Z1's proofs are on testpc2's disposable Samsung Flash Drive (serial AA00000000000489, pinned
by model, serial, size and bus before every write): flasher_progress (a 2 GB image mid-write, red Cancel, no Back,
the status bar at 6-9%), flasher_cancelled ("Flash cancelled" and "Writing had started, so the target drive is not
usable as it is...") and flasher_windows_usb_wait (Cancel during "Waiting for Windows to recognize the new
partition... 0 s of 180 s"). RG9 at 1366x768: flasher_windows_iso_builds_1366, 722x712 on a 768-line screen, groups
scrolling, Download, Cancel and Close in view; at 1600x900 it is 844 tall. Six rows have no picture:
backup_wizard_progress, restore_wizard_permissions and restore_wizard_progress need the VM's throwaway data disk
(the sample backup), which round 9's cleanup removed; their items are held by tests. The first pictures, on r10a,
are what found 823baafb's four fixes (the wizard's invisible dividers, "Text/csv" and the 320 px dual pane, the idle
AI line still cut, the ISO dialog's two filled buttons). Seen and left as judgement: a wide view's Name column
still cuts Screenshot Settings' 36-character file names (Name takes all the room the other columns leave; the full
name is in the tooltip and the details pane); Diagnostics' four one-click maintenance tools are filled, as review 5
saw them without a finding; the AI rail's Add is filled beside Load Key once a workflow is chosen (two sections).

**Static gates and suites.** All of ci.yml's static gates (lizard 1.24.3 from the main venv) green at both commits.
Local label suite, T2's exact labels ('^(product|release-tooling|tooling|cert)$'): 316 of 316 at 823baafb. The
broader unanchored set, which adds the 21 cert-tooling and tooling-extended tests: 337 of 337 at d8a44a0f and at
823baafb, the two app-launching tests skipped as always locally (823baafb's first run met
test_windows_runtime_dependencies' stale-build guard, after the local drills had relinked sak_gui.lib but not the
app; a full relink and rerun were clean). Two loaded -j 6 runs of the 20 changed test executables, each repeated
twice: green. T2: run 42503233394 (d8a44a0f) and run 42511319680 (823baafb) green.

### Partition Manager: steps Windows finishes after Cancel (2026-11-09)

A Wipe Disk's full format kept writing in Windows' storage service for half an hour after the app and its helper
were killed, while the app had said "Command cancelled" and "Partition operation batch cancelled". A VM run killed
each kind of step mid-way with Stop-Process -Force: `Format-Volume -Full`, `Optimize-Volume -Defrag` and `-ReTrim`, a
shrinking `Resize-Partition` and `Repair-Volume -Scan` went on in Windows; cipher.exe /w and convert.exe stopped at
once; an extend, Clear-Disk, a quick format and `Optimize-Volume -Analyze` end too quickly to matter.

- FIXED: each step that hands such work to Windows writes a marker just before and just after the cmdlet
  (`src/core/windows_continued_work.cpp`): `SAK-WINDOWS-CONTINUES|BEGIN|<work>|<bytes>` and `...|END|<work>`,
  through `[Console]::Out` (as pipeline output the End marker waited behind the cmdlet's returned object until the
  script ended; checked with a harmless script under powershell.exe 5.1 in both the command-line and stdin forms),
  the End marker from a `finally`, all on one line so a disk guard goes in front of both. Sites: Wipe Disk (the full
  format and the SSD ReTrim prelude), Wipe Partition, Format and Create with a full format, HDD defrag and its scan,
  SSD ReTrim, Resize when it shrinks, Split, the ext shrink, Check File System, Scan File Systems, the post-checks of
  Allocate Free Space, Change Cluster Size, Move, Primary/Logical, Volume Serial and Dynamic-to-Basic, and Check Disk
  Errors' scan. The helper already streams a step's stdout, so the executor reads the marker as it arrives (its relay
  is now a direct connection on the step's thread), and the controller holds the phase.
- FIXED: while a step is in that phase, Cancel is off (`PartitionManagerController::canCancel`), a cancel request is
  refused with the reason, and the panel says the step cannot be stopped once it has started and that Windows
  continues it even if S.A.K. Utility is closed, with when it started. The close prompt says closing will not stop it,
  asks "Exit anyway?" instead of offering "Exit after cancelling" (`RunningOperations` takes an optional note per
  operation), and exiting still stops the step's script and everything queued after it.
- FIXED: a step whose script ended (cancel, close, timeout, lost helper) with its phase open is reported as possibly
  still running in Windows, and the batch as "stopped", not "cancelled" (log, report, popup as a warning, assistant
  result). Check Disk Errors stopped during a drive's scan says Windows may still finish that read-only scan. The
  Optimize Drives dialog no longer calls the defrag and ReTrim "cancellable".
- Tests: `test_partition_manager_core` (`windowsContinued_*`, `partitionExecutor_streamedMarkerReportsTheWindowsPhase`,
  `..._stopDuringWindowsPhaseIsNotCalledCancelled`, `..._windowsPhaseReachesAnotherThread`),
  `test_partition_manager_controller` (`windowsPhaseTakesCancelAwayUntilItEnds`), `test_partition_manager_panel`
  (`windowsPhaseDisablesCancelAndSaysSo`), `test_running_work_exit_guard` (`workWindowsFinishesIsNotPromisedAStop`,
  `cannotStopTextForEveryIntent`), `test_check_disk_errors_action` (`aStopDuringTheScanSaysWindowsMayFinishIt`). 16
  drills, each guard broken on its own, its test red for that reason, restored byte for byte: the marker helper
  (every outliving kind unmarked), a marker on cipher.exe, `canCancel` ignoring the phase, the cancel refusal, the
  panel's phase connection, the notice's visibility, the close note, the prompt's question, `cannotStopNotes`, the
  batch's "stopped" message, the output-based phase check, the streamed Begin signal, the report's step status, Check
  Disk Errors' stopped message and its scan marker, and the controller clearing the phase at the batch's end.
- FIXED: a killed script also skips its own cleanup. The VSS backup helper (`Invoke-SakBackupViaShadow`) left its
  shadow copy and its `sak-shadow-<hex>` link in the temp folder; cipher /w left EFSTMPWP and its fill files on the
  volume. The scripts now write `SAK-CLEANUP|CREATED|<SHADOW|LINK|CIPHER>|<value>` the moment each exists (the
  shadow's ID as Create returns it, the link's path before mklink, cipher's folder only when it did not exist before
  cipher started) and `...|RELEASED|...` once their own finally has removed it. After a step ends without success,
  the executor runs a cleanup in the same helper (`src/core/partition_stop_cleanup.cpp`) for what is still claimed:
  the link only while it is a link, the shadow copy only by `Get-CimInstance Win32_ShadowCopy -Filter "ID='{id}'"`,
  cipher's folder only while it is a plain folder of plain files. A value that is not exactly what the scripts write
  (a GUID, a `sak-shadow-` + 32 hex path, one letter) is never followed. The step's result, the batch's message, the
  report and the assistant's result say what was removed and what was not, with why; a helper that is gone is not
  relaunched for it, and its leftovers are reported as not removed.
- INCOMPLETE (found on the way, older than this change): when Create succeeds but the new shadow copy cannot be
  read back, the backup helper falls through to a live copy and never removes that copy, even when the step
  succeeds. A stopped step now removes it; a successful one still leaves it.
- Live check on the VM (48 GiB VDI, real app and helper; pictures in
  `E:\SAK-Utility-Test-Scratch\certification-final\live-check\key\`): the phase notice, the disabled Cancel, the close
  prompt and the cipher cleanup passed. It found four more, fixed here:
  - FIXED (older than this change): Wipe Disk on a disk with no partition table failed at once with "Clear-Disk : The
    disk has not been initialized." The wipe reads the disk first, clears only one with a table, and starts a new one
    from Initialize-Disk; its SSD ReTrim does not ask a new disk for partitions. Wipe Disk was the only operation that
    clears first: Convert Partition Style already skipped Clear-Disk on a RAW disk, Initialize Disk never clears, and
    diskpart's `clean` runs only in Primary/Logical, on a disk that holds the partition being converted.
  - FIXED: the markers reached the Log window -- the phase markers through the report's step output, the cleanup's
    result lines as they streamed. The executor reads them and then drops them (`withoutScriptMarkers`,
    `isScriptMarkerLine`): from the streamed log, from each step's output, and so from the result, the report text,
    HTML and JSON and the assistant's result. A dry run still shows its script as it would run, the statements that
    write the markers included.
  - FIXED: the batch's "complete" result opened over a close prompt still saying a full format was running.
    `RunningOperations::notifyChanged` tells an open prompt when a phase begins or ends (it rewrites its question, note
    and button) and when a run is over, before that run's result opens (it closes; nothing is cancelled, and a second
    close exits). The panel's probe still says running while its result is read, by design (the controller blocks
    interaction until then), so the notice names the finished owner and the prompt leaves it out.
  - FIXED (older wording): "Partition changes is running. Exit anyway?" The prompt reads "Still running: <names>."
    and then a question with no pronoun, for every name registered with `RunningOperations::add` (twelve today) and any
    number of them.
  - NOT CHANGED, by design: a cancelled step stays queued ("1 Operation Pending") so it can be applied again or
    removed. Its queue line is the operation's summary alone, with nothing that reads as done.
- The live re-check passed F1-F4 (a RAW disk wiped twice, no marker in the Log window, the prompt rewritten at the
  phase's end, the new wording) and found two more, fixed:
  - FIXED: a cancelled step read "Failed: Wipe Free Space - Disk 1 Partition 2" over "Command cancelled. ...". A step
    now carries `cancelled`, set wherever a cancel stops it (the local script, the helper's task cancelled by the
    helper or by the executor, a cancel before anything was sent), and reads "Cancelled:" in the report text, HTML
    and JSON and in the assistant's step. A timeout stays "Failed:", and a stop during a phase Windows carries on
    stays "May still be running in Windows:". The assistant's apply result for a cancelled batch is its cancel result,
    not "FAILED".
  - FIXED: stripping the markers left the blank lines PowerShell's formatter had put around them (three empty lines
    above a table). A run of blank lines that held a marker collapses to the longest stretch the script wrote between
    markers; a run with no marker in it is the script's own and is kept.
- DESIGN DECISION: no time estimate; the panel says so. Get-StorageJob's PercentComplete for "Format Volume" climbs
  steadily, but reading it while the format runs needs `Format-Volume -AsJob` (its errors then arrive through
  Receive-Job on a destructive path) or a second runspace polling beside it, and neither can be proved without a rig.
  A defrag's percentage resets each pass, a shrink has no storage job, and a scan ends in seconds. The marker carries
  the size Windows works on for a later estimate.
- INCOMPLETE: a live run through the real helper on the VM -- a full format with Cancel going off at its marker and
  back on at the End marker, and a close during it -- was not done in this round (no VM in this lane). Tests drive the
  marker through the executor's real relay and the panel through the controller; the stdout behaviour of
  powershell.exe was checked locally with a script that touches no disk.

### Fix round S: what the VM smoke found (2026-11-09)

From the VM smoke at f7619773 (`E:\SAK-Utility-Test-Scratch\vm-smoke\results.md`). Each fix has a test that went red
with its guard broken, each guard drilled on its own and restored byte for byte (26 drills on testpc2 in `W:\sak`, and
the rest in lane2; logs in `E:\SAK-Utility-Test-Scratch\fixround-s\drills\`).

- FIXED, data-loss class (S1): Reset Network left a static adapter offline. `netsh int ip reset` clears an
  adapter's static IPv4 setup; DHCP stays off, so it came back with only 169.254.x.x, no gateway and no DNS, before
  and after the restart, and nothing warned. Reset Network now reads every adapter first
  (`src/core/network_reset_safeguard.cpp`, read-only cmdlets, the persistent store and the registry: language-neutral)
  and its question names each adapter with settings typed in by hand, IPv4 and IPv6, and any turned-off adapter
  Windows does not report. Before any change it saves them to `data\network-backups` and refuses to run if it cannot.
  After the reset it puts them back, each gateway with the metric it had (automatic stays automatic, below), turns
  DHCP back on where it was on, and checks that every adapter that was up has DHCP or an address that is not
  self-assigned, with every address typed in by hand back. An adapter that fails the check is a failure that names
  the saved file. Tests: `test_network_reset_safeguard` (fixtures captured on the VM), `test_reset_network_action`
  (the whole reset against faked processes answering with the VM's output), `test_network_diagnostic_panel`.
- FIXED (S2): a reset that worked reported "failed". `netsh int ip reset` and `int ipv6 reset` exit 1 on a stock
  Windows 11 26200: 33 items OK, one protected item "Access is denied.", and "Restart the computer to complete this
  action". That reads as done with a restart; any other failed item, a timeout, or a non-zero exit with output not
  recognised (another language) is still a failure. The check waits up to 90 s for the adapters instead of failing at
  5 s, and an adapter still restarting at the limit is a restart pending with a warning, not a failure. What the check
  prints and the verdict come from one reading. Fixtures: netsh's stdout byte for byte from the VM.
- FIXED (S3): an adapter with only a 169.254.x.x address read "Connected". It reads "No valid address", with a
  tooltip that says why.
- FIXED (S4): quick actions run through the elevated helper logged "(0s)". The controller times the request to the
  helper's answer.
- FIXED (N1-N4): the Windows ISO build list is in release order, newest first; the line under it reads "No build
  selected." instead of a blank band; Refresh Disks is drawn white on its blue button (1.04:1 before in dark, 5.21:1
  or more after, both themes over 3:1); Restore Settings from the toolbar starts on the backed-up adapter's name, then
  its type, then the first, and backups record the type.
- FIXED (H1, H2, scripts): each certification group has its own job limit, ssd-device sized from its pinned drive
  (600 minutes for the 256 GB drive), hdd-device and data-device 60, and a whole-disk wipe step is sized from its
  drive; the final run runs the VM rig's groups and the physical PC's as two chains at the same time
  (`docs/PARTITION_MANAGER_CERTIFICATION.md`). Tests: `test_partition_manager_row_evidence`,
  `test_partition_manager_rig_pc_target`, `test_partition_manager_certification_chains`.
- PROVEN on the VM (`E:\SAK-Utility-Test-Scratch\fixround-s\vm\`, pictures in
  `E:\SAK-Utility-Test-Scratch\capture\fixround-s\`), each pass from `pre-round8` with the guest hygiene on: static
  10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3: the question named them; the result "Network settings reset
  successfully; settings typed in by hand were put back - REBOOT REQUIRED"; after the reset and after a restart the
  same address, gateway (effective metric 281 as before) and DNS, DNS resolving, HTTPS 200, Winsock 28 entries,
  firewall 494 to 394 rules (`proof-static\`, which pinned the captured metric, 256, before the automatic-metric fix;
  `proof-gateway-reset\` is the static pass on the final code). DHCP: the same result without the put-back, DHCP on and working after
  the reset and the restart (`proof-dhcp\`). The adapter list after `netsh int ip reset` by hand read "No valid
  address" at 169.254.193.132. The VM was left at `pre-round8`, powered off.
- FIXED, found on the way: a static gateway's metric. Measured on the VM (`fixround-s\vm\gwmetric\`): Windows' own
  setting reads route print "Default", effective metric 281, Get-NetRoute 256 and the interface's DefaultGatewayMetric
  0; `netsh ... set address ... gateway=` with no `gwmetric`, which Set Static IP sent, pins 1 (effective 26), and
  `gwmetric=0`, which Restore Settings sent, pins 0 (effective 25), so on a PC with more than one adapter either took
  the default route. Only the address set with `gateway=none` and the gateway added as a route with no metric is
  automatic, so Set Static IP, Restore Settings and the assistant's static-IP action all do that now, and Reset Network
  records whether each gateway's metric was automatic (DefaultGatewayMetric 0 beside a route metric that is not) and
  puts it back that way. Tests: `test_network_adapter_admin` (the commands sent, recorded),
  `test_network_reset_safeguard`; four drills in lane2. On the VM from `pre-round8`, each from a fresh restore: the
  menu's Set Static IP ("Static IP set on Ethernet" in the app log) left route print "Default", Get-NetRoute 256 and
  registry metric 0 (`fixround-s\vm\proof-gateway\`), and Reset Network put back the address and then the gateway
  with the same: "Default", effective 281, after the reset and after a restart, DNS and HTTPS working
  (`fixround-s\vm\proof-gateway-reset\`).
- DESIGN DECISION: the disabled-adapter case. Windows reports no settings for a turned-off adapter, so Reset Network
  names it in its question and asks the technician to note any settings by hand; it does not read them from the
  registry and cannot put them back.

### Partition Manager: the final run's launch, a verify scan that waited 36 s, and steps Windows finishes (2026-11-09)

**The launch at 74220bf9.** The final run stopped at its first real step, before any group ran: it kept the chains
script's path in `$Chains`, then wrote its plan to `$chains`, one variable in PowerShell, and pwsh was handed a
dictionary as its script. 767db1c9 fixed it, made `scripts/check_powershell_syntax.ps1` fail on any variable written
under two casings in one scope, and runs the real launch against stand-ins (AGENTS.md, "A path only `-PlanOnly` ever
ran").

**The run at 767db1c9: vhd.partition-clone-region.** Ten vhd rows passed. The eleventh's clone succeeded ("Clone
Partition from \\.\T: to \\.\PhysicalDrive2 / Running full clone verification"), and the app then said "Post-apply
verification failed: The disk scan did not finish: no progress for 30 s (last step: physical disks)". The guest's
logs (`E:\SAK-Utility-Test-Scratch\certification-final\run-767db1c9\guest_clone_evidence.log`), guest times:
- 15:24:51.6 and 15:24:57.6: SetDisk took the target disk offline and online around the raw write; U: and V:
  dismounted and mounted again.
- From 15:24:57.6 the Storage Management log shows no storage query at all until 15:25:34.0, "StorageWMI provider
  started" (WMI-Activity 5857, wmiprvse pid 4680): 36 s after the verify scan began at about 15:24:58. The step
  client's own log: its last task started at 23:25:01 UTC, and the cancel came at 23:25:29.
- So Get-PhysicalDisk, unbounded and silent, waited on the Storage WMI provider loading again after the device
  change; the same call took 31 ms twice earlier in the row, and 41-198 ms on the idle VM afterwards. TrustedInstaller
  servicing started in the same window (its start type went to auto at 15:24:56), loading the VM.

**The fix: no call in the scan waits silently or without limit.** Each slow call in the inventory and verify scan
runs bounded, says every 5 s that it is still waiting (`SAK-SCAN-PROGRESS waiting: <step>, <n> s`), and stops at
90 s (`kInventoryQueryCapSeconds`, about 2.5 times the 36 s seen), reporting "did not finish (<step>)". The 30 s idle
limit stays for a scan that truly goes silent, and the 300 s ceiling is unchanged.
- Get-Disk, Get-Partition and Get-Volume run as CIM jobs. Get-PhysicalDisk has no -AsJob (found on the VM), so it,
  the reliability counters, Get-BitLockerVolume and its module load, and the dynamic disk probe run in a side
  runspace in the same process. smartctl and fsutil say they are waiting through `-OnWait`.
- Without the disk list or the dynamic disk probe there is no scan: at their limit it ends "The disk scan did not
  finish (<step>): ...", marked did-not-finish, never "no disks". The other reads become that disk's or volume's
  error, as before.
- A call left waiting holds its runspace, and an ordinary exit then waited on it (7 s for a stand-in sleep; for a
  call that never answers, never). A scan that left one waiting ends its own powershell.exe with TerminateProcess;
  the app and the helper each start the scan as a child powershell.exe, so nothing else is touched.
- On the VM, read-only, the bounded scan and the 767db1c9 scan gave the same inventory twice each (one disk, four
  partitions, three volumes, media type, reliability, BitLocker and dirty bit all known), in 5 s against 4 s.
- Tests: `inventoryScan_aSlowCallThatSaysItIsWaitingFinishes` (a job, a runspace call and a program, each slower
  than the idle limit), `inventoryScan_aCallPastItsOwnLimitSaysWhichStep` (a stuck runspace call in a .NET wait no
  pipeline stop can break, and a stuck job: did-not-finish by step, ended in under 9 s),
  `inventoryScan_aWaitingLineKeepsItsStep`, `inventoryScan_everySlowCallIsBoundedAndNamed`. Ten drills, each red:
  the three heartbeats, the two limits, the hard stop, the did-not-finish exit code, the waiting line's step, and the
  physical-disk and BitLocker bounds.

**testpc1: the SSD wipe outlived its app.** The run's physical-pc chain was stopped at 15:35 on the coordinator's
word, 29 minutes into `external.ssd-secure-erase`. Its full format kept writing at about 19 MB/s as a Windows
"Format Volume" storage job inside smphost after the step client, the helper and the scheduled task were stopped;
Stop-Service sat in Stop Pending, and ending smphost's own host process (pid 15448, `svchost.exe -k smphost`, hosting
smphost alone) stopped it at 14%. The NVMe was left GPT with its MSR and a 256,043,384,832-byte basic partition with
no file system; the next run's first device row clears the disk.

**Steps Windows finishes after a kill.** Measured on the VM, each step kind in its own powershell.exe killed
mid-way: Format-Volume -Full, Optimize-Volume -Defrag (and -ReTrim, the same job kind), a Resize-Partition shrink
(no storage job at all) and Repair-Volume -Scan all ran on to completion; cipher.exe /w and convert.exe stopped at
once. The app's Cancel had stayed enabled through all of them and reported "cancelled".

**Owner ruling, tonight only (2026-11-09).** Two build lanes: lane2 and lane3 each at `/m:4` and `ctest -j 3` while
both are active; the final gate at `/m:8` and `-j 6` with lane3 idle; lane3's worktree and build directory removed
after the fold. (Recorded here: `AGENTS.local.md` lives in the main worktree, which this run does not write to.)

**Live checks on the VM, through the app and its elevated helper**
(`E:\SAK-Utility-Test-Scratch\certification-final\live-check\`). On a 48 GiB throwaway disk, during Wipe Disk's 79 s full format the ribbon Cancel was disabled with the
notice "Wipe Disk - Disk 1: Windows is carrying out a full format (47.98 GB). This step cannot be stopped once it has
started, and Windows continues it even if S.A.K. Utility is closed. Started at 8:59 PM. No time estimate is
available."; the close prompt said closing would not stop it; the wipe completed. A cancelled Wipe Free Space reported
"Removed after the stop: cipher's temporary folder W:\EFSTMPWP." and the folder was gone. That pass found four faults,
fixed and checked again on the VM (`recheck\`): Wipe Disk failed on a RAW disk ("Clear-Disk : The disk has not been
initialized"), and now starts from Initialize-Disk; the Log window showed the internal markers, and now shows none; a
"complete" result could open over a close prompt still saying "running", and the prompt now rewrites itself at the
phase's end and closes with the run; and the prompt read "Partition changes is running", now "Still running: Partition
changes." for every caller. A cancelled step now reads "Cancelled:", not "Failed:". By design, unchanged: a cancelled
operation stays in the queue as pending, to be applied again or removed, and does not read as done.

**For after 0.9.3.**
- Show the full format's percent and an estimate by polling Get-StorageJob for its job from a side runspace. Tonight
  the panel says "No time estimate is available": the honest alternative, Format-Volume -AsJob, would change how a
  destructive step reports its errors.
- The assistant's apply-result path (`cancelledApplyResult` in `app_mutating_actions.cpp`) has no unit test.
- The marker strip on the elevated path has no direct test; the VM re-check's Log window is its only proof.
- A shadow copy that is created but cannot be read back is left behind by a step that succeeds (logged as INCOMPLETE in this file).
- The step client writes the storage objects it reads into its reports as Windows gives them, and each carries the
  local computer name: `"ServerName":"<name>"` and the host part of its ObjectId (`{1}\\<name>\root/...`). The
  certified evidence at ab9cc504 holds the lab VM's random name (DESKTOP-EB5HH3F, 50 times in 6 files) and the label
  TESTPC2 (4 times, `external.usb-removable`), accepted for 0.9.3 (no domain, user or address). The step client should
  drop ServerName and the ObjectId's host from what it serialises; and every future run's PcFile should list the PC's
  own computer name in `deny_names`, so selection refuses evidence that carries it.
- Decide whether release signing should sign the third-party GPL filesystem tools at all. `release.yml` signs every
  unsigned .exe and .dll, which changes the bytes of the e2fsprogs and hfsprogs binaries and hfsprogs' msys-2.0.dll
  that `tools/filesystem/manifest.json` pins and the application checks before it runs one ("Filesystem tool hash
  mismatch"). For 0.9.3 the release workflow re-pins the PACKAGE's manifest to the signed bytes, publishes the
  upstream-to-signed mapping (`filesystem-tools-signed-sha256.txt`), and asks the application's own manifest
  validation about the package before and after signing (`.github/release-tools/`). The alternative: the signing catalog
  skips manifest-pinned files and `verify_authenticode_signatures.ps1` accepts an unsigned file whose hash matches
  its pin, so the shipped manifest stays the repository's. That is a `scripts/` change, which the certification
  digest covers.

### Fix round N: the first nightly and the first release dry run (2026-11-10)

The release workflow and the nightly had never run (they can be dispatched only from `main`). Their first runs on
`main` 4f7daa47 found what follows; the owner ruled to fix all of it before release and certify the result again.
Each fix that changes behaviour has a test that went red with the fix taken out, each drilled on its own and restored
byte for byte; a deletion is proved by the build and both clang-tidy passes.

**Release dry run 42913905285.** The readiness gate refused the certification evidence ("the evidence is for other
code"): on the runner, `bundle_smartmontools.ps1` judged the tracked, correctly pinned smartctl.exe untrusted,
downloaded smartmontools again and rewrote the tracked `tools/smartmontools/README.txt` without its byte-order mark,
which moved the certification digest. The cause: it read `$LASTEXITCODE` after `& smartctl --version | ... |
Select-Object -First 1`, and PowerShell 7 does not record the exit code of a native command whose pipeline
Select-Object -First stops, so in a new session it is `$null` and `$null -ne 0` is true (measured: 40 of 40 fresh
pwsh 7.6.9 sessions; Windows PowerShell 5.1 sets 0). The application runs smartctl through QProcess and its own
scripts read `Process.ExitCode` under Windows PowerShell 5.1, so no customer path has it.
- `bundle_smartmontools.ps1` captures the output first and then the exit code, says which check failed, and leaves a
  file alone when its bytes or its text are already right (README in the tracked encoding). Test
  `test_bundle_smartmontools` (T2, offline: a pinned bundle in a new pwsh 7 session is accepted, nothing downloaded,
  no file rewritten) and `test_bundle_smartmontools_rebundle` (T4, downloads: `-Force` rewrites no file). Three
  drills red: the old check, the old README write, a plain copy over an identical smartctl.exe.
- `check_powershell_syntax.ps1` (T1 on changed scripts, T2 on the whole tree) now refuses `$LASTEXITCODE` read after
  a native command's pipeline is cut short by Select-Object -First or -Index, with no other native command between.
  Its only hits in the tree were the two in `bundle_smartmontools.ps1`. `test_powershell_syntax_gate` (pwsh and
  Windows PowerShell) covers five refused shapes, five passed ones and the two-casings rule, which had no test; five
  drills red.
- `release.yml` bundles smartmontools and iPerf3 into the runner's temp folder and compares them with the tracked
  binaries, and checks after bundling and before readiness that no step changed a tracked file (#3).

**Nightly 42913851120.**
- clang-tidy naming gate: clang's own diagnostics stopped it, and a local run of both passes the way the nightly runs
  them found more than CI had reached (the gate stops at its first failing pass). Missing designated fields
  (verify_system_files_action.cpp, process_runner.cpp, diagnostic_benchmark_panel.cpp, network_diagnostic_panel.cpp,
  user_migration_panel.cpp) are named; unused constants (organizer_panel.cpp, windows11_theme.cpp,
  network_reset_safeguard.cpp, pst_named_properties.cpp, uup_dump_api.cpp, uup_iso_builder.cpp,
  windows_iso_downloader.cpp) are deleted; an unneeded lambda capture (partition_operation_requests.cpp) is gone;
  `clonePreviewScaledBytes` and the one-floor `segmentSpans`, called only by the panel's test hooks, are deleted and
  the hooks call what production calls; three names follow the naming rules (`kDefenderTasks`, `theme_options`,
  `kInstance`; `theme_options` now takes its length from its entries, as the magic-number gate wants of a constant
  without the k prefix), and so do two static regexes in `thermal_monitor.cpp` (`kSafeIdentity`, `kDiskStatus`), a file on
  the core pass's must-stay-clean list that CI had never reached.
- Mutation catalogs: five mutants whose text had moved are retargeted (restore-image payload target, the APFS resize
  wording, SMART's warning boundary and NVMe media errors) or removed (`critical-threshold-boundary-ge-to-gt`: SMART no
  longer has a critical raw threshold), and `clone-preview-scale-wraps-product` moved into `saturatingMultiplyDivide`
  with the wrapper it named.
- `test_false_green_command_propagation`: `(Get-Command git.exe).Source` is an array where PATH holds two git.exe, as
  on the runner; reproduced locally with two on PATH and fixed with `-TotalCount 1`. The same lookup is hardened in
  `generate_compile_commands.ps1`, `test_startup_budget.ps1` and the toolchain action.
- `test_source_fingerprint`: the runner's TEMP is under an 8.3 short name (RUNNER~1), so the root arrived short while
  CMake named each input long, and every input read as outside its root. `verify_source_fingerprint.py` expands short
  names with GetLongPathNameW (names only; reparse points are still refused) before comparing. A new case checks both
  spellings each way; drill red.
- Debug + AddressSanitizer: no AddressSanitizer report. `test_network_reset_safeguard` called `error()` on a
  `std::expected` holding a value, because Qt 6.12's QVERIFY2 evaluates its message even when the check passes; it now
  uses the guard the other nine such calls use. The two MCP session suites each failed their built-in case with
  "Built-in MCP app is missing": it starts sak_utility.exe from the test's own folder, and in Debug the tests and the
  application build to different folders; `sak_stage_tools_for_debug_tests` now stages it (in Release they share
  one, and on a workstation the hardware guard skips the case, which is why only CI saw it). Three suites crossed
  limits set against Release: `SAK_TEST_TIMEOUT_SCALE` multiplies every TIMEOUT by 5 in the ASan job only (refused
  at configure without ENABLE_ASAN), measured in `tests/CMakeLists.txt`. The job now keeps a crash dump of any test
  that ends abnormally, prints a debugger stack for it, and uploads the QTest result files, none of which it kept
  before; the first run's three failures carried no detail.
- The explorer suite's Debug assert (in `openFolderInSecondPaneListsThatFolderInPaneB`, CI only, three runs of three)
  was `QList::takeFirst` on an empty list in Qt's `QHeaderViewPrivate::resizeSections`. The header's layout pass
  queues one width per column shown when it starts, then emits `sectionResized` as it goes, also for a column hidden
  since the last pass, which still holds its width. `FileExplorerDetailsView` refitted from that signal and showed
  columns it had dropped for room, so the pass met a shown column it had queued no width for: an assert in Debug,
  undefined behaviour in Release. The refit now runs once, queued, after the pass has returned. Two Release tests
  force the pass with the view's own calls (the splitter's narrow-then-wide resizes, and the header menu hiding a
  column in a narrow pane) and check that no emission changes which columns show: both red on the old code, green
  three runs of three on the fix. On CI, the nightly's ASan build of the explorer suite with the fix (temporary branch
  probe-asan-explorer, run 42948322870) passed fifteen runs of fifteen, 124 cases each with no crash dump, where it
  had asserted on every run before; the branch and its runs' artifacts are deleted. A sweep of the GUI found no
  other slot on a header signal or on a view's resize that shows or hides a section or changes a resize mode.
- `test_dedicated_threads` failed once on unchanged code under a -j 4 T2 shard: it times the shared thread pool while
  its own spinners hold two cores. It now runs alone (RUN_SERIAL) and prints the latency it measured. With the jobs
  put back on the shared pool the probe never started within its 3000 ms (three of three); on threads of their own
  it started in 0-100 ms, so its bound is 2000 ms, not 1000: a busy machine gets twenty times that, still short of
  "never". Thirty runs beside the full T2 label set at -j 6: 30 passed, 0-237 ms; again on the round's final tree,
  30 passed, 0-989 ms.
- Added by the owner: the selected main tab drew its icon as a flat white silhouette (`tabIcon`'s QIcon::On
  pixmap, UI review S25), which lost every line inside the drawing. It now shows the icon's own drawing. Those colours
  fail on the old blue fill (in the light theme 0-15% of an icon's pixels at 3:1), so the selected tab is filled like
  the page under it (light #ffffff, dark #172033) with a 2 px accent outline; every tab has the 2 px border,
  transparent until selected, so no tab moves. `selectedTabIconsKeepTheirDrawing` selects each tab in both themes and
  fails on an icon reshaped or in fewer colours than its own drawing, and on the 3:1 rule read on the real fill: red on
  all 22 before, green after (weakest: light Application Management 0.59 of its pixels, light Partition Manager's
  weakest quarter 0.46).

### The About tab's testing line (2026-11-11)

The screenshot recapture after 18614fca found the About tab still saying "Partition Manager has not yet been tested
through the app on real hardware. Back up first." (`src/gui/main_window.cpp`), false since the certification tested
it on two real disks. On the owner's ruling its Testing line now quotes the generated "What the site may say" block of
`docs/PARTITION_MANAGER_CERTIFICATION.md`: all six sentences, so the limits (internal SATA drives were not tested;
SSD erase is TRIM plus overwrite, not an ATA or NVMe sanitize) and "Back up before changing a disk." are in it.
`aboutTestingTextQuotesTheCertification` (test_main_window_help) reads that block from the document at test time
(`SAK_CERTIFICATION_DOC`) and fails when an About sentence is not one of the block's in its order, when a limit (a
sentence with "not") or the "Back up" sentence is left out, or when "Apple-certified" appears. Red on the old line,
red with the sanitize limit dropped and with "Back up" reworded, green on the new line. When new evidence changes
the block, the test fails until the About line follows it.

A sweep of every user-visible string in `src`, `include` and `resources` (the About, Help, licence and credits
HTML, tooltips and status text, the wiki link, the AI assistant's instructions, skills, workflows and tool
descriptions) for "not yet tested", "real hardware", "certif", "uncertified", "HardwareCertified", "VM-only",
"tested", "experimental" and the like found no other stale certification claim. "certification" is left only as an
internal sidebar key (shown as "Writable Mac Disks") and in code and comments, and the APFS safety note's "validated
with Apple's own tools" says what the block says.

Partition Manager's file-system registry (`src/core/partition_file_system_registry.cpp`), shown in Properties and
Inspect as "Blocked filesystem actions" and "Required bundled tools", was checked entry by entry against what ships
(`tools/filesystem/manifest.json`: e2fsck, mke2fs and resize2fs for ext2/3/4, newfs_hfs and fsck_hfs for HFS+/HFSX;
`scripts/stage_portable_release.ps1`: sak_apfs_writer_cli and sak_hfs_writer_cli). On the owner's ruling:
- XFS read "Format and repair remain blocked until the bundled tools have been tested" and listed xfs_repair as a
  required bundled tool; Btrfs read "Deep btrfs check requires manifest-approved btrfs" and "Repair remains disabled
  until destructive VM and lab testing proves safe scenarios" and listed btrfs. Neither tool ships. Each now reads
  "<XFS|Btrfs> format, repair and the deep check (<tool>) are not available because S.A.K. Utility includes no
  <XFS|Btrfs> tools", with no required tool.
- HFS+/HFSX: "... remain blocked pending testing of each operation" and "... remain blocked in this milestone" now
  say the same operations "are not available".
- Linux swap: "resize remains partition-only until operation-specific proof exists" now reads "resize changes only
  the partition, not the swap area inside it".
- ext2/3/4 named other file systems' writes ("XFS/Btrfs/APFS write workflows and HFS+ file writes are not enabled by
  ext tool approval", two of which exist now); it now says what is blocked for ext: "Writing files inside an ext
  volume is not available: S.A.K. Utility reads and copies out ext files only".
- Unchanged, as true and promising nothing: NTFS, FAT12/16/32, exFAT (none blocked, no tools), ReFS (no entry: the
  generic "no registered filesystem workflow"), APFS (its size, Fusion, sealed, case-sensitive and locked-volume
  limits; sak_apfs_writer_cli ships) and the unidentified entry.
`fileSystemRegistry_requiresOnlyToolsThatShip` (test_partition_manager_core) reads the manifest and the staging
script at test time and fails when an entry names a tool that does not ship for it, when a manifest tool is missing
from an entry it serves, or when a blocked action promises a future ("until ... tested", "pending testing",
"milestone", "yet") or an approval. The registry pins and the XFS Properties/Inspect rows in test_partition_manager_panel
are updated. Drills: the whole old registry, xfs_repair alone put back, and the swap promise alone put back each
turned the tests red.

The 317211fc dev nightly (43047447744) failed its T4 CTest labels job on two tests:
- `test_linux_iso_catalog_live`: "HTTP 0" from releases.ubuntu.com for the Ubuntu 26.04 desktop and server ISOs.
  That was Canonical's outage, not ours: from the workstation the same hour, two of the host's three addresses
  (185.125.190.37 and .40) did not answer on 443, while 91.189.91.108 answered 200 for both ISOs. Left as it is.
- `test_process_capture_output_windows_powershell`, case stdout-exact: "Exact-boundary output child failed (expected
  '0', got '1')". The Python fixture (`tests/support/process_capture_deadline_fixture.py`) waited 2 s for the outer
  test's acknowledgement, while the outer side (`scripts/test_process_capture.ps1`) gives itself 10 s to produce it,
  so a slow Windows PowerShell 5.1 outer side under T4's load lost the race and the fixture exited 1. Each fixture
  wait now outlasts its outer window by 5 s: 15 s for the output acknowledgements (outer 10 s), 35 s for the
  deadline cases' bound.ack (outer 30 s watchdog), and the self-expiry watchdog 45 s (the largest receipt delay, two
  output acknowledgements and a margin), reasoned at the constants. Drill: with the old 2 s wait and the outer side
  held 3 s before output-ready.ack, stdout-exact failed with the nightly's exact message (and minimum-exact on the
  fixture's traceback); with the fix, both passed, as they do without the delay. All twelve process-capture tests
  pass in both shells, and the Windows PowerShell output test passed 20 runs of 20 (17-27 s each) beside two
  back-to-back runs of the full T2 label set at -j 6 (322 of 322 both times). The 317211fc nightly's ASan job
  passed, 297 of 297 with no crash dump.

The 77a92478 dev nightly (43065407990) failed its T4 CTest labels job on
`test_source_fingerprint::test_header_change_reconfigures_on_build` (the stamp did not change after a header edit and
`cmake --build`), passed by the two nightlies before it. Measured with the same fixture: MSBuild's ZERO_CHECK runs
CMake again only when an input is at least a millisecond newer than `CMakeFiles/generate.stamp`; an input dated 1 ms
before it, equal to it or 100 ns after it went unnoticed (3 of 3 each), 1 ms to 2 s after it was noticed (3 of 3
each). Windows dates a write by its clock tick, so the test's edit, written the instant configure returned, could
tie the stamp. The test now dates the edit one second past the newest configure output (no sleep); drilled red 3 of
3 with the edit dated to tie the stamp, green 3 of 3 with the fix.

To stop finding one intermittent failure per nightly, a flush on a temporary branch (probe-flush, run 43072540193,
77a92478 plus one workflow file, since deleted) ran the nightly's T4 CTest labels job as six parallel copies with
each test repeated up to three times until it failed, the T4 static job once, and the eight T2 build + test shards
with each test repeated up to twice. T4 static and all eight T2 shards passed. Two tests failed, nothing else:
- the fingerprint case above, in four of the six copies (fixed above);
- `test_linux_iso_catalog_live`, once in copy 6: Fedora's redirector gave no answer in 30 s; it passed in every
  other copy. The second upstream outage in a day after Canonical's, so on the coordinator's ruling the live check
  now asks a URL a second time, after 5 s, only when the first try got no HTTP answer at all (a timeout or status
  0), and logs each retry in the test output ("upstream retry: ...") so a flaky upstream stays visible. Any HTTP
  status is final on the first answer, so a dead address (404) or a refusal (403) still fails at once.
  `catalogDownloadCheckRetriesOnlyTransportFailures` (T2, offline) scripts the answers: no answer then 200, and
  HTTP 0 then 206, pass with one logged retry; 200 passes at once; 404 and 403 fail on the first answer; no answer
  twice, HTTP 0 twice, and no answer then 404 fail. Drills: one try only, retrying every status, and a third try
  each turned it red.

### Security round: an administrator's S.A.K. on a drive other accounts can write to (2026-11-11)

Owner rulings this round answers (2026-11-11): fix every app-side item of the security brief before 0.9.3; S.A.K.
runs as administrator straight from its folder, often a flash drive ("it needs to run securely from the flashdrive
with nothing copied to the user's machine"), and "the temp files cannot live on a user's pc not even temporarily
unless it's in the app directory". Items 5 (iPerf3, LAN Transfer, the browser bridge's hello frame) and 6 (treating
the remote side as hostile), `scripts/sign-exe.ps1` and `requirements-gates.txt` were done in a parallel round; their
entries are theirs. The one item of 5 done here is the elevated pipe's per-frame deadline.

One shared primitive carries the C++ half: `sak::no_follow` (`include/sak/no_follow_fs.h`). A `HeldDirectory` is
opened without following a link, checked to be exactly the folder named (no link anywhere from the drive or share
root down), and held without FILE_SHARE_DELETE, which pins it and every folder above it (measured: renaming an
ancestor of a held folder is refused). A `StagedFile` is created CREATE_NEW under a random name in a held folder and
published by a rename relative to the held folder's handle (NtSetInformationFile; kernel32 refuses a FILE_RENAME_INFO
with a RootDirectory, error 87, measured). `removeTree` removes a link as itself and never descends into it.
`writeHeldReadOnlyFile` writes a new file, then holds it open for reading, sharing reading only, and reads it back.
The PowerShell half is `sak_script_types.dll` (`Sak.SafeFiles`, `Sak.ShadowMapping` and the types the scripts used
to compile with Add-Type), built from `src/core/partition_scripts/sak_script_types.cs` and loaded from its bytes. The
durable rule is in AGENTS.md ("Elevated code never follows a link, and nothing of S.A.K.'s lives outside its
folder").

| Item | What was wrong | Fix | Test | Drill (red, restored byte for byte, green) |
|---|---|---|---|---|
| 4a.1 | The run-temp cleanup (at exit and of earlier runs' folders) used `QDir::removeRecursively`, which in Qt 6 follows a junction: a junction left in `data\temp` turned an elevated cleanup into a recursive delete of whatever it named (drilled: the canary was deleted). | `no_follow::removeTree` removes a link as itself and never descends into it; earlier runs' folders are removed through the held `data\temp`. | `test_no_follow_fs` removeTree cases; `test_config_manager::staleRunFoldersNeverFollowAJunction`; VM `ntfs-stale-junction` | primitive drills; `4a-run-dacl`, `4a-temp-processwide` |
| 4a.2 | The elevated helper and its scripts worked in `<program>\data\temp` with fixed or guessable names any account that can write the drive could pre-empt. | Every run (the app, unelevated too, and the helper) makes its own `data\temp\run-<pid>-<128-bit random>`: CREATE_NEW, held, never a link, protected DACL on NTFS (SYSTEM and Administrators, plus the person when not elevated); TEMP and TMP point there for the whole process and what it starts; removed at exit and at the next start if a run was killed. Nothing goes to %TEMP% or C:\Windows\Temp (owner ruling). | `test_config_manager::runTempIsProcessWideAndProtected`; VM: owner Administrators, protected, SYSTEM and Administrators only; a limited token's junction, file and rename in it refused | `4a-run-dacl`, `4a-temp-processwide` |
| 4a scripts | diskpart scripts, APFS/HFS+ payloads, the FileVault password, Wi-Fi profiles: files another program reads by name, written in place where they could be swapped before the reader opened them; Add-Type compiled C# into the temp folder and loaded it; the shadow-copy link was an `mklink /D` in the temp folder. | Each such file is written CREATE_NEW and held read-only (no writer, no delete) until its reader is done (`writeHeldReadOnlyFile`, `writeHeldSessionTempFile`, `[Sak.SafeFiles]::WriteHeldReadOnly`); measured that diskpart (`_wfopen`) and netsh read through the hold. The scripts' C# (including the storage scan's dynamic-disk probe, which the scan compiled even unelevated) ships as `sak_script_types.dll`, pinned by hash, loaded from its bytes. The shadow copy is reached through a drive letter `DefineDosDevice` maps in the helper's logon session, removed in the same `finally` and at the helper's next start if a run was killed. | `test_no_follow_fs::heldReadOnlyFileOpensForReadersAndRefusesWriters`, `test_script_types` (6), `test_partition_manager_core` (diskpart, payload, credential, shadow, `stopCleanup_onlyShadowCopyDevicesAreStaleMappings`), testpc1 netsh measurement | `4a-diskpart-hold`, `4a-held-reopen-share`, `cs-holdreadonly-share`, `cred-hold`, `cred-removed`, `4a-shadow-device` |
| 4a UUP | The Windows-via-UUP workspace had a deterministic name in `data\temp` and was reused as found. | Reused (so a download resumes) only when it is one S.A.K. made: on NTFS owned by Administrators or SYSTEM with a protected DACL; on FAT, where nothing can be told apart, every file is hashed again before it counts. Otherwise removed (a link as itself) and made new. | `test_uup_iso_builder::workspaceThatIsAJunctionIsRemovedNotFollowed`, `foreignWorkspaceIsNotReused` | `4a-uup-foreign` |
| 4a.3 | The helper's logs in `data\logs` had predictable names opened for append, so a planted link redirected an elevated write. | The log folder is held without following a link; each log is created CREATE_NEW and held deny-write for the session (`FileLogSink`). | `test_logger::logFolderThatIsAJunctionIsRefused`, `openLogDeniesOtherWritersButNotReaders`; the TSan row uses an in-memory sink (`test_logger_concurrency`) | `4a3-logger-folder`, `4a3-logger-share` |
| 4b B1 | File Organizer created category folders and renamed files into them with no link check. | Category folders are opened strictly below the held target folder; each move is a rename relative to the held folder, replacing only on "overwrite". | `test_organizer_worker::junctionCategoryFolderIsRefusedAndItsTargetUntouched`, `junctionCategoryFolderReceivesNoNewFile` | `B1-organizer` |
| 4b B2/B3 | Backup files staged at the predictable `<dest>.sakpart` opened with Truncate; the destination opened with Truncate; the profile backup's pass-through copy and the restore's copies and renames used `QFile::copy`/`rename`, which follow a link (even a dangling one). | `StagedFile` under a fresh random name in the held folder, published by rename; `copyFileTo`/`moveTo` everywhere in profile backup and restore. | `test_backup_file_codec` (3); `test_user_profile_backup_worker::backupNeverWritesThroughAJunction`; `test_user_profile_restore_worker::restoreNeverWritesThroughAJunction`. The drills found that both junction tests pass with `QFile::copy` put back: each worker refuses a link in the destination's folders before any copy, so what the no-follow copy closes is a link planted at the file's own name after that check, which no test can time. `backupCopiesWithNoLinkFollowingCall` and `restoreCopiesWithNoLinkFollowingCall` (`tests/support/link_following_calls.h`) now fail on any Qt copy or rename in either worker's source. | `B2B3-codec-revert`, `B3-backup-copy`, `B2-restore-copy` (red only after those two tests) |
| 4b B4 | Partition move/backup ran `New-Item -Force` then `robocopy /MIR` into the chosen root. | `New-SakBackupFolder` (no `-Force`, then held); robocopy `/E /XJ`, never `/MIR` into a chosen root. | `test_partition_manager_core` (backup script assertions); one copy of these functions now (the move/backup and the manifest scripts had two) | `B4-mir`, `B4-new-folder` |
| 4b B6 | Create Image opened the destination file with `FileMode::Create`. | The folder is held and the file made with `[Sak.SafeFiles]::CreateReplacing`: an ordinary file is replaced by name, a link refuses. | `test_partition_manager_core::scriptBuilder_buildsCloneVerificationScript`; `test_script_types::createReplacingRefusesALinkAndReplacesAFile` | `B6-createreplacing`, `cs-holddirectory` |
| 4b B7 | Email export wrote attachments with `QFile::open(WriteOnly)`. | The output folder is held; each file is a new file in it. | `test_email_export_worker::sidecarAttachmentsNeverFollowAJunction`, `junctionOutputFolderIsRefused` | `B7-email-revert` |
| 4c | An elevated S.A.K. ran whatever program files sat in its folder, often a flash drive other accounts can write to; elevated children searched the current folder and PATH. | Before anything runs as administrator (Run as Administrator, the helper, and an elevated start itself), every .exe and .dll in the folder (not `data\`) is opened, held deny-write/deny-delete for the elevated side's life, and accepted only by (i) its pinned path and SHA-256 (the build's manifest; a shipped file missing from it fails `everyShippedFileTheBuildCopiesIsPinned`), (ii) the running program's own signature, or (iii) on an unsigned build, the three first-party programs by name; anything else refuses, naming the file. Elevated processes: `SetDefaultDllDirectories`, `SetDllDirectory("")`, Qt plugins only from the program folder, a module audit after start, current directory System32, `NoDefaultCurrentDirectoryInExePath`, and `/DEPENDENTLOADFLAG:0x1000` on all four programs. | `test_program_integrity` (13), `test_elevation_hardening` (2), `test_process_runner::elevatedChildStartsInSystem32WithoutCurrentDirectorySearch`; VM: planted DLL refused on NTFS, FAT32 and exFAT | `4c-rule-iii`, `4c-pin-hash`, `4c-broken-signature`, `4c-tree-junction`, `4c-hold-share`, `4c-audit-program`, `4c-audit-roots`, `4c-broker-verify`, `4c-restart-verify`, `4c-child-env`, `4c-child-cwd`, `4c-dependentloadflag` |
| 4d | No check of who signed the helper before launching it. | Same signer as the running program: a trusted chain needs the same subject and EKUs; an untrusted one the identical leaf certificate. | `test_program_integrity::sameSignerNeedsTheSameIdentityOrTheSameCertificate`; VM on a test-signed package: clean elevates, an unsigned helper and one signed by another publisher refuse | `4d-ekus`, `4d-leaf` |
| 5 (pipe) | The elevated pipe read each chunk with its own timeout, so a sender trickling bytes held it indefinitely. | One absolute deadline per frame (`readPipeBytesBefore`). | `test_elevated_pipe_protocol::dribbledFrameStopsAtItsOneDeadline`, `wholeFrameInTimeIsRead` | `5-frame-deadline` |
| FAT | Found by the VM proof: FAT32 and exFAT refuse `FileAttributeTagInfo`, which every no-follow check read, so a clean package on such a drive would not run as administrator at all. | Refused tag info falls back to `FileBasicInfo` with tag 0 (C++ `attributesFromAnswers`, C# `RequireOrdinary`); a reparse attribute with no readable tag still refuses. | `test_no_follow_fs::attributesAreReadOnEveryFileSystem`; `test_fat_volumes` (CI elevated step: real FAT32 and exFAT VHDX; its set-up has not passed yet, see the VM table); VM | `fat-attributes` |
| HFS+ | Since 2026-08-31 every raw-device HFS+ staging script failed at `New-SakSparseImageFile` (`[ref]$Owned` no caller passed); and on a FAT program drive staging cannot work. | The unused `[ref]$Owned` removed (a first fix made it optional with a `$null` default, which does not bind either: the new caller test caught that, and the VM showed the original failure through the app, `shots\hfs-format-live-failure-before-fix.png`); `Assert-SakStagingVolume` refuses a non-NTFS/ReFS drive and a short one before the target is read. | `hfsStaging_everyCallerRunsTheStagingFunction`, `verifyHfsStagingVolumeCheck`, `generatedScripts_callTheirOwnFunctionsWithDeclaredParameters` (every generated script's own-function calls: declared parameters, every [ref] passed) | `hfs-staging-volume`, `hfs-staging-owned`, `guard-undeclared-param`, `guard-unpassed-ref` |
| HFS+ tool runner | Found by the VM proof once staging worked: HFS+ Format through the app failed with "Cannot convert the "System.Object[]" value of type "System.Object[]" to type "System.Int32"". `Invoke-SakCheckedHfsTool` passed the tool's output down the pipeline and then returned its exit code, so every caller got an array. Present since the 2026-06-13 base (`c4cdefc6`): together with the rows above and below, the app's HFS+ Format and Repair have never worked. | The tool's output goes to the console (`[Console]::Out.WriteLine`); the function returns only the exit code. | `hfsStaging_repairReadsTheToolsExitCode` (runs the script's own function against a stand-in tool) | `hfs-tool-runner-output` |
| Raw partition writes | Found by the VM proof: Format to APFS of an NTFS partition failed through the app ("Unable to zero APFS format target: Unknown error"), and HFS+ Format of one with "Access to the path is denied." (`shots\hfs-format-live-failure-access-denied-b21.png`). Windows mounts the file system it knows (NTFS, FAT32, exFAT) when a partition is opened, drive letter or not, and the scripts dismounted only a lettered volume. Through that file system a write into its own sectors is refused, and one over the partition's last sectors comes back short with no error (measured: 1,044,480 of 1,048,576 bytes, also after a lock and dismount without extended DASD access). Present since the 2026-06-13 base (`c4cdefc6`): every test and certifier wrote image files or partitions Windows had no file system on. | A writable partition handle takes the volume first: extended DASD access, `FSCTL_LOCK_VOLUME` (refused while anything else has the partition open: "The partition is in use, so Windows will not hand it over: close File Explorer windows and programs using it, then try again"), then `FSCTL_DISMOUNT_VOLUME`. The C++ writer (`WindowsRawDevice`: the APFS writer and the HFS+ file writer) and the scripts' `Sak.RawPartitionWrite` (the HFS+ staged copy) both do it; a short raw write now says so instead of "Unknown error". | `test_raw_partition_write` (CI elevated step; NTFS, FAT32 and exFAT VHDX: first and last MiB through both writers, read back; both refuse a partition in use; its set-up has not passed yet, see the VM table); the same checks as guest scripts on the VM; `rawDeviceClassifier_canonicalizesExtendedDeviceSpellings`; `test_script_types::rawPartitionWriteTakesOnlyPartitions`; HFS+ script assertions | `raw-partition-classify`, `hfs-copy-takes-partition`, `cs-rawpartition-ispartition`; on the VM `raw-partition-take`, `cs-rawpartition-take` |
| Script loader | Found by the VM proof: with the partition lock in, HFS+ Format stopped at "Unable to find type [Sak.RawPartitionWrite]." (`shots\hfs-format-live-failure-unloaded-type-b22.png`). The HFS+ staging scripts never loaded `sak_script_types.dll`; the guard checked only that each type a script names is in the DLL. Extended, the guard also caught this round's own payload holds (the APFS root-file and HFS+ payload scripts named `Sak.SafeFiles` without loading it) before any build left the tree. | The HFS+ staging functions and both payload scripts load the DLL first; the guard now refuses a script that names a type without loading the DLL before it (outside a function body, in text order). | `generatedScripts_callTheirOwnFunctionsWithDeclaredParameters` (load before use) | `guard-unloaded-type` |
| PowerShell module path | Found when the tool-runner test ran under the T2 runner: Windows PowerShell 5.1 started with PowerShell 7's `PSModulePath` finds 7's `Microsoft.PowerShell.Utility` first and has no `Get-FileHash` (measured), which the HFS+ tool check and other partition scripts call. S.A.K. started from a PowerShell 7 window, and CI's runner, passed that path to every script. | A Windows PowerShell child gets no `PSModulePath` and builds its own (`process_runner`, every launch through it); other children keep it. | `test_process_runner::runPowerShell_childBuildsItsOwnModulePath`; `hfsStaging_repairReadsTheToolsExitCode` under the T2 runner | `ps-modulepath` |
| APFS repair offer | Found by the VM proof: for a container the app had just formatted, Check Non-Windows File System showed only the read-only metadata. The panel offered "Queue generated APFS repair" only for one fixed layout (space manager block 10, volume block 6, OIDs 102-104) that the writer no longer produces at 256 MiB. From the 2026-06-13 base (`c4cdefc6`); the tests fed that text, never a container the writer made. A first fix offered it for any container the writer formatted; the b28 VM rerun then showed the option and failed at Apply ("APFS writer helper failed with exit code 1"), because `repair-raw` handles only one space-manager chunk (`appendGeneratedGeometryBlocker`: 4096-byte blocks, at most 32,768 blocks, 128 MiB). | The scan reports who formatted each APFS volume ("APFS volume formatted by: S.A.K. Utility APFS writer", from `apfs_formatted_by`); the panel offers the repair only when that is the writer and the scanned block size and count pass `PartitionApfsWriter::repairGeometryBlocker`, the same function the writer's guard now calls, so the two cannot drift. Otherwise the metadata dialog's "Repair" row says why: "S.A.K. can repair only APFS containers it formatted itself, up to 128.0 MB. This container was formatted by something else (a Mac, for example), so only the read-only metadata check is offered." or "... This container is 256.0 MB, so ...". | `test_partition_manager_panel` (containers the writer really formats, read by the production detector: 64 MiB offered; 256 MiB and a foreign formatter get the metadata dialog with those exact words) | `apfs-repair-gate`, `apfs-formatter-detail`, `apfs-repair-geometry`, `apfs-repair-reason-row` |
| Running image | Found by the operation-proof agent on testpc2: the certification step client (`partition_manager_live_certifier`) never recorded its own program file at start, so the 4c check had nothing to compare with and refused every elevation it asked for, saying a file in the folder was not part of S.A.K. | The step client records itself first thing in `main()` (the opsproof patch, with a self-test check); a program that never recorded itself is refused as what it is, "S.A.K. did not record its own program file when it started; an internal error" (`program_check_failed`), never as a file in the folder; every program and test that can reach an elevation records itself, and a guard fails on one that does not. | `partition_manager_live_certifier --self-test` (`step-client-vouches-for-itself`); `test_program_integrity::uncapturedProgramRefusesAsItsOwnFault`, `everyProgramThatCanElevateRecordsItself` | `step-client-capture`, `uncaptured-refusal-truthful`, `capture-guard` |
| Audit e1 | The read-only generated-script audit (2026-11-11): `Invoke-SakFilesystemTool` ran the bundled tool with `2>&1` under Stop, and e2fsprogs print their version banner to stderr (resize2fs always, e2fsck and mke2fs mostly), so the first line threw NativeCommandError: ext Format, Grow, Shrink and Repair stopped there. | Stop is relaxed for the native call only; the lines go to the log and `$LASTEXITCODE` decides, as in the diskpart and HFS+ runners. The post-format fsck call too. | `generatedScripts_toolRunnerSurvivesStderrAndKeepsTheExitCode` (the script's own function, a stand-in tool that writes a banner to stderr and exits 0, then 3) | `e1-tool-runner-stderr` |
| Audit e2 | `Dismount-Volume`, which Windows PowerShell 5.1 does not have, ran before every lettered target of swap Format, ext Format/Repair/Grow/Shrink, ext Create and every APFS script; an unlettered target went on with its file system mounted. | The script takes the partition (`[Sak.RawPartitionWrite]::Release`: write-through handle, lock, dismount; refused with a plain message while anything has it open); a format by a bundled tool first clears the old file system's first and last MiB (`ClearFileSystem`), so Windows does not mount it again under the tool; swap writes its header through the taken partition. The static guard now checks that every command a script calls exists in Windows PowerShell 5.1. | the guard; script assertions (ext format, shrink, create, APFS, swap); `test_raw_partition_write::scriptReleaseAndClearTakeThePartition` | `e2-take-not-dismount` |
| Audit e3 | HFS+ staged format and the raw-device writer wrote with no lock or dismount. | The "Raw partition writes" row. | as there | as there |
| Audit e4 | `Get-SakAttachedSystemPartition` returned its "is now active" notice along with the partition. | The notice goes to the log (`[Console]::Out`). | `generatedScripts_valueFunctionsReturnOnlyTheirValue` (stand-in Storage cmdlets) | `e4-boot-pick-leak` |
| Audit e5 | The writer-helper wrappers ran the helper with `2>&1` under Stop: its output and exit code never reached the log. | As e1. | `generatedScripts_writerWrapperReportsTheHelpersExitCode` (the wrapper's own text pointed at a stand-in) | `e5-writer-wrapper-stderr` |
| Audit e6 | `Get-SakHfsAllocatedBytes` returned 0 on any error, which turned the staging free-space check off. | An unreadable header (a damaged volume is what repair is for) asks for room for the whole partition. | `generatedScripts_valueFunctionsReturnOnlyTheirValue` | `e6-hfs-allocated-fail-closed` |
| Audit e7 | The staged image's written ranges were parsed from fsutil's translated output, so HFS+ staging could not work on a non-English Windows. | `Sak.SparseFiles` (`FSCTL_SET_SPARSE`, `FSCTL_QUERY_ALLOCATED_RANGES`); no fsutil. | `generatedScripts_sparseRangesComeFromTheFileSystem`; `test_script_types::sparseRangesAreReadFromTheFileSystem` | `e7-sparse-ranges-dll`, `e7-cs-setsparse` |
| Audit e8 | The writer wrappers built their argument list in the automatic `$args`. | `$cliArgs`; the guard refuses an assignment to `$args`, `$input`, `$_`, `$PSItem`, `$this`. | the guard | `e8-args-assign` |
| Audit e9 | Merge made its destination with `New-Item -Force` and checked for a link around it. | Made new and held (`New-SakBackupFolder`), copied with `Invoke-SakRobocopy`, which stops on a code of 4 or more before the source is removed. | `scriptBuilder_buildsMergeScript` assertions | `e9-merge-hold` |
| Script types | Found by the same guard in this round, before any build left the tree: moving the Add-Type blocks into the DLL left Copy Partition naming `Sak.CopyVolumeLock`, a type the script used to compile as a renamed copy of `ImageVolumeLock` and the DLL did not have. | `Sak.CopyVolumeLock` in the DLL wraps `ImageVolumeLock` and words its refusals for Copy Partition; the guard now also checks that every type a generated script names is one the DLL defines. | `generatedScripts_callTheirOwnFunctionsWithDeclaredParameters` (types), `test_script_types::typesLoadWithoutAddType`, `copyVolumeLockSpeaksForCopyPartition` | `guard-missing-type` |
| Lettered non-Windows volume (opsproof fix2) | Found by the operation proof on testpc2 (b28, ops.format-ext4): after a good Format to ext4 of a lettered partition, the scan's dirty-bit (fsutil, error 87) and BitLocker ("Element not found") reads failed on the RAW volume; their warnings came before the raw probe identified ext4, made the post-apply scan non-authoritative ("Post-apply verification failed") and refused every later Apply. The same for any lettered ext, HFS+, APFS or swap volume (a customer's ext4 stick). | The coordinator's patch: the volume-state warnings are added after the raw probe and skipped for a volume it identified as a file system Windows does not mount; a RAW volume nothing identified still warns. The b30 VM proof then found the same at Add to Queue: ext repair on a lettered customer ext4 volume (basic-data type, so Windows letters a RAW volume) stopped with "Not Added to the Queue: BitLocker protection and lock state could not be determined". The safety validator's BitLocker blocker and unknown-dirty-bit warning now skip such a volume too, through one shared test (`PartitionFileSystemRegistry::identifiedNonWindowsVolume`). | `inventoryWarnings_skipVolumesWindowsCannotProbe` (ext4 by raw signature: no warning; unidentified RAW: both); `safetyValidator_identifiedNonWindowsVolumeIsNotUnknownState` (the same two volumes at queue time) | `fix2-skip-dirty-bit`, `fix2-skip-bitlocker`, `fix2-validator-bitlocker`, `fix2-validator-dirty` |
| Partition type after a non-Windows Format (owner ruling through the coordinator) | Format to ext2/3/4, Linux swap, HFS+ or APFS left the partition a Windows data partition with its letter, so Windows mounted a RAW volume over it and offered "You need to format the disk in drive T:". | Every drive letter and folder path removed before the partition is taken (the b32 VM proof: APFS Format of a lettered RAW or freshly formatted NTFS partition failed at Apply, "APFS writer helper failed with exit code 1", because File Explorer held the lettered volume and the writer's lock was refused for its 10 s), and again after the tool or writer, then the file system's own type (GPT Linux file system 0FC63DAF, Linux swap 0657FD6D, Apple HFS+ 48465300, APFS 7C3457EF; MBR 0x83, 0x82, 0xAF); a disk with no partition table is refused. Measured on testpc1 for all seven: the type change alone also drops the volume and letter, the raw partition path still opens, RawPartitionWrite takes it and the write reads back, and Format-Volume then fails ("No matching MSFT_Volume objects found"). So Windows Format first sets a Linux or Apple type back to a Windows data partition (GPT basic data; MBR 0x07, or 0x0C for FAT32), waits for the volume and fails plainly if none comes; any other type is left alone. Create of a non-Windows file system is not a window path (F2, the second fixer). | `generatedScripts_partitionTypeStepsRunAgainstStandIns` (both steps in Windows PowerShell 5.1 against stand-in Storage cmdlets), `scriptBuilder_nonWindowsFormatTakesItsPartitionType`, `scriptBuilder_windowsFormatRestoresAWindowsDataType`; elevated `test_raw_partition_write::formatRoundTripSetsAndRestoresThePartitionType` (the app's own ext4 then NTFS Format scripts on a lettered MBR partition) | `type-step-format`, `type-step-access-paths`, `type-step-letters-first`, `type-restore-windows-format`, `type-restore-volume-wait` |
| Merge and the volume copies (opsproof F10, fix5) | Found by the operation proof on testpc2 (b28, ops.merge): Merge stopped with robocopy exit 9, "ERROR 5 ... Copying File U:\System Volume Information\WPSettings.dat Access is denied", on an ordinary NTFS volume; it failed closed (no Remove-Partition). Not a regression of this round: at the base the certified copies ran `robocopy /MIR` and Merge `robocopy /E`, neither with an exclusion. | The coordinator's patch: `Invoke-SakRobocopy` leaves out `(Join-Path $from 'System Volume Information')` and `(Join-Path $from '$RECYCLE.BIN')`, the root only, as `Get-SakFileManifest` already did; the same names deeper down are copied. The same helper serves the cluster-size, volume-serial and allocate copies. | `generatedScripts_volumeCopyLeavesOutOnlyTheRootVolumeFolders` (the scripts' own Invoke-SakRobocopy and Get-SakFileManifest in Windows PowerShell 5.1 over a stand-in root holding both folders at the root and deeper down; the real robocopy, with /COPYALL given as /COPY:DAT because copying auditing information needs a right an unelevated test lacks); the text check in `scriptBuilder_buildsMergeScript` | `fix5-root-volume-folders` |
| APFS Format without a volume name | Found by the b34 VM proof: Format to APFS of a RAW or unlabelled partition failed at Apply with only "APFS writer helper failed with exit code 1". The Format dialog starts from the partition's current label, and the writer refuses an empty name ("APFS format requires a non-empty volume name"; reproduced by hand on the VM and on an image locally). | Refused at Add to Queue: "An APFS volume needs a name: type one in Volume label". | `safetyValidator_apfsFormatNeedsAVolumeName` | `apfs-format-needs-name` |
| Helper start timeout (opsproof F13) | On testpc2 with Driver Verifier on, a Type ID change was refused with "Elevated helper did not respond in time": the client gave the helper 10 s to open its pipe, and the helper checks every program file first. On the VM, the first start after a fresh unpack with Defender on took about 1.4 s (0.3 s of it the helper's own start); a slow USB drive, Defender's first scan of the helper or a busy PC can take far longer. | `kPipeConnectTimeoutMs` 30 s (the client's wait for the pipe and the helper's wait for the client), and the broker logs "still waiting for the administrator helper to start (N s of 30 s)" every 5 s. The per-frame deadlines are unchanged. | `test_elevated_pipe_protocol::testProtocolConstants` | `f13-connect-timeout` |
| Defender (opsproof, testpc2) | Defender on testpc2 quarantined `tests/support/partition_image_volume_lock_fixture.ps1` (this round's version) as Trojan:PowerShell/Fleisnam.E, which broke the build's code-digest step there: the file carried the literal `[System.Reflection.Assembly]::Load(` next to the Add-Type C# its drills compile. `partition_os_migration_fixture.ps1` had the same pair. | Both fixtures check the scripts' loader by the DLL's name instead, so neither carries the reflective-load text. `test_defender_clean` (tooling-extended) scans the tests and scripts trees and the program folder with Defender (`-DisableRemediation`) and fails on a detection or a scan that could not run. On the VM, with real-time protection on and signatures updated to 1.463.261.0: the b34 package (C:\SAK) and both trees clean; a generated-style loader through AMSI loads the types with no detection; the control, the quarantined blob written as a .ps1, is detected as Trojan:PowerShell/Fleisnam.E. | `test_defender_clean` | the control |
| Function defined before use (opsproof F14, fix6) | A regression of this round, found by the operation proof on testpc2 (b28, ops.cluster-ntfs): Change Cluster Size and Allocate Free Space, both certified, failed on every run with "New-SakBackupFolder : The term 'New-SakBackupFolder' is not recognized". Their setup called the function the round introduced before `robocopyManifestFunctionsScript()`, which defines it, came in the script; nothing was destroyed (it stops before Format-Volume). The static guard checked that own-function calls name declared parameters, not that the definition comes first. | The coordinator's patch puts the functions first in both builders. The class is closed in the guard: every call a generated script makes outside its functions to one of its own functions must come after that function's definition, and after the definitions of every own function it calls in turn, across the whole corpus. | `generatedScripts_callTheirOwnFunctionsWithDeclaredParameters` (`Get-SakOrderProblems`; it also requires the corpus to hold such calls), and the patch's order assertions in the allocate and cluster script tests | `f15-define-before-use` (this exact break; named before the finding was renumbered F14) |
| Long scripts and the Storage cmdlets (opsproof F17, fix8) | Since ae87377b (2026-11-03): a script over 30,000 characters runs from standard input, and the reader ran it as `& ([scriptblock]::Create($sakScript))`. Its `$ErrorActionPreference = 'Stop'` then stayed in a child scope the Storage module's CDXML cmdlets do not see, so a failed Remove-Partition, New-Partition, Format-Volume or Resize-Partition without its own `-ErrorAction` went on and the script exited 0: Apply reported success (proved on testpc2 by the operation proof). The helper's RunPowerShell task builds its command with the same `sak::powerShellInvocation`. No script in the generated-script corpus is that long (the guard now names any that is); Migrate OS and the HFS+/APFS file writes with a large payload are. | The coordinator's patch dot-sources the script, so it runs in the scope a short script runs in; the reader's own variables (`$sakReader`, `$sakScript`) are removed first, so the script finds only `$sakStdinScriptBlock`. The exit code comes through. | `test_process_runner::powerShellInvocation_longScriptReadsItselfFromStandardInput`; `runPowerShell_longScriptRunsAndKeepsItsExitCode` (real hardware only: exit 7 kept, a throw stops, `Get-Partition -DiskNumber 99999` under the script's Stop stops it, the reader's variables are gone), passed elevated on testpc1 through the same code path the helper uses | `f17-dot-source` |
| Restore Image and the corpus (opsproof F20, fix10) | A regression of this round in a certified operation, found by the operation proof on testpc2 (b28, vhd.image-restore): every Restore Image stopped at once with "Unable to find type [SakLockedFilePath]"; nothing was written. The branch never loaded `sak_script_types.dll` before the content pin named the type. The static guard that checks exactly that ("uses a type before loading the DLL") missed it: no corpus input built a valid Restore Image script, so the guard never saw one. | The coordinator's patch loads the DLL first in that branch. The class is closed with a coverage guard: every operation type builds at least one real script from the corpus, and each named branch appears in one (Format Windows, ext, HFS+, APFS, swap; Check ext, HFS+, APFS repair; Resize Windows and ext; Create Image in both capture modes; Restore Image; Copy Disk to a disk; Copy Partition from a volume; Migrate OS block by block and through an image; the backup of Change Cluster Size, Allocate, Move, Primary/Logical, Serial and Dynamic to Basic; the Merge copy). The corpus gained the inputs for those, so the static checks (own-function calls and parameters, define before use, types loaded before use, commands that exist in Windows PowerShell 5.1) now see each one. | `generatedScripts_corpusBuildsEveryOperationAndBranch`; the patch's order assertion in the pinned Restore Image test; elevated `test_raw_partition_write::restoreImageThroughTheGeneratedScript` (the app's own Restore Image script, a 2 MiB image file onto a whole 64 MiB virtual disk with its identity pinned as the panel pins it, then the disk read back as the image): passed on testpc1 with the b36 package's DLL, and with the loader line left out it failed there with "Unable to find type [SakLockedFilePath]" | `f20-restore-loads-types` (red: the guard, "uses SakLockedFilePath without loading sak_script_types.dll before it" for three corpus scripts, and the assertion); `f20-corpus-coverage-image` (without the image inputs: "no corpus input builds: Create Image; Restore Image; Migrate OS; Create Image, locked volumes; Create Image, offline disk; Restore Image; Migrate OS, block by block; Migrate OS, through an image"; a first drill that dropped only the Restore Image input stayed green, because the raw-disk copy inputs build a Restore Image script too). The guard also fails if an operation type is added after the last one it counts; the testpc1 run above, red then green |
| Flush after the dismount (coordinator ruling) | FAT32 and exFAT answer FlushFileBuffers with ERROR_NOT_READY (21) once their file system is dismounted, and both writers took that as success. | Only that code, only on a partition the writer itself took and dismounted, and only when every block written since the last flush reads back as written (SHA-256 per 4 KiB block, recorded as each write is made; a partial block is recorded as it will read after the write): otherwise the flush fails, "The partition did not flush, and the block at offset N does not read back as written". C++ (`WindowsRawDevice::syncToDevice`) and C# (`TakenPartitionStream.Flush`). | `test_raw_partition_write` FAT32 and exFAT rows of both writers (the comment says which rows pass through the check) | `flush-readback-cpp`, `flush-readback-cs`, both on testpc1 (elevated): with a recorded hash that cannot match, the FAT32 and exFAT rows fail with "The partition did not flush, and the block at offset 0 does not read back as written" and NTFS (whose flush succeeds) passes; with the real code all three pass |

Measured along the way:
- FAT32 and exFAT refuse `GetFileInformationByHandleEx(FileAttributeTagInfo)`; NTFS answers it (VM, 2026-11-11:
  `[Sak.SafeFiles]::HoldReadOnly` and `HoldDirectory` failed on X: and Y: and held on C:). `FileBasicInfo` answers
  on all four.
- In Windows PowerShell 5.1 a `[ref]` parameter that is not passed is `$null`, and assigning through it throws under
  `$ErrorActionPreference = 'Stop'`; giving it a `$null` default instead fails to bind ("Reference type is expected
  in argument").
- diskpart reads its `/s` script with the C runtime's `_wfopen` (its import table), which shares reading and
  writing, so it opens through S.A.K.'s read-only hold; a writer and a delete are refused
  (`heldReadOnlyFileOpensForReadersAndRefusesWriters`, `heldScriptRefusesWritersAndAllowsReaders`; FAT on the VM).
- `netsh wlan add profile filename=` reads through the hold too: testpc1, 2026-11-11, a throwaway user=current
  profile added (exit 0) and deleted with the file held; a writer refused meanwhile; the control (the file opened
  with no sharing) made netsh fail, so the test tells; the profile list identical before and after.
- `/DEPENDENTLOADFLAG:0x1000` on an exe: a DLL only on PATH or in the current folder no longer loads
  (STATUS_DLL_NOT_FOUND), one beside the program still does (two-file stand-in, link.exe and lld-link).
- HFS+ format, repair and file changes have stopped at their first step since 2026-08-31 (R5 IDX33):
  `New-SakSparseImageFile` set `$Owned.Value` with no caller passing `-Owned`, which throws under
  `$ErrorActionPreference = 'Stop'` (measured in Windows PowerShell 5.1) before the target is read. Fixed with the
  staging check by removing the unused parameter; the new test runs every caller's call and would have caught it.
  Before that they failed later, at the tool runner and at the partition write (rows above): the app's HFS+ Format
  and Repair never worked, and Format to APFS worked only on a partition Windows had no file system on.
- A partition opened for writing (`\\?\GLOBALROOT\Device\HarddiskN\PartitionM`, NTFS, no drive letter; VM,
  2026-11-11): a 1 MiB write at the start succeeded; one over the last MiB wrote 1,044,480 bytes and reported
  success, with and without `FSCTL_LOCK_VOLUME` plus `FSCTL_DISMOUNT_VOLUME`; on the same partition with no file
  system Windows knows (RAW) the same APFS format succeeded. With `FSCTL_ALLOW_EXTENDED_DASD_IO` added, before the
  lock or after the dismount, both MiB were written whole; on RAW that control is refused (error 87) and is not
  needed. After the handle closed Windows found no file system it knew there (`logs\raw-lock.log`,
  `logs\raw-lock2.log`).

The Apple sentence (owner ruling through the coordinator, 2026-11-11). "Writes to Apple file systems were
validated with Apple's own tools." came from `Get-PmAppleSentence`, emitted whenever
`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json` had a certified capability. That evidence is the HFS+ and
APFS write engine on earlier code (H-a..H-h 2026-06-14 to 06-28, A-a..A-h 2026-06-26 to 08-31, the H8/A8 physical
gates 2026-06-28), run by certifiers and the writer tools on images and a USB disk; even
`external.hfs-file-apply-physical` (2026-06-10) staged with its own FileStream steps, not the app's script. No
evidence ran the app's raw-device HFS+ format, repair or file change, and the final v0.9.3 run had no Apple row, so
next to "On the v0.9.3 code ... ran through the app" the sentence overstated the current code. It now reads
"Separately, on earlier code (June to August 2026), S.A.K.'s HFS+ and APFS write engine was validated with Apple's
own tools.", with the months and the file systems taken from the matrix and a certified capability with no date
stopping the wording (`test_partition_manager_certification_status`). The About tab, README and the certification
document quote it; the recertification regenerates the block.

The raw-device HFS+ file-change operations (`PartitionOperationType::HfsReplaceFile`, `HfsGrowFile` and the rest of
the HfsXxx family) are built only by tests and certifiers: nothing in the app creates them, so their staged scripts
are not a user path. A person changes files on an HFS+ partition through File Management, which uses the in-process
`PartitionHfsFileSystemWriter` and no staging script. The family stays, covered by the new caller test and the static
guard; keep or remove is the owner's call after the release.

INCOMPLETE, after 0.9.3 on the owner's ruling: certification rows that run the app's own HFS+ and APFS format, repair and
file-change paths through the step client, so the Apple sentence can one day be about current code.

VM proof (2026-11-11, VirtualBox "Windows 11" on the Mac, restored to `pre-round8`, guest hygiene applied, restored
and powered off afterwards; no snapshot taken or deleted). Package `SAK-Utility-v0.9.3-secfix-5585dca5-wt` staged by
the release workflow's two scripts from lane2's build (zip SHA-256 `2A74926E48F976E901B2B0CC9F4A4C06DDB16DC0E3418E45B0805CB4700F5485`),
unpacked to C:\SAK (NTFS), X:\SAK (FAT32 VHD) and Y:\SAK (exFAT VHD), plus C:\SAKSigned, a copy whose 23 unsigned
program files were signed with a test certificate trusted on that VM only. Each start below is the app started with
Technician's elevated token (the guest's SAKCaptureElevated task); the attack is from its limited token. Evidence:
`E:\SAK-Utility-Test-Scratch\secfix-vm\` (`logs\<proof>-<step>.log`, `shots\<proof>-launch.png`, `logs\timeline.log`).

| Proof | What was planted | Result |
|---|---|---|
| `ntfs-stale-junction` (4a.1, 4a.2) | `data\temp\run-999996-...\link`, a junction to `C:\SecCanary` | at the elevated start the stale run folder was removed and the canary stayed (1 entry, same bytes); the run's own folder: owner Administrators, protected DACL, SYSTEM and Administrators only; from the limited token a junction, a file and a rename in it were each refused (access denied); removed at exit |
| `ntfs-temp-junction` (4a) | `data\temp` itself a junction to the canary | "S.A.K. Utility could not create its temporary folder in its data folder: C:\SAK\data\temp is a link (a junction or symbolic link), so it was not used"; nothing created through it, canary intact |
| `ntfs-planted-dll` (4c) | `sak_extra.dll`, an unsigned DLL that is not S.A.K.'s | elevated start refused: "S.A.K. won't run as administrator while C:\SAK\sak_extra.dll is in its folder; it isn't part of S.A.K." |
| `signed-clean` (4d) | nothing (test-signed package) | elevated start, main window, run folder protected as above |
| `signed-helper-unsigned` (4d) | the helper swapped for the unsigned build's | refused, naming `C:\SAKSigned\sak_elevated_helper.exe` |
| `signed-helper-other-signer` (4d) | the helper re-signed by a second trusted test publisher | refused, naming the helper |
| `unsigned-helper-swapped` | on the unsigned package, the helper signed by another publisher | refused (rule iii accepts only an unsigned first-party program) |
| `fat32-clean`, `exfat-clean` on the first package | nothing | refused: "S.A.K. won't run as administrator while X:\SAK\bz2.dll is in its folder" (and Y:): FAT32 and exFAT refuse `FileAttributeTagInfo` (`before-fat-fix\`). Fixed (FAT row above); package `...-wt` rebuilt (zip SHA-256 `BDDA5D4957E53FCD4189B760FE831AF765EACA5B1B335541CD8909DEE1147F69`) |
| `fat32-clean`, `exfat-clean` after the fix | nothing | elevated start, main window; the run's folder made on the volume (no ACLs there, as expected) |
| `vm-drill-partition-lock` (raw partition writes) | the APFS writer CLI built with the volume lock switched off, then the package's own; the scripts' DLL compiled with a plain FileStream, then the package's own; each against Disk 4's NTFS partition, formatted NTFS again before each | drilled CLI: "Unable to zero APFS format target: Windows wrote only 1044480 of 1048576 bytes at offset 267386880: the rest is past the end of the partition as this handle sees it"; package CLI: ok. Drilled DLL: "Access to the path is denied." and the last MiB on disk unchanged; package DLL: written, and the last MiB read back through the disk equal (`logs\vm-drill-partition-lock.log`) |
| elevated suites on the package | `test_no_follow_fs`, `test_script_types`, `test_fat_volumes`, `test_raw_partition_write` with SAK_TEST_ALLOW_REAL_HARDWARE=1 | On this VM `test_no_follow_fs` 25/25 and `test_script_types` pass; the two virtual-disk suites could not set up here or on testpc1 at first. The cause was the tests' own set-up, not the machines: `diskpart(create) + makeVolume(...)` as one sum, whose operands C++ may evaluate in either order, and MSVC ran `makeVolume` first, before the disk existed ("The system cannot find the file specified"). Two statements now, with a comment. On testpc1 (a real PC; 64 MiB virtual disks in TEMP only, removed after): `test_fat_volumes` 10/10, `test_no_follow_fs` 25/25, `test_script_types` 11/11, and `test_raw_partition_write` 14 of 15, its FAT32 row of `scriptReleaseAndClearTakeThePartition` refused: "The partition is in use". The check had read as passing in two earlier runs, because under the default Continue a refused `ClearFileSystem` failed only its statement and `'CLEARED'` still printed; it now runs under Stop, matches whole lines, and says how long the hold lasted: taken on the first retry after the 2 s refusal, about 3 s after the file on the volume was closed. The take now tries for 10 s (50 x 200 ms, C++ and C#). The final package of this round (b38, `SAK-Utility-v0.9.3-secfix-63c0f806-wt`, zip SHA-256 `7E7F75A238A43C88C945695D4CA37EBF2BA8A9DB27407999DE709FEC558BC04E`) with that build's test programs, on testpc1, 2026-11-12: `test_raw_partition_write` 16/16 (with the partition-type round trip and Restore Image), `test_fat_volumes` 10/10, `test_no_follow_fs` 25/25, `test_script_types` 11/11, and `test_process_runner`'s two long-script cases. Test programs and package must come from one build: with newer test programs on an older package, `programFolderOnTheVolume` refuses the package's `sak_script_types.dll` ("it isn't part of S.A.K."), since the C# build is not byte-for-byte repeatable. |

Live proof through the app's own window (owner ruling through the coordinator, "Option A", 2026-11-11). Same VM;
package `SAK-Utility-v0.9.3-secfix-5585dca5-wt` rebuilt from this tree (zip SHA-256
`9065BF0299C0DEB4EF7DD250F694A71989356E637E785775F663BC270D3223BA`), unpacked to C:\SAK (NTFS) and started with the
elevated token; the site repo's capture harness clicked through Partition Manager and File Explorer as a person
would. Targets: two throwaway 512 MiB virtual disks, Disk 3 (HFSPROOF) and Disk 4 (APFSPROOF), each with a 256 MiB
NTFS data partition and no drive letter. Each result was read back by tools that are not the operation: the bundled
`fsck_hfs -n -f` on a read-only copy of the partition, and `partition_filesystem_probe_certifier --expect` (the
production reader) on the partition itself. No Apple tool runs on Windows, so the APFS read-back is the production
reader only. Evidence: `capture\<tag>\` (pictures and the window's accessibility tree) and `logs\verify-<tag>.log`.

| Operation (window) | On the way (each fixed above, picture kept) | Result | Read back |
|---|---|---|---|
| Partition Manager > Format Partition > HFS+, Disk 3 partition 2 (NTFS) | "The property 'Value' cannot be found" (`hfs-format-live-failure-before-fix.png`); "Cannot convert ... System.Object[] ... to System.Int32" (`hfs-format-live-failure-tool-runner.png`); "Access to the path is denied." (`...-access-denied-b21.png`); "Unable to find type [Sak.RawPartitionWrite]." (`...-unloaded-type-b22.png`) | batch complete; helper task success (`capture\hfs-format-b24`) | fsck_hfs exit 0, "The volume HFSPROOF appears to be OK."; probe: HFS+, valid volume header (`verify-hfs-format-b24b.log`) |
| Check Non-Windows File System > Queue HFS+ repair, Disk 3 partition 2 | (as above: never ran) | batch complete; success (`capture\hfs-repair-b24`) | fsck_hfs exit 0, OK; probe HFS+, no blockers (`verify-hfs-repair-b24.log`) |
| File Management > File Explorer > Add Raw/Image (the HFS+ partition) > New > New Folder "SECFIX-PROOF" | | the folder listed (`capture\fm-change-b24`) | fsck_hfs exit 0, OK; probe lists `/SECFIX-PROOF` (`verify-fm-change-b24.log`) |
| Format Partition > NTFS, Disk 4 partition 2 (control: diskpart through the helper) | | success | the APFS format below found NTFS there |
| Format Partition > APFS, Disk 4 partition 2 (NTFS) | "APFS writer helper failed with exit code 1" ("Unable to zero APFS format target: Unknown error") | batch complete; success (`capture\apfs-format-b24`) | probe: APFS, 65,536 blocks of 4 KiB, one volume "APFSPROOF", no blockers (`verify-apfs-format-b24.log`) |
| Check Non-Windows File System on that APFS container | | NOT OFFERED: only the read-only "APFS Metadata (Last Disk Scan)" dialog (`capture\apfs-repair-b24\diagnostics`) | see below |
| (b28) Format Partition > APFS, Disk 4 partition 2 (NTFS, 256 MiB) | | success (`capture\apfs-format-b28`) | probe: APFS, passed (`verify-apfs-format-b28.log`) |
| (b28) Check Non-Windows File System > Queue generated APFS repair, that 256 MiB container | | offered (the gate fix), then Apply failed: "APFS writer helper failed with exit code 1" (the writer's repair handles one space-manager chunk, 128 MiB) | the gate now also asks the writer's geometry (the "APFS repair offer" row) |
| (b28) Format > ext4; Check > Queue ext repair; Format > Linux swap (Disk 3) | | success each (`capture\ext-format-b28`, `ext-repair-b28`, `swap-format-b28`) | bundled `e2fsck -fn` exit 0; swap signature SWAPSPACE2, version 1, probe passed |
| (b30) Resize/Move ext4 256 to 400 MB, then 400 to 200 MB | the size box reads "400 MB" (flow fixed) | success each (`capture\ext-grow-b30`, `ext-shrink-b30`) | `e2fsck -fn` exit 0 after each |
| (b30) Format > ext4 over ext4 | | success (`capture\ext-format-b30`) | partition type Linux file system (0FC63DAF), no volume, no letter; `e2fsck -fn` 0 |
| (b30) Format > NTFS of that Linux-typed partition | | success (`capture\ntfs-from-ext-b30`) | type back to basic data; probe NTFS |
| (b30) Format > ext4 of a lettered NTFS partition (E:) | | success (`capture\ext-format-lettered-b30`) | letter gone, Linux type, `e2fsck -fn` 0 |
| (b30) Format > Linux swap over swap | | success (`capture\swap-format-b30`) | type Linux swap (0657FD6D), no volume, SWAPSPACE2 |
| (b30, b32) Check > Queue ext repair on a lettered customer ext4 volume (basic-data type, E:, made outside S.A.K.) | b30: "Not Added to the Queue: BitLocker protection and lock state could not be determined" | b32: queued, applied, verified (`capture\ext-repair-lettered-b32`) | `e2fsck -fn` 0 |
| (b32) Format > NTFS of a customer Linux-typed partition (made outside S.A.K.) | | success (`capture\ntfs-customer-linux-b32`) | basic data, NTFS, probe passed |
| (b32, b34) Format > APFS of a lettered RAW and a lettered NTFS partition | b32: failed after 10 s (Explorer held the lettered volume); b34: the letters now go first, and it failed in 3 s: the volume had no label, and the writer refuses an empty name | b35: refused at Add to Queue without a name ("An APFS volume needs a name: type one in Volume label", `capture\apfs-format-unnamed-b35`); with a name, success on a 100 MB container (`apfs-format-named-b35`) and on a lettered 150 MB NTFS partition (`apfs-format-lettered-b35`) | probe APFS, type APFS (7C3457EF), no letter (`verify-apfs-format-named-b35.log`, `verify-apfs-format-lettered-b35.log`) |
| (b35) Check > Queue generated APFS repair, the 100 MB container the app formatted | | success (`capture\apfs-repair-small-b35`) | probe APFS, passed (`verify-apfs-repair-small-b35.log`) |
| (b35) Check Non-Windows File System on a container over 128 MiB | | the read-only metadata dialog, no repair (`capture\apfs-check-large-b35`, `apfs-check-150-b35`) | the panel tests pin its "Repair" row words |
| (b34) Merge Partitions, Disk 3 partition 3 (F:, files including nested `System Volume Information` and `$RECYCLE.BIN`) into partition 2 (E:) | (the F10 exit 9 was found by the operation proof) | success (`capture\merge-b34`) | partition 2 256 to 496 MB; every file under E:\Merged-Partition-3 with the same SHA-256, nested folders included (`logs\prep-b34-after-merge.log`) |
| (b34) Reformat for New Serial Number, E: | first try: "Backup directory does not exist" (a folder that did not exist; the queue-time check is F14, the second fixer's) | success (`capture\volume-serial-b34b`) | serial 0xe4de9be3 to 0x18edf146, every file identical (`logs\prep-b34-after-serial2.log`) |
| (b35) Change Cluster Size, E: to 8 KB | (F14 was found by the operation proof) | success (`capture\cluster-b35`) | 4096 to 8192 bytes per cluster, every file identical (`logs\prep-b35-after-cluster.log`) |
| (b35) Allocate Free Space, 50 MB from Disk 4 partition 3 (G:) to partition 2 (F:) | | success (`capture\allocate-b35`) | F: 150 to 200 MB, G: recreated 200 to 150 MB, every file on both identical (`logs\prep-b35-after-allocate.log`) |

APFS repair through the window (found by this proof, fixed; see the "APFS repair offer" row). The panel offered "Queue generated APFS repair" only for one fixed layout the writer no longer produces, so the option never appeared (from the 2026-06-13 base, `c4cdefc6`). It is now offered for a container S.A.K.'s writer formatted whose geometry the writer's own repair accepts: one space-manager chunk, 128 MiB. The b28 rerun showed the option for a 256 MiB container and the Apply then failed ("APFS writer helper failed with exit code 1"), which is why the geometry is part of the gate now; for any other container the metadata dialog says why there is no repair.

Tool-only defect (no app path reaches it): `sak_apfs_writer_cli repair-image`, the image-only repair, reports a healthy container as a failure ("APFS repair did not find any incorrect supported object checksums", exit 1; measured on a 64 MiB container the writer had just formatted). No generated script calls it: the app's repair runs `repair-raw`, which does not have that blocker; only `scripts/test_sak_apfs_writer_cli.ps1` and the probe certifier call the image path. Left for after the release.

Known limitations (documented, not open defects):
- HFS+ format, repair and file changes need S.A.K. on an NTFS drive: they stage an image of the partition in the
  data folder, and only NTFS and ReFS hold a sparse file (FAT32 also caps a file at 4 GiB). They refuse before the
  target is read, naming the drive and its file system, and ask for room for the volume's allocated blocks first.
  For the wiki notes list.
- The Windows USB creator's WIM split now needs room on the drive S.A.K. runs from; its existing free-space check
  names the folder. The same holds for what programs S.A.K. starts put in TEMP (an installer unpacking itself,
  Chocolatey's downloads): it lands in the run's folder on that drive and goes when the run ends.
- Before each elevation the check reads every program file (about 150 MB): on the VM's FAT32 virtual disk an
  elevated start took about 6 s to its main window; on a slow USB 2 stick the first read takes longer, and Windows
  caches it after.
- While S.A.K. runs as administrator, its program files are locked: they cannot be changed, replaced or deleted
  until it exits (the same for the helper's life). Deliberate: that hold is what makes the check mean anything.
- A program file is accepted by its pinned path and SHA-256 (what a release does not change: the Qt runtime and
  plugins, the VC runtime, vcpkg's DLLs, the bundled tools), or by carrying the running program's own signature.
  An unsigned (developer) build, having no signature to compare, also accepts its own three other programs unsigned,
  by name only: `sak_elevated_helper.exe`, `sak_apfs_writer_cli.exe` and `sak_hfs_writer_cli.exe`. So on an
  unsigned build a swapped helper is not caught; the 4d check is a signed release's.

Residual risks (stated):
- Same account: another process of the same user, unelevated, can still do what that user can do to the program's
  folder before S.A.K. starts; the check then refuses an elevation, but an unelevated run trusts its own folder as
  any portable program does.
- Started as administrator from Explorer (right-click, Run as administrator) rather than through S.A.K.'s own Run as
  Administrator or helper: Windows resolves the program's imports before any S.A.K. code runs, and for a DLL that
  is not one of Windows' KnownDLLs (version.dll, dwmapi.dll, uxtheme.dll and the like) it looks in the program's
  folder first. A DLL planted there under such a name runs its DllMain as administrator before S.A.K.'s start-up
  check, which then refuses the run, naming it. No program can close this from inside itself; S.A.K.'s own
  elevation paths check the folder before they elevate, and the module audit ends a run in which anything outside
  the rules was loaded. `/DEPENDENTLOADFLAG` keeps PATH and the current folder out of that search.
- FAT32 and exFAT keep no ACLs and hold no links: on such a drive any local account can read or change a temporary
  file in `data\temp` while it exists. A file another program reads by name is held read-only from creation until
  that program is done, so it cannot be changed in that time; the HFS+ staging image (not on FAT, see above) and
  the USB creator's split WIM are written by the tools themselves. The FileVault password file exists for the
  operation's length, readable by local accounts on FAT; it is overwritten with zeros and removed afterwards, which
  flash wear levelling may not erase physically.
- Programs S.A.K. starts inherit TEMP and TMP pointing into `data\temp`, so their own temporary files land there
  too; what they write elsewhere is theirs (below).
- What Windows itself records outside the program's folder when any program runs, S.A.K. included (Windows'
  records, not S.A.K.'s files, and not something a portable program can prevent): the Prefetch file, the
  Background Activity Moderator and Application Compatibility entries, the UserAssist and MUICache entries Explorer
  keeps for a program it launched, the Windows Error Reporting record if it crashes (S.A.K.'s own crash dumps go to
  `data\crashes`), and event-log records such as a UAC elevation; plus whatever a feature exists to change on the
  PC (a restored profile, a Wi-Fi profile added, a package installed).
- The TSan row now runs the logger through an in-memory sink (`test_logger_concurrency`); the Windows file sink
  itself is covered by `test_logger` under the normal suite.

### Security round sec2: listeners, hostile remote data, signing and gate pins (2026-11-11)

A parallel round to the one above, on the same owner rulings: items 5 and 6 of the security brief (the network
and IPC listeners; treating the remote side as hostile) and the two items "also inside the digest" (`sign-exe.ps1`,
`requirements-gates.txt`). It was built on its own branch from 5585dca5 and cherry-picked onto this round's tree in
the merge round below. Every finding was re-read in the code at 77a92478 before it was fixed; all were real. Each
fix has a test that went red with the fix taken out and green with it restored.

#### 5. Network and IPC listeners

**iPerf3 server.** It listened on every address the PC had, kept a peer that connected and went
quiet for as long as it liked, and started whether or not its inbound firewall rule was added
(unelevated, `netsh` refuses, and Windows then asked the user whether to let `iperf3.exe` through,
public networks included). Now (`bandwidth_tester.cpp`): `-B <address>` binds it to the one
adapter address chosen in the panel, `--idle-timeout 30` drops a silent peer and goes back to
listening, and a server other PCs can reach starts only after its rule is in place. The rule also
names the bound address (`localip=`). Without the rule the server is not started and the panel
says why: "iPerf3 server not started: Windows Firewall did not add its inbound rule (...). Adding
it needs S.A.K. running as administrator." A loopback server needs no rule (Windows Firewall does
not filter loopback). Tests: `test_bandwidth_tester` (the argv; Windows' own listener table shows
127.0.0.1 alone; with the rule refused, no server and the message).

**LAN Transfer server.** It listened on `QHostAddress::Any`. It now listens on the chosen adapter
address and refuses a wildcard. Test: `test_network_diagnostic_controller` (the listener table
shows 127.0.0.1 alone; Any refused).

**"Listen on" (new UI, small).** Neither server had any choice of interface, so each server row in
the Network Diagnostics panel gains a "Listen on:" list of the connected adapters' IPv4 addresses
(Bandwidth tab, iPerf3 group, before Start Server; LAN Transfer tab, server group, before Listen
Port). Each list starts on the adapter holding the default route and remembers the address that
server was last started on (`data/config/ui_state.ini`, `NetworkDiagnostics/*ServerListenAddress`).
Tests: `test_network_diagnostic_controller` (choices, preference), `test_network_diagnostic_panel`
(default, per-server memory). Screens to retake: Network Diagnostics > Bandwidth, and Network
Diagnostics > LAN Transfer.

**Browser bridge pipe.** The handshake read the first frame against the 64 MiB native-messaging
limit and checked the token, and only then the peer: any same-user process that opened the pipe
held its single instance for the 30 s deadline and could make the server allocate up to 64 MiB.
Now the peer (our own binary, launched by Chrome) is checked the moment it connects, before a byte
is read, and the hello's length prefix is refused above 4 KiB before anything is allocated.
Replies keep the full limit (they may carry screenshots). Tests: `test_browser_bridge_pipe` (a
60 MiB prefix is dropped at once with no allocation of 1 MiB or more; Windows PowerShell as a
foreign peer is dropped at once and the real relay is admitted right after).

#### 6. Treating the remote side as hostile

**Installer downloads.** The internalization engine (installers and `.nupkg`s) and the direct
downloader held each download whole in RAM, up to 4 GiB, to hash it before writing. They now share
`PackageInternalizationEngine::downloadVerifiedFile`: the body streams to a file in the process's
temp folder (`QDir::tempPath()`, the per-run folder the app points TEMP at), then is copied into
place through a `QSaveFile` while it is hashed, so the bytes that land are the bytes that were
checked, and nothing is committed on a mismatch. An installer with no usable checksum is now
refused before it is downloaded rather than after. `runNetworkTransfer` gained a body sink, and
refuses to hold any response that may exceed 512 MiB in memory: a caller that could get one must
stream it. Tests: `test_package_internalization_engine` (64 MiB over loopback with no Qt allocation
of 1 MiB or more, staged in the temp folder and never beside the target; a mismatch leaves
nothing; the cap stops a streamed body), `test_bandwidth_tester` (the in-memory ceiling).

**HFS+ leaf chains.** None of the four leaf walks (extents overflow, catalog listing, catalog
consistency scan, attributes scan) remembered the nodes it had visited, so a leaf linking back
into the chain re-read the same records up to the 200,000-node cap: a listing repeated entries, a
scan counted them again, and the extents load appended them to every fork's extent list. Each walk
now stops at a node reached twice: "HFS+ <tree> leaf chain loops back to node N". Test:
`test_partition_manager_core` (`hfsFileSystemReader_refusesLeafChainLoops`, all four walks).

**zlib.** The APFS decmpfs decoder (inline and ZLIB_RSRC chunks) used `qUncompress`, which grows
its buffer to whatever the stream holds, and compared the size afterwards. The same shape was in
the HFS+ decmpfs chunk decoder and the PST 4K block reader. All three now use one
`inflateZlibExactly` that inflates into the declared size plus one byte and stops there. Tests:
`test_bounded_inflate`, `test_partition_manager_core` (APFS and HFS+ bombs), `test_pst_parser`
(a 32 MiB bomb declaring 4 KiB: no allocation of 1 MiB or more).

**APFS extended attributes.** A B-tree node's table entries were bounds-checked one by one but
never against each other, so many entries could point at one record: a 1 MiB leaf of 100,000
entries naming one 64 KiB attribute value had the scan hold 100,000 copies (gigabytes). And two
records for one attribute both survived, with the decmpfs one (the file's content) decided by scan
order. Now a node whose entries overlap is structurally invalid, and a second record for one
(object, name) is refused. Test: `test_partition_manager_core`
(`apfsFileSystemReader_refusesAliasedOrRepeatedXattrs`, both on writer-made images patched and
re-checksummed).

#### Also inside the digest

**sign-exe.ps1.** It accepted any valid signature on the Artifact Signing dlib, which signtool runs
with signing rights, and any valid signature on the exe it had just signed. Now every DLL in the
dlib's folder must be signed by Microsoft (the client's own DLLs are "Microsoft Corporation", the
VC runtime it carries "Microsoft Windows"), and the signed exe must be signed by
`CN=Randy Northrup, O=Randy Northrup` (the publisher `verify_authenticode_signatures.ps1` requires
of every packaged binary) and timestamped. Test: `test_sign_exe_signers` (release-tooling): the
checks on real signed files, and `sign-exe.ps1` itself run in a sandbox with a stand-in `az`,
stopping at a Chocolatey-signed dlib.

**requirements-gates.txt.** Every package pip installs from it, the three tools and the twelve they
pull in, is pinned to one version and to the SHA-256 of every file PyPI publishes for it, so pip
runs in hash-checking mode. Proved on testpc2 in a fresh venv: installs and runs clang-format
23.1.3, lizard 1.24.3 and pre-commit 4.7.0; a wrong hash and an unlisted dependency are both
refused. The CI action that reads the clang-format pin parsed `clang-format==(.+)$`, which the new
line continuation would have broken (" \" captured with the version); it now reads the version
only.

#### Proof (testpc2, W:\sak at the sec2 tree)

- Build: Visual Studio 2022 / Qt 6.12.0, Release, BUILD EXIT 0, 293 executables.
- Targeted tests (console session, standard-user token): 15/15 green.
- Drills: 26 guards, each broken alone, red, restored byte for byte, rebuilt, green: 23 C++
  (zlib x4, HFS+ walks x4, APFS x2, downloads x4, pipe x2, servers and panel x7) and 3 PowerShell
  (sign-exe). One first attempt (iPerf3 `-B` removed) did not compile, so its green proved
  nothing; it was rewritten to compile and went red.
- T2 labels (product, release-tooling, tooling, cert), `-j 6`: 323 of 324 passed;
  `test_partition_manager_rig_pc_target` failed for testpc2's own state, not this change: the
  final certification left `C:\SAKCert\disposable-machine.json` there, so the rig script passes
  its disposable-machine check and stops at its administrator check, where the test expects the
  "runs only in a VirtualBox" refusal. sec2 changes none of the scripts involved.
- clang-tidy naming gate, both passes (LLVM 23.1.3): non-core 155/155 TUs clean; core 171/171
  healthy, 15 ratcheted TUs clean.
- ci.yml static gates, whole tree: all pass (hygiene, registration, qrc, PowerShell syntax,
  clang-format, lizard, ASCII, secret regex, licenses, blocking, GUI tokens / magic numbers /
  stylesheet literals, magic numbers, dialog buttons, logged message boxes, error-message
  uniqueness, build-system lint).

#### Residual risks

- Before the merge, `QDir::tempPath()` was the user's %TEMP%, so a partial download was staged
  there. Resolved in the merge round (follow-up c): it is the app's per-run folder.
- The offline deployment build kept its work folders in the chosen output folder
  (`<output>\_work`, `<output>\_sak_temp_XXXXXX`). Resolved in the merge round (follow-up b).
- iPerf3 and the LAN server bind IPv4 adapter addresses only (the list offers no IPv6).
- The LAN transfer server still adds no firewall rule of its own; bound to one address, Windows
  asks about it as before.
- An oversized embedded APFS extended attribute is not refused; the memory it takes is now linear
  in its size (no aliasing).
- https download callers are proven at the shared helper (`downloadVerifiedFile`), not each caller:
  the tests have no TLS loopback server.
- iperf3's own handling of a hostile client is upstream's; the idle limit and the bind narrow who
  can reach it and for how long.
- The exe timestamp check moved into `authenticode_signer.ps1` unchanged; no validly signed file
  without a timestamp was at hand to drill it.

### Release signing isolation and workflow pinning (2026-11-11)

From the security audit of a brief forwarded by the owner, who ruled that signing moves to OIDC.

- `release.yml` is four jobs. **build** (read-only token, no secrets, no environment, `persist-credentials: false`)
  does everything up to the signing catalog, including the filesystem tool check before signing, with no GitHub
  Actions cache: the toolchain action's new `cache: 'false'` turns off the pip, Qt, vcpkg and pinned-download caches,
  so nothing another workflow wrote reaches what ships. It hands on the unsigned package (one zip, which keeps its
  empty and dot-named folders, such as `tools/chocolatey/.chocolatey`, that a folder artifact drops) and the release
  check tools, for one day. **sign** runs in the `release` environment with `id-token: write` only and no checkout:
  download-artifact, an unpack with .NET's own ZipFile, azure/login (OIDC, `allow-no-subscriptions`), the signing
  action with `cache-dependencies: false` and `exclude-environment-credential: true`, an inline check that every
  catalog file is Valid, signed by CN=Randy Northrup and timestamped, and the signed zip handed on for one day. The
  pins of azure/login and the signing action are win-app-migrator's. **verify** (read-only, no environment) re-pins
  the packaged filesystem tools between the application's own check before and after, verifies Authenticode, runs
  the signed readiness gate from a build/Release laid out from the signed package, and builds the archive and
  SHA256SUMS. **publish** (tags only, `contents: write`, not in the environment) re-checks the uploaded zips, the
  binaries' signatures and every SHA256SUMS line, then creates the release. The repository does not attest, so
  neither does this. A tag that disagrees with VERSION is refused before anything is built.
- The `release` environment exists and admits `v*` tags only. AZURE_CLIENT_ID and AZURE_TENANT_ID stay repository
  secrets (they are IDs, and secrets cannot be read to copy them). AZURE_CLIENT_SECRET is no longer referenced and
  stays until OIDC has signed once. Azure must hold a federated credential for
  `repo:RandyNorthrup/S.A.K.-Utility:environment:release` on the signing app registration; nothing here can see it.
- A pre-release tag (one with a hyphen, such as `v0.9.3-rc.1`) runs build, sign and verify and stops before
  publish: that is the dry run of signing now that the environment refuses a manual run from a branch. A manual
  run from a branch builds and checks the unsigned package and skips the signing job.
- Elsewhere: `persist-credentials: false` on every checkout in ci, nightly and secret-scan; the TruffleHog action
  pinned to the 3.99.7 image by tag and digest (it ran `latest`); gitleaks in secret-scan checked against a pinned
  SHA-256 rather than the same release's checksums file; OpenCppCoverage installed as 0.9.9; Python as 3.14.10
  rather than "3.14"; and expressions moved out of `run:` blocks into `env:` (the version in release, the shard
  numbers and job results in ci). The old single job's unused `id-token: write` is gone with it.

### Merge round: the security rounds, the workflow hardening and the operation fixes on one tree (2026-11-12)

On lane2 at 8e33392b (the security round above, gate green), in this order: sec2's seven commits and the workflow
hardening's five, cherry-picked; the follow-ups below, each with a test that fails without it, drilled red with the
fix broken in the source and green with every byte restored; then opsfix's branch, merged; then one gate.

**Cherry-picks.** sec2 (a991c37e, 7d43b936, 3eb68a6d, 8bdfea89, 6fa760d2, 567354ed, 5c85add1) and gh-hardening
(5f776451, a2fc5a43, 0adf96cc, 10040c24, acff82a8). Git merged every file the brief expected to conflict
(`CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/README.md`, `test_partition_manager_core.cpp`, and
`.github/actions/windows-build-env/action.yml`, where sec2's clang-format pin regex for the hashed requirements file
and the hardening's tool pins both stand) without a conflict; the one conflict was this file, where both rounds
added a section at the same place: both are kept, the security round first. sec2's notes are folded into the sec2
section above and the CHANGELOG, and `docs/security-sec2-notes.md` is gone (it said nothing they do not). The merged
tree built (0 errors, 0 warnings) and passed T2, 330 of 330, before any follow-up.

| Follow-up | Fix | Test | Drill (red, then green) |
| --- | --- | --- | --- |
| a. sec2's four `removeRecursively` tree deletes | `sak::no_follow::removeTree` in `OfflineDeploymentWorker::removeOwnedWorkDir` and in the internalization engine's work-folder reset and its `_rels` and `package` cleanup. | `removeOwnedWorkDir_neverFollowsAJunctionInside`, `cleanNugetArtifacts_neverFollowsAJunction` (`_rels` itself a junction, and one inside `package`), `resetWorkFolder_neverFollowsAJunction`: a canary behind each junction survives. | `a-offline-work-removetree`, `a-engine-reset-removetree`, `a-engine-rels-removetree`, `a-engine-package-removetree` |
| b. Offline Deployment's work folders (owner: no work file outside the app's folder) | A bundle's and a direct download's work folders are new ones in the run folder (`newWorkDirPath`, under `QDir::tempPath()`), not `<output>\_work` or `<output>\_sak_temp_*`. Before either starts, when no installer's size is known yet, the drive holding the run folder needs 1 GiB free; then each download whose server declares its size (Content-Length) is checked before its body is taken, for that size, twice when its copy into place lands on the same drive, plus 64 MiB (`downloadWorkBytes`, asked through the transfer's new `on_size_declared`). Either refusal reads: "Not enough free space for the work files: S.A.K. keeps them in its own folder (...), and that drive has ... free; at least 1.00 GB is needed. Free some space on the drive S.A.K. runs from, or run it from a larger one." An installer taken from inside a package is placed by a no-follow copy that is read back (`placeVerifiedCopy`); a downloaded one was already hashed as it was copied into place. | `newWorkDirPath_isANewFolderInTheRunFolder`; `bundle_refusesWhenTheRunFolderIsShortOfSpace` and `directDownload_refusesWhenTheRunFolderIsShortOfSpace` (the refusal names the run folder, not the output, and nothing is made in either); `placeVerifiedCopy_keepsOnlyACopyThatReadsBack`; `downloadVerifiedFile_needsRoomForItsDeclaredSize` (a stand-in for the free space: one byte short of the need is refused before the body is taken, nothing left at the target or staged; exactly the need downloads) | `b-bundle-work-in-run-folder(-2)`, `b-space-check-shared`, `b-bundle-runs-the-check`, `b-direct-runs-the-check`, `b-work-dir-root`, `b-placed-copy-verified-2` (the first version of that drill did not compile, so it proved nothing and was rewritten), `b-size-declared-hookup`, `b-download-checks-declared-size`, `b-download-second-copy`, `b-download-margin` |
| c. Downloads stage in `data\temp\run-*` | Nothing new to fix: the test runs the app's own startup step (`app_paths::establishSessionTempDirectory`), so it proves Qt reads the TEMP the app set and the partial file is in the run folder while the download runs. | `downloadVerifiedFile_stagesInTheRunFolderTheAppSetsAtStartup` | `c-run-folder-temp` (TEMP and TMP not set), `c-download-staging-dir` (staged beside the target) |
| d. `test_partition_manager_rig_pc_target` on an approved PC | The rows script takes `-ApprovalPath` (same default); the test names a missing file of its own (the refusal on any machine) and one naming another machine (refused for that, so the file named is the one read). The approval on testpc2 is left alone. | the test itself | `d-rig-approval-path` |
| f. The Defender guard in CI | The nightly T4 job builds the program too and runs `test_defender_clean` over `tests`, `scripts` and `build\Release` in its own step, excluded from its ctest step. Where Defender is not running the step writes a warning into the job summary and passes with `-AllowMissingDefender`; `test_defender_clean.ps1` now also fails where Defender's service or antivirus is off, not only where its scanner is missing. Whether a hosted runner has Defender running is not known yet: it shows on the first nightly. | `test_defender_clean_status` (tooling, T2): with `Get-MpComputerStatus` answering "off", the scan fails, or passes saying NOTHING WAS SCANNED only when allowed | `f-defender-not-running` |
| i. Must-run-elevated | `ci.yml` runs `test_raw_partition_write` whole and `test_process_runner::runPowerShell_longScriptRunsAndKeepsItsExitCode` elevated in their shard, reads every case's PASS line, and uploads the proof; the T2 result job requires it, as for `test_fat_volumes` and the real-helper test. | On testpc1 with the b38 package and its test programs, the workflow's own step bodies (taken from `ci.yml` unchanged, run in Windows PowerShell 5.1 there): FAT 10/10, raw writes 16/16, the long-script case 3/3; the result job's check then printed OK for the three, and a problem only for the real-helper test, which that run did not plan. | The same check with the Restore Image PASS line deleted from the proof: "test_raw_partition_write: no PASS for RawPartitionWriteTests::restoreImageThroughTheGeneratedScript()" |

**Sweep for work files beside a chosen destination.** Fixed: Offline Deployment (above). Found and left, being the
output itself under construction rather than work files, each published only when complete: `QSaveFile`'s
temporary sibling (the verified download, the manifest, the AI store's atomic writes), `no_follow::StagedFile`'s
`.sakpart` (backups and restores), and the Windows-via-UUP converter's `<iso>.partial` beside the chosen ISO (its
workspace is already in the app's data folder). The disk benchmark and stress test write their test files to the
drive under test by design. Everything else temporary uses `QDir::tempPath()` or `QDir::temp()`, the run folder.

**Residual 5 (the C# DLL is not byte-for-byte repeatable).** The release workflow is consistent: its build job
builds `sak_utility` (with the helper and `sak_script_types.dll`, whose pin `cmake/script_types_pin.cmake` writes
from the DLL that build made) and the probe certifier the signed readiness gate drives in one `cmake --build`, stages
the package from that same `build\Release`, and builds `sak_filesystem_manifest_check` on its own, which compiles
only the filesystem tool manifest code (no program integrity, no DLL pin) and checks the repository's own manifest.
After signing, the DLL's bytes change and the same-signer rule accepts it. Only a person mixing builds (newer test
programs on an older package) hits the mismatch; AGENTS.md now says so.

**opsfix (h).** Merged at e5664dc2, its 20 commits since f39224a7, as 34ab4a62. One file conflicted,
`test_partition_manager_core.cpp`, six hunks, each kept both sides: the slot list (this round's corpus tests and
opsfix's Resize tests), the corpus (this round's coverage inputs and opsfix's writer-names payloads, both appended),
the generated-script checker (both counters, a summary line printing " ORDERED n STORAGE-VERBOSE n", and opsfix's
writer stand-in checker after it), and the guard's assertions (neither count may be 0). Everything else merged
without a conflict; opsfix's Windows Format already set this round's Windows data type back before its take. The
merged tree built clean and passed T2, 331 of 331.

**Hide on the shared removal (g).** Hide's own loop over the access paths is gone: it runs
`PartitionScriptBuilder::removePartitionAccessPathsScript`, the function a non-Windows Format runs before it takes
the partition, then hides and reads the result back as opsfix wrote it. Tests: opsfix's
`generatedScripts_hideRemovesThePathsFirstAndUnhideGivesALetter` (the whole Hide and Unhide scripts in Windows
PowerShell 5.1 against stand-ins) and the Hide text check, which now requires the shared function's text before
`-IsHidden $true`. Drill: `g-hide-uses-shared-removal`.

**opsfix's open items (j, k, l).**
- j. clang-tidy's naming check found 24 camelCase parameters and local constants on lines opsfix wrote in
  `partition_script_builder.cpp` and `partition_safety_validator.cpp` (opsfix counted 25). Each is renamed with its
  uses, by the check's own fixes for those declarations only: the two files go from 216 findings after the merge to
  192, below the 208 before it. `filesystemToolCallScriptWithArgsExpression`, left without a caller when opsfix
  removed the non-Windows Create builders and reported unused by clang (which fails the tidy gate), is removed.
- k. The ext VM gate and the physical cross-file-system validation read only `e2fsck`'s exit code. The shared
  storage helper gains `Get-SakE2fsckOutputProblem` ("likely to be corrupt", "physical size of the device", or
  "Abort? no" is a failure, opsfix's F11 rule) and `Assert-SakE2fsckPassed`, and all ten `e2fsck` runs in the two
  scripts go through it. Test: `test_partition_manager_storage_helpers` (the operation proof's output, each line
  alone, a clean run, and every call in both scripts). Drills: `k-e2fsck-corrupt-line`, `k-e2fsck-physical-size`,
  `k-e2fsck-abort-no`, `k-ext-gate-reads-output`, `k-physical-reads-output`.
- l. In practice the assistant could not run any backup-first operation: Move and Allocate Free Space (the only two
  in its list; Change Cluster Size, Serial, Primary/Logical and Dynamic to Basic are not) were refused by the safety
  validator with "requires an off-volume backup directory" even when the model named one, because only Partition
  Manager's queue binds the backup folder's volume (`backup_volume_guid`, an app-owned field the model may not send)
  and checks it again before Apply. Not a weaker run, a refusal with a misleading reason. `assistantPartitionOperation`
  now refuses all six first: "Move Partition copies the volume's files to a backup folder first, and S.A.K. checks
  that folder's drive only when the person queues the operation in Partition Manager, so the assistant does not run
  it. Ask the person to queue it in Partition Manager.", and the tool's payload description says so.
  `overwrite_confirmed` matters only to Create Image's existing-file check, not an assistant operation. Test:
  `assistantRefusesTheBackupOperationsPlainly`. Drills: `l-assistant-backup-refusal`, `l-assistant-description`.

**Screens to retake.** Network Diagnostics > Bandwidth (iPerf3's "Listen on" list, the refusal when Windows
Firewall did not add the rule: "iPerf3 server not started: Windows Firewall did not add its inbound rule (...).
Adding it needs S.A.K. running as administrator.", and a silent peer dropped after 30 s); Network Diagnostics >
LAN Transfer ("Listen on"); the About tab (its Testing item's sentence on Apple's own tools); Offline Deployment's
two new refusals ("Not enough free space for the work files: ...", and "Nothing was started: S.A.K. keeps the work
files in its own folder (...), and could not tell how much free space that drive has."); and the
assistant's new refusal of the backup-first operations (j, k, l above), which shows in its chat.

**Residuals recorded, not fixed.** "Listen on" lists IPv4 addresses only; the LAN server adds no firewall rule of
its own; an oversized embedded APFS extended attribute is not refused (its memory is now linear in its size); https
download callers are proven at the shared helper, not each caller (no TLS loopback in the tests); whether a hosted
runner has Defender running shows on the first nightly.

**Gate.** One run, alone on the PC, on 1af44e9c: full build, 0 errors and 0 compiler warnings; T2 labels at -j 6, 331 of
331; cert-tooling 6 of 6; tooling-extended 17 of 17 (test_defender_clean among them); both clang-tidy passes,
non-core 155 of 155 TUs clean, core 174 of 174 healthy with 5,291 measured findings (5,304 before the merge round)
and 21 ratcheted clean; every ci.yml static gate, 18 of 18; the hooks on every commit; the pre-push secret scan.
The first attempt stopped at clang-tidy's core pass: "missing field 'helper_operation' initializer" in the
designated initializer opsfix's read-only check route builds in partition_manager_controller.cpp (a compile
diagnostic MSVC does not give); the fields are named (1af44e9c) and the whole gate ran again.

### After the merge round: T2's bootsect.exe, the nightly's stale mutants, the volsnap settle, and opsproof's final run (2026-11-12)

On lane2 after 9170b149 (the merge round above), from GitHub's first runs of the merged tree, the operation proof's
crash investigation, and opsproof's final run on 9170b149 (F18's script half, F27 to F32). Each fix has a test that
fails without it, drilled red with the fix broken in the source and green with every byte restored. After the
drills, lizard's length and complexity limits moved six tests' new parts into tests of their own
(`fileSystemTools_hfsReadOnlyCheckIsStaged`, `scriptBuilder_hideOnMbrUsesTheTypeByte`,
`generatedScripts_mbrHideAndUnhideUseTheTypeByte`, `generatedScripts_logicalToPrimaryCountsDataPartitionsOnly`,
`generatedScripts_wipeFreeSpaceRemovesCipherFolderWithoutFollowingLinks`, and helpers for the resize cases, the
capture-failure runner and the HFS+ raw test) and split the resize-as-data functions in two; the assertions are the
ones drilled.

**T2: "calls bootsect.exe, which Windows PowerShell 5.1 does not have".** The generated-script guard
(`generatedScripts_callTheirOwnFunctionsWithDeclaredParameters`) asked the machine it ran on whether a native tool
exists, and GitHub's Windows Server runner has no `bootsect.exe`. It now asks Windows PowerShell 5.1 about cmdlets,
functions and aliases only, and `kClientSystem32Tools`, a committed list of 16 inbox System32 tools found on this
project's Windows 11 Pro PCs, about native tools; a tool not on the list is a finding wherever the guard runs (test:
`generatedScripts_commandGuardAsksTheListNotTheHost`; drill `gh-guard-host-independent`). The product: every route
that runs `bootsect.exe` checks it exists first and refuses plainly, "This Windows has no bootsect.exe (...), which
... needs, so nothing was changed": the running Windows' BIOS boot repair (which now runs the checked path), an
attached disk's BIOS repair, and Migrate OS to an MBR disk (`Assert-SakBootsectPresent`). Test:
`generatedScripts_biosRoutesRefuseWithoutBootsect`, all three in Windows PowerShell 5.1 with `bootsect.exe` absent
and stand-ins that would show a disk touched. Drills: `gh-bios-repair-checks-bootsect`, `gh-target-boot-assert`,
`gh-migrate-checks-bootsect`, `gh-attached-checks-bootsect`.

**Nightly T4: four mutation-catalog find-strings matched nothing.** Each is re-pointed at the code that now does
its job, and each was run as its catalog states it:

| Mutant | Re-pointed at | Killed by |
| --- | --- | --- |
| `partition_manager_controller.json` batch-preview-ignores-projected-inventory | the queue's preview line, now followed by `appendQueuedBackupBlocker` (opsfix) | `queueChecksEachOperationAgainstTheLayoutBeforeIt` (new): Delete All Partitions then Initialize Disk, which only the projected layout allows. Re-pointed alone it survived: opsfix's batch test now creates NTFS, which the scan's layout allows too. |
| `pst_block_decoder.json` 4k-exact-size-check-inverted | `inflateZlibExactly`'s size comparison (`bounded_inflate.cpp`, since sec2) | `unicode4kCompressedBlockInflates`, `unicode4kDecompressSizeMismatchFailsClosed` |
| `pst_block_decoder.json` 4k-exact-size-check-disabled | the same comparison | `unicode4kDecompressSizeMismatchFailsClosed` |
| `pst_block_decoder.json` 4k-decompress-failure-isEmpty-masked | inflate's own result (`(result < 0 && result > 0) || ...`, so the variable stays used and the mutant compiles; the first version, the condition deleted, did not compile). Equivalent under qUncompress, not now: a stream that inflates to exactly the declared length with a broken adler32 would be accepted. | `unicode4kCorruptChecksumFailsClosed` (new) |

Each was run as its catalog states it (built, its suite through `run_ctest_no_dialogs.ps1`): all four killed.
`run_all_mutation_catalogs.py --validate`: PASS, 61 catalogs. AGENTS.md: re-point a stale mutant, never delete it.

**The volsnap settle.** The operation proof's crash investigation (2026-11-12; its report is outside the repository)
found that on Windows 11 26100 removing a whole disk within seconds of an online scan (`chkdsk /scan`,
`Repair-Volume -Scan`) can leave a volsnap timer queued in a freed volume device object: five bugchecks across
testpc1, testpc2 and the VM. A scan-then-detach loop crashed testpc2 in its second round; with 10 s between the scan
and the detach, 24 rounds under the same load did not. After every online scan S.A.K. runs, the script now says
"Letting Windows finish with the volume before reporting the result (a drive removed right after a scan can crash
Windows)." and waits `kVolumeScanSettleSeconds` (15) before it goes on or reports the result: Check File System
(Repair-Volume for NTFS and ReFS, chkdsk without /f for FAT32 and exFAT), Scan File Systems (after each volume), the
closing scans of Defragment and of Allocate Free Space, Change Cluster Size, Move, Primary/Logical, Serial and
Dynamic to Basic, and Check Disk Errors. Check File System's result on a drive that can be unplugged (Windows calls
it removable, or it is on USB, SD, MMC or 1394) ends with "Wait a few seconds after this finishes before unplugging
the drive."; a drive inside the PC gets no such line. The settle runs in a finally around the scan
(`volumeScanSettledStatement`), passed or thrown, and a scan that threw then stops its script with its own words
(the coordinator's call the same day: a person who reads "failed" is as likely to pull the drive at once; the first
commit, 70b478cf, waited on the success path only).

| Test | What it proves |
| --- | --- |
| `generatedScripts_volumeScanSettlesBeforeDone` | The whole Check File System script in Windows PowerShell 5.1, NTFS and FAT32 passing, NTFS and exFAT throwing: the scan, its End marker, the message, `Start-Sleep -Seconds 15`, then the result in that order; a throw surfaces with its own words after the wait, and the script goes no further. |
| `generatedScripts_everyOnlineScanSettles` | Every script the corpus builds: each Repair-Volume -Scan or chkdsk call sits between volume-scan markers, Begin and End pair up, the Begin marker opens the settle's try, and its finally, right after the End marker, is the settle. Check File System and Allocate Free Space must be among the scripts with scans. |
| `buildScanVolumeScript_isReadOnly` | Check Disk Errors: the scan in the settle's try, the settle in its finally, before the result line. |
| `checkFileSystemSaysToWaitBeforeUnplugging` | The result's line for USB and for a removable SATA drive, none for NVMe. |

Drills, each red then green with every byte restored: `vs-settle-removed-from-scan-line` (both new core tests),
`vs-settle-removed-at-allocate-site` (the corpus guard alone names Allocate Free Space), `vs-settle-before-the-scan`
(both), `vs-settle-one-second` (the runtime test and Check Disk Errors' test),
`vs-settle-removed-from-check-disk-errors`, `vs-panel-note-never-added`, `vs-panel-note-for-every-drive` (NVMe got
the line), `vs-panel-usb-not-unpluggable`.

**F27: every ext Resize on GPT failed, with a false message.** Opsproof's final run on 9170b149: "Windows did
not give the partition its type back ... This operation is only supported on data partitions", the partition
still Linux-typed and its old size. `Invoke-SakResizeAsDataPartition` (opsfix 39759560) set GPT's
no-default-letter attribute before the basic-data type, and Windows sets that attribute on a data partition only
(StorageWMI 42011); its finally repeated the error and called it a type that did not come back. Now (opsproof's
patch, taken as written): the data type first, then the attribute; on the way back the attribute while the
partition still has the data type, then its own type; and "did not give the partition its type back" only when
the type read back really differs (otherwise the resize's own failure, or that the attributes could not be set
back). The unit test's stand-in had taken the attribute on any type, so it proved nothing about this: a stand-in
proves only what it models. It now refuses the attribute on a GPT partition that is not basic data, and a new
case (an access path Windows will not remove, on MBR) shows the message is not claimed when the type is back.
`PartitionScriptBuilder::resizeAsDataPartitionFunctionScript` is public so that Windows itself can be asked:
`test_raw_partition_write::resizeAsDataGivesAnExtPartitionItsTypeBack` (elevated, must-run in CI) makes a
Linux-typed GPT partition on a VHDX as S.A.K.'s ext Format leaves one (no letter, the attribute set), grows it
through the app's own function and reads back its type, attributes, letters and size, then asks for more than the
disk holds and reads back the same, with Windows' own reason and not the false message.
On testpc1 (elevated, the test program beside the merged package): red with the function as it was on
9170b149, with opsproof's own words ("RESULT THREW Windows did not give the partition its type back
({0fc63daf-8483-4772-8e79-3d69d8477de4}): This operation is only supported on data partitions."), green after.
Windows reads a Linux-typed partition's attributes its own way (the no-default-letter attribute set before the
type change reads False, and the partition reads hidden), so the test compares the partition with what Windows
read before the resize, not with a fixed line; the first green run on testpc1 showed that. Core drills:
`f27-attribute-before-type`, `f27-restore-type-before-attribute` (red recorded; the round was stopped during its
green build to take F29-F32, with the file restored byte for byte and rebuilt green before every later run),
`f27-type-back-message`.

**F28: Check Non-Windows File System failed on every healthy HFS+ volume.** Opsproof's final run on 9170b149:
"fsck_hfs -n -f //?/GLOBALROOT/Device/Harddisk4/Partition2 ... Device is not a block device ... exit code 8".
Since F25 the read-only check runs in the helper through the generated script, and the staging that repair and
format use was not applied to it; fsck_hfs opens a block device or a file only. Now (opsproof's patch, with the
long-task timeout added): the check reads the partition into a sparse image in the run folder (TEMP, which S.A.K.
sets to it), runs `fsck_hfs -n -f` on the image through `Invoke-SakCheckedHfsTool` (only exit 0 passes), removes
the image, and writes nothing back. A run folder on a FAT32 or exFAT drive is refused before anything is read,
in the same words as repair and format, which now name the check too ("S.A.K. stages HFS+ format, check, repair
and file changes as an image of the partition in its own data folder ... Run S.A.K. from an NTFS-formatted
drive"). `runReadOnlyCheckThroughHelper` hands the helper the operation, and that operation builds the staged
check; the run's output and failure reach the result in their own words
(`test_partition_manager_controller::hfsReadOnlyCheckRunsTheStagedScript`). The script: the F28 assertions in
`fileSystemTools_sayWhenTheyCouldNotOpenThePartition` (staged, `-n -f` on the image, no write-back, the image in
the run folder and removed, the drive checked first) and `hfsStaging_everyCallerRunsTheStagingFunction` (the check
among the callers). Windows itself: `test_raw_partition_write::hfsReadOnlyCheckStagesAndWritesNothing`
(elevated, must-run in CI): an HFS+ partition the app's own Format made on a GPT VHDX, checked through the app's
own script; healthy passes, a damaged volume header (file count and free-block count) fails, the partition's
SHA-256 is the same after each check, no image is left, and with the run folder on a FAT32 volume the check
refuses in those words and makes no image.
On testpc1: red with the check unstaged, as on 9170b149 ("Device is not a block device", "Filesystem tool exit
code: 8" on the healthy volume), green after. Core drills: `f28-check-not-staged` (core and controller tests),
`f28-check-writes-back`.

**F18's script half: Convert logical to primary always stopped at Apply.** Opsproof's final run on 9170b149:
"Primary/logical conversion requires a single data partition on the disk", before any change. fix9 taught the safety
validator that the extended container Windows lists beside a logical partition is not a data partition; the script's
own count, `@(Get-Partition -DiskNumber N)`, still included it (since 4500612f8). The two now share one rule in
`partition_manager_types`: `isExtendedPartitionContainer` (the validator, on the scan's Type) and
`dataPartitionsOnlyPowerShell` (the script, on the disk's Type at Apply), from one constant and the same test. Test:
`scriptBuilder_buildsOfflineMoveAndMetadataScripts` runs the generated script's own two counting lines in Windows
PowerShell 5.1 against a stand-in Get-Partition (a logical partition with its extended container passes, one primary
passes, two logical partitions in one container are refused); the validator's logical-with-container case already
ran. Drills: `f18-script-counts-extended`, `f18-shared-rule` (the shared constant broken: the validator's test and
the script's both failed).

**F29: a capture whose snapshot Windows aborted got only DISM's words.** Opsproof's VM final run: with the shadow
storage at its minimum, volsnap aborted the Image-route capture snapshot (event 36, "shadow copy storage could
not grow due to a user imposed limit") and DISM exited 3 ("The system cannot find the path specified"), not 433,
so `Get-SakCaptureFailureMessage` (opsfix e142bd5b) said nothing of the snapshot. Opsproof's patch, as written:
433 or 3, still only with a volsnap 33 or 36 for that volume since the capture began; exit 2 still gets nothing.
Test: `test_partition_os_migration::imageCaptureFailureSaysWhatStoppedIt` gains the ABORTED case (exit 3 with a
volsnap 36). Drill: `f29-exit-3`. The VM's rerun on the settle build (26e12127) still failed F29: Windows
aborted the snapshot (volsnap 36 at 18:34:58) before the capture began (DISM's error at 18:35:08; the shadow
storage filled while the target was being formatted), and the catch read volsnap's events and dism.log from the
capture's start, so the only event was dropped. Opsproof's fix19, as written: `Open-SakSnapshots` records
`$script:sakSnapshotTaken` just before the snapshot is created, and the catch reads from then, falling back to the
capture's start. Test: `imageCaptureFailureReadsFromTheSnapshot` checks the script reads from the snapshot's time.
Drills: `f29b-events-from-capture-start`, `f29b-snapshot-time-not-recorded`.

**F30 (serious): Hide on an MBR disk hid every partition on it.** Opsproof on testpc2 with a two-partition MBR VHDX:
Set-Partition -IsHidden and -NoDefaultDriveLetter are the whole disk's attributes on MBR, so Hide of one partition
hid all of them and took all their letters. Now (opsproof's patch, with the refusal moved before anything changes):
on MBR one partition is hidden by its type's hidden form (01, 04, 06, 07, 0B, 0C, 0E become 11, 14, 16, 17, 1B, 1C,
1E) and shown by its own; a type with no hidden form is refused before its paths are touched, "S.A.K. hides an MBR
partition by its type, and type 83 has no hidden form, so nothing was changed"; GPT is unchanged. Tests:
`generatedScripts_hideRemovesThePathsFirstAndUnhideGivesALetter` (the stand-ins now have a disk style and an MBR
type: MBR Hide sets 17 after the paths go and never -IsHidden, MBR Unhide sets 07 and gives a letter, an 83
partition is refused with nothing removed) and, against Windows itself,
`test_raw_partition_write::hideOnMbrHidesOnlyThatPartition` (the app's own Hide and Unhide of partition 1 of a
two-partition MBR VHDX: partition 1 hidden with no letter and type 17 while partition 2 keeps its letter and stays
visible; Unhide gives partition 1 type 07 and a letter back). On testpc1: red with Hide by the attribute, as on
9170b149 (after Hide of partition 1: "P1 True - 07" and "P2 True - 07", partition 2 hidden and its letter gone, as
on testpc2), green after. The whole elevated suite on testpc1 with the final program: 19 of 19. Core drills:
`f30-hide-by-attribute-on-mbr`, `f30-unhide-by-attribute-on-mbr`, `f30-unsupported-type-not-refused`.

**F31: Merge took the free space after the source too.** Owner's decision through the coordinator: fix it. Merge
grew the target to its supported maximum, so two 256 MiB partitions on a 1 GB disk became 1008 MiB. It now ends
where the source ended: `$mergedSize = source offset + source size - target offset`, read before the source is
removed (opsproof's patch). Test: `scriptBuilder_buildsMergeScript` (the resize to $mergedSize, never SizeMax, the
size taken before the removal). Drill: `f31-merge-grows-to-max`.

**F32: Wipe Free Space left cipher's EFSTMPWP folder at the volume root.** Opsproof's patch removed it with
`Remove-Item -Recurse -Force`, which in Windows PowerShell 5.1 goes through a junction and deletes what is behind
it; that was not taken. The folder goes only when this run made it and cipher exited 0, through the script's own
no-follow removal (`Remove-SakTreeNoFollow`: a junction or link, or the folder itself being one, is removed as the
link), and a failure to remove it is a warning, since the free space was wiped. Tests:
`stopCleanup_scriptsMarkWhatTheyMakeAndUnmake` (the removal after cipher, only for a folder this run made, through
the no-follow function, no -Recurse anywhere) and the function itself in Windows PowerShell 5.1 on a folder holding
a read-only file and a junction to a folder with a canary, then on a folder that is itself a junction: the tree
goes, the canary stays. Drills: `f32-removal-follows-links` (the canary behind the junction was deleted),
`f32-remove-item-recurse`.

**The settle in the certification tooling, and on the failure path.** The readback harness (`Test-RbVolumeHealth`
in `scripts/partition_manager_readback.ps1`) runs Repair-Volume -Scan and `chkdsk /scan`, and the rows then detach
their VHDX: the pattern of crashes 3 and 4, and very likely the VM's 0x139s during earlier certification runs. The
coordinator's call: wherever scripts/ runs an online scan and then takes a volume or disk down, wait 15 s first,
through one shared helper. `Wait-RbVolumeScanSettle` (the readback) logs "Letting Windows finish with the volume
before going on (15 s; a disk removed right after a scan can crash Windows)" and waits; `Test-RbVolumeHealth`'s two
scans run in a try whose finally calls it, and the certification rig's boot-facts job, which runs Repair-Volume -Scan
inside a throwaway VM that is then powered off and its disk detached, does the same with the function's text copied
from the readback (`Get-RigReadbackFunctionText`), not a second definition. Those are the only online scans in
scripts/. `test_partition_manager_scan_settle` (cert-tooling, pwsh and Windows PowerShell 5.1) reads every script in
scripts/: each online scan, as code or as a job's text, sits in a try whose finally calls `Wait-RbVolumeScanSettle`,
with no take-down (Dismount-DiskImage, -IsOffline $true, mountvol, a VirtualBox detach or controlvm, Remove-
PartitionAccessPath) between the scan and that finally; then it runs the readback's scan against stand-ins, passing
(scan, chkdsk, the wait, then the dirty bit) and throwing (the wait, then the same error), and runs the rig's copy of
the wait on its own. In the product, the same call put the settle in a finally around every online scan (above).
Drills, each red then green with every byte restored: `pf-settle-success-only` (the product's
wait after the scan, not in a finally: the runtime test's throwing routes, the corpus guard and Check Disk Errors'
test), `pf-settle-swallows-the-error` (a catch in the settle's try: the scan's error no longer stopped the script),
`h-readback-no-settle`, `h-readback-settle-success-only`, `h-takedown-before-settle` (a mountvol line between the scan
and its wait), `h-rig-no-settle`.

**Gate.** One run, alone on the PC, on 2cc767d5 (the code; the naming commit and this section came after):
configure; full build, 0 errors and 0 compiler warnings; T2 labels at -j 6, 331 of 331; cert-tooling 6 of 6;
tooling-extended 17 of 17; the mutation label 1 of 1 and `run_all_mutation_catalogs.py --validate` PASS (61
catalogs); both clang-tidy passes, non-core 155 of 155 TUs clean, core 174 of 174 healthy; every ci.yml static
gate, 18 of 18; the hooks on every commit. Core's measured naming findings rose from 5,291 to 5,295 there: the
four camelCase names opsproof's F28 function brought. f6b5d1ba renames them; the naming gate then measured 5,291
again, and `test_partition_manager_core` passed 449 of 449. The elevated cases ran on testpc1 (above): the whole
`test_raw_partition_write` suite, 19 of 19.

### The Debug + ASan nightly on 9170b149: two tests that never ended, tests with no time limit, and the Debug layout (2026-11-12)

Nightly 43277300774 on 9170b149 (the one on 9aa97f95 was 297 of 297): the Debug + ASan job ran into
its 330-minute limit, and six suites failed. Reproduced in a local Debug + ASan build configured as
the nightly's is (`-DENABLE_ASAN=ON -DSAK_TEST_TIMEOUT_SCALE=5`), run through
`scripts/run_ctest_no_dialogs.ps1`. None of it is a product hang. Each failure is the Debug build's
layout, which the security round (19b982d1) made matter; a Release package already has every file
beside its programs and was not affected.

**The writer CLI tests never ended, and test_single_instance_app's first instance did not start.**
The security round linked sak_utility, the helper and both writer CLIs with
`/DEPENDENTLOADFLAG:0x1000`: they resolve their imports only from their own folder and System32,
never PATH. A Debug build imports vcpkg's Debug DLLs (`zd.dll`, and for sak_utility also
`bz2d.dll`), which only PATH held: the build staged the Release names (`z.dll`, `bz2.dll`) beside
the programs. `dumpbin /dependents` of the nightly's programs as built locally: each CLI's one
import not beside it was `zd.dll`. Under ctest without `--interactive-debug-mode 0` a missing
import is a Windows error box that waits for an answer, so the CLIs never ended and sak_utility
never started ("The first instance did not start within 90 s"). Each configuration now stages its
own vcpkg DLLs beside sak_utility and both writer CLIs (`SAK_VCPKG_DEBUG_RUNTIME_DLL_GROUPS` in
CMakeLists.txt, staged for each of the three, so a build of one CLI alone has them too); every
import of the four programs resolves beside them or in System32 (dumpbin, checked locally). A
Release build stages what it did.

**Neither test had a time limit.** 283 of 361 tests set no TIMEOUT, ctest has none of its own
here (no `include(CTest)`), and `SAK_TEST_TIMEOUT_SCALE` multiplies only limits that exist. Every
test now has one: a test that sets none gets `SAK_TEST_DEFAULT_TIMEOUT`, 300 s, and configure fails
for a test registered without one (the check runs after the last `add_test`). 300 s is 4.7 times
the slowest such test in Release (`test_file_explorer_archive_service`, 64 s on the workstation)
and, scaled to 1500 s under ASan, 7 times the slowest such test there
(`test_file_management_explorer_panel`, 204 s on that nightly). `test_app_theme`, 21 s in Release
but 1182 s and 1760 s on the last two ASan nightlies, sets its own 600 s (3000 s scaled). ctest
4.4.5 ends a test's whole process tree at its limit: a test whose child slept 90 s holding its
output ended at its 8 s limit, with no process left. The writer CLI tests would now have failed
after 25 minutes, not 330.

**test_elevation_hardening ("helper is missing"), test_script_types, and all 22 of
test_partition_manager_core's failures.** Debug test programs ran from `build/tests/Debug`, while
the product looks for the helper and `sak_script_types.dll` beside the running program (security
round). Release's tests sit beside the app in `build/Release`; Debug's now sit beside it in
`build/Debug` (`CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG`), with `Qt6Testd.dll` and `qoffscreend.dll`
staged there as Release stages `Qt6Test.dll` and `qoffscreen.dll`, and the step that copied the
tools and sak_utility.exe into `tests/Debug` is gone. Core's
`inventoryScan_aCallPastItsOwnLimitSaysWhichStep` ("no progress for 6 s") was the same cause: the
scan ends itself through `Sak.ScanProcess` from that DLL, and without it the quiet limit ended it
instead. The bootsect.exe PROBLEM in that run was fixed in 2b392fed (above).

**test_program_integrity (7 failures).** It named Release's files (`Qt6Core.dll`,
`platforms\qwindows.dll`, `z.dll`); it now names the build's own (`Qt6Cored.dll`, `qwindowsd.dll`,
`zd.dll` in Debug). The program manifest pinned only Release's vcpkg DLLs, so a Debug build's
`zd.dll`, `bz2d.dll`, its own `liblzma.dll` and the ASan runtime failed the shipped-file check and
the module audit. A Debug build now compiles a list of its own (`cmake/program_manifest.cmake`,
under `_DEBUG`: vcpkg's `debug/bin` in place of `bin` and, with ENABLE_ASAN, the ASan runtime); the
Release list is line for line what it was. The helper-missing case was the layout above.

**Tests and drills.** Locally, Debug + ASan as the nightly configures it, through the no-dialogs
wrapper at -j 6: the `product` and `fuzz-extended` labels, 304 of 304 (303 passed; the
skipped one is test_single_instance_app, which starts the application and so runs only where
`SAK_TEST_ALLOW_REAL_HARDWARE` is set: its cause is read from sak_utility's imports above).
test_partition_manager_core 528 s, test_app_theme 1188 s. Drills, each red with the fix broken and
green with every byte restored: `asan-debug-dlls-release-names` (Debug staged the Release names:
both CLI tests red, the CLI exiting -1073741515, STATUS_DLL_NOT_FOUND, at once under the wrapper),
`asan-dlls-staged-for-the-app-only` (the CLIs built alone, as before: the same), `asan-tests-in-tests-debug`
("helper is missing", test_script_types red), `asan-integrity-release-names` (5 cases red: "open
failed" and the copies), `asan-debug-manifest-is-release` (the audit names the ASan runtime; the
shipped-file check names bz2d.dll, the ASan runtime and zd.dll), `asan-no-debug-offscreen`
(test_elevation_hardening 0xc0000409, "no Qt platform plugin could be initialized"),
`asan-no-debug-qttest` (the audit names Qt6Testd.dll from Qt's own folder) and
`no-default-timeout` (configure: "These tests have no TIMEOUT, so a hang would wait out the whole CI
job"). The first round's greens of the three test_program_integrity drills failed on `z.dll` and
`bz2.dll` that the first drill's red half had staged in `build\Debug`; with those removed all three
ran again, red then green. A fresh Debug build never has them, and a Debug tree from before this
change needs them deleted once: the shipped-file check names them.

**A missing DLL fails at once, and is found before the tests.** The coordinator's addition, in the
same change. Every ctest call in the workflows and under `scripts/` now passes
`--interactive-debug-mode 0` (ci.yml's T2 shards and registration query, nightly.yml's T4 job,
the coverage, branch-coverage, mutation, shard-planning, skip-audit and AI smoke runners), so a
test program that cannot load a DLL exits with 0xC0000135 instead of waiting on a box. One call
keeps the default: the nightly's Debug + ASan test step. Measured on testpc1 (2026-11-13, a
throwaway crash probe, LocalDumps set as that job sets it, then removed): at error mode 0 Windows
Error Reporting wrote a dump for an access violation and for a fail-fast; at 0x0003, what the
flag gives, it wrote neither; at 0x0001 it wrote one. ctest can give only 0x0003, so the flag
there would have left that job's dump and stack steps nothing to read. That step says so in a
`ctest-error-mode-exempt:` comment, and what guards it instead: every test's TIMEOUT, and a new
step before it. `scripts/check_ctest_error_mode.ps1` (a T2 static gate, `test_check_ctest_error_mode`)
fails on a ctest call without the flag (15 calls found: each form the tree uses, its flag on a
continued line included), allows the exemption only in nightly.yml, only with the words dump,
TIMEOUT and check_program_imports in it, and only once, and fails when it finds no workflow call
at all. `scripts/check_program_imports.ps1` reads each executable's import and delay-import tables
from the file (no dumpbin), follows the DLLs beside it transitively, and fails naming each
executable and each DLL that is neither beside it nor in System32 (API sets aside). ci.yml's
shards, nightly's T4 job and the ASan job run it on their output folder before their tests:
locally 305 executables in Release and 299 in Debug pass, in about 15 s. Drilled by moving
`zd.dll` out of the Debug folder: it named sak_apfs_writer_cli, sak_hfs_writer_cli and
sak_utility (60 missing imports in all), and passed with the DLL back. `test_check_program_imports`
runs it on a real writer CLI alone (red, naming its zlib and Qt DLLs), with the DLLs it names
copied in (green), with its zlib DLL removed again (red, naming z.dll, and zd.dll against the Debug
build), on a stand-in import table (a DLL beside the program is followed and its own missing import
named with it), and on a folder with no executables (red). Drills, each red then green with every
byte restored: `imports-not-transitive`, `imports-anything-resolves`, `imports-empty-folder-passes`,
`ctest-flag-any-value`, `ctest-exemption-anywhere`, `ctest-exemption-no-reasons`,
`ctest-no-continuation`, `ctest-strings-read-as-calls`, `ctest-two-exemptions-allowed`,
`ctest-vacuous-passes`, `ci-t2-without-flag`, `mutation-runner-without-flag`,
`nightly-exemption-without-timeout`. Not drilled: the delay-import table, since no executable here
has one to remove.

**Gate.** Alone on the PC, on 99f69abc with this change uncommitted: configure; full Release build, 0 errors
and 0 compiler warnings; T2 labels at -j 6, 333 of 333; cert-tooling 8 of 8; tooling-extended 17 of 17; the
mutation label 1 of 1 and `run_all_mutation_catalogs.py --validate` PASS (61 catalogs); the clang-tidy naming gate,
non-core 155 of 155 TUs clean and core 174 of 174 healthy (5,291 measured, unchanged); every ci.yml static gate,
19 of 19, the new one included; the hooks on the commit. The first T2 run took 9 minutes, against 5 for the same
suites minutes before and after, with every long suite 2 to 4 times slower than it runs: something else loaded the
PC, and it lost `test_partition_manager_release_claims` to process capture's cleanup budget and
`test_partition_image_volume_lock_selected_mbr_unpinned_guid` to its 210 s limit; run again, all 333 passed, those
two in 36 s and 16 s. The Release T2 run before the ctest flag and the import check were added (the same CMake and
test changes): 331 of 331. The coverage job has no import step: it builds RelWithDebInfo, whose tests run from
`build/tests/RelWithDebInfo` with Qt on PATH, and its sixteen core suites start none of the four programs. release.yml
runs no ctest.

### Merge of ops: the Partition Manager operation proof onto lane2 (2026-11-13)

The `ops` branch (89139e37, 73 commits since 5585dca5) merged into lane2 after the ASan fix: the
operation-proof driver (`scripts/run_partition_manager_operation_proof.ps1`), its plan, rows,
checks, VM hygiene and Defender records, and the `-OperationProof` paths of the certification
runner and the rig. Where both sides changed a file, lane2's version is kept. The step client
(`partition_manager_scenario_step.cpp`) had reached lane2 already (3259cccc, the opsfix commits,
the fix1 patch), so the merged file is lane2's; its one conflict, the assistant route, keeps
`sak::assistantPartitionOperation` (e3c021ed) over the `makeOperation` call that commit replaced.
`tests/CMakeLists.txt` and `test_partition_manager_live_certifier.ps1` carried the same change on
both sides. The certification runner's one conflict, its param block, keeps both lane2's
`-ApprovalPath` and ops's `-OperationProof`.

Made to hold lane2's rules, which ops's scripts predate, each a T2 or cert-tooling test that
failed on the merge as it came:

- **The settle** (`test_partition_manager_scan_settle`, cert-tooling). The operation proof's own
  readers ran chkdsk's and Repair-Volume's online scans with no settle after them, and rows then
  take their virtual disks down. Each scan now settles in a finally through
  `Wait-RbVolumeScanSettle`; `-NoSettle` keeps the volsnap drills' own timing (Drill D none,
  Drill E its `-SettleSeconds`), as `Close-OpsRowVolumes -NoSettle` already did, and
  `Close-OpsRowVolumes` waits through the shared helper instead of its own 15 s sleep. The rows
  and the checks refuse to load without the readback, which the runner loads first. The guard
  itself: it now lists scripts by extension (Windows PowerShell 5.1 ignores `-Include` beside
  `-LiteralPath`, so its 5.1 run had been reading every file under scripts/, the plan's data
  among them), and a comment naming the readback no longer counts as loading it. Drills, each
  red then green: `ops-chkdsk-without-settle`, `ops-refs-scan-without-settle`,
  `ops-checks-without-readback` (red only once comments stopped counting), `guard-include-listing`.
- **No `-ErrorAction SilentlyContinue` in a `run_partition_manager_*.ps1`**
  (`test_partition_manager_storage_helpers`, cert). The driver had eight. Listings of a folder
  that may not exist yet (the PC's claims, the run's output, the runs folder) test for it first
  and stop on any other error; a stale virtual disk is dismounted only when Windows has it
  attached; the run's own temporary files (a job script removing itself, the unpacked package,
  the scratch folder) use `-ErrorAction Ignore`, as the group runner removes its job file.
- **Certification rows stop at the first that fails** (`test_partition_manager_rig_pc_target`,
  cert): the runner now runs every row only under `-OperationProof`; the test reads that form.

Not run here: the operation proof itself (testpc2 and the VM). Its `-PlanOnly` lists 116 rows
(device 3, none 2, testpc2 102, vm 9) and the plan agrees with the row bodies.

**Checks**, on the merge staged, alone on the PC: configure and the Release build, 0 errors and 0 compiler
warnings (the code digest changes, as expected); cert-tooling 8 of 8; `test_partition_manager_row_evidence`, its
operation-proof guard included; the T2 labels, 333 of 333; `-PlanOnly`; every ci.yml static gate, 19 of 19; the
hooks on the merge commit. Drills for the two T2 fixes, red then green: `ops-driver-silentlycontinue`,
`runner-stops-proof-rows-too`.

### Fix batch 3: what the screenshot retakes and the 1db87df4 recertification found (2026-11-13)

On the `fix3` branch from 1db87df4, one commit per item, each test drilled red with the fix broken and green
with every byte restored.

**iPerf3 server refusal.** Unelevated, a server on an address other PCs can reach said "iPerf3 server not
started: Windows Firewall did not add its inbound rule (The requested operation requires elevation (Run as
administrator).). Adding it needs S.A.K. running as administrator." Three faults: the status bar's permanent
widgets (the Standard label and Run as Admin) cut it off at "S.A", so the instruction was lost; netsh's own
sentence gave nested brackets and ".)."; and it named administrator rights as the cause even when S.A.K. was
elevated and the rule failed for another reason. Now (`bandwidth_tester.cpp`): unelevated, such a start is
refused before anything runs (no stale-rule sweep, no server process, no netsh) with "Run as Admin to start the
iPerf3 server: adding its firewall rule needs administrator rights.", leading with the action and naming the
status bar's button by its own text. Elevated, a rule Windows still does not add says "iPerf3 server not
started: Windows Firewall did not add its inbound rule: <netsh's words>." with netsh's trailing punctuation
dropped, so it ends with one full stop, and no claim of a cause. Loopback is unchanged: no rule, so no
administrator. The Bandwidth tab says it before Start Server is pressed: Start Server's tooltip ends "(requires
admin on this address, for its firewall rule: Run as Admin)", as Reset Network's says "(requires admin)", and the
stopped server reads "Server: Stopped (needs Run as Admin)", for a non-loopback Listen on address only.
Tests: `test_bandwidth_tester` (unelevated, refused at once with no process started and nothing late; elevated,
injected with `setElevatedForTesting`, the rule attempted and refused by the test hardware guard, netsh's reason
and no administrator claim; loopback unelevated starts; `serverNeedsElevation` for IPv4, IPv6 and loopback; the
message's punctuation for a reason ending in ".", ")", "!", ";", "...", spaces or nothing),
`test_network_diagnostic_panel` (the note on a LAN address and not on 127.0.0.1, and Start Server's refusal).
Drills: `iperf-no-up-front-refusal`, `iperf-sweep-before-refusal`, `iperf-loopback-needs-admin`,
`iperf-old-admin-claim`, `iperf-punctuation-kept`, `panel-no-admin-note`, `panel-note-ignores-loopback`.

**vhd.clear-disk-wipe on the 1db87df4 recertification.** Its step `refused_on_thin` failed: 9623ad18 moved Wipe
Disk's thin-provisioned refusal from Apply to the queue, so the step client reported "Not added to the queue: Wipe
Disk needs a full format, and Windows does not allow one on a thin-provisioned disk, such as a virtual disk file.
Nothing was changed." as Failed, while the row still asked for `failed` (an Apply that failed). The cert agent's
static check of every step against the queue refusals that 9623ad18, 66ecdd53, f5e55dfd, 51b318da and opsfix
moved found this one alone. The row body and the matrix generator now both say `refused` (the matrix regenerated,
never edited), and no matrix step expects `failed` any more; `expectedFailure` stays. The row's readback still
proves the thin disk unchanged: its raw table and its file manifest before and after. A refused step needs no
helper connection (`Test-PmStep`), which a queue refusal never makes. And so the class cannot recur silently: a
step that expected Apply to fail and was refused before Apply now reports "expected Apply to fail; it was refused
at queue: <text>" (or "at apply"), self-test `step-a-failure-refused-at-queue-says-so`. Tests:
`test_partition_manager_row_evidence` reads every row body's `Invoke-CertStep -Step ... -Expect` (to the end of
the call, past a multi-line `-Choices` table) and the matrix, and requires them to agree, this row a refusal in
both; it accepts a queue-time refusal for a `refused` step (no execution, no helper), names a matrix that still
asks for `failed`, and refuses a refused step whose client failed. Drills: `thin-row-body-expects-failed`,
`thin-matrix-expects-failed`, `evidence-wants-a-helper-for-refusals`, `body-reader-first-line-only`,
`step-client-plain-queue-wording`. Not drilled: the two call sites of the new wording (no local test reaches a
real queue refusal), only the function both call.

### UI review round 9: review 4's findings and the screens no picture had covered (2026-11-07)

From the independent review of round 8's pictures (`ui-review\recheck4.md`: two regressions and 28 new findings) and
the coordinator's item list, on dev at a4049894. Every item was confirmed on that commit first. Every fix has a
test, and every test was drilled red with its fix broken in the source, one guard at a time, by a script that
restores every byte and touches the file. 78 guards, each red in the end. Eight did not go red on their first run.
Y2's pending suite steps survived because the change was not one (see Mediums); it and its test were taken out. Y9
survived because offscreen, without the theme, a row is short enough that the old list's minimum height held three;
the test now applies the theme first, and the old code then shows two whole rows in 74 px. Y13 survived because the
target list fitted the offscreen window even with the page stack as tall as its tallest page; the test now also
checks that the stack asks for the height of the page it shows. Three Y3 guards changed one colour of a redrawn icon
and the rest of the drawing carried it (light AI Assistant, dark Backup and Restore, dark Partition Manager); each
now restores the whole old drawing and goes red. The dark stethoscope tube's guard survived even with the whole old
drawing restored (56% of the icon's pixels at 3:1), which is why the quarter rule below was added; with it, red. One
guard (light Partition Manager) first failed to build because the test executable was still locked by the run before
it; rerun, red. The guards of the functions split for lizard (Y13's page fitting, Y17's two checks, Y18, Y21 and
Y23) ran on the split code.

**Mediums.**
- RG4, the AI workflow picker's list. Qt's Windows styles open a combo box's list at the box's own width, and
  round 7 had taken out the 420 px minimum, so the list cut its names behind its scroll bar and its 18 rows ran
  39 px past the window. The AI page's five pickers are now WindowFittedComboBox: the list opens at its widest
  entry's width (the view's own measure of its items, its frame and, when the rows scroll, its scroll bar), capped
  at the window, and keeps inside the window, beside the field where it fits and otherwise on the roomier side, cut
  to fit. The field ends a name too long for it in an ellipsis (DF11's rest: with the side panel's scroll bar
  beside it the picker cut "Technician Service Report (5 phas" through a glyph); the whole name is in the list and
  the tooltip.
- Y1, disabled menu entries. The theme's QWidget colour applied to every menu entry, so a disabled one kept the
  enabled label and only its icon faded; Convert Dynamic Disk to Basic on a basic disk read as usable. QMenu::item:
  disabled now takes the disabled label's colour, #606f85 on the light menu and #94a3b8 on the dark one, as a
  disabled button's label does. It is in the application's style sheet, so every menu and context menu has it.
- Y2, "Downloading Windows UUP files..." in emerald #059669, 3.60:1 and 3.89:1. Both ISO dialogs' download phase
  takes the success text token (#117f3a light, #16a54b dark); the emerald token had no other user and is gone.
  The purple and running-blue phase colours reach AA in both themes and are now held by the test with it. The
  sweep of raw colour tokens used as text found the benchmark suite's pending steps in the disabled-text token,
  which looked like #94a3b8 (2.56:1) on the light page. Not a defect: a style sheet takes that token as
  palette(midlight), the muted colour, as it takes the muted token, so the steps already read at the muted
  colour. A change to the muted token and its test survived the drill for that reason and were taken out.

**Byte formatting (Y10, X7).** Six formatters became one: sak::formatBytes in format_utils.h ("512 B", "1.5 KB",
"4.88 GB", "7.28 TB": KB and MB to one decimal, GB and TB to two), with a qint64 overload that shows a negative
size as such. formatBytesCompact, the backup wizard's own GB formatter ("4.9 GB", "0.0 GB"), the AI page's integer
"(3 KB)", Advanced Search's "242 bytes", Partition Manager's own precision constant and a dozen local
QString::number(.../1024...) sites in the workers, the ISO tools, the flasher and the reports now call it.
test_one_byte_format reads every string literal under src/ and include/, comments left out, and refuses one that
puts a byte unit after a placeholder ("%1 MB") or after a number built in code (" MB"), which is how a size is
formatted by hand, so a seventh cannot appear; rates ("MB/s"), spin box suffixes and nine messages that name a
setting in the unit it is set in ("File limit: %1 MB") are listed in it with the reason. Tests that pinned the
old wording ("0 bytes", "500 bytes/s", "Total size: 42 bytes", "60 bytes / 100 bytes") pin the one style now.

**Wording (Y11, Y19, Y20).** "WiFi" was in 88 string literals; it reads "Wi-Fi" in all of them, and the adapter
type the data keeps ("WiFi" in JSON exports, the AI tools' arguments and comparisons) is shown through
sak::adapterTypeLabel. The backup and restore pages that keep every adapter's IP settings say "Network Adapter
Settings" and "network adapters", the backup's button is "Scan Adapters", and the restore says "network settings
for 'Wi-Fi'". test_user_text_spelling refuses "WiFi" and a lowercase "ethernet" in any string literal (a Settings
address such as ms-settings:network-ethernet is not wording). Y20's rest: "Found 1 saved Wi-Fi profile" has no
full stop; Email Inspector says "3 results" as its list does; Compare Panes lists only the sections with entries
and says when both panes match; Space Analyzer hides a Type column with one value in every row; Copy Partition's
review says a raw device or image target is "as large as the source" (was "custom/unknown"); and Copy Disk and
Migrate OS name a disk "Disk 1 (\\.\PhysicalDrive1)" in both the bar and the summary.

**Lows.**
- RG5: a partition with no letter showed "*:" in its floor-width segment. The review bars name it as Windows
  does: "EFI", "MSR", "Recovery", else "Partition N".
- Y3: every tab icon now has most of its pixels at 3:1 on its tab in both themes, read pixel by pixel as the
  reviewer reads them; round 8's test held only four on the light tab. Light: AI Assistant (0% at 3:1 offscreen; the
  reviewer never saw it unselected), Help and Support, About, Partition Manager, Email Tools and Network Management
  are drawn in darker tones, with the old drawings kept as the "-dark" files the dark theme picks. Help's question
  mark and About's "i" are drawn bolder so the selected tab's white drawing still has a glyph-shaped hole (with the
  darker discs, the anti-aliased glyph fell under the near-white floor and the selected icons became blank discs,
  which the test caught). Network Management's outline was a quarter pixel wide at 20 px, all anti-aliased edge; it
  is a whole pixel. Dark: Partition Manager and Network Management lighter, Backup and Restore's arrowhead a lighter
  blue (its share was 50%), and Benchmark and Diagnostics' stethoscope tube drawn in light grey at the light
  drawing's width (it was #36404d, about 1.3:1 on the dark tab). The tube is a small part: with the old drawing the
  whole icon still had 56% of its pixels at 3:1, and its drill survived. The test now also reads each quarter of an
  icon that holds a tenth of its ink and wants a third or more of that quarter's pixels at 3:1, so no part of a
  drawing can be mostly lost behind the rest: the old tube's quarter read 0.15, and every drawing now reads 0.41 or
  more (light Benchmark and Diagnostics 0.41 and Partition Manager 0.42 are the least). A third, not a half, is a
  judgement: a quarter's share is not the whole icon's, and half would ask round 8's approved light drawings to be
  redrawn again. Offscreen readings (whole icon, weakest quarter), light then dark: light AI Assistant 0.79 and
  0.70, Backup and Restore 0.65 and 0.58, File Management 0.96 and 0.93, Partition Manager 0.61 and 0.42, Image
  Flasher 0.81 and 0.75, Benchmark and Diagnostics 0.62 and 0.41, Email Tools 0.57 and 0.55, Application Management
  0.58 and 0.50, Network Management 0.86 and 0.83, Help and Support 0.75 and 0.68, About 0.88 and 0.87; dark AI
  Assistant 0.81 and 0.70, Backup and Restore 0.51 and 0.47, File Management 1.00 and 1.00, Partition Manager 0.88
  and 0.81, Image Flasher 0.82 and 0.75, Benchmark and Diagnostics 0.71 and 0.51, Email Tools 0.61 and 0.60,
  Application Management 0.88 and 0.83, Network Management 0.89 and 0.88, Help and Support 0.87 and 0.87, About 0.80
  and 0.78. Dark Backup and Restore is the closest to the bar; its remaining misses are the anti-aliased edges of
  its ring. Panel headers follow the theme as the tabs do.
- Y4: an enabled button icon is a one-colour glyph in its button's label colour (white on the toned buttons),
  through the same toneGlyphPixmap the selected tab uses, so Attach, Instructions and Clear look alike.
- Y5: the File Explorer details pane's text, headings and preview start at one inset, 12 px; a styled label
  indents its text by half an "x" unless told not to, which put the headings 6 px right of their lines.
- Y6: only the active pane's footer says "Active pane"; the other is reset to its own summary.
- Y7: Advanced Search's preview is set in Consolas, so its line numbers line up.
- Y8 and Y16: tables size their columns to what they hold and give one column the rest (fitColumnsToContents):
  the backup's Installed Applications, App Data and network adapters, the restore's folders, network adapters
  and applications, Advanced Uninstall's Publisher, the SMART table and the Vulnerability Scanner, whose
  Published column shows the day and whose recommendations wrap whole (a word-wrapping delegate sizes the row).
- Y9: the ISO builds list shows four whole rows in every state.
- Y12: one primary button per surface: File Organizer's and Duplicate Finder's pages, Application Management,
  the email file scan (Open Selected) and the flasher, whose Settings and Back are secondary. The raw flash's erase
  confirmation has a red Yes and No as its default, as Batch Uninstall's destructive action is red.
- Y13: Image Flasher's step buttons have their own row above the Log switch, and the page stack is as tall as the
  page it shows, so the target list ends above the row.
- Y14: Check Disk says one sentence per drive (the raw "NoErrorsFound" is gone); Optimize Power names Windows'
  "High performance" plan as Windows does.
- Y15: the AI status bar says "Last turn: ..." and "Session: ...".
- Y17: Resize/Move says the Support text once, full width, with no label beside it.
- Y18: the flasher settings have one field column, the widest label's, and give each unit once ("Buffer size:"
  with "64 MB").
- Y21: every page's first group box or table sits 12 px inside its pane. The four the review named (Benchmark and
  Diagnostics, the network adapters, File Organizer, the Vulnerability Scanner) and, found by the same test, the
  eleven network tool pages, Wi-Fi Manager and App Installation's Online Install and Offline Deploy tabs. The File
  Explorer is left out: it is a file manager's shell (sidebar, panes, details pane) laid out to its own 8 px gaps,
  with no group boxes.
- Y23: until Scan SMART has run, the Thermal drive row says "N/A - not read yet; Scan SMART reads it", not "not
  reported by this PC".
- Y24: the restore's two option boxes share one width (made good after review 5's first pictures; below).
- Y25: the workflow pane's document no longer repeats the pane's title as its H1.
- Y26: a score bar is blank until there is a score and no longer names the baseline; the Score line names it
  ("baseline Samsung 980 PRO = 1000"); CPU lists its two results one a line.
- Y27: a finished backup leaves only Finish (no Cancel, no spent Start Backup); going back from the page gives
  the earlier pages their Cancel again.
- Y28: a path in the Details pane carries a zero-width break chance after each backslash and a word joiner after
  each hyphen, so "C:\SAK-Samples\Docs" breaks after a backslash, not at "SAK-"; detailsText() and a copy strip
  them, so what is copied is the path as given (round 8's "no character is inserted" no longer holds on screen,
  and holds for everything that leaves the pane). The data recovery review's path takes the same marks.
- N9: a Store app with no O= in its certificate's subject is named by its package manifest's
  PublisherDisplayName, as the Store names it, not "CN=5699BA41-...".
- S22: the hidden File Explorer sub-tab no longer posts "Explorer loaded 32 items" to the status bar while File
  Organizer shows; a panel forwards status only while visible.
- DF11's rest is under RG4.

**Not defects, and judgement calls.** Y22 (a 100% bar beside finished messages): the bar holds at 100% for 1.5 s
and then hides; the pictures were taken inside that hold, and the disk-menu shot follows a right-click that came
within it. No change; review 5's pictures are taken after the hold. One byte style app-wide, "B", short as the
lists already were, and a progress line's "60 B / 100 B" with it. The download phase is success green, as the item
list said, rather than a new informational colour. "*:" is replaced by real names rather than left blank: a
technician reading a migrate bar needs to know which sliver is the EFI partition. The Support text is full width
without a label rather than top-aligned beside one: it is a sentence, not a value. Y28's marks are invisible
characters in the text shown, not a custom text layout: Qt's line breaker follows Unicode's rules, which allow a
break after a hyphen and none after a backslash, so the marks are the smallest change that breaks a path where a
person would, and they never leave the pane. Y12: one primary per surface, the action that does the surface's
work; Clear All and other destructive or secondary actions are secondary or danger-toned.

**Before the push.** The suites these changes reach, then the T2 labels three times at -j 6, 336 tests each
(test_startup_budget and test_single_instance_app skip themselves), the second and third with the whole-tree static
gates running beside them: all green, and the 19 static gates clean. The first label run, on the build before the
function splits for lizard, had eight suites red: six pinned the old byte wording and now pin the one style; the
Details test compared the shown path without taking out its break marks; one test read CPU's two result lines on a
hidden tab, where nothing is laid out. The rest were real and are fixed, each with a guard above: the details
headings' 6 px indent, the 8 px insets of Wi-Fi Manager, the network tool pages and App Installation's inner tabs,
the light AI Assistant tab icon at 0% (never seen unselected in a review picture), Network Management under half in
both themes, and the selected Help and About tabs gone to blank discs once their discs were darkened. The commit
hooks' cppcheck then asked for a pointer to const in the File Explorer test's Safety-pane helper; that test was
rebuilt and rerun before the commit.

**Found by review 5's first picture.** T2 run 42439404645 on efd9ea3a passed. The light app_main_window from that
commit's package then showed the disabled Rename Chat icon as an "!". Y4's glyph rule had taken the selected tab's:
every pale part of a drawing (every channel 185 or more) is left out as a hole. Rename Chat's drawing is a white
field holding a pale blue selection (#c2e8ff) and a thin cursor, so only the cursor was left. A button's glyph now
leaves out only what is drawn white (every channel 240 or more), and it decides that on the drawing at four times
its size, then scales the glyph down: read at button size, Run Details' white "i" is nowhere a whole pixel wide, no
whiter than a pale fill, and it would have been filled in. buttonGlyphsKeepTheirPaleFills checks both, and three
guards went red: the old floor, no oversampling, and enabled icons left in their own colours (Y4's guard, moved to
the new code). The selected tab keeps its own floor, 185, under which the artwork's light fills are its holes. Then
the labels three times at -j 6 (336 each, the first beside the static gates), green.

**Found by review 5's light pictures, before the dark ones.** T2 run 42447591779 on 1c532c64 passed. Looking at the
light restore pictures from that commit's package found two of this round's own items not done. Y24:
restore_wizard_permissions still showed the two option boxes 145 px and 115 px wide. The fix had set both to the
wider one's size hint when the page was built, before the theme's style sheet had polished them, so the hints lacked
its padding and each box took its own larger hint on screen; the test compared those same unpolished hints and
passed. The page now gives both boxes one width when it is entered, read after polishing them, and the test shows
the page in the theme and compares the boxes' real widths. Y19: the restore's plan on that page said "Ethernet
adapters (4)" for four adapters of which two were Wi-Fi and Bluetooth, and the restore welcome page ("Networks &
Ethernet"), the network page's integrity message, the backup's log ("Saved 4 Ethernet configurations") and the
restore worker's messages said Ethernet for the same settings. They say "network adapters" and "network adapter
settings" now, and test_user_text_spelling refuses "Ethernet" in any string literal of the user-profile backup and
restore sources. Four guards went red: the widths set without polishing, the widths not set on entry, the plan's old
line, and one worker message put back; all four again on the final code, after the session that wrote them stopped
(VS Code crashed late on 2026-11-07 and took the round's agent with it; a second session finished the round).

**Found by review 5's pictures, and by the label runs.**
- File Explorer's command palette listed a command that cannot run yet ("Compare Panes - Enable dual pane first.")
  in full-contrast text, #0f172a and #f8fafc, as dark as the commands that run. Round 6 had made it a disabled row,
  and its test checked the row's flags, not how it is drawn: the theme's text colour overrode Qt's disabled palette,
  as it had for menu entries (Y1). QAbstractItemView::item:disabled now takes the disabled label's colour in both
  themes, beside Y1's rule, so every list, table and tree draws a row it cannot use greyed (Partition Manager's
  boot-disk entry in a target list and the restore's unmatched applications were already meant to read that way).
  disabledListRowsLookDisabled reads a themed list's two rows as Y1's test reads a menu's.
- The view menu's Dual Pane read the same on and off: under the theme a checked entry with an icon draws the icon
  where the tick goes. Round 6's "Dual Pane shows a check" held for the action's state, which its test read, and not
  on screen. The entry has no icon now and shows a tick, as Hidden Items and File Extensions do; the layouts keep
  their icons (one exclusive choice, which the view button shows with its own icon). dualPaneMenuItemShowsItsState
  also checks that no on/off toggle in the menu carries an icon.
- test_worker_base failed once in the label run beside the static gates: its direct-destruction probe, a child that
  must wait out the 15 s stop deadline and then stop through qFatal, was allowed 5 s for starting a Qt process and
  Windows' fail-fast handling, and took more. The bound only has to catch a destructor that never stops, so it allows
  45 s now. A guard that makes the destructor wait forever still goes red.
- The last label run before this one (suite9e) timed out test_partition_manager_panel at 155.65 s (120 s limit; 7 to
  12 s in every other run). Its log stops after scanFileSystemsIsDiskLevelOnly passed, so it stalled in
  spaceAnalyzerExposesCommercialViews, which only writes, scans and removes two files in a temporary folder, or in its
  cleanup. The process took 35 s to die after ctest's kill, as one held in the kernel does, not one looping in the
  test. At that minute Windows Update failed a Store install and deleted the oldest C: shadow copy, and the machine
  logged GPU and network-driver live kernel dumps shortly after (VS Code crashed then). It did not happen again: 25
  runs idle and 25 beside 16 busy threads, with QtTest's own watchdog set to name a stalled function, all passed in 7
  to 13 s, as did every label run since. No code change; recorded as not reproduced.
- Y12's rest: review 4 named Quick Partition among the surfaces with too many strong buttons, and round 9 left it
  out; review 5's picture showed the preset row's Save in the primary tone beside Add to Queue. Load, Save and
  Delete are secondary now (quickPartitionHasOnePrimaryButton, drilled red).
- T2 on 5e10f6d7 failed twice before building anything: every shard's Toolchain step got "504 Gateway Time-out"
  from GitHub's release downloads (ripgrep, cppcheck), as did a download from the workstation. The pinned CI
  downloads now go through .github/actions/windows-build-env/pinned_download.ps1: a copy restored by actions/cache
  (keyed by the pinned SHA-256) is used only when it hashes to the pin; a download is tried up to four times,
  10, 30 and 60 s apart, when the server answers 5xx or not at all; any other HTTP error, and any file that is not
  the pin, fails at once. ripgrep, CMake and cppcheck in the toolchain action, nightly's LLVM and release's
  gitleaks and trufflehog use it; secret-scan.yml's curl retries the same outages. test_pinned_download (tooling)
  holds the helper's rules against a stand-in for Invoke-WebRequest, five guards drilled red; the workflows
  themselves are proved by T2 once GitHub's downloads answer.
- Leftovers after an MSI uninstall. winget now installs Notepad++ from its MSI: "Notepad++ (x64)", an Uninstall
  key named by its product code, no InstallLocation, UninstallString "MsiExec.exe /X{...}". On testpc2, with the
  restore point and auto-clean unticked, S.A.K. removed it and its scan reported nothing ("0 programs", "Ready", no
  Leftover Items) while %APPDATA%\Notepad++ (8 entries) stayed: review 3's fallback names a program by the folder
  its uninstaller sits in, for an MSI System32, and "notepad++ (x64)" is no folder's name. Two changes. Before
  the native uninstaller runs (Standard and Forced), and in the assistant's
  leftover preview, LeftoverScanner::withWindowsInstallerLocation asks Windows Installer for the product's
  InstallLocation and, when that is empty too, the installed paths of the components listed in its cached package
  (opened read-only): the nearest folder above each that is named for the product (its leaf, or a "Vendor\App"
  pair equal to the name, so "Google\Chrome" never widens to "Google" and "Notepad++\plugins" never stands for
  "Notepad++"), the one holding the most components; registry key paths are skipped. That folder becomes the
  install location and is screened like any other. Second, the display name without a trailing architecture tag
  ("(x64)", "(x86)", "(64-bit)", "(32-bit)", alone or together) is a name-only match: at least four letters or
  digits, not the publisher, a well-known vendor or a shared folder name, and unused while any other installed
  program has that name (its display name with or without the tag, its install folder or its uninstaller's
  folder, read from the registry when the scan starts; an incomplete read refuses it). Anything found only through
  it is Risky with "matched only by the name ..." in its description, so it is never ticked and never
  auto-cleaned. Four tests in test_leftover_scanner, nine guards drilled red (the match, self not counted as
  another program, each of the three ways another program has the name, Risky, the pair rule, the nearest folder,
  registry paths skipped), all nine again after the code was reshaped for the magic-number and lizard gates.
  The labels at -j 6 then failed test_stage_portable_release and test_windows_runtime_dependencies on the new
  msi.dll import until windows_runtime_dlls.ps1 listed it as a System32 DLL; with it, 337 of 337 and the static
  gates green.
- Judgement, not changed (Y20's rest): Compare Panes' message box still wraps "staff-list.csv" at its hyphen. Y28's
  word joiners keep a path whole in the Details pane because every way out of the pane strips them; a message box's
  text is also logged and copied with Ctrl+C, so marks put there would leave with it. A hyphen is an ordinary break
  point in a message.
- The whole-tree lizard gate run from lane2 reported 17 parse desyncs in files nobody had changed: lane2 has no .venv,
  so run_lizard.py ran the first lizard on PATH, the user site's 1.21.2 (AGENTS.local.md: broken). With the main
  tree's 1.24.3 first on PATH it passes. Not a code issue; the desync check made the wrong tool fail loudly.

Then the labels at -j 6, 336 tests a run: before the last three fixes, one run beside the static gates
(test_worker_base red, above) and one green; with them, two runs, the first beside the static gates, green; and,
after cppcheck asked for the two option boxes to be named rather than looped over, one run, green. The run
before that one failed test_partition_manager_live_certifier ("Raw source sidecar hash mismatch") only because
commits were made while its build ran: pre-commit put the committed versions of the unstaged files back for a
minute, and the build compiled and fingerprinted that mix. AGENTS.md now lists the trap.

**What no test here covers.** Whether each reworded line reads well is review, not test. The icon readings are
offscreen at 20 px; the pictures are the real tab. A list opening beside its field is measured on the offscreen
platform's screen; review 5's ai_workflow_list and ai_access_options show it on the real one.

**Review 5: pictures of what round 9 changed, and of the screens no picture had covered.** 75 shots in both themes,
150 rows: 148 pictures (74 light, 74 dark), each passed through the harness's privacy gate and looked at by eye, in
E:\SAK-Utility-Test-Scratch\capture\review5 with an index.json in review 4's form that names the item or items each
picture proves and the run, machine and package it came from. Every package was built from lane2's build tree at a
pushed head and installed by the harness: r9b (1c532c64, SHA-256 E7529BA7...A7CD) for 88 pictures, the first
session's runs and this session's first testpc2 runs; r9c (5e10f6d7, 177F31BF...D609) for 50, the retakes after the
Y24, Y19, palette and Dual Pane fixes; r9d (d0862b35, 112DCD96...482C) for 8, Quick Partition and the light
benchmark, USB wait and NSIS leftovers retakes; r9e (555e2d38, CB936969...C42B) for the 2 MSI leftovers pictures.
Machines: testpc2 for 82, the capture VM for 42 (the restore wizard, the pages that list accounts, Rig A), testpc1
for 24 (the first session's, before it stopped). 67 rows ran variants, named in each row: testpc2's spare
Samsung NVMe as W: WORK for the benchmark, its disposable Samsung Flash Drive (serial AA00000000000489, pinned
before every write, large-drive threshold 512 GB) for the flasher and Quick Partition, a 1.2 MB test image labelled
WIN11_SAK_TEST for the USB wait (testpc2 has no Windows ISO), a 2 GB random image so the flasher is pictured
mid-write, the tenths shot taken 2 s into a real UUP download, and on the VM a copy of the sample backup with one
folder taken out so the restore ends with problems without touching the network. What round 9's items show: the
restore's two option boxes one width, "Network adapters (1)" and "Wi-Fi & Network Adapters" (Y24, Y19); the backup's
last page with Finish only (Y27); "Restore Finished with Problems" with one verdict (RG3/N10); the command palette's
"Compare Panes - Enable dual pane first." greyed and Dual Pane's tick; Offline Deploy with no bar held at 100%; the
flasher at 6% of a 2 GB image; "0.0%" and "0.1%" on the UUP download bar (X19); the disk benchmark on W: and its log
"Starting disk benchmark on W:\..." with one backslash; "Waiting for Windows to recognize the new partition... 0 s of
180 s"; Quick Partition's Load, Save and Delete secondary beside Add to Queue (r9d); Notepad++'s leftovers listed
for its NSIS build (r9d) and its MSI build (r9e, through the folder Windows Installer gave), both
%APPDATA%\Notepad++ and HKLM\SOFTWARE\WOW6432Node\Notepad++, Safe. Not captured, in either theme:
partition_manager_no_disks (the message needs a disk scan that fails and finds no disks; no test machine fails its
storage scan without breaking its storage stack; test_partition_manager_panel aScanThatFoundNoDisksSaysWhy holds
it). The gate masked addresses and MACs in net_adapters_table and backup_wizard_ethernet and the saved networks'
names in backup_wizard_wifi. Found while capturing, not defects of the app: the harness runs a shot elevated only
when its notes name administrator rights, so the USB wait and tenths shots first ran unelevated; a "Cancel" step
matched a disabled control of that name (the button is "Cancel Flash"); NSIS draws its buttons as Win32 buttons
that UI Automation lists as panes, so the uninstaller's prompts were answered by class name and BM_CLICK; winget
now installs Notepad++ from its MSI, which is how the MSI gap above was found; the VM did not sign in by itself
after a restart.

**Seen in the pictures, not changed.** For the next review to judge: Advanced Uninstall's list page has three
primary buttons (Refresh, Batch, Settings; Y12 named other pages); the finished restore still offers "< Back" and
"Cancel" beside Finish; Partition Manager's main table and disk map label a partition with no letter "*:" (RG5
named them in the review dialogs only); the flasher's page during the partition wait shows one line and no Cancel;
a benchmark run as a standard user logs "ERROR: Failed to read SMART data for PhysicalDrive2" for the USB stick;
the leftovers pictures are taken while the program list refreshes, so the removed program's row shows under
"Loading installed programs..."; File Explorer's Name column cuts the settings screenshots' names while Type says
"File" for a PNG; and Screenshot Settings still names its Wi-Fi file "WiFi_Settings" and says so in its "Not on
this PC" line.

**Left behind.** On testpc2: W: and its build tree; the harness's standing setup (C:\SAKCapture's scripts and its
SAKCapture tasks, C:\SAK with the r9e package, C:\SakWork's setup scripts); C:\SAK-Samples, empty; the Balanced
power plan; the console at Intel primary 1600x900 again (it had drifted during the round to the virtual display as
primary at 1920x1080 with the Intel output at 1024x768 beside it, and display.ps1 set put back the README's layout);
and the disposable Samsung Flash Drive as Disk 2, cleared (Clear-Disk leaves it MBR with no partitions), its media
loaded. Removed: Notepad++ (both builds), its settings, registry keys and the uninstallers' temp files, the
samples and outputs, C:\SampleBackup, the round's job folders (C:\SAKCapture\r9, C:\SakWork\r9f) and job task, and
the claim lines. The capture VM is back at its pre-round8 snapshot, powered off, its clone disks detached and
deleted; ~/sak-capture-r9 stays on the Mac for its owner to remove. On testpc1 this session removed what the round
had put there, C:\SakIso with it, though the brief kept that for the final capture and the VM smoke; testpc1 was
then left as it was, and the final capture copies the ISO there again.

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
- Found 2026-11-10, before the first signed run: release signing signs the unsigned filesystem tools whose SHA-256
  `tools/filesystem/manifest.json` pins, so a signed package's ext and HFS tools would have been refused by the app
  ("Filesystem tool hash mismatch"), and no T3 step checked packaged tool hashes after signing. `release.yml` now
  builds a check from the application's own manifest validation
  (`.github/release-tools/filesystem_manifest_check`), runs it before signing, requires it to refuse the signed tools,
  re-pins the package's manifest to the signed bytes (`.github/release-tools/repin_signed_filesystem_tools.ps1`), runs it
  again, and publishes the upstream-to-signed mapping. Whether to sign these tools at all is on the "For after
  0.9.3" list. Swept the same day: of the 22 unsigned .exe and .dll files the release signs (the four `sak_*`
  binaries, zlib, bzip2 and liblzma, and 15 bundled third-party files), only the six under `tools/filesystem` are
  pinned at runtime. The application finds the elevated helper by its image path, and runs 7-Zip, aria2c, the UUP
  converter, iperf3 and smartctl after a path-containment check only; its one signer check is on choco.exe, which
  Chocolatey signed, so the signing catalog leaves it alone. Build-time pins (`bundle_uup_tools.ps1`,
  `bundle_iperf3.ps1`, `bundle_smartmontools.ps1`) are checked before signing.
- Found 2026-11-10 by the first `release.yml` dry run: its readiness gate refused the certification evidence ("the
  evidence is for other code") because "Bundle smartmontools" rewrote the tracked `tools/smartmontools/README.txt`
  in the checkout, and the certification digest covers it. The release workflow now bundles smartmontools and iPerf3
  into the runner's temp folder and requires the tracked binaries to match those pinned copies byte for byte, checks
  that no step has changed a tracked file (after bundling and before readiness). The script's own fault is fixed in
  "Fix round N" above.
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
  HFS+ on the Best Buy NVMe enclosure, disk 3 partition 3, passed the same
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
- All packaged `.exe` and `.dll` files have valid Authenticode signatures, and the first-party
  `sak_*` binaries are signed by exactly `CN=Randy Northrup, O=Randy Northrup`, the publisher
  sakutility.com names. Bundled third-party tools keep their vendor's signature.
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
Partition Manager certification runs on the rig, and its evidence is committed. `docs/PARTITION_MANAGER_CERTIFICATION.md` has the procedure, and readiness checks what was committed.

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
is 0.9.3 and the recipe below produces `SAK-Utility-v0.9.3`; this proof
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
of scope, encryption credential-gated, sealed-system and case-sensitive volumes refused,
and APFS container shrink + chunk-adding grow as documented follow-ons. (Corrected
2026-11-01: this said sealed volumes had a typed override and that Apply was limited to
generated layouts. Neither is in the code.)

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
  -ExpectedSerialNumber <serial> `
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
