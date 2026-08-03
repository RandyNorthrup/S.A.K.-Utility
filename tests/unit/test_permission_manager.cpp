// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_permission_manager.cpp
/// @brief Unit tests for ACL/permission management

#include "sak/permission_manager.h"
#include "sak/user_profile_types.h"

#include <QDir>
#include <QFile>
#include <QProcess>
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

    // B5 security regressions (ACL round-trips on a user-owned temp file)
    void stripPermissions_doesNotProduceNullDacl();
    void setStandardUser_keepsSystemAndAdmins();
    void setSddl_ownerlessDoesNotNullOwner();

    // Reparse-point (junction/symlink) redirect hardening
    void stripPermissions_refusesReparsePoint();

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
// B5 security regressions
// ============================================================================

// B5-01: stripPermissions must NOT set a NULL DACL (which grants everyone full
// access). A user owns their temp file, so the DACL edit succeeds without
// elevation, and the resulting SDDL must not be a null DACL.
void PermissionManagerTests::stripPermissions_doesNotProduceNullDacl() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = m_tempDir.filePath("strip_nulldacl.txt");
    QFile file(f);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    QVERIFY2(mgr.stripPermissions(f), qPrintable(mgr.getLastError()));
    const QString sddl = mgr.getSecurityDescriptorSddl(f);
    QVERIFY(!sddl.isEmpty());
    // A NULL DACL serializes as "NO_ACCESS_CONTROL"; it must be absent.
    QVERIFY2(!sddl.contains("NO_ACCESS_CONTROL"),
             qPrintable(QStringLiteral("strip produced a null DACL: %1").arg(sddl)));
    // Inheritance must be re-enabled (unprotected), i.e. not a protected "D:P".
    QVERIFY2(!sddl.contains(QStringLiteral("D:P")),
             qPrintable(QStringLiteral("strip left a protected DACL: %1").arg(sddl)));
#else
    QVERIFY(true);
#endif
}

// B5-02: a "standard user" ACL must keep SYSTEM and Administrators, or the OS
// and admins lose all access to the file.
void PermissionManagerTests::setStandardUser_keepsSystemAndAdmins() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = m_tempDir.filePath("standarduser.txt");
    QFile file(f);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());

    QVERIFY2(mgr.setStandardUserPermissions(f, ownerSid), qPrintable(mgr.getLastError()));
    const QString sddl = mgr.getSecurityDescriptorSddl(f);
    QVERIFY(!sddl.isEmpty());
    // SYSTEM -> "SY", local Administrators -> "BA" in SDDL.
    QVERIFY2(sddl.contains(QStringLiteral(";SY)")),
             qPrintable(QStringLiteral("SYSTEM missing from ACL: %1").arg(sddl)));
    QVERIFY2(sddl.contains(QStringLiteral(";BA)")),
             qPrintable(QStringLiteral("Administrators missing from ACL: %1").arg(sddl)));
#else
    QVERIFY(true);
#endif
}

// B5-03: applying an SDDL that specifies ONLY a DACL must not null the owner.
void PermissionManagerTests::setSddl_ownerlessDoesNotNullOwner() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = m_tempDir.filePath("sddl_ownerless.txt");
    QFile file(f);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    const QString ownerBefore = mgr.getOwner(f);
    QVERIFY(!ownerBefore.isEmpty());

    // DACL-only SDDL (no O: / G:), granting the current user full access.
    const QString dacl = QStringLiteral("D:(A;;FA;;;%1)").arg(ownerBefore);
    QVERIFY2(mgr.setSecurityDescriptorSddl(f, dacl), qPrintable(mgr.getLastError()));

    const QString ownerAfter = mgr.getOwner(f);
    QCOMPARE(ownerAfter, ownerBefore);
#else
    QVERIFY(true);
#endif
}

// A junction/symlink target must be REFUSED, never followed, so an attacker who
// swaps a benign path for a reparse point cannot redirect an elevated ACL change
// onto another object. The no-follow handle detects the reparse and fails closed.
void PermissionManagerTests::stripPermissions_refusesReparsePoint() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString target = m_tempDir.filePath("reparse_target");
    QVERIFY(QDir().mkpath(target));
    const QString link = m_tempDir.filePath("reparse_link");

    QProcess proc;
    proc.start(
        "cmd",
        {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    QVERIFY(proc.waitForFinished(10'000));
    if (proc.exitCode() != 0) {
        QSKIP("Could not create a directory junction (non-NTFS or policy)");
    }

    const auto result = mgr.tryStripPermissions(link);
    QVERIFY2(!result.has_value(), "strip must refuse a reparse point, not follow it");
    QVERIFY2(mgr.getLastError().contains("reparse", Qt::CaseInsensitive),
             qPrintable(mgr.getLastError()));
#else
    QVERIFY(true);
#endif
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
