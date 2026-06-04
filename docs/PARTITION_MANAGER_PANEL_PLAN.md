# Partition Manager Panel - Comprehensive Implementation Plan

**Version**: 1.0
**Date**: June 1, 2026
**Status**: V1 baseline and Windows-supported commercial parity operations implemented in local tree; current strict VHD plus VM/hardware/lab claim level is `HardwareCertified`
**Target placement**: New top-level panel before Image Flasher
**Plan completion**: V1 implementation complete for non-destructive automated gates

---

## Executive Summary

The Partition Manager Panel will add a modern disk and partition workspace to S.A.K. Utility. The panel will focus on technician-safe Windows disk management: read-only inventory first, visual partition maps, queued operations, explicit preflight checks, and elevated execution only when the user applies a reviewed plan.

The design should feel closer to MiniTool Partition Wizard or AOMEI Partition Assistant than to Windows Disk Management: clear disk maps, actionable side panels, pending operation review, health signals, clone/migration wizards, and strong safety feedback. The implementation must reuse S.A.K.'s existing drive scanner, SMART analyzer, benchmark workers, image-flashing raw I/O patterns, elevated task dispatcher, process runner, and quick-action disk repair helpers where possible.

## Required Feature Set

- [x] **Basic partition operations** - Create, delete, format, label, assign/remove drive letter.
- [x] **Resize operations** - Shrink and extend supported volumes with exact size preview.
- [x] **MBR/GPT conversion** - Safe system-disk MBR-to-GPT through `mbr2gpt.exe`; destructive empty-disk conversions through Windows Storage APIs.
- [x] **Partition merge and split** - Composite operations with data-safety limits clearly shown.
- [x] **File system conversion** - FAT/FAT32 to NTFS where supported; destructive recreate flow for unsupported conversions.
- [x] **Disk cloning and imaging** - Disk-to-disk clone, partition clone, image backup, image restore, and verification.
- [x] **OS migration tools** - Guided system-disk migration to SSD/HDD with boot validation and recovery steps.
- [x] **Boot repair utilities** - EFI/BCD repair, WinRE status repair, active partition checks, and volume repair reuse.
- [x] **S.M.A.R.T. monitoring** - Existing smartmontools-backed drive health integrated into the disk map.
- [x] **SSD optimization** - TRIM status, ReTrim, alignment check, and SSD health recommendations.
- [x] **Secure data wiping** - Partition wipe, free-space wipe, disk wipe, and SSD-aware wipe warnings.

### Implementation Status - Current As Of 2026-06-02 UTC / 2026-06-02 PDT

- Added `PartitionManagerPanel` before Image Flasher with an AOMEI/MiniTool-style ribbon, left Actions and Wizards pane, partition table, proportional disk map, bottom legend, pending queue, undo/redo/discard, dry-run, apply, log toggle, Icons8 operation icons, and accessibility labels.
- Added storage inventory parsing, layout hashing, unallocated-region discovery, system/boot/EFI/MSR/recovery classification, storage reliability counters, BitLocker/dirty-bit collection, and read-only unsupported-state flags.
- Added planner, queue, safety validator, script builder, executor, controller, reports, elevated feature metadata, icon resources, CMake wiring, and main-window tab wiring.
- Implemented create/delete/format/drive-letter/label/resize, adjacent merge, split, MBR/GPT conversion with MBR2GPT system mode, FAT/FAT32-to-NTFS conversion, clone/image/restore/OS-migration copy plans, copy partition, read-only partition recovery scan, recovered-partition write-back candidates, file-system check, surface test, hide/unhide, active/inactive, partition type ID, initialize disk, delete all partitions, boot repair, SSD ReTrim, free-space wipe, partition wipe, and non-system disk wipe script generation. Create Image is classified as read-only source risk and safety validation blocks raw-device destinations or image files saved on the selected source disk/partition.
- Added source/target/options/review wizard flows for Copy Disk, Copy Partition, Partition Recovery, and Migrate OS; disk and OS copy plans include a graphical source/target layout preview, keep/fit copy behavior, source/target sizing, known undersized-target blockers, explicit target overwrite confirmation before Apply, exact-byte clone scripts with sample/full byte verification, and post-copy OS migration boot-validation output captured by the operation report. Copy Partition includes a visual unallocated-region picker, target size/offset preview, offset-aware verification, undersized-region blockers, overwrite confirmation for raw-device/region writes, and payload target-disk safety validation. Partition Recovery can queue a candidate write-back from offset, size, and partition type ID; Apply requires acknowledgement, non-overlap, disk-bound, and target disk safety checks.
- Added target-identity Add-to-Queue dialogs for create, format, resize, and MovePartition, including direct draggable preview-bar handles, synchronized slider/numeric controls, graphical create/resize/move size previews, warning text, free-space-before placement, partition-type/label/file-system/cluster-size/drive-letter/full-format controls, and script payload handling. Create payloads validate Windows-supported GPT/MBR type choices, type/file-system compatibility, and selected unallocated-region size/offset bounds before Apply. Create scripts carry selected-region placement into `New-Partition -Offset`. Create and Format payloads validate supported allocation-unit sizes before script execution and use `Format-Volume -AllocationUnitSize` for explicit cluster-size choices. Extend Partition Wizard routes adjacent-free-space extension through queued resize. Move/Resize exposes adjacent resize, start-move, and donor-space modes; start-move queues `MovePartition` backup/delete/recreate-at-offset/restore/hash-verify execution, and donor-space extension routes through the adjacent-donor Allocate Free Space engine. Explore opens mounted partitions. Properties opens a copyable details modal for disks, partitions, volumes, SMART counters, flags, and unallocated regions.
- Added visible commercial-parity coverage for AOMEI/MiniTool utility actions: Quick Partition queues same-style equal-size or custom size/label full-disk layouts for non-system basic disks with saved presets, GPT support up to 9 partitions, and an MBR-safe four-data-partition cap with Windows extended/logical container behavior recorded during certification; Extend Partition Wizard routes adjacent-free-space extension through queued resize; Allocate Free Space now queues an immediately adjacent mounted donor backup/delete, target extend, donor recreate, restore, SHA-256 manifest comparison, and repair scan path with off-target/off-donor backup validation; Allocate Free Space To queues adjacent unallocated space by either resizing the previous partition online or moving the following partition backward through `MovePartition`; Data Recovery scans image files or raw volume/device paths read-only for PNG, JPEG, and PDF signatures, reviews recoverable candidates, restores to a separate destination, and verifies the source hash stays unchanged; Space Analyzer runs a read-only async mounted-volume usage scan with Tree View, Largest Files, and File Types tabs plus safe right-click Open, Explore, and Copy Path actions; Disk Benchmark launches the existing Benchmark and Diagnostics panel; Manage BitLocker opens an in-app BitLocker status dialog with copyable `manage-bde`/PowerShell command guidance and Windows management launch; Disk Defrag opens an in-app optimization dialog with media-aware analyze/defrag/ReTrim command guidance, only queues HDD defrag when media is reported as HDD, blocks SSD/NVMe defrag in core/script guards, and runs through cancellable elevated Apply; Change Cluster Size queues a destructive backup, reformat with explicit allocation unit, restore, and SHA-256 manifest verification path with off-volume backup validation; primary/logical conversion, volume serial-number changes, and one-volume dynamic-to-basic conversion queue destructive backup/rebuild-or-reformat/restore/verify scripts; SSD Secure Erase opens an in-app readiness dialog, requires typed queue confirmation, runs Windows ReTrim where mounted volumes exist, and queues the existing clear-level disk wipe through Apply; Make Bootable Media launches Image Flasher.
- Added final Apply review with operation count, risk tiers, warnings/blockers, visual before/after disk-map diff, and layout-hash evidence before elevated execution.
- Research audit compared the feature matrix against current AOMEI/MiniTool workflows plus Microsoft/NIST storage guidance, then closed production gaps for live BitLocker/dirty-bit collection, storage reliability inventory, unsupported file-system conversion preflight, split/merge/create bounds validation, Apply disablement when blockers exist, partition-identity formatting, selected-target guards, and clear-level non-system disk wipe scripts with SSD purge warnings.
- Removed menu-bar creation, fixed the left Actions and Wizards pane width, changed Wizards and Partition Operations entries to compact icon text links, moved scrolling inside the Partition Operations group above the visible Pending Operations queue, removed the redundant sidebar layout-preview box, kept the before/after layout diff in the final Apply review, removed row-number gutters and table cell borders while adding disk separators to match the AOMEI partition list, removed the extra disk-map frame, tightened disk-map padding to a 1 px gutter, rounded the disk row/tile container corners, added GPT/Primary, Logical, Simple, Spanned, Striped, Mirrored, RAID5, and Unallocated disk-map legend colors, verified those legend colors against rendered segment roles, moved type color to the inner usage bar while keeping the outer segment shell neutral, limited disk-map highlight to selected targets, synchronized mouse/keyboard disk-map segment selection with table selection in both directions, made disk tiles and partition segments the same height, kept Redo disabled until Undo creates a redoable action, used fixed-size Icons8 SVG ribbon buttons with dedicated disabled SVG mode mappings and a dedicated Dry Run icon, made the color-coded rounded disk map resizable upward into table space, and moved inventory refresh to lazy first-display async execution so adding the panel does not lengthen application startup.
- Automated verification passed with `test_partition_manager_core`, release readiness, full CTest, and a live read-only inventory-script smoke test. Release readiness runs Partition Manager certification-matrix integrity, commercial/destructive feature mapping, external checklist/lab package, VHD preflight, certification gap/bundle, feature matrix, release-claim, strict handoff, evidence-payload, and certification-tool self-tests without mutating disks by default. Strict elevated VHD execution is 12/12 passed and all 18 external VM/hardware/lab evidence gates are imported, so strict hardware handoff now reports `HardwareCertified`.
- Latest full local automated proof completed before the v0.9.1.9 release push on 2026-06-04 PDT: Release build passed and full `ctest --test-dir build -C Release --output-on-failure` passed 133/133. Current strict hardware proof imports all 18 external VM/hardware/lab gates from `docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`, keeps the 12/12 disposable-VHD proof under `artifacts\partition-manager-certification\vhd-strict\`, and reports `HardwareCertified` through the strict hardware handoff using `artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json` plus `external-evidence.imported.checklist.md`.
- Current external evidence proof completed on 2026-06-03 PDT / 2026-06-03 UTC: strict elevated disposable-VHD certification remains 12/12 passed with report `artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\partition-manager-certification-report.json`, all 18/18 external VM/hardware/lab gates are imported in `artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`, and `scripts\run_partition_manager_hardware_certification_strict.ps1 -CertificationRoot artifacts\partition-manager-certification\vhd-strict -ExternalEvidenceManifest artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json -ExternalEvidenceChecklist artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md -ExternalEvidenceRoot artifacts\partition-manager-certification\vm-lab\external-evidence` passed with claim level `HardwareCertified`.
- Live Windows 11 VirtualBox VM proof completed on 2026-06-02 PDT using VirtualBox 7.2.8 and VM `SAK-PM-Lab-Win11` with Windows 11 25H2, one 80 GB system disk, and two 4 GB RAW data disks. The app started successfully from the read-only VirtualBox shared repo path `\\vboxsvr\sakrepo\build\Release\sak_utility.exe` with `SAK_STARTUP_SMOKE_OK`, and the visible Partition Manager UI showed 3 disks from that shared executable in `artifacts\partition-manager-certification\vm-lab\sak-utility-share-partition-manager-mouse-fixed.png`. The VM run closed two real defects before claiming proof: portable runtime paths now fall back to a writable data root when the executable directory is not writable, and Storage inventory no longer aborts when `Get-Partition` returns no partitions for RAW disks. The guest account is still non-admin, so in-guest destructive mutation is not claimed from this VM session; strict VHD evidence remains the elevated 12/12 host run, and hardware certification is completed through the imported 18/18 external gate matrix.
- `external.system-mbr2gpt` is now passed through disposable VirtualBox BIOS/MBR fixture `SAK-PM-BIOS-MBR-Fixture-20260603-091138`: `mbr2gpt /validate /disk:0 /allowFullOS` and `/convert /disk:0 /allowFullOS` both exited 0, the disk changed from MBR to GPT with an EFI System partition, VirtualBox UEFI boot files were normalized offline with `bcdboot`, and the same converted disk booted in fresh EFI VM `SAK-PM-MBR2GPT-EFI-Boot-20260603-1140` to `GuestAdditionsRunLevel=3`. Evidence: `artifacts\partition-manager-certification\vm-lab\external-evidence\external.system-mbr2gpt\report.json`.
- Current AOMEI/MiniTool parity conclusion: S.A.K. is code-complete for the Windows-supported commercial-parity surface documented here, including direct execution paths for MovePartition start moves, adjacent unallocated allocation, primary/logical conversion, volume serial-number regeneration, and one-volume dynamic-to-basic conversion. It is not full execution parity with every current AOMEI Partition Assistant Professional or MiniTool Partition Wizard capability because arbitrary multi-partition reshuffles, vendor ATA/NVMe purge commands, and unsupported file-system conversions remain outside the shipped safety scope. Official current product pages still advertise dynamic/basic conversion, primary/logical conversion, cluster-size changes without formatting, move-location/donor-space behavior, deep live-drive data recovery, SSD secure erase, BitLocker encryption/mutation, bootable/recovery media, disk health/benchmark/space-analyzer utilities, and broad file-system formatting. S.A.K. matches or exposes many of these workflows with queued Apply, safety validation, and backup/restore verification where destructive mutation is required. Hardware certification for the shipped destructive/action scope is complete: Allocate Free Space, cluster-size, HDD defrag, file recovery, BitLocker mutation, BIOS/UEFI boot repair, system MBR-to-GPT, removable USB, SSD/NVMe, SSD secure erase, partition move, primary/logical conversion, volume serial-number, dynamic-to-basic conversion, OS migration reboot, and hardware wipe evidence are all imported in the 18/18 external matrix.
- Added `scripts/check_partition_manager_feature_matrix.ps1` as a release-readiness gate. It verifies 12 commercial-parity groups against implementation, UI, tests, documentation, `docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`, and certification gates so feature claims cannot drift from code.
- Added a shared certification matrix with required evidence keys and safety contracts. The VHD harness stamps those contracts into reports, the external-evidence JSON scaffold and Markdown checklist include them for lab runs, the verifier rejects passed VHD or external scenarios without matrix-required non-empty evidence payload values, and the claim-level script counts only matrix-complete non-empty evidence.
- Added `scripts/check_partition_manager_certification_matrix_integrity.ps1` and `scripts/check_partition_manager_external_checklist.ps1` as release-readiness gates. They verify unique `vhd.*` and `external.*` IDs, required evidence keys, required value contracts, safety contracts, VHD harness coverage, exact certification-doc coverage, checklist-verifier support, and generated external checklist coverage for all matrix-backed lab gates.
- Added `scripts/check_partition_manager_release_claims.ps1` so release-facing docs stay tied to the generated certification claim level, require completed-evidence wording at `HardwareCertified`, and reject stale blocker caveats after strict hardware evidence is complete.
- Added release-readiness switches for archived strict certification evidence. Default readiness stays non-mutating, while `-PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict -RequirePartitionVhdEvidence` verifies the strict elevated VHD report without replacing it with a plan-only run. Strict external evidence also requires `-PartitionExternalEvidenceChecklist` beside `-PartitionExternalEvidenceManifest` so hardware-certified wording cannot bypass lab-checklist verification.
- Added `scripts/update_partition_manager_external_manifest_from_reports.ps1` so completed external lab `report.json` files can be imported into the JSON manifest only after matching the certification matrix ID, safety contract, required evidence keys, required values, artifact path, and verification summary. The certification self-test now proves successful import plus bad-report rejection before strict hardware handoff can use an imported manifest.

### Official AOMEI/MiniTool Workflow Audit - 2026-06-01

Official AOMEI and MiniTool documentation shows that a familiar partition-manager panel is more than the table and disk map. It needs selection-aware right-click menus, wizard step flows, review modals, and advanced operations exposed in predictable places. The current S.A.K. implementation now backs the Windows-supported disk, partition, and unallocated context actions with queued operation generation, safety validation, script generation, and unit tests.

Research sources used:

- MiniTool Partition Wizard help center: `https://www.partitionwizard.com/help/`
- MiniTool Move/Resize workflow: `https://www.partitionwizard.com/help/resize-partition.html`
- MiniTool Copy Disk Wizard: `https://www.partitionwizard.com/help/copy-disk.html`
- MiniTool Migrate OS Wizard: `https://www.partitionwizard.com/help/migrate-os-to-ssd-hd.html`
- MiniTool Partition Recovery: `https://www.partitionwizard.com/help/partition-recovery.html`
- AOMEI Partition Assistant manual: `https://www.diskpart.com/help/`
- AOMEI main window and pending-operation model: `https://www.diskpart.com/help/partition-assistant-main-window.amp.html`
- AOMEI Extend Partition Wizard: `https://www.diskpart.com/help/extend-partition-wizard.html`
- AOMEI Migrate OS workflow: `https://www.diskpart.com/help/migrate-system-wizard.html`

