// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_generate_system_report_action.cpp
/// @brief Unit tests for GenerateSystemReportAction pure seams (Codex B6-15):
///        a collector that fails to run must be surfaced, and a saved-but-empty
///        report must not be reported as a successful comprehensive report.

#include "sak/actions/generate_system_report_action.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using sak::GenerateSystemReportAction;

class GenerateSystemReportActionTests : public QObject {
    Q_OBJECT

    using Action = GenerateSystemReportAction;

private Q_SLOTS:
    void collectorFailed_truthTable();
    void reportGenerationSucceeded_requiresSaveAndCollectors();
    void saveReportCountsUtf8BytesNotUtf16Units();
};

void GenerateSystemReportActionTests::collectorFailed_truthTable() {
    QVERIFY(!Action::collectorFailed(false, 0));  // clean run
    QVERIFY(Action::collectorFailed(true, 0));    // timed out
    QVERIFY(Action::collectorFailed(false, 1));   // non-zero exit
    // NEGATIVE exit codes: -1 is the never-run default of a ProcessResult, and a crash arrives
    // as an NTSTATUS reinterpreted as a negative int. A guard written `exit_code > 0` calls both
    // a clean collector and reports a system report built from nothing.
    QVERIFY(Action::collectorFailed(false, -1));
    QVERIFY(Action::collectorFailed(false, -1'073'741'819));
    // THIRD ARM, which the two-argument calls above never reach: a collector that exits 0, on
    // time, but hands back blank or ceiling-truncated stdout is still a failure.
    QVERIFY(Action::collectorFailed(false, 0, true));
    QVERIFY(!Action::collectorFailed(false, 0, false));  // explicit false == the 2-arg default
    QVERIFY(Action::collectorFailed(true, 1, true));     // all three arms together
}

void GenerateSystemReportActionTests::reportGenerationSucceeded_requiresSaveAndCollectors() {
    QVERIFY(Action::reportGenerationSucceeded(true, true));  // wrote + all collectors ran
    // Saved, but a collector failed -> must NOT be a success (the B6-15 bug).
    QVERIFY(!Action::reportGenerationSucceeded(true, false));
    // Write failed -> failure regardless.
    QVERIFY(!Action::reportGenerationSucceeded(false, true));
    QVERIFY(!Action::reportGenerationSucceeded(false, false));
}

void GenerateSystemReportActionTests::saveReportCountsUtf8BytesNotUtf16Units() {
    // The report is WRITTEN as UTF-8, but its size was reported with QString::size(), which
    // counts UTF-16 code units. On a localized Windows -- where device names, user names and
    // service descriptions are not ASCII -- the ExecutionResult's bytes_processed and the
    // "Size: N KB" line shown to the technician both disagreed with the file on disk.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("report.txt"));

    // Cyrillic: one QString character each, TWO UTF-8 bytes each, so the two units cannot agree.
    const QString cyrillic = QString::fromUtf8("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    const QString report = QStringLiteral("System Report\n") + cyrillic.repeated(100);

    Action action(temp.path());
    qint64 bytes_written = -1;
    QVERIFY(action.saveReport(report, path, &bytes_written));

    // The reported count is the SIZE OF THE FILE ON DISK -- the number the technician can check
    // in Explorer.
    QCOMPARE(bytes_written, QFileInfo(path).size());
    // It is genuinely larger than the UTF-16 unit count, so this fixture actually distinguishes
    // the two units rather than passing on ASCII where they coincide.
    QVERIFY2(bytes_written > report.size(),
             qPrintable(QStringLiteral("bytes=%1 units=%2").arg(bytes_written).arg(report.size())));
    // And it is at least the encoded payload: the device is opened with QIODevice::Text, so on
    // Windows each '\n' becomes "\r\n" and the file is LARGER than the bytes handed to write().
    // Reporting the payload size would have been a subtler version of the same unit mistake.
    QVERIFY2(
        bytes_written >= static_cast<qint64>(report.toUtf8().size()),
        qPrintable(
            QStringLiteral("file=%1 payload=%2").arg(bytes_written).arg(report.toUtf8().size())));
}

QTEST_GUILESS_MAIN(GenerateSystemReportActionTests)
#include "test_generate_system_report_action.moc"
