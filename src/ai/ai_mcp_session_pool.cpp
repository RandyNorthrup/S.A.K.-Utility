// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_session_pool.h"

#include <QCryptographicHash>
#include <QStringList>

#include <algorithm>

namespace sak::ai {

namespace {

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
    std::sort(entries.begin(), entries.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& entry : entries) {
        const QByteArray bytes = entry.toUtf8();
        hash.addData(QByteArray::number(bytes.size()) + ':');
        hash.addData(bytes);
    }
    return request.command.trimmed() + QLatin1Char('\x1f') +
           QString::fromLatin1(hash.result().toHex()) + QLatin1Char('\x1f') +
           QString::number(request.timeout_ms);
}

AiMcpSessionConfig configFor(const AiMcpStdioCallRequest& request) {
    return {.command = request.command,
            .environment = request.environment,
            .request_timeout_ms = request.timeout_ms};
}

}  // namespace

AiMcpSessionPool::~AiMcpSessionPool() {
    closeAll();
}

std::shared_ptr<AiMcpSessionPool::Entry> AiMcpSessionPool::entryFor(
    const AiMcpStdioCallRequest& request) {
    const QString key = sessionKey(request);
    QMutexLocker lock(&m_mutex);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        it = m_sessions.emplace(key, std::make_shared<Entry>()).first;
    }
    // Hand back a shared handle: the caller locks entry->mutex OUTSIDE m_mutex, and
    // holding this reference keeps the Entry alive even if closeAll() clears the map
    // in between (the caller then just reopens a null session).
    return it->second;
}

bool AiMcpSessionPool::ensureSessionOpen(Entry& entry,
                                         const AiMcpStdioCallRequest& request,
                                         QString* error) {
    if (entry.session && entry.session->isOpen()) {
        return true;
    }
    entry.session = std::make_unique<AiMcpStdioSession>();
    if (!entry.session->open(configFor(request), error)) {
        entry.session.reset();
        return false;
    }
    return true;
}

QJsonObject AiMcpSessionPool::callTool(const AiMcpStdioCallRequest& request,
                                       QString* error_message) {
    if (request.command.trimmed().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("MCP stdio command is empty");
        }
        return {};
    }
    const std::shared_ptr<Entry> entry = entryFor(request);
    QMutexLocker call_lock(&entry->mutex);

    QString error;
    if (!ensureSessionOpen(*entry, request, &error)) {
        if (error_message) {
            *error_message = error;
        }
        return {};
    }

    const QJsonObject result =
        entry->session->callTool(request.tool_name, request.arguments, &error);
    if (!error.isEmpty()) {
        // The call failed -- the process may have died. Drop the session so the next
        // call reopens a fresh one rather than writing into a corpse.
        entry->session.reset();
        if (error_message) {
            *error_message = error;
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    // Re-wrap into the full JSON-RPC message shape the one-shot client returns, so
    // this is a drop-in transport for callers that read message["result"].
    return QJsonObject{{QStringLiteral("result"), result}};
}

QVector<AiMcpToolDescriptor> AiMcpSessionPool::listTools(const AiMcpStdioCallRequest& request,
                                                         QString* error_message) {
    if (request.command.trimmed().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("MCP stdio command is empty");
        }
        return {};
    }
    const std::shared_ptr<Entry> entry = entryFor(request);
    QMutexLocker call_lock(&entry->mutex);

    QString error;
    if (!ensureSessionOpen(*entry, request, &error)) {
        if (error_message) {
            *error_message = error;
        }
        return {};
    }

    const QVector<AiMcpToolDescriptor> tools = entry->session->listTools(&error);
    if (!error.isEmpty()) {
        entry->session.reset();
        if (error_message) {
            *error_message = error;
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return tools;
}

void AiMcpSessionPool::closeAll() {
    QMutexLocker lock(&m_mutex);
    for (auto& [key, entry] : m_sessions) {
        // Serialize against any in-flight call on this entry before tearing its
        // session down, so we never destroy a session mid-call.
        QMutexLocker entry_lock(&entry->mutex);
        entry->session.reset();
    }
    m_sessions.clear();
}

int AiMcpSessionPool::openSessionCount() const {
    QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_sessions.size());
}

QString AiMcpSessionPool::sessionKeyForTesting(const AiMcpStdioCallRequest& request) {
    return sessionKey(request);
}

}  // namespace sak::ai
