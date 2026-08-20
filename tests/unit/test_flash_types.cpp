// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_flash_types.cpp
/// @brief Unit tests for FlashProgress and FlashResult value types (TST-02)

#include "sak/flash_coordinator.h"

#include <QtTest/QtTest>

using namespace sak;

class FlashTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- FlashProgress tests ---

    void progressZeroTotalReturnsZero() {
        FlashProgress p{};
        p.totalBytes = 0;
        p.bytesWritten = 0;
        QCOMPARE(p.getOverallProgress(), 0.0);
    }

    void progressHalfway() {
        FlashProgress p{};
        p.totalBytes = 1000;
        p.bytesWritten = 500;
        QCOMPARE(p.getOverallProgress(), 50.0);
    }

    void progressComplete() {
        FlashProgress p{};
        p.totalBytes = 2048;
        p.bytesWritten = 2048;
        QCOMPARE(p.getOverallProgress(), 100.0);
    }

    void progressFractional() {
        FlashProgress p{};
        p.totalBytes = 3;
        p.bytesWritten = 1;
        QCOMPARE(p.getOverallProgress(), (1.0 / 3.0) * kPercentMaxF);
    }

    void progressLargeValues() {
        FlashProgress p{};
        p.totalBytes = Q_INT64_C(10'000'000'000);  // 10 GB
        p.bytesWritten = Q_INT64_C(7'500'000'000);
        QCOMPARE(p.getOverallProgress(), 75.0);
    }

    // --- FlashResult tests ---

    void resultNoErrorsWhenEmpty() {
        FlashResult r{};
        r.success = true;
        QVERIFY(!r.hasErrors());
        QCOMPARE(r.totalDrives(), 0);
    }

    void resultHasErrorsWhenFailed() {
        FlashResult r{};
        r.failedDrives << "\\\\.\\PhysicalDrive3";
        QVERIFY(r.hasErrors());
    }

    void resultTotalDrivesSumsCorrectly() {
        FlashResult r{};
        r.successfulDrives << "D:" << "E:";
        r.failedDrives << "F:";
        QCOMPARE(r.totalDrives(), 3);
    }

    void resultSuccessOnlyDrives() {
        FlashResult r{};
        r.success = true;
        r.successfulDrives << "D:" << "E:" << "F:";
        QVERIFY(!r.hasErrors());
        QCOMPARE(r.totalDrives(), 3);
    }

    void resultEjectListsStartEmpty() {
        // Empty means "eject was not attempted", which is what the completion page
        // reads to decide whether to say anything about removal at all.
        FlashResult r{};
        QVERIFY(r.ejectedDrives.isEmpty());
        QVERIFY(r.ejectFailedDrives.isEmpty());
    }

    void resultEjectFailureIsNotAFlashFailure() {
        // A drive can be written and verified and still refuse to eject. That must
        // not turn a successful flash into a failed one, and must not be counted as
        // a target -- but it must still be visible, or the user unplugs a drive
        // Windows still has mounted.
        FlashResult r{};
        r.success = true;
        r.successfulDrives << "\\\\.\\PhysicalDrive2";
        r.ejectFailedDrives << "\\\\.\\PhysicalDrive2";
        QVERIFY(!r.hasErrors());
        QCOMPARE(r.totalDrives(), 1);
        QCOMPARE(r.ejectFailedDrives.size(), 1);
        QVERIFY(r.ejectedDrives.isEmpty());
    }
};

QTEST_MAIN(FlashTypesTests)
#include "test_flash_types.moc"
