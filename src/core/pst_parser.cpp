// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_parser.cpp
/// @brief PST/OST binary parser implementation (MS-PST specification)

#include "sak/pst_parser.h"

#include "sak/email_html_sanitizer.h"
#include "sak/error_codes.h"
#include "sak/logger.h"

#include <QDataStream>
#include <QFileInfo>
#include <qt_windows.h>
#include <QtEndian>
#include <QTextDocument>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>

using sak::error_code;

// ============================================================================
// MS-PST section 5.1 -- Compressible Encryption Decrypt Table
// ============================================================================

// clang-format off
static constexpr std::array<uint8_t, 256> kDecryptTable = {
    0x47, 0xF1, 0xB4, 0xE6, 0x0B, 0x6A, 0x72, 0x48,
    0x85, 0x4E, 0x9E, 0xEB, 0xE2, 0xF8, 0x94, 0x53,
    0xE0, 0xBB, 0xA0, 0x02, 0xE8, 0x5A, 0x09, 0xAB,
    0xDB, 0xE3, 0xBA, 0xC6, 0x7C, 0xC3, 0x10, 0xDD,
    0x39, 0x05, 0x96, 0x30, 0xF5, 0x37, 0x60, 0x82,
    0x8C, 0xC9, 0x13, 0x4A, 0x6B, 0x1D, 0xF3, 0xFB,
    0x8F, 0x26, 0x97, 0xCA, 0x91, 0x17, 0x01, 0xC4,
    0x32, 0x2D, 0x6E, 0x31, 0x95, 0xFF, 0xD9, 0x23,
    0xD1, 0x00, 0x5E, 0x79, 0xDC, 0x44, 0x3B, 0x1A,
    0x28, 0xC5, 0x61, 0x57, 0x20, 0x90, 0x3D, 0x83,
    0xB9, 0x43, 0xBE, 0x67, 0xD2, 0x46, 0x42, 0x76,
    0xC0, 0x6D, 0x5B, 0x7E, 0xB2, 0x0F, 0x16, 0x29,
    0x3C, 0xA9, 0x03, 0x54, 0x0D, 0xDA, 0x5D, 0xDF,
    0xF6, 0xB7, 0xC7, 0x62, 0xCD, 0x8D, 0x06, 0xD3,
    0x69, 0x5C, 0x86, 0xD6, 0x14, 0xF7, 0xA5, 0x66,
    0x75, 0xAC, 0xB1, 0xE9, 0x45, 0x21, 0x70, 0x0C,
    0x87, 0x9F, 0x74, 0xA4, 0x22, 0x4C, 0x6F, 0xBF,
    0x1F, 0x56, 0xAA, 0x2E, 0xB3, 0x78, 0x33, 0x50,
    0xB0, 0xA3, 0x92, 0xBC, 0xCF, 0x19, 0x1C, 0xA7,
    0x63, 0xCB, 0x1E, 0x4D, 0x3E, 0x4B, 0x1B, 0x9B,
    0x4F, 0xE7, 0xF0, 0xEE, 0xAD, 0x3A, 0xB5, 0x59,
    0x04, 0xEA, 0x40, 0x55, 0x25, 0x51, 0xE5, 0x7A,
    0x89, 0x38, 0x68, 0x52, 0x7B, 0xFC, 0x27, 0xAE,
    0xD7, 0xBD, 0xFA, 0x07, 0xF4, 0xCC, 0x8E, 0x5F,
    0xEF, 0x35, 0x9C, 0x84, 0x2B, 0x15, 0xD5, 0x77,
    0x34, 0x49, 0xB6, 0x12, 0x0A, 0x7F, 0x71, 0x88,
    0xFD, 0x9D, 0x18, 0x41, 0x7D, 0x93, 0xD8, 0x58,
    0x2C, 0xCE, 0xFE, 0x24, 0xAF, 0xDE, 0xB8, 0x36,
    0xC8, 0xA1, 0x80, 0xA6, 0x99, 0x98, 0xA8, 0x2F,
    0x0E, 0x81, 0x65, 0x73, 0xE4, 0xC2, 0xA2, 0x8A,
    0xD4, 0xE1, 0x11, 0xD0, 0x08, 0x8B, 0x2A, 0xF2,
    0xED, 0x9A, 0x64, 0x3F, 0xC1, 0x6C, 0xF9, 0xEC,
};
// clang-format on

// ============================================================================
// Constants
// ============================================================================

/// Maximum BTree depth to prevent infinite recursion
static constexpr int kMaxBTreeDepth = 20;

/// MS-PST page-trailer ptype for a Node BTree page (ptypeNBT, MS-PST 2.2.2.7.7.1)
static constexpr uint8_t kPageTypeNbt = 0x81;

/// MS-PST page-trailer ptype for a Block BTree page (ptypeBBT)
static constexpr uint8_t kPageTypeBbt = 0x80;

/// Upper bound on a single assembled data tree (bytes). Kept well under INT_MAX so the
/// cumulative int offsets used while stitching XBLOCK/XXBLOCK children can never overflow;
/// a data tree that would exceed this is treated as a corrupt/hostile fan-out and fails closed.
static constexpr int kMaxAssembledDataTreeSize = 1024 * 1024 * 1024;

/// ANSI PST BTree page size (512 bytes)
static constexpr int kAnsiPageSizeLocal = sak::email::kAnsiPageSize;

/// Legacy Unicode PST BTree page size (wVer=23)
static constexpr int kLegacyUnicodePageSizeLocal = sak::email::kLegacyUnicodePageSize;

/// Unicode PST BTree page size (4096 bytes for Unicode4K)
static constexpr int kUnicodePageSizeLocal = sak::email::kUnicodePageSize;

/// Page trailer size for ANSI (validation fields at end of page)
static constexpr int kAnsiPageTrailerSize = 12;

/// Page trailer size for Unicode
static constexpr int kUnicodePageTrailerSize = 16;

/// Page trailer size for Unicode4K (24 bytes: ptype/repeat/wSig/dwCRC/bid/rid)
static constexpr int kUnicode4kPageTrailerSize = 24;

/// Metadata size for Unicode4K pages (cEnt(2)+cEntMax(2)+cbEnt(1)+cLevel(1)+pad(2))
static constexpr int kUnicode4kMetadataSize = 8;

/// Padding between metadata and trailer in Unicode4K pages
static constexpr int kUnicode4kPaddingSize = 8;

/// ANSI BTree entry size (Node BTree)
static constexpr int kAnsiNbtEntrySize = 16;

/// Unicode BTree entry size (Node BTree)
static constexpr int kUnicodeNbtEntrySize = 32;

/// ANSI BTree entry size (Block BTree)
static constexpr int kAnsiBbtEntrySize = 12;

/// Unicode BTree entry size (Block BTree)
static constexpr int kUnicodeBbtEntrySize = 24;

static constexpr int kPstOpenProgressHeader = 0;
static constexpr int kPstOpenProgressNodeBTree = 10;
static constexpr int kPstOpenProgressBlockBTree = 30;
static constexpr int kPstOpenProgressFolders = 60;
static constexpr int kPstOpenProgressComplete = 100;

static constexpr int kPstHeaderReadSize = 580;
static constexpr int kHeaderMagicOffset = 0;
static constexpr int kHeaderCrcOffset = 4;
static constexpr int kHeaderContentTypeOffset = 8;
static constexpr int kHeaderDataVersionOffset = 10;
static constexpr int kHeaderClientVersionOffset = 12;
static constexpr int kHeaderPlatformCreateOffset = 14;
static constexpr int kHeaderPlatformAccessOffset = 15;
static constexpr int kHeaderEncryptionOffset = 513;      // bCryptMethod, Unicode header (0x201)
static constexpr int kHeaderEncryptionOffsetAnsi = 461;  // bCryptMethod, ANSI header (0x1CD)
static constexpr int kUnicodeRootOffset = 0xB4;
static constexpr int kAnsiRootOffset = 0xA4;
static constexpr int kRootFileSizeOffset = 4;
static constexpr int kUnicodeRootNbtOffset = 44;
static constexpr int kUnicodeRootBbtOffset = 60;
static constexpr int kAnsiRootNbtOffset = 24;
static constexpr int kAnsiRootBbtOffset = 32;

/// Heap-on-Node signature byte
static constexpr uint8_t kHnSignature = 0xEC;

/// BTree-on-Heap signature byte
static constexpr uint8_t kBthSignature = 0xB5;

/// Table Context signature byte
static constexpr uint8_t kTcSignature = 0x7C;

/// Property Context signature byte
static constexpr uint8_t kPcSignature = 0xBC;

static constexpr uint16_t kPidTagLtpRowId = 0x67F2;
static constexpr uint64_t kNidTypeMask = 0x1F;
static constexpr uint64_t kNidLower32Mask = 0xFF'FF'FF'FF;
static constexpr uint64_t kBidInternalFlag = 0x02;
static constexpr int kHidOffsetRecordSize = 2;
static constexpr int kUnicode4kEntrySizeOffset = 4;
static constexpr int kUnicode4kLevelOffset = 5;
static constexpr int kLegacyEntrySizeOffset = 2;
static constexpr int kLegacyLevelOffset = 3;
static constexpr int kBTreeChildPageOffsetUnicode = 16;
static constexpr int kBTreeChildPageOffsetAnsi = 8;
static constexpr int kNbtDataBidOffsetUnicode = 8;
static constexpr int kNbtSubnodeBidOffsetUnicode = 16;
static constexpr int kNbtParentNodeOffsetUnicode = 24;
static constexpr int kNbtDataBidOffsetAnsi = 4;
static constexpr int kNbtSubnodeBidOffsetAnsi = 8;
static constexpr int kNbtParentNodeOffsetAnsi = 12;
static constexpr int kBbtFileOffsetUnicode = 8;
static constexpr int kBbtCbOffsetUnicode = 16;
static constexpr int kBbtFileOffsetAnsi = 4;
static constexpr int kBbtCbOffsetAnsi = 8;
static constexpr int kBlockTreeHeaderSize = 8;
static constexpr int kBlockTreeEntryCountOffset = 2;
static constexpr int kDataTreeBidSizeUnicode = 8;
static constexpr int kDataTreeBidSizeAnsi = 4;
static constexpr uint8_t kDataTreeXBlockLevel = 1;
static constexpr uint8_t kDataTreeXxBlockLevel = 2;
static constexpr int kHnHeaderMinSize = 12;
static constexpr int kHnSignatureOffset = 2;
static constexpr int kHnClientSignatureOffset = 3;
static constexpr int kHnRootHidOffset = 4;
static constexpr int kBthHeaderMinSize = 8;
static constexpr int kBthKeySizeOffset = 1;
static constexpr int kBthDataSizeOffset = 2;
static constexpr int kBthIndexLevelsOffset = 3;
static constexpr int kBthRootHidOffset = 4;
static constexpr int kBthChildHidSize = 4;
static constexpr int kPropertyValueRefOffset = 2;
static constexpr int kPropertyValueRefSize = 4;
static constexpr int kPropertyRecordMinInlineSize = 6;
static constexpr int kSubnodeLeafSizeUnicode = 24;
static constexpr int kSubnodeLeafSizeAnsi = 12;
static constexpr int kSubnodeDataBidOffsetUnicode = 8;
static constexpr int kSubnodeSubnodeBidOffsetUnicode = 16;
static constexpr int kSubnodeDataBidOffsetAnsi = 4;
static constexpr int kSubnodeSubnodeBidOffsetAnsi = 8;
static constexpr int kSubnodeIntermediateSizeUnicode = 16;
static constexpr int kSubnodeIntermediateSizeAnsi = 8;
static constexpr int kSubnodeIntermediateBidOffsetUnicode = 8;
static constexpr int kSubnodeIntermediateBidOffsetAnsi = 4;
static constexpr int kPropertyIdHexWidth = 4;
static constexpr int kPropertyIdHexBase = 16;
static constexpr int kSubjectPrefixMinChars = 2;
static constexpr int kSubjectPrefixStripChars = 2;
static constexpr ushort kSubjectPrefixMarker = 0x0001;
static constexpr int kInt16PropertyBytes = 2;
static constexpr int kInt32PropertyBytes = 4;
static constexpr int kInt64PropertyBytes = 8;
static constexpr int kFloat64DisplayPrecision = 6;
static constexpr int64_t kFileTimeEpochDiff = 116'444'736'000'000'000LL;
static constexpr int64_t kFileTimeTicksPerMillisecond = 10'000;
static constexpr int kTcinfoHeaderSize = 22;
static constexpr int kTcColumnDescriptorSize = 8;
static constexpr int kTcColumnPropIdOffset = 2;
static constexpr int kTcColumnDataOffset = 4;
static constexpr int kTcColumnDataSizeOffset = 6;
static constexpr int kTcColumnBitIndexOffset = 7;
static constexpr int kTcHeaderRgibTci1bOffset = 6;
static constexpr int kTcHeaderRgibTciBmOffset = 8;
static constexpr int kTcHeaderRowIndexHidOffset = 10;
static constexpr int kTcHeaderRowsHnidOffset = 14;
static constexpr int kLegacyMetadataSize = 4;
static constexpr int kBitPackingBitsPerByte = 8;
static constexpr int kBitPackingHighBitIndex = 7;
static constexpr int kHidIndexShift = 5;
static constexpr int kHidIndexMask4k = 0x3FFF;
static constexpr int kHidIndexMaskLegacy = 0x7FF;
static constexpr int kHidBlockIndexShift4k = 19;
static constexpr int kHidBlockIndexShiftLegacy = 16;
static constexpr int kHidBlockIndexMask4k = 0x1FFF;
static constexpr int kHidBlockIndexMaskLegacy = 0xFFFF;
static constexpr int kResolvedRecipientPartCount = 2;
static constexpr int kBthRowIdValueOffset = 4;

// ---------------------------------------------------------------------------
// MS-PST integrity-field layout (validated byte-for-byte against real Outlook
// PST (Unicode wVer=23) and OST (Unicode4k wVer=36) stores)
// ---------------------------------------------------------------------------

/// dwCRCPartial covers this many bytes of the header starting at wMagicClient.
static constexpr int kHeaderCrcDataStart = 8;
/// MS-PST 2.2.2.6 dwCRCPartial length (bytes from offset 8).
static constexpr int kHeaderCrcPartialLen = 471;
/// MS-PST 2.2.2.6 dwCRCFull length (bytes from offset 8); Unicode/Unicode4k only.
static constexpr int kHeaderCrcFullLen = 516;
/// Offset of dwCRCFull in the Unicode/Unicode4k header.
static constexpr int kHeaderCrcFullOffset = 0x20C;

/// PAGETRAILER field offsets (relative to the start of the trailer).
static constexpr int kPageTrailerSigOffset = 2;  // wSig
static constexpr int kPageTrailerCrcOffset = 4;  // dwCRC
static constexpr int kPageTrailerBidOffset = 8;  // bid (u64 Unicode/4k, u32 ANSI)

/// BLOCKTRAILER (Unicode 16 / ANSI 12) and Unicode4k block-footer field offsets.
/// The first 16 bytes of the 24-byte 4k footer share this layout.
static constexpr int kBlockTrailerCbOffset = 0;   // cb (repeats BBT data size)
static constexpr int kBlockTrailerSigOffset = 2;  // wSig
static constexpr int kBlockTrailerCrcOffset = 4;  // dwCRC over the cb data bytes
static constexpr int kBlockTrailerBidOffset = 8;  // bid (u64 Unicode/4k, u32 ANSI)
static constexpr int kBlockTrailerSizeUnicode = 16;
static constexpr int kBlockTrailerSizeAnsi = 12;
/// On-disk blocks (non-4k) are padded so data+trailer rounds up to this boundary.
static constexpr int kBlockAlignment = 64;

/// Bits shifted out of the CRC register per input byte, and the low-byte mask that
/// indexes the CRC lookup table (MS-PST reflected CRC-32 update step).
static constexpr int kBitsPerByte = 8;
static constexpr uint32_t kByteMask = 0xFFu;
/// WORD size (bits) and mask used by MS-PST ComputeSig to fold a 32-bit value.
static constexpr int kBitsPerWord = 16;
static constexpr uint32_t kWordMask = 0xFFFFu;

// MS-PST 2.2.2.7.1 / 2.2.2.8.1: 32-bit "weak" CRC. Standard reflected CRC-32
// lookup table (poly 0xEDB88320) with initial value 0 and NO final inversion --
// this differs from ISO 3309 CRC-32, which pre/post-inverts with 0xFFFFFFFF.
// Writes the computed CRC to @p out; returns false (without writing) when the
// requested range lies outside @p data so the caller fails closed rather than
// hashing a truncated span.
static bool msPstWeakCrc(const QByteArray& data, int offset, int len, uint32_t& out) {
    static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? ((c >> 1) ^ 0xED'B8'83'20u) : (c >> 1);
            }
            table[i] = c;
        }
        return table;
    }();

    if (offset < 0 || len < 0 ||
        static_cast<qsizetype>(offset) + static_cast<qsizetype>(len) > data.size()) {
        return false;
    }
    uint32_t crc = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData()) + offset;
    for (int i = 0; i < len; ++i) {
        crc = (crc >> kBitsPerByte) ^ kTable[(crc ^ bytes[i]) & kByteMask];
    }
    out = crc;
    return true;
}

