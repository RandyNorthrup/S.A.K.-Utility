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
    void stripPermissions_refusesHardLink();  // R5-G10-9

    // SDDL round-trip must not silently drop the primary group
    void setSddl_appliesGroup();

    // R5-G10-9: SDDL apply must fail closed on a dangerous/malformed descriptor
    void setSddl_refusesPresentNullDacl();
    void setSddl_refusesSacl();
    void setSddl_refusesEmbeddedNul();

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

// canModifyPermissions and its two tests were removed together. The function had no production
// caller, so these tests were the only thing keeping it compiled, and both merely restated
// QFileInfo::exists() && isWritable() back to itself on a file the test had just created (or
// just not created) -- they exercised Qt, not the permission manager.

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
    // Empty comes back from three different arms (the embedded-NUL refusal, a GetNamedSecurityInfoW
    // failure, a conversion failure), and m_lastError is never cleared on entry -- so an arm that
    // returns empty WITHOUT recording an error leaves a stale message from an unrelated earlier
    // call. Pin WHICH arm fired: the parent directory exists, so the Win32 error is
    // ERROR_FILE_NOT_FOUND (2), measured identically for the forward-slash form
    // QTemporaryDir::filePath() produces and for the backslash form.
    QCOMPARE(mgr.getLastError(), QStringLiteral("Failed to get security descriptor: 2"));
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

    // Seed a PROTECTED, explicit DACL first so the two negative assertions below are not trivially
    // true. A freshly created temp file ALREADY reads back as "O:...G:...D:AI(A;ID;...)...":
    // neither "NO_ACCESS_CONTROL" nor "D:P" is present BEFORE stripPermissions runs at all, so
    // both negatives held on the pre-state and proved nothing about the strip. Measured: dropping
    // the UNPROTECTED bit from the strip's SECURITY_INFORMATION left the fresh file's SDDL
    // byte-identical and the old assertions green, while on a protected file it leaves an EMPTY
    // PROTECTED DACL that locks everyone out.
    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());
    QVERIFY2(mgr.setSecurityDescriptorSddl(f, QStringLiteral("D:P(A;;FA;;;%1)").arg(ownerSid)),
             qPrintable(mgr.getLastError()));
    const QString seeded = mgr.getSecurityDescriptorSddl(f);
    QVERIFY2(seeded.contains(QStringLiteral("D:P")), qPrintable(seeded));

    QVERIFY2(mgr.stripPermissions(f), qPrintable(mgr.getLastError()));
    const QString sddl = mgr.getSecurityDescriptorSddl(f);
    QVERIFY(!sddl.isEmpty());
    // A NULL DACL serializes as "NO_ACCESS_CONTROL"; it must be absent.
    QVERIFY2(!sddl.contains("NO_ACCESS_CONTROL"),
             qPrintable(QStringLiteral("strip produced a null DACL: %1").arg(sddl)));
    // Inheritance must be re-enabled (unprotected), i.e. not a protected "D:P" -- and because the
    // seed installed exactly that, this now proves the strip CLEARED protection rather than
    // restating a property the fixture already had.
    QVERIFY2(!sddl.contains(QStringLiteral("D:P")),
             qPrintable(QStringLiteral("strip left a protected DACL: %1").arg(sddl)));
    // ...and that it actually rewrote the DACL rather than leaving the seeded one in place.
    QVERIFY2(sddl != seeded, qPrintable(sddl));
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
    // SYSTEM -> "SY", local Administrators -> "BA" in SDDL. NOTE: a freshly created temp file
    // already inherits ACEs for SY and BA, so these two alone are satisfied by the pre-state.
    QVERIFY2(sddl.contains(QStringLiteral(";SY)")),
             qPrintable(QStringLiteral("SYSTEM missing from ACL: %1").arg(sddl)));
    QVERIFY2(sddl.contains(QStringLiteral(";BA)")),
             qPrintable(QStringLiteral("Administrators missing from ACL: %1").arg(sddl)));
    // buildStandardUserDacl grants EXACTLY three trustees (kAclTrusteeCount) and the DACL is
    // applied PROTECTED, so the result is three ACEs and no inherited ones. Pinning only the two
    // well-known siblings left the DESTINATION USER unasserted (a 3-entry table that names
    // Administrators twice still passes), and accepted an UNPROTECTED apply that keeps every one of
    // the parent's inherited ACEs alongside them.
    QCOMPARE(sddl.count(QLatin1Char('(')), qsizetype(3));
    QVERIFY2(sddl.contains(QStringLiteral("D:P")),
             qPrintable(QStringLiteral("standard-user DACL is not protected: %1").arg(sddl)));
    // The destination-user ACE. Its trustee is this file's own owner SID, so the writer renders it
    // exactly as the descriptor's O: field -- taking the expected text from there is alias-proof (a
    // well-known SID prints as a two-letter alias in BOTH places).
    QVERIFY2(sddl.startsWith(QLatin1String("O:")), qPrintable(sddl));
    const qsizetype groupPos = sddl.indexOf(QLatin1String("G:"));
    QVERIFY2(groupPos > 2, qPrintable(sddl));
    QVERIFY2(sddl.contains(QStringLiteral(";%1)").arg(sddl.mid(2, groupPos - 2))),
             qPrintable(QStringLiteral("destination user missing from ACL: %1").arg(sddl)));
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
    QCOMPARE(result.error(), sak::error_code::permission_update_failed);
    // Pin the kReparseRefused text exactly, the way the ancestor-junction test below does. A
    // case-insensitive contains("reparse") also accepts a by-NAME pre-check bolted on ahead of the
    // no-follow open (e.g. "Refused: reparse point detected"), which is precisely the TOCTOU-open
    // shape the handle-based guard exists to replace.
    QCOMPARE(mgr.getLastError(),
             QStringLiteral("Refused: target is a reparse point (junction/symlink)"));
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
void PermissionManagerTests::stripPermissions_refusesHardLink() {
#ifdef Q_OS_WIN
    // A file with more than one hard link name is the SAME underlying object under two names. An
    // elevated ACL/owner change on one name would silently re-permission the shared object the
    // other name also points at, so the no-follow open refuses a target whose nNumberOfLinks > 1 --
    // the hard-link sibling of the reparse-point and ancestor-junction guards.
    sak::PermissionManager mgr;
    const QString shared = m_tempDir.filePath("hardlink_shared.txt");
    QFile f(shared);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("shared");
    f.close();
    const QString link = m_tempDir.filePath("hardlink_alias.txt");

    QProcess proc;
    proc.start(
        "cmd",
        {"/c", "mklink", "/H", QDir::toNativeSeparators(link), QDir::toNativeSeparators(shared)});
    QVERIFY(proc.waitForFinished(10'000));
    if (proc.exitCode() != 0) {
        QSKIP("Could not create a hard link (non-NTFS or policy)");
    }

    const auto result = mgr.tryStripPermissions(link);
    QVERIFY2(!result.has_value(), "strip must refuse a file with multiple hard links");
    QVERIFY2(mgr.getLastError().contains("hard link", Qt::CaseInsensitive),
             qPrintable(mgr.getLastError()));

    // Guard-isolation control: a plain single-link file in the same dir must NOT be refused for the
    // hard-link reason (it may succeed, or fail for another reason, but never the multi-link
    // guard).
    const QString solo = m_tempDir.filePath("hardlink_solo.txt");
    QFile s(solo);
    QVERIFY(s.open(QIODevice::WriteOnly));
    s.write("solo");
    s.close();
    const auto soloResult = mgr.tryStripPermissions(solo);
    if (!soloResult.has_value()) {
        QVERIFY(!mgr.getLastError().contains("hard link", Qt::CaseInsensitive));
    }
#else
    QVERIFY(true);
#endif
}

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
    // Pin the ancestor-junction (kPathRedirected) refusal exactly. The old OR also accepted
    // "junction"/"reparse", which belong to the DIFFERENT kReparseRefused message -- so a
    // regression that collapsed the ancestor-redirect guard into the reparse guard would have
    // stayed green. "redirected ... ancestor" is unique to this branch.
    QCOMPARE(err,
             QStringLiteral(
                 "Refused: path resolves through a redirected junction/symlink ancestor"));
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

namespace {
#ifdef Q_OS_WIN
// Create a fresh, test-owned file and return its path (own file per test, so an SDDL apply in one
// test cannot perturb another -- see G18-8).
QString makeOwnedSddlFile(const QTemporaryDir& dir, const QString& name) {
    const QString f = dir.filePath(name);
    QFile file(f);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    file.write("x");
    file.close();
    return f;
}
#endif
}  // namespace

// R5-G10-9: a present-but-NULL DACL in an SDDL grants Everyone full control.
// setSecurityDescriptorSddl must refuse it (collectParsedDacl rejects a present NULL DACL) rather
// than apply it. The refusal is a parse-time check that runs before any file write, so no elevation
// is required.
void PermissionManagerTests::setSddl_refusesPresentNullDacl() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = makeOwnedSddlFile(m_tempDir, QStringLiteral("sddl_nulldacl.txt"));

    QVERIFY(!mgr.setSecurityDescriptorSddl(f, QStringLiteral("D:NO_ACCESS_CONTROL")));
    QCOMPARE(mgr.getLastError(),
             QStringLiteral("SDDL specifies neither an owner nor a valid DACL"));

    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());

    // Guard isolation. applyParsedSecurityDescriptor returns ERROR_INVALID_SECURITY_DESCR -- this
    // same message -- from five places, one of which is "the descriptor specified nothing at all"
    // (si == 0). A bare "D:NO_ACCESS_CONTROL" hits that arm too, so an implementation that merely
    // IGNORED the present-but-NULL DACL instead of refusing it stayed green. Carrying a valid OWNER
    // alongside the NULL DACL makes si != 0, leaving collectParsedDacl's present-NULL-DACL
    // rejection as the only branch that can produce this refusal.
    const QString ownerNullDacl = QStringLiteral("O:%1D:NO_ACCESS_CONTROL").arg(ownerSid);
    QVERIFY(!mgr.setSecurityDescriptorSddl(f, ownerNullDacl));
    QCOMPARE(mgr.getLastError(),
             QStringLiteral("SDDL specifies neither an owner nor a valid DACL"));

    // Non-vacuity: a well-formed DACL granting the current owner full access IS accepted, so the
    // rejection is specific to the NULL DACL, not a blanket refusal of every SDDL.
    QVERIFY2(mgr.setSecurityDescriptorSddl(f, QStringLiteral("D:(A;;FA;;;%1)").arg(ownerSid)),
             qPrintable(mgr.getLastError()));
#else
    QVERIFY(true);
#endif
}

