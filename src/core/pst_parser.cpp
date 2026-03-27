// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_parser.cpp
/// @brief PST/OST binary parser implementation (MS-PST specification)

#include "sak/pst_parser.h"

#include "sak/error_codes.h"
#include "sak/logger.h"

#include <QDataStream>
#include <QFileInfo>
#include <qt_windows.h>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>

using sak::error_code;

// ============================================================================
// MS-PST §5.1 — Compressible Encryption Decrypt Table
// ============================================================================

// clang-format off
static constexpr std::array<uint8_t, 256> kDecryptTable = {
    0x47, 0xF1, 0xB4, 0xE6, 0x0B, 0x6A, 0x72, 0x48,
    0x85, 0x4E, 0x9E, 0xEB, 0xE2, 0xF8, 0x94, 0x53,
    0xE0, 0xBB, 0xA0, 0x02, 0xE8, 0x5A, 0x09, 0xAB,
    0xDB, 0xE3, 0xBA, 0xC6, 0x7C, 0xC4, 0x1D, 0x7E,
    0x30, 0x2B, 0x01, 0x44, 0x7F, 0x4A, 0xCC, 0x68,
    0x59, 0x14, 0xF0, 0x04, 0xC3, 0xD2, 0xE1, 0x73,
    0xF7, 0xB1, 0x4B, 0x23, 0x7B, 0x43, 0x78, 0x12,
    0x2D, 0x5C, 0xBE, 0x3D, 0x74, 0x16, 0x09, 0x4C,
    0xC0, 0x28, 0xFE, 0x18, 0xF6, 0xAE, 0xD7, 0x46,
    0x73, 0x06, 0x92, 0xAC, 0x69, 0x81, 0x3B, 0xC1,
    0x2F, 0x43, 0x67, 0xF5, 0xD0, 0xBE, 0x3F, 0xDE,
    0xBC, 0xA8, 0x65, 0x9A, 0xD5, 0x54, 0xEF, 0xA1,
    0x5B, 0x41, 0xA5, 0x29, 0xCE, 0x04, 0xE7, 0x63,
    0x56, 0xBF, 0x87, 0x00, 0x0A, 0x0C, 0xAF, 0x0E,
    0x8E, 0x3E, 0x22, 0x11, 0xB0, 0xFB, 0x95, 0x90,
    0x60, 0x83, 0x17, 0xF9, 0x88, 0xB3, 0x33, 0x76,
    0x38, 0x08, 0xEE, 0xFA, 0x5F, 0x9C, 0xA7, 0xCB,
    0x62, 0x1A, 0x25, 0xDA, 0x4D, 0xD8, 0xEC, 0x13,
    0x34, 0xFD, 0xFC, 0xC8, 0x57, 0xF4, 0x98, 0x3C,
    0xAA, 0x10, 0x26, 0x32, 0x8F, 0x21, 0x96, 0x82,
    0x5E, 0x64, 0xD1, 0x84, 0xCD, 0x20, 0x6E, 0x93,
    0x36, 0xA3, 0x8A, 0xDD, 0x49, 0x0D, 0x6D, 0x97,
    0x2E, 0x39, 0x61, 0x8B, 0x58, 0xC5, 0xA9, 0x7D,
    0xCA, 0xC2, 0xB5, 0x42, 0x6C, 0x86, 0xED, 0xB9,
    0x37, 0x50, 0x7A, 0x19, 0xB7, 0x70, 0x91, 0x89,
    0xD9, 0x8D, 0xD3, 0x1E, 0x66, 0x1C, 0xA2, 0x52,
    0x0F, 0x4F, 0xDF, 0xD4, 0x6F, 0x40, 0xBD, 0x8C,
    0x5D, 0x15, 0xC7, 0x27, 0x71, 0x07, 0x3A, 0xA4,
    0xB8, 0x1B, 0x24, 0xE4, 0xE9, 0x99, 0x9F, 0x55,
    0x9B, 0x2A, 0x77, 0xF3, 0xCF, 0xB6, 0xD6, 0x6B,
    0xC9, 0x1F, 0xB2, 0x9D, 0x80, 0x31, 0xA6, 0x45,
    0x75, 0x79, 0x51, 0x03, 0x2C, 0x35, 0xAD, 0xE5,
};
// clang-format on

// ============================================================================
// Constants
// ============================================================================

/// Maximum BTree depth to prevent infinite recursion
static constexpr int kMaxBTreeDepth = 20;

/// ANSI PST BTree page size (512 bytes)
static constexpr int kAnsiPageSizeLocal = sak::email::kAnsiPageSize;

/// Unicode PST BTree page size (4096 bytes for Unicode4K, 512 for older)
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

/// Heap-on-Node signature byte
static constexpr uint8_t kHnSignature = 0xEC;

/// BTree-on-Heap signature byte
static constexpr uint8_t kBthSignature = 0xB5;

/// Table Context signature byte
static constexpr uint8_t kTcSignature = 0x7C;

/// Property Context signature byte
static constexpr uint8_t kPcSignature = 0xBC;

// MS-OXCMSG: PR_SUBJECT may begin with a prefix marker (U+0001) followed by
// a single character encoding the prefix length.  Strip these control bytes.
static QString stripSubjectPrefix(const QString& subject) {
    if (subject.size() >= 2 && subject.at(0) == QChar(0x0001)) {
        return subject.mid(2);
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
    return version == sak::email::kAnsiVersion || version == sak::email::kUnicodeVersion ||
           version == sak::email::kUnicode4kVersion;
}

// ============================================================================
// Property Value Formatting Helpers
// ============================================================================

template <typename T>
static T localReadLE(const QByteArray& data, int offset) {
    T value;
    std::memcpy(&value, data.constData() + offset, sizeof(T));
    return value;
}

static QString formatInt16Value(const QByteArray& raw) {
    if (raw.size() < 2) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int16_t>(raw, 0));
}

static QString formatInt32Value(const QByteArray& raw) {
    if (raw.size() < 4) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int32_t>(raw, 0));
}

static QString formatInt64Value(const QByteArray& raw) {
    if (raw.size() < 8) {
        return QStringLiteral("<invalid>");
    }
    return QString::number(localReadLE<int64_t>(raw, 0));
}

static QString formatBooleanValue(const QByteArray& raw) {
    if (raw.size() < 1) {
        return QStringLiteral("<invalid>");
    }
    return raw[0] ? QStringLiteral("true") : QStringLiteral("false");
}

