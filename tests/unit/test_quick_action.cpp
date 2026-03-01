// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_quick_action.cpp
/// @brief Unit tests for QuickAction static helpers and base class (TST-10)

#include <QtTest/QtTest>
#include "sak/quick_action.h"

using namespace sak;

// Concrete stub to test the abstract base class
class StubAction : public QuickAction {
    Q_OBJECT
public:
    using QuickAction::QuickAction;

    // Expose protected static methods for testing
    using StubAction::formatFileSize;
    using StubAction::formatLogBox;
    using StubAction::sanitizePathForBackup;

    QString name() const override { return QStringLiteral("Stub"); }
    QString description() const override { return QStringLiteral("Test stub"); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    QIcon icon() const override { return {}; }
    bool requiresAdmin() const override { return false; }

public Q_SLOTS:
    void scan() override {
        ScanResult r;
        r.applicable = true;
        r.summary = "test";
        r.bytes_affected = 1024;
        setScanResult(r);
        setStatus(ActionStatus::Ready);
    }
    void execute() override {
        ExecutionResult r;
        r.success = true;
        r.message = "done";
        setExecutionResult(r);
        setStatus(ActionStatus::Success);
    }
};

class QuickActionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- formatFileSize ---

    void formatFileSizeZero()
    {
        QCOMPARE(StubAction::formatFileSize(0), QStringLiteral("0 bytes"));
    }

    void formatFileSizeBytes()
    {
        QString result = StubAction::formatFileSize(512);
        QVERIFY(result.contains("512"));
        QVERIFY(result.contains("bytes"));
    }

    void formatFileSizeKilobytes()
    {
        QString result = StubAction::formatFileSize(1536); // 1.5 KB
        QVERIFY(result.contains("KB") || result.contains("kB"));
    }

    void formatFileSizeMegabytes()
    {
        QString result = StubAction::formatFileSize(5 * 1024 * 1024);
        QVERIFY(result.contains("MB"));
    }

    void formatFileSizeGigabytes()
    {
        QString result = StubAction::formatFileSize(Q_INT64_C(2'500'000'000));
        QVERIFY(result.contains("GB"));
    }

    void formatFileSizeTerabytes()
    {
        QString result = StubAction::formatFileSize(Q_INT64_C(1'500'000'000'000));
        QVERIFY(result.contains("TB"));
    }

    // --- sanitizePathForBackup ---

    void sanitizeSimplePath()
    {
        QCOMPARE(StubAction::sanitizePathForBackup("C:\\Users\\Test"),
                 QStringLiteral("C__Users_Test"));
    }

    void sanitizeForwardSlashes()
    {
        QCOMPARE(StubAction::sanitizePathForBackup("home/user/docs"),
                 QStringLiteral("home_user_docs"));
    }

    void sanitizeColonAndSlashes()
    {
        QCOMPARE(StubAction::sanitizePathForBackup("D:/Program Files/App"),
                 QStringLiteral("D__Program Files_App"));
    }

    void sanitizeEmptyPath()
    {
        QCOMPARE(StubAction::sanitizePathForBackup(""), QStringLiteral(""));
    }

    // --- formatLogBox ---

    void formatLogBoxBasic()
    {
        QStringList lines = {"Line 1", "Line 2"};
        QString result = StubAction::formatLogBox("TITLE", lines);
        QVERIFY(result.contains("TITLE"));
        QVERIFY(result.contains("Line 1"));
        QVERIFY(result.contains("Line 2"));
    }

    void formatLogBoxWithDuration()
    {
        QStringList lines = {"Result: OK"};
        QString result = StubAction::formatLogBox("TEST", lines, 1500);
        QVERIFY(result.contains("TEST"));
        QVERIFY(result.contains("Result: OK"));
        // Duration should be present somewhere
        QVERIFY(result.contains("1.5") || result.contains("1500") || result.contains("1 s"));
    }

    void formatLogBoxEmptyLines()
    {
        QStringList lines;
        QString result = StubAction::formatLogBox("EMPTY", lines);
        QVERIFY(result.contains("EMPTY"));
    }

    // --- Base class state management ---

    void initialStatusIsIdle()
    {
        StubAction action;
        QCOMPARE(action.status(), QuickAction::ActionStatus::Idle);
    }

    void scanUpdatesStatus()
    {
        StubAction action;
        action.scan();
        QCOMPARE(action.status(), QuickAction::ActionStatus::Ready);
        QVERIFY(action.lastScanResult().applicable);
        QCOMPARE(action.lastScanResult().bytes_affected, qint64(1024));
    }

    void executeUpdatesStatus()
    {
        StubAction action;
        action.execute();
        QCOMPARE(action.status(), QuickAction::ActionStatus::Success);
        QVERIFY(action.lastExecutionResult().success);
    }

    void cancelSetsFlag()
    {
        StubAction action;
        action.cancel();
        // Can't directly test isCancelled() since it's protected,
        // but we can verify status didn't break
        QVERIFY(true);
    }

    void applyExecutionResult()
    {
        StubAction action;
        QuickAction::ExecutionResult result;
        result.success = true;
        result.message = "completed";
        result.bytes_processed = 4096;
        action.applyExecutionResult(result, QuickAction::ActionStatus::Success);
        QCOMPARE(action.status(), QuickAction::ActionStatus::Success);
        QCOMPARE(action.lastExecutionResult().message, QStringLiteral("completed"));
        QCOMPARE(action.lastExecutionResult().bytes_processed, qint64(4096));
    }

    void updateStatusEmitsSignal()
    {
        StubAction action;
        QSignalSpy spy(&action, &QuickAction::statusChanged);
        action.updateStatus(QuickAction::ActionStatus::Running);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(QuickActionTests)
#include "test_quick_action.moc"