// MS-PST 5.4 ComputeSig: WORD(ib XOR bid >> 16) XOR WORD(ib XOR bid). The spec
// folds only the low 32 bits of (ib XOR bid): dwCRC = WORD(value >> 16) ^ WORD(value).
// Folding bits [32:63] as well diverges from the spec once the byte offset ib
// exceeds 4 GiB (a >4 GiB OST/PST), which would false-reject every trailer past
// that mark. Authenticates that a page/block trailer belongs at its file offset
// with its declared BID.
static uint16_t msPstComputeSig(uint64_t ib, uint64_t bid) {
    const uint64_t value = ib ^ bid;
    return static_cast<uint16_t>(((value >> kBitsPerWord) ^ value) & kWordMask);
}

// MS-OXCMSG: PR_SUBJECT may begin with a prefix marker (U+0001) followed by
// a single character encoding the prefix length.  Strip these control bytes.
static QString stripSubjectPrefix(const QString& subject) {
    if (subject.size() >= kSubjectPrefixMinChars && subject.at(0) == QChar(kSubjectPrefixMarker)) {
        return subject.mid(kSubjectPrefixStripChars);
    }
    return subject;
}

// ============================================================================
// Property Type Classification Helpers
// ============================================================================

/// Types whose PC/TC values are stored as HNIDs requiring resolution
static bool isHnidResolvableType(uint16_t prop_type) {
    return prop_type == sak::email::kPropTypeString8 || prop_type == sak::email::kPropTypeUnicode ||
           prop_type == sak::email::kPropTypeBinary ||
           prop_type == sak::email::kPropTypeMultiInt32 ||
           prop_type == sak::email::kPropTypeMultiString ||
           prop_type == sak::email::kPropTypeMultiBinary || prop_type == sak::email::kPropTypeGuid;
}

/// PC variable types: HNID-resolvable + large fixed types (>4 bytes)
static bool isPcVariableType(uint16_t prop_type) {
    return isHnidResolvableType(prop_type) || prop_type == sak::email::kPropTypeSysTime ||
           prop_type == sak::email::kPropTypeInt64 || prop_type == sak::email::kPropTypeFloat64;
}

/// Validate that the PST data version is a known format
static bool isKnownDataVersion(uint16_t version) {
    // MS-PST 2.2.2.6: an ANSI store's wVer is 14 OR 15 (both accepted); 15 stays classified as
    // ANSI since 15 < kUnicodeVersion and 15 != kUnicode4kVersion.
    return version == sak::email::kAnsiVersion || version == sak::email::kAnsiVersion2 ||
           version == sak::email::kUnicodeVersion || version == sak::email::kUnicode4kVersion;
}

// ============================================================================
// Property Value Formatting Helpers
// ============================================================================

template <typename T>
static T localReadLE(const QByteArray& data, int offset) {
    // Bounds-check every fixed read: BTree leaf loops derive the record stride
    // from an attacker-controlled entry_size that can be smaller than the fixed
    // record actually read (e.g. a legacy page reading a uint32 at off+24 while
    // entry_size is 2), which would over-read past the page buffer. Return 0 for
    // an out-of-range field rather than performing an OOB memcpy.
    if (offset < 0 ||
        static_cast<qsizetype>(offset) + static_cast<qsizetype>(sizeof(T)) > data.size()) {
        return T{0};
    }
    T value;
    std::memcpy(&value, data.constData() + offset, sizeof(T));
    return value;
}

static QString formatInt16Value(const QByteArray& raw) {
    if (raw.size() < kInt16PropertyBytes) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int16_t>(raw, 0));
}

static QString formatInt32Value(const QByteArray& raw) {
    if (raw.size() < kInt32PropertyBytes) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int32_t>(raw, 0));
}

static QString formatInt64Value(const QByteArray& raw) {
    if (raw.size() < kInt64PropertyBytes) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int64_t>(raw, 0));
}

static QString formatBooleanValue(const QByteArray& raw) {
    if (raw.size() < 1) {
        return QStringLiteral("<invalid>");
    }
    return (raw[0] != 0) ? QStringLiteral("true") : QStringLiteral("false");
}

static QString formatFloat64Value(const QByteArray& raw) {
    if (raw.size() < kInt64PropertyBytes) {
        return QStringLiteral("<invalid>");
    }
    double val;
    std::memcpy(&val, raw.constData(), kInt64PropertyBytes);
    return QString::number(val, 'g', kFloat64DisplayPrecision);
}

static QString formatString8Value(const QByteArray& raw) {
    // PT_STRING8 properties can be encoded in various codepages.
    // Try UTF-8 first (most modern systems).
    auto utf8_result = QString::fromUtf8(raw);
    if (!utf8_result.contains(QChar::ReplacementCharacter)) {
        return utf8_result;
    }

    // Fall back to the system ANSI codepage (e.g., CP1252 on
    // Western Windows systems, CP932 on Japanese, etc.).
    const int needed = MultiByteToWideChar(
        CP_ACP, MB_PRECOMPOSED, raw.constData(), static_cast<int>(raw.size()), nullptr, 0);
    if (needed > 0) {
        QString result;
        result.resize(needed);
        const int written = MultiByteToWideChar(CP_ACP,
                                                MB_PRECOMPOSED,
                                                raw.constData(),
                                                static_cast<int>(raw.size()),
                                                reinterpret_cast<wchar_t*>(result.data()),
                                                needed);
        // Trust the actually-written count, not the sizing pass: if the second call
        // converts fewer code units (or fails, returning 0) do not leave uninitialized
        // tail characters in the string.
        if (written > 0) {
            result.resize(written);
            return result;
        }
    }

    return QString::fromLatin1(raw);
}

static QString formatUnicodeValue(const QByteArray& raw) {
    if (raw.size() < kInt16PropertyBytes) {
        return QString();
    }
    auto result = QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()),
                                     raw.size() / kInt16PropertyBytes);
    // Strip null terminators that PST files sometimes include
    while (!result.isEmpty() && result.back().isNull()) {
        result.chop(1);
    }
    return result;
}

static QString formatSysTimeValue(const QByteArray& raw) {
    if (raw.size() < kInt64PropertyBytes) {
        return QStringLiteral("<invalid>");
    }
    const int64_t ft = localReadLE<int64_t>(raw, 0);
    // Guard the signed subtraction below: a hostile value near INT64_MIN would underflow
    // (undefined behavior). Such a value is not a real FILETIME (100ns ticks since 1601).
    if (ft < INT64_MIN + kFileTimeEpochDiff) {
        return QStringLiteral("<invalid>");
    }
    const int64_t unix_ms = (ft - kFileTimeEpochDiff) / kFileTimeTicksPerMillisecond;
    return QDateTime::fromMSecsSinceEpoch(unix_ms, QTimeZone::UTC).toString(Qt::ISODate);
}

static QString formatBinaryValue(const QByteArray& raw) {
    constexpr int kMaxInlineHexBytes = 32;
    if (raw.size() <= kMaxInlineHexBytes) {
        return raw.toHex(' ');
    }
    return QStringLiteral("(%1 bytes)").arg(raw.size());
}

using PropFormatter = QString (*)(const QByteArray&);

// clang-format off
static const QHash<uint16_t, PropFormatter>& propFormatters() {
    static const QHash<uint16_t, PropFormatter> kMap = {
        {sak::email::kPropTypeInt16,    formatInt16Value},
        {sak::email::kPropTypeInt32,    formatInt32Value},
        {sak::email::kPropTypeInt64,    formatInt64Value},
        {sak::email::kPropTypeBoolean,  formatBooleanValue},
        {sak::email::kPropTypeFloat64,  formatFloat64Value},
        {sak::email::kPropTypeString8,  formatString8Value},
        {sak::email::kPropTypeUnicode,  formatUnicodeValue},
        {sak::email::kPropTypeSysTime,  formatSysTimeValue},
        {sak::email::kPropTypeBinary,   formatBinaryValue},
    };
    return kMap;
}
// clang-format on

// ============================================================================
// Property Dispatch Tables (eliminate large switch statements)
// ============================================================================

using DetailSetter = void (*)(sak::PstItemDetail&, const sak::MapiProperty&);
using SummarySetter = void (*)(sak::PstItemSummary&, const sak::MapiProperty&);
using FolderSetter = void (*)(sak::PstFolder&, const sak::MapiProperty&);
using AttachSetter = void (*)(sak::PstAttachmentInfo&, const QString&);

// clang-format off
static const QHash<uint16_t, DetailSetter>& messageIdentityDetailSetters() {
    static const QHash<uint16_t, DetailSetter> kMap = {
        {sak::email::kPropIdSubject,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.subject = stripSubjectPrefix(p.display_value);
         }},
        {sak::email::kPropIdSenderName,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (d.sender_name.isEmpty()) d.sender_name = p.display_value;
         }},
        {sak::email::kPropIdSentRepresentingName,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (d.sender_name.isEmpty()) d.sender_name = p.display_value;
         }},
        {sak::email::kPropIdSenderEmail,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (d.sender_email.isEmpty()) d.sender_email = p.display_value;
         }},
        {sak::email::kPropIdSentRepresentingEmail,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (d.sender_email.isEmpty()) d.sender_email = p.display_value;
         }},
        {sak::email::kPropIdMessageDeliveryTime,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (!d.date.isValid())
                 d.date = QDateTime::fromString(p.display_value, Qt::ISODate);
         }},
        {sak::email::kPropIdClientSubmitTime,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             if (!d.date.isValid())
                 d.date = QDateTime::fromString(p.display_value, Qt::ISODate);
         }},
        {sak::email::kPropIdMessageSize,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.size_bytes = p.display_value.toLongLong();
         }},
        {sak::email::kPropIdImportance,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.importance = p.display_value.toInt();
         }},
        {sak::email::kPropIdMessageClass,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.item_type = PstParser::classifyMessageClass(p.display_value);
         }},
    };
    return kMap;
}

static const QHash<uint16_t, DetailSetter>& messageBodyDetailSetters() {
    static const QHash<uint16_t, DetailSetter> kMap = {
        {sak::email::kPropIdBody,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.body_plain = p.display_value;
         }},
        {sak::email::kPropIdHtmlBody,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.body_html = QString::fromUtf8(p.raw_value);
         }},
        {sak::email::kPropIdRtfCompressed,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.body_rtf_compressed = p.raw_value;
         }},
        {sak::email::kPropIdTransportHeaders,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.transport_headers = p.display_value;
         }},
        {sak::email::kPropIdDisplayTo,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.display_to = p.display_value;
         }},
        {sak::email::kPropIdDisplayCc,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.display_cc = p.display_value;
         }},
        {sak::email::kPropIdDisplayBcc,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.display_bcc = p.display_value;
         }},
        {sak::email::kPropIdInternetMessageId,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.message_id = p.display_value;
         }},
        {sak::email::kPropIdInReplyTo,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.in_reply_to = p.display_value;
         }},
    };
    return kMap;
}

static const QHash<uint16_t, DetailSetter>& contactTaskDetailSetters() {
    static const QHash<uint16_t, DetailSetter> kMap = {
        {sak::email::kPropIdGivenName,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.given_name = p.display_value;
         }},
        {sak::email::kPropIdSurname,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.surname = p.display_value;
         }},
        {sak::email::kPropIdCompanyName,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.company_name = p.display_value;
         }},
        {sak::email::kPropIdJobTitle,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.job_title = p.display_value;
         }},
        {sak::email::kPropIdEmailAddress,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.email_address = p.display_value;
         }},
        {sak::email::kPropIdBusinessPhone,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.business_phone = p.display_value;
         }},
        {sak::email::kPropIdMobilePhone,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.mobile_phone = p.display_value;
         }},
        {sak::email::kPropIdHomePhone,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.home_phone = p.display_value;
         }},
        {sak::email::kPropIdTaskPriority,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.importance = p.display_value.toInt();
         }},
    };
    return kMap;
}

static const QHash<uint16_t, DetailSetter>& detailSetters() {
    static const QHash<uint16_t, DetailSetter> kMap = [] {
        auto map = messageIdentityDetailSetters();
        map.insert(messageBodyDetailSetters());
        map.insert(contactTaskDetailSetters());
        return map;
    }();
    return kMap;
}

static const QHash<uint16_t, SummarySetter>& summarySetters() {
    static const QHash<uint16_t, SummarySetter> kMap = {
        {kPidTagLtpRowId,  // PidTagLtpRowId -- NID
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             if (col.raw_value.size() >= kPropertyValueRefSize)
                 item.node_id = localReadLE<uint32_t>(col.raw_value, 0);
         }},
        {sak::email::kPropIdSubject,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.subject = stripSubjectPrefix(col.display_value);
         }},
        {sak::email::kPropIdSenderName,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.sender_name = col.display_value;
         }},
        {sak::email::kPropIdSenderEmail,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.sender_email = col.display_value;
         }},
        {sak::email::kPropIdMessageDeliveryTime,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             if (!item.date.isValid())
                 item.date = QDateTime::fromString(col.display_value, Qt::ISODate);
         }},
        {sak::email::kPropIdClientSubmitTime,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             if (!item.date.isValid())
                 item.date = QDateTime::fromString(col.display_value, Qt::ISODate);
         }},
        {sak::email::kPropIdMessageSize,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.size_bytes = col.display_value.toLongLong();
         }},
        {sak::email::kPropIdHasAttachments,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             if (col.display_value == QLatin1String("true")) {
                 item.has_attachments = true;
             }
         }},
        {sak::email::kPropIdMessageFlags,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.message_flags = col.display_value.toUInt();
             item.is_read = (item.message_flags & 0x01) != 0;
             constexpr uint32_t kMsgFlagHasAttach = 0x10;
             if ((item.message_flags & kMsgFlagHasAttach) != 0) {
                 item.has_attachments = true;
             }
         }},
        {sak::email::kPropIdImportance,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.importance = col.display_value.toInt();
         }},
        {sak::email::kPropIdMessageClass,
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             item.item_type = PstParser::classifyMessageClass(col.display_value);
         }},
    };
    return kMap;
}

static const QHash<uint16_t, FolderSetter>& folderSetters() {
    static const QHash<uint16_t, FolderSetter> kMap = {
        {sak::email::kPropIdDisplayName,
         [](sak::PstFolder& f, const sak::MapiProperty& p) {
             f.display_name = p.display_value;
         }},
        {sak::email::kPropIdContentCount,
         [](sak::PstFolder& f, const sak::MapiProperty& p) {
             f.content_count = p.display_value.toInt();
         }},
        {sak::email::kPropIdContentUnreadCount,
         [](sak::PstFolder& f, const sak::MapiProperty& p) {
             f.unread_count = p.display_value.toInt();
         }},
        {sak::email::kPropIdContainerClass,
         [](sak::PstFolder& f, const sak::MapiProperty& p) {
             f.container_class = p.display_value;
         }},
        {sak::email::kPropIdSubfolders,
         [](sak::PstFolder& f, const sak::MapiProperty& p) {
             f.subfolder_count = (p.display_value == QLatin1String("true")) ? 1 : 0;
         }},
    };
    return kMap;
}

static const QHash<uint16_t, AttachSetter>& attachmentSetters() {
    static const QHash<uint16_t, AttachSetter> kMap = {
        {sak::email::kPropIdAttachFilename,
         [](sak::PstAttachmentInfo& a, const QString& v) { a.filename = v; }},
        {sak::email::kPropIdAttachLongFilename,
         [](sak::PstAttachmentInfo& a, const QString& v) { a.long_filename = v; }},
        {sak::email::kPropIdAttachSize,
         [](sak::PstAttachmentInfo& a, const QString& v) { a.size_bytes = v.toLongLong(); }},
        {sak::email::kPropIdAttachMimeTag,
         [](sak::PstAttachmentInfo& a, const QString& v) { a.mime_type = v; }},
        {sak::email::kPropIdAttachContentId,
         [](sak::PstAttachmentInfo& a, const QString& v) { a.content_id = v; }},
        {sak::email::kPropIdAttachMethod,
         [](sak::PstAttachmentInfo& a, const QString& v) {
             a.attach_method = v.toInt();
             a.is_embedded_message = (a.attach_method == sak::email::kAttachEmbeddedMessage);
         }},
    };
    return kMap;
}
// clang-format on

/// Sender property ID -> slot mapping: 0=name, 1=email
static const QHash<uint16_t, int>& senderPropSlots() {
    static const QHash<uint16_t, int> kMap = {
        {sak::email::kPropIdSenderName, 0},
        {sak::email::kPropIdSentRepresentingName, 0},
        {sak::email::kPropIdSenderEmail, 1},
        {sak::email::kPropIdSentRepresentingEmail, 1},
    };
    return kMap;
}

/// Extract child NIDs from a hierarchy table
static QVector<uint64_t> extractChildNids(const QVector<QVector<sak::MapiProperty>>& htable) {
    QVector<uint64_t> child_nids;
    child_nids.reserve(htable.size());
    for (const auto& row : htable) {
        for (const auto& col : row) {
            if (col.tag_id == kPidTagLtpRowId && col.raw_value.size() >= kPropertyValueRefSize) {
                const uint64_t nid = localReadLE<uint32_t>(col.raw_value, 0);
                if (nid != 0) {
                    child_nids.append(nid);
                }
                break;
            }
        }
    }
    return child_nids;
}

