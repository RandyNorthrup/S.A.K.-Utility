// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QTest>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include "sak/quick_action_result_io.h"

using namespace sak;

/**
 * @brief Unit tests for quick_action_result_io free functions.
 *
 * Covers:
 *  - actionStatusToString / actionStatusFromString round-trips
 *  - Case-insensitive and whitespace-tolerant parsing
 *  - writeExecutionResultFile / readExecutionResultFile round-trips
 *  - Error handling on bad JSON and missing files
 */
class TestQuickActionResultIO : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── actionStatusToString ────────────────────────────────
    void toStringIdle();
    void toStringScanning();
    void toStringReady();
    void toStringRunning();
    void toStringSuccess();
    void toStringFailed();
    void toStringCancelled();

    // ── actionStatusFromString ──────────────────────────────
    void fromStringExact();
    void fromStringMixedCase();
    void fromStringWithWhitespace();
    void fromStringUnknownReturnsIdle();
    void fromStringEmptyReturnsIdle();

    // ── round-trip enum ↔ string ────────────────────────────
    void roundTripAllStatuses();

    // ── file write / read ───────────────────────────────────
    void writeReadRoundTrip();
    void writeReadLargeValues();
    void readMissingFileReturnsFalse();
    void readInvalidJsonReturnsFalse();
    void readEmptyFileReturnsFalse();
    void writeToInvalidPathReturnsFalse();
};

// ============================================================================
// actionStatusToString
// ============================================================================

void TestQuickActionResultIO::toStringIdle() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Idle), "Idle");
}

void TestQuickActionResultIO::toStringScanning() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Scanning), "Scanning");
}

void TestQuickActionResultIO::toStringReady() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Ready), "Ready");
}

void TestQuickActionResultIO::toStringRunning() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Running), "Running");
}

void TestQuickActionResultIO::toStringSuccess() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Success), "Success");
}

void TestQuickActionResultIO::toStringFailed() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Failed), "Failed");
}

void TestQuickActionResultIO::toStringCancelled() {
    QCOMPARE(actionStatusToString(
        QuickAction::ActionStatus::Cancelled), "Cancelled");
}

// ============================================================================
// actionStatusFromString
// ============================================================================

void TestQuickActionResultIO::fromStringExact() {
    QCOMPARE(actionStatusFromString("Scanning"),
             QuickAction::ActionStatus::Scanning);
    QCOMPARE(actionStatusFromString("Ready"),
             QuickAction::ActionStatus::Ready);
    QCOMPARE(actionStatusFromString("Running"),
             QuickAction::ActionStatus::Running);
    QCOMPARE(actionStatusFromString("Success"),
             QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("Failed"),
             QuickAction::ActionStatus::Failed);
    QCOMPARE(actionStatusFromString("Cancelled"),
             QuickAction::ActionStatus::Cancelled);
    QCOMPARE(actionStatusFromString("Idle"),
             QuickAction::ActionStatus::Idle);
}

void TestQuickActionResultIO::fromStringMixedCase() {
    QCOMPARE(actionStatusFromString("SUCCESS"),
             QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("failed"),
             QuickAction::ActionStatus::Failed);
    QCOMPARE(actionStatusFromString("CANCELLED"),
             QuickAction::ActionStatus::Cancelled);
    QCOMPARE(actionStatusFromString("scanning"),
             QuickAction::ActionStatus::Scanning);
}

void TestQuickActionResultIO::fromStringWithWhitespace() {
    QCOMPARE(actionStatusFromString("  Success  "),
             QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("\tFailed\n"),
             QuickAction::ActionStatus::Failed);
}

void TestQuickActionResultIO::fromStringUnknownReturnsIdle() {
    QCOMPARE(actionStatusFromString("Bogus"),
             QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("xyz123"),
             QuickAction::ActionStatus::Idle);
}

void TestQuickActionResultIO::fromStringEmptyReturnsIdle() {
    QCOMPARE(actionStatusFromString(""),
             QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("   "),
             QuickAction::ActionStatus::Idle);
}

// ============================================================================
// Round-trip enum → string → enum
// ============================================================================

