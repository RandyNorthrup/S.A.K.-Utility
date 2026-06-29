# Release Readiness

## Automated Gates

Required before a release candidate is published:

- Full Release build succeeds.
- Full CTest suite passes with zero failures.
- Secret scan passes.
- Blocking-pattern guard passes.
- Accessibility, raw style-token, raw style-literal, GUI magic-number, global magic-number, logged dialog, Partition Manager certification-matrix-integrity/commercial-destructive-feature-matrix/external-checklist/external-lab-package/certification-gap-report/VHD-preflight/certification-artifact-bundle/feature-matrix/evidence-payload/release-claim/strict-handoff, and Lizard gates pass.
- Third-party license audit passes.
- Full driver-level APFS (A1-A8) and HFS+/HFSX (H1-H8) write is Apple-certified - Apple `fsck_apfs`/`fsck_hfs` + macOS-kernel mount at every milestone, with physical-USB destructive + crash-interruption + rollback at the A8/H8 track gates (evidence `external.apfs-a8-physical`, `external.hfs-h8-physical`). The driver-capability matrix owner is `docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`; this is a distinct scope from the Partition Manager destructive-operations claim level.
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
  `docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Deep XFS/Btrfs tool checks and
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
  and volume-label change
  actions through UI/queue/apply. The certifier-generated format image must detect as
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
- Partition Manager ext/HFS+/APFS image browsers must open both normal image
  files and read-only Windows raw partition aliases through
  `openFileOrRawDeviceReadOnly`; physical Apple proof requires HFS+ and APFS
  root listing and can require selected-file read proof, HFS+ selected-attribute
  proof, plus APFS bounded export proof with
  `scripts/run_partition_manager_physical_apple_probe_validation.ps1 -RequireHfsFileProof -RequireHfsAttributeProof -RequireApfsFileProof -RequireApfsExportProof`.
  Current physical proof read `/Fonts/00TT.TTF` from the expendable APFS
  partition and recorded SHA-256
  `d075a134b3092fd36c6e45acc88d2efd163e60857cb2f0a2621569f446fa06d2`;
  APFS bounded export proof exported `/Fonts` with 63 files, 1 directory,
  64 scanned entries, and 21161830 bytes. The same report records HFS+
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
The current local package proof staged `S.A.K.-Utility-0.9.1.9`, passed portable
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
powershell -NoProfile -ExecutionPolicy Bypass -File `
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
powershell -NoProfile -ExecutionPolicy Bypass -File `
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
32 TiB cap, incl. the A8 physical-USB destructive/crash/rollback gate
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
see `docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Release wording must keep
XFS/Btrfs to read-only metadata, and may state full driver-level APFS + HFS+/HFSX
read/write with the standing hard boundaries: APFS Fusion/Tier2 multi-device out
of scope, encryption credential-gated, sealed-system-volume writes behind a typed
seal-invalidation confirmation, APFS writes gated to S.A.K. generated-layout
containers at the Apply layer, and APFS container shrink + chunk-adding grow as
documented follow-ons.

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
powershell -NoProfile -ExecutionPolicy Bypass -File `
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
powershell -NoProfile -ExecutionPolicy Bypass -File `
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
The minimal machine-readable release evidence snapshot is tracked under
`artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\`
and `artifacts\partition-manager-certification\vm-lab\external-evidence\` so a
clean CI checkout can verify `HardwareCertified` claims without local-only
artifact state.

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
