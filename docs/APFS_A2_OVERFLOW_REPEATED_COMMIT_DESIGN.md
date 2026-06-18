# APFS A2 — repeated metadata-overflow in-place commit (cascade 5+) design

Status: **DONE + Apple-certified 2026-06-18 (unified-group rotation).** A 2 TiB overflow
container took 4 chained in-place commits (xid 2->6); the macOS kernel mounted it read-write
and CONTINUED it (xid 6->8), and `fsck_apfs -n` reported the container + volume "appears to
be OK" with the space manager, the space-manager free queue trees, and allocated space all
clean. Evidence
`artifacts/.../external.apfs-cab-tier-cloud/apfs-overflow-repeated-commit-fsck-clean.png`. The
unified-group rotation (below) is the implementation. The CAB in-place commit is the same
group rotation with cab 0 added (a follow-on); CAB in-place commit stays fail-closed for now.

Original status: **designing/implementing.** The metadata-overflow FORMAT tier (>~1.3 TiB) is
Apple-certified (single-CIB / multi-CIB / overflow / CAB FORMAT all `fsck_apfs`-clean, the
CAB tier across cab_count 2/3/4). In-place COMMIT is certified through multi-CIB; on the
overflow tier only a **single** commit past genesis is allowed (`loadFsCommitContext` fails
closed on repeated overflow commits, and on >~10 TiB where the boundary chunk leaves cib 0).
This is the next A2 gap: **repeated** in-place commits on a metadata-overflow container.

## Why a single commit is the current limit

On the overflow tier chunk 0 is fully reserved, so a commit allocates from the boundary
chunk (`metadataChunks - 1`). The boundary chunk's allocation bitmap is copied-on-write to a
**fixed** internal-pool block (`firstFreeIpBlock`). A second commit would re-COW that same
block — but it is the live bitmap the previous checkpoint still references — corrupting the
allocator once the macOS kernel reclaims it. (apfsck does not model kernel continuation, so
only `fsck_apfs` after a real kernel mount catches this; cert is VM-mandatory.)

## Fix: rotate the boundary-chunk bitmap like chunk 0's bitmap

Chunk 0's `{cib0, bitmap}` already rotates crash-safely through 3 IP slots (genesis / live /
spare), with the IP free-queue holding a 2-deep window of the aged-out slots. Give the
boundary chunk (M-1) the **same 3-slot bitmap rotation**:

- The boundary chunk's bitmap gets `{ghost, live, spare}` slots, placed in the contiguous
  IP-usage prefix right after cib 0's (and, in the CAB tier, cab 0's) rotation slots, so the
  `sm_ip_bitmap` usage stays a single run from `ip_base`.
- `extraChunkBitmaps` therefore covers only the **fully-reserved** overflow chunks 1..M-2
  (immutable, never change); the boundary chunk M-1 is the only overflow chunk with free
  space, so it is the only one that rotates.
- The genesis and live cib 0 chunk-(M-1) entries point at the ghost / live boundary bitmap.

### Layout (computeMultiCibLayout), contiguous from ip_base

```
[immutable cibs (cib_count-1)]
[immutable cabs (CAB tier, cab_count-1)]
[chunk 1..M-2 immutable bitmaps (M-2)]          <- was M-1; boundary chunk removed
cib0Base: ghost cib0, ghost chunk0-bitmap, [ghost cab0], [ghost boundary-bitmap]   <- ghost group
          live  cib0, live  chunk0-bitmap, [live  cab0], [live  boundary-bitmap]   <- live group
          [spare boundary-bitmap]                                                  <- free until commit 1
```

`genesisIpUsage` / `liveIpUsage` counts are unchanged from the certified overflow tier
((M-2)+1 ghost boundary == the old M-1 immutable count), but cib0Base shifts down by one and
the boundary bitmap moves into the rotation region, so the **overflow FORMAT bytes change and
must be re-certified** (single-CIB / multi-CIB stay byte-identical: M==1 has no boundary
slots). Post-IP metadata (omaps, volume, fs-tree, seedData, ipDelta) is unchanged.

