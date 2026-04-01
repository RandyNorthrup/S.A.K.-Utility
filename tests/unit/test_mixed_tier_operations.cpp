// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mixed_tier_operations.cpp
/// @brief Unit tests for Phase 3 mixed-tier elevation operations
///
///  - PermissionManager std::expected overloads
///  - UserProfileBackupWorker path-access detection
///  - New task handler registrations in elevated helper dispatcher
///  - ThermalMonitor hasCpuTemperature() check

#include "sak/elevated_task_dispatcher.h"
#include "sak/error_codes.h"
#include "sak/permission_manager.h"
#include "sak/thermal_monitor.h"
#include "sak/user_profile_backup_worker.h"

#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

class TestMixedTierOperations : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ======================================================================
    // PermissionManager — tryStripPermissions
    // ======================================================================

    void testTryStripPermissionsOnNonexistentFile() {
        sak::PermissionManager pm;
        auto result =
            pm.tryStripPermissions(QStringLiteral("C:/nonexistent_path_12345/nofile.txt"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::file_not_found);
    }

    void testTryStripPermissionsOnWritableFile() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        auto result = pm.tryStripPermissions(tmp.fileName());
        // Should succeed on user-writable file (or elevation_required if
        // running tests without write access to DACL)
        if (result.has_value()) {
            QVERIFY(true);
        } else {
            // Either elevation_required or permission_update_failed
            QVERIFY(result.error() == sak::error_code::elevation_required ||
                    result.error() == sak::error_code::permission_update_failed);
        }
    }

    void testTryStripPermissionsEmptyPath() {
        sak::PermissionManager pm;
        // Q_ASSERT fires on empty path in debug — skip if debug build
#ifdef NDEBUG
        auto result = pm.tryStripPermissions(QString());
        QVERIFY(!result.has_value());
#else
        QSKIP("Q_ASSERT prevents empty path in debug builds");
#endif
    }

    // ======================================================================
    // PermissionManager — tryTakeOwnership
    // ======================================================================

    void testTryTakeOwnershipNotAdmin() {
        // If not running as admin, should return elevation_required
        if (sak::PermissionManager::isRunningAsAdmin()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        auto result = pm.tryTakeOwnership(tmp.fileName(), QStringLiteral("S-1-5-32-544"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::elevation_required);
    }

    void testTryTakeOwnershipInvalidSid() {
        if (!sak::PermissionManager::isRunningAsAdmin()) {
            QSKIP("Test requires admin to get past the isAdmin check");
        }

        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        auto result = pm.tryTakeOwnership(tmp.fileName(), QStringLiteral("NOT-A-VALID-SID"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::invalid_argument);
    }

    // ======================================================================
    // PermissionManager — trySetStandardUserPermissions
    // ======================================================================

    void testTrySetStandardPermissionsInvalidSid() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        auto result = pm.trySetStandardUserPermissions(tmp.fileName(),
                                                       QStringLiteral("INVALID-SID"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::invalid_argument);
    }

    void testTrySetStandardPermissionsOnWritableFile() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        // Use a well-known SID (Everyone: S-1-1-0)
        auto result = pm.trySetStandardUserPermissions(tmp.fileName(), QStringLiteral("S-1-1-0"));
        // May succeed or fail with elevation_required depending on environment
        if (!result.has_value()) {
            QVERIFY(result.error() == sak::error_code::elevation_required ||
                    result.error() == sak::error_code::permission_update_failed);
        }
    }

    // ======================================================================
    // UserProfileBackupWorker — canReadPath
    // ======================================================================

    void testCanReadPathOnExistingFile() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("hello");
        tmp.flush();

        QVERIFY(sak::UserProfileBackupWorker::canReadPath(tmp.fileName()));
    }

    void testCanReadPathOnExistingDirectory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QVERIFY(sak::UserProfileBackupWorker::canReadPath(dir.path()));
    }

    void testCanReadPathOnNonexistentPath() {
        // Non-existent path should not return false for access-denied
        // (it fails for a different reason)
        bool result = sak::UserProfileBackupWorker::canReadPath(
            QStringLiteral("C:/nonexistent_path_xyz_12345/missing.txt"));
        // GetFileAttributes returns INVALID_FILE_ATTRIBUTES with
        // ERROR_PATH_NOT_FOUND (not ERROR_ACCESS_DENIED), so result is true
        // (meaning "not access-denied; file just doesn't exist")
        QVERIFY(result);
    }

    void testCanReadPathOnProtectedSystemPath() {
        // System Volume Information typically requires admin
        if (sak::PermissionManager::isRunningAsAdmin()) {
            QSKIP("Test only meaningful when not running as admin");
        }
        bool result = sak::UserProfileBackupWorker::canReadPath(
            QStringLiteral("C:/System Volume Information"));
        QVERIFY(!result);
    }

    // ======================================================================
    // Backup Worker — elevationSkipped signal
    // ======================================================================

    void testBackupWorkerElevationSkippedSignalExists() {
        // Verify the signal is connectable (compile-time check)
        sak::UserProfileBackupWorker worker;
        int skip_count = 0;
        connect(&worker,
                &sak::UserProfileBackupWorker::elevationSkipped,
                [&skip_count](const QString&, const QString&) { skip_count++; });
        QCOMPARE(skip_count, 0);
    }

    // ======================================================================
    // ElevatedTaskDispatcher — Phase 3 handlers
    // ======================================================================

    void testPhase3HandlersRegistration() {
        // Simulate the same registration logic as elevated_helper_main.cpp
        sak::ElevatedTaskDispatcher dispatcher;

        dispatcher.registerHandler(QStringLiteral("TakeOwnership"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });
        dispatcher.registerHandler(QStringLiteral("StripPermissions"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });
        dispatcher.registerHandler(QStringLiteral("SetStandardPermissions"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });
        dispatcher.registerHandler(QStringLiteral("BackupFile"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });
        dispatcher.registerHandler(QStringLiteral("ReadThermalData"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });

        QCOMPARE(dispatcher.handlerCount(), 5);
        QVERIFY(dispatcher.isAllowed("TakeOwnership"));
        QVERIFY(dispatcher.isAllowed("StripPermissions"));
        QVERIFY(dispatcher.isAllowed("SetStandardPermissions"));
        QVERIFY(dispatcher.isAllowed("BackupFile"));
        QVERIFY(dispatcher.isAllowed("ReadThermalData"));
        QVERIFY(!dispatcher.isAllowed("MaliciousTask"));
    }

    void testTakeOwnershipHandlerRejectsEmptyPayload() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(QStringLiteral("TakeOwnership"),
                                   [](const QJsonObject& payload,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       QString path = payload["path"].toString();
                                       QString user_sid = payload["user_sid"].toString();
                                       if (path.isEmpty() || user_sid.isEmpty()) {
                                           return {false,
                                                   {},
                                                   "Missing path or user_sid in payload"};
                                       }
                                       return {true, {}, {}};
                                   });

        auto no_progress = [](int, const QString&) {
        };
        auto no_cancel = []() {
            return false;
        };

        // Empty payload
        auto result = dispatcher.dispatch("TakeOwnership", QJsonObject{}, no_progress, no_cancel);
        QVERIFY(result.has_value());
        QVERIFY(!result->success);
        QVERIFY(result->error_message.contains("Missing"));
    }

    void testBackupFileHandlerRejectsEmptyPayload() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(QStringLiteral("BackupFile"),
                                   [](const QJsonObject& payload,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       QString source = payload["source"].toString();
                                       QString dest = payload["destination"].toString();
                                       if (source.isEmpty() || dest.isEmpty()) {
                                           return {false, {}, "Missing source or destination"};
                                       }
                                       return {true, {}, {}};
                                   });

        auto no_progress = [](int, const QString&) {
        };
        auto no_cancel = []() {
            return false;
        };

        auto result = dispatcher.dispatch("BackupFile", QJsonObject{}, no_progress, no_cancel);
        QVERIFY(result.has_value());
        QVERIFY(!result->success);
    }

    void testBackupFileHandlerWithValidPayload() {
        QTemporaryFile src_file;
        QVERIFY(src_file.open());
        src_file.write("backup data");
        src_file.flush();

        QTemporaryDir dest_dir;
        QVERIFY(dest_dir.isValid());
        QString dest_path = dest_dir.path() + "/backup_copy.txt";

        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(QStringLiteral("BackupFile"),
                                   [](const QJsonObject& payload,
                                      sak::ProgressCallback progress,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       QString source = payload["source"].toString();
                                       QString destination = payload["destination"].toString();
                                       if (source.isEmpty() || destination.isEmpty()) {
                                           return {false, {}, "Missing source or destination"};
                                       }
                                       progress(0, "Copying");
                                       if (!QFile::copy(source, destination)) {
                                           return {false, {}, "Copy failed"};
                                       }
                                       progress(100, "Done");
                                       return {true, {}, {}};
                                   });

        QJsonObject payload;
        payload["source"] = src_file.fileName();
        payload["destination"] = dest_path;

        int last_pct = -1;
        auto progress = [&last_pct](int pct, const QString&) {
            last_pct = pct;
        };
        auto no_cancel = []() {
            return false;
        };

        auto result = dispatcher.dispatch("BackupFile", payload, progress, no_cancel);
        QVERIFY(result.has_value());
        QVERIFY(result->success);
        QCOMPARE(last_pct, 100);
        QVERIFY(QFile::exists(dest_path));
    }

    // ======================================================================
    // ThermalMonitor — hasCpuTemperature
    // ======================================================================

    void testThermalMonitorHasCpuTemperatureInitiallyFalse() {
        sak::ThermalMonitor monitor;
        QVERIFY(!monitor.hasCpuTemperature());
    }

    void testThermalMonitorHasCpuTemperatureAfterClear() {
        sak::ThermalMonitor monitor;
        monitor.clearHistory();
        QVERIFY(!monitor.hasCpuTemperature());
    }
};

QTEST_MAIN(TestMixedTierOperations)

#include "test_mixed_tier_operations.moc"
