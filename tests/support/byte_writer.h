// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file byte_writer.h
/// @brief Shared little-/big-endian byte pokers for building binary test fixtures.
///
/// These are the primitive integer writers used to lay out on-disk structures (superblocks,
/// inodes, B-tree nodes) inside an in-memory QByteArray image. They live in one place so a
/// fixture builder and the fuzz harness that reuses it never carry divergent copies. The
/// signatures deliberately match the pointer + qsizetype convention already used across the
/// partition-manager fixtures so existing call sites need no change.
///
/// (tests/support/pst_fixture.h keeps its own reference + int variants: a different calling
/// convention for a different subsystem, not a copy of these.)

#pragma once

#include <QByteArray>
#include <QtEndian>

#include <cstdint>

namespace sak::testfixtures {

inline void writeAscii(QByteArray* bytes, qsizetype offset, const char* value) {
    Q_ASSERT(bytes);
    const QByteArray text(value);
    for (qsizetype index = 0; index < text.size(); ++index) {
        (*bytes)[offset + index] = text.at(index);
    }
}

inline void writeRaw(QByteArray* bytes, qsizetype offset, const QByteArray& value) {
    Q_ASSERT(bytes);
    for (qsizetype index = 0; index < value.size(); ++index) {
        (*bytes)[offset + index] = value.at(index);
    }
}

inline void writeLe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

inline void writeLe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

inline void writeLe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>(value & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 4] = static_cast<char>((value >> 32) & 0xFF);
    (*bytes)[offset + 5] = static_cast<char>((value >> 40) & 0xFF);
    (*bytes)[offset + 6] = static_cast<char>((value >> 48) & 0xFF);
    (*bytes)[offset + 7] = static_cast<char>((value >> 56) & 0xFF);
}

inline void writeBe16(QByteArray* bytes, qsizetype offset, uint16_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>(value & 0xFF);
}

inline void writeBe32(QByteArray* bytes, qsizetype offset, uint32_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>(value & 0xFF);
}

inline void writeBe64(QByteArray* bytes, qsizetype offset, uint64_t value) {
    Q_ASSERT(bytes);
    (*bytes)[offset] = static_cast<char>((value >> 56) & 0xFF);
    (*bytes)[offset + 1] = static_cast<char>((value >> 48) & 0xFF);
    (*bytes)[offset + 2] = static_cast<char>((value >> 40) & 0xFF);
    (*bytes)[offset + 3] = static_cast<char>((value >> 32) & 0xFF);
    (*bytes)[offset + 4] = static_cast<char>((value >> 24) & 0xFF);
    (*bytes)[offset + 5] = static_cast<char>((value >> 16) & 0xFF);
    (*bytes)[offset + 6] = static_cast<char>((value >> 8) & 0xFF);
    (*bytes)[offset + 7] = static_cast<char>(value & 0xFF);
}

inline uint16_t readBe16(const QByteArray& bytes, qsizetype offset) {
    return qFromBigEndian<uint16_t>(bytes.constData() + offset);
}

inline uint32_t readBe32(const QByteArray& bytes, qsizetype offset) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset))) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 1))) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 2))) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(offset + 3)));
}

}  // namespace sak::testfixtures
