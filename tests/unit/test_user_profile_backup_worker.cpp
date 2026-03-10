// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_worker.h"

#include <QTest>

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

QTEST_MAIN(TestUserProfileBackupWorker)
#include "test_user_profile_backup_worker.moc"
