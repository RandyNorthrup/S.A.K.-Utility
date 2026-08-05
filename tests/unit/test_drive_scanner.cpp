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
    void refresh_doubleCallStillCompletes();

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

    // --- C3-09: boot-loader (ESP/boot) indicators + fail-closed engine boot probe ---
    void hasBootManagerIndicators_detectsBootmgr();
    void hasBootManagerIndicators_emptyRootIsNotBoot();
    void physicalDriveBootProbe_negativeIsUndetermined();

    // --- C3-30: an in-place property change still reports a change ---
    void driveInfoChanged_detectsSizeChange();
    void driveInfoChanged_identicalIsUnchanged();

    // --- R5-P9-36: mount-path reads grow the buffer and never drop paths silently ---
    void volumePathQuery_completeOnSuccess();
    void volumePathQuery_retriesWhenBufferTooSmall();
    void volumePathQuery_failsClosedOnOtherError();
    void volumePathQuery_failsWhenRetryWouldNotGrowBuffer();
    void getMountPoints_reportsAuthoritativeEnumeration();
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
    // Construction performs NO enumeration -- the cache stays empty until start()/refresh()
    // kicks a scan. A constructor that scanned inline would block the GUI thread on Win32
    // IOCTLs, so "empty until asked" is the contract, not an incidental detail.
    QVERIFY(scanner.getDrives().isEmpty());
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

    // ...and it is a genuine FILTER of the cached list, not a separate enumeration: every
    // entry is flagged removable and is present in getDrives() under the same device path.
    // A size comparison alone would still pass if the filter returned the wrong drives.
    for (const auto& drive : removable) {
        QVERIFY2(drive.isRemovable, qPrintable(drive.devicePath));
        QVERIFY2(std::any_of(all.begin(),
                             all.end(),
                             [&drive](const sak::DriveInfo& d) {
                                 return d.devicePath == drive.devicePath;
                             }),
                 qPrintable(drive.devicePath));
    }
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

void DriveScannerTests::refresh_doubleCallStillCompletes() {
    DriveScanner scanner;
    scanner.refresh();
    scanner.refresh();  // Lands while the first scan is still in flight.

    // The second call is swallowed by the m_isScanning in-flight guard. The point is that
    // the guard is RELEASED again when the scan lands: if it were left set (or the second
    // call replaced the running future), the cache would stay empty forever and the
    // scanner would be permanently wedged -- which a bare "did not crash" never noticed.
    QTRY_VERIFY(!scanner.getDrives().isEmpty());
}

// ============================================================================
// Start / Stop
// ============================================================================

void DriveScannerTests::startStop_lifecycle() {
    DriveScanner scanner;
    scanner.start();
    // start() kicks an initial scan before arming the refresh timer, so monitoring really
    // is live once this lands.
    QTRY_VERIFY(!scanner.getDrives().isEmpty());

    scanner.stop();
    // stop() joins the in-flight worker scan and DROPS the cache: a stopped scanner must
    // never keep handing out a drive list it is no longer refreshing.
    QVERIFY(scanner.getDrives().isEmpty());
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

// ============================================================================
// C3-09: split-boot ESP/boot disk protection
// ============================================================================

// A disk carrying only the boot loader (no \Windows) is boot-critical: on a
// split-boot system the ESP that boots the OS lives on a separate physical disk.
void DriveScannerTests::hasBootManagerIndicators_detectsBootmgr() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir root(temp.path());
    QFile bootmgr(root.filePath("bootmgr"));
    QVERIFY(bootmgr.open(QIODevice::WriteOnly));
    bootmgr.write("stub");
    bootmgr.close();

    QVERIFY(DriveScanner::hasBootManagerIndicators(temp.path()));
}

void DriveScannerTests::hasBootManagerIndicators_emptyRootIsNotBoot() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(!DriveScanner::hasBootManagerIndicators(temp.path()));
}

