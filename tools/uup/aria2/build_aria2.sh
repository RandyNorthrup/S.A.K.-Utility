#!/usr/bin/env bash
# Copyright (c) 2026 Randy Northrup. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# build_aria2.sh - builds the bundled tools/uup/aria2c.exe from the source tarballs kept in
# this folder (tools/uup/aria2/). Run it through scripts/build_aria2.ps1, which verifies every
# tarball against its pinned SHA-256 first and starts this script in the MSYS2 UCRT64
# environment. It ships with the sources because the GPL counts the scripts that control
# compilation as part of aria2c.exe's complete source.
#
# aria2 uses Windows' own TLS stack and certificate store (WinTLS, built without OpenSSL and
# GnuTLS, as the official aria2 Windows build is). Every library is built from source and
# linked statically together with the GCC runtime, so aria2c.exe imports only Windows DLLs.
# libssh2 uses its Windows CNG crypto backend. BitTorrent's Diffie-Hellman and ARC4 use
# aria2's internal implementations (the official build links GMP for that instead).
#
# Usage: build_aria2.sh <source-dir> <work-dir> <jobs>

set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <source-dir> <work-dir> <jobs>" >&2
    exit 2
fi

SRC_DIR=$(cd "$1" && pwd)
WORK_DIR=$2
JOBS=$3

if [ "${MSYSTEM:-}" != "UCRT64" ]; then
    echo "error: run in the MSYS2 UCRT64 environment (MSYSTEM=UCRT64), not '${MSYSTEM:-}'" >&2
    exit 2
fi

ARIA2_VER=1.37.0
ZLIB_VER=1.3.2
CARES_VER=1.34.9
EXPAT_VER=2.9.0
SQLITE_VER=3540100
LIBSSH2_VER=1.11.1

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
WORK_DIR=$(cd "$WORK_DIR" && pwd)
PREFIX=$WORK_DIR/prefix
mkdir -p "$PREFIX/include" "$PREFIX/lib/pkgconfig"

# Only our prefix is searched for .pc files. The UCRT64 environment also has zlib and GMP
# installed as compiler dependencies; neither may leak into the link.
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PKG_CONFIG_PATH=
# Build paths must not reach the binaries, the PE header carries no link timestamp, and
# __DATE__/__TIME__ (aria2c --version prints them) come from SOURCE_DATE_EPOCH, so the same
# sources and toolchain give the same bytes. 1793404800 is 2026-10-31T00:00:00Z.
export SOURCE_DATE_EPOCH=1793404800
export CFLAGS="-O2 -g0 -ffile-prefix-map=$WORK_DIR=."
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib -Wl,--no-insert-timestamp"

step() {
    echo
    echo "=== $* ==="
}

unpack() {
    tar -xf "$SRC_DIR/$1" -C "$WORK_DIR"
}

step "zlib $ZLIB_VER"
unpack "zlib-$ZLIB_VER.tar.xz"
cd "$WORK_DIR/zlib-$ZLIB_VER"
./configure --prefix="$PREFIX" --static
make -j"$JOBS" libz.a
make install

step "c-ares $CARES_VER"
unpack "c-ares-$CARES_VER.tar.gz"
cd "$WORK_DIR/c-ares-$CARES_VER"
./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-tests
make -j"$JOBS"
make install

step "expat $EXPAT_VER"
unpack "expat-$EXPAT_VER.tar.xz"
cd "$WORK_DIR/expat-$EXPAT_VER"
./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --without-docbook --without-examples --without-tests --without-xmlwf
make -j"$JOBS"
make install

step "SQLite $SQLITE_VER"
unpack "sqlite-autoconf-$SQLITE_VER.tar.gz"
cd "$WORK_DIR/sqlite-autoconf-$SQLITE_VER"
./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --disable-readline --disable-editline
make -j"$JOBS"
make install

step "libssh2 $LIBSSH2_VER (WinCNG)"
unpack "libssh2-$LIBSSH2_VER.tar.xz"
cd "$WORK_DIR/libssh2-$LIBSSH2_VER"
./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --with-crypto=wincng --with-libz --with-libz-prefix="$PREFIX" \
    --disable-examples-build --disable-docker-tests --disable-sshd-tests
make -j"$JOBS"
make install

step "aria2 $ARIA2_VER"
unpack "aria2-$ARIA2_VER.tar.xz"
cd "$WORK_DIR/aria2-$ARIA2_VER"
for patch_file in "$SRC_DIR"/aria2-"$ARIA2_VER"-sak-*.patch; do
    [ -e "$patch_file" ] || continue
    echo "applying $(basename "$patch_file")"
    patch -p1 --forward --batch < "$patch_file"
done
./configure \
    --prefix="$PREFIX" \
    --disable-nls \
    --without-included-gettext \
    --with-wintls \
    --without-gnutls \
    --without-openssl \
    --without-appletls \
    --without-libnettle \
    --without-libgcrypt \
    --without-libgmp \
    --with-libcares \
    --with-libz \
    --with-libexpat \
    --without-libxml2 \
    --with-sqlite3 \
    --with-libssh2 \
    --disable-libaria2 \
    ARIA2_STATIC=yes
# deps/ holds aria2's bundled wslay (WebSocket for RPC), which aria2c links.
make -j"$JOBS" -C deps
make -j"$JOBS" -C src aria2c.exe
strip -o "$WORK_DIR/aria2c.exe" src/aria2c.exe

step "Result"
ls -l "$WORK_DIR/aria2c.exe"
echo "Imported DLLs:"
objdump -p "$WORK_DIR/aria2c.exe" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u
