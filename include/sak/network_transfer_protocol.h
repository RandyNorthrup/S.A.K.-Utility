// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>

#include <optional>

class QTcpSocket;

namespace sak {

enum class TransferMessageType {
    Hello,
    AuthChallenge,
    AuthResponse,
    TransferManifest,
    TransferApprove,
    TransferReject,
    FileTransferStart,
    FileTransferAck,
    TransferComplete,
    Error,
    Heartbeat
};

/// @brief Message framing and serialization for transfer protocol
class TransferProtocol {
public:
    /// @brief Build a JSON message with the given type and optional payload
    static QJsonObject makeMessage(TransferMessageType type, const QJsonObject& payload = {});

    /// @brief Parse a message type string into the corresponding enum value
    /// @param type The string representation of the message type
    /// @return The parsed TransferMessageType, or std::nullopt if unrecognized
    static std::optional<TransferMessageType> parseType(const QString& type);

    /// @brief Convert a TransferMessageType enum value to its string representation
    static QString typeToString(TransferMessageType type);

    /// @brief Encode a JSON message to a length-prefixed byte array
    static QByteArray encodeMessage(const QJsonObject& message);

    /// @brief Write a JSON message to a TCP socket
    static bool writeMessage(QTcpSocket* socket, const QJsonObject& message);

    /// @brief Read and decode all complete messages from incoming data
    static QList<QJsonObject> readMessages(QByteArray& buffer, const QByteArray& incoming);
};

}  // namespace sak