/// Resolve block offset for a Heap-on-Node HID block index
static std::expected<int, error_code> resolveHnBlockOffset(uint16_t hid_block_index,
                                                           const QVector<int>& block_offsets,
                                                           int heap_size) {
    if (hid_block_index == 0) {
        return 0;
    }
    if (block_offsets.isEmpty() || hid_block_index > block_offsets.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    const int offset = block_offsets[hid_block_index - 1];
    if (offset + kHidOffsetRecordSize > heap_size) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    return offset;
}

/// Find a property by tag ID, returning pointer to raw value (or nullptr)
static const QByteArray* findPropertyById(const QVector<sak::MapiProperty>& props,
                                          uint16_t tag_id) {
    for (const auto& prop : props) {
        if (prop.tag_id == tag_id) {
            return &prop.raw_value;
        }
    }
    return nullptr;
}

// ============================================================================
// Table Context helpers (use PstParser::TcColDesc and PstParser::TcInfo)
// ============================================================================

static std::expected<PstParser::TcInfo, error_code> parseTcInfo(const QByteArray& tc_raw) {
    if (tc_raw.size() < kTcinfoHeaderSize) {
        return std::unexpected(error_code::pst_table_context_error);
    }
    if (static_cast<uint8_t>(tc_raw[0]) != kTcSignature) {
        return std::unexpected(error_code::pst_table_context_error);
    }

    PstParser::TcInfo info;
    const uint8_t col_count = static_cast<uint8_t>(tc_raw[1]);
    info.rgib_tci_1b = localReadLE<uint16_t>(tc_raw, kTcHeaderRgibTci1bOffset);
    info.rgib_tci_bm = localReadLE<uint16_t>(tc_raw, kTcHeaderRgibTciBmOffset);
    info.hid_row_index = localReadLE<uint32_t>(tc_raw, kTcHeaderRowIndexHidOffset);
    info.hnid_rows = localReadLE<uint32_t>(tc_raw, kTcHeaderRowsHnidOffset);

    info.columns.reserve(col_count);
    for (int col_idx = 0; col_idx < col_count; ++col_idx) {
        const int base = kTcinfoHeaderSize + (col_idx * kTcColumnDescriptorSize);
        // Fail closed: a declared column whose descriptor runs past the TCINFO buffer means the
        // header's column count is inconsistent with the (CRC-authenticated) heap allocation.
        // Returning the columns parsed so far would surface a partial schema as a complete table.
        if (base + kTcColumnDescriptorSize > tc_raw.size()) {
            return std::unexpected(error_code::pst_table_context_error);
        }
        PstParser::TcColDesc col;
        col.prop_type = localReadLE<uint16_t>(tc_raw, base);
        col.prop_id = localReadLE<uint16_t>(tc_raw, base + kTcColumnPropIdOffset);
        col.ib_data = localReadLE<uint16_t>(tc_raw, base + kTcColumnDataOffset);
        col.cb_data = static_cast<uint8_t>(tc_raw[base + kTcColumnDataSizeOffset]);
        col.i_bit = static_cast<uint8_t>(tc_raw[base + kTcColumnBitIndexOffset]);
        info.columns.append(col);
    }
    return info;
}

// ============================================================================
// File-Scope BTree Entry Readers
// ============================================================================

static sak::PstNode readNodeLeafEntry(const QByteArray& data, int off, bool is_unicode) {
    sak::PstNode node;
    if (is_unicode) {
        // MS-PST section 2.2.2.7.7.4: NBTENTRY nid field is 8 bytes in Unicode
        // files but only the lower 32 bits are the actual NID.  OST files
        // may have non-zero upper bits, so mask to 32 bits.
        node.node_id = localReadLE<uint64_t>(data, off) & kNidLower32Mask;
        node.data_bid = localReadLE<uint64_t>(data, off + kNbtDataBidOffsetUnicode);
        node.subnode_bid = localReadLE<uint64_t>(data, off + kNbtSubnodeBidOffsetUnicode);
        node.parent_node_id = localReadLE<uint32_t>(data, off + kNbtParentNodeOffsetUnicode);
    } else {
        node.node_id = localReadLE<uint32_t>(data, off);
        node.data_bid = localReadLE<uint32_t>(data, off + kNbtDataBidOffsetAnsi);
        node.subnode_bid = localReadLE<uint32_t>(data, off + kNbtSubnodeBidOffsetAnsi);
        node.parent_node_id = localReadLE<uint32_t>(data, off + kNbtParentNodeOffsetAnsi);
    }
    return node;
}

struct BlockLeafEntry {
    uint64_t bid = 0;
    uint64_t file_offset = 0;
    uint16_t cb = 0;
};

static BlockLeafEntry readBlockLeafEntry(const QByteArray& data, int off, bool is_unicode) {
    BlockLeafEntry entry;
    if (is_unicode) {
        entry.bid = localReadLE<uint64_t>(data, off);
        entry.file_offset = localReadLE<uint64_t>(data, off + kBbtFileOffsetUnicode);
        entry.cb = localReadLE<uint16_t>(data, off + kBbtCbOffsetUnicode);
    } else {
        entry.bid = localReadLE<uint32_t>(data, off);
        entry.file_offset = localReadLE<uint32_t>(data, off + kBbtFileOffsetAnsi);
        entry.cb = localReadLE<uint16_t>(data, off + kBbtCbOffsetAnsi);
    }
    return entry;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PstParser::PstParser(QObject* parent) : QObject(parent) {}

PstParser::~PstParser() {
    close();
}

// ============================================================================
// Public API
// ============================================================================

bool PstParser::failOpen(const QString& message, bool close_parser) {
    // NOTE: despite the name, this FAILS CLOSED -- it means "fail the open()": log
    // the real error, optionally close the parser, and return false so the caller
    // aborts. It never proceeds with a partially-parsed file.
    sak::logError("PstParser: {}", message.toStdString());
    Q_EMIT errorOccurred(message);
    if (close_parser) {
        close();
    }
    return false;
}

bool PstParser::openReadOnlyFile(const QString& file_path) {
    m_file.setFileName(file_path);
    if (m_file.open(QIODevice::ReadOnly)) {
        return true;
    }
    return failOpen(QStringLiteral("Cannot open file: %1").arg(file_path), false);
}

bool PstParser::loadPstStructure() {
    Q_EMIT progressUpdated(kPstOpenProgressHeader, QStringLiteral("Parsing file header..."));

    auto header_result = parseHeader();
    if (!header_result) {
        return failOpen(QStringLiteral("Invalid PST header: %1")
                            .arg(QString::fromUtf8(sak::to_string(header_result.error()))),
                        true);
    }

    Q_EMIT progressUpdated(kPstOpenProgressNodeBTree, QStringLiteral("Loading Node BTree..."));
    auto nbt_result = loadNodeBTree(m_header.root_nbt_page);
    if (!nbt_result) {
        return failOpen(QStringLiteral("Failed to load Node BTree: %1")
                            .arg(QString::fromUtf8(sak::to_string(nbt_result.error()))),
                        true);
    }

    Q_EMIT progressUpdated(kPstOpenProgressBlockBTree, QStringLiteral("Loading Block BTree..."));
    auto bbt_result = loadBlockBTree(m_header.root_bbt_page);
    if (!bbt_result) {
        return failOpen(QStringLiteral("Failed to load Block BTree: %1")
                            .arg(QString::fromUtf8(sak::to_string(bbt_result.error()))),
                        true);
    }

    Q_EMIT progressUpdated(kPstOpenProgressFolders, QStringLiteral("Building folder hierarchy..."));
    auto tree_result = buildFolderHierarchy(sak::email::kNidRootFolder);
    if (!tree_result) {
        return failOpen(QStringLiteral("Failed to build folder hierarchy: %1")
                            .arg(QString::fromUtf8(sak::to_string(tree_result.error()))),
                        true);
    }

    m_folder_tree = std::move(*tree_result);
    return true;
}

void PstParser::populateFileInfo(const QString& file_path) {
    const QFileInfo fi(file_path);
    m_file_info.file_path = file_path;
    m_file_info.file_size_bytes = fi.size();
    m_file_info.is_unicode = m_is_unicode;
    m_file_info.is_ost = (m_header.content_type == sak::email::kOstContentType);
    m_file_info.encryption_type = m_encryption_type;
    m_file_info.total_folders = countFolders(m_folder_tree);
    m_file_info.total_items = countTotalItems();
    m_file_info.last_modified = fi.lastModified();
}

void PstParser::loadMessageStoreDisplayName() {
    auto store_it = m_nbt_cache.find(sak::email::kNidMessageStore);
    if (store_it == m_nbt_cache.end()) {
        return;
    }

    auto props = readPropertyContext(sak::email::kNidMessageStore);
    if (!props) {
        return;
    }

    for (const auto& prop : *props) {
        if (prop.tag_id == sak::email::kPropIdDisplayName) {
            m_file_info.display_name = prop.display_value;
            break;
        }
    }
}

void PstParser::open(const QString& file_path) {
    close();
    m_cancelled.store(false, std::memory_order_relaxed);

    if (!openReadOnlyFile(file_path)) {
        return;
    }

    if (!loadPstStructure()) {
        return;
    }


    sak::logInfo("PstParser: NBT cache loaded -- {} nodes", m_nbt_cache.size());


    sak::logInfo("PstParser: BBT cache loaded -- {} blocks", m_bbt_cache.size());

    populateFileInfo(file_path);

    loadMessageStoreDisplayName();

    m_is_open = true;
    Q_EMIT progressUpdated(kPstOpenProgressComplete, QStringLiteral("File loaded successfully"));
    Q_EMIT fileOpened(m_file_info);
    Q_EMIT folderTreeLoaded(m_folder_tree);
}

void PstParser::close() {
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_is_open = false;
    m_header = {};
    m_file_info = {};
    m_folder_tree.clear();
    m_nbt_cache.clear();
    m_bbt_cache.clear();
    m_is_unicode = false;
    m_is_4k = false;
    m_encryption_type = 0;
}

bool PstParser::isOpen() const {
    return m_is_open;
}

sak::PstFileInfo PstParser::fileInfo() const {
    return m_file_info;
}

sak::PstFolderTree PstParser::folderTree() const {
    return m_folder_tree;
}

void PstParser::loadFolderItems(uint64_t folder_node_id, int offset, int limit) {
    auto result = readFolderItems(folder_node_id, offset, limit);
    if (result) {
        // Enrich items with sender names from individual PCs.
        // The TC often lacks usable sender data, so we do a
        // lightweight per-item PC read for sender properties only.
        enrichItemSenders(*result);

        // Read total count from the folder's content_count
        int total = 0;
        auto node_it = m_nbt_cache.find(folder_node_id);
        if (node_it != m_nbt_cache.end()) {
            auto props = readPropertyContext(folder_node_id);
            if (props) {
                for (const auto& prop : *props) {
                    if (prop.tag_id == sak::email::kPropIdContentCount) {
                        total = prop.display_value.toInt();
                        break;
                    }
                }
            }
        }
        if (total == 0) {
            total = static_cast<int>(result->size());
        }
        Q_EMIT folderItemsLoaded(folder_node_id, std::move(*result), total);
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load folder items: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void PstParser::loadItemDetail(uint64_t item_node_id) {
    auto result = readItemDetail(item_node_id);
    if (result) {
        Q_EMIT itemDetailLoaded(std::move(*result));
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load item detail: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void PstParser::loadItemProperties(uint64_t item_node_id) {
    auto result = readItemProperties(item_node_id);
    if (result) {
        Q_EMIT itemPropertiesLoaded(item_node_id, std::move(*result));
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load item properties: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void PstParser::loadAttachmentContent(uint64_t message_node_id, int attachment_index) {
    auto result = readAttachmentData(message_node_id, attachment_index);
    if (result) {
        // Get the attachment filename
        auto att_result = readAttachments(message_node_id);
        QString filename;
        if (att_result && attachment_index < att_result->size()) {
            const auto& att = (*att_result)[attachment_index];
            filename = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        }
        Q_EMIT attachmentContentReady(
            message_node_id, attachment_index, std::move(*result), filename);
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load attachment: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void PstParser::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

// ============================================================================
// Synchronous API (for worker threads)
// ============================================================================

std::expected<QVector<sak::PstItemSummary>, error_code> PstParser::readFolderItems(
    uint64_t folder_node_id, int offset, int limit) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }

    // Build the contents table NID from the folder NID
    // Contents table NID = (folder_nid & 0xFFFFFFE0) | NID_TYPE_CONTENTS_TABLE
    const uint64_t contents_nid = (folder_node_id & ~kNidTypeMask) |
                                  sak::email::kNidTypeContentsTable;

    return readContentsTable(contents_nid, offset, limit);
}

std::expected<sak::PstItemDetail, error_code> PstParser::readItemDetail(uint64_t item_node_id) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    return readMessage(item_node_id);
}

std::expected<QVector<sak::MapiProperty>, error_code> PstParser::readItemProperties(
    uint64_t item_node_id) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    return readPropertyContext(item_node_id);
}

std::expected<QByteArray, error_code> PstParser::extractAttachmentFromSubnode(
    const sak::PstNode& subnode) {
    HeapContext ctx;
    auto data_result = readDataTree(subnode.data_bid, &ctx.block_offsets);
    if (!data_result) {
        return std::unexpected(data_result.error());
    }
    ctx.heap_data = std::move(*data_result);

    if (subnode.subnode_bid != 0) {
        // Fail closed on a sub-node BTree failure: continuing with an empty subnode map makes
        // every HNID that points into a sub-node (often the attachment payload itself) resolve to
        // empty, so a corrupt subnode would otherwise yield truncated/empty attachment bytes
        // returned as success.
        auto sn = readSubNodeBTree(subnode.subnode_bid);
        if (!sn) {
            return std::unexpected(sn.error());
        }
        ctx.subnode_map = std::move(*sn);
    }

    auto bth = collectBthLeafData(ctx, kPcSignature);
    if (!bth) {
        return std::unexpected(bth.error());
    }

    auto att_props = parsePropertyRecords(*bth, ctx);
    const auto* data = findPropertyById(att_props, sak::email::kPropIdAttachData);
    if (data != nullptr) {
        return *data;
    }
    return std::unexpected(error_code::pst_attachment_error);
}

std::expected<QByteArray, error_code> PstParser::readAttachmentData(uint64_t message_node_id,
                                                                    int attachment_index) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }

    auto msg_it = m_nbt_cache.find(message_node_id);
    if (msg_it == m_nbt_cache.end()) {
        sak::logWarning("PstParser: attachment NID 0x{:08X} not found in NBT cache ({} entries)",
                        static_cast<uint32_t>(message_node_id),
                        m_nbt_cache.size());
        return std::unexpected(error_code::pst_node_not_found);
    }

    if (msg_it->subnode_bid == 0) {
        return std::unexpected(error_code::pst_attachment_error);
    }

    auto subnodes = readSubNodeBTree(msg_it->subnode_bid);
    if (!subnodes) {
        return std::unexpected(subnodes.error());
    }

    int found_count = 0;
    for (auto sub_it = subnodes->begin(); sub_it != subnodes->end(); ++sub_it) {
        const uint8_t sub_type = static_cast<uint8_t>(sub_it.key() & kNidTypeMask);
        if (sub_type != sak::email::kNidTypeAttachment) {
            continue;
        }
        // Use the SAME gate as readAttachments, which now FAILS CLOSED on an
        // unparsable attachment instead of compacting past it. The caller's index was
        // assigned there under this same gate, so failing here (rather than skipping)
        // keeps index i mapped to the same sub-node and never returns a different
        // attachment's bytes.
        auto probe = readSingleAttachment(sub_it.value(), found_count);
        if (!probe) {
            return std::unexpected(probe.error());
        }
        if (found_count == attachment_index) {
            return extractAttachmentFromSubnode(sub_it.value());
        }
        ++found_count;
    }

    return std::unexpected(error_code::pst_attachment_error);
}

// ============================================================================
// NDB Layer -- Header Parsing
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readHeaderBytes() {
    auto header_bytes = readBytes(kHeaderMagicOffset, kPstHeaderReadSize);
    if (!header_bytes) {
        return std::unexpected(header_bytes.error());
    }
    if (header_bytes->size() < kPstHeaderReadSize) {
        return std::unexpected(error_code::pst_invalid_header);
    }
    return std::move(*header_bytes);
}

std::expected<void, error_code> PstParser::parseHeaderPreamble(const QByteArray& data) {
    m_header.magic = readLE<uint32_t>(data, kHeaderMagicOffset);
    if (m_header.magic != sak::email::kPstMagic) {
        sak::logError("PstParser: Invalid magic: 0x{:08X}", m_header.magic);
        return std::unexpected(error_code::pst_invalid_header);
    }

    m_header.crc = readLE<uint32_t>(data, kHeaderCrcOffset);
    m_header.content_type = readLE<uint16_t>(data, kHeaderContentTypeOffset);
    if (m_header.content_type != sak::email::kPstContentType &&
        m_header.content_type != sak::email::kOstContentType) {
        sak::logError("PstParser: Invalid content type: 0x{:04X}", m_header.content_type);
        return std::unexpected(error_code::pst_invalid_header);
    }

    m_header.data_version = readLE<uint16_t>(data, kHeaderDataVersionOffset);
    m_is_unicode = (m_header.data_version >= sak::email::kUnicodeVersion);
    m_is_4k = (m_header.data_version == sak::email::kUnicode4kVersion);
    if (!isKnownDataVersion(m_header.data_version)) {
        // Fail closed: an unrecognized wVer means the on-disk layout (page/trailer/entry sizes,
        // BREF widths) is unknown, so continuing would parse every downstream field at guessed
        // offsets. Reject rather than mine garbage from a mislabeled or crafted file.
        sak::logError("PstParser: Unsupported data version: {}", m_header.data_version);
        return std::unexpected(error_code::pst_invalid_header);
    }

    m_header.client_version = readLE<uint16_t>(data, kHeaderClientVersionOffset);
    m_header.platform_create = static_cast<uint8_t>(data[kHeaderPlatformCreateOffset]);
    m_header.platform_access = static_cast<uint8_t>(data[kHeaderPlatformAccessOffset]);
    return {};
}

std::expected<void, error_code> PstParser::parseHeaderEncryption(const QByteArray& data) {
    // bCryptMethod lives at a different offset per format: 513 (0x201) in the
    // Unicode header, 461 (0x1CD) in the ANSI header (bSentinel@460 then
    // bCryptMethod@461; the ANSI header has no dwAlign). Reading the Unicode
    // offset for an ANSI PST both reads past the header and misdetects the crypt
    // method, so an encoded ANSI file decodes as garbage.
    const int encryption_offset = m_is_unicode ? kHeaderEncryptionOffset
                                               : kHeaderEncryptionOffsetAnsi;
    if (encryption_offset >= data.size()) {
        return std::unexpected(error_code::pst_invalid_header);
    }
    m_header.encryption_type = static_cast<uint8_t>(data[encryption_offset]);
    m_encryption_type = m_header.encryption_type;

    if (m_encryption_type == sak::email::kEncryptHigh) {
        sak::logError("PstParser: High encryption not supported");
        return std::unexpected(error_code::pst_unsupported_encryption);
    }
    // Fail closed on any crypt method we do not recognize. Only None and
    // Compressible are decoded downstream; an unknown value would otherwise be
    // silently treated as unencrypted and mine garbage from encoded blocks.
    if (m_encryption_type != sak::email::kEncryptNone &&
        m_encryption_type != sak::email::kEncryptCompressible) {
        sak::logError("PstParser: Unknown encryption method: 0x{:02X}", m_encryption_type);
        return std::unexpected(error_code::pst_unsupported_encryption);
    }
    return {};
}

void PstParser::parseHeaderRootPointers(const QByteArray& data) {
    if (m_is_unicode) {
        m_header.file_size = readLE<uint64_t>(data, kUnicodeRootOffset + kRootFileSizeOffset);
        m_header.root_nbt_page = readLE<uint64_t>(data, kUnicodeRootOffset + kUnicodeRootNbtOffset);
        m_header.root_bbt_page = readLE<uint64_t>(data, kUnicodeRootOffset + kUnicodeRootBbtOffset);
        return;
    }

    m_header.file_size = readLE<uint32_t>(data, kAnsiRootOffset + kRootFileSizeOffset);
    m_header.root_nbt_page = readLE<uint32_t>(data, kAnsiRootOffset + kAnsiRootNbtOffset);
    m_header.root_bbt_page = readLE<uint32_t>(data, kAnsiRootOffset + kAnsiRootBbtOffset);
}

std::expected<void, error_code> PstParser::verifyHeaderIntegrity(const QByteArray& data) {
    // dwCRCPartial (offset 4) is present in every MS-PST version and covers 471
    // bytes from wMagicClient (offset 8).
    uint32_t partial = 0;
    if (!msPstWeakCrc(data, kHeaderCrcDataStart, kHeaderCrcPartialLen, partial)) {
        return std::unexpected(error_code::pst_invalid_header);
    }
    if (partial != m_header.crc) {
        sak::logError(
            "PstParser: header dwCRCPartial mismatch (computed 0x{:08X}, stored 0x{:08X})",
            partial,
            m_header.crc);
        return std::unexpected(error_code::pst_integrity_check_failed);
    }

    // dwCRCFull (offset 0x20C) exists only in the Unicode/Unicode4k header and
    // authenticates the fuller 516-byte prefix (through bidNextB).
    if (m_is_unicode) {
        uint32_t full = 0;
        if (!msPstWeakCrc(data, kHeaderCrcDataStart, kHeaderCrcFullLen, full)) {
            return std::unexpected(error_code::pst_invalid_header);
        }
        const uint32_t stored_full = readLE<uint32_t>(data, kHeaderCrcFullOffset);
        if (full != stored_full) {
            sak::logError(
                "PstParser: header dwCRCFull mismatch (computed 0x{:08X}, stored 0x{:08X})",
                full,
                stored_full);
            return std::unexpected(error_code::pst_integrity_check_failed);
        }
    }
    return {};
}

void PstParser::logParsedHeader() const {
    sak::logInfo(
        "PstParser: Header parsed - version={}, unicode={}, encryption={}, NBT=0x{:X}, "
        "BBT=0x{:X}",
        m_header.data_version,
        m_is_unicode,
        m_encryption_type,
        m_header.root_nbt_page,
        m_header.root_bbt_page);
}

std::expected<void, error_code> PstParser::parseHeader() {
    auto header_bytes = readHeaderBytes();
    if (!header_bytes) {
        return std::unexpected(header_bytes.error());
    }

    const auto& data = *header_bytes;
    // section 2.2.2.6 -- dwMagic at offset 0
    auto preamble_result = parseHeaderPreamble(data);
    if (!preamble_result) {
        return std::unexpected(preamble_result.error());
    }

    // section 2.2.2.6 -- authenticate the header before trusting any field past the
    // preamble. dwCRCPartial/dwCRCFull cover the version, ROOT BREF pointers, and
    // crypt method; a mismatch means the bytes we are about to parse as the on-disk
    // layout are corrupt or forged, so fail closed here.
    auto integrity_result = verifyHeaderIntegrity(data);
    if (!integrity_result) {
        return std::unexpected(integrity_result.error());
    }

    // wMagicClient at offset 8 -- content type (SM or SO)
    // wVer at offset 10 -- version
    // bCryptMethod -- encryption type
    // ANSI: offset 513, Unicode: offset 513
    auto encryption_result = parseHeaderEncryption(data);
    if (!encryption_result) {
        return std::unexpected(encryption_result.error());
    }

    // Root structure pointers differ between ANSI and Unicode.
    // MS-PST section 2.2.2.6: header fields before ROOT include rgnid[32]
    // (128 bytes) plus bid/unique fields whose sizes depend on format.
    //   Unicode: ROOT at offset 0xB4 (180)
    //   ANSI:    ROOT at offset 0xA4 (164)
    // ROOT layout (section 2.2.2.7.5):
    //   +0  dwReserved (4)
    //   +4  ibFileEof  (8 Unicode / 4 ANSI)
    //   +36 BREFNBT    (16 Unicode / 8 ANSI)
    //   +52 BREFBBT    (16 Unicode / 8 ANSI)
    parseHeaderRootPointers(data);

    logParsedHeader();

    return {};
}

// ============================================================================
// NDB Layer -- BTree Page Parsing
// ============================================================================

PstParser::PageFormatSizes PstParser::pageFormatSizes() const {
    PageFormatSizes fmt;
    fmt.page_size = m_is_4k        ? kUnicodePageSizeLocal
                    : m_is_unicode ? kLegacyUnicodePageSizeLocal
                                   : kAnsiPageSizeLocal;
    fmt.trailer_size = m_is_4k        ? kUnicode4kPageTrailerSize
                       : m_is_unicode ? kUnicodePageTrailerSize
                                      : kAnsiPageTrailerSize;
    fmt.meta_size = m_is_4k ? kUnicode4kMetadataSize : kLegacyMetadataSize;
    fmt.meta_pad = m_is_4k ? kUnicode4kPaddingSize : (m_is_unicode ? kLegacyMetadataSize : 0);
    return fmt;
}

std::expected<PstParser::BTreePageInfo, error_code> PstParser::parseBTreePage(
    uint64_t page_offset, int depth, uint8_t expected_ptype) {
    if (depth > kMaxBTreeDepth) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return std::unexpected(error_code::operation_cancelled);
    }

    auto fmt = pageFormatSizes();
    auto page_data = readBytes(static_cast<qint64>(page_offset), fmt.page_size);
    if (!page_data) {
        return std::unexpected(page_data.error());
    }
    if (page_data->size() < fmt.page_size) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    const int trailer_offset = fmt.page_size - fmt.trailer_size;
    const uint8_t ptype = static_cast<uint8_t>((*page_data)[trailer_offset]);
    // ptype and its duplicate (ptypeRepeat) must agree, and the page must be exactly the BTree
    // type the caller expected (ptypeNBT vs ptypeBBT). The all-zeros page that a truncated or
    // crafted file yields satisfies the duplicate check by coincidence; the expected-type gate
    // rejects it so a non-BTree region is never walked as one.
    if (ptype != static_cast<uint8_t>((*page_data)[trailer_offset + 1]) ||
        ptype != expected_ptype) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    // Authenticate the page via its PAGETRAILER before walking it as a BTree.
    auto trailer_ok = verifyPageTrailer(*page_data, fmt, page_offset);
    if (!trailer_ok) {
        return std::unexpected(trailer_ok.error());
    }

    const int meta_offset = trailer_offset - fmt.meta_pad - fmt.meta_size;
    if (meta_offset < 0) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    BTreePageInfo info;
    info.data = std::move(*page_data);
    info.meta_offset = meta_offset;
    parsePageMeta(info, meta_offset);
    return info;
}

std::expected<void, error_code> PstParser::verifyPageTrailer(const QByteArray& page_data,
                                                             const PageFormatSizes& fmt,
                                                             uint64_t page_offset) {
    // section 2.2.2.7.1: dwCRC over the page body preceding the trailer, and
    // wSig == ComputeSig(page ib, page bid). A structurally-typed-but-corrupt page
    // (bit rot, or a crafted page whose ptype matches by luck) fails here.
    const int trailer_offset = fmt.page_size - fmt.trailer_size;
    uint32_t page_crc = 0;
    if (!msPstWeakCrc(page_data, 0, fmt.page_size - fmt.trailer_size, page_crc)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    if (page_crc != readLE<uint32_t>(page_data, trailer_offset + kPageTrailerCrcOffset)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    const uint16_t page_sig = readLE<uint16_t>(page_data, trailer_offset + kPageTrailerSigOffset);
    const uint64_t page_bid =
        m_is_unicode ? readLE<uint64_t>(page_data, trailer_offset + kPageTrailerBidOffset)
                     : readLE<uint32_t>(page_data, trailer_offset + kPageTrailerBidOffset);
    if (msPstComputeSig(page_offset, page_bid) != page_sig) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    return {};
}

void PstParser::parsePageMeta(BTreePageInfo& info, int meta_offset) {
    if (m_is_4k) {
        info.entry_count = readLE<uint16_t>(info.data, meta_offset);
        info.entry_size = static_cast<uint8_t>(info.data[meta_offset + kUnicode4kEntrySizeOffset]);
        info.level = static_cast<uint8_t>(info.data[meta_offset + kUnicode4kLevelOffset]);
    } else {
        info.entry_count = static_cast<uint8_t>(info.data[meta_offset]);
        info.entry_size = static_cast<uint8_t>(info.data[meta_offset + kLegacyEntrySizeOffset]);
        info.level = static_cast<uint8_t>(info.data[meta_offset + kLegacyLevelOffset]);
    }
}

// ============================================================================
// NDB Layer -- BTree Loading
// ============================================================================

std::expected<void, error_code> PstParser::loadNodeBTree(uint64_t page_offset, int depth) {
    QSet<uint64_t> visited;
    return loadNodeBTreeGuarded(page_offset, depth, visited);
}

std::expected<void, error_code> PstParser::loadNodeBTreeGuarded(uint64_t page_offset,
                                                                int depth,
                                                                QSet<uint64_t>& visited) {
    // Expand each page offset at most once per traversal: a valid NBT visits every page once,
    // so a repeat is a cycle or a fan-out bomb (an internal page listing the same child many
    // times). Fail closed on both before doing any further work.
    if (visited.contains(page_offset)) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    visited.insert(page_offset);

    auto page = parseBTreePage(page_offset, depth, kPageTypeNbt);
    if (!page) {
        return std::unexpected(page.error());
    }

    const auto& data = page->data;
    const int entry_size = page->entry_size;
    const int meta_offset = page->meta_offset;

    if (page->level == 0) {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            const int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            auto node = readNodeLeafEntry(data, off, m_is_unicode);
            m_nbt_cache.insert(node.node_id, node);
        }
    } else {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            const int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            const uint64_t child_page =
                m_is_unicode ? readLE<uint64_t>(data, off + kBTreeChildPageOffsetUnicode)
                             : readLE<uint32_t>(data, off + kBTreeChildPageOffsetAnsi);
            auto child_result = loadNodeBTreeGuarded(child_page, depth + 1, visited);
            if (!child_result) {
                return child_result;
            }
        }
    }
    return {};
}

std::expected<void, error_code> PstParser::loadBlockBTree(uint64_t page_offset, int depth) {
    QSet<uint64_t> visited;
    return loadBlockBTreeGuarded(page_offset, depth, visited);
}

std::expected<void, error_code> PstParser::loadBlockBTreeGuarded(uint64_t page_offset,
                                                                 int depth,
                                                                 QSet<uint64_t>& visited) {
    // See loadNodeBTreeGuarded: reject a revisited page offset so a crafted BBT cannot cycle
    // or fan out without bound.
    if (visited.contains(page_offset)) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    visited.insert(page_offset);

    auto page = parseBTreePage(page_offset, depth, kPageTypeBbt);
    if (!page) {
        return std::unexpected(page.error());
    }

    const auto& data = page->data;
    const int entry_size = page->entry_size;
    const int meta_offset = page->meta_offset;

    if (page->level == 0) {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            const int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            auto entry = readBlockLeafEntry(data, off, m_is_unicode);
            m_bbt_cache.insert(entry.bid,
                               BbtEntry{.file_offset = entry.file_offset, .cb = entry.cb});
        }
    } else {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            const int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            const uint64_t child_page =
                m_is_unicode ? readLE<uint64_t>(data, off + kBTreeChildPageOffsetUnicode)
                             : readLE<uint32_t>(data, off + kBTreeChildPageOffsetAnsi);
            auto child_result = loadBlockBTreeGuarded(child_page, depth + 1, visited);
            if (!child_result) {
                return child_result;
            }
        }
    }
    return {};
}

// ============================================================================
// NDB Layer -- Block Reading
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readBlock(uint64_t bid) {
    // section 2.2.2.2 -- The BBT stores BIDs with the fInternal bit (bit 1)
    // intact, so look up the BID as-is.
    auto it = m_bbt_cache.find(bid);
    if (it == m_bbt_cache.end()) {
        return std::unexpected(error_code::pst_block_not_found);
    }

    const auto& entry = it.value();
    const uint64_t file_offset = entry.file_offset;
    const int cb = static_cast<int>(entry.cb);

    if (cb == 0) {
        return QByteArray{};
    }

    // Read exactly cb bytes of block data (section 2.2.2.8 -- data precedes trailer).
    // readBytes returns whatever the file has, so a truncated file yields a short buffer;
    // downstream fixed-offset reads treat the block as cb bytes, so require the full length
    // and fail closed on a short read rather than parsing past the end.
    auto block_data = readBytes(static_cast<qint64>(file_offset), cb);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }
    if (block_data->size() != cb) {
        return std::unexpected(error_code::read_error);
    }

    return postProcessBlock(std::move(*block_data), file_offset, cb, bid);
}

