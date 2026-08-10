# APFS live re-cert follow-on program

Origin: the 2026-08-10 live re-certification of the R5 APFS-writer residual campaign
(waves A-G) against GENUINE Apple containers on the macOS VM (10.10.11.102, macOS 26.6,
apfs_kext 2811.160.7). That re-cert is recorded in docs/CODEX_REVIEW_5_REMEDIATION.md
("Fix wave 3 -> LIVE RE-CERT DONE") and the campaign scratchpad apfs_deferred_residuals.md.

Verdict of the re-cert: no wave A-G fail-closed guard false-rejects genuine Apple metadata
on any supported path. It certified clean (kernel mount + read-back sha-match + fsck_apfs
"container appears to be OK"): list-image walk, import-image + add, in-place COW
patch/insert/write/delete (single-chunk), resize shrink (F51 zone), and (after the fix
below) resize grow. It also surfaced the concrete work items tracked here. Randy
(2026-08-10): "all of this needs to be done" -- every item below is REQUIRED, not optional.

Chosen order: (1) broaden the live cert first (it is 2-for-2 at finding real defects and
may reshape the fixes), (2) import metadata fidelity, (3) foreign multi-chunk in-place COW,
(4) infra backlog last (needs no live rig; everything else uses the VM while it is up).

--------------------------------------------------------------------------------
## DONE

- [x] GROW chunk-0 free-count off-by-reclaimed (commit ea3ee59, doc 6521c5d).
      A real multi-chunk-source grow frees aged main-free-queue runs into chunk 0's
      bitmap but layoutMultiChunkGrow seeded chunk 0's cib free count from the SOURCE
      count, under-counting by reclaimed.size(). Pre-existing (2026-07-03), invisible to
      the generated gate. Fixed by recomputing chunk 0 free from the bitmap popcount.
      Re-certified on a real Apple container (fsck_apfs OK pre-mount; apfsck clean
      256->512 and 256->1024); Release ctest 225/225.

--------------------------------------------------------------------------------
## WORKSTREAM 1 -- broaden the live cert (find remaining real-Apple defects)

Status: DONE 2026-08-10. Exercised every wave A-G path against genuine Apple metadata that
the first pass did not cover. ONE real defect found (the grow bug, already fixed above); no
others. All clean on real Apple containers (apfsck kernel-free oracle + Apple kernel fsck).

- [x] Snapshot ops: harvesting an APPLE-created snapshot on an external volume is blocked --
      the VM has SIP ENABLED (csrutil status: enabled), so fs_snapshot_create is unavailable
      and tmutil localsnapshot only snapshots the boot set. Instead certified the stronger
      direction: S.A.K. commit-image-snapshot-create/-delete/-revert on a REAL single-chunk
      Apple container, then Apple's kernel fsck_apfs walked each snapshot by name+xid clean:
      snapAB "snapshot 1 of 2 (sakA, xid 6)" + "2 of 2 (sakB, xid 7)" container OK; snapDel
      "1 of 1 (sakB)" (sakA correctly gone); snapRev both present, OK. (Reading an
      APPLE-created snapshot still wants a SIP-off Mac -- carried as a residual.)
- [x] Chained mutations: resize pipeline on a real multi-chunk Apple container
      (grow 256->512 -> shrink 512->256 -> grow 256->768 -> shrink 768->256) and a COW
      pipeline on a real single-chunk Apple container (patch -> insert -> write -> delete),
      each output re-fed -- apfsck clean at EVERY step.
- [x] Deeper / larger tree: a real Apple container with 800 root files + a depth-3 nested
      dir -- list-image returned all 801 entries, no blockers, no wave B node-budget trip.
- [x] Multi-volume: a real 1 GiB Apple container with two volumes (VOL1 + VOL2 via diskutil
      apfs addVolume; a 256 MiB container caps at 1 volume, nx_max_file_systems=ceil/512MiB).
      list-image + import-image correctly resolve the first volume (wave C target-volume oid)
      and re-emit apfsck-clean.

Confirmed already (not defects): flat multi-chunk import re-emit is apfsck-clean; in-place
COW on a real MULTI-chunk internal pool correctly fail-closes at F16 (see WS3).

Residual carried to WS1-followups: reading an APPLE-created snapshot chain (needs a SIP-off
Mac to harvest one); the physical MacBook rig (10.10.11.38) or a VM SIP-disable is the path.

