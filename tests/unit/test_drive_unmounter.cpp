// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_drive_unmounter.cpp
/// @brief Unit tests for DriveUnmounter

#include "sak/drive_unmounter.h"

#include <QtTest/QtTest>

class TestDriveUnmounter : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void lastError_emptyInitially();
    void getVolumesOnDrive_invalidDrive();
    void ejectDrive_rejectsNegativeDriveNumber();
};

void TestDriveUnmounter::construction_default() {
    DriveUnmounter unmounter;
    QVERIFY(dynamic_cast<QObject*>(&unmounter) != nullptr);
}

void TestDriveUnmounter::lastError_emptyInitially() {
    DriveUnmounter unmounter;
    QVERIFY(unmounter.lastError().isEmpty());
}

void TestDriveUnmounter::getVolumesOnDrive_invalidDrive() {
    DriveUnmounter unmounter;
    // Drive 999 should not exist on any system
    const auto volumes = unmounter.getVolumesOnDrive(999);
    QVERIFY(volumes.isEmpty());
}

void TestDriveUnmounter::ejectDrive_rejectsNegativeDriveNumber() {
    // A negative drive number cannot name a physical disk, and it also aliases the
    // volume-probe sentinels used during enumeration. Refuse it before any device is
    // opened, and say why -- a bare false with no lastError would be reported to the
    // user as an unexplained eject failure.
    DriveUnmounter unmounter;
    QVERIFY(!unmounter.ejectDrive(-1));
    QVERIFY(!unmounter.lastError().isEmpty());
}

QTEST_MAIN(TestDriveUnmounter)
#include "test_drive_unmounter.moc"
