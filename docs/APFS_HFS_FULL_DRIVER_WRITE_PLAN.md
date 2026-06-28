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
| H-f | Inline/broad attribute growth + attribute-tree overflow records | **✅ CERTIFIED 2026-06-27** — streaming multi-leaf attributes B-tree (depth-general) + `kHFSPlusAttrExtents` overflow records for >8-fragment fork attributes (cumulative-startBlock accounting); host `fsck_hfs` clean + macOS kernel mount + kernel xattr read-back + Apple native `fsck_hfs` hfs-683.100.9 clean on a 160-attr depth-2 tree AND a fragmented overflow fork attribute (`external.hfs-attribute-overflow-multileaf`) | Arbitrary attribute size + attribute B-tree overflow records | Extend attribute engine to overflow records, reuse H-d allocator | `fsck_hfs` attribute-count buckets clean — **DONE** |
| H-g | Big-endian (PowerPC-era) journal replay | **✅ CERTIFIED 2026-06-27** — the replay engine decodes a journal whose endian marker selects big-endian, reading every journal-header/block-list/block-info field in the journal's own byte order (ju16/ju32/ju64 + writeJu32/writeJu64 dispatch on a per-state `big_endian` flag; `writeBe64` added); payloads copied verbatim; clean-journal write-back stamps the native-endian start+checksum. Unit `hfsFileSystemWriter_replaysBigEndianJournal` replays the SAME transaction as the certified LE test with every field big-endian and asserts the replayed region is **byte-identical** to the certified direct-write path (the gate), + clean-header / no-op re-replay / tampered-checksum fail-closed | Replay BE journals for completeness | Mirror the LE journal engine with endian-swap | Byte-equality vs direct-write path (no Apple BE rig; unit-certified) — **DONE** |
| H-h | HFS wrapper / embedded HFS-in-HFS+ legacy edge | **✅ CERTIFIED 2026-06-27** — confirmed + certified the write path under the embedded-volume offset (`loadWrappedVolumeHeader` sets `m_volume.volume_offset` to the embedded HFS+ start; every read/write/alternate-header-sync adds it; fail-closed behind `allow_wrapped_volume`). Apple diskdev_cmds 540.1 `newfs_hfs -w` made a real HFS wrapper (MDB 'BD' @1024, embedded H+ @0x7000); S.A.K. created a file in the embedded volume (`--allow-wrapped-volume`, name landed at byte 3092640 inside the embedded volume); Apple `fsck_hfs -d -n` reports "The volume SAKH7W appears to be OK" **both before and after** the S.A.K. write. Unit `hfsFileSystemWriter_writesIntoWrappedVolume` round-trips an embedded write + asserts fail-closed without the override + MDB untouched + nothing at offset 0. Evidence `external.hfs-bigendian-journal-wrapper` | Confirm write path under wrapper offset | Reuse logical-span staging with wrapper offset | `fsck_hfs` on wrapped volume — **DONE** |

### 3.2 APFS gaps

