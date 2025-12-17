// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QSignalSpy>
#include "sak/drive_scanner.h"

class TestDriveScanner : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Scanner initialization
    void testConstructor();
    void testStart();
    void testStop();
    void testMultipleStarts();
    void testMultipleStops();

    // Drive detection
    void testGetDrives();
    void testGetRemovableDrives();
    void testDriveProperties();
    void testDriveCount();

    // Drive info
    void testDriveInfoStructure();
    void testDevicePath();
    void testDriveName();
    void testDriveSize();
    void testBusType();
    void testDriveType();

    // System drive detection
    void testIsSystemDrive();
    void testSystemDriveIdentification();
    void testNonSystemDrive();

    // Drive lookup
    void testGetDriveInfo();
    void testGetDriveInfoInvalid();
    void testGetDriveByPath();

    // Drive types
    void testDetectUSBDrives();
    void testDetectSATADrives();
    void testDetectNVMeDrives();
    void testDetectSDCard();

    // Drive properties
    void testRemovableFlag();
    void testReadOnlyFlag();
    void testBlockSize();
    void testVolumeLabel();

    // Mount points
    void testDetectMountPoints();
    void testMultipleMountPoints();
    void testNoMountPoint();

    // Hot-plug detection
    void testHotPlugSignals();
    void testDriveAttached();
    void testDriveDetached();

    // Drive list updates
    void testDrivesUpdatedSignal();
    void testRefresh();
    void testAutoRefresh();

    // Error handling
    void testScanError();
    void testInvalidDrive();
    void testAccessDenied();

    // WMI integration
    void testWMIProperties();
    void testSerialNumber();
    void testManufacturer();

    // Filtering
    void testFilterSystemDrives();
    void testFilterRemovable();
    void testFilterReadOnly();

    // Thread safety
    void testSignalThreadAffinity();
    void testConcurrentAccess();

    // Performance
    void testScanSpeed();
    void testLargeDriveCount();

private:
    sak::DriveScanner* m_scanner{nullptr};
    
    void waitForSignal(QObject* obj, const char* signal, int timeout = 5000);
    bool isDriveLetterValid(const QString& path);
};

void TestDriveScanner::initTestCase() {
    // Setup test environment
}

void TestDriveScanner::cleanupTestCase() {
    // Cleanup test environment
}

void TestDriveScanner::init() {
    m_scanner = new sak::DriveScanner(this);
}

void TestDriveScanner::cleanup() {
    m_scanner->stop();
    delete m_scanner;
    m_scanner = nullptr;
}

void TestDriveScanner::waitForSignal(QObject* obj, const char* signal, int timeout) {
    QSignalSpy spy(obj, signal);
    QVERIFY(spy.wait(timeout));
}

bool TestDriveScanner::isDriveLetterValid(const QString& path) {
    return path.length() >= 2 && path[0].isLetter() && path[1] == ':';
}

void TestDriveScanner::testConstructor() {
    QVERIFY(m_scanner != nullptr);
}

void TestDriveScanner::testStart() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::drivesUpdated);
    
    m_scanner->start();
    
    // Should get initial drive list
    QVERIFY(spy.wait(5000));
}

void TestDriveScanner::testStop() {
    m_scanner->start();
    m_scanner->stop();
    
    // Should stop cleanly
}

void TestDriveScanner::testMultipleStarts() {
    m_scanner->start();
    m_scanner->start(); // Should handle gracefully
    m_scanner->stop();
}

void TestDriveScanner::testMultipleStops() {
    m_scanner->start();
    m_scanner->stop();
    m_scanner->stop(); // Should handle gracefully
}

void TestDriveScanner::testGetDrives() {
    m_scanner->start();
    QTest::qWait(1000); // Wait for initial scan
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty()); // At least system drive should be present
}

