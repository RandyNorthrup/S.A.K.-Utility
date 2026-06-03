# Partition Manager Certification

This document separates code-complete automated proof from destructive VM and hardware certification. Unit tests and release-readiness gates prove script generation, safety blockers, queue state, UI chrome, and non-destructive behavior. Destructive storage behavior needs disposable media evidence before release claims can say it is hardware-certified.

`docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` is the machine-readable source for certification scenario IDs, required evidence keys, and safety contracts. The harness stamps those contracts into each result, the external-evidence JSON scaffold, Markdown checklist, and per-gate lab package carry them for lab runs, the verifier rejects missing contracts or missing/blank passed-scenario evidence payload values, and the claim-level script counts only evidence that satisfies the matrix with non-empty values.
`scripts/new_partition_manager_certification_gap_report.ps1` and
`scripts/check_partition_manager_certification_gap_report.ps1` turn that status
into JSON/Markdown gap output listing every incomplete VHD or external gate,
evidence key, required value, safety contract, and status-aware next command. A
`VhdDataDiskCertified` gap report points at external-evidence collection and the
per-gate report importer plus strict hardware handoff instead of asking
operators to rerun the VHD matrix.
The per-gate lab package also writes `report.template.json` files so operators
can produce consistent local `report.json` evidence artifacts with the gate ID,
matrix contract, required evidence payload, artifact list, and verification
summary.
`scripts/update_partition_manager_external_manifest_from_reports.ps1` imports
completed per-gate `report.json` files back into the external manifest only
after each report matches the matrix ID, safety contract, required evidence
keys, required values, and non-empty verification summary.
`scripts/new_partition_manager_certification_bundle.ps1` and
`scripts/check_partition_manager_certification_bundle.ps1` bind the current
status, harness report, VHD preflight, gap report, external manifest, and lab
checklist together with SHA-256 hashes for release handoff.
`scripts/check_partition_manager_release_claims.ps1` then checks README,
changelog, audit, plan, certification, and readiness wording against the
generated claim level so release notes cannot accidentally claim more
certification than the evidence supports.
`scripts/run_partition_manager_hardware_certification_strict.ps1` is the final
non-mutating hardware-certification handoff. It verifies an existing strict VHD
evidence root, the external JSON manifest, the paired checklist, and the
per-gate lab package, writes status/gap/bundle artifacts, and exits nonzero
unless the generated claim level is `HardwareCertified`.

## Current Certification State

