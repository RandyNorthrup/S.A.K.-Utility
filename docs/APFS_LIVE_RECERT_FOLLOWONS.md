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

Status: IN PROGRESS. Exercise every wave A-G path against genuine Apple metadata that the
first pass did not cover. Fix whatever it finds (each fix gated + live re-certified).

- [ ] Apple snapshot-bearing container: harvest a real Apple volume WITH a snapshot, then
      list-image / import / resize it (wave B snap-meta tree walk + wave C object auth on
      the snapshot chain). Snapshot creation on an external volume is the hard part
      (tmutil localsnapshot targets the boot set; fs_snapshot_create needs an entitlement
      / SIP posture -- see macos-vm-validation-resources.md). Try; if unreachable on the
      VM, note it and reach for the physical Mac.
- [ ] Chained mutations on a real Apple container: insert -> grow -> shrink -> patch, each
      output re-fed as the next input, fsck clean at every step.
- [ ] Deeper / larger real Apple trees (many files, nested dirs) -> wave B tree-walk node
      budget + wave C on a bigger object set. list-image walks nested; import is flat-only.
- [ ] Multi-volume real Apple container (additional_volume_names) if reachable.

Confirmed already this pass (not defects): flat multi-chunk import re-emit is apfsck-clean;
in-place COW on a real MULTI-chunk internal pool correctly fail-closes at F16 (see WS3).

--------------------------------------------------------------------------------
## WORKSTREAM 2 -- import metadata fidelity (wave G / F1 completion)

Status: SCOPED, not started. Defect CONFIRMED on the live rig: import-image resets an
adopted real Apple file's owner/group/mode/times/bsd_flags to generated defaults (observed
alpha.txt mode 0640 -> 0644 after import). The re-emitted container is valid (apfsck
clean) but loses fidelity. Wave G/F1 preserved this on the writer's own foreign-adopt path;
the CLI import re-emit path (ImportedFile{name,data} -> commitImageOnlyFileWrite) does not.

Plan (each layer additive; generated path must stay byte-identical -> 225/225):
- [ ] Reader: PartitionApfsFileEntry += uid, gid, bsd_flags, create/mod/change/access time.
      parseInodeRecord already reads mode(0x50)/flags(0x30); add owner(0x48), group(0x4C),
      bsd_flags(0x44), times(0x10/0x18/0x20/0x28) -- all within the guarded 0x5C bound.
- [ ] CLI: ImportedFile carries the metadata (with a valid/has-metadata sentinel);
      collectImportSourceFiles copies it from the listing entry. The ADDED --file-name file
      has no source metadata -> sentinel false -> generated defaults.
- [ ] Writer: commitImageOnlyFileWrite request += optional inode metadata; apply it when
      building the inode value (reuse the wave G applyRecoveredInodeMetadata semantics).
- [ ] Live re-cert: import a real Apple container with a non-default owner/mode file; verify
      the re-emitted container preserves uid/gid/mode/times and is fsck-clean.

--------------------------------------------------------------------------------
## WORKSTREAM 3 -- foreign multi-chunk in-place COW

Status: DESIGN, not started. In-place COW file mutation (patch/insert/write/delete) on a
real MULTI-chunk Apple internal pool currently fail-closes at nextIpSlot ("live internal-
pool cib address is not a valid rotation slot"). That refusal is CORRECT today: Apple's real
multi-chunk IP geometry (16-slot bitmap ring + cib at ip_base+8) is not the S.A.K.-generated
3-slot rotation model, and the pre-F16 unguarded code would have rotated the cib into a wrong
block (silent corruption). Single-chunk real Apple in-place COW is fully certified; resize
and import-image already handle real multi-chunk containers.

Plan:
- [ ] Route the in-place COW file-mutation finalize through the foreignOverflow rotation
      machinery that resize already uses (allocate rotation slots from real free IP blocks,
      drive the real ip_bm ring) instead of the generated 3-slot nextIpSlot model.
- [ ] Until then keep the refusal, but make the nextIpSlot message distinguish "unsupported
      real multi-chunk internal pool" from a genuine generated-container corruption (the same
      guard fires for both today).
- [ ] Also: single-chunk-source GROW to exactly 2 chunks where the pool would land in the
      last/only grown chunk fails closed ("placing it in the last chunk is a later
      increment"). Implement that grow increment. (Larger single-chunk grows and all
      multi-chunk-source grows already work.)
- [ ] Live re-cert every path on real Apple containers.

--------------------------------------------------------------------------------
## WORKSTREAM 4 -- infra / gate backlog (no live rig needed; last)

Status: BACKLOG. Not APFS. Carried from the R5 campaign.
- [ ] Wire the source-tree ASCII gate (src is now clean; docs gate already exists).
- [ ] G21 gate debt; cppcheck-suppression audit.
- [ ] G14 coverage ledger; G18 mutation testing; fuzz harnesses; fault injection.
- [ ] clang-tidy backlog (995); G20; remaining gate program; branch protection.

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