| Area | Expected commercial behavior | Current code state | Required closure |
| --- | --- | --- | --- |
| Selection right-click menu | Disk, partition, and unallocated rows expose valid operations plus advanced tools with disabled/blocked states. | Added selection-aware table context menu with Apply/Dry Run/Undo/Discard, disk actions, partition actions, unallocated actions, Properties, queued operation-backed advanced entries, safety blocker tooltips/status text, and a copyable Properties modal for disk, partition, volume, SMART, flag, and free-space fields. | Keep the certification matrix synchronized with any newly enabled destructive operation. |
| Create/Format modals | Dialog collects size, location, file system, label, drive letter, cluster size, quick/full format, type, and preview. | Create and format use target-identity Add-to-Queue dialogs with direct draggable preview-bar handles plus synchronized slider/numeric controls, size/free-space-before placement, partition-type/file-system/cluster-size/label/drive-letter/full-format controls, script payload support, validated GPT/MBR create-type output, validated selected-region `New-Partition -Offset` placement, validated `Format-Volume -AllocationUnitSize` output, and graphical create-size/free-space visualization. | Add visual handle affordance polish if operator review requests it. |
| Resize/Move/Extend | Wizard supports drag handles, exact before/after sizes, adjacent-space checks, and extend-from-free-space or donor-volume modes. | Resize uses a target-identity Add-to-Queue dialog with a direct draggable target-size preview handle, synchronized slider/numeric controls, exact target size, mode selector, donor selector, contiguous-free-space-after limits, current/target/max-online preview rows, no-op and shrink-below-used-space blockers, and queued map updates that consume or create free spans. Extend Partition Wizard opens this queued path when adjacent free space exists. Allocate Free Space supports the adjacent-donor case by backing up the donor, deleting it, extending the target, recreating the donor at the reduced size, restoring files, comparing SHA-256 manifests, and repair-scanning both volumes. Move partition start queues `MovePartition` backup/delete/recreate-at-offset/restore/verify execution. Allocate Free Space To on unallocated rows queues online resize for previous-adjacent targets or MovePartition for following-adjacent targets. Legacy Resize payloads with hidden start-offset/donor fields remain blocked. | Broaden future multi-partition layout reshuffle only if a dedicated engine can preserve every intervening partition with manifest verification. |
| Migrate OS wizard | Steps cover source system detection, target disk, copy options, target layout preview, 1 MB SSD alignment, target GPT/MBR choice, boot-order instructions, and final review. | OS migration uses a source/target/options/review wizard with target disk picker, graphical source/target layout preview, keep/fit layout behavior, sample/full verification, source/target size preview, undersized-target blocker, 1 MB alignment, GPT boot preference, explicit target overwrite confirmation, exact-byte clone verification script generation, and post-copy boot validation output for target partition scheme, EFI/active partition checks, and firmware boot-order guidance. | External VM proof is imported; live target writes still require explicit overwrite confirmation and safe direct execution path. |
| Copy Disk wizard | Steps cover source disk, target disk, copy options, fit/keep/edit partitions, alignment, target wipe confirmation, and review. | Copy Disk uses a source/target/options/review wizard with target disk picker, graphical source/target layout preview, keep/fit layout behavior, sample/full verification, source/target size preview, undersized-target blocker, 1 MB alignment, explicit target overwrite confirmation, and exact-byte clone verification script generation. | External proof is imported; live disk-to-disk execution stays guarded by target overwrite confirmation and runtime safety checks. |
| Copy Partition wizard | Steps cover source partition, unallocated/target selection, sizing/location, and review. | Copy Partition uses a source/target/options/review wizard with image/custom path targeting, visual unallocated-region picker, target size/offset preview, offset-aware `ClonePartition` verification, undersized-region blockers, raw-device/region overwrite confirmation, and payload target-disk safety validation. | Raw-region partition-clone proof is covered by strict VHD certification; live-device writes stay behind explicit target confirmation, target identity checks, and script-level raw-write safeguards. |
| Partition Recovery wizard | Steps cover disk/range selection, quick/full scan, found-partition list, preview, select recovered partitions, and apply. | Partition Recovery uses a source/scan/candidate/review wizard, queues a read-only quick/full raw-disk signature scan that reports candidate NTFS/FAT/exFAT boot sectors, and can queue a restore candidate from offset, size, and type ID. | Recovered-partition write-back proof is covered by strict VHD certification; live-device recovery runs through target identity, acknowledgement, overlap, and disk-bound checks. |
| Advanced partition tools | Hide/unhide, set active/inactive, primary/logical conversion, cluster size, partition type ID, serial number, surface test, check file system, Explore, and properties. | Added queued operations and script tests for file-system check, surface test, hide/unhide, active/inactive, partition type ID, Explore, and a copyable Properties modal. Existing-volume cluster-size change queues off-volume backup, explicit allocation-unit reformat, restore, SHA-256 verification, ACL/alternate-stream preservation checks in external evidence, and repair scan. Primary/logical conversion now backs up a single-volume MBR data disk, rebuilds as primary or logical, restores, hash-verifies, and repair-scans. Volume serial-number change backs up, reformats to regenerate serial, restores, hash-verifies, and repair-scans. | Keep destructive metadata operations limited to mounted data volumes with off-volume backup and explicit confirmation. |
| Commercial utility parity | AOMEI/MiniTool expose Quick Partition, Data Recovery, Allocate Free Space/Extend Wizard, Disk Benchmark, Space Analyzer, Disk Defrag, SSD Secure Erase, BitLocker management, and bootable media utilities. | Quick Partition queues same-style equal-size or custom size/label full-disk layouts for non-system basic disks, persists GPT layouts up to 9 partitions, and caps MBR at four data partitions with extended/logical container behavior recorded in destructive VHD evidence. Extend Partition Wizard queues adjacent free-space growth through resize. Allocate Free Space queues adjacent donor backup/delete/extend/recreate/restore with SHA-256 verification. Allocate Free Space To queues adjacent unallocated space through Resize or MovePartition. Partition Recovery covers partition-table recovery. Data Recovery supports read-only image and raw volume/device file carving for PNG, JPEG, and PDF signatures with candidate review, separate restore destination, and source-hash verification. Space Analyzer provides a read-only mounted-volume usage scan with Tree View, Largest Files, and File Types tabs plus safe right-click Open, Explore, and Copy Path actions. Disk Benchmark opens the existing Benchmark panel; Manage BitLocker shows BitLocker status, lock/protection state, copyable external commands, Windows management launch, and imported BitLocker mutation evidence. Disk Defrag shows media-aware defrag/ReTrim command guidance, queues direct HDD defrag only when media is reported as HDD, blocks SSD/NVMe in UI/core/script guards, and routes Apply through cancellable elevated execution; external HDD defrag proof passed on disposable VirtualBox HDD media. SSD Secure Erase shows disk identity and SSD/NVMe classification, then queues typed-confirmed Windows ReTrim plus clear-level disk wipe for non-system SSD/NVMe disks; Make Bootable Media routes to Image Flasher. Primary/logical, serial-number, and dynamic-to-basic operations now have direct backup/restore execution paths. | Keep external certification evidence in lockstep with each destructive workflow. |
| Disk tools | Initialize MBR/GPT, delete all partitions, dynamic disk/basic conversion, surface test, rebuild MBR, wipe disk, SSD optimization, SMART. | Added queued operations and tests for initialize disk, delete all partitions, and disk surface/health scan; existing code covers empty-disk MBR/GPT, wipe, ReTrim, boot repair, and SMART inventory. RAW basic disks are parsed as initializable unallocated media instead of dynamic disks, so disposable RAW data disks used in VM/VHD proof stay actionable. Dynamic-to-basic conversion supports one mounted simple dynamic volume by backing it up, deleting the volume, converting the disk to basic, recreating a primary partition, restoring, hash-verifying, and repair-scanning. | Multi-volume dynamic disks remain blocked unless a future engine can preserve every volume layout and file manifest. |
| Pending operations | Operations queue visible, Apply is explicit, final confirmation reviews changed layout. | Pending queue, layout hash guard, dry-run, apply, undo/redo/discard, and final Apply review with risks, visual before/after disk-map diff, and layout hash exist. | Strict VHD and external evidence prove the certified destructive-operation preview/report paths; keep future operations tied to before/after reports. |
| Disk map and legend | Bottom map behaves like familiar partition-manager maps: type colors are meaningful, legend matches rendered colors, map/table selection stays synchronized, and row contents align. | Disk-map segments expose GPT/Primary, Logical, Simple, Spanned, Striped, Mirrored, RAID5, and Unallocated roles; tests verify every legend swatch matches the rendered inner usage-bar color for that role. Segment shells remain neutral except selected targets, disk tiles and partition segments share the same height, and mouse/keyboard activation on map segments selects the matching table row while table selection updates map highlights. | Keep any new partition role mapped in `partitionColorRole()`, the bottom legend, and the legend-color test before exposing it in the UI. |

