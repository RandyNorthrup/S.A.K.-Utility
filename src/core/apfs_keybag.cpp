// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_keybag.cpp
/// @brief APFS FileVault keybag + DER key-blob construction implementation.

#include "sak/apfs_keybag.h"

#include "sak/apfs_crypto.h"

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
uint16_t getLe16(const QByteArray& b, int off) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + off));
}
uint32_t getLe32(const QByteArray& b, int off) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + off));
}

/// @brief One parsed DER TLV field (tag + value; constructed values re-parsed).
struct DerField {
    uint8_t tag{0};
    QByteArray value;
};

/// @brief Decode a big-endian DER INTEGER value into a uint64.
uint64_t derBigEndianU64(const QByteArray& value) {
    uint64_t v = 0;
    for (char c : value) {
        v = (v << 8) | static_cast<uint8_t>(c);
    }
    return v;
}

/// @brief Assign one inner keyblob DER field into the parsed params by its tag.
void assignKeyBlobField(KeyBlobParams* out, uint8_t tag, const QByteArray& value) {
    switch (tag) {
    case 0x81:
        out->uuid = value;
        break;
    case 0x82:
        out->flags8 = value;
        break;
    case 0x83:
        out->wrappedKey = value;
        break;
    case 0x84:
        out->iterations = derBigEndianU64(value);
        break;
    case 0x85:
        out->salt = value;
        break;
    default:
        break;
    }
}

/// @brief Parse a flat sequence of DER TLV fields (short + long-form lengths).
QList<DerField> derParse(const QByteArray& buf) {
    QList<DerField> out;
    int i = 0;
    while (i + 2 <= buf.size()) {
        const uint8_t tag = static_cast<uint8_t>(buf.at(i++));
        int len = static_cast<uint8_t>(buf.at(i++));
        if ((len & 0x80) != 0) {
            int n = len & 0x7f;
            len = 0;
            for (int k = 0; k < n && i < buf.size(); ++k) {
                len = (len << 8) | static_cast<uint8_t>(buf.at(i++));
            }
        }
        if (i + len > buf.size()) {
            break;
        }
        out.append({tag, buf.mid(i, len)});
        i += len;
    }
    return out;
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

QList<KeybagEntry> parseKeybagBlock(const QByteArray& block) {
    QList<KeybagEntry> out;
    if (block.size() < 0x30) {
        return out;
    }
    const uint32_t magic = getLe32(block, 0x18);
    if (magic != kApfsObjectTypeContainerKeybag && magic != kApfsObjectTypeVolumeKeybag) {
        return out;
    }
    if (getLe16(block, 0x20) != kApfsKeybagVersion) {
        return out;
    }
    const int nkeys = getLe16(block, 0x22);
    int p = 0x30;
    for (int i = 0; i < nkeys; ++i) {
        if (p + 0x18 > block.size()) {
            break;
        }
        const int klen = getLe16(block, p + 0x12);
        if (p + 0x18 + klen > block.size()) {
            break;
        }
        out.append({block.mid(p, 16), getLe16(block, p + 0x10), block.mid(p + 0x18, klen)});
        p += (0x18 + klen + 15) & ~15;
    }
    return out;
}

bool parseKeyBlob(const QByteArray& blob, KeyBlobParams* out) {
    const QList<DerField> top = derParse(blob);
    if (top.isEmpty() || top.first().tag != 0x30) {
        return false;
    }
    QByteArray keyblob;
    for (const auto& f : derParse(top.first().value)) {
        if (f.tag == 0xA3) {
            keyblob = f.value;
        }
    }
    for (const auto& f : derParse(keyblob)) {
        assignKeyBlobField(out, f.tag, f.value);
    }
    return out->wrappedKey.size() == 40;
}

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

/// @brief Wrap an inner keyblob field list in the outer SEQUENCE. The outer HMAC
/// authenticates the keyblob ([0xA3] TLV) under the key SHA256(magic || outerSalt).
QByteArray wrapBlob(const KeyBlobParams& p, const QByteArray& inner) {
    const QByteArray keyblob = derTlv(0xA3, inner);
    QByteArray hmacInput(kApfsKeyBlobHmacMagic, kApfsKeyBlobHmacMagicLen);
    hmacInput.append(p.outerSalt);
    const QByteArray hmac = sak::apfs_crypto::hmacSha256(sak::apfs_crypto::sha256(hmacInput),
                                                         keyblob);
    QByteArray outer;
    outer.append(derTlv(0x80, QByteArray(1, '\0')));
    outer.append(derTlv(0x81, hmac));
    outer.append(derTlv(0x82, p.outerSalt));
    outer.append(keyblob);
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
