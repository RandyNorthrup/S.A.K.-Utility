// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_session_pool.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QStringList>

#include <algorithm>
#include <limits>
#include <mutex>

namespace sak::ai {

namespace {

// Hard ceiling on the number of distinct pooled sessions. Each slot can own a live server
// process + worker thread, so an unbounded map (distinct command/env/timeout keys) would let a
// caller exhaust processes/threads/handles. When the pool is full and a new key arrives we first
// reclaim any already-dead slot, then fail the call closed rather than spawning without limit.
constexpr std::size_t kMaxPooledSessions = 32;
constexpr int kPoolCleanupTimeoutMs = 5000;

std::unique_lock<QMutex> lockBeforeDeadline(QMutex& mutex, const QDeadlineTimer& deadline) {
    const qint64 remaining =
        std::clamp<qint64>(deadline.remainingTime(), 0, std::numeric_limits<int>::max());
    if (!mutex.tryLock(static_cast<int>(remaining))) {
        return {};
    }
    return std::unique_lock<QMutex>(mutex, std::adopt_lock);
}

// The cache key must isolate launch environments: a server started read-only must
// never be reused for a full-access call. Key on the command plus a hash of the
// FULL, sorted environment, so any env difference (security profile, redaction
// flag, anything) yields a distinct pooled session. Each env entry is length-
// prefixed into the hash so an embedded newline in one value cannot make two
// distinct environments (e.g. {"A=x\nB=y"} vs {"A=x","B=y"}) collide.
// timeout_ms is part of the key too: a pooled session bakes its request timeout in
// at open() time, so a later call with a different timeout must NOT inherit the
// first call's (now stale) timeout -- it keys to its own session instead.
QString sessionKey(const AiMcpStdioCallRequest& request) {
    QStringList entries = request.environment.toStringList();
    std::ranges::sort(entries);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // INHERITANCE IS PART OF THE ENVIRONMENT, and toStringList() cannot express it: an
    // inherit-from-parent environment reports an EMPTY list, exactly like one that was
    // deliberately emptied. Those two are as different as launch environments get -- one hands
    // the server every variable this process holds, the other hands it none -- yet they hashed
    // identically, so a session opened with the full user environment could be reused for a call
    // that asked for a bare one. That is the precise failure this key exists to prevent.
    hash.addData(request.environment.inheritsFromParent() ? QByteArrayLiteral("inherit:1")
                                                          : QByteArrayLiteral("inherit:0"));
    for (const QString& entry : entries) {
        const QByteArray bytes = entry.toUtf8();
        hash.addData(QByteArray::number(bytes.size()) + ':');
        hash.addData(bytes);
    }
    return request.command.trimmed() + QLatin1Char('\x1f') +
           QString::fromLatin1(hash.result().toHex()) + QLatin1Char('\x1f') +
           QString::number(AiMcpStdioSession::effectiveRequestTimeoutMs(request.timeout_ms));
}

AiMcpSessionConfig configFor(const AiMcpStdioCallRequest& request) {
    return {.command = request.command,
            .environment = request.environment,
            .request_timeout_ms = AiMcpStdioSession::effectiveRequestTimeoutMs(request.timeout_ms)};
}

std::expected<void, QString> environmentCanBePooled(const AiMcpStdioCallRequest& request) {
    if (!request.environment.inheritsFromParent()) {
        return {};
    }
    return std::unexpected(
        QStringLiteral("MCP session pool requires an explicit process environment"));
}

}  // namespace

AiMcpSessionPool::AiMcpSessionPool()
    : m_session_factory([]() { return std::make_unique<AiMcpStdioSession>(); }) {}

AiMcpSessionPool::~AiMcpSessionPool() {
    const auto closed = closeAll();
    if (!closed) {
        quarantineUnprovenSessions(closed.error());
    }
}

void AiMcpSessionPool::quarantineUnprovenSessions(const QString& cleanup_error) {
    // Report + contain (owner decision 2026-10-24). This was a qFatal, so one pooled child that
    // outlived the shared cleanup deadline under machine load took the whole application down.
    // closeAll() already spent that deadline; each still-owned session goes to process-lifetime
    // custody as it stands, without a second wait, where its kill-on-close Job stays armed.
    //
    // A lock held NOW is different: closeAll() has released both levels, so a holder is a caller
    // still running inside a pool under destruction. That use-after-free is a programming error
    // no custody can contain, and it still stops the process.
    const std::unique_lock<QMutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        qFatal("MCP pool destroyed while in use: %s", qPrintable(cleanup_error));
    }
    const QString reason =
        QStringLiteral("MCP pool destruction could not prove cleanup: %1").arg(cleanup_error);
    for (const auto& slot : m_sessions) {
        Entry& entry = *slot.second;
        const std::unique_lock<QMutex> entry_lock(entry.mutex, std::try_to_lock);
        if (!entry_lock.owns_lock()) {
            qFatal("MCP pool destroyed during an in-flight call: %s", qPrintable(cleanup_error));
        }
        entry.detached = true;
        if (entry.session) {
            AiMcpStdioSession::quarantine(std::move(entry.session), reason);
        }
    }
    m_sessions.clear();
}

