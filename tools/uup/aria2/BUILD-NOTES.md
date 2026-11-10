# aria2 S.A.K. Build Notes

`tools/uup/aria2c.exe` downloads UUP sets and Linux ISOs (`src/core/uup_iso_builder.cpp`,
`src/core/linux_iso_downloader.cpp`). It is aria2 1.37.0, the latest aria2 release, built by
S.A.K. Utility from the sources in this folder, because the official Windows build
(`aria2-1.37.0-win-64bit-build1.zip`, 2023-11-15) embeds libraries that have since had security
releases. Every file here and `aria2c.exe` itself are pinned by SHA-256 in
`scripts/bundle_uup_tools.ps1`.

## What is in aria2c.exe

| Component | Version | License | Source tarball here |
|---|---|---|---|
| aria2 | 1.37.0 | GPL-2.0-or-later | `aria2-1.37.0.tar.xz` |
| wslay (bundled in aria2's `deps/`, WebSocket for RPC) | 1.1.1 | MIT | `aria2-1.37.0.tar.xz` |
| zlib | 1.3.2 | zlib | `zlib-1.3.2.tar.xz` |
| c-ares | 1.34.9 | MIT | `c-ares-1.34.9.tar.gz` |
| Expat | 2.9.0 | MIT | `expat-2.9.0.tar.xz` |
| SQLite | 3.54.1 | public domain | `sqlite-autoconf-3540100.tar.gz` |
| libssh2 | 1.11.1 (WinCNG crypto backend) | BSD-3-Clause | `libssh2-1.11.1.tar.xz` |
| MinGW-w64 CRT startup code and winpthreads | 14.0.0.r569.g34eeafb7d | MinGW-w64 runtime licenses (ZPL, MIT, BSD-style) | MSYS2 UCRT64 toolchain |
| libgcc, libstdc++ | GCC 16.2.0 | GPL-3.0 with the GCC Runtime Library Exception | MSYS2 UCRT64 toolchain |

Each is the latest release on 2026-10-31. Everything is linked statically, so `aria2c.exe` needs
no DLL beside it: it imports only Windows system DLLs and the UCRT API sets that ship with
Windows 10 and 11. `NOTICE` holds every license text.

TLS goes through Windows (aria2's WinTLS backend: Schannel and the Windows certificate store),
exactly as in the official build: aria2 is configured without OpenSSL and GnuTLS. libssh2 uses
Windows CNG for its cryptography. Message digests use aria2's internal implementation (as in the
official build), and BitTorrent's Diffie-Hellman and ARC4 use aria2's internal implementations;
the official build links GMP for the Diffie-Hellman bignum instead, which this build leaves out
(one less library and its LGPL obligations; BitTorrent stays enabled). Metalink and XML-RPC use
Expat, as in the official build. NLS is off.

## Source provenance

All tarballs were downloaded on 2026-10-31 from the projects' own release pages:

| Tarball | SHA-256 | Checked against |
|---|---|---|
| `aria2-1.37.0.tar.xz` | `60A420AD7085EB616CB6E2BDF0A7206D68FF3D37FB5A956DC44242EB2F79B66B` | aria2 publishes no signature for 1.37.0. The SHA-256 matches MSYS2's `mingw-w64-aria2` PKGBUILD and the SHA-512 matches Arch Linux's `aria2` PKGBUILD. |
| `zlib-1.3.2.tar.xz` | `D7A0654783A4DA529D1BB793B7AD9C3318020AF77667BCAE35F95D0E42A792F3` | Good signature by Mark Adler, `5ED46A6721D365587791E2AA783FCD8E58BCAFBA`; matches GitHub's release digest |
| `c-ares-1.34.9.tar.gz` | `9364B70A12EB59A06830FB488D9357F90100DFF0D98B23EA3593A183D43E6F2F` | Good signature by Brad House, primary key `DA7D64E4C82C6294CB73A20E22E3D13B5411B7CA`; matches GitHub's release digest |
| `expat-2.9.0.tar.xz` | `1E6371862CC31999B368C3B89B49994F0677E1BAB5F1B2B85AE3741F5D803051` | Good signature by Sebastian Pipping, primary key `3176EF7DB2367F1FCA4F306B1F9B0E909AF37285`; matches GitHub's release digest |
| `sqlite-autoconf-3540100.tar.gz` | `926EF8D3EAB93FBF6E06011901273EEC076B25594A4E1CDADD50C987F4F6D64E` | SQLite publishes SHA3-256 `b6ced585c750916b1052667239a12c25a7ff34e7f06e1e6e01ecb0427657d9c4` on its download page; it matches |
| `libssh2-1.11.1.tar.xz` | `9954CB54C4F548198A7CBEBAD248BDC87DD64BD26185708A294B2B50771E3769` | Good signature by Daniel Stenberg, `27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2` |

The signing-key fingerprints were taken from MSYS2's PKGBUILDs for the same packages
(`validpgpkeys`), not from the signatures themselves.

## Rebuilding

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_aria2.ps1 [-Install]
```

`scripts/build_aria2.ps1` verifies every tarball (and any `aria2-*-sak-*.patch`; there is none)
against its pinned SHA-256, checks that the MSYS2 UCRT64 toolchain matches the pinned package
versions, runs `build_aria2.sh` (this folder) in that environment at BelowNormal priority with
two make jobs, refuses an `aria2c.exe` that imports anything but Windows system DLLs, and prints
its SHA-256 and `aria2c --version`. `-Install` copies it to `tools/uup/aria2c.exe`.

The build environment is the repo's MSYS2 at `tools/filesystem/_build/msys64` (gitignored; the
same one the e2fsprogs and hfsprogs bundles use) with:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf make
```

Toolchain used: `mingw-w64-ucrt-x86_64-gcc` 16.2.0-6, `-binutils` 2.47-4, `-crt`, `-headers`
and `-winpthreads` 14.0.0.r569.g34eeafb7d-1, `-pkgconf` 1~3.0.10-1. The UCRT64 environment was
chosen over MINGW64: UCRT ships with Windows 10 and 11, and MSYS2 builds its own aria2 package
only for UCRT64 and the clang environments.

`build_aria2.sh` builds each library as a static library into a private prefix with
`-O2 -g0 -ffile-prefix-map=<work>=.`, links with `-Wl,--no-insert-timestamp`, and sets
`SOURCE_DATE_EPOCH=1793404800` (2026-10-31T00:00:00Z), which is the build date
`aria2c --version` prints:

- zlib: `./configure --static`
- c-ares: `./configure --disable-shared --enable-static --disable-tests`
- Expat: `./configure --disable-shared --enable-static --without-docbook --without-examples --without-tests --without-xmlwf`
- SQLite: `./configure --disable-shared --enable-static --disable-readline --disable-editline`
- libssh2: `./configure --disable-shared --enable-static --with-crypto=wincng --with-libz`
- aria2: `./configure --disable-nls --with-wintls --without-gnutls --without-openssl --without-libnettle --without-libgcrypt --without-libgmp --with-libcares --with-libz --with-libexpat --without-libxml2 --with-sqlite3 --with-libssh2 --disable-libaria2 ARIA2_STATIC=yes`, then `make -C deps` (aria2's bundled wslay, for WebSocket RPC), `make -C src aria2c.exe` and `strip`

`PKG_CONFIG_LIBDIR` points only at the private prefix, so the zlib and GMP that the UCRT64
compiler itself depends on cannot be picked up.

`NOTICE` was assembled from the license files inside the tarballs above (aria2's `COPYING`,
wslay's `deps/wslay/COPYING`, zlib's `LICENSE`, c-ares' `LICENSE.md`, Expat's and libssh2's
`COPYING`, the blessing at the head of `sqlite3.c`) and from the toolchain packages'
`/ucrt64/share/licenses/{crt,winpthreads,libgcc}` files, each copied verbatim.

## Result (2026-10-31)

`aria2c.exe`: SHA-256 `0094FC1C68AE8BFB009B955874BB590A9CDCC4449A80A94DC3912AE2E5350A80`,
5,497,856 bytes (the official build it replaces was 5,649,408). A second clean run of
`scripts/build_aria2.ps1` in a different work directory produced the same bytes. `aria2c --version`:

```
aria2 version 1.37.0
Enabled Features: Async DNS, BitTorrent, Firefox3 Cookie, GZip, HTTPS, Message Digest, Metalink, XML-RPC, SFTP
Hash Algorithms: sha-1, sha-224, sha-256, sha-384, sha-512, md5, adler32
Libraries: zlib/1.3.2 expat/2.9.0 sqlite3/3.54.1 c-ares/1.34.9 libssh2/1.11.1
Compiler: mingw-w64 15.0.0 (alpha) / gcc 16.2.0
  built by  x86_64-w64-mingw32
  on        Oct 31 2026 00:00:00
```

The feature and hash lists are the official build's; its libraries line read
`zlib/1.3 expat/2.5.0 sqlite3/3.43.1 GMP/6.3.0 c-ares/1.19.1 libssh2/1.11.0`. (aria2 prints no
TLS library on Windows; WinTLS is in use.)

Imports (`objdump -p`): `ADVAPI32`, `bcrypt`, `CRYPT32`, `IPHLPAPI`, `KERNEL32`, `Secur32`,
`SHELL32`, `WS2_32`, `WSOCK32` and the `api-ms-win-crt-*` UCRT API sets. Nothing else ships
beside it.

Proof run against this binary, no CA file anywhere:

- `--check-certificate=true` download of
  `https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0.tar.xz`
  (redirected to GitHub's object store) exited 0 and the file's SHA-256 matched the pin above.
- The same download through the exact argument list `UupIsoBuilder::buildAria2Arguments` passes
  (`--input-file` with `checksum=sha-1=...`, 16 connections, `--check-integrity=true`,
  `--check-certificate=true`) exited 0 with the same SHA-256.
- `https://expired.badssl.com/`, `wrong.host.badssl.com`, `self-signed.badssl.com` and
  `untrusted-root.badssl.com` were each refused (exit 1) with Schannel's reason: "The received
  certificate has expired", "The target principal name is incorrect", and "The certificate chain
  was issued by an authority that is not trusted". With `--check-certificate=false` the expired
  host loads, so the refusal is the certificate check and not the network.
- The official 1.37.0 build gave the same results on the same checks.
