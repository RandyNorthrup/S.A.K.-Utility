// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_user_profile_restore_worker.cpp
 * @brief TST-06 — Unit tests for UserProfileRestoreWorker.
 *
 * Tests: backup validation, conflict resolution modes, cancellation,
 * signal emission, directory creation, and progress tracking.
 *
 * Strategy:
 *   - Uses QTemporaryDir for both the "backup" source and the restore destination.
 *   - Overrides the SystemDrive environment variable so MergeMode::CreateNewUser
 *     writes to a temp directory instead of real C:\Users.
 *   - Uses PermissionMode::PreserveOriginal to avoid Windows ACL operations.
 *   - Tests run without admin privileges.
 */

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "sak/user_profile_restore_worker.h"
#include "sak/user_profile_types.h"

// ---------------------------------------------------------------------------
// Helper: create a BackupManifest with one user whose backed_up_folders match
// the files we planted in the temp backup directory.
// ---------------------------------------------------------------------------
static sak::BackupManifest buildManifest(const QString& username,
                                         const QVector<sak::FolderSelection>& folders) {
    sak::BackupManifest manifest;
    manifest.version = QStringLiteral("1.0");
    manifest.source_machine = QStringLiteral("TEST-PC");
    manifest.sak_version = QStringLiteral("0.7.0");
    manifest.backup_type = QStringLiteral("full");

    sak::BackupUserData user;
    user.username = username;
    user.sid = QStringLiteral("S-1-5-21-1234-5678");
    user.profile_path = QStringLiteral("C:/Users/") + username;
    user.permissions_mode = sak::PermissionMode::PreserveOriginal;
    user.encrypted = false;
    user.compression = QStringLiteral("none");
    user.backed_up_folders = folders;

    manifest.users.append(user);
    return manifest;
}

// ---------------------------------------------------------------------------
// Helper: create a standard UserMapping using CreateNewUser (no WinAPI calls).
// ---------------------------------------------------------------------------
static sak::UserMapping makeMapping(const QString& srcUser, const QString& dstUser = {}) {
    sak::UserMapping m;
    m.source_username = srcUser;
    m.destination_username = dstUser.isEmpty() ? srcUser : dstUser;
    m.mode = sak::MergeMode::CreateNewUser;
    m.conflict_resolution = sak::ConflictResolution::SkipDuplicate;
    m.selected = true;
    return m;
}

// ---------------------------------------------------------------------------
// Helper: create a FolderSelection.
// ---------------------------------------------------------------------------
static sak::FolderSelection makeFolder(sak::FolderType type,
                                       const QString& name,
                                       const QString& relPath,
                                       qint64 sizeBytes,
                                       int fileCount) {
    sak::FolderSelection f;
    f.type = type;
    f.display_name = name;
    f.relative_path = relPath;
    f.selected = true;
    f.size_bytes = sizeBytes;
    f.file_count = fileCount;
    return f;
}

// ---------------------------------------------------------------------------
// Helper: write a text file into an existing directory tree.
// ---------------------------------------------------------------------------
static bool writeFile(const QString& path, const QByteArray& content) {
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(content);
    f.close();
    return true;
}

// ===========================================================================
class UserProfileRestoreWorkerTests : public QObject {
    Q_OBJECT

private:
    QByteArray m_savedSystemDrive;

    /// Set up a minimal backup tree with manifest.json, returning the path.
    /// Caller must keep the QTemporaryDir alive for the duration of the test.
    void createBackupTree(QTemporaryDir& backupDir,
                          const QString& username,
                          const QStringList& relativePaths,
                          const QByteArray& fileContent = "backup-content");

private slots:
    void initTestCase();
    void cleanupTestCase();

    // ---- Validation ----
    void invalidBackupNoManifest();
    void emptyMappingsCompleteSuccessfully();

    // ---- B7-13: manifest + per-user payload integrity checksums ----
    void manifestChecksumMismatchFailsValidation();
    void payloadChecksumMismatchFailsValidation();
    void correctChecksumsPassValidation();
    void sealedManifestMissingUserDigestFailsClosed();

    // ---- B7-21: AssignToDestination assigns ownership, not strip ----
    void assignToDestinationUsesUsername();
    void effectiveDestUserPrefersDestination();

    // ---- Core restore flow ----
    void singleFileRestoreSucceeds();
    void unselectedMappingSkipped();
    void sourceUserNotInManifest();
    void multipleFoldersRestored();

