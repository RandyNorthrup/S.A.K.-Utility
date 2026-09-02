// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace sak::ai {

/// @brief Blocking one-shot MCP-over-HTTP transport.
///
/// Calls must run off the GUI thread. Remote endpoints require HTTPS; plain HTTP is accepted only
/// for loopback with proxy use disabled. Requests and responses are size bounded, redirects stay
/// same-origin, timeout input is clamped to the supported range, and JSON-RPC responses must match
/// the request id. Transport, protocol, and logical JSON-RPC failures return an empty object and
/// populate the mandatory error sink.
class AiMcpHttpClient {
public:
    AiMcpHttpClient() = delete;

    /// Executes one bounded tools/call request and returns its full JSON-RPC response object.
    [[nodiscard]] static QJsonObject callTool(const QUrl& endpoint,
                                              const QString& tool_name,
                                              const QJsonObject& arguments,
                                              int timeout_ms,
                                              QString& error_message);

    /// Parses a complete JSON or SSE response body through the production framing rules.
    [[nodiscard]] static QJsonObject extractJsonRpcMessageForTesting(
        const QByteArray& response_body, QString* error_message = nullptr);
    /// Builds the production tools/call payload for shape tests.
    [[nodiscard]] static QJsonObject toolCallPayloadForTesting(const QString& tool_name,
                                                               const QJsonObject& arguments);
    /// @brief Whether a JSON-RPC response echoes our request id (correlation). A response
    ///        with a different -- or non-numeric -- id is a stray/out-of-band message and
    ///        must be rejected, never accepted as the tool result. Test seam for the
    ///        correlation gate in callTool().
    [[nodiscard]] static bool responseIdMatchesRequestForTesting(const QJsonObject& message);
};

}  // namespace sak::ai