static QString formatFloat64Value(const QByteArray& raw) {
    if (raw.size() < 8) {
        return QStringLiteral("<invalid>");
    }
    double val;
    std::memcpy(&val, raw.constData(), 8);
    return QString::number(val, 'g', 6);
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
    int needed =
        MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, raw.constData(), raw.size(), nullptr, 0);
    if (needed > 0) {
        QString result;
        result.resize(needed);
        MultiByteToWideChar(CP_ACP,
                            MB_PRECOMPOSED,
                            raw.constData(),
                            raw.size(),
                            reinterpret_cast<wchar_t*>(result.data()),
                            needed);
        return result;
    }

    return QString::fromLatin1(raw);
}

static QString formatUnicodeValue(const QByteArray& raw) {
    if (raw.size() < 2) {
        return QString();
    }
    auto result = QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()),
                                     raw.size() / 2);
    // Strip null terminators that PST files sometimes include
    while (!result.isEmpty() && result.back().isNull()) {
        result.chop(1);
    }
    return result;
}

static QString formatSysTimeValue(const QByteArray& raw) {
    if (raw.size() < 8) {
        return QStringLiteral("<invalid>");
    }
    int64_t ft = localReadLE<int64_t>(raw, 0);
    constexpr int64_t kFileTimeEpochDiff = 116'444'736'000'000'000LL;
    int64_t unix_ms = (ft - kFileTimeEpochDiff) / 10'000;
    return QDateTime::fromMSecsSinceEpoch(unix_ms, Qt::UTC).toString(Qt::ISODate);
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
static const QHash<uint16_t, DetailSetter>& detailSetters() {
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
        {sak::email::kPropIdMessageClass,
         [](sak::PstItemDetail& d, const sak::MapiProperty& p) {
             d.item_type = PstParser::classifyMessageClass(p.display_value);
         }},
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

static const QHash<uint16_t, SummarySetter>& summarySetters() {
    static const QHash<uint16_t, SummarySetter> kMap = {
        {0x67F2,  // PidTagLtpRowId — NID
         [](sak::PstItemSummary& item, const sak::MapiProperty& col) {
             if (col.raw_value.size() >= 4)
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

/// Sender property ID → slot mapping: 0=name, 1=email
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
            if (col.tag_id == 0x67F2 && col.raw_value.size() >= 4) {
                uint64_t nid = localReadLE<uint32_t>(col.raw_value, 0);
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
    int offset = block_offsets[hid_block_index - 1];
    if (offset + 2 > heap_size) {
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
    if (tc_raw.size() < 22) {
        return std::unexpected(error_code::pst_table_context_error);
    }
    if (static_cast<uint8_t>(tc_raw[0]) != 0x7C) {
        return std::unexpected(error_code::pst_table_context_error);
    }

    PstParser::TcInfo info;
    uint8_t col_count = static_cast<uint8_t>(tc_raw[1]);
    info.rgib_tci_1b = localReadLE<uint16_t>(tc_raw, 6);
    info.rgib_tci_bm = localReadLE<uint16_t>(tc_raw, 8);
    info.hnid_rows = localReadLE<uint32_t>(tc_raw, 14);

    constexpr int kTcinfoHeaderSize = 22;
    info.columns.reserve(col_count);
    for (int col_idx = 0; col_idx < col_count; ++col_idx) {
        int base = kTcinfoHeaderSize + (col_idx * 8);
        if (base + 8 > tc_raw.size()) {
            break;
        }
        PstParser::TcColDesc col;
        col.prop_type = localReadLE<uint16_t>(tc_raw, base);
        col.prop_id = localReadLE<uint16_t>(tc_raw, base + 2);
        col.ib_data = localReadLE<uint16_t>(tc_raw, base + 4);
        col.cb_data = static_cast<uint8_t>(tc_raw[base + 6]);
        col.i_bit = static_cast<uint8_t>(tc_raw[base + 7]);
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
        node.node_id = localReadLE<uint64_t>(data, off);
        node.data_bid = localReadLE<uint64_t>(data, off + 8);
        node.subnode_bid = localReadLE<uint64_t>(data, off + 16);
        node.parent_node_id = localReadLE<uint32_t>(data, off + 24);
    } else {
        node.node_id = localReadLE<uint32_t>(data, off);
        node.data_bid = localReadLE<uint32_t>(data, off + 4);
        node.subnode_bid = localReadLE<uint32_t>(data, off + 8);
        node.parent_node_id = localReadLE<uint32_t>(data, off + 12);
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
        entry.file_offset = localReadLE<uint64_t>(data, off + 8);
        entry.cb = localReadLE<uint16_t>(data, off + 16);
    } else {
        entry.bid = localReadLE<uint32_t>(data, off);
        entry.file_offset = localReadLE<uint32_t>(data, off + 4);
        entry.cb = localReadLE<uint16_t>(data, off + 8);
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

void PstParser::open(const QString& file_path) {
    close();
    m_cancelled.store(false, std::memory_order_relaxed);

    m_file.setFileName(file_path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        const auto message = QStringLiteral("Cannot open file: %1").arg(file_path);
        sak::logError("PstParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        return;
    }

    Q_EMIT progressUpdated(0, QStringLiteral("Parsing file header..."));

    auto header_result = parseHeader();
    if (!header_result) {
        const auto message = QStringLiteral("Invalid PST header: %1")
                                 .arg(QString::fromUtf8(sak::to_string(header_result.error())));
        sak::logError("PstParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        close();
        return;
    }

    Q_EMIT progressUpdated(10, QStringLiteral("Loading Node BTree..."));

    auto nbt_result = loadNodeBTree(m_header.root_nbt_page);
    if (!nbt_result) {
        const auto message = QStringLiteral("Failed to load Node BTree: %1")
                                 .arg(QString::fromUtf8(sak::to_string(nbt_result.error())));
        sak::logError("PstParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        close();
        return;
    }

    Q_EMIT progressUpdated(30, QStringLiteral("Loading Block BTree..."));

    sak::logInfo("PstParser: NBT cache loaded — {} nodes", m_nbt_cache.size());

    auto bbt_result = loadBlockBTree(m_header.root_bbt_page);
    if (!bbt_result) {
        const auto message = QStringLiteral("Failed to load Block BTree: %1")
                                 .arg(QString::fromUtf8(sak::to_string(bbt_result.error())));
        sak::logError("PstParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        close();
        return;
    }

    Q_EMIT progressUpdated(60, QStringLiteral("Building folder hierarchy..."));

    sak::logInfo("PstParser: BBT cache loaded — {} blocks", m_bbt_cache.size());

    auto tree_result = buildFolderHierarchy(sak::email::kNidRootFolder);
    if (!tree_result) {
        const auto message = QStringLiteral("Failed to build folder hierarchy: %1")
                                 .arg(QString::fromUtf8(sak::to_string(tree_result.error())));
        sak::logError("PstParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        close();
        return;
    }

    m_folder_tree = std::move(*tree_result);

    // Fill file info
    QFileInfo fi(file_path);
    m_file_info.file_path = file_path;
    m_file_info.file_size_bytes = fi.size();
    m_file_info.is_unicode = m_is_unicode;
    m_file_info.is_ost = (m_header.content_type == sak::email::kOstContentType);
    m_file_info.encryption_type = m_encryption_type;
    m_file_info.total_folders = countFolders(m_folder_tree);
    m_file_info.total_items = countTotalItems();
    m_file_info.last_modified = fi.lastModified();

    // Try to get the message store display name
    auto store_it = m_nbt_cache.find(sak::email::kNidMessageStore);
    if (store_it != m_nbt_cache.end()) {
        auto props = readPropertyContext(sak::email::kNidMessageStore);
        if (props) {
            for (const auto& prop : *props) {
                if (prop.tag_id == sak::email::kPropIdDisplayName) {
                    m_file_info.display_name = prop.display_value;
                    break;
                }
            }
        }
    }

    m_is_open = true;
    Q_EMIT progressUpdated(100, QStringLiteral("File loaded successfully"));
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
    uint64_t contents_nid = (folder_node_id & ~static_cast<uint64_t>(0x1F)) |
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

std::expected<QByteArray, error_code> PstParser::readAttachmentData(uint64_t message_node_id,
                                                                    int attachment_index) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }

    auto msg_it = m_nbt_cache.find(message_node_id);
    if (msg_it == m_nbt_cache.end()) {
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
        uint8_t sub_type = static_cast<uint8_t>(sub_it.key() & 0x1F);
        if (sub_type != sak::email::kNidTypeAttachment) {
            continue;
        }
        if (found_count != attachment_index) {
            ++found_count;
            continue;
        }

        auto att_props = readPropertyContext(sub_it.key());
        if (!att_props) {
            return std::unexpected(att_props.error());
        }
        auto* data = findPropertyById(*att_props, sak::email::kPropIdAttachData);
        if (data) {
            return *data;
        }
        return std::unexpected(error_code::pst_attachment_error);
    }

    return std::unexpected(error_code::pst_attachment_error);
}

// ============================================================================
// NDB Layer — Header Parsing
// ============================================================================

std::expected<void, error_code> PstParser::parseHeader() {
    // Read the first 580 bytes (Unicode header is larger than ANSI)
    constexpr int kHeaderReadSize = 580;
    auto header_bytes = readBytes(0, kHeaderReadSize);
    if (!header_bytes) {
        return std::unexpected(header_bytes.error());
    }

    const auto& data = *header_bytes;
    if (data.size() < kHeaderReadSize) {
        return std::unexpected(error_code::pst_invalid_header);
    }

    // §2.2.2.6 — dwMagic at offset 0
    m_header.magic = readLE<uint32_t>(data, 0);
    if (m_header.magic != sak::email::kPstMagic) {
        sak::logError("PstParser: Invalid magic: 0x{:08X}", m_header.magic);
        return std::unexpected(error_code::pst_invalid_header);
    }

    // CRC at offset 4
    m_header.crc = readLE<uint32_t>(data, 4);

    // wMagicClient at offset 8 — content type (SM or SO)
    m_header.content_type = readLE<uint16_t>(data, 8);
    if (m_header.content_type != sak::email::kPstContentType &&
        m_header.content_type != sak::email::kOstContentType) {
        sak::logError("PstParser: Invalid content type: 0x{:04X}", m_header.content_type);
        return std::unexpected(error_code::pst_invalid_header);
    }

    // wVer at offset 10 — version
    m_header.data_version = readLE<uint16_t>(data, 10);
    m_is_unicode = (m_header.data_version >= sak::email::kUnicodeVersion);
    m_is_4k = (m_header.data_version == sak::email::kUnicode4kVersion);

    if (!isKnownDataVersion(m_header.data_version)) {
        sak::logWarning("PstParser: Unusual version: {}", m_header.data_version);
    }

    // wVerClient at offset 12
    m_header.client_version = readLE<uint16_t>(data, 12);

    // bPlatformCreate at offset 14, bPlatformAccess at offset 15
    m_header.platform_create = static_cast<uint8_t>(data[14]);
    m_header.platform_access = static_cast<uint8_t>(data[15]);

    // bCryptMethod — encryption type
    // ANSI: offset 513, Unicode: offset 513
    int crypt_offset = m_is_unicode ? 513 : 513;
    m_header.encryption_type = static_cast<uint8_t>(data[crypt_offset]);
    m_encryption_type = m_header.encryption_type;

    if (m_encryption_type == sak::email::kEncryptHigh) {
        sak::logError("PstParser: High encryption not supported");
        return std::unexpected(error_code::pst_unsupported_encryption);
    }

    // Root structure pointers differ between ANSI and Unicode.
    // MS-PST §2.2.2.6: header fields before ROOT include rgnid[32]
    // (128 bytes) plus bid/unique fields whose sizes depend on format.
    //   Unicode: ROOT at offset 0xB4 (180)
    //   ANSI:    ROOT at offset 0xA4 (164)
    // ROOT layout (§2.2.2.7.5):
    //   +0  dwReserved (4)
    //   +4  ibFileEof  (8 Unicode / 4 ANSI)
    //   +36 BREFNBT    (16 Unicode / 8 ANSI)
    //   +52 BREFBBT    (16 Unicode / 8 ANSI)
    if (m_is_unicode) {
        constexpr int root_offset = 0xB4;

        m_header.file_size = readLE<uint64_t>(data, root_offset + 4);

        // BREFNBT.bid at ROOT+36, BREFNBT.ib at ROOT+44
        m_header.root_nbt_page = readLE<uint64_t>(data, root_offset + 44);

        // BREFBBT.bid at ROOT+52, BREFBBT.ib at ROOT+60
        m_header.root_bbt_page = readLE<uint64_t>(data, root_offset + 60);
    } else {
        constexpr int root_offset = 0xA4;

        m_header.file_size = readLE<uint32_t>(data, root_offset + 4);

        // BREFNBT: bid at ROOT+20, ib at ROOT+24
        m_header.root_nbt_page = readLE<uint32_t>(data, root_offset + 24);

        // BREFBBT: bid at ROOT+28, ib at ROOT+32
        m_header.root_bbt_page = readLE<uint32_t>(data, root_offset + 32);
    }

    sak::logInfo(
        "PstParser: Header parsed — version={}, unicode={}, "
        "encryption={}, NBT=0x{:X}, BBT=0x{:X}",
        m_header.data_version,
        m_is_unicode,
        m_encryption_type,
        m_header.root_nbt_page,
        m_header.root_bbt_page);

    return {};
}

// ============================================================================
// NDB Layer — BTree Page Parsing
// ============================================================================

PstParser::PageFormatSizes PstParser::pageFormatSizes() const {
    PageFormatSizes fmt;
    fmt.page_size = m_is_unicode ? kUnicodePageSizeLocal : kAnsiPageSizeLocal;
    fmt.trailer_size = m_is_4k        ? kUnicode4kPageTrailerSize
                       : m_is_unicode ? kUnicodePageTrailerSize
                                      : kAnsiPageTrailerSize;
    fmt.meta_size = m_is_4k ? kUnicode4kMetadataSize : 4;
    fmt.meta_pad = m_is_4k ? kUnicode4kPaddingSize : (m_is_unicode ? 4 : 0);
    return fmt;
}

std::expected<PstParser::BTreePageInfo, error_code> PstParser::parseBTreePage(uint64_t page_offset,
                                                                              int depth) {
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

    int trailer_offset = fmt.page_size - fmt.trailer_size;
    uint8_t ptype = static_cast<uint8_t>((*page_data)[trailer_offset]);
    if (ptype != static_cast<uint8_t>((*page_data)[trailer_offset + 1])) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    int meta_offset = trailer_offset - fmt.meta_pad - fmt.meta_size;
    if (meta_offset < 0) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    BTreePageInfo info;
    info.data = std::move(*page_data);
    info.meta_offset = meta_offset;
    if (m_is_4k) {
        info.entry_count = readLE<uint16_t>(info.data, meta_offset);
        info.entry_size = static_cast<uint8_t>(info.data[meta_offset + 4]);
        info.level = static_cast<uint8_t>(info.data[meta_offset + 5]);
    } else {
        info.entry_count = static_cast<uint8_t>(info.data[meta_offset]);
        info.entry_size = static_cast<uint8_t>(info.data[meta_offset + 2]);
        info.level = static_cast<uint8_t>(info.data[meta_offset + 3]);
    }
    return info;
}

// ============================================================================
// NDB Layer — BTree Loading
// ============================================================================

std::expected<void, error_code> PstParser::loadNodeBTree(uint64_t page_offset, int depth) {
    auto page = parseBTreePage(page_offset, depth);
    if (!page) {
        return std::unexpected(page.error());
    }

    const auto& data = page->data;
    int entry_size = page->entry_size;
    int meta_offset = page->meta_offset;

    if (page->level == 0) {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            auto node = readNodeLeafEntry(data, off, m_is_unicode);
            if (m_nbt_cache.size() < sak::email::kNodeBTreeCacheSize) {
                m_nbt_cache.insert(node.node_id, node);
            }
        }
    } else {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            uint64_t child_page = m_is_unicode ? readLE<uint64_t>(data, off + 16)
                                               : readLE<uint32_t>(data, off + 8);
            auto child_result = loadNodeBTree(child_page, depth + 1);
            if (!child_result) {
                return child_result;
            }
        }
    }
    return {};
}

std::expected<void, error_code> PstParser::loadBlockBTree(uint64_t page_offset, int depth) {
    auto page = parseBTreePage(page_offset, depth);
    if (!page) {
        return std::unexpected(page.error());
    }

    const auto& data = page->data;
    int entry_size = page->entry_size;
    int meta_offset = page->meta_offset;

    if (page->level == 0) {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            auto entry = readBlockLeafEntry(data, off, m_is_unicode);
            if (m_bbt_cache.size() < sak::email::kBlockBTreeCacheSize) {
                m_bbt_cache.insert(entry.bid, BbtEntry{entry.file_offset, entry.cb});
            }
        }
    } else {
        for (int idx = 0; idx < page->entry_count; ++idx) {
            int off = idx * entry_size;
            if (off + entry_size > meta_offset) {
                break;
            }

            uint64_t child_page = m_is_unicode ? readLE<uint64_t>(data, off + 16)
                                               : readLE<uint32_t>(data, off + 8);
            auto child_result = loadBlockBTree(child_page, depth + 1);
            if (!child_result) {
                return child_result;
            }
        }
    }
    return {};
}

// ============================================================================
// NDB Layer — Block Reading
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readBlock(uint64_t bid) {
    // §2.2.2.2 — The BBT stores BIDs with the fInternal bit (bit 1)
    // intact, so look up the BID as-is.
    auto it = m_bbt_cache.find(bid);
    if (it == m_bbt_cache.end()) {
        return std::unexpected(error_code::pst_block_not_found);
    }

    const auto& entry = it.value();
    uint64_t file_offset = entry.file_offset;
    int cb = static_cast<int>(entry.cb);

    if (cb == 0) {
        return QByteArray{};
    }

    // Read exactly cb bytes of block data (§2.2.2.8 — data precedes trailer)
    auto block_data = readBytes(static_cast<qint64>(file_offset), cb);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }

    auto& raw = *block_data;

    // Unicode4K files (wVer >= 36) may have zlib-compressed blocks.
    // The block footer contains an uncompressed_data_size field;
    // when it differs from cb the block must be deflate-decompressed.
    // Reference: libpff pff_block_footer_64bit_4k_page_t
    if (m_is_4k && cb > 0) {
        raw = decompressBlockIf4k(raw, file_offset, cb);
    }

    if (m_encryption_type == sak::email::kEncryptCompressible) {
        bool is_internal = (bid & 0x02) != 0;
        if (!is_internal) {
            auto span = std::span<uint8_t>(reinterpret_cast<uint8_t*>(raw.data()),
                                           static_cast<size_t>(raw.size()));
            decryptBlock(span);
        }
    }

    return raw;
}

std::expected<QByteArray, error_code> PstParser::readDataTree(uint64_t bid,
                                                              QVector<int>* block_offsets) {
    bool is_internal = (bid & 0x02) != 0;
    if (!is_internal) {
        auto result = readBlock(bid);
        if (result && block_offsets) {
            block_offsets->append(result->size());
        }
        return result;
    }

    auto block_data = readBlock(bid);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }

    const auto& data = *block_data;
    if (data.size() < 8) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    uint8_t block_level = static_cast<uint8_t>(data[1]);
    uint16_t entry_count = readLE<uint16_t>(data, 2);
    QByteArray result;

    if (block_level == 1) {
        auto status = readXblockChildren(data, entry_count, result, block_offsets);
        if (!status) {
            return std::unexpected(status.error());
        }
    } else if (block_level == 2) {
        auto status = readXxblockChildren(data, entry_count, result, block_offsets);
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
                                                              QVector<int>* block_offsets) {
    int bid_size = m_is_unicode ? 8 : 4;
    for (int idx = 0; idx < entry_count; ++idx) {
        int offset = 8 + (idx * bid_size);
        uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset)
                                          : readLE<uint32_t>(data, offset);
        auto child_data = readBlock(child_bid);
        if (!child_data) {
            return std::unexpected(child_data.error());
        }
        result.append(*child_data);
        if (block_offsets) {
            block_offsets->append(result.size());
        }
    }
    return {};
}

std::expected<void, error_code> PstParser::readXxblockChildren(const QByteArray& data,
                                                               uint16_t entry_count,
                                                               QByteArray& result,
                                                               QVector<int>* block_offsets) {
    int bid_size = m_is_unicode ? 8 : 4;
    for (int idx = 0; idx < entry_count; ++idx) {
        int offset = 8 + (idx * bid_size);
        uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset)
                                          : readLE<uint32_t>(data, offset);
        int base_offset = result.size();
        QVector<int> child_offsets;
        auto child_data = readDataTree(child_bid, &child_offsets);
        if (!child_data) {
            return std::unexpected(child_data.error());
        }
        result.append(*child_data);
        if (block_offsets) {
            for (int child_off : child_offsets) {
                block_offsets->append(base_offset + child_off);
            }
        }
    }
    return {};
}

// ============================================================================
// NDB Layer — Block Decompression (Unicode4K / wVer ≥ 36)
// ============================================================================

QByteArray PstParser::decompressBlockIf4k(const QByteArray& raw, uint64_t file_offset, int cb) {
    using namespace sak::email;

    // Calculate aligned block size (512-byte increments for 4K pages)
    int aligned = ((cb + kBlock4kAlignment - 1) / kBlock4kAlignment) * kBlock4kAlignment;
    if ((aligned - cb) < kBlock4kFooterSize) {
        aligned += kBlock4kAlignment;
    }

    // Read the 24-byte block footer from the end of the aligned allocation
    qint64 footer_pos = static_cast<qint64>(file_offset) + aligned - kBlock4kFooterSize;
    auto footer_result = readBytes(footer_pos, kBlock4kFooterSize);
    if (!footer_result || footer_result->size() != kBlock4kFooterSize) {
        return raw;
    }

    uint16_t uncompressed_size = readLE<uint16_t>(*footer_result, kBlock4kUncompSizeOffset);

    if (uncompressed_size == 0 || uncompressed_size == static_cast<uint16_t>(cb)) {
        return raw;
    }

    // Block is zlib-compressed — decompress using qUncompress.
    // qUncompress expects a 4-byte big-endian size prefix before the
    // raw zlib stream.
    QByteArray prefixed;
    prefixed.reserve(4 + raw.size());
    uint32_t be_size = qToBigEndian(static_cast<uint32_t>(uncompressed_size));
    prefixed.append(reinterpret_cast<const char*>(&be_size), 4);
    prefixed.append(raw);

    QByteArray decompressed = qUncompress(prefixed);
    if (decompressed.isEmpty()) {
        sak::logWarning("Block decompression failed at offset {}", std::to_string(file_offset));
        return raw;
    }

    return decompressed;
}

void PstParser::decryptBlock(std::span<uint8_t> data) const {
    for (auto& byte : data) {
        byte = kDecryptTable[byte];
    }
}

// ============================================================================
// LTP Layer — BTH Multi-Level Traversal
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readBthLeafData(
    const QByteArray& heap_data,
    uint32_t node_hid,
    uint8_t key_size,
    int level,
    const QVector<int>& block_offsets) {
    auto node_data = readHeapOnNode(heap_data, node_hid, block_offsets);
    if (!node_data) {
        return std::unexpected(node_data.error());
    }

    if (level == 0) {
        return *node_data;
    }

    int entry_size = key_size + 4;
    if (entry_size == 0) {
        return QByteArray{};
    }

    int entry_count = node_data->size() / entry_size;
    QByteArray combined;

    for (int idx = 0; idx < entry_count; ++idx) {
        int offset = idx * entry_size;
        if (offset + entry_size > node_data->size()) {
            break;
        }
        uint32_t child_hid = readLE<uint32_t>(*node_data, offset + key_size);
        if (child_hid == 0) {
            continue;
        }
        auto child_result =
            readBthLeafData(heap_data, child_hid, key_size, level - 1, block_offsets);
        if (child_result) {
            combined.append(*child_result);
        }
    }

    return combined;
}

// ============================================================================
// LTP Layer — BTH Collection & Property Parsing
// ============================================================================

std::expected<PstParser::BthLeafResult, error_code> PstParser::collectBthLeafData(
    const HeapContext& ctx, uint8_t expected_client_sig) {
    if (ctx.heap_data.size() < 12) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(ctx.heap_data[2]) != kHnSignature) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(ctx.heap_data[3]) != expected_client_sig) {
        return std::unexpected(error_code::pst_property_context_error);
    }

    uint32_t hid_root = readLE<uint32_t>(ctx.heap_data, 4);
    auto bth_header = readHeapOnNode(ctx.heap_data, hid_root, ctx.block_offsets);
    if (!bth_header) {
        return std::unexpected(bth_header.error());
    }
    if (bth_header->size() < 8) {
        return std::unexpected(error_code::pst_property_context_error);
    }
    if (static_cast<uint8_t>((*bth_header)[0]) != kBthSignature) {
        return std::unexpected(error_code::pst_property_context_error);
    }

    uint8_t key_size = static_cast<uint8_t>((*bth_header)[1]);
    uint8_t data_size = static_cast<uint8_t>((*bth_header)[2]);
    uint8_t idx_levels = static_cast<uint8_t>((*bth_header)[3]);
    uint32_t bth_root_hid = readLE<uint32_t>(*bth_header, 4);
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
    return BthLeafResult{std::move(*leaf_result), key_size, data_size};
}

QVector<sak::MapiProperty> PstParser::parsePropertyRecords(const BthLeafResult& bth,
                                                           const HeapContext& ctx) {
    int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return {};
    }

    int record_count = bth.leaf_data.size() / record_size;
    QVector<sak::MapiProperty> properties;
    properties.reserve(record_count);

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        int rec_off = rec_idx * record_size;
        if (rec_off + record_size > bth.leaf_data.size()) {
            break;
        }

        sak::MapiProperty prop;
        prop.tag_id = readLE<uint16_t>(bth.leaf_data, rec_off);

        if (bth.data_size >= 6) {
            prop.tag_type = readLE<uint16_t>(bth.leaf_data, rec_off + bth.key_size);
            uint32_t value_ref = readLE<uint32_t>(bth.leaf_data, rec_off + bth.key_size + 2);

            if (isPcVariableType(prop.tag_type)) {
                auto resolved =
                    resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
                if (resolved) {
                    prop.raw_value = std::move(*resolved);
                }
            } else {
                prop.raw_value.resize(4);
                std::memcpy(prop.raw_value.data(), &value_ref, 4);
            }
        }

        prop.property_name = propertyIdToName(prop.tag_id);
        prop.display_value = formatPropertyValue(prop.tag_type, prop.raw_value);
        properties.append(std::move(prop));
    }
    return properties;
}

// ============================================================================
// LTP Layer — Property Context
// ============================================================================

std::expected<QVector<sak::MapiProperty>, error_code> PstParser::readPropertyContext(uint64_t nid) {
    auto node_it = m_nbt_cache.find(nid);
    if (node_it == m_nbt_cache.end()) {
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
        auto sn_result = readSubNodeBTree(node.subnode_bid);
        if (sn_result) {
            ctx.subnode_map = std::move(*sn_result);
        }
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
// LTP Layer — Table Context
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

    uint32_t hid_root = readLE<uint32_t>(ctx.heap_data, 4);
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

    QByteArray row_data = loadTcRowData(*tc, node, ctx);
    if (row_data.isEmpty()) {
        return QVector<QVector<sak::MapiProperty>>{};
    }

    return buildTcRows(row_data, *tc, ctx);
}

std::expected<void, sak::error_code> PstParser::validateHeapForTc(const QByteArray& heap_data) {
    if (heap_data.size() < 12) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(heap_data[2]) != kHnSignature) {
        return std::unexpected(error_code::pst_invalid_heap);
    }
    if (static_cast<uint8_t>(heap_data[3]) != kTcSignature) {
        return std::unexpected(error_code::pst_table_context_error);
    }
    return {};
}

QByteArray PstParser::loadTcRowData(const TcInfo& tc, const sak::PstNode& node, HeapContext& ctx) {
    QByteArray row_data;
    if ((tc.hnid_rows & 0x1F) == 0) {
        auto heap_r = readHeapOnNode(ctx.heap_data, tc.hnid_rows, ctx.block_offsets);
        if (heap_r) {
            row_data = std::move(*heap_r);
        }
    } else if (node.subnode_bid != 0) {
        auto subs = readSubNodeBTree(node.subnode_bid);
        if (subs) {
            auto sub = subs->find(tc.hnid_rows);
            if (sub != subs->end()) {
                auto sub_data = readDataTree(sub->data_bid);
                if (sub_data) {
                    row_data = std::move(*sub_data);
                }
            }
        }
    }

    // Load subnodes for HNID resolution in cells
    if (node.subnode_bid != 0) {
        auto sn_result = readSubNodeBTree(node.subnode_bid);
        if (sn_result) {
            ctx.subnode_map = std::move(*sn_result);
        }
    }

    return row_data;
}

QVector<QVector<sak::MapiProperty>> PstParser::buildTcRows(const QByteArray& row_data,
                                                           const TcInfo& tc,
                                                           const HeapContext& ctx) {
    int row_size = tc.rgib_tci_bm;
    if (row_size == 0) {
        return {};
    }
    int row_count = row_data.size() / row_size;
    QVector<QVector<sak::MapiProperty>> rows;
    rows.reserve(row_count);

    for (int row_idx = 0; row_idx < row_count; ++row_idx) {
        int row_off = row_idx * row_size;
        if (row_off + row_size > row_data.size()) {
            break;
        }

        QVector<sak::MapiProperty> row_props;
        row_props.reserve(tc.columns.size());
        TcRowView row_view{row_off, row_size, row_off + tc.rgib_tci_1b};

        for (const auto& col : tc.columns) {
            auto prop = buildTcCell(row_data, row_view, col, ctx);
            row_props.append(std::move(prop));
        }
        rows.append(std::move(row_props));
    }
    return rows;
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
    int ceb_byte = row_view.ceb_off + (col.i_bit / 8);
    int ceb_bit = 7 - (col.i_bit % 8);
    if (ceb_byte < row_view.row_off + row_view.row_size) {
        cell_exists = (static_cast<uint8_t>(row_data[ceb_byte]) >> ceb_bit) & 1;
    }

    int cell_off = row_view.row_off + col.ib_data;
    if (cell_exists && cell_off + col.cb_data <= row_data.size()) {
        prop.raw_value = row_data.mid(cell_off, col.cb_data);
        if (isHnidResolvableType(col.prop_type) && col.cb_data == 4) {
            uint32_t hnid = readLE<uint32_t>(row_data, cell_off);
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
// LTP Layer — Heap-on-Node
// ============================================================================

std::expected<QByteArray, error_code> PstParser::readHeapOnNode(const QByteArray& heap_data,
                                                                uint32_t hn_id,
                                                                const QVector<int>& block_offsets) {
    if (hn_id == 0 || heap_data.isEmpty()) {
        return QByteArray{};
    }

    uint16_t hid_index = static_cast<uint16_t>((hn_id >> 5) & 0x7FF);
    uint16_t hid_block_index = static_cast<uint16_t>((hn_id >> 16) & 0xFFFF);

    auto block_off = resolveHnBlockOffset(hid_block_index, block_offsets, heap_data.size());
    if (!block_off) {
        return std::unexpected(block_off.error());
    }
    int block_offset = *block_off;

    uint16_t ib_hnpm = readLE<uint16_t>(heap_data, block_offset);
    int page_map_offset = block_offset + ib_hnpm;
    if (page_map_offset + 4 > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    uint16_t alloc_count = readLE<uint16_t>(heap_data, page_map_offset);
    if (hid_index == 0 || hid_index > alloc_count) {
        return QByteArray{};
    }

    int alloc_table_offset = page_map_offset + 4;
    int entry_offset = alloc_table_offset + ((hid_index - 1) * 2);
    int next_offset = alloc_table_offset + (hid_index * 2);

    if (next_offset + 2 > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    uint16_t start = readLE<uint16_t>(heap_data, entry_offset);
    uint16_t end = readLE<uint16_t>(heap_data, next_offset);

    int abs_start = block_offset + start;
    int abs_end = block_offset + end;

    if (abs_start > abs_end || abs_end > heap_data.size()) {
        return std::unexpected(error_code::pst_invalid_heap);
    }

    return heap_data.mid(abs_start, abs_end - abs_start);
}

// ============================================================================
// LTP Layer — Unified HNID Resolution
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

    // 2. Non-zero low 5 bits → NID not in sub-node map
    if ((hnid & 0x1F) != 0) {
        return QByteArray{};
    }

    // 3. HID — delegate to heap reader (validates block range)
    return readHeapOnNode(heap_data, hnid, block_offsets);
}

// ============================================================================
// LTP Layer — Sub-Node BTree
// ============================================================================

std::expected<QHash<uint64_t, sak::PstNode>, error_code> PstParser::readSubNodeBTree(
    uint64_t subnode_bid, int depth) {
    if (depth > kMaxBTreeDepth) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    auto block_data = readBlock(subnode_bid);
    if (!block_data) {
        return std::unexpected(block_data.error());
    }

    const auto& data = *block_data;
    if (data.size() < 8) {
        return std::unexpected(error_code::pst_corrupted_btree);
    }

    uint8_t level = static_cast<uint8_t>(data[1]);
    uint16_t entry_count = readLE<uint16_t>(data, 2);
    constexpr int kSubnodeHeaderSize = 8;

    sak::logInfo(
        "PstParser: readSubNodeBTree(0x{:X}) sig=0x{:02X}, "
        "level={}, entries={}, data_size={}",
        subnode_bid,
        static_cast<uint8_t>(data[0]),
        level,
        entry_count,
        data.size());

    if (level == 0) {
        return readSubNodeLeafEntries(data, kSubnodeHeaderSize, entry_count);
    }
    return readSubNodeIntermediateEntries(data, kSubnodeHeaderSize, entry_count, depth);
}

QHash<uint64_t, sak::PstNode> PstParser::readSubNodeLeafEntries(const QByteArray& data,
                                                                int header_size,
                                                                uint16_t entry_count) {
    QHash<uint64_t, sak::PstNode> result;
    int entry_size = m_is_unicode ? 24 : 12;

    for (int entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
        int offset = header_size + (entry_idx * entry_size);
        if (offset + entry_size > data.size()) {
            break;
        }

        sak::PstNode node;
        if (m_is_unicode) {
            node.node_id = readLE<uint64_t>(data, offset) & 0xFF'FF'FF'FF;
            node.data_bid = readLE<uint64_t>(data, offset + 8);
            node.subnode_bid = readLE<uint64_t>(data, offset + 16);
        } else {
            node.node_id = readLE<uint32_t>(data, offset);
            node.data_bid = readLE<uint32_t>(data, offset + 4);
            node.subnode_bid = readLE<uint32_t>(data, offset + 8);
        }
        result.insert(node.node_id, node);
    }
    return result;
}

std::expected<QHash<uint64_t, sak::PstNode>, error_code> PstParser::readSubNodeIntermediateEntries(
    const QByteArray& data, int header_size, uint16_t entry_count, int depth) {
    QHash<uint64_t, sak::PstNode> result;
    int entry_size = m_is_unicode ? 16 : 8;
    int bid_offset = m_is_unicode ? 8 : 4;

    for (int entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
        int offset = header_size + (entry_idx * entry_size);
        if (offset + entry_size > data.size()) {
            break;
        }

        uint64_t child_bid = m_is_unicode ? readLE<uint64_t>(data, offset + bid_offset)
                                          : readLE<uint32_t>(data, offset + bid_offset);

        auto child_result = readSubNodeBTree(child_bid, depth + 1);
        if (child_result) {
            for (auto it = child_result->begin(); it != child_result->end(); ++it) {
                result.insert(it.key(), it.value());
            }
        }
    }
    return result;
}

// ============================================================================
// LTP Layer — Property Formatting
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

    // Unknown type — hex dump for small values, size summary for large
    constexpr int kMaxInlineHexBytes = 16;
    if (raw_value.size() <= kMaxInlineHexBytes) {
        return raw_value.toHex(' ');
    }
    return QStringLiteral("(%1 bytes, type 0x%2)")
        .arg(raw_value.size())
        .arg(prop_type, 4, 16, QLatin1Char('0'));
}

// clang-format off
static const QHash<uint16_t, QString>& propertyNameTable() {
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
// clang-format on

QString PstParser::propertyIdToName(uint16_t prop_id) {
    const auto& table = propertyNameTable();
    auto name_it = table.find(prop_id);
    if (name_it != table.end()) {
        return name_it.value();
    }
    return QStringLiteral("0x%1").arg(prop_id, 4, 16, QLatin1Char('0'));
}

// ============================================================================
// Messaging Layer — Folder Hierarchy
// ============================================================================

std::expected<sak::PstFolderTree, error_code> PstParser::buildFolderHierarchy(uint64_t root_nid,
                                                                              int depth) {
    if (depth > sak::email::kMaxFolderDepth) {
        return std::unexpected(error_code::pst_folder_traversal_error);
    }
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return std::unexpected(error_code::operation_cancelled);
    }

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

    loadChildFolders(folder, root_nid, depth);

    folder.subfolder_count = folder.children.size();

    sak::PstFolderTree tree;
    tree.append(std::move(folder));
    return tree;
}

void PstParser::loadChildFolders(sak::PstFolder& folder, uint64_t root_nid, int depth) {
    uint64_t hierarchy_nid = (root_nid & ~static_cast<uint64_t>(0x1F)) |
                             sak::email::kNidTypeHierarchyTable;

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
    for (uint64_t child_nid : child_nids) {
        auto child_tree = buildFolderHierarchy(child_nid, depth + 1);
        if (child_tree && !child_tree->isEmpty()) {
            auto child_folder = (*child_tree)[0];
            child_folder.parent_node_id = root_nid;
            folder.children.append(std::move(child_folder));
        }
    }
}

// ============================================================================
// Messaging Layer — Contents Table
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
    int start = std::min(offset, static_cast<int>(rows.size()));
    int end_idx = (limit > 0) ? std::min(start + limit, static_cast<int>(rows.size()))
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
// Messaging Layer — Full Message Read
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

    auto att_result = readAttachments(message_nid);
    if (att_result) {
        detail.attachments = std::move(*att_result);
    }

    return detail;
}

// ============================================================================
// Messaging Layer — Lightweight Sender Read
// ============================================================================

std::pair<QString, QString> PstParser::extractSenderFromLeaf(const BthLeafResult& bth,
                                                             const HeapContext& ctx) {
    int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return {};
    }

    int record_count = bth.leaf_data.size() / record_size;
    const auto& slot_map = senderPropSlots();
    QString results[2];  // 0=name, 1=email

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }

        uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
        auto slot_it = slot_map.find(prop_id);
        if (slot_it == slot_map.end()) {
            continue;
        }
        if (!results[slot_it.value()].isEmpty()) {
            continue;
        }

        uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
        uint32_t value_ref = readLE<uint32_t>(bth.leaf_data, rec_offset + bth.key_size + 2);

        QByteArray raw_value;
        if (isPcVariableType(prop_type)) {
            auto resolved =
                resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
            if (resolved) {
                raw_value = std::move(*resolved);
            }
        } else {
            raw_value.resize(4);
            std::memcpy(raw_value.data(), &value_ref, 4);
        }

        results[slot_it.value()] = formatPropertyValue(prop_type, raw_value);
        if (!results[0].isEmpty() && !results[1].isEmpty()) {
            break;
        }
    }

    return {results[0], results[1]};
}

std::pair<QString, QString> PstParser::readSenderFromPC(uint64_t message_nid) {
    auto node_it = m_nbt_cache.find(message_nid);
    if (node_it == m_nbt_cache.end()) {
        return {};
    }

    const auto& node = node_it.value();
    if (node.data_bid == 0) {
        return {};
    }

    HeapContext ctx;
    auto data_result = readDataTree(node.data_bid, &ctx.block_offsets);
    if (!data_result) {
        return {};
    }
    ctx.heap_data = std::move(*data_result);

    if (node.subnode_bid != 0) {
        auto sn_result = readSubNodeBTree(node.subnode_bid);
        if (sn_result) {
            ctx.subnode_map = std::move(*sn_result);
        }
    }

    auto bth = collectBthLeafData(ctx, kPcSignature);
    if (!bth) {
        return {};
    }

    return extractSenderFromLeaf(*bth, ctx);
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
    uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
    uint32_t value_ref = readLE<uint32_t>(bth.leaf_data, rec_offset + bth.key_size + 2);
    QByteArray raw_value;
    if (isPcVariableType(prop_type)) {
        auto resolved = resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
        if (resolved) {
            raw_value = std::move(*resolved);
        }
    } else {
        raw_value.resize(4);
        std::memcpy(raw_value.data(), &value_ref, 4);
    }
    return formatPropertyValue(prop_type, raw_value);
}

void PstParser::scanBthForSubjectAndClass(const BthLeafResult& bth,
                                          const HeapContext& ctx,
                                          sak::PstItemSummary& item,
                                          bool need_subject,
                                          bool need_class) {
    int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return;
    }
    int record_count = bth.leaf_data.size() / record_size;
    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }
        uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
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

    bool need_subject = item.subject.isEmpty();
    bool need_class = (item.item_type == sak::EmailItemType::Unknown);
    if (need_subject || need_class) {
        scanBthForSubjectAndClass(bth, ctx, item, need_subject, need_class);
    }
}

