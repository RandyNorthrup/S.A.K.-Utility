// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_apfs_free_queue_guard.cpp
/// @brief F25: the free-queue reserved-region guard, exercised from BOTH sides.
///
/// An APFS free-queue record schedules ghost blocks for reuse, so a record covering live
/// container metadata hands the allocator the checkpoint ring or the internal pool and the next
/// allocation destroys them. The guard that refuses such a record is easy to write and easy to
/// write WRONG: F25 was implemented, reverted, and re-written incorrectly twice more, each time
/// as a FALSE-CLOSE that rejected a legitimate commit, and each time caught only by a full build
/// plus the container round-trip tests. This file pins both directions so the next attempt is
/// caught by a unit test in seconds instead:
///
///   ACCEPT  -- the records a correct commit really produces (a rotation's ghost slot inside the
///              internal pool, a grow's freed OLD pool location) must be allowed;
///   REFUSE  -- a record over the anchor, either checkpoint ring, or (for the main-device queue)
///              the pool and its bitmap ring must be refused with a blocker naming the region.
///
/// The region SET is the caller's job -- the writer resolves it to post-commit geometry and
/// scopes it per queue -- so these tests supply regions directly and pin the overlap arithmetic
/// and the refusal, which is what this seam owns.

#include "sak/apfs_free_queue_guard.h"

#include <QStringList>
#include <QtTest/QtTest>

#include <limits>

using sak::apfs::FreeQueueRun;
using sak::apfs::freeQueueRunsAvoidReserved;
using sak::apfs::ReservedRegion;

namespace {

// The generated container's layout, the one every round-trip test runs against:
// block 0 anchor, descriptor ring [1,9), data ring [9,169), bitmap ring [169,185),
// internal pool [185,191).
QVector<ReservedRegion> mainDeviceRegions() {
    return {{0, 1, QStringLiteral("nx_superblock anchor")},
            {1, 8, QStringLiteral("checkpoint descriptor ring")},
            {9, 160, QStringLiteral("checkpoint data ring")},
            {185, 6, QStringLiteral("internal pool")},
            {169, 16, QStringLiteral("internal-pool bitmap ring")}};
}

// The internal-pool queue's scope: the pool and its bitmap ring are that queue's own DOMAIN, so
// only the anchor and the two rings are reserved against it.
QVector<ReservedRegion> internalPoolRegions() {
    return {{0, 1, QStringLiteral("nx_superblock anchor")},
            {1, 8, QStringLiteral("checkpoint descriptor ring")},
            {9, 160, QStringLiteral("checkpoint data ring")}};
}

}  // namespace

class ApfsFreeQueueGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void acceptsTheRecordsARealCommitProduces();
    void refusesRunsOverEachReservedRegion();
    void refusesEveryFormOfOverlap();
    void failsClosedOnAddressSpaceOverflow();
    void emptyInputsAreAccepted();
};

