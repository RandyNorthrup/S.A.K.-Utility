// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_restore_point_manager.cpp
/// @brief Unit tests for RestorePointManager

#include "sak/restore_point_manager.h"

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
    void listRestorePoints_doesNotCrash();
};

void TestRestorePointManager::construction_default() {
    RestorePointManager manager;
    QVERIFY(dynamic_cast<QObject*>(&manager) != nullptr);
}

void TestRestorePointManager::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<RestorePointManager>);
    QVERIFY(!std::is_move_constructible_v<RestorePointManager>);
}

void TestRestorePointManager::isElevated_returnsBool() {
    const bool elevated = RestorePointManager::isElevated();
    // Unit tests typically run without elevation
    QVERIFY(!elevated || elevated);
    // Verify it's a deterministic call — same result twice
    QCOMPARE(RestorePointManager::isElevated(), elevated);
}

void TestRestorePointManager::isSystemRestoreEnabled_returnsBool() {
    RestorePointManager manager;
    const bool enabled = manager.isSystemRestoreEnabled();
    // Verify deterministic — same result on repeated call
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
    QVERIFY(!RestorePointManager::restoreEnabledFromProbe(true, QString()));
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

QTEST_MAIN(TestRestorePointManager)
#include "test_restore_point_manager.moc"
