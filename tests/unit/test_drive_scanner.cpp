// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_drive_scanner.cpp
/// @brief Unit tests for drive enumeration and removable detection

#include "sak/drive_scanner.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstring>
#include <vector>

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

    // --- B6-23: partial enumeration must not spuriously detach cached drives ---
    void drivesDetached_partialScanKeepsAll();
    void drivesDetached_authoritativeReportsRemoved();
    void mergeDriveList_partialUnionsNeverDrops();
    void mergeDriveList_authoritativeReplaces();

    // --- B6-24: STORAGE_DEVICE_DESCRIPTOR field read is OOB-safe ---
    void descriptorString_readsBoundedField();
    void descriptorString_rejectsOutOfRangeOffset();
    void descriptorString_stopsAtBufferEndWhenUnterminated();
    void descriptorString_absentFieldIsEmpty();

    // --- B6-25: writability probe fails closed (never announces writable on error) ---
    void driveReadOnly_confirmedWritable();
    void driveReadOnly_writeProtected();
    void driveReadOnly_zeroLength();
    void driveReadOnly_unknownErrorFailsClosed();

    // --- B6-26: Windows indicators detected on a volume root ---
    void hasWindowsIndicators_detectsSystem32();
    void hasWindowsIndicators_emptyRootIsNotSystem();
};

namespace {
sak::DriveInfo makeDrive(const QString& path) {
    sak::DriveInfo info;
    info.devicePath = path;
    info.size = 1024;
    return info;
}
}  // namespace

// ============================================================================
// Constructor
// ============================================================================

void DriveScannerTests::constructor_defaults() {
    DriveScanner scanner;
    // Initially may have no drives until refresh is called
    QVERIFY(true);  // No crash
}

// ============================================================================
// DriveInfo Struct
// ============================================================================

void DriveScannerTests::driveInfo_defaultInvalid() {
    sak::DriveInfo info;
    QVERIFY(!info.isValid());
}

void DriveScannerTests::driveInfo_validWhenPopulated() {
    sak::DriveInfo info;
    info.devicePath = "\\\\.\\PhysicalDrive0";
    info.name = "Test Drive";
    info.size = 500'107'862'016;
    info.blockSize = 512;
    QVERIFY(info.isValid());
}

// ============================================================================
// Drive Enumeration
// ============================================================================

void DriveScannerTests::getDrives_returnsNonEmpty() {
    DriveScanner scanner;
    scanner.refresh();

    // The scan runs on a worker thread; spin the event loop until it lands.
    QTRY_VERIFY(!scanner.getDrives().isEmpty());
}

void DriveScannerTests::getRemovableDrives_subset() {
    DriveScanner scanner;
    scanner.refresh();
    QTRY_VERIFY(!scanner.getDrives().isEmpty());

    auto all = scanner.getDrives();
    auto removable = scanner.getRemovableDrives();

    // Removable count should be <= total count
    QVERIFY(removable.size() <= all.size());
}

void DriveScannerTests::isSystemDrive_systemDriveDetected() {
    DriveScanner scanner;
    scanner.refresh();
    QTRY_VERIFY(!scanner.getDrives().isEmpty());

    auto drives = scanner.getDrives();
    bool foundSystem = false;
    for (const auto& drive : drives) {
        if (drive.isSystem) {
            QVERIFY(scanner.isSystemDrive(drive.devicePath));
            foundSystem = true;
        }
    }
    QVERIFY(foundSystem);  // Should always have a system drive
}

// ============================================================================
// Refresh
// ============================================================================

void DriveScannerTests::refresh_doesNotCrash() {
    DriveScanner scanner;
    scanner.refresh();
    scanner.refresh();  // Double refresh should be safe
    QVERIFY(true);
}

// ============================================================================
// Start / Stop
// ============================================================================

void DriveScannerTests::startStop_lifecycle() {
    DriveScanner scanner;
    scanner.start();
    QTest::qWait(100);  // Let it run briefly
    scanner.stop();
    QVERIFY(true);      // No crash
}

// ============================================================================
// getDriveInfo
// ============================================================================

void DriveScannerTests::getDriveInfo_nonExistentDrive() {
    DriveScanner scanner;
    scanner.refresh();

    auto info = scanner.getDriveInfo("\\\\.\\PhysicalDrive999");
    QVERIFY(!info.isValid());
}

// ============================================================================
// B6-23: enumeration_ok gating -- a partial scan must never detach cached drives
// ============================================================================

void DriveScannerTests::drivesDetached_partialScanKeepsAll() {
    const QList<sak::DriveInfo> current = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive1")};
    const QList<sak::DriveInfo> scanned;  // enumeration failed -> empty/probe-only

    // enumeration_ok == false: NOTHING is reported detached even though scanned is empty.
    const auto detached = DriveScanner::drivesDetached(current, scanned, /*enumeration_ok=*/false);
    QVERIFY(detached.isEmpty());
}

void DriveScannerTests::drivesDetached_authoritativeReportsRemoved() {
    const QList<sak::DriveInfo> current = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive1")};
    const QList<sak::DriveInfo> scanned = {makeDrive("\\\\.\\PhysicalDrive0")};

    const auto detached = DriveScanner::drivesDetached(current, scanned, /*enumeration_ok=*/true);
    QCOMPARE(detached.size(), 1);
    QCOMPARE(detached.first().devicePath, QStringLiteral("\\\\.\\PhysicalDrive1"));
}

