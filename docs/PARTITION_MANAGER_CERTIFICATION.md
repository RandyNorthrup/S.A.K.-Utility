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

Partition Manager ext filesystem write proof passed on 2026-06-05 PDT in the
same VM. The elevated gate targeted only disposable VirtualBox disk 1, created
an unmounted GPT data partition, used the app-style raw path
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, verified bundled e2fsprogs
hashes, formatted ext4 with `mke2fs`, ran clean `e2fsck -p` repair and
read-only checks, grew the partition, ran `resize2fs`, shrank the file system
before shrinking the partition, rechecked clean, and
cleared disk 1 back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
The destructive runner now refuses to proceed outside a detected VirtualBox
guest unless `-AllowNonVirtualBoxGuest` is explicitly supplied for a disposable
non-VirtualBox lab VM, so `-Force` alone is not enough on a normal host.
This certifies the queued ext format/repair/grow/shrink path. HFS+/XFS/Btrfs/APFS
write paths remain blocked pending operation-specific destructive proof.

A later ext direct-admin automation rerun passed on 2026-06-05 PDT /
2026-06-06 UTC in `SAK-PM-Automation-Win11`. The host launcher used the real
guest password file, waited for Guest Additions desktop readiness with retries,
validated an already high-integrity administrator token through guestcontrol,
and recorded `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`. Evidence:
`artifacts\partition-manager-certification\vm-lab\host-launch\ext-filesystem-20260605224315.json`
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
This supplemental cross-filesystem proof is not part of the imported 18/18
commercial external matrix unless that matrix is expanded with non-Windows
filesystem gates.

