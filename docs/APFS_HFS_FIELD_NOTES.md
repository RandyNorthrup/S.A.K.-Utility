# APFS / HFS+ Writer Field Notes

Hard-won technique for the userspace APFS and HFS+ write engines: which validator can judge
what, the byte-layout and accounting traps that cost real debugging time, invariants that must
hold across a commit, failure modes seen only on genuine Apple-created volumes, and the reasons
behind shapes that look arbitrary.

This file is deliberately **not** a spec and **not** a tracker. On-disk field offsets, struct
layouts and named limits live in the code and in the design docs, and are cited here rather than
repeated:

| Want | Look at |
|---|---|
| APFS on-disk offsets, with the reason each was chosen | the `kApfs*Offset` constant block atop `src/core/partition_apfs_writer.cpp` |
| APFS in-place COW checkpoint design | `docs/archive/APFS_A2_INPLACE_COMMIT_GROUND_TRUTH.md`, `docs/archive/APFS_A2_CRASH_SAFETY_DESIGN.md`, `docs/archive/APFS_A2_MULTI_LEAF_FSTREE_DESIGN.md`, `docs/archive/APFS_A2_OVERFLOW_REPEATED_COMMIT_DESIGN.md` |
| CAB spaceman tier (`cibs_per_cab`, cab addressing) | `docs/archive/APFS_A2_CAB_TIER_DESIGN.md` |
| Snapshot create/revert byte recipes harvested from macOS | `docs/apfs-harvest/snapshot-create-recipe.txt`, `snapshot-revert-recipe.txt` |
| Cert rig invocations, credentials policy, foreign-volume workstreams | `docs/APFS_LIVE_RECERT_FOLLOWONS.md` |
| Roadmap, acceptance definition, cross-cutting promotion rules | `docs/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md` |
| decmpfs / resource-fork / lzbitmap byte layouts | `include/sak/apfs_compression.h`, `apfs_resource_fork.h`, `apfs_lzbitmap.h`, `apfs_lzbitmap_codec.h` |
| FileVault keybag, DER key blobs, XTS keying, outer-HMAC formula | `include/sak/apfs_keybag.h` |
| HFS+ node/catalog/extent offsets and hard-link record shapes | `include/sak/partition_hfs_core.h`; B-tree engine in `partition_hfs_internal.h` |
| HFS+ collation table and its NUL-folding rule | `include/sak/partition_hfs_case_folding.h` |

---

## 1. Oracle discipline

Three validators disagree, and each is authoritative for a different class of defect. Picking the
wrong one is the single most expensive mistake in this codebase.

### 1.1 Who judges what

| Oracle | Authoritative for | Blind to |
|---|---|---|
| Unit readback (our own reader) | fs-tree and omap record shape, byte-level payload round-trip | the spaceman entirely -- it walks omap/fs-tree and never touches the cib array, so it **passes on a completely broken allocator**. It also skips the extent-ref tree, so no unit can catch the snapshot ownership class in section 3. |
| apfsprogs `apfsck` | allocation accounting, extent-ref coverage, B-tree structure, checksums; deep decode of ZLIB, PLAIN and LZBITMAP compression | valence; whole-slot cib rotation; spaceman struct overlap; Apple's own stream-backed `com.apple.decmpfs` ("header won't fit"); LZVN/LZFSE forks |
| Apple `fsck_apfs` / a live kernel mount | everything apfsck misses, plus anything the kernel enforces that apfsck merely tolerates | nothing relevant, but it is slow, needs a Mac or VM, and an RW automount mutates the thing you are measuring |

apfsck is **not** authoritative. It has passed containers the macOS kernel then corrupted. It is
nonetheless the *stricter* of the two on the checks it does implement, so a structure that both
tools accept is far better evidenced than one only the kernel accepts. Certify on both.

Cases where apfsck says yes and the kernel says no:

- **Whole-slot cib rotation.** The kernel rotates only `cib0`; `cib1..N-1` stay fixed. Only
  `fsck_apfs` after the kernel has mounted RW and *continued* the checkpoint ring catches a writer
  that rotates the wrong slot.
- **Spaceman struct overlap** at `sm_dev[1].addr_offset`.
- **Directory valence.** apfsck has no valence check at all.
- **A CAB where Apple forbids one.** Apple rejects any cab for `cib_count <= 507`; apfsck accepts
  it happily.

And one known apfsck **false positive**: "Chunk-info block: xid is too recent" on a create-dir
over an empty genesis container is bogus.

### 1.2 apfsck invocation traps

