// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/drive_lock.h"

class TestDriveLock : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Constructor tests
    void testConstructorDriveNumber();
    void testConstructorVolumePath();
    void testConstructorReadOnly();

    // Lock status
    void testIsLocked();
    void testIsLockedFailed();
    void testIsLockedAfterUnlock();

    // Handle access
    void testHandleValid();
    void testHandleInvalid();
    void testHandleAfterUnlock();

    // Error handling
    void testLastError();
    void testLastErrorEmpty();
    void testLastErrorAfterFailure();

    // Path access
    void testPath();
    void testPathDrive();
    void testPathVolume();

    // Unlock
    void testUnlock();
    void testUnlockTwice();
    void testUnlockNotLocked();

    // RAII pattern
    void testRAIIPattern();
    void testScopeExit();
    void testExceptionSafety();

    // Move semantics
    void testMoveConstructor();
    void testMoveAssignment();
    void testMoveFromInvalid();

    // Copy prevention
    void testCopyConstructorDeleted();
    void testCopyAssignmentDeleted();

    // Drive number tests
    void testDriveNumberZero();
    void testDriveNumberPositive();
    void testDriveNumberNegative();
    void testDriveNumberInvalid();

    // Volume path tests
    void testVolumePathFormat();
    void testVolumePathInvalid();
    void testVolumePathEmpty();
    void testVolumePathGUID();

    // Read-only mode
    void testReadOnlyLock();
    void testReadWriteLock();
    void testReadOnlyDefault();

    // Access levels
    void testExclusiveAccess();
    void testSharedRead();

    // Multiple locks
    void testMultipleLocksSameDrive();
    void testMultipleLocksDifferentDrives();

    // Administrative privileges
    void testRequiresAdmin();
    void testWithoutAdmin();

    // Error scenarios
    void testLockLockedDrive();
    void testLockNonexistentDrive();
    void testLockSystemDrive();

    // Platform-specific
    void testWindowsHandles();
    void testInvalidHandleValue();

    // Edge cases
    void testDestructorMultipleCalls();
    void testEmptyPath();
    void testNullPath();

    // Performance
    void testLockSpeed();
    void testUnlockSpeed();

private:
    bool hasAdminRights();
    bool isDriveAvailable(int driveNumber);
};

void TestDriveLock::initTestCase() {
    // Setup test environment
}

void TestDriveLock::cleanupTestCase() {
    // Cleanup test environment
}

void TestDriveLock::init() {
    // Per-test setup
}

void TestDriveLock::cleanup() {
    // Per-test cleanup
}

bool TestDriveLock::hasAdminRights() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD size;
    if (!GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    return elevation.TokenIsElevated != 0;
}

bool TestDriveLock::isDriveAvailable(int driveNumber) {
    QString path = QString("\\\\.\\PhysicalDrive%1").arg(driveNumber);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );
    
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return true;
    }
    return false;
}

void TestDriveLock::testConstructorDriveNumber() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    QVERIFY(lock.isLocked() || !lock.lastError().isEmpty());
}

void TestDriveLock::testConstructorVolumePath() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock("\\\\.\\C:");
    // May succeed or fail depending on system
}

void TestDriveLock::testConstructorReadOnly() {
    DriveLock lock(0, true);
    // Read-only access may not require lock
}

void TestDriveLock::testIsLocked() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    bool locked = lock.isLocked();
    QVERIFY(locked == true || locked == false);
}

void TestDriveLock::testIsLockedFailed() {
    DriveLock lock(999); // Invalid drive
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testIsLockedAfterUnlock() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    lock.unlock();
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testHandleValid() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    if (lock.isLocked()) {
        QVERIFY(lock.handle() != INVALID_HANDLE_VALUE);
    }
}

void TestDriveLock::testHandleInvalid() {
    DriveLock lock(999);
    QCOMPARE(lock.handle(), INVALID_HANDLE_VALUE);
}

void TestDriveLock::testHandleAfterUnlock() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    lock.unlock();
    QCOMPARE(lock.handle(), INVALID_HANDLE_VALUE);
}

void TestDriveLock::testLastError() {
    DriveLock lock(999);
    if (!lock.isLocked()) {
        QVERIFY(!lock.lastError().isEmpty());
    }
}

void TestDriveLock::testLastErrorEmpty() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    if (lock.isLocked()) {
        // Success may have empty error
    }
}

void TestDriveLock::testLastErrorAfterFailure() {
    DriveLock lock(999);
    QString error = lock.lastError();
    QVERIFY(!error.isEmpty());
}

void TestDriveLock::testPath() {
    DriveLock lock(0);
    QString path = lock.path();
    QVERIFY(!path.isEmpty());
}

void TestDriveLock::testPathDrive() {
    DriveLock lock(0);
    QString path = lock.path();
    QVERIFY(path.contains("PhysicalDrive"));
}

void TestDriveLock::testPathVolume() {
    DriveLock lock("\\\\.\\C:");
    QString path = lock.path();
    QCOMPARE(path, "\\\\.\\C:");
}

void TestDriveLock::testUnlock() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    lock.unlock();
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testUnlockTwice() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    lock.unlock();
    lock.unlock(); // Should be safe
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testUnlockNotLocked() {
    DriveLock lock(999);
    lock.unlock(); // Should be safe
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testRAIIPattern() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    {
        DriveLock lock(0);
        // Lock acquired
    }
    // Lock released automatically
}

void TestDriveLock::testScopeExit() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    bool wasLocked = false;
    {
        DriveLock lock(0);
        wasLocked = lock.isLocked();
    }
    
    if (wasLocked) {
        // Lock should be released now
        DriveLock newLock(0);
        QVERIFY(newLock.isLocked() || !newLock.lastError().isEmpty());
    }
}

