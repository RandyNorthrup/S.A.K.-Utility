// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_restore_point_manager.cpp
/// @brief Unit tests for RestorePointManager

#include "sak/elevation_manager.h"
#include "sak/process_runner.h"
#include "sak/restore_point_manager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestRestorePointManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void isElevated_returnsBool();
    void isSystemRestoreEnabled_returnsBool();
    void restoreEnabledFromProbe_failsClosed();
    void createRestorePointPreflight_failsClosedWithoutAdmin();  // R5-G23-4 no-admin
    void listRestorePoints_doesNotCrash();
    void dailyLimitReusesARecentRestorePoint();
    void dailyLimitWithoutARecentPointStillFails();
};

void TestRestorePointManager::construction_default() {
    RestorePointManager manager;
    // The upcast to QObject* is compile-time non-null (RestorePointManager : public QObject),
    // so it verifies nothing; pin the moc name to prove Q_OBJECT is present and namespaced.
    QCOMPARE(QByteArray(manager.metaObject()->className()),
             QByteArrayLiteral("sak::RestorePointManager"));
}

void TestRestorePointManager::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<RestorePointManager>);
    QVERIFY(!std::is_move_constructible_v<RestorePointManager>);
}

void TestRestorePointManager::isElevated_returnsBool() {
    const bool elevated = RestorePointManager::isElevated();
    // (!elevated || elevated) is A||!A -- vacuously true for EVERY implementation, including a
    // hardcoded "return true"; the determinism check below only compares the function to itself.
    // The real contract is delegation: RestorePointManager::isElevated forwards to the one
    // canonical token check (src/core/restore_point_manager.cpp:376-378), and createRestorePoint
    // feeds that value straight into its preflight gate (cpp:157), so a hardcoded true makes a
    // non-elevated process skip the admin refusal and run Checkpoint-Computer. Both sides read
    // the same live token, so this holds on an elevated or an unelevated host alike. Same pin the
    // campaign uses for PermissionManager (tests/unit/test_permission_manager.cpp:465).
    QCOMPARE(elevated, sak::ElevationManager::isElevated());
    // Verify it's a deterministic call -- same result twice
    QCOMPARE(RestorePointManager::isElevated(), elevated);
}

void TestRestorePointManager::isSystemRestoreEnabled_returnsBool() {
    RestorePointManager manager;
    const bool enabled = manager.isSystemRestoreEnabled();
    // Verify deterministic -- same result on repeated call
    QCOMPARE(manager.isSystemRestoreEnabled(), enabled);
}

// B5-10: the enabled decision must fail closed. A failed probe is NOT enabled,
// and only an explicit "ENABLED" from a successful probe counts (the old VSS
// fallback reported enabled whenever VSS was running).
void TestRestorePointManager::restoreEnabledFromProbe_failsClosed() {
    // Successful probe, explicit answers.
    QVERIFY(RestorePointManager::restoreEnabledFromProbe(true, QStringLiteral("ENABLED")));
    QVERIFY(RestorePointManager::restoreEnabledFromProbe(true, QStringLiteral("ENABLED\r\n")));
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(true, QStringLiteral("DISABLED")));
    // Failed probe -> never enabled, regardless of stray output.
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(false, QStringLiteral("ENABLED")));
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(false, QString()));
    // Unexpected/garbage output on a successful probe -> not enabled.
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(true, QStringLiteral("VSS Running")));
    // EXACT match, not substring/prefix/suffix: production compares trimmed() == "ENABLED"
    // (src/core/restore_point_manager.cpp:68) against a script that writes only the bare
    // 'ENABLED'/'DISABLED' sentinels (cpp:56-59). A contains()/endsWith() reading would report
    // "NOT ENABLED" as enabled and a contains()/startsWith() reading would accept ENABLED trailed
    // by a warning -- both are this safety check failing OPEN, which is what B5-10 removed.
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(true, QStringLiteral("NOT ENABLED")));
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(
        true, QStringLiteral("ENABLED\r\nWARNING: not a REG_DWORD")));
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(true, QString()));
}

