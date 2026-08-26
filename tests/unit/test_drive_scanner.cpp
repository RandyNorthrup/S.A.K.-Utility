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

// Real drive enumeration goes out to the OS (WMI / SetupAPI / volume APIs) and takes SECONDS.
// Under a loaded full-suite run it measured 8000-10550 ms on this machine against QTest's 5000 ms
// QTRY default, so the whole suite failed the gate while the scanner was working correctly --
// QtTest's own message even reports how long would have sufficed. That is a LOAD-dependent test
// failure, the exact class scripts/run_flake_soak.py exists to find (R5-G18-8).
//
// Bounded generously rather than removed: a scan that never completes must still fail, so this
// is a longer deadline, not an unbounded wait.
constexpr int kDriveEnumerationTimeoutMs = 30'000;

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

    // isValid() is a TWO-arm AND (!devicePath.isEmpty() && size > 0) and the default record trips
    // BOTH arms, so it stays invalid even with one arm deleted. Exercise each arm alone.
    sak::DriveInfo pathOnly;
    pathOnly.devicePath = QStringLiteral("\\\\.\\PhysicalDrive0");
    QVERIFY(!pathOnly.isValid());  // size still 0

    sak::DriveInfo sizeOnly;
    sizeOnly.size = 1024;
    QVERIFY(!sizeOnly.isValid());  // devicePath still empty

    // ...and the size arm is `> 0`, not `!= 0`: a negative geometry result is not a valid drive.
    sak::DriveInfo negativeSize;
    negativeSize.devicePath = QStringLiteral("\\\\.\\PhysicalDrive0");
    negativeSize.size = -1;
    QVERIFY(!negativeSize.isValid());
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

    // The first scan lands on an EMPTY cache, so applyDriveScan() must PUBLISH it: one
    // driveAttached per drive found, and exactly one drivesUpdated carrying the whole list. No
    // test in this file ever asserted a signal, so an applyDriveScan() that filled m_drives and
    // emitted nothing at all (every subscribed panel showing no drives) passed the suite.
    int attachedCount = 0;
    int updatedCount = 0;
    QList<sak::DriveInfo> published;
    QObject::connect(&scanner,
                     &DriveScanner::driveAttached,
                     &scanner,
                     [&attachedCount](const sak::DriveInfo&) { ++attachedCount; });
    QObject::connect(&scanner,
                     &DriveScanner::drivesUpdated,
                     &scanner,
                     [&updatedCount, &published](const QList<sak::DriveInfo>& drives) {
                         ++updatedCount;
                         published = drives;
                     });

    scanner.refresh();

    // The scan runs on a worker thread; spin the event loop until it lands.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

    const QList<sak::DriveInfo> drives = scanner.getDrives();
    QCOMPARE(updatedCount, 1);  // one scan, one publication -- not per-drive, not zero
    QCOMPARE(attachedCount, static_cast<int>(drives.size()));
    QStringList publishedPaths;
    for (const sak::DriveInfo& drive : published) {
        publishedPaths.append(drive.devicePath);
    }
    QStringList cachedPaths;
    for (const sak::DriveInfo& drive : drives) {
        cachedPaths.append(drive.devicePath);
    }
    QCOMPARE(publishedPaths, cachedPaths);  // the published list IS the cache, in cache order

    // Every cached record passed the isValid() filter in enumerateDrivesOnce(), carries the exact
    // "\\.\PhysicalDriveN" path queryDriveInfo() builds, and the list is in the STRICTLY ascending
    // drive-number order enumeratePhysicalDriveNumbers() sorts and de-duplicates into. A bare
    // !isEmpty() equally passed a scan that cached invalid records or listed a drive twice.
    const QString prefix = QStringLiteral("\\\\.\\PhysicalDrive");
    int previousNumber = -1;
    for (const sak::DriveInfo& drive : drives) {
        QVERIFY2(drive.isValid(), qPrintable(drive.devicePath));
        QCOMPARE(drive.devicePath.left(prefix.size()), prefix);
        bool numberOk = false;
        const int number = drive.devicePath.mid(prefix.size()).toInt(&numberOk);
        QVERIFY2(numberOk && number > previousNumber, qPrintable(drive.devicePath));
        previousNumber = number;
    }
    scanner.refresh();

    // The scan runs on a worker thread; spin the event loop until it lands.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);
}

