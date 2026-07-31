# Codex Full-Tree Review -- Remediation Tracking

Source: 10 Codex (gpt-5.6-sol, xhigh) passes over all 1st-party src, 2026-07-30.
~424 raw findings; 12 Claude verification agents (~141 CONFIRMED, ~24 PARTIAL,
3 FALSE-POSITIVE, ~2% FP). Raw lists preserved in scratchpad codex-*.md.

GOAL: every issue FIXED, TESTED, CERTIFIED. Check items off as completed.

## How to read this doc
- Each item: `- [ ] ID [SEV] file:line -- problem. Fix: approach.`
- Flip `[ ]` -> `[x]` when the item is fixed AND covered by a test AND the
  full suite is green. Append `(commit <sha>)`.
- Each batch closes with a CERT line: build clean + gate green + unit suite
  green + targeted test added. Only mark a batch DONE when its CERT passes.
- FALSE-POSITIVE and ACCEPTED items are listed at the bottom for the record
  (not counted against completion).

## Fix order (batches)
0. Own teardown-fix gaps (806d5c5)      -- 2 items
1. Elevated command injection            -- 7 items
2. Disk destructive / data-loss          -- 14 items
3. WorkerBase base-join UAF + teardown   -- 15 items
4. flash_worker memory-safety            -- 8 items
5. System security (ACL/elevation/probe) -- 16 items
6. Actions fail-open cluster             -- 22 items
7. Email subsystem                       -- 34 items
8. Files subsystem                       -- 30 items
9. Network subsystem                     -- 24 items
10. Apps / supply-chain                  -- 40 items
11. Partition APFS/HFS write-safety      -- 14 items
12. AI harness                           -- 8 items
13. win32 / browser control              -- 10 items

Progress: 0 batches DONE.

---

## BATCH 0 -- Own teardown-fix gaps (extends 806d5c5) -- DONE
- [x] B0-01 [CRIT] src/gui/main_window.cpp:1635 -- appendLogIfActive writes m_panelLogs[tabIdx] with no m_shutting_down guard; m_panelLogs destroyed before panels, ~AdvancedUninstallPanel emits logOutput -> write into destroyed QMap (UAF on normal close). FIXED: guard with m_shutting_down.
- [x] B0-02 [CRIT] src/gui/main_window.cpp:631 -- currentChanged->materializeTab lambda has no m_shutting_down guard; panels emit currentChanged during teardown -> builds panel + assigns member unique_ptr mid-teardown. FIXED: guard with m_shutting_down.
- CERT B0: Release offscreen smoke close-loop 20/20 exit 0 (all panels materialized then torn down); shipping-config method, matches 806d5c5 release cert. Debug app-loop N/A (pre-existing Q_ASSERT pops modal dialog under smoke teardown; unrelated to this fix; guards are deterministic early-returns).

## BATCH 1 -- Elevated command injection
- [ ] B1-01 [CRIT] src/core/partition_script_builder.cpp:4533,4551 -- new_file_system inserted UNQUOTED into Format-Volume (Split op); `NTFS; <cmd>` runs. Fix: route through isSupportedFileSystem allowlist + quote.
- [ ] B1-02 [CRIT] src/core/partition_script_builder.cpp:4447,4472,4474 -- target_style UNQUOTED into Initialize-Disk (ConvertStyle); `GPT; <cmd>` runs. Fix: GPT/MBR allowlist + quote.
- [ ] B1-03 [CRIT] src/core/partition_script_builder.cpp:1776,4918,5057 -- diskpart label strips quotes but keeps CR/LF; embedded newline becomes extra diskpart command line. Fix: strip/reject CR/LF in label.
- [ ] B1-04 [HIGH] src/core/partition_executor.cpp:114,238 -- credential temp path into single-quoted PS literal, no escaping; apostrophe / crafted TEMP injects into elevated RunPowerShell. Fix: quotePowerShell (used on peers).
- [ ] B1-05 [HIGH] src/ai/ai_workflow_placeholders.cpp:81 + resources/ai/workflows/technician_tool_assisted_task.json:33 -- ${tool_arguments} substituted UNQUOTED; `; Remove-Item ...` injection. Fix: quote/validate substitution.
- [ ] B1-06 [CRIT] src/actions/reset_network_action.cpp:148 -- temp path into `cmd.exe /C "netsh ... > %1"`; %VAR% expands inside quotes -> elevated injection via attacker TEMP/env. Fix: avoid cmd.exe %VAR%, pass literal/validated path.
- [ ] B1-07 [HIGH] src/core/package_internalization_engine.cpp:599 -- unescaped user paths into single-quoted PowerShell (extraction/repack). Fix: PS-escape paths.
- CERT B1: injection unit tests (metachar/CRLF/apostrophe) reject; suite green.

