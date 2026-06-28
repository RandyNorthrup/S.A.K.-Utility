// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_keybag.cpp
/// @brief APFS FileVault keybag + DER key-blob construction implementation.

#include "sak/apfs_keybag.h"

#include <QtEndian>

#include <cstring>

namespace sak::apfs_keybag {

namespace {

void putLe16(QByteArray& b, int off, uint16_t v) {
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + off));
}
void putLe32(QByteArray& b, int off, uint32_t v) {
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + off));
}
void putLe64(QByteArray& b, int off, uint64_t v) {
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + off));
}

/// @brief Minimal big-endian DER INTEGER content for a non-negative value
/// (leading 0x00 added when the top bit is set, per DER).
QByteArray derIntegerBytes(uint64_t value) {
    QByteArray be;
    if (value == 0) {
        be.append('\0');
    } else {
        while (value != 0) {
            be.prepend(static_cast<char>(value & 0xFF));
            value >>= 8;
        }
    }
    if ((static_cast<unsigned char>(be.at(0)) & 0x80) != 0) {
        be.prepend('\0');
    }
    return be;
}

}  // namespace

QByteArray derLength(int length) {
    QByteArray out;
    if (length < 0x80) {
        out.append(static_cast<char>(length));
        return out;
    }
    QByteArray be;
    int v = length;
    while (v != 0) {
        be.prepend(static_cast<char>(v & 0xFF));
        v >>= 8;
    }
    out.append(static_cast<char>(0x80 | be.size()));
    out.append(be);
    return out;
}

QByteArray derTlv(uint8_t tag, const QByteArray& value) {
    QByteArray out;
    out.append(static_cast<char>(tag));
    out.append(derLength(static_cast<int>(value.size())));
    out.append(value);
    return out;
}

namespace {

/// @brief Wrap an inner keyblob field list in the outer SEQUENCE (hmac/salt/keyblob).
QByteArray wrapBlob(const KeyBlobParams& p, const QByteArray& inner) {
    QByteArray outer;
    outer.append(derTlv(0x80, QByteArray(1, '\0')));
    outer.append(derTlv(0x81, p.hmac32));
    outer.append(derTlv(0x82, p.outerSalt));
    outer.append(derTlv(0xA3, inner));
    return derTlv(0x30, outer);
}

/// @brief The first three inner keyblob fields shared by VEK and KEK blobs.
QByteArray innerHead(const KeyBlobParams& p) {
    QByteArray inner;
    inner.append(derTlv(0x80, QByteArray(1, '\0')));
    inner.append(derTlv(0x81, p.uuid));
    inner.append(derTlv(0x82, p.flags8));
    inner.append(derTlv(0x83, p.wrappedKey));
    return inner;
}

}  // namespace

QByteArray buildVekBlob(const KeyBlobParams& p) {
    return wrapBlob(p, innerHead(p));
}

QByteArray buildKekBlob(const KeyBlobParams& p) {
    QByteArray inner = innerHead(p);
    inner.append(derTlv(0x84, derIntegerBytes(p.iterations)));
    inner.append(derTlv(0x85, p.salt));
    return wrapBlob(p, inner);
}

QByteArray buildKeybagBlock(
    uint32_t magic, uint64_t oid, uint64_t xid, const QList<KeybagEntry>& entries, int blockSize) {
    QByteArray b(blockSize, '\0');
    putLe64(b, 0x08, oid);
    putLe64(b, 0x10, xid);
    putLe32(b, 0x18, magic);
    int p = 0x30;
    for (const auto& e : entries) {
        const int klen = static_cast<int>(e.keydata.size());
        std::memcpy(b.data() + p, e.uuid.constData(), 16);
        putLe16(b, p + 0x10, e.tag);
        putLe16(b, p + 0x12, static_cast<uint16_t>(klen));
        std::memcpy(b.data() + p + 0x18, e.keydata.constData(), static_cast<size_t>(klen));
        p += (0x18 + klen + 15) & ~15;
    }
    putLe16(b, 0x20, kApfsKeybagVersion);
    putLe16(b, 0x22, static_cast<uint16_t>(entries.size()));
    // kl_nbytes covers the 16-byte kb_locker header + the packed entry region.
    putLe32(b, 0x24, static_cast<uint32_t>(0x10 + (p - 0x30)));
    return b;
}

}  // namespace sak::apfs_keybag
