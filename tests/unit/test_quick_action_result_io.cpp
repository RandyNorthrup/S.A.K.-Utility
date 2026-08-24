// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action_result_io.h"

#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

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
    // -- actionStatusToString --------------------------------
    void toStringIdle();
    void toStringScanning();
    void toStringReady();
    void toStringRunning();
    void toStringSuccess();
    void toStringFailed();
    void toStringCancelled();

    // -- actionStatusFromString ------------------------------
    void fromStringExact();
    void fromStringMixedCase();
    void fromStringWithWhitespace();
    void fromStringUnknownReturnsIdle();
    void fromStringEmptyReturnsIdle();

    // -- round-trip enum <-> string ----------------------------
    void roundTripAllStatuses();

    // -- file write / read -----------------------------------
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
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Idle), "Idle");
}

void TestQuickActionResultIO::toStringScanning() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Scanning), "Scanning");
}

void TestQuickActionResultIO::toStringReady() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Ready), "Ready");
}

void TestQuickActionResultIO::toStringRunning() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Running), "Running");
}

void TestQuickActionResultIO::toStringSuccess() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Success), "Success");
}

void TestQuickActionResultIO::toStringFailed() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Failed), "Failed");
}

void TestQuickActionResultIO::toStringCancelled() {
    QCOMPARE(actionStatusToString(QuickAction::ActionStatus::Cancelled), "Cancelled");
}

// ============================================================================
// actionStatusFromString
// ============================================================================

void TestQuickActionResultIO::fromStringExact() {
    QCOMPARE(actionStatusFromString("Scanning"), QuickAction::ActionStatus::Scanning);
    QCOMPARE(actionStatusFromString("Ready"), QuickAction::ActionStatus::Ready);
    QCOMPARE(actionStatusFromString("Running"), QuickAction::ActionStatus::Running);
    QCOMPARE(actionStatusFromString("Success"), QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("Failed"), QuickAction::ActionStatus::Failed);
    QCOMPARE(actionStatusFromString("Cancelled"), QuickAction::ActionStatus::Cancelled);
    QCOMPARE(actionStatusFromString("Idle"), QuickAction::ActionStatus::Idle);
}

void TestQuickActionResultIO::fromStringMixedCase() {
    QCOMPARE(actionStatusFromString("SUCCESS"), QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("failed"), QuickAction::ActionStatus::Failed);
    QCOMPARE(actionStatusFromString("CANCELLED"), QuickAction::ActionStatus::Cancelled);
    QCOMPARE(actionStatusFromString("scanning"), QuickAction::ActionStatus::Scanning);
}

void TestQuickActionResultIO::fromStringWithWhitespace() {
    QCOMPARE(actionStatusFromString("  Success  "), QuickAction::ActionStatus::Success);
    QCOMPARE(actionStatusFromString("\tFailed\n"), QuickAction::ActionStatus::Failed);
}

void TestQuickActionResultIO::fromStringUnknownReturnsIdle() {
    QCOMPARE(actionStatusFromString("Bogus"), QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("xyz123"), QuickAction::ActionStatus::Idle);
    // Near-miss tokens. The parser compares the trimmed+lowered token for EQUALITY
    // (quick_action_result_io.cpp:46-63), so a superstring, a substring-carrier, or a prefix
    // of a real token must NOT be accepted. "Bogus"/"xyz123" share no substring with any
    // token, so without these a contains()/startsWith() parser stays green everywhere.
    QCOMPARE(actionStatusFromString("Successful"), QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("not success"), QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("succes"), QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("cancelled-by-user"), QuickAction::ActionStatus::Idle);
}

void TestQuickActionResultIO::fromStringEmptyReturnsIdle() {
    QCOMPARE(actionStatusFromString(""), QuickAction::ActionStatus::Idle);
    QCOMPARE(actionStatusFromString("   "), QuickAction::ActionStatus::Idle);
}

// ============================================================================
// Round-trip enum -> string -> enum
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
        QVERIFY2(!str.isEmpty(), "actionStatusToString returned empty");
        QCOMPARE(actionStatusFromString(str), s);
    }
}

// ============================================================================
// File write / read round-trip
// ============================================================================

