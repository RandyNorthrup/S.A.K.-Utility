# CODEX REVIEW 4 -- Remediation Tracking

Fourth whole-codebase independent review (Codex gpt-5.6-sol, xhigh), run
2026-08-03 over commit f3979fe. Seven subsystem passes (pst/email,
profile-restore+network, win32mcp, apfs/partition, ai, core-util,
gui/actions/elevated) produced ~259 raw findings. Each pass was verified
against the local tree by an independent Claude subagent that classified every
finding CONFIRMED / FALSE-POSITIVE / ALREADY-GUARDED / DESIGN-INTENT, applying
this repo's rule that the fail-closed requirement targets security/destructive
operations (a best-effort recovery reader that surfaces its incompleteness is
not a fail-open defect).

Result of verification: the confirmed set below. Codex severities were
adjusted where the verifier proved the exploit path weaker or stronger than
claimed (e.g. two "CRITICAL" netsh/%SystemRoot% items dropped to LOW; the
apfs "mutates after read failure" cluster was already fail-closed).

Status legend: [ ] open  [x] fixed+gated  [~] deferred (reason noted).

Full per-finding evidence lives in the review scratchpad (verified-A1..B3.md).

## Summary

- CRITICAL: 4
- HIGH: 18
- MEDIUM: ~40
- LOW: ~55 (robustness / defense-in-depth / dead code)

===============================================================================
## CRITICAL
===============================================================================

- [x] C1 [A4-1] partition_safety_validator.cpp:1825 / partition_script_builder.cpp:3779,4860
  FIXED (wave 1): operationTargetScopeMismatch() rejects a disk-scoped op with a
  non-Disk target (and vice-versa) fail-closed before dispatch. Regression test
  safetyValidator_blocksDiskScopedWipeWithPartitionTarget.
  Whole-disk wipe bypass. validate() dispatches guards by target.kind; the
  partition path never runs validateDiskStateBlockers, and WipeDisk is absent
  from isDestructivePartitionOperation (65-123) so blocksProtectedPartition is
  false. buildScript keys on operation.type and emits
  `Clear-Disk -Number <target.disk_number> -RemoveData -RemoveOEM`.
  COMBO: operation.type=WipeDisk + target.kind=Partition|Volume +
  target.disk_number=OS disk => whole OS disk wiped with no guard.
  Fix: in validate(), cross-check operation vs target.kind -- reject disk-scoped
  operations (WipeDisk/InitializeDisk/CloneDisk/ConvertPartitionStyle/...) whose
  target.kind != Disk, and partition-scoped ops whose target.kind == Disk,
  before dispatch.

- [x] C2 [B1-1] ai_tool_policy.cpp:322,337,576
  FIXED (wave 1): commandIsReadOnlyDiagnostic now recurses into every top-level
  (...) sub-expression and requires it read-only too (topLevelParenGroups,
  depth-bounded). Regression rows nested-mutator-*, nested-read-count.
  ReadOnlyPc allowlist ignores nested commands.
  `Write-Output (Restart-Computer -Force)` / `Write-Output (Stop-Process ...)`:
  segmentLeadIsReadOnly accepts the write-output lead; commandHasUnsafeConstruct
  flags `$(` but not a plain `(...)` group; the risky regex lacks
  stop-process/restart-computer. Passes read-only with risky_change=false =>
  no lease, no restore point, no confirm. Directly model-reachable in Unattended.
  Fix: in the read-only path reject `(` grouping / `&` call / pipeline-into-arg;
  add stop-process/restart-computer/stop-computer to the risk regex.

