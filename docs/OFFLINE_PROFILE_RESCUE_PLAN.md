# Offline profile rescue plan (profile from a dead machine's drive into a S.A.K. backup)

STATUS: PROPOSED, in review (PR #12). Not approved by the maintainer; the "Decisions" below are
this proposal's design choices, open to the maintainer's review. Grounded against the live tree
on 2026-11-21.

Goal: attach a disk from a non-running Windows install, have S.A.K. recognize the profiles
on it, run them through the EXISTING backup pipeline (filters, compression, AES-256, SHA-256
manifest, verify), and restore on a working PC with the EXISTING restore wizard.

## Why it fails today (four blockers, verified)

1. The user scanner is live-only: `NetUserEnum` on the RUNNING system's SAM accounts
   (`include/sak/windows_user_scanner.h:25-30`), profile paths resolved on this machine
   (`:59`). A mounted `D:\Users\bob` from a dead laptop appears nowhere.
2. Custom folders cannot reach outside a profile: the dialog computes a path RELATIVE to the
   chosen profile and refuses anything outside it (`src/gui/per_user_customization_dialog.cpp:354-363`).
3. Nothing loads a registry hive: no `RegLoadKey` anywhere in the tree, so the offline OS's
   ProfileList (SID->profile map) and computer name cannot be read today.
4. A BitLocker-encrypted source yields no files. The app backs up recovery keys
   (`src/actions/backup_bitlocker_keys_action.cpp`, elevated WMI) but has no offline unlock;
   `Unlock-BitLocker` appears only as guidance text (`:1269`).

## The key architectural fact: the engine needs no changes

`UserProfileBackupWorker::startBackup(manifest, users, destination, smartFilter, options)`
(`include/sak/user_profile_backup_worker.h:70-74`) is fully data-driven: it takes
`UserProfile{username, sid, profile_path, folder_selections(relative)}` and copies. Feed it
UserProfiles built from an offline volume and everything else is reused unchanged:

- SmartFilter already excludes registry hives from any profile backup, non-negotiably
  (`user_profile_types.h:163-167`, `user_profile_types.cpp:105-111`, re-unioned from JSON at
  `src/core/smart_file_filter.cpp:34`). Same guarantee holds for offline profiles: no hives.
- Destination writes already go through `no_follow::copyFileTo(..., Publish::NoReplace)`
  (`src/core/user_profile_backup_worker.cpp:918`).
- Codec/manifest integrity as-is: `BackupManifest::hashDirectoryTree` + checksum
  (`user_profile_types.h:342-360`).
- Restoring reads a backup from ANY drive and maps users to accounts on the running system
  or `CreateNewUser` -- a new folder under `Users\`, no account created
  (`src/gui/user_profile_restore_wizard.cpp:82,156-157`; `user_profile_restore_wizard_pages.cpp:107-141`).
  Rescue's destination path therefore needs ZERO restore-side changes.
- Privileges exist: `PermissionManager` enables `SE_BACKUP_NAME`/`SE_RESTORE_NAME`
  (`src/core/permission_manager.cpp:335-337`). Reading files owned by unknown SIDs on a
  mounted disk needs backup privileges -- a plain DACL has no admin override
  (`tests/unit/test_mixed_tier_operations.cpp:190`). Rescue runs must be elevated.
- Disk discovery exists: `DriveScanner::hasWindowsIndicators(root)` proves a Windows install
  at a root and works on UNMOUNTED volumes via `\\?\Volume{GUID}\` roots
  (`include/sak/drive_scanner.h:266`; `getVolumeRootsForDrive`; certified by
  `tests/unit/test_drive_scanner.cpp:614-642` on a temp tree). `containsWindowsInstallation`
  (`src/core/drive_scanner.cpp:928`), `getMountPoints` (`:681`).

So the feature is: a NEW headless offline scanner producing UserProfiles + a wizard source
mode. Not a new backup engine.

## Work items

Phase 1 -- offline volume access (new `src/core/offline_profile_source.{h,cpp}`, headless seam)
- Enumerate physical drives EXCLUDING the live system drive (refuse rescuing from the PC
  running S.A.K.; `containsWindowsInstallation` on its drive number).
- For each volume root (drive letter if mounted, else `\\?\Volume{GUID}\` -- no disk-state
  change, no letter assignment, so nothing is brought online or remounted): probe
  `hasWindowsIndicators` to name candidate offline Windows volumes.
- Qt path handling mangles `\\?\` prefixes (`src/core/drive_scanner.cpp` header notes);
  follow the established native-backslash GetFileAttributesW pattern, and normalize
  `\\?\Volume{GUID}\Users\bob` sources into every `UserProfile.profile_path`.
- BitLocker gate: if the chosen volume is encrypted and locked, REFUSE plainly with the
  reason; route the technician to BitLocker tools. Fail-closed, never a silent empty scan.
  Whether S.A.K offers offline unlock with a typed recovery key is Decision 1.

Phase 2 -- offline profile enumeration (new, unit-testable against temp trees)
- A profile = a direct subfolder of `<root>\Users` containing `NTUSER.DAT` (the marker that
  it is a real profile, not a stray folder); exclude `Public`, `Default*`, `desktop.ini`.
- Username = folder name, labeled honestly; SID recorded as `sid` where readable.
- Optional identity upgrade (Decision 2): load `<root>\Windows\System32\config\SOFTWARE`
  with `RegLoadKey` under a temp HKLM key, elevated; read
  `...\CurrentVersion\ProfileList` (SID->path) and `CurrentVersion\ComputerName` for
  `manifest.source_machine`. Unload in a `finally`, verify the unload, fail closed if a hive
  stays loaded. First RegLoadKey in the tree, so it gets its own seam + its own tests.
- Reuse `estimateProfileSize` / `getDefaultFolderSelections` statics
  (`include/sak/windows_user_scanner.h:69,85`) against the offline root.

Phase 2b -- headless action seam (the repo rule: no GUI commands)
- Profile backup today has NO headless action id at all (grep of the action registries finds
  none): the wizard + `UserProfileBackupWorker` is the only route. The offline rescue should
  therefore OPEN the seam, mirroring the offline-image precedent -- the existing
  `scan_recoverable` action takes an `image_path` argument, refuses unsafe paths first and opens
  the source read-only (`src/core/app_readonly_actions.cpp:1189-1210`, comment at 1118-1125):
  a new action (e.g. `backup_offline_profiles`: source volume/path, profiles, options)
  registered with its own schema and tests. Whether the live wizard ALSO gets an action id in
  the same change is a scope question for the maintainer (Decision 7).

Phase 3 -- wizard source mode (GUI only)
- A source step on page 1: "This PC" (today) or "Another Windows installation" -> offline scan list, each profile with size estimate, same folder-selection dialog (`PerUserCustomizationDialog`) and global smart-filter/compression/encryption/verify pages unchanged.
- `manifest.source_machine` = offline computer name (or volume label when the hive can't be read). Today backup stamps it with `QSysInfo::machineHostName()` (`src/gui/user_profile_backup_wizard_pages.cpp:896`) and the restore welcome page displays it, HTML-escaped, for the technician (`src/gui/user_profile_restore_wizard.cpp:185-199`) -- an honest offline name keeps that label meaningful.
- The out-of-profile refusal (`per_user_customization_dialog.cpp:354-363`) stays: relative paths are computed against the OFFLINE profile root, so it needs no relaxation -- only that the wizard sets `profile_path` to the offline folder.

Phase 4 -- restore-side honesty (small, no format change)
- Mapping an offline SID to the destination system: never merge the SID; the existing `PermissionMode::StripAll` default already handles it. `AssignToDestination` works as-is (dispatched in `src/core/user_profile_restore_worker.cpp:1046-1053`). `PreserveOriginal` re-applies a SID the destination machine doesn't know -> the restore wizard must show the offline SID as unresolved rather than promise ownership it can't grant.
- A username collision (destination already has "bob") is the existing path: the technician pairs the source user with the existing account or `CreateNewUser` under the rules at `src/gui/user_profile_restore_wizard_pages.cpp:108-130`, and file-level conflicts follow the existing `ConflictResolution` combo on the options page (`:1481-1484`). Not new behavior.
- NTUSER.DAT stays out of the payload (filters), so the restored profile gets a fresh hive -- honest behavior, and it must be stated in the wizard's restore summary ("settings and files, not registry settings").

## Decisions (proposed design choices, not maintainer rulings)

1. **BitLocker.** OFFLINE UNLOCK offered: a locked encrypted source is unlocked with a typed
   recovery key (the larger option, chosen over plain refusal). Key entered in the wizard,
   never logged; unlock attempted through the elevated path; a wrong or unusable key refuses
   plainly with the reason.
2. **Registry hive.** SOFTWARE hive loaded when elevated to recover exact SIDs and the
   computer name, and SAM for the machine SID. Fail closed: when SOFTWARE or SAM cannot be
   read, every folder is listed as skipped with the reason, because local ownership is
   unproven.
3. **NTUSER.DAT.** Stays excluded from the payload (as in live backups).
4. **Domain / Microsoft-account profiles.** v1 is LIMITED TO LOCAL ACCOUNTS: the hive read
   distinguishes them, and domain profiles are listed as skipped with a reason.
5. **Elevation.** Required; unelevated runs refuse with the standard elevation message.
6. **Version bump.** PATCH: `0.9.3` -> `0.9.4` (`VERSION`, `project()` in `CMakeLists.txt`).
7. **Headless seam scope.** LIVE wizards ALSO get action ids in this change:
   `backup_user_profiles` and `restore_user_profiles` join the new `backup_offline_profiles`
   (three registrations, schemas and tests).

## Test plan (must-fail-without-the-fix drills included)

- Unit: profile-folder enumeration against temp trees (NTUSER.DAT marker, Public/Default
  exclusion, symlink/junction refusal via `tests/support/junction_fixture.h` canary).
- Unit: `hasWindowsIndicators` already certified (`test_drive_scanner.cpp:614-642`); reuse.
- Unit: worker fed an offline-shaped manifest writes to a temp destination, manifest checksum
  + sidecar rules re-verified; drill: break the offline source path, tests must go red.
- Live (must-run-elevated, `SAK_TEST_ALLOW_REAL_HARDWARE=1`, VHDX like `test_raw_partition_write`):
  build a small offline Windows tree inside a mounted VHDX, scan it, back it up, restore on the
  rig, add PASS lines to ci.yml's must-run-elevated list.
- Guard: rescuing from the LIVE system drive's own `Users` folder must be refused; drill by
  pointing the scanner at C:.
- BitLocker-locked volume: refuse with the named reason, never an empty success (fail closed).

## Progress (grounded in the live tree, 2026-11-22)

Build/test proof below is cited from the 2026-11-22 run on this tree: full Release build
green; CTest `-L product` 306/306 passed; `check_program_imports.ps1 -Directory
build/Release` clean (301 executables); `check_startup_budget.ps1` unelevated 3674 ms
against the 15000 ms budget. The elevated VHDX hardware case is NOT covered by that proof
and stays uncertified (see the last bullet).

- **Phase 1 and 2: implemented.** `include/sak/offline_profile_source.h` +
  `src/core/offline_profile_source.cpp` (added to the library source list at
  `CMakeLists.txt:477`): volume discovery (`OfflineProfileSource::discover`,
  `discoverVolume`), the live-system-drive guard (`isLiveSystemRoot`,
  `liveSystemVolumeName`), the `OfflineRegistryHive` RAII loader with a verified unload,
  `readOfflineHives` (SOFTWARE ProfileList + ComputerName, SAM machine SID via
  `machineSidFromSamAccountValue`), `scanUsersDirectory` with the NTUSER.DAT marker and the
  built-in-folder exclusions, `profileSkipReason` for domain/Microsoft accounts, and BitLocker
  offline unlock (`normalizeRecoveryKey`, `bitLockerUnlockScript`, `unlockEncryptedVolume`)
  staged through a held read-only session-temp key file that is shredded whichever way the
  runner goes. Elevated PowerShell goes through an injectable `ElevatedScriptRunner`, so every
  decision above is unit-testable without elevation.
- **Phase 2b: implemented.** Three registrations in `src/core/app_mutating_actions.cpp`
  (`registerUserProfileMutatingOps`): `backup_user_profiles`, `backup_offline_profiles`
  (`requires_admin`), `restore_user_profiles`, each driving the SAME
  `UserProfileBackupWorker` / `UserProfileRestoreWorker` the wizards drive, through the
  `AsyncActionInvocation` bridge. Schema builders `backupUserProfilesParamsSchema`,
  `backupOfflineProfilesParamsSchema`, `restoreUserProfilesParamsSchema`. Catalog pins raised
  74 -> 77 in `tests/unit/test_ai_assistant_panel_tool_dispatch.cpp`, and the three ids are on
  the cancellable list.
- **Tests green, with red drills proved:** `tests/unit/test_offline_profile_source.cpp`
  (registered at `tests/CMakeLists.txt:5656`, `product` label) -- 23 cases pass
  (`CTEST EXIT: 0`). Each of five seams was drilled separately and each turned the suite red
  (`CTEST EXIT: 8`), then restored and rebuilt: the SAM SID offset, the built-in folder list,
  the machine-SID prefix compare, the live-volume GUID resolution, and the live-volume guard.
  The dispatch pins (77 ids, the cancellable list) pass in
  `tests/unit/test_ai_assistant_panel_tool_dispatch.cpp`, and the ten reachable profile suites
  (`test_user_profile*`, `test_backup_*`, `test_fuzz_backup_codec`) pass.
- **A real defect the tests caught.** `liveSystemVolumeName()` returned what
  `GetVolumePathNameW` hands back -- a drive letter -- which never equals the
  `\\?\Volume{GUID}` names the enumeration returns, so `discover()` would have listed the
  RUNNING system as an offline source and `discoverVolume("C:")` refused with a syntax
  complaint instead of "this is the running Windows installation". Both now resolve any
  mount spelling to the volume GUID (`resolveVolumeGuid`) before they compare, and refuse
  fail-closed when Windows cannot name the mount.
- **Product suite green on this machine:** the `product` CTest label runs 305 tests,
  `CTEST EXIT: 0`, 100% passed. The app-level gates pass too: `scripts/check_program_imports.ps1`
  over `build\Release` (300 executables, every import resolves beside them or in System32) and
  `scripts/check_startup_budget.ps1` (cold smoke startup 3644 ms against a 15000 ms budget,
  `SAK_STARTUP_SMOKE_OK`). `test_single_instance_app` is skipped here by design: it runs the real
  application against this PC's drives and is CI-only (`SAK_TEST_ALLOW_REAL_HARDWARE`).
- **Two failures that suite surfaced were pre-existing on this PC, not the addon.** Both were
  fixed, and neither touches the offline code.
  - `tests/unit/test_file_explorer_item_model.cpp` asserted the literal `"QQZ7 File"`, but
    `FileExplorerItemModel::typeText()` deliberately asks Windows for the type name
    (`SHGetFileInfoW`), and Windows answers in the machine's UI language -- `"QQZ7-bestand"` on
    this Dutch Windows. The product was correct on a localized PC and the test was wrong; it now
    compares against what Windows reports and requires the name to carry the extension, so it
    holds in any UI language. Drilled by making `typeText()` return a generic `"File"`: the suite
    went red, restored, rebuilt, green.
  - `scripts/test_sak_hfs_writer_cli.ps1` ran under this build's cached
    `SAK_TEST_POWERSHELL_PATH`, which configure had resolved to Windows PowerShell 5.1 before
    pwsh was installed on this workstation. Under 5.1, `$ErrorActionPreference = "Stop"` turns a
    native tool's *expected* stderr -- the very refusal that test checks -- into a terminating
    error, so the script died on a passing guard. Reconfigured to `pwsh.exe` 7.7, the shell CI
    uses, it passes. A shell-agnostic rewrite of that script's ~40 native calls was NOT done: a
    partial fix would read as covered and not be, and the wider question (require pwsh 7 at
    configure, or route every native call through a capture helper) is the owner's call.
- **Phase 3 (wizard source mode): implemented, suite green.** New
  `include/sak/offline_profile_source_page.h` + `src/gui/offline_profile_source_page.cpp`:
  `UserProfileBackupSourcePage` (This PC / another installation), `BitLockerRecoveryKeyDialog`
  (password-echo field, `keyProblem()` refuses a wrong-shape key on screen without ever
  repeating the digits), and `UserProfileBackupOfflineProfilesPage` (volume rows that say WHY
  they are unreadable, profile rows with skip reasons, the Unlock button armed only for a
  GUID-named locked volume, and `nextId()` skipping the This-PC-only customize steps for an
  offline run). The wizard stamps the manifest with the DEAD machine's name through
  `UserProfileBackupWizard::manifestSourceMachine()` -> the page's `sourceComputerName()`,
  never this PC's host name. Suite `tests/unit/test_user_profile_backup_offline_source.cpp`
  (registered at `tests/CMakeLists.txt`, `product` label): 14 cases pass.
  - Drill campaign, each drilled separately with exact-byte save/restore + touch (the sources
    are CRLF; LF-only patch anchors silently applied nothing -- caught, re-run CRLF-safe):
    dropping the `cleanupPage()` release, `keyProblem()`'s shape check, the Unlock-arming rule,
    the manifest stamp, the unreadable-row reason text, the two `nextId()` skips, and the
    no-echo rule on a refused unlock each turned its test RED, then restored and rebuilt green.
    One drill stayed green for a real reason, not a gap: `OfflineVolumeMount`'s destructor
    itself calls `release()` and reports failure, so the mount comes down even when
    `validatePage()`'s explicit previous-release block is removed; the test pins the
    observable contract (exactly one unmount per double `validatePage()`).
  - The page's `setSeamsForTesting` was moved behind `SAK_GUI_TEST_HOOKS`
    (`SAK_GUI_OFFLINE_SOURCE_TEST_HOOK_SOURCES` in `tests/CMakeLists.txt`) after
    `test_release_binary_test_hooks` correctly RED-flagged the shipped `sak_gui.lib` carrying
    a `ForTesting@` symbol -- the same shape `SAK_CORE_TEST_HOOKS` uses for
    `QuickActionController`. The startup accessibility audit also RED-flagged the new Unlock
    button: its accessible name must HOLD the visible label (`accessible_name_rule.h`), which
    it now does.
- **Phase 4 (restore honesty): implemented, suite green, drilled.**
  `UserProfileRestorePermissionSettingsPage::updateSummary()` now distinguishes a manifest
  whose `source_machine` differs from this PC: it names the machine and says its account IDs
  "cannot be resolved here", instead of the local "may cause access issues" hedge -- and it
  HTML-escapes `source_machine`, which is text from a file anyone can hand the technician
  (the same risk the welcome page's escape tests pin). The restore summary states plainly
  that a backup never holds the original NTUSER.DAT and no registry settings come back.
  Three cases in `tests/unit/test_user_profile_app_restore_flow.cpp` pin all of it
  (offline refusal + escape, this-PC case keeps the local wording, the NTUSER.DAT line on
  every permission mode); the suite passes 48/48.
  - Drill campaign, each drilled separately with exact-byte save/restore + touch: forcing
    the offline branch off (RED: the named machine disappears from the summary), feeding
    `source_machine` in unescaped (RED: the failure text shows a raw `evil<b>x</b>`
    injection -- the test really pins the escape), deleting the NTUSER.DAT honesty line
    (RED), and inverting the machine-name comparison (RED: the this-PC case loses its
    local wording). All restored, rebuilt, and the suite re-ran 48/48.
- **Whole-tree style gates caught eight more violations the per-file hooks never saw.**
  `scripts/run_lizard.py` found 4 functions over the length/CCN/PARAM limits in
  `app_mutating_actions.cpp` and `offline_profile_source_page.cpp`, and
  `scripts/check_magic_numbers.py` found 9 bare literals in the SID parse and size walk of
  `offline_profile_source.cpp` (none blocked a commit; the T2 static-gates job runs them,
  and pre-commit only sees changed files). All twelve were fixed STRUCTURALLY, no behavior
  change: seam extractions (`isDotEntry`, `mountOfflineVolume`, `offlineRefusalWithMount`,
  `offlineUsersToBack`, `offlineMountReleaseWarning`, `runRestoreInvocation`,
  `buildProfileTableWidget`, `sourceStatusText`), option bundles (`ProfileBackupRequest`,
  `ProfileRestoreRequest`, `TestSeams` for the page's six test seams -- the 9 call sites in
  `test_user_profile_backup_offline_source.cpp` updated), and named constants for the SID
  layout (`kSidRevisionNt`, `kSidMachineSubAuthorities`, `kSidAuthorityField*`,
  `kBitsPerByte`, `kMachineRidFirst`, `kFileSizeHighShift`). The whole-tree run
  (lizard, magic-numbers, clang-format, and the other static-gates-job scripts) is now
  green against this tree.
- **Elevated VHDX rig case: INCOMPLETE**, and therefore the hardware claims stay uncertified
  (`docs/RELEASE_READINESS.md` rule). `mountAndUnmount_realVolumeWithoutLetter` now proves
  the mount and unmount scripts on a real volume (a fresh VHDX with one NTFS partition and no
  drive letter); a VHDX holding a real Windows installation, BitLocker-locked or not, is
  still the rig case.
- **BitLocker unlock: proven once by hand, not by a committed test.** The original
  workstation is Windows 10 Home 25H2 (`EditionID: Core`), which cannot BitLocker-protect a
  fixed VHDX. The PR #12 review fixes were run on Windows Server 2022 with the BitLocker
  feature installed: a VHDX volume was encrypted with a recovery password, locked, and had
  its drive letter removed. A one-off local test (not committed: it needs that prepared
  disk) drove the product path on it: the scan reported the volume unreadable, a wrong
  recovery key was refused and the volume stayed locked, the right key unlocked it through
  `unlockEncryptedVolume` with the key staged by `StagedScriptCredentials`, the re-read
  volume was readable, a canary file was read back through the product's own GUID-path
  mount, and no file in the session temp folder held the key afterwards. That run found that
  `Unlock-BitLocker` reports a wrong key as a non-terminating error and returns, so the
  unlock script now checks `LockStatus` afterwards and refuses. The committed tests pin
  that refusal against a stand-in; a locked VHDX holding a real Windows installation is
  still the rig case.
- **Manual-validation run, 2026-11-23: the hive path was never exercised anywhere, and
  three genuine defects came out of it.** The wizard's Rescan on a real mounted fake
  offline volume returned nothing. The cause was environmental first: the elevated token's
  device map held a stale `G:` symlink (it resolved to `\Device\HarddiskVolume7`, the
  MSFT-reserved partition with no filesystem, left there by earlier VHDX attach/detach
  cycles, while the medium-IL view of `G:` resolved to the live volume), so the elevated
  scan saw a volume medium-IL code called readable. `G:` was removed, the reserved
  partition deleted from the fake disk, and the volume remounted at `H:` -- the stale
  device-map entry only clears on sign-out or reboot. With the volume reachable, an
  elevated end-to-end test then exposed three real defects, each proven by its failure
  before the fix:
  - `kLoadRegistryPrivilegeName` named `SeLoadRegistryPrivilege`, a privilege that does not
    exist in Windows: `LookupPrivilegeValueW` failed with `ERROR_NO_SUCH_PRIVILEGE` on every
    machine, so no offline hive was ever loaded anywhere and the folder fallback of that
    version masked it (the fallback is gone: unreadable hives now skip every folder). `RegLoadKey`/`RegUnloadKey` need `SeRestorePrivilege`
    (`SE_RESTORE_NAME`); the name and its comments are fixed.
  - `OfflineRegistryHive::readBinary` passed `nullptr` as `lpType` to the sizing
    `RegQueryValueExW` and then checked `type != REG_BINARY`, but `type` stayed 0 the whole
    time, so the function always returned empty: no offline SAM hive could ever yield a
    machine SID. It now passes `&type`.
  - The computer name was read through `CurrentControlSet`, which inside a loaded hive is a
    link node whose target is an absolute registry path: it resolves into the LIVE
    machine's namespace, not the loaded copy, so an offline PC would have been labeled with
    the scanning PC's own name. `computerNameFromSystemHive` now reads `Select\Default` ->
    `ControlSet00x`, with no guessed default.
- **The elevated hive test was claimed before but never existed; it does now.**
  `readOfflineHives_liveMachineHives` in `tests/unit/test_offline_profile_source.cpp` is the
  end-to-end proof of the whole load path (privilege lookup and enable, `RegLoadKey`,
  ProfileList, computer name, SAM machine SID). The live hives cannot be reloaded (the
  kernel holds them), so it saves copies with `reg save`. (That first version synthesized
  a SAM `Domains\Account` "F" value; review showed the real machine SID is the trailing
  24 bytes of "V", so the test now saves the live SAM itself and compares the parsed SID
  with the LSA's account-domain SID -- see "Review fixes" below.) It
  QSKIPs without elevation and passes elevated, with the machine SID and the current
  user's ProfileList row read back from the saved copies.
- **The elevated test is registered where CI can prove it ran.** `.github/workflows/ci.yml`
  gained an elevated step in the shard that plans `test_offline_profile_source` (runs the
  whole suite with the runner's elevated token, requires the case's own PASS line, uploads
  `build/elevated-proof/test_offline_profile_source.txt`) and the "Every
  must-run-elevated test ran elevated" list. `tests/skip_site_inventory.txt` and
  `tests/skip_baseline.txt` gained the bounded reason for its unelevated skip;
  `check_test_skips.ps1 -StaticOnly` passes (15 files, 33 sites).
- **The fake fixture's hives were synthesized, not saved from Windows.** A throwaway
  elevated tool (kept outside the tree) wrote SOFTWARE/SYSTEM/SAM for a fake volume, and its
  SAM carried a synthesized "F" value. That fixture predates the "V" fix below and proves
  nothing about reading a real installation; the elevated test's SAM, SYSTEM and SOFTWARE,
  saved from the live machine at test time, are the evidence for the hive path.
- **Gate findings fixed on the way** (each by its proper route, no behavior change):
  lizard rejected the new test at 128 lines (extracted `processIsElevated`,
  `saveLiveTreeAsHive`, and a `writeSamShapedHive` since removed with the synthesized SAM) and, in the same pass,
  `sak::backupOfflineProfiles` at CCN 11 (extracted `includedOfflineProfiles` and
  `withOfflineFields`); cppcheck's `uninitMemberVarNoCtor` on the two request structs was
  answered honestly -- `timeout_ms` got a member initializer, and the reference members
  carry the repository's documented-bound suppression ("reference member, always bound",
  the same wording `ai_store_file_loader.cpp` uses) because a reference cannot have an
  initializer. The changed-file hooks, the manual-stage whole-tree gates and the full
  Release build are green after these.
- **Local tooling gap repaired on this workstation.** `gitleaks`, `cppcheck` and `rg` were
  absent from PATH, so three hooks could not run at all (each failed closed with "tool
  missing"); cppcheck 2.22.0, gitleaks and ripgrep are now installed and the hooks execute.
  The local full skip-log audit still reports the usual hardware-guard artifacts of a
  non-CI machine (its logs are local runs); the static inventory, which is the part that
  fails without a correct registration, passes.
- **Review fixes (PR #12 review).** The SAM-shaped test hive above was synthesized, and the
  parser matched the synthesis rather than Windows: the machine SID is now read from the
  `SAM\Domains\Account` value `V` (its last 24 bytes) through a REG_OPTION_BACKUP_RESTORE
  open under SeBackupPrivilege, and the elevated test reads SAM, SYSTEM and SOFTWARE hives
  saved from the live machine at test time and compares the SID with the live account
  domain's. An unreadable SAM or ProfileList now skips every folder (fail closed). Volume
  GUID paths get their trailing backslash for `mountvol`, the unmount is `mountvol /d` (the
  volume stays online), `Select\Default` is read as a REG_DWORD (`ControlSet001`),
  `restore_user_profiles` is destructive, profile copies use the long-running timeout,
  volume requests match exactly, and the wizard page scans, prepares and unlocks off the GUI
  thread, reusing a live mount on a second Next. An `ERROR_NOT_READY` drive is reported as
  not ready (an empty card reader or DVD drive), not as probably encrypted. The elevated
  `mountAndUnmount_realVolumeWithoutLetter` attaches a VHDX whose volume has no drive
  letter, mounts it through its `\\?\Volume{GUID}\` path, and checks the volume still
  answers after the unmount. `RegLoadAppKey` was evaluated for private
  hive loads and refused real saved hives with ERROR_BADDB (1009), so `RegLoadKey` stays.
- **The test PowerShell runner ran each statement on its own.** `runPowerShellScript` in
  `test_offline_profile_source.cpp` read its script from standard input, so a Stop error
  ended one statement and the next still ran (the `AGENTS.md` rule on script scope). It
  passed over a failed `diskpart format` in the VHDX test: diskpart answers "Incorrect
  function" to `format` when started from that test binary (run by hand, the same script
  formats), and the runner went on to the next statement. The runner now passes one `-EncodedCommand`, so a failure
  ends the script with an error, and the VHDX test formats with `Format-Volume`. The same
  real run showed `GetVolumePathNamesForVolumeNameW` refusing a volume name without its
  trailing backslash (ERROR_INVALID_NAME); `queryMountPaths` now adds it.

## Estimate

- Phase 1+2 (scanner + hive + action seam + tests): the real work; roughly the size of the
  offline-image recovery subsystem, which exists as a precedent.
- Phase 3 (wizard source mode): a new page plus wiring; small relative to the scanner.
- Phase 4 (restore honesty): wording and one unresolved-SID display; small.
- Certification: one elevated VHDX rig session, uncertified until it runs -- label hardware
  claims accordingly per RELEASE_READINESS.md.

## Cross-cutting notes

- ASCII-only doc; three-part version; portable state in QSettings ini (`config_manager.cpp`),
  no registry for S.A.K.'s own options.
- No new background work at startup; the offline scan runs only when the user opens this mode.
- All file:line citations re-openable; this doc depends on nothing under docs/ (`AGENTS.md`
  rule).