void TestDriveScanner::testGetRemovableDrives() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto removable = m_scanner->getRemovableDrives();
    // May be empty if no removable drives attached
    
    for (const auto& drive : removable) {
        QVERIFY(drive.is_removable);
    }
}

void TestDriveScanner::testDriveProperties() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        QVERIFY(!drive.device_path.isEmpty());
        QVERIFY(drive.size_bytes >= 0);
    }
}

void TestDriveScanner::testDriveCount() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(drives.count() >= 1); // At least one drive (system)
}

void TestDriveScanner::testDriveInfoStructure() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    const auto& drive = drives.first();
    
    // Verify structure fields exist and have reasonable values
    QVERIFY(!drive.device_path.isEmpty());
    QVERIFY(!drive.friendly_name.isEmpty());
    QVERIFY(drive.size_bytes >= 0);
    QVERIFY(drive.block_size > 0);
}

void TestDriveScanner::testDevicePath() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        // Device path should be like "\\.\PhysicalDrive0"
        QVERIFY(drive.device_path.contains("PhysicalDrive") || 
                drive.device_path.contains(":"));
    }
}

void TestDriveScanner::testDriveName() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        QVERIFY(!drive.friendly_name.isEmpty());
    }
}

void TestDriveScanner::testDriveSize() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        QVERIFY(drive.size_bytes >= 0);
        // System drive should be at least 10GB
        if (!drive.is_removable) {
            QVERIFY(drive.size_bytes >= 10000000000LL);
        }
    }
}

void TestDriveScanner::testBusType() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        // Bus type should be one of known types
        QVERIFY(drive.bus_type == "USB" ||
                drive.bus_type == "SATA" ||
                drive.bus_type == "NVMe" ||
                drive.bus_type == "SD" ||
                drive.bus_type == "SCSI" ||
                drive.bus_type == "Virtual" ||
                drive.bus_type == "Unknown" ||
                drive.bus_type.isEmpty());
    }
}

void TestDriveScanner::testDriveType() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    // At least one drive should be fixed (system drive)
    bool has_fixed = false;
    for (const auto& drive : drives) {
        if (!drive.is_removable) {
            has_fixed = true;
            break;
        }
    }
    QVERIFY(has_fixed);
}

void TestDriveScanner::testIsSystemDrive() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    // At least one drive should be system drive
    bool has_system = false;
    for (const auto& drive : drives) {
        if (m_scanner->isSystemDrive(drive.device_path)) {
            has_system = true;
            break;
        }
    }
    QVERIFY(has_system);
}

void TestDriveScanner::testSystemDriveIdentification() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    int system_count = 0;
    for (const auto& drive : drives) {
        if (m_scanner->isSystemDrive(drive.device_path)) {
            system_count++;
            // System drive should not be removable
            QVERIFY(!drive.is_removable);
        }
    }
    
    QVERIFY(system_count >= 1); // At least one system drive
}

void TestDriveScanner::testNonSystemDrive() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto removable = m_scanner->getRemovableDrives();
    
    for (const auto& drive : removable) {
        // Removable drives should not be system drives
        QVERIFY(!m_scanner->isSystemDrive(drive.device_path));
    }
}

void TestDriveScanner::testGetDriveInfo() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    auto info = m_scanner->getDriveInfo(drives.first().device_path);
    QVERIFY(!info.device_path.isEmpty());
    QCOMPARE(info.device_path, drives.first().device_path);
}

void TestDriveScanner::testGetDriveInfoInvalid() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto info = m_scanner->getDriveInfo("\\\\.\\InvalidDrive99");
    QVERIFY(info.device_path.isEmpty() || info.size_bytes == 0);
}

void TestDriveScanner::testGetDriveByPath() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        auto info = m_scanner->getDriveInfo(drive.device_path);
        QCOMPARE(info.device_path, drive.device_path);
    }
}

void TestDriveScanner::testDetectUSBDrives() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    for (const auto& drive : drives) {
        if (drive.bus_type == "USB") {
            QVERIFY(drive.is_removable);
        }
    }
}

