// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <expected>

/// @file ai_mcp_stdio_session.h
/// @brief A persistent Model Context Protocol stdio session. The server process stays alive across
/// calls: it pays the spawn + initialize cost once, exposes tools/list discovery, and then serves
/// many tools/call requests over the same stdin/stdout pipe.
namespace sak::ai {

/// One pooled MCP stdio tool-call request.
struct AiMcpStdioCallRequest {
    QString command;
    QString tool_name;
    QJsonObject arguments;
    QProcessEnvironment environment;
    int timeout_ms{0};
};

/// One tool advertised by an MCP server's tools/list response.
struct AiMcpToolDescriptor {
    QString name;
    QString description;
    QJsonObject input_schema;
};

/// One validated page from an MCP tools/list result.
struct AiMcpToolsListPage {
    QVector<AiMcpToolDescriptor> tools;
    QString next_cursor;
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
/// Threading contract: open(), listTools(), callTool(), and close() must NOT overlap on one
/// session because worker/thread pointers are not internally locked. Sequential calls from
/// different threads are fine. isOpen() is the one concurrent-safe observation: its process
/// liveness flag is atomic and updated by the worker's exit/error paths.
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
    [[nodiscard]] bool open(const AiMcpSessionConfig& config, QString& error_message);

    /// True only while the initialized server process remains live.
    [[nodiscard]] bool isOpen() const;

    /// Returns the deadline the session applies to a caller-supplied timeout. The pool uses the
    /// same value in its cache key so two requests with identical effective deadlines cannot
    /// consume separate process/thread slots merely because both supplied sub-minimum values.
    [[nodiscard]] static int effectiveRequestTimeoutMs(int requested_ms);

    /// Follows bounded tools/list pagination and returns the complete advertised tool set. On
    /// malformed pages, cursor cycles, page/tool ceilings, transport failure, or timeout, returns
    /// an empty vector and sets @p error_message.
    [[nodiscard]] QVector<AiMcpToolDescriptor> listTools(QString& error_message);

    /// Sends tools/call and returns the raw JSON-RPC result object (the caller reads
    /// result.content). On error returns an empty object and sets @p error_message.
    [[nodiscard]] QJsonObject callTool(const QString& tool_name,
                                       const QJsonObject& arguments,
                                       QString& error_message);

    /// Stops the server process and joins the worker thread. Idempotent; also run
    /// by the destructor.
    void close();

    /// Strict parser shared by live discovery and parser tests. A page without a tools array, a
    /// malformed descriptor, or a non-string nextCursor is an error, never valid empty discovery.
    [[nodiscard]] static std::expected<AiMcpToolsListPage, QString> parseToolsListPage(
        const QJsonObject& result);

private:
    class Worker;
    Worker* m_worker{nullptr};  // deleteLater() on worker-thread finish
    QThread* m_thread{nullptr};
    std::atomic_bool m_open{false};
};

}  // namespace sak::ai