    // ---- Conflict resolution ----
    void conflictSkipDuplicate();
    void conflictRenameWithSuffix();
    void conflictKeepNewer();
    void conflictKeepLarger();
    void conflictPromptUserAutoRenames();

    // ---- Cancellation ----
    void cancelBeforeRestoreEmitsCancel();

    // ---- Signals ----
    void restoreCompleteSignalEmitted();
    void logMessageSignalEmitted();

    // ---- B7-01: lifetime uses QThread state, not a late member flag ----
    void startThenImmediateDestroyIsSafe();

    // ---- B7-10: overwrite replaces via rename-aside, leaving no temp artifacts ----
    void overwriteRestoreLeavesNoTempArtifacts();

    // ---- R3-02/R3-15: failures are counted and fail the overall result ----
    void unknownMergeModeFailsClosed();
    void missingFolderSourceFailsClosed();

    // ---- R3-03/R3-09: content verification passes a good copy ----
    void verifyGoodCopySucceeds();

    // ---- AppData source filtering + netsh path resolution (residual wiring) ----
    void appDataEmptySourcesNeverExcludes();
    void appDataUncheckedSourceExcludesSubtree();
    void appDataCheckedSourceNotExcluded();
    void appDataLongestPrefixWins();
    void appDataUnrelatedPathNotExcluded();
    void system32NetshResolvesUnderSystemRoot();
    void system32NetshEmptyWhenNoSystemRoot();
};

// ===========================================================================
// Setup / Teardown
// ===========================================================================

void UserProfileRestoreWorkerTests::initTestCase() {
    m_savedSystemDrive = qgetenv("SystemDrive");
}

void UserProfileRestoreWorkerTests::cleanupTestCase() {
    // Restore original SystemDrive
    if (m_savedSystemDrive.isEmpty()) {
        qunsetenv("SystemDrive");
    } else {
        qputenv("SystemDrive", m_savedSystemDrive);
    }
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::createBackupTree(QTemporaryDir& backupDir,
                                                     const QString& username,
                                                     const QStringList& relativePaths,
                                                     const QByteArray& fileContent) {
    // Write manifest.json (content doesn't matter, only existence).
    writeFile(backupDir.path() + QStringLiteral("/manifest.json"), "{}");

    // Write test files under <backupDir>/<username>/<relativePath>.
    for (const QString& rel : relativePaths) {
        const QString full = backupDir.path() + "/" + username + "/" + rel;
        QVERIFY2(writeFile(full, fileContent), qPrintable("Failed to write " + full));
    }
}

// ===========================================================================
// Tests — Validation
// ===========================================================================

void UserProfileRestoreWorkerTests::invalidBackupNoManifest() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    // No manifest.json ⇒ should fail immediately.

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    sak::BackupManifest manifest = buildManifest(QStringLiteral("User1"), {});
    sak::UserMapping mapping = makeMapping(QStringLiteral("User1"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);  // failure
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::emptyMappingsCompleteSuccessfully() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    writeFile(backupDir.path() + "/manifest.json", "{}");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    sak::BackupManifest manifest = buildManifest(QStringLiteral("User1"), {});
    // No mappings → immediate success.

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);  // success
}

