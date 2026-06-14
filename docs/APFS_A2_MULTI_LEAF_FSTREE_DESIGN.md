# A2 — Multi-leaf fs-tree: design + implementation plan

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

## Open items to confirm by harvest (mount a S.A.K. container in the macOS VM,
## write ~30 files so apfs.kext splits the tree, unmount, byte-diff/decode)
- **Leaf node oid source**: whether child node oids come from the volume
  `apfs_next_obj_id`, a separate fs-tree oid counter, or the container/omap oid
  space. (newfs/apfs.kext assigns them; the harvest shows the exact values.)
- **Internal record value**: oid only (8 bytes) vs oid + a flags/transid word.
- **Internal record key**: the full child-first-key bytes vs a truncated/ghost
  key, and the internal-node key/value sizes written to the (root) info trailer.
- **btn_flags** subtleties (BTNODE_FIXED_KV_SIZE is **not** set on the fs-tree,
  which uses variable keys; confirm the internal node carries the same).

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