Current default non-mutating readiness status as of 2026-06-02 UTC / 2026-06-02
PDT is `CodeCompleteOnly`. The latest default release-readiness proof passed on
2026-06-02 09:40 PDT with the non-mutating harness report at
`artifacts\partition-manager-certification\readiness\run-20260602-094050\partition-manager-certification-report.json`.
That readiness run also verified
`artifacts\partition-manager-certification\readiness\external-evidence\`,
`artifacts\partition-manager-certification\readiness\vhd-preflight.json`,
`artifacts\partition-manager-certification\readiness\certification-gap-report.md`,
and
`artifacts\partition-manager-certification\readiness\certification-artifact-bundle.json`.

Strict disposable-VHD status is `VhdDataDiskCertified` when release readiness
uses the strict evidence root. The latest elevated strict VHD proof passed on
2026-06-02 19:49 PDT / 2026-06-03 UTC with 12/12 VHD gates passed, no failures,
and no skipped VHD scenarios at
`artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\partition-manager-certification-report.json`.
The previous strict proof passed on 2026-06-01 20:34 PDT at
`artifacts\partition-manager-certification\vhd-strict\run-20260601-203234\partition-manager-certification-report.json`.
Strict readiness against that evidence passed on 2026-06-01 22:13 PDT with
`-PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict -RequirePartitionVhdEvidence`.
Default readiness still records a non-admin VHD preflight blocker when run from
this shell, while the elevated strict preflight recorded administrator=true and
ready=true. The VM-lab external manifest now has 18/18 external gates passed.
The imported manifest is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`.
The paired imported checklist is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`,
and the importer refreshed the VM-lab run-sheet manifest references to match
that imported manifest before strict handoff.
The imported claim level now reaches `HardwareCertified`: strict VHD proof has
12/12 VHD mutation scenarios passed, and the imported external manifest at
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`
has 18/18 external VM/hardware/lab gates passed with non-empty
matrix-required evidence values and local artifact paths. The paired checklist
is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`.

Live Windows 11 VM verification was added on 2026-06-02 PDT using VirtualBox
7.2.8 and VM `SAK-PM-Lab-Win11` with Windows 11 25H2, one GPT system disk, and
two 4 GB RAW data disks. `SAK_STARTUP_SMOKE_OK` passed from the read-only
VirtualBox shared path `\\vboxsvr\sakrepo\build\Release\sak_utility.exe`, and
the visible Partition Manager UI rendered 3 disks from that shared executable
in
`artifacts\partition-manager-certification\vm-lab\sak-utility-share-partition-manager-mouse-fixed.png`.
That VM pass verifies non-admin inventory and read-only shared-folder startup
after two code fixes: writable data-root fallback for read-only executable
locations and RAW-disk inventory tolerance when `Get-Partition` returns no
partitions.

A follow-up elevated VM data-disk smoke passed on 2026-06-02 07:44 PDT in the
same VM. UAC elevation was confirmed with `is_admin=true`, then the smoke script
mutated only disposable VirtualBox disks 1 and 2 after boot/system/model/size
guards. It created, formatted, and resized a GPT NTFS partition on disk 1,
deleted and cleared it back to RAW, created an MBR FAT32 partition on disk 2,
converted it to NTFS with `convert.exe`, and cleared it back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\partition-vm-admin-report.json`.
The final guest inventory confirmed both 4 GB data disks were RAW, non-boot, and
non-system. This is supplemental VM destructive data-disk evidence only; it does
not upgrade the claim level because it is not a complete external evidence
manifest.

The first matrix-backed external VM gate was completed on 2026-06-02 09:26 PDT:
`external.bitlocker` - BitLocker locked/unlocked data-volume blocker proof. The
VM lab created a disposable 1 GB NTFS data volume on
VirtualBox disk 2, enabled BitLocker used-space-only encryption, recorded the
locked state, verified a write was blocked while locked, unlocked with the
generated recovery password, verified an unlocked write by SHA-256, recorded a
clean dirty-bit query, redacted the disposable recovery password from host
evidence, and cleared disk 2 back to RAW. This proves the locked/unlocked
BitLocker blocker gate only; in-app BitLocker mutation has a separate passed
gate under `external.bitlocker-mutation`.

The second matrix-backed external VM gate was completed on 2026-06-02 18:00
PDT: `external.file-level-data-recovery` - File-level Data Recovery scan and restore proof.
The VM lab created a disposable NTFS data volume, wrote and
deleted `sak-deleted-recovery-fixture.pdf`, scanned the
raw volume read-only with `FileRecoveryEngine`, restored the matching candidate
to a separate destination from captured scan bytes, verified SHA-256
`31080995DA446C2192A63231DFDA08572DA754EAF9D18DEBDE38CDE27F7D703F`, and
verified the scanned source range was unchanged. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery\report.json`.

The third matrix-backed external VM gate was completed on 2026-06-02 18:38 PDT:
`external.bitlocker-mutation` - In-app BitLocker unlock/suspend/resume mutation proof
on disposable encrypted volume. The VM lab enabled BitLocker used-space-only on
a disposable VBOX data volume, locked it, unlocked it with a generated recovery
password, suspended and resumed protection, verified final protection was on,
sanitized command output, and cleared the lab disk. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation\report.json`.

