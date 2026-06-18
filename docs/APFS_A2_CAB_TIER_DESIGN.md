# APFS A2 — CAB-tier spaceman emission (implementation spec)

Status: **designed, not implemented.** The CAB tier is the last + hardest generated-APFS
geometry tier (containers above ~3 TiB, where the inline cib-address array no longer fits the
spaceman block). The tier just below it (metadata-overflow) needed three dated cascade commits
(`307df6d`, `27e8948`, …) to pass `fsck_apfs`; CAB is comparable and modifies the same certified
spaceman/IP allocator, so it must be implemented as a focused effort with per-step `fsck_apfs`
verification on the cloud box (`docs/A2_CLOUD_CERT_PLAYBOOK.md`), not in one shot.

## What changes structurally

Today S.A.K. always emits `cab_count == 0`: the spaceman device's address array (at
`sm_addr_offset`) holds **CIB** block numbers inline. The format fail-closes when
`cib_count > cibs_per_cab` (507) — see `generatedApfsContainerFormatBlocker`
(`partition_apfs_writer.cpp:~4310`) and `computeContainerGeometry` (`:~5488`, already computes
`cab_count = ceil(cib_count / 507)`).

CAB tier (`cab_count > 1`): the spaceman address array holds **CAB** block numbers; each CAB is
an `apfs_cib_addr_block` holding up to 507 CIB block numbers.

## Reference (apfsprogs mkapfs/spaceman.c — verified)

`apfs_cib_addr_block` on-disk layout (block-sized, type `APFS_OBJECT_TYPE_SPACEMAN_CAB`):
```
0x00  cab_o          obj_phys     (32-byte object header)
0x20  cab_index      u32          index of this CAB
0x24  cab_cib_count  u32          number of CIB addrs in this CAB
0x28  cab_cib_addr[] u64[...]     CIB block numbers (up to 507)
```
`cibs_per_cab() = (blocksize - 0x28) / 8 = (4096 - 40) / 8 = 507` (matches `kApfsSpacemanCibsPerCab`).

`make_single_device` (the only branch S.A.K. lacks):
```c
dev->sm_addr_offset = cib_addr_base_off;
if (cab_count == 0) {                           // current S.A.K. path
    cib_addr[i] = first_cib + i;  emit CIB i
} else {                                         // CAB tier (NEW)
    cab_addr[i] = first_cab + i;  emit CAB i
    // make_cib_addr_block: for j in 0..506:
    //   cib_index = 507*i + j; cib_bno = first_cib + cib_index
    //   cab_cib_addr[j] = cib_bno; emit CIB cib_index at cib_bno
}
dev->sm_cab_count = cab_count;                   // sm_dev field (see offset below)
```
mkapfs also: `ip_block_count = 3 * (chunk_count + cib_count + cab_count)` already noted in
`buildSpacemanBlock:1739` (currently drops `cab_count` because it's 0).

## Exact S.A.K. changes

1. **sm_dev cab_count field.** Add `kApfsSpacemanDeviceCabCountOffset` (mkapfs `sm_cab_count`
   sits right after `sm_cib_count`@0x10, so **0x14**; confirm against an Apple/mkapfs dump). Write
   it in `buildSpacemanBlock` from a new `params.cabCount`. Also write `sm_dev.sm_first_cab` if the
   on-disk struct carries it (mkapfs `first_cab` is internal; the on-disk addr array is what fsck
   reads — verify whether a first_cab field exists in `apfs_spaceman_device`).

2. **`buildSpacemanBlock`** (`:1703`): thread `cabCount` + a `cabAddrs` vector. When `cabCount>0`,
   write `cabAddrs` (not `cibAddrs`) at `cibArrayOffset`, set the cab_count field, and
   `ipBlockCount = 3*(chunkCount + cibCount + cabCount)`. The tier2 addr-offset clear
   (`:1753`) must use `cabCount` entries when in CAB tier. Keep the `cab_count==0` path byte-identical.

3. **`computeMultiCibLayout`** (used by `emptyFormatBlocks:5799`): extend to compute `cabCount`,
   place `firstCab` (cabCount blocks) and `firstCib` (cib_count blocks) in the metadata region,
   and produce both the ghost + live **cab**-address vectors (the CABs rotate like cib0 does
   today — confirm which CABs are immutable vs rotated, mirroring the cib0-only rotation in
   `27e8948`). The `ipDelta` must grow by the CAB blocks too.

4. **`emptyFormatBlocks`** (`:5787`): emit each CAB block (`buildCibAddrBlock`, new — mirror
   `buildChunkInfoBlock` but with the CAB layout above) at `firstCab+i`, and the CIBs at the
   `firstCib + (507*i + j)` positions referenced by each CAB.

5. **Allocation bitmap / accounting.** The CAB blocks are allocated metadata: mark them used in
   the chunk-0 (and boundary-chunk, for overflow) allocation bitmap, and include them in
   `allocated`/`free` counts. This is the part that historically needed the most fsck cascades
   (underallocation / "allocated space" errors). Reuse the boundary-chunk allocator from `27e8948`.

6. **Blocker.** In `generatedApfsContainerFormatBlocker` (`:4310`) drop the `cab_count==0`
   requirement once CAB emits; keep a ceiling at the largest tier you have certified (and a hard
   fail-closed above the real 4 TB / max single container if applicable).

7. **In-place commit.** The A2 in-place path (`appendGeneratedApfsLayoutBlockers`) validates a
   single-chunk layout; CAB-tier in-place mutation is a separate, later step. CAB FORMAT first.

## Verification loop (cloud)

On the cloud box (`93.115.26.82`, build `/root/sak/clibuild`): `format-raw` a >3 TiB sparse
container on XFS, attach to the qemu-macOS VM, `fsck_apfs -n /dev/diskN` (drive Terminal via
**vncdotool**, lowercase + tab-complete `fsck_apfs` — it can't send Shift). Iterate on each fsck
complaint (cab object type/checksum, addr-array overlap, ip_block_count, allocation counts) the
same way cascades 3–5 went. The format-hash skip (`53a4868`) keeps each format ~0s.

## Guardrails

- Everything gated on `cab_count > 0`; the certified single-CIB / multi-CIB / metadata-overflow
  paths (`cab_count == 0`) stay byte-identical — run the 124 partition-core unit tests after each
  change to prove no regression.
- Production format cap is 2 TiB (`partition_script_builder.cpp`), below the CAB boundary, so a
  WIP CAB path is not reachable from the production Apply route until it certifies.
