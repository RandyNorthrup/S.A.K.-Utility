// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_screenshot_settings_action.cpp
/// @brief Unit tests for ScreenshotSettingsAction report seams (Codex B6-17):
///        a report-write failure must be surfaced (no advertised path for an
///        unwritten file), and the report text builder is exercised directly.

#include "sak/actions/screenshot_settings_action.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

using sak::ScreenshotSettingsAction;

/// Friend of ScreenshotSettingsAction: reaches the private report seams.
class ScreenshotSettingsActionTests : public QObject {
    Q_OBJECT

    using Action = ScreenshotSettingsAction;
    using CaptureResult = ScreenshotSettingsAction::CaptureResult;
    using CaptureContext = ScreenshotSettingsAction::CaptureContext;

private Q_SLOTS:
    void reportPathLine_onlyAdvertisesWrittenReport();
    void buildScreenshotReportText_containsPagesAndSections();
    void generateReport_writesAndDetectsFailure();
    // WaveD-09: success requires a full capture AND a written report; a partial
    // capture or an unwritten report must fail closed, not report success.
    void buildExecutionResult_failsClosedOnPartialOrUnwrittenReport();
};

void ScreenshotSettingsActionTests::reportPathLine_onlyAdvertisesWrittenReport() {
    QCOMPARE(Action::reportPathLine(true, QStringLiteral("C:/reports/r.txt")),
             QStringLiteral("REPORT_PATH:C:/reports/r.txt\n"));
    // A failed write must not advertise a path that does not exist.
    const QString failed = Action::reportPathLine(false, QStringLiteral("C:/reports/r.txt"));
    QVERIFY(!failed.contains(QStringLiteral("REPORT_PATH:")));
    QCOMPARE(failed, QStringLiteral("REPORT_WRITE_FAILED:1\n"));
}

void ScreenshotSettingsActionTests::buildScreenshotReportText_containsPagesAndSections() {
    CaptureResult capture;
    capture.captured_pages << QStringLiteral("Display_Settings");
    capture.failed_pages << QStringLiteral("WiFi_Settings");
    capture.screenshots_taken = 1;
    capture.failed_attempts = 1;

    const QString text = Action::buildScreenshotReportText(
        2, capture, QStringLiteral("C:/out"), QStringLiteral("20260731_010203"));

    QVERIFY(text.contains(QStringLiteral("WINDOWS SETTINGS SCREENSHOT REPORT")));
    QVERIFY(text.contains(QStringLiteral("Display_Settings")));
    QVERIFY(text.contains(QStringLiteral("FAILED PAGES")));
    QVERIFY(text.contains(QStringLiteral("WiFi_Settings")));
}

void ScreenshotSettingsActionTests::generateReport_writesAndDetectsFailure() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ScreenshotSettingsAction action(dir.path());
    CaptureResult capture;
    capture.captured_pages << QStringLiteral("Display_Settings");
    capture.screenshots_taken = 1;

    // Real directory -> report is written and committed.
    QVERIFY(action.generateReport(dir.path(), QStringLiteral("ts1"), 1, capture));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Screenshot_Report_ts1.txt"))));

    // Non-existent parent directory -> open fails -> reported as a write failure.
    const QString missing = dir.filePath(QStringLiteral("no_such_subdir"));
    QVERIFY(!action.generateReport(missing, QStringLiteral("ts2"), 1, capture));
}

void ScreenshotSettingsActionTests::buildExecutionResult_failsClosedOnPartialOrUnwrittenReport() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir out(dir.path());

    auto contextWith = [&](bool report_written) {
        CaptureContext ctx;
        ctx.total_pages = 3;
        ctx.monitor_count = 1;
        ctx.timestamp = QStringLiteral("ts");
        ctx.start_time = QDateTime::currentDateTime();
        ctx.report_written = report_written;
        return ctx;
    };

    // Full capture + written report -> success.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
        cap.screenshots_taken = 3;
        action.buildExecutionResult(cap, out, contextWith(true));
        QVERIFY2(action.lastExecutionResult().success, "full capture + report is a success");
    }

    // Some pages failed, even though the report was written -> fail closed.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b");
        cap.screenshots_taken = 2;
        cap.failed_attempts = 1;
        action.buildExecutionResult(cap, out, contextWith(true));
        QVERIFY2(!action.lastExecutionResult().success, "a partial capture must not be a success");
    }

    // Every page captured but the report could not be written -> fail closed.
    {
        ScreenshotSettingsAction action(dir.path());
        CaptureResult cap;
        cap.captured_pages << QStringLiteral("a") << QStringLiteral("b") << QStringLiteral("c");
        cap.screenshots_taken = 3;
        action.buildExecutionResult(cap, out, contextWith(false));
        QVERIFY2(!action.lastExecutionResult().success,
                 "an unwritten report must not be a success");
    }
}

QTEST_GUILESS_MAIN(ScreenshotSettingsActionTests)
#include "test_screenshot_settings_action.moc"