void TestQuickActionResultIO::writeReadRoundTrip() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path = tmpDir.path() + "/result.json";

    QuickAction::ExecutionResult original;
    original.success = true;
    original.message = "Completed successfully";
    original.bytes_processed = 1024;
    original.files_processed = 42;
    original.duration_ms = 5000;
    original.output_path = "C:/Backups/test";
    original.log = "Step 1 done\nStep 2 done";

    const auto originalStatus = QuickAction::ActionStatus::Success;

    QString writeError;
    QVERIFY(writeExecutionResultFile(path, original, originalStatus, &writeError));
    QVERIFY(writeError.isEmpty());

    QuickAction::ExecutionResult loaded;
    QuickAction::ActionStatus loadedStatus;
    QString readError;
    QVERIFY(readExecutionResultFile(path, &loaded, &loadedStatus, &readError));
    QVERIFY(readError.isEmpty());

    QCOMPARE(loaded.success, original.success);
    QCOMPARE(loaded.message, original.message);
    QCOMPARE(loaded.bytes_processed, original.bytes_processed);
    QCOMPARE(loaded.files_processed, original.files_processed);
    QCOMPARE(loaded.duration_ms, original.duration_ms);
    QCOMPARE(loaded.output_path, original.output_path);
    QCOMPARE(loaded.log, original.log);
    QCOMPARE(loadedStatus, originalStatus);
}

void TestQuickActionResultIO::writeReadLargeValues() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path = tmpDir.path() + "/large.json";

    QuickAction::ExecutionResult original;
    original.success = false;
    original.message = "Partial failure";
    // Use values that fit in double without precision loss
    original.bytes_processed = 1'099'511'627'776LL;  // 1 TB
    original.files_processed = 999'999;
    original.duration_ms = 86'400'000LL;             // 24 hours
    original.output_path.clear();
    original.log.clear();

    const auto status = QuickAction::ActionStatus::Failed;

    QVERIFY(writeExecutionResultFile(path, original, status));

    QuickAction::ExecutionResult loaded;
    QuickAction::ActionStatus loadedStatus;
    QVERIFY(readExecutionResultFile(path, &loaded, &loadedStatus));

    // Pin the fixture magnitudes as literals: these are the exact values the double-encoded
    // counters must survive (quick_action_result_io.cpp:109-111 writes each counter through
    // static_cast<double>; :23-40 + :185-191 read it back).
    QCOMPARE(loaded.bytes_processed, 1'099'511'627'776LL);
    QCOMPARE(loaded.files_processed, 999'999LL);
    QCOMPARE(loaded.duration_ms, 86'400'000LL);
    // Siblings this fixture sets but never checked. success=false is the ONLY exercise of the
    // false arm of the persisted bool anywhere in the suite (writeReadRoundTrip only ever proves
    // true), so without it a reader that hard-codes success stays green
    // (quick_action_result_io.cpp:192). The two deliberately-cleared strings must survive as
    // empty (:197-198).
    QCOMPARE(loaded.success, false);
    QCOMPARE(loaded.message, QStringLiteral("Partial failure"));
    QCOMPARE(loaded.output_path, QString());
    QCOMPARE(loaded.log, QString());
    QCOMPARE(loadedStatus, QuickAction::ActionStatus::Failed);
}

// ============================================================================
// Error paths
// ============================================================================

void TestQuickActionResultIO::readMissingFileReturnsFalse() {
    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile("C:/nonexistent/path.json", &result, &status, &error));
    // The fixed prefix identifies the open-fail branch (other branches use distinct prefixes);
    // only the .arg(errorString()) suffix is OS/locale-variant.
    QVERIFY(error.startsWith(QStringLiteral("Failed to read result file: ")));
}