// ACCEPT side. Each of these is a record a correct commit really emits, and each corresponds to
// one of the three ways F25 was previously written wrong.
void ApfsFreeQueueGuardTests::acceptsTheRecordsARealCommitProduces() {
    QStringList blockers;

    // A rotation commit's ghost slot sits INSIDE the internal pool. Reserving the pool against
    // the internal-pool queue rejected every ordinary commit -- mistake (2).
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{185, 2}}, QStringLiteral("internal-pool"), internalPoolRegions(), &blockers),
             qPrintable(blockers.join(QLatin1Char('|'))));

    // The overflow tier also frees the old boundary-chunk bitmap, which lives in the pool's
    // BITMAP RING -- likewise the internal-pool queue's own domain.
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{172, 1}}, QStringLiteral("internal-pool"), internalPoolRegions(), &blockers),
             qPrintable(blockers.join(QLatin1Char('|'))));

    // A chunk-adding grow re-homes the pool and queues its OLD location. Under POST-commit
    // geometry (pool now at 4000) those blocks are free, and must be accepted -- mistake (1),
    // the one that got the first implementation reverted.
    const QVector<ReservedRegion> afterGrow = {
        {0, 1, QStringLiteral("nx_superblock anchor")},
        {1, 8, QStringLiteral("checkpoint descriptor ring")},
        {9, 160, QStringLiteral("checkpoint data ring")},
        {4000, 6, QStringLiteral("internal pool")},
        {169, 16, QStringLiteral("internal-pool bitmap ring")}};
    QVERIFY2(
        freeQueueRunsAvoidReserved({{185, 6}}, QStringLiteral("main-device"), afterGrow, &blockers),
        qPrintable(blockers.join(QLatin1Char('|'))));

    // An ordinary main-device record in the general free region.
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{5000, 64}}, QStringLiteral("main-device"), mainDeviceRegions(), &blockers),
             qPrintable(blockers.join(QLatin1Char('|'))));

    // Adjacency is not overlap. [191,193) begins exactly where the internal pool ends, and a
    // real commit does emit records there. A guard written with <= instead of < rejects it.
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{191, 2}}, QStringLiteral("main-device"), mainDeviceRegions(), &blockers),
             qPrintable(blockers.join(QLatin1Char('|'))));

    // The OTHER side of the same boundary needs an ISOLATED region: the generated layout is
    // packed solid from block 0 to the end of the pool (anchor, then [1,9), [9,169), [169,185),
    // [185,191)), so there is no free block immediately below any region to probe with -- a
    // fixture that tried [165,169) would be testing the data ring's interior, not adjacency.
    const QVector<ReservedRegion> lone = {{500, 10, QStringLiteral("lone region")}};
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{496, 4}, {510, 4}}, QStringLiteral("main-device"), lone, &blockers),
             qPrintable(blockers.join(QLatin1Char('|'))));

    // Nothing above may have produced a blocker: an accepted run that still appends one would
    // trip advanceCheckpoint's own "blockers is not empty" gate later in the commit.
    QVERIFY2(blockers.isEmpty(), qPrintable(blockers.join(QLatin1Char('|'))));
}

// REFUSE side, one region at a time, so no single region carries the whole test. Each assertion
// pins the region NAME too -- a guard that refuses for the wrong reason is a different bug, and
// the message is what the operator is handed.
void ApfsFreeQueueGuardTests::refusesRunsOverEachReservedRegion() {
    struct Probe {
        FreeQueueRun run;
        const char* queue;
        QVector<ReservedRegion> regions;
        QString expectRegion;
    };
    const QVector<Probe> probes = {
        {{0, 1}, "main-device", mainDeviceRegions(), QStringLiteral("nx_superblock anchor")},
        {{4, 2}, "main-device", mainDeviceRegions(), QStringLiteral("checkpoint descriptor ring")},
        {{100, 8}, "main-device", mainDeviceRegions(), QStringLiteral("checkpoint data ring")},
        {{186, 1}, "main-device", mainDeviceRegions(), QStringLiteral("internal pool")},
        {{170, 4}, "main-device", mainDeviceRegions(), QStringLiteral("internal-pool bitmap ring")},
        // The anchor and BOTH rings are reserved against the internal-pool queue as well: its
        // domain is the pool, never the container's own checkpoint state.
        {{0, 1}, "internal-pool", internalPoolRegions(), QStringLiteral("nx_superblock anchor")},
        {{2, 1},
         "internal-pool",
         internalPoolRegions(),
         QStringLiteral("checkpoint descriptor ring")},
        {{9, 1}, "internal-pool", internalPoolRegions(), QStringLiteral("checkpoint data ring")},
    };

    for (const Probe& probe : probes) {
        QStringList blockers;
        const QString queue = QString::fromLatin1(probe.queue);
        QVERIFY2(!freeQueueRunsAvoidReserved({probe.run}, queue, probe.regions, &blockers),
                 qPrintable(QStringLiteral("run [%1,+%2) on the %3 queue was ACCEPTED over %4")
                                .arg(probe.run.paddr)
                                .arg(probe.run.length)
                                .arg(queue, probe.expectRegion)));
        QCOMPARE(blockers.size(), 1);
        QVERIFY2(blockers.first().contains(probe.expectRegion),
                 qPrintable(blockers.first() + QStringLiteral("  (expected region: ") +
                            probe.expectRegion + QLatin1Char(')')));
        // The message names the offending queue, so an operator can tell which tree to look at.
        QVERIFY2(blockers.first().contains(queue), qPrintable(blockers.first()));
    }
}