std::expected<std::shared_ptr<AiMcpSessionPool::Entry>, QString> AiMcpSessionPool::entryFor(
    const AiMcpStdioCallRequest& request) {
    const QString key = sessionKey(request);
    const QMutexLocker lock(&m_mutex);
    if (m_cleanup_blocked.load(std::memory_order_acquire)) {
        return std::unexpected(QStringLiteral("MCP pool cleanup is unresolved; refusing reuse"));
    }
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        if (m_sessions.size() >= kMaxPooledSessions) {
            // Full: reclaim any dead slot (a key whose session already failed/closed and was
            // dropped) before refusing, so transient churn cannot permanently wedge the pool.
            // tryLock keeps us from ever blocking on an in-flight call while holding m_mutex.
            for (auto i = m_sessions.begin(); i != m_sessions.end();) {
                Entry& candidate = *i->second;
                bool dead = false;
                if (candidate.mutex.tryLock()) {
                    dead = !candidate.session;
                    candidate.mutex.unlock();
                }
                if (dead) {
                    i = m_sessions.erase(i);
                } else {
                    ++i;
                }
            }
        }
        if (m_sessions.size() >= kMaxPooledSessions) {
            return std::unexpected(QStringLiteral("MCP session pool is at capacity"));
        }
        it = m_sessions.emplace(key, std::make_shared<Entry>()).first;
    }
    // Hand back a shared handle: the caller locks entry->mutex OUTSIDE m_mutex, and
    // holding this reference keeps the Entry alive even if closeAll() clears the map
    // in between (the caller then just reopens a null session).
    return it->second;
}

std::expected<void, QString> AiMcpSessionPool::ensureSessionOpen(
    Entry& entry, const AiMcpStdioCallRequest& request) {
    if (entry.detached) {
        // closeAll() tore this Entry down and dropped it from the map. Refuse to reopen so a call
        // that grabbed the handle mid-teardown cannot spawn a process after shutdown.
        return std::unexpected(QStringLiteral("MCP session pool is closing"));
    }
    if (m_cleanup_blocked.load(std::memory_order_acquire)) {
        return std::unexpected(QStringLiteral("MCP pool cleanup is unresolved; refusing reuse"));
    }
    if (entry.session && entry.session->isOpen()) {
        return {};
    }
    if (entry.session) {
        const auto closed = entry.session->close();
        if (!closed) {
            m_cleanup_blocked.store(true, std::memory_order_release);
            return std::unexpected(closed.error());
        }
        entry.session.reset();
    }
    entry.session = m_session_factory();
    if (!entry.session) {
        return std::unexpected(QStringLiteral("MCP session factory returned no session"));
    }
    const auto open_result = entry.session->open(configFor(request));
    if (!open_result) {
        // open() already attempted its one cleanup boundary. Never retry it implicitly or
        // destroy retained ownership when that attempt could not prove cleanup.
        releaseIfClosed(entry);
        return std::unexpected(open_result.error());
    }
    return {};
}

void AiMcpSessionPool::releaseIfClosed(Entry& entry) {
    if (entry.session->isClosed()) {
        entry.session.reset();
    } else {
        m_cleanup_blocked.store(true, std::memory_order_release);
    }
}

QString AiMcpSessionPool::closeAfterFailure(Entry& entry, const QString& primary_error) {
    const QString diagnostic = entry.session->closeAfterFailure(primary_error);
    releaseIfClosed(entry);
    return diagnostic;
}