The fourth matrix-backed external gate was completed on 2026-06-02 20:19 PDT:
`external.allocate-free-space` - Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof.
The elevated disposable-VHD lab created adjacent NTFS source and donor volumes,
backed up donor files off the VHD, deleted the
donor partition, extended the source by 128 MiB, recreated the donor from the
remaining space, restored donor data, verified a three-file SHA-256 manifest,
ran clean repair scans on both volumes, and dismounted/removed the VHD.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.allocate-free-space\report.json`.

The fifth matrix-backed external gate was completed on 2026-06-02 21:03 PDT:
`external.cluster-size-change` - Existing-volume cluster-size change proof.
The elevated disposable-VHD lab created an NTFS volume, backed up fixture data
with file hashes, ACLs, and an alternate data stream, reformatted the same
volume from 4096-byte to 16384-byte allocation units, restored data, matched
the SHA-256 manifest, preserved four ACL entries and one alternate data stream,
ran a clean repair scan, and removed the VHD. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cluster-size-change\report.json`.

The sixth matrix-backed external VM gate was completed on 2026-06-02 22:41 PDT
/ 2026-06-03 UTC: `external.hdd-defrag-execution` - Direct in-app HDD defrag
execution proof. The elevated Windows 11 VirtualBox lab targeted only
disposable 4 GB `VBOX HARDDISK` disk 1 after boot/system/model/size guards,
created an NTFS fixture volume, ran `Optimize-Volume -Analyze` and
`Optimize-Volume -Defrag`, ran `Repair-Volume -Scan` with no file-system
problems, and cleared the disk back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution\report.json`.

The final boot-action external gates were completed on 2026-06-03 PDT.
`external.os-migration-reboot` booted a target-only cloned OS VM to Guest
Additions run level 3. `external.boot-repair-uefi` and
`external.boot-repair-bios` both captured intentionally broken BCD symptoms and
verified offline repair back to run level 3. `external.system-mbr2gpt` validated
and converted disposable BIOS/MBR disk 0 with `mbr2gpt` exit code 0, changed
the disk to GPT with an EFI System partition, and booted the converted disk in
fresh EFI VM `SAK-PM-MBR2GPT-EFI-Boot-20260603-1140` to Guest Additions run
level 3. The passed report is
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.system-mbr2gpt\report.json`.

Commercial parity wording remains evidence-scoped. Current AOMEI and MiniTool
feature pages still include operations whose direct execution is not certified
here, including dynamic/basic conversion, primary/logical conversion,
non-adjacent move-location/allocation, live-drive file recovery, SSD secure
erase, boot/reboot proof, removable-media proof, and physical wipe proof.
S.A.K. now has production code for adjacent-donor Allocate Free Space,
existing-volume cluster-size backup/reformat/restore/hash verification, direct
HDD defrag execution, and BitLocker mutation paths, but release notes must not call those destructive
paths 100% hardware-certified until strict VHD plus all external gates pass.

## Certification Harness

Use `scripts/run_partition_manager_destructive_certification.ps1` from an elevated PowerShell session. The script creates temporary VHDX files under `artifacts/partition-manager-certification`, attaches them, mutates only those VHD-backed disks, verifies results, and writes a JSON report. Non-admin runs are accepted for plan-only reporting, but VHD mutation scenarios are skipped with an Administrator-shell blocker.

Before requesting UAC or starting the destructive VHD run, use the read-only
preflight:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_partition_manager_vhd_preflight.ps1 `
  -OutputPath artifacts/partition-manager-certification/vhd-preflight.json `
  -RequireAdministrator `
  -RequireReady
```

The preflight does not create, attach, detach, format, wipe, or mutate disks. It
checks administrator state, required Storage cmdlets and Windows tools,
temporary T-Z drive-letter availability, workspace estimate, VHD size, and
matrix scenario counts. Release readiness also writes `vhd-preflight.json` and
verifies its shape without failing normal non-admin shells.