Physical cross-filesystem destructive proof passed on 2026-06-05 PDT /
2026-06-06 UTC against an expendable external USB NVMe disk after the read-only
Apple HFS+/APFS evidence was captured. The local launcher used Windows UAC
`runas` auth instead of keypress automation. The elevated runner required admin,
`-Force`, non-boot/non-system guards, USB media by default, and serial or
friendly-name matching for the large physical disk. It formatted ext2, ext3,
and ext4 on app-style `\\?\GLOBALROOT\Device\Harddisk2\Partition2` raw targets,
ran clean `e2fsck -p` repair and read-only checks, grew and shrank each file
system with `resize2fs`, verified S.A.K. raw probes after format/grow/shrink,
wrote and verified Linux swap SWAPSPACE2 metadata, detected Linux swap with
`partition_filesystem_probe_certifier.exe`, and cleared the disk back to RAW.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive\report.json`.
This supplemental physical proof does not approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth,
compressed-file writes,
arbitrary APFS media mutation, non-generated APFS file writes/resize/arbitrary repair,
XFS/Btrfs writes, or deep XFS/Btrfs tool checks.
APFS writer infrastructure includes fail-closed preflight, Fletcher-64 object
checksum calculation/stamping/verification, deterministic mutation planning,
generated/minimal raw-target format/write/patch/delete, empty root-directory
create/delete, volume-label change, repair, and structured execution-evidence
validation. The writer now also builds an APFS format scratch image with NXSB, object maps, APSB,
root tree, spaceman accounting, stamped APFS object checksums, SHA-256 report
output, and optional generated root seed-file read-back through a contiguous
multi-block extent. It also has generated-image root-file write/patch/delete proof:
the write path copies the source to a separate scratch image, adds or replaces
one bounded root regular file, preserves existing bounded root regular files,
updates root-tree/volume/spaceman metadata, and requires post-write
listing/read-back proof. The patch path copies the source to a separate scratch
image, applies a bounded byte-range update inside an existing root regular file,
preserves sibling root files, rejects out-of-range writes, and requires exact
read-back proof. The delete path removes one bounded root regular file, zeroes
the previous generated file-data region, preserves sibling root files, and
requires deleted-file hash plus negative read-back proof. The directory path
creates or deletes one empty generated-layout root directory, preserves bounded
root files and existing empty root directories across rewrites, verifies the
created directory is empty, and verifies deleted-directory absence. It also has
generated-image repair proof for
recognized APFS metadata object checksums: the repair path copies the source to
a separate scratch image, restamps corrupt supported object checksums, and
requires post-repair detection/list/read-back proof. The execution gate requires
matching evidence ID, operation,
target path, structure mapping, checksum vectors, source/scratch image hash
chain, copy-on-write checkpoint proof, object-map update proof, space-manager
accounting proof, fsck/diskutil validation, target read-back proof,
crash-replay proof, rollback-boundary proof, and artifact paths before even an
image-only plan can be considered executable. Raw-media APFS exposure is
currently limited to S.A.K.-generated one-spaceman-chunk targets from 64 MiB
through 128 MiB; larger generated raw targets remain blocked because Apple
kernel validation rejected the previous 51 GB generated spaceman geometry even
though Windows-side detection accepted it.
That ceiling is now lifted for the validated multi-chunk range: the writer emits multi-spaceman-chunk single-CIB containers, and the new geometry passed Apple-native validation in a qemu/KVM macOS Sequoia recovery VM (apfs.kext 2332.101.1) on 2026-06-14 UTC. Each S.A.K.-written container was attached as a bare raw SATA disk; macOS auto-synthesized and auto-mounted every container read-write (the volume superblock records `last modified by apfs_kext (2332.101.1)`), and `fsck_apfs` reported `The container ... appears to be OK` for all of: 64 MiB single-chunk (`SAK1`), 256 MiB multi-chunk (`SAK2CHUNK`), and 1 GiB eight-chunk (`SAK8`). A kernel read-write round-trip (mount, `mkdir` + `touch`, `sync`, unmount) advanced the checkpoint transaction id (4 to 8 on the single-chunk container, 4 to 7 on the 256 MiB multi-chunk) and `fsck_apfs` still reported the container OK afterward, proving the kernel can mutate the S.A.K.-written container and keep it consistent. Evidence: `artifacts\partition-manager-certification\vm-lab\apple-tool-evidence\report.json` plus the paired `*.fsck_apfs-PASS.png` and `*kernel-rw-roundtrip-PASS.png` screendumps. This is Apple-native VM validation on virtual SATA media; the physical-USB destructive Apple-native lane, plus crash-injection and rollback-boundary evidence, are still required before the certification matrix flips to a full APFS raw-write `yes`. The writer now emits a cryptographically-random v4 UUID per container and volume at format and reads the on-disk UUID back during mutation (no deterministic UUID code remains): two same-size, same-name containers produced distinct UUIDs, a mutation round-trip preserved the on-disk UUID byte-for-byte, and the random-UUID shipping output was re-certified in the same VM (`SAK1`/`SAK2CHUNK`/`SAK8` all `fsck_apfs` OK with `*.random-uuid.fsck_apfs-PASS.png` evidence).
A crash/rollback test was also run on 2026-06-14 UTC: with the 1 GiB `SAK8` container mounted read-write, a `mkfile 900m` write was started and qemu was hard-killed (`pkill -9`) mid-write to simulate power loss. On reboot macOS auto-mounted `SAK8` again (APFS checkpoint recovery), and `fsck_apfs` reported `The container /dev/rdisk0 appears to be OK` (checkpoint transaction id 5) - the uncommitted mid-write data rolled back to the last valid checkpoint with no corruption, proving the S.A.K.-written container participates correctly in APFS copy-on-write checkpoint atomicity (evidence `apfs.sak8-crash-rollback.fsck_apfs-PASS.png`). The physical-USB destructive Apple-native lane was then completed on 2026-06-14 UTC against the disposable JMicron Tech USB disk (Windows Disk 3, user-authorized destruction). A 128 MiB S.A.K. APFS container (volume `SAKUSB`, random UUID) was written to the disk's Apple_APFS GPT partition, the whole physical disk was attached to WSL2 with `wsl --mount --bare \\.\PHYSICALDRIVE3` and passed to the macOS recovery VM as a raw SATA disk; macOS recognized it as `Apple_APFS Container disk3` on physical `/dev/disk2s2`, auto-mounted `SAKUSB` read-write (`last modified by apfs_kext (2332.101.1)`), and `fsck_apfs` reported `The container /dev/rdisk2s2 appears to be OK` (evidence `apfs.sakusb-physical-jmicron.fsck_apfs-PASS.png`). With Apple `fsck_apfs`, kernel read-write mount, physical-USB-destructive, crash, and rollback all now demonstrated on Apple's own tools (recorded in `apple-tool-evidence\report.json` under `evidence_chain_complete`), the full evidence required by the locked scope for the APFS multi-chunk space-manager capability (driver matrix A-a / milestone A1) was collected and human-reviewed on 2026-06-14, and that cell is flipped to certified in `docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md` (single-CIB multi-chunk, 64 MiB–1 GiB validated; the multi-CIB/CAB tier, the full-driver A8 gate, and the in-place-COW / snapshot / multi-volume / encryption rows A-b..A-h remain open).
The A2 generated-APFS CAB tier was Apple-certified on 2026-06-18 UTC, completing the
multi-CIB/CAB FORMAT lane left open above. Containers larger than ~7.8 TiB need more than
507 chunk-info blocks, so the spaceman can no longer list the cib addresses inline; the
writer now emits the CAB tier (`cab_count > 0`), where the spaceman publishes a
`cab`-address array pointing at `apfs_cib_addr_block` objects (`SPACEMAN_CAB`, type 0x06),
each holding up to 507 cib block numbers. Only `cab 0` rotates (it references the rotating
`cib 0`); `cab 1..N-1` are immutable, mirroring the certified cib-0 rotation, and the CAB
blocks live inside the (grown) internal-pool reserved prefix so the existing chunk-bitmap
allocation accounting already covers them. Apple `fsck_apfs` was run on three CAB
containers spanning the production range, each formatted, attached to a qemu/KVM macOS
Sequoia VM, unmounted, and checked: 8 TiB (`cab_count 2`, 65536 chunks / 521 cibs, volume
`CABTEST`, USB), the 24 TB A8 drive size (`cab_count 3`, volume `CAB24TB`, SATA), and 24 TiB
(`cab_count 4`, the production ceiling and highest cab_count, volume `CAB24TIB`, SATA). Every
run reported `The volume ... appears to be OK` and `The container ... appears to be OK` with
`Verifying allocated space` clean. The APFS kernel extension (`apfs_kext 2332.101.1`)
auto-mounted the containers read-write and committed to them (the 8 TiB and 24 TB volume
superblocks record `last modified by apfs_kext`, checkpoint advanced from the writer's xid 2
to xid 4). The apfsprogs `apfsck` reference checker independently validated `cab_count` 2/3/4
clean at 7.81 TiB, 8 TiB, 16 TiB, the 24 TB A8 drive size, and 24 TiB; targets past the
writer's ~48 TiB ip-bitmap-ring ceiling fail closed. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-cab-tier-cloud\report.json`
plus `apfs-cab-fsck-apfs-clean.png`. The production format/repair cap is raised to 24 TiB
(covering the A8 drive); the multi-CIB/CAB in-place-COW mutation lane stays single-chunk.
The writer now also formats existing image targets in place only after explicit
destructive wipe confirmation, clears stale edge signatures, writes generated
APFS metadata through a seekable-device block writer, and proves post-format
APFS detection plus empty-root listing in unit coverage. Generated-layout APFS
volume-label change now updates the APSB volume-name field only after
generated-layout verification, restamps the APFS object checksum, reports
old/new labels, and verifies read-back through the APFS browser in local image
coverage. The production
`sak_apfs_writer_cli.exe` bridge exposes generated APFS create/format,
generated-layout checksum repair, and generated-layout root-file write/patch/delete
plus empty root-directory create/delete and generated-layout volume-label change
through queue/apply with raw partition identity, explicit confirmation, helper
JSON output, and refresh evidence. Generated-image root-file
create/replace/byte-range patch/delete and generated-image empty root-directory
create/delete plus generated-image volume-label change remain helper-exposed
with payload/read-back, directory-empty, old/new-label, or negative-read-back
hashes. Arbitrary Apple APFS mutation remains blocked.
The helper no longer blanket-enables protected, compressed, snapshot, or
multi-volume APFS mutation options; generated APFS v2 single-volume layouts pass
through the generated-layout proof path, while active snapshot/revert metadata,
unsupported incompatible feature flags, and multi-volume containers remain
blocked without future operation-specific proof.
APFS generated-image and generated/raw-target mutation now enforce a
S.A.K.-generated/minimal layout guard before writing: fixed NXSB, object-map,
APSB, root-tree, spaceman headers, checksums, object-map references, root-tree
bounds, and free-space accounting must match the generated layout. The repair
path tolerates one or more bad checksums only on those known generated metadata
objects and scans only the known generated metadata blocks, not arbitrary APFS
objects. The guard also checks that generated spaceman chunk and CIB geometry
matches the certified one-chunk layout before mutation.
HFS+ arbitrary-write work has started with a constrained original-code core
writer/helper slice: image-only data-fork overwrite, data/resource-fork
allocated-block grow/shrink replacement, bounded initial-extent data/resource/fork-backed-attribute
allocation growth, explicit zero-length truncate, constrained empty-file and
empty-folder create/delete, bounded file create with data-fork allocation,
single-leaf catalog rename/move,
allocated-file delete, optional released-block zeroing for file/folder-tree delete,
bounded folder-tree delete, and inline and fork-backed
attribute replacement through `PartitionHfsFileSystemWriter` and
`sak_hfs_writer_cli.exe`. It requires explicit writer enablement, target
confirmation, certification evidence ID, journal override when the source
volume is journaled, compressed-file blocking through `com.apple.decmpfs`, JSON
helper output, catalog or attribute logical-size updates, allocation-bitmap/free-block-counter
read-back for growth, delete/create operations, and secure released-block wipe,
stale-tail zeroing on shrink/truncate,
broad allocation-growth blocking, inline-attribute record-capacity enforcement,
and read-back SHA-256 verification.
The HFS File Partition Manager action now queues the same bounded data-fork,
resource-fork, truncate, empty-file/folder create/delete, bounded file create
with data, single-leaf catalog rename/move, allocated-file delete, optional released-block zeroing, bounded folder-tree delete, inline-attribute,
fork-backed-attribute replacement, and bounded fork-backed-attribute growth
mutation paths for selected HFS+/HFSX raw partitions through HFS logical-size
sparse staging, exact raw-target validation, staged `fsck_hfs` repair/check,
changed-range copyback, Apply review, and operation reports. The physical HFS File Apply proof passed
on pinned expendable USB HFS media at
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`;
it staged 262,144 logical HFS bytes, replaced the data fork, resource fork, and
inline `com.apple.FinderInfo` attribute, created/deleted a file and an empty
folder tree with optional secure released-block zeroing, and the refreshed
2026-06-08 UTC physical lane staged a catalog rename before secure delete with
read-back proof. It verified the helper
zeroing warnings plus final root-listing absence, tolerated journaled `fsck_hfs`
exit code 8 only for that explicitly allowed journaled workflow, copied one
sparse range back, and verified raw read-back SHA-256 hashes.
Apple-native HFS+ validation was added on 2026-06-14 UTC in the qemu/KVM macOS Sequoia recovery VM: Apple `newfs_hfs` created a 1 GiB non-journaled HFS+ volume (`SAKHFS`, baseline `fsck_hfs` OK), `sak_hfs_writer_cli.exe create-file-image` (userspace, no kernel driver) added `/sak-cert.txt` (54 bytes) and synchronized the alternate volume header, macOS then auto-mounted the mutated volume and `cat` returned the exact written payload (Apple's HFS+ kernel driver read the S.A.K.-written file byte-for-byte), and `fsck_hfs -f` reported `The volume SAKHFS appears to be OK`. Evidence: `artifacts\partition-manager-certification\vm-lab\apple-tool-evidence\hfs.sak-create-file.kernel-mount-readback-PASS.png` and `hfs.sak-create-file.fsck_hfs-PASS.png`. The created file initially recorded `_unknown` owner and an epoch mtime; the writer was then updated (2026-06-14) to stamp the catalog create/contentMod/attributeMod/access dates with the real time and HFSPlusBSDInfo `owner=0 group=0 fileMode=0o100644` (files) / `0o040755` (folders). Byte-level verification of a freshly created record confirmed `createDate=2026-06-14 07:43:25, owner=0, group=0, mode=0x81a4`, and clang-format/lizard/cppcheck plus the full test suite pass. Bounded initial-extent
data/resource/fork-backed-attribute allocation growth, single-leaf catalog rename/move,
and bounded file create/delete are covered by core/helper tests; raw unbounded HFS+ file/folder create/delete,
complex HFS+ file delete, HFS+ B-tree split/rebalance, inline/broad HFS+ attribute
growth, compressed-file writes, and broad HFS+ allocation growth beyond the bounded initial-extent slice remain uncertified
until operation-specific proof exists.
The supplemental destructive APFS raw-format proof lane is
`scripts/run_partition_manager_apfs_raw_format_validation.ps1`, with
`scripts/launch_partition_manager_apfs_raw_format_validation_local.ps1` for UAC
launch. It requires admin, `-Force`, APFS GPT type, non-boot/non-system target
checks, optional serial/friendly-name pinning, raw-target opt-in, and hardware
evidence flags before formatting an expendable APFS partition. The lane now
also requires the target to be 64-128 MiB so the generated layout stays inside
the certified one-spaceman-chunk envelope. The 2026-06-12 PDT / 2026-06-13 UTC
JMicron run (`run-20260612-192652`, 51,170,148,352-byte target) is retained as
Windows-side evidence only: the macOS recovery kernel later rejected that large
generated APFS target with spaceman checkpoint/container corruption. Current
Windows-side small-target proof passed on JMicron serial `DD56419883A5B`, Disk 2
Partition 2, 134,217,728 bytes, with report
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`.
Apple-native validation of that small target remains required before claiming
Apple-native APFS destructive raw certification again.
The APFS writer CLI self-test now also covers generated-image volume-label
change, old/new-label JSON, APFS browser read-back, and raw volume-label refusal
on normal file paths. Latest local proof:
`artifacts\partition-manager-certification\vm-lab\apfs-cli-self-test\run-d609fd4714b244a38c21c8e9c79b78aa`.
Physical raw APFS volume-label Apply proof is recorded in
`artifacts\file-management-live-certification\disk3-apfs-raw-format\report.json`
from guarded run `run-20260612-192652`; that large-target proof is
Windows-side-only. Current 128 MiB Windows-side raw-format proof is recorded at
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`;
Apple-native validation of that small generated target remains pending.
The read-only APFS browser uses the same Fletcher-64 object-checksum verifier
for APFS metadata object blocks and fails closed on corrupt metadata checksums;
file extent payload blocks are read as data, not as APFS object metadata.

File Management live cross-filesystem proof is destructive and separate from
the Partition Manager matrix. Current Windows-side live evidence passed through
`scripts\run_file_management_live_filesystem_certification.ps1` on pinned
expendable raw media. APFS passed on JMicron serial `DD56419883A5B`, Disk 2
Partition 2, 134,217,728 bytes, with File Explorer create/write/read,
duplicate-finder, advanced-search, delete-file, negative-read, and
delete-directory proof at
`artifacts\file-management-live-certification\disk2-apfs-script-after-fix\file-management-live-certification.json`.
HFS+ passed on Best Buy serial `DD564198838A8`, Disk 3 Partition 3, with File
Explorer create/write/read, duplicate-finder, advanced-search, rename,
delete-file, negative-read, and delete-directory proof at
`artifacts\file-management-live-certification\disk3-hfs-script-after-fix\file-management-live-certification.json`.
APFS still needs macOS Recovery validation of the small generated slice before
it can be called Apple-native certified.

Linux compatibility validation for the ext bundle passed on 2026-06-07 UTC
through Arch WSL for ext2, ext3, and ext4. The harness created each image with
the bundled Windows `mke2fs`, grew and shrank it with bundled Windows
`resize2fs`, verified clean Linux `e2fsck`/`dumpe2fs` output, loop-mounted the
image read/write in Linux, wrote `sak-linux-proof.txt`, rechecked clean with
Linux `e2fsck`, and rechecked clean with bundled Windows `e2fsck` after the
Linux write. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext2-linux-validation\report.json`,
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext3-linux-validation\report.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext4-linux-validation\report.json`.
This is certification tooling only and does not add any WSL or Linux runtime
dependency to the portable Windows app.

Linux-created XFS/Btrfs metadata validation passed again on 2026-06-07 UTC through
Arch WSL. The harness created disposable XFS and Btrfs images with `mkfs.xfs`
and `mkfs.btrfs`, then ran `partition_filesystem_probe_certifier.exe` from
Windows against each image with expected file-system and sanity requirements.
S.A.K. detected XFS and Btrfs, captured label/UUID/geometry/counter metadata,
reported sane metadata, and deleted the disposable images. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-metadata-validation\report.json`.
This validates read-only detector metadata used by the in-app original metadata
consistency check. Deep XFS/Btrfs tool checks, format, repair, and write support
remain blocked pending approved Windows-portable tools and operation-specific
proof.
Full arbitrary APFS/HFS+/XFS/Btrfs write support is not certified. Any such
claim requires a separate driver/tooling milestone that covers native
allocation, logs/journals or checkpoints, metadata checksums, snapshots/clones,
compression/encryption where applicable, multi-device or multi-volume state,
crash replay, rollback, VM proof, physical proof, and UI/apply safety gates.