--------------------------------------------------------------------------------
## WORKSTREAM 2 -- import metadata fidelity (wave G / F1 completion)

Status: DONE 2026-08-10 (commit 2a2412c). Defect was CONFIRMED on the live rig
(import-image reset an adopted real Apple file's owner/group/mode/times/bsd_flags to
generated defaults -- a 0640 file re-emitted as 0644, non-default owners lost). Threaded
the recovered inode identity end to end, reusing the wave G applyRecoveredInodeMetadata
already in the writer. Each layer additive + sentinel-guarded, so the generated path stays
byte-identical (full Release ctest 225/225).

- [x] Reader: parseInodeRecord now reads owner(0x48)/group(0x4C)/bsd_flags(0x44)/times
      (0x10/0x18/0x20/0x28) into InodeRecord; PartitionApfsFileEntry exposes them with a
      has_inode_metadata gate. All offsets are within the value_length >= 0x5C bound.
- [x] CLI: ImportedFile carries the metadata from the listing entry; the optional added
      --file-name file has none -> has_metadata false -> generated defaults.
- [x] Writer: PartitionApfsImageFileInsertCommitRequest += preserve_inode_metadata + eight
      scalars; commitImageOnlyFileWrite builds an ApfsRecoveredInodeMetadata and threads it
      ApfsRootFileWriteRequest -> ApfsFileInsertRequest -> payload -> applyRecoveredInodeMetadata.
- [x] Live re-cert: imported a real Apple container whose files were chowned 502:80 mode
      0640 and 503:81 mode 0600 -- the re-emit preserved those exact owner/group/mode
      (verified via diskutil enableOwnership + stat) and the container fsck_apfs was clean;
      host apfsck clean for the single- and multi-chunk imports.

--------------------------------------------------------------------------------
## WORKSTREAM 3 -- foreign multi-chunk in-place COW + grow increments

- [x] WS3a HONEST MESSAGE (commit 3789c41): the nextIpSlot refusal now names both real
      causes -- "in-place file mutation of a real multi-chunk Apple internal pool is not yet
      supported (use import-image or resize); on a S.A.K.-generated container this indicates
      checkpoint corruption" -- instead of the bare "not a valid rotation slot". No behavior
      change; still fails closed. Verified it fires with the new text on a real multi-chunk
      Apple container.
- [x] WS3b FULL SINGLE-CHUNK GROW (commit 3789c41): a container at exactly one full chunk
      (32768 blocks / 128 MiB) could not grow (the chunk-adding path wants a chunk-0 tail a
      full chunk does not have). Now routed through the multi-chunk-source path (pool in the
      first grown chunk). Certified on a real Apple 128 MiB container: grows to 256/384/512
      MiB are apfsck-clean and fsck_apfs "container appears to be OK" on the macOS kernel with
      all files sha-preserved. Sub-chunk sources unchanged; 225/225.

- [ ] WS3c FOREIGN MULTI-CHUNK IN-PLACE COW -- DEFERRED WITH DESIGN (corruption-critical,
      dedicated pass). In-place COW file mutation (patch/insert/write/delete) on a real
      MULTI-chunk Apple internal pool fails closed at nextIpSlot (correctly, per WS3a). The
      refusal is safe; the feature is the work.
      WHY IT IS A DEDICATED PASS, not a quick reuse: the resize foreignOverflow machinery is
      OVERFLOW-specific -- it is entered only when a mutation SPILLS past chunk 0 (the boundary
      chunk allocation at partition_apfs_writer.cpp configureOverflowAllocation, which sets
      ctx->foreignOverflow). A small file mutation on a multi-chunk container never spills, so
      it takes the GENERAL ring rotation (computeIpRotation -> nextIpSlot -> advanceCheckpoint),
      which is hard-coded to the S.A.K.-generated 3-slot co-located ring (cib0Base + k*stride,
      kIpSlotCount=3). Apple's real multi-chunk pool instead uses a 16-slot ip_bm ring
      (ip_bm_base) with the cib at ip_base+8 -- a different structure the generated model cannot
      represent. Teaching the GENERAL rotation to detect a foreign IP layout and drive the real
      ip_bm ring (allocate the new cib/bitmap from real free IP blocks, advance the real ring
      slot, build the used-set from the live IP bitmap) for EVERY in-place commit is the real
      work -- comparable in scope + risk to the original wave E2 foreignOverflow effort, and it
      must be certified byte-for-byte against real Apple containers before it can ship (a wrong
      rotation silently corrupts live Apple metadata). Supported alternatives for real
      multi-chunk Apple mutation today: import-image (flat root files) and resize.
      DESIGN SKETCH: (1) in loadFsCommitContext, run the foreign-IP probe (foreignIpGeometryInRange
      / the spaceman ip_base+ip_bm_base read) unconditionally, not just on the overflow path, and
      set a ctx.foreignIp flag + the real ring geometry. (2) generalize computeIpRotation: when
      ctx.foreignIp, compute the next real ring slot from ip_bm_free_head/tail + the live slot
      (probe-layout already decodes these) instead of nextIpSlot's 3-slot model. (3) allocate the
      new cib / chunk-0 bitmap from real free IP blocks (ip_base+ip_block_count onward) and build
      the IP used-set from the live IP bitmap. (4) cert on the rig: patch/insert/write/delete on
      real 256 MiB + multi-CIB Apple containers, apfsck + Apple kernel fsck clean, chained.