- `-v` prints the **version** and exits successfully. It is not verbose. Every `-cuvw` run is a
  silent no-op. `-u` (report-unknown) trips on Apple's DEFRAG incompatible feature, which apfsck
  does not implement. The correct flags for this project are `-cw` (invocation in
  `docs/APFS_LIVE_RECERT_FOLLOWONS.md`).
- **Read the output line, not just the exit code.** The hard checks call `report()`, which is
  `noreturn` and exits with failure, so a structural defect does give a non-zero exit. The leak
  reporter does not: it prints a leak line and lets the run finish at exit 0.
- **`wsl ... apfsck | Select-Object` returns `$LASTEXITCODE` of the pipe, not of apfsck.** A SIGBUS
  reads back as EXIT 0. Run it from a `.sh` file that ends with `echo EXIT=$?` and parse that.
- `btree.c` does `++v_block_count` per leaked block, so un-leaking drops the stored count
  one-for-one. Compute an allocation delta locally instead of re-walking the tree to explain a
  difference.
- A frozen snapshot superblock that is freed on disk is **never** counted in `v_block_count` (its
  object type is FS), so a merge/teardown allocation formula gets no quiescent-style `-1` for it.
- To pin an exact expected allocation, patch apfsck's `super.c` to `fprintf` actual vs expected
  `v_block_count` (per-volume `ALLOCDBG` lines: two lines means two volumes were walked), then
  restore the `.bak`.

### 1.3 fsck_apfs invocation traps

- On an RW-mounted volume or container, `fsck_apfs -n` refuses with "mounted with write access;
  re-run with -l". `diskutil unmountDisk diskN` first -- **offline `-n` is the stronger check**.
- On an unmounted **locked** encrypted volume it fails "crypto I/O mode ... Invalid argument". So
  unlock and mount, *then* `fsck_apfs -n -l`.
- When using a genuine Apple-created baseline (`diskutil eraseDisk APFS NAME GPT /dev/diskN`),
  always add `UUID=<vol> none apfs rw,noauto` to `/etc/fstab` afterwards. Otherwise macOS
  auto-mounts read-write on plug-in and its own commits contaminate the fsck verdict you are
  trying to attribute to our writer.

### 1.4 Cert-design rules that caught real bugs

- **A fileless cert image proves nothing about snapshots.** The extent-ref ownership model in
  section 3 only exists once a volume holds a file. Always cert with a file present.
- **A single-commit cert proves nothing about preservation.** Mutating a volume that already held
  a compressed / xattr'd / sparse file once rebuilt it as a plain inode, and one commit could not
  see it. Test every new special-inode form by mutating the volume a **second** time. All three
  preservation loops (`recoverPreservedFiles`, `restoreRenamePayload`, `buildDeleteFileList`) must
  go through the shared `preserveFileExtentsAndState` / `recoverInodeState` /
  `applyPreservedInodeState` path so a new form is handled once, not three times.
- **Rebuild the Windows CLI after any writer change before regenerating cert images.** A stale CLI
  silently reproduces the bug you just fixed.
- Env-gated probe harnesses live in `tests/unit/test_partition_manager_core.cpp` (grep `SAK_CERT_`
  and `SAK_PROBE` for the full set) and persist images for host apfsck. They are the only way to
  exercise the foreign-volume and snapshot classes that units cannot assert on.

### 1.5 The refuted-finding rule

This writer **rebuilds the whole fs-tree into fresh blocks every commit**. It does not do real
APFS's partial-COW node sharing. That single property kills a recurring class of plausible-sounding
findings:

- Freeing all old fs-tree nodes cannot free a snapshot-owned node -- there are none shared.
- A snapshot create shares every block with live, so apfsck's counted delta is only the frozen
  superblock.
- `buildForeignIpUsedSet` reading only IP-bitmap block 0 is correct at every supported size:
  internal-pool usage stays far under block 0's 32768 bits, which would take over 512 TiB to
  exceed against the 32 TiB `kApfsInPlaceCommitMaxBytes` cap.

Reproduce a finding of that shape with a probe **before** changing anything.

---

## 2. APFS allocation and spaceman

- **fsck credits volume space from the extent-ref tree.** Every data block needs a `j_phys_ext`
  record, or you get `alloc_count` / overallocation warnings that look like a bitmap bug.
- **An empty file must not emit a `j_file_extent`.** A zero-length extent is rejected outright.
- Non-root fs-tree leaves need `o_type` BTREE_NODE (0x3), not the root's 0x2. An internal root's
  `btree_info` `longest_key`/`longest_val` must describe the **whole tree**, not just its own
  child-oid pointers.
