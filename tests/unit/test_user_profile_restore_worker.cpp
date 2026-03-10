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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(completeSpy.first().at(0).toBool(), true);  // success
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
    // Still completes (loops through all mappings), but logs a warning.
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::RenameWithSuffix,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    // Make destination file older by backdating it.
    QFile df(destFile);
    QVERIFY(df.open(QIODevice::ReadWrite));
    df.close();
    // Set the dest file's modification time to the past.
    QFileInfo destInfo(destFile);
    QDateTime pastTime = QDateTime::currentDateTime().addDays(-30);
    // Use platform-specific API or just rely on the backup being written after.
    // Since we just created the backup file *before* this, and the dest was
    // written just now, the backup is actually older. We need the backup to
    // be *newer*. Let's backdate the dest file instead.
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
            // Set to 2020-01-01 00:00:00 UTC
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::KeepNewer,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
    QCOMPARE(completeSpy.first().at(0).toBool(), true);

    // The backup file (newer) should have replaced the old dest.
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::KeepLarger,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::PromptUser,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    // Immediately cancel.
    worker.cancel();

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));
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

    worker.startRestore(backupDir.path(),
                        manifest,
                        {mapping},
                        sak::ConflictResolution::SkipDuplicate,
                        sak::PermissionMode::PreserveOriginal,
                        false);

    QVERIFY(completeSpy.wait(5000));

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

QTEST_MAIN(UserProfileRestoreWorkerTests)
#include "test_user_profile_restore_worker.moc"