std::expected<QByteArray, error_code> PstParser::postProcessBlock(QByteArray raw,
                                                                  uint64_t file_offset,
                                                                  int cb,
                                                                  uint64_t bid) {
    // Authenticate the block via its on-disk trailer (section 2.2.2.8.1) BEFORE any
    // decompression or decryption -- the dwCRC is authored over the raw cb bytes
    // exactly as stored. A corrupt or forged block fails closed here.
    auto trailer_result = verifyBlockTrailer(raw, file_offset, cb, bid);
    if (!trailer_result) {
        return std::unexpected(trailer_result.error());
    }

    // Unicode4K files (wVer >= 36) may have zlib-compressed blocks. The block footer
    // carries an uncompressed_data_size; when it differs from cb the block is
    // deflate-compressed. Reference: libpff pff_block_footer_64bit_4k_page_t.
    if (m_is_4k && cb > 0) {
        auto decompressed = decompressBlockIf4k(raw, file_offset, cb);
        if (!decompressed) {
            return std::unexpected(decompressed.error());
        }
        raw = std::move(*decompressed);
    }

    // Compressible encryption applies only to external (non-internal) blocks.
    if (m_encryption_type == sak::email::kEncryptCompressible && (bid & kBidInternalFlag) == 0) {
        auto span = std::span<uint8_t>(reinterpret_cast<uint8_t*>(raw.data()),
                                       static_cast<size_t>(raw.size()));
        decryptBlock(span);
    }

    return raw;
}

std::expected<QByteArray, error_code> PstParser::readDataTree(uint64_t bid,
                                                              QVector<int>* block_offsets,
                                                              int depth) {
    QSet<uint64_t> visited;
    return readDataTreeGuarded(bid,
                               block_offsets,
                               DataTreeGuard{.depth = depth, .visited = visited});
}

