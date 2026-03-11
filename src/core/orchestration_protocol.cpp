// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/orchestration_protocol.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace sak {

namespace {

struct OrchestrationTypeEntry {
    OrchestrationMessageType type;
    const char* name;
};

static constexpr OrchestrationTypeEntry kOrchestrationTypes[] = {
    {OrchestrationMessageType::DestinationRegister, "DESTINATION_REGISTER"},
    {OrchestrationMessageType::HealthCheckRequest, "HEALTH_CHECK_REQUEST"},
    {OrchestrationMessageType::HealthCheckResponse, "HEALTH_CHECK_RESPONSE"},
    {OrchestrationMessageType::DeploymentAssign, "DEPLOYMENT_ASSIGN"},
    {OrchestrationMessageType::AssignmentControl, "ASSIGNMENT_CONTROL"},
    {OrchestrationMessageType::StartTransfer, "START_TRANSFER"},
    {OrchestrationMessageType::ProgressUpdate, "PROGRESS_UPDATE"},
    {OrchestrationMessageType::DeploymentComplete, "DEPLOYMENT_COMPLETE"},
    {OrchestrationMessageType::Error, "ERROR"},
    {OrchestrationMessageType::Heartbeat, "HEARTBEAT"},
};

}  // namespace

QJsonObject OrchestrationProtocol::makeMessage(OrchestrationMessageType type,
                                               const QJsonObject& payload) {
    QJsonObject message = payload;
    message["message_type"] = typeToString(type);
    message["protocol_version"] = "1.0";
    return message;
}

std::optional<OrchestrationMessageType> OrchestrationProtocol::parseType(const QString& type) {
    Q_ASSERT(!type.isEmpty());
    for (const auto& entry : kOrchestrationTypes) {
        if (type == QLatin1String(entry.name)) {
            return entry.type;
        }
    }
    return std::nullopt;
}

QString OrchestrationProtocol::typeToString(OrchestrationMessageType type) {
    for (const auto& entry : kOrchestrationTypes) {
        if (entry.type == type) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("UNKNOWN");
}

QByteArray OrchestrationProtocol::encodeMessage(const QJsonObject& message) {
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

bool OrchestrationProtocol::writeMessage(QTcpSocket* socket, const QJsonObject& message) {
    if (!socket) {
        return false;
    }
    QByteArray framed = encodeMessage(message);
    const auto bytes_written = socket->write(framed);
    return bytes_written == framed.size();
}

QList<QJsonObject> OrchestrationProtocol::readMessages(QByteArray& buffer,
                                                       const QByteArray& incoming) {
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