void PstParser::enrichSingleItemProps(sak::PstItemSummary& item) {
    if (item.node_id == 0) {
        return;
    }

    bool need_sender = item.sender_name.isEmpty();
    bool need_subject = item.subject.isEmpty();
    bool need_class = (item.item_type == sak::EmailItemType::Unknown);
    if (!need_sender && !need_subject && !need_class) {
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
// Messaging Layer — Attachments
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
        auto sn = readSubNodeBTree(subnode.subnode_bid);
        if (sn) {
            ctx.subnode_map = std::move(*sn);
        }
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
    int record_size = bth.key_size + bth.data_size;
    if (record_size == 0) {
        return;
    }

    int record_count = bth.leaf_data.size() / record_size;
    const auto& setters = attachmentSetters();

    for (int rec_idx = 0; rec_idx < record_count; ++rec_idx) {
        int rec_offset = rec_idx * record_size;
        if (rec_offset + record_size > bth.leaf_data.size()) {
            break;
        }

        uint16_t prop_id = readLE<uint16_t>(bth.leaf_data, rec_offset);
        uint16_t prop_type = readLE<uint16_t>(bth.leaf_data, rec_offset + bth.key_size);
        uint32_t value_ref = readLE<uint32_t>(bth.leaf_data, rec_offset + bth.key_size + 2);

        QByteArray raw_val;
        if (isPcVariableType(prop_type)) {
            auto resolved =
                resolveHnid(value_ref, ctx.heap_data, ctx.block_offsets, ctx.subnode_map);
            if (resolved) {
                raw_val = std::move(*resolved);
            }
        } else {
            raw_val.resize(4);
            std::memcpy(raw_val.data(), &value_ref, 4);
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
        uint8_t sub_type = static_cast<uint8_t>(sub_it.key() & 0x1F);
        if (sub_type != sak::email::kNidTypeAttachment) {
            continue;
        }

        auto att = readSingleAttachment(sub_it.value(), att_index);
        if (att) {
            attachments.append(std::move(*att));
            ++att_index;
        }
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
        {"IPM.Note", sak::EmailItemType::Email},
        {"IPM.Post", sak::EmailItemType::Email},
        {"IPM.Report", sak::EmailItemType::Email},
        {"IPM.Contact", sak::EmailItemType::Contact},
        {"IPM.Appointment", sak::EmailItemType::Calendar},
        {"IPM.Task", sak::EmailItemType::Task},
        {"IPM.StickyNote", sak::EmailItemType::StickyNote},
        {"IPM.Activity", sak::EmailItemType::JournalEntry},
        {"IPM.DistList", sak::EmailItemType::DistList},
        {"IPM.Schedule.Meeting", sak::EmailItemType::MeetingRequest},
        {"IPM", sak::EmailItemType::Email},
    };

    for (const auto& m : kMappings) {
        if (message_class.startsWith(QLatin1String(m.prefix), Qt::CaseInsensitive)) {
            return m.type;
        }
    }
    return sak::EmailItemType::Unknown;
}

sak::PstNodeType PstParser::nodeType(uint64_t nid) {
    return static_cast<sak::PstNodeType>(nid & 0x1F);
}

std::expected<QByteArray, error_code> PstParser::readBytes(qint64 offset, qint64 count) {
    Q_ASSERT(m_file.isOpen());
    Q_ASSERT(count > 0);

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
    Q_ASSERT(offset >= 0);
    Q_ASSERT(offset + static_cast<int>(sizeof(T)) <= data.size());

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
    std::function<int(const sak::PstFolderTree&)> count_recursive;
    count_recursive = [&](const sak::PstFolderTree& tree) -> int {
        int total = 0;
        for (const auto& folder : tree) {
            total += folder.content_count;
            total += count_recursive(folder.children);
        }
        return total;
    };
    return count_recursive(m_folder_tree);
}

int PstParser::countFolders(const sak::PstFolderTree& tree) {
    int count = 0;
    for (const auto& folder : tree) {
        count += 1;
        count += countFolders(folder.children);
    }
    return count;
}
