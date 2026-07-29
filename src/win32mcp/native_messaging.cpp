// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/native_messaging.h"

#include <QJsonDocument>
#include <QtEndian>

namespace sak::win32mcp {

QByteArray encodeFrame(const QJsonObject& message) {
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.reserve(4 + body.size());
    const quint32 length = qToLittleEndian<quint32>(static_cast<quint32>(body.size()));
    frame.append(reinterpret_cast<const char*>(&length), 4);
    frame.append(body);
    return frame;
}

NativeFrame parseFrame(const QByteArray& buffer) {
    NativeFrame frame;
    if (buffer.size() < 4) {
        frame.status = NativeFrame::Status::NeedMore;
        return frame;
    }
    const quint32 length =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(buffer.constData()));
    if (length == 0) {
        frame.status = NativeFrame::Status::Error;
        frame.error = QStringLiteral("Native message length is zero");
        return frame;
    }
    if (length > static_cast<quint32>(kMaxNativeMessageBytes)) {
        frame.status = NativeFrame::Status::Error;
        frame.error = QStringLiteral("Native message length %1 exceeds the %2-byte cap")
                          .arg(length)
                          .arg(kMaxNativeMessageBytes);
        return frame;
    }
    const int total = 4 + static_cast<int>(length);
    if (buffer.size() < total) {
        frame.status = NativeFrame::Status::NeedMore;
        return frame;
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer.mid(4, static_cast<int>(length)),
                                                      &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        frame.status = NativeFrame::Status::Error;
        frame.error = QStringLiteral("Native message is not a JSON object: %1")
                          .arg(parse_error.errorString());
        return frame;
    }
    frame.status = NativeFrame::Status::Ok;
    frame.message = doc.object();
    frame.consumed = total;
    return frame;
}

}  // namespace sak::win32mcp
