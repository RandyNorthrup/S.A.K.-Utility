# Partition Manager Certification

This document says what Partition Manager's destructive operations have been proved to do, and on what. Unit tests prove script generation, safety blockers, queue state and panel behaviour. This is about the disks: whether an operation did what it says when it ran.

## How a row works

`certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` lists every row. `scripts/generate_partition_manager_matrix.py` writes it, and it is never edited by hand. A row names what it runs, what it may honestly run on, what it does not prove, and what a separate tool must read back.

A row has three parts, and they do not share code:

1. **A fixture**, made with Windows' own tools in `scripts/partition_manager_certification_row_bodies.ps1`, because a fixture is not what is under test.
2. **The operation**, run the way a person's Apply runs it. A step client (`tests/certification/partition_manager_scenario_step.cpp`) scans the disks, finds its disk by identity and typed confirmation, builds the request with the same functions the panel calls (`src/core/partition_operation_requests.cpp`), queues it through `PartitionManagerController`, and applies it through the real `sak_elevated_helper.exe`. It runs under a standard-user token, as the app does, and Windows' own account of both tokens goes in the evidence. Data Recovery is the exception: the panel calls that engine in its own process, so that row runs elevated with no helper, and says so.
3. **The readback**, by `scripts/partition_manager_readback.ps1`, which reads the partition table straight off the device, asks Windows about volumes and files, hashes raw ranges, and scans for markers a wipe must have destroyed. A boot row starts the disk alone in a VM of its own.

What this does not drive is the panel's dialogs. The dialogs' choices reach the same functions, and `tests/unit/test_partition_manager_panel.cpp` tests what they queue, but no row presses them.

`scripts/partition_manager_row_evidence.ps1` decides whether a row's evidence is accepted, and both the claim level and the strict verifier use it. It checks each promise the matrix names against the readback, parses each stored partition table again from its raw bytes, and requires each step to have run through the controller and the helper. A field being present proves nothing.

Nothing is signed. A hand-edited report has to agree with its raw bytes and with every step's report, and the whole set is committed, so a stale run or an honest mistake fails. Someone forging all of it on purpose would not be stopped.

## What the claim level says

The level says what ran on what, and there is no level for hardware in general.

| Level | Means |
|---|---|
| `FailedEvidence` | A row in the selected run failed. |
| `CodeCompleteOnly` | Not every disposable-VHD row is accepted. |
| `VhdDataDiskCertified` | Every disposable-VHD row, on virtual disks in a VM, and not every row a VM can run. |
| `VirtualMachinesValidated` | Every row a VM can run, with at least one that needs a real device run on a virtual one, or not run because only a real device can. |
| `NamedDevicesValidated` | Every row, with each that needs a real device run on one. It says those devices, and no others. |

The defrag row needs a real drive that spins, on a physical PC. It passes when Windows reports the drive's media type as `HDD`, or when Windows reports `Unspecified` and the drive itself reports a nonzero rotation rate through the bundled smartctl. A rate never overrules any other answer from Windows, so a drive Windows calls `SSD` fails whatever rate it reports, and a drive that reports no rate fails. The row records the two facts separately, Windows' media type exactly as Windows gives it and smartctl's rotation rate, and the verifier decides from both. The rig's Seagate Expansion Desk 8 TB is on USB, behind a bridge that hides its type: on this device Windows did not report a hard disk, it said `Unspecified`, and the drive reported 5425 rpm. A VirtualBox disk is not accepted at all, whatever it reports. A claim about a real device comes from a row that ran on that device, on a physical PC with the drive plugged straight in. VirtualBox on the rig's Mac cannot do this: over xHCI macOS refuses authorization and Windows gets Code 10, and on a USB 2 controller VirtualBox refuses a SuperSpeed device. A raw-disk link would show Windows a `VBOX HARDDISK` on SATA, which can only count as a virtual machine. The words the site may use are generated from the status and appear in the block below.

## The freeze rule

Docs and the site may change after a freeze. Code may not.

`scripts/partition_manager_code_digest.py` hashes every tracked file except docs, Markdown, evidence, workflows and the configuration of tools that build nothing, with CRLF read as LF so the same commit gives the same digest on a workstation and on a clean CI clone. Evidence holds that digest, and a change to any file it covers invalidates all of it. T3 checks the digest, because it rebuilds and so cannot check a binary's hash. `scripts/run_partition_manager_certification_rig.ps1` records the tested binaries' hashes when it stages them.

## Which evidence counts

`scripts/select_partition_manager_evidence.ps1` selects one run by writing `artifacts/partition-manager-certification/evidence/current/index.json`. Status and verification read only that index. Older runs stay in git as history and can never be picked up as current. Evidence from before 2026-11-04 tested Windows and VirtualBox commands rather than this app, and does not count.

## Running it

The rig is a host with the disposable VirtualBox guest, and for the device rows a disposable physical PC reached over SSH. `AGENTS.local.md` says which machines those are. Nothing runs on the machine you work at but compiling and these scripts' ssh calls: every row changes disks, and every disk it changes is in the guest or on that PC.

| Script | What it is for |
|---|---|
| `scripts/run_partition_manager_final_certification.ps1` | The whole run, on one frozen commit |
| `scripts/run_partition_manager_certification_groups.ps1` | One group of rows, for a trial |
| `scripts/run_partition_manager_certification_rig.ps1` | One action on the rig: stage, deploy, VM on and off, a throwaway disk |
| `scripts/run_partition_manager_app_certification.ps1` | Runs in the guest or on a PC named as disposable, and refuses to run anywhere else |
| `scripts/partition_manager_certification_row_bodies.ps1` | What each row does |

A trial writes to a folder of your choosing and is never selected. Run a row with:

```powershell
pwsh -NoProfile -File scripts/run_partition_manager_certification_rig.ps1 -Action Stage -ReleaseDir build\Release -StageDir <folder>
pwsh -NoProfile -File scripts/run_partition_manager_certification_rig.ps1 -Action Deploy -StageDir <folder>
pwsh -NoProfile -File scripts/run_partition_manager_certification_groups.ps1 -Group vhd -Rows vhd.quick-partition -EvidenceDir <folder> -StageDir <folder>
```

### On a physical PC

The device groups (`ssd-device`, `hdd-device`, `data-device`) run on a physical PC when given `-PcFile`. The other groups stop: they need throwaway disks the VM rig can attach, and a VM of their own to start a disk in. Boot rows also stop on a physical PC, because nothing here can prove a boot there or put the machine back. Proving a boot on real firmware needs a chain written for that PC's disks, and someone to power-cycle it when a row leaves it unbootable.

What the PC needs: a fresh Windows with nothing to keep, UAC on, OpenSSH by key, a local administrator who signs in automatically, and the drives plugged in. The rows run from a scheduled task in that signed-in session, the client under its standard-user token, and Windows' own policy answers the consent prompt, so nobody is at the keyboard and every report says so. No row shows that a person pressing Yes behaves the same.

1. Read the machine's id on the PC: `Get-CimInstance Win32_ComputerSystemProduct | Select-Object UUID` and `Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model`.
2. Write the PC file outside the repository. `confirmation` is what a person types to say this machine, and only this one, may be changed. A board with no id of its own cannot be named. `port`, `jump` and `deny_names` are optional, and `deny_names` are checked for in the evidence along with the rig's own:

   ```json
   { "host": "Technician@10.0.0.5", "key": "C:\\keys\\pc-a", "machine_uuid": "...", "confirmation": "DISPOSABLE <manufacturer> <model> <uuid>", "deny_names": ["..."] }
   ```

3. Prepare it. This checks it is the machine named, that UAC is on and someone is signed in, writes the approval the rows look for, registers the elevated task, and lists the disks:

   ```powershell
   pwsh -NoProfile -File scripts/run_partition_manager_certification_rig.ps1 -Action PcPrepare -PcFile <pc.json>
   ```

4. Write the pin file from that list, by model and exact size. It refuses the disk that runs that Windows, and two disks that match, and prints no serial number:

   ```powershell
   pwsh -NoProfile -File scripts/run_partition_manager_certification_rig.ps1 -Action WritePins -PcFile <pc.json> -PinFile <pins.json> -PinSpec "ssd=<model>|<bytes>;hdd=<model>|<bytes>"
   ```

### The final run

