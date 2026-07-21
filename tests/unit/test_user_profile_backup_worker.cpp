// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_worker.h"

#include <QDir>
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
    // ── Construction ────────────────────────────────────────
    void testConstruction();
    void testNotRunningInitially();

    // ── Cancel ──────────────────────────────────────────────
    void testCancelWhenNotRunning();

    // ── Encryption not supported (P06-25) ───────────────────
    void testEncryptedBackupRefusedFailsClosed();
};

// ============================================================================
// Construction
// ============================================================================

void TestUserProfileBackupWorker::testConstruction() {
    UserProfileBackupWorker worker;
    // Should not crash; worker created with internal
    // SmartFileFilter and PermissionManager
    QVERIFY(!worker.isRunning());
}

void TestUserProfileBackupWorker::testNotRunningInitially() {
    UserProfileBackupWorker worker;
    QVERIFY(!worker.isRunning());
}

// ============================================================================
// Cancel
// ============================================================================

void TestUserProfileBackupWorker::testCancelWhenNotRunning() {
    UserProfileBackupWorker worker;
    // Cancel when not running should not crash
    worker.cancel();
    QVERIFY(!worker.isRunning());
}

// ============================================================================
// Encryption not supported (P06-25): a backup requesting encryption must abort
// with a failure instead of writing plaintext copies mislabeled as AES-256.
// ============================================================================

void TestUserProfileBackupWorker::testEncryptedBackupRefusedFailsClosed() {
    UserProfileBackupWorker worker;

    UserProfile user;
    user.username = "tester";
    user.profile_path = QDir::tempPath();
    user.is_selected = true;

    std::atomic<bool> done{false};
    bool successFlag = true;
    QString message;
    // DirectConnection avoids marshalling BackupManifest (no metatype); the
    // captures are read only after wait() joins the worker thread.
    QObject::connect(
        &worker,
        &UserProfileBackupWorker::backupComplete,
        &worker,
        [&](bool ok, const QString& msg, const BackupManifest&) {
            successFlag = ok;
            message = msg;
            done.store(true);
        },
        Qt::DirectConnection);

    UserProfileBackupWorker::BackupOptions options;
    options.encrypt = true;
    options.password = "secret";
    worker.startBackup(BackupManifest{},
                       {user},
                       QDir::tempPath() + "/sak_up_backup_test_dest",
                       SmartFilter{},
                       options);

    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 5000);
    worker.wait();
    QVERIFY(!successFlag);
    QVERIFY(message.contains("not supported"));
}

QTEST_MAIN(TestUserProfileBackupWorker)
#include "test_user_profile_backup_worker.moc"
