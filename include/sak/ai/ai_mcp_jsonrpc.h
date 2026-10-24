// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>

#include <expected>

/// @file ai_mcp_jsonrpc.h
/// @brief Single source of truth for the Model Context Protocol JSON-RPC 2.0
/// framing shared by the persistent stdio session and bundled server. Keeping the
/// protocol version, client identity, and message shapes in one place prevents drift.
namespace sak::ai::mcp {

/// MCP protocol revision this client speaks. Bump in lock-step with server
/// support; clients and servers must advertise the same value.
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

[[nodiscard]] inline QJsonObject toolsListPayload(int id, const QString& cursor = {}) {
    QJsonObject params;
    if (!cursor.isEmpty()) {
        params[QStringLiteral("cursor")] = cursor;
    }
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("method"), QStringLiteral("tools/list")},
                       {QStringLiteral("params"), params}};
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

/// Returns an initialize-response contract error, or an empty string for a usable result.
[[nodiscard]] inline QString initializeResponseError(const QJsonObject& message) {
    const QJsonValue result = message.value(QStringLiteral("result"));
    if (!result.isObject()) {
        return QStringLiteral("MCP initialize response is missing a result object");
    }
    if (result.toObject().value(QStringLiteral("protocolVersion")).toString().trimmed().isEmpty()) {
        return QStringLiteral("MCP initialize result missing protocolVersion");
    }
    return {};
}

/// Returns a non-empty surfaced error for a JSON-RPC error response.
[[nodiscard]] inline QString jsonRpcErrorMessage(const QJsonObject& message) {
    const QString detail = message.value(QStringLiteral("error"))
                               .toObject()
                               .value(QStringLiteral("message"))
                               .toString()
                               .trimmed();
    return detail.isEmpty() ? QStringLiteral("MCP JSON-RPC error") : detail;
}

/// Serialize one JSON-RPC object as a single newline-delimited line (the stdio
/// transport frames one message per line).
[[nodiscard]] inline QByteArray jsonLine(const QJsonObject& object) {
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return bytes;
}

/// Parse one newline-delimited JSON-RPC line into its intact object, or return
/// the specific size, syntax, or version failure. Failure cannot resemble an empty success.
[[nodiscard]] inline std::expected<QJsonObject, QString> parseJsonLine(const QByteArray& line) {
    if (line.size() > kMaxJsonRpcMessageBytes) {
        return std::unexpected(
            QStringLiteral("MCP message exceeds the %1-byte ceiling").arg(kMaxJsonRpcMessageBytes));
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::unexpected(
            QStringLiteral("Invalid MCP stdio JSON response: %1").arg(parse_error.errorString()));
    }
    const QJsonObject object = doc.object();
    // JSON-RPC 2.0 mandates the version tag on every request, response, and notification.
    // Reject a message that omits or misstates it rather than passing an unvalidated object
    // onward as success -- an arbitrary syntactically-valid object is not a JSON-RPC message.
    if (object.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0")) {
        return std::unexpected(
            QStringLiteral("MCP message is missing the JSON-RPC 2.0 version tag"));
    }
    return object;
}

}  // namespace sak::ai::mcp