### Completion Decisions

These decisions close the open scope gaps so implementation can start without another planning pass:

- First implementation targets Windows basic disks only. Dynamic disks and Storage Spaces are visible read-only with unsupported badges.
- System disks are read-only for create/delete/format/resize/merge/split/wipe in v1. Allowed system-disk actions are inventory, SMART, report, `mbr2gpt` validation/conversion, OS migration clone planning, and boot repair.
- Data disks get mutating actions first: create, delete, format, drive letter, label, shrink, extend, split, empty-disk MBR/GPT conversion, partition wipe, disk wipe.
- Clone and image v1 uses raw sector copy/image with full or sampled verification. Used-space-only clone is later work.
- OS migration v1 clones full boot chain to another disk and then validates or repairs boot files. It does not resize or delete current OS partitions online.
- SSD Secure Erase queues Windows ReTrim plus clear-level non-system disk wipe through Apply with typed confirmation. Device-specific ATA Secure Erase or NVMe Format/Sanitize remains out of scope unless a future hardware-specific engine is added.
- Partition move is not promised in v1. Merge only supports cases that can be represented as safe shrink/create/copy/delete/extend operations.

### Feature Support Matrix

| Feature | V1 support | Backend | Existing reuse | Risk tier |
| --- | --- | --- | --- | --- |
| Disk/partition inventory | Full | `DriveScanner`, PowerShell Storage JSON | `DriveScanner`, `diagnostic_types` | Read-only |
| SMART monitoring | Full | bundled `smartctl.exe` | `SmartDiskAnalyzer` | Read-only |
| Create partition | Data disks only | `New-Partition`, `Format-Volume` | `ProcessRunner`, elevated helper | Destructive |
| Delete partition | Data disks only | `Remove-Partition`, diskpart fallback | elevated helper | Destructive |
| Format partition | Data partitions only | `Format-Volume` | elevated helper | Destructive |
| Assign/remove drive letter | Data volumes only | `Set-Partition` | elevated helper | Low |
| Check file system | Mounted volumes | `Repair-Volume -Scan` | `CheckDiskErrorsAction` patterns | Read-only |
| Surface test | Disk/partition scan | `chkdsk /scan`, reliability counters | `ProcessRunner`, SMART inventory | Read-only |
| Hide/unhide partition | Data partitions only | `Set-Partition -IsHidden` | elevated helper | Low |
| Set active/inactive | MBR data partitions only | `Set-Partition -IsActive` | elevated helper | System-critical |
| Partition type ID | Data partitions only | `Set-Partition -GptType/-MbrType` | elevated helper | System-critical |
| Initialize disk | Empty/raw data disk only | `Initialize-Disk` | elevated helper | Destructive |
| Delete all partitions | Data disk only | `Remove-Partition` loop | elevated helper | Destructive |
| Shrink/extend | Data partitions only | `Get-PartitionSupportedSize`, `Resize-Partition` | `ProcessRunner` | Destructive |
| MBR to GPT | System disk via `mbr2gpt`; empty data disk via Storage cmdlets | `mbr2gpt.exe`, `Initialize-Disk` | elevated helper | System-critical |
| GPT to MBR | Empty data disk only | `Initialize-Disk` after clear | elevated helper | Destructive |
| Merge/split | Adjacent data partitions only | composite plan | planner/queue | Destructive |
| File system conversion | FAT/FAT32 to NTFS only in-place | `convert.exe` | `ProcessRunner` | Destructive |
| Clone/image | Raw disk/partition copy first | new clone/image workers | `FlashWorker`, `ImageSource`, `DriveUnmounter` | Destructive |
| Partition recovery scan | Read-only candidate scan | raw disk boot-sector signature scan | `ProcessRunner`, report pipeline | Read-only |
| Recovered partition restore | Acknowledged candidate write-back | `New-Partition -Offset -Size` with MBR/GPT type ID | `ProcessRunner`, safety validator, report pipeline | System-critical |
| OS migration | External target disk | clone worker, `bcdboot` | `WindowsUSBCreator` boot patterns | System-critical |
| Boot repair | UEFI/BIOS repair helpers | `bcdboot`, `reagentc`, `Repair-Volume` | `CheckDiskErrorsAction`, `WindowsUSBCreator` | System-critical |
| SSD optimization | TRIM, ReTrim, alignment | `Optimize-Volume`, `fsutil` | `ProcessRunner` | Low |
| Secure wipe | Free-space, partition, disk wipe | `cipher /w`, full format, `Clear-Disk`, `clean all` | elevated helper | Destructive |

---

## Scope

### In Scope

- Windows 10/11 first, matching current S.A.K. platform direction.
- Basic disks, GPT and MBR, NTFS, FAT32, exFAT, ReFS read-only status where supported by Windows.
- Physical disks, partitions, volumes, mount points, BitLocker state, system/boot/EFI/recovery flags.
- Read-only inventory without admin when possible.
- Operation preview and queued apply model for all mutating actions.
- Elevated helper execution for destructive and privileged tasks.
- HTML/JSON report export for disk layout, health, and operation history.
- VM-backed test matrix for destructive operations.

### Out Of Scope For First Release

- Dynamic disks, Storage Spaces mutation, hardware RAID management, Linux file system mutation, Apple APFS/HFS+ partition editing.
- Moving partition starts on live Windows volumes unless a safe backend is added. Windows built-ins can shrink/extend but do not provide full third-party-style move support.
- Online OS migration without reboot or offline workflow for locked system volumes.
- Bypassing BitLocker, firmware locks, or device security controls.

---

## Existing S.A.K. Support To Reuse