- **Resolve the live cib and bitmap from the spaceman (`loadLiveAllocationSlot`) before
  allocating.** Allocating off a fixed slot reuses live blocks and corrupts the volume superblock.
- **Never hardcode the cib-array offset.** 2568 is only the `ip_bm_size == 1` collapse; at
  `ipBmSize 2` it lands in the IP ring and yields a bogus block address. Call
  `spacemanCibArrayOffset`, which scales the inline arrays by mkapfs's exact formula (reference
  implementation: apfsprogs `mkapfs/spaceman.c`).
- `cib0`, the chunk-0 bitmap, the boundary bitmap and `cab0` rotate as **one group**
  (`ipGroupStride`), not independently.
- **apfsck reads the cib array from the main device's `sm_addr_offset` field**, not from an
  `ip_bm_size`-derived offset -- on read *and* on write. Leaving that field stale across an
  `ip_bm_size` transition gives SIGBUS on a grow and "wrong index" on a shrink.
- The internal pool **cannot be pre-sized at format and cannot grow in place** (pinned metadata and
  extents follow it). Adding a chunk therefore means relocating the pool and rebuilding the
  spaceman in **one** COW checkpoint.
- When relocating the pool, put the pool-chunk bitmap at `freePoolBase` (base + 8). `base + 4` is
  cib rotation slot 1, and colliding with it produces "A block is used twice" on the *next*
  mutation, not this one. Set `cib0Base = actualIpBase`: a grow lays rotation groups first, so
  delta-shifting the canonical value underflows `nextIpSlot` and double-marks chunk 0's bitmap.
- A block-count change must also recompute `nx_ephemeral_info[0]`, `sfq_tree_node_limit`, and --
  when a grow crosses a 512 MiB boundary -- `nx_max_file_systems`. The exact formula and why it
  cannot simply be raised is in `src/core/partition_script_builder.cpp`.
- `writeRotatedCib` must stamp chunk 0's `ci_xid` to the commit xid: the cib object's xid must
  equal the maximum chunk xid it covers.

### Fragmentation is rare on purpose

The main free queue holds recent frees **used**, so free space stays contiguous and multi-extent
files essentially never occur by accident. To exercise the multi-extent path deliberately: delete,
age at least `kMainFqRollbackWindow` commits so the hole actually reclaims, *then* allocate across
it.

### Filename hashing

The volume is always case-insensitive. Full-case-fold and NFD-normalize a name **before** the
crc32c dirent hash, and compare names in that same folded form -- never `QString ==`. Get this
wrong and the file is written correctly but is unfindable. The rules and the fold table's
regeneration procedure are commented at `appleCaseFold` / `fullCaseFoldExpansions` /
`drecNameLenAndHash`.

---

## 3. Snapshots and diverge

### The ownership model everything follows from

`snapshot-create` freezes the **current** live extent-ref tree onto the new snapshot and re-points
LIVE at an empty one. Consequences:

- On a volume with files, the **oldest** snapshot owns *all* `KIND_NEW` `physical_ext` records.
- Newer snapshots and live both start empty.
- The live tree is therefore a **signed delta** over those bases, not a standalone map.

| Trap | Symptom | Defence |
|---|---|---|
| Deleting the min-xid **owner** and freeing its extent-ref tree | apfsck "doesn't seem covered by any physical extent" | re-home the owner's tree onto the next-oldest kept snapshot (`reHomeSnapshotExtentRef`) and free *that* one's empty tree instead |
| Resolving a versioned omap oid by index 0 | the diverge-added file silently vanishes; apfsck "wrong count of sparse bytes" | index 0 is the **lowest** xid, i.e. a snapshot root. Resolve by **max** xid (`liveVersionPaddr`) |
| COW'ing the omap header without carrying its snapshot fields | "Snapshot: missing omap entry"; the OMS tree is orphaned | preserve `om_snap_count`, `snap_tree_oid`, `most_recent`, `pending_revert_min` and `pending_revert_max` |
| Rebuilding the live extent-ref tree on a diverge | loses the delta semantics | **extend** it (`extendDivergeExtentRecords`), never rebuild |
| Hardlink diverge leaves the live extref empty, so an extent-ref-gated teardown never runs | "Leaked omap record: unexpected object type" | snapshot-delete's omap teardown runs **first** and independent of extent-ref state (`planSnapshotDeleteRebuild`) |

`computeDivergeState` keeps live-omap versions with xid at or below `om_most_recent_snap`; the live
fs-tree is frozen unless its xid exceeds that.

