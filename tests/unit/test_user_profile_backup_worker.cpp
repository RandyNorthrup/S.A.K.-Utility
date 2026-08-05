// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/backup_file_codec.h"
#include "sak/user_profile_backup_worker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace sak;

/**
 * @brief Unit tests for UserProfileBackupWorker.
 *
 * Tests construction, initial state, and cancel behavior.
 * Actual backup operations require real file system access
 * and are not tested here.
 */
class TestUserProfileBackupWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------
    void testNotRunningInitially();

    // -- Cancel ----------------------------------------------
    void testCancelWhenNotRunning();

    // -- B7-01: lifetime uses QThread state, not a late member flag --
    void testStartThenImmediateDestroyIsSafe();

    // -- B7-08: username / relative-path traversal validation --
    void isSafePathSegment_acceptsNamesRejectsTraversal();
    void isSafeRelativePath_acceptsRelativeRejectsEscapes();

    // -- B7-20: incomplete backups must not report a clean success --
    void backupMissingSelectedFolderReportsFailure();
    void backupExistingFolderReportsSuccess();

    // -- R5-G19-1: the transforms must actually be applied, not just accepted --
    void encryptedBackupStoresCiphertextAndRestoresIt();
    void compressedBackupStoresContainerAndRestoresIt();
    void encryptionWithoutPasswordRefusesToStart();

private:
    struct BackupOutcome {
        std::atomic<bool> done{false};
        bool success{false};
    };
    // What a helper-driven backup reported. `completed` is kept separate from `success`
    // so a run that never emits backupComplete fails its test instead of defaulting into
    // whichever outcome that test expected.
    struct BackupRun {
        bool completed{false};
        bool success{false};
    };
    // Run a one-user backup on @p worker to completion. The worker is caller-owned so a
    // test can put it in a specific state (e.g. a stale cancel) before the run.
    static BackupRun runBackup(UserProfileBackupWorker& worker,
                               const QString& profilePath,
                               const sak::FolderSelection& folder,
                               const QString& destPath);
    /// Same, with explicit transform options.
    static BackupRun runBackupWith(UserProfileBackupWorker& worker,
                                   const QString& profilePath,
                                   const sak::FolderSelection& folder,
                                   const QString& destPath,
                                   const UserProfileBackupWorker::BackupOptions& options);
};

TestUserProfileBackupWorker::BackupRun TestUserProfileBackupWorker::runBackup(
    UserProfileBackupWorker& worker,
    const QString& profilePath,
    const sak::FolderSelection& folder,
    const QString& destPath) {
    UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = PermissionMode::PreserveOriginal;  // no ACL ops in the test
    return runBackupWith(worker, profilePath, folder, destPath, options);
}

TestUserProfileBackupWorker::BackupRun TestUserProfileBackupWorker::runBackupWith(
    UserProfileBackupWorker& worker,
    const QString& profilePath,
    const sak::FolderSelection& folder,
    const QString& destPath,
    const UserProfileBackupWorker::BackupOptions& options) {
    UserProfile user;
    user.username = QStringLiteral("tester");
    user.profile_path = profilePath;
    user.is_selected = true;
    user.folder_selections = {folder};

    // Pre-create the destination so the disk-space check (which runs before the
    // backup creates the tree) sees a valid storage volume.
    QDir().mkpath(destPath);

    BackupOutcome outcome;
    QObject::connect(
        &worker,
        &UserProfileBackupWorker::backupComplete,
        &worker,
        [&outcome](bool ok, const QString&, const BackupManifest&) {
            outcome.success = ok;
            outcome.done.store(true);
        },
        Qt::DirectConnection);

    worker.startBackup(BackupManifest{}, {user}, destPath, SmartFilter{}, options);

    // QTRY macros cannot return a value from a static helper; poll then join.
    for (int i = 0; i < 500 && !outcome.done.load(); ++i) {
        QTest::qWait(10);
    }
    worker.wait();
    return {outcome.done.load(), outcome.success};
}

