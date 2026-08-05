// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_flasher_policy.cpp
/// @brief Unit tests for the pure Image Flasher setting decisions.
///
/// These are the seams behind two settings that used to be collected and then
/// ignored: the large-drive warning threshold, and the concurrent-write ceiling.
/// Each test states the user-visible consequence of the answer, because the
/// failure mode these functions guard is a silently skipped safety prompt or a
/// run that never starts a queued drive.

#include "sak/flasher_policy.h"

#include <QtTest/QtTest>

#include <limits>

using namespace sak;

class FlasherPolicyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- largeDriveThresholdBytes ---

    void thresholdConvertsGbToBytes() {
        QCOMPARE(largeDriveThresholdBytes(1), kBytesPerGB);
        QCOMPARE(largeDriveThresholdBytes(128), 128 * kBytesPerGB);
        QCOMPARE(largeDriveThresholdBytes(2048), 2048 * kBytesPerGB);
    }

    void thresholdRejectsNonPositive() {
        // -1 means "unusable", NOT "no threshold": the caller must warn rather than
        // treat the check as satisfied.
        constexpr int64_t kUnusable = -1;
        QCOMPARE(largeDriveThresholdBytes(0), kUnusable);
        QCOMPARE(largeDriveThresholdBytes(-1), kUnusable);
        QCOMPARE(largeDriveThresholdBytes(std::numeric_limits<int>::min()), kUnusable);
    }

    void thresholdCannotOverflowForAnyIntInput() {
        // The widest input still converts to a positive byte count, which is why
        // there is no overflow branch to test: INT_MAX GB is a quarter of int64's
        // range. If this ever stops holding, the conversion needs a guard again.
        const int64_t widest = largeDriveThresholdBytes(std::numeric_limits<int>::max());
        QVERIFY(widest > 0);
        QCOMPARE(widest / kBytesPerGB, static_cast<int64_t>(std::numeric_limits<int>::max()));
    }

    // --- driveExceedsLargeThreshold ---

    void driveBelowThresholdDoesNotWarn() {
        const int64_t threshold = largeDriveThresholdBytes(128);
        QVERIFY(!driveExceedsLargeThreshold(16 * kBytesPerGB, threshold));
        QVERIFY(!driveExceedsLargeThreshold(0, threshold));
    }

    void driveExactlyAtThresholdDoesNotWarn() {
        // The threshold is the largest size still considered ordinary, so a drive
        // exactly at it is not "over" it. Off-by-one here would warn on every
        // 128 GB stick with the default setting.
        const int64_t threshold = largeDriveThresholdBytes(128);
        QVERIFY(!driveExceedsLargeThreshold(threshold, threshold));
        QVERIFY(driveExceedsLargeThreshold(threshold + 1, threshold));
    }

    void driveAboveThresholdWarns() {
        const int64_t threshold = largeDriveThresholdBytes(128);
        QVERIFY(driveExceedsLargeThreshold(2000 * kBytesPerGB, threshold));
    }

    void unknownCapacityWarns() {
        // A capacity that could not be read is exactly what this warning exists for.
        // Passing it silently is the failure being prevented.
        const int64_t threshold = largeDriveThresholdBytes(128);
        QVERIFY(driveExceedsLargeThreshold(-1, threshold));
    }

    void unusableThresholdWarnsAboutEveryDrive() {
        // Fail closed: an unusable threshold must not delete the prompt the user
        // asked for. Even a 1-byte drive warns.
        QVERIFY(driveExceedsLargeThreshold(1, -1));
        QVERIFY(driveExceedsLargeThreshold(64 * kBytesPerGB, -1));
        QVERIFY(driveExceedsLargeThreshold(-1, -1));
    }

    // --- startableWorkerCount ---

    void nothingQueuedStartsNothing() {
        QCOMPARE(startableWorkerCount(4, 0, 0), 0);
        QCOMPARE(startableWorkerCount(4, 2, 0), 0);
        QCOMPARE(startableWorkerCount(4, 0, -1), 0);
    }

    void serialCeilingStartsOneAtATime() {
        // The shipped default. Three queued drives with nothing running starts one.
        QCOMPARE(startableWorkerCount(1, 0, 3), 1);
        // ...and nothing more until that one finishes.
        QCOMPARE(startableWorkerCount(1, 1, 2), 0);
    }

    void ceilingFillsAllFreeSlots() {
        QCOMPARE(startableWorkerCount(4, 0, 3), 3);
        QCOMPARE(startableWorkerCount(4, 1, 3), 3);
        QCOMPARE(startableWorkerCount(4, 2, 3), 2);
        QCOMPARE(startableWorkerCount(4, 4, 3), 0);
    }

    void neverStartsMoreThanAreQueued() { QCOMPARE(startableWorkerCount(16, 0, 2), 2); }

    void runningAtOrOverCeilingStartsNothing() {
        QCOMPARE(startableWorkerCount(2, 2, 5), 0);
        // Over the ceiling (a ceiling lowered mid-run) must not go negative.
        QCOMPARE(startableWorkerCount(2, 9, 5), 0);
    }

    void ceilingBelowOneStillWrites() {
        // A ceiling of zero would otherwise stall the run forever: nothing could
        // start, so nothing could ever finish and free a slot.
        QCOMPARE(startableWorkerCount(0, 0, 3), 1);
        QCOMPARE(startableWorkerCount(-5, 0, 3), 1);
        QCOMPARE(startableWorkerCount(0, 1, 3), 0);
    }

    void negativeRunningCountIsTreatedAsIdle() {
        // Defensive: a corrupted running count must not manufacture extra slots.
        QCOMPARE(startableWorkerCount(2, -3, 5), 2);
    }
};

QTEST_MAIN(FlasherPolicyTests)
#include "test_flasher_policy.moc"