1. Freeze: commit, and leave the tree clean. `python scripts/partition_manager_code_digest.py --root .` must say `code_dirty: false`. Docs and the site may still change after this. Code may not.
2. Build Release from that commit, on any machine.
3. Restore the VM to its clean snapshot, start it, and sign in.
4. For the device rows, prepare the physical PC and write the pin file, as above, immediately before the run. The pin file holds serial numbers, so it stays outside the repository. Check it by eye against the drives in front of you. Every row reads the disk again, against this, immediately before each step:

   ```json
   {
     "ssd": { "model": "...", "size_bytes": "0", "serial_number": "...", "bus_type": "USB", "unique_id": "...", "confirmation": "CERTIFY ... ... ..." },
     "hdd": { "model": "...", "size_bytes": "0", "serial_number": "...", "bus_type": "USB", "unique_id": "...", "confirmation": "CERTIFY ... ... ..." }
   }
   ```

5. Run it. The device rows and the data rows named for the hard drive run on the PC, and everything else on the VM rig. Leave out `-PinFile` and `-PcFile` to run the device rows on virtual disks, which caps the claim at `VirtualMachinesValidated`:

   ```powershell
   pwsh -NoProfile -File scripts/run_partition_manager_final_certification.ps1 -ReleaseDir build\Release -PinFile <pins.json> -PcFile <pc.json>
   ```

   With `-PcFile`, the VM rig's groups and the PC's device groups run as two chains at the same time (`scripts/run_partition_manager_certification_chains.ps1`), each in its own pwsh with its own log, `chain-vm-rig.log` and `chain-physical-pc.log`, in the run's evidence folder; `-PlanOnly` shows both. Without it every group is one chain on the VM rig. A chain stops at its first group that fails, with that group's evidence kept, and a group stops at its first row that fails: the rows after it do not run. The other chain runs on to its end, both results are reported, and the run fails if either failed; the evidence is checked, selected and the block regenerated only once both chains have ended and both passed. A step the rig runs in a guest fails when its log says the job stopped, ran past its time or exited nonzero. A failed row is a finding: fix the code, and a code fix starts again from step 1.

   Each group's guest job has its own limit (`Get-PmGroupJobMinutes` in `scripts/partition_manager_row_evidence.ps1`): `ssd-device` two whole-disk passes of its pinned drive at 15 MiB/s, 20% under the slowest the rehearsal saw (19.1 MB/s), plus 40 minutes, rounded up to the hour, which is 600 minutes for the 256 GB drive; `hdd-device` and `data-device` 60 minutes each (4.1 and 4.5 measured); every other group 240. `-RowMinutes` overrides it for a trial. One whole-disk wipe step is limited to its drive at the same rate plus 30 minutes, and never less than the 6 hours it had.
6. Read the block below, which step 5 regenerated, with its claim level and the words the site may use.
7. Commit `artifacts/partition-manager-certification/evidence/current` and this document, and push. Tag the commit, which runs T3. T3 passes the digest of its clean checkout to `scripts/verify_partition_manager_certification.ps1`, checks the block, and fails on evidence from other code.

### What a run takes

Measured on the rig in the trial of 2026-11-04, one group at a time. The device groups have not run on a physical PC, so theirs are estimates.

| Part | Time |
|---|---|
| Configure, build the step client and helper, stage, deploy | 5 minutes |
| `vhd`: twelve rows, the wipe on a throwaway VDI | 29 minutes |
| `data`: eight rows on VHDs, the dynamic disk on a VDI | 17 minutes |
| `boot-uefi`: migrate, prove it starts, break, repair, prove it starts again | 22 minutes |
| `boot-bios`: build a BIOS install | 83 minutes |
| `boot-bios`: repair chain, then system MBR-to-GPT | 20 minutes |
| `ssd-virtual` and `usb-virtual` | 5 minutes each |
| The device groups on a PC: two whole-disk passes on the 256 GB SSD, a defrag of 16 GB on the hard drive, and four small data rows | about an hour, not measured |

The whole run is about three and a half hours on the VM rig, with the PC's chain beside it (232 minutes for `ssd-device`, 4.1 for `hdd-device` and 4.5 for `data-device` in the rehearsal of 2026-11-09), so about four hours with setup. A failed group stops its chain, and a code fix starts the run again from the freeze.

The rig signs in to a VM through its keyboard, and takes the sign-in as done only when Windows names who is signed in. It tries four times, waiting up to 90 seconds after each, and a VM that never signs in stops the run as "did not sign in": Windows can report that nobody is signed in before its sign-in screen is drawn, and keys typed then are lost.

A proof VM is shut down from inside its Windows, as a person would. Pulling the power leaves Windows recording a failed shutdown, and on BIOS the next start then stops at "Your PC couldn't turn off properly" until someone presses Enter.

## Current Certification State

<!-- BEGIN GENERATED certification-state: do not edit by hand. Regenerate with scripts/check_partition_manager_release_claims.ps1 -Update -->

Current claim level: `CodeCompleteOnly`.
Accepted VHD evidence: 0/12.
Accepted external evidence: 0/18.
Incomplete VHD scenarios: vhd.create-format-resize-delete, vhd.fat32-to-ntfs, vhd.quick-partition, vhd.quick-partition-custom, vhd.quick-partition-mbr, vhd.adjacent-extend, vhd.recovered-partition-restore, vhd.empty-style-conversion, vhd.clear-disk-wipe, vhd.image-clone, vhd.image-restore, vhd.partition-clone-region.
Incomplete external gates: external.system-mbr2gpt, external.os-migration-reboot, external.boot-repair-uefi, external.boot-repair-bios, external.usb-removable, external.file-level-data-recovery, external.ssd-retrim, external.hdd-defrag-execution, external.ssd-secure-erase, external.bitlocker, external.bitlocker-mutation, external.allocate-free-space, external.partition-move, external.cluster-size-change, external.primary-logical-conversion, external.volume-serial-number, external.dynamic-to-basic, external.hardware-wipe.

On this evidence, destructive certification remains partial because VHD evidence is incomplete and VM/hardware/lab evidence is incomplete.
Do not mark the Partition Manager destructive paths fully certified until those reports exist.

What the site may say for this evidence:

> On the v0.9.3 code, Partition Manager has automated tests, and its disk operations have not been validated on disposable disks. Writes to Apple file systems were validated with Apple's own tools. Back up before changing a disk.

Disposable-VHD scenarios (`certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json`):

| Scenario | Name | Accepted on this evidence | Ran on |
|---|---|---|---|
| `vhd.create-format-resize-delete` | Create, format, resize, and delete data partition | no | nothing yet |
| `vhd.fat32-to-ntfs` | FAT32 to NTFS in-place conversion | no | nothing yet |
| `vhd.quick-partition` | Quick Partition equal-size data-disk layout | no | nothing yet |
| `vhd.quick-partition-custom` | Quick Partition custom size and label layout | no | nothing yet |
| `vhd.quick-partition-mbr` | Quick Partition MBR four-data-partition layout | no | nothing yet |
| `vhd.adjacent-extend` | Adjacent free-space Extend Partition Wizard path | no | nothing yet |
| `vhd.recovered-partition-restore` | Recovered partition write-back candidate restore | no | nothing yet |
| `vhd.empty-style-conversion` | Empty data disk GPT/MBR conversion | no | nothing yet |
| `vhd.clear-disk-wipe` | Clear-level non-system disk wipe | no | nothing yet |
| `vhd.image-clone` | Offline VHD image clone and sentinel verification | no | nothing yet |
| `vhd.image-restore` | Offline VHD image restore and overwrite verification | no | nothing yet |
| `vhd.partition-clone-region` | Partition clone to raw target region | no | nothing yet |

External VM/hardware/lab gates (same matrix):