std::expected<QByteArray, error_code> PstParser::readDataTreeGuarded(uint64_t bid,
                                                                     QVector<int>* block_offsets,
                                                                     DataTreeGuard guard) {
    // Bound recursion: a crafted PST can build a self-referential or cyclic chain
    // of internal (XBLOCK/XXBLOCK) blocks that would otherwise recurse until the
    // native stack overflows.
    if (guard.depth > kMaxBTreeDepth) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    // Expand each BID at most once per traversal. A valid data tree references
    // every block exactly once, so a repeated BID is either a cycle or a
    // fan-out bomb (a block that lists the same child thousands of times, which
    // without this guard expands as entry_count^depth). Fail closed on both.
    if (guard.visited.contains(bid)) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    guard.visited.insert(bid);

    const bool is_internal = (bid & kBidInternalFlag) != 0;
    if (!is_internal) {
        auto result = readBlock(bid);
        if (result && (block_offsets != nullptr)) {
            block_offsets->append(static_cast<int>(result->size()));
        }
        return result;
    }

    auto block_data = readBlock(bid);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }

    const auto& data = *block_data;
    if (data.size() < kBlockTreeHeaderSize) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    const uint8_t block_level = static_cast<uint8_t>(data[1]);
    const uint16_t entry_count = readLE<uint16_t>(data, kBlockTreeEntryCountOffset);

    // The in-block entry count is independent of the block's byte length (which
    // comes from the BBT), so a small block can claim a large count. Reject any
    // count whose entries would run past the block before reading child bids.
    const int bid_size = m_is_unicode ? kDataTreeBidSizeUnicode : kDataTreeBidSizeAnsi;
    if (static_cast<int64_t>(kBlockTreeHeaderSize) +
            (static_cast<int64_t>(entry_count) * bid_size) >
        data.size()) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    return readInternalDataBlock(data, block_level, entry_count, block_offsets, guard);
}

std::expected<QByteArray, error_code> PstParser::readInternalDataBlock(const QByteArray& data,
                                                                       uint8_t block_level,
                                                                       uint16_t entry_count,
                                                                       QVector<int>* block_offsets,
                                                                       DataTreeGuard guard) {
    const DataTreeGuard child_guard{.depth = guard.depth + 1, .visited = guard.visited};
    QByteArray result;
    if (block_level == kDataTreeXBlockLevel) {
        auto status = readXblockChildren(data, entry_count, result, block_offsets, child_guard);
        if (!status) {
            return std::unexpected(status.error());
        }
    } else if (block_level == kDataTreeXxBlockLevel) {
        auto status = readXxblockChildren(data, entry_count, result, block_offsets, child_guard);
        if (!status) {
            return std::unexpected(status.error());
        }
    } else {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    return result;
}

std::expected<void, error_code> PstParser::readXblockChildren(const QByteArray& data,
                                                              uint16_t entry_count,
                                                              QByteArray& result,
                                                              QVector<int>* block_offsets,
                                                              DataTreeGuard guard) {
    const int bid_size = m_is_unicode ? kDataTreeBidSizeUnicode : kDataTreeBidSizeAnsi;
    for (int idx = 0; idx < entry_count; ++idx) {
        const int offset = kBlockTreeHeaderSize + (idx * bid_size);
        const uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset)
                                                : readLE<uint32_t>(data, offset);
        // Recurse through readDataTreeGuarded so nested internal (XBLOCK/XXBLOCK)
        // children are properly expanded (some PST/OST files build non-spec nesting
        // where an XBLOCK entry itself carries the internal bid flag 0x02) while the
        // shared visited set bounds total work.
        const int base_offset = static_cast<int>(result.size());
        QVector<int> child_offsets;
        auto child_data = readDataTreeGuarded(child_bid,
                                              (block_offsets != nullptr) ? &child_offsets : nullptr,
                                              guard);
        if (!child_data) {
            return std::unexpected(child_data.error());
        }
        // Cap the assembled size BEFORE appending so a near-limit result plus a near-limit child
        // cannot momentarily allocate ~2x the ceiling; fail closed once the sum would exceed it.
        // This also keeps the cumulative int offsets below from overflowing.
        if (static_cast<int64_t>(result.size()) + child_data->size() > kMaxAssembledDataTreeSize) {
            return std::unexpected(error_code::pst_corrupted_btree);
        }
        result.append(*child_data);
        if (block_offsets != nullptr) {
            for (const int child_off : child_offsets) {
                block_offsets->append(base_offset + child_off);
            }
        }
    }
    return {};
}

std::expected<void, error_code> PstParser::readXxblockChildren(const QByteArray& data,
                                                               uint16_t entry_count,
                                                               QByteArray& result,
                                                               QVector<int>* block_offsets,
                                                               DataTreeGuard guard) {
    const int bid_size = m_is_unicode ? kDataTreeBidSizeUnicode : kDataTreeBidSizeAnsi;
    for (int idx = 0; idx < entry_count; ++idx) {
        const int offset = kBlockTreeHeaderSize + (idx * bid_size);
        const uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset)
                                                : readLE<uint32_t>(data, offset);
        const int base_offset = static_cast<int>(result.size());
        QVector<int> child_offsets;
        auto child_data = readDataTreeGuarded(child_bid, &child_offsets, guard);
        if (!child_data) {
            return std::unexpected(child_data.error());
        }
        // Cap the assembled size BEFORE appending (see readXblockChildren): a near-limit result
        // plus a near-limit child must not momentarily allocate ~2x the ceiling. Fail closed once
        // the sum would exceed it; this also keeps the cumulative int offsets below safe.
        if (static_cast<int64_t>(result.size()) + child_data->size() > kMaxAssembledDataTreeSize) {
            return std::unexpected(error_code::pst_corrupted_btree);
        }
        result.append(*child_data);
        if (block_offsets != nullptr) {
            for (const int child_off : child_offsets) {
                block_offsets->append(base_offset + child_off);
            }
        }
    }
    return {};
}

// ============================================================================
// NDB Layer -- Block Decompression (Unicode4K / wVer >= 36)
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readBlockTrailer(uint64_t file_offset, int cb) {
    using namespace sak::email;  // kBlock4kAlignment / kBlock4kFooterSize
    // Locate the on-disk trailer. The two format families pad differently:
    //   * non-4k: data+trailer rounds up to a 64-byte boundary (BLOCKTRAILER at end).
    //   * 4k:     cb rounds up to 512, then a 512 pad is added if the 24-byte footer
    //             would not otherwise fit -- the same rule decompressBlockIf4k uses.
    const int trailer_size = m_is_4k        ? kBlock4kFooterSize
                             : m_is_unicode ? kBlockTrailerSizeUnicode
                                            : kBlockTrailerSizeAnsi;
    int64_t trailer_pos = 0;
    if (m_is_4k) {
        int aligned = ((cb + kBlock4kAlignment - 1) / kBlock4kAlignment) * kBlock4kAlignment;
        if ((aligned - cb) < kBlock4kFooterSize) {
            aligned += kBlock4kAlignment;
        }
        trailer_pos = static_cast<int64_t>(file_offset) + aligned - kBlock4kFooterSize;
    } else {
        const int64_t disk =
            ((static_cast<int64_t>(cb) + trailer_size + kBlockAlignment - 1) / kBlockAlignment) *
            kBlockAlignment;
        trailer_pos = static_cast<int64_t>(file_offset) + disk - trailer_size;
    }

    auto trailer = readBytes(trailer_pos, trailer_size);
    if (!trailer || trailer->size() != trailer_size) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    return std::move(*trailer);
}

std::expected<void, error_code> PstParser::verifyBlockTrailer(const QByteArray& raw,
                                                              uint64_t file_offset,
                                                              int cb,
                                                              uint64_t bid) {
    auto trailer = readBlockTrailer(file_offset, cb);
    if (!trailer) {
        return std::unexpected(trailer.error());
    }
    // Declared size must repeat the BBT's cb.
    if (readLE<uint16_t>(*trailer, kBlockTrailerCbOffset) != static_cast<uint16_t>(cb)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    // dwCRC over the raw cb bytes.
    uint32_t block_crc = 0;
    if (!msPstWeakCrc(raw, 0, cb, block_crc)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    if (block_crc != readLE<uint32_t>(*trailer, kBlockTrailerCrcOffset)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    // wSig authenticates offset+bid; the trailer's own bid must match the BBT bid.
    if (msPstComputeSig(file_offset, bid) != readLE<uint16_t>(*trailer, kBlockTrailerSigOffset)) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    const uint64_t trailer_bid = m_is_unicode ? readLE<uint64_t>(*trailer, kBlockTrailerBidOffset)
                                              : readLE<uint32_t>(*trailer, kBlockTrailerBidOffset);
    if (trailer_bid != bid) {
        return std::unexpected(error_code::pst_integrity_check_failed);
    }
    return {};
}

std::expected<QByteArray, error_code> PstParser::decompressBlockIf4k(const QByteArray& raw,
                                                                     uint64_t file_offset,
                                                                     int cb) {
    using namespace sak::email;

    // Calculate aligned block size (512-byte increments for 4K pages)
    int aligned = ((cb + kBlock4kAlignment - 1) / kBlock4kAlignment) * kBlock4kAlignment;
    if ((aligned - cb) < kBlock4kFooterSize) {
        aligned += kBlock4kAlignment;
    }

    // Read the 24-byte block footer from the end of the aligned allocation. A truncated or
    // corrupt allocation that cannot yield the full footer is unreadable; fail closed rather
    // than returning the still-compressed bytes as if they were the block payload.
    const qint64 footer_pos = static_cast<qint64>(file_offset) + aligned - kBlock4kFooterSize;
    auto footer_result = readBytes(footer_pos, kBlock4kFooterSize);
    if (!footer_result || footer_result->size() != kBlock4kFooterSize) {
        return std::unexpected(error_code::read_error);
    }

    const uint16_t uncompressed_size = readLE<uint16_t>(*footer_result, kBlock4kUncompSizeOffset);

    if (uncompressed_size == 0 || uncompressed_size == static_cast<uint16_t>(cb)) {
        return raw;
    }

    // Block is zlib-compressed -- decompress using qUncompress.
    // qUncompress expects a 4-byte big-endian size prefix before the
    // raw zlib stream.
    QByteArray prefixed;
    prefixed.reserve(kInt32PropertyBytes + raw.size());
    uint32_t be_size = qToBigEndian(static_cast<uint32_t>(uncompressed_size));
    prefixed.append(reinterpret_cast<const char*>(&be_size), kInt32PropertyBytes);
    prefixed.append(raw);

    QByteArray decompressed = qUncompress(prefixed);
    if (decompressed.isEmpty()) {
        // The footer said this block is compressed; a decompress failure means the block is
        // corrupt. Fail closed -- returning the compressed bytes would feed garbage upstream.
        sak::logError("Block decompression failed at offset {}", std::to_string(file_offset));
        return std::unexpected(error_code::pst_decompression_failed);
    }

    // The footer's uncompressed_size is the block's declared inflated length; qUncompress trusts
    // the BE size prefix only for its allocation hint, so a stream that inflates to a different
    // length is a corrupt (or crafted) block. Enforce the exact-size invariant fail-closed rather
    // than handing a short/long buffer to the size-sensitive readers downstream.
    if (decompressed.size() != uncompressed_size) {
        sak::logError("Block decompressed size {} != declared {} at offset {}",
                      std::to_string(decompressed.size()),
                      std::to_string(uncompressed_size),
                      std::to_string(file_offset));
        return std::unexpected(error_code::pst_decompression_failed);
    }

    return decompressed;
}

void PstParser::decryptBlock(std::span<uint8_t> data) const {
    for (auto& byte : data) {
        byte = kDecryptTable[byte];
    }
}

// ============================================================================
// LTP Layer -- BTH Multi-Level Traversal
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readBthLeafData(
    const QByteArray& heap_data,
    uint32_t node_hid,
    uint8_t key_size,
    int level,
    const QVector<int>& block_offsets) {
    QSet<uint32_t> visited_hids;
    return readBthLeafDataGuarded(node_hid,
                                  level,
                                  BthWalk{.heap_data = heap_data,
                                          .key_size = key_size,
                                          .block_offsets = block_offsets,
                                          .visited_hids = visited_hids});
}

std::expected<QByteArray, error_code> PstParser::readBthLeafDataGuarded(uint32_t node_hid,
                                                                        int level,
                                                                        BthWalk walk) {
    // Reject a heap HID revisited on this traversal: a valid BTH references each HID exactly once,
    // so a repeat is a cycle or a fan-out bomb (a node listing the same child HID repeatedly, which
    // without this guard expands as entry_count^idx_levels). Stop the branch on both.
    if (walk.visited_hids.contains(node_hid)) {
        return QByteArray{};
    }
    walk.visited_hids.insert(node_hid);

    auto node_data = readHeapOnNode(walk.heap_data, node_hid, walk.block_offsets);
    if (!node_data) {
        return std::unexpected(node_data.error());
    }

    if (level == 0) {
        return *node_data;
    }

    const int entry_size = walk.key_size + kBthChildHidSize;
    if (entry_size == 0) {
        return QByteArray{};
    }

    const int entry_count = static_cast<int>(node_data->size() / entry_size);
    QByteArray combined;

    for (int idx = 0; idx < entry_count; ++idx) {
        const int offset = idx * entry_size;
        if (offset + entry_size > node_data->size()) {
            break;
        }
        const uint32_t child_hid = readLE<uint32_t>(*node_data, offset + walk.key_size);
        if (child_hid == 0) {
            continue;
        }
        auto child_result = readBthLeafDataGuarded(child_hid, level - 1, walk);
        if (child_result) {
            // Cap the assembled leaf the same way readDataTree caps its data tree (line ~1821):
            // an attacker-crafted heap with many distinct leaf HIDs could otherwise grow
            // `combined` past INT_MAX, and the record counts derived from combined.size() (all
            // int) would truncate. Fail closed once the sum would exceed the ceiling, which also
            // keeps those downstream int record counts/offsets in range.
            if (static_cast<int64_t>(combined.size()) + child_result->size() >
                kMaxAssembledDataTreeSize) {
                return std::unexpected(error_code::pst_corrupted_btree);
            }
            combined.append(*child_result);
        }
    }

    return combined;
}

// ============================================================================
// LTP Layer -- BTH Collection & Property Parsing
// ============================================================================

std::expected<PstParser::BthLeafResult, error_code> PstParser::collectBthLeafData(
    const HeapContext& ctx, uint8_t expected_client_sig) {
    if (ctx.heap_data.size() < kHnHeaderMinSize) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(ctx.heap_data[kHnSignatureOffset]) != kHnSignature) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(ctx.heap_data[kHnClientSignatureOffset]) != expected_client_sig) {
        return std::unexpected(error_code::pst_property_context_error);
    }

    const uint32_t hid_root = readLE<uint32_t>(ctx.heap_data, kHnRootHidOffset);
    auto bth_header = readHeapOnNode(ctx.heap_data, hid_root, ctx.block_offsets);
    if (!bth_header) {
        return std::unexpected(bth_header.error());
    }
    if (bth_header->size() < kBthHeaderMinSize) {
        return std::unexpected(error_code::pst_property_context_error);
    }
    if (static_cast<uint8_t>((*bth_header)[0]) != kBthSignature) {
        return std::unexpected(error_code::pst_property_context_error);
    }

    const uint8_t key_size = static_cast<uint8_t>((*bth_header)[kBthKeySizeOffset]);
    const uint8_t data_size = static_cast<uint8_t>((*bth_header)[kBthDataSizeOffset]);
    const uint8_t idx_levels = static_cast<uint8_t>((*bth_header)[kBthIndexLevelsOffset]);
    const uint32_t bth_root_hid = readLE<uint32_t>(*bth_header, kBthRootHidOffset);
    if (bth_root_hid == 0) {
        return BthLeafResult{};
    }

    auto leaf_result =
        (idx_levels == 0)
            ? readHeapOnNode(ctx.heap_data, bth_root_hid, ctx.block_offsets)
            : readBthLeafData(ctx.heap_data, bth_root_hid, key_size, idx_levels, ctx.block_offsets);
    if (!leaf_result) {
        return std::unexpected(leaf_result.error());
    }
    return BthLeafResult{.leaf_data = std::move(*leaf_result),
                         .key_size = key_size,
                         .data_size = data_size};
}

QVector<sak::MapiProperty> PstParser::parsePropertyRecords(const BthLeafResult& bth,
                                                           const HeapContext& ctx) {
    const int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return {};
    }

    const int record_count = static_cast<int>(bth.leaf_data.size() / record_size);
    QVector<sak::MapiProperty> properties;
    properties.reserve(record_count);

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        const int rec_off = rec_idx * record_size;
        if (rec_off + record_size > bth.leaf_data.size()) {
            break;
        }

        sak::MapiProperty prop;
        prop.tag_id = readLE<uint16_t>(bth.leaf_data, rec_off);

        if (bth.data_size >= kPropertyRecordMinInlineSize) {
            prop.tag_type = readLE<uint16_t>(bth.leaf_data, rec_off + bth.key_size);
            uint32_t value_ref = readLE<uint32_t>(bth.leaf_data,
                                                  rec_off + bth.key_size + kPropertyValueRefOffset);

            if (isPcVariableType(prop.tag_type)) {
                auto resolved =
                    resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
                if (resolved) {
                    prop.raw_value = std::move(*resolved);
                }
            } else {
                prop.raw_value.resize(kPropertyValueRefSize);
                std::memcpy(prop.raw_value.data(), &value_ref, kPropertyValueRefSize);
            }
        }

        prop.property_name = propertyIdToName(prop.tag_id);
        prop.display_value = formatPropertyValue(prop.tag_type, prop.raw_value);
        properties.append(std::move(prop));
    }
    return properties;
}

// ============================================================================
// LTP Layer -- Property Context
// ============================================================================