**Diverge delete rule.** If the block is present in a live `KIND_NEW` record it is post-snapshot:
drop the record and free the block. If it is absent it is pre-snapshot: append a `KIND_UPDATE`
`-1` delta (owner `kApfsPhysExtUndefinedOwner`) and **do not free** -- snapshot-delete's fold nets
it to zero later. Clone is the mirror `+1` (`planDivergeCloneExtents`, applied before sizing).
Patch is an insert fused with a delete. Keep the live delta paddr-sorted throughout.

### Allocation-count arithmetic

`alloc_count` is **logical**, and apfsck sums every snapshot's `v_block_count`. Deltas are fixed
and independent of volume size:

| Operation | Delta |
|---|---|
| First snapshot of a quiescent volume | +2 (`kApfsSnapshotAllocDelta`: frozen superblock plus one snap structure) |
| Each subsequent snapshot | +1 (`kApfsSnapshotSubsequentAllocDelta`) |
| Delete the last snapshot | -2 |
| Delete a snapshot with survivors | -1 |
| Revert | 0 |

### Revert is a deferred tag, not an operation

`snapshot-revert` COWs the volume superblock, sets `revert_to_xid` to the snapshot xid and
`revert_to_sblock_oid` to the frozen superblock's paddr, and leaves everything else identical. The
snapshot is **kept**, and net block change is zero. The **kernel** completes the revert on the next
mount. After it does, the live extent-ref tree is a delta over the now-deleted snapshot's
`KIND_NEW` base, so teardown must interval-refcount-**fold** across the base/live boundary
(`foldSnapshotDeleteExtentRecords`).

### Other snapshot traps

- apfsck's `keycmp` orders by id, then type, then `strcmp(name)` -- **not** by `name_len`. Sort
  `j_snap_name` keys on the name bytes, not the length-prefixed key.
- Delete and patch on a snapshotted volume are **not** fail-closed. Both run `computeDivergeState`
  and diverge. Only snapshot-delete fails closed, and only when the merge would strand an extent
  referenced elsewhere (`mergeSnapshotExtentTrees` plus its coverage check).
- Harvesting from macOS: `__fs_snapshot` op codes are create=1, delete=2, revert=5. `perl` calling
  `syscall(519)` returns ENOSYS and `apfs_systemsnapshot` returns EPERM -- both are dead ends.

---

## 4. Compression (decmpfs)

### Inline vs resource fork

**The choice is made by uncompressed size against the 64 KiB `APFS_COMPRESS_BLOCK`, not by whether
the compressed payload happens to fit the 3804-byte embedded-xattr limit.** Getting this backwards
produces a file the kernel reads back as **zero bytes** while apfsck stays perfectly happy.

An inline-compressed file has **no data stream at all**: no DSTREAM xfield, no dstream-id record,
no extents. Only the NAME xfield plus the decmpfs xattr record.

### Resource-fork traps

- The kernel decompresses `ZLIB_RSRC` through the **classic HFS resource-fork reader**, so the
  entire Apple wrapper is mandatory. A bare block-table-plus-header (`data_offs = 16`,
  `mgmt_size = 0`) satisfies apfsck and fails the kernel.
- ResourceFork xattr flags are `APFS_XATTR_DATA_STREAM` **only**. Adding `FILE_SYSTEM_OWNED` does
  not help. Fork extents are keyed by the `xattr_obj_id`, which is a pure extent-owner with no
  separate inode or dstream-id record. `default_crypto_id` is 0 on an unencrypted volume, not
  `CRYPTO_SW_ID`.
- On a compressed inode, `APFS_INODE_HAS_RSRC_FORK` **replaces** the `NO_RSRC_FORK` default; they
  are mutually exclusive. Forgetting it is only a non-fatal apfsck warning with exit 0, so it will
  not fail your gate -- watch for the line.

### Which oracle can judge which algorithm

| Algorithm | apfsck | Kernel needed? |
|---|---|---|
| ZLIB (inline + RSRC), PLAIN, LZBITMAP_RSRC | deep-decodes and validates | no, but preferred |
| LZVN, LZFSE (inline or RSRC) | **no decoder.** Parses the fork as if it were `cmpf` and passes or fails by blob luck -- a freshly generated, entirely valid LZFSE file can fail it | **yes, the only oracle** |
| Apple's own stream-backed `com.apple.decmpfs` | cannot parse it at all ("header won't fit") | yes, via `fsck_apfs` |

