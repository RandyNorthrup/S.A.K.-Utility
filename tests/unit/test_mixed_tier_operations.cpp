// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mixed_tier_operations.cpp
/// @brief Unit tests for Phase 3 mixed-tier elevation operations
///
///  - PermissionManager std::expected overloads
///  - UserProfileBackupWorker path-access detection
///  - New task handler registrations in elevated helper dispatcher

#include "sak/elevated_task_dispatcher.h"
#include "sak/error_codes.h"
#include "sak/permission_manager.h"
#include "sak/user_profile_backup_worker.h"

#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

class TestMixedTierOperations : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ======================================================================
    // PermissionManager -- tryStripPermissions
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
        const QString path = tmp.fileName();

        sak::PermissionManager pm;
        auto result = pm.tryStripPermissions(path);
        // Both outcomes carry a contract. On success the manager must be left with NO error
        // text and the DACL must be an EMPTY (re-inheriting) one, never the NULL DACL that
        // serializes as NO_ACCESS_CONTROL and grants everyone full access. On failure the code
        // must be one of the two documented ones AND the failure must be explained. The old
        // success branch was QVERIFY(true), so a strip that silently nulled the DACL passed.
        if (result.has_value()) {
            QVERIFY2(pm.getLastError().isEmpty(), qPrintable(pm.getLastError()));
            tmp.close();  // read the descriptor back without our own open handle in the way
            const QString sddl = pm.getSecurityDescriptorSddl(path);
            QVERIFY(!sddl.isEmpty());
            QVERIFY2(!sddl.contains(QStringLiteral("NO_ACCESS_CONTROL")), qPrintable(sddl));
        } else {
            QVERIFY(result.error() == sak::error_code::elevation_required ||
                    result.error() == sak::error_code::permission_update_failed);
            QVERIFY(!pm.getLastError().isEmpty());
        }
    }

    void testTryStripPermissionsEmptyPath() {
        // The debug-only QSKIP was stale: tryStripPermissions has no Q_ASSERT on the path, it
        // runs QFileInfo::exists() first, which is false for an empty path. Both configurations
        // now run the test, and it pins the specific code instead of only "not a value".
        sak::PermissionManager pm;
        auto result = pm.tryStripPermissions(QString());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::file_not_found);
    }

    // ======================================================================
    // PermissionManager -- tryTakeOwnership
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
        // The admin gate runs BEFORE the SID is parsed, so the expected error depends on how the
        // suite was launched. Assert BOTH orderings rather than skipping: the old QSKIP fired in
        // every non-elevated run, i.e. in practice always, so this name covered nothing.
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("test");
        tmp.flush();

        sak::PermissionManager pm;
        auto result = pm.tryTakeOwnership(tmp.fileName(), QStringLiteral("NOT-A-VALID-SID"));
        QVERIFY(!result.has_value());
        const auto expected = sak::PermissionManager::isRunningAsAdmin()
                                  ? sak::error_code::invalid_argument
                                  : sak::error_code::elevation_required;
        QCOMPARE(result.error(), expected);
    }

    // ======================================================================
    // PermissionManager -- trySetStandardUserPermissions
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
        const QString path = tmp.fileName();

        sak::PermissionManager pm;
        // Use a well-known SID (Everyone: S-1-1-0)
        auto result = pm.trySetStandardUserPermissions(path, QStringLiteral("S-1-1-0"));
        // May succeed or fail with elevation_required depending on environment, but the success
        // path was previously unasserted. The ACL this call installs is PROTECTED (inheritance
        // blocked, "D:P") and must keep SYSTEM and Administrators alongside the named user, or
        // the OS and every admin lose access to the file.
        if (result.has_value()) {
            tmp.close();  // read the descriptor back without our own open handle in the way
            const QString sddl = pm.getSecurityDescriptorSddl(path);
            QVERIFY(!sddl.isEmpty());
            QVERIFY2(sddl.contains(QStringLiteral("D:P")), qPrintable(sddl));
            QVERIFY2(sddl.contains(QStringLiteral(";SY)")), qPrintable(sddl));
            QVERIFY2(sddl.contains(QStringLiteral(";BA)")), qPrintable(sddl));
        } else {
            QVERIFY(result.error() == sak::error_code::elevation_required ||
                    result.error() == sak::error_code::permission_update_failed);
            QVERIFY(!pm.getLastError().isEmpty());
        }
    }

    // ======================================================================
    // UserProfileBackupWorker -- canReadPath
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
    // Backup Worker -- elevationSkipped signal
    // ======================================================================

    void testBackupWorkerElevationSkippedSignalExists() {
        // The old body connected a counter and asserted it was still 0. Nothing emits during
        // the test, so that held no matter what the signal did -- a dead observer. Emit the
        // signal through the meta-object instead: that proves it is registered with the
        // (QString, QString) signature the wizard connects to and that both arguments arrive.
        sak::UserProfileBackupWorker worker;
        int skip_count = 0;
        QString seen_path;
        QString seen_owner;
        connect(&worker,
                &sak::UserProfileBackupWorker::elevationSkipped,
                [&](const QString& path, const QString& owner) {
                    ++skip_count;
                    seen_path = path;
                    seen_owner = owner;
                });

        QVERIFY(
            QMetaObject::invokeMethod(&worker,
                                      "elevationSkipped",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("D:/profiles/x/ntuser.dat")),
                                      Q_ARG(QString, QStringLiteral("S-1-5-18"))));
        QCOMPARE(skip_count, 1);
        QCOMPARE(seen_path, QStringLiteral("D:/profiles/x/ntuser.dat"));
        QCOMPARE(seen_owner, QStringLiteral("S-1-5-18"));
    }

    // ======================================================================
    // ElevatedTaskDispatcher -- Phase 3 handlers
    // ======================================================================

    void testPhase3HandlersRegistration() {
        // NOTE: this exercises the dispatcher's allowlist, not the helper's registration --
        // registerAllTasks() lives in the elevated_helper executable's main TU and cannot be
        // linked here, so a handler dropped from the helper would NOT fail this test.
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
        dispatcher.registerHandler(QStringLiteral("ReadPartitionProbe"),
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });

        QCOMPARE(dispatcher.handlerCount(), 6);
        QVERIFY(dispatcher.isAllowed("TakeOwnership"));
        QVERIFY(dispatcher.isAllowed("StripPermissions"));
        QVERIFY(dispatcher.isAllowed("SetStandardPermissions"));
        QVERIFY(dispatcher.isAllowed("BackupFile"));
        QVERIFY(dispatcher.isAllowed("ReadThermalData"));
        QVERIFY(dispatcher.isAllowed("ReadPartitionProbe"));
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
        // The dispatcher must hand the handler's diagnosis back verbatim; asserting only
        // "!success" left a dispatcher that dropped error_message undetected.
        QCOMPARE(result->error_message, QStringLiteral("Missing source or destination"));
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
};

QTEST_MAIN(TestMixedTierOperations)

#include "test_mixed_tier_operations.moc"
