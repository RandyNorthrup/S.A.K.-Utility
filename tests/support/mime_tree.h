// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mime_tree.h
/// @brief Reads how a written message's parts nest, as text a test can compare.

#pragma once

#include <QByteArray>
#include <QByteArrayList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace sak::test {

/// One entity's headers and body, split at its first blank line.
inline void splitMimeEntity(const QByteArray& entity, QString* headers, QByteArray* body) {
    const qsizetype blank = entity.indexOf("\r\n\r\n");
    *headers = QString::fromLatin1(blank < 0 ? entity : entity.left(blank));
    *body = blank < 0 ? QByteArray() : entity.mid(blank + 4);
}

/// The tree of a MIME entity: "mixed(related(alternative(text/plain,text/html),image/png),a/b)".
/// A real reader follows the boundaries, so this does too: a part in the wrong container reads as
/// being in the wrong container, however the text happens to be ordered.
inline QString mimeTree(const QByteArray& entity) {
    QString headers;
    QByteArray body;
    splitMimeEntity(entity, &headers, &body);
    headers.replace(QStringLiteral("\r\n "), QStringLiteral(" "));
    static const QRegularExpression kType(QStringLiteral(R"(Content-Type:\s*([^;\r\n]+))"));
    static const QRegularExpression kBoundary(QStringLiteral("boundary=\"([^\"]+)\""));
    const QString type = kType.match(headers).captured(1).trimmed();
    if (!type.startsWith(QStringLiteral("multipart/"))) {
        return type;
    }
    const QByteArray delimiter = "--" + kBoundary.match(headers).captured(1).toLatin1();
    QStringList children;
    const QByteArrayList pieces = ("\r\n" + body).split('\n');
    QByteArray part;
    bool inside = false;
    for (const QByteArray& raw : pieces) {
        const QByteArray line = raw.endsWith('\r') ? raw.chopped(1) : raw;
        if (line == delimiter || line == delimiter + "--") {
            if (inside) {
                children.append(mimeTree(part));
            }
            part.clear();
            inside = line == delimiter;
            continue;
        }
        if (inside) {
            part += line + "\r\n";
        }
    }
    return type.mid(QStringLiteral("multipart/").size()) + QLatin1Char('(') +
           children.join(QLatin1Char(',')) + QLatin1Char(')');
}

}  // namespace sak::test