void TestUserProfileBackupWorker::backupMissingSelectedFolderReportsFailure() {
    QTemporaryDir srcDir;  // profile root WITHOUT a "Documents" subfolder
    QVERIFY(srcDir.isValid());
    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());

    sak::FolderSelection folder;
    folder.type = sak::FolderType::Documents;
    folder.relative_path = QStringLiteral("Documents");  // missing on disk
    folder.selected = true;

    // A selected folder that does not exist means the backup omitted requested data;
    // it must NOT report a clean success (B7-20).
    UserProfileBackupWorker worker;
    const BackupRun run =
        runBackup(worker, srcDir.path(), folder, destParent.filePath(QStringLiteral("dest")));
    QVERIFY2(run.completed, "backup never emitted backupComplete");
    QVERIFY(!run.success);
}

void TestUserProfileBackupWorker::backupExistingFolderReportsSuccess() {
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    QVERIFY(QDir(srcDir.path()).mkpath(QStringLiteral("Documents")));
    {
        QFile f(srcDir.filePath(QStringLiteral("Documents/hello.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
    }
    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());

    sak::FolderSelection folder;
    folder.type = sak::FolderType::Documents;
    folder.relative_path = QStringLiteral("Documents");
    folder.selected = true;

    // A real, readable folder backs up cleanly -> success (no false failure).
    UserProfileBackupWorker worker;
    const BackupRun run =
        runBackup(worker, srcDir.path(), folder, destParent.filePath(QStringLiteral("dest")));
    QVERIFY2(run.completed, "backup never emitted backupComplete");
    QVERIFY(run.success);

    // The copy really landed. Success alone does not prove it: a filtered-out file is
    // "skipped, not an error", so a filter regression that dropped every file would
    // still report success. Layout is <dest>/<username>/<relative_path>/<file>.
    const QString copied = destParent.filePath(QStringLiteral("dest/tester/Documents/hello.txt"));
    QVERIFY2(QFile::exists(copied), qPrintable("Missing: " + copied));
    QFile copiedFile(copied);
    QVERIFY(copiedFile.open(QIODevice::ReadOnly));
    QCOMPARE(copiedFile.readAll(), QByteArray("hello"));
}

// ============================================================================
// R5-G19-1: the transforms are applied, not merely accepted
// ============================================================================

namespace {

/// Build a one-file profile whose content is a marker that must not survive in
/// cleartext once encryption is on. Returns the marker.
QByteArray seedProfile(const QString& profileRoot) {
    const QByteArray marker = "PROFILE-SECRET-abcdefghijklmnopqrstuvwxyz-0123456789";
    QByteArray content;
    for (int i = 0; i < 200; ++i) {
        content.append(marker);
    }
    if (!QDir(profileRoot).mkpath(QStringLiteral("Documents"))) {
        return {};
    }
    QFile file(QDir(profileRoot).filePath(QStringLiteral("Documents/secret.txt")));
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        return {};
    }
    file.close();
    return content;
}

sak::FolderSelection documentsSelection() {
    sak::FolderSelection folder;
    folder.type = sak::FolderType::Documents;
    folder.relative_path = QStringLiteral("Documents");
    folder.selected = true;
    return folder;
}

QByteArray readAllBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

}  // namespace

void TestUserProfileBackupWorker::encryptedBackupStoresCiphertextAndRestoresIt() {
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    const QByteArray original = seedProfile(srcDir.path());
    QVERIFY(!original.isEmpty());

    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());
    const QString destPath = destParent.filePath(QStringLiteral("dest"));

    UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = PermissionMode::PreserveOriginal;
    options.encrypt = true;
    options.password = QStringLiteral("profile_backup_pw");

    UserProfileBackupWorker worker;
    const BackupRun run =
        runBackupWith(worker, srcDir.path(), documentsSelection(), destPath, options);
    QVERIFY2(run.completed, "backup never emitted backupComplete");
    QVERIFY(run.success);

    const QString stored = destPath + QStringLiteral("/tester/Documents/secret.txt");
    QVERIFY2(QFile::exists(stored), qPrintable("Missing: " + stored));

    // The defect this replaces was a run that reported AES-256 and wrote plaintext, so
    // assert on the STORED BYTES, not on the log line.
    const QByteArray storedBytes = readAllBytes(stored);
    QVERIFY(!storedBytes.isEmpty());
    QVERIFY2(!storedBytes.contains("PROFILE-SECRET"),
             "the stored file still contains the plaintext marker");
    QCOMPARE(sak::backupContainerKind(stored), sak::BackupContainerKind::Encrypted);

    // ...and it is genuinely recoverable, which is what makes it a backup.
    const QString restored = destParent.filePath(QStringLiteral("restored.txt"));
    auto decoded = sak::readBackupFile(stored, restored, options.password);
    QVERIFY2(decoded.has_value(), "the encrypted backup could not be decoded");
    QCOMPARE(readAllBytes(restored), original);
}