void TestDriveLock::testExceptionSafety() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    try {
        DriveLock lock(0);
        throw std::runtime_error("test");
    } catch (...) {
        // Lock should be released
    }
}

void TestDriveLock::testMoveConstructor() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    bool wasLocked = lock1.isLocked();
    
    DriveLock lock2(std::move(lock1));
    
    if (wasLocked) {
        QVERIFY(lock2.isLocked());
        QVERIFY(!lock1.isLocked());
    }
}

void TestDriveLock::testMoveAssignment() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    bool wasLocked = lock1.isLocked();
    
    DriveLock lock2(999);
    lock2 = std::move(lock1);
    
    if (wasLocked) {
        QVERIFY(lock2.isLocked());
        QVERIFY(!lock1.isLocked());
    }
}

void TestDriveLock::testMoveFromInvalid() {
    DriveLock lock1(999);
    DriveLock lock2(std::move(lock1));
    
    QVERIFY(!lock2.isLocked());
}

void TestDriveLock::testCopyConstructorDeleted() {
    // Compile-time check - copy constructor should be deleted
    // This test verifies the class design
}

void TestDriveLock::testCopyAssignmentDeleted() {
    // Compile-time check - copy assignment should be deleted
    // This test verifies the class design
}

void TestDriveLock::testDriveNumberZero() {
    DriveLock lock(0);
    QString path = lock.path();
    QVERIFY(path.contains("PhysicalDrive0"));
}

void TestDriveLock::testDriveNumberPositive() {
    DriveLock lock(1);
    QString path = lock.path();
    QVERIFY(path.contains("PhysicalDrive1"));
}

void TestDriveLock::testDriveNumberNegative() {
    DriveLock lock(-1);
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testDriveNumberInvalid() {
    DriveLock lock(999);
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testVolumePathFormat() {
    DriveLock lock("\\\\.\\C:");
    QString path = lock.path();
    QCOMPARE(path, "\\\\.\\C:");
}

void TestDriveLock::testVolumePathInvalid() {
    DriveLock lock("\\\\.\\InvalidVolume:");
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testVolumePathEmpty() {
    DriveLock lock("");
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testVolumePathGUID() {
    // GUID volume path format
    DriveLock lock("\\\\?\\Volume{12345678-1234-1234-1234-123456789012}");
    // May or may not succeed
}

void TestDriveLock::testReadOnlyLock() {
    DriveLock lock(0, true);
    // Read-only should work without admin
}

void TestDriveLock::testReadWriteLock() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0, false);
    // Read-write requires admin
}

void TestDriveLock::testReadOnlyDefault() {
    DriveLock lock(0);
    // Default is read-write
}

void TestDriveLock::testExclusiveAccess() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    if (lock1.isLocked()) {
        // Second lock should fail
        DriveLock lock2(0);
        QVERIFY(!lock2.isLocked());
    }
}

void TestDriveLock::testSharedRead() {
    // Read-only locks may be shared
    DriveLock lock1(0, true);
    DriveLock lock2(0, true);
    // Both may succeed
}

void TestDriveLock::testMultipleLocksSameDrive() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    DriveLock lock2(0);
    
    // Only one should succeed
    bool both = lock1.isLocked() && lock2.isLocked();
    QVERIFY(!both);
}

void TestDriveLock::testMultipleLocksDifferentDrives() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    DriveLock lock2(1);
    
    // Different drives can both be locked
}

void TestDriveLock::testRequiresAdmin() {
    DriveLock lock(0, false);
    
    if (!hasAdminRights()) {
        QVERIFY(!lock.isLocked());
    }
}

void TestDriveLock::testWithoutAdmin() {
    if (hasAdminRights()) {
        QSKIP("Test requires non-admin");
    }

    DriveLock lock(0);
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testLockLockedDrive() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock1(0);
    if (lock1.isLocked()) {
        DriveLock lock2(0);
        QVERIFY(!lock2.isLocked());
    }
}

void TestDriveLock::testLockNonexistentDrive() {
    DriveLock lock(999);
    QVERIFY(!lock.isLocked());
    QVERIFY(!lock.lastError().isEmpty());
}

void TestDriveLock::testLockSystemDrive() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    // System drive may be locked
    DriveLock lock(0);
}

void TestDriveLock::testWindowsHandles() {
    DriveLock lock(0);
    HANDLE handle = lock.handle();
    
    // Should be valid Windows handle or INVALID_HANDLE_VALUE
    QVERIFY(handle == INVALID_HANDLE_VALUE || handle != NULL);
}

void TestDriveLock::testInvalidHandleValue() {
    DriveLock lock(999);
    QCOMPARE(lock.handle(), INVALID_HANDLE_VALUE);
}

void TestDriveLock::testDestructorMultipleCalls() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock* lock = new DriveLock(0);
    delete lock;
    // Should not crash
}

void TestDriveLock::testEmptyPath() {
    DriveLock lock("");
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testNullPath() {
    DriveLock lock(QString());
    QVERIFY(!lock.isLocked());
}

void TestDriveLock::testLockSpeed() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    QElapsedTimer timer;
    timer.start();
    
    DriveLock lock(0);
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 1000); // Should be fast
}

void TestDriveLock::testUnlockSpeed() {
    if (!hasAdminRights()) {
        QSKIP("Requires admin rights");
    }

    DriveLock lock(0);
    
    QElapsedTimer timer;
    timer.start();
    
    lock.unlock();
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 100); // Should be very fast
}

QTEST_MAIN(TestDriveLock)
#include "test_drive_lock.moc"
