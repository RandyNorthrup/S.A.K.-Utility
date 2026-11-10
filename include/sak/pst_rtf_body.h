// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_rtf_body.h
/// @brief Compressed RTF message bodies (MS-OXRTFCP) and their conversion to text.
///
/// Outlook keeps some messages, and most calendar items, with PR_RTF_COMPRESSED as their only
/// body. Without reading it those items showed a blank preview and could not be converted.

#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace sak::pst {

/// The text carried by an RTF body. Outlook wraps HTML and plain-text mail in RTF, marked
/// \\fromhtml1 or \\fromtext; anything else is real RTF, reduced here to its text.
struct RtfBodyText {
    QString text;
    bool is_html = false;
};

/// Inflate a PR_RTF_COMPRESSED value (MS-OXRTFCP). Returns nothing for a value that fails its
/// header, size or CRC checks: a damaged body must not surface as garbled text. Uncompressed
/// ("MELA") values are returned as stored, after the same checks.
[[nodiscard]] std::optional<QByteArray> decompressRtf(const QByteArray& compressed);

/// Pull the text out of an RTF document. Formatting, font and colour tables, pictures and other
/// hidden destinations are dropped.
[[nodiscard]] RtfBodyText rtfToText(const QByteArray& rtf);

}  // namespace sak::pst