| Existing area | Current files | Partition Manager use |
| --- | --- | --- |
| Drive discovery | `include/sak/drive_scanner.h`, `src/core/drive_scanner.cpp` | Physical disk list, system-drive detection, removable/read-only flags, bus type, hot-plug refresh. |
| Hardware storage models | `include/sak/diagnostic_types.h` | Seed `DiskInfo`, `PartitionInfo`, `VolumeInfo`, health summaries, and report payloads. |
| SMART health | `include/sak/smart_disk_analyzer.h`, `src/core/smart_disk_analyzer.cpp` | Drive health cards, warnings before destructive operations, clone/migration risk scoring. |
| Disk benchmark | `include/sak/disk_benchmark_worker.h` | Optional performance evidence before/after SSD optimization or migration. |
| Raw disk write patterns | `include/sak/flash_worker.h`, `src/gui/image_flasher_panel.cpp` | Progress, cancellation, verification, sector alignment, raw device safety patterns. |
| Image source support | `include/sak/image_source.h` | Disk image import/export, compressed image reading, checksum patterns. |
| Drive unmount/lock | `include/sak/drive_unmounter.h` | Exclusive access preflight for clone, wipe, restore, and offline partition tasks. |
| Windows USB creation | `include/sak/windows_usb_creator.h`, `src/core/windows_usb_creator.cpp` | Existing `diskpart`, formatting, partition active checks, `bcdboot` patterns. |
| Disk repair quick action | `include/sak/actions/check_disk_errors_action.h`, `src/actions/check_disk_errors_action.cpp` | Reuse `Repair-Volume` scan/offline-fix approach in boot repair and preflight. |
| Elevated execution | `include/sak/elevated_task_dispatcher.h`, `src/elevated/elevated_helper_main.cpp` | Allowlisted partition tasks with progress, cancellation, and JSON results. |
| Elevation metadata | `include/sak/elevation_tier.h` | Add Partition Manager feature IDs and precise admin reasons. |
| Process wrapper | `include/sak/process_runner.h`, `src/core/process_runner.cpp` | Run PowerShell Storage cmdlets, `mbr2gpt.exe`, `bcdboot.exe`, `reagentc.exe`, `diskpart.exe`. |
| UI constants/helpers | `include/sak/layout_constants.h`, `include/sak/style_constants.h`, shared GUI helpers | Dense, accessible, responsive layout matching current Windows 11 theme. |

---

## Product Model

### Primary User Stories

1. Technician opens Partition Manager, immediately sees all disks, health, partition style, free space, and boot/system markers.
2. Technician selects a disk or partition and receives only valid actions for that selection.
3. Technician creates, deletes, formats, resizes, converts, clones, repairs, optimizes, or wipes through guided dialogs.
4. Every mutating action is added to a pending operation queue with exact target disk/partition identity.
5. Technician reviews warnings and preflight checks, then clicks Apply once.
6. S.A.K. executes the operation through elevated helpers, streams progress, verifies result, and writes an operation report.

### Safety Principles

- No destructive action runs at dialog close or selection change. Mutating actions enter pending queue first.
- Apply button remains disabled until validation passes.
- Every destructive confirmation names disk number, model, serial, size, partition number, drive letter, file system, and expected result.
- System disk operations require extra confirmation and recovery recommendation.
- BitLocker volumes require detection and either suspend guidance or hard block depending on operation.
- Clone, restore, wipe, and conversion operations require stable power warning.
- Dry-run scripts are generated and previewed where possible.
- All elevated tasks must be allowlisted and parameter validated before execution.

---

## Interface Plan

### Top-Level Placement

Current simple panel order is:

1. Backup and Restore
2. File Management
3. Image Flasher
4. Benchmark and Diagnostics
5. Email Tools

Planned order:

1. Backup and Restore
2. File Management
3. Partition Manager
4. Image Flasher
5. Benchmark and Diagnostics
6. Email Tools

Implementation target: add `MainWindow::createPartitionManagerPanel()` after `createFileManagementPanel()` and before `createImageFlasherPanel()`.

### Main Layout

```
PartitionManagerPanel
|-- Header toolbar
|   |-- Refresh
|   |-- Disk selector
|   |-- Health summary chips
|   |-- Pending operation count
|   `-- Apply / Undo / Redo / Discard
|-- Main splitter
|   |-- Left operation rail
|   |   |-- Operations
|   |   |-- Clone & Image
|   |   |-- OS Migration
|   |   |-- Boot Repair
|   |   |-- SSD
|   |   `-- Wipe
|   |-- Center workspace
|   |   |-- Disk map list
|   |   |-- Partition table
|   |   `-- Event/progress strip
|   `-- Right details pane
|       |-- Selected disk/partition details
|       |-- SMART summary
|       |-- Valid actions
|       `-- Preflight warnings
`-- Bottom pending operation queue
```

### Visual Behavior

- Disk map uses stable-height horizontal bars, one row per physical disk.
- Partitions use color-coded file system bands with labels that truncate safely.
- Important badges: System, Boot, EFI, Recovery, BitLocker, Read-only, Removable, Unallocated.
- Invalid actions are hidden or disabled with accessible tooltips explaining why.
- Pending queue is always visible when it has entries.
- Keyboard navigation supports disk rows, partition bars, table rows, operation rail, and queue actions.
- Screen reader labels include disk number, model, partition number, file system, size, and action risk.
- Responsive layout collapses right details pane below center content at narrow widths.

### Detailed UI Regions

Header toolbar:
- Left: panel title, refresh button, last refresh time.
- Center: disk selector with model, disk number, size, health.
- Right: pending count, Undo, Redo, Discard, Apply.
- Apply uses high-emphasis style only when queue has valid operations.

Left operation rail:
- Uses icon + short label buttons.
- Sections: Operations, Clone & Image, OS Migration, Boot Repair, SSD, Wipe, Reports.
- Buttons disable when selected target cannot support action.
- Tooltip explains exact blocker, for example "Cannot extend: no adjacent unallocated space after this partition."

Center disk map:
- One stable-height row per disk.
- Disk row header shows Disk N, model, bus, style, total size, health state.
- Partition bars are proportional but enforce minimum visual width for tiny EFI/MSR/recovery partitions.
- Unallocated space appears as neutral dashed region.
- Selected partition has strong focus ring, not only color.

Center table:
- Columns: Disk, Partition, Letter, Label, File system, Type, Capacity, Used, Free, Flags, Health.
- Sort and filter are read-only view changes, not operation changes.
- Row context menu mirrors operation rail and disabled reasons.

Right details pane:
- Shows selected disk/partition identity first.
- Shows SMART summary for physical disk.
- Shows operation eligibility list.
- Shows warnings and blockers before any dialog opens.

Bottom queue:
- Queue entries show operation, target, risk, estimated impact.
- Each entry has remove and details buttons.
- Queue has dependency order and cannot be manually reordered if dependency would break.
- Apply opens final review dialog with before/after summary.

### Dialog Requirements

Every mutating dialog must include:

- Target identity block.
- Operation-specific inputs.
- Preview summary.
- Warnings/blockers region.
- Add to Queue button, not immediate execution.
- Cancel button.

High-risk dialogs also include:

- Typed confirmation token based on disk number and model.
- "I have a current backup" checkbox where data loss is possible.
- Stable power warning.
- Link/button to generate pre-operation report.

### Accessibility And Responsive Checklist

- All icon-only buttons have accessible names and tooltips.
- Disk map partitions are keyboard selectable.
- Color is never sole status signal; use labels/badges/icons.
- Focus order follows header, operation rail, disk map/table, details, queue.
- Text truncates with tooltip instead of overlapping.
- Minimum interactive target: 32 px height for toolbar buttons, 36 px for primary actions unless existing S.A.K. constants require larger.
- Layout supports 1280 px wide desktop without horizontal overflow.
- At narrower widths, details pane collapses below center workspace and queue remains reachable.
- High contrast and dark theme use palette-backed colors, not hard-coded light backgrounds.

---

## Architecture

### New Core Components

```
sak::PartitionManagerPanel
sak::PartitionManagerController
sak::StorageInventoryWorker
sak::PartitionOperationPlanner
sak::PartitionOperationQueue
sak::PartitionExecutor
sak::PartitionScriptBuilder
sak::PartitionSafetyValidator
sak::DiskCloneWorker
sak::DiskImageWorker
sak::OsMigrationPlanner
sak::BootRepairWorker
sak::SsdOptimizerWorker
sak::SecureWipeWorker
sak::PartitionReportGenerator
```

### New Data Models

```cpp
namespace sak {

enum class PartitionOperationType {
    Create,
    Delete,
    Format,
    SetDriveLetter,
    SetPartitionLabel,
    CheckFileSystem,
    SurfaceTest,
    PartitionRecoveryScan,
    SetPartitionHidden,
    SetPartitionActive,
    SetPartitionTypeId,
    InitializeDisk,
    DeleteAllPartitions,
    Resize,
    ConvertPartitionStyle,
    Merge,
    Split,
    ConvertFileSystem,
    CloneDisk,
    ClonePartition,
    CreateImage,
    RestoreImage,
    MigrateOs,
    RepairBoot,
    OptimizeSsd,
    WipePartition,
    WipeDisk,
    WipeFreeSpace
};

enum class OperationRisk {
    ReadOnly,
    Low,
    Destructive,
    SystemCritical
};

struct DiskInfo {
    uint32_t disk_number{0};
    QString device_path;
    QString model;
    QString serial_number;
    QString bus_type;
    QString media_type;
    QString partition_style;
    uint64_t size_bytes{0};
    bool is_system{false};
    bool is_boot{false};
    bool is_removable{false};
    bool is_read_only{false};
};

struct VolumeInfo {
    QString volume_guid;
    QString drive_letter;
    QString label;
    QString file_system;
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    bool bitlocker_enabled{false};
    bool dirty_bit_set{false};
};

struct PartitionInfoEx {
    uint32_t disk_number{0};
    uint32_t partition_number{0};
    QString partition_guid;
    QString type_name;
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
    bool is_system{false};
    bool is_boot{false};
    bool is_efi{false};
    bool is_recovery{false};
    bool is_active{false};
    std::optional<VolumeInfo> volume;
};

struct PartitionOperation {
    QString id;
    PartitionOperationType type;
    OperationRisk risk;
    QJsonObject payload;
    QString summary;
    QStringList warnings;
};

struct OperationPreview {
    QVector<PartitionOperation> operations;
    QString before_layout_hash;
    QString after_layout_description;
    QStringList blockers;
    QStringList warnings;
};

} // namespace sak
```

### Data Flow

```
StorageInventoryWorker
    -> DriveScanner
    -> PowerShell Storage module
    -> SmartDiskAnalyzer
    -> PartitionManagerController
    -> PartitionManagerPanel

User action
    -> PartitionOperationPlanner
    -> PartitionSafetyValidator
    -> PartitionOperationQueue
    -> Apply
    -> Elevated PartitionExecutor
    -> Verify inventory refresh
    -> PartitionReportGenerator
```

### File Map

Files implemented for this panel:

| File | Purpose |
| --- | --- |
| `include/sak/partition_manager_types.h` | Disk, partition, volume, operation, preview, result, and report value types. |
| `include/sak/partition_manager_panel.h` | Qt panel surface, selection model, queue controls, and signal wiring. |
| `src/gui/partition_manager_panel.cpp` | Main panel composition, disk map, table, details pane, operation dialogs. |
| `include/sak/partition_manager_controller.h` | Controller between UI, inventory, planner, queue, and executor. |
| `src/core/partition_manager_controller.cpp` | State machine, refresh, validation, apply flow, report handoff. |
| `include/sak/storage_inventory_worker.h` | Async storage inventory worker with cancellation. |
| `src/core/storage_inventory_worker.cpp` | PowerShell Storage JSON inventory plus `DriveScanner` merge. |
| `include/sak/partition_operation_planner.h` | Pure planning API for all queued operations. |
| `src/core/partition_operation_planner.cpp` | Builds operation previews and blockers without executing changes. |
| `include/sak/partition_operation_queue.h` | Queue state, undo, redo, discard, dependency ordering. |
| `src/core/partition_operation_queue.cpp` | Queue implementation and layout hash tracking. |
| `include/sak/partition_safety_validator.h` | Safety rules for system/boot/EFI/recovery/BitLocker/removable/read-only targets. |
| `src/core/partition_safety_validator.cpp` | Target validation and warning generation. |
| `include/sak/partition_script_builder.h` | Typed command generation for PowerShell and diskpart scripts. |
| `src/core/partition_script_builder.cpp` | Escaped script generation and redacted preview text. |
| `include/sak/partition_executor.h` | Elevated execution client and non-elevated read-only execution. |
| `src/core/partition_executor.cpp` | Task dispatch, progress stream, cancellation, verification refresh. |
| `include/sak/disk_clone_worker.h` | Raw disk/partition clone and verification worker. |
| `src/core/disk_clone_worker.cpp` | Sector-aligned raw copy using flasher patterns. |
| `include/sak/disk_image_worker.h` | Disk/partition image create and restore worker. |
| `src/core/disk_image_worker.cpp` | Image manifest, checksum, compression hooks. |
| `include/sak/boot_repair_worker.h` | BIOS/UEFI boot repair planner and executor. |
| `src/core/boot_repair_worker.cpp` | `bcdboot`, EFI mount, BCD checks, WinRE checks. |
| `include/sak/ssd_optimizer_worker.h` | TRIM/alignment query and ReTrim operation. |
| `src/core/ssd_optimizer_worker.cpp` | SSD optimization command execution and report. |
| `include/sak/secure_wipe_worker.h` | Free-space, partition, and disk wipe worker. |
| `src/core/secure_wipe_worker.cpp` | Wipe command execution, typed confirmation validation, result report. |
| `include/sak/partition_report_generator.h` | HTML/JSON report builder. |
| `src/core/partition_report_generator.cpp` | Before/after layout report, operation log, warnings. |
| `tests/unit/test_partition_operation_planner.cpp` | Planner test coverage. |
| `tests/unit/test_partition_safety_validator.cpp` | Safety-rule test coverage. |
| `tests/unit/test_partition_script_builder.cpp` | Script generation and escaping coverage. |
| `tests/unit/test_partition_operation_queue.cpp` | Queue state and dependency coverage. |
| `tests/unit/test_storage_inventory_parser.cpp` | Mocked inventory parser coverage. |
| `tests/unit/test_partition_manager_accessibility.cpp` | Labels, focus, and action-state coverage where practical. |

Existing files to update:

- `CMakeLists.txt` - add new sources and tests.
- `include/sak/main_window.h`, `src/gui/main_window.cpp` - add panel member and create it before Image Flasher.
- `include/sak/elevation_tier.h` - add Partition Manager feature IDs.
- `src/elevated/elevated_helper_main.cpp` - register partition task allowlist.
- `resources/icons.qrc` and `resources/icons/` - add panel and operation icons if existing icons are insufficient.
- `README.md`, `CHANGELOG.md`, `docs/PRODUCTION_GRADE_AUDIT.md` - update after implementation, not during plan-only work.

### Controller State Machine

```
Idle
  -> RefreshingInventory
  -> Ready
  -> PlanningOperation
  -> QueueDirty
  -> PreflightRunning
  -> AwaitingElevation
  -> Applying
  -> Verifying
  -> Ready
  -> Failed
  -> Cancelled
```

Rules:

- `RefreshingInventory` cancels stale workers before starting a new scan.
- `PlanningOperation` never mutates disk state.
- `QueueDirty` uses layout hash from last inventory snapshot.
- `PreflightRunning` re-checks layout hash, BitLocker state, dirty bit, and mounted/locked state.
- `Applying` rejects if current layout hash differs from queued preview.
- `Verifying` refreshes inventory and compares expected result.
- `Failed` preserves report, command output, and recovery guidance.

### Elevated Task Allowlist

| Task ID | Payload | Output | Notes |
| --- | --- | --- | --- |
| `Partition.Create` | disk number, offset/size, file system, label, drive letter preference | partition id, volume id | Data disk only in v1. |
| `Partition.Delete` | disk number, partition number, expected GUID/offset/size | deleted partition id | Blocks system/boot/EFI/recovery by default. |
| `Partition.Format` | volume id/drive letter, file system, label, full/quick | formatted volume id | Requires exact volume identity match. |
| `Partition.SetDriveLetter` | partition id, old/new drive letter | updated mount points | Low risk but elevated when Windows requires. |
| `Partition.SetPartitionLabel` | partition id, drive letter, new label | updated volume label | Requires a mounted volume in v1. |
| `Partition.CheckFileSystem` | drive letter, scan mode | scan report | Read-only scan path. |
| `Partition.SurfaceTest` | disk or drive letter | surface/health report | Read-only online scan path. |
| `Partition.PartitionRecoveryScan` | disk number, quick/full mode | candidate boot-sector offsets | Read-only candidate discovery. |
| `Partition.SetPartitionHidden` | partition identity, hidden flag | updated hidden/default-letter state | Blocks protected partitions in v1. |
| `Partition.SetPartitionActive` | MBR partition identity, active flag | updated active flag | MBR only and system-critical. |
| `Partition.SetPartitionTypeId` | partition identity, MBR type or GPT GUID | updated type ID | System-critical metadata operation. |
| `Partition.InitializeDisk` | disk number, GPT/MBR style | initialized disk | Empty/raw data disks only. |
| `Partition.DeleteAllPartitions` | disk number, confirmation token | emptied disk | Data disks only; requires Apply review. |
| `Partition.Resize` | disk number, partition number, old size, new size, contiguous free space after selected partition, optional blocked move/donor intent | new size | Requires target change, used-space floor, adjacent-free-space ceiling, supported size query, and no unsupported start-move/donor payload. |
| `Partition.ConvertStyle` | disk number, source style, target style, mode | conversion result | `mbr2gpt` for system MBR to GPT; empty data disks only otherwise. |
| `Partition.ConvertFileSystem` | drive letter, source FS, target FS | conversion result | V1 in-place only for FAT/FAT32 to NTFS. |
| `Partition.CloneDisk` | source disk, target disk, layout mode, verify mode | clone result | Requires target wipe confirmation. |
| `Partition.ClonePartition` | source partition, target region, verify mode | clone result | Data partitions first. |
| `Partition.CreateImage` | source disk/partition, destination path, verify mode | manifest path, checksum | Read-only source risk; destination must be a file path and cannot be on the source disk/partition. |
| `Partition.RestoreImage` | image path, target disk/partition, verify mode | restore result | Same safety rules as flash/clone. |
| `Partition.MigrateOs` | source disk, target disk, layout mode, boot mode | migration result | Copies boot chain and runs boot validation. |
| `Partition.RepairBoot` | disk/volume, boot mode, repair options | repair report | Uses `bcdboot`, EFI mount, `reagentc`. |
| `Partition.OptimizeSsd` | volume/disk, retrim flag | optimization report | No defrag path for SSD. |
| `Partition.Wipe` | target identity, wipe mode, confirmation token | wipe report | Requires typed confirmation and layout hash. |

All payloads include:

- `layout_hash`
- `target_identity`
- `operation_id`
- `requested_by_version`
- `dry_run`
- `timeout_ms`

### Storage Inventory Contract

Inventory should be collected as structured JSON from PowerShell, then merged with `DriveScanner` and SMART reports:

```powershell
$disks = Get-Disk | ForEach-Object {
  $disk = $_
  [pscustomobject]@{
    Number = $disk.Number
    FriendlyName = $disk.FriendlyName
    SerialNumber = $disk.SerialNumber
    HealthStatus = "$($disk.HealthStatus)"
    OperationalStatus = @($disk.OperationalStatus | ForEach-Object { "$_" })
    PartitionStyle = "$($disk.PartitionStyle)"
    Size = [uint64]$disk.Size
    IsBoot = [bool]$disk.IsBoot
    IsSystem = [bool]$disk.IsSystem
    IsReadOnly = [bool]$disk.IsReadOnly
    BusType = "$($disk.BusType)"
    Partitions = @(Get-Partition -DiskNumber $disk.Number | ForEach-Object {
      $partition = $_
      $volume = $partition | Get-Volume -ErrorAction SilentlyContinue
      [pscustomobject]@{
        PartitionNumber = $partition.PartitionNumber
        Guid = "$($partition.Guid)"
        Type = "$($partition.Type)"
        GptType = "$($partition.GptType)"
        Offset = [uint64]$partition.Offset
        Size = [uint64]$partition.Size
        DriveLetter = "$($partition.DriveLetter)"
        IsActive = [bool]$partition.IsActive
        IsBoot = [bool]$partition.IsBoot
        IsSystem = [bool]$partition.IsSystem
        IsReadOnly = [bool]$partition.IsReadOnly
        Volume = if ($volume) {
          [pscustomobject]@{
            FileSystem = "$($volume.FileSystem)"
            FileSystemLabel = "$($volume.FileSystemLabel)"
            HealthStatus = "$($volume.HealthStatus)"
            Size = [uint64]$volume.Size
            SizeRemaining = [uint64]$volume.SizeRemaining
          }
        } else { $null }
      }
    })
  }
}
$disks | ConvertTo-Json -Depth 8 -Compress
```

Parser rules:

- Empty strings become `std::nullopt` or empty `QString`, not magic values.
- Numeric values parse as unsigned 64-bit and reject negative input.
- Unknown enum strings map to `Unknown`, not a crash.
- Missing volume data is valid for EFI, recovery, MSR, and unformatted partitions.
- Layout hash uses disk number, serial, partition style, partition number, GUID, offset, size, file system, and drive letter.

---

## Windows Backend Strategy

### Preferred APIs

