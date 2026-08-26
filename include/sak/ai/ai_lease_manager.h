// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QElapsedTimer>
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
    // The TTL must sit ABOVE the longest legitimate hold, or a still-running mutating op has
    // its lease reclaimed as "abandoned" and a second mutating op starts concurrently -- the
    // exact concurrent-mutation the lease exists to prevent. The longest single lease-holding
    // call is an app action (sak_app_action run), whose timeout is clamped at
    // kAppActionMaxTimeoutSeconds = 14400s (4 hours) -- longer than the 7200s package-tool cap
    // -- so the TTL is derived from that true maximum plus release/overhead margin. It only
    // fires when a release was genuinely lost (a crashed/hung handler), capping the wedge.
    static constexpr qint64 kDefaultLeaseTtlSeconds = 16'200;  // 4.5 hours (> 14400s max op)

    struct Lease {
        QString lease_id;
        QString agent_id;
        QStringList tool_scope;
        QString risk_level;
        QDateTime acquired_at_utc;
        // Wall-clock instant after which the lease is treated as abandoned and
        // may be reclaimed (acquired_at_utc + TTL).
        QDateTime expires_at_utc;
        // The same deadline on the manager's own STEADY clock, in milliseconds since the manager
        // was constructed. expires_at_utc is wall-clock and therefore adjustable: a clock moved
        // backward pushes it out of reach and would wedge every future mutating action behind an
        // abandoned lease forever. This one only ever measures real elapsed time, so it can close
        // that wedge without ever reclaiming a lease early.
        qint64 monotonic_expiry_ms{0};
        bool exclusive{false};
    };

    struct AcquireResult {
        bool granted{false};
        Lease lease;
        // Why the lease was refused. Names the blocking lease by publicLabel(), never by its live
        // id: this text reaches the refused caller and, for a model-issued tool call, the tool
        // result and the persisted transcript.
        QString reason;
        // Ids of stale leases auto-reclaimed while servicing this acquire, so the caller can audit
        // that an abandoned lease was cleared. These are full ids on purpose: a reclaimed lease is
        // already gone from the table, so its id no longer authorizes anything (release() answers
        // false for it), and the exact id is what makes the audit trail match the acquire that
        // minted it.
        QStringList reclaimed_expired;
    };

    explicit AiLeaseManager(qint64 ttl_seconds = kDefaultLeaseTtlSeconds);

    [[nodiscard]] AcquireResult acquire(const QString& agent_id,
                                        const QStringList& tool_scope,
                                        const QString& risk_level,
                                        bool exclusive);
    // Releases @p lease_id. Returns false when no such lease was held -- a wrong, blank or stale
    // id (one the TTL sweep already reclaimed) must be distinguishable from a real release rather
    // than reported as one. Lease ids carry an unguessable token so a release cannot be aimed at
    // another agent's lease by counting.
    bool release(const QString& lease_id);

    // Releases every lease whose TTL has genuinely elapsed and returns the reclaimed ids.
    // acquire() calls this first; a periodic sweeper may call it too so abandoned leases do not
    // block new leases between acquires.
    //
    // @p now_utc is only consulted for a lease that carries no monotonic deadline. The manager's
    // OWN steady clock decides for every lease it minted, because now_utc is caller-supplied wall
    // time: a forward NTP correction, a user setting the date ahead, or simply a caller passing a
    // future instant would otherwise reclaim a lease that is still running and let a second
    // mutating action start beside it -- the exact concurrency this class exists to prevent.
    QStringList reclaimExpired(const QDateTime& now_utc);

    // The non-secret half of a lease id, safe to put in a message, a log line or a tool result.
    // A lease id is a BEARER CREDENTIAL: whoever holds it can release() that lease. The counter
    // prefix identifies which lease is meant without disclosing the token that authorizes it.
    // Returns the whole string when it carries no token (nothing to protect).
    [[nodiscard]] static QString publicLabel(const QString& lease_id);

    [[nodiscard]] qint64 leaseTtlSeconds() const { return m_ttl_seconds; }
    [[nodiscard]] bool hasActiveExclusive() const;
    [[nodiscard]] int activeLeaseCount() const;
    // Non-secret publicLabel() values for every active lease -- deliberately NOT the live ids.
    // This is a diagnostic accessor, and handing a caller the bearer token of a lease it does not
    // hold would let it release another agent's lease. A holder already has its own id.
    [[nodiscard]] QStringList activeLeaseLabels() const;

private:
    [[nodiscard]] QStringList reclaimExpiredLocked(const QDateTime& now_utc);

    mutable QMutex m_mutex;
    QHash<QString, Lease> m_active;
    quint64 m_next_id{1};
    qint64 m_ttl_seconds;
    // Steady clock started at construction; the source of Lease::monotonic_expiry_ms.
    QElapsedTimer m_clock;
};

}  // namespace sak::ai
