# A2 — Multi-leaf fs-tree: design + implementation plan

> **STATUS (2026-06-14): DONE and Apple-certified (commits e29422e, 60f9101,
> 34c3086).** Builder + commit-path wiring split the fs-tree into an internal
> root over leaf nodes past ~15 files; chained inserts list every file with no
> truncation (host test `apfsWriter_inPlaceFileInsertGrowsIntoMultiLeafFsTree`).
> A generated 20-file container (volume MLEAF) was **macOS-kernel auto-mounted
> read-write** and **`fsck_apfs` passed fully** (container + volume + multi-leaf
> fsroot tree + extent-ref tree). fsck caught two on-disk defects the host reader
> accepted: leaf nodes need `o_type` `OBJECT_TYPE_BTREE_NODE` (0x3) not the root's
> `BTREE` (0x2); and the internal root's `btree_info` longest-key/value must
> measure the whole tree's records, not the root's child-oid pointers. Evidence:
> `artifacts/.../apple-tool-evidence/apfs-a2-multileaf-fsck-PASS.png`.

The in-place commit (and the whole-rewrite) builds the root file-system tree as
a **single 4096-byte leaf node** (`buildRootTreeBlock`: flags ROOT|LEAF, level 0).
That caps a generated container at ~15 root files; beyond it the records now
fail closed (commit f3ba0ef) instead of corrupting. Multi-leaf support lifts the
cap to thousands by splitting the fs-tree into an internal root + leaf nodes.

## Current single-leaf layout (the builder we extend)
- obj header (0x20) + btree node header: flags @0x20 (ROOT|LEAF = 0x0003),
  level @0x22 (0), nkeys @0x24, table-space @0x26/0x28.
- Variable TOC at 0x38: 8-byte entries {key_off(2), key_len(2), value_off(2),
  value_len(2)}. Keys grow up from `0x38 + tocLength`; values grow down from
  `blockSize - btree_info(40)`.
- 40-byte `btree_info` trailer (only on the ROOT node): node size, key/value
  sizes, key_count, node_count.
- fs-tree records: per file an inode (j_inode), dirent (j_drec_hashed), dstream
  id (j_dstream_id) and, with data, a file-extent (j_file_extent); sorted by
  (object id, record type, then drec name-hash / extent logical offset).

## Target two-level layout
1. **Leaf nodes** (level 0, flag LEAF = 0x0002, **no** btree_info trailer): the
   sorted records distributed across `ceil(totalBytes / leafCapacity)` leaves,
   each ~half-to-two-thirds full so inserts don't immediately re-split. Each leaf
   is a distinct **virtual object** with its own oid, mapped by the volume omap.
2. **Root node** (level 1, flag ROOT = 0x0001, keeps the btree_info trailer,
   keeps oid `kApfsFormatRootTreeOid` = 1028): one record per leaf, key = that
   leaf's first (smallest) key, value = 8-byte child node oid. node_count in the
   info trailer = 1 + leaf count; key_count = total records.
3. **Volume object map**: gains an entry per node — `{1028 → rootPaddr}` plus
   `{leafOid_i → leafPaddr_i}` for each leaf. For up to ~250 nodes these still
   fit one omap leaf (`buildObjectMapTreeBlock` already emits N entries; it would
   need the same overflow guard as the fs-tree for very large trees).

## Ground truth (HARVESTED 2026-06-15 — apfs.kext split a S.A.K. MLH container
## after ~60 touch'd files into a root + 4 leaves; mlh.s60.img)
- **Leaf node oid source**: CONFIRMED the container `nx_next_oid` (nx_superblock
  offset 0x58, = 1030 at genesis). The four leaves took oids 1030, 1031, 1032,
  1033 (consecutive); `nx_next_oid` advanced 1030 → 1034 (consumed one per leaf).
  The fs-tree root keeps oid 1028 (`apfs_root_tree_oid`).
- **Internal (root) node**: btn_flags = ROOT (0x0001, **not** LEAF), level 1,
  subtype FSTREE (0x0e), keeps the 40-byte btree_info trailer. Variable-kv TOC
  (8-byte entries). One record per child: **key = that child leaf's first
  (smallest) key, copied whole** (klen 24 for a leading dirent, 8 for a leading
  inode), **value = the 8-byte child node oid** (vlen 8, oid only — no flags).
  Records ordered by child first-key.
- **Leaf nodes**: btn_flags = LEAF (0x0002), level 0, subtype FSTREE, **no**
  btree_info trailer, variable record counts (apfs.kext split unevenly: 73 and
  25 records in two of the leaves), holding the fs records for their key range.
- **Volume object map** maps every node: {1028 → rootPaddr} + {1030..1033 →
  leafPaddr} (5 fixed-kv entries; the omap node TOC is 448 bytes, 16-byte
  keys/values).

## Allocation / accounting impact (reuses the existing in-place engine)
- The COW chain grows from 6 blocks to `6 + leafCount` (each new fs-tree node is
  a freshly allocated block; the old single root is freed). `computeGeneratedLayout`
  free scan + the chunk-0 bitmap/CIB accounting already handle arbitrary block
  counts; the volume `apfs_fs_alloc_count` increases by `leafCount` (extra nodes).
- The volume omap tree node is rebuilt with N+1 entries (it is already COW'd
  whenever the fs-tree changes).
- The free-count deltas threaded through `advanceCheckpoint` /
  `applyFileInsertAllocation` extend unchanged (signed deltas already exist).

## Test plan
- Host: insert > 15 files, assert the reader walks the two-level tree and lists
  every file with correct sizes; round-trip a file's content.
- macOS VM: `fsck_apfs -n` clean (fsroot tree spans multiple nodes), kernel
  mount lists all files, `cat` reads content. Then stress to multiple leaves and
  a re-split.

## Risk note
A b-tree split (node distribution + internal node format + per-node omap
entries + level handling) is a focused subsystem that needs VM `fsck_apfs`
iteration; it should be built fresh with a live VM, not bolted on at the tail of
a long session. The harvest above is the first step.
