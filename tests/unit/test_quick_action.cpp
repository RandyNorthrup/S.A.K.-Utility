// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_quick_action.cpp
/// @brief Unit tests for QuickAction static helpers and base class (TST-10)

#include "sak/quick_action.h"

#include <QtTest/QtTest>

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

    // Expose the protected cancellation flag so cancel() can be asserted on
    // instead of merely called.
    using QuickAction::isCancelled;

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

    void formatFileSizeZero() {
        QCOMPARE(StubAction::formatFileSize(0), QStringLiteral("0 bytes"));
    }

    // The unit suffix alone would still pass with a wrong magnitude or a
    // silently changed precision, so pin the whole rendered string. QString::arg
    // formats "%1" through the C locale, so the decimal point is not
    // machine-dependent.

    void formatFileSizeBytes() {
        QCOMPARE(StubAction::formatFileSize(512), QStringLiteral("512 bytes"));
    }

    void formatFileSizeKilobytes() {
        QCOMPARE(StubAction::formatFileSize(1536), QStringLiteral("1.5 KB"));
    }

    void formatFileSizeMegabytes() {
        QCOMPARE(StubAction::formatFileSize(5 * 1024 * 1024), QStringLiteral("5.0 MB"));
    }

    void formatFileSizeGigabytes() {
        // 2'500'000'000 / 1024^3 = 2.3283..., rendered with two decimals.
        QCOMPARE(StubAction::formatFileSize(Q_INT64_C(2'500'000'000)), QStringLiteral("2.33 GB"));
    }

    void formatFileSizeTerabytes() {
        // 1'500'000'000'000 / 1024^4 = 1.3642..., rendered with two decimals.
        QCOMPARE(StubAction::formatFileSize(Q_INT64_C(1'500'000'000'000)),
                 QStringLiteral("1.36 TB"));
    }

    // --- sanitizePathForBackup ---

    void sanitizeSimplePath() {
        QCOMPARE(StubAction::sanitizePathForBackup("X:\\Profiles\\Test"),
                 QStringLiteral("X__Profiles_Test"));
    }

    void sanitizeForwardSlashes() {
        QCOMPARE(StubAction::sanitizePathForBackup("home/user/docs"),
                 QStringLiteral("home_user_docs"));
    }

    void sanitizeColonAndSlashes() {
        QCOMPARE(StubAction::sanitizePathForBackup("D:/Program Files/App"),
                 QStringLiteral("D__Program Files_App"));
    }

    void sanitizeEmptyPath() {
        QCOMPARE(StubAction::sanitizePathForBackup(""), QStringLiteral(""));
    }

    // --- formatLogBox ---

    void formatLogBoxBasic() {
        QStringList lines = {"Line 1", "Line 2"};
        QString result = StubAction::formatLogBox("TITLE", lines);
        // Pin the exact box: fields are leftJustified(65), so pad counts are 65 minus token length.
        const QString border =
            QStringLiteral("+================================================================+\n");
        QCOMPARE(result,
                 border + QStringLiteral("| TITLE") + QString(60, ' ') + QStringLiteral("|\n") +
                     border + QStringLiteral("| Line 1") + QString(59, ' ') +
                     QStringLiteral("|\n") + QStringLiteral("| Line 2") + QString(59, ' ') +
                     QStringLiteral("|\n") + border);
    }

    void formatLogBoxWithDuration() {
        QStringList lines = {"Result: OK"};
        QString result = StubAction::formatLogBox("TEST", lines, 1500);
        // 1500 ms -> "1.50 seconds"; the footer is intentionally not box-aligned (46-space pad).
        const QString border =
            QStringLiteral("+================================================================+\n");
        QCOMPARE(result,
                 border + QStringLiteral("| TEST") + QString(61, ' ') + QStringLiteral("|\n") +
                     border + QStringLiteral("| Result: OK") + QString(55, ' ') +
                     QStringLiteral("|\n") + border +
                     QStringLiteral("| Completed in: 1.50 seconds") + QString(46, ' ') +
                     QStringLiteral("|\n") + border);
    }

    void formatLogBoxEmptyLines() {
        QStringList lines;
        QString result = StubAction::formatLogBox("EMPTY", lines);
        // No content lines and no duration -> a 3-border box; pins EMPTY and footer absence at
        // once.
        const QString border =
            QStringLiteral("+================================================================+\n");
        QCOMPARE(result,
                 border + QStringLiteral("| EMPTY") + QString(60, ' ') + QStringLiteral("|\n") +
                     border + border);
    }

    // --- Base class state management ---

    void initialStatusIsIdle() {
        StubAction action;
        QCOMPARE(action.status(), QuickAction::ActionStatus::Idle);
    }

    void scanUpdatesStatus() {
        StubAction action;
        action.scan();
        QCOMPARE(action.status(), QuickAction::ActionStatus::Ready);
        QVERIFY(action.lastScanResult().applicable);
        QCOMPARE(action.lastScanResult().bytes_affected, qint64(1024));
    }

    void executeUpdatesStatus() {
        StubAction action;
        action.execute();
        QCOMPARE(action.status(), QuickAction::ActionStatus::Success);
        QVERIFY(action.lastExecutionResult().success);
    }

    void cancelSetsFlag() {
        StubAction action;
        QVERIFY(!action.isCancelled());

        action.cancel();
        // cancel() raises the flag unconditionally, but from Idle it must not
        // fabricate a Cancelled status for an action that never ran.
        QVERIFY(action.isCancelled());
        QCOMPARE(action.status(), QuickAction::ActionStatus::Idle);

        // clearCancellation() is what lets a later run start un-cancelled; a
        // stale flag would make every future run cancel immediately.
        action.clearCancellation();
        QVERIFY(!action.isCancelled());
    }

    void applyExecutionResult() {
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

    void updateStatusEmitsSignal() {
        StubAction action;
        QSignalSpy spy(&action, &QuickAction::statusChanged);
        action.updateStatus(QuickAction::ActionStatus::Running);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(QuickActionTests)
#include "test_quick_action.moc"
