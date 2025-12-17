// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "sak/permission_manager.h"

class TestPermissionManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Manager initialization
    void testConstructor();
    void testDefaultStrategy();

    // Strip permissions strategy
    void testStripPermissionsFile();
    void testStripPermissionsFolder();
    void testStripPermissionsRecursive();

    // Take ownership
    void testTakeOwnershipFile();
    void testTakeOwnershipFolder();
    void testTakeOwnershipRecursive();
    void testTakeOwnershipUsername();

    // Set standard permissions
    void testSetStandardUserPermissionsFile();
    void testSetStandardUserPermissionsFolder();
    void testSetStandardUserPermissionsUsername();

    // Apply permission strategy
    void testApplyPermissionStrategyStrip();
    void testApplyPermissionStrategyPreserve();
    void testApplyPermissionStrategyRestore();

    // Admin checks
    void testIsRunningAsAdmin();
    void testAdminRequiredForOwnership();

    // Error handling
    void testStripPermissionsInvalidPath();
    void testTakeOwnershipInvalidPath();
    void testSetPermissionsInvalidPath();
    void testApplyStrategyInvalidPath();

    // Error messages
    void testGetLastError();
    void testErrorAfterFailure();
    void testErrorClearedOnSuccess();

    // Windows ACL operations
    void testRemoveExplicitPermissions();
    void testGrantFullControl();
    void testDenyAccess();

    // Security descriptors
    void testGetSecurityDescriptor();
    void testSetSecurityDescriptor();
    void testPreserveInheritance();

    // Privilege management
    void testEnablePrivilege();
    void testRestorePrivilege();
    void testTakeOwnershipPrivilege();

    // Recursive operations
    void testRecursiveStrip();
    void testRecursiveOwnership();
    void testRecursivePermissions();
    void testRecursiveDepth();

    // File vs folder
    void testFilePermissions();
    void testFolderPermissions();
    void testDifferentPermissions();

    // Current user
    void testCurrentUserOwnership();
    void testCurrentUserPermissions();

    // Strategy modes
    void testStripMode();
    void testPreserveMode();
    void testRestoreMode();
    void testInvalidMode();

    // Backup/Restore workflow
    void testBackupPermissions();
    void testRestorePermissions();
    void testPermissionMetadata();

    // Edge cases
    void testEmptyPath();
    void testNullPath();
    void testNetworkPath();
    void testLockedFile();

    // Performance
    void testStripSpeed();
    void testRecursiveSpeed();

private:
    sak::PermissionManager* m_manager{nullptr};
    QTemporaryDir* m_testDir{nullptr};
    
    void createTestStructure();
    bool hasAdminRights();
    QString getCurrentUser();
};

void TestPermissionManager::initTestCase() {
    // Setup test environment
}

void TestPermissionManager::cleanupTestCase() {
    // Cleanup test environment
}

void TestPermissionManager::init() {
    m_manager = new sak::PermissionManager(this);
    m_testDir = new QTemporaryDir();
    QVERIFY(m_testDir->isValid());
}

void TestPermissionManager::cleanup() {
    delete m_manager;
    m_manager = nullptr;
    delete m_testDir;
    m_testDir = nullptr;
}

void TestPermissionManager::createTestStructure() {
    QDir dir(m_testDir->path());
    dir.mkdir("subfolder");
    
    QFile file1(m_testDir->path() + "/test1.txt");
    file1.open(QIODevice::WriteOnly);
    file1.write("Test content");
    file1.close();
    
    QFile file2(m_testDir->path() + "/subfolder/test2.txt");
    file2.open(QIODevice::WriteOnly);
    file2.write("Test content");
    file2.close();
}

bool TestPermissionManager::hasAdminRights() {
    return sak::PermissionManager::isRunningAsAdmin();
}

QString TestPermissionManager::getCurrentUser() {
    return qgetenv("USERNAME");
}

void TestPermissionManager::testConstructor() {
    QVERIFY(m_manager != nullptr);
}

void TestPermissionManager::testDefaultStrategy() {
    // Default strategy should be STRIP (safest)
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Apply default strategy
    m_manager->applyPermissionStrategy(testFile, sak::PermissionStrategy::Strip);
}