// R5-G10-9: this apply path handles only owner/group/DACL, so an SDDL carrying a SACL (an audit or
// mandatory-integrity label) must be refused rather than silently dropped while the rest is
// applied.
void PermissionManagerTests::setSddl_refusesSacl() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = makeOwnedSddlFile(m_tempDir, QStringLiteral("sddl_sacl.txt"));
    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());

    // A low-integrity mandatory-label SACL alongside a valid DACL.
    const QString withSacl = QStringLiteral("D:(A;;FA;;;%1)S:(ML;;NW;;;LW)").arg(ownerSid);
    // The SACL check runs BEFORE applyParsedSecurityDescriptor touches the file, so the refusal
    // must also be FAIL-CLOSED: the DACL carried alongside the SACL must not have been applied.
    // "returns false with a message mentioning SACL" alone also accepts an implementation that
    // applies owner/group/DACL first and only then notices the SACL.
    const QString before = mgr.getSecurityDescriptorSddl(f);
    QVERIFY(!before.isEmpty());
    QVERIFY(!mgr.setSecurityDescriptorSddl(f, withSacl));
    QCOMPARE(mgr.getLastError(),
             QStringLiteral(
                 "Refused: SDDL carries a SACL (audit/integrity label), which is not applied"));
    QCOMPARE(mgr.getSecurityDescriptorSddl(f), before);

    // Non-vacuity: the SAME descriptor WITHOUT the S: field applies, so the refusal is the SACL,
    // not the DACL alongside it.
    QVERIFY2(mgr.setSecurityDescriptorSddl(f, QStringLiteral("D:(A;;FA;;;%1)").arg(ownerSid)),
             qPrintable(mgr.getLastError()));