std::expected<QVector<sak::MapiProperty>, error_code> PstParser::readPropertyContext(uint64_t nid) {
    auto node_it = m_nbt_cache.find(nid);
    if (node_it == m_nbt_cache.end()) {
        sak::logWarning("PstParser: NID 0x{:08X} not found in NBT cache ({} entries)",
                        static_cast<uint32_t>(nid),
                        m_nbt_cache.size());
        return std::unexpected(error_code::pst_node_not_found);
    }

    const auto& node = node_it.value();
    if (node.data_bid == 0) {
        return QVector<sak::MapiProperty>{};
    }

    HeapContext ctx;
    auto data_result = readDataTree(node.data_bid, &ctx.block_offsets);
    if (!data_result) {
        return std::unexpected(data_result.error());
    }
    ctx.heap_data = std::move(*data_result);

    if (node.subnode_bid != 0) {
        // Fail closed on a sub-node BTree failure: silently continuing leaves subnode_map empty,
        // so every HNID that points into a sub-node resolves to an empty value and the property
        // context is returned as if those (often the body/large properties) were simply absent.
        auto sn_result = readSubNodeBTree(node.subnode_bid);
        if (!sn_result) {
            return std::unexpected(sn_result.error());
        }
        ctx.subnode_map = std::move(*sn_result);
    }

    auto bth = collectBthLeafData(ctx, kPcSignature);
    if (!bth) {
        return std::unexpected(bth.error());
    }
    if (bth->leaf_data.isEmpty()) {
        return QVector<sak::MapiProperty>{};
    }
    return parsePropertyRecords(*bth, ctx);
}

// ============================================================================
// LTP Layer -- Table Context
// ============================================================================

std::expected<QVector<QVector<sak::MapiProperty>>, error_code> PstParser::readTableContext(
    uint64_t nid) {
    auto node_it = m_nbt_cache.find(nid);
    if (node_it == m_nbt_cache.end()) {
        return std::unexpected(error_code::pst_node_not_found);
    }

    const auto& node = node_it.value();
    if (node.data_bid == 0) {
        return QVector<QVector<sak::MapiProperty>>{};
    }

    HeapContext ctx;
    auto data_result = readDataTree(node.data_bid, &ctx.block_offsets);
    if (!data_result) {
        return std::unexpected(data_result.error());
    }
    ctx.heap_data = std::move(*data_result);

    auto valid = validateHeapForTc(ctx.heap_data);
    if (!valid) {
        return std::unexpected(valid.error());
    }

    const uint32_t hid_root = readLE<uint32_t>(ctx.heap_data, kHnRootHidOffset);
    auto tc_raw = readHeapOnNode(ctx.heap_data, hid_root, ctx.block_offsets);
    if (!tc_raw) {
        return std::unexpected(tc_raw.error());
    }

    auto tc = parseTcInfo(*tc_raw);
    if (!tc) {
        return std::unexpected(tc.error());
    }
    if (tc->hnid_rows == 0) {
        return QVector<QVector<sak::MapiProperty>>{};
    }

    const TcRowMatrix matrix = loadTcRowData(*tc, node, ctx);
    if (matrix.data.isEmpty()) {
        return QVector<QVector<sak::MapiProperty>>{};
    }

    return buildTcRows(matrix, *tc, ctx);
}

std::expected<void, sak::error_code> PstParser::validateHeapForTc(const QByteArray& heap_data) {
    if (heap_data.size() < kHnHeaderMinSize) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(heap_data[kHnSignatureOffset]) != kHnSignature) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(heap_data[kHnClientSignatureOffset]) != kTcSignature) {
        return std::unexpected(error_code::pst_table_context_error);
    }
    return {};
}

PstParser::TcRowMatrix PstParser::loadTcRowData(const TcInfo& tc,
                                                const sak::PstNode& node,
                                                HeapContext& ctx) {
    TcRowMatrix matrix;

    // Load the subnode map once (used for both row-matrix lookup and per-cell
    // HNID resolution).  The old code walked the subnode BTree twice.
    if (node.subnode_bid != 0) {
        auto sn_result = readSubNodeBTree(node.subnode_bid);
        if (sn_result) {
            ctx.subnode_map = std::move(*sn_result);
        }
    }

    if ((tc.hnid_rows & kNidTypeMask) == 0) {
        // Heap-stored: single allocation, no inter-block padding.
        auto heap_r = readHeapOnNode(ctx.heap_data, tc.hnid_rows, ctx.block_offsets);
        if (heap_r) {
            matrix.data = std::move(*heap_r);
            matrix.block_ends.append(static_cast<int>(matrix.data.size()));
        }
    } else {
        // Subnode-stored: row matrix spans multiple data blocks with per-
        // block tail padding (MS-PST section 2.3.4.4.1).  Capture block boundaries.
        auto sub = ctx.subnode_map.find(tc.hnid_rows);
        if (sub != ctx.subnode_map.end()) {
            auto sub_data = readDataTree(sub->data_bid, &matrix.block_ends);
            if (sub_data) {
                matrix.data = std::move(*sub_data);
            }
        }
    }

    return matrix;
}

QVector<QVector<sak::MapiProperty>> PstParser::buildTcRows(const TcRowMatrix& matrix,
                                                           const TcInfo& tc,
                                                           const HeapContext& ctx) {
    const int row_size = tc.rgib_tci_bm;
    if (row_size == 0 || matrix.data.isEmpty() || matrix.block_ends.isEmpty()) {
        return {};
    }

    // Per MS-PST section 2.3.4.4.1, when the row matrix is stored as a sub-node,
    // each data block contains floor(block_bytes / row_size) rows followed
    // by padding.  dwRowIndex from TCROWID is a *logical* row number across
    // all blocks, so we must convert it to (block_i, row_in_block) and skip
    // the padding of prior blocks.
    const int block_count = static_cast<int>(matrix.block_ends.size());
    const int first_block_bytes = matrix.block_ends.first();
    const int rows_per_block = (block_count > 1) ? (first_block_bytes / row_size)
                                                 : static_cast<int>(matrix.data.size() / row_size);

    // MS-PST section 2.3.4.3.1 -- TCROWID BTH is the authoritative list of live rows.
    // The row matrix often contains stale/deleted/padding slots that are NOT
    // valid rows; iterating the matrix directly yields garbled subjects and
    // inflated counts.  Walk the row-index BTH and materialize only the rows
    // it references (in their natural order).
    QVector<uint32_t> live_row_indices = collectTcLiveRowIndices(tc, ctx);
    if (live_row_indices.isEmpty()) {
        // A present-but-unreadable TCROWID BTH (hid_row_index != 0 yet no live rows
        // parsed) is corruption, not a genuinely index-less table: fail closed to no
        // rows rather than enumerating every physical matrix slot, which would surface
        // stale/deleted/padding slots as if they were live rows. Fall back to contiguous
        // enumeration only when the table truly carries no row-index BTH.
        if (tc.hid_row_index != 0) {
            return {};
        }
        live_row_indices =
            fallbackTcRowIndices(block_count, rows_per_block, matrix.data.size() / row_size);
    }

    QVector<QVector<sak::MapiProperty>> rows;
    rows.reserve(live_row_indices.size());
    for (const uint32_t row_index : live_row_indices) {
        const int row_off = tcRowOffset(matrix, row_index, row_size, rows_per_block, block_count);
        if (row_off < 0) {
            continue;
        }
        rows.append(materializeTcRow(matrix.data, row_off, row_size, tc, ctx));
    }
    return rows;
}

QVector<uint32_t> PstParser::fallbackTcRowIndices(int block_count,
                                                  int rows_per_block,
                                                  qsizetype max_valid_rows) {
    QVector<uint32_t> indices;
    // Widen before the multiply: block_count and rows_per_block are int and both derive
    // from parsed PST bytes, so their 32-bit product can exceed INT32_MAX and wrap to a
    // bogus (possibly negative) reserve.
    const qsizetype slot_count = static_cast<qsizetype>(block_count) * rows_per_block;
    // Fail closed on a corrupt geometry. In a well-formed matrix every block but the last is
    // the same size, so the physical-slot count is at most max_valid_rows (data bytes /
    // row_size) plus one block's worth of rows. A crafted TC with a large first block and
    // many tiny later blocks inflates block_count*rows_per_block far past that, which would
    // otherwise force a multi-GB reservation of padding slots; reject the whole table (as the
    // hid_row_index != 0 path above does) rather than enumerating it. The bound never rejects
    // a uniform-block matrix, and it keeps the inner (b*rows_per_block)+r below 2^31.
    if (slot_count > max_valid_rows + rows_per_block) {
        return {};
    }
    indices.reserve(slot_count);
    for (int b = 0; b < block_count; ++b) {
        for (int r = 0; r < rows_per_block; ++r) {
            indices.append(static_cast<uint32_t>((b * rows_per_block) + r));
        }
    }
    return indices;
}

QVector<sak::MapiProperty> PstParser::materializeTcRow(const QByteArray& row_data,
                                                       int row_off,
                                                       int row_size,
                                                       const TcInfo& tc,
                                                       const HeapContext& ctx) {
    QVector<sak::MapiProperty> row_props;
    row_props.reserve(tc.columns.size());
    const TcRowView row_view{.row_off = row_off,
                             .row_size = row_size,
                             .ceb_off = row_off + tc.rgib_tci_1b};
    for (const auto& col : tc.columns) {
        row_props.append(buildTcCell(row_data, row_view, col, ctx));
    }
    return row_props;
}

int PstParser::tcRowOffset(const TcRowMatrix& matrix,
                           uint32_t row_index,
                           int row_size,
                           int rows_per_block,
                           int block_count) {
    if (rows_per_block <= 0 || block_count <= 0) {
        return -1;
    }
    // Do the division in unsigned 64-bit: row_index is untrusted file data, and a
    // value >= 0x80000000 cast to int becomes negative, producing a negative
    // block index that reads block_ends out of bounds.
    const uint64_t block_u = static_cast<uint64_t>(row_index) /
                             static_cast<uint32_t>(rows_per_block);
    const uint64_t row_in_block_u = static_cast<uint64_t>(row_index) %
                                    static_cast<uint32_t>(rows_per_block);
    if (block_u >= static_cast<uint64_t>(block_count) ||
        block_u >= static_cast<uint64_t>(matrix.block_ends.size())) {
        return -1;
    }
    const int block_i = static_cast<int>(block_u);
    const int row_in_block = static_cast<int>(row_in_block_u);
    const int block_start = (block_i == 0) ? 0 : matrix.block_ends[block_i - 1];
    const int block_end = matrix.block_ends[block_i];
    const int off = block_start + (row_in_block * row_size);
    if (off < 0 || off + row_size > block_end) {
        return -1;
    }
    return off;
}

QVector<uint32_t> PstParser::collectTcLiveRowIndices(const TcInfo& tc, const HeapContext& ctx) {
    QVector<uint32_t> live_row_indices;
    if (tc.hid_row_index == 0) {
        return live_row_indices;
    }
    auto bth_header = readHeapOnNode(ctx.heap_data, tc.hid_row_index, ctx.block_offsets);
    if (!bth_header || bth_header->size() < kBthHeaderMinSize ||
        static_cast<uint8_t>((*bth_header)[0]) != kBthSignature) {
        return live_row_indices;
    }
    const uint8_t key_size = static_cast<uint8_t>((*bth_header)[kBthKeySizeOffset]);
    const uint8_t data_size = static_cast<uint8_t>((*bth_header)[kBthDataSizeOffset]);
    const uint8_t idx_levels = static_cast<uint8_t>((*bth_header)[kBthIndexLevelsOffset]);
    const uint32_t root_hid = readLE<uint32_t>(*bth_header, kBthRootHidOffset);
    // TCROWID record layout: dwRowID (key, 4 bytes) + dwRowIndex (data, 4 bytes).
    if (key_size != kInt32PropertyBytes || data_size != kInt32PropertyBytes || root_hid == 0) {
        return live_row_indices;
    }
    auto leaf =
        (idx_levels == 0)
            ? readHeapOnNode(ctx.heap_data, root_hid, ctx.block_offsets)
            : readBthLeafData(ctx.heap_data, root_hid, key_size, idx_levels, ctx.block_offsets);
    if (!leaf) {
        return live_row_indices;
    }
    return extractTcRowIndicesFromLeaf(*leaf);
}

QVector<uint32_t> PstParser::extractTcRowIndicesFromLeaf(const QByteArray& leaf) {
    constexpr int kTcRowIdRecordSize = 8;
    QVector<uint32_t> live_row_indices;
    const int record_count = static_cast<int>(leaf.size() / kTcRowIdRecordSize);
    live_row_indices.reserve(record_count);
    for (int rec = 0; rec < record_count; ++rec) {
        live_row_indices.append(
            readLE<uint32_t>(leaf, (rec * kTcRowIdRecordSize) + kBthRowIdValueOffset));
    }
    return live_row_indices;
}