- Inventory: `Get-Disk`, `Get-Partition`, `Get-Volume`, `Get-PhysicalDisk`, `Get-BitLockerVolume`.
- Create: `Initialize-Disk`, `New-Partition`, `Format-Volume`, `Set-Partition`.
- Resize: `Resize-Partition` after `Get-PartitionSupportedSize`.
- Repair: existing `Repair-Volume` pattern.
- Optimize SSD: `Optimize-Volume -ReTrim`, `fsutil behavior query DisableDeleteNotify`, partition alignment checks.
- MBR to GPT for OS disks: `mbr2gpt.exe /validate` then `mbr2gpt.exe /convert`.
- Boot repair: `bcdboot.exe`, `bootrec.exe` when available, `reagentc.exe`, EFI partition mount/unmount.
- Secure wipe: `cipher /w` for free space, `Format-Volume -Full`, `Clear-Disk -RemoveData`, `diskpart clean all`.

### DiskPart Use

Use `diskpart.exe` only when Storage cmdlets do not expose the needed behavior or existing S.A.K. flows already rely on it. DiskPart scripts must be generated from typed parameters, never from raw user text, and must be logged in redacted reports before execution.

### Known Windows Limits

- Extending a volume needs contiguous unallocated space immediately after the partition unless a later offline move/donor engine is introduced.
- Shrink may be blocked by unmovable files.
- Windows built-ins cannot safely move partition starts like full commercial partition managers.
- GPT to MBR conversion is destructive through normal Windows tools unless the disk is empty.
- NTFS to FAT32/exFAT is not a safe in-place built-in conversion path; plan as backup, format, restore.
- System partition operations often need WinPE or reboot/offline execution.

### Operation Rule Matrix

| Operation | Allowed target | Hard block | Preflight | Verify |
| --- | --- | --- | --- | --- |
| Create partition | Unallocated region on data disk | system disk, read-only disk, dynamic disk, no free region | disk online, free span exact, file system supported | new partition exists, size and file system match |
| Delete partition | Data partition | system, boot, EFI, MSR, recovery, BitLocker locked, read-only | exact GUID/offset/size match, user confirmation | partition missing, free span created |
| Format partition | Data volume | system, boot, EFI, recovery, read-only, unsupported FS | volume unlocked, no dirty bit unless forced, label valid | file system and label match |
| Set drive letter | Mounted or mountable data partition | system reserved hidden partitions unless advanced | letter available, partition identity match | mount point changed |
| Set partition label | Mounted data partition | unmounted/system reserved partitions unless advanced | drive letter available, partition identity match | volume label changed |
| Check file system | Mounted volume | no drive letter | drive letter available | scan report emitted |
| Surface test | Disk or mounted volume | no readable target | disk/volume online | scan/reliability report emitted |
| Partition recovery scan | Disk | unreadable disk | raw disk readable | candidate boot-sector offsets emitted |
| Recovered partition restore | Disk candidate offset/size/type ID | missing candidate identity, overlap with existing partition, candidate exceeds disk, no acknowledgement, unsafe target disk | disk online/read/write, candidate bounds reviewed on disposable media | restored partition appears without overlap and reboot/data checks pass |
| Hide/unhide partition | Data partition | protected system/boot/EFI/MSR/recovery | partition identity match | hidden/default-letter state changed |
| Set active/inactive | MBR data partition | GPT disk, protected partitions | MBR partition identity match | active flag changed |
| Partition type ID | Data partition | missing type ID, protected partitions | MBR type or GPT GUID supplied | type ID changed |
| Initialize disk | Empty/raw data disk | disk with partitions, system disk | disk online/read-write | partition style initialized |
| Delete all partitions | Data disk | system disk, read-only disk, no partitions | disk identity and confirmation | partitions removed |
| Shrink | Data NTFS/raw partition | system in v1, unsupported FS, BitLocker locked | `Get-PartitionSupportedSize`, dirty bit clear | partition size equals target |
| Extend | Data NTFS/ReFS partition | no adjacent free space, target exceeds contiguous free span, no-op target, system in v1 | max size query, adjacent span available, target bigger than used space | partition size equals target and adjacent span shrinks |
| MBR to GPT | OS MBR disk or empty data disk | unsupported layout, UEFI unsupported, BitLocker active without suspend | `mbr2gpt /validate`, firmware warning | partition style GPT, EFI present for OS |
| GPT to MBR | Empty data disk | any existing partitions, system disk | no partitions, target under MBR limits | partition style MBR |
| Split | Data partition | system in v1, cannot shrink enough | supported shrink size, new FS valid | original shrunk, new partition created |
| Merge | Adjacent data partitions | non-adjacent, system/recovery/EFI, insufficient space, incompatible plan | source backup/copy plan, target space check | target extended, source removed only after copy |
| FAT/FAT32 to NTFS | Data volume | system in v1, dirty file system, BitLocker locked | `chkdsk`/`Repair-Volume`, free space check | file system NTFS |
| Disk clone | Source disk to target disk | target too small, same disk, target system disk unless advanced | SMART status, target confirmation, lock target | layout copied, verification mode passes |
| Partition clone | Source partition to target region | target too small, overlap, unsupported locked source | source/target identity, lock target | checksum/sample verification |
| Image create | Disk/partition to file | destination inside source target region if unsafe | destination free space, source health | manifest and checksum written |
| Image restore | Image to disk/partition | target too small, system target unless advanced | image manifest validation, target confirmation | restored bytes/layout match |
| OS migration | System disk to external/internal target | target too small, same disk, BitLocker not handled | SMART, boot mode, partition chain, target wipe confirm | target boot chain valid, report next boot steps |
| Boot repair | Selected Windows install/disk | no Windows install found, ambiguous target | boot mode, EFI/MSR/BCD/WinRE status | BCD/WinRE status and boot files present |
| SSD ReTrim | SSD/NVMe volume | HDD-only, unsupported FS | TRIM support query | command success and report updated |
| Free-space wipe | Data volume | system volume in v1, read-only, insufficient temp handling | free space estimate, confirmation | wipe command success |
| Partition wipe | Data partition | system/boot/EFI/recovery, read-only | exact identity, typed confirmation | partition blank/formatted per mode |
| Disk wipe | Non-system data disk | system disk, unknown identity, read-only | target identity, typed confirmation, AC power warning | disk blank/offline state expected |

### Command Backend Recipes

All snippets are templates, not raw strings to concatenate with UI text. `PartitionScriptBuilder` must build them from validated typed values.

Create and format data partition:

```powershell
$partition = New-Partition -DiskNumber <disk> -Size <bytes> -AssignDriveLetter -ErrorAction Stop
Format-Volume -Partition $partition -FileSystem <NTFS|exFAT|FAT32> -NewFileSystemLabel <label> -Confirm:$false -ErrorAction Stop
```

Shrink or extend data partition:

```powershell
$supported = Get-PartitionSupportedSize -DiskNumber <disk> -PartitionNumber <partition> -ErrorAction Stop
if (<targetBytes> -lt $supported.SizeMin -or <targetBytes> -gt $supported.SizeMax) { throw "Target size outside supported range" }
Resize-Partition -DiskNumber <disk> -PartitionNumber <partition> -Size <targetBytes> -ErrorAction Stop
```

MBR to GPT system disk:

```powershell
mbr2gpt.exe /validate /disk:<disk> /allowFullOS
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
mbr2gpt.exe /convert /disk:<disk> /allowFullOS
exit $LASTEXITCODE
```

SSD ReTrim:

```powershell
Optimize-Volume -DriveLetter <letter> -ReTrim -Verbose -ErrorAction Stop
```

Boot repair for UEFI Windows install:

```powershell
mountvol <tempLetter>: /S
bcdboot <windowsPath> /s <tempLetter>: /f UEFI
reagentc /info
mountvol <tempLetter>: /D
```

Free-space wipe:

```powershell
cipher.exe /w:<driveRoot>
```

Disk wipe:

```powershell
Clear-Disk -Number <disk> -RemoveData -Confirm:$false -ErrorAction Stop
```

### Error Handling

- Return structured error codes, not only raw command text.
- Store raw stdout/stderr in report details, trimmed and redacted for UI.
- On timeout, cancel child process and mark operation result `TimedOut`.
- On cancellation, stop before next operation and preserve current disk state report.
- On layout mismatch, do not execute queued operations; force refresh and re-plan.
- On partial failure, show completed operations, failed operation, and recovery guidance.

---

## Feature Design

### 1. Basic Partition Operations

Create:
- Select unallocated space.
- Choose size, file system, label, drive letter, quick/full format.
- Preview new partition location and resulting free space.
- Execute with `New-Partition` and `Format-Volume`.

Delete:
- Block system, boot, EFI, recovery by default.
- Allow recovery/EFI only from advanced mode with repair-media warning.
- Delete through `Remove-Partition` or diskpart fallback.

Format:
- Support NTFS, exFAT, FAT32 where Windows allows.
- Warn that format destroys files.
- Offer quick/full format.
- Use `Format-Volume`.

### 2. Resize Operations

Shrink:
- Query supported min/max size.
- Show unmovable-file limit if Windows reports it.
- Block no-op targets and targets smaller than known used volume bytes.
- Queue a layout preview that creates a free span after the partition.
- Execute with `Get-PartitionSupportedSize` plus `Resize-Partition`.

Extend:
- Require adjacent usable unallocated space for online resize, or use Allocate Free Space/MovePartition for donor and preceding-free-space workflows.
- Show blocker when recovery partition sits between C: and free space.
- Cap target size to contiguous free space immediately after the partition.
- Queue a layout preview that consumes the adjacent free span.
- Execute with `Get-PartitionSupportedSize` plus `Resize-Partition`.

Move/donor:
- Expose start-move and donor-space modes in the Move/Resize dialog for familiar commercial UX.
- Queue `MovePartition` for start moves with off-volume backup, delete/recreate at target offset, restore, SHA-256 manifest comparison, and repair scan.
- Queue Allocate Free Space for adjacent donor shrink/restore and Allocate Free Space To for adjacent unallocated resize/move cases.
- Reject any legacy `Resize` payload carrying hidden start-move or donor-space fields so only the explicit engines can run those workflows.

### 3. MBR/GPT Conversion

System MBR to GPT:
- Run `mbr2gpt.exe /validate`.
- Check UEFI support guidance.
- Warn firmware must switch to UEFI after conversion.
- Run conversion only after confirmation.

Data disk conversion:
- Empty disk only in first release.
- Use `Initialize-Disk -PartitionStyle GPT|MBR` or diskpart fallback.
- Existing volumes must be deleted or cloned elsewhere first.

### 4. Merge And Split

Merge:
- First release supports adjacent data partitions only.
- If both partitions contain data, plan copy-to-target then delete source then extend target.
- Block when file systems, BitLocker, insufficient target space, or system/recovery partitions make merge unsafe.

Split:
- Shrink selected partition.
- Create new partition in freed space.
- Optional folder move is future work.

### 5. File System Conversion

