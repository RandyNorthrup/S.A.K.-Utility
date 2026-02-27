// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <optional>

class QTcpSocket;

namespace sak {

enum class OrchestrationMessageType {
    DestinationRegister,
    HealthCheckRequest,
    HealthCheckResponse,
    DeploymentAssign,
    AssignmentControl,
    StartTransfer,
    ProgressUpdate,
    DeploymentComplete,
    Error,
    Heartbeat
};

/// @brief Message framing for orchestrator-destination protocol
class OrchestrationProtocol {
public:
    /// @brief Build a JSON message with the given orchestration type and optional payload
    static QJsonObject makeMessage(OrchestrationMessageType type, const QJsonObject& payload = {});

    /// @brief Parse a message type string into the corresponding enum value
    /// @param type The string representation of the orchestration message type
    /// @return The parsed OrchestrationMessageType, or std::nullopt if unrecognized
    static std::optional<OrchestrationMessageType> parseType(const QString& type);

    /// @brief Convert an OrchestrationMessageType enum value to its string representation
    static QString typeToString(OrchestrationMessageType type);

    /// @brief Encode a JSON message to a length-prefixed byte array
    static QByteArray encodeMessage(const QJsonObject& message);

    /// @brief Write a JSON message to a TCP socket
    static bool writeMessage(QTcpSocket* socket, const QJsonObject& message);

    /// @brief Read and decode all complete messages from incoming data
    static QList<QJsonObject> readMessages(QByteArray& buffer, const QByteArray& incoming);
};

} // namespace sak