sak::MapiProperty PstParser::buildTcCell(const QByteArray& row_data,
                                         const TcRowView& row_view,
                                         const TcColDesc& col,
                                         const HeapContext& ctx) {
    sak::MapiProperty prop;
    prop.tag_id = col.prop_id;
    prop.tag_type = col.prop_type;
    prop.property_name = propertyIdToName(col.prop_id);

    bool cell_exists = true;
    const int ceb_byte = row_view.ceb_off + (col.i_bit / kBitPackingBitsPerByte);
    const int ceb_bit = kBitPackingHighBitIndex - (col.i_bit % kBitPackingBitsPerByte);
    if (ceb_byte < row_view.row_off + row_view.row_size) {
        cell_exists = (((static_cast<uint8_t>(row_data[ceb_byte]) >> ceb_bit) & 1) != 0);
    }

    // Bound the cell against THIS row's end (row_off + row_size), not the whole matrix buffer:
    // a column whose ib_data/cb_data run past the fixed-row layout must not be allowed to read
    // bytes that belong to the next row (or the block padding) just because the matrix is large.
    const int cell_off = row_view.row_off + col.ib_data;
    const int row_end = row_view.row_off + row_view.row_size;
    if (cell_exists && cell_off >= row_view.row_off && cell_off + col.cb_data <= row_end &&
        row_end <= row_data.size()) {
        prop.raw_value = row_data.mid(cell_off, col.cb_data);
        if (isHnidResolvableType(col.prop_type) && col.cb_data == kPropertyValueRefSize) {
            const uint32_t hnid = readLE<uint32_t>(row_data, cell_off);
            auto resolved = resolveHnid(hnid, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
            if (resolved) {
                prop.raw_value = std::move(*resolved);
            }
        }
        prop.display_value = formatPropertyValue(col.prop_type, prop.raw_value);
    }
    return prop;
}

// ============================================================================
// LTP Layer -- Heap-on-Node
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readHeapOnNode(const QByteArray& heap_data,
                                                                uint32_t hn_id,
                                                                const QVector<int>& block_offsets) {
    if (hn_id == 0 || heap_data.isEmpty()) {
        return QByteArray{};
    }

    const auto [hid_index, hid_block_index] = unpackHid(hn_id);

    auto block_off =
        resolveHnBlockOffset(hid_block_index, block_offsets, static_cast<int>(heap_data.size()));
    if (!block_off) {
        return std::unexpected(block_off.error());
    }
    const int block_offset = *block_off;

    const uint16_t ib_hnpm = readLE<uint16_t>(heap_data, block_offset);
    const int page_map_offset = block_offset + ib_hnpm;
    if (page_map_offset + kInt32PropertyBytes > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    const uint16_t alloc_count = readLE<uint16_t>(heap_data, page_map_offset);
    if (hid_index == 0 || hid_index > alloc_count) {
        return QByteArray{};
    }

    const int alloc_table_offset = page_map_offset + kInt32PropertyBytes;
    const int entry_offset = alloc_table_offset + ((hid_index - 1) * kHidOffsetRecordSize);
    const int next_offset = alloc_table_offset + (hid_index * kHidOffsetRecordSize);

    if (next_offset + kHidOffsetRecordSize > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    const uint16_t start = readLE<uint16_t>(heap_data, entry_offset);
    const uint16_t end = readLE<uint16_t>(heap_data, next_offset);

    const int abs_start = block_offset + start;
    const int abs_end = block_offset + end;

    if (abs_start > abs_end || abs_end > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    return heap_data.mid(abs_start, abs_end - abs_start);
}

PstParser::HidParts PstParser::unpackHid(uint32_t hn_id) const {
    // 64-bit 4K-page PFF files use a 5/14/13 bit layout for HID references
    // (type / hid_index / hid_block_index) instead of the standard 5/11/16.
    // Ref: libpff "64-bit 4k page table value reference".
    HidParts parts;
    parts.hid_index = m_is_4k
                          ? static_cast<uint16_t>((hn_id >> kHidIndexShift) & kHidIndexMask4k)
                          : static_cast<uint16_t>((hn_id >> kHidIndexShift) & kHidIndexMaskLegacy);
    parts.hid_block_index =
        m_is_4k ? static_cast<uint16_t>((hn_id >> kHidBlockIndexShift4k) & kHidBlockIndexMask4k)
                : static_cast<uint16_t>((hn_id >> kHidBlockIndexShiftLegacy) &
                                        kHidBlockIndexMaskLegacy);
    return parts;
}

// ============================================================================
// LTP Layer -- Unified HNID Resolution
// ============================================================================

std::expected<QByteArray, error_code> PstParser::resolveHnid(
    uint32_t hnid,
    const QByteArray& heap_data,
    const QVector<int>& block_offsets,
    const QHash<uint64_t, sak::PstNode>& subnode_map) {
    if (hnid == 0) {
        return QByteArray{};
    }

    // 1. Check sub-node BTree for this HNID
    if (subnode_map.contains(hnid)) {
        return readDataTree(subnode_map[hnid].data_bid);
    }

    // 2. Non-zero low 5 bits -> NID not in sub-node map
    if ((hnid & kNidTypeMask) != 0) {
        return QByteArray{};
    }

    // 3. HID -- delegate to heap reader (validates block range)
    return readHeapOnNode(heap_data, hnid, block_offsets);
}

// ============================================================================
// LTP Layer -- Sub-Node BTree
// ============================================================================

std::expected<QHash<uint64_t, sak::PstNode>, error_code> PstParser::readSubNodeBTree(
    uint64_t subnode_bid, int depth) {
    QSet<uint64_t> visited_bids;
    return readSubNodeBTreeGuarded(subnode_bid, depth, visited_bids);
}

std::expected<QHash<uint64_t, sak::PstNode>, error_code> PstParser::readSubNodeBTreeGuarded(
    uint64_t subnode_bid, int depth, QSet<uint64_t>& visited_bids) {
    if (depth > kMaxBTreeDepth) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    // Reject a block BID revisited on this traversal: a valid sub-node BTree references each block
    // once, so a repeat is a cycle or a fan-out bomb (entry_count^depth). Fail closed on both.
    if (visited_bids.contains(subnode_bid)) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }
    visited_bids.insert(subnode_bid);

    auto block_data = readBlock(subnode_bid);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }

    const auto& data = *block_data;
    if (data.size() < kBlockTreeHeaderSize) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    const uint8_t level = static_cast<uint8_t>(data[1]);
    const uint16_t entry_count = readLE<uint16_t>(data, kBlockTreeEntryCountOffset);
    constexpr int kSubnodeHeaderSize = 8;

    if (level == 0) {
        return readSubNodeLeafEntries(data, kSubnodeHeaderSize, entry_count);
    }
    return readSubNodeIntermediateEntries(
        data, kSubnodeHeaderSize, entry_count, depth, visited_bids);
}

QHash<uint64_t, sak::PstNode> PstParser::readSubNodeLeafEntries(const QByteArray& data,
                                                                int header_size,
                                                                uint16_t entry_count) {
    QHash<uint64_t, sak::PstNode> result;
    const int entry_size = m_is_unicode ? kSubnodeLeafSizeUnicode : kSubnodeLeafSizeAnsi;

    for (int entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
        const int offset = header_size + (entry_idx * entry_size);
        if (offset + entry_size > data.size()) {
            break;
        }

        sak::PstNode node;
        if (m_is_unicode) {
            node.node_id = readLE<uint64_t>(data, offset) & kNidLower32Mask;
            node.data_bid = readLE<uint64_t>(data, offset + kSubnodeDataBidOffsetUnicode);
            node.subnode_bid = readLE<uint64_t>(data, offset + kSubnodeSubnodeBidOffsetUnicode);
        } else {
            node.node_id = readLE<uint32_t>(data, offset);
            node.data_bid = readLE<uint32_t>(data, offset + kSubnodeDataBidOffsetAnsi);
            node.subnode_bid = readLE<uint32_t>(data, offset + kSubnodeSubnodeBidOffsetAnsi);
        }
        result.insert(node.node_id, node);
    }
    return result;
}

std::expected<QHash<uint64_t, sak::PstNode>, error_code> PstParser::readSubNodeIntermediateEntries(
    const QByteArray& data,
    int header_size,
    uint16_t entry_count,
    int depth,
    QSet<uint64_t>& visited_bids) {
    QHash<uint64_t, sak::PstNode> result;
    const int entry_size = m_is_unicode ? kSubnodeIntermediateSizeUnicode
                                        : kSubnodeIntermediateSizeAnsi;
    const int bid_offset = m_is_unicode ? kSubnodeIntermediateBidOffsetUnicode
                                        : kSubnodeIntermediateBidOffsetAnsi;

    for (int entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
        const int offset = header_size + (entry_idx * entry_size);
        if (offset + entry_size > data.size()) {
            break;
        }

        const uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset + bid_offset)
                                                : readLE<uint32_t>(data, offset + bid_offset);

        auto child_result = readSubNodeBTreeGuarded(child_bid, depth + 1, visited_bids);
        if (child_result) {
            for (auto it = child_result->begin(); it != child_result->end(); ++it) {
                result.insert(it.key(), it.value());
            }
        }
    }
    return result;
}

// ============================================================================
// LTP Layer -- Property Formatting
// ============================================================================

QString PstParser::formatPropertyValue(uint16_t prop_type, const QByteArray& raw_value) const {
    if (raw_value.isEmpty()) {
        return QString();
    }

    const auto& formatters = propFormatters();
    auto fmt_it = formatters.find(prop_type);
    if (fmt_it != formatters.end()) {
        return (*fmt_it)(raw_value);
    }

    // Unknown type -- hex dump for small values, size summary for large
    constexpr int kMaxInlineHexBytes = 16;
    if (raw_value.size() <= kMaxInlineHexBytes) {
        return raw_value.toHex(' ');
    }
    return QStringLiteral("(%1 bytes, type 0x%2)")
        .arg(raw_value.size())
        .arg(prop_type, kPropertyIdHexWidth, kPropertyIdHexBase, QLatin1Char('0'));
}

// clang-format off
static const QHash<uint16_t, QString>& corePropertyNameTable() {
    static const QHash<uint16_t, QString> kTable = {
        {sak::email::kPropIdSubject,              QStringLiteral("PR_SUBJECT")},
        {sak::email::kPropIdMessageClass,         QStringLiteral("PR_MESSAGE_CLASS")},
        {sak::email::kPropIdSenderName,           QStringLiteral("PR_SENDER_NAME")},
        {sak::email::kPropIdSenderEmail,          QStringLiteral("PR_SENDER_EMAIL_ADDRESS")},
        {sak::email::kPropIdSentRepresentingName, QStringLiteral("PR_SENT_REPRESENTING_NAME")},
        {sak::email::kPropIdSentRepresentingEmail,QStringLiteral("PR_SENT_REPRESENTING_EMAIL_ADDRESS")},
        {sak::email::kPropIdDisplayTo,            QStringLiteral("PR_DISPLAY_TO")},
        {sak::email::kPropIdDisplayCc,            QStringLiteral("PR_DISPLAY_CC")},
        {sak::email::kPropIdDisplayBcc,           QStringLiteral("PR_DISPLAY_BCC")},
        {sak::email::kPropIdMessageDeliveryTime,  QStringLiteral("PR_MESSAGE_DELIVERY_TIME")},
        {sak::email::kPropIdClientSubmitTime,     QStringLiteral("PR_CLIENT_SUBMIT_TIME")},
        {sak::email::kPropIdBody,                 QStringLiteral("PR_BODY")},
        {sak::email::kPropIdHtmlBody,             QStringLiteral("PR_HTML")},
        {sak::email::kPropIdRtfCompressed,        QStringLiteral("PR_RTF_COMPRESSED")},
        {sak::email::kPropIdTransportHeaders,     QStringLiteral("PR_TRANSPORT_MESSAGE_HEADERS")},
        {sak::email::kPropIdMessageFlags,         QStringLiteral("PR_MESSAGE_FLAGS")},
        {sak::email::kPropIdMessageSize,          QStringLiteral("PR_MESSAGE_SIZE")},
        {sak::email::kPropIdHasAttachments,       QStringLiteral("PR_HASATTACH")},
        {sak::email::kPropIdImportance,           QStringLiteral("PR_IMPORTANCE")},
        {sak::email::kPropIdSensitivity,          QStringLiteral("PR_SENSITIVITY")},
        {sak::email::kPropIdInternetMessageId,    QStringLiteral("PR_INTERNET_MESSAGE_ID")},
        {sak::email::kPropIdInReplyTo,            QStringLiteral("PR_IN_REPLY_TO_ID")},
        {sak::email::kPropIdDisplayName,          QStringLiteral("PR_DISPLAY_NAME")},
        {sak::email::kPropIdContentCount,         QStringLiteral("PR_CONTENT_COUNT")},
        {sak::email::kPropIdContentUnreadCount,   QStringLiteral("PR_CONTENT_UNREAD")},
        {sak::email::kPropIdSubfolders,           QStringLiteral("PR_SUBFOLDERS")},
        {sak::email::kPropIdContainerClass,       QStringLiteral("PR_CONTAINER_CLASS")},
        {sak::email::kPropIdAttachFilename,       QStringLiteral("PR_ATTACH_FILENAME")},
        {sak::email::kPropIdAttachLongFilename,   QStringLiteral("PR_ATTACH_LONG_FILENAME")},
        {sak::email::kPropIdAttachData,           QStringLiteral("PR_ATTACH_DATA_BIN")},
        {sak::email::kPropIdAttachSize,           QStringLiteral("PR_ATTACH_SIZE")},
        {sak::email::kPropIdAttachMimeTag,        QStringLiteral("PR_ATTACH_MIME_TAG")},
        {sak::email::kPropIdAttachContentId,      QStringLiteral("PR_ATTACH_CONTENT_ID")},
        {sak::email::kPropIdAttachMethod,         QStringLiteral("PR_ATTACH_METHOD")},
        {sak::email::kPropIdEmailAddress,         QStringLiteral("PR_EMAIL_ADDRESS")},
        {sak::email::kPropIdCompanyName,          QStringLiteral("PR_COMPANY_NAME")},
        {sak::email::kPropIdBusinessPhone,        QStringLiteral("PR_BUSINESS_TELEPHONE_NUMBER")},
        {sak::email::kPropIdMobilePhone,          QStringLiteral("PR_MOBILE_TELEPHONE_NUMBER")},
        {sak::email::kPropIdHomePhone,            QStringLiteral("PR_HOME_TELEPHONE_NUMBER")},
        {sak::email::kPropIdJobTitle,             QStringLiteral("PR_TITLE")},
        {sak::email::kPropIdGivenName,            QStringLiteral("PR_GIVEN_NAME")},
        {sak::email::kPropIdSurname,              QStringLiteral("PR_SURNAME")},
        {sak::email::kPropIdTaskPriority,         QStringLiteral("PR_PRIORITY")},
    };
    return kTable;
}

static const QHash<uint16_t, QString>& extendedPropertyNameTable() {
    static const QHash<uint16_t, QString> kTable = {
        {0x0029, QStringLiteral("PR_READ_RECEIPT_REQUESTED")},
        {0x003b, QStringLiteral("PR_SENT_REPRESENTING_SEARCH_KEY")},
        {0x003f, QStringLiteral("PR_RECEIVED_BY_ENTRYID")},
        {0x0040, QStringLiteral("PR_RECEIVED_BY_NAME")},
        {0x0041, QStringLiteral("PR_SENT_REPRESENTING_ENTRYID")},
        {0x0043, QStringLiteral("PR_RECEIVED_BY_SEARCH_KEY")},
        {0x0044, QStringLiteral("PR_RECEIVED_REPRESENTING_NAME")},
        {0x004f, QStringLiteral("PR_REPLY_RECIPIENT_ENTRIES")},
        {0x0050, QStringLiteral("PR_REPLY_RECIPIENT_NAMES")},
        {0x0051, QStringLiteral("PR_RECEIVED_BY_SEARCH_KEY")},
        {0x0052, QStringLiteral("PR_RECEIVED_REPRESENTING_SEARCH_KEY")},
        {0x0057, QStringLiteral("PR_MESSAGE_TO_ME")},
        {0x0058, QStringLiteral("PR_MESSAGE_CC_ME")},
        {0x0059, QStringLiteral("PR_MESSAGE_RECIP_ME")},
        {0x0064, QStringLiteral("PR_SENT_REPRESENTING_ADDRTYPE")},
        {0x0070, QStringLiteral("PR_CONVERSATION_TOPIC")},
        {0x0071, QStringLiteral("PR_CONVERSATION_INDEX")},
        {0x0075, QStringLiteral("PR_RECEIVED_BY_ADDRTYPE")},
        {0x0076, QStringLiteral("PR_RECEIVED_BY_EMAIL_ADDRESS")},
        {0x0077, QStringLiteral("PR_RECEIVED_REPRESENTING_ADDRTYPE")},
        {0x0078, QStringLiteral("PR_RECEIVED_REPRESENTING_EMAIL_ADDRESS")},
        {0x0c06, QStringLiteral("PR_NON_DELIVERY_REPORT_REQUESTED")},
        {0x0c19, QStringLiteral("PR_SENDER_ENTRYID")},
        {0x0c1d, QStringLiteral("PR_SENDER_SEARCH_KEY")},
        {0x0c1e, QStringLiteral("PR_SENDER_ADDRTYPE")},
        {0x3007, QStringLiteral("PR_CREATION_TIME")},
        {0x3008, QStringLiteral("PR_LAST_MODIFICATION_TIME")},
        {0x300b, QStringLiteral("PR_SEARCH_KEY")},
        {0x3fde, QStringLiteral("PR_INTERNET_CPID")},
        {0x3ff1, QStringLiteral("PR_MESSAGE_LOCALE_ID")},
        {0x3ff8, QStringLiteral("PR_CREATOR_NAME")},
        {0x3ff9, QStringLiteral("PR_CREATOR_ENTRYID")},
        {0x3ffa, QStringLiteral("PR_LAST_MODIFIER_NAME")},
        {0x3ffb, QStringLiteral("PR_LAST_MODIFIER_ENTRYID")},
        {0x3ffd, QStringLiteral("PR_MESSAGE_CODEPAGE")},
        {0x5d01, QStringLiteral("PR_SENDER_SMTP_ADDRESS")},
        {0x5d02, QStringLiteral("PR_SENT_REPRESENTING_SMTP_ADDRESS")},
        {0x5d07, QStringLiteral("PR_RECEIVED_BY_SMTP_ADDRESS")},
        {0x5d08, QStringLiteral("PR_RECEIVED_REPRESENTING_SMTP_ADDRESS")},
    };
    return kTable;
}

static const QHash<uint16_t, QString>& propertyNameTable() {
    static const QHash<uint16_t, QString> kTable = [] {
        auto table = corePropertyNameTable();
        table.insert(extendedPropertyNameTable());
        return table;
    }();
    return kTable;
}
// clang-format on

QString PstParser::propertyIdToName(uint16_t prop_id) {
    const auto& table = propertyNameTable();
    auto name_it = table.find(prop_id);
    if (name_it != table.end()) {
        return name_it.value();
    }
    return QStringLiteral("0x%1").arg(
        prop_id, kPropertyIdHexWidth, kPropertyIdHexBase, QLatin1Char('0'));
}

// ============================================================================
// Messaging Layer -- Folder Hierarchy
// ============================================================================

std::expected<sak::PstFolderTree, error_code> PstParser::buildFolderHierarchy(uint64_t root_nid,
                                                                              int depth) {
    QSet<uint64_t> visited_nids;
    return buildFolderHierarchyGuarded(root_nid, depth, visited_nids);
}

std::expected<sak::PstFolderTree, error_code> PstParser::buildFolderHierarchyGuarded(
    uint64_t root_nid, int depth, QSet<uint64_t>& visited_nids) {
    if (depth > sak::email::kMaxFolderDepth) {
        return std::unexpected(error_code::pst_folder_traversal_error);
    }
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return std::unexpected(error_code::operation_cancelled);
    }

    // Reject a folder NID revisited on this traversal: valid folders form a tree (each NID appears
    // once), so a repeat is a cycle or a fan-out bomb (a hierarchy table listing the same child
    // thousands of times, which without this guard expands as children^depth). Fail closed.
    if (visited_nids.contains(root_nid)) {
        return std::unexpected(error_code::pst_folder_traversal_error);
    }
    visited_nids.insert(root_nid);

    auto props_result = readPropertyContext(root_nid);
    if (!props_result) {
        return std::unexpected(props_result.error());
    }

    sak::PstFolder folder;
    folder.node_id = root_nid;

    const auto& setters = folderSetters();
    for (const auto& prop : *props_result) {
        auto setter_it = setters.find(prop.tag_id);
        if (setter_it != setters.end()) {
            (*setter_it)(folder, prop);
        }
    }

    loadChildFolders(folder, root_nid, depth, visited_nids);

    folder.subfolder_count = static_cast<int>(folder.children.size());

    sak::PstFolderTree tree;
    tree.append(std::move(folder));
    return tree;
}

