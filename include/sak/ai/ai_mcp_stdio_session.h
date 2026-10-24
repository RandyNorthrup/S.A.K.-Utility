// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

class AiMcpStdioSessionTests;
class AiMcpSessionPoolTests;
class QDeadlineTimer;
class QIODevice;

namespace sak {
class WindowsProcessJob;
}

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
/// method may be called from any thread OTHER than that worker thread. Requests
/// use blocking queued calls through their response deadline; close waits on an
/// owned queued completion through its cleanup deadline.
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
    /// handshake. Returns a diagnostic if the command is missing, the process cannot
    /// start, or the handshake fails/times out. Calling
    /// open() on an already-open session closes the previous one first.
    [[nodiscard]] std::expected<void, QString> open(const AiMcpSessionConfig& config);

    /// Replaces QThread::start for real-allocation observation and launch-failure tests.
    /// Set before open(); an empty function restores normal thread startup.
    void setThreadStarterForTesting(std::function<void(QThread&)> starter);

    /// True only while the initialized server process remains live.
    [[nodiscard]] bool isOpen() const;

    /// Returns the deadline the session applies to a caller-supplied timeout. The pool uses the
    /// same value in its cache key so two requests with identical effective deadlines cannot
    /// consume separate process/thread slots merely because both supplied sub-minimum values.
    [[nodiscard]] static int effectiveRequestTimeoutMs(int requested_ms);

    /// Follows bounded tools/list pagination and returns the complete advertised tool set. On
    /// malformed pages, cursor cycles, page/tool ceilings, transport failure, or timeout, returns
    /// a diagnostic. An empty vector is a successful empty catalog, not an error sentinel.
    [[nodiscard]] std::expected<QVector<AiMcpToolDescriptor>, QString> listTools();

    /// Sends tools/call and returns the raw JSON-RPC result object (the caller reads
    /// result.content), or a diagnostic on failure.
    [[nodiscard]] std::expected<QJsonObject, QString> callTool(const QString& tool_name,
                                                               const QJsonObject& arguments);

    /// Stops the contained process family and joins the worker only after observed cleanup.
    /// Failure retains ownership and refuses further requests; a later close may retry cleanup.
    /// Idempotent. Destruction fails closed if this operation cannot establish safe teardown.
    [[nodiscard]] std::expected<void, QString> close();

    /// Strict parser shared by live discovery and parser tests. A page without a tools array, a
    /// malformed descriptor, or a non-string nextCursor is an error, never valid empty discovery.
    [[nodiscard]] static std::expected<AiMcpToolsListPage, QString> parseToolsListPage(
        const QJsonObject& result);

private:
    friend class ::AiMcpStdioSessionTests;
    friend class ::AiMcpSessionPoolTests;
    friend class AiMcpSessionPool;

    class Worker;
    struct PendingCleanup;
    using JobFactory =
        std::function<std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString>()>;
    [[nodiscard]] std::expected<void, QString> startWorker();
    [[nodiscard]] std::expected<void, QString> closeWithDeadline(const QDeadlineTimer& deadline);
    [[nodiscard]] std::expected<void, QString> cleanupWorkerBeforeJoin(
        const QDeadlineTimer& deadline);
    [[nodiscard]] static std::expected<std::optional<QJsonObject>, QString> drainMatchingLines(
        QIODevice& input,
        int id,
        const QDeadlineTimer& deadline,
        const std::function<QString()>& timeout_message);
    [[nodiscard]] QString closeAfterFailure(const QString& primary_error);
    [[nodiscard]] bool isClosed() const;
    /// Exposes the actual QObject only to friend tests for controlled worker-lifetime faults.
    [[nodiscard]] QObject* workerObjectForTesting() const;
    Worker* m_worker{nullptr};            // deleteLater() on worker-thread finish
    QPointer<QObject> m_worker_liveness;  // Observe proved Worker destruction after join.
    QThread* m_thread{nullptr};
    bool m_join_requested{false};         // Process cleanup proved; only join remains on a retry.
    std::shared_ptr<PendingCleanup> m_pending_cleanup;  // One queued close, retained on timeout.
    std::function<void(QThread&)> m_thread_starter;
    JobFactory m_job_factory;
    std::atomic_bool m_open{false};
    // Private boundary-test seed, copied into the real Worker before it starts serving requests.
    // Normal sessions always start with zero IDs issued; no public counter override exists.
    int m_initial_request_id_for_testing{0};
};

}  // namespace sak::ai
