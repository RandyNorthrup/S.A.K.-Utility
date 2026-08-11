// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"

#include "sak/layout_constants.h"

#include <QMutexLocker>
#include <QRandomGenerator>

#include <limits>

namespace sak::ai {

namespace {
constexpr int kLeaseIdWidth = 4;
// Unguessable half of the lease id. The counter alone is predictable, so any code holding the
// manager could release a lease it does not own by counting; a random token makes an id something
// only its holder can present.
constexpr int kLeaseTokenBase = 16;
constexpr int kLeaseTokenWidth = 16;
constexpr qint64 kLeaseMillisPerSecond = 1000;

// TTL in milliseconds, saturated rather than overflowed: the ceiling is far beyond any real hold
// and a wrapped deadline would read as "already expired" and reclaim a live lease immediately.
qint64 leaseTtlMillis(qint64 ttl_seconds) {
    constexpr qint64 kMaxTtlMs = std::numeric_limits<qint64>::max() / 2;
    return ttl_seconds > kMaxTtlMs / kLeaseMillisPerSecond ? kMaxTtlMs
                                                           : ttl_seconds * kLeaseMillisPerSecond;
}
}  // namespace

AiLeaseManager::AiLeaseManager(qint64 ttl_seconds)
    : m_ttl_seconds(ttl_seconds > 0 ? ttl_seconds : kDefaultLeaseTtlSeconds) {
    m_clock.start();
}

AiLeaseManager::AcquireResult AiLeaseManager::acquire(const QString& agent_id,
                                                      const QStringList& tool_scope,
                                                      const QString& risk_level,
                                                      bool exclusive) {
    const QMutexLocker lock(&m_mutex);
    AcquireResult result;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    // Clear any abandoned lease first so a lost release() cannot wedge new leases.
    result.reclaimed_expired = reclaimExpiredLocked(now);
    if (!m_active.isEmpty()) {
        const auto existing = *m_active.constBegin();
        result.granted = false;
        result.reason = QStringLiteral("Active mutating lease '%1' held by '%2' blocks new lease")
                            .arg(existing.lease_id, existing.agent_id);
        return result;
    }
    Lease lease;
    // Counter for readability in logs, random token so the id cannot be guessed or replayed.
    lease.lease_id = QStringLiteral("lease_%1_%2")
                         .arg(m_next_id++, kLeaseIdWidth, kDecimalBase, QLatin1Char('0'))
                         .arg(QRandomGenerator::global()->generate64(),
                              kLeaseTokenWidth,
                              kLeaseTokenBase,
                              QLatin1Char('0'));
    lease.agent_id = agent_id;
    lease.tool_scope = tool_scope;
    lease.risk_level = risk_level;
    lease.acquired_at_utc = now;
    lease.expires_at_utc = now.addSecs(m_ttl_seconds);
    lease.monotonic_expiry_ms = m_clock.elapsed() + leaseTtlMillis(m_ttl_seconds);
    lease.exclusive = exclusive;
    m_active.insert(lease.lease_id, lease);
    result.granted = true;
    result.lease = lease;
    return result;
}

bool AiLeaseManager::release(const QString& lease_id) {
    const QMutexLocker lock(&m_mutex);
    // Report whether a lease was actually held: a release for an id this manager does not hold
    // (already reclaimed as expired, or simply wrong) is not the same event as a real release and
    // must not be reported as one.
    // QHash::remove already answers the question as a bool in Qt 6; comparing it against 0
    // mixes bool with int, which this build treats as an error.
    return m_active.remove(lease_id);
}

QStringList AiLeaseManager::reclaimExpired(const QDateTime& now_utc) {
    const QMutexLocker lock(&m_mutex);
    return reclaimExpiredLocked(now_utc);
}

QStringList AiLeaseManager::reclaimExpiredLocked(const QDateTime& now_utc) {
    QStringList reclaimed;
    const qint64 elapsed_ms = m_clock.isValid() ? m_clock.elapsed() : 0;
    for (auto it = m_active.begin(); it != m_active.end();) {
        const Lease& lease = it.value();
        // A null expiry (should not happen for leases minted here) is treated as
        // non-expiring so we never reclaim a lease we cannot reason about.
        const bool wall_expired = lease.expires_at_utc.isValid() && lease.expires_at_utc <= now_utc;
        // Steady-clock backstop: a wall clock moved BACKWARD (NTP correction, a user changing the
        // date) pushes expires_at_utc out of reach and would wedge every future mutating action
        // behind an abandoned lease until the app restarts. This branch measures real elapsed
        // time only, so it can never reclaim a lease before its TTL has genuinely passed.
        const bool steady_expired = lease.monotonic_expiry_ms > 0 &&
                                    elapsed_ms >= lease.monotonic_expiry_ms;
        if (wall_expired || steady_expired) {
            reclaimed.append(it.key());
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
    return reclaimed;
}

bool AiLeaseManager::hasActiveExclusive() const {
    const QMutexLocker lock(&m_mutex);
    for (const auto& lease : m_active) {
        if (lease.exclusive) {
            return true;
        }
    }
    return false;
}

int AiLeaseManager::activeLeaseCount() const {
    const QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_active.size());
}

QStringList AiLeaseManager::activeLeaseIds() const {
    const QMutexLocker lock(&m_mutex);
    QStringList ids;
    ids.reserve(m_active.size());
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it) {
        ids.append(it.key());
    }
    return ids;
}

}  // namespace sak::ai
