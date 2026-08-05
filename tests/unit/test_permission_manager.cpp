// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_permission_manager.cpp
/// @brief Unit tests for ACL/permission management

#include "sak/elevation_manager.h"
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

    // canModifyPermissions
    void canModifyPermissions_existingFile();
    void canModifyPermissions_nonExistentFile();

    // getOwner
    void getOwner_existingFile();
    void getOwner_nonExistentFile();

    // Security descriptor
    void getSecurityDescriptorSddl_existingFile();
    void getSecurityDescriptorSddl_nonExistentFile();

    // B5 security regressions (ACL round-trips on a user-owned temp file)
    void stripPermissions_doesNotProduceNullDacl();
    void setStandardUser_keepsSystemAndAdmins();
    void setSddl_ownerlessDoesNotNullOwner();

    // Reparse-point (junction/symlink) redirect hardening
    void stripPermissions_refusesReparsePoint();
    void stripPermissions_refusesAncestorJunction();

    // SDDL round-trip must not silently drop the primary group
    void setSddl_appliesGroup();

    // applyPermissionStrategy

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

// constructor_defaults was removed here. Its body was "construct, QVERIFY(true)", and the only
// state a freshly constructed manager exposes is getLastError(), which getLastError_initiallyEmpty
// already asserts is empty. Every other test in this file constructs a manager too, so the
// "does not crash on construction" claim is not lost.

// ============================================================================
// canModifyPermissions
// ============================================================================

void PermissionManagerTests::canModifyPermissions_existingFile() {
    sak::PermissionManager mgr;
    // canModifyPermissions is exists() && isWritable() -- it does NOT consult elevation, so a
    // file this process just created in its own temp dir must come back true. The old comment
    // claimed the answer depended on elevation and the body asserted nothing at all, which made
    // the negative case (canModifyPermissions_nonExistentFile) the only real coverage.
    QVERIFY2(mgr.canModifyPermissions(m_testFile), qPrintable(m_testFile));
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
    const QString owner = mgr.getOwner(m_testFile);
    // getOwner runs the owner SID through ConvertSidToStringSidW, so the result is always the
    // textual SID form "S-1-...". Asserting only "non-empty" accepted any junk string, e.g. an
    // error message accidentally returned in place of the SID.
    QVERIFY2(owner.startsWith(QLatin1String("S-1-")), qPrintable(owner));
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
    const QString sddl = mgr.getSecurityDescriptorSddl(m_testFile);
    // The call requests OWNER | GROUP | DACL, and every file on a security-aware volume has an
    // owner and a DACL, so BOTH fields must be present and the owner must come first (SDDL field
    // order is O: G: D: S:). The old disjunction passed when either half went missing.
    QVERIFY(!sddl.isEmpty());
    QVERIFY2(sddl.startsWith(QLatin1String("O:")), qPrintable(sddl));
    QVERIFY2(sddl.contains(QLatin1String("D:")), qPrintable(sddl));
}

void PermissionManagerTests::getSecurityDescriptorSddl_nonExistentFile() {
    sak::PermissionManager mgr;
    QString sddl = mgr.getSecurityDescriptorSddl(m_tempDir.filePath("nonexistent.txt"));
    QVERIFY(sddl.isEmpty());
}

// ============================================================================
// stripPermissions
// ============================================================================

// stripPermissions_existingFile was removed here. It created a temp file, called
// stripPermissions, Q_UNUSED'd the result and ended in QVERIFY(true) -- a strictly weaker
// duplicate of stripPermissions_doesNotProduceNullDacl below, which performs the same call on
// the same kind of user-owned temp file but asserts the return value AND the resulting SDDL.

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

// An ANCESTOR-directory junction/symlink must also be refused: FILE_FLAG_OPEN_
// REPARSE_POINT only guards the FINAL component, so the kernel still traverses an
// intermediate junction. Opening base\file.txt THROUGH a junction that replaces an
// ancestor dir must be caught by verifying the handle's resolved path.
void PermissionManagerTests::stripPermissions_refusesAncestorJunction() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString base = m_tempDir.filePath("anc_base");
    QVERIFY(QDir().mkpath(base));
    const QString realFile = base + "\\file.txt";
    QFile ff(realFile);
    QVERIFY(ff.open(QIODevice::WriteOnly));
    ff.write("x");
    ff.close();

    const QString link = m_tempDir.filePath("anc_link");
    QProcess proc;
    proc.start(
        "cmd",
        {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(base)});
    QVERIFY(proc.waitForFinished(10'000));
    if (proc.exitCode() != 0) {
        QSKIP("Could not create a directory junction (non-NTFS or policy)");
    }

    // Reach the (non-reparse) file THROUGH the junction ancestor.
    const auto result = mgr.tryStripPermissions(link + "\\file.txt");
    QVERIFY2(!result.has_value(), "must refuse a path redirected through an ancestor junction");
    const QString err = mgr.getLastError();
    QVERIFY2(err.contains("redirect", Qt::CaseInsensitive) ||
                 err.contains("junction", Qt::CaseInsensitive) ||
                 err.contains("reparse", Qt::CaseInsensitive),
             qPrintable(err));
#else
    QVERIFY(true);
#endif
}

// B-fix: applyParsedSecurityDescriptor previously applied only OWNER and DACL, so a
// save-then-restore round-trip silently DROPPED the primary group. Setting an SDDL
// that specifies a group must actually apply it.
void PermissionManagerTests::setSddl_appliesGroup() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = m_tempDir.filePath("sddl_group.txt");
    QFile file(f);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();

    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());

    // Set owner AND group to the current user's SID (the owner may set its own token
    // SID as the primary group). Before the fix, the G: field was ignored on apply.
    const QString sddl = QStringLiteral("O:%1G:%1D:(A;;FA;;;%1)").arg(ownerSid);
    if (!mgr.setSecurityDescriptorSddl(f, sddl)) {
        QSKIP("Environment does not permit setting the primary group");
    }
    const QString out = mgr.getSecurityDescriptorSddl(f);
    QVERIFY(!out.isEmpty());
    QVERIFY2(out.contains(QStringLiteral("G:%1").arg(ownerSid)),
             qPrintable(QStringLiteral("primary group was not applied on restore: %1").arg(out)));
#else
    QVERIFY(true);
#endif
}

// applyPermissionStrategy and its applyStrategy_stripAll test were removed together. The
// function had no production caller anywhere in the tree - both the backup and the restore
// worker dispatch on PermissionMode themselves - so this test was the only thing keeping it
// compiled, and it asserted nothing: it called the function, discarded the result with
// Q_UNUSED because "result depends on privileges", and ended in QVERIFY(true). A test that
// cannot fail does not make dead code live.

// ============================================================================
// isRunningAsAdmin
// ============================================================================

void PermissionManagerTests::isRunningAsAdmin_returnsBoolean() {
    // The whole contract of isRunningAsAdmin is that it reports the SAME elevation state as the
    // canonical token check, ElevationManager::isElevated(); every elevation gate in the manager
    // (tryTakeOwnership) branches on it. The old body discarded the result and asserted
    // QVERIFY(true), which a hardcoded "return false" would have passed -- silently turning
    // every elevated operation into elevation_required.
#ifdef Q_OS_WIN
    QCOMPARE(sak::PermissionManager::isRunningAsAdmin(), sak::ElevationManager::isElevated());
#else
    QSKIP("Elevation state is a Windows token property");
#endif
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