apfsck's `parse_decmpfs` fail-closes with "Inode is not compressed but has decmpfs xattr" when
`bsd_flags` is wrong, which makes an apfsck pass a *genuine* proof of compression rather than a
proof that some bytes were written.

Kernel cert for a fork: attach with
`hdiutil attach -imagekey diskimage-class=CRawDiskImage`, `shasum` the file (the kernel
decompresses transparently, so a matching hash proves decode), and `stat -f %z
file/..namedfork/rsrc` to prove the fork actually exists.

### LZVN framing

LZVN has no self-describing uncompressed form, and its encoder returns 0 for incompressible or
tiny input. A raw payload is therefore stored behind a single marker byte
(`kApfsDecmpfsLzvnRawMarker`, mirroring HFS+'s `kHfsDecmpfsLzvnRawChunkMarker`) chosen because a
valid LZVN stream never begins with it.

### A deliberate test-oracle seam

`apfsDecodeInlineDecmpfs` handles zlib and plain only and returns `nullopt` for every other
algorithm, even though `apfsDecmpfsAlgoIsInline` accepts LZVN, LZFSE and LZBITMAP as inline. That
asymmetry is intentional: a correct LZVN or LZFSE readback **proves the codec-specific reader path
ran**, rather than proving a shared fallback quietly handled it. Do not "simplify" it by teaching
the shared decoder those algorithms.

---

## 5. Encryption (FileVault)

### Scope is surgical

XTS(VEK) covers **only** fs-tree nodes (reached via the volume omap) and file data extents.
Everything else stays plaintext: the container (NXSB, checkpoints, spaceman, container omap), the
APSB, the volume `omap_phys` and its omap tree, the extent-ref tree, and the snap-meta tree.

Fletcher is computed over the **plaintext** block and *then* the block is encrypted. The fs-tree's
omap leaf value sets the encrypted `ov_flags` bit.

### Keybag traps

- Keybag magic is a tag treated as a big-endian uint32 so that the little-endian on-disk bytes
  spell it: container `'keys'`, volume `'recs'`. **Host parsers accept both endiannesses, so a
  byte-reversed magic passes every local test and only the kernel rejects it.** Byte-diff against a
  real harvest.
- The outer keyblob HMAC is kernel-checked, but **apfs-fuse and libfsapfs do not verify it**, so
  reading them cannot reveal the formula. Do not go looking there, and do not brute-force it. The
  derivation lives in `include/sak/apfs_keybag.h`.
- Kernel **unlocks but refuses to mount** (`-69842`) means a missing default crypto-state record;
  the fs-tree needs the `j_crypto` record from `defaultVolumeCryptoStateRecord`.
- Host apfsck requires its **own** keybag encoding, incompatible with Apple's
  (`buildApfsckContainerKeybagBlock` vs `buildKeybagBlock`). Select by variant. Never try to unify
  them.

### Windows CNG -- settled, do not re-litigate

`BCRYPT_XTS_AES_ALGORITHM` opens successfully and accepts a key, but `BCryptEncrypt` and
`BCryptDecrypt` return `STATUS_INVALID_PARAMETER` for **every** parameter combination, while
AES-ECB works in the same process. XTS is therefore built from AES-ECB in
`src/core/apfs_crypto.cpp`. Do not retry CNG XTS.

Related build trap: a stray 0-byte `build/include/sak/apfs_crypto.h` **shadows** the real header,
because `CMAKE_CURRENT_BINARY_DIR/include` is on the search path. The symptom is C2065 on every
constant. Delete the file.

### Per-file keys -- a locked scope, not an open bug

macOS only ever creates ONEKEY volumes (`newfs_apfs -E`, `diskutil apfs addVolume -passphrase`; no
per-file flag exists). Per-file encryption is iOS Data Protection backed by the Secure Enclave, so
it cannot be harvested from macOS and the design is pinned from apfsck source instead.

`apfs.kext` will **not** software-mount a per-file volume: it unlocks -- which proves the keybag
and RFC3394 chain are Apple-faithful -- and then returns `-69842`, while an otherwise identical
ONEKEY volume mounts. This is not a bug to chase.

### Test and wiring traps

- **Encrypted formats randomize the VEK, salt and UUID, so two are never byte-equal.** Assert
  *unlockability* (`verifyApfsImageUnlockableWithPassword`), never image-hash equality.
- The writer preflight lists its source **through the reader**, so a change to reader
  encryption-detection can hijack a writer blocker. Gate detection on a non-zero keylocker, not on
  the `fs_flags` bit.
- Credentials pass by locked temp **file** (`--volume-password-file`), never on the command line
  and never through an environment variable, so the secret never reaches script text or a child
  process listing.
- Encrypted format fails closed above a single-chunk ceiling because the keybag reserved-prefix
  accounting is unverified against the multi-CIB / CAB spaceman. The full reasoning is at
  `kMaximumApfsGeneratedEncryptedFormatBytes` in `src/core/partition_script_builder.cpp`.
- Free keybag-validity signal: macOS Recovery shows its "Disk Password" pre-boot gate only for a
  *valid* FileVault volume. A blank disk avoids the gate, so the gate appearing is evidence.

---

## 6. Foreign (real Apple) volumes

Mutating a pre-existing Apple-created container is a different problem from mutating one we
formatted, because format writes `field == constant` -- so our own output stays byte-identical
whether you read the field or assume the constant, and the bug only appears on foreign media.

**Parse the real on-disk offset field, never a computed offset or a format constant.** apfsck reads
them that way and so must we. This applies on read *and* on write.

| Real-Apple behaviour | What breaks | Defence |
|---|---|---|
| macOS nodes offset their omap-leaf slots | a dense `index * 16` read returns bogus oids | read the fixed-KV TOC `{koff, voff}` pairs |
| Foreign layouts can invert cib vs bitmap ordering (ours is always cib < bitmap) | a `liveCib` base overshoots into the *new* cib | freed-cib slot and IP ghost blocks are `min(cib, bitmap)` |
| Block 0 is not necessarily the newest checkpoint | you mutate a stale superblock | newest checkpoint is a max-xid `xp_desc` scan (`newestCheckpointSuperblock`) |
| **Apple over-allocates**: a single-extent file can own more blocks than `ceil(logical / bs)` | the extent-ref orphans the tail block | keep recovered extents whenever non-empty (not only when fragmented), and set dstream `alloced_size = max(rounded logical, actual extent bytes)` |
| Hard links: re-collection produces one payload per directory entry, duplicating the inode | duplicate inode records | merge via the `j_sibling_link` records; a single-name delete must exclude that (name, parent) pair first |

A real macOS volume can share our object-id **values**, so provenance is never detectable from
oids alone.

**Keep `writeApfsRepairBlock`'s guard** (reject a bad block index, and reject a block-0 write whose
buffer lacks NXSB magic). A miscomputed bitmap block address zeroes the container superblock, and
the resulting failure mode looks exactly like dead hardware.

