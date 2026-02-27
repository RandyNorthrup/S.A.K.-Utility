// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_drive_scanner.cpp
/// @brief Unit tests for drive enumeration and removable detection

#include <QtTest/QtTest>

#include "sak/drive_scanner.h"

#include <QSignalSpy>

class DriveScannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Constructor
    void constructor_defaults();

    // DriveInfo struct
    void driveInfo_defaultInvalid();
    void driveInfo_validWhenPopulated();

    // Drive enumeration
    void getDrives_returnsNonEmpty();
    void getRemovableDrives_subset();
    void isSystemDrive_systemDriveDetected();

    // Refresh
    void refresh_doesNotCrash();

    // Start/Stop
    void startStop_lifecycle();

    // getDriveInfo
    void getDriveInfo_nonExistentDrive();
};

// ============================================================================
// Constructor
// ============================================================================

void DriveScannerTests::constructor_defaults()
{
    DriveScanner scanner;
    // Initially may have no drives until refresh is called
    QVERIFY(true); // No crash
}

// ============================================================================
// DriveInfo Struct
// ============================================================================

void DriveScannerTests::driveInfo_defaultInvalid()
{
    sak::DriveInfo info;
    QVERIFY(!info.isValid());
}

void DriveScannerTests::driveInfo_validWhenPopulated()
{
    sak::DriveInfo info;
    info.devicePath = "\\\\.\\PhysicalDrive0";
    info.name = "Test Drive";
    info.size = 500107862016;
    info.blockSize = 512;
    QVERIFY(info.isValid());
}

// ============================================================================
// Drive Enumeration
// ============================================================================

void DriveScannerTests::getDrives_returnsNonEmpty()
{
    DriveScanner scanner;
    scanner.refresh();

    auto drives = scanner.getDrives();
    // A system always has at least one drive
    QVERIFY(!drives.isEmpty());
}

void DriveScannerTests::getRemovableDrives_subset()
{
    DriveScanner scanner;
    scanner.refresh();

    auto all = scanner.getDrives();
    auto removable = scanner.getRemovableDrives();

    // Removable count should be <= total count
    QVERIFY(removable.size() <= all.size());
}

void DriveScannerTests::isSystemDrive_systemDriveDetected()
{
    DriveScanner scanner;
    scanner.refresh();

    auto drives = scanner.getDrives();
    bool foundSystem = false;
    for (const auto& drive : drives) {
        if (drive.isSystem) {
            QVERIFY(scanner.isSystemDrive(drive.devicePath));
            foundSystem = true;
        }
    }
    QVERIFY(foundSystem); // Should always have a system drive
}

// ============================================================================
// Refresh
// ============================================================================

void DriveScannerTests::refresh_doesNotCrash()
{
    DriveScanner scanner;
    scanner.refresh();
    scanner.refresh(); // Double refresh should be safe
    QVERIFY(true);
}

// ============================================================================
// Start / Stop
// ============================================================================

void DriveScannerTests::startStop_lifecycle()
{
    DriveScanner scanner;
    scanner.start();
    QTest::qWait(100); // Let it run briefly
    scanner.stop();
    QVERIFY(true); // No crash
}

// ============================================================================
// getDriveInfo
// ============================================================================

void DriveScannerTests::getDriveInfo_nonExistentDrive()
{
    DriveScanner scanner;
    scanner.refresh();

    auto info = scanner.getDriveInfo("\\\\.\\PhysicalDrive999");
    QVERIFY(!info.isValid());
}

QTEST_GUILESS_MAIN(DriveScannerTests)
#include "test_drive_scanner.moc"
