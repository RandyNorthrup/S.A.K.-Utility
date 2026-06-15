# A2 — In-place commit crash-safety: design + Apple ground truth

The current in-place commit (insert/delete) COWs the file-system chain and
advances the checkpoint ring, but **overwrites the live chunk-info block (187)
and allocation bitmap (188) in place**. That breaks crash-safety: a commit
interrupted after the bitmap write but before the new `nx_superblock` leaves the
previous checkpoint (xid N-1) referencing blocks the now-modified bitmap marks
free → `fsck_apfs` flags it. The crash-interruption gate (truncate mid-commit,
confirm the previous checkpoint still mounts) cannot pass until the cib/bitmap
are copy-on-written like everything else.

## Apple ground truth (decoded from the a2base touch harvest)

Apple's apfs.kext never overwrites the cib/bitmap in place. The **internal pool**
(IP) holds three cib slots + three bitmap slots that the commits cycle through:

| IP block | role across the harvested commits (xid) |
|----------|-----------------------------------------|
| 185 | cib: ghost xid1 → reused at **xid7** |
| 186 | bitmap for slot 185 |
| 187 | cib: live xid2 → reused at xid5 |
| 188 | bitmap for slot 187 |
| 189 | cib: first used at xid6 |
| 190 | bitmap for slot 189 |

The latest spaceman's `sm_dev[MAIN].cib_addr` (offset 2568) moved 187 → 185
across the run. Each commit writes the new cib + bitmap into a **different** IP
slot than the live one, so the previous checkpoint's cib/bitmap stay intact
(crash-safe), and points the re-emitted spaceman at the new slot.

Which IP slot is free is tracked by two structures:
- **`sm_ip_bitmap` ring** — the 16 IP-bitmap blocks 169-184 (`sm_ip_bitmap_base`,
  `sm_ip_bitmap_block_count`). Each commit consumes one to record that
  checkpoint's IP-region allocation state; the harvest changed blocks 171-175.
- **Spaceman ring state** (offsets 0x144=2520 region): `sm_ip_bm_tx_multiplier`,
  the bitmap-ring xid at 2520, a free-head/free-next chain at 2528, and the ring
  array at 2536 with `0xFFFF` in-use markers. Genesis sets free-head 1 / tail 15
  (one slot used); the live container sets free-head 2 / tail 0 (two slots used).
  The harvested xid7 spaceman shows the chain advanced to 6.

## Confirmed live IP state of a generated container (2026-06-14, step-1 prep)

Parsed the spaceman of a certified generated 64 MiB container (mleaf.img, 22
in-place commits) host-side — confirms the bug and pins the exact targets:

| spaceman field | offset | value |
|----------------|--------|-------|
| blocks_per_chunk | 0x24 | 32768 |
| dev[MAIN] block_count / chunk_count / cib_count | 0x30/0x38/0x40 | 16384 / 1 / 1 |
| dev[MAIN] addr_offset (→ cib_addr array) | 0x50 | 2568 |
| flags | 0x90 | 0x1 |
| sm_ip_bm_tx_multiplier | 0x94 | 16 |
| sm_ip_block_count | 0x98 | **6** |
| sm_ip_bitmap_size (blocks) | 0xA0 | 1 |
| sm_ip_bitmap_block_count | 0xA4 | **16** |
| sm_ip_bitmap_base | 0xA8 | **169** |
| sm_ip_base | 0xB0 | **185** |
| cib_addr[0] | 2568 | **187** |
| ring xid | 2520 | 2 |
| ring free-head | 2528 | **1** |
| ring array (u16[16]) | 2536 | [FFFF, FFFF, 3,4,…,15, 0] |

So the generated 64 MiB container's IP region is **6 blocks = 3 cib/bitmap
slots**: slot0 (185,186), slot1 (187,188), slot2 (189,190); the live slot is
**slot1** (cib 187, bitmap 188). **The bug, confirmed empirically:** after 22
commits `cib_addr` is still 187 and the ring free-head is still 1 — the writer
rewrites slot1 in place every commit (Apple round-robins the slots and advances
the ring). The implementation cycles cib/bitmap slot1→slot2→slot0→slot1… , moves
`cib_addr@2568` to the new slot's cib, advances the `sm_ip_bitmap` ring
(free-head @2528, ring array @2536, xid @2520) over blocks 169–184, and re-emits
the spaceman with the new cib_addr + ring (not a verbatim copy). Parse any
generated container's live spaceman the same way (highest-xid object of type
0x80000005).

## Decoded IP-region rotation (2026-06-14, step-1 prep cont'd)

Parsed the cib/bitmap/ipbm bytes of the generated 64 MiB container:
- **cib 187** (type 0x40000007, xid 17): chunk[0] {addr 0, block_count 16384,
  free_count 16181, bitmap_addr 188}. Overwritten in place each commit (xid 17).
- **bitmap 188**: raw bit array, 203 bits set (203 allocated blocks in chunk 0).
- **IP region 185-190**: 185 cib (xid 1, ghost), 186 ghost bitmap, 187 cib (xid
  17, live), 188 live bitmap, **189 + 190 are ZEROED — the spare slot S.A.K.
  never uses (the bug)**.