## BATCH 2 -- Disk destructive / data-loss
- [ ] B2-01 [CRIT] src/core/partition_safety_validator.cpp:152,1773 + partition_script_builder.cpp:4794 -- WipeDisk blocked only on is_system; boot-but-not-system disk wipes with no runtime guard. Fix: add is_boot runtime block on wipe.
- [ ] B2-02 [CRIT] src/core/partition_manager_controller.cpp:204,213 -- GUI apply accepts warning-bearing live inventory on hash match, never reruns validation; null IsBoot/IsSystem fail open into wipe. Fix: rerun validation; treat null as unsafe; include is_boot/is_system in hash.
- [ ] B2-03 [CRIT] src/core/partition_safety_validator.cpp:963,969 + partition_script_builder.cpp:1993,2250 -- clone accepts target_disk_number without target_offset_bytes; builder defaults offset 0 -> writes source over target partition table. Fix: require explicit offset or reject.
- [ ] B2-04 [CRIT] src/core/windows_usb_creator.cpp:103 -- any numeric disk accepted then DiskPart clean; no removable/system/boot/identity check. Fix: identity + removable/system/boot guard.
- [ ] B2-05 [CRIT] src/core/flash_coordinator.cpp:384 -- any accessible physical disk passes validation for raw write; system/boot/fixed/identity checks missing. Fix: same guard set.
- [ ] B2-06 [CRIT] src/core/leftover_scanner.cpp:891 -- protected path matching installLocation exempted, classified Safe, auto-selected; C:\Windows can become recursive-delete target. Fix: never exempt protected roots.
- [ ] B2-07 [CRIT] src/core/advanced_uninstall_controller.cpp:215,239 -- cleanLeftovers accepts arbitrary paths -> CleanupWorker with NO protected-path check on GUI path (guard wired only to AI path). Fix: wire leftover_cleanup_guard.h to GUI path.
- [ ] B2-08 [HIGH] src/core/drive_unmounter.cpp:77,149 -- enumeration/query failure collapses to empty list treated as successful unmount -> raw write to mounted disk. Fix: fail closed on enum error.
- [ ] B2-09 [HIGH] src/core/drive_unmounter.cpp:84,268 -- automount-prevent failure ignored, locks released, disk can remount during raw writes; also no rollback / persistent-offline on partial failure. Fix: honor failures + rollback.
- [ ] B2-10 [CRIT] src/core/partition_hfs_file_system_writer.cpp:42 + partition_raw_device_io.cpp:514,568 -- HFS image-only gate matches only `\\.\` and `\\?\GLOBALROOT\`; PhysicalDrive*/volume-GUID/`/dev/*` fall through to QFile RW -> raw device opened R/W. Fix: complete raw-target detection (dedupe helper).
- [ ] B2-11 [CRIT] src/tools/sak_apfs_writer_cli.cpp:217 + sak_hfs_writer_cli.cpp:58 -- --output-json truncates path after fs op, no alias check vs --target/--output-image -> JSON overwrites image/raw target. Fix: reject alias of target.
- [ ] B2-12 [HIGH] src/tools/sak_apfs_writer_cli.cpp:723 -- import-image QFile::remove(output) (ignored) before copy; copy failure destroys prior output, no rollback. Fix: copy-then-replace.
- [ ] B2-13 [HIGH] src/core/partition_safety_validator.cpp:502,526,542 -- create-image destination checks only textual drive/GUID prefixes; junction/mount/symlink can redirect image onto source disk. Fix: resolve real path.
- [ ] B2-14 [HIGH] src/core/partition_script_builder.cpp:1814,1829,1833 -- backup/reformat silently falls back from failed VSS snapshot to live copy, then destroys source. Fix: fail or explicit consent on snapshot failure.
- CERT B2: guard unit tests (boot disk, null flags, zero-offset clone, protected root, raw-path gate, output-json alias); suite green.

## BATCH 3 -- WorkerBase base-join UAF + teardown races
Systemic root: ~WorkerBase joins the thread as BASE subobject, AFTER derived
members destroyed -> UAF for any subclass destroyed mid-execute(). Preferred
root fix: move stop+join into a protected helper called by each most-derived
dtor (or a CRTP/enforced pattern), then remove terminate() fail-open.
- [ ] B3-01 [HIGH] src/threading/worker_base.cpp:18,21 -- terminate() fallback + unchecked final wait (systemic base). Fix: no terminate default; join in derived; check wait.
- [ ] B3-02 [MED] src/threading/worker_base.cpp:42 -- run() clears an already-requested stop -> cancel right after start() lost. Fix: don't clear pending stop.
- [ ] B3-03 [HIGH] include/sak/advanced_search_worker.h:44 + src/core/advanced_search_controller.cpp:120,124 -- no derived-level join; members destroyed before base stops running execute(). Fix: derived-dtor join.
- [ ] B3-04 [HIGH] include/sak/organizer_worker.h:60 + src/gui/organizer_panel.cpp:157,944 -- OrganizerWorker no derived join; unique_ptr destroyed while running. Fix: derived-dtor join.
- [ ] B3-05 [HIGH] src/gui/file_management_explorer_panel.cpp:518,524 -- search-worker wait result ignored; base teardown can delete running AdvancedSearchWorker. Fix: check wait / derived join.
- [ ] B3-06 [HIGH] src/core/diagnostic_controller.cpp:309-323 + include/sak/cpu_benchmark_worker.h:39 -- four benchmark worker waits unchecked; live workers lose derived state before base shutdown. Fix: check waits; derived join.
- [ ] B3-07 [CRIT] src/gui/app_installation_panel_actions.cpp:79,386 + app_installation_panel.cpp:99 -- QtConcurrent::run jobs capture raw this; dtor neither cancels nor joins search futures -> deref freed this/m_choco_manager. Fix: track+cancel/join futures or QPointer guard.
- [ ] B3-08 [HIGH] src/core/quick_action_controller.cpp:112,116,251,410 -- deleteLater on running QThread on wait-timeout; m_actions freed while thread executes; require_confirmation discarded; same action scanned+executed concurrently (failed moveToThread + state race). Fix: join before delete; honor confirmation; serialize action.
- [ ] B3-09 [HIGH] src/core/network_diagnostic_controller.cpp:167,176,1281 -- timed-out ops force-terminated, second wait unchecked, thread deleted while running; cancel() misses LAN upload/adapter/report. Fix: extend cancel coverage; check waits.
- [ ] B3-10 [HIGH] src/core/ost_converter_controller.cpp:125,139,144,328,337 -- ignores failed waits then deletes worker/thread possibly executing; cancel uses terminate() and ignores wait. Fix: safe stop, check waits.
- [ ] B3-11 [HIGH] src/core/advanced_uninstall_controller.cpp:544,549 -- enumeration force-terminate, second wait ignored, tracking cleared while m_enumerator may run. Fix: check waits.
- [ ] B3-12 [HIGH] src/threading/partition_apply_worker.cpp:39 -- teardown can force-terminate thread mid-op, leaving elevated child running with owner state destroyed. Fix: bound to pre-write UAC window only / safe cancel.
- [ ] B3-13 [HIGH] src/core/app_mutating_actions.cpp:1734,2046 -- uninstall/cleanup bridges force-terminate timed-out destructive workers, ignore final wait. Fix: safe stop, check waits.
- [ ] B3-14 [HIGH] src/core/partition_manager_controller.cpp:54-84 -- dtor detaches active apply/check futures instead of joining; destructive disk work continues after teardown. Fix: join or explicit documented detach with cancel.
- [ ] B3-15 [MED] GUI detached mutations continue after close (uncancellable): src/gui/user_profile_restore_wizard_pages.cpp:1899 (Chocolatey install), src/gui/network_diagnostic_panel.cpp:2374 (netsh IP/DNS), src/gui/wifi_manager_panel.cpp:1766 (WLAN profile writes). Fix: track + cancel/await on close.
- CERT B3: worker stress test (start/cancel/destroy mid-execute) crash-free under ASAN-equivalent debug; offscreen close-loop; suite green.

## BATCH 4 -- flash_worker memory-safety
- [ ] B4-01 [CRIT] src/threading/flash_worker.cpp:560 -- short sample read resizes buffers to bytesRead; next iteration reads full block into shrunken buffer -> HEAP OOB WRITE. Fix: keep buffer capacity; track valid length separately.
- [ ] B4-02 [CRIT/HIGH] src/threading/flash_worker.cpp:72 -- ~FlashWorker closes device + destroys m_imageSource before base ~WorkerBase joins; on coordinator wait-timeout (flash_coordinator.cpp:515) live execute() touches closed handle/freed source -> UAF + corrupted media. Fix: derived-dtor join before close.
- [ ] B4-03 [HIGH] src/threading/flash_worker.cpp:116 -- lockVolume()/dismountVolume() returns discarded; raw write proceeds against possibly-mounted volume. Fix: fail on lock/dismount failure.
- [ ] B4-04 [CRIT] src/threading/flash_worker.cpp:106 -- no target-capacity check before write; oversized image overwrites whole target then fails partial. Fix: capacity check pre-write.
- [ ] B4-05 [HIGH] src/threading/flash_worker.cpp:344 -- premature zero-byte source read ends write loop, reports success without verifying expected length. Fix: length invariant.
- [ ] B4-06 [HIGH] src/threading/flash_worker.cpp:514 -- sample verify auto-passes images smaller than one sample block (no bytes compared). Fix: compare actual bytes.
- [ ] B4-07 [HIGH] src/threading/flash_worker.cpp:577 -- target sample read never checks bytesReadFromDevice; stale bytes compared, short read counted verified. Fix: verify read length.
- [ ] B4-08 [CRIT] src/core/flash_coordinator.cpp:515,522 -- after worker fails to stop, m_workers.clear() destroys running QThread -> fatal/UAF. Fix: don't destroy unstopped worker; escalate/await.
- Related (coordinator correctness): flash_coordinator.cpp:168 duplicate target paths not rejected (concurrent same-disk write); :285 signals emitted while m_mutex held (deadlock); :234 setBufferCount stored-not-used.
- CERT B4: OOB + capacity + verify unit tests (short read, sub-block, oversized); suite green.

## BATCH 5 -- System security (ACL / elevation / probe / diagnostics)
- [ ] B5-01 [CRIT] src/core/permission_manager.cpp:49,334 -- "strip permissions" passes NULL DACL to SetNamedSecurityInfoW -> grants Everyone unrestricted access. Fix: restore inherited ACL, not null DACL.
- [ ] B5-02 [HIGH] src/core/permission_manager.cpp:124,420 -- standard-user setup replaces ACL with one user ACE + protects from inheritance, removing SYSTEM/admin/service access. Fix: preserve required principals.
- [ ] B5-03 [HIGH] src/core/permission_manager.cpp:294 -- SDDL application always writes owner+protected DACL even when SDDL omits them -> null/invalid descriptors. Fix: only set provided fields.
- [ ] B5-04 [CRIT->DEF] src/elevated/elevated_helper_main.cpp:60,307,857 + elevated_pipe_server.cpp:29,314 -- missing/invalid parent PID disables client validation while pipe permits Builtin Users. NOT exploitable today (requireAdministrator, broker passes valid PID, fail-closed on mismatch, pipe nonce) -- defense-in-depth. Fix: drop BU from DACL + fail-closed when PID absent.
- [ ] B5-05 [HIGH] src/elevated/elevated_helper_main.cpp:380 -- elevated PowerShell task sets success=true after process start even on timeout/cancel/nonzero exit. Fix: gate success on exit code.
- [ ] B5-06 [HIGH] src/core/elevation_broker.cpp:44,158,406 -- m_pipe_handle read/written across cancel/exec threads without sync -> data race UB. Fix: synchronize.
- [ ] B5-07 [HIGH] src/core/process_runner.cpp:26 + elevated_helper_main.cpp:400 -- output accumulated unbounded; helper output limit applied only afterward -> memory exhaustion. Fix: bound during accumulation.
- [ ] B5-08 [HIGH] src/core/app_paths.cpp:33 + logger.cpp:169 -- predictable probe filenames opened truncating + deleted; pre-existing files / planted links overwritten by elevated helper. Fix: unique temp + no-follow.
- [ ] B5-09 [HIGH] src/core/disk_benchmark_worker.cpp:279,347,429 -- fixed benchmark filename check/create/delete TOCTOU can truncate/delete replacement file. Fix: unique temp, exclusive create.
- [ ] B5-10 [HIGH] src/core/restore_point_manager.cpp:24 -- restore-point query failure falls back to VSS service state, falsely reporting System Protection enabled (safety check fail-open). Fix: fail closed.
- [ ] B5-11 [HIGH] src/core/smart_disk_analyzer.cpp:129,270 -- malformed/incomplete smartctl JSON -> default report assessed Healthy; :93 missing smartctl/zero disks -> empty report treated success. Fix: surface parse/absence as error.
- [ ] B5-12 [HIGH] src/core/hardware_inventory_scanner.cpp:98,204 -- WMI timeout/process/JSON failure returns empty data without errorOccurred; scan still succeeds. :635 EnumAdapters1 non-NOT_FOUND falls through -> null deref. Fix: emit error; guard adapter null.
- [ ] B5-13 [HIGH] src/core/stress_test_worker.cpp:184,354,454,801 -- CPU thread-count zero + memory/disk/GPU init failures silently skip components, still PASS. Fix: skipped component != PASS.
- [ ] B5-14 [HIGH] src/core/diagnostic_controller.cpp:105-180,293,339,622 -- CPU/disk/memory/hardware failures discarded, AllPassed without successful results; teardown ignores waits; cancelled SMART worker re-advances new step. Fix: require success; check waits; ignore stale completions.
- [ ] B5-15 [MED] src/core/input_validator.cpp:201,276 -- write validation checks only DOS read-only attr not ACL; allow_symlinks=false checks only final component (ancestor junction bypass). Fix: real writability + full-path reparse check.
- [ ] B5-16 [MED] src/core/diagnostic_report_generator.cpp:78 + diagnostic_controller.cpp:414 -- reports overwrite non-atomically (truncated on short write/full disk); second-resolution names collide; empty format list reports success with no file. Fix: atomic write; unique names; require output.
- Lower/quality (system): restore_point_manager.cpp:145 SilentlyContinue -> queryOk true; thermal_monitor.cpp:48 nonpositive interval; keep_awake.cpp global refcount vs per-thread API; registry_snapshot_engine 3-level cap; config_manager QSettings::status unchecked; logger.h source_location in template; input_validator empty-input Q_ASSERT crash; hardware_inventory DXGI/WMI index join; smart disk PhysicalDrive0-15 cap.
- CERT B5: ACL round-trip test (strip restores inherited, not Everyone); diagnostics fail-closed tests; suite green.

## BATCH 6 -- Actions fail-open cluster
- [ ] B6-01 [HIGH] src/actions/backup_bitlocker_keys_action.cpp:624 -- ACL-hardening failure leaves plaintext keys under inherited ACL, still success. Fix: fail on ACL failure.
- [ ] B6-02 [HIGH] backup_bitlocker_keys_action.cpp:118 -- Get/Set-Acl -Path (wildcard) not -LiteralPath ([ ] in path). Fix: -LiteralPath.
- [ ] B6-03 [HIGH] backup_bitlocker_keys_action.cpp:573 -- same-second dir + WriteOnly truncate overwrites prior backup. Fix: unique dir / no-truncate.
- [ ] B6-04 [HIGH] backup_bitlocker_keys_action.cpp:523 -- failed protector query -> empty list, keys omitted, success. Fix: surface failure.
- [ ] B6-05 [HIGH] backup_bitlocker_keys_action.cpp:552 -- gate counts any protector -> "Backed up 0 recovery keys" success. Fix: require recovery password.
- [ ] B6-06 [HIGH] backup_bitlocker_keys_action.cpp:720 -- QTextStream/close errors ignored -> truncated key files counted written. Fix: check stream status.
- [ ] B6-07 [HIGH] src/actions/verify_system_files_action.cpp:92 -- DISM "No component store corruption detected" contains "corruption" -> needless RestoreHealth on every clean machine. Fix: match exact corruption states.
- [ ] B6-08 [HIGH] verify_system_files_action.cpp:35 -- SFC fixed temp filename (concurrent overwrite/delete; reparse redirection). Fix: unique temp.
- [ ] B6-09 [HIGH] src/actions/check_disk_errors_action.cpp:108 -- "Check Disk Errors" schedules Repair-Volume -OfflineScanAndFix (mutation during a check). Fix: separate check from repair / consent.
- [ ] B6-10 [MED] check_disk_errors_action.cpp:280 -- one drive ok -> failed/timed-out drives don't affect success/failure count. Fix: aggregate all drives.
- [ ] B6-11 [HIGH] src/actions/reset_network_action.cpp:142 -- Winsock-backup failure only appended to errors, destructive reset proceeds. Fix: block on backup failure.
- [ ] B6-12 [HIGH] reset_network_action.cpp:279 -- Restart-NetAdapter no -ErrorAction Stop -> non-terminating errors still success. Fix: -ErrorAction Stop.
- [ ] B6-13 [HIGH] reset_network_action.cpp:334 -- verify timeout/start-fail/nonzero exit ignored in success. Fix: gate on verify.
- [ ] B6-14 [HIGH] reset_network_action.cpp:256 -- firewall reset with no export/backup of custom rules. Fix: export first.
- [ ] B6-15 [HIGH] src/actions/generate_system_report_action.cpp:257 -- collector start/nonzero-exit -> empty output, saving empty report still success. Fix: surface collector failure.
- [ ] B6-16 [MED] generate_system_report_action.cpp:229 -- BiosSeralNumber [VERIFY: real typo'd PS5.1 property; codex FALSE-POSITIVE -- keep as-is unless test says otherwise].
- [ ] B6-17 [MED] src/actions/screenshot_settings_action.cpp:280 -- report-write failure ignored, path always advertised, success on screenshots_taken>0. Fix: surface write failure.
- [ ] B6-18 [MED] src/actions/optimize_power_settings_action.cpp:306 -- custom plan containing "High Performance" -> skips activation. Fix: match plan GUID.
- [ ] B6-19 [HIGH] src/core/app_mutating_actions.cpp:164,3195 -- new/empty-dir + zip-output TOCTOU (WriteOnly clobbers raced-in file; archive engine removes on failure). Fix: exclusive create.
- [ ] B6-20 [MED] app_mutating_actions.cpp:2290 -- DHCP reset discards DNS-reset result but claims both reset. Fix: report actual.
- [ ] B6-21 [HIGH] src/threading/duplicate_finder_worker.cpp:573 + app_readonly_actions.cpp:1385 -- hash failures filtered then incomplete set reported success. Fix: surface hash failures.
- [ ] B6-22 [HIGH] src/core/advanced_search_worker.cpp:671,210 + app_readonly_actions.cpp:1247 -- unreadable/over-limit == "no matches" reported success (false negative); invalid exclude regex silently dropped (searches excluded paths). Fix: surface unreadable; surface bad regex.
- Related (drive_scanner, reported in system/core): drive_scanner.cpp:622 unmounted Windows partition = non-system; :478 writability error fails open writable; :315 storage-descriptor OOB read; :187 discards enumeration_ok -> spurious detach.
- CERT B6: fail-open unit tests (clean DISM, empty report, backup failure, bad regex); suite green.

## BATCH 7 -- Email subsystem
- [ ] B7-01 [CRIT] src/core/user_profile_backup_worker.cpp:49,99,109 + user_profile_restore_worker.cpp:100,141,151 -- QThread shadows isRunning() with late m_running; destroy/second-start before run() sets it -> destroy live thread / config race. Fix: use QThread state.
- [ ] B7-02 [CRIT] src/core/email_inspector_controller.cpp:179 + mbox_parser.cpp:317,681 -- concurrent MBOX attachment requests race shared QFile/offsets/m_attachment_sink -> UB / mixing. Fix: serialize / per-request state.
- [ ] B7-03 [CRIT] src/core/ost_converter_controller.cpp:125,139,144,328,337 -- ignores failed waits, deletes worker/thread possibly executing; terminate() on cancel. (Also B3-10.) Fix: safe stop.
- [ ] B7-04 [HIGH] src/core/email_profile_manager.cpp:375,680 -- restore blindly imports attacker-controlled .reg via reg.exe; no hive/value restriction -> arbitrary HKCU/HKLM. Fix: validate/confine reg content.
- [ ] B7-05 [HIGH] src/core/html_email_writer.cpp:212 -- saved HTML embeds untrusted email HTML verbatim (no sanitize/CSP) -> script/remote content on open. Fix: sanitize + CSP.
- [ ] B7-06 [HIGH] src/core/pdf_email_writer.cpp:103,153 -- untrusted HTML to QTextDocument; absolute local refs disclose local files into PDF. Fix: strip/deny local resource loads.
- [ ] B7-07 [HIGH] src/core/email_profile_manager.cpp:47,393 -- destination confinement lexical only; junction under home redirects QFile::copy outside home. Fix: resolve real path.
- [ ] B7-08 [HIGH] src/core/user_profile_backup_worker.cpp:188,246,324 -- unvalidated username+relative_path; `..` reads/writes outside roots; file symlinks copied without reparse check. Fix: validate/confine + reparse check.
- [ ] B7-09 [HIGH] src/core/user_data_manager.cpp:293,668,737 -- overwrite_existing ignored: ZIP restore always -Force, dir restore never overwrites. Fix: honor flag both paths.
- [ ] B7-10 [HIGH] src/core/user_profile_restore_worker.cpp:72,87 -- "atomic" replace removes original before rename; rename failure loses destination. Fix: rename-then-remove.
- [ ] B7-11 [HIGH] src/core/user_data_manager.cpp:617,648,668 -- encrypted restore decrypts to extensionless temp then ZIP-only Expand-Archive; encrypted backups cannot restore. Fix: restore correct format.
- [ ] B7-12 [HIGH] src/core/pst_writer.cpp:134 + msg_writer.cpp:82 + dbx_writer.cpp:94 + pst_splitter.cpp:36 -- PST/MSG/DBX writers always not_implemented; PST is DEFAULT OST-conversion format. Fix: implement or block+relabel default.
- [ ] B7-13 [HIGH] src/core/user_profile_types.cpp:267,314 + user_profile_restore_worker.cpp:578,697 -- manifest/per-user checksums serialized but never computed/verified; restore checks only manifest existence; "verify" checks only readability. Fix: compute+verify checksums.
- [ ] B7-14 [HIGH] src/core/user_profile_types.cpp:104,120,296,347 -- missing/malformed filter JSON clears mandatory dangerous-file defaults; empty-object Q_ASSERT abort in debug. Fix: keep defaults; no assert on empty.
- [ ] B7-15 [HIGH] src/core/email_inspector_controller.cpp:116 -- public closeFile() can close/clear parser while QtConcurrent tasks still use it -> UAF. Fix: guard/await tasks.
- [ ] B7-16 [HIGH] src/core/email_inspector_controller.cpp:250 + email_profile_manager.cpp:206,227,332 -- discovery/backup/restore no state guards, run concurrently on shared vectors/maps, reset each other's cancel. Fix: single-flight guard.
- [ ] B7-17 [HIGH] src/core/conversion_report_generator.cpp:54,73,125 -- CSV export no formula-injection protection; email cells execute in Excel/Calc; headers unescaped. Fix: neutralize +-=@ and escape.
- [ ] B7-18 [HIGH] src/core/email_inspector_controller.cpp:25 + html_email_writer.cpp:111 + mbox_writer.cpp:70 + email_export_worker.cpp:916,963 + email_profile_manager.cpp:748 -- short writes accepted as success -> truncated reports/messages/attachments. Fix: verify bytes written.
- [ ] B7-19 [HIGH] src/core/html_email_writer.cpp:119,131 -- attachment names sanitized but not deduped; colliding names truncate earlier files. Fix: dedupe.
- [ ] B7-20 [HIGH] src/core/user_profile_backup_worker.cpp:188,225,297,219 -- backup reports success after missing folders / dir failures / elevation skips (don't increment m_filesErrored). Fix: count errors.
- [ ] B7-21 [HIGH] src/core/user_profile_restore_worker.cpp:444,543 -- AssignToDestination never receives destination username -> always strips permissions. Fix: pass username.
- [ ] B7-22 [HIGH] include/sak/migration_report.h:109 -- public getters use unchecked vector operator[] -> OOB on bad index. Fix: bounds-check.
- [ ] B7-23 [MED] email_inspector_controller.cpp:206,229 -- search/export with no open file sets busy but launches nothing -> stuck. Fix: reset state.
- [ ] B7-24 [MED] email_inspector_controller.cpp:339 + mbox_parser.cpp:140,173 -- cancelling MBOX parser leaves cancel flag set; later reads fail until reopen. Fix: reset flag.
- [ ] B7-25 [MED] email_export_worker.cpp:532,1200 -- unreadable embedded attachments add error but message counts fully exported. Fix: mark partial.
- [ ] B7-26 [MED] email_export_worker.cpp:1058,1315 -- attachment-only PST export passes vector position not att.index -> wrong payload. Fix: use att.index.
- [ ] B7-27 [MED] email_export_worker.cpp:196,236 -- folder/mailbox paging silently stops on read error, exports partial without recording failure. Fix: record failure.
- [ ] B7-28 [MED] mbox_parser.cpp:178 + email_search_worker.cpp:179,192 -- body search uses result position not msg.message_index; one skip shifts later lookups. Fix: use message_index.
- [ ] B7-29 [MED] email_profile_manager.cpp:353,365 -- restore increments "profiles restored" regardless of rejected/missing/registry/copy failures. Fix: count only success.
- [ ] B7-30 [MED] user_data_manager.cpp:445,485 -- verifyBackup true for checksum-less metadata even if payload missing; compareChecksums true when both reads fail. Fix: require payload+hash.
- [ ] B7-31 [MED] user_data_manager.cpp:427 -- deleteBackup uses QFile::remove for directory payloads -> uncompressed backups undeletable. Fix: remove recursively.
- [ ] B7-32 [MED] mbox_writer.cpp:95,120 -- existing outputs opened append -> rerun duplicates/merges old mail. Fix: truncate/new.
- [ ] B7-33 [MED] mbox_parser.cpp:438,535,648 -- any delimiter-prefixed line treated MIME boundary; non-multipart root attachment treated body -> corrupts valid messages. Fix: proper boundary parse.
- [ ] B7-34 [MED] Remaining email medium/low: user_profile_backup_worker static progress counters race (:521); is_selected omitted from JSON (:108); migration import mutates before validate (:328,346); PromptUser never prompts (:463,522); compression level stored-not-used (:85,454); email report totals never populated (:293,287); email_report CSV path unescaped (:347); advanced MAPI/attachment-name search never evaluated (:287,369); backup filter always m_users[0] (:287,355); HTML From double-escape (:162); conversion title `???` placeholder (:275); inline-image uses filename not Content-ID (:214). Fix: per-item.
- CERT B7: thread start/destroy stress; malicious .reg/junction/`..` path tests; short-write tests; hostile HTML/CSV fixtures; suite green.

## BATCH 8 -- Files subsystem
- [ ] B8-01 [HIGH] src/core/file_management_file_system.cpp:1451 -- local recursive delete no empty/root/containment guard; empty path -> CWD. Fix: guard.
- [ ] B8-02 [HIGH] file_management_file_system.cpp:1139 -- raw entry names joined onto host dest; `..`/separator escape export dir. Fix: basename confinement.
- [ ] B8-03 [HIGH] file_management_file_system.cpp:267 -- non-local HFS/APFS ignore inbound read_only when computing can_write_files -> write-protected raw targets writable. Fix: honor read_only.
- [ ] B8-04 [HIGH] file_management_file_system.cpp:169 -- APFS/HFS option builders hard-code writer enable/confirmation/evidence -> bridge can't fail closed absent evidence. Fix: require evidence.
- [ ] B8-05 [HIGH] file_management_file_system.cpp:1760 -- replace deletes sole case-insensitive match on case-sensitive HFSX/APFS -> replacing foo deletes Foo. Fix: case-correct match.
- [ ] B8-06 [HIGH] src/core/file_explorer_transfer_worker.cpp:46 -- replace deletes existing dest before validating/copying source; failure loses original. Fix: copy-then-replace.
- [ ] B8-07 [HIGH] src/core/file_explorer_archive_service.cpp:294 -- compression truncates existing archive + removes on failure -> destroys prior archive. Fix: temp-then-rename.
- [ ] B8-08 [HIGH] file_explorer_archive_service.cpp:145 -- extraction silently replaces existing files, no rollback on later failure. Fix: collision policy + rollback.
- [ ] B8-09 [HIGH] file_explorer_archive_service.cpp:346,422 -- materializes whole ZIP central dir before entry cap; smart-root repeats unbounded alloc. Fix: cap before materialize.
- [ ] B8-10 [HIGH] file_explorer_archive_service.cpp:136 -- each ZIP entry decompressed into one QByteArray; 4-GiB per-file limit permits exhaustion. Fix: stream + lower cap.
- [ ] B8-11 [HIGH] src/core/xz_decompressor.cpp:20 -- lzma_auto_decoder given UINT64_MAX memlimit -> crafted header unbounded memory. Fix: real memlimit.
- [ ] B8-12 [HIGH] src/core/streaming_decompressor.cpp:108 -- first stream-end treated final EOF; drops concatenated members, accepts trailing garbage. Fix: continue/verify.
- [ ] B8-13 [HIGH] src/core/file_recovery_engine.cpp:344 -- recovered files use truncating QFile, ignore short-write/close, report success after disk-full/partial. Fix: verify writes.
- [ ] B8-14 [HIGH] file_recovery_engine.cpp:420 -- candidate scan O(scan x max_candidate) on repeated unterminated signatures -> crafted-image CPU DoS. Fix: bound.
- [ ] B8-15 [MED] file_recovery_engine.cpp:169 -- candidate id joined without basename validation -> `../` traversal outside restore dir. Fix: basename.
- [ ] B8-16 [HIGH] src/core/file_scanner.cpp:338 -- default scan blocks only is_symlink() dirs; Windows junctions followed, symlink files target-stat'd -> root escape + UNC cred leak. Fix: reparse-aware.
- [ ] B8-17 [HIGH] file_scanner.cpp:49 -- concurrent scans on header-documented thread-safe instance race m_visited_dirs/counters -> UB. Fix: sync or document not-thread-safe.
- [ ] B8-18 [MED] file_management_file_system.cpp:930 -- max_bytes+1 wraps to zero for UINT64_MAX -> disables raw-file read cap. Fix: guard wrap.
- [ ] B8-19 [MED] file_management_file_system.cpp:1186 -- dir export/import ok=true despite depth/entry/symlink/capped-file omissions. Fix: surface omissions.
- [ ] B8-20 [MED] file_management_file_system.cpp:312 -- listing omits QDir::Hidden/System (show-hidden broken); maxEntries applied after full materialize. Fix: include flags; cap early.
- [ ] B8-21 [MED] Archive/service correctness: file_explorer_archive_service.cpp:335 (readability-only -> empty list ok=true), :223 (entry cap counts only files, dirs bypass), :311 (missing sources -> warnings, partial ok); file_explorer_archive_worker.cpp:46 (partial subset archived+completed), :249 (collision check discards listing failure), :194 (exhausted name -> childPath("")). Fix: per-item.
- [ ] B8-22 [MED] Model/UI correctness: file_explorer_group_proxy_model.cpp:90 (dataChanged no regroup), :39 (nested resets); file_explorer_item_model.cpp:335 (sort permutes without remapping persistent indexes); file_explorer_status_center.cpp:301 (terminal progress leaves stale state); file_explorer_command_registry.cpp:253 (read-gating omits compress/extract/flatten). Fix: per-item.
- [ ] B8-23 [MED] Hash/recovery correctness: file_hash.cpp:46 (zero chunk -> empty digest release), :105 (negative char to ::tolower UB); file_recovery_engine.cpp:482 (prefix compare claims whole-source unchanged), :389 (short read accepted complete); deleted_item_scanner.cpp:107 (per-item read fail not clearing reliable); decompressor_factory.cpp:78 (ZIP advertised, create() null); smart_file_filter.cpp:61 (invalid exclusion fail-open); recycle_bin.cpp:22 (empty/NUL/wildcard to SHFileOperationW). Fix: per-item.
- [ ] B8-24 [LOW] file_explorer_transfer_worker.cpp:282 (dir progress cannot reach total); grouping.cpp:34 (decimal thresholds labeled GiB/MiB); properties_calc.cpp:33 (exact-cap dir always incomplete); session_store.cpp:43 (unranged enum cast); include/sak/file_hash.h:149 (hashToHex declared no definition -> link fail for callers). Fix: per-item.
- CERT B8: zip-bomb + traversal + symlink/junction + recursive-delete-guard fixtures; suite green.

## BATCH 9 -- Network subsystem
- [ ] B9-01 [CRIT->DEAD] src/core/imap_uploader.cpp:510,201 -- mismatched message/flag/date vectors rely on Q_ASSERT; release OOB at(). NOTE: ImapUploader has NO caller (dead code) -- latent until OST->IMAP wired. Fix: bounds-check (still fix) or gate behind wiring.
- [ ] B9-02 [CRIT->DEAD] imap_uploader.cpp:157,217,312 -- CREATE/APPEND reject + oversized skip increment failures but session finishes success (silent partial/zero migration). Fix: fail session.
- [ ] B9-03 [HIGH->DEAD] imap_uploader.cpp:201 -- caller/PST flags joined into APPEND without token validation -> CRLF/paren injection. Fix: validate tokens.
- [ ] B9-04 [HIGH->DEAD] imap_uploader.cpp:238,432 -- use_ssl=false sends creds+bodies plaintext, no STARTTLS. Fix: enforce TLS.
- [ ] B9-05 [HIGH->DEAD] imap_uploader.cpp:334,362 -- parser accepts tags/`+` anywhere not line prefix -> false success/premature literal. Fix: line-prefix parse.
- [ ] B9-06 [HIGH] src/core/network_share_browser.cpp:225 -- write probe uses 8 UUID hex, opens without NewOnly; collision/race truncates+deletes existing remote file. Fix: full UUID + NewOnly.
- [ ] B9-07 [HIGH] src/core/ethernet_config_manager.cpp:179,226,289 -- DHCP restore enables DHCP DNS then replaces captured servers with static; DNS-reset failure ignored. Fix: correct order + honor failure.
- [ ] B9-08 [HIGH] src/core/active_connections_monitor.cpp:109 -- TCP/UDP table-read failure -> empty snapshot, emits every prior connection closed, destroys baseline. Fix: preserve on error.
- [ ] B9-09 [HIGH] src/core/firewall_rule_auditor.cpp:287,416 -- COM failure emits errorOccurred but public ops still emit empty rulesEnumerated/auditComplete -> failed audit looks clean. Fix: don't emit clean on failure.
- [ ] B9-10 [HIGH] firewall_rule_auditor.cpp:140,218 -- per-rule COM getter failure -> defaults, extraction still succeeds, hides rules. Fix: surface per-rule failure.
- [ ] B9-11 [HIGH] firewall_rule_auditor.cpp:485,628,675 -- conflict logic ignores addresses/remote ports/service/selectors; SMB-gap misses wildcard/empty-port; unknown expr fails open no-overlap. Fix: full selector compare.
- [ ] B9-12 [HIGH] src/core/network_diagnostic_report_generator.cpp:91,105 + ethernet_config_manager.cpp:120 -- HTML report closes QFile before flush, never checks stream, always emits reportGenerated; writers truncate non-atomically. Fix: flush+check, atomic.
- [ ] B9-13 [HIGH] src/core/wifi_analyzer.cpp:191 -- auth/encryption matched to every BSSID sharing SSID -> evil-twin mislabeled secure. Fix: per-BSSID.
- [ ] B9-14 [HIGH] wifi_analyzer.cpp:251 -- OUI DB unsynchronized mutable statics; concurrent scans race loaded/QHash -> UB. Fix: sync/once.
- [ ] B9-15 [HIGH] src/core/wifi_setup_script.cpp:114 -- plaintext password XML to predictable %TEMP%\wifi_profile_sak.xml; race + creds left after failure. Fix: unique temp, wipe.
- [ ] B9-16 [HIGH] src/core/bandwidth_tester.cpp:243,505 -- iPerf firewall add/remove async under one global rule name; races leave rule open / delete other instance's rule. Fix: unique rule per instance.
- [ ] B9-17 [HIGH] src/core/connectivity_tester.cpp:282,353,480 -- configs lack bounds: huge packet/max-hop allocate attacker-sized; negative delay/timeout cast multi-day; TTL/payload truncate. Fix: validate bounds.
- [ ] B9-18 [MED] Network mediums: network_share_browser.cpp:163 (ERROR_MORE_DATA treated complete, truncates); network_diagnostic_report_generator.cpp:256 (DNS type unescaped HTML injection); dns_diagnostic_tool.cpp:353 (flush emits success after failure), :70 (unknown type silently A query), :232 (reverse-lookup accepts nonnumeric octets), :264 (ordered answer compare); active_connections_monitor.cpp:323 (empty snapshot suppresses newConnection); connectivity_tester.cpp:365 (cancelled ping keeps configured sent count); bandwidth_tester.cpp:402 (valid exit + bad JSON -> all-zero success), :164 (HTTP latency returns time on HEAD fail); wifi_analyzer.cpp:223 (WlanScan result ignored, fixed sleep -> stale), :397 (continuous scan overwrites on failure); wifi_profile_scanner.cpp:76,110 (English-text-dependent, per-profile failure still success); wifi_setup_script.cpp:30,182 (unknown modes -> WPA2-PSK, SSID byte-length not enforced); port_scanner.cpp:131,377 (closed-port depends on English "refused"). Fix: per-item.
- [ ] B9-19 [MED] Network missing: network_transfer_runner.cpp:117 (buffers whole response, no byte limit -> remote memory exhaustion); port_scanner.h:32 + port_scanner.cpp:341 (maxConcurrent unimplemented, serial); ethernet_config_manager.cpp:102 (MAC command output discarded). Fix: per-item.
- CERT B9: firewall-fail-closed + wifi per-BSSID + bounds + injection tests; suite green.

## BATCH 10 -- Apps / supply-chain
- [ ] B10-01 [CRIT] src/core/windows_usb_creator.cpp:103 -- (see B2-04).
- [ ] B10-02 [CRIT] src/core/flash_coordinator.cpp:384 -- (see B2-05).
- [ ] B10-03 [CRIT] src/core/leftover_scanner.cpp:891 -- (see B2-06).
- [ ] B10-04 [CRIT] src/core/advanced_uninstall_controller.cpp:215 -- (see B2-07).
- [ ] B10-05 [CRIT] src/core/flash_coordinator.cpp:515 -- (see B4-08).
- [ ] B10-06 [HIGH] flash_coordinator.cpp:168 -- duplicate target paths not rejected (concurrent same-disk write). Fix: reject dup.
- [ ] B10-07 [HIGH] flash_coordinator.cpp:285 -- signals emitted while m_mutex held; direct slots calling locked getters deadlock. Fix: emit unlocked.
- [ ] B10-08 [HIGH] src/core/uup_dump_api.cpp:109 + linux_distro_catalog.cpp:648 -- abort() synchronously removes from list being range-iterated -> iterator invalidation. Fix: snapshot before abort.
- [ ] B10-09 [HIGH] src/core/advanced_uninstall_controller.cpp:544,447 -- enum shutdown force-terminates, ignores second wait, permits child QThread destroy while running; uninstall completion Idle before thread exits -> next op destroys running worker. Fix: check waits, delay Idle.
- [ ] B10-10 [HIGH] src/core/package_internalization_engine.cpp:599 -- (see B1-07).
- [ ] B10-11 [HIGH] package_internalization_engine.cpp:187 -- unvalidated package IDs/versions form extraction/output paths -> traversal/overwrite/recursive cleanup outside work dir. Fix: validate IDs.
- [ ] B10-12 [HIGH] src/core/offline_deployment_worker.cpp:526 -- traversal-bearing package ID escapes _sak_temp_<id> then removeRecursively deletes escaped target. Fix: validate ID.
- [ ] B10-13 [HIGH] offline_deployment_worker.cpp:140 -- existing <output>/_work reused + recursively deleted without ownership marker. Fix: marker/confirm.
- [ ] B10-14 [HIGH] offline_deployment_worker.cpp:690 -- decoded URL filenames appended without basename/confinement -> encoded traversal outside output_dir. Fix: basename.
- [ ] B10-15 [HIGH] src/core/nuget_api_client.cpp:342 -- fallback filename uses unsanitized package_id -> traversal outside output_dir. Fix: sanitize.
- [ ] B10-16 [HIGH] src/core/script_rewriter.cpp:123 -- replacing only URL text leaves surrounding quotes -> invalid/inert PowerShell. Fix: replace full token.
- [ ] B10-17 [HIGH] package_internalization_engine.cpp:473 -- parsed installer checksums never verified before repack. Fix: verify checksum.
- [ ] B10-18 [HIGH] offline_deployment_worker.cpp:393 -- bundle install ignores manifest filename/checksum/size/internalized fields; invokes bare `choco` (PATH hijack). Fix: verify manifest + use bundled exe.
- [ ] B10-19 [HIGH] src/core/linux_iso_downloader.cpp:284,517 -- SourceForge may downgrade to HTTP + checksum-less entries reported complete; cancellation doesn't abort checksum/hash, new download replaces shared state. Fix: enforce HTTPS+checksum; abort verify.
- [ ] B10-20 [HIGH] src/core/uup_dump_api.cpp:421,384 -- missing/malformed SHA-1 accepted even for HTTP; builder omits integrity verify; invalid entries dropped, partial set emitted success. Fix: require checksum; surface drops.
- [ ] B10-21 [HIGH] src/core/uup_iso_builder.cpp:686,779,805 + uup_iso_builder.h:37 -- aria2 exit 7 proceeds verifying only one file; existing ISO deleted before conversion; converter errorOccurred emits no terminal failure (stuck forever); any-thread contract false. Fix: verify all files; convert-then-replace; handle errorOccurred; marshal.
- [ ] B10-22 [HIGH] src/core/windows_usb_creator_extract.cpp:561 -- missing/failed/timed-out bcdboot treated success; BIOS-only + file-presence checks falsely certify NTFS media UEFI-bootable. Fix: gate on bcdboot.
- [ ] B10-23 [HIGH] src/core/cleanup_worker.cpp:117 -- recycle-bin mode silently falls back to permanent deletion when recycling fails. Fix: surface fallback.
- [ ] B10-24 [HIGH] src/core/vulnerability_scanner.cpp:132,148 -- malformed JSON parses as empty catalog no error (false clean scan); NVD/CISA matching discards version ranges, never compares installed versions. Fix: surface parse error; compare versions.
- [ ] B10-25 [HIGH] src/core/nuget_api_client.cpp:427 -- dependency ranges discarded, non-root deps resolve to latest -> incompatible offline bundles. Fix: honor ranges.
- [ ] B10-26 [MED] app_installation_worker.cpp:102,438 -- maxConcurrent=0 permanent polling, >1 still serial; verification returns success when neither choco nor registry/AppX confirm install. Fix: bound; require confirmation.
- [ ] B10-27 [MED] Data-corruption writers: offline_deployment_worker.cpp:814 (manifest truncate-before-write), script_rewriter.cpp:79 (in-place truncate, no stream check), package_matcher.cpp:724 (mapping export truncate non-atomic). Fix: atomic writes.
- [ ] B10-28 [MED] nuget_api_client.cpp:169,190,604 -- concurrent dep-resolution shares/clears one graph state (mixing); cancel() permanent aborts nothing; framework-qualified parsing records framework token not dep ID. Fix: per-request state; abort replies; fix parse.
- [ ] B10-29 [MED] package_internalization_engine.cpp:129 -- no recognized URLs treated successful internalization. Fix: surface.
- [ ] B10-30 [MED] cleanup_worker.cpp:45 + advanced_uninstall_controller.cpp:267,142 + vulnerability_scanner.cpp:1547,502 -- cancellation reported success; enum cancel Idle-before-stop (stale completes new); autoCleanSafe stored-never-applied; cancel returns full count no cancelled flag; stale cache -> successful transfer no metadata. Fix: per-item.
- [ ] B10-31 [MED] Enumeration/parse: program_enumerator.cpp:125 (empty enum hits non-empty assert -> debug abort); app_scanner.cpp:97 (subkey > buffer stops enumeration silently); iso_analyzer.cpp:518 (El Torito only LBA 17, bootable before validating catalog); windows_user_scanner.cpp:343 (per-folder sizing usually reports zero); image_source.cpp:323 (CompressedImageSource::size returns uninitialized); leftover_scanner.cpp:556 (English-label + naive comma split); linux_distro_catalog.cpp:631 (new release keeps prev checksum URL); chocolatey_manager.cpp:521 (validation permits leading `-` option injection; rejects valid SemVer). Fix: per-item.
- [ ] B10-32 [LOW] Quality/dead: uup_iso_builder.h:225 (m_workerThread unused); flash_coordinator.cpp:234 (setBufferCount stored-not-used); app_scanner.h:129 (parseAppXPackages declared-never-defined); package_matcher.h:64 (MatchConfig::use_cache ignored). Fix: implement or remove.
- CERT B10: supply-chain tests (checksum-required, PATH-hijack blocked, package-ID traversal rejected, bcdboot gate, iterator-invalidation); suite green.

## BATCH 11 -- Partition APFS/HFS write-safety
- [ ] B11-01 [HIGH] src/core/partition_file_system_detector.cpp:2103,2123 + partition_apfs_file_system_reader.cpp:602 + partition_apfs_writer.cpp:4839 -- APFS geometry never reconciled with validated size; commit guard max(claimed,device) WIDENS writable range on oversized claim. Fix: min + reconcile.
- [ ] B11-02 [HIGH] partition_apfs_writer.cpp:5424,5627,5647,5663 -- checkpoint-map entry count consumed + COW-written before capacity check; cpm_size+blockSize-1 overflow. Fix: bound before write.
- [ ] B11-03 [HIGH] partition_apfs_writer.cpp:20924,20927,20932 -- raw format zeroes target before constructing all blocks, proceeds even with blockers, ignores writeImageBlocks failure. Fix: construct-then-write, honor blockers/failure.
- [ ] B11-04 [HIGH] partition_apfs_writer.cpp:4699,4708,20145 -- format writes lack target block-count/byte-range; blockIndex*blockSize can wrap past container. Fix: pass+enforce range bound.
- [ ] B11-05 [HIGH] partition_apfs_writer.cpp:743,6535,7206,8773 -- boundedNodeKeyCount hardening incomplete; extent-ref/fs-tree/free-queue/checkpoint walkers still trust raw counts -> runaway loops. Fix: bound all walkers.
- [ ] B11-06 [HIGH] include/sak/partition_hfs_internal.h:8180,8187,8195 -- HFS+ alternate-volume-header sync failure warning-only; caller sets ok=true -> stale backup header. Fix: fail on sync error.
- [ ] B11-07 [HIGH] src/core/partition_raw_device_io.cpp:255,270 -- unaligned raw write clamps scratch to DWORD_MAX but copies original maxSize+prefix -> heap OOB (needs >4GiB unaligned; latent). Fix: clamp copy length.
- [ ] B11-08 [HIGH] partition_raw_device_io.cpp:461,478 -- POSIX sparse copy treats every SEEK_DATA failure as EOD; EINVAL/EIO -> success with hole-filled dest (POSIX-only). Fix: distinguish errno.
- [ ] B11-09 [HIGH] src/core/partition_manager_controller.cpp:47 + partition_file_system_tool_runner.cpp:374 -- missing app-dir manifest -> CWD manifest becomes root of trust (self-hashed). Fix: require app-dir manifest.
- [ ] B11-10 [HIGH] src/core/partition_executor.cpp:152,163,231 -- execute() clears pre-dispatch cancel; cancel between loop check and setActiveBroker finds null broker -> destructive command still launches. Fix: honor pre-dispatch cancel.
- [ ] B11-11 [MED] src/core/apfs_keybag.cpp:62,91,151 + apfs_crypto.cpp:269 -- DER long-form len signed shift/add overflow before bounds check; PBKDF2 iterations unbounded from DER -> BCrypt hang; parseKeyBlob null-deref + stale wrappedKey; outer HMAC never verified. Fix: bound iterations, guard null, verify HMAC. (PARTIAL: some already guarded.)
- [ ] B11-12 [MED] src/core/encryption.cpp:75,194,306,400 -- EncryptionParams unvalidated (1 iter/short key/negative->huge); empty-plaintext round-trip broken. Fix: validate params; handle empty.
- [ ] B11-13 [MED] src/core/partition_manager_controller.cpp:263,355 -- applyIsRunning() uses isFinished() (flips before queued slot); double-apply -> old callback resets new executor. Fix: explicit running flag.
- [ ] B11-14 [MED] CLI DoS/secret: sak_hfs_writer_cli.cpp:208,581 (--name-pad unbounded QString), apfs:93/hfs:45 (readAll before 64MiB cap), sak_apfs_writer_cli.cpp:314,471 (invalid xattr/sparse silently mangled), :400,1875 (password on cmdline, unwiped QString), :1567 (raw resize same value, no-op). Fix: bound; validate; secure secret.
- Lower: partition_file_system_tool_runner.cpp:156 (fsck `-` target no `--`); partition_ext_file_system_reader.cpp:243,345 (trim removes valid leading/trailing space; duplicated warnings); secure_memory.h:121 (secure_buffer<T> non-trivial UB, only trivial instantiated); payloadUInt64 unchecked double->uint64 cast (script_builder:137, safety_validator:238); HFS attribute predicate returns true on scan-fail (:9563,9580).
- CERT B11: APFS/HFS range-bound + walker-bound + DER + cancellation-window regression tests; Mac-cert unaffected; suite green.

## BATCH 12 -- AI harness
- [ ] B12-01 [CRIT] src/ai/ai_orchestrator.cpp:511,642 + ai_orchestrator.h:38 -- workflows have no session-policy ceiling; catalog agent policy passed verbatim -> Chat/Research workflow gains package/mutating capability. Fix: clamp to session ceiling.
- [ ] B12-02 [HIGH] src/ai/ai_tool_policy.cpp:216,368 -- ReadOnlyPc fail-open blacklist misses redirection/reg add/sc stop/taskkill/shutdown/schtasks -> execute without lease/confirmation. Fix: allowlist or complete denylist.
- [ ] B12-03 [HIGH] src/ai/ai_workflow_placeholders.cpp:81 -- (see B1-05).
- [ ] B12-04 [HIGH] include/sak/ai/ai_lease_manager.h:26 -- lease TTL 3600s < 7200s max op -> second mutating op reclaims live lease -> concurrent mutation. Fix: TTL >= max op.
- [ ] B12-05 [MED] src/ai/ai_provider_gateway.cpp:693,695,705 -- WIN32_MCP_SECURITY_PROFILE + REDACT_SENSITIVE_OUTPUT set but never read by server -> full catalog advertised, raw sensitive output reaches model. Fix: consume env in server.
- [ ] B12-06 [MED] src/ai/ai_subagent_runner.cpp:424,520 + ai_subagent_runner.h:35 -- per-task timeout_seconds parsed never enforced; wall-clock can't interrupt sync call. Fix: enforce timeout.
- [ ] B12-07 [LOW] src/ai/ai_openai_model_client.cpp:162 + ai_mcp_http_client.cpp:214 + ai_mcp_stdio_client.cpp:295 -- unconditional semaphore acquire after QThread::start(); thread-create failure hangs forever. Fix: timed acquire / check start.
- [ ] B12-08 [LOW] src/ai/ai_provider_gateway.cpp:618,640 -- list_processes/kill_process/start_process classified but absent from manifest (dead). Fix: remove.
- CERT B12: policy-ceiling + lease-TTL + placeholder-injection tests; suite green.

## BATCH 13 -- win32 / browser control
- [ ] B13-01 [HIGH] src/win32mcp/win32_mcp_capture.cpp:107 + win32_mcp_ocr.cpp:228 + win32_mcp_geometry.h:12 -- ocr_region allocates full 16384x16384x4 (~1GiB) 32bpp surface before downscale. Fix: cap capture dims pre-alloc.
- [ ] B13-02 [MED] src/ai/ai_mcp_http_client.cpp:144,167 -- HTTP MCP replies no streaming cap; 1MiB limit applied only after full buffer. Fix: setReadBufferSize + incremental abort.
- [ ] B13-03 [MED] src/win32mcp/browser_extension_installer.cpp:486,489,499,509 -- uninstall ignores RegDelete/QFile::remove failures, always ok=true. Fix: surface failures.
- [ ] B13-04 [MED] src/win32mcp/browser_bridge.cpp:138 + browser_bridge_relay.h:37 + browser_control.cpp:38 -- onDetached() has no production caller; refs stay live after external navigation/tab-close/CDP-detach. Fix: wire detach event.
- [ ] B13-05 [MED] src/win32mcp/browser_bridge_pipe.cpp:219,267,276 -- 2nd start() move-assigns joinable std::thread -> std::terminate; stop() under once_flag disables later teardown. Fix: guard restart.
- [ ] B13-06 [MED] src/win32mcp/win32_mcp_entry.cpp:53 -- any chrome-extension:// arg selects relay mode; origin never validated vs pinned ID. Fix: validate origin.
- [ ] B13-07 [LOW] src/win32mcp/win32_mcp_ocr.cpp:220,228 -- signed x+width / y+height overflow UB before validation. Fix: checked arithmetic.
- [ ] B13-08 [LOW] src/win32mcp/browser_bridge_relay.cpp:221 + browser_bridge_relay.h:64 -- relay handshake accepts any welcome, never checks protocol value. Fix: verify protocol.
- [ ] B13-09 [LOW] src/ai/ai_mcp_http_client.cpp -- (buffer cap, see B13-02).
- [ ] B13-10 [LOW] Semaphore-hang (see B12-07) shared with MCP clients. Fix: with B12-07.
- CERT B13: capture-cap + uninstall-failure + relay-protocol tests; suite green.

---

## FALSE-POSITIVES (verified codex-wrong; do NOT fix)
- pst_parser.cpp:1454 visited-set "by value" -- member is a REFERENCE (pst_parser.h:265); fan-out detection intact.
- pst_parser.cpp:1005 negative attachment index OOB -- dominated by readAttachmentData success gate; callers pass >=0.
- generate_system_report_action.cpp:229 BiosSeralNumber -- real (typo'd) PS 5.1 Get-ComputerInfo property; correct spelling returns empty. Correct as written.

## PARTIAL / ACCEPTED (documented tradeoffs -- confirm no change needed during batch)
- partition clone zero-offset overwrite gated by target_wipe_confirmed + Assert-SakRawWriteTarget (still hardening via B2-03).
- VSS-snapshot fallback to live copy: by-design, manifest-verified (still B2-14 for consent).
- quick_action_controller.cpp:251 require_confirmation delegated to GUI (still tighten B3-08).
- partition_apfs_writer raw-format surfaces failure (ok=blockers.isEmpty).
- LPE elevated pipe = defense-in-depth only, not exploitable (still B5-04).
- ImapUploader = dead code, unwired (B9-01..05 still hardened before wiring).
- disk-selection GUI-gated by isSystemDrive (engine still single-layer -> B2-04/05).
