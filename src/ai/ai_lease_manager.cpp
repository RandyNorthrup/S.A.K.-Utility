// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"

#include "sak/layout_constants.h"

#include <QMutexLocker>
#include <QRandomGenerator>
#include <QStringList>

namespace sak::ai {
namespace {

constexpr int kLeaseIdWidth = 4;
constexpr int kLeaseTokenBase = 16;
constexpr int kLeaseTokenWidth = 16;
constexpr int kLeaseIdLabelParts = 2;

}  // namespace

QMutex AiLeaseManager::s_mutex;
std::optional<AiLeaseManager::Lease> AiLeaseManager::s_active;
quint64 AiLeaseManager::s_next_id = 1;

AiLeaseManager::AcquireResult AiLeaseManager::acquire(const QString& agent_id) {
    const QMutexLocker lock(&s_mutex);
    AcquireResult result;
    if (s_active.has_value()) {
        const Lease& existing = *s_active;
        result.reason = QStringLiteral("Active mutating lease '%1' held by '%2' blocks new lease")
                            .arg(publicLabel(existing.lease_id), existing.agent_id);
        return result;
    }

    Lease lease;
    lease.lease_id = QStringLiteral("lease_%1_%2")
                         .arg(s_next_id++, kLeaseIdWidth, kDecimalBase, QLatin1Char('0'))
                         .arg(QRandomGenerator::system()->generate64(),
                              kLeaseTokenWidth,
                              kLeaseTokenBase,
                              QLatin1Char('0'));
    lease.agent_id = agent_id;
    s_active = lease;
    result.granted = true;
    result.lease = lease;
    return result;
}

bool AiLeaseManager::release(const QString& lease_id) {
    const QMutexLocker lock(&s_mutex);
    if (!s_active.has_value() || s_active->lease_id != lease_id) {
        return false;
    }
    s_active.reset();
    return true;
}

QString AiLeaseManager::publicLabel(const QString& lease_id) {
    const QStringList parts = lease_id.split(QLatin1Char('_'));
    if (parts.size() <= kLeaseIdLabelParts) {
        return lease_id;
    }
    return parts.at(0) + QLatin1Char('_') + parts.at(1);
}

}  // namespace sak::ai
