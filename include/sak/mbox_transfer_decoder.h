// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_transfer_decoder.h
/// @brief Pure MIME Content-Transfer-Encoding decoding (base64 / quoted-printable).
///
/// The encoded body of a mail part is attacker-controlled bytes. The decoding was a
/// private member of MboxParser (base64) plus a file-scope helper (quoted-printable), so
/// neither could be fuzzed on its own. This seam lifts both to pure, stateless functions
/// with MboxParser::decodeTransferEncoding now delegating here. Behaviour is identical,
/// including the strict base64 policy: Qt's default base64 decoder silently yields partial
/// bytes for malformed input, so a decode error must fail the whole part (ok == false)
/// rather than hand back a truncated body. The member turns ok == false back into its
/// m_mime_decode_failed flag; the pure result lets a fuzz harness assert the contract
/// directly.

#pragma once

#include "sak/layout_constants.h"  // sak::kHexBase

#include <QByteArray>
#include <QLatin1String>
#include <QString>
#include <Qt>

namespace sak::mbox {

// Quoted-printable "=XX" sequence geometry: two hex digits at offsets +1 and +2, and the
// high nibble is shifted by four bits.
inline constexpr int kQuotedPrintableHexDigitCount = 2;
inline constexpr int kQuotedPrintableFirstHexOffset = 1;
inline constexpr int kQuotedPrintableSecondHexOffset = 2;
inline constexpr int kQuotedPrintableNibbleShift = 4;

/// Decode quoted-printable data. A malformed "=" sequence (too short, non-hex, or a soft
/// line break) is passed through or skipped exactly as before, so this never fails; the
/// output is always shorter than or equal to the input.
[[nodiscard]] inline QByteArray decodeQuotedPrintable(const QByteArray& data) {
    QByteArray result;
    result.reserve(data.size());

    for (int idx = 0; idx < data.size(); ++idx) {
        const char current_char = data[idx];
        if (current_char != '=') {
            result.append(current_char);
            continue;
        }
        if (idx + kQuotedPrintableHexDigitCount >= data.size()) {
            result.append(current_char);
            continue;
        }
        const char hex1 = data[idx + kQuotedPrintableFirstHexOffset];
        const char hex2 = data[idx + kQuotedPrintableSecondHexOffset];

        // Soft line break.
        if (hex1 == '\r' || hex1 == '\n') {
            idx += (hex1 == '\r' && hex2 == '\n') ? kQuotedPrintableHexDigitCount
                                                  : kQuotedPrintableFirstHexOffset;
            continue;
        }
        // Hex-encoded byte.
        bool ok_first = false;
        bool ok_second = false;
        const int val1 = QByteArray(1, hex1).toInt(&ok_first, sak::kHexBase);
        const int val2 = QByteArray(1, hex2).toInt(&ok_second, sak::kHexBase);
        if (ok_first && ok_second) {
            result.append(static_cast<char>((val1 << kQuotedPrintableNibbleShift) | val2));
            idx += kQuotedPrintableHexDigitCount;
            continue;
        }
        result.append(current_char);
    }
    return result;
}

/// Result of decoding a transfer-encoded part. @c ok is false only when a strict base64
/// decode failed, in which case @c bytes is empty (the caller must fail the part closed
/// rather than use a partial decode).
struct TransferDecodeResult {
    QByteArray bytes;
    bool ok = true;
};

/// Decode @p data according to its Content-Transfer-Encoding. base64 is strict (fails
/// closed on any invalid character); quoted-printable is lenient; 7bit/8bit/binary and any
/// unrecognized encoding pass through unchanged.
[[nodiscard]] inline TransferDecodeResult decodeTransferEncoding(const QByteArray& data,
                                                                 const QString& encoding) {
    if (encoding.compare(QLatin1String("base64"), Qt::CaseInsensitive) == 0) {
        // Strip the line-wrapping whitespace MIME inserts, then decode strictly.
        QByteArray compact = data;
        compact.removeIf(
            [](char byte) { return byte == '\r' || byte == '\n' || byte == ' ' || byte == '\t'; });
        const auto decoded = QByteArray::fromBase64Encoding(
            compact, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return {{}, false};
        }
        return {decoded.decoded, true};
    }
    if (encoding.compare(QLatin1String("quoted-printable"), Qt::CaseInsensitive) == 0) {
        return {decodeQuotedPrintable(data), true};
    }
    // 7bit, 8bit, binary, or unknown -- no decoding needed.
    return {data, true};
}

}  // namespace sak::mbox