void TestQuickActionResultIO::readInvalidJsonReturnsFalse() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path = tmpDir.path() + "/bad.json";

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not valid json {{{");
    f.close();

    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile(path, &result, &status, &error));
    QVERIFY(error.startsWith(QStringLiteral("Invalid result file JSON: ")));

    // A file whose JSON PARSES but whose payload is corrupt must be refused too, and for its
    // own stated reason. These are distinct fail-closed guards from the parse branch above and
    // no test in the suite reached either of them, so a reader that dropped both stayed green.
    // (a) a counter present but negative, (b) present but non-numeric: :23-40 + :185-191.
    const QString negPath = tmpDir.path() + "/negative.json";
    QFile negFile(negPath);
    QVERIFY(negFile.open(QIODevice::WriteOnly));
    negFile.write("{\"bytes_processed\":-1,\"status\":\"Success\"}");
    negFile.close();
    QString negError;
    QVERIFY(!readExecutionResultFile(negPath, &result, &status, &negError));
    QCOMPARE(negError,
             QStringLiteral("Result file has an invalid numeric field (non-numeric, negative, "
                            "or out of range)."));

    const QString textPath = tmpDir.path() + "/text_counter.json";
    QFile textFile(textPath);
    QVERIFY(textFile.open(QIODevice::WriteOnly));
    textFile.write("{\"files_processed\":\"lots\",\"status\":\"Success\"}");
    textFile.close();
    QString textError;
    QVERIFY(!readExecutionResultFile(textPath, &result, &status, &textError));
    QCOMPARE(textError,
             QStringLiteral("Result file has an invalid numeric field (non-numeric, negative, "
                            "or out of range)."));

    // (c) a parseable object with an UNKNOWN status token: the reader is strict here and must
    //     NOT reuse the lenient unknown -> Idle mapping (:230-235 vs the public :90-100).
    const QString badStatusPath = tmpDir.path() + "/bad_status.json";
    QFile badStatusFile(badStatusPath);
    QVERIFY(badStatusFile.open(QIODevice::WriteOnly));
    badStatusFile.write("{\"success\":true,\"message\":\"m\",\"status\":\"Bogus\"}");
    badStatusFile.close();
    QString badStatusError;
    QVERIFY(!readExecutionResultFile(badStatusPath, &result, &status, &badStatusError));
    QCOMPARE(badStatusError, QStringLiteral("Result file has a missing or unknown status field."));

    // (d) and an object with no status field at all takes that same fail-closed exit.
    const QString noStatusPath = tmpDir.path() + "/no_status.json";
    QFile noStatusFile(noStatusPath);
    QVERIFY(noStatusFile.open(QIODevice::WriteOnly));
    noStatusFile.write("{\"success\":true,\"message\":\"m\"}");
    noStatusFile.close();
    QString noStatusError;
    QVERIFY(!readExecutionResultFile(noStatusPath, &result, &status, &noStatusError));
    QCOMPARE(noStatusError, QStringLiteral("Result file has a missing or unknown status field."));
}

void TestQuickActionResultIO::readEmptyFileReturnsFalse() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path = tmpDir.path() + "/empty.json";

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();

    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status;
    QString error;
    QVERIFY(!readExecutionResultFile(path, &result, &status, &error));
    // An empty file parses to a null JSON document -> the invalid-JSON branch.
    QVERIFY(error.startsWith(QStringLiteral("Invalid result file JSON: ")));

    // The other degenerate size, plus the exact boundary between the two branches. The cap is
    // `file.size() > kMaxResultFileBytes` with kMaxResultFileBytes = 4 * 1024 * 1024
    // (quick_action_result_io.cpp:161-164), so 4 MiB exactly must still be READ (and then fail
    // on its contents) while 4 MiB + 1 must be refused by the size guard with its own message.
    // Pinned as a pair: one case alone cannot see a `>` -> `>=` slip.
    const QString atLimitPath = tmpDir.path() + "/at_limit.json";
    QFile atLimit(atLimitPath);
    QVERIFY(atLimit.open(QIODevice::WriteOnly));
    QVERIFY(atLimit.resize(4 * 1024 * 1024));
    atLimit.close();
    QString atLimitError;
    QVERIFY(!readExecutionResultFile(atLimitPath, &result, &status, &atLimitError));
    QVERIFY(atLimitError.startsWith(QStringLiteral("Invalid result file JSON: ")));

    const QString overLimitPath = tmpDir.path() + "/over_limit.json";
    QFile overLimit(overLimitPath);
    QVERIFY(overLimit.open(QIODevice::WriteOnly));
    QVERIFY(overLimit.resize(4 * 1024 * 1024 + 1));
    overLimit.close();
    QString overLimitError;
    QVERIFY(!readExecutionResultFile(overLimitPath, &result, &status, &overLimitError));
    QCOMPARE(overLimitError, QStringLiteral("Result file is too large to be a valid result."));
}

void TestQuickActionResultIO::writeToInvalidPathReturnsFalse() {
    QuickAction::ExecutionResult result;
    result.success = true;
    result.message = "Test";
    QString error;
    QVERIFY(!writeExecutionResultFile(
        "Z:/nonexistent/dir/file.json", result, QuickAction::ActionStatus::Success, &error));
    QVERIFY(error.startsWith(QStringLiteral("Failed to write result file: ")));
}

QTEST_MAIN(TestQuickActionResultIO)
#include "test_quick_action_result_io.moc"
