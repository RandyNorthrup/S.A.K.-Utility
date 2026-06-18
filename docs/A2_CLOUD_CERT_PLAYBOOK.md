# A2 Cloud Certification Playbook (bare-metal hourly)

Purpose: finish the **paused A2 tiers** (multi-block "2-block" spaceman, CAB tier,
repeated-overflow-commit, dstream-xfield harvest) on a rented **bare-metal hourly**
Linux box using a **sparse** large virtual disk + the existing qemu macOS Sequoia VM —
**without** a physical multi-TB drive. The physical 24 TB drive stays reserved for the
**A8** hardware gate (real-device destructive + crash + rollback), which a sparse image
cannot satisfy.

## Why this works (the core trick)

APFS `format` + in-place `commit` write **only metadata, concentrated near the start of
the container** (NXSB, checkpoint area, spaceman + CIBs, container/volume omap, fstree,
and the per-chunk bitmaps for the *allocated* metadata region). Free chunks are *tracked*
by the spaceman but never written. So a 24 TB container's actual written bytes are well
under ~1 GB. A **sparse image** that *presents* as 24 TB but only stores the written
blocks is therefore equivalent, for the geometry/kernel/fsck cert, to a real 24 TB device.

The local wall was **ext4's 16 TiB max file size** (`truncate -s 24T` → "File too large").
The fix is a host filesystem with a large max file size — **XFS (max file 8 EiB)** — on a
*small* disk. You are paying for a small disk + a few hours of bare metal, not 24 TB of
storage.

## 0. What you need

- A bare-metal **hourly** host with hardware virtualization (VT-x/AMD-V):
  - Hetzner (dedicated, hourly via auction/cloud-metal), Equinix Metal, Vultr Bare Metal,
    OVH/So you Start, Latitude.sh — any x86_64 metal with `/dev/kvm`.
  - Specs: ≥ 8 cores, **≥ 32 GB RAM** (macOS VM ~8 GB + host headroom), **≥ 200 GB**
    local SSD/NVMe (host OS + ~10 GB macOS VM images + the sparse image's *actual* bytes).
    No large disk required.
- The repo (this checkout) reachable from the host (git clone or `scp`).
- The existing macOS VM assets from your WSL box (fastest): `/root/OSX-KVM`
  (OVMF, `OpenCore/OpenCore.qcow2`, `BaseSystem.img`) plus the driver scripts
  `sak-vm.sh`, `sak-click.sh`, and a `sak-boot-*.sh`. ~3.5 GB, `scp`-able.

> EULA note: running macOS on non-Apple hardware is against Apple's license (gray area for
> testing — identical to your current local qemu-macOS). The only license-clean option is a
> real cloud Mac (AWS EC2 Mac), which is far pricier and awkward to give a huge disk.

## 1. Provision + base host

```bash
# On the rented metal (Ubuntu/Debian example)
sudo apt-get update
sudo apt-get install -y qemu-system-x86 qemu-utils ovmf socat python3 \
    git cmake ninja-build build-essential xfsprogs \
    qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev
ls -l /dev/kvm                      # must exist
egrep -c '(vmx|svm)' /proc/cpuinfo  # must be > 0
```

(Arch host: `pacman -S qemu-full edk2-ovmf socat python git cmake ninja base-devel xfsprogs qt6-base`.)

## 2. XFS scratch for the sparse image (lifts the 16 TiB ext4 limit)

If the host's main disk is ext4, make a small **XFS** filesystem just for the image. A
100 GB XFS loop is plenty (the sparse 24 TB image stores < 1 GB of real bytes):

```bash
sudo truncate -s 100G /xfsstore.img
sudo mkfs.xfs -q /xfsstore.img
sudo mkdir -p /xfs
sudo mount -o loop /xfsstore.img /xfs
# Sanity: XFS allows the huge sparse file ext4 refused
truncate -s 24T /xfs/a2.img && du -h /xfs/a2.img   # logical 24T, actual ~0
```

(Or, if the host gives you a raw spare disk, `mkfs.xfs /dev/sdX` and mount that instead.)

## 3. Build the S.A.K. APFS writer CLI on the host