void DriveScannerTests::getRemovableDrives_subset() {
    DriveScanner scanner;
    scanner.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

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
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

    auto drives = scanner.getDrives();
    bool foundSystem = false;
    for (const auto& drive : drives) {
        if (drive.isSystem) {
            QVERIFY2(scanner.isSystemDrive(drive.devicePath), qPrintable(drive.devicePath));
            foundSystem = true;
        } else {
            // The negative arm: a cached NON-system drive must not be reported as one. Only the
            // positive arm was asserted, so `return true;` passed the whole test.
            QVERIFY2(!scanner.isSystemDrive(drive.devicePath), qPrintable(drive.devicePath));
        }
    }
    QVERIFY(foundSystem);  // Should always have a system drive

    // isSystemDrive() resolves the path through getDriveInfo() (drive_scanner.cpp:208-211): an
    // unknown or empty device path is not in the cache and is never the system drive. These two
    // hold on any machine, so they kill the `return true;` mutant unconditionally.
    QVERIFY(!scanner.isSystemDrive(QStringLiteral("\\\\.\\PhysicalDrive999")));
    QVERIFY(!scanner.isSystemDrive(QString()));
}

// ============================================================================
// Refresh
// ============================================================================

void DriveScannerTests::refresh_doubleCallStillCompletes() {
    DriveScanner scanner;
    scanner.refresh();
    QVERIFY(scanner.m_isScanning.load());  // the first scan claimed the in-flight guard
    scanner.refresh();  // Lands while the first scan is still in flight -> swallowed at the CAS.

    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

    // The guard is RELEASED again when the scan lands. onScanFinished() clears m_isScanning right
    // after applyDriveScan(), both on THIS thread, so it is already false the moment the cache
    // becomes visible -- no timing assumption. The bare !isEmpty() above passes even with the
    // guard left permanently set, because the FIRST scan still landed; a wedged scanner then
    // ignores every later refresh() and hot-plug notification forever.
    QVERIFY(!scanner.m_isScanning.load());

    // ...and a released guard really does admit the next scan.
    scanner.refresh();
    QVERIFY(scanner.m_isScanning.load());
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.m_isScanning.load(), kDriveEnumerationTimeoutMs);
    scanner.refresh();  // Lands while the first scan is still in flight.

    // The second call is swallowed by the m_isScanning in-flight guard. The point is that
    // the guard is RELEASED again when the scan lands: if it were left set (or the second
    // call replaced the running future), the cache would stay empty forever and the
    // scanner would be permanently wedged -- which a bare "did not crash" never noticed.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);
}

// ============================================================================
// Start / Stop
// ============================================================================