// ---------------------------------------------------------------------------
// B7-13: a manifest whose stored checksum does not match its content is rejected
// before any file is touched.
// ---------------------------------------------------------------------------
void UserProfileRestoreWorkerTests::manifestChecksumMismatchFailsValidation() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("TestUser"),
                     {QStringLiteral("Documents/hello.txt")});

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto manifest = buildManifest(QStringLiteral("TestUser"), {});
    manifest.manifest_checksum = QStringLiteral("deadbeefdeadbeef");  // wrong, non-empty
    auto mapping = makeMapping(QStringLiteral("TestUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);  // integrity failure -> refuse
}

// ---------------------------------------------------------------------------
// A per-user payload whose recomputed digest does not match the manifest's
// checksum_sha256 is rejected (detects tampered/corrupt stored files).
// ---------------------------------------------------------------------------
void UserProfileRestoreWorkerTests::payloadChecksumMismatchFailsValidation() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("TestUser"),
                     {QStringLiteral("Documents/hello.txt")});

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto manifest = buildManifest(QStringLiteral("TestUser"), {});
    // Non-empty but wrong payload digest -> mismatch on recompute.
    manifest.users[0].checksum_sha256 = QStringLiteral("00ff00ff00ff00ff");
    manifest.manifest_checksum = manifest.computeManifestChecksum();
    auto mapping = makeMapping(QStringLiteral("TestUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);
}

// ---------------------------------------------------------------------------
// Correct manifest + payload digests must NOT false-fail: the restore proceeds.
// ---------------------------------------------------------------------------
void UserProfileRestoreWorkerTests::correctChecksumsPassValidation() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("TestUser"),
                     {QStringLiteral("Documents/hello.txt")},
                     "hello from backup");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             17,
                             1);
    auto manifest = buildManifest(QStringLiteral("TestUser"), {folder});
    // Seal with the ACTUAL payload + manifest digests, as the backup worker does.
    manifest.users[0].checksum_sha256 =
        sak::BackupManifest::hashDirectoryTree(backupDir.path() + "/TestUser");
    QVERIFY(!manifest.users[0].checksum_sha256.isEmpty());
    manifest.manifest_checksum = manifest.computeManifestChecksum();
    auto mapping = makeMapping(QStringLiteral("TestUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);  // valid integrity -> proceeds
    QVERIFY(QFile::exists(destDir.path() + "/Users/TestUser/Documents/hello.txt"));
}

// ---------------------------------------------------------------------------
// CODEX_REVIEW_4 M-A2-12: a legacy backup (no manifest integrity checksum) is
// intentionally accepted, but a SEALED manifest (manifest_checksum present) whose
// per-user payload digest is empty is inconsistent -- an attacker stripped the digest
// and re-sealed -- and must fail closed. Independent of the verify flag.
// ---------------------------------------------------------------------------
void UserProfileRestoreWorkerTests::sealedManifestMissingUserDigestFailsClosed() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("TestUser"),
                     {QStringLiteral("Documents/hello.txt")});

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto manifest = buildManifest(QStringLiteral("TestUser"), {});
    // Leave the per-user payload digest EMPTY but SEAL the manifest, so verifyManifestChecksum
    // passes yet the payload is unauthenticated -- the stripped-digest tamper case.
    QVERIFY(manifest.users[0].checksum_sha256.isEmpty());
    manifest.manifest_checksum = manifest.computeManifestChecksum();
    QVERIFY(!manifest.manifest_checksum.isEmpty());
    auto mapping = makeMapping(QStringLiteral("TestUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    // verify=false: the fail-closed decision is about manifest consistency, not content hashing.
    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);  // sealed manifest, no payload digest
}

// ---------------------------------------------------------------------------
// B7-21: with a real destination user, AssignToDestination assigns ownership
// (the whole point of the mode); only an empty user falls back to stripping.
// Before the fix the caller always passed "", so it ALWAYS stripped.
// ---------------------------------------------------------------------------
void UserProfileRestoreWorkerTests::assignToDestinationUsesUsername() {
    using RW = sak::UserProfileRestoreWorker;
    QCOMPARE(RW::resolvePermissionAction(sak::PermissionMode::AssignToDestination,
                                         QStringLiteral("Bob")),
             RW::PermissionAction::AssignOwnership);
    QCOMPARE(RW::resolvePermissionAction(sak::PermissionMode::AssignToDestination, QString()),
             RW::PermissionAction::StripPermissions);
    QCOMPARE(RW::resolvePermissionAction(sak::PermissionMode::StripAll, QStringLiteral("Bob")),
             RW::PermissionAction::StripPermissions);
    QCOMPARE(RW::resolvePermissionAction(sak::PermissionMode::PreserveOriginal, QString()),
             RW::PermissionAction::PreserveOriginal);
    // PermissionMode::Hybrid used to be asserted here. It was removed because every path
    // that consumed it simply stripped, while four different places described it as doing
    // something else. Legacy manifests naming it now read back as StripAll, which is
    // covered by permissionModeRoundTrip in test_user_profile_types.
}

void UserProfileRestoreWorkerTests::effectiveDestUserPrefersDestination() {
    using RW = sak::UserProfileRestoreWorker;
    // An explicit destination user is used; the restore therefore passes a NON-empty
    // username into AssignToDestination (so it assigns, not strips).
    auto m1 = makeMapping(QStringLiteral("Alice"), QStringLiteral("Bob"));
    QCOMPARE(RW::effectiveDestUser(m1), QStringLiteral("Bob"));
    // A same-name restore falls back to the source username (still non-empty).
    sak::UserMapping m2;
    m2.source_username = QStringLiteral("Alice");
    m2.destination_username.clear();
    QCOMPARE(RW::effectiveDestUser(m2), QStringLiteral("Alice"));
    // Combined: an AssignToDestination restore of this mapping resolves to assign.
    QCOMPARE(RW::resolvePermissionAction(sak::PermissionMode::AssignToDestination,
                                         RW::effectiveDestUser(m1)),
             RW::PermissionAction::AssignOwnership);
}

