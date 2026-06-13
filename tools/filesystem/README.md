# Partition Filesystem Tools

This directory is the only approved portable bundle path for Partition Manager
non-native filesystem tools.

Current state:

- `manifest.json` approves e2fsprogs 1.47.4 tools for ext2/ext3/ext4:
  `e2fsck` for `check-read-only` and `repair`, `mke2fs` for `format`, and
  `resize2fs` for `resize`.
- `manifest.json` approves hfsprogs 540.1.linux3-6+sak-msys tools for
  HFS+/HFSX: `newfs_hfs` for `format` and `fsck_hfs` for `check-read-only`
  and `repair`.
- `e2fsck.exe`, `mke2fs.exe`, and `resize2fs.exe` are bundled under
  `tools/filesystem/e2fsprogs/` with the upstream source archive, local MinGW
  patch, upstream notice, and build notes.
- `newfs_hfs.exe` and `fsck_hfs.exe` are bundled under
  `tools/filesystem/hfsprogs/` with the Debian hfsprogs source archive,
  Debian patch archive, local MSYS portability patch, runtime DLL license
  texts, upstream notice, and build notes.
- Tool binaries and command shapes are approved. ext2/ext3/ext4 format, repair,
  grow, and shrink workflows are wired through Pending Operations with confirmation,
  safety validation, Apply review, elevated PowerShell execution, and
  manifest/hash revalidation. HFS+/HFSX format and repair are also wired
  through Pending Operations with sparse staging, raw-target identity, and the
  same confirmation model; read-only HFS checks stay on original parser reports.
  ext format, repair, grow, and shrink have destructive VM proof on disposable
  raw media; HFS+/HFSX image proof and guarded physical raw-partition
  destructive proof are recorded.
  XFS/Btrfs/APFS write paths remain blocked until operation-specific
  VM/destructive certification or tool proof is complete.
  The ext destructive VM proof runner is
  `scripts/run_partition_manager_ext_filesystem_vm_gate.ps1`; its launcher is
  `scripts/launch_partition_manager_ext_filesystem_vm_gate_local.ps1`.
  Parser-backed read-only browse, extract, and bounded directory export for
  ext2/ext3/ext4 and HFS+/HFSX live in the app code and do not require an
  external runtime tool.

Bundle rules:

- No runtime download, installer, service, driver install, WSL dependency, or
  package-manager dependency.
- Each tool must live under `tools/filesystem/<tool-id>/`.
- Each executable must be listed by `manifest.json` with version, upstream URL,
  license, source archive SHA-256, relative binary path, binary SHA-256,
  supported file systems, and supported operations.
- Each companion DLL/license/source/helper artifact must be listed in the
  owning tool's `runtime_files` array with relative path and SHA-256.
- Any extra shipped file under `tools/filesystem/` fails release readiness unless
  it is top-level `manifest.json`, top-level `README.md`, or manifest-listed.
  The top-level `_build/` scratch directory is ignored by readiness and removed
  from build/staged output.
- `THIRD_PARTY_LICENSES.md` must include the same tool id, display name,
  upstream URL, license, and `tools/filesystem` bundle path before release.
- Build output and portable release staging copy this directory as part of the
  normal `tools/` bundle.

Selected first target:

- ext2/ext3/ext4 read-only checks via e2fsprogs `e2fsck -n -f`.
- ext2/ext3/ext4 confirmed format through `mke2fs`, confirmed repair through
  `e2fsck -p`, and confirmed grow/shrink through `resize2fs`. The destructive
  VM gate passed on 2026-06-05 and writes evidence under
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext-filesystem-write/report.json`.
  A separate Linux compatibility gate passed on 2026-06-05 under
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.ext-linux-validation/report.json`:
  Linux e2fsck/dumpe2fs accepted the Windows-created/grown/shrunk ext4 image,
  Linux mounted it through a loop device, wrote a fixture file, and Windows
  e2fsck rechecked the Linux-written image clean.
- ext2/ext3/ext4 parser-backed read-only browse, selected-file extract, and
  bounded recursive directory export.
- HFS+/HFSX confirmed sparse-staged format through bundled `newfs_hfs` and
  confirmed sparse-staged repair through `fsck_hfs -p -f`. Image-level
  validation formats a disposable image, checks it, and verifies S.A.K. raw
  detection with `partition_filesystem_probe_certifier.exe`; guarded physical
  raw-partition proof writes sparse image metadata ranges to expendable media
  and probe-verifies HFS+/HFSX after each stage.
- XFS/Btrfs parser-backed read-only superblock metadata with lightweight
  counter/geometry sanity notes. Runtime check/format support remains blocked
  until Windows-portable xfsprogs/btrfs-progs builds are proven and
  manifest-approved. Linux-created XFS/Btrfs image validation passed through
  `partition_filesystem_probe_certifier.exe` and
  `scripts/run_partition_manager_linux_metadata_validation.ps1`; this is
  detector evidence only, not runtime checker approval.
- APFS parser-backed read-only container superblock metadata with lightweight
  block-geometry sanity notes, plus read-only browse/extract/export for normal
  unencrypted/uncompressed files. Write support remains blocked.
- HFS+/HFSX parser-backed read-only catalog browse, data/resource-fork extract,
  selected attribute-value extract, and bounded recursive directory export with
  resource forks written as `.rsrc` sidecars. Attribute value writes remain
  blocked.
- Do not approve stale Cygwin e2fsprogs builds for production write/check
  support. Current Cygwin package metadata showed e2fsprogs 1.44.5-1 and
  `System Unmaintained`; build or source-pinned newer tooling first.

Deferred targets:

- XFS/Btrfs read-only checks after a Windows-portable toolchain is proven.
- Btrfs repair and APFS write support stay blocked by default.
- HFS+ file/folder writes and attribute-value writes stay blocked until a
  separate original-code or tool-backed write workflow is designed and certified.
