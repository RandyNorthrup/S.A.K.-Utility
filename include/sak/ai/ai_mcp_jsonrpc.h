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

/// Hard ceiling on a single newline-delimited JSON-RPC message. The stdio transports
/// already bound their own reads, but this fails closed for every caller: a line larger
/// than this is rejected before QJsonDocument::fromJson allocates a DOM for it, so a
/// hostile/oversized message cannot exhaust memory here.
inline constexpr int kMaxJsonRpcMessageBytes = 16 * 1024 * 1024;

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
    if (line.size() > kMaxJsonRpcMessageBytes) {
        if (error_message) {
            *error_message = QStringLiteral("MCP message exceeds the %1-byte ceiling")
                                 .arg(kMaxJsonRpcMessageBytes);
        }
        return {};
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = QStringLiteral("Invalid MCP stdio JSON response: %1")
                                 .arg(parse_error.errorString());
        }
        return {};
    }
    const QJsonObject object = doc.object();
    // JSON-RPC 2.0 mandates the version tag on every request, response, and notification.
    // Reject a message that omits or misstates it rather than passing an unvalidated object
    // onward as success -- an arbitrary syntactically-valid object is not a JSON-RPC message.
    if (object.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0")) {
        if (error_message) {
            *error_message = QStringLiteral("MCP message is missing the JSON-RPC 2.0 version tag");
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return object;
}

}  // namespace sak::ai::mcp