void TestUserProfileBackupWorker::compressedBackupStoresContainerAndRestoresIt() {
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    const QByteArray original = seedProfile(srcDir.path());
    QVERIFY(!original.isEmpty());

    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());
    const QString destPath = destParent.filePath(QStringLiteral("dest"));

    UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = PermissionMode::PreserveOriginal;
    options.compress = true;

    UserProfileBackupWorker worker;
    const BackupRun run =
        runBackupWith(worker, srcDir.path(), documentsSelection(), destPath, options);
    QVERIFY2(run.completed, "backup never emitted backupComplete");
    QVERIFY(run.success);

    const QString stored = destPath + QStringLiteral("/tester/Documents/secret.txt");
    QVERIFY(QFile::exists(stored));
    QCOMPARE(sak::backupContainerKind(stored), sak::BackupContainerKind::Plain);

    // A highly repetitive payload must actually get smaller: the removed version defaulted
    // to "Balanced" and wrote a verbatim copy on every run.
    QVERIFY2(QFileInfo(stored).size() < original.size() / 2,
             "the compressed backup is not meaningfully smaller than the source");

    const QString restored = destParent.filePath(QStringLiteral("restored.txt"));
    auto decoded = sak::readBackupFile(stored, restored, QString());
    QVERIFY(decoded.has_value());
    QCOMPARE(readAllBytes(restored), original);
}

void TestUserProfileBackupWorker::encryptionWithoutPasswordRefusesToStart() {
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    QVERIFY(!seedProfile(srcDir.path()).isEmpty());

    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());
    const QString destPath = destParent.filePath(QStringLiteral("dest"));

    UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = PermissionMode::PreserveOriginal;
    options.encrypt = true;  // no password

    UserProfileBackupWorker worker;
    const BackupRun run =
        runBackupWith(worker, srcDir.path(), documentsSelection(), destPath, options);
    QVERIFY2(run.completed, "the refusal must still report a terminal outcome");
    QVERIFY2(!run.success, "encryption without a password must not report success");

    // Fail closed: nothing may have been written, least of all a plaintext copy.
    QVERIFY2(!QFile::exists(destPath + QStringLiteral("/tester/Documents/secret.txt")),
             "a refused encrypted backup still wrote a file");
}

// ============================================================================
// Construction
// ============================================================================

// A duplicate testConstruction() -- same two lines, same single assertion -- used to sit
// here. Construction (and the internal SmartFileFilter / PermissionManager it wires up)
// is exercised by every test in this file, so only the named claim is kept.
void TestUserProfileBackupWorker::testNotRunningInitially() {
    UserProfileBackupWorker worker;
    QVERIFY(!worker.isRunning());
}

// ============================================================================
// Cancel
// ============================================================================