// R5-G23-4 (no-admin hostile-env dimension): creating a restore point is an elevated operation.
// A non-elevated process must be REFUSED at the preflight, before any elevated work is attempted.
// The elevation gate reads the live token inside createRestorePoint, which a unit test cannot
// force; the pure restorePointPreflightRefusal takes elevation as a parameter so the fail-closed
// refusal is deterministic and testable without spoofing the process token or running the real
// (side-effecting) restore-point creation.
void TestRestorePointManager::createRestorePointPreflight_failsClosedWithoutAdmin() {
    using RPM = RestorePointManager;
    // Not elevated, valid description -> the administrator-privileges refusal (the no-admin guard).
    QCOMPARE(RPM::restorePointPreflightRefusal(QStringLiteral("Before update"), false),
             QStringLiteral("Creating restore points requires administrator privileges."));
    // Empty description is refused first, whether or not elevated (order preserved).
    QCOMPARE(RPM::restorePointPreflightRefusal(QString(), true),
             QStringLiteral("A restore point description is required."));
    QCOMPARE(RPM::restorePointPreflightRefusal(QString(), false),
             QStringLiteral("A restore point description is required."));
    // Elevated AND a valid description -> proceed (empty refusal). This is the ONLY input pair that
    // lets the real elevated work run, so a mutation that dropped the elevation guard would make
    // the (valid, false) case above return empty and turn this test red.
    QVERIFY(RPM::restorePointPreflightRefusal(QStringLiteral("Before update"), true).isEmpty());
    // The pure preflight is only a GATE if the real entry point acts on it; nothing in this file
    // (or anywhere in tests/) reached createRestorePoint, so a body that computed the refusal and
    // proceeded anyway stayed green. Pin the wiring at src/core/restore_point_manager.cpp:157-161:
    // refuse, report the refusal string VERBATIM on restorePointFailed, and never claim success.
    // An empty description is rejected by the FIRST guard on any host, elevated or not
    // (cpp:142-144), so this spawns no PowerShell and creates no restore point.
    RPM manager;
    QSignalSpy failed_spy(&manager, &RPM::restorePointFailed);
    QSignalSpy created_spy(&manager, &RPM::restorePointCreated);
    QVERIFY(!manager.createRestorePoint(QString()));
    QCOMPARE(failed_spy.count(), 1);
    QCOMPARE(failed_spy.at(0).at(0).toString(),
             QStringLiteral("A restore point description is required."));
    QCOMPARE(created_spy.count(), 0);
}

void TestRestorePointManager::listRestorePoints_doesNotCrash() {
    RestorePointManager manager;
    const auto points = manager.listRestorePoints();
    // Verify each restore point has a valid datetime
    for (const auto& [datetime, description] : points) {
        QVERIFY(datetime.isValid());
        QVERIFY(!description.isEmpty());
    }
}

namespace {
ProcessResult checkpointRun(int exit_code, const QString& out, const QString& err) {
    ProcessResult result;
    result.exit_code = exit_code;
    result.std_out = out;
    result.std_err = err;
    return result;
}

const QString kDailyLimitWarning = QStringLiteral(
    "A new system restore point cannot be created because one has already been created within the "
    "past 1440 minutes.");
}  // namespace

// The second uninstall of a day was refused: Windows will not make a second restore point within
// 24 hours, and that refusal was treated as "no restore point". The one it made earlier is a
// rollback point from before this change, so it is reused -- and the user is told which.
void TestRestorePointManager::dailyLimitReusesARecentRestorePoint() {
    const QDateTime now = QDateTime::currentDateTime();
    int queries = 0;
    const auto recent = [&](bool& ok) {
        ++queries;
        ok = true;
        return QVector<QPair<QDateTime, QString>>{
            {now.addSecs(-3 * 3600), QStringLiteral("SAK: Before uninstall Foo")},
            {now.addDays(-9), QStringLiteral("Windows Update")}};
    };
    const auto outcome = RestorePointManager::outcomeOfCheckpoint(
        checkpointRun(1, {}, kDailyLimitWarning), recent, now);
    QCOMPARE(outcome.kind, RestorePointManager::Outcome::Kind::ReusedRecent);
    QVERIFY(outcome.usable());
    QVERIFY2(outcome.message.contains(QStringLiteral("SAK: Before uninstall Foo")),
             qPrintable(outcome.message));
    QCOMPARE(queries, 1);

    // A real creation is a creation, and never consults the list.
    const auto created = RestorePointManager::outcomeOfCheckpoint(
        checkpointRun(0, QStringLiteral("SUCCESS"), {}), recent, now);
    QCOMPARE(created.kind, RestorePointManager::Outcome::Kind::Created);
    QCOMPARE(queries, 1);
}

void TestRestorePointManager::dailyLimitWithoutARecentPointStillFails() {
    const QDateTime now = QDateTime::currentDateTime();
    const auto old_only = [&](bool& ok) {
        ok = true;
        return QVector<QPair<QDateTime, QString>>{{now.addDays(-2), QStringLiteral("Old")}};
    };
    QCOMPARE(RestorePointManager::outcomeOfCheckpoint(
                 checkpointRun(1, {}, kDailyLimitWarning), old_only, now)
                 .kind,
             RestorePointManager::Outcome::Kind::Failed);
    const auto unreadable = [](bool& ok) {
        ok = false;
        return QVector<QPair<QDateTime, QString>>{};
    };
    QCOMPARE(RestorePointManager::outcomeOfCheckpoint(
                 checkpointRun(1, {}, kDailyLimitWarning), unreadable, now)
                 .kind,
             RestorePointManager::Outcome::Kind::Failed);
    // Any other failure is a failure; the list is not even read.
    bool listed = false;
    const auto tracking = [&](bool& ok) {
        listed = true;
        ok = true;
        return QVector<QPair<QDateTime, QString>>{{now, QStringLiteral("Just now")}};
    };
    QCOMPARE(RestorePointManager::outcomeOfCheckpoint(
                 checkpointRun(1, {}, QStringLiteral("Access denied")), tracking, now)
                 .kind,
             RestorePointManager::Outcome::Kind::Failed);
    QVERIFY(!listed);
}

QTEST_MAIN(TestRestorePointManager)
#include "test_restore_point_manager.moc"