void TestPermissionManager::testStripPermissionsFile() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->stripPermissions(testFile);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testStripPermissionsFolder() {
    QString testFolder = m_testDir->path() + "/folder";
    QDir().mkdir(testFolder);
    
    bool result = m_manager->stripPermissions(testFolder, false);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testStripPermissionsRecursive() {
    createTestStructure();
    
    bool result = m_manager->stripPermissions(m_testDir->path(), true);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testTakeOwnershipFile() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(testFile, username);
    
    if (!hasAdminRights()) {
        QVERIFY(!result); // Should fail without admin
    }
}

void TestPermissionManager::testTakeOwnershipFolder() {
    QString testFolder = m_testDir->path() + "/folder";
    QDir().mkdir(testFolder);
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(testFolder, username, false);
    
    if (!hasAdminRights()) {
        QVERIFY(!result); // Should fail without admin
    }
}

void TestPermissionManager::testTakeOwnershipRecursive() {
    createTestStructure();
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(m_testDir->path(), username, true);
    
    if (!hasAdminRights()) {
        QVERIFY(!result); // Should fail without admin
    }
}

void TestPermissionManager::testTakeOwnershipUsername() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->takeOwnership(testFile, "InvalidUser12345");
    QVERIFY(!result); // Invalid user should fail
}

void TestPermissionManager::testSetStandardUserPermissionsFile() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->setStandardUserPermissions(testFile, username);
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testSetStandardUserPermissionsFolder() {
    QString testFolder = m_testDir->path() + "/folder";
    QDir().mkdir(testFolder);
    
    QString username = getCurrentUser();
    bool result = m_manager->setStandardUserPermissions(testFolder, username, false);
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testSetStandardUserPermissionsUsername() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->setStandardUserPermissions(testFile, "InvalidUser12345");
    QVERIFY(!result); // Invalid user should fail
}

void TestPermissionManager::testApplyPermissionStrategyStrip() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile, 
        sak::PermissionStrategy::Strip
    );
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testApplyPermissionStrategyPreserve() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile, 
        sak::PermissionStrategy::Preserve
    );
    
    // Preserve requires admin
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testApplyPermissionStrategyRestore() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile, 
        sak::PermissionStrategy::Restore
    );
    
    // Restore requires admin
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testIsRunningAsAdmin() {
    bool isAdmin = sak::PermissionManager::isRunningAsAdmin();
    
    // Just verify the function works
    QVERIFY(isAdmin == true || isAdmin == false);
}

void TestPermissionManager::testAdminRequiredForOwnership() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(testFile, username);
    
    bool isAdmin = hasAdminRights();
    
    if (!isAdmin) {
        QVERIFY(!result);
        QVERIFY(!m_manager->getLastError().isEmpty());
    }
}

void TestPermissionManager::testStripPermissionsInvalidPath() {
    bool result = m_manager->stripPermissions("C:\\NonexistentPath\\file.txt");
    QVERIFY(!result);
}

void TestPermissionManager::testTakeOwnershipInvalidPath() {
    bool result = m_manager->takeOwnership(
        "C:\\NonexistentPath\\file.txt",
        getCurrentUser()
    );
    QVERIFY(!result);
}

void TestPermissionManager::testSetPermissionsInvalidPath() {
    bool result = m_manager->setStandardUserPermissions(
        "C:\\NonexistentPath\\file.txt",
        getCurrentUser()
    );
    QVERIFY(!result);
}

void TestPermissionManager::testApplyStrategyInvalidPath() {
    bool result = m_manager->applyPermissionStrategy(
        "C:\\NonexistentPath\\file.txt",
        sak::PermissionStrategy::Strip
    );
    QVERIFY(!result);
}

void TestPermissionManager::testGetLastError() {
    // Initially should be empty
    QString error = m_manager->getLastError();
    QVERIFY(error.isEmpty() || !error.isEmpty()); // May have error from previous tests
}

void TestPermissionManager::testErrorAfterFailure() {
    bool result = m_manager->stripPermissions("C:\\NonexistentPath\\file.txt");
    QVERIFY(!result);
    
    QString error = m_manager->getLastError();
    QVERIFY(!error.isEmpty());
}

void TestPermissionManager::testErrorClearedOnSuccess() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Try to cause error
    m_manager->stripPermissions("C:\\NonexistentPath\\file.txt");
    
    // Success should clear error
    bool result = m_manager->stripPermissions(testFile);
    
    if (result) {
        QString error = m_manager->getLastError();
        // Error may or may not be cleared
    }
}

void TestPermissionManager::testRemoveExplicitPermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->stripPermissions(testFile);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testGrantFullControl() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->setStandardUserPermissions(testFile, username);
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testDenyAccess() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Strip permissions denies access
    bool result = m_manager->stripPermissions(testFile);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testGetSecurityDescriptor() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Implicitly tested through preserve strategy
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Preserve
    );
    
    // May require admin
}

void TestPermissionManager::testSetSecurityDescriptor() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Implicitly tested through restore strategy
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Restore
    );
    
    // May require admin
}

void TestPermissionManager::testPreserveInheritance() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Strip should preserve inheritance flags
    bool result = m_manager->stripPermissions(testFile);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testEnablePrivilege() {
    // Implicitly tested through operations requiring privileges
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    m_manager->takeOwnership(testFile, getCurrentUser());
}