| Gate | Name | Accepted on this evidence | Ran on |
|---|---|---|---|
| `external.system-mbr2gpt` | System MBR-to-GPT conversion in disposable boot VM | no | nothing yet |
| `external.os-migration-reboot` | OS migration target boot and firmware-order proof | no | nothing yet |
| `external.boot-repair-uefi` | UEFI boot repair with intentionally broken BCD | no | nothing yet |
| `external.boot-repair-bios` | BIOS/MBR boot repair with intentionally broken BCD | no | nothing yet |
| `external.usb-removable` | USB removable disk destructive operation proof | no | nothing yet |
| `external.file-level-data-recovery` | File-level Data Recovery scan and restore proof | no | nothing yet |
| `external.ssd-retrim` | SSD/NVMe ReTrim proof | no | nothing yet |
| `external.hdd-defrag-execution` | Direct in-app HDD defrag execution proof | no | nothing yet |
| `external.ssd-secure-erase` | SSD/NVMe secure erase command execution proof | no | nothing yet |
| `external.bitlocker` | BitLocker locked/unlocked data-volume blocker proof | no | nothing yet |
| `external.bitlocker-mutation` | In-app BitLocker unlock/suspend/resume mutation proof | no | nothing yet |
| `external.allocate-free-space` | Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof | no | nothing yet |
| `external.partition-move` | Offline partition start-move proof | no | nothing yet |
| `external.cluster-size-change` | Existing-volume cluster-size change proof | no | nothing yet |
| `external.primary-logical-conversion` | Primary/logical conversion on disposable MBR disk | no | nothing yet |
| `external.volume-serial-number` | Volume serial-number metadata mutation proof | no | nothing yet |
| `external.dynamic-to-basic` | Dynamic disk to basic conversion proof | no | nothing yet |
| `external.hardware-wipe` | Non-system hardware wipe on disposable physical disk | no | nothing yet |

Apple filesystem driver capabilities (`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`):

| Capability | Filesystem | Name | Status |
|---|---|---|---|
| `H-a` | HFS+ | Special-file node-pool growth | certified 2026-06-14 |
| `H-b` | HFS+ | Depth-general and width-general catalog and attributes B-trees | certified 2026-06-17 |
| `H-c` | HFS+ | B-tree underflow merge and rebalance | certified 2026-06-21 |
| `H-d` | HFS+ | General allocator and multi-leaf extents-overflow trees | certified 2026-06-25 |
| `H-e` | HFS+ | Hardlinks, symlinks, private metadata, and complex delete | certified 2026-06-27 |
| `H-f` | HFS+ | Broad attribute growth and attribute overflow records | certified 2026-06-27 |
| `H-g` | HFS+ | Big-endian journal replay | certified 2026-06-27 |
| `H-h` | HFS+ | HFS wrapper and embedded HFS+ offset writes | certified 2026-06-27 |
| `A-a` | APFS | Multi-CIB and multi-chunk space manager | certified 2026-08-31 |
| `A-b` | APFS | Crash-safe in-place COW mutation of Apple containers | certified 2026-08-31 |
| `A-c` | APFS | Snapshot create, delete, and revert | certified 2026-06-26 |
| `A-d` | APFS | Multi-volume containers | certified 2026-06-26 |
| `A-e` | APFS | Compression read and write | certified 2026-06-27 |
| `A-f` | APFS | Credential-gated FileVault encryption | certified 2026-06-27 |
| `A-g` | APFS | Container grow and shrink | certified 2026-08-31 |
| `A-h` | APFS | Clones, sparse files, hardlinks, xattr and ACL, sealed-volume policy | certified 2026-06-28 |
| `A-i` | APFS | Fusion and multi-device Tier2 containers | design decision: out of scope, fails closed |

<!-- END GENERATED certification-state -->

## History

What follows came before this document said what ran on what. Evidence from before 2026-11-04 tested Windows and VirtualBox commands, not this app, and does not count toward the level above. It stays in git as history.

The June checkpoints below are historical evidence, not a current certification claim. Their
retained artifacts remain available; they have not been relabeled as passing the stronger contract.

### Historical June 2026 checkpoints

The default non-mutating readiness status as of 2026-06-02 UTC / 2026-06-02
PDT was `CodeCompleteOnly`. That default release-readiness proof passed on
2026-06-02 09:40 PDT with the non-mutating harness report at
`artifacts\partition-manager-certification\readiness\run-20260602-094050\partition-manager-certification-report.json`.
That readiness run also verified
`artifacts\partition-manager-certification\readiness\external-evidence\`,
`artifacts\partition-manager-certification\readiness\vhd-preflight.json`,
`artifacts\partition-manager-certification\readiness\certification-gap-report.md`,
and
`artifacts\partition-manager-certification\readiness\certification-artifact-bundle.json`.

At that checkpoint, strict status was reported as `VhdDataDiskCertified`, not a current claim,
when release readiness used the strict evidence root. The elevated strict VHD proof passed on
2026-06-02 19:49 PDT / 2026-06-03 UTC with 12/12 VHD gates passed, no failures,
and no skipped VHD scenarios at
`artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\partition-manager-certification-report.json`.
The previous strict proof passed on 2026-06-01 20:34 PDT at
`artifacts\partition-manager-certification\vhd-strict\run-20260601-203234\partition-manager-certification-report.json`.
Strict readiness against that evidence passed on 2026-06-01 22:13 PDT with
`-PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict -RequirePartitionVhdEvidence`.
That default readiness recorded a non-admin VHD preflight blocker, while the elevated
strict preflight recorded administrator=true and ready=true. The retained VM-lab external
manifest records 18/18 external gates passed.
The imported manifest is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`.
The paired imported checklist is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`,
and the importer refreshed the VM-lab run-sheet manifest references to match
that imported manifest before strict handoff.
The historical imported claim level was reported as `HardwareCertified`: that strict VHD proof
recorded 12/12 VHD mutation scenarios passed, and the imported external manifest at
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`
has 18/18 external VM/hardware/lab gates passed with non-empty
matrix-required evidence values and local artifact paths. The paired checklist
is
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`.

Live Windows 11 VM verification was added on 2026-06-02 PDT using VirtualBox
7.2.8 and VM `SAK-PM-Lab-Win11` with Windows 11 25H2, one GPT system disk, and
two 4 GB RAW data disks. `SAK_STARTUP_SMOKE_OK` passed from the read-only
VirtualBox shared path `\\vboxsvr\sakrepo\build\Release\sak_utility.exe`, and
the visible Partition Manager UI rendered 3 disks from that shared executable
in
`artifacts\partition-manager-certification\vm-lab\sak-utility-share-partition-manager-mouse-fixed.png`.
That VM pass verifies non-admin inventory and read-only shared-folder startup
after two code fixes: writable data-root fallback for read-only executable
locations and RAW-disk inventory tolerance when `Get-Partition` returns no
partitions.

A follow-up elevated VM data-disk smoke passed on 2026-06-02 07:44 PDT in the
same VM. UAC elevation was confirmed with `is_admin=true`, then the smoke script
mutated only disposable VirtualBox disks 1 and 2 after boot/system/model/size
guards. It created, formatted, and resized a GPT NTFS partition on disk 1,
deleted and cleared it back to RAW, created an MBR FAT32 partition on disk 2,
converted it to NTFS with `convert.exe`, and cleared it back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\partition-vm-admin-report.json`.
The final guest inventory confirmed both 4 GB data disks were RAW, non-boot, and
non-system. This is supplemental VM destructive data-disk evidence only; it does
not upgrade the claim level because it is not a complete external evidence
manifest.

Partition Manager ext filesystem write proof passed on 2026-06-05 PDT in the
same VM. The elevated gate targeted only disposable VirtualBox disk 1, created
an unmounted GPT data partition, used the app-style raw path
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, verified bundled e2fsprogs
hashes, formatted ext4 with `mke2fs`, ran clean `e2fsck -p` repair and
read-only checks, grew the partition, ran `resize2fs`, shrank the file system
before shrinking the partition, rechecked clean, and
cleared disk 1 back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
The destructive runner now refuses to proceed outside a detected VirtualBox
guest unless `-AllowNonVirtualBoxGuest` is explicitly supplied for a disposable
non-VirtualBox lab VM, so `-Force` alone is not enough on a normal host.
This certifies the queued ext format/repair/grow/shrink path. HFS+/XFS/Btrfs/APFS
write paths remain blocked pending operation-specific destructive proof.

A later ext direct-admin automation rerun passed on 2026-06-05 PDT /
2026-06-06 UTC in `SAK-PM-Automation-Win11`. The host launcher used the real
guest password file, waited for Guest Additions desktop readiness with retries,
validated an already high-integrity administrator token through guestcontrol,
and recorded `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`. Evidence:
`artifacts\partition-manager-certification\vm-lab\host-launch\ext-filesystem-20260605224315.json`
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
This supplemental cross-filesystem proof is not part of the imported 18/18
commercial external matrix unless that matrix is expanded with non-Windows
filesystem gates.