- FAT/FAT32 to NTFS: use `convert.exe` when preflight passes.
- NTFS to FAT32/exFAT: destructive recreate workflow only.
- exFAT to NTFS: destructive recreate unless safe backend is added.
- Always recommend backup and run file-system check first.

### 6. Disk Cloning And Imaging

Clone disk:
- Source and target selection with size/sector compatibility checks.
- Require target disk confirmation.
- If target larger, offer proportional expand or keep original layout.
- Use raw copy worker derived from `FlashWorker` patterns, but reverse source/target where appropriate.
- Verify with checksum/sample validation.

Clone partition:
- Target partition or unallocated region validation.
- File-system-aware copy can be later; initial plan should use sector copy for supported aligned partitions.

Image backup:
- Create raw image from disk or partition.
- Optional compression through existing image source/decompression patterns extended to output.
- Save metadata manifest: disk model, serial, partition layout, checksums.

Image restore:
- Reuse image verification and raw write safety patterns from Image Flasher.
- Block accidental system disk restore unless advanced confirmation passes.

### 7. OS Migration

- Wizard detects Windows, EFI/System Reserved, recovery, and OS partitions.
- Clone required boot chain, not just C:.
- Support target SSD/HDD layout preview.
- Run boot validation after clone.
- Offer `bcdboot` repair if target is not bootable.
- Warn user to change BIOS/UEFI boot order.
- System-live migration may require offline/reboot workflow; first release can require external target and no in-use target volumes.

### 8. Boot Repair Utilities

- Detect boot mode: BIOS/MBR or UEFI/GPT.
- Repair EFI boot files using `bcdboot`.
- Rebuild or validate BCD entries.
- Mount/unmount EFI System Partition safely.
- Run `reagentc /info`, enable/disable/update WinRE where needed.
- Reuse `Repair-Volume` scan/offline-fix for file-system issues.
- Generate a repair report with commands run and next boot steps.

### 9. S.M.A.R.T. Monitoring

- Integrate `SmartDiskAnalyzer` into disk rows.
- Show health state, temperature, power-on hours, reallocated/pending sectors, NVMe wear, media errors.
- Block clone/migration from failing source only with explicit override; recommend image backup first.
- Refresh SMART data manually and on panel open.

### 10. SSD Optimization

- Detect SSD/NVMe media type.
- Check partition alignment.
- Query TRIM status.
- Run ReTrim through `Optimize-Volume`.
- Avoid defrag wording for SSD flows.
- Report before/after status and warnings.

### 11. Secure Data Wiping

Wipe modes:
- Free-space wipe: `cipher /w`.
- Partition wipe: full format or overwrite worker.
- Disk wipe: `Clear-Disk -RemoveData`, diskpart `clean all`, or overwrite worker.
- SSD secure erase: future advanced backend only; warn that overwrite semantics differ on SSDs.

Safety:
- Require typed confirmation for disk wipe.
- Show exact disk identity.
- Block current OS disk wipe in normal mode.
- Require AC power warning for laptops when detected.

---

## Report And Audit Trail

### Report Contents

Each report contains:

- S.A.K. version and report timestamp.
- User privilege/elevation state.
- Before layout inventory.
- SMART summary for affected disks.
- Operation queue with risk tiers.
- Preflight result.
- Commands executed, with sensitive paths redacted where needed.
- Progress timeline.
- After layout inventory.
- Verification result.
- Warnings, blockers, errors, and recovery guidance.

### JSON Schema Outline

```json
{
  "schema_version": 1,
  "sak_version": "0.9.x",
  "created_at": "2026-06-01T00:00:00Z",
  "operation_batch_id": "uuid",
  "before_layout_hash": "sha256",
  "after_layout_hash": "sha256",
  "disks": [],
  "operations": [],
  "preflight": {
    "passed": true,
    "warnings": [],
    "blockers": []
  },
  "execution": {
    "started_at": "",
    "finished_at": "",
    "status": "success|failed|cancelled|timed_out",
    "steps": []
  },
  "verification": {
    "passed": true,
    "details": []
  }
}
```

### Logging Rules

- Log target disk identity before operation starts.
- Log layout hash before and after.
- Log exact generated script in debug/report detail, not in status bar.
- Never log raw user secrets or network paths with credentials.
- Status bar shows only short operation state, matching existing S.A.K. status-bar pattern.

---

## Security And Abuse Resistance

- Elevated helper accepts only registered task IDs.
- Task payloads must include expected disk/partition identity and layout hash.
- Script builder escapes labels, paths, and drive letters from typed values.
- Disk numbers must be numeric and revalidated immediately before execution.
- File paths for images must use canonical paths and must not point inside wiped/restored target regions.
- Confirmation token required for destructive disk-level tasks.
- System disk destructive operations are blocked in v1 even with admin.
- Reports mark destructive actions clearly.
- Command injection tests cover labels, paths, drive letters, and diskpart script generation.

---

## Implementation Phases

### Phase 0 - Discovery And Hardening

- Add `docs/PARTITION_MANAGER_PANEL_PLAN.md`.
- Audit existing storage helpers and identify shared APIs.
- Decide first-release supported operations and unsupported states.
- Add feature IDs to `elevation_tier.h` plan.
- Lock v1 decisions listed in Completion Decisions.
- Create detailed test VM checklist.

Acceptance:
- Plan reviewed.
- Supported/unsupported operation matrix approved.
- No unresolved open questions remain for Phase 1.

### Phase 1 - Read-Only Inventory And UI

- Add `PartitionManagerPanel`.
- Add `StorageInventoryWorker`.
- Display disks, partitions, volumes, SMART, and flags.
- Add disk map, table, details pane, and report export.
- Add tab before Image Flasher.
- Add read-only unsupported badges for dynamic disks and Storage Spaces.
- Add accessibility names for disk map and operation controls.

Acceptance:
- No admin required for read-only view where Windows permits.
- UI remains usable at narrow and wide widths.
- Screen reader labels and keyboard selection work.
- No mutating task exists in this phase.

### Phase 2 - Operation Planner And Queue

- Add `PartitionOperationPlanner`.
- Add pending operation queue with Apply, Undo, Redo, Discard.
- Add safety validator and layout preview.
- Generate PowerShell/diskpart scripts without executing.
- Add operation dialogs for create, delete, format, drive letter, shrink, extend.
- Add final review dialog.

Acceptance:
- Unit tests cover valid/invalid plans.
- No mutating code path exists outside Apply.
- Queue survives refresh only when layout hash still matches.

### Phase 3 - Basic Mutating Operations

- Create, delete, format, label, drive letter, shrink, extend.
- Add elevated helper task allowlist.
- Verify layout after execution.
- Add report generation for each applied queue.
- Add cancellation behavior between operations.

Acceptance:
- VM tests cover data disk operations.
- System/boot/EFI/recovery destructive actions are blocked by default.
- Failed operation preserves report and recovery guidance.

### Phase 4 - Conversion, Merge, Split

- MBR-to-GPT system conversion with `mbr2gpt`.
- Empty data disk GPT/MBR conversion.
- Split through shrink + create.
- Merge adjacent data partitions through explicit composite plan.
- File-system conversion support matrix.
- Add FAT/FAT32 to NTFS conversion path.
- Add blockers for unsafe conversion and non-adjacent merge.

Acceptance:
- MBR2GPT validation failure is displayed without changing disk.
- Unsupported conversion routes show clear blockers.
- Merge never deletes source before copy/verification step succeeds.

### Phase 5 - Clone, Image, OS Migration

- Disk/partition clone workers.
- Image backup/restore and manifest.
- OS migration wizard.
- Boot repair fallback.
- Add target disk wipe confirmation.
- Add full and sampled verification modes.

Acceptance:
- Clone verification supports full and sample modes.
- OS migration copies complete boot chain and reports boot validation.
- Image restore refuses mismatched manifest unless advanced override is used.

### Phase 6 - Boot Repair, SSD, Wipe

- Boot repair panel.
- SSD optimization panel.
- Secure wipe panel.
- Full operation reports.
- Add typed confirmation for disk wipe.
- Add SSD warning for overwrite/full-format semantics.

Acceptance:
- Wipe operations require typed confirmation.
- Boot repair actions are reversible where possible and fully logged.
- SSD ReTrim does not run HDD defrag path.

### Phase 7 - Polish, Docs, Release Gates

- Update README, CHANGELOG, and production audit after implementation.
- Add user documentation and technician safety guide.
- Add screenshots.
- Run full CTest, readiness script, and manual VM matrix before release.
- Add final UI screenshots to docs.
- Add release notes with unsupported states and safety warnings.

Acceptance:
- No new UI overlap at tested desktop/mobile-equivalent widths.
- Accessibility audit passes.
- Destructive operation test matrix completed in VMs only.
- Release cannot ship until all critical destructive-operation tests pass.

### Implementation Checklist

Phase 1 checklist:

- [x] Add panel class and tab placement before Image Flasher.
- [x] Build read-only inventory worker.
- [x] Parse PowerShell JSON into typed models.
- [x] Merge `DriveScanner` metadata.
- [x] Merge SMART reports.
- [x] Render disk map.
- [x] Render partition table.
- [x] Render details pane.
- [x] Add report export.
- [x] Add unit tests for parser and layout hash.

Phase 2 checklist:

- [x] Add planner.
- [x] Add safety validator.
- [x] Add queue model.
- [x] Add Add to Queue dialogs.
- [x] Add final review dialog.
- [x] Add script preview.
- [x] Add tests for system/boot/EFI/recovery blockers.

Phase 3 checklist:

- [x] Register elevated task IDs.
- [x] Implement create/delete/format/letter/resize execution.
- [x] Add preflight revalidation.
- [x] Add post-operation verification.
- [x] Add operation report.
- [x] Rerun disposable-VHD data-disk tests after adding `vhd.image-restore` and `vhd.partition-clone-region`; strict evidence at `artifacts\partition-manager-certification\vhd-strict\run-20260601-203234\partition-manager-certification-report.json` covers all 12 VHD scenarios.

Phase 4-7 checklist:

- [x] Implement conversion/merge/split/file-system conversion.
- [x] Implement clone/image workers.
- [x] Implement OS migration wizard.
- [x] Implement boot repair.
- [x] Implement SSD optimization.
- [x] Implement secure wipe.
- [ ] Complete external manual VM/hardware matrix.
- [x] Update README/CHANGELOG/audit/user docs.

---

## Test Strategy

### Unit Tests

- `PartitionOperationPlanner` creates correct plans.
- `PartitionSafetyValidator` blocks system/boot/EFI/recovery operations.
- `PartitionScriptBuilder` escapes and validates all parameters.
- `PartitionOperationQueue` undo/redo/discard state.
- `StorageInventoryWorker` parsing with mocked PowerShell JSON.
- `BootRepairPlanner` BIOS/UEFI branch selection.
- `SecureWipePlanner` confirmation requirements.
- `PartitionScriptBuilder` rejects invalid disk numbers, labels, drive letters, paths, and confirmation tokens.
- `PartitionReportGenerator` writes before/after layout hashes.
- `PartitionManagerController` refuses apply when current layout hash differs from queued hash.

