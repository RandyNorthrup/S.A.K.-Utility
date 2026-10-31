# hfsprogs 540.1.linux3 S.A.K. Build Notes

This bundle provides portable HFS+/HFSX format and repair tools for Partition
Manager:

- `newfs_hfs.exe` for confirmed HFS+/HFSX format.
- `fsck_hfs.exe` for read-only check and confirmed repair.

Source inputs:

- `hfsprogs_540.1.linux3.orig.tar.gz`
- `hfsprogs_540.1.linux3-6.debian.tar.xz`
- `hfsprogs-540.1.linux3-sak-msys.patch`

The S.A.K. patch adds MSYS stand-ins for `<sys/sysctl.h>`, `<linux/sysctl.h>`
and the `<sys/ioctl.h>` request macros, and `include/sak_sha1.h`. `newfs_hfs`
uses SHA-1 only to hash local entropy into the new volume's identifier, so the
patch points `makehfs.c` at that header and drops `-lcrypto`: the bundle has no
OpenSSL runtime to keep patched or to reach end of life.

Build environment:

- MSYS2 base under `tools/filesystem/_build/msys64`
- MSYS GCC 15.2.0
- libutil-linux-devel headers for UUID support

Build commands:

```sh
tar -xzf hfsprogs_540.1.linux3.orig.tar.gz
cd diskdev_cmds-540.1.linux3
tar -xf ../hfsprogs_540.1.linux3-6.debian.tar.xz
for p in $(cat debian/patches/series); do patch -N -p1 < debian/patches/$p || true; done
patch -p1 < ../hfsprogs-540.1.linux3-sak-msys.patch
make CC=gcc
objdump -p newfs_hfs.tproj/newfs_hfs.exe | grep 'DLL Name'   # msys-2.0.dll and KERNEL32.dll only
```

The builds are not bit-reproducible (the PE header carries a link timestamp).
`newfs_hfs.exe` was rebuilt 2026-10-30 to drop OpenSSL; `fsck_hfs.exe`, whose
sources the patch does not touch, is the earlier build.

Image-level smoke proof:

```sh
truncate -s 64M /tmp/sak-hfsprogs-smoke.img
./newfs_hfs.tproj/newfs_hfs -v SAK_HFS_SMOKE /tmp/sak-hfsprogs-smoke.img
./fsck_hfs.tproj/fsck_hfs -fy /tmp/sak-hfsprogs-smoke.img
partition_filesystem_probe_certifier.exe --input temp/sak-hfsprogs-smoke.img --output temp/sak-hfsprogs-smoke.probe.json --expect HFS+ --hfs-check
```

The same image-level proof also passed with the bundled executables run from
`tools/filesystem/hfsprogs` against a native Windows path.