- [x] C3 [B1-2] ai_tool_policy.cpp:419,576
  FIXED (wave 1): risk regex now also flags .NET static-member calls (]::),
  method invocation (.\w+(), Remove-Item aliases (ri/rm at command position), and
  stop-process/restart-computer/stop-computer/restart-service/kill. Regression
  rows added to readOnlyPolicyBlocksNativeMutators.
  Full-access risk classification is a leaky blacklist. `ri` (Remove-Item
  alias), `[IO.File]::WriteAllText(...)`, and arbitrary run_process executables
  are not in commandLooksRiskyChange => risky_change=false => no
  lease/restore/confirm.
  Fix: allowlist known-safe instead of blacklisting known-bad; treat `::`,
  `.Method(`, `&`-call, aliases, and any unrecognized run_process program as
  risky-by-default.

- [x] C4 [B1-4] ai_tool_policy.cpp:551,560 / ai_assistant_panel.cpp:5413
  FIXED (wave 1): policyContext now marks any obfuscated/indirected shell command
  catastrophic (context.catastrophic |= commandLooksObfuscated), forcing the hard
  human confirm instead of a restore-point downgrade. Regression test
  obfuscatedShellCommandsForceCatastrophic.
  Obfuscated catastrophic downgrade.
  `& ('For'+'mat-Volume') -DriveLetter D -Force`: indirection makes
  commandLooksObfuscated true (=> risky) but commandLooksCatastrophic needs a
  contiguous `\bformat-volume\b`, so catastrophic_change stays false; the panel
  gates the mandatory human confirm on catastrophic only => Unattended offers
  merely a restore point for a volume wipe.
  Fix: when obfuscation/indirection is present on a shell command, force
  catastrophic_change (fail closed).

===============================================================================
## HIGH
===============================================================================

- [x] H1 [A1-1] pst_parser.cpp:305 -- REGRESSION (wave F, this author). FIXED (wave 1):
  msPstComputeSig now folds only the low 32 bits per MS-PST 5.4
  (WORD(value>>16) ^ WORD(value)); the erroneous >>32 fold is gone.
  msPstComputeSig adds `value ^= (value >> 32)`; MS-PST ComputeSig folds only
  the low 32 bits (WORD(ib>>16) ^ WORD(ib)). For a byte offset ib > 4 GiB the
  computed signature diverges from spec, so every block/page above the 4 GiB
  mark false-FAILS verifyPageTrailer/verifyBlockTrailer -- silently breaking
  recovery of exactly the large OST/PST files this tool targets. The wave-F
  fixtures were all sub-4 GiB, so it was not caught.
  Fix: delete the `value ^= (value >> 32)` line.

- [x] H2 [A1-16] email_profile_manager.cpp:167,712 FIXED (wave 2): extensionless
  restore destinations now reject any dot-directory/dotfile segment (~/.ssh etc.).
  isRestorableDataFile returns true for an EMPTY suffix, so a crafted backup
  manifest restores attacker content to new extensionless files / dotfiles
  under $HOME (.ssh/authorized_keys, .bashrc) => persistence / code-exec.
  Fix: for extensionless entries require the destination under a known
  mail-store subtree; reject dot-directories / dotfiles.

- [x] H3 [A2-3] user_profile_restore_worker.cpp:727 / user_profile_restore_wizard_pages.cpp:803,1081,1292
  FIXED (wave 7): the manifest already serializes+checksums wifi_profiles/ethernet_configs/
  app_data_sources but the backup never populated them and restore read the unprotected
  *.json sidecars. Backup now mirrors the SELECTED entries into the manifest
  (recordNetworkSelectionsInManifest, before the worker seals the checksum); restore reads
  ONLY from the checksum-verified manifest and disables WiFi/Ethernet/AppData restore when
  the manifest has no valid non-empty checksum (fail closed). Sidecars are still written for
  inspection but ignored by restore. Regression test manifestChecksumCoversNetworkSelections.
  WiFi/Ethernet/AppData sidecars (wifi_profiles.json, ethernet_configs.json,
  app_data_sources.json) are loaded raw and never covered by the manifest /
  payload checksum set, then applied elevated via netsh
  (`wlan add profile user=all`, adapter/DNS reconfig).
  Fix: record each sidecar's SHA-256 in the checksum-verified manifest and
  verify before load/apply; refuse network apply from any backup lacking
  sidecar integrity.

- [x] H4 [A3-1] browser/extension/background.js:504,2353 FIXED (wave 6): armed HTTP-auth
  credentials are now bound to the arming tab's origin ({username,password,origin}); the
  Fetch.authRequired handler provides them only when params.authChallenge.origin matches
  (originsMatch), and they are cleared on top-frame navigation. Arming fails closed if the
  tab origin cannot be resolved. (JS source; the packaged .crx must be rebuilt to deploy.)
  HTTP-auth credentials are armed with `urlPattern:"*"` and cleared only on
  detach (not on navigation), so armed Basic creds answer any later 401 --
  including cross-origin subresources of a controlled tab (credential leak).
  Fix: bind creds to the arming origin (only ProvideCredentials when the auth
  challenge origin matches) and clear httpAuthCreds in the topFrameNav branch.

- [x] H5 [A4-5] partition_apfs_file_system_reader.cpp:961 / partition_apfs_writer.cpp:12034
  FIXED (wave 4): PartitionApfsFileReadResult gained a `truncated` flag set whenever the
  read hits the byte cap (regular + compressed paths); buildFilePatchRequest now fails
  closed when read.truncated (a read-modify-write must not write a prefix back).
  APFS patch truncates large files: the reader caps at kApfsMaximumSeedFileBytes
  and only WARNS (read.ok stays true), so the patch writer replaces the whole
  file with the truncated buffer, dropping the tail.
  Fix: treat read truncation on the patch path as a blocker (fail closed).

- [~] H6 [B1-3] ai_execution_broker.cpp:194,482 DEFERRED (wave 3): a correct fix requires
  deriving effective elevation from the process token and either routing an
  already-elevated plain launch through the gated elevated path or de-elevating it -- a
  privilege-architecture change too risky to retrofit inline without an elevated-path
  test harness. Concrete exploit is now bounded by the strengthened content classifier
  (C2/C3/C4): a destructive command is gated by content regardless of requires_admin.
  Tracked for a dedicated design pass.
  requires_admin is model-controlled and defaults false on malformed input; the
  non-admin path launches via plain QProcess inheriting S.A.K.'s token, so when
  S.A.K. is elevated a model can set requires_admin=false and still run elevated,
  skipping the elevated-runner gate, and drops the auto-risky flag.

- [x] H7 [B1-8] ai_tool_policy.cpp:194 FIXED (wave 3): the action verb is matched as a
  whole word (textContainsAnyWord), so "list installed apps" no longer satisfies install
  intent via the substring. Regression row install-substring-in-installed.
  Install-intent inferred from a question: "Can you list installed apps?" ->
  substring "installed" + directed-request marker "can you " => hasDirectedIntentFor
  true, authorizing an install mutation from an unrelated question.
  Fix: match verbs on word boundaries and require the verb to be the directed
  (imperative) action, not merely co-occurring with a request marker.

- [x] H8 [B2-2] windows_usb_creator_extract.cpp:50 FIXED (wave 2): separator-boundary
  containment (== dir OR startsWith dir+'/'). Test isSafeBundledExecutable_rejectsSiblingPrefixDir.
  isSafeBundledExecutable containment uses boundary-free startsWith; a canonical
  sibling like C:/AppEvil passes C:/App => elevated arbitrary-code execution.
  Fix: require canonical == dir OR startsWith(dir + separator).

- [x] H9 [B2-3] uup_iso_builder.cpp:88 FIXED (wave 2): isTrustedBundledExe now takes the
  bundled-tools root and confines the canonical path under it (junctioned ancestor resolved).
  isTrustedBundledExe validates only the leaf (exists/isFile/!symlink/!junction),
  no canonicalization or ancestor confinement => elevated converter can run an
  attacker-planted binary via a junctioned ancestor.
  Fix: canonicalize and confine under the BundledTools root; reject ancestor
  reparse points.

- [x] H10 [B2-6] leftover_scanner.cpp:673,803 FIXED (wave 2): schtasks/netsh resolved via
  system32Exe() (GetWindowsDirectoryW), fail-closed when the Windows root is untrusted.
  Bare `schtasks.exe` / `netsh.exe` (process_runner CreateProcess searches
  app-dir/CWD before System32) => executable hijack, potentially elevated.
  Fix: System32-qualify (the repo already does this for bcdboot and
  profile-restore netsh).

- [x] H11 [B2-5] uup_iso_builder.cpp:396 FIXED (wave 2): writeAria2Entry returns bool and
  rejects newline in url/sha1 and traversal/absolute/drive-qualified out=; caller aborts.
  writeAria2Entry writes uupdump-API-sourced fileName/url/checksum raw into the
  aria2 control file with no newline/traversal check => directive injection and
  `out=` path escape (arbitrary file overwrite).
  Fix: reject newline in url/sha1; reject `..` / absolute / drive-qualified
  fileName.

- [x] H12 [B2-7] leftover_scanner.cpp:1076 FIXED (wave 2): install-location Safe classification
  requires exact match or a '\'-boundary prefix, not a bare startsWith.
  classifyFileRisk install-location startsWith lacks a separator boundary => an
  adjacent-prefix sibling path is classified Safe and exempted from
  protected-root checks (eligible for automatic recursive deletion).
  Fix: reuse the correct boundary helper already present at :1129.

- [x] H13 [B3-13] verify_system_files_action.cpp:159 FIXED (wave 2): DISM success now requires
  completedSuccessfully() (exit 0, not timed out/cancelled) AND the phrase "completed
  successfully" (excludes "did not complete successfully").
  DISM false success: `std_out.contains("successfully")` sets m_dism_successful,
  ignoring exit code and timeout; the failure string "did not complete
  successfully" matches.
  Fix: gate on `!timed_out && exit_code==0` plus a specific success phrase.

- [x] H14 [B3-2] reset_network_action.cpp:154 / process_runner.cpp:217 / verify_system_files_action.cpp:65,104
  FIXED (wave 5): added sak::system32Path() (GetSystemDirectoryW, fail-closed);
  runPowerShell launches the System32-qualified interpreter (choke point for every PS
  caller); netsh/ipconfig/nbtstat/powercfg qualified at their call sites; sfc/DISM in the
  PS scripts resolved via [System.Environment]::GetFolderPath('System'). Regression test
  system32Path_qualifiesUnderSystem32.
  Elevated PATH hijack: system exes launched by bare name
  (netsh/ipconfig/nbtstat/powercfg, powershell.exe, `Start-Process 'sfc'`,
  DISM.exe) from the elevated helper.
  Fix: resolve every system exe to its GetSystemDirectoryW absolute path (the
  helper already has resolveSystemPowerShellPath for its own tasks).

- [x] H15 [A4-16] partition_script_builder.cpp:4545 FIXED (wave 3): the partition
  enumeration now runs only in the non-RAW branch with -ErrorAction Stop, so an
  enumeration failure aborts instead of being swallowed into an empty "empty disk"
  result that would Clear-Disk. RAW->GPT conversion still works (RAW has no partitions).
  ConvertPartitionStyle enumeration fail-open: `Get-Partition -DiskNumber %1
  -ErrorAction SilentlyContinue` yields empty on error, the "requires empty
  disk" gate passes, and Clear-Disk proceeds.
  Fix: -ErrorAction Stop before the empty-disk gate.

- [x] H16 [A4-15] partition_script_builder.cpp:4592 FIXED (wave 3): robocopy exit is now
  gated at >= 4 (any mismatch/failure aborts) before Remove-Partition, instead of > 7.
  Source GUID/size pinning beyond number+adjacency remains a follow-on hardening.
  Merge deletes source after an incomplete copy: robocopy exit 4-7 (mismatch)
  is accepted, then Remove-Partition deletes the source, which is pinned only by
  number + adjacency.
  Fix: accept only robocopy 0-3 (or verify a manifest); pin source by
  GUID/size/offset.

===============================================================================
## MEDIUM
===============================================================================

profile-restore / network (A2):
- [ ] M-A2-6  worker.cpp:552 verify/perms run AFTER swap-in; failure does not restore original.
- [ ] M-A2-4  worker.cpp:756,860 payload verify TOCTOU + compares dest vs re-read source, not manifest digest.
- [x] M-A2-13 worker.cpp:694 FIXED (wave 8): assignOwnershipToUser fails closed on SID-resolve/takeOwnership failure instead of silently stripping + reporting success.
- [x] M-A2-14 worker.cpp:470 FIXED (wave 8): copyDirectory fails closed on an unreadable source dir; QDir::System added so system entries aren't silently skipped.
- [x] M-A2-11 worker.cpp:276 FIXED (wave 8): success = (m_filesErrored==0) && !m_cancelled.
- [x] M-A2-15 execute.cpp:134 FIXED (wave 8): onStartRestore returns early if m_worker is already set (no second concurrent worker).
- [ ] M-A2-16 wizard.cpp:59,146 backupPath* only checks non-empty; invalid edit never clears stale backupPath/manifest.
- [ ] M-A2-12 worker.cpp:734,753 empty manifest_checksum/checksum_sha256 logged and accepted.
- [ ] M-A2-2  worker.cpp:110,479 leaf-only reparse check; ancestor dirs not re-validated after mkpath; check-then-copy race.
- [ ] M-A2-5  worker.cpp:156 rollback QFile::rename return ignored; predictable temp names.

win32mcp (A3):
- [x] M-A3-16 native_messaging.cpp:11 FIXED (wave 9): kMaxHostToBrowserBytes (1 MiB); writeStdoutFrame refuses an oversized Chrome-facing frame (returns false -> pipe torn down) instead of letting Chrome kill the host.
- [x] M-A3-41 win32_mcp_dialog_choice.cpp:45 FIXED (wave 9): negative words (cancel/no/abort/deny/decline/stop/never) added to the disqualifier list so "Yes, cancel" is never auto-affirmed. Test negativeCaptionRejectedDespiteAffirmativeWord.
- [ ] M-A3-65 win32_mcp_dispatch.cpp:135 advertised inputSchema not enforced server-side (defense-in-depth).
- [ ] M-A3-3  background.js:790 lastSnapshotTabId set before capture completes.

apfs/partition (A4):
- [ ] M-A4-4  partition_apfs_writer.cpp:8852 unvalidated free-queue {paddr,length} -> OOM / wrap.
- [ ] M-A4-6  partition_apfs_writer.cpp:17198 non-atomic APFS replace (delete then insert as two checkpoints).
- [ ] M-A4-8  partition_safety_validator.cpp:1390 OS-disk data partitions lack the disk-level backstop.
- [ ] M-A4-11 partition_script_builder.cpp:2074 CreateImage overwrites an existing file without confirm.
- [ ] M-A4-14 partition_script_builder.cpp:4563 merge target_folder path traversal (`..`).
- [ ] M-A4-27 partition_apfs_file_system_reader.cpp:359 / partition_ext_file_system_reader.cpp:1060 export junction/symlink TOCTOU.
- [ ] M-A4-28 partition_safety_validator.cpp:1825 validator target.kind switch has no default -> enum fail-open.
- [ ] M-A4-31 partition_apfs_writer.cpp:18402 incompatible-feature mask value_or(0) -> missing metadata passes preflight.

ai (B1):
- [ ] M-B1-5  ai_app_action_planner.cpp:116 provider-gateway app_run_action has no catastrophic path.
- [ ] M-B1-6  ai_provider_gateway_tool_runner.cpp:397 recipe input-tier steps run without the per-call hard confirm.
- [ ] M-B1-13 ai_mcp_session_pool.cpp:33 timeout_ms in key + no eviction -> unbounded server processes.
- [ ] M-B1-14 ai_mcp_stdio_client.cpp:165 reads a full line before the byte cap.
- [ ] M-B1-15 ai_mcp_stdio_client.cpp:128 server-exit uses terminate() not tree-kill.
- [ ] M-B1-16 ai_execution_broker.cpp:90 job-create/assign failure -> kill primary only, descendants survive.
- [ ] M-B1-17 ai_provider_registry.cpp:72 exe confinement lexical cleanPath, not canonicalFilePath; validate/launch TOCTOU.
- [ ] M-B1-21 ai_workflow_placeholders.cpp:81 PowerShell single-quote invariant never validated; user workflows override built-in ids.
- [x] M-B1-22 ai_assistant_panel.cpp:5453 FIXED (wave 12): a mutation requiring a lease now fails closed (emits leaseDeniedResult, blocks) when m_leaseManager is absent, instead of returning true.
- [x] M-B1-28 ai_credential_store.cpp:357 FIXED (wave 12): the assignment-secret regex gained an optional "? after the key name so quoted-JSON secrets ("password":"...") are redacted.

core-util (B2):
- [x] M-B2-11 offline_deployment_worker.cpp:1046 FIXED (wave 11): resolveChocoExecutable now requires choco.exe be a real regular file (not symlink/junction) canonically confined under the bundled-tools root, mirroring the H9 pattern.
- [ ] M-B2-1  uup_iso_builder.cpp:268,1266 workspace create/removeRecursively no reparse/ownership check.
- [ ] M-B2-10 offline_deployment_worker.cpp:244,746 marker-write failure returns true; no reparse/identity before removeRecursively.
- [ ] M-B2-13 offline_deployment_worker.cpp:359 header says unmet-dep=fatal but code warns+proceeds; zero-package bundle emits operationCompleted.
- [ ] M-B2-15 offline_deployment_worker.cpp:1580 empty declared checksum passes unverified; downloaded>0 ships partial.
- [ ] M-B2-16 vulnerability_scanner.cpp:1525 enumerateInstalledProgramsFast no incomplete flag -> denied hive = "complete" inventory.
- [x] M-B2-35 package_internalization_engine.cpp:371 FIXED (wave 11): isSafePackageComponent now rejects Win32-illegal chars (< > " | ? *), trailing dot/space, and reserved device names (CON/PRN/AUX/NUL/COM1-9/LPT1-9). Regression rows added.
- [ ] M-B2-31 windows_usb_creator_extract.cpp:618 bcdboot given drive root not a Windows dir (tracked in-code).
- [x] M-B2-28 disk_benchmark_worker.cpp:600,765 FIXED (wave 10): validateTestFileSize now rejects sequential_passes<=0/>1000 (loop-skip 0 MB/s success) and random_duration_sec<=0/>3600 (bounds the *1000 int overflow).
- [x] M-B2-29 disk_benchmark_worker.cpp:183 FIXED (wave 10): randomIoResultUsable requires total_failures < total_ops (was <=), so a 50%-failure run is not scored as usable.

gui/actions/elevated (B3):
- [x] M-B3-10 network_diagnostic_panel.cpp:4658 FIXED (wave 13): onResetNetworkClicked now confirms (showQuestionLogged, default No) before the destructive reset.
- [x] M-B3-11 diagnostic_benchmark_panel.cpp:1078 FIXED (wave 13): onQuickActionClicked confirms for the repairing "Verify System Files" action (SFC/DISM) before running.
- [ ] M-B3-12 reset_network_action.cpp:224 reboot-required set even when Winsock failed; cancel leaves partial mutation unrolled.
- [ ] M-B3-19 backup_bitlocker_keys_action.cpp:671 cancel ignores removeRecursively() -> plaintext keys may remain, reported clean.
- [ ] M-B3-20 backup_bitlocker_keys_action.cpp:323 detection returns {} on parse/query error -> "no BitLocker" == failure.
- [ ] M-B3-22 optimize_power_settings_action.cpp:311 failed powercfg discovery -> hard-coded GUID fallback then mutates.
- [ ] M-B3-5  elevated_helper_main.cpp:845 client-loss treated as "no message" -> dead client never cancels the privileged task.
- [ ] M-B3-7  organizer_panel.cpp:199 QThread wait(15s) then unique_ptr delete of a possibly-running thread -> abort.
- [ ] M-B3-8  user_profile_backup/restore_wizard_execute.cpp no page dtor joins the QThread-subclass worker -> closing wizard mid-op aborts.
- [ ] M-B3-9  screenshot_settings_action.cpp:44 grabWindow/QPixmap on a worker thread (GUI-thread affinity).
- [ ] M-B3-1  partition_script_builder.cpp:2297 Restore Image approval pins only size not content; same-size swap accepted; UNC bypasses same-disk check.

pst/email (A1):
- [ ] M-A1-26 user_data_manager.cpp:965 atomicReplaceFile deletes target then renames; rename failure destroys the original.
- [ ] M-A1-20 user_data_manager.cpp:281,307,404 public path validation via Q_ASSERT_X (release no-op) -> empty paths run CWD-relative.
- [ ] M-A1-29 email_constants.h:215 kAnsiVersion=14 only; valid ANSI wVer 15 PSTs rejected.
- [ ] M-A1-25 user_data_manager.cpp:783 decryptArchiveToTempFile readAll() of an attacker file with no size cap before the zip-bomb preflight -> OOM.

===============================================================================
## LOW (robustness / defense-in-depth / dead code)
===============================================================================

A1:  #10 attachment drop no log; #27 deleteBackup removes sidecar on payload-delete failure; #34 non-atomic metadata/manifest write; #36 decrypted-temp remove ignored; #41 getBackupInfo hides read failure; #40 errorOccurred never emitted; #38 wrong magic comment; #39 failOpen misnomer.
A2:  #1 netsh via %SystemRoot% (use GetSystemDirectoryW); #23 missing ethernetRestoreComplete(false); #24 dead code (page startRestore + 5 on* slots + folderConfig); #8 dhcp_enabled default true; #17 appdata separator normalization + equal-length dup; #21 sidecar readAll no size cap.
A3:  #10 focus recheck; #11 drag requireSnapshotTab; #12 press/release try/finally (stuck CDP key); #28 click_text virtual-screen bound; #20 Network.enable swallow -> network_idle false-succeed; #21 bodyText "" -> absent-wait false-satisfy; #31 present-but-failed Invoke falls through; #56 PDF fromBase64 without AbortOnBase64; #58 forcelist uninstall TOCTOU; #63 EnumWindows/GetWindowRect returns unchecked; plus #8/#13/#15/#18/#19/#23/#24/#32/#33/#52/#53 defense-in-depth; #66/#67/#68 dead/duplicated code.
A4:  #3 absent-oid no blocker; #9 usedBytes 0 on corrupt metadata; #17 recreate size overflow; #18 builder payloadBool strictness; #19 recreate-FS divergence; #21 blockCount_==0 bound; #24/#25 EXT reader bounds; #30 detector supplemental errors; #32 remaining unchecked adds; #33 EXT read ceiling; #34 inconsistent-encryption accepted.
B1:  #11 wall-clock lease expiry; #12 isJsonRpcResponse no id/jsonrpc check; #18 malformed function_call dropped; #20 subagent batch no up-front validate; #23 readAll unbounded (trusted HTTPS); #24 finalizeResult clobbers success:false; #25 isError string->false; #27 cred size TOCTOU; #29 broker coercions; #30 timeout signed-overflow; #31 cli method run via powershell; #32 setPermissions no owner DACL; #33 cred version unchecked; #34 dispatcher release non-RAII/no owner check; #35 provider registry phantom-available; #36 dup tree-kill helper.
B2:  #20 uup rollback rename return ignored; #21 hashless post-download not SHA-1'd; #22 benchmark file-id pinning; #25/#26 advanced_search incomplete flag not routed to filesUnreadable(); #28/#29/#30 disk_benchmark validation/overflow/flush; #37 safeInstallerFilename reserved-name; #40 wrong-typed bools coerced; #43 scanStandardDirs no error channel; #46 empty-updateId asserts; #32 extract 7z last-entry/critical-name/join.
B3:  #17 PS progress chunk >4 MiB pipe break; #4 elevated job-containment failure not surfaced; #14 payloadUInt64 UB cast; #23 volume-usage divide-by-zero; #24 read-only disks excluded from scan; #25/#26 dead code (captureScreen decl, restore-page slots, m_resumedApprovedToolCallIds); #3 qputenv return + winsock reopen reparse residual.

===============================================================================
## Not defects (verified FALSE-POSITIVE / ALREADY-GUARDED / DESIGN-INTENT)
===============================================================================

Recorded so they are not re-flagged:
- A1: #2,#4,#5,#12,#14,#31,#33 false-pos; #3,#9,#17,#22,#30,#32,#37 guarded; best-effort recovery #6,#7,#8,#11,#13,#15,#18,#19,#21,#23,#24,#28,#35.
- A2: #10 (DHCP coercion gated on IP Helper verify -- correct), #22 (overflow only in dead slots); design-intent #7,#9,#19,#20.
- A3: #2,#4,#5,#7,#17,#26,#30,#36,#38,#39,#47,#54 false-pos/guarded; ~26 design-intent (surfaced/best-effort/inert), incl. #50 not compiled into the shipping binary.
- A4: #2,#3-main (advanceCheckpoint fails closed on any blocker), #7,#10,#12,#13,#20,#22,#23,#26,#29.
- B1: #7 (operation always set), #9,#10 (fails closed in effect), #19 (batch validated before active), #26 (skip = tool unoffered).
- B2: #4 (UAF guarded), #9 (.NET repack terminating), #41 (severity labeled not understated); many design-intent surfaced-partial incl. #17/#18 (app_readonly 676/707 provenance is a shrink-only whitelist -- a timed-out scan cannot authorize deleting un-scanned items).
- B3: #16 (single serial task) false-pos; #6 (AI UAF -- token + m_shuttingDown + QPointer, prior fix), #18 (File Explorer detach -- deliberate prior fix), #15 (trusted pipe), #21 (report success gated on collectors+save), #3 (mostly mitigated).
