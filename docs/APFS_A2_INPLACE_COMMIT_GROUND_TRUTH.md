# A2 — In-place COW checkpoint commit: Apple ground truth

Harvested 2026-06-14 in the qemu/KVM macOS Sequoia VM (apfs.kext 2332.101.1).
Method: `sak_apfs_writer_cli format-image` a 64 MiB single-chunk container
(`A2BASE`, genesis xid 2); attach raw to the VM; `mount -uw /Volumes/A2BASE`;
`touch a2proof.txt`; `diskutil unmount`; copy the mutated image back; byte-diff
vs the pre-mutation copy. Apple ran **5 commits** (xid 3→7: mount-replay,
remount, touch, sync, unmount). 64 of 16384 blocks changed.

## Object graph per commit (xid N → N+1)

The container is a strict copy-on-write log. **Nothing is overwritten in place
except the descriptor-ring slots reused by the new checkpoint and block 0** (the
container-superblock anchor copy). Every mutated object is re-emitted at a newly
allocated block.

### File-add commit (full chain — Apple xid4/5/6, 6 fs-metadata blocks)
COW order, leaf → root:
1. **FSTREE root** (btree root, subtype `0x0e` FSTREE) — new inode + dirent
   records inserted; stable virtual oid; new block.
2. **Volume omap b-tree node** (btree root, subtype `0x0b` OMAP) — remaps
   `{fstree_oid → new fstree paddr, xid=N}`; new block.
3. **Volume omap header** (`0x0b` OMAP) — points at the new volume-omap tree
   root; **physical** (oid == paddr); new block; its paddr goes into the volume
   superblock `apfs_omap_oid`.
4. **Volume superblock** (`0x0d` FS, `apfs_superblock_t`) — `apfs_omap_oid` →
   new volume-omap header; file/alloc counters bumped; stable **virtual** oid
   (resolved through the container omap); new block.
5. **Container omap b-tree node** (btree root, subtype `0x0b` OMAP) — remaps
   `{volume_sb_oid → new volume-sb paddr, xid=N}`; new block.
6. **Container omap header** (`0x0b` OMAP) — points at the new container-omap
   tree root; **physical**; new block; its paddr goes into `nx_omap_oid`.

### Container-only commit (Apple xid3/7 — 3 blocks)
Volume superblock changed but fstree unchanged → COW only steps 4-6
(volume-sb + container-omap header + container-omap tree node). This is the
minimal valid xid advance and the right **A2 increment 1** target.

### Ephemeral set (every commit, 4 blocks, written into the data ring)
`spaceman` (oid 1024), `reaper` (oid 1025), free-queue IP b-tree (oid 1027,
subtype free-queue), free-queue main b-tree (oid 1029). Re-emitted fresh each
commit at the next 4 data-ring slots, stamped xid=N. Ephemeral oids are stable
across commits.

### Checkpoint (every commit, 2 desc-ring blocks)
`checkpoint-map` (`0x0c`, LAST flag) listing every ephemeral `oid → paddr` of
this xid, followed by the `nx_superblock` (`0x01`). Block 0 is then overwritten
with a copy of the newest `nx_superblock`.

### Spaceman / allocation (every commit)
Chunk-info-block (`0x07` CIB) + its allocation bitmap re-emitted to mark the
newly allocated blocks used and the COW-freed blocks free; internal-pool bitmap
region updated. COW-freed blocks are scheduled in the free-queue with xid=N.

## Checkpoint-position field progression (exact, from the harvest)

`nx_superblock` ring math. desc base=1 (8 blocks), data base=9 (160 blocks):

| state            | xid | desc idx/len/next | data idx/len/next | nx_omap_oid | next_xid |
|------------------|-----|-------------------|-------------------|-------------|----------|
| BEFORE (genesis) | 2   | 2 / 2 / 4         | 2 / 4 / 6         | 199         | 3        |
| Apple xid4       | 4   | 6 / 2 / 0         | 10 / 4 / 14       | 208         | 5        |
| Apple xid5       | 5   | 0 / 2 / 2         | 14 / 4 / 18       | 214         | 6        |
| Apple xid6       | 6   | 2 / 2 / 4         | 18 / 4 / 22       | 220         | 7        |
| Apple xid7       | 7   | 4 / 2 / 6         | 22 / 4 / 26       | 223         | 8        |

**Per-commit rule (the engine):**
- `desc_index := old desc_next`; `desc_len := 2`; `desc_next := (desc_index + 2) mod 8`.
  checkpoint-map at `desc_base + desc_index`, nx_superblock at `+ desc_index + 1`.