`sak_apfs_writer_cli` pulls only portable sources (`partition_apfs_writer.cpp` and the CLI
have zero Windows guards; only `partition_raw_device_io.cpp` has Windows-specific code, and
**file-image** ops use Qt's cross-platform `QFile` path). So a Linux Release build formats
sparse **file** images fine.

```bash
git clone <your-remote-or-scp> sak && cd sak
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target sak_apfs_writer_cli
./build/sak_apfs_writer_cli --help
```

If the **full** repo `CMakeLists.txt` fails to configure on Linux (it carries Windows-only
GUI/`windeployqt` targets), build the CLI standalone with a throwaway `CMakeLists.txt`
(does not modify the repo) — it needs only these five sources plus `Qt6::Core`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(sak_apfs_cli LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(sak_apfs_writer_cli
  src/tools/sak_apfs_writer_cli.cpp
  src/core/partition_apfs_file_system_reader.cpp
  src/core/partition_apfs_writer.cpp
  src/core/partition_file_system_detector.cpp
  src/core/partition_raw_device_io.cpp)
target_include_directories(sak_apfs_writer_cli PRIVATE include)
target_link_libraries(sak_apfs_writer_cli PRIVATE Qt6::Core)
```

```bash
cmake -S . -B clibuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=
# (point -S at a dir containing the throwaway CMakeLists + the repo's src/ and include/)
```

## 4. Stage the macOS VM

Fastest — copy your working VM from the WSL box:

```bash
# from the WSL/Windows side
scp -r /root/OSX-KVM root@<metal-host>:/root/OSX-KVM
scp /root/sak-vm.sh /root/sak-click.sh root@<metal-host>:/root/
```

Or rebuild from scratch: `git clone https://github.com/kholia/OSX-KVM`, run its
`fetch-macOS-v2.py` (pick Sequoia), `qemu-img convert BaseSystem.dmg -O raw BaseSystem.img`,
and use its OpenCore image. Confirm a one-time recovery boot before continuing.

## 5. Format the large sparse container (host CLI)

Pick the tier you are exercising (the writer's own geometry gate is the precise authority):

```bash
# 2-block / multi-block spaceman tier (the "full 4 TB single container", ~3-8.57 TiB)
./build/sak_apfs_writer_cli format-raw --target /xfs/a2.img \
    --size-bytes 4398046511104 --volume-name A2SPM --confirm-target   # 4 TiB

# CAB tier (cib-address array no longer fits inline; > 8.57 TiB)
./build/sak_apfs_writer_cli format-raw --target /xfs/a2.img \
    --size-bytes 10995116277760 --volume-name A2CAB --confirm-target  # 10 TiB
```

> Reality check: the CAB tier, multi-block spaceman, and repeated-overflow-commit are
> currently **fail-closed in the writer** (unimplemented), so `format-raw`/commit will
> *reject* them until you implement those tiers. This box is the **develop + certify**
> environment for that code — not a one-shot "run existing cert." `dstream-xfield` is a
> harvest task (read Apple's `xf_used_data` flag value, `0x08` vs `0x20`).

## 6. Attach to the macOS VM + run the Apple oracle

Add the sparse image as a raw disk in the boot script:

```text
-drive id=A2,if=none,file=/xfs/a2.img,format=raw,cache=unsafe \
-device ide-hd,bus=sata.0,drive=A2 \
-qmp unix:/tmp/sak-qmp.sock,server,nowait \
-monitor unix:/tmp/sak-mon.sock,server,nowait
```

```bash
cd /root/OSX-KVM && bash sak-boot-a2cloud.sh > /root/qemu.log 2>&1 &
# drive recovery -> Terminal with sak-vm.sh / sak-click.sh (see the H2 session notes):
#   diskutil list                      # find the A2 disk (diskN)
#   /sbin/fsck_apfs -n /dev/diskN      # Apple fsck_apfs = the cert oracle
#   diskutil apfs unlockVolume / mount # kernel mount (read the inserted file, RW touch)
```

For the in-place commit tiers (repeated-overflow etc.), follow the existing A2 pattern:
mutate the image with the host CLI commit command, re-attach, re-run `fsck_apfs` + kernel
mount, confirm the kernel **continues** the S.A.K. checkpoint ring (xid advance) clean.

## 7. Evidence + teardown

```bash
# screendump via the monitor, like the existing apple-tool-evidence images
printf 'screendump /root/shot.ppm\n' | socat - UNIX-CONNECT:/tmp/sak-mon.sock
# copy fsck_apfs output + shots back, drop into artifacts/.../apple-tool-evidence
scp root@<metal-host>:/root/shot.ppm ./
# DESTROY the instance so hourly billing stops
```

## 8. Cost + scope

- Bare metal hourly: roughly **$0.5–2/hr**; a tier session is a few hours → a few dollars.
- Disk: a small XFS scratch (~100 GB) — you are **not** buying 24 TB of cloud storage.
- This unblocks **A2's paused engine tiers**. It does **not** replace **A8**: the final
  HFS/APFS hardware gate still needs a real physical multi-TB device for destructive +
  crash-interruption + rollback proof. Order the 24 TB drive only for A8.

## Quick reference — sizes

| Tier | Size to format | Notes |
|------|----------------|-------|
| multi-CIB (already certified) | up to ~1 TiB | repeated commits OK |
| metadata-overflow (certified) | ~1.3–3 TiB | single insert-commit certified |
| 2-block / multi-block spaceman | "full 4 TB" ~3–8.57 TiB | writer fail-closed today |
| CAB tier | > 8.57 TiB (e.g. 10 TiB) | writer fail-closed today |
| repeated-overflow-commit | overflow size, ≥2 commits | boundary-chunk bitmap rotation |
| dstream-xfield harvest | any | read Apple `xf_used_data` 0x08 vs 0x20 |
