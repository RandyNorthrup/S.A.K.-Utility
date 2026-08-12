// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_header_parser.h
/// @brief Pure RFC 5322 header parsing for MBOX/EML messages, split out as a seam.
///
/// The header block of a mail message is attacker-controlled bytes. The parsing was a
/// private member of MboxParser (which owns an open QFile), so it could not be exercised
/// with a raw byte buffer on its own. Lifting it to a pure, stateless free function --
/// with MboxParser::parseHeaders now delegating here -- keeps behaviour identical while
/// letting a fuzz harness feed it malformed header blocks directly (folded/continuation
/// lines, missing colons, embedded CRs, no terminating blank line).
///
/// The output contract, relied on by the fuzz invariant and by every caller: each key is
/// lower-cased and trimmed of surrounding whitespace, and an empty name is never emitted.

#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace sak::mbox {

/// Parse the RFC 5322 header block at the front of @p raw_message into a name->value map.
/// Header names are lower-cased and trimmed; folded continuation lines (leading space or
/// tab) are joined onto the preceding value; parsing stops at the first blank line.
[[nodiscard]] inline QMap<QString, QString> parseRfc5322Headers(const QByteArray& raw_message) {
    QMap<QString, QString> headers;

    // Find the header/body boundary.
    int header_end = static_cast<int>(raw_message.indexOf("\r\n\r\n"));
    if (header_end < 0) {
        header_end = static_cast<int>(raw_message.indexOf("\n\n"));
    }

    const QByteArray header_block = (header_end >= 0) ? raw_message.left(header_end) : raw_message;

    // Split into lines and handle continuation (folded headers).
    const QList<QByteArray> lines = header_block.split('\n');

    QString current_name;
    QString current_value;

    auto flushHeader = [&]() {
        if (!current_name.isEmpty()) {
            headers.insert(current_name.toLower().trimmed(), current_value.trimmed());
        }
        current_name.clear();
        current_value.clear();
    };

    for (const auto& raw_line : lines) {
        QByteArray line = raw_line;
        if (line.endsWith('\r')) {
            line.chop(1);
        }

        if (line.isEmpty()) {
            break;
        }

        // Continuation line (starts with whitespace).
        if (line.startsWith(' ') || line.startsWith('\t')) {
            current_value += QLatin1Char(' ') + QString::fromUtf8(line).trimmed();
            continue;
        }

        // New header.
        flushHeader();
        const int colon = static_cast<int>(line.indexOf(':'));
        if (colon > 0) {
            current_name = QString::fromUtf8(line.left(colon));
            current_value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        }
    }
    flushHeader();

    return headers;
}

}  // namespace sak::mbox