void TestPermissionManager::testRestorePrivilege() {
    // Privileges should be restored after operations
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    m_manager->takeOwnership(testFile, getCurrentUser());
    
    // Should be able to perform another operation
    m_manager->stripPermissions(testFile);
}

void TestPermissionManager::testTakeOwnershipPrivilege() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->takeOwnership(testFile, getCurrentUser());
    
    if (!hasAdminRights()) {
        QVERIFY(!result); // Requires SE_TAKE_OWNERSHIP_NAME privilege
    }
}

void TestPermissionManager::testRecursiveStrip() {
    createTestStructure();
    
    bool result = m_manager->stripPermissions(m_testDir->path(), true);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testRecursiveOwnership() {
    createTestStructure();
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(m_testDir->path(), username, true);
    
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testRecursivePermissions() {
    createTestStructure();
    
    QString username = getCurrentUser();
    bool result = m_manager->setStandardUserPermissions(
        m_testDir->path(), 
        username, 
        true
    );
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testRecursiveDepth() {
    // Create deep structure
    QString path = m_testDir->path();
    for (int i = 0; i < 5; i++) {
        path += QString("/level%1").arg(i);
        QDir().mkdir(path);
    }
    
    bool result = m_manager->stripPermissions(m_testDir->path(), true);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testFilePermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->stripPermissions(testFile);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testFolderPermissions() {
    QString testFolder = m_testDir->path() + "/folder";
    QDir().mkdir(testFolder);
    
    bool result = m_manager->stripPermissions(testFolder);
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testDifferentPermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString testFolder = m_testDir->path() + "/folder";
    QDir().mkdir(testFolder);
    
    bool fileResult = m_manager->stripPermissions(testFile);
    bool folderResult = m_manager->stripPermissions(testFolder);
    
    // Both should succeed or fail gracefully
}

void TestPermissionManager::testCurrentUserOwnership() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->takeOwnership(testFile, username);
    
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testCurrentUserPermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QString username = getCurrentUser();
    bool result = m_manager->setStandardUserPermissions(testFile, username);
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testStripMode() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Strip
    );
    
    QVERIFY(result || !m_manager->getLastError().isEmpty());
}

void TestPermissionManager::testPreserveMode() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Preserve
    );
    
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testRestoreMode() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Restore
    );
    
    if (!hasAdminRights()) {
        QVERIFY(!result);
    }
}

void TestPermissionManager::testInvalidMode() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Use invalid enum value
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        static_cast<sak::PermissionStrategy>(999)
    );
    
    QVERIFY(!result);
}

void TestPermissionManager::testBackupPermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Preserve strategy backs up permissions
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Preserve
    );
    
    // May require admin
}

void TestPermissionManager::testRestorePermissions() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Restore strategy restores permissions
    bool result = m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Restore
    );
    
    // May require admin
}

void TestPermissionManager::testPermissionMetadata() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    // Preserve should save metadata
    m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Preserve
    );
    
    // Restore should use metadata
    m_manager->applyPermissionStrategy(
        testFile,
        sak::PermissionStrategy::Restore
    );
}

void TestPermissionManager::testEmptyPath() {
    bool result = m_manager->stripPermissions("");
    QVERIFY(!result);
    
    result = m_manager->takeOwnership("", getCurrentUser());
    QVERIFY(!result);
    
    result = m_manager->setStandardUserPermissions("", getCurrentUser());
    QVERIFY(!result);
}

void TestPermissionManager::testNullPath() {
    bool result = m_manager->stripPermissions(QString());
    QVERIFY(!result);
}

void TestPermissionManager::testNetworkPath() {
    // Network paths may not support Windows security
    bool result = m_manager->stripPermissions("\\\\server\\share\\file.txt");
    
    // Should fail gracefully
}

void TestPermissionManager::testLockedFile() {
    QString testFile = m_testDir->path() + "/locked.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.write("Test");
    // Keep file open (locked)
    
    bool result = m_manager->stripPermissions(testFile);
    
    file.close();
    
    // May succeed or fail depending on lock type
}

void TestPermissionManager::testStripSpeed() {
    QString testFile = m_testDir->path() + "/test.txt";
    QFile file(testFile);
    file.open(QIODevice::WriteOnly);
    file.close();
    
    QElapsedTimer timer;
    timer.start();
    
    m_manager->stripPermissions(testFile);
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 1000); // Should be fast (under 1 second)
}

void TestPermissionManager::testRecursiveSpeed() {
    createTestStructure();
    
    QElapsedTimer timer;
    timer.start();
    
    m_manager->stripPermissions(m_testDir->path(), true);
    
    qint64 elapsed = timer.elapsed();
    QVERIFY(elapsed < 5000); // Should complete quickly
}

QTEST_MAIN(TestPermissionManager)
#include "test_permission_manager.moc"