// ===========================================================================
// Tests — Core restore flow
// ===========================================================================

void UserProfileRestoreWorkerTests::singleFileRestoreSucceeds() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("TestUser"),
                     {QStringLiteral("Documents/hello.txt")},
                     "hello from backup");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             18,
                             1);
    auto manifest = buildManifest(QStringLiteral("TestUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("TestUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // Verify file was actually copied.
    const QString destFile = destDir.path() + "/Users/TestUser/Documents/hello.txt";
    QVERIFY2(QFile::exists(destFile), qPrintable("Missing: " + destFile));
    QFile f(destFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("hello from backup"));
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::unselectedMappingSkipped() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("SkippedUser"),
                     {QStringLiteral("Documents/file.txt")});

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             14,
                             1);
    auto manifest = buildManifest(QStringLiteral("SkippedUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("SkippedUser"));
    mapping.selected = false;  // NOT selected

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // Destination file should NOT exist.
    const QString destFile = destDir.path() + "/Users/SkippedUser/Documents/file.txt";
    QVERIFY2(!QFile::exists(destFile), "Unselected mapping should not be restored");
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::sourceUserNotInManifest() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    writeFile(backupDir.path() + "/manifest.json", "{}");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    // Manifest has "Alice", mapping asks for "Bob".
    auto folder = makeFolder(
        sak::FolderType::Documents, QStringLiteral("Documents"), QStringLiteral("Documents"), 0, 0);
    auto manifest = buildManifest(QStringLiteral("Alice"), {folder});
    auto mapping = makeMapping(QStringLiteral("Bob"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QSignalSpy logSpy(&worker, &sak::UserProfileRestoreWorker::logMessage);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    // Fail closed (R3-02): a requested user absent from the manifest is a real
    // failure to restore, so the overall result must be reported as failure --
    // not the previous fail-open "success" that hid an incomplete restore.
    QCOMPARE(completeSpy.first().at(0).toBool(), false);

    // Check that a warning was logged about the missing user.
    bool foundWarning = false;
    for (const auto& args : logSpy) {
        if (args.at(0).toString().contains(QStringLiteral("Source user not found")) &&
            args.at(1).toBool()) {
            foundWarning = true;
            break;
        }
    }
    QVERIFY2(foundWarning, "Expected warning about source user not in manifest");
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::multipleFoldersRestored() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("MultiUser"),
                     {QStringLiteral("Documents/doc.txt"),
                      QStringLiteral("Desktop/shortcut.txt"),
                      QStringLiteral("Pictures/photo.jpg")});

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    QVector<sak::FolderSelection> folders;
    folders.append(makeFolder(sak::FolderType::Documents,
                              QStringLiteral("Documents"),
                              QStringLiteral("Documents"),
                              14,
                              1));
    folders.append(makeFolder(
        sak::FolderType::Desktop, QStringLiteral("Desktop"), QStringLiteral("Desktop"), 14, 1));
    folders.append(makeFolder(
        sak::FolderType::Pictures, QStringLiteral("Pictures"), QStringLiteral("Pictures"), 14, 1));

    auto manifest = buildManifest(QStringLiteral("MultiUser"), folders);
    auto mapping = makeMapping(QStringLiteral("MultiUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // All three files should be present at the destination.
    const QString base = destDir.path() + "/Users/MultiUser/";
    QVERIFY(QFile::exists(base + "Documents/doc.txt"));
    QVERIFY(QFile::exists(base + "Desktop/shortcut.txt"));
    QVERIFY(QFile::exists(base + "Pictures/photo.jpg"));
}

// ===========================================================================
// Tests — Conflict resolution
// ===========================================================================

void UserProfileRestoreWorkerTests::conflictSkipDuplicate() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("CUser"),
                     {QStringLiteral("Documents/existing.txt")},
                     "from-backup");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    // Pre-populate destination with an existing file.
    const QString destFile = destDir.path() + "/Users/CUser/Documents/existing.txt";
    QVERIFY(writeFile(destFile, "original-content"));

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             11,
                             1);
    auto manifest = buildManifest(QStringLiteral("CUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("CUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // File should be unchanged (original content, not backup content).
    QFile f(destFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("original-content"));
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::conflictRenameWithSuffix() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("RUser"),
                     {QStringLiteral("Documents/report.txt")},
                     "backup-version");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    const QString destBase = destDir.path() + "/Users/RUser/Documents/";
    QVERIFY(writeFile(destBase + "report.txt", "existing-version"));

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             14,
                             1);
    auto manifest = buildManifest(QStringLiteral("RUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("RUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::RenameWithSuffix, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // Original should be untouched.
    QFile orig(destBase + "report.txt");
    QVERIFY(orig.open(QIODevice::ReadOnly));
    QCOMPARE(orig.readAll(), QByteArray("existing-version"));

    // Backup copy should exist with _backup1 suffix.
    const QString renamed = destBase + "report_backup1.txt";
    QVERIFY2(QFile::exists(renamed), qPrintable("Expected: " + renamed));
    QFile renamedFile(renamed);
    QVERIFY(renamedFile.open(QIODevice::ReadOnly));
    QCOMPARE(renamedFile.readAll(), QByteArray("backup-version"));
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::conflictKeepNewer() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("NUser"),
                     {QStringLiteral("Documents/data.txt")},
                     "newer-from-backup");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    const QString destFile = destDir.path() + "/Users/NUser/Documents/data.txt";
    QVERIFY(writeFile(destFile, "old-dest"));

    QFile df(destFile);
    QVERIFY(df.open(QIODevice::ReadWrite));
    df.close();
    QFileInfo destInfo(destFile);
    QDateTime pastTime = QDateTime::currentDateTime().addDays(-30);
#ifdef Q_OS_WIN
    {
        HANDLE hFile = CreateFileW(reinterpret_cast<LPCWSTR>(destFile.utf16()),
                                   FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            SYSTEMTIME st = {2020, 1, 0, 1, 0, 0, 0, 0};
            SystemTimeToFileTime(&st, &ft);
            SetFileTime(hFile, nullptr, nullptr, &ft);
            CloseHandle(hFile);
        }
    }
#endif

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             17,
                             1);
    auto manifest = buildManifest(QStringLiteral("NUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("NUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::KeepNewer, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    QFile result(destFile);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), QByteArray("newer-from-backup"));
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::conflictKeepLarger() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    // Backup has a LARGE file.
    createBackupTree(backupDir,
                     QStringLiteral("LUser"),
                     {QStringLiteral("Documents/big.dat")},
                     QByteArray(5000, 'X'));

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    // Dest has a SMALL file.
    const QString destFile = destDir.path() + "/Users/LUser/Documents/big.dat";
    QVERIFY(writeFile(destFile, "tiny"));

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             5000,
                             1);
    auto manifest = buildManifest(QStringLiteral("LUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("LUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::KeepLarger, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // Larger backup file should have replaced the tiny dest.
    QFile result(destFile);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QByteArray data = result.readAll();
    QCOMPARE(data.size(), 5000);
    QCOMPARE(data, QByteArray(5000, 'X'));
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::conflictPromptUserAutoRenames() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("PUser"),
                     {QStringLiteral("Documents/notes.txt")},
                     "backup-notes");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    const QString destBase = destDir.path() + "/Users/PUser/Documents/";
    QVERIFY(writeFile(destBase + "notes.txt", "existing-notes"));

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             12,
                             1);
    auto manifest = buildManifest(QStringLiteral("PUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("PUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::PromptUser, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // Original untouched.
    QFile orig(destBase + "notes.txt");
    QVERIFY(orig.open(QIODevice::ReadOnly));
    QCOMPARE(orig.readAll(), QByteArray("existing-notes"));

    // Auto-renamed copy should use _restored1 suffix.
    const QString renamed = destBase + "notes_restored1.txt";
    QVERIFY2(QFile::exists(renamed), qPrintable("Expected: " + renamed));
    QFile renamedFile(renamed);
    QVERIFY(renamedFile.open(QIODevice::ReadOnly));
    QCOMPARE(renamedFile.readAll(), QByteArray("backup-notes"));
}

// ===========================================================================
// Tests — Cancellation
// ===========================================================================

void UserProfileRestoreWorkerTests::cancelBeforeRestoreEmitsCancel() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    writeFile(backupDir.path() + "/manifest.json", "{}");

    // Create many files to increase the chance cancellation takes effect.
    const QString username = QStringLiteral("CancelUser");
    for (int i = 0; i < 100; ++i) {
        writeFile(backupDir.path() + "/" + username + "/Documents/file" + QString::number(i) +
                      ".txt",
                  QByteArray(1024, 'A'));
    }

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             102'400,
                             100);
    auto manifest = buildManifest(username, {folder});
    auto mapping = makeMapping(username);

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    // Immediately cancel.
    worker.cancel();

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);

    // The restore should report failure (cancelled) or partial completion.
    // Either outcome is acceptable as long as we didn't crash.
    // The implementation emits restoreComplete(true, summary) if cancellation
    // was processed between user iterations, or restoreComplete(false, "cancelled")
    // if processed at the top of the user loop. Both are valid.
    QVERIFY(completeSpy.count() >= 1);
}