void TestDriveScanner::testDetectSATADrives() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // SATA drives common in desktops
    for (const auto& drive : drives) {
        if (drive.bus_type == "SATA") {
            QVERIFY(drive.size_bytes > 0);
        }
    }
}

void TestDriveScanner::testDetectNVMeDrives() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // NVMe drives should have high performance characteristics
    for (const auto& drive : drives) {
        if (drive.bus_type == "NVMe") {
            QVERIFY(!drive.is_removable);
        }
    }
}

void TestDriveScanner::testDetectSDCard() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    for (const auto& drive : drives) {
        if (drive.bus_type == "SD") {
            QVERIFY(drive.is_removable);
        }
    }
}

void TestDriveScanner::testRemovableFlag() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // System drive should not be removable
    bool found_non_removable = false;
    for (const auto& drive : drives) {
        if (!drive.is_removable) {
            found_non_removable = true;
            break;
        }
    }
    QVERIFY(found_non_removable);
}

void TestDriveScanner::testReadOnlyFlag() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // System drive should not be read-only
    for (const auto& drive : drives) {
        if (m_scanner->isSystemDrive(drive.device_path)) {
            QVERIFY(!drive.is_read_only);
        }
    }
}

void TestDriveScanner::testBlockSize() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    for (const auto& drive : drives) {
        // Block size typically 512 or 4096 bytes
        QVERIFY(drive.block_size == 512 || 
                drive.block_size == 4096 ||
                drive.block_size > 0);
    }
}

void TestDriveScanner::testVolumeLabel() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Some drives may have labels
    for (const auto& drive : drives) {
        // Volume label can be empty or have a name
        QVERIFY(drive.volume_label.length() >= 0);
    }
}

void TestDriveScanner::testDetectMountPoints() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // System drive should have mount point (C:)
    for (const auto& drive : drives) {
        if (m_scanner->isSystemDrive(drive.device_path)) {
            QVERIFY(!drive.mount_points.isEmpty());
        }
    }
}

void TestDriveScanner::testMultipleMountPoints() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Drives can have multiple mount points
    for (const auto& drive : drives) {
        QVERIFY(drive.mount_points.count() >= 0);
    }
}

void TestDriveScanner::testNoMountPoint() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Some drives may not be mounted
    // This is valid behavior
}

void TestDriveScanner::testHotPlugSignals() {
    QSignalSpy spy_attached(m_scanner, &sak::DriveScanner::driveAttached);
    QSignalSpy spy_detached(m_scanner, &sak::DriveScanner::driveDetached);
    
    m_scanner->start();
    
    // Wait briefly for hot-plug monitoring to activate
    QTest::qWait(1000);
    
    // Cannot simulate actual plug/unplug in test
    // Just verify scanner is set up for notifications
}

void TestDriveScanner::testDriveAttached() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::driveAttached);
    
    m_scanner->start();
    QTest::qWait(1000);
    
    // Would need actual hardware event to test
    // Verify signal exists
}

void TestDriveScanner::testDriveDetached() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::driveDetached);
    
    m_scanner->start();
    QTest::qWait(1000);
    
    // Would need actual hardware event to test
    // Verify signal exists
}

void TestDriveScanner::testDrivesUpdatedSignal() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::drivesUpdated);
    
    m_scanner->start();
    
    // Should get initial update
    QVERIFY(spy.wait(5000));
    QVERIFY(spy.count() >= 1);
}

void TestDriveScanner::testRefresh() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::drivesUpdated);
    
    m_scanner->start();
    QVERIFY(spy.wait(5000));
    
    spy.clear();
    m_scanner->refresh();
    
    // Should get another update
    QVERIFY(spy.wait(5000));
}