Physical cross-filesystem destructive proof passed on 2026-06-05 PDT /
2026-06-06 UTC against an expendable external USB NVMe disk after the read-only
Apple HFS+/APFS evidence was captured. The local launcher used Windows UAC
`runas` auth instead of keypress automation. The elevated runner required admin,
`-Force`, non-boot/non-system guards, USB media by default, and serial or
friendly-name matching for the large physical disk. It formatted ext2, ext3,
and ext4 on app-style `\\?\GLOBALROOT\Device\Harddisk2\Partition2` raw targets,
ran clean `e2fsck -p` repair and read-only checks, grew and shrank each file
system with `resize2fs`, verified S.A.K. raw probes after format/grow/shrink,
wrote and verified Linux swap SWAPSPACE2 metadata, detected Linux swap with
`partition_filesystem_probe_certifier.exe`, and cleared the disk back to RAW.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive\report.json`.
This supplemental physical proof does not approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth,
compressed-file writes,
arbitrary APFS media mutation, non-generated APFS file writes/resize/arbitrary repair,
XFS/Btrfs writes, or deep XFS/Btrfs tool checks.
APFS writer infrastructure includes fail-closed preflight, Fletcher-64 object
checksum calculation/stamping/verification, deterministic mutation planning,
generated/minimal raw-target format/write/patch/delete, empty root-directory
create/delete, volume-label change, repair, and structured execution-evidence
validation. The writer now also builds an APFS format scratch image with NXSB, object maps, APSB,
root tree, spaceman accounting, stamped APFS object checksums, SHA-256 report
output, and optional generated root seed-file read-back through a contiguous
multi-block extent. It also has generated-image root-file write/patch/delete proof:
the write path copies the source to a separate scratch image, adds or replaces
one bounded root regular file, preserves existing bounded root regular files,
updates root-tree/volume/spaceman metadata, and requires post-write
listing/read-back proof. The patch path copies the source to a separate scratch
image, applies a bounded byte-range update inside an existing root regular file,
preserves sibling root files, rejects out-of-range writes, and requires exact
read-back proof. The delete path removes one bounded root regular file, zeroes
the previous generated file-data region, preserves sibling root files, and
requires deleted-file hash plus negative read-back proof. The directory path
creates or deletes one empty generated-layout root directory, preserves bounded
root files and existing empty root directories across rewrites, verifies the
created directory is empty, and verifies deleted-directory absence. It also has
generated-image repair proof for
recognized APFS metadata object checksums: the repair path copies the source to
a separate scratch image, restamps corrupt supported object checksums, and
requires post-repair detection/list/read-back proof. The execution gate requires
matching evidence ID, operation,
target path, structure mapping, checksum vectors, source/scratch image hash
chain, copy-on-write checkpoint proof, object-map update proof, space-manager
accounting proof, fsck/diskutil validation, target read-back proof,
crash-replay proof, rollback-boundary proof, and artifact paths before even an
image-only plan can be considered executable. Raw-media APFS exposure is
currently limited to S.A.K.-generated one-spaceman-chunk targets from 64 MiB
through 128 MiB; larger generated raw targets remain blocked because Apple
kernel validation rejected the previous 51 GB generated spaceman geometry even
though Windows-side detection accepted it.
That ceiling is now lifted for the validated multi-chunk range: the writer emits multi-spaceman-chunk single-CIB containers, and the new geometry passed Apple-native validation in a qemu/KVM macOS Sequoia recovery VM (apfs.kext 2332.101.1) on 2026-06-14 UTC. Each S.A.K.-written container was attached as a bare raw SATA disk; macOS auto-synthesized and auto-mounted every container read-write (the volume superblock records `last modified by apfs_kext (2332.101.1)`), and `fsck_apfs` reported `The container ... appears to be OK` for all of: 64 MiB single-chunk (`SAK1`), 256 MiB multi-chunk (`SAK2CHUNK`), and 1 GiB eight-chunk (`SAK8`). A kernel read-write round-trip (mount, `mkdir` + `touch`, `sync`, unmount) advanced the checkpoint transaction id (4 to 8 on the single-chunk container, 4 to 7 on the 256 MiB multi-chunk) and `fsck_apfs` still reported the container OK afterward, proving the kernel can mutate the S.A.K.-written container and keep it consistent. Evidence: `artifacts\partition-manager-certification\vm-lab\apple-tool-evidence\report.json` plus the paired `*.fsck_apfs-PASS.png` and `*kernel-rw-roundtrip-PASS.png` screendumps. This is Apple-native VM validation on virtual SATA media; the physical-USB destructive Apple-native lane, plus crash-injection and rollback-boundary evidence, are still required before the certification matrix flips to a full APFS raw-write `yes`. The writer now emits a cryptographically-random v4 UUID per container and volume at format and reads the on-disk UUID back during mutation (no deterministic UUID code remains): two same-size, same-name containers produced distinct UUIDs, a mutation round-trip preserved the on-disk UUID byte-for-byte, and the random-UUID shipping output was re-certified in the same VM (`SAK1`/`SAK2CHUNK`/`SAK8` all `fsck_apfs` OK with `*.random-uuid.fsck_apfs-PASS.png` evidence).
A crash/rollback test was also run on 2026-06-14 UTC: with the 1 GiB `SAK8` container mounted read-write, a `mkfile 900m` write was started and qemu was hard-killed (`pkill -9`) mid-write to simulate power loss. On reboot macOS auto-mounted `SAK8` again (APFS checkpoint recovery), and `fsck_apfs` reported `The container /dev/rdisk0 appears to be OK` (checkpoint transaction id 5) - the uncommitted mid-write data rolled back to the last valid checkpoint with no corruption, proving the S.A.K.-written container participates correctly in APFS copy-on-write checkpoint atomicity (evidence `apfs.sak8-crash-rollback.fsck_apfs-PASS.png`). The physical-USB destructive Apple-native lane was then completed on 2026-06-14 UTC against the disposable JMicron Tech USB disk (Windows Disk 3, user-authorized destruction). A 128 MiB S.A.K. APFS container (volume `SAKUSB`, random UUID) was written to the disk's Apple_APFS GPT partition, the whole physical disk was attached to WSL2 with `wsl --mount --bare \\.\PHYSICALDRIVE3` and passed to the macOS recovery VM as a raw SATA disk; macOS recognized it as `Apple_APFS Container disk3` on physical `/dev/disk2s2`, auto-mounted `SAKUSB` read-write (`last modified by apfs_kext (2332.101.1)`), and `fsck_apfs` reported `The container /dev/rdisk2s2 appears to be OK` (evidence `apfs.sakusb-physical-jmicron.fsck_apfs-PASS.png`). With Apple `fsck_apfs`, kernel read-write mount, physical-USB-destructive, crash, and rollback all now demonstrated on Apple's own tools (recorded in `apple-tool-evidence\report.json` under `evidence_chain_complete`), the full evidence required by the locked scope for the APFS multi-chunk space-manager capability (driver matrix A-a / milestone A1) was collected and human-reviewed on 2026-06-14, and that cell is flipped to certified in `docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md` (single-CIB multi-chunk, 64 MiB-1 GiB validated; the multi-CIB/CAB tier, the full-driver A8 gate, and the in-place-COW / snapshot / multi-volume / encryption rows A-b..A-h remain open).
The A2 generated-APFS CAB tier was Apple-certified on 2026-06-18 UTC, completing the
multi-CIB/CAB FORMAT lane left open above. Containers larger than ~7.8 TiB need more than
507 chunk-info blocks, so the spaceman can no longer list the cib addresses inline; the
writer now emits the CAB tier (`cab_count > 0`), where the spaceman publishes a
`cab`-address array pointing at `apfs_cib_addr_block` objects (`SPACEMAN_CAB`, type 0x06),
each holding up to 507 cib block numbers. Only `cab 0` rotates (it references the rotating
`cib 0`); `cab 1..N-1` are immutable, mirroring the certified cib-0 rotation, and the CAB
blocks live inside the (grown) internal-pool reserved prefix so the existing chunk-bitmap
allocation accounting already covers them. Apple `fsck_apfs` was run on three CAB
containers spanning the production range, each formatted, attached to a qemu/KVM macOS
Sequoia VM, unmounted, and checked: 8 TiB (`cab_count 2`, 65536 chunks / 521 cibs, volume
`CABTEST`, USB), the 24 TB A8 drive size (`cab_count 3`, volume `CAB24TB`, SATA), and 24 TiB
(`cab_count 4`, the production ceiling and highest cab_count, volume `CAB24TIB`, SATA). Every
run reported `The volume ... appears to be OK` and `The container ... appears to be OK` with
`Verifying allocated space` clean. The APFS kernel extension (`apfs_kext 2332.101.1`)
auto-mounted the containers read-write and committed to them (the 8 TiB and 24 TB volume
superblocks record `last modified by apfs_kext`, checkpoint advanced from the writer's xid 2
to xid 4). The apfsprogs `apfsck` reference checker independently validated `cab_count` 2/3/4
clean at 7.81 TiB, 8 TiB, 16 TiB, the 24 TB A8 drive size, and 24 TiB; targets past the
writer's ~48 TiB ip-bitmap-ring ceiling fail closed. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-cab-tier-cloud\report.json`
plus `apfs-cab-fsck-apfs-clean.png`. The production format/repair cap is raised to 24 TiB
(covering the A8 drive); the multi-CIB/CAB in-place-COW mutation lane stays single-chunk.
Repeated in-place COMMIT on a metadata-overflow container (>~1.3 TiB) was Apple-certified on
2026-06-18 UTC via a unified-group crash-safe rotation. On the overflow tier chunk 0 is fully
reserved, so commits allocate from the boundary chunk (M-1) whose allocation bitmap changes
every commit; cib 0, its chunk-0 bitmap, and that boundary bitmap therefore rotate together as
one group through 3 crash-safe IP slots (stride 2 + 1), and the whole group frees as a single
internal-pool free-queue run. (The single-/multi-CIB tiers reduce to the certified stride-2
{cib0,bitmap} rotation byte-for-byte.) A 2 TiB overflow container (volume `OVF`) took four
chained `commit-image-file-insert` commits (xid 2->6); attached to the qemu/KVM macOS Sequoia
VM the kernel auto-mounted it read-write and CONTINUED it (apfs_kext 2332.101.1, xid 6->8 - the
exact kernel-continuation that the earlier whole-slot multi-CIB rotation failed), and after
unmount `fsck_apfs -n /dev/disk0` reported `The volume /dev/rdisk1s1 ... appears to be OK` and
`The container /dev/disk0 appears to be OK` with the space manager, the space-manager free
queue trees, and allocated space all clean. Evidence
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-cab-tier-cloud\apfs-overflow-repeated-commit-fsck-clean.png`.

Repeated in-place COMMIT on a CAB-tier container (`cab_count > 0`, >~7.8 TiB) was Apple-certified
on 2026-06-18 UTC by extending that unified group with cab 0. On the CAB tier the spaceman
addresses cibs through a cab-address array, so cab 0 (whose `cab_cib_addr[0]` points at the
rotating cib 0) rotates with the group too: each commit re-emits cab 0 into its rotated group
slot pointing at the new cib 0 and re-points the spaceman cab-array at the new cab 0. The group
is therefore {cib0, chunk-0 bitmap, cab0, boundary bitmap} of stride 4 through 3 crash-safe IP
slots (the overflow tier keeps stride 3, single-/multi-CIB stride 2, byte-for-byte). An 8 TiB
container (`cab_count 2`, volume `CABA`) took three chained `commit-image-file-insert` commits
(xid 2->5); attached to the qemu/KVM macOS Sequoia VM the kernel auto-mounted it read-write, read
all three inserted files (`a2cab1.txt`/`a2cab2.txt`/`a2cab3.txt`, contents matching), and
CONTINUED the checkpoint to xid 7, after which `fsck_apfs -n /dev/disk5` reported
`The volume /dev/rdisk5s1 ... appears to be OK` and `The container /dev/disk5 appears to be OK`
(checkpoint xid 7, space manager, free queue trees, object map, fsroot/extent-ref trees, and
allocated space all clean). `apfsck` independently validated the CAB FORMAT and each chained
commit. Evidence
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-cab-tier-cloud\apfs-cab-inplace-commit-fsck-clean.png`
and `...\apfs-cab-inplace-commit-kernel-read-files.png`. The in-place commit cap
(`kApfsInPlaceCommitMaxBytes`) was raised to 32 TiB so the validator accepts CAB sources; the
only remaining fail-closed tier is the >~10 TiB case where the boundary chunk leaves cib 0.

