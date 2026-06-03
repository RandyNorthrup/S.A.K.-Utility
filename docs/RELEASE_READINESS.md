# Release Readiness

## Automated Gates

Required before a release candidate is published:

- Full Release build succeeds.
- Full CTest suite passes with zero failures.
- Secret scan passes.
- Blocking-pattern guard passes.
- Accessibility, raw style-token, raw style-literal, GUI magic-number, global magic-number, logged dialog, Partition Manager certification-matrix-integrity/commercial-destructive-feature-matrix/external-checklist/external-lab-package/certification-gap-report/VHD-preflight/certification-artifact-bundle/feature-matrix/evidence-payload/release-claim/strict-handoff, and Lizard gates pass.
- Third-party license audit passes.
- QRC resource verification passes.
- Portable package smoke passes from a clean extracted folder.
- Startup E2E smoke passes from the packaged folder.
- All packaged `.exe` and `.dll` files have valid Authenticode signatures.
- `SHA256SUMS.txt` is regenerated after signing.

Use:

```powershell
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"

powershell -ExecutionPolicy Bypass -File scripts/stage_portable_release.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

powershell -ExecutionPolicy Bypass -File scripts/create_release_archive.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

$extract = "build\Release\clean-readiness-extract"
Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $extract | Out-Null
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract -Force

powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 `
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
powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 `
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
If `docs/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` gains a VHD scenario,
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

Fourteen matrix-backed external gates now pass in VM-lab evidence:
`external.bitlocker`, `external.bitlocker-mutation`,
`external.file-level-data-recovery`, `external.allocate-free-space`,
`external.cluster-size-change`, `external.hdd-defrag-execution`,
`external.usb-removable`, `external.ssd-retrim`,
`external.ssd-secure-erase`, `external.partition-move`,
`external.primary-logical-conversion`, `external.volume-serial-number`,
`external.dynamic-to-basic`, and `external.hardware-wipe`.
Evidence lives under
`artifacts\partition-manager-certification\vm-lab\external-evidence\`, with
partial imported manifest
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`
and paired imported checklist
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`.
The current imported status is `HardwareCertified`: 18/18 external gates
passed and strict VHD evidence remains 12/12 passed. The final gates completed
on 2026-06-03 PDT:

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
`-RequirePartitionVhdEvidence -RequirePartitionExternalEvidence` passes and the
release-claim guard accepts the completed certification wording.

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