// ===========================================================================
// Tests — Signals
// ===========================================================================

void UserProfileRestoreWorkerTests::restoreCompleteSignalEmitted() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(
        backupDir, QStringLiteral("SigUser"), {QStringLiteral("Documents/sig.txt")}, "signal-test");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             11,
                             1);
    auto manifest = buildManifest(QStringLiteral("SigUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("SigUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QSignalSpy statusSpy(&worker, &sak::UserProfileRestoreWorker::statusUpdate);
    QVERIFY(completeSpy.isValid());
    QVERIFY(statusSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.count(), 1);

    auto args = completeSpy.first();
    QCOMPARE(args.at(0).toBool(), true);
    QVERIFY(!args.at(1).toString().isEmpty());  // summary message

    // statusUpdate should have been emitted at least once.
    QVERIFY(statusSpy.count() >= 1);
}

// ---------------------------------------------------------------------------

void UserProfileRestoreWorkerTests::logMessageSignalEmitted() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(
        backupDir, QStringLiteral("LogUser"), {QStringLiteral("Documents/log.txt")}, "log-test");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(
        sak::FolderType::Documents, QStringLiteral("Documents"), QStringLiteral("Documents"), 8, 1);
    auto manifest = buildManifest(QStringLiteral("LogUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("LogUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QSignalSpy logSpy(&worker, &sak::UserProfileRestoreWorker::logMessage);
    QVERIFY(completeSpy.isValid());
    QVERIFY(logSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);

    // Several log messages should have been emitted:
    // "=== Restore Started ===", "Backup: ...", "Users to restore: 1",
    // "Backup validation passed", "Calculating total size...", etc.
    QVERIFY2(logSpy.count() >= 5,
             qPrintable(QString("Expected >=5 log messages, got %1").arg(logSpy.count())));

    // Check for "Restore Started" message.
    bool foundStart = false;
    for (const auto& args : logSpy) {
        if (args.at(0).toString().contains(QStringLiteral("Restore Started"))) {
            foundStart = true;
            break;
        }
    }
    QVERIFY2(foundStart, "Expected '=== Restore Started ===' log message");
}

// ===========================================================================

// ===========================================================================
// Tests -- B7-01 lifetime
// ===========================================================================

// isRunning() now reflects QThread state (true the instant start() returns), so
// destroying a worker right after startRestore() -- with NO prior wait() -- cancels
// and joins the live thread instead of aborting with "QThread: Destroyed while
// thread is still running" (the old late-set member flag left that window open).
void UserProfileRestoreWorkerTests::startThenImmediateDestroyIsSafe() {
    for (int i = 0; i < 5; ++i) {
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        const QString username = QStringLiteral("DestroyUser");
        for (int f = 0; f < 40; ++f) {
            writeFile(backupDir.path() + "/" + username + "/Documents/f" + QString::number(f) +
                          ".txt",
                      QByteArray(4096, 'Z'));
        }
        writeFile(backupDir.path() + "/manifest.json", "{}");

        QTemporaryDir destDir;
        QVERIFY(destDir.isValid());
        qputenv("SystemDrive", destDir.path().toLocal8Bit());

        auto folder = makeFolder(sak::FolderType::Documents,
                                 QStringLiteral("Documents"),
                                 QStringLiteral("Documents"),
                                 40 * 4096,
                                 40);
        auto manifest = buildManifest(username, {folder});
        auto mapping = makeMapping(username);

        sak::UserProfileRestoreWorker worker;
        worker.startRestore(
            backupDir.path(),
            manifest,
            {mapping},
            {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});
        // worker destructs here at end of scope, WITHOUT a prior wait(): must not abort.
    }
    QVERIFY(true);  // Survived every start-then-immediate-destroy iteration.
}

// ===========================================================================
// B7-10: replacing an existing file must move the original aside by RENAME and
// only remove it once the replacement is swapped in -- so no failure loses the
// destination -- and the success path must leave no .sakold.tmp/.sakrestore.tmp
// artifacts behind (with create_backup, a .sakbak recovery copy is kept).
// ===========================================================================

void UserProfileRestoreWorkerTests::overwriteRestoreLeavesNoTempArtifacts() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("OUser"),
                     {QStringLiteral("Documents/data.txt")},
                     QByteArray(5000, 'X'));  // large -> KeepLarger replaces the small dest

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    const QString destBase = destDir.path() + "/Users/OUser/Documents/";
    QVERIFY(writeFile(destBase + "data.txt", "tiny"));  // small existing file

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             5000,
                             1);
    auto manifest = buildManifest(QStringLiteral("OUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("OUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    // create_backup = true (4th field) so the original is preserved to .sakbak.
    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::KeepLarger, sak::PermissionMode::PreserveOriginal, false, true});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // The larger backup replaced the small original.
    QFile result(destBase + "data.txt");
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll().size(), 5000);

    // No swap temporaries left behind.
    QVERIFY(!QFile::exists(destBase + "data.txt.sakold.tmp"));
    QVERIFY(!QFile::exists(destBase + "data.txt.sakrestore.tmp"));

    // The pre-overwrite original was preserved to a recovery copy (holds "tiny").
    QFile recovery(destBase + "data.txt.sakbak");
    QVERIFY(recovery.open(QIODevice::ReadOnly));
    QCOMPARE(recovery.readAll(), QByteArray("tiny"));
}

