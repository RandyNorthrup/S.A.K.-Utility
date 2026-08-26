// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"

#include "sak/layout_constants.h"

#include <QMutexLocker>
#include <QRandomGenerator>

#include <algorithm>
#include <limits>

namespace sak::ai {

namespace {
constexpr int kLeaseIdWidth = 4;
// Unguessable half of the lease id. The counter alone is predictable, so any code holding the
// manager could release a lease it does not own by counting; a random token makes an id something
// only its holder can present.
//
// Because the token is what AUTHORIZES a release, it is a bearer credential and must come from a
// cryptographic source. QRandomGenerator::global() is a seeded PRNG, not a CSPRNG: an observer of
// a handful of ids could recover its state and predict the rest, which is exactly the "guess
// another agent's lease id" attack the token is here to stop. system() is the OS CSPRNG.
constexpr int kLeaseTokenBase = 16;
constexpr int kLeaseTokenWidth = 16;
constexpr qint64 kLeaseMillisPerSecond = 1000;

// A lease id is "lease_<counter>_<token>". Everything up to and including the counter is
// non-secret and identifies the lease; the trailing token is the credential.
constexpr int kLeaseIdLabelParts = 2;

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
        // The LABEL, never the live id. This reason is surfaced to the denied caller and, for a
        // model-issued tool call, is written into the tool result and the persisted transcript.
        // The id is a bearer credential: naming it here would hand the token that authorizes
        // release() to the one caller we just refused, and leave it sitting in the transcript.
        result.reason = QStringLiteral("Active mutating lease '%1' held by '%2' blocks new lease")
                            .arg(publicLabel(existing.lease_id), existing.agent_id);
        return result;
    }
    Lease lease;
    // Counter for readability in logs, random token so the id cannot be guessed or replayed.
    lease.lease_id = QStringLiteral("lease_%1_%2")
                         .arg(m_next_id++, kLeaseIdWidth, kDecimalBase, QLatin1Char('0'))
                         .arg(QRandomGenerator::system()->generate64(),
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
    const bool steady_available = m_clock.isValid();
    const qint64 elapsed_ms = steady_available ? m_clock.elapsed() : 0;
    for (auto it = m_active.begin(); it != m_active.end();) {
        const Lease& lease = it.value();
        // THE STEADY CLOCK IS THE AUTHORITY, and it is consulted ALONE whenever it can answer.
        // The previous form ORed the wall-clock arm in, which made the reclaim decision only as
        // trustworthy as the LEAST trustworthy clock: reclaimExpired() is public and takes a
        // caller-supplied now_utc, so a forward NTP correction, a user setting the date ahead, or
        // a caller simply passing a future instant reclaimed a lease whose TTL had NOT elapsed --
        // and a second mutating action then started beside the first, which is the concurrent
        // mutation this class exists to prevent. This arm measures real elapsed time only, so it
        // can never fire early, and it still closes the backward-jump wedge the wall arm cannot.
        const bool steady_usable = steady_available && lease.monotonic_expiry_ms > 0;
        bool expired = false;
        if (steady_usable) {
            expired = elapsed_ms >= lease.monotonic_expiry_ms;
        } else {
            // No monotonic deadline to consult (a lease not minted by this manager, or a steady
            // clock that never started). Fall back to wall time, which is better than treating
            // the lease as immortal -- a lost release would otherwise wedge mutations forever.
            // A null expiry is treated as non-expiring: never reclaim what cannot be reasoned
            // about.
            expired = lease.expires_at_utc.isValid() && lease.expires_at_utc <= now_utc;
        }
        if (expired) {
            reclaimed.append(it.key());
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
    return reclaimed;
}

QString AiLeaseManager::publicLabel(const QString& lease_id) {
    // "lease_<counter>_<token>" -> "lease_<counter>". Split from the LEFT and keep the first two
    // parts, so a token that itself contains an underscore cannot smuggle any of itself into the
    // label. Anything that is not in that shape carries no token to protect and is returned whole
    // -- returning a truncation of an unrecognised id would name no lease at all.
    const QStringList parts = lease_id.split(QLatin1Char('_'));
    if (parts.size() <= kLeaseIdLabelParts) {
        return lease_id;
    }
    return parts.at(0) + QLatin1Char('_') + parts.at(1);
}

bool AiLeaseManager::hasActiveExclusive() const {
    const QMutexLocker lock(&m_mutex);
    return std::ranges::any_of(m_active, [](const auto& lease) { return lease.exclusive; });
}

int AiLeaseManager::activeLeaseCount() const {
    const QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_active.size());
}

QStringList AiLeaseManager::activeLeaseLabels() const {
    const QMutexLocker lock(&m_mutex);
    QStringList labels;
    labels.reserve(m_active.size());
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it) {
        // Labels, not ids: this accessor answers "which leases are active", and the live id is
        // the credential that authorizes releasing them. A caller that legitimately holds a lease
        // already has its own id and does not need to read it back from here.
        labels.append(publicLabel(it.key()));
    }
    return labels;
}

}  // namespace sak::ai