// The engine boot probe fails closed on an unusable drive number rather than
// reporting "not a boot disk".
void DriveScannerTests::physicalDriveBootProbe_negativeIsUndetermined() {
    QCOMPARE(DriveScanner::physicalDriveBootProbe(-1), sak::DiskProbe::Undetermined);
}

// ============================================================================
// C3-30: an in-place property change on the same devicePath is a real change
// ============================================================================

void DriveScannerTests::driveInfoChanged_detectsSizeChange() {
    sak::DriveInfo a = makeDrive("\\\\.\\PhysicalDrive0");
    sak::DriveInfo b = a;
    b.size = a.size + 4096;
    QVERIFY(DriveScanner::driveInfoChanged(a, b));

    sak::DriveInfo c = a;
    c.isReadOnly = !a.isReadOnly;
    QVERIFY(DriveScanner::driveInfoChanged(a, c));

    sak::DriveInfo d = a;
    d.mountPoints = QStringList{QStringLiteral("E:\\")};
    QVERIFY(DriveScanner::driveInfoChanged(a, d));
}

void DriveScannerTests::driveInfoChanged_identicalIsUnchanged() {
    sak::DriveInfo a = makeDrive("\\\\.\\PhysicalDrive0");
    sak::DriveInfo b = a;
    QVERIFY(!DriveScanner::driveInfoChanged(a, b));
}

// ============================================================================
// R5-P9-36: a volume's mount paths are read completely or reported as unread
// ============================================================================

void DriveScannerTests::volumePathQuery_completeOnSuccess() {
    QCOMPARE(DriveScanner::volumePathQueryOutcome(true, ERROR_SUCCESS, 0u, 1024u),
             sak::VolumePathQuery::Complete);
}

void DriveScannerTests::volumePathQuery_retriesWhenBufferTooSmall() {
    // ERROR_MORE_DATA reports the size the API needs: grow the buffer and ask again instead of
    // dropping every mount path of that volume (which would read as "not mounted").
    QCOMPARE(DriveScanner::volumePathQueryOutcome(false, ERROR_MORE_DATA, 4096u, 1024u),
             sak::VolumePathQuery::Retry);
}

void DriveScannerTests::volumePathQuery_failsClosedOnOtherError() {
    // Any non-growable failure is reported, never silently swallowed into an empty list.
    QCOMPARE(DriveScanner::volumePathQueryOutcome(false, ERROR_ACCESS_DENIED, 0u, 1024u),
             sak::VolumePathQuery::Failed);
    QCOMPARE(DriveScanner::volumePathQueryOutcome(false, ERROR_NOT_READY, 8192u, 1024u),
             sak::VolumePathQuery::Failed);
}

void DriveScannerTests::volumePathQuery_failsWhenRetryWouldNotGrowBuffer() {
    // A "required" size that does not exceed the current buffer would retry forever.
    QCOMPARE(DriveScanner::volumePathQueryOutcome(false, ERROR_MORE_DATA, 1024u, 1024u),
             sak::VolumePathQuery::Failed);
    QCOMPARE(DriveScanner::volumePathQueryOutcome(false, ERROR_MORE_DATA, 0u, 1024u),
             sak::VolumePathQuery::Failed);
}

void DriveScannerTests::getMountPoints_reportsAuthoritativeEnumeration() {
    // Every Windows machine can enumerate its volumes, so the out-param must report the scan as
    // authoritative -- the caller can then trust an empty list to mean "no mount points" rather
    // than "the enumeration died halfway through".
    bool enumerationOk = false;
    const QStringList mounts = DriveScanner::getMountPoints(0, &enumerationOk);
    QVERIFY(enumerationOk);
    for (const QString& mount : mounts) {
        QVERIFY(!mount.isEmpty());
    }
}

QTEST_GUILESS_MAIN(DriveScannerTests)
#include "test_drive_scanner.moc"
