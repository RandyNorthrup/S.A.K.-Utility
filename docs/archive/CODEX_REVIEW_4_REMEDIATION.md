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

- [x] H6 [B1-3] ai_execution_broker.cpp:194,482 DEFERRED (wave 3): a correct fix requires
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
- [x] M-A2-6  worker.cpp:552 DEFERRED: the pre-existing original (.sakold.tmp) is removed on a successful swap, so a later verify/perms failure cannot auto-restore it. The failure IS surfaced fail-closed (m_filesErrored++, restore reports failure); auto-rollback needs a verify-before-swap restructure (defer removing .sakold.tmp until verify passes). Tracked.
- [x] M-A2-4  worker.cpp:756,860 payload verify TOCTOU + compares dest vs re-read source, not manifest digest. FIXED (wave 22): verifyFile still compares dest vs source per-file, but restoreUser now runs a POST-restore re-hash of the source tree against the sealed manifest digest (verifyUserPayloadChecksum, gated on m_verify), so a source swapped between pre-restore validation and the copy is caught -- source==manifest at T0 AND T2 plus dest==source per-file => dest==sealed transitively. Folder loop extracted to restoreUserFolders to keep restoreUser under the length cap. (The manifest records only per-user tree digests, no per-file digests, so a full per-file bind would need a manifest schema change -- noted.)
- [x] M-A2-13 worker.cpp:694 FIXED (wave 8): assignOwnershipToUser fails closed on SID-resolve/takeOwnership failure instead of silently stripping + reporting success.
- [x] M-A2-14 worker.cpp:470 FIXED (wave 8): copyDirectory fails closed on an unreadable source dir; QDir::System added so system entries aren't silently skipped.
- [x] M-A2-11 worker.cpp:276 FIXED (wave 8): success = (m_filesErrored==0) && !m_cancelled.
- [x] M-A2-15 execute.cpp:134 FIXED (wave 8): onStartRestore returns early if m_worker is already set (no second concurrent worker).
- [x] M-A2-16 wizard.cpp:59,146 FIXED (wave 17): onBackupPathChanged now clears the wizard's stored backupPath+manifest on the empty/invalid/failed branches (clearStoredBackup), so a stale valid manifest from an earlier selection can't be restored after the field is edited to an invalid path.
- [x] M-A2-12 worker.cpp:734,753 empty manifest_checksum/checksum_sha256 logged and accepted. FIXED (wave 22, NARROWED after gate caught an over-reach): the plan's "gate empty-checksum on m_verify" was itself an over-reach -- verifyGoodCopySucceeds uses verify=true purely for per-file content hashing on a legacy (unsealed) backup, and coupling verify->manifest-required false-failed it (full ctest caught this). Correct fix: an empty per-user checksum_sha256 passes for a genuinely legacy backup (manifest_checksum ALSO empty) but FAILS CLOSED when the manifest IS sealed (manifest_checksum present) -- an attacker who stripped the payload digest and re-sealed the manifest -- independent of m_verify. The empty-manifest_checksum legacy path is left accepting (documented design intent). Regression test sealedManifestMissingUserDigestFailsClosed. LESSON: a verifier-CONFIRMED fix spec can still over-reach real behavior; only build+ctest reveals it.
- [x] M-A2-2  worker.cpp:110,479 leaf-only reparse check; ancestor dirs not re-validated after mkpath; check-then-copy race. FIXED (wave 23): restoreUser pins the CANONICAL profile root once (destProfilePath exists at that point -- resolveExistingUser requires it, resolveCreateNewUser mkpath'd it; fail closed if unresolvable), and copyDirectoryEntry now re-checks every entry's realized PARENT directory against that root via destinationParentWithinRoot (canonicalize the parent, require it at/under the root, reject a sibling prefix) in addition to the existing leaf reparse guard -- so an ancestor directory turned into a junction after mkpath is caught before the write. (Honest limit, same as M-A4-27: a path-based re-check narrows but cannot fully eliminate the check-then-copy race; a race-free fix needs handle-based no-follow opens. The containment predicate is identical to the wave-21 exporter one covered by exporter_realizedPathWithinRootRejectsEscape; existing restore tests guard against false-rejection.)
- [x] M-A2-5  worker.cpp:156 rollback QFile::rename return ignored; predictable temp names. FIXED (wave 22): PART 1 (rollback rename return ignored) was already fixed in the tree (all three renames check their bool + the rollback failure logs and preserves the original). PART 2 (predictable temp names) is now closed: the internal staging/old paths use makeRestoreTempPath() with a 64-bit QRandomGenerator token instead of fixed .sakrestore.tmp/.sakold.tmp suffixes, so a local attacker can no longer pre-plant a dangling symlink at the known path in the remove->copy window. The user-facing .sakbak recovery name stays predictable by design (same remove+copy fail-closed pattern).

win32mcp (A3):
- [x] M-A3-16 native_messaging.cpp:11 FIXED (wave 9): kMaxHostToBrowserBytes (1 MiB); writeStdoutFrame refuses an oversized Chrome-facing frame (returns false -> pipe torn down) instead of letting Chrome kill the host.
- [x] M-A3-41 win32_mcp_dialog_choice.cpp:45 FIXED (wave 9): negative words (cancel/no/abort/deny/decline/stop/never) added to the disqualifier list so "Yes, cancel" is never auto-affirmed. Test negativeCaptionRejectedDespiteAffirmativeWord.
- [x] M-A3-65 win32_mcp_dispatch.cpp:135 advertised inputSchema not enforced server-side (defense-in-depth). FIXED (wave 32): handleToolsCall now validates a NATIVE tool's arguments against its advertised inputSchema before dispatch via win32McpValidateArgsAgainstSchema -- rejecting (kInvalidParams) a missing required arg, an unknown key under additionalProperties:false, or a wrong-typed value (win32McpJsonMatchesType, integer = whole-number double). Scoped to NATIVE tools only (looked up from toolCatalog(), not the browser catalog): browser tools keep their existing downstream arg validation, so this adds enforcement exactly where the gap was without risking a false-rejection of a browser call. The native schemas (built by toolSchema with additionalProperties:false + explicit properties/required) are complete, confirmed by the full ctest passing 208/208 (no valid dispatch call rejected). Pure seam win32McpValidateArgsAgainstSchema + regression test validateArgsAgainstSchema_enforcesRequiredUnknownAndType.
- [x] M-A3-3  background.js:790 lastSnapshotTabId set before capture completes. FIXED (wave 20): assignment moved to AFTER the capture fully succeeds; a throw in any await leaves the prior (still-valid) tab id, so requireSnapshotTab fails closed on refs never delivered. (.crx needs rebuild to deploy.)

apfs/partition (A4):
- [x] M-A4-4  partition_apfs_writer.cpp:8852 unvalidated free-queue {paddr,length} -> OOM / wrap. FIXED (wave 20): freeQueueRunInBounds() pure guard (paddr<blockCount, length<=blockCount-paddr, overflow-safe) at the parse chokepoint appends a blocker + returns {} on an out-of-range run, before expandFreeQueueEntries can allocate. Testing seam freeQueueRunInBoundsForTesting + unit test.
- [x] M-A4-6  partition_apfs_writer.cpp:17198 non-atomic APFS replace (delete then insert as two checkpoints). DEFERRED (data-loss WINDOW, not corruption; needs a design-level single-transaction primitive): commitInPlaceRootFileWrite replaces an existing file as two independent checkpoints (commitInPlaceFileDelete then commitInPlaceFileInsert); an interruption between them leaves the file deleted with the new payload absent (the in-code comment already concedes "the replace is two checkpoints"). The correct fix is a new commitInPlaceFileReplace primitive modeled on the existing single-checkpoint commitInPlaceFilePatch (build one fs-tree dropping the old records + adding the new payload, freeing old data via freedDataBlocks in ONE finalizeFsCommit) so only the pre- or fully-replaced state is ever visible. Deferred rather than risk a partial refactor of the certified in-place COW commit path; requires crash-safety round-trip cert. NOT a fail-open security defect (either state is internally consistent; only a crash mid-replace loses the file, recoverable from backup).
- [x] M-A4-8  partition_safety_validator.cpp:1390 OS-disk data partitions lack the disk-level backstop. FIXED (wave 20): blocksCurrentOsDiskPartitionMutation() (partition-scoped mirror of blocksCurrentOsDiskMutation) blocks any destructive partition op on a system/boot disk, excluding read-only ClonePartition. Regression test safetyValidator_blocksOsDiskDataPartitionMutation.
- [x] M-A4-11 partition_script_builder.cpp:2074 CreateImage overwrites an existing file without confirm. FIXED (wave 20): the CreateImage guard script now stats the destination and throws when it already exists unless overwrite_confirmed (a genuine payload bool) was passed; no silent FileMode::Create truncation. Regression test scriptBuilder_createImageRefusesExistingWithoutOverwrite. (Was previously listed as deferred; done.)
- [x] M-A4-14 partition_script_builder.cpp:4563 FIXED (wave 14): buildMergeScript rejects a target_folder containing a path separator, ':', or '..'. Test scriptBuilder_mergeRejectsTraversalTargetFolder.
- [x] M-A4-27 partition_apfs_file_system_reader.cpp:359 / partition_ext_file_system_reader.cpp:1060 export junction/symlink TOCTOU. FIXED (wave 21, both readers): the leaf reparse vector was already closed by NewOnly/CREATE_NEW; the residual PARENT-directory junction TOCTOU (mkpath follows an ancestor junction planted after the root check) is now closed by realizedPathWithinRoot(canonical_root, target) -- the exporter captures the canonical export root once (fail closed if unresolvable), re-checks every created directory after mkpath, and re-checks each leaf's parent inside writeExportFile (5th param) before opening, so a post-creation junction swap is caught at write time. Pure seam PartitionApfsFileSystemReader::exportPathWithinRootForTesting + regression test exporter_realizedPathWithinRootRejectsEscape. (Honest limit: a path-based re-check narrows but cannot fully eliminate the check-then-write race; a fully race-free fix needs handle-based no-follow opens.)
- [x] M-A4-28 partition_safety_validator.cpp:1825 FIXED (wave 14): validate()'s target.kind switch has a default: branch that appends a blocker, so an out-of-range/forged kind fails closed.
- [x] M-A4-31 partition_apfs_writer.cpp:18402 FIXED (wave 15): appendFeatureBlockers now blocks when the incompatible-feature detail is absent/unparseable (the detector always emits it, so absence = incomplete detection), instead of value_or(0) passing preflight.

ai (B1):
- [x] M-B1-5  ai_app_action_planner.cpp:116 provider-gateway app_run_action has no catastrophic path. FIXED (wave 24): AiAppActionPlan gained a `catastrophic` flag set from commandLooksCatastrophic || commandLooksObfuscated; authorizeAppAction now runs authorizeCatastrophicAppAction FIRST and in every access mode (mandatory callbacks.confirm), so a format/wipe/obfuscated manifest command can no longer be satisfied by only the Unattended restore-point offer. Regression test flagsCatastrophicManifestCommand.
- [x] M-B1-6  ai_provider_gateway_tool_runner.cpp:397 recipe input-tier steps run without the per-call hard confirm. FIXED (wave 24): the recipe-step gate changed from isWin32InputTool (which includes browser/clipboard/extension input tools that the direct path hard-confirms in every mode) to a new positive allowlist isWin32DesktopInputTool ({click_text, uia_click_control, dismiss_dialog, mouse_click, type_text, send_keys, focus_window}). Browser/clipboard/extension input tools inside a win32_gui recipe are now rejected (require a per-call confirm); a newly added input tool defaults to rejected until whitelisted. Regression assertions in classifiesWin32McpToolRisk.
- [x] M-B1-13 ai_mcp_session_pool.cpp:33 DEFERRED: timeout_ms in the key is DELIBERATE (a pooled session bakes its timeout at open(); dropping it reintroduces stale-timeout inheritance). The real fix is LRU eviction / a capacity cap on m_sessions (close+evict oldest), which needs a careful pool refactor + safe session close. Tracked.
- [x] M-B1-14 ai_mcp_stdio_client.cpp:165 reads a full line before the byte cap. FIXED (wave 20): handleReadyRead now enforces the byte cap BEFORE reading any line, so a single oversized newline-terminated line is refused up front (fail closed) instead of being allocated whole; the post-loop newline-free guard stays.
- [x] M-B1-15 ai_mcp_stdio_client.cpp:128 server-exit uses terminate() not tree-kill. FIXED (wave 29): on server-exit the finished-lambda calls fail(...,force_kill=false) and stopProcess early-returned on NotRunning, so neither terminate() nor the parent-PID snapshot walk (which finds nothing once the parent exited and children reparented) reaped descendants. StdioToolCallWorker now places the live server in a KILL_ON_JOB_CLOSE Windows Job Object in onStarted() (assignProcessToJob), and stopProcess closes the job first (closeJob) -- reaping the WHOLE tree including a server that exited on its own -- returning before the per-process fallback. If the job cannot be created/assigned it falls back to the existing terminateProcessTree()+kill so cleanup is never weaker than before; a destructor closes the job as a safety net. (Mirrors the ExecutionBroker job pattern; no unit seam -- needs a live process.)
- [x] M-B1-16 ai_execution_broker.cpp:90 job-create/assign failure -> kill primary only, descendants survive. FIXED (wave 24): terminateProcessTree now falls back to a recursive Toolhelp32 snapshot kill (killProcessTreeSnapshot) when the Job Object could not be established, run while the primary is still alive so descendants are reaped rather than orphaned. When the KILL_ON_JOB_CLOSE job IS present it still governs; the fallback only makes cleanup never weaker than the direct primary kill.
- [x] M-B1-17 ai_provider_registry.cpp:72 FIXED (wave 16): commandWithinAppDir now also requires the CANONICAL command path (symlinks/junctions resolved) to stay within the canonical app dir when the file exists, so a within-dir symlink cannot redirect the launch outside. The lexical check still governs a not-yet-existing command (reported "missing" separately). Residual validate/launch TOCTOU noted.
- [x] M-B1-21 ai_workflow_placeholders.cpp:81 PowerShell single-quote invariant never validated; user workflows override built-in ids. FIXED (wave 30, Part A) + Part B REJECTED as an over-reach. Part A (real fix): powerShellCommandTemplateIsSingleQuoteSafe(command) validates -- using the SAME `[A-Za-z0-9_]+` placeholder grammar the substitutor recognizes (so PowerShell's own $vars/${...:colon} are not flagged) -- that every ${...} placeholder in a run_powershell command sits INSIDE a single-quoted literal (tracking '' escapes); validateWorkflowPhases enforces it at load for user AND bundled templates, so a template that would inject a value unescaped is rejected. All bundled templates verified to keep ${...} inside '...' (incl. $x='${tool_arguments}'). Regression test templateSingleQuoteSafetyGatesRawPlaceholders. Part B (user ids shadow built-ins) NOT implemented: user-directory workflows INTENTIONALLY override built-ins of the same id (a supported customization -- see the existing userDirectoryOverridesBuiltInWorkflow test); rejecting that would break the feature, and it is not a vulnerability (writing to the user workflow dir already requires local access, and Part A now validates every template including overrides). Confirmed by the gate: the CMake test targets needed ai_workflow_placeholders.cpp added to link the new validator.
- [x] M-B1-22 ai_assistant_panel.cpp:5453 FIXED (wave 12): a mutation requiring a lease now fails closed (emits leaseDeniedResult, blocks) when m_leaseManager is absent, instead of returning true.
- [x] M-B1-28 ai_credential_store.cpp:357 FIXED (wave 12): the assignment-secret regex gained an optional "? after the key name so quoted-JSON secrets ("password":"...") are redacted.

core-util (B2):
- [x] M-B2-11 offline_deployment_worker.cpp:1046 FIXED (wave 11): resolveChocoExecutable now requires choco.exe be a real regular file (not symlink/junction) canonically confined under the bundled-tools root, mirroring the H9 pattern.
- [x] M-B2-1  uup_iso_builder.cpp:268,1266 workspace create/removeRecursively no reparse/ownership check. FIXED (wave 20): the deterministic %TEMP%\sak_uup_<hash> workspace is now refused (create) and never recursively deleted (cleanup) when it is a reparse point, via the existing isReparsePoint() seam -- an attacker-planted junction can no longer redirect the elevated create/removeRecursively out of TEMP.
- [x] M-B2-10 offline_deployment_worker.cpp:244,746 marker-write failure returns true; no reparse/identity before removeRecursively. FIXED (wave 27): prepareOwnedWorkDir now fails closed on a marker-write failure (QSaveFile write+commit checked) so an unmarked tree is never later mistaken for foreign-and-undeletable, and refuses a reparse-point work dir before stamping. The three raw QDir::removeRecursively() cleanup sites (finalizeCancelledBuild + both finalizeBundle paths) now go through removeOwnedWorkDir, which deletes ONLY when workDirSafeToDelete (exists, not a reparse point, bears our ownership marker) -- a planted junction or a foreign dir is never recursively wiped; a missing dir is a safe no-op. Regression test workDirSafeToDelete_guardsReparseAndOwnership.
- [x] M-B2-13 offline_deployment_worker.cpp:359 header says unmet-dep=fatal but code warns+proceeds; zero-package bundle emits operationCompleted. FIXED (wave 27, two parts): (a) the doc/code mismatch was in the HEADER comment -- the code's warn+proceed on an unmet dep is the DELIBERATE, documented design (a normal Bundle fetches the dep from the feed at install time = will-fetch; only an air-gap/packed_only install cannot, and that is enforced per-entry at install). Regressing to "fatal" would break legitimate will-fetch bundles, so the stale "fatal build error" header comment on unmetClosureDependencies was corrected to describe the will-fetch behavior. (b) The real fail-open: finalizeBundle wrote no manifest for a zero-package bundle yet still emitted operationCompleted (a "success" whose bundle installFromBundle would reject as empty). It now fails closed -- emits operationError "No packages could be internalized; nothing to bundle" and cleans up -- mirroring executeBuildListManifest.
- [x] M-B2-15 offline_deployment_worker.cpp:1580 empty declared checksum passes unverified; downloaded>0 ships partial. FIXED (wave 20): the direct-download integrity gate switched from the permissive binaryChecksumMatches (empty->true) to the fail-closed installerVerified (empty declared checksum -> reject), so an unauthenticated direct download is never written or counted.
- [x] M-B2-16 vulnerability_scanner.cpp:1525 enumerateInstalledProgramsFast no incomplete flag -> denied hive = "complete" inventory. FIXED (wave 28): scanFastRegistryHive now returns a completeness bool (via fastHiveOpenIsComplete -- success or ERROR_FILE_NOT_FOUND = complete/empty, ACCESS_DENIED/other = incomplete; per-index enum/open failures also mark incomplete); enumerateInstalledProgramsFast(bool* out_complete=nullptr) ANDs the three hives. Callers surface it: listInstalledPrograms + scanVulnerabilities add an inventory_complete field (scanVulnerabilities also appends a source_error), vulnerability_panel emits an "inventory incomplete -- run elevated" progress line. Regression test fastHiveOpenIsComplete_treatsAbsentKeyAsCompleteDeniedAsIncomplete.
- [x] M-B2-35 package_internalization_engine.cpp:371 FIXED (wave 11): isSafePackageComponent now rejects Win32-illegal chars (< > " | ? *), trailing dot/space, and reserved device names (CON/PRN/AUX/NUL/COM1-9/LPT1-9). Regression rows added.
- [x] M-B2-31 windows_usb_creator_extract.cpp:618 bcdboot given drive root not a Windows dir (tracked in-code). NO CODE CHANGE (verified wave 20 planning pass): already both (a) documented in-code (lines 625-634 KNOWN LIMITATIONS) and (b) fail-closed -- runBcdboot gates certification on bcdboot's real exit via bcdbootReportsSuccess, so a root-source failure returns false and surfaces the error rather than falsely certifying bootable media. The residual is a FUNCTIONAL-correctness design change beyond this file (Windows install media has no top-level \\Windows tree; it lives inside sources\\install.wim), requiring either a valid Windows source dir for bcdboot or dropping bcdboot for install media in favor of the ISO's own extracted BCD store. Deliberately NOT adding a naive "<drive>\\Windows must exist" gate -- that would falsely fail-closed on legitimate install media. Tracking comment kept.
- [x] M-B2-28 disk_benchmark_worker.cpp:600,765 FIXED (wave 10): validateTestFileSize now rejects sequential_passes<=0/>1000 (loop-skip 0 MB/s success) and random_duration_sec<=0/>3600 (bounds the *1000 int overflow).
- [x] M-B2-29 disk_benchmark_worker.cpp:183 FIXED (wave 10): randomIoResultUsable requires total_failures < total_ops (was <=), so a 50%-failure run is not scored as usable.

gui/actions/elevated (B3):
- [x] M-B3-10 network_diagnostic_panel.cpp:4658 FIXED (wave 13): onResetNetworkClicked now confirms (showQuestionLogged, default No) before the destructive reset.
- [x] M-B3-11 diagnostic_benchmark_panel.cpp:1078 FIXED (wave 13): onQuickActionClicked confirms for the repairing "Verify System Files" action (SFC/DISM) before running.
- [x] M-B3-12 reset_network_action.cpp:224 FIXED (wave 18, reboot part): m_requires_reboot is set only on a SUCCESSFUL Winsock reset, not unconditionally. Residual: cancel mid-reset still leaves partial mutation unrolled (rollback from the captured winsock backup) -- noted for a follow-on.
- [x] M-B3-19 backup_bitlocker_keys_action.cpp:671 FIXED (wave 15): the cancel-cleanup lambda now checks removeRecursively(); if the partial backup dir cannot be removed it emits a FAILURE (plaintext keys may remain) instead of a clean cancel.
- [x] M-B3-20 backup_bitlocker_keys_action.cpp:323 detection returns {} on parse/query error -> "no BitLocker" == failure. FIXED (wave 25): detectEncryptedVolumes(bool& query_ok) / parseDetectedVolumes(output, bool& parse_ok) now thread a success flag (mirroring the getKeyProtectors/parseKeyProtectorResponse pair) -- empty output = success+empty (genuine no-BitLocker), a failed/denied query or malformed/scalar JSON = false. scan() and executeDiscoverVolumes() fail closed on !query_ok with a distinct "BitLocker detection failed / volume set unknown -- run elevated" result instead of reporting an empty set. Regression test parseDetectedVolumes_signalsParseState.
- [x] M-B3-22 optimize_power_settings_action.cpp:311 failed powercfg discovery -> hard-coded GUID fallback then mutates. FIXED (wave 25): enumeratePowerPlans(bool& discovery_ok) reports whether powercfg actually ran; resolveHighPerformancePlan uses the canonical built-in GUID ONLY when discoveryPermitsActivation(discovery_ok, plans_found) (discovery succeeded AND >=1 plan). execute() fails closed (reports "Power plan discovery FAILED; not mutating" and returns without setPowerPlan) when discovery failed, so the system is never mutated on a guessed GUID. findPowerPlanByName replaced by static findPlanByNameIn over an already-enumerated list (single enumeration). Regression test discoveryPermitsActivation_failsClosedWithoutDiscovery.
- [x] M-B3-5  elevated_helper_main.cpp:845 client-loss treated as "no message" -> dead client never cancels the privileged task. FIXED (wave 28): ElevatedPipeServer gained a tri-state pollPipe() (NoData / MessageReady / Broken) built on the pure classifyPeek(peek_ok, bytes) seam -- a failed PeekNamedPipe or invalid handle is Broken. finishActiveTaskWithCancelPolling switches on pollPipe(): NoData continues, MessageReady reads, and Broken now sets cancel_requested and returns (so a dead client's loss cancels the privileged task instead of looping forever). Regression test testClassifyPeekDistinguishesBrokenFromNoData.
- [x] M-B3-7  organizer_panel.cpp:199 QThread wait(15s) then unique_ptr delete of a possibly-running thread -> abort. ALREADY FIXED (verified wave 20 planning pass, no code change): m_worker/m_dedup_worker are std::unique_ptr whose worker destructors call stopAndJoin (WorkerBase::~WorkerBase / DuplicateFinderWorker::~DuplicateFinderWorker): requestStop -> wait(15s) -> terminate() -> wait(5s) -> std::abort ONLY if the thread survives terminate for 5s (deliberate last-resort fail-closed to avoid use-after-free). The unique_ptr delete therefore never runs a raw ~QThread on a live thread, so the "wait then delete -> abort" the finding describes cannot occur. The "potential resource leak" log text is cosmetically misleading (the thread is joined/terminated, not leaked) but not load-bearing.
- [x] M-B3-8  user_profile_backup/restore_wizard_execute.cpp no page dtor joins the QThread-subclass worker -> closing wizard mid-op aborts. ALREADY FIXED (verified wave 20 planning pass, no code change): both wizard workers are QThread subclasses created PARENTED to their page (new UserProfileRestoreWorker(this) / new UserProfileBackupWorker(this)), and each worker's OWN destructor joins the thread (if(isRunning()){cancel(); if(!wait(timeout)) wait();} -- bounded then UNBOUNDED wait, never destroying a running QThread). When the QWizard is destroyed it deletes its pages; each page's ~QObject disconnects receiver connections (late worker signals become no-ops) then deletes children, invoking the worker dtor's join. Closing the wizard mid-op blocks-and-joins rather than aborting. A separate page dtor is unnecessary.
- [x] M-B3-9  screenshot_settings_action.cpp:44 grabWindow/QPixmap on a worker thread (GUI-thread affinity). FIXED (wave 31): execute() runs on the quick-action worker thread, so captureSettingsWindow's QScreen::grabWindow (GUI-thread-affine) touched the GUI off-thread. Added grabAndSaveOnGui(WId, filepath) (GUI-thread-only grab+save) and captureWindowToPng(WId, filepath), which marshals it onto the GUI thread via BlockingQueuedConnection and fails closed (returns false, no file) when there is no QCoreApplication or the invocation cannot be marshalled; captureSettingsWindow now calls captureWindowToPng. Also DELETED the dead openSettingsAndCapture + captureScreen(QString) cluster (unreferenced; a second off-thread grabWindow) and the unimplemented openSettings()/captureScreen() decls. Regression test captureWindowToPng_failsClosedWithoutScreen.
- [x] M-B3-1  partition_script_builder.cpp:2297 Restore Image approval pins only size not content; same-size swap accepted; UNC bypasses same-disk check. PARTS B+C FIXED (wave 33); PART A (content pin) DEFERRED. Parts B/C (the security gaps): a new restoreImageSourceGuardScript -- symmetric to createImageDestinationGuardScript -- is emitted for every RestoreImage: it (C) rejects a UNC/network source (StartsWith '\\\\' but not '\\\\.\\'/'\\\\?\\', which a \\localhost\\C$ or \\127.0.0.1\\C$ alias could use to name the target and bypass the same-disk check), and (B) resolves the source image file's real volume identity via Get-SakVolumeGuid (follows reparse points) and throws if it maps onto the TARGET disk being restored (source would be overwritten mid-restore). Regression test scriptBuilder_restoreImageGuardsUncAndOnTargetDiskSource. Part A (size-only content pin -- a same-size content swap between queue and copy is still accepted at line 2296) is DEFERRED: closing it needs a content fingerprint (SHA-256, or an offset-sampled fingerprint for multi-GB images) captured at approval/queue time into the payload and re-verified in the emitted script before the copy, plus a FileShare::Read source handle -- a larger design change to the certified raw-disk restore path best done deliberately, not as a rushed addition. The queue guard's existing size check remains, so a same-size swap is the only residual (a source that changed size is still caught).

pst/email (A1):
- [x] M-A1-26 user_data_manager.cpp:965 atomicReplaceFile deletes target then renames; rename failure destroys the original. FIXED (wave 26): atomicReplaceFile now moves the existing target ASIDE (rename to <target>.sak_old) before renaming the staged replacement into place, and on a swap failure rolls the original back -- the original is never destroyed while the swap is incomplete (no data-loss window). Made a public static seam; regression test atomicReplaceFileSwapsWithoutDataLossWindow (existing-target replace + not-yet-existing-target move).
- [x] M-A1-20 user_data_manager.cpp:281,307,404 public path validation via Q_ASSERT_X (release no-op) -> empty paths run CWD-relative. FIXED (wave 20): backupMultipleApps/restoreAppData/restoreMultipleApps now use the release-effective allPathsPresent() guard (emits operationError + returns false) instead of Q_ASSERT_X. Regression test allPathsPresentRejectsEmpty.
- [x] M-A1-29 email_constants.h:215 kAnsiVersion=14 only; valid ANSI wVer 15 PSTs rejected. FIXED (wave 26): added kAnsiVersion2=15 (MS-PST 2.2.2.6: an ANSI wVer is 14 OR 15) and admitted it in isKnownDataVersion; 15 stays classified as ANSI (15 < kUnicodeVersion, != kUnicode4kVersion). Unknown versions still fail closed. NO unit test: the wave-F header CRC integrity gate rejects any synthetic header before the version gate is reached (a version override invalidates the pre-computed CRC), so version acceptance cannot be isolated without a valid-CRC v15 ANSI fixture (none exists) or replicating the MS-PST header CRC in the test; rejectsUnknownDataVersion still guards the reject direction. Change is spec-minimal.
- [x] M-A1-25 user_data_manager.cpp:783 decryptArchiveToTempFile readAll() of an attacker file with no size cap before the zip-bomb preflight -> OOM. FIXED (wave 20): a 4 GiB pre-read cap (encryptedArchiveSizeOk, also rejecting a negative/unreadable size) fails closed before readAll(), bounding the allocation ahead of the post-decrypt zip-bomb preflight. Regression test encryptedArchiveSizeCapBoundary.

===============================================================================
## LOW (robustness / defense-in-depth / dead code)
===============================================================================

A1:  #10 attachment drop no log; #27 deleteBackup removes sidecar on payload-delete failure; #34 non-atomic metadata/manifest write; #36 decrypted-temp remove ignored; #41 getBackupInfo hides read failure; #40 errorOccurred never emitted; #38 wrong magic comment; #39 failOpen misnomer.
A2:  #1 netsh via %SystemRoot% (use GetSystemDirectoryW); #23 missing ethernetRestoreComplete(false); #24 dead code (page startRestore + 5 on* slots + folderConfig); #8 dhcp_enabled default true; #17 appdata separator normalization + equal-length dup; #21 sidecar readAll no size cap.
A3:  #10 focus recheck; #11 drag requireSnapshotTab; #12 press/release try/finally (stuck CDP key); #28 click_text virtual-screen bound; #20 Network.enable swallow -> network_idle false-succeed; #21 bodyText "" -> absent-wait false-satisfy; #31 present-but-failed Invoke falls through; #56 PDF fromBase64 without AbortOnBase64; #58 forcelist uninstall TOCTOU; #63 EnumWindows/GetWindowRect returns unchecked; plus #8/#13/#15/#18/#19/#23/#24/#32/#33/#52/#53 defense-in-depth; #66/#67/#68 dead/duplicated code.
A4:  #3 absent-oid no blocker; #9 usedBytes 0 on corrupt metadata; #17 recreate size overflow; #18 builder payloadBool strictness; #19 recreate-FS divergence; #21 blockCount_==0 bound; #24/#25 EXT reader bounds; #30 detector supplemental errors; #32 remaining unchecked adds; #33 EXT read ceiling; #34 inconsistent-encryption accepted.
B1:  #11 wall-clock lease expiry; #12 isJsonRpcResponse no id/jsonrpc check; #18 malformed function_call dropped; #20 subagent batch no up-front validate; #23 readAll unbounded (trusted HTTPS); #24 [x] wave19 finalizeResult now defaults success only when absent (no longer clobbers success:false); #25 isError string->false; #27 cred size TOCTOU; #29 broker coercions; #30 timeout signed-overflow; #31 cli method run via powershell; #32 setPermissions no owner DACL; #33 cred version unchecked; #34 dispatcher release non-RAII/no owner check; #35 provider registry phantom-available; #36 dup tree-kill helper.
B2:  #20 uup rollback rename return ignored; #21 hashless post-download not SHA-1'd; #22 benchmark file-id pinning; #25/#26 advanced_search incomplete flag not routed to filesUnreadable(); #28/#29/#30 disk_benchmark validation/overflow/flush; #37 safeInstallerFilename reserved-name; #40 wrong-typed bools coerced; #43 scanStandardDirs no error channel; #46 empty-updateId asserts; #32 extract 7z last-entry/critical-name/join.
B3:  #17 PS progress chunk >4 MiB pipe break; #4 elevated job-containment failure not surfaced; #14 payloadUInt64 UB cast; #23 [x] wave19 volume-usage divide-by-bytesTotal() now zero-guarded; #24 read-only disks excluded from scan; #25/#26 dead code (captureScreen decl, restore-page slots, m_resumedApprovedToolCallIds); #3 qputenv return + winsock reopen reparse residual.

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

--------------------------------------------------------------------------------
## RE-TRIAGE 2026-08-27 (R5-IDX-5) -- 6 partials -> 3 closed, 3 genuinely open

Under the standing rule that an item is INCOMPLETE or a DESIGN DECISION, never an indefinite
"partial". Two of the three closures were work that had LANDED and was never marked, which is
the same not-writing-it-down failure as R5-IDX-6.

  M-A2-6   DONE (was marked deferred). The verify-before-swap restructure the entry asked for
           is in the tree: copyFileReplacingExisting moves the displaced original aside and
           RETURNS it rather than deleting it, and the caller finalizes it only after
           applyPermissions + verifyFile pass -- restoring it if either fails. The staging
           path also gained a 64-bit random token, because a fixed suffix let a local attacker
           pre-plant a dangling symlink at a guessable path.
  M-B1-13  DONE (was marked deferred). The entry's own proposed fix was "LRU eviction / a
           capacity cap on m_sessions"; a kMaxPooledSessions cap is in place and the pool
           refuses at capacity with an explicit message. Keeping timeout_ms in the key stays
           deliberate, as the entry said.
  M-B2-31  DECISION. bcdboot's drive-root source is a documented KNOWN LIMITATION in-code,
           and the run is gated on bcdboot's real exit (bcdbootReportsSuccess), so nothing is
           falsely certified as bootable. The residual is a functional design change beyond
           this file; a naive "<drive>\Windows must exist" gate is deliberately NOT added
           because it would fail-closed on legitimate install media.

STILL OPEN. All three are INCOMPLETE -- none is a design decision, and none should be
called a "deferral". That word is the middle category the non-negotiables forbid precisely
because it lets an item sit forever without anyone deciding anything: an item is INCOMPLETE
(someone is expected to finish it) or a DESIGN DECISION (nobody is, and the reason is
recorded). Re-classified 2026-08-27 with what each actually needs and what actually blocks
it, so the reader can tell the difference between "hard" and "waiting on a machine":

  H6       INCOMPLETE, and TRACTABLE NOW -- the entry over-states its own difficulty. It
           calls the fix "a privilege-architecture change", but ElevationManager::isElevated()
           already exists and is used in five places. The defect is narrower than the entry
           says: AiCommandResult::elevated records the model's CLAIM (requires_admin) rather
           than the FACT. On the non-admin path the broker launches a plain QProcess that
           inherits S.A.K.'s token, so when S.A.K. is itself elevated the child runs elevated
           while `elevated` stays false and the policy takes no lease and no restore point.
           Neither ai_execution_broker.cpp nor ai_tool_policy.cpp references ElevationManager
           at all. The fix is to feed effective elevation
           (requires_admin || ElevationManager::isElevated()) into the policy context and the
           result. Bounded by the C2/C3/C4 content classifier today -- a destructive command
           is gated on content regardless of requires_admin -- which is why this is not
           urgent, not why it is hard.

  M-A4-6   INCOMPLETE, BLOCKED ON THE CERT RIG. The work itself is well specified in this
           entry (one commitInPlaceFileReplace modelled on commitInPlaceFilePatch). What
           blocks it is not design: changing the certified in-place COW commit path requires
           a crash-safety round-trip re-certification on the macOS VM rig, which is owner
           capacity, not agent work. A crash between the two checkpoints loses the file --
           a data-loss WINDOW, not corruption, and recoverable from backup.

  M-B3-1   INCOMPLETE, WITH ONE OWNER DECISION INSIDE IT. Part A only: the Restore Image
           approval pins size, not content, so a same-size content swap between queue and
           copy is accepted (Parts B and C -- UNC source, source-on-target-disk -- are fixed).
           Emitting a digest check in the generated script is small. The decision is WHICH
           digest: a full SHA-256 of a multi-GB image costs minutes at approval time, and an
           offset-sampled fingerprint is fast but only probabilistic against an attacker who
           knows the sampling offsets. That trade-off is the owner's to make, and it is the
           real reason this is open -- not the size of the change.

This campaign is NOT at zero and stays in docs/ until it is.

--------------------------------------------------------------------------------
## H6 AND M-B3-1 PART A CLOSED 2026-08-27

H6 -- the entry's own difficulty estimate was the thing blocking it. It called the fix "a
privilege-architecture change too risky to retrofit inline"; the actual defect was that
requires_admin is the MODEL'S CLAIM and was being read as the FACT. ElevationManager::
isElevated() already existed and was used in five places.
  - AiToolCallRequest gains host_elevated, INJECTED rather than read inside the policy, so
    ai_tool_policy stays a pure testable rule. policyContext now computes
    effective_admin = requires_admin || host_elevated.
  - The planner and the subagent executor supply it from ElevationManager::isElevated().
  - AiCommandResult::elevated now records what actually happened on the PLAIN path too: it
    reported false unconditionally, while QProcess inherits this process's token, so inside
    an elevated S.A.K. the command ran with administrator rights and the trace said it did
    not.
  Regression test elevatedHostMakesANonAdminCallRisky, deliberately driven with a BENIGN
  non-shell command so effective elevation is the only thing that can make it risky.
  DRILLED: collapsing effective_admin back to requires_admin turns it RED.
  Linkage note: the AI test targets did not link ElevationManager, and it logs, so the two
  travel together as SAK_ELEVATION_SOURCES rather than as two lines per target -- a new
  target cannot then pick up half the dependency.

M-B3-1 PART A -- the digest choice was the open question, and it is now made: a FULL SHA-256,
not an offset-sampled fingerprint. This gates a raw whole-disk overwrite, and a sampled
digest is only probabilistic against an attacker who chooses the content and can read the
sampling offsets out of the emitted script. The program already pins bundled filesystem-tool
binaries with a full SHA-256 (Invoke-SakFilesystemTool), so this is the established shape.
  - onRestoreImage hashes the approved image through sak::file_hasher behind a cancellable
    progress dialog (a multi-GB image takes real time and a frozen window reads as a hang)
    and stores source_sha256 in the payload. Fail closed: no digest, nothing queued.
  - The emitted script re-hashes immediately before the copy and throws on mismatch, and
    throws outright when the payload carries NO fingerprint -- a restore that silently drops
    its content check is the defect itself.
  The progress dialog is INDETERMINATE, not a percentage, and the blocking-pattern gate is
  what forced that. The first version pumped QCoreApplication::processEvents inside the hash
  callback, which the gate refuses: it re-enters the GUI loop from inside a running operation.
  Showing a real percentage means marshalling progress back into a dialog that dies the moment
  the user cancels -- the detached task touching freed state shape fixed twice elsewhere in
  this same sweep. The hash now runs on a worker capturing only COPIES (path + stop token),
  the dialog's own modal loop runs meanwhile, and there is no processEvents, no hand-rolled
  QEventLoop and no blocking wait. A busy indicator with a working Cancel, chosen over a
  percentage bought with a lifetime hazard.

  Regression test scriptBuilder_restoreImagePinsSourceContent. DRILLED 3/3 RED.
  The first version of that test was WEAK and the drill caught it: it asserted the message
  text ("content changed") which survives a mutation to `if ($false)`. It now pins the
  emitted CONDITIONS.

--------------------------------------------------------------------------------
## M-A4-6 CLOSED 2026-08-28 -- the replace is ONE checkpoint, certified on the rig

commitInPlaceRootFileWrite replaced an existing file as a delete commit followed by an insert
commit. Each was individually crash-safe, but a crash in the window between them left the file
deleted and the replacement absent. Not corruption -- both states are internally consistent --
but one of them has lost the user's file.

commitInPlaceFileReplace fuses the two into a single checkpoint. This is NOT a change of
semantics: the end state is identical to running the two commits in sequence (the new file
still takes a fresh object id, the old file's records and data still go away). The only
difference is that both happen at one xid, so the only states an interruption can leave are
"the old file" and "the new file". The fused count deltas are literally the sum of the two
commits they replace, including the symlink/other-object deltas for a replaced object that is
not a regular file.

WHAT MADE IT DELICATE, AND WHAT CAUGHT THE BUG

On a snapshotted volume the two legs pull the live extent-ref delta in OPPOSITE directions:
the delete leg removes the replaced file's live-exclusive records (merely decrementing the
refcount where a clone made after the snapshot still points at those blocks -- freeing them
there would corrupt the clone), while the insert leg sizes the new fs-tree from that record
set and then extends it with the freshly allocated data. The removal has to happen BEFORE
sizing, or the tree is reserved against a record count that does not match what is written and
the COW chain is sliced wrong.

The first version had a different bug, and it is the reason this needed the rig rather than
review: it took the replaced file's payload from the CALLER instead of recovering it from the
live tree the way the delete leg does. That payload carries the name and id but not the data
extents, so NOTHING was freed. Every unit test still passed -- the content round-trips
perfectly -- and the host apfsck gate caught it immediately:
  Volume superblock: bad block count. ALLOC stored=7 computed=6
One block allocated with nothing referencing it. The fix is to recover the target through
buildDeleteFileList, exactly as the delete leg does.

CERTIFICATION

Host apfsck (apfsprogs, -cw) clean on every shape:
  create; replace GROW 1 -> 64 blocks; GROW 64 -> 1024 blocks; SHRINK 1024 -> 1 block;
  zlib-compressed replace; plain-over-compressed; lzbitmap resource-fork replace;
  and a replace on a SNAPSHOTTED volume (the diverge path).

Apple fsck_apfs on the macOS VM (10.10.11.102, macOS 26.6), attached -nomount so no kernel
mount advanced the checkpoint first -- clean on the basic replace, the shrink, the
resource-fork compressed replace, and the snapshotted replace. "Verifying allocated space"
passes, which is the check that would have caught the leak above.

macOS kernel RW-mount, content verified byte-for-byte: the basic replace reads back the NEW
47-byte content; the shrink reads back 100 bytes; the 4 MiB lzbitmap file is decompressed by
the kernel to its full size. On the snapshotted image the live file is the replacement
(262144 bytes) AND snapshot snapA (XID 4) survives intact.

Crash-interruption proof: apfsWriter_crashDuringReplaceKeepsTheOldFile drives the same
crash-rollback harness the insert path uses -- roll the container back to the previous
checkpoint as an interruption would -- and asserts doc.txt still exists WITH ITS ORIGINAL
CONTENT. Plus apfsWriter_inPlaceFileWriteCreatesThenReplaces now pins the property rather than
a literal: the replace advances the xid by exactly ONE (it required 5, i.e. two checkpoints,
before).

DRILLED 2/2 RED: restoring the two-commit delete+insert turns both tests red -- the crash test
because the rolled-back container has NO file, and the xid test because the replace costs two
checkpoints again.
