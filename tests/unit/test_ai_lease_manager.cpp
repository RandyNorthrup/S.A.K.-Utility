// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_lease_manager.cpp
/// @brief Unit tests for AiLeaseManager, focused on the mutating-lease TTL added
/// in harness Wave 2: an abandoned lease (lost release) must not wedge every
/// future mutating action -- it is reclaimed once its TTL elapses.

#include "sak/ai/ai_lease_manager.h"

#include <QtTest/QtTest>

namespace sak::ai {

class AiLeaseManagerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // The core "one mutating action at a time" invariant still holds, and a
    // released lease frees the slot for the next acquire.
    void singleLeaseBlocksSecondAndReleaseReacquires() {
        AiLeaseManager manager;
        QCOMPARE(manager.leaseTtlSeconds(), AiLeaseManager::kDefaultLeaseTtlSeconds);

        const auto first = manager.acquire(QStringLiteral("overseer"),
                                           {QStringLiteral("run_powershell")},
                                           QStringLiteral("system_change"),
                                           true);
        QVERIFY(first.granted);
        QVERIFY(manager.hasActiveExclusive());
        QCOMPARE(manager.activeLeaseCount(), 1);

        const auto blocked = manager.acquire(QStringLiteral("subagent"),
                                             {QStringLiteral("run_cmd")},
                                             QStringLiteral("system_change"),
                                             true);
        QVERIFY(!blocked.granted);
        QVERIFY(!blocked.reason.isEmpty());

        manager.release(first.lease.lease_id);
        QCOMPARE(manager.activeLeaseCount(), 0);
        QVERIFY(manager
                    .acquire(QStringLiteral("subagent"),
                             {QStringLiteral("run_cmd")},
                             QStringLiteral("system_change"),
                             true)
                    .granted);
    }

    // Every minted lease carries an expiry TTL seconds after acquisition.
    void acquireStampsExpiryFromTtl() {
        AiLeaseManager manager(120);
        const auto lease = manager.acquire(QStringLiteral("overseer"),
                                           {QStringLiteral("a")},
                                           QStringLiteral("system_change"),
                                           true);
        QVERIFY(lease.granted);
        QVERIFY(lease.lease.acquired_at_utc.isValid());
        QVERIFY(lease.lease.expires_at_utc.isValid());
        QCOMPARE(lease.lease.acquired_at_utc.secsTo(lease.lease.expires_at_utc),
                 static_cast<qint64>(120));
    }

    // reclaimExpired() is a no-op before expiry and reclaims exactly the lease at
    // or after its expiry instant.
    void reclaimExpiredHonoursExpiryBoundary() {
        AiLeaseManager manager(3600);
        const auto lease = manager.acquire(QStringLiteral("overseer"),
                                           {QStringLiteral("a")},
                                           QStringLiteral("system_change"),
                                           true);
        QVERIFY(lease.granted);

        QVERIFY(manager.reclaimExpired(lease.lease.acquired_at_utc).isEmpty());
        QCOMPARE(manager.activeLeaseCount(), 1);

        const QStringList reclaimed = manager.reclaimExpired(lease.lease.expires_at_utc.addSecs(1));
        QCOMPARE(reclaimed.size(), 1);
        QCOMPARE(reclaimed.first(), lease.lease.lease_id);
        QCOMPARE(manager.activeLeaseCount(), 0);
    }

    // An abandoned lease (holder crashed without releasing) blocks new leases
    // until its TTL elapses, after which the slot self-heals.
    void abandonedLeaseNoLongerWedgesAfterExpiry() {
        AiLeaseManager manager(3600);
        const auto held = manager.acquire(QStringLiteral("overseer"),
                                          {QStringLiteral("a")},
                                          QStringLiteral("system_change"),
                                          true);
        QVERIFY(held.granted);

        const auto blocked = manager.acquire(QStringLiteral("subagent"),
                                             {QStringLiteral("b")},
                                             QStringLiteral("system_change"),
                                             true);
        QVERIFY(!blocked.granted);
        QVERIFY(blocked.reclaimed_expired.isEmpty());

        manager.reclaimExpired(held.lease.expires_at_utc.addSecs(1));
        QVERIFY(manager
                    .acquire(QStringLiteral("subagent"),
                             {QStringLiteral("b")},
                             QStringLiteral("system_change"),
                             true)
                    .granted);
    }

    // End-to-end: acquire() uses the real clock, so a short-TTL lease left
    // unreleased is auto-reclaimed on the next acquire and reported.
    void acquireReclaimsExpiredUsingRealClock() {
        AiLeaseManager manager(1);
        const auto held = manager.acquire(QStringLiteral("overseer"),
                                          {QStringLiteral("a")},
                                          QStringLiteral("system_change"),
                                          true);
        QVERIFY(held.granted);

        QTest::qWait(1100);

        const auto next = manager.acquire(QStringLiteral("subagent"),
                                          {QStringLiteral("b")},
                                          QStringLiteral("system_change"),
                                          true);
        QVERIFY(next.granted);
        QCOMPARE(next.reclaimed_expired.size(), 1);
        QCOMPARE(next.reclaimed_expired.first(), held.lease.lease_id);
    }

    // A non-positive TTL is rejected in favour of the safe default rather than
    // creating leases that expire immediately (or never).
    void nonPositiveTtlFallsBackToDefault() {
        QCOMPARE(AiLeaseManager(0).leaseTtlSeconds(), AiLeaseManager::kDefaultLeaseTtlSeconds);
        QCOMPARE(AiLeaseManager(-5).leaseTtlSeconds(), AiLeaseManager::kDefaultLeaseTtlSeconds);
    }

    // The default TTL must exceed the longest legitimate lease-holding op plus margin, or a
    // still-running op would have its lease reclaimed and a second mutating op could start
    // concurrently. The true maximum is an app action (sak_app_action run), whose timeout is
    // clamped at kAppActionMaxTimeoutSeconds = 14400s (longer than the 7200s package cap the
    // stale rationale cited). Pin the invariant so the TTL can never regress below max op +
    // margin.
    void defaultTtlExceedsLongestMutatingOp() {
        constexpr qint64 kLongestLeaseHoldSeconds = 14'400;  // kAppActionMaxTimeoutSeconds
        constexpr qint64 kReleaseMarginSeconds = 600;
        QVERIFY(AiLeaseManager::kDefaultLeaseTtlSeconds >=
                kLongestLeaseHoldSeconds + kReleaseMarginSeconds);
    }
};

}  // namespace sak::ai

QTEST_GUILESS_MAIN(sak::ai::AiLeaseManagerTest)
#include "test_ai_lease_manager.moc"
