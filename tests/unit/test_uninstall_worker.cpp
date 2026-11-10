// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_uninstall_worker.cpp
/// @brief Unit tests for UninstallWorker

#include "sak/advanced_uninstall_types.h"
#include "sak/uninstall_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestUninstallWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void construction_isWorkerBase();
    void programInfo_defaults();
    void leftoverItem_defaults();
    void uninstallReport_defaults();
    void scanLevel_values();
    void mode_values();
    void nativeUninstallSucceeded_classifiesExitCodes();
};

void TestUninstallWorker::construction_default() {
    ProgramInfo program;
    program.displayName = QStringLiteral("Test Program");
    UninstallWorker worker(program, UninstallWorker::Mode::Standard, ScanLevel::Safe, false);
    // The dynamic_cast to a compile-time base could never be null. Pin the real post-construction
    // invariant: a freshly built, un-started worker is idle and not stop-requested.
    QVERIFY(!worker.isExecuting());
    QVERIFY(!worker.stopRequested());
}

void TestUninstallWorker::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<UninstallWorker>);
    QVERIFY(!std::is_move_constructible_v<UninstallWorker>);
}

void TestUninstallWorker::construction_isWorkerBase() {
    ProgramInfo program;
    program.displayName = QStringLiteral("Test Program");
    UninstallWorker worker(program, UninstallWorker::Mode::Standard, ScanLevel::Safe, false);
    QVERIFY(dynamic_cast<WorkerBase*>(&worker) != nullptr);
}

void TestUninstallWorker::programInfo_defaults() {
    ProgramInfo info;
    QVERIFY(info.displayName.isEmpty());
    QVERIFY(info.publisher.isEmpty());
    QCOMPARE(info.estimatedSizeKB, static_cast<qint64>(0));
    QCOMPARE(info.actualSizeBytes, static_cast<qint64>(0));
    QVERIFY(!info.isSystemComponent);
    QVERIFY(!info.isOrphaned);
    QVERIFY(!info.isBloatware);
}

void TestUninstallWorker::leftoverItem_defaults() {
    LeftoverItem item;
    QCOMPARE(item.type, LeftoverItem::Type::File);
    QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
    QVERIFY(item.path.isEmpty());
    QVERIFY(item.description.isEmpty());
    QCOMPARE(item.sizeBytes, static_cast<qint64>(0));
    QVERIFY(!item.selected);
}

void TestUninstallWorker::uninstallReport_defaults() {
    UninstallReport report;
    QVERIFY(report.programName.isEmpty());
    QVERIFY(!report.restorePointCreated);
    QCOMPARE(report.uninstallResult, UninstallReport::UninstallResult::Success);
    QCOMPARE(report.nativeExitCode, 0);
    QCOMPARE(report.filesDeleted, 0);
    QCOMPARE(report.foldersDeleted, 0);
    QCOMPARE(report.registryKeysDeleted, 0);
    QCOMPARE(report.failedDeletions, 0);
}

void TestUninstallWorker::scanLevel_values() {
    QCOMPARE(static_cast<int>(ScanLevel::Safe), 0);
    QCOMPARE(static_cast<int>(ScanLevel::Moderate), 1);
    QCOMPARE(static_cast<int>(ScanLevel::Advanced), 2);
}

void TestUninstallWorker::mode_values() {
    QCOMPARE(static_cast<int>(UninstallWorker::Mode::Standard), 0);
    QCOMPARE(static_cast<int>(UninstallWorker::Mode::ForcedUninstall), 1);
    QCOMPARE(static_cast<int>(UninstallWorker::Mode::UwpRemove), 2);
    QCOMPARE(static_cast<int>(UninstallWorker::Mode::RegistryOnly), 3);
}

void TestUninstallWorker::nativeUninstallSucceeded_classifiesExitCodes() {
    // Exit status 0 with exit code 0 is success.
    QVERIFY(UninstallWorker::nativeUninstallSucceeded(0, 0, false));
    // 3010 = MSI reboot-required: a SUCCESS that asks for a restart. It must be accepted (and its
    // code carried into the report), not treated as a failure.
    QVERIFY(UninstallWorker::nativeUninstallSucceeded(0, 3010, false));
    // A real failure code (e.g. 1603 fatal MSI error) is not success.
    QVERIFY(!UninstallWorker::nativeUninstallSucceeded(0, 1603, false));
    // A cancelled run is never success, even with a zero exit code.
    QVERIFY(!UninstallWorker::nativeUninstallSucceeded(0, 0, true));
    // A non-zero process exit status is never success.
    QVERIFY(!UninstallWorker::nativeUninstallSucceeded(1, 0, false));
}

QTEST_MAIN(TestUninstallWorker)
#include "test_uninstall_worker.moc"