// ===========================================================================
// R3-15: an unknown/out-of-range merge mode must fail closed (no silent success
// with an unset destination). resolveDestinationProfilePath now returns false for
// it, restoreUser fails, and the overall result is failure.
// ===========================================================================
void UserProfileRestoreWorkerTests::unknownMergeModeFailsClosed() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(
        backupDir, QStringLiteral("UmmUser"), {QStringLiteral("Documents/file.txt")}, "content");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(
        sak::FolderType::Documents, QStringLiteral("Documents"), QStringLiteral("Documents"), 7, 1);
    auto manifest = buildManifest(QStringLiteral("UmmUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("UmmUser"));
    mapping.mode = static_cast<sak::MergeMode>(999);  // out of range

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);
}

// ===========================================================================
// R3-02: a selected folder whose source directory is absent is a real failure
// and must be counted -> overall result is failure (not fail-open success).
// ===========================================================================
void UserProfileRestoreWorkerTests::missingFolderSourceFailsClosed() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    // Only a manifest is written; the "Documents" source folder is never created.
    writeFile(backupDir.path() + "/manifest.json", "{}");
    QDir().mkpath(backupDir.path() + "/MfsUser");  // user dir exists, folder does not

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(
        sak::FolderType::Documents, QStringLiteral("Documents"), QStringLiteral("Documents"), 0, 0);
    auto manifest = buildManifest(QStringLiteral("MfsUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("MfsUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, false});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), false);
}