Plan-only run, no disk mutation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_destructive_certification.ps1
```

Disposable VHD data-disk matrix:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_destructive_certification.ps1 `
  -RunVhdDataDiskMatrix
```

Disposable VHD matrix with automatic UAC relaunch and a strict evidence gate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_destructive_certification.ps1 `
  -RunVhdDataDiskMatrix `
  -RelaunchElevated `
  -RequireVhdDataDiskEvidence
```

The strict gate exits nonzero if any VHD scenario is skipped, so release evidence cannot silently pass from a non-admin shell.

For the full strict VHD handoff sequence, use the orchestrator. It runs the
preflight with `-RequireReady`, runs the destructive disposable-VHD matrix,
verifies strict VHD evidence, writes claim status, writes gap and bundle
artifacts, and exits nonzero unless every disposable-VHD scenario passes:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_vhd_certification_strict.ps1 `
  -RelaunchElevated
```

After the strict VHD report and every external VM/hardware/lab gate are complete,
use the strict hardware handoff. It does not mutate disks; it verifies the
archived VHD report plus the external evidence package and fails unless the
claim level is `HardwareCertified`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_hardware_certification_strict.ps1 `
  -CertificationRoot artifacts\partition-manager-certification\vhd-strict `
  -ExternalEvidenceManifest artifacts\partition-manager-certification\external-evidence.json `
  -ExternalEvidenceChecklist artifacts\partition-manager-certification\external-evidence.checklist.md `
  -ExternalEvidenceRoot artifacts\partition-manager-certification\external
```

## Evidence Verification

Use `scripts/verify_partition_manager_certification.ps1` to validate a generated report before making release claims:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_partition_manager_certification.ps1 `
  -ReportPath artifacts/partition-manager-certification/run-YYYYMMDD-HHMMSS/partition-manager-certification-report.json
```

Require every disposable-VHD scenario to pass:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_partition_manager_certification.ps1 `
  -ReportPath artifacts/partition-manager-certification/run-YYYYMMDD-HHMMSS/partition-manager-certification-report.json `
  -RequireVhdDataDiskEvidence
```

Require external VM/hardware proof with a manifest:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/new_partition_manager_external_evidence_manifest.ps1 `
  -OutputPath artifacts/partition-manager-certification/external-evidence.json `
  -ChecklistPath artifacts/partition-manager-certification/external-evidence.checklist.md `
  -EvidenceRoot artifacts/partition-manager-certification/external `
  -CreateEvidenceDirectories

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/update_partition_manager_external_manifest_from_reports.ps1 `
  -ManifestPath artifacts/partition-manager-certification/external-evidence.json `
  -EvidenceRoot artifacts/partition-manager-certification/external `
  -OutputPath artifacts/partition-manager-certification/external-evidence.imported.json `
  -ChecklistPath artifacts/partition-manager-certification/external-evidence.checklist.md `
  -OutputChecklistPath artifacts/partition-manager-certification/external-evidence.imported.checklist.md `
  -RequireAllReports

powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_partition_manager_certification.ps1 `
  -ReportPath artifacts/partition-manager-certification/run-YYYYMMDD-HHMMSS/partition-manager-certification-report.json `
  -RequireVhdDataDiskEvidence `
  -RequireExternalGateEvidence `
  -ExternalEvidenceManifest artifacts/partition-manager-certification/external-evidence.imported.json
```

External evidence manifest shape:

```json
{
  "tool": "partition-manager-external-certification",
  "results": [
    {
      "id": "external.system-mbr2gpt",
      "status": "Passed",
      "required_evidence_keys": [
        "vm_id",
        "before_partition_style",
        "mbr2gpt_validate_exit_code",
        "mbr2gpt_convert_exit_code",
        "after_partition_style",
        "boot_result"
      ],
      "safety_contract": [
        "disposable_boot_vm_only",
        "pre_conversion_snapshot",
        "validate_before_convert",
        "post_conversion_boot_verified"
      ],
      "evidence": {
        "vm_id": "hyperv-mbr2gpt-001",
        "before_partition_style": "MBR",
        "mbr2gpt_validate_exit_code": 0,
        "mbr2gpt_convert_exit_code": 0,
        "after_partition_style": "GPT",
        "boot_result": "booted Windows after conversion"
      },
      "evidence_path": "artifacts/partition-manager-certification/external/system-mbr2gpt/report.json"
    }
  ]
}
```

