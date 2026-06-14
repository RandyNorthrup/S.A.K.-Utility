# APFS + HFS+/HFSX Full Driver-Level Write Plan

**Goal**: Take S.A.K.'s APFS and HFS+/HFSX write support from the current
generated/minimal/bounded scope to **full driver-level read/write on arbitrary
Apple-written media**, with enterprise production-grade code, complete tests, and
external certification using Apple's own validators in the macOS VMs and physical
disposable USB media.

**Locked scope (2026-06-13)**:

- In scope: APFS full driver-level write. HFS+/HFSX full driver-level write.
- Future enhancement, not this program: XFS, Btrfs (read-only metadata stays the
  shipped scope until a separate plan).
- Out of scope (Files-clone exclusions, unchanged): cloud drives, FTP, Git
  integration, third-party integrations.
- Execution posture: build every capability, ship each one only after its own
  external-validator proof exists (Apple `fsck_apfs` / `fsck_hfs` + macOS kernel
  mount + physical USB destructive + crash-interruption + rollback). No capability
  flips to "Yes" in any user-facing matrix before its proof artifact lands.

This plan is the forward roadmap. The authoritative record of what is *already*
certified is [PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md](PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md);
the File Explorer surface that consumes these writers is
[FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md](FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md).
This document does not restate their history; it defines the gap to "full driver"
and the milestones to close it.

