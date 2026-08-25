# Line-Coverage Baseline (R5-G14-16)

CODEX_REVIEW_5 MEASURED GAP 3 was that the suite had no coverage measurement of any
kind, so "N tests pass" carried an unknown denominator. This is the first real
measurement, produced with OpenCppCoverage 0.9.9.0 over a RelWithDebInfo build.

Reproduce it with:

```
cmake --build build --config RelWithDebInfo --target <core test targets>
scripts/run_coverage.ps1 -TestRegex '<regex>' -OutDir build/coverage
```

The script needs OpenCppCoverage (choco install opencppcoverage) and a RelWithDebInfo
build, because the default Release build strips the PDBs coverage maps against. It is
NOT a pre-commit hook: an instrumented run is far too slow for every commit. The CI
workflow carries an opt-in "coverage" job (workflow_dispatch) that installs the tool,
builds the core tests, runs this script, and uploads the HTML report.

## Scope of this baseline

Measured over a CURATED CORE SET of tests, not the full 235-test suite: the parsers and
the security boundary (PST, MBOX, ISO, the AI tool policy, the MIME/HTML/framing seams,
encryption, input validation, path utilities, crash reporter). The per-subsystem
aggregate below is therefore pessimistic for src/core: these exes statically link many
core files (via logger, error_codes, email_types) that this set does not exercise, and
every such line counts against the total. The per-file table is the sharper signal.

Measured 2026-08-12. Exclusion inventory (R5-G14-16c) added 2026-08-17.

## Per subsystem (curated core set)

| Subsystem      | Covered / Total | Line rate |
|----------------|-----------------|-----------|
| src\ai         | 624 / 702       | 88.89%    |
| include\sak    | 448 / 515       | 86.99%    |
| src\win32mcp   | 34 / 34         | 100.00%   |
| src\core       | 2657 / 6536     | 40.65%    |
| TOTAL measured | 3763 / 7787     | 48.32%    |

## Per file (the directly-tested files)

| File                      | Covered / Total | Rate   |
|---------------------------|-----------------|--------|
| email_html_sanitizer.h    | 19 / 19         | 100.0% |
| mbox_header_parser.h      | 32 / 32         | 100.0% |
| mbox_transfer_decoder.h   | 34 / 34         | 100.0% |
| native_messaging.cpp      | 34 / 34         | 100.0% |
| mbox_parser.cpp           | 387 / 431       | 89.8%  |
| ai_mcp_jsonrpc.h          | 25 / 28         | 89.3%  |
| ai_tool_policy.cpp        | 312 / 351       | 88.9%  |
| iso_analyzer.cpp          | 209 / 257       | 81.3%  |
| input_validator.cpp       | 262 / 337       | 77.7%  |
| encryption.cpp            | 288 / 385       | 74.8%  |
| path_utils.cpp            | 107 / 157       | 68.2%  |
| pst_parser.cpp            | 470 / 1534      | 30.6%  |
| crash_reporter.cpp        | 15 / 60         | 25.0%  |

## What the low numbers tell us (and the follow-ups they name)

- **pst_parser.cpp 30.6%.** The PST fuzz seeds fail closed at the header / CRC layer, so
  the deep BTree, LTP, and messaging code is never reached. This is the exact gap already
  noted for R5-G14-5: adding page-trailer-valid store seeds so accepted files exercise
  those layers is the next increment, and this number will move when it lands.
- **crash_reporter.cpp 25.0%.** Only the pure helpers are unit-tested; the actual
  SetUnhandledExceptionFilter / MiniDumpWriteDump path needs a real fault to exercise and
  is certified manually (see R5-G23-2). Coverage cannot reach it from a normal test.
- **The newly-fuzzed seams (sanitizer, mbox header, mbox transfer decoder, native
  messaging) are at 100%.** Extracting a parser into a pure seam and fuzzing it drives its
  lines fully, which is the coverage case for continuing the seam program.

## Coverage exclusion inventory (R5-G14-16c)

The headless unit suite runs with QT_QPA_PLATFORM=offscreen, no admin token, no real
hardware, and no network peer. A large, security-critical slice of the tree therefore
CANNOT execute under coverage by construction -- it is instead certified by live rigs,
external elevated gates, real-kernel round-trips, or manual QA. Below is that exclusion
set: the file, the specific area, why headless cannot reach it, and what certifies it
instead. In every case the PURE logic beside the excluded code (validators, parsers,
decision functions, script/string builders) IS unit-tested; only the hardware/OS/GUI
boundary is excluded. This inventory is what makes "coverage over testable code" honest:
these lines are out of the testable-headless universe on purpose, not skipped.