// Every geometric way a run can meet a region. A guard that only compares start addresses, or
// only tests containment, passes some of these and fails others.
void ApfsFreeQueueGuardTests::refusesEveryFormOfOverlap() {
    const QVector<ReservedRegion> pool = {{185, 6, QStringLiteral("internal pool")}};
    const QVector<QPair<FreeQueueRun, const char*>> cases = {
        {{185, 6}, "exactly the region"},
        {{186, 2}, "strictly inside"},
        {{183, 4}, "straddles the start"},
        {{189, 4}, "straddles the end"},
        {{180, 20}, "encloses the region"},
        {{190, 1}, "the region's last block"},
        {{185, 1}, "the region's first block"},
    };
    for (const auto& [run, shape] : cases) {
        QStringList blockers;
        QVERIFY2(!freeQueueRunsAvoidReserved({run}, QStringLiteral("main-device"), pool, &blockers),
                 shape);
        QCOMPARE(blockers.size(), 1);
    }

    // ...and a refusal anywhere in a MULTI-run queue refuses the whole queue: the first two runs
    // are legitimate, the third is not. A guard that returned the last verdict, or that only
    // examined runs[0], would accept this.
    QStringList blockers;
    QVERIFY(!freeQueueRunsAvoidReserved(
        {{5000, 4}, {6000, 4}, {186, 1}}, QStringLiteral("main-device"), pool, &blockers));
    QCOMPARE(blockers.size(), 1);
    QVERIFY2(blockers.first().contains(QStringLiteral("[186,187)")), qPrintable(blockers.first()));
}

// A run whose paddr+length wraps must FAIL CLOSED. Computing the end anyway yields a value BELOW
// the start, which makes every overlap test false and turns the guard into a no-op for exactly
// the most malformed record it will ever see.
void ApfsFreeQueueGuardTests::failsClosedOnAddressSpaceOverflow() {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    QStringList blockers;
    QVERIFY(!freeQueueRunsAvoidReserved(
        {{kMax - 1, 8}}, QStringLiteral("main-device"), mainDeviceRegions(), &blockers));
    QCOMPARE(blockers.size(), 1);
    QVERIFY2(blockers.first().contains(QStringLiteral("overflows the address space")),
             qPrintable(blockers.first()));

    // The largest run that does NOT wrap is still evaluated normally, so the guard is a real
    // boundary and not a blanket refusal of large addresses.
    QStringList ok;
    QVERIFY2(freeQueueRunsAvoidReserved(
                 {{kMax - 8, 8}}, QStringLiteral("main-device"), mainDeviceRegions(), &ok),
             qPrintable(ok.join(QLatin1Char('|'))));
    QVERIFY(ok.isEmpty());

    // A malformed REGION is refused for the same reason, rather than silently skipped.
    QStringList regionBlockers;
    QVERIFY(!freeQueueRunsAvoidReserved({{10, 1}},
                                        QStringLiteral("main-device"),
                                        {{kMax - 1, 8, QStringLiteral("bogus region")}},
                                        &regionBlockers));
    QCOMPARE(regionBlockers.size(), 1);
    QVERIFY2(regionBlockers.first().contains(QStringLiteral("overflows the address space")),
             qPrintable(regionBlockers.first()));
}

void ApfsFreeQueueGuardTests::emptyInputsAreAccepted() {
    QStringList blockers;
    // No records: nothing to release. Most commits are this case.
    QVERIFY(freeQueueRunsAvoidReserved(
        {}, QStringLiteral("main-device"), mainDeviceRegions(), &blockers));
    // A zero-length record covers nothing; the tree builders drop it, and it is not this
    // guard's business to refuse it. Placed AT a reserved block so the acceptance is decided by
    // the length and nothing else.
    QVERIFY(freeQueueRunsAvoidReserved(
        {{185, 0}}, QStringLiteral("main-device"), mainDeviceRegions(), &blockers));
    // A zero-BLOCK region cannot be overlapped -- the writer strips these, but the seam must not
    // depend on the caller having done so.
    QVERIFY(freeQueueRunsAvoidReserved({{185, 4}},
                                       QStringLiteral("main-device"),
                                       {{185, 0, QStringLiteral("empty region")}},
                                       &blockers));
    QVERIFY2(blockers.isEmpty(), qPrintable(blockers.join(QLatin1Char('|'))));
}

QTEST_APPLESS_MAIN(ApfsFreeQueueGuardTests)
#include "test_apfs_free_queue_guard.moc"