The optional checklist is generated from the same certification matrix as the JSON manifest. Use it as the lab run sheet, but treat the JSON manifest plus linked evidence artifacts as the authoritative machine-verified evidence.
`scripts/check_partition_manager_external_checklist.ps1` verifies that the generated checklist includes every external gate, safety contract, required evidence key, required value hint, and artifact path from the matrix.
`scripts/check_partition_manager_external_lab_package.ps1` verifies the optional per-gate evidence package directories, run sheets, and `report.template.json` files. Those directories are lab handoff scaffolding only; they do not count as passed evidence until completed `report.json` files are imported into the manifest or the manifest is otherwise filled with non-empty matrix-required values plus real `evidence_path` files or valid HTTPS `evidence_url` links.
When `scripts/update_partition_manager_external_manifest_from_reports.ps1`
imports per-gate reports, pass `-ChecklistPath` and `-OutputChecklistPath` so
the strict handoff receives a checklist whose manifest line matches the
imported manifest. The importer also refreshes run-sheet README and
`report.template.json` manifest references under the evidence root.

Each external result must match a documented external gate, have `status` set to `Passed`, include every matrix-required key in `evidence`, fill each required value with non-null/non-empty evidence, and include either `evidence_path` or `evidence_url`.
When `evidence_path` is used for a passed gate, the verifier requires that local artifact to exist. When `evidence_url` is used, it must be an absolute HTTP or HTTPS URL. Pending scaffold entries are accepted only when strict external evidence is not requested.

The verifier rejects unexpected or duplicate scenario IDs, missing schema version, malformed timestamps, mismatched summary counts, failed harness scenarios, missing external artifacts for passed gates, and malformed evidence URLs.
For passed VHD and external scenarios, it also rejects missing or blank required evidence values such as disk identity, size/layout verification, sentinel checks, file-system conversion proof, boot proof, BitLocker state, or hardware wipe proof. Matrix-defined required values are enforced too, so the UEFI boot-repair gate must record UEFI mode and the BIOS/MBR boot-repair gate must record BIOS or Legacy BIOS mode. Numeric zero and boolean false remain valid recorded values when the scenario contract expects them. For external manifests, it requires the matrix-backed safety contract, a complete non-empty `evidence` payload, plus either an existing `evidence_path` or a valid HTTP/HTTPS `evidence_url` when strict external evidence is requested.

Generate deterministic release-claim wording from the same evidence:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/get_partition_manager_certification_status.ps1 `
  -ReportPath artifacts/partition-manager-certification/run-YYYYMMDD-HHMMSS/partition-manager-certification-report.json `
  -ExternalEvidenceManifest artifacts/partition-manager-certification/external-evidence.json
```

Claim levels are:

- `CodeCompleteOnly` - automated code, UI, queue, script, and report checks pass, but VHD or VM/hardware/lab evidence is incomplete.
- `VhdDataDiskCertified` - every disposable-VHD data-disk scenario passed.
- `HardwareCertified` - every disposable-VHD scenario and every external VM/hardware/lab gate passed with evidence.
- `FailedEvidence` - a certification report contains a failed scenario.

Run the non-destructive tool regression self-test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_partition_manager_certification_tools.ps1
```

The self-test creates synthetic reports and manifests under `artifacts/`, verifies every claim level, checks the generated external lab checklist and lab package, imports completed per-gate `report.json` files into a manifest, exercises the strict hardware orchestrator with complete synthetic evidence, and confirms malformed, missing, blank, or wrong-value evidence plus stale checklist/lab-package gate/evidence/value rows are rejected.