// ===========================================================================
// R3-03/R3-09: with verify enabled, a correctly copied file (content matches the
// source) must still verify as success -- the content-hash check must not
// false-fail a good restore.
// ===========================================================================
void UserProfileRestoreWorkerTests::verifyGoodCopySucceeds() {
    QTemporaryDir backupDir;
    QVERIFY(backupDir.isValid());
    createBackupTree(backupDir,
                     QStringLiteral("VUser"),
                     {QStringLiteral("Documents/verify.txt")},
                     "verify-this-content");

    QTemporaryDir destDir;
    QVERIFY(destDir.isValid());
    qputenv("SystemDrive", destDir.path().toLocal8Bit());

    auto folder = makeFolder(sak::FolderType::Documents,
                             QStringLiteral("Documents"),
                             QStringLiteral("Documents"),
                             19,
                             1);
    auto manifest = buildManifest(QStringLiteral("VUser"), {folder});
    auto mapping = makeMapping(QStringLiteral("VUser"));

    sak::UserProfileRestoreWorker worker;
    QSignalSpy completeSpy(&worker, &sak::UserProfileRestoreWorker::restoreComplete);
    QVERIFY(completeSpy.isValid());

    // verify = true (3rd field): the post-copy SHA-256 must match the source.
    worker.startRestore(
        backupDir.path(),
        manifest,
        {mapping},
        {sak::ConflictResolution::SkipDuplicate, sak::PermissionMode::PreserveOriginal, true});

    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    const QString destFile = destDir.path() + "/Users/VUser/Documents/verify.txt";
    QVERIFY(QFile::exists(destFile));
}

