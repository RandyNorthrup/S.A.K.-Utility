// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

namespace sak::ai {

/// @brief Tracks active mutating-tool leases so the dispatcher can enforce
/// "only one mutating action runs at a time" across overseer + subagents.
///
/// Every lease carries a TTL: if a handler crashes, hangs, or otherwise never
/// calls release(), the abandoned lease would block ALL future mutating actions
/// forever. On acquire (and on an explicit sweep) leases older than the TTL are
/// reclaimed so the mutating path self-heals instead of wedging permanently.
class AiLeaseManager {
public:
    // A single mutating command tool call is bounded by its own process timeout
    // (about 2 minutes by default), so this TTL sits far above any legitimate
    // hold. It only fires when a release was genuinely lost, capping the wedge.
    static constexpr qint64 kDefaultLeaseTtlSeconds = 3600;  // 1 hour

    struct Lease {
        QString lease_id;
        QString agent_id;
        QStringList tool_scope;
        QString risk_level;
        QDateTime acquired_at_utc;
        // Wall-clock instant after which the lease is treated as abandoned and
        // may be reclaimed (acquired_at_utc + TTL).
        QDateTime expires_at_utc;
        bool exclusive{false};
    };

    struct AcquireResult {
        bool granted{false};
        Lease lease;
        QString reason;
        // Ids of stale leases auto-reclaimed while servicing this acquire, so the
        // caller can audit that an abandoned lease was cleared.
        QStringList reclaimed_expired;
    };

    explicit AiLeaseManager(qint64 ttl_seconds = kDefaultLeaseTtlSeconds);

    [[nodiscard]] AcquireResult acquire(const QString& agent_id,
                                        const QStringList& tool_scope,
                                        const QString& risk_level,
                                        bool exclusive);
    void release(const QString& lease_id);

    // Releases every lease whose expires_at_utc <= now_utc and returns the
    // reclaimed ids. acquire() calls this first; a periodic sweeper may call it
    // too so abandoned leases do not block new leases between acquires.
    QStringList reclaimExpired(const QDateTime& now_utc);

    [[nodiscard]] qint64 leaseTtlSeconds() const { return m_ttl_seconds; }
    [[nodiscard]] bool hasActiveExclusive() const;
    [[nodiscard]] int activeLeaseCount() const;
    [[nodiscard]] QStringList activeLeaseIds() const;

private:
    [[nodiscard]] QStringList reclaimExpiredLocked(const QDateTime& now_utc);

    mutable QMutex m_mutex;
    QHash<QString, Lease> m_active;
    quint64 m_next_id{1};
    qint64 m_ttl_seconds;
};

}  // namespace sak::ai
