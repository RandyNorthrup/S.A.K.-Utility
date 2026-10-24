// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_mcp_stdio_session.h"

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVector>

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>

class AiMcpSessionPoolTests;

/// @file ai_mcp_session_pool.h
/// @brief A thread-safe cache of persistent MCP stdio sessions, keyed so that a
/// server process is reused across tool calls instead of respawned every time --
/// while never sharing one process across two different launch environments (a
/// read-only session is never reused for a full-access call). InheritFromParent environments are
/// rejected because their effective variables can change without changing a cache key; callers
/// must provide an explicit environment snapshot.
namespace sak::ai {

class AiMcpSessionPool {
public:
    AiMcpSessionPool();
    ~AiMcpSessionPool();

    AiMcpSessionPool(const AiMcpSessionPool&) = delete;
    AiMcpSessionPool& operator=(const AiMcpSessionPool&) = delete;

    /// Runs one tool call on the pooled session for (request.command, request environment,
    /// effective timeout), opening the session on first use and reusing it afterwards. Returns a
    /// transport wrapper shaped as {"result": ...}; JSON-RPC correlation members are consumed by
    /// the persistent session and are not re-synthesized. On error returns a non-empty diagnostic.
    /// A transport/session error drops the session only after checked cleanup. If cleanup fails,
    /// ownership is retained and subsequent pool admissions are refused until closeAll succeeds.
    /// Pre-session validation and capacity refusals leave existing sessions intact.
    [[nodiscard]] std::expected<QJsonObject, QString> callTool(
        const AiMcpStdioCallRequest& request);

    /// Discovers the tools advertised by the server for (request.command, request environment,
    /// effective timeout) via tools/list, reusing the pooled session. request.tool_name and
    /// request.arguments are ignored.
    /// On failure returns a non-empty diagnostic; an empty advertised catalog is a successful
    /// value, not a failure sentinel.
    [[nodiscard]] std::expected<QVector<AiMcpToolDescriptor>, QString> listTools(
        const AiMcpStdioCallRequest& request);

    /// Closes every pooled session with one shared cleanup deadline. Failed entries remain
    /// owned and prevent further admissions until a successful closeAll retry. Idempotent.
    /// Destruction that cannot establish cleanup logs the diagnostic and transfers each unproven
    /// session to sak::ContainedProcessQuarantine, whose kill-on-close Jobs stay armed, instead of
    /// terminating the application. Destroying a pool that is still in use remains fatal.
    [[nodiscard]] std::expected<void, QString> closeAll();

    /// Number of pooled session SLOTS currently held -- including a slot whose session already
    /// failed or was closed and is waiting to be reopened or reclaimed. This is deliberately NOT
    /// a count of live sessions, and is named for what it returns: it was called
    /// openSessionCount(), which promised something the body never computed. For tests and
    /// diagnostics.
    [[nodiscard]] int pooledSlotCount() const;

    /// The cache key derived for a request: command + full launch-environment hash + effective
    /// timeout. Exposed for tests.
    [[nodiscard]] static QString sessionKeyForTesting(const AiMcpStdioCallRequest& request);

private:
    friend class ::AiMcpSessionPoolTests;

    struct Entry {
        std::unique_ptr<AiMcpStdioSession> session;
        QMutex mutex;  // serializes calls to this one session (it is one-at-a-time)
        // Set by closeAll() (under mutex) when this Entry is torn down and dropped from the map.
        // A caller that grabbed the shared handle just before closeAll() must NOT reopen a fresh
        // session on it afterwards -- that would spawn a process after the pool reported closed.
        bool detached{false};
    };

    // Returns a shared handle so an in-flight caller keeps its Entry (and its
    // mutex) alive even if closeAll() clears the map concurrently -- the caller
    // then locks a live mutex and simply sees a null (or detached) session,
    // instead of touching freed memory. Returns a diagnostic when the pool already holds its
    // maximum number of distinct sessions (fail closed).
    [[nodiscard]] std::expected<std::shared_ptr<Entry>, QString> entryFor(
        const AiMcpStdioCallRequest& request);

    // Opens entry.session for `request` if not already open. Call with entry.mutex
    // held. Returns the open diagnostic; drops only proved-clean owners, retaining failed cleanup.
    [[nodiscard]] std::expected<void, QString> ensureSessionOpen(
        Entry& entry, const AiMcpStdioCallRequest& request);
    void releaseIfClosed(Entry& entry);
    [[nodiscard]] QString closeAfterFailure(Entry& entry, const QString& primary_error);
    [[nodiscard]] std::expected<void, QString> closeAllWithDeadline(const QDeadlineTimer& deadline);
    // Destruction-only: moves every session closeAll() could not prove clean into custody.
    void quarantineUnprovenSessions(const QString& cleanup_error);

    mutable QMutex m_mutex;  // guards m_sessions (the map), not the sessions
    std::map<QString, std::shared_ptr<Entry>> m_sessions;
    std::atomic_bool m_cleanup_blocked{false};
    std::function<std::unique_ptr<AiMcpStdioSession>()> m_session_factory;
};

}  // namespace sak::ai