### Windows access path

The raw path for a GPT Apple_APFS partition is `\\.\Harddisk<N>Partition<M>` -- offset 0 of that
handle is the container -- **not** `PhysicalDriveN`. Paragon's `ApfsForWindowsMountService` locks
the partition: raw reads return 0xFF or `ERROR_SHARING_VIOLATION` until the service is stopped.

---

## 7. Directory trees

- **A listing that hits the entry cap is refused, not truncated.** Real `.Spotlight-V100/Store-V2`
  directories exceed the cap, and truncating silently drops live records from the rebuilt tree.
- Deleting a directory must refuse one holding a child file **or** a child subdirectory. Checking
  only files orphans the subtree, and `fsck_apfs` rejects the volume.
- Rename and move must reject a destination name already taken by a file **or** a subdirectory (a
  duplicate dirent is an fsck error), and must reject a move into the target's own subtree by
  walking the destination parent's ancestor chain to the root looking for the moved id.
- Children reference their directory by **object id, not path**, so a rename mutates only the
  target directory's payload and the entire subtree follows with no per-child edit.
- `directChildCount` counts child files **and** child subdirectories, so every directory inode's
  valence is exact at any depth. apfsck will not catch a wrong valence -- only the kernel and
  `fsck_apfs` will.
- **A flat or single-level tree must remain byte-identical to the pre-nesting layout** (the parent
  id defaults to the root id). Any record diff on a flat tree is a regression, which makes flat
  trees a free canary for nesting changes.
- The subtree recursion passes a sink **struct**, not loose parameters. `scripts/run_lizard.py`
  enforces `PARAM <= 5`, so adding one more argument there breaks the quality gate.

Kernel cert watch items for nesting, both of which have caught real bugs: kernel-enumerated
`nchildren` equals the drec count at depth 2 and below, and fsck reports no orphaned inode.

---

## 8. HFS+

### Mutation model

Load the **whole** tree, edit leaf records, rebuild the index bottom-up, write back. Loaders
recurse by height; emit packs each level into as few nodes as fit until one root remains. It is
depth-generic and O(tree) per mutation -- slow but correct. Incremental split is an efficiency
question, not a correctness one; do not treat the full rebuild as a defect.

