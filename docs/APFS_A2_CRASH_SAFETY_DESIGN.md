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