void DriveScannerTests::mergeDriveList_partialUnionsNeverDrops() {
    const QList<sak::DriveInfo> current = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive1")};
    const QList<sak::DriveInfo> scanned = {makeDrive("\\\\.\\PhysicalDrive2")};

    // Non-authoritative: cached drives are kept, the newly-seen one is added.
    const auto merged = DriveScanner::mergeDriveList(current, scanned, /*enumeration_ok=*/false);
    QCOMPARE(merged.size(), 3);
    QVERIFY(std::any_of(merged.begin(), merged.end(), [](const sak::DriveInfo& d) {
        return d.devicePath == QStringLiteral("\\\\.\\PhysicalDrive0");
    }));
    QVERIFY(std::any_of(merged.begin(), merged.end(), [](const sak::DriveInfo& d) {
        return d.devicePath == QStringLiteral("\\\\.\\PhysicalDrive2");
    }));
}

void DriveScannerTests::mergeDriveList_authoritativeReplaces() {
    const QList<sak::DriveInfo> current = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive1")};
    const QList<sak::DriveInfo> scanned = {makeDrive("\\\\.\\PhysicalDrive0")};

    const auto merged = DriveScanner::mergeDriveList(current, scanned, /*enumeration_ok=*/true);
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.first().devicePath, QStringLiteral("\\\\.\\PhysicalDrive0"));
}

// ============================================================================
// B6-24: descriptorString bounds-checks the driver-supplied field offset
// ============================================================================

void DriveScannerTests::descriptorString_readsBoundedField() {
    std::vector<BYTE> buffer(64, 0);
    const char* vendor = "SanDisk";
    std::memcpy(buffer.data() + 8, vendor, std::strlen(vendor));  // NUL already present after

    const QString out = DriveScanner::descriptorString(buffer.data(), 64, 32, 8);
    QCOMPARE(out, QStringLiteral("SanDisk"));
}

void DriveScannerTests::descriptorString_rejectsOutOfRangeOffset() {
    std::vector<BYTE> buffer(64, 'A');

    // Offset at/after bytes_returned -> outside the returned data -> empty (no OOB read).
    QVERIFY(DriveScanner::descriptorString(buffer.data(), 64, 32, 32).isEmpty());
    QVERIFY(DriveScanner::descriptorString(buffer.data(), 64, 32, 40).isEmpty());
}

void DriveScannerTests::descriptorString_stopsAtBufferEndWhenUnterminated() {
    std::vector<BYTE> buffer(64, 'A');  // no NUL anywhere

    // bytes_returned=32, offset=8 -> at most 24 chars read, never past the returned region.
    const QString out = DriveScanner::descriptorString(buffer.data(), 64, 32, 8);
    QCOMPARE(out.size(), 24);
}

void DriveScannerTests::descriptorString_absentFieldIsEmpty() {
    std::vector<BYTE> buffer(64, 'A');
    QVERIFY(DriveScanner::descriptorString(buffer.data(), 64, 32, 0).isEmpty());
}

// ============================================================================
// B6-25: writability probe fails closed
// ============================================================================

void DriveScannerTests::driveReadOnly_confirmedWritable() {
    // IOCTL succeeded -> the only affirmative "writable" signal.
    QVERIFY(!DriveScanner::driveReadOnlyFromProbe(true, ERROR_SUCCESS, false, 0));
}

void DriveScannerTests::driveReadOnly_writeProtected() {
    QVERIFY(DriveScanner::driveReadOnlyFromProbe(false, ERROR_WRITE_PROTECT, false, 0));
}

void DriveScannerTests::driveReadOnly_zeroLength() {
    QVERIFY(DriveScanner::driveReadOnlyFromProbe(false, ERROR_NOT_READY, true, 0));
}

void DriveScannerTests::driveReadOnly_unknownErrorFailsClosed() {
    // Access denied / unsupported IOCTL: undeterminable -> read-only, NOT a fail-open "writable".
    QVERIFY(DriveScanner::driveReadOnlyFromProbe(false, ERROR_ACCESS_DENIED, false, 0));
    QVERIFY(DriveScanner::driveReadOnlyFromProbe(false, ERROR_INVALID_FUNCTION, true, 512'000));
}

// ============================================================================
// B6-26: Windows-installation indicators on a volume root
// ============================================================================

void DriveScannerTests::hasWindowsIndicators_detectsSystem32() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir root(temp.path());
    QVERIFY(root.mkpath("Windows/System32"));
    QFile kernel(root.filePath("Windows/System32/ntoskrnl.exe"));
    QVERIFY(kernel.open(QIODevice::WriteOnly));
    kernel.write("stub");
    kernel.close();

    QVERIFY(DriveScanner::hasWindowsIndicators(temp.path()));
}

void DriveScannerTests::hasWindowsIndicators_emptyRootIsNotSystem() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(!DriveScanner::hasWindowsIndicators(temp.path()));
}

QTEST_GUILESS_MAIN(DriveScannerTests)
#include "test_drive_scanner.moc"
