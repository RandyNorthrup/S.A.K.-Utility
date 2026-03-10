// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_permission_manager.cpp
/// @brief Unit tests for ACL/permission management

#include "sak/permission_manager.h"
#include "sak/user_profile_types.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class PermissionManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Constructor
    void constructor_defaults();

    // canModifyPermissions
    void canModifyPermissions_existingFile();
    void canModifyPermissions_nonExistentFile();

    // getOwner
    void getOwner_existingFile();
    void getOwner_nonExistentFile();

    // Security descriptor
    void getSecurityDescriptorSddl_existingFile();
    void getSecurityDescriptorSddl_nonExistentFile();

    // stripPermissions
    void stripPermissions_existingFile();

    // applyPermissionStrategy
    void applyStrategy_stripAll();

    // isRunningAsAdmin
    void isRunningAsAdmin_returnsBoolean();

    // getLastError
    void getLastError_initiallyEmpty();

private:
    QTemporaryDir m_tempDir;
    QString m_testFile;
};

void PermissionManagerTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_testFile = m_tempDir.filePath("perm_test.txt");

    QFile f(m_testFile);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("permission test data");
    f.close();
}

// ============================================================================
// Constructor
// ============================================================================

void PermissionManagerTests::constructor_defaults() {
    sak::PermissionManager mgr;
    QVERIFY(true);  // No crash
}

// ============================================================================
// canModifyPermissions
// ============================================================================

void PermissionManagerTests::canModifyPermissions_existingFile() {
    sak::PermissionManager mgr;
    // May or may not return true depending on elevation status
    bool canModify = mgr.canModifyPermissions(m_testFile);
    Q_UNUSED(canModify);  // Just verify no crash
    QVERIFY(true);
}

void PermissionManagerTests::canModifyPermissions_nonExistentFile() {
    sak::PermissionManager mgr;
    bool canModify = mgr.canModifyPermissions(m_tempDir.filePath("nonexistent.txt"));
    QVERIFY(!canModify);
}

// ============================================================================
// getOwner
// ============================================================================

void PermissionManagerTests::getOwner_existingFile() {
    sak::PermissionManager mgr;
    QString owner = mgr.getOwner(m_testFile);
    // Should return a SID or username string
    QVERIFY(!owner.isEmpty());
}

void PermissionManagerTests::getOwner_nonExistentFile() {
    sak::PermissionManager mgr;
    QString owner = mgr.getOwner(m_tempDir.filePath("nonexistent.txt"));
    QVERIFY(owner.isEmpty());
}

// ============================================================================
// Security Descriptor
// ============================================================================

void PermissionManagerTests::getSecurityDescriptorSddl_existingFile() {
    sak::PermissionManager mgr;
    QString sddl = mgr.getSecurityDescriptorSddl(m_testFile);
    // SDDL string should be non-empty for a file we own
    QVERIFY(!sddl.isEmpty());
    // SDDL strings typically start with D: or O:
    QVERIFY(sddl.contains("D:") || sddl.contains("O:"));
}

void PermissionManagerTests::getSecurityDescriptorSddl_nonExistentFile() {
    sak::PermissionManager mgr;
    QString sddl = mgr.getSecurityDescriptorSddl(m_tempDir.filePath("nonexistent.txt"));
    QVERIFY(sddl.isEmpty());
}

// ============================================================================
// stripPermissions
// ============================================================================

void PermissionManagerTests::stripPermissions_existingFile() {
    sak::PermissionManager mgr;
    // Create a separate file for this test to avoid affecting other tests
    QString stripFile = m_tempDir.filePath("strip_test.txt");
    QFile f(stripFile);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("strip test");
    f.close();

    bool result = mgr.stripPermissions(stripFile);
    // May require elevation; just verify no crash
    Q_UNUSED(result);
    QVERIFY(true);
}

// ============================================================================
// applyPermissionStrategy
// ============================================================================

void PermissionManagerTests::applyStrategy_stripAll() {
    sak::PermissionManager mgr;
    QString stratFile = m_tempDir.filePath("strategy_test.txt");
    QFile f(stratFile);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("strategy test");
    f.close();

    bool result = mgr.applyPermissionStrategy(stratFile, sak::PermissionMode::StripAll);
    // Result depends on privileges
    Q_UNUSED(result);
    QVERIFY(true);
}

// ============================================================================
// isRunningAsAdmin
// ============================================================================

void PermissionManagerTests::isRunningAsAdmin_returnsBoolean() {
    bool isAdmin = sak::PermissionManager::isRunningAsAdmin();
    Q_UNUSED(isAdmin);
    QVERIFY(true);  // Just verify it returns without crashing
}

// ============================================================================
// getLastError
// ============================================================================

void PermissionManagerTests::getLastError_initiallyEmpty() {
    sak::PermissionManager mgr;
    // On a fresh instance, last error should be empty
    QVERIFY(mgr.getLastError().isEmpty());
}

QTEST_GUILESS_MAIN(PermissionManagerTests)
#include "test_permission_manager.moc"