void DriveScannerTests::startStop_lifecycle() {
    DriveScanner scanner;
    QVERIFY(!scanner.m_refreshTimer->isActive());  // idle until start()
    scanner.start();
    // start() kicks an initial scan before arming the refresh timer, so monitoring really
    // is live once this lands.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);
    // The cache alone did not prove monitoring is LIVE: the fallback refresh timer must be armed,
    // otherwise start() is a one-shot scan and no drive ever hot-plugs in again.
    QVERIFY(scanner.m_refreshTimer->isActive());

    scanner.stop();
    // stop() joins the in-flight worker scan and DROPS the cache: a stopped scanner must
    // never keep handing out a drive list it is no longer refreshing.
    QVERIFY(scanner.getDrives().isEmpty());
    // ...and it disarms the timer, so nothing repopulates the cache it just dropped.
    QVERIFY(!scanner.m_refreshTimer->isActive());
    scanner.start();
    // start() kicks an initial scan before arming the refresh timer, so monitoring really
    // is live once this lands.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

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
    // Wait for the scan to LAND. refresh() only kicks a worker future (drive_scanner.cpp:225);
    // without this the cache is still EMPTY when the lookup runs, so getDriveInfo() misses for
    // every path and the miss this test is named for is never reached.
    QTRY_VERIFY_WITH_TIMEOUT(!scanner.getDrives().isEmpty(), kDriveEnumerationTimeoutMs);

    const auto info = scanner.getDriveInfo(QStringLiteral("\\\\.\\PhysicalDrive999"));
    QVERIFY(!info.isValid());
    // A miss returns a DEFAULT-constructed record, not a neighbouring drive's payload.
    QVERIFY(info.devicePath.isEmpty());
    QCOMPARE(info.size, qint64(0));
    QCOMPARE(info.blockSize, 0u);
    QVERIFY(info.name.isEmpty());
    QVERIFY(info.mountPoints.isEmpty());

    // ...and a path that IS cached hits, proving the miss above came from the lookup rather than
    // from an unpopulated cache.
    const auto cached = scanner.getDrives().first();
    const auto hit = scanner.getDriveInfo(cached.devicePath);
    QVERIFY(hit.isValid());
    QCOMPARE(hit.devicePath, cached.devicePath);
    QCOMPARE(hit.size, cached.size);
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

    // ...and it is the enumeration_ok flag that carries that, not the empty `scanned`: a PARTIAL
    // scan that really saw one cached drive and missed the other is still non-authoritative, so it
    // detaches nothing either. An `if (scanned.isEmpty()) return {};` short-circuit passed the
    // case above, which is the shape a real partial enumeration actually takes.
    const QList<sak::DriveInfo> partial = {makeDrive("\\\\.\\PhysicalDrive0")};
    QVERIFY(DriveScanner::drivesDetached(current, partial, /*enumeration_ok=*/false).isEmpty());
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
    // Drive0 is ALREADY cached, so the union must de-duplicate it rather than append a second
    // copy; a scanned list disjoint from `current` never reaches the containsDevicePath() guard.
    const QList<sak::DriveInfo> scanned = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive2")};

    // Non-authoritative: cached drives are kept, the newly-seen one is added.
    const auto merged = DriveScanner::mergeDriveList(current, scanned, /*enumeration_ok=*/false);
    // mergeDriveList copies current (order preserved) then appends each scanned drive not
    // already present, so the union is the fixed ordered list [Drive0, Drive1, Drive2]. The old
    // any_of pair never asserted Drive1 -- the cached drive this test's name promises is never
    // dropped -- so a [Drive0, Drive0, Drive2] regression that dropped Drive1 passed. Pin order.
    QCOMPARE(merged.size(), 3);
    QCOMPARE(merged.at(0).devicePath, QStringLiteral("\\\\.\\PhysicalDrive0"));
    QCOMPARE(merged.at(1).devicePath, QStringLiteral("\\\\.\\PhysicalDrive1"));
    QCOMPARE(merged.at(2).devicePath, QStringLiteral("\\\\.\\PhysicalDrive2"));
}