- [ ] WS3c-2 (smaller follow-on): shrink is the inverse; audit whether a real multi-chunk
      Apple SHRINK hits the same general-rotation limitation (resize shrink certified clean on
      the generated + the first-pass real container, but the foreign-IP shrink ring path should
      be re-confirmed once WS3c lands).

--------------------------------------------------------------------------------
## WORKSTREAM 4 -- infra / gate backlog (no live rig needed)

Status: IN PROGRESS. Not APFS. Carried from the R5 campaign. Two gates wired this pass;
the rest is a debt-reduction + test-infra program (multi-session, much of it cosmetic).

- [x] ASCII-only gate wired (commit 7f48e91). scripts/check_ascii_only.ps1 already scanned
      every tracked text file but was never in .pre-commit-config.yaml; normalized three
      residual source comments (two U+2404 control-pictures for the HFS NUL-prefix, an em
      dash, a Cyrillic homoglyph) so the whole tree is clean (1467 files) and wired the hook
      (self-excludes binary + vendored/evidence; runs on changed files only).
- [x] GUI style-token gate wired (commit 2bee958). Its one residual was a QColor::rgba()
      method call folded into a pixmap cache key, not a raw color literal; the matcher now
      excludes a `.`/`::`-prefixed call, debt is zero, hook wired.
- [ ] Three gate scripts still carry real debt (wired the moment each reaches zero, per the
      note in .pre-commit-config.yaml). MEASURED 2026-08-10:
        check_gui_magic_numbers.ps1        18  (raw layout ints in setContentsMargins/
                                                setSpacing/setFixedHeight -> named ui:: consts;
                                                bounded, low-value cosmetic)
        check_gui_stylesheet_literals.ps1  74  (inline stylesheet literals -> style constants)
        check_magic_numbers.py            452  (whole-tree magic numbers -> named constants)
- [ ] clang-tidy backlog (~995 findings) -- a full clang-tidy pass + fix program.
- [ ] cppcheck-suppression audit -- review every inline suppression is still justified.
- [ ] Test-infra program (each a dedicated effort): G14 coverage ledger, G18 mutation
      testing, fuzz harnesses, fault injection, G20/G21 remaining gate wiring.
- [ ] Branch protection -- needs the GitHub repo admin (Randy); flag once ready.

--------------------------------------------------------------------------------
## Cert rig quick reference (reuse)

- macOS VM 10.10.11.102 (macOS 26.6, apfs_kext 2811.160.7), user randy / pass Calm4200*.
  plink/pscp: `-hostkey SHA256:IWU3+XtT1JRceluCcuTa4pjmzKupjrkjLGyV3jYMG+E -pw Calm4200*`.
- Harvest a bare Apple container: `hdiutil create -size N -layout NONE -fs APFS x.dmg`
  (NXSB at block 0, no GPT; the .dmg extension is required). Attach, `diskutil
  enableOwnership` if testing owner, populate, detach, pscp to Windows.
- Host apfsck (kernel-free oracle): `wsl -d archlinux -u root -- /root/apfsprogs/apfsck/apfsck
  -cw <img>` via the PowerShell tool (Git Bash mangles /root,/mnt). EXIT 0 == clean.
- Mac fsck without kernel-mutation confounder: `hdiutil attach -nomount` then
  `fsck_apfs -n /dev/r<container>` BEFORE any mount (a rw auto-mount advances the checkpoint).
- Physical USB raw-block cert available after 4:30 PM PST if image-level is not enough.