WS3c closed the genuine Apple scattered-internal-pool mutation gap on 2026-08-31. A freshly
Apple-created 256 MiB source and a genuine 18,043,854,848-byte, 135-chunk/2-CIB source each
completed chained insert, patch, replace, and delete commits. Every stage passed host `apfsck
-cw`, Apple `fsck_apfs -n` before mount, exact macOS-kernel read-back, and Apple fsck after the
kernel continued the checkpoint. The same 135-chunk source then shrank to 17,179,869,184 bytes
(128 chunks) without losing the replacement or neighboring file. The foreign path adopts the
source's authenticated CIB, chunk-bitmap, CAB, and free-queue geometry, excludes every live
metadata block, and keeps S.A.K.-formatted media on its byte-identical generated rotation path.
Machine-readable evidence and genuine-byte compact fixtures are in
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apfs-foreign-multicib-cow\report.json`
and `tests\fixtures\apfs\`.

The writer now also formats existing image targets in place only after explicit
destructive wipe confirmation, clears stale edge signatures, writes generated
APFS metadata through a seekable-device block writer, and proves post-format
APFS detection plus empty-root listing in unit coverage. Generated-layout APFS
volume-label change now updates the APSB volume-name field only after
generated-layout verification, restamps the APFS object checksum, reports
old/new labels, and verifies read-back through the APFS browser in local image
coverage. The production
`sak_apfs_writer_cli.exe` bridge exposes generated APFS create/format,
generated-layout checksum repair, and generated-layout root-file write/patch/delete
plus empty root-directory create/delete and generated-layout volume-label change
through queue/apply with raw partition identity, explicit confirmation, helper
JSON output, and refresh evidence. Generated-image root-file
create/replace/byte-range patch/delete and generated-image empty root-directory
create/delete plus generated-image volume-label change remain helper-exposed
with payload/read-back, directory-empty, old/new-label, or negative-read-back
hashes. (Arbitrary Apple APFS mutation was blocked at this I1-era stage; now superseded - the in-place COW engine and A1-A8 driver operations are Apple-certified, and they write Apple-created containers. See the full driver-level capstone below.)
The helper no longer blanket-enables protected, compressed, snapshot, or
multi-volume APFS mutation options; generated APFS v2 single-volume layouts pass
through the generated-layout proof path, while active snapshot/revert metadata,
unsupported incompatible feature flags, and multi-volume containers remain
blocked without future operation-specific proof.
APFS generated-image and generated/raw-target mutation now enforce a
S.A.K.-generated/minimal layout guard before writing: fixed NXSB, object-map,
APSB, root-tree, spaceman headers, checksums, object-map references, root-tree
bounds, and free-space accounting must match the generated layout. The repair
path tolerates one or more bad checksums only on those known generated metadata
objects and scans only the known generated metadata blocks, not arbitrary APFS
objects. The guard also checks that generated spaceman chunk and CIB geometry
matches the certified one-chunk layout before mutation.
HFS+ arbitrary-write work has started with a constrained original-code core
writer/helper slice: image-only data-fork overwrite, data/resource-fork
allocated-block grow/shrink replacement, bounded initial-extent data/resource/fork-backed-attribute
allocation growth, explicit zero-length truncate, constrained empty-file and
empty-folder create/delete, bounded file create with data-fork allocation,
single-leaf catalog rename/move,
allocated-file delete, optional released-block zeroing for file/folder-tree delete,
bounded folder-tree delete, and inline and fork-backed
attribute replacement through `PartitionHfsFileSystemWriter` and
`sak_hfs_writer_cli.exe`. It requires explicit writer enablement, target
confirmation, certification evidence ID, journal override when the source
volume is journaled, compressed-file blocking through `com.apple.decmpfs`, JSON
helper output, catalog or attribute logical-size updates, allocation-bitmap/free-block-counter
read-back for growth, delete/create operations, and secure released-block wipe,
stale-tail zeroing on shrink/truncate,
broad allocation-growth blocking, inline-attribute record-capacity enforcement,
and read-back SHA-256 verification.
The HFS File Partition Manager action now queues the same bounded data-fork,
resource-fork, truncate, empty-file/folder create/delete, bounded file create
with data, single-leaf catalog rename/move, allocated-file delete, optional released-block zeroing, bounded folder-tree delete, inline-attribute,
fork-backed-attribute replacement, and bounded fork-backed-attribute growth
mutation paths for selected HFS+/HFSX raw partitions through HFS logical-size
sparse staging, exact raw-target validation, staged `fsck_hfs` repair/check,
changed-range copyback, Apply review, and operation reports. The physical HFS File Apply proof passed
on pinned expendable USB HFS media at
`artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`;
it staged 262,144 logical HFS bytes, replaced the data fork, resource fork, and
inline `com.apple.FinderInfo` attribute, created/deleted a file and an empty
folder tree with optional secure released-block zeroing, and the refreshed
2026-06-08 UTC physical lane staged a catalog rename before secure delete with
read-back proof. It verified the helper
zeroing warnings plus final root-listing absence, tolerated journaled `fsck_hfs`
exit code 8 only for that explicitly allowed journaled workflow, copied one
sparse range back, and verified raw read-back SHA-256 hashes.
Apple-native HFS+ validation was added on 2026-06-14 UTC in the qemu/KVM macOS Sequoia recovery VM: Apple `newfs_hfs` created a 1 GiB non-journaled HFS+ volume (`SAKHFS`, baseline `fsck_hfs` OK), `sak_hfs_writer_cli.exe create-file-image` (userspace, no kernel driver) added `/sak-cert.txt` (54 bytes) and synchronized the alternate volume header, macOS then auto-mounted the mutated volume and `cat` returned the exact written payload (Apple's HFS+ kernel driver read the S.A.K.-written file byte-for-byte), and `fsck_hfs -f` reported `The volume SAKHFS appears to be OK`. Evidence: `artifacts\partition-manager-certification\vm-lab\apple-tool-evidence\hfs.sak-create-file.kernel-mount-readback-PASS.png` and `hfs.sak-create-file.fsck_hfs-PASS.png`. The created file initially recorded `_unknown` owner and an epoch mtime; the writer was then updated (2026-06-14) to stamp the catalog create/contentMod/attributeMod/access dates with the real time and HFSPlusBSDInfo `owner=0 group=0 fileMode=0o100644` (files) / `0o040755` (folders). Byte-level verification of a freshly created record confirmed `createDate=2026-06-14 07:43:25, owner=0, group=0, mode=0x81a4`, and clang-format/lizard/cppcheck plus the full test suite pass. Bounded initial-extent
data/resource/fork-backed-attribute allocation growth, single-leaf catalog rename/move,
and bounded file create/delete are covered by core/helper tests. (These HFS+ operations - unbounded file/folder create/delete, complex/hard-linked file delete, B-tree split + underflow merge/rebalance, inline/broad attribute growth + overflow records, decmpfs compressed-file writes, and broad allocation growth - were subsequently certified: H1-H8 are Apple-certified incl. the H8 physical-USB gate. See the full driver-level capstone below.)
The supplemental destructive APFS raw-format proof lane is
`scripts/run_partition_manager_apfs_raw_format_validation.ps1`, with
`scripts/launch_partition_manager_apfs_raw_format_validation_local.ps1` for UAC
launch. It requires admin, `-Force`, APFS GPT type, non-boot/non-system target
checks, optional serial/friendly-name pinning, raw-target opt-in, and hardware
evidence flags before formatting an expendable APFS partition. The lane historically
capped the target at 64-128 MiB to keep the generated layout inside the
then-certified one-spaceman-chunk envelope; the A1 multi-CIB/CAB space manager
has since lifted that to a 24 TiB cap. The 2026-06-12 PDT / 2026-06-13 UTC
JMicron run (`run-20260612-192652`, 51,170,148,352-byte target) is retained as
Windows-side evidence only: the macOS recovery kernel later rejected that large
generated APFS target with spaceman checkpoint/container corruption. Current
Windows-side small-target proof passed on JMicron serial `DD56419883A5B`, Disk 2
Partition 2, 134,217,728 bytes, with report
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`.
Apple-native validation has since been completed across the full A1-A8 driver
track, incl. the A8 physical-USB destructive/crash/rollback gate
(`external.apfs-a8-physical`).
The APFS writer CLI self-test now also covers generated-image volume-label
change, old/new-label JSON, APFS browser read-back, and raw volume-label refusal
on normal file paths. Latest local proof:
`artifacts\partition-manager-certification\vm-lab\apfs-cli-self-test\run-d609fd4714b244a38c21c8e9c79b78aa`.
Physical raw APFS volume-label Apply proof is recorded in
`artifacts\file-management-live-certification\disk3-apfs-raw-format\report.json`
from guarded run `run-20260612-192652`; that large-target proof is
Windows-side-only. Current 128 MiB Windows-side raw-format proof is recorded at
`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`;
Apple-native validation has since been completed across the A1-A8 driver track
(`external.apfs-a8-physical`), so this 128 MiB run is retained as I1-era
Windows-side evidence.
The read-only APFS browser uses the same Fletcher-64 object-checksum verifier
for APFS metadata object blocks and fails closed on corrupt metadata checksums;
file extent payload blocks are read as data, not as APFS object metadata.

