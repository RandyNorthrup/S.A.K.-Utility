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

INCOMPLETE, VM smoke:

- Time a real flash at the 64 MB buffer against 256 MB, on one real image and one real drive. If 64 MB
  is measurably slower, raise it. Round 3 settled on 64 MB everywhere without a timing.
- Flush Cache unelevated. `DnsFlushResolverCache` was not run here, because it changes this PC's DNS
  state. Microsoft's pages for `ipconfig /flushdns` and `Clear-DnsClientCache` state no administrator
  requirement, and do not say there is none, so this stays unproven. If it needs an administrator after
  all, the button says "Failed to flush the DNS resolver cache." The action registry and the tooltip say
  Standard. `include/sak/elevation_tier.h`, which said Elevated, was read only by tests and is gone.
- Diagnose on a Windows with no msdt.exe: the plan is tested, the launch of Settings is not. This PC
  has msdt.exe and it was not launched.
- Restore Settings on a non-English Windows, end to end. The script's output is tested on this PC
  (English) and the JSON reader on other languages' names.
- Reset Network, Backup Settings' restore, and every other network apply: command building and result
  handling only, by standing rule.
- SFC and Check Disk results on real runs: fixed output only. Nothing here ran `sfc`, `dism /online` or
  `Repair-Volume`.
- A real mapped drive through Check Disk, elevated. Windows may hide it from an elevated process, which
  is why the report says network drives are not checked whether or not it names one.
- The assistant's Unattended prompt and the workflow notice through a real model run. No paid API was
  called: the tests stop at the run or use the approval hook.

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
testpc1's USB DISK 3.0 stick (pinned by model, serial 067D19C65080 and size 31042043904): install2.swm, 2,839,447,229
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
model, serial NAAA3QVK and size 8001563221504): MSR at 17408, E: 3 GB at 16 MiB, F: 2 GB at 3238002688, the VM's
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
Proved on testpc1 with Rig D, the disposable USB DISK 3.0 stick (pinned by serial 067D19C65080 and size 31042043904)
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
