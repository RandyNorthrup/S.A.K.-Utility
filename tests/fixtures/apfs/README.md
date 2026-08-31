# APFS binary fixtures

`foreign-apple-256m.dmg.gz` is a deterministic gzip (`gzip -9 -n`) of a genuine Apple-created
256 MiB raw APFS container. It was created on the pinned macOS 26.6 VM on 2026-08-31 with:

```text
hdiutil create -size 256m -layout NONE -fs APFS -volname SAKWS3C <path>.dmg
```

The mounted volume was populated only with `original.txt` and `nested/proof.txt`, using fixed
non-sensitive test strings, then synchronized and cleanly detached. No credentials or user data
are present. Host `apfsck -cw` exited 0 before compression.

Uncompressed size: 268435456 bytes

Uncompressed SHA-256: `6532b126b276a38acecf2248d744f52379c1d2d59fcf03ef30ab084990e1049b`

Compressed SHA-256: `b10953e474c4bb35d6f27e32bd8457079ee7f6a659209c67c39b1f4438f967ea`

The fixture exists to keep the foreign multi-chunk internal-pool rotation regression hermetic.
Generated S.A.K. containers use a different three-slot CIB rotation and cannot prove this path.

`foreign-apple-multicib-17g.bin` is a compact exact-block reconstruction of a genuine
Apple-created 18,043,854,848-byte APFS container with 135 chunks and two CIBs. The source was a
17 GiB `hdiutil` sparse image; its bare `Apple_APFS` partition was read after a clean detach.
The block-map header is `SAKAPFS1`, followed by little-endian image size, block size, record count,
then 59 records of `{uint64 block_index, 4096 exact bytes}`. Unlisted blocks are zero. It contains
only `original.txt` and `nested/proof.txt` with fixed non-sensitive strings.

Block-map SHA-256: `dff5af2c57bd6855e665dd2874eb95c617c0395ec14329f2688440604902e16a`

`foreign-apple-multicib-after-insert.bin` is the same container after the first certified
S.A.K. foreign COW insert (xid 4 -> 5). It has 74 nonzero records. This state is critical: its
new CIB happens to sit at a generated-looking slot, while chunk 3 still owns bitmap block 10900.
The next rotation must remain foreign and must not reuse 10900 even if it appears on the IP free
queue. This exact state exposed both the persistent-classification and all-live-bitmap exclusion
defects during WS3c.

After-insert block-map SHA-256:
`10ab2269b3bf70750e2a08f5a3a578e14f3352ca987d480ce77f77e6588284fe`
