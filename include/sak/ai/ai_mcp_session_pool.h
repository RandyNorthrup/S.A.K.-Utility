// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_mcp_stdio_client.h"
#include "sak/ai/ai_mcp_stdio_session.h"

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVector>

#include <map>
#include <memory>

/// @file ai_mcp_session_pool.h
/// @brief A thread-safe cache of persistent MCP stdio sessions, keyed so that a
/// server process is reused across tool calls instead of respawned every time --
/// while never sharing one process across two different launch environments (a
/// read-only session is never reused for a full-access call).
namespace sak::ai {

class AiMcpSessionPool {
public:
    AiMcpSessionPool() = default;
    ~AiMcpSessionPool();

    AiMcpSessionPool(const AiMcpSessionPool&) = delete;
    AiMcpSessionPool& operator=(const AiMcpSessionPool&) = delete;

    /// Runs one tool call on the pooled session for (request.command, request
    /// environment), opening the session on first use and reusing it afterwards.
    /// Returns the FULL JSON-RPC message ({"result": ...}), byte-shaped exactly
    /// like AiMcpStdioClient::callTool so it is a drop-in transport. On error
    /// returns {} + sets @p error_message and drops the (possibly dead) session so
    /// the next call reopens it.
    [[nodiscard]] QJsonObject callTool(const AiMcpStdioCallRequest& request,
                                       QString* error_message = nullptr);

    /// Discovers the tools advertised by the server for (request.command, request
    /// environment) via tools/list, reusing the pooled session. request.tool_name
    /// and request.arguments are ignored.
    [[nodiscard]] QVector<AiMcpToolDescriptor> listTools(const AiMcpStdioCallRequest& request,
                                                         QString* error_message = nullptr);

    /// Closes every pooled session (joins their worker threads). Idempotent; also
    /// run by the destructor.
    void closeAll();

    /// Number of pooled session slots currently held (a slot with a live or a
    /// last-failed session). For tests / diagnostics.
    [[nodiscard]] int openSessionCount() const;

    /// The cache key derived for a request (command + a hash of its full launch
    /// environment). Exposed for tests.
    [[nodiscard]] static QString sessionKeyForTesting(const AiMcpStdioCallRequest& request);

private:
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
    // instead of touching freed memory. Returns nullptr when the pool already
    // holds its maximum number of distinct sessions (fail closed).
    [[nodiscard]] std::shared_ptr<Entry> entryFor(const AiMcpStdioCallRequest& request);

    // Opens entry.session for `request` if not already open. Call with entry.mutex
    // held. Returns false (and sets *error) on open failure, dropping the session.
    [[nodiscard]] static bool ensureSessionOpen(Entry& entry,
                                                const AiMcpStdioCallRequest& request,
                                                QString* error);

    mutable QMutex m_mutex;  // guards m_sessions (the map), not the sessions
    std::map<QString, std::shared_ptr<Entry>> m_sessions;
};

}  // namespace sak::ai
