// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QVector>

#include <atomic>

/// @file ai_mcp_stdio_session.h
/// @brief A persistent Model Context Protocol stdio session. Unlike
/// AiMcpStdioClient::callTool -- which spawns a fresh server process, runs the
/// initialize handshake, makes one tools/call, and tears the process down every
/// time -- a session keeps the server process alive across calls: it pays the
/// spawn + initialize cost once, exposes tools/list discovery, and then serves
/// many tools/call requests over the same stdin/stdout pipe.
namespace sak::ai {

/// One tool advertised by an MCP server's tools/list response.
struct AiMcpToolDescriptor {
    QString name;
    QString description;
    QJsonObject input_schema;
};

/// Configuration for opening a session against one MCP server executable.
struct AiMcpSessionConfig {
    QString command;
    QProcessEnvironment environment;
    /// Per-request response deadline. Values below a small floor are raised to it
    /// so a caller cannot pass 0 and get an instant timeout.
    int request_timeout_ms{0};
};

/// Owns a long-lived MCP server process and issues JSON-RPC requests to it. The
/// process and all QProcess I/O live on a dedicated worker thread; each public
/// method may be called from any thread OTHER than that worker thread and blocks
/// the caller (via a blocking queued connection) until the matching JSON-RPC
/// response arrives or the request deadline elapses.
///
/// Threading contract: one call at a time. The methods must NOT be invoked
/// concurrently on the same session from two threads -- the member state
/// (open flag, worker/thread pointers) is not internally locked. Sequential calls
/// from different threads are fine; overlapping calls need external serialization.
class AiMcpStdioSession {
public:
    AiMcpStdioSession();
    ~AiMcpStdioSession();

    AiMcpStdioSession(const AiMcpStdioSession&) = delete;
    AiMcpStdioSession& operator=(const AiMcpStdioSession&) = delete;

    /// Starts the server process and runs the initialize / notifications initialized
    /// handshake. Returns false (and sets @p error_message) if the command is
    /// missing, the process cannot start, or the handshake fails/times out. Calling
    /// open() on an already-open session closes the previous one first.
    [[nodiscard]] bool open(const AiMcpSessionConfig& config, QString* error_message = nullptr);

    [[nodiscard]] bool isOpen() const;

    /// Sends tools/list and returns the advertised tools. On error returns an empty
    /// vector and sets @p error_message.
    [[nodiscard]] QVector<AiMcpToolDescriptor> listTools(QString* error_message = nullptr);

    /// Sends tools/call and returns the raw JSON-RPC result object (the caller reads
    /// result.content). On error returns an empty object and sets @p error_message.
    [[nodiscard]] QJsonObject callTool(const QString& tool_name,
                                       const QJsonObject& arguments,
                                       QString* error_message = nullptr);

    /// Stops the server process and joins the worker thread. Idempotent; also run
    /// by the destructor.
    void close();

    /// @name Test seams (payload shape + response parsing; no live process).
    /// @{
    [[nodiscard]] static QJsonObject toolsListPayloadForTesting(int id);
    [[nodiscard]] static QVector<AiMcpToolDescriptor> parseToolsListResultForTesting(
        const QJsonObject& result);
    /// @}

private:
    class Worker;
    Worker* m_worker{nullptr};  // owned by m_thread (deleted on thread finish)
    class QThread* m_thread{nullptr};
    // Atomic so a concurrent isOpen() read cannot tear against an open()/close() write. Callers
    // must still serialize the blocking calls themselves (documented above); this only hardens
    // the flag itself.
    std::atomic<bool> m_open{false};
};

}  // namespace sak::ai