- **ipbm ring**: block 169 = 0x03 (bits 0,1 → IP blocks 185,186 in use = the
  ghost checkpoint's IP state), block 170 = 0x0f (bits 0-3 → 185,186,187,188 =
  the live checkpoint's IP state); 171-184 zero. So each ipbm block is a bitmap
  of the 6-block IP region for one checkpoint; the ring advances one block/commit.

**Rotation the implementation performs** (3-slot round robin): the new cib/bitmap
go into the slot that is neither live nor ghost (the oldest, currently the spare
189/190). After a commit at xid N: new live = the written slot, new ghost = the
prior live slot, the prior ghost slot becomes reusable. Sequence of live cib:
187 → 189 → 185 → 187 …  Each commit also: (a) updates `cib_addr@2568` to the new
cib; (b) writes a fresh ipbm block (171, 172, …) holding the new IP state bitmap
(the bits for the new-live + new-ghost slots), and advances the ring free-head
@2528 / ring array @2536 / xid @2520; (c) re-emits the spaceman carrying the new
cib_addr + ring state. The old (now-ghost-aged-out) cib/bitmap blocks are only
returned to the chunk bitmap once their referencing checkpoint leaves the ring
(free-queue, step 6).

## Implementation plan (the hardest remaining APFS subsystem)

1. **Resolve the live IP state**: parse the spaceman (already resolvable via the
   checkpoint-map) for `cib_addr`, `sm_ip_bitmap_base`, and the ring free-head.
2. **Pick the next free IP slot** for the new cib + bitmap (the slot the live
   checkpoint does not use). For the genesis container the spare pair is
   (189, 190); sustained operation cycles through the three pairs via the ring.
3. **COW the cib + bitmap** into that slot: the new bitmap carries the
   allocation flips, the new cib carries the updated free count and points at the
   new bitmap. The old cib/bitmap (187/188) are left untouched.
4. **Advance the `sm_ip_bitmap` ring**: consume the next IP-bitmap block (169-184)
   for this checkpoint, update the free-head/next chain and the ring xid, and
   record the IP-region allocation in it.
5. **Re-emit the spaceman** with `cib_addr` → the new cib slot and the advanced
   ring state (instead of copying it verbatim as the current engine does).
6. **Free-queue (deferred reclamation)**: COW-freed fs blocks must not be reused
   until their xid ages out of the 4-checkpoint ring. Enqueue them in the
   spaceman free-queue B-trees (fq-main / fq-ip, keyed by {xid, paddr}) instead
   of immediately clearing them in the bitmap; a later commit reaps them once the
   referencing checkpoint is gone. This is what makes *sustained* multi-commit
   operation crash-safe (single-commit safety only needs steps 1-5).

## Truncation test result (2026-06-14): rotation done, free-queue REQUIRED

The cib/bitmap rotation is implemented and Apple-certified at rest (commit
1240750: macOS kernel auto-mounts a rotated container read-write, `fsck_apfs`
passes including "Verifying allocated space"). But the truncation test exposed
that the rotation alone is **not** crash-safe:

- Built state N (2 files) -> commit to N+1 (3 files); simulated a crash before
  the final block-0 write by reverting only block 0 of the N+1 image to N's.
- The result still read **3 files** (N+1's state), not N's 2 - the previous
  checkpoint was destroyed.

Root cause (confirmed by scanning the images): every commit's COW chain lands on
the **same blocks 201-206** because `findFreeBlocksInBitmap` immediately reclaims
the previous commit's freed chain. So N+1 overwrites N's container-omap / volume
/ fs-tree blocks; reverting to N's superblock then walks into blocks now holding
N+1's data. The cib/bitmap rotation keeps N's *allocation* metadata intact, but
nothing keeps N's *fs* blocks intact.

**Conclusion: the free-queue (step 6) is mandatory for crash-safety, not just an
optimisation for sustained operation.** COW-freed fs blocks must stay allocated
(enqueued by xid in the spaceman free-queue B-tree) until the freeing checkpoint
ages out of the descriptor ring (8 desc blocks / 2 = 4 checkpoints), so a later
commit cannot reuse a block a surviving checkpoint still references. Only then
does reverting block 0 roll back cleanly. The rotation + free-queue together make
the truncation test pass.

## Crash-proof test plan
- Build a committed container; run one in-place commit but truncate the image
  after the cib/bitmap COW and before the new `nx_superblock` (and a second
  variant truncating after the checkpoint-map but before the nx_superblock).
- Attach to the macOS VM: apfs.kext must mount the **previous** checkpoint (the
  ring's highest fully-written nx_superblock), the file-system content must match
  the pre-commit state, and `fsck_apfs -n` must be clean.
- Then a sustained loop: many commits + a crash between two of them, confirming
  no block referenced by a surviving checkpoint was reused (the free-queue gate).

## Risk note
This is the single most intricate APFS subsystem (spaceman IP ring + free-queue +
reaper) and, like A1, needs many VM `fsck_apfs` iterations. It is a focused
multi-cycle effort and should be built fresh, not bolted on at the tail of a long
session — a subtle spaceman bug corrupts the allocator silently.