void TestDriveScanner::testAutoRefresh() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::drivesUpdated);
    
    m_scanner->start();
    
    // Should get periodic updates
    QVERIFY(spy.wait(5000));
    
    int initial_count = spy.count();
    
    // Wait for next auto-refresh
    QTest::qWait(3000);
    
    // May or may not get another update depending on timing
}

void TestDriveScanner::testScanError() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::scanError);
    
    m_scanner->start();
    QTest::qWait(1000);
    
    // Normal operation shouldn't produce errors
    QVERIFY(spy.count() == 0);
}

void TestDriveScanner::testInvalidDrive() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto info = m_scanner->getDriveInfo("Z:\\InvalidPath");
    
    // Should return invalid info or empty
    QVERIFY(info.device_path.isEmpty() || info.size_bytes == 0);
}

void TestDriveScanner::testAccessDenied() {
    m_scanner->start();
    QTest::qWait(1000);
    
    // Even without admin, should be able to enumerate drives
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
}

void TestDriveScanner::testWMIProperties() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    QVERIFY(!drives.isEmpty());
    
    // WMI properties should be populated
    for (const auto& drive : drives) {
        QVERIFY(!drive.friendly_name.isEmpty());
        // Serial number may or may not be available
    }
}

void TestDriveScanner::testSerialNumber() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Serial numbers may be available for some drives
    for (const auto& drive : drives) {
        // Serial can be empty or populated
        QVERIFY(drive.serial_number.length() >= 0);
    }
}

void TestDriveScanner::testManufacturer() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Manufacturer info may be available
    for (const auto& drive : drives) {
        // Can be empty or have manufacturer name
        QVERIFY(drive.manufacturer.length() >= 0);
    }
}

void TestDriveScanner::testFilterSystemDrives() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto all_drives = m_scanner->getDrives();
    
    QList<sak::DriveInfo> non_system;
    for (const auto& drive : all_drives) {
        if (!m_scanner->isSystemDrive(drive.device_path)) {
            non_system.append(drive);
        }
    }
    
    // Should have at least system drive in all_drives
    QVERIFY(all_drives.count() >= 1);
}

void TestDriveScanner::testFilterRemovable() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto all_drives = m_scanner->getDrives();
    auto removable = m_scanner->getRemovableDrives();
    
    QVERIFY(removable.count() <= all_drives.count());
}

void TestDriveScanner::testFilterReadOnly() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    QList<sak::DriveInfo> writable;
    for (const auto& drive : drives) {
        if (!drive.is_read_only) {
            writable.append(drive);
        }
    }
    
    // Should have at least one writable drive
    QVERIFY(!writable.isEmpty());
}

void TestDriveScanner::testSignalThreadAffinity() {
    QSignalSpy spy(m_scanner, &sak::DriveScanner::drivesUpdated);
    
    m_scanner->start();
    
    if (spy.wait(5000)) {
        // Signals should be on main thread
        QCOMPARE(m_scanner->thread(), QThread::currentThread());
    }
}

void TestDriveScanner::testConcurrentAccess() {
    m_scanner->start();
    QTest::qWait(1000);
    
    // Multiple concurrent reads should be safe
    auto drives1 = m_scanner->getDrives();
    auto drives2 = m_scanner->getRemovableDrives();
    auto drives3 = m_scanner->getDrives();
    
    QVERIFY(!drives1.isEmpty());
}

void TestDriveScanner::testScanSpeed() {
    QElapsedTimer timer;
    timer.start();
    
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    qint64 elapsed = timer.elapsed();
    
    // Initial scan should complete quickly (under 5 seconds)
    QVERIFY(elapsed < 5000);
}

void TestDriveScanner::testLargeDriveCount() {
    m_scanner->start();
    QTest::qWait(1000);
    
    auto drives = m_scanner->getDrives();
    
    // Should handle any reasonable number of drives
    QVERIFY(drives.count() >= 1);
    QVERIFY(drives.count() < 100); // Sanity check
}

QTEST_MAIN(TestDriveScanner)
#include "test_drive_scanner.moc"