### Integration Tests

- Elevated helper rejects unregistered partition task IDs.
- Process cancellation stops long-running scripts.
- Post-operation inventory refresh detects layout change.
- Report generator includes before/after layout.
- Elevated helper rejects malformed payloads.
- Long-running clone/wipe cancellation stops at safe step boundaries.
- Failed command maps to structured error code and report entry.

### Manual VM Matrix

Run `scripts/run_partition_manager_vhd_certification_strict.ps1 -RelaunchElevated` first to collect disposable-VHD evidence for create/format/resize/delete, FAT32-to-NTFS conversion, Quick Partition GPT equal/custom and MBR four-data-partition layouts, adjacent Extend Partition Wizard growth, recovered partition write-back, empty GPT/MBR conversion, clear-disk wipe, offline VHD image clone, offline VHD image restore, and raw-region partition clone. The strict handoff exits nonzero if any VHD scenario is skipped. Then use `scripts/new_partition_manager_external_evidence_manifest.ps1 -CreateEvidenceDirectories` to scaffold VM/hardware/lab evidence, fill every external gate with `Passed`, every required `evidence` payload key with a non-empty value, and an existing `evidence_path` or valid `evidence_url`. Run `scripts/run_partition_manager_hardware_certification_strict.ps1` with the strict VHD evidence root, external manifest, checklist, and evidence directory before release notes claim hardware-certified destructive behavior.

- [x] Supplemental Windows 11 VirtualBox data-disk destructive smoke passed on 2026-06-02 07:44 PDT at `artifacts\partition-manager-certification\vm-lab\partition-vm-admin-report.json`: UAC elevated token, disposable VBOX disk guards, GPT NTFS create/format/resize/delete/clear, MBR FAT32-to-NTFS conversion, and final RAW cleanup on disks 1 and 2.
- [x] `external.bitlocker` locked/unlocked protected data-volume blocker gate passed on 2026-06-02 09:26 PDT at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker\report.json`.
- [x] `external.file-level-data-recovery` passed on 2026-06-02 18:00 PDT at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery\report.json`; disposable NTFS raw-volume scan restored `sak-deleted-recovery-fixture.pdf` to a separate destination, verified SHA-256 `31080995DA446C2192A63231DFDA08572DA754EAF9D18DEBDE38CDE27F7D703F`, and verified the scanned source range stayed unchanged.
- [x] `external.bitlocker-mutation` passed on 2026-06-02 18:38 PDT at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation\report.json`; disposable BitLocker data volume lock/unlock/suspend/resume was verified with sanitized command evidence.
- [x] `external.allocate-free-space` passed on 2026-06-02 20:19 PDT at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.allocate-free-space\report.json`; adjacent disposable-VHD source/donor volumes verified donor backup/delete/source extend/donor recreate/restore, SHA-256 manifest match, and clean repair scans.
- [x] `external.cluster-size-change` passed on 2026-06-02 21:03 PDT at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.cluster-size-change\report.json`; disposable-VHD NTFS volume changed allocation unit size from 4096 to 16384 bytes, restored 3 files, matched SHA-256 hashes, preserved 4 ACL entries, preserved 1 alternate data stream, repair-scanned clean, and cleaned up the VHD.
- [x] `external.hdd-defrag-execution` passed on 2026-06-02 22:41 PDT / 2026-06-03 UTC at `artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution\report.json`; elevated Windows 11 VirtualBox lab targeted disposable 4 GB `VBOX HARDDISK` disk 1, ran `Optimize-Volume -Analyze`, `Optimize-Volume -Defrag`, and `Repair-Volume -Scan`, then cleared the disk back to RAW.
- [x] `external.partition-move`, `external.primary-logical-conversion`, `external.volume-serial-number`, `external.dynamic-to-basic`, and `external.hardware-wipe` passed on 2026-06-03 01:03 PDT through `scripts\launch_partition_manager_vm_data_external_gates_nonusb.cmd`; the elevated VM runner used disposable 4 GB VirtualBox data disk 1, emitted per-gate `report.json` files under `artifacts\partition-manager-certification\vm-lab\external-evidence\`, hash/mount/repair-scan evidence where applicable, and left the disk RAW.
- [x] `external.ssd-retrim`, `external.ssd-secure-erase`, and `external.usb-removable` passed on 2026-06-03 01:18 PDT through `scripts\launch_partition_manager_vm_ssd_usb_external_gates.cmd`; host proof in `artifacts\partition-manager-certification\vm-lab\vbox-showvminfo-ssd-usb.txt` records SATA port 2 `nonrotational=on`/discard for the disposable SSD fixture and a USB storage VDI.
- [x] Final boot/action gates passed on 2026-06-03 PDT: `external.os-migration-reboot`, `external.boot-repair-uefi`, `external.boot-repair-bios`, and `external.system-mbr2gpt` all have imported `report.json` evidence under `artifacts\partition-manager-certification\vm-lab\external-evidence\`. The imported manifest plus checklist now record 18/18 external gates passed, and strict hardware handoff reports `HardwareCertified`.

- GPT data disk with NTFS/exFAT/FAT32.
- MBR data disk under 2 TB.
- System MBR-to-GPT conversion in disposable VM.
- USB removable drive.
- SSD/NVMe device or virtual equivalent for ReTrim path.
- Failed SMART source-drive simulation via mocked smartctl output.
- Boot repair in UEFI VM with broken BCD.
- Layout mismatch between queue and apply.
- Recovery partition between C: and unallocated space.
- Full target disk wipe on disposable non-system disk.
- Clone to larger disk with keep-size and expand-layout modes.

### Test Data Fixtures

- `tests/fixtures/partition_inventory/gpt_basic.json`
- `tests/fixtures/partition_inventory/mbr_basic.json`
- `tests/fixtures/partition_inventory/dynamic_disk.json`
- `tests/fixtures/partition_inventory/system_uefi.json`
- `tests/fixtures/partition_inventory/system_bios_mbr.json`
- `tests/fixtures/partition_inventory/bitlocker_locked.json`
- `tests/fixtures/partition_inventory/recovery_between_os_and_free.json`
- `tests/fixtures/smart/healthy_nvme.json`
- `tests/fixtures/smart/failing_sata.json`

### Release Gates

- `cmake --build build --config Release` succeeds.
- `ctest --test-dir build -C Release --output-on-failure` succeeds.
- `scripts/check_release_readiness.ps1` succeeds.
- `scripts/check_partition_manager_certification_matrix_integrity.ps1` verifies every certification ID, evidence contract, harness entry, and certification-doc entry.
- `scripts/check_partition_manager_external_checklist.ps1` verifies the generated external lab checklist against every external matrix gate.
- `scripts/check_partition_manager_external_lab_package.ps1` verifies every generated external run-sheet directory against the matrix.
- `scripts/check_partition_manager_feature_matrix.ps1` verifies every commercial-parity feature group against code, UI, tests, docs, the certification matrix, and certification gates.
- `scripts/run_partition_manager_hardware_certification_strict.ps1` verifies strict VHD plus external evidence before any `HardwareCertified` release claim.
- `scripts/check_partition_manager_release_claims.ps1` verifies README, changelog, audit, plan, certification, and readiness wording matches the generated claim level.
- Manual VM matrix signed off for every destructive operation shipped.
- Accessibility smoke test covers keyboard navigation and screen reader names.
- Dark/light screenshots reviewed for overlap and contrast.
- No destructive action can execute without final Apply.
- No system disk destructive operation is available in v1.

---

## Documentation Deliverables

- `docs/PARTITION_MANAGER_PANEL_PLAN.md` - this implementation plan.
- `docs/PARTITION_MANAGER_CERTIFICATION.md` - destructive VHD harness and VM/hardware/lab gate procedure.
- README roadmap link.
- Future user guide: safe partition operations, clone/migration workflow, wipe warnings.
- Future developer guide: elevated partition task allowlist and test VM setup.
- Future release notes once implementation begins.
- Future troubleshooting guide: common blockers and recovery actions.
- Future VM validation checklist for maintainers after external boot and hardware matrix runs are archived.

---

## Implementation Constraints

- Full third-party-style partition move is not available through Windows built-ins.
- Safe NTFS-to-FAT32/exFAT conversion needs backup/format/restore, not an in-place Windows command.
- OS migration and boot repair need VM validation before release.
- Secure erase on SSDs needs deeper device-specific handling before exposing as more than wipe/retrim guidance.
- Dynamic disks and Storage Spaces are read-only in v1.
- Current OS disk destructive mutation is blocked in v1.

## Ready-To-Start Criteria

- Phase 1 can start from this document.
- No open planning questions block read-only UI and inventory work.
- Required feature list has v1 support decision, backend, reuse path, and risk tier.
- Destructive work has safety model, operation rules, elevated task contracts, and test gates.
- Known constraints are explicit and should be shown in UI when relevant.

---

## References Reviewed

- Smart Disk Analyzer: https://github.com/Dharaneesh20/Smart-Disk-Analyser
- MiniTool Partition Wizard help center: https://www.partitionwizard.com/help/
- MiniTool Partition Wizard feature set: https://www.minitool.com/partition-manager/partition-wizard-home.html
- MiniTool move/resize workflow: https://www.partitionwizard.com/help/resize-partition.html
- MiniTool copy disk wizard: https://www.partitionwizard.com/help/copy-disk.html
- MiniTool migrate OS wizard: https://www.partitionwizard.com/help/migrate-os-to-ssd-hd.html
- MiniTool partition recovery wizard: https://www.partitionwizard.com/help/partition-recovery.html
- AOMEI Partition Assistant manual: https://www.diskpart.com/help/
- AOMEI Partition Assistant feature set: https://www.aomeitech.com/aomei-partition-assistant.html
- AOMEI main window/pending-operation model: https://www.diskpart.com/help/partition-assistant-main-window.amp.html
- AOMEI extend partition wizard: https://www.diskpart.com/help/extend-partition-wizard.html
- AOMEI migrate OS workflow: https://www.diskpart.com/help/migrate-system-wizard.html
- Microsoft MBR2GPT: https://learn.microsoft.com/en-us/windows/deployment/mbr-to-gpt
- Microsoft disk GPT/MBR conversion limits: https://learn.microsoft.com/en-us/windows-server/storage/disk-management/change-an-mbr-disk-into-a-gpt-disk
- Microsoft shrink/extend volume behavior: https://learn.microsoft.com/en-us/windows-server/storage/disk-management/shrink-a-basic-volume
