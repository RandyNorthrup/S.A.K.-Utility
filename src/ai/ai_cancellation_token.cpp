// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_cancellation_token.h"

#include <QJsonArray>

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
    child->id =
        id.trimmed().isEmpty()
            ? QStringLiteral("%1_child_%2").arg(m_state->id).arg(m_state->children.size() + 1)
            : id.trimmed();
    child->parent = m_state;
    if (m_state->cancelled) {
        child->cancelled = true;
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

    int count = 0;
    for (const auto& child : m_state->children) {
        if (!child.expired()) {
            ++count;
        }
    }
    return count;
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
    return m_state && m_state->cancelled;
}

QString CancellationToken::cancelReason() const {
    return m_state ? m_state->reason : QString();
}

QDateTime CancellationToken::cancelledAtUtc() const {
    return m_state ? m_state->cancelled_at_utc : QDateTime();
}

QJsonObject CancellationToken::toJson() const {
    QJsonObject obj;
    if (!m_state) {
        obj[QStringLiteral("valid")] = false;
        return obj;
    }

    obj[QStringLiteral("valid")] = true;
    obj[QStringLiteral("id")] = m_state->id;
    obj[QStringLiteral("cancelled")] = m_state->cancelled;
    obj[QStringLiteral("reason")] = m_state->reason;
    obj[QStringLiteral("cancelled_at")] = m_state->cancelled_at_utc.toString(Qt::ISODateWithMs);

    QJsonArray children;
    for (const auto& weak_child : m_state->children) {
        if (const auto child = weak_child.lock()) {
            QJsonObject child_json;
            child_json[QStringLiteral("id")] = child->id;
            child_json[QStringLiteral("cancelled")] = child->cancelled;
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
    if (!state || state->cancelled) {
        return;
    }

    state->cancelled = true;
    state->reason = reason;
    state->cancelled_at_utc = when_utc;

    const auto children = state->children;
    for (const auto& weak_child : children) {
        cancelState(weak_child.lock(), reason, when_utc);
    }
}

}  // namespace sak::ai