> **"Driver-level" means capability parity, not a kernel driver.** Everything in
> this plan is **userspace** raw-block read/write against a **dismounted** volume
> (`\\?\GLOBALROOT\Device\HarddiskN\PartitionM` + sparse staging + helper CLIs).
> S.A.K. installs **no** Windows kernel-mode filesystem driver, mounts nothing
> through the OS, and the base feature set must keep that property. See
> [§1A](#1a-runtime-footprint-and-optional-component-gating) for the rule covering
> any future capability that *would* need an installed component.

---

## 1A. Runtime Footprint And Optional-Component Gating

Hard architecture constraint (per 2026-06-13 user directive):

**The default install must not require any kernel driver or persistent system
component, and any capability that does require one must be optional, separately
installed, and feature-gated by runtime detection.**

Baseline footprint (no install shape change — this is the whole current design):

- Offline raw-block I/O on a dismounted partition. No `.sys` driver, no live mount.
- Elevation is **per-operation UAC** through the existing allowlisted helper tasks
  (`ReadPartitionProbe`, raw writer CLIs), not a persistent installed service.
- All §3–§6 milestones (in-place COW, multi-CIB spaceman, snapshots, encryption,
  resize, HFS B-tree/allocator work) are pure userspace block manipulation and
  **add nothing to the install footprint.**

Optional components that *would* change the shape — gate each one:

| Component | Only needed for | Default | Gating |
|---|---|---|---|
| WinFsp / Dokany (userspace mount FS) | Optional "browse APFS/HFS as a Windows drive letter" convenience | **Not installed** | Detect at runtime; if absent, hide/disable the drive-letter-mount command only; S.A.K.'s own raw explorer stays fully functional without it |
| Persistent elevated helper service | Faster repeated raw access without per-op UAC | **Not installed** | Per-op UAC remains the default path; service is an opt-in install that only removes prompts, never unlocks new capability |
| Any future kernel-mode component | (none currently planned) | **Forbidden in base** | Would require its own plan + signing + opt-in installer + this same gating |

Gating rules:

- **Detect, don't assume.** A capability that depends on an optional component
  must probe for it at startup and expose a single source-of-truth availability
  flag the command registry / safety validator read.
- **Degrade, don't break.** Missing component → dependent commands are *visible
  but disabled* with exact blocker text ("Requires optional WinFsp mount support —
  not installed"), exactly like the raw-FS write blockers. Core raw read/write
  must never depend on an optional component.
- **No silent install.** An optional component is installed only by explicit user
  action, never bundled-on or auto-installed by a feature click.
- **No capability behind a driver.** Driver-level write capability (the point of
  this plan) is delivered through userspace raw I/O and must remain reachable with
  zero optional components installed. Optional components may only add
  *convenience* (drive-letter mount, fewer UAC prompts), never *capability*.

This constraint is acceptance-level: a milestone fails its gate if it makes a core
read/write capability depend on an installed driver or service.

---

## 1. Definition Of "Full Driver-Level Write" (Acceptance)

A target counts as full driver-level write-certified only when **all** hold:

1. S.A.K. can read and mutate a volume **written by Apple's own driver**, not only
   a S.A.K.-generated layout, without rewriting the whole container.
2. Mutation is **in-place and crash-safe**: it follows the file system's native
   transaction model (HFS+ journal; APFS checkpoint/object-map copy-on-write),
   preserves existing metadata checksums, and a mid-operation power loss leaves
   the volume either at the old state or the new state, never corrupt.
3. After mutation, **Apple's own checker passes** (`fsck_hfs -fn` / `fsck_apfs`)
   and the **macOS kernel mounts the volume read-write** in the VM.
4. The native **allocation model** is preserved (real bitmap/space-manager
   accounting, real B-tree balancing, real extent allocation), with no size or
   shape cap beyond documented hard physical boundaries.
5. The capability is reachable through the production path (queue → safety
   validation → Apply review → exact-target identity → explicit confirmation →
   operation report) and is gated by certification evidence IDs.
6. There is a **rollback boundary**: the operation can be aborted and reverted
   from staged state, proven by test.

Hard physical boundaries that remain fail-closed by design (documented, not
defects):

- Encrypted/protected volumes require **user-supplied credentials**. Without the
  volume password / recovery key, the data is cryptographically unavailable; this
  is a property of FileVault/APFS encryption, not a missing feature. Encryption is
  in scope **only** as a credential-gated path (user provides the secret).
- Fusion / multi-device (Tier2) APFS containers are **out of scope** (decision
  2026-06-13): no Fusion certification rig exists, so "full driver" acceptance is
  single-device media only. Detect Fusion/Tier2 and fail closed with a clear
  "multi-device Fusion not supported" blocker. Revisit only if a Fusion test rig
  is sourced.

---

## 2. Current Certified Baseline (Compressed)

Source of truth: [PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md](PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md).
Summary of what already passes external validation:

**HFS+/HFSX** — strong, but bounded.
- Read: full (catalog, extents-overflow, attributes, decmpfs types 3/4/7/8/11/12,
  compressed transparent reads, journal parse). Apple `fsck_hfs` (hfs-650.0.2)
  passes S.A.K.-built volumes; macOS auto-mounts read-write.
- Write implemented + certified: data/resource fork overwrite, bounded
  allocation growth via extents-overflow insertion (one root split), catalog tree
  mutation depth 1–3 / ≤256 working leaves (create/delete/rename/move/split/
  collapse), inline + fork-backed attribute create/delete + bounded growth,
  bounded file create, allocated-file delete + overflow-record removal, bounded
  folder-tree delete + bitmap release + optional zeroing, alternate volume header
  sync, **little-endian journal replay validated against an authentic Apple crash
  journal**, decmpfs read+write all types (lzfse/lzvn via `third_party/lzfse`),
  format/repair via bundled hfsprogs.

**APFS** — bounded to generated/minimal + import-rewrite.
- Read: full for unencrypted/uncompressed (checkpoint/omap traversal, volume
  superblock, object checksums, space manager, fs-tree browse, extract, bounded
  export). Apple `fsck_apfs` passes S.A.K.-generated containers; kernel
  auto-synthesizes and mounts; zero fsck warnings.
- Write implemented + certified: generated/minimal single-volume layout in the
  **64–128 MiB one-spaceman-chunk envelope** (format, root + child file CRUD +
  byte-range patch, empty dir create/delete, volume-label change, checksum
  repair); `import-image` rewrite of arbitrary unencrypted snapshot-free Apple
  containers with added/overwritten files (read whole container, re-emit a new
  one), VM-certified.

Code anchors: `PartitionHfsFileSystemWriter` in
[partition_hfs_file_system_reader.cpp](src/core/partition_hfs_file_system_reader.cpp);
`PartitionApfsWriter` in [partition_apfs_writer.cpp](src/core/partition_apfs_writer.cpp)
and [partition_apfs_writer.h](include/sak/partition_apfs_writer.h); helper CLIs
`sak_hfs_writer_cli.exe`, `sak_apfs_writer_cli.exe`; certifier
`partition_filesystem_probe_certifier.exe`.

---

## 3. Gap Analysis

Each row is a capability that must move from its current state to "full driver".
"Validator" is the external proof that gates the flip to Yes.

### 3.1 HFS+/HFSX gaps

| # | Capability | Current | Target | Primary method | Validator |
|---|---|---|---|---|---|
| H-a | Special-file node-pool growth (catalog/attributes/extents-overflow B-tree files) | Hard cap: "does not have enough free nodes" blocker | Grow the special-file fork via the allocation bitmap, extend the node map, add nodes | Allocate blocks for the B-tree file fork, rewrite header-node map + free-node count | `fsck_hfs -fn`, kernel RW mount |
| H-b | Catalog/attributes B-tree depth > 3 levels and width > 256 working leaves | Bounded engine loads ≤256 leaves, depth ≤3 | Streaming node engine, arbitrary depth/width | Replace whole-tree load with on-demand node paging + path-walk mutation | `fsck_hfs -fn` on large synthetic + Apple-written trees |
| H-c | B-tree underflow handling (merge/rebalance on delete) | Collapse-only, no merge/redistribute | Sibling merge + key redistribution on underflow | Standard B-tree delete-rebalance against `hfs_format.h` node rules | `fsck_hfs` "Invalid sibling link" clean after heavy delete |
| H-d | General volume allocator + multi-leaf extents trees + recursive extents-overflow-file overflow | Bounded initial-extent slice, one extents-overflow root split | Full bitmap allocator (fragmenting), multi-leaf extents, overflow of the extents file's own fork | Real allocation-bitmap allocator; extents engine recursion guard | `fsck_hfs` extent checks on fragmented volume |
| H-e | Hardlinks, private metadata dir, complex file delete, symlink create, startup/bad-block files | Symlinks export-only; complex/hardlinked delete blocked | Create/delete hardlinks via `␄␄␄␄HFS+ Private Data`, iNode/iNodeFile linkage, symlink create | hfs-650 link semantics; indirect-node refcount maintenance | `fsck_hfs` hardlink + link-count checks, kernel mount |
| H-f | Inline/broad attribute growth + attribute-tree overflow records | Bounded inline + fork-backed; overflow records blocked | Arbitrary attribute size + attribute B-tree overflow records | Extend attribute engine to overflow records, reuse H-d allocator | `fsck_hfs` attribute-count buckets clean |
| H-g | Big-endian (PowerPC-era) journal replay | Fail-closed by explicit check | Replay BE journals for completeness | Mirror the LE journal engine with endian-swap | Byte-equality vs direct-write path (no Apple BE rig; unit-certified) |
| H-h | HFS wrapper / embedded HFS-in-HFS+ legacy edge | Direct + wrapper header read | Confirm write path under wrapper offset | Reuse logical-span staging with wrapper offset | `fsck_hfs` on wrapped volume |

### 3.2 APFS gaps

| # | Capability | Current | Target | Primary method | Validator |
|---|---|---|---|---|---|
| A-a | Multi-CIB / multi-chunk space manager (size > 128 MiB, real geometry) | **✅ CERTIFIED 2026-06-14** — single-CIB multi-chunk shipped; 64 MiB / 256 MiB / 1 GiB pass Apple `fsck_apfs` + kernel rw mount + crash/rollback + physical-USB. Multi-CIB/CAB tier (>~16 GiB / >126 chunks) still gated | Arbitrary-size space manager: multiple chunk-info blocks, internal pool, CIB addressing, free-queue | Build spaceman to mirror same-geometry `newfs_apfs`; CIB array + bitmap blocks | `fsck_apfs`, kernel mount on multi-GB target — **DONE** (`artifacts/partition-manager-certification/vm-lab/apple-tool-evidence/`) |
| A-b | True in-place copy-on-write checkpoint mutation of arbitrary Apple containers | **🟡 CORE CERTIFIED 2026-06-14** — increment 1 (re-checkpoint engine) **+** increment 2 (real in-place COW **file insert**) both Apple-certified: `commitImageOnlyFileInsert` COWs the omap chain (container omap→volume sb→volume omap→fstree) + swaps the allocation bitmap + advances the checkpoint; Apple kernel mounts it RW and **reads the inserted file**, apfs.kext continues the S.A.K. ring (xid 3→5), and `fsck_apfs` reports container + volume **OK**. Increment 2b (**non-empty files / data extents**) also certified: a 62-byte file inserted in place, Apple kernel `cat` reads the exact content from the data extent, and `fsck_apfs` is clean (extent-ref tree + alloc_count + allocated space all consistent). Multi-block + chained multi-file inserts + **in-place file delete** certified: insert (empty/non-empty/multi-block/chained, preserving existing files+data) and delete (removes a file, preserves the rest) both pass Apple `cat`/`ls` + `fsck_apfs` clean. Remaining: free-queue crash-safety, arbitrary Apple containers, multi-leaf fstree (>~250 files), fragmented free space | Mutate existing container in place: checkpoint ring, ephemeral commit, omap COW, xid advance, reaper, free-queue | COW checkpoint engine over the existing NXSB/checkpoint descriptor area | `fsck_apfs` + kernel RW mount after each commit; crash proof — **insert + delete (all variants) DONE** (`artifacts/…/apfs-a2-increment1-checkpoint-commit.report.json`, `apfs-a2-increment2-file-insert.report.json`, `apfs-a2-increment2b-data-extent.report.json`) |
| A-c | Snapshots (create/delete/revert) | Read-only; snapshotted containers blocked | Create/delete snapshots, manage `snap_meta_tree`, `extentref_tree`, omap snapshot tree | APFS snapshot metadata + extent-reference accounting | `fsck_apfs` snapshot checks, kernel `tmutil`/mount |
| A-d | Multi-volume containers | Single-volume only | Add/remove/mutate volumes sharing one space manager | Volume superblock array + shared spaceman allocation | `fsck_apfs` multi-volume, kernel mount of each volume |
| A-e | APFS compression read+write | decmpfs read via reader; APFS-side write blocked | Read+write `com.apple.decmpfs` inline + resource-fork compressed in APFS | Reuse `third_party/lzfse` codecs; APFS xattr + dstream wiring | Byte-match Apple decode; kernel reads written file |
| A-f | Encryption (FileVault / APFS encrypted), **credential-gated** | Blocked on any encryption flag | Read+write decrypted content when user supplies **volume password OR personal recovery key** (both accepted, 2026-06-13) | Keybag parse, KEK/VEK unwrap from password and recovery-key paths, AES-XTS; explicit credential gate | Apple unlocks/mounts the S.A.K.-written encrypted volume in VM |
| A-g | Resize (container + volume grow/shrink) | Blocked | Grow/shrink container and volume with spaceman reflow | Spaceman + omap + checkpoint resize, reuse A-a | `fsck_apfs` + kernel mount after resize; `diskutil` cross-check |
| A-h | Clones, sparse files, dir hardlinks, full xattr/ACL, sealed-volume policy | Partial | File clones (shared extents), sparse extents, directory hardlinks, ACL/xattr; sealed system volume writable **only behind a typed seal-invalidation confirmation** (2026-06-13) | APFS inode/extent sharing + `j_*` record set; detect `apfs_seal`/integrity metadata | `fsck_apfs`, kernel clone/sparse behavior; sealed-write path proven to be gated + warned |
| A-i | Fusion / multi-device (Tier2) | Blocked | **Out of scope** — detect + fail closed | Detect Tier2 container, refuse with clear blocker | N/A (no rig); revisit only if a Fusion rig is sourced |

---

## 4. Proven Methods And Research References

Grounding sources (read-only research; no code import of GPL drivers):

- **Apple File System Reference** (Apple Developer PDF) — authoritative on-disk
  format for APFS objects, omap, spaceman, checkpoints, snapshots, encryption,
  fs-tree `j_*` records. The single most important reference for the APFS track.
- **`hfs_format.h`** already vendored at
  [tools/filesystem/_build/.../include/hfs/hfs_format.h](tools/filesystem/_build/hfsprogs-build/diskdev_cmds-540.1.linux3/include/hfs/hfs_format.h)
  and the bundled **`fsck_hfs` / `newfs_hfs` (hfs-650 / diskdev_cmds-540.1)**
  source — authoritative HFS+ node, catalog, extents, attribute, journal layout
  and the exact checks S.A.K. output must satisfy. Use fsck source as the spec for
  what "valid" means.
- **`libfsapfs` (Joachim Metz)** and **`apfs` format notes (Jonas Plum / forensics
  community)** — independent cross-checks for APFS object/record layout. Use for
  read cross-validation only; do not vendor.
- **`third_party/lzfse`** (already in tree, BSD-3, in THIRD_PARTY_LICENSES.md) —
  reuse for both HFS+ decmpfs (done) and APFS compression (A-e).
- Existing internal ground-truth method that already worked and must be reused:
  **let Apple's driver write the structure in the VM, harvest the bytes, mirror
  them.** The generated-APFS fs-tree record engine (CRC-32C-over-UTF-32LE dirent
  hashing, `xf_used_data` semantics, inode flag 0x8000) was built this way and
  passed `fsck_apfs`. Every new APFS structure (snapshot, multi-volume, encrypted)
  follows the same loop: Apple writes it → harvest → mirror → `fsck_apfs` →
  kernel mount.

Engineering method per capability (the loop the codebase already uses):

1. Spec the on-disk structure from the reference + fsck source.
2. Implement the writer behind an explicit fail-closed enable flag.
3. Unit-prove byte-equality against an Apple-written sample where one exists.
4. CLI self-test on a real `newfs_*` volume with `fsck_*` after each phase.
5. VM external validation: Apple `fsck` + kernel mount.
6. Physical USB destructive proof + crash-interruption + rollback.
7. Only then wire to the production Apply path and flip the capability matrix.

---

## 5. Certification Infrastructure (Reused)

All gates reuse existing assets — no new rigs required for the non-Fusion scope:

- **macOS VMs**: Sonoma 14 + Tahoe recovery images in VirtualBox (see
  [memory/macos-vm-validation-resources.md](../memory/macos-vm-validation-resources.md))
  run Apple `fsck_apfs`, `fsck_hfs`, `diskutil`, and kernel mount against S.A.K.
  output. This is the external validator that retired the "no validator on
  Windows" excuse.
- **Disposable USB media**: pinned expendable JMicron + USB-NVMe disks for
  physical raw destructive proof through `\\?\GLOBALROOT\Device\HarddiskN\PartitionM`.
- **Linux/WSL**: ext + cross-check tooling (not needed for APFS/HFS but available).
- **Crash-interruption proof**: hard-reset / mid-transaction capture before
  remount, then Apple forced `fsck` + kernel mount — already demonstrated for HFS+
  journal and APFS import; extend to every new in-place mutation.
- **Rollback proof**: staged-image / checkpoint-abort path must restore the prior
  state, proven by hash before/after.

Existing destructive lane scripts to extend rather than replace:
`scripts/run_partition_manager_apfs_raw_format_validation.ps1`,
`scripts/run_partition_manager_ext_filesystem_vm_gate.ps1` (pattern),
`scripts/run_partition_manager_physical_apple_probe_validation.ps1`,
`partition_filesystem_probe_certifier.exe`.

---

## 6. Milestone Roadmap

Two parallel tracks (HFS track **H**, APFS track **A**) plus integration **I**.
Within each track, milestones are ordered by dependency and unlock value. Each
milestone's exit gate is its external validator from §3. **No matrix flip without
the artifact.**

### HFS+ track

| MS | Name | Unlocks | Exit gate |
|---|---|---|---|
| H1 ✅ | Writer split + special-file node-pool growth (H-a) | Extract `PartitionHfsFileSystemWriter` + shared B-tree/allocator module into own TUs; unbounded create — catalog/attr/extents files grow instead of hitting the node cap | No behavior change from the split (existing HFS tests green); `fsck_hfs -fn` clean after exhausting + growing a `newfs_hfs -s` small catalog; kernel RW mount — **DONE 2026-06-14 (Windows-side)**: split into `partition_hfs_core.h` (primitives) + `partition_hfs_internal.h` (HfsReader engine), reader/writer in own .cpp TUs, behavior-preserving (113/113 core subtests). Generic node-pool growth engine for all 3 special-file B-trees (catalog/extents/attributes); catalog + attributes growth unit-tested; bundled `fsck_hfs` clean after growing a small catalog 16→126 blocks over 800 files. **macOS VM exit gate PASSED 2026-06-14**: the grown `H1GROW` volume kernel RW auto-mounted (`touch` succeeded) and Apple `fsck_hfs` (hfs-583.100.9) reported "appears to be OK" (`hfs.h1-catalog-growth.vm-fsck_hfs-kernel-mount-PASS.png`). **H1 exit gate fully met.** Bounds still fail-closed (H-b/H-d): multi-map-node growth, special-file-fork extents-overflow; broader HFS full-driver flip (H8) still needs USB-destructive + crash + rollback |
| H2 | Streaming B-tree engine: arbitrary depth/width (H-b) | Real-world large directories | `fsck_hfs` clean on >256-leaf + depth-4 synthetic and on an Apple-written large catalog |
| H3 | B-tree underflow merge/rebalance (H-c) | Heavy-delete stability | `fsck_hfs` "Invalid sibling link"-clean after randomized create/delete churn |
| H4 | General allocator + multi-leaf extents + recursive overflow (H-d) | Arbitrary file growth/fragmentation | `fsck_hfs` extent checks on a deliberately fragmented volume; kernel mount |
| H5 | Hardlinks + private dir + complex/hardlinked delete + symlink create (H-e) | Full namespace ops | `fsck_hfs` link-count + hardlink checks; kernel mount; round-trip a hardlinked tree |
| H6 | Attribute overflow records + broad attribute growth (H-f) | Large xattrs / ACLs | `fsck_hfs` attribute buckets clean |
| H7 | Big-endian journal replay + HFS wrapper write edge (H-g, H-h) | Completeness | Unit byte-equality (BE); `fsck_hfs` on wrapped volume |
| H8 | **HFS+ full-driver certification gate** | Flip HFS matrix to Yes | Physical USB destructive + crash + rollback on an Apple-written HFS+ volume; Apple `fsck_hfs` + kernel RW mount |

### APFS track

| MS | Name | Unlocks | Exit gate |
|---|---|---|---|
| A1 ✅ | Multi-CIB / multi-chunk space manager (A-a) | Arbitrary size > 128 MiB | `fsck_apfs` + kernel mount on a multi-GB generated container (closes the 51 GB `spaceman_sanity_check` rejection) — **DONE 2026-06-14** (64 MiB/256 MiB/1 GiB Apple `fsck_apfs` + kernel rw mount + write round-trip + crash/rollback + physical-USB; also flips the matrix's general crash/rollback/physical-USB lanes to proven). Writer UUIDs are now random v4 with read-back; HFS catalog metadata stamped |
| A2 | In-place COW checkpoint mutation of arbitrary Apple containers (A-b) | Mutate real Apple media without full rewrite | `fsck_apfs` + kernel RW mount after each in-place commit; crash-interruption proof |
| A3 | Snapshots create/delete/revert (A-c) | Snapshotted containers writable | `fsck_apfs` snapshot checks; kernel mount; `tmutil`/`diskutil` cross-check |
| A4 | Multi-volume containers (A-d) | Multi-volume Apple media | `fsck_apfs` multi-volume; kernel mount of each volume |
| A5 | APFS compression read+write (A-e) | Compressed files | Apple kernel reads S.A.K.-written compressed file; byte-match decode |
| A6 | Encryption, credential-gated (A-f) | FileVault/encrypted APFS with user secret | Apple unlocks + mounts the S.A.K.-written encrypted volume in VM |
| A7 | Resize + clones + sparse + dir hardlinks + xattr/ACL + sealed-volume policy (A-g, A-h) | Remaining native ops | `fsck_apfs` + kernel mount per op; `diskutil` resize cross-check |
| A8 | **APFS full-driver certification gate** | Flip APFS matrix to Yes | Physical USB destructive + crash + rollback on arbitrary-size, Apple-written, multi-volume (+ snapshot) container; `fsck_apfs` + kernel RW mount |
| ~~A9~~ | ~~Fusion / Tier2~~ | **Out of scope** (2026-06-13) — detect + fail closed, no cert rig | N/A |

### Integration track

| MS | Name | Work | Exit gate |
|---|---|---|---|
| I1 | Capability-matrix promotion | As each H/A gate passes, wire the new writer capability through `FileManagementFileSystemBridge`, `PartitionSafetyValidator`, `PartitionScriptBuilder`, the `HFS File`/`APFS File` Apply actions, and `FileExplorerCommandRegistry`; flip the matrix cell to Yes; remove the stale blocker text | Per-capability: production Apply route exercised end to end + identity guard + confirmation |
| I2 | Docs + release gate | Update §3 matrices in this doc, cross-fs plan, explorer plan capability matrix, README, CHANGELOG, THIRD_PARTY_LICENSES, certification doc, release readiness | Full local CTest; release readiness; no claim exceeds evidence |

Suggested global order (interleave so external-validation VM time is shared):
**A1 → H1 → A2 → H2 → H3 → A3 → H4 → A4 → H5 → A5 → H6/H7 → A6 → A7 → H8/A8 → I2.**
A1 and A2 are the highest-value APFS unlocks and gate everything else on that
track; H1–H4 are the HFS-driver core.

---

## 7. Per-Milestone Checklist Template

Every milestone uses the same enterprise loop (matches §4 method and the existing
cross-fs build order). Copy this block per milestone; do not check a box without
the named artifact.

- [ ] **Spec**: on-disk structure documented from Apple reference + fsck source,
      with field offsets and invariants the fsck check enforces.
- [ ] **Ground truth**: Apple-written sample harvested in VM where the structure
      exists (snapshot, multi-volume, encrypted, compressed, large spaceman).
- [ ] **Writer**: implemented behind explicit fail-closed enable flag + evidence
      ID; no default-on broadening.
- [ ] **Unit**: byte-equality vs Apple sample and/or vs already-certified direct
      path; fail-closed tests for every precondition.
- [ ] **CLI self-test**: real `newfs_*` volume, `fsck_*` after each phase, wired
      into CTest.
- [ ] **VM gate**: Apple `fsck_apfs`/`fsck_hfs` clean + kernel mounts read-write;
      evidence JSON under `artifacts/partition-manager-certification/vm-lab/`.
- [ ] **Physical USB**: destructive proof on pinned disposable media through
      `GLOBALROOT` alias with raw identity guard + read-back hashes.
- [ ] **Crash-interruption**: mid-transaction reset, then Apple forced `fsck` +
      kernel mount = clean (old-or-new, never corrupt).
- [ ] **Rollback**: staged-abort restores prior state, hash-proven.
- [ ] **Production wiring (I1)**: queue → safety validation → Apply review →
      exact-target identity → confirmation → operation report.
- [ ] **Matrix flip + docs**: capability cell → Yes only now; blocker text removed
      everywhere it appeared; cross-fs + explorer + README/CHANGELOG synced.

---

## 8. Cross-Cutting Engineering Rules

Inherits the cross-fs plan's **Promotion rule** verbatim: any capability needs
source implementation, UI gating, safety validation, queue/apply scripts,
manifest/license gates for any bundled tool, destructive VM proof, physical media
proof, crash-interruption proof, rollback proof, and release-readiness docs before
it is enabled. Until then, fail closed.

Additional rules for this program:

- **In-place over rewrite**: A2 onward, do not regress to whole-container rewrite
  on real Apple media; `import-image` stays only as a fallback/compat path.
- **Checksum integrity is non-negotiable**: every APFS object write restamps
  Fletcher-64; every HFS+ B-tree/header write keeps node + volume accounting
  exact. A single bad checksum = fail the operation.
- **Transaction model is mandatory**: APFS mutations commit through a checkpoint
  with xid advance + omap COW; HFS+ mutations that the journal covers go through
  journal semantics. No "torn" multi-block writes without transaction wrapping.
- **Identity guard before every raw write**: re-validate the exact selected
  `GLOBALROOT` partition + disk serial/size immediately before mutation (existing
  rule, keep enforcing through the new larger/arbitrary targets).
- **Encryption is credential-in, never credential-stored**: A6 takes the secret at
  operation time, holds it only in memory, never persists keys or unlocked state.
- **No size/shape cap left as a silent blocker**: once H-a/A-a land, remove the
  "exceeds … cap" blocker strings (see grep hits in
  [partition_apfs_writer.cpp](src/core/partition_apfs_writer.cpp)) and replace with
  real allocation, not a higher hardcoded number.
- **Zero base-install footprint** (per [§1A](#1a-runtime-footprint-and-optional-component-gating)):
  no milestone may make a core read/write capability depend on an installed kernel
  driver or persistent service. Optional components are detect-gated, opt-in, and
  add convenience only, never capability.

---

## 9. Risk Register

| Risk | Impact | Mitigation |
|---|---|---|
| APFS in-place COW (A2) corrupts a real container mid-checkpoint | Data loss on test media only (disposable) | Disposable USB only; crash + rollback gates before any production wiring; never run on non-test media |
| No Apple BE-journal rig (H-g) | Can't externally certify BE replay | Unit byte-equality vs direct-write path; document as unit-certified, BE volumes are PowerPC-era and rare |
| Encryption crypto (A6) subtle/incorrect | Silent data corruption or weakened security | Certify by Apple kernel unlock+mount of S.A.K.-written volume; refuse on any keybag field S.A.K. can't fully account for |
| Sealed/signed system volume (A-h) | Mutating breaks the seal; volume may stop booting | Detect `apfs_seal`/integrity metadata; write only behind a **typed seal-invalidation confirmation** stating the macOS install may not boot. Never mutate a sealed volume silently or by default |
| Matrix flipped before proof (doc drift) | Release claim exceeds evidence | I1 rule: cell flips only in the same change that lands the evidence artifact; release wording gate in I2 |
| Two plan docs drift (cross-fs vs explorer vs this) | Inconsistent capability claims | This doc owns the driver matrix; explorer/cross-fs reference it; I2 syncs all three |
| A capability quietly grows the install footprint (driver/service dependency) | Breaks the "no kernel driver" base guarantee | [§1A](#1a-runtime-footprint-and-optional-component-gating) acceptance rule: core capability fails its gate if it needs an installed component; optional components are detect-gated and opt-in only |

---

## 10. Plan/Code Flaws Found During This Pass

Surfacing issues in the existing plan + code the user flagged as possibly flawed:

1. **Two capability matrices can drift.** The explorer plan
   ([FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md](FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md#L322))
   hardcodes "Certified slices" / "No" for HFS/APFS, while the cross-fs plan holds
   the real boundary. As driver milestones land, both must update. **Fix**: I1/I2
   make this doc the single matrix owner; the other two link here.
2. **"Cap exceeds" blockers are placeholders, not allocation.** Code paths like
   `"APFS … exceeds … cap"` and the 64–128 MiB envelope are interim guards. They
   must be *replaced* by real spaceman/allocator logic (A-a, H-a), not bumped to a
   bigger constant — otherwise the same class of `spaceman_sanity_check` rejection
   recurs at the new size.
3. **HFS writer lives inside the 12k-line reader file.** `PartitionHfsFileSystemWriter`
   is in [partition_hfs_file_system_reader.cpp](src/core/partition_hfs_file_system_reader.cpp).
   Before H2/H3/H4 add a streaming B-tree engine and a general allocator, consider
   splitting the writer (and a shared B-tree/allocator module) into its own
   translation unit to keep the driver core reviewable and testable. Non-blocking
   but recommended as an H1 refactor.
4. **`import-image` is a correctness shortcut, not a driver.** It rewrites whole
   containers; it cannot preserve snapshots, multi-volume layout, or large
   geometry, and it is O(container). A2 must supersede it for real media.
5. **Explorer milestones M7–M12 are unrelated to this scope** and remain open in
   their own plan; this program does not advance them. Flagged so the open boxes
   there are not mistaken for driver-write gaps.

---

## 11. Documentation Obligations

Update in the same change that lands each capability's proof:

- This doc: flip the §3 matrix cell, add the evidence path.
- [PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md](PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md):
  narrow the "Enterprise Arbitrary-Write Scope" blocker list.
- [FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md](FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md):
  update the File-System Capability Matrix cell.
- `docs/PARTITION_MANAGER_CERTIFICATION.md`: live proof section.
- `docs/RELEASE_READINESS.md`: release gate.
- `README.md`: only completed capabilities; keep "Files-inspired" / accurate
  APFS-HFS wording until A8/H8 pass.
- `THIRD_PARTY_LICENSES.md`: any new bundled reference/codec.
- `tests/README.md`: new unit/CLI/VM/physical lanes.

---

## 12. Decisions And Open Questions

Resolved (2026-06-13):

- **Encryption (A6)**: accept **both** volume password and personal recovery key.
- **Fusion/Tier2 (A9)**: **out of scope** — detect + fail closed, no cert rig.
- **Drive-letter mount (§1A)**: **decide later** — raw explorer only for now;
  WinFsp/Dokany convenience stays parked. Install footprint stays zero meanwhile.
- **Sealed system volume (A-h)**: **typed-confirmation override** — write allowed
  only behind a typed confirmation that it invalidates the seal and may stop macOS
  booting. Default is read-only/blocked until that confirmation.

- **Snapshots (A3)**: **CLI/helper-first.** Prove the snapshot engine through
  `sak_apfs_writer_cli.exe` + `fsck_apfs` + kernel mount before any UI; expose as a
  Partition Manager panel action only in a later dedicated UI milestone (mirrors
  how HFS+ compressed-file writes ship CLI-only today).
- **HFS-writer refactor (flaw #3)**: **split at H1.** Extract
  `PartitionHfsFileSystemWriter` and a shared B-tree + allocation-bitmap module out
  of [partition_hfs_file_system_reader.cpp](src/core/partition_hfs_file_system_reader.cpp)
  into their own translation units before H2–H4 add the streaming B-tree engine and
  general allocator. Keeps the driver core reviewable and unit-testable.

No open questions remain; all decisions are locked.