#else
    QVERIFY(true);
#endif
}

// R5-G10-9: Win32 truncates a string at the first NUL, so an embedded NUL in the path or the SDDL
// would apply a security change to a DIFFERENT (prefix) target than the validated/displayed string.
// It must be refused before the parse.
void PermissionManagerTests::setSddl_refusesEmbeddedNul() {
#ifdef Q_OS_WIN
    sak::PermissionManager mgr;
    const QString f = makeOwnedSddlFile(m_tempDir, QStringLiteral("sddl_nul.txt"));
    const QString ownerSid = mgr.getOwner(f);
    QVERIFY(!ownerSid.isEmpty());
    const QString validSddl = QStringLiteral("D:(A;;FA;;;%1)").arg(ownerSid);

    const QString pathWithNul = f + QChar(u'\0') + QStringLiteral("ignored");
    QVERIFY(!mgr.setSecurityDescriptorSddl(pathWithNul, validSddl));
    QVERIFY2(mgr.getLastError().contains(QStringLiteral("embedded NUL")),
             qPrintable(mgr.getLastError()));

    const QString sddlWithNul = validSddl + QChar(u'\0') + QStringLiteral("S:(ML;;NW;;;LW)");
    QVERIFY(!mgr.setSecurityDescriptorSddl(f, sddlWithNul));
    QVERIFY2(mgr.getLastError().contains(QStringLiteral("embedded NUL")),
             qPrintable(mgr.getLastError()));

    // Non-vacuity: the clean path + clean SDDL applies.
    QVERIFY2(mgr.setSecurityDescriptorSddl(f, validSddl), qPrintable(mgr.getLastError()));
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