File Management live cross-filesystem proof is destructive and separate from
the Partition Manager matrix. Current Windows-side live evidence passed through
`scripts\run_file_management_live_filesystem_certification.ps1` on pinned
expendable raw media. APFS passed on JMicron serial `DD56419883A5B`, Disk 2
Partition 2, 134,217,728 bytes, with File Explorer create/write/read,
duplicate-finder, advanced-search, delete-file, negative-read, and
delete-directory proof at
`artifacts\file-management-live-certification\disk2-apfs-script-after-fix\file-management-live-certification.json`.
HFS+ passed on Best Buy serial `DD564198838A8`, Disk 3 Partition 3, with File
Explorer create/write/read, duplicate-finder, advanced-search, rename,
delete-file, negative-read, and delete-directory proof at
`artifacts\file-management-live-certification\disk3-hfs-script-after-fix\file-management-live-certification.json`.

2026-07-12 File Explorer command-route certification (foreign media + HFS+,
Apple-validated). The bridge now derives an APFS container size from its block-0
superblock and write-enables any known-size in-range APFS raw/image target,
S.A.K.-generated and real Apple-created (foreign) alike. Foreign-APFS: the full
destructive command route (create-directory, write-file, byte-exact
read-after-write, duplicate-finder, advanced-search, delete-file,
read-after-delete, delete-directory) passed on a 7.45 TB real Apple-created
newfs_apfs container (Seagate Disk 2 Partition 2) via
`file_management_live_certifier --destructive --allow-foreign-apfs-destructive`,
then Apple-derived `apfsck -cw` on the 7.45 TB partition returned EXIT 0 clean;
evidence
`artifacts\file-management-live-certification\disk2-foreign-apfs-destructive\`.
HFS+: the destructive command route (add rename and read-after-rename) passed on
a 256 MiB Apple-HFS partition (Disk 1) formatted by the bundled `newfs_hfs`, with
bundled `fsck_hfs` and the probe certifier `--hfs-check` clean before and after;
evidence
`artifacts\file-management-live-certification\disk1-hfs-file-management-cert\`.
ext4 read-only browse and byte-exact sample read, plus XFS/Btrfs metadata-only
read-only blockers, passed on WSL-built images at
`artifacts\file-management-live-certification\ro-ext-xfs-btrfs-20260712\`.

Linux compatibility validation for the ext bundle passed on 2026-06-07 UTC
through Arch WSL for ext2, ext3, and ext4. The harness created each image with
the bundled Windows `mke2fs`, grew and shrank it with bundled Windows
`resize2fs`, verified clean Linux `e2fsck`/`dumpe2fs` output, loop-mounted the
image read/write in Linux, wrote `sak-linux-proof.txt`, rechecked clean with
Linux `e2fsck`, and rechecked clean with bundled Windows `e2fsck` after the
Linux write. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext2-linux-validation\report.json`,
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext3-linux-validation\report.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext4-linux-validation\report.json`.
This is certification tooling only and does not add any WSL or Linux runtime
dependency to the portable Windows app.

Linux-created XFS/Btrfs metadata validation passed again on 2026-06-07 UTC through
Arch WSL. The harness created disposable XFS and Btrfs images with `mkfs.xfs`
and `mkfs.btrfs`, then ran `partition_filesystem_probe_certifier.exe` from
Windows against each image with expected file-system and sanity requirements.
S.A.K. detected XFS and Btrfs, captured label/UUID/geometry/counter metadata,
reported sane metadata, and deleted the disposable images. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-metadata-validation\report.json`.
This validates read-only detector metadata used by the in-app original metadata
consistency check. Deep XFS/Btrfs tool checks, format, repair, and write support
remain blocked pending approved Windows-portable tools and operation-specific
proof.
Full arbitrary APFS/HFS+/XFS/Btrfs write support is not certified. Any such
claim requires a separate driver/tooling milestone that covers native
allocation, logs/journals or checkpoints, metadata checksums, snapshots/clones,
compression/encryption where applicable, multi-device or multi-volume state,
crash replay, rollback, VM proof, physical proof, and UI/apply safety gates.

