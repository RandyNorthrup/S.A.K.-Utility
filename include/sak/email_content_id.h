// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file email_content_id.h
/// @brief What a Content-ID can be, and whether a message's HTML uses one. The writers share this.

#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

namespace sak {

/// @brief A stored Content-ID as a header can carry it, or empty. It is a token inside angle
///        brackets; one with spaces, brackets or controls would break the header.
[[nodiscard]] inline QByteArray usableContentId(const QString& content_id) {
    constexpr qsizetype kAngleBracketPairLength = 2;
    QString id = content_id;
    if (id.size() >= kAngleBracketPairLength && id.startsWith(QLatin1Char('<')) &&
        id.endsWith(QLatin1Char('>'))) {
        id = id.mid(1, id.size() - kAngleBracketPairLength);
    }
    static const QRegularExpression kToken(QStringLiteral("^[A-Za-z0-9!#$%&'*+/=?^_`{|}~.@-]+$"));
    return kToken.match(id).hasMatch() ? id.toLatin1() : QByteArray();
}

/// @brief Whether @p html shows the part named @p content_id. The URL scheme ignores case and a URL
///        may percent-encode the id. "cid:front" does not cite "front2".
[[nodiscard]] inline bool htmlCitesContentId(const QString& html, const QByteArray& content_id) {
    if (content_id.isEmpty()) {
        return false;
    }
    // \x22 and \x27 are the two quotes: a literal quote in a raw string confuses moc and lizard.
    static const QRegularExpression kReference(QStringLiteral(R"re(cid:([^\s\x22\x27<>)]+))re"),
                                               QRegularExpression::CaseInsensitiveOption);
    const QString id = QString::fromLatin1(content_id);
    auto matches = kReference.globalMatch(html);
    while (matches.hasNext()) {
        const QString cited = matches.next().captured(1);
        if (cited == id || QUrl::fromPercentEncoding(cited.toUtf8()) == id) {
            return true;
        }
    }
    return false;
}

}  // namespace sak
