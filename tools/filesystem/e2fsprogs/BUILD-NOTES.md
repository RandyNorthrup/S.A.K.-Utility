# e2fsprogs bundle notes

Bundle scope:

- Tool: `e2fsck.exe`
- Tool: `mke2fs.exe`
- Tool: `resize2fs.exe`
- Version: e2fsprogs 1.47.4
- Approved manifest operations:
  - `check-read-only`: `e2fsck -n -f <target>`
  - `repair`: `e2fsck -p -f <target>` after destructive confirmation
  - `format`: `mke2fs -t <ext2|ext3|ext4> -F [-L <label>] <target>` after destructive confirmation
  - `resize`: `resize2fs <target>` after destructive confirmation
- Supported filesystems: ext2, ext3, ext4

This bundle is built from the upstream e2fsprogs 1.47.4 source archive with
`e2fsprogs-1.47.4-sak-mingw.patch` applied. The patch ports only the command
frontend pieces needed for a MinGW Windows tool bundle:

- use the libext2fs Windows/default I/O backend instead of the Unix I/O manager
- skip Unix-only signal, battery, resource, and fork-based logging paths on Windows
- guard unavailable POSIX headers
- use `NUL` instead of `/dev/null`
- force the bundled `sort_r` fallback on MinGW
- avoid MinGW `fstat`-driven image truncation in the Windows zeroout path
- preserve Windows raw-device paths containing `?` (`\\.\`, `\\?\`, `//./`,
  `//?/`) through e2fsck, resize2fs, and libext2fs option parsing
- open Windows raw-device targets with shared read/write/delete handles so
  follow-up size probes can inspect the same unmounted partition
- avoid `resize2fs` self-conflicting exclusive opens for regular image files

Scratch build command used for this bundle:

```bash
CFLAGS='-O2' LDFLAGS='-static' ../configure \
  --host=x86_64-w64-mingw32 \
  --build=x86_64-pc-msys \
  --prefix="$work/install-win" \
  --disable-nls \
  --disable-fuse2fs \
  --disable-defrag \
  --disable-uuidd \
  --disable-debugfs \
  --disable-imager \
  --disable-fsck \
  --disable-e2initrd-helper \
  --disable-testio-debug

sed -i 's/lib\/ss //g; s/ tests\/progs//g' Makefile
find . -name Makefile -o -name MCONFIG | xargs sed -i 's/RDYNAMIC = -rdynamic/RDYNAMIC =/'
make -j4 libs
make -j4 -C e2fsck e2fsck
make -j4 -C misc mke2fs
make -j4 -C resize resize2fs
```

Smoke proof used before approval:

- `e2fsck.exe -V` runs on Windows
- `mke2fs.exe -t ext4 -F` formats a disposable 64 MiB image without truncating it
- `e2fsck.exe -n -f` returns exit code 0 against the disposable ext4 image
- after growing the image to 96 MiB, `resize2fs.exe` expands the file system to
  the new container size
- `e2fsck.exe -n -f` returns exit code 0 against the resized image
- destructive VM proof on 2026-06-05 passed against disposable
  `\\?\GLOBALROOT\Device\Harddisk1\Partition2`: ext4 format, clean repair,
  read-only check, partition grow, `resize2fs`, post-grow read-only check, and
  RAW cleanup
- Linux validation proof on 2026-06-05 passed through Arch WSL:
  Windows-bundled `mke2fs` created ext4, Windows-bundled `resize2fs` grew it,
  Linux `e2fsck`/`dumpe2fs` verified clean metadata, Linux loop mount wrote
  `sak-linux-proof.txt`, Linux `e2fsck` rechecked clean, and bundled Windows
  `e2fsck` rechecked clean after the Linux write
- `objdump -p` shows only Windows/UCRT API-set imports, no bundled companion DLLs

The manifest approves the tool binaries and command shapes. Partition Manager
now wires ext format, repair, and grow through Pending Operations, safety
validators, Apply review, elevated PowerShell execution, manifest/hash
revalidation, and explicit confirmation. ext shrink remains blocked until a
separate shrink-specific destructive certification path exists. Read-only ext browse, selected-file extract, and
bounded directory export are provided by original S.A.K. parser code, not by
this external tool bundle.
