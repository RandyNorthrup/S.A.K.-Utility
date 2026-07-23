// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>

/// @file ai_mcp_jsonrpc.h
/// @brief Single source of truth for the Model Context Protocol JSON-RPC 2.0
/// framing used by both the one-shot stdio client and the persistent stdio
/// session. Keeping the protocol version, client identity, and message shapes in
/// one place stops the two transports from drifting apart.
namespace sak::ai::mcp {

/// MCP protocol revision this client speaks. Bump in lock-step with server
/// support; both transports must advertise the same value.
inline constexpr char kProtocolVersion[] = "2024-11-05";

[[nodiscard]] inline QJsonObject initializePayload(int id) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), QStringLiteral("initialize")},
        {QStringLiteral("params"),
         QJsonObject{
             {QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion)},
             {QStringLiteral("capabilities"), QJsonObject{}},
             {QStringLiteral("clientInfo"),
              QJsonObject{{QStringLiteral("name"), QStringLiteral("sak-utility")},
                          {QStringLiteral("version"), QStringLiteral("1")}}},
         }},
    };
}

[[nodiscard]] inline QJsonObject initializedNotification() {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("method"), QStringLiteral("notifications/initialized")},
                       {QStringLiteral("params"), QJsonObject{}}};
}

[[nodiscard]] inline QJsonObject toolsListPayload(int id) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("method"), QStringLiteral("tools/list")},
                       {QStringLiteral("params"), QJsonObject{}}};
}

[[nodiscard]] inline QJsonObject toolCallPayload(int id,
                                                 const QString& tool_name,
                                                 const QJsonObject& arguments) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"), tool_name},
                     {QStringLiteral("arguments"), arguments}}},
    };
}

/// Serialize one JSON-RPC object as a single newline-delimited line (the stdio
/// transport frames one message per line).
[[nodiscard]] inline QByteArray jsonLine(const QJsonObject& object) {
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return bytes;
}

/// Parse one newline-delimited JSON-RPC line into an object. Returns an empty
/// object and sets @p error_message on malformed input; clears it on success.
[[nodiscard]] inline QJsonObject parseJsonLine(const QByteArray& line, QString* error_message) {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = QStringLiteral("Invalid MCP stdio JSON response: %1")
                                 .arg(parse_error.errorString());
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

}  // namespace sak::ai::mcp