Linux swap format compatibility validation passed on 2026-06-05 PDT through
Arch WSL. The harness created a disposable image, wrote an original SWAPSPACE2
v1 header using the same field layout as the queued app formatter, then verified
the image with S.A.K. `partition_filesystem_probe_certifier.exe`, Linux `blkid`,
and Linux `swaplabel`. All probes reported swap type, label, UUID, version, and
page-size metadata consistently. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-validation\report.json`.
This validates the app's generated Linux swap metadata format without adding a
runtime WSL dependency.

Linux swap raw-partition VM proof passed on 2026-06-05 PDT in
`SAK-PM-Lab-Win11`. The host launcher
`scripts/launch_partition_manager_vm_gate_host.ps1` resolved the real guest
password file from VM unattended metadata, validated `VBoxManage guestcontrol`
as `saklab`, and recorded a redacted host launch report. The elevated runner
created a disposable GPT partition on VirtualBox disk 1, set the Linux swap GPT
type, wrote original SWAPSPACE2 v1 metadata to
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, reread and verified the raw
header, verified S.A.K. raw detection with
`partition_filesystem_probe_certifier.exe`, and cleared disk 1 back to RAW.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.
The host launch report is under
`artifacts\partition-manager-certification\vm-lab\host-launch\`. The original
proof used a keypress UAC fallback on the legacy lab VM; that fallback is now
opt-in only.

A direct-admin automation rerun passed on 2026-06-05 PDT / 2026-06-06 UTC in
`SAK-PM-Automation-Win11`, created by
`scripts/new_partition_manager_automation_vm.ps1`. The VM-creation report
`artifacts\partition-manager-certification\vm-lab\automation-vm\SAK-PM-Automation-Win11-20260605210356.json`
records `direct_admin_guestcontrol_verified=true`. The host launch report
`artifacts\partition-manager-certification\vm-lab\host-launch\linux-swap-20260605220305.json`
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`. The VM gate report remains
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`
with `status=Passed`, `detected_file_system=Linux swap`, raw target
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, exact 134217728-byte probe size,
and cleanup back to RAW. This
raw-media lane is supplemental and is not counted in the imported 18/18
external matrix unless the matrix is expanded with a Linux swap gate.

The current automation-VM creator stages a generated guest password only after an empty,
no-share file receives and passes an exact current-owner ACL check. It refuses existing insecure,
oversized, empty, redirected, or race-planted password files. System32 `icacls.exe` and
VBoxManage exits are captured and enforced; ACL or write failure removes only a file created by
that invocation and reports cleanup failure. Registered PS7 and PS5.1 tests own this host-side
credential-staging contract; this hardening does not change the dated VM certification result.

Filesystem probe helper offset proof was added on 2026-06-05 PDT. The helper
`partition_filesystem_probe_certifier.exe` accepts `--input-offset-bytes`, reads
regular files and Windows raw device paths from that offset, and reports the
offset in JSON. `scripts/test_partition_filesystem_probe_certifier.ps1` creates
a small padded ext2 fixture, validates the offset read, and verifies invalid
offset input fails; the script is wired into CTest and release readiness. A
lab-only Sonoma DMG HFS+ fixture also passed with `--input-offset-bytes
134250496` and sane HFS+ header metadata. The unpacked Tahoe installer contains
a compressed `SharedSupport.dmg` with an HFS+ payload, useful for future
container extraction tests but not a runtime dependency or release asset.

Physical Apple filesystem read-only proof passed on 2026-06-05 PDT /
2026-06-06 UTC against an expendable external USB NVMe GPT disk. The disk was
non-boot and non-system. Partition 2 used Apple APFS GUID
`{7c3457ef-0000-11aa-aa11-00306543ecac}` and passed APFS detection, root
listing, `/Fonts` listing, selected-file read proof, bounded export proof, and
space-manager free-byte reporting through
`\\?\GLOBALROOT\Device\Harddisk2\Partition2`; partition 3 used Apple HFS GUID
`{48465300-0000-11aa-aa11-00306543ecac}` and passed HFS+ detection, root
metadata, and free-byte reporting through
`\\?\GLOBALROOT\Device\Harddisk2\Partition3`. The APFS file proof read
`/.fseventsd/0000000000d34037` at 34163 bytes with SHA-256
`d5b58364da8b5f9e7996bcc9494db9f93a76484fc137e5aa6153809b8935c0d4`. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-filesystem-physical\report.json`.
APFS bounded recursive export proof also passed from `/.fseventsd` on the same
raw-partition alias with 6 files, 0 directories, 6 scanned entries, and
67906 exported bytes. (Corrected 2026-08-30: these figures said the APFS proof read `/Fonts/00TT.TTF` at 25056 bytes with sha `d075a134...` and exported `/Fonts` with 63 files / 1 directory / 64 scanned entries / 21161830 bytes. That file and hash are the HFS+ proof's -- `hfs-read-00tt.json` in the same evidence directory records them against HFS+ -- so an HFS+ read was attributed to APFS. `/Fonts/00TT.TTF` does exist on the APFS partition (`apfs-list-file-candidate-directory.json` lists it at 25056 bytes; the operator copied files to both Apple partitions), but no APFS READ of it with a recorded hash exists, and the `/Fonts` export figures appear in no artifact at all. The APFS proof picks its subject by bounded candidate search, so the final run selected `/.fseventsd` instead. Quote the artifact, not the filename: this proof rewrites its own subject every run.) The latest APFS probe reported `total_bytes=127992487936`
and `free_bytes=127866097664`; the latest HFS+ probe reported
`total_bytes=127724052480` and `free_bytes=125197647872`, listed 9 root
entries, and read
`/polyhavenassets_blendermarket_v1.2.0  (Blender 4.5+).zip` at 185293 bytes
with SHA-256
`72222f9f83d177ea9a2970edb756ab9f3f9f6b00a12f47af63259f215d970709`, then
read inline HFS+ attribute `com.apple.decmpfs` for file ID 1894 at 16 bytes
with SHA-256
`e2aed0d76e90c81c39f9916d56a8f1631c440aec356877ea7dae197b069ea64f`. The
physical Apple report records `validation_requirements` showing HFS file,
HFS attribute, APFS file, and APFS export proofs were required for the latest
run.
The app inventory path now tries those `GLOBALROOT` partition aliases before
using elevated PhysicalDrive offset reads. This proof is read-only and does not
approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth, compressed-file writes,
arbitrary APFS
mutation, non-generated APFS file writes, or APFS resize behavior.

APFS multi-partition physical probe coverage was expanded on 2026-06-07 UTC
using a second expendable non-boot USB disk with five APFS partitions. The
read-only validation command
`scripts\run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber 3 -AllowMissingHfs -ProbeAllApfsPartitions -RequireAllApfsPartitions`
passed, and every APFS partition reported sane nonzero free-space counters
through `\\?\GLOBALROOT\Device\Harddisk3\PartitionN`. This specifically
regresses APFS containers whose space-manager checkpoint object is outside the
initial 2 MiB signature probe. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-apfs-multipart-physical\report.json`.

The first matrix-backed external VM gate was completed on 2026-06-02 09:26 PDT:
`external.bitlocker` - BitLocker locked/unlocked data-volume blocker proof. The
VM lab created a disposable 1 GB NTFS data volume on
VirtualBox disk 2, enabled BitLocker used-space-only encryption, recorded the
locked state, verified a write was blocked while locked, unlocked with the
generated recovery password, verified an unlocked write by SHA-256, recorded a
clean dirty-bit query, redacted the disposable recovery password from host
evidence, and cleared disk 2 back to RAW. This proves the locked/unlocked
BitLocker blocker gate only; in-app BitLocker mutation has a separate passed
gate under `external.bitlocker-mutation`.

The second matrix-backed external VM gate was completed on 2026-06-02 18:00
PDT: `external.file-level-data-recovery` - File-level Data Recovery scan and restore proof.
The VM lab created a disposable NTFS data volume, wrote and
deleted `sak-deleted-recovery-fixture.pdf`, scanned the
raw volume read-only with `FileRecoveryEngine`, restored the matching candidate
to a separate destination from captured scan bytes, verified SHA-256
`31080995DA446C2192A63231DFDA08572DA754EAF9D18DEBDE38CDE27F7D703F`, and
verified the scanned source range was unchanged. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.file-level-data-recovery\report.json`.

The third matrix-backed external VM gate was completed on 2026-06-02 18:38 PDT:
`external.bitlocker-mutation` - In-app BitLocker unlock/suspend/resume mutation proof
on disposable encrypted volume. The VM lab enabled BitLocker used-space-only on
a disposable VBOX data volume, locked it, unlocked it with a generated recovery
password, suspended and resumed protection, verified final protection was on,
sanitized command output, and cleared the lab disk. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.bitlocker-mutation\report.json`.

The fourth matrix-backed external gate was completed on 2026-06-02 20:19 PDT:
`external.allocate-free-space` - Allocate Free Space adjacent donor-volume backup/delete/extend/recreate/restore proof.
The elevated disposable-VHD lab created adjacent NTFS source and donor volumes,
backed up donor files off the VHD, deleted the
donor partition, extended the source by 128 MiB, recreated the donor from the
remaining space, restored donor data, verified a three-file SHA-256 manifest,
ran clean repair scans on both volumes, and dismounted/removed the VHD.
Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.allocate-free-space\report.json`.

The fifth matrix-backed external gate was completed on 2026-06-02 21:03 PDT:
`external.cluster-size-change` - Existing-volume cluster-size change proof.
The elevated disposable-VHD lab created an NTFS volume, backed up fixture data
with file hashes, ACLs, and an alternate data stream, reformatted the same
volume from 4096-byte to 16384-byte allocation units, restored data, matched
the SHA-256 manifest, preserved four ACL entries and one alternate data stream,
ran a clean repair scan, and removed the VHD. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cluster-size-change\report.json`.

The sixth matrix-backed external VM gate was completed on 2026-06-02 22:41 PDT
/ 2026-06-03 UTC: `external.hdd-defrag-execution` - Direct in-app HDD defrag
execution proof. The elevated Windows 11 VirtualBox lab targeted only
disposable 4 GB `VBOX HARDDISK` disk 1 after boot/system/model/size guards,
created an NTFS fixture volume, ran `Optimize-Volume -Analyze` and
`Optimize-Volume -Defrag`, ran `Repair-Volume -Scan` with no file-system
problems, and cleared the disk back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.hdd-defrag-execution\report.json`.

The final boot-action external gates were completed on 2026-06-03 PDT.
`external.os-migration-reboot` booted a target-only cloned OS VM to Guest
Additions run level 3. `external.boot-repair-uefi` and
`external.boot-repair-bios` both captured intentionally broken BCD symptoms and
verified offline repair back to run level 3. `external.system-mbr2gpt` validated
and converted disposable BIOS/MBR disk 0 with `mbr2gpt` exit code 0, changed
the disk to GPT with an EFI System partition, and booted the converted disk in
fresh EFI VM `SAK-PM-MBR2GPT-EFI-Boot-20260603-1140` to Guest Additions run
level 3. The passed report is
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.system-mbr2gpt\report.json`.

Commercial parity wording remains evidence-scoped. Current AOMEI and MiniTool
feature pages still include operations whose direct execution is not certified
here, including dynamic/basic conversion, primary/logical conversion,
non-adjacent move-location/allocation, live-drive file recovery, SSD secure
erase, boot/reboot proof, removable-media proof, and physical wipe proof.
S.A.K. now has production code for adjacent-donor Allocate Free Space,
existing-volume cluster-size backup/reformat/restore/hash verification, direct
HDD defrag execution, and BitLocker mutation paths, but release notes must not call those destructive
paths 100% hardware-certified until strict VHD plus all external gates pass.

### Full driver-level APFS + HFS+/HFSX write - complete (A1-A8 / H1-H8)

As of 2026-06-28, both Apple filesystem write tracks are full driver-level
Apple-certified, each milestone proven by Apple `fsck_apfs` / `fsck_hfs` and
macOS-kernel mount in the qemu/KVM macOS Sequoia VMs, and the two track gates by
physical-USB destructive + crash-interruption + rollback on disposable media.
The capability-status owner is
`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`; the section 3 table in
`docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md` mirrors it, and the per-milestone narrative above is a
chronological record, and its A1/A2-era "remains blocked" / "remains
uncertified" / "rows A-b..A-h remain open" conclusions were accurate when
written and are closed by the milestones below. This Apple-filesystem driver
certification is a distinct scope from the Partition Manager destructive-operations
claim level tracked elsewhere in this document.

**APFS:** A1 multi-CIB / multi-chunk space manager (single-CIB + CAB tier to a
24 TiB cap, plus genuine Apple scattered-IP 135-chunk/2-CIB mutation and shrink;
`external.apfs-cab-tier-cloud`, `external.apfs-foreign-multicib-cow`); A2 in-place crash-safe COW
checkpoint mutation of files + directories incl. cross-directory move and
object-id-preserving byte-range patch, wired through both the File Management
bridge and the Partition Manager queue (`external.apfs-fulltree-cow-mutations`,
`external.apfs-cow-directory-mutations`, `external.apfs-cow-file-move`,
`external.apfs-cow-file-patch`, `external.apfs-metadata-overflow-deadzone`,
`external.apfs-production-raw-physical`); A3 snapshots create/delete/revert
(`external.apfs-cow-snapshot-create` / `-delete` / `-revert`, revert completed
on a real Mac); A4 multi-volume containers (`external.apfs-multi-volume`); A5
inline zlib `com.apple.decmpfs` compression (`external.apfs-compression`); A6
credential-gated FileVault encryption (`external.apfs-encryption`); A7 clones /
sparse files / hard-links / xattr-ACL / cross-chunk container grow and shrink
(`external.apfs-a7-kernel-cert`, `external.apfs-a7-raw-physical`,
`external.apfs-foreign-multicib-cow`); A8 the
physical-USB full-driver gate - a S.A.K. multi-volume + snapshot container on a
disposable 7.3 TiB USB: the macOS kernel mounted both volumes, read the file,
and enumerated the snapshot; Apple `fsck_apfs` was clean; then a kernel write +
qemu-kill power-loss + reboot survived byte-exact with post-crash `fsck_apfs`
clean (`external.apfs-a8-physical`).

**HFS+/HFSX:** H1 special-file node-pool growth; H2 streaming depth/width-general
catalog B-tree (synthetic + a real Apple-written catalog); H3 underflow
merge/rebalance; H4 streaming multi-leaf extents-overflow + fragmenting
allocator (`external.hfs-multi-leaf-extents`); H5 hard-links / symlinks /
complex delete (`external.hfs-hardlinks-symlinks`); H6 attribute overflow
records + broad attribute growth (`external.hfs-attribute-overflow-multileaf`);
H7 big-endian journal replay + embedded HFS-wrapper write edge
(`external.hfs-bigendian-journal-wrapper`); H8 the physical-USB full-driver
gate - Apple `newfs_hfs` journaled volume on the disposable USB, S.A.K. mutate,
kernel auto-mount + read + Apple `fsck_hfs` clean, then kernel write + qemu-kill
+ reboot journal-replayed and the file survived byte-exact, fsck clean again
(`external.hfs-h8-physical`).

I1 wired every capability through the production Apply route (16/16) with the
FileVault secret materialized to an owner-scoped temp credential file at
execution and shred-deleted, never on a command line. Fail-closed by design (not
defects): APFS Fusion/Tier2 multi-device is out of scope (no rig); encrypted
volumes require the user credential; sealed/signed system volumes and case-sensitive
volumes are refused; APFS container shrink and chunk-adding grow are documented
follow-ons. (Corrected 2026-11-01: this said sealed volumes had a typed override and that
Apply was limited to generated layouts. Neither is in the code.)