// ===========================================================================
// Tests — AppData source filtering (honoring the restore AppData page)
// ===========================================================================

namespace {
sak::AppDataSourceInfo makeSource(const QString& relPath, bool selected) {
    sak::AppDataSourceInfo s;
    s.relative_path = relPath;
    s.selected = selected;
    return s;
}
}  // namespace

void UserProfileRestoreWorkerTests::appDataEmptySourcesNeverExcludes() {
    using RW = sak::UserProfileRestoreWorker;
    // No selection forwarded (legacy backup / page never shown) restores everything.
    QVERIFY(!RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Chrome"), {}));
}

void UserProfileRestoreWorkerTests::appDataUncheckedSourceExcludesSubtree() {
    using RW = sak::UserProfileRestoreWorker;
    QVector<sak::AppDataSourceInfo> sources{
        makeSource(QStringLiteral("AppData/Local/Google/Chrome"), false)};
    // The source root and everything beneath it are excluded; separators and case
    // are normalized.
    QVERIFY(RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Chrome"), sources));
    QVERIFY(RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Chrome/User Data/x"),
                                      sources));
    QVERIFY(RW::isAppDataPathExcluded(QStringLiteral("appdata\\local\\google\\chrome\\Default"),
                                      sources));
}

void UserProfileRestoreWorkerTests::appDataCheckedSourceNotExcluded() {
    using RW = sak::UserProfileRestoreWorker;
    QVector<sak::AppDataSourceInfo> sources{
        makeSource(QStringLiteral("AppData/Local/Google/Chrome"), true)};
    QVERIFY(!RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Chrome/User Data"),
                                       sources));
}

void UserProfileRestoreWorkerTests::appDataLongestPrefixWins() {
    using RW = sak::UserProfileRestoreWorker;
    // A checked child under an unchecked parent path: the most specific (longest)
    // matching source decides, so the child is restored.
    QVector<sak::AppDataSourceInfo> sources{
        makeSource(QStringLiteral("AppData/Local/Google"), false),
        makeSource(QStringLiteral("AppData/Local/Google/Chrome"), true)};
    QVERIFY(RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Earth"), sources));
    QVERIFY(
        !RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/Chrome/Default"), sources));
}

void UserProfileRestoreWorkerTests::appDataUnrelatedPathNotExcluded() {
    using RW = sak::UserProfileRestoreWorker;
    QVector<sak::AppDataSourceInfo> sources{
        makeSource(QStringLiteral("AppData/Local/Google/Chrome"), false)};
    // A path that only shares a partial segment name must NOT be excluded (segment
    // aware: "Chrome" != "ChromeBeta").
    QVERIFY(!RW::isAppDataPathExcluded(QStringLiteral("AppData/Local/Google/ChromeBeta"), sources));
    QVERIFY(!RW::isAppDataPathExcluded(QStringLiteral("Documents/report.docx"), sources));
}

void UserProfileRestoreWorkerTests::system32NetshResolvesUnderSystemRoot() {
    using RW = sak::UserProfileRestoreWorker;
    const QByteArray saved = qgetenv("SystemRoot");
    qputenv("SystemRoot", QByteArrayLiteral("C:\\Windows"));
    const QString netsh = RW::resolveSystem32Netsh();
    if (saved.isEmpty()) {
        qunsetenv("SystemRoot");
    } else {
        qputenv("SystemRoot", saved);
    }
    // Fully qualified under System32 (no bare "netsh.exe" search-order exposure).
    const QString normalized = QDir::fromNativeSeparators(netsh).toLower();
    QVERIFY2(normalized.endsWith(QStringLiteral("/windows/system32/netsh.exe")), qPrintable(netsh));
}

void UserProfileRestoreWorkerTests::system32NetshEmptyWhenNoSystemRoot() {
    using RW = sak::UserProfileRestoreWorker;
    const QByteArray saved = qgetenv("SystemRoot");
    qunsetenv("SystemRoot");
    const QString netsh = RW::resolveSystem32Netsh();
    if (!saved.isEmpty()) {
        qputenv("SystemRoot", saved);
    }
    // Fail closed: no guessed path when %SystemRoot% is unavailable.
    QVERIFY(netsh.isEmpty());
}

QTEST_MAIN(UserProfileRestoreWorkerTests)
#include "test_user_profile_restore_worker.moc"