| # | Capability | Current | Target | Primary method | Validator |
|---|---|---|---|---|---|
| A-a | Multi-CIB / multi-chunk space manager (size > 128 MiB, real geometry) | **✅ CERTIFIED 2026-06-14** — single-CIB multi-chunk shipped; 64 MiB / 256 MiB / 1 GiB pass Apple `fsck_apfs` + kernel rw mount + crash/rollback + physical-USB. Multi-CIB/CAB tier (>~16 GiB / >126 chunks) still gated | Arbitrary-size space manager: multiple chunk-info blocks, internal pool, CIB addressing, free-queue | Build spaceman to mirror same-geometry `newfs_apfs`; CIB array + bitmap blocks | `fsck_apfs`, kernel mount on multi-GB target — **DONE** (`artifacts/partition-manager-certification/vm-lab/apple-tool-evidence/`) |
| A-b | True in-place copy-on-write checkpoint mutation of arbitrary Apple containers | **✅ CERTIFIED (latest 2026-06-20)** — the in-place COW checkpoint engine is complete and production-wired. `commitImageOnlyFileInsert`/Delete/Rename/Write COW the omap chain (container omap→volume sb→volume omap→fstree) + extent-ref tree, swap the allocation bitmap, and advance the checkpoint; Apple kernel mounts each result RW, **reads/`cat`s the content**, continues the S.A.K. ring, and `fsck_apfs` reports container + volume **OK**. Certified scope: insert (empty/non-empty/multi-block/multi-extent/chained preserving existing files+data), delete, rename, write (create-or-replace); **full-tree round-trip** (root files + directories + children) and **all 4 directory mutations** (create/delete/child-write/child-delete, fsroot-valence clean); **multi-leaf fs-tree**; **crash-interruption rollback** + main/IP **free-queues** (arbitrary file sizes); **multi-chunk → multi-CIB → metadata-overflow (>1.3 TiB) → CAB tier** FORMAT+COMMIT to a 32 TiB cap, incl. **repeated-overflow + CAB in-place commit** via unified-group rotation; full-Unicode filenames. Certified on physical 238 GB + 2 TiB-on-4 TB + 8 TB raw media. **Production-wired (A2-3):** the `FileManagementFileSystemBridge` APFS file AND directory routes run on this engine. **Deferred (non-gating):** the ~2.9–7.8 TiB metadata-overflow inline cib-array "dead zone" (needs a harvested Apple ~4 TiB reference); the benign DSTREAM-xfield apfsck note (documented apfsck-vs-Apple divergence, kernel/`fsck_apfs` accept). Arbitrary multi-volume + snapshot physical full-driver gate = A8 (out of scope). | Mutate existing container in place: checkpoint ring, ephemeral commit, omap COW, xid advance, reaper, free-queue | COW checkpoint engine over the existing NXSB/checkpoint descriptor area | `fsck_apfs` + kernel RW mount after each commit; crash proof — **DONE** (`artifacts/partition-manager-certification/vm-lab/external-evidence/external.apfs-cow-directory-mutations/`, `…/external.apfs-fulltree-cow-mutations/`, `…/external.apfs-production-raw-physical/`, `…/external.apfs-cab-tier-cloud/`) |
| A-c | Snapshots (create/delete/revert) | Read-only; snapshotted containers blocked | Create/delete snapshots, manage `snap_meta_tree`, `extentref_tree`, omap snapshot tree | APFS snapshot metadata + extent-reference accounting | `fsck_apfs` snapshot checks, kernel `tmutil`/mount |
| A-d | Multi-volume containers | Single-volume only | Add/remove/mutate volumes sharing one space manager | Volume superblock array + shared spaceman allocation | `fsck_apfs` multi-volume, kernel mount of each volume |
| A-e | APFS compression read+write | **✅ CERTIFIED 2026-06-27 (inline zlib)** — `--compress-zlib` writes an embedded `com.apple.decmpfs` (ZLIB_ATTR) + `UF_COMPRESSED` inode on the COW engine; reader decodes it byte-perfect; apfsck clean + macOS kernel md5 byte-match + Apple `fsck_apfs` OK (`external.apfs-compression`). Resource-fork (large) + lzvn/lzfse/lzbitmap read are documented follow-ons | Read+write `com.apple.decmpfs` inline + resource-fork compressed in APFS | Reuse `third_party/lzfse` codecs; APFS xattr + dstream wiring | Byte-match Apple decode; kernel reads written file — **DONE** (`artifacts/partition-manager-certification/vm-lab/external-evidence/external.apfs-compression/`) |
| A-f | Encryption (FileVault / APFS encrypted), **credential-gated** | **✅ CERTIFIED 2026-06-27** — `sak_apfs_writer_cli --volume-password` formats a software-encrypted (FileVault) volume; the macOS kernel (apfs_kext 2332.101.1) UNLOCKED + MOUNTED it with the password (`diskutil apfs unlockVolume` → "Unlocked and mounted APFS Volume", FileVault: Yes (Unlocked), /Volumes/SAKENC) and Apple `fsck_apfs -n -l` reports volume + container **OK** (encryption key structures + decrypted fsroot tree checked). Crypto (PBKDF2/RFC3394/AES-XTS) vector-certified; keybag DER blobs + outer HMAC (`HMAC(SHA256(magic‖outer_salt), keyblob)`) byte-match real macOS; default crypto-state record emitted. **Follow-ons COMPLETE 2026-06-27:** (1) **personal-recovery-key path** — `--recovery-key` adds a second volume-keybag unlock record (the shared KEK wrapped by the recovery key, keyed by Apple's PRK UUID) so the volume unlocks by either secret; (2) **S.A.K.-side decrypt reader** — the APFS reader walks the keybag chain from the credential to the VEK and AES-XTS-decrypts the fs-tree + ONEKEY data itself (no longer kernel-only), fail-closed without/with-wrong credential. Both unit-proven (reader unlocks via password AND recovery key). | Keybag parse, KEK/VEK unwrap from password and recovery-key paths, AES-XTS; explicit credential gate | Apple unlocks/mounts the S.A.K.-written encrypted volume in VM — **DONE** (`artifacts/partition-manager-certification/vm-lab/external-evidence/external.apfs-encryption/`) |
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
| H2 ✅ | Streaming B-tree engine: arbitrary depth/width (H-b) | Real-world large directories | **EXIT GATE MET (2026-06-17)** — `fsck_hfs` clean on a >256-leaf + depth-4 synthetic catalog **and on an Apple-written large catalog**, both Apple-certified. **SYNTHETIC GATE MET (2026-06-16)**: the catalog B-tree load (recursive by height) + bottom-up index rebuild were already depth-general; raised the depth/leaf caps (3->8 / 256->16384) and made withCatalogNodePoolGrowth retry with a doubling free-node target (the rebuild needs ~whole-index free nodes, not a single split path - was failing at 734 files). Bundled `fsck_hfs -n -f` "appears to be OK" on a `newfs_hfs`(4096-node) volume grown to tree_depth 5 / >256 leaf nodes via 1500 long-named inserts; 124/124 unit tests. **SYNTHETIC GATE Apple-CERTIFIED (2026-06-16).** Two macOS Sequoia VM passes: (1) depth-5 unjournaled SAKH2 - kernel auto-mounted + `ls` READ all 1500 files (f-0000..f-1499). (2) depth-4 JOURNALED SAKH2J (902 files, >256 leaves, `newfs_hfs -J` + S.A.K. inserts) - kernel RW-MOUNTED + `touch` WROTE a new file into the deep catalog, then offline Apple `fsck_hfs` (hfs-683.100.9) reported "The volume SAKH2J appears to be OK" (catalog + catalog-hierarchy + extents + attrs + bitmap all checked) AFTER both S.A.K. and kernel writes. Evidence hfs.h2-deep-catalog-{kernel-read-1500-files,kernel-rw-write,apple-fsck-PASS}.png. **APPLE-WRITTEN-CATALOG GATE Apple-CERTIFIED (2026-06-17).** A real macOS-written HFS+ volume (GPT partition, /usr/bin copy → 194 files, hard links → the reserved `\0\0\0\0HFS+ Private Data` metadata directory) was mutated by S.A.K. (`create-empty-file-image`). This exposed + fixed a real collation bug: `catalogName` rejected NUL/'/' (valid on-disk) and `compareCatalogNames` approximated the case-fold (folding NUL to 0 instead of 0xFFFF), so the private-data directory mis-sorted. Fix = Apple's exact `FastUnicodeCompare` + `gLowerCaseTable` (commit cf611b7, new `partition_hfs_case_folding.h`). Certified in a macOS Sequoia VM: kernel **mounted** disk0s1 APPLECAT, `ls` **read** S.A.K.'s catalog (incl. the inserted file), `mount -uw` + `touch` **RW-wrote** a new file into S.A.K.'s rebuilt catalog, and offline Apple `fsck_hfs` (hfs-683.100.9) reported "The volume APPLECAT appears to be OK" — **including the `multi-linked files` and `catalog hierarchy` checks** (the hard-link/private-dir paths). Evidence hfs.h2-apple-written-catalog-{kernel-mount-read-write,apple-fsck-PASS}.png. **REMAINING (beyond the exit gate):** map-node growth (>~32000 nodes) still fail-closed (far beyond the gate); streaming incremental-split for efficiency (rebuild-everything is O(tree)/mutation). See [[h2-hfs-btree-scope]] |
| H3 ✅ | B-tree underflow merge/rebalance (H-c) | Heavy-delete stability | `fsck_hfs` "Invalid sibling link"-clean after randomized create/delete churn — **EXIT GATE MET 2026-06-21 (Apple `fsck_hfs` clean + unit + adversarial review).** A macOS-kernel-RW-mount re-test is **redundant for H3**: the merge emits its leaf/index nodes + sibling links through the SAME `emitCatalogLeafWrites` / `emitCatalogIndexLevels` functions the macOS HFS kernel driver already RW-mounted + accepted in H1 (`H1GROW`) and H2 (`SAKH2J` depth-4, `APPLECAT` Apple-written hard-link catalog incl. multi-linked + catalog-hierarchy) — merge introduces no new on-disk structure, only fewer leaves and an optional collapse to a depth-1 leaf-root (the fixtures' starting state). The bundled `fsck_hfs` 540.1 IS Apple's diskdev_cmds checker (same Invalid-sibling-link / catalog-hierarchy code as macOS hfs-683), so the literal gate is met by Apple's own checker. Delete-side merge: `mergeUnderfullCatalogLeaves` coalesces adjacent leaves when one is underfull (used < node_size/2) and their records fit one node, freeing the absorbed node; the rebuild-everything engine already rewrites every leaf's fLink/bLink + rebuilds the index bottom-up, so the tree also collapses in depth (down to a valid depth-1 single-leaf root) as deletes drain it. 50%-merge vs 100%-split **hysteresis** prevents split↔merge thrash. **Adversarial review:** 5 lenses (sibling-chain, node-accounting, termination, root-collapse, record-order), 0 confirmed defects (root-collapse lens byte-verified the leaf used-bytes accounting against the real node serializer). **Unit:** `mergesUnderfullCatalogLeavesOnDelete` (partial delete with no leaf emptied → tree collapses to depth 1 + freeNodes==total−2, decisive merge proof) + `survivesRandomizedCreateDeleteChurn` (300 seeded create/delete ops; index-traversal count vs leaf-chain `scanCatalogRecords` walk agree at every checkpoint; peak depth ≥2; final drain to depth 1); 135/135 core tests, no regressions; clang-format+lizard+cppcheck clean. **Apple `fsck_hfs` (540.1 diskdev_cmds, bundled):** `newfs_hfs -J` 256 MB SAKH3 → 900 files (depth 4, 1806 leaf records, 577 used nodes) → deleted 810/900 → **depth 4→3, used nodes 577→47** (merge reclaimed 530 nodes for 92 survivors) → "The volume SAKH3 appears to be OK" (catalog + catalog-hierarchy + multi-linked + extents + bitmap, no Invalid sibling link). Evidence hfs-h3-underfull-leaf-merge.report.json. **macOS-kernel RW-mount CERTIFIED 2026-06-21:** the live macOS Sequoia HFS kernel RW-mounted the churned SAKH3 (merged/depth-collapsed) catalog, `touch`-wrote `h3-kernel-rw.txt` into it (listed by the kernel's own `ls`), and after S.A.K. replayed the kernel's journal (`replay-journal-image`: 4 transactions / 9 blocks) Apple `fsck_hfs` reported "The volume SAKH3 appears to be OK" (catalog + catalog-hierarchy + multi-linked + bitmap clean). **VM-control note:** the macOS-guest USB-HID flakiness is the known QEMU #2349 (USB doesn't re-attach after EFI; works in OpenCore, flaky in macOS) — NOT a qemu-11 break; reliable headless driving = sole VNC client + QMP stop/cont + persistent `vncdotool` (shift-modified keysyms). |
| H4 ✅ | General allocator + multi-leaf extents + recursive overflow (H-d) | Arbitrary file growth/fragmentation | `fsck_hfs` extent checks on a deliberately fragmented volume; kernel mount — **EXIT GATE MET + Apple-CERTIFIED 2026-06-25 (engine + Apple-`fsck_hfs`-clean + unit + macOS-kernel mount).** Replaced the bounded extents-overflow engine ("empty or single-leaf, one root-leaf split") with a **streaming multi-leaf engine** mirroring the certified catalog model: load the whole extents tree (depth-general, recursive by height) → apply per-leaf insert/remove edits → split overfull / merge underfull leaves (collapsing depth, unlike the catalog the extents tree drops its last empty leaf to depth 0) → rebuild index levels bottom-up → compose header. The first-fit **fragmenting** allocation-bitmap allocator now threads an arbitrary fragment count through the inline 8 extents + an unbounded set of overflow records across multiple leaves. The **extents-overflow file's own fork** is held to the 8 volume-header extents (TN1150: the extents file has no overflow extents — confirmed in the bundled `SVerify1.c:3835` / `SExtents.c:922`), failing closed with a clear message at a 9th (the "recursion guard"). **Apple `fsck_hfs` (540.1 diskdev_cmds — the source Apple ships):** `newfs_hfs -b 512 -n e=1024` 16 MiB volume → two files grown 1 block/round for 130 interleaved rounds (first-fit interleaves their blocks → ~131 single-block extents each; ~260 multi-leaf re-mutations) → **depth-2 extents-overflow B-tree, 32 leaf records / 3 leaves** → "Checking extents overflow file … The volume SAKH4 appears to be OK." Deleting one fragmented file removes its 16 records (32→16) and fsck stays clean (multi-leaf removal + merge on a real volume). **Unit:** `hfsFileSystemWriter_collapsesDepthTwoExtentsTreeOnDelete` + `hfsFileSystemWriter_splitsLeafBelowExistingIndexNode` (leaf split below an existing index → 3-leaf tree, root index has 3 children); 140/140 core; clang-format/lizard/cppcheck clean. Evidence `external.hfs-multi-leaf-extents/report.json`. **macOS-KERNEL MOUNT Apple-CERTIFIED 2026-06-25 (macOS Sequoia VM, qemu/KVM recovery):** the kernel MOUNTED the bare unjournaled HFS+ SAKH4, `ls -l /Volumes/SAKH4` showed both fragmented files at 67072 bytes, the kernel READ both back BYTE-PERFECT across the depth-2 multi-leaf extents tree (`INTEG a=0 b=0` = every byte is its fill char, so the kernel reassembled ~131 single-block fragments/file from inline 8 + multi-leaf overflow records), then Apple's NATIVE `fsck_hfs` (hfs-683.100.9) on `/dev/rdisk0` — "Checking extents overflow file … The volume SAKH4 appears to be OK". Evidence hfs.h4-kernel-mount-readback.png + hfs.h4-apple-fsck-PASS.png. **H4 EXIT GATE FULLY MET.** Commit 7f4ef50 (engine) + the Apple-cert commit. See [[h2-hfs-btree-scope]] |
| H5 ✅ | Hardlinks + private dir + complex/hardlinked delete + symlink create (H-e) | Full namespace ops | `fsck_hfs` link-count + hardlink checks; kernel mount; round-trip a hardlinked tree — **EXIT GATE MET + Apple-CERTIFIED 2026-06-27 (engine + Apple-`fsck_hfs`-clean + unit + macOS-kernel mount + shared-data round-trip).** Byte layout harvested from a real macOS Sequoia volume (`ln`/`ln -s`) and mirrored exactly. **Hard-link CREATE:** the first link to a regular file keeps its CNID but moves it into the on-demand `\0\0\0\0HFS+ Private Data` metadata directory as `iNode<CNID>` (data blocks stay put), turns the original name into a `hlnk`/`hfs+` alias, and adds the new alias; further links prepend to the doubly-linked chain and bump link count. Inode `reserved1@4 = hl_firstLinkID`, `BSDInfo.special@44 = linkCount`; alias `ownerID@32 = hl_prevLinkID`, `groupID@36 = hl_nextLinkID`, `special@44 = iNodeNum`, mode = inode mode & ~0222, flags `0x0022`. **SYMLINK CREATE:** file mode `S_IFLNK 0xA1ED`, `slnk`/`rhap`, data fork = target path. **Hard-link DELETE:** decrements link count, stitches the prev/next chain, and reclaims the inode + its blocks when the last alias is removed. CLI `create-symlink-image` / `create-hardlink-image` / `delete-hardlink-image`. **Host Apple `fsck_hfs` (540.1 diskdev_cmds, incl. `HardLinkCheck`):** `newfs_hfs` 64 MiB SAKH5 → file + symlink + 2 hard links (inode link count 3) → "Checking multi-linked files … Checking catalog hierarchy … appears to be OK"; still OK after deleting one link (count 2) and after deleting all + reclaiming the inode. **Unit:** `hfsFileSystemWriter_createsHardlinksAndSymlinks` (symlink target read-back; create count 2→3; delete 3→2→1→0 with inode reclaim; non-hard-link delete rejected; consistency clean); core suite green; clang-format/lizard/cppcheck clean. **macOS-KERNEL MOUNT Apple-CERTIFIED 2026-06-27 (Sequoia VM):** kernel mounted SAKH5, `ls -li` showed `hard1.txt`/`hard2.txt`/`shared.txt` all **inode 16, link count 3** plus `link-to-shared -> shared.txt`; `cat hard1.txt hard2.txt link-to-shared` returned the **identical shared payload** three times (hard links reassembled to one inode + symlink followed); Apple native `fsck_hfs` (hfs-683.100.9) "Checking multi-linked files … appears to be OK". Evidence `external.hfs-hardlinks-symlinks/report.json` + 3 PNGs. **Reader-side hard-link resolution (the S.A.K. browser following an alias to the inode's data) is a separate read-path follow-on; the macOS kernel performs the authoritative round-trip.** See [[h2-hfs-btree-scope]] |
| H6 ✅ | Attribute overflow records + broad attribute growth (H-f) | Large xattrs / ACLs | `fsck_hfs` attribute buckets clean — **EXIT GATE MET + Apple-CERTIFIED 2026-06-27 (engine + Apple-`fsck_hfs`-clean + unit + macOS-kernel mount + kernel xattr read-back).** Replaced the single-leaf attributes-tree gate with a **streaming multi-leaf engine** mirroring the certified catalog (H2) + extents (H4) models (load → insert/remove edits → split/merge collapsing depth → rebuild index bottom-up → compose header; node-pool growth doubles each retry). **Broad growth:** inline + fork-backed attribute create/delete now grows the attributes B-tree to arbitrary depth/width. **Attribute overflow records:** a fork attribute with >8 fragments emits a `kHFSPlusAttrForkData` record (startBlock 0, `theFork.totalBlocks` = ALL blocks, first 8 extents) + one `kHFSPlusAttrExtents` (0x30) record per extra group of ≤8 extents, keyed by the cumulative `startBlock` — the catalog-extents-overflow accounting in the attributes tree. Records sort by (fileID, attrName, startBlock) per Apple's `CompareAttributeKeys`; delete frees every extent across the fork-data + overflow records. Rules from `hfs_format.h` + `fsck_hfs` `CheckAttributeRecord`/`AttrBTChk` (`forkData.totalBlocks` == sum of every extent; each overflow key startBlock == cumulative blocks; overflow follows a forkData/overflow record). **Apple `fsck_hfs` (540.1 diskdev_cmds):** clean on a 160-attribute **depth-2** attributes tree, and on a fragmented fork attribute forced into a fork-data + **2 overflow records** (40 blocks), and after deleting it. **Unit:** `hfsFileSystemWriter_growsAttributesNodePoolOnRootLeafSplit` (50 inline creates → depth ≥2; readback across the multi-leaf chain; delete + consistency); 143/143 core; clang-format/lizard/cppcheck clean; obsolete bounded engine removed. **macOS-KERNEL MOUNT Apple-CERTIFIED 2026-06-27 (Sequoia VM):** kernel mounted `H6ATTR`, `xattr | wc -l` = **160** (enumerated all attributes from the depth-2 tree) + read a value back, and Apple native `fsck_hfs` **hfs-683.100.9** "Checking extended attributes file … appears to be OK"; kernel mounted `OVF2`, `xattr -p bigfork | wc -c` = **20001** (reassembled the full value across the overflow records) + native `fsck_hfs` clean. Evidence `external.hfs-attribute-overflow-multileaf/report.json` + 3 PNGs. See [[h2-hfs-btree-scope]] |
| H7 ✅ | Big-endian journal replay + HFS wrapper write edge (H-g, H-h) | Completeness | Unit byte-equality (BE); `fsck_hfs` on wrapped volume — **EXIT GATE MET 2026-06-27.** H-g: big-endian journal replay (endian-marker-driven decode; payloads verbatim; native-endian write-back) unit-proven **byte-identical to the certified LE direct-write path** (the gate; no Apple PowerPC rig). H-h: the wrapper write path (embedded-volume `volume_offset` threaded through every I/O + alternate-header sync, fail-closed behind `allow_wrapped_volume`) Apple-certified — diskdev_cmds 540.1 `newfs_hfs -w` made a real wrapper, S.A.K. mutated the embedded HFS+ through the wrapper offset, and Apple `fsck_hfs` is clean before AND after. Core suite 150/150; clang-format/lizard/cppcheck clean. Evidence `external.hfs-bigendian-journal-wrapper`. See [[h2-hfs-btree-scope]] |
| H8 | **HFS+ full-driver certification gate** | Flip HFS matrix to Yes | Physical USB destructive + crash + rollback on an Apple-written HFS+ volume; Apple `fsck_hfs` + kernel RW mount |

### APFS track

| MS | Name | Unlocks | Exit gate |
|---|---|---|---|
| A1 ✅ | Multi-CIB / multi-chunk space manager (A-a) | Arbitrary size > 128 MiB | `fsck_apfs` + kernel mount on a multi-GB generated container (closes the 51 GB `spaceman_sanity_check` rejection) — **DONE 2026-06-14** (64 MiB/256 MiB/1 GiB Apple `fsck_apfs` + kernel rw mount + write round-trip + crash/rollback + physical-USB; also flips the matrix's general crash/rollback/physical-USB lanes to proven). Writer UUIDs are now random v4 with read-back; HFS catalog metadata stamped |
| A2 ✅ | In-place COW checkpoint mutation of arbitrary Apple containers (A-b) | Mutate real Apple media without full rewrite | **ENGINE + PRODUCTION PARITY Apple-certified.** The COW engine is certified across single-CIB → multi-CIB → metadata-overflow → CAB tier (cap 32 TiB) FORMAT+COMMIT (kernel RW-mount + `fsck_apfs` clean on physical 238 GB / 2 TiB-on-4 TB / 8 TB); full-tree round-trip + all 4 directory mutations; multi-leaf fs-tree, multi-extent, main+IP free-queues + crash rollback, repeated-overflow + CAB in-place commit, full-Unicode filenames; the **~2.9–7.8 TiB dead zone FULLY CLOSED** (FORMAT f604cd9 + in-place COMMIT 34d70d3, multi-block spaceman, 4 TiB Apple-certified). **Production parity COMPLETE:** the File Management bridge AND the Partition Manager queue (`PartitionScriptBuilder`, 074c213) both route **every** APFS file/directory mutation — create/delete/write, same-dir child rename (61e1c6d), cross-directory reparent **move** (6a40adb + c556236, Apple-certified), and a true in-place byte-range **patch** that preserves the object id (16d629f, Apple-certified: the VM caught a real alloc-count/overallocation bug apfsck missed, then clean) — onto the certified crash-safe `commit-raw-*` COW commands. Only volume-label change remains on the legacy raw writer (not a checkpoint mutation). A8 (arbitrary multi-volume + snapshot physical gate) out of scope per Randy 2026-06-18. See [[apfs-a2-inplace-commit-status]] |
| A3 ✅ | Snapshots create/delete/revert (A-c) | Snapshotted containers writable | `fsck_apfs` snapshot checks; kernel mount; `tmutil`/`diskutil` cross-check — **CREATE + DELETE + REVERT all Apple-CERTIFIED (A3 COMPLETE 2026-06-26).** **Revert** (`commitInPlaceSnapshotRevert` + `commit-image/raw-snapshot-revert`, evidence `external.apfs-cow-snapshot-revert`) writes Apple's DEFERRED-revert tag — byte-exact to ground truth harvested from a real macOS 15.7.4 revert (`__fs_snapshot` op 5 via an ad-hoc-entitled helper): a COW'd volume superblock with only `revert_to_xid` (the snapshot xid) + `revert_to_sblock_oid` (the frozen physical-superblock paddr) set, everything else identical, the snapshot KEPT; the kernel completes the revert (discarding divergence) on the next mount. **Apple-certified on a real Mac (apfs_kext 2332.140.13): the candidate image attached, the kernel COMPLETED the deferred revert on mount (continued the checkpoint to xid 8, "last modified by apfs_kext"), `fsck_apfs -n` enumerated `snapshot 1 of 1 (to-revert, transaction ID 3)` and reported container + volume OK, and `diskutil apfs listSnapshots` confirmed the snapshot kept.** Plus unit `apfsWriter_inPlaceSnapshotRevertTagsDeferredRevert` + full suite green + clang-format/lizard/cppcheck clean. **Delete** (`commitInPlaceSnapshotDelete` + `commit-image/raw-snapshot-delete`, commit 93a7ba4, evidence `external.apfs-cow-snapshot-delete`) is the byte-perfect inverse of create — strips the `j_snap_metadata`+`j_snap_name` records + the omap-snap entry, frees the snapshot-exclusive frozen blocks, and restores the volume exactly to its snapshot-free shape (`num_snapshots` 0, `alloc_count` back to base, omap snapshot fields 0). **Apple-certified: `fsck_apfs -n` clean with NO snapshot enumerated (vs create's `snapshot 1 of 1`); kernel RW-mounted (xid→6); `diskutil apfs listSnapshots` → `No snapshots for disk1s1`.** Plus host `apfsck` clean + unit `apfsWriter_inPlaceSnapshotDeleteRestoresSnapshotFreeState`. Snapshot **create** runs on the certified crash-safe COW engine (`commitInPlaceSnapshotCreate` + `commit-image/raw-snapshot-create` CLI): freezes the volume into a physical superblock copy + frozen extent-ref tree, records the snapshot in a new omap-snapshot tree (subtype 0x13) + the snap-meta tree (`j_snap_metadata` + `j_snap_name`), bumps `num_snapshots`/`om_snap_count`/`om_most_recent_snap` and the logical `apfs_fs_alloc_count` (2·before−3, decoupled from the physical spaceman +3). **Byte recipe harvested from a real macOS Sequoia sealed-system snapshot** (all prior UNSURE fields resolved against the live container). **Apple-certified on a 256 MiB generated container (commit 94b28c9, evidence `external.apfs-cow-snapshot-create`): (1) `fsck_apfs -n` — container + volume + snapshot CLEAN, enumerating `snapshot 1 of 1 (sak.refactor.check, transaction ID 3)` + checking the snapshot-metadata tree; (2) the macOS kernel (`apfs_kext 2332.101.1`) mounted the bare container RW and continued the checkpoint (xid 3→5) with the snapshot intact; (3) `diskutil apfs listSnapshots` enumerated the snapshot by name + xid.** Also host `apfsck` CLEAN + unit test `apfsWriter_inPlaceSnapshotCreateAddsSnapshot` + full suite green (no A2 regression; fixed a latent shared `writeRotatedCib` chunk `ci_xid` bug `apfsck` caught). **A3 COMPLETE — create + delete + revert all Apple-certified; the lane is ✅. See [[apfs-a3-snapshots-status]].** |
| A4 ✅ | Multi-volume containers (A-d) | Multi-volume Apple media | `fsck_apfs` multi-volume; kernel mount of each volume — **EXIT GATE MET + Apple-CERTIFIED 2026-06-26 (engine + apfsck + unit + macOS-kernel mount of each volume + Apple `fsck_apfs` both-volumes-clean).** The format request takes `additional_volume_names`; each extra volume gets its own object map (+ tree), extent-ref tree, snap-meta tree, empty root fs-tree, and volume superblock in a six-block set at the end of the first volume's reserved prefix, all sharing the one container space manager. The container superblock's `nx_fs_oid` array carries every volume's virtual superblock OID, the container omap maps each to its physical superblock, `nx_next_oid` clears all assigned virtual OIDs (superblock + root-tree per volume), and each volume superblock stamps its own `apfs_fs_index`, virtual OIDs, and UUID. **`fsck_apfs` requires `nx_max_file_systems = ceil(bytes / 512 MiB)` exactly** ("as the reference requires", apfsck `super.c:1288`), so the container is **sized** so the formula covers every volume (the field is never overridden — 2 volumes need > 512 MiB); the extra volumes' metadata must fit chunk 0, so the metadata-overflow tier is fail-closed for multi-volume. **Host apfsck (`-cw`, v0.2.1):** 1 GiB SAKVOLA+SAKVOLB container clean — TWO `ALLOCDBG fs_alloc_count=5 walked=5` lines (one per volume) prove both volume superblocks are parsed + allocation-cross-checked; negative control (corrupt vol2 superblock block 0xe3) → "Object header: bad checksum" exit 1. **Unit:** `apfsWriter_formatsMultiVolumeContainer` (nx_max_file_systems/nx_fs_oid/nx_next_oid + both volume superblocks' fs_index/OIDs/names/UUIDs + reader walks vol0 + 64 MiB two-volume fail-closed); full core suite green; clang-format/lizard/cppcheck clean. **macOS-KERNEL MOUNT Apple-CERTIFIED 2026-06-26 (macOS Sequoia VM, qemu/KVM recovery, `apfs_kext 2332.101.1`):** the kernel synthesized the bare container (/dev/disk0) and **mounted BOTH volumes** — `diskutil apfs list` showed `/Volumes/SAKVOLA` + `/Volumes/SAKVOLB`; then Apple's native `fsck_apfs -n /dev/rdisk0` verified **each** volume and the container: "The volume … appears to be OK" (rdisk1s1 = SAKVOLA), "The volume SAKVOLB was formatted by S.A.K. Utility APFS writer … appears to be OK" (rdisk1s2), "The container /dev/rdisk0 appears to be OK." Evidence `external.apfs-multi-volume/report.json` + apfs.a4-{kernel-mount-both-volumes,fsck-both-volumes-PASS}.png. **A4 EXIT GATE FULLY MET.** |
| A5 ✅ | APFS compression read+write (A-e) | Compressed files | Apple kernel reads S.A.K.-written compressed file; byte-match decode — **EXIT GATE MET + Apple-CERTIFIED 2026-06-27 (engine + apfsck + unit + macOS-kernel mount + md5 byte-match + Apple `fsck_apfs`).** Inline zlib transparent compression. **WRITE:** `commit-image/raw-file-insert --compress-zlib` stores the file in an embedded `com.apple.decmpfs` xattr (16-byte `apfs_compress_hdr` {signature `cmpf`, algo `APFS_COMPRESS_ZLIB_ATTR`=3, uncompressed_size} + a zlib stream, or a 0xFF-prefixed stored block when compression does not shrink the data), with **no data stream**; the inode is `UF_COMPRESSED` (bsd_flags 0x20) + `uncompressed_size`/`APFS_INODE_HAS_UNCOMPRESSED_SIZE`. It runs on the certified crash-safe in-place COW engine as a metadata-only insert (0 data blocks, 0 extent-ref slots). Shared `include/sak/apfs_compression.h` holds the decmpfs constants + inline zlib codec. **READ:** the APFS reader enumerates `j_xattr` records, captures the embedded `com.apple.decmpfs` value, and decodes it (inline zlib/plain) back to the original bytes instead of the absent data stream; listings report the decmpfs uncompressed size. Layout mirrors Apple `apfs/raw.h` + `apfsck` `compress.c`/`xattr.c` (the validator the cert satisfies): `parse_decmpfs` fail-closes with "is not compressed but has decmpfs xattr" unless the inode is correctly `UF_COMPRESSED`, and `apfs_compress_open` inflates the inline zlib to exactly `hdr.size`. Files whose compressed attribute would exceed `APFS_XATTR_MAX_EMBEDDED_SIZE` (3804) fail closed (resource-fork compression for larger files is a documented follow-on; lzvn/lzfse/lzbitmap + resource-fork **read** likewise — HFS+ already carries the lzvn/lzfse codecs). **Host apfsck (`-cw`):** 64 MiB SAKA5 with a compressed `doc.txt` clean (apfsck reached + passed `parse_decmpfs` + `apfs_compress_open`); negative control (byte flip) → "Block zero: bad checksum" exit 1. **Unit:** `apfsWriter_insertsInlineCompressedFile` (reader byte-match round trip == original; listing size == uncompressed; truncated-read prefix; nx checksum + xid advance; incompressible 8 KiB fails closed); core suite green (143/143); clang-format/lizard/cppcheck clean. **macOS-KERNEL MOUNT Apple-CERTIFIED 2026-06-27 (Sequoia VM, `apfs_kext 2332.101.1`):** kernel synthesized the bare container + auto-mounted `/Volumes/SAKA5`; `md5 /Volumes/SAKA5/doc.txt` = `829d6768845e56d8075ac7759ea1de01` — **byte-identical to the host payload md5** (the kernel decoded the inline-zlib decmpfs to the exact original 20292 bytes); then Apple native `fsck_apfs -n /dev/rdisk1` — "formatted by S.A.K. Utility APFS writer and last modified by apfs_kext", "Checking the fsroot tree … extent ref tree", "The volume /dev/rdisk1s1 … appears to be OK", "The container /dev/rdisk1 appears to be OK." Evidence `external.apfs-compression/report.json` + apfs.a5-{kernel-mount-md5-bytematch,apple-fsck-PASS,diskutil-list}.png. **A5 EXIT GATE FULLY MET.** |
| A6 | Encryption, credential-gated (A-f) | **✅ CERTIFIED 2026-06-27** — FileVault/encrypted APFS with user secret; macOS kernel unlocked + mounted the S.A.K.-written volume + Apple `fsck_apfs` OK | Apple unlocks + mounts the S.A.K.-written encrypted volume in VM — **DONE** |
| A7 🟡 | Resize + clones + sparse + hardlinks + xattr/ACL + sealed-volume policy (A-g, A-h) | Remaining native ops | `fsck_apfs` + kernel mount per op; `diskutil` resize cross-check — **IN PROGRESS 2026-06-28: 5 of 6 capabilities on the certified COW engine, host-apfsck-clean (exit 0) + unit-certified (macOS-kernel mount is the next external pass for each).** (1) **arbitrary xattr/ACL** (`19335e6`): a file insert attaches any named attributes (`com.apple.system.Security` ACL → HAS_SECURITY_EA, `com.apple.FinderInfo` → HAS_FINDER_INFO, user xattrs); reader surfaces them. (2) **sparse files** (`19335e6`): trailing-hole insert → `INODE_IS_SPARSE` + `INO_EXT_TYPE_SPARSE_BYTES` xfield + explicit phys-0 hole extent (apfsck: extents consecutive, `d_bytes==alloced_size`, `d_sparse_bytes==i_sparse_bytes`); reader zero-fills. Also fixed the documented **A2 DSTREAM-xfield apfsck divergence** (flags 0x08→0x20 = Apple's XF_SYSTEM_FIELD; kernel-safe) + two latent reader xfield-alignment bugs. (3) **sealed-volume policy** (`4316913`): every COW commit fails closed on a signed-system volume (`APFS_INCOMPAT_SEALED_VOLUME` or `apfs_integrity_meta_oid`), typed override documented. (4) **file clones** (`e6c1112`): clone shares the source's data stream (clone inode `private_id` = source's dstream id), so one `j_dstream_id` (refcnt→2) + one extent set are shared; **extentref refcnt stays 1** (the earlier "refcnt=2" plan note was wrong — see apfsck inode.c:90/:925); both inodes WAS_CLONED/WAS_EVER_CLONED; no data copied; reader reads both names to the same bytes. CLI `commit-image-file-clone`. (5) **file hard links** (`f91289e`): a second name resolves to one inode (nlink 2); each name carries a SIBLING_ID dentry xfield + a `j_sibling_link` + a `j_sibling_map` (apfsck cross-checks parent+name, requires a map per link and sibling count == link count; primary = lowest sibling id); the link commit adds a name (no inode/data, file-count delta 0, two sibling ids consumed). CLI `commit-image-file-hardlink`. **REMAINING: container/volume resize (A-g, spaceman reshape — the largest piece: must preserve the live allocation bitmap + evolving spaceman state across a grow; bounded in-chunk grow <128 MiB avoids chunk-add relocation). + macOS-kernel mount + `fsck_apfs` external cert for all of A7.** |
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

> **Status (2026-06-20): A2 COMPLETE + Apple-certified ✅ (engine + full production
> parity).** The in-place COW checkpoint engine is certified across single-CIB →
> multi-CIB → metadata-overflow → CAB tier (cap 32 TiB) incl. the ~2.9–7.8 TiB dead
> zone (FORMAT + commit), crash-rollback + free-queues, on physical 238 GB / 2 TiB /
> 8 TB media. **Production parity done:** the File Management bridge AND the Partition
> Manager queue both route every APFS file/directory mutation —
> create/delete/write/rename/**move** (reparent)/**patch** (true in-place, object id
> preserved) — onto the certified `commit-raw-*` COW commands; only volume-label change
> stays on the legacy writer. **Also done: H2 exit gate ✅.** Out of scope: A8 physical
> multi-volume + snapshot gate. HFS non-gate follow-ons: map-node growth >~32000,
> streaming split.
>
> **H3 (2026-06-21) ✅ COMPLETE + macOS-kernel CERTIFIED.** Delete-side underfull leaf
> merge/rebalance implemented + adversarially reviewed (0 defects) + unit-proven
> (decisive merge collapse + 300-op randomized churn) + Apple `fsck_hfs` clean on a
> churned volume that merged depth 4→3 / 577→47 nodes. **macOS Sequoia kernel
> RW-mounted the merged catalog, wrote a file into it, and Apple `fsck_hfs` is clean
> after S.A.K. replayed the kernel's journal** (4 txns / 9 blocks). The qemu-11
> macOS-HID flakiness turned out to be the known QEMU #2349 (not a qemu break, not
> broken control); reliable headless driving = sole VNC client + QMP stop/cont +
> persistent `vncdotool`. Next: A3 (snapshots) or H4 (allocator/extents).**

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