std::expected<QJsonObject, QString> AiMcpSessionPool::callTool(
    const AiMcpStdioCallRequest& request) {
    if (request.command.trimmed().isEmpty()) {
        return std::unexpected(QStringLiteral("MCP stdio command is empty"));
    }
    if (request.tool_name.trimmed().isEmpty()) {
        return std::unexpected(QStringLiteral("MCP tool name is empty"));
    }
    const auto environment_result = environmentCanBePooled(request);
    if (!environment_result) {
        return std::unexpected(environment_result.error());
    }
    const auto entry_result = entryFor(request);
    if (!entry_result) {
        return std::unexpected(entry_result.error());
    }
    const std::shared_ptr<Entry>& entry = *entry_result;
    const QMutexLocker call_lock(&entry->mutex);

    const auto open_result = ensureSessionOpen(*entry, request);
    if (!open_result) {
        return std::unexpected(open_result.error());
    }

    const auto call_result = entry->session->callTool(request.tool_name, request.arguments);
    if (!call_result) {
        return std::unexpected(closeAfterFailure(*entry, call_result.error()));
    }
    // Preserve the gateway transport wrapper {"result": ...}. This is NOT a complete JSON-RPC
    // message: the "jsonrpc" and "id" members belong to the session's own request/response
    // correlation and are consumed there, so re-synthesising them here would invent values
    // rather than report them.
    return QJsonObject{{QStringLiteral("result"), *call_result}};
}

std::expected<QVector<AiMcpToolDescriptor>, QString> AiMcpSessionPool::listTools(
    const AiMcpStdioCallRequest& request) {
    if (request.command.trimmed().isEmpty()) {
        return std::unexpected(QStringLiteral("MCP stdio command is empty"));
    }
    const auto environment_result = environmentCanBePooled(request);
    if (!environment_result) {
        return std::unexpected(environment_result.error());
    }
    const auto entry_result = entryFor(request);
    if (!entry_result) {
        return std::unexpected(entry_result.error());
    }
    const std::shared_ptr<Entry>& entry = *entry_result;
    const QMutexLocker call_lock(&entry->mutex);

    const auto open_result = ensureSessionOpen(*entry, request);
    if (!open_result) {
        return std::unexpected(open_result.error());
    }

    auto tools = entry->session->listTools();
    if (!tools) {
        return std::unexpected(closeAfterFailure(*entry, tools.error()));
    }
    return tools;
}

std::expected<void, QString> AiMcpSessionPool::closeAll() {
    return closeAllWithDeadline(QDeadlineTimer(kPoolCleanupTimeoutMs));
}

std::expected<void, QString> AiMcpSessionPool::closeAllWithDeadline(
    const QDeadlineTimer& deadline) {
    const auto lock = lockBeforeDeadline(m_mutex, deadline);
    if (!lock) {
        m_cleanup_blocked.store(true, std::memory_order_release);
        return std::unexpected(QStringLiteral("MCP pool remained busy through cleanup deadline"));
    }
    QStringList errors;
    for (auto iterator = m_sessions.begin(); iterator != m_sessions.end();) {
        // Keep the Entry alive through mutex unlock even when the map's reference is erased.
        const auto entry = iterator->second;
        // Serialize against any in-flight call on this entry before tearing its
        // session down, so we never destroy a session mid-call.
        const auto entry_lock = lockBeforeDeadline(entry->mutex, deadline);
        if (!entry_lock) {
            errors.append(QStringLiteral("MCP pool entry remained busy through cleanup deadline"));
            ++iterator;
            continue;
        }
        // Mark detached under the entry mutex so an in-flight caller that grabbed this handle
        // before we cleared the map cannot reopen a session on it after closeAll() returns.
        entry->detached = true;
        if (entry->session) {
            const auto closed = entry->session->closeWithDeadline(deadline);
            if (!closed) {
                errors.append(closed.error());
                ++iterator;
                continue;
            }
            entry->session.reset();
        }
        iterator = m_sessions.erase(iterator);
    }
    const bool unresolved = !errors.isEmpty();
    m_cleanup_blocked.store(unresolved, std::memory_order_release);
    if (unresolved) {
        return std::unexpected(errors.join(QStringLiteral("; ")));
    }
    return {};
}

int AiMcpSessionPool::pooledSlotCount() const {
    const QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_sessions.size());
}

QString AiMcpSessionPool::sessionKeyForTesting(const AiMcpStdioCallRequest& request) {
    return sessionKey(request);
}

}  // namespace sak::ai