## Commit

- `computeGeneratedLayout` exposes `boundaryBitmapBase` (the 3-slot base) and the boundary
  rotation, mirroring `cib0Base` / `nextIpSlot` / `computeIpRotation`.
- `allocateFsCommitBlocks` / `applyOverflowAllocation`: the new boundary bitmap goes to
  `nextBoundarySlot(liveBoundary)` instead of the fixed `firstFreeIpBlock`.
- `writeRotatedCib`: cib 0's chunk-(M-1) entry bitmap_addr -> the new boundary slot.
- `buildIpFreeQueueWindow`: add the boundary's 2-deep window `{prevFreedBoundary@xid-1,
  thisFreedBoundary@xid}` (length-1 runs) alongside the cib-0 window.
- `finalizeFsCommit.ipBitmapUsage`: mark the live + the 2 pending boundary slots used
  (matching the cib-0 accounting), and reclaim the aged-out slot.
- `loadFsCommitContext`: drop the repeated-overflow-commit guard; keep the >~10 TiB
  (boundary-chunk-outside-cib-0) guard for the next cascade (A2-2).

## CRITICAL FINDING (2026-06-18): the rotation must be a UNIFIED GROUP

A first implementation attempt (boundary bitmap as an independent 3-slot rotation
interleaved into the cib-0 region, with its own `computeBoundaryRotation`) got the
overflow FORMAT and the FIRST overflow commit apfsck-clean, but the SECOND commit
failed: cib 0's crash-safe rotation (`nextIpSlot`, stride 2) computed its spare slot
ON TOP OF an interleaved boundary/live block, corrupting it.

Root cause + correct design: **cib 0, cab 0, and the boundary-chunk bitmap all rotate
together on every commit** (cab 0's `cab_cib_addr[0]` points at the rotating cib 0, and
the boundary chunk's bitmap changes every allocation), so they form ONE rotation GROUP,
not three independent rotations. The crash-safe IP region must therefore be 3 GROUP
slots, each `{cib0, chunk0-bitmap, (cab0), (boundary-bitmap)}` of `stride = 2 + cab + ovf`
blocks:

```
cib0Base + 0*stride : ghost group  {cib0, bitmap, (cab0), (boundary)}   <- genesis-used
cib0Base + 1*stride : live  group  {cib0, bitmap, (cab0), (boundary)}   <- live-used
cib0Base + 2*stride : spare group  (free; commit 1 rotates into it)
```

`nextIpSlot` / `computeIpRotation` must use this variable `stride` (the certified
single-/multi-CIB tiers keep `stride == 2`, byte-identical); the commit rotates the WHOLE
group, re-points cab 0 / boundary in the new group, and the IP free-queue frees the whole
previous group (`stride`-length run). This unifies the CAB in-place commit (A2-2) with the
overflow boundary rotation (A2-1): they are the same group rotation.

This is a real refactor of the certified crash-safe rotation machinery (`nextIpSlot`,
`computeIpRotation`, `applyOverflowAllocation`, `writeRotatedCib`, the CAB emission, and
the IP free-queue stride) and re-certifies the overflow AND CAB commit tiers on the VM
(apfsck cannot model kernel continuation). It is the dedicated next cascade; the prior
attempt was reverted to keep the committed CAB-certified engine intact.

## Verification (mandatory order)

1. 124 partition-core unit tests: single-CIB / multi-CIB / single-overflow-commit
   byte-identical (M==1 unchanged).
2. apfsck on the cloud box: overflow FORMAT clean at 2 TiB; repeated overflow commits clean
   at rest.
3. macOS VM `fsck_apfs` after a real kernel RW-mount + continuation on a 2 TiB overflow
   container: format + N>=2 chained commits -> container/volume OK (apfsck cannot validate
   kernel continuation; this is the authority).