void DriveScannerTests::mergeDriveList_authoritativeReplaces() {
    const QList<sak::DriveInfo> current = {makeDrive("\\\\.\\PhysicalDrive0"),
                                           makeDrive("\\\\.\\PhysicalDrive1")};
    const QList<sak::DriveInfo> scanned = {makeDrive("\\\\.\\PhysicalDrive0")};

    const auto merged = DriveScanner::mergeDriveList(current, scanned, /*enumeration_ok=*/true);
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.first().devicePath, QStringLiteral("\\\\.\\PhysicalDrive0"));

    // "Replace" is not "intersect": the scanned list here is a SUBSET of the cache, so an
    // implementation that kept only the drives common to both passed the case above. An
    // authoritative scan must install a newly-seen drive and adopt the scan's own order.
    const QList<sak::DriveInfo> fresh = {makeDrive("\\\\.\\PhysicalDrive2"),
                                         makeDrive("\\\\.\\PhysicalDrive0")};
    const auto replaced = DriveScanner::mergeDriveList(current, fresh, /*enumeration_ok=*/true);
    QCOMPARE(replaced.size(), 2);
    QCOMPARE(replaced.at(0).devicePath, QStringLiteral("\\\\.\\PhysicalDrive2"));
    QCOMPARE(replaced.at(1).devicePath, QStringLiteral("\\\\.\\PhysicalDrive0"));
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

    // Both cases above keep bytes_returned (32) <= buffer_size (64), so only the bytes_returned
    // half of the std::min() clamp is exercised. The hostile case B6-24 exists for is a driver
    // that OVER-reports bytes_returned: the offset must then be measured against OUR buffer.
    // `readable` is deliberately larger than the declared buffer_size so a regression reads real
    // 'A' bytes (a non-empty result that fails deterministically) instead of stepping off the
    // allocation.
    std::vector<BYTE> readable(256, 'A');
    QVERIFY(DriveScanner::descriptorString(readable.data(), 64, 4096, 64).isEmpty());
    QVERIFY(DriveScanner::descriptorString(readable.data(), 64, 4096, 200).isEmpty());
}

