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
        // The lease RECORD's caller-supplied fields. acquire() copies four of them in and the
        // file observed one, indirectly, only because the refusal reason interpolates it.
        QCOMPARE(first.lease.agent_id, QStringLiteral("overseer"));
        QCOMPARE(first.lease.tool_scope, QStringList{QStringLiteral("run_powershell")});
        QCOMPARE(first.lease.risk_level, QStringLiteral("system_change"));
        QVERIFY(first.lease.exclusive);
        QVERIFY(!first.lease.lease_id.isEmpty());

        const auto blocked = manager.acquire(QStringLiteral("subagent"),
                                             {QStringLiteral("run_cmd")},
                                             QStringLiteral("system_change"),
                                             true);
        QVERIFY(!blocked.granted);
        // Exactly one lease is active (first, agent "overseer"), so the blocked reason names it
        // verbatim. Pin the wording, the blocking lease id, and the holder id.
        QCOMPARE(blocked.reason,
                 QStringLiteral("Active mutating lease '%1' held by 'overseer' blocks new lease")
                     .arg(first.lease.lease_id));

        // release() returns bool, and that answer was DISCARDED here and in every other test in
        // the tree. It is not cosmetic: false means "no such lease was held", and the dispatcher's
        // entire fail-closed breach detection -- the lease_reclaimed_midop path that stamps a
        // mutation as failed because its exclusivity was violated mid-flight -- fires ONLY when
        // release() answers false. Nothing anywhere could observe that answer, so the guard rested
        // on an unasserted return. activeLeaseCount() sees the removal, not the report.
        QVERIFY2(manager.release(first.lease.lease_id), "releasing a held lease must report true");
        QCOMPARE(manager.activeLeaseCount(), 0);
        // ... and the FALSE side, in all three of its shapes.
        QVERIFY2(!manager.release(first.lease.lease_id),
                 "a second release of the same id must report false");
        QVERIFY2(!manager.release(QStringLiteral("no-such-lease")),
                 "releasing an unknown id must report false");
        QVERIFY2(!manager.release(QString()), "releasing a blank id must report false");
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
        // A lease carries TWO deadlines and only the wall one was asserted -- no test in the tree
        // ever read monotonic_expiry_ms. The steady arm of the reclaim is gated on
        // `monotonic_expiry_ms > 0`, so a lease left at the struct default {0} silently disables
        // its own backstop: the field's guard turns the omission into a fail-open.
        QVERIFY2(lease.lease.monotonic_expiry_ms > 0,
                 "a minted lease must carry a steady-clock deadline, or its backstop is disabled");
        // ... and the magnitude is knowable: the stamp is elapsed-since-construction plus the TTL
        // in milliseconds, and the manager was built moments ago, so it sits just above 120000.
        QVERIFY2(lease.lease.monotonic_expiry_ms >= 120'000 &&
                     lease.lease.monotonic_expiry_ms < 130'000,
                 qPrintable(QStringLiteral("monotonic_expiry_ms was %1, expected ~120000")
                                .arg(lease.lease.monotonic_expiry_ms)));
    }

    // hasActiveExclusive() must distinguish "some lease is EXCLUSIVE" from "some lease exists".
    // Every acquire() in this file passed exclusive=true, so the predicate was only ever asked
    // about a manager whose sole lease was exclusive, and Lease::exclusive was read on no result.
    // Non-exclusive mutating leases are a real production state: the dispatcher forwards the
    // policy's requires_exclusive_lease as this argument, and a package install is an allowed,
    // lease-requiring, NON-exclusive decision.
    void nonExclusiveLeaseIsNotAnExclusiveOne() {
        AiLeaseManager manager(3600);
        const auto shared = manager.acquire(QStringLiteral("installer"),
                                            {QStringLiteral("sak_package_manager")},
                                            QStringLiteral("package_install"),
                                            false);
        QVERIFY(shared.granted);
        QVERIFY2(!shared.lease.exclusive, "the exclusive flag must round-trip as given");
        QCOMPARE(manager.activeLeaseCount(), 1);
        QVERIFY2(!manager.hasActiveExclusive(),
                 "a non-exclusive lease must not answer hasActiveExclusive()");

        // Control: the same manager reports true once an exclusive lease exists.
        manager.release(shared.lease.lease_id);
        const auto exclusive = manager.acquire(QStringLiteral("overseer"),
                                               {QStringLiteral("run_powershell")},
                                               QStringLiteral("system_change"),
                                               true);
        QVERIFY(exclusive.granted);
        QVERIFY(exclusive.lease.exclusive);
        QVERIFY(manager.hasActiveExclusive());
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
        // THE BOUNDARY ITSELF. The test's comment claims it reclaims "exactly the lease at or
        // after its expiry instant", but the no-op sweep was taken an hour early and the
        // reclaiming sweep a full second late, so the `expires_at_utc <= now_utc` compare was
        // never touched from either side. One millisecond BEFORE must not reclaim...
        QVERIFY2(manager.reclaimExpired(lease.lease.expires_at_utc.addMSecs(-1)).isEmpty(),
                 "a sweep one millisecond before expiry must not reclaim");
        QCOMPARE(manager.activeLeaseCount(), 1);
        // ... and exactly AT the instant must, since the compare is inclusive.
        const QStringList at_instant = manager.reclaimExpired(lease.lease.expires_at_utc);
        QCOMPARE(at_instant.size(), 1);
        QCOMPARE(at_instant.first(), lease.lease.lease_id);
        QCOMPARE(manager.activeLeaseCount(), 0);

        // Re-mint and confirm the late sweep still works, so the original claim is kept too.
        const auto second = manager.acquire(QStringLiteral("overseer"),
                                            {QStringLiteral("a")},
                                            QStringLiteral("system_change"),
                                            true);
        QVERIFY(second.granted);
        const QStringList reclaimed =
            manager.reclaimExpired(second.lease.expires_at_utc.addSecs(1));
        QCOMPARE(reclaimed.size(), 1);
        QCOMPARE(reclaimed.first(), second.lease.lease_id);
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

        // A second manager, minted before the same wait, proves the STEADY-CLOCK arm at no extra
        // cost. reclaimExpiredLocked reclaims on `wall_expired || steady_expired`, and every sweep
        // in this file is driven by a now_utc at or past expires_at_utc -- so the wall arm alone
        // always sufficed and the steady arm was never the sole reason a lease was reclaimed. Its
        // documented purpose is the case no fixture created: a wall clock moved BACKWARD pushes
        // expires_at_utc out of reach and would wedge every future mutating action behind an
        // abandoned lease until the app restarts.
        AiLeaseManager steady_manager(1);
        const auto steady_held = steady_manager.acquire(QStringLiteral("overseer"),
                                                        {QStringLiteral("a")},
                                                        QStringLiteral("system_change"),
                                                        true);
        QVERIFY(steady_held.granted);

        QTest::qWait(1100);

        // Sweep with a now_utc far in the PAST, so the wall arm cannot possibly fire: only the
        // steady arm can reclaim here.
        const QStringList steady_reclaimed =
            steady_manager.reclaimExpired(steady_held.lease.acquired_at_utc.addYears(-1));
        QCOMPARE(steady_reclaimed.size(), 1);
        QCOMPARE(steady_reclaimed.first(), steady_held.lease.lease_id);
        QCOMPARE(steady_manager.activeLeaseCount(), 0);

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
        // The fallback was proved ONLY through the accessor. No fixture ever minted a lease from
        // a manager built with a non-positive TTL, so nothing tied the REPORTED TTL to the one
        // acquire() actually stamps -- and the test's own comment says the fallback exists "rather
        // than creating leases that expire immediately", which is exactly the half that went
        // unproved. Mint from such a manager and check the stamp itself.
        AiLeaseManager manager(0);
        const auto lease = manager.acquire(QStringLiteral("overseer"),
                                           {QStringLiteral("a")},
                                           QStringLiteral("system_change"),
                                           true);
        QVERIFY(lease.granted);
        QCOMPARE(lease.lease.acquired_at_utc.secsTo(lease.lease.expires_at_utc),
                 AiLeaseManager::kDefaultLeaseTtlSeconds);
        QVERIFY2(lease.lease.monotonic_expiry_ms > 0,
                 "a lease minted under the fallback TTL must still carry a steady deadline");
        // ... and it does NOT expire immediately: a sweep now must leave it alone.
        QVERIFY(manager.reclaimExpired(QDateTime::currentDateTimeUtc()).isEmpty());
        QCOMPARE(manager.activeLeaseCount(), 1);
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
        // The TTL is not only a floor. It is equally the CAP on how long a genuinely abandoned
        // lease wedges every mutating action before the slot self-heals, so a regression to days
        // or years satisfies the floor above while destroying the property the class exists for.
        // This was the only test constraining the constant at all, and it constrained one side;
        // every other comparison in the file measures leaseTtlSeconds() against the constant, so
        // both sides move together and pin nothing about its value. The shipped literal:
        QCOMPARE(AiLeaseManager::kDefaultLeaseTtlSeconds, static_cast<qint64>(16'200));
    }
};

}  // namespace sak::ai

QTEST_GUILESS_MAIN(sak::ai::AiLeaseManagerTest)
#include "test_ai_lease_manager.moc"
