// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_protocol.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace sak {

namespace {

struct TransferTypeEntry {
    TransferMessageType type;
    const char* name;
};

static constexpr TransferTypeEntry kTransferTypes[] = {
    {TransferMessageType::Hello, "HELLO"},
    {TransferMessageType::AuthChallenge, "AUTH_CHALLENGE"},
    {TransferMessageType::AuthResponse, "AUTH_RESPONSE"},
    {TransferMessageType::TransferManifest, "TRANSFER_MANIFEST"},
    {TransferMessageType::TransferApprove, "TRANSFER_APPROVE"},
    {TransferMessageType::TransferReject, "TRANSFER_REJECT"},
    {TransferMessageType::FileTransferStart, "FILE_TRANSFER_START"},
    {TransferMessageType::FileTransferAck, "FILE_TRANSFER_ACK"},
    {TransferMessageType::TransferComplete, "TRANSFER_COMPLETE"},
    {TransferMessageType::Error, "ERROR"},
    {TransferMessageType::Heartbeat, "HEARTBEAT"},
};

}  // namespace

QJsonObject TransferProtocol::makeMessage(TransferMessageType type, const QJsonObject& payload) {
    QJsonObject message = payload;
    message["message_type"] = typeToString(type);
    message["protocol_version"] = "1.0";
    return message;
}

std::optional<TransferMessageType> TransferProtocol::parseType(const QString& type) {
    Q_ASSERT(!type.isEmpty());
    for (const auto& entry : kTransferTypes) {
        if (type == QLatin1String(entry.name)) {
            return entry.type;
        }
    }
    return std::nullopt;
}

QString TransferProtocol::typeToString(TransferMessageType type) {
    for (const auto& entry : kTransferTypes) {
        if (entry.type == type) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("UNKNOWN");
}

QByteArray TransferProtocol::encodeMessage(const QJsonObject& message) {
    QJsonDocument doc(message);
    QByteArray payload = doc.toJson(QJsonDocument::Compact);

    QByteArray framed;
    framed.resize(4);
    const auto length = static_cast<quint32>(payload.size());
    framed[0] = static_cast<char>((length >> 24) & 0xFF);
    framed[1] = static_cast<char>((length >> 16) & 0xFF);
    framed[2] = static_cast<char>((length >> 8) & 0xFF);
    framed[3] = static_cast<char>(length & 0xFF);
    framed.append(payload);
    return framed;
}

bool TransferProtocol::writeMessage(QTcpSocket* socket, const QJsonObject& message) {
    if (!socket) {
        return false;
    }
    QByteArray framed = encodeMessage(message);
    const auto bytes_written = socket->write(framed);
    return bytes_written == framed.size();
}

QList<QJsonObject> TransferProtocol::readMessages(QByteArray& buffer, const QByteArray& incoming) {
    Q_ASSERT(!buffer.isEmpty());
    Q_ASSERT(!incoming.isEmpty());
    buffer.append(incoming);
    QList<QJsonObject> messages;

    while (buffer.size() >= 4) {
        const quint32 length = (static_cast<quint8>(buffer[0]) << 24) |
                               (static_cast<quint8>(buffer[1]) << 16) |
                               (static_cast<quint8>(buffer[2]) << 8) |
                               static_cast<quint8>(buffer[3]);

        if (buffer.size() < 4 + static_cast<int>(length)) {
            break;
        }

        QByteArray payload = buffer.mid(4, length);
        buffer.remove(0, 4 + length);

        QJsonParseError error{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            messages.append(doc.object());
        }
    }

    return messages;
}

}  // namespace sak
