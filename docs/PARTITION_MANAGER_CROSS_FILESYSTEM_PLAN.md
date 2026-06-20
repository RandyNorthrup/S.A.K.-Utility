# Partition Manager Cross-Filesystem Support Plan

Status: Milestone A/B plus ext2/ext3/ext4 create/format/repair/grow/shrink, Linux swap create/format queue wiring, HFS+/HFSX bundled sparse-staged create/format/repair wiring, HFS+ image-only data-fork overwrite plus data/resource-fork allocated-block grow/shrink/truncate, bounded fork-backed-attribute allocation growth, constrained empty-file and empty-folder catalog create/delete, single-leaf catalog rename/move, bounded file create with data-fork allocation, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release, core support and helper bridge, APFS read-only browse/extract/export, APFS image-only format scratch-image generation, APFS image-only root-file write/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change with payload/read-back, directory-empty, label read-back, or negative-read-back proof, APFS image-only object-checksum repair, generated APFS create/format/repair queue wiring, and generated-layout APFS raw root-file write/patch/delete plus empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label Apply wiring are implemented; ext VM/Linux proof for ext2/ext3/ext4, Linux swap image-level compatibility proof, Linux swap raw-partition VM proof, HFS+/HFSX image-level tool proof, physical USB ext2/ext3/ext4 plus Linux swap destructive proof, physical HFS+/HFSX sparse-staged destructive proof, offset-based raw-probe helper proof, physical HFS+/APFS read-only plus HFS selected-attribute and APFS bounded export proof, HFS+ image-only same-size initial-extent and extents-overflow overwrite unit proof, HFS+ image-only data/resource-fork allocated-block grow/shrink/truncate replacement proof, HFS+ bounded initial-extent data/resource/fork-backed-attribute allocation-growth proof, HFS+ empty-file/non-empty-file/empty-folder create-delete plus single-leaf catalog rename/move, bounded folder-tree delete, and secure released-block wipe catalog mutation unit proof, HFS staged raw data/resource/inline/fork-backed-attribute plus single-leaf catalog rename/move and bounded secure delete physical Apply proof, HFS writer CLI fail-closed plus successful helper write/read-back, catalog rename, and bounded folder-tree delete CTest coverage, APFS generated-format-image plus multi-block seed-file certifier proof, APFS generated-image root-file write/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label proof, APFS generated-image checksum-repair proof, APFS raw format/write/patch/delete/empty-directory-create/delete/root-directory-child-file-write-patch-delete/volume-label-change/repair proof on pinned expendable media with non-empty directory delete blocked, APFS writer CLI self-test coverage for image format, image volume-label change, root-file write/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, repair, and raw-path refusal, APFS writer CLI physical format/repair proof, and APFS raw volume-label Apply proof are recorded.

Current APFS raw-write boundary: the original 51 GB generated-raw claim was
invalidated by a `spaceman_sanity_check` rejection; the A1 multi-CIB
space-manager and the A2 in-place COW commit engine have since been implemented
and Apple-certified (`fsck_apfs` + kernel RW mount across single-CIB → multi-CIB
→ metadata-overflow → CAB tier, on physical 238 GB / 2-4 TB / 8 TB USB). As of
the A2-3 production wiring (2026-06-20), generated APFS raw **format, repair, and
in-place file + directory mutation** — root-file write/patch/delete, empty
root-directory create/delete, root-directory child-file write/delete, rename,
and the File Management bridge's APFS file + directory routes — all run on the
certified crash-safe COW engine, supported from 64 MiB through a 32 TiB cap. The
writer's geometry gate is the precise authority; the ~2.9–7.8 TiB
metadata-overflow inline cib-array band and arbitrary non-generated Apple APFS
mutation remain fail-closed. The single capability-matrix owner is
[APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](APFS_HFS_FULL_DRIVER_WRITE_PLAN.md) (driver
matrix A-b / milestone A2).

2026-06-09 engine milestone update: the first certifiable slices of the
2026-06-08 hard-boundary milestones are now implemented and test-proven.

- HFS+ catalog B-tree root-leaf split is implemented for one-level catalog
  trees: when a create operation no longer fits the single root leaf, the
  writer allocates three free nodes from the catalog header-node map, builds
  new left/right leaf nodes plus a new root index node as orphan writes,
  commits by rewriting the header node (tree depth, root, first/last leaf,
  leaf-record count, free-node count, and map bits) in one node-sized write,
  then erases the freed old root leaf so `fsck_hfs` accepts the volume. The
  split requires big keys plus variable-length index keys, at least three free
  nodes inside the header-node map record, and a consistent allocation map;
  every precondition fails closed. Catalog mutations on the resulting
  two-level tree remain blocked (`single leaf node` blocker), so a split
  volume is create-once until depth-2 leaf mutation ships.
- Broad HFS+ allocation growth through extents-overflow insertion is
  implemented for data forks, resource forks, and bounded file create: growth
  beyond eight extents keeps the first eight extents in the catalog record and
  inserts overflow records (eight extents per record, keyed by file ID, fork
  type, and logical start block) into the extents-overflow B-tree. The
  extents engine supports materializing the first leaf in an empty tree,
  single-leaf insertion/removal, root-leaf split, freeing the tree when the
  last record is removed, and erases freed nodes. Allocated-file delete and
  folder-tree delete now remove the deleted forks' overflow records in the
  same operation. Multi-leaf extents trees beyond one root split remain
  blocked.
- HFS+ compressed-file round trips are implemented for `com.apple.decmpfs`
  type 3 (inline zlib or raw-marker payloads) and type 4 (resource-fork
  chunked zlib with the 64 KiB chunk table and resource-map trailer): reads
  decompress transparently through `readFile` (so Browse/extract and exports
  cover compressed files), and `replaceCompressedFileContent` rewrites
  compressed content behind the explicit compressed-file mutation opt-in with
  decompression read-back proof. Compressed replacement is exposed at the
  core-API and `sak_hfs_writer_cli.exe` `replace-compressed-image` level only
  (same exposure model as generated-image APFS root-file writes); it is not a
  Partition Manager panel action until a dedicated UI milestone adds its own
  queue operation, safety validation, and Apply review. Compression types 7/8
  (lzvn) and 11/12 (lzfse) remain blocked, and attribute-tree overflow records
  remain blocked.
- APFS snapshot and encrypted/protected state is now pinned structurally in
  the generated-layout verifier used by every generated-layout mutation
  route: the volume superblock must carry no snapshot-metadata tree, no
  revert XID/superblock, zero snapshots, and zero volume flags (encryption or
  protection state), and both object maps must carry no snapshot or pending
  revert state. The read-only detector's object-map snapshot field offsets
  were corrected to the on-disk `omap_phys` layout (`om_tree_oid` at 0x30,
  snapshot tree at 0x38, most-recent snapshot at 0x40, pending revert at
  0x48/0x50).

2026-06-10 engine milestone update: two-level catalog mutation, alternate
volume header synchronization, and attribute creation are now implemented and
certified.

- The catalog writer is now a unified tree-mutation engine covering one- and
  two-level catalog B-trees. Every catalog mutation (file/folder create,
  delete, allocated delete, folder-tree delete, rename/move) loads the full
  working tree (header-node map, root index node, all leaves; bounded to 64
  leaves), applies per-leaf record edits with root-index key maintenance,
  splits any leaf that overflows at either depth (the depth-1 root-leaf split
  keeps the old root intact until the single header-node commit write),
  removes leaves that empty, collapses two-level trees back to one level when
  a single leaf remains, frees and erases retired nodes so `fsck_hfs` accepts
  the volume, and keeps free-node/map/leaf-count accounting exact. Unit
  coverage drives a catalog through third-leaf splits, first-leaf index-key
  updates, cross-leaf rename, folder lifecycle, and a full drain back to an
  empty one-level catalog; the CLI self-test proves post-split create, rename,
  and delete on a real `newfs_hfs` volume with `fsck_hfs -n -f` after every
  phase; physical raw proof on the pinned USB HFS partition stages depth-2
  create, attribute create, and delete with staged `fsck_hfs` and raw
  read-back hashes.
- Every operation that mutates the volume header now synchronizes the
  alternate volume header (the 512-byte copy 1024 bytes before the end of the
  volume), replacing the previous "alternate volume header was not updated"
  warning class with a synchronized proof line.
- New extended attributes can now be created: `createInlineAttributeValue`
  plus the `create-inline-attribute-image` CLI command insert an inline
  attribute record into the attributes B-tree (materializing the first leaf
  in an empty tree, inserting into a single-leaf tree, or splitting the root
  leaf), and set `kHFSHasAttributesMask` on the owning catalog record so
  `fsck_hfs` attribute-count buckets stay consistent. Real-volume CLI proof
  materializes the attributes tree on a `newfs_hfs` volume, passes `fsck_hfs`,
  and verifies the created value hash through the probe certifier.

2026-06-10 second-wave update: the previously listed engine milestones are now
implemented and certified.

- Attribute lifecycle is complete: `deleteAttributeValue` removes inline and
  fork-backed attributes (releasing allocation blocks and the free-block
  counter exactly, freeing the attributes B-tree leaf when the last record is
  removed, and clearing `kHFSHasAttributesMask` when a file loses its last
  attribute), and `createForkAttributeValue` creates fork-backed attributes
  with fresh allocations. Real-volume CLI proof runs create/delete cycles with
  `fsck_hfs -n -f` after every step.
- decmpfs LZVN and LZFSE codecs (types 7, 8, 11, 12) read and write through
  Apple's own reference library vendored at `third_party/lzfse` (BSD-3,
  registered in THIRD_PARTY_LICENSES.md), including the bare offset-table
  resource-fork container used by types 8/12 and the LZVN raw-chunk escape.
  Replacement round trips run the real encoders against the matching decoders.
- The catalog tree engine now rebuilds index levels generically: three-level
  catalogs (root index over index nodes over leaves, with per-level sibling
  chains) mutate, split, and collapse back through depth 2 to depth 1; the
  working-set bound is 256 leaves. The depth-3 unit scenario drives ~150
  long-name creates through two root splits and drains back to an empty
  one-level tree; the CLI self-test proves depth-3 bulk create and delete on a
  real `newfs_hfs` volume with `fsck_hfs` clean after each phase (the
  "Invalid sibling link" failure mode fsck detects was caught and fixed during
  bring-up).
- HFS+ journal replay is implemented for little-endian in-filesystem journals:
  journal-info-block and header validation (magic, endian, checksum,
  geometry), transaction checksum verification, circular-buffer reads,
  sector-addressed block writes under the write cap, and a clean-header
  commit. Certification is by byte-equality: the unit scenario replays a
  synthesized dirty journal and requires the result to byte-match a volume
  produced by the already-certified direct write path, with fail-closed proofs
  for tampered transactions and non-journaled volumes. Caveat: Windows has no
  external journal replayer (the Linux `fsck_hfs` build stubs replay out), so
  crash-generated Apple journals cannot be exercised on this rig;
  `newfs_hfs -J` volumes (needs-init journals) are handled and proven in the
  CLI self-test.

2026-06-10 external-fixture validation (real Apple-built media, no VM): the
HFS+ volume inside the user's `Sonoma 14.dmg` installer (14 GB, journaled,
LZVN-compressed, 2519 catalog records, written by macOS itself) was carved and
run through S.A.K.:

- The consistency check passes on the Apple-built multi-level catalog.
- Three Apple-encoded LZVN decmpfs files (`InstallAssistant`,
  `createinstallmedia`, `License.html`) read through the S.A.K. decmpfs path
  byte-match the independent 7-Zip LZVN decode (SHA-256 equality), validating
  the type-7/8 decoders against Apple's own encoder and a second independent
  decoder.
- The real macOS-written journal header parses cleanly (magic, endian,
  checksum algorithm, and geometry verified against authentic Apple data) and
  the clean journal correctly no-ops. The remaining journal caveat narrows to
  crash-generated (dirty) Apple journals, which require a macOS VM.