Linux swap format compatibility validation passed on 2026-06-05 PDT through
Arch WSL. The harness created a disposable image, wrote an original SWAPSPACE2
v1 header using the same field layout as the queued app formatter, then verified
the image with S.A.K. `partition_filesystem_probe_certifier.exe`, Linux `blkid`,
and Linux `swaplabel`. All probes reported swap type, label, UUID, version, and
page-size metadata consistently. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-validation\report.json`.
This validates the app's generated Linux swap metadata format without adding a
runtime WSL dependency.

Linux swap raw-partition VM proof passed on 2026-06-05 PDT in
`SAK-PM-Lab-Win11`. The host launcher
`scripts/launch_partition_manager_vm_gate_host.ps1` resolved the real guest
password file from VM unattended metadata, validated `VBoxManage guestcontrol`
as `saklab`, and recorded a redacted host launch report. The elevated runner
created a disposable GPT partition on VirtualBox disk 1, set the Linux swap GPT
type, wrote original SWAPSPACE2 v1 metadata to
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, reread and verified the raw
header, verified S.A.K. raw detection with
`partition_filesystem_probe_certifier.exe`, and cleared disk 1 back to RAW.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.
The host launch report is under
`artifacts\partition-manager-certification\vm-lab\host-launch\`. The original
proof used a keypress UAC fallback on the legacy lab VM; that fallback is now
opt-in only.

A direct-admin automation rerun passed on 2026-06-05 PDT / 2026-06-06 UTC in
`SAK-PM-Automation-Win11`, created by
`scripts/new_partition_manager_automation_vm.ps1`. The VM-creation report
`artifacts\partition-manager-certification\vm-lab\automation-vm\SAK-PM-Automation-Win11-20260605210356.json`
records `direct_admin_guestcontrol_verified=true`. The host launch report
`artifacts\partition-manager-certification\vm-lab\host-launch\linux-swap-20260605220305.json`
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`. The VM gate report remains
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`
with `status=Passed`, `detected_file_system=Linux swap`, raw target
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, exact 134217728-byte probe size,
and cleanup back to RAW. This
raw-media lane is supplemental and is not counted in the imported 18/18
external matrix unless the matrix is expanded with a Linux swap gate.

Filesystem probe helper offset proof was added on 2026-06-05 PDT. The helper
`partition_filesystem_probe_certifier.exe` accepts `--input-offset-bytes`, reads
regular files and Windows raw device paths from that offset, and reports the
offset in JSON. `scripts/test_partition_filesystem_probe_certifier.ps1` creates
a small padded ext2 fixture, validates the offset read, and verifies invalid
offset input fails; the script is wired into CTest and release readiness. A
lab-only Sonoma DMG HFS+ fixture also passed with `--input-offset-bytes
134250496` and sane HFS+ header metadata. The unpacked Tahoe installer contains
a compressed `SharedSupport.dmg` with an HFS+ payload, useful for future
container extraction tests but not a runtime dependency or release asset.

Physical Apple filesystem read-only proof passed on 2026-06-05 PDT /
2026-06-06 UTC against an expendable external USB NVMe GPT disk. The disk was
non-boot and non-system. Partition 2 used Apple APFS GUID
`{7c3457ef-0000-11aa-aa11-00306543ecac}` and passed APFS detection, root
listing, `/Fonts` listing, selected-file read proof, bounded export proof, and
space-manager free-byte reporting through
`\\?\GLOBALROOT\Device\Harddisk2\Partition2`; partition 3 used Apple HFS GUID
`{48465300-0000-11aa-aa11-00306543ecac}` and passed HFS+ detection, root
metadata, and free-byte reporting through
`\\?\GLOBALROOT\Device\Harddisk2\Partition3`. The APFS file proof read
`/Fonts/00TT.TTF` at 25056 bytes with SHA-256
`d075a134b3092fd36c6e45acc88d2efd163e60857cb2f0a2621569f446fa06d2`. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-filesystem-physical\report.json`.
APFS bounded recursive export proof also passed from `/Fonts` on the same
raw-partition alias with 63 files, 1 directory, 64 scanned entries, and
21161830 exported bytes. The latest APFS probe reported `total_bytes=127992487936`
and `free_bytes=127866097664`; the latest HFS+ probe reported
`total_bytes=127724052480` and `free_bytes=125197647872`, listed 9 root
entries, and read
`/polyhavenassets_blendermarket_v1.2.0  (Blender 4.5+).zip` at 185293 bytes
with SHA-256
`72222f9f83d177ea9a2970edb756ab9f3f9f6b00a12f47af63259f215d970709`, then
read inline HFS+ attribute `com.apple.decmpfs` for file ID 1894 at 16 bytes
with SHA-256
`e2aed0d76e90c81c39f9916d56a8f1631c440aec356877ea7dae197b069ea64f`. The
physical Apple report records `validation_requirements` showing HFS file,
HFS attribute, APFS file, and APFS export proofs were required for the latest
run.
The app inventory path now tries those `GLOBALROOT` partition aliases before
using elevated PhysicalDrive offset reads. This proof is read-only and does not
approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth, compressed-file writes,
arbitrary APFS
mutation, non-generated APFS file writes, or APFS resize behavior.

APFS multi-partition physical probe coverage was expanded on 2026-06-07 UTC
using a second expendable non-boot USB disk with five APFS partitions. The
read-only validation command
`scripts\run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber 3 -AllowMissingHfs -ProbeAllApfsPartitions -RequireAllApfsPartitions`
passed, and every APFS partition reported sane nonzero free-space counters
through `\\?\GLOBALROOT\Device\Harddisk3\PartitionN`. This specifically
regresses APFS containers whose space-manager checkpoint object is outside the
initial 2 MiB signature probe. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-apfs-multipart-physical\report.json`.

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