void DriveScannerTests::descriptorString_stopsAtBufferEndWhenUnterminated() {
    std::vector<BYTE> buffer(64, 'A');  // no NUL anywhere

    // bytes_returned=32, offset=8 -> at most 24 chars read, never past the returned region.
    const QString out = DriveScanner::descriptorString(buffer.data(), 64, 32, 8);
    QCOMPARE(out, QString(24, QLatin1Char('A')));

    // The same clamp bounds the READ LENGTH, not just the offset: a driver that over-reports
    // bytes_returned cannot stretch an unterminated field past our buffer either. buffer_size=64,
    // offset=8 -> exactly 56 characters however large bytes_returned claims to be. (`readable`
    // holds 256 real bytes so an unclamped regression returns 248+ chars and fails
    // deterministically instead of running off the allocation.)
    std::vector<BYTE> readable(256, 'A');
    QCOMPARE(DriveScanner::descriptorString(readable.data(), 64, 4096, 8),
             QString(56, QLatin1Char('A')));
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

    // An empty root trips no rule at all, so none of the four AND-rules is actually exercised.
    // Boot files ALONE are explicitly not a Windows installation (drive_scanner.cpp:868-872) --
    // that rule is exactly what separates hasWindowsIndicators() from hasBootManagerIndicators().
    QTemporaryDir bootOnly;
    QVERIFY(bootOnly.isValid());
    QFile bootmgr(QDir(bootOnly.path()).filePath("bootmgr"));
    QVERIFY(bootmgr.open(QIODevice::WriteOnly));
    bootmgr.write("stub");
    bootmgr.close();
    QVERIFY(!DriveScanner::hasWindowsIndicators(bootOnly.path()));     // no \Windows beside it
    QVERIFY(DriveScanner::hasBootManagerIndicators(bootOnly.path()));  // ...but still boot-critical

    // ...and rule 1 is an AND: a Windows\System32 DIRECTORY with no kernel in it is not an
    // installation, while \bootmgr next to a \Windows tree (rule 3) is.
    QTemporaryDir noKernel;
    QVERIFY(noKernel.isValid());
    QDir noKernelRoot(noKernel.path());
    QVERIFY(noKernelRoot.mkpath("Windows/System32"));
    QVERIFY(!DriveScanner::hasWindowsIndicators(noKernel.path()));
    QFile bootWithWindows(noKernelRoot.filePath("bootmgr"));
    QVERIFY(bootWithWindows.open(QIODevice::WriteOnly));
    bootWithWindows.write("stub");
    bootWithWindows.close();
    QVERIFY(DriveScanner::hasWindowsIndicators(noKernel.path()));
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

    // \bootmgr is only ONE of the three indicators. The split-boot case this guard exists for
    // (drive_scanner.h:251-254) carries EFI\Microsoft\Boot\bootmgfw.efi and NO \bootmgr, and
    // BOOTNXT is the third arm -- neither was ever exercised, so a probe narrowed to bootmgr
    // alone stayed green while leaving a split-boot ESP erasable.
    QTemporaryDir esp;
    QVERIFY(esp.isValid());
    QDir espRoot(esp.path());
    QVERIFY(espRoot.mkpath("EFI/Microsoft/Boot"));
    QFile bootmgfw(espRoot.filePath("EFI/Microsoft/Boot/bootmgfw.efi"));
    QVERIFY(bootmgfw.open(QIODevice::WriteOnly));
    bootmgfw.write("stub");
    bootmgfw.close();
    QVERIFY(DriveScanner::hasBootManagerIndicators(esp.path()));

    QTemporaryDir nxtDir;
    QVERIFY(nxtDir.isValid());
    QFile bootnxt(QDir(nxtDir.path()).filePath("BOOTNXT"));
    QVERIFY(bootnxt.open(QIODevice::WriteOnly));
    bootnxt.write("stub");
    bootnxt.close();
    QVERIFY(DriveScanner::hasBootManagerIndicators(nxtDir.path()));
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
    const sak::DriveInfo base = makeDrive("\\\\.\\PhysicalDrive0");

    // driveInfoChanged() compares NINE fields and each one must count as a change on its own.
    // Only size, isReadOnly and mountPoints were pinned, so dropping any of the other six
    // conjuncts left an in-place property change silently unreported (no drivesUpdated, stale
    // panels) while this test stayed green.
    sak::DriveInfo sized = base;
    sized.size = base.size + 4096;
    QVERIFY(DriveScanner::driveInfoChanged(base, sized));

    sak::DriveInfo blocked = base;
    blocked.blockSize = base.blockSize + 512;
    QVERIFY(DriveScanner::driveInfoChanged(base, blocked));

    sak::DriveInfo named = base;
    named.name = QStringLiteral("Generic USB Flash Disk");
    QVERIFY(DriveScanner::driveInfoChanged(base, named));

    sak::DriveInfo readOnly = base;
    readOnly.isReadOnly = !base.isReadOnly;
    QVERIFY(DriveScanner::driveInfoChanged(base, readOnly));

    sak::DriveInfo system = base;
    system.isSystem = !base.isSystem;
    QVERIFY(DriveScanner::driveInfoChanged(base, system));

    sak::DriveInfo removable = base;
    removable.isRemovable = !base.isRemovable;
    QVERIFY(DriveScanner::driveInfoChanged(base, removable));

    sak::DriveInfo mounted = base;
    mounted.mountPoints = QStringList{QStringLiteral("E:\\")};
    QVERIFY(DriveScanner::driveInfoChanged(base, mounted));

    sak::DriveInfo labelled = base;
    labelled.volumeLabel = QStringLiteral("SAK");
    QVERIFY(DriveScanner::driveInfoChanged(base, labelled));

    sak::DriveInfo bus = base;
    bus.busType = QStringLiteral("USB");
    QVERIFY(DriveScanner::driveInfoChanged(base, bus));
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
    // A successful call wins over whatever GetLastError() happens to hold. collectMountPaths()
    // passes GetLastError() UNCONDITIONALLY (drive_scanner.cpp:792-793), including after a call
    // that succeeded, so a stale ERROR_MORE_DATA left behind by an earlier API must not turn a
    // completed query into a retry -- and, once the attempts run out, into a bogus "unmounted".
    QCOMPARE(DriveScanner::volumePathQueryOutcome(true, ERROR_MORE_DATA, 65'536u, 1024u),
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