2026-06-10 macOS-VM external validation (Apple's own tools on S.A.K. output):
a Sonoma recovery VM now runs Apple validators against S.A.K.-generated
filesystems, closing the "no external validator on Windows" gap for
generated-layout APFS.

- Apple `fsck_hfs` (hfs-650.0.2) passed S.A.K.-built HFS+ end to end:
  catalog, extents overflow, multi-linked files, depth-3 catalog hierarchy,
  extended attributes, and volume bitmap; macOS auto-mounted the volume
  read-write.
- Apple `fsck_apfs` caught a real generated-APFS defect the Windows-side
  verifier had accepted: object headers lacked APFS storage-class flag bits
  (`o_type 0x1 should be 0x80000001`). The writer now emits ephemeral
  (superblock, space manager), physical (object maps and their B-trees), and
  virtual (volume superblock, root fs-tree) storage classes, and the layout
  verifier expects them. This converts the prior "no APFS validator"
  rationale into an active validation loop: future generated-APFS changes can
  be re-checked against `fsck_apfs` in the VM.
- Second `fsck_apfs` iteration caught and fixed `nx_next_oid (1000) is less
  than the minimum (1024)` (APFS reserves OIDs below 1024; the writer now
  seeds `nx_next_oid` at 1024). Third iteration identifies the next concrete
  milestone: `nx_xp_desc_blocks (0) is less than 8` — the generated layout
  needs a real checkpoint descriptor area (checkpoint-map block plus
  superblock copy, with the ephemeral space manager mapped through it) before
  Apple's checker will walk past the container superblock. That structural
  addition is the queued next step for generated-APFS Apple conformance.
- **2026-06-17 — HFS+ arbitrary-depth catalog mutation promoted to the
  production write route (I1).** The H1/H2 catalog B-tree engine (split,
  rebalance, and node-pool growth at arbitrary depth/width) is now exercised
  end to end through the File Management production bridge: the
  `file_management_live_certifier --destructive` run drove create-directory,
  write-file, rename, and delete through `FileManagementFileSystemBridge`
  against a depth-4 / 525-leaf (>256) HFS+ catalog, every step passed, and
  bundled `fsck_hfs -n -f` reported the volume OK; the catalog engine itself is
  Apple-certified (kernel RW mount + `fsck_hfs` hfs-683.100.9). The registry
  capability matrix moves catalog B-tree split/rebalance from blocked to
  confirmed. Attribute-tree split, recursive extents-overflow attribute
  records, complex file delete, unbounded folder-tree delete, and broad
  allocation growth remain blocked.
- **2026-06-17 — generated APFS multi-CIB format/repair promoted to the
  production queue (I1, A1).** The A1 multi-CIB space manager (Apple-certified
  format with `fsck_apfs` + kernel mount through the metadata-overflow tier)
  is now reachable through Partition Manager: `PartitionScriptBuilder` raises
  the generated APFS raw format and repair cap from one spaceman chunk
  (128 MiB) to the certified multi-CIB / metadata-overflow range (64 MiB
  through ~2 TiB, conservatively inside the writer's non-CAB edge), and the
  `sak_apfs_writer_cli format-raw` command formats a true multi-CIB (20 GiB)
  generated container ok. The registry capability matrix moves multi-CIB
  format/repair to confirmed. In-place generated APFS root-file write/patch/
  delete stay single-spaceman-chunk (≤128 MiB) because the production write
  commands still use the single-chunk in-place path; wiring the A2 multi-CIB
  in-place commit into them is the remaining APFS write-cap step (A2 is paused
  on its 12 TB-gated tiers). CAB-tier format targets above ~2 TiB remain
  fail-closed.
- **2026-06-12 — generated APFS Apple-certified end to end.** The iteration
  loop above ran to completion: the generated layout was rebuilt to mirror a
  same-geometry `newfs_apfs` container (two-checkpoint genesis, ghost
  objects on real free-queue records, chunk-info/bitmaps inside the internal
  pool, full volume-superblock field set), and the file-system record engine
  was rebuilt from ground truth harvested by letting Apple's own driver write
  a file in the VM (hashed directory-entry keys via CRC-32C over UTF-32LE
  code points — verified against three Apple-written samples — dstream-id
  records, parent/private inode ids, NAME/DSTREAM extended fields with
  Apple's `xf_used_data` semantics, inode flag 0x8000, block-rounded extent
  lengths, ids numbered from 16, numeric name-hash key ordering). Apple's
  `fsck_apfs` now reports the generated volume and container "appears to be
  OK", and the macOS kernel auto-synthesizes and mounts the generated volume.
  The last cosmetic fsck warning was then eliminated (the extent-reference
  tree now carries a physical-extent record per file), leaving zero warnings.
- **2026-06-12 — arbitrary Apple-media APFS mutation + authentic crash-journal
  replay.** The `import-image` flow reads any unencrypted, snapshot-free APFS
  container the generic reader can walk — including containers written by
  Apple's own driver — and re-emits it through the certified writer with
  optional added or overwritten files; certified in the VM by round-tripping
  and mutating a kernel-written container with clean `fsck_apfs` verdicts.
  The former "arbitrary Apple-media APFS mutation" boundary is lifted for
  that envelope (encrypted volumes remain credential-bounded; snapshotted
  containers remain read-only). HFS+ journal replay was also validated
  against an authentic crash journal: a macOS-written `newfs_hfs -J` volume
  hard-reset mid-transaction was captured before remount, S.A.K. replayed the
  open transaction, and Apple's forced `fsck_hfs -fn` reported the volume
  "appears to be OK" (macOS subsequently mounted it read-write).

Remaining hard boundary (permanent on this rig, by design):

- Encrypted/protected-volume crypto: unlocking FileVault/Core Storage or
  encrypted APFS requires user-provided volume credentials plus Apple keybag
  cryptography, and no Windows validator exists for the decrypted result.
  Writes fail closed on any encryption state.
- Arbitrary Apple-media APFS mutation (including snapshot creation): APFS
  checkpoint/omap/xid semantics cannot be certified end to end without an
  external APFS validator, which does not exist on Windows. The certified
  posture is the structural generated-layout pinning that fails closed on any
  snapshot, revert, or encryption state.
- Catalogs deeper than three levels or wider than 256 working leaves, and
  growth of the catalog/attributes special files themselves (a 64 MB
  `newfs_hfs` volume ships a 128-node catalog file; exhausting it reports an
  explicit "does not have enough free nodes" blocker rather than attempting
  the write).
- Big-endian (PowerPC-era) journals are blocked by an explicit fail-closed
  check.

2026-06-08 update: generated-layout APFS volume-label change is now implemented
for image and raw helper paths. It updates the APSB volume-name field only after
generated-layout verification, restamps the APFS object checksum, reports old
and new labels, verifies read-back through the S.A.K. APFS browser, and is wired
to the APFS File panel mode plus `change-image-volume-label` and
`change-raw-volume-label` CLI commands. Local image proof and raw-path refusal
proof are recorded at
`artifacts\partition-manager-certification\vm-lab\apfs-cli-self-test\run-d609fd4714b244a38c21c8e9c79b78aa`.
Physical raw APFS volume-label Apply proof on the large JMicron target is now
Windows-side-only after Apple kernel validation rejected that generated layout:
`artifacts\file-management-live-certification\disk3-apfs-raw-format\run-20260612-192652`.
Current Windows-side small-target raw proof passed on JMicron serial
`DD56419883A5B`, Disk 2 Partition 2, 134,217,728 bytes, at
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`.
Apple-native raw volume-label proof still requires macOS Recovery validation of
that 64-128 MiB generated APFS target. This does not approve arbitrary Apple
APFS metadata mutation.
HFS+ data-fork, resource-fork, inline-attribute, and fork-backed-attribute mutation including bounded fork-backed-attribute growth is now wired to
the Partition Manager Apply path for selected HFS+/HFSX raw partitions through
the `HFS File` action. It reads the direct or wrapped HFS+/HFSX volume header,
stages only the HFS logical volume span inside the selected raw partition into
a sparse image, runs `sak_hfs_writer_cli.exe`, repairs/checks the staged image
with bundled `fsck_hfs`, then copies changed sparse ranges back to the same raw
partition alias. Journaled HFS+ `fsck_hfs` incomplete-verification exit code 8
is accepted only on the HFS File mutation path when journaled staging was
explicitly allowed; final raw read-back hashes must still match.
HFS+ single-leaf catalog rename/move is now implemented in the core writer,
`sak_hfs_writer_cli.exe`, safety validation, script generation, and the HFS
File dialog. It rewrites the source catalog record and matching thread record,
updates parent valence when both parent records are present, blocks root-folder
rename/move, blocks destination collisions, and blocks moving a folder into
itself or a descendant. Unit coverage proves same-parent file rename,
cross-parent file move with read-back, and folder rename; the helper self-test
proves CLI rename with listing/read-back. Broad catalog B-tree split/rebalance
remains blocked.
APFS existing-image in-place format and raw-partition generated format are now
implemented with explicit wipe confirmation, stale-edge zeroing, post-format
APFS detection, requested volume-name listing, and empty-root browse proof.
The APFS formatter now writes through a seekable target API backed by an
explicit read/write raw-device opener, while parser/browser raw access remains
read-only by default. Raw targets require explicit raw opt-in plus hardware
certification evidence flags. Queue/apply exposes S.A.K. generated APFS
format/create, generated-layout checksum repair, generated-layout bounded
root-file write/patch/delete, generated-layout empty root-directory
create/delete, generated-layout root-directory child-file write/patch/delete,
and generated-layout volume-label change through `sak_apfs_writer_cli.exe`; the APFS File
panel action stays disabled unless captured metadata proves the selected
target is a S.A.K. generated APFS layout.
`scripts/run_partition_manager_apfs_raw_format_validation.ps1` provides the
destructive elevated proof lane for expendable Apple APFS GPT partitions:
format, write one bounded root file, patch that generated file, create/delete
one empty root directory, write/read/patch/delete one bounded file inside that
generated root directory, prove non-empty directory delete stays blocked, corrupt a
generated APFS metadata checksum on block 199, repair it, verify file read-back after
repair, change the APFS volume label, verify relabeled root listing plus patched
file hash preservation, delete the generated file, and verify negative read-back. The local launcher starts
that lane through Windows UAC `runas` without keypress automation.
The File Management panel reuses these readers and certified writer slices:
File Organizer generic moves stay local/mounted-file-API only, while Duplicate
Finder, Advanced Search, and File Explorer can target supported raw/image
ext2/ext3/ext4, HFS+/HFSX, and APFS. File Explorer exposes explicit
create/write/rename/delete only where the bridge has a certified operation.
S.A.K. now has a read-only raw signature detector for missing file-system
labels, including NTFS, exFAT, FAT12/16/32, ext2/3/4, XFS, Btrfs,
Linux swap, direct and HFS-wrapper HFS+/HFSX, and APFS. Raw ext2/ext3/ext4,
Linux swap, XFS, Btrfs, APFS, and HFS+/HFSX detections now carry read-only
metadata and parser sanity notes into Properties and Inspect. When the normal app process
cannot open `\\.\PhysicalDriveN`
read-only for this probe, the scan retries through the allowlisted elevated
`ReadPartitionProbe` helper task, capped to the detector probe window and still
read-only. ext2/ext3/ext4 also has an original read-only directory browser,
selected-file extractor, symlink target display, and bounded recursive directory
export for direct, indirect, and extent-mapped file data. Symlinks are not
followed or recreated; exports create `.symlink.txt` sidecars containing the
target text.
HFS+/HFSX now has original read-only catalog B-tree consistency checks,
attributes B-tree key scans with fork/inline metadata reporting, catalog
browsing, selected data-fork extraction, selected resource-fork extraction, and
bounded recursive directory export with resource-fork sidecars for files covered
by initial extents or HFS+ extents overflow records.
HFS+/HFSX also has manifest-approved hfsprogs 540.1.linux3-6+sak-msys command
paths: `newfs_hfs` for confirmed format and `fsck_hfs -p -f` for confirmed
repair. Windows raw partition targets use sparse staging because the MSYS HFS
tools operate on image paths, not `GLOBALROOT` device handles: S.A.K. stages a
sparse HFS image, runs hash-verified hfsprogs against that image, and writes
allocated metadata ranges back to the selected raw target. These route through
the same raw-target identity, safety validation, Apply review, manifest/hash
revalidation, operation report, and explicit confirmation model as ext.
`PartitionHfsFileSystemWriter` now starts the arbitrary-write path with
image-only HFS+ data-fork mutation, resource-fork allocated-block
replacement/truncate for existing files, bounded initial-extent allocation growth for data/resource forks, bounded file create with data, constrained empty-file/empty-folder catalog
create/delete, and single-leaf catalog rename/move. It requires explicit
writer enablement, target-write confirmation, a certification evidence ID,
journal override when the volume is journaled, compressed-file blocking through
`com.apple.decmpfs`, and read-back SHA-256 verification. Same-size overwrite is
supported for data forks covered by initial extents and extents-overflow-backed
files. Grow/shrink replacement and zero-length truncate are supported only when
the selected data or resource fork fits inside its already allocated blocks; the
writer updates the selected catalog logical-size field and zeroes stale tail
bytes on shrink/truncate, but it does not allocate new blocks. Empty-file and
empty-folder create/delete are limited to a single catalog leaf that has enough
free record space, write zero-fork file or zero-valence folder records plus
matching thread records, update catalog leaf counts, primary volume
file/folder counts, next catalog ID, and parent-folder valence when the parent
record is present, then verify by listing/read-back.
It deliberately blocks unbounded folder-tree delete, complex file delete, B-tree
split/rebalance, allocation bitmap growth, journal replay, and alternate volume
header synchronization.
`sak_hfs_writer_cli.exe` exposes those image-only overwrite, allocated-block
replacement, bounded allocation-growth replacement, data/resource-fork truncate, empty-file/folder create/delete, single-leaf catalog rename/move,
bounded file create, inline-attribute and fork-backed-attribute replacement/growth paths as a packaged helper with payload-file input for payload-writing commands and
structured JSON output; its CTest self-test now builds known HFS+ fixtures
through the probe certifier, verifies fail-closed structured rejection for
non-HFS media and missing confirmation, runs successful same-size overwrite,
data-fork grow/shrink/truncate, resource-fork replace/truncate, fork-backed-attribute
bounded allocation growth, inline-attribute and fork-backed-attribute replacement, non-empty file create/read/list/delete, single-leaf catalog rename/read-back, empty-file create/read/list/delete, and empty-folder create/list/delete, compares before/after hashes, and verifies
read-back through the
certifier. `test_partition_manager_core` proves the same behaviors through the
direct core API.
`PartitionScriptBuilder`, `PartitionSafetyValidator`, and the `HFS File` panel
action now expose those bounded HFS+ data-fork, resource-fork, and
inline/fork-backed-attribute writes plus empty-file/folder create/delete, single-leaf catalog rename/move, and bounded file create for selected raw partitions through the same queue,
Apply review, exact-target validation, HFS logical-size sparse staging, staged
repair/check, operation-report, and explicit confirmation flow as other
destructive non-native writes. Physical proof on pinned expendable USB HFS
media passed at
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`.
wider inline/broad attribute growth, compressed-file attribute
writes, unbounded folder-tree delete, complex file delete, broad allocation growth beyond the bounded initial-extent slice, B-tree
split/rebalance, and recursive overflow
from the extents-overflow file itself remain blocked.
APFS now has original read-only checkpoint/object-map traversal, volume
superblock resolution, metadata object-checksum validation for APFS object
blocks, space-manager free-space counter parsing, volume object-map lookup,
root file-system tree browsing, directory listing, selected-file extraction,
and bounded recursive directory export for normal unencrypted, uncompressed files covered by file extent
records. `PartitionApfsWriter` now provides a fail-closed writer certification
preflight for future image-only APFS mutation work; it requires explicit
experimental enablement, image-only scope, destructive evidence IDs,
transaction-safe checkpoint/object-map/space-manager proof, crash-interruption
proof, and separate hardware proof before any raw media mutation. It also has
original Fletcher-64 APFS object checksum calculation/stamping/verification, a
deterministic operation-specific image-only mutation plan builder for file,
format, repair, and resize phases, and an image-only format builder that writes
a new scratch image with an NXSB container superblock, container and volume
object maps, APSB volume superblock, root tree, spaceman free-space accounting,
and stamped APFS object checksums. `partition_filesystem_probe_certifier.exe`
can now build a 64 MiB generated APFS format image, optionally seed one root
regular file as a contiguous multi-block extent, verify S.A.K. raw APFS
detection, verify nonzero free-byte spaceman counters, and verify root listing,
seed-file read-back, and requested volume name through the read-only APFS
browser. The certifier can also copy a generated APFS image to a
separate scratch image, add or replace one bounded root regular file while
preserving existing bounded root regular files, add or replace one bounded
regular child file inside a supported generated root directory while preserving
that parent directory, update APFS volume/root-tree/spaceman metadata, and
verify detection, listing, and file read-back. The certifier can
also delete one bounded generated-layout root regular file, zero the previous
generated data-block region, delete one bounded generated-layout regular child
file from a supported generated root directory, preserve remaining bounded root
regular files/directories, and verify negative read-back or empty-directory
read-back for the deleted path.
The certifier can
also copy a generated APFS image whose supported
metadata object checksum is corrupt, restamp the recognized APFS object block
checksums in a separate scratch image, and verify post-repair detection,
listing, and seed-file read-back.
The same writer now has raw-target certifier APIs for generated/minimal APFS:
in-place format, bounded root regular-file write, bounded byte-range patch,
bounded root-file delete, bounded empty root-directory create/delete, bounded
root-directory child-file write/patch/delete, generated-layout volume-label
change, and bounded known-metadata object-checksum repair.
Before any generated APFS image or raw target is
written or repaired, the writer validates the fixed generated/minimal layout:
NXSB, container object map, volume APSB, volume object map, root tree, spaceman
object headers, object checksums, object-map references, root-tree bounds, and
free-space accounting must match S.A.K.'s generated layout. Repair tolerates a
bad checksum only on those generated metadata objects, scans only the known
generated metadata blocks, and still blocks arbitrary APFS media. The previous
large-target destructive physical lane passed Windows-side on pinned JMicron USB APFS media with volume
`SAK APFS Raw Proof`, wrote
`/sak-apfs-raw-proof.txt`, verified the initial file hash
`5844ada68560cb30055adc4b7ed2a85e13bce42aedb2b6b3691241605f1cb678`
after write, patched the generated file at byte offset 8, verified patched hash
`19b8bdc8cd59a16e481bca323519db37119b2eaaa8992898de76100c9d54b516`,
created empty root directory `/SAK Raw Proof Folder`, verified it listed as
empty, wrote `/SAK Raw Proof Folder/sak-apfs-child-proof.txt`, verified child
file hash
`f22535d6b86d4744537a0f58cdfc613162efa764610a9b191b53d5762a63566e`,
patched that child file at byte offset 8, verified patched child hash
`1d45dc05a258a2e29a7b77cdf5a6763e8f6ac138ee0c4066d6aaf6115eb45d27`,
proved non-empty directory delete is blocked, deleted the child file, verified
the directory listed empty again, deleted the directory, verified root-listing absence,
repaired one intentionally corrupted generated metadata block 199 while preserving
that patched file hash, changed the volume label to `SAK APFS Raw Relabeled`,
verified relabeled root listing and patched-file hash preservation, deleted the
file, and verified negative read-back. Latest large-target guarded run:
`run-20260612-192652`, completed 2026-06-13T02:26:55Z, is now
Windows-side-only because Apple kernel validation rejected the generated
spaceman geometry. Current Windows-side raw APFS proof passed on the 128 MiB
JMicron slice at
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`.
Current raw APFS claims cover only generated/minimal single-volume APFS targets
created by S.A.K. inside the 64-128 MiB one-spaceman-chunk envelope, not
arbitrary encrypted, compressed, snapshotted, multi-volume, or larger generated
APFS; Apple-native validation of the small target is still pending.
`sak_apfs_writer_cli.exe` is the production Apply bridge for generated APFS
create/format, generated-layout checksum repair, generated-layout raw
root-file write/patch/delete plus empty root-directory create/delete,
root-directory child-file write/patch/delete, and volume-label change, and a
helper bridge for generated-image root-file create/replace/byte-range
patch/delete plus empty root-directory create/delete, root-directory child-file
write/patch/delete, and volume-label change with payload/read-back,
directory-empty, deleted-file, old/new-label, and negative-read-back hashes.
Raw root-file, root-directory, root-directory child-file, and volume-label
commands require a Windows
raw-device target, target-size proof, selected-partition identity, explicit
confirmation, generated-layout confirmation, raw opt-in, and raw hardware
evidence.
The generated PowerShell
script validates selected partition identity, dismounts any mounted Windows
volume, calls the helper with the selected `GLOBALROOT` raw partition path and
live partition size, emits JSON helper output, and refreshes storage. The helper
has a CTest self-test for image format, generated-image volume-label change,
generated-image root-file
create/replace/byte-range patch/delete, generated-image empty root-directory
create/delete, generated-image root-directory child-file write/patch/delete,
non-empty-directory delete blocking, missing-delete blocking, checksum repair
after intentional metadata corruption, and raw-command refusal for normal file
paths, including raw volume-label refusal on normal file paths. A pinned
physical helper proof must stay inside a 64-128 MiB generated APFS target. The
current 128 MiB JMicron Windows-side raw-format proof is recorded at
`artifacts/file-management-live-certification/disk2-apfs-128mb-raw-format/report.json`.
The previous JMicron helper evidence at
`artifacts/file-management-live-certification/disk3-apfs-raw-format/report.json`
and latest helper run `run-20260612-192652` are retained as Windows-side-only
evidence after Apple rejected the large generated layout; Apple-native
validation of the small generated target remains pending.
Hardening note: the production APFS helper does not blanket-enable protected,
compressed, snapshot, or multi-volume APFS mutation options. Generated APFS v2
single-volume containers are allowed through the generated-layout proof path;
active snapshot/revert metadata, unsupported incompatible feature flags, and
multi-volume containers remain blocked unless a future operation-specific proof
explicitly covers them.
The writer also has a structured image-only execution-evidence gate, so a plan
or generated scratch image cannot be treated as executable unless the evidence
ID, operation, target path, structure mapping, checksum vectors, source/scratch
image hash chain, copy-on-write checkpoint proof, object-map update proof,
space-manager accounting proof, fsck/diskutil validation, target read-back
proof, crash replay proof, rollback-boundary proof, and artifact paths all
match the plan. It does not expose non-generated APFS file writes, APFS resize, arbitrary
Apple APFS mutation, mounting, checkpoint replay beyond the selected latest
checkpoint, or encrypted/compressed APFS reads.
S.A.K. also has a bundled-tool manifest validator, a CMake-required
`tools/filesystem/manifest.json` bundle path, manifest-approved e2fsprogs
1.47.4 binaries for ext2/ext3/ext4 command generation, and manifest-approved
hfsprogs 540.1.linux3-6+sak-msys binaries for HFS+/HFSX command generation.
`e2fsck` provides ext `check-read-only` and confirmed repair, `mke2fs` provides
confirmed ext format, `resize2fs` provides confirmed ext grow/shrink,
`fsck_hfs` provides HFS+/HFSX staged confirmed repair, and
`newfs_hfs` provides staged confirmed HFS+/HFSX format. Create Partition now exposes only certified non-native create+format targets: ext2/ext3/ext4 through `mke2fs`, HFS+/HFSX through sparse-staged `newfs_hfs`, Linux swap through the original SWAPSPACE2 writer, and generated APFS through `sak_apfs_writer_cli.exe`. ext format, repair, grow, and shrink now route through
Pending Operations, safety validation, Apply review, elevated PowerShell
execution, manifest/hash revalidation, and explicit confirmation. Linux swap can be formatted
through an original S.A.K. SWAPSPACE2 v1 header writer that runs through the same queue,
Apply review, raw-target identity, and destructive-confirmation path without external tools.
Non-native write operations now require the exact selected
`\\?\GLOBALROOT\Device\HarddiskN\PartitionM` raw partition alias in both the
safety validator and script builder. Forged, missing, or mismatched
`target_path` payloads are blocked before Apply, and the queued ext repair
dialog makes the destructive target field read-only while repair mode is active.
A disposable-image compatibility gate verifies that S.A.K., Linux `blkid`, and
Linux `swaplabel` recognize the generated SWAPSPACE2 metadata. Raw-partition VM
proof now has `scripts/run_partition_manager_linux_swap_vm_gate.ps1`, the
in-guest launcher `scripts/launch_partition_manager_linux_swap_vm_gate_local.ps1`,
and host launcher `scripts/launch_partition_manager_vm_gate_host.ps1`; it writes
SWAPSPACE2 metadata to an app-style raw partition target, verifies the raw
header, runs the S.A.K. probe certifier, and clears the disposable VM disk back
to RAW.
XFS/Btrfs write paths and non-generated APFS write paths remain blocked until
destructive certification or tool proof is complete. Generated-layout APFS
create/format, repair, root-file write/patch/delete, empty-directory
create/delete, root-directory child-file write/patch/delete, and volume-label
change are approved only through the explicit S.A.K. generated-layout helper
path. HFS+/HFSX format
and repair are approved through the
bundled hfsprogs sparse-staging path, image-level proof, and physical
raw-partition destructive proof.

## Enterprise Arbitrary-Write Scope

The requested arbitrary APFS/HFS+/XFS/Btrfs write target means mutating
pre-existing volumes not generated by S.A.K. while preserving each file system's
native allocation model, journals/logs, checkpoint or transaction semantics,
metadata checksums, snapshots/clones, compression, encryption, multi-device
state, rollback behavior, and crash recovery. That is a filesystem-driver
program, not a small panel feature. It is not certified in the current codebase
and must not be advertised as current support.

Current release-approved non-native write scope is:

- ext2/ext3/ext4 create/format/repair/grow/shrink through bundled e2fsprogs
  with manifest/hash gates and VM/Linux proof.
- Linux swap create/format through original SWAPSPACE2 metadata writer with VM
  proof.
- HFS+/HFSX create/format/repair through bundled hfsprogs sparse staging with
  image and physical raw-partition proof, plus staged selected-partition
  data-fork, resource-fork, inline-attribute, fork-backed-attribute replacement, and bounded
  fork-backed-attribute growth for HFS+ records through `sak_hfs_writer_cli.exe`.
- APFS generated/minimal create/format, helper-level bounded generated-image
  root-file write/replace/byte-range patch/delete, empty root-directory
  create/delete, root-directory child-file write/patch/delete, and volume-label
  change, certification-lane bounded generated-layout raw root-file
  write/patch/delete, empty root-directory create/delete, root-directory
  child-file write/patch/delete helper plumbing, generated-layout volume-label
  helper plumbing, and generated-layout checksum repair through original S.A.K.
  writer code and helper proof. Raw patch/delete/child-file hardware proof is
  recorded by the guarded destructive lane; raw volume-label hardware proof is
  still pending.

Current enterprise blockers for arbitrary write are:

- APFS: arbitrary Apple APFS mutation remains blocked for non-generated
  containers, snapshots/revert state, encrypted/protected volumes, compressed
  files, multi-volume containers, arbitrary object-map updates, arbitrary
  resize, and full checkpoint replay.
- HFS+/HFSX: catalog root-leaf split for one-level trees, broad allocation
  growth through bounded extents-overflow insertion, overflow-record removal
  on delete, and decmpfs type-3/type-4 compressed round trips are implemented
  with unit, CLI, and fsck_hfs proof. Catalog/extents mutation on trees deeper
  than one level (including any catalog mutation after a split), B-tree
  merge/rebalance on delete, inline/broad attribute growth, attribute-tree
  overflow records, journal replay, decmpfs lzvn/lzfse codecs, and recursive
  extents-overflow-file overflow remain blocked.
- XFS: format/repair/write remains blocked until a Windows-portable approved
  xfsprogs path or original implementation has destructive proof. Read-only
  metadata checks remain the shipped scope.
- Btrfs: format/write/repair remains blocked until a Windows-portable approved
  btrfs-progs path or original implementation has destructive proof. Repair must
  stay expert-gated because upstream Btrfs documentation treats `--repair` as a
  dangerous mode.

Promotion rule: any future arbitrary-write capability must add source-level
implementation, UI gating, safety validation, queue/apply scripts, manifest and
license gates for bundled tools if used, destructive VM proof, physical media
proof, crash-interruption proof, rollback proof, and release-readiness docs
before the action is enabled. Until then the production behavior is fail-closed.
A dedicated VM destructive proof runner
now exists at `scripts/run_partition_manager_ext_filesystem_vm_gate.ps1`, with a
VM-desktop launcher at `scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1`.
The ext format/repair/grow/shrink VM gate passed on 2026-06-05 against an app-style
`\\?\GLOBALROOT\Device\Harddisk1\Partition2` raw target and left the disposable
disk RAW after cleanup. The Linux swap raw-partition VM gate also passed on
2026-06-05 through the host launcher: guestcontrol used a real password file
resolved from VM unattended metadata, the elevated runner wrote and detected
SWAPSPACE2 on
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, and disk 1 was cleared back to
RAW. A direct-admin automation rerun also passed on 2026-06-05 PDT /
2026-06-06 UTC in `SAK-PM-Automation-Win11`: `VBoxManage guestcontrol` reached
a high-integrity administrator token as `saklab`, the host report recorded
`launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`, and the same Linux swap raw-partition proof passed
without keypress UAC. The legacy VM fallback remains opt-in only; the host
launcher refuses filtered tokens unless `-AllowUacKeypressFallback` is supplied
explicitly for manual emergency use. Linux compatibility gates also passed for
ext2, ext3, and ext4 on 2026-06-07 UTC: Arch WSL Linux `e2fsck`/`dumpe2fs`
verified each Windows-created/grown/shrunk image, Linux loop-mounted each image
read/write, wrote `sak-linux-proof.txt`, Linux rechecked clean, and bundled
Windows `e2fsck` rechecked clean after the Linux write. WSL remains
certification tooling only; it is not an app runtime dependency.
The same dedicated automation VM later reran the ext format/repair/grow/shrink
gate through direct-admin guestcontrol with `uac_accept_sent_at=null`; evidence
is under
`artifacts/partition-manager-certification/vm-lab/host-launch/ext-filesystem-20260605224315.json`
and
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext-filesystem-write/report.json`.
The host launcher now waits for Guest Additions desktop readiness with retries
before starting guestcontrol work so cold-boot VM gates do not fail on a single
early `waitrunlevel` miss.
`partition_filesystem_probe_certifier.exe` now accepts `--input-offset-bytes`,
has a generated-fixture PowerShell self-test wired into CTest and release
readiness, and can validate raw file systems embedded after container/header
padding. It can also emit HFS+/HFSX consistency and attribute metadata through
`--hfs-check`, then read selected HFS+ attribute values with file ID/name
arguments when real media exposes them. A lab-only Sonoma DMG fixture passed as real HFS+ at byte offset
134250496 with sane header metadata. The unpacked Tahoe installer contains
`SharedSupport.dmg`, which 7-Zip identifies as a compressed UDIF with an HFS+
payload; it is useful for future container-extraction testing, but it is not a
runtime asset and is not needed for current readiness.
Physical read-only Apple filesystem proof also passed on 2026-06-05 PDT /
2026-06-06 UTC using an expendable external USB NVMe disk. Normal-user raw reads
of `\\.\PhysicalDrive2` were denied, but the same partitions were readable
through `\\?\GLOBALROOT\Device\Harddisk2\Partition2` and `Partition3`. Inventory
raw detection now tries that partition alias before requesting elevated
PhysicalDrive offset reads. The physical validation script detected APFS on
partition 2 and HFS+ on partition 3 with sane metadata, browsed both roots,
read `/Fonts/00TT.TTF` read-only from APFS with SHA-256
`d075a134b3092fd36c6e45acc88d2efd163e60857cb2f0a2621569f446fa06d2`, read
`/polyhavenassets_blendermarket_v1.2.0  (Blender 4.5+).zip` from HFS+ with
SHA-256 `72222f9f83d177ea9a2970edb756ab9f3f9f6b00a12f47af63259f215d970709`,
read inline HFS+ attribute `com.apple.decmpfs` for file ID 1894 at 16 bytes
with SHA-256 `e2aed0d76e90c81c39f9916d56a8f1631c440aec356877ea7dae197b069ea64f`,
and exported `/Fonts` from APFS with bounded caps. The same physical lane now
records `validation_requirements` and `probe_limits` in the report so required
optional proofs are auditable.
APFS arbitrary-media writes remain blocked until the writer preflight has
matching operation-specific proof for non-generated containers, file writes,
and resize. Generated/minimal APFS raw format, bounded root-file write/patch/
delete, root-directory child-file, volume-label, and checksum repair are
hardware-certifiable only inside the 64-128 MiB one-spaceman-chunk envelope;
the previous large-target proof is Windows-side-only after Apple validation
rejected its spaceman geometry. The 128 MiB JMicron Windows-side raw-format
proof now passes, but Apple-native validation of that small generated target is
still pending.
Full arbitrary APFS/HFS+/XFS/Btrfs write support is not a current release
claim. Release wording must keep XFS/Btrfs to read-only metadata, HFS+/HFSX to
read-only browse plus sparse-staged format/repair, and APFS to 64-128 MiB
one-spaceman-chunk generated-layout format/write/patch/delete/repair helper scope only unless a future milestone
adds full driver/tool proof.
Evidence:
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.apple-filesystem-physical/report.json`.
APFS free-space reporting now also handles real containers whose space-manager
checkpoint object is outside the first 2 MiB probe window. The detector keeps
the byte-window `detectBytes` path for fixtures and elevated probe payloads, but
`detectFromDevice` and raw app inventory perform a bounded read-only scan of
checkpoint data blocks on random-access devices and then attach the same
space-manager details to the detection result. A second expendable USB disk
with five APFS partitions passed
`scripts/run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber 3 -AllowMissingHfs -ProbeAllApfsPartitions -RequireAllApfsPartitions`.
All five APFS probes passed with nonzero free-space counters:
partition 2 `free_bytes=50916139008`, partition 3 `free_bytes=50920783872`,
partition 4 `free_bytes=50925522944`, partition 5 `free_bytes=50930257920`,
and partition 6 `free_bytes=51055095808`. Evidence:
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.apple-apfs-multipart-physical/report.json`.
That same five-partition APFS lane now also passes
`-RequireApfsFileProof -RequireApfsExportProof` across all APFS partitions. The
harness performs a bounded recursive directory search, including hidden Apple
metadata directories when no visible file is available, and records
`candidate_directories_scanned` plus per-partition export counts.
The latest physical Apple proof reran on 2026-06-07 UTC and also verifies APFS
space-manager free-space reporting on real media: partition 2 reported
`total_bytes=127992487936` and `free_bytes=127866097664`, matching parsed
`free blocks=31217309` at a 4096-byte block size. The current HFS+ partition
reported `total_bytes=127724052480` and `free_bytes=125197647872`, listed 9
root entries, and passed selected-file plus selected-attribute read proof. This
proves HFS+ and APFS metadata, HFS+ and APFS selected-file reads, HFS+
selected-attribute reads, and APFS bounded export on physical media.
HFS+/HFSX sparse-staged format/repair proof is tracked separately in the
guarded hfsprogs destructive lane; unbounded HFS+ folder-tree delete, complex HFS+ file delete,
B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice, inline/broad HFS+ attribute growth,
compressed-file writes,
arbitrary APFS mutation, non-generated APFS file writes, APFS resize, APFS
encrypted/compressed files, and arbitrary Apple APFS repair remain blocked.
Generated APFS create/format, generated-layout volume-label change, and
generated-layout checksum repair are wired
through `sak_apfs_writer_cli.exe`, Pending Operations, safety validation, Apply
review, explicit selected-target confirmation, and production helper JSON
reports.
Generated-image root-file create/replace/byte-range patch/delete and
volume-label change are helper-exposed with payload, old/new-label,
deleted-file, read-back, and negative-read-back hashes. Generated-layout raw
APFS volume-label change is a Partition Manager Apply action with pinned
physical raw proof.
The APFS writer preflight module is compiled and tested as the certification
gate for future engine work. The writer module now also covers APFS object
checksum vectors, checksum stamping/verification, safe target-path normalization,
operation-specific image-only plan shapes for file, format, repair, and resize
workflows, structured image-only execution-evidence validation, and generated
APFS format scratch images. The generated image path writes a new non-overwrite
image file only; it stamps APFS object checksums, records a SHA-256 hash, emits
plan evidence requirements, and is verified by
`scripts/test_partition_filesystem_probe_certifier.ps1` through S.A.K. raw APFS
detection, read-only root listing, and 9000-byte seeded-file read-back. The
same script builds an empty generated image, writes a 9000-byte root file into a
separate image, writes a second root file into a non-empty generated image while
proving the original file still reads back, replaces an existing generated root
file with read-back proof, corrupts the generated root-tree
checksum, proves read-only browse fails closed on that checksum, repairs
supported APFS object checksums into a separate image, and proves post-repair
listing plus seeded-file read-back. Generated-image file write is exposed
through `sak_apfs_writer_cli.exe`; raw file write remains certification-lane
only and is not exposed as a user mutation action.
APFS image-offset proof also passed on 2026-06-06 UTC. The probe certifier
detected a sane APFS container inside `temp/Sonoma 14.dmg` at offset
`1233310720` and wrote
`artifacts/partition-manager-certification/tool-tests/sonoma-apfs-offset-probe.json`.
The Tahoe `SharedSupport.dmg` `NXSB` string hits were not accepted as direct
APFS proof because no 4096-byte-aligned superblock hit with sane block geometry
was found.
The Partition Manager panel also gates those unsupported paths in the UI:
Windows-native Check File System, resize, cluster-size change, and label-change
actions are disabled with explicit reasons on non-native HFS+/HFSX/APFS/XFS/Btrfs
targets, while the supported read-only Inspect/Browse/Check Non-Windows actions
remain enabled where their capability is implemented.

Physical destructive cross-filesystem proof also passed on 2026-06-05 PDT /
2026-06-06 UTC on the same expendable external USB NVMe disk after the read-only
Apple evidence was captured. The UAC launcher uses Windows `runas` auth instead
of keypress automation; the elevated runner requires `-Force`, rejects
boot/system disks, defaults to USB-only media, and pins large disks by serial or
friendly-name guard. The run formatted ext2, ext3, and ext4 on app-style
`GLOBALROOT` raw partition targets, ran clean `e2fsck -p` and read-only checks,
grew and shrank each file system with `resize2fs`, verified S.A.K. raw probes
after format/grow/shrink, wrote original SWAPSPACE2 Linux swap metadata,
verified the raw header, detected Linux swap with the probe certifier, and
cleared the external disk back to RAW. Evidence:
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.cross-filesystem-physical-destructive/report.json`.

## Goal

Add portable read, format, and repair support for common Linux file systems and HFS+ inside Partition Manager without requiring runtime installs, drivers, WSL, package managers, or network downloads.

All capabilities must be wired through the existing Partition Manager panel, Pending Operations queue, Dry Run, final Apply review, elevated helper, operation reports, safety validators, and certification harness. Tools must be original S.A.K. code or bundled open-source third-party binaries/libraries with pinned source, license notices, hashes, and reproducible-build notes.

## Selected Tool Stack

The app is AGPL/open source and the distribution is not being closed, so
reciprocal licenses are acceptable when source, notices, and license terms are
shipped correctly. Portability is still the hard constraint: the Windows app
must not require users to install drivers, WSL, package managers, or runtime
dependencies.

Chosen order:

1. ext2/ext3/ext4 first, using e2fsprogs as external bundled tools under
   `tools/filesystem/e2fsprogs/`. `e2fsck -n -f` read-only checks are the first
   user-visible runtime operation; original S.A.K. parser code provides
   read-only directory listing, selected-file extraction, symlink target
   display, and bounded recursive directory export without mounting. Symlink
   exports create target-text sidecars instead of creating Windows symlinks.
   `mke2fs`, confirmed `e2fsck -p`, and `resize2fs` are now wired for confirmed
   ext format, repair, grow, and shrink through the normal queue/apply path.
2. HFS+/HFSX read-only inspect/extract uses original S.A.K. parser code for
   volume headers, HFS wrappers, catalog B-tree leaf records, initial extents,
   file data/resource forks, and extents-overflow records for catalog/data/resource
   forks. The Check action runs an original read-only catalog/header
   consistency report with attributes B-tree key scanning and defined
   fork/inline metadata reporting. `newfs_hfs` and `fsck_hfs` from a
   source-pinned hfsprogs 540.1.linux3-6+sak-msys bundle now provide confirmed
   sparse-staged HFS+/HFSX format and repair through the same
   queue/apply path. The HFS File action stages selected raw partitions for
   bounded existing data-fork, resource-fork, truncate, inline/fork-backed-attribute
   mutation including bounded fork-backed-attribute growth, bounded file create, and constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release.
   Unbounded folder-tree delete and complex file delete, B-tree split/rebalance,
   inline/broad attribute growth, compressed-file writes, broad allocation growth beyond the bounded initial-extent slice,
   and APFS-style behavior remain out of scope.
3. XFS and Btrfs now have original read-only superblock metadata parsing plus
   in-app metadata consistency checks with lightweight counter/geometry sanity
   notes. This is not a substitute for deep
   `xfs_repair -n` or `btrfs check --readonly`. Deep checker and write workflows remain blocked until native
   Windows-portable command-line tool builds are proven. VM/live-ISO testing is
   valid for certification and validation, but it is not acceptable as an app
   runtime dependency.
4. Btrfs repair remains disabled by default. The official tooling warns against
   casual repair use, so S.A.K. will expose only read-only checks until a narrow
   certified repair scenario exists.
5. APFS has read-only container superblock, checkpoint ring, object-map OID,
   volume-OID-slot metadata, bounded probe-window volume-superblock candidate
   metadata, referenced-object-header preflight metadata, visible object-map
   tree-anchor plus root/non-root B-tree node-header and root `btree_info_t`
   metadata, and certified read-only volume browse, selected-file extraction,
   and bounded recursive directory export through object-map traversal.
   Encrypted/compressed APFS file reads, repair, format, resize, and write
   remain blocked for this milestone.

Bundle path rules:

- Source-controlled bundle root: `tools/filesystem/`.
- Runtime manifest: `tools/filesystem/manifest.json`.
- Per-tool layout: `tools/filesystem/<tool-id>/...`.
- Build output: normal CMake `tools/` copy includes the filesystem bundle.
- Release gates: CMake requires the manifest; portable staging and smoke checks
  require the manifest in packages; `scripts/check_partition_filesystem_tool_manifest.ps1`
  validates metadata, primary binary hashes, `runtime_files` companion hashes,
  and rejects unmanifested files; `scripts/check_third_party_licenses.ps1`
  requires any approved manifest tool to appear in `THIRD_PARTY_LICENSES.md`.
- Destructive ext proof gate: run
  `scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1` from the
  `SAK-PM-Lab-Win11` VM desktop after confirming the selected disk is disposable.
  The elevated runner verifies e2fsprogs hashes, formats ext4 on a raw partition
  target, runs `e2fsck -p`, runs a read-only check, grows the partition,
  executes `resize2fs`, shrinks the filesystem before shrinking the partition,
  rechecks the filesystem, clears the disk, and writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext-filesystem-write/report.json`.
  Latest status: passed on 2026-06-05 for ext4 format, repair, read-only
  checks, adjacent grow, shrink, `resize2fs`, and cleanup on disposable VM media.
  Preferred unattended reruns use
  `scripts/launch_partition_manager_vm_gate_host.ps1 -Gate ext-filesystem` with
  the dedicated direct-admin automation VM and a real guest password file.
- Linux compatibility proof: run
  `scripts/run_partition_manager_ext_linux_validation.ps1 -DistroName archlinux -FileSystem ext2`,
  `-FileSystem ext3`, and `-FileSystem ext4`.
  The harness creates a disposable ext image with the Windows bundle, grows and shrinks it,
  validates it with Linux `e2fsck`/`dumpe2fs`, loop-mounts it read/write, writes
  a fixture file, rechecks with Linux, rechecks with bundled Windows `e2fsck`,
  deletes the image by default, and writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext2-linux-validation/report.json`,
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext3-linux-validation/report.json`,
  and
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext4-linux-validation/report.json`.
- Linux-created XFS/Btrfs metadata proof: build
  `partition_filesystem_probe_certifier`, then run
  `scripts/run_partition_manager_linux_metadata_validation.ps1 -DistroName archlinux`.
  The harness creates disposable Linux-formatted XFS and Btrfs images with
  `mkfs.xfs` and `mkfs.btrfs`, runs the S.A.K. read-only detector certifier
  with expected file-system and sanity requirements, deletes the images by
  default, and writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.linux-metadata-validation/report.json`.
  Latest status: passed on 2026-06-05 for XFS and Btrfs metadata detection
  against Linux-created images. The UI now exposes an original read-only
  metadata consistency check from captured probe data. Deep `xfs_repair` /
  `btrfs check`, format, and repair stay blocked until Windows-portable tools
  are approved.
- Offset raw-probe helper proof: build
  `partition_filesystem_probe_certifier`, then run
  `scripts/test_partition_filesystem_probe_certifier.ps1` or the CTest
  `test_partition_filesystem_probe_certifier`. The script generates a tiny ext2
  fixture with 4 KiB of prefix padding, verifies `--input-offset-bytes`, checks
  deterministic report fields, and confirms invalid offset input fails.
  Release readiness runs the same script when certification-tool self-tests are
  enabled.
- Physical Apple filesystem proof: connect a known disposable or expendable
  Mac-formatted disk, then run
  `scripts/run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber <N>`.
  The script is read-only, blocks boot/system disks unless explicitly overridden,
  probes `GLOBALROOT` partition aliases, and requires one HFS+ and one APFS
  partition to pass. It always records HFS+ and APFS browse proof and can require
  selected file-read proof, HFS+ selected-attribute proof, and APFS bounded
  export proof with
  `-RequireHfsFileProof -RequireHfsAttributeProof -RequireApfsFileProof -RequireApfsExportProof`;
  the current physical proof read `/Fonts/00TT.TTF` from APFS, exported `/Fonts`
  from APFS with 63 files, 1 directory, 64 scanned entries, and 21161830 bytes,
  read the HFS+ zip fixture, read inline HFS+ attribute `com.apple.decmpfs`,
  and recorded HFS+ metadata/free-byte proof. It writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.apple-filesystem-physical/report.json`.
- HFS+/HFSX image tool proof: build `partition_filesystem_probe_certifier`, then
  run `scripts/run_partition_manager_hfsprogs_image_validation.ps1`. The
  harness creates disposable HFS+ and HFSX images with bundled `newfs_hfs`,
  runs `fsck_hfs`, verifies S.A.K. raw detection and HFS consistency reporting,
  records tool versions/hashes, deletes the images by default, and writes
  `artifacts/partition-manager-certification/tool-tests/hfsprogs-image-validation/report.json`.
- Physical HFS+/HFSX destructive tool proof: connect an expendable external
  disk with an HFS partition, then run
  `scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1`
  with `-DiskNumber`, `-PartitionNumber` when needed,
  `-ExpectedSerialNumber` or `-ExpectedFriendlyNamePattern`, and `-Force`.
  The elevated runner formats sparse HFS images as HFS+, HFSX, then HFS+ again
  by default, runs `fsck_hfs` repair/final read-only checks on those images,
  copies allocated metadata ranges to the selected raw partition, verifies S.A.K.
  raw detection with the probe certifier after each case, and
  writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfsprogs-physical-destructive/report.json`.
- Physical destructive cross-filesystem proof: connect an expendable external
  disk, then run
  `scripts/launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1`
  with `-DiskNumber`, `-ExpectedSerialNumber` or
  `-ExpectedFriendlyNamePattern`, and `-Force`. The launcher uses UAC `runas`;
  the elevated runner wipes only the pinned non-boot/non-system disk, validates
  ext2/ext3/ext4 format/repair/grow/shrink plus Linux swap raw header detection,
  writes JSON evidence, and clears the disk back to RAW by default. It writes
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.cross-filesystem-physical-destructive/report.json`.

Rejected or deferred source paths:

- Cygwin publishes an e2fsprogs package with Windows executables, but the current
  x86_64 setup index lists e2fsprogs 1.44.5-1 under `System Unmaintained` with
  Cygwin runtime/library dependencies. That is too stale for production ext4
  write/check support because modern ext4 features can outpace old `e2fsck`.
- MSYS2 package search did not show a native e2fsprogs package. Do not assume a
  MinGW/UCRT e2fsprogs binary exists without current package proof.
- WSL, Linux ISOs, and VMs are valid for building/certification/lab validation
  only. They cannot become runtime prerequisites for the portable Windows app.

## Immediate Inventory Fix

Current Windows inventory can miss file-system labels for hidden/protected partitions because `Get-Partition` and `Get-Volume` expose partition and volume data separately. The inventory path now uses a resilient volume lookup helper and parser fallbacks:

- Try direct partition-to-volume lookup.
- Try drive-letter lookup when present.
- Try volume access paths when Windows exposes them.
- If Windows still exposes no volume object, infer protected partition labels only where the partition type is deterministic:
  - EFI System Partition: `FAT32`
  - Microsoft Reserved Partition: `Other`
- Keep used/free bytes as unknown (`0`) for inferred labels, so the UI does not fake capacity usage.
- If Windows still has no file-system label and raw device read access is available, run a small read-only signature probe for NTFS, exFAT, FAT12/16/32, ext2/ext3/ext4, XFS, Btrfs, Linux swap, direct or HFS-wrapper HFS+/HFSX, and APFS container signatures, then attach parsed metadata where the parser supports it, including Linux swap version/page-size/UUID/label metadata, APFS checkpoint ring metadata, bounded APFS volume-superblock candidate metadata, referenced APFS object headers found inside the probe window, visible APFS object-map tree anchors, and visible APFS root/non-root B-tree node headers plus root `btree_info_t` records for referenced objects.
- If the normal process cannot open or seek the raw disk probe, retry the same
  capped read through the elevated helper's allowlisted `ReadPartitionProbe`
  task. The helper accepts only `\\.\PhysicalDriveN`, reads at most the detector
  probe window, returns base64 probe bytes, and does not expose generic shell or
  write capability.
- Store the detection source (`WindowsVolume`, `InferredProtected`, or `RawSignature`) and safe raw metadata details so the File System table cell tooltip and Properties dialog can show how the label was determined and what the detector found.

## Implemented Milestone A Pieces

- Added `PartitionFileSystemDetector` for fixed-offset, read-only signature checks.
- Added `PartitionFileSystemRegistry` so the UI can state whether a file system has Windows-native support, detection-only support, or blocked future manifest/certification work.
- Added inventory enrichment for partitions with no Windows/protected file-system label. It reads only a small probe window from the physical device and silently skips detection if raw read access is unavailable.
- Added detector fixture tests for NTFS, exFAT, FAT12/16/32, ext2, ext3, ext4, XFS, Btrfs, HFS+, HFSX, APFS, Linux swap, and unknown data, including metadata fields for ext, Linux swap, XFS, Btrfs, APFS, and HFS+. XFS/Btrfs/APFS fixtures assert lightweight metadata sanity evidence lines, and APFS fixtures assert checkpoint ring metadata, bounded volume-superblock candidate metadata, referenced-object-header preflight metadata, visible object-map tree-anchor metadata, visible root/non-root B-tree node-header metadata, and root `btree_info_t` metadata.
- Added HFS+/HFSX volume-header metadata parsing for direct HFS+ volumes and
  classic HFS wrapper-embedded HFS+ volumes:
  version, journal flag, file count, folder count, block size, total blocks,
  free blocks, and total/free byte counts when the header's block math is valid.
- Added ext2/ext3/ext4 superblock metadata parsing for raw-detected volumes:
  block size, total blocks, free blocks, total/free byte counts, inode counts,
  block/inode group sizing, journal flag, feature flags, and volume label when present.
- Persisted raw detector metadata into `PartitionVolumeInfo` so the File System
  table tooltip and Properties dialog expose the same evidence to technicians.
- Added detection-source display in the partition table tooltip and Properties dialog.
- Added current file-system support status, supported S.A.K. actions, blocked actions, and required bundled tools to the Properties dialog.
- Added an enabled "Inspect Non-Windows File System" action for partitions with
  captured read-only raw metadata. It opens the same copyable Properties-style
  metadata table without mounting, checking, repairing, or writing the file
  system.
- Added a file-system-aware "Check Non-Windows File System" action in the sidebar and partition context menu. It runs manifest-approved ext read-only checks, original XFS/Btrfs/APFS metadata consistency reports, original HFS+/HFSX catalog plus attributes-key consistency reports, and manifest-approved HFS+/HFSX `fsck_hfs` read-only checks where supported; unsupported selections stay disabled with the current blocker and required bundled tools.
- Added `scripts/check_partition_filesystem_tool_manifest.ps1` to release readiness so future bundled filesystem tools cannot ship without complete metadata, safe relative paths, existing binaries, and matching SHA-256 hashes.
- Added `PartitionFileSystemToolManifest` and `tools/filesystem/manifest.json` so future non-native format/repair tools must have id, version, upstream URL, license, source archive SHA-256, relative binary path, binary SHA-256, supported file systems, and supported operations before a write-capable workflow can approve them.
- Added `PartitionFileSystemToolRunner` command plumbing for manifest-approved read-only checks:
  - ext2/ext3/ext4: `e2fsck -n -f <target>`
  - XFS: `xfs_repair -n <target>`
  - Btrfs: `btrfs check --readonly <target>`
- Added manifest-approved ext2/ext3/ext4 command builders for confirmed
  e2fsprogs write-capable command shapes:
  - format: `mke2fs -t <ext2|ext3|ext4> -F [-L <label>] <target>`
  - repair: `e2fsck -p -f <target>`
  - resize: `resize2fs <target>`
- Bundled and manifest-approved e2fsprogs `e2fsck`, `mke2fs`, and `resize2fs`
  1.47.4 for ext2/ext3/ext4 command generation, including the upstream source
  archive, local MinGW patch, upstream notice, build notes, binary hashes,
  source hash, and runtime companion hashes.
- Wired the sidebar and partition context-menu "Check Non-Windows File System" action so ext and HFS+/HFSX tool checks enable only when the selected filesystem, target path, manifest entry, binary hash, operation, and filesystem approval all resolve cleanly. HFS+/HFSX now exposes original catalog/attributes check, read-only `fsck_hfs`, and confirmed queued `fsck_hfs` repair in one mode dialog. Original-code XFS/Btrfs/APFS metadata reports and HFS+ catalog plus attributes-key consistency reports run without external tools.
- Added `PartitionExtFileSystemReader` for original read-only ext2/ext3/ext4
  directory listing, selected-file extraction, symlink target display, and
  bounded recursive directory export from direct, indirect, and extent-mapped
  file data. Symlink export writes target text as `.symlink.txt` sidecars and
  does not follow or create links. It blocks traversal paths, oversized reads,
  encrypted/inline-data, compressed, bigalloc, and external-journal volumes
  instead of guessing.
- Wired the sidebar and partition context-menu "Browse Non-Windows File
  System" action for ext2/ext3/ext4. It prompts for the raw target and ext path,
  then opens a copyable directory listing table with selected-file extraction,
  symlink target display, and bounded directory export without mounting or
  writing source volumes.
- Added `PartitionHfsFileSystemReader` for original read-only HFS+/HFSX
  catalog consistency checks, attributes B-tree key scans with defined
  fork/inline metadata reporting, catalog browsing, selected
  data-fork/resource-fork extraction, selected attribute-value extraction, and bounded directory export. It parses direct and
  classic-wrapper HFS+ volume headers, catalog B-tree headers, leaf records,
  folder/file records, UTF-16 names, initial file data-fork extents, and
  extents-overflow records for catalog/data/resource/attribute fork reads. It blocks traversal
  paths, oversized reads, inline/broad attribute growth, recursive
  extents-overflow-file overflow, writes, repair, format, and journal replay.
- Added `PartitionHfsFileSystemWriter` for the first original HFS+ arbitrary
  write slice: image-only data-fork overwrite plus data/resource-fork
  allocated-block grow/shrink replacement and explicit zero-length truncate with explicit writer
  enablement, target confirmation, certification evidence ID, journal override,
  compressed-file blocking, existing-allocation enforcement, catalog logical-size
  update, stale-tail zeroing, and read-back SHA-256 verification.
- Added HFS File Apply wiring for selected HFS+/HFSX raw partitions. The script
  stages the selected raw partition to a sparse image, runs
  `sak_hfs_writer_cli.exe`, repairs/checks the staged image with bundled
  `fsck_hfs`, copies changed sparse ranges back to the exact same raw partition
  alias, refreshes storage, and keeps the operation inside the normal
  queue/review/report flow.
- Wired the same "Browse Non-Windows File System" sidebar/context action for
  HFS+/HFSX, with a copyable directory listing table, selected-file extract,
  resource-fork extract, and bounded directory export that reads the source
  volume read-only.

Still open:

- Raw signature detection itself does not mount, repair, browse, or write any non-Windows file system.
- ext2/ext3/ext4 family detection is feature-based and conservative; exact old ext variants can still need tool probing before any write-capable action.
- ext browse currently lists directories, extracts selected regular files,
  displays symlink targets, and exports directories recursively through the core
  parser with entry, per-file, and total-byte caps. Symlinks export as sidecar
  text files so the app does not create links or follow possible traversal
  targets.
- HFS+ browse currently lists catalog folders/files and extracts selected data
  forks and resource forks covered by initial extents or HFS+ extents-overflow
  records. It also exports directories recursively, writes resource forks as
  `.rsrc` sidecars, and can run an original read-only catalog/header
  consistency report with attributes B-tree key scanning plus defined
  fork/inline metadata reporting. It can extract selected inline and fork-backed
  attribute values by file ID and name with read caps. It does not mount volumes,
  replay journals, perform repair-grade consistency validation, write
  inline/broad attribute growth requiring record expansion or overflow records, delete unbounded folder trees or complex files,
  split/rebalance catalog B-trees, grow allocation, write
  compressed-file data, or follow recursive overflow from the extents-overflow
  file itself. Bounded data-fork overwrite,
  data/resource-fork allocated-block replacement/truncate, bounded initial-extent allocation growth, inline-attribute
  replacement, fork-backed attribute replacement/growth, bounded file create, single-leaf catalog rename/move, plus constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release are implemented
  as core writer/helper paths and wired as staged raw Apply actions for selected
  HFS+/HFSX partitions.
- Linux/HFS+/APFS format/repair actions remain hidden or blocked unless manifest hashes or original-code gates match, license/source records pass review, safety rules are wired, and VM/lab, image, or hardware proof exists. ext2/ext3/ext4 format, repair, grow, and shrink are wired through the queue/apply path with confirmation and passed VM raw-media proof; Linux swap format is original-code and VM-proven; HFS+/HFSX sparse-staged format/repair is wired through bundled hfsprogs with image and physical proof; HFS+ image overwrite, data/resource-fork allocated-block grow/shrink replacement, bounded initial-extent allocation growth, explicit zero-length truncate, inline-attribute replacement, fork-backed attribute replacement within existing allocated blocks, bounded fork-backed attribute allocation growth, constrained empty-file/empty-folder create/delete, single-leaf catalog rename/move, bounded file create with data, allocated-file delete, optional released-block zeroing, and bounded folder-tree delete are core-tested, exposed through `sak_hfs_writer_cli.exe`, and wired through staged raw Apply for selected HFS+/HFSX partitions; generated APFS create/format, generated-layout checksum repair, generated-layout root-file write/patch/delete, generated-layout empty root-directory create/delete, generated-layout root-directory child-file write/patch/delete, and generated-layout volume-label change are wired through `sak_apfs_writer_cli.exe` with explicit confirmation and generated-layout guards, while generated-image root-file create/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change are helper-exposed with read-back, directory-empty, old/new-label, or negative-read-back hashes; XFS/Btrfs/APFS arbitrary Apple-media writes plus unbounded HFS+ folder-tree delete, complex HFS+ file delete, B-tree split/rebalance, broad allocation growth beyond the bounded initial-extent slice, compressed-file writes, and inline/broad attribute growth remain blocked pending operation-specific certification or tool proof.
- HFS+/HFSX APFS-style browsing is not planned. Current HFS+/HFSX support is
  raw signature detection, volume-header metadata, catalog consistency checks,
  attributes B-tree key scans with defined fork/inline metadata reporting,
  catalog listing, selected data/resource-fork extraction, selected
  attribute-value extraction, and extents-overflow resolution only.

## Research Snapshot

| Area | Finding | Candidate |
|---|---|---|
| Windows inventory | `Get-Partition` returns partition objects; `Get-Volume` returns volume objects and has a `-Partition` parameter. Dynamic volume support is limited to repair, optimize, and format cmdlets. | Keep Windows Storage cmdlets for native NTFS/FAT/exFAT inventory, then use raw signature fallback for native and non-Windows labels when Windows does not return a volume. |
| FAT/exFAT | Windows has native format/repair paths; dosfstools provides `mkfs.fat`, `fsck.fat`, `fatlabel` under GPL-3.0; exfatprogs provides standard exFAT create/fix/debug utilities under GPL-2.0. | Prefer native Windows for current FAT/exFAT. Bundle tools only for offline image/VHD parity if needed. |
| ext2/ext3/ext4 | e2fsprogs provides ext2/3/4 utilities, including create/check/manipulation tools. | Best first Linux target: `mke2fs`, `e2fsck`, `resize2fs`, `dumpe2fs` as bundled external tools under `tools/filesystem/e2fsprogs/`, not linked into app code. |
| XFS | xfsprogs provides XFS management tools including `mkfs.xfs` and `xfs_repair`; `xfs_repair -n` is check-only/no-modify but has known limitations. The XFS primary superblock exposes block size, data/free block counters, allocation-group fields, inode counters, UUID, and file-system name. | Current parser exposes read-only superblock metadata. Defer runtime check/format/repair until a Windows-portable build is proven. |
| Btrfs | btrfs-progs feature support tracks kernel/progs versions and includes creation/check tooling; `btrfs check --readonly` is the safe default, while repair is explicitly dangerous. The Btrfs primary superblock at 64 KiB exposes total/used bytes, devices, sector/node/leaf sizes, flags, UUID, generation, and label. | Current parser exposes read-only superblock metadata. Defer runtime check/format/repair until a Windows-portable build is proven. Read-only check only; repair disabled by default. |
| HFS+ | hfsutils is classic HFS only and explicitly does not support HFS+. libfshfs supports read-only HFS+ / HFSX 10.3+ and is LGPL-3.0-or-later, but is experimental and does not support every compression variant. Apple TN1150 defines the HFS+ volume header at byte 1024, the HFS wrapper embedded-volume fields, the catalog as a B-tree, the file record `dataFork` and `resourceFork`, the extents overflow file as a fixed-key B-tree keyed by file ID, fork type, and fork start block, and an optional attributes B-tree whose key layout was never fully finalized. | Current path uses original S.A.K. header, wrapper, catalog consistency, attributes key/value scanning, initial extent, extents-overflow parsing for catalog/data/resource/attribute reads, existing-fork bounded writes, bounded file create with data, single-leaf catalog rename/move, and constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release. Bundled hfsprogs handles format and fsck-style check/repair; evaluate libfshfs-style work only for corruption-tolerant recovery, broader HFS+ variants, or future expanded file-write proof. |
| HFS+ format/repair | hfsprogs/newfs_hfs/fsck_hfs exist from Apple-derived sources in Debian hfsprogs 540.1.linux3-6. | Current bundle builds `newfs_hfs` and `fsck_hfs` for Windows/MSYS, ships source/patch/runtime notices, validates image-level HFS+/HFSX output with the S.A.K. probe certifier, and routes raw app operations through sparse staging plus manifest/hash/safety gates. Physical raw-partition proof passed on an expendable USB NVMe HFS partition. |
| APFS | Apple's reference defines the NXSB container superblock fields for block size, block count, feature flags, UUID, next object ID, next transaction ID, checkpoint block counts, checkpoint bases/next/start/lengths, object-map OID, and file-system OID array. It also defines the shared object header and Fletcher-64 object checksum, object-map fields for flags/snapshot counts/tree OIDs/revert XIDs, APSB volume superblock fields, B-tree node header fields, root `btree_info_t`, directory records, inodes, file extent records, and spaceman chunk/CIB geometry. libfsapfs is experimental, read-only, LGPL-3.0-or-later, and does not support several modern APFS features. | Current parser exposes read-only container/checkpoint ring/object-map/volume-OID metadata, bounded probe-window volume-superblock candidate metadata, referenced-object-header preflight, visible object-map tree anchors, visible referenced root/non-root B-tree node headers, root `btree_info_t`, metadata object-checksum validation on APFS object blocks, root/directory browsing, and selected-file extraction for normal unencrypted/uncompressed file extents. Current writer supports generated/minimal APFS image create/format, generated-image root-file create/replace/byte-range patch/delete, generated-image empty root-directory create/delete, generated-image root-directory child-file write/patch/delete, generated-image volume-label change, generated/minimal one-spaceman-chunk raw root-file write/patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change through Apply when generated-layout proof is captured, and checksum repair; generated raw targets larger than 128 MiB and arbitrary Apple APFS write support remain blocked. |

## Supported Formats Roadmap

| Format | Detect | Read/list/extract | Format | Repair | Notes |
|---|---:|---:|---:|---:|---|
| FAT12/16/32 | Current boot-sector detection | Native Windows | Native Windows | Native Windows / optional dosfstools | Hidden/unmounted labels can be detected without mounting when normal or elevated read-only raw probe access is available. |
| exFAT | Current boot-sector detection | Native Windows | Native Windows / optional exfatprogs | Native Windows / optional exfatprogs | Keep native first. |
| NTFS | Current boot-sector detection | Native Windows | Native Windows | Native Windows `Repair-Volume` | Existing feature path remains primary. |
| ext2/ext3/ext4 | Current | Current metadata, read-only directory listing, selected-file extraction, symlink target display/sidecar export, bounded recursive directory export, and read-only check | Current confirmed queue/apply path | Current confirmed queue/apply path | Original parser lists/extracts files, displays symlink targets without following them, and exports folders; `e2fsck -n -f` is user-visible; `mke2fs` formats, `e2fsck -p` repairs, and `resize2fs` grows or shrinks ext file systems after confirmation and usage checks. |
| XFS | Current | Current read-only superblock metadata and original metadata consistency check | Blocked by design | Blocked by design | Shipped scope is read-only metadata only. Deep `xfs_repair -n`, format, and repair require a Windows-portable approved tool plus destructive proof before exposure. |
| Btrfs | Current | Current read-only superblock metadata and original metadata consistency check | Blocked by design | Blocked by design | Shipped scope is read-only metadata only. Deep `btrfs check --readonly` requires a Windows-portable approved tool; repair stays disabled because upstream tooling treats repair as expert-only and high risk. |
| Linux swap | Current | Current read-only header metadata | Current confirmed queue/apply path | N/A | Original SWAPSPACE2 v1 formatter writes version, last-page, UUID, label, and page-size-bound signature after explicit confirmation; repair is not applicable. |
| HFS+ / HFSX | Current direct and wrapper detection | Current header metadata, catalog consistency checks, attributes B-tree key scans with fork/inline metadata reporting, catalog listing, selected data/resource-fork extraction, selected attribute-value extraction, bounded recursive directory export with `.rsrc` sidecars, catalog/data/resource/attribute extents-overflow support, image-only same-size data-fork overwrite, image-only data/resource-fork allocated-block grow/shrink/truncate replacement, image-only bounded initial-extent allocation-growth replacement, image-only inline-attribute replacement, image-only fork-backed attribute replacement within existing allocated blocks, image-only bounded fork-backed attribute allocation growth, image-only bounded file create with data, image-only single-leaf catalog rename/move, and constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release core/helper support | Current confirmed queue/apply path | Current confirmed queue/apply path | `newfs_hfs` formats and `fsck_hfs` repairs through sparse staging for raw partition targets after confirmation. Existing-file data-fork overwrite, data/resource-fork replacement/truncate, bounded initial-extent allocation growth, inline-attribute replacement, fork-backed attribute replacement within existing allocated blocks, bounded fork-backed attribute allocation growth, bounded file create with data, single-leaf catalog rename/move, empty-file/folder create/delete, allocated-file delete, optional released-block zeroing, and bounded folder-tree delete are exposed through `sak_hfs_writer_cli.exe` and wired to staged raw Apply for selected HFS+/HFSX partitions. Arbitrary-depth catalog B-tree split, rebalance, and node-pool growth (depth-4 / >256-leaf certified through the production bridge with Apple `fsck_hfs` + kernel RW mount) are now confirmed. Inline/broad attribute growth, compressed-file writes, unbounded folder-tree delete, complex file delete, attribute-tree B-tree split, broad allocation growth beyond the bounded initial-extent slice, recursive extents-overflow-file overflow, and APFS-style behavior remain blocked. |
| APFS | Current | Current read-only container/checkpoint ring/object-map/volume-OID metadata, bounded probe-window volume-superblock candidate metadata, referenced-object-header preflight, visible object-map tree-anchor metadata, visible referenced root/non-root B-tree node headers, root `btree_info_t`, metadata object-checksum validation on APFS object blocks, directory browse, selected-file extraction, bounded recursive directory export, fail-closed writer certification preflight, operation-specific mutation plans, generated APFS format scratch-image build/verify support with optional multi-block seed-file read-back for certification, image-only root-file add/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change proof plus one-spaceman-chunk raw-target generated root-file add/replace/byte-range patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change Apply plumbing for generated/minimal APFS with bounded existing root-file/directory preservation, deleted data-region zeroing, helper-level payload/read-back/deleted-file/directory-empty/old-new-label/negative-read-back hashes, non-empty directory delete blocking, and supported-object-checksum repair proof for generated/minimal APFS | Current one-spaceman-chunk (64-128 MiB) generated/minimal APFS create and format plus generated-layout root-file write/patch/delete, empty root-directory create/delete, root-directory child-file write/patch/delete, and volume-label change through queue/apply via `sak_apfs_writer_cli.exe` | Current one-spaceman-chunk generated/minimal APFS checksum repair through queue/apply; arbitrary APFS repair blocked | Encrypted/compressed files, arbitrary Apple APFS writes, non-generated file/folder writes, generated raw targets larger than 128 MiB until multi-CIB spaceman proof, resize, and broad hardware APFS mutation remain blocked until operation-specific proof exists. |

## Architecture

### 1. File-System Capability Registry

`PartitionFileSystemRegistry` tracks:

- `id`: `ext4`, `xfs`, `btrfs`, `hfsplus`, etc.
- Display name and aliases.
- Signature rules: offsets, magic bytes, UUID/GPT hints.
- Operations: detect, inspect, extract, format, check, repair, label, resize.
- Required tool IDs and minimum tool versions.
- Risk level and required confirmations.

### 2. Raw Signature Detector

Add read-only `PartitionFileSystemDetector`:

- Reads only small fixed offsets from `\\.\PhysicalDriveN` at partition offset.
- Uses elevated helper only when raw access fails without elevation.
- Never mounts or writes.
- Detects:
  - NTFS, exFAT, FAT12/16/32 boot sectors after validating the `55 AA` boot-sector signature.
  - ext superblock at partition offset + 1024 bytes.
  - XFS magic and primary superblock metadata at start, including block size,
    data/free block counters, allocation-group fields, inode counters, UUID,
    version flags, features2, and file-system name.
  - Btrfs superblock at 64 KiB, including total/used bytes, devices,
    sector/node/leaf sizes, compat/ro/incompat flags, UUID, generation, and
    label.
  - Direct HFS+/HFSX volume header at 1024 bytes.
  - Classic HFS wrapper-embedded HFS+/HFSX using the wrapper MDB at 1024 bytes,
    embedded signature at `0x7C`, embedded extent at `0x7E`, allocation block
    size, and allocation-block start.
  - HFS+/HFSX version, journal flag, file/folder counts, and total/free block
    math when sane.
  - ext2/ext3/ext4 superblock metadata, including total/free block math,
    inode counts, feature flags, journal flag, and label.
  - APFS container superblock at start, including block size, block count,
    feature flags, UUID, next object/transaction IDs, checkpoint block
    counts/bases/next/start/lengths, space-manager/object-map/reaper OIDs, maximum
    file-system count, and populated volume-OID slots.
  - Linux swap signature near page end plus modern header version, page size,
    last page, bad-page count, UUID, and label when present.
  - The current probe window is 2 MiB so common HFS wrapper offsets can be
    detected without mounting or scanning whole devices.
- Stores detection source: `WindowsVolume`, `PartitionType`, `RawSignature`, `ToolProbe`, or `InferredProtected`.

### 3. Bundled Tool Runtime

Use the runtime `tools/filesystem/manifest.json` file. The source-controlled
manifest exists today, CMake fails if it is missing, and it currently approves
e2fsprogs 1.47.4 tool entries for ext2/ext3/ext4 `check-read-only`, confirmed
`repair`, confirmed `format`, and confirmed `resize`, plus hfsprogs
540.1.linux3-6+sak-msys tool entries for HFS+/HFSX confirmed `format`,
`check-read-only`, and confirmed `repair`. Future approved tools or operations
must be added before any additional non-native write/repair workflow is exposed:

- Tool name, version, upstream URL, license, source archive hash.
- Primary binary hash, companion `runtime_files` hashes, and build recipe.
- Supported operations and file systems.
- Whether tool is executable-only or linked library.

Rules:

- No runtime download.
- No installer.
- No service/driver install.
- Tools execute from app-managed portable tool directory.
- Extra shipped files under `tools/filesystem/` are release-blocking unless
  listed in a tool's `runtime_files` array, except top-level `manifest.json` and
  `README.md`. Top-level `_build/` is scratch-only and is removed from build and
  staged release output.
- GPLv2-only tools stay external processes unless legal review approves another integration model.
- `THIRD_PARTY_LICENSES.md` must name every manifest-approved tool before release.
- Every tool invocation must be logged in the operation report with args redacted where needed.

### 4. Operation Runner

`PartitionFileSystemToolRunner` now has gated command paths:

- Resolves tool path from manifest.
- Validates operation and file-system capabilities.
- Builds read-only check commands for ext2/ext3/ext4 and image-target HFS+/HFSX.
- Builds confirmed ext2/ext3/ext4 command shapes for `mke2fs`, `e2fsck -p`, and
  `resize2fs`, plus HFS+/HFSX command shapes for `newfs_hfs` and `fsck_hfs`.
- Runs via the centralized process runner and captures stdout/stderr/exit code.
- Blocks operations if tool hash, operation approval, file-system approval, or
  license manifest coverage is missing.

Current ext and HFS+/HFSX write execution:

- Runs through the existing elevated PowerShell apply path.
- Revalidates the bundled tool hash inside the elevated script before launch.
- Revalidates disk/partition identity and dismounts mounted volumes before tool
  execution where Windows exposes a volume.
- Captures tool stdout/stderr in `PartitionExecutionStep` output.
- Blocks unsupported file systems, missing or mismatched selected raw partition
  targets, missing confirmation, protected/read-only/dynamic targets, stale
  layout hashes, and ext shrink without detected usage metadata. HFS+/HFSX
  format and repair never expose file/folder writes or resize.

### 5. UI Wiring

Partition Manager panel additions:

- File System column gets detection-source and S.A.K. support tooltips.
- Properties dialog adds:
  - Detected file system
  - Detection source
  - Current S.A.K. support level
  - Supported S.A.K. actions
  - Blocked filesystem actions
  - File-system metadata from read-only raw detector probes when present
- Required bundled tool
- Right-click and sidebar actions become file-system-aware:
  - `Inspect Non-Windows File System` enables for non-native detected
    partitions when read-only detector metadata exists.
  - `Browse Non-Windows File System` enables for ext2/ext3/ext4, HFS+/HFSX, and APFS
    and opens a read-only directory listing plus selected-file extraction,
    HFS+ selected attribute-value extraction, symlink target display for ext,
    bounded recursive ext/HFS+/APFS directory export, and original S.A.K. parser code.
  - `Check Non-Windows File System` runs ext2/ext3/ext4 checks only when the
    selected target path and manifest-approved `e2fsck` read-only checker
    resolve cleanly; it can run a read-only check immediately or queue confirmed
    ext repair for Apply. For HFS+/HFSX it exposes original catalog/attributes
    check, read-only `fsck_hfs -n -f`, and confirmed sparse-staged
    `fsck_hfs -p -f` repair when the selected raw target path and
    manifest-approved tool resolve cleanly. The same action also enables original-code
    XFS/Btrfs/APFS metadata consistency reports where raw probe evidence supports them.
  - `Format Partition` can queue confirmed ext2/ext3/ext4 format through
    `mke2fs` and sparse-staged HFS+/HFSX format through `newfs_hfs`.
  - `Create Partition` can queue native Windows creates or certified non-native
    create+format operations for ext2/ext3/ext4, HFS+/HFSX, and Linux swap.
    The non-native path creates the partition without a Windows drive letter,
    derives the new `GLOBALROOT` raw partition alias at Apply time, and runs the
    same manifest/hash-validated formatter used by Format Partition.
  - `Resize/Move Partition` can queue confirmed same-start adjacent ext grow and
    shrink through `resize2fs`.
  - Current original metadata consistency check for XFS/Btrfs/APFS from captured
    read-only probe data.
  - Current original HFS+/HFSX catalog consistency and attributes B-tree key
    scan with defined fork/inline metadata reporting and selected
    attribute-value extraction from raw target data.
  - Deep XFS/Btrfs tool checks, XFS/Btrfs format, and XFS/Btrfs repair stay
    disabled until a Windows-portable tool bundle is approved and destructive
    certification exists for the exact operation.
- Unsupported actions show disabled reason rather than disappearing.

### 6. Safety Model

All non-native file-system writes must require:

- Target is not boot/system/pagefile/crashdump/hibernation.
- Target is offline or safely dismounted.
- Layout hash still matches.
- Tool manifest hash verified.
- Dry-run/plan output shown where the tool supports it.
- Final Apply review names exact file system, target disk/partition, operation type, and destructive risk.
- Typed confirmation for format/repair/write operations.

Repair policy:

- ext: allow `e2fsck -n` read-only check first; repair only after check summary and confirmation.
- XFS: allow original metadata consistency checks from captured read-only probe
  data now; allow deep `xfs_repair -n` only after a portable build is approved;
  repair only after check summary, confirmation, and VM proof.
- Btrfs: allow original metadata consistency checks from captured read-only probe
  data now; allow deep `btrfs check --readonly` only after a portable build is
  approved; repair stays disabled by default until VM/lab certification proves
  supported scenarios.
- HFS+: read-only inspect/browse/extract/catalog consistency checks, attributes
  key scans, and selected attribute-value extraction first; bundled
  sparse-staged `newfs_hfs` format and `fsck_hfs` repair are allowed only
  through manifest/hash validation, raw target identity, Apply review, and
  confirmation. Same-size image-only data-fork overwrite, data/resource-fork
  allocated-block grow/shrink replacement, explicit zero-length truncate,
  inline-attribute replacement, fork-backed attribute replacement within existing allocated blocks, bounded fork-backed attribute allocation growth, bounded file create with data, single-leaf catalog rename/move, and constrained empty-file/empty-folder create/delete are allowed only through the core writer
  API/helper with explicit certification evidence and through staged raw Apply
  for selected HFS+/HFSX partitions; unbounded folder-tree delete, complex file delete, B-tree
  split/rebalance, inline/broad attribute growth,
  compressed-file writes, and broad allocation growth beyond the bounded
  initial-extent slice remain blocked.
- APFS: read-only inspect/browse/extract/export first; encrypted/compressed
  files, format, repair, resize, and write blocked.

## Test And Certification Plan

1. Unit tests:
   - Signature detector fixtures for NTFS, exFAT, FAT12/16/32, ext2/3/4, XFS, Btrfs, HFS+, APFS, Linux swap, including Linux swap header metadata, XFS/Btrfs/APFS superblock/checkpoint metadata assertions, APFS checkpoint-window sanity warnings, bounded APFS volume-superblock candidate assertions, referenced APFS object-header preflight assertions, visible APFS object-map tree-anchor assertions, visible APFS B-tree node-header assertions, and root APFS `btree_info_t` assertions.
   - Registry capability matrix.
   - ext2/ext3/ext4 read-only directory, file-read, symlink target, and bounded
     recursive export fixtures for direct, indirect, and extent-mapped file data.
   - HFS+/HFSX read-only catalog consistency, attributes key/fork-metadata scan, selected attribute-value read, catalog, data/resource-fork, bounded recursive export, image-only same-size data-fork overwrite, image-only data/resource-fork allocated-block grow/shrink/truncate replacement, image-only bounded initial-extent allocation-growth replacement, image-only inline-attribute replacement, image-only fork-backed attribute replacement within existing allocated blocks, image-only bounded fork-backed attribute allocation growth, image-only bounded file create with data, single-leaf catalog rename/move, and constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release
     export fixtures for root listing, nested listing, case-insensitive lookup,
     traversal blocking, sidecar export, and read caps.
   - APFS CLI/config blockers plus physical root/nested listing, selected-file
     read proof, bounded recursive export proof, and five-partition recursive
     candidate-search proof.
   - Tool-manifest hash and license validation.
   - Command generation for each supported operation.
   - Safety blockers for system/protected/removable/dirty/locked targets.

2. Integration tests:
   - Offline image files for read-only inspect/extract.
   - Disposable VHDs for format/check/repair command generation and dry-run.
   - Golden output parsers for each bundled tool.

3. VM/destructive certification:
   - One clean VHD per file system and operation.
   - Run `scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1`
     from the VM desktop for ext format/repair/grow/shrink raw-partition proof.
     The elevated runner requires admin, `-Force`, a non-boot/non-system small
     target disk, and a detected VirtualBox guest unless
     `-AllowNonVirtualBoxGuest` is explicitly supplied for a disposable
     non-VirtualBox lab VM.
   - Run `scripts/run_partition_manager_ext_linux_validation.ps1` to prove
     Windows-created/grown/shrunk ext images are cleanly accepted, mounted, written,
     and rechecked by Linux e2fsprogs.
   - Verify Windows-native disk layout before/after.
   - Optional: verify Linux live ISO can mount and validate ext/XFS/Btrfs
     outputs when a booted live-ISO lane is needed; WSL/Linux VM validation is
     enough for ext bundle compatibility proof and is not a runtime dependency.
   - Verify macOS or trusted HFS+ tooling when expanding beyond confirmed
      sparse-staged HFS+/HFSX format/repair into HFS+ file/folder writes.

4. Release gates:
   - No tool without license/source/hash manifest.
   - Release readiness runs `scripts/check_partition_filesystem_tool_manifest.ps1` against `tools/filesystem/manifest.json`.
   - No unmanifested executable, DLL, license, source, or helper file under `tools/filesystem/`.
   - Third-party license readiness requires every approved filesystem tool in `THIRD_PARTY_LICENSES.md`.
   - No UI action without safety validator and test coverage.
   - No write operation without VM certification evidence.
   - Release docs must state exact supported operation level per file system.

## Milestones

### Milestone A: Accurate Detection

- [x] Add raw signature detector.
- [x] Fill File System column for hidden/unmounted native and non-native signatures as detected when normal or elevated read-only raw probe access is available, not guessed.
- [x] Add detection-source tooltip and Properties field.
- [x] Keep operations read-only.
- [x] Add manifest-gated read-only checker command plumbing for ext/XFS/Btrfs.
- [x] Bundle and approve e2fsprogs `e2fsck` 1.47.4 for ext2/ext3/ext4 read-only checks.
- [x] Enable the Non-Windows File System check action only when manifest, target, operation, and hash validation pass.
- [x] Pin non-native write operations to the exact selected
      `\\?\GLOBALROOT\Device\HarddiskN\PartitionM` target in validator, script
      builder, and queued repair UI.
- [x] Add optional elevated raw-read retry path when non-admin physical-drive access is denied.

### Milestone B: Read-Only Inspect

- [x] Add HFS+/HFSX raw volume-header metadata parsing.
- [x] Add classic HFS wrapper parsing for embedded HFS+/HFSX volumes.
- [x] Add in-app Properties metadata viewer for ext, XFS, Btrfs, APFS, and HFS+ header/superblock/container data.
- [x] Add sidebar and context-menu Inspect Non-Windows File System action for
      captured raw metadata.
- [x] Add ext2/ext3/ext4 read-only directory listing where parser support is reliable.
- [x] Add ext2/ext3/ext4 selected-file extraction where parser support is reliable.
- [x] Add ext2/ext3/ext4 bounded recursive directory export where parser support is reliable.
- [x] Add ext2/ext3/ext4 symlink target display and safe sidecar export.
- [x] Add HFS+/HFSX read-only catalog listing where parser support is reliable.
- [x] Add HFS+/HFSX selected data-fork extraction where parser support is reliable.
- [x] Add HFS+ extents-overflow resolution for fragmented catalog/data/resource fork reads.
- [x] Add HFS+/HFSX selected resource-fork extraction where parser support is reliable.
- [x] Add HFS+/HFSX bounded recursive directory export with resource-fork sidecars.
- [x] Add HFS+/HFSX original read-only catalog consistency checks.
- [x] Add HFS+/HFSX original read-only attributes B-tree key scans in the check report.
- [x] Add HFS+/HFSX read-only defined attribute fork/inline metadata reporting.
- [x] Add HFS+/HFSX selected read-only attribute value extraction for inline and fork-backed attributes.
- [x] Add XFS/Btrfs read-only superblock metadata parsing and APFS read-only container/checkpoint/object-map/volume-OID metadata parsing.
- [x] Add bounded APFS volume-superblock candidate scanning from the read-only probe window.
- [x] Add APFS checkpoint ring next/start index reporting and impossible-window sanity warnings before object-map traversal work.
- [x] Add APFS referenced-object-header preflight for known OIDs inside the read-only probe window.
- [x] Add APFS object-map tree-anchor metadata parsing when the OMAP object is visible inside the read-only probe window.
- [x] Add APFS B-tree node-header and root `btree_info_t` preflight for referenced B-tree roots/nodes visible inside the read-only probe window.
- [x] Add APFS read-only object-map traversal, metadata object-checksum validation, root/directory browsing, and selected-file extraction for normal unencrypted/uncompressed files.
- [x] Add APFS space-manager free-block parsing so Properties and inventory can report real APFS free bytes instead of zero when the spaceman object is visible in the probe window.
- [x] Add APFS bounded checkpoint-data random reads so Properties, inventory,
      and certification probes can report real APFS free bytes when the
      space-manager object is outside the first 2 MiB probe window.
- [x] Add APFS fail-closed image-only format scratch-image builder with NXSB,
      object maps, APSB volume superblock, root tree, spaceman free-space
      accounting, object checksums, SHA-256 report output, and certifier
      detection/listing proof.
- [x] Add APFS in-place existing-image format and production helper bridge with
      destructive wipe confirmation, stale edge zeroing, post-format APFS
      detection, requested volume-name listing, empty-root browse proof,
      raw-partition Apply wiring, and JSON helper output.
- [x] Add explicit read/write raw-device opener and raw-capable APFS
      existing-target format API behind raw opt-in plus hardware-evidence
      preflight flags.
- [x] Add APFS raw-format destructive validation runner and UAC launcher for
      pinned expendable APFS partitions. Physical proof passed Windows-side on a
      128 MiB JMicron USB APFS partition with bounded APFS detection/root-listing
      readback; the previous large-partition result is Windows-side-only because
      Apple rejected the generated spaceman geometry. Current raw APFS
      destructive proof must stay inside a 64-128 MiB one-spaceman-chunk
      partition until multi-CIB support is implemented and Apple-validated.
- [x] Extend the APFS raw destructive lane to write one bounded root regular
      file to generated/minimal raw APFS, verify read-back hash, intentionally
      corrupt one generated APFS metadata checksum block, repair it in place,
      and verify file read-back after repair.
- [x] Add File Management cross-filesystem target bridge and destructive live
      certifier coverage for File Explorer explicit HFS+/one-chunk generated-APFS
      create/write/read/search/duplicate/rename/delete operations on pinned
      expendable raw partitions.
- [x] Extend the APFS raw destructive lane and certifier routes for bounded
      generated/minimal root-file byte-range patch and delete with exact
      read-back or negative-read-back validation. Local tests prove fail-closed
      raw target rejection; physical proof under `external.apfs-raw-format` is
      valid only for the 64-128 MiB one-spaceman-chunk envelope until the
      Apple-native large-target defect is fixed.
- [x] Add APFS generated-image seed-file write proof for one root regular file
      backed by a contiguous multi-block extent and certifier read-back
      validation.
- [x] Add APFS generated-image root-file write proof that writes a 9000-byte
      file into an empty generated APFS scratch image, writes a second file into
      a non-empty generated image, replaces an existing root file, and proves
      bounded existing root-file preservation.
- [x] Add APFS generated-image root-file byte-range patch proof that modifies
      an existing bounded root regular file, preserves sibling root files,
      blocks out-of-range writes, and verifies exact read-back SHA-256.
- [x] Add APFS generated-image root-file delete proof that removes a bounded
      root regular file, zeroes the prior generated data-block region, proves
      negative read-back for the deleted path, preserves sibling root files, and
      supports empty-root after deleting the last file.
- [x] Add APFS generated-image empty root-directory create/delete proof that
      inserts a bounded directory record, proves the created directory lists as
      empty, deletes the directory, and verifies root-listing absence.
- [x] Add Partition Manager Apply wiring for generated-layout APFS root-file
      write, byte-range patch, and delete through a gated APFS File panel
      action, exact raw target identity, generated-layout confirmation, and
      `sak_apfs_writer_cli.exe` payload staging.
- [x] Add Partition Manager Apply wiring and physical proof for generated-layout
      APFS empty root-directory create/delete through the same APFS File
      panel, exact raw target identity, generated-layout confirmation, and
      `sak_apfs_writer_cli.exe` helper bridge.
- [x] Add APFS generated-image and generated-raw root-directory child-file
      write/delete proof, including non-empty directory delete blocking, child
      read-back hash proof, empty-directory-after-delete proof, APFS File panel
      mode wiring, exact raw target identity, generated-layout confirmation, and
      `sak_apfs_writer_cli.exe` helper bridge.
- [x] Add APFS generated-image and generated-raw volume-label change plumbing,
      including APSB checksum restamp, old/new-label JSON reporting, APFS
      browser read-back, APFS File panel mode, and raw command refusal in the CLI
      self-test plus pinned physical raw Apply proof.
- [x] Add APFS generated-image object-checksum repair proof that restamps a
      corrupt supported APFS metadata object in a separate scratch image and
      validates post-repair listing/read-back.
- [x] Pin snapshot, revert, and encrypted/protected volume state to zero in
      the generated-layout verifier used by every generated-layout mutation
      route: volume snapshot-metadata tree OID, revert XID/superblock OID,
      snapshot count, and volume flags must be zero, and both object maps must
      carry no snapshot or pending-revert state. Tampered images fail closed
      with explicit snapshot/revert/encryption blockers; unit tamper tests pass,
      and physical raw APFS proof must now use the 64-128 MiB one-spaceman-chunk
      target envelope before Apple-native certification is claimed.
- [x] Correct the read-only detector's APFS object-map snapshot field offsets
      to the on-disk `omap_phys` layout (`om_tree_oid` 0x30, snapshot tree
      0x38, most-recent snapshot 0x40, pending revert 0x48/0x50) so
      snapshot-risk gating reads the real fields.
- [x] Add APFS physical root/nested listing and selected-file read proof through `GLOBALROOT` raw partition aliases.
- [x] Add APFS bounded recursive directory export and physical export proof through `GLOBALROOT` raw partition aliases.
- [x] Add read-only raw-device opener for Windows `GLOBALROOT` partition aliases so ext/HFS+/APFS browse, extraction, export where supported, and physical-certification file proof work against unmounted raw partitions, not only image files.
- [x] Gate unsupported HFS+/HFSX/APFS/XFS/Btrfs Windows-native check, resize, cluster-size, and label-change UI actions so users are routed to the correct read-only Non-Windows workflow instead of a misleading write path.
- No mount driver.

### Milestone C: ext Format/Check/Repair

- [x] Bundle e2fsprogs `e2fsck` under `tools/filesystem/e2fsprogs/`.
- [x] Add read-only `e2fsck -n -f` checks first.
- [x] Bundle and manifest-approve `mke2fs` and `resize2fs` binaries.
- [x] Add confirmed command builders for ext2/ext3/ext4 format, repair, and resize.
- [x] Wire ext format/repair/grow/shrink through Pending Operations, Apply review,
      elevated/raw target handling, and operation reports.
- [x] Add destructive VM gate runner for ext format, clean repair command path,
      partition grow, shrink, `resize2fs`, recheck, and cleanup on disposable VM media.
- [x] Certify and enable ext shrink after shrink-specific disposable VM validation.
- [x] Execute and record the ext destructive VM gate report.
- [x] Execute Linux compatibility validation for Windows-created/grown/shrunk ext2, ext3, and ext4 images.
- [x] Certify ext shrink disposable VM path before enabling shrink.
- [x] Add confirmed original Linux swap SWAPSPACE2 format command generation,
      UI confirmation, safety validation, and queue/apply wiring.
- [x] Add certified non-native Create Partition paths for ext2/ext3/ext4,
      HFS+/HFSX, and Linux swap with UI confirmation, safety validation, runtime
      raw-target derivation, and script-builder tests.
- [x] Execute and record Linux swap SWAPSPACE2 image compatibility validation
      against S.A.K. and Linux `blkid`/`swaplabel`.
- [x] Add Linux swap raw-partition VM destructive gate runner and in-guest UAC
      launcher.
- [x] Add guarded physical USB destructive proof for ext2/ext3/ext4
      format/repair/grow/shrink and Linux swap raw header detection.

### Milestone D: XFS And Btrfs

- [x] Add read-only XFS/Btrfs superblock metadata before tool enablement.
- [x] Add in-app original read-only metadata consistency checks from captured
      XFS/Btrfs probe data.
- [x] Keep runtime deep checks, format, and repair disabled because no
      Windows-portable xfsprogs/btrfs-progs bundle is approved in the manifest.
- [x] Keep Btrfs repair disabled by default. Official Btrfs tooling warns that
      repair should only be used with expert guidance, so this is not a shipped
      user action without a narrow certified scenario.

### Milestone E: HFS+ Format/Repair Candidate

- [x] Choose hfsprogs external tool path for HFS+/HFSX format and fsck-style check/repair.
- [x] Complete source, license, manifest, and runtime-DLL notice review for the bundled tool path.
- [x] Add image-level validation for bundled HFS+/HFSX format/check/repair.
- [x] Record guarded physical raw-partition destructive proof before claiming hardware-certified HFS+ mutation support.
- [x] Add image-only same-size HFS+ data-fork overwrite, data/resource-fork
  allocated-block grow/shrink/truncate replacement, inline-attribute replacement, fork-backed attribute growth, and constrained
      empty-file/empty-folder catalog create/delete, single-leaf catalog rename/move, bounded file create with data, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release as original arbitrary-write core slices with read-back verification.
- [x] Add staged raw Apply wiring for selected HFS+/HFSX data-fork,
      resource-fork, bounded initial-extent allocation growth, truncate, empty-file/folder create/delete, single-leaf catalog rename/move, bounded file create, allocated-file delete, optional released-block zeroing, bounded folder-tree delete, inline/fork-backed-attribute replacement, and bounded fork-backed-attribute growth through the HFS File
      action and `sak_hfs_writer_cli.exe`.
- [x] Add HFS+ catalog B-tree root-leaf split for one-level catalog trees with
      header-map node allocation, new left/right leaf nodes, new root index
      node, single header-node commit write, freed-node erasure, and
      fail-closed preconditions (big keys, variable index keys, free-node
      count, consistent allocation map). Unit fixtures, CLI self-test on a
      real `newfs_hfs` volume with `fsck_hfs -n -f` proof after the split, and
      physical raw proof on expendable USB HFS media all pass.
- [x] Add bounded HFS+ extents-overflow B-tree insertion/removal for broad
      data/resource-fork allocation growth and bounded file create beyond
      eight extents, including empty-tree first-leaf materialization,
      single-leaf insert/remove, root-leaf split, tree free on last-record
      removal, freed-node erasure, and overflow-record removal on
      allocated-file and folder-tree delete. Unit fixtures, CLI self-test with
      fragmented growth on a real `newfs_hfs` volume plus `fsck_hfs` proof,
      and physical raw proof with raw overflow-file read-back all pass.
- [x] Add HFS+ decmpfs type-3 (inline zlib/raw) and type-4 (resource-fork
      chunked zlib) compressed-file read round trips through `readFile` and
      compressed-content replacement through `replaceCompressedFileContent`
      behind the explicit compressed-file mutation opt-in, with decompression
      read-back proof, unit fixtures for both types plus unsupported-type
      blockers, and a CLI fail-closed proof for non-compressed targets.
- [x] Add the unified HFS+ catalog tree-mutation engine for one- and two-level
      catalogs: per-leaf edits with root-index maintenance, depth-1 and
      depth-2 leaf splits, emptied-leaf removal, two-to-one level collapse,
      freed-node erasure, and exact free-node/map/leaf-count accounting, with
      unit, real-volume `fsck_hfs`, and physical raw proof for post-split
      create/rename/delete.
- [x] Add alternate volume header synchronization to every header-mutating
      HFS+ operation.
- [x] Add HFS+ inline attribute creation through the attributes B-tree
      (empty-tree leaf materialization, single-leaf insert, root-leaf split)
      with the owning catalog record's attribute flag set, real-volume
      `fsck_hfs` proof, and physical raw proof.
- [x] Add HFS+ attribute delete (inline and fork-backed, with block release,
      attributes-leaf free on empty, and catalog attribute-flag clear) and
      fork-backed attribute creation, with real-volume `fsck_hfs` proof after
      every step.
- [x] Add decmpfs LZVN/LZFSE codecs (types 7/8/11/12) through the vendored
      Apple reference library, including the offset-table resource container,
      with encoder/decoder replacement round trips.
- [x] Add three-level catalog tree mutation (generic index-level rebuild with
      per-level sibling chains, 256-leaf working bound) with depth-3 unit and
      real-volume `fsck_hfs` proof including collapse back to one level.
- [x] Add HFS+ journal replay for little-endian in-filesystem journals with
      byte-equality certification against the certified direct-write path and
      fail-closed tamper/geometry proofs.
- Keep encrypted/protected-volume crypto, arbitrary Apple-media APFS
  mutation, catalogs deeper than three levels or wider than 256 working
  leaves, catalog/attributes special-file growth, and big-endian journals
  blocked (see the hard-boundary section at the top of this plan).

## Closed Decisions And Guardrails

- Keep the current MinGW e2fsprogs patch as a source-pinned internal patch set
  shipped with manifest hashes and build notes. Replace it only when an
  upstream or alternate Windows-portable build passes the same manifest,
  license, VM, and Linux-compatibility gates.
- Keep ext read-only browse/extract/export on original S.A.K. parser code so
  users do not need mounts, drivers, WSL, package managers, or external runtime
  tools for read-only file recovery.
- Keep raw unbounded HFS+ folder-tree delete, complex HFS+ file delete, B-tree
  split/rebalance, inline/broad HFS+ attribute growth, and HFS+
  broad allocation growth beyond the bounded initial-extent slice out of the shipped scope. Image-only data-fork overwrite,
  data/resource-fork allocated-block grow/shrink/truncate replacement,
  inline-attribute replacement, fork-backed attribute replacement/growth, bounded file create with data, single-leaf catalog rename/move, and constrained empty-file/empty-folder catalog create/delete, allocated-file delete with allocation-bitmap release, optional released-block zeroing for file/folder-tree delete, and bounded folder-tree delete with allocation-bitmap release are the certified core writer slices; any expanded raw/UI mutation path needs its own
  design, queue operation, safety validator, Apply review, destructive media
  proof, and docs.
- Keep APFS encrypted/compressed reads and arbitrary APFS write/repair/format/resize
  out of the shipped scope. Current production mutation support is limited to
  S.A.K. generated-layout create/format, checksum repair, and bounded root-file
  write/patch/delete, empty root-directory create/delete, and root-directory
  child-file write/patch/delete plus volume-label change through explicit
  confirmation and generated-layout guards.
- Keep Linux live ISO/WSL validation as a separate certification lane, not a
  runtime dependency and not a mandatory local release-readiness step.
- Keep the runtime filesystem-tool manifest hand-authored for the current tool
  set. The manifest validator remains the release gate; move to a generated
  source manifest only if more bundled tools make hand maintenance unsafe.

## Sources

- Microsoft `Get-Partition`: https://learn.microsoft.com/en-us/powershell/module/storage/get-partition
- Microsoft `Get-Volume`: https://learn.microsoft.com/en-us/powershell/module/storage/get-volume
- Linux kernel ext4 superblock layout: https://docs.kernel.org/filesystems/ext4/super.html
- Linux kernel swap header layout: https://sources.debian.org/src/linux/6.12.86-1~bpo12%2B1/include/linux/swap.h
- Btrfs on-disk format: https://btrfs.readthedocs.io/en/latest/dev/On-disk-format.html
- XFS filesystem structure: https://www.kernel.org/pub/linux/utils/fs/xfs/docs/xfs_filesystem_structure.pdf
- Apple HFS Plus volume format: https://developer.apple.com/library/archive/technotes/tn/tn1150.html
- Apple File System Reference: https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf
- linux-apfs-rw APFS raw structure definitions: https://github.com/linux-apfs/linux-apfs-rw/blob/master/apfs_raw.h
- e2fsprogs: https://e2fsprogs.sourceforge.net/
- e2fsprogs utility list: https://e2fsprogs.sourceforge.net/ext2.html
- Cygwin e2fsprogs package summary: https://cygwin.com/cygwin/packages/summary/e2fsprogs.html
- MSYS2 package management: https://www.msys2.org/docs/package-management/
- Btrfs check safety and repair warnings: https://btrfs.readthedocs.io/en/latest/btrfs-check.html
- dosfstools: https://github.com/dosfstools/dosfstools
- exfatprogs: https://github.com/exfatprogs/exfatprogs
- btrfs-progs: https://github.com/kdave/btrfs-progs
- Btrfs kernel documentation: https://docs.kernel.org/filesystems/btrfs.html
- xfsprogs package overview: https://packages.fedoraproject.org/pkgs/xfsprogs/xfsprogs/
- xfs_repair manual: https://man7.org/linux/man-pages/man8/xfs_repair.8.html
- hfsutils HFS classic limitation: https://www.mars.org/home/rob/proj/hfs/
- libfshfs HFS+ read-only support: https://github.com/libyal/libfshfs
- libfsapfs APFS read-only status: https://github.com/libyal/libfsapfs
- Debian hfsprogs source package: https://packages.debian.org/source/sid/hfsprogs