void TestUserProfileBackupWorker::testCancelWhenNotRunning() {
    UserProfileBackupWorker worker;
    {
        QSignalSpy logSpy(&worker, &UserProfileBackupWorker::logMessage);
        QVERIFY(logSpy.isValid());
        // Cancel with no thread to cancel: must not crash, must not start anything, and
        // must still announce itself (cancel() emits on the calling thread, so the spy
        // has the message by the time it returns).
        worker.cancel();
        QVERIFY(!worker.isRunning());
        QCOMPARE(logSpy.count(), 1);
        QVERIFY(logSpy.first().at(0).toString().contains(QStringLiteral("Canceling backup")));
    }

    // ...and the cancel flag it set must not poison the NEXT run: startBackup clears it,
    // so a real backup on this same worker still completes successfully. Without that
    // reset every backup after an idle cancel would report "Backup cancelled".
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    QVERIFY(QDir(srcDir.path()).mkpath(QStringLiteral("Documents")));
    {
        QFile f(srcDir.filePath(QStringLiteral("Documents/hello.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
    }
    QTemporaryDir destParent;
    QVERIFY(destParent.isValid());

    sak::FolderSelection folder;
    folder.type = sak::FolderType::Documents;
    folder.relative_path = QStringLiteral("Documents");
    folder.selected = true;

    const BackupRun run =
        runBackup(worker, srcDir.path(), folder, destParent.filePath(QStringLiteral("dest")));
    QVERIFY2(run.completed, "backup never emitted backupComplete");
    QVERIFY2(run.success, "a cancel with nothing to cancel poisoned the next backup");
}

// ============================================================================
// B7-01: isRunning() now reflects QThread state (true the instant start()
// returns), so destroying a worker right after startBackup() -- with NO prior
// wait() -- cancels and joins the live thread instead of tearing it down
// mid-run. The old late-set member flag left a window where the destructor saw
// "not running" and aborted with "QThread: Destroyed while thread is running".
// ============================================================================

void TestUserProfileBackupWorker::testStartThenImmediateDestroyIsSafe() {
    for (int i = 0; i < 5; ++i) {
        UserProfileBackupWorker worker;
        const UserProfileBackupWorker::BackupOptions options;
        // An empty user list is a backup of nothing and run() returns almost at once,
        // which is what keeps this start-then-destroy loop fast. It previously relied on
        // options.encrypt = true to make run() self-terminate; that abort path is gone
        // with the encryption controls it existed to reject.
        worker.startBackup(
            BackupManifest{}, {}, QDir::tempPath() + "/sak_up_b7_01_dest", SmartFilter{}, options);
        // worker destructs here at end of scope, WITHOUT a prior wait(): must not abort.
    }
    // Reaching this line IS the assertion. A regressed lifetime does not return a wrong
    // value, it kills the process inside the loop above: QThread's destructor calls
    // qFatal("QThread: Destroyed while thread is still running"), which aborts before any
    // QCOMPARE could run. There is no observable to compare.
    QVERIFY(true);
}

// ============================================================================
// B7-08: an untrusted username becomes a backup subdirectory and an untrusted
// folder.relative_path is joined onto both roots -- traversal must be rejected.
// ============================================================================

void TestUserProfileBackupWorker::isSafePathSegment_acceptsNamesRejectsTraversal() {
    QVERIFY(UserProfileBackupWorker::isSafePathSegment(QStringLiteral("Alice")));
    QVERIFY(UserProfileBackupWorker::isSafePathSegment(QStringLiteral("john.doe")));

    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QString()));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral(".")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("..")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("a/b")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("a\\b")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("..\\..\\Windows")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("C:")));
    QVERIFY(!UserProfileBackupWorker::isSafePathSegment(QStringLiteral("a*b")));
}

void TestUserProfileBackupWorker::isSafeRelativePath_acceptsRelativeRejectsEscapes() {
    QVERIFY(UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("AppData/Roaming")));
    QVERIFY(UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("Documents")));
    QVERIFY(UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("sub/dir/file.txt")));

    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QString()));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("../x")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("a/../b")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("a\\..\\b")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("a/./b")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("/etc/passwd")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("C:/Windows")));
    QVERIFY(!UserProfileBackupWorker::isSafeRelativePath(QStringLiteral("\\\\server\\share")));
}

QTEST_MAIN(TestUserProfileBackupWorker)
#include "test_user_profile_backup_worker.moc"
