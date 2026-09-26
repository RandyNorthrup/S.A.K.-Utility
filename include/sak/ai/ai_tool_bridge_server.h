// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace sak::ai {

inline constexpr auto kToolBridgeTokenVariable = "SAK_AI_TOOL_BRIDGE_TOKEN";
inline constexpr auto kToolBridgeServerName = "sak";

/// @brief In-app MCP server that exposes S.A.K.'s local tools to agent runtimes.
///
/// Agent runtimes (Codex, Claude Code, Gemini CLI, Muse Code) are given a
/// standard stdio MCP server entry that launches `sak_ai_tool_bridge`, a tiny
/// relay that connects back here over a per-session local socket guarded by a
/// random token. Tool calls are surfaced as `toolCallRequested` so the panel's
/// existing tool loop (access mode, human gates, elevation, tracing) runs them.
class AiToolBridgeServer : public QObject {
    Q_OBJECT

public:
    explicit AiToolBridgeServer(QObject* parent = nullptr);
    ~AiToolBridgeServer() override;

    bool start(QString* error_message = nullptr);
    void stop();
    [[nodiscard]] bool isListening() const;
    [[nodiscard]] QString serverName() const;
    [[nodiscard]] QString token() const { return m_token; }

    void completeToolCall(const QString& call_id, const QString& output, bool is_error);
    /// @brief Answer every open call with an error (cancel / shutdown).
    void failPendingCalls(const QString& reason);
    [[nodiscard]] int pendingCallCount() const { return static_cast<int>(m_pending.size()); }

    /// @brief MCP `tools/list` entries built from the S.A.K. tool catalog.
    [[nodiscard]] static QJsonArray mcpToolList();
    /// @brief Handle one MCP JSON-RPC message; returns the reply (empty = none).
    ///
    /// `tools/call` returns empty and emits toolCallRequested instead.
    [[nodiscard]] QJsonObject handleMcpMessage(const QString& connection_id,
                                               const QJsonObject& message);

Q_SIGNALS:
    void toolCallRequested(const QString& call_id,
                           const QString& tool_name,
                           const QString& arguments_json);

private:
    struct PendingCall {
        QPointer<QLocalSocket> socket;
        QJsonValue request_id;
    };

    void acceptConnection();
    void readSocket(QLocalSocket* socket);
    void handleLine(QLocalSocket* socket, const QByteArray& line);
    static void writeMessage(QLocalSocket* socket, const QJsonObject& message);

    QLocalServer* m_server;
    QString m_token;
    QHash<QString, PendingCall> m_pending;
    QHash<QLocalSocket*, QByteArray> m_buffers;
    QHash<QLocalSocket*, bool> m_authenticated;
    QPointer<QLocalSocket> m_dispatch_socket;
    qint64 m_next_call{1};
};

}  // namespace sak::ai