void PstParser::loadChildFolders(sak::PstFolder& folder,
                                 uint64_t root_nid,
                                 int depth,
                                 QSet<uint64_t>& visited_nids) {
    const uint64_t hierarchy_nid = (root_nid & ~kNidTypeMask) | sak::email::kNidTypeHierarchyTable;

    sak::logInfo(
        "PstParser: folder 0x{:X} hierarchy NID=0x{:X}, "
        "in cache={}",
        root_nid,
        hierarchy_nid,
        m_nbt_cache.contains(hierarchy_nid));

    if (!m_nbt_cache.contains(hierarchy_nid)) {
        return;
    }

    auto htable_result = readTableContext(hierarchy_nid);
    if (!htable_result) {
        sak::logWarning("PstParser: readTableContext(0x{:X}) failed: {}",
                        hierarchy_nid,
                        sak::to_string(htable_result.error()));
        return;
    }

    sak::logInfo("PstParser: hierarchy table for 0x{:X} has {} rows",
                 root_nid,
                 htable_result->size());
    auto child_nids = extractChildNids(*htable_result);
    for (const uint64_t child_nid : child_nids) {
        auto child_tree = buildFolderHierarchyGuarded(child_nid, depth + 1, visited_nids);
        // Surface, rather than silently swallow, a child that fails to parse or yields no
        // subtree: dropping it produces a partial folder tree that looks complete. Log each drop
        // (with the NID and reason) so a truncated/corrupt hierarchy is visible in diagnostics.
        if (!child_tree) {
            sak::logWarning("PstParser: child folder 0x{:X} failed to parse: {} (partial tree)",
                            child_nid,
                            sak::to_string(child_tree.error()));
            continue;
        }
        if (child_tree->isEmpty()) {
            sak::logWarning("PstParser: child folder 0x{:X} produced an empty subtree (dropped)",
                            child_nid);
            continue;
        }
        auto child_folder = (*child_tree)[0];
        child_folder.parent_node_id = root_nid;
        folder.children.append(std::move(child_folder));
    }
}

// ============================================================================
// Messaging Layer -- Contents Table
// ============================================================================

std::expected<QVector<sak::PstItemSummary>, error_code> PstParser::readContentsTable(
    uint64_t contents_nid, int offset, int limit) {
    auto table_result = readTableContext(contents_nid);
    if (!table_result) {
        if (table_result.error() == error_code::pst_node_not_found) {
            return QVector<sak::PstItemSummary>{};
        }
        return std::unexpected(table_result.error());
    }

    const auto& rows = *table_result;
    // Clamp the pagination offset: a negative offset (reachable when a caller's
    // page*page_size int-multiply overflows) must not produce a negative start
    // index and read rows[] out of bounds. Clamp to [0, rows.size()].
    const int start = std::clamp(offset, 0, static_cast<int>(rows.size()));
    const int end_idx = (limit > 0) ? std::min(start + limit, static_cast<int>(rows.size()))
                                    : static_cast<int>(rows.size());

    QVector<sak::PstItemSummary> items;
    items.reserve(end_idx - start);

    const auto& setters = summarySetters();
    for (int row_idx = start; row_idx < end_idx; ++row_idx) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return std::unexpected(error_code::operation_cancelled);
        }

        sak::PstItemSummary item;
        for (const auto& col : rows[row_idx]) {
            auto setter_it = setters.find(col.tag_id);
            if (setter_it != setters.end()) {
                (*setter_it)(item, col);
            }
        }
        items.append(std::move(item));
    }

    return items;
}

// ============================================================================
// Messaging Layer -- Full Message Read
// ============================================================================

std::expected<sak::PstItemDetail, error_code> PstParser::readMessage(uint64_t message_nid) {
    auto props_result = readPropertyContext(message_nid);
    if (!props_result) {
        return std::unexpected(props_result.error());
    }

    sak::PstItemDetail detail;
    detail.node_id = message_nid;

    const auto& setters = detailSetters();
    for (const auto& prop : *props_result) {
        auto setter_it = setters.find(prop.tag_id);
        if (setter_it != setters.end()) {
            (*setter_it)(detail, prop);
        }
    }

    // Propagate an attachment-read failure rather than returning an attachment-free message:
    // silently dropping it would present a message that has attachments as if it had none
    // (data loss the caller cannot detect). A message with no attachments returns an empty
    // vector (success), so this only fails on a genuine parse error.
    auto att_result = readAttachments(message_nid);
    if (!att_result) {
        return std::unexpected(att_result.error());
    }
    detail.attachments = std::move(*att_result);

    // Ensure both body representations are always populated when the message
    // has any body at all.  Many PST messages store only PR_HTML (no PR_BODY);
    // downstream consumers (the Plain Text view, search indexers, exporters)
    // must not have to re-implement HTML-to-text conversion.
    //
    // The body is attacker-authored, so it goes through the same sanitizer the viewers and the
    // export writers use before it is parsed -- otherwise script/handler text could surface as
    // "plain text" in the Plain Text view and in every exported artifact. No resource-denying
    // document is needed here: toPlainText() never lays the document out, so nothing is loaded.
    if (detail.body_plain.isEmpty() && !detail.body_html.isEmpty()) {
        QTextDocument doc;
        doc.setHtml(sak::sanitizeEmailBodyHtml(detail.body_html));
        detail.body_plain = doc.toPlainText();
    }

    return detail;
}

// ============================================================================
// Messaging Layer -- Lightweight Sender Read
// ============================================================================

std::pair<QString, QString> PstParser::extractSenderFromLeaf(const BthLeafResult& bth,
                                                             const HeapContext& ctx) {
    const int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return {};
    }

    const int record_count = static_cast<int>(bth.leaf_data.size() / record_size);
    const auto& slot_map = senderPropSlots();
    QString results[kResolvedRecipientPartCount];

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        const int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }

        const uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
        auto slot_it = slot_map.find(prop_id);
        if (slot_it == slot_map.end()) {
            continue;
        }
        if (!results[slot_it.value()].isEmpty()) {
            continue;
        }

        const uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
        uint32_t value_ref = readLE<uint32_t>(bth.leaf_data,
                                              rec_offset + bth.key_size + kPropertyValueRefOffset);

        QByteArray raw_value;
        if (isPcVariableType(prop_type)) {
            auto resolved =
                resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
            if (resolved) {
                raw_value = std::move(*resolved);
            }
        } else {
            raw_value.resize(kPropertyValueRefSize);
            std::memcpy(raw_value.data(), &value_ref, kPropertyValueRefSize);
        }

        results[slot_it.value()] = formatPropertyValue(prop_type, raw_value);
        if (!results[0].isEmpty() && !results[1].isEmpty()) {
            break;
        }
    }

    return {results[0], results[1]};
}

bool PstParser::loadNodeHeapContext(const sak::PstNode& entry, HeapContext& ctx) {
    auto data_result = readDataTree(entry.data_bid, &ctx.block_offsets);
    if (!data_result) {
        return false;
    }
    ctx.heap_data = std::move(*data_result);

    if (entry.subnode_bid != 0) {
        auto sn_result = readSubNodeBTree(entry.subnode_bid);
        if (sn_result) {
            ctx.subnode_map = std::move(*sn_result);
        }
    }
    return true;
}

QString PstParser::readBthRecordValue(const BthLeafResult& bth,
                                      const HeapContext& ctx,
                                      int rec_offset) {
    const uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
    uint32_t value_ref = readLE<uint32_t>(bth.leaf_data,
                                          rec_offset + bth.key_size + kPropertyValueRefOffset);
    QByteArray raw_value;
    if (isPcVariableType(prop_type)) {
        auto resolved = resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
        if (resolved) {
            raw_value = std::move(*resolved);
        }
    } else {
        raw_value.resize(kPropertyValueRefSize);
        std::memcpy(raw_value.data(), &value_ref, kPropertyValueRefSize);
    }
    return formatPropertyValue(prop_type, raw_value);
}

void PstParser::scanBthForSubjectAndClass(const BthLeafResult& bth,
                                          const HeapContext& ctx,
                                          sak::PstItemSummary& item,
                                          bool need_subject,
                                          bool need_class) {
    const int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return;
    }
    const int record_count = static_cast<int>(bth.leaf_data.size() / record_size);
    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        const int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }
        const uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
        if (need_subject && prop_id == sak::email::kPropIdSubject) {
            item.subject = stripSubjectPrefix(readBthRecordValue(bth, ctx, rec_offset));
            need_subject = false;
        }
        if (need_class && prop_id == sak::email::kPropIdMessageClass) {
            item.item_type = classifyMessageClass(readBthRecordValue(bth, ctx, rec_offset));
            need_class = false;
        }
        if (!need_subject && !need_class) {
            break;
        }
    }
}

void PstParser::enrichItemFromBth(sak::PstItemSummary& item,
                                  const BthLeafResult& bth,
                                  const HeapContext& ctx) {
    if (item.sender_name.isEmpty()) {
        auto [name, email] = extractSenderFromLeaf(bth, ctx);
        if (!name.isEmpty()) {
            item.sender_name = name;
        }
        if (item.sender_email.isEmpty() && !email.isEmpty()) {
            item.sender_email = email;
        }
    }

    const bool need_class = (item.item_type == sak::EmailItemType::Unknown);
    if (need_class) {
        const bool dont_touch_subject = false;
        scanBthForSubjectAndClass(bth, ctx, item, dont_touch_subject, need_class);
    }
}

void PstParser::enrichSingleItemProps(sak::PstItemSummary& item) {
    if (item.node_id == 0) {
        return;
    }

    const bool need_sender = item.sender_name.isEmpty();
    const bool need_class = (item.item_type == sak::EmailItemType::Unknown);
    if (!need_sender && !need_class) {
        return;
    }

    auto node_it = m_nbt_cache.find(item.node_id);
    if (node_it == m_nbt_cache.end()) {
        return;
    }
    if (node_it->data_bid == 0) {
        return;
    }

    HeapContext ctx;
    if (!loadNodeHeapContext(*node_it, ctx)) {
        return;
    }

    auto bth = collectBthLeafData(ctx, kPcSignature);
    if (!bth) {
        return;
    }

    enrichItemFromBth(item, *bth, ctx);
}

void PstParser::enrichItemSenders(QVector<sak::PstItemSummary>& items) {
    for (auto& item : items) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        enrichSingleItemProps(item);
    }
}

// ============================================================================
// Messaging Layer -- Attachments
// ============================================================================

std::expected<sak::PstAttachmentInfo, error_code> PstParser::readSingleAttachment(
    const sak::PstNode& subnode, int att_index) {
    HeapContext ctx;
    auto att_data = readDataTree(subnode.data_bid, &ctx.block_offsets);
    if (!att_data) {
        return std::unexpected(att_data.error());
    }
    ctx.heap_data = std::move(*att_data);

    if (subnode.subnode_bid != 0) {
        // Fail closed on a sub-node BTree failure to match this function's documented contract
        // (it is the index-alignment gate for readAttachmentData): swallowing it would report an
        // embedded-message attachment as parsed while its subnode-resolved fields silently vanish.
        auto sn = readSubNodeBTree(subnode.subnode_bid);
        if (!sn) {
            return std::unexpected(sn.error());
        }
        ctx.subnode_map = std::move(*sn);
    }

    auto bth = collectBthLeafData(ctx, kPcSignature);
    if (!bth) {
        return std::unexpected(bth.error());
    }

    sak::PstAttachmentInfo att;
    att.index = att_index;
    populateAttachmentFromLeaf(att, *bth, ctx);
    return att;
}

void PstParser::populateAttachmentFromLeaf(sak::PstAttachmentInfo& att,
                                           const BthLeafResult& bth,
                                           const HeapContext& ctx) {
    const int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return;
    }

    const int record_count = static_cast<int>(bth.leaf_data.size() / record_size);
    const auto& setters = attachmentSetters();

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        const int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }

        const uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
        const uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
        uint32_t value_ref = readLE<uint32_t>(bth.leaf_data,
                                              rec_offset + bth.key_size + kPropertyValueRefOffset);

        QByteArray raw_val;
        if (isPcVariableType(prop_type)) {
            auto resolved =
                resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
            if (resolved) {
                raw_val = std::move(*resolved);
            }
        } else {
            raw_val.resize(kPropertyValueRefSize);
            std::memcpy(raw_val.data(), &value_ref, kPropertyValueRefSize);
        }

        auto setter_it = setters.find(prop_id);
        if (setter_it != setters.end()) {
            (*setter_it)(att, formatPropertyValue(prop_type, raw_val));
        }
    }
}

std::expected<QVector<sak::PstAttachmentInfo>, error_code> PstParser::readAttachments(
    uint64_t message_nid) {
    auto node_it = m_nbt_cache.find(message_nid);
    if (node_it == m_nbt_cache.end()) {
        return std::unexpected(error_code::pst_node_not_found);
    }

    if (node_it->subnode_bid == 0) {
        return QVector<sak::PstAttachmentInfo>{};
    }

    auto subnodes = readSubNodeBTree(node_it->subnode_bid);
    if (!subnodes) {
        return std::unexpected(subnodes.error());
    }

    QVector<sak::PstAttachmentInfo> attachments;
    int att_index = 0;

    for (auto sub_it = subnodes->begin(); sub_it != subnodes->end(); ++sub_it) {
        const uint8_t sub_type = static_cast<uint8_t>(sub_it.key() & kNidTypeMask);
        if (sub_type != sak::email::kNidTypeAttachment) {
            continue;
        }

        // Fail closed on an unparsable attachment sub-node rather than silently
        // dropping it: a compacted list would report fewer attachments than the
        // message actually carries yet look complete (data loss the caller cannot
        // detect). This matches readMessage, which propagates an attachment-read
        // failure instead of presenting a message as attachment-free.
        auto att = readSingleAttachment(sub_it.value(), att_index);
        if (!att) {
            return std::unexpected(att.error());
        }
        attachments.append(std::move(*att));
        ++att_index;
    }

    return attachments;
}

// ============================================================================
// Utility Functions
// ============================================================================

sak::EmailItemType PstParser::classifyMessageClass(const QString& message_class) {
    if (message_class.isEmpty() || message_class == QLatin1String("<empty>")) {
        return sak::EmailItemType::Email;
    }

    struct Mapping {
        const char* prefix;
        sak::EmailItemType type;
    };
    static constexpr Mapping kMappings[] = {
        {.prefix = "IPM.Note", .type = sak::EmailItemType::Email},
        {.prefix = "IPM.Post", .type = sak::EmailItemType::Email},
        {.prefix = "IPM.Report", .type = sak::EmailItemType::Email},
        {.prefix = "IPM.Contact", .type = sak::EmailItemType::Contact},
        {.prefix = "IPM.Appointment", .type = sak::EmailItemType::Calendar},
        {.prefix = "IPM.Task", .type = sak::EmailItemType::Task},
        {.prefix = "IPM.StickyNote", .type = sak::EmailItemType::StickyNote},
        {.prefix = "IPM.Activity", .type = sak::EmailItemType::JournalEntry},
        {.prefix = "IPM.DistList", .type = sak::EmailItemType::DistList},
        {.prefix = "IPM.Schedule.Meeting", .type = sak::EmailItemType::MeetingRequest},
        {.prefix = "IPM", .type = sak::EmailItemType::Email},
    };

    for (const auto& m : kMappings) {
        if (message_class.startsWith(QLatin1String(m.prefix), Qt::CaseInsensitive)) {
            return m.type;
        }
    }
    return sak::EmailItemType::Unknown;
}

sak::PstNodeType PstParser::nodeType(uint64_t nid) {
    return static_cast<sak::PstNodeType>(nid & kNidTypeMask);
}

QVector<uint64_t> PstParser::allNodeIds() const {
    return m_nbt_cache.keys();
}

std::expected<QByteArray, error_code> PstParser::readBytes(qint64 offset, qint64 count) {
    // A closed file makes seek() fail, and a count of zero or less makes read()
    // return an empty buffer; both are rejected below, so no assert is needed.
    if (!m_file.seek(offset)) {
        return std::unexpected(error_code::seek_error);
    }

    QByteArray data = m_file.read(count);
    if (data.isEmpty()) {
        return std::unexpected(error_code::read_error);
    }

    return data;
}

template <typename T>
T PstParser::readLE(const QByteArray& data, int offset) {
    // Bounds-check at runtime, not only via the release-disabled Q_ASSERT: page/BTH
    // entry sizes are attacker-controlled, so a fixed-offset read can otherwise be
    // driven past the buffer. Return 0 for an out-of-range field rather than memcpy
    // out of bounds (mirrors the free-function localReadLE).
    if (offset < 0 ||
        static_cast<qsizetype>(offset) + static_cast<qsizetype>(sizeof(T)) > data.size()) {
        return T{0};
    }
    T value;
    std::memcpy(&value, data.constData() + offset, sizeof(T));
    return value;
}

// Explicit template instantiations
template uint8_t PstParser::readLE<uint8_t>(const QByteArray&, int);
template uint16_t PstParser::readLE<uint16_t>(const QByteArray&, int);
template int16_t PstParser::readLE<int16_t>(const QByteArray&, int);
template uint32_t PstParser::readLE<uint32_t>(const QByteArray&, int);
template int32_t PstParser::readLE<int32_t>(const QByteArray&, int);
template uint64_t PstParser::readLE<uint64_t>(const QByteArray&, int);
template int64_t PstParser::readLE<int64_t>(const QByteArray&, int);

int PstParser::countTotalItems() const {
    // folder.content_count is read straight from the store, so a crafted tree of large counts
    // would overflow a signed int accumulator (UB) before the total is even returned. Accumulate
    // in int64_t and saturate at INT_MAX after every add so the running sum can never overflow the
    // int64_t either, then narrow the (already in-range) result back to the int the API exposes.
    std::function<int64_t(const sak::PstFolderTree&)> count_recursive;
    count_recursive = [&](const sak::PstFolderTree& tree) -> int64_t {
        int64_t total = 0;
        for (const auto& folder : tree) {
            total += folder.content_count;
            total += count_recursive(folder.children);
            total = std::min<int64_t>(total, std::numeric_limits<int>::max());
        }
        return total;
    };
    return static_cast<int>(count_recursive(m_folder_tree));
}

int PstParser::countFolders(const sak::PstFolderTree& tree) {
    int count = 0;
    for (const auto& folder : tree) {
        count += 1;
        count += countFolders(folder.children);
    }
    return count;
}