### 1. Raw storage device I/O (real \\.\PhysicalDriveN handles)

- `src/threading/flash_worker.cpp` **openDevice/queryDeviceSectorSize/queryDeviceCapacity** -- CreateFileW on \\.\PhysicalDriveN + IOCTL identity/geometry/length; unit tests hit PhysicalDrive99/999 which never open. Cert: flash_live_certifier, artifacts/flash-live-certification/disk3-flashworker-b4/report.txt, R1 cert b29506f.
- `src/threading/flash_worker.cpp` **writeImage/writeChunk/finalizeWrite** -- sector-aligned NO_BUFFERING WriteFile to a real device (destructive). Cert: flash_live_certifier full-verify (64 MiB, source SHA-512 == device SHA-512).
- `src/threading/flash_worker.cpp` **verifyFull/verifySample/compareDeviceBlock** -- NO_BUFFERING read-back + hash of on-disk bytes. Cert: flash_live_certifier B4-01/06/07.
- `src/threading/flash_worker.cpp` **refuseIfTargetIsOsDisk/physicalDriveBacksWindows** -- OS-disk self-guard via IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS. Cert: flash_live_certifier system-disk refusal; manual.
- `src/core/flash_coordinator.cpp` **physicalDriveOsDiskCheck / unmountVolumes offline path** -- authoritative boot-disk guard + persistent-OFFLINE dismount (needs admin + real disk). Cert: b29506f + partition-manager-certification VM-lab admin report.
- `src/core/drive_unmounter.cpp` **preventAutoMount/ejectDrive/lockAndDismountVolume** -- IOCTL_DISK_SET_DISK_ATTRIBUTES OFFLINE + eject/lock/dismount on a real removable disk. Cert: flash-live-certification unmount stage; manual.
- `src/core/partition_raw_device_io.cpp` **WindowsRawDevice (aligned read/write/seek/sync)** -- instantiates only for a real raw-device path; headless drives the QFile image branch. Cert: partition-manager strict VHD run + APFS physical-USB (apfs-a8-status). (The pure classifiers isWindowsRawDevicePath/canonicalRawDevicePath ARE unit-tested.)
- `src/core/drive_scanner.cpp` **queryDriveInfo/physicalDriveBootProbe/enumeratePhysicalDriveNumbers** -- enumerates real \\.\PhysicalDriveN + per-disk IOCTLs; hardware-specific. Cert: manual (real-drive enumeration on Randy's PC).
- `src/core/stress_test_worker.cpp` **runDiskStress/writeDiskStressFile** and `src/core/disk_benchmark_worker.cpp` **runSequential/RandomQd32 + createTestFile** -- NO_BUFFERING direct I/O throughput against real media (hardware-bound). Cert: manual live rig (validators validateDiskTarget/duration/block-size ARE unit-tested).
- `src/core/partition_executor.cpp` **executeElevatedScript/executeScript** -- diskpart apply against a real disk through the elevated broker. Cert: partition-manager strict disposable-VHD (12/12 gates) + VM-lab admin report.

### 2. Foreign-filesystem writes certified only by a real OS kernel round-trip

- `src/core/partition_apfs_writer.cpp` **commitRaw* family + formatExistingContainerTarget (raw-device branch)** -- in-place COW commit / format to a real \\.\PhysicalDrive; mountability provable only by the macOS kernel. Cert: APFS A2/A7 in-place + physical-USB (apfs-a2-inplace-commit-status, apfs-a8-status); MacBook kernel rig 10.10.11.38. (setRawDeviceTargetPredicateForTesting drives the ORCHESTRATION headless; the device open/aligned-write/flush is excluded.)
- `src/core/partition_apfs_writer.cpp` **FileVault ONEKEY + per-file-encrypted format** -- AES-XTS-encrypted keybag/fs-tree; correctness = kernel UNLOCKS+mounts (apfsck cannot read VEK-encrypted metadata). Cert: APFS A6 Apple-cert (apfs-a6-encryption-status).
- `src/core/partition_apfs_writer.cpp` **commitRawSnapshotRevert (deferred revert_to_xid/sblock tag)** -- kernel performs the actual post-snapshot-divergence discard on next mount. Cert: APFS A3 REVERT Apple-cert.
- `src/core/partition_apfs_writer.cpp` **commitRawResize (in-chunk grow)** -- consistency of the grown container proven only by kernel remount + fsck_apfs on media spanning new_size. Cert: APFS A7 + physical-USB raw grow bed2d29.
- `src/core/partition_apfs_writer.cpp` **transparent-compression inserts (decmpfs zlib/lzfse/lzvn/lzbitmap, resource-fork algos 4/8/12/14)** -- fidelity = Apple's kernel decompressor returns the original (kernel md5). Cert: APFS A5 + resource-fork (apfs-a5-compression-status, apfs-resource-fork-status).
- `src/core/partition_apfs_writer.cpp` **multi-chunk/CAB-tier + multi-volume format emission** -- validity proven by kernel auto-synth+mount + fsck_apfs on the shared-spaceman container. Cert: APFS A1/A4 + apple-tool-evidence/report.json (qemu macOS apfs.kext). (computeContainerGeometry IS unit-tested.)
- `include/sak/partition_apfs_writer.h` **crash_replay/rollback_boundary/hardware_raw_media evidence fields** -- attest a real power-loss round-trip (write, hard-kill mid-commit, reboot, kernel recovery, fsck). Cert: APFS A8 crash-rollback pngs (the validator only checks the bit is present).
- `src/core/partition_hfs_file_system_writer.cpp` **replayJournal / createHardlink/Symlink / replaceFileWithAllocationGrowth+decmpfs** -- BE journal replay, indirect-node hardlinks, extent-growth/compression proven by fsck_hfs + HFS+ kernel mount readback. Cert: HFS+ H1-H8 phys-USB Apple-cert (h2-hfs-btree-scope).

### 3. Admin / OS-mutating ops (elevation; would damage the host)

- `src/core/cleanup_worker.cpp` **removeService/removeScheduledTask/removeFirewallRule** (sc/schtasks/netsh delete) and **deleteRegistryKey/Value** (RegDeleteTreeW) -- irreversibly remove real host state; CATASTROPHIC-gated. Cert: parse/decision helpers unit-tested (test_leftover_scanner); execution manual/live (R3 WAVE F).
- `src/core/ethernet_config_manager.cpp` **runNetsh/restoreSettings/setSourceDhcp/applyStaticIp** and `src/core/app_mutating_actions.cpp` **setAdapterDhcp/StaticIp/Dns** -- elevated netsh rewrites live adapter IPv4/DNS. Cert: isIpv4Literal + snapshot round-trip unit-tested; live apply is [[no-vm-networking-cert]], certed once on Randy's PC (R3 WAVE F).
- `src/core/wifi_setup_script.cpp` **connectWifiWindows** and `src/actions/reset_network_action.cpp` **executeResetWinsock/IpStack/Firewall** -- netsh wlan connect / full stack teardown (needs a real AP / would sever connectivity). Cert: profile-XML builder + validators unit-tested; connect manual + [[no-vm-networking-cert]].
- `src/core/restore_point_manager.cpp` **createRestorePoint** (elevated Checkpoint-Computer) and `src/actions/backup_bitlocker_keys_action.cpp` **executeExtractKeys** (Win32_EncryptableVolume WMI) -- need admin + live System Restore / a real BitLocker volume. Cert: parse helpers unit-tested; BitLocker external gate launch_partition_manager_bitlocker_mutation_external_gate_elevated.cmd.
- `src/core/partition_executor.cpp` / `src/core/partition_script_builder.cpp` **executeElevatedScript / mbr2gpt convert emission** and `src/core/windows_usb_creator.cpp` **runDiskpartScript/cleanAndPartitionDisk** -- diskpart/mbr2gpt/bcdboot against a real boot/removable disk. Cert: script BUILDERS + safety validators unit-tested; external VM/USB gates (run_partition_manager_destructive_certification, mbr2gpt external gate).
- `src/core/elevation_manager.cpp` **executeElevated/restartElevated** + `src/core/elevation_broker.cpp` **launchHelper/connectPipe/verifyServerImage** + `src/core/elevated_pipe_server.cpp` **createPipe/ConnectNamedPipe** -- ShellExecuteExW runas UAC + elevated-helper named-pipe handshake (needs interactive consent + a live elevated peer). Cert: isElevated/serializeArgs/error-message helpers unit-tested; round-trip via elevated external gates; manual.

### 4. Crash / fault / exception handlers (need a real fault to fire)

- `src/core/crash_reporter.cpp` **sakUnhandledExceptionFilter + MiniDumpWriteDump + re-entrancy guard + install (SetUnhandledExceptionFilter)** -- run only on a genuine unhandled SEH fault with live EXCEPTION_POINTERS; faulting in-process would abort the test runner. Cert: R5-G23-2; helpers crashFileStem/exceptionCodeName/formatSummary unit-tested (test_crash_reporter); filter body manual/soak.
- `src/main.cpp` **top-level catch(...) safety net + font-warmup teardown join** -- fire only on a fatal escape to the entry frame / a real GUI-teardown font-DB race. Cert: manual; GUI-startup-truncation memory.
- `src/threading/worker_base.cpp` **stopAndJoin forced terminate()+std::abort watchdog** -- runs only if a thread survives terminate()+5s join (cannot manufacture headless). Cert: cooperative stop certified by test_worker_base destructorStopsThread; abort is never-reached last resort.
- `src/gui/main_window.cpp` **~MainWindow m_shutting_down teardown guard** -- suppresses a currentChanged-during-destruction use-after-free needing a real window-teardown race. Cert: commit 806d5c5 + 20-run Release shutdown soak 0/20 (manual).

### 5. Live external clients and network peers

- `src/core/connectivity_tester.cpp` **sendIcmpEcho/traceroute/probeHop**, `src/core/dns_diagnostic_tool.cpp` **performQuery**, `src/core/port_scanner.cpp` **scanPort/runTcpProbe**, `src/core/bandwidth_tester.cpp` **runIperfTest**, `src/core/network_share_browser.cpp` **enumerateShares/testReadWriteAccess** -- real ICMP/DNS/TCP/iperf3/SMB against a live peer; headless has none. Cert: manual (live on Randy's PC); the parse/mapping helpers (sanitizeConfig, answersEquivalent, getServiceName, iperf3 output parse) are unit-tested.
- `src/core/linux_iso_downloader.cpp` / `src/core/uup_dump_api.cpp` / `src/core/uup_iso_builder.cpp` **startDownload / fetch* / aria2+UUPMediaConverter pipeline** -- real multi-GB HTTPS downloads + external process ISO assembly. Cert: manual; URL/redirect-safety + isSafeAria2FileEntry guards fuzzed (test_fuzz_uup_manifest_guard, test_linux_iso_downloader).
- `src/core/wifi_analyzer.cpp` **WLAN scan (WlanScan/WlanGetAvailableNetworkList)** -- real radio scan of nearby APs (never on the VM per no-VM-networking). Cert: manual; frequencyToChannel/deriveBssSecurity/lookupVendor unit-tested.
- `src/win32mcp/browser_control.cpp` / `win32_mcp_input.cpp` / `win32_mcp_desktop.cpp` / `native_messaging.cpp` **CDP pipe exchange / SendInput / UIA walk + capture / native-messaging relay** -- drive a real Chrome + real desktop session. Cert: Track B browser/desktop live-cert rig (VM 10.10.11.183, ext 0.3.11); framing/contract fuzzed (test_fuzz_mcp_framing, test_browser_contract).
- `src/core/email_export_worker.cpp` **exported PST/EML/MBOX open-in-client fidelity** -- opening in real Outlook/Thunderbird is a human-observed live cert. Cert: R5-G23-11 (BLOCKED-ON-USER); writer byte-output unit-tested (test_email_export_worker, test_html_email_writer).

### 6. GUI paths a headless offscreen session cannot drive

- `src/gui/ai_assistant_panel.cpp` **runScreenshotTool (grabWindow(0))** and `src/gui/splash_screen.cpp` **showCentered/paintEvent** -- capture/composite the real desktop framebuffer; offscreen never exposes/composites. Cert: docs/AI_ASSISTANT_MANUAL_QA_RUNBOOK.md + ai-assistant-vm-smoke.
- `src/gui/main_window.cpp` **Run-as-Admin -> restartElevated** -- ShellExecuteExW runas raises a secure-desktop UAC prompt. Cert: manual (R3 WAVE F elevation live-cert).
- `src/gui/file_management_explorer_panel.cpp` **QDrag::exec startDrag + cross-process drop eventFilter** -- native blocking DnD modal / OS shell CF_HDROP delivery. Cert: manual; the mime-decision helpers (dropActionFor, mimeHasPasteableItems) ARE unit-tested.
- `src/gui/image_flasher_panel.cpp` **QFileDialog::getOpenFileName** -- native OS file picker (no DontUseNativeDialog set). Cert: manual QA runbook.
- clipboard interop (`network_diagnostic_panel.cpp`, `partition_manager_panel.cpp`, `advanced_uninstall_panel.cpp`, `ai_transcript_view.cpp` **clipboard()->setText**) -- real cross-app OS clipboard/CF_UNICODETEXT delivery. Cert: manual.
- `src/gui/detachable_log_window.cpp` **moveEvent snap (frameGeometry)**, `src/gui/info_button.cpp` **availableGeometry clamp**, `src/gui/file_explorer_status_center_widget.cpp` **devicePixelRatioF HiDPI branch** -- depend on a real WM frame / real monitor bounds / DPR != 1. Cert: manual.

## Branch coverage (R5-G14-16b) -- DONE 2026-08-25

Branch coverage is now measurable: `scripts/run_branch_coverage.ps1` over a clang-cl tree
configured with `-DENABLE_COVERAGE=ON` (the CMake option refuses to configure under MSVC
rather than emit uninstrumented binaries whose empty report would read as full coverage).

WHY IT WAS NEEDED, measured rather than argued. A line is "covered" the moment any part of
it executes, so an untaken arm sharing a line with a taken one is invisible.
`include/sak/mbox_transfer_decoder.h` sits at 100.0% in the per-file table above. Measured
with branch coverage against the pre-b99 test corpus it was **87.50%, four branches never
taken**:

| branch | source |
|--------|--------|
| `hex1 == '\n'`  | the bare-LF soft break every Unix mailer writes |
| `hex2 == '\n'`  | the CRLF arm of the soft-break ternary |
| `ok_second`     | short-circuited away by every malformed-hex fixture |
| whitespace arm  | the base64 unwrap every wrapped MIME body takes |

All four were real holes, found by hand in sweep b99 and pinned there; the same file now
reports 32/32 branches. Line coverage read 100% throughout.

### First whole-suite measurement (2026-08-25, at commit 58fcb1a2)

Scope: **228 of the 246 built test executables**. Eighteen do not compile under clang-cl and
are listed below -- they fail on conformance differences MSVC permits (a default member
initializer used inside its own enclosing class, protected-member access, a using-declaration
naming its own class), NOT on anything wrong with the tests. Excluded targets:

    test_active_connections_monitor    test_migration_report
    test_ai_app_action_planner         test_msg_writer
    test_ai_assistant_panel_tool_dispatch  test_network_diagnostic_controller
    test_ai_command_tool_planner       test_package_matcher
    test_app_installation_worker       test_pst_splitter
    test_browser_bridge_pipe           test_pst_writer
    test_browser_bridge_relay          test_quick_action
    test_dbx_writer                    test_user_data_manager
    test_imap_uploader                 test_win32_mcp_server

Raw result: **19,788 branches never taken in one direction**, across 272 files. That raw
number is NOT a work list, and quoting it as one would misrepresent it: it counts every
region the exclusion inventory below already documents as unreachable from a unit test. The
five largest contributors are all in those categories --

| file | missed | exclusion |
|------|--------|-----------|
| include/sak/partition_hfs_internal.h | 3339 | #2 foreign-FS writes, macOS-kernel certified |
| src/core/partition_apfs_writer.cpp | 1662 | #2 same |
| src/gui/partition_manager_panel.cpp | 1258 | #6 GUI |
| src/gui/file_management_explorer_panel.cpp | 1143 | #6 GUI |
| src/gui/wifi_manager_panel.cpp | 245 | #6 GUI |

Subtracting the GUI and foreign-filesystem-writer trees leaves **10,964 actionable missed
branches**, written to `build-cov/coverage/ACTIONABLE.txt` by the script. Those are branches
in code the suite does drive, and they are the same class the G18-4 weak-assertion sweep has
been finding by inspection -- a guard whose refusal arm no fixture reaches, because every
fixture supplies the benign shape the guard exists to reject. Sample, from a file sweep b97
had already been through:

    uup_iso_builder.cpp:67  never TRUE  if (unit == QLatin1String("KiB"))
    uup_iso_builder.cpp:70  never TRUE  if (unit == QLatin1String("GiB"))
    uup_iso_builder.cpp:81  never TRUE  info.exists() && (info.isSymLink() || info.isJunction())
    uup_iso_builder.cpp:88  never TRUE  !value.contains('\n') && !value.contains('\r')

The junction arm is the same shape that turned up a real subtree-escape defect in G10-9.

Caveats that bound the number honestly: coverage over a SUBSET of tests overstates misses,
because a test binary statically links code it never drives (the same distortion noted for
the per-subsystem table above) -- so the eighteen excluded targets inflate the count for the
PST/MSG/DBX writer and user-data files in particular. llvm-cov also reported 1259 functions
with mismatched data across the merged profile, whose counts are correspondingly unreliable.

## Not yet done (tracked, not claimed)

The measured line baseline above is over the
CURATED CORE SET, not the full suite; raising specific reachable numbers is tracked as its
own increments (e.g. R5-G14-5 adds page-trailer-valid PST store seeds so pst_parser.cpp
executes its deep BTree/LTP layers). The dispositions of R5-G14-16a (enforce 100% line) and
R5-G14-16d (wire a blocking gate) are recorded against those items in
docs/CODEX_REVIEW_5_REMEDIATION.md.