- **Node-pool growth must size for a whole fresh index level set**, not `tree_depth + 2` free
  nodes, because every rebuild allocates new index nodes. `withCatalogNodePoolGrowth` doubles per
  retry up to `kHfsMaxNodePoolGrowthTarget`.
- Leaf merging is a left-to-right greedy coalesce; left records all sort before right's, so
  concatenation stays ordered. **The 50%-merge against 100%-split hysteresis is what stops
  thrash.** Without merging, delete churn grows the leaf count monotonically and depth never
  shrinks.
- The catalog collapses to a depth-1 single-leaf root while the extents tree collapses to depth 0.
  That asymmetry is intentional and documented at `HfsExtentsWorkingLeaf` in
  `partition_hfs_internal.h` -- not something to "fix".

### Layout facts no constant name carries

- **A fork is not contiguous.** Node N maps through the fork's extents, so never walk the leaf
  chain as `N * nodeSize`.
- On a wrapper volume, `loadDirectVolumeHeader` **fails by design** on the `'BD'` MDB (the family
  check accepts only H+/HX); `loadWrappedVolumeHeader` then takes over. The failure is the
  dispatch, not an error.
- Names containing NUL or `/` are **valid on disk** -- the kernel maps `/` to `:` -- so never
  reject them. (The reserved metadata directory's own name is four NULs; see
  `partition_hfs_core.h` for how to construct it, since `QStringLiteral` truncates at NUL.)
- Stamping hard-link inode flags must **preserve** existing flags. Clearing the has-attributes mask
  desyncs the attribute bucket count and fsck reports "Incorrect number of extended attributes".
- Deleting a hard-link **alias** must route to the hardlink delete path -- an alias owns no forks.
- The first `ln` **keeps the original file's CNID as the inode**; the old name gets a *new* CNID.
  Assuming the reverse silently swaps which name owns the data.

### Collation

The `FastUnicodeCompare` table (Apple's `gLowerCaseTable`, attribution in
`THIRD_PARTY_LICENSES.md`) folds U+0000 to 0xFFFF so it sorts **last**, which is how the reserved
hard-link metadata directory is placed after its siblings.
`QString::compare(Qt::CaseInsensitive)` folds NUL to 0 and sorts it **first**, so substituting it
mis-sorts the private-data directory against its siblings. Never substitute Qt's comparison for the
table.

### hfsprogs traps

- `newfs_hfs -n` wants `c=N,e=N`, not a bare number. Minimum catalog node is 4096, minimum extents
  node is 1024.
- `newfs_hfs -w` needs `/usr/share/misc/hfsbootdata` to exist. A zero-filled 8 KiB stand-in works.
- **macOS Recovery mounts unjournaled HFS+ read-only.** Make cert volumes journaled (`-J`, then
  pass `--allow-journaled-volume` to our CLI) or `mount -uw`. `fsck_hfs -fn` force-checks a
  journaled volume, but a **dirty** journal makes read-only fsck cry corruption -- replay the
  journal first, then `-fn`.
- The bundled `tools/filesystem` checker is diskdev_cmds 540.1, i.e. Apple's own code, so its
  verdict carries Apple weight rather than third-party weight.
- **Git Bash rewrites a leading-slash `--hfs-path` into a Windows path.** Set `MSYS_NO_PATHCONV=1`
  or run from PowerShell. Decode images with python, never with hand-rolled PowerShell BE16/BE32
  arithmetic.

---

## 9. CLI traps (`sak_apfs_writer_cli`)

These are argument-shape traps, not usage documentation. `--help` is the reference.

| Trap | Detail |
|---|---|
| `--target` is required for **every** command | `format-image` writes to `--target`; `--output-image` is the *destination* for image commits, not the target |
| `--size-bytes` is parsed unconditionally and must be a positive integer for every command | Even `commit-image-*`, which reads the real size and block size out of the image and ignores what you passed. `--block-size-bytes` has a default, so only `--size-bytes` bites |
| `commit-image-file-insert --size-bytes` does **not** resize the container | To get a genuine multi-chunk source, format one: `format-image --size-bytes` |
| The new-name flag is asymmetric | Image clone/hardlink and **both** rename paths take the new name in `--directory-name`; raw clone/hardlink take `--new-file-name`; move takes `--new-file-name` plus `--destination-directory-name` |
| `--directory-name` silently falls back to `--file-name` | So an image clone or hardlink invoked with only `--file-name` names the new entry identically to the source instead of erroring |
| Resize sizing differs by path | `commit-raw-resize` grows to `--new-size-bytes` and treats `--size-bytes` as the raw *device* size (defaulting new-size to it); image resize uses `--size-bytes` |
| `--snapshot-name` becomes required once a volume holds more than one snapshot | |