Run release readiness against an existing strict VHD evidence root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 `
  -PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict `
  -RequirePartitionVhdEvidence
```

The default release-readiness path stays non-mutating and uses plan-only
certification evidence. It writes `external-evidence.template.json`,
`external-evidence.checklist.md`, an `external-evidence/` per-gate lab package,
and `vhd-preflight.json` under the selected certification root so the lab
checklist, lab package, manifest, and VHD readiness blockers stay
matrix-synchronized. It also writes `certification-gap-report.json`,
`certification-gap-report.md`, and `certification-artifact-bundle.json`, then
verifies that those files exactly match the current certification status,
matrix, artifact hashes, and linked report paths. The
strict form above does not create a new VHD run; it requires an existing report
under the selected certification root. Add `-PartitionExternalEvidenceManifest`,
`-PartitionExternalEvidenceChecklist`, and `-RequirePartitionExternalEvidence`
only after the external VM/hardware matrix is complete. Strict external evidence
requires the paired checklist so hardware-certified wording cannot bypass the
human lab sheet verifier.

Keep generated VHDs for manual inspection:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_partition_manager_destructive_certification.ps1 `
  -RunVhdDataDiskMatrix `
  -KeepVhd
```

## VHD Matrix Coverage

The VHD matrix covers Windows-supported destructive data-disk flows without touching existing disks:

- `vhd.create-format-resize-delete` - Create, format, resize, and delete data partition.
- `vhd.fat32-to-ntfs` - FAT32 to NTFS in-place conversion.
- `vhd.quick-partition` - Quick Partition equal-size data-disk layout.
- `vhd.quick-partition-custom` - Quick Partition custom size and label layout.
- `vhd.quick-partition-mbr` - Quick Partition MBR four-data-partition layout, including any Windows-created extended/logical container for the MBR fourth data partition.
- `vhd.adjacent-extend` - Adjacent free-space Extend Partition Wizard path.
- `vhd.recovered-partition-restore` - Recovered partition write-back candidate restore from offset, size, and GPT type ID.
- `vhd.empty-style-conversion` - Empty data disk GPT/MBR conversion.
- `vhd.clear-disk-wipe` - Clear-level non-system disk wipe.
- `vhd.image-clone` - Offline VHD image clone and sentinel verification.
- `vhd.image-restore` - Offline VHD image restore and overwrite verification.
- `vhd.partition-clone-region` - Partition clone to raw target region with signature and outside-marker verification.

Each scenario produces `Passed`, `Failed`, or `Skipped` evidence in `partition-manager-certification-report.json`.

## External Gates

These scenarios need disposable VM or hardware lab proof. The imported VM-lab
manifest currently has 18/18 external gates passed: `external.bitlocker`,
`external.bitlocker-mutation`, `external.file-level-data-recovery`,
`external.allocate-free-space`, `external.cluster-size-change`,
`external.hdd-defrag-execution`, `external.usb-removable`,
`external.ssd-retrim`, `external.ssd-secure-erase`,
`external.partition-move`, `external.primary-logical-conversion`,
`external.volume-serial-number`, `external.dynamic-to-basic`, and
`external.hardware-wipe`, `external.os-migration-reboot`,
`external.boot-repair-uefi`, `external.boot-repair-bios`, and
`external.system-mbr2gpt`.

- `external.system-mbr2gpt` - System MBR-to-GPT conversion in disposable boot VM. Passed. This also satisfies
  System MBR-to-GPT conversion in a disposable boot VM. Disposable BIOS/MBR VM
  `SAK-PM-BIOS-MBR-Fixture-20260603-091138` validated and converted disk 0
  with `mbr2gpt` exit code 0, changed layout from MBR to GPT, created an EFI
  System partition, and the converted disk booted under fresh EFI VM
  `SAK-PM-MBR2GPT-EFI-Boot-20260603-1140` to Guest Additions run level 3.
