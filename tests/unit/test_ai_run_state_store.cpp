// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state.h"
#include "sak/ai/ai_run_state_store.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiRunStateStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void emptySessionDirectoryReportsError();
    void saveAndLoadRoundTrips();
    void clearRemovesSnapshot();
    void loadWithoutSnapshotReturnsDefault();
};

void AiRunStateStoreTests::emptySessionDirectoryReportsError() {
    sak::ai::AiRunStateStore store;
    QString error;
    QVERIFY(!store.saveSnapshot({}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(store.runStatePath().isEmpty());
}

void AiRunStateStoreTests::saveAndLoadRoundTrips() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiRunStateStore store(temp_dir.path());

    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_abc");
    state.workflow_id = QStringLiteral("drive_health_deep_check");
    state.status = sak::ai::AiRunStatus::Running;
    state.phase_id = QStringLiteral("collect_evidence");
    state.active_subagents = 1;
    state.completed_subagents = 2;
    state.active_tools = 3;
    state.completed_tools = 4;
    state.message = QStringLiteral("Inspecting drive");

    QString error;
    QVERIFY(store.saveSnapshot(state, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo(store.runStatePath()).exists());

    const auto loaded = store.loadSnapshot(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.run_id, state.run_id);
    QCOMPARE(loaded.workflow_id, state.workflow_id);
    QCOMPARE(loaded.status, state.status);
    QCOMPARE(loaded.phase_id, state.phase_id);
    QCOMPARE(loaded.active_subagents, state.active_subagents);
    QCOMPARE(loaded.completed_subagents, state.completed_subagents);
    QCOMPARE(loaded.active_tools, state.active_tools);
    QCOMPARE(loaded.completed_tools, state.completed_tools);
    QCOMPARE(loaded.message, state.message);
}

void AiRunStateStoreTests::clearRemovesSnapshot() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiRunStateStore store(temp_dir.path());
    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_xyz");
    state.status = sak::ai::AiRunStatus::Completed;

    QVERIFY(store.saveSnapshot(state));
    QVERIFY(QFileInfo(store.runStatePath()).exists());

    QString error;
    QVERIFY(store.clearSnapshot(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo(store.runStatePath()).exists());
    // Clearing again is a no-op.
    QVERIFY(store.clearSnapshot(&error));
}

void AiRunStateStoreTests::loadWithoutSnapshotReturnsDefault() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    QString error;
    const auto state = store.loadSnapshot(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(state.status, sak::ai::AiRunStatus::Idle);
    QVERIFY(state.run_id.isEmpty());
}

QTEST_GUILESS_MAIN(AiRunStateStoreTests)
#include "test_ai_run_state_store.moc"