---

## 10. Host, build and rig traps

- **A test target that compiles core `.cpp` files directly needs `QT_NO_KEYWORDS`** (see
  `tests/CMakeLists.txt`). Without it the target silently fails to compile and **those tests never
  run at all** -- a green suite that is not testing anything.
- **The Windows Qt CLI swallows `fprintf(stderr)`.** Both `2>file` and PowerShell `2>&1` capture
  nothing. Write debug output through a `QFile`; `fopen` trips MSVC warnings-as-errors.
- A "wrong overwrite" blocker means the scratch `--output-image` already exists. Remove it per test
  case.
- **`QFile::copy` on Windows does not preserve sparseness.** Use `copyFileSparse`, or a TiB-scale
  scratch file expands to its full size and fills the disk.
- Whole-disk `format-raw` needs a **4096-aligned** size. 512e drives report a size that is not a
  multiple, so target an aligned Apple_APFS partition instead of the whole disk.
- `apfsWritableBlockBound` treats the **physical** device or image size as the hard cap, falling
  back to reported geometry only when the size is unknown. This matters on a resize-grow, where the
  context still holds the *old* `nx_block_count` while the backing image has already been extended.
- `setRawDeviceTargetPredicateForTesting` overrides **only** the device-path classifier, so a temp
  container exercises the real `commitRaw*` orchestration with every other guard live. It is a
  narrow seam, not a bypass.

### mkapfs / WSL

- `mkapfs` lives at `/root/apfsprogs/mkapfs` and is not on `PATH`. Give it an explicit `[blocks]`
  count.
- **Put bash in a `.sh` file and run the file.** PowerShell mangles `$?` and `$VAR`, and variables
  in an inline `wsl ... bash -c` expand *empty*, so output files land in WSL's `/` instead of where
  you expected. Use full `/mnt/c/...` paths.

### Big-container oracle without the hardware

The CAB tier needs **address space, not platters**:

1. `fsutil file createnew <path> 0`
2. `fsutil sparse setflag <path>`
3. `fsutil file seteof <path> <9TiB>`
4. WSL `mkapfs /mnt/c/... <blocks>` with an explicit block count

That yields a real foreign CAB container occupying a few MB physically. Mutate it through the **raw
in-place** path, which keeps it sparse. To ship it to a Mac, pack **allocated ranges only**:
`fsutil queryallocranges` to an offset/length/bytes stream, unpacked on the far side with seek plus
truncate so it stays sparse, then `hdiutil attach` as `CRawDiskImage`.

---

## 11. Locked rulings

These are decisions, not open questions. Re-deriving them wastes a cycle.

- **COW is the sole mutation engine.** Never re-add a non-COW rewrite path for real media.
- **The lzbitmap codec is clean-room.** apfsprogs' `libzbitmap` is GPL-2.0-only and this project is
  AGPL-3.0-or-later, so vendoring it is a license conflict. Implement from the on-disk **format**
  (magic, chunk-header layout -- a factual interface), never from apfsprogs algorithm code. The
  legal cross-validation reference is the MIT-licensed `eafer/libzbitmap`: round-trip vectors in
  both directions against our codec, and **compile it as C, not C++** (`xor` is a C++ keyword).
- **cib-0 non-boundary reclaim is a documented fail-safe, not a stub.** cib 0 holds only the
  never-freed internal-pool reservation plus the already-reclaimed boundary chunk, so the case is
  unreachable by construction.

### Known limits that are fail-safe by construction

Do not "fix" any of these by loosening the guard:

| Limit | Why it is safe |
|---|---|
| CAB-tier far-chunk reclaim defers on the free queue | a far-cib repoint lives inside a cab; the reclaim is queued, never lost and never corrupting |
| omap snapshot entries must fit **one** fixed-KV node (`omapSnapshotEntriesFitNode`, roughly 140 snapshots at a 4 KiB block) | fails closed; snap-meta itself is multi-node (`planVariableKvTree`), so only the omap side is bounded |
| Directory xattrs are not carried by directory payloads | a real loss, but apfsck and the kernel both accept it silently -- so it must be tracked deliberately rather than discovered |
| Snapshot-delete refuses a merge that would strand an extent referenced elsewhere | correctness over completeness; the coverage check is the whole point |
