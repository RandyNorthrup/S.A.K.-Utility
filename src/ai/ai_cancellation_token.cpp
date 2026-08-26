// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"

#include <QJsonArray>

#include <algorithm>
#include <utility>  // std::move -- used directly below, so included directly

namespace sak::ai {

CancellationToken::CancellationToken(std::shared_ptr<State> state) : m_state(std::move(state)) {}

CancellationToken CancellationToken::createRoot(const QString& id) {
    auto state = std::make_shared<State>();
    state->id = id.trimmed().isEmpty() ? QStringLiteral("run") : id.trimmed();
    return CancellationToken(std::move(state));
}

bool CancellationToken::isValid() const noexcept {
    return static_cast<bool>(m_state);
}

QString CancellationToken::id() const {
    return m_state ? m_state->id : QString();
}

CancellationToken CancellationToken::createChild(const QString& id) const {
    if (!m_state) {
        return {};
    }

    auto child = std::make_shared<State>();
    child->parent = m_state;
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    // Drop children whose tokens are gone before appending. Without this a long-lived root that
    // creates a child per operation grows its vector forever, and every cancel(), childCount()
    // and toJson() walks every child it has ever had.
    m_state->children.removeIf([](const auto& child_ref) { return child_ref.expired(); });
    ++m_state->next_child_index;
    child->id = id.trimmed().isEmpty()
                    ? QStringLiteral("%1_child_%2").arg(m_state->id).arg(m_state->next_child_index)
                    : id.trimmed();
    if (m_state->cancelled.load(std::memory_order_relaxed)) {
        // The child is not shared yet, so its fields need no synchronization here.
        child->cancelled.store(true, std::memory_order_relaxed);
        child->reason = m_state->reason;
        child->cancelled_at_utc = m_state->cancelled_at_utc;
    }
    m_state->children.append(child);
    return CancellationToken(std::move(child));
}

int CancellationToken::childCount() const {
    if (!m_state) {
        return 0;
    }

    const std::lock_guard<std::mutex> lock(m_state->mutex);
    return static_cast<int>(std::ranges::count_if(m_state->children, [](const auto& child) {
        return !child.expired();
    }));
}

void CancellationToken::cancel(const QString& reason) const {
    if (!m_state) {
        return;
    }
    cancelState(m_state,
                reason.trimmed().isEmpty() ? QStringLiteral("cancelled") : reason.trimmed(),
                QDateTime::currentDateTimeUtc());
}

bool CancellationToken::isCancellationRequested() const noexcept {
    return m_state && m_state->cancelled.load(std::memory_order_acquire);
}

QString CancellationToken::cancelReason() const {
    if (!m_state) {
        return QString();
    }
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->reason;
}

QDateTime CancellationToken::cancelledAtUtc() const {
    if (!m_state) {
        return QDateTime();
    }
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->cancelled_at_utc;
}

QJsonObject CancellationToken::toJson() const {
    QJsonObject obj;
    if (!m_state) {
        obj[QStringLiteral("valid")] = false;
        return obj;
    }

    QVector<std::weak_ptr<State>> child_snapshot;
    {
        const std::lock_guard<std::mutex> lock(m_state->mutex);
        obj[QStringLiteral("valid")] = true;
        obj[QStringLiteral("id")] = m_state->id;
        obj[QStringLiteral("cancelled")] = m_state->cancelled.load(std::memory_order_relaxed);
        obj[QStringLiteral("reason")] = m_state->reason;
        obj[QStringLiteral("cancelled_at")] = m_state->cancelled_at_utc.toString(Qt::ISODateWithMs);
        child_snapshot = m_state->children;
    }

    // Read each child under its OWN lock (never two locks at once) to avoid deadlock.
    QJsonArray children;
    for (const auto& weak_child : child_snapshot) {
        if (const auto child = weak_child.lock()) {
            const std::lock_guard<std::mutex> child_lock(child->mutex);
            QJsonObject child_json;
            child_json[QStringLiteral("id")] = child->id;
            child_json[QStringLiteral("cancelled")] =
                child->cancelled.load(std::memory_order_relaxed);
            child_json[QStringLiteral("reason")] = child->reason;
            children.append(child_json);
        }
    }
    obj[QStringLiteral("children")] = children;
    return obj;
}

void CancellationToken::cancelState(const std::shared_ptr<State>& state,
                                    const QString& reason,
                                    const QDateTime& when_utc) {
    if (!state) {
        return;
    }

    QVector<std::weak_ptr<State>> children;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        state->reason = reason;
        state->cancelled_at_utc = when_utc;
        // Publish reason/time (written above, under the lock) before flagging cancelled,
        // so a lock-free reader that observes cancelled==true then locks to read them.
        state->cancelled.store(true, std::memory_order_release);
        children = state->children;
    }

    // Recurse without holding this node's lock so only one mutex is ever held at a time.
    for (const auto& weak_child : children) {
        cancelState(weak_child.lock(), reason, when_utc);
    }
}

}  // namespace sak::ai
