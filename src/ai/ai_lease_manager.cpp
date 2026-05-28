// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"

#include "sak/layout_constants.h"

#include <QMutexLocker>

namespace sak::ai {

namespace {
constexpr int kLeaseIdWidth = 4;
}

AiLeaseManager::AcquireResult AiLeaseManager::acquire(const QString& agent_id,
                                                      const QStringList& tool_scope,
                                                      const QString& risk_level,
                                                      bool exclusive) {
    QMutexLocker lock(&m_mutex);
    AcquireResult result;
    if (!m_active.isEmpty()) {
        const auto existing = *m_active.constBegin();
        result.granted = false;
        result.reason = QStringLiteral("Active mutating lease '%1' held by '%2' blocks new lease")
                            .arg(existing.lease_id, existing.agent_id);
        return result;
    }
    Lease lease;
    lease.lease_id =
        QStringLiteral("lease_%1").arg(m_next_id++, kLeaseIdWidth, kDecimalBase, QLatin1Char('0'));
    lease.agent_id = agent_id;
    lease.tool_scope = tool_scope;
    lease.risk_level = risk_level;
    lease.acquired_at_utc = QDateTime::currentDateTimeUtc();
    lease.exclusive = exclusive;
    m_active.insert(lease.lease_id, lease);
    result.granted = true;
    result.lease = lease;
    return result;
}

void AiLeaseManager::release(const QString& lease_id) {
    QMutexLocker lock(&m_mutex);
    m_active.remove(lease_id);
}

bool AiLeaseManager::hasActiveExclusive() const {
    QMutexLocker lock(&m_mutex);
    for (const auto& lease : m_active) {
        if (lease.exclusive) {
            return true;
        }
    }
    return false;
}

int AiLeaseManager::activeLeaseCount() const {
    QMutexLocker lock(&m_mutex);
    return m_active.size();
}

QStringList AiLeaseManager::activeLeaseIds() const {
    QMutexLocker lock(&m_mutex);
    QStringList ids;
    ids.reserve(m_active.size());
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it) {
        ids.append(it.key());
    }
    return ids;
}

}  // namespace sak::ai