void TestQuickActionResultIO::roundTripAllStatuses() {
    const std::vector<QuickAction::ActionStatus> all = {
        QuickAction::ActionStatus::Idle,
        QuickAction::ActionStatus::Scanning,
        QuickAction::ActionStatus::Ready,
        QuickAction::ActionStatus::Running,
        QuickAction::ActionStatus::Success,
        QuickAction::ActionStatus::Failed,
        QuickAction::ActionStatus::Cancelled,
    };
    for (auto s : all) {
        const QString str = actionStatusToString(s);
        QVERIFY2(!str.isEmpty(),
                 "actionStatusToString returned empty");
        QCOMPARE(actionStatusFromString(str), s);
    }
}

// ============================================================================
// File write / read round-trip
// ============================================================================

void TestQuickActionResultIO::writeReadRoundTrip() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path =
        tmpDir.path() + "/result.json";

    QuickAction::ExecutionResult original;
    original.success = true;
    original.message = "Completed successfully";
    original.bytes_processed = 1024;
    original.files_processed = 42;
    original.duration_ms = 5000;
    original.output_path = "C:/Backups/test";
    original.log = "Step 1 done\nStep 2 done";

    const auto originalStatus =
        QuickAction::ActionStatus::Success;

    QString writeError;
    QVERIFY(writeExecutionResultFile(
        path, original, originalStatus, &writeError));
    QVERIFY(writeError.isEmpty());

    QuickAction::ExecutionResult loaded;
    QuickAction::ActionStatus loadedStatus;
    QString readError;
    QVERIFY(readExecutionResultFile(
        path, &loaded, &loadedStatus, &readError));
    QVERIFY(readError.isEmpty());

    QCOMPARE(loaded.success, original.success);
    QCOMPARE(loaded.message, original.message);
    QCOMPARE(loaded.bytes_processed,
             original.bytes_processed);
    QCOMPARE(loaded.files_processed,
             original.files_processed);
    QCOMPARE(loaded.duration_ms,
             original.duration_ms);
    QCOMPARE(loaded.output_path,
             original.output_path);
    QCOMPARE(loaded.log, original.log);
    QCOMPARE(loadedStatus, originalStatus);
}

void TestQuickActionResultIO::writeReadLargeValues() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path =
        tmpDir.path() + "/large.json";

    QuickAction::ExecutionResult original;
    original.success = false;
    original.message = "Partial failure";
    // Use values that fit in double without precision loss
    original.bytes_processed = 1099511627776LL;  // 1 TB
    original.files_processed = 999999;
    original.duration_ms = 86400000LL;  // 24 hours
    original.output_path.clear();
    original.log.clear();

    const auto status =
        QuickAction::ActionStatus::Failed;

    QVERIFY(writeExecutionResultFile(
        path, original, status));

    QuickAction::ExecutionResult loaded;
    QuickAction::ActionStatus loadedStatus;
    QVERIFY(readExecutionResultFile(
        path, &loaded, &loadedStatus));

    QCOMPARE(loaded.bytes_processed,
             original.bytes_processed);
    QCOMPARE(loaded.files_processed,
             original.files_processed);
    QCOMPARE(loaded.duration_ms,
             original.duration_ms);
    QCOMPARE(loadedStatus, status);
}

// ============================================================================
// Error paths
// ============================================================================

void TestQuickActionResultIO::readMissingFileReturnsFalse() {
    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile(
        "C:/nonexistent/path.json",
        &result, &status, &error));
    QVERIFY(!error.isEmpty());
}

void TestQuickActionResultIO::readInvalidJsonReturnsFalse() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path =
        tmpDir.path() + "/bad.json";

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not valid json {{{");
    f.close();

    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile(
        path, &result, &status, &error));
    QVERIFY(!error.isEmpty());
}

void TestQuickActionResultIO::readEmptyFileReturnsFalse() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path =
        tmpDir.path() + "/empty.json";

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();

    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile(
        path, &result, &status, &error));
}

void TestQuickActionResultIO::writeToInvalidPathReturnsFalse()
{
    QuickAction::ExecutionResult result;
    result.success = true;
    result.message = "Test";
    QString error;
    QVERIFY(!writeExecutionResultFile(
        "Z:/nonexistent/dir/file.json",
        result,
        QuickAction::ActionStatus::Success,
        &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestQuickActionResultIO)
#include "test_quick_action_result_io.moc"