- `external.os-migration-reboot` - OS migration target boot and firmware-order proof. Passed. A target-only cloned OS VM booted
  from the cloned disk with UEFI firmware and reached Guest Additions run level
  3.
- `external.boot-repair-uefi` - UEFI boot repair with intentionally broken BCD. Passed. A disposable UEFI boot disk was
  snapshotted, BCD/fallback loader were removed, broken boot was captured, and
  offline `bcdboot` repair restored a run-level-3 boot.
- `external.boot-repair-bios` - BIOS/MBR boot repair with intentionally broken BCD. Passed. A disposable BIOS/MBR boot disk was
  snapshotted, `\Boot\BCD` was removed, Windows Recovery `0xc000000f` was
  captured, and offline `bcdboot` plus `bootsect` restored a run-level-3 boot.
- `external.usb-removable` - USB removable disk destructive operation proof.
- `external.ssd-retrim` - SSD/NVMe ReTrim and vendor purge warning proof.
- `external.hdd-defrag-execution` - Direct in-app HDD defrag execution proof on disposable HDD volume.
- `external.ssd-secure-erase` - SSD/NVMe secure erase command execution proof on disposable SSD/NVMe device.
- `external.partition-move` - Offline partition start-move proof.
- `external.cluster-size-change` - Existing-volume cluster-size change proof.
- `external.primary-logical-conversion` - Primary/logical conversion on disposable MBR disk. Primary/logical conversion on a disposable MBR disk also requires extended-container identity, logical-volume identity, before/after order and offsets, mount validation, file-hash validation, and bootability proof where applicable.
- `external.volume-serial-number` - Volume serial-number metadata mutation proof.
- `external.dynamic-to-basic` - Dynamic disk to basic conversion proof in a disposable VM.
- `external.hardware-wipe` - Non-system hardware wipe on disposable physical disk.

The harness records these as `ExternalGate` IDs, and the VM-lab imported manifest now has passed `report.json` evidence for every listed gate.

## Release Rule

Release readiness can pass in default non-mutating mode for code-complete behavior, or in strict mode with imported evidence for `HardwareCertified`. Release notes and UI copy must keep that distinction clear:

- Automated gates prove non-destructive logic and generated scripts.
- VHD matrix proves disposable data-disk destructive flows.
- VM/hardware/lab matrix proves system MBR-to-GPT, OS migration reboot proof, UEFI/BIOS boot repair, removable USB, SSD/NVMe, HDD defrag execution, SSD secure erase, non-adjacent partition move, primary-logical/serial/dynamic conversion, and physical wipe flows. BitLocker, BitLocker mutation, file-level recovery, adjacent-donor Allocate Free Space, existing-volume cluster-size change, and HDD defrag execution are also imported; the full 18/18 external evidence set now upgrades the strict handoff to `HardwareCertified`.
- The verifier script enforces those boundaries when strict release evidence is required.
- Release readiness runs the certification-matrix integrity guard, syntax-checks the strict VHD and strict hardware orchestrators, then runs the harness in plan-only mode, creates an external-evidence JSON scaffold plus human lab checklist plus per-gate lab package from the same matrix, writes and verifies the read-only VHD preflight report, verifies the checklist and lab package, verifies both report shape and manifest shape, writes a certification-status summary, writes and verifies JSON/Markdown certification-gap reports, writes and verifies a hashed certification-artifact bundle, and runs the non-destructive certification-tool self-test, so certification schema drift fails without touching disks.
- Certification scenario IDs, evidence keys, and safety contracts must stay in `docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`; update the matrix and self-test together when adding or changing a certification gate.
- Release readiness runs the release-claim guard after generating certification status, so documentation wording remains tied to the current `CodeCompleteOnly`, `VhdDataDiskCertified`, `HardwareCertified`, or `FailedEvidence` state.