- `data_index := old data_next`; `data_len := 4`; `data_next := (data_index + 4) mod 160`.
  ephemerals at `data_base + data_index .. +3`.
- `xid := old xid + 1`; `next_xid := xid + 1`.
- `nx_omap_oid := paddr of the newly COW'd container-omap header`.
- `nx_spaceman_oid`, `nx_reaper_oid`, `nx_next_oid` carry forward (a single file
  add consumes a *volume* object id, not a container oid, so `next_oid` is
  unchanged at 1030).
- Write block 0 = copy of the new nx_superblock.

The S.A.K. genesis live state (BEFORE row) already places the live checkpoint at
desc idx 2 / data idx 2 with `next` pointers at 4 / 6 — so the first S.A.K. A2
commit (xid 3) lands cpm@blk5, nxsb@blk6, ephemerals@blk15-18, exactly where
Apple's first commit (xid3, now aged out of the ring) would have. The genesis
layout is already commit-ready; A2 only needs the COW-allocate + ring-advance
engine.

## Validator
`fsck_apfs -n` (Apple, in the VM) after each commit + kernel RW re-mount; crash
proof = truncate the write log mid-commit and confirm the previous checkpoint
still mounts (the ring keeps the last 4 checkpoints).

## Increment status

### Increment 1 — re-checkpoint engine — DONE + CERTIFIED (commits ae3b941, e6ac106)
`commitImageOnlyCheckpoint`: advance the checkpoint by one transaction in place
with **no** file-system COW (volume content, object maps, spaceman free state
carry forward). Re-emit the 4 ephemerals into the next data-ring slot, write a
fresh checkpoint-map + nx_superblock into the next descriptor-ring slot,
re-anchor block 0. Apple `fsck_apfs -n /dev/disk0` reported the container +
volume "appear to be OK", and apfs.kext kernel-auto-mounted RW and **continued
the S.A.K. checkpoint ring** (xid 4 → 6) — proving the ring is byte-correct.
Helpers: `readLiveCheckpoint`, `reemitCheckpointEphemerals`,
`advanceNxSuperblockCheckpoint`, `stampAndWriteApfsBlock`.

### Increment 2 — real in-place file insert (COW chain + allocation) — PLANNED
For a single-chunk 64 MiB container (`ipDelta = 0`), live genesis addresses:
rootTree 197, volOmap hdr 193 / tree 194, volSb 198, containerOmap hdr 199 /
tree 200; free region starts at 201. A file-add commit (xid N → N+1):

1. Read existing files (reader); append the new file → file list.
2. Allocate from the free region (201+): rootTree→A, volOmap-tree→B,
   volOmap-hdr→C, volSb→E, containerOmap-tree→F, containerOmap-hdr→G, plus
   `D` file-data blocks.
3. Build the COW chain at the new addresses, all stamped xid=N:
   - A = `buildRootTreeBlock(files)` with data extents → the D blocks.
   - B = `buildObjectMapTreeBlock(B, {{rootTreeOid 1028, N, A}}, N)`.
   - C = `buildObjectMapBlock({C, treeBlock=B, N, volOmapFlags})`.
   - E = copy of volSb 198, patch `apfs_omap_oid`(0x80)→C,
     `apfs_fs_alloc_count`(0x58)+=D, `apfs_num_files`(0xB8)++, obj xid→N.
   - F = `buildObjectMapTreeBlock(F, {{volumeOid 1026, N, E}}, N)`.
   - G = `buildObjectMapBlock({G, treeBlock=F, N, containerOmapFlags})`.
4. **Spaceman accounting** (the iterative-fsck part):
   - Chunk bitmap (188): clear bits {193,194,197,198,199,200}; set
     {A..G, D-blocks}. The metadata swap is net-0; net used change = +D.
   - CIB (187) chunk-0 `free_count` -= D; spaceman main `sm_free_count` -= D.
   - These live in the internal-pool ring; Apple COW's the CIB+bitmap (187→189)
     each commit — increment 2a may edit them in place (xid bump + restamp;
     fsck-valid but not crash-safe), increment 2b COW's them through the IP ring.
   - Crash safety: enqueue freed {193,194,197-200} in the free-queue at xid=N
     (increment 2b) rather than immediate bitmap free.
5. Advance the checkpoint via the increment-1 engine, but with the re-emitted
   spaceman reflecting the new free count and `nx_omap_oid := G`.
6. Validator: `fsck_apfs -n` "Verifying allocated space" must pass (bitmap ==
   references), volume mounts RW, the new file reads back; then crash proof.

Reuses `buildRootTreeBlock`, `buildObjectMapTreeBlock`, `buildObjectMapBlock`,
`buildVolumeSuperblock`-style fields, and the increment-1 ring engine.
